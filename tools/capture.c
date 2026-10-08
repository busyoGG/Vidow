/* 窗口录屏 / 预览的最小客户端：按标题挑一个窗口，连续抓它的画面，每抓到一帧打印
 *
 *   capture <n> <毫秒> <像素校验和>
 *
 * 校验和就是「这一帧的画面是不是新的」：窗口滚到视口外时，如果客户端被节流停帧，
 * 抓到的每一帧都一模一样（校验和不变）—— 录屏录到的就是一张冻住的图。vidow 那边
 * 「有人拿着这个窗口的截图源」会把它标成强制渲染（src/portal.c 的 capture_hold），
 * 于是客户端继续出帧、校验和继续变。
 *
 * 走的是 ext-image-copy-capture-v1 + ext-foreign-toplevel-list-v1 + ext-image-capture-source-v1
 * 这一套（和 xdg-desktop-portal 的窗口截图源同一条路）。
 *
 * 用法：capture <标题子串> [运行秒数] [每秒抓几帧=30]
 *
 * 默认按 30fps 抓（真录屏就是这个节奏）。**别不限制地连抓**：合成器每次抓帧都会给
 * 被录的窗口发一次帧回调，客户端于是画得比屏幕快几十倍 —— 量出来的是测试客户端的
 * 空转，不是真实录屏的行为（踩过：量到 2500fps）。
 */
#define _GNU_SOURCE

#include <errno.h>
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

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

static struct wl_display *display;
static struct wl_shm *shm;
static struct ext_foreign_toplevel_list_v1 *toplevel_list;
static struct ext_foreign_toplevel_image_capture_source_manager_v1 *source_mgr;
static struct ext_image_copy_capture_manager_v1 *capture_mgr;

static const char *want_title;
static struct ext_foreign_toplevel_handle_v1 *target;

static struct ext_image_copy_capture_session_v1 *session;
static struct ext_image_copy_capture_frame_v1 *frame;
static struct wl_buffer *buffer;
static void *pixels;
static int buf_w, buf_h;
static uint32_t shm_format = WL_SHM_FORMAT_XRGB8888;
static bool session_ready;
static int alloc_w, alloc_h;

static int captures;
static int interval_us = 33333; /* 30fps */
static struct timespec start;
static bool running = true;

static int ms_since_start(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int)((now.tv_sec - start.tv_sec) * 1000 +
		(now.tv_nsec - start.tv_nsec) / 1000000);
}

/* ------------------------------------------------------------ shm */

static int create_shm_file(size_t size) {
	int fd = memfd_create("capture", MFD_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static bool make_buffer(void) {
	if (buf_w < 1 || buf_h < 1) {
		fprintf(stderr, "capture: 还没拿到 buffer_size，建不了缓冲\n");
		return false;
	}
	int stride = buf_w * 4;
	size_t size = (size_t)stride * (size_t)buf_h;
	int fd = create_shm_file(size);
	if (fd < 0) {
		return false;
	}
	pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		close(fd);
		return false;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int)size);
	buffer = wl_shm_pool_create_buffer(pool, 0, buf_w, buf_h, stride, shm_format);
	wl_shm_pool_destroy(pool);
	close(fd);
	alloc_w = buf_w;
	alloc_h = buf_h;
	return true;
}

/* ------------------------------------------------------------ 帧 */

static void capture_next(void);

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *f) {
	(void)data;
	captures++;
	/* 校验和：画面变没变，一眼看出来 */
	uint32_t sum = 0;
	int stride = buf_w;
	const uint32_t *px = pixels;
	int step = (buf_w * buf_h) / 512 + 1;
	for (int i = 0; i < buf_w * buf_h; i += step) {
		sum = sum * 31u + px[(i / stride) * stride + (i % stride)];
	}
	printf("capture %d %d %08x\n", captures, ms_since_start(), sum);
	fflush(stdout);
	ext_image_copy_capture_frame_v1_destroy(f);
	frame = NULL;
	if (interval_us > 0) {
		usleep((useconds_t)interval_us);
	}
	capture_next();
}

static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *f,
		uint32_t reason) {
	(void)data;
	fprintf(stderr, "capture: 这一帧失败（reason=%u）\n", reason);
	ext_image_copy_capture_frame_v1_destroy(f);
	frame = NULL;
	capture_next();
}

