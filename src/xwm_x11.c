/* 我们自己的 X11 窗口管理（XWM）。
 *
 * 这个文件取代 wlroots 的 xwayland/xwm.c。以前 wlr_xwayland_create() 会在
 * 内部建一个 wlr_xwm，X 连接、事件循环、窗口对象、什么时候把 pixmap 导出成
 * dmabuf，全在它手里 —— 我们只能在外面看它给出的结果。Steam 第二次开主窗口
 * 全黑就栽在这儿：X 侧明明有内容（import -window 抓到 99% 非黑），我们能拿到
 * 3808x2050 的纹理，纹理里却一个像素都没有，而决定这件事的代码在 wlroots
 * 内部，插不进去。
 *
 * 现在照 Hyprland 的 src/xwayland/ 做：
 *
 *   wlr_xwayland_server   只负责 fork/exec Xwayland，把 wm_fd 交给我们
 *   xcb_connect_to_fd     我们自己连上那个 fd，自己抢 SubstructureRedirect
 *   wl_event_loop_add_fd  我们自己读事件
 *   wlr_xwayland_shell_v1 只用来把 wl_surface 和 serial 对上号
 *
 * 和 Hyprland 的对应关系：
 *   CXWM::CXWM()              → xwm_create()
 *   CXWM::gatherResources()   → atoms_intern()
 *   CXWM::createWMWindow()    → wm_window_create()
 *   CXWM::onEvent()           → x11_dispatch()
 *   CXWM::handle*()           → x11_handle_*()
 *   CXWM::associate()         → x11_associate()
 *   CXWM::readWindowData()    → x11_read_window_data()
 *   CXWaylandSurface::configure() → vidow_x11_surface_configure()
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/log.h>
#include <wlr/xwayland/shell.h>

#include "server.h"
#include "x11surface.h"
#include "xwm.h"

static struct vidow_x11_server *g_x11;

/* ------------------------------------------------------------------ 原子 */

struct atom_spec {
	const char *name;
	size_t offset;
};

#define ATOM(field, name) { name, offsetof(struct vidow_x11_server, atoms.field) }

static const struct atom_spec atom_specs[] = {
	ATOM(wm_protocols, "WM_PROTOCOLS"),
	ATOM(wm_delete_window, "WM_DELETE_WINDOW"),
	ATOM(wm_take_focus, "WM_TAKE_FOCUS"),
	ATOM(wm_state, "WM_STATE"),
	ATOM(wm_name, "WM_NAME"),
	ATOM(wm_class, "WM_CLASS"),
	ATOM(wm_normal_hints, "WM_NORMAL_HINTS"),
	ATOM(wm_hints, "WM_HINTS"),
	ATOM(wm_transient_for, "WM_TRANSIENT_FOR"),
	ATOM(wm_window_role, "WM_WINDOW_ROLE"),
	ATOM(net_wm_name, "_NET_WM_NAME"),
	ATOM(net_wm_state, "_NET_WM_STATE"),
	ATOM(net_wm_state_fullscreen, "_NET_WM_STATE_FULLSCREEN"),
	ATOM(net_wm_state_maximized_vert, "_NET_WM_STATE_MAXIMIZED_VERT"),
	ATOM(net_wm_state_maximized_horz, "_NET_WM_STATE_MAXIMIZED_HORZ"),
	ATOM(net_wm_state_hidden, "_NET_WM_STATE_HIDDEN"),
	ATOM(net_wm_state_modal, "_NET_WM_STATE_MODAL"),
	ATOM(net_wm_state_focused, "_NET_WM_STATE_FOCUSED"),
	ATOM(net_wm_window_type, "_NET_WM_WINDOW_TYPE"),
	ATOM(net_wm_window_type_normal, "_NET_WM_WINDOW_TYPE_NORMAL"),
	ATOM(net_wm_window_type_dialog, "_NET_WM_WINDOW_TYPE_DIALOG"),
	ATOM(net_wm_window_type_menu, "_NET_WM_WINDOW_TYPE_MENU"),
	ATOM(net_wm_window_type_tooltip, "_NET_WM_WINDOW_TYPE_TOOLTIP"),
	ATOM(net_wm_window_type_popup, "_NET_WM_WINDOW_TYPE_POPUP_MENU"),
	ATOM(net_wm_opacity, "_NET_WM_WINDOW_OPACITY"),
	ATOM(net_wm_pid, "_NET_WM_PID"),
	ATOM(net_active_window, "_NET_ACTIVE_WINDOW"),
	ATOM(net_client_list, "_NET_CLIENT_LIST"),
	ATOM(net_client_list_stacking, "_NET_CLIENT_LIST_STACKING"),
	ATOM(net_supported, "_NET_SUPPORTED"),
	ATOM(net_supporting_wm_check, "_NET_SUPPORTING_WM_CHECK"),
	ATOM(net_workarea, "_NET_WORKAREA"),
	ATOM(net_wm_moveresize, "_NET_WM_MOVERESIZE"),
	ATOM(net_wm_surface_serial, "WL_SURFACE_SERIAL"),
	ATOM(utf8_string, "UTF8_STRING"),
	/* Xwayland 的开关：它要等有人接管 WM_S0 才把 -listenfd 的 socket 挂进
	 * 自己的事件循环（见 xserver hw/xwayland/xwayland.c 的
	 * wm_selection_callback -> listen_on_fds）。没人接管就永远不 listen，
	 * 所有 X 客户端卡死在 connect()。 */
	ATOM(wm_s0, "WM_S0"),
};
#undef ATOM

static void atoms_intern(struct vidow_x11_server *x11) {
	/* 和 x11_read_window_data() 同一个道理：**先全发，再全收**。
	 * 这里是 35 个 atom，按老写法就是 35 次同步往返，全在 XWM 建起来的
	 * 那一刻串行做完 —— 启动时白等一截。cookie 攒着一起收只要 1 次。 */
	enum { ATOM_COUNT = sizeof(atom_specs) / sizeof(atom_specs[0]) };
	xcb_intern_atom_cookie_t cookies[ATOM_COUNT];
	for (size_t i = 0; i < ATOM_COUNT; i++) {
		cookies[i] = xcb_intern_atom(x11->conn, 0,
			(uint16_t)strlen(atom_specs[i].name), atom_specs[i].name);
	}
	for (size_t i = 0; i < ATOM_COUNT; i++) {
		xcb_intern_atom_reply_t *reply =
			xcb_intern_atom_reply(x11->conn, cookies[i], NULL);
		if (reply == NULL) {
			wlr_log(WLR_ERROR, "xwm: intern %s 失败",
				atom_specs[i].name);
			continue;
		}
		*(xcb_atom_t *)(void *)((char *)x11 + atom_specs[i].offset) =
			reply->atom;
		free(reply);
	}
}

/* ------------------------------------------------------------------ 查找 */

static struct vidow_x11_surface *surface_for_window(xcb_window_t w) {
	if (g_x11 == NULL) {
		return NULL;
	}
	struct vidow_x11_surface *s;
	wl_list_for_each(s, &g_x11->surfaces, link) {
		if (s->window_id == w) {
			return s;
		}
	}
	return NULL;
}

