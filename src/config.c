#include "server.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <wlr/util/log.h>

enum kind {
	K_INT,
	K_DOUBLE,
	K_BOOL,
	K_EASING,
	K_STRING,
	K_COLOR, /* "RRGGBB" or "RRGGBBAA", straight (non-premultiplied) alpha */
};

struct entry {
	const char *key;
	enum kind kind;
	size_t offset;
	size_t size;
};

#define OFF(field) offsetof(struct vidow_config, field)
#define SZ(field) sizeof(((struct vidow_config *)0)->field)

static const struct entry entries[] = {
	{ "decor_enabled",   K_BOOL,   OFF(decor_enabled),   SZ(decor_enabled) },
	{ "corner_radius",   K_DOUBLE, OFF(corner_radius),   SZ(corner_radius) },
	{ "border_width",    K_DOUBLE, OFF(border_width),    SZ(border_width) },
	{ "border_color",    K_COLOR,  OFF(border_color),    SZ(border_color) },
	{ "border_color_focus", K_COLOR, OFF(border_color_focus), SZ(border_color_focus) },
	{ "shadow_radius",   K_DOUBLE, OFF(shadow_radius),   SZ(shadow_radius) },
	{ "shadow_color",    K_COLOR,  OFF(shadow_color),    SZ(shadow_color) },
	{ "shadow_offset_x", K_INT,    OFF(shadow_offset_x), SZ(shadow_offset_x) },
	{ "shadow_offset_y", K_INT,    OFF(shadow_offset_y), SZ(shadow_offset_y) },
	{ "window_opacity",  K_DOUBLE, OFF(window_opacity),  SZ(window_opacity) },
	{ "noise",           K_DOUBLE, OFF(noise),           SZ(noise) },
	{ "blur_enabled",    K_BOOL,   OFF(blur_enabled),    SZ(blur_enabled) },
	{ "blur_all",        K_BOOL,   OFF(blur_all),        SZ(blur_all) },
	{ "blur_levels",     K_INT,    OFF(blur_levels),     SZ(blur_levels) },
	{ "blur_strength",   K_DOUBLE, OFF(blur_strength),   SZ(blur_strength) },
	{ "blur_alpha_min",  K_DOUBLE, OFF(blur_alpha_min),  SZ(blur_alpha_min) },
	{ "blur_offset",     K_DOUBLE, OFF(blur_offset),     SZ(blur_offset) },
	{ "force_render_hz", K_INT,    OFF(force_render_hz), SZ(force_render_hz) },
	{ "ipc",             K_BOOL,   OFF(ipc),             SZ(ipc) },
	{ "clipboard",       K_BOOL,   OFF(clipboard),       SZ(clipboard) },
	{ "clipboard_primary", K_BOOL, OFF(clipboard_primary),
		SZ(clipboard_primary) },
	{ "clipboard_max_bytes", K_INT, OFF(clipboard_max_bytes),
		SZ(clipboard_max_bytes) },
	{ "ipc_socket",      K_STRING, OFF(ipc_socket),      SZ(ipc_socket) },
	{ "activation_token_timeout", K_INT,
		OFF(activation_token_timeout), SZ(activation_token_timeout) },
	{ "honor_xdg_activation_with_invalid_serial", K_BOOL,
		OFF(honor_xdg_activation_with_invalid_serial),
		SZ(honor_xdg_activation_with_invalid_serial) },
	{ "window_open_ms",  K_INT,    OFF(window_open_ms),  SZ(window_open_ms) },
	{ "window_close_ms", K_INT,    OFF(window_close_ms), SZ(window_close_ms) },
	{ "window_move_ms",  K_INT,    OFF(window_move_ms),  SZ(window_move_ms) },
	{ "layer_open_ms",   K_INT,    OFF(layer_open_ms),   SZ(layer_open_ms) },
	{ "layer_close_ms",  K_INT,    OFF(layer_close_ms),  SZ(layer_close_ms) },
	{ "easing",          K_EASING, OFF(easing),          SZ(easing) },
	{ "open_alpha_from", K_DOUBLE, OFF(open_alpha_from), SZ(open_alpha_from) },
	{ "open_slide_px",   K_INT,    OFF(open_slide_px),   SZ(open_slide_px) },
	{ "close_alpha_to",  K_DOUBLE, OFF(close_alpha_to),  SZ(close_alpha_to) },
	{ "close_slide_px",  K_INT,    OFF(close_slide_px),  SZ(close_slide_px) },
	{ "shader_enabled",  K_BOOL,   OFF(shader_enabled),  SZ(shader_enabled) },
	{ "shader_open",     K_STRING, OFF(shader_open),     SZ(shader_open) },
	{ "shader_close",    K_STRING, OFF(shader_close),    SZ(shader_close) },
	{ "shader_idle",     K_STRING, OFF(shader_idle),     SZ(shader_idle) },
	{ "shader_dir",      K_STRING, OFF(shader_dir),      SZ(shader_dir) },
	{ "shader_flip_y",   K_BOOL,   OFF(shader_flip_y),   SZ(shader_flip_y) },
	{ "scale",           K_STRING, OFF(scale),           SZ(scale) },
	{ "outputs",         K_STRING, OFF(outputs),         SZ(outputs) },
	{ "xwayland",        K_BOOL,   OFF(xwayland),        SZ(xwayland) },
	{ "xwayland_scale",  K_STRING, OFF(xwayland_scale),  SZ(xwayland_scale) },
	{ "xwayland_env",    K_BOOL,   OFF(xwayland_env),    SZ(xwayland_env) },
	{ "portal_conf_sync", K_BOOL,  OFF(portal_conf_sync), SZ(portal_conf_sync) },
	{ "portal_desktop",  K_STRING, OFF(portal_desktop),  SZ(portal_desktop) },
};

