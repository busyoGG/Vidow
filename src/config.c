/* 配置系统。
 *
 * 配置文件只有一个，而且是 Lua（script.c 负责跑）：~/.config/vidow/config.lua。
 * 这里管的是另外三件事：
 *
 *   1. 默认值（defaults()）—— 没有配置文件也要能跑；
 *   2. 键表（entries[]）—— 键名 / 类型 / 范围，Lua 那边和 IPC 那边都靠它，
 *      所以配置键不需要在 Lua 里声明一遍，写 `corner_radius = 12` 就生效；
 *   3. 环境变量覆盖（VIDOW_<KEY>）—— 优先级最高，测试靠它。
 *
 * 文件的读取时机见 script.c：要等合成器起得来（输出、seat 都在了）才跑，
 * 所以 `ipc` / `xwayland` 这类「启动时就消费掉」的键，配置落地后由
 * vidow_config_sync() 再对一次账。
 */
#include "server.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <wlr/util/log.h>

enum kind {
	K_INT,
	K_DOUBLE,
	K_BOOL,
	K_EASING,
	K_STRING,
	K_COLOR, /* "RRGGBB" or "RRGGBBAA", straight (non-premultiplied) alpha */
	/* Alt+Tab 切换器的两个枚举（写字符串，存整数；见 mru.c） */
	K_MRU_SCOPE,  /* "all" / "workspace" / "output" */
	K_MRU_LAYOUT, /* "strip" / "grid" */
};

struct entry {
	const char *key;
	enum kind kind;
	size_t offset;
	size_t size;
	int section;      /* 打印默认配置时的分节，见 sections[] */
	const char *doc;  /* 打印在那行配置上面的注释（不能有换行） */
};

/* 配置的「类」。entries[] 按这个顺序排（section 就是这里的下标），一份默认配置
 * 也按这个顺序拆成 configs/ 下的文件：一类一个文件，入口 config.lua 用
 * vidow.include() 把它们读进来。
 *
 * 顺序是有意义的：include 的先后 = 键生效的先后（同一个键只有第一次写的算数），
 * 文件名的数字前缀就是为了让这个顺序在文件管理器里也看得出来。 */
struct section {
	const char *title; /* 文件里的分节标题 */
	const char *file;  /* configs/ 下的文件名 */
	const char *doc;   /* 文件头多一句说明（可为 NULL） */
};

static const struct section sections[] = {
	{ "间距（gap）", "01-gaps.lua", NULL },
	{ "窗口装饰", "02-decor.lua",
		"圆角、边框、阴影、不透明度 —— 这一类的开关是 decor_enabled" },
	{ "背景模糊", "03-blur.lua", NULL },
	{ "开关动画", "04-anim.lua",
		"窗口 / layer 的开关与移动动画，以及拖窗口时的果冻效果（jelly）" },
	{ "自定义着色器", "05-shader.lua", NULL },
	{ "刷新与补帧", "06-render.lua", NULL },
	{ "剪贴板", "07-clipboard.lua", NULL },
	{ "IPC", "08-ipc.lua", NULL },
	{ "缩放与输出", "09-output.lua",
		"缩放是一个键；每块屏的缩放 / 位置 / 模式用 vidow.output{…} 声明" },
	{ "XWayland", "10-xwayland.lua", NULL },
	{ "窗口激活", "11-activation.lua", NULL },
	{ "xdg-desktop-portal", "12-portal.lua", NULL },
	{ "指针", "13-cursor.lua", NULL },
	/* 这一份不是「键」，是 vidow.* 的调用（键位 / 工作区 / 窗口规则）。 */
	{ "键位 / 工作区 / 规则", "14-keys.lua",
		"键位是配置给的（合成器不内置），工作区与窗口规则也在这一份里" },
	/* Alt+Tab 切换器那一族排在它**后面**：文件名只能往后加（15-…），不能往前
	 * 插 —— 14-keys.lua 这个名字早就写进用户的 config.lua 里了，改名等于把老
	 * 配置读崩。 */
	{ "最近窗口（Alt+Tab）", "15-mru.lua",
		"Alt+Tab 的窗口切换器（niri 的 recent windows）：列哪些窗口、怎么排版、"
		"选中框长什么样" },
	/* 会话环境同样是往后加（16-…）：前面那些文件名早就写进用户的 config.lua 了。 */
	{ "会话环境 / 自启动 / 事件钩子", "16-env.lua",
		"和 systemd 用户管理器对账（吸 / 推）、exec_once 自启动、vidow.on() 事件钩子" },
};
#define N_SECTIONS ((int)(sizeof(sections) / sizeof(sections[0])))
/* 「键位 / 工作区 / 规则」那一节：它不是配置键，entries[] 里没有它，打印也
 * 单独一套。注意它已经不是最后一节了（Alt+Tab 那一族排在它后面）。 */
#define SECTION_KEYS 13
/* 缩放与输出那一节多一段 vidow.output{…} 的写法（不是配置键）。 */
#define SECTION_OUTPUT 8
/* 16-env.lua 多一段 vidow.* 的写法（会话环境 / 自启动 / 事件钩子，不是配置键）。 */
#define SECTION_ENV 15

#define OFF(field) offsetof(struct vidow_config, field)
#define SZ(field) sizeof(((struct vidow_config *)0)->field)

