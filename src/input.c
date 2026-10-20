#include "gesture.h"
#include "ipc.h"
#include "server.h"
#include "sessionlock.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <wayland-server-protocol.h>
#include <wlr/backend/libinput.h>
#include <wlr/util/log.h>

#include <libinput.h>

/* 指针手势（Mod+左键拖窗口 / Mod+右键缩放 / Mod+滚轮切列）要求的修饰键。这是
 * 指针手势，不是键位 —— 键位一个内置的都没有，全在 config.lua 里绑。 */
#define MOD_KEY WLR_MODIFIER_LOGO /* Super */

/* 键位：一个内置的都没有 —— 全都由 config.lua 的 vidow.bind() 声明（默认配置在
 * configs/14-keys.lua 里给了常用的一套）。合成器只负责「按下的键有没有人绑」，
 * 没命中的按键照旧透传给客户端。
 *
 * 这里以前有一张写死的键位表（Mod+Return 起终端、Mod+Q 关窗、Mod+H/J/K/L 挪焦点、
 * Mod+V 浮动、Mod+O 全景、Mod+F 全屏、Ctrl+Alt+F1..12 切 tty …），现在那些都是
 * 动作字符串（spawn / close-window / focus-column / toggle-floating / toggle-overview
 * / toggle-fullscreen / switch-vt …），用户绑哪个键都行，也能一个都不绑。 */

/* =========================================================== 会话锁（S3）
 *
 * 锁上以后输入**只**给 lock surface（ext-session-lock-v1 XML:77-82 / 246-250）。
 * 这一节是那条路唯一的一套函数：每个输入入口（keyboard_key / modifiers、指针
 * motion / button / axis）第一行就问 server->locked，是就整条走这里、立刻 return
 * —— 命中测试（vidow_view_at）、悬停聚焦、拖拽 / 手势 / 概览 / MRU / 输入法一个
 * 都不跑（方案 §2 环节 3 的「单一 choke point」）。
 *
 * 为什么非得放每个入口的第一行：vidow 的输入入口太多（见方案 §4 风险 1），漏一条
 * 按键 / 指针就漏给被藏起来的客户端，锁就白上了。 */

/* 锁定期间键盘焦点给「光标所在那块屏」的 lock surface（方案 §2 环节 3）。 */
static void lock_focus_keyboard(struct vidow_server *server) {
	vidow_sessionlock_focus(server);
}

/* 锁定期间的一个键盘事件：白名单键位（vidow_keybind_key 里判，只放行 VT 切换）→
 * lock surface。真键盘和注入共用这一条（注入在 headless 里是唯一能造键盘事件的
 * 路，两边必须同一条，否则测出来的不是真路径）。 */
static void lock_key_route(struct vidow_server *server, struct wlr_keyboard *kb,
		uint32_t time_msec, uint32_t keycode, uint32_t state) {
	const xkb_keysym_t *syms = NULL;
	int nsyms = kb->xkb_state != NULL
		? xkb_state_key_get_syms(kb->xkb_state, keycode + 8, &syms)
		: 0;
	bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
	uint32_t mods = wlr_keyboard_get_modifiers(kb);
	/* 白名单（方案 §2 环节 3：只有 VT 切换这类逃生口）—— vidow_keybind_key 在
	 * locked 状态下自己只认白名单里的动作，其余绑定一律不跑。 */
	for (int i = 0; i < nsyms; i++) {
		vidow_keybind_key(server, syms[i], pressed, mods, false);
	}
	wlr_seat_set_keyboard(server->seat, kb);
	lock_focus_keyboard(server);
	wlr_seat_keyboard_notify_key(server->seat, time_msec, keycode, state);
}

/* 锁定期间的一个指针事件：焦点只给 lock surface，绝不落到被藏起来的窗口上。
 * 返回 false = 一块 lock surface 都还没有（调用方自己决定吞掉）。 */
static bool lock_pointer_route(struct vidow_server *server, uint32_t time_msec,
		uint32_t button, bool pressed, bool have_button) {
	double sx = 0, sy = 0;
	struct wlr_surface *surface =
		vidow_sessionlock_pointer_surface(server, &sx, &sy);
	/* 键盘焦点跟着光标所在屏走（多屏时指针挪到哪块屏，哪块屏的锁屏界面收键盘）。 */
	lock_focus_keyboard(server);
	if (surface == NULL) {
		/* 客户端还没建 lock surface：指针焦点清掉，别留给被藏起来的窗口。 */
		vidow_cursor_fx_default_cursor(server);
		wlr_seat_pointer_notify_clear_focus(server->seat);
		return false;
	}
	wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
	if (have_button) {
		wlr_seat_pointer_notify_button(server->seat, time_msec, button,
			pressed ? WL_POINTER_BUTTON_STATE_PRESSED
				: WL_POINTER_BUTTON_STATE_RELEASED);
	} else {
		wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
	}
	return true;
}

/* ======================================================= 会话锁（S3）结束 */

static void keyboard_modifiers(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_keyboard *kb = wl_container_of(listener, kb, modifiers);

	/* 正在拆（wl_display_run 已经返回）：seat 可能已经没了，别再往里塞东西。 */
	if (kb->server->shutting_down) {
		return;
	}

	/* 会话锁：修饰键也只给 lock surface。放在输入法判断**之前** —— 锁上的
	 * 时候输入法不许再抢键盘（见 im.c 里的守卫）。 */
	if (kb->server->locked) {
		wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
		lock_focus_keyboard(kb->server);
		wlr_seat_keyboard_notify_modifiers(kb->server->seat,
			&kb->wlr_keyboard->modifiers);
		return;
	}

	/* 输入法抢着键盘时修饰键也归它：它会连同按键一起用虚拟键盘回注。
	 *
	 * **但 seat 上的修饰键状态也必须跟着更新** —— 合成器自己的键位（Mod+滚轮
	 * 那类）读的是 seat->keyboard_state.modifiers，不更新就会「按着 Mod 滚一次
	 * 之后，再滚就不认了，得松开重按」（用户报的）。这里只更新状态，按键本身
	 * 仍然只发给输入法。 */
	if (vidow_im_owns_keyboard(kb->server, kb)) {
		vidow_im_modifiers(kb->server, kb->wlr_keyboard);
		wlr_seat_keyboard_notify_modifiers(kb->server->seat,
			&kb->wlr_keyboard->modifiers);
		return;
	}

	wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(kb->server->seat,
		&kb->wlr_keyboard->modifiers);

	/* Alt+Tab 切换器只在「还按着修饰键」的时候开着（niri 同此）：所有修饰键都
	 * 松开了就是「选好了」—— 把焦点给选中的那个，关掉。按住 Alt 连点 Tab 的
	 * 手势到这里结束。 */
	if (vidow_mru_active(kb->server)) {
		uint32_t held = kb->wlr_keyboard->modifiers.depressed &
			(WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL |
			 WLR_MODIFIER_SHIFT);
		if (held == 0) {
			vidow_mru_commit(kb->server);
		}
	}
}

/* 有键按下了：打字就藏光标（hide_when_typing）+ 唤醒关掉的屏
 * （key_press_enables_dpms）—— 两个开关默认都关。真键盘和注入的键都走这里：
 * 「人按了键」这件事不分键盘从哪来（注入那条路平时不触发键位，但活动是活动）。 */
static void input_note_key_press(struct vidow_server *server) {
	if (server->config.hide_when_typing) {
		vidow_cursor_fx_set_hidden(server, true);
	}
	if (server->config.key_press_enables_dpms) {
		vidow_output_wake_disabled(server);
	}
}

static void keyboard_key(struct wl_listener *listener, void *data) {
	struct vidow_keyboard *kb = wl_container_of(listener, kb, key);
	struct vidow_server *server = kb->server;
	struct wlr_keyboard_key_event *event = data;

	/* 正在拆：wlr_keyboard_finish() 会给每个还按着的键补一条**松开**事件，走到
	 * 下面那条 wlr_seat_set_keyboard(server->seat) 时 seat 可能已经释放了
	 * （coredump：keyboard_key -> wlr_seat_set_keyboard，退出时崩）。退出路上
	 * 谁按着什么键已经没人关心，直接丢。 */
	if (server->shutting_down) {
		return;
	}

	/* evdev keycode -> xkb keycode */
	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state, keycode, &syms);


	bool handled = false;
	uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);
	/* 焦点那个表面上有个生效中的「快捷键抑制」（虚拟机的窗口要原样收 Super /
	 * Alt+Tab，见 keyboardgrab.c）时，vidow 自己的键位全部让路 —— 客户端要的
	 * 就是这些键。绑定里写了 { inhibited = true } 的那几条是逃生口，照样生效
	 * （不然抑制着的窗口能把用户锁死在里面，连关窗都关不了）。 */
	bool inhibited = vidow_shortcuts_inhibited(server);
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		input_note_key_press(server);
	}
	/* 会话锁：锁定期间键盘只给 lock surface（白名单里的 VT 切换除外，见
	 * lock_key_route）。放在 input_note_key_press 之后 —— key_press_enables_dpms
	 * 锁定期间照样要生效，屏关着的时候得能按键唤醒（方案 §4 风险 2）。 */
	if (server->locked) {
		lock_key_route(server, kb->wlr_keyboard, event->time_msec,
			event->keycode, event->state);
		return;
	}
	/* config.lua 里 vidow.bind() 声明的键位（动作字符串见 README 的动作表）：
	 * 工作区切换 / 窗口与列的方向移动 / 浮动 / 全屏 / 全景 / 起命令 / 切 tty …
	 * 一个内置的都不预设，绑什么是什么；没命中的按键透传给客户端。 */
	/* 按下和松开都要过绑定表：「松开才触发」的绑定和「单独一个修饰键」都靠松开
	 * 那一下收尾（见 keybind.c 的 pending_release）。 */
	bool pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
	for (int i = 0; i < nsyms; i++) {
		handled = vidow_keybind_key(server, syms[i], pressed, mods,
			inhibited) || handled;
	}

	/* Alt+Tab 切换器开着（**按住修饰键期间，从按下 Alt 那一刻算起**）：键盘也归
	 * 它管 —— 窗口马上就要换，按键给谁都可能给错。Esc 取消、Enter 确认、方向键
	 * 换个格（Left/Up = 往前、Right/Down = 往后，和滚轮同一套 clamp 步进：**到边
	 * 停住、不循环**）；Alt+Tab 那两条绑定在上面已经处理过了（键会重复，每重复
	 * 一次就是往前走一格 —— 那条路是「到头绕回」，故意和方向键 / 滚轮不一样）。
	 * 没绑的键（打字那些）到这里一律吃掉，不透给客户端。 */
	if (vidow_mru_active(server)) {
		if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && nsyms > 0) {
			if (syms[0] == XKB_KEY_Escape) {
				vidow_mru_cancel(server);
			} else if (syms[0] == XKB_KEY_Return ||
					syms[0] == XKB_KEY_KP_Enter) {
				vidow_mru_commit(server);
			} else if (syms[0] == XKB_KEY_Left ||
					syms[0] == XKB_KEY_Up) {
				vidow_mru_step_clamp(server, -1);
			} else if (syms[0] == XKB_KEY_Right ||
					syms[0] == XKB_KEY_Down) {
				vidow_mru_step_clamp(server, 1);
			}
		}
		return;
	}

	/* overview 开着的时候它接管键盘：Esc 退出，别的键也别漏给客户端
	 * （窗口都缩没了，按键给谁都不对）。Super+… 的快捷键在上面照常生效。 */
	if (vidow_overview_active(server)) {
		if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && nsyms > 0 &&
				syms[0] == XKB_KEY_Escape) {
			vidow_overview_exit(server);
		}
		return;
	}

	/* 输入法要吃原始按键，由它决定消费还是用虚拟键盘回注。只对「被抢走的
	 * 那个」键盘这么做：输入法回注用的是它自己的虚拟键盘，否则会转圈。
	 * 我们自己的快捷键（Super+…）在上面已经处理过了，仍然能用。 */
	if (!handled && vidow_im_owns_keyboard(server, kb)) {
		vidow_im_key(server, event->time_msec, event->keycode, event->state);
		return;
	}

	if (!handled) {
		/* 往窗口里打字 = 明确使用它：MRU 的 debounce 立刻结束（见 mru.c）——
		 * 不然「刚点过去的窗口，马上打字，再 Alt+Tab」会回错地方。 */
		if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
			vidow_mru_note_key(server);
		}
		/* X11 抓着键盘时按键一律给抓着它的那个表面（XGrabKeyboard 的语义：谁抓
		 * 谁收，用户点了别的窗口也照给）—— 见 keyboardgrab.c。 */
		vidow_xwayland_grab_assert(server);
		/* 输入法抓着键盘 → 按键归它。修饰键那条早就有 vidow_im_owns_keyboard
		 * 这个判据（见上面的 modifiers 分支），只有按键这条漏了 —— 于是
		 * fcitx5 一抓键盘，按键被**丢掉**（既不到它也不到客户端）= 用户报的
		 * 「启动了 Fcitx5 之后键盘都不能用了」。用同一个判据，锁定守卫也一起有。 */
		if (vidow_im_owns_keyboard(server, kb)) {
			vidow_im_grab_send_key(event->time_msec, event->keycode,
				event->state == WL_KEYBOARD_KEY_STATE_PRESSED);
			return;
		}
		wlr_seat_set_keyboard(server->seat, kb->wlr_keyboard);
		wlr_seat_keyboard_notify_key(server->seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_keyboard *kb = wl_container_of(listener, kb, destroy);

	wl_list_remove(&kb->modifiers.link);
	wl_list_remove(&kb->key.link);
	wl_list_remove(&kb->destroy.link);
	wl_list_remove(&kb->link);

	/* 别让 seat 指着一个马上要释放的键盘 */
	struct vidow_server *server = kb->server;
	if (wlr_seat_get_keyboard(server->seat) == kb->wlr_keyboard) {
		struct vidow_keyboard *next = NULL;
		if (!wl_list_empty(&server->keyboards)) {
			next = wl_container_of(server->keyboards.next, next, link);
		}
		wlr_seat_set_keyboard(server->seat, next != NULL ? next->wlr_keyboard : NULL);
	}
	free(kb);
}

/* 键盘自己的参数（configs/21-keyboard.lua）：
 *   repeat_rate / repeat_delay -> wl_keyboard.repeat_info（客户端自己重复）
 *   numlock                    -> 起手把 NumLock 点亮
 * 两者都是「设备上/客户端上」的状态，所以插键盘、配置改了都要重套一遍。 */
static void keyboard_apply_config(struct vidow_server *server,
		struct wlr_keyboard *kb) {
	if (kb == NULL) {
		return;
	}
	const struct vidow_config *cfg = &server->config;
	wlr_keyboard_set_repeat_info(kb, cfg->repeat_rate, cfg->repeat_delay);

	/* NumLock：xkb 的锁定修饰位（一般叫 Mod2）—— 点亮它 = 客户端看到
	 * wl_keyboard.modifiers 里 NumLock 锁着、小键盘出数字。默认关（改前不碰）。 */
	if (!cfg->numlock || kb->keymap == NULL || kb->xkb_state == NULL) {
		return;
	}
	xkb_mod_index_t idx = xkb_keymap_mod_get_index(kb->keymap, XKB_MOD_NAME_NUM);
	if (idx == XKB_MOD_INVALID) {
		wlr_log(WLR_DEBUG, "numlock: 这份 keymap 里没有 Mod2，跳过");
		return;
	}
	xkb_mod_mask_t mask = (xkb_mod_mask_t)1 << idx;
	if ((kb->modifiers.locked & mask) != 0) {
		return; /* 已经亮着（按过 NumLock 键了） */
	}
	xkb_state_update_mask(kb->xkb_state, kb->modifiers.depressed,
		kb->modifiers.latched, kb->modifiers.locked | mask, 0, 0,
		kb->modifiers.group);
	kb->modifiers.locked |= mask;
	/* 把新的修饰位发给客户端（wlroots 0.20 没有 keyboard 级的 notify，走 seat
	 * 那一个）。不是 seat 正拿着的那把键盘就别发 —— 那会把别人的修饰状态带歪。 */
	if (wlr_seat_get_keyboard(server->seat) == kb) {
		wlr_seat_keyboard_notify_modifiers(server->seat, &kb->modifiers);
	}
	wlr_log(WLR_INFO, "键盘: numlock = true，NumLock 起手点亮");
}

