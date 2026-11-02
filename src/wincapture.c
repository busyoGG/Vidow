/* wincapture.c — 「把这个窗口单独渲染出来」这件事的实现，两个用家：
 *
 *   1. 按窗口录屏 / 预览的截图源（ext-image-capture-source-v1 的窗口源，本文件下半）；
 *   2. portal 录屏那条路（zkde_screencast 的 stream_window，见 screencast.c）。
 *
 * 为什么不用 wlroots 那份（wlr_ext_image_capture_source_v1_create_with_scene_node）：
 * 它给「窗口的子树」在**同一张场景图**里、按节点的绝对坐标摆一个虚拟输出出来渲染
 * （types/ext_image_capture_source_v1/scene.c）。三个后果都踩过：
 *
 *   1) 抓帧是**损伤驱动**的：虚拟输出那一趟只在场景有损伤时才渲染（"We can only emit
 *      frames with damage"）。窗口不动、客户端也不重绘的时候（一个静止的终端），
 *      客户端 `capture` 要的那一帧**永远不来** —— 选择器的预览卡在「等第一帧」
 *      （客户端那边 busy 一直是 true、之后不再重试，所以把窗口挪进视口也还是没预览），
 *      OBS 那边一会儿 streaming 一会儿 paused，error 还是 none（协议里没这个错，
 *      就是没帧）。量过：静止窗口滚到视口外，抓 3 秒只出 1 帧（第一帧），一直在动的
 *      窗口同样 3 秒 89 帧。
 *   2) 抓的是「这块坐标里的一切」：压在窗口上的别的东西、隔壁屏伸过来的一条，
 *      都会进画面 —— 录出来不是这个窗口，是这块屏幕。
 *   3) 虚拟输出跟着节点走：节点一动（我们让滚出视口的窗口停到屏外）尺寸 / 位置
 *      就变，客户端得重建会话，正好撞上它在等第一帧。
 *
 * Hyprland 的做法（src/managers/screenshare/ScreenshareFrame.cpp 的 renderWindow）
 * 是**把窗口单独渲染进自己的缓冲**：跟它在屏幕上摆在哪、被谁压着都没关系。这里
 * 照这个来 ——
 *
 *   一棵**自己的场景**（里面只有这个窗口的一份镜像：表面 + 子表面）
 *   + 一个假输出（wlroots 的渲染入口都要一个输出落脚，同 capture.c 那套）
 *   + 要一帧就渲染一帧（不看损伤）。
 *
 * 渲染出来的是**客户端表面本身**：没有我们的装饰、模糊、圆角、阴影 —— 和
 * Hyprland 的窗口导出一样（"will not contain any server-side decorations"）。
 * 好的副作用：抓帧既然跟真实节点没关系了，屏幕上那一份就可以随便裁 —— 跨屏的
 * 那半块裁掉（不再串屏，见 view.c 的 view_clip_box），停到屏外也不影响抓帧。
 */

#include "server.h"

#include <drm_fourcc.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>

#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>

#include "ext-image-capture-source-v1-server-protocol.h"

/* 假输出永远不真的提交（我们只要 build_state 渲染出来的那块缓冲），但
 * wlr_output_init 断言 commit 非空。同 capture.c 里那一份。 */
static bool output_test(struct wlr_output *output,
		const struct wlr_output_state *state) {
	(void)output;
	(void)state;
	return true;
}

static bool output_commit(struct wlr_output *output,
		const struct wlr_output_state *state) {
	(void)output;
	(void)state;
	return true;
}

static const struct wlr_output_impl output_impl = {
	.test = output_test,
	.commit = output_commit,
};

static const struct wlr_backend_impl backend_impl = {0};

/* ------------------------------------------------------- 窗口的「取景器」 */

struct vidow_win_grab {
	struct vidow_server *server;
	struct vidow_view *view;

	/* 私有场景：里面只有这个窗口的一份镜像。 */
	struct wlr_scene *scene;
	struct wlr_scene_tree *mirror;

	/* 假后端 + 假输出：只是给 scene_output 一个落脚的地方，永远不真的提交。 */
	struct wlr_backend backend;
	struct wlr_output output;
	struct wlr_scene_output *scene_output;

	/* 表面没了（窗口关了）：镜像那棵树跟着就没了，这里只记一笔，别再去碰它。 */
	bool dead;

	/* 装饰（wincapture_decor，默认开）：烘好的两张图各挂一个场景节点，把窗口
	 * 内容那个镜像**夹在中间** —— 屏上的叠放次序就是「底衬 < 内容 < 画框」，
	 * 一张图做不到（画框得压在内容上面，模糊得垫在内容下面）。
	 *   decor_blur  背后的模糊底衬（在镜像下面）
	 *   decor_edge  圆角 / 边框 / 投影 / 颗粒（在镜像上面）
	 * 两张都懒建、按尺寸缓存：抓帧是每帧一次的活，每帧重新分配几块 GBM 缓冲
	 * 就是白白几十次 BO 映射。 */
	struct wlr_scene_buffer *decor_blur;
	struct wlr_scene_buffer *decor_edge;
	struct wlr_buffer *blur_buf;
	int blur_w, blur_h;
	struct wlr_buffer *edge_buf;
	int edge_w, edge_h;

	struct wl_listener surface_destroy;
	struct wl_listener scene_output_destroy;
};

/* 视图还在不在。窗口可能已经没了（表面销毁和我们这一趟在同一个事件循环轮次里，
 * 视图先走一步）—— 碰 grab->view 之前先在这儿认一下，别读一块已经 free 的内存。 */
static bool grab_view_alive(const struct vidow_win_grab *grab) {
	struct vidow_view *view;
	if (grab->view == NULL) {
		return false;
	}
	wl_list_for_each(view, &grab->server->views, link) {
		if (view == grab->view) {
			return true;
		}
	}
	return false;
}

/* 日志里那个名字。 */
static const char *grab_label(struct vidow_win_grab *grab) {
	const char *title = grab_view_alive(grab) ?
		vidow_view_title(grab->view) : NULL;
	return title != NULL ? title : "(无题)";
}

