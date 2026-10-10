/*
 * screencast.c — zkde_screencast_unstable_v1：KWin 的私有录屏协议。
 *
 * 为什么 vidow 要实现一个 KDE 的私有协议：xdg-desktop-portal-kde 的 ScreenCast
 * 就建在它上面 —— 绑不到这个 global，它**连 org.freedesktop.impl.portal.ScreenCast
 * 接口都不注册**（实测：在 Hyprland 上那个后端也只导出 10 个接口，没有 ScreenCast /
 * Screenshot / RemoteDesktop）。所以「KDE 的录屏 portal」在非 KWin 的合成器上不是
 * 配错了，是根本不存在。想让它可用，只有让合成器把这套协议和它背后的 PipeWire 流
 * 提供出来。
 *
 * 协议的形状（protocols/zkde-screencast-unstable-v1.xml，v6）：
 *
 *     客户端 --stream_output(stream, wl_output, pointer_mode)--> 合成器
 *     客户端 <--serial(hi, lo)--- 合成器     （v6，先发）
 *     客户端 <--created(node)---- 合成器     （node = 合成器进程里的 PipeWire node id）
 *     客户端 <--failed(msg)------ 合成器     （出错；跟 created 二选一）
 *
 * 关键点：**PipeWire 流是合成器建的**（created 给的是合成器自己那个 node 的 id），
 * 客户端拿这个 id 去 pw_stream_connect(PW_DIRECTION_INPUT, node) 消费。所以这个模块
 * 同时是个 PipeWire 生产者：把输出的一帧渲染出来，喂进一个 memfd 缓冲。
 *
 * 线程模型：PipeWire 的 loop 直接挂进 wl_event_loop（pw_loop_get_fd + pw_loop_iterate），
 * 全程在合成器主线程上跑 —— 不用锁、不用跨线程往 Wayland 资源上写事件。代价是喂帧
 * 那一下（一次 read_pixels）会占住主线程，跟 screencopy 的行为一致。
 *
 * 光标模式（pointer_mode，协议里的 enum pointer）：hidden / embedded / metadata。
 */

#include "ipc.h"
#include "server.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

#include <drm_fourcc.h>
#include <pipewire/pipewire.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include "zkde-screencast-unstable-v1-server-protocol.h"

/* 协议 XML 里的 interface version（v6 才有 serial 事件）。 */
#define VIDOW_SC_VERSION 6

/* ------------------------------------------------------- PipeWire 生产者 */

/* 一个进程一份：loop 的 fd 挂进 wl_event_loop，所有 PipeWire 回调都在合成器
 * 主线程上跑。 */
struct vidow_sc_pw {
	struct vidow_screencast *sc;
	struct pw_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct spa_hook core_listener;
	struct pw_core_events core_events;
	struct wl_event_source *source;
	/* 心跳：每 1s 给每路流催一次图。屏幕静止时没有任何输出提交，而消费者可能
	 * 是「刚连上」的 —— 不催的话它要等到下一次画面变化才有第一帧（实测：负载
	 * 高的时候 gst 连上来晚了，一帧都拿不到）。1Hz 的代价是静止画面上每路流
	 * 每秒多渲染一帧，可以接受。 */
	struct wl_event_source *tick;
	int sync_seq;
	bool ready;
	/* created 之前要报的 object.serial（v6）。node id 会复用，所以客户端要的是
	 * 这个 64 位序列号 —— 从 registry 的 global 事件里拿（node id → serial）。 */
	struct pw_registry *registry;
	struct spa_hook registry_listener;
	struct {
		uint32_t node;
		uint64_t serial;
	} known[64];
	size_t n_known;
};

/* 一路流。抓帧走 vidow 自己那条路（vidow_capture_grab：遮挡 / 马赛克都盖到了），
 * 像素进 PipeWire 的 memfd 缓冲。 */
struct vidow_sc_pw_stream {
	struct vidow_screencast_stream *stream;
	struct vidow_sc_pw *pw;
	struct pw_stream *pw_stream;
	struct spa_hook listener;
	struct spa_video_info_raw video;
	uint32_t node_id;
	uint64_t serial;
	bool have_serial;
	bool announced;
	bool have_format;
	/* 输出提交时抓好的那一帧，等 process 来取（拿的是加锁的缓冲）。 */
	struct wlr_buffer *pending;
	struct wl_listener output_commit;
	/* pointer_mode=metadata：光标位图的读回缓冲 + 上一次发出去的位图指纹。
	 * 指纹一样就不重发（bitmap_offset=0 = 消费者沿用上一次那张图），光标形状 /
	 * 大小变了才重发。 */
	uint8_t *cursor_pixels;
	uint32_t cursor_hash;
	bool cursor_hashed;
	/* 位图一共发出去过几张（只在变了的时候发）：日志里跟「光标在这一路里」
	 * 对着看，能分清是「我们没发位图」还是「消费者没画」。 */
	uint64_t cursor_bitmaps;
	/* 光标在这一路里的状态（0 还没看过 / 1 报出去了 / 2 这块屏上没有可见光标 /
	 * 3 有光标但在取景框外）。**只在变的那一帧**记一条 INFO —— 每帧记会刷屏，
	 * 只记第一次又看不出「后来到底有没有」：用户报「录屏里看不到光标」时，日志
	 * 里要一眼看出是「指针在别的屏上」「光标在流外面」还是「有光标没报出去」。 */
	int cursor_state;
	/* 光标元数据报过了（只在 metadata 模式、协商到格式之后报一次）。 */
	bool meta_announced;
	/* 给消费者发过多少帧；第一帧顺手看一眼是不是黑的（「有流但黑屏」这类报告
	 * 光靠日志分不清是**我们发的就是黑的**还是消费者那边没画）。 */
	uint64_t frames;
	bool first_checked;
	/* 上一次**真的发出去**一帧的时刻（CLOCK_MONOTONIC 纳秒，0 = 还没发过）：
	 * 发帧节奏的上限（见 stream_send_due）按它算。 */
	int64_t last_send_ns;
	/* 诊断用的计数（`vidowmsg state` 的 screencast 那一块）：抓了几次整帧、
	 * 看见多少次输出提交。用户报「录制期间显示器帧率被压到 60」时，这两个数
	 * 和 frames 摆在一起就能看出「是不是每次提交都抓了一整帧」。 */
	uint64_t grabs;
	uint64_t commits;
	/* 消费者来要帧、但被节奏上限挡下的次数（见 stream_send_due）：这个数大
	 * 说明消费者要得比屏幕刷新率还急，我们按上限压着 —— 不是丢帧，是「屏幕
	 * 上本来就没有新画面」。 */
	uint64_t throttled;
	/* 一帧的主线程成本拆成两半（都是累计纳秒，`vidowmsg state` 的
	 * screencast 里报平均值）：
	 *   grab_ns     —— 整块离屏渲染（把场景画进抓帧缓冲）
	 *   readback_ns —— 从那张缓冲回读到 PipeWire 的 memfd（GPU→CPU 同步拷贝，
	 *                  4K 一帧 33MB）
	 * 用户报「录制时整个桌面（光标）卡」时，这两个数一眼看出钱花在哪一半：
	 * 渲染贵 → 少抓 / 复用上一帧；回读贵 → 该给消费者 dmabuf（不落 CPU）。 */
	int64_t grab_ns;
	int64_t readback_ns;
	int64_t grab_ns_max;
};

/* 光标在这一路里的状态（见 vidow_sc_pw_stream 的 cursor_state）。 */
enum {
	VIDOW_SC_CURSOR_NONE = 0,     /* 还没看过 */
	VIDOW_SC_CURSOR_REPORTED,     /* 报出去了 */
	VIDOW_SC_CURSOR_NONE_SCREEN,  /* 这块屏上没有可见光标（指针在别的屏上） */
	VIDOW_SC_CURSOR_OUTSIDE,      /* 有光标，但在取景框外面 */
	VIDOW_SC_CURSOR_NO_FRAME,     /* 取景框算不出来（窗口停到屏外 / 已经没了） */
};

/* SPA_META_Cursor 的元数据空间大小：位置 / 热点每帧都给，位图最多
 * VIDOW_CURSOR_MAX_PX² × 4 字节（跟消费者在建流时一次性声明，之后不能改）。 */
#define VIDOW_SC_CURSOR_META_SIZE \
	(sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) + \
	 (size_t)VIDOW_CURSOR_MAX_PX * VIDOW_CURSOR_MAX_PX * 4)

static void pw_stream_fill(struct vidow_sc_pw_stream *ps,
		struct pw_buffer *buffer);
static void stream_fill_cursor(struct vidow_sc_pw_stream *ps, struct spa_buffer *buf);
static void stream_announce_cursor_meta(struct vidow_sc_pw_stream *ps);
static void stream_announce(struct vidow_sc_pw_stream *ps);
static struct wlr_buffer *stream_grab(struct vidow_sc_pw_stream *ps);

