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

/* 三次贝塞尔某一维在参数 t 处的值：两个控制点 (a, b)、端点固定在 0 / 1，
 * B(t) = 3(1-t)²t·a + 3(1-t)t²·b + t³。x 和 y 各算一次，用的就是这一份。 */
static double bezier_axis(double a, double b, double t) {
	double u = 1.0 - t;
	return 3.0 * u * u * t * a + 3.0 * u * t * t * b + t * t * t;
}

/* 自定义贝塞尔：**按 x 求 y**。
 *
 * 配置给的是两个控制点 (x1,y1) (x2,y2)，曲线的 x(t)、y(t) 都是上面那条式子。
 * 动画拿到的是进度 x（0..1），要的是它对应的 y —— 所以先解 x(t) = x，再取
 * y(t)。直接拿 x 当参数去算 y 是错的：那等于「进度按 t 均匀」，曲线形状完全
 * 不对（该冲的地方不冲）。
 *
 * 怎么解：控制点 x 都在 0..1（config.c 校验过），x(t) 因此单调，二分就行。
 * 牛顿迭代收敛快，但 x'(t) 在 x1 = 1 / x2 = 0 这类边上会退化（除零或原地
 * 打转），二分不用管这些。32 次 = 区间 2^-32，比要的 1e-4 精度高好几个数量级，
 * 而且每次迭代就几次乘加 —— 一帧里最多几条动画，这点开销看不见。 */
static double bezier_at(const struct vidow_ease *ease, double x) {
	double lo = 0.0, hi = 1.0;
	for (int i = 0; i < 32; i++) {
		double mid = 0.5 * (lo + hi);
		if (bezier_axis(ease->p1x, ease->p2x, mid) < x) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	return bezier_axis(ease->p1y, ease->p2y, 0.5 * (lo + hi));
}

/* 回弹幅度 → easeOutBack 里那个常数 c1。
 *
 * 曲线是 f(u) = 1 + c3·u³ + c1·u²（u = t - 1，c3 = c1 + 1），它的峰值过冲
 *     s = 4·c1³ / (27·(c1+1)²)
 * 对 c1 > 0 单调递增，所以给定「冲过终点多少」反解 c1 用二分就行。这个只在
 * 配置解析时算一次（见 config.c 的 sync_easings），不在每帧里跑。
 *
 * 默认幅度 0.10 反解出来是 1.701540，和当年写死的 1.70158 差 4e-5 —— 曲线差
 * ~4e-7，逐帧渲染看不出来（写死那个值的峰值过冲是 0.1000041，本来就不是正好
 * 0.10）。 */
double vidow_ease_out_back_c1(double overshoot) {
	if (overshoot <= 0.0) {
		/* 不过冲：f(u) = 1 + u³，正好就是 out_cubic。 */
		return 0.0;
	}
	double lo = 0.0, hi = 32.0;
	for (int i = 0; i < 60; i++) {
		double mid = 0.5 * (lo + hi);
		double s = 4.0 * mid * mid * mid /
			(27.0 * (mid + 1.0) * (mid + 1.0));
		if (s < overshoot) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	return 0.5 * (lo + hi);
}

static double ease(const struct vidow_ease *ease, double t) {
	if (t <= 0.0) {
		return 0.0;
	}
	if (t >= 1.0) {
		return 1.0;
	}
	double u;
	switch (ease->kind) {
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
		/* 回弹幅度是配置里给的（`back_overshoot`，或者曲线里写
		 * `easing = "out_back 0.2"`）：c1 已经在解析时反解好了。 */
		double c1 = ease->back_c1, c3 = c1 + 1.0;
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
	case VIDOW_EASE_BEZIER:
		return bezier_at(ease, t);
	}
	return t;
}

/* 一条 anim 里要用第二条曲线（小窗转换：位置单调、缩放可过冲）时用这个取，
 * 别为第二条曲线再起一条 anim —— 两条 anim 会多出两个生命周期要收。 */
double vidow_ease_value(const struct vidow_ease *curve, double t) {
	return ease(curve, t);
}

struct vidow_anim *vidow_anim_start(struct vidow_server *server,
		const char *tag, int duration_ms, struct vidow_ease easing,
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
			anim->update(anim->data, ease(&anim->easing, raw), raw);
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
