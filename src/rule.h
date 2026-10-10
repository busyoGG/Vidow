/* 窗口规则 / 图层规则：一条规则 = 匹配谁 + 怎么对待它，用户写在 config.lua 里。
 *
 *   vidow.rule{ app_id = "mpv", force_render = true }
 *   vidow.rule{ title = "^预览", force_render = true, hz = 15 }
 *   vidow.rule{ class = "Steam", focus_on_open = false }
 *   vidow.rule{ namespace = "notifications", capture = "hide" }
 *
 * 匹配：每个字符串字段都是一条 **POSIX 扩展正则**（ERE），不锚定 = 找到就算命中，
 * 大小写不敏感；留空 = 不限。写正则之前那些普通字符串照样是「子串匹配」——
 * "mpv" 当正则用就是在整串里找 mpv。含正则元字符（. * + ? [ ] ( ) { } | ^ $ \）
 * 的串得转义：title = "v1\\.0"。整条规则写 literal = true 就退回「纯子串」匹配。
 * 串前面加 ! = 取反（「这个字段不是它」），要匹配字面的 ! 就写 \\!。
 *
 * 一条规则**只管一边**：写了 namespace / layer（layer surface 才有的字段）就是
 * 图层规则；写了窗口才有的字段就是窗口规则。字段跟目标对不上（比如给图层规则写
 * app_id）时这条规则**不匹配**（宁可没生效也别悄悄匹配上一切）。
 *
 * 规则按配置里的先后顺序逐条套用，**后面的覆盖前面的**：同一个字段以「最后一条
 * 写了它的规则」为准；谁都没写的字段用默认值（不是回上一条规则给的值，也不是
 * 回「这条规则没写」的那个默认 —— 一条规则只动它写了的字段）。 */
#ifndef VIDOW_RULE_H
#define VIDOW_RULE_H

#include <stdbool.h>
#include <stddef.h>
#include <regex.h>

#include <wayland-util.h>

struct vidow_server;
struct vidow_view;
struct vidow_layer_surface;

/* 抓帧遮挡：这个窗口 / layer 在别人截图、录屏的时候怎么出现。
 *   OFF    不管（默认）
 *   HIDE   完全不出现（那块地方露出它背后的东西）
 *   MOSAIC 出现，但是打马赛克（真实内容降采样再放大，看得出布局看不出字） */
enum vidow_capture_mode {
	VIDOW_CAPTURE_OFF = 0,
	VIDOW_CAPTURE_HIDE,
	VIDOW_CAPTURE_MOSAIC,
};

/* 马赛克块边长（屏幕像素）的默认值：16px 的块在 1080p 上大概 120 格宽，
 * 看得出版面、看不出字。 */
#define VIDOW_CAPTURE_MOSAIC_DEFAULT 16
/* 块边长下限：1 就是原图，没有意义（渲染上也要 ≥2 才能叫马赛克）。 */
#define VIDOW_CAPTURE_MOSAIC_MIN 2

/* 模式串长度上限（含结尾的 0）：正则可以比子串长，但也别太长。 */
#define VIDOW_RULE_PAT 192
/* 作用里的字符串（workspace / output 的名字）长度上限。 */
#define VIDOW_RULE_VAL 128

/* ------------------------------------------------------------ 匹配字段
 *
 * 三个维度分开排：字符串（正则）、布尔、数值。加一个字段要动四处：这里的枚举、
 * rule.c 的 vidow_rule_fields[]（名字 / 类型 / 管哪一边 / 一句话说明）、
 * rule.c 的 ctx_from_view / ctx_from_layer（填值）、以及文档。 */