static const char *easing_names[] = {
	"linear", "out_cubic", "out_quint", "in_out_cubic", "out_back",
};

static bool parse_easing(const char *value, enum vidow_easing *out) {
	for (size_t i = 0; i < VIDOW_EASE_OUT_BACK + 1; i++) {
		if (strcmp(value, easing_names[i]) == 0) {
			*out = (enum vidow_easing)i;
			return true;
		}
	}
	return false;
}

/* Returns false if the key is unknown or the value cannot be parsed. */
static bool apply(struct vidow_config *cfg, const char *key, const char *value) {	for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		const struct entry *e = &entries[i];
		if (strcmp(e->key, key) != 0) {
			continue;
		}
		void *field = (char *)cfg + e->offset;
		switch (e->kind) {
		case K_INT:
			*(int *)field = atoi(value);
			return true;
		case K_DOUBLE:
			*(double *)field = atof(value);
			return true;
		case K_BOOL:
			*(bool *)field = strcmp(value, "0") != 0 &&
				strcasecmp(value, "false") != 0 &&
				strcasecmp(value, "no") != 0 &&
				strcasecmp(value, "off") != 0;
			return true;
		case K_EASING:
			if (!parse_easing(value, (enum vidow_easing *)field)) {
				wlr_log(WLR_ERROR, "config: unknown easing '%s'", value);
				return false;
			}
			return true;
		case K_STRING:
			snprintf((char *)field, e->size, "%s", value);
			return true;
		case K_COLOR: {
			const char *hex = value[0] == '#' ? value + 1 : value;
			size_t len = strlen(hex);
			if (len != 6 && len != 8) {
				wlr_log(WLR_ERROR, "config: colour '%s' must be RRGGBB or RRGGBBAA",
					value);
				return false;
			}
			char part[3] = {0};
			float *out = field;
			for (int i = 0; i < 4; i++) {
				if (i == 3 && len == 6) {
					out[3] = 1.0f;
					break;
				}
				memcpy(part, hex + i * 2, 2);
				out[i] = (float)strtol(part, NULL, 16) / 255.0f;
			}
			return true;
		}
		}
	}
	return false;
}

