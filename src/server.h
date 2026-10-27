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
#include "rule.h"
#include "outputconf.h"
#include "hotplug.h"
#include "clipboard.h"
#include "sessionlock.h"
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
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

/* ext-workspace-v1（src/extworkspace.c）：把工作区 / 输出状态推给 shell。
 * noctalia 的 dock 自动隐藏靠它判断「活动工作区空不空」。 */
void vidow_extworkspace_init(struct vidow_server *server);
void vidow_extworkspace_finish(struct vidow_server *server);
void vidow_extworkspace_sync(struct vidow_server *server);
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
/* 我们自己的 X11 窗口对象与服务端。这里直接 include 而不是前置声明：
 * view / rule / xwmscale 都要读窗口对象的字段（title、class、size_hints
 * 这些），只有完整定义才编得过。x11surface.h 自己不依赖 server.h，没有环。 */
#include "x11surface.h"
struct vidow_jelly;
struct vidow_xdo_manager;
struct wlr_session_lock_manager_v1;
struct wlr_session_lock_v1;

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
	/* 配置里自己写的三次贝塞尔（`easing = "bezier 0.05 0.9 0.1 1.08"`）：形状和
	 * 过冲幅度都从两个控制点来，见 anim.c 的 bezier_at()。追加在最后：前面那几个
	 * 的值会经 IPC 露出去（vidowmsg anims 的 easing），别插在中间把老值挪了。 */
	VIDOW_EASE_BEZIER,
};

/* 一条缓动曲线的完整描述：曲线名 + 它的参数。
 *
 * 为什么不再是一个裸枚举：曲线的参数（贝塞尔控制点、out_back 的回弹幅度）现在
 * 是配置里写的，一个枚举装不下。曲线整份跟着动画走 —— anim.c 的 ease() 是纯函数，
 * 不回头去读配置（配置随时可能重载，动画跑到一半换曲线会跳）。 */
struct vidow_ease {
	enum vidow_easing kind;
	/* kind == VIDOW_EASE_BEZIER：两个控制点 (p1x,p1y) (p2x,p2y)。
	 * x 必须落在 0..1（x(t) 要单调，否则「按 x 求 y」没有唯一解，见 config.c 的
	 * parse_easing），y 随便 —— y > 1 的那部分就是过冲幅度。 */
	double p1x, p1y, p2x, p2y;
	/* kind == VIDOW_EASE_OUT_BACK：回弹幅度（0.10 = 冲过终点 10%）。
	 * has_overshoot == false = 曲线里没写，跟着配置的全局 back_overshoot 走。 */
	double overshoot;
	bool has_overshoot;
	/* 上面那个幅度反解出来的常数（见 vidow_ease_out_back_c1()）：配置解析时算
	 * 一次放在这儿，ease() 每帧直接用 —— 每帧反解一次不值当。 */
	double back_c1;
};

/* Alt+Tab 切换器（见 mru.c）的两个枚举：配置里写字符串
 * （scope 写 "all" / "workspace" / "output"，layout 写 "strip" / "grid"），
 * 落到这里就是整数。 */
enum vidow_mru_scope { VIDOW_MRU_ALL, VIDOW_MRU_WORKSPACE, VIDOW_MRU_OUTPUT };
enum vidow_mru_layout { VIDOW_MRU_STRIP, VIDOW_MRU_GRID };

/* 焦点列在视口里的居中策略（niri 的 `center-focused-column`，见
 * configs/20-layout.lua 和 view.c 的 output_arrange Pass 2）。配置里写字符串，
 * 落到这里就是整数。 */
enum vidow_center_focused_column {
	/* 默认，也是 vidow 一直以来的行为：换焦点只滚动「刚好让焦点列整个露出来」
	 * 的那点距离，焦点列因此常常贴着视口左 / 右边缘。 */
	VIDOW_CENTER_NEVER = 0,
	/* 焦点列永远摆到视口正中（条带两头不够居中时夹到边界，见 view.c）。 */
	VIDOW_CENTER_ALWAYS,
	/* 焦点列和「上一个焦点列」能一起完整放进视口时不动；放不下（= 溢出）才
	 * 居中。niri 的同名取值。 */
	VIDOW_CENTER_ON_OVERFLOW,
};

/* 列宽预设表最多几档（niri 的 preset-column-widths 是个可长可短的表；vidow 的
 * 配置值是一行文本，用一个定长数组收下）。 */
#define VIDOW_MAX_COLUMN_WIDTH_PRESETS 16

/* 能写 layout{} 的屏最多几块（vidow.output{ name=…, layout={…} } 一个名字一条）。 */
#define VIDOW_MAX_OUTPUT_LAYOUTS 16

/* 一块屏的 layout{} 覆盖（niri 的 `output "DP-3" { layout { … } }`）：一块屏一组，
 * **只记写了的项**（has_* 那一排），没写的继承全局 —— 这就是「没配就继承」的实现
 * 方式。配置里写在 vidow.output{ name = "DP-3", layout = { gaps = 20 } }，平铺键
 * output_layout 是它的文本写法（见 config.c 的 parse_output_layout）。 */
struct vidow_output_layout {
	char name[64];
	bool has_gaps;        int gaps;
	bool has_gaps_top;    int gaps_top;
	bool has_gaps_bottom; int gaps_bottom;
	bool has_gaps_left;   int gaps_left;
	bool has_gaps_right;  int gaps_right;
	bool has_gaps_inner;  int gaps_inner;
	bool has_center_focused_column;
	enum vidow_center_focused_column center_focused_column;
	bool has_preset_column_widths;
	double preset_column_widths[VIDOW_MAX_COLUMN_WIDTH_PRESETS];
	int preset_column_widths_count;
	bool has_default_column_width;
	double default_column_width;
};

/* 一块屏最终生效的那份布局参数（全局值 + 该屏 layout{} 覆盖）。布局 / 列宽那边
 * 拿这个算，不再直接读 server->config 里的那几个字段 —— 见 vidow_layout_for()。 */
struct vidow_layout {
	int gaps, gaps_top, gaps_bottom, gaps_left, gaps_right, gaps_inner;
	enum vidow_center_focused_column center_focused_column;
	double preset_column_widths[VIDOW_MAX_COLUMN_WIDTH_PRESETS];
	int preset_column_widths_count;
	double default_column_width;
};

/* libinput 那些开 / 关设置的取值（configs/19-input.lua；套到设备上的地方在
 * input.c 的 vidow_input_apply_libinput）。
 *
 * 为什么不是一个 bool：vidow 以前**一个 libinput 设置都不碰**，而 libinput 的
 * 默认值是按设备 / udev 规则来的（同一台机器上，触控板的 tap 默认和鼠标的不是
 * 一回事），照抄一个常数就等于替用户改了他没配的东西 —— 「不配置时零变化」这条
 * 要求只能靠「不设置」本身来满足。 */
enum vidow_tristate {
	VIDOW_TRI_AUTO = 0, /* 没配：不动这台设备，用 libinput 自己的默认 */
	VIDOW_TRI_ON,
	VIDOW_TRI_OFF,
};

/* 一个「可以不设」的数（accel_speed）：同上，libinput 的默认值随设备走，而且
 * 0.0 和「没配」不是一个意思。set == false = 不动。 */
struct vidow_opt_double {
	double value;
	bool set;
};

struct vidow_config {
	/* 主屏（输出连接器名，如 "DP-2"）。空 = 布局里的第一块屏。
	 * 指针初始位置和新窗口默认落点都用它 —— 以前两处都按布局原点算，
	 * 双屏且主屏在右边时指针会从最左边冒出来（用户报的）。 */
	char *primary_output;
	/* 客户端请求最大化时兑现吗。默认 false：Qt/KDE/Electron 会恢复「上次是
	 * 最大化」，兑现了就「一开就占满整列」。 */
	bool honor_maximize_request;
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
	/* 列宽变化的过渡（cycle-column-width / 列最大化 / IPC 改宽）。0 = 直接到位。
	 * 拖动改宽（Mod+右键拖）不走它 —— 那个必须跟手。见 view.c 的
	 * vidow_column_set_width。 */
	int column_width_ms;
	/* ---- 布局（configs/20-layout.lua）----
	 * 焦点列在视口里的居中策略（niri 的 center-focused-column）。默认
	 * VIDOW_CENTER_NEVER = vidow 一直以来的「最小滚动」行为，不配置零变化。
	 * 见 view.c 的 output_arrange Pass 2。 */
	enum vidow_center_focused_column center_focused_column;
	/* 列宽预设表：cycle-column-width（Mod+R）在这几档之间循环，单位是「可用
	 * 宽度的比例」（niri 的 preset-column-widths 里的 proportion）。默认
	 * 1/3、1/2、2/3 —— 就是原来写死在 view.c 的那三个。 */
	double preset_column_widths[VIDOW_MAX_COLUMN_WIDTH_PRESETS];
	int preset_column_widths_count;
	/* 每屏 layout{} 覆盖（niri 的 output { layout { … } }）：一块屏一条，只记
	 * 写了的项，没写的继承上面那几个全局值（见 vidow_layout_for）。写在
	 * vidow.output{ name=…, layout={…} }，或平铺键 output_layout（文本写法）。 */
	struct vidow_output_layout output_layouts[VIDOW_MAX_OUTPUT_LAYOUTS];
	int output_layouts_count;
	/* output_layout 的文本形式（vidowmsg config output_layout /
	 * VIDOW_OUTPUT_LAYOUT 读写的就是这一份；写进去当场解析成上面那张表）。 */
	char output_layout[512];
	/* 按需 VRR（niri 的 `variable-refresh-rate on-demand=true`）：这块屏默认
	 * 「有窗口要才提刷新率」，静止就降回来。默认 false = 不碰 VRR（vidow 以前
	 * 的行为：vidow.output{ vrr = true } 静态开）。见 output.c 的
	 * vidow_output_vrr_on_demand_update。 */
	bool vrr_on_demand;
	/* 新列（窗口规则没写 column_width 的）开出来多宽，比例。默认 0.5 —— 就是
	 * 原来 view.c 里那个 DEFAULT_COLUMN_WIDTH。niri 的 default-column-width。 */
	double default_column_width;
	/* 切工作区的过渡时长（0 = 直接换，不动画）。旧的推开、新的从另一边进来，
	 * 见 workspace.c 的 slide_start。 */
	int workspace_switch_ms;
	/* 切工作区的缓动；和全局 easing 分开，别的布局动画不受影响。 */
	struct vidow_ease workspace_easing;
	struct vidow_ease column_width_easing;
	int layer_open_ms;
	int layer_close_ms;
	struct vidow_ease layer_open_easing;
	struct vidow_ease layer_close_easing;
	struct vidow_ease easing;
	/* 动画两大概括：window / layer 的组默认值。具体动画的字段仍然
	 * 单独存在（下面那些），组里的 ms/easing 在配置解析时统一铺到它们
	 * 上面；没写的具体项保留原默认值。 */
	int window_anim_ms;
	struct vidow_ease window_anim_easing;
	int layer_anim_ms;
	struct vidow_ease layer_anim_easing;

	/* 普通布局重排（开/关窗、换列、滚动）里每个窗口位移补间的曲线。
	 * 和 workspace_easing 分开：工作区过渡是整条带子，普通布局是各窗口
	 * 自己的补间。默认 out_cubic，和原来一致。 */
	struct vidow_ease window_move_easing;

	double open_alpha_from; /* opacity at t=0 while opening */
	int open_slide_px;      /* slide-in distance while opening */
	double close_alpha_to;  /* opacity at t=1 while closing */
	int close_slide_px;     /* slide-out distance while closing */

	/* 每个阶段自己的缓动曲线：默认 out_elastic —— 入场和关闭会过冲一下再
	 * 收住。上面那个 easing 只管位移补间、layer surface 和 overview：布局重排
	 * 要的是「快而稳」，过冲反而晃眼。 */
	struct vidow_ease open_easing;
	struct vidow_ease close_easing;
	/* 回弹幅度：out_back 冲过终点多少（0.10 = 10%）。三个曲线都能用 —— 曲线自己
	 * 写了幅度（`easing = "out_back 0.2"`）的以曲线为准，这里管没写的那些。
	 * 见 config.c 的 sync_easings()。 */
	double back_overshoot;
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
	/* 圆角的形状指数：2 = 圆弧（原来那种），4 = 超椭圆（squircle）。默认 4 ——
	 * 和 Alt+Tab 的 MRU 卡片（mru.c 的 MRU_SQUIRCLE_N）、portal 选源对话框
	 * （tools/vidowportal.c 的 SQUIRCLE_N）同一种形状。**三处都是 4.0，改要
	 * 一起改**（那两处是 CPU 光栅 / Cairo 路径，没有 uniform 通道）。夹在
	 * 2 ~ 8，越界由 vidow_corner_n_clamp() 夹住（见 rule.h）。 */
	double corner_n;
	double border_width;
	float border_color[4];       /* straight (non-premultiplied) rgba */
	float border_color_focus[4];
	double shadow_radius;
	float shadow_color[4];
	int shadow_offset_x, shadow_offset_y;
	double window_opacity;
	double noise;

	/* 窗口抓帧（按窗口录屏 / 窗口截图源）里带不带「屏上那圈画框 + 背后的模糊
	 * 底衬」。默认开：录出来就跟屏幕上看到的一样。关掉 = 只有客户端表面本身
	 * （Hyprland 的窗口导出那种口径，给只想要裸窗口内容的用家，比如 OBS 的
	 * 窗口源自己再加边框）。见 wincapture.c。 */
	bool wincapture_decor;

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
	/* 弹出物（xdg_popup / 菜单 / 输入法候选框）背后的模糊单独开关。
	 * 默认开；关掉之后 popup 后面就是透的（不画底衬，也不分配离屏目标）。 */
	bool popup_blur_enabled;
	/* 弹出物模糊的透明度剔除阈值（0 = 用各自动的默认值：普通菜单 1/255、
	 * 输入法候选框 0.35）。> 0 时统一用它 —— 半透明菜单体边缘太糊就调高。 */
	double popup_blur_alpha_min;
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

	/* 逻辑尺寸统一：所有屏的逻辑分辨率对齐到最大那块（见 output.c 的
	 * output_unify_target）。打开后每块屏的 scale 由「物理尺寸 / 基准逻辑」
	 * 反推 —— 1080p 屏会被虚拟放大（超采样），换来窗口跨屏不跳大小、
	 * X 屏幕和布局永远对得上账。默认关。 */
	bool logical_uniform;

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

	/* 抓帧兜底的节奏（Hz，见 output.c 的 capture_watchdog）。屏幕静止时
	 * 没有输出提交，录屏 / 远程控制全靠它按这个频率催帧。默认 60；0 = 关掉
	 * 兜底（只靠后端自己提交）。环境变量 VIDOW_CAPTURE_FPS 优先。 */
	int capture_fps;

	/* 摇晃找光标（见 cursorfx.c）：快速来回晃几下指针，指针就地放大一会儿。
	 * 放大是换一张更大的光标图，走的是硬件光标那条路 —— 放得下就不进合成器
	 * 的渲染通道，所以不会出现在截图里；放不下会被 wlroots 降级成软光标，所以
	 * 倍数会按硬件上限先夹一下（cursor_shake_scale 是想要的倍数）。 */
	bool cursor_shake;
	double cursor_shake_scale;

	/* 光标主题名 / 逻辑尺寸（config 的 cursor_theme / cursor_size）。空串 =
	 * 沿用老行为：主题先看 XCURSOR_THEME、再 gsettings、最后 Adwaita；尺寸先看
	 * XCURSOR_SIZE、再 gsettings、最后 24。改这两项会在运行中**重建** cursor
	 * manager（见 vidow_cursor_apply），不用重启。 */
	char cursor_theme[64];
	int cursor_size;

	/* 焦点跟随光标 / 键盘换焦点时把光标带过去（照 niri 的 input 那一节，
	 * 键名 / 取值 / 默认值都照它）。
	 *
	 * focus_follows_mouse = niri 的 input { focus-follows-mouse }：指针压到
	 * 哪个窗口，哪个窗口就接键盘。默认 false（niri 不写这个节点就是不跟）。
	 * focus_follows_mouse_max_scroll_amount = 那个节点的 max-scroll-amount
	 * 子项：聚焦会让条带滚动多少才算「能跟」。写 "10%" 这种百分比（工作区
	 * 宽度的百分之几），"0%" = 只有已经整个在屏上的窗口才跟，留空 = 不限
	 * （niri 不写子项就是这个意思）。值按原样存着，`vidowmsg config` 打出来
	 * 能原样抄回配置。
	 *
	 * warp_mouse_to_focus = niri 的 input { warp-mouse-to-focus }：**键盘**
	 * 换焦点时把光标挪到新窗口上（鼠标悬停聚焦不挪 —— 光标本来就在那儿）。
	 * 默认 false。warp_mouse_to_focus_mode 是它的 mode 子项：留空 = niri 的
	 * 默认「X / Y 分别判断」（只往需要的那个轴挪），"center-xy" /
	 * "center-xy-always" 和 niri 同名同义。 */
	bool focus_follows_mouse;
	char focus_follows_mouse_max_scroll_amount[16];
	bool warp_mouse_to_focus;
	char warp_mouse_to_focus_mode[24];

	/* ---- libinput 设备配置（configs/19-input.lua，套上去的地方在 input.c）----
	 * 照 niri 的 input {} 那一节，键名转成 vidow 的 snake_case。
	 *
	 * 默认值 = **vidow 以前的行为**：一个 libinput 设置都不碰（三态是 AUTO、
	 * accel_profile / scroll_method 是空串、accel_speed 没设），所以不配置时
	 * 与以前逐字节一致。scroll_factor 是 vidow 自己乘的倍率（libinput 没这个
	 * 接口），1.0 = 不缩放，也是以前的行为。
	 *
	 * 这些是**设备属性**，不是布局参数：套一次就存在设备上，所以设备热插拔、
	 * 配置重载都要重套（见 vidow_input_apply_all 和 config.c 的观察者）。 */
	enum vidow_tristate tap;              /* 点按 = 左键 */
	enum vidow_tristate dwt;              /* 打字时关掉触控板 */
	enum vidow_tristate natural_scroll;   /* 反向滚动 */
	enum vidow_tristate left_handed;      /* 左右键对调 */
	enum vidow_tristate middle_emulation; /* 左右键同按 = 中键 */
	enum vidow_tristate drag_lock;        /* 点按拖拽：松手不立刻放手 */
	char accel_profile[16];               /* "" = 不设；adaptive / flat */
	struct vidow_opt_double accel_speed;  /* -1.0 .. 1.0；没设 = 不动 */
	char scroll_method[24];               /* "" = 不设；no-scroll / two-finger / edge / on-button-down */
	char click_method[24];                /* "" = 不设；button-areas / clickfinger */
	double scroll_factor;                 /* 滚动倍率，1.0 = 不缩放 */

	/* 光标隐藏（configs/13-cursor.lua，照 niri 的 cursor {} 那一节）。
	 *
	 * 默认值 = **vidow 以前的行为**：两个都不藏（光标一直在，跟以前一样）。
	 * hide_when_typing = niri 的 cursor { hide-when-typing }：一打字就把光标
	 * 藏起来（指针一动就回来）。hide_after_inactive_ms = 那个节点的
	 * hide-after-inactive-ms：指针静止这么久（毫秒）就藏，0 = 不藏。
	 * 实现见 cursorfx.c 的 vidow_cursor_fx_set_hidden + input.c 的定时器。 */
	bool hide_when_typing;
	int hide_after_inactive_ms;

	/* 键盘自己的参数（configs/21-keyboard.lua）。
	 *
	 * repeat_delay / repeat_rate 就是改前 input.c 里写死的 600 / 25（毫秒 /
	 * 每秒几次），只是现在能配了 —— 它们通过 wl_keyboard.repeat_info 传给
	 * 客户端，**自动重复是客户端自己做的**（wlroots 0.20 不在合成器里重复，
	 * 只把这两个数报给客户端），所以 vidow 自己的键位看不到重复事件。
	 * numlock = niri 的 input { keyboard { numlock } }：起手把 NumLock 点亮。
	 * niri 默认 true，vidow 默认 false —— 改前 vidow 一个键盘状态都不碰。
	 * mod_key = niri 的 mod-key：键位里写的 `Mod` 到底是哪个键（super / alt）。
	 * niri 还有个 mod-key-nested（嵌套里的 Mod 换成另一个键），**没做**。 */
	int repeat_delay;
	int repeat_rate;
	bool numlock;
	char mod_key[16];

	/* 输入唤醒关掉的屏（Hyprland 的 misc { mouse_move_enables_dpms /
	 * key_press_enables_dpms }）：指针一动 / 按键一下就把「关着的屏」打开。
	 *
	 * 默认都是 false = **vidow 以前的行为**：屏一旦用 `vidowmsg output-power
	 * ... off`（或者后端因为合盖 / 省电）关掉，只有再发一条 IPC 才会回来，
	 * 输入不会唤醒它。设 true 才有「一动就亮」。见 input.c 的
	 * vidow_output_wake_disabled。 */
	bool mouse_move_enables_dpms;
	bool key_press_enables_dpms;

	/* 切到「当前就在的」那块工作区时，改成切回**上一个**工作区
	 * （niri 的 workspace-auto-back-and-forth）。默认 false = 改前行为：
	 * 切到当前工作区 = 什么都不做。记账在 keybind.c 的动作那条路上。 */
	bool workspace_auto_back_and_forth;

	/* 剪贴板（见 clipboard.c）：X11(Xwayland) 与 Wayland 两边共用一份内容。
	 * clipboard = false 就不接管，退回 wlroots 自带那套（只在有 X11 窗口聚焦
	 * 时才同步）；clipboard_primary 决定选区（PRIMARY）要不要一起同步；
	 * clipboard_max_bytes 是每个 mime 类型读进内存的上限 —— 「源程序退出后还能
	 * 粘贴」靠的就是这份内存副本。 */
	bool clipboard;
	bool clipboard_primary;
	int clipboard_max_bytes;
	/* 历史：clipboard_history 是保留条数（0 = 不留历史），
	 * clipboard_history_bytes 是这份历史的总内存预算。两个上限谁先到谁先淘汰。 */
	int clipboard_history;
	int clipboard_history_bytes;

