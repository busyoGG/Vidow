/* capture.c — vidow 自己出帧的输出抓帧：ext-image-copy-capture 的「输出抓帧源」
 * （ext_output_image_capture_source_manager_v1）以及共用的抓帧引擎。
 *
 * 为什么要自己实现：wlroots 的两条抓帧路都是**把屏幕上那一帧的缓冲直接拷给客户端**
 *   - screencopy：从 output->swapchain 抓一张，blit 进客户端缓冲；
 *   - ext-image-copy-capture 的输出源：监听 output->events.commit，把 state->buffer 拷走。
 * 也就是说「抓帧 == 屏幕那一帧」在 wlroots 里是写死的。想让 capture = "hide" 的窗口
 * / layer **只在抓帧里消失、屏幕上照常显示**，没有别的口子 —— 只能自己出帧：
 * 自己把场景渲染一遍（渲染前把要藏的节点透明度压 0），再把这张图交给客户端。
 *
 * 一次抓帧的流程（同步，不排队）：
 *   1. vidow_capturemask_hold()：引用计数地把遮挡压上（capturemask.c）
 *   2. wlr_scene_output_build_state()：渲染进**我们自己的** scene_output
 *   3. vidow_capturemask_release()：放开遮挡
 *   4. 把渲染出来的缓冲拷进客户端缓冲（wlr_ext_image_copy_capture_frame_v1_copy_buffer）
 *
 * 为什么另开一个 scene_output（配一个假输出）而不是复用屏幕那个：scene_output 里有
 * 个 damage ring，那是「这块缓冲上哪些区域是新的」的账本，和具体缓冲一一对应。
 * 抓帧的缓冲和屏幕的缓冲不是同一块，两本账混在一起会让屏幕少画一块（屏幕上留下
 * 上一帧的残影）。假输出 + 自己的 scene_output 把两本账彻底分开 —— wlroots 自己的
 * 窗口抓帧源（types/ext_image_capture_source_v1/scene.c）也是这么干的。
 *
 * 已知取舍：
 *   - 抓到的帧里没有光标（不合成光标平面）。要光标的客户端（grim -c）走
 *     ext_image_copy_capture_cursor_session_v1 自己把光标画上去，那条路这里也实现了。
 *   - 输出转屏（transform 90/270）时按正立方向出图，尺寸跟着真输出走；旋转屏上
 *     的取景可能不对（vidow 的转屏支持本来就只覆盖常见情形）。
 */
#define _GNU_SOURCE

#include "server.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <wlr/backend.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/interfaces/wlr_output.h>
#include <drm_fourcc.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include <spa/param/video/raw.h>

#include "ext-image-capture-source-v1-server-protocol.h"

/* ---------------------------------------------------------------- 抓帧引擎 */

struct vidow_capture {
	struct vidow_server *server;
	struct wlr_output *output;   /* 真输出：尺寸 / 缩放 / 位置都看它 */

	/* 假后端 + 假输出：只是给 scene_output 一个落脚的地方，永远不真的提交。
	 * （enabled 一直是 false，所以它不会被算成「窗口的主输出」，不影响客户端的
	 * dmabuf feedback / 直出判定。） */
	struct wlr_backend backend;
	struct wlr_output grab_output;
	struct wlr_scene_output *scene_output;

	/* 输出抓帧源（客户端 create_source 时才建） */
	struct vidow_capture_source *source;

	/* 马赛克用的小块缓冲（缩小后的那一张），一块屏一个、按需重建 */
	struct wlr_buffer *mosaic;
	int mosaic_w, mosaic_h;

	/* 光标源（pointer cursor session 用） */
	struct vidow_cursor_source *cursor;

	struct wl_listener output_destroy;
	struct wl_listener output_commit;
};

struct vidow_cursor_source;

static struct vidow_capture *capture_create(struct vidow_server *server,
		struct wlr_output *output);

static struct vidow_output *vidow_output_for(struct vidow_server *server,
		struct wlr_output *wlr_output) {
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->wlr_output == wlr_output) {
			return output;
		}
	}
	return NULL;
}

/* 拿这块屏的抓帧器（没有就建）。 */
static struct vidow_capture *capture_for_output(struct vidow_server *server,
		struct wlr_output *wlr_output) {
	struct vidow_output *output = vidow_output_for(server, wlr_output);
	if (output == NULL) {
		return NULL;
	}
	if (output->capture == NULL) {
		output->capture = capture_create(server, wlr_output);
	}
	return output->capture;
}

/* 等这一帧的 GPU 真的画完（把命令流冲干净）。
 *
 * 为什么直通那条路必须等：这块缓冲的内存是**直接交给消费者**的，而消费者
 * （GStreamer 的 pipewiresrc、浏览器那套）常常是 mmap 下来在 CPU 上读 —— 它没有
 * 任何办法知道 GPU 写完没有。不等的话它读到的是**还没画进去的那张**：实测 gst
 * 那边六帧全白（刚分配出来的显存就是白的），而同一张缓冲走回读就是对的（因为
 * glReadPixels 本身就是同步点）。
 *
 * 读 1×1 一个像素是「把命令流冲干净」最省事的做法：数据量可以忽略，代价是等 GPU
 * 把这一帧画完 —— 那笔钱本来就要花，只是从「消费者那边读不对」变成「我们这边等」。
 * 真要做到不等，得给消费者带**显式同步**（dma-fence / syncobj，见 SPA_DATA_SyncObj），
 * 那是另一件事。 */
void vidow_grab_sync(struct wlr_renderer *renderer, struct wlr_buffer *buffer) {
	struct wlr_texture *texture = wlr_texture_from_buffer(renderer, buffer);
	if (texture == NULL) {
		return;
	}
	uint32_t px = 0;
	wlr_texture_read_pixels(texture, &(struct wlr_texture_read_pixels_options){
		.data = &px,
		.format = DRM_FORMAT_XRGB8888,
		.stride = 4,
		.src_box = (struct wlr_box){ .width = 1, .height = 1 },
	});
	wlr_texture_destroy(texture);
}

