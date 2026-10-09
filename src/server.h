#ifndef VIDOW_SERVER_H
#define VIDOW_SERVER_H

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include "scale.h"
#include "outputconf.h"
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
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
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
struct vidow_jelly;

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
	/* 阻尼振荡：冲过终点再回摆一下收住（见 anim.c）。窗口的「q 弹」就是它。 */
	VIDOW_EASE_OUT_ELASTIC,
};

/* Alt+Tab 切换器（见 mru.c）的两个枚举：配置里写字符串
 * （scope 写 "all" / "workspace" / "output"，layout 写 "strip" / "grid"），
 * 落到这里就是整数。 */
enum vidow_mru_scope { VIDOW_MRU_ALL, VIDOW_MRU_WORKSPACE, VIDOW_MRU_OUTPUT };
enum vidow_mru_layout { VIDOW_MRU_STRIP, VIDOW_MRU_GRID };

struct vidow_config {
	/* ---- gap ----
	 * 一个统一值管全部：`gaps = 10` → 屏幕边 10px、窗口之间也是 10px。
	 * 要分开写就 `gaps = { top=30, left=40, inner=20 }`（或者平铺键
	 * gaps_top / gaps_inner…），没写的那几项跟着统一值走。
	 * 都按逻辑像素算；统一值负的一律当 0（负 gap 会让工作区比屏幕还大），
	 * 但单独设置的 -1 不是「负值」，是「跟随统一值」。 */
	int gaps;                 /* 统一值：四边与窗口之间都用它 */
	int gaps_top;             /* 四边单独设「屏幕边 → 窗口」，-1 = 跟随 */
	int gaps_bottom;
	int gaps_left;
	int gaps_right;
	int gaps_inner;           /* 窗口 ↔ 窗口（列与列、同列上下），-1 = 跟随 */
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

	/* 每个阶段自己的缓动曲线：默认 out_elastic —— 入场和关闭会过冲一下再
	 * 收住。上面那个 easing 只管位移补间、layer surface 和 overview：布局重排
	 * 要的是「快而稳」，过冲反而晃眼。 */
	enum vidow_easing open_easing;
	enum vidow_easing close_easing;
	/* 入场从多小弹出来 / 关闭缩到多小（1.0 = 不缩放）。 */
	double open_scale_from;
	double close_scale_to;
	/* 拖动窗口时的果冻（见 jelly.c）：抓点跟着光标走，窗口其余部分像一块布
	 * 一样被拖着走、松手后晃两下收住。jelly_strength 是形变倍率（0.5 = 收
	 * 敛些，0 = 不弯）。 */
	bool jelly;
	double jelly_strength;

	/* server-side decorations */
	bool decor_enabled;
	/* 让客户端别画自己的装饰（CSD）：装饰协商一律回 SERVER_SIDE，并且建
	 * org_kde_kwin_server_decoration_manager 让只认老协议的客户端（GTK3）也
	 * 听得见 —— 见 decoration.c。*/
	bool prefer_no_csd;
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
	/* 「X 光」：重叠时也用整屏那份底衬（壁纸 + layer 那层），**忽略别的窗口** ——
	 * Hyprland 的 blur:xray 就是这条。观感上是「糊的是壁纸，不是压着的那个窗口」，
	 * 换来的是不建私有底衬：重叠场景下每帧从 N×(1+2·levels−1) 趟降到 ~3 趟，
	 * 每个元素的私有底衬显存也全免。默认关（真背景模糊才是这个合成器的卖点）。 */
	bool blur_xray;
	int blur_levels;      /* 2..4 downsample steps */
	double blur_strength; /* 0 = sharp, 1 = fully blurred */
	/* 透明度剔除（见 fx.c 的 decor_fragment_src）：元素**自己**的 alpha 低于这个
	 * 就不糊。管的是模糊的**范围**（哪儿该糊），不碰糊出来的像素 —— 底衬不做剔除。 */
	double blur_alpha_min;
	double blur_offset;    /* kawase tap scale */

	bool shader_enabled;
	char shader_decor[64]; /* "" = built-in decoration shader */
	char shader_open[64]; /* "" = built-in fade/slide only */
	char shader_close[64];
	char shader_idle[64]; /* applied permanently while the window lives */
	char shader_dir[PATH_MAX];
	bool shader_flip_y;   /* sample the source texture bottom-up */

	/* 缩放（见 scale.c）。scale 是所有输出的默认策略："auto" = 按 EDID 的 ppi
	 * 推，让同一个逻辑尺寸在两块不同 DPI 的屏上是同一个物理尺寸。单块屏要
	 * 点名，用配置里的 vidow.output{ name = "DP-1", scale = 2 }（见
	 * outputconf.c）—— 缩放和摆在哪是同一块屏的两件事，写在一组里。 */
	char scale[16];

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

	/* 输入注入（见 input.c 的「注入」段 + ipc.c）：IPC 的 move-cursor /
	 * pointer-button / pointer-scroll / send-key / type-text 能合成鼠标和键盘
	 * 事件，等于「谁连上 socket 谁就能操控整个桌面」。默认开 —— socket 是
	 * 0600（只有本用户能连），而状态栏 / 脚本 / 自动化本来就靠 IPC；每次注入
	 * 都会打一条 info 日志留痕。关掉之后只剩 vidow 自己的快捷键能动。 */
	bool input_injection;

	/* 摇晃找光标（见 cursorfx.c）：快速来回晃几下指针，指针就地放大一会儿。
	 * 放大是换一张更大的光标图，走的是硬件光标那条路 —— 放得下就不进合成器
	 * 的渲染通道，所以不会出现在截图里；放不下会被 wlroots 降级成软光标，所以
	 * 倍数会按硬件上限先夹一下（cursor_shake_scale 是想要的倍数）。 */
	bool cursor_shake;
	double cursor_shake_scale;

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
	/* 录屏 / 截图交给哪个后端：auto（默认，自己人优先）/ vidow / kde / wlr /
	 * hyprland / none。跟 portal_desktop（会话名）是两件事。 */
	char portal_capture[32];

	/* Alt+Tab 切换器（niri 的 recent windows，见 mru.c）。
	 *   enabled        总开关：关掉就不记账、Alt+Tab 不认（那两条绑定变成没动作）
	 *   debounce_ms    焦点落到台账上之前要「待够」多久 —— 键盘一路走过去碰到的
	 *                  窗口不该算「用过」（0 = 立刻落账）
	 *   open_delay_ms  按住多久面板才显形：快速点一下 Alt+Tab 只想切窗口，
	 *                  不该整屏闪一下（0 = 立刻显形）
	 *   scope          列哪些窗口：all（所有屏所有工作区）/ output（当前这块屏）
	 *                  / workspace（当前工作区）
	 *   layout         strip = 一行，装不下横向滚；grid = 不滚，换行铺在屏幕里
	 *   per_row        grid 一行最多几个（strip 用不上）
	 *   gap/row_gap    预览之间 / 行与行之间（逻辑像素）
	 *   max_scale      预览最多缩到多少（0.5 = 缩一半；1.0 = 原尺寸）
	 *   max_height     预览高度上限（大屏上别一个窗口占满屏）
	 *   min_scale      再挤也不能比它更小
	 *   backdrop_alpha 切换器后面那层压暗的浓度
	 *   highlight_*    选中项背后那块板 / 边框 */
	bool mru_enabled;
	int mru_debounce_ms;
	int mru_open_delay_ms;
	enum vidow_mru_scope mru_scope;
	enum vidow_mru_layout mru_layout;
	int mru_per_row;
	int mru_gap;
	int mru_row_gap;
	double mru_max_scale;
	int mru_max_height;
	double mru_min_scale;
	double mru_backdrop_alpha;
	int mru_highlight_padding;
	int mru_highlight_border;
	float mru_highlight_color[4];
};

/* gap 的唯一算法：单独设了就用它，没设（-1）跟随统一值 gaps；其余负值当 0
 * （负 gap 会让工作区比屏幕还大，布局直接散架）。视图和日志都用这两个，别各写
 * 一遍。 */
#define VIDOW_GAP_FOLLOW (-1)

static inline int vidow_gap_resolve(int value, int fallback) {
	if (value == VIDOW_GAP_FOLLOW) {
		value = fallback;
	}
	return value > 0 ? value : 0;
}

/* 屏幕边 → 窗口：side 是该边单独设的值（-1 = 跟随）。 */
static inline int vidow_gap_side(const struct vidow_config *cfg, int side) {
	return vidow_gap_resolve(side, cfg->gaps);
}

/* 窗口 ↔ 窗口（列与列、同列上下）。 */
static inline int vidow_gap_inner(const struct vidow_config *cfg) {
	return vidow_gap_resolve(cfg->gaps_inner, cfg->gaps);
}

/* --------------------------------------------------------------- animation */

/* One running animation. Progress comes from CLOCK_MONOTONIC. */
struct vidow_anim {
	struct wl_list link; /* vidow_server.anims */
	struct vidow_server *server;
	int64_t start_ns;
	int64_t duration_ns;
	const char *tag; /* for logs */
	enum vidow_easing easing;
	/* t = 曲线值（可能过冲，位移/缩放用它），linear = 线性进度（单调，透明度用它）*/
	void (*update)(void *data, double t, double linear);
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
	int u_tex, u_texel, u_offset;
	/* kawase 专用：只画目标里的一块（u_rect，目标像素）时的目标尺寸/矩形。 */
	int u_rect, u_dst;
};

/* 任意四边形网格（jelly.c 的果冻形变用）：顶点直接给「渲染目标物理像素坐标 +
 * 源贴图归一化 UV」，片元就是采样。 */
struct vidow_mesh_prog {
	unsigned int program;
	int u_texture, u_size;
};

/* The built-in decoration program: rounded corners, border, drop shadow,
 * blurred backdrop and grain. */
struct vidow_decor_prog {
	unsigned int program;
	int u_texture, u_blur, u_size, u_target_size, u_pad;
	int u_radius, u_border_width, u_border_color, u_shadow_color;
	int u_shadow_offset, u_shadow_radius, u_opacity, u_noise, u_alpha;
	int u_origin, u_output_size, u_flip_y;
	int u_blur_strength;
	int u_blur_region, u_blur_region_on;
	/* 内容贴图在装饰矩形里的采样映射（见 fx.c 的 decor_fragment_src）。 */
	int u_tex_map;
	/* 元素自己的 alpha（「透明度剔除」，见 fx.c 的 decor_fragment_src）。 */
	int u_mask, u_mask_on, u_mask_min;
};

/* One kawase pyramid. The compositor keeps a shared one for the wallpaper
 * (captured with every toplevel hidden) plus a private one per window that
 * actually overlaps another window, because that window's backdrop has to
 * keep its neighbour in it. */
/* kawase 金字塔的级数上限。每多一级模糊半径（原始像素）翻一倍：1080p 上 4 级
 * 半径 ~14px、6 级 ~56px。niri 的 passes 是同一个意思（那边允许到 31）。
 * 数组尺寸也跟着它走，改这儿就够，别再用字面量 4。 */
#define MAX_BLUR_LEVELS 6

