#ifndef VIDOW_SERVER_H
#define VIDOW_SERVER_H

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include "scale.h"
#include "hotplug.h"
#include "clipboard.h"
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v1.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland.h>

#define VIDOW_VIEW_TAG_MAGIC 0x50574d56u /* "PWMV" */

struct vidow_server;

/* 一个已经发出去的 xdg-activation 令牌（客户端拿到的那个随机串）。资源销毁了它
 * 还得活着：协议说「收到的令牌继续有效」。实现见 activation.c，IPC 要把它报出来
 * 所以结构体放在这儿（跟别的模块一样）。 */
struct vidow_activation_token {
	struct wl_list link; /* vidow_server.activation_tokens */
	char *name;          /* 给客户端的随机串 */
	struct wl_client *client;
	struct wl_listener client_destroy;

	struct wlr_surface *surface; /* set_surface 那个，兜底用（可空） */
	struct wl_listener surface_destroy;
	bool surface_listener_added;

	char *app_id;     /* set_app_id，只为日志 */
	uint32_t serial;  /* set_serial 给的 serial */
	bool has_serial;  /* 客户端到底给没给 set_serial */
	bool serial_ok;   /* 校验过了吗（没给 seat / seat 不认识 = 过） */
	struct timespec created;
};

#include "keybind.h"
#include "rule.h"
#include "workspace.h"

/* ------------------------------------------------------------------ config */

enum vidow_easing {
	VIDOW_EASE_LINEAR = 0,
	VIDOW_EASE_OUT_CUBIC,
	VIDOW_EASE_OUT_QUINT,
	VIDOW_EASE_IN_OUT_CUBIC,
	VIDOW_EASE_OUT_BACK,
};

struct vidow_config {
	/* ---- gap（照 niri 那套四个值 + `gaps <n>` 简写）----
	 *   屏幕边 → 窗口 = workspace_gaps_outer + window_gaps_outer
	 *   窗口 ↔ 窗口   = window_gaps_inner（列与列之间、同列上下之间）
	 * vidow 一块屏同时只显示一个工作区，所以 workspace_gaps_inner（niri 里是
	 * 「两块工作区之间」）目前无处可施，留着是为了配置能照抄 niri。
	 * 都按逻辑像素算；负值一律当 0（负 gap 会让工作区比屏幕还大）。 */
	int gaps;                 /* 简写：一次设满四个，只作记录 */
	int workspace_gaps_inner;
	int workspace_gaps_outer;
	int window_gaps_inner;
	int window_gaps_outer;
	/* 四边单独设「离屏幕边」的距离（i3-gaps 的 gaps top|bottom|left|right 那套）。
	 * -1 = 没设，跟着上面的统一值走；>= 0 就盖住那一边。
	 * 只对「屏幕边 → 窗口」有意义：窗口之间的间距本来就是横向（列间距）和纵向
	 * （同列上下）两个方向，没有第四边。 */
	int gaps_top;
	int gaps_bottom;
	int gaps_left;
	int gaps_right;
	/* Durations are wall-clock milliseconds. A slow compositor drops frames;
	 * it never stretches an animation. */
	/* 视口外强制渲染（窗口规则 / 录屏预览）每秒补多少帧，0 = 不限。
	 * 补帧会主动排下一帧，不节流的话在没 vsync 的后端上会空转到几千 fps。 */
	int force_render_hz;
	/* xdg-activation 的令牌多久作废（秒，0 = 永不作废）。wlroots 默认 30 秒，
	 * 而通知往往是「几十分钟前弹的、现在才点」—— 令牌早没了，客户端顶不上来
	 * （点了通知窗口不动）。所以默认不过期。 */
	int activation_token_timeout;
	/* serial 校验不通过的激活请求理不理（niri 配置 debug 段里那个同名选项）。
	 * 默认 false = 不理（等于 wlroots 一直以来的行为）：令牌照发，但后面用它
	 * 激活时被忽略。有些应用拿不到 / 传不对 serial（工具包写 0），打开这个它们
	 * 才顶得上来。见 activation.c。 */
	bool honor_xdg_activation_with_invalid_serial;
	int window_open_ms;
	int window_close_ms;
	int window_move_ms;
	int layer_open_ms;
	int layer_close_ms;
	enum vidow_easing easing;

	double open_alpha_from; /* opacity at t=0 while opening */
	int open_slide_px;      /* slide-in distance while opening */
	double close_alpha_to;  /* opacity at t=1 while closing */
	int close_slide_px;     /* slide-out distance while closing */

	/* server-side decorations */
	bool decor_enabled;
	double corner_radius;
	double border_width;
	float border_color[4];       /* straight (non-premultiplied) rgba */
	float border_color_focus[4];
	double shadow_radius;
	float shadow_color[4];
	int shadow_offset_x, shadow_offset_y;
	double window_opacity;
	double noise;

	/* kawase blur behind windows */
	bool blur_enabled;
	bool blur_all;        /* also blur behind tiled windows */
	int blur_levels;      /* 2..4 downsample steps */
	double blur_strength; /* 0 = sharp, 1 = fully blurred */
	double blur_alpha_min; /* cull: only blur backdrop pixels at least this opaque */
	double blur_offset;    /* kawase tap scale */

	bool shader_enabled;
	char shader_decor[64]; /* "" = built-in decoration shader */
	char shader_open[64]; /* "" = built-in fade/slide only */
	char shader_close[64];
	char shader_idle[64]; /* applied permanently while the window lives */
	char shader_dir[PATH_MAX];
	bool shader_flip_y;   /* sample the source texture bottom-up */

	/* 缩放（见 scale.c）。scale 是所有输出的默认策略："auto" = 按 EDID 的 ppi
	 * 推，让同一个逻辑尺寸在两块不同 DPI 的屏上是同一个物理尺寸；outputs 按
	 * 输出名覆盖，写法 "DP-1:2 HDMI-A-1:1.25"。 */
	char scale[16];
	char outputs[256];

	/* XWayland。xwayland=false 干脆不起 X 服务；xwayland_scale="auto" = 取所有
	 * 输出缩放的最大值（X 只有一套坐标，只能按最密的那块屏来）；"off" = 1:1。
	 * xwayland_env = 给 X11 客户端设 GDK_SCALE / QT_SCALE_FACTOR / Xft.dpi。 */
	bool xwayland;
	char xwayland_scale[16];
	bool xwayland_env;

	/* IPC（见 ipc.c）：unix socket 上的 JSON 请求/应答 + 事件订阅。
	 * ipc = false 干脆不建 socket；ipc_socket 留空 = 按 WAYLAND_DISPLAY 自动取
	 * $XDG_RUNTIME_DIR/vidow-<display>.sock。 */
	bool ipc;
	char ipc_socket[PATH_MAX];

	/* 剪贴板（见 clipboard.c）：X11(Xwayland) 与 Wayland 两边共用一份内容。
	 * clipboard = false 就不接管，退回 wlroots 自带那套（只在有 X11 窗口聚焦
	 * 时才同步）；clipboard_primary 决定选区（PRIMARY）要不要一起同步；
	 * clipboard_max_bytes 是每个 mime 类型读进内存的上限 —— 「源程序退出后还能
	 * 粘贴」靠的就是这份内存副本。 */
	bool clipboard;
	bool clipboard_primary;
	int clipboard_max_bytes;

	/* xdg-desktop-portal 后端选择（见 portalconf.c）。portal_desktop 是 vidow
	 * 设给自己的 XDG_CURRENT_DESKTOP，也是 <name>-portals.conf 的那个 name；
	 * 留空 = 完全不碰 portal 那一摊。 */
	bool portal_conf_sync;
	char portal_desktop[32];
};

/* --------------------------------------------------------------- animation */

/* One running animation. Progress comes from CLOCK_MONOTONIC. */
struct vidow_anim {
	struct wl_list link; /* vidow_server.anims */
	struct vidow_server *server;
	int64_t start_ns;
	int64_t duration_ns;
	const char *tag; /* for logs */
	enum vidow_easing easing;
	void (*update)(void *data, double t);
	void (*done)(void *data, bool cancelled);
	void *data;
	int ticks;      /* update() calls so far */
	bool running;   /* update() is on the stack */
	bool cancelled; /* cancel requested from inside update() */
	bool finished;
};

/* ---------------------------------------------------------------- shaders */

enum vidow_shader_phase {
	VIDOW_SHADER_NONE = 0,
	VIDOW_SHADER_OPEN,
	VIDOW_SHADER_CLOSE,
	VIDOW_SHADER_IDLE,
};

/* A compiled fragment shader, loaded from <shader_dir>/<name>.frag. */
struct vidow_shader {
	struct wl_list link; /* vidow_server.shaders */
	char name[64];
	unsigned int program; /* GLuint */
	int u_texture, u_progress, u_phase, u_size, u_origin, u_output_size;
	int u_alpha, u_time, u_flip_y;
};

