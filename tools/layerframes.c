/* layer surface 的帧回调计数器：内容全透明、输入区为空的那种面板（osd / 收起的
 * dock / 全屏时被藏起来的 bar）。
 *
 * 每一帧都先要一个 wl_surface.frame 回调，等合成器给了才画下一帧（Qt / Quickshell
 * 用的就是这套节流）。打印：
 *
 *   frame <n> <毫秒> <state>     state = clear | show
 *   show <毫秒>                  第一次切到不透明内容
 *
 * 用途：验证「内容全透明 → 合成器关掉场景节点」之后，补帧泵还在不在喂这个客户端。
 *
 *   - 一开始交一张**全透明**（ARGB alpha=0）的缓冲，并且把 input region 设成空
 *     —— 合成器会把场景节点整个关掉（直通扫屏那条优化）。
 *   - 客户端不看屏幕、只按帧回调出帧：节点一关，wlr_scene 就不再发 frame_done，
 *     于是这里一行 `frame` 都不再打印（这就是「dock 收起来就再也弹不回来」的
 *     机理）。
 *   - 合成器的 layer 补帧泵（view.c 的 vidow_view_render_pump）应该按 16ms 给它
 *     发帧；到 VIDOW_TEST_SHOW_MS（默认 500ms）客户端切成不透明内容，合成器复检
 *     到内容后把节点重新点亮 —— 日志里能看到 `layer ... 节点关` → `节点开`。
 *
 * 用法：layerframes [运行秒数]
 * 环境：VIDOW_TEST_SHOW_MS=500   多少毫秒后切成不透明
 *       VIDOW_TEST_LAYER_FRAMES_SIZE=800x200  面板尺寸（默认 800x200）
 */
#define _GNU_SOURCE

#include <errno.h>
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

#include "wlr-layer-shell-client-protocol.h"

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;

static struct wl_surface *surface;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_buffer *clear_buffer;
static struct wl_buffer *show_buffer;
static void *clear_pixels;
static void *show_pixels;
static int width = 800, height = 200;

static int frames;
static struct timespec start;
static bool running = true;
static bool configured;
static bool showing;
static int show_after_ms = 500;

static int ms_since_start(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int)((now.tv_sec - start.tv_sec) * 1000 +
		(now.tv_nsec - start.tv_nsec) / 1000000);
}

static int create_shm_file(size_t size) {
	int fd = memfd_create("layerframes", MFD_CLOEXEC);
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

/* 一张全透明、一张不透明：两套缓冲，切内容时不用等 release。 */
static struct wl_buffer *make_buffer(uint32_t color, void **pixels_out) {
	int stride = width * 4;
	int size = stride * height;
	int fd = create_shm_file((size_t)size);
	if (fd < 0) {
		fprintf(stderr, "layerframes: shm 建不出来: %s\n", strerror(errno));
		return NULL;
	}
	void *pixels = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
		MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		fprintf(stderr, "layerframes: mmap 失败: %s\n", strerror(errno));
		close(fd);
		return NULL;
	}
	uint32_t *p = pixels;
	for (int i = 0; i < width * height; i++) {
		p[i] = color;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
		width, height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_buffer_add_listener(buffer, &buffer_listener, NULL);
	wl_shm_pool_destroy(pool);
	close(fd);
	*pixels_out = pixels;
	return buffer;
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time);

static const struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

static void draw_next(void) {
	struct wl_buffer *use = showing ? show_buffer : clear_buffer;
	wl_surface_attach(surface, use, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	struct wl_callback *callback = wl_surface_frame(surface);
	wl_callback_add_listener(callback, &frame_listener, NULL);
	wl_surface_commit(surface);
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
	(void)data;
	(void)time;
	wl_callback_destroy(callback);
	frames++;
	printf("frame %d %d %s\n", frames, ms_since_start(),
		showing ? "show" : "clear");
	fflush(stdout);

	/* 到点了切不透明：故意等到**收到帧回调之后**才切 —— 节点被关掉、补帧泵
	 * 不喂的话，这里永远走不到（这正是要测的那条路）。 */
	if (!showing && ms_since_start() >= show_after_ms) {
		showing = true;
		printf("show %d\n", ms_since_start());
		fflush(stdout);
	}
	draw_next();
}

static void layer_surface_configure(void *data,
		struct zwlr_layer_surface_v1 *ls, uint32_t serial,
		uint32_t w, uint32_t h) {
	(void)data;
	(void)w;
	(void)h;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	if (configured) {
		return;
	}
	configured = true;
	draw_next();
}

static void layer_surface_closed(void *data,
		struct zwlr_layer_surface_v1 *ls) {
	(void)data;
	(void)ls;
	running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
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
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, 4);
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
	int limit_ms = argc > 1 ? atoi(argv[1]) * 1000 : 0;
	const char *show_env = getenv("VIDOW_TEST_SHOW_MS");
	if (show_env != NULL) {
		show_after_ms = atoi(show_env);
	}
	const char *size_env = getenv("VIDOW_TEST_LAYER_FRAMES_SIZE");
	if (size_env != NULL) {
		int w = 0, h = 0;
		if (sscanf(size_env, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
			width = w;
			height = h;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &start);

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "layerframes: 连不上合成器\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (compositor == NULL || shm == NULL || layer_shell == NULL) {
		fprintf(stderr, "layerframes: 少了 compositor / shm / layer_shell\n");
		return 1;
	}

	clear_buffer = make_buffer(0x00000000u, &clear_pixels);
	show_buffer = make_buffer(0xff2266ccu, &show_pixels);
	if (clear_buffer == NULL || show_buffer == NULL) {
		return 1;
	}

	surface = wl_compositor_create_surface(compositor);
	/* input region 留空：真正的 osd / 收起的全屏画布就是这种（什么都不申报），
	 * 合成器才会因为「内容全透明」把节点关掉。 */
	struct wl_region *empty = wl_compositor_create_region(compositor);
	wl_surface_set_input_region(surface, empty);
	wl_region_destroy(empty);

	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell,
		surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "layerframes");
	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_surface_listener,
		NULL);
	zwlr_layer_surface_v1_set_size(layer_surface, (uint32_t)width,
		(uint32_t)height);
	zwlr_layer_surface_v1_set_anchor(layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, 0);
	zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	wl_surface_commit(surface);

	while (running) {
		/* 用 prepare_read + poll，而不是裸 wl_display_dispatch：节点被关掉、
		 * 补帧泵不喂的时候一个事件都不会来，裸 dispatch 会永久阻塞 ——
		 * 连「跑满 N 秒就退」都做不到（测试会挂死）。 */
		while (wl_display_prepare_read(display) != 0) {
			if (wl_display_dispatch_pending(display) < 0) {
				running = false;
				break;
			}
		}
		if (!running) {
			break;
		}
		wl_display_flush(display);
		int remain = 1000;
		if (limit_ms > 0) {
			remain = limit_ms - ms_since_start();
			if (remain <= 0) {
				wl_display_cancel_read(display);
				break;
			}
			if (remain > 1000) {
				remain = 1000;
			}
		}
		struct pollfd pfd = {
			.fd = wl_display_get_fd(display),
			.events = POLLIN,
		};
		int pr = poll(&pfd, 1, remain);
		if (pr < 0) {
			wl_display_cancel_read(display);
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (pr == 0) {
			wl_display_cancel_read(display);
			continue;
		}
		if (wl_display_read_events(display) < 0) {
			break;
		}
		if (wl_display_dispatch_pending(display) < 0) {
			break;
		}
	}
	fprintf(stderr, "layerframes: 一共 %d 帧\n", frames);
	return 0;
}