/* 录屏直通那条路要一张 **linear** 交换链（出一张 dmabuf 交给消费者）。
 *
 * 为什么要 linear：dmabuf 的排布（修饰符）在 SPA 里要单独协商，而报修饰符会把
 * GStreamer 的 pipewiresrc 的 caps 协商整死（实测 handle_format_change 直接断言
 * 失败、一帧都拿不到）；**不报**修饰符时消费者的默认假设就是 linear（modifier 0）
 * —— 所以让抓帧渲染进一张 linear 缓冲，两边默认值就对上了，一个修饰符都不用谈。
 *
 * 建不出来（pixman / shm 分配器根本没有 dmabuf）返回 NULL，调用方退回老路
 * （消费者按 memfd 分配 + 回读）。 */
struct wlr_swapchain *vidow_grab_target_new(struct vidow_server *server,
		int width, int height) {
	if (width <= 0 || height <= 0) {
		return NULL;
	}
	struct wlr_drm_format_set set = {0};
	struct wlr_swapchain *sc = NULL;
	if (wlr_drm_format_set_add(&set, DRM_FORMAT_XRGB8888,
			DRM_FORMAT_MOD_LINEAR)) {
		sc = wlr_swapchain_create(server->allocator, width, height,
			set.formats);
	}
	wlr_drm_format_set_finish(&set);
	return sc;
}

/* 渲染场景里的一个矩形，返回**加锁的**缓冲（调用方负责 wlr_buffer_unlock）。遮挡
 * 由调用方负责压好（vidow_capturemask_hold/release）。
 *
 * box 是**布局坐标**（逻辑坐标，不是像素），scale 是「逻辑 → 像素」的换算，
 * 出图尺寸 = round(box.width * scale) × round(box.height * scale)。整屏抓帧就是
 * 「box = 这块屏在布局里的框、scale = 这块屏的缩放」，按区域抓帧是请求里那个框。
 * 按窗口抓帧不走这儿（窗口单独渲染，见 wincapture.c）。
 *
 * target 非空 = 渲染进**调用方那张** linear 交换链（录屏 dmabuf 直通），空 = 渲染
 * 进抓帧自己的交换链（老行为，给截图 / 按窗口抓帧用）。 */
/* 同上，但出图尺寸**给死**（<=0 表示按 box × scale 算）。整屏抓帧用给死的那条：
 * 缩放不是整数时「逻辑框 × 缩放」round 回来可能差一两个像素，而客户端缓冲是照
 * 输出尺寸建的（见 capture_render）。 */
static struct wlr_buffer *capture_render_box_size(struct vidow_capture *cap,
		const struct wlr_box *box, double scale, int width, int height,
		struct wlr_swapchain *target) {
	if (scale <= 0) {
		return NULL;
	}
	if (width <= 0 || height <= 0) {
		width = (int)lround(box->width * scale);
		height = (int)lround(box->height * scale);
	}
	if (width <= 0 || height <= 0) {
		return NULL;
	}

	/* 缩放决定「逻辑坐标 → 像素」的换算，和真输出不一致的话抓出来的图会错位
	 * （高分屏上最明显）。位置同理：场景是按布局坐标排的。 */
	cap->grab_output.scale = scale;
	wlr_scene_output_set_position(cap->scene_output, box->x, box->y);

	/* 假输出的尺寸得手动跟上：场景的 damage 记账是按 output->width/height 裁的
	 * （scene_output_damage 里 intersect），假输出从来没提交过模式，这两个值
	 * 一直是 0 —— damage 全被裁光，抓帧只有第一帧是新的，之后拿到的都是
	 * swapchain 里放着的旧图（踩过：把规则从 hide 改成 off，抓帧还是黑的）。
	 * 直接写字段（不 commit）：commit 会让它变成「enabled 的输出」，抢走窗口的
	 * 主输出判定，客户端的 dmabuf feedback 就跟着变了。 */
	cap->grab_output.width = width;
	cap->grab_output.height = height;

	/* 每次抓帧都整屏重画：这块假输出没有输出级的 damage 记账，整屏重画最省心，
	 * 一次抓帧本来也要读整屏。掩码那点 damage 也就不会算漏。 */
	pixman_region32_t whole;
	pixman_region32_init_rect(&whole, 0, 0, width, height);
	wlr_damage_ring_add(&cap->scene_output->damage_ring, &whole);
	pixman_region32_fini(&whole);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_state_set_custom_mode(&state, width, height, 0);

	/* 出图目标只认**尺寸正好对上**的那张：交换链是调用方按流尺寸建的，对不上
	 * （比如消费者协商了个别的尺寸）就不能拿它当渲染目标 —— 渲染器会按 state
	 * 的模式去配，配不上那一趟直接失败。对不上就不给目标，调用方自己看着办
	 * （抓帧自己的交换链 + 回读）。 */
	struct wlr_swapchain *sc = NULL;
	if (target != NULL && target->width == width &&
			target->height == height) {
		sc = target;
	}
	struct wlr_scene_output_state_options opts = { .swapchain = sc };

	struct wlr_buffer *buffer = NULL;
	if (wlr_scene_output_build_state(cap->scene_output, &state,
			sc != NULL ? &opts : NULL)) {
		buffer = state.buffer;
		if (buffer != NULL) {
			wlr_buffer_lock(buffer);
		}
	}
	wlr_output_state_finish(&state);

	if (buffer == NULL) {
		if (sc != NULL) {
			/* 直通那条路的交换链只有 4 张，全被消费者攥着的时候这一帧
			 * 出不来 —— 不是错误，下一帧就好了。 */
			wlr_log(WLR_DEBUG, "抓帧: %s 直通交换链没空闲缓冲，这帧跳过",
				cap->output->name);
		} else {
			wlr_log(WLR_ERROR, "抓帧: %s 渲染失败", cap->output->name);
		}
	}
	return buffer;
}

/* 整块输出（屏幕那一帧的大小）。
 *
 * 出图尺寸**直接取 output->width/height**，不从「布局里的逻辑框 × 缩放」倒算：
 * 缩放不是整数时（比如 1.7）逻辑框是取整过的（1280/1.7 = 753），753 × 1.7 = 1280.1
 * 再 round 回去可能差一两个像素（1278），而客户端缓冲是照 `buffer` 事件里报的
 * 1280x720 建的 —— 尺寸对不上，拷贝那一步就失败（踩过：scale=1.7 的屏上
 * grim 全挂，test-blurscale.sh 第一条断言红）。按区域 / 按窗口抓帧没这个问题：
 * 那种流的尺寸本来就是「框 × 缩放」算出来的，两边同一个来源。 */
