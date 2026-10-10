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
	{ "xwayland", RK_BOOL, VRB_XWAYLAND, VIDOW_RULE_WINDOW,
		"是 X11（XWayland）窗口" },
	{ "popup", RK_BOOL, VRB_POPUP, VIDOW_RULE_WINDOW,
		"菜单 / 提示 / 下拉框这类弹出物（X11 的 override-redirect）" },
	{ "floating", RK_BOOL, VRB_FLOATING, VIDOW_RULE_WINDOW,
		"现在是浮动窗口" },
	{ "fullscreen", RK_BOOL, VRB_FULLSCREEN, VIDOW_RULE_WINDOW,
		"现在全屏" },
	{ "maximized", RK_BOOL, VRB_MAXIMIZED, VIDOW_RULE_WINDOW,
		"所在列正整列最大化" },
	{ "minimized", RK_BOOL, VRB_MINIMIZED, VIDOW_RULE_WINDOW,
		"收起来了（还在列里，但不显示）" },
	{ "fixed_size", RK_BOOL, VRB_FIXED_SIZE, VIDOW_RULE_WINDOW,
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

void vidow_rule_set_literal(struct vidow_rule *rule, bool literal) {
	if (rule != NULL) {
		rule->literal = literal;
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
	/* 字符串字段编译成正则。literal = true 就跳过编译，走子串匹配。 */
	for (size_t i = 0; i < VRS_COUNT; i++) {
		struct vidow_rule_match *m = &rule->str[i];
		if (m->pat[0] == '\0' || rule->literal) {
			continue;
		}
		int rc = regcomp(&m->re, m->pat, REG_EXTENDED | REG_ICASE | REG_NOSUB);
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
struct rule_eff {
	bool set[VRA_COUNT];
	union vidow_rule_value val[VRA_COUNT];
};

static void accumulate(const struct vidow_rule *rule, struct rule_eff *eff) {
	for (size_t i = 0; i < VRA_COUNT; i++) {
		if (!rule->aset[i]) {
			continue;
		}
		eff->set[i] = true;
		eff->val[i] = rule->aval[i];
	}
}

static void apply_view(struct vidow_server *server, struct vidow_view *view,
		const struct rule_eff *eff) {
	/* 先摆默认值，再把命中的规则按顺序叠上来：同一个字段以最后一条写了它的规则
	 * 为准，谁都没写就是这里给的默认值。 */
	bool force_render = false;
	int hz = server->config.force_render_hz;
	bool focus_on_open = true;
	bool focus_on_activate = true;
	/* 这个的默认值是 false（不理）：规则没写就是不理。 */
	bool honor_invalid_serial = false;
	/* X11 缩放桥：0 = 跟随全局 xwayland_scale（规则没写就是这个）。 */
	double x11_scale = 0.0;
	/* 抓帧遮挡：默认不管（照常出现在截图里）。 */
	int capture = VIDOW_CAPTURE_OFF;
	int capture_mosaic = VIDOW_CAPTURE_MOSAIC_DEFAULT;

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
}

static void apply_layer(struct vidow_server *server,
		struct vidow_layer_surface *ls, const struct rule_eff *eff) {
	(void)server;
	int capture = VIDOW_CAPTURE_OFF;
	int capture_mosaic = VIDOW_CAPTURE_MOSAIC_DEFAULT;
	if (eff->set[VRA_CAPTURE]) {
		capture = (int)eff->val[VRA_CAPTURE].i;
	}
	if (eff->set[VRA_CAPTURE_MOSAIC]) {
		capture_mosaic = (int)eff->val[VRA_CAPTURE_MOSAIC].i;
	}
	if (ls->capture_mode == capture) {
		ls->capture_mosaic = capture_mosaic;
		return;
	}
	ls->capture_mode = capture;
	ls->capture_mosaic = capture_mosaic;
	wlr_log(WLR_INFO, "rule: layer %s 抓帧遮挡 = %s",
		ls->layer_surface->namespace != NULL ?
			ls->layer_surface->namespace : "(无 namespace)",
		capture == VIDOW_CAPTURE_HIDE ? "不出现" :
		capture == VIDOW_CAPTURE_MOSAIC ? "马赛克" : "关");
}

void vidow_rule_apply(struct vidow_server *server, struct vidow_view *view) {
	if (view == NULL || view->popup) {
		return;
	}
	/* initial_* 快照：第一次套规则（= 上屏那一刻）记下来，之后只补空的。 */
	vidow_view_note_initial_names(view);
	struct rule_ctx ctx;
	ctx_from_view(server, view, &ctx);

	struct rule_eff eff = { 0 };
	const struct vidow_rule *rule;
	wl_list_for_each(rule, &server->rules, link) {
		if (!rule_matches(rule, &ctx)) {
			continue;
		}
		accumulate(rule, &eff);
	}
	apply_view(server, view, &eff);
}

void vidow_rule_apply_layer(struct vidow_server *server,
		struct vidow_layer_surface *ls) {
	if (ls == NULL || ls->layer_surface == NULL) {
		return;
	}
	struct rule_ctx ctx;
	ctx_from_layer(server, ls, &ctx);

	struct rule_eff eff = { 0 };
	const struct vidow_rule *rule;
	wl_list_for_each(rule, &server->rules, link) {
		if (!rule_matches(rule, &ctx)) {
			continue;
		}
		accumulate(rule, &eff);
	}
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