static void server_add_keyboard(struct vidow_server *server,
		struct wlr_keyboard *wlr_keyboard, struct wlr_input_device *device,
		struct wlr_virtual_keyboard_v1 *virtual, bool inject) {
	struct vidow_keyboard *kb = calloc(1, sizeof(*kb));
	if (kb == NULL) {
		return;
	}
	kb->server = server;
	kb->wlr_keyboard = wlr_keyboard;
	kb->virtual = virtual;
	kb->inject = inject;

	/* 虚拟键盘自己会送 keymap（协议要求），别用我们的覆盖它：输入法回注的
	 * 按键是按它自己的 keymap 发的，覆盖了就会解释成别的键（ESC 关窗口）。 */
	/* 虚拟键盘（输入法回注用的）：它通常会自己送 keymap（协议要求），那就不
	 * 动它 —— 覆盖了会把回注的键解释成别的键。但它**没送**的时候必须补一份：
	 * 实测 fcitx5 就不送（`vidowmsg seats` 里 layout: null），空 keymap 让客户端
	 * 翻译不出任何键，表现就是「输入法开着，一个字都打不出来，只有合成器自己
	 * 的快捷键还能用」——用户报的就是这个。 */
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	bool need_keymap = virtual == NULL || wlr_keyboard->keymap == NULL;
	if (context != NULL && need_keymap) {
		struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (keymap != NULL) {
			wlr_keyboard_set_keymap(wlr_keyboard, keymap);
			xkb_keymap_unref(keymap);
		}
		xkb_context_unref(context);
	}
	/* 重复速率 / 延迟和 NumLock 都从配置来（默认 25 / 600 / 不点亮 = 改前行为）。 */
	keyboard_apply_config(server, wlr_keyboard);

	kb->modifiers.notify = keyboard_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &kb->modifiers);
	kb->key.notify = keyboard_key;
	wl_signal_add(&wlr_keyboard->events.key, &kb->key);
	kb->destroy.notify = keyboard_destroy;
	wl_signal_add(&device->events.destroy, &kb->destroy);

	wl_list_insert(&server->keyboards, &kb->link);
	wlr_seat_set_keyboard(server->seat, wlr_keyboard);
	/* 输入法正抢着键盘时，得让它知道键盘换了，否则它拿不到 keymap */
	vidow_im_set_keyboard(server, wlr_keyboard);
}

static void server_new_keyboard(struct vidow_server *server,
		struct wlr_input_device *device) {
	server_add_keyboard(server, wlr_keyboard_from_input_device(device), device,
		NULL, false);
}

/* 座位现在有什么能力。每加一个输入设备（真设备 / 虚拟键盘 / 虚拟指针）都要重算
 * 一次：**少了这一步客户端会一直以为「这个座位没有指针」**，于是根本不会收到
 * 指针事件（headless / 只用虚拟指针的会话里尤其明显 —— 那种会话里一台真设备
 * 都没有，server_new_input 永远不会跑）。 */
static void seat_update_capabilities(struct vidow_server *server) {
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

/* wtype / wlrctl and friends inject input through this protocol. */
static void input_device_note(struct vidow_server *server,
		struct wlr_input_device *device); /* 定义在下面（设备账本 + 事件） */

static void server_new_virtual_keyboard(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *virtual_keyboard = data;

	wlr_log(WLR_INFO, "virtual keyboard attached");
	input_device_note(server, &virtual_keyboard->keyboard.base);
	server_add_keyboard(server, &virtual_keyboard->keyboard,
		&virtual_keyboard->keyboard.base, virtual_keyboard, false);

	seat_update_capabilities(server);
}

/* 设备类型名字（事件里报出来）。 */
static const char *device_type_name(enum wlr_input_device_type type) {
	switch (type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		return "keyboard";
	case WLR_INPUT_DEVICE_POINTER:
		return "pointer";
	case WLR_INPUT_DEVICE_TOUCH:
		return "touch";
	case WLR_INPUT_DEVICE_TABLET:
		return "tablet";
	case WLR_INPUT_DEVICE_TABLET_PAD:
		return "tablet-pad";
	case WLR_INPUT_DEVICE_SWITCH:
		return "switch";
	}
	return "?";
}

static void input_device_gone(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_input_device *dev =
		wl_container_of(listener, dev, destroy);
	wl_list_remove(&dev->destroy.link);
	wl_list_remove(&dev->link);
	/* 先报事件再放掉名字：钩子命令的环境变量是从这条载荷拼的 */
	const struct vidow_event_field fields[] = {
		VIDOW_EV_STR("name", dev->name),
		VIDOW_EV_STR("type", dev->type),
		VIDOW_EV_END,
	};
	vidow_ipc_emit_simple(dev->server, "input-device-removed", fields);
	free(dev);
}

/* 记一笔「这个设备连上来了」+ 报事件；拔的那一下由 destroy 信号收（见
 * input_device_gone）。 */
static void input_device_note(struct vidow_server *server,
		struct wlr_input_device *device) {
	struct vidow_input_device *dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		return;
	}
	dev->server = server;
	dev->device = device;
	snprintf(dev->name, sizeof(dev->name), "%s",
		device->name != NULL ? device->name : "(无名)");
	snprintf(dev->type, sizeof(dev->type), "%s",
		device_type_name(device->type));
	dev->destroy.notify = input_device_gone;
	wl_signal_add(&device->events.destroy, &dev->destroy);
	wl_list_insert(server->input_devices.prev, &dev->link);

	const struct vidow_event_field fields[] = {
		VIDOW_EV_STR("name", dev->name),
		VIDOW_EV_STR("type", dev->type),
		VIDOW_EV_END,
	};
	vidow_ipc_emit_simple(server, "input-device-added", fields);
}

void vidow_input_finish(struct vidow_server *server) {
	struct vidow_input_device *dev, *tmp;
	wl_list_for_each_safe(dev, tmp, &server->input_devices, link) {
		wl_list_remove(&dev->destroy.link);
		wl_list_remove(&dev->link);
		free(dev);
	}
	/* 账本没了，配置改了就别再往里套设置了（配置重载理论上还能跑到这儿）。 */
	vidow_config_observe(NULL, NULL);
}

/* ------------------------------------------------- libinput 设备配置 */

/* 配置里那一族 libinput 设置（configs/19-input.lua，照 niri 的 input {} 那一节）
 * 套到设备上。两个时机：
 *
 *   1. 设备插上来 —— server_new_input 那条路，热插拔也是它；
 *   2. 配置改了 —— config.c 的观察者（见 vidow_input_init）。设置是**存在设备
 *      上**的，配置重载 / `vidowmsg set-config` 不会自动跟过去，得重套一遍。
 *
 * 只认 libinput 的设备：虚拟键盘 / 虚拟指针（wtype、vidowmsg send-key 那些）不是
 * libinput 的，wlr_libinput_get_device_handle() 对它们会 assert 掉。
 *
 * 没配的项一概不碰（三态 AUTO / 空串 / accel_speed 没设）：这是「不配置时零
 * 变化」的落点 —— vidow 以前一个 libinput 设置都不碰，而 libinput 的默认值随
 * 设备和 udev 规则走，替用户写死一个常数就等于改了他没配的东西。 */

/* 三态 → 日志里的一个词。AUTO 不会走到这儿（没配就不打）。 */
static const char *tri_text(enum vidow_tristate v) {
	return v == VIDOW_TRI_ON ? "on" : "off";
}

/* 往「这台设备上真设了什么」那行里追加一项。默认全是「不碰」，所以打一串
 * auto / - 没意义，只记真的动过的。 */
static void applied_add(char *buf, size_t size, const char *what,
		const char *value) {
	snprintf(buf + strlen(buf), size - strlen(buf), "%s%s=%s",
		buf[0] != '\0' ? " " : "", what, value);
}

void vidow_input_apply_libinput(const struct vidow_config *cfg,
		struct libinput_device *dev, const char *name) {
	if (cfg == NULL || dev == NULL) {
		return;
	}
	char done[256] = "";
	char num[32];

	/* 点按 = 左键。finger count == 0 就是这台设备不支持点按（鼠标 / 老触控板）。 */
	if (cfg->tap != VIDOW_TRI_AUTO &&
			libinput_device_config_tap_get_finger_count(dev) > 0) {
		libinput_device_config_tap_set_enabled(dev,
			cfg->tap == VIDOW_TRI_ON ? LIBINPUT_CONFIG_TAP_ENABLED :
			LIBINPUT_CONFIG_TAP_DISABLED);
		applied_add(done, sizeof(done), "tap", tri_text(cfg->tap));
	}
	/* 打字时关触控板（disable-while-typing）。 */
	if (cfg->dwt != VIDOW_TRI_AUTO &&
			libinput_device_config_dwt_is_available(dev)) {
		libinput_device_config_dwt_set_enabled(dev,
			cfg->dwt == VIDOW_TRI_ON);
		applied_add(done, sizeof(done), "dwt", tri_text(cfg->dwt));
	}
	/* 点按拖拽时松手不立刻放手。跟 tap 一样要求设备支持点按。 */
	if (cfg->drag_lock != VIDOW_TRI_AUTO &&
			libinput_device_config_tap_get_finger_count(dev) > 0) {
		libinput_device_config_tap_set_drag_lock_enabled(dev,
			cfg->drag_lock == VIDOW_TRI_ON ?
			LIBINPUT_CONFIG_DRAG_LOCK_ENABLED :
			LIBINPUT_CONFIG_DRAG_LOCK_DISABLED);
		applied_add(done, sizeof(done), "drag_lock",
			tri_text(cfg->drag_lock));
	}
	/* 反向滚动。 */
	if (cfg->natural_scroll != VIDOW_TRI_AUTO &&
			libinput_device_config_scroll_has_natural_scroll(dev)) {
		libinput_device_config_scroll_set_natural_scroll_enabled(dev,
			cfg->natural_scroll == VIDOW_TRI_ON);
		applied_add(done, sizeof(done), "natural_scroll",
			tri_text(cfg->natural_scroll));
	}
	/* 左右键对调。 */
	if (cfg->left_handed != VIDOW_TRI_AUTO &&
			libinput_device_config_left_handed_is_available(dev)) {
		libinput_device_config_left_handed_set(dev,
			cfg->left_handed == VIDOW_TRI_ON);
		applied_add(done, sizeof(done), "left_handed",
			tri_text(cfg->left_handed));
	}
	/* 左右键同时按 = 中键。 */
	if (cfg->middle_emulation != VIDOW_TRI_AUTO &&
			libinput_device_config_middle_emulation_is_available(dev)) {
		libinput_device_config_middle_emulation_set_enabled(dev,
			cfg->middle_emulation == VIDOW_TRI_ON);
		applied_add(done, sizeof(done), "middle_emulation",
			tri_text(cfg->middle_emulation));
	}
	/* 加速曲线 / 加速强度（两个都要求这台设备支持加速）。 */
	if (libinput_device_config_accel_is_available(dev)) {
		if (cfg->accel_profile[0] != '\0') {
			enum libinput_config_accel_profile profile =
				strcmp(cfg->accel_profile, "flat") == 0 ?
				LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT :
				LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
			libinput_device_config_accel_set_profile(dev, profile);
			applied_add(done, sizeof(done), "accel_profile",
				cfg->accel_profile);
		}
		if (cfg->accel_speed.set) {
			libinput_device_config_accel_set_speed(dev,
				cfg->accel_speed.value);
			snprintf(num, sizeof(num), "%.3f",
				cfg->accel_speed.value);
			applied_add(done, sizeof(done), "accel_speed", num);
		}
	}
	/* 什么时候把指针移动当滚动。设备支持的方法是个位掩码，写了个它不支持的
	 * 就跳过并在日志里说清楚（不然用户只会看到「配了没反应」）。 */
	if (cfg->scroll_method[0] != '\0') {
		enum libinput_config_scroll_method method;
		uint32_t supported = libinput_device_config_scroll_get_methods(dev);
		bool known = true;
		if (strcmp(cfg->scroll_method, "no-scroll") == 0) {
			method = LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
		} else if (strcmp(cfg->scroll_method, "two-finger") == 0) {
			method = LIBINPUT_CONFIG_SCROLL_2FG;
		} else if (strcmp(cfg->scroll_method, "edge") == 0) {
			method = LIBINPUT_CONFIG_SCROLL_EDGE;
		} else if (strcmp(cfg->scroll_method, "on-button-down") == 0) {
			method = LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN;
		} else {
			known = false;
			method = LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
		}
		if (!known) {
			wlr_log(WLR_ERROR, "input: %s：scroll_method '%s' 不认识"
				"（只能是 no-scroll / two-finger / edge / "
				"on-button-down），这台设备上不生效", name,
				cfg->scroll_method);
		} else if ((supported & (uint32_t)method) == 0) {
			wlr_log(WLR_INFO, "input: %s：这台设备不支持 scroll_method "
				"%s，跳过", name, cfg->scroll_method);
		} else {
			libinput_device_config_scroll_set_method(dev, method);
			applied_add(done, sizeof(done), "scroll_method",
				cfg->scroll_method);
		}
	}
	/* 触控板的按键怎么划分（整块按下去 / 几根手指 = 哪个键）。 */
	if (cfg->click_method[0] != '\0') {
		enum libinput_config_click_method method;
		uint32_t supported = libinput_device_config_click_get_methods(dev);
		bool known = true;
		if (strcmp(cfg->click_method, "button-areas") == 0) {
			method = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
		} else if (strcmp(cfg->click_method, "clickfinger") == 0) {
			method = LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER;
		} else {
			known = false;
			method = LIBINPUT_CONFIG_CLICK_METHOD_NONE;
		}
		if (!known) {
			wlr_log(WLR_ERROR, "input: %s：click_method '%s' 不认识"
				"（只能是 button-areas / clickfinger），这台设备上"
				"不生效", name, cfg->click_method);
		} else if ((supported & (uint32_t)method) == 0) {
			wlr_log(WLR_INFO, "input: %s：这台设备不支持 click_method "
				"%s，跳过", name, cfg->click_method);
		} else {
			libinput_device_config_click_set_method(dev, method);
			applied_add(done, sizeof(done), "click_method",
				cfg->click_method);
		}
	}

	/* 一行一台设备，只在真设了什么的时候打。scroll_factor 不在这儿 —— 它不是
	 * 设备属性，是 vidow 自己在滚轮事件上乘的（见 vidow_pointer_axis）。 */
	if (done[0] != '\0') {
		wlr_log(WLR_INFO, "input: %s：%s", name, done);
	}
}

/* toggle-touchpad 动作（configs/14-keys.lua 里绑一条）：把触控板整体关掉 / 打开。
 * 只认 libinput 的**触控板**（有 tap 能力的设备）—— 鼠标不碰。全开着就全关，
 * 全关着就全开（一个动作来回切）。关 = libinput 的 send-events 置 DISABLED，
 * 设备本身还在，配置 / 热插拔照旧。 */
