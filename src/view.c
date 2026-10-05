#include "server.h"
#include "ipc.h"

#include <math.h>
#include <stdlib.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

#define DEFAULT_COLUMN_WIDTH 0.5

/* gap 的算法都在 server.h 里（vidow_gap_side / vidow_gap_inner）：统一值 gaps
 * 管全部，单边设了就用单边的，-1 = 跟随。 */

void vidow_place_view(struct vidow_view *view, int x, int y, int w, int h);
static void view_ensure_column(struct vidow_server *server,
		struct vidow_view *view);
static void view_leave_column(struct vidow_view *view);
/* X11：按窗口的绝对坐标把它归到某块输出上（弹出物可以开在另一块屏上）。 */
static void x11_track_output(struct vidow_view *view);

void vidow_view_join_layout(struct vidow_server *server,
		struct vidow_view *view) {
	view_ensure_column(server, view);
}

void vidow_view_leave_layout(struct vidow_view *view) {
	view_leave_column(view);
}

/* ------------------------------------------------------------- lookup */

static struct vidow_view *view_from_surface(struct vidow_server *server,
		struct wlr_surface *surface) {
	if (surface == NULL) {
		return NULL;
	}

	struct wlr_xwayland_surface *xsurface =
		wlr_xwayland_surface_try_from_wlr_surface(surface);
	if (xsurface != NULL) {
		struct vidow_view *view;
		wl_list_for_each(view, &server->views, link) {
			if (view->kind == VIDOW_VIEW_X11 && view->x11 == xsurface) {
				return view;
			}
		}
		return NULL;
	}

	struct wlr_xdg_surface *xdg_surface =
		wlr_xdg_surface_try_from_wlr_surface(surface);
	if (xdg_surface == NULL) {
		return NULL;
	}

	if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		return view_from_surface(server, xdg_surface->popup->parent);
	}
	if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return NULL;
	}

	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->xdg_toplevel == xdg_surface->toplevel) {
			return view;
		}
	}
	return NULL;
}

/* -------------------------------------------------- 窗口访问器（两种壳） */

/* Wayland 窗口和 X11 窗口的差别全部收在这里。布局、装饰、动画、输入法、
 * portal 都只认 vidow_view 这一个类型。 */

struct wlr_surface *vidow_view_surface(const struct vidow_view *view) {
	if (view == NULL) {
		return NULL;
	}
	if (view->kind == VIDOW_VIEW_X11) {
		return view->x11 != NULL ? view->x11->surface : NULL;
	}
	return view->xdg_toplevel != NULL ? view->xdg_toplevel->base->surface :
		NULL;
}

const char *vidow_view_title(const struct vidow_view *view) {
	if (view == NULL) {
		return NULL;
	}
	if (view->kind == VIDOW_VIEW_X11) {
		return view->x11 != NULL ? view->x11->title : NULL;
	}
	return view->xdg_toplevel != NULL ? view->xdg_toplevel->title : NULL;
}

const char *vidow_view_app_id(const struct vidow_view *view) {
	if (view == NULL) {
		return NULL;
	}
	if (view->kind == VIDOW_VIEW_X11) {
		if (view->x11 == NULL) {
			return NULL;
		}
		return view->x11->class != NULL ? view->x11->class : view->x11->instance;
	}
	return view->xdg_toplevel != NULL ? view->xdg_toplevel->app_id : NULL;
}

/* 事件用的名字快照：window-closed 来的时候客户端那边的标题已经收走了。 */
void vidow_view_note_names(struct vidow_view *view) {
	const char *title = vidow_view_title(view);
	const char *app_id = vidow_view_app_id(view);
	snprintf(view->last_title, sizeof(view->last_title), "%s",
		title != NULL ? title : "");
	snprintf(view->last_app_id, sizeof(view->last_app_id), "%s",
		app_id != NULL ? app_id : "");
}

struct wlr_box vidow_view_geometry(const struct vidow_view *view) {
	if (view->kind == VIDOW_VIEW_X11) {
		/* X11 窗口没有「窗口几何」和「表面」的区别：整块就是内容。 */
		struct wlr_box box = {0};
		if (view->x11 != NULL) {
			int scale = vidow_view_x11_scale_int(view);
			box.width = view->x11->width / scale;
			box.height = view->x11->height / scale;
		}
		return box;
	}
	if (view->xdg_toplevel == NULL) {
		return (struct wlr_box){0};
	}
	return view->xdg_toplevel->base->geometry;
}

/* 可见窗口在源缓冲里的位置/尺寸，逻辑单位（见 struct vidow_fx_geo）。
 *
 * 客户端用 xdg_surface.set_window_geometry 告诉我们「表面里哪一块是窗口」：
 * 自己画装饰的客户端（GTK / kitty）把表面做大一圈画阴影，窗口尺寸和偏移就是
 * 那圈留白。装饰、模糊区域、输入法候选框全都按可见窗口定位，留白永远在窗口
 * 外面 —— 这是 niri 的 LayoutElement::size() / buf_loc() 那一套。
 *
 * X11 没有这个区别：整块就是内容。 */
struct vidow_fx_geo vidow_view_fx_geo(const struct vidow_view *view) {
	struct vidow_fx_geo geo = {0};
	struct wlr_box g = vidow_view_geometry(view);
	if (view->kind == VIDOW_VIEW_X11) {
		geo.w = g.width;
		geo.h = g.height;
		geo.src_w = g.width;
		geo.src_h = g.height;
		return geo;
	}
	struct wlr_surface *surface = vidow_view_surface(view);
	int sw = surface != NULL ? surface->current.width : 0;
	int sh = surface != NULL ? surface->current.height : 0;
	if (g.width <= 0 || g.height <= 0) {
		/* 客户端没声明窗口几何：wlroots 会给「整块表面」这个默认值，但万一
		 * 拿到个空的，就当整块表面是窗口（老行为）。 */
		geo.w = sw;
		geo.h = sh;
		geo.src_w = sw;
		geo.src_h = sh;
		return geo;
	}
	if (sw <= 0 || sh <= 0) {
		/* 表面尺寸还没来（第一帧）：留白那圈按几何推断，别采到缓冲外面。 */
		sw = g.x + g.width;
		sh = g.y + g.height;
	}
	geo.off_x = g.x;
	geo.off_y = g.y;
	geo.w = g.width;
	geo.h = g.height;
	geo.src_w = sw;
	geo.src_h = sh;
	return geo;
}

/* X11 的像素 = 「参考设备像素」，也就是逻辑坐标乘 xwayland_scale。给客户端的
 * 一切都按这个换算：告诉它窗口多大、在哪，读回来的位置尺寸再除回来。
 *
 * 全局那个倍数（server->xwayland_scale）是「假设 X 客户端都按 GDK_SCALE /
 * QT_SCALE_FACTOR 画了 N 倍」。不认这套 env 的客户端（微信的登录窗、CEF 系）
 * 画的还是 1 倍，除下去就小了 N 倍（N=2 时面积只剩 1/4）—— 这种窗口用
 * 窗口规则 x11_scale = 1 逐窗口改，见 rule.c。 */
int vidow_view_x11_scale_int(const struct vidow_view *view) {
	double scale = view->x11_scale;
	if (!(scale > 0.0)) {
		scale = view->server->xwayland_scale;
	}
	int n = (int)lround(scale);
	return n > 0 ? n : 1;
}

/* 这个窗口是不是被逐窗口规则改了缩放桥（用来在日志里说清楚为什么它和别的
 * X11 窗口不一样大）。 */
bool vidow_view_x11_scale_overridden(const struct vidow_view *view) {
	return view->x11_scale > 0.0;
}

/* X 像素 -> 逻辑坐标。C 的整数除法是向零取整，屏幕左边/上边（负坐标）会算偏
 * 一格，弹出物定位要向下取整。 */
static int x11_floor_div(int value, int scale) {
	if (scale <= 0) {
		return value;
	}
	return value >= 0 ? value / scale : -((-value + scale - 1) / scale);
}

struct wlr_surface *vidow_view_parent_surface(const struct vidow_view *view) {
	if (view->kind == VIDOW_VIEW_X11) {
		if (view->x11 == NULL || view->x11->parent == NULL) {
			return NULL;
		}
		return view->x11->parent->surface;
	}
	if (view->xdg_toplevel == NULL || view->xdg_toplevel->parent == NULL) {
		return NULL;
	}
	return view->xdg_toplevel->parent->base->surface;
}

/* 客户端自己要的尺寸，逻辑单位。 */
void vidow_view_client_size(const struct vidow_view *view, int *w, int *h) {
	int width = 0, height = 0;
	if (view->kind == VIDOW_VIEW_X11) {
		if (view->x11 != NULL && view->x11->width > 0 &&
				view->x11->height > 0) {
			int scale = vidow_view_x11_scale_int(view);
			width = view->x11->width / scale;
			height = view->x11->height / scale;
		}
	} else if (view->xdg_toplevel != NULL) {
		struct wlr_box geo = vidow_view_geometry(view);
		width = geo.width;
		height = geo.height;
		if (width <= 0 || height <= 0) {
			width = view->xdg_toplevel->base->surface->current.width;
			height = view->xdg_toplevel->base->surface->current.height;
		}
	}
	*w = width;
	*h = height;
}

void vidow_view_set_size(struct vidow_view *view, int w, int h) {
	if (!vidow_view_configure_ready(view)) {
		/* 客户端第一帧还没来：几何先记着，初始 configure 由 commit 那边发。 */
		return;
	}

	if (view->kind == VIDOW_VIEW_X11) {
		if (view->popup) {
			/* 菜单、提示这类窗口自己决定在哪、多大，别去指挥它。 */
			return;
		}
		if (w <= 0 || h <= 0) {
			/* 「尺寸客户端自己定」（浮动窗口的常规语义，xdg 那边回 0x0）。
			 * X11 没有这个约定，而 configure 一定会带上宽高 —— 用客户端
			 * *当前* 的尺寸回过去是错的：它这时候还是 map 之前的 1x1，
			 * 于是我们等于命令它「保持 1x1」，GTK 的对话框就再也长不出来了。
			 * 所以这里什么都不发，让它先按自己的尺寸上屏，尺寸由
			 * vidow_float_on_commit 从 X 那边读回来（x11->width/height），
			 * 之后 arrange 会带着真实尺寸再 configure 一次位置。 */
			return;
		}
		int scale = vidow_view_x11_scale_int(view);
		int cw = w * scale;
		int ch = h * scale;
		if (cw < 1) {
			cw = 1;
		}
		if (ch < 1) {
			ch = 1;
		}
		wlr_xwayland_surface_configure(view->x11,
			(int16_t)(view->geom.x * scale), (int16_t)(view->geom.y * scale),
			(uint16_t)cw, (uint16_t)ch);
		return;
	}

	wlr_xdg_toplevel_set_size(view->xdg_toplevel, w, h);
}

void vidow_view_set_tiled(struct vidow_view *view, bool tiled) {
	if (view->kind != VIDOW_VIEW_XDG || view->xdg_toplevel == NULL) {
		return;
	}
	if (!vidow_view_configure_ready(view)) {
		return;
	}
	if (tiled) {
		wlr_xdg_toplevel_set_tiled(view->xdg_toplevel,
			WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
	} else {
		wlr_xdg_toplevel_set_tiled(view->xdg_toplevel, 0);
	}
}

void vidow_view_set_activated(struct vidow_view *view, bool activated) {
	if (!vidow_view_configure_ready(view)) {
		return;
	}
	if (view->kind == VIDOW_VIEW_X11) {
		wlr_xwayland_surface_activate(view->x11, activated);
		return;
	}
	wlr_xdg_toplevel_set_activated(view->xdg_toplevel, activated);
}

void vidow_view_set_fullscreen(struct vidow_view *view, bool fullscreen) {
	if (!vidow_view_configure_ready(view)) {
		return;
	}
	if (view->kind == VIDOW_VIEW_X11) {
		wlr_xwayland_surface_set_fullscreen(view->x11, fullscreen);
		return;
	}
	wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, fullscreen);
}

void vidow_view_close(struct vidow_view *view) {
	if (view->kind == VIDOW_VIEW_X11) {
		wlr_xwayland_surface_close(view->x11);
		return;
	}
	wlr_xdg_toplevel_send_close(view->xdg_toplevel);
}

struct vidow_view *vidow_view_from_xwayland_surface(struct vidow_server *server,
		struct wlr_xwayland_surface *xsurface) {
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->kind == VIDOW_VIEW_X11 && view->x11 == xsurface) {
			return view;
		}
	}
	return NULL;
}

/* 场景单位 -> surface 局部坐标的倍数。只有 X11 不是 1：它的缓冲区被我们按
 * xwayland_scale 倍声明过，场景里占的位置比客户端自己的坐标空间小。 */
double vidow_view_hit_scale(const struct vidow_view *view) {
	if (view->kind != VIDOW_VIEW_X11 || view->scene_surface == NULL ||
			view->x11 == NULL || view->x11->surface == NULL) {
		return 1.0;
	}
	struct wlr_scene_buffer *node = view->scene_surface->buffer;
	if (node == NULL || node->dst_width <= 0) {
		return 1.0;
	}
	return (double)view->x11->surface->current.width / (double)node->dst_width;
}

void vidow_view_x11_scale(struct vidow_view *view) {
	if (view->kind != VIDOW_VIEW_X11 || view->x11 == NULL ||
			view->scene_surface == NULL || view->x11->surface == NULL) {
		return;
	}
	/* 弹出物的输出由它的绝对坐标决定（菜单能开在另一块屏上），日志里那句
	 * 「输出 Nx」得按它真正落的那块屏说。 */
	if (view->popup) {
		x11_track_output(view);
	}
	struct wlr_surface *surface = view->x11->surface;
	int bw = surface->current.buffer_width;
	int bh = surface->current.buffer_height;
	if (bw <= 0 || bh <= 0) {
		return; /* 还没提交过缓冲区 */
	}
	int scale = vidow_view_x11_scale_int(view);
	int dst_w = (bw + scale / 2) / scale;
	int dst_h = (bh + scale / 2) / scale;
	if (dst_w < 1) {
		dst_w = 1;
	}
	if (dst_h < 1) {
		dst_h = 1;
	}
	struct wlr_scene_buffer *node = view->scene_surface->buffer;
	bool changed = node->dst_width != dst_w || node->dst_height != dst_h;
	wlr_scene_buffer_set_dest_size(node, dst_w, dst_h);
	/* 逐窗口规则把桥改小（x11_scale = 1）的时候，我们画得比「表面自己声明的
	 * 逻辑尺寸」大 —— 而场景的命中判定会问表面自己的输入区域（wlr_scene_surface
	 * 的 point_accepts_input 按 surface->current.width 卡边界），于是右边和下边
	 * 那一大片「看得见、点不着」。尺寸对不上的时候就别问它了：命中区域交给
	 * 节点自己的盒子（= 我们画出来的那块）。
	 *
	 * 代价是这类窗口不再遵守 X 的 shape（输入区域）—— 只有被规则改过缩放的
	 * 窗口走这条路，默认情况下尺寸一致，输入区域照旧生效。 */
	if (dst_w != surface->current.width || dst_h != surface->current.height) {
		node->point_accepts_input = NULL;
	}
	/* 每帧都会走到这里（每个 commit）：只在真的变了的时候说话。 */
	if (changed || view->logged_scale != scale) {
		view->logged_scale = scale;
		view->logged_buf_w = bw;
		/* 「表面 N 倍」= Xwayland 在 wl_surface 上声明的 buffer scale
		 * （vidow 写了 _XWAYLAND_GLOBAL_OUTPUT_SCALE 之后它就是那个全局缩放）。
		 * 它决定了 surface 自己的逻辑坐标系，输入坐标换算按它走 —— 和 vidow
		 * 这边「按 N 倍声明」是两回事，排查点击 / 尺寸问题时两个都要看。 */
		wlr_log(WLR_INFO,
			"x11: %s 缓冲 %dx%d 按 %d 倍声明 -> 场景 %dx%d（表面 %d 倍，"
			"输出 %.2fx，落到设备像素 %dx%d）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			bw, bh, scale, dst_w, dst_h,
			surface->current.scale > 0 ? surface->current.scale : 1,
			(double)(view->output != NULL ? view->output->scale : 1.0),
			(int)lround(dst_w * (view->output != NULL ?
				(double)view->output->scale : 1.0)),
			(int)lround(dst_h * (view->output != NULL ?
				(double)view->output->scale : 1.0)));
	}
}