static const struct entry entries[] = {
	/* gap：一个统一值管全部 + 四边（和窗口之间）单独盖。以前拆成
	 * workspace_gaps_inner/outer + window_gaps_inner/outer 四个键，还要加
	 * `gaps` 简写一起设 —— 九个键说一件「间距」的事，而且 `gaps = 10` 实际
	 * 得到的是离屏幕边 20px（内外两份相加）。现在只剩一个数加几个覆盖项。 */
	{ "gaps",             K_INT, OFF(gaps),             SZ(gaps), 0,
		"屏幕边到窗口、以及窗口之间的间距（一个数全管）" },
	{ "gaps_top",         K_INT, OFF(gaps_top),         SZ(gaps_top), 0,
		"单设上边距，-1 = 跟随 gaps（其余负值当 0）" },
	{ "gaps_bottom",      K_INT, OFF(gaps_bottom),      SZ(gaps_bottom), 0,
		"单设下边距，-1 = 跟随 gaps（其余负值当 0）" },
	{ "gaps_left",        K_INT, OFF(gaps_left),        SZ(gaps_left), 0,
		"单设左边距，-1 = 跟随 gaps（其余负值当 0）" },
	{ "gaps_right",       K_INT, OFF(gaps_right),       SZ(gaps_right), 0,
		"单设右边距，-1 = 跟随 gaps（其余负值当 0）" },
	{ "gaps_inner",       K_INT, OFF(gaps_inner),       SZ(gaps_inner), 0,
		"单设窗口之间（列与列、同列上下）的间距，-1 = 跟随 gaps（其余负值当 0）" },

	{ "decor_enabled",   K_BOOL,   OFF(decor_enabled),   SZ(decor_enabled), 1,
		"圆角/边框/阴影这套装饰的总开关" },
	{ "prefer_no_csd",   K_BOOL,   OFF(prefer_no_csd),   SZ(prefer_no_csd), 1,
		"让客户端别画自己的装饰（CSD）：开启后连只认 KDE 老协议的客户端"
		"（GTK3）也听得见 —— 客户端把标题栏一起收回去，窗口只剩 vidow 的边框" },
	{ "corner_radius",   K_DOUBLE, OFF(corner_radius),   SZ(corner_radius), 1,
		"圆角半径（逻辑像素，0 = 直角）" },
	{ "border_width",    K_DOUBLE, OFF(border_width),    SZ(border_width), 1,
		"边框宽度（0 = 不画边框）" },
	{ "border_color",    K_COLOR,  OFF(border_color),    SZ(border_color), 1,
		"未聚焦窗口的边框颜色" },
	{ "border_color_focus", K_COLOR, OFF(border_color_focus),
		SZ(border_color_focus), 1, "聚焦窗口的边框颜色" },
	{ "shadow_radius",   K_DOUBLE, OFF(shadow_radius),   SZ(shadow_radius), 1,
		"阴影半径（0 = 不画阴影）" },
	{ "shadow_color",    K_COLOR,  OFF(shadow_color),    SZ(shadow_color), 1,
		"阴影颜色，alpha 管浓淡" },
	{ "shadow_offset_x", K_INT,    OFF(shadow_offset_x), SZ(shadow_offset_x), 1,
		"阴影横向偏移（像素）" },
	{ "shadow_offset_y", K_INT,    OFF(shadow_offset_y), SZ(shadow_offset_y), 1,
		"阴影纵向偏移（像素）" },
	{ "window_opacity",  K_DOUBLE, OFF(window_opacity),  SZ(window_opacity), 1,
		"窗口（含装饰）的不透明度，0.5 = 半透明" },
	{ "noise",           K_DOUBLE, OFF(noise),           SZ(noise), 1,
		"往装饰里掺的噪点强度，0 = 不加" },

	{ "blur_enabled",    K_BOOL,   OFF(blur_enabled),    SZ(blur_enabled), 2,
		"背景模糊总开关" },
	{ "blur_all",        K_BOOL,   OFF(blur_all),        SZ(blur_all), 2,
		"平铺窗口也模糊身后的壁纸；关掉只模糊浮动窗口后面" },
	{ "blur_xray",       K_BOOL,   OFF(blur_xray),       SZ(blur_xray), 2,
		"重叠时也用整屏那份底衬（壁纸 + layer 那层），忽略别的窗口 —— 糊的是壁纸"
		"不是压着的那个窗口（Hyprland 的 blur:xray 同款）。省很多：不建私有底衬，"
		"重叠场景每帧从 N×8 趟降到 ~3 趟、私有底衬的显存全免" },
	{ "blur_levels",     K_INT,    OFF(blur_levels),     SZ(blur_levels), 2,
		"Kawase 模糊的层级数，越大越糊。每多一级 = 每个模糊元素每帧多两趟离屏"
		"渲染（实测 2560x1440 缩放 2、四个模糊元素：3 级 25 趟/帧、6.3% 单核；"
		"2 级 17 趟、5.8%；4 级 33 趟、6.7%），糊的半径约 blur_offset × "
		"(3×2^级数 − 5) 设备像素（offset=1.5 时 3 级 ≈29px、4 级 ≈65px）" },
	{ "blur_strength",   K_DOUBLE, OFF(blur_strength),   SZ(blur_strength), 2,
		"模糊强度倍数" },
	{ "blur_alpha_min",  K_DOUBLE, OFF(blur_alpha_min),  SZ(blur_alpha_min), 2,
		"窗口内容透明度低于这个值的地方不参与模糊（省 GPU）" },
	{ "blur_offset",     K_DOUBLE, OFF(blur_offset),     SZ(blur_offset), 2,
		"模糊采样偏移，越大越散" },

	{ "window_open_ms",  K_INT,    OFF(window_open_ms),  SZ(window_open_ms), 3,
		"窗口打开动画时长（毫秒，0 = 不动画）" },
	{ "window_close_ms", K_INT,    OFF(window_close_ms), SZ(window_close_ms), 3,
		"窗口关闭动画时长" },
	{ "window_move_ms",  K_INT,    OFF(window_move_ms),  SZ(window_move_ms), 3,
		"窗口移动 / 布局重排的动画时长" },
	{ "layer_open_ms",   K_INT,    OFF(layer_open_ms),   SZ(layer_open_ms), 3,
		"layer-shell（状态栏、通知、挂件）出现的动画时长" },
	{ "layer_close_ms",  K_INT,    OFF(layer_close_ms),  SZ(layer_close_ms), 3,
		"layer-shell 消失的动画时长" },
	{ "easing",          K_EASING, OFF(easing),          SZ(easing), 3,
		"缓动曲线：linear / out_cubic / out_quint / in_out_cubic / out_back / "
		"out_elastic" },
	{ "open_alpha_from", K_DOUBLE, OFF(open_alpha_from), SZ(open_alpha_from), 3,
		"打开动画的起始透明度" },
	{ "open_slide_px",   K_INT,    OFF(open_slide_px),   SZ(open_slide_px), 3,
		"打开动画的起始位移（像素）" },
	{ "close_alpha_to",  K_DOUBLE, OFF(close_alpha_to),  SZ(close_alpha_to), 3,
		"关闭动画的结束透明度（透明度按线性进度淡：缓动曲线只管位移和缩放，"
		"过冲的曲线喂给透明度会让它在动画头 1/4 就掉到 0）" },
	{ "close_slide_px",  K_INT,    OFF(close_slide_px),  SZ(close_slide_px), 3,
		"关闭动画的结束位移（像素）" },
	{ "open_easing",     K_EASING, OFF(open_easing),     SZ(open_easing), 3,
		"打开动画的缓动曲线（默认 out_elastic：过冲一下再收住）" },
	{ "close_easing",    K_EASING, OFF(close_easing),    SZ(close_easing), 3,
		"关闭动画的缓动曲线（管位移 / 缩放；透明度不受它影响）" },
	{ "open_scale_from", K_DOUBLE, OFF(open_scale_from), SZ(open_scale_from), 3,
		"打开动画的起始缩放（1.0 = 不缩；0.82 = 弹一下）" },
	{ "close_scale_to",  K_DOUBLE, OFF(close_scale_to),  SZ(close_scale_to), 3,
		"关闭动画的结束缩放（1.0 = 不缩）" },

	{ "jelly",           K_BOOL,   OFF(jelly),           SZ(jelly), 3,
		"拖窗口时的果冻效果：抓点跟着光标走，窗口其余部分像一块布被拖着、松手"
		"后晃两下收住（要窗口有装饰节点，也就是 decor 或 shader 开着）" },
	{ "jelly_strength",  K_DOUBLE, OFF(jelly_strength),  SZ(jelly_strength), 3,
		"果冻的形变倍率（1.0 = 默认，0.5 = 收敛些，0 = 不弯）" },

	{ "shader_enabled",  K_BOOL,   OFF(shader_enabled),  SZ(shader_enabled), 4,
		"自定义 shader 总开关（需要 GLES2 渲染器）" },
	{ "shader_dir",      K_STRING, OFF(shader_dir),      SZ(shader_dir), 4,
		"shader 目录，加载 <目录>/<名字>.frag" },
	{ "shader_flip_y",   K_BOOL,   OFF(shader_flip_y),   SZ(shader_flip_y), 4,
		"采样时翻转 y（自己写的 shader 上下反了才需要）" },
	{ "shader_open",     K_STRING, OFF(shader_open),     SZ(shader_open), 4,
		"窗口打开时用的 shader 名字，留空 = 不用" },
	{ "shader_close",    K_STRING, OFF(shader_close),    SZ(shader_close), 4,
		"窗口关闭时用的 shader 名字，留空 = 不用" },
	{ "shader_idle",     K_STRING, OFF(shader_idle),     SZ(shader_idle), 4,
		"窗口空闲（一直开着不动画）时用的 shader 名字，留空 = 不用" },

	{ "force_render_hz", K_INT,    OFF(force_render_hz), SZ(force_render_hz), 5,
		"滚出视口 / 被藏起来 / 最小化的窗口每秒补多少帧（0 = 不限），配合规则的 force_render" },

	{ "clipboard",       K_BOOL,   OFF(clipboard),       SZ(clipboard), 6,
		"接管 X11(XWayland) ↔ Wayland 的剪贴板同步" },
	{ "clipboard_primary", K_BOOL, OFF(clipboard_primary),
		SZ(clipboard_primary), 6, "PRIMARY（选中即复制）也一起同步" },
	{ "clipboard_max_bytes", K_INT, OFF(clipboard_max_bytes),
		SZ(clipboard_max_bytes), 6,
		"每种类型在内存里留的副本上限（字节），超了丢这个类型" },

	{ "ipc",             K_BOOL,   OFF(ipc),             SZ(ipc), 7,
		"开 IPC socket（vidowmsg、状态栏、脚本靠它）" },
	{ "ipc_socket",      K_STRING, OFF(ipc_socket),      SZ(ipc_socket), 7,
		"socket 路径，留空 = 按 WAYLAND_DISPLAY 自动取" },
	{ "input_injection", K_BOOL,   OFF(input_injection), SZ(input_injection), 7,
		"IPC 能不能合成鼠标 / 键盘事件（move-cursor / pointer-button / "
		"pointer-scroll / send-key / type-text）：等于「谁连上 socket 谁就能"
		"操控整个桌面」。默认开，每次注入都打一条 info 日志" },

	{ "scale",           K_STRING, OFF(scale),           SZ(scale), 8,
		"所有输出的默认缩放：auto = 按 EDID 的 ppi 推，也可以写死 \"1\" / \"1.5\""
		"；单块屏点名用 vidow.output{ name = …, scale = … }" },

	{ "xwayland",        K_BOOL,   OFF(xwayland),        SZ(xwayland), 9,
		"开 XWayland（有人连 X 才启动服务）" },
	{ "xwayland_scale",  K_STRING, OFF(xwayland_scale),  SZ(xwayland_scale), 9,
		"XWayland 的缩放：auto = 所有输出里最大的那个" },
	{ "xwayland_env",    K_BOOL,   OFF(xwayland_env),    SZ(xwayland_env), 9,
		"给客户端设 XDG_SESSION_TYPE 这类 X11 环境变量" },

	{ "activation_token_timeout", K_INT, OFF(activation_token_timeout),
		SZ(activation_token_timeout), 10,
		"激活令牌多久作废（秒），0 = 不过期" },
	{ "honor_xdg_activation_with_invalid_serial", K_BOOL,
		OFF(honor_xdg_activation_with_invalid_serial),
		SZ(honor_xdg_activation_with_invalid_serial), 10,
		"serial 校验不过的激活请求也理（默认不理）" },

	{ "portal_conf_sync", K_BOOL,  OFF(portal_conf_sync), SZ(portal_conf_sync), 11,
		"把 XDG_CURRENT_DESKTOP 设成下面这个名字" },
	{ "portal_desktop",  K_STRING, OFF(portal_desktop),  SZ(portal_desktop), 11,
		"XDG_CURRENT_DESKTOP 的值（xdg-desktop-portal 按它选 <名字>-portals.conf）" },
	{ "portal_capture",  K_STRING, OFF(portal_capture),  SZ(portal_capture), 11,
		"录屏 / 截图交给谁：auto（默认）/ vidow / kde / wlr / hyprland / none" },

	{ "systemd_env",     K_BOOL,   OFF(systemd_env),     SZ(systemd_env), 15,
		"启动时从 systemd 用户管理器吸环境（我们自己没有的才吸：PATH / "
		"SSH_AUTH_SOCK / environment.d 里那些）；WAYLAND_DISPLAY / DISPLAY 这类"
		"由 vidow 自己定，不吸" },
	{ "systemd_env_export", K_BOOL, OFF(systemd_env_export),
		SZ(systemd_env_export), 15,
		"把 vidow 的环境推给 systemd 用户管理器 + dbus 激活环境"
		"（systemctl --user import-environment + dbus-update-activation-environment "
		"--systemd）：xdg-desktop-portal、systemd 服务和桌面文件激活的程序才"
		"连得上 vidow" },

	{ "cursor_shake",    K_BOOL,   OFF(cursor_shake),    SZ(cursor_shake), 12,
		"摇晃指针就放大一下（快速来回晃几下，指针变大一会儿，好找）" },
	{ "cursor_shake_scale", K_DOUBLE, OFF(cursor_shake_scale),
		SZ(cursor_shake_scale), 12,
		"每一圈加多少倍：2 = 第一圈就到大一倍，之后每圈再 +1（2x → 3x → 4x …，"
		"线性加不是乘）。放大走硬件光标那条路（放得下就不进截图），放不下会被降级"
		"成软光标，所以实际倍数会先按硬件光标尺寸上限夹住" },

	{ "mru_enabled",     K_BOOL,   OFF(mru_enabled),     SZ(mru_enabled), 14,
		"Alt+Tab 的窗口切换器（niri 的 recent windows，见 src/mru.c）：关掉就不"
		"记账，那两个动作也不认" },
	{ "mru_debounce_ms", K_INT,    OFF(mru_debounce_ms), SZ(mru_debounce_ms), 14,
		"焦点要「待够」多久才算真的用过（毫秒）：键盘一路导航碰到的窗口只算路过，"
		"0 = 立刻记账" },
	{ "mru_open_delay_ms", K_INT,  OFF(mru_open_delay_ms), SZ(mru_open_delay_ms), 14,
		"按住多久面板才显形（毫秒）：快速点一下 Alt+Tab 只想切窗口、不该整屏闪一下，"
		"0 = 立刻显形" },
	{ "mru_scope",       K_MRU_SCOPE, OFF(mru_scope),    SZ(mru_scope), 14,
		"列哪些窗口：all（所有屏所有工作区）/ workspace（当前工作区）/ output"
		"（当前这块屏）" },
	{ "mru_layout",      K_MRU_LAYOUT, OFF(mru_layout),  SZ(mru_layout), 14,
		"排版：strip = 一行，装不下横向滚；grid = 不滚，按 mru_per_row 换行铺在"
		"屏幕里（装不下就整体缩小预览）" },
	{ "mru_per_row",     K_INT,    OFF(mru_per_row),     SZ(mru_per_row), 14,
		"grid 一行最多几个（窗口再多就换行）" },
	{ "mru_gap",         K_INT,    OFF(mru_gap),         SZ(mru_gap), 14,
		"预览之间留多少（逻辑像素）" },
	{ "mru_row_gap",     K_INT,    OFF(mru_row_gap),     SZ(mru_row_gap), 14,
		"grid 行与行之间留多少（逻辑像素）" },
	{ "mru_max_scale",   K_DOUBLE, OFF(mru_max_scale),   SZ(mru_max_scale), 14,
		"预览最多缩到多少（0.5 = 缩一半，1 = 原尺寸）" },
	{ "mru_max_height",  K_INT,    OFF(mru_max_height),  SZ(mru_max_height), 14,
		"预览高度上限（逻辑像素）：大屏上别让一个窗口占满整屏" },
	{ "mru_min_scale",   K_DOUBLE, OFF(mru_min_scale),   SZ(mru_min_scale), 14,
		"再挤也不能比它更小（grid 里窗口特别多时的下限）" },
	{ "mru_backdrop_alpha", K_DOUBLE, OFF(mru_backdrop_alpha),
		SZ(mru_backdrop_alpha), 14,
		"切换器后面那层压暗的浓度（0 = 不压暗）" },
	{ "mru_highlight_padding", K_INT, OFF(mru_highlight_padding),
		SZ(mru_highlight_padding), 14,
		"选中项背后那块板比预览大多少（逻辑像素）" },
	{ "mru_highlight_border", K_INT, OFF(mru_highlight_border),
		SZ(mru_highlight_border), 14,
		"选中项的边框宽度（0 = 不画边框）" },
	{ "mru_highlight_color", K_COLOR, OFF(mru_highlight_color),
		SZ(mru_highlight_color), 14,
		"选中项那块板和边框的颜色（RRGGBB 或 RRGGBBAA）" },
};

