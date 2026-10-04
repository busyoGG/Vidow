#define _GNU_SOURCE /* memfd_create */
/* xdg-foreign 测试客户端。
 *
 * 开一个 xdg_toplevel，用 zxdg_exporter_v1 和 zxdg_exporter_v2 各导出一个
 * handle 打到 stdout，然后挂着不动。真实场景里这就是应用自己的窗口：portal
 * （GTK3 的 xdg-desktop-portal-gtk 用 v1，GTK4 / Qt6 用 v2）拿这个 handle 把
 * 文件选择器挂到它上面，对话框才会居中、才会跟着父窗口走。
 *
 * 两个 handle 必须能被对方版本的 importer 认出来 —— 合成器只建一个
 * wlr_xdg_foreign_registry 就是为了这个。把 V1 那个 handle 当 parent_window
 * 传给 GTK3 的 portal，能挂上就说明跨版本共用 registry 是通的。
 *
 * 用法：WAYLAND_DISPLAY=wayland-0 ./build/foreign
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
#include "xdg-foreign-unstable-v1-client-protocol.h"
#include "xdg-foreign-unstable-v2-client-protocol.h"

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct zxdg_exporter_v1 *exporter_v1;
static struct zxdg_exporter_v2 *exporter_v2;
static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
static int exported_count;

static int create_shm_file(size_t size) {
	int fd = memfd_create("foreign", MFD_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void attach_buffer(void) {
	int w = 400, h = 260;
	int stride = w * 4;
	int size = stride * h;
	int fd = create_shm_file((size_t)size);
	if (fd < 0) {
		fprintf(stderr, "foreign: shm failed\n");
		exit(1);
	}
	uint32_t *data = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
		MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "foreign: mmap failed\n");
		exit(1);
	}
	for (int i = 0; i < w * h; i++) {
		data[i] = 0xff204080u; /* 不透明蓝：好认 */
	}
	munmap(data, (size_t)size);

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
		WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, w, h);
	wl_surface_commit(surface);
}

/* ------------------------------------------------------------- listeners */

static void handle_v1_handle(void *data, struct zxdg_exported_v1 *exported,
		const char *handle) {
	(void)data;
	(void)exported;
	printf("V1=%s\n", handle);
	fflush(stdout);
}

static const struct zxdg_exported_v1_listener exported_v1_listener = {
	.handle = handle_v1_handle,
};

static void handle_v2_handle(void *data, struct zxdg_exported_v2 *exported,
		const char *handle) {
	(void)data;
	(void)exported;
	printf("V2=%s\n", handle);
	fflush(stdout);
	if (++exported_count >= 2) {
		printf("READY\n");
		fflush(stdout);
	}
}

static const struct zxdg_exported_v2_listener exported_v2_listener = {
	.handle = handle_v2_handle,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xs,
		uint32_t serial) {
	(void)data;
	xdg_surface_ack_configure(xs, serial);

	if (!exported_count) {
		/* 窗口上了屏再导出：导入方要求父窗口是 mapped 的，否则
		 * wlroots 会把 parent 设成 NULL。 */
		attach_buffer();
		if (exporter_v1 != NULL) {
			struct zxdg_exported_v1 *exported_v1 =
				zxdg_exporter_v1_export(exporter_v1, surface);
			zxdg_exported_v1_add_listener(exported_v1,
				&exported_v1_listener, NULL);
		}
		if (exporter_v2 != NULL) {
			struct zxdg_exported_v2 *exported_v2 =
				zxdg_exporter_v2_export_toplevel(exporter_v2, surface);
			zxdg_exported_v2_add_listener(exported_v2,
				&exported_v2_listener, NULL);
		}
		exported_count = 1;
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
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
			version < 4 ? version : 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
			version < 6 ? version : 6);
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	} else if (strcmp(interface, zxdg_exporter_v1_interface.name) == 0) {
		exporter_v1 = wl_registry_bind(registry, name,
			&zxdg_exporter_v1_interface, 1);
	} else if (strcmp(interface, zxdg_exporter_v2_interface.name) == 0) {
		exporter_v2 = wl_registry_bind(registry, name,
			&zxdg_exporter_v2_interface, 1);
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

int main(void) {
	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "foreign: 连不上合成器\n");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (compositor == NULL || shm == NULL || wm_base == NULL) {
		fprintf(stderr, "foreign: 缺少 wl_compositor / wl_shm / xdg_wm_base\n");
		return 1;
	}
	if (exporter_v1 == NULL && exporter_v2 == NULL) {
		fprintf(stderr, "foreign: 合成器没有 zxdg_exporter_v1 / v2\n");
		return 1;
	}
	printf("EXPORTERS v1=%s v2=%s\n", exporter_v1 ? "yes" : "no",
		exporter_v2 ? "yes" : "no");
	fflush(stdout);

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_set_title(toplevel, "foreign-app");
	xdg_toplevel_set_app_id(toplevel, "vidow-foreign-test");
	wl_surface_commit(surface);

	while (wl_display_dispatch(display) != -1) {
		/* 挂着让 handle 一直有效 */
	}
	return 0;
}