/* ------------------------------------------------------------ 属性读取 */

static char *prop_to_string(xcb_get_property_reply_t *reply) {
	if (reply == NULL || xcb_get_property_value_length(reply) <= 0) {
		return NULL;
	}
	int len = xcb_get_property_value_length(reply);
	char *buf = malloc((size_t)len + 1);
	if (buf == NULL) {
		return NULL;
	}
	memcpy(buf, xcb_get_property_value(reply), (size_t)len);
	buf[len] = '\0';
	return buf;
}

static void set_string(char **dst, char *src) {
	if (src == NULL) {
		return;
	}
	free(*dst);
	*dst = src;
}

static void read_wm_class(struct vidow_x11_surface *s,
		xcb_get_property_reply_t *reply) {
	int len = xcb_get_property_value_length(reply);
	if (len <= 0) {
		return;
	}
	const char *v = xcb_get_property_value(reply);
	size_t ilen = strnlen(v, (size_t)len);
	if (ilen > 0) {
		char *inst = malloc(ilen + 1);
		if (inst != NULL) {
			memcpy(inst, v, ilen);
			inst[ilen] = '\0';
			set_string(&s->instance, inst);
		}
	}
	if (ilen + 1 < (size_t)len) {
		const char *cls = v + ilen + 1;
		size_t remain = (size_t)len - ilen - 1;
		size_t clen = strnlen(cls, remain);
		if (clen > 0) {
			char *c = malloc(clen + 1);
			if (c != NULL) {
				memcpy(c, cls, clen);
				c[clen] = '\0';
				set_string(&s->class, c);
			}
		}
	}
}

static void read_atom_list(xcb_atom_t **dst, size_t *count,
		xcb_get_property_reply_t *reply) {
	int n = xcb_get_property_value_length(reply) / (int)sizeof(xcb_atom_t);
	free(*dst);
	*dst = NULL;
	*count = 0;
	if (n <= 0) {
		return;
	}
	*dst = malloc(sizeof(xcb_atom_t) * (size_t)n);
	if (*dst == NULL) {
		return;
	}
	memcpy(*dst, xcb_get_property_value(reply),
		sizeof(xcb_atom_t) * (size_t)n);
	*count = (size_t)n;
}

static void sync_state_flags(struct vidow_x11_surface *s) {
	struct vidow_x11_server *x11 = g_x11;
	if (x11 == NULL) {
		return;
	}
	s->fullscreen = false;
	s->maximized_vert = false;
	s->maximized_horz = false;
	s->minimized = false;
	s->modal = false;
	for (size_t i = 0; i < s->state_count; i++) {
		xcb_atom_t a = s->state_atoms[i];
		if (a == x11->atoms.net_wm_state_fullscreen) {
			s->fullscreen = true;
		} else if (a == x11->atoms.net_wm_state_maximized_vert) {
			s->maximized_vert = true;
		} else if (a == x11->atoms.net_wm_state_maximized_horz) {
			s->maximized_horz = true;
		} else if (a == x11->atoms.net_wm_state_hidden) {
			s->minimized = true;
		} else if (a == x11->atoms.net_wm_state_modal) {
			s->modal = true;
		}
	}
}

static void x11_read_prop(struct vidow_x11_surface *s, xcb_atom_t atom,
		xcb_get_property_reply_t *reply) {
	struct vidow_x11_server *x11 = g_x11;
	if (x11 == NULL || reply == NULL) {
		return;
	}
	if (atom == x11->atoms.net_wm_name) {
		set_string(&s->title, prop_to_string(reply));
	} else if (atom == x11->atoms.wm_name) {
		if (s->title == NULL) { /* _NET_WM_NAME 优先 */
			set_string(&s->title, prop_to_string(reply));
		}
	} else if (atom == XCB_ATOM_WM_CLASS) {
		read_wm_class(s, reply);
	} else if (atom == x11->atoms.wm_window_role) {
		set_string(&s->role, prop_to_string(reply));
	} else if (atom == x11->atoms.wm_normal_hints) {
		int len = xcb_get_property_value_length(reply);
		if (len >= (int)sizeof(xcb_size_hints_t)) {
			if (s->size_hints == NULL) {
				s->size_hints = calloc(1, sizeof(*s->size_hints));
			}
			if (s->size_hints != NULL) {
				memcpy(s->size_hints, xcb_get_property_value(reply),
					sizeof(xcb_size_hints_t));
			}
		}
	} else if (atom == x11->atoms.wm_hints) {
		int len = xcb_get_property_value_length(reply);
		if (len >= (int)sizeof(xcb_icccm_wm_hints_t)) {
			if (s->hints == NULL) {
				s->hints = calloc(1, sizeof(*s->hints));
			}
			if (s->hints != NULL) {
				memcpy(s->hints, xcb_get_property_value(reply),
					sizeof(xcb_icccm_wm_hints_t));
			}
		}
	} else if (atom == x11->atoms.wm_protocols) {
		read_atom_list(&s->protocols, &s->protocol_count, reply);
	} else if (atom == x11->atoms.net_wm_window_type) {
		read_atom_list(&s->window_types, &s->window_type_count, reply);
	} else if (atom == x11->atoms.net_wm_state) {
		read_atom_list(&s->state_atoms, &s->state_count, reply);
		sync_state_flags(s);
	} else if (atom == x11->atoms.net_wm_opacity) {
		if (xcb_get_property_value_length(reply) >= 4) {
			memcpy(&s->opacity, xcb_get_property_value(reply), 4);
		}
	} else if (atom == x11->atoms.net_wm_pid) {
		if (xcb_get_property_value_length(reply) >= 4) {
			memcpy(&s->pid, xcb_get_property_value(reply), 4);
		}
	}
}