bool vidow_input_toggle_touchpad(struct vidow_server *server, const char **why) {
	int touchpads = 0, disabled = 0;
	struct vidow_input_device *dev;
	wl_list_for_each(dev, &server->input_devices, link) {
		if (!wlr_input_device_is_libinput(dev->device)) {
			continue;
		}
		struct libinput_device *d =
			wlr_libinput_get_device_handle(dev->device);
		if (d == NULL || libinput_device_config_tap_get_finger_count(d) <= 0) {
			continue; /* 不是触控板 */
		}
		touchpads++;
		if (libinput_device_config_send_events_get_mode(d) ==
				LIBINPUT_CONFIG_SEND_EVENTS_DISABLED) {
			disabled++;
		}
	}
	if (touchpads == 0) {
		if (why != NULL) {
			*why = "没有触控板（libinput 的 tap 能力设备）";
		}
		return false;
	}
	bool enable = disabled == touchpads; /* 全关着 -> 全开；否则全关 */
	enum libinput_config_send_events_mode mode = enable ?
		LIBINPUT_CONFIG_SEND_EVENTS_ENABLED :
		LIBINPUT_CONFIG_SEND_EVENTS_DISABLED;
	int n = 0;
	wl_list_for_each(dev, &server->input_devices, link) {
		if (!wlr_input_device_is_libinput(dev->device)) {
			continue;
		}
		struct libinput_device *d =
			wlr_libinput_get_device_handle(dev->device);
		if (d == NULL || libinput_device_config_tap_get_finger_count(d) <= 0) {
			continue;
		}
		libinput_device_config_send_events_set_mode(d, mode);
		n++;
	}
	wlr_log(WLR_INFO, "触控板: %s（%d 块）", enable ? "打开" : "关掉", n);
	if (why != NULL) {
		*why = enable ? "触控板已打开" : "触控板已关掉";
	}
	return true;
}

/* ------------------------------------------------------------ 光标隐藏
 *
 * hide_when_typing / hide_after_inactive_ms（configs/13-cursor.lua，照 niri 的
 * cursor {} 那一节）。两个开关共用「藏」这一个状态，显示回来的那条路只有一条：
 * 指针一动（vidow_pointer_motion）。 */
static int cursor_hide_timeout(void *data) {
	struct vidow_server *server = data;
	vidow_cursor_fx_set_hidden(server, true);
	return 0;
}

/* 重新计时：hide_after_inactive_ms <= 0 就把闹钟停掉。 */
static void cursor_hide_arm(struct vidow_server *server) {
	if (server->cursor_hide_timer == NULL) {
		return;
	}
	int ms = server->config.hide_after_inactive_ms;
	wl_event_source_timer_update(server->cursor_hide_timer, ms > 0 ? ms : 0);
}

/* 指针动了：光标回来 + 重新计时（藏 / 显示都走 cursorfx.c 那一处）。 */
static void cursor_wake(struct vidow_server *server) {
	vidow_cursor_fx_set_hidden(server, false);
	cursor_hide_arm(server);
}

/* 一台 wlr 设备：是 libinput 的才往下走（虚拟设备会被 wlroots 那套 assert 掉）。 */
static void input_apply_device(const struct vidow_config *cfg,
		struct wlr_input_device *device) {
	if (device == NULL || !wlr_input_device_is_libinput(device)) {
		return;
	}
	struct libinput_device *dev = wlr_libinput_get_device_handle(device);
	if (dev == NULL) {
		return;
	}
	vidow_input_apply_libinput(cfg, dev,
		device->name != NULL ? device->name : "(无名)");
}

void vidow_input_apply_all(struct vidow_server *server) {
	struct vidow_input_device *dev;
	wl_list_for_each(dev, &server->input_devices, link) {
		input_apply_device(&server->config, dev->device);
	}
}

/* 配置改了（三条写路径最后都落到 config.c 的 apply()）：只有这一族的键变了才
 * 重套，别的键（gap、动画…）改一次重套一遍所有设备是白费。key == NULL =
 * 整份配置都可能变了（配置重载先恢复默认值，那一步不走单个键）。 */
static const char *input_config_keys[] = {
	"tap", "dwt", "natural_scroll", "left_handed", "middle_emulation",
	"drag_lock", "accel_profile", "accel_speed", "scroll_method",
	"click_method",
};

/* 键盘自己的那几个键（configs/21-keyboard.lua）+ 光标隐藏那两个：改了要立刻
 * 生效 —— 键盘参数是「设备上 / 客户端上」的状态，光标隐藏要重设闹钟。 */
static const char *keyboard_config_keys[] = {
	"repeat_delay", "repeat_rate", "numlock",
};

static void keyboard_apply_all(struct vidow_server *server) {
	struct vidow_keyboard *kb;
	wl_list_for_each(kb, &server->keyboards, link) {
		keyboard_apply_config(server, kb->wlr_keyboard);
	}
}

static void input_config_changed(struct vidow_config *cfg, const char *key,
		void *data) {
	(void)cfg; /* 和 server->config 是同一份 */
	struct vidow_server *server = data;
	if (key != NULL) {
		bool ours = false;
		for (size_t i = 0;
				i < sizeof(input_config_keys) / sizeof(input_config_keys[0]);
				i++) {
			if (strcmp(key, input_config_keys[i]) == 0) {
				ours = true;
				break;
			}
		}
		if (!ours) {
			bool keyboard = false;
			for (size_t i = 0;
					i < sizeof(keyboard_config_keys) /
						sizeof(keyboard_config_keys[0]); i++) {
				if (strcmp(key, keyboard_config_keys[i]) == 0) {
					keyboard = true;
					break;
				}
			}
			if (keyboard) {
				keyboard_apply_all(server);
				return;
			}
			if (strcmp(key, "hide_when_typing") == 0 ||
					strcmp(key, "hide_after_inactive_ms") == 0) {
				if (server->config.hide_after_inactive_ms <= 0) {
					/* 关掉了：把藏起来的光标放回来。 */
					cursor_wake(server);
				} else {
					cursor_hide_arm(server);
				}
				return;
			}
			return;
		}
	}
	vidow_input_apply_all(server);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	struct vidow_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;

	input_device_note(server, device);
	/* 设备属性是存在设备上的：插上来就得套一遍（热插拔也走这儿，所以不只是
	 * 启动时那一次）。虚拟设备在里面被滤掉。 */
	input_apply_device(&server->config, device);
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		wlr_cursor_attach_input_device(server->cursor, device);
		break;
	default:
		break;
	}

	seat_update_capabilities(server);
}

/* --------------------------------------------------------------- pointer */

/* 拖着东西的时候指针事件全归 grab：平铺拖拽走 drag.c，浮动窗口走 float.c。 */
static void pointer_grab_update(struct vidow_server *server) {
	if (server->grab_mode == VIDOW_GRAB_PAN) {
		vidow_pan_motion(server);
	}
	if (server->grab_mode == VIDOW_GRAB_TILE) {
		vidow_tile_grab_update(server, server->cursor->x, server->cursor->y);
		return;
	}
	if (server->grab_mode == VIDOW_GRAB_RESIZE_TILE) {
		vidow_tile_resize_update(server, server->cursor->x, server->cursor->y);
		return;
	}
	vidow_float_grab_update(server, server->cursor->x, server->cursor->y);
}

/* 焦点跟随光标（niri 的 input { focus-follows-mouse }）：指针压到哪个窗口，
 * 哪个窗口就接键盘。**鼠标悬停是用户的显式动作**，所以全屏窗口也照跟 —— 不给
 * 全屏开特例（那条「状态变化不许抢焦点」的规矩管的是窗口自己变全屏时的那次
 * 状态变更，不是这里）。
 *
 * max-scroll-amount 那个子项照 niri 的算法：先算「聚焦它条带要滚多少」，超过
 * 「工作区宽度的百分之几」就不聚焦（"0%" = 只有已经整个在屏上的窗口才跟）。
 * 不写 = 不限。 */
/* 「这一趟指针位移是键盘换焦点顺手带过去的」——vidow_warp_cursor_to_focus 用，
 * 免得刚聚焦的窗口又被自己带过去的那一下悬停聚焦抢给浮窗。 */
static bool warp_in_progress = false;

static void focus_follows_mouse(struct vidow_server *server,
		struct vidow_view *hit) {
	/* 会话锁（S3）：锁定期间**必须不生效** —— 被藏起来的窗口一个都不该因为
	 * 鼠标经过而拿到键盘焦点（方案 §4 风险 2：这条不能靠默认值碰运气，
	 * 用户把 focus_follows_mouse 配成 true 也照样不许跟）。 */
	if (server->locked) {
		return;
	}
	/* 记下「指针现在压着谁」，并拿到「上一次压着谁」。
	 * 焦点只在这一格**从别的窗口换过来**时才跟着走 —— 键盘刚把焦点切到 B、
	 * 指针还停在 A 里的时候，在 A 里挪一下不该把焦点抢回 A（用户报的
	 * 「窗口焦点移动后…否则会和焦点切换有冲突」）。离开再回来 = 变了，才跟。 */
	struct vidow_view *prev_hover = server->hovered_view;
	server->hovered_view = hit;
	/* 小窗（S2）：迷你 / 图标**不跟**（和锁屏那条同一个位置、同一个形状）。
	 * 它们不接键盘，鼠标扫过屏幕边缘时不该把焦点从正在打字的窗口上拽走。 */
	if (hit != NULL && (hit->mini == VIDOW_MINI_MINI ||
			hit->mini == VIDOW_MINI_ICON)) {
		return;
	}
	if (warp_in_progress) {
		/* 这一趟指针位移是「键盘换焦点顺手把光标带过去」造成的（见
		 * vidow_warp_cursor_to_focus），不是用户在挪鼠标。 */
		return;
	}
	if (!server->config.focus_follows_mouse || hit == NULL ||
			hit == server->focused) {
		return;
	}
	/* 指针没离开过这个窗口（上一次压着的也是它）：不跟。 */
	if (hit == prev_hover) {
		return;
	}
	const char *limit = server->config.focus_follows_mouse_max_scroll_amount;
	if (limit[0] != '\0') {
		double pct = 0.0;
		if (vidow_config_parse_percent(limit, &pct)) {
			struct vidow_output *output = hit->column != NULL &&
				hit->column->output != NULL ?
				vidow_output_lookup(server, hit->column->output) : NULL;
			if (output != NULL) {
				struct wlr_box work = vidow_work_area(server,
					output->wlr_output);
				double allowed = work.width * pct / 100.0;
				double amount =
					vidow_view_scroll_amount_to_activate(server, hit);
				if (amount > allowed) {
					wlr_log(WLR_DEBUG, "悬停聚焦: %s 要滚 %.0fpx"
						"（限 %s），这次不跟",
						vidow_view_title(hit) != NULL ?
							vidow_view_title(hit) : "(无题)",
						amount, limit);
					return;
				}
			}
		}
	}
	vidow_focus_view(hit);
}

/* ================================================ 小窗（mini window，S2）
 *
 * 迷你 / 图标态**不把任何输入转给客户端**（键盘靠 view.c 的焦点守卫，指针靠这里），
 * 但合成器自己仍然认得它：单击 = 放大（图标 -> 迷你，迷你 -> 自由小窗），拖拽 =
 * 移动它。自由小窗（FREE）照常收输入，这条分流不碰它。
 *
 * ⚠️ 和会话锁（S3）那套**方向相反**：锁屏是「每个入口第一行整条短路」（连命中测试
 * 都不跑），这里是「命中照跑（vidow_view_at 必须能命中迷你态 —— 单击要能放大，
 * 这是和锁屏唯一相反的地方），只有**转给客户端那一份**吞掉」。所以分流点放在命中
 * 测试之后，不在函数第一行。
 *
 * 单击 vs 拖拽按**累计位移**（hypot(dx,dy)，不是每帧增量 —— 慢速拖拽每帧只动 1px，
 * 按增量永远跨不过阈值）和 mini_drag_threshold_px（默认 4 逻辑像素）区分：
 *   ≤ 阈值 松手 = 单击 = 放大；> 阈值 = 拖拽（单向不回退，判成拖拽就锁到松手，
 *   否则慢速拖出去再拖回来会在松手那一刻突然放大）。
 */

/* 这个窗口现在是不是「只看不摸」的形态（迷你 / 图标）。 */
static bool mini_swallows_input(const struct vidow_view *view) {
	return view != NULL && (view->mini == VIDOW_MINI_MINI ||
		view->mini == VIDOW_MINI_ICON);
}

/* 把客户端那一份指针焦点清掉：光标底下的窗口换成小窗了，它不该再收 enter /
 * motion（只清别人的，没拿着焦点就什么都不做）。 */
static void mini_pointer_clear_client_focus(struct vidow_server *server) {
	if (server->seat->pointer_state.focused_surface != NULL) {
		wlr_seat_pointer_notify_clear_focus(server->seat);
	}
}

/* 拖拽中：把窗口挪到「按下点 + 位移」，再折回**比例锚点**（小窗的位置语义就是
 * 比例锚点，见 float.c 的 vidow_mini_resolve_geom）—— 不直接写 float_geom，
 * 那样下一次 arrange 会被锚点算回来。 */
static void mini_grab_update(struct vidow_server *server) {
	struct vidow_view *view = server->mini_grab_view;
	if (view == NULL) {
		return;
	}
	if (!server->mini_grab_dragging) {
		double dx = server->cursor->x - server->mini_grab_press_x;
		double dy = server->cursor->y - server->mini_grab_press_y;
		double threshold = (double)server->config.mini_drag_threshold_px;
		if (threshold < 0.0) {
			threshold = 0.0;
		}
		if (hypot(dx, dy) <= threshold) {
			return; /* 还没越过阈值：当它还是单击，什么都不做 */
		}
		server->mini_grab_dragging = true; /* 单向：判成拖拽就锁到松手 */
	}
	struct wlr_box work = vidow_work_area(server, view->output);
	if (work.width < 1 || work.height < 1) {
		return;
	}
	double x = server->cursor->x - server->mini_grab_dx;
	double y = server->cursor->y - server->mini_grab_dy;
	/* 锚点 0 = 贴工作区左 / 上边，1 = 贴右 / 下边。迷你态**和自由态一样**允许
	 * 越界（allow_over = true）—— 用户原话「迷你态在工作区有左右死区，无法拖拽
	 * 过去，自由态可以拖过去」，两者就该一模一样。 */
	/* 光标在哪块屏上就换归属（和自由态一样）：不换的话迷你态永远拖不到邻屏
	 * —— 用户：「无法拖拽到另一个屏幕」。 */
	struct wlr_output *under = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	if (under != NULL && under != view->output) {
		vidow_mini_drag_to_output(server, view, under,
			server->cursor->x, server->cursor->y);
		vidow_arrange(server);
		return;
	}
	vidow_mini_anchor_from(view, work, view->float_geom.width,
		view->float_geom.height, x, y, true);
	if (view->mini == VIDOW_MINI_ICON) {
		/* 图标吸附：永远贴最近那条工作区边 —— 那条轴上钉死在边上，另一轴跟着
		 * 光标（所以还能沿边上下 / 左右挪）；出屏方向也跟着这条边（resolve 里
		 * 按 mini_edge 让 mini_icon_overflow 那点探出屏外）。用户：
		 * 「icon态需要吸附在屏幕边缘」。 */
		enum vidow_mini_edge e = vidow_mini_nearest_edge(server, view,
			view->float_geom.width, view->float_geom.height, x, y);
		view->mini_edge = e;
		switch (e) {
		case VIDOW_MINI_EDGE_LEFT:
			view->mini_ax = 0.0;
			break;
		case VIDOW_MINI_EDGE_RIGHT:
			view->mini_ax = 1.0;
			break;
		case VIDOW_MINI_EDGE_TOP:
			view->mini_ay = 0.0;
			break;
		case VIDOW_MINI_EDGE_BOTTOM:
			view->mini_ay = 1.0;
			break;
		case VIDOW_MINI_EDGE_NONE:
			break;
		}
	}
	vidow_mini_resolve_geom(view);
	vidow_arrange(server);
}

