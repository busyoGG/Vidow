/* screencopy.c — zwlr_screencopy_manager_v1（vidow 自己实现）。
 *
 * 为什么要自己写：wlroots 那份在客户端 copy 之后**把屏幕上那一帧的缓冲拷过去**
 * （从 output->swapchain 抓一张，blit 进客户端缓冲）。抓到的永远等于屏幕，所以
 * capture = "hide" 的窗口 / layer 藏不掉。这里换成：自己渲染一遍场景（渲染前把
 * 要藏的节点压成透明，见 capturemask.c），把这张图拷给客户端 —— 屏幕上那一帧
 * 完全不受影响。
 *
 * 节奏跟着 wlroots 那份：copy 之后**等下一次输出提交**再出帧（不是当场渲染）。
 * 这一点不能省：录屏客户端（wf-recorder）就是靠 ready 事件被节流的，当场出帧
 * 会让它按编码速度空转（实测过 2500fps 那种）。**节奏的主人是客户端**：它发
 * 一个 copy、我们回一个 ready，它再发下一个；capture_fps 不是「推帧频率」，
 * 只是「屏幕静止时多久替它催一次」的兜底间隔（见 output.c 的
 * capture_interval_ms，默认 60Hz，0 = 关掉兜底）。
 *
 * 支持：wl_shm 与 linux-dmabuf 目标缓冲、区域抓帧（capture_output_region）、
 * copy_with_damage 的 damage 事件、overlay_cursor（自己把光标图合成上去）。
 */
#define _GNU_SOURCE

#include "server.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <drm_fourcc.h>
#include <wayland-server-protocol.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>

#include "wlr-screencopy-unstable-v1-server-protocol.h"

#define SCREENCOPY_MANAGER_VERSION 3

struct vidow_screencopy {
	struct vidow_server *server;
	struct wl_global *global;
	struct wl_list frames;  /* vidow_screencopy_frame.link */
	struct wl_list clients; /* vidow_screencopy_client.link */
	struct wl_listener display_destroy;
};

/* ------------------------------------------------- 客户端（damage 记账用） */

struct vidow_screencopy_damage {
	struct wl_list link;
	struct wlr_output *output;
	pixman_region32_t damage;
	struct wl_listener output_precommit;
	struct wl_listener output_destroy;
};

struct vidow_screencopy_client {
	struct wl_list link;
	struct vidow_screencopy *manager;
	struct wl_list damages; /* vidow_screencopy_damage.link */
	int ref;
};

struct vidow_screencopy_frame {
	struct wl_list link; /* vidow_screencopy.frames */
	struct vidow_screencopy *manager;
	struct vidow_screencopy_client *client;
	struct wl_resource *resource;
	struct wlr_output *output;

	bool overlay_cursor;
	bool with_damage;
	bool copied;

	/* 要拷的区域（输出缓冲坐标，已经过 transform / scale 换算） */
	struct wlr_box box;
	uint32_t shm_format;
	uint32_t shm_stride;
	uint32_t dmabuf_format;

	struct wlr_buffer *buffer;
	uint32_t buffer_cap;

	struct wl_listener output_destroy;
	struct wl_listener output_commit;

	/* 抓帧那一段（重画整个场景 + 读回像素）**推迟到 idle 里做**。
	 *
	 * 原来它直接在 output 的 commit 监听器里同步跑：渲染器还在提交这一帧，
	 * 我们就插进去重画一遍整个场景，再 wlr_texture_read_pixels 等 GPU 把像素
	 * 读回 CPU。那条路径是同步的（glReadPixels 语义），整个合成器主循环就
	 * 停在那儿等 —— 屏幕上看到的是「截图的时候卡一下」，用户报的就是这个。
	 *
	 * 挪到 idle 之后：commit 监听器只挂一个 idle source 就返回，主循环立刻
	 * 回去处理输入 / 输出，抓帧在下一轮空闲时才跑。抓到的还是同一帧（我们
	 * 自己重画，见 vidow_capture_grab 的说明），所以像素内容不变。 */
	struct wl_event_source *idle;
};

static void frame_fail(struct vidow_screencopy_frame *frame);
static void frame_idle_grab(void *data);

/* ------------------------------------------------------------ damage 记账 */

