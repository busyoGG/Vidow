/* portal.c — xdg-desktop-portal 及其 GTK / KDE 后端需要的协议。
 *
 * portal 前端（xdg-desktop-portal 本身）不是 Wayland 客户端，它只做 DBus 转发。
 * 真正连到我们这儿的是后端进程：xdg-desktop-portal-gtk（GTK）和
 * xdg-desktop-portal-kde（Qt）。它们用到的协议：
 *
 *   xdg-shell                 窗口本体                      （已有）
 *   xdg-activation-v1         弹窗要焦点 / 通知点击后顶窗口（实现在 activation.c）
 *   xdg-foreign v1 + v2       把对话框挂到发起请求的窗口上（父子关系）
 *   foreign-toplevel-mgmt-v1  窗口列表 + activate / close 请求
 *   ext-foreign-toplevel-list-v1 + ext-foreign-toplevel-image-capture-source-v1
 *                             新版窗口列表 / 按窗口录屏
 *
 * 缺 xdg-foreign：GTK / KDE 的文件选择器会变成一个孤立的顶层窗口，而不是
 * 挂在应用窗口上的对话框（不会居中、不会跟随父窗口最小化）。
 * 缺 xdg-activation：portal 弹出来的对话框拿不到键盘焦点，要手动点一下。
 * 缺 foreign-toplevel：按窗口录屏的 portal 看不到任何可选的窗口。
 *
 * 前端选择哪个后端由 ~/.config/xdg-desktop-portal/<desktop>-portals.conf 决定，
 * 与合成器无关；这里只负责让后端能把界面画对、把窗口关系说清楚。
 */

#include "server.h"

#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v1.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#include <wlr/util/log.h>

/* 客户端可能把 title / app_id 留成 NULL，而 wlroots 会直接 strdup() 它们。 */
static const char *or_empty(const char *s) {
	return s != NULL ? s : "";
}

static struct vidow_view *view_from_surface(struct vidow_server *server,
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

/* ------------------------------------------- foreign-toplevel-management v1 */

static void ftm_request_activate(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view =
		wl_container_of(listener, view, ftm_request_activate);
	/* waybar 点任务栏按钮、portal 把窗口顶到前面，都走这里。 */
	vidow_view_activate(view);
}

static void ftm_request_close(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, ftm_request_close);
	vidow_view_close(view);
}

static void ftm_request_fullscreen(struct wl_listener *listener, void *data) {
	struct wlr_foreign_toplevel_handle_v1_fullscreen_event *event = data;
	struct vidow_view *view =
		wl_container_of(listener, view, ftm_request_fullscreen);

	view->fullscreen = event->fullscreen;
	vidow_view_set_fullscreen(view, event->fullscreen);
	vidow_arrange(view->server);
}

/* 任务栏要求最小化 / 放回（waybar 的 taskbar 模块、各种面板都走这条）。
 * 收起和放回是**同一个信号**，event->minimized 说要哪个 —— 别去找
 * request_unminimize，协议里没那个东西。 */
static void ftm_request_minimize(struct wl_listener *listener, void *data) {
	struct wlr_foreign_toplevel_handle_v1_minimized_event *event = data;
	struct vidow_view *view =
		wl_container_of(listener, view, ftm_request_minimize);

	if (event->minimized) {
		vidow_view_set_minimized(view, true);
	} else if (view->minimized) {
		/* 放回顺手给焦点：点任务栏里那个缩条，期待的就是它回到眼前。 */
		vidow_view_restore(view);
	}
}

/* --------------------------------------------------------- ext-...-list v1 */

static void capture_request(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, new_capture_request);
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request
		*request = data;

	/* 这个 handle 是我们自己建的，data 就是窗口；不是我们的就不认。 */
	struct vidow_view *view = request->toplevel_handle != NULL ?
		request->toplevel_handle->data : NULL;
	if (view == NULL || view->scene_tree == NULL) {
		return;
	}

	/* 按窗口录屏：我们自己那份截图源（见 src/wincapture.c）—— 它渲染的是这个
	 * 窗口自己的一份镜像，跟窗口在屏幕上摆在哪、有没有被裁、停没停到屏外都没
	 * 关系，客户端要一帧就出一帧。生命周期挂在窗口表面上：表面没了源也就没了。 */
	struct wlr_ext_image_capture_source_v1 *source =
		vidow_win_capture_source_create(server, view);
	if (source == NULL) {
		wlr_log(WLR_ERROR, "portal: 无法为窗口创建截图源");
		return;
	}
	if (!wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(
			request, source)) {
		wlr_log(WLR_ERROR, "portal: 窗口截图源请求被拒绝（客户端已走）");
		return;
	}

	/* 有人要录 / 预览这个窗口：它滚到视口外也得接着出帧，否则录到的是冻住的一张。
	 * 记录挂在窗口上（见 server.h 的 vidow_capture_hold）：源销毁、或者录屏客户端
	 * 直接跑掉（资源空了）都算不再持有。 */
	vidow_view_capture_hold(view, server, source, request->client);
	wlr_log(WLR_INFO, "portal: 有人要录 / 预览 %s，它开始强制渲染（视口外也出帧）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)");
}

