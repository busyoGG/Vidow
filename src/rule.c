#include "rule.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <wlr/util/log.h>

#include "server.h"
#include "workspace.h"

/* ------------------------------------------------------------ 字段表
 *
 * 这里是「规则的词汇表」：Lua 那边（script.c 的 l_rule / l_rules）、IPC 那边
 * （ipcstate.c 的 rules）和套用逻辑（本文件）都只认这张表 —— 加一个字段就加
 * 一行，不用在三处各写一遍。 */

const struct vidow_rule_field vidow_rule_fields[] = {
	/* ---- 匹配：字符串（POSIX 扩展正则，不锚定 = 子串语义，大小写不敏感） */
	{ "app_id", RK_STR, VRS_APP_ID, VIDOW_RULE_WINDOW,
		"Wayland 的 app_id / X11 的 WM_CLASS class 段" },
	{ "title", RK_STR, VRS_TITLE, VIDOW_RULE_WINDOW,
		"当前标题（客户端会改它：浏览器、终端都是）" },
	{ "class", RK_STR, VRS_CLASS, VIDOW_RULE_WINDOW,
		"X11 的 WM_CLASS class 段（Wayland 窗口没有）" },
	{ "instance", RK_STR, VRS_INSTANCE, VIDOW_RULE_WINDOW,
		"X11 的 WM_CLASS instance 段（Wayland 窗口没有）" },
	{ "role", RK_STR, VRS_ROLE, VIDOW_RULE_WINDOW,
		"X11 的 WM_WINDOW_ROLE" },
	{ "initial_app_id", RK_STR, VRS_INITIAL_APP_ID, VIDOW_RULE_WINDOW,
		"上屏那一刻的 app_id（之后客户端改标题不影响它）" },
	{ "initial_title", RK_STR, VRS_INITIAL_TITLE, VIDOW_RULE_WINDOW,
		"上屏那一刻的标题 —— 认「这个应用刚开的那一屏」用它" },
	{ "initial_class", RK_STR, VRS_INITIAL_CLASS, VIDOW_RULE_WINDOW,
		"上屏那一刻的 WM_CLASS class 段" },
	{ "initial_instance", RK_STR, VRS_INITIAL_INSTANCE, VIDOW_RULE_WINDOW,
		"上屏那一刻的 WM_CLASS instance 段" },
	{ "exe", RK_STR, VRS_EXE, VIDOW_RULE_WINDOW | VIDOW_RULE_LAYER,
		"客户端可执行文件名（/proc/<pid>/exe 的最后一段）" },
	{ "cmdline", RK_STR, VRS_CMDLINE, VIDOW_RULE_WINDOW | VIDOW_RULE_LAYER,
		"客户端命令行（/proc/<pid>/cmdline，参数用空格接）" },
	{ "namespace", RK_STR, VRS_NAMESPACE, VIDOW_RULE_LAYER,
		"layer surface 的 namespace（waybar / mako / notifications …）" },
	{ "layer", RK_STR, VRS_LAYER, VIDOW_RULE_LAYER,
		"layer 的层：background / bottom / top / overlay" },
	{ "on_output", RK_STR, VRS_ON_OUTPUT, VIDOW_RULE_WINDOW | VIDOW_RULE_LAYER,
		"现在摆在哪块屏上（输出名，如 DP-1）" },
	{ "on_workspace", RK_STR, VRS_ON_WORKSPACE, VIDOW_RULE_WINDOW,
		"现在属于哪块工作区（名字）" },

	/* ---- 匹配：布尔 */
	{ "is_xwayland", RK_BOOL, VRB_XWAYLAND, VIDOW_RULE_WINDOW,
		"是 X11（XWayland）窗口" },
	{ "is_popup", RK_BOOL, VRB_POPUP, VIDOW_RULE_WINDOW,
		"菜单 / 提示 / 下拉框这类弹出物（X11 的 override-redirect）" },
	{ "is_floating", RK_BOOL, VRB_FLOATING, VIDOW_RULE_WINDOW,
		"现在是浮动窗口" },
	{ "is_fullscreen", RK_BOOL, VRB_FULLSCREEN, VIDOW_RULE_WINDOW,
		"现在全屏" },
	{ "is_maximized", RK_BOOL, VRB_MAXIMIZED, VIDOW_RULE_WINDOW,
		"所在列正整列最大化" },
	{ "is_minimized", RK_BOOL, VRB_MINIMIZED, VIDOW_RULE_WINDOW,
		"收起来了（还在列里，但不显示）" },
	{ "is_fixed_size", RK_BOOL, VRB_FIXED_SIZE, VIDOW_RULE_WINDOW,
		"客户端声明了固定尺寸（min == max），微信登录窗那种" },

	/* ---- 匹配：数值 */
	{ "pid", RK_NUM, VRN_PID, VIDOW_RULE_WINDOW | VIDOW_RULE_LAYER,
		"客户端进程号（精确相等）" },

	/* ---- 作用 */
	{ "force_render", RK_ACT_BOOL, VRA_FORCE_RENDER, VIDOW_RULE_WINDOW,
		"滚出视口 / 被藏起来 / 最小化了也照样补帧" },
	{ "hz", RK_ACT_INT, VRA_HZ, VIDOW_RULE_WINDOW,
		"只给这个窗口定补帧频率（0 = 不限）" },
	{ "focus_on_open", RK_ACT_BOOL, VRA_FOCUS_ON_OPEN, VIDOW_RULE_WINDOW,
		"它开新窗口时抢不抢焦点（默认 true）" },
	{ "focus_on_activate", RK_ACT_BOOL, VRA_FOCUS_ON_ACTIVATE, VIDOW_RULE_WINDOW,
		"别人请求激活它时理不理（默认 true）" },
	{ "fake_focus", RK_ACT_BOOL, VRA_FAKE_FOCUS, VIDOW_RULE_WINDOW,
		"焦点移走时也让客户端以为它还处于激活状态（xdg activated / X11 "
		"focused）—— 给那种一失焦就暂停 / 收 UI 的程序用" },
	{ "honor_invalid_serial", RK_ACT_BOOL, VRA_HONOR_INVALID_SERIAL,
		VIDOW_RULE_WINDOW,
		"它的激活请求即使令牌 serial 校验不过也照样理（默认 false）" },
	{ "x11_scale", RK_ACT_DOUBLE, VRA_X11_SCALE, VIDOW_RULE_WINDOW,
		"X11 缩放桥：1 = 这个客户端没按 GDK_SCALE 画，缓冲区不除" },
	{ "capture", RK_ACT_CAPTURE, VRA_CAPTURE,
		VIDOW_RULE_WINDOW | VIDOW_RULE_LAYER,
		"截图 / 录屏里怎么出现：\"off\"（照常）/ \"hide\"（不出现）/ "
		"\"mosaic\"（打马赛克）" },
	{ "capture_mosaic", RK_ACT_INT, VRA_CAPTURE_MOSAIC,
		VIDOW_RULE_WINDOW | VIDOW_RULE_LAYER,
		"马赛克的块边长（屏幕像素，默认 16，最小 2）；只在 capture = "
		"\"mosaic\" 时有用，单独写不算一个作用" },
	{ "overview", RK_ACT_OVERVIEW, VRA_OVERVIEW, VIDOW_RULE_LAYER,
		"进全景的时候露不露：\"always\"（默认，照常）/ \"only\"（只在全景里"
		"显示）/ \"never\"（全景里不出现）。全景用另一张壁纸靠它 —— 跑第二个"
		"壁纸客户端（比如 awww-daemon -n ov，namespace 就成了 awww-daemonov）"
		"再按 namespace 匹配，写 \"only\"" },
	/* 一直在的：这个窗口要 VRR（niri 的 variable-refresh-rate）。 */
	{ "vrr", RK_ACT_BOOL, VRA_VRR, VIDOW_RULE_WINDOW,
		"true = 这个窗口显示在「按需 VRR」的屏上时把那块屏的刷新率提上去"
		"（niri 的 variable-refresh-rate；屏那边写 vrr_on_demand = true 才有效）" },
	/* 开窗时兑现一次的 */
	{ "float", RK_ACT_BOOL, VRA_FLOAT, VIDOW_RULE_WINDOW,
		"true = 强制浮动（对话框那类本来就会浮的不用写），false = 强制平铺" },
	{ "workspace", RK_ACT_STR, VRA_WORKSPACE, VIDOW_RULE_WINDOW,
		"开在哪块工作区：具名工作区（没有就建）、特殊工作区（\"special:名字\" "
		"或直接写名字）、序号（\"2\"）" },
	{ "output", RK_ACT_STR, VRA_OUTPUT, VIDOW_RULE_WINDOW,
		"开在哪块屏（那块屏现在显示着的工作区）" },
	{ "width", RK_ACT_INT, VRA_WIDTH, VIDOW_RULE_WINDOW,
		"浮动窗口的宽（逻辑像素；不写 = 客户端自己定）" },
	{ "height", RK_ACT_INT, VRA_HEIGHT, VIDOW_RULE_WINDOW,
		"浮动窗口的高（逻辑像素；不写 = 客户端自己定）" },
	{ "center", RK_ACT_BOOL, VRA_CENTER, VIDOW_RULE_WINDOW,
		"浮动窗口开在屏幕中间" },
	{ "fullscreen", RK_ACT_BOOL, VRA_FULLSCREEN, VIDOW_RULE_WINDOW,
		"开出来就全屏" },
	{ "maximize", RK_ACT_BOOL, VRA_MAXIMIZE, VIDOW_RULE_WINDOW,
		"开出来就整列最大化" },
	{ "minimize", RK_ACT_BOOL, VRA_MINIMIZE, VIDOW_RULE_WINDOW,
		"开出来就收起来（留在列里，但不显示、不抢焦点）" },
	{ "column_width", RK_ACT_DOUBLE, VRA_COLUMN_WIDTH, VIDOW_RULE_WINDOW,
		"这个窗口新开的列有多宽（占可用宽度的比例，0.1 ~ 1.0；默认 0.5）" },
	/* 小窗（mini window，S1）。优先级：键位（运行时）> 窗口规则 > 全局键
	 * （config.c 的 mini_* 那一族）。 */
	{ "mini_window", RK_ACT_BOOL, VRA_MINI_WINDOW, VIDOW_RULE_WINDOW,
		"true = 这个窗口一开出来就是迷你小窗（false = 显式不是小窗）" },
	{ "mini_state", RK_ACT_STR, VRA_MINI_STATE, VIDOW_RULE_WINDOW,
		"开出来的初始形态：\"free\" / \"mini\" / \"icon\"（比 mini_window 更细）" },
	{ "mini_aspect", RK_ACT_STR, VRA_MINI_ASPECT, VIDOW_RULE_WINDOW,
		"这个窗口自由小窗态的宽高比策略：\"auto\" / \"lock\" / \"free\"。"
		"三个取值都是强制语义，压过全局键 mini_aspect；auto = 让自动信号决定"
		"（X11 有 PAspect / min==max 提示就锁，否则 free）" },
	{ "mini_preserve_buffer", RK_ACT_BOOL, VRA_MINI_PRESERVE_BUFFER,
		VIDOW_RULE_WINDOW,
		"这个窗口小窗化时保不保留客户端原生分辨率（默认跟全局 "
		"mini_preserve_buffer；有的全屏游戏必须重配才能正常渲染，单独关掉它）" },
	{ "anim", RK_ACT_BOOL, VRA_ANIM,
		VIDOW_RULE_WINDOW | VIDOW_RULE_LAYER,
		"false = 不做开合动画（窗口 / 图层都是：开、关都瞬间到位）" },
	{ "open_ms", RK_ACT_INT, VRA_OPEN_MS,
		VIDOW_RULE_WINDOW | VIDOW_RULE_LAYER,
		"开启动画时长（毫秒；覆盖 window_open_ms / layer_open_ms，0 = 不做）" },
	{ "close_ms", RK_ACT_INT, VRA_CLOSE_MS,
		VIDOW_RULE_WINDOW | VIDOW_RULE_LAYER,
		"关闭动画时长（毫秒；覆盖 window_close_ms / layer_close_ms，0 = 不做）" },
	/* 外观：装饰着色器的参数，没写的用全局配置（configs/02-decor.lua、
	 * 03-blur.lua 那些）。全局开关（decor_enabled / blur_enabled）仍是总闸。 */
	{ "decor", RK_ACT_BOOL, VRA_DECOR, VIDOW_RULE_WINDOW,
		"false = 这个窗口不画装饰（边框 / 圆角 / 阴影都不要）" },
	{ "opacity", RK_ACT_DOUBLE, VRA_OPACITY,
		VIDOW_RULE_WINDOW | VIDOW_RULE_LAYER,
		"整块的不透明度，0.5 = 半透明（窗口 = 内容 + 边框；图层 = 它自己的内容，"
		"底衬的模糊不跟着淡）" },
	{ "corner_radius", RK_ACT_DOUBLE, VRA_CORNER_RADIUS, VIDOW_RULE_WINDOW,
		"圆角半径（逻辑像素，0 = 直角）" },
	{ "corner_n", RK_ACT_DOUBLE, VRA_CORNER_N, VIDOW_RULE_WINDOW,
		"圆角的形状指数：4 = 超椭圆（squircle，和 Alt+Tab 卡片 / portal 选择器"
		"同一种）、2 = 圆弧（原来那种）；范围 2 ~ 8" },
	{ "border_width", RK_ACT_DOUBLE, VRA_BORDER_WIDTH, VIDOW_RULE_WINDOW,
		"边框宽度（0 = 不画边框）" },
	{ "border_color", RK_ACT_COLOR, VRA_BORDER_COLOR, VIDOW_RULE_WINDOW,
		"没聚焦时的边框颜色（RRGGBB / RRGGBBAA）" },
	{ "border_color_focus", RK_ACT_COLOR, VRA_BORDER_COLOR_FOCUS,
		VIDOW_RULE_WINDOW,
		"聚焦时的边框颜色" },
	{ "shadow_radius", RK_ACT_DOUBLE, VRA_SHADOW_RADIUS, VIDOW_RULE_WINDOW,
		"阴影半径（0 = 不画阴影）" },
	{ "shadow_color", RK_ACT_COLOR, VRA_SHADOW_COLOR, VIDOW_RULE_WINDOW,
		"阴影颜色，alpha 管浓淡" },
	{ "shadow_offset_x", RK_ACT_INT, VRA_SHADOW_OFFSET_X, VIDOW_RULE_WINDOW,
		"阴影横向偏移（像素）" },
	{ "shadow_offset_y", RK_ACT_INT, VRA_SHADOW_OFFSET_Y, VIDOW_RULE_WINDOW,
		"阴影纵向偏移（像素）" },
	{ "noise", RK_ACT_DOUBLE, VRA_NOISE, VIDOW_RULE_WINDOW,
		"往装饰里掺的噪点强度，0 = 不加" },
	{ "blur", RK_ACT_BOOL, VRA_BLUR, VIDOW_RULE_WINDOW | VIDOW_RULE_LAYER,
		"false = 它背后不模糊（true = 平铺窗口也模糊；全局 blur_enabled 仍是"
		"总开关，关着就一律不模糊）" },
	{ "blur_xray", RK_ACT_BOOL, VRA_BLUR_XRAY, VIDOW_RULE_WINDOW,
		"重叠时也用整屏那份底衬（糊的是壁纸，不是压着的那个窗口）" },
};