/* A tiny GLSL program plus its uniform locations. */
struct vidow_gl_prog {
	unsigned int program;
	int u_tex, u_add, u_texel, u_offset, u_alpha_min;
};

/* The built-in decoration program: rounded corners, border, drop shadow,
 * blurred backdrop and grain. */
struct vidow_decor_prog {
	unsigned int program;
	int u_texture, u_blur, u_size, u_target_size, u_pad;
	int u_radius, u_border_width, u_border_color, u_shadow_color;
	int u_shadow_offset, u_shadow_radius, u_opacity, u_noise, u_alpha;
	int u_origin, u_output_size, u_flip_y;
	int u_blur_alpha_min, u_blur_strength;
	int u_blur_region, u_blur_region_on;
};

/* One kawase pyramid. The compositor keeps a shared one for the wallpaper
 * (captured with every toplevel hidden) plus a private one per window that
 * actually overlaps another window, because that window's backdrop has to
 * keep its neighbour in it. */
struct vidow_blur_set {
	struct wlr_buffer *down[4];
	struct wlr_buffer *up[4];
	int w, h, levels;
	uint64_t gen;  /* scene generation this pyramid was built for */
	double offset; /* kawase 采样步长，建这张金字塔时用的 */
	double alpha_min;
	bool valid;
};

/* niri 风格的 overview：把整条列带缩到一块屏里看全景，点谁就去谁。
 *
 * 场景是全局的（一个窗口的位置就是「世界坐标」），所以 overview 不动布局、不改
 * 客户端尺寸，只给每个窗口算一个「世界 -> 屏幕」的变换：
 *
 *     screen = out_center + (world - cam) * scale
 *
 * t = 0（正常）：scale = 1、cam = 那块屏的中心 —— 正好是恒等变换，进出都不突变；
 * t = 1（展开）：scale 刚好把整条列带铺进工作区，cam = 列带中心（+ 滚轮平移）。
 * 每帧算一次，所有窗口共用（见 view.c 的 vidow_view_apply_anim）。 */
struct vidow_overview {
	bool on;                 /* 目标状态（动画朝它走） */
	double t;                /* 0..1，当前进度 */
	double scale;            /* 由 t 算出的缩放 */
	double cam_x, cam_y;     /* 由 t 算出的相机中心（世界坐标） */
	double pan;              /* 滚轮平移（世界坐标），换列时清零 */
	bool swallow_release;    /* 点选退出后，把它对应的那次松手也吃掉 */
	double out_cx, out_cy;   /* 展开时铺在哪块屏的中心 */
	int out_x, out_y, out_w, out_h; /* 那块屏的布局位置（遮罩用） */
	struct wlr_output *output;      /* 进入时锁定的那块屏 */
	struct wlr_scene_rect *dim;     /* 压暗壁纸的遮罩 */
	struct vidow_anim *anim;
};

/* A window's content rendered through the decoration (and optionally an
 * animation shader) into offscreen buffers, then shown as a scene node.
 * Buffers are ping-ponged so we never draw into one the scene still holds. */
struct vidow_fx_target {
	struct wlr_scene_buffer *node;
	/* decoration pass */
	struct wlr_buffer *buf[2];
	int buf_index;
	int w, h;                 /* 渲染目标（物理像素），内容加阴影内边距 */
	int content_w, content_h; /* 内容，物理像素 */
	int dst_w, dst_h;         /* 场景节点尺寸（逻辑单位）= w/h 除以 scale */
	int px_pad;               /* 内边距，物理像素 */
	double scale;             /* 渲染倍率：窗口所在输出的缩放 */
	/* animation pass, only used when a shader is configured */
	struct wlr_buffer *anim_buf[2];
	int anim_index;
	struct vidow_shader *shader;
	enum vidow_shader_phase phase;
	double progress;
	int64_t start_ns;

	bool active;  /* decoration or shader is in use */
	bool blur;    /* sample the blurred backdrop */

	/* Blur backdrop. Either the server's shared wallpaper pyramid (borrowed)
	 * or a private one captured with this window taken out (owned). The
	 * texture only changes when the backdrop really changed, so a still
	 * desktop costs no blur and no redraw at all. */
	struct wlr_texture *blur_tex;     /* may be NULL */
	bool blur_owned;                  /* we must destroy blur_tex */
	uint64_t blur_tex_id;             /* identity of blur_tex, 0 = none */
	uint64_t blur_ver;                /* bumped whenever blur_tex changes */
	uint64_t blur_drawn;              /* version baked into the current buffer */
	struct wlr_texture *blur_own_tex; /* texture of the private pyramid */
	uint64_t blur_own_id;             /* identity of blur_own_tex */
	struct vidow_blur_set blur_set; /* private pyramid, allocated on demand */

	/* ext-background-effect 的模糊区域（窗口局部坐标）。blur_region_on 为
	 * true 时着色器只在这个盒子里画模糊；-1 表示不限制。 */
	bool blur_region_on;
	float blur_region[4]; /* x, y, w, h */

	/* 只画「背后的模糊」：layer surface 的底衬用它。内容和阴影/边框/圆角/噪点
	 * 全部关掉，输出就是一层模糊，垫在 layer surface 自己的节点下面。 */
	bool blur_only;

	bool hidden;  /* temporarily disabled for the backdrop capture */
};

/* 「有人正拿着这个窗口」（窗口截图源：录屏 / 窗口预览）。记录挂在窗口上，源自己
 * 销毁时摘掉；客户端断开（录屏程序崩了 / 被关掉）时也摘掉 —— 不然一个跑掉的
 * 录屏客户端会让窗口一直按 60fps 补帧。 */
struct vidow_capture_hold {
	struct wl_list link; /* vidow_view.capture_sources */
	struct wl_listener destroy;        /* 截图源自己没了（窗口关了） */
	struct wl_listener client_destroy; /* 客户端跑了（录屏程序崩了 / 被关掉） */
	struct wl_client *client;
	struct vidow_view *view;
	struct vidow_server *server;
	struct wlr_ext_image_capture_source_v1 *source;
};

/* A window that is fading out after its surface is gone: the last committed
 * buffer is snapshotted so the close animation has something to draw. */
struct vidow_closing {
	struct wl_list link; /* vidow_server.closing */
	struct vidow_server *server;
	struct wlr_scene_buffer *node;
	struct wlr_buffer *buffer; /* locked snapshot */
	struct vidow_anim *anim;
	struct vidow_fx_target fx;
	double alpha;
	double off_y;
	int slide_px;
	int x, y, w, h;
	double scale;
};

/* ------------------------------------------------------------------ output */

struct vidow_output {
	struct wl_list link; /* vidow_server.outputs */
	struct vidow_server *server;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	struct wlr_output_layout_output *layout_output;

	struct wlr_box usable_area; /* layout coords, minus layer exclusive zones */
	/* 上次已知的布局位置（逻辑坐标）。拔线时 wlr_output_layout 可能已经先把这块
	 * 输出忘掉了，热插拔要靠这份存着的位置算「挪到别的屏」的平移量。 */
	struct wlr_box box;
	/* 这块屏的热插拔记忆（按输出名认）。在 new_output 里就取好存下：
	 * 拔线时 wlroots 已经把 wlr_output->name 收走了，那时候再按名字找就晚了。 */
	struct vidow_output_memory *memory;
	double viewport_x;          /* horizontal scroll offset of the column strip */
	int64_t last_frame_ns;      /* last frame event, for the animation watchdog */


	/* Backdrop snapshot: the scene without the blurred windows, used as the
	 * source for the kawase chain. */
	struct wlr_buffer *backdrop;
	int backdrop_w, backdrop_h;

	/* Wallpaper-only blurred backdrop for this output: the scene below the
	 * toplevels with every window hidden, so it survives client repaints,
	 * window moves and viewport scrolling. */
	struct wlr_texture *blur_tex;
	struct vidow_blur_set blur_set;
	uint64_t blur_id;   /* identity of blur_tex, 0 = none */
	uint64_t blur_gen;  /* gen_layer the pyramid was captured at */
	int blur_levels;
	/* 建这张金字塔时用的是哪套参数：改 blur_offset / blur_alpha_min 也得重算
	 * （它们是在 kawase 的着色器里作为 uniform 用的，缓存的贴图不会跟着变）。 */
	double blur_offset;
	double blur_alpha_min;

