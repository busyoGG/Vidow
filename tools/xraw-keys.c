/* xraw-keys —— 记录 XInput2 的 raw 键盘事件。
 *
 * 为什么需要它：Wine / Proton / 很多游戏引擎读的是 XI2 **raw events**，
 * 而不是 core 事件。XkbSetDetectableAutoRepeat() 只影响 core/XKB 事件流，
 * **对 raw events 无效** —— autorepeat 产生的「假 KeyRelease」在 raw 流里
 * 照样出现。所以即使游戏开了 detectable，它仍然会看到 W 每 41ms 松开一次，
 * 于是「前进」断断续续 = 角色抖动。
 *
 * raw events 是全局的，不需要窗口，也不需要焦点。
 *
 * 编译：
 *   cc -O2 -o build/xraw-keys tools/xraw-keys.c -lX11 -lXi
 *
 * 用法：
 *   ./build/xraw-keys [秒数]
 */
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <stdio.h>
#include <stdlib.h>
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
		fprintf(stderr, "xraw-keys: 连不上 X server\n");
		return 1;
	}

	int xi_opcode, xi_event, xi_error;
	if (!XQueryExtension(dpy, "XInputExtension", &xi_opcode, &xi_event,
			&xi_error)) {
		fprintf(stderr, "xraw-keys: X server 没有 XInputExtension\n");
		return 1;
	}
	int major = 2, minor = 0;
	if (XIQueryVersion(dpy, &major, &minor) != Success) {
		fprintf(stderr, "xraw-keys: XI2 不可用\n");
		return 1;
	}

	unsigned char mask_bits[XIMaskLen(XI_LASTEVENT)] = {0};
	XISetMask(mask_bits, XI_RawKeyPress);
	XISetMask(mask_bits, XI_RawKeyRelease);
	XIEventMask mask;
	mask.deviceid = XIAllMasterDevices;
	mask.mask_len = sizeof(mask_bits);
	mask.mask = mask_bits;

	Window root = DefaultRootWindow(dpy);
	if (XISelectEvents(dpy, root, &mask, 1) != Success) {
		fprintf(stderr, "xraw-keys: XISelectEvents 失败\n");
		return 1;
	}
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
			XGenericEventCookie *cookie = &ev.xcookie;
			if (cookie->type != GenericEvent ||
					cookie->extension != xi_opcode) {
				continue;
			}
			if (!XGetEventData(dpy, cookie)) {
				continue;
			}
			XIRawEvent *raw = cookie->data;
			if (cookie->evtype == XI_RawKeyPress ||
					cookie->evtype == XI_RawKeyRelease) {
				printf("%8.1f %s keycode=%d detail=%d\n",
					now_ms() - t0,
					cookie->evtype == XI_RawKeyPress ?
						"RAW_PRESS  " : "RAW_RELEASE",
					raw->detail, raw->detail);
				fflush(stdout);
				count++;
			}
			XFreeEventData(dpy, cookie);
		}
		usleep(2000);
	}

	printf("DONE total=%d\n", count);
	XCloseDisplay(dpy);
	return 0;
}
