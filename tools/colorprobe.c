/* 色彩管理探针：合成器到底告诉了客户端什么。
 *
 * 客户端（游戏 / Proton / gamescope）判断 HDR 的唯一依据就是这里印出来的东西：
 *   - 没有 wp_color_manager_v1        → 一律按 sRGB，HDR 选项不会出现
 *   - 有 global，但 preferred 报 sRGB → 还是 SDR 模式
 *   - preferred 报 bt2020 + st2084_pq → 游戏才会去画 HDR
 *
 * 用法：
 *   ./build/colorprobe          全局 + 每块输出的当前空间 + 一个真实 surface 的 preferred
 *   ./build/colorprobe --pq     额外声明「我这份内容就是 PQ / BT.2020」
 *                              （验证合成器接不接、接了以后画面对不对）
 *
 * 退出码：0 = 有 wp_color_manager_v1；2 = 没有（2026-11-02 的基线）。
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "color-management-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define MAX_OUTPUTS 8

struct state;

struct out {
	struct state *st;
	struct wl_output *wl;
	char name[64];
	struct wp_color_management_output_v1 *cmo;
};

struct state {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wp_color_manager_v1 *cm;

	struct out outs[MAX_OUTPUTS];
	int n_outputs;

	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	struct wp_color_management_surface_v1 *cm_surface;
	struct wp_image_description_v1 *pq_desc;
	bool pq_ready;

	bool saw_pq;
	bool saw_bt2020;
	bool saw_srgb_only;
	bool want_pq;

	char features[512];
	char tf_list[512];
	char prim_list[256];
};

struct desc_ctx {
	struct state *st;
	char label[128];
};

/* ------------------------------------------------------------ 名字映射 */

static const char *primaries_name(uint32_t p) {
	switch (p) {
	case WP_COLOR_MANAGER_V1_PRIMARIES_SRGB: return "srgb";
	case WP_COLOR_MANAGER_V1_PRIMARIES_BT2020: return "bt2020";
	case WP_COLOR_MANAGER_V1_PRIMARIES_CIE1931_XYZ: return "cie1931_xyz";
	case WP_COLOR_MANAGER_V1_PRIMARIES_DCI_P3: return "dci_p3";
	case WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3: return "display_p3";
	case WP_COLOR_MANAGER_V1_PRIMARIES_ADOBE_RGB: return "adobe_rgb";
	case WP_COLOR_MANAGER_V1_PRIMARIES_GENERIC_FILM: return "generic_film";
	case WP_COLOR_MANAGER_V1_PRIMARIES_PAL: return "pal";
	case WP_COLOR_MANAGER_V1_PRIMARIES_PAL_M: return "pal_m";
	case WP_COLOR_MANAGER_V1_PRIMARIES_NTSC: return "ntsc";
	default: return "?";
	}
}

static const char *tf_name(uint32_t tf) {
	switch (tf) {
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886: return "bt1886";
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22: return "gamma22";
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA28: return "gamma28";
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST240: return "st240";
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR: return "ext_linear";
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_LOG_100: return "log100";
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_LOG_316: return "log316";
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_XVYCC: return "xvycc";
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB: return "srgb";
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_SRGB: return "ext_srgb";
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ: return "st2084_pq";
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST428: return "st428";
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG: return "hlg";
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4:
		return "compound_power_2_4";
	default: return "?";
	}
}

static const char *feature_name(uint32_t f) {
	switch (f) {
	case WP_COLOR_MANAGER_V1_FEATURE_ICC_V2_V4: return "icc_v2_v4";
	case WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC: return "parametric";
	case WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES: return "set_primaries";
	case WP_COLOR_MANAGER_V1_FEATURE_SET_TF_POWER: return "set_tf_power";
	case WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES: return "set_luminances";
	case WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES:
		return "set_mastering_primaries";
	case WP_COLOR_MANAGER_V1_FEATURE_EXTENDED_TARGET_VOLUME:
		return "extended_target_volume";
	case WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_SCRGB: return "windows_scrgb";
	default: return "?";
	}
}

static void append(char *buf, size_t len, const char *s) {
	size_t n = strlen(buf);
	snprintf(buf + n, len - n, "%s%s", n ? " " : "", s);
}

/* ------------------------------------------------------- 描述信息打印 */

static void info_done(void *data, struct wp_image_description_info_v1 *info) {
	free(data);
	wp_image_description_info_v1_destroy(info);
	printf("\n");
}