/* 记下内容在场景里的 1:1 尺寸：overview 要拿它乘缩放。
 * 只在没进 overview 的时候记 —— overview 里是我们自己把场景尺寸改小的，
 * 再记一次就等于把缩放叠进去（越缩越小）。 */
static void view_dst_capture(struct vidow_view *view) {
	if (view->server->overview.t > 0.0 || view->scene_surface == NULL) {
		return;
	}
	struct wlr_scene_buffer *src = view->scene_surface->buffer;
	if (src == NULL) {
		return;
	}
	int w = src->dst_width > 0 ? src->dst_width :
		(src->buffer != NULL ? (int)src->buffer->width : 0);
	int h = src->dst_height > 0 ? src->dst_height :
		(src->buffer != NULL ? (int)src->buffer->height : 0);
	if (w > 0 && h > 0) {
		view->base_dst_w = w;
		view->base_dst_h = h;
	}
}

struct vidow_view *vidow_view_at(struct vidow_server *server,
		double lx, double ly, struct wlr_surface **surface,
		double *sx, double *sy) {
	struct wlr_scene_node *node =
		wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}

	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (scene_surface == NULL) {
		return NULL;
	}
	*surface = scene_surface->surface;

	/* Walk up the scene-graph until we hit a node tagged with our view. */
	for (struct wlr_scene_node *n = &scene_buffer->node; n != NULL;
			n = n->parent != NULL ? &n->parent->node : NULL) {
		struct vidow_view_tag *tag = n->data;
		if (tag != NULL && tag->magic == VIDOW_VIEW_TAG_MAGIC) {
			/* X11 的缓冲区被我们按 xwayland_scale 倍声明过：场景坐标比
			 * 客户端自己的坐标空间小，命中坐标要乘回去才是 surface 局部
			 * 坐标（否则只有左上角 1/scale 那块能点中）。 */
			double hit = vidow_view_hit_scale(tag->view);
			if (hit != 1.0) {
				*sx *= hit;
				*sy *= hit;
			}
			return tag->view;
		}
	}
	return NULL;
}

/* ------------------------------------------------------------- columns */

static struct vidow_view *column_first_view(struct vidow_column *col) {
	struct vidow_view *view;
	if (wl_list_empty(&col->views)) {
		return NULL;
	}
	view = wl_container_of(col->views.next, view, column_link);
	return view;
}

static bool column_has_views(struct vidow_column *col) {
	return !wl_list_empty(&col->views);
}

/* 这块屏上现在该摆哪些列：有特殊工作区开着就摆它的（普通工作区让位），否则摆
 * 活动的那块普通工作区。空指针（还没建过工作区）返回空链表，arrange 就当没列。 */
static struct wl_list *output_shown_columns(struct vidow_server *server,
		struct wlr_output *output) {
	/* 每块屏摆自己的那块 —— 不能问「现在显示着的工作区」（那是聚焦屏的），
	 * 否则另一块屏上的窗口在 arrange 里被跳过、画不出来。 */
	struct vidow_workspace *ws = vidow_workspace_shown_for(server, output);
	if (ws == NULL || ws->output != output) {
		static struct wl_list empty;
		wl_list_init(&empty);
		return &empty;
	}
	return &ws->columns;
}

static void view_ensure_column(struct vidow_server *server,
		struct vidow_view *view) {
	if (view->column != NULL) {
		return;
	}

	struct vidow_column *col = calloc(1, sizeof(*col));
	if (col == NULL) {
		return;
	}
	col->server = server;
	col->id = ++server->next_column_id;
	col->output = view->output;
	col->home = view->home;
	col->width_fraction = DEFAULT_COLUMN_WIDTH;
	wl_list_init(&col->views);

	/* 这条列落在哪块工作区：窗口自己记着的那块（新窗口在 map 时就定好了，
	 * 见 view_map），没有就落到这块屏的活动工作区上。 */
	struct vidow_workspace *ws = view->ws;
	if (ws == NULL) {
		ws = vidow_workspace_active(server);
		view->ws = ws;
	}
	if (ws != NULL) {
		col->ws = ws;
		wl_list_init(&col->link);
		/* New windows open in a column to the right of the focused one. */
		struct vidow_column *focused_col =
			server->focused != NULL ? server->focused->column : NULL;
		if (focused_col != NULL && focused_col->ws == ws) {
			wl_list_insert(&focused_col->link, &col->link);
		} else {
			wl_list_insert(ws->columns.prev, &col->link);
		}
	} else {
		wl_list_init(&col->link); /* 没有工作区可归：挂在自己头上，谁都摆不到 */
	}

	view->column = col;
	wl_list_insert(col->views.prev, &view->column_link);
	/* 落在栈底那块空的上了：它就是「新工作区」，底下再补一块（niri 的「窗口开
	 * 在最后一块空工作区上时，下面立刻出现一块新的」）。要在列真的挂进工作区
	 * 之后再判 —— 不然这时候它还是「空的」。 */
	if (!ws->special && ws->output != NULL) {
		vidow_workspace_ensure_bottom(server, ws->output);
	}
}

static void view_leave_column(struct vidow_view *view) {
	if (view->column == NULL) {
		return;
	}
	struct vidow_column *col = view->column;
	wl_list_remove(&view->column_link);
	view->column = NULL;
	if (!column_has_views(col)) {
		if (col->ws != NULL) {
			wl_list_remove(&col->link);
		}
		free(col);
	}
}

/* ------------------------------------------------------------- layout */

/* wlr_scene_xdg_surface_create() nests the scene surface inside a tree of its
 * own, so dig for it instead of assuming a flat layout. */
static struct wlr_scene_surface *find_scene_surface(struct wlr_scene_node *node,
		struct wlr_surface *want) {
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(
			wlr_scene_buffer_from_node(node));
		return ss != NULL && ss->surface == want ? ss : NULL;
	}
	if (node->type != WLR_SCENE_NODE_TREE) {
		return NULL;
	}
	struct wlr_scene_tree *tree = wl_container_of(node, tree, node);
	struct wlr_scene_node *child;
	wl_list_for_each(child, &tree->children, link) {
		struct wlr_scene_surface *ss = find_scene_surface(child, want);
		if (ss != NULL) {
			return ss;
		}
	}
	return NULL;
}

/* ------------------------------------------------------------ animation */

/* 窗口所在输出的缩放。装饰着色器的离屏缓冲按它分配 —— 「同一块窗口在不同 DPI
 * 的屏上都清晰」的另一半：客户端那边靠 buffer_scale 声明自己画了多大，这边靠
 * 渲染倍率保证装饰和着色器也是按设备像素画的。 */
static double view_scale(const struct vidow_view *view) {
	if (view->output == NULL) {
		return 1.0;
	}
	double scale = view->output->scale;
	return scale > 0.0 ? scale : 1.0;
}

/* Redraw the view at its animated position/opacity. The tiled geometry itself
 * (view->geom) is never animated: clients are only ever configured with the
 * final size, so a tween can never race a resize. */
/* ------------------------------------------------ 视口外强制渲染（补帧回调） */


static bool view_alive(struct vidow_server *server, struct vidow_view *want);

/* 窗口的整棵表面树（含子表面 / 弹窗）都补一份帧回调。 */
struct pump_ctx {
	struct timespec *now;
	int surfaces;
};

static void pump_surface(struct wlr_surface *surface, int sx, int sy, void *data) {
	(void)sx;
	(void)sy;
	struct pump_ctx *ctx = data;
	wlr_surface_send_frame_done(surface, ctx->now);
	ctx->surfaces++;
}

/* 这个窗口的矩形和它自己那块屏有交集吗（都是布局坐标）。
 *
 * 为什么要问：wlr_scene 是一张**全局坐标**的场景图，每块屏的场景输出会画「落在
 * 自己那块坐标里的所有节点」。列往右滚出屏宽之后，它的坐标正好落进右边那块屏的
 * 框里 —— 于是就画到隔壁显示器上了（niri 不会：它每块屏各渲染自己那棵，屏外的
 * 自然被 framebuffer 裁掉）。所以这里按框判一次，越界的干脆不画。
 *
 * 还没摆放（几何是空的）和不知道归哪块屏的都当「画得到」：宁可多画一帧，也别把
 * 一个刚 map 的窗口藏没了。 */
bool vidow_view_in_output_box(struct vidow_server *server,
		const struct vidow_view *view) {
	if (view->output == NULL || view->geom.width <= 0 ||
			view->geom.height <= 0) {
		return true;
	}
	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, view->output, &box);
	if (box.width < 1 || box.height < 1) {
		return true;
	}
	return view->geom.x < box.x + box.width &&
		view->geom.x + view->geom.width > box.x &&
		view->geom.y < box.y + box.height &&
		view->geom.y + view->geom.height > box.y;
}

/* 这个窗口现在画得到吗：所在工作区显示着 **而且** 它的矩形和这块屏有交集。
 * 画得到就不该补帧（wlr_scene 已经给过回调了，再补一次会让客户端多画一遍）。 */
static bool view_on_screen(struct vidow_server *server,
		struct vidow_output *output, struct vidow_view *view) {
	if (!vidow_workspace_view_visible(server, view)) {
		return false;
	}
	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
	if (box.width < 1) {
		return false;
	}
	/* overview 里所有窗口都被缩到画面里了，这时它们都画得到。 */
	if (vidow_overview_active(server)) {
		return true;
	}
	return view->geom.x < box.x + box.width &&
		view->geom.x + view->geom.width > box.x &&
		view->geom.y < box.y + box.height &&
		view->geom.y + view->geom.height > box.y;
}

/* 每帧调一次：把「规则要求保活」或者「正被人拿着（录屏 / 预览）」但**没画到**的
 * 窗口补上帧回调，并且让输出继续出帧 —— 不然补帧链自己就断了（没东西要画的时候
 * wlroots 不会给帧事件）。
 *
 * 为什么需要这个：Wayland 的帧回调是「我要用你的画面了」的许可。窗口滚到视口外 /
 * 所在工作区被藏起来时，场景里什么都没画到它，客户端拿不到许可 —— 视频播放器、
 * 游戏、任何按帧节流的程序就此停帧。切回来是一张冻住的画面，录屏录到的也是它。 */
void vidow_view_render_pump(struct vidow_server *server,
		struct vidow_output *output, uint32_t msec) {
	struct timespec now = {0};
	now.tv_sec = msec / 1000;
	now.tv_nsec = (long)(msec % 1000) * 1000000L;

	bool keep_going = false;
	int64_t next_due = 0; /* 被节流挡下的那些窗口里，最早该醒的时刻 */
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		bool want = view->mapped && !view->popup && view->output ==
			output->wlr_output &&
			(view->force_render || vidow_view_capture_held(view));
		if (!want) {
			if (view->render_pumping) {
				view->render_pumping = false;
				wlr_log(WLR_DEBUG, "强制渲染: %s 停（不再需要）",
					vidow_view_title(view) != NULL ?
						vidow_view_title(view) : "(无题)");
			}
			continue;
		}
		if (view_on_screen(server, output, view)) {
			if (view->render_pumping) {
				view->render_pumping = false;
				wlr_log(WLR_DEBUG, "强制渲染: %s 停（它已经画在画面上）",
					vidow_view_title(view) != NULL ?
						vidow_view_title(view) : "(无题)");
			}
			continue;
		}

		/* 节流（逐窗口，规则 / 全局配置说了算；0 = 不限）。还没到点的记下最早
		 * 该醒的时刻，收尾时定个闹钟 —— 不能什么都不做：客户端提交一次之后就
		 * 没人再推了，30Hz / 15Hz 的补帧会断在半路（踩过：全局 30 补出来 0 帧）。 */
		if (view->pump_interval_ms > 0 &&
				msec < (uint32_t)view->pump_next_ms) {
			if (next_due == 0 || view->pump_next_ms < next_due) {
				next_due = view->pump_next_ms;
			}
			continue;
		}
		view->pump_next_ms = view->pump_interval_ms > 0 ?
			(int64_t)msec + view->pump_interval_ms : 0;

		struct wlr_surface *surface = vidow_view_surface(view);
		if (surface != NULL) {
			struct pump_ctx ctx = { .now = &now, .surfaces = 0 };
			wlr_surface_for_each_surface(surface, pump_surface, &ctx);
			if (ctx.surfaces > 0) {
				keep_going = true;
			}
		}
		if (!view->render_pumping) {
			view->render_pumping = true;
			char rate[32];
			if (view->pump_interval_ms > 0) {
				snprintf(rate, sizeof(rate), "节流 %dHz",
					1000 / view->pump_interval_ms);
			} else {
				snprintf(rate, sizeof(rate), "不限速");
			}
			wlr_log(WLR_INFO, "强制渲染: %s 在视口外/藏起来，照样补帧回调"
				"（%s，%s）", vidow_view_title(view) != NULL ?
					vidow_view_title(view) : "(无题)",
				vidow_view_capture_held(view) ? "有人正在录屏 / 预览它" :
					"窗口规则要求", rate);
		}
	}

	/* 补帧得有人推：不主动排下一帧的话，画面没变化时 wlroots 就不给帧事件了。 */
	if (keep_going) {
		wlr_output_schedule_frame(output->wlr_output);
	}

	/* 被节流挡下的：定个到点闹钟（比当前定的更早就改早一点）。 */
	if (next_due > 0 && output->pump_timer != NULL) {
		if (output->pump_at_ms == 0 || next_due < output->pump_at_ms) {
			int64_t delay = next_due - (int64_t)msec;
			if (delay < 1) {
				delay = 1;
			}
			wl_event_source_timer_update(output->pump_timer, (int)delay);
			output->pump_at_ms = next_due;
		}
	} else if (output->pump_at_ms != 0 && output->pump_timer != NULL) {
		wl_event_source_timer_update(output->pump_timer, 0); /* 没人被挡下：撤闹钟 */
		output->pump_at_ms = 0;
	}
}

/* 有人正拿着这个窗口的画面吗（有一条持有记录就算）。 */
bool vidow_view_capture_held(const struct vidow_view *view) {
	return !wl_list_empty(&view->capture_sources);
}

static void capture_hold_drop(struct vidow_capture_hold *hold,
		const char *why) {
	wl_list_remove(&hold->link);
	wl_list_remove(&hold->destroy.link);
	if (hold->client != NULL) {
		wl_list_remove(&hold->client_destroy.link);
	}
	if (hold->view != NULL && view_alive(hold->server, hold->view)) {
		wlr_log(WLR_INFO, "portal: %s，%s 不再强制渲染", why,
			vidow_view_title(hold->view) != NULL ?
				vidow_view_title(hold->view) : "(无题)");
	}
	free(hold);
}

static void capture_source_gone(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_capture_hold *hold = wl_container_of(listener, hold, destroy);
	capture_hold_drop(hold, "窗口截图源没了");
}

/* 录屏 / 预览的客户端断开：它拿着的那份「保活」也要放掉，不然一个崩掉的录屏程序
 * 会让窗口一直按 60fps 补帧。 */
static void capture_client_gone(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_capture_hold *hold =
		wl_container_of(listener, hold, client_destroy);
	capture_hold_drop(hold, "录屏 / 预览的客户端断了");
}