/* ------------------------------------------------------------ 配置组
 *
 * 一族配置既能「一个键管全部」，也能只写其中几项，而不会摊成一堆并列的顶层
 * 键：
 *     gaps   = 10                       -- 全部 10px
 *     gaps   = { top = 30, inner = 20 } -- 只改上边距和窗口之间
 *     border = { width = 0, color = "#2a2e3aff" }
 * 表里每项展开成下面这张表里对应的平铺键 —— 平铺键照样能用（环境变量
 * VIDOW_*、vidowmsg set-config、老配置都走平铺键）。
 *
 * scalar 是「组名 = 一个值」时落到哪个平铺键；NULL 表示这个组必须写表
 * （组名本身不是配置键，一个光秃秃的值没法解释）。 */
struct group_field {
	const char *name; /* 表里的项名 */
	const char *key;  /* 展开成哪个平铺键 */
};

struct group {
	const char *name;
	const char *scalar; /* 组名 = 标量时用哪个键，NULL = 只认表 */
	const struct group_field *fields;
	size_t n;
};

static const struct group_field gaps_fields[] = {
	{ "top",    "gaps_top" },
	{ "bottom", "gaps_bottom" },
	{ "left",   "gaps_left" },
	{ "right",  "gaps_right" },
	{ "inner",  "gaps_inner" },
};
static const struct group_field decor_fields[] = {
	{ "enabled",       "decor_enabled" },
	{ "corner_radius", "corner_radius" },
	{ "opacity",       "window_opacity" },
	{ "noise",         "noise" },
};
static const struct group_field border_fields[] = {
	{ "width",       "border_width" },
	{ "color",       "border_color" },
	{ "color_focus", "border_color_focus" },
};
static const struct group_field shadow_fields[] = {
	{ "radius",   "shadow_radius" },
	{ "color",    "shadow_color" },
	{ "offset_x", "shadow_offset_x" },
	{ "offset_y", "shadow_offset_y" },
};
static const struct group_field blur_fields[] = {
	{ "enabled",   "blur_enabled" },
	{ "all",       "blur_all" },
	{ "xray",      "blur_xray" },
	{ "levels",    "blur_levels" },
	{ "strength",  "blur_strength" },
	{ "alpha_min", "blur_alpha_min" },
	{ "offset",    "blur_offset" },
};
static const struct group_field anim_fields[] = {
	{ "open_ms",        "window_open_ms" },
	{ "close_ms",       "window_close_ms" },
	{ "move_ms",        "window_move_ms" },
	{ "layer_open_ms",  "layer_open_ms" },
	{ "layer_close_ms", "layer_close_ms" },
	{ "easing",         "easing" },
	{ "open_alpha_from", "open_alpha_from" },
	{ "open_slide_px",  "open_slide_px" },
	{ "close_alpha_to", "close_alpha_to" },
	{ "close_slide_px", "close_slide_px" },
	{ "open_easing",    "open_easing" },
	{ "close_easing",   "close_easing" },
	{ "open_scale_from", "open_scale_from" },
	{ "close_scale_to", "close_scale_to" },
	{ "jelly",          "jelly" },
	{ "jelly_strength", "jelly_strength" },
};
static const struct group_field shader_fields[] = {
	{ "enabled", "shader_enabled" },
	{ "dir",     "shader_dir" },
	{ "flip_y",  "shader_flip_y" },
	{ "open",    "shader_open" },
	{ "close",   "shader_close" },
	{ "idle",    "shader_idle" },
};
static const struct group_field clipboard_fields[] = {
	{ "enabled",   "clipboard" },
	{ "primary",   "clipboard_primary" },
	{ "max_bytes", "clipboard_max_bytes" },
};
static const struct group_field ipc_fields[] = {
	{ "enabled", "ipc" },
	{ "socket",  "ipc_socket" },
};
static const struct group_field xwayland_fields[] = {
	{ "enabled", "xwayland" },
	{ "scale",   "xwayland_scale" },
	{ "env",     "xwayland_env" },
};
static const struct group_field mru_fields[] = {
	{ "enabled",           "mru_enabled" },
	{ "debounce_ms",       "mru_debounce_ms" },
	{ "open_delay_ms",     "mru_open_delay_ms" },
	{ "scope",             "mru_scope" },
	{ "layout",            "mru_layout" },
	{ "per_row",           "mru_per_row" },
	{ "gap",               "mru_gap" },
	{ "row_gap",           "mru_row_gap" },
	{ "max_scale",         "mru_max_scale" },
	{ "max_height",        "mru_max_height" },
	{ "min_scale",         "mru_min_scale" },
	{ "backdrop_alpha",    "mru_backdrop_alpha" },
	{ "highlight_padding", "mru_highlight_padding" },
	{ "highlight_border",  "mru_highlight_border" },
	{ "highlight_color",   "mru_highlight_color" },
};
static const struct group_field portal_fields[] = {
	{ "conf_sync", "portal_conf_sync" },
	{ "desktop",   "portal_desktop" },
	{ "capture",   "portal_capture" },
};

#define GROUP(name, scalar, fields) \
	{ name, scalar, fields, sizeof(fields) / sizeof(fields[0]) }

static const struct group groups[] = {
	GROUP("gaps",      "gaps",           gaps_fields),
	GROUP("decor",     "decor_enabled",  decor_fields),
	GROUP("border",    NULL,             border_fields),
	GROUP("shadow",    NULL,             shadow_fields),
	GROUP("blur",      "blur_enabled",   blur_fields),
	GROUP("anim",      NULL,             anim_fields),
	GROUP("shader",    "shader_enabled", shader_fields),
	GROUP("clipboard", "clipboard",      clipboard_fields),
	GROUP("ipc",       "ipc",            ipc_fields),
	GROUP("xwayland",  "xwayland",       xwayland_fields),
	GROUP("portal",    NULL,             portal_fields),
	GROUP("mru",       "mru_enabled",    mru_fields),
};

static const struct group *group_named(const char *name) {
	for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
		if (strcmp(groups[i].name, name) == 0) {
			return &groups[i];
		}
	}
	return NULL;
}

/* 组名 = 标量时落到哪个平铺键（`clipboard = false` → clipboard）。不是组、
 * 或者这个组不认标量，返回 NULL。 */
const char *vidow_config_group_scalar(const char *name) {
	const struct group *g = group_named(name);
	return g != NULL ? g->scalar : NULL;
}

/* 表里的项名 → 平铺键（`border` + `width` → border_width）。 */
const char *vidow_config_group_field(const char *group, const char *field) {
	const struct group *g = group_named(group);
	if (g == NULL) {
		return NULL;
	}
	for (size_t i = 0; i < g->n; i++) {
		if (strcmp(g->fields[i].name, field) == 0) {
			return g->fields[i].key;
		}
	}
	return NULL;
}

/* 删掉的键名：配置里写了它们以前是生效的，现在只会变成普通全局变量（静默失效）。
 * 认出来，让 script.c 喊一声 —— 「配置写了没反应」最难查。 */
static const struct {
	const char *key;
	const char *hint;
} legacy_keys[] = {
	{ "workspace_gaps_inner", "gaps（一个数管全部）或 gaps = { inner = … }" },
	{ "workspace_gaps_outer", "gaps 或 gaps = { top/bottom/left/right = … }" },
	{ "window_gaps_inner",    "gaps_inner 或 gaps = { inner = … }" },
	{ "window_gaps_outer",    "gaps 或 gaps = { top/bottom/left/right = … }" },
	{ "outputs",              "vidow.output{ name = \"DP-3\", scale = 2 }"
		"（一块屏一组，缩放和位置写在一起）" },
	{ "output_positions",     "vidow.output{ name = \"DP-3\", x = 0, y = 0 }"
		"（一块屏一组，缩放和位置写在一起）" },
};

bool vidow_config_legacy_hint(const char *key, const char **hint) {
	for (size_t i = 0; i < sizeof(legacy_keys) / sizeof(legacy_keys[0]); i++) {
		if (strcmp(legacy_keys[i].key, key) == 0) {
			if (hint != NULL) {
				*hint = legacy_keys[i].hint;
			}
			return true;
		}
	}
	return false;
}

/* 这个组认哪些项 —— 报错信息里列出来，别让人猜。 */
size_t vidow_config_group_fields(const char *group, const char **out, size_t max) {
	const struct group *g = group_named(group);
	if (g == NULL) {
		return 0;
	}
	size_t n = g->n < max ? g->n : max;
	for (size_t i = 0; i < n; i++) {
		out[i] = g->fields[i].name;
	}
	return n;
}

/* 是不是「族」的组名（gaps / border / blur …）。 */
bool vidow_config_group_has(const char *name) {
	return group_named(name) != NULL;
}

static const char *easing_names[] = {
	"linear", "out_cubic", "out_quint", "in_out_cubic", "out_back",
	"out_elastic",
};

static const char *mru_scope_names[] = { "all", "workspace", "output" };
static const char *mru_layout_names[] = { "strip", "grid" };

static bool parse_mru_scope(const char *value, enum vidow_mru_scope *out) {
	for (size_t i = 0; i < 3; i++) {
		if (strcmp(value, mru_scope_names[i]) == 0) {
			*out = (enum vidow_mru_scope)i;
			return true;
		}
	}
	return false;
}

static bool parse_mru_layout(const char *value, enum vidow_mru_layout *out) {
	for (size_t i = 0; i < 2; i++) {
		if (strcmp(value, mru_layout_names[i]) == 0) {
			*out = (enum vidow_mru_layout)i;
			return true;
		}
	}
	return false;
}

static bool parse_easing(const char *value, enum vidow_easing *out) {
	for (size_t i = 0; i < VIDOW_EASE_OUT_ELASTIC + 1; i++) {
		if (strcmp(value, easing_names[i]) == 0) {
			*out = (enum vidow_easing)i;
			return true;
		}
	}
	return false;
}

/* "RRGGBB" / "RRGGBBAA"（# 可有可无）→ 四个 0..1 的分量，直通 alpha（非预乘）。
 * 配置键（K_COLOR）和窗口规则的颜色作用共用这一份。 */
bool vidow_config_parse_color(const char *value, float out[4]) {
	if (value == NULL) {
		return false;
	}
	const char *hex = value[0] == '#' ? value + 1 : value;
	size_t len = strlen(hex);
	if (len != 6 && len != 8) {
		return false;
	}
	char part[3] = {0};
	for (int i = 0; i < 4; i++) {
		if (i == 3 && len == 6) {
			out[3] = 1.0f;
			break;
		}
		memcpy(part, hex + i * 2, 2);
		if (!isxdigit((unsigned char)part[0]) ||
				!isxdigit((unsigned char)part[1])) {
			return false;
		}
		out[i] = (float)strtol(part, NULL, 16) / 255.0f;
	}
	return true;
}