struct vidow_blur_set {
	struct wlr_buffer *down[MAX_BLUR_LEVELS];
	struct wlr_buffer *up[MAX_BLUR_LEVELS];
	/* 每块缓冲配一张**常驻**贴图（跟缓冲同生共死）。链条每一趟都要把上一趟的
	 * 结果当源采样：以前是每趟 wlr_texture_from_buffer() 导入一张、用完销毁，
	 * 真后端上（GBM/dmabuf）那是一次 EGLImage + BO 映射/解映射 —— 实测拖一个
	 * 与别的窗口重叠的窗口时每帧 16~23 次。缓冲复用着，贴图一起复用就行。 */
	struct wlr_texture *down_tex[MAX_BLUR_LEVELS];
	struct wlr_texture *up_tex[MAX_BLUR_LEVELS];
	int w, h, levels;
	uint64_t gen;  /* scene generation this pyramid was built for */
	double offset; /* kawase 采样步长，建这张金字塔时用的 */
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

/* Alt+Tab 切换器（niri 的 recent windows）。台账和切换器都在 mru.c，这里只放
 * 状态：台账一个 pending + 一个 debounce 闹钟；切换器一张清单（order）、一份
 * 排版（slot）、选中项和面板那几个矩形。 */
struct vidow_mru_slot {
	struct vidow_view *view;
	int x, y, w, h;   /* 预览摆在哪（布局坐标）+ 尺寸，都是屏幕上的值 */
	double zoom;      /* 内容缩放（1.0 = 原尺寸） */
};

struct vidow_mru {
	/* ---- 台账 ---- */
	struct vidow_view *pending;     /* 拿到焦点、还在 debounce 里的那个 */
	struct wl_event_source *timer;  /* debounce 到点 */
	/* ---- 切换器 ---- */
	bool on;                        /* 按住修饰键期间：键盘被接管，焦点先不动 */
	bool shown;                     /* 面板显形了（open_delay 到了）：画面才动 */
	struct vidow_view *selected;
	struct vidow_view *original;    /* 进来之前焦点在哪 */
	struct wlr_output *output;      /* 在哪块屏上摆 */
	struct wl_event_source *open_timer; /* open_delay 闹钟 */
	char filter_app_id[128];        /* 只看这个 app 的窗口（空 = 全部） */
	enum vidow_mru_scope scope;     /* 这次列哪些窗口（配置给默认，绑定能覆盖） */
	struct vidow_view **order;      /* 清单（最近用过在前），MRU_MAX 个 */
	int n;                          /* 清单里有几个 */
	struct vidow_mru_slot *slot;    /* 排版结果，slot_n 个 */
	int slot_n;
	double scroll;                  /* strip 模式横向滚了多少 */
	int total_w, total_h;           /* 整条清单占多大（日志 / 断言用） */
	struct wlr_scene_rect *backdrop; /* 压暗背景 */
	struct wlr_scene_rect *plate;    /* 选中项背后那块板 */
	struct wlr_scene_rect *border[4];/* 选中项的边框（四条细矩形） */
};

/* 切换器里的窗口这一帧该怎么摆（view.c 的 view_anim_place 问它）。 */
enum vidow_mru_place {
	VIDOW_MRU_PLACE_NONE,    /* 不归切换器管：照常走布局 / overview */
	VIDOW_MRU_PLACE_PREVIEW, /* 摆到预览的位置上（x/y/zoom 已填好） */
	VIDOW_MRU_PLACE_HIDE,    /* 切换器开着、但它不在清单里：收起来 */
};

/* A window's content rendered through the decoration (and optionally an
 * animation shader) into offscreen buffers, then shown as a scene node.
 * Buffers are ping-ponged so we never draw into one the scene still holds. */
/* 「可见窗口」在源缓冲里的位置和尺寸，逻辑单位 —— 就是 niri 的
 * LayoutElement::size() / buf_loc() 那一对：
 *
 *   off_x/off_y  可见窗口左上角在源缓冲里的偏移（客户端自己画装饰时留的阴影边，
 *                没有留白就是 0）
 *   w/h          可见窗口尺寸（= xdg_surface.set_window_geometry 那块）
 *   src_w/src_h  整块源缓冲的尺寸
 *
 * 布局、边框、圆角、阴影一律按「可见窗口」算，源缓冲按 -off 反向偏移渲染。客户端
 * 的留白永远落在窗口外面，边框紧贴窗口，中间不可能再有缝。
 *
 * tex_u0/tex_v0/tex_du/tex_dv 是同一件事在**源贴图里的归一化坐标**（0..1）：
 * u0/v0 是可见窗口左上角、du/dv 是它占的比例。上面那几个整数是逻辑单位，贴图却
 * 是「客户端缓冲像素」—— 两者只有在「缓冲 = 逻辑尺寸 × 整数倍、且整块都有效」时
 * 才一一对应。客户端用 wp_viewport 声明了有效区域（chromium / KDE 在分数缩放下会
 * 把缓冲开得比窗口大一圈、只画其中一块）时就不对应了：按逻辑整数算出来的是「整块
 * 缓冲」，右边/下边那圈没画过的像素会一起铺进窗口，屏幕上就是「框到位了、画面小
 * 一圈、周围一条黑边」。所以贴着贴图采的那条路（装饰着色器）用这四个浮点数，
 * 逻辑单位那几个继续给布局、命中、关闭快照用。全 0 = 没算（按老公式退回去）。 */
struct vidow_fx_geo {
	int off_x, off_y;
	int w, h;
	int src_w, src_h;
	float tex_u0, tex_v0, tex_du, tex_dv;
};

/* 攒下来的损伤区域（布局逻辑坐标）：留几个盒子，装不下了才合并 —— 两个客户端在
 * 同一帧里各动窗口的一角时，单个包围盒会把中间那一大片没变的地方也算成损伤
 * （见 fx.c 的 dmg_add）。4 个够用了：一帧里真能撞到同一张底衬的损伤笔数就那么多。 */
#define VIDOW_DMG_BOXES 4
struct vidow_dmg_set {
	struct wlr_box box[VIDOW_DMG_BOXES];
	int n;
};

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
	/* 动画缩放，围绕窗口中心（1.0 = 原尺寸）。缩放只作用在场景节点上，离屏
	 * 缓冲照旧按原尺寸画 —— 为了几个百分点的过冲每帧重分配一块缓冲不值。 */
	double anim_scale;
	/* animation pass, only used when a shader is configured */
	struct wlr_buffer *anim_buf[2];
	int anim_index;
	struct vidow_shader *shader;
	enum vidow_shader_phase phase;
	double progress;
	int64_t start_ns;

	bool active;  /* decoration or shader is in use */
	bool blur;    /* sample the blurred backdrop */

	/* 越界裁剪（见 view.c 的 vidow_view_apply_clip）：这块窗口只画自己那块屏
	 * 里的那部分。clip = 那块屏的框（布局逻辑坐标），clip_on 为 false 时整块
	 * 都画。裁剪只作用在**显示**上：几何、客户端、录屏抓帧都不动。 */
	bool clip_on;
	struct wlr_box clip;
	/* 没装饰（着色器也关着）时窗口是客户端表面自己在画，那一份裁的是表面树
	 * （wlr_scene_subsurface_tree_set_clip，见 view.c）。存的是**上一次真的
	 * 调进场景**的那个局部框：动画帧里窗口每帧都在动，只按 clip_on 判断会漏
	 * —— 裁出来的是上一帧位置的那一块。宽 0 = 没裁。 */
	struct wlr_box clip_local;

	/* Blur backdrop. Either the server's shared wallpaper pyramid (borrowed)
	 * or a private one captured with this window taken out (owned). The
	 * texture only changes when the backdrop really changed, so a still
	 * desktop costs no blur and no redraw at all. */
	struct wlr_texture *blur_tex;     /* may be NULL */
	bool blur_owned;                  /* we must destroy blur_tex */
	uint64_t blur_tex_id;             /* identity of blur_tex, 0 = none */
	uint64_t blur_ver;                /* bumped whenever blur_tex changes */
	uint64_t blur_drawn;              /* version baked into the current buffer */
	float mask_min_used;              /* 当前缓冲是按哪个剔除阈值画的 */
	struct wlr_texture *blur_own_tex; /* texture of the private pyramid */
	uint64_t blur_own_id;             /* identity of blur_own_tex */
	struct vidow_blur_set blur_set; /* private pyramid, allocated on demand */

	/* 这张模糊贴图盖住输出的哪一块（设备像素，相对输出原点）。窗口用的永远是整屏
	 * 那张（blur_tex_w == 0 就按整屏算）；会被窗口遮住的 layer surface 用的是
	 * **它自己那一小块区域**的抓取，采样坐标得减掉区域原点，否则整块错位。 */
	int blur_tex_x, blur_tex_y, blur_tex_w, blur_tex_h;

	/* 私有底衬**那一小块区域**的缓冲 + 包着它的贴图（只有重叠的窗口用；layer
	 * 那一路把同样的东西挂在 struct vidow_layer_surface 上）。位置和尺寸就是
	 * 上面那组 blur_tex_*。抓的时候只渲染这块区域：基座从共享底衬上 1:1 拷一块
	 * 过来，别的窗口再画上去 —— 以前每帧要把壁纸整个重画一遍（见 view_region_build）。
	 * 贴图是包的缓冲、不是包的像素，缓冲复用的时候贴图跟着复用。 */
	struct wlr_buffer *own_bd;
	struct wlr_texture *own_bd_tex;
	int own_bd_w, own_bd_h;

	/* 上一次重建之后攒下来的损伤（**布局逻辑坐标**；n == 0 = 没有；「整屏都算」
	 * 时里面是一个巨大的盒子）。世界代次只是粗筛：别的窗口在屏幕另一头重画
	 * （视频 / 浏览器）不该让我这张底衬重抓，真正的判据是「这次损伤有没有碰到
	 * 我这一块」（见 blur_note_damage / blur_damage_hits）。
	 *
	 * 存的是**几个盒子**不是一个包围盒：两个客户端同一帧里各动窗口的一角，
	 * 包围盒会把中间没变的那一大片也算成损伤（见 fx.c 的 dmg_add）。 */
	struct vidow_dmg_set pending_damage;

	/* ext-background-effect 的模糊区域（窗口局部坐标）。blur_region_on 为
	 * true 时着色器只在这个盒子里画模糊；-1 表示不限制。 */
	bool blur_region_on;
	float blur_region[4]; /* x, y, w, h */

	/* 内容贴图在装饰矩形里的采样映射（归一化）：uv = tex_off + 内容坐标 *
	 * tex_scale。窗口几何比表面小一圈时（客户端 CSD 留白），内容就按这份偏移
	 * 采；没留白时 0 / 1（layer 底衬、X11 永远是这个）。 */
	float tex_off_x, tex_off_y, tex_scale_x, tex_scale_y;

	/* 只画「背后的模糊」：layer surface 的底衬用它。内容和阴影/边框/圆角/噪点
	 * 全部关掉，输出就是一层模糊，垫在 layer surface 自己的节点下面。 */
	bool blur_only;

	bool hidden;  /* temporarily disabled for the backdrop capture */

	/* 抓帧遮挡（窗口规则 capture = "hide"）：这个 fx 节点正被压成透明 —— 抓帧
	 * 的时候不出现，屏幕上照旧。vidow_fx_frame 每帧都会把节点透明度写回 1.0，
	 * 所以遮挡必须是它认的一个标志位，而不是外面戳一下（见 capturemask.c）。 */
	bool masked;
	/* 正在被拖出果冻形变（见 jelly.c）：装饰结果由果冻那个节点显示，这个节点
	 * 透明度压 0 让位。同样必须是标志位（每帧都会被写回去）。 */
	bool jelly;
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
	double anim_scale_to;   /* 关闭动画结束时的缩放（1.0 = 不缩） */
	enum vidow_easing easing;
	int x, y, w, h;
	struct vidow_fx_geo geo; /* 快照里哪一块是「可见窗口」（见上） */
	double scale;
};

/* ------------------------------------------------------------------ output */

struct vidow_output {
	struct wl_list link; /* vidow_server.outputs */
	struct vidow_server *server;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	struct wlr_output_layout_output *layout_output;
	/* 这块屏的抓帧器（capture.c 的 vidow_capture）：独立的假输出 + 自己的
	 * scene_output，抓帧时自己渲染一帧。懒建。 */
	struct vidow_capture *capture;

