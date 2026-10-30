/* debug 帧率叠层（配置键 debug_show_refresh，默认关）：
 *
 *   屏幕左上角   —— 这块屏过去一秒实际出了多少帧（output 渲染帧率）。
 *   每个窗口右上角 —— 这个窗口过去一秒提交了多少帧（client commit 率 =
 *                    应用自己的渲染节奏；空转重绘的应用会在这里露出高数字）。
 *
 * 为什么值得有：Vidow 是 demand-driven，静止桌面本不该出帧。左上角那个总帧率
 * 应当接近 0；某窗口右上角的高 commit 率则直接指出「是哪个应用在空转重绘」。
 *
 * 为什么自己画：wlr_scene 只能画 rect / buffer，没有文字 API。这里用一个 5x7
 * 点阵字模把 "<fps> FPS" 画进一张 CPU 像素 buffer，走 mru.c 那套最小 wlr_buffer
 * （像素在堆上，begin_data_ptr_access 指过去），不碰 allocator / GL / 渲染线程。
 */
#include "server.h"

#include <drm_fourcc.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

/* ---------------------------------------------------------- 最小 CPU buffer */

struct dbg_buf {
	struct wlr_buffer base;
	uint32_t *px;
};

static void dbg_buf_destroy(struct wlr_buffer *buffer) {
	struct dbg_buf *b = wl_container_of(buffer, b, base);
	free(b->px);
	free(b);
}

static bool dbg_buf_begin_access(struct wlr_buffer *buffer, uint32_t flags,
		void **data, uint32_t *format, size_t *stride) {
	(void)flags;
	struct dbg_buf *b = wl_container_of(buffer, b, base);
	*data = b->px;
	*format = DRM_FORMAT_ARGB8888;
	*stride = (size_t)buffer->width * 4;
	return true;
}

static void dbg_buf_end_access(struct wlr_buffer *buffer) {
	(void)buffer;
}

static const struct wlr_buffer_impl dbg_buf_impl = {
	.destroy = dbg_buf_destroy,
	.begin_data_ptr_access = dbg_buf_begin_access,
	.end_data_ptr_access = dbg_buf_end_access,
};

static struct wlr_buffer *dbg_buf_create(int w, int h) {
	if (w < 1 || h < 1) {
		return NULL;
	}
	struct dbg_buf *b = calloc(1, sizeof(*b));
	if (b == NULL) {
		return NULL;
	}
	b->px = calloc((size_t)w * (size_t)h, sizeof(b->px[0]));
	if (b->px == NULL) {
		free(b);
		return NULL;
	}
	wlr_buffer_init(&b->base, &dbg_buf_impl, w, h);
	return &b->base;
}

/* ------------------------------------------------------------ 5x7 点阵字模
 * 每字符 5 列，每列一个字节，bit0 = 最上面一行，bit6 = 最下面一行。 */
struct dbg_glyph {
	char c;
	uint8_t col[5];
};

static const struct dbg_glyph dbg_font[] = {
	{ '0', { 0x3E, 0x51, 0x49, 0x45, 0x3E } },
	{ '1', { 0x00, 0x42, 0x7F, 0x40, 0x00 } },
	{ '2', { 0x42, 0x61, 0x51, 0x49, 0x46 } },
	{ '3', { 0x21, 0x41, 0x45, 0x4B, 0x31 } },
	{ '4', { 0x18, 0x14, 0x12, 0x7F, 0x10 } },
	{ '5', { 0x27, 0x45, 0x45, 0x45, 0x39 } },
	{ '6', { 0x3C, 0x4A, 0x49, 0x49, 0x30 } },
	{ '7', { 0x01, 0x71, 0x09, 0x05, 0x03 } },
	{ '8', { 0x36, 0x49, 0x49, 0x49, 0x36 } },
	{ '9', { 0x06, 0x49, 0x49, 0x29, 0x1E } },
	{ '.', { 0x00, 0x60, 0x60, 0x00, 0x00 } },
	{ 'F', { 0x7F, 0x09, 0x09, 0x09, 0x01 } },
	{ 'P', { 0x7F, 0x09, 0x09, 0x09, 0x06 } },
	{ 'S', { 0x46, 0x49, 0x49, 0x49, 0x31 } },
	{ ' ', { 0x00, 0x00, 0x00, 0x00, 0x00 } },
};

