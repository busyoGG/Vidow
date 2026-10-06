/* 声明一个**比表面还大**的窗口几何的客户端（xdg_surface.set_window_geometry）。
 *
 * 起因是用户报的「窗口先渲染小一点然后才按照缩放显示」：chromium 系在改大小的那一
 * 两秒里，表面已经落位、声明的窗口几何却还是新尺寸（比表面大）。vidow 以前直接
 * 拿这份声明当窗口大小，采内容时的 UV 就跑到 1 外面被 clamp —— 屏幕上「窗口框
 * 到位了、里面的画面小一圈、边上一条糊边」。规范上这份几何必须落在表面里，
 * 所以合成器该当它没声明（窗口 = 整块表面，niri 也是这个规矩，见 view.c 的
 * vidow_view_geometry）。
 *
 * 用法：geowin <标题> <缓冲宽> <缓冲高> <声明宽> <声明高> [--fixed] [--linger=秒]
 *   --fixed   带上 min == max 的尺寸提示（浮动窗口那条路，方便断言 float_geometry）
 *
 * 输出（一行一条）：
 *   geowin mapped <缓冲宽>x<缓冲高> geometry <声明宽>x<声明高>
 *   geowin exit <毫秒>
 *
 * 缓冲内容：左红中绿右蓝三条竖带，正中间一条白线 —— 白线的位置能看出内容有没有
 * 被拉伸（声明框大于表面时，旧行为会把内容挤到左上角、右边一条糊边）。 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
static struct wl_buffer *buffer;
static int buf_w = 200, buf_h = 150;
static int geo_w = 300, geo_h = 250;
static int linger_ms = 5000;
static int shrink_w, shrink_h;
static bool fixed_size;
static bool mapped;
static int64_t start_ms;

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	(void)data;
	xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_listener = { .ping = ping };

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w,
		int32_t h, struct wl_array *states) {
	(void)data; (void)t; (void)w; (void)h; (void)states;
}
static void toplevel_close(void *data, struct xdg_toplevel *t) {
	(void)data; (void)t;
	exit(0);
}
static void toplevel_configure_bounds(void *data, struct xdg_toplevel *t,
		int32_t w, int32_t h) {
	(void)data; (void)t; (void)w; (void)h;
}
static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *t,
		struct wl_array *caps) {
	(void)data; (void)t; (void)caps;
}
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_configure_bounds,
	.wm_capabilities = toplevel_wm_capabilities,
};

static void xdg_surface_configure(void *data, struct xdg_surface *s,
		uint32_t serial) {
	(void)data;
	fprintf(stderr, "geowin configure serial=%u mapped=%d\n", serial, mapped);
	xdg_surface_ack_configure(s, serial);
}
static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void buffer_release(void *data, struct wl_buffer *b) {
	(void)data; (void)b;
}
static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static struct wl_buffer *make_buffer(int w, int h) {
	int stride = w * 4;
	int size = stride * h;
	int fd = -1;
	/* memfd_create 要 _GNU_SOURCE */
	fd = memfd_create("geowin", 0);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		perror("memfd");
		exit(1);
	}
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	uint32_t *px = data;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			uint32_t color;
			if (x == w / 2) {
				color = 0xffffffff;
			} else if (x < w / 3) {
				color = 0xffff0000;
			} else if (x < 2 * w / 3) {
				color = 0xff00ff00;
			} else {
				color = 0xff0000ff;
			}
			px[y * w + x] = color;
		}
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(buf, &buffer_listener, NULL);
	return buf;
}

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
		const char *iface, uint32_t version) {
	(void)data;
	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
			version < 4 ? version : 4);
	} else if (strcmp(iface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface,
			version < 6 ? version : 6);
		xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	}
}
static void registry_remove(void *data, struct wl_registry *reg, uint32_t name) {
	(void)data; (void)reg; (void)name;
}
static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

int main(int argc, char **argv) {
	const char *title = "geowin";
	int argi = 1;
	if (argc > argi && argv[argi][0] != '-') {
		title = argv[argi++];
	}
	if (argc > argi + 3) {
		buf_w = atoi(argv[argi++]);
		buf_h = atoi(argv[argi++]);
		geo_w = atoi(argv[argi++]);
		geo_h = atoi(argv[argi++]);
	}
	for (; argi < argc; argi++) {
		if (strcmp(argv[argi], "--fixed") == 0) {
			fixed_size = true;
		} else if (strncmp(argv[argi], "--linger=", 9) == 0) {
			linger_ms = atoi(argv[argi] + 9) * 1000;
		} else if (strncmp(argv[argi], "--shrink=", 9) == 0) {
			/* 上屏之后把**表面**换成更小的一块缓冲，但**不动**声明的窗口
			 * 几何 —— 这就是「几何比表面大」那个中间态（chromium 改大小）。 */
			sscanf(argv[argi] + 9, "%dx%d", &shrink_w, &shrink_h);
		}
	}

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "geowin: 连不上合成器\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (compositor == NULL || shm == NULL || wm_base == NULL) {
		fprintf(stderr, "geowin: 缺 compositor / shm / xdg_wm_base\n");
		return 1;
	}

	start_ms = now_ms();
	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, title);
	xdg_toplevel_set_app_id(toplevel, "geowin");
	if (fixed_size) {
		xdg_toplevel_set_min_size(toplevel, buf_w, buf_h);
		xdg_toplevel_set_max_size(toplevel, buf_w, buf_h);
	}
	wl_surface_commit(surface);
	wl_display_roundtrip(display);

	buffer = make_buffer(buf_w, buf_h);
	wl_surface_attach(surface, buffer, 0, 0);
	/* 声明的窗口几何（可以故意比表面大） */
	xdg_surface_set_window_geometry(xdg_surface, 0, 0, geo_w, geo_h);
	wl_surface_damage_buffer(surface, 0, 0, buf_w, buf_h);
	wl_surface_commit(surface);
	mapped = true;
	wl_display_flush(display);
	printf("geowin mapped %dx%d geometry %dx%d\n", buf_w, buf_h, geo_w, geo_h);
	fflush(stdout);

	/* 先让它正常上屏，再缩表面（几何一个字不动） */
	if (shrink_w > 0 && shrink_h > 0) {
		int64_t until = now_ms() + 400;
		while (now_ms() < until) {
			struct pollfd pfd = {
				.fd = wl_display_get_fd(display),
				.events = POLLIN,
			};
			if (poll(&pfd, 1, 50) > 0 && wl_display_dispatch(display) < 0) {
				break;
			}
		}
		struct wl_buffer *small = make_buffer(shrink_w, shrink_h);
		wl_surface_attach(surface, small, 0, 0);
		wl_surface_damage_buffer(surface, 0, 0, shrink_w, shrink_h);
		wl_surface_commit(surface);
		wl_display_flush(display);
		printf("geowin shrink %dx%d\n", shrink_w, shrink_h);
		fflush(stdout);
	}

	int64_t deadline = now_ms() + linger_ms;
	while (now_ms() < deadline) {
		/* 用 poll 等事件（dispatch_timeout 要 struct timespec*，不折腾）；
		 * 超时没事件也继续等，直到 linger 到点。 */
		struct pollfd pfd = {
			.fd = wl_display_get_fd(display),
			.events = POLLIN,
		};
		if (poll(&pfd, 1, 50) > 0) {
			if (wl_display_dispatch(display) < 0) {
				break;
			}
		}
	}
	printf("geowin exit %lld\n", (long long)(now_ms() - start_ms));
	fflush(stdout);
	return 0;
}
