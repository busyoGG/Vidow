/* portalconf.c — portal 后端选择（portals.conf）的用户级配置与同步。
 *
 * 选哪个后端（GTK / KDE）不是合成器决定的，是 xdg-desktop-portal 读
 * $XDG_CONFIG_HOME/xdg-desktop-portal/<desktop>-portals.conf 决定的，会话名取
 * XDG_CURRENT_DESKTOP 里冒号分隔的每一段。vidow 当会话合成器时那就是
 * vidow-portals.conf。
 *
 * 直接手改那个文件也能用，但它属于 portal，而且「默认该选谁」这件事应该跟着
 * vidow 走。所以分三层：
 *
 *   1. 内置默认（default_lines）：GTK 打底，GTK 没有的接口交给 KDE
 *   2. ~/.config/vidow/portals.conf：用户覆盖，逐键合并 —— 想换文件选择器就写一行
 *      `org.freedesktop.impl.portal.FileChooser=kde`；值留空 = 删掉这一项
 *   3. $XDG_CONFIG_HOME/xdg-desktop-portal/vidow-portals.conf：同步产物，vidow 写
 *
 * 同步只在内容真的变了时才动文件；变了就顺手重启 xdg-desktop-portal。重启前必须
 * 先 import-environment —— 后端是被 dbus 激活起来的，环境从 systemd 用户管理器
 * 拿，不导入的话后端连的还是宿主合成器，对话框会画到别的桌面上。
 */

#include "server.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <wlr/util/log.h>

#define MAX_LINES 64
#define KEY_LEN 128
#define VAL_LEN 256

struct conf_line {
	char key[KEY_LEN];
	char value[VAL_LEN];
};

/* GTK 后端覆盖 FileChooser / AppChooser / Print / Notification / Inhibit /
 * Access / Account / Email / DynamicLauncher / Lockdown / Settings。
 * KDE 补齐 GTK 没有的接口 —— 注意 ScreenCast / Screenshot / RemoteDesktop /
 * InputCapture 的 KDE 实现走 KWin 私有 IPC，在 vidow 上会失败，所以故意不列；
 * 录屏 / 截图交给 capture_backend() 探测出来的那个后端（见下面）。 */
static const char *default_lines[] = {
	"default=gtk",
	"org.freedesktop.impl.portal.FileChooser=kde",
	"org.freedesktop.impl.portal.GlobalShortcuts=kde",
	"org.freedesktop.impl.portal.Wallpaper=kde",
	"org.freedesktop.impl.portal.Usb=kde",
	"org.freedesktop.impl.portal.Clipboard=kde",
	"org.freedesktop.impl.portal.Background=kde",
};

/* ScreenCast / Screenshot 该给谁。GTK 后端根本没有这两个接口；KDE 的实现建在
 * KWin 的私有 wayland 协议上（zkde_screencast_unstable_v1，二进制里就写着
 * "zkde_screencast_unstable_v1 does not seem to be available"），vidow 不实现它，
 * 指过去只会让客户端以为支持、一调用就失败。真能在这台合成器上干活的是：
 *   - wlr       专为 wlroots 写，走 zwlr_screencopy_manager_v1
 *   - hyprland  它的 ScreenCast 在非 Hyprland 的 wlroots 合成器上同样退到
 *               zwlr_screencopy_manager_v1（UseIn 里就写着 wlroots/sway/Wayfire/
 *               river），二进制里有 "compositor doesn't support zwlr_screencopy_v1"
 *               这条退路
 * vidow 自己实现了 zwlr_screencopy_manager_v1（screencopy.c），两个都能用。
 * 装了哪个用哪个，wlr 优先（更轻，就是为这个场景写的）；一个都没装就一个字都不
 * 写 —— 写了等于骗客户端。用户仍可在 ~/.config/vidow/portals.conf 里覆盖。 */
