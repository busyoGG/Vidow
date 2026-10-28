/* xpopup：给 XWayland 测试用的最小 X11 客户端 —— 按命令行给的绝对坐标造一个
 * 纯色窗口，然后待着不动。窗口 id 打到 stdout 第一行（bash 拿它当 parent=）。
 *
 *   xpopup owner <x> <y> <w> <h>
 *   xpopup popup <kind> <x> <y> <w> <h> [parent=<winid>] [child-of=<winid>]
 *                                  [late=<ms>]
 *                                             [fixed] [move=<dx>,<dy>[,<ms>]]
 *
 * kind 决定它是什么窗口，覆盖 X11 里弹出物的两种做法：
 *   or        override-redirect，不设 _NET_WM_WINDOW_TYPE（老派菜单）
 *   tooltip   override-redirect + _NET_WM_WINDOW_TYPE_TOOLTIP
 *   menu      _NET_WM_WINDOW_TYPE_POPUP_MENU，**不**设 override-redirect
 *   combo     _NET_WM_WINDOW_TYPE_COMBO（Qt 的下拉框）
 *   utility   _NET_WM_WINDOW_TYPE_UTILITY（普通窗口，vidow 该装饰它）
 *   normal    什么都不设（普通窗口）
 *
 * late=<ms>  映射之后才设窗口类型 —— 模拟「属性比 map 晚到」，vidow 得改判
 * fixed      在 WM_NORMAL_HINTS 里声明 min == max = 创建尺寸（「我自己定大小」）：
 *            这种窗口不会被布局改尺寸，用来验证「改判之后位置尺寸听客户端的」
 * move=<dx>,<dy>[,<ms>]  默认 1 秒后把自己挪 (dx,dy)：OR 窗口走 ConfigureNotify，
 *            非 OR 走 ConfigureRequest（两条路都得让它落对位置）
 *
 * 颜色：owner 蓝、popup 红，写死，方便对截图做像素定位。
 */
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

#define OWNER_PIXEL 0x0000c0u
#define POPUP_PIXEL 0xc00000u
#define GREEN_PIXEL 0x00c000u

static xcb_atom_t intern(xcb_connection_t *conn, const char *name) {
	xcb_intern_atom_cookie_t ck =
		xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
	xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn, ck, NULL);
	if (reply == NULL) {
		return XCB_ATOM_NONE;
	}
	xcb_atom_t atom = reply->atom;
	free(reply);
	return atom;
}

static void set_type(xcb_connection_t *conn, xcb_window_t win,
		const char *type) {
	xcb_atom_t prop = intern(conn, "_NET_WM_WINDOW_TYPE");
	xcb_atom_t value = intern(conn, type);
	if (prop == XCB_ATOM_NONE || value == XCB_ATOM_NONE) {
		fprintf(stderr, "xpopup: 取不到类型 atom %s\n", type);
		return;
	}
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, prop,
		XCB_ATOM_ATOM, 32, 1, &value);
	xcb_flush(conn);
}

static void set_transient(xcb_connection_t *conn, xcb_window_t win,
		xcb_window_t parent) {
	xcb_atom_t prop = intern(conn, "WM_TRANSIENT_FOR");
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, prop,
		XCB_ATOM_WINDOW, 32, 1, &parent);
	/* 还有 ICCCM 的 WM_CLIENT_MACHINE/WM_PROTOCOLS 就不管了，测试用不着。 */
}

static void set_string(xcb_connection_t *conn, xcb_window_t win,
		const char *prop_name, const char *value) {
	xcb_atom_t prop = intern(conn, prop_name);
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, prop,
		XCB_ATOM_STRING, 8, (uint32_t)strlen(value), value);
}

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void usage(void) {
	fprintf(stderr,
		"用法：xpopup owner <x> <y> <w> <h>\n"
		"      xpopup popup <or|tooltip|menu|combo|utility|normal>"
		" <x> <y> <w> <h> [parent=<winid>] [child-of=<winid>]"
		" [late=<ms>] [move=<dx>,<dy>]\n");
	exit(2);
}