	/* xdg-desktop-portal 后端选择（见 portalconf.c）。portal_desktop 是 vidow
	 * 设给自己的 XDG_CURRENT_DESKTOP，也是 <name>-portals.conf 的那个 name；
	 * 留空 = 完全不碰 portal 那一摊。 */
	bool portal_conf_sync;
	char portal_desktop[32];
	/* 录屏 / 截图交给哪个后端：auto（默认，自己人优先）/ vidow / kde / wlr /
	 * hyprland / none。跟 portal_desktop（会话名）是两件事。 */
	char portal_capture[32];
	/* 会话环境（见 env.c）：启动时从 systemd 用户管理器吸环境、把 vidow 的环境
	 * 推回给 systemd / dbus（由 systemd 拉起来的应用才看得到 vidow）。 */
	bool systemd_env;
	bool systemd_env_export;

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
	 *   card_*         每一项背后那张卡片（超椭圆圆角）：内边距 / 圆角半径 /
	 *                  底色 / 描边宽度与颜色
	 *   highlight_*    选中那张卡片：描边宽度与颜色（底色 = 卡片底混一点它） */
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
	/* 显形 / 收起的过渡时长（毫秒）。0 = 不做过渡，瞬间到位。 */
	int mru_open_ms;
	int mru_close_ms;
	/* 换选中项时卡片位移的过渡时长（毫秒），0 = 瞬间跳过去。
	 * 和 mru_open_ms 是两回事：那个管面板显形，这个管「按 Tab 之后卡片
	 * 从旧位置滑到新位置」。 */
	int mru_slide_ms;
	int mru_card_padding;
	int mru_card_radius;
	float mru_card_color[4];
	int mru_card_border;
	int mru_hover_border;
	float mru_card_border_color[4];
	float mru_hover_color[4];
	int mru_highlight_border;
	float mru_highlight_color[4];

	/* 全景（overview，见 src/overview.c）：压在壁纸之上、窗口之下的那层遮罩。
	 * 颜色带 alpha（RRGGBBAA），alpha 就是「压多暗」，0 = 不压暗。 */
	float overview_dim_color[4];
	/* 全景里把壁纸糊掉（niri 那样）：模糊的那张壁纸直接摊满整块屏，垫在遮罩
	 * 下面。关掉（或者 blur_enabled = false / 没有可用底衬）就只剩压暗。 */
	bool overview_blur;

	/* 桌面底色：没有壁纸客户端（layer-shell 的 background 层）时垫在最底下的
	 * 颜色，有壁纸客户端时被它盖住。RRGGBB 或 RRGGBBAA。 */
	float background_color[4];

	/* ---- 小窗（mini window，configs/24-mini.lua）----
	 * 小窗是浮动窗口上的一个形态（见 enum vidow_mini_state）：迷你 / 图标 /
	 * 自由小窗。S1 只铺数据模型 + 配置 + IPC，形态还不会自动切换、渲染与
	 * 输入路由未动。 */
	bool mini_enabled;         /* 总开关，false = 这套东西完全不生效 */
	bool mini_drop_zone;       /* 拖拽时亮落点区 */
	char mini_drop_zone_w[16]; /* 落点区宽（工作区百分比，如 "15%"） */
	char mini_drop_zone_h[16];
	char mini_w[16];           /* 迷你态尺寸（工作区百分比，如 "28%"） */
	char mini_h[16];
	char mini_free_w[16];      /* 自由小窗的初始尺寸（工作区百分比） */
	char mini_free_h[16];
	int mini_icon_size;        /* 图标边长（逻辑像素） */
	double mini_icon_overflow; /* 图标沿贴边那一侧出屏的比例，0..0.6 */
	int mini_icon_hover_ms;    /* 图标悬停多久出预览 */
	int mini_icon_hover_leave_ms;
	int mini_icon_hover_grace_ms;
	bool mini_icon_preview;    /* 悬停预览开关 */
	bool mini_toolbar;         /* 悬停工具条开关 */
	int mini_edge_snap_px;     /* 拖到屏边多少像素内算「贴边」 */
	/* 甩出（S5）：松手时指针速度 ≥ 这个值（逻辑像素 / 秒）= 甩出。0 = 关掉。
	 * 速度按最近 100ms 的采样算（不是单帧增量，见 input.c 的采样环）。 */
	int mini_fling_speed;
	int mini_transition_ms;    /* 形态转换时长（0 = 不做动画） */
	int mini_hover_fade_in_ms;
	int mini_hover_fade_out_ms;
	bool mini_remember_size;   /* 记住 FREE 的尺寸（false = 每次用初始比例） */
	int mini_edge_margin;      /* 图标 / 迷你态离屏边的最小留白 */
	/* 保留客户端原生分辨率：小窗态只是**呈现**变了，由合成器缩放显示。
	 * false = 小窗态按小窗尺寸重配客户端（省显存，但有的游戏一 resize 就
	 * 重置成窗口模式）。**不要**再加一个取反的 mini_resize_client，两个键会打架。 */
	bool mini_preserve_buffer;
	/* 自由小窗的宽高比策略："auto" / "lock" / "free"（默认 "auto"）。
	 * 三个取值都是强制语义，优先级：键位 > 窗口规则 > 这个全局键。
	 * "auto" = 让自动信号决定（X11 有 PAspect / min==max 提示就锁，否则
	 * free）；lock / free 显式压过自动信号。写错的值当场报错且**值不变**。
	 * 只对自由小窗有效 —— 迷你 / 图标态永远按内容比例。 */
	char mini_aspect[16];
	/* 迷你态「单击 = 放大」vs「拖拽 = 移动」的位移阈值（逻辑像素）。迷你态
	 * 要同时是「点一下放大」和「拖着挪位置」，只能按位移分：指针按下后走了
	 * 这么多像素才算拖拽，没走到就松手当单击。 */
	int mini_drag_threshold_px;
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

/* 列宽：比例 → 像素。niri 的 resolve_column_width：
 *     width = (working_area.w - gaps) * proportion - gaps
 * —— 比例分的是**连 gap 一起算**的那块区域，窗口 = 那块区域减掉一个内部 gap。
 * niri 的 working_area 没有预先扣 gap，vidow 的可用区（vidow_work_area）已经把
 * 左右两道 gap 扣掉了，同一个式子换算过来就是 (可用宽 + gap) * 比例 - gap：
 * 两者一模一样，只是坐标系差着左右各一个 gap。
 *
 * 这么分的好处：Σ比例 = 1 时，几条列连着中间的 gap 正好铺满可用区 —— 两条 0.5
 * 不会被 gap 挤出去一格（以前宽 = 可用宽 × 比例，两条 0.5 加起来比可用区多一个
 * gap，得滚动才看得全第二条）。纵向本来就没这个问题：n 块窗口的高度是
 * (可用高 - (n-1) * gap) / n，从第一天起就是这么分的。 */
static inline int vidow_column_width_px(const struct vidow_config *cfg,
		int work_width, double fraction) {
	int inner = vidow_gap_inner(cfg);
	int w = (int)((work_width + inner) * fraction) - inner;
	return w > 1 ? w : 1;
}

/* 上面那条的逆（拖动改宽：像素 → 比例）。niri 的 set_column_width 里
 * proportion = (px + gaps) / (working_area.w - gaps) 是同一个式子的另一头。 */
static inline double vidow_column_fraction_of_px(const struct vidow_config *cfg,
		int work_width, double px) {
	int inner = vidow_gap_inner(cfg);
	int full = work_width + inner;
	return full > 0 ? (px + inner) / (double)full : 1.0;
}

/* 上面那几个的「每屏版」：参数是 vidow_layout_for() 算出来的那块屏的最终布局值
 * （全局 + 该屏 layout{} 覆盖）。算法一字不差，只是值可能来自覆盖表。 */
static inline int vidow_layout_gap_side(const struct vidow_layout *lay, int side) {
	return vidow_gap_resolve(side, lay->gaps);
}

static inline int vidow_layout_gap_inner(const struct vidow_layout *lay) {
	return vidow_gap_resolve(lay->gaps_inner, lay->gaps);
}

static inline int vidow_layout_column_width_px(const struct vidow_layout *lay,
		int work_width, double fraction) {
	int inner = vidow_layout_gap_inner(lay);
	int w = (int)((work_width + inner) * fraction) - inner;
	return w > 1 ? w : 1;
}

/* 这块屏最终生效的布局参数（全局 + 该屏 layout{} 覆盖；output 可以是 NULL = 只
 * 要全局那份）。定义在 config.c。 */
void vidow_layout_for(const struct vidow_server *server,
		const struct wlr_output *output, struct vidow_layout *out);

/* 按需 VRR（niri 的 variable-refresh-rate on-demand）：这块屏上有没有「要 VRR」
 * 的窗口显示着，据此把 adaptive sync 提上去 / 降回来。没开按需模式的屏什么都不
 * 做。定义在 output.c。 */
void vidow_output_vrr_on_demand_update(struct vidow_server *server,
		struct wlr_output *wlr_output);

/* output_layout 文本写法（`DP-3: gaps=20; center_focused_column=always | DP-2: gaps=8`）
 * → 每屏覆盖表；写完把文本重新规范化一遍（读回来就是同一个写法）。定义在
 * config.c。 */
bool vidow_output_layout_apply_text(struct vidow_config *cfg, const char *text,
		char *err, size_t errlen);

/* --------------------------------------------------------------- animation */

/* One running animation. Progress comes from CLOCK_MONOTONIC. */
struct vidow_anim {
	struct wl_list link; /* vidow_server.anims */
	struct vidow_server *server;
	int64_t start_ns;
	int64_t duration_ns;
	const char *tag; /* for logs */
	struct vidow_ease easing;
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

/* 果冻的「内容 + 屏幕空间模糊」一遍合成：内容 UV 跟形变网格走，模糊 UV 按
 * 顶点当前屏幕坐标算。片元就是 content + blur * (1 - content.a)，和装饰着色器
 * 最后那条合成公式一致。 */
struct vidow_mesh_blur_prog {
	unsigned int program;
	int u_texture, u_blur, u_size, u_blur_strength;
	int u_src_size, u_content_off, u_content_size, u_radius, u_corner_n;
	int u_mask, u_mask_on, u_mask_map, u_mask_min;
	int u_shadow, u_shadow_off, u_shadow_size, u_shadow_offset;
	int u_shadow_radius, u_shadow_color, u_shadow_on;
};

/* The built-in decoration program: rounded corners, border, drop shadow,
 * blurred backdrop and grain. */
struct vidow_decor_prog {
	unsigned int program;
	int u_texture, u_blur, u_size, u_target_size, u_content_off, u_content_alpha;
	int u_radius, u_corner_n, u_border_width, u_border_color, u_shadow_color;
	int u_shadow_offset, u_shadow_radius, u_shadow_only, u_opacity, u_noise,
		u_alpha;
	int u_origin, u_output_size, u_flip_y;
	int u_blur_scale, u_blur_bias;
	/* 跨屏分段采样（见 fx.c 的 decor_fragment_src）：u_split 是分界在窗口内的 x，
	 * 小于 0 表示不跨屏；u_origin2 / u_output_size2 是第二块屏那套坐标。 */
	int u_split, u_origin2, u_output_size2;
	int u_blur_strength;
	int u_blur_region, u_blur_region_on;
	/* 内容贴图在装饰矩形里的采样映射（见 fx.c 的 decor_fragment_src）。 */
	int u_tex_map;
	/* 元素自己的 alpha（「透明度剔除」，见 fx.c 的 decor_fragment_src）。 */
	int u_mask, u_mask_on, u_mask_min;
	/* mask 的采样映射（见 struct vidow_fx_target 的 mask_*）。 */
	int u_mask_map;
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

/* Alt+Tab 切换器（niri 的 recent windows）。台账和切换器都在 mru.c，这里只放
 * 状态：台账一个 pending + 一个 debounce 闹钟；切换器一张清单（order）、一份
 * 排版（slot）、选中项和面板那几个矩形。 */
struct vidow_mru_slot {
	struct vidow_view *view;
	int x, y, w, h;   /* 预览摆在哪（布局坐标）+ 尺寸，都是屏幕上的值 */
	int cx, cy, cw, ch; /* 这项背后那张卡片（= 预览 + 内边距），鼠标按它命中 */
	double zoom;      /* 内容缩放（1.0 = 原尺寸） */
};

/* 换选中项时「这张卡片刚才画在哪儿」（见 vidow_mru.slide_from 的说明）。 */
struct vidow_mru_slide {
	struct vidow_view *view;
	int cx, cy, cw, ch;
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
	struct wlr_scene_tree *tree;    /* 浮层：压暗 + 卡片 + 缩略图，压在窗口之上 */
	struct wlr_scene_rect *backdrop; /* 压暗背景 */
	/* 进 / 出的过渡进度（0 = 完全没显形，1 = 摆到位）。跟 overview 的 ov->t
	 * 同一个意思、同一套用法：mru_scene_apply 按 t 插值卡片的位置 / 缩放 /
	 * 透明度，vidow_anim_tick 每帧推。
	 *
	 * 以前这里是**瞬时的**：open_delay 一到就把面板整块摆上去、关掉时整块撤掉，
	 * 画面上一帧之间跳过去（用户：「MRU 需要过渡动画」「移动的过程中也要过渡」）。
	 * t 只影响**画出来什么样**，不影响 mru_layout 算出来的目标几何 —— 布局
	 * 仍然是瞬时的，动画只是把「从哪儿来 / 往哪儿去」补出来。 */
	double t;
	struct vidow_anim *anim;        /* 推 t 的那条 */
	bool closing;                   /* 这条动画在往 0 走（关）还是往 1 走（开） */

	/* ---- 换选中项时的位移过渡 ----
	 *
	 * 上面那个 t 只管「面板显形 / 收起」。**换选中项是另一回事**：按一下 Tab
	 * 清单就重排了，每张卡片的目标位置当场变成新值，卡片一帧之间跳过去
	 * （用户：「MRU 的窗口卡片移动没有过渡动画」）。
	 *
	 * 所以每次重排前把「现在画在哪儿」抄一份存这儿，重排后按 slide 从旧位置
	 * 插到新位置。存的是**卡片框**（cx/cy/cw/ch），不是缩略图 —— 缩略图跟着
	 * 卡片走，插值一次就够。
	 *
	 * 键是窗口指针：清单顺序变了、项被挤出视口再挤回来，位置都还能对上。
	 * 找不到旧位置（新冒出来的项）就从目标位置开始，不插值。 */
	struct vidow_mru_slide *slide_from;
	int slide_from_n;
	double slide;                   /* 0 = 全在旧位置，1 = 全在新位置 */
	struct vidow_anim *slide_anim;  /* 推 slide 的那条 */
	/* 每项背后那张卡片（超椭圆圆角的底 + 描边，见 mru.c）。节点按需建、建了留着
	 * 复用；圆角是按覆盖率算出来的小图（wlr_scene_rect 画不出圆角），尺寸或样式
	 * 变了才重画。 */
	struct vidow_mru_card **card;
	int card_n;
	struct vidow_view *hover;       /* 鼠标底下的那一项（只影响样式，不改选中） */
	bool swallow_release;           /* 点选那一下的松手也吃掉（别漏给客户端） */
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

/* 跨屏时窗口的底衬要**按屏各存一份**（照 Hyprland：它的模糊纹理 m_blurFB 是
 * 每块显示器一张，渲染时取「当前正在画的那块」的那张，没有「这个窗口归哪块
 * 屏」的说法）。
 *
 * Vidow 的窗口装饰是离屏烘的一张图，上面烘死了「从哪个原点、按什么尺寸采样」
 * —— 窗口骑在两块屏中间时，两块屏的采样原点差着一整屏，共用一份字段就是互相
 * 覆盖：先渲染的那块屏拿到后渲染那块屏的坐标，屏幕上就成了用户 2026-10-23 报
 * 的「一半正常圆斑、另一半被拉成横条」。所以每块屏各存一格。
 *
 * Instances form a dynamic list: large windows may intersect any number of
 * outputs. Each instance owns its scene node, targets and private blur chain. */
struct vidow_blur_slot {
	struct vidow_blur_slot *next;
	struct wlr_output *out;  /* NULL = 空闲格 */
	/* Layout box of that output (logical coords), snapshotted while the
	 * instance is updated: querying the layout box from the render path
	 * returned nothing for the second output. */
	struct wlr_box layout_box;
	struct wlr_texture *tex; /* NULL = 这块屏这一趟还没有底衬 */
	bool owned;
	uint64_t id;
	int rx, ry, rw, rh;      /* 区域，设备像素，相对这块屏的原点 */
	int ow, oh, ox, oy;      /* 这块屏的视野（尺寸 + 布局原点，设备像素） */
	/* **这一块屏自己的装饰缓冲**（整窗那么大，按这块屏的倍率烘）。
	 * 两块屏倍率不同（1x / 2x）时缓冲尺寸差一倍：共用一对就得每帧在两套
	 * 尺寸之间来回扔掉重建，而且先烘的那块屏的图会被后烘的覆盖 —— 屏幕
	 * 上就是「一半正常、一半糊」。 */
	struct vidow_blur_set blur_set;
	struct wlr_buffer *own_bd;
	struct wlr_texture *own_bd_tex;
	struct wlr_texture *blur_own_tex;
	uint64_t blur_own_id;
	int own_bd_w, own_bd_h;
	struct wlr_scene_buffer *node;
	struct wl_listener node_destroy;
	struct wlr_buffer *anim[2];
	struct wlr_buffer *dec[2];
	int dec_index;
	int dec_w, dec_h;        /* 渲染目标尺寸，设备像素 */
	int dec_content_w, dec_content_h;
	int dec_px_pad;
	double dec_scale;        /* 烘这张图用的倍率 */
	int dec_dst_w, dec_dst_h; /* 场景节点尺寸（逻辑单位） */
	float mask_saved_opacity;
	bool capture_enabled;
	bool dec_valid;          /* Cached decoration was successfully rendered. */
	uint64_t dec_revision, dec_blur_ver;
	double dec_output_scale;
	struct wlr_box dec_output_box;
	/* 烘这张图时窗口**画在哪儿**（geom + anim_off，布局坐标）。
	 *
	 * 拖拽只动 anim_off、不动 geom，也不推进 bake_revision（那条只在
	 * vidow_view_apply_anim 里走），所以下面那串缓存判据在拖动全程一个都
	 * 不变 —— 这块屏的装饰就被判定「还是这一版」，永远不重烘，节点停在
	 * 上一次烘出来的位置。表现：从 2x 屏往 1x 屏拖，窗口卡在 2x 屏边上
	 * 不动，超过一整个窗口宽才突然跳过去（那时窗口和这块屏不再相交，
	 * 走的是另一条分支）；反过来从 1x 拖到 2x 就正常。
	 *
	 * 记下来之后，位置一变就重烘 —— 拖拽才需要，静止时这个值也不动，
	 * 不会多烘。 */
	struct wlr_box dec_screen_box;
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
	/* 模糊 UV 的缩放补偿（见 fx.c 的 vidow_fx_frame）。默认 1 / 0，只有
	 * 开窗动画缩放时非零；别的 fx 目标（layer / popup / 抓帧）不用。 */
	float blur_scale_x, blur_scale_y;
	float blur_bias_x, blur_bias_y;
	/* animation pass, only used when a shader is configured */
	struct wlr_buffer *anim_buf[2];
	int anim_index;
	struct vidow_shader *shader;
	enum vidow_shader_phase phase;
	double progress;
	int64_t start_ns;

	bool active;  /* decoration or shader is in use */
	bool blur;    /* sample the blurred backdrop */

	/* 全屏直通：这个窗口现在整块交给场景直画（不建 / 不画装饰节点），wlroots
	 * 才有机会把客户端的缓冲直接扫上屏（判据见 fx.c 的
	 * vidow_view_scanout_ok）。true = 上一次是按直通处理的 —— view.c 的
	 * view_scanout_switch 用它判断状态翻转（翻转那一帧要把表面透明度接回去、
	 * 把装饰节点收起来）。 */
	bool scanout;

	/* 越界裁剪（见 view.c 的 vidow_view_apply_clip）：这块窗口只画自己那块屏
	 * 里的那部分。clip = 那块屏的框（布局逻辑坐标），clip_on 为 false 时整块
	 * 都画。裁剪只作用在**显示**上：几何、客户端、录屏抓帧都不动。 */
	bool clip_on;
	/* 这一份 fx 属于哪个窗口（小窗 / 图标 / MRU 卡片那些不是窗口，是 NULL）。
	 * 裁剪在 vidow_fx_frame() 里做，那个函数只拿得到 target、拿不到 view ——
	 * 而「overview 开着时裁到 overview 那块屏」这条判据需要 view。 */
	struct vidow_view *view;
	struct wlr_box clip;
	/* 没装饰（着色器也关着）时窗口是客户端表面自己在画，那一份裁的是表面树
	 * （wlr_scene_subsurface_tree_set_clip，见 view.c）。存的是**上一次真的
	 * 调进场景**的那个局部框：动画帧里窗口每帧都在动，只按 clip_on 判断会漏
	 * —— 裁出来的是上一帧位置的那一块。宽 0 = 没裁。 */
	struct wlr_box clip_local;
	/* 「客户端声明的有效源区域」（缓冲像素；wp_viewport 的 src 框，没设就是整块
	 * 缓冲）。**不能**直接读场景节点的 src_box：越界裁剪（wlroots 的
	 * wlr_scene_subsurface_tree_set_clip）就是改写 src_box 实现的 —— 裁完剩下的
	 * 那一小条会变成整个 src_box。装饰采源缓冲的哪一块照的就是这一份（view.c 的
	 * fx_geo_tex_map），读那个字段就会把「裁剩下的一小条」当成整块有效内容。
	 * 抄的是**没裁过**的那一版（见 view.c 的 view_snapshot_content_box）。
	 * 宽 0 = 还没抄到，那边按节点 src_box / 整块缓冲兜底。 */
	struct wlr_fbox content_box;

	/* Blur backdrop. Either the server's shared wallpaper pyramid (borrowed)
	 * or a private one captured with this window taken out (owned). The
	 * texture only changes when the backdrop really changed, so a still
	 * desktop costs no blur and no redraw at all. */
	struct wlr_texture *blur_tex;     /* may be NULL */
	bool blur_owned;                  /* we must destroy blur_tex */
	uint64_t blur_tex_id;             /* identity of blur_tex, 0 = none */
	uint64_t blur_ver;                /* bumped whenever blur_tex changes */
	uint64_t blur_drawn;              /* version baked into the current buffer */
	uint64_t bake_revision;           /* Content/placement/effect update request. */
	float mask_min_used;              /* 当前缓冲是按哪个剔除阈值画的 */
	struct wlr_texture *blur_own_tex; /* texture of the private pyramid */
	uint64_t blur_own_id;             /* identity of blur_own_tex */
	struct vidow_blur_set blur_set; /* private pyramid, allocated on demand */

	/* 这张模糊贴图盖住输出的哪一块（设备像素，相对输出原点）。窗口用的永远是整屏
	 * 那张（blur_tex_w == 0 就按整屏算）；会被窗口遮住的 layer surface 用的是
	 * **它自己那一小块区域**的抓取，采样坐标得减掉区域原点，否则整块错位。 */
	int blur_tex_x, blur_tex_y, blur_tex_w, blur_tex_h;

	/* 上面那张贴图代表的是**哪块屏**的视野：那块屏的设备像素尺寸 + 布局原点。
	 * 跟贴图一起在 vidow_blur_update() 里记下（只有那一趟知道归属）。
	 *
	 * 为什么不能现场读 server->output_w/x：那对全局量说的是「**正在渲染**的那块
	 * 屏」（output.c 的 vidow_output_render 开头才写）。而装饰重烘那趟（output.c
	 * 帧循环里 blur_drawn != blur_ver 的那个循环）**不看归属**，会给别的屏上的
	 * 窗口也重烘一次 —— 那一刻按 server->output_w 算采样 UV，就是拿整屏底衬只取
	 * 中间一块铺满整窗：中心放大 + 变糊（用户报的「静止时底衬啪一下换挡」，见
	 * README 253）。采样只认跟着贴图记的这一组。 */
	int blur_out_w, blur_out_h, blur_out_x, blur_out_y;