static void x11_read_window_data(struct vidow_x11_surface *s) {
	struct vidow_x11_server *x11 = g_x11;
	if (x11 == NULL) {
		return;
	}
	const xcb_atom_t props[] = {
		XCB_ATOM_WM_CLASS,
		XCB_ATOM_WM_NAME,
		XCB_ATOM_WM_TRANSIENT_FOR,
		x11->atoms.wm_hints,
		x11->atoms.net_wm_state,
		x11->atoms.net_wm_name,
		x11->atoms.net_wm_window_type,
		x11->atoms.wm_normal_hints,
		x11->atoms.wm_protocols,
		x11->atoms.wm_window_role,
		x11->atoms.net_wm_opacity,
		x11->atoms.net_wm_pid,
	};
	/* **先把 12 个请求全发出去，再一起收回复**。
	 *
	 * 以前是「发一个 → 等回复 → 发下一个」，每个属性一次同步往返。12 个
	 * 属性 = 12 次往返，而且全都在新窗口的 associate 路径上同步执行 ——
	 * 主循环停在那里等 X 服务器，用户看到的就是「打开新窗口时卡一下」。
	 * X 服务器慢的时候（远端、忙、Xwayland 正在初始化）会卡几百毫秒。
	 *
	 * xcb 的 cookie 本来就是为这个设计的：请求发出去不等，攒够了再收。
	 * 12 次往返压成 1 次 —— 只有第一次真的等，后面 11 个回复早就到了。 */
	enum { PROP_COUNT = sizeof(props) / sizeof(props[0]) };
	xcb_get_property_cookie_t cookies[PROP_COUNT];
	for (size_t i = 0; i < PROP_COUNT; i++) {
		cookies[i] = xcb_get_property(x11->conn, 0, s->window_id,
			props[i], XCB_ATOM_ANY, 0, 2048);
	}
	for (size_t i = 0; i < PROP_COUNT; i++) {
		xcb_get_property_reply_t *reply =
			xcb_get_property_reply(x11->conn, cookies[i], NULL);
		if (reply == NULL) {
			continue;
		}
		if (props[i] == XCB_ATOM_WM_TRANSIENT_FOR) {
			if (xcb_get_property_value_length(reply) >= 4) {
				xcb_window_t parent = XCB_WINDOW_NONE;
				memcpy(&parent, xcb_get_property_value(reply), 4);
				s->parent = surface_for_window(parent);
			}
		} else {
			x11_read_prop(s, props[i], reply);
		}
		free(reply);
	}
}

/* -------------------------------------------------------------- 生命周期 */

static void surface_free(struct vidow_x11_surface *s) {
	free(s->title);
	free(s->class);
	free(s->instance);
	free(s->role);
	free(s->window_types);
	free(s->state_atoms);
	free(s->size_hints);
	free(s->hints);
	free(s->protocols);
	free(s);
}

static void handle_surface_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_x11_surface *s =
		wl_container_of(listener, s, surface_destroy);
	wl_list_remove(&s->surface_destroy.link);
	wl_list_init(&s->surface_destroy.link);
	s->surface = NULL;
}

/* 表面交帧：这是「客户端真的画出东西了」的唯一证据。
 *
 * 规则和 Hyprland 的 XSurface.cpp:97 一样：**有 buffer 就该在屏幕上，
 * 没有就不该** —— 不看 X 的 map 状态，不看我们自己的标志位。 */
static void handle_surface_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_x11_surface *s =
		wl_container_of(listener, s, surface_commit);
	if (s->view != NULL) {
		xwm_surface_sync(s->view);
	}
}

static void x11_associate(struct vidow_x11_surface *s,
		struct wlr_surface *surface) {
	if (s->surface != NULL || surface == NULL) {
		return;
	}
	s->surface = surface;
	/* 卡顿探针（VIDOW_STUTTER=1）：新窗口的 associate 是同步跑的，主循环
	 * 停在这里等多久，用户就卡多久。读属性要问 X 服务器，慢的时候（远端
	 * X、Xwayland 正忙）几百毫秒都可能。 */
	struct timespec assoc_t0 = {0};
	bool assoc_probe = getenv("VIDOW_STUTTER") != NULL;
	if (assoc_probe) {
		clock_gettime(CLOCK_MONOTONIC, &assoc_t0);
	}
	x11_read_window_data(s);
	if (assoc_probe) {
		struct timespec assoc_t1 = {0};
		clock_gettime(CLOCK_MONOTONIC, &assoc_t1);
		int64_t dt_ns = (int64_t)(assoc_t1.tv_sec - assoc_t0.tv_sec) *
			1000000000LL + (assoc_t1.tv_nsec - assoc_t0.tv_nsec);
		if (dt_ns >= 20 * 1000000LL) {
			wlr_log(WLR_INFO, "卡顿: 新窗口读属性 xid=0x%x 花了 %.1fms",
				s->window_id, (double)dt_ns / 1e6);
		}
	}

	s->surface_destroy.notify = handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &s->surface_destroy);
	s->surface_commit.notify = handle_surface_commit;
	wl_signal_add(&surface->events.commit, &s->surface_commit);

	wlr_log(WLR_INFO, "xwm: associate xid=0x%x serial=%llu surface=%p",
		s->window_id, (unsigned long long)s->serial, (void *)surface);

	if (s->view != NULL) {
		vidow_view_attach_x11_surface(s->view);
		xwm_surface_sync(s->view);
	}
}

static void x11_dissociate(struct vidow_x11_surface *s) {
	if (s->surface == NULL) {
		return;
	}
	wl_list_remove(&s->surface_destroy.link);
	wl_list_remove(&s->surface_commit.link);
	wl_list_init(&s->surface_destroy.link);
	wl_list_init(&s->surface_commit.link);
	s->surface = NULL;
}

/* ------------------------------------------------------------ 事件处理 */

static void x11_handle_create(xcb_create_notify_event_t *e) {
	if (g_x11 == NULL || e->window == g_x11->wm_window) {
		return;
	}
	if (surface_for_window(e->window) != NULL) {
		return;
	}
	struct vidow_x11_surface *s = calloc(1, sizeof(*s));
	if (s == NULL) {
		return;
	}
	s->window_id = e->window;
	s->x = e->x;
	s->y = e->y;
	s->width = e->width;
	s->height = e->height;
	s->override_redirect = e->override_redirect;
	wl_list_init(&s->surface_destroy.link);
	wl_list_init(&s->surface_commit.link);
	wl_list_insert(&g_x11->surfaces, &s->link);

	wlr_log(WLR_INFO, "xwm: create xid=0x%x %dx%d at %d,%d or=%d",
		e->window, e->width, e->height, e->x, e->y,
		(int)e->override_redirect);

	vidow_view_new_x11(s);
}

static void x11_handle_destroy(xcb_destroy_notify_event_t *e) {
	struct vidow_x11_surface *s = surface_for_window(e->window);
	if (s == NULL) {
		return;
	}
	wlr_log(WLR_INFO, "xwm: destroy xid=0x%x", e->window);
	x11_dissociate(s);
	if (s->view != NULL) {
		/* **先把这个 view 的 x11 反向指针断掉，再释放 s。**
		 *
		 * 用户报的「vidow 崩溃了」（部署 2624757 之后立刻崩，coredump 是
		 * SIGSEGV，栈：#0 libc <- #1 libc <- #2 libc <- #3 libwlroots
		 * <- #4 _wlr_log <- #5 view_apply_fullscreen
		 * <- #6 view_sync_x11_fullscreen <- #7 vidow_views_sync_x11_fullscreen
		 * <- #8 output_frame）。
		 *
		 * vidow_views_sync_x11_fullscreen() 每帧遍历 server->views，对每个
		 * view 读 view->x11->fullscreen。而 vidow_view_x11_gone() 只做
		 * x11_drop_surface()：它释放场景/FX 状态，**既不移除 view、也不清
		 * view->x11**。真正清 view->x11 的地方只有 vidow_view_free()，而
		 * X 窗口销毁**不等于** view 销毁（view 还挂在布局里）。
		 *
		 * 于是 s 被 free 之后，view->x11 成了野指针，下一帧
		 * view_sync_x11_fullscreen() 一读就是 SIGSEGV —— 崩在 _wlr_log
		 * 里是因为拿到的 title 指针也是垃圾。
		 *
		 * X 窗口先死、view 后死这个顺序很常见：日志里 32x32 / 1x1 的
		 * helper 窗口（Qt、fcitx 那类）都是 create 完立刻 destroy。 */
		if (s->view->x11 == s) {
			s->view->x11 = NULL;
		}
		vidow_view_x11_gone(s->view);
		s->view = NULL;
	}
	wl_list_remove(&s->link);
	wl_list_init(&s->link);
	surface_free(s);
}