	/* 视口外补帧的到点闹钟：节流到 30Hz / 15Hz 的时候，光靠客户端提交来推进
	 * 是不行的（提交一次之后就没人再推了，链子断在半路）—— 到点了由它叫醒一次。 */
	struct wl_event_source *pump_timer;
	int64_t pump_at_ms; /* 闹钟定在什么时候（0 = 没定） */

	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

/* -------------------------------------------------------------------- view */

/* Stored in scene_tree->node.data so a hit-tested scene node can be traced
 * back to the view that owns it. */
/* How a window sits in the layout. */
enum vidow_float_mode {
	VIDOW_FLOAT_NONE = 0, /* tiled: lives in a column */
	VIDOW_FLOAT_FIXED,    /* floats at its own position on the output */
	VIDOW_FLOAT_FOLLOW,   /* floats at a fixed offset from a parent view */
};

enum vidow_grab_mode {
	VIDOW_GRAB_NONE = 0,
	VIDOW_GRAB_MOVE,
	VIDOW_GRAB_RESIZE,
};

/* niri-style column: a vertical stack of windows, columns are laid out left
 * to right and the whole strip scrolls horizontally. */
struct vidow_column {
	struct wl_list link;    /* vidow_workspace.columns, left to right */
	uint64_t id;            /* IPC 用的稳定编号（见 vidow_view.id） */
	struct wl_list views;   /* vidow_view.column_link, top to bottom */
	struct vidow_server *server;
	struct vidow_workspace *ws; /* 属于哪块工作区 */
	struct wlr_output *output; /* 现在摆在哪块屏上（拔线后可能先借别的屏） */
	/* 归属哪块屏（热插拔记忆）。output 是「住哪」，home 是「属于哪」——
	 * 屏拔了就借别的屏住，home 不动；屏回来时按 home 认领回去。 */
	struct vidow_output_memory *home;

	double width_fraction; /* of the usable width */
	int offset;            /* from the usable area's left edge */
	int width;             /* computed each arrange */
};

struct vidow_view_tag {
	uint32_t magic;
	struct vidow_view *view;
};

/* 窗口是 Wayland 的还是 X11 的（XWayland 桥过来的）。两者共用同一套布局 /
 * 装饰 / 动画代码，差别收敛到下面几个 vidow_view_* 访问器里。 */
enum vidow_view_kind {
	VIDOW_VIEW_XDG = 0,
	VIDOW_VIEW_X11,
};

struct vidow_view {
	struct wl_list link;        /* vidow_server.views */
	/* IPC 用的稳定编号（进程内唯一，从 1 起）。窗口一建就分配，之后不变 ——
	 * 外面的脚本靠它指认窗口（vidowmsg close-window --id 3）。 */
	uint64_t id;
	struct wl_list column_link; /* vidow_column.views, top to bottom */
	struct wl_list ws_link;     /* 浮窗专属：挂在工作区的 views 上（进列了就不挂） */
	struct vidow_column *column;
	/* 属于哪块工作区（浮窗 map 时定下，列的窗口跟着列走）。工作区藏起来的时候
	 * 窗口还活着，只是场景节点被关掉。 */
	struct vidow_workspace *ws;
	struct vidow_server *server;
	enum vidow_view_kind kind;
	struct wlr_xdg_toplevel *xdg_toplevel;   /* kind == XDG 时有效 */
	struct wlr_xwayland_surface *x11;        /* kind == X11 时有效 */
	struct wlr_scene_tree *scene_tree;
	struct wlr_output *output; /* 现在摆在哪块屏上 */
	/* 归属哪块屏（热插拔记忆）：平铺窗口跟着所在列的 home，浮动窗口是自己的。
	 * 屏拔了先借别的屏住，屏回来时按 home 挪回去。 */
	struct vidow_output_memory *home;
	/* 浮动窗口「相对自己那块屏原点」的偏移。布局会随着拔插重新摆（拔掉左边那块，
	 * 右边那块可能从 x=1280 挪到 x=0），所以只记绝对坐标会被搬错一次屏。 */
	int home_dx, home_dy;

	struct wlr_box geom; /* last arranged geometry, layout coords */
	/* 最后已知的标题 / app_id（IPC 的 window-closed 事件要带它们，而客户端的
	 * 字符串在销毁信号之后就没了）。map / 标题变化时刷新。 */
	char last_title[128];
	char last_app_id[128];
	struct vidow_view_tag tag;
	bool mapped;
	bool configured; /* initial configure already answered */
	bool fullscreen;

	/* X11（XWayland）专用：popup = 菜单 / 提示 / 下拉框这类「弹出物」。对应
	 * Wayland 的 xdg_popup：不装饰、不跑 shader、不做开关动画、不参与布局
	 * （位置尺寸由客户端给死），也不改「谁是焦点窗口」。 */
	bool popup;
	/* 视口外强制渲染：窗口滚出画面（或所在工作区被藏起来）时照样给它补帧回调，
	 * 客户端不会停帧 —— 录屏 / 窗口预览 / 切回来看到的就是新画面而不是冻住的一张。
	 *   force_render       —— 窗口规则给的（config.lua 的 vidow.rule）
	 *   render_holders     —— 有谁正拿着它（窗口截图源 / 预览），拿着就强制
	 *   render_pumping     —— 现在正补着（只在状态翻转时打日志） */
	bool force_render;
	/* 焦点行为（窗口规则给的，见 rule.c）：新窗口抢不抢焦点、别人请求激活它时
	 * 理不理。默认都是 true（= 老行为）。有些客户端（Electron 系）会开一个
	 * 看不见的辅助窗口把自己顶上来，或者拿一个早就过期的激活令牌去抢焦点 ——
	 * 这两条就是给那种窗口留的开关。 */
	bool focus_on_open;
	bool focus_on_activate;
	/* 这个窗口的激活请求即使 serial 校验不过也照样理（窗口规则 honor_invalid_serial，
	 * 见 activation.c）。默认 false = 不理。 */
	bool honor_invalid_serial;
	/* 有人请求激活它、但它还没上屏：记下来，等 map 的时候兑现（点通知常常比
	 * 窗口第一次 commit 还早一点）。 */
	bool pending_activate;
	struct wl_list capture_sources; /* vidow_capture_hold.link */
	bool render_pumping;
	/* 补帧节流：这个窗口每秒最多补多少帧（0 = 不限）。规则可以逐窗口覆盖，
	 * 没写就用配置里的 force_render_hz。pump_next_ms 是下一次到点的时刻。 */
	int pump_interval_ms;
	int64_t pump_next_ms;
	/* 内容在场景里的 1:1 尺寸（不带 overview 缩放）。overview 里尺寸要乘上缩放，
	 * 而 multiply 是会累加的 —— 所以基线只在 commit 时（没进 overview 时）记一次。 */
	int base_dst_w, base_dst_h;
	/* 上次记「缓冲区多大 / 几倍」日志时的值：每个 commit 都刷一行没意义。 */
	int logged_scale;
	int logged_buf_w;
	/* X11：客户端最后一次 configure 请求的尺寸（X 像素，0 = 没请求过）。
	 * 客户端常常在 map 之前就发这个请求，那时候它自己的窗口还是 1x1、我们也
	 * 还不知道它是个对话框 —— 记下来，等尺寸真的要用的时候再取。 */
	int x11_want_w, x11_want_h;
	struct wl_listener x11_associate;
	struct wl_listener x11_dissociate;
	struct wl_listener x11_set_title;
	struct wl_listener x11_set_class;
	struct wl_listener x11_set_parent;
	struct wl_listener x11_set_geometry;
	struct wl_listener x11_set_override_redirect;
	struct wl_listener x11_set_hints;
	struct wl_listener x11_set_size_hints;
	struct wl_listener x11_set_window_type;
	struct wl_listener x11_request_configure;
	struct wl_listener x11_request_move;
	struct wl_listener x11_request_resize;
	struct wl_listener x11_request_activate;
	struct wl_listener x11_request_close;
	struct wl_listener x11_request_fullscreen;
	struct wl_listener x11_request_maximize;
	struct wl_listener x11_destroy;

	/* animation */
	struct vidow_anim *anim_move; /* position tween */
	struct vidow_anim *anim_open; /* open tween */
	double anim_off_x, anim_off_y;  /* added to geom.x/y while animating */
	double anim_alpha;              /* 0..1, applied to every buffer */
	double anim_from_x, anim_from_y; /* move tween start offsets */
	bool placed;                     /* laid out at least once */

	/* custom shader output */
	struct wlr_scene_surface *scene_surface;
	struct vidow_fx_target fx;
	int fx_dx, fx_dy; /* surface offset inside the view tree */
	struct wlr_buffer *last_buffer; /* locked, feeds the close animation */

	/* floating */
	enum vidow_float_mode float_mode;
	/* IPC 的 set-window-size 要的尺寸（0 = 没要过）。客户端下一次提交要是还带
	 * 旧尺寸，不能把「我让它多大」改回去 —— 一个迟到的提交会把这条命令吃掉
	 * （踩过：改完尺寸，客户端先为别的原因提交一次旧尺寸，float_geom 就被它
	 * 覆盖回去，随后 arrange 又按旧尺寸 configure，等于没改）。 */
	int float_want_w, float_want_h;
	struct wlr_box float_geom; /* FIXED: absolute box
	                              FOLLOW: x/y = offset from the parent tile */
	struct vidow_view *parent; /* FOLLOW: the view we track */

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener set_title;
	struct wl_listener set_parent;
	struct wl_listener request_fullscreen;
	struct wl_listener request_maximize;
	struct wl_listener request_move;
	struct wl_listener request_resize;