const size_t vidow_rule_field_count =
	sizeof(vidow_rule_fields) / sizeof(vidow_rule_fields[0]);

/* 子串匹配、大小写不敏感（自己写：strcasestr 要 _GNU_SOURCE，本项目按 POSIX 编）。
 * pattern 为空 = 不限（匹配一切）。 */
static bool contains(const char *haystack, const char *pattern) {
	if (pattern == NULL || pattern[0] == '\0') {
		return true;
	}
	if (haystack == NULL) {
		return false;
	}
	size_t n = strlen(pattern);
	for (const char *p = haystack; *p != '\0'; p++) {
		if (strncasecmp(p, pattern, n) == 0) {
			return true;
		}
	}
	return false;
}

const struct vidow_rule_field *vidow_rule_field_find(const char *name) {
	if (name == NULL) {
		return NULL;
	}
	for (size_t i = 0; i < vidow_rule_field_count; i++) {
		if (strcmp(vidow_rule_fields[i].name, name) == 0) {
			return &vidow_rule_fields[i];
		}
	}
	return NULL;
}

const char *vidow_rule_all_names(unsigned targets) {
	static char buf[2048];
	size_t used = 0;
	buf[0] = '\0';
	for (size_t i = 0; i < vidow_rule_field_count; i++) {
		const struct vidow_rule_field *f = &vidow_rule_fields[i];
		if (targets != 0 && !(f->targets & targets)) {
			continue;
		}
		int n = snprintf(buf + used, sizeof(buf) - used, "%s%s",
			used > 0 ? " / " : "", f->name);
		if (n < 0 || (size_t)n >= sizeof(buf) - used) {
			break;
		}
		used += (size_t)n;
	}
	return buf;
}

bool vidow_rule_init(struct vidow_server *server) {
	wl_list_init(&server->rules);
	/* 字段名重了就是 bug（查找只认第一个，写名字的那个人拿到的不是他想要的
	 * 那个字段）—— 这种错在配置里看着像「规则没生效」，很难查，所以在启动时
	 * 就喊出来。踩过一次：fullscreen 既是匹配字段又是作用字段。 */
	for (size_t i = 0; i < vidow_rule_field_count; i++) {
		for (size_t j = i + 1; j < vidow_rule_field_count; j++) {
			if (strcmp(vidow_rule_fields[i].name,
					vidow_rule_fields[j].name) == 0) {
				wlr_log(WLR_ERROR, "rule: 字段表里 %s 重了（第 %zu 项和"
					"第 %zu 项）—— 只有第一个能找到",
					vidow_rule_fields[i].name, i, j);
			}
		}
	}
	return true;
}

struct vidow_rule *vidow_rule_new(void) {
	struct vidow_rule *rule = calloc(1, sizeof(*rule));
	return rule;
}