	struct wlr_box usable_area; /* layout coords, minus layer exclusive zones */
	/* 上次已知的布局位置（逻辑坐标）。拔线时 wlr_output_layout 可能已经先把这块
	 * 输出忘掉了，热插拔要靠这份存着的位置算「挪到别的屏」的平移量。 */
	struct wlr_box box;
	/* 这块屏的热插拔记忆（按输出名认）。在 new_output 里就取好存下：
	 * 拔线时 wlroots 已经把 wlr_output->name 收走了，那时候再按名字找就晚了。 */
	struct vidow_output_memory *memory;
	double viewport_x;          /* horizontal scroll offset of the column strip */
	/* 上一次在这块屏上亮着的工作区（focus-workspace-previous / auto-back-and-forth） */
	uint64_t prev_ws_id;
	int64_t last_frame_ns;      /* last frame event, for the animation watchdog */
	/* 上一次**真正出帧**的时刻（后端给的帧事件，或者看门狗自己推的那一帧）。
	 * 看门狗靠它给自己限速：跟后端**自己**的帧间隔走，比它晚 1/4 才补一帧。
	 * 不并进 last_frame_ns —— 那个是「后端最后一次给帧」，帧追踪
	 * （VIDOW_TRACE_FRAMES）拿它判「帧链断没断」，看门狗推的帧不能算进去，
	 * 不然帧链断着也能骗过追踪。 */
	int64_t last_render_ns;
	/* 后端帧间隔的滑动平均（只喂「看着正常」的那些间隔，见 output_frame）：
	 * 看门狗的目标节奏就是它，不用写死 40ms。0 = 还不知道。 */
	int64_t frame_gap_ns;
	/* 帧追踪（VIDOW_TRACE_FRAMES=1 时才有意义）：这块屏被要过帧、要了几次、
	 * 最近一次是谁要的。**要了帧却迟迟没有帧事件**就是「这块屏的帧链断了」——
	 * 用户报过「某块屏上的窗口停在最后一帧、拖到另一块屏就活」，只有真 DRM 上
	 * 才会出现，靠这三个字段 + trace 定时器能一眼分开是后端没给帧还是 vidow
	 * 自己没画。默认不开，正常路径只有一次赋值。 */
	bool want_frame;
	int want_frame_n;
	const char *want_frame_why;
	/* 客户端最近一次交帧（view_commit 那一路）。**它比 last_frame_ns 新**而这块
	 * 屏又迟迟不出帧 = 客户端在画、屏幕不更新，正是用户报过的「停在最后一帧」。 */
	int64_t content_ns;


	/* Backdrop snapshot: the scene without the blurred windows, used as the
	 * source for the kawase chain. */
	struct wlr_buffer *backdrop;
	int backdrop_w, backdrop_h;
	/* backdrop 的贴图版本。被窗口遮住的 layer surface 要给它自己那一小块区域
	 * 拼底衬（基座 + 窗口），基座就是从这上面 1:1 拷过去的一块 —— 比每次重抓
	 * 一遍壁纸便宜太多。跟着 backdrop 一起重建。 */
	struct wlr_texture *backdrop_tex;

	/* Wallpaper-only blurred backdrop for this output: the scene below the
	 * toplevels with every window hidden, so it survives client repaints,
	 * window moves and viewport scrolling. */
	struct wlr_texture *blur_tex;
	struct vidow_blur_set blur_set;
	uint64_t blur_id;   /* identity of blur_tex, 0 = none */
	uint64_t blur_gen;  /* gen_layer the pyramid was captured at */
	int blur_levels;
	/* 建这张金字塔时用的采样步长：改 blur_offset 也得重算（它是 kawase 着色器的
	 * uniform，缓存的贴图不会跟着变）。blur_alpha_min 现在只进装饰着色器、不进
	 * 金字塔，改它由 vidow_fx 那边直接让帧循环重画（见 blur_mask_min_changed）。 */
	double blur_offset;

	/* 抓帧遮挡：这块屏上一次看到的抓帧状态。只在翻转时记一行日志 —— 这个功能
	 * 平时完全看不见，出问题时日志是唯一的线索（见 capturemask.c）。 */
	bool capture_active;

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
	/* 平铺窗口的 Mod+左键拖拽（src/drag.c）：位置只动场景节点，松手才决定归哪 */
	VIDOW_GRAB_TILE,
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
	/* niri 式「列最大化」：整列撑满工作区宽度（width_fraction 顶到 1.0），再按一次
	 * 还原成原来那个比例（记在 maximized_saved 里）。手动改宽度（cycle-column-width
	 * / set-column-width / expand-column-to-available-width）就是退出这个状态 ——
	 * 状态的唯一入口是 vidow_column_set_maximized()（见 view.c）。 */
	bool maximized;
	double maximized_saved;
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
	/* 「只有这个窗口自己变了」的累计代次（拖动 / 内容重画 / 开合动画 / 果冻
	 * 形变，见 vidow_backdrop_damage_view）。私有底衬抓的是「世界减掉这个窗口」，
	 * 抓的时候这个窗口本来就藏着，所以它自己的变化**不影响**那张图：判断要不要
	 * 重抓时看的是 server->gen_scene - self_gen，而不是 gen_scene 本身。 */
	uint64_t self_gen;
	/* 最后一次**真的发出去**的 configure（xdg 只看宽高；X11 的移动就是 configure，
	 * 所以连位置一起记）。一样的就不再发：拖动时每动一下都会 arrange 一次，无条件
	 * 发的话每个窗口的客户端都要重画一遍、交一帧 —— 实测拖 200 步 = 每个窗口
	 * 200 次 configure + 200 次 commit，底衬跟着每帧重抓（见 vidow_view_set_size）。 */
	int cfg_x, cfg_y, cfg_w, cfg_h;
	bool cfg_sent;
	/* 同理，最后一次真的发出去的 tiled 状态（place_view 每次都调 set_tiled，
	 * 不去重的话每次都会给客户端排一个 configure）。 */
	int cfg_tiled;
	bool cfg_tiled_sent;
	/* 同理，最后一次真的发出去的 maximized（列最大化时列里每个窗口都要收到一个
	 * set_maximized 应答，arrange / map 会反复调到那儿）。 */
	bool cfg_maximized;
	bool cfg_maximized_sent;
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
	/* 「最近用过」的时间戳（单调时钟 ns；0 = 还没记过）。Alt+Tab 的切换器
	 * （mru.c）现按它排一份清单 —— 不另维护链表，窗口开关 / 换工作区 / 销毁
	 * 都不用同步第二份数据。 */
	int64_t mru_ts;
	bool configured; /* initial configure already answered */
	bool fullscreen;
	/* 最小化：窗口**留在原来的列里**（列宽、上下顺序、滚动位置、归属工作区和屏
	 * 全都不用重建，放回来是瞬时的），但不占位、不显示、不参与焦点 —— 布局、
	 * 焦点遍历、可见性都会跳过它（见 view.c 的 view_shows()）。
	 *
	 * 规则 force_render 的窗口最小化之后**照样补帧**（录屏 / 预览要的是新画面）：
	 * 补帧那条路的判据是「没画在画面上」，最小化正好让 view_on_screen() 变假，
	 * 于是它进补帧分支 —— 别在这里加「最小化就不补帧」的判断。
	 * minimized_seq：什么时候收起来的（单调递增），「放回最近收起来的那个」按它挑。 */
	bool minimized;
	uint64_t minimized_seq;
	/* 「这次列最大化是客户端自己要的」（xdg 的 set_maximized / X11 的
	 * _NET_WM_STATE_MAXIMIZED_*）：只有它自己再请求撤销时才理 —— 用户按
	 * Mod+Shift+F 要来的最大化，客户端随手发一个 set_maximized(false) 撤不掉
	 * （见 view.c 的 view_request_maximize）。 */
	bool maximize_granted;
	/* 滚出自己那块屏 / 所在工作区藏起来：**不关场景节点**，而是把它停到所有输出
	 * 框外面的「停车位」（见 view.c 的 view_park_pos、view_anim_place 里的用法）。
	 *
	 * 为什么不用 wlr_scene_node_set_enabled(false)（以前就是这么干的）：按窗口录屏
	 * 和选择器里的预览走的是 wlroots 的窗口截图源，它渲染的就是这棵场景子树 ——
	 * 节点一关，录到 / 预览到的就是一片黑（「滚出视口的窗口在 portal 里没预览」
	 * 就是这么来的）。停到输出框外面：哪块屏的框里都没有它，屏幕上照样看不见；
	 * 截图源那个虚拟输出是按节点自己的位置摆的（见 wlroots 的
	 * ext_image_capture_source_v1/scene.c），节点停哪儿它跟到哪儿，录到的是完整
	 * 窗口。 */
	bool scene_parked;
	/* 帧追踪用：上一次记 commit 的时间（VIDOW_TRACE_FRAMES=1 才写） */
	int64_t trace_commit_ns;

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
	/* 抓帧遮挡（窗口规则 capture，机制见 capturemask.c）：有人在截图 / 录屏时，
	 * 把它在场景里的透明度压成 0 —— 屏幕上照旧、抓到的帧里没有它。
	 * 不用 node->enabled：场景的命中测试会跳过关掉的节点，窗口就点不着了（输入和
	 * vidowmsg view-at 都会走错）。透明度只影响合成，命中测试不看它。
	 * mask_opacity 记着「现在正压着」，两个 saved 是压之前的值（fx 节点和表面节点
	 * 的透明度本来就不一样：装饰画的时候表面是 0）。 */
	int capture_mode;   /* enum vidow_capture_mode */
	int capture_mosaic; /* 马赛克块边长（屏幕像素） */
	bool mask_opacity;
	float mask_saved_fx;
	float mask_saved_surf;
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
	/* 这个窗口的 X11 缩放桥（窗口规则 x11_scale 给的，0 = 跟随全局
	 * xwayland_scale）。缓冲区按它除才是布局里的逻辑尺寸，命中坐标也按它乘回去
	 * —— 认 GDK_SCALE 的客户端要除、不认的不能除，只有用户知道是哪种，所以逐
	 * 窗口可写。见 vidow_view_x11_scale_int()。 */
	double x11_scale;
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
	struct wl_listener x11_request_minimize;
	struct wl_listener x11_destroy;

	/* animation */
	struct vidow_anim *anim_move; /* position tween */
	struct vidow_anim *anim_open; /* open tween */
	double anim_off_x, anim_off_y;  /* added to geom.x/y while animating */
	double anim_alpha;              /* 0..1, applied to every buffer */
	double anim_scale;              /* 动画缩放，围绕窗口中心；1.0 = 原尺寸 */
	bool dragging;                  /* 正被鼠标拖着（位置归 drag.c 管） */
	int drag_dx, drag_dy;           /* 抓点在窗口里的位置 */
	double anim_from_x, anim_from_y; /* move tween start offsets */
	bool placed;                     /* laid out at least once */

	/* custom shader output */
	struct wlr_scene_surface *scene_surface;
	struct vidow_fx_target fx;
	int fx_dx, fx_dy; /* surface offset inside the view tree */
	struct wlr_buffer *last_buffer; /* locked, feeds the close animation */
	/* 拖动时的果冻形变（见 jelly.c）：非空就是「现在正弯着」——窗口本来的
	 * 装饰节点让位，由它显示。松手后它自己晃回去再消失。 */
	struct vidow_jelly *jelly;

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
	struct wl_listener request_minimize;

	/* 窗口列表（waybar / xdg-desktop-portal 的 ScreenCast 窗口选择器） */
	struct wlr_foreign_toplevel_handle_v1 *ftm_handle;
	struct wlr_ext_foreign_toplevel_handle_v1 *ftl_handle;
	/* wlroots 不判断状态是否真的变了，重复 set_activated 会白刷一遍客户端 */
	bool portal_activated;
	bool portal_entered;
	struct wl_listener ftm_request_activate;
	struct wl_listener ftm_request_close;
	struct wl_listener ftm_request_fullscreen;
	struct wl_listener ftm_request_minimize;
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
	/* 抓底衬时这个 surface 被我们临时藏起来过（抓完要还原，见 fx.c）。 */
	bool capture_hidden;
	/* 抓帧遮挡（规则 capture = 写了 namespace 的那种，见 capturemask.c）：
	 * 和上面那个 capture_hidden 不是一回事 —— 那个是模糊底衬的临时藏法。 */
	int capture_mode;   /* enum vidow_capture_mode */
	int capture_mosaic; /* 马赛克块边长（屏幕像素） */
	bool mask_opacity;
	float mask_saved_fx;
	float mask_saved_surf;