void vidow_view_capture_hold(struct vidow_view *view,
		struct vidow_server *server,
		struct wlr_ext_image_capture_source_v1 *source,
		struct wl_client *client) {
	if (view == NULL || source == NULL) {
		return;
	}
	struct vidow_capture_hold *hold = calloc(1, sizeof(*hold));
	if (hold == NULL) {
		return;
	}
	hold->view = view;
	hold->server = server;
	hold->source = source;
	hold->client = client;
	hold->destroy.notify = capture_source_gone;
	wl_signal_add(&source->events.destroy, &hold->destroy);
	if (client != NULL) {
		hold->client_destroy.notify = capture_client_gone;
		wl_client_add_destroy_listener(client, &hold->client_destroy);
	}
	wl_list_insert(view->capture_sources.prev, &hold->link);
}

/* 窗口要没了：把持有记录清干净（源自己会跟着场景节点销毁，这里只管我们这份）。 */
static void view_capture_holds_clear(struct vidow_view *view) {
	struct vidow_capture_hold *hold, *tmp;
	wl_list_for_each_safe(hold, tmp, &view->capture_sources, link) {
		wl_list_remove(&hold->link);
		wl_list_remove(&hold->destroy.link);
		if (hold->client != NULL) {
			wl_list_remove(&hold->client_destroy.link);
		}
		free(hold);
	}
}

void vidow_view_apply_anim(struct vidow_view *view) {
	/* 场景节点可能已经没了：X11 销毁窗口时先摘掉表面（scene_tree 归 NULL）再撤
	 * 动画，而撤销动画会回调 view_open_done() 回到这里 —— 以前就死在这行上。 */
	if (view->scene_tree == NULL) {
		return;
	}
	int wx = view->geom.x + (int)lround(view->anim_off_x);
	int wy = view->geom.y + (int)lround(view->anim_off_y);

	/* overview 里：位置走「世界 -> 屏幕」变换，内容尺寸乘上缩放；
	 * 不在那块屏上的窗口淡出（overview 只看一块屏上的列带）。 */
	struct vidow_server *server = view->server;
	double ov = vidow_overview_scale(server);
	double alpha = view->anim_alpha;
	if (server->overview.t > 0.0 && server->overview.output != NULL &&
			view->output != server->overview.output) {
		alpha *= 1.0 - server->overview.t;
	}
	double sx_mapped = 0, sy_mapped = 0;
	vidow_overview_map(server, wx, wy, &sx_mapped, &sy_mapped);
	int x = (int)lround(sx_mapped);
	int y = (int)lround(sy_mapped);

	wlr_scene_node_set_position(&view->scene_tree->node, x, y);
	vidow_fx_set_alpha(&view->scene_tree->node, alpha);

	if (!view->fx.active || view->scene_surface == NULL) {
		return;
	}
	struct wlr_scene_buffer *src = view->scene_surface->buffer;
	if (src == NULL) {
		return;
	}
	if (ov != 1.0 && view->base_dst_w > 0 && view->base_dst_h > 0) {
		int dw = (int)lround(view->base_dst_w * ov);
		int dh = (int)lround(view->base_dst_h * ov);
		wlr_scene_buffer_set_dest_size(src, dw > 0 ? dw : 1, dh > 0 ? dh : 1);
	}
	int vx = 0, vy = 0, sx = 0, sy = 0;
	wlr_scene_node_coords(&view->scene_tree->node, &vx, &vy);
	wlr_scene_node_coords(&src->node, &sx, &sy);
	view->fx_dx = sx - vx;
	view->fx_dy = sy - vy;

	struct vidow_fx_geo geo = vidow_view_fx_geo(view);

	/* ext-background-effect：客户端要的那块才模糊。区域是 surface 局部坐标，
	 * 着色器里比的是窗口局部坐标 —— 两者差一个 CSD 留白偏移（没留白就是 0）。 */
	struct wlr_box region;
	if (vidow_bgeffect_blur_box(view->server,
			vidow_view_surface(view), &region)) {
		view->fx.blur_region_on = true;
		view->fx.blur_region[0] = (float)(region.x - geo.off_x);
		view->fx.blur_region[1] = (float)(region.y - geo.off_y);
		view->fx.blur_region[2] = (float)region.width;
		view->fx.blur_region[3] = (float)region.height;
	} else {
		view->fx.blur_region_on = false;
	}

	/* The decoration shader applies the fade itself, so the node stays opaque
	 * and the real surface is only kept alive for hit-testing. */
	/* 装饰节点摆在「可见窗口」左上角（视图树的原点就是它，见 vidow_fx_geo），
	 * 不在源缓冲的原点上：源缓冲多出来的那圈 CSD 留白落在装饰矩形外面。 */
	view->fx.anim_scale = view->anim_scale;
	bool fx_drawn = vidow_fx_frame(view->server, &view->fx, src, &geo, 0, 0,
		x, y, alpha, view->server->focused == view,
		view_scale(view) * ov, ov);
	/* 装饰图真的画上屏了才把真实表面藏起来（只留给命中测试）。没画出来
	 * 还藏就是窗口整个消失 —— 关掉 decor/shader 时踩过，屏幕一片黑。 */
	wlr_scene_buffer_set_opacity(src, fx_drawn ? 0.0f : 1.0f);
}

/* 从 wlr_surface 找回它的窗口：文本输入和 ext-background-effect 都要用。
 * 子表面按根表面匹配，所以焦点在子表面上时也能找到。 */
struct vidow_view *vidow_view_from_surface(struct vidow_server *server,
		struct wlr_surface *surface) {
	if (surface == NULL) {
		return NULL;
	}
	struct wlr_surface *root = wlr_surface_get_root_surface(surface);
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (vidow_view_surface(view) == root) {
			return view;
		}
	}
	return NULL;
}

static void view_move_update(void *data, double t, double linear) {
	(void)linear;
	struct vidow_view *view = data;
	view->anim_off_x = view->anim_from_x * (1.0 - t);
	view->anim_off_y = view->anim_from_y * (1.0 - t);
	vidow_backdrop_damage(view->server, false);
	vidow_view_apply_anim(view);
}

static void view_move_done(void *data, bool cancelled) {
	struct vidow_view *view = data;
	view->anim_move = NULL;
	if (!cancelled) {
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
	}
	vidow_view_apply_anim(view);
}

static void view_open_update(void *data, double t, double linear) {
	(void)linear;
	struct vidow_view *view = data;
	struct vidow_config *cfg = &view->server->config;
	/* 回弹的曲线会过冲（t 稍微越过 1）：位移和缩放跟着过冲才是回弹，透明度
	 * 不行 —— 负的 alpha 在场景节点上没定义。
	 *
	 * 入场这里透明度**照旧跟曲线走**：过冲只会让它提前到 1（早点变成不透明），
	 * 没有「提前结束」的问题。关闭那边正好反过来 —— 斜坡是 1 → close_alpha_to，
	 * 过冲等于提前结束，所以那边必须用线性进度（见 fx.c 的 closing_update）。 */
	view->anim_alpha = cfg->open_alpha_from +
		(1.0 - cfg->open_alpha_from) * t;
	if (view->anim_alpha < 0.0) {
		view->anim_alpha = 0.0;
	} else if (view->anim_alpha > 1.0) {
		view->anim_alpha = 1.0;
	}
	view->anim_off_y = cfg->open_slide_px * (1.0 - t);
	view->anim_scale = cfg->open_scale_from +
		(1.0 - cfg->open_scale_from) * t;
	view->fx.progress = t;
	vidow_backdrop_damage(view->server, false);
	vidow_view_apply_anim(view);
}

static void view_open_done(void *data, bool cancelled) {
	(void)cancelled;
	struct vidow_view *view = data;
	struct vidow_server *server = view->server;
	struct vidow_config *cfg = &server->config;

	view->anim_open = NULL;
	view->anim_alpha = 1.0;
	view->anim_off_y = 0.0;
	view->anim_scale = 1.0;
	wlr_log(WLR_DEBUG, "open done %s",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?");

	/* Drop the opening shader but keep the window decorated. */
	if (cfg->shader_idle[0] != '\0' &&
			vidow_fx_set_shader(server, &view->fx, cfg->shader_idle,
				VIDOW_SHADER_IDLE)) {
		view->fx.progress = 1.0;
	} else {
		view->fx.shader = NULL;
		view->fx.phase = VIDOW_SHADER_NONE;
	}
	if (!view->fx.active) {
		vidow_fx_end(server, &view->fx);
	}
	vidow_view_apply_anim(view);
}

void vidow_view_open_anim(struct vidow_view *view) {
	struct vidow_config *cfg = &view->server->config;

	/* 弹出物（X11 菜单 / 提示 / 下拉框）和 Wayland 的 xdg_popup 一样：不装饰、
	 * 不跑 shader、也不做入场动画。它是跟着操作「啪」地出现的一块小图，滑动
	 * 或淡入都显得慢半拍；而且它常在自己的位置上重开，动画会看着像窗口在飘。
	 * 这里只把场景节点摆到最终位置。 */
	if (view->popup) {
		vidow_anim_cancel(view->anim_open);
		view->anim_open = NULL;
		view->anim_alpha = 1.0;
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		view->anim_scale = 1.0;
		vidow_view_apply_anim(view);
		return;
	}

	if (view->scene_surface != NULL && view->scene_surface->buffer != NULL) {
		int vx = 0, vy = 0, sx = 0, sy = 0;
		wlr_scene_node_coords(&view->scene_tree->node, &vx, &vy);
		wlr_scene_node_coords(&view->scene_surface->buffer->node, &sx, &sy);
		view->fx_dx = sx - vx;
		view->fx_dy = sy - vy;
	}

	view->anim_alpha = cfg->open_alpha_from;
	view->anim_scale = cfg->open_scale_from;
	/* 入场动画接管位置：把可能刚起的位移补间撤掉。新窗口 map 的同一帧里
	 * arrange 已经先跑过一遍（带着列偏移的变化），那会儿还没有入场动画，
	 * 位移补间就起来了 —— 两个动画抢同一组 anim_off、每帧互相覆盖，看着就是
	 * 「闪一下再弹一下」（上滑被抹成 0，还横着挪几像素）。 */
	vidow_anim_cancel(view->anim_move);
	view->anim_move = NULL;
	view->anim_off_x = 0.0;
	view->anim_off_y = cfg->open_slide_px;

	vidow_fx_begin(view->server, &view->fx, view->scene_tree, cfg->shader_open);

	vidow_anim_cancel(view->anim_open);
	view->anim_open = vidow_anim_start(view->server, "open",
		cfg->window_open_ms, cfg->open_easing, view_open_update,
		view_open_done, view);
	if (view->anim_open == NULL) {
		view_open_done(view, true);
	}
}

/* The surface is gone by the time we get here, so the close animation runs on
 * a snapshot of the last committed buffer. */
static void view_snapshot_close(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	/* 弹出物不做关闭动画：菜单收起来就是收起来了（它常常在关掉的同一帧里被
	 * 重开或换成另一个菜单，淡出的残影会串到下一个菜单上）。 */
	if (view->popup || view->last_buffer == NULL ||
			server->config.window_close_ms <= 0) {
		/* 「怎么没有关闭动画」是收到过的反馈，而这里有三条不同的路都能让它
		 * 不出现（弹出物 / 没有最后一帧快照 / 时长是 0）。日志里说清楚是哪条，
		 * 不然只能靠猜（`vidowmsg windows` 里能查回 popup / float_mode）。 */
		wlr_log(WLR_DEBUG, "关闭动画跳过 %s：%s",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			view->popup ? "弹出物（菜单 / 提示 / 下拉框）不做关闭动画" :
			view->last_buffer == NULL ? "手里没有最后一帧快照" :
			"window_close_ms = 0（配置里关了）");
		return;
	}
	/* 快照是整块源缓冲（表面），可见窗口在里面偏着 —— 把偏移一起交给关闭动画，
	 * 否则 CSD 窗口淡出的时候会整体斜出去那圈留白。 */
	struct vidow_fx_geo geo = vidow_view_fx_geo(view);
	if (geo.w <= 0 || geo.h <= 0 || geo.src_w <= 0 || geo.src_h <= 0) {
		/* 已经拆干净了（X11 的 surface 没了 / 还没 commit）：整块就是窗口。 */
		geo = (struct vidow_fx_geo){0};
		if (view->scene_surface != NULL && view->scene_surface->buffer != NULL) {
			struct wlr_scene_buffer *sb = view->scene_surface->buffer;
			if (sb->dst_width > 0) {
				geo.src_w = sb->dst_width;
			}
			if (sb->dst_height > 0) {
				geo.src_h = sb->dst_height;
			}
		}
		if (geo.src_w <= 0) {
			geo.src_w = view->geom.width;
		}
		if (geo.src_h <= 0) {
			geo.src_h = view->geom.height;
		}
		geo.w = geo.src_w;
		geo.h = geo.src_h;
	}
	int w = geo.src_w, h = geo.src_h;
	vidow_closing_start(server, server->layer_toplevels, view->last_buffer,
		&geo, view->geom.x + (int)lround(view->anim_off_x),
		view->geom.y + (int)lround(view->anim_off_y), w, h,
		server->config.window_close_ms, server->config.close_slide_px,
		true, server->config.shader_close, view_scale(view),
		server->config.close_easing, server->config.close_scale_to);
}

void vidow_place_view(struct vidow_view *view, int x, int y, int w, int h) {
	if (w < 1) {
		w = 1;
	}
	if (h < 1) {
		h = 1;
	}

	struct vidow_config *cfg = &view->server->config;
	bool moved = view->geom.x != x || view->geom.y != y;
	bool resized = view->geom.width != w || view->geom.height != h;

	/* 「现在画在哪」必须在覆盖 geom 之前取：覆盖之后再拿 geom 算补间起点，
	 * 得到的是新位置减新位置 = 0，动画照跑 190ms / 30 帧但一格都不动。 */
	int drawn_x = view->geom.x + (int)lround(view->anim_off_x);
	int drawn_y = view->geom.y + (int)lround(view->anim_off_y);

	view->geom = (struct wlr_box){ .x = x, .y = y, .width = w, .height = h };
	wlr_log(WLR_DEBUG, "place %s -> %d,%d %dx%d",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		x, y, w, h);
	/* 布局把它挪/缩了：订阅者（状态栏、脚本）要知道新几何。第一次摆放
	 * （placed == false）也报 —— 那是「它现在在哪儿」的第一份事实。 */
	if (view->mapped && (moved || resized || !view->placed)) {
		vidow_ipc_emit_window(view->server, "window-moved", view);
	}
	/* set_size 自己会判断客户端准备好了没有；平铺提示 X11 不吃。 */
	vidow_view_set_size(view, w, h);
	vidow_view_set_tiled(view, true);

	/* 正被鼠标拖着：位置归 drag.c 管（apply_anim 用 anim_off 把它摆在光标下面），
	 * 这里别起补间、也别把偏移清零，不然拖到一半会被 arrange 拽回去。 */
	if (view->dragging) {
		vidow_anim_cancel(view->anim_move);
		view->anim_move = NULL;
		vidow_view_apply_anim(view);
		return;
	}

	if (!view->placed) {
		view->placed = true;
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		vidow_view_apply_anim(view);
		return;
	}

	/* 入场动画正跑着：位置归它管。新窗口 map 的同一帧里 arrange 会跑好几遍
	 * （建列、抢焦点、视口滚动各一次），第二遍起就满足 moved 了 —— 于是位移
	 * 补间和入场动画抢同一组 anim_off，每帧互相覆盖：上滑被抹成 0、还横着挪
	 * 几像素，看着就是「闪一下再弹一下」。这里只更新 geom，offset 一个字都不碰
	 * （淡入那点时间差里看不出这几像素的位移）。 */
	if (view->anim_open != NULL) {
		vidow_anim_cancel(view->anim_move);
		view->anim_move = NULL;
		vidow_view_apply_anim(view);
		return;
	}

	/* 弹出物不做移动补间：它是「跟着手指出现」的一块图，滑动会显慢半拍
	 * （Wayland 的 xdg_popup 同样不走任何动画）。 */
	if (view->popup || !moved || cfg->window_move_ms <= 0) {
		vidow_anim_cancel(view->anim_move);
		view->anim_move = NULL;
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		vidow_view_apply_anim(view);
		return;
	}

	/* Tween from wherever the window is drawn right now, so a new layout
	 * change mid-flight does not snap it back. */
	view->anim_from_x = (double)(drawn_x - x);
	view->anim_from_y = (double)(drawn_y - y);
	vidow_anim_cancel(view->anim_move);
	view->anim_move = vidow_anim_start(view->server, "move",
		cfg->window_move_ms, cfg->easing, view_move_update, view_move_done, view);
	if (view->anim_move != NULL) {
		wlr_log(WLR_DEBUG, "move tween %s: %d,%d -> %d,%d（%dms）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
			drawn_x, drawn_y, x, y, cfg->window_move_ms);
	} else {
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		vidow_view_apply_anim(view);
	}
}

struct wlr_box vidow_work_area(struct vidow_server *server,
		struct wlr_output *output) {
	struct wlr_box area = {0};
	struct vidow_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output == output) {
			area = o->usable_area;
			break;
		}
	}
	if (area.width <= 0 || area.height <= 0) {
		wlr_output_layout_get_box(server->output_layout, output, &area);
	}

	/* 缩进可用区：窗口不贴屏幕边、也不贴状态栏。四边可以分别设
	 * （gaps_top/bottom/left/right 或一张表 gaps = { top = … }），
	 * 没设就一起用统一值 gaps。 */
	const struct vidow_config *cfg = &server->config;
	int top = vidow_gap_side(cfg, cfg->gaps_top);
	int bottom = vidow_gap_side(cfg, cfg->gaps_bottom);
	int left = vidow_gap_side(cfg, cfg->gaps_left);
	int right = vidow_gap_side(cfg, cfg->gaps_right);
	area.x += left;
	area.y += top;
	area.width -= left + right;
	area.height -= top + bottom;
	/* 四边加起来超过屏幕就退化成 1px：宁可挤扁，也别给布局一个负的框。 */
	if (area.width < 1) {
		area.width = 1;
	}
	if (area.height < 1) {
		area.height = 1;
	}
	return area;
}

