#define _GNU_SOURCE /* memfd_create */
/* ext-background-effect 测试客户端。
 *
 * 开一个半透明的 xdg_toplevel，用 ext-background-effect-v1 只给其中一块
 * 区域请求模糊。背后的壁纸（tools/wall.py 的硬边方块）在区域里应该糊掉、
 * 区域外应该还是硬的 —— 截图比对像素对比度就能验证。
 *
 * 用法：./build/bgblur [宽 高 区域x 区域y 区域宽 区域高]
 * 默认 640x400，区域取窗口中间那条 640x120。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "ext-background-effect-v1-client-protocol.h"

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct ext_background_effect_manager_v1 *bge_manager;
static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
static struct ext_background_effect_surface_v1 *bge;
static struct wl_region *region;
static int width = 640, height = 400;
static int buf_w = 640, buf_h = 400; /* 当前缓冲尺寸（跟着 configure 走） */
/* 默认只给中间一条横带给模糊区域 —— 那是给「区域里外对比」用的老测试。
 * --full（或 argc<7 时显式传）改成正整窗：区域 = 0,0,宽,高。 */
static int rx = 0, ry = 140, rw = 640, rh = 120;
static int full_window = 0;

/* ------------------------------------------------------------ shm buffer */

static int create_shm_file(size_t size) {
	int fd = memfd_create("bgblur", MFD_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* 半透明深色：背后的东西能透出来，模糊与否一目了然 */
static void attach_buffer(void) {
	width = buf_w;
	height = buf_h;
	int stride = width * 4;
	int size = stride * height;
	int fd = create_shm_file((size_t)size);
	if (fd < 0) {
		fprintf(stderr, "bgblur: shm failed\n");
		exit(1);
	}
	uint32_t *data = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
		MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "bgblur: mmap failed\n");
		exit(1);
	}
	for (int i = 0; i < width * height; i++) {
		data[i] = 0x66000000u; /* premultiplied A=0.4 黑 */
	}
	munmap(data, (size_t)size);

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
		stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	wl_surface_commit(surface);
}

/* ---------------------------------------------------------------- 协议 */

static void wm_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	(void)data;
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void xdg_surface_configure(void *data, struct xdg_surface *xs,
		uint32_t serial) {
	(void)data;
	xdg_surface_ack_configure(xs, serial);
	/* 合成器给的尺寸才是最终尺寸，照它重建缓冲，免得被缩放 */
	attach_buffer();
	/* 整窗模糊：区域要按**实际拿到了多大**来设 —— 客户端申请的尺寸会被
	 * 合成器按平铺布局改小（申请 900x600、实际 631x708），拿申请值设区域
	 * 就只盖住一部分，看起来「只有一条是糊的」。 */
	if (full_window && bge != NULL && compositor != NULL) {
		struct wl_region *r = wl_compositor_create_region(compositor);
		wl_region_add(r, 0, 0, width, height);
		ext_background_effect_surface_v1_set_blur_region(bge, r);
		wl_region_destroy(r);
	}
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *tl,
		int32_t w, int32_t h, struct wl_array *states) {
	(void)data;
	(void)tl;
	(void)states;
	if (w > 0 && h > 0) {
		buf_w = w;
		buf_h = h;
	}
}

static void toplevel_close(void *data, struct xdg_toplevel *tl) {
	(void)data;
	(void)tl;
	exit(0);
}

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *tl,
		int32_t w, int32_t h) {
	(void)data;
	(void)tl;
	(void)w;
	(void)h;
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *tl,
		struct wl_array *caps) {
	(void)data;
	(void)tl;
	(void)caps;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_configure_bounds,
	.wm_capabilities = toplevel_wm_capabilities,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
		const char *interface, uint32_t version) {
	(void)data;
	(void)version;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
	} else if (strcmp(interface,
			ext_background_effect_manager_v1_interface.name) == 0) {
		bge_manager = wl_registry_bind(reg, name,
			&ext_background_effect_manager_v1_interface, 1);
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

static void bge_capabilities(void *data,
		struct ext_background_effect_manager_v1 *manager, uint32_t flags) {
	(void)data;
	(void)manager;
	printf("capabilities: blur=%d\n", (flags & 1) != 0);
	fflush(stdout);
}

static const struct ext_background_effect_manager_v1_listener bge_listener = {
	.capabilities = bge_capabilities,
};

int main(int argc, char *argv[]) {
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--full") == 0) {
			full_window = 1;
		}
	}
	if (argc >= 7) {
		width = atoi(argv[1]);
		height = atoi(argv[2]);
		rx = atoi(argv[3]);
		ry = atoi(argv[4]);
		rw = atoi(argv[5]);
		rh = atoi(argv[6]);
	}

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "bgblur: cannot connect\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (compositor == NULL || shm == NULL || wm_base == NULL) {
		fprintf(stderr, "bgblur: missing core globals\n");
		return 1;
	}
	if (bge_manager == NULL) {
		fprintf(stderr, "bgblur: no ext_background_effect_manager_v1\n");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	ext_background_effect_manager_v1_add_listener(bge_manager, &bge_listener,
		NULL);

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, "bgblur");
	xdg_toplevel_set_app_id(toplevel, "bgblur");

	/* 只给这一块请求模糊。BGBLUR_NO_REGION=1 时不请求，用作对照。 */
	bge = ext_background_effect_manager_v1_get_background_effect(bge_manager,
		surface);
	if (getenv("BGBLUR_NO_REGION") == NULL) {
		region = wl_compositor_create_region(compositor);
		if (full_window) {
			/* 整窗模糊：录屏 / 看效果时不该只剩中间一条。尺寸取这个客户端
			 * 自己申请的（buf_w/h 要等 configure 才有值，那时已经晚了，
			 * 所以用命令行给的 width/height）。 */
			wl_region_add(region, 0, 0, width, height);
			printf("bgblur: 整窗模糊区域 0,0 %dx%d\n", width, height);
		} else {
			wl_region_add(region, rx, ry, rw, rh);
		}
		ext_background_effect_surface_v1_set_blur_region(bge, region);
		wl_region_destroy(region);
	}

	wl_surface_commit(surface);
	wl_display_roundtrip(display);
	printf("bgblur: %dx%d, blur region %d,%d %dx%d\n", width, height, rx, ry,
		rw, rh);
	fflush(stdout);

	while (wl_display_dispatch(display) != -1) {
		/* 事件循环 */
	}
	return 0;
}
