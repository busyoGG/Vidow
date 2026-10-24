/*
 * vdrag - 用虚拟指针模拟一次鼠标拖拽，用来验证合成器的交互式移动/缩放。
 *
 * 用法: vdrag <x1> <y1> <x2> <y2> [--right] [--hold|--move-only] [--wait <毫秒>]
 *   (x1,y1) 按下左键的位置，(x2,y2) 拖到的位置，绝对坐标（layout 坐标）。
 *
 *   默认：按下一路拖到 (x2,y2) 再松手（一次完整的拖拽）。
 *   --hold      走到 (x2,y2) 后**不松手**，停住 --wait 毫秒（默认 1500）再松。
 *               用来在**拖拽中途**截图取证（跨屏拖拽那几条 bug 只在按住的
 *               时候出现，松手之后窗口已经换了归属屏，什么都看不出来）。
 *   --move-only 只把指针移到 (x2,y2)，不按下、不拖动（续上一次 --hold 的拖拽）。
 *   --steps <n>  拖拽分几步走（默认 12；录屏调大，动作才够慢、帧才够多）。
 *   --frame-cmd <命令>  每走一步都执行一次（录屏用：命令里自己截一帧）。
 *   --mid-cmd <命令> / --mid-at <步>  走到第几步时执行一次命令。
 *
 * 需要合成器实现 zwlr_virtual_pointer_manager_v1。
 */
#include <stdbool.h>
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

/* 绝对坐标：协议的语义是「(x, y) 在 0..x_extent × 0..y_extent 这块范围里
 * 的比例」，wlroots 收到按 (x/extent, y/extent) 归一化再映射到输出布局。
 * 口径统一走这里：调用方给的 0..10000 万分数直接当 extent 用。 */
static void motion_abs(struct zwlr_virtual_pointer_v1 *pointer, uint32_t time,
		int vx, int vy, uint32_t extent) {
	zwlr_virtual_pointer_v1_motion_absolute(pointer, time,
		(uint32_t)vx, (uint32_t)vy, extent, extent);
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
	uint32_t button = BTN_LEFT;
	bool hold = false, move_only = false;
	int hold_ms = 1500;
	int step_ms = 40;
	int steps = 12;
	const char *mid_cmd = NULL;
	const char *frame_cmd = NULL;
	int mid_at = -1;
	/* 逐帧 / 中途命令失败过就记下来，最后统一报非 0（不能当场 return：
	 * 那样按键会停在按下状态，合成器以为拖拽还活着）。 */
	bool failed = false;
	for (int i = 5; i < argc; i++) {
		if (strcmp(argv[i], "--right") == 0) {
			button = BTN_RIGHT;
		} else if (strcmp(argv[i], "--hold") == 0) {
			hold = true;
		} else if (strcmp(argv[i], "--move-only") == 0) {
			move_only = true;
		} else if (strcmp(argv[i], "--wait") == 0 && i + 1 < argc) {
			hold_ms = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--step-ms") == 0 && i + 1 < argc) {
			step_ms = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--steps") == 0 && i + 1 < argc) {
			steps = atoi(argv[++i]);
			if (steps < 1) {
				steps = 1;
			}
		} else if (strcmp(argv[i], "--frame-cmd") == 0 && i + 1 < argc) {
			frame_cmd = argv[++i];
		} else if (strcmp(argv[i], "--mid-cmd") == 0 && i + 1 < argc) {
			mid_cmd = argv[++i];
		} else if (strcmp(argv[i], "--mid-at") == 0 && i + 1 < argc) {
			mid_at = atoi(argv[++i]);
		}
	}

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

	/* 调用方给的是 0..10000 的万分数（好读、好算），协议按同一 extent 归一化。 */
	const uint32_t extent = 10000;

	motion_abs(pointer, now_ms(), x1, y1, extent);
	zwlr_virtual_pointer_v1_frame(pointer);
	wl_display_roundtrip(display);
	sleep_ms(120);

	if (move_only) {
		/* 续拖：指针照走，不碰按键 —— 上一次 --hold 按下之后还没人松手。
		 * 走多步和真拖同一节奏（一次跳过去的 motion 事件太少，
		 * 合成器那边「一帧一个位置」的落后一拍看不出来）。 */
		for (int i = 1; i <= 12; i++) {
			int x = x1 + (x2 - x1) * i / 12;
			int y = y1 + (y2 - y1) * i / 12;
			motion_abs(pointer, now_ms(), x, y, extent);
			zwlr_virtual_pointer_v1_frame(pointer);
			wl_display_roundtrip(display);
			sleep_ms(step_ms);
		}
		printf("held %d %d\n", x2, y2);
		fflush(stdout);
		sleep_ms(hold_ms);
		zwlr_virtual_pointer_v1_destroy(pointer);
		wl_display_roundtrip(display);
		wl_display_disconnect(display);
		return 0;
	}

	zwlr_virtual_pointer_v1_button(pointer, now_ms(), button,
		WL_POINTER_BUTTON_STATE_PRESSED);
	zwlr_virtual_pointer_v1_frame(pointer);
	wl_display_roundtrip(display);
	sleep_ms(120);

	for (int i = 1; i <= steps; i++) {
		int x = x1 + (x2 - x1) * i / steps;
		int y = y1 + (y2 - y1) * i / steps;
		motion_abs(pointer, now_ms(), x, y, extent);
		zwlr_virtual_pointer_v1_frame(pointer);
		wl_display_roundtrip(display);
		sleep_ms(step_ms);
		/* --mid-at <步>: 走到这一步时执行 --mid-cmd（测试用来在**同一个指针
		 * 会话里**停住截图 / 再做一件事）。用外部命令而不是 sleep 猜时机：
		 * 调用方要的是「窗口已经画在新屏上了」那一刻的像素。 */
		/* --frame-cmd：每一步都跑一次（录制用，逐帧截图）。 */
		if (frame_cmd != NULL) {
			int rc = system(frame_cmd);
			if (rc != 0) {
				fprintf(stderr, "vdrag: frame-cmd 失败（退出码 %d）\n", rc);
				failed = true;
			}
		}
		if (mid_cmd != NULL && i == mid_at) {
			int rc = system(mid_cmd);
			if (rc != 0) {
				fprintf(stderr, "vdrag: mid-cmd 退出码 %d\n", rc);
				failed = true;
			}
		}
	}

	/* --hold：先在 stdout 报一声「指针已经停在终点、按键还按着」，调用方
	 * （测试脚本）看到这行再去截图 —— 靠 sleep 猜时机在慢的 CI 上必然翻车。
	 * 输出要**立刻**可见：不 flush 的话它被缓冲在管道里，等于没报。 */
	if (hold) {
		printf("held %d %d\n", x2, y2);
		fflush(stdout);
	}
	sleep_ms(hold ? hold_ms : 0);

	zwlr_virtual_pointer_v1_button(pointer, now_ms(), button,
		WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(pointer);
	wl_display_roundtrip(display);

	zwlr_virtual_pointer_v1_destroy(pointer);
	wl_display_roundtrip(display);
	wl_display_disconnect(display);
	/* 拖拽本身走完了，但**逐帧命令失败过**就得报非 0：不然录制脚本照旧
	 * 收到 0，把「某一帧没截到」当成「录完整了」—— 缺帧、覆盖帧全变成
	 * 静默通过。松手照常做，别把合成器的按下状态留在那儿。 */
	if (failed) {
		fprintf(stderr, "vdrag: 有逐帧命令失败，拖拽完成但录制不完整\n");
		return 1;
	}
	return 0;
}