/* 镜像树里所有带尺寸的节点的外接框（相对镜像树自己，逻辑坐标）。跟 wlroots
 * 那份 scene.c 里的算法一样，只是我们不借它的私有头。 */
static void node_extents(struct wlr_scene_node *node, int lx, int ly,
		int *x_min, int *y_min, int *x_max, int *y_max) {
	switch (node->type) {
	case WLR_SCENE_NODE_TREE: {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			node_extents(child, lx + child->x, ly + child->y,
				x_min, y_min, x_max, y_max);
		}
		break;
	}
	case WLR_SCENE_NODE_RECT: {
		struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
		if (lx < *x_min) {
			*x_min = lx;
		}
		if (ly < *y_min) {
			*y_min = ly;
		}
		if (lx + rect->width > *x_max) {
			*x_max = lx + rect->width;
		}
		if (ly + rect->height > *y_max) {
			*y_max = ly + rect->height;
		}
		break;
	}
	case WLR_SCENE_NODE_BUFFER: {
		struct wlr_scene_buffer *buf = wlr_scene_buffer_from_node(node);
		int w = 0, h = 0;
		if (buf->dst_width > 0 && buf->dst_height > 0) {
			w = buf->dst_width;
			h = buf->dst_height;
		} else if (buf->buffer != NULL) {
			w = buf->buffer->width;
			h = buf->buffer->height;
			wlr_output_transform_coords(buf->transform, &w, &h);
		}
		if (lx < *x_min) {
			*x_min = lx;
		}
		if (ly < *y_min) {
			*y_min = ly;
		}
		if (lx + w > *x_max) {
			*x_max = lx + w;
		}
		if (ly + h > *y_max) {
			*y_max = ly + h;
		}
		break;
	}
	}
}

/* 镜像的外接框（相对镜像树自己）：子表面可能带负偏移（xdg 的 geometry 补偿），
 * 所以 x/y 可能是负的 —— 调用方按它把镜像摆正。没内容（客户端还没提交过缓冲）
 * 就返回 false。 */
static bool mirror_extents(struct vidow_win_grab *grab, struct wlr_box *box) {
	if (grab->dead || grab->mirror == NULL) {
		return false;
	}
	int x_min = INT_MAX, y_min = INT_MAX;
	int x_max = INT_MIN, y_max = INT_MIN;
	node_extents(&grab->mirror->node, 0, 0, &x_min, &y_min, &x_max, &y_max);
	if (x_max <= x_min || y_max <= y_min) {
		return false;
	}
	*box = (struct wlr_box){
		.x = x_min, .y = y_min,
		.width = x_max - x_min, .height = y_max - y_min,
	};
	return true;
}

bool vidow_win_grab_box(struct vidow_win_grab *grab, int *width, int *height) {
	struct wlr_box box = {0};
	if (!mirror_extents(grab, &box)) {
		return false;
	}
	*width = box.width;
	*height = box.height;
	return true;
}

struct wlr_swapchain *vidow_win_grab_swapchain(struct vidow_win_grab *grab) {
	return grab->output.swapchain;
}

/* 抓帧框（**布局坐标**）+「布局逻辑 → 流像素」的缩放：跟 vidow_win_grab_render
 * 里那次「等比摆进 out_w × out_h」是同一套算法（那圈黑边也算在框里）。返回的框
 * 就是**流像素 0..out_w / 0..out_h 对应的那块布局矩形**，于是
 *
 *     (布局坐标 - 框原点) × 缩放 = 流里的像素位置
 *
 * 按窗口录屏的光标要按它换算：那条路是「窗口等比缩放进流」，缩放跟屏的缩放不是
 * 一回事（窗口 400x300 摆进 400x300 的流是 1 倍，窗口 954x1068 摆进 674x748 的流
 * 是 0.7 倍），拿屏的框去换算，位置会整个落到流外面 —— 表现就是「有画面、光标不
 * 进来」（OBS 的 cursor_mode=4 那条路，实测元数据里 id=0）。
 *
 * 窗口停到屏外（藏起来的工作区 / 滚出视口）时返回 false：那种窗口不在屏上，没有
 * 「光标指着它」这回事。 */
bool vidow_win_grab_frame_box(struct vidow_win_grab *grab, int out_w, int out_h,
		struct wlr_box *box, double *scale) {
	if (grab == NULL || box == NULL || scale == NULL) {
		return false;
	}
	struct wlr_box ext = {0};
	if (!mirror_extents(grab, &ext) || !grab_view_alive(grab) ||
			grab->view->scene_parked) {
		return false;
	}
	struct wlr_box at = vidow_view_screen_box(grab->view);
	if (out_w <= 0 || out_h <= 0) {
		out_w = ext.width;
		out_h = ext.height;
	}
	double sx = (double)out_w / ext.width;
	double sy = (double)out_h / ext.height;
	double s = sx < sy ? sx : sy;
	if (s <= 0) {
		return false;
	}
	/* 外接框等比缩放后居中 → 反推回「整块流对应的布局矩形」。 */
	double lw = (double)out_w / s;
	double lh = (double)out_h / s;
	*box = (struct wlr_box){
		.x = (int)lround(at.x + ext.x - (lw - ext.width) / 2.0),
		.y = (int)lround(at.y + ext.y - (lh - ext.height) / 2.0),
		.width = (int)lround(lw),
		.height = (int)lround(lh),
	};
	*scale = s;
	return true;
}

/* ------------------------------------------------- 一次抓帧的几何（取景框） */

/* 这一帧的全部几何。三趟渲染和装饰那一趟都从这一份拿数 —— 各算一遍必然会差
 * 一两个像素（取整方式不同），那就是「画框跟窗口错开半格」。 */
struct grab_geom {
	int out_w, out_h;         /* 出图尺寸（像素） */
	double scale;             /* 布局逻辑 → 出图像素 */
	struct wlr_box ext;       /* 镜像外接框（相对镜像树，逻辑） */
	int mirror_x, mirror_y;   /* 镜像节点位置（输出逻辑坐标） */
	int content_x, content_y; /* 内容左上角在出图里的位置（像素） */
	int content_w, content_h; /* 内容尺寸（像素） */
	/* 取景框（布局逻辑坐标）：出图 0..out_w / 0..out_h 对应的那块矩形 */
	int frame_x, frame_y, frame_w, frame_h;
	struct wlr_box at;        /* 窗口现在画在屏上的框（布局逻辑） */
};

