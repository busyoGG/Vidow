/* 键盘抑制客户端：`zwp_keyboard_shortcuts_inhibit_manager_v1` 的最小复现 ——
 * 客户端说「我这个窗口在的时候别吃我的快捷键」（虚拟机的窗口要原样收 Super /
 * Alt+Tab，远程桌面客户端也一样）。
 *
 * 用法：keyinhibit [hold|drop|noinhibit] [标题]
 *   hold       窗口一 map 好就建一个 inhibitor，一直拿着（默认）
 *   drop       一样建，但收到 SIGUSR1 就销毁它（打印 `dropped`）—— 用来验
 *              「抑制没了，键位又生效了」
 *   noinhibit  不建 inhibitor（对照：同一个窗口、同样的按键，键位该生效）
 *
 * 每件事打一行，测试断言这些行（`./test-keyboard-grab.sh`）：
 *   ready <w>x<h>          窗口 map 好了
 *   inhibitor created      要走了抑制
 *   active 1 / active 0    合成器认了 / 撤销了（协议里不给回应就是拒绝）
 *   dropped                自己把 inhibitor 销毁了（drop 模式）
 *   enter / leave          键盘焦点进来 / 出去
 *   key <keycode> pressed|released  收到的按键（evdev 键码，和 wl_keyboard 一致）
 *   keycount <n>           收到多少个按键事件 —— 「键有没有送到客户端」靠它量
 *
 * 抑制到底抑制了什么由**合成器**决定（协议只管传话）：vidow 那边是「焦点这个
 * 表面上有生效中的抑制时，vidow 自己的键位全部让路」，见 src/keyboardgrab.c。 */

/* usleep / memfd_create 要 _GNU_SOURCE（项目全局按 POSIX 编） */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_keyboard *keyboard;
static struct xdg_wm_base *wm_base;
static struct zwp_keyboard_shortcuts_inhibit_manager_v1 *inhibit_mgr;

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
static struct wl_buffer *buffer;
static void *pixels;
static uint32_t *pixels32;
static int width = 400, height = 300;

static struct zwp_keyboard_shortcuts_inhibitor_v1 *inhibitor;

static const char *mode = "hold";
static bool configured, dropped;
static int key_count;

static volatile sig_atomic_t running = true;
static volatile sig_atomic_t drop_requested = false;

static void emit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void emit(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

static int create_shm_file(size_t size) {
	int fd = memfd_create("keyinhibit", MFD_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* --------------------------------------------------------- 抑制对象 */

static void inhibitor_handle_active(void *data,
		struct zwp_keyboard_shortcuts_inhibitor_v1 *obj) {
	(void)data; (void)obj;
	emit("active 1");
}

static void inhibitor_handle_inactive(void *data,
		struct zwp_keyboard_shortcuts_inhibitor_v1 *obj) {
	(void)data; (void)obj;
	emit("active 0");
}

static const struct zwp_keyboard_shortcuts_inhibitor_v1_listener
inhibitor_listener = {
	.active = inhibitor_handle_active,
	.inactive = inhibitor_handle_inactive,
};

static void make_inhibitor(void) {
	if (inhibitor != NULL || surface == NULL || seat == NULL ||
			inhibit_mgr == NULL) {
		return;
	}
	inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
		inhibit_mgr, surface, seat);
	zwp_keyboard_shortcuts_inhibitor_v1_add_listener(inhibitor,
		&inhibitor_listener, NULL);
	emit("inhibitor created");
}

static void drop_inhibitor(void) {
	if (inhibitor == NULL) {
		return;
	}
	zwp_keyboard_shortcuts_inhibitor_v1_destroy(inhibitor);
	inhibitor = NULL;
	dropped = true;
	emit("dropped");
}

/* -------------------------------------------------------------- 键盘 */

static void kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
		int32_t fd, uint32_t size) {
	(void)data; (void)kb; (void)format; (void)size;
	close(fd);
}

static void kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
		struct wl_surface *surf, struct wl_array *keys) {
	(void)data; (void)kb; (void)serial; (void)surf; (void)keys;
	emit("enter");
}

static void kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
		struct wl_surface *surf) {
	(void)data; (void)kb; (void)serial; (void)surf;
	emit("leave");
}

static void kb_key(void *data, struct wl_keyboard *kb, uint32_t serial,
		uint32_t time, uint32_t key, uint32_t state) {
	(void)data; (void)kb; (void)serial; (void)time;
	key_count++;
	emit("key %u %s", key,
		state == WL_KEYBOARD_KEY_STATE_PRESSED ? "pressed" : "released");
	emit("keycount %d", key_count);
}