static struct wlr_buffer *capture_render(struct vidow_capture *cap,
		struct wlr_swapchain *target) {
	struct wlr_output *output = cap->output;
	if (output->width <= 0 || output->height <= 0) {
		return NULL;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(cap->server->output_layout, output, &box);
	return capture_render_box_size(cap, &box, output->scale, output->width,
		output->height, target);
}

/* 按 box × scale 算出图尺寸的那条（区域 / 窗口抓帧用）。 */
static struct wlr_buffer *capture_render_box(struct vidow_capture *cap,
		const struct wlr_box *box, double scale, struct wlr_swapchain *target) {
	return capture_render_box_size(cap, box, scale, 0, 0, target);
}

/* 抓帧的**通用入口**：box 为 NULL = 整块输出，否则是那块屏上的一个矩形。
 *
 * target 非空 = 录屏直通：渲染进调用方那张 linear 交换链，出图能直接当 dmabuf
 * 交给消费者（见 vidow_grab_target_get）。target 为空 = 老行为（抓帧自己的
 * 交换链），截图 / screencopy / ext-copy 都走这条。 */
struct wlr_buffer *vidow_capture_grab_into(struct vidow_server *server,
		struct wlr_output *output, const struct wlr_box *box, double scale,
		struct wlr_swapchain *target) {
	if (output == NULL) {
		return NULL;
	}
	if (box != NULL && (box->width <= 0 || box->height <= 0)) {
		return NULL;
	}
	struct vidow_capture *cap = capture_for_output(server, output);
	if (cap == NULL) {
		return NULL;
	}
	vidow_capturemask_hold(server);
	struct wlr_buffer *buffer = box != NULL ?
		capture_render_box(cap, box, scale, target) :
		capture_render(cap, target);
	if (buffer != NULL && box == NULL) {
		/* 遮挡把原图压掉了，这里再把「块状版本」画回去（capture = "mosaic"）。
		 * 放在这里两条抓帧路（screencopy / ext-copy）就都盖到了。 */
		vidow_capture_paint_mosaic(server, output, buffer);
	}
	vidow_capturemask_release(server);
	return buffer;
}

/* 抓一块屏幕上的任意矩形（布局坐标）。给「按区域录屏 / 截图」用。 */
struct wlr_buffer *vidow_capture_grab_box(struct vidow_server *server,
		struct wlr_output *output, const struct wlr_box *box, double scale) {
	return vidow_capture_grab_into(server, output, box, scale, NULL);
}

/* --------------------------------------------------------------- 光标 */

/* 光标的两种用法都从 wlr_output_cursor 取状态（硬件平面上的那个也是它），所以放大
 * 摇晃那种大光标、客户端自己设的光标都拿得到，和屏幕上看到的一致：
 *   1. 合成进画面（ext-copy 的 PAINT_CURSORS / screencopy 的 overlay_cursor /
 *      录屏的 pointer_mode=embedded）—— vidow_capture_paint_cursor*；
 *   2. 只报「在哪、长什么样」（录屏的 pointer_mode=metadata，走 PipeWire 缓冲的
 *      SPA_META_Cursor），画面里不带光标 —— vidow_capture_cursor_info。
 *
 * 坐标有两套，混起来就是错位：
 *   - wlr_output_cursor 的 x/y 是**这块屏自己的像素坐标**（wlroots 在
 *     wlr_output_cursor_move 里已经乘过 output->scale 了）；
 *   - 这里的 box 是**布局坐标**里的抓帧框，scale 是「布局逻辑 → 抓帧像素」。
 * 两个缩放可以不一样：整屏 / 区域抓帧时它等于 output->scale，按窗口录屏时是
 * 「窗口等比缩放进流」的那个倍率（跟屏的缩放无关，见 vidow_win_grab_frame_box）。
 * 所以先把光标换回布局坐标，再乘抓帧的缩放 —— 直接用 oc->x 当布局坐标的话，
 * 非 1 倍屏上会偏（第二块屏上直接画到框外）。 */
static void cursor_dst_pos(struct vidow_server *server, struct wlr_output *output,
		const struct wlr_box *box, double scale, struct wlr_output_cursor *oc,
		int *x, int *y) {
	struct wlr_box obox;
	wlr_output_layout_get_box(server->output_layout, output, &obox);
	double oscale = output->scale > 0 ? output->scale : 1.0;
	double lx = obox.x + oc->x / oscale;
	double ly = obox.y + oc->y / oscale;
	*x = (int)lround((lx - box->x) * scale);
	*y = (int)lround((ly - box->y) * scale);
	wlr_log(WLR_DEBUG, "cursor_dst_pos: oc=%.1f,%.1f oscale=%.2f obox=%d,%d "
		"box=%d,%d scale=%.2f -> %d,%d", oc->x, oc->y, oscale, obox.x, obox.y,
		box->x, box->y, scale, *x, *y);
}

/* 把这块屏上的光标合成到抓帧图上（ext-copy 的 PAINT_CURSORS、screencopy 的
 * overlay_cursor）。抓帧是场景渲染，场景里没有光标 —— 光标是输出自己的光标平面
 * 或者后端在提交时合上去的，所以要光标的客户端得我们自己画。
 *
 * box 是**布局坐标**里的框（整屏抓帧 = 这块屏在布局里占的那一块），scale 是
 * 「逻辑 → 像素」；光标在框外就不画（按区域 / 按窗口抓帧时，光标跑到别处去了就
 * 不该出现在这张图里）。 */
void vidow_capture_paint_cursor_box(struct vidow_server *server,
		struct wlr_output *output, struct wlr_buffer *dst,
		const struct wlr_box *box, double scale) {
	if (scale <= 0) {
		scale = 1.0;
	}
	struct wlr_output_cursor *oc;
	wl_list_for_each(oc, &output->cursors, link) {
		if (!oc->enabled || !oc->visible || oc->texture == NULL) {
			continue;
		}
		int cx, cy;
		cursor_dst_pos(server, output, box, scale, oc, &cx, &cy);
		struct wlr_box dst_box = {
			.x = cx - oc->hotspot_x,
			.y = cy - oc->hotspot_y,
			.width = (int)oc->width,
			.height = (int)oc->height,
		};
		if (dst_box.x + dst_box.width <= 0 || dst_box.y + dst_box.height <= 0 ||
				dst_box.x >= dst->width || dst_box.y >= dst->height) {
			continue;
		}
		struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
			output->renderer, dst, NULL);
		if (pass == NULL) {
			return;
		}
		wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
			.texture = oc->texture,
			.src_box = oc->src_box,
			.dst_box = dst_box,
			.transform = oc->transform,
		});
		wlr_render_pass_submit(pass);
	}
}