/* loop 可读 → 跑一轮 PipeWire 的事件循环。 */
static int pw_loop_ready(int fd, uint32_t mask, void *data) {
	(void)fd;
	struct vidow_sc_pw *pw = data;
	if (mask & (WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		pw_loop_iterate(pw->loop, 0);
	}
	return 0;
}

#define VIDOW_SC_TICK_MS 1000

static int pw_tick(void *data) {
	struct vidow_sc_pw *pw = data;
	struct vidow_screencast_stream *stream;
	wl_list_for_each(stream, &pw->sc->streams, link) {
		if (stream->pw != NULL && stream->pw->pw_stream != NULL) {
			pw_stream_trigger_process(stream->pw->pw_stream);
		}
	}
	wl_event_source_timer_update(pw->tick, VIDOW_SC_TICK_MS);
	return 0;
}

static void on_core_done(void *data, uint32_t id, int seq) {
	struct vidow_sc_pw *pw = data;
	if (id == PW_ID_CORE && seq == pw->sync_seq) {
		pw->ready = true;
		wlr_log(WLR_INFO, "screencast: PipeWire core 就绪");
	}
}

static void on_core_error(void *data, uint32_t id, int seq, int res,
		const char *message) {
	(void)data;
	(void)seq;
	wlr_log(WLR_ERROR, "screencast: PipeWire 出错（id=%u res=%d）：%s", id, res,
		message != NULL ? message : "?");
}

/* registry：只为了把「node id → object.serial」记下来（created 之前要报 serial）。 */
static void on_registry_global(void *data, uint32_t id, uint32_t permissions,
		const char *type, uint32_t version, const struct spa_dict *props) {
	(void)permissions;
	(void)version;
	struct vidow_sc_pw *pw = data;
	if (props == NULL || type == NULL || strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
		return;
	}
	const char *serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
	if (serial == NULL || pw->n_known >= 64) {
		return;
	}
	pw->known[pw->n_known].node = id;
	pw->known[pw->n_known].serial = strtoull(serial, NULL, 10);
	pw->n_known++;
	/* 有流正等着这个 node 的 serial 就顺手报出去。 */
	struct vidow_screencast_stream *stream;
	wl_list_for_each(stream, &pw->sc->streams, link) {
		if (stream->pw != NULL && stream->pw->node_id == id) {
			stream_announce(stream->pw);
		}
	}
}

static void on_registry_global_remove(void *data, uint32_t id) {
	(void)data;
	(void)id;
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_registry_global,
	.global_remove = on_registry_global_remove,
};

static const struct pw_core_events pw_core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done,
	.error = on_core_error,
};

struct vidow_sc_pw *vidow_sc_pw_create(struct vidow_screencast *sc,
		struct wl_display *display) {
	struct vidow_sc_pw *pw = calloc(1, sizeof(*pw));
	if (pw == NULL) {
		return NULL;
	}
	pw->sc = sc;
	pw->sync_seq = -1;

	/* 进程级初始化，幂等；后面 pw_* 的报错走 PipeWire 自己的日志（stderr）。 */
	pw_init(NULL, NULL);

	pw->loop = pw_loop_new(NULL);
	if (pw->loop == NULL) {
		free(pw);
		return NULL;
	}
	pw->context = pw_context_new(pw->loop, NULL, 0);
	if (pw->context == NULL) {
		pw_loop_destroy(pw->loop);
		free(pw);
		return NULL;
	}

	/* loop 的 fd 交给 wl_event_loop：一条线程跑到底，不用锁，也不存在
	 * 「从 PipeWire 线程往 Wayland 资源上写事件」这种麻烦。 */
	pw->source = wl_event_loop_add_fd(wl_display_get_event_loop(display),
		pw_loop_get_fd(pw->loop), WL_EVENT_READABLE, pw_loop_ready, pw);
	if (pw->source == NULL) {
		pw_context_destroy(pw->context);
		pw_loop_destroy(pw->loop);
		free(pw);
		return NULL;
	}

	pw->tick = wl_event_loop_add_timer(wl_display_get_event_loop(display),
		pw_tick, pw);
	if (pw->tick != NULL) {
		wl_event_source_timer_update(pw->tick, VIDOW_SC_TICK_MS);
	}

	pw->core = pw_context_connect(pw->context, NULL, 0);
	if (pw->core == NULL) {
		/* INFO 不是 ERROR：录屏是可选功能，没有 PipeWire 的会话（测试环境就是）
		 * 不该在日志里留一条 ERROR —— 一堆测试都断言「日志里没有 ERROR」。 */
		wlr_log(WLR_INFO, "screencast: 连不上 PipeWire（会话里没跑 pipewire？）");
		/* 心跳也必须摘掉：它的 data 是 pw，下面 free(pw) 之后定时器一响就是
		 * 野指针（实测：没 PipeWire 的会话里 vidow 一秒后必崩）。 */
		if (pw->tick != NULL) {
			wl_event_source_remove(pw->tick);
			pw->tick = NULL;
		}
		wl_event_source_remove(pw->source);
		pw_context_destroy(pw->context);
		pw_loop_destroy(pw->loop);
		free(pw);
		return NULL;
	}
	pw_core_add_listener(pw->core, &pw->core_listener, &pw_core_events, pw);
	pw->registry = pw_core_get_registry(pw->core, PW_VERSION_REGISTRY, 0);
	if (pw->registry != NULL) {
		pw_registry_add_listener(pw->registry, &pw->registry_listener,
			&registry_events, pw);
	}
	pw->sync_seq = pw_core_sync(pw->core, PW_ID_CORE, 0);
	return pw;
}

/* ------------------------------------------------------------ 流的事件 */

static uint64_t serial_for_node(struct vidow_sc_pw *pw, uint32_t node) {
	for (size_t i = 0; i < pw->n_known; i++) {
		if (pw->known[i].node == node) {
			return pw->known[i].serial;
		}
	}
	return 0;
}

static void stream_announce(struct vidow_sc_pw_stream *ps) {
	struct vidow_screencast_stream *stream = ps->stream;
	if (ps->announced || stream->resource == NULL) {
		return;
	}
	if (ps->node_id == SPA_ID_INVALID) {
		return;
	}
	uint32_t version = (uint32_t)wl_resource_get_version(stream->resource);
	/* v6 起客户端优先认 object.serial（node id 会复用），而它是 registry 报的
	 * 属性 —— 可能比流的状态变化晚到，等它。 */
	if (version >= 6 && !ps->have_serial) {
		uint64_t serial = serial_for_node(ps->pw, ps->node_id);
		if (serial == 0) {
			return;
		}
		ps->serial = serial;
		ps->have_serial = true;
	}
	ps->announced = true;
	if (version >= 6) {
		zkde_screencast_stream_unstable_v1_send_serial(stream->resource,
			(uint32_t)(ps->serial >> 32), (uint32_t)(ps->serial & 0xffffffffu));
	}
	zkde_screencast_stream_unstable_v1_send_created(stream->resource,
		ps->node_id);
	wlr_log(WLR_INFO, "screencast: %s → PipeWire node %u",
		stream->output != NULL ? stream->output->name : "?", ps->node_id);
}

static void pw_stream_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error) {
	(void)old;
	struct vidow_sc_pw_stream *ps = data;
	if (state == PW_STREAM_STATE_ERROR) {
		vidow_screencast_stream_fail(ps->stream,
			error != NULL ? error : "vidow: PipeWire 流出错");
		return;
	}
	if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) {
		ps->node_id = pw_stream_get_node_id(ps->pw_stream);
		stream_announce(ps);
	}
}

static void pw_stream_param_changed(void *data, uint32_t id,
		const struct spa_pod *param) {
	struct vidow_sc_pw_stream *ps = data;
	if (param == NULL || id != SPA_PARAM_Format) {
		return;
	}
	if (spa_format_video_raw_parse(param, &ps->video) < 0) {
		return;
	}
	ps->have_format = true;
	/* 尺寸**跟着消费者走**：我们只报一种尺寸，可消费者（OBS / 浏览器）会挑它
	 * 自己要的那个，挑不到就整个协商失败（实测：让 gst 按 portal 报的尺寸
	 * 协商，直接 flow-return=-4、一帧都拿不到）。消费者挑了什么，就按什么渲染 ——
	 * 出图尺寸和协商尺寸不一致的话，读像素那一步要么读越界、要么写不进缓冲，
	 * 表现就是「连上了但一片黑」。 */
	struct vidow_screencast_stream *stream = ps->stream;
	uint32_t nw = ps->video.size.width, nh = ps->video.size.height;
	if (nw > 0 && nh > 0 && ((int)nw != stream->out_w || (int)nh != stream->out_h)) {
		wlr_log(WLR_INFO,
			"screencast: 消费者要 %ux%u，跟着它改（原来是 %dx%d）",
			nw, nh, stream->out_w, stream->out_h);
		stream->out_w = (int)nw;
		stream->out_h = (int)nh;
	}
	wlr_log(WLR_INFO, "screencast: 协商到 %ux%u（spa 格式 %u）",
		ps->video.size.width, ps->video.size.height, ps->video.format);
	stream_announce_cursor_meta(ps);
}

