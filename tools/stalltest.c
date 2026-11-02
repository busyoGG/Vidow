/* 发送缓冲区回归测试：**客户端主线程睡满 STALL_MS**，合成器这段时间照样
 * 按 144Hz 灌指针事件 —— 连接不能被踢。
 *
 * 为什么要有这条：libwayland 每个客户端那条发送缓冲区默认只有 4096 字节
 * （wayland-private.h 的 WL_BUFFER_DEFAULT_MAX_SIZE）。满了以后下一条事件
 * 放不进去，wl_connection_queue() 返回 -1(E2BIG)，libwayland 只打一行
 * "Data too big for buffer (4088 + 20 > 4096)." 就把这个客户端的连接
 * **整条销毁** —— 客户端看到的是 ECONNRESET（"连接被对方重置"）。
 * 这就是 Element 的死法（2026-11-02 22:41:15 / 21:34:12，都是 exit 0、
 * 没有 core、还写了 window-state.json，日志最后一行是
 * "Fatal Wayland communication error: 连接被对方重置"）。
 * Hyprland / niri 都把默认值抬到 1MB（见 main.c 里 wl_display_create 后面那段）。
 *
 * 本测试做的事（一次性跑完，顺序很关键）：
 *   1. 连上 vidow，拿 compositor / shm / seat / pointer，建一个 400x300 的
 *      xdg 窗口并贴上屏幕（先等一帧，保证光标在窗口上，指针焦点是它）；
 *   2. **一次** dispatch 之后整个主线程睡 STALL_MS 毫秒，期间一个字节都不读；
 *      光标的 motion 由 vidow 自己按刷新率发（用户挪鼠标 / 144Hz 面板），
 *      每条 21 字节（20 + 1）—— 1 秒能堆到十几 KB；
 *   3. 醒来跑一次 roundtrip（发真实请求 + 等回包）。连接被踢就返回 -1
 *      （errno EPIPE），退出码 1。
 *
 * 用法：stalltest [--stall 毫秒]（默认 2000）
 * 退出码：0 = 连接健在；1 = 被踢 / 握手失败。
 *
 * 注意：连上之后先 roundtrip 再建窗口 —— 没 roundtrip 之前 registry 里
 * 的 global 只是本进程缓冲里的，xdg_wm_base 之类还没真发出去。 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

#define WIN_W 400
#define WIN_H 300

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct xdg_wm_base *wm_base;
static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
static struct wl_buffer *buffer;
static int motions;
static bool configured;
static unsigned ping_serial;

/* 睡的时候屏幕还在动：主线程一睡，vidow 收到的 motion 全堆在它那条
 * 发送缓冲区里。 */
#define STALL_MS 2000

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int ms) {
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
	}
}

/* 有事件就收，没有就等到 ms 用完 —— **绝不阻塞在 dispatch 上**。
 * dispatch 是阻塞读，窗口没新事件时它会一直等（测试就挂死了）。 */
static void pump(int ms) {
	int fd = wl_display_get_fd(display);
	int64_t end = now_ms() + ms;
	while (now_ms() < end) {
		wl_display_flush(display);
		struct pollfd p = { .fd = fd, .events = POLLIN };
		int r = poll(&p, 1, (int)(end - now_ms()));
		if (r <= 0) {
			return;
		}
		wl_display_dispatch(display);
	}
}

/* ------------------------------------------------------------ shm 缓冲 */

static void buffer_release(void *data, struct wl_buffer *b) {
	(void)data;
	wl_buffer_destroy(b);
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static struct wl_buffer *make_buffer(void) {
	int stride = WIN_W * 4;
	size_t size = (size_t)stride * WIN_H;
	int fd = memfd_create("stalltest", MFD_CLOEXEC);
	if (fd < 0) {
		perror("memfd_create");
		return NULL;
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		perror("ftruncate");
		close(fd);
		return NULL;
	}
	uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
		fd, 0);
	if (pixels == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return NULL;
	}
	for (size_t i = 0; i < size / 4; i++) {
		pixels[i] = 0xff303040u;
	}
	munmap(pixels, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, WIN_W, WIN_H,
		stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(buf, &buffer_listener, NULL);
	return buf;
}

/* ------------------------------------------------------------ xdg-shell */

static void wm_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	(void)data;
	ping_serial = serial;
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_listener = {
	.ping = wm_ping,
};

static void xs_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
	(void)data;
	xdg_surface_ack_configure(xs, serial);
	configured = true;
}