static void defaults(struct vidow_config *cfg) {
	memset(cfg, 0, sizeof(*cfg));
	cfg->force_render_hz = 60; /* 视口外补帧：跟屏幕刷新率差不多就够 */
	/* 激活令牌不过期：wlroots 默认 30 秒，而「弹通知」和「点通知」之间常常隔着
	 * 几分钟 —— 令牌过期了 wlroots 直接把激活请求丢掉，我们连信号都收不到
	 * （点了通知窗口不动，看着像客户端坏了）。 */
	cfg->activation_token_timeout = 0;
	/* serial 校验不通过的激活请求默认不理（= wlroots 一直以来的行为）。 */
	cfg->honor_xdg_activation_with_invalid_serial = false;
	/* IPC 默认开着：socket 只有自己这个用户能连（0600），而状态栏 / 脚本全靠它。
	 * 路径留空 = 按 WAYLAND_DISPLAY 自动取（$XDG_RUNTIME_DIR/vidow-<display>.sock）。 */
	cfg->ipc = true;
	cfg->ipc_socket[0] = '\0';
	/* 剪贴板默认接管：X11(Xwayland) 和 Wayland 两边共用一份内容，而且不看焦点
	 * （wlroots 自带那套只在有 X11 窗口聚焦时才同步）。每个类型读进内存的上限
	 * 32MiB —— 「源程序退出后还能粘」靠的就是这份副本。 */
	cfg->clipboard = true;
	cfg->clipboard_primary = true;
	cfg->clipboard_max_bytes = 32 * 1024 * 1024;
	cfg->window_open_ms = 180;
	cfg->window_close_ms = 140;
	cfg->window_move_ms = 190;
	cfg->layer_open_ms = 200;
	cfg->layer_close_ms = 150;
	cfg->easing = VIDOW_EASE_OUT_CUBIC;

	cfg->open_alpha_from = 0.0;
	cfg->open_slide_px = 22;
	cfg->close_alpha_to = 0.0;
	cfg->close_slide_px = 14;

	cfg->decor_enabled = true;
	cfg->corner_radius = 12.0;
	cfg->border_width = 2.0;
	cfg->border_color[0] = 0.30f; /* unfocused: dim */
	cfg->border_color[1] = 0.33f;
	cfg->border_color[2] = 0.42f;
	cfg->border_color[3] = 1.0f;
	cfg->border_color_focus[0] = 0.48f; /* focused: accent */
	cfg->border_color_focus[1] = 0.65f;
	cfg->border_color_focus[2] = 0.97f;
	cfg->border_color_focus[3] = 1.0f;
	cfg->shadow_radius = 24.0;
	cfg->shadow_color[0] = 0.0f;
	cfg->shadow_color[1] = 0.0f;
	cfg->shadow_color[2] = 0.0f;
	cfg->shadow_color[3] = 0.55f;
	cfg->shadow_offset_x = 0;
	cfg->shadow_offset_y = 6;
	cfg->window_opacity = 1.0;
	cfg->noise = 0.0;

	cfg->blur_enabled = true;
	cfg->blur_all = true; /* tiled windows blur the wallpaper too */
	cfg->blur_levels = 4;
	cfg->blur_strength = 1.0;
	cfg->blur_alpha_min = 0.15;
	cfg->blur_offset = 1.5;

	cfg->shader_enabled = true;
	cfg->shader_open[0] = '\0';
	cfg->shader_close[0] = '\0';
	cfg->shader_idle[0] = '\0';
	cfg->shader_flip_y = false;

	/* 缩放：默认按 EDID 的 ppi 推，让同一个逻辑尺寸在两块不同 DPI 的屏上是
	 * 同一个物理尺寸；outputs 里可以按输出名覆盖。 */
	snprintf(cfg->scale, sizeof(cfg->scale), "auto");
	cfg->outputs[0] = '\0';

	/* XWayland：默认懒启动（有人连 X 才起服务）。xwayland_scale=auto 取所有
	 * 输出缩放的最大值 —— X 只有一套坐标，只能按最密的那块屏来，低了就是放大糊。 */
	cfg->xwayland = true;
	snprintf(cfg->xwayland_scale, sizeof(cfg->xwayland_scale), "auto");
	cfg->xwayland_env = true;

	/* portal：vidow 当会话合成器时把 XDG_CURRENT_DESKTOP 设成 Vidow，
	 * 于是 xdg-desktop-portal 读的是 vidow-portals.conf。 */
	cfg->portal_conf_sync = true;
	snprintf(cfg->portal_desktop, sizeof(cfg->portal_desktop), "Vidow");

	const char *home = getenv("HOME");
	snprintf(cfg->shader_dir, sizeof(cfg->shader_dir), "%s/.config/vidow/shaders",
		home != NULL ? home : ".");
}