void vidow_rule_discard(struct vidow_rule *rule) {
	if (rule == NULL) {
		return;
	}
	for (size_t i = 0; i < VRS_COUNT; i++) {
		if (rule->str[i].has_re) {
			regfree(&rule->str[i].re);
		}
	}
	free(rule);
}

/* ------------------------------------------------------------ 写字段 */

bool vidow_rule_set_str(struct vidow_rule *rule, int slot, const char *pattern,
		char *err, size_t errlen) {
	if (rule == NULL || slot < 0 || slot >= VRS_COUNT) {
		return false;
	}
	struct vidow_rule_match *m = &rule->str[slot];
	m->negate = false;
	m->has_re = false;
	const char *pat = pattern != NULL ? pattern : "";
	/* 前面加 ! = 取反（要匹配字面的 ! 就写 \!，正则那边 glibc 会把 \! 当 '!'）。 */
	if (pat[0] == '!') {
		m->negate = true;
		pat++;
	}
	if (strlen(pat) >= sizeof(m->pat)) {
		snprintf(err, errlen, "模式串太长（最多 %zu 个字符）：%s",
			sizeof(m->pat) - 1, pat);
		return false;
	}
	snprintf(m->pat, sizeof(m->pat), "%s", pat);
	return true;
}

bool vidow_rule_set_bool(struct vidow_rule *rule, int slot, bool value) {
	if (rule == NULL || slot < 0 || slot >= VRB_COUNT) {
		return false;
	}
	rule->bset[slot] = true;
	rule->bval[slot] = value;
	return true;
}

bool vidow_rule_set_num(struct vidow_rule *rule, int slot, long long value) {
	if (rule == NULL || slot < 0 || slot >= VRN_COUNT) {
		return false;
	}
	rule->nset[slot] = true;
	rule->nval[slot] = value;
	return true;
}

bool vidow_rule_set_act_bool(struct vidow_rule *rule, int slot, bool value) {
	if (rule == NULL || slot < 0 || slot >= VRA_COUNT) {
		return false;
	}
	rule->aset[slot] = true;
	rule->aval[slot].b = value;
	return true;
}

bool vidow_rule_set_act_int(struct vidow_rule *rule, int slot, long long value) {
	if (rule == NULL || slot < 0 || slot >= VRA_COUNT) {
		return false;
	}
	rule->aset[slot] = true;
	rule->aval[slot].i = value;
	return true;
}

bool vidow_rule_set_act_double(struct vidow_rule *rule, int slot,
		double value) {
	if (rule == NULL || slot < 0 || slot >= VRA_COUNT) {
		return false;
	}
	rule->aset[slot] = true;
	rule->aval[slot].d = value;
	return true;
}

bool vidow_rule_set_act_str(struct vidow_rule *rule, int slot, const char *value,
		char *err, size_t errlen) {
	if (rule == NULL || slot < 0 || slot >= VRA_COUNT) {
		return false;
	}
	if (value == NULL || value[0] == '\0') {
		snprintf(err, errlen, "名字不能是空的");
		return false;
	}
	if (strlen(value) >= VIDOW_RULE_VAL) {
		snprintf(err, errlen, "名字太长（最多 %d 个字符）：%s",
			VIDOW_RULE_VAL - 1, value);
		return false;
	}
	/* 小窗那两个字符串字段的取值是有限的：写错一位（"mini" 敲成 "miny"）
	 * 当场报错，别等到窗口开出来才发现形态没生效。 */
	if (slot == VRA_MINI_STATE) {
		enum vidow_mini_state state;
		if (!vidow_mini_parse_state(value, &state) ||
				state == VIDOW_MINI_NONE) {
			snprintf(err, errlen, "mini_state 只能是 \"free\" / \"mini\" / "
				"\"icon\"，给的是 \"%s\"", value);
			return false;
		}
	} else if (slot == VRA_MINI_ASPECT) {
		enum vidow_mini_aspect aspect;
		if (!vidow_mini_parse_aspect(value, &aspect)) {
			snprintf(err, errlen, "mini_aspect 只能是 \"auto\" / \"lock\" / "
				"\"free\"，给的是 \"%s\"", value);
			return false;
		}
	}
	rule->aset[slot] = true;
	snprintf(rule->aval[slot].s, sizeof(rule->aval[slot].s), "%s", value);
	return true;
}

bool vidow_rule_set_act_color(struct vidow_rule *rule, int slot,
		const char *value, char *err, size_t errlen) {
	if (rule == NULL || slot < 0 || slot >= VRA_COUNT) {
		return false;
	}
	float color[4];
	if (!vidow_config_parse_color(value, color)) {
		snprintf(err, errlen, "颜色得是 RRGGBB 或 RRGGBBAA（比如 \"ff0000\" / "
			"\"#ff000080\"），给的是 \"%s\"", value != NULL ? value : "");
		return false;
	}
	rule->aset[slot] = true;
	memcpy(rule->aval[slot].color, color, sizeof(color));
	return true;
}

bool vidow_rule_set_capture(struct vidow_rule *rule, int slot,
		const char *value, char *err, size_t errlen) {
	if (rule == NULL || slot < 0 || slot >= VRA_COUNT) {
		return false;
	}
	int mode;
	if (value == NULL || value[0] == '\0' || strcmp(value, "off") == 0) {
		mode = VIDOW_CAPTURE_OFF;
	} else if (strcmp(value, "hide") == 0) {
		mode = VIDOW_CAPTURE_HIDE;
	} else if (strcmp(value, "mosaic") == 0) {
		mode = VIDOW_CAPTURE_MOSAIC;
	} else {
		snprintf(err, errlen, "只能是 \"off\"、\"hide\" 或 \"mosaic\"，"
			"给的是 \"%s\"", value);
		return false;
	}
	rule->aset[slot] = true;
	rule->aval[slot].i = mode;
	return true;
}

bool vidow_rule_set_overview(struct vidow_rule *rule, int slot,
		const char *value, char *err, size_t errlen) {
	if (rule == NULL || slot < 0 || slot >= VRA_COUNT) {
		return false;
	}
	int mode;
	if (value == NULL || value[0] == '\0' || strcmp(value, "always") == 0) {
		mode = VIDOW_OVERVIEW_ALWAYS;
	} else if (strcmp(value, "only") == 0) {
		mode = VIDOW_OVERVIEW_ONLY;
	} else if (strcmp(value, "never") == 0) {
		mode = VIDOW_OVERVIEW_NEVER;
	} else {
		snprintf(err, errlen, "只能是 \"always\"、\"only\" 或 \"never\"，"
			"给的是 \"%s\"", value);
		return false;
	}
	rule->aset[slot] = true;
	rule->aval[slot].i = mode;
	return true;
}

void vidow_rule_set_literal(struct vidow_rule *rule, bool literal) {
	if (rule != NULL) {
		rule->literal = literal;
	}
}

/* 全局配置 + 规则作用 → 一份「现在长什么样」。规则没写的外观参数就用全局的；
 * 写了的那几项以规则为准。全局开关（decor_enabled / blur_enabled）压在上面：
 * 关掉全局就是关掉一切，规则只能收窄不能突破。 */
void vidow_rule_look(const struct vidow_config *cfg,
		const struct vidow_rule_eff *eff, struct vidow_look *out) {
	out->opacity = cfg->window_opacity;
	out->corner_radius = cfg->corner_radius;
	out->corner_n = vidow_corner_n_clamp(cfg->corner_n);
	out->border_width = cfg->border_width;
	memcpy(out->border_color, cfg->border_color, sizeof(out->border_color));
	memcpy(out->border_color_focus, cfg->border_color_focus,
		sizeof(out->border_color_focus));
	out->shadow_radius = cfg->shadow_radius;
	memcpy(out->shadow_color, cfg->shadow_color, sizeof(out->shadow_color));
	out->shadow_offset_x = cfg->shadow_offset_x;
	out->shadow_offset_y = cfg->shadow_offset_y;
	out->noise = cfg->noise;
	out->decor = cfg->decor_enabled;
	out->blur = cfg->blur_enabled;
	out->blur_xray = cfg->blur_xray;
	if (eff == NULL) {
		return;
	}
	if (eff->set[VRA_OPACITY]) {
		out->opacity = eff->val[VRA_OPACITY].d;
	}
	if (eff->set[VRA_CORNER_RADIUS]) {
		out->corner_radius = eff->val[VRA_CORNER_RADIUS].d;
	}
	/* 规则的和全局的一样夹到 2 ~ 8（范围检查那边已经拦过一次，这里再夹一道
	 * 是为了「不管值从哪来，进 look 的一定是合法形状」）。 */
	if (eff->set[VRA_CORNER_N]) {
		out->corner_n = vidow_corner_n_clamp(eff->val[VRA_CORNER_N].d);
	}
	if (eff->set[VRA_BORDER_WIDTH]) {
		out->border_width = eff->val[VRA_BORDER_WIDTH].d;
	}
	if (eff->set[VRA_BORDER_COLOR]) {
		memcpy(out->border_color, eff->val[VRA_BORDER_COLOR].color,
			sizeof(out->border_color));
	}
	if (eff->set[VRA_BORDER_COLOR_FOCUS]) {
		memcpy(out->border_color_focus,
			eff->val[VRA_BORDER_COLOR_FOCUS].color,
			sizeof(out->border_color_focus));
	}
	if (eff->set[VRA_SHADOW_RADIUS]) {
		out->shadow_radius = eff->val[VRA_SHADOW_RADIUS].d;
	}
	if (eff->set[VRA_SHADOW_COLOR]) {
		memcpy(out->shadow_color, eff->val[VRA_SHADOW_COLOR].color,
			sizeof(out->shadow_color));
	}
	if (eff->set[VRA_SHADOW_OFFSET_X]) {
		out->shadow_offset_x = (int)eff->val[VRA_SHADOW_OFFSET_X].i;
	}
	if (eff->set[VRA_SHADOW_OFFSET_Y]) {
		out->shadow_offset_y = (int)eff->val[VRA_SHADOW_OFFSET_Y].i;
	}
	if (eff->set[VRA_NOISE]) {
		out->noise = eff->val[VRA_NOISE].d;
	}
	if (eff->set[VRA_DECOR]) {
		/* 和 blur 一样：全局开关是总闸，规则只能收窄（decor_enabled = false 时
		 * 规则写 decor = true 也开不回来 —— 全局关装饰多半是为了性能 / 兼容，
		 * 被一条规则悄悄打开会更意外）。 */
		out->decor = cfg->decor_enabled && eff->val[VRA_DECOR].b;
	}
	if (eff->set[VRA_BLUR]) {
		out->blur = cfg->blur_enabled && eff->val[VRA_BLUR].b;
	}
	if (eff->set[VRA_BLUR_XRAY]) {
		out->blur_xray = eff->val[VRA_BLUR_XRAY].b;
	}
}