/* 指针按在窗口上。返回 true = 这个按下被小窗吃掉（调用方立刻 return，客户端收
 * 不到任何指针事件）。左键额外起「单击 / 拖拽」判定（见本节顶上那段）。 */
static bool mini_pointer_press(struct vidow_server *server,
		struct vidow_view *view, uint32_t button) {
	if (!mini_swallows_input(view)) {
		return false;
	}
	/* 非左键（中键 / 右键 / 侧键）：照吞，不做任何事 —— 「迷你 / 图标收不到
	 * 任何输入」是死规矩，不按按钮分。 */
	if (button != BTN_LEFT) {
		return true;
	}
	/* 小窗（S4）悬停工具条：按下点落在某个按钮上 → 直接做那个动作，不起拖拽。
	 * 工具条不是表面，命中靠矩形（vidow_mini_toolbar_hit）—— 这里和绘制用的是
	 * 同一份矩形（server->mini_toolbar_btn_box）。 */
	int btn = vidow_mini_toolbar_hit(server, view, server->cursor->x,
		server->cursor->y);
	if (btn >= 0) {
		vidow_mini_toolbar_press(server, view, btn);
		/* 形态可能变了（还原成列 / 关掉 / 换档）：工具条收掉，免得留在
		 * 一个已经不在小窗态的窗口上。 */
		vidow_mini_toolbar_hide(server);
		return true;
	}
	server->mini_grab_view = view;
	server->mini_grab_button = button;
	server->mini_grab_press_x = server->cursor->x;
	server->mini_grab_press_y = server->cursor->y;
	/* 按下点相对窗口左上角的偏移：拖拽时保持它，窗口才不会跳一下。 */
	server->mini_grab_dx = server->cursor->x - view->float_geom.x;
	server->mini_grab_dy = server->cursor->y - view->float_geom.y;
	server->mini_grab_dragging = false;
	return true;
}

/* 松手：判定单击还是拖拽。返回 true = 这次松手被吃掉（客户端收不到）。 */
static bool mini_pointer_release(struct vidow_server *server, uint32_t button) {
	struct vidow_view *view = server->mini_grab_view;
	if (view == NULL) {
		return false;
	}
	uint32_t grabbed = server->mini_grab_button;
	bool dragging = server->mini_grab_dragging;
	server->mini_grab_view = NULL;
	server->mini_grab_dragging = false;
	if (button != grabbed) {
		return true; /* 别的键的松手：照吞，不结束这次判定 */
	}
	if (!dragging) {
		/* 小窗（S5）甩出**逃生口**：位移没到 mini_drag_threshold_px 但松手速度
		 * 够快 —— 一次很快的小幅甩动（< 4px）本来会被判成「单击 = 放大」，
		 * 那和甩出直接冲突。速度优先：够快就算甩出，不是单击。 */
		double vx = 0.0, vy = 0.0;
		if (vidow_mini_fling_velocity(server, &vx, &vy) &&
				vidow_mini_fling(server, view, vx, vy)) {
			return true;
		}
		/* 单击 = 放大：图标 -> 迷你，迷你 -> 自由小窗（计划 T3 / T4）。
		 * 自由小窗可交互，所以放大之后把焦点给它（vidow_mini_set_state 里
		 * 对 MINI / ICON 会先交出焦点，FREE 不会 —— 这里是反向那一步）。 */
		enum vidow_mini_state next = view->mini == VIDOW_MINI_ICON ?
			VIDOW_MINI_MINI : VIDOW_MINI_FREE;
		vidow_mini_set_state(view, next);
		if (view->mini == VIDOW_MINI_FREE) {
			vidow_focus_view(view);
		}
	} else {
		/* 小窗（S4）：拖拽松手 = 落点判定（用户点名的那条）——
		 *   内边缘（外侧没有相邻输出）→ 收成图标；
		 *   跨屏边（外侧有相邻输出）→ 移到那块屏、保持形态。
		 * 和浮窗拖拽共用同一份（vidow_mini_drag_finish）。
		 *
		 * 小窗（S5）：**甩出去**的（松手速度 ≥ mini_fling_speed）优先 ——
		 * 它按速度方向取最近的那条边，不管光标停在哪。 */
		vidow_mini_drop_zone_hide(server);
		double vx = 0.0, vy = 0.0;
		bool have_v = vidow_mini_fling_velocity(server, &vx, &vy);
		wlr_log(WLR_DEBUG, "mini fling: 松手速度 %s（%.0f,%.0f px/s，采样 %d 点）",
			have_v ? "有" : "没有", have_v ? vx : 0.0, have_v ? vy : 0.0,
			server->mini_fling_n);
		if (have_v && vidow_mini_fling(server, view, vx, vy)) {
			return true;
		}
		vidow_mini_drag_finish(server, view);
	}
	return true;
}

/* 小窗（S2）分流：指针压在迷你 / 图标上 —— 命中测试照跑（要能点中），但客户端
 * 那一份事件不发。返回 true = 调用方立刻 return。 */
static bool mini_pointer_hover_swallow(struct vidow_server *server,
		struct vidow_view *hit) {
	if (!mini_swallows_input(hit)) {
		/* 小窗（S4）：光标落在**工具条自己**身上时工具条留着（它不在窗口的命中
		 * 范围里 —— 不是表面，vidow_view_at 命中不到它），而且这一份移动也不
		 * 转给客户端（工具条是合成器盖在桌面上的东西）。不这么写的话，指针
		 * 一离开窗口往按钮上走，工具条就没了 —— 永远点不中。
		 *
		 * 判的是**附近**（vidow_mini_toolbar_near，整块外扩 12px）而不是按钮矩形：
		 * 工具条跟窗口之间隔着 4px（摆在小窗上方 `g.y - h - 4`），指针穿那条缝的
		 * 一帧既不在窗口上、也不在按钮上 —— 只认按钮矩形的话工具条就在缝里被收掉
		 * 了（用户报的「整个工具条都不能交互」；注入测试一步跳到按钮上，所以没
		 * 暴露出来）。 */
		if (server->mini_toolbar_view != NULL &&
				vidow_mini_toolbar_near(server, server->cursor->x,
					server->cursor->y)) {
			return true;
		}
		/* 光标离开小窗（或落在别的东西上）→ 工具条收起来。只在小窗态之间
		 * 切换时重新摆（下面那句 show 会重算位置）。 */
		if (server->mini_toolbar_view != NULL &&
				server->mini_toolbar_view != hit) {
			vidow_mini_toolbar_hide(server);
		}
		return false;
	}
	mini_pointer_clear_client_focus(server);
	/* 小窗（S4）悬停工具条：迷你 / 图标态悬停就露出来（放大 / 收成图标 /
	 * 还原成列 / 关闭）。重复调用是幂等的，位置每次重算 —— 窗口被拖动 /
	 * 换屏之后工具条跟着走。 */
		/* 工具条**删掉了**（用户：「小窗工具栏去掉」）：唯一的 show 调用就在这儿，
		 * 去掉之后工具栏在任何配置下都不可能露出来。剩下的死代码（drag.c 的
		 * layout/glyph/show/hide/near、float.c 的 hit/press）下一步整段删。 */
	return true;
}

void vidow_pointer_motion(struct vidow_server *server, uint32_t time_msec) {
	/* 有人在录「光标不进画面」的流（metadata / embedded）：光标的位置就是那一帧
	 * 的内容，指针一动就得推一帧（否则录进去的光标停在上一帧 —— 静止的窗口上
	 * 移动指针时尤其明显）。放在最前面：拖着东西 / 摇晃那些分支提前 return 也
	 * 一样要推（拖拽时光标也在动）。 */
	vidow_screencast_cursor_moved(server);

	/* 小窗（S5）甩出：每一次指针移动都往采样环里推一格（松手时按最近 ~100ms
	 * 那一段算速度，见 float.c 的 vidow_mini_fling_velocity）。放在最前面 ——
	 * 下面拖着东西的分支会提前 return，那正是最需要采样的时候。 */
	vidow_mini_fling_sample(server, server->cursor->x, server->cursor->y);

	/* 光标隐藏（hide_when_typing / hide_after_inactive_ms）：指针一动就把光标
	 * 显示回来并重新计时。放在最前面 —— 拖着东西 / 摇晃那些分支提前 return 也
	 * 一样算「人动了」。 */
	cursor_wake(server);
	/* 输入唤醒关掉的屏（默认关，见 server.h 的 mouse_move_enables_dpms）。 */
	if (server->config.mouse_move_enables_dpms) {
		vidow_output_wake_disabled(server);
	}

	/* 会话锁：锁定期间指针只属于 lock surface —— 命中测试（vidow_view_at）、
	 * 指针约束、悬停聚焦、拖拽 / 概览 / MRU 一概不跑（方案 §2 环节 3）。
	 * 放在 DPMS 唤醒之后：屏关着的时候指针一动也得能把屏唤醒，否则没法输密码
	 * （方案 §4 风险 2 —— 安全性靠「唤醒后画的是黑 rect / lock surface」保证，
	 * 不靠不唤醒）。 */
	if (server->locked) {
		lock_pointer_route(server, time_msec, 0, false, false);
		return;
	}

	/* 指针约束（游戏锁鼠标 / 夹光标）：先把这一步的位移发出去（relative_pointer），
	 * 锁定的把光标钉回去、夹住的夹进围栏。必须在命中测试**之前** —— 位置定下来
	 * 之后，指针焦点 / enter / motion 才对得上（见 pointerconstraint.c）。 */
	vidow_pointer_constraint_motion(server, time_msec);

	/* 摇晃找光标：先看这一下位移算不算「在晃」（拖拽中也算 —— 拖到一半找不到
	 * 指针了一样要放大）。 */
	vidow_cursor_fx_motion(server);

	/* 拖着东西的时候指针事件全归 grab：平铺拖拽走 drag.c，浮动窗口走 float.c。
	 * 测试注入（pointer-motion 动作）也走这里，和真实鼠标同一条路。 */
	if (server->grab_mode != VIDOW_GRAB_NONE) {
		pointer_grab_update(server);
		return;
	}
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	if (output != NULL) {
		server->focused_output = output;
	}

	double sx = 0, sy = 0;
	struct wlr_surface *surface = NULL;
	struct vidow_view *hit =
		vidow_view_at(server, server->cursor->x, server->cursor->y, &surface,
			&sx, &sy);

	/* X11 窗口的命中坐标换算比较容易出错（缓冲区按 xwayland_scale 倍声明过），
	 * 留一行调试日志：局部坐标应该是「布局坐标 - 窗口原点」的 xwayland_scale 倍。 */
	/* overview 里窗口都缩过了，命中坐标对不上，别把移动事件转给客户端 */
	if (vidow_overview_active(server)) {
		return;
	}

	/* 切换器开着指针就归它管（**从按下 Alt 那一刻算起**，不等面板显形）：面板
	 * 显形之后移动 = 点亮底下那一项；还没显形的 open_delay 期光标底下还是真窗口，
	 * 事件一转给客户端就漏了（用户口径：模态从按下 Alt 那一刻算起）—— 而且那会儿
	 * 也不该有 hover 聚焦，不然「按住 Alt 的瞬间鼠标扫过谁就把焦点给了谁」。 */
	if (vidow_mru_active(server)) {
		if (vidow_mru_showing(server)) {
			vidow_mru_hover(server, server->cursor->x, server->cursor->y);
		}
		return;
	}

	/* 小窗（S2）形态拖拽中：指针全归它（客户端一个事件都不收，也不换焦点）。 */
	if (server->mini_grab_view != NULL) {
		/* 小窗（S4）：拖拽中工具条收起来（不然它跟着窗口乱飘、还会挡住
		 * 落点）；落点区照常随时算（拖到落点区松手 = 迷你小窗，和浮窗
		 * 那条一致）。 */
		vidow_mini_toolbar_hide(server);
		mini_grab_update(server);
		vidow_mini_drop_zone_update(server);
		mini_pointer_clear_client_focus(server);
		return;
	}

	/* 小窗（S5）悬停预览：指针一动就过一次状态机（延迟 / 离开 / 宽限）。
	 * 放在命中测试之后 —— 它要按「光标底下是不是图标」分流。 */
	vidow_mini_preview_motion(server, hit);

	/* 小窗（S2）：压在迷你 / 图标上 —— 客户端收不到 enter / motion，悬停聚焦也
	 * 跳过（focus_follows_mouse 里另有一道）。命中测试上面已经跑过了（必须能
	 * 命中它 —— 单击要能放大，这是和锁屏相反的地方），这里只吞转发那一份。 */
	if (mini_pointer_hover_swallow(server, hit)) {
		return;
	}

	/* 约束跟着指针焦点走：指针底下的表面 == 约束那个表面才生效，移开就解除。 */
	vidow_pointer_constraint_focus(server, surface);

	/* 焦点跟随光标（默认关）：压在哪个窗口上就聚焦哪个。放在命中测试之后、
	 * 指针焦点通知之前 —— 悬停聚焦只动键盘焦点，指针焦点还是按老样子发给
	 * 客户端。拖着东西的时候上面早就 return 了（拖拽中不换焦点，和 niri 的
	 * 「指针被抓着就不跟」一个意思）。 */
	focus_follows_mouse(server, hit);

	if (hit != NULL && hit->kind == VIDOW_VIEW_X11) {
		wlr_log(WLR_DEBUG, "x11: 指针 %.0f,%.0f -> %s 局部 %.0f,%.0f",
			server->cursor->x, server->cursor->y,
			vidow_view_title(hit) != NULL ? vidow_view_title(hit) : "(无题)",
			sx, sy);
	}

	if (surface != NULL) {
		wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
	} else {
		/* 主题默认箭头；摇晃放大期间只记状态、不碰真图（见 cursorfx.c）。 */
		vidow_cursor_fx_default_cursor(server);
		/* **有键按着的时候不许清焦点**：那是 Wayland 的隐式抓取（implicit
		 * grab）—— 按下的那一刻起，指针事件就该一直送给按下时那个表面，直到
		 * 松手为止，哪怕光标已经跑到窗口外面。
		 *
		 * 用户报的「窗口内拖拽选文本，鼠标移到窗口外选择就停住；移回窗口再松手
		 * 选择还在跟着鼠标走」就是这里断的。实测事件序列（客户端收到的）：
		 *   PRESS (140,73) / MOTION (290,73)        窗口内，正常
		 *   MOTION (490,653) / MOTION (550,653)     光标在窗口外，还在送 —— 对
		 *   MOTION (590,73) / LEAVE (590,73)        移回来时这里清了焦点
		 *   松手 → 客户端 **一条 RELEASE 都没收到**
		 * 清了焦点之后 leave 已经发出，客户端认为指针早就走了；而 release 又被
		 * 合成器自己吞掉（notify_button 只送给 focused_surface，那时是 NULL），
		 * 于是客户端一直以为按钮还按着 —— 「释放鼠标选择还在跟随」正是这个。
		 *
		 * server->seat->pointer_state.button_count 是 wlroots 自己维护的按下
		 * 计数（notify_button 里加减），拿它判「现在是不是抓着」最准，不用自己
		 * 再记一份状态。 */
		if (server->seat->pointer_state.button_count == 0) {
			wlr_seat_pointer_notify_clear_focus(server->seat);
		} else {
			/* 仍在抓取：motion 要送给抓取者，坐标得是**相对那个表面**的。
			 *
			 * sx/sy 这里是 0（vidow_view_at 没算 —— 光标根本不在任何窗口上），
			 * 直接送 0 会让抓取中的客户端以为指针一动不动：文本选择、拖动这类
			 * 操作正是靠 motion 的位移推进的，停在 0 就等于「选择卡住不动」。
			 *
			 * 换算：场景是全局坐标，减掉抓取窗口的场景节点原点就是它的局部
			 * 坐标（和 wlr_scene_node_at 一个算法，只是不做框内裁剪 —— 光标
			 * 现在就在框外，这正是要允许的）。拿不到节点就退回 0,0：至少
			 * release 能送到，不会留下「一直按着」的坏状态。 */
			double gx = 0, gy = 0;
			struct wlr_surface *grabbed =
				server->seat->pointer_state.focused_surface;
			struct vidow_view *gv = vidow_view_from_surface(server, grabbed);
			if (gv != NULL && gv->scene_tree != NULL) {
				int nx = 0, ny = 0;
				if (wlr_scene_node_coords(&gv->scene_tree->node, &nx, &ny)) {
					gx = server->cursor->x - (double)nx;
					gy = server->cursor->y - (double)ny;
				}
			}
			wlr_seat_pointer_notify_motion(server->seat, time_msec, gx, gy);
		}
	}
}

