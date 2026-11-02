/* xsettingsget —— 读 XSETTINGS 里的一个值（测试断言用）。
 *
 * 用法：
 *   xsettingsget                     打印 Gdk/WindowScalingFactor（整数）
 *   xsettingsget <名字>              打印任意设置（整数原样，字符串带引号）
 *   xsettingsget --owner             打印 _XSETTINGS_S0 的属主窗口
 *
 * 退出码：找到并打印 0；没有 XSETTINGS / 没有这个设置 1。
 *
 * 线格式见 src/xwm_x11.c 的 XSETTINGS 段：头 12 字节，每条设置是
 * type(1) pad(1) nlen(2) name pad→4、u32 1、值（INTEGER = u32，
 * STRING = u32 len + bytes）。这里只解析整数和字符串，够测试用。 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>

static int find_setting(unsigned char *data, size_t len, const char *want,
		int *type_out, uint32_t *int_out, const char **str_out,
		size_t *str_len_out) {
	if (len < 12) {
		return 0;
	}
	size_t off = 12;
	while (off + 4 <= len) {
		uint8_t type = data[off];
		uint16_t nlen = (uint16_t)(data[off + 2] | (data[off + 3] << 8));
		size_t name_at = off + 4;
		if (name_at + nlen > len) {
			return 0;
		}
		size_t val_at = (name_at + nlen + 3u) & ~(size_t)3;
		if (val_at + 8 > len) {
			return 0;
		}
		size_t end = 0;
		uint32_t ival = 0;
		const char *sval = NULL;
		size_t slen = 0;
		if (type == 0) {
			ival = (uint32_t)data[val_at + 4] |
				((uint32_t)data[val_at + 5] << 8) |
				((uint32_t)data[val_at + 6] << 16) |
				((uint32_t)data[val_at + 7] << 24);
			end = val_at + 8;
		} else if (type == 1) {
			slen = (size_t)data[val_at + 4] |
				((size_t)data[val_at + 5] << 8) |
				((size_t)data[val_at + 6] << 16) |
				((size_t)data[val_at + 7] << 24);
			if (val_at + 8 + slen > len) {
				return 0;
			}
			sval = (const char *)(data + val_at + 8);
			end = (val_at + 8 + slen + 3u) & ~(size_t)3;
		} else {
			return 0;
		}
		if (nlen == strlen(want) &&
				memcmp(data + name_at, want, nlen) == 0) {
			*type_out = type;
			*int_out = ival;
			*str_out = sval;
			*str_len_out = slen;
			return 1;
		}
		off = end;
	}
	return 0;
}

int main(int argc, char **argv) {
	const char *want = argc >= 2 && strcmp(argv[1], "--owner") != 0 ?
		argv[1] : "Gdk/WindowScalingFactor";
	bool want_owner = argc >= 2 && strcmp(argv[1], "--owner") == 0;

	int screen_num = 0;
	xcb_connection_t *conn = xcb_connect(NULL, &screen_num);
	if (xcb_connection_has_error(conn)) {
		fprintf(stderr, "xsettingsget: 连不上 X（DISPLAY=%s）\n",
			getenv("DISPLAY") != NULL ? getenv("DISPLAY") : "?");
		return 1;
	}
	xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(conn));
	for (int i = 0; i < screen_num; i++) {
		xcb_screen_next(&it);
	}
	xcb_screen_t *screen = it.data;

	xcb_intern_atom_cookie_t sel_c = xcb_intern_atom(conn, 0,
		(uint16_t)strlen("_XSETTINGS_S0"), "_XSETTINGS_S0");
	xcb_intern_atom_cookie_t prop_c = xcb_intern_atom(conn, 0,
		(uint16_t)strlen("_XSETTINGS_SETTINGS"), "_XSETTINGS_SETTINGS");
	xcb_intern_atom_reply_t *sel_r = xcb_intern_atom_reply(conn, sel_c, NULL);
	xcb_intern_atom_reply_t *prop_r = xcb_intern_atom_reply(conn, prop_c, NULL);
	if (sel_r == NULL || prop_r == NULL) {
		return 1;
	}
	xcb_window_t owner = XCB_NONE;
	xcb_get_selection_owner_reply_t *owner_r =
		xcb_get_selection_owner_reply(conn,
			xcb_get_selection_owner(conn, sel_r->atom), NULL);
	if (owner_r != NULL) {
		owner = owner_r->owner;
		free(owner_r);
	}
	if (want_owner) {
		printf("0x%x\n", (unsigned)owner);
		free(sel_r);
		free(prop_r);
		xcb_disconnect(conn);
		return owner != XCB_NONE ? 0 : 1;
	}
	if (owner == XCB_NONE) {
		fprintf(stderr, "xsettingsget: 没有 _XSETTINGS_S0 属主\n");
		return 1;
	}

	xcb_get_property_reply_t *pr = xcb_get_property_reply(conn,
		xcb_get_property(conn, 0, owner, prop_r->atom,
			XCB_GET_PROPERTY_TYPE_ANY, 0, 1 << 16), NULL);
	int type = 0;
	uint32_t ival = 0;
	const char *sval = NULL;
	size_t slen = 0;
	int found = 0;
	if (pr != NULL && pr->format == 8 && pr->type == prop_r->atom) {
		found = find_setting(xcb_get_property_value(pr),
			(size_t)xcb_get_property_value_length(pr), want,
			&type, &ival, &sval, &slen);
	}
	if (!found) {
		fprintf(stderr, "xsettingsget: 属主 0x%x 里没有 %s\n",
			(unsigned)owner, want);
	} else if (type == 1) {
		printf("\"%.*s\"\n", (int)slen, sval);
	} else {
		printf("%u\n", ival);
	}
	if (pr != NULL) {
		free(pr);
	}
	free(sel_r);
	free(prop_r);
	xcb_disconnect(conn);
	(void)screen;
	return found ? 0 : 1;
}
