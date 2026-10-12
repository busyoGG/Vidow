/* 键盘那两个协议：`zwp_keyboard_shortcuts_inhibit_manager_v1`（客户端说「别吃我的
 * 快捷键」）和 `zwp_xwayland_keyboard_grab_manager_v1`（Xwayland 替 X11 的
 * XGrabKeyboard 要键盘）。
 *
 * 为什么缺不得：`strings /usr/bin/Xwayland` 里就有这两个接口名，缺了它们
 *   - 虚拟机的窗口 / 远程桌面客户端（looking-glass 那类）按不了 Super、Alt+Tab ——
 *     这些键被合成器自己的键位吃掉，进不了客户端的客人系统；
 *   - X11 的 `XGrabKeyboard` 传不到合成器，Xwayland 只能在 X 那边假装抓到了：
 *     焦点一换（点了别的窗口）按键就跑到别处去，抓着键盘的那个客户端再也收不到键。
 *
 * 两个协议 wlroots 0.20 的情况不一样：
 *   - 抑制：wlroots 有（wlr_keyboard_shortcuts_inhibit_v1.h），它管 global 和
 *     active/inactive 事件，**「抑制到底抑制了什么」是合成器的活**；
 *   - X11 抓取：wlroots 连头文件都没有，服务端自己写（协议 XML 在
 *     protocols/xwayland-keyboard-grab-unstable-v1.xml，和 ext-background-effect
 *     一样现场生成 server-header + private-code）。
 *
 * 策略：
 *   1. 抑制：客户端一要就给（activate，和 sway 一样），但**只有当抑制的那个表面
 *      正是键盘焦点时才算数**（协议原话：inhibitors 只在它的表面有焦点时生效）。
 *      算数的时候 vidow 自己的键位全部让路 —— 客户端要的就是原样收到这些键。
 *      键位可以在配置里用 `vidow.bind(..., { inhibited = true })` 单独豁免
 *      （逃生口：不然一个抑制着的窗口能把用户锁死在里面）。
 *   2. X11 抓取：Xwayland 建一个 grab 对象，我们把**座位键盘焦点**按到它那个表面上
 *      （`view_popup_grab_keyboard` 那套：只挪 seat 焦点，不动 server->focused ——
 *      焦点边框该是谁还是谁）。之后每次按键都再确认一遍（vidow_xwayland_grab_assert），
 *      因为 X11 的语义是「谁抓谁收，不管焦点在谁身上」：用户点了别的窗口，键还是
 *      给抓着键盘的那个 X 客户端。vidow 自己的键位照常生效（协议明说合成器可以
 *      过滤一部分自己用），抓取对象销毁时把焦点还给 server->focused。
 *
 * 访问限制：协议要求「只给 Xwayland」。grab_keyboard 里查表面是不是 Xwayland 的
 * （wlr_xwayland_surface_try_from_wlr_surface），不是就回协议错误、不建对象。
 *
 * 判据：./test-keyboard-grab.sh（Wayland 客户端抑制 + X11 的 XGrabKeyboard）。 */

#include "server.h"

#include <stdlib.h>

#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/util/log.h>

#include "json.h"
#include "xwayland-keyboard-grab-unstable-v1-server-protocol.h"

/* --------------------------------------------------------------- 抑制 */

static void inhibitor_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_shortcuts_inhibitor *inh =
		wl_container_of(listener, inh, destroy);
	wl_list_remove(&inh->destroy.link);
	wl_list_remove(&inh->link);
	free(inh);
}