static void damage_destroy(struct vidow_screencopy_damage *damage) {
	wl_list_remove(&damage->output_precommit.link);
	wl_list_remove(&damage->output_destroy.link);
	wl_list_remove(&damage->link);
	pixman_region32_fini(&damage->damage);
	free(damage);
}

static void damage_handle_output_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_screencopy_damage *damage =
		wl_container_of(listener, damage, output_destroy);
	damage_destroy(damage);
}

static void damage_handle_output_precommit(struct wl_listener *listener, void *data) {
	struct vidow_screencopy_damage *damage =
		wl_container_of(listener, damage, output_precommit);
	const struct wlr_output_event_precommit *event = data;
	struct wlr_output *output = damage->output;

	if (event->state->committed & WLR_OUTPUT_STATE_DAMAGE) {
		pixman_region32_union(&damage->damage, &damage->damage,
			&event->state->damage);
		pixman_region32_intersect_rect(&damage->damage, &damage->damage, 0, 0,
			output->width, output->height);
	} else if (event->state->committed & WLR_OUTPUT_STATE_BUFFER) {
		/* 没报 damage 就整屏算脏（保守，但客户端只会多编码一点）。 */
		pixman_region32_union_rect(&damage->damage, &damage->damage, 0, 0,
			output->width, output->height);
	}
}

static struct vidow_screencopy_damage *damage_get(
		struct vidow_screencopy_client *client, struct wlr_output *output) {
	struct vidow_screencopy_damage *damage;
	wl_list_for_each(damage, &client->damages, link) {
		if (damage->output == output) {
			return damage;
		}
	}
	damage = calloc(1, sizeof(*damage));
	if (damage == NULL) {
		return NULL;
	}
	damage->output = output;
	pixman_region32_init_rect(&damage->damage, 0, 0, output->width,
		output->height);
	wl_list_insert(&client->damages, &damage->link);
	damage->output_precommit.notify = damage_handle_output_precommit;
	wl_signal_add(&output->events.precommit, &damage->output_precommit);
	damage->output_destroy.notify = damage_handle_output_destroy;
	wl_signal_add(&output->events.destroy, &damage->output_destroy);
	return damage;
}

static struct vidow_screencopy_client *client_create(
		struct vidow_screencopy *manager) {
	struct vidow_screencopy_client *client = calloc(1, sizeof(*client));
	if (client == NULL) {
		return NULL;
	}
	client->manager = manager;
	wl_list_init(&client->damages);
	wl_list_insert(&manager->clients, &client->link);
	return client;
}

static void client_unref(struct vidow_screencopy_client *client) {
	if (--client->ref > 0) {
		return;
	}
	struct vidow_screencopy_damage *damage, *tmp;
	wl_list_for_each_safe(damage, tmp, &client->damages, link) {
		damage_destroy(damage);
	}
	wl_list_remove(&client->link);
	free(client);
}

static struct vidow_screencopy_client *client_from_resource(
		struct wl_resource *resource) {
	return wl_resource_get_user_data(resource);
}

/* ------------------------------------------------------------ 格式换算 */

/* wl_shm 的枚举值和 DRM fourcc 是两套编号，只有两个是特例：协议里
 * ARGB8888 = 0、XRGB8888 = 1 是硬编码的，**其余格式的值就是 DRM fourcc**
 * （WL_SHM_FORMAT_XBGR8888 == DRM_FORMAT_XBGR8888 == 0x34324258）。
 *
 * 而 wlr_buffer_begin_data_ptr_access() 给的 format 一律是 **DRM** 的（跟
 * wlr_texture_read_pixels 的 format 参数一个口径）。所以要比较这两个东西，就得
 * 先把特例那两个换过去；换错的表现是「有的客户端能用、有的永远 invalid buffer
 * format」：GLES2 的读回格式是 XBGR8888（值相同，蒙对了），pixman 是 XRGB8888
 * （1 vs 0x34325258，永远不等）。 */
static uint32_t shm_format_to_drm(uint32_t shm_format) {
	switch (shm_format) {
	case WL_SHM_FORMAT_ARGB8888: /* 0：协议特例，不是 DRM fourcc */
		return DRM_FORMAT_ARGB8888;
	case WL_SHM_FORMAT_XRGB8888: /* 1：同上 */
		return DRM_FORMAT_XRGB8888;
	default:
		return shm_format;
	}
}

