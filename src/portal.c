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

	/* 按窗口录屏：source 直接指向窗口的场景节点，跟随窗口移动和重绘。
	 * 它的生命周期挂在场景节点上，节点没了 source 也就没了。 */
	struct wlr_ext_image_capture_source_v1 *source =
		wlr_ext_image_capture_source_v1_create_with_scene_node(
			&view->scene_tree->node,
			wl_display_get_event_loop(server->display),
			server->allocator, server->renderer);
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

void vidow_portal_view_title(struct vidow_view *view) {
	const char *title = or_empty(vidow_view_title(view));
	const char *app_id = or_empty(vidow_view_app_id(view));

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

void vidow_portal_view_mapped(struct vidow_view *view, bool mapped) {
	if (view->ftm_handle == NULL) {
		return;
	}

	if (mapped) {
		if (view->output != NULL && !view->portal_entered) {
			wlr_foreign_toplevel_handle_v1_output_enter(view->ftm_handle,
				view->output);
			view->portal_entered = true;
		}
		return;
	}

	if (view->output != NULL && view->portal_entered) {
		wlr_foreign_toplevel_handle_v1_output_leave(view->ftm_handle,
			view->output);
		view->portal_entered = false;
	}
	/* 窗口下了屏就不再是"当前窗口"。焦点转移不一定经过这里（最后一个窗口
	 * 关掉时没有下一个窗口来接手），所以显式收一次。 */
	vidow_portal_view_activated(view, false);
}

void vidow_portal_view_activated(struct vidow_view *view, bool activated) {
	if (view == NULL || view->ftm_handle == NULL) {
		return;
	}
	if (view->portal_activated == activated) {
		return;
	}
	view->portal_activated = activated;
	wlr_foreign_toplevel_handle_v1_set_activated(view->ftm_handle, activated);
}

void vidow_portal_view_new(struct vidow_view *view) {
	struct vidow_server *server = view->server;

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

	/* 标题多半还是空的（第一个 commit 之前），先同步一次状态，之后靠
	 * set_title / set_parent 事件补。 */
	vidow_portal_view_title(view);
	vidow_portal_view_parent(view);
}

void vidow_portal_view_free(struct vidow_view *view) {
	if (view->ftm_handle != NULL) {
		/* handle 一销毁，挂在它信号上的 listener 也就没了；先摘干净。 */
		wl_list_remove(&view->ftm_request_activate.link);
		wl_list_remove(&view->ftm_request_close.link);
		wl_list_remove(&view->ftm_request_fullscreen.link);
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