/* ------------------------------------------------------------ public API */

/* 前向声明：定义在下面 view_new 那一节。 */
static void portal_register_toplevel(struct vidow_view *view);

void vidow_portal_view_title(struct vidow_view *view) {
	const char *title = or_empty(vidow_view_title(view));
	const char *app_id = or_empty(vidow_view_app_id(view));

	/* 标题 / app_id 到齐了才注册（见 view_new 里那段说明）。这里是补注册
	 * 的那条路：正常窗口在 view_new 时还没有 WM_NAME，第一次 commit 之后
	 * 才会有，那时才该出现在任务栏里。已经注册过的幂等跳过。 */
	if (vidow_view_title(view) == NULL && vidow_view_app_id(view) == NULL) {
		return;
	}
	portal_register_toplevel(view);

	if (view->ftm_handle != NULL) {
		wlr_foreign_toplevel_handle_v1_set_title(view->ftm_handle, title);
		wlr_foreign_toplevel_handle_v1_set_app_id(view->ftm_handle, app_id);
	}
	if (view->ftl_handle != NULL) {
		struct wlr_ext_foreign_toplevel_handle_v1_state state = {
			.title = title,
			.app_id = app_id,
		};
		wlr_ext_foreign_toplevel_handle_v1_update_state(view->ftl_handle,
			&state);
	}
}

void vidow_portal_view_parent(struct vidow_view *view) {
	if (view->ftm_handle == NULL) {
		return;
	}
	struct vidow_view *parent =
		view_from_surface(view->server, vidow_view_parent_surface(view));
	wlr_log(WLR_DEBUG, "portal: 父子 %s -> %s", or_empty(vidow_view_title(view)),
		parent != NULL ? or_empty(vidow_view_title(parent)) : "(无)");
	wlr_foreign_toplevel_handle_v1_set_parent(view->ftm_handle,
		parent != NULL ? parent->ftm_handle : NULL);
}

/* 窗口换屏（拖到另一块显示器 / 工作区被搬走）时，foreign-toplevel 的
 * output_enter/leave 也必须跟着换。Noctalia 的 activeToplevelOutput() 就是读
 * 这个集合来判「当前活动窗口在哪块屏」；不换的话活动应用图标会挂到旧屏的
 * 工作区上。 */
void vidow_portal_view_output(struct vidow_view *view) {
	if (view == NULL || view->ftm_handle == NULL) {
		return;
	}
	struct wlr_output *want = view->mapped ? vidow_view_owner_output(view) : NULL;
	if (want == NULL) {
		want = view->output;
	}
	if (view->portal_output == want) {
		return;
	}
	if (view->portal_output != NULL && view->portal_entered) {
		wlr_foreign_toplevel_handle_v1_output_leave(view->ftm_handle,
			view->portal_output);
		view->portal_entered = false;
	}
	view->portal_output = want;
	if (want != NULL) {
		wlr_foreign_toplevel_handle_v1_output_enter(view->ftm_handle, want);
		view->portal_entered = true;
	}
}

void vidow_portal_view_mapped(struct vidow_view *view, bool mapped) {
	if (mapped) {
		/* 上屏了才算数（判据同 Hyprland 的 validMapped && !overrideRedirect，
		 * 见 vidow_portal_view_new 里的说明）。popup / 没上屏的辅助窗口
		 * 都在 portal_register_toplevel 里挡掉。 */
		if (view->popup) {
			return;
		}
		portal_register_toplevel(view);
		vidow_portal_view_title(view);
		vidow_portal_view_parent(view);
		if (view->ftm_handle == NULL) {
			return;
		}
		vidow_portal_view_output(view);
		return;
	}

	/* **顺序不能反**：先给 output 发 leave、先收 activated，最后才销毁
	 * handle。反过来的话 handle 已经是 NULL，output_leave 直接段错误
	 * （2026-10-27 实测 SIGSEGV，栈顶就是
	 * wlr_foreign_toplevel_handle_v1_output_leave）。 */
	/* **handle 可能是 NULL，必须先判。** 这条函数会被走两遍：
	 * x11_drop_surface() 调一次、vidow_view_x11_gone() 里的
	 * vidow_portal_view_drop_toplevel() 之后状态已经清过。第一遍把
	 * ftm_handle 置 NULL 之后，如果 portal_output / portal_entered 还留着，
	 * 第二遍进来就拿着 NULL handle 调 output_leave —— 2026-10-27 两次
	 * SIGSEGV（02:15:12 那次栈顶就是这里，关 Steam 好友列表时触发）。 */
	if (view->ftm_handle != NULL && view->portal_output != NULL &&
			view->portal_entered) {
		wlr_foreign_toplevel_handle_v1_output_leave(view->ftm_handle,
			view->portal_output);
		view->portal_entered = false;
	}
	view->portal_entered = false;
	view->portal_output = NULL;
	/* 窗口下了屏就不再是"当前窗口"。焦点转移不一定经过这里（最后一个窗口
	 * 关掉时没有下一个窗口来接手），所以显式收一次。 */
	vidow_portal_view_activated(view, false);

	/* 最后摘掉条目：辅助窗口 unmap 之后不该继续占着列表。真窗口重新 map
	 * 时会再注册回来。 */
	vidow_portal_view_drop_toplevel(view);
}