	/* 它自己那一小块区域的底衬抓取（含窗口）：基座是共享底衬里对应的那块，窗口
	 * 另外画上去（见 fx.c 的 layer_region_build）。只有会被窗口遮住的层（top /
	 * overlay）才需要 —— bottom 层的下面根本没有窗口。 */
	struct wlr_buffer *bd;
	/* 包着 bd 的常驻贴图（跟缓冲同生共死，见 fx.c 的 layer_bd_drop）：链条拿它
	 * 当第一趟的源，复用缓冲就不用每趟重新导入。 */
	struct wlr_texture *bd_tex;
	int bd_w, bd_h;      /* 设备像素 */
	int bd_x, bd_y;      /* 相对输出原点的位置，设备像素 */
	uint64_t bd_gen_scene, bd_gen_layer; /* 抓的时候的世界代次 */
};

/* ---------------------------------------------------------------- keyboard */

struct vidow_keyboard {
	struct wl_list link; /* vidow_server.keyboards */
	struct vidow_server *server;
	struct wlr_keyboard *wlr_keyboard;
	/* 虚拟键盘（wtype、输入法回注）的来源，物理键盘为 NULL */
	struct wlr_virtual_keyboard_v1 *virtual;
	/* vidow 自己造的注入键盘（IPC 的 send-key / type-text 用它按键），
	 * 不是真设备也不是客户端来的 */
	bool inject;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

/* 客户端要的一个「快捷键抑制」（zwp_keyboard_shortcuts_inhibitor_v1）。一个表面
 * 一个，见 keyboardgrab.c —— 抑制只在它的表面拿着键盘焦点时生效。 */
struct vidow_shortcuts_inhibitor {
	struct wl_list link; /* vidow_server.inhibitors */
	struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor;

	struct wl_listener destroy;
};

/* Xwayland 替 X11 的 XGrabKeyboard 要的一次键盘抓取
 * （zwp_xwayland_keyboard_grab_v1，协议服务端自己写的，见 keyboardgrab.c）。
 * 生效期间按键一律送给 surface（X11 语义：谁抓谁收），对象没了就还给焦点窗口。 */
struct vidow_xwayland_grab {
	struct wl_list link; /* vidow_server.xwayland_grabs */
	struct vidow_server *server;
	struct wl_resource *resource;
	struct wlr_surface *surface;
	bool linked; /* 还在 xwayland_grabs 名单上（摘过就别再摘，见 keyboardgrab.c） */

	struct wl_listener surface_destroy;
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

/* 临时探针（VIDOW_RENDER_STATS=1）：见 src/fx.c 里的同名结构 */
struct vidow_frame_stats {
	int shared, own, chains, bakes, tex_new, tex_del, frames, passes;
	int64_t blur_ns, decor_ns, jelly_ns, commit_ns, bake_ns;
	/* 探针：拖拽的「指针事件 -> 出帧」延迟 */
	int64_t grab_lat_ns, grab_lat_max_ns;
	int grab_lat_n;
};
extern struct vidow_frame_stats vidow_stats;

struct vidow_server {
	struct wl_display *display;
	struct wlr_backend *backend;
	/* wl_display_run() 已经返回了 = 正在拆。**拆的时候不能再处理设备事件**：
	 * wl_display_destroy() 会依次 finish 后端（键盘 / 输出），那些 finish 会再发
	 * 一轮事件进我们的监听器，而这时 seat / 工作区链已经在别处拆过一半了 ——
	 * 键盘那条会走到 wlr_seat_set_keyboard(已释放的 seat)（coredump：keyboard_key），
	 * 输出那条会走到工作区链上（coredump：vidow_workspace_count 读 link.next，
	 * si_addr=0x8，说明链上有个 NULL）。退出时这些状态本来就不用维护，直接不进。
	 * 见 main.c / input.c 的 keyboard_key / output.c 的 output_gone。 */
	bool shutting_down;
	/* 真机（tty）上才有：wlr_backend_autocreate() 建出来的会话。嵌套 / headless
	 * 是 NULL。切 VT 要它（wlr_session_change_vt），「现在轮到我画了吗」也看它
	 * （events.active，见 main.c 的 session_active）。 */
	struct wlr_session *session;
	struct wl_listener session_active;
	/* 要切的 tty 号 + 干这件事的 idle 源。切 VT 不能从键盘事件处理里直接做
	 * （那一进去就会拆掉所有输出、挂起输入设备），所以挂个 idle 延到这一轮
	 * 派发结束（见 keybind.c 的 vidow_vt_switch）。 */
	unsigned vt_pending;
	struct wl_event_source *vt_idle;
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

	/* 这台 Xwayland 认不认 _XWAYLAND_GLOBAL_OUTPUT_SCALE（补丁版才认）。认了
	 * X 屏幕才是设备像素；不认就得把 xwayland_scale 夹到 1（几何对、2 倍屏上
	 * 糊）。判据是量 X 屏幕的尺寸，见 xwayland.c 的探测。 */
	bool xwayland_scale_ok;
	bool xwayland_scale_probed;
	struct wl_event_source *xwayland_probe; /* 量 X 屏幕尺寸的定时器 */
	unsigned xwayland_probe_tries;
	/* 剪贴板桥（见 clipboard.c）：X11(Xwayland) <-> Wayland */
	struct vidow_clipboard clipboard;
	struct wlr_layer_shell_v1 *layer_shell;
	/* zxdg_decoration_manager_v1（见 decoration.c）：客户端问「装饰谁画」。
	 * 不给这个全局，kitty / GTK / Qt 就自己画标题栏，跟我们的圆角打架。 */
	struct wlr_xdg_decoration_manager_v1 *decoration_manager;
	struct wl_listener new_decoration;
	/* org_kde_kwin_server_decoration_manager：GTK3 只认这个老协议（libgdk-3
	 * 里根本没有 zxdg_decoration_manager_v1），没有它 GTK3 永远自己画 CSD，
	 * 带一圈阴影留白。见 decoration.c。 */
	struct wlr_server_decoration_manager *kde_decoration_manager;
	struct wl_listener new_kde_decoration;
	/* 输出抓帧：vidow 自己实现（capture.c / screencopy.c）。客户端缓冲里放的
	 * 是**我们自己渲染**的一帧，所以 capture = "hide" 的窗口 / layer 只在抓帧
	 * 里消失，屏幕上照常显示。 */
	struct vidow_capture_manager *capture_manager;
	struct vidow_screencopy *screencopy;
	/* 录屏：zkde_screencast_unstable_v1（KWin 私有协议，KDE 的录屏 portal
	 * 建在它上面），见 screencast.c。NULL = 没建成，不致命。 */
	struct vidow_screencast *screencast;
	/* 新标准抓帧（ext-image-copy-capture）的会话管理器 —— 用 wlroots 那份
	 * （协议管线 / 约束 / dmabuf feedback 都靠它），只有「输出抓帧源」换成
	 * vidow 自己的（见 capture.c）。窗口抓帧源仍是 wlroots 的，抓帧遮挡要靠
	 * 它的 new_session 信号判断「有人在抓窗口」。见 capturemask.c。 */
	struct wlr_ext_image_copy_capture_manager_v1 *ext_capture;
	struct wl_list capture_sessions; /* vidow_capture_session.link */
	struct wl_listener new_capture_session;
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
	struct vidow_cursor_fx *cursor_fx; /* 摇晃放大（cursorfx.c），可能为 NULL */
	struct wlr_seat *seat;

	/* 指针约束（pointerconstraint.c）：游戏锁鼠标 / 夹光标要的两个协议。
	 * active_constraint 非空 = 现在有一个正生效（IPC 会报出来）。 */
	struct wlr_pointer_constraints_v1 *pointer_constraints;
	struct wlr_relative_pointer_manager_v1 *relative_pointer;
	struct wlr_pointer_constraint_v1 *active_constraint;
	pixman_region32_t confine; /* 生效中那个约束的围栏（表面局部坐标） */
	bool constraint_listeners; /* 三个监听是不是挂上了（挂上了才敢摘） */
	/* 上一次处理过的光标位置：位移靠它和当前位置相减得到（见
	 * vidow_pointer_constraint_motion），锁定时也是「钉住」的基准。 */
	double pointer_last_x, pointer_last_y;
	double pointer_lock_x, pointer_lock_y; /* 锁定时钉在哪（布局坐标） */
	struct wl_listener new_constraint;
	struct wl_listener constraints_destroy;
	struct wl_listener constraint_destroy;
	struct wl_listener constraint_set_region;
	struct wl_listener constraint_commit;

	/* 键盘那两个协议（keyboardgrab.c）：快捷键抑制 + X11 的键盘抓取。
	 * xwayland_grabs 非空 = 现在有 X 客户端抓着键盘（IPC 会报出来）。 */
	struct wlr_keyboard_shortcuts_inhibit_manager_v1 *shortcuts_inhibit;
	struct wl_list inhibitors; /* vidow_shortcuts_inhibitor.link */
	struct wl_listener new_inhibitor;
	struct wl_listener inhibit_destroy; /* manager 销毁时摘监听 */
	struct wl_global *xwayland_grab_global;
	struct wl_list xwayland_grabs; /* vidow_xwayland_grab.link */

	struct vidow_config config;
	struct vidow_decor_prog decor;
	struct vidow_gl_prog kawase_down, kawase_up;
	struct vidow_mesh_prog mesh;
	struct wlr_texture *empty_tex; /* 1x1 transparent, for disabled samplers */
	uint64_t blur_serial;          /* hands out blur texture identities */

	/* Backdrop generations. gen_layer covers the wallpaper and the layer
	 * surfaces, gen_scene additionally covers every window: a cached blur is
	 * reused until the matching generation moves. */
	uint64_t gen_layer, gen_scene;
	/* vidow_arrange() 这一趟是不是「只有正被拖的那个窗口在动」（见那里的说明）：
	 * 拖动时每动一下都会重摆一次，这种情况不该把整个世界代次推一遍。置位之后，
	 * 只要有任何一个**别的**窗口报「我变了」（vidow_backdrop_damage_view）就清掉。 */
	bool arrange_self_only;

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
	/* 注入用的键盘（IPC 的 send-key / type-text）。wlroots 没有公开的「造一个
	 * 键盘」接口，键盘组能给出一个干净的 wlr_keyboard（自带 keymap 和
	 * xkb_state），借它按下注入的键就够了。它也挂在上面的 keyboards 列表里：
	 * headless 会话里它就是「这个座位有键盘」的唯一证据（少了它客户端连
	 * wl_keyboard 都不会去要，注入的键也就没地方送）。 */
	struct wlr_keyboard_group *inject_group;
	struct wl_list layers;    /* vidow_layer_surface.link */
	struct wl_list columns;   /* vidow_column.link（所有列的并集，顺序 = 工作区顺序） */
	struct wl_list workspaces; /* vidow_workspace.link */
	struct wl_list keybinds;   /* vidow_keybind.link */
	struct wl_list rules;      /* vidow_rule.link（窗口规则） */
	struct wl_list output_confs; /* vidow_output_conf.link（每块屏一组配置） */
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
	/* 这次抓取是不是**客户端自己发起**的（CSD 标题栏拖拽 -> xdg_toplevel.move、
	 * X11 的 _NET_WM_MOVERESIZE -> request_move）。那条路上「按下」是转给客户端
	 * 了的（input.c 末尾那句 notify_button），松手也必须还给它 —— 不然客户端
	 * 停在「一直按着」的状态：拖完再动鼠标它还在拖，之后第一下点击被当成按下
	 * 而不是点一下，得再点一次才能恢复（用户报的「拖拽之后就不行了，点一下
	 * 界面、移出去再移回来再点一次就好了」）。Mod+拖拽那条路按下是吃掉的，
	 * 松手也一起吃掉，客户端两边都没见过，状态是对称的。 */
	bool grab_client_press;
	/* 拖动时指针比刷新快：一帧里会来好几个移动事件，中间那些位置屏幕上从来没
	 * 出现过。重画装饰（整块窗口那么大的离屏渲染）+ 记损伤推迟到出帧之前做一次
	 * （见 vidow_tile_grab_update / output.c 的帧回调）—— 实测每个事件 66µs
	 * （1000Hz 鼠标 = 每帧 0.4ms，8000Hz = 每帧 3.3ms，全是白算的）。
	 * grab_last_box = **已经画出来**的那个位置（损伤从它算起）。 */
	bool grab_dirty;
	struct wlr_box grab_last_box;
	/* 浮动窗口拖拽同理：每帧最多重排一次（见 vidow_float_grab_update）。 */
	bool arrange_dirty;
	/* 探针：最近一次拖拽移动事件的时刻（见 output.c 的拖拽延迟统计） */
	int64_t grab_ev_ns;

