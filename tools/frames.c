/* 帧回调计数器：一个「只按合成器的许可出帧」的窗口。
 *
 * 每一帧都先要一个 wl_surface.frame 回调，等合成器给了才画下一帧（Wayland 客户端的
 * 标准节流方式，mpv / 游戏 / Qt 都这么做）。每出一帧打印一行：
 *
 *   frame <n> <毫秒> <颜色>
 *
 * 这样「客户端还在不在出帧」就变成可断言的一行行输出：窗口滚到视口外（或者所在
 * 工作区被藏起来）时，如果合成器不再给帧回调，这个窗口就不再打印 —— 视频会停、
 * 录屏录到的是冻住的一张。vidow 的窗口规则 force_render / 窗口截图源就是让它在
 * 那种情况下照样出帧（见 src/view.c 的 vidow_view_render_pump）。
 *
 * 用法：frames <标题> [运行秒数]     颜色每帧都变，方便录屏/截图看出画面是不是新的。
 */
/* memfd_create 要 _GNU_SOURCE（项目全局按 POSIX 编） */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
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
static void *pixels;
static int width = 320, height = 200;
static uint32_t *pixels32;

static int frames;
static struct timespec start;
static bool running = true;
static bool configured;

static int ms_since_start(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int)((now.tv_sec - start.tv_sec) * 1000 +
		(now.tv_nsec - start.tv_nsec) / 1000000);
}

static int create_shm_file(size_t size) {
	int fd = memfd_create("frames", MFD_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void buffer_release(void *data, struct wl_buffer *wl_buffer) {
	(void)data;
	(void)wl_buffer;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static void fill(uint32_t color) {
	for (int i = 0; i < width * height; i++) {
		pixels32[i] = color;
	}
}

/* 画下一帧。顺序很关键：**先**要帧回调，**再** attach/commit —— 反过来的话
 * 合成器要等下一次提交才回回调，而客户端正等着回调才提交，两边就互相等了
 * （第一次写这个工具就是这么卡住的：一帧都出不来）。 */
static void draw_next(void);

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
	(void)data;
	(void)time;
	wl_callback_destroy(callback);
	frames++;
	/* 颜色每帧变一点：录屏 / 截图能看出这是新画面而不是冻住的那张。 */
	uint32_t r = (uint32_t)((frames * 7) % 256);
	uint32_t g = (uint32_t)((frames * 3) % 256);
	uint32_t b = (uint32_t)(255 - (frames * 5) % 256);
	fill(0xff000000u | (r << 16) | (g << 8) | b);
	printf("frame %d %d %02x%02x%02x\n", frames, ms_since_start(), r, g, b);
	fflush(stdout);

	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	draw_next();
}

static const struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

static void draw_next(void) {
	struct wl_callback *callback = wl_surface_frame(surface);
	wl_callback_add_listener(callback, &frame_listener, NULL);
	wl_surface_commit(surface);
}

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
		uint32_t serial) {
	(void)data;
	xdg_surface_ack_configure(xdg_surface, serial);
	if (!configured) {
		configured = true;
		wl_surface_attach(surface, buffer, 0, 0);
		wl_surface_damage_buffer(surface, 0, 0, width, height);
		draw_next();
	}
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	(void)data;
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	(void)data;
	(void)version;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
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

static void on_signal(int signo) {
	(void)signo;
	running = false;
}

int main(int argc, char *argv[]) {
	const char *title = argc > 1 ? argv[1] : "frames";
	int limit_ms = argc > 2 ? atoi(argv[2]) * 1000 : 0;
	clock_gettime(CLOCK_MONOTONIC, &start);

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "frames: 连不上合成器\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (compositor == NULL || shm == NULL || wm_base == NULL) {
		fprintf(stderr, "frames: 少了 compositor / shm / xdg_wm_base\n");
		return 1;
	}

	int stride = width * 4;
	int size = stride * height;
	int fd = create_shm_file((size_t)size);
	if (fd < 0) {
		fprintf(stderr, "frames: shm 建不出来: %s\n", strerror(errno));
		return 1;
	}
	pixels = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		fprintf(stderr, "frames: mmap 失败: %s\n", strerror(errno));
		return 1;
	}
	pixels32 = pixels;
	fill(0xff3366ccu);

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
		WL_SHM_FORMAT_XRGB8888);
	wl_buffer_add_listener(buffer, &buffer_listener, NULL);
	wl_shm_pool_destroy(pool);
	close(fd);

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_set_title(toplevel, title);
	xdg_toplevel_set_app_id(toplevel, "frames");
	wl_surface_commit(surface);

	while (running) {
		if (wl_display_dispatch(display) < 0) {
			break;
		}
		if (limit_ms > 0 && ms_since_start() > limit_ms) {
			break;
		}
	}
	fprintf(stderr, "frames: 一共 %d 帧\n", frames);
	return 0;
}