	/* **按屏各存一份底衬**（见上面 struct vidow_blur_slot 的说明）。
	 *
	 * vidow_blur_update 每趟只处理**一块屏**（它本来就是按屏调的），窗口和
	 * 这块屏有交集就往这一格里写；装饰采样时按「这次烘是给哪块屏的」取
	 * （fx.c 的 blur_slot_for）。窗口完全进到某块屏之后，另一格由那一趟自己
	 * 收掉（它不再和那块屏相交）。 */
	struct vidow_blur_slot *blur_slot;
	/* 这一帧正在用的是哪一格（ensure_targets 按 bake_output 选好；NULL = 老的单值）。 */
	struct vidow_blur_slot *cur_slot;
	/* 这一次重烘是给哪块屏的（decor_draw / decor_pass 采样时按它选格子）。 */
	struct wlr_output *bake_output;

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
	/* mask 贴图在元素局部坐标里的采样映射（归一化）。默认 0 / 1；xdg popup
	 * 只糊窗口体（去掉 CSD 阴影）时，mask 要采表面缓冲里 geometry 那一块。 */
	float mask_off_x, mask_off_y, mask_scale_x, mask_scale_y;
	/* mask 的透明度阈值；0 = 用全局 blur_alpha_min。弹出物会设成 1/255，
	 * 只剔掉 alpha 恰好为 0 的透明留白（见 fx.c 的 popup_blur_frame）。 */
	float mask_min;

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

/* overview 里的一行 = 这块屏上的一块工作区。行表按需分配、复用（见 rows）。
 *
 * 每行自己的几个数：
 *   band_w    —— 这块工作区的条带（工作区局部像素）有多宽（只喂日志）；
 *   rect_w /
 *   rect_x    —— 这一行的**矩形**（缩小版的屏：高 = row_h，宽 = 高 × 那块屏的长宽比，
 *                横向居中）在屏幕上的位置和宽度；
 *   y         —— 这一行的矩形在屏幕上的顶边；
 *   content_x —— 条带原点画在屏幕上的 x（已经含**行内横向滚动**：跟着这块工作区
 *                自己的视口走，见 overview.c）。
 * plate 是矩形的底板、bg 是矩形里的**背景图**（那块工作区所在屏的壁纸底衬）—— 两个
 * 都跟遮罩同一层、垫在窗口下面。 */
struct vidow_ov_row {
	struct vidow_workspace *ws;
	struct wlr_scene_rect *plate;
	struct wlr_scene_buffer *bg;
	int band_w;
	int rect_w, rect_x;
	int y;
	double content_x;
};

/* niri 风格的 overview：把这块屏上的**所有工作区**上下排成一列看全景（每块工作区
 * 缩成一条「这块屏」那么大的区域），点谁就去谁。
 *
 * 场景是全局的（一个窗口的位置就是「世界坐标」），所以 overview 不动布局、不改
 * 客户端尺寸，只给每个窗口算一个「工作区局部 -> 屏幕」的变换：
 *
 *     screen = 那一行的 (content_x, y) + (局部坐标 + 动画偏移) * scale
 *
 * t = 0（正常）必须是**恒等变换**（scale = 1、位置就是它自己摆在哪），进出的动画
 * 都在 t 上插值，两端都不能突变；t = 1（展开）就是上面的式子。每帧算一次，所有
 * 窗口共用（见 view.c 的 vidow_view_apply_anim）。
 *
 * 三件事各归各的输入：
 *   - 列表纵向滚动（list_scroll）：滚轮，一次一格 —— 列表比工作区高时才有得滚；
 *   - 行内横向滚动：跟着**那块工作区自己的视口**（焦点在工作区里换窗口时它自己
 *     会动，见 view.c 的 output_arrange Pass 2）—— 所以「移动窗口焦点的键」动的是
 *     行内预览，不是整条相机乱跑；
 *   - 每行的区域 / 背景板：浓度跟着遮罩的 alpha 走（不另加配置项，alpha = 0
 *     「不压暗」时它跟着一起消失，壁纸原样）。
 *
 * 定义放在 vidow_fx_target 之后：里面内嵌了一个 fx 目标（糊掉的那张壁纸）。 */
/* 每块屏各自的那一份 overview 状态（见 struct vidow_overview 的说明）。
 *
 * 为什么拆出来：以前 overview 是**单实例** —— 进来时把 server->focused_output
 * 记进 ov->output，之后所有行、遮罩、底衬都只服务那一块屏。别的屏上一片空白
 * （用户：「overview 触发的时候应该所有显示器都进 overview，现在虽然都进了但
 * 非激活显示器不显示内容」）。
 *
 * 现在每块屏各自摆自己的行、自己一块遮罩、自己一份底衬，互不影响。共用的是
 * 「开关状态和进度」（ov->on / ov->t / ov->scale），因为它们本来就该同步 ——
 * 所有屏一起进、一起出，不然画面会散架。 */
struct vidow_ov_screen {
	bool used;               /* 这块屏这次进来有没有摆过行（没摆就不画） */
	double scale;            /* 这块屏的缩略比例（由 t 算出 —— 各屏行高不同） */
	struct vidow_ov_row *rows;      /* 行表（每行 = 这块屏上的一块工作区） */
	int n_rows, cap_rows;
	int n_skipped;           /* 这块屏上没摆行的空工作区 */
	double list_scroll;      /* 列表纵向滚动（屏幕像素） */
	double row_h;            /* 一行的屏幕高（= 工作区高 × scale） */
	int out_x, out_y, out_w, out_h; /* 这块屏的工作区（摆行 / 遮罩都用它） */
	struct wlr_scene_rect *dim;     /* 这块屏的压暗遮罩 */
	/* 这块屏糊掉的那张壁纸（overview_blur）：摊满整块屏、垫在遮罩下面。 */
	struct vidow_fx_target fx;
	bool fx_on;                     /* 这一帧真的画上了 */
	struct vidow_anim *scroll_anim; /* 这块屏列表滚动的缓动 */
	double scroll_from;
	double scroll_to;
};

/* overview 的**全局**部分：开关状态和进度。
 *
 * 这些是所有屏共用的 —— 所有显示器一起进、一起出，进度同步（不然画面会散架）。
 * 每块屏自己的那份（行表 / 遮罩 / 底衬 / 滚动）在 struct vidow_ov_screen 里，
 * 挂在 struct vidow_output 上（见那边的说明）。 */
struct vidow_overview {
	bool on;                 /* 目标状态（动画朝它走） */
	double t;                /* 0..1，当前进度 */
	double anim_from;        /* 这次动画的起点 t（中途反向就从这儿接着走） */
	bool swallow_release;    /* 点选退出后，把它对应的那次松手也吃掉 */
	struct wlr_output *output;      /* 主屏：进来时聚焦的那块（IPC / 果冻判定用） */
	struct vidow_anim *anim;
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
	struct vidow_ease easing;
	int x, y, w, h;
	struct vidow_fx_geo geo; /* 快照里哪一块是「可见窗口」（见上） */
	double scale;
	/* 关的时候长什么样（窗口规则的装饰参数）—— 快照要按同一份参数画，
	 * 不然关窗动画一出来装饰就变回全局配置的样子了。 */
	struct vidow_rule_eff rule_eff;
	/* 关的时候是不是全屏：全屏窗口不画服务端装饰（见 rule.c 的 vidow_view_look），
	 * 快照也得按同一份参数画。 */
	bool fullscreen;
};

/* ------------------------------------------------------------------ output */

struct vidow_output {
	struct wl_list link; /* vidow_server.outputs */
	struct vidow_server *server;
	struct wlr_output *wlr_output;
	/* 这块屏自己的 overview 状态（行表 / 遮罩 / 底衬 / 滚动）。
	 *
	 * 以前这些全是单实例、只服务「进来时聚焦的那块屏」，别的屏一片空白。
	 * 现在每块屏各有一份 —— 所有屏一起进 overview，各自摆自己屏上的工作区
	 * （用户 2026-10-25：「overview 触发的时候应该所有显示器都进 overview，
	 * 现在虽然都进了但非激活显示器不显示内容」）。 */
	struct vidow_ov_screen ov;
	struct wlr_scene_output *scene_output;
	struct wlr_output_layout_output *layout_output;
	/* 这块屏的抓帧器（capture.c 的 vidow_capture）：独立的假输出 + 自己的
	 * scene_output，抓帧时自己渲染一帧。懒建。 */
	struct vidow_capture *capture;
	/* 这块屏上有光标抓帧（ext-image-copy-capture 的 pointer cursor session）在等
	 * 一帧：光标不在画面里 / 在硬件光标层时这块屏永远不会提交，「光标抓帧卡住」
	 * 只能靠抓帧看门狗推这块屏一下（见 output.c 的 capture_watchdog 和
	 * vidow_output_capture_kick_output）。一次性标记：看门狗用掉就清。 */
	bool capture_pending;

	struct wlr_box usable_area; /* layout coords, minus layer exclusive zones */
	/* 上次已知的布局位置（逻辑坐标）。拔线时 wlr_output_layout 可能已经先把这块
	 * 输出忘掉了，热插拔要靠这份存着的位置算「挪到别的屏」的平移量。 */
	struct wlr_box box;
	/* 这块屏的热插拔记忆（按输出名认）。在 new_output 里就取好存下：
	 * 拔线时 wlroots 已经把 wlr_output->name 收走了，那时候再按名字找就晚了。 */
	struct vidow_output_memory *memory;
	/* 会话锁（见 sessionlock.c）：这块屏的黑幕 rect（锁定期间不透明黑，垫在
	 * lock surface 底下；节点常开，由 layer_lock_blank 那棵树统一开关），以及
	 * 「锁上之后这块屏还没呈现过一帧」的记账（锁定握手靠它数）。 */
	struct wlr_scene_rect *lock_blank;
	bool lock_frame_pending;
	double viewport_x;          /* horizontal scroll offset of the column strip */
	/* 条带横向滚动的过渡（换焦点把焦点列滚回可见区那一下）。
	 *
	 * 视口本来是**瞬跳**的：每个窗口再各自补一条 window_move_ms 的位移，
	 * 于是观感是「几个窗口各走各的」—— 窗口 A 走了 200px、B 走了 380px，
	 * 各自从自己的旧位置飘到新位置，看着就是它们在互相追。
	 *
	 * 真正该动的东西只有一件：**条带本身**。让视口按曲线走，窗口每帧按「当前
	 * 视口」摆位（不跑位移补间），整条带子就是刚性的 —— 窗口相对位置一点不变。
	 * 这就是 niri 的做法。
	 *
	 * viewport_anim_from / to：这条过渡的起终点（逻辑像素）。 */
	struct vidow_anim *viewport_anim;
	double viewport_anim_from;
	double viewport_anim_to;
	/* 下一帧允许视口走过渡（只有「换焦点列」才置真，见 output_arrange
	 * Pass 2）。关窗 / 列宽变化 / 拖拽落位不置它 —— 那些情况该由每个窗口
	 * 自己的位移补间表现补位，视口一过渡就会把所有窗口的补间掐掉。 */
	bool viewport_want_slide;
	/* 切工作区的过渡：正在被推开的（旧的）那块工作区，和驱动它的动画。
	 * 过渡期间 arrange 会把 slide_from 的列也摆一遍（按它自己的 slide_y），
	 * 见 view.c 的 output_arrange 和 workspace.c 的 slide_start。 */
	struct vidow_workspace *slide_from;
	struct vidow_anim *slide_anim;
	/* 手指正拖着条带（三指横扫那种连续手势）：这期间视口由 gesture_viewport
	 * 说了算，arrange 不做「把焦点列滚回可见区」那套（见 view.c 的 Pass 2）。 */
	bool gesture_drag;
	double gesture_viewport;
	/* 平铺窗口拖拽时的**贴边自动滚条带**（niri 的 Dnd 手感）：光标压进工作区
	 * 左右边缘那条带里，条带就按秒速往那边滚 —— 停着不动也继续滚，所以推进放
	 * 在每帧（output.c 的 vidow_output_render 调 vidow_tile_drag_scroll_step），
	 * 这里只记「视口归拖拽说了算」和「往哪边、多快」。
	 *
	 * 为什么必须有：条带比屏宽时，屏幕外那些列（和它们之间的落点）光靠推光标
	 * 永远够不到 —— 用户报的「整个显示器左右有一部分死区 / 窗口拖过去被弹回」
	 * 就是这一条。 */
	bool drag_scroll;
	double drag_viewport;
	double drag_scroll_speed;   /* 像素/秒，正 = 往右滚，0 = 不在边缘带里 */
	int64_t drag_scroll_ns;     /* 上一次推进的时刻（算 dt） */
	/* 按需 VRR（niri 的 variable-refresh-rate on-demand）：这块屏是不是按需模式
	 * （配置说了算，每次 arrange 对一遍账），以及上一次按需要到的状态（省掉
	 * 每帧一次 commit —— 见 output.c 的 vidow_output_vrr_on_demand_update）。 */
	bool vrr_on_demand_mode;
	bool vrr_on_demand_on;
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
	uint64_t blur_gen;  /* Per-output backdrop revision captured by the pyramid. */
	uint64_t backdrop_revision;
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
	/* 输出提交了新状态（换模式 / 改缩放 / 热插拔重排）。锁定期间靠它立刻把黑 rect
	 * 和 lock surface 对齐到新的逻辑尺寸 —— 不能等下一帧（见 sessionlock.c）。 */
	struct wl_listener commit;
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

/* ------------------------------------------------- 小窗（mini window，S1）
 *
 * 小窗**不是**新的浮动模式：`float_mode` 仍是 VIDOW_FLOAT_FIXED（复用
 * float_geom / home_dx,home_dy / float_place_recursive() / vidow_float_set_size()
 * 那一整套），它只是浮动窗口上的一个「形态」字段。三态 + NONE：
 *
 *   NONE  不在小窗态（可能平铺、也可能普通浮动窗口）
 *   FREE  自由小窗：位置与尺寸都记比例锚点，客户端可以改尺寸
 *   MINI  迷你态：尺寸跟配置钉死，只记位置锚点
 *   ICON  图标态：正方形（配置的边长），贴边、按比例超出屏幕一点
 *
 * 位置**不记绝对坐标**，记的是「相对自己那块输出工作区的比例锚点」
 * （mini_ax/mini_ay ∈ 0..1，图标允许 -0.5..1.5 好让它出屏）。理由和
 * home_dx/home_dy 一样但更强：改工作区尺寸 / 改缩放 / 拔插屏之后比例锚点
 * 天然跟着走，不需要在每个热插拔路径里补换算。
 *
 * 尺寸：FREE 记比例（mini_wf/mini_hf，mini_size_known = 记过没有）；
 * MINI / ICON 不记 —— 永远跟配置，改配置当场生效。 */
enum vidow_mini_state {
	VIDOW_MINI_NONE = 0,
	VIDOW_MINI_FREE,
	VIDOW_MINI_MINI,
	VIDOW_MINI_ICON,
};

/* 图标态贴着哪条边（沿这条边按比例超出屏幕一点，另一轴按锚点）。 */
enum vidow_mini_edge {
	VIDOW_MINI_EDGE_NONE = 0,
	VIDOW_MINI_EDGE_LEFT,
	VIDOW_MINI_EDGE_RIGHT,
	VIDOW_MINI_EDGE_TOP,
	VIDOW_MINI_EDGE_BOTTOM,
};

/* 自由小窗的宽高比策略。三个取值都是「强制」语义，优先级：
 * 键位（运行时）> 窗口规则 > 全局键（config.c 的 mini_aspect）。
 *   auto  让自动信号决定：X11 有 PAspect / min==max 提示就锁，否则 free
 *   lock  显式锁内容比例（压过自动信号）
 *   free  显式自由拖（压过自动信号）
 * 迷你 / 图标态永远按内容比例显示，不受它影响。 */
enum vidow_mini_aspect {
	VIDOW_MINI_ASPECT_UNSET = 0, /* 规则没写（不是 "auto" —— 那是一个值） */
	VIDOW_MINI_ASPECT_AUTO,
	VIDOW_MINI_ASPECT_LOCK,
	VIDOW_MINI_ASPECT_FREE,
};

enum vidow_grab_mode {
	VIDOW_GRAB_NONE = 0,
	VIDOW_GRAB_MOVE,
	VIDOW_GRAB_RESIZE,
	/* 平铺窗口的 Mod+左键拖拽（src/drag.c）：位置只动场景节点，松手才决定归哪 */
	VIDOW_GRAB_TILE,
	/* 平铺窗口的 Mod+右键拖拽（src/drag.c）：改这一列的宽度（列宽的右边缘跟着
	 * 光标走）。vidow 的平铺尺寸模型里没有「单个窗口的高度」，所以只横向。 */
	VIDOW_GRAB_RESIZE_TILE,
	/* 拖 CSD 标题栏 = 平移视图（窗口不动），见 drag.c 的 vidow_pan_* */
	VIDOW_GRAB_PAN,
};

/* niri-style column: a vertical stack of windows, columns are laid out left
 * to right and the whole strip scrolls horizontally. */
struct vidow_column {
	struct wl_list link;    /* vidow_workspace.columns, left to right */
	uint64_t id;            /* IPC 用的稳定编号（见 vidow_view.id） */
	struct wl_list views;   /* vidow_view.column_link, top to bottom */
	struct vidow_server *server;
	struct vidow_workspace *ws; /* 属于哪块工作区（唯一归属） */
	struct wlr_output *output; /* 现在摆在哪块屏上（由 ws->output 同步） */

	double width_fraction; /* of the usable width */
	/* 列宽过渡（见 view.c 的 vidow_column_set_width）：补间在跑的时候，**这一帧
	 * 拿来算像素**的是 width_shown，width_fraction 一直是逻辑目标 —— 连着按
	 * cycle-column-width 时下一档要从目标往上数，不能从画到一半的地方数。不在
	 * 补间时两者相等。 */
	struct vidow_anim *anim_width;
	double width_shown;
	/* 补间的两端（比例）：anim_from 是起手那一刻画在屏上的值，anim_to 是新目标。 */
	double anim_from;
	double anim_to;
	/* niri 式「列最大化」：整列撑满工作区宽度（width_fraction 顶到 1.0），再按一次
	 * 还原成原来那个比例（记在 maximized_saved 里）。手动改宽度（cycle-column-width
	 * / set-column-width / expand-column-to-available-width）就是退出这个状态 ——
	 * 状态的唯一入口是 vidow_column_set_maximized()（见 view.c）。 */
	bool maximized;
	double maximized_saved;
	int offset;            /* from the usable area's left edge */
	int width;             /* computed each arrange */
	/* 上一次 arrange 把它**画在屏上**的那一块：左边（列带滚过之后的绝对坐标，
	 * 已经减掉视口偏移）和它在那条带上占的宽度。拖动找落点（drag.c）看的就是
	 * 「屏幕上看得见的那个位置」—— 全屏那列占一整屏、整列最小化的宽度是 0 这些
	 * 规矩只有 arrange 知道，外面别自己再算一遍条带数学。 */
	int screen_x, slot_width;
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
	/* 被并进**别的**列之前，它自己那条列的宽度。拖出来 / expel 的时候用它
	 * 恢复原宽 —— 并进一条宽 1.0 的列再拖出来，不该把 1.0 带走。
	 * set = false 时没有可恢复的值，按当前列宽走。 */
	double column_width_home;
	bool column_width_home_set;
	/* 属于哪块工作区（浮窗 map 时定下，列的窗口跟着列走）。工作区藏起来的时候
	 * 窗口还活着，只是场景节点被关掉。 */
	struct vidow_workspace *ws;
	struct vidow_server *server;
	enum vidow_view_kind kind;
	struct wlr_xdg_toplevel *xdg_toplevel;   /* kind == XDG 时有效 */
	struct vidow_x11_surface *x11;           /* kind == X11 时有效（见 x11surface.h） */
	struct wlr_scene_tree *scene_tree;
	struct wlr_output *output; /* 现在摆在哪块屏上（由 ws->output 同步） */
	/* 表面当前进的是哪块输出。enter/leave 自己管（见 view_sync_surface_outputs），
	 * 变了才重发。 */
	struct wlr_output *surface_output;
	/* 浮动窗口「相对自己那块屏原点」的偏移。布局会随着拔插重新摆（拔掉左边那块，
	 * 右边那块可能从 x=1280 挪到 x=0），所以只记绝对坐标会被搬错一次屏。 */
	int home_dx, home_dy;

	struct wlr_box geom; /* last arranged geometry, layout coords */
	/* 最后已知的标题 / app_id（IPC 的 window-closed 事件要带它们，而客户端的
	 * 字符串在销毁信号之后就没了）。map / 标题变化时刷新。 */
	char last_title[128];
	char last_app_id[128];
	/* 窗口规则的 initial_* 字段：上屏那一刻的标题 / app_id / class / instance。
	 * 客户端之后改标题不影响这里（浏览器、终端都会改）。X11 的 WM_CLASS 有时比
	 * map 还晚到，所以空着的时候还要补（见 view.c 的 view_note_initial_names）。 */
	char init_title[128];
	char init_app_id[128];
	char init_class[128];
	char init_instance[128];
	/* 窗口规则的 exe / cmdline：按 pid 去 /proc 抓一次就缓存（probe_pid 变了才
	 * 重抓 —— 一个窗口的客户端进程不会换）。 */
	int probe_pid;
	char probe_exe[128];
	char probe_cmdline[256];
	/* 窗口规则算出来的作用（rule.c 填；开窗时兑现的那些见 vidow_rule_placement，
	 * 渲染 / 布局那边的「这个窗口现在该什么样」也看它）。 */
	struct vidow_rule_eff rule_eff;
	struct vidow_view_tag tag;
	bool mapped;
	/* Xwayland may re-map an already-associated surface without another
	 * wl_surface commit; use the X map-request edge to replay view_map once. */
	bool x11_map_replayed;
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
	 * 窗口。
	 *
	 * 停到停车位上还有一个连带后果：这块窗口就不再「在」任何输出上了，客户端按
	 * 「进了哪些输出」算倍率的（kitty 那类）会掉回 1 倍重画 —— 所以倍率得靠
	 * preferred_buffer_scale 钉住，见 view.c 的 view_pin_scale。 */
	bool scene_parked;
	/* 上一次钉给客户端的 preferred_buffer_scale（0 = 还没钉过）。只在变了的时候
	 * 发协议事件，见 view.c 的 view_pin_scale。 */
	int32_t pref_scale;
	/* 帧追踪用：上一次记 commit 的时间（VIDOW_TRACE_FRAMES=1 才写） */
	int64_t trace_commit_ns;

