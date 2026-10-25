#include "xtrace.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#define VIDOW_XTRACE_FILE "/tmp/vidow-x11-trace"

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