static uint32_t drm_format_to_shm(uint32_t format) {
	switch (format) {
	case DRM_FORMAT_ARGB8888:
		return WL_SHM_FORMAT_ARGB8888;
	case DRM_FORMAT_XRGB8888:
		return WL_SHM_FORMAT_XRGB8888;
	default:
		/* 其余 32 位格式（XBGR8888 / ABGR8888 …）两边同值，原样发。 */
		return format;
	}
}

/* 上面这几个都是 32 位格式（wl_shm 侧也只有这几个有对应值）。 */
static uint32_t shm_format_stride(uint32_t width) {
	return width * 4;
}

/* ------------------------------------------------------------ 拷进目标缓冲 */

static bool copy_to_shm(struct vidow_screencopy_frame *frame,
		struct wlr_buffer *src) {
	struct wlr_renderer *renderer = frame->output->renderer;
	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(frame->buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
		return false;
	}

	struct wlr_texture *texture = wlr_texture_from_buffer(renderer, src);
	if (texture == NULL) {
		wlr_buffer_end_data_ptr_access(frame->buffer);
		return false;
	}

	bool ok = wlr_texture_read_pixels(texture,
		&(struct wlr_texture_read_pixels_options){
			.data = data,
			.format = format,
			.stride = stride,
			.src_box = frame->box,
		});
	wlr_texture_destroy(texture);
	wlr_buffer_end_data_ptr_access(frame->buffer);
	return ok;
}

static bool copy_to_dmabuf(struct vidow_screencopy_frame *frame,
		struct wlr_buffer *src) {
	struct wlr_renderer *renderer = frame->output->renderer;
	struct wlr_texture *texture = wlr_texture_from_buffer(renderer, src);
	if (texture == NULL) {
		return false;
	}
	struct wlr_render_pass *pass =
		wlr_renderer_begin_buffer_pass(renderer, frame->buffer, NULL);
	if (pass == NULL) {
		wlr_texture_destroy(texture);
		return false;
	}
	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
		.texture = texture,
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
		.dst_box = (struct wlr_box){
			.width = frame->buffer->width,
			.height = frame->buffer->height,
		},
		.src_box = (struct wlr_fbox){
			.x = frame->box.x,
			.y = frame->box.y,
			.width = frame->box.width,
			.height = frame->box.height,
		},
	});
	bool ok = wlr_render_pass_submit(pass);
	wlr_texture_destroy(texture);
	return ok;
}

/* -------------------------------------------------------------- 帧的生命周期 */

static struct vidow_screencopy_frame *frame_from_resource(
		struct wl_resource *resource) {
	return wl_resource_get_user_data(resource);
}

static void frame_destroy(struct vidow_screencopy_frame *frame) {
	if (frame == NULL) {
		return;
	}
	/* 资源可能还活着（我们出帧 / 失败之后客户端才 destroy）：先把 user_data
	 * 清掉，不然它拿着的是已经 free 的指针（踩过：段错误在 wl_list_remove）。 */
	if (frame->resource != NULL) {
		wl_resource_set_user_data(frame->resource, NULL);
	}
	wl_list_remove(&frame->output_destroy.link);
	wl_list_remove(&frame->output_commit.link);
	wl_list_remove(&frame->link);
	/* 还挂着的 idle 必须在这里销毁：它带着 frame 指针，frame 一 free 就是
	 * use-after-free（客户端截完就断开、输出刚好没了，都会走到这条路）。 */
	if (frame->idle != NULL) {
		wl_event_source_remove(frame->idle);
		frame->idle = NULL;
	}
	wlr_buffer_unlock(frame->buffer);
	if (frame->client != NULL) {
		client_unref(frame->client);
	}
	free(frame);
}

static void frame_handle_output_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_screencopy_frame *frame =
		wl_container_of(listener, frame, output_destroy);
	frame->output = NULL;
	/* 屏没了：这一帧永远等不到提交，别让客户端干等。 */
	frame_fail(frame);
	frame_destroy(frame);
}