static void x11_handle_configure_request(xcb_configure_request_event_t *e) {
	struct vidow_x11_surface *s = surface_for_window(e->window);
	if (s == NULL) {
		return;
	}
	/* 客户端想改几何。照 Hyprland 的做法：记下来交给布局决定最终尺寸，
	 * 不直接照办 —— 我们才是 WM。 */
	if (e->value_mask & XCB_CONFIG_WINDOW_X) {
		s->x = e->x;
	}
	if (e->value_mask & XCB_CONFIG_WINDOW_Y) {
		s->y = e->y;
	}
	if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
		s->width = e->width;
	}
	if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
		s->height = e->height;
	}
	if (s->view != NULL) {
		vidow_view_x11_configure_request(s->view, s->x, s->y,
			s->width, s->height);
	}
}

static void x11_handle_configure_notify(xcb_configure_notify_event_t *e) {
	struct vidow_x11_surface *s = surface_for_window(e->window);
	if (s == NULL) {
		return;
	}
	if (s->x == e->x && s->y == e->y && s->width == e->width &&
			s->height == e->height) {
		return;
	}
	s->x = e->x;
	s->y = e->y;
	s->width = e->width;
	s->height = e->height;
	if (s->view != NULL) {
		vidow_view_x11_geometry_changed(s->view, s->x, s->y,
			s->width, s->height);
	}
}

static void x11_handle_map_request(xcb_map_request_event_t *e) {
	struct vidow_x11_surface *s = surface_for_window(e->window);
	if (s == NULL) {
		return;
	}
	/* 我们才是 WM：真正的 map 由我们发。 */
	xcb_map_window(g_x11->conn, e->window);
	s->withdrawn = false;
	if (s->view != NULL) {
		xwm_surface_sync(s->view);
	}
}

static void x11_handle_map_notify(xcb_map_notify_event_t *e) {
	struct vidow_x11_surface *s = surface_for_window(e->window);
	if (s == NULL) {
		return;
	}
	/* **这里必须同步一次状态机**。
	 *
	 * 以前这段在 wlroots 的 xwm_handle_map_notify() 里，它只清 withdrawn、
	 * 不碰 mapped —— 而 mapped 只在 handle_map_request 里设。Steam 重开
	 * 主窗口走的是 MapNotify（窗口是复用的：close-window 只发
	 * WM_DELETE_WINDOW，Steam 保留窗口），于是表面永远不被认为在屏幕上，
	 * Xwayland 也就不导出 dmabuf，我们拿到一张全零纹理。 */
	if (s->view != NULL) {
		xwm_surface_sync(s->view);
	}
}

static void x11_handle_unmap_notify(xcb_unmap_notify_event_t *e) {
	struct vidow_x11_surface *s = surface_for_window(e->window);
	if (s == NULL) {
		return;
	}
	if (s->view != NULL) {
		xwm_surface_sync(s->view);
	}
}

static void x11_handle_property_notify(xcb_property_notify_event_t *e) {
	struct vidow_x11_surface *s = surface_for_window(e->window);
	if (s == NULL) {
		return;
	}
	xcb_get_property_cookie_t cookie = xcb_get_property(g_x11->conn, 0,
		s->window_id, e->atom, XCB_ATOM_ANY, 0, 2048);
	xcb_get_property_reply_t *reply =
		xcb_get_property_reply(g_x11->conn, cookie, NULL);
	if (reply == NULL) {
		return;
	}
	if (e->atom == XCB_ATOM_WM_TRANSIENT_FOR) {
		if (xcb_get_property_value_length(reply) >= 4) {
			xcb_window_t parent = XCB_WINDOW_NONE;
			memcpy(&parent, xcb_get_property_value(reply), 4);
			s->parent = surface_for_window(parent);
		}
	} else if (e->atom == g_x11->atoms.net_wm_state && s->state_echo_valid) {
		/* **自己写的回显**：publish_state() 写 _NET_WM_STATE，X 会把这条
		 * 变更再以 PropertyNotify 发回给我们。指纹一样就说明是我们刚写的
		 * 那一份，读它、重判全屏/最大化/最小化全是白跑 —— 每次都多一趟 X
		 * 往返，状态没变也会走一遍重排。
		 *
		 * 真客户端改属性时指纹必然不同（原子集合变了），所以不会误吞。 */
		uint64_t h = 1469598103934665603ULL;
		int len = xcb_get_property_value_length(reply);
		int cnt = len / 4;
		const uint32_t *vals = xcb_get_property_value(reply);
		uint64_t acc = 0;
		for (int i = 0; i < cnt; i++) {
			acc ^= (uint64_t)vals[i];
		}
		h ^= acc;
		h *= 1099511628211ULL;
		if (h == s->state_echo_hash) {
			free(reply);
			return; /* 是我们自己写的：不重读、不重判 */
		}
		/* 不是回显 = 客户端真的改了状态：照常读进来（下面 free 之后统一
		 * 走 property_changed 那条分派）。 */
		x11_read_prop(s, e->atom, reply);
	} else {
		x11_read_prop(s, e->atom, reply);
	}
	free(reply);
	if (s->view != NULL) {
		vidow_view_x11_property_changed(s->view, e->atom);
	}
}