/* Returns false if the key is unknown or the value cannot be parsed. */
static bool apply(struct vidow_config *cfg, const char *key, const char *value) {
	for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		const struct entry *e = &entries[i];
		if (strcmp(e->key, key) != 0) {
			continue;
		}
		void *field = (char *)cfg + e->offset;
		switch (e->kind) {
		case K_INT:
			*(int *)field = atoi(value);
			return true;
		case K_DOUBLE:
			*(double *)field = atof(value);
			return true;
		case K_BOOL:
			*(bool *)field = strcmp(value, "0") != 0 &&
				strcasecmp(value, "false") != 0 &&
				strcasecmp(value, "no") != 0 &&
				strcasecmp(value, "off") != 0;
			return true;
		case K_EASING:
			if (!parse_easing(value, (enum vidow_easing *)field)) {
				wlr_log(WLR_ERROR, "config: unknown easing '%s'", value);
				return false;
			}
			return true;
		case K_MRU_SCOPE:
			if (!parse_mru_scope(value, (enum vidow_mru_scope *)field)) {
				wlr_log(WLR_ERROR, "config: mru scope 只能是 all / "
					"workspace / output（给的是 '%s'）", value);
				return false;
			}
			return true;
		case K_MRU_LAYOUT:
			if (!parse_mru_layout(value, (enum vidow_mru_layout *)field)) {
				wlr_log(WLR_ERROR, "config: mru layout 只能是 strip / "
					"grid（给的是 '%s'）", value);
				return false;
			}
			return true;
		case K_STRING:
			snprintf((char *)field, e->size, "%s", value);
			return true;
		case K_COLOR:
			if (!vidow_config_parse_color(value, (float *)field)) {
				wlr_log(WLR_ERROR, "config: colour '%s' must be RRGGBB or RRGGBBAA",
					value);
				return false;
			}
			return true;
		}
	}
	return false;
}

/* ------------------------------------------------------------ 文件位置 */

/* 配置目录：$XDG_CONFIG_HOME/vidow，没设就是 ~/.config/vidow。 */
void vidow_config_dir(char *buf, size_t size) {
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg != NULL && xdg[0] == '/') {
		snprintf(buf, size, "%s/vidow", xdg);
		return;
	}
	const char *home = getenv("HOME");
	snprintf(buf, size, "%s/.config/vidow", home != NULL ? home : ".");
}

/* 唯一的配置文件。$VIDOW_CONFIG 指哪读哪（$VIDOW_CONFIG_LUA 是旧名，还认）。 */
void vidow_config_path(char *buf, size_t size) {
	const char *override = getenv("VIDOW_CONFIG");
	if (override == NULL || override[0] == '\0') {
		override = getenv("VIDOW_CONFIG_LUA");
	}
	if (override != NULL && override[0] != '\0') {
		snprintf(buf, size, "%s", override);
		return;
	}
	char dir[PATH_MAX];
	vidow_config_dir(dir, sizeof(dir));
	snprintf(buf, size, "%s/config.lua", dir);
}

/* 配置统一成 Lua 之前那个 key = value 文件：还在的话喊一声，别让它悄悄失效
 * （用户会以为 vidow 坏了）。只喊一次。 */
static void warn_legacy_flat(void) {
	static bool warned = false;
	if (warned) {
		return;
	}
	warned = true;
	char dir[PATH_MAX - 16];
	vidow_config_dir(dir, sizeof(dir));
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/config", dir);
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return;
	}
	fclose(f);
	wlr_log(WLR_ERROR, "config: %s 不再被读取了（配置已统一成 Lua）—— "
		"把里面的键搬进 %s/config.lua，然后删掉这个文件", path, dir);
}

static void defaults(struct vidow_config *cfg) {
	memset(cfg, 0, sizeof(*cfg));
	cfg->force_render_hz = 60; /* 视口外补帧：跟屏幕刷新率差不多就够 */
	/* 激活令牌不过期：wlroots 默认 30 秒，而「弹通知」和「点通知」之间常常隔着
	 * 几分钟 —— 令牌过期了 wlroots 直接把激活请求丢掉，我们连信号都收不到
	 * （点了通知窗口不动，看着像客户端坏了）。 */
	cfg->activation_token_timeout = 0;
	/* gap 默认保持原型原来的样子：离屏幕边 6px、窗口之间 6px —— 现在就是一个
	 * 数（以前拆成内外两份，`gaps 6` 会变成离屏幕边 12px）。 */
	cfg->gaps = 6;
	/* 单边设置：-1 = 没设，跟着统一值走。 */
	cfg->gaps_top = -1;
	cfg->gaps_bottom = -1;
	cfg->gaps_left = -1;
	cfg->gaps_right = -1;
	cfg->gaps_inner = -1;
	/* serial 校验不通过的激活请求默认不理（= wlroots 一直以来的行为）。 */
	cfg->honor_xdg_activation_with_invalid_serial = false;
	/* IPC 默认开着：socket 只有自己这个用户能连（0600），而状态栏 / 脚本全靠它。
	 * 路径留空 = 按 WAYLAND_DISPLAY 自动取（$XDG_RUNTIME_DIR/vidow-<display>.sock）。 */
	cfg->ipc = true;
	cfg->ipc_socket[0] = '\0';
	/* 会话环境默认双向对账（见 env.c）：从 systemd 吸（我们自己没有的）、把
	 * vidow 的环境推回去 —— 推不出去的话 systemd / dbus 拉起来的应用看不到
	 * vidow 的 WAYLAND_DISPLAY，连的还是宿主合成器。 */
	cfg->systemd_env = true;
	cfg->systemd_env_export = true;
	/* 输入注入默认开：IPC 能合成鼠标 / 键盘事件（move-cursor / pointer-button /
	 * pointer-scroll / send-key / type-text），自动化靠它动手。socket 是 0600，
	 * 只有本用户能连，而且每次注入都打一条 info 日志留痕；要收紧就写 false。 */
	cfg->input_injection = true;
	/* 摇晃找光标默认开：快速来回晃几下指针就放大一会儿，好找。放大是换一张更大
	 * 的光标图，走硬件光标那条路（不进渲染通道 = 不进截图），所以倍数会先按硬件
	 * 上限夹一下（见 cursorfx.c）。 */
	/* Alt+Tab 的窗口切换器（见 mru.c）：默认开。debounce 750ms / open_delay
	 * 150ms 是 niri 的默认值 —— 前者让「路过」的窗口不进台账，后者让快速点一下
	 * Alt+Tab 只是切窗口、不闪屏。排版默认一行（strip）；要「不滚动、换行铺在
	 * 屏幕里」就 mru = { layout = "grid", per_row = 4 }。 */
	cfg->mru_enabled = true;
	cfg->mru_debounce_ms = 750;
	cfg->mru_open_delay_ms = 150;
	cfg->mru_scope = VIDOW_MRU_ALL;
	cfg->mru_layout = VIDOW_MRU_STRIP;
	cfg->mru_per_row = 5;
	cfg->mru_gap = 16;
	cfg->mru_row_gap = 16;
	cfg->mru_max_scale = 0.5;
	cfg->mru_max_height = 480;
	cfg->mru_min_scale = 0.05;
	cfg->mru_backdrop_alpha = 0.8;
	cfg->mru_highlight_padding = 30;
	cfg->mru_highlight_border = 2;
	cfg->mru_highlight_color[0] = 0x99 / 255.0f;
	cfg->mru_highlight_color[1] = 0x99 / 255.0f;
	cfg->mru_highlight_color[2] = 0x99 / 255.0f;
	cfg->mru_highlight_color[3] = 1.0f;

	cfg->cursor_shake = true;
	cfg->cursor_shake_scale = 2.0;
	/* 剪贴板默认接管：X11(Xwayland) 和 Wayland 两边共用一份内容，而且不看焦点
	 * （wlroots 自带那套只在有 X11 窗口聚焦时才同步）。每个类型读进内存的上限
	 * 32MiB —— 「源程序退出后还能粘」靠的就是这份副本。 */
	cfg->clipboard = true;
	cfg->clipboard_primary = true;
	cfg->clipboard_max_bytes = 32 * 1024 * 1024;
	cfg->window_open_ms = 180;
	cfg->window_close_ms = 140;
	cfg->window_move_ms = 190;
	cfg->layer_open_ms = 200;
	cfg->layer_close_ms = 150;
	cfg->easing = VIDOW_EASE_OUT_CUBIC;

	cfg->open_alpha_from = 0.0;
	cfg->open_slide_px = 22;
	cfg->close_alpha_to = 0.0;
	cfg->close_slide_px = 14;
	/* 入场/关闭默认回弹：从 0.82 弹到 1.0，冲到 1.5% 再收住。上面的 easing
	 * 保持 out_cubic —— 布局重排不该过冲。 */
	cfg->open_easing = VIDOW_EASE_OUT_ELASTIC;
	cfg->close_easing = VIDOW_EASE_OUT_ELASTIC;
	cfg->open_scale_from = 0.82;
	cfg->close_scale_to = 0.82;

	/* 拖窗口的果冻（见 jelly.c）。默认开着：这是「拖着走」这件事本身的
	 * 手感，不是额外的花活。 */
	cfg->jelly = true;
	cfg->jelly_strength = 1.0;

	cfg->decor_enabled = true;
	/* 关着：客户端自己画的装饰（CSD）不动它。开了就告诉客户端「别画了」，
	 * 见 decoration.c —— GTK3 只有这条老协议听得见。 */
	cfg->prefer_no_csd = false;
	cfg->corner_radius = 12.0;
	cfg->border_width = 2.0;
	cfg->border_color[0] = 0.30f; /* unfocused: dim */
	cfg->border_color[1] = 0.33f;
	cfg->border_color[2] = 0.42f;
	cfg->border_color[3] = 1.0f;
	cfg->border_color_focus[0] = 0.48f; /* focused: accent */
	cfg->border_color_focus[1] = 0.65f;
	cfg->border_color_focus[2] = 0.97f;
	cfg->border_color_focus[3] = 1.0f;
	/* 薄薄贴边一圈：24px 那个晕开太散（看着像窗口浮在一团灰里）。8px 的衰减
	 * 正好是「沿窗口边缘一圈」，偏移也从 6 收到 2（几乎对称，只留一点方向感）。
	 * vidow_fx_pad() 按它算装饰离屏图的内边距，所以变小也省了那块缓冲。 */
	cfg->shadow_radius = 8.0;
	cfg->shadow_color[0] = 0.0f;
	cfg->shadow_color[1] = 0.0f;
	cfg->shadow_color[2] = 0.0f;
	cfg->shadow_color[3] = 0.55f;
	cfg->shadow_offset_x = 0;
	cfg->shadow_offset_y = 2;
	cfg->window_opacity = 1.0;
	cfg->noise = 0.0;

	cfg->blur_enabled = true;
	cfg->blur_all = true; /* tiled windows blur the wallpaper too */
	/* 默认关：重叠时各自算私有底衬才是「真背景模糊」（糊的是压着的那个窗口，
	 * 不是壁纸）。要省就开 blur_xray —— 见 server.h 那段。 */
	cfg->blur_xray = false;
	/* 默认 3 级（≈29 设备像素半径）：4 级糊得更狠（≈65px）但每个模糊元素每帧
	 * 多两趟离屏渲染，实测四个重叠元素下 25 趟/帧 vs 33 趟/帧。要更糊写 4。 */
	cfg->blur_levels = 3;
	cfg->blur_strength = 1.0;
	cfg->blur_alpha_min = 0.15;
	cfg->blur_offset = 1.5;

	cfg->shader_enabled = true;
	cfg->shader_open[0] = '\0';
	cfg->shader_close[0] = '\0';
	cfg->shader_idle[0] = '\0';
	cfg->shader_flip_y = false;

	/* 缩放：默认按 EDID 的 ppi 推，让同一个逻辑尺寸在两块不同 DPI 的屏上是
	 * 同一个物理尺寸。单块屏要点名（缩放 / 摆在哪）写在配置的
	 * vidow.output{ name = … } 里，见 outputconf.c。 */
	snprintf(cfg->scale, sizeof(cfg->scale), "auto");

	/* XWayland：默认懒启动（有人连 X 才起服务）。xwayland_scale=auto 取所有
	 * 输出缩放的最大值 —— X 只有一套坐标，只能按最密的那块屏来，低了就是放大糊。 */
	cfg->xwayland = true;
	snprintf(cfg->xwayland_scale, sizeof(cfg->xwayland_scale), "auto");
	cfg->xwayland_env = true;

	/* portal：vidow 当会话合成器时把 XDG_CURRENT_DESKTOP 设成 Vidow，
	 * 于是 xdg-desktop-portal 读的是 vidow-portals.conf。 */
	cfg->portal_conf_sync = true;
	snprintf(cfg->portal_desktop, sizeof(cfg->portal_desktop), "Vidow");
	/* 录屏后端默认 auto：vidow-portal 在边上就用自己那个（选源对话框 + 实时
	 * 预览，不依赖任何外部后端），没有才退到 wlr / hyprland。 */
	snprintf(cfg->portal_capture, sizeof(cfg->portal_capture), "auto");

	/* 自定义 shader 的默认目录：跟着配置文件走（$XDG_CONFIG_HOME/vidow/shaders）。 */
	char dir[PATH_MAX - 16];
	vidow_config_dir(dir, sizeof(dir));
	snprintf(cfg->shader_dir, sizeof(cfg->shader_dir), "%s/shaders", dir);
}
/* VIDOW_WINDOW_OPEN_MS=90 ./build/vidow — handy for tests. */
void vidow_config_apply_env(struct vidow_config *cfg) {
	char name[128];
	for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		const struct entry *e = &entries[i];
		snprintf(name, sizeof(name), "VIDOW_%s", e->key);
		for (char *p = name; *p != '\0'; p++) {
			*p = toupper((unsigned char)*p);
		}
		const char *value = getenv(name);
		if (value != NULL) {
			apply(cfg, e->key, value);
		}
	}
}