static void info_icc(void *data, struct wp_image_description_info_v1 *info,
		int32_t fd, uint32_t size) {
	(void)data; (void)info;
	printf(" icc=%uB", size);
	if (fd >= 0) {
		close(fd);
	}
}

static void info_primaries(void *data, struct wp_image_description_info_v1 *info,
		int32_t rx, int32_t ry, int32_t gx, int32_t gy,
		int32_t bx, int32_t by, int32_t wx, int32_t wy) {
	(void)data; (void)info;
	printf(" primaries=(%.3f,%.3f)(%.3f,%.3f)(%.3f,%.3f) 白点(%.3f,%.3f)",
		rx / 1e6, ry / 1e6, gx / 1e6, gy / 1e6,
		bx / 1e6, by / 1e6, wx / 1e6, wy / 1e6);
}

static void info_primaries_named(void *data,
		struct wp_image_description_info_v1 *info, uint32_t p) {
	struct desc_ctx *ctx = data;
	(void)info;
	if (p == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020) {
		ctx->st->saw_bt2020 = true;
	}
	printf(" primaries=%s", primaries_name(p));
}

static void info_tf_power(void *data, struct wp_image_description_info_v1 *info,
		uint32_t eexp) {
	(void)data; (void)info;
	printf(" tf=power(%.4f)", eexp / 10000.0);
}

static void info_tf_named(void *data, struct wp_image_description_info_v1 *info,
		uint32_t tf) {
	struct desc_ctx *ctx = data;
	(void)info;
	if (tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ) {
		ctx->st->saw_pq = true;
	} else if (tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB ||
			tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22) {
		ctx->st->saw_srgb_only = true;
	}
	printf(" tf=%s", tf_name(tf));
}

static void info_luminances(void *data,
		struct wp_image_description_info_v1 *info,
		uint32_t min_lum, uint32_t max_lum, uint32_t reference_lum) {
	(void)data; (void)info;
	printf(" 亮度 min=%u max=%u 参考白=%u", min_lum, max_lum, reference_lum);
}

static void info_target_luminance(void *data,
		struct wp_image_description_info_v1 *info,
		uint32_t min_lum, uint32_t max_lum) {
	(void)data; (void)info;
	printf(" 目标 min=%u max=%u", min_lum, max_lum);
}

static void info_target_primaries(void *data,
		struct wp_image_description_info_v1 *info,
		int32_t rx, int32_t ry, int32_t gx, int32_t gy,
		int32_t bx, int32_t by, int32_t wx, int32_t wy) {
	(void)data; (void)info;
	printf(" 目标primaries=(%.3f,%.3f)(%.3f,%.3f)(%.3f,%.3f) 白点(%.3f,%.3f)",
		rx / 1e6, ry / 1e6, gx / 1e6, gy / 1e6,
		bx / 1e6, by / 1e6, wx / 1e6, wy / 1e6);
}

static void info_target_cll(void *data,
		struct wp_image_description_info_v1 *info, uint32_t cll) {
	(void)data; (void)info;
	printf(" max_cll=%u", cll);
}

static void info_target_fall(void *data,
		struct wp_image_description_info_v1 *info, uint32_t fall) {
	(void)data; (void)info;
	printf(" max_fall=%u", fall);
}

static const struct wp_image_description_info_v1_listener info_listener = {
	.done = info_done,
	.icc_file = info_icc,
	.primaries = info_primaries,
	.primaries_named = info_primaries_named,
	.tf_power = info_tf_power,
	.tf_named = info_tf_named,
	.luminances = info_luminances,
	.target_primaries = info_target_primaries,
	.target_luminance = info_target_luminance,
	.target_max_cll = info_target_cll,
	.target_max_fall = info_target_fall,
};

static void desc_failed(void *data, struct wp_image_description_v1 *desc,
		uint32_t cause, const char *msg) {
	struct desc_ctx *ctx = data;
	(void)desc;
	printf("  %s 失败（cause=%u：%s）\n", ctx->label, cause,
		msg != NULL ? msg : "");
	free(ctx);
}

static void desc_ready(void *data, struct wp_image_description_v1 *desc,
		uint32_t identity) {
	(void)identity;
	struct desc_ctx *ctx = data;
	printf("  %s", ctx->label);
	struct wp_image_description_info_v1 *info =
		wp_image_description_v1_get_information(desc);
	wp_image_description_info_v1_add_listener(info, &info_listener, ctx);
}