static const struct xdg_surface_listener xs_listener = {
	.configure = xs_configure,
};

static void tl_configure(void *data, struct xdg_toplevel *tl, int32_t w,
		int32_t h, struct wl_array *states) {
	(void)data;
	(void)tl;
	(void)w;
	(void)h;
	(void)states;
}

static void tl_close(void *data, struct xdg_toplevel *tl) {
	(void)data;
	(void)tl;
}

static void tl_bounds(void *data, struct xdg_toplevel *tl, int32_t w,
		int32_t h) {
	(void)data;
	(void)tl;
	(void)w;
	(void)h;
}

static void tl_wm_capabilities(void *data, struct xdg_toplevel *tl,
		struct wl_array *caps) {
	(void)data;
	(void)tl;
	(void)caps;
}

static const struct xdg_toplevel_listener tl_listener = {
	.configure = tl_configure,
	.close = tl_close,
	.configure_bounds = tl_bounds,
	.wm_capabilities = tl_wm_capabilities,
};

/* ------------------------------------------------------------ seat */

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
	(void)data;
	(void)p;
	(void)serial;
	(void)s;
	(void)x;
	(void)y;
}

static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *s) {
	(void)data;
	(void)p;
	(void)serial;
	(void)s;
}

static void ptr_motion(void *data, struct wl_pointer *p, uint32_t t,
		wl_fixed_t x, wl_fixed_t y) {
	(void)data;
	(void)p;
	(void)t;
	(void)x;
	(void)y;
	motions++;
}

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
		uint32_t t, uint32_t b, uint32_t state) {
	(void)data;
	(void)p;
	(void)serial;
	(void)t;
	(void)b;
	(void)state;
}

static void ptr_axis(void *data, struct wl_pointer *p, uint32_t t,
		uint32_t axis, wl_fixed_t value) {
	(void)data;
	(void)p;
	(void)t;
	(void)axis;
	(void)value;
}

static void ptr_frame(void *data, struct wl_pointer *p) {
	(void)data;
	(void)p;
}

static void ptr_axis_source(void *data, struct wl_pointer *p, uint32_t s) {
	(void)data;
	(void)p;
	(void)s;
}

static void ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t t,
		uint32_t axis) {
	(void)data;
	(void)p;
	(void)t;
	(void)axis;
}

static void ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis,
		int32_t discrete) {
	(void)data;
	(void)p;
	(void)axis;
	(void)discrete;
}

static void ptr_axis_value120(void *data, struct wl_pointer *p, uint32_t axis,
		int32_t value120) {
	(void)data;
	(void)p;
	(void)axis;
	(void)value120;
}

static void ptr_axis_relative_direction(void *data, struct wl_pointer *p,
		uint32_t axis, uint32_t dir) {
	(void)data;
	(void)p;
	(void)axis;
	(void)dir;
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = ptr_enter,
	.leave = ptr_leave,
	.motion = ptr_motion,
	.button = ptr_button,
	.axis = ptr_axis,
	.frame = ptr_frame,
	.axis_source = ptr_axis_source,
	.axis_stop = ptr_axis_stop,
	.axis_discrete = ptr_axis_discrete,
	.axis_value120 = ptr_axis_value120,
	.axis_relative_direction = ptr_axis_relative_direction,
};

static void seat_capabilities(void *data, struct wl_seat *s,
		uint32_t caps) {
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && pointer == NULL) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
}