/* 这一路发一帧的节奏上限 = min(消费者要的帧率, 这块屏的刷新率)。
 *
 * 为什么要有这个上限：抓一帧是**整块离屏渲染 + 一次整块回读**，而且是在合成器的
 * 主线程上做的（PipeWire 的 loop 挂在 Wayland 的 event loop 上，见 pw_loop_ready）。
 * 以前只要输出提交就抓一帧 —— 144Hz 的屏上就是每秒 144 趟整屏渲染，消费者手上
 * 还有空缓冲时每次提交都再回读一整块（4K 上 33MB），主线程被占满：屏幕上所有
 * 动画 / 拖动跟着掉到 60fps（用户报的「portal 录制导致显示器帧率被限制到 60」）。
 *
 * 消费者要的帧率来自格式协商的 SPA_FORMAT_VIDEO_framerate（录制软件自己填的那个）；
 * 0/1（或者还没协商）算「不限」，那就只按屏幕刷新率封顶 —— 也就是「上限允许到
 * 显示器帧率上限」。屏幕刷新率用输出的**标称** refresh（不是量到的帧间隔：量到的
 * 那个会被「录制把帧率拖下去」反过来影响，帧间隔变大 → 上限放宽 → 抓得更多，
 * 是个正反馈）。输出没报刷新率的（headless / 虚拟输出）才退回量到的帧间隔；
 * 两个都拿不到就不设上限（0 = 不限）。
 *
 * 上限只影响**发帧**：屏幕上的渲染一帧不少，掉帧的只是录制这一路。 */
static int64_t stream_send_interval_ns(struct vidow_sc_pw_stream *ps) {
	int64_t iv = 0;
	struct wlr_output *out = ps->stream->output;
	struct vidow_output *output = out != NULL ?
		vidow_output_lookup(ps->stream->sc->server, out) : NULL;
	if (out != NULL && out->refresh > 0) {
		/* 标称刷新率就是「显示器帧率上限」—— 用它而不是量到的帧间隔：
		 * 量到的那个会被「录制把帧率拖下去了」这件事反过来影响（帧间隔变大 →
		 * 上限放宽 → 更多抓帧 → 更慢），是个正反馈。 */
		iv = 1000000000LL / out->refresh;
	} else if (output != NULL && output->frame_gap_ns > 0) {
		/* 没报刷新率的（headless / 虚拟输出）：用后端量到的帧间隔兜底。 */
		iv = output->frame_gap_ns;
	}
	if (ps->have_format && ps->video.framerate.denom > 0 &&
			ps->video.framerate.num > 0) {
		int64_t consumer = 1000000000LL * ps->video.framerate.denom /
			ps->video.framerate.num;
		if (consumer > 0 && (iv == 0 || consumer > iv)) {
			iv = consumer; /* 消费者要得比屏幕慢：听消费者的 */
		}
	}
	if (iv > 0 && iv < 4 * 1000000LL) {
		iv = 4 * 1000000LL; /* 下限 4ms（250Hz）：0 / 怪值别把我们坑了 */
	}
	return iv;
}

static bool stream_send_due(struct vidow_sc_pw_stream *ps) {
	if (ps->last_send_ns == 0) {
		return true; /* 一帧都还没发过：第一帧立刻给 */
	}
	int64_t iv = stream_send_interval_ns(ps);
	if (iv <= 0) {
		return true;
	}
	return vidow_now_ns() - ps->last_send_ns >= iv;
}

/* 图转起来了 → 把手上那一帧填进去。手上没有（流刚建好、或者消费者刚连上来）
 * 就当场抓一帧：屏幕静止时没有任何输出提交，不这么做消费者一个缓冲都拿不到 ——
 * 表现是「连上了但一直是黑的」。 */
static void pw_stream_process(void *data) {
	struct vidow_sc_pw_stream *ps = data;
	struct pw_buffer *buffer = pw_stream_dequeue_buffer(ps->pw_stream);
	if (buffer == NULL) {
		return;
	}
	/* 到点才抓：上限见 stream_send_due。没到点就只把缓冲还回去（不填）——
	 * 消费者那一格拿到的是空缓冲，但它本来就不该在这时候要帧（是我们催早了）。 */
	if (ps->pending == NULL) {
		if (stream_send_due(ps)) {
			int64_t t0 = vidow_now_ns();
			ps->pending = stream_grab(ps);
			int64_t dt = vidow_now_ns() - t0;
			ps->grab_ns += dt;
			if (dt > ps->grab_ns_max) {
				ps->grab_ns_max = dt;
			}
		} else {
			ps->throttled++;
		}
	}
	if (ps->pending != NULL) {
		pw_stream_fill(ps, buffer);
		wlr_buffer_unlock(ps->pending);
		ps->pending = NULL;
	}
	pw_stream_queue_buffer(ps->pw_stream, buffer);
}

static const struct pw_stream_events pw_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = pw_stream_state_changed,
	.param_changed = pw_stream_param_changed,
	.process = pw_stream_process,
};

/* ------------------------------------------------------------ 抓帧 → 缓冲 */

/* 把 ps->pending 那一帧写进 PipeWire 的缓冲。格式固定 BGRx（= DRM XRGB8888），
 * 尺寸取协商结果（我们只报了输出像素尺寸一种，所以协商结果就是它）。 */
static void pw_stream_fill(struct vidow_sc_pw_stream *ps,
		struct pw_buffer *buffer) {
	struct spa_buffer *buf = buffer->buffer;
	if (buf->n_datas < 1 || buf->datas[0].data == NULL) {
		return;
	}
	struct wlr_buffer *src = ps->pending;
	uint32_t width = (uint32_t)src->width;
	uint32_t height = (uint32_t)src->height;
	if (ps->have_format) {
		width = ps->video.size.width;
		height = ps->video.size.height;
	}
	size_t stride = (size_t)width * 4;
	size_t size = stride * height;
	if (buf->datas[0].maxsize < size) {
		wlr_log(WLR_ERROR, "screencast: PipeWire 缓冲太小（%u < %zu）",
			buf->datas[0].maxsize, size);
		return;
	}

	int64_t rb_t0 = vidow_now_ns();
	struct wlr_texture *texture = wlr_texture_from_buffer(
		ps->stream->output->renderer, src);
	if (texture == NULL) {
		return;
	}
	bool ok = wlr_texture_read_pixels(texture,
		&(struct wlr_texture_read_pixels_options){
			.data = buf->datas[0].data,
			.format = DRM_FORMAT_XRGB8888,
			.stride = stride,
			.src_box = (struct wlr_box){
				.width = (int)width,
				.height = (int)height,
			},
		});
	wlr_texture_destroy(texture);
	if (!ok) {
		return;
	}
	ps->readback_ns += vidow_now_ns() - rb_t0;

	if (buf->datas[0].chunk == NULL) {
		return;
	}
	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->size = size;
	buf->datas[0].chunk->stride = (int32_t)stride;

	ps->frames++;
	ps->last_send_ns = vidow_now_ns();
	if (!ps->first_checked) {
		/* 第一帧报一句：黑屏那类报告（「第一次录是黑的」）看这一行就知道是我们
		 * 发的图就是黑的，还是消费者没画。每 64 个像素采一个，代价可以忽略。 */
		ps->first_checked = true;
		const uint8_t *px = buf->datas[0].data;
		size_t n = 0, lit = 0;
		for (size_t i = 0; i + 3 < size; i += 4 * 64) {
			n++;
			if (px[i] > 8 || px[i + 1] > 8 || px[i + 2] > 8) {
				lit++;
			}
		}
		wlr_log(WLR_INFO,
			"screencast: 第一帧 %ux%u（非黑 %zu%%，采样 %zu 点）",
			width, height, n > 0 ? 100 * lit / n : 0, n);
	}

	if (ps->stream->pointer_mode == ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_METADATA) {
		stream_fill_cursor(ps, buf);
	}
}

/* -------------------------------------------------- 光标元数据（metadata）

 * pointer_mode=metadata：画面里不带光标（stream_grab 不画），改成往缓冲的
 * SPA_META_Cursor 里塞位置 / 热点 / 位图，消费者自己合成。
 *
 * 元数据得**等格式协商完再报**（pw_stream_update_params，见 stream_announce_cursor_meta）：
 * 缓冲是消费者连上来、格式定下来的那一刻按双方的参数一起分配的，连接前报的 meta
 * 进不了缓冲（xdpw / KWin 都是这个顺序 —— connect 只给格式，param_changed 里再
 * update）。报晚了的表现就是「消费者拿到的缓冲里压根没有 SPA_META_Cursor」。
 *
 * 位置和热点**每帧都给**（id=1），位图只在变了的时候给（bitmap_offset=0 表示
 * 「没有新位图，沿用上一次那张」，和 KWin 一样）。指纹用读回来的像素算，比拿
 * wlr_output_cursor 的贴图指针当身份靠谱：贴图会释放再分配，指针能撞上。
 *
 * 抓帧框按来源换算（和 stream_grab 一一对应）：整屏 / 虚拟输出是这块屏在布局里
 * 占的那一块，区域是请求里那个框，窗口是等比缩放后的取景框。 */
static void stream_announce_cursor_meta(struct vidow_sc_pw_stream *ps) {
	struct vidow_screencast_stream *stream = ps->stream;
	if (stream->pointer_mode != ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_METADATA ||
			ps->meta_announced) {
		return;
	}
	ps->meta_announced = true;
	size_t stride = (size_t)stream->out_w * 4;
	uint8_t pod_buffer[512];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer,
		sizeof(pod_buffer));
	const struct spa_pod *params[2];
	/* 缓冲参数跟着重报一遍：xdpw 也是这么干的（格式定下来之后两样一起 update）。 */
	params[0] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 32),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int((int)(stride * (size_t)stream->out_h)),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int((int)stride),
		SPA_PARAM_BUFFERS_dataType,
			SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd),
		0);
	params[1] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
		SPA_PARAM_META_size, SPA_POD_Int((int)VIDOW_SC_CURSOR_META_SIZE));
	pw_stream_update_params(ps->pw_stream, params, 2);
	wlr_log(WLR_INFO, "screencast: 报了光标元数据（SPA_META_Cursor，%zu 字节）",
		VIDOW_SC_CURSOR_META_SIZE);
}