static const struct dbg_glyph *dbg_find_glyph(char c) {
	for (size_t i = 0; i < sizeof(dbg_font) / sizeof(dbg_font[0]); i++) {
		if (dbg_font[i].c == c) {
			return &dbg_font[i];
		}
	}
	return NULL;
}

/* 把 text 画进 w×h 的 ARGB8888（预乘）像素里：半透明黑底 + 白字。 */
static void dbg_draw_text(uint32_t *px, int w, int h, const char *text,
		int scale, int pad) {
	const uint32_t bg = 0x99000000u; /* 黑底 a=0x99 */
	const uint32_t fg = 0xFFFFFFFFu; /* 白字 a=0xFF */
	for (int i = 0; i < w * h; i++) {
		px[i] = bg;
	}
	int x = pad;
	const int y = pad;
	for (const char *p = text; *p != '\0'; p++) {
		const struct dbg_glyph *g = dbg_find_glyph(*p);
		if (g == NULL) {
			x += 6 * scale;
			continue;
		}
		for (int col = 0; col < 5; col++) {
			uint8_t bits = g->col[col];
			for (int row = 0; row < 7; row++) {
				if ((bits & (1u << row)) == 0) {
					continue;
				}
				for (int dy = 0; dy < scale; dy++) {
					for (int dx = 0; dx < scale; dx++) {
						int px_x = x + col * scale + dx;
						int px_y = y + row * scale + dy;
						if (px_x >= 0 && px_x < w && px_y >= 0 && px_y < h) {
							px[px_y * w + px_x] = fg;
						}
					}
				}
			}
		}
		x += 6 * scale;
	}
}

/* 造一张写着 text 的 buffer，回填尺寸。用完 wlr_buffer_drop() 收。 */
static struct wlr_buffer *dbg_make_text_buffer(const char *text,
		int scale, int pad, int *out_w, int *out_h) {
	int n = (int)strlen(text);
	int w = pad * 2 + n * 6 * scale - scale;
	int h = pad * 2 + 7 * scale;
	struct wlr_buffer *buf = dbg_buf_create(w, h);
	if (buf == NULL) {
		return NULL;
	}
	void *data = NULL;
	uint32_t fmt = 0;
	size_t stride = 0;
	if (wlr_buffer_begin_data_ptr_access(buf, WLR_BUFFER_DATA_PTR_ACCESS_WRITE,
			&data, &fmt, &stride)) {
		dbg_draw_text(data, w, h, text, scale, pad);
		wlr_buffer_end_data_ptr_access(buf);
	}
	*out_w = w;
	*out_h = h;
	return buf;
}

/* ---------------------------------------------------------------- 每窗口叠层 */

void vidow_debugrate_view_commit(struct vidow_view *view) {
	if (view == NULL || view->server == NULL ||
			!view->server->config.debug_show_refresh) {
		return;
	}
	view->dbg_frames++;
	if (view->dbg_window_ns == 0) {
		view->dbg_window_ns = vidow_now_ns();
	}
}

