#include "xtrace.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#define VIDOW_XTRACE_FILE "/tmp/vidow-x11-trace"

/* 和 vidow_xtrace_on 同一套：环境变量或文件，文件方式不用重启。
 * 两秒缓存，免得每帧（160fps 的游戏就是每 6ms）都 stat 一次。 */
bool vidow_commit_trace_on(void) {
	if (getenv("VIDOW_COMMIT_TRACE") != NULL) {
		return true;
	}
	static bool cached = false;
	static time_t checked = 0;
	time_t now = time(NULL);
	if (now == checked) {
		return cached;
	}
	checked = now;
	struct stat st;
	cached = stat("/tmp/vidow-commit-trace", &st) == 0;
	return cached;
}

bool vidow_xtrace_on(void) {
	if (getenv("VIDOW_X11_TRACE") != NULL) {
		return true;
	}
	static bool cached = false;
	static time_t checked = 0;
	time_t now = time(NULL);
	if (now == checked) {
		return cached;
	}
	checked = now;
	struct stat st;
	cached = stat(VIDOW_XTRACE_FILE, &st) == 0;
	return cached;
}