	/* X11（XWayland）专用：popup = 菜单 / 提示 / 下拉框这类「弹出物」。对应
	 * Wayland 的 xdg_popup：不装饰、不跑 shader、不做开关动画、不参与布局
	 * （位置尺寸由客户端给死），也不改「谁是焦点窗口」。 */
	bool popup;
	/* 1x1 辅助 toplevel（剪贴板 helper）：需要短暂拿键盘才能 set_selection，
	 * 但关闭后焦点要还给之前那个窗口。 */
	bool focus_helper;
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
	/* 窗口规则 fake_focus：焦点移走时也不给客户端发 deactivated，让它一直以为
	 * 自己还激活（给一失焦就暂停 / 收 UI 的程序用）。只伪装客户端看到的
	 * activated 状态；键盘焦点、合成器焦点、任务栏都还是真的。 */
	bool fake_focus;
	/* 这个窗口要 VRR（窗口规则 vrr = true，niri 的 variable-refresh-rate）：它
	 * 显示在「按需 VRR」的屏上时把那块屏的刷新率提上去（见 output.c 的
	 * vidow_output_vrr_on_demand_update）。默认 false = 不提，跟以前一样。 */
	bool vrr;
	/* 这个窗口的激活请求即使 serial 校验不过也照样理（窗口规则 honor_invalid_serial，
	 * 见 activation.c）。默认 false = 不理。 */
	bool honor_invalid_serial;
	/* 有人请求激活它、但它还没上屏：记下来，等 map 的时候兑现（点通知常常比
	 * 窗口第一次 commit 还早一点）。 */
	bool pending_activate;
	struct wl_list capture_sources; /* vidow_capture_hold.link */
	bool render_pumping;
	/* 补帧判定的迟滞（见 view.c 的 vidow_view_render_pump）：连续多少帧
	 * 「看不上屏」才真的开始补。view_on_screen() 依赖 view->geom / 工作区，
	 * arrange 期间这些值会逐帧变化，把它当瞬时真值会让 render_pumping 每帧
	 * 来回翻（实测 85 次/秒），每次翻转都喂客户端一帧 frame_done —— 全屏游戏
	 * 因此被反复唤醒重画，GPU 功耗异常。0 = 还没确认过。
	 * pump_hidden_frames 是「连续看不上屏」的帧数，pump_shown_frames 是反过来的。 */
	int pump_hidden_frames;
	int pump_shown_frames;
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
	/* 小窗（S3）：场景节点现在被缩过（保留原生分辨率那条路，见 view.c 的
	 * view_mini_scale / view_anim_place）。退出小窗 / 关掉保留之后要还原成原生
	 * 尺寸，这个标志就是「该不该还原」。 */
	bool mini_scaled;
	/* 小窗（S3b）：内容居中用的原始偏移 —— 进缩放态那一刻记下内容节点在它父节点
	 * 里的位置（xdg 的窗口几何留白有可能是 wlroots 设在这一层的），居中偏移加在
	 * 它上面、退出缩放态时还原成它。见 view.c 的 view_anim_place。 */
	int mini_base_dx, mini_base_dy;
	/* 小窗（S3b）：运行时键位覆盖（toggle-mini-aspect）。UNSET = 没按过。
	 * 有效值 = 键位 > 窗口规则 > 全局键，见 vidow_mini_aspect_effective()。 */
	enum vidow_mini_aspect mini_aspect_runtime;
	/* 上次记「缓冲区多大 / 几倍」日志时的值：每个 commit 都刷一行没意义。
	 * 两处日志各用一个变量：x11 那条（view.c 的 vidow_view_x11_scale）用的是
	 * 「按 N 倍声明」的放大倍率，xdg 那条（客户端缓冲区 / 逻辑尺寸反推）用的是
	 * 客户端实际画的倍率。以前共用一个 logged_scale，两处互相覆盖，同一个值
	 * 永远对不上，于是每条 commit 都打一行 —— Steam 那次一秒刷了几千行。 */
	int logged_scale;    /* x11 缩放桥：上次「按 N 倍声明」的倍率 */
	int logged_buf_w;    /* x11 缩放桥：上次的缓冲宽度 */
	int logged_client_scale; /* xdg：上次客户端实际画的倍率 */
	int logged_client_buf_w; /* xdg：上次的缓冲宽度 */
	/* 上次客户端交帧的单调时间（ns）。用来量「客户端多久交一帧」—— 卡顿如果
	 * 出在客户端侧（vidow 这边 128fps 也不慢），这个数会明显大于帧周期。 */
	int64_t last_commit_ns;
	int64_t commit_n;        /* 本窗口交帧次数 */
	int64_t commit_ns_sum;   /* 本窗口交帧间隔累计（ns） */
	int64_t commit_ns_max;   /* 本窗口最坏间隔（ns） */
	int64_t commit_report_ns; /* 上次打统计的时刻（0 = 还没开始） */
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
	struct wl_listener x11_map_request;
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
	/* niri 的 interactive move：一按下去就把窗口从布局里摘走（后面的列
	 * 当场并拢），松手才插回去。这里不物理摘链表，只让布局/焦点判据跳过
	 * 它（见 view.c 的 view_shows），拖动结束再清掉。 */
	bool drag_detached;
	/* niri 的起拖阈值：光标从按下点走够 16 逻辑像素（256 px²）才真的
	 * 把窗口摘出布局 / 降透明度。没走够就松手 = 一次点击，布局不该闪。 */
	double drag_start_cx, drag_start_cy;
	int drag_dx, drag_dy;           /* 抓点在窗口里的位置 */
	double anim_from_x, anim_from_y; /* move tween start offsets */
	bool placed;                     /* laid out at least once */

	/* custom shader output */
	struct wlr_scene_surface *scene_surface;
	struct vidow_fx_target fx;
	int fx_dx, fx_dy; /* surface offset inside the view tree */
	struct wlr_buffer *last_buffer; /* locked, feeds the close animation */
	/* X11 Depth 32 窗口的 alpha 判定结果（见 vidow_view_x11_scale）。
	 * x11_alpha_known = 还没判过；x11_alpha_on = 这个窗口用 alpha 通道
	 * （而不是整块当不透明）；x11_opacity = 记到节点上的整体不透明度。
	 * 三个一起比，判定没变就不动场景节点 —— 这个函数每帧 commit 都会走。 */
	bool x11_alpha_known;
	bool x11_alpha_on;
	float x11_opacity;
	/* 重映射后强制重建 XWayland 导出的 dmabuf。
	 *
	 * 现象：Steam 关掉主窗口再打开，X 侧 pixmap 有内容（import 抓到 99%
	 * 非黑），我们拿到的纹理也是 3808x2050，但里面全零 —— XWayland 导出
	 * 的那份 dmabuf 没被重新填过。用户实测**拖一下 / 缩放一下 / 切一次
	 * 工作区**画面就出来了，三种操作的共同点是逼 XWayland 重建 pixmap。
	 *
	 * 所以这里在「重新上屏」的那一刻主动抖一下尺寸：先 +1 像素、下一轮
	 * 再还原。X 服务器只在尺寸真的变了才重新分配 pixmap，同尺寸的
	 * configure 它不理（ICCCM 连 ConfigureNotify 都不发，view.c 里
	 * 504 行那个分支就是给这种情况补事件的），所以必须真的改一下。 */
	/* X11 表面的「欠帧」计数，见 view.c 里 xwm 交帧那段说明。
	 *
	 * XWayland 的渲染由 wl_surface 的 frame callback 驱动，而回调只在
	 * Vidow 真画完一帧时才发；Vidow 又是按需渲染。两边互等，画面就冻在
	 * 重映射后的那张空白帧上（实测 Steam 第二次开主窗口：重开后只有
	 * 1 帧，首开 2285 帧；手动切一次工作区强制整屏重绘，立刻涨到
	 * 1008 帧、画面正常）。这里记「还欠它几帧」，由补帧闹钟推着走完。 */
	int x11_want_frames;
	/* 上一次为「续帧」记账的时刻（毫秒）。见 view.c 里那段说明：没有这个
	 * 冷却的话，每次 commit 都会重新记一笔 30 帧，而催帧又促使 Xwayland
	 * 再交一帧 —— 正反馈，实测 160 次/秒的空转。 */
	uint32_t x11_want_frames_at;
	/* 拖动时的果冻形变（见 jelly.c）：非空就是「现在正弯着」——窗口本来的
	 * 装饰节点让位，由它显示。松手后它自己晃回去再消失。 */
	struct vidow_jelly *jelly;

	/* floating */
	enum vidow_float_mode float_mode;
	/* 浮窗记账挂在哪块工作区（NULL = 还没记）：ws->floats 的 +1/-1 只在
	 * vidow_workspace_track() 里动，靠这个指针保证同一块工作区只记一次。 */
	struct vidow_workspace *floats_ws;
	/* IPC 的 set-window-size 要的尺寸（0 = 没要过）。客户端下一次提交要是还带
	 * 旧尺寸，不能把「我让它多大」改回去 —— 一个迟到的提交会把这条命令吃掉
	 * （踩过：改完尺寸，客户端先为别的原因提交一次旧尺寸，float_geom 就被它
	 * 覆盖回去，随后 arrange 又按旧尺寸 configure，等于没改）。 */
	int float_want_w, float_want_h;
	struct wlr_box float_geom; /* FIXED: absolute box
	                              FOLLOW: x/y = offset from the parent tile */
	struct vidow_view *parent; /* FOLLOW: the view we track */

	/* ---- 小窗（mini window，S1；见 enum vidow_mini_state 顶上那段）----
	 * 只有 float_mode == VIDOW_FLOAT_FIXED 时才有意义：进小窗态时
	 * float_mode 会被设成 FIXED，位置尺寸由下面的锚点算出来。 */
	enum vidow_mini_state mini;
	/* 位置锚点：相对**自己那块输出的工作区**的比例。算位置时：
	 *   x = work.x + round(mini_ax × (work.w − 窗口宽))
	 * 所以 0 = 贴工作区左 / 上边，1 = 贴右 / 下边。图标态允许超出 0..1
	 * （夹在 -0.5..1.5），好让它按 mini_icon_overflow 出屏一点。 */
	double mini_ax, mini_ay;
	/* FREE 的尺寸比例（相对工作区）；mini_size_known = 记过没有。
	 * MINI / ICON 不看这两个 —— 它们的尺寸永远跟配置。 */
	double mini_wf, mini_hf;
	bool mini_size_known;
	/* ICON 贴哪条边（沿这条边按比例出屏）。NONE = 不贴边（居中）。 */
	enum vidow_mini_edge mini_edge;
	/* 小窗是**屏级对象**（像安卓的画中画），不属于任何工作区：进小窗态时
	 * view->ws 会被清成 NULL 并把这笔账从那块工作区的 floats 里减掉（不然
	 * 一块只剩小窗的工作区永远收不掉）。mini_home 按输出名记着它属于哪块屏，
	 * 拔屏再插回来时按它认领回去。 */
	struct vidow_output_memory *mini_home;
	/* 窗口规则写的宽高比策略（VRA_MINI_ASPECT）。UNSET = 规则没写。 */
	enum vidow_mini_aspect mini_aspect_rule;
	/* 窗口规则写的「保留客户端原生分辨率」（VRA_MINI_PRESERVE_BUFFER）。 */
	bool mini_preserve_buffer_set;
	bool mini_preserve_buffer;

	/* ---- 小窗（mini window，S5）：形态转换的过渡动画 ----
	 *
	 * 起点**严格取「上一帧画在哪」**（geom + anim_off，和 view.c 的
	 * drawn_x/drawn_y 同源），在改几何之前取一次、存成下面那两个常量差值；
	 * update() 里绝不重读 view->geom（`daeba31` 的教训：推断起点会让滚动 /
	 * 转换时窗口从屏幕外飞入）。位置曲线一律单调，只有缩放用 out_back。 */
	struct vidow_anim *mini_anim;
	double mini_from_dx, mini_from_dy; /* 起点 − 目标（转换期间是常量） */
	double mini_scale_from;            /* 缩放补间起点（相对目标盒） */
	struct vidow_ease mini_scale_ease; /* 只用在缩放上（可过冲） */

	/* ---- 小窗（mini window，S5）：图标态的「快照」节点 ----
	 *
	 * 按该窗口**有效**的 force_render 分叉：开 → 图标就是窗口自己那块活节点
	 * （render_pump 保活，图标和预览都是动的）；关 → 真实节点停到停车位（客户端
	 * 停帧）、屏上由这个快照节点画 `view->last_buffer` 的最后一帧。见 view.c 的
	 * vidow_mini_uses_snapshot() / mini_snapshot_place()。 */
	struct wlr_scene_buffer *mini_snapshot;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener set_title;
	struct wl_listener set_parent;
	struct wl_listener request_fullscreen;
	struct wl_listener request_maximize;
	struct wl_listener request_move;
	/* 客户端交帧 → 排一帧（见 view.c 的 view_surface_commit） */
	struct wl_listener surface_commit;
	struct wl_listener request_resize;
	struct wl_listener request_minimize;

	/* 窗口列表（waybar / xdg-desktop-portal 的 ScreenCast 窗口选择器） */
	struct wlr_foreign_toplevel_handle_v1 *ftm_handle;
	struct wlr_ext_foreign_toplevel_handle_v1 *ftl_handle;
	/* wlroots 不判断状态是否真的变了，重复 set_activated 会白刷一遍客户端 */
	bool portal_activated;
	bool portal_entered;
	/* foreign-toplevel 的 output_enter/leave 跟着窗口真正落的那块屏走：
	 * 只记 bool 的话窗口从 DP-2 拖到 DP-3 之后，Noctalia 的 activeToplevelOutput()
	 * 还指着旧屏 —— 工作区上的活动应用图标就挂错屏 / 挂错工作区。 */
	struct wlr_output *portal_output;
	struct wl_listener ftm_request_activate;
	struct wl_listener ftm_request_close;
	struct wl_listener ftm_request_fullscreen;
	struct wl_listener ftm_request_minimize;

	/* X11 弹出物背后的模糊（见 struct vidow_popup_blur）；普通窗口是 NULL。 */
	struct vidow_popup_blur *popup_blur;
};

/* --------------------------------------------------------- 弹出物的模糊 */

/* 弹出物（窗口的 xdg_popup / X11 菜单提示 / 输入法候选框）背后的模糊。
 *
 * 和 layer surface 那条路是同一套（见 fx.c 的 layer_blur_frame）：在弹出物
 * 自己的场景树里垫一个只画模糊的节点（blur_only），底衬按它那一小块区域抓
 * —— 基座从共享底衬上 1:1 拷一块，再把窗口 / 别的弹出物画上去；抓的时候
 * 弹出物自己藏着（不然模糊里会浮出它自己的影子）。节点是它的孩子，弹出物
 * 移动 / 改尺寸时自动跟着走（输入法候选框那条路节点挂在 layer_im 上，位置
 * 每帧按绝对坐标摆）。
 *
 * 谁建：view.c 的 vidow_popup_scene_create（Wayland xdg_popup）、view.c 的
 * x11_attach_surface / x11_refresh_popup（X11 菜单）、im.c 的 im_new_popup
 * （输入法候选框）。生命周期各自管：xdg 那条挂在 surface 的 destroy 上，
 * X11 / 候选框在拆场景节点 / 表面时显式收。 */
struct vidow_popup_blur {
	struct wl_list link; /* vidow_server.popup_blurs */
	struct vidow_server *server;
	/* 模糊节点挂在哪棵树上，坐标也从这棵树取（xdg / X11 是弹出物自己的树，
	 * 候选框是 layer_im —— 那时节点位置要按绝对坐标摆）。 */
	struct wlr_scene_tree *tree;
	/* 抓底衬时要藏起来的节点（弹出物自己的树 / 表面节点）。 */
	struct wlr_scene_node *hide;
	struct wlr_surface *surface; /* 尺寸 + 透明度剔除用的缓冲 */
	struct vidow_view *view;     /* X11 弹出物视图（尺寸从它来）；其余 NULL */
	const char *tag;             /* 日志用（"xdg" / "x11" / "im"） */

	struct vidow_fx_target fx;
	/* 抓来的那块区域底衬（跟 layer 的 bd 一个套路）。 */
	int bd_w, bd_h, bd_x, bd_y;
	struct vidow_output *bd_output; /* 这份底衬是从哪块屏抓的 */
	uint64_t bd_gen_scene, bd_gen_layer;
	int blur_fail;       /* 上一次没画成的原因（只在变了时打日志） */
	bool capture_hidden; /* 抓的时候把自己藏起来了（回填时用） */
	/* 上一帧的几何：位置 / 尺寸变了要重烘（候选框那条路节点不跟着表面走）。 */
	int last_x, last_y, last_w, last_h;

	struct wl_listener owner_destroy; /* 宿主对象销毁：自己摘掉 */
	struct wl_listener node_destroy;  /* 模糊节点跟着场景树没了 */
	/* hide 那个节点的销毁 —— **必须有**。hide 是弹出物自己的场景节点（或者
	 * 它所在的那棵树），弹出物关掉的时候它先没，而 pb 还活着（宿主可能是另
	 * 一个对象）。没有这个监听器，pb->hide 就成了野指针，下一次
	 * popup_blur_geom 里 wlr_scene_node_coords(pb->hide, …) 直接 SIGSEGV。
	 * 2026-10-21 12:43 kate 的弹出菜单关掉时崩的就是这里（栈：
	 * wlr_scene_node_coords ← popup_blur_geom ← popup_blur_owner ←
	 * vidow_popup_blur_invalidate ← popup_blur_commit）。 */
	struct wl_listener hide_destroy;
	struct wl_listener commit;        /* 内容变了：重烘（mask / 圆角） */
	bool owner_destroy_added, node_destroy_added, hide_destroy_added,
		commit_added;
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
	/* 上次钉给客户端的 buffer scale（见 layer.c 的 layer_pin_scale）。0 = 还没发。 */
	int pref_scale;
	/* 图层规则算出来的作用（rule.c 填）。 */
	struct vidow_rule_eff rule_eff;
	/* 上一次 layer 模糊没画成的**原因**（fx.c 的 layer_blur_frame 填）：
	 * 0 = 正常画了。只在原因变化时打一行日志 —— 这个函数每帧都跑，
	 * 不打限流就是刷屏。 */
	int blur_fail;
	/* 图层规则的 exe / cmdline：和窗口那边一样，按 pid 抓一次就缓存。 */
	int probe_pid;
	char probe_exe[128];
	char probe_cmdline[256];

	/* animation */
	struct vidow_anim *anim;
	double anim_off_x, anim_off_y;
	double anim_alpha;
	/* 布局（vidow_arrange_layers）定下来的位置，**不含**动画偏移。动画每帧
	 * 只按 base + anim_off 挪这一块自己，不整趟重排 —— 重排会给每块 layer
	 * 面重发一次 configure，见 layer.c 的 layer_apply_anim。 */
	int base_x, base_y;
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
	/* 进全景的时候露不露（规则 overview = "only" / "never"，见 rule.h 的
	 * enum vidow_overview_mode）：只在 overview.c 里用，是它盖在
	 * 「整棵树一起开 / 关」上面的一层逐 surface 覆盖。 */
	int overview_mode;
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

	/* ---- 按键自动重复 ----
	 *
	 * 为什么合成器得自己干这件事：wlroots 0.20 **不做重复**，它只把
	 * repeat_rate/repeat_delay 通过 wl_keyboard.repeat_info 报给客户端，
	 * 指望客户端自己重复。Wayland 原生客户端（终端、编辑器）确实自己重复，
	 * 所以一直没暴露问题。
	 *
	 * 但 Xwayland 不是这样：它是 Wayland 客户端，同时又是 X 服务器的输入
	 * 驱动。X11 的 autorepeat 是**服务器**行为（客户端调
	 * XkbSetDetectableAutoRepeat 与否只决定它看到什么形式），Xwayland 等的是
	 * 我们按 repeat_info 把重复键送过来 —— 收不到就永远不重复。
	 *
	 * 实测（xev 抓 X 侧）：按住一个键 5 秒，应该约 125 次重复，实际只收到
	 * 1~3 次 —— 游戏里就是「按住 W 走两步停一下」。
	 *
	 * 所以这里自己起一个定时器，按住不放就按 repeat_rate 重发。发的是
	 * **同一对 press+release**（X11 autorepeat 的原生形式）：Xwayland 拿到
	 * 之后会翻译成 X 的 KeyPress/KeyRelease 对，客户端用
	 * XkbSetDetectableAutoRepeat 合并成连续 KeyPress。只发 press 不发
	 * release 的话 X 侧会认为键一直没松开，反而卡住。 */
	struct wl_event_source *repeat_timer;
	/* 现在按住的那个键（evdev 码 + 时间戳）。-1 = 没有键按着。 */
	int32_t repeat_keycode;
	uint32_t repeat_time_msec;
	/* 已经等过初始延迟没有：false = 还在 delay 阶段。 */
	bool repeat_started;
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
/* XWM 坐标契约：Wayland 布局/scale 不变；X 每屏使用最大当前原生分辨率。
 * 输出虚拟原点由 Wayland 原点的有序行列决定，负布局原点归一到非负 X。
 * point 为逐屏仿射、size 为逐轴线性，double 精度；DPI 不参与坐标换算。
 * 失败返回 false，point/size 输出原值；boxes/union 清零。输出指针可为 NULL。
 * output_at_native 优先半开矩形命中，屏外取最近输出，无输出返回 NULL。 */
bool vidow_xwm_output_boxes(const struct vidow_server *server,
	struct wlr_output *output, struct wlr_box *wayland, struct wlr_box *native);
bool vidow_xwm_native_union(const struct vidow_server *server,
	struct wlr_box *box);
struct wlr_output *vidow_xwm_output_at_native(const struct vidow_server *server,
	double x, double y);
bool vidow_xwm_to_native_point(const struct vidow_server *server,
	struct wlr_output *output, double x, double y, double *nx, double *ny);
bool vidow_xwm_from_native_point(const struct vidow_server *server,
	struct wlr_output *output, double x, double y, double *lx, double *ly);
bool vidow_xwm_to_native_size(const struct vidow_server *server,
	struct wlr_output *output, double w, double h, double *nw, double *nh);
bool vidow_xwm_from_native_size(const struct vidow_server *server,
	struct wlr_output *output, double w, double h, double *lw, double *lh);
/* 兼容旧调用：仅 X 轴长度，不可用于绝对位置或高度；新调用用上述 API。 */
int vidow_xwm_scale(const struct vidow_view *view);
int vidow_xwm_to_native(const struct vidow_view *view, int v);
int vidow_xwm_from_native(const struct vidow_view *view, int v);
bool vidow_view_surface_coords(struct vidow_view *view,
	struct wlr_surface *surface, double lx, double ly, double *sx, double *sy);
struct wlr_box vidow_xwm_surface_box(const struct vidow_view *view);
void vidow_xwm_configure(struct vidow_view *view, int x, int y, int w, int h);

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
	/* 候选框背后的模糊（见 struct vidow_popup_blur）。 */
	struct vidow_popup_blur *blur;

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
/* 一条历史里留几份 mime：浏览器复制一次带 text/html + text/plain + image/png +
 * text/uri-list 好几份，全留太占内存（见 clipboard.c 的 history_push，按 mime_rank
 * 挑最顺手的几份）。 */
#define VIDOW_CLIP_HIST_MIMES 6

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

/* 一条剪贴板历史。内容是从当时那份选区内容里拷出来的**独立副本**（选区内容随时
 * 会被下一次复制覆盖），所以这里自带 data 生命周期，跟 vidow_clip_selection 无关。
 *
 * 用途：GUI（tools/vidowclip.py）列出历史、点一条复制回来；`vidowmsg clipboard-history*`
 * 也能用。见 clipboard.c 的 history_push。 */
struct vidow_clip_hist_entry {
	struct wl_list link; /* vidow_clipboard.history（最新在前） */
	uint64_t id;         /* 单调递增，GUI 指认条目用（下标会随新条目飘） */
	uint64_t time_ms;    /* 什么时候进来的（CLOCK_MONOTONIC） */
	bool primary;        /* 从哪个选区来的 */
	bool from_x11;       /* 那份内容当时是从 X11 取来的 */
	int nmimes;
	struct vidow_clip_mime mimes[VIDOW_CLIP_HIST_MIMES];
	size_t bytes;        /* 所有 mime 的字节和（算内存预算用） */
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