static void stream_fill_cursor(struct vidow_sc_pw_stream *ps, struct spa_buffer *buf) {
	struct vidow_screencast_stream *stream = ps->stream;
	struct spa_meta_cursor *meta = spa_buffer_find_meta_data(buf, SPA_META_Cursor,
		sizeof(*meta));
	if (meta == NULL) {
		wlr_log(WLR_DEBUG, "screencast: 缓冲里没有 SPA_META_Cursor（n_metas=%u）",
			buf->n_metas);
		return; /* 消费者没要这个元数据（老版本 / 自己拼的流） */
	}
	/* 先摆成「没有光标」：消费者看到 id=0 就不画。 */
	meta->id = 0;
	meta->flags = 0;
	meta->position = (struct spa_point){0, 0};
	meta->hotspot = (struct spa_point){0, 0};
	meta->bitmap_offset = 0;

	if (stream->output == NULL) {
		return;
	}
	struct wlr_box box;
	double scale = 1.0;
	switch (stream->kind) {
	case VIDOW_SC_REGION:
		box = stream->box;
		scale = stream->scale;
		break;
	case VIDOW_SC_WINDOW: {
		/* 取景框按**取景器**算：流里的画面是「窗口等比缩放进 out_w × out_h」，
		 * 跟屏的缩放不是一回事（见 vidow_win_grab_frame_box）。以前拿布局几何
		 * 和屏的缩放换算，光标位置落到流外面，元数据里永远是 id=0 —— OBS 那条
		 * 路（cursor_mode=4）的表现就是「有画面、光标不进来」。 */
		if (!vidow_win_grab_frame_box(stream->win_grab, stream->out_w,
				stream->out_h, &box, &scale)) {
			if (ps->cursor_state != VIDOW_SC_CURSOR_NO_FRAME) {
				ps->cursor_state = VIDOW_SC_CURSOR_NO_FRAME;
				wlr_log(WLR_INFO, "screencast: 光标不在这一路里 —— 取景框算不出来"
					"（窗口停到屏外了 / 已经没了）");
			}
			return;
		}
		break;
	}
	case VIDOW_SC_OUTPUT:
	case VIDOW_SC_VIRTUAL:
	default:
		wlr_output_layout_get_box(stream->sc->server->output_layout,
			stream->output, &box);
		scale = stream->output->scale > 0 ? stream->output->scale : 1.0;
		break;
	}

	if (ps->cursor_pixels == NULL) {
		ps->cursor_pixels = malloc((size_t)VIDOW_CURSOR_MAX_PX *
			VIDOW_CURSOR_MAX_PX * 4);
		if (ps->cursor_pixels == NULL) {
			return;
		}
	}
	struct vidow_cursor_info info;
	if (!vidow_capture_cursor_info(stream->sc->server, stream->output, &box, scale,
			&info, ps->cursor_pixels,
			(size_t)VIDOW_CURSOR_MAX_PX * VIDOW_CURSOR_MAX_PX * 4)) {
		if (ps->cursor_state != VIDOW_SC_CURSOR_NONE_SCREEN) {
			ps->cursor_state = VIDOW_SC_CURSOR_NONE_SCREEN;
			wlr_log(WLR_INFO, "screencast: 光标不在这一路里 —— 这块屏（%s）上"
				"没有可见光标（指针在别的屏上？）",
				stream->output != NULL ? stream->output->name : "?");
		}
		wlr_log(WLR_DEBUG, "screencast: metadata：这块屏上没有可见光标");
		ps->cursor_hashed = false; /* 光标没了：下次再出现要重发位图 */
		return;
	}
	wlr_log(WLR_DEBUG, "screencast: metadata：光标 %d,%d 热点 %d,%d 位图 %dx%d",
		info.x, info.y, info.hotspot_x, info.hotspot_y, info.width, info.height);
	/* 光标完全在框外（区域 / 窗口流里最常见）：不报，消费者也别画。 */
	if (info.x + info.hotspot_x < 0 || info.y + info.hotspot_y < 0 ||
			info.x - info.hotspot_x >= stream->out_w ||
			info.y - info.hotspot_y >= stream->out_h) {
		if (ps->cursor_state != VIDOW_SC_CURSOR_OUTSIDE) {
			ps->cursor_state = VIDOW_SC_CURSOR_OUTSIDE;
			wlr_log(WLR_INFO, "screencast: 光标不在这一路里 —— 它在取景框外面"
				"（流里的位置 %d,%d 热点 %d,%d，流 %dx%d，框 %d,%d %dx%d 缩放 %.3f）",
				info.x, info.y, info.hotspot_x, info.hotspot_y, stream->out_w,
				stream->out_h, box.x, box.y, box.width, box.height, scale);
		}
		return;
	}
	if (ps->cursor_state != VIDOW_SC_CURSOR_REPORTED) {
		ps->cursor_state = VIDOW_SC_CURSOR_REPORTED;
		wlr_log(WLR_INFO, "screencast: 光标在这一路里 —— 流里 %d,%d（热点 %d,%d，"
			"位图 %dx%d，流 %dx%d，框 %d,%d %dx%d 缩放 %.3f）",
			info.x, info.y, info.hotspot_x, info.hotspot_y, info.width,
			info.height, stream->out_w, stream->out_h, box.x, box.y, box.width,
			box.height, scale);
	}

	meta->id = 1;
	meta->position.x = info.x;
	meta->position.y = info.y;
	meta->hotspot.x = info.hotspot_x;
	meta->hotspot.y = info.hotspot_y;
	if (info.width <= 0 || info.height <= 0) {
		return; /* 只有位置（位图太大 / 带旋转 / 读不出来） */
	}

	size_t bitmap_size = (size_t)info.width * info.height * 4;
	uint32_t hash = 2166136261u; /* FNV-1a */
	for (size_t i = 0; i < bitmap_size; i++) {
		hash = (hash ^ ps->cursor_pixels[i]) * 16777619u;
	}
	if (ps->cursor_hashed && hash == ps->cursor_hash) {
		return; /* 位图没变：只说位置 */
	}
	ps->cursor_hash = hash;
	ps->cursor_hashed = true;

	/* 位图真的发出去时记一条（只在变了的时候发，所以不会刷屏）：「消费者那边没
	 * 光标」这类报告里，日志必须能分清**我们到底发没发过位图** —— 只报位置
	 * （bitmap_offset=0）的话，消费者手上没有图可画。 */
	ps->cursor_bitmaps++;
	wlr_log(WLR_INFO, "screencast: 光标位图发过去了（%dx%d，第 %llu 张，%s）",
		info.width, info.height, (unsigned long long)ps->cursor_bitmaps,
		ps->cursor_bitmaps == 1 ? "第一次" : "形状 / 大小换了");

	meta->bitmap_offset = sizeof(struct spa_meta_cursor);
	struct spa_meta_bitmap *bitmap = SPA_PTROFF(meta, meta->bitmap_offset,
		struct spa_meta_bitmap);
	bitmap->format = info.format;
	bitmap->size.width = (uint32_t)info.width;
	bitmap->size.height = (uint32_t)info.height;
	bitmap->stride = (int32_t)(info.width * 4);
	bitmap->offset = sizeof(struct spa_meta_bitmap);
	memcpy(SPA_PTROFF(bitmap, bitmap->offset, void), ps->cursor_pixels, bitmap_size);
}

/* 抓当前这一帧（遮挡 / 马赛克都盖到了），要光标就画上。返回加锁的缓冲。
 *
 * 四种来源各抓各的：
 *   OUTPUT / VIRTUAL  整块输出那一帧（虚拟输出也是一块真输出）
 *   REGION            这块屏上的一个矩形（布局坐标 + 缩放）
 *   WINDOW            只这一个窗口（取景器渲染，见 wincapture.c）
 */