/* ------------------------------------------------------------ interface */

bool vidow_config_has(const char *key) {
	for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		if (strcmp(entries[i].key, key) == 0) {
			return true;
		}
	}
	return false;
}

bool vidow_config_set(struct vidow_config *cfg, const char *key,
		const char *value) {
	return apply(cfg, key, value);
}

static const struct entry *entry_for(const char *key) {
	for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		if (strcmp(entries[i].key, key) == 0) {
			return &entries[i];
		}
	}
	return NULL;
}

/* Read a key back as text, for configs written in a scripting language.
 * Four rotating buffers so one expression can read several keys. */
const char *vidow_config_get(const struct vidow_config *cfg,
		const char *key) {
	static char slots[4][64];
	static int next = 0;
	char *buf = slots[next];
	next = (next + 1) % 4;
	buf[0] = '\0';

	const struct entry *e = entry_for(key);
	if (e == NULL) {
		return NULL;
	}
	const void *field = (const char *)cfg + e->offset;
	switch (e->kind) {
	case K_INT:
		snprintf(buf, 64, "%d", *(const int *)field);
		break;
	case K_DOUBLE:
		snprintf(buf, 64, "%.10g", *(const double *)field);
		break;
	case K_BOOL:
		snprintf(buf, 64, "%s", *(const bool *)field ? "true" : "false");
		break;
	case K_EASING:
		snprintf(buf, 64, "%s",
			easing_names[*(const enum vidow_easing *)field]);
		break;
	case K_MRU_SCOPE: {
		int v = *(const enum vidow_mru_scope *)field;
		snprintf(buf, 64, "%s", mru_scope_names[v >= 0 && v < 3 ? v : 0]);
		break;
	}
	case K_MRU_LAYOUT: {
		int v = *(const enum vidow_mru_layout *)field;
		snprintf(buf, 64, "%s", mru_layout_names[v >= 0 && v < 2 ? v : 0]);
		break;
	}
	case K_STRING:
		snprintf(buf, 64, "%s", (const char *)field);
		break;
	case K_COLOR: {
		const float *c = field;
		snprintf(buf, 64, "#%02x%02x%02x%02x",
			(int)(c[0] * 255.0f + 0.5f), (int)(c[1] * 255.0f + 0.5f),
			(int)(c[2] * 255.0f + 0.5f), (int)(c[3] * 255.0f + 0.5f));
		break;
	}
	}
	return buf;
}

/* 把所有键按顺序喂出去（IPC 的 config / state 用）。value 一律是文本 —— 外面
 * 拿到的和 config.lua 里读到的（vidow.get）是同一个东西。 */
void vidow_config_each(const struct vidow_config *cfg,
		void (*cb)(void *data, const char *key, const char *value), void *data) {
	if (cb == NULL) {
		return;
	}
	for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		const char *value = vidow_config_get(cfg, entries[i].key);
		cb(data, entries[i].key, value != NULL ? value : "");
	}
}

void vidow_config_log(const struct vidow_config *cfg) {
	wlr_log(WLR_INFO,
		"config: decor=%s prefer_no_csd=%s radius=%.1f border=%.1f "
		"shadow=%.1f/%.1f,%d opacity=%.2f noise=%.2f blur=%s all=%s "
		"levels=%d strength=%.2f alpha_min=%.2f",
		cfg->decor_enabled ? "on" : "off",
		cfg->prefer_no_csd ? "on" : "off", cfg->corner_radius,
		cfg->border_width, cfg->shadow_radius, cfg->shadow_color[3] * 255.0,
		cfg->shadow_offset_y, cfg->window_opacity, cfg->noise,
		cfg->blur_enabled ? "on" : "off", cfg->blur_all ? "on" : "off",
		cfg->blur_levels, cfg->blur_strength, cfg->blur_alpha_min);
	wlr_log(WLR_INFO, "config: force_render_hz=%d（视口外补帧的节流，0 = 不限）",
		cfg->force_render_hz);
	wlr_log(WLR_INFO, "config: gap 统一 %d → 屏幕边到窗口 上%d 下%d 左%d 右%d、"
		"窗口之间 %d（单边 -1 = 跟随统一值）",
		cfg->gaps,
		vidow_gap_side(cfg, cfg->gaps_top), vidow_gap_side(cfg, cfg->gaps_bottom),
		vidow_gap_side(cfg, cfg->gaps_left), vidow_gap_side(cfg, cfg->gaps_right),
		vidow_gap_inner(cfg));
	wlr_log(WLR_INFO, "config: activation_token_timeout=%d 秒（激活令牌多久作废，"
		"0 = 不过期）", cfg->activation_token_timeout);
	wlr_log(WLR_INFO, "config: honor_xdg_activation_with_invalid_serial=%s"
		"（serial 校验不过的激活请求理不理）",
		cfg->honor_xdg_activation_with_invalid_serial ? "on" : "off");
	wlr_log(WLR_INFO, "config: ipc=%s socket=%s",
		cfg->ipc ? "on" : "off",
		cfg->ipc_socket[0] != '\0' ? cfg->ipc_socket : "(自动)");
	wlr_log(WLR_INFO, "config: input_injection=%s（IPC 能不能合成鼠标 / 键盘）",
		cfg->input_injection ? "on" : "off");
	wlr_log(WLR_INFO, "config: cursor_shake=%s scale=%.2f（摇晃指针就放大，好找）",
		cfg->cursor_shake ? "on" : "off", cfg->cursor_shake_scale);
	wlr_log(WLR_INFO, "config: clipboard=%s primary=%s max=%d 字节",
		cfg->clipboard ? "on" : "off",
		cfg->clipboard_primary ? "on" : "off", cfg->clipboard_max_bytes);
	wlr_log(WLR_INFO,
		"config: scale=%s xwayland=%s xscale=%s xenv=%s（每块屏的缩放 / 位置见"
		"上面的 output 配置行）",
		cfg->scale, cfg->xwayland ? "on" : "off", cfg->xwayland_scale,
		cfg->xwayland_env ? "on" : "off");
	wlr_log(WLR_INFO,
		"config: open=%dms(%s %.2f) close=%dms(%s %.2f) move=%dms "
		"layer=%d/%dms easing=%s shader=%s open=%s close=%s idle=%s",
		cfg->window_open_ms, easing_names[cfg->open_easing],
		cfg->open_scale_from,
		cfg->window_close_ms, easing_names[cfg->close_easing],
		cfg->close_scale_to,
		cfg->window_move_ms,
		cfg->layer_open_ms, cfg->layer_close_ms, easing_names[cfg->easing],
		cfg->shader_enabled ? "on" : "off",
		cfg->shader_open[0] ? cfg->shader_open : "-",
		cfg->shader_close[0] ? cfg->shader_close : "-",
		cfg->shader_idle[0] ? cfg->shader_idle : "-");
}

/* 回到默认值，再压上环境变量。配置文件（Lua）跑之前、以及每次重载之前都调
 * 这个 —— 重载等于「重新读一遍配置」，删掉的行才会真的消失。 */
void vidow_config_reset(struct vidow_config *cfg) {
	defaults(cfg);
	vidow_config_apply_env(cfg);
	warn_legacy_flat();
	vidow_config_log(cfg);
}

/* ------------------------------------------------- 完整的默认配置（打印） */

/* 哪个分节里有哪个「族」，打默认配置时附一句表写法。 */
static const struct {
	int section;
	const char *group;
} section_groups[] = {
	{ 0, "gaps" },
	{ 1, "decor" }, { 1, "border" }, { 1, "shadow" },
	{ 2, "blur" },
	{ 3, "anim" },
	{ 4, "shader" },
	{ 6, "clipboard" },
	{ 7, "ipc" },
	{ 9, "xwayland" },
	{ 11, "portal" },
	{ 14, "mru" },
};

/* 少数默认值是「跟着本机配置目录走」的（现在是 shader_dir）：照绝对路径打出来，
 * 这份默认配置就只在一台机器上成立（还会把 HOME 写进仓库）。这些键只打一行注释
 * 说明默认值怎么来的。 */
static const struct {
	const char *key;
	const char *note;
} derived_defaults[] = {
	{ "shader_dir",
		"默认跟着配置目录走（<配置目录>/shaders，"
		"$XDG_CONFIG_HOME/vidow/shaders 或 ~/.config/vidow/shaders）" },
};

static const char *derived_note(const char *key) {
	for (size_t i = 0; i < sizeof(derived_defaults) / sizeof(derived_defaults[0]);
			i++) {
		if (strcmp(derived_defaults[i].key, key) == 0) {
			return derived_defaults[i].note;
		}
	}
	return NULL;
}

/* 把某个键当前的值按 Lua 语法打出来：字符串/颜色/缓动名字要带引号并转义，
 * 数字和 true / false 原样。 */
static void print_lua_value(FILE *f, const struct vidow_config *cfg,
		const char *key, enum kind kind) {
	const char *v = vidow_config_get(cfg, key);
	if (v == NULL) {
		v = "";
	}
	if (kind != K_STRING && kind != K_COLOR && kind != K_EASING &&
			kind != K_MRU_SCOPE && kind != K_MRU_LAYOUT) {
		fputs(v, f);
		return;
	}
	fputc('"', f);
	for (const char *p = v; *p != '\0'; p++) {
		if (*p == '"' || *p == '\\') {
			fputc('\\', f);
			fputc(*p, f);
		} else if (*p == '\n') {
			fputs("\\n", f);
		} else {
			fputc(*p, f);
		}
	}
	fputc('"', f);
}