static void x11_handle_client_message(xcb_client_message_event_t *e) {
	struct vidow_x11_surface *s = surface_for_window(e->window);
	if (s == NULL) {
		return;
	}
	struct vidow_x11_server *x11 = g_x11;
	if (x11 == NULL) {
		return;
	}
	/* **serial 配对**：Xwayland 对每个窗口发一条 WL_SURFACE_SERIAL
	 * client message，data32[0..1] 是 64 位 serial。拿它去 xwayland_shell
	 * 里找对应的 wl_surface —— 找不到 surface，窗口就永远画不出来。 */
	if (e->type == x11->atoms.net_wm_surface_serial) {
		if (s->serial != 0) {
			return;
		}
		uint32_t lo = e->data.data32[0];
		uint32_t hi = e->data.data32[1];
		s->serial = ((uint64_t)hi << 32) | lo;
		if (x11->shell != NULL) {
			struct wlr_surface *surface =
				wlr_xwayland_shell_v1_surface_from_serial(
					x11->shell, s->serial);
			if (surface != NULL) {
				x11_associate(s, surface);
			}
		}
		return;
	}
	/* _NET_WM_STATE（EWMH）：data32[0] = 0 remove / 1 add / 2 toggle，
	 * data32[1..2] 是要改的原子。 */
	if (e->type == x11->atoms.net_wm_state) {
		uint32_t action = e->data.data32[0];
		for (int i = 0; i < 2; i++) {
			xcb_atom_t a = (xcb_atom_t)e->data.data32[1 + i];
			if (a == XCB_ATOM_NONE) {
				continue;
			}
			if (a == x11->atoms.net_wm_state_fullscreen) {
				s->fullscreen = action == 1 ? true :
					action == 0 ? false : !s->fullscreen;
			} else if (a == x11->atoms.net_wm_state_maximized_vert) {
				s->maximized_vert = action != 0;
			} else if (a == x11->atoms.net_wm_state_maximized_horz) {
				s->maximized_horz = action != 0;
			} else if (a == x11->atoms.net_wm_state_hidden) {
				s->minimized = action != 0;
			} else if (a == x11->atoms.net_wm_state_modal) {
				s->modal = action != 0;
			}
		}
		if (s->view != NULL) {
			vidow_view_x11_state_changed(s->view);
		}
		return;
	}
	if (e->type == x11->atoms.net_active_window ||
			e->type == x11->atoms.net_wm_moveresize) {
		if (s->view != NULL) {
			vidow_view_x11_activate_request(s->view);
		}
		return;
	}
}

static void x11_handle_error(xcb_generic_event_t *e) {
	xcb_value_error_t *err = (xcb_value_error_t *)e;
	wlr_log(WLR_DEBUG, "xwm: X 错误 code=%u major=%u minor=%u",
		err->error_code, err->major_opcode, err->minor_opcode);
}

/* 主事件循环。照 Hyprland 的 CXWM::onEvent()。 */
static int x11_dispatch(int fd, uint32_t mask, void *data) {
	(void)fd;
	(void)data;
	struct vidow_x11_server *x11 = g_x11;
	if (x11 == NULL) {
		return 0;
	}
	if ((mask & WL_EVENT_HANGUP) || (mask & WL_EVENT_ERROR)) {
		wlr_log(WLR_ERROR, "xwm: Xwayland 断开了");
		return 0;
	}
	int count = 0;
	xcb_generic_event_t *event;
	while ((event = xcb_poll_for_event(x11->conn)) != NULL) {
		count++;
		/* 先给挂上来的钩子（剪贴板桥）一个机会。它认的事件吃掉，剩下的
		 * 才轮到窗口管理 —— 和以前 wlroots 的 user_event_handler 同一个
		 * 约定。 */
		if (x11->event_hook != NULL &&
				x11->event_hook(x11->event_hook_data, event)) {
			free(event);
			continue;
		}
		switch (event->response_type & 0x7f) {
		case XCB_CREATE_NOTIFY:
			x11_handle_create((xcb_create_notify_event_t *)event);
			break;
		case XCB_DESTROY_NOTIFY:
			x11_handle_destroy((xcb_destroy_notify_event_t *)event);
			break;
		case XCB_CONFIGURE_REQUEST:
			x11_handle_configure_request(
				(xcb_configure_request_event_t *)event);
			break;
		case XCB_CONFIGURE_NOTIFY:
			x11_handle_configure_notify(
				(xcb_configure_notify_event_t *)event);
			break;
		case XCB_MAP_REQUEST:
			x11_handle_map_request((xcb_map_request_event_t *)event);
			break;
		case XCB_MAP_NOTIFY:
			x11_handle_map_notify((xcb_map_notify_event_t *)event);
			break;
		case XCB_UNMAP_NOTIFY:
			x11_handle_unmap_notify((xcb_unmap_notify_event_t *)event);
			break;
		case XCB_PROPERTY_NOTIFY:
			x11_handle_property_notify(
				(xcb_property_notify_event_t *)event);
			break;
		case XCB_CLIENT_MESSAGE:
			x11_handle_client_message(
				(xcb_client_message_event_t *)event);
			break;
		case 0:
			x11_handle_error(event);
			break;
		default:
			break;
		}
		free(event);
	}
	if (count > 0) {
		xcb_flush(x11->conn);
	}
	return count;
}

/* -------------------------------------------------------- WM 窗口与初始化 */

static void wm_window_create(struct vidow_x11_server *x11) {
	static const char wm_name[] = "vidow";
	x11->wm_window = xcb_generate_id(x11->conn);
	xcb_create_window(x11->conn, XCB_COPY_FROM_PARENT, x11->wm_window,
		x11->screen->root, 0, 0, 10, 10, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, x11->screen->root_visual, 0, NULL);
	xcb_change_property(x11->conn, XCB_PROP_MODE_REPLACE, x11->wm_window,
		x11->atoms.net_wm_name, x11->atoms.utf8_string, 8,
		(uint32_t)(sizeof(wm_name) - 1), wm_name);
	/* EWMH 要求 root 和 wm_window 两边都指向同一个窗口。 */
	xcb_change_property(x11->conn, XCB_PROP_MODE_REPLACE, x11->screen->root,
		x11->atoms.net_supporting_wm_check, XCB_ATOM_WINDOW, 32, 1,
		&x11->wm_window);
	xcb_change_property(x11->conn, XCB_PROP_MODE_REPLACE, x11->wm_window,
		x11->atoms.net_supporting_wm_check, XCB_ATOM_WINDOW, 32, 1,
		&x11->wm_window);

	/* **接管 WM_S0 —— 这是 Xwayland 肯 listen 的前提。**
	 *
	 * 用户报的「X 阻塞了，X 应用打不开，自启动流程也卡住」根因就在这里。
	 * Xwayland 拿到 -listenfd 之后**不立刻** listen，而是挂起等一个信号：
	 *     hw/xwayland/xwayland.c
	 *       wm_selection_callback()   ← 有人把 WM_S0 设为 owner
	 *         listen_on_fds()         ← 这时候才 ListenOnOpenFD(listen_fds[i])
	 * 没人接管 WM_S0，listen_on_fds() 就永不执行，两个 -listenfd 永远不
	 * 进 Xwayland 的 epoll，accept() 一次都不调。
	 *
	 * 实测症状（全部吻合）：Xwayland 的 epoll 里只有 Wayland 连接和 -wm
	 * 那个 fd，没有 listen fd；strace 下连接进了队列却零次 accept；
	 * /tmp/.X11-unix/X0 的 Recv-Q 一直积压 fcitx5 和 xrdb（backlog 只有 1，
	 * 见 wlroots sockets.c 的 listen(fd, 1)）；xdpyinfo 直接超时；
	 * X 客户端 ESTAB 恒为 0。之前误判成 wlroots 懒启动的锅，把 lazy 关掉
	 * （67c9ff7）也没用 —— 因为根本不在那条路上。
	 *
	 * Hyprland 同样是这一步：src/xwayland/XWM.cpp 的
	 *     xcb_set_selection_owner(conn, m_wmWindow, WM_S0, XCB_CURRENT_TIME) */
	xcb_set_selection_owner(x11->conn, x11->wm_window, x11->atoms.wm_s0,
		XCB_CURRENT_TIME);
}

