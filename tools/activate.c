/* 激活请求客户端：一个窗口 + 一次 xdg-activation 请求。
 *
 * 「点通知，窗口该跳出来并拿到焦点」这条链路要能测，就得有个客户端**自己请求
 * 激活**。真实场景是：应用（或通知守护进程）在几十分钟前弹通知时拿了一个激活
 * 令牌，你现在点它，它才用那个令牌请求把自己顶上来 —— 令牌过期了 wlroots 会
 * 直接丢掉请求，合成器连信号都收不到。
 *
 * 用法：activate <app_id> <标题> [延迟秒数] [--serial=click|zero|none]
 *   延迟秒数 = 上屏之后等多久才**用令牌请求激活**（0 = 上屏就请求）。令牌本身
 *   在上屏时就创建了 —— 延迟多少秒就等于「令牌放了多久才用」，过期那条判据靠它。
 *   --serial=click  用鼠标点在这个窗口上的那次事件的 serial（真实应用走这条）
 *   --serial=zero   set_serial(0, seat)（有些工具包就这么发 —— wlroots 会拒！）
 *   --serial=none   根本不调 set_serial（协议允许，wlroots 放行）
 *   --before-map    窗口还没上屏就先请求激活（客户端 commit 出表面、还没交第一帧
 *                   就请求 —— 合成器得把这个请求记着，等窗口出来再兑现）
 *   --drop-surface=<秒>  用令牌之后，把这个窗口的表面销毁掉（客户端还活着）：
 *                   令牌上 set_surface 指的 surface 先没了，合成器不能崩
 *                   （wlroots 在 surface 资源销毁时会断言 destroy 监听器链表为空）
 *
 * 输出（一行一条，方便断言）：
 *   activate ready <毫秒>                    窗口上屏了
 *   activate serial <号>（<来源>）           请求里用的 serial
 *   activate token <名字>                    拿到令牌（= 弹通知的那一刻）
 *   activate committed <毫秒>                拿令牌请求激活（= 点通知的那一刻）
 *
 * 画面是四象限纯色（跟 tools/pattern.py 一样），这样截图里能按点认出「它现在画在
 * 屏幕上没有」。 */
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

#include "xdg-activation-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct xdg_activation_v1 *activation;

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
static struct wl_buffer *buffer;
static uint32_t *pixels32;
static int width = 360, height = 240;

static const char *app_id = "activate";
static const char *title = "激活";
static int delay_ms;
/* 请求里带什么 serial：click = 点在这个窗口上的那次事件（真实应用）、
 * zero = set_serial(0, seat)（工具包偷懒的写法）、none = 不带 serial。 */
enum serial_mode {
	SERIAL_CLICK,
	SERIAL_ZERO,
	SERIAL_NONE,
};
static enum serial_mode serial_mode = SERIAL_CLICK;
static uint32_t last_click_serial;
static struct wl_pointer *pointer;
static bool ready;
static bool requested;
static bool token_ready;
static bool activated;
static bool before_map;
/* 令牌上不带 set_surface —— 真实的通知守护进程就是这么干的：它只是把令牌塞进
 * 子进程的环境变量，被激活的 surface 由应用自己在 activate 里给。 */
static bool no_token_surface;
/* 用令牌之后过几毫秒把这个窗口的表面销毁掉（-1 = 不销毁）。客户端不断开连接。 */
static int drop_ms = -1;
static bool dropped;
static char token_saved[64];
static struct timespec start;

static int ms_since_start(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int)((now.tv_sec - start.tv_sec) * 1000 +
		(now.tv_nsec - start.tv_nsec) / 1000000);
}

static int create_shm_file(size_t size) {
	int fd = memfd_create("activate", MFD_CLOEXEC);
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

/* 四象限：左上红、右上绿、左下蓝、右下白（跟 quad_check 的采样点对齐）。 */
static void fill_pattern(void) {
	uint32_t quad[4] = {
		0xffff0000u, /* 左上 红 */
		0xff00ff00u, /* 右上 绿 */
		0xff0000ffu, /* 左下 蓝 */
		0xffffffffu, /* 右下 白 */
	};
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			int q = (y < height / 2 ? 0 : 2) + (x < width / 2 ? 0 : 1);
			pixels32[y * width + x] = quad[q];
		}
	}
}

static void draw(void) {
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	wl_surface_commit(surface);
}

/* ------------------------------------------------------------ 激活请求 */