	/* 窗口列表（waybar / xdg-desktop-portal 的 ScreenCast 窗口选择器） */
	struct wlr_foreign_toplevel_handle_v1 *ftm_handle;
	struct wlr_ext_foreign_toplevel_handle_v1 *ftl_handle;
	/* wlroots 不判断状态是否真的变了，重复 set_activated 会白刷一遍客户端 */
	bool portal_activated;
	bool portal_entered;
	struct wl_listener ftm_request_activate;
	struct wl_listener ftm_request_close;
	struct wl_listener ftm_request_fullscreen;
};

/* ----------------------------------------------------------- layer shell */

struct vidow_layer_surface {
	struct wl_list link; /* vidow_server.layers */
	uint64_t id;         /* IPC 用的稳定编号（见 vidow_view.id） */
	struct vidow_server *server;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene;
	struct vidow_output *output; /* 现在摆在哪块屏上 */
	/* 归属哪块屏（热插拔记忆）：状态栏这类东西屏拔了要挪到别的屏上，
	 * 屏回来时再挪回去。 */
	struct vidow_output_memory *home;
	uint32_t layer;

	/* animation */
	struct vidow_anim *anim;
	double anim_off_x, anim_off_y;
	double anim_alpha;
	struct wlr_box geom; /* last configured box, layout coords */
	double anim_from_x, anim_from_y; /* open tween start offsets */
	struct wlr_buffer *last_buffer; /* locked, feeds the close animation */

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener new_popup;

	/* 它背后的模糊：一个垫在 ls->scene->tree 底下的节点，用和窗口装饰同一套
	 * 着色器画（blur_only）。半透明的状态栏透出来就是模糊的底衬。 */
	struct vidow_fx_target fx;
};

/* ---------------------------------------------------------------- keyboard */

struct vidow_keyboard {
	struct wl_list link; /* vidow_server.keyboards */
	struct vidow_server *server;
	struct wlr_keyboard *wlr_keyboard;
	/* 虚拟键盘（wtype、输入法回注）的来源，物理键盘为 NULL */
	struct wlr_virtual_keyboard_v1 *virtual;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

/* ------------------------------------------------------------ input method */

/* 客户端的一个输入框（zwp_text_input_v3）。一个客户端可以同时开好几个，
 * 比如两个窗口各一个，所以这里是个链表。 */
struct vidow_text_input {
	struct wl_list link; /* vidow_server.text_inputs */
	struct vidow_server *server;
	struct wlr_text_input_v3 *input;
	bool enabled; /* 客户端要输入，且我们已经把它交给输入法 */

	struct wl_listener enable;
	struct wl_listener commit;
	struct wl_listener disable;
	struct wl_listener destroy;
};

/* 输入法的候选框（zwp_input_popup_surface_v2）。协议规定由合成器渲染并
 * 摆位：它自己不知道屏幕坐标，我们得把光标矩形喂回去。 */
struct vidow_im_popup {
	struct vidow_server *server;
	struct wlr_input_popup_surface_v2 *popup;
	struct wlr_scene_surface *scene;

	struct wl_listener commit;
	struct wl_listener destroy;
};

/* 当前连上的输入法（fcitx5 / ibus）。同一时间只服务一个。 */
struct vidow_im {
	struct vidow_server *server;
	struct wlr_input_method_v2 *method;
	struct wlr_input_method_keyboard_grab_v2 *grab;
	bool active; /* 已经给输入法发过 activate */

	struct wl_listener commit;
	struct wl_listener new_popup;
	struct wl_listener grab_keyboard;
	struct wl_listener grab_destroy;
	struct wl_listener destroy;
};

/* ------------------------------------------------------------------ 剪贴板 */

/* 为什么要自己接管（而不是直接用 wlroots 的）：wlroots 0.20 的 xwayland/selection/
 * {incoming,outgoing}.c 确实做了 X11 <-> Wayland 的桥，但两个方向都卡在同一条闸门
 * 上 —— 「必须有 X11 窗口聚焦」：
 *
 *     incoming.c  xwm_handle_selection_notify(): if (xwm->focus_surface == NULL)
 *                     → "denying write access to clipboard: no xwayland surface focused"
 *     outgoing.c  xwm_handle_selection_request(): if (xwm->focus_surface == NULL &&
 *                     xwm->drag_focus == NULL) → "denying read access to selection"
 *
 * 于是「在 Wayland 终端里跑 xclip -i」「X11 程序退出之后再粘」「没有 X11 窗口聚焦时
 * 用 xclip -o」全都不通。struct wlr_xwm 是 opaque 的，闸门既没有配置项也没有公开
 * API 能关，所以这里用 wlr_xwayland->user_event_handler（公开字段，在 xwm 默认处理
 * 之前被调用，返回 true 就跳过 wlroots 的处理）自己接管：
 *
 *   1) 拦下 CLIPBOARD / PRIMARY 的 XFixesSelectionNotify（X 侧换了 owner），自己
 *      xcb_convert_selection 取数 —— wlroots 的 xwm 从此看不到这两个选择的变化，
 *      它那两条闸门自然失效；
 *   2) 拦下发往「X 侧代理窗口」的 SelectionRequest 自己应答。X 的规矩是请求只送给
 *      当前 owner，而 xwm 一看到 Wayland 这边 set_selection 就去抢 owner
 *      （handle_seat_set_selection），抢到的是它自己那个代理窗口 —— 所以「X11
 *      客户端来粘」的请求全都会落到那个窗口上，拦下来就接管了这条路径，同样不看
 *      焦点。我们**不跟它抢 owner**：让它当着，请求照样归我们答，省掉一场 owner
 *      争夺战（也就没有 SelectionClear 来回抖）；
 *   3) 数据全部读进内存（每个 mime 一份，clipboard_max_bytes 封顶）：X11 或
 *      Wayland 那边的源程序退出之后，两边都还能粘贴。
 *
 * 不认识的一律不碰：DND（XdndSelection）的 XFixes 通知原样放给 wlroots，拖放照旧
 * 走它那套。
 */
#define VIDOW_CLIP_MIMES_MAX 32  /* 一份内容最多缓存几个 mime 类型 */
#define VIDOW_CLIP_INCR_CHUNK (64 * 1024) /* INCR 分块大小（ICCCM 惯例） */
#define VIDOW_CLIP_TIMEOUT_MS 5000        /* 对方不吭声时的兜底 */

enum vidow_clip_kind {
	VIDOW_CLIP_CLIPBOARD,
	VIDOW_CLIP_PRIMARY,
};

/* 缓存下来的一个 mime 类型。data 可能还没取全（complete=false），也可能超了上限
 * 被丢掉（overflow=true）—— 两种情况下对应的类型都不对外提供。 */
struct vidow_clip_mime {
	char *mime;
	xcb_atom_t atom; /* X 侧的 target（X11 来的内容才有意义） */
	unsigned char *data;
	size_t len;
	bool complete;
	bool overflow;
};

/* 一次「X11 -> 我们」的取数会话：先问 TARGETS，再一个类型一个类型取。 */
struct vidow_clip_fetch {
	bool active;
	int next;              /* 下一个要取的类型下标（-1 = TARGETS 还没回来） */
	xcb_atom_t *targets;   /* TARGETS 的结果 */
	int ntargets;
	struct wl_array chunk; /* 当前这个类型正在收的字节（INCR 时一块一块攒） */
	bool incr;
	xcb_atom_t property;   /* 当前取数用的属性名 */
	uint64_t started_ms;   /* 兜底超时用 */
};

/* 一次「我们 -> X11」的应答（对某个 SelectionRequest）。数据没取全时先挂着
 * （waiting=true），取全了再发；大块走 INCR，一块一块等对面删属性。 */
struct vidow_clip_out {
	struct wl_list link; /* vidow_clip_selection.out */
	struct vidow_clip_selection *sel;
	xcb_window_t requestor;
	xcb_atom_t property;
	xcb_atom_t target;
	char *mime;    /* 要发的那份的类型名（取数会重建类型表，所以记名字不记指针） */
	size_t offset; /* INCR 分块发到哪儿了 */
	bool incr;
	bool waiting;         /* 数据还没取全，先等 */
	bool sent_end;        /* INCR 的 0 长度收尾已经发过 */
	uint64_t deadline_ms; /* 一直等不到数据就别让对面干等 */
};

/* 等待中的「Wayland 客户端来粘」：数据还没取全时先攥着 fd，取全了再写。
 * 也可能只是「还没写完」（管道满了），那就挂个可写事件继续。 */
struct vidow_clip_waiter {
	struct wl_list link; /* vidow_clip_selection.waiters */
	struct vidow_clip_selection *sel;
	char *mime;
	int fd;
	size_t offset;
	struct wl_event_source *source;
	uint64_t deadline_ms;
};

/* 一次「把 Wayland 客户端的数据拉进内存」：一个 mime 一条管道。 */
struct vidow_clip_pull {
	struct wl_list link; /* vidow_clip_selection.pulls */
	struct vidow_clip_selection *sel;
	char *mime;
	int fd;
	struct wl_event_source *source;
	uint64_t deadline_ms;
};

struct vidow_clip_selection {
	struct vidow_server *server;
	struct vidow_clipboard *clip;
	enum vidow_clip_kind kind;
	xcb_atom_t atom;
	const char *name; /* "clipboard" / "primary" */
	bool primary;