/* 一个族写成一张表长什么样 —— 直接给出能粘的默认值，省得去翻代码。 */
static void print_group_hint(FILE *f, const struct vidow_config *cfg,
		const char *name) {
	const struct group *g = group_named(name);
	if (g == NULL) {
		return;
	}
	fprintf(f, "-- 这一族也能写成一张表，只写要改的项（%s = { ", g->name);
	for (size_t i = 0; i < g->n; i++) {
		fprintf(f, "%s%s = ", i > 0 ? ", " : "", g->fields[i].name);
		const struct entry *e = entry_for(g->fields[i].key);
		if (derived_note(g->fields[i].key) != NULL) {
			/* 跟着本机路径走的默认值：这里也不写死路径。 */
			fputs("\"<配置目录>/shaders\"", f);
		} else {
			print_lua_value(f, cfg, g->fields[i].key,
				e != NULL ? e->kind : K_INT);
		}
	}
	fputs(" }）\n", f);
	if (g->scalar != NULL) {
		fprintf(f, "--     不写表也行：%s = ", g->name);
		const struct entry *e = entry_for(g->scalar);
		print_lua_value(f, cfg, g->scalar, e != NULL ? e->kind : K_BOOL);
		fputs("（一个值管全部）\n", f);
	}
}

static void print_section_groups(FILE *f, const struct vidow_config *cfg,
		int section) {
	for (size_t i = 0; i < sizeof(section_groups) / sizeof(section_groups[0]);
			i++) {
		if (section_groups[i].section == section) {
			print_group_hint(f, cfg, section_groups[i].group);
		}
	}
}

/* ------------------------------------------------- 默认配置（打印 / 落盘）
 *
 * 默认配置不是一整份文件：按上面 sections[] 拆成一类一个文件（configs/），入口
 * 只剩 include 行。打印和落盘都从同一张表出来 —— 「入口」「各类文件」「仓库里的
 * 示例」三者不会走散。
 *
 * 每个键都写着它的默认值：抄下来就等于什么都没改，改哪行哪行才生效。不读环境
 * 变量 —— 打出来的是「什么都没配」时的样子。 */

/* 分类文件的文件头：说清楚它自己是怎么被读进来的。 */
static void print_section_header(FILE *f, int section) {
	fprintf(f,
		"-- %s —— vidow 配置的一类，由入口 config.lua 读进来：\n"
		"--     vidow.include(\"configs/%s\")\n"
		"--\n",
		sections[section].title, sections[section].file);
	if (section == SECTION_KEYS) {
		/* 这一份没有配置键：删掉一行就是「不要这个」，没有「回到默认值」一说。 */
		fputs("-- 这一份没有配置键，全是 vidow.* 的调用 —— 删掉一行就是不要它。\n", f);
	} else {
		fputs("-- 下面每行都是内置默认值：改哪行哪行才生效，"
			"删掉那行就回到默认值。\n", f);
	}
	if (sections[section].doc != NULL) {
		fprintf(f, "-- %s\n", sections[section].doc);
	}
	fputc('\n', f);
}

/* 一节的配置键：注释 + 键 = 默认值，末尾附上这一节的「族」写法提示。 */
static void print_section_keys(FILE *f, const struct vidow_config *cfg,
		int section) {
	fprintf(f, "-- ================== %s\n", sections[section].title);
	for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		const struct entry *e = &entries[i];
		if (e->section != section) {
			continue;
		}
		const char *derived = derived_note(e->key);
		if (derived != NULL) {
			fprintf(f, "-- %s\n-- %s\n-- %s = \"<配置目录>/shaders\""
				"   ← 要写死就自己换成绝对路径\n",
				e->doc != NULL ? e->doc : "", derived, e->key);
			continue;
		}
		fprintf(f, "-- %s\n%s = ", e->doc != NULL ? e->doc : "", e->key);
		print_lua_value(f, cfg, e->key, e->kind);
		fputc('\n', f);
	}
	print_section_groups(f, cfg, section);
}

/* 缩放与输出那一节多一段：每块屏的声明写法（那不是配置键，是 vidow.output{}）。 */
static void print_output_hint(FILE *f) {
	fputs(
"\n"
"-- 显示器（输出）是另一类写法：**一块屏一组** —— 缩放、摆在哪儿（逻辑坐标）、\n"
"-- 什么模式、开不开 VRR、几位色、方向、开不开，都是这块屏的事，写在一行里\n"
"-- （写几行就是几块屏；只写要管的字段，没写的 vidow 一概不动）：\n"
"--\n"
"--     vidow.output{ name = \"DP-3\", scale = 2, x = 0, y = 0 }\n"
"--     vidow.output{ name = \"DP-2\", scale = 2, x = 1920, y = 0,\n"
"--                   mode = \"3840x2160@144\", vrr = true, depth = 10 }\n"
"--     vidow.output{ name = \"HDMI-A-1\", enabled = false }\n"
"--\n"
"-- 能写的字段（只有 name 必写，其余不写就是「不管这一项」）：\n"
"--     name       连接器名（`vidowmsg outputs` 里那个，比如 DP-3）\n"
"--     enabled    这块屏开不开（false = 关掉，拔线那套记忆不受影响）\n"
"--     scale      \"auto\"（按 EDID 的 ppi 推）/ \"off\"（不缩放）/ 一个正数（2 = 两倍）\n"
"--     x, y       摆在布局的哪个逻辑坐标（写哪个算哪个；只写 x = 0 = 摆到最左边）\n"
"--     mode       \"3840x2160@144\" / \"3840x2160\"（刷新率不挑）/ \"…@59.94\"（小数也行）\n"
"--     vrr        true / false（自适应同步，屏不支持就忽略）\n"
"--     depth      位深，只能 8 或 10\n"
"--     transform  方向：normal / 90 / 180 / 270 / flipped / flipped-90 / flipped-180 /\n"
"--                flipped-270（也认 \"0\" 当 normal）\n"
"--\n"
"-- 模式有哪些先看 `vidowmsg outputs`。老写法（outputs / output_positions）已经\n"
"-- 没了，写了会在日志里喊一声。\n", f);
}

/* 16-env.lua 后面那一段：会话环境 / 自启动 / 事件钩子这三件事的 vidow.* 写法
 * （上面两个键只管「要不要对账」，怎么用是这些函数的事）。 */
static void print_env_lua(FILE *f) {
	fputs(
"\n"
"-- ================== 会话环境（和 systemd 用户管理器对账）\n"
"--\n"
"-- 从 vidow 里起的命令继承的是**进程环境**；由 systemd / dbus 拉起来的应用\n"
"-- （xdg-desktop-portal、systemd --user 的服务、桌面文件激活的程序）继承的是\n"
"-- **用户管理器里的环境**。两个不打通，就会出现「配置里起的程序连得上 vidow，\n"
"-- systemd 起的那个连的是宿主合成器」。所以两个方向都做，默认都开。\n"
"--\n"
"-- 推（vidow → systemd / dbus）：systemctl --user import-environment +\n"
"--   dbus-update-activation-environment --systemd，推 WAYLAND_DISPLAY / DISPLAY /\n"
"--   XDG_CURRENT_DESKTOP / XDG_SESSION_TYPE / XDG_SESSION_DESKTOP / VIDOW_SOCKET\n"
"--   以及下面 vidow.env{} 声明过的每一个；退出时把改过的还回去。\n"
"-- 吸（systemd → vidow）：启动时问一次用户管理器，把**我们自己没有的**吸进来\n"
"--   （PATH / SSH_AUTH_SOCK / environment.d 里那些）。\n"
"-- 够不着用户管理器（嵌套跑 / headless 测试里 XDG_RUNTIME_DIR 是临时的）就整个\n"
"--   跳过：那时只有走会话总线的那两条路「能改到」，改的是**宿主会话** —— 不碰。\n"
"--\n"
"-- vidow.env(\"名字\")                     读（没有就是 nil）\n"
"-- vidow.env(\"名字\", \"值\")               写（顺手推给 systemd）\n"
"-- vidow.env(\"名字\", false)              删\n"
"-- vidow.env{ A = \"1\", B = false }       一次一串\n"
"-- vidow.env_export()                    立刻推一轮（平时是自动的）\n"
"-- 改过的变量记着原值，vidow 退出时还原；`vidowmsg env` 把账本报出来。\n"
"--\n"
"-- ================== 自启动（Hyprland 的 exec-once / exec）\n"
"--\n"
"-- vidow.exec(\"命令\")                     等它跑完，拿 stdout（看着同步，不阻塞渲染）\n"
"-- vidow.exec(\"命令\", { status = true })   要退出码：返回 out, code, err\n"
"-- vidow.exec(\"waybar\", { wait = false })  撒手跑：不等、不要输出（起常驻程序用它）\n"
"-- vidow.exec_once(\"waybar\")              整个 vidow 生命周期只起一次（重载不重来）\n"
"-- vidow.exec_once{ \"swaybg -i 壁纸.png\", \"mako\" }\n"
"--\n"
"-- exec_once 的命令**先排队**，等这份配置跑完（环境、缩放、DISPLAY 都定了）再起。\n"
"-- 「起过了」按**命令原文**记：改过原文就是一条新命令，会再起一次；配置里删掉的会\n"
"-- 从名单里忘掉（写回来 = 新命令）。这三条起的子进程都继承 vidow 的完整环境。\n"
"--\n"
"-- ================== 事件钩子（Hyprland socket2 的等价物）\n"
"--\n"
"-- vidow.on(\"事件名\", \"命令\")            返回 id；事件来了跑这条 shell 命令\n"
"-- vidow.on(\"事件名\", function(e) … end) 返回 id；或者跑一段 Lua（里面能 await vidow.exec）\n"
"-- vidow.on(\"*\", { \"命令\", fn })         一次挂一串，返回 id 表\n"
"-- vidow.off(id)                         摘掉一条（id 就是 on() 返回的那个）\n"
"--\n"
"-- 命令钩子拿得到事件：VIDOW_EVENT / VIDOW_EVENT_TIME_MS / VIDOW_EVENT_JSON，\n"
"-- 载荷里每个标量字段再来一个 VIDOW_<字段名大写>（- 变 _）。Lua 钩子收到一张表\n"
"-- （e.event / e.time_ms / e.data）。事件名单（42 个）见 README 的事件表，\n"
"-- 或者 `vidowmsg events`；订阅同一份事件流也可以走 IPC。\n", f);
}

/* 14-keys.lua：键位 / 工作区 / 规则。没有配置键，全是 vidow.* 的调用。
 *
 * 这里给的是**一套默认键位** —— 合成器自己一个键位都不内置（原来 input.c 里有
 * 一张写死的键位表，现在那些都成了动作字符串，绑哪个键由这里说了算）。所以
 * 「把这几行删掉」= 那些键就还给客户端，这正是能改 / 能删的意思。 */
