/* env.c — 环境变量账本 + 和 systemd 用户管理器两个方向的对账。见 env.h。
 *
 * 三个方向：
 *
 *   1. 子进程继承（vidow 自己 → 配置里 exec / spawn / 事件钩子起的命令）：它们
 *      fork 的是 vidow 自己，环境天然就在，这里只负责「vidow 的进程环境是对的」。
 *   2. 推给 systemd（vidow → systemd / dbus）：import-environment +
 *      dbus-update-activation-environment --systemd。由 systemd 或 dbus 拉起来的
 *      应用（xdg-desktop-portal、systemd 服务、桌面文件激活的程序）才算「会话里的
 *      应用」，它们的环境从用户管理器拿 —— 不推的话它们看不到 vidow 的
 *      WAYLAND_DISPLAY / XDG_CURRENT_DESKTOP。
 *   3. 从 systemd 吸（systemd → vidow）：vidow 从 tty 直接起（run.sh）时环境很寡，
 *      用户管理器里那些（PATH、SSH_AUTH_SOCK、environment.d 里写的东西）吸收进来，
 *      配置里 exec 的子进程才有一份完整的会话环境。
 *
 * 记账的目的是**别赖账**：vidow.env 改过的变量记着原值，退出时还原；哪些变量是我们
 * 管的、原值是什么，IPC 的 env 命令一条条报出来。 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/util/log.h>

#include "env.h"
#include "json.h"
#include "server.h"

/* 会推给 systemd 的「会话变量」：值由 vidow 定，或者在启动时被改过。
 * 没设的变量不推（推空值反而会把好环境擦掉）。
 *
 * 注意这里**没有 XDG_RUNTIME_DIR**：那是会话自己的东西（systemd 用户管理器里
 * 本来就该有），而嵌套跑 / headless 测试里 vidow 的 XDG_RUNTIME_DIR 是临时的 ——
 * 推上去等于把所有 systemd / dbus 拉起来的程序指到一个不存在的 runtime 目录上
 * （踢过这个坑）。portal 那边要连 PipeWire，自己带着快照和还原管它（见
 * portalconf.c），不归这一份。XCURSOR_SIZE 同理不推：那是「给 X11 客户端的提示」，
 * 推上去会改掉系统里所有程序的指针大小。 */
static const char *session_vars[] = {
	"WAYLAND_DISPLAY",
	"DISPLAY",
	"XDG_CURRENT_DESKTOP",
	"XDG_SESSION_TYPE",
	"XDG_SESSION_DESKTOP",
	"VIDOW_SOCKET",
};

/* 从 systemd 吸收时**不碰**的：这些是「哪个合成器 / 哪个会话」的身份，必须由
 * vidow 自己定（吸进来的多半是宿主那套）。 */
static const char *absorb_deny[] = {
	"WAYLAND_DISPLAY",
	"WAYLAND_SOCKET",
	"DISPLAY",
	"XDG_SESSION_TYPE",
	"XDG_SESSION_DESKTOP",
	"XDG_CURRENT_DESKTOP",
};
/* 前缀也不吸的（wlroots 自己那套开关：吸收了会把我们的后端选择搅乱） */
static const char *absorb_deny_prefix[] = { "WLR_", "VIDOW_" };

static bool denied_by(const char *name, const char *const *list, size_t n,
		bool prefix) {
	for (size_t i = 0; i < n; i++) {
		size_t len = strlen(list[i]);
		if (prefix ? strncmp(name, list[i], len) == 0
				: strcmp(name, list[i]) == 0) {
			return true;
		}
	}
	return false;
}

/* 配置还没跑的时候只有环境变量能说话：VIDOW_SYSTEMD_ENV / VIDOW_SYSTEMD_ENV_EXPORT
 * （配置跑完就由 vidow_config_apply_env 统一管，两边读的是同一个值）。 */
static bool env_override_bool(const char *name, bool fallback) {
	const char *v = getenv(name);
	if (v == NULL || v[0] == '\0') {
		return fallback;
	}
	if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0 ||
			strcmp(v, "off") == 0 || strcmp(v, "no") == 0) {
		return false;
	}
	if (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 ||
			strcmp(v, "on") == 0 || strcmp(v, "yes") == 0) {
		return true;
	}
	return fallback;
}