/* 窗口那一份：全屏窗口不画服务端装饰（SSD）—— 边框、圆角、阴影、噪点全归零，
 * 它直接铺满整块屏（摆位见 view.c 的 output_arrange，全屏窗口取整块输出框，
 * 不吃 gap、也不理状态栏的占位）。niri 同款：全屏窗口没有圆角。
 *
 * 注意「不画装饰」不等于「关掉 fx」：模糊、内容合成、离屏缓冲照旧走装饰着色器，
 * 只是那几笔装饰参数是 0（见 decor_draw 的 u_radius / u_border_width /
 * u_shadow_* / u_noise）。 */
void vidow_view_look(const struct vidow_config *cfg,
		const struct vidow_rule_eff *eff, bool fullscreen,
		struct vidow_look *out) {
	vidow_rule_look(cfg, eff, out);
	if (!fullscreen) {
		return;
	}
	out->corner_radius = 0.0;
	out->border_width = 0.0;
	out->shadow_radius = 0.0;
	out->noise = 0.0;
}

/* 这个目标该用多长的开合动画：全局时长打底，规则的 open_ms / close_ms 压上去，
 * anim = false 就是把两个时长都归零（调用方照「时长 <= 0 = 不做动画」处理，和
 * 配置里写 0 是同一条路）。 */
void vidow_rule_anim(const struct vidow_config *cfg,
		const struct vidow_rule_eff *eff, bool layer,
		struct vidow_rule_anim *out) {
	out->on = !(eff != NULL && eff->set[VRA_ANIM] && !eff->val[VRA_ANIM].b);
	out->open_ms = layer ? cfg->layer_open_ms : cfg->window_open_ms;
	out->close_ms = layer ? cfg->layer_close_ms : cfg->window_close_ms;
	if (eff != NULL && eff->set[VRA_OPEN_MS]) {
		out->open_ms = (int)eff->val[VRA_OPEN_MS].i;
	}
	if (eff != NULL && eff->set[VRA_CLOSE_MS]) {
		out->close_ms = (int)eff->val[VRA_CLOSE_MS].i;
	}
	if (!out->on) {
		out->open_ms = 0;
		out->close_ms = 0;
	}
	if (out->open_ms < 0) {
		out->open_ms = 0;
	}
	if (out->close_ms < 0) {
		out->close_ms = 0;
	}
}

bool vidow_rule_has_field(const struct vidow_rule *rule,
		const struct vidow_rule_field *field) {
	if (rule == NULL || field == NULL) {
		return false;
	}
	switch (field->kind) {
	case RK_STR:
		return rule->str[field->slot].pat[0] != '\0' ||
			rule->str[field->slot].negate;
	case RK_BOOL:
		return rule->bset[field->slot];
	case RK_NUM:
		return rule->nset[field->slot];
	default:
		return rule->aset[field->slot];
	}
}

const char *vidow_rule_field_str(const struct vidow_rule *rule,
		const struct vidow_rule_field *field) {
	if (rule == NULL || field == NULL || field->kind != RK_STR ||
			!vidow_rule_has_field(rule, field)) {
		return NULL;
	}
	return rule->str[field->slot].pat;
}

/* ------------------------------------------------------------ 提交 */

/* 一条规则的日志：匹配那一半 + 作用那一半。规则是配置里写的，出问题时日志
 * 是唯一线索，所以两边都打全。 */
static void log_rule(const struct vidow_rule *rule) {
	char match[512];
	size_t used = 0;
	match[0] = '\0';
	for (size_t i = 0; i < vidow_rule_field_count; i++) {
		const struct vidow_rule_field *f = &vidow_rule_fields[i];
		if (f->kind != RK_STR || !vidow_rule_has_field(rule, f)) {
			continue;
		}
		int n = snprintf(match + used, sizeof(match) - used, "%s%s%s=%s",
			used > 0 ? " " : "", rule->str[f->slot].negate ? "!" : "",
			f->name, rule->str[f->slot].pat);
		if (n < 0 || (size_t)n >= sizeof(match) - used) {
			break;
		}
		used += (size_t)n;
	}
	for (size_t i = 0; i < vidow_rule_field_count; i++) {
		const struct vidow_rule_field *f = &vidow_rule_fields[i];
		if (f->kind == RK_STR || !vidow_rule_has_field(rule, f)) {
			continue;
		}
		char value[VIDOW_RULE_VAL + 8];
		switch (f->kind) {
		case RK_BOOL:
			snprintf(value, sizeof(value), "%s",
				rule->bval[f->slot] ? "true" : "false");
			break;
		case RK_NUM:
			snprintf(value, sizeof(value), "%lld", rule->nval[f->slot]);
			break;
		case RK_ACT_BOOL:
			snprintf(value, sizeof(value), "%s",
				rule->aval[f->slot].b ? "true" : "false");
			break;
		case RK_ACT_INT:
			snprintf(value, sizeof(value), "%lld", rule->aval[f->slot].i);
			break;
		case RK_ACT_DOUBLE:
			snprintf(value, sizeof(value), "%.2f", rule->aval[f->slot].d);
			break;
		case RK_ACT_STR:
			snprintf(value, sizeof(value), "%s", rule->aval[f->slot].s);
			break;
		case RK_ACT_COLOR: {
			const float *c = rule->aval[f->slot].color;
			snprintf(value, sizeof(value), "#%02x%02x%02x%02x",
				(int)(c[0] * 255.0f + 0.5f), (int)(c[1] * 255.0f + 0.5f),
				(int)(c[2] * 255.0f + 0.5f), (int)(c[3] * 255.0f + 0.5f));
			break;
		}
		case RK_ACT_CAPTURE:
			snprintf(value, sizeof(value), "%s",
				rule->aval[f->slot].i == VIDOW_CAPTURE_HIDE ? "hide" :
				rule->aval[f->slot].i == VIDOW_CAPTURE_MOSAIC ? "mosaic" :
				"off");
			break;
		case RK_ACT_OVERVIEW:
			snprintf(value, sizeof(value), "%s",
				rule->aval[f->slot].i == VIDOW_OVERVIEW_ONLY ? "only" :
				rule->aval[f->slot].i == VIDOW_OVERVIEW_NEVER ? "never" :
				"always");
			break;
		default:
			snprintf(value, sizeof(value), "?");
			break;
		}
		int n = snprintf(match + used, sizeof(match) - used, "%s%s=%s",
			used > 0 ? " " : "", f->name, value);
		if (n < 0 || (size_t)n >= sizeof(match) - used) {
			break;
		}
		used += (size_t)n;
	}
	if (used == 0) {
		snprintf(match, sizeof(match), "*（匹配所有窗口）");
	}
	wlr_log(WLR_INFO, "rule: %s%s", match, rule->literal ? " [literal]" : "");
}

/* 配置里的模式串有一半是照 Lua 的写法写的（`^org%.kde%.kate$`），而这里
 * 编译的是 POSIX 扩展正则 —— 那边 `%` 不是转义符，`%.` 会去找字面的
 * 「% + 任意字符」，整条规则就静默不命中了（用户配置里的 output / app_id
 * 规则大量这么写，表现为「窗口没落到指定的屏」）。
 *
 * 这里把 Lua 风格的那几种转义翻成 ERE：`%x`（x 不是字母数字）→ `\x`，
 * `%%` → 字面 `%`；`%` 后面跟字母数字的（Lua 的 %d / %a 类）原样留着 ——
 * ERE 没有对应写法，翻了反而会变成别的意思。末尾落单的 `%` 也原样留着。
 * 只用于编译，m->pat 里保存的仍是用户写的原文（vidowmsg rules 读回来
 * 还是原样）。 */
