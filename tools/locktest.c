/* 最小 ext-session-lock-v1 客户端：用来验 vidow 的会话锁（S1）。
 *
 * 本机一个锁屏客户端都没有（swaylock / swaylock-effects / waylock / gtklock /
 * hyprlock / i3lock 全缺，见方案 §3.1），所以自己写一个刚好够用的：不做密码、
 * 不做界面，只走一遍协议规定的握手。
 *
 * 用法：locktest    （stdin 里输 `u` = unlock_and_destroy；`q` = 直接退出）
 *
 * 打出来的行就是测试断言的东西（`./test-sessionlock.sh`）：
 *   output <name> <w>x<h>          发现了这块屏
 *   locksurface <name>             为这块屏建了 lock surface
 *   configure <name> <w>x<h>       收到 configure（已 ack + 贴了纯色 shm + commit）
 *   LOCKED                         收到 ext_session_lock_v1.locked（测试的同步点）
 *   FINISHED                       收到 finished（合成器拒了这把锁 / 已经解锁）
 *   unlock-sent                    我们发了 unlock_and_destroy
 *   kbfocus                        键盘焦点落到我们身上（wl_keyboard.enter）
 *   key <keycode> down|up          收到的按键（S3 的 D：合成器把键送给了锁屏界面，
 *                                  而不是被藏起来的窗口）
 *   pfocus                         指针焦点落到我们身上（wl_pointer.enter）
 *   motion <x> <y>                 指针移动（同上，S3 的 E）
 *   button <button> down|up        指针按键
 *   axis <axis> <value>            滚轮
 *
 * 颜色固定 0x226699（RGB），测试脚本拿它当「锁屏内容」的判据。
 *
 * 抄的是 tools/pointerlock.c 的骨架（wayland 客户端 + shm 样板）。 */

#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "ext-session-lock-v1-client-protocol.h"

#define MAX_OUTPUTS 16
#define LOCK_COLOR 0xff226699u /* XRGB8888：R=0x22 G=0x66 B=0x99 */

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct ext_session_lock_manager_v1 *lock_mgr;
static struct wl_seat *seat;
static struct wl_keyboard *keyboard;
static struct wl_pointer *pointer;
static struct ext_session_lock_v1 *lock;

struct output {
	struct wl_output *wlr_output;
	char name[64];
	bool have_name;
	struct wl_surface *surface;
	struct ext_session_lock_surface_v1 *lock_surface;
	struct wl_buffer *buffer;
	void *pixels;
	size_t pixels_size;
	int width, height;
};

static struct output outputs[MAX_OUTPUTS];
static int output_count;

static bool running = true;
static bool locked;

static void emit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void emit(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

static const char *out_name(const struct output *o) {
	return o->have_name ? o->name : "?";
}

static int create_shm_file(size_t size) {
	int fd = memfd_create("locktest", MFD_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* 贴一张 w×h 的纯色 shm buffer（每次 configure 都要跟它给的尺寸对上，协议里
 * commit 的尺寸和最后一次 ack 的 configure 不一致是 protocol error）。 */
static bool make_buffer(struct output *o, int w, int h) {
	int stride = w * 4;
	size_t size = (size_t)stride * (size_t)h;
	int fd = create_shm_file(size);
	if (fd < 0) {
		fprintf(stderr, "locktest: shm 建不出来: %s\n", strerror(errno));
		return false;
	}
	void *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		fprintf(stderr, "locktest: mmap 失败: %s\n", strerror(errno));
		close(fd);
		return false;
	}
	uint32_t *px = pixels;
	for (size_t i = 0; i < size / 4; i++) {
		px[i] = LOCK_COLOR;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int)size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	if (o->buffer != NULL) {
		wl_buffer_destroy(o->buffer);
	}
	if (o->pixels != NULL) {
		munmap(o->pixels, o->pixels_size);
	}
	o->buffer = buffer;
	o->pixels = pixels;
	o->pixels_size = size;
	o->width = w;
	o->height = h;
	return true;
}

/* -------------------------------------------------- lock surface 的回调 */

static void lock_surface_configure(void *data,
		struct ext_session_lock_surface_v1 *surface, uint32_t serial,
		uint32_t width, uint32_t height) {
	struct output *o = data;
	/* 先 ack 再 commit（协议要求：首个 configure 没 ack 就 commit 是 error）。 */
	ext_session_lock_surface_v1_ack_configure(surface, serial);
	if (width == 0 || height == 0) {
		return;
	}
	if (o->buffer == NULL || o->width != (int)width || o->height != (int)height) {
		if (!make_buffer(o, (int)width, (int)height)) {
			return;
		}
	}
	wl_surface_attach(o->surface, o->buffer, 0, 0);
	wl_surface_damage_buffer(o->surface, 0, 0, (int)width, (int)height);
	wl_surface_commit(o->surface);
	emit("configure %s %ux%u", out_name(o), width, height);
}

static const struct ext_session_lock_surface_v1_listener lock_surface_listener = {
	.configure = lock_surface_configure,
};

/* ------------------------------------------------------- lock 的回调 */

static void lock_locked(void *data, struct ext_session_lock_v1 *l) {
	(void)data; (void)l;
	locked = true;
	emit("LOCKED");
}

static void lock_finished(void *data, struct ext_session_lock_v1 *l) {
	(void)data; (void)l;
	emit("FINISHED");
	running = false;
}

static const struct ext_session_lock_v1_listener lock_listener = {
	.locked = lock_locked,
	.finished = lock_finished,
};

/* ------------------------------------------------------------ 输出 */

static void output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
		int32_t pw, int32_t ph, int32_t subpixel, const char *make,
		const char *model, int32_t transform) {
	(void)data; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)subpixel;
	(void)make; (void)model; (void)transform;
}