void vidow_portal_view_activated(struct vidow_view *view, bool activated) {
	if (view == NULL || view->ftm_handle == NULL) {
		return;
	}
	/* 焦点落到它身上时顺手把 output 集合校正一次：拖过屏再点回来这条路上，
	 * 不一定有人调 mapped。 */
	vidow_portal_view_output(view);
	if (view->portal_activated == activated) {
		return;
	}
	view->portal_activated = activated;
	wlr_foreign_toplevel_handle_v1_set_activated(view->ftm_handle, activated);
	/* 同步全屏状态：dock / 任务栏（noctalia 这类）靠它判断「当前是不是被占满」，
	 * 从而决定自动隐藏。以前只同步 activated，这两个字段永远是 false ——
	 * 用户报的「noctalia 无法判断当前是否窗口占满工作区，dock 无法自动隐藏」。 */
	wlr_foreign_toplevel_handle_v1_set_fullscreen(view->ftm_handle,
		view->fullscreen);
}

/* 把窗口挂进 foreign-toplevel（两份协议都挂）。view_new 和「标题后来才到」
 * 两条路共用 —— 后者见 vidow_portal_view_title()。幂等：已经挂过就直接返回。 */
static void portal_register_toplevel(struct vidow_view *view) {
	struct vidow_server *server = view->server;

	/* **必须 mapped。** 这是 Hyprland 的 validMapped（见 view_new 里的
	 * 说明），也是这里唯一的闸：vidow_portal_view_title() 会在属性变化时
	 * 被反复调用，那时候窗口**可能还没上屏**（Steam / fcitx 的辅助窗口
	 * create 完先写属性、再决定 map 不 map，很多永远不 map）。
	 *
	 * 少了这道闸就会漏：实测 2026-10-27 的注册日志里
	 *   `Steam popup=0 mapped=0`、`QQ popup=0 mapped=0`
	 * 都是没上屏就登记了，顶栏于是摆着它们。 */
	if (view->popup || !view->mapped || view->ftm_handle != NULL ||
			view->ftl_handle != NULL) {
		return;
	}

	if (server->ftm != NULL) {
		view->ftm_handle = wlr_foreign_toplevel_handle_v1_create(server->ftm);
		if (view->ftm_handle != NULL) {
			view->ftm_handle->data = view;
			view->ftm_request_activate.notify = ftm_request_activate;
			wl_signal_add(&view->ftm_handle->events.request_activate,
				&view->ftm_request_activate);
			view->ftm_request_close.notify = ftm_request_close;
			wl_signal_add(&view->ftm_handle->events.request_close,
				&view->ftm_request_close);
			view->ftm_request_fullscreen.notify = ftm_request_fullscreen;
			wl_signal_add(&view->ftm_handle->events.request_fullscreen,
				&view->ftm_request_fullscreen);
			view->ftm_request_minimize.notify = ftm_request_minimize;
			wl_signal_add(&view->ftm_handle->events.request_minimize,
				&view->ftm_request_minimize);
		}
	}

	if (server->ftl != NULL) {
		struct wlr_ext_foreign_toplevel_handle_v1_state state = {
			.title = or_empty(vidow_view_title(view)),
			.app_id = or_empty(vidow_view_app_id(view)),
		};
		view->ftl_handle =
			wlr_ext_foreign_toplevel_handle_v1_create(server->ftl, &state);
		if (view->ftl_handle != NULL) {
			view->ftl_handle->data = view;
		}
	}
}

