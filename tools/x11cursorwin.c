/* x11cursorwin —— 一个普通 X11 窗口，从 stdin 读命令换它自己的窗口光标。
 *
 * 给 test-x11cursor-none.sh 用：区分「X11 窗口没设光标（继承 root 的透明
 * 1x1）」和「全屏游戏自己画光标」这两种 set_cursor(NULL)。
 *
 * stdin 命令（每行一条）：
 *   hand     窗口光标 = XC_hand2（一支真实的客户端光标图，16x16）
 *   box N [hx hy]
 *            窗口光标 = 一支 N x N 的方块（边框 + 对角交叉），热点默认 0,0。
 *            和 hand2 这种字体光标不同，尺寸完全由 N 定：用来验「X11 光标
 *            pixmap 的像素 = 设备像素」（2 倍屏上不许再乘 2、也不许被硬件
 *            光标平面裁掉）。
 *   none     窗口光标 = None（X 沿继承链落到 root 的透明占位）
 *   quit     退出
 *
 * 每处理一条打一行，方便 shell 等它。窗口标题固定 "x11cursorwin"。 */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/cursorfont.h>

static Cursor box_cursor(Display *dpy, Window win, int size, int hx, int hy) {
	int screen = DefaultScreen(dpy);
	Pixmap pm = XCreatePixmap(dpy, RootWindow(dpy, screen), size, size, 1);
	GC g = XCreateGC(dpy, pm, 0, NULL);
	XSetForeground(dpy, g, 1);
	int edge = size / 16;
	if (edge < 2) {
		edge = 2;
	}
	if (edge > 6) {
		edge = 6;
	}
	XFillRectangle(dpy, pm, g, 0, 0, size, edge);
	XFillRectangle(dpy, pm, g, 0, size - edge, size, edge);
	XFillRectangle(dpy, pm, g, 0, 0, edge, size);
	XFillRectangle(dpy, pm, g, size - edge, 0, edge, size);
	int d = (int)(size * 0.7);
	XSetLineAttributes(dpy, g, 1, LineSolid, CapNotLast, JoinMiter);
	XDrawLine(dpy, pm, g, d, d, size - 1 - d, size - 1 - d);
	XDrawLine(dpy, pm, g, d, size - 1 - d, size - 1 - d, d);
	XFreeGC(dpy, g);
	Cursor c = XCreatePixmapCursor(dpy, pm, pm,
		&(XColor){.red = 0xffff, .green = 0, .blue = 0},
		&(XColor){.red = 0xffff, .green = 0, .blue = 0}, hx, hy);
	XFreePixmap(dpy, pm);
	XDefineCursor(dpy, win, c);
	XFlush(dpy);
	return c;
}

int main(void) {
	Display *dpy = XOpenDisplay(NULL);
	if (dpy == NULL) {
		fprintf(stderr, "x11cursorwin: 连不上 X（DISPLAY=%s）\n",
			getenv("DISPLAY") != NULL ? getenv("DISPLAY") : "?");
		return 1;
	}
	int size = 16, hx = 0, hy = 0;
	Cursor box = 0;
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
		} else if (sscanf(line, "box %d %d %d", &size, &hx, &hy) >= 1) {
			if (size < 1) {
				size = 16;
			}
			if (size > 512) {
				size = 512;
			}
			if (hx < 0 || hx >= size) {
				hx = 0;
			}
			if (hy < 0 || hy >= size) {
				hy = 0;
			}
			if (box != 0) {
				XFreeCursor(dpy, box);
			}
			box = box_cursor(dpy, win, size, hx, hy);
			printf("box %d %d %d\n", size, hx, hy);
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
	if (box != 0) {
		XFreeCursor(dpy, box);
	}
	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);
	return 0;
}