static void rule_pattern_to_ere(const char *pat, char *out, size_t cap) {
	size_t o = 0;
	for (size_t i = 0; pat[i] != '\0' && o + 2 < cap; i++) {
		if (pat[i] != '%') {
			out[o++] = pat[i];
			continue;
		}
		char next = pat[i + 1];
		if (next == '\0') {
			out[o++] = '%';
		} else if (next == '%') {
			out[o++] = '%';
			i++;
		} else if ((next >= 'a' && next <= 'z') ||
				(next >= 'A' && next <= 'Z') ||
				(next >= '0' && next <= '9')) {
			out[o++] = '%';
		} else {
			out[o++] = '\\';
			out[o++] = next;
			i++;
		}
	}
	out[o] = '\0';
}

bool vidow_rule_commit(struct vidow_server *server, struct vidow_rule *rule,
		char *err, size_t errlen) {
	if (server == NULL || rule == NULL) {
		return false;
	}
	/* 至少写一个作用：不然这条规则什么也不做，八成是写错了字段名。 */
	bool has_action = false;
	for (size_t i = 0; i < VRA_COUNT; i++) {
		if (rule->aset[i]) {
			has_action = true;
			break;
		}
	}
	if (!has_action) {
		snprintf(err, errlen, "至少要写一个作用：%s",
			vidow_rule_all_names(0));
		return false;
	}
	/* 马赛克块边长：1 就是原图，没有意义。 */
	if (rule->aset[VRA_CAPTURE_MOSAIC] &&
			rule->aval[VRA_CAPTURE_MOSAIC].i < VIDOW_CAPTURE_MOSAIC_MIN) {
		snprintf(err, errlen, "capture_mosaic 是马赛克的块边长（屏幕像素），"
			"至少 %d，给的是 %lld", VIDOW_CAPTURE_MOSAIC_MIN,
			rule->aval[VRA_CAPTURE_MOSAIC].i);
		return false;
	}
	/* 列宽占比：0.1 ~ 1.0（外面那个数就是「占可用宽度的比例」）。 */
	if (rule->aset[VRA_COLUMN_WIDTH] &&
			(rule->aval[VRA_COLUMN_WIDTH].d < 0.1 ||
			 rule->aval[VRA_COLUMN_WIDTH].d > 1.0)) {
		snprintf(err, errlen, "column_width 是列宽占可用宽度的比例，"
			"得在 0.1 ~ 1.0 之间，给的是 %.2f",
			rule->aval[VRA_COLUMN_WIDTH].d);
		return false;
	}
	/* 浮动窗口的尺寸：不写 = 客户端自己定（0），写了就得是正数。 */
	const char *size_keys[2] = { "width", "height" };
	const int size_slots[2] = { VRA_WIDTH, VRA_HEIGHT };
	for (int i = 0; i < 2; i++) {
		if (rule->aset[size_slots[i]] &&
				rule->aval[size_slots[i]].i < 1) {
			snprintf(err, errlen, "%s 是逻辑像素，得是正数，给的是 %lld",
				size_keys[i], rule->aval[size_slots[i]].i);
			return false;
		}
	}
	/* 外观参数的取值范围：比例 / 长度都得是非负，透明度 0 ~ 1，圆角形状
	 * 指数 2（圆弧）~ 8（VIDOW_CORNER_N_MIN / MAX，见 rule.h）。 */
	struct { int slot; const char *name; double lo, hi; } ranges[] = {
		{ VRA_OPACITY, "opacity", 0.0, 1.0 },
		{ VRA_CORNER_RADIUS, "corner_radius", 0.0, 1e9 },
		{ VRA_CORNER_N, "corner_n", VIDOW_CORNER_N_MIN, VIDOW_CORNER_N_MAX },
		{ VRA_BORDER_WIDTH, "border_width", 0.0, 1e9 },
		{ VRA_SHADOW_RADIUS, "shadow_radius", 0.0, 1e9 },
		{ VRA_NOISE, "noise", 0.0, 1e9 },
	};
	for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
		if (!rule->aset[ranges[i].slot]) {
			continue;
		}
		double v = rule->aval[ranges[i].slot].d;
		if (v < ranges[i].lo || v > ranges[i].hi) {
			snprintf(err, errlen, "%s 得在 %g ~ %g 之间，给的是 %g",
				ranges[i].name, ranges[i].lo, ranges[i].hi, v);
			return false;
		}
	}
	/* 动画时长：毫秒，负数没有意义（0 = 不做动画，那是合法的）。 */
	const char *ms_keys[2] = { "open_ms", "close_ms" };
	const int ms_slots[2] = { VRA_OPEN_MS, VRA_CLOSE_MS };
	for (int i = 0; i < 2; i++) {
		if (rule->aset[ms_slots[i]] && rule->aval[ms_slots[i]].i < 0) {
			snprintf(err, errlen, "%s 是毫秒数，不能是负数（0 = 不做这个"
				"动画），给的是 %lld", ms_keys[i], rule->aval[ms_slots[i]].i);
			return false;
		}
	}
	/* 字符串字段编译成正则。literal = true 就跳过编译，走子串匹配。 */
	for (size_t i = 0; i < VRS_COUNT; i++) {
		struct vidow_rule_match *m = &rule->str[i];
		if (m->pat[0] == '\0' || rule->literal) {
			continue;
		}
		char ere[VIDOW_RULE_PAT];
		rule_pattern_to_ere(m->pat, ere, sizeof(ere));
		int rc = regcomp(&m->re, ere, REG_EXTENDED | REG_ICASE | REG_NOSUB);
		if (rc != 0) {
			char why[128];
			regerror(rc, &m->re, why, sizeof(why));
			const struct vidow_rule_field *f = NULL;
			for (size_t j = 0; j < vidow_rule_field_count; j++) {
				if (vidow_rule_fields[j].kind == RK_STR &&
						vidow_rule_fields[j].slot == (int)i) {
					f = &vidow_rule_fields[j];
					break;
				}
			}
			snprintf(err, errlen, "%s 的正则不对（%s）：%s%s",
				f != NULL ? f->name : "模式串", why,
				m->negate ? "!" : "", m->pat);
			return false;
		}
		m->has_re = true;
	}
	wl_list_insert(server->rules.prev, &rule->link);
	log_rule(rule);
	return true;
}

void vidow_rule_clear(struct vidow_server *server) {
	struct vidow_rule *rule, *tmp;
	wl_list_for_each_safe(rule, tmp, &server->rules, link) {
		wl_list_remove(&rule->link);
		vidow_rule_discard(rule);
	}
}

/* ------------------------------------------------------------ 目标这一侧的值
 *
 * 匹配就是「把目标现在的样子摊成一张表，再逐条比」。这张表只在对得上号的字段
 * 上有值：窗口没有 namespace，layer 没有浮动状态。 */

struct rule_ctx {
	const char *str[VRS_COUNT];
	bool bval[VRB_COUNT];
	bool nset[VRN_COUNT];
	long long nval[VRN_COUNT];
	bool is_layer;
};

/* /proc 里抓客户端的可执行文件名和命令行（规则 exe / cmdline 用）。
 * pid <= 0（拿不到）就都留空。 */
static void probe_process(int pid, char *exe, size_t exelen,
		char *cmdline, size_t cmdlen) {
	exe[0] = '\0';
	cmdline[0] = '\0';
	if (pid <= 0) {
		return;
	}
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/exe", pid);
	char buf[1024];
	ssize_t n = readlink(path, buf, sizeof(buf) - 1);
	if (n > 0) {
		buf[n] = '\0';
		const char *base = strrchr(buf, '/');
		snprintf(exe, exelen, "%s", base != NULL ? base + 1 : buf);
	}
	snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return;
	}
	char raw[1024];
	size_t got = fread(raw, 1, sizeof(raw) - 1, f);
	fclose(f);
	if (got == 0) {
		return;
	}
	for (size_t i = 0; i < got; i++) {
		if (raw[i] == '\0') {
			raw[i] = ' ';
		}
	}
	while (got > 0 && raw[got - 1] == ' ') {
		got--;
	}
	raw[got] = '\0';
	snprintf(cmdline, cmdlen, "%s", raw);
}

/* 有没有规则要 exe / cmdline：没有就不去碰 /proc。 */
static bool rules_want_proc(const struct vidow_server *server) {
	const struct vidow_rule *rule;
	wl_list_for_each(rule, &server->rules, link) {
		if (rule->str[VRS_EXE].pat[0] != '\0' ||
				rule->str[VRS_CMDLINE].pat[0] != '\0') {
			return true;
		}
	}
	return false;
}

static int view_pid(const struct vidow_view *view) {
	if (view->kind == VIDOW_VIEW_XDG && view->xdg_toplevel != NULL &&
			view->xdg_toplevel->base != NULL &&
			view->xdg_toplevel->base->client != NULL) {
		pid_t pid = 0;
		uid_t uid = 0;
		gid_t gid = 0;
		wl_client_get_credentials(view->xdg_toplevel->base->client->client,
			&pid, &uid, &gid);
		return (int)pid;
	}
	if (view->kind == VIDOW_VIEW_X11 && view->x11 != NULL) {
		return (int)view->x11->pid;
	}
	return 0;
}

