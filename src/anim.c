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
	}
	return t;
}

struct vidow_anim *vidow_anim_start(struct vidow_server *server,
		const char *tag, int duration_ms, enum vidow_easing easing,
		void (*update)(void *data, double t),
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
	if (anim->running) {
		/* Called from inside update(): let the tick loop finish with it so we
		 * never free the struct we are currently standing on. */
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
			anim->update(anim->data, ease(anim->easing, raw));
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
}