static struct wlr_buffer *stream_grab(struct vidow_sc_pw_stream *ps) {
	struct vidow_screencast_stream *stream = ps->stream;
	if (stream->output == NULL) {
		return NULL;
	}
	ps->grabs++;
	struct wlr_buffer *frame = NULL;
	struct wlr_box cursor_box = {0};
	double cursor_scale = 0;
	switch (stream->kind) {
	case VIDOW_SC_REGION:
		frame = vidow_capture_grab_box(stream->sc->server, stream->output,
			&stream->box, stream->scale);
		cursor_box = stream->box;
		cursor_scale = stream->scale;
		break;
	case VIDOW_SC_WINDOW:
		/* 走窗口自己的取景器（私有场景 + 镜像，见 wincapture.c）：跟它在屏幕上
		 * 摆在哪没关系 —— 按布局坐标抓的话，窗口滚出视口 / 停到屏外之后那块坐标
		 * 里什么都没有，录到的是**一片黑**。 */
		if (stream->win_grab == NULL) {
			/* 窗口没了（取景器跟着拆了）：这路流没内容了。抓帧那条路（输出
			 * 提交 / 1Hz 心跳）还会再来问一次 —— 实测：不挡的话下一次输出
			 * 提交就在 mirror_extents 里空指针崩了。 */
			return NULL;
		}
		frame = vidow_win_grab_render(stream->win_grab, stream->out_w,
			stream->out_h);
		break;
	case VIDOW_SC_OUTPUT:
	case VIDOW_SC_VIRTUAL:
	default:
		frame = vidow_capture_grab(stream->sc->server, stream->output);
		break;
	}
	if (frame == NULL) {
		return NULL;
	}
	if (stream->pointer_mode == ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED) {
		switch (stream->kind) {
		case VIDOW_SC_REGION:
			/* 区域抓帧：光标位置和区域框都在布局坐标里，直接把框给出去
			 * （框外的光标不画）。 */
			vidow_capture_paint_cursor_box(stream->sc->server, stream->output,
				frame, &cursor_box, cursor_scale);
			break;
		case VIDOW_SC_WINDOW:
			/* 窗口抓帧也画：取景框和缩放有了（vidow_win_grab_frame_box），
			 * 光标跟窗口在同一个坐标系里了。停到屏外的窗口不算 —— 那时没有
			 * 「光标指着它」这回事。 */
			if (vidow_win_grab_frame_box(stream->win_grab, stream->out_w,
					stream->out_h, &cursor_box, &cursor_scale)) {
				vidow_capture_paint_cursor_box(stream->sc->server,
					stream->output, frame, &cursor_box, cursor_scale);
			}
			break;
		case VIDOW_SC_OUTPUT:
		case VIDOW_SC_VIRTUAL:
		default:
			vidow_capture_paint_cursor(stream->sc->server, stream->output, frame);
			break;
		}
	}
	return frame;
}

/* 输出提交了（有新画面）→ 抓一帧存着，催一次 PipeWire 的图。 */
static void stream_handle_output_commit(struct wl_listener *listener, void *data) {
	struct vidow_sc_pw_stream *ps =
		wl_container_of(listener, ps, output_commit);
	struct vidow_screencast_stream *stream = ps->stream;
	struct wlr_output_event_commit *event = data;

	if (stream->output == NULL) {
		return;
	}
	if (!(event->state->committed & WLR_OUTPUT_STATE_BUFFER)) {
		return;
	}
	ps->commits++;
	if (ps->pending != NULL) {
		/* 上一帧还没被取走：丢掉它（录屏要的是最新画面），下一次真的要发时
		 * 现抓一张新的。 */
		wlr_buffer_unlock(ps->pending);
		ps->pending = NULL;
	}
	/* **不在这里抓帧**：抓一次是整块离屏渲染 + 回读，而消费者手上没空缓冲时
	 * 抓了也发不出去（以前每个提交都抓一遍，144Hz 的屏上就把主线程占满了）。
	 * 真正的抓帧在 pw_stream_process 里 —— 消费者要一帧、而且到点了才抓。 */
	if (stream_send_due(ps)) {
		pw_stream_trigger_process(ps->pw_stream);
	}
}

/* 指针动了 → 光标不在画面里的那几路流推一帧。
 *
 * 为什么必须推：metadata 模式下光标**不画进画面**，它的位置 / 位图是每帧的元数据
 * —— 跟画面内容一样属于「这一帧长什么样」。而帧的推手只有两个：输出提交（屏幕上
 * 有东西重画）和 1 秒一次的心跳。指针在**静止的窗口**上移动时这两个都不发生，消费
 * 者（OBS）手里就一直是上一次那一帧的光标状态 —— 那一次很可能正好是「光标在取景框
 * 外面」（id=0 → 不画）或者位置早就过期了。用户报的「录屏里光标时有时无 / 现在又
 * 看不到光标了」就是这么来的：光标移动跟不跟得上，全看窗口自己在不在重画。
 *
 * embedded 模式同理（光标是画进画面的，指针一动画面就变了），hidden 模式不用
 * （画面里根本没有光标）。
 *
 * 节奏跟发帧那条上限走（stream_send_due = min(消费者帧率, 屏幕刷新率)）：指针事件
 * 可以几百 Hz，每个都抓一整帧没必要 —— 消费者取缓冲也就这个速率。以前这里写死
 * 16ms（60Hz），144Hz 的屏上「指针跟手」就只到 60fps。 */
void vidow_screencast_cursor_moved(struct vidow_server *server) {
	if (server == NULL || server->screencast == NULL) {
		return;
	}
	struct vidow_screencast_stream *stream;
	bool any = false;
	wl_list_for_each(stream, &server->screencast->streams, link) {
		if (stream->pw != NULL && stream->pw->pw_stream != NULL &&
				stream->pointer_mode != ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_HIDDEN) {
			any = true;
			break;
		}
	}
	if (!any) {
		return; /* 没人在录光标：一次都不往下走 */
	}
	int64_t ns = vidow_now_ns();
	wl_list_for_each(stream, &server->screencast->streams, link) {
		if (stream->pw == NULL || stream->pw->pw_stream == NULL ||
				stream->pointer_mode == ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_HIDDEN) {
			continue;
		}
		if (!stream_send_due(stream->pw)) {
			continue; /* 还没到这一路该有的节奏 */
		}
		stream->cursor_push_ns = ns;
		if (stream->cursor_pushes == 0) {
			/* 只记第一次：这条日志是给「录屏里光标不动 / 时有时无」这类报告
			 * 看的（有它 = 这一路真的在跟着指针推帧），每 16ms 记一条会刷屏。 */
			wlr_log(WLR_INFO, "screencast: 指针动了 —— 这一路开始跟着指针推帧"
				"（pointer_mode=%u）", stream->pointer_mode);
		}
		stream->cursor_pushes++;
		/* 手上那张是上一次指针位置抓的：丢掉，真要发的时候现抓（光标的位置 /
		 * 位图属于「这一帧长什么样」）。 */
		if (stream->pw->pending != NULL) {
			wlr_buffer_unlock(stream->pw->pending);
			stream->pw->pending = NULL;
		}
		pw_stream_trigger_process(stream->pw->pw_stream);
	}
}

/* 一路流的发帧节奏诊断（见 server.h 的 struct vidow_sc_stream_stats）。 */
bool vidow_screencast_stream_stats(const struct vidow_screencast_stream *stream,
		struct vidow_sc_stream_stats *out) {
	if (stream == NULL || out == NULL) {
		return false;
	}
	*out = (struct vidow_sc_stream_stats){ .last_send_ms = -1 };
	struct vidow_sc_pw_stream *ps = stream->pw;
	if (ps == NULL) {
		return true;
	}
	out->has_pw = ps->pw_stream != NULL;
	if (ps->have_format && ps->video.framerate.denom > 0 &&
			ps->video.framerate.num > 0) {
		out->consumer_fps = (double)ps->video.framerate.num /
			(double)ps->video.framerate.denom;
	}
	int64_t iv = stream_send_interval_ns(ps);
	out->cap_ms = iv > 0 ? (double)iv / 1e6 : 0.0;
	out->frames = ps->frames;
	out->grabs = ps->grabs;
	out->commits = ps->commits;
	out->throttled = ps->throttled;
	if (ps->grabs > 0) {
		out->grab_ms = (double)ps->grab_ns / (double)ps->grabs / 1e6;
	}
	out->grab_ms_max = (double)ps->grab_ns_max / 1e6;
	if (ps->frames > 0) {
		out->readback_ms = (double)ps->readback_ns / (double)ps->frames / 1e6;
	}
	if (ps->last_send_ns > 0) {
		out->last_send_ms = ps->last_send_ns / 1000000;
	}
	return true;
}

/* ------------------------------------------------------------ 流的生命周期 */

static const char *kind_name(enum vidow_sc_kind kind);
static int count_streams(const struct vidow_screencast *sc);

