#include "xtrace.h"

#include <stdarg.h>
#include <stdio.h>
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

/* 光标链路探针：同一套（环境变量或文件、按秒缓存）。 */
bool vidow_cursor_trace_on(void) {
	if (getenv("VIDOW_CURSOR_TRACE") != NULL) {
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
	cached = stat("/tmp/vidow-cursor-trace", &st) == 0;
	return cached;
}

/* 拖拽跟手探针（见头文件里的说明）：单独一个文件，写完就落盘。 */
void vidow_gtrace(const char *fmt, ...) {
	/* VIDOW_GTRACE 指名写到哪儿（测试用临时目录，别和真实会话那份混）。 */
	const char *path = getenv("VIDOW_GTRACE");
	FILE *f = fopen(path != NULL && path[0] != '\0' ?
		path : "/tmp/vidow-grab-trace.log", "a");
	if (f == NULL) {
		return;
	}
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(f, "%ld.%03ld ", (long)ts.tv_sec,
		(long)(ts.tv_nsec / 1000000));
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