static void handle_new_inhibitor(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, new_inhibitor);
	struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor = data;

	/* 先登记再认：认了（activate）之后这个抑制就算数了，可我们的名单里没有它，
	 * 那它抑制不到任何东西（而且客户端会以为合成器答应了）。 */
	struct vidow_shortcuts_inhibitor *inh = calloc(1, sizeof(*inh));
	if (inh == NULL) {
		return;
	}
	inh->inhibitor = inhibitor;
	inh->destroy.notify = inhibitor_destroy;
	wl_signal_add(&inhibitor->events.destroy, &inh->destroy);
	wl_list_insert(&server->inhibitors, &inh->link);

	/* 协议里没有「拒绝」的回应：不给客户端发 active 就是拒绝。vidow 一律给
	 * （要拒绝得给用户一个开关，现在没有这个需求）。 */
	wlr_keyboard_shortcuts_inhibitor_v1_activate(inhibitor);

	struct vidow_view *view = vidow_view_from_surface(server, inhibitor->surface);
	wlr_log(WLR_INFO, "快捷键抑制: %s 要走了键盘快捷键（抑制只在它拿着焦点时生效）",
		view != NULL && vidow_view_title(view) != NULL ?
			vidow_view_title(view) : "(表面)");
}

/* manager 没了（display 销毁）：把监听摘干净 —— wlroots 的 handle_display_destroy
 * 会断言 events.new_inhibitor 上没人挂着（指针约束那边踩过同一个坑）。 */
static void handle_inhibit_manager_destroy(struct wl_listener *listener,
		void *data) {
	(void)data;
	struct vidow_server *server =
		wl_container_of(listener, server, inhibit_destroy);
	wl_list_remove(&server->new_inhibitor.link);
	wl_list_remove(&server->inhibit_destroy.link);
	server->shortcuts_inhibit = NULL;
}

/* 现在键盘焦点那个表面有没有一个**生效中**的抑制。keyboard_key 用它决定要不要
 * 跳过 vidow 自己的键位（见 input.c）。 */
bool vidow_shortcuts_inhibited(struct vidow_server *server) {
	struct vidow_shortcuts_inhibitor *inh;
	wl_list_for_each(inh, &server->inhibitors, link) {
		struct wlr_keyboard_shortcuts_inhibitor_v1 *i = inh->inhibitor;
		if (i == NULL || !i->active || i->surface == NULL ||
				i->seat != server->seat) {
			continue;
		}
		if (i->surface == server->seat->keyboard_state.focused_surface) {
			return true;
		}
	}
	return false;
}

/* ------------------------------------------------------- X11 键盘抓取 */

static void grab_surface_destroy(struct wl_listener *listener, void *data);
static void grab_resource_destroy(struct wl_resource *resource);

/* 现在生效的那个 grab：列表头上那个（`wl_list_insert` 往头上插，所以头上是**最近**
 * 建的那个 —— Xwayland 可能同时抓着好几个对象，和 X 那边「最后一次 XGrabKeyboard
 * 赢」一致）。 */
static struct vidow_xwayland_grab *current_grab(struct vidow_server *server) {
	if (wl_list_empty(&server->xwayland_grabs)) {
		return NULL;
	}
	struct vidow_xwayland_grab *g =
		wl_container_of(server->xwayland_grabs.next, g, link);
	return g->surface != NULL ? g : NULL;
}

/* 把座位键盘焦点按到抓着键盘的那个表面上（如果还没在它身上）。 */
void vidow_xwayland_grab_assert(struct vidow_server *server) {
	struct vidow_xwayland_grab *g = current_grab(server);
	if (g == NULL) {
		return;
	}
	struct wlr_seat *seat = server->seat;
	if (seat->keyboard_state.focused_surface == g->surface) {
		return;
	}
	/* 独占层拿着键盘时不抢（和 vidow_focus_view / 弹出物那两处一个规矩）。 */
	if (server->keyboard_layer != NULL) {
		return;
	}
	/* g->surface 理论上还在（表面死了会自己从名单上摘掉），但这个壳兜底：
	 * 真要是 NULL，wlroots 那边是 assert，宁可清焦点也不能把合成器 abort 掉。 */
	vidow_seat_focus_surface(seat, g->surface);
}

/* grab 结束了（客户端销毁对象 / 连接断了 / 抓着键盘的表面死了）：把焦点还给
 * 焦点窗口。if_focused 是「刚没的那个表面」，焦点不在它身上就什么都不用做。
 * 调之前**先从名单上摘掉自己**，不然「还有没有别的 grab」会把自己算进去。 */