	/* 当前对外的那一份内容（Wayland 发布的源和 X11 的应答都读它）。 */
	struct vidow_clip_mime mimes[VIDOW_CLIP_MIMES_MAX];
	int nmimes;
	/* 正在从 X11 取的那一份：取全了才换上去（见 clipboard.c 的 commit）。
	 * 为什么要两份：X11 那边一取数，我们就得把 Wayland 这边的源发布出去，
	 * 而 wlroots 的 xwm 一看到 Wayland 有非 X11 的源就去抢 X owner —— 抢走之后
	 * 我们再去向原 owner 要数据就没人答了。所以取数期间不碰 X 侧 owner。 */
	struct vidow_clip_mime staging[VIDOW_CLIP_MIMES_MAX];
	int nstaging;
	bool from_x11; /* 现在这份内容是从 X11 取来的 */
	xcb_window_t x_owner; /* X 侧当前 owner（0 = 没有，IPC 报出去用） */
	xcb_atom_t property;  /* 取数时用的属性名（两份选择各一个，别互相串） */

	/* 我们发布给 Wayland 的源：X11 来的内容，以及 Wayland 客户端退出之后从缓存
	 * 重新发出去的那一份。**每次发布都是一个新对象**（见 clipboard.c 的
	 * struct vidow_clip_source）：seat 换选择时会 destroy 掉旧的那个，复用同一个
	 * 结构体等于「同一个对象被 destroy 两次」，wl_array_release 之后数组里是毒
	 * 指针，第二次就段错误。NULL = 现在没发布。 */
	struct wlr_data_source *published;
	struct wlr_primary_selection_source *published_primary;

	struct wl_listener seat_set; /* 或 seat_set_primary，见 clipboard.c */
	/* XFixes 通知去重：我们自己的窗口和 xwm 的代理窗口都注册了 XFixes，
	 * 同一件事会收到两遍（同一个连接）。按 (owner, time) 记一下。 */
	bool have_last;
	uint32_t last_owner;
	uint32_t last_time;

	struct vidow_clip_fetch fetch;
	struct wl_list out;     /* struct vidow_clip_out */
	struct wl_list waiters; /* struct vidow_clip_waiter */
	struct wl_list pulls;   /* struct vidow_clip_pull */
	struct wl_event_source *tick; /* 兜底：超时、重发等待中的请求 */
	uint32_t timestamp;     /* X 侧 TIMESTAMP 用 */
};

struct vidow_clipboard {
	struct vidow_server *server;
	bool active;   /* 接管中（配置开着 + X 侧就绪） */
	bool have_x11; /* xwm 的连接可用 */

	xcb_connection_t *conn;
	xcb_window_t window; /* 我们自己的代理窗口（XFixes 的监听点） */
	struct wl_event_source *attach_timer; /* ready 之后延迟接管（见 clipboard.c） */
	/* xwm 自己的两个代理窗口：它们的 owner 通知要跳过，否则会把我们刚发出去的
	 * 数据再取回来（识别见 clipboard.c 的 find_proxies()）。 */
	xcb_window_t proxy_clipboard;
	xcb_window_t proxy_primary;
	int xfixes_first; /* XFixes 的 first_event（判事件用） */

	xcb_atom_t atom_clipboard, atom_primary, atom_targets, atom_timestamp,
		atom_utf8, atom_text, atom_incr, atom_wl_selection, atom_wl_primary,
		atom_multiple, atom_save_targets, atom_delete, atom_string,
		atom_compound_text;

	struct vidow_clip_selection clip;
	struct vidow_clip_selection primary;
};

/* ------------------------------------------------------------------ server */

struct vidow_server {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wlr_scene *scene;
	struct wlr_scene_tree *layer_background;
	/* overview 的压暗遮罩：夹在壁纸和窗口之间（见 overview.c） */
	struct wlr_scene_tree *layer_overview_dim;
	struct wlr_scene_tree *layer_bottom;
	struct wlr_scene_tree *layer_toplevels;
	struct wlr_scene_tree *layer_popups; /* X11 的菜单/提示：压在所有窗口之上 */
	struct wlr_scene_tree *layer_top;
	struct wlr_scene_tree *layer_overlay;
	struct wlr_scene_tree *layer_im; /* 输入法候选框，压在所有层之上 */
	struct wlr_output_layout *output_layout;
	struct wlr_scene_output_layout *scene_layout;

	struct wlr_compositor *compositor;
	struct wlr_xdg_shell *xdg_shell;
	/* XWayland（见 xwayland.c）。xwayland 为 NULL = 配置里关了。 */
	struct wlr_xwayland *xwayland;
	struct wl_listener new_xwayland_surface;
	struct wl_listener xwayland_ready;
	struct wl_listener xwayland_destroy;
	/* X11 客户端被告知的缩放。X 只有一套坐标，所以只能是一个全局值。 */
	double xwayland_scale;
	/* 剪贴板桥（见 clipboard.c）：X11(Xwayland) <-> Wayland */
	struct vidow_clipboard clipboard;
	struct wlr_layer_shell_v1 *layer_shell;
	/* zxdg_decoration_manager_v1（见 decoration.c）：客户端问「装饰谁画」。
	 * 不给这个全局，kitty / GTK / Qt 就自己画标题栏，跟我们的圆角打架。 */
	struct wlr_xdg_decoration_manager_v1 *decoration_manager;
	struct wl_listener new_decoration;
	struct wlr_screencopy_manager_v1 *screencopy;
	struct wl_global *bgeffect; /* ext_background_effect_manager_v1 */
	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard;
	struct wlr_virtual_pointer_manager_v1 *virtual_pointer;

	/* xdg-desktop-portal 及其 GTK / KDE 后端要的协议，见 portal.c */
	/* xdg-activation-v1 自己实现（见 activation.c）：唯一理由是这个校验规则
	 * 得能配（honor_xdg_activation_with_invalid_serial），wlroots 那份写死了。 */
	struct wl_global *activation_global;
	struct wl_list activation_tokens; /* vidow_activation_token.link */
	struct wl_event_source *activation_timer; /* 清过期令牌（timeout > 0 才建） */
	int activation_sweep_ms;
	struct wlr_foreign_toplevel_manager_v1 *ftm;
	struct wlr_ext_foreign_toplevel_list_v1 *ftl;
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *ftl_capture;
	struct wlr_xdg_foreign_registry *foreign_registry;
	struct wlr_xdg_foreign_v1 *foreign_v1;
	struct wlr_xdg_foreign_v2 *foreign_v2;
	struct wl_listener new_capture_request;   /* ext-foreign-toplevel-image-capture-source */

	struct wlr_text_input_manager_v3 *text_input_mgr;
	struct wlr_input_method_manager_v2 *input_method_mgr;
	struct wl_list text_inputs; /* vidow_text_input.link */
	struct vidow_im im;
	struct vidow_im_popup im_popup;
	struct wl_listener new_text_input;
	struct wl_listener new_input_method;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wlr_seat *seat;

	struct vidow_config config;
	struct vidow_decor_prog decor;
	struct vidow_gl_prog kawase_down, kawase_up;
	struct wlr_texture *empty_tex; /* 1x1 transparent, for disabled samplers */
	uint64_t blur_serial;          /* hands out blur texture identities */

	/* Backdrop generations. gen_layer covers the wallpaper and the layer
	 * surfaces, gen_scene additionally covers every window: a cached blur is
	 * reused until the matching generation moves. */
	uint64_t gen_layer, gen_scene;

	/* 正在渲染的那个输出的物理尺寸和布局原点（物理像素），给着色器当
	 * u_output_size / u_origin 的基准。 */
	int output_w, output_h;
	int output_x, output_y;

	struct wl_list anims;    /* vidow_anim.link */
	struct wl_list closing; /* vidow_closing.link */
	struct wl_list shaders; /* vidow_shader.link */
	bool shader_ok;         /* GLES2 custom shader path usable */

