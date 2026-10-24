/*
 * fxlock-probe：只读取证用的独立 XCB 客户端。
 *
 * 目的：和 tools/xpopup.c 划清界限 —— xpopup 的 Expose / ConfigureNotify
 * 处理都会整窗重涂（见 xpopup.c 的 expose / CONFIGURE_NOTIFY 分支），
 * 那两条路径本身就会把测试色覆盖掉，不能拿来证明「合成器有没有重贴图」。
 * 这个探针不订阅任何会重涂的事件：
 *
 *   - 不选 StructureNotify / ExposureMask；Expose / ConfigureNotify 收不到。
 *   - 初始一帧用 create_window 的 background_pixel 画出「首帧色」。
 *   - 延迟 ms 之后，用 XPutImage 把一块「次帧色」画到原点，然后**不发任何
 *     新请求**，让 X 服务器自然地把这次绘制合并进下一次缓冲提交。
 *
 * 每一步都往 stderr 打时间戳，便于和合成器的 bake 日志对齐。
 *
 * 用法: fxlock-probe [--delay-ms N] [--w W] [--h H] [--x X] [--y Y]
 *                    [--first RRGGBB] [--second RRGGBB] [--tag NAME]
 * 退出: 保持运行；SIGTERM / SIGINT 退出。
 */
#include <xcb/xcb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int64_t now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int64_t t0;

static void mark(const char *what) {
	int64_t us = now_us() - t0;
	fprintf(stderr, "[probe %lld.%03lld] %s\n",
		(long long)(us / 1000000), (long long)(us % 1000000 / 1000), what);
	fflush(stderr);
}

static unsigned long rgb(xcb_connection_t *conn, int screen_idx, uint32_t hex) {
	return (unsigned long)(hex & 0xffffff); /* TrueColor 24bpp: 与像素值同构 */
}

int main(int argc, char **argv) {
	int delay_ms = 3000;
	int w = 400, h = 300, x = 100, y = 100;
	uint32_t first = 0x0000c0, second = 0x00c000;
	const char *tag = "probe";
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--delay-ms") && i + 1 < argc) {
			delay_ms = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--w") && i + 1 < argc) {
			w = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--h") && i + 1 < argc) {
			h = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--x") && i + 1 < argc) {
			x = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--y") && i + 1 < argc) {
			y = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--first") && i + 1 < argc) {
			first = (uint32_t)strtoul(argv[++i], NULL, 16);
		} else if (!strcmp(argv[i], "--second") && i + 1 < argc) {
			second = (uint32_t)strtoul(argv[++i], NULL, 16);
		} else if (!strcmp(argv[i], "--tag") && i + 1 < argc) {
			tag = argv[++i];
		}
	}

	t0 = now_us();
	xcb_connection_t *conn = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(conn)) {
		fprintf(stderr, "fxlock-probe: 连不上 X\n");
		return 1;
	}
	const xcb_setup_t *setup = xcb_get_setup(conn);
	xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;

	xcb_window_t win = xcb_generate_id(conn);
	/*
	 * 关键：event_mask = 0。不要 StructureNotify、不要 ExposureMask，
	 * 这样服务端不会给本客户端送 Expose / ConfigureNotify —— 探针自己
	 * 永远不会因为事件而重绘，屏幕上出现「次帧色」只能是它主动画的那一次。
	 */
	uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	uint32_t values[2] = {
		(uint32_t)rgb(conn, 0, first),
		0, /* 不订阅任何事件 */
	};
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root,
		(int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win,
		XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, (uint32_t)strlen(tag), tag);

	xcb_map_window(conn, win);
	xcb_flush(conn);
	mark("mapped (first frame = background_pixel)");
	printf("%u\n", win);
	fflush(stdout);

	struct timespec ts = { delay_ms / 1000, (long)(delay_ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) == -1 && ts.tv_sec + ts.tv_nsec > 0) {
	}

	/* 次帧：一块和窗口等大的实色，走 XPutImage（纯服务端绘制）。
	 * 之后不再发任何请求，也不再处理任何事件 —— 客户端侧到此为止。 */
	size_t stride = (size_t)w * 4;
	size_t bytes = stride * (size_t)h;
	uint8_t *px = malloc(bytes);
	if (px == NULL) {
		fprintf(stderr, "fxlock-probe: 内存不够\n");
		return 1;
	}
	uint8_t r = (uint8_t)((second >> 16) & 0xff);
	uint8_t g = (uint8_t)((second >> 8) & 0xff);
	uint8_t b = (uint8_t)(second & 0xff);
	for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
		px[i * 4 + 0] = b; /* 小端 BGRX */
		px[i * 4 + 1] = g;
		px[i * 4 + 2] = r;
		px[i * 4 + 3] = 0xff;
	}
	xcb_gcontext_t gc = xcb_generate_id(conn);
	uint32_t fg = (uint32_t)rgb(conn, 0, second);
	xcb_create_gc(conn, gc, win, XCB_GC_FOREGROUND, &fg);
	xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, win, gc,
		(uint16_t)w, (uint16_t)h, 0, 0, 0, screen->root_depth,
		(uint32_t)bytes, px);
	xcb_flush(conn);
	free(px);
	mark("put_image issued (second frame), no further requests, no event subscription");

	/* 保持存活，断开即销毁窗口。 */
	for (;;) {
		pause();
	}
	return 0;
}
