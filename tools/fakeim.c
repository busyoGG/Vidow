/* 假输入法：只为验证合成器的 input-method-v2 中继。
 *
 * 它不像 fcitx5 那样真的转换文字，而是：
 *   - 按输入法协议连上合成器，等 activate，抢键盘
 *   - 每收到一个按键，就 commit 一段 "<键码>" 文字回去
 * 于是「按键 -> 合成器 -> 输入法 -> 合成器 -> 客户端」这条链有没有接好，
 * 看客户端里出现的字符就知道了。
 *
 * 用法：WAYLAND_DISPLAY=... ./build/fakeim
 * 事件日志走 stdout。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>

#include "input-method-unstable-v2-client-protocol.h"

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_seat *seat;
static struct zwp_input_method_manager_v2 *im_manager;
static struct zwp_input_method_v2 *im;
static struct zwp_input_method_keyboard_grab_v2 *grab;
static uint32_t done_count;
static int committed;

static void im_activate(void *data, struct zwp_input_method_v2 *method) {
	(void)data;
	(void)method;
	printf("activate\n");
	fflush(stdout);
}

static void im_deactivate(void *data, struct zwp_input_method_v2 *method) {
	(void)data;
	(void)method;
	printf("deactivate\n");
	fflush(stdout);
}

static void im_surrounding_text(void *data, struct zwp_input_method_v2 *method,
		const char *text, uint32_t cursor, uint32_t anchor) {
	(void)data;
	(void)method;
	printf("surrounding_text [%s] cursor=%u anchor=%u\n", text, cursor, anchor);
	fflush(stdout);
}

static void im_text_change_cause(void *data, struct zwp_input_method_v2 *method,
		uint32_t cause) {
	(void)data;
	(void)method;
	(void)cause;
}

static void im_content_type(void *data, struct zwp_input_method_v2 *method,
		uint32_t hint, uint32_t purpose) {
	(void)data;
	(void)method;
	printf("content_type hint=%u purpose=%u\n", hint, purpose);
	fflush(stdout);
}

static void im_done(void *data, struct zwp_input_method_v2 *method) {
	(void)data;
	(void)method;
	/* 这版协议 done 不带 serial：serial 就是「已经发过多少个 done」 */
	done_count++;
	printf("done #%u\n", done_count);
	fflush(stdout);
}

static void im_unavailable(void *data, struct zwp_input_method_v2 *method) {
	(void)data;
	(void)method;
	printf("unavailable\n");
	fflush(stdout);
}

static const struct zwp_input_method_v2_listener im_listener = {
	.activate = im_activate,
	.deactivate = im_deactivate,
	.surrounding_text = im_surrounding_text,
	.text_change_cause = im_text_change_cause,
	.content_type = im_content_type,
	.done = im_done,
	.unavailable = im_unavailable,
};

static void grab_keymap(void *data,
		struct zwp_input_method_keyboard_grab_v2 *keyboard_grab,
		uint32_t format, int32_t fd, uint32_t size) {
	(void)data;
	(void)keyboard_grab;
	printf("grab: keymap format=%u size=%u\n", format, size);
	fflush(stdout);
	close(fd);
}

static void grab_key(void *data,
		struct zwp_input_method_keyboard_grab_v2 *keyboard_grab,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	(void)data;
	(void)keyboard_grab;
	(void)serial;
	(void)time;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		return;
	}
	char text[32];
	snprintf(text, sizeof(text), "<%u>", key);
	zwp_input_method_v2_commit_string(im, text);
	zwp_input_method_v2_commit(im, done_count);
	committed++;
	printf("key %u -> commit \"%s\" (%d)\n", key, text, committed);
	fflush(stdout);
}

static void grab_modifiers(void *data,
		struct zwp_input_method_keyboard_grab_v2 *keyboard_grab,
		uint32_t serial, uint32_t depressed, uint32_t latched,
		uint32_t locked, uint32_t group) {
	(void)data;
	(void)keyboard_grab;
	(void)serial;
	(void)depressed;
	(void)latched;
	(void)locked;
	(void)group;
}

static void grab_repeat_info(void *data,
		struct zwp_input_method_keyboard_grab_v2 *keyboard_grab,
		int32_t rate, int32_t delay) {
	(void)data;
	(void)keyboard_grab;
	(void)rate;
	(void)delay;
}

static const struct zwp_input_method_keyboard_grab_v2_listener grab_listener = {
	.keymap = grab_keymap,
	.key = grab_key,
	.modifiers = grab_modifiers,
	.repeat_info = grab_repeat_info,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
		const char *interface, uint32_t version) {
	(void)data;
	(void)version;
	if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
	} else if (strcmp(interface,
			zwp_input_method_manager_v2_interface.name) == 0) {
		im_manager = wl_registry_bind(reg, name,
			&zwp_input_method_manager_v2_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *reg,
		uint32_t name) {
	(void)data;
	(void)reg;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int main(void) {
	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "fakeim: cannot connect to the compositor\n");
		return 1;
	}
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (seat == NULL || im_manager == NULL) {
		fprintf(stderr, "fakeim: no seat / input-method-manager-v2\n");
		return 1;
	}

	im = zwp_input_method_manager_v2_get_input_method(im_manager, seat);
	zwp_input_method_v2_add_listener(im, &im_listener, NULL);

	grab = zwp_input_method_v2_grab_keyboard(im);
	zwp_input_method_keyboard_grab_v2_add_listener(grab, &grab_listener, NULL);
	printf("fakeim ready\n");
	fflush(stdout);

	while (wl_display_dispatch(display) != -1) {
		/* 事件循环 */
	}
	return 0;
}