static void output_arrange(struct vidow_server *server,
		struct vidow_output *output) {
	struct wlr_box work = vidow_work_area(server, output->wlr_output);
	if (work.width < 1 || work.height < 1) {
		return;
	}

	/* 窗口之间（列与列、同列上下）的间距 = gaps_inner，没设就跟 gaps 一样。 */
	int inner = vidow_gap_inner(&server->config);

	/* Pass 1: column widths and offsets, measured from the work area. */
	struct wl_list *shown = output_shown_columns(server, output->wlr_output);
	int cursor = 0;
	struct vidow_column *col;
	wl_list_for_each(col, shown, link) {
		int width = (int)(work.width * col->width_fraction);
		if (width < 1) {
			width = 1;
		}
		col->width = width;
		col->offset = cursor;
		cursor += width + inner;
	}
	int total_width = cursor > 0 ? cursor - inner : 0;

	/* Pass 2: scroll the minimum amount that makes the focused column fully
	 * visible. Focus moves, the view follows. */
	double viewport = output->viewport_x;
	int max_viewport = total_width - work.width;
	if (max_viewport < 0) {
		max_viewport = 0;
	}
	struct vidow_view *focus = server->focused;
	if (focus != NULL && focus->float_mode == VIDOW_FLOAT_FOLLOW &&
			focus->parent != NULL) {
		/* scroll to where the dialog's parent lives */
		focus = focus->parent;
	}
	struct vidow_column *focused_col =
		focus != NULL ? focus->column : NULL;
	if (focused_col != NULL && focused_col->output == output->wlr_output) {
		int right = focused_col->offset + focused_col->width;
		if (right > viewport + work.width) {
			viewport = right - work.width;
		}
		if (focused_col->offset < viewport) {
			viewport = focused_col->offset;
		}
	}
	if (viewport < 0) {
		viewport = 0;
	}
	if (viewport > max_viewport) {
		viewport = max_viewport;
	}
	output->viewport_x = viewport;

	/* Pass 3: place the tiled windows. Floating views are placed afterwards
	 * so that a follow-float can read its parent's fresh tile geometry. */
	struct vidow_view *fullscreen = NULL;
	wl_list_for_each(col, shown, link) {
		int x = work.x + col->offset - (int)viewport;

		int n = 0;
		struct vidow_view *view;
		wl_list_for_each(view, &col->views, column_link) {
			if (view->fullscreen && view->mapped) {
				fullscreen = view;
				continue;
			}
			n++;
		}
		if (n == 0) {
			continue;
		}

		int y = work.y;
		wl_list_for_each(view, &col->views, column_link) {
			if (view->fullscreen && view->mapped) {
				continue;
			}
			int h = (work.height - (n - 1) * inner) / n;
			vidow_place_view(view, x, y, col->width, h);
			y += h + inner;
		}
	}

	vidow_float_place_all(server, output);

	if (fullscreen != NULL) {
		vidow_place_view(fullscreen, work.x, work.y, work.width, work.height);
		wlr_scene_node_raise_to_top(&fullscreen->scene_tree->node);
	}
}

void vidow_arrange(struct vidow_server *server) {
	struct vidow_output *output;
	/* Moving windows changes what is behind every other window. */
	vidow_backdrop_damage(server, false);
	wl_list_for_each(output, &server->outputs, link) {
		output_arrange(server, output);
	}
	/* 弹出物不进布局（它们的位置尺寸由客户端给死），所以放在所有输出之后单独
	 * 摆一遍 —— 也正因为不看输出，菜单能开在任意一块屏上。 */
	vidow_popup_place_all(server);

	/* 位置变了就要重判一次「谁该出现在场景里」：滚动把一列推出视口之后，它得
	 * 立刻从场景里摘掉，不然它的坐标落进隔壁屏的框里，就画到隔壁显示器上了
	 * （见 vidow_view_in_output_box）。 */
	vidow_workspace_apply_visibility(server);
}

/* ------------------------------------------------------------- 热插拔 */

/* 一块屏拔了 / 回来了：列、浮动窗口、layer surface 的「归属」记在
 * vidow_output_memory 里，这里只动「现在摆在哪块屏上」。详见 hotplug.h。 */

/* 这个窗口一定属于哪块屏（记忆）。窗口一建出来就记上，浮动窗口靠它回到原来的屏。 */
static struct vidow_output_memory *home_of(struct vidow_server *server,
		struct wlr_output *wlr_output) {
	if (wlr_output == NULL) {
		return NULL;
	}
	return vidow_output_memory_for(&server->output_memories, wlr_output->name);
}

/* 指针还活着吗 —— 记忆里存的是裸指针，可能早就被关掉了，用之前必须验一遍
 * （只比较，不解引用；只有真在 views 列表里才认）。 */
static bool view_alive(struct vidow_server *server, struct vidow_view *want) {
	if (want == NULL) {
		return false;
	}
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view == want) {
			return true;
		}
	}
	return false;
}

/* 工作区挂在哪个更新器上。配置可以给每块工作区指定「属于哪块屏」，工作区里
 * 的列就看这一条。 */
static bool ws_on_memory(struct vidow_workspace *ws,
		struct vidow_output_memory *memory) {
	if (ws->output == NULL || memory == NULL) {
		return false;
	}
	char key[VIDOW_OUTPUT_KEY_MAX];
	vidow_output_key(ws->output->name, key, sizeof(key));
	return strcmp(key, memory->key) == 0;
}

/* 把「属于 memory 这块屏」的工作区里的列整组搬到 to 上住（to 可以是 NULL =
 * 暂时没屏，列就挂着不动）。拔线时用：工作区名不变，列换了屏；插回来时按同一
 * 组名字搬回去，所以列的顺序、列宽、滚动位置都还在。
 *
 * to_memory：搬到哪块屏的**同名工作区**上（屏 A 的 "1" -> 屏 B 的 "1"；
 * 没有同名工作区就现场建一块）。这样多屏的时候布局不会挤成一团。 */
static void columns_migrate(struct vidow_server *server,
		struct vidow_output_memory *memory, struct wlr_output *to,
		struct vidow_output_memory *to_memory, bool adopt_orphans,
		size_t *out_columns, size_t *out_views) {
	*out_columns = 0;
	*out_views = 0;

	/* 先把要搬的列收集起来：搬运过程中会在工作区之间挪动链表。 */
	size_t n = 0;
	struct vidow_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (ws->special) {
			continue;
		}
		if (!(ws_on_memory(ws, memory) ||
				(adopt_orphans && ws->output == NULL))) {
			continue;
		}
		struct vidow_column *col;
		wl_list_for_each(col, &ws->columns, link) {
			n++;
		}
	}
	if (n == 0) {
		return;
	}

	struct vidow_column **cols = calloc(n, sizeof(*cols));
	if (cols == NULL) {
		return;
	}
	size_t i = 0;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (ws->special || !(ws_on_memory(ws, memory) ||
				(adopt_orphans && ws->output == NULL))) {
			continue;
		}
		struct vidow_column *col;
		wl_list_for_each(col, &ws->columns, link) {
			if (i < n) {
				cols[i++] = col;
			}
		}
	}

	for (i = 0; i < n; i++) {
		struct vidow_column *col = cols[i];
		/* 目的工作区按**名字**认：同一套名字在别的屏上是另一块工作区。 */
		struct vidow_workspace *target = NULL;
		if (to_memory != NULL) {
			wl_list_for_each(ws, &server->workspaces, link) {
				if (!ws->special &&
						ws_on_memory(ws, to_memory) &&
						strcmp(ws->name, col->ws != NULL ?
							col->ws->name : "") == 0) {
					target = ws;
					break;
				}
			}
			if (target == NULL) {
				target = vidow_workspace_create(server,
					col->ws != NULL ? col->ws->name : NULL, to);
			}
		}
		if (target == NULL) {
			continue; /* 没有接收的屏：列原地待着，屏回来再搬 */
		}
		wl_list_remove(&col->link);
		wl_list_insert(target->columns.prev, &col->link);
		col->ws = target;
		col->output = to;
		struct vidow_view *view;
		wl_list_for_each(view, &col->views, column_link) {
			view->output = to;
			(*out_views)++;
		}
		(*out_columns)++;
	}
	free(cols);
}

/* 这块屏上现在有多少列 / 多少个窗口（拔线日志要报的是这个数：工作区整块跟着
 * 列走，所以「搬了几列」在搬家之后往往已经是 0 —— 报原始数量才不会误导）。 */
static void count_output_columns(struct vidow_server *server,
		struct wlr_output *output, size_t *cols, size_t *views) {
	*cols = 0;
	*views = 0;
	struct vidow_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (ws->special || ws->output != output) {
			continue;
		}
		struct vidow_column *col;
		wl_list_for_each(col, &ws->columns, link) {
			(*cols)++;
			*views += (size_t)wl_list_length(&col->views);
		}
	}
}

void vidow_view_output_gone(struct vidow_server *server,
		struct vidow_output_memory *memory, struct wlr_output *dead,
		struct vidow_output *fallback) {
	struct wlr_output *to = fallback != NULL ? fallback->wlr_output : NULL;

	/* overview 是锁着一块屏的（每帧都按那块屏的工作区算缩放和相机）：它锁的
	 * 那块屏没了就把 overview 收掉，不然下一帧就在读已经释放的屏。先把 output
	 * 清掉再收 —— 收的过程里那几帧走 overview_apply() 的 output == NULL 分支。 */
	if (server->overview.output == dead) {
		server->overview.output = NULL;
		vidow_overview_exit(server);
	}

	size_t on_screen_cols = 0, on_screen_views = 0;
	count_output_columns(server, dead, &on_screen_cols, &on_screen_views);

	/* 先把「属于这块屏」的工作区整块交给接收的屏 —— 工作区是屏上的东西，屏没了
	 * 它就跟过去（名字不变），里面的列一列都不用动。之后按名字把列补回对应的
	 * 工作区（同名工作区已经合并到接收屏上那份了）。 */
	vidow_workspace_output_gone(server, dead, to);
	size_t cols = 0, in_columns = 0;
	struct vidow_output_memory *to_memory = fallback != NULL ?
		vidow_output_memory_for(&server->output_memories,
			fallback->wlr_output->name) : NULL;
	columns_migrate(server, memory, to, to_memory, false, &cols, &in_columns);

	size_t floats = 0;
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->column != NULL) {
			continue; /* 列里的窗口跟着列走过了 */
		}
		if (view->output != dead && view->home != memory) {
			continue;
		}
		bool movable = !view->popup &&
			view->float_mode != VIDOW_FLOAT_FOLLOW && view->placed;
		if (movable) {
			/* 「相对自己那块屏原点」的偏移（home_dx/home_dy）。位置本身不在这里摆：
			 * 拔掉一块屏之后 wlroots 会重排剩下的输出，这一刻问到的目的屏布局位置
			 * 还是旧的，等下一次摆放（vidow_float_place_all）时再算才准。 */
			view->home_dx = view->float_geom.x - memory->last_box.x;
			view->home_dy = view->float_geom.y - memory->last_box.y;
			floats++;
		}
		view->output = to;
		/* 位置不用在这里算：float.c 的摆放会把「相对偏移」换算成新屏上的绝对
		 * 位置（拔线那一刻目的屏的布局位置还是旧的，早了会算错）。 */
	}

	wlr_log(WLR_INFO, "hotplug: 拔掉 %s（%zu 列 / %zu 个窗口 / %zu 个浮动窗口），"
		"先挪到 %s 上住，归属记着",
		memory != NULL ? memory->key : dead != NULL ? dead->name : "?",
		on_screen_cols > 0 ? on_screen_cols : cols,
		on_screen_views > 0 ? on_screen_views : in_columns,
		floats, to != NULL ? to->name : "(没有屏了)");
}

void vidow_view_output_added(struct vidow_server *server,
		struct vidow_output_memory *memory, struct vidow_output *output) {
	/* 屏回来了：按 home 把属于它的工作区整块认领回来 —— 列、列宽、列序、滚动位置
	 * 全在工作区里跟着走，不需要再按名字搬一次（那样反倒会把列序转一圈）。 */
	vidow_workspace_output_added(server, memory, output->wlr_output);
	size_t cols = 0, in_columns = 0;
	count_output_columns(server, output->wlr_output, &cols, &in_columns);

	size_t floats = 0;
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->column != NULL || view->popup || view->home != memory) {
			continue;
		}
		bool movable = view->float_mode != VIDOW_FLOAT_FOLLOW && view->placed;
		if (!movable) {
			continue;
		}
		/* 号码报的是「回到这块屏的浮动窗口数」，不管它是跟着工作区回来的，
		 * 还是这一步才挪回来的。 */
		floats++;
		if (view->output == output->wlr_output) {
			continue; /* 已经跟着工作区回来了 */
		}
		view->output = output->wlr_output;
	}

	/* 滚动位置：离开时停在哪儿，回来还停在哪儿（列宽是列自己的，本来就在）。 */
	if (memory->last_box.width > 0) {
		output->viewport_x = memory->viewport_x;
	}

	vidow_arrange(server);

	/* 焦点：离开时谁在这儿，回来还是谁（前提是它还在、还映射着、真落在这块屏上）。 */
	if (view_alive(server, memory->focused) && memory->focused->mapped &&
			memory->focused->output == output->wlr_output) {
		vidow_focus_view(memory->focused);
	}

	if (cols > 0 || floats > 0) {
		wlr_log(WLR_INFO, "hotplug: %s 回来了，还原 %zu 列 / %zu 个窗口 / %zu 个"
			"浮动窗口（列宽、滚动位置都留着）", memory->key, cols,
			in_columns, floats);
	}
}

