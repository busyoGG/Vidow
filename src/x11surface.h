#ifndef VIDOW_X11SURFACE_H
#define VIDOW_X11SURFACE_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <xcb/composite.h>
#include <xcb/render.h>

struct wlr_surface;
struct wlr_xwayland_shell_v1;
struct vidow_view;

/* 一个 X11 顶层窗口。
 *
 * 这是我们自己维护的窗口对象，取代 wlroots 的 wlr_xwayland_surface。
 * 照着 Hyprland 的 src/xwayland/XSurface.hpp 建 —— 那边也是同样的字段
 * 集合，因为它就是 ICCCM / EWMH 要求的那一套，没有多余的。
 *
 * 为什么不再用 wlr_xwayland_surface：那个对象由 wlroots 的 XWM 创建和
 * 管理，窗口什么时候该显示、什么时候该把 pixmap 导出成 dmabuf，全在它
 * 手里。Steam 第二次开主窗口全黑就是栽在这上面 —— 我们能看到 X 侧有
 * 内容（import -window 抓到 99% 非黑），能拿到 3808x2050 的纹理，但
 * 纹理像素全是 0，而那段逻辑在 wlroots 内部，我们插不进去。
 *
 * 现在 xcb 连接、事件循环、窗口生命周期都归我们。wlr_xwayland 整个不
 * 用了，只用 wlr_xwayland_server（启动进程）和 wlr_xwayland_shell_v1
 * （把 wl_surface 和 serial 对上号）。 */
struct vidow_x11_surface {
	xcb_window_t window_id;

	/* 关联的 Wayland 表面。Xwayland 通过 xwayland_shell_v1 交上来，
	 * 配对靠 WL_SURFACE_SERIAL 这个 client message（见 xwm.c）。 */
	struct wlr_surface *surface;
	uint64_t serial; /* 0 = 还没收到 serial */
	/* 上一次**建过场景**的那块表面。和 surface 不同：surface 在 dissociate
	 * 时被清成 NULL，这个留着，用来判断「这次关联是不是换了块新表面」。
	 *
	 * **场景树是按哪一块表面建的**（记它的 serial）。
	 *
	 * Steam 关掉主窗口再从托盘打开，会复用同一个 X 窗口、并换一块新的
	 * wl_surface。实测：
	 *   00:01:08.650  associate xid=0x2000041 serial=2 surface=0x55b37c4fba80
	 *   00:07:17.536  associate xid=0x2000041 serial=6 surface=0x55b37c4fba80
	 * 注意**表面地址一模一样**（新表面分在旧表面释放的那块内存上），所以
	 * 拿指针比是错的 —— 我第一版就是指针比，判据一次都没触发，黑屏照旧。
	 * serial 每次关联都会涨，是唯一可靠的判据。
	 *
	 * 0 = 还没建过场景。 */
	uint64_t scene_serial;

	/* 几何（X 像素）。来自 ConfigureNotify，也是我们发给客户端的值。 */
	int x, y;
	int width, height;

	bool override_redirect;
	bool mapped;
	bool withdrawn; /* ICCCM：窗口已撤下，等下一次 map */
	bool fullscreen;
	bool maximized_vert, maximized_horz;
	bool minimized;
	bool modal;
	/* _NET_WM_PID。规则匹配（按进程名 / pid）要用。 */
	uint32_t pid;

	/* 窗口属性。都是 readWindowData() 从 X 读回来的。 */
	char *title;    /* _NET_WM_NAME 优先，退回 WM_NAME */
	char *class;    /* WM_CLASS 第二段 */
	char *instance; /* WM_CLASS 第一段 */
	char *role;     /* WM_WINDOW_ROLE */

	/* _NET_WM_WINDOW_TYPE 里的原子。工具条 / 菜单 / 通知这类不该被当成
	 * 普通窗口管理。 */
	xcb_atom_t *window_types;
	size_t window_type_count;

	/* _NET_WM_STATE 里的原子。 */
	xcb_atom_t *state_atoms;
	size_t state_count;
	/* 我们自己最后一次 publish_state() 写下去的那份 _NET_WM_STATE（排序后的
	 * 指纹）。X 会把自己写的属性再以 PropertyNotify 回发一次，那条通知进来
	 * 又要读属性、又要重判全屏/最大化/最小化 —— 白跑一趟 X 往返，还可能引起
	 * 重排。指纹一样就认作「这是我们自己写的回显」，直接跳过。
	 *
	 * 只比指纹不设时间窗：属性是 X 串行投递的，回显一定在我们写之后到；
	 * 真客户端改了属性，指纹必然不同。 */
	uint64_t state_echo_hash;
	bool state_echo_valid;