static void use_token(void);

static void token_done(void *data, struct xdg_activation_token_v1 *token,
		const char *token_str) {
	(void)data;
	if (token_ready) {
		return;
	}
	token_ready = true;
	snprintf(token_saved, sizeof(token_saved), "%s", token_str);
	printf("activate token %s\n", token_str);
	fflush(stdout);
	xdg_activation_token_v1_destroy(token);
	if (before_map) {
		/* 还没上屏就要请求：立刻用它。 */
		use_token();
	}
}

/* 拿令牌去激活一个**表面**（这里就是自己的窗口 —— 应用点通知之后把自己顶上来
 * 就是这个动作）。令牌是**上屏时**就创建好的：真实应用是弹通知那一刻创建的，
 * 过了几分钟你才点 —— 令牌要是过期了，这一下就白按了。 */
static void use_token(void) {
	if (!token_ready || activated) {
		return;
	}
	activated = true;
	xdg_activation_v1_activate(activation, token_saved, surface);
	printf("activate committed %d\n", ms_since_start());
	fflush(stdout);
}

static const struct xdg_activation_token_v1_listener token_listener = {
	.done = token_done,
};

/* 现在能不能建令牌了：--serial=click 要等真的被点一下（真实应用就是拿「用户
 * 刚点了我」这个事件的 serial 去建令牌的，没有它就发不出合法请求）。 */
static bool serial_ready(void) {
	return serial_mode != SERIAL_CLICK || last_click_serial != 0;
}

/* 创建令牌（= 应用弹通知的那一刻） */
static void make_token(void) {
	if (activation == NULL || requested || !serial_ready()) {
		return;
	}
	requested = true;
	struct xdg_activation_token_v1 *token =
		xdg_activation_v1_get_activation_token(activation);
	xdg_activation_token_v1_add_listener(token, &token_listener, NULL);
	xdg_activation_token_v1_set_app_id(token, app_id);
	if (serial_mode == SERIAL_CLICK && seat != NULL) {
		/* 「谁触发的」：用点在这个窗口上的那次事件的 serial —— 真实应用就是
		 * 这么发的（没有点击就退化成 0，wlroots 会拒，日志里看得出来）。 */
		printf("activate serial %u（点击事件）\n", last_click_serial);
		fflush(stdout);
		xdg_activation_token_v1_set_serial(token, last_click_serial, seat);
	} else if (serial_mode == SERIAL_ZERO && seat != NULL) {
		printf("activate serial 0（set_serial(0, seat)）\n");
		fflush(stdout);
		xdg_activation_token_v1_set_serial(token, 0, seat);
	} else {
		printf("activate serial -（不带 serial）\n");
		fflush(stdout);
	}
	if (!no_token_surface) {
		xdg_activation_token_v1_set_surface(token, surface);
	} else {
		printf("activate token-surface 不带（守护进程式：只在 activate 里带）\n");
		fflush(stdout);
	}
	xdg_activation_token_v1_commit(token);
}

/* 把窗口的表面销毁掉，客户端**不断开连接**：令牌上 set_surface 指的就是它，
 * 合成器得受得住「令牌还在、它指的 surface 没了」。 */
static void drop_surface(void) {
	dropped = true;
	if (toplevel != NULL) {
		xdg_toplevel_destroy(toplevel);
		toplevel = NULL;
	}
	if (xdg_surface != NULL) {
		xdg_surface_destroy(xdg_surface);
		xdg_surface = NULL;
	}
	if (surface != NULL) {
		wl_surface_destroy(surface);
		surface = NULL;
	}
	printf("activate surface-dropped %d\n", ms_since_start());
	fflush(stdout);
}

/* ------------------------------------------------------------- 指针事件 */

static void pointer_enter(void *data, struct wl_pointer *pointer,
		uint32_t serial, struct wl_surface *surface, wl_fixed_t sx,
		wl_fixed_t sy) {
	(void)data;
	(void)pointer;
	(void)serial;
	(void)surface;
	(void)sx;
	(void)sy;
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
		uint32_t serial, struct wl_surface *surface) {
	(void)data;
	(void)pointer;
	(void)serial;
	(void)surface;
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
		uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
	(void)data;
	(void)pointer;
	(void)time;
	(void)sx;
	(void)sy;
}