/* 键盘换焦点之后把光标挪到新窗口上（niri 的 input { warp-mouse-to-focus }）。
 * 只在**键盘 / IPC 动作**那条路调（见 keybind.c）—— 鼠标悬停聚焦不调：光标
 * 本来就在窗口里，niri 也是这么分的。
 *
 * 三种落点照 niri 的 mode 同名同义：
 *   留空              X / Y 分别判断：哪个轴在窗口外就挪哪个轴，另一个轴不动
 *   center-xy         光标整个在窗口外才挪，挪就挪到窗口正中
 *   center-xy-always  永远挪到窗口正中
 *
 * wlr_cursor_warp 只挪位置、不发事件（wlroots 的 cursor_warp_unchecked），所以
 * 和注入那条路一样显式再走一遍 vidow_pointer_motion（指针焦点 / enter / motion
 * 全在里面），最后自己补一个 wl_pointer.frame —— 为什么要补见
 * vidow_inject_pointer_move 那段注释。 */
void vidow_warp_cursor_to_focus(struct vidow_server *server) {
	if (!server->config.warp_mouse_to_focus || server->focused == NULL ||
			server->cursor == NULL) {
		return;
	}
	struct wlr_box g = server->focused->geom;
	if (g.width < 1 || g.height < 1) {
		return;
	}
	const char *mode = server->config.warp_mouse_to_focus_mode;
	double cx = g.x + g.width / 2.0;
	double cy = g.y + g.height / 2.0;
	double x = server->cursor->x;
	double y = server->cursor->y;
	bool inside_x = x >= g.x && x < g.x + g.width;
	bool inside_y = y >= g.y && y < g.y + g.height;
	if (mode[0] == '\0') {
		if (!inside_x) {
			x = cx;
		}
		if (!inside_y) {
			y = cy;
		}
	} else if (strcmp(mode, "center-xy") == 0) {
		if (!inside_x || !inside_y) {
			x = cx;
			y = cy;
		}
	} else { /* center-xy-always */
		x = cx;
		y = cy;
	}
	if (x == server->cursor->x && y == server->cursor->y) {
		return;
	}
	if (!wlr_cursor_warp(server->cursor, NULL, x, y)) {
		return;
	}
	/* 挪光标这一下自己会经过 vidow_pointer_motion，而那里会跑焦点跟随光标 ——
	 * 光标底下要是压着一个浮窗（焦点窗口被它盖住一角），那一下就会把焦点从
	 * 刚聚焦的窗口抢到浮窗上。这里明确：这一趟是「带光标过去」，不是「用户
	 * 把鼠标挪过去」，别让悬停聚焦插手。 */
	warp_in_progress = true;
	vidow_pointer_motion(server, (uint32_t)(vidow_now_ns() / 1000000LL));
	warp_in_progress = false;
	wlr_seat_pointer_notify_frame(server->seat);
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
	struct vidow_server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;

	wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x,
		event->delta_y);

	vidow_pointer_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener,
		void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;

	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x,
		event->y);

	vidow_pointer_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	struct vidow_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;

	vidow_pointer_button(server, event->button,
		event->state == WL_POINTER_BUTTON_STATE_PRESSED, event->time_msec,
		false);
}

/* 合成器自己的指针绑定（Mod+左键 / Mod+滚轮）要看的修饰键。
 *
 * **不能读 seat 上那个键盘设备**：输入法（fcitx5 一启动就会抢）抢走键盘之后，
 * seat 上的键盘换成它的虚拟键盘，物理键盘的修饰键只经
 * wlr_seat_keyboard_notify_modifiers() 发给客户端 —— 虚拟键盘自己的 xkb_state
 * 不会跟着变，读它就永远拿到「没按 Mod」。现象就是「按住 Mod 滚一次之后就不
 * 认了，得松开重按」（用户报的，a3d4dc9 修过一次，但那条只补了通知、没改这里
 * 的取值）。
 *
 * 所以直接找一把**物理键盘**（virtual == NULL、也不是我们自己的注入键盘）读
 * 它的 xkb 状态；没有物理键盘
 * （纯 headless / 只有注入）时退回 seat 上那把，再没有就是 0。 */
static uint32_t pointer_modifiers(struct vidow_server *server) {
	uint32_t mods = 0;
	struct vidow_keyboard *kb;
	wl_list_for_each(kb, &server->keyboards, link) {
		if (kb->virtual == NULL && !kb->inject) {
			mods |= wlr_keyboard_get_modifiers(kb->wlr_keyboard);
		}
	}
	if (mods != 0) {
		return mods;
	}
	struct wlr_keyboard *seat_kb = wlr_seat_get_keyboard(server->seat);
	return seat_kb != NULL ? wlr_keyboard_get_modifiers(seat_kb) : 0;
}

/* 指针按下 / 松开。测试注入（VIDOW_TEST_ACTION 里的 pointer-button）也走这里，
 * 和真实鼠标完全同一条路。 */
void vidow_pointer_button(struct vidow_server *server, uint32_t button,
		bool pressed, uint32_t time_msec, bool force_mod) {
	/* 会话锁：锁定期间指针按键只给 lock surface —— 不进拖拽 grab、不过绑定表、
	 * 不给被藏起来的窗口（方案 §2 环节 3）。 */
	if (server->locked) {
		lock_pointer_route(server, time_msec, button, pressed, true);
		return;
	}
	/* 小窗（S5）悬停预览：它是**只读**的（不抢焦点 / 不进 MRU / 不参与命中），
	 * 但**吞点击** —— 指针底下有一块「像窗口的东西」，点下去却激活了背后的
	 * 窗口是明显的错。按下 / 松手成对吞掉（客户端两边都没见过，状态对称）。
	 * 放在最前面：预览可能压在别的窗口上，压着的时候不该把事件给背后那个。 */
	if (server->mini_preview_view != NULL) {
		bool on_preview = vidow_mini_preview_hit(server, server->cursor->x,
			server->cursor->y);
		if (pressed && on_preview) {
			server->mini_preview_pressed = true;
			wlr_log(WLR_INFO, "mini preview: 吞掉点击（%.0f,%.0f，预览 "
				"%dx%d @ %d,%d）—— 只读，不激活背后的窗口",
				server->cursor->x, server->cursor->y,
				server->mini_preview_box.width,
				server->mini_preview_box.height,
				server->mini_preview_box.x, server->mini_preview_box.y);
			return;
		}
		if (!pressed && server->mini_preview_pressed) {
			server->mini_preview_pressed = false;
			return;
		}
	}
	/* 小窗（S4）悬停工具条：它不是表面（合成器自绘的矩形），命中得单独判一次
	 * —— 按在按钮上就整个吃掉（不转给客户端、也不起拖拽），松手那次也成对吃掉
	 * （客户端两边都没见过，状态才是对称的）。
	 * 放在形态拖拽那条之前：按钮可能压在窗口上，压在按钮上时不该起拖拽。 */
	if (server->mini_toolbar_view != NULL && pressed) {
		int btn = vidow_mini_toolbar_hit(server,
			server->mini_toolbar_view, server->cursor->x,
			server->cursor->y);
		if (btn >= 0) {
			struct vidow_view *tv = server->mini_toolbar_view;
			vidow_mini_toolbar_press(server, tv, btn);
			/* 形态可能变了（还原成列 / 关掉 / 换档）：工具条收掉。 */
			vidow_mini_toolbar_hide(server);
			server->mini_toolbar_pressed = true;
			return;
		}
	}
	/* 松手那一半**无条件**吃、并且**无条件清标志**（S3 修）：以前这一支写在
	 * `if (server->mini_toolbar_view != NULL)` 里面，而按下那条路紧接着
	 * `vidow_mini_toolbar_hide()` 就把 view 清成 NULL —— 标志就此留在 true 没人
	 * 清。后果是「悬停着工具条的迷你窗口」上的**下一对**按下 / 松开被这对陈旧
	 * 标志吞掉：形态不变（按一下没反应），而且那次按下起的 mini_grab 再没人收尾
	 * —— 紧接着一次普通 pointer-motion 就能把窗口**在没按键的情况下**拖走
	 * （实测 `place w1 -> 464,506` 就是这么来的）。
	 *
	 * 标志被吞过之后 grab 还挂着时（旧状态 / 别的路径丢过松手）接着往下走，让
	 * 小窗那条松手把它一起收掉。 */
	if (!pressed && server->mini_toolbar_pressed) {
		server->mini_toolbar_pressed = false;
		if (server->mini_grab_view == NULL) {
			return;
		}
	}
	/* 小窗（S2）形态拖拽：按下那次已经被吃掉（见下面的 mini_pointer_press），
	 * 这一对松手也整个吃掉 —— 客户端从头到尾没见过这个按键，状态是对称的。 */
	if (server->mini_grab_view != NULL) {
		if (!pressed) {
			mini_pointer_release(server, button);
		}
		return;
	}
	if (server->grab_mode != VIDOW_GRAB_NONE) {
		/* the grab owns the pointer: swallow everything until release */
		if (!pressed) {
			/* 客户端自己发起的那次抓取（CSD 标题栏 / X11 的
			 * _NET_WM_MOVERESIZE）：按下那次是转给客户端的，松手得还给它。
			 * 在读之前记下来 —— end_grab 会把状态清掉。 */
			bool client_saw_press = server->grab_client_press;
			if (server->grab_mode == VIDOW_GRAB_PAN) {
				vidow_pan_end(server);
			} else
			if (server->grab_mode == VIDOW_GRAB_TILE) {
				vidow_tile_grab_end(server);
			} else if (server->grab_mode == VIDOW_GRAB_RESIZE_TILE) {
				vidow_tile_resize_end(server);
			} else {
				vidow_float_end_grab(server);
			}
			/* 松手之后指针重新交还给客户端：拖拽期间每一次移动都被 grab 吃掉了，
			 * 客户端手里那份指针位置还是**起拖之前**的。窗口跟着光标挪过之后，
			 * 这个旧坐标可能已经落在窗口外（拖到屏幕边被夹住时必然如此），于是
			 * 「拖完第一下点击」被客户端当成点在别处 —— 用户报的「拖拽之后点不着，
			 * 鼠标移出去再移回来就好了」（移出去/移回来会触发 enter，位置才刷新）。
			 * 这里补一次和真实移动同一条路的 motion，坐标当场对上。 */
			vidow_pointer_motion(server, time_msec);
			/* 松手本身也要转给客户端 —— 只补给位置是不够的：客户端手里那次
			 * 按下永远没等到对应的松开，它会一直以为左键还按着（GTK 的
			 * in_move_drag、Chromium 的鼠标键状态都靠松开事件复位）。症状就是
			 * 「用标题栏拖一次之后窗口就不听使唤了：鼠标一动它还在拖，第一下
			 * 点击被当成按下」。放在 motion 之后发：motion 会把焦点和局部坐标
			 * 刷成当前值，松开事件才落在对的位置上。 */
			if (client_saw_press) {
				wlr_seat_pointer_notify_button(server->seat, time_msec, button,
					WL_POINTER_BUTTON_STATE_RELEASED);
			}
			/* 这一对（位置 + 松开）是合成器自己造的，不是设备帧里来的：得自己
			 * 收一个 frame，客户端才会当场处理。wl_pointer v5 起事件按帧分组，
			 * GTK/GDK 把「没等到 frame」的事件先扣在自己缓冲里 —— 真鼠标那条
			 * 路上 frame 由 libinput 的帧事件补上（server_cursor_frame），注入 /
			 * 合成这条路没有设备帧，漏掉这一句客户端就要等到**下一个**指针事件
			 * 才反应过来（实测：按下之后什么都不做，客户端 5 秒都没收到）。 */
			wlr_seat_pointer_notify_frame(server->seat);
		}
		return;
	}

	if (vidow_overview_active(server) || server->overview.swallow_release) {
		/* 点一下 = 选中那个窗口（然后缩回去）；点空白 = 取消。
		 * 点选那次按下已经被吃掉了，它对应的松手也一起吃掉。 */
		if (pressed && button == BTN_LEFT &&
				vidow_overview_active(server)) {
			vidow_overview_click(server, server->cursor->x, server->cursor->y);
		}
		if (!pressed) {
			server->overview.swallow_release = false;
		}
		return;
	}

	/* Alt+Tab 切换器：点一下卡片 = 直接切到那个窗口（不用等松修饰键），点空白 =
	 * 什么都不做。移动归它管从**按下 Alt 那一刻**就算（见上面 motion 那一段），
	 * 指针的按下 / 松开也一样：open_delay 期面板还没摆出来，这一下要是漏给客户端
	 * 就是「按住 Alt 还没看见面板，鼠标已经点进窗口里了」。
	 *
	 * 吃掉的按下要记下来，好把它对应的松手也吃掉（vidow_mru_click 也置这个标志；
	 * 不这么做客户端会收到一个**没有按下配对的松开**，和第 1370 行那段长注释说的
	 * 是同一回事）。 */
	if (vidow_mru_active(server)) {
		if (!pressed) {
			server->mru.swallow_release = false;
			return;
		}
		server->mru.swallow_release = true;
		if (vidow_mru_showing(server) && button == BTN_LEFT) {
			vidow_mru_click(server, server->cursor->x, server->cursor->y);
		}
		return;
	}

	/* 「面板已经关了、只剩 swallow_release 还挂着」时的按下不拦（照常走下面的
	 * 绑定表）—— 不然万一哪次松手丢了，所有按键就永远被吞在这儿。 */
	if (server->mru.swallow_release) {
		if (!pressed) {
			server->mru.swallow_release = false;
			return;
		}
	}

	/* 小窗（S2）：按在迷你 / 图标上 —— 整个事件归小窗（客户端收不到任何指针
	 * 事件），所以必须在**绑定表之前**分流：不然 Mod+左键的 interactive-move
	 * 会顺着 keybind 的 view_under_cursor 抓到它，走浮窗那套 grab（那条路松手
	 * 会把窗口重新认领回某块工作区，和小窗「跟屏不跟工作区」冲突）。
	 * 命中测试在这儿现做一次（按下才做）：命中是小窗才有意义，正常路径那次
	 * 照旧跑（它还要算局部坐标给客户端）。 */
	if (pressed) {
		double msx = 0, msy = 0;
		struct wlr_surface *msurface = NULL;
		struct vidow_view *mhit = vidow_view_at(server, server->cursor->x,
			server->cursor->y, &msurface, &msx, &msy);
		if (mini_pointer_press(server, mhit, button)) {
			return;
		}
	}

	/* 绑定表（`vidow.bind("Mod+mouse-left", …)`）：鼠标按键和键盘键同一张表、
	 * 同一套动作。按下 / 松开都要问 —— 「松开才触发」的绑定靠松开那一下收尾。 */
	uint32_t mods = pointer_modifiers(server);
	if (force_mod) {
		/* 测试注入「假装按着 Mod」（见 server.h）：测试环境里没有真键盘，
		 * 而拖动这类手势要求按着 Mod —— 绑定的匹配照样按真修饰键走。 */
		mods |= MOD_KEY;
	}
	if (vidow_keybind_button(server, button, pressed, mods,
			vidow_shortcuts_inhibited(server))) {
		return;
	}

	if (!pressed) {
		wlr_seat_pointer_notify_button(server->seat, time_msec, button,
			WL_POINTER_BUTTON_STATE_RELEASED);
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct vidow_view *view = vidow_view_at(server, server->cursor->x,
		server->cursor->y, &surface, &sx, &sy);

	/* 「这一下点到谁头上了」是排查点击 / 焦点问题的第一现场：命中的窗口、
	 * 命中坐标、以及键盘焦点给谁。 */
	struct wlr_surface *pfocus = server->seat->pointer_state.focused_surface;
	struct vidow_view *pfocus_view = vidow_view_from_surface(server, pfocus);
	wlr_log(WLR_DEBUG, "点击 button %u 在 %.0f,%.0f：命中 %s（局部 %.0f,%.0f），"
		"指针焦点在 %s", button, server->cursor->x, server->cursor->y,
		view != NULL ? vidow_view_title(view) : "(没有窗口)", sx, sy,
		pfocus == NULL ? "(无)" :
			(pfocus_view != NULL ? vidow_view_title(pfocus_view) :
				"(别的表面)"));

	/* A layer surface asking for keyboard focus (ON_DEMAND launcher) wins. */
	if (vidow_layer_focus_surface(server, surface)) {
		wlr_seat_pointer_notify_button(server->seat, time_msec, button,
			WL_POINTER_BUTTON_STATE_PRESSED);
		return;
	}

	/* 指针手势（拖窗口 / 改列宽）现在是**绑定 + 动作**，见 keybind.c 的
	 * `interactive-move` / `interactive-resize` 和上面那次 vidow_keybind_button()：
	 * 默认配置里是 `Mod+mouse-left` / `Mod+mouse-right`。没绑就没有 —— 和键位
	 * 同一条规矩：合成器一个都不内置。 */

	wlr_seat_pointer_notify_button(server->seat, time_msec, button,
		WL_POINTER_BUTTON_STATE_PRESSED);

	/* 点在空区域（没有窗口、也没有 layer surface）：键盘焦点跟着光标换到
	 * 这块屏的活动工作区上。只改 focused_output 不够 —— server->focused
	 * 还停在上一块屏的窗口上，之后的键位（close-window / focus-column …）
	 * 会继续作用在那边，用户看到的就是「在 4K 上随便点，动的却是左边」。
	 * niri 的 focus_output 在它那套每屏一份焦点里等价于这一句。 */
	struct wlr_output *click_output = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	if (view == NULL && surface == NULL && click_output != NULL) {
		vidow_workspace_focus_output(server, click_output);
	} else if (view != NULL && view != server->focused) {
		vidow_focus_view(view);
	} else if (view != NULL) {
		/* 已经拿着键盘的那个也要抬一下：浮窗之间靠点击换层级，而它可能正被
		 * 后开（或者刚被点过）的另一个浮窗压着 —— 这时候焦点没变，不抬就等于
		 * 「点了没反应」。平铺窗口不用管：它们本来就该在浮窗下面（见
		 * vidow_restack）。 */
		vidow_restack(server, view);
	}
}

/* ------------------------------------------------------------ 触控板手势
 *
 * libinput 的 swipe / pinch / hold 原样转给 gesture.c（那里管配置表和「拖视口」）。
 * 手势事件也是指针事件：没有触控板（headless / 只有鼠标）时一个都不会来。 */
static void server_pointer_swipe_begin(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, pointer_swipe_begin);
	struct wlr_pointer_swipe_begin_event *event = data;
	vidow_gesture_swipe_begin(server, (int)event->fingers);
}