enum vidow_rule_str {
	VRS_APP_ID = 0,        /* Wayland 的 app_id / X11 的 WM_CLASS class 段 */
	VRS_TITLE,             /* 当前标题 */
	VRS_CLASS,             /* X11 的 WM_CLASS class 段 */
	VRS_INSTANCE,          /* X11 的 WM_CLASS instance 段 */
	VRS_ROLE,              /* X11 的 WM_WINDOW_ROLE */
	VRS_INITIAL_APP_ID,    /* 上屏那一刻的 app_id（客户端后来改标题不影响它） */
	VRS_INITIAL_TITLE,
	VRS_INITIAL_CLASS,
	VRS_INITIAL_INSTANCE,
	VRS_EXE,               /* 客户端可执行文件名（/proc/<pid>/exe 的最后一段） */
	VRS_CMDLINE,           /* 客户端命令行（/proc/<pid>/cmdline，参数用空格接） */
	VRS_NAMESPACE,         /* layer surface 的 namespace（waybar / mako …） */
	VRS_LAYER,             /* layer 的层：background / bottom / top / overlay */
	VRS_ON_OUTPUT,         /* 现在摆在哪块屏上（输出名） */
	VRS_ON_WORKSPACE,      /* 现在属于哪块工作区（名字） */
	VRS_COUNT
};

enum vidow_rule_bool {
	VRB_XWAYLAND = 0,      /* 是 X11（XWayland）窗口 */
	VRB_POPUP,             /* 菜单 / 提示 / 下拉框这类弹出物 */
	VRB_FLOATING,          /* 现在是浮动窗口 */
	VRB_FULLSCREEN,
	VRB_MAXIMIZED,         /* 所在列正整列最大化 */
	VRB_MINIMIZED,         /* 收起来了（还在列里，但不显示） */
	VRB_FIXED_SIZE,        /* 客户端声明了固定尺寸（min == max），微信登录窗那种 */
	VRB_COUNT
};

enum vidow_rule_num {
	VRN_PID = 0,           /* 客户端进程号 */
	VRN_COUNT
};

/* ------------------------------------------------------------ 作用字段
 *
 * 「写了才动」：没写的字段回默认值，不是留着上一条规则给的值。 */

enum vidow_rule_act {
	VRA_FORCE_RENDER = 0,  /* 视口外也补帧（录屏 / 预览要新画面） */
	VRA_HZ,                /* 补帧频率（0 = 不限） */
	VRA_FOCUS_ON_OPEN,     /* 它开新窗口时抢不抢焦点 */
	VRA_FOCUS_ON_ACTIVATE, /* 别人请求激活它时理不理 */
	VRA_HONOR_INVALID_SERIAL, /* 令牌 serial 校验不过也理 */
	VRA_X11_SCALE,         /* X11 缩放桥（1 = 不除） */
	VRA_CAPTURE,           /* off / hide / mosaic */
	VRA_CAPTURE_MOSAIC,    /* 马赛克块边长 */
	/* 开窗时兑现一次的（见 vidow_rule_placement）：重载不搬窗口、不改尺寸。 */
	VRA_FLOAT,             /* true = 强制浮动，false = 强制平铺 */
	VRA_WORKSPACE,         /* 开在哪块工作区（具名 / 特殊工作区 / 序号） */
	VRA_OUTPUT,            /* 开在哪块屏（那块的当前工作区） */
	VRA_WIDTH,             /* 浮动窗口的宽（逻辑像素，0 = 客户端自己定） */
	VRA_HEIGHT,
	VRA_CENTER,            /* 浮动窗口居中 */
	VRA_FULLSCREEN,        /* 开就全屏 */
	VRA_MAXIMIZE,          /* 开就整列最大化 */
	VRA_MINIMIZE,          /* 开就收起来（不显示、不抢焦点） */
	VRA_COLUMN_WIDTH,      /* 新列的宽度占比（0.1 ~ 1.0） */
	/* 一直在的 */
	VRA_ANIM,              /* false = 这个窗口不做开合动画 */
	/* 外观（装饰着色器的参数；没写的用全局配置） */
	VRA_DECOR,             /* false = 这个窗口不画装饰（边框 / 圆角 / 阴影） */
	VRA_OPACITY,           /* 整块（含装饰）的不透明度 */
	VRA_CORNER_RADIUS,
	VRA_BORDER_WIDTH,
	VRA_BORDER_COLOR,      /* 没聚焦时的边框颜色 */
	VRA_BORDER_COLOR_FOCUS,
	VRA_SHADOW_RADIUS,
	VRA_SHADOW_COLOR,
	VRA_SHADOW_OFFSET_X,
	VRA_SHADOW_OFFSET_Y,
	VRA_NOISE,
	VRA_BLUR,              /* false = 它背后不模糊（全局 blur_enabled 仍是总开关） */
	VRA_BLUR_XRAY,         /* 重叠时也用整屏那份底衬 */
	VRA_COUNT
};

