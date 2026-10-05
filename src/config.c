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
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <wlr/util/log.h>

enum kind {
	K_INT,
	K_DOUBLE,
	K_BOOL,
	K_EASING,
	K_STRING,
	K_COLOR, /* "RRGGBB" or "RRGGBBAA", straight (non-premultiplied) alpha */
};

struct entry {
	const char *key;
	enum kind kind;
	size_t offset;
	size_t size;
	int section;      /* 打印默认配置时的分节，见 section_titles[] */
	const char *doc;  /* 打印在那行配置上面的注释（不能有换行） */
};

/* 默认配置的分节标题。entries[] 按这个顺序排，打印时换节就插一个标题。 */
static const char *section_titles[] = {
	"间距（gap）",
	"窗口装饰",
	"背景模糊",
	"开关动画",
	"自定义着色器",
	"刷新与补帧",
	"剪贴板",
	"IPC",
	"缩放与输出",
	"XWayland",
	"窗口激活",
	"xdg-desktop-portal",
};
#define N_SECTIONS (sizeof(section_titles) / sizeof(section_titles[0]))

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
	{ "blur_levels",     K_INT,    OFF(blur_levels),     SZ(blur_levels), 2,
		"Kawase 模糊的层级数，越大越糊（也更费 GPU）" },
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
		"滚出视口的窗口每秒补多少帧（0 = 不限），配合窗口规则的 force_render" },

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

	{ "scale",           K_STRING, OFF(scale),           SZ(scale), 8,
		"缩放：auto = 按 EDID 的 ppi 推，也可以写死 \"1\" / \"1.5\"" },
	{ "outputs",         K_STRING, OFF(outputs),         SZ(outputs), 8,
		"按输出名覆盖缩放（如 \"DP-1:1.5,HDMI-A-1:1\"），留空 = 用 scale" },

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
static const struct group_field portal_fields[] = {
	{ "conf_sync", "portal_conf_sync" },
	{ "desktop",   "portal_desktop" },
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

static bool parse_easing(const char *value, enum vidow_easing *out) {
	for (size_t i = 0; i < VIDOW_EASE_OUT_ELASTIC + 1; i++) {
		if (strcmp(value, easing_names[i]) == 0) {
			*out = (enum vidow_easing)i;
			return true;
		}
	}
	return false;
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
		case K_STRING:
			snprintf((char *)field, e->size, "%s", value);
			return true;
		case K_COLOR: {
			const char *hex = value[0] == '#' ? value + 1 : value;
			size_t len = strlen(hex);
			if (len != 6 && len != 8) {
				wlr_log(WLR_ERROR, "config: colour '%s' must be RRGGBB or RRGGBBAA",
					value);
				return false;
			}
			char part[3] = {0};
			float *out = field;
			for (int i = 0; i < 4; i++) {
				if (i == 3 && len == 6) {
					out[3] = 1.0f;
					break;
				}
				memcpy(part, hex + i * 2, 2);
				out[i] = (float)strtol(part, NULL, 16) / 255.0f;
			}
			return true;
		}
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
	cfg->blur_levels = 4;
	cfg->blur_strength = 1.0;
	cfg->blur_alpha_min = 0.15;
	cfg->blur_offset = 1.5;

	cfg->shader_enabled = true;
	cfg->shader_open[0] = '\0';
	cfg->shader_close[0] = '\0';
	cfg->shader_idle[0] = '\0';
	cfg->shader_flip_y = false;

	/* 缩放：默认按 EDID 的 ppi 推，让同一个逻辑尺寸在两块不同 DPI 的屏上是
	 * 同一个物理尺寸；outputs 里可以按输出名覆盖。 */
	snprintf(cfg->scale, sizeof(cfg->scale), "auto");
	cfg->outputs[0] = '\0';

	/* XWayland：默认懒启动（有人连 X 才起服务）。xwayland_scale=auto 取所有
	 * 输出缩放的最大值 —— X 只有一套坐标，只能按最密的那块屏来，低了就是放大糊。 */
	cfg->xwayland = true;
	snprintf(cfg->xwayland_scale, sizeof(cfg->xwayland_scale), "auto");
	cfg->xwayland_env = true;

	/* portal：vidow 当会话合成器时把 XDG_CURRENT_DESKTOP 设成 Vidow，
	 * 于是 xdg-desktop-portal 读的是 vidow-portals.conf。 */
	cfg->portal_conf_sync = true;
	snprintf(cfg->portal_desktop, sizeof(cfg->portal_desktop), "Vidow");

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
	wlr_log(WLR_INFO, "config: clipboard=%s primary=%s max=%d 字节",
		cfg->clipboard ? "on" : "off",
		cfg->clipboard_primary ? "on" : "off", cfg->clipboard_max_bytes);
	wlr_log(WLR_INFO,
		"config: scale=%s outputs=%s xwayland=%s xscale=%s xenv=%s",
		cfg->scale, cfg->outputs[0] != '\0' ? cfg->outputs : "-",
		cfg->xwayland ? "on" : "off", cfg->xwayland_scale,
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
	if (kind != K_STRING && kind != K_COLOR && kind != K_EASING) {
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

/* 打印一份完整的默认配置（`vidow --default-config`）。每个键都写着它的默认
 * 值：抄下来就等于什么都没改，改哪行哪行才生效。不读环境变量 —— 打出来的
 * 是「什么都没配」时的样子。 */
void vidow_config_default_lua(FILE *out) {
	struct vidow_config cfg;
	defaults(&cfg);
	FILE *f = out != NULL ? out : stdout;

	fputs(
"-- vidow 的完整默认配置（`vidow --default-config` 打印出来的一份）\n"
"--\n"
"-- 每行都是内置默认值：整份抄到 ~/.config/vidow/config.lua 就等于什么都没改。\n"
"-- 文件里写了的键会一直盖住内置默认值 —— 想让 vidow 以后改进的默认值生效，\n"
"-- 就把那一行删掉（或者注释掉）。\n"
"--\n"
"-- 一族配置可以写成一张表，只盖其中几项，比如：\n"
"--     gaps   = 10\n"
"--     gaps   = { top = 30, inner = 20 }       -- 上边距 30、窗口之间 20、其余 6\n"
"--     border = { width = 0, color = \"#2a2e3aff\" }\n"
"-- 表里的项名写在下面每一节里；平铺键（gaps_inner = 20）和表写法是一回事。\n"
"--\n"
"-- 优先级：环境变量 VIDOW_<键名大写> > 这个文件 > 内置默认值。\n"
"-- 同一个键在文件里写两遍只有第一遍生效（第一遍之后它就是个普通全局变量了）。\n"
"-- 运行时改：vidowmsg set-config <键> <值>（不落盘）。\n"
"-- 改完这个文件不用重启：  kill -HUP $(pgrep -x vidow)\n", f);

	int section = -1;
	for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		const struct entry *e = &entries[i];
		if (e->section != section) {
			if (section >= 0) {
				print_section_groups(f, &cfg, section);
			}
			section = e->section;
			fprintf(f, "\n-- ================== %s\n",
				section < (int)N_SECTIONS ? section_titles[section]
					: "其他");
		}
		const char *derived = derived_note(e->key);
		if (derived != NULL) {
			fprintf(f, "-- %s\n-- %s\n-- %s = \"<配置目录>/shaders\""
				"   ← 要写死就自己换成绝对路径\n",
				e->doc != NULL ? e->doc : "", derived, e->key);
			continue;
		}
		fprintf(f, "-- %s\n%s = ", e->doc != NULL ? e->doc : "", e->key);
		print_lua_value(f, &cfg, e->key, e->kind);
		fputc('\n', f);
	}
	if (section >= 0) {
		print_section_groups(f, &cfg, section);
	}

	fputs(
"\n"
"-- ================== 工作区（niri 那套：动态、按屏、按序号）\n"
"--\n"
"-- 每块屏各自一份纵向的工作区栈，序号 = 在这块屏上从上往下第几块。栈底永远是\n"
"-- 一块空的：窗口开在它上面，底下立刻再补一块；中间那些空的、没名字的，切走就\n"
"-- 自动消失。所以**不用预先建工作区**，直接切就行（序号越界会夹到最后一块）。\n"
"--\n"
"-- 想要「永远在、空了也不消失」的工作区，就用具名工作区（niri 的 named\n"
"-- workspace），它在屏上的位置按声明顺序排，底部那块空的永远在最后：\n"
"--\n"
"--     vidow.workspace_create(\"chat\")             -- 建一块叫 chat 的（永远在）\n"
"--     vidow.bind(\"Mod+C\", \"workspace chat\")      -- 切过去\n"
"--     vidow.bind(\"Mod+Shift+C\", \"move chat\")     -- 把当前窗口搬进去\n"
"--\n"
"-- 内置键位：Mod+U / Mod+I 上下切工作区，Mod+Shift+U/I 把窗口搬到上/下一块，\n"
"-- Mod+Ctrl+U/I 整块工作区上下重排。下面 18 行是「数字键直跳」的绑定，不想要\n"
"-- 就删掉（工作区是动态的，Mod+3 会夹到当前最后一块，用着用着就长出来了）。\n"
"for i = 1, 9 do\n"
"\tvidow.bind(\"Mod+\" .. i, \"workspace \" .. i)\n"
"\tvidow.bind(\"Mod+Shift+\" .. i, \"move \" .. i)\n"
"end\n", f);
}