static void frame_send_ready(struct vidow_screencopy_frame *frame,
		const struct timespec *when) {
	time_t tv_sec = when->tv_sec;
	uint32_t tv_sec_hi = (sizeof(tv_sec) > 4) ? (uint32_t)(tv_sec >> 32) : 0;
	uint32_t tv_sec_lo = (uint32_t)(tv_sec & 0xFFFFFFFF);
	zwlr_screencopy_frame_v1_send_ready(frame->resource, tv_sec_hi, tv_sec_lo,
		(uint32_t)when->tv_nsec);
}

static void frame_fail(struct vidow_screencopy_frame *frame) {
	/* 失败要说清理由（抓不到图 / 目标缓冲不合适 / 拷不进去）。协议上客户端
	 * 只收到一句 failed，wf-recorder 那边就打出来 "Failed to copy frame"，
	 * 没有这条日志根本分不出断在哪一步。 */
	wlr_log(WLR_ERROR, "screencopy: 出帧失败 output=%s cached=%d cap=%d "
		"buffer=%p（%d x %d）",
		frame->output != NULL && frame->output->name != NULL ?
			frame->output->name : "-",
		frame->copied, (int)frame->buffer_cap,
		(void *)frame->buffer,
		frame->buffer != NULL ? frame->buffer->width : 0,
		frame->buffer != NULL ? frame->buffer->height : 0);
	if (frame->resource != NULL) {
		zwlr_screencopy_frame_v1_send_failed(frame->resource);
	}
}

static void frame_send_damage(struct vidow_screencopy_frame *frame) {
	if (!frame->with_damage || frame->client == NULL || frame->output == NULL) {
		return;
	}
	struct vidow_screencopy_damage *damage =
		damage_get(frame->client, frame->output);
	if (damage == NULL) {
		return;
	}
	int n_rects = 0;
	const pixman_box32_t *rects =
		pixman_region32_rectangles(&damage->damage, &n_rects);
	for (int i = 0; i < n_rects; i++) {
		zwlr_screencopy_frame_v1_send_damage(frame->resource,
			(uint32_t)rects[i].x1, (uint32_t)rects[i].y1,
			(uint32_t)(rects[i].x2 - rects[i].x1),
			(uint32_t)(rects[i].y2 - rects[i].y1));
	}
	pixman_region32_clear(&damage->damage);
}

/* 输出提交了：现在出帧。抓帧图是**我们自己渲染**的（vidow_capture_grab 里把
 * 规则说了要藏的压下去、渲染、再放开），所以拷给客户端的东西和屏幕上那一帧
 * 可以不一样。 */
static void frame_handle_output_commit(struct wl_listener *listener, void *data) {
	struct vidow_screencopy_frame *frame =
		wl_container_of(listener, frame, output_commit);
	struct wlr_output_event_commit *event = data;
	struct wlr_output *output = frame->output;

	if (output == NULL) {
		return;
	}
	if ((event->state->committed & WLR_OUTPUT_STATE_ENABLED) && !output->enabled) {
		goto err;
	}
	if (!(event->state->committed & WLR_OUTPUT_STATE_BUFFER)) {
		return;
	}
	if (frame->buffer == NULL) {
		return;
	}

	/* copy_with_damage：没有新的 damage 就继续等（协议要求「等到有变化」）。 */
	if (frame->with_damage && frame->client != NULL) {
		struct vidow_screencopy_damage *damage =
			damage_get(frame->client, output);
		if (damage != NULL && pixman_region32_empty(&damage->damage)) {
			return;
		}
	}

	wl_list_remove(&frame->output_commit.link);
	wl_list_init(&frame->output_commit.link);

	/* 抓帧推迟到 idle：这一趟要重画整个场景 + 同步把像素读回 CPU，直接在这里
	 * 跑就把合成器主循环卡住了（见结构体里 idle 那段说明）。挂上就返回。 */
	if (frame->idle == NULL) {
		frame->idle = wl_event_loop_add_idle(
			wl_display_get_event_loop(frame->manager->server->display),
			frame_idle_grab, frame);
	}
	if (frame->idle == NULL) {
		goto err;
	}
	return;

err:
	frame_fail(frame);
	frame_destroy(frame);
}