	xcb_size_hints_t *size_hints; /* WM_NORMAL_HINTS */
	xcb_icccm_wm_hints_t *hints;  /* WM_HINTS */
	xcb_atom_t *protocols;        /* WM_PROTOCOLS */
	size_t protocol_count;

	/* _NET_WM_OPACITY（0..0xffffffff），0 = 没设。 */
	uint32_t opacity;

	struct vidow_x11_surface *parent;
	struct vidow_view *view; /* 反向指针，view 是我们在合成器侧的对象 */

	struct wl_listener surface_destroy;
	struct wl_listener surface_commit;

	struct wl_list link; /* vidow_x11_server.surfaces */
};

/* X11 服务端的全局状态（我们自己的 XWM）。 */
struct vidow_x11_server {
	struct vidow_server *owner; /* 反向指针，建 view 时要用 */
	struct wl_display *display;
	struct wl_event_loop *event_loop;

	xcb_connection_t *conn;
	xcb_screen_t *screen;
	int screen_num;

	/* xwayland_shell_v1：唯一还借 wlroots 的地方。只用来按 serial 把
	 * wl_surface 找回来，不参与窗口管理。 */
	struct wlr_xwayland_shell_v1 *shell;

	struct wl_event_source *event_source;

	/* 我们自己创建的辅助窗口：_NET_SUPPORTING_WM_CHECK 指向它。 */
	xcb_window_t wm_window;

	/* XSETTINGS（_XSETTINGS_S0）的属主窗口与上次发布的值。
	 *
	 * 为什么合成器自己发布：GTK3 的 X11 后端**不**从 Xft.dpi 推窗口倍率
	 * （只把它当字体 DPI），它听 XSETTINGS 的 Gdk/WindowScalingFactor。没有
	 * XSETTINGS 属主时 GTK 按 1 倍画，vidow 又按输出的 native:logical 除 2
	 * —— 窗口内容在屏上只剩一半（2x 屏上是四分之一面积）。KDE 的 kded6
	 * 会通过 DBus 激活 xsettingsd，用 kwinrc 里那份过期的 Scale=1 把
	 * Gdk/WindowScalingFactor 写成 1，正好把这半缩落实。见 xsettings_publish。
	 *
	 * xsettings_scale 是最近一次发布的倍率；被别的程序抢走选区
	 * （SelectionClear）时按它重新发一份，见 x11_handle_selection_clear。 */
	xcb_window_t xsettings_xid;
	int xsettings_scale;
	uint32_t xsettings_serial;

	/* 给 root 设的光标，以及做它用的 32 位 ARGB render format。 */
	xcb_cursor_t cursor_xid;
	xcb_render_pictformat_t render_format_id;

	/* 常用原子，启动时一次性 intern。 */
	struct {
		xcb_atom_t wm_protocols;
		xcb_atom_t wm_delete_window;
		xcb_atom_t wm_take_focus;
		xcb_atom_t wm_state;
		xcb_atom_t wm_name;
		xcb_atom_t wm_class;
		xcb_atom_t wm_normal_hints;
		xcb_atom_t wm_hints;
		xcb_atom_t wm_transient_for;
		xcb_atom_t wm_window_role;
		xcb_atom_t net_wm_name;
		xcb_atom_t net_wm_state;
		xcb_atom_t net_wm_state_fullscreen;
		xcb_atom_t net_wm_state_maximized_vert;
		xcb_atom_t net_wm_state_maximized_horz;
		xcb_atom_t net_wm_state_hidden;
		xcb_atom_t net_wm_state_modal;
		xcb_atom_t net_wm_state_focused;
		xcb_atom_t net_wm_window_type;
		xcb_atom_t net_wm_window_type_normal;
		xcb_atom_t net_wm_window_type_dialog;
		xcb_atom_t net_wm_window_type_menu;
		xcb_atom_t net_wm_window_type_tooltip;
		xcb_atom_t net_wm_window_type_popup;
		xcb_atom_t net_wm_opacity;
		xcb_atom_t net_wm_pid;
		xcb_atom_t net_active_window;
		xcb_atom_t net_client_list;
		xcb_atom_t net_client_list_stacking;
		xcb_atom_t net_supported;
		xcb_atom_t net_supporting_wm_check;
		xcb_atom_t net_workarea;
		xcb_atom_t net_wm_moveresize;
		xcb_atom_t net_wm_surface_serial;
		xcb_atom_t utf8_string;
		/* Xwayland 要等有人接管 WM_S0 才肯 listen -listenfd 的 socket。 */
		xcb_atom_t wm_s0;
		/* 声明「我是 X 合成器」。别的客户端（截图、录屏、DRI3 探测）靠它
		 * 判断有没有合成器在跑。 */
		xcb_atom_t net_wm_cm_s0;
		/* XSETTINGS 协议：选区 / 属性 / 通知消息。 */
		xcb_atom_t xsettings_s0;
		xcb_atom_t xsettings_settings;
		xcb_atom_t manager;
	} atoms;