	/* overview（niri 风格全景） */
	struct vidow_overview overview;

	/* Alt+Tab 切换器 / 最近用过的窗口（见 mru.c） */
	struct vidow_mru mru;

	/* IPC（见 ipc.c / ipcstate.c） */
	struct wl_event_source *ipc_listen; /* 监听 socket 的 fd 源 */
	struct wl_list ipc_clients;         /* ipc_client.link */
	char ipc_path[PATH_MAX];            /* 实际在听的路径（空 = 没起来） */
	int ipc_subscribers;                /* 订阅了事件的连接数 */
	/* IPC 报给外面的稳定编号（从 1 起，0 = 还没分配） */
	uint64_t next_view_id;
	uint64_t next_column_id;
	uint64_t next_layer_id;
	/* 最小化的先后（「放回最近收起来的那个」按它挑，见 view.c） */
	uint64_t next_minimize_seq;

	struct vidow_view *focused;
	struct vidow_layer_surface *keyboard_layer; /* exclusive layer grab */
	struct wlr_output *focused_output;

	/* Fallback clock: advances animations when the backend stops delivering
	 * frame events (a nested Wayland output stalls while the host is idle). */
	struct wl_event_source *anim_timer;
	/* 看门狗正在跑（定时器已经排上，还没回调）。拖动时每个指针事件都会来
	 * kick 一下，而 wl_event_source_timer_update 是「重排」不是「没排就排」——
	 * 1000Hz 的鼠标每 1ms 把 24ms 的闹钟往后推一次，闹钟永远不响，拖动就又冻住了
	 * （实测：按住帧事件 + 200Hz 匀速拖 2 秒，只出了 1 帧）。所以这里记住状态，
	 * 只在没排的时候排一次。 */
	bool watchdog_armed;
	/* 会话切回来（或输出刚建好）之后主动画一帧的一次性闹钟，见
	 * vidow_output_kick_all()。 */
	struct wl_event_source *kick_timer;
	/* 帧追踪的定时器（VIDOW_TRACE_FRAMES=1 才建），见
	 * vidow_output_schedule_frame()。 */
	struct wl_event_source *trace_timer;
};

/* -------------------------------------------------------------------- init */

bool vidow_output_init(struct vidow_server *server);
/* 要一帧（vidow 主动排帧的唯一入口：VIDOW_TRACE_FRAMES=1 时记下是谁要的，
 * 供帧追踪用）。找得到对应的 vidow_output 才记账 —— 记账只是诊断，
 * 找不到也照样排帧。why 只进日志。 */
void vidow_output_schedule_frame(struct vidow_server *server,
		struct wlr_output *wlr_output, const char *why);
/* 客户端交帧了（view_commit）：给帧追踪记一笔时间戳。 */
void vidow_output_trace_commit(struct vidow_output *output);
/* 按 wlroots 的输出反查 vidow 的那一份（找不到 NULL）。 */
struct vidow_output *vidow_output_lookup(struct vidow_server *server,
		struct wlr_output *wlr_output);
/* 按配置（全局 scale + vidow.output{ name = … } 里那块屏的 scale）算并设一个
 * 输出的缩放；SIGHUP 后整套重来。 */
void vidow_output_apply_scale(struct vidow_server *server,
		struct vidow_output *output);
void vidow_output_apply_scale_all(struct vidow_server *server);
/* 每块屏立刻画一帧（走 1ms 定时器）。会话切回来、输出刚建出来的时候用：这时候
 * 没有任何 damage，后端也不会主动送 frame 事件，不主动画就一直是黑的。 */
void vidow_output_kick_all(struct vidow_server *server);
/* 有人要抓帧了：把抓帧看门狗叫起来（静止的屏幕永远不提交，抓帧得自己推帧，
 * 见 output.c 的 capture_watchdog）。抓帧请求进来时调一次。 */
void vidow_output_capture_kick(struct vidow_server *server);
/* 按配置（vidow.output{ name = …, x = …, y = … }）把每块屏摆到它该在的位置；
 * SIGHUP 后对一次账。 */
void vidow_output_apply_positions_all(struct vidow_server *server);
/* 配置里那块屏的开关 / 模式 / 方向 / VRR / 位深改过：按新配置对一遍账（SIGHUP、
 * 配置刚跑完）。没变的什么都不做。 */
void vidow_output_apply_state_all(struct vidow_server *server);
/* "3840x2160@144" -> 分辨率 + 刷新率（@ 可省，刷新建 0 = 不挑）。config 用。 */
bool vidow_output_parse_mode(const char *text, int *w, int *h, double *hz);
/* 方向的名字 -> 枚举（normal / 90 / 180 / 270 / flipped / flipped-90 …）。 */
bool vidow_output_parse_transform(const char *text,
		enum wl_output_transform *out);
/* 输出的运行时改动（IPC 用）。都会 commit 一次并把跟着变的东西（布局、背景
 * 缓存、光标、可用区域）重做一遍。why 只进日志。 */
bool vidow_output_set_scale(struct vidow_server *server,
		struct wlr_output *wlr_output, double scale, const char *why);
bool vidow_output_set_transform(struct vidow_server *server,
		struct wlr_output *wlr_output, enum wl_output_transform transform);
/* VRR（adaptive sync）。后端 / 这块屏不支持时只报一句，返回 false。 */
bool vidow_output_set_vrr(struct vidow_server *server,
		struct wlr_output *wlr_output, bool enabled);
/* 渲染位深（8 / 10）。10 位 = DRM_FORMAT_XRGB2101010 的输出缓冲，不支持就保持
 * 原样并说清楚，返回 false。 */
bool vidow_output_set_depth(struct vidow_server *server,
		struct wlr_output *wlr_output, int depth);
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
/* 输出盒（布局坐标）-> 根 toplevel 表面自己的坐标系：unconstrain 要的是后者，
 * 少这一步窗口不在布局原点时菜单会被摆到屏外（见 view.c 的注释）。 */
void vidow_popup_box_to_toplevel_space(const struct vidow_view *view,
	struct wlr_box *box);
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
/* 配置「族」：一族的键既能一个值管全部，也能写成一张表只盖其中几项
 * （`gaps = { top = 30, inner = 20 }`、`border = { width = 0 }`）。
 * 解析表的地方在 script.c（Lua 那边），这里只回答「这个名字是不是族」、
 * 「族名 = 一个值」落到哪个平铺键、表里的项名展开成哪个平铺键。 */
bool vidow_config_group_has(const char *name);
const char *vidow_config_group_scalar(const char *name);
const char *vidow_config_group_field(const char *group, const char *field);
size_t vidow_config_group_fields(const char *group, const char **out,
		size_t max);
/* 删掉的旧键名（gaps 那一家）：配置里再写就是普通全局变量、静默失效，所以让
 * script.c 认出来喊一声。命中就给 true，并把「现在该写什么」放进 *hint。 */
bool vidow_config_legacy_hint(const char *key, const char **hint);
/* 把内置默认值打印成 Lua 配置。配置按类别拆成 configs/ 下的一类一个文件，入口
 * config.lua 只留 vidow.include() 行，所以这里有三个口子：
 *
 *   vidow_config_default_lua()         —— 入口（只有 include 行）
 *   vidow_config_default_all_lua()     —— 全部分类文件拼在一起（能当单文件配置用）
 *   vidow_config_default_section_lua() —— 其中一份，名字不认识返回 false
 *
 * 环境变量不参与 —— 打出来的是「什么都没配」时的样子。 */
void vidow_config_default_lua(FILE *out);
void vidow_config_default_all_lua(FILE *out);
bool vidow_config_default_section_lua(FILE *out, const char *name);
int vidow_config_section_index(const char *name);
int vidow_config_section_count(void);
const char *vidow_config_section_file(int index);
/* 把上面那套写进配置目录（`vidow --write-config`）：入口 + configs/ 下每一份，
 * 父目录不存在就建。入口已经有一份就整个不写（除非 force）；分类文件已有的跳过、
 * 只补缺的。写了哪些 / 跳过了哪些打在 report 上，原因写进 err。成功返回 0。 */
int vidow_config_write_default(const char *path, bool force, FILE *report,
		char *err, size_t err_len);

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
 * 失败只打日志：自动重载是锦上添花，SIGHUP 永远还在。
 *
 * 盯的不只是 config.lua：vidow.include() 读进来的每一份（configs/ 下的每一份）都登记
 * 进来，改哪一份都自动重载。 */
bool vidow_configwatch_init(struct vidow_server *server);
void vidow_configwatch_finish(struct vidow_server *server);
/* 重新加载配置之前调一次：上一次 include 过的文件不算数了，重新登记。 */
void vidow_configwatch_begin(struct vidow_server *server);
/* vidow.include() 读了一份文件就登记一份（configwatch 没起来就是空操作）。 */
void vidow_configwatch_add(struct vidow_server *server, const char *path);

/* ------------------------------------------------------------- animation */

int64_t vidow_now_ns(void);
struct vidow_anim *vidow_anim_start(struct vidow_server *server,
		const char *tag, int duration_ms, enum vidow_easing easing,
		void (*update)(void *data, double t, double linear),
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

/* 按渲染器能渲染的格式挑一张离屏缓冲（fx.c 的 alloc_target：优先带 alpha 的
 * 格式，装饰圆角才不会发黑）。摇晃放大也拿它分配光标图。 */
struct wlr_buffer *vidow_alloc_target(struct vidow_server *server, int w, int h);

void vidow_fx_set_alpha(struct wlr_scene_node *node, double alpha);
bool vidow_shader_init(struct vidow_server *server);
struct vidow_shader *vidow_shader_get(struct vidow_server *server,
		const char *name);

/* ---- 拖窗口的果冻（jelly.c） ---- */
/* 起手：把窗口现在显示的那块带装饰的离屏图切成网格挂到弹簧上。要窗口有装饰
 * 节点（fx 节点）才能做；做不了就返回 false，拖动照旧、只是不弯。 */
bool vidow_jelly_begin(struct vidow_view *view);
/* 抓底衬期间把果冻节点也藏起来（它是窗口内容的另一份拷贝）。 */
void vidow_jelly_capture_visible(struct vidow_view *view, bool on);
/* 松手：解掉抓点，整张网自己晃回去，晃静了自动交接（装饰节点重新露出来）。 */
void vidow_jelly_release(struct vidow_view *view);
/* 立刻收掉（源对不上了 / 松手收尾结束）。 */
void vidow_jelly_end(struct vidow_view *view);
/* 视图正在被销毁（free(view) 之前必须调）：果冻结构里攥着 view 指针，而它
 * 那个场景节点比 view 活得久。 */
void vidow_jelly_view_gone(struct vidow_view *view);
/* 每帧积分 + 重画，在 output.c 的帧循环里叫。 */
void vidow_jelly_tick(struct vidow_server *server, int64_t now_ns);
/* 还有窗口在弯 -> 帧循环得继续排帧（不然静止屏幕上晃到一半就停了）。 */
bool vidow_jelly_pending(struct vidow_server *server);
/* 正在显示形变结果的那个场景节点（没有就是 NULL）。抓帧遮挡要用：藏着不露的
 * 窗口拖起来的时候，画在屏幕上的其实是这个节点。 */
struct wlr_scene_buffer *vidow_jelly_node(struct vidow_view *view);

bool vidow_fx_begin(struct vidow_server *server, struct vidow_fx_target *fx,
		struct wlr_scene_tree *parent, const char *shader_name);
/* scale = 窗口所在输出的缩放：离屏缓冲按它分配，装饰才是按设备像素画的
 * （不然整块窗口在 HiDPI 屏上会被放大一遍，糊）。ox/oy 是窗口在输出上的位置，
 * 逻辑坐标。 */
/* geo = 可见窗口在源缓冲里的位置/尺寸（见 struct vidow_fx_geo）；x/y 是可见窗口
 * 左上角在 fx 节点父坐标系里的位置（视图里就是 0,0：场景树节点的原点就是窗口
 * 原点），装饰矩形会摆在它外面一圈 pad 的地方。 */
/* 把一张贴图按任意三角形网格画进 dst（jelly.c 用）。verts 里每项 4 个 float：
 * x, y（渲染目标物理像素）, u, v（源贴图归一化）。 */
bool vidow_fx_draw_mesh(struct vidow_server *server, struct wlr_buffer *dst,
	int w, int h, struct wlr_texture *src, const float *verts, int n_verts);

bool vidow_fx_frame(struct vidow_server *server, struct vidow_fx_target *fx,
		struct wlr_scene_buffer *src, const struct vidow_fx_geo *geo,
		int x, int y, int ox, int oy,
		double alpha, bool focused, double scale, double ov);
void vidow_fx_end(struct vidow_server *server, struct vidow_fx_target *fx);
void vidow_fx_release(struct vidow_fx_target *fx);
void vidow_fx_flush(struct vidow_server *server);
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
/* 只记一笔损伤区域（布局逻辑坐标，同 pending_damage 的说明），不动世界代次 ——
 * 给「窗口挪走之后，老位置也得让别人的底衬重抓」这种一个事件两处损伤用。 */
void vidow_backdrop_damage_box(struct vidow_server *server,
		const struct wlr_box *box);
/* 窗口「现在画在哪」：布局几何 + 动画偏移（拖拽 / 开合 / overview 都算在这上面）。 */
struct wlr_box vidow_view_screen_box(const struct vidow_view *view);
/* 同上，但世界代次也前进：这次变的**就是** box 这一块（关窗动画那种）。 */
void vidow_backdrop_damage_region(struct vidow_server *server,
		const struct wlr_box *box);
/* 同上，但这一笔账记在某个窗口头上：它自己变了（不影响它自己的私有底衬）。 */
void vidow_backdrop_damage_view(struct vidow_view *view);
/* 同上，但损伤区域由调用方给（NULL = 整块窗口）：这次变的**就是**这一块 ——
 * 客户端只重画了窗口里的一小块时用（见 vidow_view_surface_damage）。 */
void vidow_backdrop_damage_view_rect(struct vidow_view *view,
		const struct wlr_box *box);

/* 客户端这一帧到底动了哪一块（布局逻辑坐标）。surface->buffer_damage 是**缓冲
 * 像素**坐标（wlroots 已经把 surface/buffer damage、transform、scale 都算进去
 * 了），除以「缓冲像素 / 表面逻辑像素」就是布局逻辑像素，再加场景节点的位置。 */
enum vidow_surface_damage {
	/* 一个像素都没变（只是帧回调 / ack_configure 的那种 commit）：世界代次都
	 * 不用动 —— 不动，别人缓存的底衬就还是对的。 */
	VIDOW_SURFACE_DAMAGE_NONE = 0,
	/* box 里就是那一块。 */
	VIDOW_SURFACE_DAMAGE_BOX,
	/* 说不清（变换 / viewport 缩放 / 有 subsurface / 场景节点被改过显示尺寸 /
	 * overview 里位置是映射过的 / 换了新缓冲却没报损伤）：按整块窗口算 ——
	 * 宁可多抓一次，不能糊着旧的。 */
	VIDOW_SURFACE_DAMAGE_WHOLE,
};
enum vidow_surface_damage vidow_view_surface_damage(struct vidow_view *view,
		struct wlr_box *box);
/* Release the blur resources an output owns. */
void vidow_blur_output_finish(struct vidow_output *output);

void vidow_view_apply_anim(struct vidow_view *view);
/* 只摆位置（便宜，拖动跟手时每个指针事件走这条） / 只重烘装饰图（贵，一帧一次）。 */
void vidow_view_anim_bake(struct vidow_view *view);
void vidow_view_apply_anim_place(struct vidow_view *view);

/* ------------------------------------------------------- 鼠标拖窗口（drag.c） */

/* 平铺窗口的 Mod+左键拖拽：开始 / 跟手 / 松手落位（跨屏就归那块屏的工作区）。 */
void vidow_tile_grab_begin(struct vidow_server *server,
		struct vidow_view *view);
void vidow_tile_grab_update(struct vidow_server *server, double lx, double ly);
void vidow_tile_grab_end(struct vidow_server *server);

/* 指针按键（真实输入和测试注入走同一条路，见 input.c）。 */
/* force_mod：假装按着 Mod（测试注入用 —— 拖拽要求按着 Mod，而测试环境里没有
 * 真键盘，见 VIDOW_TEST_ACTION 的 pointer-button 动作）。 */
void vidow_pointer_button(struct vidow_server *server, uint32_t button,
		bool pressed, uint32_t time_msec, bool force_mod);
/* 指针移动（测试注入也走这里）。 */
void vidow_pointer_motion(struct vidow_server *server, uint32_t time_msec);
/* 滚轮。真实滚轮和 IPC 注入走同一条路（overview 平移 / Mod+滚轮换列 / 转发给
 * 客户端），所以直接收一个 wlr_pointer_axis_event 的字段而不是拆开的 dx/dy。 */
void vidow_pointer_axis(struct vidow_server *server,
		enum wl_pointer_axis orientation, double delta, int32_t delta_discrete,
		enum wl_pointer_axis_source source, uint32_t time_msec);

/* ------------------------------------------------------------- 抓帧遮挡
 *
 * 窗口规则 capture = "hide" / "mosaic" 让一个窗口或 layer surface 在别人截图 /
 * 录屏时不出现（或者以马赛克出现）。机制见 capturemask.c。
 *
 * vidow 自己实现输出抓帧（capture.c / screencopy.c）：抓帧的那一帧是**自己渲染**
 * 的（渲染前把要藏的节点透明度压 0），所以屏幕那一帧完全不受影响 —— 藏着的东西
 * 屏幕上照常显示，抓帧里没有。遮挡只在渲染抓帧那一瞬间压上（引用计数，见
 * vidow_capturemask_hold / _release）。
 *
 * 还剩一条老路要照顾：ext-image-copy-capture 的**窗口**抓帧源是 wlroots 那份
 * （直接渲染窗口的场景子树），接不进去 —— 有人在抓一个 capture = "hide" 的窗口
 * 时，只能整段时间把它从屏幕上藏起来（就是 vidow_capture_session 这套）。 */
/* 一个还活着的 ext-image-copy-capture 会话（不是 vidow 自己出帧的那种）。 */
struct vidow_capture_session {
	struct wl_list link; /* vidow_server.capture_sessions */
	struct vidow_server *server;
	struct wl_listener destroy;
};

/* 输出抓帧（capture.c：引擎 + 输出抓帧源；screencopy.c：老协议 zwlr_screencopy）。 */
struct vidow_capture;
struct vidow_capture_manager;
struct vidow_screencopy;

/* 建两个抓帧协议：ext_output_image_capture_source_manager_v1（配合 wlroots 的
 * ext_image_copy_capture_manager_v1）和 zwlr_screencopy_manager_v1。返回 NULL
 * 表示没建成（不致命：抓帧协议没有，别的照常）。 */
struct vidow_capture_manager *vidow_capture_manager_create(struct wl_display *display,
		struct vidow_server *server);
struct vidow_screencopy *vidow_screencopy_create(struct wl_display *display,
		struct vidow_server *server);
/* 还有没有等着的 screencopy 帧（静止的屏幕上没有输出提交，output.c 的
 * capture_watchdog 靠它决定要不要催一帧）。 */
bool vidow_screencopy_has_pending(struct vidow_screencopy *manager);

/* 渲染一帧抓帧图（遮挡压好、渲染、放开都在里面），返回**加锁的**缓冲，调用方
 * 负责 wlr_buffer_unlock()。渲染不出来返回 NULL。 */
struct wlr_buffer *vidow_capture_grab(struct vidow_server *server,
		struct wlr_output *output);
/* 抓这块屏上的任意一个矩形（box 是**布局坐标**，scale 是「逻辑 → 像素」，出图
 * 尺寸 = round(w*scale) × round(h*scale)）。按区域录屏 / 按区域截图用它。 */
struct wlr_buffer *vidow_capture_grab_box(struct vidow_server *server,
		struct wlr_output *output, const struct wlr_box *box, double scale);

/* layer 的内容节点（surface 的 buffer）：wlroots 把它包在 subsurface tree 里，
 * 所以要递归找。抓帧遮挡（capturemask.c）和马赛克（capture.c）都要用它。 */
struct wlr_scene_buffer *vidow_layer_surface_buffer(
		struct wlr_scene_layer_surface_v1 *scene);

/* 把 capture = "mosaic" 的窗口 / layer 用「缩小再 NEAREST 放大」的块状图盖到
 * 抓帧图上（块边长 = 规则的 capture_mosaic，屏幕像素）。调用前先压过遮挡
 * （原图不在抓帧里），见 capture.c。 */
void vidow_capture_paint_mosaic(struct vidow_server *server,
		struct wlr_output *output, struct wlr_buffer *dst);

/* 把这块屏上的光标合成到抓帧图上（要光标的客户端才调：ext-copy 的
 * PAINT_CURSORS / screencopy 的 overlay_cursor）。抓帧是场景渲染，场景里没有
 * 光标，所以要光标的客户端得我们自己画上去。 */
void vidow_capture_paint_cursor(struct vidow_server *server,
		struct wlr_output *output, struct wlr_buffer *dst);
/* 同上，但只画落在 box 里（**布局坐标**；整屏就是这块屏在布局里占的那一块）的
 * 光标：按区域 / 按窗口录屏时，光标跑到别处去就不该出现在这张图里。 */
void vidow_capture_paint_cursor_box(struct vidow_server *server,
		struct wlr_output *output, struct wlr_buffer *dst,
		const struct wlr_box *box, double scale);

/* 光标位图的上限（像素，边长）。SPA_META_Cursor 的元数据空间在建流时按这个上限
 * 一次性声明，比这大的光标（客户端能设任意大的 cursor surface）就只报位置。 */
#define VIDOW_CURSOR_MAX_PX 256

/* 光标元数据（录屏 pointer_mode=metadata 用）：位置 / 热点是**抓帧图里的像素**
 * （框外的光标位置会是负的 / 超出图），位图是预乘 RGBA8888（SPA_VIDEO_FORMAT_RGBA），
 * 写进调用方给的 pixels（至少 VIDOW_CURSOR_MAX_PX² × 4 字节）。
 * 返回 false = 这块屏上没有可见光标；返回 true 但 width/height 为 0 = 只有位置
 * 没有位图（贴图太大 / 带旋转 / 读不出来）。 */
struct vidow_cursor_info {
	int x, y;
	int hotspot_x, hotspot_y;
	int width, height;
	uint32_t format;
};
bool vidow_capture_cursor_info(struct vidow_server *server, struct wlr_output *output,
		const struct wlr_box *box, double scale, struct vidow_cursor_info *info,
		void *pixels, size_t pixels_size);

/* ------------------------------------------------------------------ 录屏 */

/* zkde_screencast_unstable_v1：KWin 的私有录屏协议。xdg-desktop-portal-kde 的
 * ScreenCast 就建在它上面 —— 绑不到这个 global，那个后端**连
 * org.freedesktop.impl.portal.ScreenCast 接口都不注册**（实测：Hyprland 上也没有），
 * 所以「KDE 的录屏 portal」在非 KWin 的合成器上不是配错了，是根本不存在。
 *
 * 协议要求**合成器自己建 PipeWire 流**、把 node id 通过 created 事件告诉客户端，
 * 所以这个模块同时是个 PipeWire 生产者：抓帧走 vidow 自己那条路
 * （vidow_capture_grab / _box / _view），像素进 memfd 缓冲。实现见 screencast.c。
 *
 * 四种来源都实现了（对应协议里四个请求）：
 *   stream_output          整块输出
 *   stream_window          一个窗口（window_uuid = ext-foreign-toplevel-list 的
 *                          identifier，vidow 拿它反查自己的 view）
 *   stream_region          工作区里的一块矩形（布局坐标）
 *   stream_virtual_output  现场加一块**虚拟输出**（headless 后端 + 输出），它是一块
 *                          真输出：能摆窗口、能进布局，录的是它自己那一帧
 */
struct vidow_sc_pw;
struct vidow_sc_pw_stream;

/* 这一路流在录什么。 */
enum vidow_sc_kind {
	VIDOW_SC_OUTPUT = 0,  /* 整块输出 */
	VIDOW_SC_WINDOW,      /* 一个窗口 */
	VIDOW_SC_REGION,      /* 一块矩形 */
	VIDOW_SC_VIRTUAL,     /* 现场加的虚拟输出（output 指向它） */
};

struct vidow_screencast {
	struct vidow_server *server;
	struct wl_global *global;
	struct wl_list streams; /* vidow_screencast_stream.link */
	struct vidow_sc_pw *pw;
	/* 虚拟输出用的 headless 后端（第一次要虚拟输出时才建，进程里只建一个）。 */
	struct wlr_backend *virtual_backend;
};

/* 一路流（客户端手上那个 zkde_screencast_stream_unstable_v1）。 */
struct vidow_screencast_stream {
	struct wl_list link; /* vidow_screencast.streams */
	struct wl_resource *resource;
	struct vidow_screencast *sc;
	struct wlr_output *output;
	enum vidow_sc_kind kind;
	/* 渲染出图的目标像素尺寸（PipeWire 协商的那个）。 */
	int out_w, out_h;
	/* kind == WINDOW：录哪个窗口（窗口没了就结束这一路）。 */
	struct vidow_view *view;
	struct wl_listener view_destroy;
	/* kind == WINDOW：窗口的取景器（私有场景 + 镜像，见 wincapture.c）。
	 * 跟窗口在屏幕上摆在哪没关系，所以滚出视口 / 被裁也不影响出帧。 */
	struct vidow_win_grab *win_grab;
	/* kind == REGION / WINDOW：抓哪一块（布局坐标）+ 逻辑→像素的缩放。 */
	struct wlr_box box;
	double scale;
	/* kind == VIRTUAL：这块输出是我们建的，这一路结束时得销毁。 */
	bool owns_output;
	uint32_t pointer_mode;
	bool closed;
	struct wl_listener output_destroy;
	struct vidow_sc_pw_stream *pw;
};

/* 建 zkde_screencast_unstable_v1。返回 NULL = 没建成（不致命：录屏用不了，
 * 别的照常）。 */
struct vidow_screencast *vidow_screencast_create(struct wl_display *display,
		struct vidow_server *server);
/* 这一路流给客户端一个失败原因。协议里没有 error code，failed 事件才是客户端
 * 认的报错方式（回完它，客户端会 close 掉这一路）。 */
void vidow_screencast_stream_fail(struct vidow_screencast_stream *stream,
		const char *why);
/* 现场加一块虚拟输出（headless 后端 + 输出），返回 NULL 时 *why 里是原因。 */
struct wlr_output *vidow_screencast_virtual_output(struct vidow_screencast *sc,
		const char *name, int width, int height, double scale, const char **why);

/* PipeWire 那一半（screencast.c 内部用）。 */
struct vidow_sc_pw *vidow_sc_pw_create(struct vidow_screencast *sc,
		struct wl_display *display);
void vidow_sc_pw_stream_start(struct vidow_screencast_stream *stream);
void vidow_sc_pw_stream_destroy(struct vidow_screencast_stream *stream);

/* 这个抓帧源是不是 vidow 自己出的帧（是的话「有人抓帧」不该惊动屏幕 —— 屏幕那
 * 一帧本来就不受影响）。 */
bool vidow_capture_source_is_ours(struct wlr_ext_image_capture_source_v1 *source);

/* 挂上 ext-image-copy-capture 的 new_session 监听（main.c 建完协议之后调一次）。 */
void vidow_capturemask_init(struct vidow_server *server);
/* 退出前必须调（wl_display_destroy 之前）：把会话钩子摘掉，见 capturemask.c。 */
void vidow_capturemask_finish(struct vidow_server *server);

/* 压上 / 放开遮挡（引用计数：抓帧渲染期间压着，同时有别的东西在抓也不会被提前放开）。 */
void vidow_capturemask_hold(struct vidow_server *server);
void vidow_capturemask_release(struct vidow_server *server);

/* --------------------------------------------------- 摇晃找光标（cursorfx.c）
 *
 * 快速来回晃几下指针，指针就地放大一会儿（cursor_shake / cursor_shake_scale）。
 * 放大走的是硬件光标那条路：放得下就不进合成器的渲染通道，因此不会出现在截图
 * 里；放不下会被降级成软光标，所以倍数先按硬件上限夹。 */
/* 建状态 + 定时器（input_init 里调一次）。失败只是没这个功能，不影响别的。 */
void vidow_cursor_fx_init(struct vidow_server *server);
/* 每次指针移动后调（真实鼠标、测试注入、IPC 注入都走这里）。 */
void vidow_cursor_fx_motion(struct vidow_server *server);
/* 手动放大一次（IPC 的 cursor-shake）。why 里是拒绝的原因。 */
bool vidow_cursor_fx_shake(struct vidow_server *server, const char **why);
/* 客户端换了光标图（request_set_cursor）：记下来，好在放大结束时还原。 */
void vidow_cursor_fx_set_surface(struct vidow_server *server,
		struct wlr_surface *surface, int32_t hx, int32_t hy);
/* 指针底下没窗口了，该显示主题默认箭头。放大期间不动真图，只记状态。 */
void vidow_cursor_fx_default_cursor(struct vidow_server *server);
/* 光标图换了（客户端 set_cursor / 焦点切换）：放大中就换成新图，别缩回去。 */
void vidow_cursor_fx_cursor_changed(struct vidow_server *server);
/* 光标状态（magnified / scale / image / hardware / hw_max），写进当前 JSON 对象。 */
struct jbuf;
void vidow_cursor_fx_json(struct jbuf *b, struct vidow_server *server);

/* ------------------------------------------------------- 指针约束（pointerconstraint.c）
 *
 * zwp_pointer_constraints_v1 + zwp_relative_pointer_manager_v1：游戏锁鼠标要的
 * 那两个协议（Xwayland 拿相对指针做 XInput2 的 raw motion，缺了 X11 游戏转不动
 * 视角）。wlroots 只给协议对象，夹住 / 钉住光标是合成器的活，实现在这个文件里。
 * 判据：./test-pointer-lock.sh。 */
bool vidow_pointer_constraint_init(struct vidow_server *server);
/* 指针焦点变了（vidow_pointer_motion 里命中测试之后调）：新表面有约束就激活。 */
void vidow_pointer_constraint_focus(struct vidow_server *server,
		struct wlr_surface *surface);
/* 指针动了一下：发相对位移 + 锁定的钉回去 / 夹住的夹住（命中测试之前调）。 */
void vidow_pointer_constraint_motion(struct vidow_server *server,
		uint32_t time_msec);
/* 生效中的约束是哪种（"locked" / "confined" / NULL = 没有）。 */
const char *vidow_pointer_constraint_type(const struct vidow_server *server);
/* 约束状态（type / lifetime / window / cursor）写进当前 JSON 对象。 */
void vidow_pointer_constraint_json(struct jbuf *b, struct vidow_server *server);

/* ------------------------------------------- 键盘协议（keyboardgrab.c）
 *
 * zwp_keyboard_shortcuts_inhibit_manager_v1（客户端说「别吃我的快捷键」，
 * 虚拟机 / 远程桌面要的）+ zwp_xwayland_keyboard_grab_manager_v1（Xwayland 替
 * X11 的 XGrabKeyboard 要键盘；wlroots 0.20 没有这个，服务端自己写）。判据：
 * ./test-keyboard-grab.sh。 */
bool vidow_keyboard_grab_init(struct vidow_server *server);
/* 键盘焦点那个表面上有没有生效中的抑制（keyboard_key 用它决定跳不跳自己的键位）。 */
bool vidow_shortcuts_inhibited(struct vidow_server *server);
/* X11 抓着键盘时按键一律给它：每次要送按键前调（焦点被别的窗口抢走了也按回来）。 */
void vidow_xwayland_grab_assert(struct vidow_server *server);
/* 键盘状态（shortcuts_inhibited / keyboard_grab）写进当前 JSON 对象。 */
void vidow_keyboard_grab_json(struct jbuf *b, struct vidow_server *server);

/* --------------------------------------------------------------- 注入（IPC）
 *
 * 合成输入：给自动化 / 远程操控用（UI 识别之后总得能动手）。默认开着，见
 * config.input_injection。所有函数返回 false 时 err 里是给人看的原因。
 *
 * 注入的按键只给**客户端**，不触发 vidow 自己的快捷键 —— 要操作合成器请用
 * 对应的 IPC 命令（focus-window / toggle-overview / …）。
 */
/* 建注入键盘。input_init 里调一次；失败只是注入不能用，不影响别的。 */
bool vidow_input_inject_init(struct vidow_server *server);
/* 开关（config.input_injection）；关着时 err 里说明原因。 */
bool vidow_inject_allowed(struct vidow_server *server, char *err, size_t errlen);
/* 指针绝对 / 相对移动（布局坐标）。relative = true 时 x/y 是增量。 */
bool vidow_inject_pointer_move(struct vidow_server *server, double x, double y,
		bool relative, char *err, size_t errlen);
/* "left" / "right" / "middle" / 数字（BTN_LEFT = 272 这种没人记得住）。
 * mode: 0 = click（按下再松开），1 = 只按下，2 = 只松开。count > 1 时按 40ms
 * 间隔连点（客户端才会认成双击）。 */
bool vidow_inject_pointer_button(struct vidow_server *server, const char *name,
		int mode, int count, char *err, size_t errlen);
/* 按键名 → evdev 按钮码；认不出来返回 0。键位动作和 IPC 共用。 */
uint32_t vidow_pointer_button_from_name(const char *name);
/* 滚轮：discrete = true 当滚轮（WL_POINTER_AXIS_SOURCE_WHEEL），这时 dx/dy 是
 * **格数**（一格 = delta 15 像素 + delta_discrete 120，和真鼠标一致）；false 就
 * 当触摸板式的连续滚动，dx/dy 直接是像素。 */
bool vidow_inject_pointer_scroll(struct vidow_server *server, double dx, double dy,
		bool discrete, char *err, size_t errlen);
/* "ctrl+shift+t" / "Return" / "super+Tab"。mode: 0 = tap（按下再松开），
 * 1 = 只按下，2 = 只松开（要自己按组合键就先 press 修饰键、再 press 主键）。 */
bool vidow_inject_key_combo(struct vidow_server *server, const char *combo,
		int mode, char *err, size_t errlen);
/* 逐字符打一段 UTF-8（按当前 keymap 找键位，需要 Shift 就自己按）。 */
bool vidow_inject_text(struct vidow_server *server, const char *text, char *err,
		size_t errlen);
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
/* 把「你这列最大化了」告诉客户端（xdg 的 set_maximized / X11 的 _NET_WM_STATE_*）。
 * 状态没变就不再发（同 set_tiled，见 vidow_view.cfg_maximized）。 */
void vidow_view_set_maximized(struct vidow_view *view, bool maximized);
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
/* 这个窗口的 X11 缩放桥（整数倍）：逐窗口规则 x11_scale 优先，没写就用全局
 * xwayland_scale。X11 的「缓冲区像素 → 逻辑坐标」处处按它除（见 view.c）。 */
int vidow_view_x11_scale_int(const struct vidow_view *view);
bool vidow_view_x11_scale_overridden(const struct vidow_view *view);
void vidow_view_open_anim(struct vidow_view *view);
/* 可见窗口在源缓冲里的位置/尺寸（客户端 CSD 留白就跑这儿出去，见 struct
 * vidow_fx_geo）：装饰、模糊区域、输入法候选框都拿它定位。 */
struct vidow_fx_geo vidow_view_fx_geo(const struct vidow_view *view);
/* snapshot 是一份源缓冲（表面）快照，w/h 是它的逻辑尺寸；geo 说明里面哪一块是
 * 「可见窗口」（x/y 是窗口左上角在 parent 坐标系里的位置）—— 客户端 CSD 留白
 * 靠它从快照里对齐。 */
void vidow_closing_start(struct vidow_server *server,
		struct wlr_scene_tree *parent, struct wlr_buffer *snapshot,
		const struct vidow_fx_geo *geo, int x, int y, int w, int h,
		int duration_ms, int slide_px, bool decorate, const char *shader_name,
		double scale, enum vidow_easing easing, double anim_scale_to);

/* ------------------------------------------------------------------ layout */

void vidow_arrange(struct vidow_server *server);
/* 有人正拿着这个窗口的画面吗（窗口截图源 / 录屏 / 预览）：拿着的时候照样剔除
 * （停到屏外）—— 抓帧渲染的是窗口自己的一份镜像，跟它在屏幕上摆在哪没关系；
 * 拿着只影响一件事：视口外补帧那条路会一直给它补帧回调（见 view.c）。 */
bool vidow_view_capture_held(const struct vidow_view *view);
/* 这个窗口的矩形和它自己那块屏有交集吗（滚出视口的列要按它剔除，见 view.c）。 */
bool vidow_view_in_output_box(struct vidow_server *server,
		const struct vidow_view *view);
/* 把窗口**显示**的部分裁到自己那块屏里。场景图是一张全局坐标的图，跨过屏边界的
 * 窗口，越出去的那一块会落进隔壁屏的框里被它画出来（用户报的「滚动后超出视口的
 * 窗口跑到另一个显示器里面」）。裁的是显示：几何、客户端、录屏抓帧都不动 ——
 * 按窗口录屏渲染的是窗口自己的一份镜像（wincapture.c），不在这棵树上。浮窗 /
 * 正被拖的窗口 / overview / 停到屏外的窗口不裁（它们本来就允许跨屏）。 */
void vidow_view_apply_clip(struct vidow_view *view);
/* 「它现在该停到所有输出框外面吗」：滚出自己那块屏 / 工作区藏起来 / 最小化 → 该停。
 * 判据只有这一份（view_anim_place 每帧现算，workspace.c 的可见性那一趟也问它）。 */
bool vidow_view_should_park(struct vidow_server *server,
		struct vidow_view *view);
/* 停到屏外 / 放回屏里：改 scene_parked 的唯一入口（日志、损伤都在这儿）。 */
void vidow_view_set_parked(struct vidow_view *view, bool park);
/* 视口外强制渲染：这一帧给「不在画面上但要保活」的窗口补帧回调（详见 view.c）。 */
void vidow_view_render_pump(struct vidow_server *server,
		struct vidow_output *output, uint32_t msec);
/* 记一笔「有人拿着这个窗口的画面」（窗口截图源）。源销毁或者客户端跑掉就自动不再
 * 算持有；拿着期间窗口一直强制渲染。 */
void vidow_view_capture_hold(struct vidow_view *view,
		struct vidow_server *server,
		struct wlr_ext_image_capture_source_v1 *source, struct wl_client *client);
/* 给这个窗口建一个截图源（按窗口录屏 / 预览）。**自己的实现**，不是 wlroots 那份
 * 场景节点的：那份按损伤出帧、抓的是「这块坐标里的一切」、还跟着节点位置走，三个
 * 都出过问题（详见 src/wincapture.c 开头）。这里渲染的是窗口自己的一份镜像，
 * 跟它在屏幕上摆在哪、被谁压着、裁没裁都没关系。 */
struct wlr_ext_image_capture_source_v1 *vidow_win_capture_source_create(
		struct vidow_server *server, struct vidow_view *view);

/* 窗口的「取景器」：私有场景 + 一份窗口镜像 + 假输出。按窗口录屏那条路
 * （screencast.c 的 stream_window）也用它出帧 —— 屏幕上是全局坐标，窗口滚到视口外
 * 之后按布局坐标抓就是一片黑，镜像跟位置无关。 */
struct vidow_win_grab;
struct vidow_win_grab *vidow_win_grab_create(struct vidow_server *server,
		struct vidow_view *view);
/* 必须在**表面**的 destroy 里调（见 wincapture.c 里 grab_handle_surface_destroy
 * 上面那段：不能在镜像节点自己的 destroy 信号里拆整棵场景）。 */
void vidow_win_grab_destroy(struct vidow_win_grab *grab);
/* 镜像的外接框（逻辑尺寸）。没内容（客户端还没提交过缓冲）返回 false。 */
bool vidow_win_grab_box(struct vidow_win_grab *grab, int *width, int *height);
/* 这个窗口算在哪块屏上（按窗口中心的布局位置反查，找不到退回 view->output）。
 * **按窗口录屏的流尺寸和窗口抓帧源的尺寸必须用同一份判据**：一边是 zkde 流的
 * 缓冲尺寸，另一边是 portal 报给前端的 size，对不上消费者就把光标位置换算错。 */
struct wlr_output *vidow_win_grab_output(struct vidow_server *server,
		struct vidow_view *view);
/* 渲染一帧：窗口等比缩放摆进 out_w × out_h（剩下的边是黑的），返回**加锁的**缓冲
 * （调用方 unlock）。out_w / out_h 给 0 = 按窗口自己的尺寸（1:1）。不看损伤。 */
struct wlr_buffer *vidow_win_grab_render(struct vidow_win_grab *grab,
		int out_w, int out_h);
struct wlr_swapchain *vidow_win_grab_swapchain(struct vidow_win_grab *grab);
/* 抓帧框（**布局坐标**）+「布局逻辑 → 流像素」的缩放，和 render 里那次等比摆放
 * 是同一套算法：`(布局坐标 - 框原点) × 缩放` = 流里的像素位置。录屏的光标按它
 * 换算（按窗口录屏的缩放跟屏的缩放不是一回事）。窗口停到屏外 / 已经没了返回
 * false。 */
bool vidow_win_grab_frame_box(struct vidow_win_grab *grab, int out_w, int out_h,
		struct wlr_box *box, double *scale);
void vidow_place_view(struct vidow_view *view, int x, int y, int w, int h);

/* 最小化 / 放回（见 view.c）：
 *   vidow_view_set_minimized —— 唯一的状态入口。窗口留在原列里，只是不占位、不显示、
 *     不参与焦点；X11 那边会同步 WM_STATE / _NET_WM_STATE_HIDDEN，Wayland 那边回一次
 *     configure，任务栏（foreign toplevel）跟着更新状态。
 *   vidow_view_restore_last —— 放回「最近收起来的那个」（优先当前聚焦屏上的）。
 *   vidow_view_toggle_minimized —— 收了就放回、没收入就收起（按 id 指认时用）。
 *   vidow_view_restore —— 放回一个 + 给焦点（端出工作区 / 跟到那块屏，和点通知
 *     同一条路：vidow_view_activate）。 */
void vidow_view_set_minimized(struct vidow_view *view, bool minimized);
void vidow_view_restore(struct vidow_view *view);
bool vidow_view_restore_last(struct vidow_server *server);
bool vidow_view_toggle_minimized(struct vidow_view *view);

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

/* -------------------------------------------------- MRU（Alt+Tab 切换器） */

/* 台账（谁最近用过）和切换器都在 mru.c。 */
int64_t vidow_mru_now_ns(void);
bool vidow_mru_init(struct vidow_server *server);
/* 焦点换人了：记账。过了 debounce 才算「用过」（见 mru.c）。 */
void vidow_mru_note_focus(struct vidow_server *server, struct vidow_view *view);
/* 往窗口里打字了：明确使用，debounce 立刻结束。 */
void vidow_mru_note_key(struct vidow_server *server);
/* 窗口要没了：把它从台账和当前清单里摘掉。 */
void vidow_mru_forget(struct vidow_server *server, struct vidow_view *view);
/* 按「最近用过」排一份清单（新的在前），只收能出现在切换器里的窗口。
 * app_id_filter 非空 = 只收这个 app_id 的（同应用切换）。返回个数。 */
size_t vidow_mru_collect(struct vidow_server *server, const char *app_id_filter,
		struct vidow_view **out, size_t max);
/* 选择阶段开着（按住修饰键期间）：键盘归它管，焦点先不动。 */
bool vidow_mru_active(const struct vidow_server *server);
/* 面板显形了（open_delay 到了）：窗口被摆进预览里，画面归它管。 */
bool vidow_mru_showing(const struct vidow_server *server);
/* 这个窗口现在是不是切换器里的一个预览。 */
bool vidow_mru_holds(const struct vidow_server *server,
		const struct vidow_view *view);
/* 打开（dir > 0 = 往后走，对应 Alt+Tab；< 0 = 往前，Alt+Shift+Tab）。
 * 已经开着的时候再调 = 走一格（按住连点 Tab 走的就是这条）。 */
void vidow_mru_open(struct vidow_server *server, int dir, const char *app_id,
		enum vidow_mru_scope scope);
void vidow_mru_step(struct vidow_server *server, int dir);
/* 松开修饰键：焦点给选中的那个，关掉。 */
void vidow_mru_commit(struct vidow_server *server);
/* Esc：关掉，焦点原地不动。 */
void vidow_mru_cancel(struct vidow_server *server);
/* 配置 / 屏幕变了：重排一遍（没显示就什么都不做）。 */
void vidow_mru_update(struct vidow_server *server);
/* 这一帧这个窗口该摆在哪、缩多少（view.c 的摆位 / 重烘都问它）。 */
enum vidow_mru_place vidow_mru_place(const struct vidow_view *view, int *x,
		int *y, double *zoom);

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
/* from_client = 客户端自己发起的（CSD 标题栏拖拽 / _NET_WM_MOVERESIZE）：它的
 * 「按下」已经转给客户端了，松手也要转给它，见 grab_client_press 的说明。 */
void vidow_float_begin_grab(struct vidow_server *server,
		struct vidow_view *view, bool resize, bool from_client);
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
/* 层级收口：平铺 < 浮动 < 全屏（浮动永远压在平铺之上，浮窗之间靠点击换层级）。
 * top 非空 = 「这一个刚被抬起来」，它落在自己那一组的最上面；没人站错队时一个
 * 节点都不碰（见 src/view.c）。 */
void vidow_restack(struct vidow_server *server, struct vidow_view *top);
/* 窗口现在**画在**哪块屏上：它看得见的那些屏里缩放最大的那块（跨屏拖动时布局归属
 * 还没换，人已经在对面那块屏上了）。装饰离屏缓冲的倍率和私有模糊底衬归哪块屏都走
 * 这一个判据。看不见任何屏时返回 NULL。 */
struct wlr_output *vidow_view_display_output(const struct vidow_view *view);
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
/* niri 式列最大化：整列撑满工作区宽度，maximized_saved 记着原来的比例，关掉就还原。
 * 列里每个客户端都会收到 set_maximized 的应答（它们自己也知道自己最大化了）。
 * 状态入口只有这一个 —— 别在别处直接改 col->maximized。 */
void vidow_column_set_maximized(struct vidow_column *col, bool maximized);
/* 聚焦窗口所在的那条列：最大化 ⇄ 还原。没有聚焦窗口 / 浮窗（没有列）时什么也不做。 */
void vidow_column_toggle_maximized(struct vidow_server *server);
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
/* 记下进入 vidow 之前 systemd 用户管理器里的环境（后端是 dbus 按需激活的，
 * 环境从那儿拿）。在 vidow_portal_desktop_env 之后、任何 import 之前调一次。 */
void vidow_portal_env_snapshot(void);
/* 退出前把环境放回去，并重启 xdg-desktop-portal —— 不放回去，回到宿主合成器
 * 以后新拉起来的后端连的是已经消失的 vidow socket。没 import 过就什么都不做。 */
void vidow_portal_env_restore(void);
/* 同步 portals.conf 到 portal 自己的配置目录；内容变了才写、才重启
 * xdg-desktop-portal，但每次都 import-environment（先 import 后重启：后端连的
 * 是用户管理器里的合成器，不导入的话对话框会画到别的桌面上）。 */
void vidow_portal_conf_sync(const struct vidow_config *cfg);

#endif /* VIDOW_SERVER_H */
