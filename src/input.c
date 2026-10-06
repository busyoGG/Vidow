#include "server.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <wayland-server-protocol.h>
#include <wlr/util/log.h>

#define MOD_KEY WLR_MODIFIER_LOGO /* Super */

/* ------------------------------------------------------------- keybinds */

static void spawn(const char *cmd) {
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log(WLR_ERROR, "fork failed");
		return;
	}
	if (pid == 0) {
		setsid();
		execl("/bin/sh", "sh", "-c", cmd, (void *)NULL);
		_exit(127);
	}
}

/* `shift` comes from the modifier mask: some input paths (virtual keyboards)
 * report the unshifted keysym even while Shift is held. */
static bool handle_keybinding(struct vidow_server *server, xkb_keysym_t sym,
		uint32_t mods) {
	bool shift = (mods & WLR_MODIFIER_SHIFT) != 0;
	bool ctrl = (mods & WLR_MODIFIER_CTRL) != 0;
	sym = xkb_keysym_to_lower(sym);
	switch (sym) {
	case XKB_KEY_Escape:
		wl_display_terminate(server->display);
		return true;
	case XKB_KEY_Return: {
		const char *term = getenv("VIDOW_TERMINAL");
		spawn(term != NULL ? term : "kitty");
		return true;
	}
	case XKB_KEY_q:
		if (server->focused != NULL) {
			vidow_view_close(server->focused);
		}
		return true;
	case XKB_KEY_h: /* Mod+H / Mod+Shift+H: focus / move the column left */
		if (shift) {
			vidow_move_column(server, -1);
		} else {
			vidow_focus_column(server, -1);
		}
		return true;
	case XKB_KEY_l:
		if (shift) {
			vidow_move_column(server, 1);
		} else {
			vidow_focus_column(server, 1);
		}
		return true;
	case XKB_KEY_j: /* Mod+J / Mod+Shift+J: focus / move down the column */
		if (shift) {
			vidow_move_window(server, 1);
		} else {
			vidow_focus_window(server, 1);
		}
		return true;
	case XKB_KEY_k:
		if (shift) {
			vidow_move_window(server, -1);
		} else {
			vidow_focus_window(server, -1);
		}
		return true;
	case XKB_KEY_v: /* Mod+V / Mod+Shift+Space: toggle floating */
	case XKB_KEY_space:
		if (sym == XKB_KEY_v || shift) {
			if (server->focused != NULL) {
				vidow_float_toggle(server->focused);
			}
			return true;
		}
		return false;
	case XKB_KEY_r: /* cycle column width: 1/3 -> 1/2 -> 2/3 */
		vidow_cycle_column_width(server);
		return true;
	case XKB_KEY_m: /* Mod+M 收起当前窗口；Mod+Shift+M 放回最近收起来的那个 */
		if (shift) {
			if (!vidow_view_restore_last(server)) {
				wlr_log(WLR_INFO, "放回: 现在没有最小化的窗口");
			}
			return true;
		}
		if (server->focused != NULL) {
			vidow_view_set_minimized(server->focused, true);
		}
		return true;
	case XKB_KEY_o: /* niri 风格的全景：整条列带缩小铺满这块屏 */
		vidow_overview_toggle(server);
		return true;
	/* 工作区（niri 的默认键位）：Mod+U/I 上下切、Mod+Shift+U/I 把窗口搬过去、
	 * Mod+Ctrl+U/I 整块工作区重排。工作区是「每块屏一份、栈底永远空一块」，
	 * 所以上下切就是最常用的操作；数字键直跳写在默认配置里（可删）。 */
	case XKB_KEY_u:
		vidow_action_run(server, ctrl ? "move-workspace down" :
			(shift ? "move down" : "workspace down"));
		return true;
	case XKB_KEY_i:
		vidow_action_run(server, ctrl ? "move-workspace up" :
			(shift ? "move up" : "workspace up"));
		return true;
	case XKB_KEY_f:
		if (server->focused != NULL) {
			vidow_view_toggle_fullscreen(server->focused);
		}
		return true;
	default:
		return false;
	}
}

/* ------------------------------------------------------------- keyboard */

static void keyboard_modifiers(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_keyboard *kb = wl_container_of(listener, kb, modifiers);

	/* 输入法抢着键盘时修饰键也归它：它会连同按键一起用虚拟键盘回注。 */
	if (vidow_im_owns_keyboard(kb->server, kb)) {
		vidow_im_modifiers(kb->server, kb->wlr_keyboard);
		return;
	}

	wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(kb->server->seat,
		&kb->wlr_keyboard->modifiers);
}

