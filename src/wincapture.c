/* wincapture.c — 按窗口录屏 / 预览的截图源（ext-image-capture-source-v1 的窗口源）。
 *
 * 为什么不用 wlroots 那份（wlr_ext_image_capture_source_v1_create_with_scene_node）：
 * 它给「窗口的子树」在**同一张场景图**里、按节点的绝对坐标摆一个虚拟输出出来渲染
 * （types/ext_image_capture_source_v1/scene.c）。三个后果都踩过：
 *
 *   1) 抓帧是**损伤驱动**的：虚拟输出那一趟只在场景有损伤时才渲染（"We can only
 *      emit frames with damage"）。窗口不动、客户端也不重绘的时候（一个静止的
 *      终端、一份静止的预览），客户端 `capture` 要的那一帧**永远不来** ——
 *      选择器的预览卡在「等第一帧」（客户端那边 busy 一直是 true、之后不再重试，
 *      所以把窗口挪进视口也还是没预览），OBS 那边一会儿 streaming 一会儿 paused，
 *      error 还是 none（协议里没这个错，就是没帧）。量过：静止窗口滚到视口外，
 *      抓 3 秒只出 1 帧（第一帧），一直在动的窗口同样 3 秒 89 帧。
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
 *   + 客户端要一帧就渲染一帧（不看损伤）。
 *
 * 渲染出来的是**客户端表面本身**：没有我们的装饰、模糊、圆角、阴影 —— 和
 * Hyprland 的窗口导出一样（"will not contain any server-side decorations"）。
 * 好的副作用：抓帧既然跟真实节点没关系了，屏幕上那一份就可以随便裁 —— 跨屏的
 * 那半块裁掉（不再串屏，见 view.c 的 view_clip_box），停到屏外也不影响抓帧。
 */

#include "server.h"

#include <limits.h>
#include <stdlib.h>
#include <time.h>

#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>

#include "ext-image-capture-source-v1-server-protocol.h"

struct vidow_win_source {
	struct wlr_ext_image_capture_source_v1 base;

	struct vidow_server *server;
	struct vidow_view *view;

	/* 私有场景：里面只有这个窗口的一份镜像。 */
	struct wlr_scene *scene;
	struct wlr_scene_tree *mirror;

	/* 假后端 + 假输出：只是给 scene_output 一个落脚的地方，永远不真的提交。 */
	struct wlr_backend backend;
	struct wlr_output output;
	struct wlr_scene_output *scene_output;

	/* 这一帧渲染出来的那张（copy_frame 紧接着就来拿）。 */
	struct wlr_buffer *pending;

	/* 上次发出去的约束尺寸：变了才重发（constraints_update 是无条件发的，
	 * 每帧发一遍客户端会不停重建会话）。 */
	int last_w, last_h;
	bool have_constraints;
	size_t num_started;

	struct wl_listener surface_commit;
	struct wl_listener surface_destroy;
	struct wl_listener scene_output_destroy;
};

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

/* 放掉上一帧那张缓冲。**必须置空**：这张缓冲的解锁路径有三条（下一次抓帧、
 * 会话停、源销毁），而源销毁那一条会连锁触发会话的 stop —— 不置空就是解两次
 * （wlr_buffer_unlock 断言 n_locks > 0，踩过：关掉一个正在被录的窗口，合成器当场
 * abort）。 */
static void source_drop_pending(struct vidow_win_source *src) {
	wlr_buffer_unlock(src->pending);
	src->pending = NULL;
}

/* 日志里那个名字。窗口可能已经没了（表面销毁和我们这一趟在同一个事件循环轮次里，
 * 视图先走一步）—— 那就不碰它，别在日志上读一块已经 free 的内存。 */
static const char *source_label(struct vidow_win_source *src) {
	struct vidow_view *view;
	bool alive = false;
	if (src->view != NULL) {
		wl_list_for_each(view, &src->server->views, link) {
			if (view == src->view) {
				alive = true;
				break;
			}
		}
	}
	const char *title = alive ? vidow_view_title(src->view) : NULL;
	return title != NULL ? title : "(无题)";
}

/* ---------------------------------------------------------- 镜像的外接框 */

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