/* 算这一帧的几何。out_w / out_h 给 0 就按窗口自己的尺寸（1:1）—— 同
 * vidow_win_grab_render 的老口径。窗口还没内容返回 false。 */
static bool grab_geometry(struct vidow_win_grab *grab, int out_w, int out_h,
		struct grab_geom *g) {
	struct wlr_box box = {0};
	if (!mirror_extents(grab, &box)) {
		return false;
	}
	double scale = 1.0;
	if (out_w > 0 && out_h > 0) {
		double sx = (double)out_w / box.width;
		double sy = (double)out_h / box.height;
		scale = sx < sy ? sx : sy;
	} else {
		out_w = box.width;
		out_h = box.height;
	}
	if (scale <= 0.0) {
		return false;
	}
	double logical_w = (double)out_w / scale;
	double logical_h = (double)out_h / scale;
	/* 镜像摆正 + 居中：外接框挪到 (0,0) 是基准，剩下的边按等比缩放留黑。 */
	int mxl = (int)lround((logical_w - box.width) / 2.0);
	int myl = (int)lround((logical_h - box.height) / 2.0);
	*g = (struct grab_geom){
		.out_w = out_w, .out_h = out_h, .scale = scale, .ext = box,
		.mirror_x = mxl - box.x, .mirror_y = myl - box.y,
		.content_x = (int)lround((double)mxl * scale),
		.content_y = (int)lround((double)myl * scale),
		.content_w = (int)lround((double)box.width * scale),
		.content_h = (int)lround((double)box.height * scale),
		.frame_w = (int)lround(logical_w), .frame_h = (int)lround(logical_h),
	};
	if (grab_view_alive(grab)) {
		g->at = vidow_view_screen_box(grab->view);
	}
	/* 取景框左上角的布局坐标：内容左上角在布局里是 (at + 外接框原点)，而它在
	 * 出图里是 (content_x, content_y)，倒推回去就是框原点。 */
	g->frame_x = g->at.x + box.x - mxl;
	g->frame_y = g->at.y + box.y - myl;
	return true;
}

/* 渲染私有场景那一趟：窗口按 g->scale 摆进 g->out_w × g->out_h，渲染进 sc
 * （空 = 抓帧自己的交换链，出图尺寸就是约束尺寸）。返回**加锁的**缓冲。
 *
 * hide_mirror = 这一趟不画镜像（锁定期间回黑帧）；below / above = 这一趟额外
 * 打开的场景节点（烘好的底衬 / 画框），渲染完关回去。
 *
 * **不看损伤**：静止的窗口本来就没有新损伤，按损伤判就等于不给帧。 */
static struct wlr_buffer *grab_render_scene(struct vidow_win_grab *grab,
		const struct grab_geom *g, struct wlr_swapchain *sc,
		bool hide_mirror, struct wlr_scene_buffer *below,
		struct wlr_scene_buffer *above) {
	struct wlr_output *output = &grab->output;
	/* 缩放决定「逻辑坐标 → 像素」的换算；尺寸手动跟上 —— 假输出从来没提交过
	 * 模式，而 scene_output 的损伤记账是按 output->width/height 裁的
	 * （同 capture.c 里那条注释）。不 commit：commit 会让它变成「enabled 的
	 * 输出」，抢走窗口的主输出判定，客户端的 dmabuf feedback 就跟着变了。 */
	output->scale = g->scale;
	output->width = g->out_w;
	output->height = g->out_h;

	if (grab->mirror != NULL) {
		wlr_scene_node_set_position(&grab->mirror->node,
			g->mirror_x, g->mirror_y);
	}

	/* 整块重画：这块假输出没有输出级的损伤记账，整块最省心（一次抓帧本来也
	 * 要读整块），也免得损伤环里没脏东西时渲染那一趟一个像素都不画 —— 那样
	 * 拿回来的是两张交换链缓冲里更旧的那张。 */
	pixman_region32_t whole;
	pixman_region32_init_rect(&whole, 0, 0, g->out_w, g->out_h);
	wlr_damage_ring_add(&grab->scene_output->damage_ring, &whole);
	pixman_region32_fini(&whole);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_state_set_custom_mode(&state, g->out_w, g->out_h, 0);

	/* sc 非空 = 渲染进调用方那张 linear 交换链（录屏 dmabuf 直通）。尺寸对不上
	 * 就不用它（同 capture.c 里那条注释）。 */
	struct wlr_swapchain *use = NULL;
	if (sc != NULL && sc->width == g->out_w && sc->height == g->out_h) {
		use = sc;
	}
	struct wlr_scene_output_state_options opts = { .swapchain = use };

	/* 锁定期间**不画镜面**：这棵私有场景跟锁屏时被关掉的那棵正常树没关系
	 * （见 sessionlock.c），不挡的话锁定期间照样能把被藏窗口的画面抓出来
	 * （实测：锁定期抓帧跟锁前逐像素相同）。只把镜像节点关这一趟，这一帧就是
	 * 「空场景」→ 纯黑；判据每帧现读 server->locked，解锁后下一次调用自动照旧，
	 * 所以不用挂锁定 / 解锁的信号、也不存状态。
	 *
	 * **不 return NULL**：回 NULL 客户端收到的是 failed，得反复换缓冲重试，录屏
	 * 的窗口流也会卡住 —— 回一张黑帧客户端照常往下走（协议上这就是「这一帧没内容」）。 */
	bool hide = hide_mirror && grab->mirror != NULL;
	if (hide) {
		wlr_scene_node_set_enabled(&grab->mirror->node, false);
	}
	if (below != NULL) {
		wlr_scene_node_set_enabled(&below->node, true);
	}
	if (above != NULL) {
		wlr_scene_node_set_enabled(&above->node, true);
	}

	struct wlr_buffer *buffer = NULL;
	if (wlr_scene_output_build_state(grab->scene_output, &state,
			use != NULL ? &opts : NULL)) {
		buffer = state.buffer;
		if (buffer != NULL) {
			wlr_buffer_lock(buffer);
		}
	}
	if (above != NULL) {
		wlr_scene_node_set_enabled(&above->node, false);
	}
	if (below != NULL) {
		wlr_scene_node_set_enabled(&below->node, false);
	}
	if (hide) {
		wlr_scene_node_set_enabled(&grab->mirror->node, true);
	}
	wlr_output_state_finish(&state);

	if (buffer == NULL) {
		if (use != NULL) {
			wlr_log(WLR_DEBUG, "wincapture: %s 直通交换链没空闲缓冲，这帧跳过",
				grab_label(grab));
		} else {
			wlr_log(WLR_ERROR, "wincapture: %s 渲染失败（%dx%d，缩放 %.3f）",
				grab_label(grab), g->out_w, g->out_h, g->scale);
		}
		return NULL;
	}

	/* 被抓的这个窗口得拿到帧回调，它才会接着画下一帧（跟画在屏幕上时一样）。 */
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(grab->scene_output, &now);

	return buffer;
}