static void grab_restore_focus(struct vidow_server *server,
		struct wlr_surface *if_focused) {
	struct wlr_seat *seat = server->seat;
	if (if_focused == NULL || seat->keyboard_state.focused_surface != if_focused) {
		return; /* 焦点已经不在它身上了（比如紧接着又抓了一个） */
	}
	if (current_grab(server) != NULL) {
		vidow_xwayland_grab_assert(server); /* 还有别的 grab 接着要键 */
		return;
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

/* 从名单上摘掉（列表 + 表面监听），可以重复调：表面先死那条路会先摘一次，
 * 之后 wl_resource_destroy 又走到 grab_resource_destroy，别摘第二次（对已经
 * 摘下来的节点再 wl_list_remove 会把邻居接错，列表就断了）。 */
static void grab_detach(struct vidow_xwayland_grab *g) {
	if (g->linked) {
		wl_list_remove(&g->link);
		g->linked = false;
	}
	if (g->surface != NULL) {
		wl_list_remove(&g->surface_destroy.link);
	}
}

static void grab_surface_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_xwayland_grab *g =
		wl_container_of(listener, g, surface_destroy);
	/* 表面死了：Xwayland 那边接下来也会销毁对象，但别等它 —— 先从名单上
	 * 摘掉（之后的 assert 就当没有这个 grab），焦点还挂在它身上就还给焦点
	 * 窗口（不能对着一个正在销毁的表面 notify_enter）。 */
	struct wlr_surface *dead = g->surface;
	g->surface = NULL;
	grab_detach(g);
	grab_restore_focus(g->server, dead);
	if (g->resource != NULL) {
		wl_resource_destroy(g->resource); /* 析构里 free(g) */
	} else {
		free(g);
	}
}

static void grab_resource_destroy(struct wl_resource *resource) {
	struct vidow_xwayland_grab *g = wl_resource_get_user_data(resource);
	if (g == NULL) {
		return;
	}
	wl_resource_set_user_data(resource, NULL);
	g->resource = NULL;
	/* 先从名单上摘掉，再算「还有没有别的 grab 要键盘」—— 不然算上自己
	 * 那个刚没的，焦点就还留在它身上，还不到焦点窗口手里。 */
	grab_detach(g);
	grab_restore_focus(g->server, g->surface);
	free(g);
}

static void grab_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zwp_xwayland_keyboard_grab_v1_interface grab_impl = {
	.destroy = grab_handle_destroy,
};

static void manager_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	(void)client;
	wl_resource_destroy(resource);
}