bool vidow_env_name_ok(const char *name) {
	if (name == NULL || name[0] == '\0' || strlen(name) >= 128) {
		return false;
	}
	if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) {
		return false;
	}
	for (const char *p = name + 1; *p != '\0'; p++) {
		if (!(isalnum((unsigned char)*p) || *p == '_')) {
			return false;
		}
	}
	return true;
}

/* --------------------------------------------------------------- 账本 */

static struct vidow_env_var *env_find(struct vidow_server *server,
		const char *name) {
	struct vidow_env_var *v;
	wl_list_for_each(v, &server->env_vars, link) {
		if (strcmp(v->name, name) == 0) {
			return v;
		}
	}
	return NULL;
}

/* 第一次动某个变量时把原值记下来（之后一直用那一条，别记成我们自己的值）。 */
static bool env_remember(struct vidow_server *server, const char *name,
		char *err, size_t errlen) {
	if (env_find(server, name) != NULL) {
		return true;
	}
	struct vidow_env_var *v = calloc(1, sizeof(*v));
	if (v == NULL) {
		snprintf(err, errlen, "内存不够");
		return false;
	}
	snprintf(v->name, sizeof(v->name), "%s", name);
	const char *old = getenv(name);
	v->had_before = old != NULL;
	if (old != NULL) {
		v->before = strdup(old);
		if (v->before == NULL) {
			free(v);
			snprintf(err, errlen, "内存不够");
			return false;
		}
	}
	wl_list_insert(server->env_vars.prev, &v->link);
	return true;
}

bool vidow_env_set(struct vidow_server *server, const char *name,
		const char *value, char *err, size_t errlen) {
	if (!vidow_env_name_ok(name)) {
		snprintf(err, errlen, "'%s' 不像个环境变量名（[A-Za-z_][A-Za-z0-9_]*）",
			name != NULL ? name : "");
		return false;
	}
	if (value == NULL) {
		snprintf(err, errlen, "%s 的值不能是 nil（要删用 false）", name);
		return false;
	}
	if (!env_remember(server, name, err, errlen)) {
		return false;
	}
	struct vidow_env_var *v = env_find(server, name);
	if (v != NULL) {
		v->absorbed = false; /* 用户显式设的：systemd_env = 0 也别把它还回去 */
		v->explicit = true;
	}
	setenv(name, value, 1);
	wlr_log(WLR_INFO, "环境：%s=%s", name, value);
	vidow_env_export_later(server);
	return true;
}

bool vidow_env_unset(struct vidow_server *server, const char *name, char *err,
		size_t errlen) {
	if (!vidow_env_name_ok(name)) {
		snprintf(err, errlen, "'%s' 不像个环境变量名（[A-Za-z_][A-Za-z0-9_]*）",
			name != NULL ? name : "");
		return false;
	}
	if (!env_remember(server, name, err, errlen)) {
		return false;
	}
	struct vidow_env_var *v = env_find(server, name);
	if (v != NULL) {
		v->absorbed = false;
		v->explicit = true;
	}
	unsetenv(name);
	wlr_log(WLR_INFO, "环境：删掉 %s", name);
	vidow_env_export_later(server);
	return true;
}

/* --------------------------------------------- 读用户管理器的环境 */

/* 一条 NAME=VALUE。 */
struct env_kv {
	char name[128];
	char *value;
};

/* 读用户管理器的环境。两条路：
 *
 *   systemctl --user show-environment   正常会话里最好使（XDG_RUNTIME_DIR 就是
 *                                       会话的）
 *   busctl --user get-property … Manager Environment
 *                                       嵌套跑 / headless（XDG_RUNTIME_DIR 是
 *                                       临时的）时只有这条能连上：它走会话总线
 *                                       （DBUS_SESSION_BUS_ADDRESS），压根不看
 *                                       XDG_RUNTIME_DIR
 *
 * 为什么非要有第二条：**正是那种连不上的环境**里 dbus-update-activation-environment
 * 照样能把用户管理器的环境改掉 —— 读不回来就还原不了，等于把用户的会话环境改脏。
 * 两条都读不到就返回 0（那就什么都不改）。 */