void vidow_sc_pw_stream_start(struct vidow_screencast_stream *stream) {
	struct vidow_screencast *sc = stream->sc;
	if (sc->pw == NULL) {
		vidow_screencast_stream_fail(stream,
			"vidow: PipeWire 没连上（会话里没有 pipewire？）");
		return;
	}
	if (stream->output == NULL) {
		vidow_screencast_stream_fail(stream, "vidow: 没有输出可录");
		return;
	}
	struct wlr_output *output = stream->output;
	struct vidow_sc_pw_stream *ps = calloc(1, sizeof(*ps));
	if (ps == NULL) {
		vidow_screencast_stream_fail(stream, "vidow: 内存不够");
		return;
	}
	ps->stream = stream;
	ps->pw = sc->pw;
	ps->node_id = SPA_ID_INVALID;
	stream->pw = ps;

	/* 事件：这一路真的开始录了（哪块屏 / 什么内容 + 一共几路）。用户脚本靠它
	 * 做「录屏时别弹通知 / 别切工作区」这类事。失败的流不会走到这儿。 */
	const struct vidow_event_field fields[] = {
		VIDOW_EV_STR("output", output->name),
		VIDOW_EV_STR("kind", kind_name(stream->kind)),
		VIDOW_EV_NUM("streams", count_streams(sc)),
		VIDOW_EV_END,
	};
	vidow_ipc_emit_simple(sc->server, "screencast-started", fields);

	/* 流的像素尺寸：整块输出 / 虚拟输出就是输出的像素尺寸，区域 / 窗口是调用方
	 * 在请求里算好的（见 handle_stream_region / handle_stream_window）。 */
	int width = stream->out_w > 0 ? stream->out_w : output->width;
	int height = stream->out_h > 0 ? stream->out_h : output->height;
	if (width <= 0 || height <= 0) {
		stream->pw = NULL;
		free(ps);
		vidow_screencast_stream_fail(stream, "vidow: 要录的东西没有尺寸");
		return;
	}
	stream->out_w = width;
	stream->out_h = height;
	size_t stride = (size_t)width * 4;

	char name[128];
	snprintf(name, sizeof(name), "vidow-screencast-%s", output->name);
	const char *desc = "vidow 屏幕";
	switch (stream->kind) {
	case VIDOW_SC_WINDOW:
		desc = "vidow 窗口";
		break;
	case VIDOW_SC_REGION:
		desc = "vidow 区域";
		break;
	case VIDOW_SC_VIRTUAL:
		desc = "vidow 虚拟输出";
		break;
	default:
		break;
	}
	struct pw_properties *props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Video",
		PW_KEY_MEDIA_CATEGORY, "Capture",
		PW_KEY_MEDIA_ROLE, "Screen",
		PW_KEY_NODE_NAME, name,
		PW_KEY_NODE_DESCRIPTION, desc,
		NULL);
	ps->pw_stream = pw_stream_new(sc->pw->core, name, props);
	if (ps->pw_stream == NULL) {
		stream->pw = NULL;
		free(ps);
		vidow_screencast_stream_fail(stream, "vidow: 建不了 PipeWire 流");
		return;
	}
	pw_stream_add_listener(ps->pw_stream, &ps->listener, &pw_stream_events, ps);

	/* 只报一种格式（BGRx + 输出像素尺寸），协商没得选 —— 消费者要别的尺寸
	 * 自己加 filter。缓冲走 memfd：合成器这边直接 read_pixels 进去。
	 *
	 * dmabuf 直通（像素不落 CPU）**没做成**：PipeWire 这一层是**消费者**分配
	 * 缓冲，我们报 DmaBuf 它就去要 dmabuf 型的缓冲，而消费者（GStreamer /
	 * 我们自己的 probe）的分配器建不出来 —— 实测 `error alloc buffers:
	 * 不支持的操作`（ENOTSUP）、一帧都拿不到。要做到底得先有一个能分配 dmabuf
	 * 的消费者来验（OBS 那条路 / 自己写一个 gbm 分配的 probe），见 README
	 * 「录屏的发帧节奏」那一节。 */
	uint8_t pod_buffer[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer,
		sizeof(pod_buffer));
	const struct spa_pod *params[2];
	struct spa_rectangle size = SPA_RECTANGLE((uint32_t)width, (uint32_t)height);
	struct spa_fraction rate = SPA_FRACTION(0, 1);
	struct spa_fraction min_rate = SPA_FRACTION(0, 1);
	struct spa_fraction max_rate = SPA_FRACTION(1000, 1);

	struct spa_pod_frame frame;
	spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format,
		SPA_PARAM_EnumFormat);
	spa_pod_builder_add(&builder,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
		/* 尺寸给死值：给了范围消费者会挑最小（实测协商出 1x1，喂帧全被
		 * 「缓冲太小」挡掉）。要别的尺寸让它自己加 filter。 */
		SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&rate,
			&min_rate, &max_rate),
		/* 这里**不报** SPA_FORMAT_VIDEO_modifier：报了就等于说「我给你的图带
		 * 修饰符」，GStreamer 的 pipewiresrc 会因此协商不出固定 caps（实测
		 * handle_format_change 直接断言失败、一帧都拿不到）。 */
		0);
	params[0] = spa_pod_builder_pop(&builder, &frame);

	spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_ParamBuffers,
		SPA_PARAM_Buffers);
	spa_pod_builder_add(&builder,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 32),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int((int)(stride * (size_t)height)),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int((int)stride),
		SPA_PARAM_BUFFERS_dataType,
			SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd),
		0);
	params[1] = spa_pod_builder_pop(&builder, &frame);

	/* 光标元数据不在这里报：得等格式协商完（见 stream_announce_cursor_meta）。 */

	int res = pw_stream_connect(ps->pw_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
		PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS, params, 2);
	if (res < 0) {
		vidow_screencast_stream_fail(stream, "vidow: 连不上 PipeWire 图");
		return;
	}

	ps->output_commit.notify = stream_handle_output_commit;
	wl_signal_add(&output->events.commit, &ps->output_commit);
}

void vidow_sc_pw_stream_destroy(struct vidow_screencast_stream *stream) {
	struct vidow_sc_pw_stream *ps = stream->pw;
	if (ps == NULL) {
		return;
	}
	stream->pw = NULL;
	if (ps->pending != NULL) {
		wlr_buffer_unlock(ps->pending);
		ps->pending = NULL;
	}
	/* 这路流一共给了多少帧：跟「第一帧」那条对着看，「有流但黑屏 / 不动」是
	 * 帧不够还是帧本身有问题，一眼分得出来。 */
	wlr_log(WLR_INFO, "screencast: 这路流给了 %llu 帧（其中 %llu 帧是指针动了催的）",
		(unsigned long long)ps->frames,
		(unsigned long long)(stream->cursor_pushes));
	if (ps->output_commit.link.prev != NULL) {
		wl_list_remove(&ps->output_commit.link);
	}
	if (ps->pw_stream != NULL) {
		spa_hook_remove(&ps->listener);
		pw_stream_destroy(ps->pw_stream);
		ps->pw_stream = NULL;
	}
	free(ps->cursor_pixels);
	free(ps);
}

/* ------------------------------------------------------------ 资源生命周期 */

static void stream_destroy(struct vidow_screencast_stream *stream);

static void stream_resource_destroy(struct wl_resource *resource) {
	struct vidow_screencast_stream *stream =
		wl_resource_get_user_data(resource);
	if (stream == NULL) {
		return;
	}
	stream->resource = NULL;
	stream_destroy(stream);
}

/* 客户端说「我不要了」。close 在协议里是 destructor，但 wl_resource 也可能被
 * 直接销毁（客户端断线），两条路都收敛到 stream_destroy。 */
static void stream_handle_close(struct wl_client *client,
		struct wl_resource *resource) {
	(void)client;
	struct vidow_screencast_stream *stream =
		wl_resource_get_user_data(resource);
	if (stream != NULL) {
		stream->closed = true;
	}
	wl_resource_destroy(resource);
}

static const struct zkde_screencast_stream_unstable_v1_interface stream_impl = {
	.close = stream_handle_close,
};

/* 输出没了（热插拔 / 切 VT）：协议没规定，KWin 是给个 failed，照做 —— 客户端
 * （portal）会把它当成「这一路流结束了」。 */
static void stream_handle_output_destroy(struct wl_listener *listener,
		void *data) {
	(void)data;
	struct vidow_screencast_stream *stream =
		wl_container_of(listener, stream, output_destroy);
	if (stream->resource != NULL) {
		zkde_screencast_stream_unstable_v1_send_failed(stream->resource,
			"vidow: 输出没了（热插拔 / 切 VT）");
	}
	stream->output = NULL;
	wl_list_remove(&stream->output_destroy.link);
	wl_list_init(&stream->output_destroy.link);
}

void vidow_screencast_stream_fail(struct vidow_screencast_stream *stream,
		const char *why) {
	if (stream->resource == NULL) {
		return;
	}
	wlr_log(WLR_INFO, "screencast: 这一路流失败：%s", why);
	zkde_screencast_stream_unstable_v1_send_failed(stream->resource, why);
}

/* 这一路在录什么（事件里报出来用）。 */
static const char *kind_name(enum vidow_sc_kind kind) {
	switch (kind) {
	case VIDOW_SC_OUTPUT:
		return "output";
	case VIDOW_SC_WINDOW:
		return "window";
	case VIDOW_SC_REGION:
		return "region";
	case VIDOW_SC_VIRTUAL:
		return "virtual";
	}
	return "?";
}

/* 现在挂着几路流（含 stream 自己）。 */
static int count_streams(const struct vidow_screencast *sc) {
	int n = 0;
	struct vidow_screencast_stream *st;
	wl_list_for_each(st, &sc->streams, link) {
		n++;
	}
	return n;
}

static void stream_destroy(struct vidow_screencast_stream *stream) {
	struct vidow_screencast *sc = stream->sc;
	/* 事件：这一路结束了（客户端断开 / 窗口没了 / 输出拔了都会走到这儿）。
	 * streams 是「还剩几路」（这条还在列表里，所以是含自己的数）。 */
	if (sc != NULL) {
		const struct vidow_event_field fields[] = {
			VIDOW_EV_STR("output", stream->output != NULL ?
				stream->output->name : NULL),
			VIDOW_EV_STR("kind", kind_name(stream->kind)),
			VIDOW_EV_NUM("streams", count_streams(sc)),
			VIDOW_EV_END,
		};
		vidow_ipc_emit_simple(sc->server, "screencast-stopped", fields);
	}
	if (stream->resource != NULL) {
		wl_resource_set_user_data(stream->resource, NULL);
		stream->resource = NULL;
	}
	vidow_sc_pw_stream_destroy(stream);
	/* 虚拟输出是我们建的：这一路结束就把它拔掉（不留在布局里）。 */
	struct wlr_output *owned = stream->owns_output ? stream->output : NULL;
	if (stream->output != NULL) {
		wl_list_remove(&stream->output_destroy.link);
		stream->output = NULL;
	}
	if (stream->view != NULL) {
		wl_list_remove(&stream->view_destroy.link);
		stream->view = NULL;
	}
	/* 取景器得在窗口还活着的时候拆（它按表面销毁自己收尾，见 wincapture.c）。 */
	vidow_win_grab_destroy(stream->win_grab);
	stream->win_grab = NULL;
	if (owned != NULL) {
		wlr_output_destroy(owned);
	}
	wl_list_remove(&stream->link);
	free(stream);
}

