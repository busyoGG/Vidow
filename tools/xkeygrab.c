/* xkeygrab：给 XWayland 测试用的最小 X11 客户端 —— 造一个普通窗口，然后
 * `XGrabKeyboard` 把键盘抓过来，把收到的 KeyPress / KeyRelease 一行行打出来。
 *
 * 为什么需要它：`zwp_xwayland_keyboard_grab_manager_v1` 缺失时，X11 的
 * `XGrabKeyboard` 传不到合成器（`/usr/bin/Xwayland` 里就有这个接口名），
 * Xwayland 只能在 X 那边假装抓到了：焦点一换（用户点了别的窗口）按键就跑到别处
 * 去，抓着键盘的那个客户端再也收不到键。有了这个协议，Xwayland 会在窗口的
 * wl_surface 上建一个 `zwp_xwayland_keyboard_grab_v1` —— 合成器那边
 * `vidowmsg input` 的 `keyboard_grab` 当场变成 `{type: xwayland, window: N}`，
 * 之后**所有**按键都送到这个客户端（焦点在别的窗口上也照送，X11 就是这么定的）。
 *
 * 用法：xkeygrab [标题] [nograb]
 *   grabkey <status>        XGrabKeyboard 的结果（0 = GrabSuccess）
 *   key <keycode> pressed|released
 *   keycount <n>            收到多少个按键事件
 *   focus in|out            X 那边的焦点进出
 *   ungrabbed               收到 SIGUSR1 之后 XUngrabKeyboard 了
 *
 * 测试断言（./test-keyboard-grab.sh）：抓取成功后 `vidowmsg input` 里
 * keyboard_grab.window = 这个窗口，并且焦点换到别的窗口之后按键照样到它手上。 */
/* usleep 要 _GNU_SOURCE（项目全局按 POSIX 编） */
#define _GNU_SOURCE

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xcb/xcb.h>

static volatile sig_atomic_t running = true;
static volatile sig_atomic_t ungrab_requested = false;

static void on_signal(int signo) {
	if (signo == SIGUSR1) {
		ungrab_requested = true;
		return;
	}
	running = false;
}

int main(int argc, char *argv[]) {
	const char *title = argc > 1 && strcmp(argv[1], "nograb") != 0 ?
		argv[1] : "xkeygrab";
	bool grab = !(argc > 1 && strcmp(argv[1], "nograb") == 0) &&
		!(argc > 2 && strcmp(argv[2], "nograb") == 0);
	unsigned w = 400, h = 300;

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGUSR1, on_signal);

	int screen_num = 0;
	xcb_connection_t *conn = xcb_connect(NULL, &screen_num);
	if (xcb_connection_has_error(conn)) {
		fprintf(stderr, "xkeygrab: 连不上 X（DISPLAY=%s）\n",
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
		fprintf(stderr, "xkeygrab: 没有屏幕\n");
		return 1;
	}

	xcb_window_t win = xcb_generate_id(conn);
	uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	uint32_t values[] = {
		0x669922,
		XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
			XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
			XCB_EVENT_MASK_FOCUS_CHANGE,
	};
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root, 0, 0, w, h,
		0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NAME,
		XCB_ATOM_STRING, 8, (uint32_t)strlen(title), title);
	xcb_map_window(conn, win);
	xcb_flush(conn);

	/* 等窗口真映射出来（vidow 的布局摆好、Xwayland 把 surface 建出来），
	 * 不然 grab_keyboard 会打在还不存在的表面上。 */
	for (int i = 0; i < 200 && running; i++) {
		xcb_generic_event_t *ev = xcb_poll_for_event(conn);
		if (ev != NULL) {
			uint8_t type = ev->response_type & 0x7f;
			free(ev);
			if (type == XCB_MAP_NOTIFY) {
				break;
			}
		}
		usleep(10000);
	}

	uint8_t status = 255;
	if (grab) {
		xcb_grab_keyboard_reply_t *reply = xcb_grab_keyboard_reply(conn,
			xcb_grab_keyboard(conn, 1, win, XCB_CURRENT_TIME,
				XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC), NULL);
		status = reply != NULL ? reply->status : 255;
		free(reply);
		printf("grabkey %u\n", status);
	} else {
		printf("grabkey skip\n");
	}
	fflush(stdout);

	int key_count = 0;
	while (running) {
		xcb_generic_event_t *ev = xcb_poll_for_event(conn);
		if (ungrab_requested) {
			ungrab_requested = false;
			if (grab) {
				grab = false;
				xcb_ungrab_keyboard(conn, XCB_CURRENT_TIME);
				xcb_flush(conn);
				printf("ungrabbed\n");
				fflush(stdout);
			}
		}
		if (ev == NULL) {
			usleep(20000);
			continue;
		}
		uint8_t type = ev->response_type & 0x7f;
		if (type == XCB_KEY_PRESS || type == XCB_KEY_RELEASE) {
			xcb_key_press_event_t *k = (xcb_key_press_event_t *)ev;
			key_count++;
			printf("key %u %s\n", k->detail,
				type == XCB_KEY_PRESS ? "pressed" : "released");
			printf("keycount %d\n", key_count);
			fflush(stdout);
		} else if (type == XCB_FOCUS_IN || type == XCB_FOCUS_OUT) {
			printf("focus %s\n", type == XCB_FOCUS_IN ? "in" : "out");
			fflush(stdout);
		}
		free(ev);
	}

	xcb_ungrab_keyboard(conn, XCB_CURRENT_TIME);
	xcb_flush(conn);
	xcb_disconnect(conn);
	return 0;
}