/* 烘装饰图用的缓冲（尺寸 = 取景框），按尺寸缓存。 */
static struct wlr_buffer *grab_bake_target(struct wlr_buffer **slot, int *sw,
		int *sh, struct vidow_server *server, int w, int h) {
	if (*slot != NULL && *sw == w && *sh == h) {
		return *slot;
	}
	struct wlr_buffer *buf = vidow_alloc_target(server, w, h);
	if (buf == NULL) {
		return NULL;
	}
	wlr_buffer_drop(*slot);
	*slot = buf;
	*sw = w;
	*sh = h;
	return buf;
}

/* 底衬那张贴图代表的那块屏的缩放（设备像素 / 布局逻辑）。取景框的缩放常常和它
 * 不是一回事（窗口等比缩进流里），底衬采样要按布局坐标换算，所以必须知道它。
 *
 * 认**跟着贴图一起记的** blur_out_*（见 server.h）：跨屏时窗口可能采到另一块屏
 * 的底衬，拿窗口自己那块屏的缩放去算就是错位 —— 那正是提交 f022b9e 修过的
 * 跨屏底衬采样 bug。找不到那块屏（刚拔线）就退回窗口自己那块屏的缩放。 */
static double grab_backdrop_scale(struct vidow_server *server,
		const struct vidow_fx_target *fx, double fallback) {
	struct vidow_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output->width == fx->blur_out_w &&
				o->wlr_output->height == fx->blur_out_h &&
				o->box.x == fx->blur_out_x && o->box.y == fx->blur_out_y) {
			double s = o->wlr_output->scale;
			return s > 0.0 ? s : 1.0;
		}
	}
	return fallback > 0.0 ? fallback : 1.0;
}

/* 渲染一帧：窗口等比缩放摆进 out_w × out_h（剩下的边是黑的），返回**加锁的**
 * 缓冲（调用方负责 unlock）。out_w / out_h 给 0 就按窗口自己的尺寸（1:1）。
 *
 * 默认（wincapture_decor）录出来就是**这个窗口在屏上的样子**：
 *   ① 背后的模糊底衬（垫在窗口内容下面）
 *   ② 那圈画框：圆角 / 边框 / 投影 / 颗粒（压在窗口内容上面）
 * 两样都用**屏幕上那同一个着色器、同一遍**画（fx.c 的 decor_pass），差别只有
 * 几何（渲染目标是取景框、底衬采样按取景框的缩放换算）。两张图各挂一个场景节点，
 * 把窗口内容那个镜像夹在中间 —— 屏上的叠放次序就是这样，一张图做不到。
 *
 * 关掉开关（或者锁着 / 渲染器不是 GLES2 / 没有内容）就是原来那一趟：只有窗口。
 *
 * **不看损伤**：静止的窗口本来就没有新损伤，按损伤判就等于不给帧。 */
