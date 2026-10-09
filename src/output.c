#include "server.h"

#include <drm_fourcc.h> /* DRM_FORMAT_XRGB8888 / XRGB2101010（输出缓冲的位深） */
#include <strings.h> /* strcasecmp（方向的名字） */

#include <math.h>
#include <stdlib.h>
#include <time.h>

#include "ipc.h"

#include <wlr/backend/headless.h>
#include <wlr/util/log.h>

/* 环境变量只问一次。
 *
 * 这两个变量以前是**每帧**问一遍（渲染统计两次、帧追踪钩子一次），而 getenv 要
 * 遍历 environ 做 strcmp —— 实测 profile 里它占 0.39%，比我们自己一帧的模糊代码
 * （vidow_blur_update 0.26%）还显眼。进程起来之后环境不会变，缓存住就行。 */
static bool render_stats_on(void) {
	static int on = -1;
	if (on < 0) {
		on = getenv("VIDOW_RENDER_STATS") != NULL ? 1 : 0;
	}
	return on != 0;
}

/* 帧事件掐断的测试钩子（VIDOW_TEST_STALL_OUTPUT=<屏名>），同样只问一次。 */
static const char *stall_output_name(void) {
	static int done = 0;
	static const char *name = NULL;
	if (!done) {
		name = getenv("VIDOW_TEST_STALL_OUTPUT");
		done = 1;
	}
	return name;
}

/* Advance the animation clock and present one frame. */
void vidow_output_render(struct vidow_output *output) {
	struct vidow_server *server = output->server;

	/* 出帧了：看门狗按这个时刻给自己限速（见 watchdog_interval_ns）。放在最前面，
	 * 中途哪条早退都不影响「这一帧确实画过了」。 */
	output->last_render_ns = vidow_now_ns();

	server->output_w = output->wlr_output->width;
	server->output_h = output->wlr_output->height;

	/* 装饰着色器工作在「输出的物理像素」空间里（散景贴图也是物理分辨率），
	 * 所以窗口位置要减掉本输出的布局原点、再乘缩放。多输出下这是必须的：
	 * 少了原点，第二块屏上的模糊会按第一块屏的坐标采样。 */
	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
	/* 顺手记一份：拔线时 wlr_output_layout 可能已经先把这块屏忘掉了，
	 * 热插拔要靠这份存着的位置算「挪到别的屏」的平移量。
	 * 位置真的变了就打一行：拔掉一块屏时 wlroots 会重新摆放剩下的输出
	 * （实测：拔掉左边那块，右边那块从 x=1280 挪到了 x=0），这件事用户和
	 * 测试都需要看得见。 */
	if (box.x != output->box.x || box.y != output->box.y ||
			box.width != output->box.width ||
			box.height != output->box.height) {
		if (output->box.width > 0) {
			wlr_log(WLR_INFO, "output %s: 布局位置 %d,%d %dx%d -> %d,%d %dx%d",
				output->wlr_output->name, output->box.x, output->box.y,
				output->box.width, output->box.height, box.x, box.y,
				box.width, box.height);
		}
		output->box = box;
	}
	double scale = output->wlr_output->scale;
	server->output_x = (int)lround(box.x * scale);
	server->output_y = (int)lround(box.y * scale);

	/* 探针：拖拽的「指针事件 -> 出帧」延迟（这一帧要出帧了，把从最后一个事件到现在
	 * 的时间记一笔）。嵌套在别的合成器里时，光标是**上层**画的、窗口是我们画的，
	 * 这个延迟直接决定「跟不跟手」。 */
	if (render_stats_on() && server->grab_ev_ns != 0 &&
			(server->grab_dirty || server->arrange_dirty)) {
		int64_t lat = vidow_now_ns() - server->grab_ev_ns;
		vidow_stats.grab_lat_ns += lat;
		if (lat > vidow_stats.grab_lat_max_ns) {
			vidow_stats.grab_lat_max_ns = lat;
		}
		vidow_stats.grab_lat_n++;
		server->grab_ev_ns = 0;
	}

	/* overview 开着的话先把变换算好、把窗口按它摆一遍（缩放里每帧都要摆，
	 * 因为客户端 commit 会把场景尺寸改回去）。 */
	vidow_overview_update(server);

	/* 浮动窗口拖拽：一帧里来过的那些移动事件在这里一次性重排（位置取最后一个）。 */
	if (server->arrange_dirty) {
		server->arrange_dirty = false;
		vidow_arrange(server);
	}

	/* 拖动跟手：一帧里来过的那些移动事件在这里一次性摆到位（位置取最后一个）。
	 * 中间那些位置屏幕上从来没出现过，所以「从上一帧画在哪到这一帧画在哪」记一笔
	 * 损伤就够 —— 拖得快的时候一帧能跨过一大截，只记新位置会漏。 */
	if (server->grab_dirty && server->grab_view != NULL) {
		struct vidow_view *gv = server->grab_view;
		server->grab_dirty = false;
		vidow_backdrop_damage_box(server, &server->grab_last_box);
		/* 只有这个窗口自己动了：记在它头上，它自己的私有底衬（抓的时候它藏着）
		 * 不该因为这一笔就过期。 */
		vidow_backdrop_damage_view(gv);
		vidow_view_apply_anim(gv);
		server->grab_last_box = vidow_view_screen_box(gv);
	}

	/* 临时探针（VIDOW_RENDER_STATS=1）：每帧各阶段耗时 + 贴图导入/重建次数 */
	int64_t st_t0 = vidow_now_ns();
	vidow_blur_update(output);
	int64_t st_t1 = vidow_now_ns();

	/* A blurred decoration is only redrawn when this frame's backdrop differs
	 * from the one already baked into its buffer. On a still screen nothing
	 * moves behind the windows, so this loop does no work at all. */
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->fx.active && view->fx.blur_drawn != view->fx.blur_ver) {
			vidow_view_apply_anim(view);
		}
	}

	/* Progress comes from CLOCK_MONOTONIC, so a slow frame skips ahead
	 * instead of stretching the animation. */
	int64_t st_t2 = vidow_now_ns();
	vidow_anim_tick(server);
	/* 拖窗口的果冻：积分弹簧、把弯了的窗口重画一遍（见 jelly.c）。 */
	vidow_jelly_tick(server, vidow_now_ns());
	int64_t st_t3 = vidow_now_ns();

	/* 屏幕这一帧就是屏幕这一帧：抓帧是 vidow 自己另外渲染的一遍（capture.c /
	 * screencopy.c），和这里没有任何关系 —— capture = "hide" 的窗口 / layer
	 * 屏幕上照常显示，只在抓帧里没有。 */
	/* 这一帧所有离屏趟（模糊链 / 装饰 / 果冻）的命令在这里一次性提交，
	 * 而不是每趟一次 —— 见 fx.c 的 end_pass。 */
	vidow_fx_flush(server);
	wlr_scene_output_commit(output->scene_output, NULL);
	int64_t st_t4 = vidow_now_ns();
	if (render_stats_on()) {
		vidow_stats.blur_ns += st_t1 - st_t0;
		vidow_stats.decor_ns += st_t2 - st_t1;
		vidow_stats.jelly_ns += st_t3 - st_t2;
		vidow_stats.commit_ns += st_t4 - st_t3;
		vidow_stats.frames++;
		static int64_t win_ns = 0;
		if (win_ns == 0) {
			win_ns = st_t0;
		}
		if (st_t4 - win_ns >= 500 * 1000000LL) {
			double f = vidow_stats.frames > 0 ? (double)vidow_stats.frames : 1.0;
			wlr_log(WLR_INFO,
				"渲染统计: %d 帧/帧窗(%.0fms) %.1f fps | 模糊 %.2fms/帧"
				"(shared %d own %d chain %d) | 装饰 %.2fms/帧(%d 次重烘) | "
				"果冻 %.2fms/帧 | 提交 %.2fms/帧 | 贴图 +%d/-%d | 趟 %d | "
				"重烘 %.2fms/帧 | 拖拽延迟 %.2f/%.2fms(%d)",
				vidow_stats.frames, (double)(st_t4 - win_ns) / 1e6,
				1000.0 * f / ((double)(st_t4 - win_ns) / 1e6),
				(double)vidow_stats.blur_ns / 1e6 / f,
				vidow_stats.shared, vidow_stats.own, vidow_stats.chains,
				(double)vidow_stats.decor_ns / 1e6 / f, vidow_stats.bakes,
				(double)vidow_stats.jelly_ns / 1e6 / f,
				(double)vidow_stats.commit_ns / 1e6 / f,
				vidow_stats.tex_new, vidow_stats.tex_del, vidow_stats.passes,
				(double)vidow_stats.bake_ns / 1e6 / f,
				vidow_stats.grab_lat_n > 0 ?
					(double)vidow_stats.grab_lat_ns / 1e6 / vidow_stats.grab_lat_n : 0.0,
				(double)vidow_stats.grab_lat_max_ns / 1e6,
				vidow_stats.grab_lat_n);
			vidow_stats = (struct vidow_frame_stats){0};
			win_ns = st_t4;
		}
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(output->scene_output, &now);

	/* 视口外的窗口要不要保活（窗口规则 / 录屏预览）。放在场景补帧之后：已经画在
	 * 画面上的窗口场景那边已经给过回调了，这里只补没画到的那些。 */
	vidow_view_render_pump(server, output,
		(uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000));

}