/* --------------------------------------------------------------- focus */

void vidow_focus_view(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = vidow_view_surface(view);
	struct wlr_surface *prev = seat->keyboard_state.focused_surface;

	if (prev != surface) {
		if (prev != NULL) {
			struct vidow_view *prev_view =
				vidow_view_from_surface(server, prev);
			/* prev 可能已经不是窗口了：正在销毁的那个窗口从 views 里摘掉之后
			 * 才轮到换焦点，或者它根本是 layer / 输入法这类表面 —— 那就没有
			 * 谁需要被取消激活（以前这里会把 NULL 传下去，直接段错误）。 */
			if (prev_view != NULL) {
				vidow_view_set_activated(prev_view, false);
				/* 窗口列表客户端（waybar 任务栏、portal）也要知道谁失去了焦点 */
				vidow_portal_view_activated(prev_view, false);
			}
		}

		if (server->keyboard_layer != NULL) {
			/* An exclusive layer surface owns the keyboard: activate the
			 * view, but do not steal focus. */
		} else {
			struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
			if (kb != NULL) {
				wlr_seat_keyboard_notify_enter(seat, surface, kb->keycodes,
					kb->num_keycodes, &kb->modifiers);
			} else {
				wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, NULL);
			}
		}
	}

	/* 焦点变了，把 text-input 的 enter/leave 转给客户端输入框 */
	vidow_im_focus(server, seat->keyboard_state.focused_surface);

	wlr_scene_node_raise_to_top(&view->scene_tree->node);
	vidow_view_set_activated(view, true);
	vidow_portal_view_activated(view, true);

	/* The border colour depends on focus, so repaint both windows. */
	struct vidow_view *previous = server->focused;
	server->focused = view;
	if (previous != view) {
		/* 「现在谁拿着键盘」是排查焦点问题（抢焦点 / 顶不上来）的第一现场，
		 * 而且只在真的换人时才打一行。 */
		wlr_log(WLR_INFO, "焦点: %s -> %s",
			previous != NULL && vidow_view_title(previous) != NULL ?
				vidow_view_title(previous) : "(无)",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)");
	}
	/* 工作区记着「离开时焦点是谁」，换回来的时候要还原。 */
	vidow_workspace_note_focus(view->ws, view);
	if (view->output != NULL) {
		server->focused_output = view->output;
	}

	vidow_arrange(server);

	if (previous != view) {
		vidow_ipc_emit_focus(server, view);
	}

	if (previous != NULL && previous != view && previous->fx.active) {
		vidow_view_apply_anim(previous);
	}
}

/* 「把这个窗口端到人面前」：xdg-activation（点通知 / 客户端自己顶上来）、
 * 任务栏按钮（foreign-toplevel 的 activate）、X11 的 _NET_ACTIVE_WINDOW 都走这里。
 *
 * 光调 vidow_focus_view 是不够的：窗口要是住在藏起来的工作区上，焦点就落在一个
 * 看不见的窗口上 —— 看起来就是「点了通知没反应」「切不过去」。所以先把它的工作区
 * 端出来（在别的屏上就把焦点挪过去，工作区跟着它的屏走），再给键盘焦点。 */
void vidow_view_activate(struct vidow_view *view) {
	if (view == NULL || view->server == NULL || view->popup) {
		return;
	}
	struct vidow_server *server = view->server;
	const char *title = vidow_view_title(view) != NULL ?
		vidow_view_title(view) : "(无题)";

	/* 规则说别理它的激活请求（后台 Electron 反复把自己顶上来那类）。 */
	if (!view->focus_on_activate) {
		wlr_log(WLR_INFO, "激活: %s 不理（窗口规则 focus_on_activate = false）",
			title);
		return;
	}

	/* 还没上屏：记下来，等 map 的时候兑现（点通知常常比窗口第一次 commit 还早）。 */
	if (!view->mapped) {
		view->pending_activate = true;
		wlr_log(WLR_INFO, "激活: %s 还没上屏，等它出来再聚焦", title);
		return;
	}

	/* 窗口在哪块屏上，人就跟到哪块屏（跨屏点通知也一样）。 */
	struct wlr_output *output = view->ws != NULL && view->ws->output != NULL ?
		view->ws->output : view->output;
	if (output != NULL && output != server->focused_output) {
		wlr_log(WLR_INFO, "激活: %s 在 %s 上，焦点跟过去", title, output->name);
		server->focused_output = output;
	}

	if (view->ws != NULL) {
		if (view->ws->special) {
			/* 特殊工作区：确保它正显示在这块屏上（显示在别的屏上就搬过来）。 */
			if (!view->ws->visible || view->ws->output != output) {
				wlr_log(WLR_INFO, "激活: %s 在特殊工作区 %s 上，把它开到 %s",
					title, view->ws->name,
					output != NULL ? output->name : "聚焦的屏");
				vidow_workspace_show_special(server, view->ws, output);
			}
		} else if (output == NULL ||
				vidow_workspace_shown_for(server, output) != view->ws) {
			/* 普通工作区：它没亮着（或者亮着的是别的屏）—— 先端出来。
			 * vidow_workspace_activate 认「现在聚焦的屏」，所以上面先把
			 * focused_output 挪到了窗口那块屏上。 */
			wlr_log(WLR_INFO, "激活: %s 在藏起来的 %s 上，先把它端出来",
				title, view->ws->name);
			vidow_workspace_activate(server, view->ws);
		}
	}

	vidow_focus_view(view);
}

static void focus_nearest_column(struct vidow_server *server,
		struct wlr_output *output) {
	struct vidow_column *col;
	struct wl_list *shown = output_shown_columns(server, output);
	wl_list_for_each(col, shown, link) {
		struct vidow_view *view = column_first_view(col);
		if (view != NULL) {
			vidow_focus_view(view);
			return;
		}
	}

	server->focused = NULL;
	wlr_seat_keyboard_notify_clear_focus(server->seat);
	vidow_im_focus(server, NULL);
	vidow_arrange(server);
}

/* 按屏幕位置挑「右边最近的那个窗口」。浮窗（和「列都空了、只剩浮窗」）用这条：
 * 它们不在列里，没有列序可看，只能量位置 —— x 比它大的里面取最小的（右边最近的），
 * 右边没有了取 x 最大的（左边最近的）。平铺窗口取它所在列**最上面**那个（列里没有
 * 「当前窗口」这个概念，别处（focus_nearest_column）也是这么认的）。 */
static struct vidow_view *focus_nearest_by_x(struct vidow_server *server,
		struct vidow_view *dying, struct wlr_output *output) {
	struct vidow_workspace *ws = dying->ws;
	struct vidow_view *v;
	struct vidow_view *right = NULL;
	struct vidow_view *left = NULL;
	struct vidow_view *next;

	if (ws == NULL || vidow_workspace_shown_for(server, output) != ws) {
		return NULL;
	}

	wl_list_for_each(v, &server->views, link) {
		if (v == dying || !v->mapped || v->popup || v->ws != ws ||
				v->output != output || v->geom.width <= 0) {
			continue;
		}
		if (v->geom.x > dying->geom.x) {
			if (right == NULL || v->geom.x < right->geom.x) {
				right = v;
			}
		} else if (left == NULL || v->geom.x > left->geom.x) {
			left = v;
		}
	}

	next = right != NULL ? right : left;
	if (next != NULL && next->column != NULL) {
		next = column_first_view(next->column);
	}
	return next;
}

/* 关掉一个窗口之后焦点落谁身上（niri 的规矩：**位置不动**，谁滑到空出来的位置上
 * 就是谁）：
 *   1) 列里还有窗口 —— 关掉那个的位置由它下面那个顶上；关的本来就是最下面那个，
 *      就往上找一个（列里只剩它一个的情况不算，见 2）；
 *   2) 整列没了 —— 右边最近的那一列顶上（右边那列滑到空出来的位置上）；
 *   3) 右边没有了 —— 才往左找最近的一列（关掉最右边那列时，niri 也是落到左边那个）；
 *   4) 不在列里（浮窗）/ 这块工作区上只剩浮窗 —— 按屏幕位置找右边最近的那个窗口
 *      （见 focus_nearest_by_x）：浮窗没有列序可看，但「往右」这条规矩一样成立。
 * 以前这里只做 1 的一半（只看下面），剩下全丢给 focus_nearest_column() —— 那个是
 * 「从左往右找第一个」，于是关掉右边一个窗口（浮窗更是），焦点蹦到最左边那列去了。
 *
 * 必须**在 view_leave_column() 之前**调：列一摘空就自己 free 了，邻居得趁它还挂在
 * 工作区链表上才找得到。返回 NULL = 这块工作区上一个窗口都不剩（或者它压根没摆出来，
 * 那就不该往这儿塞焦点）—— 交给调用方走 focus_nearest_column() 兜底。 */
static struct vidow_view *focus_after_close(struct vidow_server *server,
		struct vidow_view *view, struct wlr_output *output) {
	struct vidow_column *col = view->column;
	struct vidow_column *c;
	struct vidow_view *next;
	struct wl_list *node;

	if (col == NULL) {
		/* 浮窗：不在列里，按位置挑。 */
		return focus_nearest_by_x(server, view, output);
	}

	if (view->column_link.next != &col->views) {
		next = wl_container_of(view->column_link.next, next, column_link);
		return next;
	}
	if (view->column_link.prev != &col->views) {
		next = wl_container_of(view->column_link.prev, next, column_link);
		return next;
	}

	/* 列里就它一个：跨列找，但只在它那块工作区正摆在这块屏上时找 —— 藏着的
	 * 工作区里的窗口不该拿到键盘（那种情况走兜底，跟以前一样挑块看得见的）。 */
	if (col->ws == NULL || vidow_workspace_shown_for(server, output) != col->ws) {
		return NULL;
	}

	for (node = col->link.next; node != &col->ws->columns; node = node->next) {
		c = wl_container_of(node, c, link);
		next = column_first_view(c);
		if (next != NULL) {
			return next;
		}
	}
	for (node = col->link.prev; node != &col->ws->columns; node = node->prev) {
		c = wl_container_of(node, c, link);
		next = column_first_view(c);
		if (next != NULL) {
			return next;
		}
	}

	/* 这块工作区上没有别的列了：可能还剩浮窗 —— 它们照样该接住键盘（niri 的
	 * 「平铺关完落到浮窗上」）。挑不到就返回 NULL，调用方走兜底。 */
	return focus_nearest_by_x(server, view, output);
}

void vidow_focus_column(struct vidow_server *server, int direction) {
	struct vidow_column *col =
		server->focused != NULL ? server->focused->column : NULL;
	if (col == NULL) {
		focus_nearest_column(server, server->focused_output);
		return;
	}

	if (col->ws == NULL) {
		return;
	}
	struct wl_list *node = direction > 0 ? col->link.next : col->link.prev;
	while (node != &col->ws->columns) {
		struct vidow_column *c = wl_container_of(node, c, link);
		if (column_has_views(c)) {
			vidow_focus_view(column_first_view(c));
			return;
		}
		node = direction > 0 ? node->next : node->prev;
	}
}

void vidow_focus_window(struct vidow_server *server, int direction) {
	struct vidow_view *view = server->focused;
	if (view == NULL || view->column == NULL) {
		focus_nearest_column(server, server->focused_output);
		return;
	}

	struct wl_list *node = direction > 0 ? view->column_link.next :
		view->column_link.prev;
	if (node != &view->column->views) {
		struct vidow_view *next = wl_container_of(node, next, column_link);
		vidow_focus_view(next);
		return;
	}

	/* Single-window column (the common case): fall through to the
	 * neighbouring column so the key still does something useful. */
	vidow_focus_column(server, direction);
}

void vidow_move_column(struct vidow_server *server, int direction) {
	struct vidow_view *view = server->focused;
	if (view == NULL || view->column == NULL) {
		return;
	}
	struct vidow_column *col = view->column;
	if (col->ws == NULL) {
		return;
	}

	struct wl_list *node = direction > 0 ? col->link.next : col->link.prev;
	while (node != &col->ws->columns) {
		struct vidow_column *other = wl_container_of(node, other, link);
		wl_list_remove(&col->link);
		if (direction > 0) {
			wl_list_insert(&other->link, &col->link);
		} else {
			wl_list_insert(other->link.prev, &col->link);
		}
		vidow_arrange(server);
		return;
	}
}

static void view_move_to_column(struct vidow_view *view,
		struct vidow_column *target, bool append) {
	struct vidow_column *old = view->column;
	if (old == target) {
		return;
	}

	wl_list_remove(&view->column_link);
	view->column = target;
	if (append) {
		wl_list_insert(target->views.prev, &view->column_link);
	} else {
		wl_list_insert(&target->views, &view->column_link);
	}

	if (old != NULL && !column_has_views(old)) {
		if (old->ws != NULL) {
			wl_list_remove(&old->link);
		}
		free(old);
	}
	/* 这条窗口现在住在列里了：列里的窗口不算浮窗。 */
	vidow_workspace_track(target->ws, view, false);
}

void vidow_move_window(struct vidow_server *server, int direction) {
	struct vidow_view *view = server->focused;
	if (view == NULL || view->column == NULL) {
		return;
	}
	struct vidow_column *col = view->column;

	struct wl_list *node = direction > 0 ? view->column_link.next :
		view->column_link.prev;
	if (node != &col->views) {
		/* swap with the neighbouring window in the same column */
		wl_list_remove(&view->column_link);
		if (direction > 0) {
			wl_list_insert(node, &view->column_link);
		} else {
			wl_list_insert(node->prev, &view->column_link);
		}
		vidow_arrange(server);
		return;
	}

	/* Only window in the column: push it into the neighbouring column, which
	 * is how a column ends up stacking several windows. */
	if (col->ws == NULL) {
		return;
	}
	node = direction > 0 ? col->link.next : col->link.prev;
	while (node != &col->ws->columns) {
		struct vidow_column *other = wl_container_of(node, other, link);
		view_move_to_column(view, other, direction > 0);
		vidow_arrange(server);
		return;
	}
}

void vidow_cycle_column_width(struct vidow_server *server) {
	struct vidow_view *view = server->focused;
	if (view == NULL || view->column == NULL) {
		return;
	}

	static const double presets[] = { 1.0 / 3.0, 0.5, 2.0 / 3.0 };
	const size_t n = sizeof(presets) / sizeof(presets[0]);
	struct vidow_column *col = view->column;

	size_t current = n - 1;
	for (size_t i = 0; i < n; i++) {
		if (col->width_fraction < presets[i] + 0.01) {
			current = i;
			break;
		}
	}
	col->width_fraction = presets[(current + 1) % n];

	vidow_arrange(server);
}

void vidow_view_toggle_fullscreen(struct vidow_view *view) {
	view->fullscreen = !view->fullscreen;
	vidow_view_set_fullscreen(view, view->fullscreen);
	vidow_arrange(view->server);
	vidow_ipc_emit_window(view->server, "window-fullscreen-changed", view);
}

/* ------------------------------------------------------------- events */

/* 弹出物要收键盘（菜单的方向键、Esc，dmenu 的输入），但它不改「谁是焦点窗口」：
 * 只把 seat 的键盘焦点挪到它自己的表面上，server->focused 一动不动 —— 父窗口的
 * 焦点边框因此不会灭，关掉菜单后焦点也不会被重新算到别的窗口上去。
 * Wayland 那边同理：xdg_popup 不接管焦点，客户端从 toplevel 自己转发按键。 */