static size_t env_manager_read(struct env_kv *out, size_t max) {
	size_t n = 0;

	FILE *f = popen("systemctl --user show-environment 2>/dev/null", "r");
	if (f != NULL) {
		char line[4096];
		while (n < max && fgets(line, sizeof(line), f) != NULL) {
			size_t len = strlen(line);
			while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
				line[--len] = '\0';
			}
			char *eq = strchr(line, '=');
			if (eq == NULL || eq == line ||
					strlen(line) > sizeof(out[0].name) - 1) {
				continue;
			}
			*eq = '\0';
			size_t namelen = strlen(line);
			if (!vidow_env_name_ok(line) ||
					namelen >= sizeof(out[0].name)) {
				continue;
			}
			memcpy(out[n].name, line, namelen + 1);
			out[n].value = strdup(eq + 1);
			n++;
		}
		pclose(f);
	}
	if (n > 0) {
		return n;
	}

	/* busctl 的应答长这样：as 3 "PATH=/usr/bin" "DISPLAY=:0" … */
	f = popen("busctl --user get-property org.freedesktop.systemd1 "
		"/org/freedesktop/systemd1 org.freedesktop.systemd1.Manager Environment "
		"2>/dev/null", "r");
	if (f == NULL) {
		return 0;
	}
	char buf[65536];
	size_t got = fread(buf, 1, sizeof(buf) - 1, f);
	buf[got] = '\0';
	if (pclose(f) != 0) {
		return 0;
	}
	char *p = strchr(buf, '"');
	while (p != NULL && n < max) {
		p++; /* 过了开头的引号 */
		char text[4096];
		size_t t = 0;
		while (*p != '\0' && *p != '"' && t + 1 < sizeof(text)) {
			if (*p == '\\' && p[1] != '\0') {
				p++; /* busctl 会把 " 和 \ 转义 */
			}
			text[t++] = *p++;
		}
		text[t] = '\0';
		if (*p == '"') {
			p++;
		}
		char *eq = strchr(text, '=');
		size_t textlen = strlen(text);
		if (eq != NULL && eq != text && textlen < sizeof(out[0].name)) {
			*eq = '\0';
			if (vidow_env_name_ok(text)) {
				memcpy(out[n].name, text, strlen(text) + 1);
				out[n].value = strdup(eq + 1);
				n++;
			}
		}
		p = strchr(p, '"');
	}
	return n;
}

static void env_kv_free(struct env_kv *kv, size_t n) {
	for (size_t i = 0; i < n; i++) {
		free(kv[i].value);
	}
}

/* ------------------------------------------------- 从 systemd 吸（启动时） */

void vidow_env_absorb_systemd(struct vidow_server *server) {
	if (!env_override_bool("VIDOW_SYSTEMD_ENV", server->config.systemd_env)) {
		wlr_log(WLR_INFO, "环境：systemd_env = 0，不从用户管理器吸环境");
		return;
	}
	if (server->env_pulled) {
		return; /* 吸过了（配置里又把它打开也一样） */
	}
	struct env_kv kv[256];
	size_t n = env_manager_read(kv, 256);
	int taken = 0;
	for (size_t i = 0; i < n; i++) {
		const char *name = kv[i].name;
		if (denied_by(name, absorb_deny,
				sizeof(absorb_deny) / sizeof(absorb_deny[0]), false) ||
				denied_by(name, absorb_deny_prefix,
				sizeof(absorb_deny_prefix) / sizeof(absorb_deny_prefix[0]),
				true)) {
			continue;
		}
		/* 我们自己已经有了就不动（启动 vidow 的那个 shell / service 说了算）。 */
		if (getenv(name) != NULL) {
			continue;
		}
		/* 记进账本：配置里 systemd_env = 0 的话要还回去，退出时也要还 */
		char err[192] = "";
		if (!env_remember(server, name, err, sizeof(err))) {
			continue;
		}
		struct vidow_env_var *v = env_find(server, name);
		if (v != NULL) {
			v->absorbed = true;
		}
		setenv(name, kv[i].value, 1);
		taken++;
		wlr_log(WLR_DEBUG, "环境：从 systemd 吸来 %s=%s", name, kv[i].value);
	}
	env_kv_free(kv, n);
	server->env_pulled = true;
	wlr_log(WLR_INFO, "环境：从 systemd 用户管理器吸了 %d 个变量（我们自己有的不动）",
		taken);
}

/* ------------------------------------------------- 推给 systemd / dbus */