/* ---------------------------------------------------------------- 帧追踪
 *
 * VIDOW_TRACE_FRAMES=1 时开：每次「要帧」记一笔（谁要的、要了几次），帧事件回来
 * 就销账；250ms 一轮检查，发现「要了帧但 300ms 没帧事件」就喊一行 —— 那块屏的
 * 帧链断了（后端没给帧事件）。用户报过「某块屏上的窗口停在最后一帧、拖到另一块屏
 * 就活」，那种只在真 DRM 上出现的问题就靠这几行定案。默认不开。 */
struct vidow_output *vidow_output_lookup(struct vidow_server *server,
		struct wlr_output *wlr_output) {
	if (server == NULL || wlr_output == NULL) {
		return NULL;
	}
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->wlr_output == wlr_output) {
			return output;
		}
	}
	return NULL;
}

static int trace_timer_cb(void *data) {
	struct vidow_server *server = data;
	int64_t now = vidow_now_ns();
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		int64_t gap = (now - output->last_frame_ns) / 1000000;
		if (gap < 300) {
			continue;
		}
		bool stale_content = output->content_ns > output->last_frame_ns;
		if (!stale_content && !output->want_frame) {
			continue;
		}
		wlr_log(WLR_ERROR, "trace: %s %lldms 没出帧（vidow 主动要过 %d 帧，"
			"最近一次：%s）%s", output->wlr_output->name, (long long)gap,
			output->want_frame_n,
			output->want_frame_why != NULL ? output->want_frame_why : "没要过",
			stale_content ? "；客户端还在交帧，屏幕却不更新 ← 帧链断了" : "");
		output->want_frame = false; /* 记一次就够，别刷屏 */
		output->content_ns = output->last_frame_ns;
	}
	wl_event_source_timer_update(server->trace_timer, 250);
	return 0;
}

/* 要一帧。记账只为诊断（VIDOW_TRACE_FRAMES=1 时 trace_timer_cb 会用它），
 * 找不到对应的 vidow_output 也照样排帧 —— 诊断不许改变行为。 */
void vidow_output_schedule_frame(struct vidow_server *server,
		struct wlr_output *wlr_output, const char *why) {
	if (wlr_output == NULL) {
		return;
	}
	struct vidow_output *output = vidow_output_lookup(server, wlr_output);
	if (output != NULL) {
		output->want_frame = true;
		output->want_frame_n++;
		output->want_frame_why = why;
	}
	wlr_output_schedule_frame(wlr_output);
}

/* 客户端交帧了：帧追踪记一笔（没开追踪就什么都不做，零开销）。 */
void vidow_output_trace_commit(struct vidow_output *output) {
	if (output == NULL || output->server->trace_timer == NULL) {
		return;
	}
	output->content_ns = vidow_now_ns();
}

/* 补帧闹钟到点：催一帧出来（渲染里会走到 vidow_view_render_pump，到点的窗口就补上）。
 * 只在这里催，不在「还没到点」的时候催 —— 那样会变成每帧都渲染却什么都不补的空转。 */
static int pump_timer_cb(void *data) {
	struct vidow_output *output = data;
	output->pump_at_ms = 0;
	vidow_output_schedule_frame(output->server, output->wlr_output,
		"补帧闹钟到点");
	return 0;
}

/* A screen capture is answered on an output commit, and a still screen never
 * commits: the nested backend will not even send us a frame callback, because
 * the host only composites on damage. Poll gently while a capture is
 * outstanding and render for it directly.
 *
 * 只在**真的有人等着**的时候转：这个定时器原来是建好就 50ms 一次永远转下去，
 * 空载也要每秒醒 20 次（实测 strace：21 秒里 epoll_wait 返回 427 次、
 * timerfd_settime 852 次 —— 期间一次抓帧都没有）。现在由抓帧请求踢一脚
 * （见 vidow_output_capture_kick），抓完没人等着就自己停 —— 空载零唤醒。 */
static int capture_watchdog(void *data) {
	struct vidow_server *server = data;
	if (!vidow_screencopy_has_pending(server->screencopy)) {
		return 0; /* 没人等着：不续期 */
	}
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		vidow_output_render(output);
	}
	wl_event_source_timer_update(server->capture_timer, 50);
	return 0;
}

/* 有人要抓帧了：把上面的看门狗叫起来（它是「静止的屏幕永远不提交」的兜底，
 * 见 capture_watchdog）。请求进来时踢一次就够 —— 它会自己续期到抓完。 */
void vidow_output_capture_kick(struct vidow_server *server) {
	if (server == NULL || server->capture_timer == NULL) {
		return;
	}
	wl_event_source_timer_update(server->capture_timer, 50);
}

/* 看门狗自己推帧的节奏：跟着后端**自己**的帧间隔走，比它晚 1/4 才补一帧。
 *
 * 后端正常出帧时一帧都不多推 —— 白推一帧就是白画一屏（模糊链 + 装饰 + 提交，
 * 4K@2 上 ~1ms，嵌套时还会多插一次 commit 打乱上层的帧链）。后端停帧时才按这个
 * 节奏补上（嵌套在别的合成器里，上层一忙就不给帧）。
 *
 * 上限 40ms 是原来那条硬编码的兜底（后端 25Hz 就认了，别自作聪明替它画）；
 * 下限 6ms 是「至少 ~160Hz」—— 拖动是跟着指针走的，25fps 的步进肉眼就是
 * 「一顿一顿」（用户报的），所以拖动期间宁可多推也不能卡。
 * frame_gap_ns == 0（还没量到过一次正常间隔）按 60Hz 猜，拖动期间按 8ms（~125Hz）
 * 猜 —— 拖动时「宁可多推」优先，动画不用那么急。 */
static int64_t watchdog_interval_ns(struct vidow_output *output, bool dragging) {
	int64_t gap = output->frame_gap_ns > 0 ? output->frame_gap_ns
		: (dragging ? 8 : 16) * 1000000LL;
	int64_t iv = gap + gap / 4;
	if (iv < 6 * 1000000LL) {
		iv = 6 * 1000000LL;
	}
	if (iv > 40 * 1000000LL) {
		iv = 40 * 1000000LL;
	}
	return iv;
}

/* The nested Wayland backend only delivers a frame event once the host
 * compositor has presented our surface, and the host only presents on damage:
 * that deadlocks an animation that is waiting for its next frame. If frames
 * dry up while an animation is in flight, drive it from a timer instead. */
static int anim_watchdog(void *data) {
	struct vidow_server *server = data;
	server->watchdog_armed = false;
	/* 拖着窗口也算「有活在飞」：浮动拖拽的落位是推迟到帧里做的（见 float.c 的
	 * arrange_dirty），帧一停它就**一点都不动** —— 上层合成器一旦不给帧事件
	 * （嵌套在别的合成器里、窗口被盖住 / 那台合成器自己闲着），拖动就冻住。 */
	bool dragging = server->grab_view != NULL;
	if (!vidow_anim_pending(server) && !vidow_jelly_pending(server) &&
			!dragging) {
		return 0;
	}
	int64_t now = vidow_now_ns();
	int64_t wait_ms = 0;
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		int64_t iv = watchdog_interval_ns(output, dragging);
		int64_t since = now - output->last_render_ns;
		if (since >= iv) {
			vidow_output_render(output); /* 后端欠着这一帧，自己补上 */
			since = 0;
			iv = watchdog_interval_ns(output, dragging);
		}
		/* 下一轮就在「这个间隔还剩多久」之后 —— 按剩余时间排，而不是固定
		 * 4ms / 16ms 地空转：4ms 一轮的话，10ms 的目标节奏实际会落成 12ms
		 * （4 的整数倍），白丢 20% 的帧。 */
		int64_t left = (iv - since) / 1000000 + 1;
		if (wait_ms == 0 || left < wait_ms) {
			wait_ms = left;
		}
	}
	/* 还有活在飞就继续自己推。这条续期是关键：不续的话看门狗只响一次，下次
	 * 得等新的指针事件来 kick（拖动时后端要是这段时间一直不给帧，就又冻住了）。 */
	server->watchdog_armed = true;
	wl_event_source_timer_update(server->anim_timer,
		(int)(wait_ms < 1 ? 1 : wait_ms));
	return 0;
}