static void print_keys_lua(FILE *f) {
	fputs(
"-- ================== 键位（默认一套，随便改 / 删）\n"
"--\n"
"-- 合成器**一个键位都不内置**：下面这些是默认配置给的，删掉就还给客户端，换成\n"
"-- 别的键也行。动作字符串见 README 的「动作」表；`spawn` 后面跟一条 shell 命令。\n"
"--\n"
"-- 窗口与列（H/J/K/L 挪焦点，加 Shift 挪列 / 挪窗口本身）\n"
"vidow.bind(\"Mod+Return\", \"spawn \" .. (os.getenv(\"VIDOW_TERMINAL\") or \"kitty\"))\n"
"vidow.bind(\"Mod+Q\", \"close-window\")\n"
"vidow.bind(\"Mod+H\", \"focus-column left\")\n"
"vidow.bind(\"Mod+L\", \"focus-column right\")\n"
"vidow.bind(\"Mod+Shift+H\", \"move-column left\")\n"
"vidow.bind(\"Mod+Shift+L\", \"move-column right\")\n"
"vidow.bind(\"Mod+J\", \"focus-window down\")\n"
"vidow.bind(\"Mod+K\", \"focus-window up\")\n"
"vidow.bind(\"Mod+Shift+J\", \"move-window down\")\n"
"vidow.bind(\"Mod+Shift+K\", \"move-window up\")\n"
"vidow.bind(\"Mod+V\", \"toggle-floating\")\n"
"vidow.bind(\"Mod+Shift+Space\", \"toggle-floating\")\n"
"vidow.bind(\"Mod+F\", \"toggle-fullscreen\")\n"
"vidow.bind(\"Mod+Shift+F\", \"maximize-column\")\n"
"vidow.bind(\"Mod+R\", \"cycle-column-width\")\n"
"vidow.bind(\"Mod+M\", \"minimize-window\")\n"
"vidow.bind(\"Mod+Shift+M\", \"restore-window\")\n"
"vidow.bind(\"Mod+O\", \"toggle-overview\")\n"
"-- Alt+Tab：按住修饰键列出最近用过的窗口（一行预览），连点 Tab 换一个，松开就把\n"
"-- 焦点给选中的那个；Esc 取消。样式 / 排版见 configs/15-mru.lua。\n"
"vidow.bind(\"Alt+Tab\", \"next-window\")\n"
"vidow.bind(\"Alt+Shift+Tab\", \"previous-window\")\n"
"-- Alt+`（反引号）：只在**当前这个应用**的窗口之间切（niri 的 Alt+` 同此）\n"
"vidow.bind(\"Alt+grave\", \"next-window same-app\")\n"
"-- 第三个参数 { inhibited = true }：焦点那个表面抑制着快捷键（虚拟机的窗口要原样\n"
"-- 收 Super / Alt+Tab，见 README 的「快捷键抑制」）时这条照样生效。退出这条留个\n"
"-- 逃生口，不然抑制着的窗口能把用户锁死在里面。\n"
"vidow.bind(\"Mod+Escape\", \"quit\", { inhibited = true })\n"
"\n"
"-- 工作区（niri 那套：动态、按屏、按序号）\n"
"--\n"
"-- 每块屏各自一份纵向的工作区栈，序号 = 在这块屏上从上往下第几块。栈底永远是\n"
"-- 一块空的：窗口开在它上面，底下立刻再补一块；中间那些空的、没名字的，切走就\n"
"-- 自动消失。所以**不用预先建工作区**，直接切就行（序号越界会夹到最后一块）。\n"
"vidow.bind(\"Mod+U\", \"workspace down\")\n"
"vidow.bind(\"Mod+I\", \"workspace up\")\n"
"vidow.bind(\"Mod+Shift+U\", \"move down\")\n"
"vidow.bind(\"Mod+Shift+I\", \"move up\")\n"
"vidow.bind(\"Mod+Ctrl+U\", \"move-workspace down\")\n"
"vidow.bind(\"Mod+Ctrl+I\", \"move-workspace up\")\n"
"-- 数字键直跳（工作区是动态的：Mod+3 会夹到当前最后一块，用着用着就长出来了）\n"
"for i = 1, 9 do\n"
"\tvidow.bind(\"Mod+\" .. i, \"workspace \" .. i)\n"
"\tvidow.bind(\"Mod+Shift+\" .. i, \"move \" .. i)\n"
"end\n"
"\n"
"-- 切 tty（只有真机、有会话时才接管；嵌套 / headless 里这组键交给宿主）\n"
"for i = 1, 12 do\n"
"\tvidow.bind(\"Ctrl+Alt+F\" .. i, \"switch-vt \" .. i)\n"
"end\n"
"\n"
"-- 想要「永远在、空了也不消失」的工作区，就用具名工作区（niri 的 named\n"
"-- workspace），它在屏上的位置按声明顺序排，底部那块空的永远在最后：\n"
"--\n"
"--     vidow.workspace_create(\"chat\")             -- 建一块叫 chat 的（永远在）\n"
"--     vidow.bind(\"Mod+C\", \"workspace chat\")      -- 切过去\n"
"--     vidow.bind(\"Mod+Shift+C\", \"move chat\")     -- 把当前窗口搬进去\n"
"\n"
"-- 窗口规则 / 图层规则（vidow.rule{…}）：一条规则 = 匹配谁 + 怎么对待它\n"
"--\n"
"--   匹配字段（可叠加，一个都不写 = 匹配一切）。字符串字段是 **POSIX 扩展正则**：\n"
"--   不锚定（找到就算命中）、大小写不敏感，写普通字符串就是子串匹配；前面加 !\n"
"--   是取反（\"!^mpv$\" = 不是它）；整条规则写 literal = true 就退回纯子串匹配，\n"
"--   含 . * + ? [ ] ( ) { } | ^ $ \\ 的串记得转义（title = \"v1\\\\.0\"）。\n"
"--\n"
"--     窗口：app_id（Wayland 的 app_id / X11 的 WM_CLASS class 段）\n"
"--           title（当前标题）/ initial_title（上屏那一刻的标题 —— 浏览器、终端\n"
"--                 会改标题，认「刚开的那一屏」用它）\n"
"--           class / instance（X11 的 WM_CLASS 两段）/ role（X11 的 WM_WINDOW_ROLE）\n"
"--           initial_app_id / initial_class / initial_instance（上屏那一刻的上面几个）\n"
"--           on_output（现在在哪块屏）/ on_workspace（现在在哪块工作区）\n"
"--     两边：pid（进程号）/ exe（可执行文件名）/ cmdline（命令行）—— 没有\n"
"--           app_id 可认的应用（老 X11 程序、包一层启动脚本的）按进程认\n"
"--     状态（is_ 开头的是「现在是不是这样」，作用字段没有前缀）：\n"
"--           is_xwayland / is_popup（菜单、提示这类弹出物）/ is_floating /\n"
"--           is_fullscreen / is_maximized（所在列整列最大化）/ is_minimized /\n"
"--           is_fixed_size（客户端声明了固定尺寸，微信登录窗那种）\n"
"--     图层：namespace（waybar / mako / notifications …）/ layer（background /\n"
"--           bottom / top / overlay）—— 写了这两个里任何一个就是图层规则，\n"
"--           窗口字段和图层字段写进同一条规则 = 这条规则谁也不匹配（当场看得见）\n"
"--\n"
"--   作用（**至少写一个**，都不写会在这一行报错；没写的字段用默认值，同一个字段\n"
"--   以最后一条写了它的规则为准）。一直在的：\n"
"--     force_render = true        滚出视口 / 被藏起来 / 最小化了也照样给它补帧\n"
"--                                （录屏、窗口预览要的是新画面；每秒几帧看\n"
"--                                 configs/06-render.lua 的 force_render_hz）\n"
"--     hz = 30                    只给这个窗口定补帧频率（0 = 不限）\n"
"--     focus_on_open = false      它开窗的时候不抢焦点（Electron 系那种看不见的\n"
"--                                辅助窗口会开个窗口把自己顶上来）\n"
"--     focus_on_activate = false  别人请求激活它（点通知 / 任务栏按钮）时不理\n"
"--     honor_invalid_serial = true  它的激活请求即使令牌 serial 校验不过也照样理\n"
"--     x11_scale = 1              X11 缩放桥：1 = 这个客户端没按 GDK_SCALE 画，\n"
"--                                缓冲区不除（只对 X11 窗口有意义）\n"
"--     capture = \"hide\"           截图 / 录屏里怎么出现：\"off\"（默认，照常）/\n"
"--                                \"hide\"（不出现）/ \"mosaic\"（打马赛克）\n"
"--     capture_mosaic = 24        马赛克的块边长（屏幕像素，默认 16，最小 2）；只在\n"
"--                                capture = \"mosaic\" 时有用，单独写不算一个作用\n"
"--   外观（装饰着色器的参数；没写的用全局配置，全局开关仍是总闸）：\n"
"--     decor = false              这个窗口不画装饰（边框 / 圆角 / 阴影都不要）\n"
"--     opacity = 0.9              整块（含装饰）的不透明度\n"
"--     corner_radius = 12         圆角半径（逻辑像素）\n"
"--     border_width = 0           边框宽度（0 = 不画边框）\n"
"--     border_color = \"ff0000\"    没聚焦时的边框颜色（RRGGBB / RRGGBBAA）\n"
"--     border_color_focus = \"00ff00\"  聚焦时的边框颜色\n"
"--     shadow_radius = 20         阴影半径（0 = 不画阴影）\n"
"--     shadow_color = \"00000080\"  阴影颜色，alpha 管浓淡\n"
"--     shadow_offset_x = 0        阴影横向偏移（像素）\n"
"--     shadow_offset_y = 4        阴影纵向偏移\n"
"--     noise = 0.1                往装饰里掺的噪点强度（0 = 不加）\n"
"--     blur = false               它背后不模糊（true = 平铺窗口也模糊）\n"
"--     blur_xray = true           重叠时也用整屏那份底衬（糊的是壁纸）\n"
"\n"
"--     anim = false               这个窗口不做开合动画（开、关都瞬间到位）\n"
"--   开窗时兑现一次（重载规则**不**搬已经开着的窗口，也不改它们的尺寸）：\n"
"--     float = true               强制浮动（false = 强制平铺；对话框那类自动识别让位）\n"
"--     workspace = \"chat\"         开在哪块工作区：具名工作区（没有就建）/ 特殊工作区\n"
"--                                （\"special:名字\"）/ 序号（\"2\"）\n"
"--     output = \"DP-1\"            开在哪块屏（那块屏现在显示着的工作区）\n"
"--     width = 800                浮动窗口开多宽（height 同理；逻辑像素）\n"
"--     center = true              浮动窗口开在屏幕中间\n"
"--     fullscreen = true          开出来就全屏\n"
"--     maximize = true            开出来就整列最大化\n"
"--     minimize = true            开出来就收起来（不显示、不抢焦点）\n"
"--     column_width = 0.3         这个窗口新开的列有多宽（占可用宽度的比例，默认 0.5）\n"
"--\n"
"--   vidow.rule{ app_id = \"^mpv$\", force_render = true }\n"
"--   vidow.rule{ title = \"^预览\", force_render = true, hz = 15 }\n"
"--   vidow.rule{ class = \"Steam\", focus_on_open = false }\n"
"--   vidow.rule{ namespace = \"^notifications$\", capture = \"hide\" }\n"
"--   vidow.rule{ exe = \"^wechat$\", x11_scale = 1 }     -- 没有 app_id 的应用按进程认\n"
"--   vidow.rule{ app_id = \"^mpv$\", workspace = \"media\", fullscreen = true }\n"
"--   读回来：vidow.rules()（没管的字段是 nil：「没管它」和「管了、给了默认值」\n"
"--   是两回事）；字段名写错了当场报错，并列出所有能写的字段名\n"
"\n"
"\n"
"\n"
"-- vidow.* 的函数（配置就是一门 Lua，下面这些是 vidow 注册进去的，全在这一份里）\n"
"--\n"
"--     配置键：\n"
"--         vidow.get(\"gaps\")                     读一个键（和 vidowmsg config 同一个值）\n"
"--         vidow.set(\"gaps\", 12)                 运行时改一个键（不落盘）。写法同配置键：\n"
"--                                                vidow.set(\"border\", { width = 0 }) 也认\n"
"--         vidow.include(\"configs/02-decor.lua\")  就地读另一个文件（相对路径按当前文件\n"
"--                                                所在目录解析）\n"
"--     窗口规则：\n"
"--         vidow.rule{ … }                       加一条规则（字段见上）\n"
"--         vidow.rules()                         读回所有规则\n"
"--     工作区：\n"
"--         vidow.workspace_create(\"chat\")         建一块具名工作区（永远在；第二个参数\n"
"--                                                指定哪块屏，比如 \"DP-1\"）\n"
"--         vidow.workspace_special(\"scratch\")    建一块特殊工作区（跟焦点屏走）\n"
"--         vidow.workspace_list()                列出普通工作区（特殊工作区不在里面）\n"
"--         vidow.workspace_active()              当前这块屏的活动工作区（名字）\n"
"--         vidow.workspace_switch(\"chat\")         切到那块具名工作区（无名工作区用动作串\n"
"--                                                \"workspace 2\"）\n"
"--         vidow.workspace_toggle_special(\"scratch\")  开 / 关那块特殊工作区\n"
"--         vidow.workspace_move(\"chat\")           把当前窗口搬到那块具名工作区\n"
"--     输出：\n"
"--         vidow.output{ … }                     声明一块屏（字段见 configs/09-output.lua）\n"
"--     跑命令与日志：\n"
"--         vidow.exec(\"命令\")                     跑一条 shell 命令，拿它的 stdout（看着是同步\n"
"--                                                的，实际不阻塞渲染）；失败返回 nil, 原因\n"
"--         vidow.exec(\"命令\", { status = true })   要退出码时用它：返回 out, code, err\n"
"--         vidow.log(\"消息\")                     往合成器日志打一行（VIDOW_LOG=debug 看得见）\n"
"--         vidow.ipc_path()                      当前 IPC socket 的路径（没开 IPC 时是 nil）\n"
"\n"
"-- 指针手势（Mod+左键拖窗口 / Mod+右键缩放 / Mod+滚轮切列）不是键位，改不了键。\n", f);
}