struct wlr_buffer *vidow_win_grab_render(struct vidow_win_grab *grab,
		int out_w, int out_h, struct wlr_swapchain *target) {
	if (grab == NULL) {
		return NULL;
	}
	struct vidow_server *server = grab->server;
	struct grab_geom g;
	if (!grab_geometry(grab, out_w, out_h, &g)) {
		return NULL;
	}

	bool locked = server->locked;
	/* 锁定期间装饰那两趟也不跑：不然锁着还能抓到「被藏窗口的画框 + 底衬」——
	 * 提交 078e9e2 补的就是这个洞（锁定期抓帧跟锁前逐像素相同），
	 * test-wincapture-lock.sh 盯着它。 */
	bool want_decor = server->config.wincapture_decor && !locked &&
		server->decor.program != 0 && grab_view_alive(grab) &&
		grab->mirror != NULL && grab->decor_blur != NULL &&
		grab->decor_edge != NULL;
	if (!want_decor) {
		return grab_render_scene(grab, &g, target, locked, NULL, NULL);
	}

	struct vidow_view *view = grab->view;
	/* 客户端内容那张贴图（借来或自己建）：不画进装饰图里（内容由镜像节点画），
	 * 只用来决定「背后的模糊画在哪儿」—— 装饰着色器的透明度剔除按元素自己的
	 * alpha 走，半透明的地方才透出底衬（跟屏上同一条判据）。 */
	bool ctex_owned = false;
	struct wlr_scene_buffer *sb = view->scene_surface != NULL ?
		view->scene_surface->buffer : NULL;
	struct wlr_texture *ctex = sb != NULL ? vidow_fx_client_texture(server,
		sb->buffer, sb, &ctex_owned) : NULL;

	/* 可见窗口（= 画框贴着的那块）在取景框里的位置。客户端自己画装饰时（CSD）
	 * 表面比窗口大一圈透明的留白（阴影边），贴到表面边上就是「录出来的框比屏上
	 * 那圈大一圈」；屏幕上那条路用的是同一份 geo（view.c 的 vidow_view_fx_geo）。 */
	struct vidow_fx_geo geo = vidow_view_fx_geo(view);
	int gw = (int)lround((double)geo.w * g.scale);
	int gh = (int)lround((double)geo.h * g.scale);
	if (gw <= 0 || gh <= 0 || geo.src_w <= 0 || geo.src_h <= 0) {
		geo.off_x = 0;
		geo.off_y = 0;
		gw = g.content_w;
		gh = g.content_h;
	}
	struct vidow_fx_target *bfx = &view->fx;
	struct wlr_output *owner = vidow_win_grab_output(server, view);
	double out_scale = owner != NULL && owner->scale > 0.0 ? owner->scale :
		g.scale;
	double bd_scale = grab_backdrop_scale(server, bfx, out_scale);

	struct vidow_look look, blur_look;
	vidow_view_look(&server->config, &view->rule_eff, view->fullscreen, &look);
	/* 底衬那一趟不能把画框 / 阴影 / 噪点再画一遍（阴影是半透明的，画两遍会变
	 * 浓）；只留圆角（`inside` 那块形状）和模糊。 */
	blur_look = look;
	blur_look.border_width = 0.0;
	blur_look.shadow_color[0] = 0.0f;
	blur_look.shadow_color[1] = 0.0f;
	blur_look.shadow_color[2] = 0.0f;
	blur_look.shadow_color[3] = 0.0f;
	blur_look.noise = 0.0;

	struct vidow_capture_decor c = {
		.content = ctex,
		.tex_u0 = (float)((double)(geo.off_x - g.ext.x) / g.ext.width),
		.tex_v0 = (float)((double)(geo.off_y - g.ext.y) / g.ext.height),
		.tex_du = (float)((double)geo.w / g.ext.width),
		.tex_dv = (float)((double)geo.h / g.ext.height),
		.content_x = g.content_x +
			(int)lround((double)(geo.off_x - g.ext.x) * g.scale),
		.content_y = g.content_y +
			(int)lround((double)(geo.off_y - g.ext.y) * g.scale),
		.content_w = gw, .content_h = gh,
		/* 可见窗口左上角的布局坐标：底衬采样按它换算到那块屏的设备像素 */
		.content_layout_x = g.at.x,
		.content_layout_y = g.at.y,
		.frame_scale = g.scale,
		.blur = bfx->blur_tex,
		.blur_x = bfx->blur_tex_x, .blur_y = bfx->blur_tex_y,
		.blur_w = bfx->blur_tex_w, .blur_h = bfx->blur_tex_h,
		/* 没有底衬贴图时（模糊关着）这两项没人用；有贴图时按贴图自己记的输出
		 * （见 server.h 的 blur_out_*）。兜底那两个数按**表面**原点算（镜像树
		 * 的原点就是表面原点，可见窗口在它里面偏 geo.off_*）。 */
		.out_x = bfx->blur_out_w > 0 ? bfx->blur_out_x : g.frame_x - geo.off_x,
		.out_y = bfx->blur_out_h > 0 ? bfx->blur_out_y : g.frame_y - geo.off_y,
		.out_w = bfx->blur_out_w > 0 ? bfx->blur_out_w : g.frame_w,
		.out_h = bfx->blur_out_h > 0 ? bfx->blur_out_h : g.frame_h,
		.out_scale = bd_scale,
		.border = server->focused == view ?
			look.border_color_focus : look.border_color,
	};

	struct wlr_buffer *blur_buf = grab_bake_target(&grab->blur_buf,
		&grab->blur_w, &grab->blur_h, server, g.out_w, g.out_h);
	struct wlr_buffer *edge_buf = grab_bake_target(&grab->edge_buf,
		&grab->edge_w, &grab->edge_h, server, g.out_w, g.out_h);
	bool baked = false;
	if (blur_buf != NULL && edge_buf != NULL) {
		c.dst = blur_buf;
		c.dst_w = g.out_w;
		c.dst_h = g.out_h;
		c.edge = false;
		c.look = &blur_look;
		baked = vidow_fx_capture_decor(server, &c);
		if (baked) {
			c.dst = edge_buf;
			c.edge = true;
			c.look = &look;
			baked = vidow_fx_capture_decor(server, &c);
		}
	}
	if (ctex_owned && ctex != NULL) {
		wlr_texture_destroy(ctex);
	}
	if (!baked) {
		return grab_render_scene(grab, &g, target, false, NULL, NULL);
	}

	/* 两张图都按取景框原点摆、整块按设备像素 1:1 铺满（镜像夹在中间）。 */
	wlr_scene_buffer_set_buffer(grab->decor_blur, blur_buf);
	wlr_scene_node_set_position(&grab->decor_blur->node, 0, 0);
	wlr_scene_buffer_set_dest_size(grab->decor_blur, g.frame_w, g.frame_h);
	wlr_scene_buffer_set_buffer(grab->decor_edge, edge_buf);
	wlr_scene_node_set_position(&grab->decor_edge->node, 0, 0);
	wlr_scene_buffer_set_dest_size(grab->decor_edge, g.frame_w, g.frame_h);

	return grab_render_scene(grab, &g, target, false, grab->decor_blur,
		grab->decor_edge);
}

static void grab_handle_scene_output_destroy(struct wl_listener *listener,
		void *data) {
	(void)data;
	struct vidow_win_grab *grab =
		wl_container_of(listener, grab, scene_output_destroy);
	grab->scene_output = NULL;
	wl_list_remove(&grab->scene_output_destroy.link);
	wl_list_init(&grab->scene_output_destroy.link);
}

/* 窗口没了：镜像那棵子树是 wlroots 的子表面树建的，表面一销毁它就跟着销毁
 * （那一下我们还在它自己的 destroy 信号里，不能在那儿拆整棵场景 —— 会递归回到
 * 正在销毁的节点上，wlr_scene_node_destroy 里断言节点的 destroy 监听表非空，
 * 当场 abort）。这里只记一笔，拆场景留给用家：它注册在**表面**的 destroy 上，
 * 而且注册在我们后面（我们建得更早），轮到它的时候镜像早摘干净了。 */