/* 一个作用的值：按 vidow_rule_fields[] 里的类型取对应的成员。 */
union vidow_rule_value {
	bool b;
	long long i;
	double d;
	float color[4];
	char s[VIDOW_RULE_VAL];
};

/* 一个字符串匹配字段：模式串 + 编译好的正则（literal 时不编译）。 */
struct vidow_rule_match {
	char pat[VIDOW_RULE_PAT];
	regex_t re;
	bool has_re;
	bool negate; /* 写的时候前面带了 ! */
};

/* 一个目标（窗口 / layer）身上最终生效的作用：命中多条规则时后面的覆盖前面的
 * （逐字段）。view->rule_eff / ls->rule_eff 存的就是这一份 —— 渲染和布局那边
 * 直接看它，不用每条规则各判一遍。 */
struct vidow_rule_eff {
	bool set[VRA_COUNT];
	union vidow_rule_value val[VRA_COUNT];
};

struct vidow_rule {
	struct wl_list link; /* vidow_server.rules */

	/* 匹配 */
	struct vidow_rule_match str[VRS_COUNT];
	bool bset[VRB_COUNT];
	bool bval[VRB_COUNT];
	bool nset[VRN_COUNT];
	long long nval[VRN_COUNT];
	bool literal; /* 所有字符串按子串匹配（不做正则） */

	/* 作用 */
	bool aset[VRA_COUNT];
	union vidow_rule_value aval[VRA_COUNT];
};

/* 一个窗口 / layer 现在的装饰参数：窗口规则写了就用规则的，没写用全局配置。
 * 渲染（fx.c）和 IPC（vidowmsg windows 的 look 那一块）都看这一份 ——
 * 「规则有没有生效」不用去量像素。 */
struct vidow_look {
	double opacity;
	double corner_radius;
	double border_width;
	float border_color[4];
	float border_color_focus[4];
	double shadow_radius;
	float shadow_color[4];
	int shadow_offset_x;
	int shadow_offset_y;
	double noise;
	bool decor;   /* 画不画装饰 */
	bool blur;    /* 它背后的模糊给不给（全局 blur_enabled 关着就一律不给） */
	bool blur_xray;
};

/* 字段类型：RK_* 是匹配字段，RK_ACT_* 是作用字段。 */
enum vidow_rule_kind {
	RK_STR = 0,
	RK_BOOL,
	RK_NUM,
	RK_ACT_BOOL,
	RK_ACT_INT,
	RK_ACT_DOUBLE,
	RK_ACT_STR,
	RK_ACT_COLOR,
	RK_ACT_CAPTURE,
};

/* 这个字段管哪一边（不适用 = 这条规则对那一边不匹配）。 */
#define VIDOW_RULE_WINDOW 1u
#define VIDOW_RULE_LAYER  2u

struct vidow_rule_field {
	const char *name;   /* Lua / JSON 里的名字 */
	enum vidow_rule_kind kind;
	int slot;           /* str[] / bset[] / nset[] / aset[] 的下标 */
	unsigned targets;   /* VIDOW_RULE_WINDOW | VIDOW_RULE_LAYER */
	const char *help;   /* 一句话说明（报错和文档用） */
};

extern const struct vidow_rule_field vidow_rule_fields[];
extern const size_t vidow_rule_field_count;

bool vidow_rule_init(struct vidow_server *server);