/* 这一帧要抓的那块框（宽高 = 镜像的外接框，原点在 (0,0)）。镜像树整体挪到
 * (-x_min,-y_min)：子表面可能带负偏移（xdg 的 geometry 补偿），不挪的话抓到的
 * 图左边 / 上边会缺一条。没内容（客户端还没提交过缓冲）就返回 false。 */
static bool mirror_box(struct vidow_win_source *src, struct wlr_box *box) {
	int x_min = INT_MAX, y_min = INT_MAX;
	int x_max = INT_MIN, y_max = INT_MIN;
	node_extents(&src->mirror->node, 0, 0, &x_min, &y_min, &x_max, &y_max);
	if (x_max <= x_min || y_max <= y_min) {
		return false;
	}
	wlr_scene_node_set_position(&src->mirror->node, -x_min, -y_min);
	*box = (struct wlr_box){
		.x = 0, .y = 0,
		.width = x_max - x_min, .height = y_max - y_min,
	};
	return true;
}

/* ---------------------------------------------------------------- 出帧 */

/* 约束（尺寸 / 格式 / 设备）：客户端拿它建自己的缓冲。约束是从假输出的交换链
 * 来的，而交换链要**渲染那一趟**才配得出来（wlr_scene_output_build_state 内部按
 * state 的模式配），所以这个只在渲染成功后调。尺寸没变就不重发 ——
 * constraints_update 是无条件发的，每帧发一遍客户端会不停重建会话。 */
static void source_send_constraints(struct vidow_win_source *src) {
	struct wlr_output *output = &src->output;
	if (output->swapchain == NULL) {
		return;
	}
	int w = output->swapchain->width, h = output->swapchain->height;
	if (src->have_constraints && src->last_w == w && src->last_h == h) {
		return;
	}
	wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&src->base,
		output->swapchain, output->renderer);
	src->have_constraints = true;
	src->last_w = w;
	src->last_h = h;
}

/* 渲染一帧，返回**加锁的**缓冲（调用方负责 unlock）。 */
static struct wlr_buffer *source_render(struct vidow_win_source *src) {
	struct wlr_box box = {0};
	if (!mirror_box(src, &box)) {
		return NULL;
	}
	struct wlr_output *output = &src->output;
	output->width = box.width;
	output->height = box.height;

	/* 整块重画：这块假输出没有输出级的损伤记账，整块最省心（一次抓帧本来也
	 * 要读整块），也免得损伤环里没脏东西时渲染那一趟一个像素都不画 —— 那样
	 * 拿回来的是两张交换链缓冲里更旧的那张。 */
	pixman_region32_t whole;
	pixman_region32_init_rect(&whole, 0, 0, box.width, box.height);
	wlr_damage_ring_add(&src->scene_output->damage_ring, &whole);
	pixman_region32_fini(&whole);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_state_set_custom_mode(&state, box.width, box.height, 0);

	struct wlr_buffer *buffer = NULL;
	if (wlr_scene_output_build_state(src->scene_output, &state, NULL)) {
		buffer = state.buffer;
		if (buffer != NULL) {
			wlr_buffer_lock(buffer);
		}
	}
	wlr_output_state_finish(&state);

	if (buffer == NULL) {
		wlr_log(WLR_ERROR, "wincapture: %s 渲染失败（%dx%d）",
			source_label(src), box.width, box.height);
		return NULL;
	}

	/* 被抓的这个窗口得拿到帧回调，它才会接着画下一帧（跟画在屏幕上时一样）。 */
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(src->scene_output, &now);

	source_send_constraints(src);
	return buffer;
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

	source_drop_pending(src);
	src->pending = source_render(src);

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
	if (wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame, src->pending,
			src->output.renderer)) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		/* 渲染出来的是正立的图，所以报 NORMAL（不是输出 / 表面的 transform）。 */
		wlr_ext_image_copy_capture_frame_v1_ready(frame,
			WL_OUTPUT_TRANSFORM_NORMAL, &now);
		return;
	}
	/* 拷不进去（客户端缓冲尺寸对不上之类）：也得回一句，不然它一直等。 */
	wlr_ext_image_copy_capture_frame_v1_fail(frame,
		EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
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
	wl_list_remove(&src->scene_output_destroy.link);
	wlr_ext_image_capture_source_v1_finish(&src->base);
	wlr_scene_output_destroy(src->scene_output);
	wlr_output_finish(&src->output);
	wlr_backend_finish(&src->backend);
	wlr_scene_node_destroy(&src->scene->tree.node);
	free(src);
}