	struct wl_list outputs;   /* vidow_output.link */
	struct wl_list views;     /* vidow_view.link */
	struct wl_list keyboards; /* vidow_keyboard.link */
	struct wl_list layers;    /* vidow_layer_surface.link */
	struct wl_list columns;   /* vidow_column.link（所有列的并集，顺序 = 工作区顺序） */
	struct wl_list workspaces; /* vidow_workspace.link */
	struct wl_list keybinds;   /* vidow_keybind.link */
	struct wl_list rules;      /* vidow_rule.link（窗口规则） */
	struct wl_list test_actions; /* vidow_test_action.link（VIDOW_TEST_ACTION） */
	struct wl_event_source *test_pipe; /* 一次性定时表 / 动作 FIFO 的 fd 源 */
	/* 见过的每一块屏都留一条记忆（拔了也不删），热插拔按它还原工作区。 */
	struct wl_list output_memories; /* vidow_output_memory.link */
	struct wl_list execs;     /* vidow_exec.link, config commands in flight */
	struct wl_event_source *exec_timer; /* one shared timeout timer */
	struct wl_event_source *capture_timer; /* drives pending screen captures */
	struct vidow_script *script;      /* NULL when there is no config.lua */
	struct wl_event_source *reload_sig; /* SIGHUP */
	struct vidow_configwatch *configwatch; /* 配置文件变了自动重载，NULL = 没盯上 */

	struct wl_listener new_output;
	struct wl_listener new_input;
	struct wl_listener new_toplevel;
	struct wl_listener new_popup;
	struct wl_listener new_layer_surface;
	struct wl_listener new_virtual_keyboard;
	struct wl_listener new_virtual_pointer;

	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;

	/* interactive move/resize grab */
	enum vidow_grab_mode grab_mode;
	struct vidow_view *grab_view;
	double grab_x, grab_y;
	struct wlr_box grab_geom;

	/* overview（niri 风格全景） */
	struct vidow_overview overview;

	/* IPC（见 ipc.c / ipcstate.c） */
	struct wl_event_source *ipc_listen; /* 监听 socket 的 fd 源 */
	struct wl_list ipc_clients;         /* ipc_client.link */
	char ipc_path[PATH_MAX];            /* 实际在听的路径（空 = 没起来） */
	int ipc_subscribers;                /* 订阅了事件的连接数 */
	/* IPC 报给外面的稳定编号（从 1 起，0 = 还没分配） */
	uint64_t next_view_id;
	uint64_t next_column_id;
	uint64_t next_layer_id;

	struct vidow_view *focused;
	struct vidow_layer_surface *keyboard_layer; /* exclusive layer grab */
	struct wlr_output *focused_output;