/* 更新一个窗口的帧率叠层（只处理这块屏上的）。 */
static void dbg_view_update(struct vidow_server *server,
		struct vidow_output *output, struct vidow_view *view, int64_t now) {
	/* 窗口没真显示：把叠层藏起来。
	 *
	 * 关键一条是 scene_parked：滚出视口 / 工作区藏起来时 Vidow 不动 geom、
	 * 也不 disable 节点，而是把 scene_tree park 到所有输出框外面（见 view.c
	 * 的 vidow_view_set_parked）。只看 node.enabled 的话，窗口已经滚走了、
	 * 那一行窗口还留在原处把数字钉在屏上（用户报的「窗口裁切掉了 fps 还在」）。 */
	bool shown = view->mapped && !view->popup && view->mini == VIDOW_MINI_NONE &&
		!view->scene_parked &&
		view->scene_tree != NULL && view->scene_tree->node.enabled &&
		view->geom.width > 0 && view->geom.height > 0;
	if (!shown) {
		if (view->dbg_node != NULL) {
			wlr_scene_node_set_enabled(&view->dbg_node->node, false);
		}
		return;
	}

	bool rebuild = false;
	if (view->dbg_window_ns != 0 && now - view->dbg_window_ns >= 500000000LL) {
		double secs = (double)(now - view->dbg_window_ns) / 1e9;
		view->dbg_fps = secs > 0 ? (double)view->dbg_frames / secs : 0.0;
		view->dbg_frames = 0;
		view->dbg_window_ns = now;
		rebuild = true;
	}
	if (view->dbg_node == NULL) {
		view->dbg_node = wlr_scene_buffer_create(server->layer_overlay, NULL);
		if (view->dbg_node == NULL) {
			return;
		}
		view->dbg_fps = 0.0;
		rebuild = true;
	}
	wlr_scene_node_set_enabled(&view->dbg_node->node, true);

	if (rebuild) {
		char text[32];
		snprintf(text, sizeof(text), "%d FPS", (int)lround(view->dbg_fps));
		int w = 0, h = 0;
		struct wlr_buffer *buf = dbg_make_text_buffer(text, 1, 3, &w, &h);
		if (buf != NULL) {
			wlr_scene_buffer_set_buffer(view->dbg_node, buf);
			wlr_buffer_drop(buf);
			view->dbg_buf_w = w;
			view->dbg_buf_h = h;
		}
	}

	/* 摆在窗口右上角，**并且裁到「窗口露在这块屏上的那块区域」里**。
	 *
	 * 位置用 vidow_view_screen_box()（geom + anim_off，场景全局坐标 —— 和
	 * 窗口节点真正被摆到的位置同一个来源），拖动 / 过渡动画里数字跟得住。
	 *
	 * 为什么要裁：layer_overlay 是**全局**场景树，wlr_scene 只按输出做视口
	 * 裁剪、不会把子节点自动切给某块屏。所以一个摆在 x=1900 的 buffer 在
	 * DP-3（0..1920）画一半、在 DP-2（1920..3840）画另一半 —— 窗口滚到屏幕
	 * 边缘 / 搬到另一块屏时，数字就跟着露到隔壁屏上（用户报的「还是跟着窗口
	 * 移动到另一个屏幕」）。
	 *
	 * **必须是「完整放得下」，不能只夹起点。** wlroots 0.20 没有
	 * wlr_scene_buffer_set_clip()（只有 subsurface_tree 版，那个连输入一起裁），
	 * 所以没法把数字裁成半个。只把起点夹进 vis 的话，vis 比数字窄时数字仍会
	 * 从 vis.x 开始一路溢出到隔壁屏 —— 用户报的「稍稍有点跨屏」就是这一截。
	 * 放不下就整个不画：干净，且绝不会跨屏。 */
	struct wlr_box ob = {0};
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &ob);
	struct wlr_box sb = vidow_view_screen_box(view);
	struct wlr_box vis;
	if (ob.width <= 0 || ob.height <= 0 ||
			!wlr_box_intersection(&vis, &sb, &ob)) {
		wlr_scene_node_set_enabled(&view->dbg_node->node, false);
		return;
	}
	if (vis.width < view->dbg_buf_w || vis.height < view->dbg_buf_h) {
		/* 窗口露出来的那块比数字还小：不画（见上面那段）。 */
		wlr_scene_node_set_enabled(&view->dbg_node->node, false);
		return;
	}
	wlr_scene_node_set_enabled(&view->dbg_node->node, true);

	const int margin = 4;
	/* 右上角，然后夹进 vis —— 上面已经保证 vis 装得下整个数字，所以夹完
	 * 一定完整落在窗口可见区域内。 */
	int nx = vis.x + vis.width - view->dbg_buf_w - margin;
	if (nx < vis.x) {
		nx = vis.x;
	}
	if (nx + view->dbg_buf_w > vis.x + vis.width) {
		nx = vis.x + vis.width - view->dbg_buf_w;
	}
	int ny = vis.y + margin;
	if (ny + view->dbg_buf_h > vis.y + vis.height) {
		ny = vis.y + vis.height - view->dbg_buf_h;
	}
	if (ny < vis.y) {
		ny = vis.y;
	}
	wlr_scene_node_set_position(&view->dbg_node->node, nx, ny);
}