static void server_pointer_swipe_update(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, pointer_swipe_update);
	struct wlr_pointer_swipe_update_event *event = data;
	vidow_gesture_swipe_update(server, (int)event->fingers, event->dx,
		event->dy);
}

static void server_pointer_swipe_end(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, pointer_swipe_end);
	struct wlr_pointer_swipe_end_event *event = data;
	vidow_gesture_swipe_end(server, event->cancelled);
}

static void server_pointer_pinch_begin(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, pointer_pinch_begin);
	struct wlr_pointer_pinch_begin_event *event = data;
	vidow_gesture_pinch_begin(server, (int)event->fingers);
}

static void server_pointer_pinch_update(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, pointer_pinch_update);
	struct wlr_pointer_pinch_update_event *event = data;
	vidow_gesture_pinch_update(server, (int)event->fingers, event->scale);
}

static void server_pointer_pinch_end(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, pointer_pinch_end);
	struct wlr_pointer_pinch_end_event *event = data;
	vidow_gesture_pinch_end(server, event->cancelled);
}

static void server_pointer_hold_begin(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, pointer_hold_begin);
	struct wlr_pointer_hold_begin_event *event = data;
	vidow_gesture_hold_begin(server, (int)event->fingers);
}

static void server_pointer_hold_end(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, pointer_hold_end);
	struct wlr_pointer_hold_end_event *event = data;
	vidow_gesture_hold_end(server, event->cancelled);
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct vidow_server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;

	vidow_pointer_axis(server, event->orientation, event->delta,
		event->delta_discrete, event->source, event->time_msec);
}

/* 滚轮。真设备和 IPC 注入走同一条路：overview 平移 / Mod+滚轮换列 / 转发给
 * 客户端。注入那边（ipc.c）调完这个再补一个 pointer frame。 */
void vidow_pointer_axis(struct vidow_server *server,
		enum wl_pointer_axis orientation, double delta, int32_t delta_discrete,
		enum wl_pointer_axis_source source, uint32_t time_msec) {
	/* 会话锁：锁定期间滚轮也只给 lock surface（概览平移 / Mod+滚轮换列都不跑，
	 * 方案 §2 环节 3）。滚轮倍率（scroll_factor）是 vidow 自己的偏好，锁屏界面
	 * 拿原始值 —— 它不该受桌面配置影响。 */
	if (server->locked) {
		double sx = 0, sy = 0;
		struct wlr_surface *surface =
			vidow_sessionlock_pointer_surface(server, &sx, &sy);
		if (surface != NULL) {
			wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
			wlr_seat_pointer_notify_axis(server->seat, time_msec, orientation,
				delta, delta_discrete, source,
				WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
		}
		return;
	}
	/* 滚动倍率（niri 的 scroll-factor，见 configs/19-input.lua）。libinput 没
	 * 这个接口，是 vidow 自己在滚轮事件上乘的。1.0（默认）= 不缩放，跟以前
	 * 一模一样。乘在**入口**而不是只乘发给客户端那一份：真设备和 IPC 注入本来
	 * 就走这一条路，倍率在这儿才一处管全。 */
	double factor = server->config.scroll_factor;
	if (factor != 1.0) {
		delta *= factor;
		if (delta_discrete != 0) {
			/* 离散值（老鼠标一格一格那种）跟着缩，不然只吃离散值的客户端
			 * 感觉不到倍率；缩到 0 那一格就白滚了，所以至少留一格（方向
			 * 不变）。 */
			double d = delta_discrete * factor;
			int32_t r = (int32_t)(d >= 0 ? floor(d + 0.5) : ceil(d - 0.5));
			if (r == 0) {
				r = delta_discrete > 0 ? 1 : -1;
			}
			delta_discrete = r;
		}
	}

	/* 有些设备只发离散值（老鼠标），所以两个都看 */
	double amount = delta != 0 ? delta : delta_discrete;

	/* overview 里滚轮滚的是**工作区列表**（上下预览那一列），**一次一格 = 一块
	 * 工作区**（格距 = 行距 `row_h + OV_ROW_GAP`，在 overview.c 里算并吸附整数倍；
	 * 用户：「overview滚轮操作应该是巡航一次滚到一个工作区」）。往下滚（amount > 0）
	 * 就往下看后面的工作区。窗口自己的横向预览不归它管 —— 那是「移动窗口焦点」那些
	 * 键的事（跟着工作区自己的视口走，见 overview.c）。
	 *
	 * **只认竖直轴**：滚轮左右倾（横向轴）以前也滚同一个列表，那是错的 —— 概览里
	 * 没有横向可滚的东西（一行的横向预览跟工作区自己的视口走），吃掉不动作。 */
	if (vidow_overview_active(server)) {
		/* 先问绑定表：按着 Mod 滚滚轮 = 「移动窗口焦点」（用户自己的
		 * 14-keys.lua：Mod+wheel-up/down = focus-column left/right）。以前这里
		 * 把整段滚轮直接交给列表滚动、从不问绑定表，于是 overview 里
		 * Mod+滚轮什么都不做（用户报的「overview状态下还是无法通过mod+滚轮
		 * 移动窗口焦点」）。没命中绑定才轮到列表滚动。 */
		uint32_t mods0 = pointer_modifiers(server);
		if (vidow_keybind_wheel(server, orientation, amount, mods0,
				vidow_shortcuts_inhibited(server))) {
			return;
		}
		if (orientation == WL_POINTER_AXIS_VERTICAL_SCROLL && amount != 0) {
			vidow_overview_scroll(server, amount > 0 ? 1 : -1);
		}
		return;
	}

	/* Alt+Tab 切换器开着（**从按下 Alt 那一刻算起**，不等面板显形）：滚轮归它管
	 * —— 竖直轴 = 选中项往后 / 往前一格（**到边停住，不循环**，见 mru.c），横向轴
	 * 吃掉不动作。面板开着时输入是模态的，一格都不透给别处。
	 *
	 * 必须排在**小窗档位**（下面那段）之前：用户撞的就是这个 —— 按住 Alt 滚滚轮，
	 * 光标压在某个迷你 / 图标小窗上时先被「档位状态机」抢走；再往下走还会命中绑定
	 * 表（默认配置里 `Alt+wheel-up/down` = 换工作区，见 configs/14-keys.lua），
	 * 结果「按着 Alt 想切窗口，工作区真的跳了」。 */
	if (vidow_mru_active(server)) {
		vidow_mru_wheel(server, orientation, amount);
		return;
	}

	/* 小窗（S2/S4）：迷你 / 图标收不到滚轮（客户端一个事件都不收）。命中测试
	 * 在这儿现做一次 —— 滚轮这条路上没有别处算过命中。
	 * S4 的档位入口：在小窗上滚轮 = 形态档位，一次一格
	 *   往下滚（amount > 0）= 降一档：FREE → MINI → ICON（到底了就停）；
	 *   往上滚 = 升一档：ICON → MINI → FREE（已经在最上面那档就停）。
	 * FREE 的上滚**照旧转发给客户端**（它是最上面那一档，没有「升」的目标，
	 * 小窗要能滚内容）；其余情形一律吞掉，不转发。 */
	{
		double wsx = 0, wsy = 0;
		struct wlr_surface *wsurface = NULL;
		struct vidow_view *whit = vidow_view_at(server, server->cursor->x,
			server->cursor->y, &wsurface, &wsx, &wsy);
		if (whit != NULL && whit->mini != VIDOW_MINI_NONE &&
				amount != 0) {
			if (whit->mini == VIDOW_MINI_FREE) {
				if (amount > 0) {
					vidow_mini_set_state(whit, VIDOW_MINI_MINI);
					return;
				}
				/* 上滚：已经在最上面那档，转发给客户端滚内容。 */
			} else {
				enum vidow_mini_state next = whit->mini;
				if (amount > 0) {
					next = whit->mini == VIDOW_MINI_MINI ?
						VIDOW_MINI_ICON : VIDOW_MINI_ICON;
				} else {
					next = whit->mini == VIDOW_MINI_ICON ?
						VIDOW_MINI_MINI : VIDOW_MINI_FREE;
				}
				if (next != whit->mini) {
					vidow_mini_set_state(whit, next);
					if (whit->mini == VIDOW_MINI_FREE) {
						vidow_focus_view(whit);
					}
					wlr_log(WLR_INFO, "mini: 滚轮 %s → %s",
						amount > 0 ? "降一档" : "升一档",
						vidow_mini_state_name(whit->mini));
				}
				return;
			}
		}
		if (mini_swallows_input(whit)) {
			return;
		}
	}

	/* 绑定表（`vidow.bind("Mod+wheel-up", …)`）：一个方向一条，和键盘同一套动作。
	 * 修饰键走 pointer_modifiers()（原因见它上面那段：输入法抢键盘后 seat 上
	 * 那把是虚拟键盘，修饰键不在它身上）。 */
	uint32_t mods = pointer_modifiers(server);
	if (vidow_keybind_wheel(server, orientation, amount, mods,
			vidow_shortcuts_inhibited(server))) {
		return;
	}

	/* 滚轮手势也走绑定（默认配置里 `Mod+wheel-up/down` → 换列）。没绑的滚轮
	 * 照旧转发给客户端，应用里的滚动不受影响。 */
	wlr_seat_pointer_notify_axis(server->seat, time_msec, orientation, delta,
		delta_discrete, source, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_server *server = wl_container_of(listener, server, cursor_frame);

	wlr_seat_pointer_notify_frame(server->seat);
}

/* ------------------------------------------------------------------ seat */

/* wp_cursor_shape_manager_v1：GTK / Qt / layer 客户端用「形状名」换光标
 * （文本 I 字、手型、缩放箭头），不再自己传 surface。没有这个全局时它们只能
 * 停在默认箭头 —— 用户报的「光标形状在 layer 上不会变化」就是这条。 */
static void cursor_shape_request(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, cursor_shape_request);
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	/* 和 request_set_cursor 一样：只认当前指针焦点那个 seat client，
	 * 背景客户端不能在指针不在它上面时改全局光标。 */
	struct wlr_seat_client *focused =
		server->seat->pointer_state.focused_client;
	if (focused != event->seat_client) {
		return;
	}
	if (event->device_type !=
			WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER) {
		return; /* 数位笔形状暂不接管：vidow 光标平面只跟鼠标 */
	}
	const char *name = wlr_cursor_shape_v1_name(event->shape);
	if (name == NULL) {
		return;
	}
	wlr_log(WLR_DEBUG, "cursor-shape: %s", name);
	vidow_cursor_fx_set_cursor_name(server, name);
}

void vidow_cursor_shape_init(struct vidow_server *server) {
	server->cursor_shape_mgr =
		wlr_cursor_shape_manager_v1_create(server->display, 1);
	if (server->cursor_shape_mgr == NULL) {
		wlr_log(WLR_INFO,
			"cursor-shape: 全局建不出来（客户端只能用 set_cursor surface）");
		return;
	}
	server->cursor_shape_request.notify = cursor_shape_request;
	wl_signal_add(&server->cursor_shape_mgr->events.request_set_shape,
		&server->cursor_shape_request);
}

static void server_request_cursor(struct wl_listener *listener, void *data) {
	struct vidow_server *server = wl_container_of(listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;

	struct wlr_seat_client *focused = server->seat->pointer_state.focused_client;
	if (focused != event->seat_client) {
		return;
	}
	/* 先告诉摇晃放大「光标图要换了」，再设上去，最后让它把新图重新放大一遍
	 * —— 放大期间客户端换光标（焦点切到别的窗口就会）不再把放大缩回去。 */
	vidow_cursor_fx_set_surface(server, event->surface, event->hotspot_x,
		event->hotspot_y);
	wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x,
		event->hotspot_y);
	vidow_cursor_fx_cursor_changed(server);
}

static void server_request_set_selection(struct wl_listener *listener,
		void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;

	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void server_request_set_primary_selection(struct wl_listener *listener,
		void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;

	wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

/* --------------------------------------------------------------- 拖拽 */

static void server_drag_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_server *server =
		wl_container_of(listener, server, drag_destroy);
	if (server->drag_icon != NULL) {
		wlr_scene_node_destroy(&server->drag_icon->node);
		server->drag_icon = NULL;
	}
	wl_list_remove(&server->drag_destroy.link);
	wl_list_remove(&server->drag_motion.link);
	wl_list_init(&server->drag_destroy.link);
	wl_list_init(&server->drag_motion.link);
	server->drag = NULL;
}

static void server_drag_motion(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_server *server =
		wl_container_of(listener, server, drag_motion);
	if (server->drag_icon == NULL) {
		return;
	}
	/* 图标跟着光标走（和别的合成器一样，图标左上角就在光标位置）。 */
	wlr_scene_node_set_position(&server->drag_icon->node,
		(int)lround(server->cursor->x), (int)lround(server->cursor->y));
}

static void server_start_drag(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, start_drag);
	struct wlr_drag *drag = data;
	server->drag = drag;
	if (drag->icon != NULL) {
		server->drag_icon = wlr_scene_drag_icon_create(server->layer_top,
			drag->icon);
		if (server->drag_icon != NULL) {
			wlr_scene_node_set_position(&server->drag_icon->node,
				(int)lround(server->cursor->x),
				(int)lround(server->cursor->y));
		}
	}
	server->drag_destroy.notify = server_drag_destroy;
	wl_signal_add(&drag->events.destroy, &server->drag_destroy);
	server->drag_motion.notify = server_drag_motion;
	wl_signal_add(&drag->events.motion, &server->drag_motion);
}

static void server_request_start_drag(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, request_start_drag);
	struct wlr_seat_request_start_drag_event *event = data;
	/* 指针拖拽：起隐式键盘 + 指针 grab，后面的 motion / drop 都由 wlroots
	 * 的 data_device 实现转发给目标客户端。 */
	wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
}