static void grab_handle_surface_destroy(struct wl_listener *listener,
		void *data) {
	(void)data;
	struct vidow_win_grab *grab =
		wl_container_of(listener, grab, surface_destroy);
	grab->dead = true;
	grab->mirror = NULL;
}

struct vidow_win_grab *vidow_win_grab_create(struct vidow_server *server,
		struct vidow_view *view) {
	if (server == NULL || view == NULL) {
		return NULL;
	}
	struct wlr_surface *surface = vidow_view_surface(view);
	if (surface == NULL) {
		return NULL;
	}

	struct vidow_win_grab *grab = calloc(1, sizeof(*grab));
	if (grab == NULL) {
		return NULL;
	}
	grab->server = server;
	grab->view = view;

	grab->scene = wlr_scene_create();
	if (grab->scene == NULL) {
		free(grab);
		return NULL;
	}
	/* 镜像：表面 + 子表面（弹出物不进来 —— 抓的是这个窗口的画面，不是它的菜单）。
	 * 同一张表面挂两棵树是允许的：wlr_scene_surface 的 addon 挂在场景节点上，
	 * 子表面那棵树是按 (表面, 父树) 认的，两棵树互不干扰。 */
	/* 模糊底衬那张图的落脚点：建在镜像**之前**（场景里同层节点的次序就是创建
	 * 次序），于是它画在内容下面。 */
	grab->decor_blur = wlr_scene_buffer_create(&grab->scene->tree, NULL);
	if (grab->decor_blur != NULL) {
		wlr_scene_node_set_enabled(&grab->decor_blur->node, false);
	}

	grab->mirror = wlr_scene_subsurface_tree_create(&grab->scene->tree, surface);
	if (grab->mirror == NULL) {
		wlr_scene_node_destroy(&grab->scene->tree.node);
		free(grab);
		return NULL;
	}

	wlr_backend_init(&grab->backend, &backend_impl);
	grab->backend.buffer_caps = WLR_BUFFER_CAP_DMABUF | WLR_BUFFER_CAP_SHM;

	wlr_output_init(&grab->output, &grab->backend, &output_impl,
		wl_display_get_event_loop(server->display), NULL);
	wlr_output_set_name(&grab->output, "WINDOW-CAPTURE");
	if (!wlr_output_init_render(&grab->output, server->allocator,
			server->renderer)) {
		wlr_log(WLR_ERROR, "wincapture: 假输出初始化失败");
		wlr_output_finish(&grab->output);
		wlr_backend_finish(&grab->backend);
		wlr_scene_node_destroy(&grab->scene->tree.node);
		free(grab);
		return NULL;
	}

	grab->scene_output = wlr_scene_output_create(grab->scene, &grab->output);
	if (grab->scene_output == NULL) {
		wlr_output_finish(&grab->output);
		wlr_backend_finish(&grab->backend);
		wlr_scene_node_destroy(&grab->scene->tree.node);
		free(grab);
		return NULL;
	}
	/* 画框那张图的落脚点：建在镜像**之后** —— 画框要压在内容上面（屏上就是
	 * 这个次序：边框描在窗口内容的外沿）。 */
	grab->decor_edge = wlr_scene_buffer_create(&grab->scene->tree, NULL);
	if (grab->decor_edge != NULL) {
		wlr_scene_node_set_enabled(&grab->decor_edge->node, false);
	}

	/* 假输出的原点钉在 (0,0)：镜像按外接框摆正，抓出来的就是「窗口本身」这块图，
	 * 跟它在屏幕上摆在哪没关系。 */
	wlr_scene_output_set_position(grab->scene_output, 0, 0);

	grab->surface_destroy.notify = grab_handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &grab->surface_destroy);

	grab->scene_output_destroy.notify = grab_handle_scene_output_destroy;
	wl_signal_add(&grab->scene_output->events.destroy,
		&grab->scene_output_destroy);

	return grab;
}

void vidow_win_grab_destroy(struct vidow_win_grab *grab) {
	if (grab == NULL) {
		return;
	}
	wl_list_remove(&grab->surface_destroy.link);
	wl_list_remove(&grab->scene_output_destroy.link);
	if (grab->scene_output != NULL) {
		wlr_scene_output_destroy(grab->scene_output);
		grab->scene_output = NULL;
	}
	wlr_output_finish(&grab->output);
	wlr_backend_finish(&grab->backend);
	wlr_buffer_drop(grab->blur_buf);
	wlr_buffer_drop(grab->edge_buf);
	wlr_scene_node_destroy(&grab->scene->tree.node);
	free(grab);
}

/* ------------------------------------------- 截图源（ext-copy 的窗口源） */

/* 这个窗口的「取景」算在哪块屏上（跟 screencast.c 的 handle_stream_window 共用
 * 这一份）：
 *   平铺的      看**它那条列**挂在哪块屏（view->column->output）—— 跟滚动无关；
 *   浮动 / 弹窗 它们不参与滚动（geom 就是自己的绝对位置），按现在画在哪块屏上
 *               反查（拖着跨屏时 view->output 还停在原来那块屏上，见 fx.c 里
 *               「窗口画在哪块屏上」那段）。
 *
 * 为什么平铺的不能也按「现在画在哪」反查：平铺窗口的 geom 里含着**滚动视口的
 * 偏移**（view.c 的 output_arrange：x = 工作区 + 列偏移 - 视口）。窗口滚出视口
 * 之后它画的那些像素落在隔壁屏的范围里，中心反查出来就是隔壁那块 —— 这一路流
 * （和窗口抓帧源）就挂到错那块屏上，后果三件（用户报的「非 0 那块屏上录屏看不到
 * 光标」，实测）：
 *   ① 光标去错那块屏的光标列表里找：指针明明在窗口上，也永远找不到，元数据里
 *      一直 id=0 —— 消费者那边就是「有画面、光标不进来」；
 *   ② 流尺寸乘的是错那块屏的 scale（2 倍屏上分辨率只有一半，图是糊的）；
 *   ③ 输出提交那条抓帧路盯的也是错那块屏。
 * 窗口滚回视口、重新对齐到自己那块屏之后，**已经建好的流不会跟着换回来** ——
 * 所以判据必须是跟滚动无关的那一个。
 *
 * 为什么两边必须一致：合成器给 zkde 窗口流开的缓冲尺寸是「窗口逻辑尺寸 × 这块屏
 * 的 scale」（设备像素），而 portal 报给前端的 size 是从**窗口抓帧源的约束**来的
 * （vidow-portal 拿缩略图的像素尺寸当 size）。两边用不同的屏 / 口径算，报出去的
 * size 就跟真实缓冲对不上 —— 消费者（OBS）按比值换算光标位置，2 倍屏上就是
 * 「录屏里看不到光标」（位置判越界，不画）。 */
