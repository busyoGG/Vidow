/* 窗口属性与几何：标题 / app_id / 几何 / 缩放 / 大小 / 全屏 / 命中测试。
 *
 * 这一层只读改 view 自己的字段，不碰布局（列、条带、视口都不在这里）。调用方是
 * 别处所有模块：IPC 查询、输入命中、X11、渲染。所以它是 view 里最靠外的一层，
 * 没有反向依赖 —— 只借了三个更靠里的小工具（裁剪框 / 小窗原生尺寸 / X11 归屏），
 * 走 view-internal.h。
 */
#include "server.h"
#include "view-internal.h"
#include "ipc.h"
#include "x11surface.h"
#include "xtrace.h"
#include "xwm.h"

#include <math.h>
#include <stdlib.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

/* ------------------------------------------------------------- lookup */

struct vidow_view *view_from_surface(struct vidow_server *server,
		struct wlr_surface *surface) {
	if (surface == NULL) {
		return NULL;
	}

	struct vidow_x11_surface *xsurface =
		vidow_x11_surface_from_wl_surface(surface);
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
		/* 再挡一次：X 窗口已经销毁、x11 结构已释放，view 还挂在布局里
		 * （见 xwm_x11.c 的 x11_handle_destroy）。 */
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

/* 规则里显式写了 x11_scale 的，几何按它修正。
 *
 * 背景：X 屏幕是设备像素，vidow 的逻辑坐标要按「这块屏的 native:logical 比」
 * 换算。规则里的 x11_scale 是给**这个客户端**的声明（它按几倍画），两者不一致
 * 时几何得跟着规则走，否则声明和画面各走一半：声明 1 倍、画面按 2 倍缩小，
 * 窗口在屏上小一半（test-x11scale.sh 的「带规则」就是量这个）。
 *
 * 只在规则显式给了 x11_scale 时修正；没给就保持按输出换算 —— 混合 DPI 下每块
 * 屏的比不一样，全局 xwayland_scale 不该拿来当基准（右屏 2x、左屏 1x）。 */
void x11_apply_rule_scale(const struct vidow_view *view,
		double *w, double *h) {
	if (view == NULL || !(view->x11_scale > 0.0) || w == NULL || h == NULL) {
		return;
	}
	int sc = vidow_view_x11_scale_int(view);
	/* output 还没指派时按 X 侧坐标找屏 —— 弹窗尤其常见（先建视图、后挂屏），
	 * 这里要是直接 return，规则就静默失效，菜单和父窗口一大一小。 */
	struct wlr_output *out = view->output;
	if (out == NULL && view->x11 != NULL) {
		out = vidow_xwm_output_at_native(view->server, view->x11->x,
			view->x11->y);
	}
	struct wlr_box nb = {0}, lb = {0};
	if (sc <= 0 || out == NULL || !vidow_xwm_output_boxes(view->server, out,
			&lb, &nb) || nb.width <= 0 || lb.width <= 0) {
		return;
	}
	double k = ((double)nb.width / (double)lb.width) / (double)sc;
	*w *= k;
	*h *= k;
}

struct wlr_box vidow_view_geometry(const struct vidow_view *view) {
	if (view->kind == VIDOW_VIEW_X11) {
		/* X11 窗口没有「窗口几何」和「表面」的区别：整块就是内容。 */
		struct wlr_box box = {0};
		if (view->x11 != NULL) {
			double w = view->x11->width, h = view->x11->height;
			vidow_xwm_from_native_size(view->server, view->output, w, h, &w, &h);
			x11_apply_rule_scale(view, &w, &h);
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
void fx_geo_tex_map(struct vidow_fx_geo *geo,
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
			/* 这里**故意**不吃规则里的 x11_scale：它的返回值被平铺 / 浮动
			 * 布局当「客户端想要的尺寸」用，按规则放大后固定尺寸的 X 窗口
			 * 会被当成「想占大地方」，在 628x348 的工作区上直接铺满
			 * （实测 place 序列 300 → 150 → 600x348 → 628x348）。
			 * 场景尺寸、布局几何、弹窗盒子三处已经按规则修好，这里保持
			 * 按输出换算。 */
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
			/* 这里以前一言不发地 return，害得我把「日志里没有 x11-cfg」
			 * 读成「configure 没发」—— 其实这条日志压根不存在，首开正常
			 * 时也是 0 条。排「Steam 重开黑屏」两种结局都得看得见。 */
			/* 只在 VIDOW_X11_TRACE 下打：这条挂在 configure 路径上，
			 * 一次会话实测 526 行同步写盘。 */
			if (vidow_xtrace_on()) {
				wlr_log(WLR_INFO, "x11-cfg %s: 跳过 %d,%d %dx%d"
					"（和上次一模一样）",
					vidow_view_title(view) != NULL ?
						vidow_view_title(view) : "(无题)",
					x, y, cw, ch);
			}
			return; /* 位置尺寸一个都没变：别让客户端白重画一帧 */
		}
		/* 「尺寸没变」也要回一声，否则客户端不知道自己被确认了。
		 *
		 * ICCCM 规定：请求的尺寸和当前尺寸相同时，X 服务器不会自动发
		 * ConfigureNotify（它只在真的发生改变时发）。Hyprland 在
		 * XSurface.cpp:206 专门为这种情况补了一个合成事件，注释写得很清楚
		 * （"ICCCM requires a synthetic event when window size is not changed"）。
		 *
		 * 我们没补。于是「关掉窗口再打开、位置尺寸和上次一模一样」这一路
		 * （Steam 主窗口就是这样）走下来：configure 发出去了，客户端交了
		 * 一帧黑的，然后停在那儿等确认 —— 等一个永远不会来的
		 * ConfigureNotify。画面就这么定在黑色上，而 X 侧其实一直在正常
		 * 绘制（import -window 能抓到完整画面），`vidowmsg` 里 mapped /
		 * visible / placed 也全都是对的。
		 *
		 * 判据是「这次要发的几何 == 上一次真发出去的几何」。
		 *
		 * 刻意**不看** cfg_sent：它记的是「这一轮协商里已经发过没有」，
		 * 而 view_unmap 会把它清成 false（unmap 之后下一次 map 是一次全新
		 * 协商）。拿它当条件，重开时它一定是 0，这段补发就永远不触发 ——
		 * 第一次写完实测就是这么哑的（日志里 `上次 16,94 3808x2050`
		 * 和这次一模一样，却一条「补发」都没有）。
		 *
		 * cfg_x/y/w/h 才是「上一次真的发出去的值」，unmap 不清它 ——
		 * 正好用它判断「X 会不会自己发 ConfigureNotify」。 */
		bool same_as_last = view->cfg_x == x &&
			view->cfg_y == y && view->cfg_w == cw &&
			view->cfg_h == ch;
		wlr_log(WLR_INFO, "x11-cfg %s: 发 %d,%d %dx%d（上次 %d,%d %dx%d，"
			"cfg_sent=%d）",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)",
			x, y, cw, ch, view->cfg_x, view->cfg_y, view->cfg_w,
			view->cfg_h, view->cfg_sent);
		vidow_x11_surface_configure(view->x11, (int16_t)x, (int16_t)y,
			(uint16_t)cw, (uint16_t)ch);
		/* 尺寸和上一轮完全相同时，X 服务器不会自己发 ConfigureNotify
		 * （ICCCM 只在尺寸真的变了才发）。手动补一个，否则客户端一直等
		 * 确认、不再重绘 —— 见上面 same_as_last 那段注释。 */
		if (same_as_last) {
			xcb_connection_t *conn = vidow_x11_connection();
			wlr_log(WLR_DEBUG, "x11-cfg %s: 命中「尺寸没变」分支，"
				"conn=%p window_id=0x%x",
				vidow_view_title(view) != NULL ?
					vidow_view_title(view) : "(无题)",
				(void *)conn,
				(unsigned)view->x11->window_id);
			if (conn != NULL &&
					view->x11->window_id != XCB_WINDOW_NONE) {
				xcb_configure_notify_event_t e;
				memset(&e, 0, sizeof(e));
				e.response_type = XCB_CONFIGURE_NOTIFY;
				e.event = view->x11->window_id;
				e.window = view->x11->window_id;
				e.x = (int16_t)x;
				e.y = (int16_t)y;
				e.width = (uint16_t)cw;
				e.height = (uint16_t)ch;
				e.border_width = 0;
				e.above_sibling = XCB_NONE;
				e.override_redirect = view->x11->override_redirect;
				xcb_send_event(conn, false, view->x11->window_id,
					XCB_EVENT_MASK_STRUCTURE_NOTIFY,
					(const char *)&e);
				xcb_flush(conn);
				wlr_log(WLR_DEBUG, "x11-cfg %s: 尺寸没变，补发 "
					"ConfigureNotify %d,%d %dx%d",
					vidow_view_title(view) != NULL ?
						vidow_view_title(view) : "(无题)",
					x, y, cw, ch);
			}
		}
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
		vidow_x11_surface_activate(view->x11, activated);
		return;
	}
	wlr_xdg_toplevel_set_activated(view->xdg_toplevel, activated);
}

void vidow_view_set_fullscreen(struct vidow_view *view, bool fullscreen) {
	if (!vidow_view_configure_ready(view)) {
		return;
	}
	if (view->kind == VIDOW_VIEW_X11) {
		vidow_x11_surface_set_fullscreen(view->x11, fullscreen);
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
void view_apply_fullscreen(struct vidow_view *view, bool want,
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
void view_sync_x11_fullscreen(struct vidow_view *view) {
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
		vidow_x11_surface_set_maximized(view->x11, maximized, maximized);
		return;
	}
	wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, maximized);
}

void vidow_view_close(struct vidow_view *view) {
	if (view->kind == VIDOW_VIEW_X11) {
		vidow_x11_surface_close(view->x11);
		return;
	}
	wlr_xdg_toplevel_send_close(view->xdg_toplevel);
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
	/* 规则里显式写了 x11_scale 的，几何也要照它走。
	 *
	 * 这条以前只当 buffer-scale 提示用（下面的注释写着 "never this DPI-related
	 * integer"），结果 x11_scale=1 的窗口缓冲区声明成 1 倍、物理尺寸 300，
	 * 场景尺寸却照样被按输出的 native:logical 比（2）除成 150 —— 声明和几何
	 * 各走一半，窗口在屏上小一半。test-x11scale.sh 的「带规则」三条全红就是它。
	 *
	 * 只在规则显式给了 x11_scale 时才按它修正；没给的时候保持按输出换算
	 * （混合 DPI 下每块屏的 native:logical 不一样，全局 xwayland_scale 不该
	 * 拿来当基准）。 */
	int scale = vidow_view_x11_scale_int(view);
	bool scale_from_rule = view->x11_scale > 0.0;
	double dw = bw, dh = bh;
	/* ⚠️ output 缺失时换算会**静默失败**（transform() 查不到布局框就原样返回），
	 * dest 于是等于缓冲像素数（2 倍大），窗口在屏上被放大两倍、右下一半跑到屏外。
	 * 和 vidow_xwm_surface_box() 一样按 X 侧坐标兜底找屏，别让它空着。
	 *
	 * （注：这条兜底不是 Steam 重开黑屏的原因 —— 实测 conv_out 一直是有效的
	 * DP-3，换算也一直算对 1904。它防的是另一条路：output 尚未指派时
	 * dest 会被算成缓冲像素数。留着是因为错的那一版确实是错的。） */
	struct wlr_output *conv_out = view->output;
	if (conv_out == NULL && view->x11 != NULL) {
		conv_out = vidow_xwm_output_at_native(view->server,
			view->x11->x, view->x11->y);
	}
	vidow_xwm_from_native_size(view->server, conv_out, bw, bh, &dw, &dh);
	if (scale_from_rule) {
		struct wlr_box nb = {0}, lb = {0};
		if (vidow_xwm_output_boxes(view->server, conv_out, &lb, &nb) &&
				nb.width > 0 && lb.width > 0 && scale > 0) {
			double out_scale = (double)nb.width / (double)lb.width;
			double k = out_scale / (double)scale;
			dw *= k;
			dh *= k;
		}
	}
	(void)scale_from_rule;
	if (vidow_xtrace_on()) {
		struct wlr_box nb = {0}, lb = {0};
		vidow_xwm_output_boxes(view->server, conv_out, &lb, &nb);
		wlr_log(WLR_INFO, "x11-dst: %s out=%s bw=%d -> dw=%.0f "
			"（native %dx%d logical %dx%d，比 %.3f）",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)",
			conv_out != NULL ? conv_out->name : "NULL", bw, dw,
			nb.width, nb.height, lb.width, lb.height,
			nb.width > 0 ? (double)lb.width / (double)nb.width : -1.0);
	}
	int dst_w = (int)lround(dw), dst_h = (int)lround(dh);
	if (dst_w < 1) {
		dst_w = 1;
	}
	if (dst_h < 1) {
		dst_h = 1;
	}
	struct wlr_scene_buffer *node = view->scene_surface->buffer;
	bool changed = node->dst_width != dst_w || node->dst_height != dst_h;
	if (vidow_xtrace_on() && changed) {
		wlr_log(WLR_INFO, "x11-dst-was: %s 之前 dst=%dx%d，将改成 %dx%d",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)",
			node->dst_width, node->dst_height, dst_w, dst_h);
	}
	wlr_scene_buffer_set_dest_size(node, dst_w, dst_h);
	if (vidow_xtrace_on()) {
		wlr_log(WLR_INFO, "x11-dst-set: %s 写 %dx%d，读回 %dx%d",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)",
			dst_w, dst_h, node->dst_width, node->dst_height);
	}
	/* X11 Depth 32 窗口的 alpha 该不该当真，按 EWMH 的老规矩判：
	 *
	 *   1. 设了 _NET_WM_WINDOW_OPACITY（< 1.0）→ 用 alpha（wlroots 已经把
	 *      这个属性解析进 x11->opacity 了，不用自己开 XCB 连接）；
	 *   2. 窗口类型天然带透明（工具提示、下拉、菜单、通知、DND……）→ 用 alpha；
	 *   3. 其余（普通窗口）→ **忽略 alpha，整块当不透明**。
	 *
	 * 第 3 条不是优化，是必须的：XWayland 给 Depth 32 窗口交的是 ARGB8888
	 * （x11-fmt 里 fmt=0x34325241），而它的 alpha 填 0 —— 按预乘采样一遍就
	 * 是 `RGB × 0 = 纯黑`。Steam 重开主窗口正好踩中：首开交 XR24
	 * （fmt=0x34325258）画得好好的，重开换成 AR24 就全黑，而 `import -window`
	 * 从 X 侧抓出来是有内容的（24 位 RGB），所以内容一直都在，是我们解释错
	 * 了缓冲区。普通窗口本来就没有透明通道可言，X 那边的合成结果已经拍平了。 */
	/* ⚠️ **`opacity == 0` 的语义是「没设」，不是「全透明」。**
	 *
	 * 见 x11surface.h：`_NET_WM_OPACITY` 存的是 0..0xffffffff 的整数，0 = 没设。
	 * 以前这里直接写 `opacity < 1.0f`，于是**每个**没设过这个属性的 X11 窗口都
	 * 被当成「要用 alpha 且透明度为 0」—— 整块乘 0，画出来全黑。
	 *
	 * 为什么首开看不出来：XWayland 首开给 Steam 交的是 XR24（无 alpha 通道），
	 * `set_opacity(0)` 对 XRGB 没有可见效果，照样画；关掉再开换成 AR24，
	 * alpha 通道一生效，整块就被乘成 0 —— 用户报的「Steam 第二次打开黑屏、
	 * 拖一下或 resize 就好」就是这个（resize 让它短暂换成 XR24 就又正常）。
	 *
	 * 判据改成「真的设过、且真的 < 1」：`opacity > 0 && opacity < 1.0f`。 */
	bool use_alpha = view->x11 != NULL && view->x11->opacity > 0 &&
		view->x11->opacity < 1.0f;
	static const int translucent_types[] = {
		VIDOW_X11_WT_TOOLTIP,
		VIDOW_X11_WT_DROPDOWN_MENU,
		VIDOW_X11_WT_POPUP_MENU,
		VIDOW_X11_WT_COMBO,
		VIDOW_X11_WT_DND,
		VIDOW_X11_WT_NOTIFICATION,
		VIDOW_X11_WT_SPLASH,
	};
	if (!use_alpha && view->x11 != NULL) {
		for (size_t i = 0;
				i < sizeof(translucent_types) /
					sizeof(translucent_types[0]); i++) {
			if (vidow_x11_surface_has_window_type(view->x11,
					translucent_types[i])) {
				use_alpha = true;
				break;
			}
		}
	}
	/* 只在判定结果变了的时候动节点：每帧重设一遍会让场景白白重烘。
	 * 记在 view->x11_alpha_on / x11_opacity 里，两个值都没变就跳过。 */
	float want_opacity = use_alpha && view->x11 != NULL &&
		view->x11->opacity > 0 && view->x11->opacity < 1.0f ?
		view->x11->opacity : 1.0f;
	if (!view->x11_alpha_known || view->x11_alpha_on != use_alpha ||
			view->x11_opacity != want_opacity) {
		view->x11_alpha_known = true;
		view->x11_alpha_on = use_alpha;
		view->x11_opacity = want_opacity;
		if (!use_alpha) {
			pixman_region32_t opaque;
			pixman_region32_init_rect(&opaque, 0, 0, dst_w, dst_h);
			wlr_scene_buffer_set_opaque_region(node, &opaque);
			pixman_region32_fini(&opaque);
		} else {
			/* 用 alpha 的窗口必须清掉那条「整块不透明」的账，否则换了
			 * 类型 / opacity 之后还按旧账画。 */
			pixman_region32_t empty;
			pixman_region32_init(&empty);
			wlr_scene_buffer_set_opaque_region(node, &empty);
			pixman_region32_fini(&empty);
		}
		wlr_scene_buffer_set_opacity(node, want_opacity);
	}
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
	if (vidow_xtrace_on()) {
		wlr_log(WLR_INFO, "x11-alpha: %s use_alpha=%d op=%.2f "
			"known=%d on=%d want_op=%.2f node_op=%.2f "
			"dst=%dx%d buf=%dx%d surf=%dx%d",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)",
			use_alpha, view->x11 != NULL ? view->x11->opacity : -1.0f,
			view->x11_alpha_known, view->x11_alpha_on, want_opacity,
			node->opacity,
			dst_w, dst_h, bw, bh,
			surface->current.width, surface->current.height);
	}
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
void view_dst_capture(struct vidow_view *view) {
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
	/* **内容全透明被关节点的 layer：命中兜底**（dec9bba 的另一半）。
	 *
	 * wlr_scene_node_at 不看关掉的节点 —— quickshell 的 dock 缩下去之后，
	 * 它底边那条热区（MouseArea，NoButton，专吃 hover 把 dock 叫出来，
	 * Dock.qml 的 hotZoneArea）就永远收不到指针事件，触发死了。热区是
	 * layer surface 自己 input region 里的一块，客户端明说「这里要输入」，
	 * 只是这块恰好全透明。top / overlay 层本来就压在窗口上面，所以这个
	 * 兜底放在场景命中**之前**：节点关着 + 指针在 geom 里 +
	 * wlr_surface_point_accepts_input 认这点 → 按它命中。input region
	 * 为空 / 不含这点（osd 之类全屏透明画布）→ 照旧落到场景命中，桌面
	 * 输入不受影响。 */
	{
		struct vidow_layer_surface *hot = NULL;
		int hx = 0, hy = 0;
		struct vidow_layer_surface *ls;
		wl_list_for_each(ls, &server->layers, link) {
			if (!ls->hide_content || ls->scene == NULL ||
					ls->scene->tree->node.enabled ||
					ls->layer_surface == NULL ||
					ls->layer_surface->surface == NULL ||
					!ls->layer_surface->surface->mapped) {
				continue;
			}
			int nx = ls->geom.x + (int)lround(ls->anim_off_x);
			int ny = ls->geom.y + (int)lround(ls->anim_off_y);
			double px = lx - nx, py = ly - ny;
			if (!wlr_surface_point_accepts_input(
					ls->layer_surface->surface, px, py)) {
				continue;
			}
			hot = ls; /* 列表后面的在上层（后创建的压着先创建的） */
			hx = nx;
			hy = ny;
		}
		if (hot != NULL) {
			if (surface != NULL) {
				*surface = hot->layer_surface->surface;
			}
			if (sx != NULL) {
				*sx = lx - hx;
			}
			if (sy != NULL) {
				*sy = ly - hy;
			}
			return NULL;
		}
	}
	struct wlr_scene_node *node =
		wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
	if (getenv("VIDOW_PICK_TRACE") != NULL) {
		int nx = 0, ny = 0;
		if (node != NULL) wlr_scene_node_coords(node, &nx, &ny);
		struct wlr_scene_buffer *sb = node != NULL &&
			node->type == WLR_SCENE_NODE_BUFFER ?
			wlr_scene_buffer_from_node(node) : NULL;
		wlr_log(WLR_INFO, "pick-trace: %.0f,%.0f node=%p type=%d at %d,%d "
			"scene_surface=%p parent=%p", lx, ly, (void *)node,
			node != NULL ? (int)node->type : -1, nx, ny,
			sb != NULL ? (void *)wlr_scene_surface_try_from_buffer(sb) : NULL,
			node != NULL ? (void *)node->parent : NULL);
	}
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