static void ctx_from_view(struct vidow_server *server, struct vidow_view *view,
		struct rule_ctx *ctx) {
	memset(ctx, 0, sizeof(*ctx));
	ctx->is_layer = false;
	const bool x11 = view->kind == VIDOW_VIEW_X11;
	ctx->str[VRS_APP_ID] = vidow_view_app_id(view);
	ctx->str[VRS_TITLE] = vidow_view_title(view);
	ctx->str[VRS_CLASS] = x11 && view->x11 != NULL ? view->x11->class : NULL;
	ctx->str[VRS_INSTANCE] = x11 && view->x11 != NULL ?
		view->x11->instance : NULL;
	ctx->str[VRS_ROLE] = x11 && view->x11 != NULL ? view->x11->role : NULL;
	ctx->str[VRS_INITIAL_APP_ID] = view->init_app_id;
	ctx->str[VRS_INITIAL_TITLE] = view->init_title;
	ctx->str[VRS_INITIAL_CLASS] = view->init_class;
	ctx->str[VRS_INITIAL_INSTANCE] = view->init_instance;
	ctx->str[VRS_ON_OUTPUT] = view->output != NULL ?
		view->output->name : NULL;
	ctx->str[VRS_ON_WORKSPACE] = view->ws != NULL ?
		vidow_workspace_name(view->ws) : NULL;
	ctx->bval[VRB_XWAYLAND] = x11;
	ctx->bval[VRB_POPUP] = view->popup;
	ctx->bval[VRB_FLOATING] = view->float_mode != VIDOW_FLOAT_NONE;
	ctx->bval[VRB_FULLSCREEN] = view->fullscreen;
	ctx->bval[VRB_MAXIMIZED] = view->column != NULL && view->column->maximized;
	ctx->bval[VRB_MINIMIZED] = view->minimized;
	int fw = 0, fh = 0;
	ctx->bval[VRB_FIXED_SIZE] = vidow_view_fixed_size_hint(view, &fw, &fh);

	int pid = view_pid(view);
	if (pid > 0) {
		ctx->nset[VRN_PID] = true;
		ctx->nval[VRN_PID] = pid;
	}
	if (rules_want_proc(server)) {
		/* 抓一次就缓存：pid 是哪个进程不会变（窗口属于这个客户端）。 */
		if (view->probe_pid != pid) {
			view->probe_pid = pid;
			probe_process(pid, view->probe_exe, sizeof(view->probe_exe),
				view->probe_cmdline, sizeof(view->probe_cmdline));
		}
		ctx->str[VRS_EXE] = view->probe_exe;
		ctx->str[VRS_CMDLINE] = view->probe_cmdline;
	}
}

static const char *layer_name(uint32_t layer) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		return "background";
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		return "bottom";
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		return "top";
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
		return "overlay";
	default:
		return "?";
	}
}

static void ctx_from_layer(struct vidow_server *server,
		struct vidow_layer_surface *ls, struct rule_ctx *ctx) {
	memset(ctx, 0, sizeof(*ctx));
	ctx->is_layer = true;
	ctx->str[VRS_NAMESPACE] = ls->layer_surface->namespace;
	ctx->str[VRS_LAYER] = layer_name(ls->layer);
	ctx->str[VRS_ON_OUTPUT] = ls->output != NULL ?
		ls->output->wlr_output->name : NULL;

	int pid = 0;
	if (ls->layer_surface->surface != NULL &&
			ls->layer_surface->surface->resource != NULL) {
		pid_t p = 0;
		uid_t uid = 0;
		gid_t gid = 0;
		wl_client_get_credentials(wl_resource_get_client(
			ls->layer_surface->surface->resource), &p, &uid, &gid);
		pid = (int)p;
	}
	if (pid > 0) {
		ctx->nset[VRN_PID] = true;
		ctx->nval[VRN_PID] = pid;
	}
	if (rules_want_proc(server)) {
		if (ls->probe_pid != pid) {
			ls->probe_pid = pid;
			probe_process(pid, ls->probe_exe, sizeof(ls->probe_exe),
				ls->probe_cmdline, sizeof(ls->probe_cmdline));
		}
		ctx->str[VRS_EXE] = ls->probe_exe;
		ctx->str[VRS_CMDLINE] = ls->probe_cmdline;
	}
}

/* ------------------------------------------------------------ 匹配 */

static bool match_str(const struct vidow_rule *rule, int slot,
		const char *haystack) {
	const struct vidow_rule_match *m = &rule->str[slot];
	if (m->pat[0] == '\0') {
		return true;
	}
	bool hit;
	if (m->has_re) {
		hit = regexec(&m->re, haystack != NULL ? haystack : "", 0, NULL,
			0) == 0;
	} else {
		hit = contains(haystack, m->pat);
	}
	return m->negate ? !hit : hit;
}

/* 一条规则对这个目标匹配不匹配。字段跟目标对不上（给图层规则写 app_id 这种）
 * 直接判不匹配 —— 宁可没生效，也别因为「这个字段没意义」而悄悄匹配上一切。 */
static bool rule_matches(const struct vidow_rule *rule,
		const struct rule_ctx *ctx) {
	unsigned side = ctx->is_layer ? VIDOW_RULE_LAYER : VIDOW_RULE_WINDOW;
	for (size_t i = 0; i < vidow_rule_field_count; i++) {
		const struct vidow_rule_field *f = &vidow_rule_fields[i];
		if (!vidow_rule_has_field(rule, f)) {
			continue;
		}
		if (!(f->targets & side)) {
			return false;
		}
		switch (f->kind) {
		case RK_STR:
			if (!match_str(rule, f->slot, ctx->str[f->slot])) {
				return false;
			}
			break;
		case RK_BOOL:
			if (ctx->bval[f->slot] != rule->bval[f->slot]) {
				return false;
			}
			break;
		case RK_NUM:
			if (!ctx->nset[f->slot] ||
					ctx->nval[f->slot] != rule->nval[f->slot]) {
				return false;
			}
			break;
		default:
			break;
		}
	}
	return true;
}

/* ------------------------------------------------------------ 套用 */

/* 累积结果：命中多条规则时，后面的覆盖前面的（逐字段 —— 一条规则只动它写了
 * 的字段，没写的字段不参与，叠出来的是「所有命中规则里最后写的那个值」）。 */
static void accumulate(const struct vidow_rule *rule,
		struct vidow_rule_eff *eff) {
	for (size_t i = 0; i < VRA_COUNT; i++) {
		if (!rule->aset[i]) {
			continue;
		}
		eff->set[i] = true;
		eff->val[i] = rule->aval[i];
	}
}