static void manager_handle_grab_keyboard(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		struct wl_resource *surface_resource,
		struct wl_resource *seat_resource) {
	struct vidow_server *server = wl_resource_get_user_data(resource);
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	struct wlr_seat_client *seat_client =
		wlr_seat_client_from_resource(seat_resource);

	/* 协议要求「只给 Xwayland」：表面必须是**这个客户端自己的** X11 窗口表面。
	 * 不是就回错误、不建对象（别的客户端想用这条路抓键盘，得先过这一关）。 */
	if (surface == NULL ||
			wlr_xwayland_surface_try_from_wlr_surface(surface) == NULL ||
			wl_resource_get_client(surface_resource) != client) {
		wlr_log(WLR_ERROR, "X11 键盘抓取: 拒绝（这个表面不是 Xwayland 的）");
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_METHOD,
			"zwp_xwayland_keyboard_grab_manager_v1 只服务 Xwayland");
		return;
	}
	if (seat_client == NULL || seat_client->seat != server->seat) {
		wlr_log(WLR_ERROR, "X11 键盘抓取: 拒绝（seat 不是我们那个）");
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_METHOD,
			"这个 seat 不是 vidow 的");
		return;
	}

	struct vidow_xwayland_grab *g = calloc(1, sizeof(*g));
	if (g == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	g->server = server;
	g->surface = surface;
	g->resource = wl_resource_create(client,
		&zwp_xwayland_keyboard_grab_v1_interface, 1, id);
	if (g->resource == NULL) {
		free(g);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(g->resource, &grab_impl, g,
		grab_resource_destroy);
	g->surface_destroy.notify = grab_surface_destroy;
	wl_signal_add(&surface->events.destroy, &g->surface_destroy);
	wl_list_insert(&server->xwayland_grabs, &g->link);
	g->linked = true;

	struct vidow_view *view = vidow_view_from_surface(server, surface);
	wlr_log(WLR_INFO, "X11 键盘抓取: %s 抓走了键盘（XGrabKeyboard）",
		view != NULL && vidow_view_title(view) != NULL ?
			vidow_view_title(view) : "(X11 表面)");

	/* X11 的语义：抓着键盘的那个客户端收全部按键。焦点按过去，之后每次按键
	 * 还会再确认（vidow_xwayland_grab_assert）。 */
	vidow_xwayland_grab_assert(server);
}

static const struct zwp_xwayland_keyboard_grab_manager_v1_interface
manager_impl = {
	.destroy = manager_handle_destroy,
	.grab_keyboard = manager_handle_grab_keyboard,
};

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct vidow_server *server = data;
	struct wl_resource *resource = wl_resource_create(client,
		&zwp_xwayland_keyboard_grab_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, server, NULL);
}

/* ------------------------------------------------------------- IPC */

/* 键盘状态（抑制 / X11 抓取）写进当前 JSON 对象。 */
void vidow_keyboard_grab_json(struct jbuf *b, struct vidow_server *server) {
	jb_kv_bool(b, "shortcuts_inhibited", vidow_shortcuts_inhibited(server));
	jb_key(b, "keyboard_grab");
	struct vidow_xwayland_grab *g = current_grab(server);
	if (g == NULL) {
		jb_null(b);
		return;
	}
	struct vidow_view *view = vidow_view_from_surface(server, g->surface);
	jb_obj(b);
	jb_kv_str(b, "type", "xwayland");
	jb_key(b, "window");
	if (view != NULL) {
		jb_uint(b, view->id);
	} else {
		jb_null(b);
	}
	jb_obj_end(b);
}

/* ------------------------------------------------------------- 起手 */

bool vidow_keyboard_grab_init(struct vidow_server *server) {
	wl_list_init(&server->inhibitors);
	wl_list_init(&server->xwayland_grabs);

	server->shortcuts_inhibit =
		wlr_keyboard_shortcuts_inhibit_v1_create(server->display);
	if (server->shortcuts_inhibit == NULL) {
		wlr_log(WLR_ERROR, "快捷键抑制协议"
			"（zwp_keyboard_shortcuts_inhibit_manager_v1）建不出来");
		return false;
	}
	server->new_inhibitor.notify = handle_new_inhibitor;
	wl_signal_add(&server->shortcuts_inhibit->events.new_inhibitor,
		&server->new_inhibitor);
	server->inhibit_destroy.notify = handle_inhibit_manager_destroy;
	wl_signal_add(&server->shortcuts_inhibit->events.destroy,
		&server->inhibit_destroy);

	/* X11 抓取：wlroots 没有，global 自己建（访问限制在 grab_keyboard 里）。 */
	server->xwayland_grab_global = wl_global_create(server->display,
		&zwp_xwayland_keyboard_grab_manager_v1_interface, 1, server,
		manager_bind);
	if (server->xwayland_grab_global == NULL) {
		wlr_log(WLR_ERROR, "X11 键盘抓取协议"
			"（zwp_xwayland_keyboard_grab_manager_v1）建不出来");
		return false;
	}

	wlr_log(WLR_INFO, "键盘协议: zwp_keyboard_shortcuts_inhibit_manager_v1 + "
		"zwp_xwayland_keyboard_grab_manager_v1 已就位");
	return true;
}