static void desc_ready2(void *data, struct wp_image_description_v1 *desc,
		uint32_t hi, uint32_t lo) {
	(void)hi; (void)lo;
	struct desc_ctx *ctx = data;
	printf("  %s", ctx->label);
	struct wp_image_description_info_v1 *info =
		wp_image_description_v1_get_information(desc);
	wp_image_description_info_v1_add_listener(info, &info_listener, ctx);
	if (ctx->st->pq_desc == desc) {
		ctx->st->pq_ready = true;
	}
}

static const struct wp_image_description_v1_listener desc_listener = {
	.failed = desc_failed,
	.ready = desc_ready,
	.ready2 = desc_ready2,
};

static void describe_later(struct state *st, const char *label,
		struct wp_image_description_v1 *desc) {
	struct desc_ctx *ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return;
	}
	ctx->st = st;
	snprintf(ctx->label, sizeof(ctx->label), "%s", label);
	wp_image_description_v1_add_listener(desc, &desc_listener, ctx);
}

/* --------------------------------------------------------------- 输出 */

static void out_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
		int32_t pw, int32_t ph, int32_t subpixel, const char *make,
		const char *model, int32_t transform) {
	(void)data; (void)o; (void)x; (void)y; (void)pw; (void)ph;
	(void)subpixel; (void)make; (void)model; (void)transform;
}
static void out_mode(void *data, struct wl_output *o, uint32_t flags,
		int32_t w, int32_t h, int32_t refresh) {
	(void)data; (void)o; (void)flags; (void)w; (void)h; (void)refresh;
}
static void out_scale(void *data, struct wl_output *o, int32_t factor) {
	(void)data; (void)o; (void)factor;
}
static void out_name(void *data, struct wl_output *o, const char *name) {
	struct out *out = data;
	(void)o;
	snprintf(out->name, sizeof(out->name), "%s", name);
}
static void out_description(void *data, struct wl_output *o, const char *desc) {
	(void)data; (void)o; (void)desc;
}
static void out_done(void *data, struct wl_output *o) {
	(void)data; (void)o;
}

static const struct wl_output_listener output_listener = {
	.geometry = out_geometry,
	.mode = out_mode,
	.scale = out_scale,
	.name = out_name,
	.description = out_description,
	.done = out_done,
};

static void cmo_image_changed(void *data,
		struct wp_color_management_output_v1 *cmo) {
	(void)data; (void)cmo;
	/* 不重复查询：这一趟只印一份快照，够判断当前模式了。 */
}

static const struct wp_color_management_output_v1_listener cmo_listener = {
	.image_description_changed = cmo_image_changed,
};

/* --------------------------------------------------- 色彩管理 global */

static void cm_supported_intent(void *data, struct wp_color_manager_v1 *cm,
		uint32_t intent) { (void)data; (void)cm; (void)intent; }

static void cm_supported_feature(void *data, struct wp_color_manager_v1 *cm,
		uint32_t feature) {
	struct state *st = data;
	(void)cm;
	append(st->features, sizeof(st->features), feature_name(feature));
}

static void cm_supported_tf(void *data, struct wp_color_manager_v1 *cm,
		uint32_t tf) {
	struct state *st = data;
	(void)cm;
	append(st->tf_list, sizeof(st->tf_list), tf_name(tf));
}

static void cm_supported_primaries(void *data,
		struct wp_color_manager_v1 *cm, uint32_t p) {
	struct state *st = data;
	(void)cm;
	append(st->prim_list, sizeof(st->prim_list), primaries_name(p));
}

static void cm_done(void *data, struct wp_color_manager_v1 *cm) {
	(void)data; (void)cm;
}

static const struct wp_color_manager_v1_listener cm_listener = {
	.supported_intent = cm_supported_intent,
	.supported_feature = cm_supported_feature,
	.supported_tf_named = cm_supported_tf,
	.supported_primaries_named = cm_supported_primaries,
	.done = cm_done,
};

/* --------------------------------------------------------------- 窗口 */

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
	(void)data;
	xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void xdg_surface_configure(void *data, struct xdg_surface *xs,
		uint32_t serial) {
	(void)data;
	xdg_surface_ack_configure(xs, serial);
}
static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *t,
		int32_t w, int32_t h, struct wl_array *states) {
	(void)data; (void)t; (void)w; (void)h; (void)states;
}
static void toplevel_close(void *data, struct xdg_toplevel *t) {
	(void)data; (void)t;
}
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

