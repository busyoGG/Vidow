/* 配置文件一改就自动重载 —— 不用再 kill -HUP。
 *
 * 盯的是**目录**，不是文件本身。编辑器保存文件的方式不止一种：
 *
 *   - 直接写进去（同一个 inode，mtime / size 变）—— 盯文件也能收到
 *   - 写个临时文件再 rename 过来（inode 换了）—— 盯文件的话 watch 会留在那个
 *     已经被删掉的 inode 上，之后再也收不到事件（vim、许多编辑器的默认行为）
 *
 * 盯目录之后自己 stat() 目标文件，比对 (dev, ino, mtime, size)，**真变了**才
 * 重载。这一层比对顺带解决两件事：一次保存会触发好几个事件（截断、写、改属性）
 * 只算一次；以及「写到一半」—— 事件来了先等 150ms 再 stat，慢速编辑器分几段
 * 写的也只重载一次。
 *
 * 配置拆成好几份（config.lua + configs/ 下那些）之后，盯的就不止一个文件了：
 * vidow.include() 每读一份就登记一份（vidow_configwatch_add），改哪一份都自动
 * 重载。目录跟着文件一起盯 —— 文件还不存在（include 写错了）时也要盯住它的
 * 目录，不然「补上那个文件」这一步不会被发现。
 *
 * 配置文件本身是 Lua，重载是 vidow_config_reload()（和 SIGHUP 同一个入口），
 * 所以自动重载和手动重载的行为完全一致。
 *
 * 这里只管**登记清单**（哪些文件、哪些目录）和**去抖**；inotify fd 本身由
 * fdwatch.c 统一轮询，可读了才回调 configwatch_poke()：有限读取事件，
 * 只有登记文件或目录的事件才启动去抖。
 */
/* realpath() 在 glibc 里挂在 XOPEN 那组下面（_POSIX_C_SOURCE 不够），单独要一下。
 * 放在任何 #include 之前。 */
#define _XOPEN_SOURCE 700

#include "server.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/util/log.h>

/* 事件来了先等这么久再 stat：编辑器一次保存往往分几步落盘。 */
#define VIDOW_CONFIGWATCH_DEBOUNCE_MS 150

/* 盯得住多少个目录 / 多少份配置文件。一个 configs/ 目录 + 入口目录 = 2 个，
 * 64 份配置够拆得很细了；满了只打一条日志（SIGHUP 永远还在）。 */
#define WATCH_MAX_DIRS 16
#define WATCH_MAX_FILES 64

struct watch_dir {
	int wd;
	char path[PATH_MAX];
};

struct watch_file {
	char path[PATH_MAX];
	struct stat seen; /* 上次看到的状态；exists = 上次存在 */
	bool exists;
};

struct vidow_configwatch {
	struct vidow_server *server;
	int fd; /* inotify（不挂事件循环，见文件头：由 fdwatch 兜着） */
	struct wl_event_source *timer;
	struct watch_dir dirs[WATCH_MAX_DIRS];
	size_t ndirs;
	struct watch_file files[WATCH_MAX_FILES];
	size_t nfiles;
	char main_file[PATH_MAX]; /* 入口（符号链接已解析） */
	char main_dir[PATH_MAX];  /* 入口所在目录：永远盯着 */
	bool overflow_logged;
};

/* 路径的目录部分（原地不改入参）。根目录下的话得到 "/"。 */
static void path_dir(const char *path, char *buf, size_t size) {
	snprintf(buf, size, "%s", path);
	char *slash = strrchr(buf, '/');
	if (slash == NULL) {
		snprintf(buf, size, ".");
		return;
	}
	if (slash == buf) {
		buf[1] = '\0';
		return;
	}
	*slash = '\0';
}

/* 两段路径指的是不是同一个目录（可能是两条不同的字符串指同一个地方：
 * $XDG_CONFIG_HOME、/tmp 下的软链接、bind mount…）。用 (dev, ino) 判，
 * stat 不到就当不同。 */