static void apply_view(struct vidow_server *server, struct vidow_view *view,
		const struct vidow_rule_eff *eff) {
	/* 先摆默认值，再把命中的规则按顺序叠上来：同一个字段以最后一条写了它的规则
	 * 为准，谁都没写就是这里给的默认值。 */
	bool force_render = false;
	int hz = server->config.force_render_hz;
	bool focus_on_open = true;
	bool focus_on_activate = true;
	bool fake_focus = false;
	/* 这个的默认值是 false（不理）：规则没写就是不理。 */
	bool honor_invalid_serial = false;
	/* X11 缩放桥：0 = 跟随全局 xwayland_scale（规则没写就是这个）。 */
	double x11_scale = 0.0;
	/* 抓帧遮挡：默认不管（照常出现在截图里）。 */
	int capture = VIDOW_CAPTURE_OFF;
	int capture_mosaic = VIDOW_CAPTURE_MOSAIC_DEFAULT;
	/* 要 VRR：默认不要（niri 的 variable-refresh-rate，规则没写就是不提）。 */
	bool vrr = false;

	if (eff->set[VRA_VRR]) {
		vrr = eff->val[VRA_VRR].b;
	}
	if (eff->set[VRA_FORCE_RENDER]) {
		force_render = eff->val[VRA_FORCE_RENDER].b;
	}
	if (eff->set[VRA_HZ]) {
		hz = (int)eff->val[VRA_HZ].i;
	}
	if (eff->set[VRA_FOCUS_ON_OPEN]) {
		focus_on_open = eff->val[VRA_FOCUS_ON_OPEN].b;
	}
	if (eff->set[VRA_FOCUS_ON_ACTIVATE]) {
		focus_on_activate = eff->val[VRA_FOCUS_ON_ACTIVATE].b;
	}
	if (eff->set[VRA_FAKE_FOCUS]) {
		fake_focus = eff->val[VRA_FAKE_FOCUS].b;
	}
	if (eff->set[VRA_HONOR_INVALID_SERIAL]) {
		honor_invalid_serial = eff->val[VRA_HONOR_INVALID_SERIAL].b;
	}
	if (eff->set[VRA_X11_SCALE]) {
		x11_scale = eff->val[VRA_X11_SCALE].d;
	}
	if (eff->set[VRA_CAPTURE]) {
		capture = (int)eff->val[VRA_CAPTURE].i;
	}
	if (eff->set[VRA_CAPTURE_MOSAIC]) {
		capture_mosaic = (int)eff->val[VRA_CAPTURE_MOSAIC].i;
	}

	int interval = 0;
	if (hz > 0) {
		interval = 1000 / (hz > 1000 ? 1000 : hz);
		if (interval < 1) {
			interval = 1;
		}
	}

	view->focus_on_open = focus_on_open;
	view->focus_on_activate = focus_on_activate;
	view->honor_invalid_serial = honor_invalid_serial;
	if (view->fake_focus != fake_focus) {
		view->fake_focus = fake_focus;
		wlr_log(WLR_INFO, "rule: %s 伪装焦点 %s（%s）",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)",
			fake_focus ? "开" : "关",
			fake_focus ? "焦点移走也不给客户端发 deactivated" :
				"恢复按真实焦点发 activated");
	}
	/* 规则一改（重载 / 第一次套用）就按新值给客户端对齐一次：
	 *   fake_focus 开 → 即使现在没焦点也发 activated(true)；
	 *   fake_focus 关 → 不是当前焦点就发 activated(false)，把伪装撤掉。
	 * 还没 configure 好的窗口由 view_map 里那次调用补上。 */
	if (view->fake_focus) {
		vidow_view_set_activated(view, true);
	} else if (server->focused != view) {
		vidow_view_set_activated(view, false);
	}

	/* 要 VRR：规则改了立刻对一遍账（按需 VRR 的屏上，这个窗口在不在 = 刷新率
	 * 提不提上去）。窗口还没上屏（output == NULL）时那次调用什么也不做。 */
	if (view->vrr != vrr) {
		view->vrr = vrr;
		wlr_log(WLR_INFO, "rule: %s 要 VRR %s（%s）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			vrr ? "是" : "否",
			vrr ? "它在的按需 VRR 屏会提刷新率" : "不再提");
		vidow_output_vrr_on_demand_update(server, view->output);
	}

	/* X11 缩放桥变了：缓冲区按新倍数重新声明一遍（逻辑尺寸跟着变），布局也要
	 * 重摆 —— 规则是 SIGHUP 重载时整体重放的，所以「改了配置没重启」也能生效。 */
	if (view->kind == VIDOW_VIEW_X11 &&
			fabs(view->x11_scale - x11_scale) > 1e-6) {
		view->x11_scale = x11_scale;
		wlr_log(WLR_INFO, "rule: %s 的 X11 缩放桥 = %s（%s）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			x11_scale > 0.0 ? "规则写的" : "跟随全局",
			x11_scale > 0.0 ? "客户端没按 GDK_SCALE 画，缓冲区不再除" :
				"按全局 xwayland_scale 除");
		vidow_view_x11_scale(view);
		vidow_arrange(server);
	}

	bool changed = view->force_render != force_render;
	view->force_render = force_render;
	view->pump_interval_ms = interval;

	/* 抓帧遮挡：模式变了才记一行（这个开关平时看不见，出问题时日志是唯一的
	 * 线索）。已经藏起来的节点不用在这里放出来 —— 下一帧渲染开头会统一放
	 * （见 capturemask.c），那时候按新规则决定还藏不藏。 */
	if (view->capture_mode != capture) {
		view->capture_mode = capture;
		view->capture_mosaic = capture_mosaic;
		wlr_log(WLR_INFO, "rule: %s 抓帧遮挡 = %s",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			capture == VIDOW_CAPTURE_HIDE ? "不出现" :
			capture == VIDOW_CAPTURE_MOSAIC ? "马赛克" : "关");
	} else {
		view->capture_mosaic = capture_mosaic;
	}

	if (changed) {
		wlr_log(WLR_INFO, "rule: %s 强制渲染（视口外补帧）%s%s",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			force_render ? "开" : "关",
			interval > 0 ? "" : "（不限速）");
	}

	/* 强制浮动 / 强制平铺：规则说了算，自动识别（对话框那类）让位。
	 * vidow_float_update_mode() 也认这一份（view->rule_eff），所以尺寸提示晚到
	 * 的时候不会把规则的结论推翻。 */
	if (eff->set[VRA_FLOAT] && !view->popup) {
		enum vidow_float_mode want_mode = eff->val[VRA_FLOAT].b ?
			VIDOW_FLOAT_FIXED : VIDOW_FLOAT_NONE;
		if (view->float_mode != want_mode) {
			wlr_log(WLR_INFO, "rule: %s 规则要求%s（现在%s）",
				vidow_view_title(view) != NULL ?
					vidow_view_title(view) : "(无题)",
				want_mode == VIDOW_FLOAT_NONE ? "平铺" : "浮动",
				view->float_mode == VIDOW_FLOAT_NONE ? "平铺" : "浮动");
			vidow_float_set_mode(view, want_mode);
		}
	}
}

static void apply_layer(struct vidow_server *server,
		struct vidow_layer_surface *ls, const struct vidow_rule_eff *eff) {
	const char *ns = ls->layer_surface->namespace != NULL ?
		ls->layer_surface->namespace : "(无 namespace)";
	int capture = VIDOW_CAPTURE_OFF;
	int capture_mosaic = VIDOW_CAPTURE_MOSAIC_DEFAULT;
	if (eff->set[VRA_CAPTURE]) {
		capture = (int)eff->val[VRA_CAPTURE].i;
	}
	if (eff->set[VRA_CAPTURE_MOSAIC]) {
		capture_mosaic = (int)eff->val[VRA_CAPTURE_MOSAIC].i;
	}
	if (ls->capture_mode != capture) {
		ls->capture_mode = capture;
		wlr_log(WLR_INFO, "rule: layer %s 抓帧遮挡 = %s", ns,
			capture == VIDOW_CAPTURE_HIDE ? "不出现" :
			capture == VIDOW_CAPTURE_MOSAIC ? "马赛克" : "关");
	}
	ls->capture_mosaic = capture_mosaic;

	/* 全景里露不露（overview = "only" / "never"）：开关是在 overview.c 的
	 * overview_layers_apply 里落的，规则一改就得让新的立刻生效 —— 不然要等到
	 * 下次开关全景才看见。全景没开的时候也只动这一件事，很便宜。 */
	int overview = VIDOW_OVERVIEW_ALWAYS;
	if (eff->set[VRA_OVERVIEW]) {
		overview = (int)eff->val[VRA_OVERVIEW].i;
	}
	if (ls->overview_mode != overview) {
		ls->overview_mode = overview;
		wlr_log(WLR_INFO, "rule: layer %s 全景里 = %s", ns,
			overview == VIDOW_OVERVIEW_ONLY ? "只在全景里显示" :
			overview == VIDOW_OVERVIEW_NEVER ? "不出现" : "照常");
	}
	vidow_overview_refresh_layers(server);

	/* 开合动画与不透明度（rule.c 的 vidow_rule_anim / vidow_rule_opacity）：
	 * 时长是**用的时候**现读的（layer.c 的 layer_open_anim / layer_snapshot_close），
	 * 不透明度是在 arrange_layers 里乘进场景 alpha 的 —— 那份 alpha 只在
	 * arrange 的时候刷，所以规则一改（重载）就得重排一遍，不然要等到下次
	 * 布局变化才看见。arrange 是幂等的，重排一次很便宜。 */
	if (eff->set[VRA_ANIM] || eff->set[VRA_OPEN_MS] ||
			eff->set[VRA_CLOSE_MS] || eff->set[VRA_OPACITY]) {
		struct vidow_rule_anim ra;
		vidow_rule_anim(&server->config, eff, true, &ra);
		wlr_log(WLR_INFO, "rule: layer %s 动画 = %s（开 %dms / 关 %dms）、"
			"不透明度 %.2f", ns, ra.on ? "按规则时长" : "关", ra.open_ms,
			ra.close_ms, vidow_rule_opacity(eff));
		vidow_arrange_layers(server);
	}
}

void vidow_rule_apply(struct vidow_server *server, struct vidow_view *view) {
	if (view == NULL || view->popup) {
		return;
	}
	/* initial_* 快照：第一次套规则（= 上屏那一刻）记下来，之后只补空的。 */
	vidow_view_note_initial_names(view);
	struct rule_ctx ctx;
	ctx_from_view(server, view, &ctx);

	struct vidow_rule_eff eff = { 0 };
	const struct vidow_rule *rule;
	wl_list_for_each(rule, &server->rules, link) {
		if (!rule_matches(rule, &ctx)) {
			continue;
		}
		accumulate(rule, &eff);
	}
	/* 存一份在窗口上：渲染 / 布局那边（fx.c、view.c、float.c）直接看它，
	 * 不用每条规则各判一遍。 */
	view->rule_eff = eff;
	apply_view(server, view, &eff);
}

/* 按名字找一块屏（先精确、再大小写不敏感的子串，和 IPC 那边一个规矩）。 */
static struct vidow_output *output_by_name(struct vidow_server *server,
		const char *name) {
	struct vidow_output *output, *sub = NULL;
	wl_list_for_each(output, &server->outputs, link) {
		if (strcasecmp(output->wlr_output->name, name) == 0) {
			return output;
		}
		if (sub == NULL && contains(output->wlr_output->name, name)) {
			sub = output;
		}
	}
	return sub;
}