/* -------------------------------------------------------------- 每屏总帧率 */

void vidow_debugrate_tick(struct vidow_server *server,
		struct vidow_output *output) {
	if (server == NULL || output == NULL) {
		return;
	}

	/* 关着：只在有残留节点时收一次，之后 O(1) 返回，不为关掉的功能每帧记账。 */
	if (!server->config.debug_show_refresh) {
		if (output->dbg_node != NULL) {
			wlr_scene_node_destroy(&output->dbg_node->node);
			output->dbg_node = NULL;
			output->dbg_buf_w = 0;
			output->dbg_buf_h = 0;
			struct vidow_view *v;
			wl_list_for_each(v, &server->views, link) {
				if (v->output != output->wlr_output) {
					continue;
				}
				if (v->dbg_node != NULL) {
					wlr_scene_node_destroy(&v->dbg_node->node);
					v->dbg_node = NULL;
					v->dbg_buf_w = 0;
					v->dbg_buf_h = 0;
				}
			}
		}
		return;
	}

	int64_t now = vidow_now_ns();

	/* 屏幕总帧率：每个渲染帧记一笔，每 500ms 结算。 */
	output->dbg_frames++;
	if (output->dbg_window_ns == 0) {
		output->dbg_window_ns = now;
	}
	bool rebuild = false;
	if (now - output->dbg_window_ns >= 500000000LL) {
		double secs = (double)(now - output->dbg_window_ns) / 1e9;
		output->dbg_fps = secs > 0 ? (double)output->dbg_frames / secs : 0.0;
		output->dbg_frames = 0;
		output->dbg_window_ns = now;
		rebuild = true;
	}

	if (output->dbg_node == NULL) {
		output->dbg_node = wlr_scene_buffer_create(server->layer_overlay, NULL);
		if (output->dbg_node == NULL) {
			return;
		}
		output->dbg_fps = 0.0;
		rebuild = true;
	}
	if (rebuild) {
		char text[32];
		snprintf(text, sizeof(text), "OUT %d FPS", (int)lround(output->dbg_fps));
		int w = 0, h = 0;
		struct wlr_buffer *buf = dbg_make_text_buffer(text, 2, 4, &w, &h);
		if (buf != NULL) {
			wlr_scene_buffer_set_buffer(output->dbg_node, buf);
			wlr_buffer_drop(buf);
			output->dbg_buf_w = w;
			output->dbg_buf_h = h;
		}
	}
	/* 屏幕总帧率摆左上角（每窗口的摆各自右上角，两者不打架）。 */
	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
	const int margin = 8;
	wlr_scene_node_set_position(&output->dbg_node->node,
		box.x + margin, box.y + margin);

	/* 每窗口帧率。 */
	struct vidow_view *v;
	wl_list_for_each(v, &server->views, link) {
		if (v->output != output->wlr_output) {
			continue;
		}
		dbg_view_update(server, output, v, now);
	}
}