/* Armed whenever an animation starts or a frame is presented, so the
 * watchdog is running even if the backend never sends another frame.
 * 已经排上就不再排 —— 拖动时每个指针事件都会调到这儿（见 server.h 的
 * watchdog_armed）。 */
void vidow_output_kick_watchdog(struct vidow_server *server) {
	if (server->anim_timer == NULL || server->watchdog_armed) {
		return;
	}
	server->watchdog_armed = true;
	wl_event_source_timer_update(server->anim_timer, 8);
}

/* 立刻给每块屏画一帧。切 tty 切回来的路上（DRM 后端会把所有输出销毁再重建）
 * 和输出刚建出来的时候，场景里没有任何 damage，后端也不欠我们 frame 事件 ——
 * 不主动画一次，屏幕就一直停在上一帧（用户看到的「卡死」）。
 * 走 1ms 定时器而不是当场画：调用点在 wlroots 的会话处理里面，等这一轮派发
 * 结束再画干净。 */
static int kick_timer_cb(void *data) {
	struct vidow_server *server = data;
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		vidow_output_render(output);
	}
	return 0;
}

void vidow_output_kick_all(struct vidow_server *server) {
	if (server->kick_timer == NULL) {
		kick_timer_cb(server); /* 没有定时器（极早期）就当场画 */
		return;
	}
	wl_event_source_timer_update(server->kick_timer, 1);
}

static void output_frame(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_output *output = wl_container_of(listener, output, frame);
	struct vidow_server *server = output->server;

	/* 测试钩子：当作「后端不再给这块屏帧事件」= 用户报过的「这块屏上的窗口停在
	 * 最后一帧，拖到另一块屏就活」。真机上只有 DRM 的 page-flip 不回来才会这样
	 * （headless 的帧事件和 commit 无关，按不住）；帧追踪的判据靠它验。 */
	const char *stall = stall_output_name();
	if (stall != NULL && strcmp(stall, output->wlr_output->name) == 0) {
		return;
	}

	int64_t now_ns = vidow_now_ns();
	if (output->want_frame && server->trace_timer != NULL) {
		int64_t gap = (now_ns - output->last_frame_ns) / 1000000;
		if (gap >= 300) {
			wlr_log(WLR_INFO, "trace: %s 又出帧了（停了 %lldms，这期间被要了"
				" %d 帧）", output->wlr_output->name, (long long)gap,
				output->want_frame_n);
		}
	}
	output->want_frame = false;
	output->want_frame_n = 0;
	/* 后端帧间隔的滑动平均（看门狗的目标节奏，见 watchdog_interval_ns）。
	 * 只喂「看着正常」的间隔（< 25ms = 40Hz 以上）：后端自己就慢或者刚停过帧的
	 * 那些长间隔喂进去，会把目标节奏越拉越长，看门狗跟着一起变慢 —— 拖动就又
	 * 开始一顿一顿。真正的停帧（几百 ms）本来就该由看门狗按健康的节奏补上。 */
	int64_t gap_ns = now_ns - output->last_frame_ns;
	if (output->last_frame_ns != 0 && gap_ns > 0 &&
			gap_ns < 25 * 1000000LL) {
		output->frame_gap_ns = output->frame_gap_ns > 0 ?
			(output->frame_gap_ns * 3 + gap_ns) / 4 : gap_ns;
	}
	output->last_frame_ns = now_ns;
	vidow_output_render(output);

	/* An animating node usually damages the scene, but not always (a pure
	 * opacity change on a fully occluded window). Ask for the next frame
	 * anyway, and arm the watchdog in case the request goes nowhere.
	 * 拖着果冻也一样：网格在动，帧不能停。 */
	if (vidow_anim_pending(server) || vidow_jelly_pending(server) ||
			server->grab_view != NULL) {
		vidow_output_schedule_frame(server, output->wlr_output,
			server->grab_view != NULL ? "拖动在跑" : "动画/果冻在跑");
		vidow_output_kick_watchdog(server);
	}
}

static void output_request_state(struct wl_listener *listener, void *data) {
	struct vidow_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;

	if (wlr_output_commit_state(output->wlr_output, event->state)) {
		/* Output size changed (e.g. the host resized our nested window):
		 * recompute the tiles and throw away the output-sized backdrops. */
		vidow_backdrop_damage(output->server, true);
		vidow_arrange(output->server);
	}
}

/* 方向的名字 -> 枚举（config 和 IPC 共用）。 */
bool vidow_output_parse_transform(const char *text, enum wl_output_transform *out) {
	static const struct {
		const char *name;
		enum wl_output_transform t;
	} names[] = {
		{ "normal", WL_OUTPUT_TRANSFORM_NORMAL },
		{ "0", WL_OUTPUT_TRANSFORM_NORMAL },
		{ "90", WL_OUTPUT_TRANSFORM_90 },
		{ "180", WL_OUTPUT_TRANSFORM_180 },
		{ "270", WL_OUTPUT_TRANSFORM_270 },
		{ "flipped", WL_OUTPUT_TRANSFORM_FLIPPED },
		{ "flipped-90", WL_OUTPUT_TRANSFORM_FLIPPED_90 },
		{ "flipped-180", WL_OUTPUT_TRANSFORM_FLIPPED_180 },
		{ "flipped-270", WL_OUTPUT_TRANSFORM_FLIPPED_270 },
	};
	if (text == NULL) {
		return false;
	}
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (strcasecmp(names[i].name, text) == 0) {
			*out = names[i].t;
			return true;
		}
	}
	return false;
}

/* 模式字符串 -> 分辨率 + 刷新率："3840x2160@144" / "3840x2160"（刷新率不挑）/
 * "3840x2160@59.94"。@ 后面的刷新率可以带小数。 */
bool vidow_output_parse_mode(const char *text, int *w, int *h, double *hz) {
	if (text == NULL) {
		return false;
	}
	char *end = NULL;
	long width = strtol(text, &end, 10);
	if (end == text || (*end != 'x' && *end != 'X')) {
		return false;
	}
	const char *rest = end + 1;
	long height = strtol(rest, &end, 10);
	if (end == rest || width <= 0 || height <= 0) {
		return false;
	}
	double refresh = 0.0;
	if (*end == '@') {
		const char *hz_text = end + 1;
		refresh = strtod(hz_text, &end);
		if (end == hz_text || refresh <= 0.0) {
			return false;
		}
	}
	if (*end != '\0') {
		return false; /* "3840x2160x2" 这种不认 */
	}
	*w = (int)width;
	*h = (int)height;
	*hz = refresh;
	return true;
}

/* 在输出报的模式表里找 w×h[+hz]。hz <= 0 = 刷新率不挑（优先那块屏的首选模式）。
 * 找不到返回 NULL（调用方可以退回自定义模式）。 */
static struct wlr_output_mode *output_find_mode(struct wlr_output *wlr_output,
		int w, int h, double hz) {
	struct wlr_output_mode *match = NULL;
	struct wlr_output_mode *mode;
	wl_list_for_each(mode, &wlr_output->modes, link) {
		if (mode->width != w || mode->height != h) {
			continue;
		}
		if (hz > 0.0 && fabs(mode->refresh / 1000.0 - hz) > 0.6) {
			continue;
		}
		match = mode;
		if (mode->preferred) {
			break;
		}
	}
	return match;
}

/* 一个输出该用多大缩放，以及为什么。w / h 是它将要上的那个模式的分辨率
 * （还没上模式时就是现在这个）。 */