static void pointer_button(void *data, struct wl_pointer *pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	(void)data;
	(void)pointer;
	(void)time;
	(void)button;
	/* 点击事件带 serial：这就是「用户刚点了我」的凭据，激活请求要带它。 */
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		last_click_serial = serial;
		printf("activate click %u\n", serial);
		fflush(stdout);
	}
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
		uint32_t axis, wl_fixed_t value) {
	(void)data;
	(void)pointer;
	(void)time;
	(void)axis;
	(void)value;
}

static void pointer_frame(void *data, struct wl_pointer *pointer) {
	(void)data;
	(void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer,
		uint32_t source) {
	(void)data;
	(void)pointer;
	(void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer,
		uint32_t time, uint32_t axis) {
	(void)data;
	(void)pointer;
	(void)time;
	(void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
		uint32_t axis, int32_t discrete) {
	(void)data;
	(void)pointer;
	(void)axis;
	(void)discrete;
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

static void seat_capabilities(void *data, struct wl_seat *seat,
		uint32_t caps) {
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && pointer == NULL) {
		pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
	(void)data;
	(void)seat;
	(void)name;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ------------------------------------------------------------ xdg-shell */

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
		uint32_t serial) {
	(void)data;
	xdg_surface_ack_configure(xdg_surface, serial);
	if (!ready) {
		ready = true;
		draw();
		printf("activate ready %d\n", ms_since_start());
		fflush(stdout);
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
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	} else if (strcmp(interface, xdg_activation_v1_interface.name) == 0) {
		activation = wl_registry_bind(registry, name,
			&xdg_activation_v1_interface, 1);
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
	if (argc > argi) {
		app_id = argv[argi++];
	}
	if (argc > argi) {
		title = argv[argi++];
	}
	if (argc > argi && argv[argi][0] != '-') {
		delay_ms = atoi(argv[argi++]) * 1000;
	}
	for (; argi < argc; argi++) {
		if (strncmp(argv[argi], "--serial=", 9) == 0) {
			const char *mode = argv[argi] + 9;
			if (strcmp(mode, "zero") == 0) {
				serial_mode = SERIAL_ZERO;
			} else if (strcmp(mode, "none") == 0) {
				serial_mode = SERIAL_NONE;
			} else {
				serial_mode = SERIAL_CLICK;
			}
		} else if (strcmp(argv[argi], "--no-serial") == 0) {
			serial_mode = SERIAL_NONE;
		} else if (strcmp(argv[argi], "--before-map") == 0) {
			before_map = true;
		} else if (strcmp(argv[argi], "--no-token-surface") == 0) {
			no_token_surface = true;
		} else if (strncmp(argv[argi], "--drop-surface=", 15) == 0) {
			drop_ms = atoi(argv[argi] + 15) * 1000;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &start);

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "activate: 连不上合成器\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (compositor == NULL || shm == NULL || wm_base == NULL) {
		fprintf(stderr, "activate: 少了 compositor / shm / xdg_wm_base\n");
		return 1;
	}
	if (activation == NULL) {
		fprintf(stderr, "activate: 合成器没有 xdg_activation_v1\n");
		return 1;
	}

	int stride = width * 4;
	int size = stride * height;
	int fd = create_shm_file((size_t)size);
	if (fd < 0) {
		fprintf(stderr, "activate: shm 建不出来: %s\n", strerror(errno));
		return 1;
	}
	void *pixels = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
		MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		fprintf(stderr, "activate: mmap 失败: %s\n", strerror(errno));
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
	xdg_toplevel_set_title(toplevel, title);
	xdg_toplevel_set_app_id(toplevel, app_id);
	wl_surface_commit(surface);
	if (before_map) {
		/* 表面建出来了但还没交第一帧（还没 map）：现在就拿令牌并请求激活。 */
		make_token();
		wl_display_flush(display);
	}

	/* 事件循环要能「到点了就发激活请求」，所以不能干等事件：poll 带 50ms 超时，
	 * 超时了也回来检查一次时间。 */
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
		if (ready && !requested) {
			make_token();   /* 上屏（且拿到 serial）：拿令牌（=「弹了通知」） */
			wl_display_flush(display);
		}
		if (ready && token_ready && !activated &&
				ms_since_start() >= delay_ms) {
			use_token();    /* 到点：拿令牌去请求激活（相当于「点了通知」） */
			wl_display_flush(display);
		}
		if (drop_ms >= 0 && !dropped && ms_since_start() >= drop_ms) {
			drop_surface();
			wl_display_flush(display);
		}
	}
	return 0;
}