/* 整屏抓帧那条路：框就是这块屏在布局里占的那一块。 */
void vidow_capture_paint_cursor(struct vidow_server *server,
		struct wlr_output *output, struct wlr_buffer *dst) {
	double scale = output->scale > 0 ? output->scale : 1.0;
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	vidow_capture_paint_cursor_box(server, output, dst, &box, scale);
}

/* ------------------------------------------------- 光标元数据（录屏 metadata）

 * pointer_mode=metadata：画面里不带光标，改成把「在哪 + 热点 + 长什么样」交给
 * 消费者自己合成（PipeWire 缓冲的 SPA_META_Cursor）。位置 / 热点好办，位图得从
 * 贴图里读回来：
 *   - oc->src_box 是**贴图像素**里的源矩形（surface 带 viewport / buffer_scale 时
 *     它不等于整张贴图）；
 *   - oc->width/height 是**输出像素**里的目标尺寸（wlroots 已经乘过 scale）。
 * 常见情况两者一样大（1:1），缩放了就按最近邻重采样。
 *
 * 已知取舍：oc->transform 不是 NORMAL（客户端给光标 surface 设了
 * wl_surface.set_buffer_transform）时不报位图，只报位置 / 热点 —— 旋转过的光标图
 * 自己转回来容易转错方向，宁可不给（位置还是对的）。真实客户端都不设这个。 */
static bool cursor_read_pixels(struct wlr_texture *texture, const struct wlr_fbox *src,
		void *data, int w, int h, uint32_t format) {
	return wlr_texture_read_pixels(texture, &(struct wlr_texture_read_pixels_options){
		.data = data,
		.format = format,
		.stride = (uint32_t)w * 4,
		.src_box = (struct wlr_box){
			.x = (int)src->x,
			.y = (int)src->y,
			.width = w,
			.height = h,
		},
	});
}

/* 读回来的字节序 → SPA_VIDEO_FORMAT_RGBA（内存里 R,G,B,A，预乘）。
 * 8888 那四个 fourcc 在内存里的排列：ARGB/XRGB = B,G,R,A/X；ABGR/XBGR = R,G,B,A/X
 * （fourcc 的第一个字母是低位，小端下就落在最后一个字节）。 */
static void cursor_to_rgba(uint8_t *dst, const uint8_t *src, size_t n,
		uint32_t format) {
	bool swap_rb = format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888;
	bool has_alpha = format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_ABGR8888;
	for (size_t i = 0; i < n; i++, dst += 4, src += 4) {
		dst[0] = swap_rb ? src[2] : src[0];
		dst[1] = src[1];
		dst[2] = swap_rb ? src[0] : src[2];
		dst[3] = has_alpha ? src[3] : 0xff;
	}
}

bool vidow_capture_cursor_info(struct vidow_server *server, struct wlr_output *output,
		const struct wlr_box *box, double scale, struct vidow_cursor_info *info,
		void *pixels, size_t pixels_size) {
	if (server == NULL || output == NULL || box == NULL || info == NULL) {
		return false;
	}
	if (scale <= 0) {
		scale = 1.0;
	}
	memset(info, 0, sizeof(*info));

	struct wlr_output_cursor *oc = NULL, *it;
	wl_list_for_each(it, &output->cursors, link) {
		if (it->enabled && it->visible && it->texture != NULL) {
			oc = it;
			break;
		}
	}
	if (oc == NULL) {
		return false;
	}

	cursor_dst_pos(server, output, box, scale, oc, &info->x, &info->y);
	info->hotspot_x = oc->hotspot_x;
	info->hotspot_y = oc->hotspot_y;

	int dw = (int)oc->width, dh = (int)oc->height;
	int sw = (int)lround(oc->src_box.width > 0 ? oc->src_box.width : dw);
	int sh = (int)lround(oc->src_box.height > 0 ? oc->src_box.height : dh);
	if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) {
		return true; /* 位置 / 热点有，位图没有 */
	}
	if (oc->transform != WL_OUTPUT_TRANSFORM_NORMAL) {
		return true;
	}
	/* 位图上限：SPA_META_Cursor 的空间在建流时按 256×256 一次性声明，比这大的
	 * 光标（客户端能设任意大的 cursor surface）就只报位置。 */
	if (dw > VIDOW_CURSOR_MAX_PX || dh > VIDOW_CURSOR_MAX_PX ||
			sw > VIDOW_CURSOR_MAX_PX || sh > VIDOW_CURSOR_MAX_PX) {
		return true;
	}
	if ((size_t)dw * dh * 4 > pixels_size) {
		return true;
	}

	/* 先按 1:1 读进临时缓冲（贴图像素），再重采样到输出像素尺寸。 */
	uint8_t *tmp = malloc((size_t)sw * sh * 4);
	if (tmp == NULL) {
		return true;
	}
	uint32_t format = wlr_texture_preferred_read_format(oc->texture);
	bool ok = cursor_read_pixels(oc->texture, &oc->src_box, tmp, sw, sh, format);
	if (!ok && format != DRM_FORMAT_ARGB8888) {
		format = DRM_FORMAT_ARGB8888;
		ok = cursor_read_pixels(oc->texture, &oc->src_box, tmp, sw, sh, format);
	}
	if (!ok) {
		free(tmp);
		return true;
	}

	uint8_t *out = pixels;
	for (int y = 0; y < dh; y++) {
		int sy = y * sh / dh;
		for (int x = 0; x < dw; x++) {
			int sx = x * sw / dw;
			cursor_to_rgba(out + ((size_t)y * dw + x) * 4,
				tmp + ((size_t)sy * sw + sx) * 4, 1, format);
		}
	}
	free(tmp);

	info->width = dw;
	info->height = dh;
	info->format = SPA_VIDEO_FORMAT_RGBA;
	return true;
}