static double output_pick_scale(struct vidow_server *server,
		struct wlr_output *wlr_output, int w, int h,
		char *why, size_t why_len) {
	struct vidow_config *cfg = &server->config;

	/* 这块屏自己那一组（vidow.output{ name = … }）里写了 scale 就按它来，
	 * 没写才用全局的 scale。 */
	const struct vidow_output_conf *conf =
		vidow_output_conf_for(server, wlr_output->name);
	struct vidow_scale_spec spec;
	bool named = conf != NULL && conf->has_scale &&
		vidow_scale_parse(conf->scale, &spec);
	if (!named && !vidow_scale_parse(cfg->scale, &spec)) {
		spec.kind = VIDOW_SCALE_AUTO;
	}

	double scale = vidow_scale_resolve(named ? &spec : NULL, cfg->scale, w, h,
		wlr_output->phys_width, wlr_output->phys_height);

	double ppi = 0.0;
	if (wlr_output->phys_width > 0 && wlr_output->phys_height > 0) {
		double diag_px = sqrt((double)w * w + (double)h * h);
		double diag_in = sqrt((double)wlr_output->phys_width *
			wlr_output->phys_width + (double)wlr_output->phys_height *
			wlr_output->phys_height) / 25.4;
		ppi = diag_in > 0.0 ? diag_px / diag_in : 0.0;
	}

	if (spec.kind == VIDOW_SCALE_FIXED) {
		if (named) {
			snprintf(why, why_len, "vidow.output{ %s } scale=%s",
				conf->name, conf->scale);
		} else {
			snprintf(why, why_len, "scale=%s", cfg->scale);
		}
	} else if (spec.kind == VIDOW_SCALE_OFF) {
		snprintf(why, why_len, "xwayland_scale=off（1:1）");
	} else if (ppi > 0.0) {
		snprintf(why, why_len, "auto：%.1fppi / 96", ppi);
	} else {
		snprintf(why, why_len, "auto：没有 EDID 尺寸，按 1:1");
	}
	return scale;
}

/* 这块屏上现在挂着几个窗口（关屏的日志用：说清楚「东西还在，只是不画了」）。 */
static int count_output_views(struct vidow_server *server,
		struct wlr_output *output) {
	int n = 0;
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->output == output) {
			n++;
		}
	}
	return n;
}

/* 这块 wlr_output 对应的 vidow_output（IPC / 运行时改状态用）。 */
static struct vidow_output *output_of(struct vidow_server *server,
		struct wlr_output *wlr_output) {
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->wlr_output == wlr_output) {
			return output;
		}
	}
	return NULL;
}

/* 输出状态（缩放 / 方向 / 模式 / 开关）变过之后要重做的东西：布局按新逻辑尺寸
 * 算、背景缓存按新分辨率重建、光标主题按新缩放取、可用区域重问一次。 */
static void output_state_applied(struct vidow_server *server,
		struct vidow_output *output) {
	wlr_xcursor_manager_load(server->cursor_mgr, output->wlr_output->scale);
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");

	/* 贴图按旧的物理分辨率建的，布局按旧逻辑尺寸算的，都要重来。 */
	vidow_blur_output_finish(output);
	output->backdrop = NULL;
	output->backdrop_w = 0;
	output->backdrop_h = 0;
	wlr_output_layout_get_box(server->output_layout, output->wlr_output,
		&output->usable_area);
	output->box = output->usable_area;
	vidow_backdrop_damage(server, true);
	vidow_arrange_layers(server);
	vidow_arrange(server);
	/* 这块屏的缩放可能变了（IPC output_scale / 配置 SIGHUP）：X 那边的全局缩放
	 * 是「所有屏里最大的那个」，跟着重算一遍 —— 它是 X 屏幕尺寸、RandR 模式和
	 * Xft.dpi 的依据。没变的话这个调用自己就返回了。 */
	vidow_xwayland_scale_apply(server);
	vidow_ipc_emit_output(server, "output-changed", output->wlr_output);
}

/* 缩放变了要重做的东西：散景贴图按输出分辨率缓存、布局按逻辑尺寸算、
 * 光标主题按缩放取大小。 */
void vidow_output_apply_scale(struct vidow_server *server,
		struct vidow_output *output) {
	struct wlr_output *wlr_output = output->wlr_output;
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	int w = mode != NULL ? mode->width : wlr_output->width;
	int h = mode != NULL ? mode->height : wlr_output->height;
	char why[64];
	double scale = output_pick_scale(server, wlr_output, w, h, why, sizeof(why));
	if (fabs(scale - (double)wlr_output->scale) < 1e-3) {
		return;
	}
	vidow_output_set_scale(server, wlr_output, scale, why);
}

/* SIGHUP / config.lua 改完 scale/outputs 之后调这个。 */
void vidow_output_apply_scale_all(struct vidow_server *server) {
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		vidow_output_apply_scale(server, output);
	}
}

/* ------------------------------------------- 运行时改输出（IPC 用）

 * 每一件都会 commit 一次，然后走 output_state_applied() 把跟着变的东西重做一遍，
 * 最后推一个 output-changed 事件。失败（后端不认）返回 false，调用者去报错。 */

bool vidow_output_set_scale(struct vidow_server *server,
		struct wlr_output *wlr_output, double scale, const char *why) {
	struct vidow_output *output = output_of(server, wlr_output);
	if (output == NULL) {
		return false;
	}
	double before = wlr_output->scale;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_scale(&state, (float)scale);
	bool ok = wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		wlr_log(WLR_ERROR, "output %s: 设置缩放 %.2f 失败", wlr_output->name,
			scale);
		return false;
	}
	/* 和「上屏那一刻」用同一套字段：日志里每块屏的最后一行永远是这个屏现在的
	 * 样子（配置是 Lua、跑得比输出创建晚，所以运行时换缩放是常态 —— 只打
	 * 「24 -> 2.00」的话，从日志里看不出它现在到底是几倍、逻辑尺寸多少）。 */
	wlr_log(WLR_INFO, "output %s: %dx%d @ %.3fHz scale %.2f（%s），逻辑 %dx%d，"
		"布局 %d,%d（缩放 %.2f -> %.2f）",
		wlr_output->name, wlr_output->width, wlr_output->height,
		(float)wlr_output->refresh / 1000.0f, wlr_output->scale,
		why != NULL ? why : "运行时改的",
		(int)lround(wlr_output->width / wlr_output->scale),
		(int)lround(wlr_output->height / wlr_output->scale),
		output->box.x, output->box.y, before, scale);
	output_state_applied(server, output);
	return true;
}

bool vidow_output_set_transform(struct vidow_server *server,
		struct wlr_output *wlr_output, enum wl_output_transform transform) {
	struct vidow_output *output = output_of(server, wlr_output);
	if (output == NULL) {
		return false;
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_transform(&state, transform);
	bool ok = wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		wlr_log(WLR_ERROR, "output %s: 设置方向 %d 失败", wlr_output->name,
			(int)transform);
		return false;
	}
	wlr_log(WLR_INFO, "output %s: 方向 -> %d（逻辑尺寸 %dx%d）", wlr_output->name,
		(int)transform, (int)lround(wlr_output->width / wlr_output->scale),
		(int)lround(wlr_output->height / wlr_output->scale));
	output_state_applied(server, output);
	return true;
}

/* 开关显示器。关掉 = 这块屏不再出画面（DRM 那边就是关 CRTC），上面的窗口都留着：
 * 位置、焦点、工作区都不动，屏回来（或者打开）就照原样画回去。 */
bool vidow_output_set_enabled(struct vidow_server *server,
		struct wlr_output *wlr_output, bool enabled) {
	struct vidow_output *output = output_of(server, wlr_output);
	if (output == NULL) {
		return false;
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, enabled);
	bool ok = wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		wlr_log(WLR_ERROR, "output %s: %s 失败", wlr_output->name,
			enabled ? "打开" : "关掉");
		return false;
	}
	wlr_log(WLR_INFO, "output %s: %s（%dx%d，缩放 %.2f，上面还有 %d 个窗口留着）",
		wlr_output->name, enabled ? "打开" : "关掉", wlr_output->width,
		wlr_output->height, (double)wlr_output->scale,
		count_output_views(server, wlr_output));
	if (!enabled && server->focused_output == wlr_output) {
		/* 聚焦的屏关掉了：焦点挪到还开着的一块屏上，别对着黑屏打字。 */
		struct vidow_output *other;
		wl_list_for_each(other, &server->outputs, link) {
			if (other != output && other->wlr_output->enabled) {
				server->focused_output = other->wlr_output;
				wlr_log(WLR_INFO, "output %s: 焦点挪到 %s",
					wlr_output->name, other->wlr_output->name);
				break;
			}
		}
	}
	output_state_applied(server, output);
	return true;
}