/* wlrctl and other test tools drive the pointer through this protocol. */
static void server_new_virtual_pointer(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, new_virtual_pointer);
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	struct wlr_virtual_pointer_v1 *virtual_pointer = event->new_pointer;

	wlr_log(WLR_INFO, "virtual pointer attached");
	input_device_note(server, &virtual_pointer->pointer.base);
	wlr_cursor_attach_input_device(server->cursor,
		&virtual_pointer->pointer.base);
	/* 虚拟指针也是指针：座位得把 POINTER 报出来，否则客户端收不到指针事件
	 * （headless 会话里这是唯一的指针来源）。 */
	seat_update_capabilities(server);
}

bool vidow_input_init(struct vidow_server *server) {
	wl_list_init(&server->input_devices);
	/* 配置改了要重套 libinput 设置（设置存在设备上，重载 / set-config 不会自己
	 * 跟过去）。挂在配置那条路上而不是某个调用点：config.lua / VIDOW_* / IPC
	 * set-config 三条写路径都从 config.c 的 apply() 过，一处盖全。
	 * 注册放在 wl_list_init 之后 —— 在那之前 server->input_devices 还不是个
	 * 链表，回调里遍历会踩空。 */
	vidow_config_observe(input_config_changed, server);
	/* 光标隐藏的闹钟（hide_after_inactive_ms；两个开关都关着时闹钟一直停着）。 */
	server->cursor_hide_timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server->display), cursor_hide_timeout,
		server);
	cursor_hide_arm(server);
	server->new_input.notify = server_new_input;
	wl_signal_add(&server->backend->events.new_input, &server->new_input);

	server->virtual_keyboard =
		wlr_virtual_keyboard_manager_v1_create(server->display);
	if (server->virtual_keyboard == NULL) {
		return false;
	}
	server->new_virtual_keyboard.notify = server_new_virtual_keyboard;
	wl_signal_add(&server->virtual_keyboard->events.new_virtual_keyboard,
		&server->new_virtual_keyboard);

	server->virtual_pointer =
		wlr_virtual_pointer_manager_v1_create(server->display);
	if (server->virtual_pointer != NULL) {
		server->new_virtual_pointer.notify = server_new_virtual_pointer;
		wl_signal_add(&server->virtual_pointer->events.new_virtual_pointer,
			&server->new_virtual_pointer);
	}

	server->cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
	server->cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server->cursor->events.motion_absolute,
		&server->cursor_motion_absolute);
	server->cursor_button.notify = server_cursor_button;
	wl_signal_add(&server->cursor->events.button, &server->cursor_button);
	server->cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
	server->cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);

	server->pointer_swipe_begin.notify = server_pointer_swipe_begin;
	wl_signal_add(&server->cursor->events.swipe_begin,
		&server->pointer_swipe_begin);
	server->pointer_swipe_update.notify = server_pointer_swipe_update;
	wl_signal_add(&server->cursor->events.swipe_update,
		&server->pointer_swipe_update);
	server->pointer_swipe_end.notify = server_pointer_swipe_end;
	wl_signal_add(&server->cursor->events.swipe_end,
		&server->pointer_swipe_end);
	server->pointer_pinch_begin.notify = server_pointer_pinch_begin;
	wl_signal_add(&server->cursor->events.pinch_begin,
		&server->pointer_pinch_begin);
	server->pointer_pinch_update.notify = server_pointer_pinch_update;
	wl_signal_add(&server->cursor->events.pinch_update,
		&server->pointer_pinch_update);
	server->pointer_pinch_end.notify = server_pointer_pinch_end;
	wl_signal_add(&server->cursor->events.pinch_end,
		&server->pointer_pinch_end);
	server->pointer_hold_begin.notify = server_pointer_hold_begin;
	wl_signal_add(&server->cursor->events.hold_begin,
		&server->pointer_hold_begin);
	server->pointer_hold_end.notify = server_pointer_hold_end;
	wl_signal_add(&server->cursor->events.hold_end,
		&server->pointer_hold_end);

	server->request_cursor.notify = server_request_cursor;
	wl_signal_add(&server->seat->events.request_set_cursor,
		&server->request_cursor);
	server->request_set_selection.notify = server_request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection,
		&server->request_set_selection);
	server->request_set_primary_selection.notify =
		server_request_set_primary_selection;
	wl_signal_add(&server->seat->events.request_set_primary_selection,
		&server->request_set_primary_selection);
	server->request_start_drag.notify = server_request_start_drag;
	wl_signal_add(&server->seat->events.request_start_drag,
		&server->request_start_drag);
	server->start_drag.notify = server_start_drag;
	wl_signal_add(&server->seat->events.start_drag, &server->start_drag);
	wl_list_init(&server->drag_destroy.link);
	wl_list_init(&server->drag_motion.link);

	/* 注入键盘（IPC 的 send-key / type-text）。建不起来只是那几个命令不能用，
	 * 不该拖垮整个输入初始化。 */
	vidow_input_inject_init(server);

	/* 摇晃找光标（cursor_shake）：指针的移动事件都经过 vidow_pointer_motion，
	 * 状态和定时器在这里建起来。 */
	vidow_cursor_fx_init(server);

	return true;
}

/* ------------------------------------------------------------------ 注入 */

/* IPC 的输入注入（自动化 / 远程操控）。两条原则：
 *   1. 走和真设备完全同一条路：指针走 vidow_pointer_motion / _button /
 *      _axis，按键走 seat 的 notify_*。所以「注入的点击」和真点击在
 *      合成器里是同一种东西（该抢焦点就抢、该起拖拽就起）。
 *   2. 按键只给**客户端**，不触发 vidow 自己的快捷键 —— 自动化要操作合成器
 *      有专门的 IPC 命令，而它想给应用发 Ctrl+C 时不该被 Mod+C 之类的绑定吃掉。
 */

#define INJECT_MAX_MODS 4

struct inject_combo {
	uint32_t mods[INJECT_MAX_MODS]; /* 修饰键的 evdev 键码，按要按下的顺序 */
	size_t nmods;
	uint32_t keycode;
};

static uint32_t inject_now_ms(void) {
	return (uint32_t)(vidow_now_ns() / 1000000LL);
}

bool vidow_inject_allowed(struct vidow_server *server, char *err, size_t errlen) {
	if (!server->config.input_injection) {
		snprintf(err, errlen, "输入注入被配置关了（input_injection = false）");
		return false;
	}
	if (server->inject_group == NULL) {
		snprintf(err, errlen, "注入键盘没建起来（看启动日志）");
		return false;
	}
	return true;
}

static struct wlr_keyboard *inject_keyboard(struct vidow_server *server) {
	return &server->inject_group->keyboard;
}

/* 把注入键盘借给 seat：客户端是按「seat 上那个键盘的 keymap」解释键码的，
 * 注入的键码也是按它算出来的，两边必须是同一份。真键盘按下时会自己抢回去
 * （keyboard_key 里每次透传都 set 一遍），所以这里不用还原。 */
static void inject_take_seat_keyboard(struct vidow_server *server) {
	struct wlr_keyboard *kb = inject_keyboard(server);
	if (wlr_seat_get_keyboard(server->seat) != kb) {
		wlr_seat_set_keyboard(server->seat, kb);
	}
}

/* 一个按键事件。xkb_state 和修饰键得自己更新：wlroots 那套是设备侧
 * （wlr_keyboard_notify_key）做的，注入没有设备，只能照它抄一遍。 */
static void inject_key(struct vidow_server *server, uint32_t keycode,
		bool pressed) {
	struct wlr_keyboard *kb = inject_keyboard(server);
	inject_take_seat_keyboard(server);

	if (kb->xkb_state != NULL) {
		xkb_state_update_key(kb->xkb_state, keycode + 8,
			pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
		kb->modifiers.depressed = xkb_state_serialize_mods(kb->xkb_state,
			XKB_STATE_MODS_DEPRESSED);
		kb->modifiers.latched = xkb_state_serialize_mods(kb->xkb_state,
			XKB_STATE_MODS_LATCHED);
		kb->modifiers.locked = xkb_state_serialize_mods(kb->xkb_state,
			XKB_STATE_MODS_LOCKED);
		kb->modifiers.group = xkb_state_serialize_layout(kb->xkb_state,
			XKB_STATE_LAYOUT_EFFECTIVE);
	}

	/* 注入的键也算「人按了键」：打字藏光标 / 唤醒关掉的屏两个开关都认它
	 * （headless 里没有真键盘，这两个开关只有注入这条路能验）。 */
	if (pressed) {
		input_note_key_press(server);
	}

	/* 会话锁：锁定期间注入的键走**真键盘那条路**（lock_key_route：白名单 +
	 * lock surface）。注入平时不触发键位表（下面那条注释），锁上以后要是还
	 * 直接往 seat 里塞，测试就量不到白名单 —— 「注入一个已绑定的键，工作区
	 * 变不变」正是这么量的。注入本来就是测试 / 调试入口，锁上以后和真按键
	 * 同一条路才说得通。
	 * 修饰键也放在焦点切过去**之后**发：不然它会先落到被藏起来的那个窗口上。 */
	if (server->locked) {
		lock_focus_keyboard(server);
		if (kb->xkb_state != NULL) {
			wlr_seat_keyboard_notify_modifiers(server->seat, &kb->modifiers);
		}
		lock_key_route(server, kb, inject_now_ms(), keycode,
			pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
				: WL_KEYBOARD_KEY_STATE_RELEASED);
		return;
	}

	if (kb->xkb_state != NULL) {
		wlr_seat_keyboard_notify_modifiers(server->seat, &kb->modifiers);
	}

	/* 注入的键也走「X11 抓着键盘就给它」那条规矩（不然测试里没法验证抓取：
	 * 真按键不好造，注入的键又不触发键位，正好拿来量「键送给了谁」）。 */
	vidow_xwayland_grab_assert(server);
	wlr_seat_keyboard_notify_key(server->seat, inject_now_ms(), keycode,
		pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
			: WL_KEYBOARD_KEY_STATE_RELEASED);
}

/* 在 keymap 里反查「哪个键、哪个 level 能打出这个 keysym」。keymap 里的键码
 * 从 8 开始（evdev 码 + 8），返回时减回去。 */
static bool inject_lookup_key(struct wlr_keyboard *kb, xkb_keysym_t sym,
		uint32_t *keycode, xkb_level_index_t *level) {
	if (kb->keymap == NULL || sym == XKB_KEY_NoSymbol) {
		return false;
	}
	xkb_keycode_t min = xkb_keymap_min_keycode(kb->keymap);
	xkb_keycode_t max = xkb_keymap_max_keycode(kb->keymap);
	xkb_layout_index_t layouts = xkb_keymap_num_layouts(kb->keymap);
	for (xkb_keycode_t kc = min; kc <= max; kc++) {
		for (xkb_layout_index_t l = 0; l < layouts; l++) {
			xkb_level_index_t levels =
				xkb_keymap_num_levels_for_key(kb->keymap, kc, l);
			for (xkb_level_index_t lv = 0; lv < levels; lv++) {
				const xkb_keysym_t *syms = NULL;
				int n = xkb_keymap_key_get_syms_by_level(kb->keymap, kc, l,
					lv, &syms);
				for (int i = 0; i < n; i++) {
					if (syms[i] == sym) {
						*keycode = kc - 8;
						*level = lv;
						return true;
					}
				}
			}
		}
	}
	return false;
}

/* level → 要按住的修饰键。0 = 不用，1 = Shift，2 = AltGr，3 = 两个都要 ——
 * 这是 xkb 里 4 级布局的通用约定（us / de / ru 这些常见布局都符合）。 */
static size_t inject_level_keys(xkb_level_index_t level, xkb_keysym_t *out) {
	size_t n = 0;
	if (level == 1 || level == 3) {
		out[n++] = XKB_KEY_Shift_L;
	}
	if (level == 2 || level == 3) {
		out[n++] = XKB_KEY_ISO_Level3_Shift;
	}
	return n;
}

static xkb_keysym_t inject_mod_keysym(const char *name) {
	if (strcasecmp(name, "shift") == 0) {
		return XKB_KEY_Shift_L;
	}
	if (strcasecmp(name, "ctrl") == 0 || strcasecmp(name, "control") == 0) {
		return XKB_KEY_Control_L;
	}
	if (strcasecmp(name, "alt") == 0 || strcasecmp(name, "mod1") == 0) {
		return XKB_KEY_Alt_L;
	}
	if (strcasecmp(name, "super") == 0 || strcasecmp(name, "logo") == 0 ||
			strcasecmp(name, "meta") == 0 || strcasecmp(name, "win") == 0 ||
			strcasecmp(name, "mod4") == 0) {
		return XKB_KEY_Super_L;
	}
	if (strcasecmp(name, "altgr") == 0 || strcasecmp(name, "level3") == 0) {
		return XKB_KEY_ISO_Level3_Shift;
	}
	return XKB_KEY_NoSymbol;
}

/* UTF-8 解一个码点，返回下一个字符的位置。坏字节按 U+FFFD 走。 */
static const char *inject_utf8_next(const char *s, uint32_t *cp) {
	unsigned char c = (unsigned char)*s;
	if (c < 0x80) {
		*cp = c;
		return s + 1;
	}
	int extra;
	uint32_t v;
	if ((c & 0xE0) == 0xC0) {
		extra = 1;
		v = c & 0x1F;
	} else if ((c & 0xF0) == 0xE0) {
		extra = 2;
		v = c & 0x0F;
	} else if ((c & 0xF8) == 0xF0) {
		extra = 3;
		v = c & 0x07;
	} else {
		*cp = 0xFFFD;
		return s + 1;
	}
	for (int i = 0; i < extra; i++) {
		if ((s[1 + i] & 0xC0) != 0x80) {
			*cp = 0xFFFD;
			return s + 1 + i;
		}
		v = (v << 6) | (uint32_t)(s[1 + i] & 0x3F);
	}
	*cp = v;
	return s + 1 + extra;
}

/* 一个键名（keysym 名字 / 单字符 / 修饰键别名）→ keysym */
static xkb_keysym_t inject_key_sym(const char *name) {
	xkb_keysym_t sym = inject_mod_keysym(name);
	if (sym != XKB_KEY_NoSymbol) {
		return sym;
	}
	/* 先按原样找（大小写敏感）：'M' 和 'm' 是两个不同的键（差一个 Shift） */
	sym = xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS);
	if (sym != XKB_KEY_NoSymbol) {
		return sym;
	}
	sym = xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
	if (sym != XKB_KEY_NoSymbol) {
		return sym;
	}
	uint32_t cp = 0;
	const char *rest = inject_utf8_next(name, &cp);
	if (*rest == '\0' && cp != 0 && cp != 0xFFFD) {
		return xkb_utf32_to_keysym(cp);
	}
	return XKB_KEY_NoSymbol;
}