/* --------------------------------------------------------------- 马赛克 */

/* 把画在屏幕上的那张图缩小到 1/block，再用 NEAREST 放大回原尺寸：一个块里所有
 * 像素取同一个采样点，看着就是马赛克。缩小那一遍用默认的线性过滤（等于块内平均），
 * 放大那一遍必须 NEAREST（不然又糊回原样）。 */
static void paint_mosaic_one(struct vidow_capture *cap, struct wlr_scene_buffer *sb,
		struct wlr_buffer *dst, int block) {
	if (sb == NULL || sb->buffer == NULL || block < 2) {
		return;
	}
	int lx, ly;
	if (!wlr_scene_node_coords(&sb->node, &lx, &ly)) {
		return; /* 没画在画面上 */
	}
	int lw = sb->dst_width > 0 ? sb->dst_width : (int)sb->buffer->width;
	int lh = sb->dst_height > 0 ? sb->dst_height : (int)sb->buffer->height;
	if (lw <= 0 || lh <= 0) {
		return;
	}
	struct wlr_output *output = cap->output;
	struct vidow_server *server = cap->server;
	double scale = output->scale > 0 ? output->scale : 1.0;
	struct wlr_box obox;
	wlr_output_layout_get_box(server->output_layout, output, &obox);
	int dw = (int)lround(lw * scale), dh = (int)lround(lh * scale);
	int dx = (int)lround((lx - obox.x) * scale);
	int dy = (int)lround((ly - obox.y) * scale);
	if (dx >= dst->width || dy >= dst->height || dx + dw <= 0 || dy + dh <= 0) {
		return; /* 整块都在抓帧区域外 */
	}
	int bw = dw / block, bh = dh / block;
	if (bw < 1) {
		bw = 1;
	}
	if (bh < 1) {
		bh = 1;
	}

	if (cap->mosaic == NULL || cap->mosaic_w != bw || cap->mosaic_h != bh) {
		wlr_buffer_unlock(cap->mosaic);
		cap->mosaic = NULL;
		/* 格式跟着抓帧输出自己的 swapchain 走（那是渲染器认的渲染目标格式；
		 * 别自己拼 wlr_drm_format —— 那套 create/add 是 wlroots 内部 API）。 */
		const struct wlr_drm_format *fmt =
			cap->grab_output.swapchain != NULL ?
				&cap->grab_output.swapchain->format : NULL;
		if (fmt == NULL) {
			return;
		}
		struct wlr_buffer *small =
			wlr_allocator_create_buffer(server->allocator, bw, bh, fmt);
		if (small == NULL) {
			return;
		}
		/* 自己拿一把锁：allocator 给的是没锁的缓冲，下次重建时要 unlock。
		 * allocator 返回的是 producer 引用；只 lock 不 drop 的话这份引用会一直
		 * 挂着，每次 mosaic 尺寸变化就漏一块 GPU 缓冲。 */
		cap->mosaic = wlr_buffer_lock(small);
		wlr_buffer_drop(small);
		cap->mosaic_w = bw;
		cap->mosaic_h = bh;
	}

	struct wlr_texture *tex =
		wlr_texture_from_buffer(server->renderer, sb->buffer);
	if (tex == NULL) {
		return;
	}
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(server->renderer,
		cap->mosaic, NULL);
	if (pass == NULL) {
		wlr_texture_destroy(tex);
		return;
	}
	struct wlr_fbox src = sb->src_box;
	if (src.width <= 0 || src.height <= 0) {
		src = (struct wlr_fbox){ .width = tex->width, .height = tex->height };
	}
	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
		.texture = tex,
		.src_box = src,
		.dst_box = (struct wlr_box){ .width = bw, .height = bh },
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	});
	wlr_render_pass_submit(pass);
	wlr_texture_destroy(tex);

	struct wlr_texture *small =
		wlr_texture_from_buffer(server->renderer, cap->mosaic);
	if (small == NULL) {
		return;
	}
	pass = wlr_renderer_begin_buffer_pass(server->renderer, dst, NULL);
	if (pass == NULL) {
		wlr_texture_destroy(small);
		return;
	}
	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
		.texture = small,
		.dst_box = (struct wlr_box){ .x = dx, .y = dy, .width = dw, .height = dh },
		.filter_mode = WLR_SCALE_FILTER_NEAREST,
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	});
	wlr_render_pass_submit(pass);
	wlr_texture_destroy(small);
}

void vidow_capture_paint_mosaic(struct vidow_server *server,
		struct wlr_output *output, struct wlr_buffer *dst) {
	struct vidow_capture *cap = capture_for_output(server, output);
	if (cap == NULL) {
		return;
	}
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->capture_mode != VIDOW_CAPTURE_MOSAIC) {
			continue;
		}
		/* 画在屏幕上的是哪个节点就糊哪个：装饰过的窗口画在 fx 节点上（表面
		 * 自己是透明的），没装饰的画在表面上。 */
		struct wlr_scene_buffer *sb =
			view->fx.node != NULL && view->fx.node->buffer != NULL ? view->fx.node :
			(view->scene_surface != NULL ? view->scene_surface->buffer : NULL);
		paint_mosaic_one(cap, sb, dst, view->capture_mosaic);
	}
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		if (ls->capture_mode != VIDOW_CAPTURE_MOSAIC) {
			continue;
		}
		/* layer 的 fx 节点是它背后的模糊底衬，内容在 surface 上。 */
		paint_mosaic_one(cap, vidow_layer_surface_buffer(ls->scene), dst,
			ls->capture_mosaic);
	}
}