/* 建一路流（输出可以是 NULL：来源不支持时也建出来，然后在上面回 failed ——
 * 协议里没有 error code，回 failed 才是客户端认的方式）。 */
static struct vidow_screencast_stream *stream_create(struct wl_client *client,
		struct vidow_screencast *sc, uint32_t stream_id, uint32_t version,
		struct wlr_output *output, uint32_t pointer_mode) {
	struct vidow_screencast_stream *stream = calloc(1, sizeof(*stream));
	if (stream == NULL) {
		wl_client_post_no_memory(client);
		return NULL;
	}
	stream->sc = sc;
	stream->output = output;
	stream->pointer_mode = pointer_mode;
	wl_list_init(&stream->output_destroy.link);
	wl_list_init(&stream->view_destroy.link);
	wl_list_insert(&sc->streams, &stream->link);

	stream->resource = wl_resource_create(client,
		&zkde_screencast_stream_unstable_v1_interface, (int)version, stream_id);
	if (stream->resource == NULL) {
		wl_list_remove(&stream->link);
		free(stream);
		wl_client_post_no_memory(client);
		return NULL;
	}
	wl_resource_set_implementation(stream->resource, &stream_impl, stream,
		stream_resource_destroy);

	if (output != NULL) {
		stream->output_destroy.notify = stream_handle_output_destroy;
		wl_signal_add(&output->events.destroy, &stream->output_destroy);
	}
	return stream;
}

/* ------------------------------------------------------------ 协议请求 */

static void handle_stream_output(struct wl_client *client,
		struct wl_resource *resource, uint32_t stream_id,
		struct wl_resource *output_resource, uint32_t pointer_mode) {
	struct vidow_screencast *sc = wl_resource_get_user_data(resource);
	if (sc == NULL) {
		return;
	}
	uint32_t version = (uint32_t)wl_resource_get_version(resource);
	struct wlr_output *output = wlr_output_from_resource(output_resource);

	struct vidow_screencast_stream *stream = stream_create(client, sc,
		stream_id, version, output, pointer_mode);
	if (stream == NULL) {
		return;
	}
	if (output == NULL) {
		vidow_screencast_stream_fail(stream,
			"vidow: 这个 wl_output 不是我们的");
		return;
	}
	wlr_log(WLR_INFO, "screencast: 请求录 %s（pointer_mode=%u，协议 v%u）",
		output->name, pointer_mode, version);

	vidow_sc_pw_stream_start(stream);
}

/* 来源不支持 / 找不到时也要建出这一路，然后在上面回 failed —— 协议里没有 error
 * code，回 failed 才是客户端认的方式（什么都不回的话客户端会一直等 created）。 */
static void reply_failed(struct wl_client *client, struct vidow_screencast *sc,
		struct wl_resource *resource, uint32_t stream_id, const char *what) {
	struct vidow_screencast_stream *stream = stream_create(client, sc,
		stream_id, (uint32_t)wl_resource_get_version(resource), NULL, 0);
	if (stream != NULL) {
		vidow_screencast_stream_fail(stream, what);
	}
}

/* ------------------------------------------------------------ 按窗口录屏 */

/* window_uuid 是 ext-foreign-toplevel-list 给的 identifier（wlroots 建 handle 时
 * 生成的，一个窗口一个、不复用）。portal 那边（我们的 vidow-portal）从同一个协议
 * 拿到它，原样传回来，这里反查成 vidow 的 view。 */
static struct vidow_view *view_by_identifier(struct vidow_server *server,
		const char *identifier) {
	if (identifier == NULL || identifier[0] == '\0') {
		return NULL;
	}
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->ftl_handle != NULL && view->ftl_handle->identifier != NULL &&
				strcmp(view->ftl_handle->identifier, identifier) == 0) {
			return view;
		}
	}
	return NULL;
}

/* 窗口没了（关掉 / 崩了）→ 这一路流结束。协议里管这个叫 closed（「合成器这边
 * 停了，你 close 掉吧」），KWin 也是这么干的。 */
static void stream_handle_view_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_screencast_stream *stream =
		wl_container_of(listener, stream, view_destroy);
	stream->view = NULL;
	wl_list_remove(&stream->view_destroy.link);
	wl_list_init(&stream->view_destroy.link);
	vidow_win_grab_destroy(stream->win_grab);
	stream->win_grab = NULL;
	if (stream->resource != NULL) {
		zkde_screencast_stream_unstable_v1_send_closed(stream->resource);
	}
}

/* 按窗口录屏：流的尺寸取窗口**当下**的大小（协议没说窗口改了尺寸怎么办，KDE 那边
 * 是重新协商；我们固定流的分辨率、把窗口等比缩放填进去，消费者不用跟着重协商）。 */
static void handle_stream_window(struct wl_client *client,
		struct wl_resource *resource, uint32_t stream_id,
		const char *window_uuid, uint32_t pointer_mode) {
	struct vidow_screencast *sc = wl_resource_get_user_data(resource);
	if (sc == NULL) {
		return;
	}
	uint32_t version = (uint32_t)wl_resource_get_version(resource);
	struct vidow_view *view = view_by_identifier(sc->server, window_uuid);
	if (view == NULL) {
		wlr_log(WLR_INFO, "screencast: 找不到窗口 identifier=%s（手上这些：）",
			window_uuid != NULL ? window_uuid : "(空)");
		struct vidow_view *v;
		wl_list_for_each(v, &sc->server->views, link) {
			wlr_log(WLR_INFO, "screencast:   「%s」=%s", vidow_view_title(v),
				v->ftl_handle != NULL && v->ftl_handle->identifier != NULL
					? v->ftl_handle->identifier : "(没有 handle)");
		}
		reply_failed(client, sc, resource, stream_id,
			"vidow: 没有这个窗口（它可能已经关了）");
		return;
	}
	if (view->ftl_handle == NULL) {
		reply_failed(client, sc, resource, stream_id,
			"vidow: 窗口列表没建起来，没法盯住这个窗口");
		return;
	}
	struct wlr_box geom = vidow_view_geometry(view);
	if (geom.width <= 0 || geom.height <= 0) {
		reply_failed(client, sc, resource, stream_id,
			"vidow: 这个窗口还没有大小");
		return;
	}
	/* 窗口在哪块屏上：问 wincapture.c 的 vidow_win_grab_output()（平铺的看它那条
	 * 列挂在哪块屏，浮动 / 弹窗按画在哪块屏上反查）—— 不能用 vidow_view_geometry()：
	 * 那是客户端声明的窗口几何，它的 x/y 是**表面内部**的偏移（一般就是 0,0），拿来
	 * 当布局坐标使，反查出来永远是布局左上角那块屏。选错屏的后果是三件（实测）：
	 * ① 光标元数据在错的那块屏上找光标 → 永远 id=0（「非 0 显示器上采集不到光标」）；
	 * ② 流尺寸乘的是错那块屏的 scale（缩放屏上分辨率不对）；③ 输出提交那条抓帧路
	 * 盯的也是错那块屏。
	 *
	 * 这个判据和窗口抓帧源（wincapture.c 的 vidow_win_grab_output）**共用一份**：
	 * portal 报给前端的 size 是从抓帧源的约束来的，两处选的屏不一样时，报出去的
	 * size 就跟这一路流的缓冲尺寸对不上（2 倍屏上消费者会把光标位置换算错）。 */
	struct wlr_output *output = vidow_win_grab_output(sc->server, view);
	if (output == NULL) {
		reply_failed(client, sc, resource, stream_id,
			"vidow: 这个窗口不在任何一块屏上");
		return;
	}

	struct vidow_screencast_stream *stream = stream_create(client, sc,
		stream_id, version, output, pointer_mode);
	if (stream == NULL) {
		return;
	}
	stream->kind = VIDOW_SC_WINDOW;
	stream->view = view;
	stream->win_grab = vidow_win_grab_create(sc->server, view);
	if (stream->win_grab == NULL) {
		stream_destroy(stream);
		reply_failed(client, sc, resource, stream_id,
			"vidow: 建不了窗口的取景器");
		return;
	}
	/* 尺寸取**窗口表面**那块（= 选择器里预览缩略图的尺寸）—— 两边对不上的话，
	 * 消费者按 portal 报的尺寸来协商就会整个失败（见 param_changed 里那段）。 */
	int gw = 0, gh = 0;
	if (vidow_win_grab_box(stream->win_grab, &gw, &gh)) {
		stream->out_w = (int)lround(gw * output->scale);
		stream->out_h = (int)lround(gh * output->scale);
	} else {
		stream->out_w = (int)lround(geom.width * output->scale);
		stream->out_h = (int)lround(geom.height * output->scale);
	}
	stream->view_destroy.notify = stream_handle_view_destroy;
	wl_signal_add(&view->ftl_handle->events.destroy, &stream->view_destroy);
	wlr_log(WLR_INFO, "screencast: 请求录窗口「%s」（%s，%s scale %.2f → %dx%d）",
		vidow_view_title(view), vidow_view_app_id(view), output->name,
		output->scale > 0 ? output->scale : 1.0,
		stream->out_w, stream->out_h);

	vidow_sc_pw_stream_start(stream);
}