	/* 所有活着的 X11 窗口。 */
	struct wl_list surfaces; /* vidow_x11_surface.link */

	/* Xwayland 通过 xwayland_shell_v1 交上来的、还没配上 X 窗口的表面。
	 * 配对按 serial 走：X 侧先发 WL_SURFACE_SERIAL client message，
	 * Wayland 侧再交 surface，两边对上了才算 associate。 */
	struct wl_list unpaired; /* vidow_x11_pending.link */

	struct wl_listener shell_new_surface;
	struct wl_listener display_destroy;

	/* 别的模块想在 X 事件流里插一脚（剪贴板桥就是靠这个拿选区事件）。
	 * 返回 true = 这条事件已经被吃掉，我们不再往下分发。
	 *
	 * 以前这是 wlr_xwayland 的 user_event_handler，现在窗口对象归我们自己，
	 * 钩子也就归我们自己 —— 语义一样，少绕一层。 */
	bool (*event_hook)(void *data, xcb_generic_event_t *event);
	void *event_hook_data;

	bool running;
};

struct vidow_x11_pending {
	uint64_t serial;
	struct wlr_surface *surface;
	struct wl_list link;
};

struct vidow_server;

/* 起停。x11_server_start() 在 Xwayland 进程就绪后调用，接管 wm_fd。 */
struct vidow_x11_server *vidow_x11_server_create(struct vidow_server *server,
	xcb_connection_t *conn);
void vidow_x11_server_destroy(struct vidow_x11_server *x11);

/* 给 X 窗口对象发 configure（ICCCM：尺寸没变时要补发合成事件）。 */
void vidow_x11_surface_configure(struct vidow_x11_surface *s,
	int16_t x, int16_t y, uint16_t w, uint16_t h);

/* 让客户端关自己（发 WM_DELETE_WINDOW）。 */
void vidow_x11_surface_close(struct vidow_x11_surface *s);

/* ---- 兼容层：语义等价于原来那几个 wlr_xwayland_* 查询 ----------------
 *
 * 窗口类型（_NET_WM_WINDOW_TYPE）在 EWMH 里是一组原子，判断「是不是菜单」
 * 得看集合里有没有那个成员。以前这是 wlroots 的函数，现在窗口对象归我们，
 * 自己实现一份，调用点不用改写法。
 *
 * 为什么不用 enum 而用原子本身：wlroots 的 enum 只是它内部原子表的下标，
 * 我们既然自己 intern 原子，直接用原子值比较更直接，也少一层映射。 */
/* 窗口类型用 wlroots 那套枚举名，值域一样、顺序一样 —— 这样 view.c 里
 * 那些 x11_float_types[] / x11_overlay_types[] 表不用改写法，只要底下的
 * 原子映射由我们提供。枚举值 → 原子 的翻译在 vidow_x11_surface_has_window_type
 * 里做。 */
enum vidow_x11_window_type {
	VIDOW_X11_WT_DESKTOP = 0,
	VIDOW_X11_WT_DOCK,
	VIDOW_X11_WT_TOOLBAR,
	VIDOW_X11_WT_MENU,
	VIDOW_X11_WT_UTILITY,
	VIDOW_X11_WT_SPLASH,
	VIDOW_X11_WT_DIALOG,
	VIDOW_X11_WT_DROPDOWN_MENU,
	VIDOW_X11_WT_POPUP_MENU,
	VIDOW_X11_WT_TOOLTIP,
	VIDOW_X11_WT_NOTIFICATION,
	VIDOW_X11_WT_COMBO,
	VIDOW_X11_WT_DND,
	VIDOW_X11_WT_NORMAL,
};

