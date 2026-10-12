#include "server.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>

#include <wlr/util/log.h>

/* Wall-clock, monotonic. Every animation deadline is derived from this, never
 * from a frame counter: a stalled or slow compositor drops frames, it does not
 * stretch the animation. */
int64_t vidow_now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static double ease(enum vidow_easing easing, double t) {
	if (t <= 0.0) {
		return 0.0;
	}
	if (t >= 1.0) {
		return 1.0;
	}
	double u;
	switch (easing) {
	case VIDOW_EASE_LINEAR:
		return t;
	case VIDOW_EASE_OUT_CUBIC:
		u = 1.0 - t;
		return 1.0 - u * u * u;
	case VIDOW_EASE_OUT_QUINT:
		u = 1.0 - t;
		return 1.0 - u * u * u * u * u;
	case VIDOW_EASE_IN_OUT_CUBIC:
		return t < 0.5 ? 4.0 * t * t * t
			: 1.0 - pow(-2.0 * t + 2.0, 3.0) / 2.0;
	case VIDOW_EASE_OUT_BACK: {
		const double c1 = 1.70158, c3 = c1 + 1.0;
		u = t - 1.0;
		return 1.0 + c3 * u * u * u + c1 * u * u;
	}
	case VIDOW_EASE_OUT_ELASTIC:
		/* 阻尼余弦：1 - e^(-k t) * cos(w t)，k = 5.5、w = 7。
		 *
		 * t = 0 正好是 0（曲线必须从起点出发，不能一上来就跳到终点），t≈0.26
		 * 到终点、t≈0.45 冲过头 8.5%、之后单调收敛回去 —— 那一下过冲和一下
		 * 回摆就是「q 弹」。k 再小回摆更明显，但尾巴拖得长：窗口不是果冻，
		 * 收住比晃得欢重要。 */
		return 1.0 - exp(-5.5 * t) * cos(7.0 * t);
	}
	return t;
}

struct vidow_anim *vidow_anim_start(struct vidow_server *server,
		const char *tag, int duration_ms, enum vidow_easing easing,
		void (*update)(void *data, double t, double linear),
		void (*done)(void *data, bool cancelled), void *data) {
	struct vidow_anim *anim = calloc(1, sizeof(*anim));
	if (anim == NULL) {
		return NULL;
	}
	anim->server = server;
	anim->start_ns = vidow_now_ns();
	anim->duration_ns = (int64_t)duration_ms * 1000000LL;
	anim->tag = tag;
	anim->easing = easing;
	anim->update = update;
	anim->done = done;
	anim->data = data;
	wl_list_insert(&server->anims, &anim->link);
	vidow_output_kick_watchdog(server);
	wlr_log(WLR_DEBUG, "anim %s start: %dms", tag, duration_ms);
	return anim;
}

void vidow_anim_cancel(struct vidow_anim *anim) {
	if (anim == NULL || anim->finished) {
		return;
	}
	if (anim->running || anim->server->anim_ticking) {
		/* 在 tick 里被取消（自己正在 update，或者是别的 anim 的 update/done
		 * 回调里转过来的 —— 后者最典型：done 里 vidow_arrange()，而 arrange
		 * 又会取消别的窗口的移动动画）。tick 用的是 wl_list_for_each_safe，
		 * tmp 是「下一个」的裸指针：这里要是 free 了，tmp 当场变野指针，下一
		 * 轮就跳进已释放的 update 里。只标 cancelled，让 tick 自己收尾。 */
		anim->cancelled = true;
		return;
	}
	anim->finished = true;
	wl_list_remove(&anim->link);
	if (anim->done != NULL) {
		anim->done(anim->data, true);
	}
	free(anim);
}

bool vidow_anim_pending(struct vidow_server *server) {
	return !wl_list_empty(&server->anims);
}

void vidow_anim_tick(struct vidow_server *server) {
	if (wl_list_empty(&server->anims)) {
		return;
	}
	int64_t now = vidow_now_ns();
	bool was_ticking = server->anim_ticking;
	server->anim_ticking = true;

	struct vidow_anim *anim, *tmp;
	wl_list_for_each_safe(anim, tmp, &server->anims, link) {
		double raw;
		if (anim->duration_ns <= 0) {
			raw = 1.0;
		} else {
			raw = (double)(now - anim->start_ns) / (double)anim->duration_ns;
			if (raw < 0.0) {
				raw = 0.0;
			}
			if (raw > 1.0) {
				raw = 1.0;
			}
		}

		anim->ticks++;
		anim->running = true;
		if (anim->update != NULL) {
			anim->update(anim->data, ease(anim->easing, raw), raw);
		}
		anim->running = false;

		if (anim->cancelled || raw >= 1.0) {
			bool cancelled = anim->cancelled;
			wlr_log(WLR_DEBUG,
				"anim %s done: %.1fms wall clock, asked %dms, %d frames%s",
				anim->tag, (double)(now - anim->start_ns) / 1e6,
				(int)(anim->duration_ns / 1000000), anim->ticks,
				cancelled ? " (cancelled)" : "");
			anim->finished = true;
			wl_list_remove(&anim->link);
			if (anim->done != NULL) {
				anim->done(anim->data, cancelled);
			}
			free(anim);
		}
	}
	server->anim_ticking = was_ticking;
}