static void supported_atoms_publish(struct vidow_x11_server *x11) {
	xcb_atom_t supported[] = {
		x11->atoms.net_wm_state,
		x11->atoms.net_active_window,
		x11->atoms.net_wm_moveresize,
		x11->atoms.net_wm_state_focused,
		x11->atoms.net_wm_state_modal,
		x11->atoms.net_wm_state_fullscreen,
		x11->atoms.net_wm_state_maximized_vert,
		x11->atoms.net_wm_state_maximized_horz,
		x11->atoms.net_wm_state_hidden,
		x11->atoms.net_client_list,
		x11->atoms.net_client_list_stacking,
		x11->atoms.net_workarea,
		x11->atoms.net_wm_window_type,
		x11->atoms.net_wm_window_type_normal,
		x11->atoms.net_wm_window_type_dialog,
		x11->atoms.net_wm_window_type_menu,
		x11->atoms.net_wm_window_type_tooltip,
		x11->atoms.net_wm_window_type_popup,
	};
	xcb_change_property(x11->conn, XCB_PROP_MODE_REPLACE, x11->screen->root,
		x11->atoms.net_supported, XCB_ATOM_ATOM, 32,
		(uint32_t)(sizeof(supported) / sizeof(supported[0])), supported);
}

static void handle_shell_new_surface(struct wl_listener *listener, void *data) {
	struct vidow_x11_server *x11 =
		wl_container_of(listener, x11, shell_new_surface);
	struct wlr_xwayland_surface_v1 *shell_surface = data;
	/* Wayland 侧先到：拿 serial 去 X 侧找已经收到 serial 的窗口。 */
	struct vidow_x11_surface *s;
	wl_list_for_each(s, &x11->surfaces, link) {
		if (s->surface == NULL && s->serial == shell_surface->serial) {
			x11_associate(s, shell_surface->surface);
			return;
		}
	}
	/* X 侧还没报 serial：先记下来，等 client message 到了再配。 */
	struct vidow_x11_pending *p = calloc(1, sizeof(*p));
	if (p == NULL) {
		return;
	}
	p->serial = shell_surface->serial;
	p->surface = shell_surface->surface;
	wl_list_insert(&x11->unpaired, &p->link);
	wlr_log(WLR_DEBUG, "xwm: shell surface serial=%llu 先挂着等 X 侧",
		(unsigned long long)p->serial);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_x11_server *x11 =
		wl_container_of(listener, x11, display_destroy);
	vidow_x11_server_destroy(x11);
}

struct vidow_x11_server *vidow_x11_server_create(struct vidow_server *server,
		xcb_connection_t *conn) {
	if (conn == NULL) {
		return NULL;
	}
	struct vidow_x11_server *x11 = calloc(1, sizeof(*x11));
	if (x11 == NULL) {
		return NULL;
	}
	x11->conn = conn;
	x11->owner = server;
	x11->display = server->display;
	x11->event_loop = wl_display_get_event_loop(server->display);
	wl_list_init(&x11->surfaces);
	wl_list_init(&x11->unpaired);
	wl_list_init(&x11->shell_new_surface.link);
	wl_list_init(&x11->display_destroy.link);

	xcb_screen_iterator_t it =
		xcb_setup_roots_iterator(xcb_get_setup(conn));
	x11->screen = it.data;
	if (x11->screen == NULL) {
		wlr_log(WLR_ERROR, "xwm: 拿不到 X screen");
		free(x11);
		return NULL;
	}

	g_x11 = x11;
	atoms_intern(x11);

	/* **抢 SubstructureRedirect**：这一步之后我们就是真正的 WM，
	 * 客户端窗口的 map / configure 请求都先到我们这儿。 */
	uint32_t values[] = {
		XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
			XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
			XCB_EVENT_MASK_PROPERTY_CHANGE,
	};
	xcb_change_window_attributes(conn, x11->screen->root,
		XCB_CW_EVENT_MASK, values);
	xcb_flush(conn);

	wm_window_create(x11);
	supported_atoms_publish(x11);

	/* Xwayland 通过 xwayland_shell_v1 把 wl_surface 交上来。 */
	if (server->xwayland_shell != NULL) {
		x11->shell = server->xwayland_shell;
		x11->shell_new_surface.notify = handle_shell_new_surface;
		wl_signal_add(&server->xwayland_shell->events.new_surface,
			&x11->shell_new_surface);
	}

	x11->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(server->display, &x11->display_destroy);

	x11->event_source = wl_event_loop_add_fd(x11->event_loop,
		xcb_get_file_descriptor(conn), WL_EVENT_READABLE, x11_dispatch,
		NULL);
	x11->running = true;

	wlr_log(WLR_INFO, "xwm: 自己的 XWM 起来了（root=0x%x，屏幕 %dx%d）",
		x11->screen->root, x11->screen->width_in_pixels,
		x11->screen->height_in_pixels);
	return x11;
}

void vidow_x11_server_destroy(struct vidow_x11_server *x11) {
	if (x11 == NULL) {
		return;
	}
	x11->running = false;
	if (x11->event_source != NULL) {
		wl_event_source_remove(x11->event_source);
		x11->event_source = NULL;
	}
	if (x11->shell_new_surface.link.next != NULL &&
			x11->shell_new_surface.link.next !=
			&x11->shell_new_surface.link) {
		wl_list_remove(&x11->shell_new_surface.link);
	}
	if (x11->display_destroy.link.next != NULL) {
		wl_list_remove(&x11->display_destroy.link);
	}
	struct vidow_x11_surface *s, *tmp;
	wl_list_for_each_safe(s, tmp, &x11->surfaces, link) {
		wl_list_remove(&s->link);
		surface_free(s);
	}
	struct vidow_x11_pending *p, *ptmp;
	wl_list_for_each_safe(p, ptmp, &x11->unpaired, link) {
		wl_list_remove(&p->link);
		free(p);
	}
	if (g_x11 == x11) {
		g_x11 = NULL;
	}
	free(x11);
}

/* ---------------------------------------------------------- 对外操作 */

void vidow_x11_surface_configure(struct vidow_x11_surface *s,
		int16_t x, int16_t y, uint16_t w, uint16_t h) {
	struct vidow_x11_server *x11 = g_x11;
	if (x11 == NULL || s == NULL) {
		return;
	}
	uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
		XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
	uint32_t values[] = { (uint32_t)x, (uint32_t)y, w, h };
	xcb_configure_window(x11->conn, s->window_id, mask, values);

	/* ICCCM：请求的尺寸和当前相同时，X 服务器不会发 ConfigureNotify。
	 * 客户端于是不知道自己被确认了，会一直等 —— 我们补一个合成事件。
	 * Hyprland 在 XSurface.cpp:206 做的同一件事。 */
	if (s->width == w && s->height == h) {
		xcb_configure_notify_event_t e;
		memset(&e, 0, sizeof(e));
		e.response_type = XCB_CONFIGURE_NOTIFY;
		e.event = s->window_id;
		e.window = s->window_id;
		e.x = x;
		e.y = y;
		e.width = w;
		e.height = h;
		e.border_width = 0;
		e.above_sibling = XCB_NONE;
		e.override_redirect = s->override_redirect;
		xcb_send_event(x11->conn, false, s->window_id,
			XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&e);
	}
	s->x = x;
	s->y = y;
	s->width = w;
	s->height = h;
	xcb_flush(x11->conn);
}

