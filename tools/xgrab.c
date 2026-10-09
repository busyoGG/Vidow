/* xgrab：给 XWayland 测试用的最小 X11 客户端 —— 造一个普通窗口，然后用
 * XGrabPointer(confine_to=自己) 把指针**夹**在窗口里，再把收到的 MotionNotify
 * 一行行打出来。
 *
 * 为什么需要它：`zwp_pointer_constraints_v1` 缺失时，Xwayland 的
 * `confine_pointer` 请求发不出去（`/usr/bin/Xwayland` 里那两个字符串
 * `lock_pointer` / `confine_pointer` 就是这两个请求），X11 客户端的 confine 抓取
 * 只能自己 XWarpPointer 兜。有了这个协议，Xwayland 会替客户端在窗口的
 * wl_surface 上建一个 `zwp_confined_pointer_v1` —— 合成器那边 `vidowmsg input`
 * 的 `pointer_constraint` 会当场变成 confined，光标也就出不了窗口。
 *
 * 用法：xgrab [标题] [noconfine]
 *   grabbed <status>   抓取结果（0 = 成功）
 *   motion <x> <y>     窗口内坐标的指针移动
 *   configure <x> <y> <w> <h>
 *
 * 测试断言：抓取成功后 `vidowmsg input` 里 pointer_constraint.type = confined，
 * 并且把光标往窗口外推的时候，收到的 motion 坐标一直在窗口尺寸之内。 */
/* usleep / memfd_create 要 _GNU_SOURCE（项目全局按 POSIX 编） */
#define _GNU_SOURCE

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xcb/xcb.h>

static bool running = true;

static void on_signal(int signo) {
	(void)signo;
	running = false;
}

int main(int argc, char *argv[]) {
	const char *title = argc > 1 ? argv[1] : "xgrab";
	/* noconfine：XGrabPointer 不带 confine_to（对照用：普通抓取不该把指针
	 * 钉住/夹住，Xwayland 也不该为此建约束）。 */
	bool confine = !(argc > 2 && strcmp(argv[2], "noconfine") == 0);
	unsigned w = 400, h = 300;

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	int screen_num = 0;
	xcb_connection_t *conn = xcb_connect(NULL, &screen_num);
	if (xcb_connection_has_error(conn)) {
		fprintf(stderr, "xgrab: 连不上 X（DISPLAY=%s）\n",
			getenv("DISPLAY") != NULL ? getenv("DISPLAY") : "(空)");
		return 1;
	}
	const xcb_setup_t *setup = xcb_get_setup(conn);
	xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
	for (int i = 0; i < screen_num; i++) {
		xcb_screen_next(&it);
	}
	xcb_screen_t *screen = it.data;
	if (screen == NULL) {
		fprintf(stderr, "xgrab: 没有屏幕\n");
		return 1;
	}

	xcb_window_t win = xcb_generate_id(conn);
	uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	uint32_t values[] = {
		0x226699,
		XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
			XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_PRESS |
			XCB_EVENT_MASK_BUTTON_RELEASE,
	};
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root, 0, 0, w, h,
		0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NAME,
		XCB_ATOM_STRING, 8, (uint32_t)strlen(title), title);
	xcb_map_window(conn, win);
	xcb_flush(conn);

	/* 等窗口真映射出来（vidow 的布局摆好、Xwayland 把 surface 建出来），
	 * 不然 confine 请求会打在还不存在的表面上。 */
	for (int i = 0; i < 200 && running; i++) {
		xcb_generic_event_t *ev = xcb_poll_for_event(conn);
		if (ev != NULL) {
			uint8_t type = ev->response_type & 0x7f;
			if (type == XCB_MAP_NOTIFY) {
				free(ev);
				break;
			}
			free(ev);
		}
		usleep(10000);
	}

	/* 指针先挪到窗口里再抓（X11 的 confine 语义：抓取时指针已经在窗口里）。 */
	xcb_warp_pointer(conn, XCB_NONE, win, 0, 0, 0, 0, (int16_t)(w / 2),
		(int16_t)(h / 2));
	xcb_flush(conn);
	usleep(300000);

	xcb_grab_pointer_reply_t *reply = xcb_grab_pointer_reply(conn,
		xcb_grab_pointer(conn, 0, win,
			XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_PRESS |
				XCB_EVENT_MASK_BUTTON_RELEASE,
			XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
			confine ? win : XCB_NONE, XCB_NONE, XCB_CURRENT_TIME), NULL);
	uint8_t status = reply != NULL ? reply->status : 255;
	free(reply);
	printf("grabbed %u %s\n", status, confine ? "confine" : "noconfine");
	fflush(stdout);

	while (running) {
		xcb_generic_event_t *ev = xcb_poll_for_event(conn);
		if (ev == NULL) {
			usleep(20000);
			continue;
		}
		uint8_t type = ev->response_type & 0x7f;
		if (type == XCB_MOTION_NOTIFY) {
			xcb_motion_notify_event_t *m = (xcb_motion_notify_event_t *)ev;
			printf("motion %d %d\n", m->event_x, m->event_y);
			fflush(stdout);
		} else if (type == XCB_CONFIGURE_NOTIFY) {
			xcb_configure_notify_event_t *c = (xcb_configure_notify_event_t *)ev;
			printf("configure %d %d %u %u\n", c->x, c->y, c->width,
				c->height);
			fflush(stdout);
		}
		free(ev);
	}

	xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
	xcb_flush(conn);
	xcb_disconnect(conn);
	return 0;
}
