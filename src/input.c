#include "server.h"

#include <stdlib.h>
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
		struct wlr_virtual_keyboard_v1 *virtual) {
	struct vidow_keyboard *kb = calloc(1, sizeof(*kb));
	if (kb == NULL) {
		return;
	}
	kb->server = server;
	kb->wlr_keyboard = wlr_keyboard;
	kb->virtual = virtual;

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
		NULL);
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
		&virtual_keyboard->keyboard.base, virtual_keyboard);

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
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");
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

	/* overview 里滚轮平移相机（世界坐标的一小步；换列会把平移清零、改回跟焦点走） */
	if (vidow_overview_active(server)) {
		double delta = event->delta != 0 ? event->delta : event->delta_discrete;
		if (delta != 0) {
			vidow_overview_pan(server, delta > 0 ? 60 : -60);
		}
		return;
	}

	/* Mod+wheel moves the view between columns, like niri's view scroll.
	 * A plain wheel is forwarded to the client so apps keep scrolling. */
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	if (kb != NULL && (wlr_keyboard_get_modifiers(kb) & MOD_KEY)) {
		double delta = event->delta != 0 ? event->delta : event->delta_discrete;
		if (delta != 0) {
			vidow_focus_column(server, delta > 0 ? 1 : -1);
		}
		return;
	}

	wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
		event->orientation, event->delta, event->delta_discrete, event->source,
		event->relative_direction);
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

	return true;
}