static void kb_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
		uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
	(void)data; (void)kb; (void)serial; (void)depressed; (void)latched;
	(void)locked; (void)group;
}

static void kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate,
		int32_t delay) {
	(void)data; (void)kb; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = kb_keymap,
	.enter = kb_enter,
	.leave = kb_leave,
	.key = kb_key,
	.modifiers = kb_modifiers,
	.repeat_info = kb_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps) {
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard == NULL) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	}
}

static void seat_name(void *data, struct wl_seat *s, const char *name) {
	(void)data; (void)s; (void)name;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ---------------------------------------------------------- xdg-shell */

static void xdg_surface_configure(void *data, struct xdg_surface *xs,
		uint32_t serial) {
	(void)data;
	xdg_surface_ack_configure(xs, serial);
	if (!configured) {
		configured = true;
		wl_surface_attach(surface, buffer, 0, 0);
		wl_surface_damage_buffer(surface, 0, 0, width, height);
		wl_surface_commit(surface);
		if (strcmp(mode, "noinhibit") != 0) {
			make_inhibitor();
		}
		emit("ready %dx%d", width, height);
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

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w,
		int32_t h, struct wl_array *states) {
	(void)data; (void)t; (void)w; (void)h; (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *t) {
	(void)data; (void)t;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

/* ----------------------------------------------------------- registry */

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	(void)data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
			version >= 4 ? 4 : version);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	} else if (strcmp(interface,
			zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name) == 0) {
		inhibit_mgr = wl_registry_bind(registry, name,
			&zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	(void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void on_signal(int signo) {
	if (signo == SIGUSR1) {
		drop_requested = true;
		return;
	}
	running = false;
}

int main(int argc, char *argv[]) {
	if (argc > 1 && (strcmp(argv[1], "hold") == 0 ||
			strcmp(argv[1], "drop") == 0 ||
			strcmp(argv[1], "noinhibit") == 0)) {
		mode = argv[1];
	}
	const char *title = "keyinhibit";
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "hold") != 0 && strcmp(argv[i], "drop") != 0 &&
				strcmp(argv[i], "noinhibit") != 0) {
			title = argv[i];
		}
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGUSR1, on_signal);

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "keyinhibit: 连不上合成器\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (compositor == NULL || shm == NULL || wm_base == NULL || seat == NULL) {
		fprintf(stderr, "keyinhibit: 少了 compositor / shm / seat / xdg_wm_base\n");
		return 1;
	}
	if (strcmp(mode, "noinhibit") != 0 && inhibit_mgr == NULL) {
		fprintf(stderr, "keyinhibit: 合成器没提供 "
			"zwp_keyboard_shortcuts_inhibit_manager_v1\n");
		return 1;
	}

	int stride = width * 4;
	int size = stride * height;
	int fd = create_shm_file((size_t)size);
	if (fd < 0) {
		fprintf(stderr, "keyinhibit: shm 建不出来: %s\n", strerror(errno));
		return 1;
	}
	pixels = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		fprintf(stderr, "keyinhibit: mmap 失败: %s\n", strerror(errno));
		return 1;
	}
	pixels32 = pixels;
	for (int i = 0; i < width * height; i++) {
		pixels32[i] = 0xff992266u;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, title);
	xdg_toplevel_set_app_id(toplevel, "keyinhibit");
	wl_surface_commit(surface);

	while (running) {
		if (wl_display_dispatch_pending(display) < 0) {
			break;
		}
		/* SIGUSR1 只是把标志立起来（信号处理里不碰 wayland 连接），回到
		 * 主循环里再动手 —— 所以要 poll 带超时，不然一直阻塞在 dispatch
		 * 上，drop 要等到下一次事件才发生。 */
		if (drop_requested) {
			drop_requested = false;
			drop_inhibitor();
		}
		if (wl_display_flush(display) < 0 && errno != EAGAIN) {
			break;
		}
		struct pollfd pfd = {
			.fd = wl_display_get_fd(display),
			.events = POLLIN,
		};
		int pr = poll(&pfd, 1, 100);
		if (pr < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (pr == 0) {
			continue;
		}
		if (wl_display_dispatch(display) < 0) {
			break;
		}
	}
	fprintf(stderr, "keyinhibit: 收工（mode=%s inhibitor=%d dropped=%d keys=%d）\n",
		mode, inhibitor != NULL, dropped, key_count);
	return 0;
}