/* 要推的名字：会话变量里**当前有值**的 + 账本里我们自己设过的（没删的）。 */
static size_t collect_export_names(struct vidow_server *server,
		char names[][128], size_t max_names) {
	size_t n = 0;
	for (size_t i = 0; i < sizeof(session_vars) / sizeof(session_vars[0]); i++) {
		if (getenv(session_vars[i]) == NULL) {
			continue;
		}
		if (n < max_names) {
			snprintf(names[n], 128, "%s", session_vars[i]);
			n++;
		}
	}
	struct vidow_env_var *v;
	wl_list_for_each(v, &server->env_vars, link) {
		if (n >= max_names) {
			break;
		}
		/* 从用户管理器吸进来的不用推回去（它本来就在那儿）；用户 vidow.env
		 * 显式改过的要推（explicit）。 */
		if (v->absorbed && !v->explicit) {
			continue;
		}
		bool dup = false;
		for (size_t i = 0; i < n; i++) {
			if (strcmp(names[i], v->name) == 0) {
				dup = true;
				break;
			}
		}
		if (dup || getenv(v->name) == NULL) {
			continue;
		}
		snprintf(names[n], 128, "%s", v->name);
		n++;
	}
	return n;
}

/* 推上去之前先把「用户管理器里原来是什么样」记下来，退出时还回去。
 *
 * 一个用户管理器是全用户共用的：vidow 嵌套跑（或者 headless 测试跑）的时候把
 * WAYLAND_DISPLAY / DISPLAY 推成自己的，桌面里那些由 systemd / dbus 拉起来的
 * 程序就全指着 vidow 了 —— 而那个 vidow 一退，它们连的地方就没了。portalconf.c
 * 对它那三个变量就是这么干的（快照 + 还原），这一份对推上去的每一个也这么干。 */
static bool env_snap_has(struct vidow_server *server, const char *name) {
	struct vidow_env_snap *sp;
	wl_list_for_each(sp, &server->env_snap, link) {
		if (strcmp(sp->name, name) == 0) {
			return true;
		}
	}
	return false;
}

static void env_snapshot_new(struct vidow_server *server, char names[][128],
		size_t n) {
	size_t missing = 0;
	for (size_t i = 0; i < n; i++) {
		if (!env_snap_has(server, names[i])) {
			missing++;
		}
	}
	if (missing == 0) {
		return;
	}
	struct env_kv kv[256];
	size_t nkv = env_manager_read(kv, 256);
	for (size_t i = 0; i < n; i++) {
		if (env_snap_has(server, names[i])) {
			continue;
		}
		const char *value = NULL;
		for (size_t k = 0; k < nkv; k++) {
			if (strcmp(kv[k].name, names[i]) == 0) {
				value = kv[k].value;
				break;
			}
		}
		struct vidow_env_snap *sp = calloc(1, sizeof(*sp));
		if (sp == NULL) {
			continue;
		}
		snprintf(sp->name, sizeof(sp->name), "%s", names[i]);
		sp->had = value != NULL;
		sp->value = value != NULL ? strdup(value) : NULL;
		wl_list_insert(server->env_snap.prev, &sp->link);
	}
	env_kv_free(kv, nkv);
	int had = 0, absent = 0;
	struct vidow_env_snap *sp;
	wl_list_for_each(sp, &server->env_snap, link) {
		if (sp->had) {
			had++;
		} else {
			absent++;
		}
	}
	wlr_log(WLR_INFO, "环境：记下用户管理器的原样（它有的 %d 个 / 没有的 %d 个）",
		had, absent);
}

/* 单引号包起来（值里可能有空格）：' 变成 '\'' 那套。 */
static void shell_quote_into(const char *in, char *out, size_t outlen) {
	size_t n = 0;
	if (outlen < 3) {
		return;
	}
	out[n++] = '\'';
	for (const char *p = in; *p != '\0' && n + 5 < outlen; p++) {
		if (*p == '\'') {
			out[n++] = '\'';
			out[n++] = '\\';
			out[n++] = '\'';
		} else {
			out[n++] = *p;
		}
	}
	out[n++] = '\'';
	out[n] = '\0';
}

