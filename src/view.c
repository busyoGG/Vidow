#include "server.h"
#include "ipc.h"

#include <math.h>
#include <stdlib.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/util/log.h>
#include <xcb/xcb.h>

/* gap 的算法都在 server.h 里（vidow_gap_side / vidow_gap_inner）：统一值 gaps
 * 管全部，单边设了就用单边的，-1 = 跟随。 */

/* 新窗口该落到哪块屏。
 *
 * `server->focused_output` 在「还没人动过鼠标」时是 NULL —— sddm 直接起 vidow 就是
 * 这种情况（用户报的：自启动的应用全是空框）。那时新窗口拿到 output = NULL，
 * 永远不进 arrange、永远不发 configure、永远不交 buffer，还会作为死窗口留在场景里
 * 拖慢每一帧。回落：用第一块输出。
 */
static struct wlr_output *view_default_output(struct vidow_server *server) {
	/* ① 鼠标在哪块屏，新窗口就落哪块屏（用户要的：跟随鼠标）。
	 * 这是唯一「用户现在正看着哪块屏」的真实信号 —— 焦点窗口可能在另一块屏上，
	 * 但用户的眼睛跟着鼠标。 */
	if (server->cursor != NULL) {
		struct wlr_output *under = wlr_output_layout_output_at(
			server->output_layout, server->cursor->x, server->cursor->y);
		if (under != NULL) {
			return under;
		}
	}
	/* ② 鼠标不在任何输出上（刚启动 / 布局外）：用焦点屏。 */
	if (server->focused_output != NULL) {
		return server->focused_output;
	}
	/* ③ 连焦点都没有（sddm 直接起 vidow，还没人动过鼠标）：主屏 → 第一块屏。 */
	if (server->config.primary_output != NULL) {
		struct vidow_output *o;
		wl_list_for_each(o, &server->outputs, link) {
			if (o->wlr_output->name[0] != '\0' &&
					strcmp(o->wlr_output->name,
						server->config.primary_output) == 0) {
				return o->wlr_output;
			}
		}
	}
	struct vidow_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		return o->wlr_output;
	}
	return NULL;
}

void vidow_place_view(struct vidow_view *view, int x, int y, int w, int h);
/* 小窗（S5）：图标态的快照节点（定义在文件后半段的预览那一节里）。 */
static void mini_snapshot_place(struct vidow_view *view);
/* 小窗（S3）：窗口的原生尺寸（场景坐标）。 */
static void view_mini_native(const struct vidow_view *view, int *w, int *h);
static void view_ensure_column(struct vidow_server *server,
		struct vidow_view *view);
void vidow_view_leave_column(struct vidow_view *view);
/* X11：按窗口的绝对坐标把它归到某块输出上（弹出物可以开在另一块屏上）。 */
static void x11_track_output(struct vidow_view *view);
/* 这块屏的归属记忆（定义在文件后半段的热插拔那一节）。 */
static struct vidow_output_memory *home_of(struct vidow_server *server,
		struct wlr_output *wlr_output);

/* 把一个窗口放回平铺布局（浮动 → 平铺那次重判走这里，见 float.c 的
 * vidow_float_set_mode）。
 *
 * **没上屏的窗口不给列**：列是条带上的一格，占着它就是占着屏上的宽度 ——
 * 一个没 map 的窗口进了列，屏上什么都不画、那格却照占，就是用户报的「凭空
 * 多出一列」。proton / Steam 那批辅助窗（现场 dump 里标题 `Input` 的窗口 50）
 * 就是这么来的：X11 窗口**从来没 map 过**，可它的属性（WM_TRANSIENT_FOR /
 * WM_NORMAL_HINTS / `_NET_WM_WINDOW_TYPE`）在运行中被改掉，vidow 于是把它从
 * 浮动重判成平铺 —— 这一步以前不看它上没上屏，直接建了一条列（几何 2883,6
 * 951x1068，正卡在 lutris 和游戏之间）。
 *
 * 延迟到首次 map：view_map() 里 mapped 先置 true，随后自己会走
 * view_ensure_column() 补上这条列，所以上屏的窗口一个都不少。
 *
 * 注意别把这条判据搬到 view_ensure_column() 里：xdg 的初始 commit 那一趟
 * （view_commit 的 initial_commit 分支）**故意**在 map 之前建列 —— 客户端要拿
 * 着「平铺格的尺寸」这个 configure 才会上屏，拦掉它窗口就会先按自己的尺寸冒
 * 出来再跳一下（而且抢跑的最大化请求也兑现在那儿）。 */
void vidow_view_join_layout(struct vidow_server *server,
		struct vidow_view *view) {
	if (!view->mapped) {
		wlr_log(WLR_DEBUG, "join layout: %s 还没上屏，先不给列（等 map）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)");
		return;
	}
	view_ensure_column(server, view);
}

void vidow_view_leave_layout(struct vidow_view *view) {
	vidow_view_leave_column(view);
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

/* 窗口规则的 initial_* 快照：上屏那一刻的标题 / app_id / class / instance。
 * 只填空着的 —— X11 的 WM_CLASS / WM_NAME 有时比 map 还晚到，那时候补上；填过
 * 的就不再动（客户端之后改标题不算「刚开的样子」，那正是 initial_* 的用处）。 */
void vidow_view_note_initial_names(struct vidow_view *view) {
	const char *title = vidow_view_title(view);
	const char *app_id = vidow_view_app_id(view);
	const bool x11 = view->kind == VIDOW_VIEW_X11 && view->x11 != NULL;
	const char *klass = x11 ? view->x11->class : NULL;
	const char *instance = x11 ? view->x11->instance : NULL;
	if (view->init_title[0] == '\0' && title != NULL) {
		snprintf(view->init_title, sizeof(view->init_title), "%s", title);
	}
	if (view->init_app_id[0] == '\0' && app_id != NULL) {
		snprintf(view->init_app_id, sizeof(view->init_app_id), "%s", app_id);
	}
	if (view->init_class[0] == '\0' && klass != NULL) {
		snprintf(view->init_class, sizeof(view->init_class), "%s", klass);
	}
	if (view->init_instance[0] == '\0' && instance != NULL) {
		snprintf(view->init_instance, sizeof(view->init_instance), "%s",
			instance);
	}
}

/* X 像素 <-> 逻辑坐标（定义见下面 vidow_view_x11_scale_int 之后）。 */

struct wlr_box vidow_view_geometry(const struct vidow_view *view) {
	if (view->kind == VIDOW_VIEW_X11) {
		/* X11 窗口没有「窗口几何」和「表面」的区别：整块就是内容。 */
		struct wlr_box box = {0};
		if (view->x11 != NULL) {
			double w = view->x11->width, h = view->x11->height;
			vidow_xwm_from_native_size(view->server, view->output, w, h, &w, &h);
			box.width = (int)lround(w);
			box.height = (int)lround(h);
		}
		return box;
	}
	if (view->xdg_toplevel == NULL) {
		return (struct wlr_box){0};
	}
	return view->xdg_toplevel->base->geometry;
}

/* 可见窗口在源贴图里的归一化位置（见 struct vidow_fx_geo 的 tex_* 说明）。
 *
 * 只用逻辑尺寸算是不够的：贴图是**客户端缓冲像素**，客户端用 wp_viewport 声明
 * 「缓冲里哪一块是有效内容」时（chromium / KDE 在分数缩放下会把缓冲开得比窗口大
 * 一圈，只画其中一块、src 框指过去），按逻辑尺寸算出来的是整块缓冲 —— 右边/下边
 * 那圈没画过的像素跟着铺进窗口，屏幕上就是「框到位了、画面小一圈、周围一条黑边」，
 * 而且会一直那样（不是中间态）。场景节点的 src_box 就是 wlroots 画这个表面时用的
 * 源区域（缓冲像素，客户端没设 viewport 时就是整块缓冲），照它换算。 */
static void fx_geo_tex_map(struct vidow_fx_geo *geo,
		const struct wlr_scene_buffer *sb, const struct wlr_surface *surface,
		const struct vidow_view *view) {
	if (sb == NULL || surface == NULL) {
		return;
	}
	/* 有效源区域（缓冲像素）：客户端声明的 viewport src 框，没设就是整块缓冲。
	 *
	 * 裁着的时候**不能**读节点那个字段：越界裁剪（wlroots 的 set_clip）改的就是
	 * 它，裁完剩下的那一小条会变成整个 src_box —— 照它换算就会把那一小条拉伸
	 * 铺满整块窗口（用户报的「表面横向拉伸变扁 + 黑边随在视口里的范围大小」）。
	 * 那一种是 view->fx.content_box 里抄下来的**没裁过**的那一版（见
	 * view_snapshot_content_box）。没裁的时候节点字段就是它本身，直接读。 */
	struct wlr_fbox box = sb->src_box;
	if (view != NULL && view->fx.clip_local.width > 0 &&
			view->fx.content_box.width > 0.0 &&
			view->fx.content_box.height > 0.0) {
		box = view->fx.content_box;
	}
	int bw = surface->current.buffer_width;
	int bh = surface->current.buffer_height;
	int sw = surface->current.width;
	int sh = surface->current.height;
	if (bw <= 0 || bh <= 0) {
		/* 场景节点手里的那块缓冲才是权威（换了缓冲、buffer_width 还没跟上）。 */
		bw = sb->buffer != NULL ? (int)sb->buffer->width : 0;
		bh = sb->buffer != NULL ? (int)sb->buffer->height : 0;
	}
	if (box.width <= 0.0 || box.height <= 0.0) {
		box = (struct wlr_fbox){0, 0, (double)bw, (double)bh};
	}
	if (bw <= 0 || bh <= 0 || sw <= 0 || sh <= 0 ||
			box.width <= 0.0 || box.height <= 0.0) {
		return; /* 算不了：让调用方按老的「整块贴图」走 */
	}
	/* 一个逻辑单位等于多少缓冲像素（viewporter 的 src 框 / 表面的逻辑尺寸）。 */
	double sx = box.width / (double)sw;
	double sy = box.height / (double)sh;
	double x0 = box.x + geo->off_x * sx;
	double y0 = box.y + geo->off_y * sy;
	double w0 = geo->w * sx;
	double h0 = geo->h * sy;
	/* 声明的窗口比手里这块有效内容还大（客户端还没按新尺寸重画完）：把能用的
	 * 那块铺满窗口，别去采边界外面的像素。 */
	if (x0 < box.x) {
		w0 -= box.x - x0;
		x0 = box.x;
	}
	if (y0 < box.y) {
		h0 -= box.y - y0;
		y0 = box.y;
	}
	if (x0 + w0 > box.x + box.width) {
		w0 = box.x + box.width - x0;
	}
	if (y0 + h0 > box.y + box.height) {
		h0 = box.y + box.height - y0;
	}
	if (w0 <= 0.0) {
		w0 = box.width;
		x0 = box.x;
	}
	if (h0 <= 0.0) {
		h0 = box.height;
		y0 = box.y;
	}
	geo->tex_u0 = (float)(x0 / (double)bw);
	geo->tex_v0 = (float)(y0 / (double)bh);
	geo->tex_du = (float)(w0 / (double)bw);
	geo->tex_dv = (float)(h0 / (double)bh);
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
	struct wlr_scene_buffer *sb = view->scene_surface != NULL ?
		view->scene_surface->buffer : NULL;
	int sw = surface != NULL ? surface->current.width : 0;
	int sh = surface != NULL ? surface->current.height : 0;
	if (g.width <= 0 || g.height <= 0) {
		/* 客户端没声明窗口几何：wlroots 会给「整块表面」这个默认值，但万一
		 * 拿到个空的，就当整块表面是窗口（老行为）。 */
		geo.w = sw;
		geo.h = sh;
		geo.src_w = sw;
		geo.src_h = sh;
		fx_geo_tex_map(&geo, sb, surface, view);
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
	/* 声明框落在表面外面：真的会发生 —— 客户端改大小的一两秒里（chromium 系
	 * 实测 1–2 秒）表面已经缩了，声明的窗口几何还是原来那个大尺寸（wlroots 只在
	 * **声明那一刻**把几何夹进当时的表面，之后表面缩小它不管）。
	 * 这时候按声明框采内容，UV 会跑到 1 外面被 clamp 成「窗口框到位了、里面的
	 * 画面小一圈、右边一条糊边」（用户报的「先渲染小一点然后才按照缩放显示」）。
	 * 那就当整块表面就是窗口内容：内容铺满这块框（等它把新尺寸画上来就恢复
	 * 1:1 了）。 */
	if (g.x < 0 || g.y < 0 || g.x + g.width > sw || g.y + g.height > sh) {
		wlr_log(WLR_DEBUG,
			"窗口几何落在表面外面（声明 %dx%d@%d,%d，表面 %dx%d）：按整块表面贴",
			g.width, g.height, g.x, g.y, sw, sh);
		geo.off_x = 0;
		geo.off_y = 0;
		geo.src_w = g.width;
		geo.src_h = g.height;
	}
	fx_geo_tex_map(&geo, sb, surface, view);
	return geo;
}

/* Preferred buffer-scale hint. Geometry uses the independent X virtual
 * output mapping, never this DPI-related integer. */
int vidow_view_x11_scale_int(const struct vidow_view *view) {
	double scale = view->x11_scale;
	if (!(scale > 0.0)) {
		scale = view->server->xwayland_scale;
	}
	int n = (int)lround(scale);
	return n > 0 ? n : 1;
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
			double w = view->x11->width, h = view->x11->height;
			vidow_xwm_from_native_size(view->server, view->output, w, h, &w, &h);
			width = (int)lround(w);
			height = (int)lround(h);
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
	/* 小窗 + 保留客户端原生分辨率（S3，默认开）：**一个尺寸 configure 都不发**
	 * —— 客户端 buffer 保持原生，由合成器把场景节点缩到小窗盒显示（见
	 * view_mini_scale）。挡在唯一的出口上：进小窗、arrange 摆位、拖拽改盒子、
	 * 迟到的 commit 全都走这儿，挡一处就够。退出小窗（mini == NONE）之后照常
	 * configure —— 还原成列 / 浮动窗口那边要按布局重新定尺寸。
	 *
	 * **只管迷你 / 图标两个形态**：那两态是「缩略图」语义（只缩不重排，保留原生
	 * buffer 才便宜）。自由小窗（FREE）是**正常窗户**语义 —— 盒子多大就命令客户端
	 * 多大多，它跟着重新排版（用户报的「自由小窗改大小被限制了比例」：以前 FREE
	 * 也吃这条，改盒子只改留白、客户端一次 configure 都不发，内容比例一动不动，
	 * 看起来就像被锁了比例）。 */
	if (view->mini == VIDOW_MINI_MINI || view->mini == VIDOW_MINI_ICON) {
		if (vidow_mini_preserve_buffer(view)) {
			return;
		}
	}
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
		/* 逻辑 -> X 设备像素的换算在 vidow_xwm_configure 里做完，
		 * 这里只给逻辑坐标（和 KDE 的 toXNative 同一条路）。 */
		int client_x = view->geom.x, client_y = view->geom.y;
		/* Workspace/column slides belong to the compositor scene, not X11.
		 * Moving a fullscreen game outside its RandR monitor makes Wine treat
		 * it as a monitor/fullscreen transition. Keep its X geometry anchored
		 * to the owning output while the scene continues sliding normally. */
		if (view->fullscreen && view->output != NULL) {
			struct wlr_box full = {0};
			wlr_output_layout_get_box(view->server->output_layout,
				view->output, &full);
			if (full.width > 0 && full.height > 0) {
				client_x = full.x;
				client_y = full.y;
				w = full.width;
				h = full.height;
			}
		}
		double nw = w, nh = h, nx = client_x, ny = client_y;
		vidow_xwm_to_native_size(view->server, view->output, w, h, &nw, &nh);
		vidow_xwm_to_native_point(view->server, view->output,
			client_x, client_y, &nx, &ny);
		int cw = (int)lround(nw), ch = (int)lround(nh);
		if (cw < 1) cw = 1;
		if (ch < 1) ch = 1;
		int x = (int)lround(nx), y = (int)lround(ny);
		if (view->cfg_sent && view->cfg_x == x && view->cfg_y == y &&
				view->cfg_w == cw && view->cfg_h == ch) {
			return; /* 位置尺寸一个都没变：别让客户端白重画一帧 */
		}
		wlr_xwayland_surface_configure(view->x11, (int16_t)x, (int16_t)y,
			(uint16_t)cw, (uint16_t)ch);
		view->cfg_x = x;
		view->cfg_y = y;
		view->cfg_w = cw;
		view->cfg_h = ch;
		view->cfg_sent = true;
		return;
	}

	if (view->cfg_sent && view->cfg_w == w && view->cfg_h == h) {
		return; /* 尺寸没变：别让客户端白重画一帧（拖动时每步都会走到这儿） */
	}
	wlr_xdg_toplevel_set_size(view->xdg_toplevel, w, h);
	view->cfg_w = w;
	view->cfg_h = h;
	view->cfg_sent = true;
}

void vidow_view_set_tiled(struct vidow_view *view, bool tiled) {
	if (view->kind != VIDOW_VIEW_XDG || view->xdg_toplevel == NULL) {
		return;
	}
	if (!vidow_view_configure_ready(view)) {
		return;
	}
	int edges = tiled ?
		(WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT) : 0;
	/* 状态没变就别再排一个 configure：place_view 每次都会调到这儿，而拖动时每动
	 * 一下都会 arrange 一次 —— 不去重的话客户端每步都收到一个 configure、重画一
	 * 帧、交一帧（实测：拖 200 步 = 每个窗口 200 次 configure + 200 次 commit），
	 * 于是「世界」每帧都在变，所有私有底衬跟着每帧重抓。 */
	if (view->cfg_tiled_sent && view->cfg_tiled == edges) {
		return;
	}
	wlr_xdg_toplevel_set_tiled(view->xdg_toplevel, edges);
	view->cfg_tiled = edges;
	view->cfg_tiled_sent = true;
}

void vidow_view_set_activated(struct vidow_view *view, bool activated) {
	if (!vidow_view_configure_ready(view)) {
		return;
	}
	/* 窗口规则 fake_focus：客户端看到的一直是 activated，焦点移走时那条
	 * deactivated 在这里被吃掉。键盘焦点 / server->focused / 任务栏不受影响
	 * —— 伪装只到客户端这一层。 */
	if (view->fake_focus) {
		activated = true;
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

/* 改全屏状态：设标志 → 告诉客户端 → 重排 → 发 IPC 事件 → 记一行 INFO。
 *
 * **全屏状态只有这一个入口**。为什么专门记日志：层级是「平铺 < 浮动 < 全屏」
 * （见 vidow_restack），全屏的窗口永远压在所有窗口上面 —— 用户报「这个窗口一直
 * 叠在最上面，不管平铺还是浮动」时，第一件要看的就是它是不是全屏。这条状态以前
 * 只有 IPC 事件、日志里一个字都没有，run.sh 的日志根本看不出来。 */
static void view_apply_fullscreen(struct vidow_view *view, bool want,
		const char *why) {
	if (view == NULL || view->fullscreen == want) {
		return;
	}
	view->fullscreen = want;
	vidow_view_set_fullscreen(view, want);
	/* 全屏**不抢焦点**（Hyprland 的规矩：全屏请求不改变焦点；要抢焦点的客户端
	 * 自己发 _NET_ACTIVE_WINDOW / xdg_activation，那条路照旧）。
	 *
	 * 上一版这里是「全屏的窗口立刻拿焦点」，为了修「没焦点的窗口自己进全屏会被摆
	 * 在半屏外」。结果更糟：游戏会**反复**在关/开之间切全屏（用户日志里
	 * 04:24.600 关 → 04:24.650 开，04:27 / 04:31 各一轮），每开一次就把焦点从用户
	 * 手里抢回去一次 —— 用户报的「全屏窗口一直 fullscreen 抢焦点」。 */
	vidow_arrange(view->server);
	vidow_ipc_emit_window(view->server, "window-fullscreen-changed", view);
	wlr_log(WLR_INFO, "全屏: %s %s（%s）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		want ? "开" : "关", why);
}

/* X11 的 _NET_WM_STATE_FULLSCREEN 有两条路：
 *   ① 客户端发 ClientMessage → wlroots 发 request_fullscreen 信号 → 我们的
 *      x11_request_fullscreen 处理（这条一直好着）；
 *   ② 客户端直接改**属性**（wine / Proton 的游戏就是这条，XChangeProperty 或
 *      map 之前就把状态挂上）→ wlroots 只把状态写进 xsurface->fullscreen，
 *      **一个字都不发**（xwm.c 的 read_surface_net_wm_state 只赋字段）。
 * ② 这条以前完全没人接：游戏被当普通平铺窗口摆成半屏一列（带装饰、停在半块屏
 * 上）—— 用户报的「启动 proton 游戏时半屏空位、窗口不是全画幅而是半屏大小、
 * 一直停在右半屏」就是这个。这里对一下两边（就是个指针比较），交帧和 map 的
 * 时候各来一次：游戏一直在交帧，map 那次能赶上「状态比表面先到」的时序。 */
static void view_sync_x11_fullscreen(struct vidow_view *view) {
	if (view == NULL || view->kind != VIDOW_VIEW_X11 || view->x11 == NULL) {
		return;
	}
	if (view->x11->fullscreen != view->fullscreen) {
		view_apply_fullscreen(view, view->x11->fullscreen,
			"X11 的 _NET_WM_STATE 属性变了");
	}
}

/* 每帧开头对一遍 X11 的 _NET_WM_STATE。
 *
 * 只靠 view_commit 那条路不够：Proton / wine 的游戏退出全屏时可能只是改 X
 * 属性、之后不再交新帧；没有新的 commit，view->fullscreen 就一直是 true，
 * 状态栏（top + exclusive_zone）被 vidow_layers_update_fullscreen 永久关着 ——
 * 游戏退出了，bar 却再也回不来。 */
void vidow_views_sync_x11_fullscreen(struct vidow_server *server) {
	if (server == NULL) {
		return;
	}
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		view_sync_x11_fullscreen(view);
	}
}

/* 退出全屏。最小化 / 变浮窗这类「整屏状态跟它冲突」的操作要先走这一步：全屏那条
 * 分支在布局里不看占位、在层级里永远在最上面，留着它那些操作都白做。 */
void vidow_view_unfullscreen(struct vidow_view *view, const char *why) {
	view_apply_fullscreen(view, false, why);
}

void vidow_view_set_maximized(struct vidow_view *view, bool maximized) {
	if (!vidow_view_configure_ready(view)) {
		return;
	}
	/* 状态没变就别再排一个 configure：列最大化的时候列里每个窗口都要收到应答，
	 * 而 arrange / map 会反复走到这儿（同 set_tiled 的去重，见 server.h）。 */
	if (view->cfg_maximized_sent && view->cfg_maximized == maximized) {
		return;
	}
	view->cfg_maximized = maximized;
	view->cfg_maximized_sent = true;
	if (view->kind == VIDOW_VIEW_X11) {
		/* X11 的「最大化」是 _NET_WM_STATE_MAXIMIZED_HORZ / VERT 两个状态位。
		 * 列最大化是整列撑满宽度，横竖一起给（和别的 WM 一样）。 */
		wlr_xwayland_surface_set_maximized(view->x11, maximized, maximized);
		return;
	}
	wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, maximized);
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

/* 场景单位 -> surface 局部坐标的倍数。**这是「往客户端发坐标」的唯一口子**
 * （vidow_view_at 在返回前乘它，指针 motion / button / view-at / overview 点击
 * 全走那儿）—— 场景里节点占的位置和表面自己的坐标空间不一样大时，只有这里知道
 * 差多少。
 *
 * 两个来源：
 *   X11     缓冲区被我们按 xwayland_scale 倍声明过，场景里占的位置比客户端
 *           自己的坐标空间小；
 *   小窗     保留原生分辨率时（S3），场景节点被缩到小窗盒，表面还是原生的。
 * 两者是同一个式子：表面宽 / 节点宽。尺寸一致时返回 1.0（这条保证非小窗 /
 * 非缩放路径逐字节不变）。 */
/* Locate the actual surface node, not the toplevel tree: decorations,
 * subsurfaces and popups can all have different origins. Do not clip a grab. */
static struct wlr_scene_buffer *view_find_surface_buffer(
		struct wlr_scene_node *node, struct wlr_surface *surface) {
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(buffer);
		return ss != NULL && ss->surface == surface ? buffer : NULL;
	}
	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wl_container_of(node, tree, node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			struct wlr_scene_buffer *found = view_find_surface_buffer(child, surface);
			if (found != NULL) return found;
		}
	}
	return NULL;
}

bool vidow_view_surface_coords(struct vidow_view *view,
		struct wlr_surface *surface, double lx, double ly, double *sx, double *sy) {
	if (view == NULL || view->scene_tree == NULL || surface == NULL) return false;
	struct wlr_scene_buffer *buffer = view_find_surface_buffer(
		&view->scene_tree->node, surface);
	if (buffer == NULL) return false;
	int x = 0, y = 0;
	/* coords are populated even when a scene ancestor is disabled. */
	wlr_scene_node_coords(&buffer->node, &x, &y);
	double dw = buffer->dst_width > 0 ? buffer->dst_width : surface->current.width;
	double dh = buffer->dst_height > 0 ? buffer->dst_height : surface->current.height;
	if (dw <= 0 || dh <= 0) return false;
	*sx = (lx - x) * surface->current.width / dw;
	*sy = (ly - y) * surface->current.height / dh;
	return true;
}

/* Legacy single-axis query for callers which only need an X length ratio.
 * Pointer motion uses vidow_view_surface_coords with independent axes. */
double vidow_view_hit_scale(const struct vidow_view *view) {
	if (view == NULL || view->scene_surface == NULL) return 1.0;
	struct wlr_scene_buffer *node = view->scene_surface->buffer;
	struct wlr_surface *surface = vidow_view_surface(view);
	if (node == NULL || node->dst_width <= 0 || surface == NULL) return 1.0;
	return (double)surface->current.width / node->dst_width;
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
	/* X buffer pixels are mapped through the output's virtual rectangle.
	 * DPI and wl_surface.buffer_scale do not define this geometry transform. */
	int scale = vidow_view_x11_scale_int(view); /* preferred buffer-scale hint only */
	double dw = bw, dh = bh;
	vidow_xwm_from_native_size(view->server, view->output, bw, bh, &dw, &dh);
	int dst_w = (int)lround(dw), dst_h = (int)lround(dh);
	if (dst_w < 1) {
		dst_w = 1;
	}
	if (dst_h < 1) {
		dst_h = 1;
	}
	struct wlr_scene_buffer *node = view->scene_surface->buffer;
	bool changed = node->dst_width != dst_w || node->dst_height != dst_h;
	wlr_scene_buffer_set_dest_size(node, dst_w, dst_h);
	/* This is a preferred-scale hint only, not a promise that Xwayland will
	 * change buffer_scale. Display geometry and input use actual committed
	 * dimensions above; never rely on this hint to repair coordinate spaces. */
	if (surface->current.scale != scale || view->logged_scale != scale) {
		wlr_surface_set_preferred_buffer_scale(surface, scale);
	}
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
	/* 每帧都会走到这里（每个 commit）：只在真的变了的时候说话。
	 * 「变了」= 场景尺寸变了 / 放大倍率变了 / 缓冲尺寸变了 —— 三者都记着，
	 * 只比 scale 的话，客户端每帧换缓冲（CEF 一直这样）就又变成刷屏源。 */
	if (changed || view->logged_scale != scale || view->logged_buf_w != bw) {
		view->logged_scale = scale;
		view->logged_buf_w = bw;
		/* 「表面 N 倍」= Xwayland 在 wl_surface 上声明的 buffer scale
		 * （vidow 写了 _XWAYLAND_GLOBAL_OUTPUT_SCALE 之后它就是那个全局缩放）。
		 * 它决定了 surface 自己的逻辑坐标系，输入坐标换算按它走 —— 和 vidow
		 * 这边「按 N 倍声明」是两回事，排查点击 / 尺寸问题时两个都要看。 */
		/* DEBUG 而不是 INFO：CEF / 游戏每帧换缓冲，这段判据虽然去重过，
		 * 在新缓冲尺寸上下浮动时还是会一行行冒出来，不该出现在日常日志里。
		 * 排查 X11 缩放 / 点击坐标时开 VIDOW_LOG=debug 就看得到。 */
		wlr_log(WLR_DEBUG,
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
	/* 小窗态（S3）也照 overview 那条办：现在节点上是**缩过的**尺寸，记进基线
	 * 就等于把缩放叠进去 —— 退出小窗之后 base_dst_* 永久是错的（overview 里
	 * 那个窗口会越缩越小）。退出小窗态时 view_anim_place 会把节点还原成原生尺寸
	 * 并在这里重记一次。 */
	if (view->server->overview.t > 0.0 || view->mini != VIDOW_MINI_NONE ||
			view->scene_surface == NULL) {
		return;
	}
	struct wlr_scene_buffer *src = view->scene_surface->buffer;
	if (src == NULL) {
		return;
	}
	int w = src->dst_width;
	int h = src->dst_height;
	if (w <= 0 || h <= 0) {
		/* 节点的 dst 是 0（退出小窗那一刻会把它清成 0，wlroots 要等这个表面
		 * 下一次 commit 才按表面尺寸重设）：这个字段量的是**场景单位**里的
		 * 1:1 尺寸，所以退回「表面自己的逻辑尺寸」—— 不是缓冲像素数：输出缩放
		 * 2 的客户端（buffer scale 2）缓冲宽高是逻辑的两倍，照缓冲像素记下来
		 * 基线就大了一倍，之后每次进小窗算出的缩放系数都小一半（实测：盒里
		 * 的内容只画成该有的 1/2 —— 172x190 的物理脚印变成 86x95）。 */
		struct wlr_surface *surface = vidow_view_surface(view);
		if (surface != NULL && surface->current.width > 0 &&
				surface->current.height > 0) {
			w = surface->current.width;
			h = surface->current.height;
		} else if (src->buffer != NULL) {
			/* 连表面都没有（理论上到不了）：退回缓冲像素。 */
			w = (int)src->buffer->width;
			h = (int)src->buffer->height;
		}
	}
	if (w > 0 && h > 0) {
		view->base_dst_w = w;
		view->base_dst_h = h;
	}
}

/* 小窗（S5）：**图标态**那块矩形上的命中，单独判一次，不走场景图。两个理由：
 *   - 走快照那条路时场景图里根本命不中它（真实节点停在停车位上，屏上画的是快照
 *     节点，而快照节点**故意不带 view tag** —— 带了会被 vidow_restack 当成窗口
 *     节点参与层级排序，一个窗口两个节点）；
 *   - 图标是**贴边 + 缩放 + 裁到自己那块屏**的一小块，场景图那条路（表面节点 +
 *     裁剪）本来就容易差几像素。小窗的输入是合成器自己吃的（mini_swallows_input），
 *     坐标只用来定形态，不需要像素级精确。
 * 盒子取 geom + anim_off（和摆位同源）—— 贴边时有一半在屏外，照收。 */
static struct vidow_view *mini_icon_view_at(struct vidow_server *server,
		double lx, double ly, double *sx, double *sy) {
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->mini != VIDOW_MINI_ICON ||
				!view->mapped || view->output == NULL ||
				!vidow_workspace_view_visible(server, view)) {
			continue;
		}
		int x = view->geom.x + (int)lround(view->anim_off_x);
		int y = view->geom.y + (int)lround(view->anim_off_y);
		if (lx < x || lx >= x + view->geom.width ||
				ly < y || ly >= y + view->geom.height) {
			continue;
		}
		/* 本地坐标：图标是缩放的，按缩放比折回去（小窗吞输入，这两个数只用来
		 * 定形态，不精确也不影响判断 —— 但别给错得离谱的）。 */
		int nw = 0, nh = 0;
		view_mini_native(view, &nw, &nh);
		double k = (nw > 0 && view->geom.width > 0) ?
			(double)nw / (double)view->geom.width : 1.0;
		if (sx != NULL) {
			*sx = (lx - x) * k;
		}
		if (sy != NULL) {
			*sy = (ly - y) * k;
		}
		return view;
	}
	return NULL;
}

/* 小窗（S3）：**迷你态**的命中按整个**盒子**算，不走场景图。理由：内容是按比例
 * contain 摆在盒子中间的（665x740 的内容装进 534x299 的盒子 → 左右各留 130 像素
 * 的留白），而命中测试只认内容节点 —— 那两块留白按下去不算窗口，拖不起来。用户
 * 报的「小窗左右有死区、拖不动、窗口卡在范围内」就是这条。
 * 盒子取 float_geom + anim_off（和摆位、和拖拽夹取同源）。 */
static struct vidow_view *mini_box_view_at(struct vidow_server *server,
		double lx, double ly, double *sx, double *sy) {
	/* 全景里不算这一笔：那时小窗画在**全景网格**里，float_geom 还是全景之前的
	 * 盒子 —— 照它判会让屏上别处的格子被这个窗口吞掉（test-overview 的「格子
	 * 右上内缩 4px 命中它自己」就是这么挂的）。全景自己的命中走场景图。 */
	if (vidow_overview_active(server)) {
		return NULL;
	}
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		/* 迷你**和自由**小窗都整盒命中：内容被 contain 缩放过，盒里左右（或
		 * 上下）留着壁纸色的留白 —— 只靠场景图命中的话，点在留白上什么也不
		 * 发生（用户报的「还是左右有死区」）。图标态不在这儿：它有自己的命中
		 * 盒（mini_icon_view_at，贴边那一块）。 */
		if ((view->mini != VIDOW_MINI_MINI &&
				view->mini != VIDOW_MINI_FREE) ||
				!view->mapped || view->output == NULL ||
				!vidow_workspace_view_visible(server, view) ||
				view->float_geom.width <= 0 || view->float_geom.height <= 0) {
			continue;
		}
		double x = view->float_geom.x + view->anim_off_x;
		double y = view->float_geom.y + view->anim_off_y;
		if (lx < x || lx >= x + view->float_geom.width ||
				ly < y || ly >= y + view->float_geom.height) {
			continue;
		}
		/* 局部坐标：从**内容节点**在场景里的原点量，再乘回表面坐标（内容被缩
		 * 过：dst ≠ surface 尺寸，见 vidow_view_hit_scale）。落在留白里会得到
		 * 负数 / 超出表面的值，夹进表面里 —— 客户端收到越界坐标没意义。 */
		if (sx != NULL && sy != NULL && view->scene_surface != NULL) {
			int nx = 0, ny = 0;
			if (wlr_scene_node_coords(&view->scene_surface->buffer->node,
					&nx, &ny)) {
				double k = vidow_view_hit_scale(view);
				struct wlr_surface *surf = vidow_view_surface(view);
				*sx = (lx - nx) * k;
				*sy = (ly - ny) * k;
				if (surf != NULL) {
					if (*sx < 0) {
						*sx = 0;
					}
					if (*sy < 0) {
						*sy = 0;
					}
					if (*sx > surf->current.width) {
						*sx = surf->current.width;
					}
					if (*sy > surf->current.height) {
						*sy = surf->current.height;
					}
				}
			}
		}
		return view;
	}
	return NULL;
}

/* 定义在下面（裁剪那段）；命中测试要用它挡掉「裁掉了但还能点到」的窗口。 */
static bool view_clip_box(struct vidow_view *view, struct wlr_box *out);

struct vidow_view *vidow_view_at(struct vidow_server *server,
		double lx, double ly, struct wlr_surface **surface,
		double *sx, double *sy) {
	struct vidow_view *snap = mini_icon_view_at(server, lx, ly, sx, sy);
	if (snap != NULL) {
		if (surface != NULL) {
			*surface = vidow_view_surface(snap);
		}
		return snap;
	}
	/* 迷你小窗同理（整盒命中，见上）：它画在平铺窗口之上，所以先于场景图判。 */
	struct vidow_view *mini = mini_box_view_at(server, lx, ly, sx, sy);
	if (mini != NULL) {
		if (surface != NULL) {
			*surface = vidow_view_surface(mini);
		}
		return mini;
	}
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
			/* **裁剪框也要管命中测试**。
			 *
			 * view_clip_box() 把窗口裁到自己那块屏里（场景图是全局坐标的，
			 * 不裁就会画到隔壁显示器上），但 wlr_scene_node_at() 完全不看
			 * 我们的裁剪框 —— 于是「滚到视口右边的窗口」会落在隔壁屏的坐标
			 * 范围里：屏幕上（自己那块屏）看不见，却在隔壁屏那个位置能点、
			 * 能拖边框、能抢焦点（用户实测：「在 DP-3 的应用虽然在 DP-2 看
			 * 不见，但有概率在 DP-2 某个位置能控制尺寸和点击跳转焦点」）。
			 *
			 * 这里按同一份判据再挡一次：命中的点不在裁剪框里就不算命中。
			 */
			struct wlr_box clip = {0};
			/* 只拿**真正画出来**的窗口的裁剪框过滤（2026-10-23）。停车位 /
			 * 没映射的窗口 view_clip_box 现在也会返回一个框（平铺一律裁，
			 * 见那边的注释），可它们屏上一个像素都没有 —— 拿那个框来挡命中
			 * 只会挡错东西：panel 上的 slurp 这类 layer 客户端按下时，
			 * 底下那个看不见的窗口把按下吃掉/挡掉，框选就变成「左上角到
			 * 鼠标」一条、拖一下就结束（用户报的「layer 拖拽从左上角触发」）。
			 * 判据和 workspace 那边同一份：停到停车位的不算画着。 */
			if (tag->view->mapped && !tag->view->scene_parked &&
					view_clip_box(tag->view, &clip)) {
				const double px = lx;
				const double py = ly;
				if (px < clip.x || py < clip.y ||
						px >= clip.x + clip.width ||
						py >= clip.y + clip.height) {
					return NULL;
				}
			}
			vidow_view_surface_coords(tag->view, scene_surface->surface, lx, ly, sx, sy);
			return tag->view;
		}
	}
	return NULL;
}

/* ------------------------------------------------------------- columns */

static bool column_has_views(struct vidow_column *col) {
	return !wl_list_empty(&col->views);
}

/* 最小化的窗口**留在原来的列里**（位置、列宽、上下顺序、归属工作区全都不用重建，
 * 放回来是瞬时的），但它不占位：布局、焦点遍历、命中都要跳过它。列里就剩它一个的
 * 时候，那条列也跟着从条带里消失（niri 的条带是「有东西才占宽度」）。
 *
 * 注意别把它当成「藏起来的工作区」那种不可见：那边是整列都不在条带上，这边列还在，
 * 只是这条列可能空着。所以下面这些判据是「还占着位置」而不是「场景节点开着」。 */
static bool view_shows(const struct vidow_view *view) {
	return view != NULL && !view->minimized && !view->drag_detached &&
		!view->focus_helper;
}

/* 列里第一个「还占着位置」的窗口（焦点落点用）。 */
static struct vidow_view *column_first_shown_view(struct vidow_column *col) {
	struct vidow_view *view;
	wl_list_for_each(view, &col->views, column_link) {
		if (view_shows(view)) {
			return view;
		}
	}
	return NULL;
}

/* 这条列在条带上占位置吗（整列都最小化了就不占）。 */
static bool column_has_shown_views(struct vidow_column *col) {
	return column_first_shown_view(col) != NULL;
}

/* 这条列里有全屏窗口吗。
 *
 * 有的话这条列在条带上占**一整屏**宽（不是它自己的 width_fraction）：全屏的窗口
 * 画出来就是整块屏那么大，滚走它自然也该滚一整屏 —— 否则滚一下只挪几个像素，
 * 它照样糊在整块屏上（用户报「全屏的窗口也要参与滚动」）。判据只认「全屏 + 已上屏
 * + 没最小化」，跟摆放那一趟用的完全一致（见 output_arrange 的 Pass 3）。 */
static bool column_has_fullscreen(struct vidow_column *col) {
	struct vidow_view *view;
	wl_list_for_each(view, &col->views, column_link) {
		if (view->fullscreen && view->mapped && view_shows(view)) {
			return true;
		}
	}
	return false;
}

/* 聚焦的这条列「在条带上多宽」：全屏的那条算一整屏 —— 全屏窗口画出来就是整块
 * 屏那么大，它在条带上占的位置自然也该是一整屏（不是给全屏开特例：列就是列，
 * 只是它的宽度就是一屏）。滚动（Pass 2 / 预测）两处都用这一个。 */
static int column_strip_width(struct vidow_column *col, int work_width) {
	return column_has_fullscreen(col) ? work_width : col->width;
}

/* 「把焦点列摆到视口里，视口该在哪」—— niri 的 scroll_amount_to_activate 算的是
 * 同一件事。三种策略（configs/20-layout.lua 的 center_focused_column）：
 *
 *   never（默认）  最小滚动：只滚「刚好让焦点列整个露出来」的那点距离，焦点列
 *                  因此常常贴着视口左 / 右边缘 —— vidow 一直以来的行为。
 *   always         焦点列永远摆到视口正中（条带两头夹住，见下面的 clamp）。
 *   on-overflow    焦点列和**上一个焦点列**能一起完整放进视口就不动；放不下
 *                  （= 溢出）才居中。niri 的同名取值。
 *
 * 只算「该在哪」，不真改视口 —— 调用方自己 clamp 到 0..max_viewport。 */
static double focus_viewport_target(struct vidow_server *server,
		struct vidow_output *output, double viewport, int work_width) {
	if (work_width < 1) {
		return viewport;
	}
	struct vidow_view *focus = server->focused;
	if (focus != NULL && focus->float_mode == VIDOW_FLOAT_FOLLOW &&
			focus->parent != NULL) {
		/* scroll to where the dialog's parent lives */
		focus = focus->parent;
	}
	struct vidow_column *focused_col = focus != NULL ? focus->column : NULL;
	if (focused_col == NULL || focused_col->output != output->wlr_output) {
		return viewport;
	}

	/* 居中策略可以按屏覆盖（niri 的 output { layout { center-focused-column } }）：
	 * 读这块屏最终生效的那份值。 */
	struct vidow_layout lay;
	vidow_layout_for(server, output->wlr_output, &lay);

	/* 这条列在条带上占多宽（全屏那条 = 一整屏）。 */
	int cw = column_strip_width(focused_col, work_width);
	/* 居中要用的宽度：列比视口还宽就没法居中（夹住，跟 niri 一样）。 */
	int center_w = cw > work_width ? work_width : cw;

	if (lay.center_focused_column == VIDOW_CENTER_ALWAYS) {
		return focused_col->offset - (work_width - center_w) / 2.0;
	}

	if (lay.center_focused_column == VIDOW_CENTER_ON_OVERFLOW) {
		/* 和上一个焦点列能不能一起完整放进视口。上一个没了 / 在别的屏上 /
		 * 就在同一条列里 —— 都算「放得下」（不居中）。 */
		struct vidow_column *prev = server->last_focused_col;
		bool fits = true;
		if (prev != NULL && prev != focused_col &&
				prev->output == output->wlr_output &&
				column_has_shown_views(prev)) {
			int pw = column_strip_width(prev, work_width);
			int lo = prev->offset < focused_col->offset ?
				prev->offset : focused_col->offset;
			int hi_a = prev->offset + pw;
			int hi_b = focused_col->offset + cw;
			int hi = hi_a > hi_b ? hi_a : hi_b;
			fits = hi - lo <= work_width;
		}
		if (!fits) {
			return focused_col->offset - (work_width - center_w) / 2.0;
		}
		/* 放得下：退回最小滚动（下面那段）。 */
	}

	/* 最小滚动：焦点列在视口右边外面就往左滚到贴右边缘，在左边外面就往右
	 * 滚到贴左边缘；已经在里面就不动。 */
	double target = viewport;
	int right = focused_col->offset + cw;
	if (right > target + work_width) {
		target = right - work_width;
	}
	if (focused_col->offset < target) {
		target = focused_col->offset;
	}
	return target;
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
	/* 列宽：窗口规则写了 column_width 就用它（这个窗口新开的列有多宽），
	 * 没写用配置里的 default_column_width（默认 0.5，就是原来写死的那个
	 * DEFAULT_COLUMN_WIDTH —— 见 configs/20-layout.lua）。新列不做过渡
	 * （它是刚生出来的，宽度没有「变化」），width_shown 跟逻辑值一起给上
	 * —— arrange 画的是 width_shown。 */
	struct vidow_layout lay;
	vidow_layout_for(server, view->output, &lay);
	col->width_fraction = view->rule_eff.set[VRA_COLUMN_WIDTH] ?
		view->rule_eff.val[VRA_COLUMN_WIDTH].d :
		lay.default_column_width;
	col->width_shown = col->width_fraction;
	wl_list_init(&col->views);

	/* 这条列落在哪块工作区：窗口自己记着的那块（新窗口在 map 时就定好了，
	 * 见 view_map），没有就落到这块屏的活动工作区上。 */
	struct vidow_workspace *ws = view->ws;
	if (ws == NULL) {
		ws = view->output != NULL ?
			vidow_workspace_active_for(server, view->output) :
			vidow_workspace_active(server);
		if (ws == NULL) {
			/* **一块屏都没有**（嵌套会话的外层窗口关了 / 最后一块屏拔了）：
			 * 这时候没有「当前工作区」这回事，但窗口照样会 map —— XWayland
			 * 还活着，X 客户端一连上来就 map（用户报的「打开 kate 直接崩」
			 * 就是这条：原来这里直接解引用 NULL，SEGV 在 view_map 里）。
			 * 安置到一块现在没屏的工作区上：屏回来的时候那块工作区会被认领
			 * 回去（vidow_workspace_adopt_homeless），窗口跟着一起回来。 */
			ws = vidow_workspace_homeless(server,
				home_of(server, view->output));
			if (ws == NULL) {
				ws = vidow_workspace_create(server, NULL, NULL);
			}
		}
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
	if (ws != NULL && !ws->special && ws->output != NULL) {
		vidow_workspace_ensure_bottom(server, ws->output);
	}
}

void vidow_view_leave_column(struct vidow_view *view) {
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
		/* 列宽过渡的 data 就是这条列：不掐掉的话下一帧它会写一块已经 free
		 * 掉的内存（列没了但补间还在跑）。 */
		vidow_anim_cancel(col->anim_width);
		col->anim_width = NULL;
		/* 「上一个焦点列」指着它的话先松手 —— 这条列马上就不是列了
		 * （center_focused_column = "on-overflow" 要用，见
		 * focus_viewport_target）。 */
		if (view->server->last_focused_col == col) {
			view->server->last_focused_col = NULL;
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

/* 窗口**属于**哪块屏：平铺 / 全屏的窗口看**它那条列**挂在哪块屏
 * （view->column->output）—— 跟滚动视口无关，稳定。
 *
 * 浮动 / 弹窗返回 NULL：它们不参与滚动（geom 就是自己的绝对位置），按「现在画在
 * 哪块屏上」算才对 —— 拖着跨屏时 view->output 还停在原来那块屏上（见 view_scale
 * 的注释）。
 *
 * 为什么平铺的不能按「现在画在哪」算：平铺窗口的 geom 里含着**滚动视口的偏移**
 * （output_arrange: x = 工作区 + 列偏移 - 视口）。窗口滚出视口之后，它画的那些
 * 像素落在隔壁屏的布局范围里（场景是全局坐标的，见 vidow_view_in_output_box），
 * 于是「画在哪块屏上」的答案变成隔壁那块 —— 倍率、模糊底衬、窗口录屏的取景框都
 * 跟着错（越界的那一块本来就被裁掉了、压根不画，隔壁那块屏跟这个窗口没关系）。 */
struct wlr_output *vidow_view_owner_output(const struct vidow_view *view) {
	/* 唯一真值：窗口属于工作区，工作区属于屏。浮窗也一样（它只是不占列宽，
	 * 不是「不属于工作区」）。弹窗 / 小窗没有工作区，各自按自己的位置算。 */
	if (view->popup || view->mini != VIDOW_MINI_NONE) {
		return NULL;
	}
	if (view->ws != NULL && view->ws->output != NULL) {
		return view->ws->output;
	}
	if (view->column != NULL) {
		return view->column->output;
	}
	return NULL;
}

static bool view_clip_box(struct vidow_view *view, struct wlr_box *out);

/* 「看得见的那一块」在布局坐标里的框（哪块屏上都没画着时是空的）：
 *   停到停车位（滚出视口 / 藏起来的工作区）→ 空；
 *   裁着（平铺窗口有一部分越出自己那块屏）→ 几何框 ∩ 自己那块屏的框；
 *   其余（浮窗 / 正被拖的窗口 / 整块都在自己屏里）→ 几何框本身。
 *
 * 为什么不能直接用几何框：平铺窗口的几何框里含着**滚动视口的偏移**，滚出视口之后
 * 会跨到隔壁屏的布局范围里 —— 照它算，倍率就落到隔壁那块屏上：2 倍屏上的窗口在
 * 录屏 / 预览里一直按 1 倍糊着，滚回视口才清楚（用户报的「窗口滚动出去后，又按照
 * 另一个屏幕的缩放变化了」）。裁剪（view_clip_box）裁掉的正是越界那一块，所以
 * 「看得见的」= 几何框 ∩ 那块屏的框。 */
static void view_visible_box(const struct vidow_view *view, struct wlr_box *out) {
	*out = (struct wlr_box){0};
	if (!view->mapped) {
		return;
	}
	if (vidow_view_should_park(view->server, (struct vidow_view *)view)) {
		return; /* 停到停车位：哪块屏上都没有它，判据回退到「它属于哪块屏」 */
	}
	struct wlr_box box = vidow_view_screen_box(view);
	struct wlr_box clip = {0};
	if (view_clip_box((struct vidow_view *)view, &clip)) {
		struct wlr_box hit;
		if (!wlr_box_intersection(&hit, &box, &clip)) {
			return; /* 裁完不剩什么 */
		}
		box = hit;
	}
	*out = box;
}

/* 这块窗口**算在哪块屏上**（倍率 / 模糊底衬都按它）：**看得见的那一块**画在哪块
 * 屏上 —— 它看得见的那些屏里缩放最大的那块（跨在两块屏上时取大的那个，理由见
 * 下面 view_scale 的注释）；一点都看不见（滚出视口、藏起来的工作区、还没摆过）时
 * 回退到「它属于哪块屏」（vidow_view_owner_output，浮动 / 弹窗没有归属时返回 NULL）。
 *
 * 「看得见的那一块」是关键：平铺窗口的几何框里含着滚动视口的偏移，滚出视口之后
 * 会跨到隔壁屏的布局范围里，但那一块被裁掉了（见函数里的说明）。
 *
 * 装饰的离屏缓冲倍率（view_scale）和私有模糊底衬归哪块屏（fx.c 的
 * vidow_blur_update）都走这一个判据 —— 「这块窗口现在算在哪块屏上」只能有一个
 * 答案，两个地方各算各的就会错位（模糊贴图的采样坐标是按底衬那块屏的设备像素算
 * 的，倍率对不上就采错地方）。 */
static struct wlr_output *view_drag_output(const struct vidow_view *view);

struct wlr_output *vidow_view_display_output(const struct vidow_view *view) {
	if (!view->mapped) {
		return NULL;
	}
	/* 拖拽中：窗口真的已经跟着光标画到对面那块屏上了 —— 模糊底衬的区域、
	 * 采样 UV 都必须按**那块屏的设备像素**算，按旧归属屏算出来就是一片横条
	 * （贴图尺寸 / 原点和实际画的地方对不上，见 view_drag_output 的说明）。
	 * 放在最前面：平铺窗口那条归属捷径会直接短路成 ws->output。 */
	struct wlr_output *dragged = view_drag_output(view);
	if (dragged != NULL) {
		return dragged;
	}
	/* 有工作区的窗口：显示归属直接取链路（窗口 -> 工作区 -> 屏）。不再按
	 * 几何框找「看得见的那块」—— 窗口跨在屏边界上时，几何框会在两块屏之间
	 * 来回翻，倍率跟着 1/2 反复切（wlr_scene_surface 也按可见面积发
	 * preferred_buffer_scale，两边对着跳）。 */
	if (!view->popup && view->mini == VIDOW_MINI_NONE) {
		struct wlr_output *chain = vidow_view_owner_output(view);
		if (chain != NULL) {
			return chain;
		}
	}
	/* 判据是「**看得见**的那一块现在画在哪块屏上」：
	 *   - 平铺窗口滚出视口时，几何框里含着滚动视口的偏移、会跨到隔壁屏的布局范围
	 *     里 —— 但越界的那一块被裁掉了（view_clip_box / fx.clip），根本没人画。
	 *     照几何框算，倍率就落到隔壁那块屏上（2 倍屏上的窗口录 / 预览时一直按
	 *     1 倍糊着，滚回视口才清楚 —— 用户报的「窗口滚动出去后，又按照另一个
	 *     屏幕的缩放变化了」）。
	 *   - 拖着窗口跨屏的那一段**不裁**（浮窗、正被拖的窗口允许跨屏，见
	 *     view_clip_box），可见部分就是几何框 —— 那时候必须按对面那块屏算，
	 *     不然拖到 4K 屏上整块窗口是放大出来的（test-mixdpi 钉着这一条）。
	 *   - 一点都看不见（滚出视口 / 藏起来的工作区 / 还没摆过）：回退到「它属于
	 *     哪块屏」（平铺的看它那条列，浮动 / 弹窗没有归属 → NULL）。
	 * 「看得见的那一块」由 view_visible_box 现算（不看上一帧存下来的 clip 状态：
	 * 摆位那份比裁剪那份先跑，读存下来的会落后一帧 —— 于是「刚滚出视口」那一帧
	 * 按几何框算成隔壁屏、把客户端的倍率钉错，见 view_pin_scale）。
	 */
	struct wlr_box box = {0};
	view_visible_box(view, &box);
	struct wlr_output *best = NULL;
	double best_scale = 0.0;
	if (box.width > 0 && box.height > 0) {
		struct vidow_output *output;
		wl_list_for_each(output, &view->server->outputs, link) {
			struct wlr_output *wlr_output = output->wlr_output;
			if (wlr_output == NULL || !wlr_output->enabled) {
				continue;
			}
			struct wlr_box obox;
			wlr_output_layout_get_box(view->server->output_layout,
				wlr_output, &obox);
			struct wlr_box hit;
			if (!wlr_box_intersection(&hit, &box, &obox)) {
				continue;
			}
			double scale = wlr_output->scale > 0.0 ?
				wlr_output->scale : 1.0;
			if (scale > best_scale) {
				best_scale = scale;
				best = wlr_output;
			}
		}
	}
	if (best == NULL) {
		best = vidow_view_owner_output(view);
	}
	return best;
}

/* 窗口**属于**哪块屏就按那块屏的缩放烘（平铺的看它那条列，浮动 / 弹窗看它现在
 * 画在哪块屏上，见 vidow_view_display_output）。装饰着色器的离屏缓冲按它分配 ——
 * 「同一块窗口在不同 DPI 的屏上都清晰」的另一半：客户端那边靠 buffer_scale 声明
 * 自己画了多大，这边靠渲染倍率保证装饰和着色器也是按设备像素画的。
 *
 * 浮动窗口拖着跨屏的那一段，判据是**画在哪**而不是归属（view->output）：归属要到
 * 松手才换，可它已经在对面那块屏上画着了。按 view->output 算，离屏缓冲还是旧倍率
 * （实测：拖到 4K 屏上还是 2076x1236 的 1 倍缓冲），整块窗口被放大到新屏上 ——
 * 4K 屏上一直糊着，松手（归属换了）才清楚。跨在两块屏上时取大的那个倍率：客户端
 * （GTK 那类）一碰到 2 倍屏就按 2 倍重画了，我们按 1 倍烘的话它交上来的 2 倍缓冲
 * 还得先缩下去、屏幕上再放大 —— 还是糊。宁可多烘（画到 1 倍屏上时是缩下去，仍然
 * 清晰），不能少烘（画到 4K 屏上是放大，糊）。
 *
 * 平铺窗口不这么算：它的 geom 里含着滚动视口的偏移，滚出视口之后「画在哪块屏上」
 * 会翻到隔壁那块去（见 vidow_view_owner_output）。 */
/* **窗口属于哪块屏 —— 唯一出口**。
 *
 * 按「窗口挂载的工作区」算，不是按它此刻画在哪儿。几何框靠不住：窗口贴着屏
 * 边界（比如 x=1917，隔壁屏从 1920 开始）、或者摆放动画里扫过隔壁屏时，
 * view->output 会短暂失效 / 指到隔壁屏上 —— 倍率被钉成隔壁屏的值（Chromium
 * 收到 2 倍后不会自己切回 1，内部页面一直按 2 倍排版），裁切也会因为拿不到
 * 屏框而整块放行（表面画到隔壁屏上，wlroots 再发一次 enter + 2 倍，两边对着
 * 跳，实测 15ms 一轮）。
 *
 * 工作区的归属只有真的换屏（换工作区 / 拖过去）才变，动画和贴边都不影响。
 * 倍率、裁切都从这里取，别再各写一套。
 *
 * **拖拽中是唯一的例外**：窗口正被往另一块屏上搬，得按落点那块屏算，否则
 * 落到 2 倍屏上时还是 1 倍缓冲、整块是放大出来的（test-mixdpi 钉这条）。 */
static struct wlr_output *view_home_output(const struct vidow_view *view) {
	/* 窗口 -> 工作区 -> 屏。拖动中也不破例：窗口还没换工作区，就还在原来
	 * 那块屏上；松手换了工作区，倍率才跟着换。 */
	if (view->popup || view->mini != VIDOW_MINI_NONE) {
		struct wlr_output *out = vidow_view_display_output(view);
		if (out != NULL) {
			return out;
		}
	}
	if (view->ws != NULL && view->ws->output != NULL) {
		return view->ws->output;
	}
	if (view->output != NULL) {
		return view->output;
	}
	return vidow_view_display_output(view);
}

/* 拖拽中窗口**此刻画在哪块屏上**（平铺 / 浮窗 / 小窗都算）。不在拖拽里返回
 * NULL —— 那时候倍率该按归属屏算（view_home_output），不能按「画在哪」翻。
 *
 * 为什么拖拽要单独一份：窗口跟着光标越过屏边界之后，它**真的已经画在对面那块
 * 屏上了**（场景图是全局坐标，见 vidow_view_screen_box），而 view_home_output
 * 要到松手换工作区才变。两者不一致时：
 *
 *   - 装饰离屏缓冲按旧屏倍率分配（ensure_targets 按 fx->scale 缓存，换屏不重建），
 *     节点却摆在新屏上 —— 拖到 2 倍屏上整块是放大糊的；
 *   - 裁剪框取旧屏（view_clip_box），窗口一越过边界就被裁到旧屏框里，屏幕上
 *     表现为「拖到一定距离窗口就没了」；
 *   - 私有模糊底衬的区域原点 / 采样 UV 按旧屏设备像素算，装饰却画在新屏上 ——
 *     UV 在 V 方向恒定，就是用户报的「一片横条」。
 *
 * 判据取**窗口覆盖面积最大**的那块屏（和 vidow_view_display_output 里跨屏那段
 * 同一个思路），避免刚好骑在边界上时来回翻。 */
static struct wlr_output *view_drag_output(const struct vidow_view *view) {
	if (view->server == NULL || !view->mapped) {
		return NULL;
	}
	bool dragging = view->dragging || view->server->grab_view == view;
	if (!dragging) {
		return NULL;
	}
	struct wlr_box box = vidow_view_screen_box(view);
	if (box.width <= 0 || box.height <= 0) {
		return NULL;
	}
	struct wlr_output *best = NULL;
	int64_t best_area = 0;
	struct vidow_output *output;
	wl_list_for_each(output, &view->server->outputs, link) {
		struct wlr_output *wlr_output = output->wlr_output;
		if (wlr_output == NULL || !wlr_output->enabled) {
			continue;
		}
		struct wlr_box obox;
		wlr_output_layout_get_box(view->server->output_layout, wlr_output,
			&obox);
		struct wlr_box hit;
		if (!wlr_box_intersection(&hit, &box, &obox)) {
			continue;
		}
		int64_t area = (int64_t)hit.width * (int64_t)hit.height;
		if (area > best_area) {
			best_area = area;
			best = wlr_output;
		}
	}
	return best;
}

static double view_scale(const struct vidow_view *view) {
	struct wlr_output *out = view_drag_output(view);
	if (out == NULL) {
		out = view_home_output(view);
	}
	if (out == NULL) {
		return 1.0;
	}
	double scale = out->scale;
	return scale > 0.0 ? scale : 1.0;
}

struct pin_scale_ctx {
	int32_t want;
};

static void pin_scale_surface(struct wlr_surface *surface, int sx, int sy,
		void *data) {
	(void)sx;
	(void)sy;
	struct pin_scale_ctx *ctx = data;
	/* 值没变就一个事件都不发。以前每次 commit / 每帧 arrange 都发一遍：
	 * wlr_surface_set_preferred_buffer_scale 自己会去重，但
	 * wlr_fractional_scale_v1_notify_scale 不会 —— Electron / GTK 每收一次
	 * 就重算一次缓冲，surface 在 1x/2x 之间来回跳，屏幕上就是界面大小一直
	 * 微微抖动（layer / 2x 那条也是同一类）。 */
	if (surface->WLR_PRIVATE.preferred_buffer_scale == ctx->want) {
		return;
	}
	wlr_surface_set_preferred_buffer_scale(surface, ctx->want);
	/* **fractional-scale 那条也要发**。Chromium / Electron / 新版 GTK 优先听
	 * wp_fractional_scale_v1.preferred_scale，它比 wl_surface.preferred_buffer_scale
	 * 优先 —— 只发后者的话它们按「上一次进过的输出」的倍率画：窗口从 DP-2（2x）
	 * 搬到 DP-3（1x）之后仍然画 2x，像素多 4 倍（实测 Element 2304x2560 铺在
	 * 949x1025 上）。这是「所有应用都卡」的真根因。 */
	wlr_fractional_scale_v1_notify_scale(surface, (double)ctx->want);
}

/* 客户端按几倍渲染：**钉在它自己那块屏的倍率上**（view_scale）。子表面一起钉
 * （mpv / vlc 那类把画面放子表面里，只钉顶层的话它们会掉回 1 倍）。
 *
 * 不钉会怎样：窗口滚出视口之后会被停到**所有输出框外面**的停车位
 * （vidow_view_set_parked）—— 它就不再「在」任何输出上了，客户端（kitty 这类按
 * 进了哪些输出算倍率的）退到 1 倍重画：4K 屏上的窗口在按窗口录屏 / 预览里变成
 * 半分辨率、糊着，滚回视口才清楚（用户报的「窗口滚动出去后，又按照另一个屏幕的
 * 缩放变化了」）。wl_surface.preferred_buffer_scale 的优先级**高于**「进了哪些
 * 输出」（kitty: `integer_scale.preferred` 顶替 `deduced`），钉住它就跟滚动无关。
 *
 * 只在真的变了的时候发（滚动每帧都摆位置，不判一下就是每帧一个协议事件）。 */
static void view_pin_scale(struct vidow_view *view) {
	struct wlr_surface *surface = vidow_view_surface(view);
	if (surface == NULL) {
		return;
	}
	/* X11 的缩放桥走 _XWAYLAND_GLOBAL_OUTPUT_SCALE，Xwayland 自己已经把
	 * 缓冲按那个倍数放大了；再发一遍 preferred_buffer_scale / fractional
	 * scale 会让它在这个倍数上**再乘一次**（2x 屏上变成 4x，用户报的
	 * 「Xwayland 窗口太大」）。X11 只发 enter / leave，不发倍率。 */
	if (view->kind == VIDOW_VIEW_X11) {
		return;
	}
	int32_t want = (int32_t)lround(view_scale(view));
	if (want < 1) {
		want = 1;
	}
	/* **只在值变化时发**（和 Hyprland 的 CFractionalScaleAddon::setScale 一致）：
	 *
	 *   if (m_scale && m_scale == scale) return;
	 *
	 * 之前这里改成了「每次 commit 都发」，理由是「Chromium 重建
	 * wp_fractional_scale_v1 对象收不到旧值」—— 那个理由站不住：协议实现本来
	 * 就该在客户端 get_fractional_scale 时补发当前值（Hyprland 的
	 * onGetFractionalScale 就是这么做的），wlroots 那份同样如此。
	 *
	 * 而每次 commit 都发是有代价的：wlr_surface_set_preferred_buffer_scale /
	 * wlr_fractional_scale_v1_notify_scale 都不去重，客户端每收一次事件就可能
	 * 重新分配缓冲 —— 实测 Electron 的 surface 每 6~12ms 在「声明 1 倍」和
	 * 「声明 2 倍」之间跳，正好等于每次 commit 的间隔。 */
	/* 根表面值变了才发协议事件；子表面仍然每帧走一遍（很便宜），这样
	 * 晚创建的子表面也能拿到当前倍率。值没变的表面在 pin_scale_surface
	 * 里直接跳过 —— 客户端不会每帧重算缓冲（见那里的注释）。 */
	bool changed = view->pref_scale != want;
	if (changed) {
		view->pref_scale = want;
		wlr_surface_set_preferred_buffer_scale(surface, want);
		wlr_fractional_scale_v1_notify_scale(surface, (double)want);
	}
	struct pin_scale_ctx ctx = { .want = want };
	wlr_surface_for_each_surface(surface, pin_scale_surface, &ctx);
	if (changed) {
		/* 带上窗口编号（IPC 那个 id）：钉是在**第一次摆放**时发生的，那会儿
		 * 标题往往还没来 —— 日志按编号认人更稳（测试也按编号找这一行）。 */
		wlr_log(WLR_INFO, "surface %llu %s: 钉住 preferred_buffer_scale = %d"
			"（它那块屏的缩放，滚出视口 / 停到屏外也不跟着变）",
			(unsigned long long)view->id,
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			want);
	}
}

/* 表面进哪块输出：**只进它工作区那块**，别的全退掉，并且只发这一块的倍率。
 *
 * wlr_scene_surface 的 outputs_update 监听按「节点可见面积」发 enter +
 * preferred scale。节点跨在屏边界上时，隔壁 2 倍屏会赢；它只在输出几何变化
 * 时重算，我们 set_clip 改了也不重算 —— 补一次 leave 会被下一帧 enter 覆盖，
 * 客户端就在 1/2 倍之间反复重画。Hyprland / niri 都是自己按 monitor 发 enter
 * 和 fractional scale，不交给场景；这里照做（建表面时摘掉那个监听，见
 * view_takeover_surface_scale）。 */
static void view_surface_enter_one(struct wlr_surface *surface,
		int sx, int sy, void *data) {
	(void)sx;
	(void)sy;
	struct wlr_output *keep = data;
	struct wlr_surface_output *so, *tmp;
	wl_list_for_each_safe(so, tmp, &surface->current_outputs, link) {
		if (so->output != keep) {
			wlr_surface_send_leave(surface, so->output);
		}
	}
	wlr_surface_send_enter(surface, keep);
}

/* 拖拽跨屏：让表面**同时 enter 到它现在压着的那几块屏**。
 *
 * 这是「第一次拖到另一块屏一定看不见窗口」的根因那一层：表面只 enter 过归属屏，
 * 另一块屏的 wlr_scene 输出在合成时就没有这个表面的可见区域，窗口画过去也是空的
 * —— 裁剪、倍率、模糊全对也没用，那块屏压根没画。松手之后工作区真的搬过去，
 * 归属跟着变，窗口才「突然出现」（用户看到的就是「松手才出现」）。
 *
 * 只在拖拽期间走这条路：平时一块窗口只属于一块屏（view_surface_enter_one 那套
 * 「只留一块」是硬约束，见 view_clip_box 的注释）。拖完松手那一刻 owner 变了，
 * 会自己回到只留一块。 */
static void view_sync_drag_outputs(struct vidow_view *view,
		struct wlr_surface *surface) {
	struct vidow_server *server = view->server;
	if (server == NULL) {
		return;
	}
	struct wlr_box box = vidow_view_screen_box(view);
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_output *wlr_output = output->wlr_output;
		if (wlr_output == NULL || !wlr_output->enabled) {
			continue;
		}
		struct wlr_box obox;
		wlr_output_layout_get_box(server->output_layout, wlr_output, &obox);
		struct wlr_box hit;
		if (!wlr_box_intersection(&hit, &box, &obox)) {
			continue;
		}
		wlr_surface_send_enter(surface, wlr_output);
	}
}

static void view_sync_surface_outputs(struct vidow_view *view) {
	if (view->popup || !view->mapped) {
		return; /* 弹窗 / 还没上屏：各自由场景和自己的输出管 */
	}
	struct wlr_surface *surface = vidow_view_surface(view);
	if (surface == NULL) {
		return;
	}
	/* 拖拽中（平铺 / 浮窗都算）：表面要 enter 到它压着的每块屏，见上面
	 * view_sync_drag_outputs 的说明。 */
	if (view_drag_output(view) != NULL) {
		view_sync_drag_outputs(view, surface);
		return;
	}
	struct wlr_output *owner = vidow_view_owner_output(view);
	if (owner == NULL && view->mini != VIDOW_MINI_NONE) {
		owner = view->output; /* 小窗是屏级对象：没有工作区，按自己的 output */
	}
	if (owner == NULL || owner == view->surface_output) {
		vidow_portal_view_output(view);
		return;
	}
	view->surface_output = owner;
	wlr_surface_for_each_surface(surface, view_surface_enter_one, owner);
	view->pref_scale = 0;
	view_pin_scale(view);
	vidow_portal_view_output(view);
}

/* 摘掉场景那条「按可见面积发 enter + preferred scale」的监听。link 重新
 * 初始化成自环：wlr_scene_surface 销毁时无条件 wl_list_remove，自环上删自己
 * 是安全的（不会碰到已摘掉的邻居）。 */
static void view_takeover_surface_scale(struct vidow_view *view) {
	/* Popups are intentionally excluded from our workspace-driven enter/leave
	 * path. Keep scene output tracking for them, otherwise they never enter
	 * an output and Xwayland can leave their initial buffer unrefreshed. */
	if (view->scene_surface == NULL) {
		return;
	}
	if (view->popup) {
		/* Classification may change after association. Restore the original
		 * listener only when our normal-window path previously detached it. */
		struct wl_listener *listener =
			&view->scene_surface->WLR_PRIVATE.outputs_update;
		if (wl_list_empty(&listener->link)) {
			wl_signal_add(&view->scene_surface->buffer->events.outputs_update,
				listener);
		}
		return;
	}
	wl_list_remove(&view->scene_surface->WLR_PRIVATE.outputs_update.link);
	wl_list_init(&view->scene_surface->WLR_PRIVATE.outputs_update.link);
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
 * 比的是**这一帧画在哪儿**（几何 + 动画偏移，见 vidow_view_screen_box），不是布局
 * 目标位置：滚动 / 开窗动画里窗口是从屏边界上扫过去的，按目标位置判的话，滑回来的
 * 那一路（目标已经落在屏里、画面还在屏外）会被当成「在屏里」，越界的那一块就画到
 * 隔壁屏上了（用户报的「视口外的窗口回来的瞬间会显示在另一个显示器上」）。
 *
 * 还没摆放（几何是空的）和不知道归哪块屏的都当「画得到」：宁可多画一帧，也别把
 * 一个刚 map 的窗口藏没了。 */
bool vidow_view_in_output_box(struct vidow_server *server,
		const struct vidow_view *view) {
	/* 平铺窗口按**它那条列挂在哪块屏**判（跟滚动视口无关），浮动 / 弹窗
	 * 没有归属、按住址判。和 view_clip_box / view_home_output 同一个来源 ——
	 * 以前这里只读 view->output，热插拔 / 规则搬屏的中间态里它可能还指着
	 * 旧屏，于是停车位按旧屏算、裁剪按新屏算，窗口在视口外的那一帧被画到
	 * 隔壁显示器上。 */
	struct wlr_output *out = vidow_view_owner_output(view);
	if (out == NULL) {
		out = view->output;
	}
	if (out == NULL || view->geom.width <= 0 ||
			view->geom.height <= 0) {
		return true;
	}
	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, out, &box);
	if (box.width < 1 || box.height < 1) {
		return true;
	}
	struct wlr_box at = vidow_view_screen_box(view);
	return at.x < box.x + box.width && at.x + at.width > box.x &&
		at.y < box.y + box.height && at.y + at.height > box.y;
}

/* 这块窗口现在该不该裁到自己那块屏里，裁的话边界是哪块屏的框。 */
static bool view_clip_box(struct vidow_view *view, struct wlr_box *out) {
	struct vidow_server *server = view->server;

	/* 小窗（mini window，S3）：**强制裁到自己那块屏**。
	 *
	 * ⚠️ 这条和下面「浮动窗口一律不裁」是**相反**的语义，别合并：浮动窗口跨屏
	 * 是允许的（拖到一半正好骑在两块屏中间，裁了就会缺一块 —— test-mixdpi 钉着
	 * 这条），而小窗是**屏级对象**（像安卓的画中画），图标态还会按
	 * mini_icon_overflow 刻意探出屏边一点。不裁的话那半块会画到隔壁显示器上
	 * （场景图是全局坐标的，隔壁那块屏照画落在自己坐标里的所有节点）。
	 *
	 * 放在「浮动不裁」那条放行**之前**：小窗态 float_mode 是 FIXED，晚一步就
	 * 被上面那行 return false 放走了。 */
	if (view->mini != VIDOW_MINI_NONE) {
		if (!view->mapped || view->output == NULL ||
				view->geom.width <= 0 || view->geom.height <= 0) {
			return false;
		}
		if (vidow_overview_active(server) ||
				vidow_view_should_park(server, view)) {
			return false;
		}
		struct wlr_box mbox = {0};
		wlr_output_layout_get_box(server->output_layout, view->output, &mbox);
		if (mbox.width < 1 || mbox.height < 1) {
			return false;
		}
		/* 整块都在自己那块屏里就不裁：裁一次要把场景节点的显示范围改小，
		 * 白白多一次损伤和一次重烘（判据和下面那条一样，比的是这一帧画在
		 * 哪儿 —— 几何 + 动画偏移）。 */
		struct wlr_box mat = vidow_view_screen_box(view);
		struct wlr_box minter;
		if (wlr_box_intersection(&minter, &mat, &mbox) &&
				minter.width == mat.width && minter.height == mat.height) {
			return false;
		}
		*out = mbox;
		return true;
	}
	/* 有工作区的窗口平时一律裁到自己那块屏： 「窗口只在自己的工作区显示」
	 * 是硬约束。小窗和弹窗在上面 / 后面各自走自己的路。
	 *
	 * **但拖拽中的窗口是例外**（下面那段，用户 2026-10-23 报的「跨一半之后
	 * 突然被另一个屏幕裁剪了」）。 */
	if (!view->mapped || view->popup ||
			vidow_overview_active(server) ||
			view->geom.width <= 0 || view->geom.height <= 0) {
		return false;
	}
	/* **浮动窗口一律裁到自己那块屏**（2026-10-23 用户明确要求）。
	 *
	 * 以前这里对 float_mode != NONE 直接放过 —— 语义是「浮窗允许跨屏」，代价
	 * 是浮窗骑在两块屏中间时那半块会画到隔壁显示器上，而且两块屏的缩放不同时
	 * 那半块的模糊底衬只能选一块屏来采（一片横条）。用户的原话：「算了浮动
	 * 窗口也不要越界」。
	 *
	 * 小窗（mini）在上面已经单独处理，走的是自己那条更强的「强制裁」；这里只
	 * 剩下普通浮窗 / X11 的浮动窗口。判据和下面平铺那条完全一样，只是不参与
	 * 「拖拽中按画在哪块屏算」那一段 —— 浮窗**拖拽中**仍然按它此刻画在哪块屏
	 * 裁（view_drag_output），这样松手前对面那块屏本来就在画它，只是裁到那块
	 * 屏自己的工作区里。 */
	if (view->float_mode != VIDOW_FLOAT_NONE) {
		struct wlr_output *fout = vidow_view_display_output(view);
		if (fout == NULL) {
			fout = view_home_output(view);
		}
		if (fout == NULL) {
			fout = wlr_output_layout_output_at(server->output_layout,
				view->geom.x + view->geom.width / 2,
				view->geom.y + view->geom.height / 2);
		}
		if (fout == NULL) {
			return false;
		}
		struct wlr_box fbox = {0};
		wlr_output_layout_get_box(server->output_layout, fout, &fbox);
		if (fbox.width < 1 || fbox.height < 1) {
			return false;
		}
		struct wlr_box fat = vidow_view_screen_box(view);
		struct wlr_box fhit;
		if (wlr_box_intersection(&fhit, &fat, &fbox) &&
				fhit.width == fat.width && fhit.height == fat.height) {
			return false; /* 整块都在自己那块屏里：不用裁 */
		}
		*out = fbox;
		return true;
	}
	/* **拖拽中的窗口不裁**：窗口跟着光标越过边界时，它真的已经（也应该）
	 * 同时出现在两块屏上 —— 裁到自己原来那块屏就把跨过去的那半块整块切掉，
	 * 屏幕上就是「拖到一半突然少一半」。旧注释里写的「正在拖的窗口也不例外」
	 * 正是这个 bug 的来源。
	 *
	 * 和浮动窗口那条同理（浮动窗口跨屏本来就允许，裁了会缺一块，
	 * test-mixdpi 钉着那条）；区别只在于平铺窗口平时确实要「只在自己的
	 * 工作区显示」，而**拖拽是那个约束的临时例外**：松手之后窗口归到新
	 * 工作区，裁剪自然恢复，不会留下永久越界的窗口。
	 *
	 * 判据用 view_drag_output()（拖着 = view->dragging 或 grab_view 是
	 * 自己），和 view_scale 取倍率、view_clip_box 取屏框、模糊底衬归属
	 * 三处同一个来源 —— 三处不一致就会出「倍率和底衬对不上」。 */
	/* **拖拽窗口照裁**（2026-10-23 改）：以前这里对拖拽豁免过，理由是
	 * 「拖到一半被裁掉一半」——那是没做按屏绘制时的权宜之计，代价是整个
	 * 跨屏模型都成了「一个窗口横跨两块屏」，模糊底衬只能选一块屏来采，
	 * 另一半就成了横条纹。
	 *
	 * 照 Hyprland：没有「跨屏的窗口」。每块屏各自渲染一遍、各自裁到自己
	 * 那块屏（Renderer.cpp 的 renderMonitor 逐屏循环，窗口和屏的交集非空就
	 * 画一次，模煳用那一屏自己的 m_blurFB）。两块屏各画一半，拼起来就是
	 * 「看起来跨屏」，而每一半都用自己那块屏的底衬 —— 这才是对的。
	 *
	 * 松手之后窗口归到新工作区，裁剪收紧，不会留下永久越界的窗口。 */
	/* 弹出物先不裁：它的位置是相对父窗口算的，越界那点（菜单探出去一条）
	 * 先留着。停到停车位上的窗口也不裁：它本来就不在任何一块屏里（不用裁）。
	 * 判据现算（跟摆位那份同一份，见 vidow_view_should_park）：只看存下来的
	 * 标志会在「这一帧刚从停车位摆回来、还没走 arrange」的时候落后一帧。
	 *
	 * **有人录屏 / 预览也照裁**（以前这儿会跳过）：按窗口录屏的截图源现在渲染的
	 * 是这个窗口自己的一份镜像（src/wincapture.c），跟这块节点裁没裁没关系 ——
	 * 不裁的话，跨屏那半块会一直画在隔壁显示器上（用户报的「开了 obs 就完全没
	 * 藏住」）。
	 *
	 * ⚠️ 停车位**不再**在这里放行（2026-10-23 用户明确要求）：平铺窗口一律裁到
	 * 自己那块屏，只有**拖拽中**和**浮动**窗口可以越界。以前停在停车位上就
	 * return false，等于把「该不该画」和「该不该裁」绑成一件事 —— 而停车位到底
	 * 摆没摆、摆到哪儿还要看它那一帧的动画状态，于是同一个位置会在「裁」和
	 * 「不裁」之间来回翻（日志里就是同一个窗口一秒内先「裁到自己那块屏里」、
	 * 再「不裁（屏框 0,0 0x0）」）。现在两条判据分开：画不画由 workspace /
	 * view_anim_place 决定，裁哪儿只按归属。停车位本来就在所有输出框外面，
	 * 裁了也同一个像素都不画，不会多花一份烘图。 */
	/* **屏框拿不到时不能直接放行**：放行 = 整块画到隔壁屏上，wlroots 会给
	 * 这个表面发 enter + 隔壁屏的 preferred_buffer_scale，Chromium 收到 2 倍
	 * 之后就在 1/2 倍之间反复切（实测 15ms 一轮，内部页面一直重排 —— 用户
	 * 报的「浏览器还是会一直变化内容大小」）。view->output 在切工作区 / 摆放
	 * 动画里会短暂失效，这时改用工作区所属的那块屏 —— 和 view_scale 用的是
	 * 同一个归属，稳定。 */
	/* **拖拽中按它此刻画在哪块屏**：窗口跟着光标越过边界之后，它真的已经画在
	 * 对面那块屏上了（场景图是全局坐标），按旧归属屏裁就会把越界那半块整块切掉
	 * —— 屏幕上就是「拖到一定距离窗口就没了」（用户 2026-10-23 报的）。倍率、
	 * 裁剪、模糊底衬三处必须是同一个判据，见 view_drag_output()。 */
	struct wlr_output *clip_out = view_drag_output(view);
	if (clip_out == NULL) {
		clip_out = view_home_output(view);
	}
	if (clip_out == NULL) {
		/* **别放行**：放行 = 命中测试拿不到屏框，窗口贴着屏幕边缘时
		 * 「右半边点不到」（用户实测：同一个窗口、同一个位置，一秒内
		 * 先后出现「裁到自己那块屏里」和「不裁（屏框 0,0 0x0）」，
		 * 而窗口右边正好压到 3832 / 屏幕到 3839）。
		 *
		 * view_home_output() 在切工作区、摆放动画的中间帧会短暂返回
		 * NULL（它要的 ws->output / view->output 那一刻都还没填上）。
		 * 这时按**几何框**找它落在哪块屏上兜底 —— 布局坐标系是唯一
		 * 的真值，和窗口此刻画在哪儿一致，正好是裁切要的那块。 */
		clip_out = wlr_output_layout_output_at(server->output_layout,
			view->geom.x + view->geom.width / 2,
			view->geom.y + view->geom.height / 2);
	}
	if (clip_out == NULL) {
		return false; /* 真的哪块屏都不沾（比如停在停车位上）：没地方可裁 */
	}
	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, clip_out, &box);
	if (box.width < 1 || box.height < 1) {
		/* 屏框查询拿不到的时候**不能放行**：放行 = 整块画到隔壁屏上（场景图是
		 * 全局坐标）。实测这块屏刚 hotplug / 布局重排的那几帧会查到 0x0，窗口
		 * 于是几十毫秒里一直「不裁」，跨屏那一半直接铺到隔壁显示器上。回退到
		 * 归属那块屏的框；连归属都查不到才真的没法裁。 */
		struct wlr_output *owner = vidow_view_owner_output(view);
		if (owner != NULL && owner != clip_out) {
			wlr_output_layout_get_box(server->output_layout, owner, &box);
		}
		if (box.width < 1 || box.height < 1) {
			return false;
		}
	}
	/* 窗口**本体**整块都在自己那块屏里就不裁（比的是这一帧画在哪儿，动画偏移
	 * 算在内 —— 见 vidow_view_in_output_box 上面那段）。装饰的阴影内边距那点
	 * 越界（窗口贴着屏边时探出去十几个像素）先留着：裁它要把场景节点的显示尺寸
	 * 改小，而窗口截图源（portal 的按窗口录屏）盯着这棵树，节点一变形它就
	 * 重发一次约束 —— 客户端那边正好在等第一帧，撞上「session already has a
	 * frame object」直接断线（踩过：test-force-render.sh 的录屏那节）。窗口
	 * 本体越界才是要修的那个 bug，那一档照裁。 */
	struct wlr_box at = vidow_view_screen_box(view);
	struct wlr_box inter;
	if (wlr_box_intersection(&inter, &at, &box) &&
			inter.width == at.width && inter.height == at.height) {
		return false;
	}
	*out = box;
	return true;
}

/* 表面树在场景里挂的那个节点（wlroots 的 set_clip 要的就是它）。 */
static struct wlr_scene_node *view_surface_tree_node(const struct vidow_view *view) {
	if (view->scene_surface == NULL || view->scene_surface->buffer == NULL ||
			view->scene_surface->buffer->node.parent == NULL) {
		return NULL;
	}
	return &view->scene_surface->buffer->node.parent->node;
}

/* 抄一份「客户端声明的有效源区域」（缓冲像素）到 view->fx.content_box。
 *
 * 为什么要有这一份：越界裁剪走的是 wlroots 的
 * wlr_scene_subsurface_tree_set_clip()，而它是**改写节点的 src_box** 实现的
 * （裁完剩下的那一小条会变成整个 src_box）。装饰采源缓冲的哪一块照的正是这一份
 * （见 fx_geo_tex_map）—— 直接读那个字段，就会把那一小条当成整块有效内容、
 * 拉伸铺满整块窗口（用户报的「窗口只有一点在视口里时表面横向拉伸变扁、黑边随
 * 在视口里的范围大小变」）。
 *
 * 抄之前先把上一刀撤掉：wlroots 会按客户端声明的 viewport / 缓冲重算那一次
 * （它自己就是这么维护这个字段的），抄完再补回去。裁着的时候每来一次 commit
 * 抄一遍 —— 客户端改 viewport（chromium 那类）之后不用等裁剪自己变。 */
static void view_snapshot_content_box(struct vidow_view *view) {
	struct wlr_surface *surface = vidow_view_surface(view);
	if (surface == NULL) {
		return;
	}
	/* **直接问 surface**，别再「先解裁剪 → 读 src_box → 再裁回去」。
	 *
	 * 解裁剪那一瞬间，场景节点的可见区域从「裁剩下的 4px」变回整块 949px：
	 * 它跨在隔壁那块 2 倍屏上，wlr_scene 立刻给表面发 enter + 2 倍；裁回去
	 * 又发 leave + 1 倍。客户端每 commit 一次就走一轮，屏幕上就是倍率 1/2
	 * 反复跳（实测 8ms 一轮）。wlr_surface_get_buffer_source_box() 读的是
	 * 客户端声明的 viewport / 缓冲，跟场景节点裁没裁无关 —— 要的就是这份
	 * 「没裁过」的有效区。 */
	wlr_surface_get_buffer_source_box(surface, &view->fx.content_box);
}

/* 缩放节点的越界裁剪（S3 修）：把表面那份内容节点裁成「这一帧落在屏里那一块」。
 *
 * 为什么不能借 wlroots 的 wlr_scene_subsurface_tree_set_clip()：那套是给
 * **1:1 逻辑坐标**的窗口准备的（内部把 clip 当节点位置用、src_box 按表面尺寸
 * 比例缩、dst 不动，见 types/scene/subsurface_tree.c 的
 * subsurface_tree_reconfigure_clip + types/scene/surface.c 的 src_box 换算）。
 * 缩放态（小窗保留原生 buffer：图标 / 迷你 / 自由小窗，以及过渡动画那几帧）
 * 下它三件事一起错：内容挪到错的地方、剩下那一小条拉伸铺满整块 dst、而且
 * dst 不缩 → 越界那半块照画到隔壁屏上（实测 16×64 条带 832 像素）。
 *
 * 这里自己裁：**源框取可见那一块、目标框也取可见那一块**，缩放比一个像素都不变，
 * 框外那部分根本不画。缓冲像素 ↔ 场景像素按「整块有效源区对应整块 dst」换算
 * （整块源区从 wlr_surface_get_buffer_source_box() 拿，客户端声明的 viewport 也
 * 算在里面）。
 *
 * 返回 true = 这一档接管了（调用方别再走 wlroots 那套）；false = 没接管（不是
 * 能安全处理的缩放表面：没有缓冲 / 旋转过 / 拿不到源区），退回原路。 */
static bool view_scaled_crop(struct vidow_view *view, const struct wlr_box *clip) {
	struct wlr_scene_buffer *sb = view->scene_surface != NULL ?
		view->scene_surface->buffer : NULL;
	struct wlr_surface *surface = vidow_view_surface(view);
	if (sb == NULL || sb->buffer == NULL || surface == NULL ||
			sb->dst_width <= 0 || sb->dst_height <= 0 ||
			surface->current.transform != WL_OUTPUT_TRANSFORM_NORMAL) {
		return false;
	}
	int cx = 0, cy = 0;
	wlr_scene_node_coords(&sb->node, &cx, &cy);
	struct wlr_box cbox = {.x = cx, .y = cy,
		.width = sb->dst_width, .height = sb->dst_height};
	struct wlr_box vis;
	if (!wlr_box_intersection(&vis, &cbox, clip)) {
		/* 这一帧整个身子都在自己那块屏外：一个像素都不该画（场景图是全局
		 * 坐标，画了就是画到隔壁屏上）。甩到所有输出框外面去 —— 和 fx 那边
		 * 同一招（src_box 空着 wlroots 会当成「整块」，那是另一个意思）。 */
		wlr_scene_buffer_set_source_box(sb, NULL);
		wlr_scene_node_set_position(&sb->node,
			sb->node.x - cx - 1000000, sb->node.y - cy - 1000000);
		return true;
	}
	if (vis.width == cbox.width && vis.height == cbox.height) {
		wlr_scene_buffer_set_source_box(sb, NULL); /* 可见那块 = 整块：不用裁 */
		return true;
	}
	struct wlr_fbox full = {0};
	wlr_surface_get_buffer_source_box(surface, &full);
	if (full.width <= 0.0 || full.height <= 0.0) {
		return false;
	}
	double kx = (double)full.width / (double)cbox.width;
	double ky = (double)full.height / (double)cbox.height;
	struct wlr_fbox src = {
		.x = full.x + (double)(vis.x - cx) * kx,
		.y = full.y + (double)(vis.y - cy) * ky,
		.width = (double)vis.width * kx,
		.height = (double)vis.height * ky,
	};
	/* 裁之前先抄一份没裁过的有效区：装饰管线（fx）采源缓冲的哪一块照的是
	 * view->fx.content_box 那一份，别让它认成裁剩下的一小条（187 那个坑）。 */
	wlr_scene_buffer_set_source_box(sb, NULL);
	view_snapshot_content_box(view);
	wlr_scene_buffer_set_source_box(sb, &src);
	wlr_scene_node_set_position(&sb->node,
		sb->node.x + (vis.x - cx), sb->node.y + (vis.y - cy));
	wlr_scene_buffer_set_dest_size(sb, vis.width, vis.height);
	return true;
}

static void view_apply_clip_impl(struct vidow_view *view) {
	if (view == NULL) {
		return;
	}
	struct wlr_box box = {0};
	bool on = view_clip_box(view, &box);
	bool was_on = view->fx.clip_on;
	if (on != view->fx.clip_on || (on && !wlr_box_equal(&box, &view->fx.clip))) {
		/* 裁掉的 / 放出来的那一块得重画。场景那边只按节点几何记损伤，我们
		 * 裁的是同一块节点里的显示范围，它看不见 —— 这里自己补一笔。 */
		vidow_backdrop_damage_view_rect(view, NULL);
	}
	view->fx.clip_on = on;
	view->fx.clip = box;
	/* overview：**只裁画**（fx 那一份离屏缓冲），不裁表面树 ——
	 * wlr_scene_subsurface_tree_set_clip 连**输入**一起裁，裁了预览就命不中
	 * （test-overview 的「点某个窗口 = 选中它」当场红）。
	 *
	 * 以前 overview 是整条放行（不裁），于是条带比屏宽的那些列直接画到隔壁
	 * 显示器上 —— 用户：「overview 窗口太多会跨屏」。裁到 overview 自己那块屏
	 * 的工作区（ov->out_* 就是它的工作区，布局坐标，和 fx->clip 同一坐标系）。 */
	if (vidow_overview_active(view->server) &&
			view->mini == VIDOW_MINI_NONE && view->mapped) {
		struct vidow_overview *ov = &view->server->overview;
		if (ov->output != NULL && ov->out_w > 0 && ov->out_h > 0) {
			view->fx.clip_on = true;
			view->fx.clip = (struct wlr_box){.x = ov->out_x,
				.y = ov->out_y, .width = ov->out_w,
				.height = ov->out_h};
			/* 这条日志是给「overview 跨屏」这个 bug 留的判据锚点：overview
			 * 开着时每个窗口都该打一行，裁的范围就是 overview 那块屏的
			 * 工作区。看不见这行 = 这段没跑（裁剪没生效）。 */
			wlr_log(WLR_INFO,
				"overview clip: view %p 裁到 %d,%d %dx%d",
				(void *)view, ov->out_x, ov->out_y,
				ov->out_w, ov->out_h);
			return; /* 表面树那一份不裁：保住命中测试 */
		}
	}
	if (on != was_on) {
		/* 这是个**状态**（跟「藏起 / 显示」一样）：裁不裁决定客户端被叫去按哪块屏
		 * 的缩放渲染（wlroots 的 wlr_scene_surface 按「可见面积最大的那块输出」
		 * 发 preferred_buffer_scale），日志里要看得见。只在翻转时记一行。 */
		wlr_log(WLR_INFO, "clip: %s %s（屏框 %d,%d %dx%d，窗口 %d,%d %dx%d）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			on ? "裁到自己那块屏里" : "不裁", box.x, box.y, box.width,
			box.height, view->geom.x, view->geom.y, view->geom.width,
			view->geom.height);
	}

	/* 没装饰（着色器也关着）的时候，窗口就是客户端表面自己在画 —— 那一份也得
	 * 裁。
	 *
	 * 这一份要调 wlroots 的 wlr_scene_subsurface_tree_set_clip()，它**只认
	 * subsurface 树**，别的树上找不到就直接 assert（`found` failed，vidow 当场
	 * abort，踩过两次：X11 的对话框窗口、X11 的弹出物）。所以只给 Wayland 的
	 * xdg toplevel —— X11 / 弹出物那两份靠 fx 那块离屏缓冲（默认配置装饰开着，
	 * 窗口画的就是它）。
	 *
	 * 裁的是**显示**：截图源抓的是窗口自己那份镜像，不在这一棵树上。 */
	if (view->xdg_toplevel == NULL) {
		/* X11 / 弹出物：表面那一份裁不了（见上），记成「没裁过」—— 免得
		 * 以后哪天真拿到一棵能裁的树时，被一个过期的局部框挡掉。 */
		view->fx.clip_local = (struct wlr_box){0};
		return;
	}
	/* 这一帧画在哪儿（几何 + 动画偏移）：表面树挂在那个位置上。 */
	struct wlr_box at = vidow_view_screen_box(view);
	struct wlr_scene_node *node = view_surface_tree_node(view);
	if (node == NULL) {
		view->fx.clip_local = (struct wlr_box){0};
		return;
	}
	/* 装饰管线（fx）画这个窗口的时候，表面那一份是隐藏的（0 透明度，见
	 * view_anim_bake 里那次 set_opacity）—— 但**照样得裁**。
	 *
	 * 表面这一份的裁剪不只是"别把内容画到隔壁屏上"：wlroots 的
	 * wlr_scene_surface 会替我们发 preferred_buffer_scale（客户端按几倍渲染），
	 * 判据是「**可见面积最大**的那块输出的缩放」（见 wlr_scene.h 里
	 * wlr_scene_surface_create 的说明）。窗口滚出视口之后它的框跨在隔壁屏上，
	 * 不裁的话那边算出来的可见面积最大 —— 客户端就被叫去按隔壁那块屏的缩放
	 * 重画：2 倍屏上录 / 预览这个窗口，它一直按 1 倍糊着，滚回视口才清楚
	 * （用户报的「窗口滚动出去后，又按照另一个屏幕的缩放变化了」）。
	 *
	 * 上一版这里是「fx 开着就让开」—— 因为 set_clip 改的是 src_box，而装饰采源
	 * 缓冲的哪一块照的正是那个字段（fx_geo_tex_map），一裁就把裁剩下的一小条
	 * 拉伸铺满整块窗口（187）。现在那边改成认 view->fx.content_box（没裁过的
	 * 那一版，见 view_snapshot_content_box），裁剪和装饰就互不干扰了。 */
	if (on) {
		/* 缩放态（迷你 / 图标 / 保留原生 buffer 的自由小窗，也包括过渡动画那几
		 * 帧）**不走 wlroots 那套 clip**：它是「1:1 逻辑坐标」语义 ——
		 * types/scene/subsurface_tree.c 里把 clip 直接当节点位置用
		 * （`set_position(clip.x, clip.y)`）、src_box 只按**表面尺寸**比例缩，
		 * dst 一个像素都不动。缩放态下这三件事一起错：① 内容被挪到错的地方；
		 * ② 剩下那一小条被拉伸铺满整块 dst；③ 越界那半块**照画**（dst 没缩、
		 * 节点还在原处）—— 实测贴右边的图标漏到隔壁屏那 16×64 条带里 832 个
		 * 像素。这一档自己裁，见 view_scaled_crop()。 */
		if (vidow_view_hit_scale(view) != 1.0 &&
				view->mini != VIDOW_MINI_NONE &&
				view_scaled_crop(view, &box)) {
			view->fx.clip_local = (struct wlr_box){0};
			return;
		}
		/* 表面局部坐标（逻辑单位，wlroots 自己乘缓冲倍率）。原点取**这一帧
		 * 画在哪儿**（几何 + 动画偏移）：表面树挂在那个位置上，减布局目标
		 * 位置的话，滚动动画里裁出来的那块会跟着偏移。位置变了就得重调一次
		 * —— 这个函数每帧都跑（重烘那一帧开头），别只按 clip_on 判断。
		 *
		 * ⚠️ 必须**先和窗口自己的框求交**，再减 at：直接把整块屏框
		 * （box = 0,0 1280x720）减 at 会得到负数 x / 屏宽的宽（例如窗口在
		 * x=1000 时 local = -1000,0 1280x720），wlroots 拿它当 dst 尺寸，
		 * 表面就被摆到屏原点、宽 1280 —— 逻辑坐标上等于把窗口铺满整块屏，
		 * 右缘正好压在隔壁屏上，隔壁屏就 enter + 2 倍。用户报的「窗口超出
		 * 屏幕就碰到隔壁显示器的工作区、缩放反复跳」就是这一条。 */
		struct wlr_box visible;
		if (!wlr_box_intersection(&visible, &box, &at)) {
			/* 一点交集都没有：停车位那条路会把它挪走，这里别留半块。 */
			view->fx.clip_local = (struct wlr_box){0};
			wlr_scene_subsurface_tree_set_clip(node, NULL);
			return;
		}
		struct wlr_box local = {
			.x = visible.x - at.x, .y = visible.y - at.y,
			.width = visible.width, .height = visible.height,
		};
		if (!wlr_box_equal(&local, &view->fx.clip_local)) {
			/* 下刀之前先抄一份没裁过的有效区（撤销上一刀 → wlroots 重算
			 * src_box → 读出来 → 再补回去），装饰那边只认这一份。 */
			view_snapshot_content_box(view);
			view->fx.clip_local = local;
			wlr_scene_subsurface_tree_set_clip(node, &local);

		}
	} else if (view->fx.clip_local.width > 0) {
		view->fx.clip_local = (struct wlr_box){0};
		wlr_scene_subsurface_tree_set_clip(node, NULL);
	}
}

/* 裁剪状态一变，**强制**再钉一次客户端的 buffer_scale。
 *
 * wlroots 的 wlr_scene_surface 只在输出几何变化时重算「进了哪些输出」；我们
 * 用 set_clip 把节点缩到屏里，它不重算，之前因为跨到隔壁 2 倍屏而收到的 enter
 * 和 2 倍 preferred scale 就一直留着（客户端每 commit 一次都按 2 倍交，屏幕上
 * 1/2 反复跳）。裁剪改完主动发一次当前归属屏的倍率，把客户端拉回来。
 *
 * pref_scale 先清 0：view_pin_scale 平时只在值变化时发，这里要的就是「没变也
 * 再发一次」。 */
/* 让这个表面（和它的子表面）退出**不属于它工作区**的输出。
 *
 * wlroots 的 wlr_scene_surface 只在输出几何变化时重算「进了哪些输出」；我们用
 * set_clip 把节点缩进屏里，它不重算 —— 之前跨到隔壁 2 倍屏收到的 enter 一直留着，
 * 客户端就按 2 倍画（wlr_surface 的 preferred scale 取 current_outputs 里最大的）。
 * niri 也是自己管 enter/leave（workspace.rs 的 enter_output_for_window /
 * output_leave），不交给场景。 */
void vidow_view_apply_clip(struct vidow_view *view) {
	if (view == NULL) {
		return;
	}
	bool was_on = view->fx.clip_on;
	struct wlr_box was_clip = view->fx.clip;
	if (getenv("VIDOW_CLIP_TRACE") != NULL) {
		struct wlr_box cb = {0};
		bool con = view_clip_box(view, &cb);
		struct wlr_output *dragged = view_drag_output(view);
		struct wlr_output *home = view_home_output(view);
		struct wlr_output *owner = vidow_view_owner_output(view);
		struct wlr_box at = vidow_view_screen_box(view);
		wlr_log(WLR_INFO,
			"cliptrace view=%p owner=%s home=%s drag=%s clip_on=%d "
			"box=%d,%d %dx%d screen_box=%d,%d %dx%d floating=%d "
			"dragging=%d grab=%d",
			(void *)view,
			owner != NULL ? owner->name : "-",
			home != NULL ? home->name : "-",
			dragged != NULL ? dragged->name : "-",
			con ? 1 : 0, cb.x, cb.y, cb.width, cb.height,
			at.x, at.y, at.width, at.height,
			view->float_mode != VIDOW_FLOAT_NONE ? 1 : 0,
			view->dragging ? 1 : 0,
			view->server->grab_view == view ? 1 : 0);
	}
	view_apply_clip_impl(view);
	bool clip_changed = view->fx.clip_on != was_on ||
		!wlr_box_equal(&view->fx.clip, &was_clip);
	if (clip_changed) {
		view->pref_scale = 0;
	}
	view_sync_surface_outputs(view);
	view_pin_scale(view);
}

/* 补帧判定的迟滞帧数（见 vidow_view_render_pump 里的说明）。
 * view_on_screen() 依赖 view->geom / 工作区可见性，arrange 期间这些值会逐帧
 * 变化；把它当瞬时真值会让 render_pumping 每帧来回翻，每翻一次喂客户端一帧
 * frame_done。连续这么多帧结论一致才承认状态变化。 */
#define PUMP_HYSTERESIS 3

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
		/* 出帧策略跟 force_render 走：翻面了就接上（见 frames_recheck）。 */
		if (view->mini == VIDOW_MINI_ICON || view->mini_snapshot != NULL) {
			vidow_mini_frames_recheck(view);
		}
		bool want = view->mapped && !view->popup && view->output ==
			output->wlr_output &&
			(view->force_render || vidow_view_capture_held(view) ||
				/* 小窗（S3）：被停到屏外（图标暂停出帧那条路）/ 所在屏关了
				 * 的时候也要保活，否则游戏自己停帧，点开是一张死画面。
				 * 下面 view_on_screen() 为真时会 continue，所以这条只在
				 * 「真的没画到」时才生效 —— 正常情况一分钱不花。 */
				/* 但**走快照那条路的图标不算**（S5）：它就该停帧（屏上画的是
				 * 最后一帧快照），保活会把它又推活、和「静止的最后一帧」
				 * 自相矛盾。 */
				(view->mini != VIDOW_MINI_NONE &&
					!vidow_mini_uses_snapshot(view)));
		if (!want) {
			view->pump_hidden_frames = 0;
			view->pump_shown_frames = 0;
			if (view->render_pumping) {
				view->render_pumping = false;
				wlr_log(WLR_DEBUG, "强制渲染: %s 停（不再需要）",
					vidow_view_title(view) != NULL ?
						vidow_view_title(view) : "(无题)");
			}
			continue;
		}

		/* **先问「它是不是已经画在画面上了」**，再谈补帧。
		 *
		 * 这条顺序是有意义的：补帧本来是给「滚出视口 / 工作区被藏起来 / 最小化」
		 * 的窗口续命的，已经画在画面上的窗口由场景那边正常发 frame_done，根本
		 * 不该走这条路。老代码把 force_render 放在前面先放行，再在这里补一句
		 * 「在屏上就停」—— 等于每帧先进来再把 render_pumping 清掉，只要上面
		 * 的判定有任何抖动（arrange 期间 view->geom / 工作区会逐帧变），
		 * 状态就跟着每帧翻一次，翻一次喂客户端一帧 frame_done：全屏游戏被反复
		 * 唤醒重画，GPU 功耗异常（实测 85 次/秒的状态翻转）。
		 *
		 * 所以这里加**迟滞**：连续 PUMP_HYSTERESIS 帧「看不上屏」才真的开始补，
		 * 连续同样多帧「看得上屏」才停。抖动幅度小于这个窗口时状态不再翻转，
		 * 真实的状态变化（切工作区 / 最小化）只晚这几帧，用户感知不到。 */
		if (view_on_screen(server, output, view)) {
			view->pump_hidden_frames = 0;
			if (view->pump_shown_frames < PUMP_HYSTERESIS) {
				view->pump_shown_frames++;
			}
			if (view->render_pumping &&
					view->pump_shown_frames >= PUMP_HYSTERESIS) {
				view->render_pumping = false;
				wlr_log(WLR_DEBUG, "强制渲染: %s 停（它已经画在画面上）",
					vidow_view_title(view) != NULL ?
						vidow_view_title(view) : "(无题)");
			}
			/* 还在迟滞窗口里（刚被判定为在屏上，但还没连续够帧数）：不补帧，
			 * 也不改状态 —— 等下一次判定。 */
			continue;
		}
		view->pump_shown_frames = 0;
		if (view->pump_hidden_frames < PUMP_HYSTERESIS) {
			view->pump_hidden_frames++;
		}
		if (view->pump_hidden_frames < PUMP_HYSTERESIS) {
			continue; /* 刚看不上屏，还没连续够：先不动 */
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
			/* 「为什么它没画在画面上」说清楚：最小化 / 工作区被藏起来 / 滚出
			 * 视口 —— 三种的判据都是 view_on_screen()，这里只是把原因写出来。 */
			const char *why = view->minimized ? "它最小化了" :
				(!vidow_workspace_view_visible(server, view) ?
					"它那块工作区藏起来了" : "它滚出视口了");
			wlr_log(WLR_INFO, "强制渲染: %s %s，照样补帧回调"
				"（%s，%s）", vidow_view_title(view) != NULL ?
					vidow_view_title(view) : "(无题)", why,
				vidow_view_capture_held(view) ? "有人正在录屏 / 预览它" :
					"窗口规则要求", rate);
		}
	}

	/* 补帧得有人推：不主动排下一帧的话，画面没变化时 wlroots 就不给帧事件了。 */
	if (keep_going) {
		vidow_output_schedule_frame(output->server, output->wlr_output,
			"布局/视图改了");
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

/* 切工作区的过渡里「正被推开的那一块」上的窗口：它不属于现在显示着的工作区
 * （vidow_workspace_view_visible 为假），但过渡期间照样得画 —— 它正从屏上滑
 * 出去。滑到屏外之后 should_park 最后那条 !in_output_box 会把它停掉。
 *
 * 只放行「画」：可见性本身不含它，所以 pick-window / 切换器清单 / 录屏判据都
 * 不会被过渡里那半屏旧窗口带偏（test-ipc.sh 的「空工作区上 pick-window 是
 * null」就是这个坑）。 */
static bool view_sliding_out(struct vidow_server *server,
		const struct vidow_view *view) {
	if (view->ws == NULL) {
		return false;
	}
	/* 找**工作区住的那块屏**，不是 view->output 这个住址：热插拔 / 规则搬屏
	 * 的中间态里两者会短暂分叉，按住址找会漏掉正在滑走的那块，过渡里旧的
	 * 窗口突然消失。 */
	struct wlr_output *want = view->ws->output != NULL ?
		view->ws->output : view->output;
	if (want == NULL) {
		return false;
	}
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->wlr_output == want) {
			return output->slide_from == view->ws;
		}
	}
	return false;
}

/* 「它现在该停到所有输出框外面吗」—— 判据只有这一份：workspace.c 的可见性那一趟
 * 和 view_anim_place（每帧现算）都问它。
 *
 * 为什么每帧要现算、不能只在 arrange 里存一个标志：滚动 / 开窗的动画帧会自己摆
 * 位置（不经过 arrange），窗口从屏边界上扫过去的时候存下来的标志就过期了 ——
 * 摆回屏里之后还停在停车位上，那个窗口就再也不出现了。 */
bool vidow_view_should_park(struct vidow_server *server,
		struct vidow_view *view) {
	if (view->popup || view->scene_tree == NULL) {
		return false; /* 弹出物：各有各的路（见 workspace.c） */
	}
	/* 还没上屏的（从没 commit 过 buffer）：**也 park**。
	 *
	 * 原来这里还有 `!view->mapped` 一条，理由是「各有各的路」—— 但没上屏的
	 * 窗口根本没有路：它留在场景里、留在布局原点，每帧都要渲染一遍。实测一个
	 * 会话里 41 个 view 有 40 个是这种（vivaldi 一个就开一堆子表面），全都
	 * 在画，于是**所有应用都卡**（桌面是静态背景，不重画，所以不卡）。
	 * 第一次 map 时 view_map 会调 vidow_view_set_parked(false) 放回来。 */
	if (!view->mapped) {
		return true;
	}
	/* 小窗（S5）：图标态 + 该窗口不保活 + 手里有最后一帧 → 真实节点停到停车位
	 * （它不在任何输出上，客户端于是停帧），屏上由快照节点画（见
	 * mini_snapshot_place）—— 这就是「图标是静止的最后一帧」那条路。 */
	if (vidow_mini_uses_snapshot(view)) {
		return true;
	}
	if (!vidow_workspace_view_visible(server, view) &&
			!view_sliding_out(server, view)) {
		/* overview 里**这块屏上别的工作区也要画**（它们上下排成一列，见
		 * overview.c）—— 平时「藏起来的工作区不画」，全景里得放回来，不然
		 * 那一行是空的。最小化的照旧不画（它连列里都不占位置，摆不出坐标）；
		 * 别的屏上的照旧停着（全景只看一块屏）。 */
		bool in_overview = vidow_overview_active(server) &&
			view->output == server->overview.output && !view->minimized;
		if (!in_overview) {
			return true; /* 最小化 / 工作区被藏起来：也不画 */
		}
	}
	if (vidow_overview_active(server)) {
		return false; /* overview 全缩进画面 */
	}
	/* 浮窗 / **正被拖的**窗口允许跨屏，照旧画。
	 *
	 * 为什么要放行：拖拽的时候窗口就是该跟着光标走到隔壁屏上 —— 用户
	 * 2026-10-20 的原话「拖拽窗口的时候应该允许显示在另一个屏幕」。不放行的
	 * 话几何框一越过屏边界就整块停到停车位，屏上表现为「拖到边上窗口就没了」，
	 * 跨屏拖动直接抄作不出来。
	 *
	 * 「正在拖」的两个来源都得认：平铺窗口拖拽走 drag.c（view->dragging），
	 * 浮窗拖动 / 列宽拖动走 input.c 那条抓取（server->grab_view）。只看前者
	 * 漏掉浮窗，只看后者漏掉还没起拖阈值的平铺窗口。 */
	if (view->float_mode != VIDOW_FLOAT_NONE || view->dragging ||
			server->grab_view == view) {
		return false;
	}
	/* 其余窗口（有工作区的）只在自己的屏里画：越界就停到停车位，别让几何框
	 * 落进隔壁屏的坐标范围（否则场景图是全局坐标，会画到隔壁显示器上）。 */
	return !vidow_view_in_output_box(server, view);
}

/* 停到屏外 / 放回屏里。**只有这一个入口**改这个标志：日志和损伤都在这儿 ——
 * 「藏起 / 显示」这行状态是给人和测试看「它现在是什么样」的，所以每次翻转都得
 * 写一行（view_map 里那次翻转也算）。 */
void vidow_view_set_parked(struct vidow_view *view, bool park) {
	if (view->scene_parked == park) {
		return;
	}
	view->scene_parked = park;
	/* 它自己进/出画面了：底衬的合成结果变了。账记在它头上（它自己的私有底衬
	 * 不受影响），别的窗口因为世界代次前进而重抓。 */
	vidow_backdrop_damage_view(view);
wlr_log(WLR_DEBUG, "workspace: %s %s（%s）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
		park ? "藏起（停到屏外）" : "显示",
		view->ws != NULL ? view->ws->name : "-");
}

/* 停到哪儿：所有输出框外面的「停车位」—— 左上角再往外挪 64K，按窗口编号横着
 * 排开（一格 16384 逻辑像素，够 16K 宽的窗口）。为什么按编号排开：按窗口录屏的
 * 虚拟输出框就是节点自己那块框，两个停着的窗口挨在一起的话，录一个会连旁边那个
 * 一起录进去。 */
static void view_park_pos(const struct vidow_view *view, int *px, int *py) {
	struct wlr_box all = {0};
	wlr_output_layout_get_box(view->server->output_layout, NULL, &all);
	if (all.width < 1 || all.height < 1) {
		all.x = 0;
		all.y = 0;
	}
	int slot = (int)(view->id % 64);
	*px = all.x - 65536 - slot * 16384;
	*py = all.y - 65536;
}

/* overview 里这个窗口的透明度：别的屏上的淡出（老规矩）、**同一块屏上别的工作区**
 * 的跟着 t 淡入淡出 —— 它们平时不画，全景里才露出来，不这么办进出时会「啪」地
 * 冒出来 / 消失。摆位（view_anim_place）和重烘（vidow_view_anim_bake）必须算出
 * 同一个值，所以只有这一份。 */
static double view_overview_alpha(struct vidow_server *server,
		struct vidow_view *view) {
	double alpha = view->anim_alpha;
	if (server->overview.t <= 0.0 || server->overview.output == NULL) {
		return alpha;
	}
	if (view->output != server->overview.output) {
		return alpha * (1.0 - server->overview.t);
	}
	/* overview 里**不做整体隐藏**：溢出工作区的部分由 fx.c 的 ov_clip 裁掉
	 * （它把这块窗口裁到「这一行的矩形」，也就是屏幕上可见的那一块）。
	 *
	 * 这里以前有一段「整块落在行矩形外就 alpha = 0」的隐藏，是 fx.c 还没有
	 * ov_clip 时用来挡「overview 窗口太多会跨屏」的。现在裁剪已经有了，那层
	 * 隐藏就成了多余而且有害的：判据（行矩形）和裁剪框（整块屏）不是同一个
	 * 东西，落在行间隙里、本来该被**裁一半**显示的窗口被整个藏掉了 ——
	 * 用户要的是「所有窗口都显示，超出的部分由显示器裁」。 */
	if (!vidow_workspace_view_visible(server, view)) {
		return alpha * server->overview.t;
	}
	return alpha;
}

/* 小窗（mini window，S3）：内容的**原生**尺寸（场景里的 1:1 尺寸）。X11 那条在
 * commit 时记过 base_dst（已经除过 xwayland_scale），没有就取表面自己的逻辑尺寸
 * —— 两者都是「没被小窗缩过」的量，所以每帧算出来稳定（拿节点现在的 dst 算就会
 * 越缩越小）。 */
static void view_mini_native(const struct vidow_view *view, int *w, int *h) {
	int nw = view->base_dst_w, nh = view->base_dst_h;
	/* xdg（非 X11）：表面的逻辑尺寸**就是**场景里的 1:1 尺寸 —— 小窗 / 图标态
	 * 不下发 configure（保留原生 buffer，见 vidow_view_set_size），客户端那一份
	 * 一直没变，随时读都是当前值。这里优先读它，别读 base_dst_*：那个字段 xdg
	 * 只在小窗退出那一刻记一次（commit 那条路不记），输出缩放一变、工作区逻辑
	 * 尺寸跟着变、客户端被重新 configure 成另一个大小之后就永久过期了 ——
	 * 重进小窗时 contain 系数按过期的原生尺寸算，盒里内容只有该有的一半
	 * （实测：172x190 物理脚印变成 84x94；从小窗态里改 scale 那条不重进，反而
	 * 是对的，因为 base_dst_* 还是 0、走的正是这个兜底）。 */
	if (view->kind != VIDOW_VIEW_X11) {
		struct wlr_surface *live = vidow_view_surface(view);
		if (live != NULL && live->current.width > 0 &&
				live->current.height > 0) {
			*w = live->current.width;
			*h = live->current.height;
			return;
		}
	}
	if (nw <= 0 || nh <= 0) {
		struct wlr_surface *surface = vidow_view_surface(view);
		nw = surface != NULL ? surface->current.width : 0;
		nh = surface != NULL ? surface->current.height : 0;
	}
	*w = nw > 0 ? nw : 0;
	*h = nh > 0 ? nh : 0;
}

/* 小窗（mini window，S3）：把原生内容缩到小窗盒的系数。1.0 = 不缩。
 *
 * contain（取宽高比里小的那个）：内容整个塞得进盒子、按比例不拉伸。宽高比策略
 * 本身（mini_aspect / PAspect / 拖拽锁比例）是 S3b 的事，这里只保证「缩了、没
 * 拉伸」。盒子比内容大时按比例放大 —— 图标是固定边长，小窗口也该填满它。
 *
 * 关掉保留（mini_preserve_buffer = false）时恒为 1.0：那条路是直接把客户端
 * configure 成小窗尺寸，场景节点不动。 */
static double view_mini_scale(const struct vidow_view *view) {
	if (view->mini == VIDOW_MINI_NONE || view->float_geom.width <= 0 ||
			view->float_geom.height <= 0 ||
			!vidow_mini_preserve_buffer(view)) {
		return 1.0;
	}
	int nw = 0, nh = 0;
	view_mini_native(view, &nw, &nh);
	if (nw <= 0 || nh <= 0) {
		return 1.0;
	}
	double sx = (double)view->float_geom.width / (double)nw;
	double sy = (double)view->float_geom.height / (double)nh;
	double s = sx < sy ? sx : sy;
	return s > 0.0 ? s : 1.0;
}

/* 给小窗的过渡动画用（float.c 的 mini_transition_start / vidow_mini_set_state）：
 * 这一帧内容画出来的缩放、以及内容原点相对盒子原点的居中偏移。必须和
 * view_anim_place 里那两处（view_mini_scale 与 mini_cx/mini_cy）同源 —— 补间
 * 的起点是「上一帧画在哪」，拿别的量推出来就会在转换的第一帧跳一下（用户报的
 * 「退出小窗的一瞬间显示原始大小」就是这个跳）。 */
double vidow_view_mini_scale_export(const struct vidow_view *view) {
	return view_mini_scale(view);
}

void vidow_view_mini_content_offset(const struct vidow_view *view, int *dx,
		int *dy) {
	if (dx != NULL) {
		*dx = 0;
	}
	if (dy != NULL) {
		*dy = 0;
	}
	if (view == NULL) {
		return;
	}
	double ms = view_mini_scale(view);
	if (ms == 1.0 || view->float_geom.width <= 0 ||
			view->float_geom.height <= 0) {
		return;
	}
	/* 几何取**可见窗口**（和 view_anim_bake 的 mini_cx/mini_cy 同一个来源）：
	 * 装饰那一条路画的就是它。 */
	struct vidow_fx_geo geo = vidow_view_fx_geo(view);
	int cw = geo.w, ch = geo.h;
	if (cw <= 0 || ch <= 0) {
		view_mini_native(view, &cw, &ch);
	}
	if (cw <= 0 || ch <= 0) {
		return;
	}
	if (dx != NULL) {
		*dx = (int)lround(((double)view->float_geom.width -
			(double)cw * ms) / 2.0);
	}
	if (dy != NULL) {
		*dy = (int)lround(((double)view->float_geom.height -
			(double)ch * ms) / 2.0);
	}
}

/* 「摆在哪」落到场景节点上：位置 / 透明度 / overview 的显示尺寸 / 装饰的采样
 * 偏移和 ext-background-effect 的区域。**便宜**，拖动跟手时每个指针事件都走这条。
 * 装饰图的重烘（贵：整块窗口那么大的离屏渲染）在 vidow_view_anim_bake 里，一帧
 * 只做一次（见 vidow_tile_grab_update / output.c 的帧回调）。 */
static void view_anim_place(struct vidow_view *view) {
	/* 场景节点可能已经没了：X11 销毁窗口时先摘掉表面（scene_tree 归 NULL）再撤
	 * 动画，而撤销动画会回调 view_open_done() 回到这里 —— 以前就死在这行上。 */
	if (view->scene_tree == NULL) {
		return;
	}
	/* 表面进哪块输出：换工作区 / 换屏 / 刚 map，都在这里跟过去。 */
	view_sync_surface_outputs(view);

	/* overview 里：位置走「工作区局部 -> 屏幕」变换（每块工作区一行，见
	 * overview.c），内容尺寸乘上缩放；不在这块屏上 / 不在现在这块工作区里的
	 * 窗口淡出。 */
	struct vidow_server *server = view->server;
	double ov = vidow_overview_scale(server);
	double alpha = view_overview_alpha(server, view);
	int x = 0, y = 0;
	double sx_mapped = 0, sy_mapped = 0;
	vidow_overview_map(server, view, view->anim_off_x, view->anim_off_y,
		&sx_mapped, &sy_mapped);
	x = (int)lround(sx_mapped);
	y = (int)lround(sy_mapped);

	/* 停车：滚出自己那块屏 / 工作区藏起来的窗口不关节点，改成停到所有输出框外面
	 * （见 server.h 的 scene_parked）。位置只有这一个出口 —— 藏不藏、摆哪儿都在
	 * 这儿定，别在别处再 wlr_scene_node_set_position 一次。判据每帧现算（动画帧
	 * 里窗口会扫过屏边界，存下来的标志会过期）。 */
	vidow_view_set_parked(view, vidow_view_should_park(server, view));
	/* 小窗（S5）：图标态走快照那条路时，屏上那块由快照节点画 —— 位置取
	 * 「图标画在哪」（geom + anim_off，和下面 park 用的是同一个来源），
	 * 不是停车位。 */
	if (view->mini == VIDOW_MINI_ICON || view->mini_snapshot != NULL) {
		mini_snapshot_place(view);
	}
	if (view->scene_parked) {
		view_park_pos(view, &x, &y);
	}

	wlr_scene_node_set_position(&view->scene_tree->node, x, y);
	vidow_fx_set_alpha(&view->scene_tree->node, alpha);

	/* 客户端该按几倍渲染也在这儿钉（跟位置一起，每帧一次、只在真的变了时发）：
	 * 停到停车位之后它就不在任何输出上了，不钉的话客户端会掉回 1 倍。 */
	view_pin_scale(view);

	/* 小窗（mini window，S3）：保留原生分辨率时，把场景节点缩到小窗盒 ——
	 * 客户端 buffer 一个像素都不动（不 configure、不重建 swapchain，见
	 * float.c 的 on_commit / view.c 的 vidow_view_set_size）。
	 *
	 * **必须放在下面那个 fx.active 判断之前**：装饰全关（fx 不 active）正是
	 * 「全屏游戏进小窗」最可能的状态，写在里面就等于不生效。
	 *
	 * 为什么在这儿设得住：wlroots 每次 commit 都会把 dst 重置成表面自己的尺寸
	 * （types/scene/surface.c 的 surface_reconfigure），而我们的 commit 监听器
	 * 排在它的后面（xdg：wlr_scene_xdg_surface_create 先注册；X11：见
	 * x11_attach_surface 的顺序），所以这里设的值当帧就留下 —— 但**每个 commit
	 * 都得重设一次**，view_commit 那边因此对迷你态也调 apply_anim。 */
	if (view->scene_surface != NULL && view->scene_surface->buffer != NULL) {
		struct wlr_scene_buffer *mini_src = view->scene_surface->buffer;
		double ms = view_mini_scale(view);
		if (ms != 1.0) {
			int nw = 0, nh = 0;
			view_mini_native(view, &nw, &nh);
			int dw = (int)lround((double)nw * ms);
			int dh = (int)lround((double)nh * ms);
			if (dw < 1) {
				dw = 1;
			}
			if (dh < 1) {
				dh = 1;
			}
			if (!view->mini_scaled) {
				/* 记下「还没居中之前内容节点在它父节点里的位置」：xdg 的窗口
				 * 几何留白（CSD 阴影那圈）有可能是 wlroots 设在这一层的，退出
				 * 缩放态时要还原成它。 */
				view->mini_base_dx = mini_src->node.x;
				view->mini_base_dy = mini_src->node.y;
			}
			wlr_scene_buffer_set_dest_size(mini_src, dw, dh);
			/* S3b：内容在盒子里**居中** —— contain 留出来的信件箱两边平分。
			 * 以前是左上对齐（内容贴着盒子左上角，留白全堆在右下），FREE 盒
			 * 里「看着是内容中间」的那一点其实落在留白上，点不着。
			 *
			 * 相对的是 scene_tree（盒子原点），所以要把父节点那一层的偏移
			 * （xdg 的窗口几何）减掉：两种留白挂在哪一层都算得对。
			 *
			 * 每帧重设（跟 dst 一样）：wlroots 每次 commit 会把节点位置按自己
			 * 的规则重算一遍，我们排在它的监听器后面，重设才留得住。 */
			int vx = 0, vy = 0, px = 0, py = 0;
			wlr_scene_node_coords(&view->scene_tree->node, &vx, &vy);
			if (mini_src->node.parent != NULL) {
				wlr_scene_node_coords(&mini_src->node.parent->node, &px, &py);
			}
			int off_x = view->mini_base_dx +
				(view->float_geom.width - dw) / 2 - (px - vx);
			int off_y = view->mini_base_dy +
				(view->float_geom.height - dh) / 2 - (py - vy);
			wlr_scene_node_set_position(&mini_src->node, off_x, off_y);
			view->mini_scaled = true;
		} else if (view->mini_scaled || view->mini != VIDOW_MINI_NONE) {
			/* 退出缩放态，**或者**本来就不缩放的小窗态（FREE：保留 buffer 不再
			 * 管它，客户端已经被 configure 成盒子大小，内容是 1:1 的）。
			 *
			 * 第二种情形必须也钉一遍：wlroots 每次 commit 都会把 dst 重算成
			 * 「缓冲像素 ÷ 缓冲缩放」，而那个值不一定等于表面自己的逻辑尺寸
			 * —— 实测 FREE 里一个逻辑 544x355 的表面被设成 **277x355**。屏上
			 * 画的是装饰图（fx 那条路）所以看着没错，但**命中测试认的是内容
			 * 节点**（wlroots 的 scene_node_get_size + 表面输入区域）⇒ 半个
			 * 窗口「看得见、点不着」：实测点盒心 (1018,167) 命中不到窗口、命中
			 * 区只剩 x∈[746,1023]（= 277 宽），而盒子是 766..1270。
			 *
			 * 位置只在**离开缩放态**时才撤居中偏移；FREE 这种本来就 1:1 的，
			 * 位置由 wlroots 按窗口几何留白（CSD 阴影）摆，别去动它。 */
			bool was_scaled = view->mini_scaled;
			view->mini_scaled = false;
			if (was_scaled) {
				/* 居中偏移一起撤掉（位置还原成进缩放态之前那一版）。 */
				wlr_scene_node_set_position(&mini_src->node,
					view->mini_base_dx, view->mini_base_dy);
			}
			if (view->kind == VIDOW_VIEW_X11) {
				vidow_view_x11_scale(view);
			} else {
				/* 钉成**表面自己的逻辑尺寸**（wlroots 下一次 commit 也会设
				 * 成这个值）—— 不用 0：0 在 wlroots 那边是「按缓冲像素当场景
				 * 单位」，2 倍缓冲的客户端会被先画大一倍，而且它下一次 commit
				 * 之前一直错（窗口不再交帧的话就一直错下去）。 */
				struct wlr_surface *surface = vidow_view_surface(view);
				if (surface != NULL && surface->current.width > 0 &&
						surface->current.height > 0) {
					wlr_scene_buffer_set_dest_size(mini_src,
						surface->current.width, surface->current.height);
				} else if (was_scaled) {
					wlr_scene_buffer_set_dest_size(mini_src, 0, 0);
				}
			}
			view_dst_capture(view);
		}
	}


	if (!view->fx.active || view->scene_surface == NULL) {
		return;
	}
	struct wlr_scene_buffer *src = view->scene_surface->buffer;
	if (src == NULL) {
		return;
	}
	/* overview 里**内容节点的盒子就是命中盒**：wlroots 的 scene_node_get_size()
	 * 拿 dst_width/dst_height 当节点盒子，wlr_scene_surface 再按表面尺寸卡一遍
	 * 边界 —— 所以这个盒子必须跟着缩放走，不然全景里点谁都不是谁（实测 3 列时
	 * 最后一个窗口的盒子从 646 一路伸到 1291，把整排吞掉）。src_box 不是原因：
	 * 它一直是整块缓冲（实测 0,0 665x740）。
	 *
	 * 1:1 尺寸走 view_mini_native()，不走 base_dst_*：那个字段 xdg 从来不记
	 * （只在 X11 的 commit 和「退出小窗」两处写），恒为 0，下面这个守卫根本进不来
	 * —— 这正是「xdg 窗口在全景里内容尺寸一次都没缩过」的原因。view_mini_native()
	 * 是唯一一处按 kind 分派的 1:1 尺寸来源（小窗那套缩放用的也是它）：非 X11 读
	 * 表面当前逻辑尺寸（随时读都是当前值），X11 读 base_dst_*（已除过
	 * xwayland_scale）—— X11 这边行为因此一个字没变。
	 *
	 * 偏移（内容节点在它父树里的位置 = xdg 的 CSD 留白）也要一起缩：只缩尺寸不缩
	 * 偏移，盒子会整体往左上偏 ~20px、右边还盖不满格子（实测右侧差 2px、左侧多出
	 * 20px 伸进两列之间的缝里）。X11 的窗口几何 x/y 是 0，这条对它天然是空操作。
	 *
	 * 小窗（MINI / ICON）不进这一支：那两态的**内容节点几何是小窗那套机制管的**
	 * （min 尺寸按 mini 缩放、位置在盒子居中，见上面 mini_scaled 那段 + 图标快照），
	 * 而且小窗不参与行布局、全景里不重映射（vidow_overview_map 对它原样返回）——
	 * 照这里乘 ov 会把盒子撑到 306x340，比小窗那块卡片还高，白盖掉旁边一列的
	 * 一条（实测 14px 宽、160px 高）。X11 那一支保持原样：它读的就是 base_dst_*，
	 * 和小窗态的既有行为一致。 */
	int nw = 0, nh = 0;
	view_mini_native(view, &nw, &nh);
	if (ov != 1.0 && nw > 0 && nh > 0 &&
			(view->mini == VIDOW_MINI_NONE || view->kind == VIDOW_VIEW_X11) &&
			view->mini_snapshot == NULL) {
		int dw = (int)lround(nw * ov);
		int dh = (int)lround(nh * ov);
		wlr_scene_buffer_set_dest_size(src, dw > 0 ? dw : 1, dh > 0 ? dh : 1);
		if (view->kind != VIDOW_VIEW_X11) {
			/* 目标是「内容节点在场景里的绝对位置 = 窗口矩形原点 - 留白 ×
			 * 缩放」—— 留白（xdg 的窗口几何偏移）不一定挂在缓冲节点自己身上：
			 * 它是 wlroots 设在这一层还是父树那一层，得减掉父节点的偏移才算
			 * 得对（和上面小窗居中那段同一个算法）。只按「节点自己的 x」设就
			 * 会在父树再叠一份 -20px。 */
			struct wlr_box geo = vidow_view_geometry(view);
			int vx = 0, vy = 0, px = 0, py = 0;
			wlr_scene_node_coords(&view->scene_tree->node, &vx, &vy);
			if (src->node.parent != NULL) {
				wlr_scene_node_coords(&src->node.parent->node, &px, &py);
			}
			wlr_scene_node_set_position(&src->node,
				(int)lround((double)vx - (double)geo.x * ov) - px,
				(int)lround((double)vy - (double)geo.y * ov) - py);
		}
	}
	int vx = 0, vy = 0, sx = 0, sy = 0;
	wlr_scene_node_coords(&view->scene_tree->node, &vx, &vy);
	wlr_scene_node_coords(&src->node, &sx, &sy);
	view->fx_dx = sx - vx;
	view->fx_dy = sy - vy;
}

/* 只摆位置（给拖动跟手那条路用，见 drag.c）。 */
void vidow_view_apply_anim_place(struct vidow_view *view) {
	view_anim_place(view);
}

/* 全屏直通：这一帧这个窗口能不能整块交给场景直画（不烘装饰那趟离屏渲染），让
 * wlroots 有机会把客户端的缓冲直接扫上屏（判据见 fx.c 的 vidow_view_scanout_ok，
 * wlroots 那一侧的要求见 wlr_scene.c 的 scene_entry_try_direct_scanout）。
 *
 * 返回 true = 这一帧按直通处理，调用方什么都不用做了。
 *
 * 为什么来回切换看不出变化：判据成立的时候「合成出来的像素」就是「客户端缓冲
 * 本身」（装饰参数全零、内容整块不透明、不裁），所以
 *   * 进直通：先把真实表面露出来 —— 内容一样，装饰节点这一帧还盖在上面，看不出
 *     变化；同一帧把装饰节点收掉。
 *   * 退直通：先把装饰那条路接回来，下面照常烘一帧，**烘成了才**把表面藏起来
 *     （见函数尾那句 set_opacity），中间没有任何一帧是空的。
 * 两个方向都会让场景节点自己报损伤（改透明度 / 开关节点都算整块），全屏窗口那
 * 一块就是整块输出 —— 屏幕上不会留下上一帧的残影（那是「跳一下」的来源）。 */
static bool view_scanout_switch(struct vidow_view *view) {
	bool ok = vidow_view_scanout_ok(view);
	if (ok == view->fx.scanout) {
		return ok;
	}
	view->fx.scanout = ok;
	if (ok) {
		if (view->scene_surface != NULL &&
				view->scene_surface->buffer != NULL) {
			wlr_scene_buffer_set_opacity(view->scene_surface->buffer, 1.0f);
		}
		vidow_fx_end(view->server, &view->fx);
		wlr_log(WLR_INFO, "scanout: %s 交给场景直画（全屏 + 装饰全零）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?");
	} else {
		/* 装饰那条路接回来：和 view_open_done / view_swap_decor 一个套路 ——
		 * 装饰开着就重建 fx 节点（自定义的 idle shader 再挂上）。 */
		struct vidow_config *cfg = &view->server->config;
		view->fx.view = view; /* 裁剪要按窗口算（见 struct vidow_fx_target） */
		if (vidow_fx_begin(view->server, &view->fx, view->scene_tree, NULL) &&
				cfg->shader_idle[0] != '\0') {
			vidow_fx_set_shader(view->server, &view->fx, cfg->shader_idle,
				VIDOW_SHADER_IDLE);
			view->fx.progress = 1.0;
		}
		wlr_log(WLR_INFO, "scanout: %s 回到合成（装饰 / 模糊 / 动画又要了）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?");
	}
	return ok;
}

/* 重烘这个窗口的装饰图（含底衬采样）。贵，一帧一次。位置先用 view_anim_place
 * 摆好；这里把同一套位置参数再算一遍（几个整数，跟重烘比起来不算什么）。 */
void vidow_view_anim_bake(struct vidow_view *view) {
	if (view->scene_tree == NULL) {
		return;
	}
	/* 裁不裁、裁到哪儿，**这一帧**就得是最新的：重烘（下面 vidow_fx_frame）
	 * 把裁剪算进场景节点里，烘完再改就晚了一帧 —— 那一帧里越界的那一块照样
	 * 画在隔壁屏上（而且静止下来之后再也没有下一帧来纠正）。 */
	vidow_view_apply_clip(view);
	/* 直通那一路：不烘，整块交给场景（见上）。 */
	if (view_scanout_switch(view)) {
		return;
	}
	struct vidow_server *server = view->server;
	double ov = vidow_overview_scale(server);
	double alpha = view_overview_alpha(server, view);
	int x = 0, y = 0;
	/* 摆位那份（view_anim_place）怎么算的，这里就得怎么算：重烘的位置和缩放
	 * 必须跟场景节点上那套一致，差一个像素就是糊 / 错位。 */
	double sx_mapped = 0, sy_mapped = 0;
	vidow_overview_map(server, view, view->anim_off_x, view->anim_off_y,
		&sx_mapped, &sy_mapped);
	x = (int)lround(sx_mapped);
	y = (int)lround(sy_mapped);

	if (!view->fx.active || view->scene_surface == NULL) {
		return;
	}
	struct wlr_scene_buffer *src = view->scene_surface->buffer;
	if (src == NULL) {
		return;
	}
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
	view->fx.view = view;
	view->fx.anim_scale = view->anim_scale;
	/* 小窗（mini window，S3）：装饰那条路也得跟着缩 —— 烘图那块画布是按
	 * 「可见窗口几何 × 倍率」开的，倍率里不含小窗系数的话，图标里的窗口还是
	 * 原生那么大一块（装饰节点盖在缩过的表面节点上，看着就是没缩）。
	 *
	 * 复用 overview 的那个 ov：它同时决定「画布开多大」和「节点画多大」，两个
	 * 一起乘上小窗系数，结果正好是「内容按比例缩到小窗盒」。out_scale（设备像素
	 * / 逻辑像素）= scale / ov 里两者同乘一个系数，所以输出倍率一点没变。
	 * 副作用只有一个：ov < 0.98 会关掉这一次渲染的底衬模糊 —— 缩到几十像素的
	 * 小窗本来也不该叠一层按原尺寸算的模糊（overview / MRU 缩略图同理）。 */
	double fx_ov = ov * view_mini_scale(view);
	/* 小窗（S3b）：装饰那条路也要**居中**。装饰节点画的是「内容」这一块
	 * （节点原点 = 内容左上角，见 fx.c 的 node_x = x - pad_ov），x/y 传的就是
	 * 内容相对窗口树原点的偏移 —— 留白两边平分才和表面那条路一致（不传的话
	 * 装饰会把内容画在盒子左上角，表面节点缩在中间也看不见）。
	 *
	 * 只在保留原生分辨率（mini_scaled）时有非零值：那正是盒子比内容大的时候。 */
	int mini_cx = 0, mini_cy = 0;
	if (view->mini_scaled) {
		mini_cx = (int)lround(((double)view->float_geom.width -
			(double)geo.w * fx_ov) / 2.0);
		mini_cy = (int)lround(((double)view->float_geom.height -
			(double)geo.h * fx_ov) / 2.0);
	}
	int64_t bake_t0 = vidow_now_ns();
	/* 这里以前有一行每次重烘就打的 DEBUG（node/geom/screen_box），日常使用是
	 * 每帧 × 每个在动的窗口一行 —— 排查摆放错位时临时加回来，别常驻。 */
	bool fx_drawn = vidow_fx_frame(view->server, &view->fx, src, &geo,
		mini_cx, mini_cy, x, y, alpha, view->server->focused == view,
		view->fullscreen, view_scale(view) * fx_ov, fx_ov, &view->rule_eff);
	/* 探针：整块窗口那么大的离屏渲染一共多少钱（见统计行的「重烘」；次数由
	 * vidow_fx_frame 的 vidow_stats.bakes 记）。拖动 / 滚动动画里每个在动的
	 * 窗口每帧都要烘一次，所以这项随窗口数线性涨。 */
	vidow_stats.bake_ns += vidow_now_ns() - bake_t0;
	/* 装饰图真的画上屏了才把真实表面藏起来（只留给命中测试）。没画出来
	 * 还藏就是窗口整个消失 —— 关掉 decor/shader 时踩过，屏幕一片黑。
	 * 拖着果冻的时候也一样：内容归 jelly.c 那个节点画。
	 *
	 * 这里**不要**再加「等开窗动画 alpha 走满才藏」之类的条件：真实表面是
	 * 一张直角矩形，装饰节点才有圆角和模糊底衬。半透明阶段把它留在上面，
	 * 方角就会盖住圆角、底衬也被这块不透明的表面挡掉 —— 表现就是 SSD 圆角
	 * 裁不出来、窗口背后的模糊变成错的（回滚过一次，就是这么坏的）。 */
	wlr_scene_buffer_set_opacity(src,
		(fx_drawn || view->mask_opacity || view->jelly != NULL) ? 0.0f : 1.0f);
}

void vidow_view_apply_anim(struct vidow_view *view) {
	/* Commits, placement and animation updates invalidate every output instance.
	 * The per-output refresh loop itself must not advance this revision. */
	view->fx.bake_revision++;
	view_anim_place(view);
	vidow_view_anim_bake(view);
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
	/* 滑动两端都算损伤：漏了老位置，别的窗口底衬里会留着它上一帧的残影。 */
	struct wlr_box rest = view->geom;
	view->anim_off_x = view->anim_from_x * (1.0 - t);
	view->anim_off_y = view->anim_from_y * (1.0 - t);
	vidow_backdrop_damage_box(view->server, &rest);
	vidow_backdrop_damage_view(view); /* 只有它自己在动 */
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
	/* 开窗动画（缩放 + 滑入）：静止那一块（缩放 1、偏移 0）也算损伤。 */
	{
		struct wlr_box rest = view->geom;
		vidow_backdrop_damage_box(view->server, &rest);
	}
	vidow_backdrop_damage_view(view); /* 只有它自己在动 */
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
	struct vidow_rule_anim anim;
	vidow_rule_anim(cfg, &view->rule_eff, false, &anim);

	/* 弹出物（X11 菜单 / 提示 / 下拉框）和 Wayland 的 xdg_popup 一样：不装饰、
	 * 不跑 shader、也不做入场动画。它是跟着操作「啪」地出现的一块小图，滑动
	 * 或淡入都显得慢半拍；而且它常在自己的位置上重开，动画会看着像窗口在飘。
	 * 这里只把场景节点摆到最终位置。
	 * 窗口规则 anim = false / open_ms = 0 是同一回事，只不过由用户点名
	 * （终端那类「开出来就该在」的窗口）。
	 *
	 * 但**装饰 / 模糊那条路（fx）照样得建**：窗口的背景模糊和边框都挂在 fx
	 * 节点上，不建 fx 这个窗口就永远没有模糊、也没有边框 —— 「不跑动画」不是
	 * 「不要 fx」。所以这里走 view_open_done()（它做的正是「没有入场动画的开窗
	 * 收尾」：换 idle shader、摆到位），而不是把窗口晾在裸表面状态。
	 * （`window_open_ms = 0` 一直走的就是这条路，test-blurpartial.sh 拿它当
	 * 默认配置跑；规则写 anim = false 之前会跳过 fx，等于顺手把模糊关了。） */
	if (view->popup || anim.open_ms <= 0) {
		vidow_anim_cancel(view->anim_open);
		view->anim_open = NULL;
		view->anim_alpha = 1.0;
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		view->anim_scale = 1.0;
		if (view->popup) {
			vidow_view_apply_anim(view);
			return;
		}
		vidow_fx_begin(view->server, &view->fx, view->scene_tree,
			cfg->shader_open);
		view_open_done(view, true);
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
		anim.open_ms, cfg->open_easing, view_open_update,
		view_open_done, view);
	if (view->anim_open == NULL) {
		view_open_done(view, true);
	}
}

/* The surface is gone by the time we get here, so the close animation runs on
 * a snapshot of the last committed buffer. */
static void view_snapshot_close(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	struct vidow_rule_anim anim;
	vidow_rule_anim(&server->config, &view->rule_eff, false, &anim);
	/* 弹出物不做关闭动画：菜单收起来就是收起来了（它常常在关掉的同一帧里被
	 * 重开或换成另一个菜单，淡出的残影会串到下一个菜单上）。 */
	if (view->popup || view->last_buffer == NULL || anim.close_ms <= 0) {
		/* 「怎么没有关闭动画」是收到过的反馈，而这里有三条不同的路都能让它
		 * 不出现（弹出物 / 没有最后一帧快照 / 时长是 0）。日志里说清楚是哪条，
		 * 不然只能靠猜（`vidowmsg windows` 里能查回 popup / float_mode）。 */
		wlr_log(WLR_DEBUG, "关闭动画跳过 %s：%s",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			view->popup ? "弹出物（菜单 / 提示 / 下拉框）不做关闭动画" :
			view->last_buffer == NULL ? "手里没有最后一帧快照" :
			anim.on ? "close_ms = 0（配置或规则里关了）" :
				"规则写了 anim = false");
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
	int cx = 0, cy = 0;
	/* 小窗（mini）：关闭动画画的是**上一帧那个小窗**，不是它原生的样子。
	 *
	 * 快照是整块源缓冲（原生尺寸），而小窗台上画着的那份是「原生 × contain 系数」
	 * 居中摆在盒子里（S3 的 view_mini_scale + S3b 的居中）。以前这条路上一个
	 * 小窗系数都没有：关掉一个 504x315 的自由小窗时，关闭动画第一帧直接按原生
	 * 649x724（含装饰留白）铺开 —— 用户报的「关闭小窗的时候也会显示原始大小」。
	 * 修法：画布 / 快照节点的尺寸都乘上 ms，位置挪到**内容**原点（盒子原点 +
	 * 居中偏移）—— 和 S5 的过渡动画取同一个「上一帧画在哪」。
	 *
	 * 纹理映射得显式写成「整块缓冲里可见窗口那一块」：原来的兜底公式拿 geo 的
	 * 偏移量当 uv 起点，而这里要把那个偏移按 ms 缩（留给**没装饰那条路**摆快照
	 * 节点用），不显式写就会把缩过的 geo 当成裁切。 */
	double mini_ms = view_mini_scale(view);
	if (mini_ms != 1.0 && mini_ms > 0.0 &&
			view->float_geom.width > 0 && view->float_geom.height > 0) {
		int off_x = geo.off_x, off_y = geo.off_y;
		if (geo.tex_du <= 0.0f || geo.tex_dv <= 0.0f) {
			geo.tex_u0 = geo.src_w > 0 ?
				(float)off_x / (float)geo.src_w : 0.0f;
			geo.tex_v0 = geo.src_h > 0 ?
				(float)off_y / (float)geo.src_h : 0.0f;
			geo.tex_du = geo.src_w > 0 ?
				(float)geo.w / (float)geo.src_w : 1.0f;
			geo.tex_dv = geo.src_h > 0 ?
				(float)geo.h / (float)geo.src_h : 1.0f;
		}
		cx = (int)lround(((double)view->float_geom.width -
			(double)geo.w * mini_ms) / 2.0);
		cy = (int)lround(((double)view->float_geom.height -
			(double)geo.h * mini_ms) / 2.0);
		geo.off_x = (int)lround((double)off_x * mini_ms);
		geo.off_y = (int)lround((double)off_y * mini_ms);
		geo.w = (int)lround((double)geo.w * mini_ms);
		geo.h = (int)lround((double)geo.h * mini_ms);
		if (geo.w < 1) {
			geo.w = 1;
		}
		if (geo.h < 1) {
			geo.h = 1;
		}
		w = (int)lround((double)w * mini_ms);
		h = (int)lround((double)h * mini_ms);
		/* 烘图倍率**不乘** ms：geo.w/h 已经是逻辑尺寸了，fx.c 那边
		 * 节点尺寸 = 画布像素 ÷ out_scale = geo.w + 2*pad，乘两遍就小一圈。
		 * 代价只有一个：装饰留白（pad）还是原生那点逻辑像素，140ms 的关闭
		 * 动画里那圈边框看着比静止时厚一点点。 */
	}
	vidow_closing_start(server, server->layer_toplevels, view->last_buffer,
		&geo, view->geom.x + (int)lround(view->anim_off_x) + cx,
		view->geom.y + (int)lround(view->anim_off_y) + cy, w, h,
		anim.close_ms, server->config.close_slide_px,
		true, server->config.shader_close, view_scale(view),
		server->config.close_easing, server->config.close_scale_to,
		&view->rule_eff, view->fullscreen);
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
	/* 横向和纵向分开记：过渡期间只有**纵向**是过渡推出来的（下面那条
	 * workspace_slide_n 的判据要用），换焦点把列滚回可见区那一下动的是横向。 */
	bool x_moved = view->geom.x != x;
	bool resized = view->geom.width != w || view->geom.height != h;

	/* 「现在画在哪」必须在覆盖 geom 之前取：覆盖之后再拿 geom 算补间起点，
	 * 得到的是新位置减新位置 = 0，动画照跑 190ms / 30 帧但一格都不动。 */
	int drawn_x = view->geom.x + (int)lround(view->anim_off_x);
	int drawn_y = view->geom.y + (int)lround(view->anim_off_y);
	/* 补间起点就是这个值，**被 park 的视图也不例外**：park 只把节点摆到屏外
	 * 的停放位、不动 geom/anim_off（见 view_anim_place），而 park 的判据恰恰是
	 * "画出来的盒子跟自己的输出框一点都不沾"（vidow_view_should_park）—— 所以
	 * 滑出去时这个值必然是屏外**它进来的那一侧**上的一点，起点本来就对：滚回来
	 * 它跟条带一起平移，位移跟别的列一模一样，不会单独飞。
	 * 别按"起点落在输出框里就把它推到屏幕边上"去改（ee769a7 干过）：那个判据在
	 * **正常**情况 —— 它明明画在屏上、只是被布局挪了一格 —— 也成立，于是每滚一列
	 * 窗口都被瞬移到屏幕边再滑回来：往左滚时起点算成输出框右缘（看着"一直占着
	 * 右边那一列、像跟别的窗口换了位置"），往右滚时起点算成 框.x - 窗口宽，而全屏
	 * 视图的宽 == 整屏宽，那个值正好是屏幕最左边（用户报的"从最左边移入"）。
	 * 列就是列，这里不按 fullscreen 分支。 */
	struct wlr_box before = {
		.x = drawn_x, .y = drawn_y,
		.width = view->geom.width, .height = view->geom.height,
	};

	view->geom = (struct wlr_box){ .x = x, .y = y, .width = w, .height = h };
	/* 布局真的把它挪/缩了（或者第一次摆）：这一笔记在**它自己**头上。它自己的
	 * 私有底衬（抓的时候它藏着）不受影响；别的窗口因为世界代次前进而重抓 ——
	 * 拖动时每动一下就重摆一次，分账全靠这里（见 vidow_arrange）。
	 *
	 * 老位置也单独记一笔：别的窗口的底衬里画着它，它挪走了那张图就过期了 ——
	 * 只记新位置的话，一步跳出去老远（拖得快、或者工作区切换）就漏掉了老位置
	 * 那一块，别的窗口底衬里会留着它的残影。 */
	if (moved || resized || !view->placed) {
		vidow_backdrop_damage_box(view->server, &before);
		vidow_backdrop_damage_view(view);
	}
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
	if (view->dragging || view->server->grab_view == view) {
		/* 平铺拖拽（grab_mode == VIDOW_GRAB_TILE）也算：它不动 view->dragging，
		 * 只置 grab_view / drag_detached，以前这里漏了它 —— 光标越过另一块屏
		 * 的边界、触发贴边自动滚 / 重排之后，arrange 进来起一条摆放补间，
		 * 补间每帧把 anim_off_* 按 anim_from_* 重新插值，把拖拽写进去的偏移
		 * 盖掉，窗口就**钉在屏边界上不再跟光标**（用户报的「拖到一定距离
		 * 窗口就看不见了」）。判据和 view_drag_output() 保持一致：拖拽期间
		 * 位置只归拖拽那条路管。 */
		vidow_anim_cancel(view->anim_move);
		view->anim_move = NULL;
		vidow_view_apply_anim(view);
		return;
	}

	/* 浮动窗口正被鼠标拖着：位置 1:1 跟光标，别起补间。
	 *
	 * 指针每动一下就走到这儿（grab 改 float_geom → arrange → place），补间于是被
	 * 每帧重起一次：弹性缓动在 t≈0 那一段几乎不动，窗口**一直落在光标后面**
	 * （用户那份 dbg 日志里量出来的：拖 273px/s 时中位滞后 199px、最大 387px，
	 * 起拖的头 125ms 窗口干脆没动）。平铺窗口的拖拽（drag.c）本来就是「只动场景
	 * 节点、布局一行都不重排」，浮动窗口没理由不跟手。 */
	if (view->server->grab_view == view &&
			view->server->grab_mode == VIDOW_GRAB_MOVE) {
		vidow_anim_cancel(view->anim_move);
		view->anim_move = NULL;
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		vidow_view_apply_anim(view);
		return;
	}

	/* 小窗（mini window，S5）形态转换正跑着：位置 / 缩放归那条补间管 ——
	 * 它的起点是「上一帧画在哪」（float.c 的 mini_transition_start），这里只
	 * 更新 geom 并把节点摆出去。**绝不能把 anim_off 清零 / 起位移补间**：
	 * 清零等于中间帧瞬移到终点（判据 18 量的是这个）。 */
	if (view->mini_anim != NULL) {
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
	if (view->popup || cfg->window_move_ms <= 0) {
		vidow_anim_cancel(view->anim_move);
		view->anim_move = NULL;
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		vidow_view_apply_anim(view);
		return;
	}

	/* 目标位置没变：这一趟布局没打算动它。
	 *
	 * **手里要是正跑着一个位移补间，那个补间就是朝这个位置跑的 —— 别掐它。**
	 * 以前这里不分青红皂白 cancel + anim_off = 0：而同一帧里 arrange 会跑好几
	 * 遍（关一个窗口那条路实测 3 遍：unmap 一次、摘列一次、换焦点一次），第二
	 * 遍就把上一遍刚起的补间清掉、偏移归零 —— 窗口当场落到终点，看起来就是
	 * 「关窗的时候隔壁那块瞬移」（用户报「窗口关闭的时候移动动画不够自然，好像
	 * 有点瞬移」）。没有补间在跑时照旧把残留偏移清掉（保持原来的语义）。 */
	if (!moved) {
		if (view->anim_move == NULL) {
			view->anim_off_x = 0.0;
			view->anim_off_y = 0.0;
		}
		vidow_view_apply_anim(view);
		return;
	}

	/* 列宽过渡正跑着：这一帧它右边那些列的位置变化**是过渡本身推出来的** ——
	 * 每帧重起一次位移补间的话它们会一直吊在过渡后面（补间刚走 5%，下一帧又
	 * 从那儿重新起一条 190ms 的），过渡结束后再自己飘到位，看着就是「隔壁那
	 * 几列慢半拍」。过渡期间直接落位：宽度本身是按曲线走的，它们跟着曲线走
	 * 正是「整条带子刚性跟着变」的样子。
	 *
	 * 放在 moved 判断**后面**：没动的窗口照旧走上面那条（手里正跑着位移补间
	 * 的那种不该被过渡掐掉）。 */
	/* 切工作区的过渡正跑着：这一帧所有窗口的纵向位移**是过渡本身推出来的** ——
	 * 每帧重起一次位移补间的话它们会一直吊在过渡后面（和上面列宽那条一个道理），
	 * 看着就是「工作区滑过去了、窗口还在原地飘」。过渡期间直接落位，整块工作区
	 * 刚性跟着曲线走。 */
	if (view->server->workspace_slide_n > 0 && !x_moved) {
		/* 纵向位移是过渡本身推出来的：这里照旧直接落位（上一帧刚起的那条
		 * 补间不掐的话它会一直吊在过渡后面）。
		 *
		 * **但手里要是有横向补间（切完工作区立刻换焦点 → 把焦点列滚回
		 * 可见区），别掐它**：以前这里一律 cancel + 偏移清零，那一下横向
		 * 就变成瞬移。补间留着，纵向起点按 0 走（纵向归过渡）。 */
		if (view->anim_move != NULL) {
			view->anim_from_y = 0.0;
			vidow_view_apply_anim(view);
			return;
		}
		vidow_anim_cancel(view->anim_move);
		view->anim_move = NULL;
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		vidow_view_apply_anim(view);
		return;
	}

	if (view->server->column_anim_n > 0) {
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
		cfg->window_move_ms, cfg->window_move_easing,
		view_move_update, view_move_done, view);
	if (view->anim_move != NULL) {
		/* 切工作区的过渡里：纵向归过渡（起点 0），横向照常补间。 */
		if (view->server->workspace_slide_n > 0) {
			view->anim_from_y = 0.0;
		}
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
	 * 没设就一起用统一值 gaps。每块屏可以用 layout{} 覆盖（niri 的
	 * output { layout { … } }）：vidow_layout_for 算的就是「全局 + 这块屏的
	 * 覆盖」，所以这里读的是这块屏最终生效的那份值。 */
	struct vidow_layout lay;
	vidow_layout_for(server, output, &lay);
	int top = vidow_layout_gap_side(&lay, lay.gaps_top);
	int bottom = vidow_layout_gap_side(&lay, lay.gaps_bottom);
	int left = vidow_layout_gap_side(&lay, lay.gaps_left);
	int right = vidow_layout_gap_side(&lay, lay.gaps_right);
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

/* 这一帧该按哪个比例画（定义在下面「列宽过渡」那一段）：补间在跑就是补间当前
 * 值，否则就是逻辑值。arrange 在上面，先声明一下。 */
static double column_layout_fraction(const struct vidow_column *col);

/* 摆一块工作区的列（Pass 3 那一套）。
 *
 * slide_y：切工作区过渡里这块工作区被推开的纵向偏移（正常 0）。
 * viewport：用这块工作区自己的滚动位置 —— 过渡期间两块工作区各滚各的。
 * fullscreen_out：列里那个全屏窗口（过渡里被推开的那块传 NULL：它那块不参与
 *   「谁该压在别人上面」的收口，它正在走人）。 */
static void arrange_columns(struct wl_list *shown,
		const struct wlr_box *work, const struct wlr_box *full_box,
		int inner, double viewport, int slide_y,
		struct vidow_view **fullscreen_out) {
	struct vidow_column *col;
	wl_list_for_each(col, shown, link) {
		int x = work->x + col->offset - (int)viewport;
		/* 全屏的那块也**跟着条带走**：左边缘 = 它自己那条列画在屏上的位置，
		 * 尺寸给整块屏。它在条带上占一整屏宽（见 Pass 1 那个 slot），所以
		 * 聚焦旁边那条列时它整块滑到屏外、旁边的列并排露出来；它自己那条列
		 * 被聚焦时视口正好落在它的 offset 上 —— 左边缘 0，严丝合缝盖住整屏。
		 * 滑出去那半块由 view_clip_box 裁在自己那块屏里（不会画到隔壁显示器）。 */
		int fx = full_box->x + col->offset - (int)viewport;

		/* 记一笔「这块列画在屏上的哪一块」：拖动找落点（drag.c）要按**屏幕上
		 * 看得见的**几何算，而全屏那列占一整屏、整列最小化的宽度是 0 这些
		 * 规矩只有这里知道。 */
		bool col_full = column_has_fullscreen(col);
		col->screen_x = col_full ? fx : x;
		/* 列宽占满整块工作区 = dock 眼里的「最大化」（vidow 没有单独的
		 * maximize 概念，niri 也是拿整列宽度当的）。同步给 foreign-toplevel，
		 * 否则 dock 永远以为没占满、不自动隐藏。 */
		{
			struct vidow_view *mv;
			wl_list_for_each(mv, &col->views, column_link) {
				if (mv->ftm_handle != NULL) {
					wlr_foreign_toplevel_handle_v1_set_maximized(
						mv->ftm_handle,
						col_full || col->width_fraction >= 0.999);
				}
			}
		}
		col->slot_width = col_full ? work->width : col->width;

		int n = 0;
		struct vidow_view *view;
		wl_list_for_each(view, &col->views, column_link) {
			if (!view_shows(view)) {
				continue;
			}
			if (view->fullscreen && view->mapped) {
				if (fullscreen_out != NULL) {
					*fullscreen_out = view;
				}
				vidow_place_view(view, fx, full_box->y + slide_y,
					full_box->width, full_box->height);
				continue;
			}
			n++;
		}
		if (n == 0) {
			continue;
		}

		int y = work->y + slide_y;
		wl_list_for_each(view, &col->views, column_link) {
			if (!view_shows(view) || (view->fullscreen && view->mapped)) {
				continue;
			}
			int h = (work->height - (n - 1) * inner) / n;
			vidow_place_view(view, x, y, col->width, h);
			y += h + inner;
		}
	}
}

int vidow_workspace_preview_layout(struct vidow_server *server,
		struct vidow_workspace *ws, int work_w, int work_h,
		void (*cb)(struct vidow_view *view, int x, int y, int w, int h,
			void *data),
		void *data) {
	if (server == NULL || ws == NULL || work_w < 1 || work_h < 1) {
		return 0;
	}
	/* 这块工作区住的那块屏的布局值（没住屏 = 全局那份）。 */
	struct vidow_layout lay;
	vidow_layout_for(server, ws->output, &lay);
	int inner = vidow_layout_gap_inner(&lay);
	int cursor = 0;
	struct vidow_column *col;
	/* 就是上面 arrange_columns 那一套（Pass 1 + Pass 3），只是**不写进
	 * view->geom**、坐标从工作区左上角起算：overview 要摆的是藏起来的工作区，
	 * 它们从来没被 arrange 过 —— col->offset / view->geom 还是上次显示时的老值
	 * （窗口被挪进来时新列的 offset 甚至是 0），照着画会叠在一起。 */
	wl_list_for_each(col, &ws->columns, link) {
		/* 整列都最小化了：这条列不占位置（宽度 0），剩下的列自然合拢。 */
		if (!column_has_shown_views(col)) {
			continue;
		}
		int width = vidow_layout_column_width_px(&lay, work_w,
			column_layout_fraction(col));
		bool col_full = column_has_fullscreen(col);
		int x = cursor;
		int n = 0;
		struct vidow_view *view;
		wl_list_for_each(view, &col->views, column_link) {
			if (!view_shows(view)) {
				continue;
			}
			if (view->fullscreen && view->mapped) {
				if (cb != NULL) {
					cb(view, x, 0, work_w, work_h, data);
				}
				continue;
			}
			n++;
		}
		int y = 0;
		wl_list_for_each(view, &col->views, column_link) {
			if (!view_shows(view) || (view->fullscreen && view->mapped)) {
				continue;
			}
			int h = (work_h - (n - 1) * inner) / n;
			if (cb != NULL) {
				cb(view, x, y, width, h, data);
			}
			y += h + inner;
		}
		/* 全屏的那条列在条带上占**一整屏**（同 Pass 1）：条带宽度 / 滚动范围
		 * 都按它算，全景里它也就跟着滚。 */
		cursor += (col_full ? work_w : width) + inner;
	}
	return cursor > 0 ? cursor - inner : 0;
}

static void output_arrange(struct vidow_server *server,
		struct vidow_output *output) {
	struct wlr_box work = vidow_work_area(server, output->wlr_output);
	if (work.width < 1 || work.height < 1) {
		return;
	}

	/* 全屏窗口占的那块 = **整块屏**（不吃 gap、也不理状态栏的占位）—— niri 的
	 * 「全屏窗口盖住整个屏幕」。取布局框：它和窗口摆位是同一个坐标系（逻辑坐标，
	 * 缩放 / 旋转已经算进去了），view_clip_box / vidow_work_area 用的也是它。 */
	struct wlr_box full_box = {0};
	wlr_output_layout_get_box(server->output_layout, output->wlr_output,
		&full_box);
	if (full_box.width < 1 || full_box.height < 1) {
		full_box = work; /* 布局里还查不到这块屏：退回工作区，别给个空框 */
	}

	/* 窗口之间（列与列、同列上下）的间距 = gaps_inner，没设就跟 gaps 一样。
	 * 每块屏可以用 layout{} 覆盖（niri 的 output { layout { … } }）：这里读的是
	 * 这块屏最终生效的那份值（全局 + 覆盖，见 vidow_layout_for）。 */
	struct vidow_layout lay;
	vidow_layout_for(server, output->wlr_output, &lay);
	int inner = vidow_layout_gap_inner(&lay);

	/* Pass 1: column widths and offsets, measured from the work area. */
	struct wl_list *shown = output_shown_columns(server, output->wlr_output);
	int cursor = 0;
	struct vidow_column *col;
	wl_list_for_each(col, shown, link) {
		/* 整列都最小化了：这条列从条带里消失（宽度 0、不占位置），剩下的列
		 * 自然合拢 —— 最小化的窗口自己还在列里，放回来就照旧。 */
		if (!column_has_shown_views(col)) {
			col->width = 0;
			/* slot_width 是 arrange_columns 写的；整列不占位时也得清零，
			 * 否则拖动找落点还会把这条空列当成条带上的一条缝（被拖窗口
			 * 已经 drag_detached，列里可能就剩它一个）。 */
			col->slot_width = 0;
			col->offset = cursor;
			continue;
		}
		/* 比例 → 像素用 vidow_column_width_px（连 gap 一起分那块区域）：
		 * 两条 0.5 连着中间的 gap 正好铺满可用区，不会多出一个 gap。
		 * 比例取 column_layout_fraction：列宽过渡在跑时是补间中间值。 */
		int width = vidow_layout_column_width_px(&lay, work.width,
			column_layout_fraction(col));
		col->width = width;
		col->offset = cursor;
		/* 全屏的那条列在条带上占**一整屏**：它画出来就是整块屏那么大，让位 /
		 * 滚走的距离自然也该是整块屏（用户报的「全屏的窗口也要参与滚动」）。
		 * 列自己的 width_fraction 不动 —— 退出全屏回来还是那个宽度。 */
		cursor += (column_has_fullscreen(col) ? work.width : width) + inner;
	}
	int total_width = cursor > 0 ? cursor - inner : 0;

	/* Pass 2: scroll the minimum amount that makes the focused column fully
	 * visible. Focus moves, the view follows. */
	double viewport = output->viewport_x;
	int max_viewport = total_width - work.width;
	if (max_viewport < 0) {
		max_viewport = 0;
	}
	/* 手指正拖着条带（三指横扫那种连续手势）：这期间视口是手指说了算 —— 不跑
	 * 下面那套「把焦点列滚回可见区」，不然刚把焦点列拖出屏幕就会被它拽回来。
	 * 松手那段吸附动画也走这里（gesture_drag 一直挂到动画结束）。 */
	if (output->gesture_drag) {
		viewport = output->gesture_viewport;
	} else if (output->drag_scroll) {
		/* 平铺拖拽期间视口归拖拽自己（贴边自动滚条带，见 drag.c 的
		 * vidow_tile_drag_autoscroll）：不能跑下面那套「把焦点列滚回可见区」
		 * —— 刚为了看落点滚出去，立刻就被它拽回来，等于滚不动。 */
		viewport = output->drag_viewport;
	} else {
		viewport = focus_viewport_target(server, output, viewport, work.width);
	}
	if (viewport < 0) {
		viewport = 0;
	}
	if (viewport > max_viewport) {
		viewport = max_viewport;
	}
	output->viewport_x = viewport;

	/* 把最终视口写回**当前显示的那块工作区**。
	 *
	 * viewport_x 挂在 output 上（一块屏只有一个滚动位置），而每块工作区想记住
	 * 「自己离开时滚到哪」—— 那份记忆就是 ws->viewport_x。老代码只在切换的
	 * 那一瞬间抄一次（workspace.c: prev->viewport_x = out->viewport_x），可那一刻
	 * 抄到的是**上一次 arrange 之后**的值：焦点跟随、新窗口插入、列宽变化都会
	 * 让 arrange 把视口滚到别处，于是抄下去的是个陈旧值，切回来的位置就不对 ——
	 * 用户看到的「切工作区窗口自己动一下」。
	 *
	 * 所以在这里（所有滚动都算完、clamp 过之后）落账：谁显示着，谁的 viewport_x
	 * 就是屏幕上这个值。切换时那份快照只是兜底，真值一直跟着屏幕走。 */
	struct vidow_workspace *vp_ws =
		vidow_workspace_shown_for(server, output->wlr_output);
	if (vp_ws != NULL) {
		vp_ws->viewport_x = viewport;
	}

	/* Pass 3: place the tiled windows. Floating views are placed afterwards
	 * so that a follow-float can read its parent's fresh tile geometry.
	 * 最小化的窗口不摆放（它原来的几何留着，放回来那次 arrange 会重算），
	 * 也不参与 n —— 同列剩下的窗口因此自己撑满。 */
	struct vidow_view *fullscreen = NULL;
	struct vidow_workspace *shown_ws =
		vidow_workspace_shown_for(server, output->wlr_output);
	/* 切工作区的过渡里被推开的那块先摆：它和现在这块都占着同一块屏，谁后摆
	 * 谁留下的 col->screen_x / slot_width 就归谁用（拖动找落点读的是这两个），
	 * 这两个值该是**当前这块**工作区的。 */
	if (output->slide_from != NULL) {
		arrange_columns(&output->slide_from->columns, &work, &full_box,
			inner, output->slide_from->viewport_x,
			(int)lround(output->slide_from->slide_y), NULL);
	}
	arrange_columns(shown, &work, &full_box, inner, viewport,
		shown_ws != NULL ? (int)lround(shown_ws->slide_y) : 0,
		&fullscreen);
	vidow_float_place_all(server, output);

	/* 浮动的全屏窗口也要占满**整块屏**（和列里的全屏同一个 full_box）：以前只有
	 * 「列里的全屏」会被摆成整屏，浮动的那种「全屏」在层级上照样永远压在最上面
	 * （vidow_restack 只认 fullscreen 标志），画面上却还是它自己那一小块 ——
	 * 用户看到的就是「它一直叠在最上面，还不占满屏」。几何只进 geom
	 * （vidow_place_view 不碰 float_geom），退出全屏时照旧回原尺寸。 */
	if (fullscreen == NULL) {
		struct vidow_view *v;
		wl_list_for_each(v, &server->views, link) {
			if (v->fullscreen && v->mapped && view_shows(v) &&
					v->float_mode != VIDOW_FLOAT_NONE && !v->popup &&
					v->output == output->wlr_output &&
					vidow_workspace_view_visible(server, v)) {
				fullscreen = v;
				break;
			}
		}
		/* 只有**浮动**的全屏在这儿摆：列里那些在上面的 Pass 3 就摆完了（它们
		 * 的位置跟着条带走、尺寸给整块屏），这里再摆一次会把那一份盖掉。 */
		if (fullscreen != NULL) {
			vidow_place_view(fullscreen, full_box.x, full_box.y,
				full_box.width, full_box.height);
		}
	}

	if (fullscreen != NULL) {
		/* 压在别人上面 / 从别人上面挪开：底衬的合成结果变了，账记在它头上。 */
		vidow_backdrop_damage_view(fullscreen);
		wlr_scene_node_raise_to_top(&fullscreen->scene_tree->node);
	}

	/* 按需 VRR（niri 的 variable-refresh-rate on-demand）：这块屏现在有没有
	 * 「要 VRR 的窗口显示着」—— 有就把刷新率提上去，没有就降回来。没开按需
	 * 模式的屏在函数第一行就返回了，所以这里等于白调一次。 */
	vidow_output_vrr_on_demand_update(server, output->wlr_output);
}

void vidow_arrange(struct vidow_server *server) {
	struct vidow_output *output;
	/* 先把住址对齐一遍：窗口 / 列住哪块屏只能由它们所在的工作区说了算。
	 * 切工作区、规则搬屏、热插拔、拖动跨屏都会改到一半，漏一处就会让滚动
	 * 越界的窗口按几何框算到隔壁屏的工作区里。 */
	vidow_workspace_sync_outputs(server);
	/* 桌面底色跟着输出布局走（拔线 / 改位置 / 改分辨率都会走到这儿）。 */
	vidow_background_apply(server);
	/* Moving windows changes what is behind every other window.
	 *
	 * 但拖动的时候每动一下都会重摆一次（被拖的那个窗口的 geom 跟着光标走）：
	 * 这种「只有它自己在动」的重摆，每一笔都已经由 place_view / 可见性翻转 /
	 * 置顶那几处记在**它自己头上**了（vidow_backdrop_damage_view）——它自己的
	 * 私有底衬抓的时候把它藏起来了，不受影响。这里要是照样把整个世界代次推一
	 * 遍，拖一个重叠的窗口时它自己的底衬每帧都会被判过期，白重抓一遍（基准实测
	 * 每帧 2 次重抓里的一次）。
	 *
	 * 「是不是只有它」：这一趟里只要有任何一个**不是正被拖的那个**窗口报了自己
	 * 变了，标志就被清掉，退回「世界变了」—— 宁可多抓一次，不能糊着旧的。 */
	server->arrange_self_only = server->grab_view != NULL;
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
	/* 全屏的时候把占位的 top layer（状态栏）藏掉：它看不见了，但输入命中
	 * 还认它，屏边上就留一条点不动的死区；退出全屏再放回来。 */
	vidow_layers_update_fullscreen(server);

	/* 摆完了收口一次层级：新窗口是「挂到最后」建出来的（在场景树里就是最上面），
	 * 浮窗得重新压回平铺之上；浮 ⇄ 平铺切换、全屏进出也都在这一趟里摆正（见
	 * vidow_restack —— 没人站错队时它一个节点都不碰）。 */
	vidow_restack(server, NULL);

	if (!server->arrange_self_only) {
		vidow_backdrop_damage(server, false);
	}
	server->arrange_self_only = false;
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
			/* 窗口跟着列走：工作区指针也要改。只改 output 的话 view->ws 还
			 * 指着搬走前那块工作区 —— 可见性按 ws 判，窗口在新屏上永远不显示，
			 * 而 IPC / overview 又按 output 说它在新屏，两边对不上。 */
			view->ws = target;
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
		struct vidow_output_memory *home = view->ws != NULL ?
			view->ws->home : home_of(server, view->output);
		if (view->output != dead && home != memory) {
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
		if (view->column != NULL || view->popup) {
			continue;
		}
		struct vidow_output_memory *home = view->ws != NULL ?
			view->ws->home : home_of(server, view->output);
		if (home != memory) {
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

/* --------------------------------------------------------------- 层级 */

/* 场景节点 → 窗口：窗口自己那棵树的 node.data 指向它的 tag（见 vidow_view_at
 * 往上走的那一段）。不是窗口节点（关闭动画的替身、别的层的节点）返回 NULL。 */
static struct vidow_view *view_from_node(struct wlr_scene_node *node) {
	struct vidow_view_tag *tag = node != NULL ? node->data : NULL;
	if (tag == NULL || tag->magic != VIDOW_VIEW_TAG_MAGIC) {
		return NULL;
	}
	return tag->view;
}

/* 场景树里同一层的先后就是层级（wlr_scene_node_at 也是从最上面那个往下找），
 * 这里维持一条不变量：
 *
 *     平铺 < 浮动 < 全屏
 *
 * 为什么要专门收口：`wlr_scene_tree_create()` 一律把新节点挂到最后，所以新开的
 * 平铺窗口会盖住已有的浮窗；聚焦那边又是无差别 raise_to_top，点一下平铺窗口浮窗
 * 就被压下去。两处都没在管「谁该在谁上面」。
 *
 * top 非空 = 「这一个刚被抬起来」（点了它 / 聚焦到它）：它落在自己那一组的最上面
 * —— 浮窗之间就是靠这一条换层级的（点哪个哪个上来）。
 *
 * 只在真有人站错队时才动手：raise 会让那块区域重画，站对了就一个节点都不碰。
 * 全屏单列一组压在最后：它是「占满整屏」，不该被浮窗盖住（老代码在 arrange 里
 * 抬全屏，这里接着保证它一直是最上面那个）。 */
void vidow_restack(struct vidow_server *server, struct vidow_view *top) {
	if (server == NULL || server->layer_toplevels == NULL) {
		return;
	}
	/* 弹出物（X11 的菜单 / 提示）挂在 layer_popups 那棵树上：它本来就在所有窗口
	 * 之上，这里只需要把自己抬起来（老行为）。 */
	if (top != NULL && top->popup) {
		if (top->scene_tree != NULL) {
			wlr_scene_node_raise_to_top(&top->scene_tree->node);
		}
		return;
	}

	/* 第一遍：按现在的层级（底 → 顶）把两组节点收下来，顺手看有没有人站错队。
	 * 收成数组是因为第二遍要动节点：wlr_scene_node_raise_to_top() 会把当前节点
	 * 挪到链表尾，边遍历边动会在同一个链表里绕圈。 */
	struct wl_array floats; /* struct wlr_scene_node * */
	struct wl_array fulls;
	wl_array_init(&floats);
	wl_array_init(&fulls);

	struct wlr_scene_node *node;
	struct wlr_scene_node *last_tile = NULL, *last_float = NULL;
	struct wlr_scene_node *last_full = NULL;
	bool float_seen = false, broken = false;
	wl_list_for_each(node, &server->layer_toplevels->children, link) {
		struct vidow_view *view = view_from_node(node);
		if (view == NULL) {
			continue;
		}
		bool full = view->fullscreen && view->mapped;
		if (!full && view->float_mode == VIDOW_FLOAT_NONE) {
			/* 平铺：它上面既不该有浮窗，也不该有全屏 */
			if (float_seen || last_full != NULL) {
				broken = true;
			}
			last_tile = node;
			continue;
		}
		struct wlr_scene_node **slot = wl_array_add(full ? &fulls : &floats,
			sizeof(*slot));
		if (slot != NULL) {
			*slot = node;
		}
		if (full) {
			last_full = node;
		} else {
			float_seen = true;
			last_float = node;
			if (last_full != NULL) {
				broken = true; /* 浮窗压在全屏上面 */
			}
		}
	}

	struct wlr_scene_node *top_node = top != NULL && top->scene_tree != NULL ?
		&top->scene_tree->node : NULL;
	bool top_full = top != NULL && top->fullscreen && top->mapped;
	bool top_float = top != NULL && !top_full &&
		top->float_mode != VIDOW_FLOAT_NONE;

	bool need = broken;
	if (top_node != NULL) {
		if (top_full) {
			need = need || last_full != top_node;
		} else if (top_float) {
			need = need || last_float != top_node;
		} else {
			/* 平铺的那个：只要它在平铺组里最上面就行 —— 浮窗 / 全屏在它上面
			 * 是应该的，不算站错队（否则点一下平铺窗口就会白抬一遍、白记一次
			 * 底衬过期）。 */
			need = need || last_tile != top_node;
		}
	}
	if (!need) {
		wl_array_release(&floats);
		wl_array_release(&fulls);
		return;
	}

	/* 第二遍：抬的顺序就是层级 —— 平铺的那个先（它在平铺组里最上面），浮窗按
	 * 现在的顺序跟上，全屏最后。top 在自己那一组里最后抬，所以它落在组的最上面。 */
	if (top_node != NULL && !top_full && !top_float) {
		wlr_scene_node_raise_to_top(top_node);
	}
	for (size_t i = 0; i < floats.size / sizeof(struct wlr_scene_node *); i++) {
		struct wlr_scene_node *n = ((struct wlr_scene_node **)floats.data)[i];
		if (n != top_node) {
			wlr_scene_node_raise_to_top(n);
		}
	}
	if (top_node != NULL && top_float) {
		wlr_scene_node_raise_to_top(top_node);
	}
	for (size_t i = 0; i < fulls.size / sizeof(struct wlr_scene_node *); i++) {
		struct wlr_scene_node *n = ((struct wlr_scene_node **)fulls.data)[i];
		if (n != top_node) {
			wlr_scene_node_raise_to_top(n);
		}
	}
	if (top_node != NULL && top_full) {
		wlr_scene_node_raise_to_top(top_node);
	}
	wl_array_release(&floats);
	wl_array_release(&fulls);

	/* 层级动了 = 「谁在谁后面」变了：底下那些窗口的私有底衬得重抓一遍。
	 * arrange 那条路本来就会记这一笔（除非它在「只有被拖的那个在动」的省电
	 * 模式里），这里管的是不经过 arrange 的那些调用（点一下浮窗换层级）。 */
	vidow_backdrop_damage(server, false);
}

/* --------------------------------------------------------------- focus */

/* 「聚焦这个窗口会让条带滚多少像素」—— niri 的 scroll_amount_to_activate。
 * 算的是和 arrange 的 Pass 2（把焦点列滚回可见区）**同一件事**：现在视口在哪、
 * 聚焦它以后视口该在哪，两者差多少就滚多少。只是这里不真滚，只把数报出去 ——
 * focus-follows-mouse 的 max-scroll-amount 拿它跟阈值比（见 input.c）。
 *
 * 浮窗的「跟随式」子窗（FLOAT_FOLLOW）和 Pass 2 一样跟着父窗口算：光标停在
 * 对话框上时，滚动的是它爹那条列。 */
double vidow_view_scroll_amount_to_activate(struct vidow_server *server,
		struct vidow_view *view) {
	if (view == NULL) {
		return 0.0;
	}
	if (view->float_mode == VIDOW_FLOAT_FOLLOW && view->parent != NULL) {
		view = view->parent;
	}
	struct vidow_column *col = view->column;
	if (col == NULL || col->output == NULL) {
		return 0.0;
	}
	struct vidow_output *output = vidow_output_lookup(server, col->output);
	if (output == NULL) {
		return 0.0;
	}
	struct wlr_box work = vidow_work_area(server, output->wlr_output);
	if (work.width < 1) {
		return 0.0;
	}
	double viewport = output->viewport_x;
	/* 和 Pass 2 同一个算法（focus_viewport_target）—— 「聚焦它会让视口滚到
	 * 哪」和「聚焦之后视口真的在哪」必须是同一件事，不然居中策略一开，
	 * focus-follows-mouse 的 max-scroll-amount 判的就全是错的。临时把焦点列
	 * 换成它，算完还回去：这个函数只算不滚，不许改 server 的状态。 */
	struct vidow_view *saved_focus = server->focused;
	server->focused = view;
	double target = focus_viewport_target(server, output, viewport, work.width);
	server->focused = saved_focus;

	/* 和 arrange 一样夹到 0..max_viewport：不夹的话「居中的列在条带最左 /
	 * 最右」会报出一个实际滚不到的距离。 */
	int max_viewport = 0;
	struct vidow_column *c;
	wl_list_for_each(c, output_shown_columns(server, output->wlr_output), link) {
		if (!column_has_shown_views(c)) {
			continue;
		}
		max_viewport = c->offset + column_strip_width(c, work.width);
	}
	max_viewport -= work.width;
	if (max_viewport < 0) {
		max_viewport = 0;
	}
	if (target < 0.0) {
		target = 0.0;
	}
	if (target > max_viewport) {
		target = max_viewport;
	}
	return target > viewport ? target - viewport : viewport - target;
}

void vidow_focus_view(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = vidow_view_surface(view);
	struct wlr_surface *prev = seat->keyboard_state.focused_surface;

	/* 最小化的窗口不接键盘（它没画在画面上）。真走到这儿是调用方漏了跳过，
	 * 放回来那条路会先把 minimized 清掉再进来。 */
	if (view->minimized) {
		return;
	}
	/* 小窗（mini window，S2）：**迷你 / 图标态不拿键盘焦点**。和上面那条同形状、
	 * 同一个思想 —— 调用方漏了跳过；放大那条路（T3/T4）会先把形态改成 FREE
	 * 再进来。不变量：server->focused 永远不指向 MINI / ICON 的窗口。
	 *
	 * ⚠️ 和锁屏（S3）那套**方向相反**：锁屏是「谁都不许进」（连命中测试都短路），
	 * 这里是「只有焦点不许进，命中必须进」—— vidow_view_at() 必须能命中迷你态
	 * （单击要能放大），所以那边一个字都不动。 */
	if (view->mini == VIDOW_MINI_MINI || view->mini == VIDOW_MINI_ICON) {
		return;
	}

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
			/* surface 可能是 NULL（wine / Proton 的游戏起来时：窗口先到、
			 * 表面还没到）。vidow_seat_focus_surface 会清焦点而不是 abort。 */
			vidow_seat_focus_surface(seat, surface);
		}
	}

	/* 焦点变了，把 text-input 的 enter/leave 转给客户端输入框 */
	vidow_im_focus(server, seat->keyboard_state.focused_surface);

	/* 抬起来：浮动永远在平铺之上、全屏压一切，同一组里谁被点谁在上（层级规则
	 * 见 vidow_restack）。 */
	vidow_restack(server, view);
	vidow_view_set_activated(view, true);
	vidow_portal_view_activated(view, true);

	/* The border colour depends on focus, so repaint both windows. */
	struct vidow_view *previous = server->focused;
	server->focused = view;
	if (previous != view) {
		/* center_focused_column = "on-overflow" 要知道「上一个焦点列」是谁
		 * （见 focus_viewport_target）。换焦点时才动它。 */
		server->last_focused_col = previous != NULL ? previous->column : NULL;
		/* 「现在谁拿着键盘」是排查焦点问题（抢焦点 / 顶不上来）的第一现场，
		 * 而且只在真的换人时才打一行。 */
		wlr_log(WLR_INFO, "焦点: %s -> %s",
			previous != NULL && vidow_view_title(previous) != NULL ?
				vidow_view_title(previous) : "(无)",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)");
		/* 「最近用过」的台账（Alt+Tab 切换器要用）：见 mru.c 的 debounce。 */
		vidow_mru_note_focus(server, view);
	}
	/* 工作区记着「离开时焦点是谁」，换回来的时候要还原。 */
	vidow_workspace_note_focus(view->ws, view);
	/* 焦点屏跟着**归属**走（工作区住哪块屏），不是 view->output 那个住址：
	 * 两者在切工作区 / 热插拔 / 规则搬屏的中间态里可能短暂分叉，按住址走会
	 * 把 focused_output 带到隔壁屏上 —— 之后的空点击、新窗口、切工作区全按
	 * 错的屏算。工作区归属只有真的换屏才变，和 view_home_output 同一个判据。 */
	struct wlr_output *focus_out =
		view->ws != NULL && view->ws->output != NULL ?
		view->ws->output : view->output;
	if (focus_out != NULL) {
		server->focused_output = focus_out;
	}

	vidow_arrange(server);
	/* 换焦点会 restack 一次，把新工作区的窗口抬到旧工作区上面；过渡里
	 * 旧工作区得重新压回去，否则滑动过程里看到的是新窗口盖住旧工作区。 */
	vidow_workspace_slide_raise(server);

	if (previous != view) {
		vidow_ipc_emit_focus(server, view);
	}

	if (previous != NULL && previous != view && previous->fx.active) {
		vidow_view_apply_anim(previous);
	}
}

/* 「跳到这个窗口」= 迷你 / 图标态先摊成自由小窗，再给焦点（见 server.h）。
 *
 * 为什么在这儿：MRU 切换器（Alt+Tab 提交 / 点击卡片，src/mru.c）和 IPC 的
 * focus-window（src/ipc.c）是「选中即跳过去」的同一个语义，共用这一个入口；
 * 迷你 / 图标态是列在切换器里的（mru_listable 不管形态），选中它就该能用它。
 *
 * 顺序反过来（先 vidow_focus_view）会被上面那条守卫挡回来：什么都不发生，
 * 也就是这次改动之前的行为。摊成 FREE 走的是 float.c 的形态机，过渡动画、
 * 出帧策略、宽高比那套一件不少。 */
void vidow_focus_view_expand_mini(struct vidow_view *view) {
	if (view == NULL) {
		return;
	}
	if (view->mini == VIDOW_MINI_MINI || view->mini == VIDOW_MINI_ICON) {
		vidow_mini_set_state(view, VIDOW_MINI_FREE);
	}
	vidow_focus_view(view);
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
	/* 事件：有人要求激活这个窗口（X11 的 _NET_ACTIVE_WINDOW、Wayland 的
	 * xdg-activation、IPC 的 activate-window、点通知预览…都走这里）。
	 * 关注点不是「谁抢了焦点」而是「应用在喊『看我』」—— 通知 / 高亮脚本要的是
	 * 这个。 */
	vidow_ipc_emit_window(server, "window-activated", view);
	const char *title = vidow_view_title(view) != NULL ?
		vidow_view_title(view) : "(无题)";

	/* 最小化的窗口被激活（点通知 / 任务栏点它 / IPC restore-window）：先放回布局，
	 * 再走下面那套「端出工作区 + 给焦点」。规则说不理激活请求的话，也照样放回 ——
	 * 「别抢焦点」不等于「别把它端出来」。 */
	if (view->minimized) {
		vidow_view_set_minimized(view, false);
	}

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
		struct vidow_view *view = column_first_shown_view(col);
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
				v->output != output || v->geom.width <= 0 ||
				!view_shows(v)) {
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
		next = column_first_shown_view(next->column);
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

	/* 同列上下：最小化的兄弟不接键盘（它没画在画面上），跳过它继续找。 */
	for (node = view->column_link.next; node != &col->views; node = node->next) {
		next = wl_container_of(node, next, column_link);
		if (view_shows(next)) {
			return next;
		}
	}
	for (node = view->column_link.prev; node != &col->views; node = node->prev) {
		next = wl_container_of(node, next, column_link);
		if (view_shows(next)) {
			return next;
		}
	}

	/* 列里就它一个：跨列找，但只在它那块工作区正摆在这块屏上时找 —— 藏着的
	 * 工作区里的窗口不该拿到键盘（那种情况走兜底，跟以前一样挑块看得见的）。 */
	if (col->ws == NULL || vidow_workspace_shown_for(server, output) != col->ws) {
		return NULL;
	}

	for (node = col->link.next; node != &col->ws->columns; node = node->next) {
		c = wl_container_of(node, c, link);
		next = column_first_shown_view(c);
		if (next != NULL) {
			return next;
		}
	}
	for (node = col->link.prev; node != &col->ws->columns; node = node->prev) {
		c = wl_container_of(node, c, link);
		next = column_first_shown_view(c);
		if (next != NULL) {
			return next;
		}
	}

	/* 这块工作区上没有别的列了：可能还剩浮窗 —— 它们照样该接住键盘（niri 的
	 * 「平铺关完落到浮窗上」）。挑不到就返回 NULL，调用方走兜底。 */
	return focus_nearest_by_x(server, view, output);
}

/* -------------------------------------------------------- 小窗（S2）焦点交出 */

/* 一个窗口要进 MINI / ICON 了：它**不接键盘**，所以焦点得先交出去。规矩照抄
 * vidow_view_set_minimized() 里那条（已测过的路）—— 「位置不动，谁滑到空出来的
 * 位置上就是谁」，不另发明一套。
 *
 * 两件事，必须**在摘列 / 摘工作区之前**做（vidow_mini_set_state 的调用点在最前面）：
 *   1) 键盘焦点换人：focus_after_close() 要靠窗口还挂在列里才找得到邻居；
 *   2) 指针焦点清掉：光标本来压在这个窗口上，形态一变它就不该再收 enter / motion
 *      （不然进迷你态的第一下移动还会转给客户端，见 input.c 的小窗分流）。
 *
 * 为什么放在 view.c：focus_after_close / focus_nearest_column / focus_nearest_by_x
 * 都是这儿的静态函数，搬出去要暴露三个。 */
void vidow_view_yield_focus_for_mini(struct vidow_view *view) {
	if (view == NULL || view->popup) {
		return;
	}
	struct vidow_server *server = view->server;
	struct wlr_output *output = view->output;

	/* 指针焦点：光标底下就是它（或者它的子表面）才清 —— 清别人的会打断正在
	 * 操作的窗口。 */
	struct wlr_surface *pfocus = server->seat->pointer_state.focused_surface;
	if (pfocus != NULL && vidow_view_from_surface(server,
			wlr_surface_get_root_surface(pfocus)) == view) {
		wlr_seat_pointer_notify_clear_focus(server->seat);
	}

	/* 形态上它也不再是「活动窗口」了：边框 / 任务栏（foreign toplevel）跟着走。 */
	vidow_view_set_activated(view, false);
	vidow_portal_view_activated(view, false);

	if (server->focused != view) {
		return;
	}
	struct vidow_view *next = focus_after_close(server, view, output);
	server->focused = NULL;
	if (next != NULL) {
		vidow_focus_view(next);
	} else {
		focus_nearest_column(server, output);
	}
	wlr_log(WLR_INFO, "mini: 焦点交出 %s（迷你 / 图标不接键盘）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)");
}

/* ================================== 小窗（mini window，S5）：图标悬停预览
 *
 * 悬停图标 mini_icon_hover_ms 之后，在**朝屏内**那一侧展开一块迷你尺寸 ×0.8 的
 * **实时预览**（图标本身贴边、有一部分在屏外，朝屏外展开会被裁掉一半）。
 *
 * 三条规矩：
 *  1) **只读**：不抢 pointer focus、不进 MRU、不拿键盘焦点、不参与
 *     vidow_view_at 命中 —— 它是**信息**不是窗口（指针扫过屏幕边缘就把键盘焦点
 *     从正在打字的窗口上拽走是不能接受的）。命中和焦点这两条是**构造性**的：
 *     预览挂在 layer_toplevels 里、节点没有 view tag，vidow_view_at 走的是
 *     「节点 → view」那条映射，认不出它。
 *  2) **但吞点击**：指针底下有一块「像窗口的东西」，点下去却激活**背后的**窗口
 *     是明显的错。吞掉最省事，也不引入「预览能不能操作」这个新问题。
 *  3) **渲染用同一张表面的第二份**（wlr_scene_subsurface_tree_create，先例
 *     wincapture.c:676）—— 不另造一套抓帧 / 上屏管线，客户端出一帧预览就跟着动。
 *
 * 时间轴（用 vidow_anim_start 当定时器，不新开 timer —— 它自己会 kick watchdog
 * 保证「就算后端不给帧事件帧也照出」）：延迟 hover_ms → 出；不在图标也不在预览
 * → 离开 leave_ms → 收；**指针进到预览矩形里就把计时清掉**（这就是宽限期：
 * 从图标移到预览要跨那 8px 空隙，不给宽限会闪一下再回来）。
 */

/* ---- 小窗（S5）：图标 / 预览的出帧策略（计划 §9②） ----
 *
 * 按该窗口**有效**的 force_render 分叉：
 *   - 开 → 活节点（图标就是窗口自己那块节点，预览是表面第二份镜面）+ 客户端
 *     照常出帧（render_pump 在它被停到屏外时保活）→ 图标和预览都是**动的**；
 *   - 关 → 快照节点（`view->last_buffer` 的最后一帧）+ **真实节点停到停车位**
 *     → 客户端停帧，图标和预览都是**静止的最后一帧**。
 *
 * 两条路必须**一致**：预览不许出现「图标静止、预览还在动」—— 所以它走的是同一个
 * 判据（vidow_mini_uses_snapshot），不是各判一份。
 *
 * 为什么不算新生命周期路径：快照节点的 buffer 由 `wlr_scene_buffer_set_buffer()`
 * 自己管引用（它 ref 新的、unref 旧的），所以只要在 `view->last_buffer` 换新
 * 的那一处同步一次就够 —— 不用另建监听、不用管窗口什么时候没。
 *
 * 退化情形：`last_buffer == NULL`（客户端一帧都没交过）→ **退回活节点**，
 * 在进图标态那次打一行 INFO（见 float.c 的 vidow_mini_set_state）。
 */
bool vidow_mini_uses_snapshot(const struct vidow_view *view) {
	return view != NULL && view->mapped && view->mini == VIDOW_MINI_ICON &&
		!view->force_render && view->last_buffer != NULL;
}

/* 快照节点收掉。它是 `layer_toplevels` 的孩子，**不跟着 view->scene_tree 走** ——
 * 窗口 unmap / 销毁时留着它，屏上就是一块「窗口都没了、还冻着最后一帧」的鬼影
 * （而且攥着那帧缓冲）。下次再进图标态 mini_snapshot_place() 会重建。 */
static void mini_snapshot_free(struct vidow_view *view) {
	if (view->mini_snapshot == NULL) {
		return;
	}
	wlr_scene_node_destroy(&view->mini_snapshot->node);
	view->mini_snapshot = NULL;
}

/* 图标态的快照节点：位置尺寸跟着图标那块盒子（contain 居中，和 view_mini_scale
 * 同一条算法），只在「快照那条路」上开。 */
static void mini_snapshot_place(struct vidow_view *view) {
	if (view->scene_tree == NULL) {
		return;
	}
	if (!vidow_mini_uses_snapshot(view)) {
		if (view->mini_snapshot != NULL) {
			wlr_scene_node_set_enabled(&view->mini_snapshot->node, false);
		}
		return;
	}
	if (view->mini_snapshot == NULL) {
		if (view->server->layer_toplevels == NULL) {
			return;
		}
		view->mini_snapshot = wlr_scene_buffer_create(
			view->server->layer_toplevels, view->last_buffer);
		if (view->mini_snapshot == NULL) {
			return;
		}
		wlr_log(WLR_INFO, "mini: %s 图标态走快照（force_render 关）→ 真实节点"
			"停到停车位、客户端停帧，图标和预览都是最后一帧",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)");
	} else if (view->mini_snapshot->buffer != view->last_buffer) {
		wlr_scene_buffer_set_buffer(view->mini_snapshot, view->last_buffer);
	}
	int x = view->geom.x + (int)lround(view->anim_off_x);
	int y = view->geom.y + (int)lround(view->anim_off_y);
	int nw = 0, nh = 0;
	view_mini_native(view, &nw, &nh);
	int dw = view->geom.width, dh = view->geom.height;
	if (nw > 0 && nh > 0) {
		double s = (double)view->geom.width / (double)nw;
		double sy = (double)view->geom.height / (double)nh;
		if (sy < s) {
			s = sy;
		}
		dw = (int)lround((double)nw * s);
		dh = (int)lround((double)nh * s);
		if (dw < 1) {
			dw = 1;
		}
		if (dh < 1) {
			dh = 1;
		}
	}
	int px = x + (view->geom.width - dw) / 2;
	int py = y + (view->geom.height - dh) / 2;
	/* 越界裁剪（S3 修）：快照节点挂在**全局**的 layer_toplevels 上（不在窗口自己
	 * 的场景树里），而场景图是全局坐标 —— 图标按 mini_icon_overflow 刻意探出屏边
	 * 那一块会被隔壁那块屏照画（实测那条 16×64 带里 832 个非背景像素、其中 172 个
	 * 是客户端的纯红）。裁法和 fx 那边同一招：源框（缓冲像素）取可见那一块、目标框
	 * （场景尺寸）也取可见那一块 —— 缩放比一个像素不变，框外那部分不画。
	 *
	 * 判据用**自己那块屏的框**（view_clip_box 那条路对停到停车位的窗口是「不裁」：
	 * 它那边裁的是真实节点，而快照模式下真实节点本来就被停到屏外了）。 */
	struct wlr_box clip = {0};
	if (view->output != NULL) {
		wlr_output_layout_get_box(view->server->output_layout, view->output,
			&clip);
	}
	if (clip.width >= 1 && clip.height >= 1) {
		struct wlr_box full = {.x = px, .y = py, .width = dw, .height = dh};
		struct wlr_box vis;
		int bw = view->last_buffer != NULL ? view->last_buffer->width : 0;
		int bh = view->last_buffer != NULL ? view->last_buffer->height : 0;
		if (!wlr_box_intersection(&vis, &full, &clip)) {
			/* 整个身子都在自己那块屏外：一个像素都不该画（画了就是画到隔壁
			 * 屏上）。甩到所有输出框外面去 —— 和 fx 那边同一招。 */
			wlr_scene_buffer_set_source_box(view->mini_snapshot, NULL);
			px -= 1000000;
			py -= 1000000;
			dw = 1;
			dh = 1;
		} else if ((vis.width != dw || vis.height != dh) && bw > 0 && bh > 0) {
			double kx = (double)bw / (double)dw;
			double ky = (double)bh / (double)dh;
			struct wlr_fbox src = {
				.x = (double)(vis.x - px) * kx,
				.y = (double)(vis.y - py) * ky,
				.width = (double)vis.width * kx,
				.height = (double)vis.height * ky,
			};
			wlr_scene_buffer_set_source_box(view->mini_snapshot, &src);
			px = vis.x;
			py = vis.y;
			dw = vis.width;
			dh = vis.height;
		} else {
			wlr_scene_buffer_set_source_box(view->mini_snapshot, NULL);
		}
	} else {
		wlr_scene_buffer_set_source_box(view->mini_snapshot, NULL);
	}
	wlr_scene_buffer_set_dest_size(view->mini_snapshot, dw, dh);
	wlr_scene_node_set_position(&view->mini_snapshot->node, px, py);
	wlr_scene_node_set_enabled(&view->mini_snapshot->node, true);
}

void vidow_mini_frames_sync(struct vidow_view *view) {
	if (view == NULL) {
		return;
	}
	struct vidow_server *server = view->server;
	if (view->mini_snapshot != NULL) {
		if (view->last_buffer == NULL) {
			/* 窗口没了 / 还没交过帧：把快照收掉（留着会攥着一张过期缓冲）。 */
			wlr_scene_node_set_enabled(&view->mini_snapshot->node, false);
		} else if (view->mini_snapshot->buffer != view->last_buffer) {
			wlr_scene_buffer_set_buffer(view->mini_snapshot, view->last_buffer);
		}
	}
	/* 预览也吃同一份快照：不能出现「图标静止、预览还在动」。 */
	if (server->mini_preview_snap != NULL &&
			server->mini_preview_view == view &&
			view->last_buffer != NULL &&
			server->mini_preview_snap->buffer != view->last_buffer) {
		wlr_scene_buffer_set_buffer(server->mini_preview_snap, view->last_buffer);
	}
	/* 进快照态那一刻 last_buffer 还是空的话，这里补摆一次（客户端下一帧就到）。 */
	vidow_mini_frames_recheck(view);
}

/* 出帧策略（快照 / 活）跟**有效** force_render 走：规则 / IPC 把它改了之后，
 * 得有人把这一面接上 —— 快照那条路要把真实节点停到停车位（客户端停帧），活
 * 那条路要把它摆回图标那块。不接的话图标会「两边都不画」（快照关了、真实节点
 * 还在停车位上，看起来就是图标凭空消失），或者反过来一直冻着。
 *
 * 谁调用：换 last_buffer 的那条路（客户端交帧）、指针一动（用户刚改完多半就在
 * 鼠标上）、每帧的补帧循环（vidow_view_render_pump）。三条都便宜，且都幂等。 */
void vidow_mini_frames_recheck(struct vidow_view *view) {
	if (view == NULL || !view->mapped) {
		return;
	}
	bool was = view->mini_snapshot != NULL && view->mini_snapshot->node.enabled;
	mini_snapshot_place(view);
	bool now = view->mini_snapshot != NULL && view->mini_snapshot->node.enabled;
	if (was != now) {
		vidow_view_apply_anim(view);
	}
}

/* 预览的内容节点 / 缓冲：活镜面那条路是树里的 scene_surface，快照那条路是
 * 一个 scene_buffer 节点 —— 位置、尺寸、透明度三个出口都走这两个取。 */
static struct wlr_scene_node *preview_content_node(struct vidow_server *server) {
	if (server->mini_preview_snap != NULL) {
		return &server->mini_preview_snap->node;
	}
	if (server->mini_preview != NULL) {
		return &server->mini_preview->node;
	}
	return NULL;
}

static struct wlr_scene_buffer *preview_content_buffer(
		struct vidow_server *server) {
	if (server->mini_preview_snap != NULL) {
		return server->mini_preview_snap;
	}
	if (server->mini_preview_surface != NULL) {
		return server->mini_preview_surface->buffer;
	}
	return NULL;
}

/* 镜面树里那层内容节点（wlr_scene_subsurface_tree_create 建的第一层就是它；
 * 子表面另外挂。和 capturemask.c 的写法一致）。 */
static struct wlr_scene_surface *preview_find_surface(
		struct wlr_scene_tree *tree) {
	if (tree == NULL) {
		return NULL;
	}
	struct wlr_scene_node *node;
	wl_list_for_each(node, &tree->children, link) {
		if (node->type != WLR_SCENE_NODE_BUFFER) {
			continue;
		}
		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(
			wlr_scene_buffer_from_node(node));
		if (ss != NULL) {
			return ss;
		}
	}
	return NULL;
}

/* 预览那块矩形：迷你态尺寸 × 0.8，摆在图标的**朝屏内**那一侧（间距
 * mini_edge_margin），再夹进工作区。 */
static bool preview_box_for(struct vidow_server *server,
		struct vidow_view *view, struct wlr_box *out) {
	struct wlr_box work = vidow_work_area(server, view->output);
	if (work.width < 1 || work.height < 1) {
		return false;
	}
	double wpct = 28.0, hpct = 28.0;
	const char *tw = vidow_config_get(&server->config, "mini_w");
	const char *th = vidow_config_get(&server->config, "mini_h");
	if (tw != NULL) {
		vidow_config_parse_percent(tw, &wpct);
	}
	if (th != NULL) {
		vidow_config_parse_percent(th, &hpct);
	}
	int pw = (int)lround(wpct / 100.0 * 0.8 * (double)work.width);
	int ph = (int)lround(hpct / 100.0 * 0.8 * (double)work.height);
	if (pw < 1) {
		pw = 1;
	}
	if (ph < 1) {
		ph = 1;
	}
	/* 图标「画在哪」= geom + anim_off（和 vidow_place_view 同源）。 */
	int ix = view->geom.x + (int)lround(view->anim_off_x);
	int iy = view->geom.y + (int)lround(view->anim_off_y);
	int iw = view->geom.width, ih = view->geom.height;
	int margin = server->config.mini_edge_margin;
	if (margin < 0) {
		margin = 0;
	}
	int x = 0, y = 0;
	switch (view->mini_edge) {
	case VIDOW_MINI_EDGE_LEFT:
		x = ix + iw + margin;
		y = iy + (ih - ph) / 2;
		break;
	case VIDOW_MINI_EDGE_RIGHT:
		x = ix - margin - pw;
		y = iy + (ih - ph) / 2;
		break;
	case VIDOW_MINI_EDGE_TOP:
		x = ix + (iw - pw) / 2;
		y = iy + ih + margin;
		break;
	case VIDOW_MINI_EDGE_BOTTOM:
		x = ix + (iw - pw) / 2;
		y = iy - margin - ph;
		break;
	case VIDOW_MINI_EDGE_NONE:
		x = ix + iw + margin;
		y = iy;
		break;
	}
	if (x < work.x) {
		x = work.x;
	}
	if (y < work.y) {
		y = work.y;
	}
	if (x + pw > work.x + work.width) {
		x = work.x + work.width - pw;
	}
	if (y + ph > work.y + work.height) {
		y = work.y + work.height - ph;
	}
	*out = (struct wlr_box){ .x = x, .y = y, .width = pw, .height = ph };
	return true;
}

bool vidow_mini_preview_hit(struct vidow_server *server, double lx, double ly) {
	if (server == NULL || server->mini_preview_view == NULL ||
			!server->mini_preview_shown) {
		return false;
	}
	const struct wlr_box *b = &server->mini_preview_box;
	return lx >= b->x && lx < b->x + b->width &&
		ly >= b->y && ly < b->y + b->height;
}

static void preview_apply_alpha(struct vidow_server *server, double a) {
	if (a < 0.0) {
		a = 0.0;
	} else if (a > 1.0) {
		a = 1.0;
	}
	server->mini_preview_alpha = a;
	struct wlr_scene_buffer *buf = preview_content_buffer(server);
	if (buf != NULL) {
		wlr_scene_buffer_set_opacity(buf, (float)a);
	}
}

static void preview_fade_update(void *data, double t, double linear) {
	(void)linear;
	struct vidow_server *server = data;
	preview_apply_alpha(server, server->mini_preview_alpha_from +
		(server->mini_preview_alpha_to - server->mini_preview_alpha_from) * t);
}

static void preview_fade_done(void *data, bool cancelled) {
	struct vidow_server *server = data;
	server->mini_preview_fade = NULL;
	if (cancelled) {
		return;
	}
	if (server->mini_preview_alpha_to <= 0.0) {
		/* 淡出完：收节点、忘掉 view（节点留着复用，下次换个表面 / 换个模式就重建）。 */
		struct wlr_scene_node *node = preview_content_node(server);
		if (node != NULL) {
			wlr_scene_node_set_enabled(node, false);
		}
		/* 快照那份是客户端缓冲的一张裸引用（wlr_scene_buffer 一直攥着）：收起来
		 * 时松开，别替一个已经没了的窗口把缓冲扣着。活镜面那条路是
		 * scene_surface，缓冲跟着它自己的生命周期走。 */
		if (server->mini_preview_snap != NULL) {
			wlr_scene_buffer_set_buffer(server->mini_preview_snap, NULL);
		}
		server->mini_preview_view = NULL;
		server->mini_preview_shown = false;
		server->mini_preview_box = (struct wlr_box){0};
	}
}

static void preview_fade_to(struct vidow_server *server, double to, int ms) {
	vidow_anim_cancel(server->mini_preview_fade);
	server->mini_preview_fade = NULL;
	server->mini_preview_alpha_from = server->mini_preview_alpha;
	server->mini_preview_alpha_to = to;
	if (ms <= 0) {
		preview_apply_alpha(server, to);
		preview_fade_done(server, false);
		return;
	}
	struct vidow_ease linear = { .kind = VIDOW_EASE_LINEAR };
	server->mini_preview_fade = vidow_anim_start(server, "mini-preview-fade",
		ms, linear, preview_fade_update, preview_fade_done, server);
	if (server->mini_preview_fade == NULL) {
		preview_apply_alpha(server, to);
		preview_fade_done(server, false);
	}
}

void vidow_mini_preview_hide(struct vidow_server *server) {
	if (server == NULL) {
		return;
	}
	vidow_anim_cancel(server->mini_hover_timer);
	server->mini_hover_timer = NULL;
	server->mini_hover_view = NULL;
	server->mini_preview_pressed = false;
	if (!server->mini_preview_shown ||
			(server->mini_preview == NULL && server->mini_preview_snap == NULL)) {
		server->mini_preview_view = NULL;
		server->mini_preview_shown = false;
		server->mini_preview_box = (struct wlr_box){0};
		return;
	}
	wlr_log(WLR_DEBUG, "mini preview: 收起来（%dx%d @ %d,%d）",
		server->mini_preview_box.width, server->mini_preview_box.height,
		server->mini_preview_box.x, server->mini_preview_box.y);
	preview_fade_to(server, 0.0, server->config.mini_hover_fade_out_ms);
}

static void preview_show(struct vidow_server *server, struct vidow_view *view) {
	if (server == NULL || view == NULL || server->locked ||
			view->mini != VIDOW_MINI_ICON) {
		return;
	}
	struct wlr_surface *surface = vidow_view_surface(view);
	if (surface == NULL) {
		return;
	}
	struct wlr_box box = {0};
	if (!preview_box_for(server, view, &box)) {
		return;
	}
	/* 出帧策略和图标**同一份判据**：走快照那条路时预览也是静止的最后一帧。 */
	bool snapshot = vidow_mini_uses_snapshot(view);
	if (snapshot) {
		if (server->mini_preview != NULL) {
			wlr_scene_node_destroy(&server->mini_preview->node);
			server->mini_preview = NULL;
			server->mini_preview_surface = NULL;
		}
		if (server->mini_preview_snap == NULL) {
			server->mini_preview_snap = wlr_scene_buffer_create(
				server->layer_top, view->last_buffer);
			if (server->mini_preview_snap == NULL) {
				return;
			}
		} else if (server->mini_preview_snap->buffer != view->last_buffer) {
			wlr_scene_buffer_set_buffer(server->mini_preview_snap,
				view->last_buffer);
		}
	} else {
		if (server->mini_preview_snap != NULL) {
			wlr_scene_node_destroy(&server->mini_preview_snap->node);
			server->mini_preview_snap = NULL;
		}
		/* 换了窗口（或者第一次）：重建镜面 —— 同一张表面挂第二棵树是允许的
		 * （见 wincapture.c 那段注释）。 */
		if (server->mini_preview == NULL || server->mini_preview_view != view) {
			if (server->mini_preview != NULL) {
				wlr_scene_node_destroy(&server->mini_preview->node);
				server->mini_preview = NULL;
				server->mini_preview_surface = NULL;
			}
			/* 挂 layer_top（不是 layer_toplevels）：它在窗口之上、会话锁和 overview
			 * 都会把它整棵关掉（sessionlock.c / overview.c 那两个数组），而且
			 * **不在模糊底衬抓取 / 裁剪检查的遍历范围里**（fx.c 那两处走的是
			 * layer_toplevels）—— 挂错地方的话，悬停预览那块镜面会被当成一个窗口
			 * 抓进底衬里，留下一块鬼影。 */
			server->mini_preview = wlr_scene_subsurface_tree_create(
				server->layer_top, surface);
			if (server->mini_preview == NULL) {
				return;
			}
			server->mini_preview_surface = preview_find_surface(server->mini_preview);
		}
		if (server->mini_preview_surface == NULL) {
			return;
		}
	}
	struct wlr_scene_buffer *content = preview_content_buffer(server);
	struct wlr_scene_node *node = preview_content_node(server);
	if (content == NULL || node == NULL) {
		return;
	}
	/* 尺寸：contain 塞进预览盒（和 S3 的 view_mini_scale 同一条算法 —— 不拉伸、
	 * 按内容自己的比例），居中摆。 */
	int nw = 0, nh = 0;
	view_mini_native(view, &nw, &nh);
	int dw = box.width, dh = box.height;
	if (nw > 0 && nh > 0) {
		double s = (double)box.width / (double)nw;
		double sy = (double)box.height / (double)nh;
		if (sy < s) {
			s = sy;
		}
		dw = (int)lround((double)nw * s);
		dh = (int)lround((double)nh * s);
		if (dw < 1) {
			dw = 1;
		}
		if (dh < 1) {
			dh = 1;
		}
	}
	wlr_scene_buffer_set_dest_size(content, dw, dh);
	wlr_scene_node_set_position(node,
		box.x + (box.width - dw) / 2, box.y + (box.height - dh) / 2);
	wlr_scene_node_set_enabled(node, true);
	/* 压在别的窗口之上：它可能比这些窗口后建出来（新开的窗口挂在后面）。 */
	wlr_scene_node_raise_to_top(node);
	server->mini_preview_view = view;
	server->mini_preview_box = box;
	server->mini_preview_shown = true;
	preview_apply_alpha(server, 0.0);
	preview_fade_to(server, 1.0, server->config.mini_hover_fade_in_ms);
	wlr_log(WLR_INFO, "mini preview: %s 的预览在 %d,%d %dx%d（图标 %d,%d %dx%d，"
		"贴 %s 边 → 朝屏内展开，%s）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
		box.x, box.y, box.width, box.height,
		view->geom.x, view->geom.y, view->geom.width, view->geom.height,
		view->mini_edge == VIDOW_MINI_EDGE_LEFT ? "左" :
			view->mini_edge == VIDOW_MINI_EDGE_RIGHT ? "右" :
			view->mini_edge == VIDOW_MINI_EDGE_TOP ? "上" :
			view->mini_edge == VIDOW_MINI_EDGE_BOTTOM ? "下" : "（无）",
		snapshot ? "快照（force_render 关，静止的最后一帧）" :
			"活镜面（force_render 开，动的）");
}

/* 指针是不是在「图标 → 预览」那条**空隙走廊**里（正朝预览走）：这时候用
 * mini_icon_hover_grace_ms（200ms）而不是离开的 300ms —— 跨那 8px 空隙本来就要
 * 一两帧「既不在图标也不在预览」，按 300ms 收会让用户看到闪一下再回来。
 * 移向别处（不在走廊里）才按 mini_icon_hover_leave_ms 收。 */
static bool preview_in_corridor(struct vidow_server *server, double lx,
		double ly) {
	if (!server->mini_preview_shown || server->mini_preview_view == NULL) {
		return false;
	}
	struct vidow_view *view = server->mini_preview_view;
	struct wlr_box prev = server->mini_preview_box;
	struct wlr_box icon = {
		.x = view->geom.x + (int)lround(view->anim_off_x),
		.y = view->geom.y + (int)lround(view->anim_off_y),
		.width = view->geom.width,
		.height = view->geom.height,
	};
	int margin = server->config.mini_edge_margin;
	if (margin < 0) {
		margin = 0;
	}
	int y1 = icon.y < prev.y ? icon.y : prev.y;
	int y2 = icon.y + icon.height > prev.y + prev.height ?
		icon.y + icon.height : prev.y + prev.height;
	struct wlr_box gap = {0};
	if (prev.x + prev.width <= icon.x) {          /* 预览在图标左边 */
		gap = (struct wlr_box){ .x = prev.x + prev.width, .y = y1,
			.width = icon.x - (prev.x + prev.width), .height = y2 - y1 };
	} else if (icon.x + icon.width <= prev.x) {   /* 预览在图标右边 */
		gap = (struct wlr_box){ .x = icon.x + icon.width, .y = y1,
			.width = prev.x - (icon.x + icon.width), .height = y2 - y1 };
	} else if (prev.y + prev.height <= icon.y) {  /* 预览在图标上方 */
		gap = (struct wlr_box){ .x = 0, .y = prev.y + prev.height,
			.width = 0, .height = icon.y - (prev.y + prev.height) };
		int x1 = icon.x < prev.x ? icon.x : prev.x;
		int x2 = icon.x + icon.width > prev.x + prev.width ?
			icon.x + icon.width : prev.x + prev.width;
		gap.x = x1;
		gap.width = x2 - x1;
	} else if (icon.y + icon.height <= prev.y) {  /* 预览在图标下方 */
		int x1 = icon.x < prev.x ? icon.x : prev.x;
		int x2 = icon.x + icon.width > prev.x + prev.width ?
			icon.x + icon.width : prev.x + prev.width;
		gap = (struct wlr_box){ .x = x1, .y = icon.y + icon.height,
			.width = x2 - x1, .height = prev.y - (icon.y + icon.height) };
	} else {
		return false; /* 两块贴在一起（没有空隙）：不存在「跨空隙」 */
	}
	/* 走廊在法线方向放宽 margin（指针抖一点也还算在走廊里），纵向也要覆盖
	 * 两块盒子合起来的范围（指针从图标往预览斜着走）。 */
	gap.x -= margin;
	gap.y -= margin;
	gap.width += margin * 2;
	gap.height += margin * 2;
	if (gap.width < 1) {
		gap.width = 1;
	}
	if (gap.height < 1) {
		gap.height = 1;
	}
	return lx >= gap.x && lx < gap.x + gap.width &&
		ly >= gap.y && ly < gap.y + gap.height;
}

static void hover_timer_done(void *data, bool cancelled) {
	struct vidow_server *server = data;
	bool leaving = server->mini_hover_leaving;
	struct vidow_view *view = server->mini_hover_view;
	server->mini_hover_timer = NULL;
	server->mini_hover_view = NULL;
	if (cancelled) {
		return;
	}
	if (leaving) {
		vidow_mini_preview_hide(server);
	} else {
		preview_show(server, view);
	}
}

static void hover_timer_start(struct vidow_server *server, int ms,
		bool leaving, struct vidow_view *view) {
	vidow_anim_cancel(server->mini_hover_timer);
	server->mini_hover_timer = NULL;
	if (ms <= 0) {
		ms = 1;
	}
	server->mini_hover_leaving = leaving;
	server->mini_hover_view = view;
	struct vidow_ease linear = { .kind = VIDOW_EASE_LINEAR };
	server->mini_hover_timer = vidow_anim_start(server, "mini-hover", ms,
		linear, NULL, hover_timer_done, server);
}

void vidow_mini_preview_motion(struct vidow_server *server,
		struct vidow_view *hit) {
	if (server == NULL || server->cursor == NULL) {
		return;
	}
	if (!server->config.mini_enabled || !server->config.mini_icon_preview) {
		vidow_mini_preview_hide(server);
		return;
	}
	struct vidow_view *icon = (hit != NULL && hit->mini == VIDOW_MINI_ICON) ?
		hit : NULL;
	/* 用户刚用 IPC / 规则把 force_render 改了的话，指针一动就把出帧策略接上
	 * （改完多半鼠标就动起来了 —— 快照态下客户端已经停帧，没有提交来触发
	 * 重新摆位，指针一动是最靠得住的那个触发点）。 */
	struct vidow_view *iv;
	wl_list_for_each(iv, &server->views, link) {
		if (iv->mini == VIDOW_MINI_ICON || iv->mini_snapshot != NULL) {
			vidow_mini_frames_recheck(iv);
		}
	}
	if (icon == NULL) {
		/* 指针在预览上（只读，但算「还在看它」）→ 清掉离开计时 = 宽限期。 */
		if (vidow_mini_preview_hit(server, server->cursor->x,
				server->cursor->y)) {
			vidow_anim_cancel(server->mini_hover_timer);
			server->mini_hover_timer = NULL;
			server->mini_hover_view = NULL;
			return;
		}
		/* 既不在图标也不在预览：
		 *   - 在「图标 → 预览」那条空隙走廊里（正朝预览走）→ 用宽限期
		 *     mini_icon_hover_grace_ms（200ms），而且**每动一下重起**：人还
		 *     在往预览走，不该被收走；
		 *   - 移向别处 → 用 mini_icon_hover_leave_ms（300ms），**已经起着就
		 *     不重起**（不然每动一下都推倒重来，永远收不掉）。 */
		bool corridor = preview_in_corridor(server, server->cursor->x,
			server->cursor->y);
		if (corridor) {
			hover_timer_start(server,
				server->config.mini_icon_hover_grace_ms, true, NULL);
			return;
		}
		if ((server->mini_preview_shown ||
				server->mini_hover_timer != NULL) &&
				(server->mini_hover_timer == NULL ||
					!server->mini_hover_leaving)) {
			hover_timer_start(server,
				server->config.mini_icon_hover_leave_ms, true, NULL);
		}
		return;
	}
	/* 在图标上：离开计时作废。 */
	if (server->mini_hover_timer != NULL && server->mini_hover_leaving) {
		vidow_anim_cancel(server->mini_hover_timer);
		server->mini_hover_timer = NULL;
	}
	if (server->mini_preview_shown && server->mini_preview_view == icon) {
		/* 已经露着 —— 但出帧策略可能刚翻面（force_render 被 IPC / 规则改了）：
		 * 那就重建一次，免得「图标活了预览还冻着」（或反过来）—— 两条路必须
		 * 一致。 */
		if ((server->mini_preview_snap != NULL) !=
				vidow_mini_uses_snapshot(icon)) {
			preview_show(server, icon);
		}
		return;
	}
	if (server->mini_hover_timer != NULL &&
			server->mini_hover_view == icon) {
		return; /* 延迟计时正跑着 */
	}
	hover_timer_start(server, server->config.mini_icon_hover_ms, false, icon);
}

/* ------------------------------------------------------------------ 最小化 */

/* 最小化 / 放回。窗口**留在原来的列里**（列宽、上下顺序、滚动位置、归属工作区和
 * 归属屏全都不用重建，放回来是瞬时的），只是不占位、不显示、不参与焦点：
 *
 *   - 布局：arrange 跳过它；整列都最小化了，那条列从条带里消失（剩下的列合拢）；
 *   - 可见性：vidow_workspace_view_visible() 说它不可见 → 场景节点关掉，跟「工作区
 *     被藏起来」是同一套；
 *   - 补帧：**规则要求保活（force_render）或者有人正在录 / 预览的窗口照样出帧** ——
 *     补帧那条路的判据是 view_on_screen()（最小化让它变假），所以它反而进了补帧分支。
 *     这里不许加「最小化就不补帧」的判断，那会把录屏录成一张冻住的画面。
 *   - 客户端：X11 要告诉它（WM_STATE=IconicState + _NET_WM_STATE_HIDDEN），Wayland
 *     回一次 configure（xdg-shell 规定状态请求必须有应答），任务栏（foreign toplevel）
 *     跟着更新状态。 */
void vidow_view_set_minimized(struct vidow_view *view, bool minimized) {
	if (view == NULL || view->popup || view->minimized == minimized) {
		return;
	}
	struct vidow_server *server = view->server;
	struct wlr_output *output = view->output;
	const char *title = vidow_view_title(view) != NULL ?
		vidow_view_title(view) : "(无题)";

	if (minimized) {
		/* 全屏的先退出全屏：布局里 fullscreen 那条分支不看占位，留着它会一直
		 * 占着整块屏。 */
		vidow_view_unfullscreen(view, "最小化");
		view->minimized_seq = ++server->next_minimize_seq;
	}
	view->minimized = minimized;

	if (view->kind == VIDOW_VIEW_X11 && view->x11 != NULL) {
		wlr_xwayland_surface_set_minimized(view->x11, minimized);
	} else if (view->xdg_toplevel != NULL &&
			vidow_view_configure_ready(view)) {
		/* 客户端第一帧还没来（surface 没 initialized）就 set_minimized 的话，
		 * 这里排 configure 会踩 schedule_configure 里那句 assert —— 见
		 * view_request_maximize 的说明；应答由初始 configure 捎过去。 */
		wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
	}
	if (view->ftm_handle != NULL) {
		wlr_foreign_toplevel_handle_v1_set_minimized(view->ftm_handle, minimized);
	}

	wlr_log(WLR_INFO, "最小化: %s %s（%s）", title,
		minimized ? "收起" : "放回",
		view->ws != NULL ? view->ws->name : "-");

	if (minimized && server->focused == view) {
		/* 焦点交给邻居 —— 跟关窗同一套规矩（位置不动，谁滑到空出来的位置就是谁）。
		 * 必须在把窗口摘出布局**之前**找（邻居要靠它还在列里才找得到）。 */
		struct vidow_view *next = focus_after_close(server, view, output);
		server->focused = NULL;
		if (next != NULL) {
			vidow_focus_view(next);
		} else {
			focus_nearest_column(server, output);
		}
	}

	vidow_workspace_apply_visibility(server);
	vidow_arrange(server);
	/* 显隐翻转的是它自己这一块（重排里已经按每个窗口记过账了）。 */
	{
		struct wlr_box box = vidow_view_screen_box(view);
		vidow_backdrop_damage_region(server, &box);
	}
	vidow_ipc_emit_window(server, minimized ? "window-minimized" :
		"window-unminimized", view);
}

/* 放回一个最小化的窗口 + 给焦点。跨屏 / 跨工作区都会跟过去 —— 走的是「点通知」那条
 * 路（vidow_view_activate：先把工作区端出来，再给键盘）。没最小化的窗口调它没关系
 * （activate 自己会判断该不该理）。 */
void vidow_view_restore(struct vidow_view *view) {
	if (view == NULL || !view->minimized) {
		return;
	}
	vidow_view_set_minimized(view, false);
	vidow_view_activate(view);
}

/* 放回「最近收起来的那个」：优先当前聚焦屏上的，那块屏上没有才跨屏挑。 */
bool vidow_view_restore_last(struct vidow_server *server) {
	struct vidow_view *best = NULL;      /* 聚焦屏上最新的 */
	struct vidow_view *best_any = NULL;  /* 全屏范围内最新的 */
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (!view->minimized) {
			continue;
		}
		if (best_any == NULL || view->minimized_seq > best_any->minimized_seq) {
			best_any = view;
		}
		if (view->output == server->focused_output &&
				(best == NULL || view->minimized_seq > best->minimized_seq)) {
			best = view;
		}
	}
	struct vidow_view *pick = best != NULL ? best : best_any;
	if (pick == NULL) {
		return false;
	}
	vidow_view_restore(pick);
	return true;
}

bool vidow_view_toggle_minimized(struct vidow_view *view) {
	if (view == NULL || view->popup) {
		return false;
	}
	if (view->minimized) {
		vidow_view_restore(view);
	} else {
		vidow_view_set_minimized(view, true);
	}
	return true;
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
		/* 整列都最小化的列跳过：焦点不该落进一条看不见的列。 */
		if (column_has_shown_views(c)) {
			vidow_focus_view(column_first_shown_view(c));
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

	/* 同列上下：跳过最小化的（它不占位，也不接键盘）。 */
	struct wl_list *node = direction > 0 ? view->column_link.next :
		view->column_link.prev;
	while (node != &view->column->views) {
		struct vidow_view *next = wl_container_of(node, next, column_link);
		if (view_shows(next)) {
			vidow_focus_view(next);
			return;
		}
		node = direction > 0 ? node->next : node->prev;
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

/* 插进 `target` 这条列的第 index 位（niri 的 insert into column：一列里上下叠
 * 起来）。index 数的是「目标列里现在还占着位置的窗口（不含正被拖动 / 正搬家的这
 * 一个）的第几位」—— 和落点算法（drag.c 的 insert_pos_at）数的是同一份东西：插
 * 在第 index 个**还占位置的**窗口之前，数不够就放在最后。
 *
 * 最小化的窗口不算位置（它不占条带），所以插进去的时候也是插在它们前面 —— 它们
 * 原来在上下哪一段就留在哪一段。
 *
 * 同一条列（拖自己那条列里的上下顺序）：就地重排。
 * 别的列：从原来那条列里摘出来（列空了就拆掉），再插进目标列 —— 跨工作区的调用
 * 方负责先把 view->ws / output 换过去（见 vidow_workspace_move_view_into_column）。 */
bool vidow_view_insert_into_column(struct vidow_view *view,
		struct vidow_column *target, int index) {
	if (view == NULL || target == NULL || view->popup) {
		return false;
	}
	if (index < 0) {
		index = 0;
	}
	struct vidow_column *old = view->column;
	if (old == target) {
		wl_list_remove(&view->column_link);
	} else {
		/* 记住「并进别人列之前」自己的列宽：再拖出来时恢复它，而不是把
		 * 目标列的宽度带走。已经在别的列里（home 已记）就不覆盖。 */
		if (old != NULL && !view->column_width_home_set) {
			view->column_width_home = old->width_fraction;
			view->column_width_home_set = true;
			wlr_log(WLR_INFO, "column: %s 并进别的列，记住原列宽 %.2f",
				vidow_view_title(view) != NULL ?
					vidow_view_title(view) : "?",
				view->column_width_home);
		}
		vidow_view_leave_column(view);
	}

	view->column = target;
	struct wl_list *at = &target->views; /* 默认：放在最后 */
	int i = 0;
	struct vidow_view *it;
	wl_list_for_each(it, &target->views, column_link) {
		if (!view_shows(it)) {
			continue;
		}
		if (i == index) {
			at = &it->column_link;
			break;
		}
		i++;
	}
	/* wl_list_insert(pos, elm) 是插在 pos 之后 —— 插到 at 前面要写 at->prev。 */
	wl_list_insert(at->prev, &view->column_link);
	if (old != target) {
		/* 这条窗口现在住在列里了：列里的窗口不算浮窗。 */
		vidow_workspace_track(target->ws, view, false);
	}
	return true;
}

/* 把窗口从它那条列里拎出来、单独成一条新列（niri 的 expel-window-from-column）。
 * 这是「并进一列」的逆操作：并进一列有两条路（拖拽落点 / move-window-direction），
 * 取出来以前一条都没有 —— 用户报的「窗口放入列后无法取出」就是它。
 *
 * 插在 ws 的列带里 `at` 那个位置**之前**（at = &ws->columns 就是追加到末尾；
 * NULL = 插在原来那条列右边）。落点由调用方算：动作 / IPC 那条路是「原来那条列的
 * 右边 / 左边」，拖拽那条路是落点算法（drag.c 的 insert_pos_at）给的那个缝 ——
 * 两边的「第几位」数的是同一份东西。
 *
 * 列里只剩它一个的时候没什么可取的（它本来就是独立一条列），浮窗 / 弹出物本来就
 * 不在列里 —— 这两种都返回 NULL，调用方报一句人话 / 走老路。
 *
 * 宽度跟着原来那条列走（同一个 width_fraction）：取出来是为了单独挪它 / 单独调它
 * 的宽度，尺寸突变一下没有意义。**列最大化状态不带过来** —— 新列不该一出生就是
 * 满宽（用户按下「取出来」要的是「它自己一条列」）。reason 只进日志。 */
struct vidow_column *vidow_view_expel_new_column(struct vidow_view *view,
		struct vidow_workspace *ws, struct wl_list *at, const char *reason) {
	if (view == NULL || ws == NULL || view->popup ||
			view->float_mode != VIDOW_FLOAT_NONE || view->column == NULL ||
			!view_shows(view)) {
		return NULL;
	}
	struct vidow_column *col = view->column;
	if (col->ws == NULL || col->ws != ws) {
		return NULL;
	}
	/* 列里还有别的**看得见**的窗口，才算「它在一条多窗口的列里」。收起来的
	 * （最小化）不算 —— 它们在布局里不占位，取出来对它们没有影响。 */
	bool stacked = false;
	struct vidow_view *it;
	wl_list_for_each(it, &col->views, column_link) {
		if (it != view && view_shows(it)) {
			stacked = true;
			break;
		}
	}
	if (!stacked) {
		return NULL;
	}

	struct vidow_server *server = view->server;
	struct vidow_column *ncol = calloc(1, sizeof(*ncol));
	if (ncol == NULL) {
		return NULL;
	}
	ncol->server = server;
	ncol->id = ++server->next_column_id;
	ncol->output = col->output;
	ncol->ws = col->ws;
	/* 取出来时优先恢复「并进这条列之前」自己的宽度；没有记录才继承当前列宽
	 * （直接 expel 一个本来就在这条列里的窗口走这一支）。 */
	ncol->width_fraction = view->column_width_home_set ?
		view->column_width_home : col->width_fraction;
	view->column_width_home_set = false;
	/* 新列继承的是**目标**宽度（原列正过渡到一半也照目标给），新列自己不过渡。 */
	ncol->width_shown = ncol->width_fraction;
	wl_list_init(&ncol->views);
	wl_list_init(&ncol->link);

	vidow_view_leave_column(view);
	view->column = ncol;
	wl_list_insert(ncol->views.prev, &view->column_link);
	if (at == NULL) {
		at = col->link.next; /* 默认：原来那条列的右边 */
	}
	/* wl_list_insert(pos, elm) 是插在 pos 之后 —— 插到 at 前面要写 at->prev。 */
	wl_list_insert(at->prev, &ncol->link);
	/* 和 insert_into_column 一样：这条窗口现在住在列里（不是浮窗）。 */
	vidow_workspace_track(ncol->ws, view, false);

	vidow_arrange(server);
	vidow_ipc_emit_column(server, "column-expel-window", ncol);
	wlr_log(WLR_INFO, "取出窗口: %s 从列 #%llu 到新列 #%llu（%s）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
		(unsigned long long)col->id, (unsigned long long)ncol->id,
		reason != NULL ? reason : "取出来");
	return ncol;
}

/* 动作 / IPC 那条路：把窗口从它那条列里拎出来单独成一条新列。
 * direction > 0 = 新列插在原来那条列**右边**（niri 的默认），< 0 = 左边。
 * 取不出来（浮窗 / 列里就它一个）返回 false，调用方报一句人话。 */
bool vidow_view_expel_from_column(struct vidow_view *view, int direction) {
	if (view == NULL || view->column == NULL) {
		return false;
	}
	struct vidow_column *col = view->column;
	/* at 得在摘之前算：wl_list_insert(at->prev, …) 插在 at 前面，而
	 * col->link.next 就是「原来那条列的右边」那个位置。 */
	struct wl_list *at = direction > 0 ? col->link.next : &col->link;
	return vidow_view_expel_new_column(view, col->ws, at,
		direction > 0 ? "右边" : "左边") != NULL;
}

/* 第 index 个「还占着位置」的窗口（0 起数，越界返回 NULL）。 */
static struct vidow_view *column_shown_view_at(struct vidow_column *col,
		int index) {
	if (index < 0) {
		return NULL;
	}
	struct vidow_view *view;
	wl_list_for_each(view, &col->views, column_link) {
		if (view_shows(view) && index-- == 0) {
			return view;
		}
	}
	return NULL;
}

/* `view` 在它那条列里排第几（按「还占着位置」的数；它自己最小化时 -1）。 */
static int column_shown_index(struct vidow_view *view) {
	if (view == NULL || view->column == NULL || !view_shows(view)) {
		return -1;
	}
	int index = 0;
	struct vidow_view *it;
	wl_list_for_each(it, &view->column->views, column_link) {
		if (it == view) {
			return index;
		}
		if (view_shows(it)) {
			index++;
		}
	}
	return -1;
}

/* 换位置（vidow_swap_window）之前，这个窗口**要求**自己那格多宽。
 *
 * 换位置是「窗口搬进对方那一格」，宽度跟着窗口走，所以目标宽度得按窗口自己的
 * 来源重算，不能直接沿用格子里原先那个数。来源优先级和建列时一致（见
 * column_new_for 与 insert_into_column）：
 *   ① 窗口规则写了 column_width —— 最强，按 app_id / title 匹配的那个；
 *   ② 这个窗口「并进别的列之前」记住的原列宽（column_width_home）；
 *   ③ 都没有 → 维持这一格现在的宽度（返回 fallback）。
 * 列最大化（maximized）的格子不进这条判断：它的宽度是 1.0 撑满，换人进来
 * 还是撑满，vidow_column_set_width 会把 maximized 状态自己接上。 */
static double view_swap_width(const struct vidow_view *view,
		const struct vidow_column *fallback) {
	if (view == NULL) {
		return fallback != NULL ? fallback->width_fraction : 0.5;
	}
	if (view->rule_eff.set[VRA_COLUMN_WIDTH]) {
		return view->rule_eff.val[VRA_COLUMN_WIDTH].d;
	}
	if (view->column_width_home_set) {
		return view->column_width_home;
	}
	return fallback != NULL ? fallback->width_fraction : 0.5;
}

/* 和相邻那列里的窗口**换个位置**（niri 那种 swap）：两个窗口各自住进对方那一格，
 * 焦点跟着自己走 —— 列里只有一个窗口时（常见情况）就是两条列的内容对调。
 *
 * 跟谁换：目标列里「纵向同一层」那个（层数不够就取最后一个）。一列里叠了好几个
 * 窗口时，这样换完两个窗口都还在原来那一层，不会跳层。整列都最小化了的列跳过
 * （它不占条带，换过去等于凭空挪一屏）。 */
bool vidow_swap_window(struct vidow_server *server, int direction) {
	struct vidow_view *view = server->focused;
	if (view == NULL || view->column == NULL ||
			view->float_mode != VIDOW_FLOAT_NONE ||
			!view_shows(view)) {
		return false;
	}
	struct vidow_column *col = view->column;
	if (col->ws == NULL || direction == 0) {
		return false;
	}
	int index = column_shown_index(view);

	struct wl_list *node = direction > 0 ? col->link.next : col->link.prev;
	while (node != &col->ws->columns) {
		struct vidow_column *other = wl_container_of(node, other, link);
		struct vidow_view *mate =
			column_shown_view_at(other, index);
		if (mate == NULL && column_has_shown_views(other)) {
			/* 对方那列层数不够：换成它最后一个（最靠下的那个）。 */
			struct vidow_view *last = NULL;
			struct vidow_view *it;
			wl_list_for_each(it, &other->views, column_link) {
				if (view_shows(it)) {
					last = it;
				}
			}
			mate = last;
		}
		if (mate == NULL || mate == view) {
			node = direction > 0 ? node->next : node->prev;
			continue;
		}
		/* 两格的前驱都得先记下来：wl_list_remove 会把节点的 prev/next 清掉。 */
		struct wl_list *a_prev = view->column_link.prev;
		struct wl_list *b_prev = mate->column_link.prev;
		wl_list_remove(&view->column_link);
		wl_list_remove(&mate->column_link);
		wl_list_insert(b_prev, &view->column_link);
		wl_list_insert(a_prev, &mate->column_link);
		view->column = other;
		mate->column = col;
		/* 宽度跟着**窗口**走（README 的语义：「两个窗口各自住进对方那一格」，
		 * 焦点也跟着窗口）。只换 wl_list 指针不够 —— 两条列的 width_fraction
		 * 各是各的，换完窗口就继承了**对方格子的宽度**，屏上看起来正是
		 * 「半宽窗口和全宽窗口交换时连宽度都交换了」（用户 2026-10-20 报的）。
		 *
		 * 两个值都得在换列**之前**算：换完 col / other 里住的是新住户，
		 * 再问「这一格原来多宽」就问不到了。 */
		double vw = view_swap_width(view, col);
		double mw = view_swap_width(mate, other);
		vidow_column_set_width(col, mw, false);
		vidow_column_set_width(other, vw, false);
		vidow_arrange(server);
		return true;
	}
	return false;
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

/* ---------------------------------------------------------- 列宽过渡 */

/* 这一帧该按哪个比例画。补间在跑就是补间当前值，否则就是逻辑值。 */
static double column_layout_fraction(const struct vidow_column *col) {
	return col->anim_width != NULL ? col->width_shown : col->width_fraction;
}

static void column_width_update(void *data, double t, double linear) {
	(void)linear;
	struct vidow_column *col = data;
	col->width_shown = col->anim_from + (col->anim_to - col->anim_from) * t;
	/* 重排推到下一帧的开头去做（output.c 的 arrange_dirty）。**不能在这儿直接
	 * 调 vidow_arrange**：arrange 会掐掉 / 新起别的补间，而我们此刻正站在
	 * anim.c 的遍历里，动的可能就是它手里那个 tmp 指针。 */
	col->server->arrange_dirty = true;
}

static void column_width_done(void *data, bool cancelled) {
	(void)cancelled;
	struct vidow_column *col = data;
	col->anim_width = NULL;
	if (col->server->column_anim_n > 0) {
		col->server->column_anim_n--;
	}
	col->server->arrange_dirty = true;
}

void vidow_column_set_width(struct vidow_column *col, double fraction, bool animate) {
	if (col == NULL) {
		return;
	}
	struct vidow_server *server = col->server;
	/* 起点取「现在画在屏上的那个值」，不是逻辑值 —— 过渡没跑完就再改一次时，
	 * 得从眼前这个宽度接着走，不能跳回上一次的目标。**必须在 cancel 之前取**：
	 * cancel 会把 anim_width 清掉，之后就问不出中间值了。 */
	double from = column_layout_fraction(col);

	col->width_fraction = fraction;

	int ms = server->config.column_width_ms;
	if (!animate || ms <= 0 || from == fraction) {
		vidow_anim_cancel(col->anim_width);
		col->anim_width = NULL;
		col->width_shown = fraction;
		/* **当场重排**，不能只挂个 arrange_dirty：没有过渡就没有动画在推帧
		 * （动画那条路靠看门狗保证帧），静态屏幕上下一帧可能几百毫秒之后才来
		 * —— 一次「直接到位」的改宽得当场就位。 */
		vidow_arrange(server);
		return;
	}

	vidow_anim_cancel(col->anim_width);
	col->anim_width = NULL;
	col->anim_from = from;
	col->anim_to = fraction;
	col->anim_width = vidow_anim_start(server, "column-width", ms,
		server->config.column_width_easing, column_width_update,
		column_width_done, col);
	if (col->anim_width == NULL) {
		/* 分配失败：别把宽度卡在中间，直接到位。 */
		col->width_shown = fraction;
		vidow_arrange(server);
		return;
	}
	server->column_anim_n++;
	/* 起点当场摆一次：从按下到下一帧之间不该还停在上一次的宽度上。 */
	column_width_update(col, 0.0, 0.0);
	wlr_log(WLR_DEBUG, "column-width tween: %.3f -> %.3f（%dms）", from,
		fraction, ms);
}

void vidow_cycle_column_width(struct vidow_server *server) {
	struct vidow_view *view = server->focused;
	if (view == NULL || view->column == NULL) {
		return;
	}

	/* 预设表来自配置（preset_column_widths，configs/20-layout.lua）：默认
	 * 1/3、1/2、2/3，就是原来写死在这儿的那三个。表为空的话兜一个 0.5 ——
	 * 配置解析保证至少一档，这里只是不给「空表除以零」的机会。 */
	struct vidow_layout lay;
	vidow_layout_for(server, view->output, &lay);
	const double *presets = lay.preset_column_widths;
	size_t n = (size_t)lay.preset_column_widths_count;
	if (n == 0) {
		static const double fallback[] = { 0.5 };
		presets = fallback;
		n = 1;
	}
	struct vidow_column *col = view->column;

	/* 手动改宽度 = 退出「列最大化」这个状态：先把它按下来（宽度留在 1.0，
	 * 于是下面按预设找「当前是第几档」时它落在最后一档之后，循环回到 1/3 ——
	 * 也就是「满宽再循环一次回到最窄」）。 */
	col->maximized = false;

	/* 「现在是第几档」按**目标**（width_fraction）数，不按画到一半的值 ——
	 * 连按几下要一档一档往上走，不能因为动画没跑完就原地打转。 */
	size_t current = n - 1;
	for (size_t i = 0; i < n; i++) {
		if (col->width_fraction < presets[i] + 0.01) {
			current = i;
			break;
		}
	}
	vidow_column_set_width(col, presets[(current + 1) % n], true);
	vidow_arrange(server);
}

void vidow_column_set_maximized(struct vidow_column *col, bool maximized) {
	if (col == NULL || col->maximized == maximized) {
		return;
	}
	struct vidow_server *server = col->server;

	if (maximized) {
		col->maximized_saved = col->width_fraction;
		col->maximized = true;
		vidow_column_set_width(col, 1.0, true);
	} else {
		col->maximized = false;
		vidow_column_set_width(col, col->maximized_saved, true);
	}

	/* 列里每个客户端都被告知一声：它们自己也知道自己现在是不是最大化的
	 * （xdg 的 set_maximized / X11 的 _NET_WM_STATE_MAXIMIZED_*）—— 客户端
	 * 以为自己是普通尺寸、布局却当它最大化，那种「一半一半」的状态最难看。
	 * 状态没变的话 vidow_view_set_maximized 自己会挡掉（去重在它里面）。
	 *
	 * 顺带把「谁要的」那笔账理一遍：**关掉**最大化的时候作废（这一轮最大化结束
	 * 了，下次客户端再撤销就不该算在它头上）；**打开**的时候不动 —— 客户端请求
	 * 那条路正是先立标记、再调这儿（见 view_request_maximize），在这儿清掉的话
	 * 它自己的撤销请求就没人理了。 */
	struct vidow_view *view;
	wl_list_for_each(view, &col->views, column_link) {
		if (!maximized) {
			view->maximize_granted = false;
		}
		vidow_view_set_maximized(view, maximized);
	}

	vidow_arrange(server);
	vidow_ipc_emit_column(server, "column-maximize-changed", col);
}

void vidow_column_toggle_maximized(struct vidow_server *server) {
	struct vidow_view *view = server->focused;
	if (view == NULL || view->column == NULL) {
		return;
	}
	vidow_column_set_maximized(view->column, !view->column->maximized);
}

void vidow_view_toggle_fullscreen(struct vidow_view *view) {
	view_apply_fullscreen(view, !view->fullscreen, "toggle-fullscreen（Mod+F / IPC）");
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

/* 「表面还没提交缓冲区」的窗口：等第一帧到了再补一次归属判定。
 *
 * 抄的是 Hyprland 的 considerMap()（src/xwayland/XSurface.cpp）：map 那一刻
 * 表面可能还没有 texture，这时尺寸（current.width/height）全是 0，拿它做
 * 任何判断都是错的 —— Hyprland 干脆什么都不做，等 commit 里再调一次。
 *
 * 我们这边 view_map() 已经把 mapped 置位了，commit 不会再走一遍 view_map，
 * 所以这里补一个等价入口：条件成立时把窗口落进工作区（不进布局的那批
 * 早在 view_map 里就 return 了，has_buffer 到了也不该进来）。 */
static void view_reconsider_map(struct vidow_view *view);

/* 弹出物判据（定义见文件后半段）—— view_map 的辅助窗口判据要用它。 */
static bool x11_is_popup(const struct wlr_xwayland_surface *x);
static void x11_trace_view(const char *event, struct vidow_view *view,
		struct wlr_surface *surface);
static bool x11_window_is_viewable(struct vidow_server *server,
		const struct wlr_xwayland_surface *x);
static void x11_replay_map_if_xmapped(struct vidow_view *view,
		bool trust_x_viewable);
/* 已认过账但场景节点还关着 → 重新打开（见 view_commit 里的调用点）。 */
static void x11_remap_if_node_closed(struct vidow_view *view);
static void view_map(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, map);
	struct vidow_server *server = view->server;
	x11_trace_view("map-enter", view, NULL);
	/* XWayland reassociation may synthesize map before the real map signal
	 * arrives. Keep the lifecycle edge idempotent: duplicate map handling would
	 * reapply rules, focus, animations, and window-opened notifications. */
	if (view->mapped) {
		return;
	}

	view->mapped = true;
	vidow_portal_view_mapped(view, true);
	/* 窗口规则先判一遍：float / workspace / output 决定它落在哪儿、怎么摆，
	 * 下面「归哪块工作区、平铺还是浮动」的判据都要看它（app_id / 标题这时
	 * 都齐了）。开窗只兑现一次的那几条在工作区落定之后（vidow_rule_placement）。 */
	vidow_rule_apply(server, view);
	/* X11 的窗口可能「状态比表面先到」：全屏那条属性在这儿先兑现一次。 */
	view_sync_x11_fullscreen(view);

	/* ---------------------------------------------------- 尺寸异常窗口
	 *
	 * 拿 Hyprland 的做法当参照（src/xwayland/XWM.cpp 的 handleMapRequest 和
	 * XSurface.cpp 的 considerMap），它**没有**「把窗口藏起来」这套机制：
	 *
	 *   1. considerMap() 只看一件事 —— **表面有没有提交过缓冲区**
	 *      （m_surface->m_current.texture）。没有就什么都不做，等下一次
	 *      commit 再来（commit 里会再调一次 considerMap）。有了就 map。
	 *   2. handleMapRequest() 里对「太小」的窗口**不隐藏，而是 configure 到
	 *      800x800** 让它变成正常窗口：
	 *          SMALL = geometry.size() < {2,2} || size < sizeHints.min
	 *          if (SMALL && !overrideRedirect) configure({pos, {800,800}});
	 *   3. override_redirect 只记状态（updateOverrideRedirect），从不藏。
	 *
	 * 也就是说 Hyprland 宁可让一个 1x1 窗口占位，也绝不把它从画面里拿掉。
	 *
	 * 我们原来那条「≤1x1 → 关场景节点 + focus_helper + return」正好反着：
	 * 关掉的节点要等 unmap 才恢复，于是**任何被误判的窗口本会话内永久不可见**。
	 * Steam 账户栏的 hover 菜单就是这么变成整块全黑的 —— 它是自绘浮层，
	 * 建窗那一下表面还没提交缓冲（current.width == 0），一头撞进「≤1x1」。
	 *
	 * 改成两条：
	 *   A. **没提交过缓冲区就先不判**（照抄 considerMap）：等 commit 里重新
	 *      走一遍 view_map 的判定，那时尺寸是真的。
	 *   B. 判定成立时**只做「不进布局」**，节点照常显示。真·辅助窗口本来
	 *      就画不出东西，多一个空节点没有任何代价；被误判的窗口从此还能
	 *      上屏 —— 用「看不见」去换「不占位」不划算。
	 *
	 * 另外判据加一道类型闸：弹出物（override-redirect / window_type）一律
	 * 不碰，菜单和提示必须显示，这是它们存在的全部意义。 */
	struct wlr_surface *helper_surface = vidow_view_surface(view);
	bool has_buffer = helper_surface != NULL &&
		helper_surface->current.buffer_width > 0 &&
		helper_surface->current.buffer_height > 0;
	bool popup_typed = view->popup ||
		(view->x11 != NULL && x11_is_popup(view->x11));
	if (!popup_typed && !has_buffer) {
		/* A：还没交缓冲，尺寸不可信。等 commit 再判（view_surface_commit
		 * 里会在有缓冲时补一次 view_map 的判定）。 */
		wlr_log(WLR_DEBUG, "map: %s 还没提交缓冲区，先不定归属（等 commit）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)");
	} else if (!popup_typed && helper_surface != NULL &&
			helper_surface->current.width <= 1 &&
			helper_surface->current.height <= 1) {
		/* B：只「不进布局、不占列」，**不动场景节点**。
		 *
		 * 这才是辅助窗口真正需要的效果（它只要一个能 set_selection 的键盘
		 * serial）；把它从画面上拿掉属于过度处置 —— 误判一次就是永久黑屏。 */
		view->focus_helper = true;
		/* **借出前先记下焦点在哪**（抄 Hyprland 单点存焦点的做法）。
		 *
		 * 以后归还时只认这一格，不再读 server->focused —— 借出期间任何一次
		 * 重算焦点都会改掉 server->focused，读它就是「粘贴一次、焦点跳到
		 * 下一个窗口」那条老毛病。 */
		server->focus_borrowed_from = server->focused;
		vidow_seat_focus_surface(server->seat, helper_surface);
		vidow_im_focus(server, helper_surface);
		wlr_log(WLR_INFO, "map: %s 是 1x1 辅助窗口，只借键盘、不进布局"
			"（焦点借自 %s）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			server->focus_borrowed_from != NULL ?
				(vidow_view_title(server->focus_borrowed_from) != NULL ?
					vidow_view_title(server->focus_borrowed_from) :
					"(无题)") : "(无)");
		 x11_trace_view("map-helper-return", view, helper_surface);
		return;
	}


	/* 落进现在显示着的那块工作区：新窗口跟着走，不偷偷开在藏起来的工作区里。
	 * 列的窗口在 view_ensure_column 里落工作区；浮窗落在这里。 */
	if (!view->popup) {
		if (view->column != NULL && view->column->ws != NULL) {
			view->ws = view->column->ws;
		} else if (view->output != NULL) {
			/* 新窗口落**光标那块屏**现在显示着的工作区，不是「全局焦点屏」
			 * 的那块。view->output 在创建时就是按光标选的（view_default_output），
			 * 而 server->focused_output 可能还停在上一块屏上（点空区域 / 切
			 * 工作区之后没跟上）—— 那会让窗口开在 A 屏、操作却按 B 屏算。 */
			view->ws = vidow_workspace_shown_for(server, view->output);
		} else {
			view->ws = vidow_workspace_shown(server);
		}
	}
	/* 弹出物不进工作区，它跟父窗口走：父窗口在显示着的那块工作区里，它才显示。
	 * 普通窗口不靠「关节点」藏起来（关了按窗口录屏 / 预览就是黑的），节点一直
	 * 开着，藏不藏由 scene_parked 定 —— 这一帧的 arrange 里就会算准（见
	 * workspace.c 的 vidow_workspace_apply_visibility）。 */
	bool show = view->popup ?
		(view->parent == NULL ||
			vidow_workspace_view_visible(server, view->parent)) :
		vidow_workspace_view_visible(server, view);
	if (view->popup) {
		wlr_scene_node_set_enabled(&view->scene_tree->node, show);
	} else {
		vidow_view_set_parked(view, !show);
		wlr_scene_node_set_enabled(&view->scene_tree->node, true);
	}
	vidow_backdrop_damage_view(view);

	struct wlr_box geo = vidow_view_geometry(view);
	struct wlr_surface *surface = vidow_view_surface(view);
	wlr_log(WLR_INFO, "map %s: surface %dx%d geometry offset %d,%d",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(untitled)",
		surface != NULL ? surface->current.width : 0,
		surface != NULL ? surface->current.height : 0, geo.x, geo.y);

	/* Re-run detection here: clients that pin a fixed size (dialogs) only
	 * send their min/max hints after the first configure round-trip.
	 * 规则写了 float 就以规则为准（vidow_float_update_mode 认 view->rule_eff）。 */
	vidow_float_update_mode(view);

	if (view->float_mode == VIDOW_FLOAT_NONE) {
		view_ensure_column(server, view);
		/* 落进一条已经最大化的列（往最大化的列里又开/搬进来一个窗口）：告诉它
		 * 一声，别让它以为自己是普通尺寸、而布局把它按整列宽度摆。xdg 那边
		 * 「抢跑的最大化请求」在 initial_commit 里已经兑过了，这里被去重挡掉。 */
		if (view->column != NULL && view->column->maximized) {
			vidow_view_set_maximized(view, true);
		}
	} else if (!view->popup) {
		vidow_workspace_track(view->ws, view, true);
	}
	/* 规则的「开出来就这样」：工作区 / 屏 / 尺寸 / 居中 / 全屏 / 最大化 / 最小化。
	 * 放在这里是因为工作区和列都已经落定（最小化收起来的窗口下面就不抢焦点了）。 */
	vidow_rule_placement(server, view);
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
	/* fake_focus 的窗口如果这次没拿到真焦点（focus_on_open = false，或者
	 * 前面已经有焦点窗口），上面那次规则套用时可能还没 configure 好。上屏
	 * 之后再补一次 activated(true)，别让它一出生就是未激活。 */
	if (view->fake_focus && server->focused != view) {
		vidow_view_set_activated(view, true);
	}

	/* Everything above placed the window where it belongs; now play the
	 * opening effect on top of that final position. */
	vidow_view_open_anim(view);

	vidow_view_note_names(view);
	vidow_ipc_emit_window(server, "window-opened", view);
}


/* 见上面的声明：表面第一帧到了、而当时因为「还没有缓冲」没定归属的，
 * 在这儿补一次。重复调用是安全的（有工作区就不再动）。 */
static void view_reconsider_map(struct vidow_view *view) {
	if (view == NULL || view->x11 == NULL || view->server == NULL) {
		return;
	}
	struct wlr_surface *surface = vidow_view_surface(view);
	if (surface == NULL || surface->current.buffer_width <= 0 ||
			surface->current.buffer_height <= 0) {
		return; /* 还是没缓冲 */
	}
	if (view->popup || view->focus_helper) {
		return; /* 弹出物 / 已按辅助窗口处理过 */
	}
	if (view->ws != NULL) {
		return; /* 已经有归属了 */
	}
	if (surface->current.width <= 1 && surface->current.height <= 1) {
		/* 第一帧就是 1x1：现在尺寸可信了，按辅助窗口办（只拦布局，
		 * 不动场景节点 —— 见 view_map 里那段）。 */
		view->focus_helper = true;
		wlr_log(WLR_INFO, "commit: %s 第一帧确认是 1x1 辅助窗口，不进布局",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)");
		return;
	}
	if (view->output != NULL) {
		view->ws = vidow_workspace_shown_for(view->server, view->output);
	} else {
		view->ws = vidow_workspace_shown(view->server);
	}
	if (view->ws != NULL) {
		wlr_log(WLR_INFO, "commit: %s 第一帧到了（%dx%d），补进工作区 %s",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			surface->current.width, surface->current.height,
			view->ws->name != NULL ? view->ws->name : "-");
	}
	view_ensure_column(view->server, view);
	vidow_arrange(view->server);
}

static void view_unmap(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, unmap);
	struct vidow_server *server = view->server;
	struct wlr_output *output = view->output;
	x11_trace_view("unmap-enter", view, NULL);

	view->mapped = false;
	/* 忘了「上一次钉过几倍」：客户端 unmap 之后可能换一个新 surface 回来
	 * （Chromium 的 widget 重建就是这样），新 surface 需要我们重新发一次
	 * preferred_buffer_scale。和 Hyprland 的 unmap 监听
	 * （m_scale = std::nullopt）同一个意思。 */
	view->pref_scale = 0;
	/* XWayland 窗口 unmap 后需重置 x11_map_replayed：不然下次 map request
	 * 会认为「已经 replay 过」直接跳过 view_map，导致 scene node 不启用 → 黑屏。
	 * Steam 主界面第二次打开黑屏就是这个原因。 */
	view->x11_map_replayed = false;
	vidow_portal_view_mapped(view, false);
	if (view->focus_helper) {
		/* 辅助窗口走了：键盘还给原来的焦点窗口。它从来没进过 server->focused，
		 * 所以不能走下面的 focus_after_close（那会把焦点算到“下一个”）。 */
		view->focus_helper = false;
		/* **还给它借走时那个窗口**，不是 server->focused。
		 *
		 * 原来读 server->focused：借出期间任何一次重算焦点都会把这个指针
		 * 换掉，于是「粘一次，焦点跳到下一个窗口」。现在只认借出那一刻
		 * 记下的那一格（Hyprland 的 m_focusWindow 也是单点、也是同一用途）。
		 *
		 * 那个窗口可能已经关了：mapped 为假 / 表面没了就退回 server->focused，
		 * 再不行就清焦点 —— 绝不往已销毁的 view 上送。 */
		struct vidow_view *owner = server->focus_borrowed_from;
		server->focus_borrowed_from = NULL;
		if (owner != NULL && (!owner->mapped ||
				vidow_view_surface(owner) == NULL)) {
			owner = server->focused;
		} else if (owner == NULL) {
			owner = server->focused;
		}
		struct wlr_surface *back = owner != NULL ?
			vidow_view_surface(owner) : NULL;
		vidow_seat_focus_surface(server->seat, back);
		vidow_im_focus(server, back);
		wlr_log(WLR_INFO, "unmap: %s 归还键盘焦点 -> %s",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			owner != NULL ? (vidow_view_title(owner) != NULL ?
				vidow_view_title(owner) : "(无题)") : "(无)");
		/* **不关场景节点**（和 view_map 那边保持一致）。
		 *
		 * 关节点曾经是「藏起辅助窗口」的第二步，但它有副作用：节点一旦关掉，
		 * 这条 view 的 scene_tree 在本会话剩余时间里就一直停成 disabled ——
		 * 客户端再来一次 map（辅助窗口经常 unmap/map 反复）时，view_map()
		 * 会提前 return（focus_helper 分支），没人再把它打开。于是窗口活着、
		 * 却永远看不见。
		 *
		 * 节点开着没有代价：真·辅助窗口本来就不画东西。 */
		return;
	}
	if (!view->popup && view->float_mode != VIDOW_FLOAT_NONE) {
		/* 浮窗离开工作区。 */
		vidow_workspace_track(view->ws, view, false);
	}
	if (!view->popup && view->float_mode == VIDOW_FLOAT_NONE &&
			view->column != NULL) {
		/* 平铺窗口自己 unmap 了，得从列里摘出去 —— 不摘的话那条列一直占着
		 * 一列宽，屏上却什么都没有。proton 游戏就会这样：它的 "Input" 输入法
		 * 辅助窗口先被平铺进一条列，随后 unmap（连接不断、surface 还在）。
		 * 用户报的正是这个：「lutris 和游戏窗口之间凭空出现了一列，什么窗口
		 * 都没有但占据了一列的空间」—— 现场 dump 里 id 59 那个窗口就是
		 * mapped=false、visible=false，可 column 还在、几何 2883,6 951x1068，
		 * 正好夹在 lutris（1926）和游戏（3834）两条列中间。
		 * 它回来时 view_map 会走 view_ensure_column 重新归位。 */
		vidow_view_leave_layout(view);
	}
	if (view->dragging) {
		/* 拖着的时候窗口没了：撤掉 grab，别让指针还挂在一个已经没了的窗口上。 */
		view->dragging = false;
		if (server->grab_view == view) {
			server->grab_mode = VIDOW_GRAB_NONE;
			server->grab_view = NULL;
			server->grab_client_press = false;
			wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
				"left_ptr");
		}
	}
	view_snapshot_close(view);
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	/* 图标态那块快照同样要收：窗口 unmap 了就不该再有它的画面（它是
	 * layer_toplevels 的孩子，scene_tree 关掉管不着它）。重新 map 时客户端一
	 * 交帧，mini_snapshot_place() 会照现状重建。 */
	mini_snapshot_free(view);
	vidow_backdrop_damage_view(view);

	if (server->mini_grab_view == view) {
		/* 小窗形态拖拽中窗口没了（客户端崩 / 被关）：撤掉，别让指针还挂在一个
		 * 已经没了的窗口上（见 input.c 的 mini_pointer_*）。 */
		server->mini_grab_view = NULL;
		server->mini_grab_dragging = false;
	}

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
		vidow_view_leave_column(view);
		server->focused = NULL;
		if (next != NULL) {
			vidow_focus_view(next);
		} else {
			focus_nearest_column(server, output);
		}
	} else {
		vidow_view_leave_column(view);
	}

	/* 列真的摘掉了，它那块工作区才可能变空 —— 栈中间的空工作区该收掉（niri：
	 * 切走 / 关掉之后那块就没了）。放在 leave_column 之后：之前它还不算空。 */
	vidow_workspace_cleanup(server);
	vidow_arrange(server);
}

/* 客户端这一帧到底动了哪一块（布局逻辑坐标，语义见 server.h）。
 *
 * surface->buffer_damage 是**缓冲像素**坐标：wlroots 在 surface_update_damage()
 * 里已经把客户端的 surface damage + buffer damage、viewport 的 src 偏移、scale、
 * transform 全算进去了（换缓冲 / 换尺寸就是整块）。要变成布局逻辑坐标只差一个
 * 「缓冲像素 / 表面逻辑像素」的倍率和场景节点的位置。
 *
 * 只有这几件事都成立时才敢用小盒子（否则返回 WHOLE，让调用方按整块算）：
 * 没有 transform / viewport 缩放、没有 subsurface（它们的 commit 我们没在听，
 * 拿不到损伤区域）、场景节点没有被改成别的显示尺寸（overview / X11 缩放）、
 * overview 没在跑（那时候节点位置是映射过的，跟布局坐标对不上）。 */
enum vidow_surface_damage vidow_view_surface_damage(struct vidow_view *view,
		struct wlr_box *box) {
	struct wlr_surface *s = vidow_view_surface(view);
	if (s == NULL || view->scene_surface == NULL ||
			view->scene_surface->buffer == NULL) {
		return VIDOW_SURFACE_DAMAGE_WHOLE;
	}
	const struct wlr_surface_state *st = &s->current;
	if (!pixman_region32_not_empty(&s->buffer_damage)) {
		/* 没报损伤。没换缓冲 = 真的一个像素都没动（只有帧回调 /
		 * ack_configure 的那种 commit），世界代次都不用动；换了新缓冲却
		 * 没报损伤 = 内容到底变没变说不清，按整块算。 */
		return (st->committed & WLR_SURFACE_STATE_BUFFER) != 0 ?
			VIDOW_SURFACE_DAMAGE_WHOLE : VIDOW_SURFACE_DAMAGE_NONE;
	}
	if (st->transform != WL_OUTPUT_TRANSFORM_NORMAL ||
			st->viewport.has_dst || st->viewport.has_src ||
			st->width <= 0 || st->height <= 0 ||
			st->buffer_width <= 0 || st->buffer_height <= 0) {
		return VIDOW_SURFACE_DAMAGE_WHOLE;
	}
	/* 有子表面就整块算损伤，是老行为，代价极大：Steam / 所有 CEF 窗口的
	 * 主表面下都挂着子表面，于是**每一次 commit** 都被判成整块损伤 ——
	 * 整块重烘离屏、整块重抓模糊，烘完又产生新损伤，自己喂自己。实测量到
	 * 每秒 5 万次 bake（正常是 60～165 次），渲染管线直接饱和，窗口停在
	 * 第一帧的黑图上（用户报的「重开黑屏」）。
	 *
	 * 子表面自己的损伤是记在**它自己**的 state 里，不在父表面的
	 * buffer_damage 里。所以这里只看父表面这一份：它有明确损伤就用它
	 * （子表面的重画由子表面自己的 commit 各自触发），一份都没有才退回
	 * 整块。宁可偶尔多烘一帧，也不能每帧都烘。 */
	if (!wl_list_empty(&st->subsurfaces_below) ||
			!wl_list_empty(&st->subsurfaces_above)) {
		/* 上面已经确认父表面这一帧**有**损伤。只在它盖住整块表面时
		 * 才退回整块 —— 不然就用下面的精细框。 */
		pixman_box32_t *sd = pixman_region32_extents(&s->buffer_damage);
		double ssc = (double)st->buffer_width / (double)st->width;
		if ((double)sd->x1 / ssc <= 0.0 && (double)sd->y1 / ssc <= 0.0 &&
				(double)sd->x2 / ssc >= (double)st->width &&
				(double)sd->y2 / ssc >= (double)st->height) {
			return VIDOW_SURFACE_DAMAGE_WHOLE;
		}
	}
	struct wlr_scene_buffer *sb = view->scene_surface->buffer;
	if (sb->dst_width > 0 &&
			(sb->dst_width != st->width || sb->dst_height != st->height)) {
		return VIDOW_SURFACE_DAMAGE_WHOLE;
	}
	if (vidow_overview_active(view->server)) {
		return VIDOW_SURFACE_DAMAGE_WHOLE;
	}
	int nx = 0, ny = 0;
	if (!wlr_scene_node_coords(&sb->node, &nx, &ny)) {
		return VIDOW_SURFACE_DAMAGE_WHOLE;
	}
	pixman_box32_t *d = pixman_region32_extents(&s->buffer_damage);
	double cs = (double)st->buffer_width / (double)st->width;
	int x1 = (int)floor((double)d->x1 / cs);
	int x2 = (int)ceil((double)d->x2 / cs);
	int y1 = (int)floor((double)d->y1 / cs);
	int y2 = (int)ceil((double)d->y2 / cs);
	/* 只算表面里**看得见**的那一块：客户端可以声明窗口几何比表面小（GTK 的
	 * CSD 留白 —— 装饰着色器只采窗口几何那一块，见 vidow_view_fx_geo），
	 * 留白那圈 vidow 根本不画。损伤全落在留白里 = 屏幕上什么都没变。 */
	struct wlr_box g = vidow_view_geometry(view);
	if (g.width <= 0 || g.height <= 0 || g.x < 0 || g.y < 0 ||
			g.x + g.width > st->width || g.y + g.height > st->height) {
		/* 没声明几何 / 声明框落在表面外面（客户端改大小的过渡期）：当整块
		 * 表面都是可见内容，跟 vidow_view_fx_geo 同一套判断。 */
		g = (struct wlr_box){ .x = 0, .y = 0,
			.width = st->width, .height = st->height };
	}
	if (x1 < g.x) {
		x1 = g.x;
	}
	if (y1 < g.y) {
		y1 = g.y;
	}
	if (x2 > g.x + g.width) {
		x2 = g.x + g.width;
	}
	if (y2 > g.y + g.height) {
		y2 = g.y + g.height;
	}
	if (x2 <= x1 || y2 <= y1) {
		return VIDOW_SURFACE_DAMAGE_NONE; /* 只在看不见的留白里动 */
	}
	*box = (struct wlr_box){
		.x = nx + x1, .y = ny + y1,
		.width = x2 - x1, .height = y2 - y1,
	};
	return VIDOW_SURFACE_DAMAGE_BOX;
}

static void view_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, commit);
	struct wlr_surface *surface = vidow_view_surface(view);
	if (!view->mapped || view->focus_helper || view->scene_tree == NULL ||
			(view->x11 != NULL && view->x11->surface != NULL &&
				!view->x11->surface->mapped)) {
		x11_trace_view("commit-anomaly", view, surface);
	}

	/* 表面第一帧到了：map 那一刻可能还没缓冲（尺寸全是 0，判不了归属），
	 * 在这儿补一次。抄 Hyprland considerMap 的思路 —— 见 view_reconsider_map。 */
	view_reconsider_map(view);

	/* 补漏的 map：X 表面已经 mapped、我们这边还没认账。
	 *
	 * 踩过的：Steam 用 `steam steam://open/main` 主动打开主界面时，X 层窗口
	 * 已经是 IsViewable（xwininfo 能看到），属性流（title / 缓冲尺寸）也都到
	 * 了，但 map 信号没走完我们这条监听 —— 场景节点停在 disabled，画面上看不
	 * 到窗口，`vidowmsg windows` 里也没有它。和 x11_associate 里那次补偿
	 * （wlr_xwayland_surface 建 view 时 surface 早就 mapped）是同一个现象，
	 * 只是晚了一步发生在这儿。
	 *
	 * 判据只看 wlroots 记住的 mapped：它和 commit 是同一条事件流里的状态，
	 * 不会比客户端实际状态超前，所以不会把真正想藏起来的窗口拉出来。 */
	x11_replay_map_if_xmapped(view, false);

	/* 补漏的 **unmap → remap**：窗口已经认过账（view->mapped 为真），但场景
	 * 节点还停在 disabled / parked —— 上面那个 x11_replay_map_if_xmapped 帮不
	 * 上忙，它的第一条判据就是「view->mapped 为假才往下走」。
	 *
	 * 为什么会有这种状态：view_map() 一进门就把 view->mapped 置真，然后才去算
	 * show / set_parked / set_enabled。Steam 每次重开主窗口都**复用同一个 X 窗
	 * 口**（xid 0x2000041 一直不变），view 和 scene_tree 都是原来那个，于是
	 * 它第二次 map 时 view_map() 在「view->mapped 已经为真」那行直接 return，
	 * 节点保持上一次 unmap 时的关闭状态 —— 客户端照交帧（x11-buf 能看到），
	 * 我们照收，就是没人把它打开。表现就是主窗口全黑、`vidowmsg windows` 里
	 * 一切正常，手动拖一下就好（拖动走 grab → arrange，顺带把它摆出来）。
	 *
	 * 判据只认「节点真关着 + 客户端真有缓冲」：这两条同时成立时，窗口不可能
	 * 是我们故意藏起来的（故意藏走的是 scene_parked，节点仍然 enabled）。 */
	x11_remap_if_node_closed(view);

	/* **排帧**。这是 X11 和 Wayland 共用的交帧入口，而 vidow 是按需渲染
	 * （vidow_output_schedule_frame）—— 不在这里要一帧，客户端交上来的新内容
	 * 就要等下一次别的什么原因触发渲染才上屏。
	 *
	 * 用户报的「Electron / Chromium 应用还是卡」就是这个：Wayland 客户端那条路
	 * 由 view_surface_commit 补上了，但 X11（XWayland 上的 vivaldi / discord /
	 * telegram / Element）走的是这个 view_commit，一直没人排帧。
	 * 放在最前面：后面的记账不影响「有新内容」这个事实。 */
	if (view->server != NULL && view->output != NULL &&
			!view->server->locked) {
		vidow_output_schedule_frame(view->server, view->output,
			"客户端交帧");
	}
	/* X11 弹出物：内容换了（圆角 / 自带阴影的 alpha 可能跟着变），背后那张
	 * 模糊按新 mask 重烘一遍。 */
	if (view->popup && view->popup_blur != NULL) {
		vidow_popup_blur_invalidate(view->popup_blur);
	}

	/* **交帧间隔**（VIDOW_TRACE_FRAMES=1）：vidow 自己 128fps 也不慢的时候，
	 * 卡顿只能在客户端侧 —— 这个数会直接显示「它多久交一帧」。144Hz 应该
	 * ~7ms，60Hz ~16ms；上百毫秒就是客户端被饿着或者自己卡住。 */
	if (view->server->trace_timer != NULL) {
		int64_t now_ns = vidow_now_ns();
		if (view->last_commit_ns != 0 && view->mapped) {
			/* **累计**每 2 秒打一次：交帧次数 / 平均间隔 / 最坏间隔。
			 * 只记 >30ms 的那些算不出帧率（快的被滤掉了），分不清「空闲」和
			 * 「真的卡」。累计器不设阈值，所以能直接算出这个窗口的真实 fps。 */
			int64_t d = now_ns - view->last_commit_ns;
			view->commit_n++;
			view->commit_ns_sum += d;
			if (d > view->commit_ns_max) {
				view->commit_ns_max = d;
			}
			if (view->commit_report_ns == 0) {
				view->commit_report_ns = now_ns;
			} else if (now_ns - view->commit_report_ns >= 2000 * 1000000LL) {
				double span = (double)(now_ns - view->commit_report_ns) / 1e9;
				wlr_log(WLR_INFO,
					"交帧统计 %s: %.1f fps（%lld 帧 / %.1fs，均 %.0fms，最坏 %.0fms）",
					vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
					(double)view->commit_n / span, (long long)view->commit_n,
					span, (double)view->commit_ns_sum / 1e6 / view->commit_n,
					(double)view->commit_ns_max / 1e6);
				view->commit_n = 0;
				view->commit_ns_sum = 0;
				view->commit_ns_max = 0;
				view->commit_report_ns = now_ns;
			}
		}
		view->last_commit_ns = now_ns;
	}

	/* 客户端交帧了 —— X11 那边顺手对一下 _NET_WM_STATE（属性那条路不发信号，
	 * 见 view_sync_x11_fullscreen）。 */
	view_sync_x11_fullscreen(view);

	/* 客户端交帧了：它声明的有效源区域（wp_viewport 的 src 框）可能跟着变了，
	 * 抄一份没裁过的那一版给装饰用（裁着的时候这个字段是被 set_clip 改过的，
	 * 见 view_snapshot_content_box）。没裁的时候只是读一下，很便宜。 */
	view_snapshot_content_box(view);

	/* 帧追踪（VIDOW_TRACE_FRAMES=1）：客户端在这里交帧。只记「真画在这块屏上、
	 * 还显示着」的那些 —— 视口外的窗口本来就没人给它出帧，记了就是误报。
	 * 这块屏要是从此不再出帧，就是「客户端在画、屏幕不更新」。 */
	/* 帧追踪：客户端到底有没有在交帧（限速 150ms 一行）。「窗口停在最后一帧」
	 * 这种事，第一刀就切在这里：客户端不交帧（在等帧回调/自己卡住）和交了帧而
	 * 画面没更新，是两个完全不同的问题。 */
	if (view->server->trace_timer != NULL) {
		int64_t now_ns = vidow_now_ns();
		if (now_ns - view->trace_commit_ns > 150 * 1000000LL) {
			view->trace_commit_ns = now_ns;
			struct wlr_surface *s0 = vidow_view_surface(view);
			wlr_log(WLR_DEBUG, "trace: commit %s（缓冲 %dx%d）",
				vidow_view_title(view) != NULL ?
					vidow_view_title(view) : "(无题)",
				s0 != NULL ? s0->current.buffer_width : 0,
				s0 != NULL ? s0->current.buffer_height : 0);
		}
	}
	struct vidow_output *trace_out =
		vidow_output_lookup(view->server, view->output);
	if (trace_out != NULL && view_shows(view) &&
			view_on_screen(view->server, trace_out, view)) {
		vidow_output_trace_commit(trace_out);
	}

	/* New pixels: everything that samples this window as a backdrop (a
	 * floating window on top of it) has to be re-blurred. 它自己那张私有底衬
	 * 不受影响（抓的时候它是藏着的），所以这笔账记在它头上。
	 *
	 * 记的是**客户端真动过的那一块**（surface damage），不是整块窗口：一个
	 * 1000x800 的窗口里只有一条 300x20 的进度条在动的时候，压着它的那个窗口
	 * 底下那片模糊只该因为这一条过期。说不清的时候照样整块算（见
	 * vidow_view_surface_damage）。 */
	struct wlr_box dmg;
	switch (vidow_view_surface_damage(view, &dmg)) {
	case VIDOW_SURFACE_DAMAGE_NONE:
		break; /* 一个像素都没变：世界代次都不动 */
	case VIDOW_SURFACE_DAMAGE_BOX:
		vidow_backdrop_damage_view_rect(view, &dmg);
		break;
	case VIDOW_SURFACE_DAMAGE_WHOLE:
		vidow_backdrop_damage_view(view);
		break;
	}

	if (view->kind == VIDOW_VIEW_X11) {
		/* X11：缓冲区的缩放比例要跟着 xwayland_scale 走（尺寸变了就重算），
		 * 其余和 xdg 一样 —— 浮动窗口自己定尺寸，贴图重画，最后锁一份给
		 * 关闭动画。 */
		vidow_view_x11_scale(view);
		view_dst_capture(view);
		if (view->float_mode != VIDOW_FLOAT_NONE) {
			vidow_float_on_commit(view);
		}
		if (view->mapped && (view->fx.active ||
				view->mini != VIDOW_MINI_NONE)) {
			vidow_view_apply_anim(view);
		}
		struct wlr_client_buffer *client =
			vidow_view_surface(view) != NULL ?
			vidow_view_surface(view)->buffer : NULL;
		/* 排错「重开黑屏」：X11 每次换新 buffer 记一行。第二次打开 Steam
		 * 时如果这行根本不出现，就是客户端没交帧（在等 configure 或自己的
		 * 状态机卡住）；出现了但画面还是空的，才是我们这边没画。
		 * 只在 buffer 真变了时打，不然 60Hz 的窗口会把日志刷爆。 */
		if (client != NULL && client->source != view->last_buffer) {
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
			if (now - view->buf_trace_at >= 2.0) {
				view->buf_trace_at = now;
				wlr_log(WLR_INFO, "x11-buf %s: src=%p last=%p %dx%d "
					"mapped=%d col=%p tree=%p",
					vidow_view_title(view) != NULL ?
						vidow_view_title(view) : "(无题)",
					(void *)client->source, (void *)view->last_buffer,
					vidow_view_surface(view) != NULL ?
						vidow_view_surface(view)->current.width : -1,
					vidow_view_surface(view) != NULL ?
						vidow_view_surface(view)->current.height : -1,
					view->mapped, (void *)view->column,
					(void *)view->scene_tree);
			}
		}
		if (client != NULL && client->source != NULL &&
				client->source != view->last_buffer) {
			wlr_buffer_unlock(view->last_buffer);
			view->last_buffer = wlr_buffer_lock(client->source);
			/* 小窗（S5）：换了最后一帧 → 快照节点（图标 + 预览）跟上。 */
			vidow_mini_frames_sync(view);
		}
		return;
	}

	/* xdg-shell: the initial commit must be answered with a configure. */
	if (view->xdg_toplevel->base->initial_commit) {
		/* 抢跑的状态请求：客户端在第一次 commit *之前* 就 set_fullscreen 的话
		 * （Qt/KDE 恢复全屏窗口会这么开窗），view_request_fullscreen 里发不了
		 * configure，只把请求留在 wlroots 那边。现在 surface initialized 了，
		 * 在 arrange 之前兑现它 —— 晚了 arrange 就按「不是全屏」排过一遍了。 */
		if (view->xdg_toplevel->requested.fullscreen && !view->fullscreen) {
			view_apply_fullscreen(view, true, "客户端开窗时就要全屏");
		}
		vidow_float_update_mode(view);
		if (view->float_mode == VIDOW_FLOAT_NONE) {
			view_ensure_column(view->server, view);
			/* 抢跑的最大化请求同理（Qt/KDE 恢复「上次是最大化」的窗口就是这么
			 * 开窗的）：view_request_maximize 里列还没建出来（列是上一行才建的），
			 * 只能在这儿兑现。放在 arrange 之前 —— 晚了就按「没最大化」排过一遍，
			 * 客户端还会先收到一个 maximized=false 的初始 configure。 */
			if (view->server->config.honor_maximize_request &&
					view->xdg_toplevel->requested.maximized &&
					view->column != NULL) {
				view->maximize_granted = true;
				vidow_column_set_maximized(view->column, true);
			}
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
			view->cfg_w = 0;
			view->cfg_h = 0;
			view->cfg_sent = true;
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
	surface = vidow_view_surface(view);
	if (surface != NULL &&
			view->logged_client_buf_w != surface->current.buffer_width) {
		view->logged_client_buf_w = surface->current.buffer_width;
		/* state 里不留 buffer_scale，用缓冲区像素 / 逻辑尺寸反推。 */
		double client_scale = surface->current.width > 0 ?
			(double)surface->current.buffer_width /
				(double)surface->current.width : 1.0;
		view->logged_client_scale = (int)lround(client_scale);
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
	/* 小窗（S3）也走这一趟，哪怕 fx 不 active：wlroots 在每次 commit 时会把场景
	 * 节点的显示尺寸重置成表面自己的尺寸（types/scene/surface.c 的
	 * surface_reconfigure），而我们的 commit 监听器排在它后面 —— 不在这一趟里
	 * 重设一次小窗那条缩放，客户端每交一帧节点就弹回原生尺寸（画面忽大忽小、
	 * 点击范围也跟着错）。装饰开着时本来就会走到这儿，这条只多覆盖「装饰全关 +
	 * 小窗」那一种。 */
	if (view->mapped && (view->fx.active || view->mini != VIDOW_MINI_NONE)) {
		vidow_view_apply_anim(view);
	}

	/* **fractional-scale 客户端的尺寸**（Chromium / Electron 全走这条路）。
	 *
	 * 它们故意 set_buffer_scale(1)，但 buffer 按 2x 画（实测 vivaldi 在 DP-2
	 * 上：buffer 3808x2050、surface 1904x1025、scale 1）—— 协议的本意是让合成器
	 * 用 wp_viewporter 缩回去。而 wlroots 的 surface_reconfigure 拿
	 * buffer_width / buffer_scale 当节点尺寸，于是 3808 直接铺在 1904 的位置上：
	 * 画面变成 1/4，配合后面的重排又弹回 1 倍 —— 用户看到的「1/4 和 1 不停变化」，
	 * 也是「Electron 卡」的直接原因（每帧都在重排重画）。
	 *
	 * 修：每次 commit 后把客户端节点的显示尺寸钉回逻辑尺寸（surface->width /
	 * height 已经是逻辑值）。fx / 小窗那条路自己会设，不打架。 */
	if (view->scene_surface != NULL && surface != NULL &&
			surface->current.width > 0 && surface->current.height > 0) {
		wlr_scene_buffer_set_dest_size(view->scene_surface->buffer,
			surface->current.width, surface->current.height);
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
		/* 小窗（S5）：换了最后一帧 → 快照节点（图标 + 预览）跟上。 */
		vidow_mini_frames_sync(view);
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

	/* 悬停记录会变成悬空指针。 */
	if (server->hovered_view == view) {
		server->hovered_view = NULL;
	}
	if (server->focus_before_layer == view) {
		server->focus_before_layer = NULL;
	}

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
	wl_list_remove(&view->request_minimize.link);
	wl_list_remove(&view->surface_commit.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->destroy.link);
	view_capture_holds_clear(view);
	vidow_portal_view_free(view);
	vidow_mru_forget(server, view);
	/* 动画回调的 data 就是这个 view（view_move_update / view_move_done）：
	 * 窗口在动画没跑完时没了，必须先把动画取消掉，否则下一次 anim tick
	 * 会拿着已释放的 view 调 vidow_view_apply_anim() —— 那就是 2026-10-12
	 * 23:20–23:47 那 5 个 SIGSEGV 核心转储（栈：#0 野地址 <- #1
	 * vidow_output_render <- output_frame <- 帧回调）。 */
	if (view->anim_move != NULL) {
		vidow_anim_cancel(view->anim_move);
		view->anim_move = NULL;
	}
	if (view->anim_open != NULL) {
		vidow_anim_cancel(view->anim_open);
		view->anim_open = NULL;
	}
	wl_list_remove(&view->link);
	/* 果冻那个场景节点比 view 活得久，结构里又攥着 view 指针：先收掉。 */
	vidow_jelly_view_gone(view);

	/* The scene node is owned by wlroots and may outlive the view; clear the
	 * tag so hit-testing cannot reach the freed struct. */
	if (view->scene_tree != NULL) {
		view->scene_tree->node.data = NULL;
	}
	if (server->mini_grab_view == view) {
		/* 小窗形态拖拽中窗口被销毁了（客户端崩 / 直接被关，没走 unmap）：
		 * 和 view_unmap 里那处一样，撤掉指针上的挂账。 */
		server->mini_grab_view = NULL;
		server->mini_grab_dragging = false;
	}
	if (server->grab_view == view) {
		vidow_float_end_grab(server);
	}
	vidow_float_detach_children(server, view);
	/* 图标态那块快照挂在 layer_toplevels 上，不跟着 scene_tree 走：窗口没了
	 * 就得自己收（不收 = 屏上冻着一块「窗口已经没了」的鬼影）。 */
	mini_snapshot_free(view);
	wlr_buffer_unlock(view->last_buffer);
	view->last_buffer = NULL;
	vidow_fx_release(&view->fx);
	vidow_anim_cancel(view->anim_move);
	view->anim_move = NULL;
	vidow_anim_cancel(view->anim_open);
	view->anim_open = NULL;
	/* 小窗形态转换的补间（S5）data 也是这个 view。 */
	vidow_anim_cancel(view->mini_anim);
	view->mini_anim = NULL;
	/* 悬停预览（S5）挂在 server 上、指着的却是这个 view：先收掉，别留一个
	 * 攥着已释放窗口的镜面节点。 */
	if (server->mini_preview_view == view) {
		vidow_mini_preview_hide(server);
	}

	/* **先撤掉别人对我的挂账**：辅助窗口的「借出方」指向的本 view 可能马上
	 * 就被 free() 掉，留着就是一个悬空指针（unmap 时读它就崩）。 */
	if (server->focus_borrowed_from == view) {
		server->focus_borrowed_from = NULL;
	}
	if (view->focus_helper) {
		/* 辅助窗口销毁：键盘还给**它借走时那个窗口**（它没进过焦点链，
		 * 读 server->focused 会跳到「下一个」窗口 —— 见 server.h 里
		 * focus_borrowed_from 的说明）。借出方已经没了就退回 server->focused。 */
		view->focus_helper = false;
		struct vidow_view *owner = server->focus_borrowed_from;
		server->focus_borrowed_from = NULL;
		if (owner != NULL && (!owner->mapped ||
				vidow_view_surface(owner) == NULL)) {
			owner = server->focused;
		} else if (owner == NULL) {
			owner = server->focused;
		}
		if (owner == view) {
			owner = NULL; /* 自己：别往正在销毁的窗口送焦点 */
		}
		struct wlr_surface *back = owner != NULL ?
			vidow_view_surface(owner) : NULL;
		vidow_seat_focus_surface(server->seat, back);
		vidow_im_focus(server, back);
	}
	bool was_focused = server->focused == view;
	/* 邻居要在摘列（和 free(view)）之前找（见 focus_after_close）。 */
	struct vidow_view *next = was_focused ?
		focus_after_close(server, view, output) : NULL;
	if (was_focused) {
		server->focused = NULL;
	}
	vidow_view_leave_column(view);
	/* 浮窗直接 destroy（没走 unmap）时也要把 ws->floats 的账摘掉。 */
	if (!view->popup && view->float_mode != VIDOW_FLOAT_NONE) {
		vidow_workspace_track(view->ws, view, false);
	}
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
	/* DEBUG：浏览器每换一个标签页、终端每 cd 一次都会来一行，日常日志里
	 * 不需要。规则重判 / IPC 广播照旧（它们只在该动的窗口上动）。 */
	wlr_log(WLR_DEBUG, "title %s", vidow_view_title(view) != NULL ?
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

	if (!vidow_view_configure_ready(view)) {
		/* 第一次 commit 之前来的全屏请求：这时候既发不出 configure，也不能改
		 * 状态 —— 改了就是「布局当它全屏、客户端以为自己不是全屏」。等它的初始
		 * commit（surface initialized 之后）再兑现，见 view_commit。 */
		return;
	}

	view_apply_fullscreen(view, want, "客户端请求（xdg_toplevel.set_fullscreen）");
}

static void view_request_maximize(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, request_maximize);

	/* 客户端可能在**第一次 commit 之前**就 set_maximized（Qt/KDE 恢复「上次是
	 * 最大化」的窗口就是这么开窗的 —— kate 一开就把合成器打死）。wlroots 收到
	 * 请求立刻发这个信号，而那时候 surface 还没 initialized：
	 * wlr_xdg_toplevel_set_maximized() 内部的 wlr_xdg_surface_schedule_configure()
	 * 第一句就是 assert(surface->initialized)，直接 abort
	 * （wlroots 0.20.2：types/xdg_shell/wlr_xdg_surface.c:168）。
	 * 所以还没 initialized 就先不碰它：等它的初始 commit（surface initialized
	 * 之后），在 view_commit 的 initial_commit 分支里兑现这个请求。 */
	if (!vidow_view_configure_ready(view)) {
		return;
	}

	/* 客户端自己发起的请求：协议上得有应答，所以把「上次发出去的是什么」这笔账
	 * 作废一次 —— 即使状态碰巧没变也要真发一个 configure 回去（去重那条路是给
	 * 布局自己变化用的，见 server.h 的 cfg_maximized_sent）。 */
	view->cfg_maximized_sent = false;

	bool want = view->xdg_toplevel->requested.maximized;
	struct vidow_column *col = view->column;

	/* 浮窗（对话框、菜单那种没有列的窗口）：照旧回一个「不最大化」——
	 * xdg-shell 要求每个状态请求都有应答，不回客户端就卡在等待里。
	 * 浮窗的尺寸是它自己定的，vidow 不替它撑满工作区。 */
	if (col == NULL) {
		view->maximize_granted = false;
		vidow_view_set_maximized(view, false);
		return;
	}

	if (want && !view->server->config.honor_maximize_request) {
		/* 配置里说不响应（默认）：协议要求每个状态请求都有应答，所以回一个
		 * 「没最大化」，但不动列宽 —— Qt/KDE/Electron 恢复「上次是最大化」
		 * 不会再让窗口一开就占满整列。 */
		view->maximize_granted = false;
		vidow_view_set_maximized(view, false);
		return;
	}
	if (want) {
		/* vidow 没有「窗口级」的最大化（列是布局的最小单位）：客户端要最大化
		 * 就把它那条列最大化，并且记下「这次是它要的」。 */
		view->maximize_granted = true;
		vidow_column_set_maximized(col, true);
		/* 列本来就是最大化的时候上面那条是空操作，应答还得自己补。 */
		vidow_view_set_maximized(view, true);
		return;
	}

	if (view->maximize_granted) {
		/* 它自己撤销：这次最大化本来就是它要的，理。 */
		view->maximize_granted = false;
		vidow_column_set_maximized(col, false);
		vidow_view_set_maximized(view, false);
		return;
	}

	/* 列是**用户**最大化的（Mod+Shift+F），客户端这时候随手发一个
	 * set_maximized(false) 不该把用户的布局撤掉 —— 照当前实际状态回话：
	 * 列最大化的照样答「最大化」，客户端和布局不会各说各话。 */
	vidow_view_set_maximized(view, col->maximized);
}

/* xdg_toplevel.set_minimized：客户端自己要求最小化（xdg-shell v6 起有，wlroots 只把
 * 请求转成信号，认不认由合成器定）。vidow 认 —— 和 IPC / 键位走同一条路；应答的那次
 * configure 由 vidow_view_set_minimized() 里发（协议要求状态请求必须有应答）。 */
static void view_request_minimize(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, request_minimize);

	vidow_view_set_minimized(view, true);
}

/* 客户端交帧了 → 排一帧。
 *
 * vidow 是**按需渲染**：客户端自己 damage 场景之后没人踢这一脚，帧回调就只能
 * 等别的事件（光标移动 / 动画 / 补帧闹钟）碰巧到来时才发 —— 表现就是「应用
 * 内部像 60 帧，桌面本身却是 160」。这一条让「客户端想画」直接变成「排一帧」。 */
static void view_surface_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, surface_commit);
	if (view->server == NULL || view->output == NULL || view->server->locked) {
		return;
	}
	vidow_output_schedule_frame(view->server, view->output, "客户端交帧");
}

static void view_request_move(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, request_move);
	struct vidow_server *server = view->server;

	/* A CSD titlebar drag: detach a tiled window into a float and grab it.
	 * 这条路上按下是转给客户端的（它才发的 xdg_toplevel.move），from_client
	 * 让松手也转回去（见 server.h 里 grab_client_press 的说明）。 */
	if (view->float_mode == VIDOW_FLOAT_NONE) {
		vidow_float_set_mode(view, VIDOW_FLOAT_FIXED);
	}
	/* 拖标题栏 = 平移视图（用户点名的那条）；想移动浮窗用 Mod+左键。 */
	vidow_pan_begin(server, view);
}

static void view_request_resize(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, request_resize);
	struct vidow_server *server = view->server;

	if (view->float_mode == VIDOW_FLOAT_NONE) {
		vidow_float_set_mode(view, VIDOW_FLOAT_FIXED);
	}
	vidow_float_begin_grab(server, view, true, true);
}

/* ------------------------------------------------------- X11（XWayland） */


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

/* X11 客户端声明的宽高比（WM_NORMAL_HINTS 的 PAspect）。num/den 就是 w/h。
 *
 * min_aspect 与 max_aspect 都设了、且相等，才算「一个确定的比值」—— 只设一边
 * 是「不小于 / 不大于」的区间，那不是锁定（ICCCM 的语义）。**vidow 以前完全没
 * 读这个字段**，S3b 才加上（见计划 §11.3）。
 *
 * Wayland 协议里没有对应物（xdg-shell 只有 min/max 尺寸，没有比例提示），所以
 * 非 X11 恒返回 false —— 那边的 auto 等同 free。 */
bool vidow_view_aspect_hint(const struct vidow_view *view, int *num, int *den) {
	if (view == NULL || view->kind != VIDOW_VIEW_X11 || view->x11 == NULL) {
		return false;
	}
	const xcb_size_hints_t *hints = view->x11->size_hints;
	if (hints == NULL ||
			(hints->flags & XCB_ICCCM_SIZE_HINT_P_ASPECT) == 0) {
		return false;
	}
	if (hints->min_aspect_num <= 0 || hints->min_aspect_den <= 0 ||
			hints->max_aspect_num <= 0 || hints->max_aspect_den <= 0) {
		return false;
	}
	/* 交叉相乘比：整数除法会把 16/9 和 16/10 判成同一个。 */
	if ((long)hints->min_aspect_num * hints->max_aspect_den !=
			(long)hints->max_aspect_num * hints->min_aspect_den) {
		return false;
	}
	*num = hints->min_aspect_num;
	*den = hints->min_aspect_den;
	return true;
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
		double nw = hints->min_width, nh = hints->min_height;
		vidow_xwm_from_native_size(view->server, view->output, nw, nh, &nw, &nh);
		*w = (int)lround(nw);
		*h = (int)lround(nh);
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
	struct wlr_output *hit = vidow_xwm_output_at_native(server,
		view->x11->x, view->x11->y);
	if (hit != NULL) {
		view->output = hit;
	} else if (view->output == NULL) {
		view->output = view_default_output(server);
	}
}

/* Client-placed X popups use the same virtual-output inverse transform
 * as normal windows. Do not clamp them to the usable area. */
bool vidow_view_x11_popup_box(struct vidow_view *view, struct wlr_box *box) {
	if (!view->popup || view->x11 == NULL) {
		return false;
	}
	if (view->x11->width <= 0 || view->x11->height <= 0) {
		return false; /* 还没定尺寸，等它自己说 */
	}
	/* Xwayland 给的 x11->x/y 是**全局 X 绝对坐标**（那个 X 屏幕横跨所有屏，
	 * 每块屏各占一段设备像素宽）。折回**场景布局坐标**要三步：
	 *   ① 减掉这块屏在 X 坐标系里的起点 dev_x；
	 *   ② 除以**这块屏自己的** scale（不是全局 xwayland_scale）；
	 *   ③ 加上这块屏在布局里的原点（右屏布局从 1920 起，不加就全挤到左屏）。
	 *
	 * 之前是「全局 X 坐标直接除以 xwayland_scale」，缺 ①③、②还用错：
	 * 单屏 / 全同倍率下碰巧对，混合 DPI 就错 —— 1x 屏上的弹窗被多除了 2
	 * 且没加布局原点，2x 屏按比例正好抵消。 */
	x11_track_output(view);
	double x = view->x11->x, y = view->x11->y;
	double w = view->x11->width, h = view->x11->height;
	vidow_xwm_from_native_point(view->server, view->output, x, y, &x, &y);
	vidow_xwm_from_native_size(view->server, view->output, w, h, &w, &h);
	box->x = (int)floor(x);
	box->y = (int)floor(y);
	box->width = (int)lround(w);
	box->height = (int)lround(h);
	if (box->width < 1) {
		box->width = 1;
	}
	if (box->height < 1) {
		box->height = 1;
	}
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
	view_takeover_surface_scale(view);
	if (view->scene_tree != NULL) {
		/* 改判成弹出物 / 变回普通窗口：背后的模糊跟着换。场景树留着（只换
		 * 父节点），所以模糊节点得我们自己销毁。 */
		if (popup && view->popup_blur == NULL && view->x11 != NULL &&
				view->x11->surface != NULL) {
			view->popup_blur = vidow_popup_blur_register(view->server,
				view->scene_tree, &view->scene_tree->node,
				view->x11->surface, view, "x11", NULL);
		} else if (!popup && view->popup_blur != NULL) {
			vidow_popup_blur_unregister(view->popup_blur, true);
			view->popup_blur = NULL;
		}
		wlr_scene_node_reparent(&view->scene_tree->node, popup ?
			view->server->layer_popups : view->server->layer_toplevels);
		view_swap_decor(view);
		/* 搬回 layer_toplevels 的节点落在那一层的**最上面**：它要是个平铺窗口，
		 * 这会儿就压在浮窗头上了 —— 层级收口一次（见 vidow_restack）。 */
		if (!popup) {
			vidow_restack(view->server, NULL);
		}
	}
	wlr_log(WLR_INFO, "x11: %s 归类为%s",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
		popup ? "弹出物（不装饰、不进布局、位置听客户端的）" : "普通窗口");
}

/* One bounded lifecycle trace for X11 views.  It records state transitions and
 * anomalous commits, not every frame, so one reopen gives a complete chain
 * without turning the session log into a frame dump. */
static void x11_trace_view(const char *event, struct vidow_view *view,
		struct wlr_surface *surface) {
	if (view == NULL) {
		return;
	}
	bool xmapped = view->x11 != NULL && view->x11->surface != NULL &&
		view->x11->surface->mapped;
	wlr_log(WLR_INFO,
		"x11-life %s xid=0x%x xs=%p surf=%p xmapped=%d mapped=%d helper=%d "
		"tree=%p scene=%p ws=%p col=%p",
		event,
		view->x11 != NULL ? view->x11->window_id : 0,
		view->x11,
		surface != NULL ? surface :
			(view->x11 != NULL ? view->x11->surface : NULL),
		xmapped, view->mapped, view->focus_helper,
		view->scene_tree, view->scene_surface, view->ws, view->column);
}

/* 直接问 X 服务器：这个窗口现在是 viewable 吗？
 *
 * 不看 wlroots 那边的 surface->mapped —— 它要等一次 commit / 属性流才更新，
 * 而「map 请求先于关联到达」的那种时序里，我们需要的是 X 侧的**真实**状态。
 * 查不到（连接没起来 / 窗口没了）一律当不是，让调用方保持原行为。 */
static bool x11_window_is_viewable(struct vidow_server *server,
		const struct wlr_xwayland_surface *x) {
	if (server == NULL || server->xwayland == NULL || x == NULL ||
			x->window_id == XCB_WINDOW_NONE) {
		return false;
	}
	xcb_connection_t *conn =
		wlr_xwayland_get_xwm_connection(server->xwayland);
	if (conn == NULL) {
		return false;
	}
	xcb_get_window_attributes_cookie_t cookie =
		xcb_get_window_attributes(conn, x->window_id);
	xcb_get_window_attributes_reply_t *reply =
		xcb_get_window_attributes_reply(conn, cookie, NULL);
	if (reply == NULL) {
		return false;
	}
	bool viewable = reply->map_state == XCB_MAP_STATE_VIEWABLE;
	free(reply);
	return viewable;
}

/* Replay the X11 map edge when wlroots mapped the surface without a new
 * commit. This is event-driven and idempotent per association. */
static void x11_replay_map_if_xmapped(struct vidow_view *view,
		bool trust_x_viewable) {
	if (view == NULL || view->kind != VIDOW_VIEW_X11 || view->mapped ||
			view->x11_map_replayed || view->focus_helper || view->popup ||
			view->x11 == NULL || view->x11->surface == NULL ||
			view->scene_tree == NULL) {
		return;
	}
	/* During normal wlroots map/commit handling, the surface flag is authoritative.
	 * Association is different: XWayland can already have an IsViewable window
	 * while the wlroots surface has not observed the map edge yet. */
	bool mapped = view->x11->surface->mapped;
	if (!mapped && trust_x_viewable) {
		mapped = x11_window_is_viewable(view->server, view->x11);
	}
	if (!mapped) {
		return;
	}
	view->x11_map_replayed = true;
	view_map(&view->map, NULL);
}

/* 已经认过账（view->mapped 为真）但场景节点还关着 → 把它打开。
 *
 * 这是 view_map() 幂等短路留下的死角：那函数一进门就 `if (view->mapped)
 * return;`，而「打开节点 + 算 parked」写在它后面。X11 客户端复用同一个 X 窗口
 * 时（Steam 重开主窗口就是这样，xid 不变），第二次 map 会在那行直接返回，
 * 节点留着上次 unmap 时的关闭状态。客户端照常交帧，我们照常收帧，画面上却
 * 什么都没有 —— 这就是「Steam 第二次打开主窗口黑屏」。
 *
 * 判据两条同时成立才动手：
 *   1. 节点确实关着（enabled == false）。我们主动藏窗口走的是 scene_parked，
 *      节点本身仍然 enabled，所以不会误伤。
 *   2. 客户端确实有缓冲。没有缓冲就没什么可显示的，等下一次 commit 再说。
 * 修的是状态，不是事件顺序 —— 跟 Hyprland 在 commitSurface 里按 texture 补
 * map() 是同一个思路（XSurface.cpp:98）。 */
static void x11_remap_if_node_closed(struct vidow_view *view) {
	if (view == NULL || view->kind != VIDOW_VIEW_X11 || !view->mapped ||
			view->popup || view->focus_helper || view->scene_tree == NULL ||
			view->x11 == NULL || view->x11->surface == NULL) {
		return;
	}
	if (view->scene_tree->node.enabled) {
		return; /* 节点开着：正常路径，没这回事 */
	}
	struct wlr_surface *surface = vidow_view_surface(view);
	if (surface == NULL || surface->current.buffer_width <= 0 ||
			surface->current.buffer_height <= 0) {
		return; /* 还没缓冲，没什么可显示的 */
	}
	wlr_log(WLR_INFO, "x11: %s 已认账但节点还关着，重新打开（重开复用同一"
		"个 X 窗口的那条路）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)");
	wlr_scene_node_set_enabled(&view->scene_tree->node, true);
	vidow_view_set_parked(view, false);
	vidow_backdrop_damage_view(view);
	vidow_arrange(view->server);
}

static void x11_attach_surface(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	if (view->x11 == NULL || view->x11->surface == NULL ||
			view->scene_tree != NULL) {
		x11_trace_view("attach-skip", view, NULL);
		return;
	}
	x11_trace_view("attach", view, view->x11->surface);

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
	view_takeover_surface_scale(view);
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

	/* 弹出物背后的模糊：节点垫在它自己的场景树底下。表面 / 场景节点拆掉的
	 * 时候由 x11_drop_surface 显式收（树归我们，节点也归我们）。 */
	if (view->popup && view->popup_blur == NULL) {
		view->popup_blur = vidow_popup_blur_register(server,
			view->scene_tree, &view->scene_tree->node,
			view->x11->surface, view, "x11", NULL);
	}

	/* A new association gets one chance to replay a map that happened before
	 * the scene surface was attached. */
	view->x11_map_replayed = false;
	x11_replay_map_if_xmapped(view, true);

	/* Xwayland can associate an already-painted surface after its last commit.
	 * wlr_scene_surface_create() installs the commit listener, but there may be
	 * no new commit after this association. The current client buffer must be
	 * copied into the new scene buffer explicitly; replaying view_commit() alone
	 * only updates Vidow bookkeeping and cannot replay wlroots' old commit. */
	if (view->x11->surface->buffer != NULL) {
		wlr_scene_buffer_set_buffer(view->scene_surface->buffer,
			&view->x11->surface->buffer->base);
	}
	if (view->x11->surface->buffer != NULL) {
		view_commit(&view->commit, NULL);
	}

}

/* wayland surface 没了（窗口 withdrawn）但 X11 窗口还在：view 留着，下次
 * associate 重新建场景节点。 */
static void x11_drop_surface(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	x11_trace_view("drop", view, NULL);
	/* A wlroots X11 surface can be dissociated and associated again without the
	 * X window being destroyed (Chromium/CEF does this while rebuilding widgets).
	 * Release Vidow-owned scene/FX state before the old scene tree disappears;
	 * otherwise the next association can retain a stale FX node and targets. */
	vidow_anim_cancel(view->anim_open);
	view->anim_open = NULL;
	vidow_anim_cancel(view->anim_move);
	view->anim_move = NULL;
	vidow_anim_cancel(view->mini_anim);
	view->mini_anim = NULL;
	vidow_fx_release(&view->fx);
	/* Dissociation can bypass the XWayland unmap signal. Do the helper-window
	 * focus cleanup here as well, otherwise the next association keeps the
	 * stale focus_helper gate and can never replay its map edge. */
	struct vidow_view *borrowed_owner = server->focus_borrowed_from;
	if (borrowed_owner == view) {
		borrowed_owner = NULL;
	}
	if (view->focus_helper) {
		struct vidow_view *owner = borrowed_owner;
		server->focus_borrowed_from = NULL;
		view->focus_helper = false;
		if (owner != NULL && (!owner->mapped ||
				vidow_view_surface(owner) == NULL)) {
			owner = server->focused;
		} else if (owner == NULL) {
			owner = server->focused;
		}
		struct wlr_surface *back = owner != NULL ?
			vidow_view_surface(owner) : NULL;
		vidow_seat_focus_surface(server->seat, back);
		vidow_im_focus(server, back);
	}
	if (view->popup_blur != NULL) {
		vidow_popup_blur_unregister(view->popup_blur, true);
		view->popup_blur = NULL;
	}
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
	x11_trace_view("drop-done", view, NULL);
	/* Dissociate is not X11 destruction. Keep the view/layout identity, but
	 * force the next association to replay map and rebuild scene placement. */
	view->mapped = false;
	view->x11_map_replayed = false;
	/* **强制清理缓冲缩放状态**：Steam 二次打开时，pref_scale 残留导致
	 * 下次 map 时状态机不同步（scene_tree disabled）。和 view_unmap
	 * 里那处重置逻辑一致（第6460行）。*/
	view->pref_scale = 0;
	vidow_portal_view_mapped(view, false);
}

static void x11_associate(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, x11_associate);
	x11_trace_view("associate", view, view->x11 != NULL ? view->x11->surface : NULL);
	x11_attach_surface(view);
	wlr_log(WLR_DEBUG, "x11: associate %s",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)");

	/* map 请求可能比这条关联先到（Steam 关掉再打开时实测就是）：
	 *
	 *   19.002  map_request   —— 此时 view->x11->surface 还是 NULL，
	 *                            前置条件一条都不满足，事件就这么丢了
	 *   19.015  attach        —— 场景树刚建好
	 *   19.015  associate     —— 本函数
	 *
	 * 而 X 侧窗口从此一直是 IsViewable，不会再补发一次 map 请求 —— 画面就
	 * 永远不上屏，`vidowmsg windows` 里也查不到它（用户报的「重开黑屏」）。
	 *
	 * 所以关联完再问一次 X 服务器的真实状态：窗口已经是 viewable 就直接走
	 * 一遍 map。x11_replay_map_if_xmapped 自己按 x11_map_replayed 去重，
	 * 重复调用是安全的。 */
	if (view->x11 != NULL && view->x11->window_id != XCB_WINDOW_NONE) {
		struct wlr_xwayland_surface *xs = view->x11;
		if (!view->mapped && x11_window_is_viewable(view->server, xs)) {
			x11_replay_map_if_xmapped(view, true);
		}
	}
}

static void x11_dissociate(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, x11_dissociate);
	x11_trace_view("dissociate", view, NULL);
	x11_drop_surface(view);
}

static void x11_map_request(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, x11_map_request);
	if (view->x11 != NULL && view->x11->surface != NULL &&
			view->x11->surface->mapped && view->scene_tree != NULL &&
			!view->mapped) {
			x11_replay_map_if_xmapped(view, false);
	}
}

static void x11_set_title(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, x11_set_title);
	/* Xwayland can emit the first property update in the same batch as
	 * association. If the association was already complete when this view was
	 * created, this is the next safe point to attach the scene surface. */
	if (view->x11 != NULL && view->scene_tree == NULL &&
			view->x11->surface != NULL) {
		x11_attach_surface(view);
	}
	/* DEBUG：同 view_set_title()，日常日志不需要每换一次标题就一行。 */
	wlr_log(WLR_DEBUG, "title %s", vidow_view_title(view) != NULL ?
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
	/* 弹出物改位置/尺寸就是它自己的事：重摆一遍（位置从 X 读）。
	 *
	 * 尺寸变了还得**重新声明场景表面的尺寸**：菜单常以 1x1 建窗、随后才长到
	 * 真实尺寸（Steam 好友列表右键菜单、Qt 下拉框都是这样）。场景表面的 dst
	 * 只在 vidow_view_x11_scale() 里设，而它原来只挂在「commit」和「全局缩放
	 * 变化」上 —— 客户端只是 configure 长大、画面内容没变就不会重新提交，
	 * 表面永远停在 1x1：屏幕上只有一个像素，看起来就是「右键菜单出不来」。 */
	if (view->kind == VIDOW_VIEW_X11) {
		vidow_view_x11_scale(view);
	}
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
	/* S3b：这条信号不只带 min/max 尺寸，也带 PAspect（宽高比）—— 客户端常常
	 * **映射之后**才设它（游戏进全屏那一刻），所以锁比例必须在这儿重判一次，
	 * 而不是只在建 view 时读一遍。 */
	vidow_mini_aspect_changed(view);
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
		/* 尺寸真的变了就得重新声明场景表面的尺寸 —— 和 x11_set_geometry 里
		 * 那段同一个道理：漏了它，1x1 建窗的菜单永远长不大。 */
		if (view->kind == VIDOW_VIEW_X11) {
			vidow_view_x11_scale(view);
		}
		vidow_arrange(view->server);
		return;
	}
	/* 浮动窗口自己定尺寸：客户端请求多大就给多大（X11 里这是它报名尺寸的
	 * 正常途径）。位置仍按我们的规则（居中 / 跟随父窗口）。 */
	if (view->float_mode != VIDOW_FLOAT_NONE && event->width > 1 &&
			event->height > 1) {
		double w = event->width, h = event->height;
		vidow_xwm_from_native_size(view->server, view->output, w, h, &w, &h);
		view->float_geom.width = (int)lround(w);
		view->float_geom.height = (int)lround(h);
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
	/* X11 的 _NET_WM_MOVERESIZE：客户端的标题栏拖拽。按下已经转给它了（X 的
	 * 隐式抓取就是这么来的），松手也得转回去，否则它停在「按住」状态。 */
	/* 拖标题栏 = 平移视图（用户点名的那条）；想移动浮窗用 Mod+左键。 */
	vidow_pan_begin(server, view);
}

static void x11_request_resize(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, x11_request_resize);
	struct vidow_server *server = view->server;
	if (view->float_mode == VIDOW_FLOAT_NONE) {
		vidow_float_set_mode(view, VIDOW_FLOAT_FIXED);
	}
	vidow_float_begin_grab(server, view, true, true);
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
	view_apply_fullscreen(view, view->x11->fullscreen, "X11 请求（_NET_WM_STATE_FULLSCREEN）");
}

static void x11_request_maximize(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view =
		wl_container_of(listener, view, x11_request_maximize);
	if (view->x11 == NULL) {
		return;
	}

	/* X11 的请求没有事件结构，状态就在 surface 上（和全屏那条一样：
	 * x11_request_fullscreen 读的也是 x11->fullscreen）——
	 * _NET_WM_STATE_MAXIMIZED_HORZ / VERT 只要有一个立起来就算要最大化。 */
	bool want = view->x11->maximized_horz || view->x11->maximized_vert;
	struct vidow_column *col = view->column;

	if (col == NULL) {
		/* 浮窗 / 弹出物没有列：布局不替它撑满，把 X 那边的状态掰回「不最大化」
		 * —— 只掰真的有偏差的那种，不然「我们设属性 → 客户端回写属性 → 信号
		 * 又来 → 再设一次」会转圈。 */
		if (want) {
			view->maximize_granted = false;
			vidow_view_set_maximized(view, false);
		}
		return;
	}

	/* 状态已经一致：多半是我们自己 set_maximized 之后客户端把 _NET_WM_STATE
	 * 回写过来触发的回声（X11 没有「请求-应答」，状态就是属性本身）——
	 * 什么也不做，不然就是上面说的那个圈。 */
	if (want == col->maximized) {
		return;
	}

	if (want) {
		view->maximize_granted = true;
		vidow_column_set_maximized(col, true);
		return;
	}
	if (view->maximize_granted) {
		view->maximize_granted = false;
		vidow_column_set_maximized(col, false);
		return;
	}
	/* 列是**用户**最大化的：X 客户端自己把属性撤了，得把它掰回去 —— 不然它以为
	 * 自己是普通尺寸，我们却按整列宽度摆它。这里把去重作废一次（上次发出去的就是
	 * true，状态没变的话会被挡掉）；回声由上面的 `want == col->maximized` 兜住，
	 * 不会转圈。 */
	view->cfg_maximized_sent = false;
	vidow_view_set_maximized(view, col->maximized);
}

/* X11 客户端自己要求最小化 / 放回：`WM_CHANGE_STATE` 的 IconicState / NormalState，
 * 或者 `_NET_WM_STATE_HIDDEN`（托盘图标「收起来 / 放出来」走这条）。wlroots 把两种
 * 都转成同一个信号，event->minimize 就是要不要收起。 */
static void x11_request_minimize(struct wl_listener *listener, void *data) {
	struct wlr_xwayland_minimize_event *event = data;
	struct vidow_view *view =
		wl_container_of(listener, view, x11_request_minimize);

	vidow_view_set_minimized(view, event->minimize);
}

static void x11_destroy_view(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, x11_destroy);
	x11_trace_view("destroy", view, NULL);
	struct vidow_server *server = view->server;
	struct wlr_output *output = view->output;

	vidow_ipc_emit_window_closed(server, view->id,
		view->last_title[0] != '\0' ? view->last_title :
			vidow_view_title(view),
		view->last_app_id[0] != '\0' ? view->last_app_id :
			vidow_view_app_id(view));

	wl_list_remove(&view->x11_associate.link);
	wl_list_remove(&view->x11_dissociate.link);
	wl_list_remove(&view->x11_map_request.link);
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
	wl_list_remove(&view->x11_request_minimize.link);
	wl_list_remove(&view->x11_destroy.link);

	x11_drop_surface(view);
	/* 弹出物可能还攥着键盘（它从来不当焦点窗口），窗口没了就得还回去。 */
	view_popup_release_keyboard(view);
	vidow_portal_view_free(view);
	vidow_mru_forget(server, view);
	/* 动画回调的 data 就是这个 view（view_move_update / view_move_done）：
	 * 窗口在动画没跑完时没了，必须先把动画取消掉，否则下一次 anim tick
	 * 会拿着已释放的 view 调 vidow_view_apply_anim() —— 那就是 2026-10-12
	 * 23:20–23:47 那 5 个 SIGSEGV 核心转储（栈：#0 野地址 <- #1
	 * vidow_output_render <- output_frame <- 帧回调）。 */
	if (view->anim_move != NULL) {
		vidow_anim_cancel(view->anim_move);
		view->anim_move = NULL;
	}
	if (view->anim_open != NULL) {
		vidow_anim_cancel(view->anim_open);
		view->anim_open = NULL;
	}
	wl_list_remove(&view->link);
	/* 同上：果冻先收掉，别留一个攥着野指针的场景节点。 */
	vidow_jelly_view_gone(view);

	if (server->grab_view == view) {
		vidow_float_end_grab(server);
	}
	vidow_float_detach_children(server, view);
	/* 图标态那块快照挂在 layer_toplevels 上，不跟着 scene_tree 走：窗口没了
	 * 就得自己收（不收 = 屏上冻着一块「窗口已经没了」的鬼影）。 */
	mini_snapshot_free(view);
	wlr_buffer_unlock(view->last_buffer);
	view->last_buffer = NULL;
	vidow_fx_release(&view->fx);
	vidow_anim_cancel(view->anim_move);
	view->anim_move = NULL;
	vidow_anim_cancel(view->anim_open);
	view->anim_open = NULL;
	/* 小窗形态转换的补间（S5）data 也是这个 view。 */
	vidow_anim_cancel(view->mini_anim);
	view->mini_anim = NULL;
	/* 悬停预览（S5）挂在 server 上、指着的却是这个 view：先收掉，别留一个
	 * 攥着已释放窗口的镜面节点。 */
	if (server->mini_preview_view == view) {
		vidow_mini_preview_hide(server);
	}

	bool was_focused = server->focused == view;
	/* 邻居要在摘列（和 free(view)）之前找（见 focus_after_close）。 */
	struct vidow_view *next = was_focused ?
		focus_after_close(server, view, output) : NULL;
	if (was_focused) {
		server->focused = NULL;
	}
	vidow_view_leave_column(view);
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
	view->output = view_default_output(server);
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
	view->id = ++server->next_view_id;
	wl_list_init(&view->ws_link);
	wl_list_init(&view->capture_sources);

	wl_list_insert(&server->views, &view->link);
	vidow_portal_view_new(view);

	x11_add_listener(&view->x11_associate, &xsurface->events.associate,
		x11_associate);
	x11_add_listener(&view->x11_dissociate, &xsurface->events.dissociate,
		x11_dissociate);
	x11_add_listener(&view->x11_map_request, &xsurface->events.map_request,
		x11_map_request);
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
	x11_add_listener(&view->x11_request_minimize,
		&xsurface->events.request_minimize, x11_request_minimize);
	x11_add_listener(&view->x11_destroy, &xsurface->events.destroy,
		x11_destroy_view);

	/* 有些窗口送来 new_surface 时表面就已经在了（还没 associate 的就等信号）。 */
	x11_attach_surface(view);

	vidow_float_update_mode(view);

	/* 客户端一上来就是图标化状态（托盘类程序会这么启动）：直接当最小化收起来 ——
	 * 不占位置、不显示、不抢焦点。X 那边本来就是它自己的状态，不用再告诉它一次。 */
	if (xsurface->minimized) {
		view->minimized = true;
		view->minimized_seq = ++view->server->next_minimize_seq;
		wlr_log(WLR_INFO, "x11: %s 启动时就是图标化状态，按最小化收起来",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)");
	}

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
	view->output = view_default_output(server);
	if (view->output == NULL && !wl_list_empty(&server->outputs)) {
		struct vidow_output *o = wl_container_of(server->outputs.next, o, link);
		view->output = o->wlr_output;
	}
	view->tag.magic = VIDOW_VIEW_TAG_MAGIC;
	view->tag.view = view;
	view->focus_on_open = true;
	view->focus_on_activate = true;
	view->honor_invalid_serial = false;
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
	view_takeover_surface_scale(view);
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
	view->request_minimize.notify = view_request_minimize;
	wl_signal_add(&xdg_toplevel->events.request_minimize, &view->request_minimize);
	view->surface_commit.notify = view_surface_commit;
	if (vidow_view_surface(view) != NULL) {
		wl_signal_add(&vidow_view_surface(view)->events.commit,
			&view->surface_commit);
	}
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

/* 把「输出在布局里的盒子」平移成「根 toplevel 表面自己的坐标系」里的盒子。
 *
 * wlr_xdg_popup_unconstrain_from_box() 要的是**根 toplevel 表面的局部坐标**
 * （它内部会减掉 popup 自己的 toplevel 坐标，见 wlroots 的 wlr_xdg_popup.c），
 * 而 wlr_output_layout_get_box() 给的是布局坐标。窗口不在布局原点 (0,0) 上时
 * 这两个差着一整个窗口原点的距离，客户端于是以为「可见区」在它右边一整块屏的
 * 地方，把菜单摆到屏外：微信主窗口在 DP-2（x=2880）上，右键菜单的场景节点
 * 坐标是 4800,115（= 2880 + 1920，1920 就是那块屏的 x），而屏幕只有 0..3840
 * —— 菜单画在屏幕外，看着就是「弹窗不显示」。窗口在 (0,0) 附近时这项差值为 0，
 * 所以单屏 + 窗口贴左上角测不出来（Hyprland 正常，是因为它减了 t1ParentCoords，
 * 见它的 CPopup::unconstrain）。 */
void vidow_popup_box_to_toplevel_space(const struct vidow_view *view,
		struct wlr_box *box) {
	if (view == NULL || box == NULL || view->scene_surface == NULL ||
			view->scene_surface->buffer == NULL) {
		return;
	}
	int sx = 0, sy = 0;
	wlr_scene_node_coords(&view->scene_surface->buffer->node, &sx, &sy);
	box->x -= sx;
	box->y -= sy;
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

/* 弹出物的「commit 了什么」诊断（见 server_new_popup 里那段注释）。 */
struct vidow_popup_trace {
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wlr_xdg_popup *popup;
	struct wlr_scene_tree *tree;
};

static void popup_trace_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_popup_trace *t = wl_container_of(listener, t, commit);
	struct wlr_surface *s = t->popup->base->surface;
	int nx = 0, ny = 0;
	bool enabled = t->tree != NULL && t->tree->node.enabled;
	if (t->tree != NULL) {
		wlr_scene_node_coords(&t->tree->node, &nx, &ny);
	}
	wlr_log(WLR_DEBUG, "popup: commit 缓冲 %dx%d 逻辑 %dx%d 节点%s 在 %d,%d"
		"（表面 mapped=%d initialized=%d）",
		s->current.buffer_width, s->current.buffer_height,
		s->current.width, s->current.height,
		enabled ? "开着" : "关着", nx, ny, s->mapped,
		t->popup->base->initialized);
}

static void popup_trace_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_popup_trace *t = wl_container_of(listener, t, destroy);
	wl_list_remove(&t->commit.link);
	wl_list_remove(&t->destroy.link);
	free(t);
}

/* 弹出物的场景树登记 / 查找（见 server.h 里 vidow_popup_scene 的说明）。
 *
 * 只有「父面是弹出物」时查找才用得着：一级弹出物挂在顶层窗口的树上是**对的**
 * （wlroots 给的坐标就是相对顶层表面的），二级弹出物的父面是**一级弹出物**，
 * 它的坐标相对一级弹出物算 —— 挂到顶层窗口的树上等于把一级弹出物在窗口里的
 * 偏移整个丢掉，二级菜单于是平移一段「一级菜单相对窗口」的距离，看着就是错位。
 * 查不到（父弹出物不是我们建的 / 登记时没内存）就退回原来的挂法。 */
static struct wlr_scene_tree *popup_scene_tree_for(
		struct vidow_server *server, struct wlr_surface *surface) {
	struct vidow_popup_scene *ps;
	wl_list_for_each(ps, &server->popup_scenes, link) {
		if (ps->surface == surface) {
			return ps->tree;
		}
	}
	return NULL;
}

static void popup_scene_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_popup_scene *ps = wl_container_of(listener, ps, destroy);
	/* 场景树由 wlroots 自己跟着 xdg_surface 销毁（见 types/scene/xdg_shell.c），
	 * 这里只把登记项摘掉 —— 留着的话下一个 surface 可能拿到同一个地址。 */
	wl_list_remove(&ps->destroy.link);
	wl_list_remove(&ps->link);
	free(ps);
}

static void popup_scene_register(struct vidow_server *server,
		struct wlr_surface *surface, struct wlr_scene_tree *tree) {
	struct vidow_popup_scene *ps = calloc(1, sizeof(*ps));
	if (ps == NULL) {
		return;
	}
	ps->surface = surface;
	ps->tree = tree;
	ps->destroy.notify = popup_scene_destroy;
	wl_signal_add(&surface->events.destroy, &ps->destroy);
	wl_list_insert(&server->popup_scenes, &ps->link);
}

/* 建一个弹出物的场景树：父面本身是弹出物（二级菜单）就挂到**父弹出物**的树上，
 * 否则挂 fallback（顶层窗口的树 / layer 表面的树）。
 *
 * 为什么要挑父树：wlroots 把弹出物节点的坐标（popup->current.geometry）当成
 * 「相对**父表面**」的偏移（types/scene/xdg_shell.c 的
 * scene_xdg_surface_update_position）。一级弹出物的父表面是窗口 / layer 表面，
 * 挂 fallback 就对；二级弹出物的父表面是**一级弹出物**，挂 fallback 等于把
 * 「一级菜单在窗口 / 状态栏里的位置」整段丢掉 —— 子菜单整体平移
 * (父菜单.x − 窗口.x, 父菜单.y − 窗口.y)，看着就是错位。查不到父树（父弹出物
 * 不是我们建的 / 登记时没内存）就退回 fallback。
 *
 * **两条路都得走这里**：窗口的弹出物（server_new_popup）和 layer surface 的
 * 弹出物（layer.c 的 layer_surface_new_popup）。7768fe7 只在窗口那条路上挑了
 * 父树，layer 那条路（状态栏菜单）漏了，子菜单于是还是老样子 —— 一条规则两个
 * 调用点，漏一个就是这次。 */
struct wlr_scene_tree *vidow_popup_scene_create(struct vidow_server *server,
		struct wlr_scene_tree *fallback, struct wlr_xdg_popup *popup) {
	struct wlr_scene_tree *parent = fallback;
	if (popup->parent != NULL) {
		struct wlr_scene_tree *pt =
			popup_scene_tree_for(server, popup->parent);
		if (pt != NULL) {
			parent = pt;
		}
	}

	/* 同一个 xdg_popup 可能被两条路各建一次：挂在 layer 表面上的弹出物，
	 * xdg_shell 的 new_popup 先以 parent = NULL 到达（这时归 server_new_popup，
	 * 先挂在 layer_toplevels 这棵全局树里），随后 layer_shell 的 new_popup
	 * 才把 parent 补上并再走一次这里。以前第二次会再建一棵树 —— 同一个
	 * surface 两个场景节点：菜单在**两块屏上各出现一份**，其中一份坐标还是
	 * 全局的。已经有树就只把它挪到正确的父树里，不再新建。 */
	struct wlr_scene_tree *tree =
		popup_scene_tree_for(server, popup->base->surface);
	if (tree != NULL) {
		if (tree->node.parent != parent) {
			wlr_scene_node_reparent(&tree->node, parent);
		}
		return tree;
	}
	tree = wlr_scene_xdg_surface_create(parent, popup->base);
	if (tree == NULL) {
		return NULL;
	}
	/* 登记自己：下一级菜单要按 surface 找到这棵树（一级的登记也没白记 ——
	 * 客户端可以再弹一个 popup，链就是这么接下去的）。 */
	popup_scene_register(server, popup->base->surface, tree);
	/* 弹出物背后的模糊（fx.c 的 vidow_popup_blur_register）：节点垫在这棵树
	 * 底下，弹出物移动 / 改尺寸时自动跟着走。树归 wlroots，销毁时它顺手把
	 * 模糊节点一起收（见 popup_blur_node_destroy）。 */
	vidow_popup_blur_register(server, tree, &tree->node,
		popup->base->surface, NULL, "xdg",
		&popup->base->surface->events.destroy);
	/* 每个弹出物挂一条「它自己 commit 了什么」的日志（一行一条，弹出物本来
	 * 就不常 commit）。「弹窗不显示」这类问题全卡在这一步上，光看场景树看不
	 * 出是客户端没交帧还是我们没画：
	 *   没有这一行           = 客户端在等 configure，压根没交缓冲
	 *   有行但节点关着/在屏外 = 我们这边的问题（节点没使能 / 位置算错）
	 *   有行、节点开着在屏内 = 画了但被别的节点压住（层级问题） */
	struct vidow_popup_trace *t = calloc(1, sizeof(*t));
	if (t != NULL) {
		t->popup = popup;
		t->tree = tree;
		t->commit.notify = popup_trace_commit;
		wl_signal_add(&popup->base->surface->events.commit, &t->commit);
		t->destroy.notify = popup_trace_destroy;
		wl_signal_add(&popup->events.destroy, &t->destroy);
	}
	return tree;
}

/* 弹出物的「根」是不是一块 layer surface（状态栏），是就把它找出来。
 *
 * 为什么二级弹出物要走这里：wlroots 的 wlr_layer_shell_v1.c 里，挂在 layer 表面上
 * 的弹出物是**两步**建的 —— 客户端先 xdg_wm_base.get_xdg_popup(parent=NULL)，这时
 * 只发 xdg_shell 那条 new_popup（父面还是 NULL），随后 zwlr_layer_surface_v1.get_popup
 * 才把父面设成 layer 表面并只发 layer 那条 new_popup。所以：一级弹出物归 layer.c 的
 * layer_surface_new_popup 管，**二级弹出物（父面是一级弹出物）只有 xdg_shell 那条信号**
 * —— 它的根还是那块 layer 表面，但 layer.c 永远看不到它，可用区得在这里自己算。 */
static struct vidow_layer_surface *popup_root_layer_surface(
		struct vidow_server *server, const struct wlr_xdg_popup *popup) {
	struct wlr_surface *s = popup->parent;
	while (s != NULL) {
		struct wlr_layer_surface_v1 *lsls =
			wlr_layer_surface_v1_try_from_wlr_surface(s);
		if (lsls != NULL) {
			struct vidow_layer_surface *ls;
			wl_list_for_each(ls, &server->layers, link) {
				if (ls->layer_surface == lsls) {
					return ls;
				}
			}
			return NULL;
		}
		struct wlr_xdg_surface *xs =
			wlr_xdg_surface_try_from_wlr_surface(s);
		if (xs == NULL || xs->role != WLR_XDG_SURFACE_ROLE_POPUP ||
				xs->popup == NULL) {
			return NULL;
		}
		s = xs->popup->parent;
	}
	return NULL;
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
	/* 父面本身是弹出物时（二级菜单）按登记表找父树 —— 见
	 * vidow_popup_scene_create。 */
	vidow_popup_scene_create(server,
		view != NULL ? view->scene_tree : server->layer_toplevels, popup);

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
	if (view != NULL) {
		/* 按窗口**真正落**的那块屏算可用区：view->output 是「它属于哪块屏的
		 * 工作区」，IPC 挪位置 / 跨屏拖拽之后它可能还是旧的那块（浮窗的位置
		 * 是绝对坐标，不依赖 view->output），而菜单能摆在哪儿得看它现在压在
		 * 哪块屏上。 */
		struct wlr_output *out = view->output;
		struct wlr_output *under = wlr_output_layout_output_at(
			server->output_layout,
			view->geom.x + view->geom.width / 2.0,
			view->geom.y + view->geom.height / 2.0);
		if (under != NULL) {
			out = under;
		}
		if (out != NULL) {
			wlr_output_layout_get_box(server->output_layout, out, &box);
			vidow_popup_box_to_toplevel_space(view, &box);
		}
	} else {
		/* 根不是窗口：挂在 layer surface 上（状态栏的菜单）。一级弹出物由
		 * layer.c 算这个盒子，二级弹出物只到这里（见
		 * popup_root_layer_surface）—— 不补这一手 box 就是 {0,0,0,0}，客户端
		 * 拿到「可用区是个 0x0 的盒子」，把子菜单摆到那个点上去：看着就是
		 * 「二级菜单跑到屏幕左上角」。 */
		struct vidow_layer_surface *root =
			popup_root_layer_surface(server, popup);
		vidow_popup_box_to_layer_space(server, root, &box);
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