	/* 历史（最新在前）。count / bytes 是当下占用的条数与字节数，push 时按
	 * clipboard_history / clipboard_history_bytes 两个上限从尾部淘汰。 */
	struct wl_list history; /* struct vidow_clip_hist_entry */
	int hist_count;
	size_t hist_bytes;
	uint64_t hist_next_id;
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

/* 一个被 vidow.env{} / vidow.env() 动过的环境变量（见 env.c）：记着「进来之前
 * 是什么样」，退出时还原 —— vidow 是从别人的 shell / systemd 里起来的，改过的
 * 东西得还回去。 */
struct vidow_env_var {
	struct wl_list link;      /* vidow_server.env_vars */
	char name[128];
	char *before;             /* NULL = 进来的时候没这个变量 */
	bool had_before;
	/* 从 systemd 吸来的（配置里 systemd_env = 0 时要还回去） */
	bool absorbed;
	/* 用户 vidow.env 显式设过：那是他要的，别自动还回去 */
	bool explicit;
};

/* 一个连上来的输入设备（键盘 / 鼠标 / 触摸板 / 虚拟键盘…），只为事件记账：
 * 谁插上了、谁拔了（见 input.c 的 input-device-added / removed）。设备自己没了
 * 就把这条 freed，vidow_input_finish() 收剩下的。 */
struct vidow_input_device {
	struct wl_list link;      /* vidow_server.input_devices */
	struct wl_listener destroy;
	struct vidow_server *server;
	/* 设备本身。拔掉时 destroy 回调会把这个条目摘掉（见 input.c 的
	 * input_device_gone），所以只要条目还在，这个指针就是活的 —— 配置改了要
	 * 重套 libinput 设置，靠它找回设备。 */
	struct wlr_input_device *device;
	char name[128];
	char type[16];
};

/* 推给 systemd 之前记下的「用户管理器里原来是什么样」（见 env.c）：退出时还回去。 */
struct vidow_env_snap {
	struct wl_list link;      /* vidow_server.env_snap */
	char name[128];
	char *value;              /* NULL = 当时用户管理器里没有这个变量 */
	bool had;
};

/* vidow.exec_once() 排队的命令：等这份配置跑完（环境、缩放、DISPLAY 都定了）再
 * 以撒手方式起，然后记进「起过了」（配置重载不重来，见 script.c）。 */
struct vidow_exec_once {
	struct wl_list link;      /* vidow_server.exec_once */
	char *cmd;
};

/* 一条事件钩子（config.lua 里的 vidow.on()，见 hooks.c / hooks.h）。
 *
 * lua = true 的那种拿 lua_ref 指着一个 Lua 函数（在那个 lua_State 的注册表里），
 * 配置重载时整个 Lua 状态会重来一遍，所以那时钩子全部作废、重新声明（见
 * vidow_script_finish）。 */
struct vidow_hook {
	struct wl_list link;      /* vidow_server.hooks */
	int id;                   /* vidow.off() 的句柄（从 1 起，永不重用） */
	bool lua;                 /* false = 起一条命令 */
	bool dead;                /* 派发途中被摘了：跳过，遍历完统一回收 */
	char event[64];           /* 事件名，或 "*" = 全部 */
	char cmd[1024];           /* shell 命令（lua 那种是空的） */
	void *lua_state;
	int lua_ref;
	uint64_t fired;           /* 跑过多少次（IPC {"cmd":"hooks"} 报出去） */
	uint64_t last_ms;         /* 上次跑是什么时候 */
};

/* 等着派发的一个事件：事件名 + 信封 JSON（和给 IPC 订阅者的那条一模一样）。
 * 钩子能做的事和 IPC 命令一样多，所以**不在合成的半路上跑**，一律进队等 idle
 * （见 hooks.c 的 vidow_hooks_dispatch）。 */
struct vidow_hook_event {
	struct wl_list link;      /* vidow_server.hook_queue */
	char *event;
	char *json;
};

/* 弹出物的场景树登记表的一条：surface → 它的场景树。
 *
 * 为什么要记：wlroots 把弹出物场景节点的坐标直接当成「相对**父表面**」的偏移
 * （wlr_scene_xdg_surface_create() 里 set_position(popup->current.geometry)，
 * 见 types/scene/xdg_shell.c），所以嵌套弹出物（二级菜单）的节点必须挂在**父
 * 弹出物**的场景树上 —— 而 wlroots 没有 surface → 场景节点的反查接口，只能自
 * 己记一份。一级弹出物不用它（挂在顶层窗口的树上就是对的）。 */
struct vidow_popup_scene {
	struct wl_list link;         /* vidow_server.popup_scenes */
	struct wlr_surface *surface; /* 弹出物自己的表面（查找时拿它比） */
	struct wlr_scene_tree *tree; /* 它的场景树，子弹出物挂在这上面 */
	struct wl_listener destroy;  /* 表面没了就摘掉这条 */
};

/* 每个按钮上的字形最多几个亮点（5x5 字模，最满的那个「三根竖条」是 15 ——
 * 留点余量给以后换字模）。建节点时超了会当场报错，不悄悄少画。 */
#define VIDOW_MINI_TOOLBAR_GLYPH_MAX 20

struct wlr_cursor_shape_manager_v1;

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
	/* 桌面底色（background_color）：垫在**所有东西**下面。壁纸由 layer-shell
	 * 的 background 层客户端画（swaybg / swww / shell），这个只是「谁都没画」
	 * 时的兜底，所以它在 layer_background 下面一层。 */
	struct wlr_scene_tree *layer_backdrop;
	struct wlr_scene_rect *backdrop;
	struct wlr_scene_tree *layer_background;
	/* overview 的压暗遮罩：夹在壁纸和窗口之间（见 overview.c） */
	struct wlr_scene_tree *layer_overview_dim;
	struct wlr_scene_tree *layer_bottom;
	struct wlr_scene_tree *layer_toplevels;
	struct wlr_scene_tree *layer_popups; /* X11 的菜单/提示：压在所有窗口之上 */
	struct wlr_scene_tree *layer_top;
	struct wlr_scene_tree *layer_overlay;
	struct wlr_scene_tree *layer_im; /* 输入法候选框，压在所有层之上 */
	/* 会话锁（ext-session-lock-v1，见 sessionlock.c）：锁定期间每块屏一块不透明
	 * 黑（layer_lock_blank），lock surface 在最顶上（layer_lock）。这两棵树反过来
	 * —— 只锁定期间开，其余时候关着。 */
	struct wlr_scene_tree *layer_lock_blank;
	struct wlr_scene_tree *layer_lock;
	/* 会话锁的会话级状态（见 sessionlock.c）。locked = true 期间：整棵正常树
	 * 关着、只画 lock surface；客户端中途死掉也保持 true（协议 XML:111-113
	 * 允许「死锁」态，不许合成器自己解锁）。 */
	struct wlr_session_lock_manager_v1 *session_lock_mgr;
	struct wlr_session_lock_v1 *session_lock;
	struct wl_listener new_lock;
	bool locked;
	bool lock_locked_sent;               /* locked 事件发过没（只能发一次） */
	struct wl_list lock_surfaces;        /* struct vidow_lock_surface */
	struct wl_listener lock_new_surface;
	struct wl_listener lock_unlock;
	struct wl_listener lock_destroy;
	unsigned lock_frames_pending;        /* 还没呈现「锁后第一帧」的屏数 */
	struct wl_event_source *lock_timeout; /* 等帧的超时兜底 */
	/* 空闲通知 / 空闲抑制（ext-idle-notify-v1 + zwp-idle-inhibit-unstable-v1，
	 * 见 main.c 的 idle_*）：swayidle / hypridle 靠前者拿 idled / resumed 事件
	 * 触发锁屏；播视频的客户端靠后者说「别锁屏」。wlroots 只提供协议、语义归
	 * 合成器 —— 活动要自己喂、inhibit 要自己转达，两处接线都在 main.c。 */
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
	struct wl_listener idle_new_inhibitor;
	struct wl_listener idle_new_input;
	int idle_inhibitors; /* 活着的 inhibitor 数（> 0 = 抑制中） */
	struct wlr_output_layout *output_layout;
	struct wlr_scene_output_layout *scene_layout;

	struct wlr_compositor *compositor;
	struct wlr_xdg_shell *xdg_shell;
	/* XWayland（见 xwayland.c）。xwayland 为 NULL = 配置里关了。
	 *
	 * **只用 wlr_xwayland_server**，不再用 wlr_xwayland。
	 *
	 * wlr_xwayland 会在内部建一个 wlr_xwm：X 连接、事件循环、窗口对象、
	 * 什么时候把 pixmap 导出成 dmabuf，全归它管。我们只能在外面看它给出
	 * 的结果 —— Steam 第二次开主窗口全黑就是栽在这上面（X 侧 import 抓到
	 * 99% 非黑，我们拿到 3808x2050 的纹理，里面像素全是 0，而决定这件事的
	 * 代码在 wlroots 内部，插不进去）。
	 *
	 * 现在照 Hyprland 的 src/xwayland/ 做：wlr_xwayland_server 只负责
	 * fork/exec Xwayland 并把 wm_fd 交出来，XWM 是我们自己的（见
	 * src/xwm_x11.c），窗口对象也是自己的（src/x11surface.h）。 */
	struct wlr_xwayland_server *xwayland_server;
	struct vidow_x11_server *xwm;
	/* xwayland_shell_v1：只用来把 wl_surface 和 WL_SURFACE_SERIAL 对上号。
	 * 这是唯一还需要 wlroots 参与 X11 的地方。 */
	struct wlr_xwayland_shell_v1 *xwayland_shell;
	/* start：fork 之后立刻（bind 白名单要在这里给，见 xwayland.c）。 */
	struct wl_listener xwayland_start;
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
	/* wp_cursor_shape_manager_v1（光标形状协议）：layer / GTK 客户端用它换形状，
	 * 不实现的话指针永远停在默认箭头。 */
	struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
	struct wl_listener cursor_shape_request;
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
	struct vidow_mesh_blur_prog mesh_blur;
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
	/* 这一趟 arrange 不要推世界代次（gen_scene）。视口过渡每帧重摆用 ——
	 * 每帧推一次会让模糊金字塔每帧整块重烘，主线程打满。见
	 * viewport_anim_update() 的说明。 */
	bool arrange_skip_world_damage;

	/* 正在渲染的那个输出的物理尺寸和布局原点（物理像素），给着色器当
	 * u_output_size / u_origin 的基准。 */
	int output_w, output_h;
	int output_x, output_y;

	struct wl_list anims;    /* vidow_anim.link */
	/* 正在跑 vidow_anim_tick()。这个标志是给 vidow_anim_cancel() 看的：tick
	 * 用 wl_list_for_each_safe 迭代，tmp 是「下一个」的裸指针，谁在这时候
	 * free 掉另一个 anim，tmp 立刻变野指针 —— 崩在 vidow_output_render →
	 * vidow_anim_tick（真机 coredump 里那个 0x2500000f00 就是踩了 free 掉的
	 * anim 的 update 函数指针）。所以 tick 期间只标 cancelled，由 tick 自己收尾。 */
	bool anim_ticking;
	struct wl_list closing; /* vidow_closing.link */
	struct wl_list shaders; /* vidow_shader.link */
	bool shader_ok;         /* GLES2 custom shader path usable */

	struct wl_list outputs;   /* vidow_output.link */
	struct wl_list views;     /* vidow_view.link */
	/* 活着的 Wayland 弹出物的场景树（vidow_popup_scene.link）：嵌套弹出物要
	 * 靠它找到父弹出物的树，见 view.c 的 server_new_popup。 */
	struct wl_list popup_scenes;
	/* 活着的弹出物模糊目标（struct vidow_popup_blur.link）：窗口的 xdg_popup /
	 * X11 菜单 / 输入法候选框各一条，见 fx.c 的 vidow_popup_blur_register。 */
	struct wl_list popup_blurs;
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
	/* ext-workspace-v1 的服务端状态（src/extworkspace.c）。NULL = 没提供。 */
	struct extws *extworkspace;
	struct wl_list keybinds;   /* vidow_keybind.link */
	struct wl_list gestures;   /* vidow_gesture.link（触控板手势） */
	struct wl_list rules;      /* vidow_rule.link（窗口规则） */
	struct wl_list output_confs; /* vidow_output_conf.link（每块屏一组配置） */
	struct wl_list test_actions; /* vidow_test_action.link（VIDOW_TEST_ACTION） */
	struct wl_event_source *test_pipe; /* 一次性定时表 / 动作 FIFO 的 fd 源 */
	/* 见过的每一块屏都留一条记忆（拔了也不删），热插拔按它还原工作区。 */
	struct wl_list output_memories; /* vidow_output_memory.link */
	struct wl_list execs;     /* vidow_exec.link, config commands in flight */
	struct wl_event_source *exec_timer; /* one shared timeout timer */
	struct wl_event_source *capture_timer; /* drives pending screen captures */
	/* 抓帧节奏（Hz）。录屏 / 远程控制靠它推进：屏幕静止时没有输出提交，
	 * 全靠 capture_watchdog 按这个频率催帧（见 output.c）。
	 *
	 * 默认 60：局域网远程控制在手机上 30fps 才勉强跟手，60fps 才叫流畅；
	 * 抓一帧 1080p 约 3-5ms，60Hz 只占 20-30% 单核，可以接受。
	 * 0 或负数 = 关掉兜底（只靠后端自己的提交出帧，适合「只录不推」的场景）。
	 * 配 VIDEO/VIDOW_CAPTURE_FPS 或 config.lua 的 capture_fps。 */
	int capture_fps;
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
	/* 光标隐藏（hide_when_typing / hide_after_inactive_ms，见 input.c 的
	 * 「光标隐藏」段）：指针静止到这个闹钟响就把光标藏起来；hidden 记着当前
	 * 藏没藏（指针一动 / 按键来的时候要显示回来）。 */
	struct wl_event_source *cursor_hide_timer;
	bool cursor_hidden;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	/* 触控板手势（libinput 的 swipe / pinch / hold，见 gesture.c）。 */
	struct wl_listener pointer_swipe_begin;
	struct wl_listener pointer_swipe_update;
	struct wl_listener pointer_swipe_end;
	struct wl_listener pointer_pinch_begin;
	struct wl_listener pointer_pinch_update;
	struct wl_listener pointer_pinch_end;
	struct wl_listener pointer_hold_begin;
	struct wl_listener pointer_hold_end;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
	/* 拖拽（wl_data_device.start_drag）：wlroots 只把请求抛上来，起不起、
	 * 图标画在哪由合成器定。不接这条，应用内拖拽（选文字拖、拖文件、拖标签）
	 * 全部没反应。 */
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;
	struct wl_listener drag_destroy;
	struct wl_listener drag_motion;
	struct wlr_scene_tree *drag_icon;
	struct wlr_drag *drag;

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
	/* **vidow 自己记的指针按键数**（不是 wlroots 的 button_count）。
	 *
	 * 为什么要另记一份：`wlr_seat_pointer_notify_enter()` 一旦换了焦点表面
	 * 就会调 `reset_buttons()`，把 pointer_state.button_count 直接清 0
	 * （types/seat/wlr_seat_pointer.c:449）。于是「窗口里按下 → 拖到窗口外 →
	 * 再拖回窗口」这条路上，移回来的那一刻 button_count 已经是 0 了，而
	 * input.c 里「有键按着就别清焦点」那道保护正是靠它判的 —— 一回来就判成
	 * 「没按着」，焦点被清掉，客户端收到 LEAVE、再也收不到 RELEASE：文本拖拽
	 * 选择停在半路（用户报的「拖出去选择就停住、移回来松手还跟着鼠标」）。
	 *
	 * 这份计数只由 vidow 在真正的按下 / 松开入口加减（vidow_pointer_button），
	 * 不受 wlroots 换焦点的影响。判「是不是还在抓」用 pointer_held()。 */
	int pointer_button_count;
	/* 小窗（S2）形态拖拽：迷你 / 图标态客户端收不到任何指针事件，所以同一个
	 * 左键按下要同时承载「单击 = 放大」和「拖拽 = 移动」两个语义 —— 按累计位移
	 * 和 mini_drag_threshold_px 区分（见 input.c 的 mini_pointer_*）。
	 * 单向不回退：一旦判成拖拽就锁到松手（慢速拖出去再拖回来不该变成单击）。
	 * 和上面的 grab_mode 是两套东西：那个是给平铺 / 浮窗的（会转事件给客户端），
	 * 这个全程吞事件。 */
	struct vidow_view *mini_grab_view;
	double mini_grab_press_x, mini_grab_press_y; /* 按下点（布局坐标） */
	double mini_grab_dx, mini_grab_dy;           /* 按下点 - 窗口左上角 */
	bool mini_grab_dragging;                     /* 已经越过阈值（单向） */
	uint32_t mini_grab_button;
	/* 甩出（S5）：最近几次指针采样（vidow_pointer_motion 每次都推一格），
	 * 松手时按「最近 ~100ms 的那一段」算速度 —— **不用单帧增量**：一帧的
	 * dx/dy 在高刷新率下小得量不出意图，在低刷新率下又抖得离谱。 */
	struct vidow_fling_pt { double x, y; int64_t ns; } mini_fling_pts[8];
	int mini_fling_n;
	/* 这一趟形态转换的时长覆盖（毫秒，0 = 用 mini_transition_ms）：甩出的
	 * 时长由速度推出来（越快越短），见 float.c 的 mini_transition_start。 */
	int mini_transition_override_ms;
	/* 拖动时指针比刷新快：一帧里会来好几个移动事件，中间那些位置屏幕上从来没
	 * 出现过。重画装饰（整块窗口那么大的离屏渲染）+ 记损伤推迟到出帧之前做一次
	 * （见 vidow_tile_grab_update / output.c 的帧回调）—— 实测每个事件 66µs
	 * （1000Hz 鼠标 = 每帧 0.4ms，8000Hz = 每帧 3.3ms，全是白算的）。
	 * grab_last_box = **已经画出来**的那个位置（损伤从它算起）。 */
	bool grab_dirty;
	struct wlr_box grab_last_box;
	/* 拖动落点预览块（niri 的 insert hint，见 drag.c）：半透明的一块，拖平铺窗口
	 * 的时候按落点摆出来 —— 新列是一条整高的「卡片」，插进已有列是那条列里的
	 * 一条横条。平时 node.enabled = false。建在 layer_toplevels 里、压在别的窗口
	 * 之上、被拖的那个之下（见 vidow_insert_hint_update）。 */
	struct wlr_scene_rect *insert_hint;
	/* ---- 小窗（mini window，S4）：拖拽落点区 + 悬停工具条 ----
	 *
	 * 落点区**只在拖拽中出现**（拖平铺 / 浮窗 / 自由小窗的时候，光标落在某块屏
	 * 右上角那块区域里才亮）—— 常亮的话「盲拖到角落」会跟现有的平铺落点撞车。
	 * 两块半透明矩形：底板 = 落点区本身，preview = 松手会得到的那个迷你小窗的
	 * 大小和位置（用户要的「带目标预览」）。
	 *
	 * 工具条：迷你 / 图标态悬停时露出的四个按钮（放大 / 收成图标 / 还原成列 /
	 * 关闭）。不是表面，命中靠矩形判定（见 vidow_mini_toolbar_hit）—— 所以位置
	 * 得记在 server 上，按钮点击和绘制用的是同一份。 */
	struct wlr_scene_rect *mini_drop_zone;    /* 落点区底板 */
	struct wlr_scene_rect *mini_drop_preview; /* 落点区里的目标预览块 */
	bool mini_drop_active;                    /* 这一刻落点区亮着没有 */
	struct wlr_output *mini_drop_output;      /* 亮在哪块屏上 */
	struct wlr_scene_rect *mini_toolbar_bg;   /* 工具条底板 */
	struct wlr_scene_rect *mini_toolbar_btn[4];
	struct vidow_view *mini_toolbar_view;     /* 工具条现在属于哪个小窗 */
	/* 工具条按钮的那一对按下 / 松手要成对吃掉（客户端两边都没见过）——
	 * 按在按钮上的那次按下记在这儿，松手时靠它判断。 */
	bool mini_toolbar_pressed;