/* idle 里真正抓帧（原来整段都在 frame_handle_output_commit 里，见那里的说明）。 */
static void frame_idle_grab(void *data) {
	struct vidow_screencopy_frame *frame = data;
	struct wlr_output *output = frame->output;

	frame->idle = NULL; /* wl_event_loop_add_idle 的 source 已经自己没了 */
	if (output == NULL || frame->buffer == NULL) {
		goto err;
	}

	/* 分段计时（VIDOW_CAPTURE_TRACE=1 才打）：截图慢 / 卡顿要能分清是
	 * 「重画整个场景」「把像素读回 CPU」「画进客户端缓冲」哪一段花的钱。 */
	bool trace = getenv("VIDOW_CAPTURE_TRACE") != NULL;
	int64_t t0 = trace ? vidow_now_ns() : 0;

	struct wlr_buffer *src =
		vidow_capture_grab(frame->manager->server, output);
	int64_t t1 = trace ? vidow_now_ns() : 0;
	if (src == NULL) {
		goto err;
	}
	if (frame->box.x < 0 || frame->box.y < 0 ||
			frame->box.x + frame->box.width > src->width ||
			frame->box.y + frame->box.height > src->height) {
		wlr_buffer_unlock(src);
		goto err;
	}

	if (frame->overlay_cursor) {
		vidow_capture_paint_cursor(frame->manager->server, output, src);
	}

	bool ok = frame->buffer_cap == WLR_BUFFER_CAP_DMABUF
		? copy_to_dmabuf(frame, src)
		: copy_to_shm(frame, src);
	int64_t t2 = trace ? vidow_now_ns() : 0;
	wlr_buffer_unlock(src);
	if (trace) {
		wlr_log(WLR_INFO, "CAPTRACE %s %dx%d %s: 场景渲染 %.2fms，"
			"拷进缓冲 %.2fms，合计 %.2fms",
			output->name, frame->box.width, frame->box.height,
			frame->buffer_cap == WLR_BUFFER_CAP_DMABUF ? "dmabuf" : "shm",
			(t1 - t0) / 1e6, (t2 - t1) / 1e6, (t2 - t0) / 1e6);
	}
	if (!ok) {
		goto err;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	zwlr_screencopy_frame_v1_send_flags(frame->resource, 0);
	frame_send_damage(frame);
	frame_send_ready(frame, &now);
	frame_destroy(frame);
	return;

err:
	frame_fail(frame);
	frame_destroy(frame);
}

/* ---------------------------------------------------------------- 请求 */

static void frame_handle_copy(struct wl_client *wl_client,
		struct wl_resource *frame_resource, struct wl_resource *buffer_resource) {
	(void)wl_client;
	struct vidow_screencopy_frame *frame = frame_from_resource(frame_resource);
	if (frame == NULL) {
		return;
	}
	if (frame->copied) {
		wl_resource_post_error(frame_resource,
			ZWLR_SCREENCOPY_FRAME_V1_ERROR_ALREADY_USED,
			"frame already used");
		return;
	}
	struct wlr_buffer *buffer = wlr_buffer_try_from_resource(buffer_resource);
	if (buffer == NULL) {
		wl_resource_post_error(frame_resource,
			ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
			"unsupported buffer type");
		return;
	}
	if (buffer->width != frame->box.width || buffer->height != frame->box.height) {
		wl_resource_post_error(frame_resource,
			ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
			"invalid buffer size");
		return;
	}

	struct wlr_dmabuf_attributes dmabuf;
	void *data;
	uint32_t format;
	size_t stride;
	if (wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
		if (frame->dmabuf_format == DRM_FORMAT_INVALID) {
			wl_resource_post_error(frame_resource,
				ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
				"dmabuf buffers are not supported");
			return;
		}
		if (dmabuf.format != frame->dmabuf_format) {
			wl_resource_post_error(frame_resource,
				ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
				"invalid buffer format");
			return;
		}
		frame->buffer_cap = WLR_BUFFER_CAP_DMABUF;
	} else if (wlr_buffer_begin_data_ptr_access(buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
		wlr_buffer_end_data_ptr_access(buffer);
		uint32_t want = shm_format_to_drm(frame->shm_format);
		if (format != want) {
			wlr_log(WLR_ERROR, "screencopy: 客户端缓冲格式 0x%x，要的是 0x%x"
				"（wl_shm 枚举 %u）", format, want, frame->shm_format);
			wl_resource_post_error(frame_resource,
				ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
				"invalid buffer format");
			return;
		}
		if (stride != frame->shm_stride) {
			wl_resource_post_error(frame_resource,
				ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
				"invalid buffer stride");
			return;
		}
		frame->buffer_cap = WLR_BUFFER_CAP_DATA_PTR;
	} else {
		wl_resource_post_error(frame_resource,
			ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
			"unsupported buffer type");
		return;
	}

	frame->buffer = wlr_buffer_lock(buffer);
	frame->copied = true;

	/* 等下一次输出提交再出帧（节奏 / 静止屏的问题见文件头）。 */
	frame->output_commit.notify = frame_handle_output_commit;
	wl_signal_add(&frame->output->events.commit, &frame->output_commit);

	/* **必须 schedule，不能只 update_needs_frame。**
	 *
	 * update_needs_frame 只发一个 needs_frame 信号、置一个标志，**不排帧**
	 * （见 wlroots 的 output.c：它只 wl_signal_emit，真正排帧的是
	 * wlr_output_schedule_frame）。Vidow 里也没有任何人监听 needs_frame，
	 * 所以那行等于什么都没做。
	 *
	 * 后果就是用户报的「截图要卡一会儿才截到」：屏幕静止时根本没有下一次
	 * 提交，frame_handle_output_commit 永远等不到；要等别的事件（鼠标动、
	 * 光标闪、客户端重绘）碰巧触发一帧，才连带把这张图发出去。动一下鼠标
	 * 截图就快、放着不动就慢 —— 就是这个。
	 *
	 * schedule_frame 会真的排一帧（进 wayland 的事件循环，走正常的渲染 →
	 * commit），commit 一到 frame_handle_output_commit 就跑。 */
	wlr_output_schedule_frame(frame->output);
}

static void frame_handle_copy_with_damage(struct wl_client *wl_client,
		struct wl_resource *frame_resource, struct wl_resource *buffer_resource) {
	struct vidow_screencopy_frame *frame = frame_from_resource(frame_resource);
	if (frame == NULL) {
		return;
	}
	frame->with_damage = true;
	frame_handle_copy(wl_client, frame_resource, buffer_resource);
}

static void frame_handle_destroy(struct wl_client *wl_client,
		struct wl_resource *frame_resource) {
	(void)wl_client;
	wl_resource_destroy(frame_resource);
}

static const struct zwlr_screencopy_frame_v1_interface frame_impl = {
	.copy = frame_handle_copy,
	.copy_with_damage = frame_handle_copy_with_damage,
	.destroy = frame_handle_destroy,
};

static void frame_handle_resource_destroy(struct wl_resource *frame_resource) {
	frame_destroy(frame_from_resource(frame_resource));
}

static void capture_output(struct wl_client *client,
		struct vidow_screencopy_client *sc_client, uint32_t version, uint32_t id,
		int32_t overlay_cursor, struct wlr_output *output,
		const struct wlr_box *box) {
	struct vidow_screencopy *manager = sc_client->manager;
	struct vidow_screencopy_frame *frame = calloc(1, sizeof(*frame));
	if (frame == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	frame->manager = manager;
	frame->client = sc_client;
	frame->output = output;
	frame->overlay_cursor = overlay_cursor != 0;
	frame->dmabuf_format = DRM_FORMAT_INVALID;
	/* 引用**先拿住**：frame_destroy() 一律会 unref，下面每条路（包括「输出不可用」
	 * 直接回 failed）都会走到它。放在后面的话，「输出不可用」那条路会白扣一次，
	 * 把 manager 资源还指着的这个 client 提前 free 掉 —— 下一次 capture_output
	 * 就在 wl_list_insert(&manager->frames) 上段错误（踩过：录屏 dialog 的缩略图
	 * 抓到一块正在销毁的输出，之后整个合成器崩）。 */
	sc_client->ref++;

	frame->resource = wl_resource_create(client,
		&zwlr_screencopy_frame_v1_interface, version, id);
	if (frame->resource == NULL) {
		wl_client_post_no_memory(client);
		frame->client = NULL;
		sc_client->ref--;
		free(frame);
		return;
	}
	wl_resource_set_implementation(frame->resource, &frame_impl, frame,
		frame_handle_resource_destroy);
	wl_list_init(&frame->output_commit.link);
	wl_list_init(&frame->output_destroy.link);
	wl_list_insert(&manager->frames, &frame->link);
	/* 有帧悬着了：把抓帧看门狗叫起来（见 output.c 的 capture_watchdog ——
	 * 静止的屏幕永远不提交，这一帧得我们自己推出来）。抓完没人等着它会自己停，
	 * 所以空载时这个定时器一次都不醒。 */
	vidow_output_capture_kick(manager->server);

	if (output == NULL || !output->enabled) {
		zwlr_screencopy_frame_v1_send_failed(frame->resource);
		frame_destroy(frame);
		return;
	}

	frame->output_destroy.notify = frame_handle_output_destroy;
	wl_signal_add(&output->events.destroy, &frame->output_destroy);

	struct wlr_renderer *renderer = output->renderer;
	if (!wlr_output_configure_primary_swapchain(output, NULL, &output->swapchain)) {
		goto error;
	}
	struct wlr_buffer *buffer = wlr_swapchain_acquire(output->swapchain);
	if (buffer == NULL) {
		goto error;
	}
	struct wlr_texture *texture = wlr_texture_from_buffer(renderer, buffer);
	wlr_buffer_unlock(buffer);
	if (texture == NULL) {
		goto error;
	}
	uint32_t read_format = wlr_texture_preferred_read_format(texture);
	wlr_texture_destroy(texture);

	if (read_format == DRM_FORMAT_INVALID) {
		/* 注意别拿 shm_format == 0 当「没有」判：ARGB8888 在 wl_shm 里就是 0。 */
		wlr_log(WLR_ERROR, "screencopy: 渲染器给不出读回格式");
		goto error;
	}
	frame->shm_format = drm_format_to_shm(read_format);
	if (output->allocator != NULL &&
			(output->allocator->buffer_caps & WLR_BUFFER_CAP_DMABUF)) {
		frame->dmabuf_format = output->render_format;
	}

	struct wlr_box buffer_box = {0};
	if (box == NULL) {
		buffer_box.width = output->width;
		buffer_box.height = output->height;
	} else {
		int ow, oh;
		wlr_output_effective_resolution(output, &ow, &oh);
		buffer_box = *box;
		wlr_box_transform(&buffer_box, &buffer_box,
			wlr_output_transform_invert(output->transform), ow, oh);
		buffer_box.x = (int)(buffer_box.x * output->scale);
		buffer_box.y = (int)(buffer_box.y * output->scale);
		buffer_box.width = (int)(buffer_box.width * output->scale);
		buffer_box.height = (int)(buffer_box.height * output->scale);
	}
	frame->box = buffer_box;
	frame->shm_stride = shm_format_stride(buffer_box.width);

	zwlr_screencopy_frame_v1_send_buffer(frame->resource, frame->shm_format,
		(uint32_t)buffer_box.width, (uint32_t)buffer_box.height,
		frame->shm_stride);
	if (version >= 3) {
		if (frame->dmabuf_format != DRM_FORMAT_INVALID) {
			zwlr_screencopy_frame_v1_send_linux_dmabuf(frame->resource,
				frame->dmabuf_format, (uint32_t)buffer_box.width,
				(uint32_t)buffer_box.height);
		}
		zwlr_screencopy_frame_v1_send_buffer_done(frame->resource);
	}
	return;

error:
	frame_fail(frame);
	frame_destroy(frame);
}

static void manager_handle_capture_output(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		int32_t overlay_cursor, struct wl_resource *output_resource) {
	struct vidow_screencopy_client *sc_client =
		client_from_resource(manager_resource);
	struct wlr_output *output = wlr_output_from_resource(output_resource);
	capture_output(client, sc_client, wl_resource_get_version(manager_resource),
		id, overlay_cursor, output, NULL);
}

static void manager_handle_capture_output_region(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		int32_t overlay_cursor, struct wl_resource *output_resource,
		int32_t x, int32_t y, int32_t width, int32_t height) {
	struct vidow_screencopy_client *sc_client =
		client_from_resource(manager_resource);
	struct wlr_output *output = wlr_output_from_resource(output_resource);
	if (width <= 0 || height <= 0) {
		wl_resource_post_error(manager_resource,
			ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
			"invalid region size");
		return;
	}
	struct wlr_box box = { .x = x, .y = y, .width = width, .height = height };
	if (output != NULL) {
		struct wlr_box full = { .width = output->width, .height = output->height };
		wlr_box_intersection(&box, &box, &full);
	}
	capture_output(client, sc_client, wl_resource_get_version(manager_resource),
		id, overlay_cursor, output, &box);
}

static void manager_handle_destroy(struct wl_client *client,
		struct wl_resource *manager_resource) {
	(void)client;
	wl_resource_destroy(manager_resource);
}

static const struct zwlr_screencopy_manager_v1_interface manager_impl = {
	.capture_output = manager_handle_capture_output,
	.capture_output_region = manager_handle_capture_output_region,
	.destroy = manager_handle_destroy,
};

static void manager_handle_resource_destroy(struct wl_resource *resource) {
	struct vidow_screencopy_client *client = client_from_resource(resource);
	if (client != NULL) {
		client_unref(client);
	}
}

static void manager_bind(struct wl_client *client, void *data, uint32_t version,
		uint32_t id) {
	struct vidow_screencopy *manager = data;
	uint32_t resource_version =
		version < SCREENCOPY_MANAGER_VERSION ? version : SCREENCOPY_MANAGER_VERSION;

	struct vidow_screencopy_client *sc_client = client_create(manager);
	if (sc_client == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	struct wl_resource *resource = wl_resource_create(client,
		&zwlr_screencopy_manager_v1_interface, resource_version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		client_unref(sc_client);
		return;
	}
	sc_client->ref++;
	wl_resource_set_implementation(resource, &manager_impl, sc_client,
		manager_handle_resource_destroy);
}

static void manager_handle_display_destroy(struct wl_listener *listener,
		void *data) {
	(void)data;
	struct vidow_screencopy *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}
/* 「这一块屏上有没有还没出完的帧」。和上面那个（只看「全局有没有」）的区别：
 * capture_watchdog 现在按屏推帧，全局有一条就整屏标脏重画所有输出的话，
 * 双屏时抓 A 屏会把 B 屏也白白重画一遍（B 屏上根本没有客户端在等）。
 *
 * 只认**真的在等提交**的帧：copy 之后 output_commit 才挂上（见 frame_handle_copy），
 * 挂上之前客户端还在等 buffer 事件、还没交缓冲，这时候不用我们推。已经挂上的那些
 * 一定是等这块屏的下一次 commit。 */
static bool frame_pending_on(struct vidow_screencopy_frame *frame,
		struct wlr_output *output) {
	if (frame == NULL || output == NULL) {
		return false;
	}
	if (frame->output != output) {
		return false;
	}
	/* copy 之后才挂上 output_commit（见 frame_handle_copy），没挂上的帧客户端还在
	 * 等 buffer 事件、还没交缓冲，不需要我们推。判断「挂上没有」不能看
	 * wl_list_empty：capture_output 里 wl_list_init 过的空 listener 也是「空链表」
	 * 形态，两种状态分不开。self 指针自己指自己 = 还没挂。 */
	return frame->output_commit.link.next != &frame->output_commit.link;
}

bool vidow_screencopy_has_pending_for_output(struct vidow_screencopy *manager,
		struct wlr_output *output) {
	/* 不算「输出关着」这一条：关着的屏确实不该推帧，但那是 capture_watchdog 那边
	 * 的职责（vidow_output_render 自己会早退，关屏的帧也等不到 commit，输出销毁时
	 * frame_handle_output_destroy 会回 failed）。这里只回答「谁在等这块屏」。 */
	if (manager == NULL || output == NULL) {
		return false;
	}
	struct vidow_screencopy_frame *frame;
	wl_list_for_each(frame, &manager->frames, link) {
		if (frame_pending_on(frame, output)) {
			return true;
		}
	}
	return false;
}

struct vidow_screencopy *vidow_screencopy_create(struct wl_display *display,
		struct vidow_server *server) {
	struct vidow_screencopy *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}
	manager->server = server;
	wl_list_init(&manager->frames);
	wl_list_init(&manager->clients);
	manager->global = wl_global_create(display, &zwlr_screencopy_manager_v1_interface,
		SCREENCOPY_MANAGER_VERSION, manager, manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}
	manager->display_destroy.notify = manager_handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);
	return manager;
}
