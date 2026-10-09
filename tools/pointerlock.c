/* 指针约束客户端：游戏锁鼠标 / 绘图软件夹画笔用的那两个协议，最小复现。
 *
 * 用法：pointerlock <lock|confine> [WxH+X+Y] [标题]
 *   lock            在窗口上要一个 zwp_locked_pointer_v1（锁鼠标）
 *   confine         要一个 zwp_confined_pointer_v1，区域默认 100x100+50+50
 *
 * 每件事都打一行，测试就断言这些行（`./test-pointer-lock.sh`）：
 *   ready <w>x<h>                窗口映射好了、指针已经进去过、约束对象建好了
 *   locked / unlocked            合成器说锁上了 / 松开了
 *   confined / unconfined        合成器说夹住了 / 放开了
 *   rel <dx> <dy>                这一下收到多少相对位移（wp_relative_pointer）
 *   reltotal <sumx> <sumy>       相对位移的累计 —— 锁定时「位置不动、位移照发」
 *                                就靠它断言
 *   motion <x> <y>               指针在窗口里的位置（surface 局部坐标）
 *
 * 约束在**窗口 map 好之后**就建（真客户端也是这样：一开始画就锁），指针进来时
 * 合成器才把它激活 —— 所以 hint 是「锁生效之前就设好的」，测试里先把光标移进
 * 窗口才会看到 locked / confined。
 *
 * 另外按 `--hint=X,Y` 可以在锁之前告诉合成器「光标该在哪」（cursor_hint），
 * 用来验解锁时把光标挪回 hint 那条路。 */

/* usleep / memfd_create 要 _GNU_SOURCE（项目全局按 POSIX 编） */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct xdg_wm_base *wm_base;
static struct zwp_pointer_constraints_v1 *constraints;
static struct zwp_relative_pointer_manager_v1 *relative_mgr;

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
static struct wl_buffer *buffer;
static void *pixels;
static uint32_t *pixels32;
static int width = 400, height = 300;

static struct zwp_locked_pointer_v1 *locked;
static struct zwp_confined_pointer_v1 *confined;
static struct zwp_relative_pointer_v1 *relative;

static const char *mode = "lock";
static int region_x = 50, region_y = 50, region_w = 100, region_h = 100;
static bool have_hint;
static int hint_x, hint_y;