static void view_popup_grab_keyboard(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = vidow_view_surface(view);
	if (surface == NULL || seat->keyboard_state.focused_surface == surface) {
		return;
	}
	if (server->keyboard_layer != NULL) {
		/* 独占层拿着键盘：跟 vidow_focus_view 一样，不去抢。 */
		return;
	}
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	if (kb != NULL) {
		wlr_seat_keyboard_notify_enter(seat, surface, kb->keycodes,
			kb->num_keycodes, &kb->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, NULL);
	}
}

/* 弹出物收起来了：键盘还给焦点窗口；连焦点窗口都没了就当没人要键盘。 */
static void view_popup_release_keyboard(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *mine = vidow_view_surface(view);
	if (mine == NULL || seat->keyboard_state.focused_surface != mine) {
		return; /* 焦点已经不在它身上了（比如紧接着又弹了一个菜单） */
	}
	if (server->keyboard_layer != NULL) {
		return;
	}
	struct wlr_surface *back = server->focused != NULL ?
		vidow_view_surface(server->focused) : NULL;
	if (back == NULL) {
		wlr_seat_keyboard_notify_clear_focus(seat);
		return;
	}
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	if (kb != NULL) {
		wlr_seat_keyboard_notify_enter(seat, back, kb->keycodes,
			kb->num_keycodes, &kb->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(seat, back, NULL, 0, NULL);
	}
}

static void view_map(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, map);
	struct vidow_server *server = view->server;

	view->mapped = true;
	vidow_portal_view_mapped(view, true);
	/* 落进现在显示着的那块工作区：新窗口跟着走，不偷偷开在藏起来的工作区里。
	 * 列的窗口在 view_ensure_column 里落工作区；浮窗落在这里。 */
	if (!view->popup) {
		if (view->column != NULL && view->column->ws != NULL) {
			view->ws = view->column->ws;
		} else {
			view->ws = vidow_workspace_shown(server);
		}
	}
	/* 弹出物不进工作区，它跟父窗口走：父窗口在显示着的那块工作区里，它才显示。 */
	bool show = view->popup ?
		(view->parent == NULL ||
			vidow_workspace_view_visible(server, view->parent)) :
		vidow_workspace_view_visible(server, view);
	wlr_scene_node_set_enabled(&view->scene_tree->node, show);
	vidow_backdrop_damage(server, false);

	struct wlr_box geo = vidow_view_geometry(view);
	struct wlr_surface *surface = vidow_view_surface(view);
	wlr_log(WLR_INFO, "map %s: surface %dx%d geometry offset %d,%d",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(untitled)",
		surface != NULL ? surface->current.width : 0,
		surface != NULL ? surface->current.height : 0, geo.x, geo.y);

	/* Re-run detection here: clients that pin a fixed size (dialogs) only
	 * send their min/max hints after the first configure round-trip. */
	vidow_float_update_mode(view);
	/* 窗口规则（app_id / 标题 / class 这时都齐了） */
	vidow_rule_apply(server, view);

	if (view->float_mode == VIDOW_FLOAT_NONE) {
		view_ensure_column(server, view);
	} else if (!view->popup) {
		vidow_workspace_track(view->ws, view, true);
	}
	/* 弹出物不抢焦点：它不装饰，焦点边框长在父窗口上，父窗口的边框不该因为
	 * 弹出一个菜单就灭掉；关掉菜单后焦点也不该被重新算到别的窗口上去。
	 * 键盘照给它（X11 的菜单要收方向键、Esc），见 view_popup_grab_keyboard。 */
	if (view->popup) {
		/* 分类往往发生在表面建出来之前（那时候没东西可摆），所以这里再按 X
		 * 说的地方摆一次；顺带保证第一次上屏就落在对的位置上，不会闪一下。 */
		vidow_popup_place(view);
		view_popup_grab_keyboard(view);
	} else if (view->pending_activate) {
		/* 有人早就请求激活它了（点通知的时候窗口还没上屏）：现在兑现。 */
		view->pending_activate = false;
		vidow_view_activate(view);
	} else if (!view->focus_on_open && server->focused != NULL) {
		/* 规则说这个应用开窗口别抢焦点（Electron 系的通知 / 剪贴板辅助窗口
		 * 就是这样：开一个看不见的窗口把你正在打字的窗口顶掉）。窗口照常
		 * 出现、照常落在当前工作区，只是不动键盘焦点。当前一个焦点窗口都
		 * 没有时（server->focused == NULL）还是给它 —— 否则没人收键盘。 */
		wlr_log(WLR_INFO, "map: %s 不抢焦点（窗口规则 focus_on_open = false）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)");
	} else {
		vidow_focus_view(view);
	}

	/* Everything above placed the window where it belongs; now play the
	 * opening effect on top of that final position. */
	vidow_view_open_anim(view);

	vidow_view_note_names(view);
	vidow_ipc_emit_window(server, "window-opened", view);
}

static void view_unmap(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, unmap);
	struct vidow_server *server = view->server;
	struct wlr_output *output = view->output;

	view->mapped = false;
	vidow_portal_view_mapped(view, false);
	if (!view->popup && view->float_mode != VIDOW_FLOAT_NONE) {
		/* 浮窗离开工作区；列里的窗口跟着列，不用记账。 */
		vidow_workspace_track(view->ws, view, false);
	}
	if (view->dragging) {
		/* 拖着的时候窗口没了：撤掉 grab，别让指针还挂在一个已经没了的窗口上。 */
		view->dragging = false;
		if (server->grab_view == view) {
			server->grab_mode = VIDOW_GRAB_NONE;
			server->grab_view = NULL;
			wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
				"left_ptr");
		}
	}
	view_snapshot_close(view);
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	vidow_backdrop_damage(server, false);

	if (server->grab_view == view) {
		vidow_float_end_grab(server);
	}

	if (view->popup) {
		/* 弹出物从来没有成为过 server->focused，只需要把它握着的键盘还回去。 */
		view_popup_release_keyboard(view);
		vidow_arrange(server);
		return;
	}

	if (server->focused == view) {
		/* 邻居要在摘列之前找（见 focus_after_close）。 */
		struct vidow_view *next = focus_after_close(server, view, output);
		view_leave_column(view);
		server->focused = NULL;
		if (next != NULL) {
			vidow_focus_view(next);
		} else {
			focus_nearest_column(server, output);
		}
	} else {
		view_leave_column(view);
	}

	/* 列真的摘掉了，它那块工作区才可能变空 —— 栈中间的空工作区该收掉（niri：
	 * 切走 / 关掉之后那块就没了）。放在 leave_column 之后：之前它还不算空。 */
	vidow_workspace_cleanup(server);
	vidow_arrange(server);
}

static void view_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, commit);

	/* New pixels: everything that samples this window as a backdrop (a
	 * floating window on top of it) has to be re-blurred. */
	vidow_backdrop_damage(view->server, false);

	if (view->kind == VIDOW_VIEW_X11) {
		/* X11：缓冲区的缩放比例要跟着 xwayland_scale 走（尺寸变了就重算），
		 * 其余和 xdg 一样 —— 浮动窗口自己定尺寸，贴图重画，最后锁一份给
		 * 关闭动画。 */
		vidow_view_x11_scale(view);
		view_dst_capture(view);
		if (view->float_mode != VIDOW_FLOAT_NONE) {
			vidow_float_on_commit(view);
		}
		if (view->mapped && view->fx.active) {
			vidow_view_apply_anim(view);
		}
		struct wlr_client_buffer *client =
			vidow_view_surface(view) != NULL ?
			vidow_view_surface(view)->buffer : NULL;
		if (client != NULL && client->source != NULL &&
				client->source != view->last_buffer) {
			wlr_buffer_unlock(view->last_buffer);
			view->last_buffer = wlr_buffer_lock(client->source);
		}
		return;
	}

	/* xdg-shell: the initial commit must be answered with a configure. */
	if (view->xdg_toplevel->base->initial_commit) {
		vidow_float_update_mode(view);
		if (view->float_mode == VIDOW_FLOAT_NONE) {
			view_ensure_column(view->server, view);
			vidow_arrange(view->server);
		} else {
			/* 浮动窗口不进列，arrange 碰不到它，得自己回一个 configure。
			 * 尺寸 0x0 = 让客户端自己定（浮动窗口的常规语义）。
			 *
			 * 这一支不是可有可无的：xdg-foreign 会在客户端第一次 commit
			 * *之前* 就把 parent 设好（Qt/KDE 的 portal 对话框就是这样），
			 * 于是 detect_mode() 在更早的 view_set_parent 里就已经把模式
			 * 定成 FOLLOW 了，到这里 vidow_float_update_mode() 是空操作，
			 * 没人发 configure 客户端就一直等。 */
			wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
		}
		view->configured = true;
		return;
	}

	if (view->float_mode != VIDOW_FLOAT_NONE) {
		/* floating windows are sized by the client */
		vidow_float_on_commit(view);
	}

	/* 客户端按几倍渲染的 —— 「清晰度」的客户端那一半。我们通过
	 * wl_surface.preferred_buffer_scale / wp_fractional_scale 告诉它按输出的
	 * 缩放画，这里核对它照做了没有：缓冲区像素数应该等于逻辑尺寸乘缩放。 */
	struct wlr_surface *surface = vidow_view_surface(view);
	if (surface != NULL &&
			view->logged_buf_w != surface->current.buffer_width) {
		view->logged_buf_w = surface->current.buffer_width;
		/* state 里不留 buffer_scale，用缓冲区像素 / 逻辑尺寸反推。 */
		double client_scale = surface->current.width > 0 ?
			(double)surface->current.buffer_width /
				(double)surface->current.width : 1.0;
		view->logged_scale = (int)lround(client_scale);
		wlr_log(WLR_INFO, "surface %s: 缓冲 %dx%d 声明 %d 倍 -> 逻辑 %dx%d"
			"（输出 %.2fx，设备像素 %dx%d）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			surface->current.buffer_width, surface->current.buffer_height,
			(int)lround(surface->current.width > 0 ?
				(double)surface->current.buffer_width /
					(double)surface->current.width : 1.0),
			surface->current.width, surface->current.height,
			(double)(view->output != NULL ? view->output->scale : 1.0),
			(int)lround(surface->current.width *
				(view->output != NULL ? (double)view->output->scale : 1.0)),
			(int)lround(surface->current.height *
				(view->output != NULL ? (double)view->output->scale : 1.0)));
	}

	/* The visible pixels come from our offscreen decoration buffer now, so a
	 * client repaint has to be pushed through it again. */
	if (view->mapped && view->fx.active) {
		vidow_view_apply_anim(view);
	}

	/* Keep a locked reference to the last frame: the close animation needs
	 * pixels, and by the time unmap fires the surface has none.
	 *
	 * Lock the client's own buffer, not the wlr_client_buffer wrapper: the
	 * wrapper goes away as soon as the surface drops it, while a lock on the
	 * source keeps the pixels alive until the animation is done. */
	struct wlr_client_buffer *client =
		view->xdg_toplevel->base->surface->buffer;
	if (client != NULL && client->source != NULL &&
			client->source != view->last_buffer) {
		wlr_buffer_unlock(view->last_buffer);
		view->last_buffer = wlr_buffer_lock(client->source);
	}
}

static void view_set_parent(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, set_parent);
	vidow_float_update_mode(view);
	/* xdg-foreign 导入父窗口时走的就是这条路（wlroots 替客户端调用
	 * wlr_xdg_toplevel_set_parent），窗口列表那边也要跟上。 */
	vidow_portal_view_parent(view);
}

static void view_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, destroy);
	struct vidow_server *server = view->server;
	struct wlr_output *output = view->output;

	/* 事件要带标题，而标题是客户端的资源、销毁信号来的时候已经收走了 ——
	 * 用 map / 改标题时记下的快照（见 vidow_view_note_names）。 */
	vidow_ipc_emit_window_closed(server, view->id,
		view->last_title[0] != '\0' ? view->last_title :
			vidow_view_title(view),
		view->last_app_id[0] != '\0' ? view->last_app_id :
			vidow_view_app_id(view));

	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->set_title.link);
	wl_list_remove(&view->set_parent.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->destroy.link);
	view_capture_holds_clear(view);
	vidow_portal_view_free(view);
	wl_list_remove(&view->link);

	/* The scene node is owned by wlroots and may outlive the view; clear the
	 * tag so hit-testing cannot reach the freed struct. */
	if (view->scene_tree != NULL) {
		view->scene_tree->node.data = NULL;
	}
	if (server->grab_view == view) {
		vidow_float_end_grab(server);
	}
	vidow_float_detach_children(server, view);
	wlr_buffer_unlock(view->last_buffer);
	view->last_buffer = NULL;
	vidow_fx_release(&view->fx);
	vidow_anim_cancel(view->anim_move);
	view->anim_move = NULL;
	vidow_anim_cancel(view->anim_open);
	view->anim_open = NULL;

	bool was_focused = server->focused == view;
	/* 邻居要在摘列（和 free(view)）之前找（见 focus_after_close）。 */
	struct vidow_view *next = was_focused ?
		focus_after_close(server, view, output) : NULL;
	if (was_focused) {
		server->focused = NULL;
	}
	view_leave_column(view);
	vidow_workspace_cleanup(server);

	free(view);

	if (was_focused) {
		if (next != NULL) {
			vidow_focus_view(next);
		} else {
			focus_nearest_column(server, output);
		}
	} else {
		vidow_arrange(server);
	}
}

static void view_set_title(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, set_title);
	wlr_log(WLR_INFO, "title %s", vidow_view_title(view) != NULL ?
		vidow_view_title(view) : "(untitled)");
	vidow_portal_view_title(view);
	/* 规则可能按标题匹配：标题变了要重新判一遍。 */
	vidow_rule_apply(view->server, view);
	vidow_view_note_names(view);
	vidow_ipc_emit_window(view->server, "window-title-changed", view);
}

static void view_request_fullscreen(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, request_fullscreen);
	bool want = view->xdg_toplevel->requested.fullscreen;

	view->fullscreen = want;
	vidow_view_set_fullscreen(view, want);
	vidow_arrange(view->server);
	vidow_ipc_emit_window(view->server, "window-fullscreen-changed", view);
}

static void view_request_maximize(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, request_maximize);

	/* Unsupported: still answer with a configure, as xdg-shell requires. */
	wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, false);
}

static void view_request_move(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, request_move);
	struct vidow_server *server = view->server;

	/* A CSD titlebar drag: detach a tiled window into a float and grab it. */
	if (view->float_mode == VIDOW_FLOAT_NONE) {
		vidow_float_set_mode(view, VIDOW_FLOAT_FIXED);
	}
	vidow_float_begin_grab(server, view, false);
}

static void view_request_resize(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, request_resize);
	struct vidow_server *server = view->server;

	if (view->float_mode == VIDOW_FLOAT_NONE) {
		vidow_float_set_mode(view, VIDOW_FLOAT_FIXED);
	}
	vidow_float_begin_grab(server, view, true);
}

/* ------------------------------------------------------- X11（XWayland） */

static bool x11_is_popup(const struct wlr_xwayland_surface *x);

/* X11 的窗口类型决定它是平铺还是浮动、要不要装饰。跟 xdg 那边的
 * detect_mode() 是一个意思，只是信息源不一样。 */
static const enum wlr_xwayland_net_wm_window_type x11_float_types[] = {
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_MENU,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_NOTIFICATION,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_COMBO,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DND,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DOCK,
};

/* 这些窗口是「弹出物」：自己定位置尺寸，我们不该给它套边框。 */
static const enum wlr_xwayland_net_wm_window_type x11_overlay_types[] = {
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_MENU,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_COMBO,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DND,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_NOTIFICATION,
};