/* VIDOW_WINDOW_OPEN_MS=90 ./build/vidow — handy for tests. */
void vidow_config_apply_env(struct vidow_config *cfg) {
	char name[128];
	for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		const struct entry *e = &entries[i];
		snprintf(name, sizeof(name), "VIDOW_%s", e->key);
		for (char *p = name; *p != '\0'; p++) {
			*p = toupper((unsigned char)*p);
		}
		const char *value = getenv(name);
		if (value != NULL) {
			apply(cfg, e->key, value);
		}
	}
}

/* ------------------------------------------------------------ interface */

bool vidow_config_has(const char *key) {
	for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		if (strcmp(entries[i].key, key) == 0) {
			return true;
		}
	}
	return false;
}

bool vidow_config_set(struct vidow_config *cfg, const char *key,
		const char *value) {
	return apply(cfg, key, value);
}

static const struct entry *entry_for(const char *key) {
	for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		if (strcmp(entries[i].key, key) == 0) {
			return &entries[i];
		}
	}
	return NULL;
}

/* Read a key back as text, for configs written in a scripting language.
 * Four rotating buffers so one expression can read several keys. */
const char *vidow_config_get(const struct vidow_config *cfg,
		const char *key) {
	static char slots[4][64];
	static int next = 0;
	char *buf = slots[next];
	next = (next + 1) % 4;
	buf[0] = '\0';

	const struct entry *e = entry_for(key);
	if (e == NULL) {
		return NULL;
	}
	const void *field = (const char *)cfg + e->offset;
	switch (e->kind) {
	case K_INT:
		snprintf(buf, 64, "%d", *(const int *)field);
		break;
	case K_DOUBLE:
		snprintf(buf, 64, "%.10g", *(const double *)field);
		break;
	case K_BOOL:
		snprintf(buf, 64, "%s", *(const bool *)field ? "true" : "false");
		break;
	case K_EASING:
		snprintf(buf, 64, "%s",
			easing_names[*(const enum vidow_easing *)field]);
		break;
	case K_STRING:
		snprintf(buf, 64, "%s", (const char *)field);
		break;
	case K_COLOR: {
		const float *c = field;
		snprintf(buf, 64, "#%02x%02x%02x%02x",
			(int)(c[0] * 255.0f + 0.5f), (int)(c[1] * 255.0f + 0.5f),
			(int)(c[2] * 255.0f + 0.5f), (int)(c[3] * 255.0f + 0.5f));
		break;
	}
	}
	return buf;
}

/* 把所有键按顺序喂出去（IPC 的 config / state 用）。value 一律是文本 —— 外面
 * 拿到的和 config.lua 里读到的（vidow.get）是同一个东西。 */
void vidow_config_each(const struct vidow_config *cfg,
		void (*cb)(void *data, const char *key, const char *value), void *data) {
	if (cb == NULL) {
		return;
	}
	for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		const char *value = vidow_config_get(cfg, entries[i].key);
		cb(data, entries[i].key, value != NULL ? value : "");
	}
}