static void output_mode(void *data, struct wl_output *o, uint32_t flags,
		int32_t w, int32_t h, int32_t refresh) {
	(void)data; (void)o; (void)flags; (void)w; (void)h; (void)refresh;
}

static void output_done(void *data, struct wl_output *o) {
	(void)data; (void)o;
}

static void output_scale(void *data, struct wl_output *o, int32_t factor) {
	(void)data; (void)o; (void)factor;
}

static void output_name(void *data, struct wl_output *o, const char *name) {
	struct output *out = data;
	(void)o;
	snprintf(out->name, sizeof(out->name), "%s", name);
	out->have_name = true;
}

static void output_description(void *data, struct wl_output *o,
		const char *description) {
	(void)data; (void)o; (void)description;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

/* 给一块屏建 lock surface（lock 请求发出去之后才能建）。 */
static void make_lock_surface(struct output *o) {
	if (lock == NULL || o->lock_surface != NULL || compositor == NULL) {
		return;
	}
	o->surface = wl_compositor_create_surface(compositor);
	if (o->surface == NULL) {
		return;
	}
	o->lock_surface = ext_session_lock_v1_get_lock_surface(lock, o->surface,
		o->wlr_output);
	ext_session_lock_surface_v1_add_listener(o->lock_surface,
		&lock_surface_listener, o);
	emit("locksurface %s", out_name(o));
}

/* ------------------------------------------------------ seat（键盘 / 指针）

 * S3 的 D / E 两条测的是「锁定期间输入去哪了」，所以这个客户端得把收到的键盘
 * 和指针事件打出来 —— 它是「锁屏界面」这一侧唯一的观察者。不做任何界面，
 * 只 emit 行。 */

static void kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
		int32_t fd, uint32_t size) {
	(void)data; (void)kb; (void)format;
	if (fd >= 0) {
		close(fd);
	}
	(void)size;
}

static void kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
		struct wl_surface *surface, struct wl_array *keys) {
	(void)data; (void)kb; (void)serial; (void)surface; (void)keys;
	emit("kbfocus");
}

static void kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
		struct wl_surface *surface) {
	(void)data; (void)kb; (void)serial; (void)surface;
	emit("kbblur");
}

static void kb_key(void *data, struct wl_keyboard *kb, uint32_t serial,
		uint32_t time, uint32_t key, uint32_t state) {
	(void)data; (void)kb; (void)serial; (void)time;
	emit("key %u %s", key,
		state == WL_KEYBOARD_KEY_STATE_PRESSED ? "down" : "up");
}

