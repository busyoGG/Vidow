/* 键位绑定：把「Mod+1」这样的字符串绑到一个动作名字上。
 *
 * 动作由用户自己写：内置动作就是工作区那几个（workspace / special / move /
 * focus / mousemove），键位完全由 config.lua 决定 —— 合成器不预设任何一个键。
 *
 *   vidow.bind("Mod+1", "workspace 1")
 *   vidow.bind("Mod+Shift+1", "move 1")
 *   vidow.bind("Mod+grave", "special scratch")
 *   vidow.bind("Mod+Shift+grave", "move scratch")
 *   vidow.bind("Mod+Escape", "quit", { inhibited = true })  -- 抑制期间也生效
 *
 * 第三个参数（可选）是选项表，现在只有一个字段 `inhibited`：焦点那个表面上有
 * 生效中的「快捷键抑制」（虚拟机的窗口要原样收 Super / Alt+Tab）时，这条绑定
 * 照样生效 —— 逃生口，不然抑制着的窗口能把用户锁死（见 keyboardgrab.c）。
 *
 * 动作字符串是开放集合，不认识的只是记一行日志（用户能自己加动作名字，
 * 见 script.c 里注册的那几个）。 */
#ifndef VIDOW_KEYBIND_H
#define VIDOW_KEYBIND_H

#include <stdbool.h>
#include <stddef.h>

#include <wayland-util.h>

#include <xkbcommon/xkbcommon.h>

struct vidow_server;

struct vidow_keybind {
	struct wl_list link; /* vidow_server.keybinds */
	char trigger[64];    /* 原样记着，日志和 list 里用 */
	char action[128];
	uint32_t mods;       /* WLR_MODIFIER_* 的掩码，0 = 不看修饰键 */
	xkb_keysym_t keysym;
	/* 抑制期间也生效（vidow.bind 第三个参数 { inhibited = true }）。 */
	bool inhibited;
};

bool vidow_keybind_init(struct vidow_server *server);

/* 加一条绑定。失败时把原因写进 err（不超过 errlen），返回 NULL。 */
struct vidow_keybind *vidow_keybind_add(struct vidow_server *server,
		const char *trigger, const char *action, bool inhibited,
		char *err, size_t errlen);

/* 清空全部绑定（config.lua 重载时用：重载等于重新声明一遍）。 */
void vidow_keybind_clear(struct vidow_server *server);

/* 有按键来了：命中哪条绑定就执行哪条，返回是否吃掉了这个按键。
 * 没命中的按键照旧透传给客户端。inhibited = 现在焦点那个表面有生效中的抑制
 * （那就只认标记了 inhibited 的那几条，见 keyboardgrab.c）。 */
bool vidow_keybind_key(struct vidow_server *server, xkb_keysym_t sym,
		uint32_t mods, bool inhibited);

/* 执行一个动作字符串（键位绑定和 VIDOW_TEST_ACTION 测试钩子共用）。 */
bool vidow_action_run(struct vidow_server *server, const char *action);

/* 切 tty（真机才有会话）。键位（Ctrl+Alt+F1..F12）和 `switch-vt n` 动作都走这里。 */
bool vidow_vt_switch(struct vidow_server *server, unsigned vt);

#endif