struct wlr_output *vidow_win_grab_output(struct vidow_server *server,
		struct vidow_view *view) {
	if (server == NULL || view == NULL) {
		return NULL;
	}
	struct wlr_output *owner = vidow_view_owner_output(view);
	if (owner != NULL) {
		return owner;
	}
	struct wlr_box at = vidow_view_screen_box(view);
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, at.x + at.width / 2, at.y + at.height / 2);
	return output != NULL ? output : view->output;
}

struct vidow_win_source {
	struct wlr_ext_image_capture_source_v1 base;

	struct vidow_server *server;
	struct vidow_view *view;
	struct vidow_win_grab *grab;

	/* 这一帧渲染出来的那张（copy_frame 紧接着就来拿）。 */
	struct wlr_buffer *pending;

	/* 上次发出去的约束尺寸：变了才重发（constraints_update 是无条件发的，
	 * 每帧发一遍客户端会不停重建会话）。 */
	int last_w, last_h;
	bool have_constraints;
	size_t num_started;

	struct wl_listener surface_commit;
	struct wl_listener surface_destroy;
};

/* 放掉上一帧那张缓冲。**必须置空**：这张缓冲的解锁路径有三条（下一次抓帧、
 * 会话停、源销毁），而源销毁那一条会连锁触发会话的 stop —— 不置空就是解两次
 * （wlr_buffer_unlock 断言 n_locks > 0，踩过：关掉一个正在被录的窗口，合成器当场
 * abort）。 */
static void source_drop_pending(struct vidow_win_source *src) {
	wlr_buffer_unlock(src->pending);
	src->pending = NULL;
}

/* 约束（尺寸 / 格式 / 设备）：客户端拿它建自己的缓冲。约束是从假输出的交换链
 * 来的，而交换链要**渲染那一趟**才配得出来（wlr_scene_output_build_state 内部按
 * state 的模式配），所以这个只在渲染成功后调。尺寸没变就不重发 ——
 * constraints_update 是无条件发的，每帧发一遍客户端会不停重建会话。 */
static void source_send_constraints(struct vidow_win_source *src) {
	struct wlr_swapchain *swapchain = vidow_win_grab_swapchain(src->grab);
	if (swapchain == NULL) {
		return;
	}
	int w = swapchain->width, h = swapchain->height;
	if (src->have_constraints && src->last_w == w && src->last_h == h) {
		return;
	}
	wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&src->base,
		swapchain, src->grab->output.renderer);
	vidow_capture_fix_shm_formats(&src->base);
	src->have_constraints = true;
	src->last_w = w;
	src->last_h = h;
}

/* 渲染一帧（窗口自己多大就多大 **× 它那块屏的 scale**），挂到 src->pending 上。
 *
 * 为什么要乘 scale：客户端拿到的约束（尺寸）就是这张图的尺寸，而 vidow-portal 把
 * 它当「这一路流的 size」报给前端 —— 跟 zkde 窗口流的缓冲尺寸（同样是
 * 逻辑 × scale，见 screencast.c）必须一个口径。乘了 scale 之后 2 倍屏上的窗口
 * 缩略图 / 截图也是原生像素，不再是糊的。 */
static void win_source_size(struct vidow_win_source *src, int *w, int *h) {
	int gw = 0, gh = 0;
	if (src->grab == NULL || !vidow_win_grab_box(src->grab, &gw, &gh)) {
		*w = 0;
		*h = 0;
		return;
	}
	double scale = 1.0;
	struct wlr_output *output = vidow_win_grab_output(src->server, src->view);
	if (output != NULL && output->scale > 0) {
		scale = output->scale;
	}
	*w = (int)lround(gw * scale);
	*h = (int)lround(gh * scale);
}

static bool source_render(struct vidow_win_source *src) {
	int w = 0, h = 0;
	win_source_size(src, &w, &h);
	struct wlr_buffer *buffer = vidow_win_grab_render(src->grab, w, h, NULL);
	if (buffer == NULL) {
		return false;
	}
	source_drop_pending(src);
	src->pending = buffer;
	source_send_constraints(src);
	return true;
}

/* ------------------------------------------------------------ 协议接口 */

static void source_start(struct wlr_ext_image_capture_source_v1 *base,
		bool with_cursors) {
	(void)with_cursors;
	struct vidow_win_source *src = wl_container_of(base, src, base);

	src->num_started++;
	if (src->num_started > 1) {
		return;
	}
	/* 先渲染一趟：约束（尺寸 / 格式）就是这一趟配出来的，而且得赶在协议层
	 * 初始化会话的损伤账本之前 —— 那个账本用的是 source->width/height，尺寸是
	 * 0 的话第一帧会被判成「没有损伤」而不拷给客户端。窗口还没内容的话这一趟
	 * 什么都不出，等它提交（见 surface_commit）。 */
	source_render(src);
}

static void source_stop(struct wlr_ext_image_capture_source_v1 *base) {
	struct vidow_win_source *src = wl_container_of(base, src, base);

	if (src->num_started > 0) {
		src->num_started--;
	}
	if (src->num_started > 0) {
		return;
	}
	source_drop_pending(src);
}

/* 客户端说「给我一帧」：**现在就渲染一帧**。这是跟 wlroots 那份最大的区别 ——
 * 它按损伤出帧，静止的窗口就没有帧可给，客户端会一直等下去。 */