/* ------------------------------------------------------------ 按区域录屏 */

static void handle_stream_region(struct wl_client *client,
		struct wl_resource *resource, uint32_t stream_id, int32_t x, int32_t y,
		uint32_t width, uint32_t height, wl_fixed_t scale,
		uint32_t pointer_mode) {
	struct vidow_screencast *sc = wl_resource_get_user_data(resource);
	if (sc == NULL) {
		return;
	}
	uint32_t version = (uint32_t)wl_resource_get_version(resource);
	if (width == 0 || height == 0) {
		reply_failed(client, sc, resource, stream_id, "vidow: 区域是空的");
		return;
	}
	/* 这块区域压在哪块屏上（取中心）：渲染用它那块屏的抓帧器。跨屏的区域只
	 * 会抓到中心那块屏上的部分 —— 按区域录屏本来就是「工作区里的一块」，
	 * 跨屏取景先不做。 */
	struct wlr_output *output = wlr_output_layout_output_at(sc->server->output_layout,
		x + (int)width / 2, y + (int)height / 2);
	if (output == NULL) {
		reply_failed(client, sc, resource, stream_id,
			"vidow: 这块区域不在任何一块屏上");
		return;
	}
	double s = wl_fixed_to_double(scale);
	if (s <= 0) {
		s = output->scale > 0 ? output->scale : 1.0;
	}

	struct vidow_screencast_stream *stream = stream_create(client, sc,
		stream_id, version, output, pointer_mode);
	if (stream == NULL) {
		return;
	}
	stream->kind = VIDOW_SC_REGION;
	stream->box = (struct wlr_box){.x = x, .y = y, .width = (int)width,
		.height = (int)height};
	stream->scale = s;
	stream->out_w = (int)lround(width * s);
	stream->out_h = (int)lround(height * s);
	wlr_log(WLR_INFO, "screencast: 请求录区域 %d,%d %ux%u（%.2fx → %dx%d）",
		x, y, width, height, s, stream->out_w, stream->out_h);

	vidow_sc_pw_stream_start(stream);
}

/* ------------------------------------------------------------ 虚拟输出 */

/* 现场加一块虚拟输出：headless 后端 + 一块输出。它是一块**真输出** —— 进布局、
 * 能摆窗口、能当窗口的主输出，录的就是它自己那一帧（和真屏走同一条路）。
 *
 * 第一次要的时候才建 headless 后端（一个进程一个，建完留着）。 */
struct wlr_output *vidow_screencast_virtual_output(struct vidow_screencast *sc,
		const char *name, int width, int height, double scale, const char **why) {
	struct wlr_backend *backend = sc->virtual_backend;
	if (backend == NULL) {
		if (wlr_backend_is_headless(sc->server->backend)) {
			/* 整个会话本来就是 headless（测试 / 无头机器）：直接用。 */
			backend = sc->server->backend;
		} else if (wlr_backend_is_multi(sc->server->backend)) {
			struct wlr_backend *headless = wlr_headless_backend_create(
				wl_display_get_event_loop(sc->server->display));
			if (headless == NULL ||
					!wlr_multi_backend_add(sc->server->backend, headless) ||
					!wlr_backend_start(headless)) {
				*why = "vidow: 起不了 headless 后端（虚拟输出要它）";
				return NULL;
			}
			backend = headless;
		} else {
			*why = "vidow: 这个后端不支持虚拟输出";
			return NULL;
		}
		sc->virtual_backend = backend;
	}
	struct wlr_output *output = wlr_headless_add_output(backend,
		(unsigned)width, (unsigned)height);
	if (output == NULL) {
		*why = "vidow: 建不出虚拟输出";
		return NULL;
	}
	if (name != NULL && name[0] != '\0') {
		/* 名字已经公布出去了，改不了（wl_output.name 是「公布之后不能变」的）：
		 * 客户端看到的是 HEADLESS-N。请求里那个名字拿去当 PipeWire 节点的描述。 */
		wlr_log(WLR_INFO, "screencast: 虚拟输出叫 %s（请求的名字是「%s」）",
			output->name, name);
	}
	if (scale > 0 && scale != output->scale) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_scale(&state, scale);
		wlr_output_commit_state(output, &state);
		wlr_output_state_finish(&state);
	}
	wlr_log(WLR_INFO, "screencast: 加了块虚拟输出 %s（%dx%d，%.2fx）",
		output->name, width, height, scale);
	return output;
}

static void handle_virtual_output(struct wl_client *client,
		struct wl_resource *resource, uint32_t stream_id, const char *name,
		int32_t width, int32_t height, wl_fixed_t scale, uint32_t pointer_mode) {
	struct vidow_screencast *sc = wl_resource_get_user_data(resource);
	if (sc == NULL) {
		return;
	}
	uint32_t version = (uint32_t)wl_resource_get_version(resource);
	if (width <= 0 || height <= 0) {
		reply_failed(client, sc, resource, stream_id, "vidow: 虚拟输出没有尺寸");
		return;
	}
	double s = wl_fixed_to_double(scale);
	if (s <= 0) {
		s = 1.0;
	}
	const char *why = NULL;
	struct wlr_output *output = vidow_screencast_virtual_output(sc,
		name != NULL && name[0] != '\0' ? name : "VIDOW-VIRTUAL",
		(int)lround(width * s), (int)lround(height * s), s, &why);
	if (output == NULL) {
		reply_failed(client, sc, resource, stream_id,
			why != NULL ? why : "vidow: 建不出虚拟输出");
		return;
	}
	struct vidow_screencast_stream *stream = stream_create(client, sc,
		stream_id, version, output, pointer_mode);
	if (stream == NULL) {
		wlr_output_destroy(output);
		return;
	}
	stream->kind = VIDOW_SC_VIRTUAL;
	stream->owns_output = true;
	stream->out_w = output->width;
	stream->out_h = output->height;
	wlr_log(WLR_INFO, "screencast: 请求录虚拟输出 %s（%dx%d）",
		output->name, stream->out_w, stream->out_h);

	vidow_sc_pw_stream_start(stream);
}

static void handle_stream_virtual_output(struct wl_client *client,
		struct wl_resource *resource, uint32_t stream_id, const char *name,
		int32_t width, int32_t height, wl_fixed_t scale,
		uint32_t pointer_mode) {
	handle_virtual_output(client, resource, stream_id, name, width, height,
		scale, pointer_mode);
}

static void handle_stream_virtual_output_with_description(struct wl_client *client,
		struct wl_resource *resource, uint32_t stream_id, const char *name,
		const char *description, int32_t width, int32_t height,
		wl_fixed_t scale, uint32_t pointer_mode) {
	(void)description;
	handle_virtual_output(client, resource, stream_id, name, width, height,
		scale, pointer_mode);
}

static void handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zkde_screencast_unstable_v1_interface manager_impl = {
	.stream_output = handle_stream_output,
	.stream_window = handle_stream_window,
	.destroy = handle_destroy,
	.stream_virtual_output = handle_stream_virtual_output,
	.stream_region = handle_stream_region,
	.stream_virtual_output_with_description =
		handle_stream_virtual_output_with_description,
};

/* ------------------------------------------------------------ global */

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct vidow_screencast *sc = data;
	struct wl_resource *resource = wl_resource_create(client,
		&zkde_screencast_unstable_v1_interface, (int)version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, sc, NULL);
}

struct vidow_screencast *vidow_screencast_create(struct wl_display *display,
		struct vidow_server *server) {
	struct vidow_screencast *sc = calloc(1, sizeof(*sc));
	if (sc == NULL) {
		return NULL;
	}
	sc->server = server;
	wl_list_init(&sc->streams);

	sc->global = wl_global_create(display, &zkde_screencast_unstable_v1_interface,
		VIDOW_SC_VERSION, sc, manager_bind);
	if (sc->global == NULL) {
		free(sc);
		return NULL;
	}

	/* PipeWire 那一半（生产者）。连不上也照样暴露协议：KDE 的 portal 会因此看到
	 * ScreenCast 接口，每一路流拿到一个明确的 failed 原因（比「接口根本不存在」
	 * 好排查），而且录屏之外的接口不受影响。 */
	sc->pw = vidow_sc_pw_create(sc, display);
	if (sc->pw == NULL) {
		wlr_log(WLR_INFO, "screencast: PipeWire 没接上 —— 协议照样暴露，但每路流"
			"都会 failed（会话里跑着 pipewire 吗？）");
	}

	wlr_log(WLR_INFO, "screencast: zkde_screencast_unstable_v1 就绪"
		"（xdg-desktop-portal-kde 的录屏靠它）");
	return sc;
}