void vidow_config_log(const struct vidow_config *cfg) {
	wlr_log(WLR_INFO,
		"config: decor=%s radius=%.1f border=%.1f shadow=%.1f/%.1f,%d "
		"opacity=%.2f noise=%.2f blur=%s all=%s levels=%d strength=%.2f "
		"alpha_min=%.2f",
		cfg->decor_enabled ? "on" : "off", cfg->corner_radius,
		cfg->border_width, cfg->shadow_radius, cfg->shadow_color[3] * 255.0,
		cfg->shadow_offset_y, cfg->window_opacity, cfg->noise,
		cfg->blur_enabled ? "on" : "off", cfg->blur_all ? "on" : "off",
		cfg->blur_levels, cfg->blur_strength, cfg->blur_alpha_min);
	wlr_log(WLR_INFO, "config: force_render_hz=%d（视口外补帧的节流，0 = 不限）",
		cfg->force_render_hz);
	wlr_log(WLR_INFO, "config: activation_token_timeout=%d 秒（激活令牌多久作废，"
		"0 = 不过期）", cfg->activation_token_timeout);
	wlr_log(WLR_INFO, "config: honor_xdg_activation_with_invalid_serial=%s"
		"（serial 校验不过的激活请求理不理）",
		cfg->honor_xdg_activation_with_invalid_serial ? "on" : "off");
	wlr_log(WLR_INFO, "config: ipc=%s socket=%s",
		cfg->ipc ? "on" : "off",
		cfg->ipc_socket[0] != '\0' ? cfg->ipc_socket : "(自动)");
	wlr_log(WLR_INFO, "config: clipboard=%s primary=%s max=%d 字节",
		cfg->clipboard ? "on" : "off",
		cfg->clipboard_primary ? "on" : "off", cfg->clipboard_max_bytes);
	wlr_log(WLR_INFO,
		"config: scale=%s outputs=%s xwayland=%s xscale=%s xenv=%s",
		cfg->scale, cfg->outputs[0] != '\0' ? cfg->outputs : "-",
		cfg->xwayland ? "on" : "off", cfg->xwayland_scale,
		cfg->xwayland_env ? "on" : "off");
	wlr_log(WLR_INFO,
		"config: open=%dms close=%dms move=%dms layer=%d/%dms easing=%s "
		"shader=%s open=%s close=%s idle=%s",
		cfg->window_open_ms, cfg->window_close_ms, cfg->window_move_ms,
		cfg->layer_open_ms, cfg->layer_close_ms, easing_names[cfg->easing],
		cfg->shader_enabled ? "on" : "off",
		cfg->shader_open[0] ? cfg->shader_open : "-",
		cfg->shader_close[0] ? cfg->shader_close : "-",
		cfg->shader_idle[0] ? cfg->shader_idle : "-");
}

void vidow_config_load(struct vidow_config *cfg) {
	defaults(cfg);

	const char *home = getenv("HOME");
	const char *override = getenv("VIDOW_CONFIG");
	char path[PATH_MAX];
	if (override != NULL) {
		snprintf(path, sizeof(path), "%s", override);
	} else {
		snprintf(path, sizeof(path), "%s/.config/vidow/config",
			home != NULL ? home : ".");
	}

	FILE *f = fopen(path, "r");
	if (f != NULL) {
		char line[512];
		int lineno = 0;
		while (fgets(line, sizeof(line), f) != NULL) {
			lineno++;
			char *hash = strchr(line, '#');
			if (hash != NULL) {
				*hash = '\0';
			}
			char *eq = strchr(line, '=');
			if (eq == NULL) {
				continue;
			}
			*eq = '\0';
			char *key = line;
			char *value = eq + 1;
			while (isspace((unsigned char)*key)) key++;
			char *end = key + strlen(key);
			while (end > key && isspace((unsigned char)end[-1])) *--end = '\0';
			while (isspace((unsigned char)*value)) value++;
			end = value + strlen(value);
			while (end > value && isspace((unsigned char)end[-1])) *--end = '\0';
			if (*key == '\0') {
				continue;
			}
			if (!apply(cfg, key, value)) {
				wlr_log(WLR_ERROR, "config %s:%d: unknown key '%s'",
					path, lineno, key);
			}
		}
		fclose(f);
	}

	vidow_config_apply_env(cfg);

	vidow_config_log(cfg);
}