/* 一份分类文件的正文（不含文件头）。 */
static void print_section_body(FILE *f, const struct vidow_config *cfg,
		int section) {
	if (section == SECTION_KEYS) {
		print_keys_lua(f);
		return;
	}
	print_section_keys(f, cfg, section);
	if (section == SECTION_OUTPUT) {
		print_output_hint(f);
	}
	if (section == SECTION_ENV) {
		print_env_lua(f);
	}
}

/* 一份分类文件（`vidow --default-config 02-decor.lua` 打出来的那份）。 */
static void print_section_file(FILE *f, const struct vidow_config *cfg,
		int section) {
	print_section_header(f, section);
	print_section_body(f, cfg, section);
}

/* 入口 config.lua：只写「哪一类配置在哪个文件里」。 */
void vidow_config_default_lua(FILE *out) {
	FILE *f = out != NULL ? out : stdout;

	fputs(
"-- vidow 的配置入口 —— 下面每一行 include 把一类配置读进来，就这一件事。\n"
"--\n"
"-- 一类一个文件，都在 configs/ 下。相对路径按**这个文件所在目录**解析（跟 C 的\n"
"-- #include \"…\" 一个意思），所以在 configs/ 里的文件互相 include 也一样好使。\n"
"-- include 的先后 = 键生效的先后（同一个键只有第一次写的算数），别打乱。\n"
"-- 想删掉某一类：把那一行 include 删掉就行（文件留着不碍事）。\n"
"--\n"
"-- 整份默认配置（每行都是内置默认值）：\n"
"--     vidow --default-config                 这个入口\n"
"--     vidow --default-config all             全部分类文件拼在一起（能当单文件配置用）\n"
"--     vidow --default-config 02-decor.lua    其中一份（也认 --default-config decor）\n"
"--     vidow --write-config                   一步写到配置目录（入口 + configs/）\n"
"--\n"
"-- 优先级：环境变量 VIDOW_<键名大写> > 配置文件 > 内置默认值。\n"
"-- 改哪一份都行，改完不用重启（config.lua 和 include 进来的文件都盯着）：\n"
"--     kill -HUP $(pgrep -x vidow)\n"
"-- 运行时改：vidowmsg set-config <键> <值>（不落盘）。\n"
"\n", f);

	for (int i = 0; i < N_SECTIONS; i++) {
		/* 注释对齐一列：最长的一行是 vidow.include("configs/07-clipboard.lua")
		 * （41 个字符）。 */
		char line[64];
		snprintf(line, sizeof(line), "vidow.include(\"configs/%s\")",
			sections[i].file);
		fprintf(f, "%-43s-- %s\n", line, sections[i].title);
	}
}

/* 全部分类文件拼在一起（`vidow --default-config all`）。include 就是就地展开，
 * 所以这一份本身也是能直接用的单文件配置：
 *     vidow --default-config all > ~/.config/vidow/config.lua */
void vidow_config_default_all_lua(FILE *out) {
	struct vidow_config cfg;
	defaults(&cfg);
	FILE *f = out != NULL ? out : stdout;

	fputs(
"-- vidow 的完整默认配置：入口 config.lua + configs/ 下的每一份拼在一起\n"
"-- （vidow.include() 就是就地展开，所以这份能当单文件配置直接用）。\n"
"-- 每个键都写着内置默认值：改哪行哪行才生效，删掉那行就回到默认值。\n"
"-- 优先级：环境变量 VIDOW_<键名大写> > 配置文件 > 内置默认值。\n", f);

	for (int i = 0; i < N_SECTIONS; i++) {
		fprintf(f,
			"\n-- ============================================================\n"
			"-- configs/%s\n"
			"-- ============================================================\n",
			sections[i].file);
		print_section_body(f, &cfg, i);
	}
}

/* 分类文件有几个 / 第几个叫什么（--help 和报错时列名字用）。 */
int vidow_config_section_count(void) {
	return N_SECTIONS;
}

const char *vidow_config_section_file(int index) {
	return index >= 0 && index < N_SECTIONS ? sections[index].file : NULL;
}

/* `--default-config <名字>`：认 "02-decor.lua"，也认 "decor"（省掉数字前缀和
 * .lua 后缀）。认不出来返回 -1。 */
int vidow_config_section_index(const char *name) {
	if (name == NULL || name[0] == '\0') {
		return -1;
	}
	for (int i = 0; i < N_SECTIONS; i++) {
		if (strcmp(sections[i].file, name) == 0) {
			return i;
		}
	}
	/* 省掉数字前缀（01-）和 .lua 后缀的写法：--default-config decor 也认。 */
	const char *base = name;
	if (isdigit((unsigned char)base[0]) && isdigit((unsigned char)base[1]) &&
			base[2] == '-') {
		base += 3;
	}
	size_t len = strlen(base);
	if (len > 4 && strcmp(base + len - 4, ".lua") == 0) {
		len -= 4;
	}
	for (int i = 0; i < N_SECTIONS; i++) {
		const char *f = sections[i].file;
		if (isdigit((unsigned char)f[0]) && isdigit((unsigned char)f[1]) &&
				f[2] == '-') {
			f += 3;
		}
		size_t flen = strlen(f);
		if (flen > 4 && strcmp(f + flen - 4, ".lua") == 0) {
			flen -= 4;
		}
		if (flen == len && strncmp(f, base, len) == 0) {
			return i;
		}
	}
	return -1;
}

/* 打印一份分类文件（`vidow --default-config <名字>`）。名字不认识返回 false。 */
bool vidow_config_default_section_lua(FILE *out, const char *name) {
	int section = vidow_config_section_index(name);
	if (section < 0) {
		return false;
	}
	struct vidow_config cfg;
	defaults(&cfg);
	print_section_file(out != NULL ? out : stdout, &cfg, section);
	return true;
}

/* mkdir -p：配置目录第一次用的时候通常整条路径都不在（~/.config 也在）。
 * 逐级建，已经存在的那几级不算错。 */
static int mkdir_parents(const char *dir) {
	char buf[PATH_MAX];
	snprintf(buf, sizeof(buf), "%s", dir);
	for (char *p = buf + 1; *p != '\0'; p++) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
			return -1;
		}
		*p = '/';
	}
	if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
		return -1;
	}
	return 0;
}

/* 把默认配置落到磁盘（`vidow --write-config`）：入口 + configs/ 下的每一份。
 *
 * 为什么要有：默认值只有 --default-config 打在屏幕上，想改一格就得先把整份
 * 重定向进文件（还得自己 mkdir 配置目录）。这里一步到位，并且**不覆盖已有的**
 * —— 配置目录里那份是用户改过的东西，覆盖它等于把人家的设置扔了。
 *
 * 入口已经有一份就整个不写（非 0 退出）；configs/ 里已经有的那几份跳过、只补
 * 缺的。要全部覆盖加 --force。写出来 / 跳过了哪些，都打在 report 上。 */
int vidow_config_write_default(const char *path, bool force, FILE *report,
		char *err, size_t err_len) {
	if (path == NULL || path[0] == '\0') {
		snprintf(err, err_len, "配置路径是空的");
		return -1;
	}

	if (!force && access(path, F_OK) == 0) {
		snprintf(err, err_len, "%s 已经有一份了（要覆盖就加 --force）", path);
		return -1;
	}

	/* 父目录不存在就建（配置目录第一次用的时候通常还没有）。 */
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash != NULL && slash != dir) {
		*slash = '\0';
		if (mkdir_parents(dir) != 0) {
			snprintf(err, err_len, "建不了目录 %s：%s", dir,
				strerror(errno));
			return -1;
		}
	} else {
		snprintf(dir, sizeof(dir), ".");
	}
	char cfgdir[PATH_MAX + 8]; /* dir 最多 PATH_MAX-1 个字符 + "/configs" */
	snprintf(cfgdir, sizeof(cfgdir), "%s/configs", dir);
	if (mkdir_parents(cfgdir) != 0) {
		snprintf(err, err_len, "建不了目录 %s：%s", cfgdir,
			strerror(errno));
		return -1;
	}

	struct vidow_config cfg;
	defaults(&cfg);

	FILE *f = fopen(path, "w");
	if (f == NULL) {
		snprintf(err, err_len, "写不了 %s：%s", path, strerror(errno));
		return -1;
	}
	vidow_config_default_lua(f);
	if (fclose(f) != 0) {
		snprintf(err, err_len, "写 %s 时出错：%s", path, strerror(errno));
		return -1;
	}
	if (report != NULL) {
		fprintf(report, "%s\n", path);
	}

	char file[PATH_MAX + 8 + 32];
	for (int i = 0; i < N_SECTIONS; i++) {
		/* 文件名是这张表里写死的（最长 14 个字符），不会截断。 */
		if (snprintf(file, sizeof(file), "%s/%s", cfgdir,
				sections[i].file) >= (int)sizeof(file)) {
			snprintf(err, err_len, "路径太长了：%s/%s", cfgdir,
				sections[i].file);
			return -1;
		}
		if (!force && access(file, F_OK) == 0) {
			if (report != NULL) {
				fprintf(report, "%s 已经有了，没动\n", file);
			}
			continue;
		}
		FILE *sf = fopen(file, "w");
		if (sf == NULL) {
			snprintf(err, err_len, "写不了 %s：%s", file,
				strerror(errno));
			return -1;
		}
		print_section_file(sf, &cfg, i);
		if (fclose(sf) != 0) {
			snprintf(err, err_len, "写 %s 时出错：%s", file,
				strerror(errno));
			return -1;
		}
		if (report != NULL) {
			fprintf(report, "%s\n", file);
		}
	}
	return 0;
}
