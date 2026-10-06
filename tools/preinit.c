/* 状态请求抢在第一次 commit 之前发出去的客户端（xdg-shell）。
 *
 * xdg-shell 允许客户端建好 xdg_toplevel 之后、交第一帧之前就 set_maximized /
 * set_minimized / set_fullscreen。wlroots 收到就**立刻**发 request_* 信号
 * （types/xdg_shell/wlr_xdg_toplevel.c：xdg_toplevel_handle_set_maximized 里
 * 直接 wl_signal_emit_mutable），而它给「还没 initialized 的 surface 排
 * configure」用的是一句 assert（0.20.2：types/xdg_shell/wlr_xdg_surface.c:168）。
 * 于是合成器要是在信号里当场应答（wlr_xdg_toplevel_set_maximized() /
 * wlr_xdg_surface_schedule_configure()），就被客户端打死：
 *
 *   vidow: types/xdg_shell/wlr_xdg_surface.c:168: wlr_xdg_surface_schedule_configure:
 *          Assertion 'surface->initialized' failed.
 *
 * Qt/KDE 恢复「上次是最大化」的窗口就是这么开窗的（kate 从 tty 起合成器、一开
 * 就崩，栈顶 view_request_maximize）。
 *
 * 用法：preinit [--maximize] [--minimize] [--fullscreen] [--after-commit]
 *               [--linger=秒] [--timeout=秒] [app_id] [标题]
 *   --maximize / --minimize / --fullscreen  要发的状态请求（可叠加；缺省 maximize）
 *   --after-commit   改成上屏（第一次 commit）之后再发 —— 对照组：这条本来就好
 *   --linger=秒       上屏之后再活多久（缺省 3 秒；0 = 上屏就退）
 *   --timeout=秒      总时限，到点无条件退出（缺省 6 秒）—— 最小化那条拿不到
 *                     configure，靠它收尾
 *
 * 输出（一行一条，方便断言）：
 *   preinit request <名字> <before|after>-commit
 *   preinit configure <宽>x<高> states=[...]
 *   preinit mapped <毫秒>
 *   preinit exit <毫秒>
 */
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
static uint32_t *pixels32;
static int width = 360, height = 240;

static const char *app_id = "preinit";
static const char *title = "抢跑";
static bool want_maximize, want_minimize, want_fullscreen;
static bool after_commit;
static int linger_ms = 3000;
static int timeout_ms = 6000;

static bool mapped;
static int mapped_ms = -1;
static struct timespec start;

static int ms_since_start(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int)((now.tv_sec - start.tv_sec) * 1000 +
		(now.tv_nsec - start.tv_nsec) / 1000000);
}

static int create_shm_file(size_t size) {
	int fd = memfd_create("preinit", MFD_CLOEXEC);
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

/* 四象限：左上红、右上绿、左下蓝、右下白（和 tools/pattern.py 一致）。 */
static void fill_pattern(void) {
	uint32_t quad[4] = {
		0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu,
	};
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			int q = (y < height / 2 ? 0 : 2) + (x < width / 2 ? 0 : 1);
			pixels32[y * width + x] = quad[q];
		}
	}
}

/* ------------------------------------------------------- 状态请求（本工具的主角） */

static void send_states(const char *when) {
	if (want_maximize) {
		printf("preinit request maximize %s-commit\n", when);
		fflush(stdout);
		xdg_toplevel_set_maximized(toplevel);
	}
	if (want_fullscreen) {
		printf("preinit request fullscreen %s-commit\n", when);
		fflush(stdout);
		xdg_toplevel_set_fullscreen(toplevel, NULL);
	}
	if (want_minimize) {
		printf("preinit request minimize %s-commit\n", when);
		fflush(stdout);
		xdg_toplevel_set_minimized(toplevel);
	}
}

/* ------------------------------------------------------------ xdg-shell */

