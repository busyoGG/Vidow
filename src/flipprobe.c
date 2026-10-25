/* flipprobe.c — 状态翻转探针的实现（见 flipprobe.h 的说明）。 */
#include "flipprobe.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <wlr/util/log.h>

struct flip_slot {
	const char *what;
	void *obj;
	bool last;
	bool seen;      /* 第一次调用只记账，不算翻转 */
	int flips;
	int total;
};

static struct flip_slot flip_slots[512];
static int flip_n;
static long long flip_last_ns;

void vidow_flip_note(const char *what, void *obj, bool now) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	long long t = (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
	if (flip_last_ns == 0) {
		flip_last_ns = t;
	}
	struct flip_slot *slot = NULL;
	for (int i = 0; i < flip_n; i++) {
		if (flip_slots[i].obj == obj &&
				strcmp(flip_slots[i].what, what) == 0) {
			slot = &flip_slots[i];
			break;
		}
	}
	if (slot == NULL) {
		if (flip_n >= (int)(sizeof(flip_slots) / sizeof(flip_slots[0]))) {
			return;
		}
		slot = &flip_slots[flip_n++];
		slot->what = what;
		slot->obj = obj;
		slot->last = now;
		slot->seen = true;
		return;
	}
	if (slot->last == now) {
		return; /* 没变，不算翻转 */
	}
	slot->last = now;
	slot->flips++;
	slot->total++;

	/* 每 2 秒结算一次 */
	if (t - flip_last_ns < 2000000000LL) {
		return;
	}
	int hot = 0;
	for (int i = 0; i < flip_n; i++) {
		if (flip_slots[i].flips >= 8) {
			hot++;
			wlr_log(WLR_INFO,
				"翻转: %s obj=%p 2 秒内翻了 %d 次（累计 %d）—— 状态不稳",
				flip_slots[i].what, flip_slots[i].obj,
				flip_slots[i].flips, flip_slots[i].total);
		}
		flip_slots[i].flips = 0;
	}
	if (hot == 0) {
		wlr_log(WLR_DEBUG, "翻转: 2 秒内无反复切换（跟踪 %d 个对象）",
			flip_n);
	}
	flip_last_ns = t;
}