static void frame_transform(void *data,
		struct ext_image_copy_capture_frame_v1 *f, uint32_t transform) {
	(void)data;
	(void)f;
	(void)transform;
}

static void frame_damage(void *data,
		struct ext_image_copy_capture_frame_v1 *f, int32_t x, int32_t y,
		int32_t w, int32_t h) {
	(void)data;
	(void)f;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
}

static void frame_presentation_time(void *data,
		struct ext_image_copy_capture_frame_v1 *f, uint32_t sec_hi,
		uint32_t sec_lo, uint32_t nsec) {
	(void)data;
	(void)f;
	(void)sec_hi;
	(void)sec_lo;
	(void)nsec;
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
	.transform = frame_transform,
	.damage = frame_damage,
	.presentation_time = frame_presentation_time,
	.ready = frame_ready,
	.failed = frame_failed,
};

static void capture_next(void) {
	if (!session_ready || buffer == NULL) {
		return;
	}
	frame = ext_image_copy_capture_session_v1_create_frame(session);
	ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, NULL);
	ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
	ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, buf_w, buf_h);
	ext_image_copy_capture_frame_v1_capture(frame);
}

static void session_buffer_size(void *data,
		struct ext_image_copy_capture_session_v1 *s, uint32_t width,
		uint32_t height) {
	(void)data;
	(void)s;
	buf_w = (int)width;
	buf_h = (int)height;
	fprintf(stderr, "capture: buffer_size %dx%d\n", buf_w, buf_h);
}

static void session_shm_format(void *data,
		struct ext_image_copy_capture_session_v1 *s, uint32_t format) {
	(void)data;
	(void)s;
	shm_format = format;
	fprintf(stderr, "capture: shm_format %u\n", format);
}

static void session_dmabuf_device(void *data,
		struct ext_image_copy_capture_session_v1 *s, struct wl_array *device) {
	(void)data;
	(void)s;
	(void)device;
}

static void session_dmabuf_format(void *data,
		struct ext_image_copy_capture_session_v1 *s, uint32_t format,
		struct wl_array *modifiers) {
	(void)data;
	(void)s;
	(void)format;
	(void)modifiers;
}

static void session_done(void *data,
		struct ext_image_copy_capture_session_v1 *s) {
	(void)data;
	(void)s;
	session_ready = true;
	fprintf(stderr, "capture: 约束齐了（%dx%d, format %u）\n", buf_w, buf_h,
		shm_format);
	/* 约束会**重发**：窗口尺寸 / 倍率一变，合成器就重来一遍 buffer_size +
	 * done。这时候手上那一帧可能还在飞 —— 再建一帧是协议错（wlroots 那边报
	 * "session already has a frame object"，客户端当场断线，录到的帧数是 0）。
	 * 有在飞的就等它回来，它回来会自己接着建下一帧。 */
	if (frame != NULL) {
		return;
	}
	if (buffer == NULL || buf_w != alloc_w || buf_h != alloc_h) {
		if (buffer != NULL) {
			wl_buffer_destroy(buffer);
			buffer = NULL;
			munmap(pixels, (size_t)alloc_w * (size_t)alloc_h * 4);
			pixels = NULL;
		}
		if (!make_buffer()) {
			fprintf(stderr, "capture: 建不出缓冲\n");
			return;
		}
	}
	capture_next();
}

static void session_stopped(void *data,
		struct ext_image_copy_capture_session_v1 *s) {
	(void)data;
	(void)s;
	fprintf(stderr, "capture: 会话结束了（窗口关掉或者合成器不给了）\n");
	running = false;
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
	.buffer_size = session_buffer_size,
	.shm_format = session_shm_format,
	.dmabuf_device = session_dmabuf_device,
	.dmabuf_format = session_dmabuf_format,
	.done = session_done,
	.stopped = session_stopped,
};

/* -------------------------------------------------- 窗口列表 */

static void start_capture(void);

static void handle_title(void *data, struct ext_foreign_toplevel_handle_v1 *h,
		const char *title) {
	(void)data;
	if (target != NULL || title == NULL) {
		return;
	}
	if (strstr(title, want_title) != NULL) {
		target = h;
		fprintf(stderr, "capture: 抓 %s\n", title);
	}
}

static void handle_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *h,
		const char *app_id) {
	(void)data;
	(void)h;
	(void)app_id;
}

static void handle_identifier(void *data,
		struct ext_foreign_toplevel_handle_v1 *h, const char *identifier) {
	(void)data;
	(void)h;
	(void)identifier;
}