	/* ---- 小窗（mini window，S5）：图标悬停预览 ----
	 *
	 * 悬停图标 mini_icon_hover_ms 之后，在**朝屏内**那一侧展开一块迷你尺寸
	 * ×0.8 的**实时预览**（同一张表面的第二份镜面，先例 wincapture.c 的
	 * wlr_scene_subsurface_tree_create）。只读：不抢焦点、不进 MRU、不参与
	 * vidow_view_at 命中 —— 但它**吞点击**（看到指针底下有块「像窗口的东西」，
	 * 点下去却激活了背后的窗口是明显的错）。
	 *
	 * 时间轴用 vidow_anim_start 当定时器（不新开 timer）：延迟 250ms → 出；
	 * 离开 300ms → 收；指针从图标移向预览要跨那 8px 空隙，所以「既不在图标
	 * 也不在预览」时给 200ms 宽限，进到预览矩形里就把计时清掉（宽限期只是
	 * 视觉稳定性，不引入新的输入语义 —— 预览不吃点击语义、只吞掉）。 */
	struct wlr_scene_tree *mini_preview;      /* 预览的镜面节点（layer_top 里） */
	struct wlr_scene_surface *mini_preview_surface; /* 里面那层内容节点（缩放 / 透明度） */
	/* 预览的快照节点（同一个窗口走快照那条路时用它，**和图标同一份策略** ——
	 * 不能出现「图标静止、预览还在动」）。 */
	struct wlr_scene_buffer *mini_preview_snap;
	struct vidow_view *mini_preview_view;     /* 预览现在属于哪个图标 */
	bool mini_preview_shown;                  /* 已经露出来了（含淡入中） */
	struct wlr_box mini_preview_box;          /* 预览那块（布局坐标，命中用） */
	struct vidow_anim *mini_preview_fade;     /* 淡入 / 淡出 */
	double mini_preview_alpha;                /* 现在的不透明度 */
	double mini_preview_alpha_from, mini_preview_alpha_to;
	struct vidow_anim *mini_hover_timer;      /* 延迟 / 离开的定时器 */
	struct vidow_view *mini_hover_view;       /* 定时器属于哪个图标 */
	bool mini_hover_leaving;                  /* 上面那条定时器是「离开」还是「延迟」 */
	bool mini_preview_pressed;                /* 预览上那对按下 / 松手要成对吞 */
	/* 最近一个小窗的 id（键位动作用）：迷你 / 图标态**不拿键盘焦点**，所以
	 * server->focused 指不到它们 —— cycle-mini-state 这类动作得有别的指认办法。
	 * 存 id 不存指针：窗口随时可能被关掉，指针会悬空。 */
	uint64_t mini_last_id;
	struct wlr_box mini_toolbar_box;          /* 工具条整块（布局坐标） */
	struct wlr_box mini_toolbar_btn_box[4];   /* 四个按钮（布局坐标） */
	/* 按钮上的字形（**不是**表面，也不是字库：每个亮点一个 3x3 的小方块，
	 * 字模在 drag.c）：以前四个按钮就是四个空方块 —— 用户报的「没有内容只有
	 * 四个格子」。顺序 = 字模行优先，节点在 vidow_mini_ui_init 里一次建好。
	 * 第一个下标 = enum vidow_mini_toolbar_btn（4 个）。 */
	struct wlr_scene_rect
		*mini_toolbar_glyph[4][VIDOW_MINI_TOOLBAR_GLYPH_MAX];
	/* 浮动窗口拖拽同理：每帧最多重排一次（见 vidow_float_grab_update）。 */
	bool arrange_dirty;
	/* 正在跑的列宽过渡有几条。过渡期间**位置跟着变的那几列不做位移补间**：
	 * 每帧都重起一次补间的话它们会一直吊在过渡后面、过渡结束后再自己飘到位
	 * （见 view.c 的 vidow_place_view）。用计数不用 bool：连着来两条 IPC 改宽
	 * 就是两条列同时在过渡。 */
	int column_anim_n;
	/* 正在跑切工作区过渡的工作区数（正常 0）：> 0 时 vidow_place_view 直接落位，
	 * 不给每个窗口起位移补间 —— 整块工作区要刚性跟着过渡曲线走。 */
	int workspace_slide_n;
	/* 探针：最近一次拖拽移动事件的时刻（见 output.c 的拖拽延迟统计） */
	int64_t grab_ev_ns;
	/* 平铺改宽（Mod+右键拖）：起拖时的光标 x 和那一列的像素宽。改宽按**增量**
	 * 算（新宽 = 起拖宽 + 光标位移）—— 按「右边缘 = 光标 x」的绝对算法，列一变宽
	 * 视口就会跟着滚，下一帧光标底下的右边缘就跑了，手会抖。 */
	double resize_from_x;
	int resize_from_width;

	/* overview（niri 风格全景） */
	struct vidow_overview overview;

	/* Alt+Tab 切换器 / 最近用过的窗口（见 mru.c） */
	struct vidow_mru mru;

	/* IPC（见 ipc.c / ipcstate.c） */
	struct wl_event_source *ipc_listen; /* 监听 socket 的 fd 源 */
	struct wl_list ipc_clients;         /* ipc_client.link */
	char ipc_path[PATH_MAX];            /* 实际在听的路径（空 = 没起来） */
	int ipc_subscribers;                /* 订阅了事件的连接数 */

	/* 事件钩子（见 hooks.c）：config.lua 里 vidow.on() 声明的那些。事件发出去
	 * 的时候 IPC 订阅者和钩子一起喂（ipcstate.c 的 emit）。 */
	struct wl_list hooks;               /* vidow_hook.link */
	struct wl_list hook_queue;          /* vidow_hook_event.link（等派发） */
	struct wl_event_source *hook_idle;
	int hook_count;                     /* 现存的钩子数（0 = 快速通道） */
	int hook_queue_len;
	int hook_next_id;
	int hook_dispatch_depth;            /* > 0 = 正在派发，摘钩子只标记不回收 */
	/* 钩子里的 Lua 调用（在等 vidow.exec 的那种，见 script.c） */
	struct wl_list hook_calls;

	/* 环境变量账本（见 env.c）：和 systemd 用户管理器两个方向对账 */
	struct wl_list input_devices;       /* vidow_input_device.link（事件用） */
	struct wl_list env_vars;            /* vidow_env_var.link */
	struct wl_list env_snap;            /* vidow_env_snap.link（用户管理器的原样） */
	struct wl_event_source *env_idle;   /* 合并导出用的 idle 源 */
	int env_exports;                    /* 推给 systemd 几次 */
	bool env_pulled;                    /* 吸过 systemd 的环境了吗 */
	/* vidow.exec_once()：排队的命令 + 已经起过的（配置重载不重来） */
	struct wl_list exec_once;           /* vidow_exec_once.link */
	char **exec_once_ran;
	size_t exec_once_ran_n;
	/* IPC 报给外面的稳定编号（从 1 起，0 = 还没分配） */
	uint64_t next_view_id;
	uint64_t next_column_id;
	uint64_t next_layer_id;
	/* 最小化的先后（「放回最近收起来的那个」按它挑，见 view.c） */
	uint64_t next_minimize_seq;

	/* 指针现在压在哪个窗口上（focus_follows_mouse 用）。
	 * 焦点只在这一格「从别的窗口换过来」时才跟着走 —— 键盘刚把焦点切走、指针
	 * 还停在同一窗口里的时候，挪一下鼠标不该把焦点抢回来。 */
	struct vidow_view *hovered_view;
	struct vidow_view *focused;
	/* 辅助窗口（wl-clipboard 那种 1x1）**借走键盘之前**，焦点在哪个窗口上。
	 *
	 * 这是「粘贴一次，焦点跳到下一个窗口」的修法，抄 Hyprland 单点存焦点的
	 * 思路（src/desktop/state/FocusState.cpp 的 m_focusWindow）：辅助窗口不走
	 * 上面这条焦点链，它只临时抢一下 wl_seat 的键盘焦点；归还时如果去读
	 * server->focused，读到的可能已经不是借出前那个窗口了（借出期间任何一次
	 * 重算焦点都会把它改掉），焦点于是落到「下一个」窗口上。
	 *
	 * 所以在借出的那一刻把当时的焦点窗口记在这儿，还的时候精确还给它。
	 * NULL 表示「借出时根本没有焦点窗口」，还的时候就清掉焦点。 */
	struct vidow_view *focus_borrowed_from;
	/* 上一次拿着焦点的**列**（center_focused_column = "on-overflow" 要用：
	 * 「焦点列和上一个焦点列能不能一起完整放进视口」是居中与否的判据，见
	 * view.c 的 focus_viewport_target）。列被销毁时（vidow_view_leave_column）
	 * 会清掉，别拿着野指针。 */
	struct vidow_column *last_focused_col;
	struct vidow_layer_surface *keyboard_layer; /* exclusive layer grab */
	/* 抢键盘之前那个窗口：layer 关掉之后要还给它。不记这一份的话，
	 * 截图 / 通知这类 layer 一关，焦点会落到「当时恰好是 focused 的那个」
	 * —— 用户报的「截图完成焦点弹到下一个窗口」。 */
	struct vidow_view *focus_before_layer;
	struct wlr_output *focused_output;
	/* 上一次切过去的那块工作区（workspace_auto_back_and_forth 要用）：只记
	 * 「键位动作切过去的」那一次，见 keybind.c 的 action_workspace_switch。 */
	struct vidow_workspace *last_workspace;

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
	/* fd 泄漏看门狗（VIDOW_FD_WATCH=1 才建，见 fdwatch.c）：每 3 秒数一次
	 * /proc/self/fd，超过软上限的 80% 就打一份分类快照。
	 *
	 * 为什么需要它：2026-10-21 12:09 那次 sunshine 连上来，vidow 的 fd 一分钟
	 * 内从 4 涨到 700+（其中 871 个是匿名 dmabuf），打满 1024 的软上限之后
	 * `dup failed: Too many open files`，IPC 全断（Broken pipe）、sunshine 再也
	 * 连不上 —— 而 vidow 自己的日志里**一个字都没有**（全在 wlroots 内部报
	 * ERROR，和别的东西混在一起）。等发现的时候现场已经过去了。这个看门狗
	 * 就是补那段沉默。 */
	struct wl_event_source *fd_watch_timer;
	/* 上一次报的数（只在涨了才报，免得同一水平刷屏）。 */
	int fd_watch_last;
	/* 已经报过几次（超过 5 次就闭嘴，避免每 3 秒一条把日志冲爆）。 */
	int fd_watch_reports;

