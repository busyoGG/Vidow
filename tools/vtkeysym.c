/* 用一块**虚拟键盘**（zwp_virtual_keyboard_v1）造出真键盘的 Ctrl+Alt+F2。
 *
 * 为什么不能直接 vidowmsg send-key：注入那条路按设计不跑键位表（按键只给
 * 客户端），所以量不到内置的 Ctrl+Alt+Fn 逃生口。为什么不能直接用 wtype：
 * wtype 自己编译的 keymap 里没有 srvr_ctrl(fkey2vt)，Ctrl+Alt+F2 解出来是
 * F2 —— 恰好绕开了真正的线上路径。
 *
 * 为什么必须自己发 modifiers 事件：wlroots 的 virtual_keyboard_key() 里
 * `update_state = false`（wlroots 0.20 types/wlr_virtual_keyboard_v1.c:92），
 * 它**不会**按按下的键自己更新修饰位，客户端得走
 * zwp_virtual_keyboard_v1.modifiers 显式报。少这一步，Ctrl/Alt 按了也等于
 * 没按，F2 永远停在第一档（模出来的就是 F2，不是 XF86Switch_VT_2）。
 *
 * 用法：vtkeysym vt2    按 Ctrl+Alt+XF86Switch_VT_2（应该切 tty 2）
 *       vtkeysym plain  按 Ctrl+Alt+a（不应该切 tty）
 * 退出码：0 = 事件发出去了；1 = 握手失败。 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"

#define KC_CTRL 29
#define KC_ALT  56
#define KC_XF86VT2 60 /* 键盘上的 F2 */
#define KC_A    30

/* 用 libxkbcommon 按系统规则编一份 keymap —— 和真键盘看到的是同一份
 * （vidow 的默认 seat keymap 也是 xkb_keymap_new_from_names(NULL) 出来的）。
 *
 * 它带 srvr_ctrl(fkey2vt)：
 *     key <FK02> { type="CTRL+ALT";
 *                  symbols[1]=[ F2, F2, F2, F2, XF86Switch_VT_2 ] };
 * 也就是 Ctrl+Alt+F2 解出来是 XF86Switch_VT_2（0x1008fe02），不是 F2。
 *
 * 不用 wtype 是因为 wtype 自己那份 keymap 里没有这一档，解出来是 F2 ——
 * 恰好绕开要测的那条路。 */
static uint32_t ctrl_mask;
static uint32_t alt_mask;

static char *build_keymap(size_t *len) {
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (ctx == NULL) {
		return NULL;
	}
	struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	char *str = NULL;
	if (km != NULL) {
		/* 修饰位掩码从这份 keymap 里查，别写死：Control/Mod1 的位序是
		 * keymap 自己的，硬编码只能在本机侥幸对上。 */
		xkb_mod_index_t ctrl = xkb_keymap_mod_get_index(km, XKB_MOD_NAME_CTRL);
		xkb_mod_index_t alt = xkb_keymap_mod_get_index(km, XKB_MOD_NAME_ALT);
		if (ctrl != XKB_MOD_INVALID) {
			ctrl_mask = 1u << ctrl;
		}
		if (alt != XKB_MOD_INVALID) {
			alt_mask = 1u << alt;
		}
		str = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
		if (str != NULL) {
			*len = strlen(str) + 1; /* 协议要含结尾 NUL 的长度 */
		}
		xkb_keymap_unref(km);
	}
	xkb_context_unref(ctx);
	return str;
}

static struct wl_display *display;
static struct wl_seat *seat;
static struct zwp_virtual_keyboard_manager_v1 *vk_mgr;
static struct zwp_virtual_keyboard_v1 *vk;