	/* Fallback clock: advances animations when the backend stops delivering
	 * frame events (a nested Wayland output stalls while the host is idle). */
	struct wl_event_source *anim_timer;
};

/* -------------------------------------------------------------------- init */

bool vidow_output_init(struct vidow_server *server);
/* 按配置（scale / outputs）算并设一个输出的缩放；SIGHUP 后整套重来。 */
void vidow_output_apply_scale(struct vidow_server *server,
		struct vidow_output *output);
void vidow_output_apply_scale_all(struct vidow_server *server);
/* 输出的运行时改动（IPC 用）。都会 commit 一次并把跟着变的东西（布局、背景
 * 缓存、光标、可用区域）重做一遍。why 只进日志。 */
bool vidow_output_set_scale(struct vidow_server *server,
		struct wlr_output *wlr_output, double scale, const char *why);
bool vidow_output_set_transform(struct vidow_server *server,
		struct wlr_output *wlr_output, enum wl_output_transform transform);
bool vidow_output_set_enabled(struct vidow_server *server,
		struct wlr_output *wlr_output, bool enabled);
bool vidow_output_set_mode(struct vidow_server *server,
		struct wlr_output *wlr_output, int width, int height, double refresh_hz);
bool vidow_output_set_position(struct vidow_server *server,
		struct wlr_output *wlr_output, int x, int y);
bool vidow_input_init(struct vidow_server *server);
bool vidow_view_init(struct vidow_server *server);
/* xdg_popup 的 unconstrain 只能在 popup 自己第一次 commit 之后做（wlroots 里
 * schedule_configure 有 assert(surface->initialized)），见 view.c 的注释。 */
void vidow_popup_unconstrain_on_commit(struct vidow_server *server,
	struct wlr_xdg_popup *popup, const struct wlr_box *box);
bool vidow_workspace_init(struct vidow_server *server);
bool vidow_keybind_init(struct vidow_server *server);
bool vidow_rule_init(struct vidow_server *server);
bool vidow_layer_init(struct vidow_server *server);

/* zxdg_decoration_manager_v1（decoration.c）：客户端问「装饰谁画」。
 * apply_config 用于 decor 配置改了之后把已经连上的客户端重新通知一遍。 */
bool vidow_decoration_init(struct vidow_server *server);
void vidow_decoration_apply_config(struct vidow_server *server);
bool vidow_im_init(struct vidow_server *server);
bool vidow_bgeffect_init(struct vidow_server *server);

/* ------------------------------------------------------------ input method */

/* 输入法抢了键盘（keyboard grab）时，按键要直接喂给它，不能再发给 seat。 */
bool vidow_im_grabbed(struct vidow_server *server);
/* 键盘换了（插拔 / 虚拟键盘上下线）：有 grab 就同步给输入法。 */
void vidow_im_set_keyboard(struct vidow_server *server, struct wlr_keyboard *kb);
/* 这个键盘的按键该不该喂给输入法：抢到键盘了就该，但输入法自己用虚拟键盘
 * 回注的按键要排除，否则会自己喂自己转圈。 */
bool vidow_im_owns_keyboard(struct vidow_server *server,
		struct vidow_keyboard *kb);
void vidow_im_key(struct vidow_server *server, uint32_t time_msec,
		uint32_t keycode, uint32_t state);
void vidow_im_modifiers(struct vidow_server *server, struct wlr_keyboard *kb);
/* 键盘焦点变了：把 text-input 的 enter/leave 转给客户端。 */
void vidow_im_focus(struct vidow_server *server, struct wlr_surface *surface);

/* ---------------------------------------------------------------- config */

/* 配置文件只有一个，而且是 Lua（见 script.c）：~/.config/vidow/config.lua。
 * 这里只负责默认值 + 环境变量覆盖；文件本身由 vidow_script_load() 跑。 */
void vidow_config_reset(struct vidow_config *cfg);
/* 配置目录（$XDG_CONFIG_HOME/vidow 或 ~/.config/vidow）。 */
void vidow_config_dir(char *buf, size_t size);
/* 配置文件的完整路径（$VIDOW_CONFIG / $VIDOW_CONFIG_LUA 优先）。 */
void vidow_config_path(char *buf, size_t size);
/* 把所有配置键按名字喂给回调（IPC 的 config 命令 / state 里的 config 用它）。
 * 顺序就是 config.c 里那张表的顺序；value 一律是文本。 */
void vidow_config_each(const struct vidow_config *cfg,
		void (*cb)(void *data, const char *key, const char *value), void *data);
void vidow_config_apply_env(struct vidow_config *cfg);
void vidow_config_log(const struct vidow_config *cfg);
bool vidow_config_has(const char *key);
bool vidow_config_set(struct vidow_config *cfg, const char *key,
		const char *value);
const char *vidow_config_get(const struct vidow_config *cfg,
		const char *key);

/* -------------------------------------------------------------- async exec */

/* Run `sh -c cmd` without ever blocking the compositor. `done` is called from
 * the event loop: stdout and stderr are captured separately (never NULL when
 * ok), and status is the exit code (or 128+signal). When ok is false -- spawn
 * failure or timeout -- errout carries the reason and status is -1. */
typedef void (*vidow_exec_done_t)(void *data, const char *out,
	const char *errout, int status, bool ok);

struct vidow_exec;

bool vidow_exec_init(struct vidow_server *server);
struct vidow_exec *vidow_exec_run(struct vidow_server *server,
		const char *cmd, vidow_exec_done_t done, void *data);
void vidow_exec_cancel_all(struct vidow_server *server);
/* 撒手跑一条 shell 命令：双 fork，不等它，输出丢掉。 */
void vidow_exec_detached(const char *script);

/* --------------------------------------------------------------- config.lua */

/* 重载配置：重新跑 config.lua（键位、窗口规则重新声明），再把跟缩放有关的
 * 东西应用一遍。SIGHUP 和 IPC 的 reload-config 走的是同一个函数。 */
void vidow_config_reload(struct vidow_server *server);
/* 配置落地后对账：ipc / xwayland 这两个「启动时就消费掉」的键要跟上配置。 */
void vidow_config_sync(struct vidow_server *server);
bool vidow_script_init(struct vidow_server *server);
bool vidow_script_load(struct vidow_server *server);
void vidow_script_finish(struct vidow_server *server);

/* 配置文件一改就自动重载（inotify 盯目录 + stat 比对，见 configwatch.c）。
 * 失败只打日志：自动重载是锦上添花，SIGHUP 永远还在。 */
bool vidow_configwatch_init(struct vidow_server *server);
void vidow_configwatch_finish(struct vidow_server *server);

/* ------------------------------------------------------------- animation */

int64_t vidow_now_ns(void);
struct vidow_anim *vidow_anim_start(struct vidow_server *server,
		const char *tag, int duration_ms, enum vidow_easing easing,
		void (*update)(void *data, double t),
		void (*done)(void *data, bool cancelled), void *data);
void vidow_anim_cancel(struct vidow_anim *anim);
void vidow_anim_tick(struct vidow_server *server);
void vidow_output_render(struct vidow_output *output);
void vidow_output_kick_watchdog(struct vidow_server *server);
bool vidow_anim_pending(struct vidow_server *server);

/* ----------------------------------------------- ext-background-effect-v1 */

/* 这个 surface 请求了模糊吗？请求了就把区域（surface 局部坐标）写进 box。 */
bool vidow_bgeffect_blur_box(struct vidow_server *server,
		struct wlr_surface *surface, struct wlr_box *box);

/* ------------------------------------------------------------- scene fx */

void vidow_fx_set_alpha(struct wlr_scene_node *node, double alpha);
bool vidow_shader_init(struct vidow_server *server);
struct vidow_shader *vidow_shader_get(struct vidow_server *server,
		const char *name);

bool vidow_fx_begin(struct vidow_server *server, struct vidow_fx_target *fx,
		struct wlr_scene_tree *parent, const char *shader_name);
/* scale = 窗口所在输出的缩放：离屏缓冲按它分配，装饰才是按设备像素画的
 * （不然整块窗口在 HiDPI 屏上会被放大一遍，糊）。ox/oy 是窗口在输出上的位置，
 * 逻辑坐标。 */
bool vidow_fx_frame(struct vidow_server *server, struct vidow_fx_target *fx,
		struct wlr_scene_buffer *src, int x, int y, int ox, int oy,
		double alpha, bool focused, double scale, double ov);
void vidow_fx_end(struct vidow_server *server, struct vidow_fx_target *fx);
void vidow_fx_release(struct vidow_fx_target *fx);
bool vidow_fx_set_shader(struct vidow_server *server,
		struct vidow_fx_target *fx, const char *name,
		enum vidow_shader_phase phase);
int vidow_fx_pad(struct vidow_server *server);
/* layer surface 背后的模糊：位置/尺寸变了要作废重画（见 fx.c）。 */
void vidow_layer_blur_invalidate(struct vidow_layer_surface *ls);
void vidow_layer_blur_release(struct vidow_layer_surface *ls);
void vidow_blur_update(struct vidow_output *output);
/* Mark the cached backdrops stale. layers = true for a change to the wallpaper
 * or a background/bottom layer surface; false for anything that only moves or
 * repaints windows. */
void vidow_backdrop_damage(struct vidow_server *server, bool layers);
/* Release the blur resources an output owns. */
void vidow_blur_output_finish(struct vidow_output *output);

void vidow_view_apply_anim(struct vidow_view *view);
/* 区域变了要重画一次：贴图身份没变，不主动失效帧循环不会动。 */
void vidow_view_blur_invalidate(struct vidow_view *view);
/* 从 wlr_surface（含子表面）找回它的窗口，找不到返回 NULL。 */
struct vidow_view *vidow_view_from_surface(struct vidow_server *server,
		struct wlr_surface *surface);
/* xdg-shell 的 configure 只能发给已经 initialized 的 surface。wlroots 0.20 把
 * 「给没初始化的 surface 排 configure」从一句警告改成了 assert，所以在客户端
 * 第一次 commit 之前调 set_size / set_activated 会直接把合成器 abort 掉。
 * 早期路径（比如 xdg-foreign 在第一次 commit 前就设好 parent）先只记状态，
 * 初始 configure 由 view_commit 的 initial_commit 分支统一发。 */
/* 这两个量决定「客户端画的缓冲区能不能 1:1 落到设备像素上」，也就是清不清晰。
 * scale 是窗口所在输出的缩放；x11_hit 见 server.h 下方 vidow_view_hit_scale。 */
struct wlr_surface *vidow_view_surface(const struct vidow_view *view);
const char *vidow_view_title(const struct vidow_view *view);
const char *vidow_view_app_id(const struct vidow_view *view);
/* 客户端自己的窗口几何偏移（CSD 阴影之类），x11 是 0。 */
struct wlr_box vidow_view_geometry(const struct vidow_view *view);
/* 客户端画完第一帧了吗？xdg 的 configure 只能发给 initialized 的 surface。 */
static inline bool vidow_view_configure_ready(const struct vidow_view *view) {
	if (view->kind == VIDOW_VIEW_X11) {
		return view->x11 != NULL && view->x11->surface != NULL &&
			view->x11->surface->mapped;
	}
	return view->xdg_toplevel != NULL &&
		view->xdg_toplevel->base->initialized;
}
/* 把布局几何写给客户端。xdg 走 configure，X11 走 _NET_MOVERESIZE_WINDOW。 */
void vidow_view_set_size(struct vidow_view *view, int w, int h);
void vidow_view_set_activated(struct vidow_view *view, bool activated);
void vidow_view_set_fullscreen(struct vidow_view *view, bool fullscreen);
void vidow_view_set_tiled(struct vidow_view *view, bool tiled);
void vidow_view_close(struct vidow_view *view);
/* 把当前标题 / app_id 记一份（事件用，见 vidow_view.last_title）。 */
void vidow_view_note_names(struct vidow_view *view);
/* 有父窗口的另一半吗（xdg 的 parent / X11 的 WM_TRANSIENT_FOR）。 */
struct wlr_surface *vidow_view_parent_surface(const struct vidow_view *view);
/* 客户端自己要的尺寸，逻辑单位（X11 的像素要除以 xwayland_scale）。 */
void vidow_view_client_size(const struct vidow_view *view, int *w, int *h);
/* X11 窗口该不该浮动 / 该不该装饰（window type、WM_TRANSIENT_FOR、尺寸提示）。 */
enum vidow_float_mode vidow_x11_detect_mode(struct vidow_view *view);
/* 弹出物在屏幕上摆在哪、多大：X 说的是绝对坐标，除回逻辑单位给场景。顺带把它
 * 归到坐标底下那块输出上（菜单可以开在另一块屏上）。false = 尺寸还没定。 */
bool vidow_view_x11_popup_box(struct vidow_view *view, struct wlr_box *box);
/* 把所有弹出物按客户端说的位置摆一遍（它们不进 tiled/floating 布局）。 */
void vidow_popup_place_all(struct vidow_server *server);
/* 摆一个弹出物（幂等：提交、map、重排都可以重调）。 */
void vidow_popup_place(struct vidow_view *view);
/* 客户端声明的「固定尺寸」（xdg 的 min==max，X11 的 WM_NORMAL_HINTS），逻辑单位。
 * 有些客户端映射之前窗口还是 1x1，但已经声明了它想要多大 —— 这里能拿到。 */
bool vidow_view_fixed_size_hint(const struct vidow_view *view, int *w, int *h);
void vidow_view_open_anim(struct vidow_view *view);
void vidow_closing_start(struct vidow_server *server,
		struct wlr_scene_tree *parent, struct wlr_buffer *snapshot,
		int x, int y, int w, int h, int duration_ms, int slide_px,
		const char *shader_name, double scale);

/* ------------------------------------------------------------------ layout */

void vidow_arrange(struct vidow_server *server);
/* 视口外强制渲染：这一帧给「不在画面上但要保活」的窗口补帧回调（详见 view.c）。 */
void vidow_view_render_pump(struct vidow_server *server,
		struct vidow_output *output, uint32_t msec);
/* 记一笔「有人拿着这个窗口的画面」（窗口截图源）。源销毁或者客户端跑掉就自动不再
 * 算持有；拿着期间窗口一直强制渲染。 */
void vidow_view_capture_hold(struct vidow_view *view,
		struct vidow_server *server,
		struct wlr_ext_image_capture_source_v1 *source, struct wl_client *client);
void vidow_place_view(struct vidow_view *view, int x, int y, int w, int h);

/* ------------------------------------------------------------- overview */

bool vidow_overview_init(struct vidow_server *server);
bool vidow_overview_active(const struct vidow_server *server);
void vidow_overview_toggle(struct vidow_server *server);
void vidow_overview_exit(struct vidow_server *server);
/* 每帧渲染前调一次：算好变换（t 变了、布局变了都靠它）并把窗口摆一遍。 */
void vidow_overview_update(struct vidow_server *server);
/* 世界坐标 -> 屏幕坐标（t = 0 时是恒等变换）。 */
void vidow_overview_map(struct vidow_server *server, double wx, double wy,
		double *sx, double *sy);
/* 屏幕坐标 -> 世界坐标（点谁选谁要用）。 */
void vidow_overview_unmap(struct vidow_server *server, double sx, double sy,
		double *wx, double *wy);
double vidow_overview_scale(const struct vidow_server *server);
/* 点一下：选中命中的那列（返回 true = 这次点击被 overview 吃掉了）。 */
bool vidow_overview_click(struct vidow_server *server, double sx, double sy);
void vidow_overview_pan(struct vidow_server *server, double dx);

/* ----------------------------------------------------------------- 热插拔 */

/* 一块屏拔了：它的列 / 浮动窗口 / layer surface 的归属留在记忆里，住处先挪到
 * fallback 上（fallback 可以是 NULL = 一块屏都不剩了）。dead 在调用期间还有效。 */
void vidow_view_output_gone(struct vidow_server *server,
		struct vidow_output_memory *memory, struct wlr_output *dead,
		struct vidow_output *fallback);
/* 一块屏来了（新插的或者回来的）：把归属它的东西放回去。 */
void vidow_view_output_added(struct vidow_server *server,
		struct vidow_output_memory *memory, struct vidow_output *output);
void vidow_layer_output_gone(struct vidow_server *server,
		struct vidow_output_memory *memory, struct wlr_output *dead,
		struct vidow_output *fallback);
void vidow_layer_output_added(struct vidow_server *server,
		struct vidow_output_memory *memory, struct vidow_output *output);
/* 测试钩子专用（VIDOW_TEST_OUTPUTS）：给一块已经加进来的输出重新认身份。 */
void vidow_hotplug_reattach(struct vidow_server *server,
		struct wlr_output *wlr_output);
void vidow_view_join_layout(struct vidow_server *server,
		struct vidow_view *view);
void vidow_view_leave_layout(struct vidow_view *view);
struct wlr_box vidow_work_area(struct vidow_server *server,
		struct wlr_output *output);

/* ------------------------------------------------------------- floating */

void vidow_float_update_mode(struct vidow_view *view);
void vidow_float_set_mode(struct vidow_view *view,
		enum vidow_float_mode mode);
void vidow_float_toggle(struct vidow_view *view);
/* 浮动窗口的尺寸 / 位置（IPC 用）。尺寸会去问客户端（它可以拒绝），位置是
 * 布局绝对坐标（内部换算成「相对自己那块屏原点」的偏移，跟着屏走）。 */
void vidow_float_set_size(struct vidow_view *view, int w, int h);
void vidow_float_move_to(struct vidow_view *view, int x, int y);
void vidow_float_center(struct vidow_view *view);
void vidow_float_place_all(struct vidow_server *server,
		struct vidow_output *output);
void vidow_float_on_commit(struct vidow_view *view);
void vidow_float_detach_children(struct vidow_server *server,
		struct vidow_view *dying);
void vidow_float_begin_grab(struct vidow_server *server,
		struct vidow_view *view, bool resize);
void vidow_float_grab_update(struct vidow_server *server, double lx,
		double ly);
void vidow_float_end_grab(struct vidow_server *server);
void vidow_arrange_layers(struct vidow_server *server);
bool vidow_layer_focus_surface(struct vidow_server *server,
		struct wlr_surface *surface);

/* ------------------------------------------------------------------- views */

struct vidow_view *vidow_view_at(struct vidow_server *server,
		double lx, double ly, struct wlr_surface **surface,
		double *sx, double *sy);
void vidow_focus_view(struct vidow_view *view);
/* 「把这个窗口端到人面前」：先让它那块工作区亮起来（在别的屏上就把焦点挪过去 /
 * 整块工作区搬过来），再给键盘焦点。xdg-activation（点通知）、任务栏按钮、
 * X11 的 _NET_ACTIVE_WINDOW 都走这里 —— 光调 vidow_focus_view 的话，窗口要是
 * 在藏起来的工作区上，焦点就落在一个看不见的窗口上（看起来像「点了没反应」）。 */
void vidow_view_activate(struct vidow_view *view);
void vidow_focus_column(struct vidow_server *server, int direction);
void vidow_focus_window(struct vidow_server *server, int direction);
void vidow_move_column(struct vidow_server *server, int direction);
void vidow_move_window(struct vidow_server *server, int direction);
void vidow_cycle_column_width(struct vidow_server *server);
void vidow_view_close(struct vidow_view *view);
/* 把当前标题 / app_id 记一份（事件用，见 vidow_view.last_title）。 */
void vidow_view_note_names(struct vidow_view *view);
void vidow_view_toggle_fullscreen(struct vidow_view *view);

/* --------------------------------------------------------------- xwayland */

/* 起 XWayland 服务（懒启动：真有人连 X 才 fork 出服务进程）。xwayland=false
 * 时什么也不注册，返回 true —— 没有 X 也是一种正常配置。 */
bool vidow_xwayland_init(struct vidow_server *server);
/* 配置落地后调：配置说关就把 X 服务拆掉，说开（或又打开）就建起来。 */
bool vidow_xwayland_apply_config(struct vidow_server *server);
/* 改完配置（scale / outputs / xwayland_scale）之后重算 X 的全局缩放并同步给
 * 客户端环境。 */
void vidow_xwayland_scale_apply(struct vidow_server *server);
void vidow_xwayland_scale_recompute(struct vidow_server *server);
/* 给 X11 客户端准备环境：GDK_SCALE / QT_SCALE_FACTOR / Xft.dpi，以及写进 X
 * root window 的 Xresources。不设的话 GTK 在 X11 下只按 1:1 画，同一个应用
 * 在 X11 下就比 Wayland 下小一半。 */
void vidow_xwayland_env(struct vidow_server *server);
/* 从 wlr_surface 找回 X11 窗口对应的 view（输入法 / ext-background-effect 用）。 */
struct vidow_view *vidow_view_from_xwayland_surface(struct vidow_server *server,
		struct wlr_xwayland_surface *xsurface);
/* 新建一个 X11 窗口的 view（xwayland.c 收到 new_surface 时调）。 */
void vidow_view_new_x11(struct vidow_server *server,
		struct wlr_xwayland_surface *xsurface);
/* 告诉 wlroots 的场景图：这个 X11 的 surface 缓冲区是按 xwayland_scale 倍画
 * 的，所以放到场景里要缩回去。命中测试的坐标也要按同样的比例换算回来。 */
void vidow_view_x11_scale(struct vidow_view *view);
/* 场景单位 -> surface 局部坐标的倍数（X11 专用，其他窗口恒为 1）。 */
double vidow_view_hit_scale(const struct vidow_view *view);

/* ------------------------------------------------------------------ portal */

/* xdg-desktop-portal 的 GTK / KDE 后端需要的协议。 */
bool vidow_portal_init(struct vidow_server *server);

/* xdg-activation-v1（见 activation.c）。在 portal 初始化之前调：窗口规则 /
 * 激活请求这条链路跟 portal 无关，portal 只是碰巧也需要它。 */
void vidow_activation_init(struct vidow_server *server);
/* 配置落地后调：按 activation_token_timeout 重设扫过期令牌的定时器。 */
void vidow_activation_apply_config(struct vidow_server *server);
/* 窗口刚建 / 将销毁：维护窗口列表 handle。 */
void vidow_portal_view_new(struct vidow_view *view);
void vidow_portal_view_free(struct vidow_view *view);
/* 标题、app_id、父子关系变了。 */
void vidow_portal_view_title(struct vidow_view *view);
void vidow_portal_view_parent(struct vidow_view *view);
/* 上屏 / 下屏：窗口列表靠它判断这个窗口在不在某个输出上。 */
void vidow_portal_view_mapped(struct vidow_view *view, bool mapped);
/* 焦点变了。view 可以是 NULL（焦点落到了 layer surface 或没有窗口）。 */
void vidow_portal_view_activated(struct vidow_view *view, bool activated);

/* -------------------------------------------------------------- portalconf */

/* 把 XDG_CURRENT_DESKTOP 设成配置里的 portal_desktop。要在任何子进程之前调，
 * 否则配置里启动的客户端看不到。 */
void vidow_portal_desktop_env(const struct vidow_config *cfg);
/* 同步 portals.conf 到 portal 自己的配置目录；内容变了才写，写完异步重启
 * xdg-desktop-portal（先 import-environment，否则后端连的还是宿主合成器）。 */
void vidow_portal_conf_sync(const struct vidow_config *cfg);

#endif /* VIDOW_SERVER_H */