int main(int argc, char **argv) {
	if (argc < 6) {
		usage();
	}
	bool owner = strcmp(argv[1], "owner") == 0;
	int arg = 2;
	const char *kind = NULL;
	if (!owner) {
		kind = argv[arg++];
	}
	int x = atoi(argv[arg++]);
	int y = atoi(argv[arg++]);
	int w = atoi(argv[arg++]);
	int h = atoi(argv[arg++]);
	if (w < 1 || h < 1) {
		usage();
	}

	xcb_window_t parent = XCB_WINDOW_NONE;
	/* 创建时的位置：move= 的位移以它为基准（窗口自己可能被合成器挪过，
	 * 用「当前位置」当基准会让位移叠加，测出来的就不是想测的那个点）。 */
	const int orig_x = x, orig_y = y;
	/* 声明的尺寸：w/h 会被合成器回的 CONFIGURE_NOTIFY 改写，grow= 到点
	 * 要长回「客户端本来要的那一档」，不能拿被改写过的 w/h。 */
	const int want_w = w, want_h = h;
	int late_ms = -1;
	int min_w = 0, min_h = 0, max_w = 0, max_h = 0;
	int move_dx = 0, move_dy = 0, move_at_ms = 1000;
	bool do_move = false;
	bool fixed_size = false;
	int grow_ms = 0;
	int want_scale = 0;
	int paint_mode = 0;
	xcb_window_t child_of = XCB_WINDOW_NONE;
	for (int i = arg; i < argc; i++) {
		if (strncmp(argv[i], "child-of=", 9) == 0) {
			child_of = (xcb_window_t)strtoul(argv[i] + 9, NULL, 0);
		} else if (strncmp(argv[i], "parent=", 7) == 0) {
			parent = (xcb_window_t)strtoul(argv[i] + 7, NULL, 0);
		} else if (strncmp(argv[i], "late=", 5) == 0) {
			late_ms = atoi(argv[i] + 5);
		} else if (strcmp(argv[i], "fixed") == 0) {
			fixed_size = true;
		} else if (strncmp(argv[i], "hints=", 6) == 0) {
			/* hints=<minw>x<minh>-<maxw>x<maxh>，0 = 这一头不声明 */
			int n = sscanf(argv[i] + 6, "%dx%d-%dx%d", &min_w, &min_h,
				&max_w, &max_h);
			if (n < 4) {
				usage();
			}
		} else if (strcmp(argv[i], "paint") == 0) {
			paint_mode = 1;
		} else if (strncmp(argv[i], "scale=", 6) == 0) {
			want_scale = atoi(argv[i] + 6);
		} else if (strncmp(argv[i], "grow=", 5) == 0) {
			/* grow=<ms>：先以 1x1 映射，过 ms 再改成声明的尺寸 ——
			 * 复现 Steam 菜单「尺寸属性比 map 晚到」的时序。 */
			grow_ms = atoi(argv[i] + 5);
		} else if (strncmp(argv[i], "move=", 5) == 0) {
			int n = sscanf(argv[i] + 5, "%d,%d,%d", &move_dx, &move_dy,
				&move_at_ms);
			do_move = n >= 2;
		} else {
			usage();
		}
	}

	bool override_redirect = false;
	const char *type = NULL;
	if (owner || strcmp(kind, "normal") == 0) {
		/* 普通窗口：什么都不设 */
	} else if (strcmp(kind, "or") == 0) {
		override_redirect = true;
	} else if (strcmp(kind, "tooltip") == 0) {
		override_redirect = true;
		type = "_NET_WM_WINDOW_TYPE_TOOLTIP";
	} else if (strcmp(kind, "menu") == 0) {
		type = "_NET_WM_WINDOW_TYPE_POPUP_MENU";
	} else if (strcmp(kind, "combo") == 0) {
		type = "_NET_WM_WINDOW_TYPE_COMBO";
	} else if (strcmp(kind, "utility") == 0) {
		type = "_NET_WM_WINDOW_TYPE_UTILITY";
	} else {
		usage();
	}

	int screen_idx = 0;
	xcb_connection_t *conn = xcb_connect(NULL, &screen_idx);
	if (xcb_connection_has_error(conn)) {
		fprintf(stderr, "xpopup: 连不上 X（DISPLAY 设了吗）\n");
		return 1;
	}
	const xcb_screen_t *screen =
		xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
	if (screen == NULL) {
		fprintf(stderr, "xpopup: 没有屏幕\n");
		return 1;
	}

	xcb_window_t win = xcb_generate_id(conn);
	/* 值的顺序必须跟掩码里各个 bit 的先后一致（X 协议按 bit 顺序填），
	 * 所以 OVERRIDE_REDIRECT 得夹在 BACK_PIXEL 和 EVENT_MASK 中间。 */
	uint32_t values[3];
	uint32_t mask = XCB_CW_BACK_PIXEL;
	values[0] = owner ? OWNER_PIXEL : POPUP_PIXEL;
	int n = 1;
	if (override_redirect) {
		mask |= XCB_CW_OVERRIDE_REDIRECT;
		values[n++] = 1;
	}
	mask |= XCB_CW_EVENT_MASK;
	values[n++] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
		XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE;
	/* grow=<ms>：先按 1x1 建窗（复现 Steam 菜单「属性/尺寸比 map 晚到」），
	 * 到点再 configure 到声明的尺寸。 */
	int grow_w = grow_ms > 0 ? 1 : w;
	int grow_h = grow_ms > 0 ? 1 : h;
	/* child-of= 用来建**真正的子窗口**（Steam / CEF 那种：坐标相对父窗口）。
	 * 和 parent= 分开：parent= 是 ICCCM 的 WM_TRANSIENT_FOR 属性（「我属于
	 * 谁」），窗口仍然挂在 root 上、坐标是屏幕坐标；child-of= 才是真子窗口，
	 * 坐标相对父窗口。两件事混在一起会把 test-xwayland-popup.sh 的 combo
	 * 场景（它传 parent= 只是为了设 transient）算错坐标。 */
	xcb_window_t host = child_of != XCB_WINDOW_NONE ? child_of : screen->root;
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, host,
		(int16_t)x, (int16_t)y, (uint16_t)grow_w, (uint16_t)grow_h, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);

	if (fixed_size) {
		/* WM_NORMAL_HINTS：min == max == 创建尺寸 = 「这是我自己的尺寸，
		 * 别改」。布局会把这类窗口当浮动窗口，尺寸听客户端的。 */
		uint32_t hints[18] = {0};
		hints[0] = XCB_ICCCM_SIZE_HINT_US_SIZE |
			XCB_ICCCM_SIZE_HINT_P_MIN_SIZE |
			XCB_ICCCM_SIZE_HINT_P_MAX_SIZE;
		hints[5] = (uint32_t)w;   /* min_width */
		hints[6] = (uint32_t)h;   /* min_height */
		hints[7] = (uint32_t)w;   /* max_width */
		hints[8] = (uint32_t)h;   /* max_height */
		xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win,
			XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 32, 18, hints);
	} else if (min_w > 0 || min_h > 0 || max_w > 0 || max_h > 0) {
		/* 只有上下限、没有固定值（微信主窗 / 各种 GTK 对话框）：**尺寸由布局
		 * 定，但客户端自己会夹在上下限里** —— 合成器给一个越界的尺寸，它就
		 * 自己缩回边界再发一个 ConfigureRequest 回来。见下面的 clamp。 */
		uint32_t hints[18] = {0};
		hints[0] = XCB_ICCCM_SIZE_HINT_US_SIZE;
		if (min_w > 0 && min_h > 0) {
			hints[0] |= XCB_ICCCM_SIZE_HINT_P_MIN_SIZE;
			hints[5] = (uint32_t)min_w;
			hints[6] = (uint32_t)min_h;
		}
		if (max_w > 0 && max_h > 0) {
			hints[0] |= XCB_ICCCM_SIZE_HINT_P_MAX_SIZE;
			hints[7] = (uint32_t)max_w;
			hints[8] = (uint32_t)max_h;
		}
		xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win,
			XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 32, 18, hints);
	}
	set_string(conn, win, "WM_CLASS", owner ? "xpopup\0XOwner" :
		"xpopup\0XPopup");
	set_string(conn, win, "WM_NAME", owner ? "xp-owner" : "xp-popup");

	/* scale=<N>：模拟打过 HiDPI 补丁的 Xwayland 客户端（Steam 就是这种）——
	 * 在窗口上写 _XWAYLAND_GLOBAL_OUTPUT_SCALE=N，Xwayland 会把这块表面的
	 * buffer 按 N 倍放大提交（缓冲 Nx=N×逻辑）。用来复现「逻辑尺寸 / 缓冲尺寸
	 * 不一致」时 vidow 的 X11 采样换算。 */
	if (want_scale > 0) {
		xcb_intern_atom_cookie_t sc = xcb_intern_atom(conn, 0,
			sizeof("_XWAYLAND_GLOBAL_OUTPUT_SCALE") - 1,
			"_XWAYLAND_GLOBAL_OUTPUT_SCALE");
		xcb_intern_atom_reply_t *sr = xcb_intern_atom_reply(conn, sc, NULL);
		if (sr != NULL) {
			uint32_t v = (uint32_t)want_scale;
			xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win,
				sr->atom, XCB_ATOM_CARDINAL, 32, 1, &v);
			free(sr);
		}
	}
	if (parent != XCB_WINDOW_NONE) {
		set_transient(conn, win, parent);
	}
	if (type != NULL && late_ms < 0) {
		set_type(conn, win, type);
	}

	/* 真画一块纯色：只有实际绘制才会产生 damage / 提交缓冲区。
	 * 背景像素在 override-redirect + 尺寸变化时不一定被重绘。 */
	xcb_colormap_t cmap = screen->default_colormap;
	xcb_gcontext_t gc = xcb_generate_id(conn);
	uint32_t gcv[2] = { owner ? OWNER_PIXEL : POPUP_PIXEL,
		(uint32_t)screen->white_pixel };
	xcb_create_gc(conn, gc, win,
		XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, gcv);
	(void)cmap;
	xcb_map_window(conn, win);
	xcb_flush(conn);

	printf("0x%x\n", win);
	fflush(stdout);

	/* paint <w> <h>：先等一小会儿（让窗口先按底色上屏、缓存冻结），再往窗口
	 * 左上角画一块**绿色**。这一步只发 XPutImage + 一次 commit，**不摆窗口、
	 * 不改属性** —— 用来验证「客户端只交帧」时合成器会不会把新内容画出来
	 * （Steam 黑屏那条：装饰缓存不失效就永远停在旧那版）。画完之后继续待在
	 * 主循环里等，窗口保持打开。 */
	if (paint_mode) {
		const char *pw_arg = getenv("XPOPUP_PAINT_W");
		const char *ph_arg = getenv("XPOPUP_PAINT_H");
		int pw = pw_arg != NULL ? atoi(pw_arg) : 200;
		int ph = ph_arg != NULL ? atoi(ph_arg) : 150;
		struct timespec ts = { .tv_sec = 3, .tv_nsec = 0 };
		nanosleep(&ts, NULL);
		const size_t stride = (size_t)pw * 4;
		uint8_t *px = malloc(stride * (size_t)ph);
		if (px != NULL) {
			for (size_t i = 0; i < (size_t)pw * (size_t)ph; i++) {
				/* BGRX，小端：绿 = 0x0000ff00 */
				px[i * 4 + 0] = 0x00;
				px[i * 4 + 1] = 0xff;
				px[i * 4 + 2] = 0x00;
				px[i * 4 + 3] = 0xff;
			}
			xcb_gcontext_t gc = xcb_generate_id(conn);
			uint32_t gcv0 = GREEN_PIXEL;
			xcb_create_gc(conn, gc, win, XCB_GC_FOREGROUND, &gcv0);
			xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, win, gc,
				(uint16_t)pw, (uint16_t)ph, 0, 0, 0,
				screen->root_depth, (uint32_t)(stride * (size_t)ph), px);
			xcb_flush(conn);
			free(px);
			printf("painted %dx%d\n", pw, ph);
			fflush(stdout);
		}
	}

	/* 窗口 id 已经交出去了，之后标准输出只用来打进度标记（测试脚本只读第一行）。 */
	int64_t start = now_ms();
	bool late_done = late_ms < 0;
	bool move_done = !do_move;
	bool grow_done = grow_ms <= 0;
	for (;;) {
		int64_t elapsed = now_ms() - start;
		if (!late_done && elapsed >= late_ms) {
			set_type(conn, win, type);
			late_done = true;
			printf("late-type %s\n", type);
			fflush(stdout);
		}
		if (!grow_done && elapsed >= grow_ms) {
			uint32_t vals[2] = { (uint32_t)want_w, (uint32_t)want_h };
			xcb_configure_window(conn, win,
				XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
				vals);
			/* 主动清一次窗口：用背景色填充会触发 damage/commit，
			 * 让合成器拿到新尺寸的缓冲区（真实菜单长大时必然重绘）。 */
			xcb_unmap_window(conn, win);
			xcb_map_window(conn, win);
			xcb_flush(conn);
			grow_done = true;
			printf("grew %dx%d\n", want_w, want_h);
			fflush(stdout);
		}
		if (!move_done && elapsed >= move_at_ms) {
			uint32_t pos[2] = {
				(uint32_t)(orig_x + move_dx),
				(uint32_t)(orig_y + move_dy),
			};
			xcb_configure_window(conn, win,
				XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, pos);
			xcb_flush(conn);
			move_done = true;
			printf("moved %d,%d\n", orig_x + move_dx, orig_y + move_dy);
			fflush(stdout);
		}
		struct pollfd pfd = {
			.fd = xcb_get_file_descriptor(conn),
			.events = POLLIN,
		};
		if (poll(&pfd, 1, 40) > 0) {
			xcb_generic_event_t *ev;
			while ((ev = xcb_poll_for_event(conn)) != NULL) {
				/* 合成器给的位置尺寸要认（正经客户端就是这么做的）：不认的话
				 * 窗口缓冲区一直停在创建时的大小，窗口内容会被拉伸着显示。
				 * 自己 xcb_configure_window 一次把窗口挪成它要的样子。 */
				/* 按键：报到 stdout，测试拿它断言「鼠标事件到底有没有
				 * 到客户端」——「看得见、点不着」只有这条能分开是合成器没
				 * 送到还是客户端自己不理。坐标是窗口局部坐标。 */
				if ((ev->response_type & ~0x80) == XCB_EXPOSE) {
					xcb_expose_event_t *ee = (void *)ev;
					xcb_poly_fill_rectangle(conn, win, gc, 1,
						&(xcb_rectangle_t){ 0, 0, ee->width,
							ee->height });
					xcb_flush(conn);
					printf("expose %ux%u\n", ee->width, ee->height);
					fflush(stdout);
				}
				if ((ev->response_type & ~0x80) ==
						XCB_BUTTON_PRESS) {
					xcb_button_press_event_t *be = (void *)ev;
					printf("click %d,%d btn%d\n",
						be->event_x, be->event_y,
						be->detail);
					fflush(stdout);
				}
				if ((ev->response_type & ~0x80) ==
						XCB_CONFIGURE_NOTIFY) {
					xcb_configure_notify_event_t *ce = (void *)ev;
					if (ce->width != w || ce->height != h ||
							ce->x != x || ce->y != y) {
						x = ce->x;
						y = ce->y;
						w = ce->width;
						h = ce->height;
						/* 尺寸已生效：重绘一次（真实客户端都会）。用
						 * BACKGROUND_PIXMAP 填充也必须产生 damage，否则
						 * Xwayland 那边不会提交新尺寸的缓冲区。 */
						xcb_poly_fill_rectangle(conn, win, gc, 1,
							&(xcb_rectangle_t){ 0, 0,
								(uint16_t)w, (uint16_t)h });
						xcb_flush(conn);
						/* 有上下限的窗口：合成器给越界尺寸就自己夹回来
						 * （真客户端就是这么干的，夹完再发 ConfigureRequest
						 * 告诉 WM「我改不了那么大 / 那么小」）。 */
						int cw = w, ch = h;
						if (min_w > 0 && cw < min_w) cw = min_w;
						if (min_h > 0 && ch < min_h) ch = min_h;
						if (max_w > 0 && cw > max_w) cw = max_w;
						if (max_h > 0 && ch > max_h) ch = max_h;
						w = cw;
						h = ch;
						uint32_t geom[4] = {
							(uint32_t)x, (uint32_t)y,
							(uint32_t)w, (uint32_t)h,
						};
						xcb_configure_window(conn, win,
							XCB_CONFIG_WINDOW_X |
							XCB_CONFIG_WINDOW_Y |
							XCB_CONFIG_WINDOW_WIDTH |
							XCB_CONFIG_WINDOW_HEIGHT, geom);
						xcb_flush(conn);
						printf("resized %d,%d %dx%d\n", x, y, w, h);
						fflush(stdout);
					}
				}
				free(ev);
			}
		}
	}
	return 0;
}