struct wlr_buffer *vidow_capture_grab(struct vidow_server *server,
		struct wlr_output *output) {
	return vidow_capture_grab_into(server, output, NULL, 0, NULL);
}

/* 光标抓帧源（pointer cursor session）：结构要先定义，输出源那边会用到它。 */
struct vidow_cursor_source {
	struct wlr_ext_image_capture_source_v1_cursor base;
	struct vidow_server *server;
	struct wlr_output *output;
	struct wlr_buffer *prev_buffer;
	bool initialized;
	bool needs_frame;
	struct wl_listener output_commit;
	struct wl_listener prev_buffer_release;
};

/* ------------------------------------------------- 输出抓帧源（ext-copy） */

struct vidow_capture_source {
	struct wlr_ext_image_capture_source_v1 base;
	struct vidow_capture *cap;
	/* 本次 request_frame 渲染出来的那一张（copy_frame 紧接着就会来拿） */
	struct wlr_buffer *pending;
	/* 客户端要我们把光标画进帧里（ext-image-copy-capture 的 PAINT_CURSORS） */
	bool paint_cursors;
	struct wl_listener output_commit;
};

static void source_update_constraints(struct vidow_capture_source *src) {
	struct wlr_output *output = src->cap->output;
	if (!output->enabled) {
		return;
	}
	/* 客户端缓冲要和屏幕缓冲同规格：用真输出的 swapchain 声明约束（格式 / 设备
	 * / 尺寸都从它来），拷贝那一步 wlroots 会做格式转换。 */
	if (!wlr_output_configure_primary_swapchain(output, NULL, &output->swapchain)) {
		return;
	}
	wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&src->base,
		output->swapchain, output->renderer);
}

static void source_start(struct wlr_ext_image_capture_source_v1 *base,
		bool with_cursors) {
	struct vidow_capture_source *src = wl_container_of(base, src, base);
	src->paint_cursors = with_cursors;
}

static void source_stop(struct wlr_ext_image_capture_source_v1 *base) {
	struct vidow_capture_source *src = wl_container_of(base, src, base);
	src->paint_cursors = false;
}

static void source_handle_output_commit(struct wl_listener *listener, void *data) {
	struct vidow_capture_source *src = wl_container_of(listener, src, output_commit);
	struct wlr_output_event_commit *event = data;
	if (event->state->committed & (WLR_OUTPUT_STATE_MODE |
			WLR_OUTPUT_STATE_RENDER_FORMAT | WLR_OUTPUT_STATE_ENABLED)) {
		source_update_constraints(src);
	}
}

/* 客户端说「给我一帧」：现在就渲染。渲染完把缓冲挂在 src->pending 上，紧接着
 * 管理器会回调 copy_frame 把它拷进客户端缓冲。 */
static void source_request_frame(struct wlr_ext_image_capture_source_v1 *base,
		bool schedule_frame) {
	struct vidow_capture_source *src = wl_container_of(base, src, base);
	(void)schedule_frame;

	wlr_buffer_unlock(src->pending);
	src->pending = vidow_capture_grab(src->cap->server, src->cap->output);

	/* 没渲染出来也要发事件：客户端还在等 ready / failed，闷着就是卡死。
	 * copy_frame 看到 pending 为空会回一个 failed。 */
	int width = src->cap->output->width, height = src->cap->output->height;
	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0,
		width > 0 ? width : 1, height > 0 ? height : 1);
	struct wlr_ext_image_capture_source_v1_frame_event event = {
		.damage = &damage,
	};
	wl_signal_emit_mutable(&base->events.frame, &event);
	pixman_region32_fini(&damage);
}

static void source_copy_frame(struct wlr_ext_image_capture_source_v1 *base,
		struct wlr_ext_image_copy_capture_frame_v1 *frame,
		struct wlr_ext_image_capture_source_v1_frame_event *event) {
	struct vidow_capture_source *src = wl_container_of(base, src, base);
	(void)event;

	if (src->pending == NULL) {
		wlr_ext_image_copy_capture_frame_v1_fail(frame,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
		return;
	}

	if (src->paint_cursors) {
		vidow_capture_paint_cursor(src->cap->server, src->cap->output,
			src->pending);
	}

	if (wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame, src->pending,
			src->cap->output->renderer)) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		/* 我们渲染出来的图是正立的，所以报 NORMAL（不是输出的 transform）。 */
		wlr_ext_image_copy_capture_frame_v1_ready(frame,
			WL_OUTPUT_TRANSFORM_NORMAL, &now);
	}
}

static struct wlr_ext_image_capture_source_v1_cursor *source_get_pointer_cursor(
		struct wlr_ext_image_capture_source_v1 *base, struct wlr_seat *seat) {
	struct vidow_capture_source *src = wl_container_of(base, src, base);
	(void)seat;
	return src->cap->cursor != NULL ? &src->cap->cursor->base : NULL;
}

static const struct wlr_ext_image_capture_source_v1_interface source_impl = {
	.start = source_start,
	.stop = source_stop,
	.request_frame = source_request_frame,
	.copy_frame = source_copy_frame,
	.get_pointer_cursor = source_get_pointer_cursor,
};

bool vidow_capture_source_is_ours(struct wlr_ext_image_capture_source_v1 *source) {
	return source != NULL && source->impl == &source_impl;
}

static void source_destroy(struct vidow_capture_source *src) {
	wlr_buffer_unlock(src->pending);
	wl_list_remove(&src->output_commit.link);
	wlr_ext_image_capture_source_v1_finish(&src->base);
	free(src);
}

/* ------------------------------------------------------------- 光标抓帧源 */