static const char *capture_backend(void) {
	static const char *const files[] = { "wlr.portal", "hyprland.portal" };
	static const char *const names[] = { "wlr", "hyprland" };
	static const char *const fallback_dirs = "/usr/local/share:/usr/share";

	const char *dirs = getenv("XDG_DATA_DIRS");
	if (dirs == NULL || *dirs == '\0') {
		dirs = fallback_dirs;
	}

	for (size_t f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
		for (const char *p = dirs; *p != '\0';) {
			const char *end = strchr(p, ':');
			size_t len = (end != NULL) ? (size_t)(end - p) : strlen(p);
			if (len > 0) {
				char path[PATH_MAX];
				snprintf(path, sizeof(path),
					"%.*s/xdg-desktop-portal/portals/%s",
					(int)len, p, files[f]);
				if (access(path, R_OK) == 0) {
					return names[f];
				}
			}
			if (end == NULL) {
				break;
			}
			p = end + 1;
		}
	}
	return NULL;
}

/* --------------------------------------------------------------- 小工具 */

/* 有界拷贝：gcc 的 -Wformat-truncation 会盯着 snprintf("%s") 的源长度，
 * 这里源都是运行时读进来的行，直接 memcpy 把意图写清楚。 */
static void copy_str(char *dst, size_t cap, const char *src) {
	size_t n = strlen(src);
	if (n >= cap) {
		n = cap - 1;
	}
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static void strip(char *s) {
	char *p = s;
	while (isspace((unsigned char)*p)) {
		p++;
	}
	if (p != s) {
		memmove(s, p, strlen(p) + 1);
	}
	char *end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1])) {
		*--end = '\0';
	}
}

/* 目标路径：$XDG_CONFIG_HOME/xdg-desktop-portal/<name>-portals.conf
 * （XDG_CONFIG_HOME 没设时退回 $HOME/.config）。name 小写。 */
static bool target_path(const char *desktop, char *out, size_t len) {
	char name[32];
	size_t i = 0;
	for (; desktop[i] != '\0' && i < sizeof(name) - 1; i++) {
		name[i] = (char)tolower((unsigned char)desktop[i]);
	}
	name[i] = '\0';
	if (i == 0) {
		return false;
	}

	const char *base = getenv("XDG_CONFIG_HOME");
	char fallback[PATH_MAX];
	if (base == NULL || *base == '\0') {
		const char *home = getenv("HOME");
		snprintf(fallback, sizeof(fallback), "%s/.config", home != NULL ? home : ".");
		base = fallback;
	}
	snprintf(out, len, "%s/xdg-desktop-portal/%s-portals.conf", base, name);
	return true;
}

/* 用户覆盖：~/.config/vidow/portals.conf（跟 config / config.lua / shaders 一个目录） */
static void source_path(char *out, size_t len) {
	const char *home = getenv("HOME");
	snprintf(out, len, "%s/.config/vidow/portals.conf", home != NULL ? home : ".");
}

static char *slurp(const char *path, size_t *size_out) {
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return NULL;
	}
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	size_t n;
	while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
		len += n;
		if (len + 1 >= cap) {
			cap *= 2;
			char *bigger = realloc(buf, cap);
			if (bigger == NULL) {
				free(buf);
				fclose(f);
				return NULL;
			}
			buf = bigger;
		}
	}
	fclose(f);
	buf[len] = '\0';
	if (size_out != NULL) {
		*size_out = len;
	}
	return buf;
}

/* ------------------------------------------------------------ 合并与生成 */

/* 读用户覆盖。只认 [preferred] 段里的 key=value，段名行和注释行跳过。 */
static int read_override(const char *path, struct conf_line *out, int cap) {
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return -1;
	}

	int n = 0;
	char line[512];
	while (n < cap && fgets(line, sizeof(line), f) != NULL) {
		char *hash = strchr(line, '#');
		if (hash != NULL) {
			*hash = '\0';
		}
		strip(line);
		if (*line == '\0' || *line == '[') {
			continue;
		}
		char *eq = strchr(line, '=');
		if (eq == NULL) {
			continue;
		}
		*eq = '\0';
		char *key = line;
		char *value = eq + 1;
		strip(key);
		strip(value);
		if (*key == '\0') {
			continue;
		}
		copy_str(out[n].key, sizeof(out[n].key), key);
		copy_str(out[n].value, sizeof(out[n].value), value);
		n++;
	}
	fclose(f);
	return n;
}