static bool inject_combo_add_mod(struct wlr_keyboard *kb,
		struct inject_combo *c, xkb_keysym_t sym) {
	uint32_t keycode = 0;
	xkb_level_index_t level = 0;
	if (!inject_lookup_key(kb, sym, &keycode, &level)) {
		return false;
	}
	for (size_t i = 0; i < c->nmods; i++) {
		if (c->mods[i] == keycode) {
			return true; /* 已经要按了（比如 "shift+A" 里 Shift 只按一次） */
		}
	}
	if (c->nmods >= INJECT_MAX_MODS) {
		return false;
	}
	c->mods[c->nmods++] = keycode;
	return true;
}

/* "ctrl+shift+t" → 修饰键列表 + 主键。最后一个 '+' 后面那个才是主键。 */
static bool inject_combo_parse(struct vidow_server *server, const char *combo,
		struct inject_combo *out, char *err, size_t errlen) {
	struct wlr_keyboard *kb = inject_keyboard(server);
	char buf[128];
	snprintf(buf, sizeof(buf), "%s", combo);
	out->nmods = 0;
	out->keycode = 0;

	char *save = NULL;
	char *tok = strtok_r(buf, "+", &save);
	char *main_tok = NULL;
	while (tok != NULL) {
		if (main_tok != NULL) {
			xkb_keysym_t msym = inject_mod_keysym(main_tok);
			if (msym == XKB_KEY_NoSymbol) {
				snprintf(err, errlen, "不认识的修饰键 '%s'"
					"（shift / ctrl / alt / super / altgr）", main_tok);
				return false;
			}
			if (!inject_combo_add_mod(kb, out, msym)) {
				snprintf(err, errlen, "这个键盘布局里没有 '%s' 这个修饰键",
					main_tok);
				return false;
			}
		}
		main_tok = tok;
		tok = strtok_r(NULL, "+", &save);
	}
	if (main_tok == NULL) {
		snprintf(err, errlen, "没给按键");
		return false;
	}

	xkb_keysym_t sym = inject_key_sym(main_tok);
	xkb_level_index_t level = 0;
	if (sym == XKB_KEY_NoSymbol ||
			!inject_lookup_key(kb, sym, &out->keycode, &level)) {
		snprintf(err, errlen, "当前键盘布局里没有 '%s' 这个键"
			"（写 keysym 名字：Return / Tab / a / F5 / XF86AudioMute）", main_tok);
		return false;
	}
	/* 主键自己所在的 level 也要按修饰键："A" 要 Shift，"@" 要 Shift+2 */
	xkb_keysym_t lvl_keys[2];
	size_t nlvl = inject_level_keys(level, lvl_keys);
	for (size_t i = 0; i < nlvl; i++) {
		if (!inject_combo_add_mod(kb, out, lvl_keys[i])) {
			snprintf(err, errlen, "当前键盘布局缺 Shift / AltGr 键位，"
				"打不出 '%s'", main_tok);
			return false;
		}
	}
	return true;
}

/* 按下 → 松开一个 keysym（自己处理 level 需要的 Shift / AltGr） */
static bool inject_tap_sym(struct vidow_server *server, xkb_keysym_t sym) {
	struct wlr_keyboard *kb = inject_keyboard(server);
	uint32_t keycode = 0;
	xkb_level_index_t level = 0;
	if (!inject_lookup_key(kb, sym, &keycode, &level)) {
		return false;
	}
	xkb_keysym_t lvl_keys[2];
	size_t nlvl = inject_level_keys(level, lvl_keys);
	uint32_t modcodes[2];
	size_t nmods = 0;
	for (size_t i = 0; i < nlvl; i++) {
		xkb_level_index_t ml = 0;
		if (!inject_lookup_key(kb, lvl_keys[i], &modcodes[nmods], &ml)) {
			return false;
		}
		nmods++;
	}
	for (size_t i = 0; i < nmods; i++) {
		inject_key(server, modcodes[i], true);
	}
	inject_key(server, keycode, true);
	inject_key(server, keycode, false);
	for (size_t i = nmods; i-- > 0;) {
		inject_key(server, modcodes[i], false);
	}
	return true;
}

bool vidow_inject_pointer_move(struct vidow_server *server, double x, double y,
		bool relative, char *err, size_t errlen) {
	if (!vidow_inject_allowed(server, err, errlen)) {
		return false;
	}
	double tx = relative ? server->cursor->x + x : x;
	double ty = relative ? server->cursor->y + y : y;
	if (!wlr_cursor_warp(server->cursor, NULL, tx, ty)) {
		snprintf(err, errlen, "%.0f,%.0f 在输出布局之外", tx, ty);
		return false;
	}
	/* wlr_cursor_warp 只挪位置、不发事件（wlroots 的 cursor_warp_unchecked），
	 * 所以显式走一遍处理：指针焦点、enter/motion、拖拽的 grab 分支都在里面。 */
	vidow_pointer_motion(server, inject_now_ms());
	/* 真设备每个帧事件都会收一个 wl_pointer.frame（libinput 的帧事件），注入
	 * 这条路没有设备帧 —— 不补上，wl_pointer v5+ 的客户端（GTK/GDK 就是）会把
	 * 这些事件扣在缓冲里等下一个 frame：实测「按下之后什么都不做」客户端 5 秒
	 * 都收不到，一动鼠标才一起收下（测试里就是这条把时序全带偏了）。滚轮注入
	 * 一直是这么补的（见 vidow_inject_pointer_scroll），这里跟上。 */
	wlr_seat_pointer_notify_frame(server->seat);
	/* 日志：同一个 100ms 窗口里的**第一条走 info，剩下的走 debug**。
	 *
	 * 注入是给脚本 / 测试用的，注入出来的指针能比真鼠标还快（基准里灌过 20000 条
	 * 连发）—— 每条一行 info 既刷屏又拖慢注入本身（一行一次 write + 一次时间格式
	 * 化）。手点一下、脚本挪一下照样看得见（info 下的第一条）；要把连发一条不漏地
	 * 看全，`VIDOW_LOG=debug`。 */
	static int64_t last_log_ns = 0;
	int64_t now_ns = vidow_now_ns();
	bool first = now_ns - last_log_ns >= 100 * 1000000LL;
	if (first) {
		last_log_ns = now_ns;
	}
	wlr_log(first ? WLR_INFO : WLR_DEBUG, "注入: 指针%s移到 %.0f,%.0f",
		relative ? "相对" : "", server->cursor->x, server->cursor->y);
	return true;
}

uint32_t vidow_pointer_button_from_name(const char *name) {
	if (name == NULL || name[0] == '\0') {
		return 0;
	}
	if (strcasecmp(name, "left") == 0) {
		return BTN_LEFT;
	}
	if (strcasecmp(name, "right") == 0) {
		return BTN_RIGHT;
	}
	if (strcasecmp(name, "middle") == 0) {
		return BTN_MIDDLE;
	}
	if (strcasecmp(name, "back") == 0) {
		return BTN_SIDE;
	}
	if (strcasecmp(name, "forward") == 0) {
		return BTN_EXTRA;
	}
	/* 也认数字（BTN_LEFT = 272 这种写在脚本里没人记得住） */
	char *end = NULL;
	long v = strtol(name, &end, 10);
	if (end != NULL && *end == '\0' && v > 0 && v < 0x200) {
		return (uint32_t)v;
	}
	return 0;
}

bool vidow_inject_pointer_button(struct vidow_server *server, const char *name,
		int mode, int count, char *err, size_t errlen) {
	if (!vidow_inject_allowed(server, err, errlen)) {
		return false;
	}
	uint32_t btn = vidow_pointer_button_from_name(name);
	if (btn == 0) {
		snprintf(err, errlen, "不认识的按钮 '%s'"
			"（left / right / middle / back / forward / 数字）",
			name != NULL ? name : "");
		return false;
	}
	if (count < 1) {
		count = 1;
	}
	for (int i = 0; i < count; i++) {
		/* 连点：时间戳拉开 40ms，客户端才会把两下认成双击 */
		uint32_t t = inject_now_ms() + (uint32_t)(i * 40);
		if (mode != 2) {
			vidow_pointer_button(server, btn, true, t, false);
		}
		if (mode != 1) {
			vidow_pointer_button(server, btn, false, t, false);
		}
	}
	/* 注入没有设备帧：自己收一个（同 vidow_inject_pointer_move，滚轮那条一直
	 * 是这么补的）。少了它，客户端会把这次按下扣在缓冲里等下一个 frame。 */
	wlr_seat_pointer_notify_frame(server->seat);
	wlr_log(WLR_INFO, "注入: 按钮 %u %s%s（在 %.0f,%.0f）", btn,
		mode == 0 ? "点一下" : mode == 1 ? "按下" : "松开",
		count > 1 ? "（连点）" : "", server->cursor->x, server->cursor->y);
	return true;
}

/* 键盘焦点按到某个表面上（所有 notify_enter 都从这里走）。
 *
 * 为什么要这个壳：wlr_seat_keyboard_notify_enter 里第一行就是 assert(surface)，
 * 传 NULL 不是「不给焦点」而是直接把合成器 abort —— 实测崩在这儿的场景是 wine /
 * Proton 的游戏起来时：X11 的窗口已经建出来、也拿到了焦点，但表面还没到（或者
 * 刚销毁），这时候滚一下滚轮（Mod+滚轮 = focus-column）就 abort 了。NULL 的语义
 * 就是「没人该拿键盘」，所以清掉焦点。 */
void vidow_seat_focus_surface(struct wlr_seat *seat,
		struct wlr_surface *surface) {
	if (seat == NULL) {
		return;
	}
	if (surface == NULL) {
		wlr_log(WLR_DEBUG, "键盘焦点: 目标窗口还没有表面（NULL），先清掉焦点"
			"（等它真有表面了会再进来一次）");
		wlr_seat_keyboard_notify_clear_focus(seat);
		return;
	}
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	if (kb != NULL) {
		wlr_seat_keyboard_notify_enter(seat, surface, kb->keycodes,
			kb->num_keycodes, &kb->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, NULL);
	}
}

bool vidow_inject_pointer_scroll(struct vidow_server *server, double dx,
		double dy, bool discrete, char *err, size_t errlen) {
	if (!vidow_inject_allowed(server, err, errlen)) {
		return false;
	}
	if (dx == 0 && dy == 0) {
		snprintf(err, errlen, "dx / dy 都是 0，没得滚");
		return false;
	}
	enum wl_pointer_axis_source source = discrete
		? WL_POINTER_AXIS_SOURCE_WHEEL : WL_POINTER_AXIS_SOURCE_CONTINUOUS;
	/* 滚轮模式下数字是**格数**：一格 = delta 15 像素 + delta_discrete 120，
	 * 和真鼠标经 libinput 过来的一模一样（libinput 的轴值 ≈15/格，离散值按
	 * WLR_POINTER_AXIS_DISCRETE_STEP = 120 放大）。连续模式就按原值给。 */
	double ddx = discrete ? dx * 15.0 : dx;
	double ddy = discrete ? dy * 15.0 : dy;
	int32_t sdx = discrete ? (int32_t)lround(dx * 120.0) : 0;
	int32_t sdy = discrete ? (int32_t)lround(dy * 120.0) : 0;
	uint32_t t = inject_now_ms();
	if (ddy != 0) {
		vidow_pointer_axis(server, WL_POINTER_AXIS_VERTICAL_SCROLL, ddy, sdy,
			source, t);
	}
	if (ddx != 0) {
		vidow_pointer_axis(server, WL_POINTER_AXIS_HORIZONTAL_SCROLL, ddx, sdx,
			source, t);
	}
	wlr_seat_pointer_notify_frame(server->seat);
	wlr_log(WLR_INFO, "注入: 滚轮 %.1f,%.1f 格（%s）", dx, dy,
		discrete ? "滚轮" : "连续");
	return true;
}

bool vidow_inject_key_combo(struct vidow_server *server, const char *combo,
		int mode, char *err, size_t errlen) {
	if (!vidow_inject_allowed(server, err, errlen)) {
		return false;
	}
	if (combo == NULL || combo[0] == '\0') {
		snprintf(err, errlen, "要一个按键（ctrl+shift+t / Return / a）");
		return false;
	}
	struct inject_combo c;
	if (!inject_combo_parse(server, combo, &c, err, errlen)) {
		return false;
	}
	if (mode != 2) { /* tap / press：修饰键先按住 */
		for (size_t i = 0; i < c.nmods; i++) {
			inject_key(server, c.mods[i], true);
		}
		inject_key(server, c.keycode, true);
	}
	if (mode != 1) { /* tap / release：主键先松开，再松修饰键 */
		inject_key(server, c.keycode, false);
		for (size_t i = c.nmods; i-- > 0;) {
			inject_key(server, c.mods[i], false);
		}
	}
	wlr_log(WLR_INFO, "注入: 按键 %s（%s）", combo,
		mode == 0 ? "按下并松开" : mode == 1 ? "按下" : "松开");
	return true;
}

bool vidow_inject_text(struct vidow_server *server, const char *text, char *err,
		size_t errlen) {
	if (!vidow_inject_allowed(server, err, errlen)) {
		return false;
	}
	if (text == NULL || text[0] == '\0') {
		snprintf(err, errlen, "要一段文字");
		return false;
	}
	size_t done = 0;
	const char *p = text;
	while (*p != '\0') {
		uint32_t cp = 0;
		p = inject_utf8_next(p, &cp);
		xkb_keysym_t sym = xkb_utf32_to_keysym(cp);
		if (sym == XKB_KEY_NoSymbol || !inject_tap_sym(server, sym)) {
			/* 中文这类字符得靠输入法，不是「按个键」能打出来的 */
			snprintf(err, errlen, "第 %zu 个字符（U+%04X）在当前键盘布局里"
				"打不出来（中文之类要走输入法）", done + 1, cp);
			return false;
		}
		done++;
	}
	wlr_log(WLR_INFO, "注入: 打了 %zu 个字符", done);
	return true;
}

bool vidow_input_inject_init(struct vidow_server *server) {
	/* wlroots 没有公开的「造一个键盘」接口，只有键盘组能给出一个干净的
	 * wlr_keyboard（自带 keymap / xkb_state）—— 借它的 keyboard 成员正好够用。 */
	server->inject_group = wlr_keyboard_group_create();
	if (server->inject_group == NULL) {
		wlr_log(WLR_ERROR, "注入键盘建不起来：IPC 的 send-key / type-text 不能用");
		return false;
	}
	struct wlr_keyboard *kb = &server->inject_group->keyboard;
	free(kb->base.name);
	kb->base.name = strdup("vidow-inject");
	/* 挂进 keyboards 列表：headless 会话里它就是「这个座位有键盘」的唯一证据
	 * （少了它客户端连 wl_keyboard 都不会去要，注入的键也就没地方送）。
	 * keymap 走 server_add_keyboard 里那套（系统默认布局），和真键盘一致。 */
	server_add_keyboard(server, kb, &kb->base, NULL, true);
	seat_update_capabilities(server);
	wlr_log(WLR_INFO, "注入键盘就绪：%s（IPC 的 send-key / type-text 用它）",
		kb->base.name);
	return true;
}
