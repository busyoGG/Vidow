/*
 * vdrag - 用虚拟指针模拟一次鼠标拖拽，用来验证合成器的交互式移动/缩放。
 *
 * 用法: vdrag <x1> <y1> <x2> <y2> [--right]
 *   (x1,y1) 按下左键的位置，(x2,y2) 拖到的位置，绝对坐标（layout 坐标）。
 *
 * 需要合成器实现 zwlr_virtual_pointer_manager_v1。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <wayland-client.h>

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct zwlr_virtual_pointer_manager_v1 *manager;
static struct wl_seat *seat;

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	(void)data;
	(void)version;
	if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	} else if (strcmp(interface,
			zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
		manager = wl_registry_bind(registry, name,
			&zwlr_virtual_pointer_manager_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void sleep_ms(int ms) {
	struct timespec ts = { .tv_sec = ms / 1000,
		.tv_nsec = (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int main(int argc, char *argv[]) {
	if (argc < 5) {
		fprintf(stderr, "用法: %s <x1> <y1> <x2> <y2> [--right]\n", argv[0]);
		return 2;
	}
	int x1 = atoi(argv[1]), y1 = atoi(argv[2]);
	int x2 = atoi(argv[3]), y2 = atoi(argv[4]);
	uint32_t button = (argc > 5 && strcmp(argv[5], "--right") == 0) ?
		BTN_RIGHT : BTN_LEFT;

	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "无法连接 Wayland 显示\n");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (manager == NULL || seat == NULL) {
		fprintf(stderr, "合成器不支持 zwlr_virtual_pointer_manager_v1\n");
		return 1;
	}

	struct zwlr_virtual_pointer_v1 *pointer =
		zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, seat);

	const uint32_t extent = 10000; /* 绝对坐标取值范围 */

	zwlr_virtual_pointer_v1_motion_absolute(pointer, now_ms(),
		(uint32_t)x1, (uint32_t)y1, extent, extent);
	zwlr_virtual_pointer_v1_frame(pointer);
	wl_display_roundtrip(display);
	sleep_ms(120);

	zwlr_virtual_pointer_v1_button(pointer, now_ms(), button,
		WL_POINTER_BUTTON_STATE_PRESSED);
	zwlr_virtual_pointer_v1_frame(pointer);
	wl_display_roundtrip(display);
	sleep_ms(120);

	const int steps = 12;
	for (int i = 1; i <= steps; i++) {
		int x = x1 + (x2 - x1) * i / steps;
		int y = y1 + (y2 - y1) * i / steps;
		zwlr_virtual_pointer_v1_motion_absolute(pointer, now_ms(),
			(uint32_t)x, (uint32_t)y, extent, extent);
		zwlr_virtual_pointer_v1_frame(pointer);
		wl_display_roundtrip(display);
		sleep_ms(40);
	}

	zwlr_virtual_pointer_v1_button(pointer, now_ms(), button,
		WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(pointer);
	wl_display_roundtrip(display);

	zwlr_virtual_pointer_v1_destroy(pointer);
	wl_display_roundtrip(display);
	wl_display_disconnect(display);
	return 0;
}