static void source_request_frame(struct wlr_ext_image_capture_source_v1 *base,
		bool schedule_frame) {
	(void)schedule_frame;
	struct vidow_win_source *src = wl_container_of(base, src, base);

	source_render(src);

	/* 没渲染出来也要发事件：客户端在等 ready / failed，闷着就是卡死
	 * （copy_frame 看到 pending 为空会回一个 failed，客户端于是会重试）。 */
	int w = src->last_w > 0 ? src->last_w : 1;
	int h = src->last_h > 0 ? src->last_h : 1;
	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, w, h);
	struct wlr_ext_image_capture_source_v1_frame_event event = {
		.damage = &damage,
	};
	wl_signal_emit_mutable(&src->base.events.frame, &event);
	pixman_region32_fini(&damage);
}

static void source_copy_frame(struct wlr_ext_image_capture_source_v1 *base,
		struct wlr_ext_image_copy_capture_frame_v1 *frame,
		struct wlr_ext_image_capture_source_v1_frame_event *event) {
	(void)event;
	struct vidow_win_source *src = wl_container_of(base, src, base);

	if (src->pending == NULL) {
		wlr_ext_image_copy_capture_frame_v1_fail(frame,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
		return;
	}
	/* 客户端缓冲跟这一帧对不上：多半是窗口挪到缩放不同的屏上了（尺寸跟着
	 * 变，见 win_source_size），而它手上那块缓冲是按上一版约束建的。约束我们
	 * 在 source_render 里已经重发过了，客户端换掉缓冲、再要一帧就好 —— 这一帧
	 * 由 copy_buffer 内部回 failed。这里先记一笔，免得日志里只剩一句「拷不进去」。 */
	if (frame->buffer != NULL &&
			(frame->buffer->width != src->pending->width ||
			 frame->buffer->height != src->pending->height)) {
		wlr_log(WLR_INFO,
			"wincapture: %s 这一帧 %dx%d，客户端缓冲 %dx%d —— 尺寸变了，这一帧回 failed（等它换缓冲）",
			grab_label(src->grab), src->pending->width, src->pending->height,
			frame->buffer->width, frame->buffer->height);
	}
	if (wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame, src->pending,
			src->grab->output.renderer)) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		/* 渲染出来的是正立的图，所以报 NORMAL（不是输出 / 表面的 transform）。 */
		wlr_ext_image_copy_capture_frame_v1_ready(frame,
			WL_OUTPUT_TRANSFORM_NORMAL, &now);
		return;
	}
	/* 走到这儿 copy_buffer 已经回过 failed 并且**把 frame 销毁了**，不能再补一句：
	 * 那是拿一块已经 free 的内存去 wl_resource_post_event，当场 SEGV（wlroots
	 * types/wlr_ext_image_copy_capture_v1.c：尺寸对不上、格式不在约束里、缓冲拷不动
	 * 三条路都是 `fail(); return false;` —— 返回 false 就等于「已经回过了」）。
	 * 踩过：选择器里缩略图抓帧正撞上窗口换屏（尺寸变），合成器直接崩。 */
}

static const struct wlr_ext_image_capture_source_v1_interface source_impl = {
	.start = source_start,
	.stop = source_stop,
	.request_frame = source_request_frame,
	.copy_frame = source_copy_frame,
};

/* ------------------------------------------------------------ 生命周期 */

static void source_destroy(struct vidow_win_source *src) {
	source_drop_pending(src);
	wl_list_remove(&src->surface_commit.link);
	wl_list_remove(&src->surface_destroy.link);
	/* 拆私有场景要在**表面**的 destroy 里做，不能等镜像节点自己报销毁
	 * （见 grab_handle_surface_destroy 上面那段）。这个用家注册得比 grab 晚，
	 * 所以轮到我们时镜像已经摘干净了。 */
	vidow_win_grab_destroy(src->grab);
	src->grab = NULL;
	wlr_ext_image_capture_source_v1_finish(&src->base);
	free(src);
}

static void source_handle_surface_destroy(struct wl_listener *listener,
		void *data) {
	(void)data;
	struct vidow_win_source *src =
		wl_container_of(listener, src, surface_destroy);
	source_destroy(src);
}

/* 窗口提交了新内容：还没给出尺寸（开窗那一瞬间就有人来抓）就补一趟；尺寸变了
 * （改窗口大小）也在这儿重发约束 —— 两件事都要渲染那一趟才做得到。 */
static void source_handle_surface_commit(struct wl_listener *listener,
		void *data) {
	(void)data;
	struct vidow_win_source *src =
		wl_container_of(listener, src, surface_commit);
	if (src->num_started == 0) {
		return;
	}
	/* 比的是**报出去的那个尺寸**（逻辑 × scale），跟 source_render 一套算法：
	 * 拿逻辑尺寸比的话，换屏（scale 变了、逻辑没变）就不会重发约束。 */
	int w = 0, h = 0;
	win_source_size(src, &w, &h);
	if (w <= 0 || h <= 0) {
		return;
	}
	if (src->have_constraints && src->last_w == w && src->last_h == h) {
		return;
	}
	source_render(src);
}

struct wlr_ext_image_capture_source_v1 *vidow_win_capture_source_create(
		struct vidow_server *server, struct vidow_view *view) {
	if (server == NULL || view == NULL) {
		return NULL;
	}
	struct wlr_surface *surface = vidow_view_surface(view);
	if (surface == NULL) {
		return NULL;
	}

	struct vidow_win_source *src = calloc(1, sizeof(*src));
	if (src == NULL) {
		return NULL;
	}
	src->server = server;
	src->view = view;
	src->grab = vidow_win_grab_create(server, view);
	if (src->grab == NULL) {
		free(src);
		return NULL;
	}

	wlr_ext_image_capture_source_v1_init(&src->base, &source_impl);

	src->surface_commit.notify = source_handle_surface_commit;
	wl_signal_add(&surface->events.commit, &src->surface_commit);

	src->surface_destroy.notify = source_handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &src->surface_destroy);

	return &src->base;
}