/* VRR（adaptive sync）。这块屏 / 这个后端不支持就只报一句：headless 后端在
 * wlroots 里根本不吃这个字段（提交会被拒），DRM 那边看连接器的 vrr_capable。 */
bool vidow_output_set_vrr(struct vidow_server *server,
		struct wlr_output *wlr_output, bool enabled) {
	struct vidow_output *output = output_of(server, wlr_output);
	if (output == NULL) {
		return false;
	}
	bool now = wlr_output->adaptive_sync_status !=
		WLR_OUTPUT_ADAPTIVE_SYNC_DISABLED;
	if (now == enabled) {
		return true;
	}
	if (!wlr_output->adaptive_sync_supported) {
		wlr_log(WLR_INFO, "output %s: 这块屏 / 这个后端不支持 VRR"
			"（adaptive sync），%s不生效", wlr_output->name,
			enabled ? "开" : "关");
		return false;
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_adaptive_sync_enabled(&state, enabled);
	bool ok = wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		wlr_log(WLR_INFO, "output %s: VRR %s 失败（后端拒了）", wlr_output->name,
			enabled ? "开" : "关");
		return false;
	}
	wlr_log(WLR_INFO, "output %s: VRR -> %s（现在 %s）", wlr_output->name,
		enabled ? "开" : "关",
		wlr_output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED ?
		"生效" : "后端没接");
	return true;
}

/* 渲染位深：10 位 = DRM_FORMAT_XRGB2101010 的输出缓冲。
 *
 * 「支不支持」不猜：先看后端报的格式表（能问就问），再直接提交试一次（后端
 * 没报格式表的时候只能这样）。失败就保持 8 位，日志里说清楚 —— 10 位要显卡、
 * 显示器、渲染器三样都到位，缺一样就只剩「本来就不生效」。 */
bool vidow_output_set_depth(struct vidow_server *server,
		struct wlr_output *wlr_output, int depth) {
	struct vidow_output *output = output_of(server, wlr_output);
	if (output == NULL) {
		return false;
	}
	uint32_t want = depth >= 10 ? DRM_FORMAT_XRGB2101010 : DRM_FORMAT_XRGB8888;
	if (wlr_output->render_format == want) {
		return true;
	}
	const struct wlr_drm_format_set *formats = wlr_output_get_primary_formats(
		wlr_output, server->allocator != NULL ?
		server->allocator->buffer_caps : WLR_BUFFER_CAP_DATA_PTR);
	/* 格式表里有这个格式就行，modifier 不挑（wlr_drm_format_set_has 是「一模一样的
	 * modifier」，拿 MOD_INVALID 去问会永远说不支持）。 */
	if (formats != NULL && formats->len > 0 &&
			wlr_drm_format_set_get(formats, want) == NULL) {
		wlr_log(WLR_INFO, "output %s: 这块屏不支持 %d 位色（输出缓冲没有 %s），"
			"保持 %d 位", wlr_output->name, depth,
			depth >= 10 ? "XRGB2101010" : "XRGB8888",
			wlr_output->render_format == DRM_FORMAT_XRGB2101010 ? 10 : 8);
		return false;
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_render_format(&state, want);
	bool ok = wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		wlr_log(WLR_INFO, "output %s: 设成 %d 位色失败（后端 / 渲染器不支持），"
			"保持 %d 位", wlr_output->name, depth,
			wlr_output->render_format == DRM_FORMAT_XRGB2101010 ? 10 : 8);
		return false;
	}
	wlr_log(WLR_INFO, "output %s: 输出缓冲 -> %s（%d 位色）", wlr_output->name,
		depth >= 10 ? "XRGB2101010" : "XRGB8888", depth);
	output_state_applied(server, output);
	return true;
}

bool vidow_output_set_mode(struct vidow_server *server,
		struct wlr_output *wlr_output, int width, int height,
		double refresh_hz) {
	struct vidow_output *output = output_of(server, wlr_output);
	if (output == NULL) {
		return false;
	}
	struct wlr_output_mode *match = output_find_mode(wlr_output, width, height,
		refresh_hz);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (match != NULL) {
		wlr_output_state_set_mode(&state, match);
	} else {
		/* 没这个模式：按自定义模式试一把（有些后端支持，有些会拒绝）。 */
		wlr_output_state_set_custom_mode(&state, width, height,
			refresh_hz > 0.0 ? (int32_t)(refresh_hz * 1000.0) : 0);
	}
	bool ok = wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		wlr_log(WLR_ERROR, "output %s: 设成 %dx%d@%.2f 失败", wlr_output->name,
			width, height, refresh_hz);
		return false;
	}
	wlr_log(WLR_INFO, "output %s: 模式 -> %dx%d @ %.2fHz（%s）", wlr_output->name,
		wlr_output->width, wlr_output->height, wlr_output->refresh / 1000.0,
		match != NULL ? "列表里的" : "自定义");
	output_state_applied(server, output);
	return true;
}

bool vidow_output_set_position(struct vidow_server *server,
		struct wlr_output *wlr_output, int x, int y) {
	struct vidow_output *output = output_of(server, wlr_output);
	if (output == NULL) {
		return false;
	}
	struct wlr_output_layout_output *layout_output =
		wlr_output_layout_add(server->output_layout, wlr_output, x, y);
	if (layout_output == NULL) {
		return false;
	}
	output->layout_output = layout_output;
	wlr_log(WLR_INFO, "output %s: 布局位置 -> %d,%d", wlr_output->name, x, y);
	output_state_applied(server, output);
	return true;
}

/* 配置里给这块屏指定的布局位置（vidow.output{ name = …, x = 0, y = 0 }，逻辑
 * 坐标）。没写 / 没点名就返回 false = 不摆，按后端给的顺序自动排。 */
static bool config_position_for(struct vidow_server *server, const char *name,
		int *x, int *y) {
	const struct vidow_output_conf *conf =
		vidow_output_conf_for(server, name);
	if (conf == NULL || !conf->has_pos) {
		return false;
	}
	*x = conf->x;
	*y = conf->y;
	return true;
}

/* 配置里那块屏的位置改过（SIGHUP / 配置刚跑完）：按新配置把每块屏再摆一次。
 * 位置没变的什么都不做（vidow_output_set_position 会把布局、背景、光标重做一遍，
 * 白做一次没必要）。 */
void vidow_output_apply_positions_all(struct vidow_server *server) {
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		int x = 0, y = 0;
		if (!config_position_for(server, output->wlr_output->name, &x, &y)) {
			continue;
		}
		if (output->box.x == x && output->box.y == y) {
			continue;
		}
		wlr_log(WLR_INFO, "output %s: 按配置（vidow.output）摆到 %d,%d"
			"（现在 %d,%d）", output->wlr_output->name, x, y, output->box.x,
			output->box.y);
		vidow_output_set_position(server, output->wlr_output, x, y);
	}
}

/* 配置里除了缩放 / 位置之外的几项（模式、方向、开关、VRR、位深）落到这块屏上。
 *
 * 每一项单独 commit：VRR / 位深这类「后端可能不认」的东西要是和开关屏、上模式
 * 挤在同一次 commit 里，一个不认就整次失败 —— 屏直接不亮，而原因只是一个可选
 * 项。分开做，谁不认就谁不生效，日志里点名。
 *
 * create = true 时只做「建屏那次 commit 没做的事」：模式 / 方向 / 开关已经在
 * 初始 state 里了，这里只补 VRR 和位深（它们不能在关着的屏上设，也不适合和
 * 上模式挤在一起）。 */
static void output_apply_conf_extra(struct vidow_server *server,
		struct vidow_output *output) {
	struct wlr_output *wlr_output = output->wlr_output;
	if (!wlr_output->enabled) {
		return; /* 关着的屏不能设 VRR / 位深（wlroots 会直接拒） */
	}
	const struct vidow_output_conf *conf =
		vidow_output_conf_for(server, wlr_output->name);
	if (conf == NULL) {
		return;
	}
	if (conf->has_vrr) {
		vidow_output_set_vrr(server, wlr_output, conf->vrr);
	}
	if (conf->has_depth) {
		vidow_output_set_depth(server, wlr_output, conf->depth);
	}
}