static void kb_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
		uint32_t depressed, uint32_t latched, uint32_t locked,
		uint32_t group) {
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

static void pt_enter(void *data, struct wl_pointer *pt, uint32_t serial,
		struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
	(void)data; (void)pt; (void)serial; (void)surface; (void)sx; (void)sy;
	emit("pfocus");
}

static void pt_leave(void *data, struct wl_pointer *pt, uint32_t serial,
		struct wl_surface *surface) {
	(void)data; (void)pt; (void)serial; (void)surface;
	emit("pblur");
}

static void pt_motion(void *data, struct wl_pointer *pt, uint32_t time,
		wl_fixed_t sx, wl_fixed_t sy) {
	(void)data; (void)pt; (void)time;
	emit("motion %.0f %.0f", wl_fixed_to_double(sx), wl_fixed_to_double(sy));
}

static void pt_button(void *data, struct wl_pointer *pt, uint32_t serial,
		uint32_t time, uint32_t button, uint32_t state) {
	(void)data; (void)pt; (void)serial; (void)time;
	emit("button %u %s", button,
		state == WL_POINTER_BUTTON_STATE_PRESSED ? "down" : "up");
}

static void pt_axis(void *data, struct wl_pointer *pt, uint32_t time,
		uint32_t axis, wl_fixed_t value) {
	(void)data; (void)pt; (void)time;
	emit("axis %u %.1f", axis, wl_fixed_to_double(value));
}

static void pt_frame(void *data, struct wl_pointer *pt) {
	(void)data; (void)pt;
}

static void pt_axis_source(void *data, struct wl_pointer *pt, uint32_t source) {
	(void)data; (void)pt; (void)source;
}

static void pt_axis_stop(void *data, struct wl_pointer *pt, uint32_t time,
		uint32_t axis) {
	(void)data; (void)pt; (void)time; (void)axis;
}

static void pt_axis_discrete(void *data, struct wl_pointer *pt, uint32_t axis,
		int32_t discrete) {
	(void)data; (void)pt; (void)axis; (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pt_enter,
	.leave = pt_leave,
	.motion = pt_motion,
	.button = pt_button,
	.axis = pt_axis,
	.frame = pt_frame,
	.axis_source = pt_axis_source,
	.axis_stop = pt_axis_stop,
	.axis_discrete = pt_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *s,
		uint32_t caps) {
	(void)data; (void)s;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard == NULL) {
		keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && pointer == NULL) {
		pointer = wl_seat_get_pointer(seat);
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

/* ---------------------------------------------------------- registry */

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	(void)data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
			version >= 4 ? 4 : version);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		if (output_count >= MAX_OUTPUTS) {
			return;
		}
		struct output *o = &outputs[output_count++];
		o->wlr_output = wl_registry_bind(registry, name, &wl_output_interface,
			version >= 4 ? 4 : version);
		wl_output_add_listener(o->wlr_output, &output_listener, o);
		/* 锁定期间新插的屏：客户端有义务为它建 lock surface。 */
		make_lock_surface(o);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface,
			version >= 5 ? 5 : version);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (strcmp(interface,
			ext_session_lock_manager_v1_interface.name) == 0) {
		lock_mgr = wl_registry_bind(registry, name,
			&ext_session_lock_manager_v1_interface, 1);
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

int main(int argc, char *argv[]) {
	(void)argc; (void)argv;

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "locktest: 连不上合成器\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display); /* globals */
	wl_display_roundtrip(display); /* 每块屏的 name / mode / done */

	if (compositor == NULL || shm == NULL) {
		fprintf(stderr, "locktest: 少了 wl_compositor / wl_shm\n");
		return 1;
	}
	if (lock_mgr == NULL) {
		fprintf(stderr, "locktest: 合成器没提供 ext_session_lock_manager_v1\n");
		return 1;
	}
	if (output_count == 0) {
		fprintf(stderr, "locktest: 一块输出都没有\n");
		return 1;
	}
	for (int i = 0; i < output_count; i++) {
		emit("output %s", out_name(&outputs[i]));
	}

	lock = ext_session_lock_manager_v1_lock(lock_mgr);
	ext_session_lock_v1_add_listener(lock, &lock_listener, NULL);
	for (int i = 0; i < output_count; i++) {
		make_lock_surface(&outputs[i]);
	}
	wl_display_flush(display);

	while (running) {
		struct pollfd fds[2] = {
			{ .fd = wl_display_get_fd(display), .events = POLLIN },
			{ .fd = STDIN_FILENO, .events = POLLIN },
		};
		wl_display_dispatch_pending(display);
		wl_display_flush(display);
		if (poll(fds, 2, -1) < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
			if (wl_display_dispatch(display) < 0) {
				break;
			}
		}
		if (fds[1].revents & POLLIN) {
			char buf[64];
			ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
			if (n <= 0) {
				/* stdin 关了（测试脚本收尾）：别忙等。 */
				break;
			}
			for (ssize_t i = 0; i < n; i++) {
				if (buf[i] == 'q') {
					running = false;
				} else if (buf[i] == 'u') {
					if (!locked) {
						emit("unlock-ignored");
						continue;
					}
					ext_session_lock_v1_unlock_and_destroy(lock);
					lock = NULL;
					emit("unlock-sent");
					wl_display_flush(display);
				}
			}
		}
	}

	fprintf(stderr, "locktest: 收工（outputs=%d locked=%d）\n", output_count,
		locked);
	return 0;
}