static bool x11_has_any_type(const struct wlr_xwayland_surface *x,
		const enum wlr_xwayland_net_wm_window_type *types, size_t count) {
	for (size_t i = 0; i < count; i++) {
		if (wlr_xwayland_surface_has_window_type(x, types[i])) {
			return true;
		}
	}
	return false;
}

static bool x11_has_fixed_size(const struct wlr_xwayland_surface *x) {
	const xcb_size_hints_t *hints = x->size_hints;
	if (hints == NULL) {
		return false;
	}
	bool min_set = (hints->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) != 0;
	bool max_set = (hints->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) != 0;
	return min_set && max_set && hints->min_width == hints->max_width &&
		hints->min_height == hints->max_height;
}

/* 客户端声明的固定尺寸，逻辑单位。 */
bool vidow_view_fixed_size_hint(const struct vidow_view *view, int *w, int *h) {
	if (view->kind == VIDOW_VIEW_X11) {
		const xcb_size_hints_t *hints =
			view->x11 != NULL ? view->x11->size_hints : NULL;
		if (hints == NULL ||
				(hints->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) == 0 ||
				(hints->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) == 0 ||
				hints->min_width != hints->max_width ||
				hints->min_height != hints->max_height ||
				hints->min_width < 2 || hints->min_height < 2) {
			return false;
		}
		int scale = vidow_view_x11_scale_int(view);
		*w = hints->min_width / scale;
		*h = hints->min_height / scale;
		return true;
	}
	if (view->xdg_toplevel == NULL) {
		return false;
	}
	const struct wlr_xdg_toplevel_state *st = &view->xdg_toplevel->current;
	if (st->min_width < 2 || st->min_height < 2 ||
			st->min_width != st->max_width ||
			st->min_height != st->max_height) {
		return false;
	}
	*w = st->min_width;
	*h = st->min_height;
	return true;
}

enum vidow_float_mode vidow_x11_detect_mode(struct vidow_view *view) {
	struct wlr_xwayland_surface *x = view->x11;
	if (x == NULL) {
		return VIDOW_FLOAT_NONE;
	}
	/* 弹出物排在最前：它自带绝对位置，当成「跟着父窗口偏移」会算错，而且它
	 * 的位置由客户端的 X 坐标说了算，压根不参与我们的布局推算。 */
	if (x11_is_popup(x)) {
		return VIDOW_FLOAT_FIXED;
	}
	/* 有父窗口就跟着父窗口（对话框）。 */
	if (x->parent != NULL) {
		return VIDOW_FLOAT_FOLLOW;
	}
	if (x->modal ||
			x11_has_any_type(x, x11_float_types,
				sizeof(x11_float_types) / sizeof(x11_float_types[0])) ||
			x11_has_fixed_size(x)) {
		return VIDOW_FLOAT_FIXED;
	}
	return VIDOW_FLOAT_NONE;
}

/* 这个 X11 窗口是不是「弹出物」——菜单 / 下拉框 / 提示气泡 / 通知。
 *
 * 两类来源：
 *   1. override-redirect：X11 的「别管我」标记，菜单和 tooltip 的基本做法；
 *   2. `_NET_WM_WINDOW_TYPE` 里那几种弹出类型（有些客户端不设 OR，只声明类型）。
 *
 * 它们在 Wayland 那边对应 xdg_popup，vidow 对 xdg_popup 做的事是：挂在父窗口的
 * 场景树上、不装饰、不跑 shader、不做动画、位置尺寸听客户端的 —— 这里一一对齐。
 *
 * 判据不看 `_MOTIF_WM_HINTS`：Wayland 没有 xdg-decoration，客户端没法要求「别
 * 装饰我」，vidow 一律给 xdg_toplevel 套装饰，X11 的普通窗口也跟它一样。
 * （`_MOTIF_WM_HINTS` 还有个坑：XWM 是在窗口刚建出来时读它的，客户端往往之后才
 * 设，读到的永远是初始值。） */
static bool x11_is_popup(const struct wlr_xwayland_surface *x) {
	if (x->override_redirect) {
		return true;
	}
	return x11_has_any_type(x, x11_overlay_types,
		sizeof(x11_overlay_types) / sizeof(x11_overlay_types[0]));
}

/* 从 X 报的位置找回它在哪块屏上（菜单是绝对定位的）。 */
static void x11_track_output(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	if (view->x11 == NULL) {
		return;
	}
	int scale = vidow_view_x11_scale_int(view);
	double lx = (double)view->x11->x / scale;
	double ly = (double)view->x11->y / scale;
	struct wlr_output *output =
		wlr_output_layout_output_at(server->output_layout, lx, ly);
	if (output != NULL) {
		view->output = output;
	} else if (view->output == NULL) {
		view->output = server->focused_output;
	}
}

/* 弹出物摆在哪、多大，完全由客户端（X）决定：X 的坐标是屏幕绝对坐标，等于逻辑
 * 坐标 × xwayland_scale，除回去就是场景里的位置。不居中、不夹工作区 —— 客户端
 * 自己会看屏幕边界，我们再夹一次只会把它挪到错的地方（菜单跑到屏幕中间）。
 *
 * 副作用是顺手把 view->output 归到坐标底下那块屏上。 */
bool vidow_view_x11_popup_box(struct vidow_view *view, struct wlr_box *box) {
	if (!view->popup || view->x11 == NULL) {
		return false;
	}
	if (view->x11->width <= 0 || view->x11->height <= 0) {
		return false; /* 还没定尺寸，等它自己说 */
	}
	int scale = vidow_view_x11_scale_int(view);
	box->x = x11_floor_div(view->x11->x, scale);
	box->y = x11_floor_div(view->x11->y, scale);
	box->width = view->x11->width / scale;
	box->height = view->x11->height / scale;
	if (box->width < 1) {
		box->width = 1;
	}
	if (box->height < 1) {
		box->height = 1;
	}
	x11_track_output(view);
	return true;
}

/* 视图在「弹出物」和「普通窗口」之间改判时，装饰/动画状态得整份换掉。
 *
 * 关键在装饰开着的时候客户端表面是被隐藏的（内容由装饰着色器连同边框、圆角、
 * 阴影、模糊一起画进离屏缓冲），所以收掉装饰必须把表面的透明度还回去 ——
 * 不然弹出物会变成一块什么都没有的透明块；反过来变回普通窗口则要重新接上装饰。
 * 两个方向都由 vidow_view_apply_anim() 收尾。 */
static void view_swap_decor(struct vidow_view *view) {
	vidow_anim_cancel(view->anim_open);
	view->anim_open = NULL;
	vidow_anim_cancel(view->anim_move);
	view->anim_move = NULL;
	view->anim_alpha = 1.0;
	view->anim_off_x = 0.0;
	view->anim_off_y = 0.0;
	view->anim_scale = 1.0;
	vidow_fx_end(view->server, &view->fx);
	if (view->scene_surface != NULL && view->scene_surface->buffer != NULL) {
		wlr_scene_buffer_set_opacity(view->scene_surface->buffer, 1.0f);
	}
	if (!view->popup) {
		/* 变回普通窗口：把装饰接回来（shader 名传 NULL = 只要装饰，不要动画） */
		vidow_fx_begin(view->server, &view->fx, view->scene_tree, NULL);
	}
	vidow_view_apply_anim(view);
}

/* 「它是不是弹出物」的答案会变：X11 的属性是分成好几条消息到的 —— 窗口刚建出来
 * 时 `_NET_WM_WINDOW_TYPE` 往往还没设（客户端先 create 再 set property），
 * override-redirect 也可能后改。所以每条相关属性到来都重算一次；变了就把场景节点
 * 搬到对应的层上（弹出物要在所有托管窗口之上）、换掉装饰状态，并让布局重判一次。 */
static void x11_refresh_popup(struct vidow_view *view) {
	bool popup = view->x11 != NULL && x11_is_popup(view->x11);
	if (popup == view->popup) {
		return;
	}
	view->popup = popup;
	if (view->scene_tree != NULL) {
		wlr_scene_node_reparent(&view->scene_tree->node, popup ?
			view->server->layer_popups : view->server->layer_toplevels);
		view_swap_decor(view);
	}
	wlr_log(WLR_INFO, "x11: %s 归类为%s",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
		popup ? "弹出物（不装饰、不进布局、位置听客户端的）" : "普通窗口");
}

static void x11_attach_surface(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	if (view->x11 == NULL || view->x11->surface == NULL ||
			view->scene_tree != NULL) {
		return;
	}

	/* 弹出物挂在自己的层（见 x11_refresh_popup）：X11 里 override-redirect
	 * 窗口永远在所有托管窗口之上。 */
	view->popup = x11_is_popup(view->x11);
	view->scene_tree = wlr_scene_tree_create(view->popup ?
		server->layer_popups : server->layer_toplevels);
	if (view->scene_tree == NULL) {
		return;
	}
	view->scene_tree->node.data = &view->tag;
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);

	view->scene_surface = wlr_scene_surface_create(view->scene_tree,
		view->x11->surface);
	if (view->scene_surface == NULL) {
		wlr_log(WLR_ERROR, "x11: 建不出场景表面");
		return;
	}

	view->map.notify = view_map;
	wl_signal_add(&view->x11->surface->events.map, &view->map);
	view->unmap.notify = view_unmap;
	wl_signal_add(&view->x11->surface->events.unmap, &view->unmap);
	view->commit.notify = view_commit;
	wl_signal_add(&view->x11->surface->events.commit, &view->commit);

	view->popup = x11_is_popup(view->x11);
	vidow_view_x11_scale(view);

	/* 表面在我们建 view 之前就已经 map 了：Xwayland 启动那一下事件是成批到的
	 * （xwm 先把窗口 map 掉，再给我们 new_surface），map 信号早就过去了 ——
	 * 不补这一下，这个 view 的场景 node 就一直停在 disabled：画面里没有它、
	 * 没有摆放、也没有开窗动画（踩过：X11 弹出物和父窗口整块不显示，像素断言
	 * 全挂，X 服务器上却有这个窗口）。正常顺序是「先建 view 再 map」，这里只是
	 * 把错过的那次 map 补回来。 */
	if (view->x11->surface->mapped) {
		view_map(&view->map, NULL);
	}
}

/* wayland surface 没了（窗口 withdrawn）但 X11 窗口还在：view 留着，下次
 * associate 重新建场景节点。 */
static void x11_drop_surface(struct vidow_view *view) {
	if (view->scene_tree == NULL) {
		return;
	}
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->commit.link);
	/* 场景节点归 wlroots 所有，可能比 view 活得久：先把 tag 摘掉。 */
	view->scene_tree->node.data = NULL;
	/* 表面那个 buffer 节点**不碰**：它是 view->scene_tree 的孩子，表面一没
	 * wlroots 就顺手把它销毁了（scene_surface_handle_surface_destroy），
	 * 它的 tag 跟着一起走，不会留下野指针。反过来，这个回调有可能在它之后才
	 * 跑到（退出时 wl_display_destroy_clients 先收客户端表面），那时候
	 * view->scene_surface 已经指向释放过的内存 —— 老代码在这里摘 tag，
	 * IPC quit 必段错误（wlr_scene_node_destroy 也不允许节点身上还挂着
	 * destroy 监听器或 addon，想挂钩子都不行）。 */
	view->scene_surface = NULL;
	view->scene_tree = NULL;
}

static void x11_associate(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, x11_associate);
	x11_attach_surface(view);
	wlr_log(WLR_DEBUG, "x11: associate %s",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)");
}

static void x11_dissociate(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, x11_dissociate);
	x11_drop_surface(view);
}

static void x11_set_title(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, x11_set_title);
	wlr_log(WLR_INFO, "title %s", vidow_view_title(view) != NULL ?
		vidow_view_title(view) : "(无题)");
	vidow_portal_view_title(view);
	vidow_rule_apply(view->server, view);
	vidow_view_note_names(view);
	vidow_ipc_emit_window(view->server, "window-title-changed", view);
}

static void x11_set_class(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, x11_set_class);
	vidow_portal_view_title(view);
	vidow_rule_apply(view->server, view);
	vidow_view_note_names(view);
}

static void x11_set_parent(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, x11_set_parent);
	vidow_float_update_mode(view);
	vidow_portal_view_parent(view);
}

static void x11_set_geometry(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, x11_set_geometry);
	if (!view->popup) {
		/* 普通窗口的位置由布局决定，X 报的坐标只在浮动窗口的首次摆放里当参考。 */
		return;
	}
	/* 弹出物改位置/尺寸就是它自己的事：重摆一遍（位置从 X 读）。 */
	vidow_arrange(view->server);
}

static void x11_set_override_redirect(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view =
		wl_container_of(listener, view, x11_set_override_redirect);
	x11_refresh_popup(view);
	vidow_float_update_mode(view);
	vidow_arrange(view->server);
}

static void x11_set_hints(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, x11_set_hints);
	/* min/max 提示要等到这里才齐，浮动判断得重跑一遍。 */
	x11_refresh_popup(view);
	vidow_float_update_mode(view);
}

/* `_NET_WM_WINDOW_TYPE` 是**另一条**消息（wlroots 的 set_hints 是 WM_HINTS）：
 * 客户端可以在映射之后才把窗口类型设成菜单/提示/下拉框，也可能反过来。类型变了
 * 就得重新分类（弹出物 <-> 普通窗口）、甚至把场景节点搬到另一层上。 */
static void x11_set_window_type(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view =
		wl_container_of(listener, view, x11_set_window_type);
	x11_refresh_popup(view);
	vidow_float_update_mode(view);
}

/* WM_NORMAL_HINTS（min/max 尺寸）是另一条信号，必须有自己的回调：
 * wl_container_of 是按 listener 字段的地址反推 view 的，两个信号共用一个回调
 * 会算出错的 view 指针（这里踩过一次，直接段错误）。 */
static void x11_set_size_hints(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view =
		wl_container_of(listener, view, x11_set_size_hints);
	vidow_float_update_mode(view);
}

static void x11_request_configure(struct wl_listener *listener, void *data) {
	struct vidow_view *view =
		wl_container_of(listener, view, x11_request_configure);
	struct wlr_xwayland_surface_configure_event *event = data;

	if (event->width > 1 && event->height > 1) {
		/* 记下来：这个请求常常早于「我们认出它是个对话框」，见 server.h */
		view->x11_want_w = event->width;
		view->x11_want_h = event->height;
	}
	/* 客户端往往先报尺寸、后报窗口类型/尺寸提示，先按最新的信息重判一次模式，
	 * 免得把一个「其实该浮动」的窗口当平铺窗口把尺寸请求吞掉。 */
	vidow_float_update_mode(view);

	wlr_log(WLR_DEBUG, "x11: %s 请求 configure %d,%d %ux%u（mask 0x%x）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
		event->x, event->y, event->width, event->height, event->mask);

	/* overlay 窗口（菜单/下拉框）是在请求自己的位置和尺寸，照单全收：我们只管
	 * 显示它，不指挥它。override-redirect 的窗口连这里都到不了（X 服务器直接
	 * 照办，不会来问 WM），会走这里的是「声明了弹出类型但没设 OR」的那批。 */
	if (view->popup) {
		uint32_t m = event->mask;
		int x = (m & XCB_CONFIG_WINDOW_X) != 0 ? event->x : view->x11->x;
		int y = (m & XCB_CONFIG_WINDOW_Y) != 0 ? event->y : view->x11->y;
		int w = (m & XCB_CONFIG_WINDOW_WIDTH) != 0 ?
			(int)event->width : view->x11->width;
		int h = (m & XCB_CONFIG_WINDOW_HEIGHT) != 0 ?
			(int)event->height : view->x11->height;
		if (w < 1) {
			w = 1;
		}
		if (h < 1) {
			h = 1;
		}
		wlr_xwayland_surface_configure(view->x11, x, y, w, h);
		vidow_arrange(view->server);
		return;
	}
	/* 浮动窗口自己定尺寸：客户端请求多大就给多大（X11 里这是它报名尺寸的
	 * 正常途径）。位置仍按我们的规则（居中 / 跟随父窗口）。 */
	if (view->float_mode != VIDOW_FLOAT_NONE && event->width > 1 &&
			event->height > 1) {
		int scale = vidow_view_x11_scale_int(view);
		view->float_geom.width = event->width / scale;
		view->float_geom.height = event->height / scale;
		vidow_arrange(view->server);
		return;
	}
	/* 平铺窗口的尺寸由布局决定，客户端说了不算；照旧重排一次，等于回一个
	 * configure 告诉它最终的几何，免得它一直等。 */
	(void)event;
	vidow_arrange(view->server);
}
static void x11_request_move(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, x11_request_move);
	struct vidow_server *server = view->server;
	if (view->float_mode == VIDOW_FLOAT_NONE) {
		vidow_float_set_mode(view, VIDOW_FLOAT_FIXED);
	}
	vidow_float_begin_grab(server, view, false);
}