/* 配置里那块屏的开关 / 模式 / 方向改过（SIGHUP、配置刚跑完）：按新配置对一遍账。
 * 每一项都先比一遍现状，没变的什么都不做（每次 set_xxx 都会重排、重画背景）。 */
void vidow_output_apply_state_all(struct vidow_server *server) {
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_output *wlr_output = output->wlr_output;
		const struct vidow_output_conf *conf =
			vidow_output_conf_for(server, wlr_output->name);
		if (conf == NULL) {
			continue;
		}
		if (conf->has_enabled && wlr_output->enabled != conf->enabled) {
			wlr_log(WLR_INFO, "output %s: 按配置（vidow.output）%s",
				wlr_output->name, conf->enabled ? "打开" : "关掉");
			vidow_output_set_enabled(server, wlr_output, conf->enabled);
		}
		if (conf->has_mode && wlr_output->enabled &&
				((int)wlr_output->width != conf->mode_w ||
				 (int)wlr_output->height != conf->mode_h ||
				 (conf->mode_hz > 0.0 &&
				  fabs(wlr_output->refresh / 1000.0 - conf->mode_hz) > 0.6))) {
			wlr_log(WLR_INFO, "output %s: 按配置（vidow.output）换模式",
				wlr_output->name);
			vidow_output_set_mode(server, wlr_output, conf->mode_w,
				conf->mode_h, conf->mode_hz);
		}
		if (conf->has_transform && wlr_output->transform != conf->transform) {
			vidow_output_set_transform(server, wlr_output, conf->transform);
		}
		output_apply_conf_extra(server, output);
	}
}

/* 一块屏的「记忆」：热插拔按输出名认人（详见 hotplug.h）。 */
static struct vidow_output_memory *output_memory(struct vidow_server *server,
		struct wlr_output *wlr_output) {
	if (wlr_output == NULL) {
		return NULL;
	}
	return vidow_output_memory_for(&server->output_memories, wlr_output->name);
}

/* 这块屏拔了，它的东西挪到哪块屏上：优先左边最近的那块（视觉上接着原来的位置），
 * 没有就取右边最近的，一块活的都没有就是 NULL（东西先悬着，等有屏了再安家）。 */
static struct vidow_output *output_pick_fallback(struct vidow_server *server,
		const struct wlr_box *dead_box) {
	size_t n = 0;
	struct vidow_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		n++;
	}
	if (n == 0) {
		return NULL;
	}

	struct vidow_hotplug_slot *slots = calloc(n, sizeof(*slots));
	struct vidow_output **outs = calloc(n, sizeof(*outs));
	if (slots == NULL || outs == NULL) {
		free(slots);
		free(outs);
		return NULL;
	}
	size_t i = 0;
	wl_list_for_each(o, &server->outputs, link) {
		/* 现问布局：拔线会让剩下的屏重新摆放，缓存的 box 可能还没更新 */
		struct wlr_box box = {0};
		wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
		slots[i].live = true;
		slots[i].x = box.x;
		outs[i] = o;
		i++;
	}
	size_t idx = vidow_hotplug_pick_fallback(slots, i, dead_box->x);
	struct vidow_output *picked = idx == SIZE_MAX ? NULL : outs[idx];
	free(slots);
	free(outs);
	return picked;
}

/* 一块屏走了（真拔线，或者测试钩子里的 wlr_output_destroy）。在 free(output)
 * 之前调用：记忆要记下它离开时的视口 / 位置 / 焦点，而 wlr_output 还是活的。 */
static struct vidow_output *output_gone(struct vidow_server *server,
		struct vidow_output *output) {
	/* Alt+Tab 切换器开着、而且它就摆在这块屏上：先关掉 —— 它攥着
	 * wlr_output 指针，再往下走这块就要被释放了。 */
	if (server->mru.output == output->wlr_output) {
		vidow_mru_cancel(server);
	}
	/* 名字在 add 的时候就存进 memory 了：到这儿 wlr_output->name 已经被
	 * wlroots 收走，按名字现找会拿到空串（这块踩过，热插拔整段不生效）。 */
	struct vidow_output_memory *memory = output->memory;
	struct vidow_output *fallback = output_pick_fallback(server, &output->box);
	if (memory == NULL) {
		/* 连记忆都没有（比如认不出名字的输出）：至少别让谁指着已经释放的
		 * wlr_output —— 照样把东西挪走。 */
		vidow_view_output_gone(server, NULL, output->wlr_output, fallback);
		vidow_layer_output_gone(server, NULL, output->wlr_output, fallback);
		return fallback;
	}

	memory->present = false;
	memory->output = NULL;
	memory->last_box = vidow_hotplug_box(output->box.x, output->box.y,
		output->box.width, output->box.height);
	memory->viewport_x = output->viewport_x;
	memory->focused = (server->focused != NULL &&
		server->focused->output == output->wlr_output) ? server->focused : NULL;

	/* 列 / 浮动窗口 / layer surface 的「住处」改成 fallback，「归属」留在记忆里。 */
	vidow_view_output_gone(server, memory, output->wlr_output, fallback);
	vidow_layer_output_gone(server, memory, output->wlr_output, fallback);
	return fallback;
}