/* 逐键合并：用户写了就盖掉，值留空 = 删掉这一项，没写过的键原样保留。
 * 这样「只想换文件选择器」就真的只需要写一行。 */
static int merge(struct conf_line *eff, int n,
		const struct conf_line *ov, int ov_n) {
	for (int i = 0; i < ov_n; i++) {
		int found = -1;
		for (int j = 0; j < n; j++) {
			if (strcmp(eff[j].key, ov[i].key) == 0) {
				found = j;
				break;
			}
		}
		if (ov[i].value[0] == '\0') {
			if (found >= 0) {
				memmove(&eff[found], &eff[found + 1],
					(size_t)(n - found - 1) * sizeof(eff[0]));
				n--;
			}
			continue;
		}
		if (found >= 0) {
			snprintf(eff[found].value, VAL_LEN, "%s", ov[i].value);
		} else if (n < MAX_LINES) {
			snprintf(eff[n].key, KEY_LEN, "%s", ov[i].key);
			snprintf(eff[n].value, VAL_LEN, "%s", ov[i].value);
			n++;
		}
	}
	return n;
}

/* --------------------------------------------------------------- 落盘 */

static bool write_atomic(const char *path, const char *content) {
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);

	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		wlr_log(WLR_ERROR, "portal: 写不了 %s: %s", tmp, strerror(errno));
		return false;
	}
	size_t len = strlen(content);
	ssize_t written = write(fd, content, len);
	close(fd);
	if (written < 0 || (size_t)written != len) {
		wlr_log(WLR_ERROR, "portal: %s 只写了 %zd/%zu 字节", tmp, written, len);
		unlink(tmp);
		return false;
	}
	if (rename(tmp, path) != 0) {
		wlr_log(WLR_ERROR, "portal: rename 到 %s 失败: %s", path, strerror(errno));
		unlink(tmp);
		return false;
	}
	return true;
}

/* 目录可能还不存在（干净的家目录）。逐级 mkdir。 */
static void ensure_parent(const char *path) {
	char buf[PATH_MAX];
	snprintf(buf, sizeof(buf), "%s", path);
	char *slash = strrchr(buf, '/');
	if (slash == NULL) {
		return;
	}
	*slash = '\0';
	for (char *p = buf + 1; *p != '\0'; p++) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		mkdir(buf, 0755);
		*p = '/';
	}
	mkdir(buf, 0755);
}

/* ------------------------------------------------------- 环境快照 / 还原 */

/* 后端是 dbus 按需激活的，环境取自 systemd 用户管理器。vidow 启动时把
 * XDG_CURRENT_DESKTOP / WAYLAND_DISPLAY / XDG_RUNTIME_DIR 改成自己的（见下面
 * portal_env_import 里的 import-environment），**退出时必须放回去**：不放回去，
 * 回到宿主合成器（Hyprland 之类）以后新拉起来的后端连的是已经消失的 vidow
 * socket，portal 全线失效 —— 而且 Linger=yes 时登出也救不回来（用户管理器不跟
 * 着会话退出，Hyprland 自己的启动逻辑也不会重启 xdg-desktop-portal）。
 *
 * 要还原的是「vidow 进来之前用户管理器里是什么」，不是 vidow 继承到的环境：
 * vidow 一般从 tty 起，那里的 WAYLAND_DISPLAY 本来就是空的。所以快照问
 * systemctl；问不到（没有 systemd）才退回进程环境。 */
static const char *const restore_keys[] = {
	"XDG_CURRENT_DESKTOP",
	"WAYLAND_DISPLAY",
	"XDG_RUNTIME_DIR",
};
#define RESTORE_N ((int)(sizeof(restore_keys) / sizeof(restore_keys[0])))

static char restore_value[RESTORE_N][VAL_LEN];
static bool restore_present[RESTORE_N];
static bool restore_armed;