static void cursor_source_update(struct vidow_cursor_source *cursor) {
	struct wlr_output *output = cursor->output;

	if (output->cursor_swapchain != NULL && !cursor->initialized) {
		wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(
			&cursor->base.base, output->cursor_swapchain, output->renderer);
		cursor->initialized = true;
	}

	struct wlr_output_cursor *oc = output->hardware_cursor;
	if (oc == NULL || !oc->visible) {
		cursor->base.entered = false;
		wl_signal_emit_mutable(&cursor->base.events.update, NULL);
		return;
	}

	if (output->cursor_swapchain != NULL &&
			((int)cursor->base.base.width != output->cursor_swapchain->width ||
			(int)cursor->base.base.height != output->cursor_swapchain->height)) {
		cursor->base.base.width = output->cursor_swapchain->width;
		cursor->base.base.height = output->cursor_swapchain->height;
		wl_signal_emit_mutable(&cursor->base.base.events.constraints_update, NULL);
	}

	cursor->base.entered = true;
	cursor->base.x = (int32_t)lround(oc->x);
	cursor->base.y = (int32_t)lround(oc->y);
	cursor->base.hotspot.x = oc->hotspot_x;
	cursor->base.hotspot.y = oc->hotspot_y;
	wl_signal_emit_mutable(&cursor->base.events.update, NULL);
}

static void cursor_source_request_frame(struct wlr_ext_image_capture_source_v1 *base,
		bool schedule_frame) {
	struct vidow_cursor_source *cursor = wl_container_of(base, cursor, base.base);
	if (schedule_frame) {
		wlr_output_update_needs_frame(cursor->output);
		cursor->needs_frame = true;
	}
	/* 光标抓帧的出帧挂在**屏幕的下一帧**上（见 cursor_source_handle_output_commit），
	 * 而光标跑出画面 / 光标在硬件光标层时那块屏一帧都不画 —— 客户端就一直等。
	 * 屏幕静止时由抓帧看门狗替这块屏推一帧。**必须在这里点名这块屏**：看门狗的
	 * screencopy 那条账现在是逐屏的（只推真的有帧在等的那块），光标这条路
	 * 没有 screencopy 帧，不记账就没人推它了。 */
	if (cursor->server != NULL) {
		vidow_output_capture_kick_output(cursor->server, cursor->output);
	}
}