static bool running = true;
static bool configured, entered, constraint_made;
static bool seen_motion;
static int last_mx, last_my;
static double rel_total_x, rel_total_y;

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
	int fd = memfd_create("pointerlock", MFD_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* --------------------------------------------------------- 约束对象 */

static void locked_handle_locked(void *data, struct zwp_locked_pointer_v1 *obj) {
	(void)data; (void)obj;
	emit("locked");
}

static void locked_handle_unlocked(void *data, struct zwp_locked_pointer_v1 *obj) {
	(void)data; (void)obj;
	emit("unlocked");
}

static const struct zwp_locked_pointer_v1_listener locked_listener = {
	.locked = locked_handle_locked,
	.unlocked = locked_handle_unlocked,
};

static void confined_handle_confined(void *data,
		struct zwp_confined_pointer_v1 *obj) {
	(void)data; (void)obj;
	emit("confined");
}

static void confined_handle_unconfined(void *data,
		struct zwp_confined_pointer_v1 *obj) {
	(void)data; (void)obj;
	emit("unconfined");
}

static const struct zwp_confined_pointer_v1_listener confined_listener = {
	.confined = confined_handle_confined,
	.unconfined = confined_handle_unconfined,
};

/* 相对位移：锁鼠标时客户端唯一的输入来源（位置不动，位移照发）。 */
static void relative_motion(void *data, struct zwp_relative_pointer_v1 *obj,
		uint32_t utime_hi, uint32_t utime_lo, wl_fixed_t dx, wl_fixed_t dy,
		wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel) {
	(void)data; (void)obj; (void)utime_hi; (void)utime_lo;
	(void)dx_unaccel; (void)dy_unaccel;
	rel_total_x += wl_fixed_to_double(dx);
	rel_total_y += wl_fixed_to_double(dy);
	emit("rel %.1f %.1f", wl_fixed_to_double(dx), wl_fixed_to_double(dy));
	emit("reltotal %.1f %.1f", rel_total_x, rel_total_y);
}

static const struct zwp_relative_pointer_v1_listener relative_listener = {
	.relative_motion = relative_motion,
};

static void make_constraint(void) {
	if (constraint_made || pointer == NULL || surface == NULL) {
		return;
	}
	constraint_made = true;

	relative = zwp_relative_pointer_manager_v1_get_relative_pointer(relative_mgr,
		pointer);
	zwp_relative_pointer_v1_add_listener(relative, &relative_listener, NULL);

	struct wl_region *region = NULL;
	if (strcmp(mode, "confine") == 0) {
		region = wl_compositor_create_region(compositor);
		wl_region_add(region, region_x, region_y, region_w, region_h);
		confined = zwp_pointer_constraints_v1_confine_pointer(constraints,
			surface, pointer, region,
			ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
		zwp_confined_pointer_v1_add_listener(confined, &confined_listener, NULL);
	} else {
		locked = zwp_pointer_constraints_v1_lock_pointer(constraints, surface,
			pointer, region, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
		zwp_locked_pointer_v1_add_listener(locked, &locked_listener, NULL);
		if (have_hint) {
			zwp_locked_pointer_v1_set_cursor_position_hint(locked,
				wl_fixed_from_int(hint_x), wl_fixed_from_int(hint_y));
		}
	}
	if (region != NULL) {
		wl_region_destroy(region);
	}
	/* 约束的 region / hint 是跟着这次 commit 一起交给合成器的 */
	wl_surface_commit(surface);
	emit("ready %dx%d", width, height);
}

/* -------------------------------------------------------------- 指针 */

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *surf, wl_fixed_t sx, wl_fixed_t sy) {
	(void)data; (void)p; (void)serial; (void)surf;
	entered = true;
	emit("enter %d %d", wl_fixed_to_int(sx), wl_fixed_to_int(sy));
	make_constraint();
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *surf) {
	(void)data; (void)p; (void)serial; (void)surf;
	emit("leave");
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time,
		wl_fixed_t sx, wl_fixed_t sy) {
	(void)data; (void)p; (void)time;
	seen_motion = true;
	last_mx = wl_fixed_to_int(sx);
	last_my = wl_fixed_to_int(sy);
	emit("motion %d %d", last_mx, last_my);
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial,
		uint32_t time, uint32_t button, uint32_t state) {
	(void)data; (void)p; (void)serial; (void)time; (void)button; (void)state;
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time,
		uint32_t axis, wl_fixed_t value) {
	(void)data; (void)p; (void)time; (void)axis; (void)value;
}

static void pointer_frame(void *data, struct wl_pointer *p) {
	(void)data; (void)p;
}

static void pointer_axis_source(void *data, struct wl_pointer *p, uint32_t src) {
	(void)data; (void)p; (void)src;
}

static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time,
		uint32_t axis) {
	(void)data; (void)p; (void)time; (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *p,
		uint32_t axis, int32_t discrete) {
	(void)data; (void)p; (void)axis; (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps) {
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && pointer == NULL) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
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
		make_constraint();
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
	} else if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
		constraints = wl_registry_bind(registry, name,
			&zwp_pointer_constraints_v1_interface, 1);
	} else if (strcmp(interface,
			zwp_relative_pointer_manager_v1_interface.name) == 0) {
		relative_mgr = wl_registry_bind(registry, name,
			&zwp_relative_pointer_manager_v1_interface, 1);
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
	(void)signo;
	running = false;
}

int main(int argc, char *argv[]) {
	if (argc > 1) {
		mode = argv[1];
	}
	if (strcmp(mode, "lock") != 0 && strcmp(mode, "confine") != 0) {
		fprintf(stderr, "pointerlock: 模式只有 lock / confine（给的是 %s）\n",
			mode);
		return 2;
	}
	const char *title = "pointerlock";
	for (int i = 2; i < argc; i++) {
		if (sscanf(argv[i], "%dx%d+%d+%d", &region_w, &region_h, &region_x,
				&region_y) == 4) {
			continue;
		}
		if (sscanf(argv[i], "--hint=%d,%d", &hint_x, &hint_y) == 2) {
			have_hint = true;
			continue;
		}
		title = argv[i];
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "pointerlock: 连不上合成器\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (compositor == NULL || shm == NULL || wm_base == NULL || seat == NULL) {
		fprintf(stderr, "pointerlock: 少了 compositor / shm / seat / xdg_wm_base\n");
		return 1;
	}
	if (constraints == NULL) {
		fprintf(stderr, "pointerlock: 合成器没提供 zwp_pointer_constraints_v1\n");
		return 1;
	}
	if (relative_mgr == NULL) {
		fprintf(stderr, "pointerlock: 合成器没提供 zwp_relative_pointer_manager_v1\n");
		return 1;
	}

	int stride = width * 4;
	int size = stride * height;
	int fd = create_shm_file((size_t)size);
	if (fd < 0) {
		fprintf(stderr, "pointerlock: shm 建不出来: %s\n", strerror(errno));
		return 1;
	}
	pixels = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		fprintf(stderr, "pointerlock: mmap 失败: %s\n", strerror(errno));
		return 1;
	}
	pixels32 = pixels;
	for (int i = 0; i < width * height; i++) {
		pixels32[i] = 0xff226699u;
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
	xdg_toplevel_set_app_id(toplevel, "pointerlock");
	wl_surface_commit(surface);

	/* 指针已经进来了才轮得到建约束；没进来就等（测试会先把光标移进来）。 */
	while (running) {
		if (wl_display_dispatch(display) < 0) {
			break;
		}
	}
	fprintf(stderr, "pointerlock: 收工（entered=%d constraint=%d motion=%d "
		"last=%d,%d reltotal=%.1f,%.1f）\n", entered, constraint_made,
		seen_motion, last_mx, last_my, rel_total_x, rel_total_y);
	return 0;
}