static void x11_request_resize(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, x11_request_resize);
	struct vidow_server *server = view->server;
	if (view->float_mode == VIDOW_FLOAT_NONE) {
		vidow_float_set_mode(view, VIDOW_FLOAT_FIXED);
	}
	vidow_float_begin_grab(server, view, true);
}

static void x11_request_activate(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view =
		wl_container_of(listener, view, x11_request_activate);
	/* X11 的 _NET_ACTIVE_WINDOW（wmctrl、Electron 的通知、任务栏都在用）：
	 * 跟 Wayland 的激活请求走同一条路（先把工作区端出来，再给焦点）。 */
	vidow_view_activate(view);
}

static void x11_request_close(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, x11_request_close);
	/* 客户端（或者 wmctrl 之类）要求关自己：我们只负责把请求传下去。 */
	vidow_view_close(view);
}

static void x11_request_fullscreen(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view =
		wl_container_of(listener, view, x11_request_fullscreen);
	if (view->x11 == NULL) {
		return;
	}
	view->fullscreen = view->x11->fullscreen;
	vidow_view_set_fullscreen(view, view->fullscreen);
	vidow_arrange(view->server);
}

static void x11_request_maximize(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view =
		wl_container_of(listener, view, x11_request_maximize);
	/* 不支持最大化：回一个不最大化，客户端就不会卡在等待里。 */
	if (view->x11 != NULL) {
		wlr_xwayland_surface_set_maximized(view->x11, false, false);
	}
}

static void x11_destroy_view(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, x11_destroy);
	struct vidow_server *server = view->server;
	struct wlr_output *output = view->output;

	vidow_ipc_emit_window_closed(server, view->id,
		view->last_title[0] != '\0' ? view->last_title :
			vidow_view_title(view),
		view->last_app_id[0] != '\0' ? view->last_app_id :
			vidow_view_app_id(view));

	wl_list_remove(&view->x11_associate.link);
	wl_list_remove(&view->x11_dissociate.link);
	wl_list_remove(&view->x11_set_title.link);
	wl_list_remove(&view->x11_set_class.link);
	wl_list_remove(&view->x11_set_parent.link);
	wl_list_remove(&view->x11_set_geometry.link);
	wl_list_remove(&view->x11_set_override_redirect.link);
	wl_list_remove(&view->x11_set_hints.link);
	wl_list_remove(&view->x11_set_size_hints.link);
	wl_list_remove(&view->x11_set_window_type.link);
	wl_list_remove(&view->x11_request_configure.link);
	wl_list_remove(&view->x11_request_move.link);
	wl_list_remove(&view->x11_request_resize.link);
	wl_list_remove(&view->x11_request_activate.link);
	wl_list_remove(&view->x11_request_close.link);
	wl_list_remove(&view->x11_request_fullscreen.link);
	wl_list_remove(&view->x11_request_maximize.link);
	wl_list_remove(&view->x11_destroy.link);

	x11_drop_surface(view);
	/* 弹出物可能还攥着键盘（它从来不当焦点窗口），窗口没了就得还回去。 */
	view_popup_release_keyboard(view);
	vidow_portal_view_free(view);
	wl_list_remove(&view->link);

	if (server->grab_view == view) {
		vidow_float_end_grab(server);
	}
	vidow_float_detach_children(server, view);
	wlr_buffer_unlock(view->last_buffer);
	view->last_buffer = NULL;
	vidow_fx_release(&view->fx);
	vidow_anim_cancel(view->anim_move);
	view->anim_move = NULL;
	vidow_anim_cancel(view->anim_open);
	view->anim_open = NULL;

	bool was_focused = server->focused == view;
	/* 邻居要在摘列（和 free(view)）之前找（见 focus_after_close）。 */
	struct vidow_view *next = was_focused ?
		focus_after_close(server, view, output) : NULL;
	if (was_focused) {
		server->focused = NULL;
	}
	view_leave_column(view);
	vidow_workspace_cleanup(server);

	free(view);

	if (was_focused) {
		if (next != NULL) {
			vidow_focus_view(next);
		} else {
			focus_nearest_column(server, output);
		}
	} else {
		vidow_arrange(server);
	}
}

static void x11_add_listener(struct wl_listener *listener,
		struct wl_signal *signal, void (*notify)(struct wl_listener *, void *)) {
	listener->notify = notify;
	wl_signal_add(signal, listener);
}

void vidow_view_new_x11(struct vidow_server *server,
		struct wlr_xwayland_surface *xsurface) {
	struct vidow_view *view = calloc(1, sizeof(*view));
	if (view == NULL) {
		return;
	}

	view->server = server;
	view->kind = VIDOW_VIEW_X11;
	view->x11 = xsurface;
	view->output = server->focused_output;
	if (view->output == NULL && !wl_list_empty(&server->outputs)) {
		struct vidow_output *o = wl_container_of(server->outputs.next, o, link);
		view->output = o->wlr_output;
	}
	view->tag.magic = VIDOW_VIEW_TAG_MAGIC;
	view->tag.view = view;
	view->anim_alpha = 1.0;
	view->anim_scale = 1.0;
	view->popup = x11_is_popup(xsurface);
	view->focus_on_open = true;
	view->focus_on_activate = true;
	view->honor_invalid_serial = false;
	view->home = home_of(server, view->output);
	view->id = ++server->next_view_id;
	wl_list_init(&view->ws_link);
	wl_list_init(&view->capture_sources);

	wl_list_insert(&server->views, &view->link);
	vidow_portal_view_new(view);

	x11_add_listener(&view->x11_associate, &xsurface->events.associate,
		x11_associate);
	x11_add_listener(&view->x11_dissociate, &xsurface->events.dissociate,
		x11_dissociate);
	x11_add_listener(&view->x11_set_title, &xsurface->events.set_title,
		x11_set_title);
	x11_add_listener(&view->x11_set_class, &xsurface->events.set_class,
		x11_set_class);
	x11_add_listener(&view->x11_set_parent, &xsurface->events.set_parent,
		x11_set_parent);
	x11_add_listener(&view->x11_set_geometry, &xsurface->events.set_geometry,
		x11_set_geometry);
	x11_add_listener(&view->x11_set_override_redirect,
		&xsurface->events.set_override_redirect, x11_set_override_redirect);
	x11_add_listener(&view->x11_set_hints, &xsurface->events.set_hints,
		x11_set_hints);
	x11_add_listener(&view->x11_set_window_type,
		&xsurface->events.set_window_type, x11_set_window_type);
	x11_add_listener(&view->x11_set_size_hints,
		&xsurface->events.set_size_hints, x11_set_size_hints);
	x11_add_listener(&view->x11_request_configure,
		&xsurface->events.request_configure, x11_request_configure);
	x11_add_listener(&view->x11_request_move, &xsurface->events.request_move,
		x11_request_move);
	x11_add_listener(&view->x11_request_resize, &xsurface->events.request_resize,
		x11_request_resize);
	x11_add_listener(&view->x11_request_activate,
		&xsurface->events.request_activate, x11_request_activate);
	x11_add_listener(&view->x11_request_close, &xsurface->events.request_close,
		x11_request_close);
	x11_add_listener(&view->x11_request_fullscreen,
		&xsurface->events.request_fullscreen, x11_request_fullscreen);
	x11_add_listener(&view->x11_request_maximize,
		&xsurface->events.request_maximize, x11_request_maximize);
	x11_add_listener(&view->x11_destroy, &xsurface->events.destroy,
		x11_destroy_view);

	/* 有些窗口送来 new_surface 时表面就已经在了（还没 associate 的就等信号）。 */
	x11_attach_surface(view);

	vidow_float_update_mode(view);

	wlr_log(WLR_INFO, "x11: 新窗口 %s（%s%s，%s%s）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
		vidow_view_app_id(view) != NULL ? vidow_view_app_id(view) : "?",
		xsurface->override_redirect ? "，override-redirect" : "",
		view->float_mode == VIDOW_FLOAT_NONE ? "平铺" : "浮动",
		view->popup ? "，弹出物（不装饰/不进布局/不做动画）" : "");
}

static void server_new_toplevel(struct wl_listener *listener, void *data) {
	struct vidow_server *server = wl_container_of(listener, server, new_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct vidow_view *view = calloc(1, sizeof(*view));
	if (view == NULL) {
		return;
	}

	view->server = server;
	view->kind = VIDOW_VIEW_XDG;
	view->xdg_toplevel = xdg_toplevel;
	view->output = server->focused_output;
	if (view->output == NULL && !wl_list_empty(&server->outputs)) {
		struct vidow_output *o = wl_container_of(server->outputs.next, o, link);
		view->output = o->wlr_output;
	}
	view->tag.magic = VIDOW_VIEW_TAG_MAGIC;
	view->tag.view = view;
	view->focus_on_open = true;
	view->focus_on_activate = true;
	view->honor_invalid_serial = false;
	view->home = home_of(server, view->output);
	view->id = ++server->next_view_id;
	wl_list_init(&view->ws_link);
	wl_list_init(&view->capture_sources);

	view->scene_tree =
		wlr_scene_xdg_surface_create(server->layer_toplevels, xdg_toplevel->base);
	if (view->scene_tree == NULL) {
		free(view);
		return;
	}
	view->scene_tree->node.data = &view->tag;
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	view->anim_alpha = 1.0;
	view->anim_scale = 1.0;

	/* wlr_scene_xdg_surface_create() returns a tree holding the surface. */
	view->scene_surface = find_scene_surface(&view->scene_tree->node,
		xdg_toplevel->base->surface);
	if (view->scene_surface == NULL) {
		wlr_log(WLR_ERROR, "no scene surface for toplevel %s",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?");
	}

	wl_list_insert(&server->views, &view->link);
	vidow_portal_view_new(view);

	view->map.notify = view_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &view->map);
	view->unmap.notify = view_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &view->unmap);
	view->commit.notify = view_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &view->commit);
	view->destroy.notify = view_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &view->destroy);
	view->set_title.notify = view_set_title;
	wl_signal_add(&xdg_toplevel->events.set_title, &view->set_title);
	view->set_parent.notify = view_set_parent;
	wl_signal_add(&xdg_toplevel->events.set_parent, &view->set_parent);
	view->request_fullscreen.notify = view_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &view->request_fullscreen);
	view->request_maximize.notify = view_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &view->request_maximize);
	view->request_move.notify = view_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &view->request_move);
	view->request_resize.notify = view_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &view->request_resize);
}

/* popup 的 unconstrain 只能等它自己第一次 commit。
 *
 * wlr_xdg_popup_unconstrain_from_box() 内部会 wlr_xdg_surface_schedule_configure()，
 * 而那第一句就是 assert(surface->initialized) —— 而 new_popup 是在客户端
 * get_popup 的那一刻发的，popup 自己的 surface 还没 commit 过，于是直接
 * SIGABRT（踩过两次：noctalia 状态栏的提示、GTK 菜单，任何 Wayland 弹出物都能
 * 打死合成器）。所以这里把这件事推到 popup 的第一次 commit 上做，那时候
 * surface 已经 initialized 了。 */
struct vidow_popup_unconstrain {
	struct wl_listener commit;
	struct wlr_xdg_popup *popup;
	struct wlr_box box;
};

static void popup_unconstrain_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_popup_unconstrain *u =
		wl_container_of(listener, u, commit);

	/* 第一次 commit 之后 initialized 才为真；万一这个信号发在角色 commit
	 * 之前，就等下一次 commit 再来（弹窗一般还会再 commit 一次）。 */
	if (!u->popup->base->initialized) {
		return;
	}
	wl_list_remove(&u->commit.link);
	/* 父面可能是 NULL（父面已销毁 / 不是 xdg surface），unconstrain 会沿
	 * parent 链解引用它 —— 没父面就不约束。 */
	if (u->popup->parent != NULL) {
		wlr_xdg_popup_unconstrain_from_box(u->popup, &u->box);
		wlr_log(WLR_DEBUG, "popup: unconstrain 完成（父面 %p，可用区 %d,%d %dx%d）",
			(void *)u->popup->parent, u->box.x, u->box.y,
			u->box.width, u->box.height);
	} else {
		wlr_log(WLR_DEBUG, "popup: 没有父面（%p），跳过 unconstrain",
			(void *)u->popup);
	}
	free(u);
}

void vidow_popup_unconstrain_on_commit(struct vidow_server *server,
		struct wlr_xdg_popup *popup, const struct wlr_box *box) {
	(void)server;
	struct vidow_popup_unconstrain *u = calloc(1, sizeof(*u));
	if (u == NULL) {
		return;
	}
	u->popup = popup;
	if (box != NULL) {
		u->box = *box;
	}
	u->commit.notify = popup_unconstrain_commit;
	wl_signal_add(&popup->base->surface->events.commit, &u->commit);
}

static void server_new_popup(struct wl_listener *listener, void *data) {
	struct vidow_server *server = wl_container_of(listener, server, new_popup);
	struct wlr_xdg_popup *popup = data;

	/* 挂在 layer surface 上的 popup（状态栏的提示 / 菜单）归 layer.c 的
	 * new_popup 管 —— 那边把节点挂在那块 layer 自己的场景树里，这样弹出物才
	 * 跟着状态栏一起动。这里再建一份就是同一个 surface 两个场景节点：屏幕
	 * 上多出一层重影，而且第一个节点没人负责销毁。 */
	if (popup->parent != NULL &&
			wlr_layer_surface_v1_try_from_wlr_surface(popup->parent) != NULL) {
		return;
	}

	struct vidow_view *view = view_from_surface(server, popup->parent);
	struct wlr_scene_tree *parent = view != NULL ? view->scene_tree :
		server->layer_toplevels;

	wlr_scene_xdg_surface_create(parent, popup->base);

	/* wlroots 0.20 不保证 popup->parent 非空（父面已经销毁、或者父面不是
	 * xdg surface 时它就是 NULL），而 unconstrain 要沿 parent 链往上算
	 * toplevel 坐标 —— wlroots 那边第一句就是解引用，没有空指针检查，
	 * 于是 SIGSEGV（noctalia 一开，状态栏弹出的提示就把合成器打死在这）。
	 * 没有父面就不约束：坐标由客户端自己的 positioner 算，最多贴边时越界
	 * 一点，不会崩。 */
	if (popup->parent == NULL) {
		wlr_log(WLR_DEBUG, "popup: 没有父面（%p），跳过 unconstrain",
			(void *)popup);
		return;
	}

	struct wlr_box box = {0};
	if (view != NULL && view->output != NULL) {
		wlr_output_layout_get_box(server->output_layout, view->output, &box);
	}
	vidow_popup_unconstrain_on_commit(server, popup, &box);
}

bool vidow_view_init(struct vidow_server *server) {
	server->new_toplevel.notify = server_new_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_toplevel);
	server->new_popup.notify = server_new_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_popup);
	return true;
}