static void cursor_source_copy_frame(struct wlr_ext_image_capture_source_v1 *base,
		struct wlr_ext_image_copy_capture_frame_v1 *frame,
		struct wlr_ext_image_capture_source_v1_frame_event *event) {
	struct vidow_cursor_source *cursor = wl_container_of(base, cursor, base.base);
	(void)event;

	/* 光标图（放大摇晃那种也是它，所以抓到的就是屏幕上真正那个光标）。没有
	 * 硬件光标（软光标 / 没光标）时拿不到 —— 直接告诉客户端这帧不行。 */
	struct wlr_buffer *buffer = cursor->output->cursor_front_buffer;
	if (buffer == NULL) {
		wlr_ext_image_copy_capture_frame_v1_fail(frame,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
		return;
	}

	if (!wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame, buffer,
			cursor->output->renderer)) {
		return;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_ext_image_copy_capture_frame_v1_ready(frame, cursor->output->transform,
		&now);
}

static const struct wlr_ext_image_capture_source_v1_interface cursor_source_impl = {
	.request_frame = cursor_source_request_frame,
	.copy_frame = cursor_source_copy_frame,
};

static void cursor_source_handle_prev_buffer_release(struct wl_listener *listener,
		void *data) {
	struct vidow_cursor_source *cursor =
		wl_container_of(listener, cursor, prev_buffer_release);
	(void)data;
	wl_list_remove(&cursor->prev_buffer_release.link);
	wl_list_init(&cursor->prev_buffer_release.link);
	cursor->prev_buffer = NULL;
}

static void cursor_source_handle_output_commit(struct wl_listener *listener,
		void *data) {
	struct vidow_cursor_source *cursor =
		wl_container_of(listener, cursor, output_commit);
	struct wlr_output_event_commit *event = data;

	cursor_source_update(cursor);

	struct wlr_buffer *buffer = cursor->output->cursor_front_buffer;
	if (buffer != NULL &&
			(buffer != cursor->prev_buffer || cursor->needs_frame)) {
		pixman_region32_t damage;
		pixman_region32_init_rect(&damage, 0, 0, buffer->width, buffer->height);
		struct wlr_ext_image_capture_source_v1_frame_event frame_event = {
			.damage = &damage,
		};
		wl_signal_emit_mutable(&cursor->base.base.events.frame, &frame_event);
		pixman_region32_fini(&damage);

		cursor->prev_buffer = buffer;
		wl_list_remove(&cursor->prev_buffer_release.link);
		cursor->prev_buffer_release.notify = cursor_source_handle_prev_buffer_release;
		wl_signal_add(&buffer->events.release, &cursor->prev_buffer_release);
	}
	cursor->needs_frame = false;

	(void)event;
}

static struct vidow_cursor_source *cursor_source_create(struct vidow_server *server,
		struct wlr_output *output) {
	struct vidow_cursor_source *cursor = calloc(1, sizeof(*cursor));
	if (cursor == NULL) {
		return NULL;
	}
	wlr_ext_image_capture_source_v1_cursor_init(&cursor->base, &cursor_source_impl);
	cursor->server = server;
	cursor->output = output;
	wl_list_init(&cursor->prev_buffer_release.link);
	cursor->output_commit.notify = cursor_source_handle_output_commit;
	wl_signal_add(&output->events.commit, &cursor->output_commit);
	cursor_source_update(cursor);
	return cursor;
}

static void cursor_source_destroy(struct vidow_cursor_source *cursor) {
	wl_list_remove(&cursor->output_commit.link);
	wl_list_remove(&cursor->prev_buffer_release.link);
	wlr_ext_image_capture_source_v1_cursor_finish(&cursor->base);
	free(cursor);
}

/* -------------------------------------------------- 输出抓帧源的管理器 */

/* ext_output_image_capture_source_manager_v1 的全局（一块屏一个源对象，多个客户端
 * 共用一个源，各自的 resource 挂在它下面）。 */
struct vidow_capture_manager {
	struct vidow_server *server;
	struct wl_global *global;
	struct wl_listener display_destroy;
};

static void capture_destroy(struct vidow_capture *cap) {
	wl_list_remove(&cap->output_destroy.link);
	wl_list_remove(&cap->output_commit.link);
	if (cap->source != NULL) {
		source_destroy(cap->source);
	}
	if (cap->cursor != NULL) {
		cursor_source_destroy(cap->cursor);
	}
	wlr_buffer_unlock(cap->mosaic);
	wlr_scene_output_destroy(cap->scene_output);
	wlr_output_finish(&cap->grab_output);
	wlr_backend_finish(&cap->backend);
	free(cap);
}

static void capture_handle_output_destroy(struct wl_listener *listener, void *data) {
	struct vidow_capture *cap = wl_container_of(listener, cap, output_destroy);
	(void)data;
	struct vidow_output *output = vidow_output_for(cap->server, cap->output);
	if (output != NULL) {
		output->capture = NULL;
	}
	capture_destroy(cap);
}

static void capture_handle_output_commit(struct wl_listener *listener, void *data) {
	struct vidow_capture *cap = wl_container_of(listener, cap, output_commit);
	struct wlr_output_event_commit *event = data;
	if (cap->source == NULL) {
		return;
	}
	if (event->state->committed & (WLR_OUTPUT_STATE_MODE |
			WLR_OUTPUT_STATE_RENDER_FORMAT | WLR_OUTPUT_STATE_ENABLED)) {
		source_update_constraints(cap->source);
	}
}

static const struct wlr_backend_impl grab_backend_impl = {0};

/* 假输出永远不真的提交（我们只要 build_state 渲染出来的那块缓冲），但
 * wlr_output_init 断言 test / commit 非空。 */
static bool grab_output_test(struct wlr_output *output,
		const struct wlr_output_state *state) {
	(void)output;
	(void)state;
	return true;
}

static bool grab_output_commit(struct wlr_output *output,
		const struct wlr_output_state *state) {
	(void)output;
	(void)state;
	return true;
}

static const struct wlr_output_impl grab_output_impl = {
	.test = grab_output_test,
	.commit = grab_output_commit,
};

static struct vidow_capture *capture_create(struct vidow_server *server,
		struct wlr_output *output) {
	struct vidow_capture *cap = calloc(1, sizeof(*cap));
	if (cap == NULL) {
		return NULL;
	}
	cap->server = server;
	cap->output = output;

	wlr_backend_init(&cap->backend, &grab_backend_impl);
	cap->backend.buffer_caps = WLR_BUFFER_CAP_DMABUF | WLR_BUFFER_CAP_SHM;

	wlr_output_init(&cap->grab_output, &cap->backend, &grab_output_impl,
		wl_display_get_event_loop(server->display), NULL);
	char name[64];
	snprintf(name, sizeof(name), "CAPTURE-%s", output->name);
	wlr_output_set_name(&cap->grab_output, name);
	cap->grab_output.scale = output->scale;

	if (!wlr_output_init_render(&cap->grab_output, server->allocator, server->renderer)) {
		wlr_log(WLR_ERROR, "抓帧: 假输出初始化失败（%s）", output->name);
		wlr_output_finish(&cap->grab_output);
		wlr_backend_finish(&cap->backend);
		free(cap);
		return NULL;
	}

	cap->scene_output = wlr_scene_output_create(server->scene, &cap->grab_output);
	if (cap->scene_output == NULL) {
		wlr_output_finish(&cap->grab_output);
		wlr_backend_finish(&cap->backend);
		free(cap);
		return NULL;
	}

	cap->output_destroy.notify = capture_handle_output_destroy;
	wl_signal_add(&output->events.destroy, &cap->output_destroy);
	cap->output_commit.notify = capture_handle_output_commit;
	wl_signal_add(&output->events.commit, &cap->output_commit);

	wlr_log(WLR_DEBUG, "抓帧: %s 建了独立的抓帧输出（%s）", output->name, name);
	return cap;
}

/* 客户端要一个「这块屏」的抓帧源。一块屏只有一个源对象（多个客户端共用一个，
 * 各自的 resource 挂在它下面 —— 和 wlroots 那份一样）。 */
static void capture_source_get(struct vidow_capture *cap) {
	if (cap->source != NULL) {
		return;
	}
	cap->source = calloc(1, sizeof(*cap->source));
	if (cap->source == NULL) {
		return;
	}
	cap->source->cap = cap;
	wlr_ext_image_capture_source_v1_init(&cap->source->base, &source_impl);
	cap->source->output_commit.notify = source_handle_output_commit;
	wl_signal_add(&cap->output->events.commit, &cap->source->output_commit);
	source_update_constraints(cap->source);
	if (cap->cursor == NULL) {
		cap->cursor = cursor_source_create(cap->server, cap->output);
	}
}

static void manager_handle_create_source(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t new_id,
		struct wl_resource *output_resource) {
	struct vidow_capture_manager *manager =
		wl_resource_get_user_data(manager_resource);
	struct wlr_output *output = wlr_output_from_resource(output_resource);
	if (output == NULL) {
		wlr_ext_image_capture_source_v1_create_resource(NULL, client, new_id);
		return;
	}

	struct vidow_capture *cap = capture_for_output(manager->server, output);
	if (cap == NULL) {
		wlr_ext_image_capture_source_v1_create_resource(NULL, client, new_id);
		return;
	}
	capture_source_get(cap);
	if (cap->source == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}
	wlr_ext_image_capture_source_v1_create_resource(&cap->source->base, client, new_id);
}

static void manager_handle_destroy(struct wl_client *client,
		struct wl_resource *manager_resource) {
	(void)client;
	wl_resource_destroy(manager_resource);
}

static const struct ext_output_image_capture_source_manager_v1_interface
	manager_impl = {
	.create_source = manager_handle_create_source,
	.destroy = manager_handle_destroy,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version,
		uint32_t id) {
	struct vidow_capture_manager *manager = data;
	struct wl_resource *resource = wl_resource_create(client,
		&ext_output_image_capture_source_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, manager, NULL);
}

static void manager_handle_display_destroy(struct wl_listener *listener, void *data) {
	struct vidow_capture_manager *manager =
		wl_container_of(listener, manager, display_destroy);
	(void)data;
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct vidow_capture_manager *vidow_capture_manager_create(struct wl_display *display,
		struct vidow_server *server) {
	struct vidow_capture_manager *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}
	manager->server = server;
	manager->global = wl_global_create(display,
		&ext_output_image_capture_source_manager_v1_interface, 1, manager,
		manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}
	manager->display_destroy.notify = manager_handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);
	return manager;
}