/* 枚举值 → X 原子。
 *
 * EWMH 里窗口类型就是一组原子，名字固定是 _NET_WM_WINDOW_TYPE_*。这里按
 * 枚举顺序建一张名字表，用到哪个才 intern 哪个（大部分程序只用 NORMAL
 * 和 DIALOG）。 */
static const char *const window_type_names[] = {
	[VIDOW_X11_WT_DESKTOP] = "_NET_WM_WINDOW_TYPE_DESKTOP",
	[VIDOW_X11_WT_DOCK] = "_NET_WM_WINDOW_TYPE_DOCK",
	[VIDOW_X11_WT_TOOLBAR] = "_NET_WM_WINDOW_TYPE_TOOLBAR",
	[VIDOW_X11_WT_MENU] = "_NET_WM_WINDOW_TYPE_MENU",
	[VIDOW_X11_WT_UTILITY] = "_NET_WM_WINDOW_TYPE_UTILITY",
	[VIDOW_X11_WT_SPLASH] = "_NET_WM_WINDOW_TYPE_SPLASH",
	[VIDOW_X11_WT_DIALOG] = "_NET_WM_WINDOW_TYPE_DIALOG",
	[VIDOW_X11_WT_DROPDOWN_MENU] = "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU",
	[VIDOW_X11_WT_POPUP_MENU] = "_NET_WM_WINDOW_TYPE_POPUP_MENU",
	[VIDOW_X11_WT_TOOLTIP] = "_NET_WM_WINDOW_TYPE_TOOLTIP",
	[VIDOW_X11_WT_NOTIFICATION] = "_NET_WM_WINDOW_TYPE_NOTIFICATION",
	[VIDOW_X11_WT_COMBO] = "_NET_WM_WINDOW_TYPE_COMBO",
	[VIDOW_X11_WT_DND] = "_NET_WM_WINDOW_TYPE_DND",
	[VIDOW_X11_WT_NORMAL] = "_NET_WM_WINDOW_TYPE_NORMAL",
};

xcb_atom_t vidow_x11_window_type_atom(int type) {
	if (type < 0 ||
			type >= (int)(sizeof(window_type_names) /
				sizeof(window_type_names[0]))) {
		return XCB_ATOM_NONE;
	}
	const char *name = window_type_names[type];
	if (name == NULL) {
		return XCB_ATOM_NONE;
	}
	/* 缓存：每个类型只 intern 一次。用静态数组，按枚举值索引。 */
	static xcb_atom_t cache[sizeof(window_type_names) /
		sizeof(window_type_names[0])];
	if (cache[type] != XCB_ATOM_NONE) {
		return cache[type];
	}
	if (g_x11 == NULL) {
		return XCB_ATOM_NONE;
	}
	xcb_intern_atom_cookie_t cookie = xcb_intern_atom(g_x11->conn, 0,
		(uint16_t)strlen(name), name);
	xcb_intern_atom_reply_t *reply =
		xcb_intern_atom_reply(g_x11->conn, cookie, NULL);
	if (reply == NULL) {
		return XCB_ATOM_NONE;
	}
	cache[type] = reply->atom;
	free(reply);
	return cache[type];
}

bool vidow_x11_surface_has_window_type(const struct vidow_x11_surface *s,
		int type) {
	if (s == NULL) {
		return false;
	}
	xcb_atom_t want = vidow_x11_window_type_atom(type);
	if (want == XCB_ATOM_NONE) {
		return false;
	}
	for (size_t i = 0; i < s->window_type_count; i++) {
		if (s->window_types[i] == want) {
			return true;
		}
	}
	return false;
}

/* 把 s 的布尔状态写成 _NET_WM_STATE。EWMH 要求 WM 在改变窗口状态后把属性
 * 写回，客户端（以及 xprop / wmctrl 这类工具）靠它判断自己现在是什么状态。 */
static void publish_state(struct vidow_x11_surface *s) {
	struct vidow_x11_server *x11 = g_x11;
	if (x11 == NULL) {
		return;
	}
	xcb_atom_t props[5];
	int n = 0;
	if (s->modal) {
		props[n++] = x11->atoms.net_wm_state_modal;
	}
	if (s->fullscreen) {
		props[n++] = x11->atoms.net_wm_state_fullscreen;
	}
	if (s->maximized_vert) {
		props[n++] = x11->atoms.net_wm_state_maximized_vert;
	}
	if (s->maximized_horz) {
		props[n++] = x11->atoms.net_wm_state_maximized_horz;
	}
	if (s->minimized) {
		props[n++] = x11->atoms.net_wm_state_hidden;
	}
	if (n == 0) {
		xcb_delete_property(x11->conn, s->window_id,
			x11->atoms.net_wm_state);
	} else {
		xcb_change_property(x11->conn, XCB_PROP_MODE_REPLACE,
			s->window_id, x11->atoms.net_wm_state, XCB_ATOM_ATOM, 32,
			(uint32_t)n, props);
	}
	/* 记下我们刚写下去的那份的指纹：X 会把这条属性变更以 PropertyNotify
	 * 回发给我们自己，读到同样的指纹就认作回显、跳过（见 x11surface.h 里
	 * state_echo_hash 的说明）。
	 *
	 * 指纹**与顺序无关**（全部异或再乘一次）：X 属性里的原子顺序不保证
	 * 跟写的顺序一样，按顺序算哈希会把真回显判成「不同」，过滤就失效。 */
	uint64_t h = 1469598103934665603ULL; /* FNV-1a 64 */
	for (int i = 0; i < n; i++) {
		h ^= (uint64_t)props[i];
	}
	h *= 1099511628211ULL;
	s->state_echo_hash = h;
	s->state_echo_valid = true;
	xcb_flush(x11->conn);
}

void vidow_x11_surface_activate(struct vidow_x11_surface *s, bool activated) {
	struct vidow_x11_server *x11 = g_x11;
	if (x11 == NULL || s == NULL) {
		return;
	}
	xcb_window_t w = activated ? s->window_id : XCB_WINDOW_NONE;
	xcb_change_property(x11->conn, XCB_PROP_MODE_REPLACE,
		x11->screen->root, x11->atoms.net_active_window,
		XCB_ATOM_WINDOW, 32, 1, &w);
	/* 焦点也真的给它：ICCCM 的输入焦点靠 SetInputFocus。 */
	if (activated) {
		xcb_set_input_focus(x11->conn, XCB_INPUT_FOCUS_POINTER_ROOT,
			s->window_id, XCB_CURRENT_TIME);
	}
	xcb_flush(x11->conn);
}

