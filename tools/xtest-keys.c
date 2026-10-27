/* xtest-keys —— 记录 X11 客户端实际收到的按键事件序列。
 *
 * 用途：不打开游戏，直接观察「按住 A+W 时 X server 发给客户端的是什么」。
 * 游戏（X11 程序）看到的就是这里打印的东西 —— 如果这里出现交替的
 * KeyRelease/KeyPress 对，游戏角色就会抖。
 *
 * 编译：
 *   cc -O2 -o build/xtest-keys tools/xtest-keys.c -lX11
 *
 * 用法：
 *   ./build/xtest-keys [秒数]        # 默认 6 秒
 *
 * 输出每行：时间(ms) 事件 键码 keysym 名称
 *
 * 窗口会自己抢焦点（XSetInputFocus），所以不需要点它。
 */
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static double now_ms(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

int main(int argc, char **argv) {
	int seconds = argc > 1 ? atoi(argv[1]) : 6;
	if (seconds <= 0) {
		seconds = 6;
	}

	Display *dpy = XOpenDisplay(NULL);
	if (dpy == NULL) {
		fprintf(stderr, "xtest-keys: 连不上 X server（DISPLAY=%s）\n",
			getenv("DISPLAY") ? getenv("DISPLAY") : "(空)");
		return 1;
	}

	int screen = DefaultScreen(dpy);
	Window root = RootWindow(dpy, screen);

	XSetWindowAttributes attrs;
	attrs.background_pixel = WhitePixel(dpy, screen);
	attrs.event_mask = KeyPressMask | KeyReleaseMask | FocusChangeMask |
		StructureNotifyMask;
	Window win = XCreateWindow(dpy, root, 0, 0, 640, 480, 0,
		CopyFromParent, InputOutput, CopyFromParent,
		CWBackPixel | CWEventMask, &attrs);
	XStoreName(dpy, win, "xtest-keys");
	XMapWindow(dpy, win);

	/* 等 map 完成，再抢焦点 */
	for (;;) {
		XEvent ev;
		XNextEvent(dpy, &ev);
		if (ev.type == MapNotify) {
			break;
		}
	}
	XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
	XSync(dpy, False);

	printf("READY\n");
	fflush(stdout);

	double t0 = now_ms();
	double deadline = t0 + seconds * 1000.0;
	int count = 0;

	while (now_ms() < deadline) {
		while (XPending(dpy) > 0) {
			XEvent ev;
			XNextEvent(dpy, &ev);
			if (ev.type == KeyPress || ev.type == KeyRelease) {
				KeySym sym = XLookupKeysym(&ev.xkey, 0);
				const char *name = XKeysymToString(sym);
				printf("%8.1f %s keycode=%u keysym=0x%lx %s\n",
					now_ms() - t0,
					ev.type == KeyPress ? "PRESS  " : "RELEASE",
					ev.xkey.keycode, (unsigned long)sym,
					name ? name : "?");
				fflush(stdout);
				count++;
			} else if (ev.type == FocusOut) {
				printf("%8.1f FOCUS_OUT\n", now_ms() - t0);
				fflush(stdout);
			} else if (ev.type == FocusIn) {
				printf("%8.1f FOCUS_IN\n", now_ms() - t0);
				fflush(stdout);
			}
		}
		usleep(2000);
	}

	printf("DONE total=%d\n", count);
	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);
	return 0;
}