bool vidow_x11_surface_has_window_type(const struct vidow_x11_surface *s,
	int type);
/* 从一个 wl_surface 反查 X 窗口对象。
 *
 * 以前这是 wlr_xwayland_surface_try_from_wlr_surface()。现在窗口对象归我们，
 * 自己按 surface 指针在表里找。找不到返回 NULL（= 这个表面不是 X11 的）。
 *
 * 注意：这是唯一一处「必须能反查」的地方 —— 键盘抓取协议要求确认表面确实
 * 来自 Xwayland，view 查找也靠它分流。 */
struct vidow_x11_surface *vidow_x11_surface_from_wl_surface(
	struct wlr_surface *surface);

/* 枚举值 → 那个类型对应的 X 原子。没 intern 过的按需 intern 一次。 */
xcb_atom_t vidow_x11_window_type_atom(int type);

/* 窗口状态变更（EWMH）。以前这些都是 wlr_xwayland_surface_set_*，现在窗口
 * 对象归我们，语义照旧：改状态 + 把 _NET_WM_STATE 写回 X。 */
void vidow_x11_surface_activate(struct vidow_x11_surface *s, bool activated);
void vidow_x11_surface_set_fullscreen(struct vidow_x11_surface *s, bool on);
void vidow_x11_surface_set_maximized(struct vidow_x11_surface *s,
	bool horz, bool vert);
void vidow_x11_surface_set_minimized(struct vidow_x11_surface *s, bool on);
/* _NET_WM_PID，规则匹配要用。没有就返回 0。 */

/* 给 X 的根窗口设光标（RGBA 像素，stride 是每行字节数）。 */
void vidow_x11_set_cursor(const unsigned char *pixels, uint32_t stride,
	int width, int height, int hotspot_x, int hotspot_y);

/* 发布 XSETTINGS（Gdk/WindowScalingFactor = scale，其余设置照抄当前属主）。
 *
 * Xft.dpi 只覆盖「字体密度」和 Chromium/CEF；GTK3 的窗口倍率听 XSETTINGS。
 * 合成器既然定了 X 世界的密度（xwayland_scale），就得自己当这个属主，
 * 否则任何后起的 xsettingsd（KDE 的 kded6 就会）都能用一份过期配置把
 * 所有 X11 窗口缩成一半。 */
void vidow_x11_xsettings_update(struct vidow_x11_server *x11, int scale);

/* 当前那份 X 连接（剪贴板桥、缩放探测用）。没有 X 时返回 NULL。 */
xcb_connection_t *vidow_x11_connection(void);

/* 当前的 X11 服务端对象（view.c 里按属性原子分派时要查原子表）。 */
struct vidow_x11_server *vidow_x11_server_get(void);
/* 拥有这个 X11 服务端的 vidow_server（建 view 时要用）。 */
struct vidow_server *vidow_x11_server_owner(void);

/* 一个 configure 请求的字段副本。以前这个结构由 wlroots 的信号参数提供，
 * 现在窗口对象归我们，就自己定一份 —— 只放 view.c 真正读的那几个。 */
struct vidow_x11_configure_request {
	int x, y;
	int width, height;
	uint32_t mask;
};

/* X 窗口对象 ↔ 合成器侧 view。 */
void vidow_view_attach_x11_surface(struct vidow_view *view);

/* 读一块 X11 表面纹理的采样值，判断它是不是全空。诊断用。 */
void vidow_view_probe_x11_texture(struct vidow_view *view);
void vidow_view_x11_gone(struct vidow_view *view);
/* X11 窗口永久销毁（DestroyNotify）：摘链 + 释放 view 结构本身。 */
void vidow_view_x11_destroy(struct vidow_view *view);
void vidow_view_x11_configure_request(struct vidow_view *view,
	int x, int y, int w, int h);
void vidow_view_x11_geometry_changed(struct vidow_view *view,
	int x, int y, int w, int h);
void vidow_view_x11_property_changed(struct vidow_view *view, xcb_atom_t atom);
void vidow_view_x11_state_changed(struct vidow_view *view);
void vidow_view_x11_activate_request(struct vidow_view *view);

#endif /* VIDOW_X11SURFACE_H */
