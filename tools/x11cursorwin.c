/* x11cursorwin —— 一个普通 X11 窗口，从 stdin 读命令换它自己的窗口光标。
 *
 * 给 test-x11cursor-none.sh 用：区分「X11 窗口没设光标（继承 root 的透明
 * 1x1）」和「全屏游戏自己画光标」这两种 set_cursor(NULL)。
 *
 * stdin 命令（每行一条）：
 *   hand   窗口光标 = XC_hand2（一支真实的客户端光标图）
 *   none   窗口光标 = None（X 沿继承链落到 root 的透明占位）
 *   quit   退出
 *
 * 每处理一条打一行，方便 shell 等它。窗口标题固定 "x11cursorwin"。 */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/cursorfont.h>

int main(void) {
	Display *dpy = XOpenDisplay(NULL);
	if (dpy == NULL) {
		fprintf(stderr, "x11cursorwin: 连不上 X（DISPLAY=%s）\n",
			getenv("DISPLAY") != NULL ? getenv("DISPLAY") : "?");
		return 1;
	}
	int screen = DefaultScreen(dpy);
	Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
		400, 300, 0, BlackPixel(dpy, screen), WhitePixel(dpy, screen));
	XStoreName(dpy, win, "x11cursorwin");
	XSelectInput(dpy, win, ExposureMask | StructureNotifyMask);
	XMapWindow(dpy, win);
	XFlush(dpy);
	printf("mapped\n");
	fflush(stdout);

	for (;;) {
		while (XPending(dpy) > 0) {
			XEvent ev;
			XNextEvent(dpy, &ev);
		}
		struct pollfd p = { .fd = STDIN_FILENO, .events = POLLIN };
		if (poll(&p, 1, 100) <= 0 || !(p.revents & POLLIN)) {
			continue;
		}
		char line[64];
		if (fgets(line, sizeof(line), stdin) == NULL) {
			break;
		}
		if (strncmp(line, "hand", 4) == 0) {
			Cursor c = XCreateFontCursor(dpy, XC_hand2);
			XDefineCursor(dpy, win, c);
			XFreeCursor(dpy, c);
			XFlush(dpy);
			printf("hand\n");
			fflush(stdout);
		} else if (strncmp(line, "none", 4) == 0) {
			XDefineCursor(dpy, win, None);
			XFlush(dpy);
			printf("none\n");
			fflush(stdout);
		} else {
			break;
		}
	}
	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);
	return 0;
}