static void output_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_output *output = wl_container_of(listener, output, destroy);
	struct vidow_server *server = output->server;

	if (output->pump_timer != NULL) {
		wl_event_source_remove(output->pump_timer);
		output->pump_timer = NULL;
	}
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);

	vidow_blur_output_finish(output);

	/* 先把这块屏上的东西安置好，再放它走：任何 view->output / col->output /
	 * ls->output 都不能指着已经被释放的结构（以前这里会留下野指针）。 */
	struct vidow_output *fallback = output_gone(server, output);

	if (server->focused_output == output->wlr_output) {
		server->focused_output = fallback != NULL ? fallback->wlr_output : NULL;
		if (server->focused_output == NULL) {
			struct vidow_output *next;
			wl_list_for_each(next, &server->outputs, link) {
				server->focused_output = next->wlr_output;
				break;
			}
		}
	}

	/* 事件要带名字，而 wlr_output 在这个信号之后就被释放了 —— 所以在这里发。 */
	vidow_ipc_emit_output(server, "output-removed", output->wlr_output);
	free(output);
	vidow_arrange(server);
	/* 最密的那块屏拔了：X 的全局缩放可以降下来。 */
	vidow_xwayland_scale_apply(server);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	struct vidow_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	if (!wlr_output_init_render(wlr_output, server->allocator, server->renderer)) {
		wlr_log(WLR_ERROR, "failed to init render for output %s", wlr_output->name);
		return;
	}

	/* 配置里这块屏那一组（vidow.output{…}）：开关 / 模式 / 方向都进第一次
	 * commit —— 一起上屏，客户端看到的第一个尺寸就是对的。VRR 和位深不行
	 * （不能在关着的屏上设，也不该和上模式挤同一次 commit），commit 之后由
	 * output_apply_conf_extra() 单独补。 */
	const struct vidow_output_conf *conf =
		vidow_output_conf_for(server, wlr_output->name);
	bool want_enabled = conf != NULL && conf->has_enabled ? conf->enabled : true;

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, want_enabled);
	struct wlr_output_mode *mode = NULL;
	int mode_w = wlr_output->width;
	int mode_h = wlr_output->height;
	if (conf != NULL && conf->has_mode) {
		mode_w = conf->mode_w;
		mode_h = conf->mode_h;
		mode = output_find_mode(wlr_output, mode_w, mode_h, conf->mode_hz);
		if (mode != NULL) {
			wlr_output_state_set_mode(&state, mode);
		} else {
			/* 模式表里没有：按自定义模式试一把（有些后端支持，有些会拒）。 */
			wlr_output_state_set_custom_mode(&state, mode_w, mode_h,
				conf->mode_hz > 0.0 ? (int32_t)(conf->mode_hz * 1000.0) : 0);
			wlr_log(WLR_INFO, "output %s: 模式表里没有 %dx%d%s，按自定义模式试",
				wlr_output->name, mode_w, mode_h,
				conf->mode_hz > 0.0 ? "（带刷新率）" : "");
		}
	} else {
		mode = wlr_output_preferred_mode(wlr_output);
		if (mode != NULL) {
			mode_w = mode->width;
			mode_h = mode->height;
			wlr_output_state_set_mode(&state, mode);
		}
	}
	if (conf != NULL && conf->has_transform) {
		wlr_output_state_set_transform(&state, conf->transform);
	}
	/* 缩放要在第一次 commit 之前定下来：它决定逻辑尺寸（布局坐标的基准），
	 * 也决定客户端拿到的 preferred_buffer_scale。 */
	char why[64];
	double scale = output_pick_scale(server, wlr_output, mode_w, mode_h, why,
		sizeof(why));
	wlr_output_state_set_scale(&state, (float)scale);
	if (!wlr_output_commit_state(wlr_output, &state)) {
		wlr_log(WLR_ERROR, "output %s: 第一次上屏失败（配置里那块屏的模式 / "
			"方向后端不认？）", wlr_output->name);
	}
	wlr_output_state_finish(&state);

	struct vidow_output *output = calloc(1, sizeof(*output));
	if (output == NULL) {
		return;
	}
	output->server = server;
	output->wlr_output = wlr_output;
	/* 帧追踪的基准：第一次出帧之前 last_frame_ns 是 0，而 vidow_now_ns() 是开机
	 * 以来的纳秒数 —— 不初始化的话第一帧会被算成「停了 3.8 小时」（实测）。 */
	output->last_frame_ns = vidow_now_ns();
	/* 看门狗的限速基准同理（它比的是「上一次真出帧」，不是「后端上一次给帧」）。 */
	output->last_render_ns = output->last_frame_ns;
	output->scene_output = wlr_scene_output_create(server->scene, wlr_output);
	/* 配置里点了名的屏摆在它说的位置；没点名就按到达顺序自动排。
	 * 「哪块在左边」不能靠自动排 —— 那个顺序是后端给的（headless 上实测是
	 * 反的），所以确定布局必须写在配置里（vidow.output{ name = …, x = … }）。 */
	int pos_x = 0, pos_y = 0;
	bool placed = config_position_for(server, wlr_output->name, &pos_x, &pos_y);
	if (placed) {
		output->layout_output = wlr_output_layout_add(server->output_layout,
			wlr_output, pos_x, pos_y);
		if (output->layout_output == NULL) {
			wlr_log(WLR_ERROR, "output %s: 摆到 %d,%d 失败，改成自动排",
				wlr_output->name, pos_x, pos_y);
			placed = false;
		}
	}
	if (!placed) {
		output->layout_output =
			wlr_output_layout_add_auto(server->output_layout, wlr_output);
	}
	wlr_output_layout_get_box(server->output_layout, wlr_output, &output->box);
	output->memory = output_memory(server, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout,
		output->layout_output, output->scene_output);

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	/* 视口外补帧的到点闹钟（见 view.c 的 vidow_view_render_pump） */
	output->pump_timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server->display), pump_timer_cb, output);

	wl_list_insert(&server->outputs, &output->link);

	wlr_xcursor_manager_load(server->cursor_mgr, wlr_output->scale);
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");

	/* 测试钩子：把这块屏钉死在软光标上。真机上「放大图超出硬件光标平面」也是
	 * 这条路，而 headless 后端的假光标平面什么都收（wlr_output_is_headless()
	 * 认的就是 impl 指针，没法拿它做假）—— 没有这个钩子就测不到「抓帧让位」
	 * （见 cursorfx.c）。 */
	if (getenv("VIDOW_TEST_SOFT_CURSOR") != NULL) {
		wlr_output_lock_software_cursors(wlr_output, true);
	}

	if (server->focused_output == NULL) {
		server->focused_output = wlr_output;
	}

	/* Usable area starts as the whole output; layer exclusive zones shrink it. */
	wlr_output_layout_get_box(server->output_layout, wlr_output,
		&output->usable_area);
	output->box = output->usable_area;
	vidow_arrange_layers(server);

	/* 布局原点也打出来：多屏 / 热插拔时「谁在左边」直接看日志，测试也不用猜。 */
	wlr_log(WLR_INFO, "output %s: %dx%d @ %.3fHz scale %.2f（%s），逻辑 %dx%d，"
		"布局 %d,%d（%s）",
		wlr_output->name, wlr_output->width, wlr_output->height,
		(float)wlr_output->refresh / 1000.0f, wlr_output->scale, why,
		(int)lround(wlr_output->width / wlr_output->scale),
		(int)lround(wlr_output->height / wlr_output->scale),
		output->box.x, output->box.y,
		placed ? "配置指定（vidow.output）" : "自动排");

	/* 热插拔：这块屏以前见过的话，把归属它的列 / 窗口 / layer surface 认领回来
	 * （列宽、滚动位置、焦点都留着）。它自己会 arrange。 */
	struct vidow_output_memory *memory = output->memory;
	if (memory != NULL) {
		bool seen_before = memory->last_box.width > 0;
		memory->present = true;
		memory->output = wlr_output;
		vidow_view_output_added(server, memory, output);
		vidow_layer_output_added(server, memory, output);
		memory->last_box = vidow_hotplug_box(output->box.x, output->box.y,
		output->box.width, output->box.height);
		if (!seen_before) {
			wlr_log(WLR_INFO, "hotplug: %s 是一块新屏（记忆里没有），"
				"列按到达顺序排", memory->key);
		}
	}

	vidow_arrange(server);

	/* 配置里那块屏还写了 VRR / 位深的话，现在补上（不能在关着的屏上设，也不该
	 * 和上模式挤同一次 commit）。 */
	output_apply_conf_extra(server, output);

	/* 新屏可能比现有的都密：X 的全局缩放要跟着涨（涨了已存在的 X11 窗口会
	 * 重新声明缓冲区比例）。 */
	vidow_xwayland_scale_apply(server);

	vidow_ipc_emit_output(server, "output-added", wlr_output);
}


/* 测试钩子专用：把一块已经加进来的输出「重新认一次身份」。
 *
 * headless 后端的输出名是自增的（HEADLESS-1、2、3…），拔了再插会变成新名字；
 * 真实机器上连接器名是稳定的，所以产品路径不需要这个 —— 测试要验「同一块屏插回来
 * 还原工作区」，只能在加完之后把名字改回原来那个，再重新认一遍归属。
 * 只有 VIDOW_TEST_OUTPUTS 那段代码会调它。 */
void vidow_hotplug_reattach(struct vidow_server *server,
		struct wlr_output *wlr_output) {
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->wlr_output == wlr_output) {
			break;
		}
	}
	if (output == NULL || output->wlr_output != wlr_output) {
		return;
	}
	struct vidow_output_memory *memory = output_memory(server, wlr_output);
	if (memory == NULL || output->memory == memory) {
		return;
	}
	output->memory = memory;
	memory->present = true;
	memory->output = wlr_output;
	vidow_view_output_added(server, memory, output);
	vidow_layer_output_added(server, memory, output);
	memory->last_box = vidow_hotplug_box(output->box.x, output->box.y,
		output->box.width, output->box.height);
}

/* ---------------------------------------------- 测试钩子（不是产品功能）

 * VIDOW_TEST_OUTPUTS="600:remove:HEADLESS-2,1500:add:1280x720"
 * 在启动后的指定毫秒真拔 / 真插一块输出：走的是 backend 的真实事件路径
 * （wlr_output_destroy / wlr_headless_add_output），不是模拟状态 —— 用来在没有
 * 真显示器可拔的环境里验热插拔（见 test-hotplug.sh）。只有 headless 后端能凭空
 * 加输出，别的后端会拒绝并打一行日志。不设这个变量时这段代码不参与。 */

struct test_output_step {
	int at_ms;
	bool add;
	char arg[64]; /* remove: 输出名；add: WxH[:名字] */
	bool done;
	struct test_output_step *next;
};

static struct test_output_step *test_output_steps;
static struct wl_event_source *test_output_timer;
static int64_t test_output_start_ms;      /* 装钩子那一刻，步骤时间是相对它的 */
static struct wlr_backend *test_headless; /* 认出来的 headless 后端（加输出要用） */
static char test_last_removed[VIDOW_OUTPUT_KEY_MAX]; /* 上次拔掉的那块，给 add:last 用 */

static void test_output_free(void) {
	while (test_output_steps != NULL) {
		struct test_output_step *next = test_output_steps->next;
		free(test_output_steps);
		test_output_steps = next;
	}
}

/* name 可以是输出名，也可以是 "left" / "right" —— 按布局位置挑一块，测试脚本就不用
 * 猜 headless 后端先创建的是哪一块。 */