void vidow_x11_surface_set_fullscreen(struct vidow_x11_surface *s, bool on) {
	if (s == NULL || s->fullscreen == on) {
		return;
	}
	s->fullscreen = on;
	publish_state(s);
}

void vidow_x11_surface_set_maximized(struct vidow_x11_surface *s,
		bool horz, bool vert) {
	if (s == NULL) {
		return;
	}
	if (s->maximized_horz == horz && s->maximized_vert == vert) {
		return;
	}
	s->maximized_horz = horz;
	s->maximized_vert = vert;
	publish_state(s);
}

void vidow_x11_surface_set_minimized(struct vidow_x11_surface *s, bool on) {
	if (s == NULL || s->minimized == on) {
		return;
	}
	s->minimized = on;
	publish_state(s);
	/* ICCCM 的图标化：真要收起来得把窗口 unmap 掉，否则客户端以为
	 * 自己还看得见。 */
	if (on && g_x11 != NULL) {
		xcb_unmap_window(g_x11->conn, s->window_id);
		xcb_flush(g_x11->conn);
	}
}

uint32_t vidow_x11_surface_pid(const struct vidow_x11_surface *s) {
	return s != NULL ? s->pid : 0;
}


void vidow_x11_surface_close(struct vidow_x11_surface *s) {
	struct vidow_x11_server *x11 = g_x11;
	if (x11 == NULL || s == NULL) {
		return;
	}
	bool supports_delete = false;
	for (size_t i = 0; i < s->protocol_count; i++) {
		if (s->protocols[i] == x11->atoms.wm_delete_window) {
			supports_delete = true;
			break;
		}
	}
	if (supports_delete) {
		xcb_client_message_event_t e;
		memset(&e, 0, sizeof(e));
		e.response_type = XCB_CLIENT_MESSAGE;
		e.format = 32;
		e.window = s->window_id;
		e.type = x11->atoms.wm_protocols;
		e.data.data32[0] = x11->atoms.wm_delete_window;
		e.data.data32[1] = XCB_CURRENT_TIME;
		xcb_send_event(x11->conn, false, s->window_id,
			XCB_EVENT_MASK_NO_EVENT, (const char *)&e);
	} else {
		xcb_kill_client(x11->conn, s->window_id);
	}
	xcb_flush(x11->conn);
}

/* 给 X 的根窗口设光标。
 *
 * 为什么需要这个：X 客户端如果不自己设光标，就会继承 root window 的光标。
 * 不设的话 Xwayland 给的是 X 默认的十字，和桌面其余部分对不上。
 *
 * 照 Hyprland 的 CXWM::setCursor()（XWM.cpp:1389）写：开一个 32 位 pixmap，
 * 把 RGBA 像素塞进去，做成 Render 扩展的 cursor，最后设到 root 上。
 *
 * 以前这是 wlr_xwayland_set_cursor() 干的活 —— 它内部也是这么做的，只是
 * 顺带从 wlr_buffer 读像素。窗口管理既然归我们了，这一步也归我们。 */
void vidow_x11_set_cursor(const unsigned char *pixels, uint32_t stride,
		int width, int height, int hotspot_x, int hotspot_y) {
	struct vidow_x11_server *x11 = g_x11;
	if (x11 == NULL || pixels == NULL || width <= 0 || height <= 0) {
		return;
	}
	const int CURSOR_DEPTH = 32;
	xcb_pixmap_t pix = xcb_generate_id(x11->conn);
	xcb_create_pixmap(x11->conn, CURSOR_DEPTH, pix, x11->screen->root,
		(uint16_t)width, (uint16_t)height);

	xcb_render_picture_t pic = xcb_generate_id(x11->conn);
	xcb_render_create_picture(x11->conn, pic, pix, x11->render_format_id,
		0, NULL);

	xcb_gcontext_t gc = xcb_generate_id(x11->conn);
	xcb_create_gc(x11->conn, gc, pix, 0, NULL);
	xcb_put_image(x11->conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pix, gc,
		(uint16_t)width, (uint16_t)height, 0, 0, 0, CURSOR_DEPTH,
		(uint32_t)(stride * (uint32_t)height), pixels);
	xcb_free_gc(x11->conn, gc);

	if (x11->cursor_xid != XCB_NONE) {
		xcb_free_cursor(x11->conn, x11->cursor_xid);
	}
	x11->cursor_xid = xcb_generate_id(x11->conn);
	xcb_render_create_cursor(x11->conn, x11->cursor_xid, pic,
		(int16_t)hotspot_x, (int16_t)hotspot_y);
	xcb_free_pixmap(x11->conn, pix);
	xcb_render_free_picture(x11->conn, pic);

	uint32_t values[] = { x11->cursor_xid };
	xcb_change_window_attributes(x11->conn, x11->screen->root,
		XCB_CW_CURSOR, values);
	xcb_flush(x11->conn);
}

/* 找 root 上的 32 位 visual 对应的 render format —— 做光标要用。
 * Hyprland 的 CXWM::getRenderFormat()。 */
static void find_render_format(struct vidow_x11_server *x11) {
	xcb_render_query_pict_formats_reply_t *formats =
		xcb_render_query_pict_formats_reply(x11->conn,
			xcb_render_query_pict_formats(x11->conn), NULL);
	if (formats == NULL) {
		wlr_log(WLR_ERROR, "xwm: 拿不到 render formats，光标设不了");
		return;
	}
	xcb_render_pictforminfo_iterator_t it =
		xcb_render_query_pict_formats_formats_iterator(formats);
	for (; it.rem > 0; xcb_render_pictforminfo_next(&it)) {
		if (it.data->type != XCB_RENDER_PICT_TYPE_DIRECT ||
				it.data->depth != 32) {
			continue;
		}
		const xcb_render_directformat_t *d = &it.data->direct;
		if (d->alpha_mask == 0xff && d->red_mask == 0xff0000 &&
				d->green_mask == 0xff00 && d->blue_mask == 0xff) {
			x11->render_format_id = it.data->id;
			break;
		}
	}
	free(formats);
	if (x11->render_format_id == XCB_NONE) {
		wlr_log(WLR_ERROR, "xwm: 找不到 32 位 ARGB 的 render format");
	}
}

xcb_connection_t *vidow_x11_connection(void) {
	return g_x11 != NULL ? g_x11->conn : NULL;
}

struct vidow_x11_surface *vidow_x11_surface_from_wl_surface(
		struct wlr_surface *surface) {
	if (surface == NULL || g_x11 == NULL) {
		return NULL;
	}
	struct vidow_x11_surface *s;
	wl_list_for_each(s, &g_x11->surfaces, link) {
		if (s->surface == surface) {
			return s;
		}
	}
	return NULL;
}

struct vidow_x11_server *vidow_x11_server_get(void) {
	return g_x11;
}

struct vidow_server *vidow_x11_server_owner(void) {
	return g_x11 != NULL ? g_x11->owner : NULL;
}