void vidow_portal_view_new(struct vidow_view *view) {
	/* **这里不注册。** 注册推迟到 map，见 vidow_portal_view_mapped()。
	 *
	 * 照抄 Hyprland 的判据（src/protocols/ForeignToplevelWlr.cpp:409）：
	 *   validMapped(w) && !overrideRedirect
	 * 关键是 **mapped** —— 只有真上了屏的窗口才算「任务栏里该有的窗口」。
	 *
	 * 以前这里无条件注册，实测（2026-10-27，254 次注册里 223 次）全都是
	 * Steam / CEF 的辅助窗口：10x10 的 `steam`、200x200 的 `steamwebhelper`、
	 * `default` / `ime` 这类输入法小窗。它们 `Map State: IsUnMapped`、永远
	 * 不上屏，却有名字，于是 noctalia 顶栏摆出一串谁也点不动的条目，还会
	 * 在 Steam 弹消息时把焦点抢过去（用户实测「steam消息弹窗也会抢焦点」）。
	 *
	 * 排除判据只看 mapped，不看标题 —— 那批窗口**是有名字的**，按标题挡
	 * 挡不住（上一版就是这么漏的）。 */
	(void)view;
}

/* 窗口从「普通窗口」改判成弹出物（_NET_WM_WINDOW_TYPE 晚到那条路，见
 * view.c 的 x11_refresh_popup）：把已经注册上去的 foreign-toplevel 条目撤掉。
 * 不撤的话列表里会留一个没有名字、点不动的幽灵条目 —— 胶囊一冒出来就再也
 * 不消失。 */
void vidow_portal_view_drop_toplevel(struct vidow_view *view) {
	if (view == NULL) {
		return;
	}
	if (view->ftm_handle != NULL) {
		wl_list_remove(&view->ftm_request_activate.link);
		wl_list_remove(&view->ftm_request_close.link);
		wl_list_remove(&view->ftm_request_fullscreen.link);
		wl_list_remove(&view->ftm_request_minimize.link);
		wlr_foreign_toplevel_handle_v1_destroy(view->ftm_handle);
		view->ftm_handle = NULL;
	}
	if (view->ftl_handle != NULL) {
		wlr_ext_foreign_toplevel_handle_v1_destroy(view->ftl_handle);
		view->ftl_handle = NULL;
	}
}

void vidow_portal_view_free(struct vidow_view *view) {
	if (view->ftm_handle != NULL) {
		/* handle 一销毁，挂在它信号上的 listener 也就没了；先摘干净。 */
		wl_list_remove(&view->ftm_request_activate.link);
		wl_list_remove(&view->ftm_request_close.link);
		wl_list_remove(&view->ftm_request_fullscreen.link);
		wl_list_remove(&view->ftm_request_minimize.link);
		wlr_foreign_toplevel_handle_v1_destroy(view->ftm_handle);
		view->ftm_handle = NULL;
	}
	if (view->ftl_handle != NULL) {
		wlr_ext_foreign_toplevel_handle_v1_destroy(view->ftl_handle);
		view->ftl_handle = NULL;
	}
}

/* -------------------------------------------------------------------- init */

bool vidow_portal_init(struct vidow_server *server) {
	/* xdg-activation-v1 自己实现（activation.c）：wlroots 那份把 serial 校验
	 * 写死了，配不了 honor_xdg_activation_with_invalid_serial。 */
	vidow_activation_init(server);

	/* xdg-foreign v1 / v2 共用同一个 registry，这样用 v1 导出的窗口也能被
	 * 用 v2 的客户端导入（GTK3 说 v1，GTK4 / Qt6 说 v2）。 */
	server->foreign_registry = wlr_xdg_foreign_registry_create(server->display);
	if (server->foreign_registry == NULL) {
		wlr_log(WLR_ERROR, "portal: 无法创建 xdg_foreign_registry");
		return false;
	}
	server->foreign_v1 = wlr_xdg_foreign_v1_create(server->display,
		server->foreign_registry);
	server->foreign_v2 = wlr_xdg_foreign_v2_create(server->display,
		server->foreign_registry);
	if (server->foreign_v1 == NULL || server->foreign_v2 == NULL) {
		wlr_log(WLR_ERROR, "portal: 无法创建 xdg_foreign v1/v2");
		return false;
	}

	/* 窗口列表：waybar 的任务栏、portal 的"选一个窗口"都用它。 */
	server->ftm = wlr_foreign_toplevel_manager_v1_create(server->display);
	if (server->ftm == NULL) {
		wlr_log(WLR_ERROR, "portal: 无法创建 foreign_toplevel_manager_v1");
		return false;
	}
	server->ftl = wlr_ext_foreign_toplevel_list_v1_create(server->display, 1);
	server->ftl_capture =
		wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(
			server->display, 1);
	if (server->ftl_capture != NULL) {
		server->new_capture_request.notify = capture_request;
		wl_signal_add(&server->ftl_capture->events.new_request,
			&server->new_capture_request);
	}

	wlr_log(WLR_INFO, "portal: xdg-activation / xdg-foreign v1+v2 / "
		"foreign-toplevel 已就绪");
	return true;
}