static void env_restore_manager(struct vidow_server *server) {
	/* 两条路都要走，因为「哪条能把用户管理器改脏」取决于 vidow 是在什么环境里
	 * 起来的：
	 *   - 正常会话（XDG_RUNTIME_DIR 就是会话的）：systemctl --user 最好使。
	 *   - 嵌套跑 / headless 测试（XDG_RUNTIME_DIR 是临时的）：systemctl --user
	 *     连不上用户管理器（"Failed to connect to user scope bus via local
	 *     transport"），可 dbus-update-activation-environment 走的是会话总线、
	 *     照样把 systemd 用户管理器的环境改了 —— 也就是说**正好在这种情况下**
	 *     改脏了、而只有 dbus-send 那条路收得回来。所以还原两条都发。 */
	char set[8192] = "";
	char unset_names[4096] = "";
	struct vidow_env_snap *sp;
	wl_list_for_each(sp, &server->env_snap, link) {
		if (sp->had && sp->value != NULL) {
			char q[1024];
			shell_quote_into(sp->value, q, sizeof(q));
			snprintf(set + strlen(set), sizeof(set) - strlen(set), "%s%s=%s",
				set[0] != '\0' ? " " : "", sp->name, q);
		} else {
			snprintf(unset_names + strlen(unset_names),
				sizeof(unset_names) - strlen(unset_names), "%s%s",
				unset_names[0] != '\0' ? " " : "", sp->name);
		}
	}
	if (set[0] == '\0' && unset_names[0] == '\0') {
		return;
	}
	char cmd[3 * (sizeof(set) + sizeof(unset_names)) + 1024];
	cmd[0] = '\0';
	if (set[0] != '\0') {
		snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd),
			"systemctl --user set-environment %s 2>/dev/null; "
			"command -v dbus-update-activation-environment >/dev/null 2>&1"
			" && dbus-update-activation-environment --systemd %s "
			"2>/dev/null; ", set, set);
	}
	/* 用户管理器里本来没有的：一个个 unset 回去。用 dbus-send（走会话总线，
	 * 不依赖 XDG_RUNTIME_DIR —— dbus-update-activation-environment 只能设不能删）。 */
	for (const char *p = unset_names; *p != '\0';) {
		const char *end = strchr(p, ' ');
		size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
		if (len > 0 && len < 128) {
			/* 用 busctl（它会等应答）而不是 dbus-send：dbus-send 不等
			 * 应答就退出，消息会丢（试过，删不掉）。 */
			snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd),
				"command -v busctl >/dev/null 2>&1 && "
				"busctl --user call org.freedesktop.systemd1 "
				"/org/freedesktop/systemd1 "
				"org.freedesktop.systemd1.Manager UnsetEnvironment "
				"as 1 %.*s 2>/dev/null; ", (int)len, p);
		}
		p = end != NULL ? end + 1 : p + len;
	}
	if (unset_names[0] != '\0') {
		snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd),
			"systemctl --user unset-environment %s 2>/dev/null; ",
			unset_names);
	}
	snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), "true");
	wlr_log(WLR_INFO, "环境：还原用户管理器里我们改过的变量（%s%s%s）",
		set[0] != '\0' ? set : "", set[0] != '\0' ? " / " : "",
		unset_names);
	vidow_exec_detached(cmd);
}

void vidow_env_export_now(struct vidow_server *server) {
	if (!env_override_bool("VIDOW_SYSTEMD_ENV_EXPORT",
			server->config.systemd_env_export)) {
		wlr_log(WLR_DEBUG, "环境：systemd_env_export = 0，不推给 systemd");
		return;
	}
	char names[64][128];
	size_t n = collect_export_names(server, names, 64);
	if (n == 0) {
		return;
	}
	/* 第一次推某个变量之前，先问一次「用户管理器里原来是什么样」 */
	env_snapshot_new(server, names, n);
	char list[64 * 129];
	list[0] = '\0';
	for (size_t i = 0; i < n; i++) {
		/* 名字都过了 vidow_env_name_ok，不用引号 */
		snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s",
			i > 0 ? " " : "", names[i]);
	}
	char cmd[2 * sizeof(list) + 256];
	snprintf(cmd, sizeof(cmd),
		"systemctl --user import-environment %s 2>/dev/null; "
		"command -v dbus-update-activation-environment >/dev/null 2>&1 && "
		"dbus-update-activation-environment --systemd %s 2>/dev/null; "
		"true",
		list, list);
	wlr_log(WLR_DEBUG, "环境：推给 systemd / dbus（%zu 个变量）", n);
	vidow_exec_detached(cmd);
	server->env_exports++;
}

static void env_export_later_cb(void *data) {
	struct vidow_server *server = data;
	server->env_idle = NULL;
	vidow_env_export_now(server);
}