/* 规则里的工作区名字 → 一块工作区：
 *   "special:名字"  特殊工作区（没有就建；名字留空 = "special"）
 *   "3"             那块屏上的第 3 块（niri 的序号写法）
 *   别的            具名工作区（没有就建 —— 规则写了名字就是要它存在）
 * 注意不能直接用 vidow_workspace_move_view_to_name()：那个只按**特殊工作区**的名字
 * 找（IPC 的老规矩），具名工作区得自己 lookup / create。 */
static struct vidow_workspace *rule_workspace(struct vidow_server *server,
		struct vidow_view *view, const char *name) {
	const char *special = NULL;
	if (strncmp(name, "special:", 8) == 0) {
		special = name + 8;
		if (special[0] == '\0') {
			special = "special";
		}
	}
	if (special != NULL) {
		struct vidow_workspace *ws;
		wl_list_for_each(ws, &server->workspaces, link) {
			if (ws->special && strcmp(vidow_workspace_name(ws), special) == 0) {
				return ws;
			}
		}
		return vidow_workspace_create_special(server, special);
	}
	char *end = NULL;
	long n = strtol(name, &end, 10);
	if (end != name && *end == '\0' && n > 0) {
		struct wlr_output *output = view->output != NULL ?
			view->output : server->focused_output;
		int cnt = vidow_workspace_count(server, output);
		if (cnt == 0) {
			return NULL;
		}
		return vidow_workspace_at(server, output, n > cnt ? cnt : (int)n);
	}
	struct vidow_workspace *ws = vidow_workspace_lookup(server, name);
	if (ws != NULL) {
		return ws;
	}
	return vidow_workspace_create(server, name,
		view->output != NULL ? view->output : server->focused_output);
}

/* 开窗时兑现一次的那些作用（工作区 / 屏 / 尺寸 / 居中 / 全屏 / 最大化 / 最小化）。
 * 只在 view_map 里调一次：重载规则不搬已经开着的窗口，也不改它们的尺寸 ——
 * 「开出来什么样」和「现在什么样」是两回事。 */
void vidow_rule_placement(struct vidow_server *server, struct vidow_view *view) {
	if (view == NULL || view->popup) {
		return;
	}
	const struct vidow_rule_eff *eff = &view->rule_eff;
	const char *title = vidow_view_title(view) != NULL ?
		vidow_view_title(view) : "(无题)";
	if (eff->set[VRA_WORKSPACE]) {
		const char *name = eff->val[VRA_WORKSPACE].s;
		struct vidow_workspace *ws = rule_workspace(server, view, name);
		if (ws != NULL && vidow_workspace_move_view(server, view, ws)) {
			wlr_log(WLR_INFO, "rule: %s 开在 %s 上", title,
				vidow_workspace_name(ws));
		} else {
			wlr_log(WLR_ERROR, "rule: %s 想开在 %s 上，但搬不过去",
				title, name);
		}
	}
	if (eff->set[VRA_OUTPUT]) {
		const char *name = eff->val[VRA_OUTPUT].s;
		struct vidow_output *output = output_by_name(server, name);
		if (output == NULL) {
			wlr_log(WLR_ERROR, "rule: %s 想开在 %s 上，但没有这块屏",
				title, name);
		} else {
			struct vidow_workspace *ws = vidow_workspace_active_for(server,
				output->wlr_output);
			if (ws != NULL && vidow_workspace_move_view(server, view, ws)) {
				wlr_log(WLR_INFO, "rule: %s 开在 %s 上", title,
					output->wlr_output->name);
			}
		}
	}

	/* 尺寸 / 居中只对浮动窗口有意义（平铺的尺寸由布局定）。 */
	if (view->float_mode != VIDOW_FLOAT_NONE) {
		int w = eff->set[VRA_WIDTH] ? (int)eff->val[VRA_WIDTH].i :
			view->float_geom.width;
		int h = eff->set[VRA_HEIGHT] ? (int)eff->val[VRA_HEIGHT].i :
			view->float_geom.height;
		if (w > 0 && h > 0 && (eff->set[VRA_WIDTH] ||
				eff->set[VRA_HEIGHT])) {
			wlr_log(WLR_INFO, "rule: %s 开成 %dx%d", title, w, h);
			vidow_float_set_size(view, w, h);
		}
		if (eff->set[VRA_CENTER] && eff->val[VRA_CENTER].b) {
			vidow_float_center(view);
		}
	}

	if (eff->set[VRA_FULLSCREEN] && eff->val[VRA_FULLSCREEN].b &&
			!view->fullscreen) {
		vidow_view_toggle_fullscreen(view);
		wlr_log(WLR_INFO, "rule: %s 开出来就全屏", title);
	}
	if (eff->set[VRA_MAXIMIZE] && eff->val[VRA_MAXIMIZE].b &&
			view->column != NULL && !view->column->maximized) {
		vidow_column_set_maximized(view->column, true);
		wlr_log(WLR_INFO, "rule: %s 开出来就整列最大化", title);
	}
	/* 小窗（mini window，S1）：先定形态（进小窗 = 先变成浮动窗口，位置尺寸由
	 * 比例锚点算），**再**看要不要最小化 —— 顺序反过来会「先收起来再进小窗」，
	 * 形态就成了收起来那个窗口的形态。规则是形态的唯一权威（键位在 S4 再加一层
	 * 运行时覆盖）。 */
	if (eff->set[VRA_MINI_ASPECT]) {
		enum vidow_mini_aspect aspect = VIDOW_MINI_ASPECT_UNSET;
		if (vidow_mini_parse_aspect(eff->val[VRA_MINI_ASPECT].s, &aspect)) {
			view->mini_aspect_rule = aspect;
		} else {
			/* 取值不认识：这条规则不生效，但配置加载那一刻已经报过错
			 * （vidow.rule 那边按字段类型校验），这里只兜一层。 */
			wlr_log(WLR_ERROR, "rule: mini_aspect 只能是 auto / lock / free"
				"（给的是 '%s'），这条规则不生效",
				eff->val[VRA_MINI_ASPECT].s);
		}
	}
	if (eff->set[VRA_MINI_PRESERVE_BUFFER]) {
		view->mini_preserve_buffer_set = true;
		view->mini_preserve_buffer = eff->val[VRA_MINI_PRESERVE_BUFFER].b;
	}
	enum vidow_mini_state mini_want = VIDOW_MINI_NONE;
	bool mini_set = false;
	if (eff->set[VRA_MINI_STATE]) {
		if (vidow_mini_parse_state(eff->val[VRA_MINI_STATE].s, &mini_want) &&
				mini_want != VIDOW_MINI_NONE) {
			mini_set = true;
		} else {
			wlr_log(WLR_ERROR, "rule: mini_state 只能是 free / mini / icon"
				"（给的是 '%s'），这条规则不生效",
				eff->val[VRA_MINI_STATE].s);
		}
	} else if (eff->set[VRA_MINI_WINDOW] && eff->val[VRA_MINI_WINDOW].b) {
		mini_want = VIDOW_MINI_MINI;
		mini_set = true;
	}
	if (mini_set && view->mini != mini_want) {
		vidow_mini_set_state(view, mini_want);
		wlr_log(WLR_INFO, "rule: %s 开出来就是%s小窗", title,
			mini_want == VIDOW_MINI_FREE ? "自由" :
			mini_want == VIDOW_MINI_ICON ? "图标" : "迷你");
	}
	/* 最小化放最后：收起来的窗口不抢焦点（下面 view_map 的焦点那一步会跳过它）。 */
	if (eff->set[VRA_MINIMIZE] && eff->val[VRA_MINIMIZE].b &&
			!view->minimized) {
		vidow_view_set_minimized(view, true);
		wlr_log(WLR_INFO, "rule: %s 开出来就收起来", title);
	}
}

void vidow_rule_apply_layer(struct vidow_server *server,
		struct vidow_layer_surface *ls) {
	if (ls == NULL || ls->layer_surface == NULL) {
		return;
	}
	struct rule_ctx ctx;
	ctx_from_layer(server, ls, &ctx);

	struct vidow_rule_eff eff = { 0 };
	const struct vidow_rule *rule;
	wl_list_for_each(rule, &server->rules, link) {
		if (!rule_matches(rule, &ctx)) {
			continue;
		}
		accumulate(rule, &eff);
	}
	ls->rule_eff = eff;
	apply_layer(server, ls, &eff);
}

void vidow_rule_apply_all(struct vidow_server *server) {
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		vidow_rule_apply(server, view);
	}
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		vidow_rule_apply_layer(server, ls);
	}
}

size_t vidow_rule_count(const struct vidow_server *server) {
	size_t n = 0;
	const struct vidow_rule *rule;
	wl_list_for_each(rule, &server->rules, link) {
		n++;
	}
	return n;
}

const struct vidow_rule *vidow_rule_at(const struct vidow_server *server,
		size_t index) {
	size_t i = 0;
	const struct vidow_rule *rule;
	wl_list_for_each(rule, &server->rules, link) {
		if (i == index) {
			return rule;
		}
		i++;
	}
	return NULL;
}