static struct vidow_output *test_pick_output(struct vidow_server *server,
		const char *name) {
	if (strcmp(name, "left") == 0 || strcmp(name, "right") == 0) {
		bool want_left = name[0] == 'l';
		struct vidow_output *best = NULL;
		struct vidow_output *o;
		wl_list_for_each(o, &server->outputs, link) {
			if (best == NULL ||
					(want_left ? o->box.x < best->box.x : o->box.x > best->box.x)) {
				best = o;
			}
		}
		return best;
	}
	struct vidow_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (strcmp(o->wlr_output->name, name) == 0) {
			return o;
		}
	}
	return NULL;
}

static void test_output_remove(struct vidow_server *server, const char *name) {
	struct vidow_output *o = test_pick_output(server, name);
	{
		if (o == NULL) {
			wlr_log(WLR_ERROR, "test: 没有叫 %s 的输出", name);
			return;
		}
		snprintf(test_last_removed, sizeof(test_last_removed), "%s",
			o->wlr_output->name);
		{
			const char *target = o->wlr_output->name;
			wlr_log(WLR_INFO, "test: 拔掉输出 %s（%s）", target, name);
			/* 和真拔线一模一样的一条路：backend 释放 wlr_output，我们收到
			 * events.destroy -> output_destroy。 */
			wlr_output_destroy(o->wlr_output);
		}
	}
}

/* size 可以是 "1280x720"，也可以点名 "1280x720:HEADLESS-2"。
 * 点名这个功能是给测试用的：headless 后端的输出名是自增的（HEADLESS-1、2、3…），
 * 拔了再插会变成 HEADLESS-3，那就永远验不到「插回来还原工作区」—— 真机器上连接器名
 * 是稳定的，所以这里把名字改成指定的那个，模拟「同一根线又插上了」。
 * （wlr_output.name 是 malloc 出来的字符串，wlroots 自己用 free() 释放，strdup 兼容。） */
static void test_output_add(struct vidow_server *server, const char *spec) {
	int w = 1280, h = 720;
	char size[64] = "";
	const char *name = NULL;
	const char *colon = strrchr(spec, ':');
	if (colon != NULL) {
		/* "add:1280x720:last" = 用上次拔掉那块屏的名字（模拟同一根线又插上了） */
		if (strcmp(colon + 1, "last") == 0 && test_last_removed[0] != '\0') {
			name = test_last_removed;
			colon = NULL;
		}
	}
	if (colon != NULL) {
		snprintf(size, sizeof(size), "%.*s", (int)(colon - spec), spec);
		name = colon + 1;
	} else {
		snprintf(size, sizeof(size), "%s", spec);
	}
	if (sscanf(size, "%dx%d", &w, &h) != 2 || w < 1 || h < 1) {
		w = 1280;
		h = 720;
	}
	/* WLR_BACKENDS=headless 时 server->backend 是个 multi backend，
	 * wlr_backend_is_headless() 对它返回 false —— 从现有输出身上认更直接。 */
	if (test_headless == NULL) {
		struct vidow_output *o;
		wl_list_for_each(o, &server->outputs, link) {
			if (wlr_output_is_headless(o->wlr_output)) {
				test_headless = o->wlr_output->backend;
				break;
			}
		}
	}
	if (test_headless == NULL) {
		wlr_log(WLR_ERROR, "test: 边上没有 headless 输出，加不出来，跳过");
		return;
	}
	wlr_log(WLR_INFO, "test: 插上 %dx%d 的 headless 输出%s%s", w, h,
		name != NULL ? "，名字改成 " : "", name != NULL ? name : "");
	struct wlr_output *out = wlr_headless_add_output(test_headless,
		(unsigned)w, (unsigned)h);
	if (out != NULL && name != NULL && name[0] != '\0') {
		free(out->name);
		out->name = strdup(name);
		/* 名字是加进来之后才改的（headless 后端的 new_output 是同步发的），
		 * 所以改完要重新认一次身份 —— 见 vidow_hotplug_reattach。 */
		vidow_hotplug_reattach(server, out);
	}
}

static int test_output_tick(void *data) {
	struct vidow_server *server = data;
	/* 步骤里的毫秒是「启动之后多久」，所以要减掉装钩子那一刻的单调时钟
	 * （vidow_now_ns() 是 CLOCK_MONOTONIC，直接拿它比会全部当成过期）。 */
	int64_t now_ms = vidow_now_ns() / 1000000 - test_output_start_ms;
	int left = 0;
	struct test_output_step *step;

	for (step = test_output_steps; step != NULL; step = step->next) {
		if (step->done) {
			continue;
		}
		if (step->at_ms > now_ms) {
			left++;
			continue;
		}
		step->done = true;
		if (step->add) {
			test_output_add(server, step->arg);
		} else {
			test_output_remove(server, step->arg);
		}
	}

	if (left > 0) {
		wl_event_source_timer_update(test_output_timer, 25);
	} else {
		test_output_free();
		test_output_timer = NULL;
	}
	return 0;
}

/* "600:remove:HEADLESS-2" / "1500:add:1280x720"，逗号分隔。 */
static bool test_output_parse(const char *spec, int *at_ms, bool *add,
		char *arg, size_t arg_len) {
	const char *colon1 = strchr(spec, ':');
	if (colon1 == NULL) {
		return false;
	}
	const char *colon2 = strchr(colon1 + 1, ':');
	if (colon2 == NULL) {
		return false;
	}
	size_t action_len = (size_t)(colon2 - colon1 - 1);
	const char *value = colon2 + 1;
	const char *end = strchr(value, ',');
	size_t value_len = end != NULL ? (size_t)(end - value) : strlen(value);

	*at_ms = atoi(spec);
	*add = action_len == 3 && strncmp(colon1 + 1, "add", 3) == 0;
	snprintf(arg, arg_len, "%.*s", (int)value_len, value);
	return true;
}

static void vidow_output_test_hook(struct vidow_server *server) {
	const char *spec = getenv("VIDOW_TEST_OUTPUTS");
	if (spec == NULL || spec[0] == '\0') {
		return;
	}

	int count = 0;
	const char *p = spec;
	while (p != NULL && *p != '\0') {
		int at_ms = 0;
		bool add = false;
		char arg[64] = "";
		if (!test_output_parse(p, &at_ms, &add, arg, sizeof(arg))) {
			break;
		}
		struct test_output_step *step = calloc(1, sizeof(*step));
		if (step == NULL) {
			break;
		}
		step->at_ms = at_ms;
		step->add = add;
		memcpy(step->arg, arg, sizeof(step->arg));
		step->next = test_output_steps;
		test_output_steps = step;
		count++;

		p = strchr(p, ',');
		if (p != NULL) {
			p++;
		}
	}

	if (count == 0) {
		wlr_log(WLR_ERROR, "test: VIDOW_TEST_OUTPUTS 里没有能识别的步骤");
		test_output_free();
		return;
	}
	test_output_start_ms = vidow_now_ns() / 1000000;
	test_output_timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server->display), test_output_tick, server);
	if (test_output_timer == NULL) {
		test_output_free();
		return;
	}
	wl_event_source_timer_update(test_output_timer, 25);
	wlr_log(WLR_INFO, "test: 输出热插拔钩子装上了（%d 步）", count);
}

bool vidow_output_init(struct vidow_server *server) {
	server->anim_timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server->display), anim_watchdog, server);
	if (server->anim_timer == NULL) {
		wlr_log(WLR_ERROR, "failed to create the animation watchdog");
	}
	server->capture_timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server->display), capture_watchdog, server);
	if (server->capture_timer == NULL) {
		wlr_log(WLR_ERROR, "failed to create the capture watchdog");
	}
	/* 这里**不**排第一次：没人在抓帧的时候它一次都不该醒（见 capture_watchdog，
	 * 由 vidow_output_capture_kick 按需叫起来）。 */
	server->kick_timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server->display), kick_timer_cb, server);
	if (server->kick_timer == NULL) {
		wlr_log(WLR_ERROR, "failed to create the resume kick timer");
	}
	if (getenv("VIDOW_TRACE_FRAMES") != NULL) {
		server->trace_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server->display), trace_timer_cb, server);
		if (server->trace_timer == NULL) {
			wlr_log(WLR_ERROR, "failed to create the frame trace timer");
		} else {
			wl_event_source_timer_update(server->trace_timer, 250);
			wlr_log(WLR_INFO, "帧追踪: 开（VIDOW_TRACE_FRAMES=1）—— 要了帧"
				"却 300ms 没出帧会报 WLR_ERROR");
		}
	}
	server->new_output.notify = server_new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);
	vidow_output_test_hook(server);
	return true;
}
