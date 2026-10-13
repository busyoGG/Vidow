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
 *   vidow.bind("Mod+Shift+R", "…", { repeat = false, cooldown_ms = 200 })
 *
 * 第三个参数（可选）是选项表：`inhibited` / `on` / `repeat` / `cooldown_ms`。焦点那个表面上有
 * 生效中的「快捷键抑制」（虚拟机的窗口要原样收 Super / Alt+Tab）时，这条绑定
 * 照样生效 —— 逃生口，不然抑制着的窗口能把用户锁死（见 keyboardgrab.c）。
 *
 * 动作字符串是开放集合，不认识的只是记一行日志（用户能自己加动作名字，
 * 见 script.c 里注册的那几个）。 */
#ifndef VIDOW_KEYBIND_H
#define VIDOW_KEYBIND_H

#include <stdbool.h>
#include <stddef.h>

#include <wayland-client-protocol.h> /* enum wl_pointer_axis（滚轮绑定要用） */
#include <wayland-util.h>

#include <xkbcommon/xkbcommon.h>

struct vidow_server;

/* 触发时机：按下（默认）还是松开。 */
enum vidow_bind_on {
	VIDOW_BIND_PRESS = 0,
	VIDOW_BIND_RELEASE,
};

/* 触发源：键盘键 / 鼠标按键 / 滚轮。三类共用一张绑定表、一套动作字符串。 */
enum vidow_bind_src {
	VIDOW_SRC_KEY = 0,
	VIDOW_SRC_MOUSE,   /* button = BTN_* */
	VIDOW_SRC_WHEEL,   /* axis + axis_dir */
};

struct vidow_keybind {
	struct wl_list link; /* vidow_server.keybinds */
	char trigger[64];    /* 原样记着，日志和 list 里用 */
	char action[128];
	uint32_t mods;       /* WLR_MODIFIER_* 的掩码，0 = 不看修饰键 */
	xkb_keysym_t keysym; /* VIDOW_SRC_KEY：主键；单独修饰键时是 NoSymbol */
	/* 抑制期间也生效（vidow.bind 第三个参数 { inhibited = true }）。 */
	bool inhibited;
	enum vidow_bind_on on;
	enum vidow_bind_src src;
	uint32_t button;            /* VIDOW_SRC_MOUSE：BTN_LEFT 这种 */
	enum wl_pointer_axis axis;  /* VIDOW_SRC_WHEEL：纵向还是横向 */
	int axis_dir;               /* VIDOW_SRC_WHEEL：+1 / -1（下/右 是 +1） */
	/* 触发里**只有修饰键**（`vidow.bind("Mod", …)`）：按一下 Super 那种。 */
	bool modifier_only;
	/* 按住键的自动重复要不要再触发一遍（niri 的 binds { repeat }，默认 true）。
	 *
	 * 注意：wlroots 0.20 **不在合成器里做自动重复**（wl_keyboard.repeat_info
	 * 只是把速率报给客户端，重复是客户端自己做的），所以 vidow 平时根本收不到
	 * 「重复的按下」——这个开关是给「万一收到」留的（虚拟键盘 / 输入法回注 /
	 * 以后 wlroots 自己重复）：那时 repeat = false 的绑定只认第一次按下。 */
	bool repeat;
	/* 两次触发之间至少隔这么多毫秒（niri 的 binds { cooldown-ms }，默认 0 =
	 * 不限）。按住一个键疯狂触发、或者一条绑定被别的路径连着调两次时挡一下。 */
	int cooldown_ms;
	int64_t last_run_ns;
};

bool vidow_keybind_init(struct vidow_server *server);

/* 加一条绑定。on_release = 松开才触发（vidow.bind 的 { on = "release" }）。
 * 失败时把原因写进 err（不超过 errlen），返回 NULL。 */
struct vidow_keybind *vidow_keybind_add(struct vidow_server *server,
		const char *trigger, const char *action, bool inhibited,
		bool on_release, bool repeat, int cooldown_ms, char *err,
		size_t errlen);

/* 清空全部绑定（config.lua 重载时用：重载等于重新声明一遍）。 */
void vidow_keybind_clear(struct vidow_server *server);

/* 有按键来了：命中哪条绑定就执行哪条，返回是否吃掉了这个按键。
 * 没命中的按键照旧透传给客户端。inhibited = 现在焦点那个表面有生效中的抑制
 * （那就只认标记了 inhibited 的那几条，见 keyboardgrab.c）。
 *
 * pressed = 按下还是松开。三种情况：
 *   1) 普通绑定 + on=press：按下时命中就执行（现在这样），吞掉那次按下；
 *   2) 普通绑定 + on=release：按下时记下来（那次按下也吞掉），松开时执行；
 *   3) 单独一个修饰键（`vidow.bind("Mod", …)`）：按下武装，期间按下任何别的
 *      键或鼠标键就解除，松开且其它修饰键都松了才执行（「按一下 Super 开全景」
 *      要的就是这个 —— 否则 Super+Q 会顺带触发）。 */
bool vidow_keybind_key(struct vidow_server *server, xkb_keysym_t sym,
		bool pressed, uint32_t mods, bool inhibited);

/* 鼠标按键 / 滚轮来了（同一个绑定表，动作字符串同一套）。返回是否吃掉。 */
bool vidow_keybind_button(struct vidow_server *server, uint32_t button,
		bool pressed, uint32_t mods, bool inhibited);
bool vidow_keybind_wheel(struct vidow_server *server,
		enum wl_pointer_axis axis, double amount, uint32_t mods,
		bool inhibited);

/* 执行一个动作字符串（键位绑定和 VIDOW_TEST_ACTION 测试钩子共用）。 */
bool vidow_action_run(struct vidow_server *server, const char *action);

/* 切 tty（真机才有会话）。键位（Ctrl+Alt+F1..F12）和 `switch-vt n` 动作都走这里。 */
bool vidow_vt_switch(struct vidow_server *server, unsigned vt);

#endif