/* 从 `systemctl --user show-environment` 的输出里取 KEY=VALUE。 */
static bool env_line_value(const char *buf, const char *key, char *out, size_t cap) {
	size_t klen = strlen(key);
	for (const char *line = buf; *line != '\0';) {
		const char *nl = strchr(line, '\n');
		size_t llen = (nl != NULL) ? (size_t)(nl - line) : strlen(line);
		if (llen > klen && strncmp(line, key, klen) == 0 && line[klen] == '=') {
			size_t vlen = llen - klen - 1;
			if (vlen >= cap) {
				vlen = cap - 1;
			}
			memcpy(out, line + klen + 1, vlen);
			out[vlen] = '\0';
			return true;
		}
		if (nl == NULL) {
			break;
		}
		line = nl + 1;
	}
	return false;
}

/* 单引号包起来，值里的 ' 写成 '\'' —— 值来自用户管理器，不能直接拼进 sh。 */
static void shell_quote(const char *src, char *out, size_t cap) {
	size_t n = 0;
	if (n + 1 < cap) {
		out[n++] = '\'';
	}
	for (const char *p = src; *p != '\0' && n + 6 < cap; p++) {
		if (*p == '\'') {
			memcpy(out + n, "'\\''", 4);
			n += 4;
		} else {
			out[n++] = *p;
		}
	}
	if (n + 1 < cap) {
		out[n++] = '\'';
	}
	out[n] = '\0';
}

/* 往 buf 尾巴上追加（buf 必须已经以 '\0' 结尾）。 */
static void append(char *buf, size_t cap, const char *fmt, ...) {
	size_t len = strlen(buf);
	if (len + 1 >= cap) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf + len, cap - len, fmt, ap);
	va_end(ap);
}

/* 把 vidow 的环境推进 systemd 用户管理器。restart_frontend 只在 portals.conf
 * 真的变了时才需要（换了选谁，前端得重新读一遍配置）。 */
static void portal_env_import(bool restart_frontend) {
	if (getenv("VIDOW_PORTAL_NO_RESTART") != NULL) {
		wlr_log(WLR_INFO, "portal: VIDOW_PORTAL_NO_RESTART 已设，不碰用户管理器");
		return;
	}

	/* import-environment 必须在前：后端是 dbus 激活起来的，环境从 systemd
	 * 用户管理器拿。不导入的话 xdg-desktop-portal-gtk 连的还是宿主合成器，
	 * 对话框会画到别的桌面上。 */
	restore_armed = true;
	if (restart_frontend) {
		vidow_exec_detached(
			"systemctl --user import-environment "
			"XDG_CURRENT_DESKTOP WAYLAND_DISPLAY XDG_RUNTIME_DIR 2>/dev/null; "
			"systemctl --user restart xdg-desktop-portal 2>/dev/null");
	} else {
		vidow_exec_detached(
			"systemctl --user import-environment "
			"XDG_CURRENT_DESKTOP WAYLAND_DISPLAY XDG_RUNTIME_DIR 2>/dev/null");
	}
}

/* ------------------------------------------------------------ public API */

void vidow_portal_desktop_env(const struct vidow_config *cfg) {
	if (cfg->portal_desktop[0] == '\0') {
		return;
	}
	setenv("XDG_CURRENT_DESKTOP", cfg->portal_desktop, 1);
}

/* 记下「vidow 进来之前」用户管理器里的值。要在第一次 import 之前调，而且只需
 * 一次 —— 后面对账（配置热重载）时再调会把 vidow 自己的值记成「原来」。 */