static int make_shm_file(size_t size) {
	int fd = memfd_create("colorprobe", MFD_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static struct wl_buffer *make_buffer(struct state *st, int w, int h) {
	size_t stride = (size_t)w * 4;
	size_t size = stride * (size_t)h;
	int fd = make_shm_file(size);
	if (fd < 0) {
		return NULL;
	}
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	for (size_t i = 0; i < size / 4; i++) {
		px[i] = 0xFF808080; /* 不透明中灰 */
	}
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(st->shm, fd, (int32_t)size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h,
		(int32_t)stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

/* ------------------------------------------------------------ registry */

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
		const char *iface, uint32_t version) {
	struct state *st = data;

	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		st->compositor = wl_registry_bind(reg, name,
			&wl_compositor_interface, version < 4 ? version : 4);
	} else if (strcmp(iface, wl_shm_interface.name) == 0) {
		st->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
		st->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(st->wm_base, &wm_listener, st);
	} else if (strcmp(iface, wl_output_interface.name) == 0) {
		if (st->n_outputs >= MAX_OUTPUTS) {
			return;
		}
		struct out *o = &st->outs[st->n_outputs++];
		o->st = st;
		o->wl = wl_registry_bind(reg, name, &wl_output_interface,
			version < 4 ? version : 4);
		wl_output_add_listener(o->wl, &output_listener, o);
	} else if (strcmp(iface, wp_color_manager_v1_interface.name) == 0) {
		st->cm = wl_registry_bind(reg, name,
			&wp_color_manager_v1_interface, version < 2 ? version : 2);
		wp_color_manager_v1_add_listener(st->cm, &cm_listener, st);
	}
}

static void reg_global_remove(void *data, struct wl_registry *reg,
		uint32_t name) {
	(void)data; (void)reg; (void)name;
}

/* 把往返多推几轮：image description 是异步的 —— 先 ready（或 ready2），
 * 再 get_information，再一串 info 事件，最后 done。一轮 roundtrip 只够走一步。 */
static void pump(struct state *st, int rounds) {
	for (int i = 0; i < rounds; i++) {
		wl_display_flush(st->display);
		wl_display_roundtrip(st->display);
		usleep(5000);
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = reg_global,
	.global_remove = reg_global_remove,
};

/* -------------------------------------------------------------- 反馈 */

static void fb_preferred_changed(void *data,
		struct wp_color_management_surface_feedback_v1 *fb, uint32_t id) {
	(void)id;
	struct state *st = data;
	struct wp_image_description_v1 *d = wp_color_management_surface_feedback_v1_get_preferred(fb);
	describe_later(st, "surface 偏好（游戏读到的）:", d);
}

static void fb_preferred_changed2(void *data,
		struct wp_color_management_surface_feedback_v1 *fb,
		uint32_t hi, uint32_t lo) {
	(void)hi; (void)lo;
	struct state *st = data;
	struct wp_image_description_v1 *d = wp_color_management_surface_feedback_v1_get_preferred(fb);
	describe_later(st, "surface 偏好（游戏读到的）:", d);
}

static const struct wp_color_management_surface_feedback_v1_listener fb_listener = {
	.preferred_changed = fb_preferred_changed,
	.preferred_changed2 = fb_preferred_changed2,
};

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv) {
	struct state st = {0};
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--pq") == 0) {
			st.want_pq = true;
		} else {
			fprintf(stderr, "用法：%s [--pq]\n", argv[0]);
			return 1;
		}
	}

	st.display = wl_display_connect(NULL);
	if (st.display == NULL) {
		fprintf(stderr, "连不上合成器（WAYLAND_DISPLAY 对吗？）\n");
		return 1;
	}
	st.registry = wl_display_get_registry(st.display);
	wl_registry_add_listener(st.registry, &registry_listener, &st);
	wl_display_roundtrip(st.display);
	wl_display_roundtrip(st.display);

	printf("== 色彩管理 ==\n");
	if (st.cm == NULL) {
		printf("  wp_color_manager_v1: **没有**\n");
	} else {
		printf("  wp_color_manager_v1: 有（v%u）\n",
			wl_proxy_get_version((struct wl_proxy *)st.cm));
		printf("    features: %s\n", st.features[0] ? st.features : "（没报）");
		printf("    TF:      %s\n", st.tf_list[0] ? st.tf_list : "（没报）");
		printf("    primaries: %s\n", st.prim_list[0] ? st.prim_list : "（没报）");
	}

	printf("\n== 输出 ==\n");
	if (st.n_outputs == 0) {
		printf("  （没看到任何 wl_output）\n");
	}
	for (int i = 0; i < st.n_outputs; i++) {
		struct out *o = &st.outs[i];
		printf("  %s:\n", o->name[0] ? o->name : "(未命名)");
		if (st.cm == NULL) {
			printf("    当前空间: 不知道（没有 color-management global）\n");
			continue;
		}
		o->cmo = wp_color_manager_v1_get_output(st.cm, o->wl);
		wp_color_management_output_v1_add_listener(o->cmo, &cmo_listener, o);
		struct wp_image_description_v1 *d =
			wp_color_management_output_v1_get_image_description(o->cmo);
		char label[96];
		snprintf(label, sizeof(label), "当前空间:");
		describe_later(&st, label, d);
	}
	pump(&st, 4);

	if (st.cm != NULL && st.compositor != NULL && st.shm != NULL &&
			st.wm_base != NULL) {
		printf("\n== 一个真实 surface 的反馈 ==\n");
		st.surface = wl_compositor_create_surface(st.compositor);
		st.xdg_surface = xdg_wm_base_get_xdg_surface(st.wm_base, st.surface);
		xdg_surface_add_listener(st.xdg_surface, &xdg_surface_listener, &st);
		st.toplevel = xdg_surface_get_toplevel(st.xdg_surface);
		xdg_toplevel_add_listener(st.toplevel, &toplevel_listener, &st);
		xdg_toplevel_set_title(st.toplevel, "colorprobe");
		xdg_toplevel_set_app_id(st.toplevel, "colorprobe");

		/* 先不带缓冲 commit 一次，等 configure —— 带着 buffer 提交会被
		 * xdg-shell 判协议错误（"attached a buffer before configure event"）。 */
		wl_surface_commit(st.surface);
		pump(&st, 2);

		struct wl_buffer *buf = make_buffer(&st, 320, 200);
		if (buf != NULL) {
			wl_surface_attach(st.surface, buf, 0, 0);
			wl_surface_damage_buffer(st.surface, 0, 0, 320, 200);
		}
		wl_surface_commit(st.surface);
		pump(&st, 3);

		struct wp_color_management_surface_feedback_v1 *fb =
			wp_color_manager_v1_get_surface_feedback(st.cm, st.surface);
		wp_color_management_surface_feedback_v1_add_listener(fb,
			&fb_listener, &st);
		struct wp_image_description_v1 *d =
			wp_color_management_surface_feedback_v1_get_preferred(fb);
		describe_later(&st, "preferred:", d);
		pump(&st, 4);

		if (st.want_pq) {
			printf("\n== 自报 PQ 内容（--pq）==\n");
			struct wp_image_description_creator_params_v1 *c =
				wp_color_manager_v1_create_parametric_creator(st.cm);
			wp_image_description_creator_params_v1_set_tf_named(c,
				WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
			wp_image_description_creator_params_v1_set_primaries_named(c,
				WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
			wp_image_description_creator_params_v1_set_luminances(c,
				1, 1000, 203);
			st.pq_desc = wp_image_description_creator_params_v1_create(c);
			describe_later(&st, "我声明的:", st.pq_desc);
			for (int i = 0; i < 8 && !st.pq_ready; i++) {
				pump(&st, 1);
			}
			if (st.pq_ready) {
				st.cm_surface = wp_color_manager_v1_get_surface(st.cm,
					st.surface);
				wp_color_management_surface_v1_set_image_description(
					st.cm_surface, st.pq_desc,
					WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
				wl_display_roundtrip(st.display);
				printf("  已把 PQ/BT.2020 设到 surface 上\n");
			} else {
				printf("  合成器没让这个描述 ready —— 不支持 PQ 参数化\n");
			}
		}
	}

	wl_display_roundtrip(st.display);

	printf("\n== 结论 ==\n");
	if (st.cm == NULL) {
		printf("  客户端只会看到 sRGB → 游戏/Proton 的 HDR 选项不会出现。\n");
	} else if (st.saw_pq && st.saw_bt2020) {
		printf("  HDR 可见：preferred 里有 bt2020 + st2084_pq。\n");
	} else {
		printf("  有色彩管理，但当前是 SDR（preferred 没有 pq/bt2020）。\n");
	}

	return st.cm == NULL ? 2 : 0;
}