static void seat_caps(void *data, struct wl_seat *s, uint32_t caps) {
	(void)data;
	(void)s;
	(void)caps;
}
static void seat_name(void *data, struct wl_seat *s, const char *name) {
	(void)data;
	(void)s;
	(void)name;
}
static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_caps,
	.name = seat_name,
};

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
		const char *iface, uint32_t version) {
	(void)data;
	(void)version;
	if (strcmp(iface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (strcmp(iface,
			zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
		vk_mgr = wl_registry_bind(reg, name,
			&zwp_virtual_keyboard_manager_v1_interface, 1);
	}
}
static void reg_remove(void *data, struct wl_registry *reg, uint32_t name) {
	(void)data;
	(void)reg;
	(void)name;
}
static const struct wl_registry_listener registry_listener = {
	.global = reg_global,
	.global_remove = reg_remove,
};

static void send_key(uint32_t keycode, bool pressed) {
	zwp_virtual_keyboard_v1_key(vk, 0, keycode,
		pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
			: WL_KEYBOARD_KEY_STATE_RELEASED);
	wl_display_flush(display);
}

/* 显式报修饰位（wlroots 不会从按键自己推）。depressed 就是当前按着的修饰
 * 键掩码；latched / locked / group 这里都用 0。 */
static void send_mods(uint32_t depressed) {
	zwp_virtual_keyboard_v1_modifiers(vk, depressed, 0, 0, 0);
	wl_display_flush(display);
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "用法：vtkeysym vt2|plain\n");
		return 1;
	}
	bool vt2 = strcmp(argv[1], "vt2") == 0;

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "vtkeysym: 连不上 compositor\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0 ||
			seat == NULL || vk_mgr == NULL) {
		fprintf(stderr, "vtkeysym: 缺 wl_seat / "
			"zwp_virtual_keyboard_manager_v1\n");
		return 1;
	}

	size_t len = 0;
	char *km_text = build_keymap(&len);
	if (km_text == NULL) {
		fprintf(stderr, "vtkeysym: 编不出 keymap（libxkbcommon）\n");
		return 1;
	}
	/* 这份 keymap 里 <FK02> 的第 5 档必须是 XF86Switch_VT_2；get_as_string
	 * 把它写成 0x1008fe02，别按名字找（会误判成「没有」）。 */
	if (strstr(km_text, "0x1008fe02") == NULL) {
		fprintf(stderr, "vtkeysym: 这份 keymap 里没有 XF86Switch_VT_2，"
			"测不到真实路径\n");
	}
	int fd = memfd_create("vtkeysym-keymap", MFD_CLOEXEC);
	if (fd < 0) {
		perror("memfd_create");
		return 1;
	}
	if (ftruncate(fd, (off_t)len) != 0) {
		perror("ftruncate");
		return 1;
	}
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memcpy(p, km_text, len);
	munmap(p, len);

	vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vk_mgr,
		seat);
	zwp_virtual_keyboard_v1_keymap(vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
		fd, (uint32_t)len);
	close(fd);
	free(km_text);
	if (wl_display_roundtrip(display) < 0) {
		fprintf(stderr, "vtkeysym: 送 keymap 之后连接断了\n");
		return 1;
	}

	/* Ctrl+Alt+<键>：和真人按键同一个顺序，每条修饰键后面补一条
	 * modifiers（见文件头：虚拟键盘的按键不带 update_state）。 */
	uint32_t target = vt2 ? KC_XF86VT2 : KC_A;
	send_key(KC_CTRL, true);
	send_mods(ctrl_mask);
	send_key(KC_ALT, true);
	send_mods(ctrl_mask | alt_mask);
	send_key(target, true);
	send_key(target, false);
	send_key(KC_ALT, false);
	send_mods(ctrl_mask);
	send_key(KC_CTRL, false);
	send_mods(0);
	wl_display_flush(display);

	/* 让 compositor 有时间派发这几条事件 */
	struct timespec ts = { 0, 300 * 1000 * 1000L };
	nanosleep(&ts, NULL);
	zwp_virtual_keyboard_v1_destroy(vk);
	wl_display_disconnect(display);
	return 0;
}