void vidow_portal_env_snapshot(void) {
	if (getenv("VIDOW_PORTAL_NO_RESTART") != NULL) {
		wlr_log(WLR_INFO, "portal: VIDOW_PORTAL_NO_RESTART 已设，不记环境快照");
		return;
	}

	/* 先问用户管理器（那才是后端真正继承的东西），问不到才退回进程环境。 */
	char buf[8192];
	buf[0] = '\0';
	bool from_manager = false;
	FILE *f = popen("systemctl --user show-environment 2>/dev/null", "r");
	if (f != NULL) {
		size_t got = fread(buf, 1, sizeof(buf) - 1, f);
		buf[got] = '\0';
		from_manager = pclose(f) == 0;
	}

	char log[4 * VAL_LEN];
	log[0] = '\0';
	for (int i = 0; i < RESTORE_N; i++) {
		restore_value[i][0] = '\0';
		restore_present[i] = false;
		if (from_manager) {
			restore_present[i] = env_line_value(buf, restore_keys[i],
				restore_value[i], sizeof(restore_value[i]));
		} else {
			const char *value = getenv(restore_keys[i]);
			if (value != NULL) {
				copy_str(restore_value[i], sizeof(restore_value[i]), value);
				restore_present[i] = true;
			}
		}
		append(log, sizeof(log), "%s %s=%s", i == 0 ? "" : " ",
			restore_keys[i], restore_present[i] ? restore_value[i] : "(未设)");
	}
	wlr_log(WLR_INFO, "portal: 环境快照%s（来源：%s）", log,
		from_manager ? "systemd 用户管理器" : "进程环境");
}

/* 退出时把环境放回去。没 import 过（没变过用户管理器）就什么都不做。 */
void vidow_portal_env_restore(void) {
	if (!restore_armed) {
		return;
	}
	restore_armed = false;

	char set_list[4 * (VAL_LEN + 8)];
	char unset_list[4 * 64];
	set_list[0] = '\0';
	unset_list[0] = '\0';
	for (int i = 0; i < RESTORE_N; i++) {
		if (restore_present[i]) {
			char quoted[VAL_LEN + 8];
			shell_quote(restore_value[i], quoted, sizeof(quoted));
			append(set_list, sizeof(set_list), "%s%s=%s",
				set_list[0] != '\0' ? " " : "", restore_keys[i], quoted);
		} else {
			/* 原来就没有这个变量 —— 还原 = 删掉，别留个空的在那 */
			append(unset_list, sizeof(unset_list), "%s%s",
				unset_list[0] != '\0' ? " " : "", restore_keys[i]);
		}
	}

	char cmd[sizeof(set_list) + sizeof(unset_list) + 128];
	cmd[0] = '\0';
	if (set_list[0] != '\0') {
		append(cmd, sizeof(cmd),
			"systemctl --user set-environment %s 2>/dev/null; ", set_list);
	}
	if (unset_list[0] != '\0') {
		append(cmd, sizeof(cmd),
			"systemctl --user unset-environment %s 2>/dev/null; ", unset_list);
	}
	append(cmd, sizeof(cmd),
		"systemctl --user restart xdg-desktop-portal 2>/dev/null");

	wlr_log(WLR_INFO, "portal: 还原用户管理器环境（%s%s），重启 xdg-desktop-portal",
		set_list, unset_list);
	vidow_exec_detached(cmd);
}

