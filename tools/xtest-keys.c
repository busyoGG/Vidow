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
 *   ./build/xtest-keys [秒数] [--detectable]   # 默认 6 秒
 *
 *   --detectable  先调 XkbSetDetectableAutoRepeat(True)。
 *                 开了之后 X server **不再发 autorepeat 产生的假 KeyRelease**，
 *                 只发 KeyPress。绝大多数游戏框架（SDL2 / GLFW / Wine）都会开。
 *                 用它来区分「游戏看到的」和「没开这个的客户端看到的」。
 *
 * 输出每行：时间(ms) 事件 键码 keysym 名称
 *
 * 窗口会自己抢焦点（XSetInputFocus），所以不需要点它。
 */
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdbool.h>
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
	int seconds = 6;
	bool detectable = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--detectable") == 0) {
			detectable = true;
		} else {
			int v = atoi(argv[i]);
			if (v > 0) {
				seconds = v;
			}
		}
	}

	Display *dpy = XOpenDisplay(NULL);
	if (dpy == NULL) {
		fprintf(stderr, "xtest-keys: 连不上 X server（DISPLAY=%s）\n",
			getenv("DISPLAY") ? getenv("DISPLAY") : "(空)");
		return 1;
	}

	/* **必须在建窗口之前调**：XkbSetDetectableAutoRepeat 影响的是本客户端
	 * 收到的 autorepeat 事件形态 —— 开了之后 X server 不再发「重复产生的
	 * KeyRelease」，只发 KeyPress。游戏框架（SDL2 / GLFW / Wine）都会开。 */
	if (detectable) {
		Bool supported = False;
		XkbSetDetectableAutoRepeat(dpy, True, &supported);
		printf("DETECTABLE requested=1 supported=%d\n", supported ? 1 : 0);
		fflush(stdout);
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