void vidow_env_export_later(struct vidow_server *server) {
	if (server == NULL || server->display == NULL ||
			!env_override_bool("VIDOW_SYSTEMD_ENV_EXPORT",
				server->config.systemd_env_export)) {
		return;
	}
	if (server->env_idle != NULL) {
		return;
	}
	server->env_idle = wl_event_loop_add_idle(
		wl_display_get_event_loop(server->display), env_export_later_cb,
		server);
}

/* 配置里 systemd_env 说不要 —— 启动时（还在用默认值）吸进来的那些得还回去，
 * 不然这个开关就成了摆设。用户自己 vidow.env 设过的不动（那是他显式要的）。 */
static void env_absorb_undo(struct vidow_server *server) {
	int undone = 0;
	struct vidow_env_var *v, *tmp;
	wl_list_for_each_safe(v, tmp, &server->env_vars, link) {
		if (!v->absorbed || v->explicit) {
			continue;
		}
		unsetenv(v->name);
		wl_list_remove(&v->link);
		free(v->before);
		free(v);
		undone++;
	}
	server->env_pulled = false;
	if (undone > 0) {
		wlr_log(WLR_INFO, "环境：systemd_env = 0 —— 把启动时吸进来的 %d 个变量"
			"还回去了", undone);
	}
}

void vidow_env_apply_config(struct vidow_server *server) {
	if (!env_override_bool("VIDOW_SYSTEMD_ENV", server->config.systemd_env)) {
		if (server->env_pulled) {
			env_absorb_undo(server);
		}
	} else if (!server->env_pulled) {
		/* 启动时被环境变量挡下了，配置里又打开了：现在吸 */
		vidow_env_absorb_systemd(server);
	}
	vidow_env_export_later(server);
}

/* ---------------------------------------------------------------- IPC */

void vidow_env_json(struct jbuf *b, struct vidow_server *server) {
	jb_obj(b);
	jb_kv_bool(b, "systemd_env", server->config.systemd_env);
	jb_kv_bool(b, "systemd_env_export", server->config.systemd_env_export);
	jb_kv_bool(b, "pulled_from_systemd", server->env_pulled);
	jb_kv_int(b, "exports", (long long)server->env_exports);
	jb_key(b, "would_export");
	jb_arr(b);
	char names[64][128];
	size_t n = collect_export_names(server, names, 64);
	for (size_t i = 0; i < n; i++) {
		jb_obj(b);
		jb_kv_str(b, "name", names[i]);
		jb_kv_str(b, "value", getenv(names[i]));
		jb_obj_end(b);
	}
	jb_arr_end(b);
	jb_key(b, "vars");
	jb_arr(b);
	struct vidow_env_var *v;
	wl_list_for_each(v, &server->env_vars, link) {
		jb_obj(b);
		jb_kv_str(b, "name", v->name);
		const char *now = getenv(v->name);
		jb_kv_str(b, "value", now);
		if (v->had_before) {
			jb_kv_str(b, "before", v->before);
		} else {
			jb_kv_null(b, "before");
		}
		jb_obj_end(b);
	}
	jb_arr_end(b);
	jb_obj_end(b);
}

/* --------------------------------------------------------------- 生命周期 */

bool vidow_env_init(struct vidow_server *server) {
	wl_list_init(&server->env_vars);
	wl_list_init(&server->env_snap);
	server->env_idle = NULL;
	server->env_exports = 0;
	server->env_pulled = false;
	return true;
}

void vidow_env_finish(struct vidow_server *server) {
	if (server->env_idle != NULL) {
		wl_event_source_remove(server->env_idle);
		server->env_idle = NULL;
	}
	/* 推给用户管理器的那些还回去（嵌套跑 / 测试跑时这一步最要紧：不还的话
	 * 桌面里由 systemd 拉起来的程序全指着已经退了的 vidow） */
	env_restore_manager(server);
	struct vidow_env_snap *sp, *sptmp;
	wl_list_for_each_safe(sp, sptmp, &server->env_snap, link) {
		wl_list_remove(&sp->link);
		free(sp->value);
		free(sp);
	}
	/* 退出时把改过的变量还原：vidow 是从调用方的 shell / systemd 里起的，别把
	 * 我们设的东西留在那儿（尤其是从别的会话里嵌套跑的时候）。 */
	struct vidow_env_var *v, *tmp;
	wl_list_for_each_safe(v, tmp, &server->env_vars, link) {
		if (v->had_before) {
			setenv(v->name, v->before, 1);
		} else {
			unsetenv(v->name);
		}
		wl_list_remove(&v->link);
		free(v->before);
		free(v);
	}
}