/* 窗口没了（表面销毁）：把私有场景整个拆掉。
 *
 * 监听的是**表面**的销毁，不是镜像节点自己的 —— 镜像那棵子树是 wlroots 的
 * 子表面树建的，表面一销毁它就跟着销毁，而那时候我们还在它的 destroy 信号里：
 * 在那儿去销毁整棵场景会**递归**回到这个正在销毁的节点上（踩过：关掉一个正在被
 * 录的窗口，wlr_scene_node_destroy 里断言节点上的 destroy 监听表非空，当场 abort）。
 * 表面这个信号上，wlroots 那两个监听（场景表面的、子表面树的）都注册在我们前面，
 * 轮到我们的时候镜像早就摘干净了。 */
static void source_handle_surface_destroy(struct wl_listener *listener,
		void *data) {
	(void)data;
	struct vidow_win_source *src =
		wl_container_of(listener, src, surface_destroy);
	source_destroy(src);
}

static void source_handle_scene_output_destroy(struct wl_listener *listener,
		void *data) {
	(void)data;
	struct vidow_win_source *src =
		wl_container_of(listener, src, scene_output_destroy);
	src->scene_output = NULL;
	wl_list_remove(&src->scene_output_destroy.link);
	wl_list_init(&src->scene_output_destroy.link);
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
	struct wlr_box box = {0};
	if (!mirror_box(src, &box)) {
		return;
	}
	if (src->have_constraints && src->last_w == box.width &&
			src->last_h == box.height) {
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

	wlr_ext_image_capture_source_v1_init(&src->base, &source_impl);

	src->scene = wlr_scene_create();
	if (src->scene == NULL) {
		wlr_ext_image_capture_source_v1_finish(&src->base);
		free(src);
		return NULL;
	}
	/* 镜像：表面 + 子表面（弹出物不进来 —— 抓的是这个窗口的画面，不是它的菜单）。
	 * 同一张表面挂两棵树是允许的：wlr_scene_surface 的 addon 挂在场景节点上，
	 * 子表面那棵树是按 (表面, 父树) 认的，两棵树互不干扰。 */
	src->mirror = wlr_scene_subsurface_tree_create(&src->scene->tree, surface);
	if (src->mirror == NULL) {
		wlr_scene_node_destroy(&src->scene->tree.node);
		wlr_ext_image_capture_source_v1_finish(&src->base);
		free(src);
		return NULL;
	}

	wlr_backend_init(&src->backend, &backend_impl);
	src->backend.buffer_caps = WLR_BUFFER_CAP_DMABUF | WLR_BUFFER_CAP_SHM;

	wlr_output_init(&src->output, &src->backend, &output_impl,
		wl_display_get_event_loop(server->display), NULL);
	wlr_output_set_name(&src->output, "WINDOW-CAPTURE");
	if (!wlr_output_init_render(&src->output, server->allocator,
			server->renderer)) {
		wlr_log(WLR_ERROR, "wincapture: 假输出初始化失败");
		wlr_output_finish(&src->output);
		wlr_backend_finish(&src->backend);
		wlr_scene_node_destroy(&src->scene->tree.node);
		wlr_ext_image_capture_source_v1_finish(&src->base);
		free(src);
		return NULL;
	}

	src->scene_output = wlr_scene_output_create(src->scene, &src->output);
	if (src->scene_output == NULL) {
		wlr_output_finish(&src->output);
		wlr_backend_finish(&src->backend);
		wlr_scene_node_destroy(&src->scene->tree.node);
		wlr_ext_image_capture_source_v1_finish(&src->base);
		free(src);
		return NULL;
	}
	/* 假输出的原点就钉在 (0,0)：镜像树已经挪到外接框的左上了，抓出来的就是
	 * 「窗口本身」这块图，跟它在屏幕上摆在哪没关系。 */
	wlr_scene_output_set_position(src->scene_output, 0, 0);

	src->surface_commit.notify = source_handle_surface_commit;
	wl_signal_add(&surface->events.commit, &src->surface_commit);

	src->surface_destroy.notify = source_handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &src->surface_destroy);

	src->scene_output_destroy.notify = source_handle_scene_output_destroy;
	wl_signal_add(&src->scene_output->events.destroy,
		&src->scene_output_destroy);

	return &src->base;
}