static void seat_name(void *data, struct wl_seat *s, const char *name) {
	(void)data;
	(void)s;
	(void)name;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ------------------------------------------------------------ registry */

static void global_add(void *data, struct wl_registry *reg, uint32_t name,
		const char *iface, uint32_t version) {
	(void)data;
	(void)version;
	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	} else if (strcmp(iface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (strcmp(iface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(reg, name, &wl_seat_interface, 5);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	}
}

static void global_remove(void *data, struct wl_registry *reg, uint32_t name) {
	(void)data;
	(void)reg;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = global_add,
	.global_remove = global_remove,
};

/* 唤醒之后必须真有一个请求在飞：只靠 roundtrip 的话，libwayland 用的是
 * 同步请求（wl_display_sync），服务器未必会复制进发送队列。set_title 是
 * 真事件，一定走 wl_resource_post_event → 写进客户端的发送缓冲区。
 *
 * 全程不碰窗口位置：挪窗口会让光标出窗、vidow 可能停掉为它出的帧，反而
 * 少了事件。 */
int main(int argc, char **argv) {
	int stall_ms = STALL_MS;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--stall") == 0 && i + 1 < argc) {
			stall_ms = atoi(argv[++i]);
		}
	}

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "stalltest: 连不上 compositor（WAYLAND_DISPLAY?）\n");
		return 1;
	}
	/* 调试/测试用：把自己的接收缓冲压小，好让「合成器发送缓冲区满」
	 * 在几 KB 内就能复现（默认要看内核攒到 ~1MB 才满）。0 = 不动。 */
	const char *rwnd = getenv("STALLTEST_RCVBUF");
	if (rwnd != NULL && atoi(rwnd) > 0) {
		int v = atoi(rwnd);
		if (setsockopt(wl_display_get_fd(display), SOL_SOCKET, SO_RCVBUF,
				&v, sizeof v) != 0) {
			perror("setsockopt SO_RCVBUF");
		}
	}
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	/* 第一次 roundtrip 才真拿到所有 global（之前只是本进程缓冲里的） */
	if (wl_display_roundtrip(display) < 0) {
		fprintf(stderr, "stalltest: roundtrip 失败\n");
		return 1;
	}
	if (compositor == NULL || shm == NULL || wm_base == NULL || seat == NULL) {
		fprintf(stderr, "stalltest: 缺 compositor/shm/xdg_wm_base/seat\n");
		return 1;
	}
	/* 第二个 roundtrip：让 wl_seat.capabilities（以及 pointer 的创建）落地 */
	if (wl_display_roundtrip(display) < 0) {
		fprintf(stderr, "stalltest: roundtrip 2 失败\n");
		return 1;
	}

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xs_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &tl_listener, NULL);
	xdg_toplevel_set_title(toplevel, "stalltest");
	xdg_toplevel_set_app_id(toplevel, "stalltest");
	wl_surface_commit(surface);

	/* 等 configure，再挂上 shm 缓冲（让窗口真的上屏幕，才有指针事件） */
	for (int i = 0; i < 200 && !configured; i++) {
		pump(10);
	}
	if (!configured) {
		fprintf(stderr, "stalltest: 等不到 configure（窗口没被合成器确认）\n");
		return 1;
	}
	buffer = make_buffer();
	if (buffer == NULL) {
		return 1;
	}
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, WIN_W, WIN_H);
	wl_surface_commit(surface);
	pump(300);

	/* ---- 关键那一下：flush（把请求送出去）→ 睡 → 读 ---- */
	pump(100);
	wl_display_flush(display);
	sleep_ms(stall_ms);

	/* 醒来：一个真请求 + roundtrip。被断连的话这里返回 -1。 */
	ping_serial = 0;
	xdg_toplevel_set_title(toplevel, "stalltest");
	int rc = wl_display_roundtrip(display);
	if (rc < 0) {
		fprintf(stderr, "stalltest: 睡 %d ms 后连接断了（errno=%d %s）"
			"—— 客户端的发送缓冲区被撑爆，libwayland 把连接扔了\n",
			stall_ms, errno, strerror(errno));
		return 1;
	}
	printf("stalltest ok：主线程睡 %d ms，期间连接没被踢，roundtrip 正常"
		"（收到 %d 条 motion，ping serial=%u）\n", stall_ms, motions,
		ping_serial);

	if (pointer != NULL) {
		wl_pointer_destroy(pointer);
	}
	xdg_toplevel_destroy(toplevel);
	xdg_surface_destroy(xdg_surface);
	wl_surface_destroy(surface);
	return 0;
}