/* Ctrl+Alt+F1..F12：切 tty（真机上内核多半自己就切了，见 vidow_vt_switch）。
 *
 * 判据是「F1..F12 + Ctrl+Alt」而不是 XF86Switch_VT_n 那个 keysym：默认键位表里
 * FK01..FK12 的第 5 个符号才是 XF86Switch_VT_n，而 CTRL+ALT 这个 type 把 Control
 * 映射回第 1 层（preserve Control）—— 真按 Ctrl+Alt+F2 拿到的是 **F2** 这个 keysym
 * 加上 ctrl|alt 修饰（`xkbcli how-to-type --keysym XF86Switch_VT_2` 一个 keycode
 * 都找不到，就是这个原因）。两个都认：键位表把 keysym 配上时那条也能用。 */
static bool vt_switch_key(struct vidow_server *server, xkb_keysym_t sym,
		uint32_t mods) {
	unsigned vt;
	if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12 &&
			(mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) ==
				(WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) {
		vt = (unsigned)(sym - XKB_KEY_F1) + 1;
	} else if (sym >= XKB_KEY_XF86Switch_VT_1 &&
			sym <= XKB_KEY_XF86Switch_VT_12) {
		vt = (unsigned)(sym - XKB_KEY_XF86Switch_VT_1) + 1;
	} else {
		return false;
	}
	/* 没有会话就别吃这个键：嵌套里那是宿主（Hyprland）的 Ctrl+Alt+F2。 */
	if (server->session == NULL) {
		wlr_log(WLR_DEBUG, "VT: 没有会话（嵌套 / headless），Ctrl+Alt+F%u 交给宿主",
			vt);
		return false;
	}
	return vidow_vt_switch(server, vt);
}

static void keyboard_key(struct wl_listener *listener, void *data) {
	struct vidow_keyboard *kb = wl_container_of(listener, kb, key);
	struct vidow_server *server = kb->server;
	struct wlr_keyboard_key_event *event = data;

	/* evdev keycode -> xkb keycode */
	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state, keycode, &syms);


	bool handled = false;
	uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);
	/* 0) 切 tty 的键（Ctrl+Alt+F1..F12）：只有真机（有会话）才接管。 */
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nsyms; i++) {
			handled = vt_switch_key(server, syms[i], mods) || handled;
		}
	}
	/* 1) 内置键位：进 overview、退出合成器、关窗、丢给布局的那些。 */
	if ((mods & MOD_KEY) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nsyms; i++) {
			handled = handle_keybinding(server, syms[i], mods) || handled;
		}
	}
	/* 2) config.lua 里 vidow.bind() 声明的键位：工作区切换 / 特殊工作区 / 搬家。
	 * 合成器自己一个工作区键都不预设 —— 用户绑什么就是什么。没命中的按键
	 * 照旧透传给客户端。 */
	if (!handled && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nsyms; i++) {
			handled = vidow_keybind_key(server, syms[i], mods) || handled;
		}
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
	struct xkb_context *context = virtual != NULL ? NULL : xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (context != NULL) {
		struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (keymap != NULL) {
			wlr_keyboard_set_keymap(wlr_keyboard, keymap);
			xkb_keymap_unref(keymap);
		}
		xkb_context_unref(context);
	}
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

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
static void server_new_virtual_keyboard(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *virtual_keyboard = data;

	wlr_log(WLR_INFO, "virtual keyboard attached");
	server_add_keyboard(server, &virtual_keyboard->keyboard,
		&virtual_keyboard->keyboard.base, virtual_keyboard, false);

	seat_update_capabilities(server);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	struct vidow_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;

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
	if (server->grab_mode == VIDOW_GRAB_TILE) {
		vidow_tile_grab_update(server, server->cursor->x, server->cursor->y);
		return;
	}
	vidow_float_grab_update(server, server->cursor->x, server->cursor->y);
}

void vidow_pointer_motion(struct vidow_server *server, uint32_t time_msec) {
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
		wlr_seat_pointer_notify_clear_focus(server->seat);
	}
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

/* 指针按下 / 松开。测试注入（VIDOW_TEST_ACTION 里的 pointer-button）也走这里，
 * 和真实鼠标完全同一条路。 */
