/* 窗口规则：按 app_id / 标题 / X11 class 给窗口定行为，用户写在 config.lua 里。
 *
 *   vidow.rule{ app_id = "mpv", force_render = true }              -- 视口外也补帧
 *   vidow.rule{ title = "预览", force_render = true, hz = 15 }     -- 而且只补 15Hz
 *   vidow.rule{ app_id = "mpv", hz = 0 }                           -- 只调节流：不限速
 *   vidow.rule{ app_id = "slack", focus_on_open = false }          -- 开新窗口别抢焦点
 *   vidow.rule{ app_id = "slack", focus_on_activate = false }      -- 别理它的抢焦点请求
 *   vidow.rule{ app_id = "weird", honor_invalid_serial = true }    -- 它令牌不灵也照样理
 *
 * 作用（可以只写一个）：
 *   force_render —— 窗口滚到视口外（或者所在工作区被藏起来）时，
 * 仍然给它补帧回调，让客户端接着出帧。原因见 view.c 里 vidow_view_render_pump()：
 * Wayland 的帧回调是合成器「我要用你的画面了」的许可，窗口不在画面上就没有回调，
 * 视频播放器 / 游戏 / 任何按帧节流的客户端会停帧 —— 录屏、窗口预览、切回来看到的
 * 就是一张冻住的画面。
 *   hz           —— 这种窗口每秒补多少帧（0 = 不限）。不写就用全局的
 *                   force_render_hz；补帧要主动排下一帧，不限速会空转到几千 fps。
 *   focus_on_open     —— 这个应用**开新窗口**的时候要不要把焦点给它（默认 true）。
 * Electron 系客户端会为了通知 / 剪贴板开一个看不见的辅助窗口，一开就把焦点从你
 * 正在打字的窗口上抢走；这类应用关掉它，窗口照常出现，只是不抢键盘。
 *   focus_on_activate —— 别人**请求激活**它（点通知、任务栏按钮、portal 顶窗口）
 * 时理不理（默认 true）。反过来的毛病（它的令牌不灵、点了通知窗口不动）由
 * 配置项 activation_token_timeout 治，见 config.c。
 *   honor_invalid_serial —— 它的激活请求**即使令牌里的 serial 校验不过**也照样理
 * （默认 false）。工具包偷懒写 set_serial(0, seat) 的应用（很常见）默认顶不上来；
 * 全局开关 honor_xdg_activation_with_invalid_serial 是「所有应用都理」，这条规则是
 * 「只有这几个应用理」—— 想开小口子就用它（代价：serial 校验本来是防抢焦点的）。
 *
 * 匹配是「子串、大小写不敏感」；规则按配置里的先后顺序逐条套用，后面的覆盖前面的。
 * 三个字段都空 = 匹配所有窗口（能用，但小心）。 */
#ifndef VIDOW_RULE_H
#define VIDOW_RULE_H

#include <stdbool.h>
#include <stddef.h>

#include <wayland-util.h>

struct vidow_server;
struct vidow_view;

struct vidow_rule {
	struct wl_list link; /* vidow_server.rules */
	char app_id[128];    /* 空 = 不限 */
	char title[128];
	char klass[128];     /* X11 的 WM_CLASS */

	bool has_force_render;
	bool force_render;

	/* 这个窗口的补帧节流（每秒几帧，0 = 不限）；has_hz = false 时用全局配置 */
	bool has_hz;
	int hz;

	/* 焦点行为；has_xxx = false 时不动（用窗口自己的默认值，都是 true） */
	bool has_focus_on_open;
	bool focus_on_open;
	bool has_focus_on_activate;
	bool focus_on_activate;

	/* 无效 serial 的激活请求理不理（默认 false = 不理） */
	bool has_honor_invalid_serial;
	bool honor_invalid_serial;

	/* 这个窗口的 X11 缩放桥（0 = 跟随全局 xwayland_scale）。
	 *
	 * 全局那套假设「X 客户端按 GDK_SCALE / QT_SCALE_FACTOR 画了 2 倍」，于是把
	 * 缓冲区除以 2 —— 认这套 env 的客户端（GTK / Qt）刚好和 Wayland 应用一样大，
	 * 不认的老程序（微信登录窗、CEF 系）就被缩掉一半（面积 1/4）。逐窗口写 1
	 * 就是「这个客户端没按 2 倍画，别除」。 */
	bool has_scale;
	double scale;
};

/* 一条规则的内容。字段多，用结构体传：全是「有没有写 / 写的什么」成对的开关，
 * 十几个位置参数迟早会传错顺序。字符串可以是 NULL / 空串。 */
struct vidow_rule_spec {
	const char *app_id;
	const char *title;
	const char *klass;

	bool has_force_render;
	bool force_render;
	bool has_hz;
	int hz;

	bool has_focus_on_open;
	bool focus_on_open;
	bool has_focus_on_activate;
	bool focus_on_activate;

	bool has_honor_invalid_serial;
	bool honor_invalid_serial;

	bool has_scale;
	double scale;
};

bool vidow_rule_init(struct vidow_server *server);

/* 加一条规则（Lua 那边解析完表之后调）。至少要写一个作用（force_render / hz /
 * focus_on_open / focus_on_activate / honor_invalid_serial / x11_scale），不然这条
 * 规则什么也不做（调用方负责报错）。 */
struct vidow_rule *vidow_rule_add(struct vidow_server *server,
		const struct vidow_rule_spec *spec);

/* 清空（SIGHUP 重载 = 重新声明一遍）。 */
void vidow_rule_clear(struct vidow_server *server);

/* 这条规则匹配不匹配这个窗口。 */
bool vidow_rule_matches(const struct vidow_rule *rule,
		const struct vidow_view *view);

/* 把规则套到这个窗口上（建窗口时、标题/class 变了、规则重载时都调）。
 * 返回强制渲染这个开关有没有变化。 */
bool vidow_rule_apply(struct vidow_server *server, struct vidow_view *view);

/* 规则重载之后，把新规则套到所有已经开着的窗口上。 */
void vidow_rule_apply_all(struct vidow_server *server);

size_t vidow_rule_count(const struct vidow_server *server);
const struct vidow_rule *vidow_rule_at(const struct vidow_server *server,
		size_t index);

#endif
