#include "server.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>

#include "ipc.h"

#include <wlr/backend/headless.h>
#include <wlr/util/log.h>

/* Advance the animation clock and present one frame. */
void vidow_output_render(struct vidow_output *output) {
	struct vidow_server *server = output->server;

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

	/* overview 开着的话先把变换算好、把窗口按它摆一遍（缩放里每帧都要摆，
	 * 因为客户端 commit 会把场景尺寸改回去）。 */
	vidow_overview_update(server);

	/* Capture what is behind the blurred windows and run the kawase chain
	 * before anything is drawn, so this frame's decorations can sample it. */
	vidow_blur_update(output);

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
	vidow_anim_tick(server);

	wlr_scene_output_commit(output->scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(output->scene_output, &now);

	/* 视口外的窗口要不要保活（窗口规则 / 录屏预览）。放在场景补帧之后：已经画在
	 * 画面上的窗口场景那边已经给过回调了，这里只补没画到的那些。 */
	vidow_view_render_pump(server, output,
		(uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000));
}

/* 补帧闹钟到点：催一帧出来（渲染里会走到 vidow_view_render_pump，到点的窗口就补上）。
 * 只在这里催，不在「还没到点」的时候催 —— 那样会变成每帧都渲染却什么都不补的空转。 */
static int pump_timer_cb(void *data) {
	struct vidow_output *output = data;
	output->pump_at_ms = 0;
	wlr_output_schedule_frame(output->wlr_output);
	return 0;
}

/* A screen capture is answered on an output commit, and a still screen never
 * commits: the nested backend will not even send us a frame callback, because
 * the host only composites on damage. Poll gently while a capture is
 * outstanding and render for it directly. */
static int capture_watchdog(void *data) {
	struct vidow_server *server = data;
	bool pending = server->screencopy != NULL &&
		!wl_list_empty(&server->screencopy->frames);
	if (pending) {
		struct vidow_output *output;
		wl_list_for_each(output, &server->outputs, link) {
			vidow_output_render(output);
		}
	}
	wl_event_source_timer_update(server->capture_timer, 50);
	return 0;
}

/* The nested Wayland backend only delivers a frame event once the host
 * compositor has presented our surface, and the host only presents on damage:
 * that deadlocks an animation that is waiting for its next frame. If frames
 * dry up while an animation is in flight, drive it from a timer instead. */
static int anim_watchdog(void *data) {
	struct vidow_server *server = data;
	if (!vidow_anim_pending(server)) {
		return 0;
	}
	int64_t now = vidow_now_ns();
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (now - output->last_frame_ns < 40 * 1000000LL) {
			continue; /* the backend is still feeding us frames */
		}
		vidow_output_render(output);
	}
	if (vidow_anim_pending(server)) {
		wl_event_source_timer_update(server->anim_timer, 16);
	}
	return 0;
}

/* Armed whenever an animation starts or a frame is presented, so the
 * watchdog is running even if the backend never sends another frame. */
void vidow_output_kick_watchdog(struct vidow_server *server) {
	if (server->anim_timer != NULL) {
		wl_event_source_timer_update(server->anim_timer, 24);
	}
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

	output->last_frame_ns = vidow_now_ns();
	vidow_output_render(output);

	/* An animating node usually damages the scene, but not always (a pure
	 * opacity change on a fully occluded window). Ask for the next frame
	 * anyway, and arm the watchdog in case the request goes nowhere. */
	if (vidow_anim_pending(server)) {
		wlr_output_schedule_frame(output->wlr_output);
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

/* 一个输出该用多大缩放，以及为什么。mode 可能为 NULL（还没上模式）。 */
static double output_pick_scale(struct vidow_server *server,
		struct wlr_output *wlr_output, struct wlr_output_mode *mode,
		char *why, size_t why_len) {
	int w = mode != NULL ? mode->width : wlr_output->width;
	int h = mode != NULL ? mode->height : wlr_output->height;
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
	char why[64];
	double scale = output_pick_scale(server, wlr_output, mode, why, sizeof(why));
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

bool vidow_output_set_mode(struct vidow_server *server,
		struct wlr_output *wlr_output, int width, int height,
		double refresh_hz) {
	struct vidow_output *output = output_of(server, wlr_output);
	if (output == NULL) {
		return false;
	}
	struct wlr_output_mode *match = NULL;
	struct wlr_output_mode *mode;
	wl_list_for_each(mode, &wlr_output->modes, link) {
		if (mode->width != width || mode->height != height) {
			continue;
		}
		if (refresh_hz > 0.0 &&
				fabs(mode->refresh / 1000.0 - refresh_hz) > 0.6) {
			continue;
		}
		match = mode;
		if (mode->preferred) {
			break;
		}
	}

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

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}
	/* 缩放要在第一次 commit 之前定下来：它决定逻辑尺寸（布局坐标的基准），
	 * 也决定客户端拿到的 preferred_buffer_scale。 */
	char why[64];
	double scale = output_pick_scale(server, wlr_output, mode, why, sizeof(why));
	wlr_output_state_set_scale(&state, (float)scale);
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct vidow_output *output = calloc(1, sizeof(*output));
	if (output == NULL) {
		return;
	}
	output->server = server;
	output->wlr_output = wlr_output;
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
	} else {
		wl_event_source_timer_update(server->capture_timer, 50);
	}
	server->kick_timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server->display), kick_timer_cb, server);
	if (server->kick_timer == NULL) {
		wlr_log(WLR_ERROR, "failed to create the resume kick timer");
	}
	server->new_output.notify = server_new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);
	vidow_output_test_hook(server);
	return true;
}