void vidow_pointer_button(struct vidow_server *server, uint32_t button,
		bool pressed, uint32_t time_msec, bool force_mod) {
	if (server->grab_mode != VIDOW_GRAB_NONE) {
		/* the grab owns the pointer: swallow everything until release */
		if (!pressed) {
			if (server->grab_mode == VIDOW_GRAB_TILE) {
				vidow_tile_grab_end(server);
			} else {
				vidow_float_end_grab(server);
			}
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

	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	bool mod = force_mod || (kb != NULL &&
		(wlr_keyboard_get_modifiers(kb) & MOD_KEY));

	/* Mod+left drag moves a float, Mod+right drag resizes it. */
	if (mod && view != NULL && view->float_mode != VIDOW_FLOAT_NONE) {
		vidow_float_begin_grab(server, view, button == BTN_RIGHT);
		if (view != server->focused) {
			vidow_focus_view(view);
		}
		return;
	}

	/* Mod+左键按住**平铺**窗口 = 拖它（niri 的 interactive move）：窗口跟着光标
	 * 飘起来，松手按落点决定归哪块屏的工作区、插在列带第几位。 */
	if (mod && button == BTN_LEFT && view != NULL && !view->popup &&
			view->float_mode == VIDOW_FLOAT_NONE && view->column != NULL) {
		vidow_tile_grab_begin(server, view);
		return;
	}

	wlr_seat_pointer_notify_button(server->seat, time_msec, button,
		WL_POINTER_BUTTON_STATE_PRESSED);

	if (view != NULL && view != server->focused) {
		vidow_focus_view(view);
	}
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
	/* 有些设备只发离散值（老鼠标），所以两个都看 */
	double amount = delta != 0 ? delta : delta_discrete;

	/* overview 里滚轮平移相机（世界坐标的一小步；换列会把平移清零、改回跟焦点走） */
	if (vidow_overview_active(server)) {
		if (amount != 0) {
			vidow_overview_pan(server, amount > 0 ? 60 : -60);
		}
		return;
	}

	/* Mod+wheel moves the view between columns, like niri's view scroll.
	 * A plain wheel is forwarded to the client so apps keep scrolling. */
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	if (kb != NULL && (wlr_keyboard_get_modifiers(kb) & MOD_KEY)) {
		if (amount != 0) {
			vidow_focus_column(server, amount > 0 ? 1 : -1);
		}
		return;
	}

	wlr_seat_pointer_notify_axis(server->seat, time_msec, orientation, delta,
		delta_discrete, source, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_server *server = wl_container_of(listener, server, cursor_frame);

	wlr_seat_pointer_notify_frame(server->seat);
}

/* ------------------------------------------------------------------ seat */

static void server_request_cursor(struct wl_listener *listener, void *data) {
	struct vidow_server *server = wl_container_of(listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;

	struct wlr_seat_client *focused = server->seat->pointer_state.focused_client;
	if (focused != event->seat_client) {
		return;
	}
	/* 先告诉摇晃放大「光标图要换了」（放大中就直接收摊），再设上去。 */
	vidow_cursor_fx_set_surface(server, event->surface, event->hotspot_x,
		event->hotspot_y);
	wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x,
		event->hotspot_y);
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

/* wlrctl and other test tools drive the pointer through this protocol. */
static void server_new_virtual_pointer(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, new_virtual_pointer);
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	struct wlr_virtual_pointer_v1 *virtual_pointer = event->new_pointer;

	wlr_log(WLR_INFO, "virtual pointer attached");
	wlr_cursor_attach_input_device(server->cursor,
		&virtual_pointer->pointer.base);
	/* 虚拟指针也是指针：座位得把 POINTER 报出来，否则客户端收不到指针事件
	 * （headless 会话里这是唯一的指针来源）。 */
	seat_update_capabilities(server);
}

bool vidow_input_init(struct vidow_server *server) {
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
		wlr_seat_keyboard_notify_modifiers(server->seat, &kb->modifiers);
	}

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
	wlr_log(WLR_INFO, "注入: 指针%s移到 %.0f,%.0f", relative ? "相对" : "",
		server->cursor->x, server->cursor->y);
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
	wlr_log(WLR_INFO, "注入: 按钮 %u %s%s（在 %.0f,%.0f）", btn,
		mode == 0 ? "点一下" : mode == 1 ? "按下" : "松开",
		count > 1 ? "（连点）" : "", server->cursor->x, server->cursor->y);
	return true;
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