/* 按名字找字段；NULL = 没这个名字（调用方报错时可以把所有名字列出来）。 */
const struct vidow_rule_field *vidow_rule_field_find(const char *name);
/* 所有字段名，用 " / " 连起来（报错用）。targets = 0 表示全部。 */
const char *vidow_rule_all_names(unsigned targets);

/* 一条新规则：字段填完调 vidow_rule_commit 挂进 server->rules；中间出错就
 * vidow_rule_discard 丢掉。 */
struct vidow_rule *vidow_rule_new(void);
void vidow_rule_discard(struct vidow_rule *rule);

/* 写字段（Lua 那边按 kind 调对应的那个）。失败返回 false 并填 err。 */
bool vidow_rule_set_str(struct vidow_rule *rule, int slot, const char *pattern,
		char *err, size_t errlen);
bool vidow_rule_set_bool(struct vidow_rule *rule, int slot, bool value);
bool vidow_rule_set_num(struct vidow_rule *rule, int slot, long long value);
bool vidow_rule_set_act_bool(struct vidow_rule *rule, int slot, bool value);
bool vidow_rule_set_act_int(struct vidow_rule *rule, int slot, long long value);
bool vidow_rule_set_act_double(struct vidow_rule *rule, int slot, double value);
bool vidow_rule_set_act_str(struct vidow_rule *rule, int slot, const char *value,
		char *err, size_t errlen);
bool vidow_rule_set_act_color(struct vidow_rule *rule, int slot,
		const char *value, char *err, size_t errlen);
bool vidow_rule_set_capture(struct vidow_rule *rule, int slot,
		const char *value, char *err, size_t errlen);
void vidow_rule_set_literal(struct vidow_rule *rule, bool literal);

/* 把全局配置和规则算出来的作用合成一份「现在长什么样」。eff 可以是 NULL
 * （没有规则的目标）。 */
struct vidow_config;
void vidow_rule_look(const struct vidow_config *cfg,
		const struct vidow_rule_eff *eff, struct vidow_look *out);

/* 这条规则写没写这个字段（读回来的时候用：vidow.rules() / IPC 的 rules）。 */
bool vidow_rule_has_field(const struct vidow_rule *rule,
		const struct vidow_rule_field *field);
/* 字符串字段的模式串，原样（带取反的 !；没写 = NULL）。 */
const char *vidow_rule_field_str(const struct vidow_rule *rule,
		const struct vidow_rule_field *field);

/* 编译正则 + 检查「至少写一个作用」，然后挂进 server->rules（成功时返回 true，
 * 之后 rule 归 server 管）。失败填 err，调用方负责 vidow_rule_discard。 */
bool vidow_rule_commit(struct vidow_server *server, struct vidow_rule *rule,
		char *err, size_t errlen);

/* 清空（SIGHUP 重载 = 重新声明一遍）。 */
void vidow_rule_clear(struct vidow_server *server);

/* 把规则套到这个窗口上（建窗口时、标题/class 变了、规则重载时都调）。只做
 * 「一直在」的那些：补帧 / 焦点 / 缩放桥 / 抓帧 / 浮动 / 动画。 */
void vidow_rule_apply(struct vidow_server *server, struct vidow_view *view);

/* 开窗时**兑现一次**的那些（工作区 / 屏 / 尺寸 / 居中 / 全屏 / 最大化 / 最小化）：
 * 只在 view_map 里调一次 —— 重载规则不搬已经开着的窗口，也不改它们的尺寸。 */
void vidow_rule_placement(struct vidow_server *server, struct vidow_view *view);

/* 把规则套到这个 layer surface 上（它 map 的时候、规则重载时都调）。 */
void vidow_rule_apply_layer(struct vidow_server *server,
		struct vidow_layer_surface *ls);

/* 规则重载之后，把新规则套到所有已经开着的窗口和 layer 上。 */
void vidow_rule_apply_all(struct vidow_server *server);

size_t vidow_rule_count(const struct vidow_server *server);
const struct vidow_rule *vidow_rule_at(const struct vidow_server *server,
		size_t index);

#endif