static const char *state_name(uint32_t s) {
	switch (s) {
	case XDG_TOPLEVEL_STATE_MAXIMIZED: return "maximized";
	case XDG_TOPLEVEL_STATE_FULLSCREEN: return "fullscreen";
	case XDG_TOPLEVEL_STATE_RESIZING: return "resizing";
	case XDG_TOPLEVEL_STATE_ACTIVATED: return "activated";
	case XDG_TOPLEVEL_STATE_TILED_LEFT: return "tiled_left";
	case XDG_TOPLEVEL_STATE_TILED_RIGHT: return "tiled_right";
	case XDG_TOPLEVEL_STATE_TILED_TOP: return "tiled_top";
	case XDG_TOPLEVEL_STATE_TILED_BOTTOM: return "tiled_bottom";
	default: return "other";
	}
}

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
		int32_t w, int32_t h, struct wl_array *states) {
	(void)data;
	(void)toplevel;
	printf("preinit configure %dx%d states=[", w, h);
	bool first = true;
	uint32_t *s;
	wl_array_for_each(s, states) {
		printf("%s%s", first ? "" : ",", state_name(*s));
		first = false;
	}
	printf("]\n");
	fflush(stdout);
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
	(void)data;
	(void)toplevel;
	printf("preinit close\n");
	fflush(stdout);
}

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel,
		int32_t w, int32_t h) {
	(void)data;
	(void)toplevel;
	(void)w;
	(void)h;
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel,
		struct wl_array *caps) {
	(void)data;
	(void)toplevel;
	(void)caps;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_configure_bounds,
	.wm_capabilities = toplevel_wm_capabilities,
};

static void draw(void) {
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	wl_surface_commit(surface);
}

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
		uint32_t serial) {
	(void)data;
	xdg_surface_ack_configure(xdg_surface, serial);
	if (!mapped) {
		mapped = true;
		draw();
		mapped_ms = ms_since_start();
		printf("preinit mapped %d\n", mapped_ms);
		fflush(stdout);
		if (after_commit) {
			/* 对照组：第一次 commit 之后再发状态请求（这是正常时序，本来就不崩）。 */
			send_states("after");
			wl_display_flush(display);
		}
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
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
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

static bool running = true;

static void on_signal(int signo) {
	(void)signo;
	running = false;
}

int main(int argc, char *argv[]) {
	int argi = 1;
	for (; argi < argc; argi++) {
		const char *a = argv[argi];
		if (strcmp(a, "--maximize") == 0) {
			want_maximize = true;
		} else if (strcmp(a, "--minimize") == 0) {
			want_minimize = true;
		} else if (strcmp(a, "--fullscreen") == 0) {
			want_fullscreen = true;
		} else if (strcmp(a, "--after-commit") == 0) {
			after_commit = true;
		} else if (strncmp(a, "--linger=", 9) == 0) {
			linger_ms = atoi(a + 9) * 1000;
		} else if (strncmp(a, "--timeout=", 10) == 0) {
			timeout_ms = atoi(a + 10) * 1000;
		} else if (a[0] != '-') {
			break;
		} else {
			fprintf(stderr, "preinit: 不认识的选项 %s\n", a);
			return 2;
		}
	}
	if (argi < argc) {
		app_id = argv[argi++];
	}
	if (argi < argc) {
		title = argv[argi++];
	}
	if (!want_maximize && !want_minimize && !want_fullscreen) {
		want_maximize = true;
	}
	clock_gettime(CLOCK_MONOTONIC, &start);

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "preinit: 连不上合成器\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (compositor == NULL || shm == NULL || wm_base == NULL) {
		fprintf(stderr, "preinit: 少了 compositor / shm / xdg_wm_base\n");
		return 1;
	}

	int stride = width * 4;
	int size = stride * height;
	int fd = create_shm_file((size_t)size);
	if (fd < 0) {
		fprintf(stderr, "preinit: shm 建不出来: %s\n", strerror(errno));
		return 1;
	}
	void *pixels = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
		MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		fprintf(stderr, "preinit: mmap 失败: %s\n", strerror(errno));
		return 1;
	}
	pixels32 = pixels;
	fill_pattern();

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
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, title);
	xdg_toplevel_set_app_id(toplevel, app_id);
	if (!after_commit) {
		/* 主角：surface 还没 initialized（一次都还没 commit），先发状态请求。 */
		send_states("before");
	}
	wl_surface_commit(surface);

	while (running) {
		while (wl_display_prepare_read(display) != 0) {
			if (wl_display_dispatch_pending(display) < 0) {
				running = false;
			}
		}
		if (!running) {
			break;
		}
		wl_display_flush(display);
		struct pollfd pfd = {
			.fd = wl_display_get_fd(display),
			.events = POLLIN,
		};
		poll(&pfd, 1, 50);
		if (pfd.revents & POLLIN) {
			if (wl_display_read_events(display) < 0) {
				break;
			}
		} else {
			wl_display_cancel_read(display);
		}
		if (wl_display_dispatch_pending(display) < 0) {
			break;
		}
		int now = ms_since_start();
		if (mapped && linger_ms >= 0 && now >= mapped_ms + linger_ms) {
			break;
		}
		if (now >= timeout_ms) {
			break;
		}
	}
	printf("preinit exit %d\n", ms_since_start());
	fflush(stdout);
	return 0;
}