void vidow_portal_conf_sync(const struct vidow_config *cfg) {
	if (cfg->portal_desktop[0] == '\0') {
		wlr_log(WLR_INFO, "portal: portal_desktop 为空，不碰 portals.conf");
		return;
	}

	char target[PATH_MAX];
	if (!target_path(cfg->portal_desktop, target, sizeof(target))) {
		return;
	}
	char source[PATH_MAX];
	source_path(source, sizeof(source));

	/* 1. 内置默认 → 用户覆盖 */
	struct conf_line eff[MAX_LINES];
	int n = 0;
	for (size_t i = 0; i < sizeof(default_lines) / sizeof(default_lines[0]); i++) {
		const char *eq = strchr(default_lines[i], '=');
		if (eq == NULL) {
			continue;
		}
		size_t klen = (size_t)(eq - default_lines[i]);
		if (klen >= KEY_LEN) {
			continue;
		}
		memcpy(eff[n].key, default_lines[i], klen);
		eff[n].key[klen] = '\0';
		snprintf(eff[n].value, VAL_LEN, "%s", eq + 1);
		n++;
	}

	/* 录屏 / 截图按装了哪个后端来定（GTK 没有这两个接口，KDE 的走 KWin 私有
	 * IPC），见 capture_backend()。用户覆盖在下一步，仍能改掉这两行。 */
	const char *capture = capture_backend();
	if (capture != NULL && n + 2 <= MAX_LINES) {
		snprintf(eff[n].key, KEY_LEN, "%s", "org.freedesktop.impl.portal.ScreenCast");
		snprintf(eff[n].value, VAL_LEN, "%s", capture);
		n++;
		snprintf(eff[n].key, KEY_LEN, "%s", "org.freedesktop.impl.portal.Screenshot");
		snprintf(eff[n].value, VAL_LEN, "%s", capture);
		n++;
		wlr_log(WLR_INFO, "portal: 录屏 / 截图交给 %s 后端", capture);
	} else if (capture == NULL) {
		wlr_log(WLR_INFO, "portal: 没装 xdg-desktop-portal-wlr / -hyprland，"
			"ScreenCast / Screenshot 不写（GTK 没这两个接口，KDE 的走 KWin）");
	}

	struct conf_line ov[MAX_LINES];
	int ov_n = read_override(source, ov, MAX_LINES);
	if (ov_n > 0) {
		wlr_log(WLR_INFO, "portal: 应用用户覆盖 %s（%d 项）", source, ov_n);
		n = merge(eff, n, ov, ov_n);
	} else if (ov_n == 0) {
		wlr_log(WLR_INFO, "portal: %s 是空的，只用内置默认", source);
	}

	/* 2. 序列化成 portal 认的格式。cap 必须按 strlen(header) 算：这个头是
	 * UTF-8 中文，比它看着长得多，少算一格就会把最后一行截掉。 */
	static const char header[] =
		"# 由 vidow 生成，别手改：改 ~/.config/vidow/portals.conf 那份。\n"
		"[preferred]\n";
	size_t cap = sizeof(header);
	for (int i = 0; i < n; i++) {
		cap += strlen(eff[i].key) + strlen(eff[i].value) + 2; /* = 和 \n */
	}
	char *content = malloc(cap);
	if (content == NULL) {
		return;
	}
	memcpy(content, header, sizeof(header) - 1);
	size_t len = sizeof(header) - 1;
	for (int i = 0; i < n; i++) {
		size_t room = cap - len;
		int wrote = snprintf(content + len, room, "%s=%s\n",
			eff[i].key, eff[i].value);
		/* snprintf 返回的是「本该写多长」，一旦被截断 len 就会超过 cap，
		 * 下一轮 cap - len 会下溢成一个巨大的 size_t —— 越界写。 */
		if (wrote < 0 || (size_t)wrote >= room) {
			wlr_log(WLR_ERROR, "portal: portals.conf 缓冲区算错了，截断");
			break;
		}
		len += (size_t)wrote;
	}

	/* 3. 内容没变就别重写文件、别重启前端（重启太吵，还会打断正在进行的
	 * portal 请求）。但 import 环境那一步**不能**跟着跳过：后端是 dbus 按需
	 * 激活的，环境取自用户管理器，而那里留的是上一个合成器的值（Hyprland
	 * 之类）—— 文件没变不代表环境是对的。 */
	size_t old_len = 0;
	char *old = slurp(target, &old_len);
	bool changed = true;
	if (old != NULL) {
		changed = old_len != len || memcmp(old, content, len) != 0;
		free(old);
	}

	if (!cfg->portal_conf_sync) {
		wlr_log(WLR_INFO, "portal: portal_conf_sync=false，只算不写"
			"（%s 需要 %d 行）", target, n);
		free(content);
		return;
	}

	if (changed) {
		ensure_parent(target);
		if (!write_atomic(target, content)) {
			free(content);
			return;
		}
		wlr_log(WLR_INFO, "portal: 写入 %s（%d 行）", target, n);
	} else {
		wlr_log(WLR_DEBUG, "portal: %s 内容没变，只 import 环境", target);
	}
	free(content);

	portal_env_import(changed);
}