static void handle_closed(void *data, struct ext_foreign_toplevel_handle_v1 *h) {
	(void)data;
	if (h == target) {
		fprintf(stderr, "capture: 目标窗口关了\n");
		running = false;
	}
}

/* 这个窗口的初始属性（标题 / app_id / identifier）发完了 —— 这才是「可以动手」的
 * 信号。注意不能用 ext_foreign_toplevel_list_v1 的 finished：那个事件的意思是
 * 「合成器不再往这个 list 对象发事件了」（比如合成器要退出），不是「枚举完了」
 * （协议里写得很清楚），等它等于永远等不到。 */
static void handle_done(void *data, struct ext_foreign_toplevel_handle_v1 *h) {
	(void)data;
	if (h == target && session == NULL) {
		start_capture();
	}
}

static const struct ext_foreign_toplevel_handle_v1_listener handle_listener = {
	.title = handle_title,
	.app_id = handle_app_id,
	.identifier = handle_identifier,
	.closed = handle_closed,
	.done = handle_done,
};

/* 列表给完之后，按标题挑一个窗口建截图源。 */
static void start_capture(void) {
	if (target == NULL) {
		fprintf(stderr, "capture: 没找到标题含 '%s' 的窗口\n", want_title);
		running = false;
		return;
	}
	struct ext_image_capture_source_v1 *source =
		ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
			source_mgr, target);
	session = ext_image_copy_capture_manager_v1_create_session(capture_mgr,
		source, 0);
	ext_image_capture_source_v1_destroy(source);
	ext_image_copy_capture_session_v1_add_listener(session, &session_listener,
		NULL);
}

static void list_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *list,
		struct ext_foreign_toplevel_handle_v1 *handle) {
	(void)data;
	(void)list;
	ext_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, NULL);
}

static void list_finished(void *data, struct ext_foreign_toplevel_list_v1 *list) {
	(void)data;
	(void)list;
	fprintf(stderr, "capture: 合成器收摊了（list finished）\n");
	running = false;
}

static const struct ext_foreign_toplevel_list_v1_listener list_listener = {
	.toplevel = list_toplevel,
	.finished = list_finished,
};

static const struct ext_foreign_toplevel_list_v1_listener list_listener;

/* -------------------------------------------------- registry */

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	(void)data;
	(void)version;
	if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface,
			ext_foreign_toplevel_list_v1_interface.name) == 0) {
		toplevel_list = wl_registry_bind(registry, name,
			&ext_foreign_toplevel_list_v1_interface, 1);
		/* 监听要在 bind 之后立刻挂上：finished 事件可能在同一个 roundtrip 里就
		 * 到了，晚一步挂就收不到（第一次写就是这么卡住的：list 的 finished 丢了，
		 * 于是永远没去建截图源）。 */
		ext_foreign_toplevel_list_v1_add_listener(toplevel_list,
			&list_listener, NULL);
	} else if (strcmp(interface,
			ext_foreign_toplevel_image_capture_source_manager_v1_interface.name)
			== 0) {
		source_mgr = wl_registry_bind(registry, name,
			&ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
	} else if (strcmp(interface,
			ext_image_copy_capture_manager_v1_interface.name) == 0) {
		capture_mgr = wl_registry_bind(registry, name,
			&ext_image_copy_capture_manager_v1_interface, 1);
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
	want_title = argc > 1 ? argv[1] : "frames";
	int limit_ms = argc > 2 ? atoi(argv[2]) * 1000 : 0;
	if (argc > 3) {
		int fps = atoi(argv[3]);
		interval_us = fps > 0 ? 1000000 / fps : 0;
	}
	clock_gettime(CLOCK_MONOTONIC, &start);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "capture: 连不上合成器\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (shm == NULL || toplevel_list == NULL || source_mgr == NULL ||
			capture_mgr == NULL) {
		fprintf(stderr, "capture: 合成器没给全需要的协议\n");
		return 1;
	}
	/* 列表的 toplevel / finished 事件靠 registry 里挂的那个监听收（见上）。 */
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);

	while (running) {
		if (wl_display_dispatch(display) < 0) {
			break;
		}
		if (limit_ms > 0 && ms_since_start() > limit_ms) {
			break;
		}
	}
	fprintf(stderr, "capture: 一共 %d 帧\n", captures);
	return 0;
}
