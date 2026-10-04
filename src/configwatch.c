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
 * 配置文件本身是 Lua，重载是 vidow_config_reload()（和 SIGHUP 同一个入口），
 * 所以自动重载和手动重载的行为完全一致。
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

struct vidow_configwatch {
	struct vidow_server *server;
	int fd; /* inotify */
	struct wl_event_source *source;
	struct wl_event_source *timer;
	char file[PATH_MAX]; /* 目标文件（符号链接已解析） */
	char dir[PATH_MAX];  /* 盯着的目录 */
	struct stat seen;    /* 上次看到的状态；exists = 上次存在 */
	bool exists;
};

static bool snapshot(struct vidow_configwatch *w, struct stat *st) {
	if (stat(w->file, st) != 0) {
		memset(st, 0, sizeof(*st));
		return false;
	}
	return true;
}

/* 和上次比，文件真的变了吗（出现 / 消失 / 换了个 inode / 内容写了）。
 * mtime 只到秒，所以连纳秒一起比 —— 同一秒里存两次同长度内容是常见操作。 */
static bool changed(struct vidow_configwatch *w, struct stat *now, bool exists) {
	if (exists != w->exists) {
		return true;
	}
	if (!exists) {
		return false;
	}
	return now->st_dev != w->seen.st_dev || now->st_ino != w->seen.st_ino ||
		now->st_size != w->seen.st_size ||
		now->st_mtim.tv_sec != w->seen.st_mtim.tv_sec ||
		now->st_mtim.tv_nsec != w->seen.st_mtim.tv_nsec;
}

static int watch_timer(void *data) {
	struct vidow_configwatch *w = data;

	struct stat now;
	bool exists = snapshot(w, &now);
	if (!changed(w, &now, exists)) {
		return 0;
	}
	w->seen = now;
	w->exists = exists;

	if (!exists) {
		wlr_log(WLR_INFO, "config.lua: %s 没了，自动重载（回默认值）", w->file);
	} else {
		wlr_log(WLR_INFO, "config.lua: %s 变了，自动重载", w->file);
	}
	vidow_config_reload(w->server);
	return 0;
}

static int watch_event(int fd, uint32_t mask, void *data) {
	struct vidow_configwatch *w = data;

	if (mask & WL_EVENT_READABLE) {
		/* 排空队列：不读干净的话水平触发会一直回调。事件内容不看 —— 目录里
		 * 别的文件（portals.conf / shaders）也会吵醒我们，stat 比对会挡住。 */
		char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
		while (read(fd, buf, sizeof(buf)) > 0) {
		}
	}
	if (w->timer != NULL) {
		wl_event_source_timer_update(w->timer, VIDOW_CONFIGWATCH_DEBOUNCE_MS);
	}
	return 0;
}

void vidow_configwatch_finish(struct vidow_server *server) {
	struct vidow_configwatch *w = server->configwatch;
	if (w == NULL) {
		return;
	}
	server->configwatch = NULL;
	if (w->timer != NULL) {
		wl_event_source_remove(w->timer);
	}
	if (w->source != NULL) {
		wl_event_source_remove(w->source);
	}
	if (w->fd >= 0) {
		close(w->fd);
	}
	free(w);
}

/* 盯住配置文件。失败（内核没 inotify / 目录不存在 / 没权限）只打日志：自动重载
 * 是锦上添花，SIGHUP 永远还在。 */
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

	/* 符号链接（dotfiles 仓库常见）：盯真身，不然改的是仓库里那个文件、
	 * 这边目录里一点动静都没有。文件还不存在时 realpath 会失败，用原路径。 */
	char resolved[PATH_MAX];
	const char *file = realpath(path, resolved) != NULL ? resolved : path;
	snprintf(w->file, sizeof(w->file), "%s", file);
	snprintf(w->dir, sizeof(w->dir), "%s", file);
	char *slash = strrchr(w->dir, '/');
	if (slash == NULL) {
		free(w);
		return false;
	}
	if (slash == w->dir) {
		w->dir[1] = '\0'; /* 就在根目录下 */
	} else {
		*slash = '\0';
	}

	w->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (w->fd < 0) {
		wlr_log(WLR_INFO, "config.lua: inotify 用不了（%s），改完要手动 SIGHUP",
			strerror(errno));
		free(w);
		return false;
	}
	if (inotify_add_watch(w->fd, w->dir, IN_CLOSE_WRITE | IN_MOVED_TO |
			IN_MOVED_FROM | IN_CREATE | IN_DELETE | IN_ATTRIB | IN_MODIFY) < 0) {
		wlr_log(WLR_INFO, "config.lua: 盯不住 %s（%s），改完要手动 SIGHUP",
			w->dir, strerror(errno));
		close(w->fd);
		free(w);
		return false;
	}

	struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
	w->source = wl_event_loop_add_fd(loop, w->fd, WL_EVENT_READABLE,
		watch_event, w);
	w->timer = wl_event_loop_add_timer(loop, watch_timer, w);
	if (w->source == NULL || w->timer == NULL) {
		wlr_log(WLR_INFO, "config.lua: 自动重载起不来，改完要手动 SIGHUP");
		vidow_configwatch_finish(server);
		return false;
	}

	/* 记下当前状态：初始化这一下不该被当成「文件变了」。 */
	w->exists = snapshot(w, &w->seen);
	server->configwatch = w;
	wlr_log(WLR_INFO, "config.lua: 盯住 %s，改了就自动重载", w->file);
	return true;
}