	/* 自唤醒 fd 统管（见 fdwatch.c）：合成器自己的自唤醒 fd（Xwayland ready、
	 * 配置 inotify、exec 管道、IPC socket、剪贴板拉取管道……）**不挂**
	 * wl_event_loop_add_fd —— libwayland 会 dup 一份藏进 /proc/self/fd 里那个
	 * 非特权用户读不了的目录，排查时什么都看不到。改由一个 16ms 的 timerfd
	 * 节拍统一 poll：没人有事 = 这一拍什么也不做。 */
	struct vidow_fdwatch *fdwatch;
	/* IPC 监听 socket 的 fd（-1 = 没在听）。fdwatch 在盯它，拆 / 换的时候
	 * 要先注销 —— 否则同样的 fd 号被新 socket 复用时会叫到旧的登记上。 */
	int ipc_fd;

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
/* 同上，但记一笔「这块屏上有抓帧在等」：光标抓帧（ext-image-copy-capture 的
 * pointer cursor session）的光标不在画面里 —— 以及光标在硬件光标层 —— 时那块屏
 * 永远不会提交，客户端就一直等下去。screencopy 那边只推**有它自己的帧**的屏，
 * 所以光标那条路得自己记这笔账（看门狗醒来时先清掉这一轮的标记，下一次
 * cursor_source_request_frame 再记一次，转一次就停）。 */
void vidow_output_capture_kick_output(struct vidow_server *server,
		struct wlr_output *wlr_output);
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
/* 退出：把输入设备的账本收掉（见 input.c）。 */
void vidow_input_finish(struct vidow_server *server);
/* libinput 的手柄：只在 input.c 里用（<libinput.h> 只在那儿包含，免得整个头文件
 * 把 libinput 拖进来）。 */
struct libinput_device;
/* 把配置里那一族 libinput 设置（configs/19-input.lua）套到一台 libinput 设备上。
 * 不是 libinput 的设备（虚拟键盘 / 虚拟指针）直接跳过 —— wlroots 的
 * wlr_libinput_get_device_handle() 对它们会 assert 掉。设了才动，没设的项一概
 * 不碰（默认值 = vidow 以前的行为）。 */
void vidow_input_apply_libinput(const struct vidow_config *cfg,
		struct libinput_device *dev, const char *name);
/* 上面那个套到当前所有设备上：设备插入时（热插拔也走这条路）和配置改了时各调
 * 一次（后者由 config.c 的观察者触发，见 vidow_input_init）。 */
void vidow_input_apply_all(struct vidow_server *server);
/* 光标隐藏 / 显示（hide_when_typing / hide_after_inactive_ms）。实现在
 * cursorfx.c（它记着「当前该显示哪张光标图」），调用点在 input.c。 */
void vidow_cursor_fx_set_hidden(struct vidow_server *server, bool hidden);
/* 把「关着的」输出全打开（mouse_move_enables_dpms / key_press_enables_dpms）。
 * 返回打开了几个（0 = 当时没有关着的屏，调用者就别打日志）。 */
int vidow_output_wake_disabled(struct vidow_server *server);
/* toggle-touchpad 动作：把触控板整体关掉 / 打开（见 input.c）。why 收一句人话
 * （成功 / 失败的原因），给 IPC 和日志用。 */
bool vidow_input_toggle_touchpad(struct vidow_server *server, const char **why);
bool vidow_view_init(struct vidow_server *server);
/* xdg_popup 的 unconstrain 只能在 popup 自己第一次 commit 之后做（wlroots 里
 * schedule_configure 有 assert(surface->initialized)），见 view.c 的注释。 */
void vidow_popup_unconstrain_on_commit(struct vidow_server *server,
	struct wlr_xdg_popup *popup, const struct wlr_box *box);
/* 输出盒（布局坐标）-> 根 toplevel 表面自己的坐标系：unconstrain 要的是后者，
 * 少这一步窗口不在布局原点时菜单会被摆到屏外（见 view.c 的注释）。 */
void vidow_popup_box_to_toplevel_space(const struct vidow_view *view,
	struct wlr_box *box);
/* 建弹出物的场景树：父面本身是弹出物（二级菜单）就挂到父弹出物的树上，否则挂
 * fallback（窗口的树 / layer 表面的树）。**两个调用点都得用它**：窗口的弹出物
 * （view.c）和 layer surface 的弹出物（layer.c）—— 挑错父树的话子菜单会平移
 * 一段「父菜单相对 fallback 原点」的距离（见 view.c 的注释）。 */
struct wlr_scene_tree *vidow_popup_scene_create(struct vidow_server *server,
	struct wlr_scene_tree *fallback, struct wlr_xdg_popup *popup);
/* 「根是这块 layer 表面」时 unconstrain 要的可用区（输出盒平移到这块 layer 表面
 * 自己的坐标系）。一级弹出物（layer.c）和挂在 layer 表面上的二级弹出物（view.c）
 * 都走它 —— 少这一步 box 会是 0x0，客户端把菜单摆到屏幕左上角（见 layer.c）。 */
void vidow_popup_box_to_layer_space(struct vidow_server *server,
	const struct vidow_layer_surface *ls, struct wlr_box *box);
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

/* 输入法抓着键盘时的按键出口（见 im.c 的 active_grab）：抓着就发给它，否则
 * 调用方走 seat。以前 grab 只存不用，fcitx5 一抓键盘所有按键就丢了。 */
void vidow_im_grab_send_key(uint32_t time_msec, uint32_t keycode, bool pressed);

/* 把键盘焦点按到某个表面上。**surface 可能是 NULL**（Xwayland 的窗口刚建出来 /
 * 表面刚销毁但窗口还在 —— wine 那些游戏起来时会来回好几趟），所以统一从这里走：
 * wlroots 的 wlr_seat_keyboard_notify_enter 里是 assert(surface)，传 NULL 就是
 * 把合成器 abort 掉。NULL 时改成「清掉键盘焦点」。 */
void vidow_seat_focus_surface(struct wlr_seat *seat,
		struct wlr_surface *surface);

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
/* "RRGGBB" / "RRGGBBAA"（# 可有可无）→ 四个 0..1 分量。配置键和窗口规则的颜色
 * 作用共用这一份。 */
bool vidow_config_parse_color(const char *value, float out[4]);
/* niri 那种百分比写法："10%" → 10.0（要求结尾就是 '%'，后面不许再有东西 ——
 * 和 niri 的 Percent 解析同一套规矩）。用于 focus-follows-mouse 的
 * max-scroll-amount。 */
bool vidow_config_parse_percent(const char *text, double *out);
bool vidow_config_has(const char *key);
/* 这个键的值是一张「数表」（`preset_column_widths = { 0.25, 0.5 }`）—— 文本形
 * 式是逗号分隔的一串数（`"0.25, 0.5"`，`vidowmsg config` 读回来的也是它）。
 * script.c 那边靠它把 Lua 数组转成这串文本，见 list_as_text()。 */
bool vidow_config_list_has(const char *key);
bool vidow_config_set(struct vidow_config *cfg, const char *key,
		const char *value);
const char *vidow_config_get(const struct vidow_config *cfg,
		const char *key);
/* 配置真的改到了之后通知谁。三条写路径（config.lua / VIDOW_* / IPC set-config）
 * 最后都落到 config.c 的 apply()，通知就挂在那儿，一处盖全。
 *
 * 现在只有 input.c 用：libinput 的设置是存在**设备**上的，配置重载 / set-config
 * 不会自动跟过去，得重套一遍。key == NULL = 整份配置都可能变了（配置重载先把
 * 默认值恢复一遍，那一步不走单个键）。回调里别再改配置（会递归）。 */
void vidow_config_observe(void (*cb)(struct vidow_config *cfg, const char *key,
		void *data), void *data);
/* 一条曲线打回配置里能写的那种文本（`vidowmsg config easing` 看到的就是它，
 * 拷回 config.lua 能原样再读一遍）。buf 至少 64 字节。 */
const char *vidow_ease_text(const struct vidow_ease *ease, char *buf,
		size_t size);
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

/* 事件钩子里的 Lua 那一半（实现在 script.c：Lua 状态机在那儿）。
 *
 * vidow_script_hook_run(): 跑一个 Lua 钩子，载荷是事件信封的 JSON（和给 IPC
 * 订阅者的那条一样）：解码成 Lua 表当参数，协程里跑，钩子里能 await
 * vidow.exec()（和配置文件里那套一样）。返回 false = 这次没跑起来（Lua 状态
 * 已经换了代 / 函数不是函数 / 出错）。 */
bool vidow_script_hook_run(struct vidow_server *server, struct vidow_hook *hook,
		const char *json);
/* 钩子没了：把 Lua 注册表里的引用还回去。 */
void vidow_script_hook_unref(void *lua_state, int ref);
/* 这个 lua_State 还是当前这一代配置的吗（配置重载会换一个）。 */
bool vidow_script_alive(const struct vidow_server *server, void *lua_state);

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
/* vidow.include() 读了哪些路径（原样，没解析符号链接）—— IPC 上报给用户看。 */
size_t vidow_configwatch_files(struct vidow_server *server,
		const char *const **out);
/* 盯着的目录（inotify）。 */
size_t vidow_configwatch_dirs(struct vidow_server *server,
		const char *const **out);

/* ---------------------------------------------------- 自唤醒 fd 统管（fdwatch.c）
 *
 * 合成器自己的自唤醒 fd 一律走这里，不交给 wl_event_loop_add_fd：
 *
 *   vidow_fdwatch_register(fd, kind, handler, data)
 *       可读 → handler(fd, data)。返回 1 = 读完了；0 = 「还没读完，下一拍再来」
 *       （返回 0 必须真读走了一点东西，否则就是白转一整拍）。
 *       注册失败（fdwatch 起不来）= false，调用方自己挂 add_fd 兜底。
 *   vidow_fdwatch_deregister(fd, kind, data)
 *       kind / data 可以给 VIDOW_FDWATCH_ANY / NULL 放宽匹配。fd 已经关了也行。
 *   vidow_fdwatch_peek(fd, kind, poke, data)
 *       只看一眼：可读就 call poke(data)（读不读、读多少，poke 自己定）。
 *   vidow_fdwatch_poke(server)
 *       从别处捅一下：下一拍立刻再来一轮，不等节拍。
 *   vidow_fdwatch_tracks(fd, kind)
 *       这个 fd 在不在册（调试 / IPC 用）。 */
enum vidow_fdwatch_kind {
	VIDOW_FDWATCH_ANY = 0,
	VIDOW_FDWATCH_XWAYLAND = 1,
	VIDOW_FDWATCH_CONFIG = 2,
	VIDOW_FDWATCH_EXEC = 3,
	VIDOW_FDWATCH_IPC = 4,
	VIDOW_FDWATCH_CLIPBOARD = 5,
	VIDOW_FDWATCH_CAPTURE = 6,
	VIDOW_FDWATCH_OTHER = 7,
};
typedef int (*vidow_fdwatch_handler)(int fd, void *data);
typedef void (*vidow_fdwatch_poke_cb)(void *data);

void vidow_fdwatch_init(struct vidow_server *server);
void vidow_fdwatch_finish(struct vidow_server *server);
bool vidow_fdwatch_register(int fd, enum vidow_fdwatch_kind kind,
		vidow_fdwatch_handler handler, void *data);
void vidow_fdwatch_deregister(int fd, enum vidow_fdwatch_kind kind, void *data);
bool vidow_fdwatch_peek(int fd, enum vidow_fdwatch_kind kind,
		vidow_fdwatch_poke_cb poke, void *data);
void vidow_fdwatch_poke(struct vidow_server *server);
bool vidow_fdwatch_tracks(int fd, enum vidow_fdwatch_kind kind);
struct vidow_server *vidow_fdwatch_server(void);
/* fd 泄漏看门狗（VIDOW_FD_WATCH=1 才真的建，见 fdwatch.c）。每 3 秒数一次
 * /proc/self/fd，超过软上限 80% 就打一份分类快照 —— 2026-10-21 那次 sunshine
 * 的匿名 dmabuf fd 泄漏，vidow 自己一个字都没打，等发现时现场已经过去了。 */
void vidow_fd_watch_init(struct vidow_server *server);
void vidow_fd_watch_finish(struct vidow_server *server);

/* ------------------------------------------------------------- animation */

int64_t vidow_now_ns(void);
struct vidow_anim *vidow_anim_start(struct vidow_server *server,
		const char *tag, int duration_ms, struct vidow_ease easing,
		void (*update)(void *data, double t, double linear),
		void (*done)(void *data, bool cancelled), void *data);
/* 回弹幅度 → out_back 里那个常数（峰值 = 4c1³/(27(c1+1)²) 的反解）。见 anim.c。 */
double vidow_ease_out_back_c1(double overshoot);
/* 取一条缓动曲线在 t（0..1 的**线性**进度）处的值。一条 anim 里要用两条曲线
 * （小窗转换：位置单调、缩放可过冲）时用这个 —— 别为第二条曲线再起一条 anim。 */
double vidow_ease_value(const struct vidow_ease *ease, double t);
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
/* 果冻专用：verts 每项 6 个 float [x, y, u_content, v_content, u_blur,
 * v_blur]；片元把内容叠在模糊上（content + blur * (1 - content.a)）。 */
bool vidow_fx_render_shadow(struct vidow_server *server,
	struct wlr_buffer *dst, struct vidow_fx_target *fx,
	const struct vidow_look *look);
bool vidow_fx_draw_mesh_blur(struct vidow_server *server,
	struct wlr_buffer *dst, int w, int h, struct wlr_texture *content,
	struct wlr_texture *blur, float blur_strength, const float *verts,
	int n_verts, int content_off_x, int content_off_y, int content_w,
	int content_h, int src_w, int src_h, float radius, float corner_n,
	struct wlr_texture *mask, float mask_ox, float mask_oy,
	float mask_sx, float mask_sy, float mask_min,
	struct wlr_texture *shadow_tex,
	int shadow_off_x, int shadow_off_y, int shadow_w, int shadow_h,
	float shadow_dx, float shadow_dy, float shadow_radius,
	const float shadow_color[4], bool shadow_on);

/* eff = 这个目标的规则作用（rule.c 的 vidow_rule_eff，NULL = 没规则）。
 * fullscreen = 这是不是一块全屏窗口：全屏不画服务端装饰（边框 / 圆角 / 阴影 /
 * 噪点全归零，见 rule.c 的 vidow_view_look）。layer 表面一律传 false。 */
bool vidow_fx_frame(struct vidow_server *server, struct vidow_fx_target *fx,
		struct wlr_scene_buffer *src, const struct vidow_fx_geo *geo,
		int x, int y, int ox, int oy,
		double alpha, bool focused, bool fullscreen, double scale, double ov,
		const struct vidow_rule_eff *eff);
/* 窗口抓帧那条路的装饰输入（wincapture.c 填，见 fx.c 的 vidow_fx_capture_decor）。
 * 全部是「这一帧的取景框」口径：渲染目标 = 取景框，内容是已经渲染好的那张窗口
 * 贴图摆在框里的某个位置。 */
struct vidow_capture_decor {
	/* 渲染目标 = 这一帧的取景框 */
	struct wlr_buffer *dst;
	int dst_w, dst_h;
	/* 可见窗口（= 画框贴着的那块）在取景框里的位置 / 尺寸，目标像素 */
	int content_x, content_y, content_w, content_h;
	/* 客户端内容贴图：**不画进这张图**（内容由场景里那个镜像节点画，见
	 * wincapture.c），只用来决定「背后的模糊画在哪儿」（装饰着色器的透明度
	 * 剔除按元素自己的 alpha 走）。tex_* 是它在装饰矩形里的采样映射，跟屏幕上
	 * 那条路的 u_tex_map 同一套（CSD 留白靠它偏掉）。 */
	struct wlr_texture *content;
	float tex_u0, tex_v0, tex_du, tex_dv;
	/* 可见窗口左上角的**布局逻辑坐标** —— 底衬采样靠它换算到那块屏的设备像素 */
	int content_layout_x, content_layout_y;
	/* 取景框的缩放：布局逻辑 → 目标像素 */
	double frame_scale;
	/* 模糊底衬：贴图 + 它盖住输出的哪一块（设备像素，w <= 0 = 整屏），以及它
	 * 代表的那块屏（设备像素尺寸 + 布局原点 + 缩放）。blur 为 NULL = 没底衬
	 * （模糊关着），这一趟就只画边框 / 圆角（跟屏上一样）。 */
	struct wlr_texture *blur;
	int blur_x, blur_y, blur_w, blur_h;
	int out_x, out_y, out_w, out_h;
	double out_scale;
	/* 这一趟画哪一层（见 wincapture.c：模糊在内容**下面**、画框在内容**上面**，
	 * 中间夹着窗口内容那个场景节点）：
	 *   edge = false：背后的模糊底衬。调用方给的 look 已经把边框 / 阴影 / 噪点
	 *                归零（重复画一遍阴影会变浓）。
	 *   edge = true ：圆角 / 边框 / 投影 / 颗粒，不采样模糊。 */
	bool edge;
	/* 画框参数（跟屏上同一份）与边框颜色（直通 alpha，焦点 / 非焦点由调用方选） */
	const struct vidow_look *look;
	const float *border;
};
bool vidow_fx_capture_decor(struct vidow_server *server,
		const struct vidow_capture_decor *c);
/* 客户端缓冲 → 贴图（窗口抓帧那条路要拿它当元素内容贴图；owned 非空时告诉
 * 调用方要不要自己销毁）。 */
struct wlr_texture *vidow_fx_client_texture(struct vidow_server *server,
		struct wlr_buffer *buffer, struct wlr_scene_buffer *scene_buffer,
		bool *owned);
void vidow_fx_end(struct vidow_server *server, struct vidow_fx_target *fx);
void vidow_fx_release(struct vidow_fx_target *fx);
/* 这个窗口这一帧能不能「直通」：装饰 / 模糊 / 动画 / 裁剪一个都不需要，屏幕上
 * 该显示的像素就是客户端缓冲自己（全屏 + 装饰参数全零）。判据宁严不宽 —— 漏判
 * 只是没省下那两笔 GPU，误判会让画面变样（CSD 留白、半透明、模糊底衬都会在直通
 * 里露出来）。见 view.c 的 view_scanout_switch。 */
bool vidow_view_blur_wanted(struct vidow_view *view);
bool vidow_view_scanout_ok(struct vidow_view *view);
void vidow_fx_flush(struct vidow_server *server);
bool vidow_fx_set_shader(struct vidow_server *server,
		struct vidow_fx_target *fx, const char *name,
		enum vidow_shader_phase phase);
int vidow_fx_pad(struct vidow_server *server);
/* layer surface 背后的模糊：位置/尺寸变了要作废重画（见 fx.c）。 */
void vidow_layer_blur_invalidate(struct vidow_layer_surface *ls);
void vidow_layer_blur_release(struct vidow_layer_surface *ls);
/* 弹出物（xdg_popup / X11 菜单 / 输入法候选框）背后的模糊（fx.c）。
 * owner_destroy 非空时挂在那个信号上自己摘掉；destroy_node 告诉卸载时
 * 场景节点归不归我们销毁（xdg 的树归 wlroots，X11 / 候选框归我们）。 */
struct vidow_popup_blur *vidow_popup_blur_register(struct vidow_server *server,
	struct wlr_scene_tree *tree, struct wlr_scene_node *hide,
	struct wlr_surface *surface, struct vidow_view *view, const char *tag,
	struct wl_signal *owner_destroy);
void vidow_popup_blur_unregister(struct vidow_popup_blur *pb,
	bool destroy_node);
void vidow_popup_blur_invalidate(struct vidow_popup_blur *pb);
void vidow_blur_update(struct vidow_output *output);
/* Mark the cached backdrops stale. layers = true for a change to the wallpaper
 * or a background/bottom layer surface; false for anything that only moves or
 * repaints windows. */
void vidow_backdrop_damage(struct vidow_server *server, bool layers);
void vidow_backdrop_damage_output(struct vidow_output *output);
/* 只记一笔损伤区域（布局逻辑坐标，同 pending_damage 的说明），不动世界代次 ——
 * 给「窗口挪走之后，老位置也得让别人的底衬重抓」这种一个事件两处损伤用。 */
/* 同上，但**只记给窗口，不记给 layer surface**。视口过渡每帧平移整条带子
 * 时用：窗口得重抓（别人的底衬里画着它），layer 不跟着视口走、不用管 ——
 * 一并记给 layer 会把 noctalia 的模糊底衬压垮。 */
void vidow_backdrop_damage_box_views_only(struct vidow_server *server,
		const struct wlr_box *box);
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
/* 拖动跟手时该催哪几块屏出帧（drag.c）：窗口跟着光标越过屏边界之后画在**对面
 * 那块屏**上，而它的归属（view->output）要到松手才换 —— 只给归属屏排帧，对面那
 * 块屏不渲染，屏幕上的表现就是「拖过去看不见窗口」。平铺拖拽和浮窗拖拽共用。 */
void vidow_drag_schedule_outputs(struct vidow_server *server,
		struct vidow_view *view, const char *why);
void vidow_tile_grab_end(struct vidow_server *server);
/* 平铺拖拽的贴边自动滚条带（niri 的 Dnd 手感）：update 里记意图，每帧推进一次。
 * 光标压着工作区左右边缘不动也会一直滚 —— 条带比屏宽时，屏幕外那些落点只有这
 * 样才够得到（用户报的「整个显示器左右有一部分死区」）。 */
void vidow_tile_drag_autoscroll(struct vidow_server *server, double lx);
bool vidow_tile_drag_scroll_step(struct vidow_server *server);

/* 落点预览块（niri 的 insert hint）：建在 layer_toplevels 里，main.c 建完场景层
 * 之后调一次。拖平铺窗口的每个指针事件都会重算一遍落点并摆这块（见 drag.c）。 */
bool vidow_drag_init(struct vidow_server *server);
/* 重算并摆预览块（没有落点 / 没在拖就收起来）。 */
void vidow_insert_hint_update(struct vidow_server *server);
/* 收起来（松手、拖到没屏的地方、暂停拖动）。 */
void vidow_insert_hint_hide(struct vidow_server *server);

/* --------------------------------- 小窗（mini window，S4）：落点区 / 工具条
 *
 * 落点区 = 每块屏右上角那一块（尺寸是 mini_drop_zone_w × mini_drop_zone_h 的
 * 工作区百分比）。**只在拖拽中出现**：光标不在里面时两块矩形都是关着的，所以
 * 「盲拖到角落松手」不会命中（那样会跟现有的平铺落点撞车）。
 * 松手时命中 → 那个窗口变成迷你小窗（vidow_mini_set_state(view, MINI)）。 */

/* 算某块屏的落点区（zone）和松手会得到的迷你小窗预览盒（preview）。
 * 返回 false = 这套东西关着（mini_enabled / mini_drop_zone）或者屏没尺寸。 */
bool vidow_mini_drop_zone_box(struct vidow_server *server,
		struct wlr_output *output, struct wlr_box *zone,
		struct wlr_box *preview);
/* 拖动中每个指针事件调一次：光标在落点区里就亮（并收掉平铺落点预览块 ——
 * 两个预览同时画，用户不知道松手会得到哪个）。 */
void vidow_mini_drop_zone_update(struct vidow_server *server);
/* 收起来（松手 / 拖拽结束）。 */
void vidow_mini_drop_zone_hide(struct vidow_server *server);
/* 松手那一刻光标是不是落在某块屏的落点区里。 */
bool vidow_mini_drop_zone_hit(struct vidow_server *server, double lx, double ly);

/* 悬停工具条（mini_toolbar）：迷你 / 图标态悬停时露出，四个按钮。
 * 按钮 id 见 enum vidow_mini_toolbar_btn。 */
enum vidow_mini_toolbar_btn {
	VIDOW_MINI_BTN_EXPAND = 0,  /* 放大：图标 → 迷你 → 自由 */
	VIDOW_MINI_BTN_ICON,        /* 收成图标 */
	VIDOW_MINI_BTN_RESTORE,     /* 还原成列 */
	VIDOW_MINI_BTN_CLOSE,       /* 关掉 */
	VIDOW_MINI_BTN_COUNT,
};
/* 给某个小窗摆出工具条（重复调幂等）。view == NULL = 收起来。 */
void vidow_mini_toolbar_show(struct vidow_server *server,
		struct vidow_view *view);
void vidow_mini_toolbar_hide(struct vidow_server *server);
/* 命中测试：这个点落在工具条的哪个按钮上（没有 = -1）。工具条不是表面，靠矩形。 */
bool vidow_mini_toolbar_near(struct vidow_server *server, double lx, double ly);

int vidow_mini_toolbar_hit(struct vidow_server *server,
		struct vidow_view *view, double lx, double ly);
/* 点一下某个按钮（工具条自己的动作，见 float.c）。 */
void vidow_mini_toolbar_press(struct vidow_server *server,
		struct vidow_view *view, int button);

/* ---- 小窗（mini window，S5）：图标悬停预览 ----
 * 指针一动就调一次（input.c 的指针分流里）：hit = 光标底下的窗口（可能 NULL）。
 * 它自己管延迟 / 离开 / 宽限三条时间轴，调用方不用知道状态。 */
void vidow_mini_preview_motion(struct vidow_server *server,
		struct vidow_view *hit);
/* 收掉预览（形态变了 / 窗口没了 / 进锁屏）。 */
void vidow_mini_preview_hide(struct vidow_server *server);
/* 这个点是不是落在预览那块上（预览吞点击用 —— 它是只读的，但点下去不能
 * 落到背后的窗口上）。 */
bool vidow_mini_preview_hit(struct vidow_server *server, double lx, double ly);
/* ---- 小窗（S5）图标 / 预览的出帧策略 ----
 * 该窗口这一刻是不是走「快照」那条路（图标态 + force_render 关 + 手里有最后一帧）。 */
bool vidow_mini_uses_snapshot(const struct vidow_view *view);
/* `view->last_buffer` 换了之后同步快照节点（图标 + 预览共用一份；图标态下也会把
 * 快照节点重新摆一次 —— 进快照态时 last_buffer 还是空的那种情况靠它兜住）。 */
void vidow_mini_frames_sync(struct vidow_view *view);
/* 出帧策略（快照 / 活）跟着有效 force_render 翻面时，把摆位接上（幂等）。 */
void vidow_mini_frames_recheck(struct vidow_view *view);
/* 建场景节点（main.c 建完场景层之后调一次，和 vidow_drag_init 一起）。 */
bool vidow_mini_ui_init(struct vidow_server *server);

/* 小窗（S4）边缘判定：这条边**外侧有没有相邻输出** —— 有（跨屏边）→ 拖到那儿
 * 松手是「移到那块屏、保持形态」；没有（内边缘）→ 收成图标。
 * 探针沿边的法线取 1px，并要求与目标屏有**非零区间重叠**（只看「角上相接」
 * 不算相邻边）。返回 NULL = 这条边外侧没有屏。 */
struct wlr_output *vidow_mini_output_across(struct vidow_server *server,
		struct wlr_output *self, enum vidow_mini_edge edge);
/* 光标贴着自己那块屏的哪条边（mini_edge_snap_px 以内）。NONE = 没贴边。 */
/* 离 (bx,by,w,h) 这个盒子最近的那条工作区边（图标态拖拽吸附用）。 */
enum vidow_mini_edge vidow_mini_nearest_edge(struct vidow_server *server,
		struct vidow_view *view, int w, int h, double bx, double by);

enum vidow_mini_edge vidow_mini_edge_under_cursor(struct vidow_server *server,
		struct vidow_view *view);
/* 跨屏边松手：换屏、保持形态（锚点是相对那块屏的工作区记的，天然对得上）。 */
void vidow_mini_move_to_output(struct vidow_server *server,
		struct vidow_view *view, struct wlr_output *to);
/* 形态拖拽松手的落点判定（内边缘 → 图标；跨屏 → 换屏保持形态）。返回 true =
 * 形态 / 归属变了。浮窗拖拽（float.c）和形态拖拽（input.c）共用这一份。 */
bool vidow_mini_drag_finish(struct vidow_server *server,
		struct vidow_view *view);

/* ---- 甩出（mini fling，S5）----
 * 采样环：vidow_pointer_motion 每次都推一格（8 格 ≈ 最近几帧）。 */
void vidow_mini_fling_sample(struct vidow_server *server, double x, double y);
/* 松手速度（布局坐标 px/s）。最近一段采样不够（< 3 点）或最后一点已经过去
 * 超过 80ms（指针停住之后才松手）→ 返回 false = 不算甩。 */
bool vidow_mini_fling_velocity(struct vidow_server *server, double *vx,
		double *vy);
/* 甩出：速度够快就朝最近的那条边飞过去。返回 true = 形态 / 归属已经变了
 * （调用方别再走自己的落点判定）。 */
bool vidow_mini_fling(struct vidow_server *server, struct vidow_view *view,
		double vx, double vy);

/* 平铺窗口的 Mod+右键拖拽：改这一列的宽度（niri 的 interactive resize 那条）。
 * 只有横向 —— 平铺窗口的高度是布局给的（整块可用高），没有「单个窗口的高度」。 */
void vidow_tile_resize_begin(struct vidow_server *server,
		struct vidow_view *view);
void vidow_tile_resize_update(struct vidow_server *server, double lx, double ly);
void vidow_tile_resize_end(struct vidow_server *server);

/* 指针按键（真实输入和测试注入走同一条路，见 input.c）。 */
/* force_mod：假装按着 Mod（测试注入用 —— 拖拽要求按着 Mod，而测试环境里没有
 * 真键盘，见 VIDOW_TEST_ACTION 的 pointer-button 动作）。 */
void vidow_pointer_button(struct vidow_server *server, uint32_t button,
		bool pressed, uint32_t time_msec, bool force_mod);
/* 指针移动（测试注入也走这里）。 */
void vidow_pointer_motion(struct vidow_server *server, uint32_t time_msec);
/* 键盘换焦点之后把光标挪到焦点窗口上（niri 的 warp-mouse-to-focus）。没开这个
 * 开关就什么都不做；光标本来就在窗口里就也不动（center-xy-always 除外）。 */
void vidow_warp_cursor_to_focus(struct vidow_server *server);
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
/* 同上，但要的是**这一块**输出上等着的帧：capture_watchdog 现在逐屏决定推不推，
 * 别的屏不应因为这块屏有人在抓帧就被整屏标脏重画。output 为 NULL 一律 false。 */
bool vidow_screencopy_has_pending_for_output(struct vidow_screencopy *manager,
		struct wlr_output *output);

/* 渲染一帧抓帧图（遮挡压好、渲染、放开都在里面），返回**加锁的**缓冲，调用方
 * 负责 wlr_buffer_unlock()。渲染不出来返回 NULL。 */
struct wlr_buffer *vidow_capture_grab(struct vidow_server *server,
		struct wlr_output *output);
/* 抓这块屏上的任意一个矩形（box 是**布局坐标**，scale 是「逻辑 → 像素」，出图
 * 尺寸 = round(w*scale) × round(h*scale)）。按区域录屏 / 按区域截图用它。 */
struct wlr_buffer *vidow_capture_grab_box(struct vidow_server *server,
		struct wlr_output *output, const struct wlr_box *box, double scale);

/* 抓帧的通用入口（上面两条都是它的特例）：box 为 NULL = 整块输出。
 *
 * target 非空 = **录屏 dmabuf 直通**：渲染进调用方那张 linear 交换链，出图能直接
 * 当 dmabuf 交给 PipeWire 消费者（不落 CPU，见 screencast.c）。线性是必须的 ——
 * 不报修饰符时消费者的默认假设就是 linear。尺寸变了会重建（*target 由调用方
 * 持有、负责 destroy）。target 为空 = 抓帧自己的交换链（老行为）。 */
struct wlr_buffer *vidow_capture_grab_into(struct vidow_server *server,
		struct wlr_output *output, const struct wlr_box *box, double scale,
		struct wlr_swapchain *target);
/* 建一张 linear 交换链当出图目标（录屏 dmabuf 直通用的那一种）。建不出来
 * （pixman / 没有 dmabuf）返回 NULL —— 调用方退回老行为（回读）。 */
struct wlr_swapchain *vidow_grab_target_new(struct vidow_server *server,
		int width, int height);
/* 等这一帧的 GPU 真的画完（把命令流冲干净）。直通那条路把缓冲内存直接交给消费者
 * 时必须调：消费者（gst / 浏览器）常常是 mmap 下来在 CPU 上读，不等就是读到还没画
 * 进去的那张（实测全白）。回读那条路不用调（glReadPixels 本身就是同步点）。 */
void vidow_grab_sync(struct wlr_renderer *renderer, struct wlr_buffer *buffer);

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
	/* 指针动了之后催上一帧的时刻（CLOCK_MONOTONIC 纳秒，0 = 还没催过）：
	 * 光标不在画面里（metadata / embedded）时，光标的位置 / 位图就是这一帧的
	 * 内容 —— 指针移动本身也得推帧（见 vidow_screencast_cursor_moved）。 */
	int64_t cursor_push_ns;
	/* 其中多少帧是「指针动了」催出来的（见 vidow_screencast_cursor_moved）。 */
	uint64_t cursor_pushes;
	bool closed;
	struct wl_listener output_destroy;
	struct vidow_sc_pw_stream *pw;
};

/* 一路录屏流的「发帧节奏」诊断数（screencast.c 里那份 PipeWire 侧私有状态对
 * 外露出的几个数；IPC 的 state.screencast 用它）。 */
struct vidow_sc_stream_stats {
	bool has_pw;               /* PipeWire 流建起来了吗 */
	double consumer_fps;       /* 消费者协商要的帧率，0 = 没指定（不限） */
	double cap_ms;             /* 发帧节奏上限（毫秒），0 = 不设上限 */
	unsigned long long frames; /* 真的发出去多少帧 */
	unsigned long long grabs;  /* 整帧抓了多少次（整块离屏渲染 + 回读） */
	unsigned long long commits;/* 看见多少次输出提交 */
	unsigned long long throttled; /* 消费者来要帧但被节奏上限挡下的次数 */
	/* 走 dmabuf 直通（把抓帧缓冲的 fd 直接交给消费者，一帧不落 CPU）发出去的
	 * 帧数。> 0 说明零拷贝那条路是活的；pixman 那种没有 dmabuf 的会话里它恒为
	 * 0（全走回读），正常。 */
	unsigned long long dmabuf_frames;
	/* 一帧的主线程成本（毫秒，平均值）：grab = 整块离屏渲染，readback = 从
	 * 抓帧缓冲回读到 PipeWire 的 memfd。录制时桌面卡不卡就看这两个数 ——
	 * 实测 2560x1440 上回读 6.2ms、渲染 1.4ms，拷贝是渲染的 4 倍多。 */
	double grab_ms;
	double grab_ms_max;
	double readback_ms;
	/* dmabuf 直通每帧在等 GPU 画完的那笔时间（毫秒，平均值）。走直通时它是
	 * 主线程上唯一剩下的大头（回读那笔变成 0）：实测就是「一帧渲染 + 冲命令
	 * 流」的时间。 */
	double sync_ms;
	long long last_send_ms;    /* 上一帧是什么时候发的（单调时钟毫秒），-1 = 还没发过 */
};
bool vidow_screencast_stream_stats(const struct vidow_screencast_stream *stream,
		struct vidow_sc_stream_stats *out);

/* 建 zkde_screencast_unstable_v1。返回 NULL = 没建成（不致命：录屏用不了，
 * 别的照常）。 */
struct vidow_screencast *vidow_screencast_create(struct wl_display *display,
		struct vidow_server *server);
/* 退出前拆掉 zkde_screencast global、PipeWire 状态和剩下的流。 */
void vidow_screencast_finish(struct vidow_server *server);
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

/* 指针动了：光标不在画面里的那几路（metadata / embedded）得跟着推一帧 —— 那种
 * 流的「这一帧长什么样」包含光标位置 / 位图，指针移动本身就是内容变了。只在真的
 * 有这种流的时候做（没有就是一次链表遍历）。 */
void vidow_screencast_cursor_moved(struct vidow_server *server);

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
/* wp_cursor_shape_manager_v1：建全局 + 监听客户端的 set_shape。 */
void vidow_cursor_shape_init(struct vidow_server *server);
/* 客户端通过 cursor-shape-v1 换了主题光标（layer / GTK 菜单等）。 */
void vidow_cursor_fx_set_cursor_name(struct vidow_server *server,
		const char *name);
/* 退出前收掉放大图 / 监听器 / 定时器和状态本身。 */
void vidow_cursor_fx_finish(struct vidow_server *server);
/* 每次指针移动后调（真实鼠标、测试注入、IPC 注入都走这里）。 */
void vidow_cursor_fx_motion(struct vidow_server *server);
/* 手动放大一次（IPC 的 cursor-shake）。why 里是拒绝的原因。 */
bool vidow_cursor_fx_shake(struct vidow_server *server, const char **why);
/* 光标主题 / 尺寸：按 config（→ 环境变量 → gsettings → 缺省）重建 cursor
 * manager 并重新加载到每块输出。启动调一次；改配置 / vidowmsg cursor-size
 * 时再调 —— 以前只在启动读一次，运行中改不动（用户报「光标大小改不了」）。 */
void vidow_cursor_apply(struct vidow_server *server);

bool vidow_cursor_set_client_surface(struct vidow_server *server,
		struct wlr_surface *surface, int32_t hx, int32_t hy);
/* 客户端换了光标图（request_set_cursor）：记下来，好在放大结束时还原。 */
/* cursor manager 重建后（主题 / 尺寸热更新）重设当前光标图。 */
void vidow_cursor_fx_reapply(struct vidow_server *server);
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
/* 客户端画完第一帧了吗？xdg 的 configure 只能发给 initialized 的 surface。
 *
 * **X11 不要求 mapped**：X 客户端的生命周期是「XMapWindow → 第一次绘制的
 * Commit → wayland surface 才 map」，而 vidow 的 view_map 正是在那次 Commit
 * 里跑的。view_map 第一行就调 vidow_rule_apply()，规则里的 fullscreen /
 * maximize 要在这里发出去 —— 那一刻 surface->mapped 可能还是 false（wlroots
 * 先发 commit 信号、之后才置 mapped），要求它等于把 X11 的全屏规则**永远
 * 丢掉**：view_apply_fullscreen 直接 return，之后再没人补发。
 *
 * 实测症状：米哈游启动器（steam_app_default，WM_NORMAL_HINTS min=max）配了
 * fullscreen 规则，窗口建出来闪一下就没了，主窗口从头到尾 IsUnMapped；同一份
 * 配置在 Hyprland 上正常（Hyprland 用 fullscreen_state 请求式下发，不看 map）。
 *
 * X11 只要有 surface 就能发 _NET_WM_STATE / configure —— 那只是 X 属性，不挑时机。
 *
 * **2026-10-26：X11 分支不再要求 surface。** 上面那段「米哈游启动器」的结论
 * 只改了「不要求 mapped」，还留着 `view->x11->surface != NULL` —— 那条同样是
 * 死锁的一半，Steam 主窗口就是这么卡死的：
 *
 *   Steam 关掉再打开 → 它建好 3808x2050 的顶层窗口，然后**等一个 configure**
 *   才 XMapWindow（实测 xwininfo: Map State: IsUnMapped，WM_STATE 都没有）
 *   → 不 map，xwayland 就不建 wayland surface
 *   → 这里的 surface != NULL 为假，configure 永远不发
 *   → Steam 永远等不到，画面永远是黑的
 *
 * 实测钉死的三点（2026-10-26 会话）：
 *   ① xwininfo -id 0x2000041 → `Map State: IsUnMapped`、`WM_STATE: not found`
 *   ② 日志里 xwm 只有 `create xid=0x2000041 3808x2050`，**没有一条 MapRequest**
 *   ③ 手动 `xdotool windowmap 0x2000041` → Vidow 立刻
 *      `associate xid=0x2000041 serial=2 surface=0x...`，画面正常（截图确认）
 *
 * 而 vidow_x11_surface_configure() 通篇只用 s->window_id 发 xcb_configure_window
 * + 合成 ConfigureNotify，**一个字节的 surface 都不碰**。所以那条判据纯属多余。
 *
 * 判据改成「窗口还在（s 非 NULL）」：X 侧的 configure 是发给 X 窗口的，只要
 * 那个窗口还没销毁就该发。 */
static inline bool vidow_view_configure_ready(const struct vidow_view *view) {
	if (view->kind == VIDOW_VIEW_X11) {
		return view->x11 != NULL &&
			view->x11->window_id != XCB_WINDOW_NONE;
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
/* 退出全屏（最小化 / 变浮窗这类「整屏状态跟它冲突」的操作先走这一步）。why 进日志。 */
void vidow_view_unfullscreen(struct vidow_view *view, const char *why);
/* 每帧对一遍 X11 的 _NET_WM_STATE（退出全屏但不再交帧的游戏，见 view.c）。 */
void vidow_views_sync_x11_fullscreen(struct vidow_server *server);
void vidow_view_set_tiled(struct vidow_view *view, bool tiled);
void vidow_view_close(struct vidow_view *view);
/* 把当前标题 / app_id 记一份（事件用，见 vidow_view.last_title）。 */
void vidow_view_note_names(struct vidow_view *view);
/* 窗口规则的 initial_* 快照（只填空着的，见 vidow_view.init_title）。 */
void vidow_view_note_initial_names(struct vidow_view *view);
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
		double scale, struct vidow_ease easing, double anim_scale_to,
		const struct vidow_rule_eff *eff, bool fullscreen);

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
		int out_w, int out_h, struct wlr_swapchain *target);
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

/* 一块**没在显示**的工作区的条带长什么样：每个窗口摆在哪（工作区局部坐标，
 * 不含视口滚动）+ 条带总宽。overview 拿它画缩略图 —— 藏起来的工作区从来没被
 * arrange 过，col->offset / view->geom 还是上次显示时的老值，不能直接用。
 * 规矩和 output_arrange 的 Pass 1 / Pass 3 是同一套（列宽按比例、整列最小化的
 * 不占位置、全屏占一整屏、同列上下平分可用高），只是**不写进 view->geom**。
 * cb 为 NULL 时只算条带宽度。返回条带总宽。 */
int vidow_workspace_preview_layout(struct vidow_server *server,
		struct vidow_workspace *ws, int work_w, int work_h,
		void (*cb)(struct vidow_view *view, int x, int y, int w, int h,
			void *data),
		void *data);

/* ------------------------------------------------------------- overview */

bool vidow_overview_init(struct vidow_server *server);
bool vidow_overview_active(const struct vidow_server *server);
void vidow_overview_toggle(struct vidow_server *server);
void vidow_overview_exit(struct vidow_server *server);
/* 图层规则的 overview 改了之后，把逐 surface 的「全景里露不露」重算一遍
 * （rule.c 的 apply_layer 调）—— 不然要等下次开关全景才生效。 */
void vidow_overview_refresh_layers(struct vidow_server *server);
/* 每帧渲染前调一次：算好变换（t 变了、布局变了都靠它）并把窗口摆一遍。 */
void vidow_overview_update(struct vidow_server *server);
/* 工作区局部坐标 -> 屏幕坐标。ox/oy 是窗口自己的动画偏移（开窗 / 关窗补间，
 * 布局像素），跟着一起缩。t = 0 时是恒等变换。 */
void vidow_overview_map(struct vidow_server *server, struct vidow_view *view,
		double ox, double oy, double *sx, double *sy);
double vidow_overview_scale(const struct vidow_server *server);

/* 这个窗口在 overview 里画出来的框 + 它那一行的矩形（都布局坐标）。给「整块
 * 落在行矩形外就不画」这条判据用。返回 false = 不适用。 */
bool vidow_overview_view_box(struct vidow_server *server,
		struct vidow_view *view, struct wlr_box *out);
/* 点一下：选中命中的那列（返回 true = 这次点击被 overview 吃掉了）。 */
bool vidow_overview_click(struct vidow_server *server, double sx, double sy);
/* 滚轮：滚的是工作区列表（上下预览），不是窗口。`steps` = 走几**行**（一格 = 一块
 * 工作区，落点吸附到行距的整数倍；正数往下、负数往上，到顶 / 到底就停）。 */
void vidow_overview_scroll(struct vidow_server *server, int steps);

/* -------------------------------------------------- MRU（Alt+Tab 切换器） */

/* 台账（谁最近用过）和切换器都在 mru.c。 */
int64_t vidow_mru_now_ns(void);
bool vidow_mru_init(struct vidow_server *server);
/* 退出前收掉卡片缓冲 / 场景树 / 定时器和两个数组。 */
void vidow_mru_finish(struct vidow_server *server);
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
/* 面板显形了（open_delay 到了）：切换器那层浮在画面上（窗口本身一动没动）。 */
bool vidow_mru_showing(const struct vidow_server *server);
/* 打开（dir > 0 = 往后走，对应 Alt+Tab；< 0 = 往前，Alt+Shift+Tab）。
 * 已经开着的时候再调 = 走一格（按住连点 Tab 走的就是这条）。 */
void vidow_mru_open(struct vidow_server *server, int dir, const char *app_id,
		enum vidow_mru_scope scope);
void vidow_mru_step(struct vidow_server *server, int dir);
/* 滚轮：竖直轴 = 选中项换个格（**到边停住，不循环** —— 和 vidow_mru_step 的
 * 绕回故意不一样，别当 bug 改回去，见 mru.c）；横向轴吃掉但不动选中项。
 * 返回是否消费这次滚轮（切换器没开 = false；开着 = 连空滚一格都吃掉）。 */
bool vidow_mru_wheel(struct vidow_server *server,
		enum wl_pointer_axis orientation, double amount);
/* 方向键：和滚轮同一套 clamp 步进（到边停住，不循环）。 */
void vidow_mru_step_clamp(struct vidow_server *server, int dir);
/* 鼠标在面板上动：命中的那一项点亮（返回 true = 确实换了高亮项，画面要重画）。
 * 只改样式，不动选中 —— 选中的那个还是键盘在走。 */
bool vidow_mru_hover(struct vidow_server *server, double sx, double sy);
/* 鼠标点一下：命中的那一项直接切过去（点空白 = 什么都不做）。返回 true = 这次
 * 点击被切换器吃掉了。 */
bool vidow_mru_click(struct vidow_server *server, double sx, double sy);
/* 松开修饰键：焦点给选中的那个，关掉。 */
void vidow_mru_commit(struct vidow_server *server);
/* Esc：关掉，焦点原地不动。 */
void vidow_mru_cancel(struct vidow_server *server);
/* 配置 / 屏幕变了：重排一遍（没显示就什么都不做）。 */
void vidow_mru_update(struct vidow_server *server);

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

/* ------------------------------------------------------------- 小窗（S1） */

/* 换形态。NONE = 还原成列（等价于 vidow_view_join_layout + 重新记账）。
 * 只动数据模型与几何（比例锚点 -> float_geom），不碰可见性 / 焦点 / 输入 /
 * 渲染 / 动画 —— 那些是 S2–S5。 */
void vidow_mini_set_state(struct vidow_view *view, enum vidow_mini_state state);
/* S2：这个窗口要进 MINI / ICON 了 —— 把键盘 / 指针焦点交出去（迷你 / 图标不接
 * 键盘）。必须在摘列 / 摘工作区**之前**调（要还在列里才找得到焦点邻居）。 */
void vidow_view_yield_focus_for_mini(struct vidow_view *view);
/* 按比例锚点算出 float_geom（幂等，不 configure 客户端）。摆放前调。 */
void vidow_mini_resolve_geom(struct vidow_view *view);

/* 拖拽中把迷你 / 图标 / 自由小窗的归属换到 to 那块屏上，并按新屏的工作区重算
 * 比例锚点（抓取偏移保持不变）。 */
void vidow_mini_drag_to_output(struct vidow_server *server,
		struct vidow_view *view, struct wlr_output *to, double lx, double ly);

/* 把一个绝对位置（盒子左上角）换算成小窗的比例锚点，并按 allow_over 夹取：
 * MINI 传 false（夹在工作区里），FREE / ICON 传 true（允许出屏一点）。 */
void vidow_mini_anchor_from(struct vidow_view *view, struct wlr_box work,
		int w, int h, double bx, double by, bool allow_over);
/* S5（修「退出小窗那一瞬间显示原始大小」）：这一帧内容画出来的缩放 / 内容原点
 * 相对盒子原点的居中偏移。补间的起点必须取这两个量（「上一帧画在哪」），
 * 不能从盒子面积推（见 float.c 的 mini_transition_start）。 */
double vidow_view_mini_scale_export(const struct vidow_view *view);
void vidow_view_mini_content_offset(const struct vidow_view *view, int *dx,
		int *dy);
/* S3：这个窗口现在要不要**保留客户端原生分辨率**（小窗态不 configure 客户端、
 * 由合成器把场景节点缩到小窗盒显示）。规则写了就按规则的，没写按全局键。 */
bool vidow_mini_preserve_buffer(const struct vidow_view *view);
/* "none" / "free" / "mini" / "icon" <- 文本（IPC / 日志）。返回 NULL = 不认识。 */
bool vidow_mini_parse_state(const char *text, enum vidow_mini_state *out);
const char *vidow_mini_state_name(enum vidow_mini_state state);
/* "auto" / "lock" / "free" <- 文本。返回 NULL = 不认识。 */
const char *vidow_mini_aspect_name(enum vidow_mini_aspect aspect);
bool vidow_mini_parse_aspect(const char *text, enum vidow_mini_aspect *out);
/* S3b：宽高比的**有效值** —— 键位（运行时）> 窗口规则 > 全局键，三级都是显式
 * 选择（auto 也是「我选择按自动信号走」）。判策略只有这一个出口，别散在各处。 */
enum vidow_mini_aspect vidow_mini_aspect_effective(const struct vidow_view *view);
/* S3b：这个窗口现在锁不锁宽高比；锁的话把比值（w/h）写到 *ratio。只有自由小窗
 * （FREE）看它 —— 迷你 / 图标态永远按内容比例显示（contain），不给逃生口。 */
bool vidow_mini_aspect_lock(const struct vidow_view *view, double *ratio);
/* S3b：客户端的比例信号变了（X11 后来才设 PAspect）时的重判点。 */
void vidow_mini_aspect_changed(struct vidow_view *view);
/* S3b：X11 客户端声明的宽高比（WM_NORMAL_HINTS 的 PAspect，num/den = w/h）。
 * 只设了一边（区间）不算。Wayland 协议没有对应物 —— 那边恒返回 false。 */
bool vidow_view_aspect_hint(const struct vidow_view *view, int *num, int *den);
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
/* 全屏窗口亮着的时候，把这块屏上「占着 exclusive zone 的 top 层」（状态栏）
 * 藏掉：不然它看不见、却还挡着一条输入死区。退出全屏再放回来。 */
void vidow_layers_update_fullscreen(struct vidow_server *server);
/* 桌面底色（background_color）落到场景里：颜色 + 整个布局的包围盒。配置重载、
 * 输出变化、启动时各调一次（幂等）。 */
void vidow_background_apply(struct vidow_server *server);
bool vidow_layer_focus_surface(struct vidow_server *server,
		struct wlr_surface *surface);

/* ------------------------------------------------------------------- views */

struct vidow_view *vidow_view_at(struct vidow_server *server,
		double lx, double ly, struct wlr_surface **surface,
		double *sx, double *sy);
void vidow_focus_view(struct vidow_view *view);
/* 把窗口放上屏 / 收下来。原来是 view.c 里的 static，xwm.c 的状态机要调，
 * 于是导出 —— 参数照旧是那两个监听器的形状（监听器指针 + 数据），这样
 * 调用方不用知道内部是怎么取 view 的，也免得再造一套平行的入口。 */
void view_map(struct wl_listener *listener, void *data);
void view_unmap(struct wl_listener *listener, void *data);
/* 「跳到这个窗口」的入口（MRU 切换器提交 / IPC 的 focus-window）：目标是迷你 /
 * 图标态的话，先把它摊成自由小窗（FREE）**再**给焦点 —— 顺序不能反，
 * vidow_focus_view 对 MINI / ICON 有守卫（不变量：server->focused 永不指向
 * MINI / ICON）。FREE 态与普通窗口照旧。 */
void vidow_focus_view_expand_mini(struct vidow_view *view);
/* 「聚焦这个窗口会让条带滚多少像素」—— 和 arrange 的 Pass 2（把焦点列滚回可见
 * 区）算的是同一件事。focus-follows-mouse 的 max-scroll-amount 拿它跟阈值比
 * （见 input.c 的 focus_follows_mouse）。 */
double vidow_view_scroll_amount_to_activate(struct vidow_server *server,
		struct vidow_view *view);
/* 层级收口：平铺 < 浮动 < 全屏（浮动永远压在平铺之上，浮窗之间靠点击换层级）。
 * top 非空 = 「这一个刚被抬起来」，它落在自己那一组的最上面；没人站错队时一个
 * 节点都不碰（见 src/view.c）。 */
void vidow_restack(struct vidow_server *server, struct vidow_view *top);
/* 这块窗口**属于**哪块屏：平铺 / 全屏的看它那条列挂在哪块屏（跟滚动视口无关，
 * 稳定）；浮动 / 弹窗返回 NULL（它们不参与滚动，按「现在画在哪块屏上」算）。
 * 平铺窗口滚出视口之后「画在哪」会翻到隔壁那块屏去，所以倍率 / 模糊底衬 / 按窗口
 * 录屏的取景框都得走这一个判据，见 src/view.c。 */
struct wlr_output *vidow_view_owner_output(const struct vidow_view *view);
/* 这块窗口**算在哪块屏上**：平铺 / 全屏看归属那块（vidow_view_owner_output），
 * 浮动 / 弹窗看它看得见的那些屏里缩放最大的那块（跨屏拖动时布局归属还没换，人已经
 * 在对面那块屏上了）。装饰离屏缓冲的倍率和私有模糊底衬归哪块屏都走这一个判据。
 * 看不见任何屏时返回 NULL。 */
struct wlr_output *vidow_view_display_output(const struct vidow_view *view);
/* 这块窗口**算在哪块屏上**（窗口 -> 工作区 -> 屏；拖动中也不破例 —— 窗口还没换
 * 工作区就还在原来那块屏上）。装饰离屏缓冲的倍率（view.c 的 view_scale）和私有
 * 模糊底衬归哪块屏（fx.c 的 view_blur_owner）**必须用这一个**：底衬的尺寸/原点
 * 是按这块屏的设备像素记的，判据不一致就是跨屏拖拽时那条一条的横道。 */
/* 「把这个窗口端到人面前」：先让它那块工作区亮起来（在别的屏上就把焦点挪过去 /
 * 整块工作区搬过来），再给键盘焦点。xdg-activation（点通知）、任务栏按钮、
 * X11 的 _NET_ACTIVE_WINDOW 都走这里 —— 光调 vidow_focus_view 的话，窗口要是
 * 在藏起来的工作区上，焦点就落在一个看不见的窗口上（看起来像「点了没反应」）。 */
void vidow_view_activate(struct vidow_view *view);
void vidow_focus_column(struct vidow_server *server, int direction);

/* 拖 CSD 标题栏 = 平移视图（窗口不动）：复用 output->viewport_x 那个视口。 */
void vidow_pan_begin(struct vidow_server *server, struct vidow_view *view);
void vidow_pan_motion(struct vidow_server *server);
void vidow_pan_end(struct vidow_server *server);
void vidow_focus_window(struct vidow_server *server, int direction);
void vidow_move_column(struct vidow_server *server, int direction);
/* 和相邻列里同层的那个窗口换个位置（swap-window 动作）。 */
bool vidow_swap_window(struct vidow_server *server, int direction);
void vidow_move_window(struct vidow_server *server, int direction);
/* 把一条窗口插进**另一条列**的第 index 位（niri 的 insert into column：一列里
 * 上下叠起来）。index 是「目标列现在这些窗口（不含自己）里的第几位」，越界就放
 * 最后。同一条列 = 就地换位置；别的列 = 从原来那条列里摘出来（列空了就拆掉）。
 * 跨工作区/跨屏那条路由 vidow_workspace_move_view_into_column() 兜。 */
bool vidow_view_insert_into_column(struct vidow_view *view,
		struct vidow_column *target, int index);
/* 从自己那条列里摘出来（列里没别人了就把它拆掉）。摘出来的窗口会短暂「没有列」，
 * 调用方得紧接着把它放进哪条列里（见 vidow_view_insert_into_column）。 */
void vidow_view_leave_column(struct vidow_view *view);
/* 把窗口从它那条列里拎出来、单独成一条新列（niri 的 expel-window-from-column），
 * 插在 ws 的列带里 `at` 那个位置之前（NULL = 原来那条列的右边）。列里只剩它一个
 * （或者它是浮窗 / 弹出物）返回 NULL。拖拽落点那条路也用它（见 drag.c）。 */
struct vidow_column *vidow_view_expel_new_column(struct vidow_view *view,
		struct vidow_workspace *ws, struct wl_list *at, const char *reason);
/* 动作 / IPC 那条路：direction > 0 = 新列插在原来那条列右边，< 0 = 左边。
 * 取不出来（浮窗 / 列里就它一个）返回 false。 */
bool vidow_view_expel_from_column(struct vidow_view *view, int direction);
void vidow_cycle_column_width(struct vidow_server *server);
/* niri 式列最大化：整列撑满工作区宽度，maximized_saved 记着原来的比例，关掉就还原。
 * 列里每个客户端都会收到 set_maximized 的应答（它们自己也知道自己最大化了）。
 * 状态入口只有这一个 —— 别在别处直接改 col->maximized。 */
void vidow_column_set_maximized(struct vidow_column *col, bool maximized);
/* 改列宽（比例）。animate = true 时按 config.column_width_ms 补间过去（列宽过渡），
 * false 直接到位。**所有改 width_fraction 的地方都得走这儿**：arrange 用的是
 * col->width_shown（补间中间值），绕过它直接写 width_fraction 会让记录的宽度和
 * 画出来的宽度不一致。唯一的例外是刚建出来的列（那时两者都是 0，建完立刻赋值）。 */
void vidow_column_set_width(struct vidow_column *col, double fraction, bool animate);
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
/* xdg-output 由自己实现（不用 wlr_xdg_output_manager_v1）：只为 Xwayland 客户端
 * 把 logical_size 报成设备像素。KWin / Hyprland 都走这条路 —— 它只改几何协商，
 * 不会像 _XWAYLAND_GLOBAL_OUTPUT_SCALE 那样顺带把 X surface 的 buffer scale
 * 变成 S（那会让 1x1 的辅助窗口撞上 wlroots 的 "not divisible by scale" 校验）。
 * 见 src/xdgoutput.c 的长注释。 */
struct vidow_xdo_manager *vidow_xdg_output_manager_create(
	struct wl_display *display, struct wlr_output_layout *layout,
	struct vidow_server *server);
/* Xwayland 起来 / 退出时告诉 xdg-output 哪个 wl_client 是它，好切尺寸。 */
void vidow_xdg_output_set_xwayland_client(struct wl_client *client);
/* X 专用虚拟矩形的原点（兼容接口，找不到返回 0,0）。
 * 完整的双轴仿射位置/尺寸换算使用 vidow_xwm_* API，不直接除输出 scale。 */
void vidow_xdg_output_dev_origin(struct wlr_output *output, int *x, int *y);
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
void vidow_view_new_x11(struct vidow_x11_surface *xsurface);
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
void vidow_portal_view_output(struct vidow_view *view);
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

/* ---------------------------------------------------------------- 退出来源 */

/* 把「这一次会话是怎么结束的」写进 $XDG_STATE_HOME/vidow/exit.log。
 *
 * 三条退出入口（SIGTERM / 键位 quit / IPC quit）各自调一次，写清来源。起因是
 * 「Ctrl+C 之后整个桌面没了」这种报告：SDDM 把会话 stderr 丢进 /dev/null，
 * journalctl 里一个字都查不到，只能靠猜是哪条路径。
 *
 * 没有初始化或写失败时是空操作 —— 记日志不能影响退出本身。 */
void vidow_exit_log(const char *source, const char *detail);

#endif /* VIDOW_SERVER_H */