static bool same_dir(const char *a, const char *b) {
	if (strcmp(a, b) == 0) {
		return true;
	}
	struct stat sa, sb;
	if (stat(a, &sa) != 0 || stat(b, &sb) != 0) {
		return false;
	}
	return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/* 盯住一个目录（按路径和 (dev, ino) 去重）。
 * 目录不在 / 没权限都只打日志：自动重载是锦上添花。 */
static bool dir_add(struct vidow_configwatch *w, const char *dir) {
	for (size_t i = 0; i < w->ndirs; i++) {
		if (strcmp(w->dirs[i].path, dir) == 0) {
			return true;
		}
		if (same_dir(w->dirs[i].path, dir)) {
			return true;
		}
	}
	if (w->ndirs >= WATCH_MAX_DIRS) {
		if (!w->overflow_logged) {
			w->overflow_logged = true;
			wlr_log(WLR_INFO, "config.lua: 盯的目录太多了，%s 之后的不盯"
				"（改完要手动 SIGHUP）", dir);
		}
		return false;
	}
	int wd = inotify_add_watch(w->fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO |
		IN_MOVED_FROM | IN_CREATE | IN_DELETE | IN_ATTRIB | IN_MODIFY);
	if (wd < 0) {
		wlr_log(WLR_INFO, "config.lua: 盯不住 %s（%s），改完要手动 SIGHUP",
			dir, strerror(errno));
		return false;
	}
	w->dirs[w->ndirs].wd = wd;
	snprintf(w->dirs[w->ndirs].path, PATH_MAX, "%s", dir);
	w->ndirs++;
	return true;
}

static bool snapshot(const char *path, struct stat *st) {
	if (stat(path, st) != 0) {
		memset(st, 0, sizeof(*st));
		return false;
	}
	return true;
}

/* 记下一份要盯的配置文件（去重），顺带盯住它所在的目录。 */
static void file_add(struct vidow_configwatch *w, const char *path) {
	char resolved[PATH_MAX];
	/* 符号链接（dotfiles 仓库常见）：盯真身，不然改的是仓库里那个文件、
	 * 这边目录里一点动静都没有。文件还不存在时 realpath 会失败，用原路径。 */
	const char *real = realpath(path, resolved) != NULL ? resolved : path;

	for (size_t i = 0; i < w->nfiles; i++) {
		if (strcmp(w->files[i].path, real) == 0) {
			return;
		}
	}
	if (w->nfiles >= WATCH_MAX_FILES) {
		if (!w->overflow_logged) {
			w->overflow_logged = true;
			wlr_log(WLR_INFO, "config.lua: include 的配置文件太多了，%s 之后"
				"的不盯（改完要手动 SIGHUP）", real);
		}
		return;
	}
	struct watch_file *f = &w->files[w->nfiles++];
	snprintf(f->path, PATH_MAX, "%s", real);
	f->exists = snapshot(f->path, &f->seen);

	char dir[PATH_MAX];
	char reall[PATH_MAX];
	path_dir(f->path, dir, sizeof(dir));
	/* 目录路径也解析符号链接，与入口目录的记录保持一致。 */
	if (realpath(dir, reall) != NULL) {
		snprintf(dir, sizeof(dir), "%s", reall);
	}
	dir_add(w, dir);
}

/* 和上次比，这份文件真的变了吗（出现 / 消失 / 换了个 inode / 内容写了）。
 * mtime 只到秒，所以连纳秒一起比 —— 同一秒里存两次同长度内容是常见操作。 */
static bool file_changed(const struct watch_file *f, const struct stat *now,
		bool exists) {
	if (exists != f->exists) {
		return true;
	}
	if (!exists) {
		return false;
	}
	return now->st_dev != f->seen.st_dev || now->st_ino != f->seen.st_ino ||
		now->st_size != f->seen.st_size ||
		now->st_mtim.tv_sec != f->seen.st_mtim.tv_sec ||
		now->st_mtim.tv_nsec != f->seen.st_mtim.tv_nsec;
}

static int watch_timer(void *data) {
	struct vidow_configwatch *w = data;

	bool any = false;
	for (size_t i = 0; i < w->nfiles; i++) {
		struct watch_file *f = &w->files[i];
		struct stat now;
		bool exists = snapshot(f->path, &now);
		if (!file_changed(f, &now, exists)) {
			continue;
		}
		f->seen = now;
		f->exists = exists;
		any = true;
		bool main = strcmp(f->path, w->main_file) == 0;
		if (!exists) {
			wlr_log(WLR_INFO, "config.lua: %s%s 没了，自动重载%s", f->path,
				main ? "" : "（include 进来的）",
				main ? "（回默认值）" : "");
		} else {
			wlr_log(WLR_INFO, "config.lua: %s%s 变了，自动重载", f->path,
				main ? "" : "（include 进来的）");
		}
	}
	if (!any) {
		return 0;
	}
	vidow_config_reload(w->server);
	return 0;
}

/* 只接受登记目录中的目标文件；队列溢出时用 stat 全量核对。 */
static bool event_matches(struct vidow_configwatch *w,
		const struct inotify_event *event) {
	if (event->mask & IN_Q_OVERFLOW) {
		return true;
	}
	for (size_t i = 0; i < w->ndirs; i++) {
		if (w->dirs[i].wd != event->wd) {
			continue;
		}
		if (event->len == 0) {
			return true; /* 登记目录自身的事件。 */
		}
		if (memchr(event->name, '\0', event->len) == NULL) {
			return false;
		}
		for (size_t j = 0; j < w->nfiles; j++) {
			const char *path = w->files[j].path;
			const char *slash = strrchr(path, '/');
			const char *name = slash != NULL ? slash + 1 : path;
			if (strcmp(name, event->name) != 0) {
				continue;
			}
			char dir[PATH_MAX];
			path_dir(path, dir, sizeof(dir));
			if (same_dir(dir, w->dirs[i].path)) {
				return true;
			}
		}
	}
	return false;
}

/* 每拍最多读 64 KiB，避免同目录的持续日志写入占住事件循环。
 * 未读完的事件留给下一拍；读空或只有无关事件不能延后配置重载。 */
static void configwatch_poke(void *data) {
	struct vidow_configwatch *w = data;
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	bool relevant = false;
	for (size_t reads = 0; reads < 16; reads++) {
		ssize_t len = read(w->fd, buf, sizeof(buf));
		if (len < 0 && errno == EINTR) {
			continue;
		}
		if (len <= 0) {
			break;
		}
		for (size_t off = 0; off + sizeof(struct inotify_event) <= (size_t)len;) {
			const struct inotify_event *event = (const void *)(buf + off);
			size_t size = sizeof(*event) + event->len;
			if (size > (size_t)len - off) {
				break;
			}
			relevant |= event_matches(w, event);
			off += size;
		}
	}
	if (relevant && w->timer != NULL) {
		wl_event_source_timer_update(w->timer, VIDOW_CONFIGWATCH_DEBOUNCE_MS);
	}
}

void vidow_configwatch_finish(struct vidow_server *server) {
	struct vidow_configwatch *w = server->configwatch;
	if (w == NULL) {
		return;
	}
	server->configwatch = NULL;
	vidow_fdwatch_deregister(w->fd, VIDOW_FDWATCH_CONFIG, w);
	if (w->timer != NULL) {
		wl_event_source_remove(w->timer);
	}
	if (w->fd >= 0) {
		close(w->fd);
	}
	free(w);
}

/* 重新加载配置之前：上一次 include 过哪些文件不算数了（配置可能改了 include
 * 列表），只留入口和它的目录，剩下的由这一轮 include 重新登记。 */
void vidow_configwatch_begin(struct vidow_server *server) {
	struct vidow_configwatch *w = server->configwatch;
	if (w == NULL) {
		return;
	}
	for (size_t i = 0; i < w->ndirs;) {
		if (strcmp(w->dirs[i].path, w->main_dir) == 0) {
			i++;
			continue;
		}
		inotify_rm_watch(w->fd, w->dirs[i].wd);
		w->dirs[i] = w->dirs[--w->ndirs];
	}
	w->nfiles = 0;
	file_add(w, w->main_file);
}

/* 给 IPC 用：把「现在盯着的文件清单」拷出来（是配置内容，不涉及 fd 细节）。 */
size_t vidow_configwatch_files(struct vidow_server *server,
		const char *const **out) {
	static char *buf[WATCH_MAX_FILES];
	struct vidow_configwatch *w = server != NULL ? server->configwatch : NULL;
	if (w == NULL) {
		*out = NULL;
		return 0;
	}
	for (size_t i = 0; i < w->nfiles; i++) {
		buf[i] = w->files[i].path;
	}
	*out = (const char *const *)buf;
	return w->nfiles;
}

size_t vidow_configwatch_dirs(struct vidow_server *server,
		const char *const **out) {
	static char *buf[WATCH_MAX_DIRS];
	struct vidow_configwatch *w = server != NULL ? server->configwatch : NULL;
	if (w == NULL) {
		*out = NULL;
		return 0;
	}
	for (size_t i = 0; i < w->ndirs; i++) {
		buf[i] = w->dirs[i].path;
	}
	*out = (const char *const *)buf;
	return w->ndirs;
}

void vidow_configwatch_add(struct vidow_server *server, const char *path) {
	struct vidow_configwatch *w = server->configwatch;
	if (w == NULL || path == NULL || path[0] == '\0') {
		return;
	}
	file_add(w, path);
}

/* 盯住配置（入口 + 它 include 的那些）。失败（内核没 inotify / 目录不存在 /
 * 没权限）只打日志：自动重载是锦上添花，SIGHUP 永远还在。 */
bool vidow_configwatch_init(struct vidow_server *server) {
	vidow_configwatch_finish(server);

	char path[PATH_MAX];
	vidow_config_path(path, sizeof(path));

	struct vidow_configwatch *w = calloc(1, sizeof(*w));
	if (w == NULL) {
		return false;
	}
	w->server = server;
	w->fd = -1;

	char resolved[PATH_MAX];
	const char *file = realpath(path, resolved) != NULL ? resolved : path;
	snprintf(w->main_file, sizeof(w->main_file), "%s", file);
	path_dir(w->main_file, w->main_dir, sizeof(w->main_dir));
	char reall[PATH_MAX];
	if (realpath(w->main_dir, reall) != NULL) {
		snprintf(w->main_dir, sizeof(w->main_dir), "%s", reall);
	}

	w->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (w->fd < 0) {
		wlr_log(WLR_INFO, "config.lua: inotify 用不了（%s），改完要手动 SIGHUP",
			strerror(errno));
		free(w);
		return false;
	}

	/* 从这里起，初始化失败也由 finish 回收 fd、timer 和登记项。 */
	server->configwatch = w;
	struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
	w->timer = wl_event_loop_add_timer(loop, watch_timer, w);
	if (w->timer == NULL) {
		wlr_log(WLR_INFO, "config.lua: 自动重载起不来，改完要手动 SIGHUP");
		vidow_configwatch_finish(server);
		return false;
	}
	/* inotify fd 可读时由 fdwatch 调用 configwatch_poke()，筛选事件后去抖。 */
	if (!vidow_fdwatch_peek(w->fd, VIDOW_FDWATCH_CONFIG, configwatch_poke, w)) {
		wlr_log(WLR_INFO, "config.lua: fdwatch 没起来，自动重载收不到事件"
			"（改完要手动 SIGHUP）");
		vidow_configwatch_finish(server);
		return false;
	}

	/* 记下当前状态：初始化这一下不该被当成「文件变了」。
	 * （file_add 顺手会把入口文件所在的目录也盯上 —— inotify 是目录级的。） */
	file_add(w, w->main_file);
	wlr_log(WLR_INFO, "config.lua: 盯住 %s，改了就自动重载", w->main_file);
	return true;
}
