/* gammaprobe —— 校色 LUT 到底下到哪去了。
 *
 * 只做一件事：拿到 zwlr_gamma_control_manager_v1，选一块输出，下发一组每通道
 * 的 gamma 表，看合成器接不接。
 *
 * 为什么值得单独一个探针：Vidow 强制 gles2，而 gles2 不支持输出色彩变换。
 * 唯一还能让 1D LUT 生效的路子是「LUT 交给 KMS」—— 这条路看不见摸不着，
 * 唯一可靠的证据就是合成器回没回 gamma_size / 有没有回 failed。
 *
 * 用法：
 *   ./build/gammaprobe                    # 列出每块屏的 gamma_size（不发 LUT）
 *   ./build/gammaprobe --set DP-2         # 用默认暖色表（≈ 3400K）下一份
 *   ./build/gammaprobe --set DP-2 1 0.8 0.8   # 自己给 R G B 系数
 *   ./build/gammaprobe --reset DP-2       # 下线性表（等于关掉）
 *
 * 退出码：0 = 有 global；2 = 没有 global。
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>

#include "wlr-gamma-control-unstable-v1-client-protocol.h"

#define MAX_OUTPUTS 8

struct out {
	char name[64];
	struct wl_output *wl;
	struct zwlr_gamma_control_v1 *ctl;
	uint32_t size;
	int done;
};

struct state {
	struct wl_display *display;
	struct wl_registry *registry;
	struct zwlr_gamma_control_manager_v1 *manager;
	struct out outs[MAX_OUTPUTS];
	int n;
};

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
		int32_t pw, int32_t ph, int32_t sub, const char *mk, const char *md,
		int32_t tr) {
	(void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub;
	(void)mk; (void)md; (void)tr;
}
static void out_mode(void *d, struct wl_output *o, uint32_t f, int32_t w,
		int32_t h, int32_t r) {
	(void)d; (void)o; (void)f; (void)w; (void)h; (void)r;
}
static void out_scale(void *d, struct wl_output *o, int32_t s) {
	(void)d; (void)o; (void)s;
}
static void out_name(void *d, struct wl_output *o, const char *name) {
	(void)o;
	struct out *out = d;
	snprintf(out->name, sizeof(out->name), "%s", name);
}
static void out_description(void *d, struct wl_output *o, const char *s) {
	(void)d; (void)o; (void)s;
}
static void out_done(void *d, struct wl_output *o) {
	(void)d; (void)o;
}

static const struct wl_output_listener output_listener = {
	.geometry = out_geometry,
	.mode = out_mode,
	.scale = out_scale,
	.name = out_name,
	.description = out_description,
	.done = out_done,
};

static void ctl_gamma_size(void *d, struct zwlr_gamma_control_v1 *c,
		uint32_t size) {
	(void)c;
	struct out *out = d;
	out->size = size;
	out->done = 1;
}

static void ctl_failed(void *d, struct zwlr_gamma_control_v1 *c) {
	(void)c;
	struct out *out = d;
	out->size = 0;
	out->done = 1;
	fprintf(stderr, "  %s：合成器回了 failed（这块屏没有 KMS gamma 表，"
		"或者它不做校色 LUT）\n", out->name);
}

static const struct zwlr_gamma_control_v1_listener ctl_listener = {
	.gamma_size = ctl_gamma_size,
	.failed = ctl_failed,
};

static void reg_global(void *d, struct wl_registry *reg, uint32_t name,
		const char *iface, uint32_t version) {
	struct state *st = d;
	if (strcmp(iface, wl_output_interface.name) == 0) {
		if (st->n >= MAX_OUTPUTS) {
			return;
		}
		struct out *o = &st->outs[st->n++];
		o->wl = wl_registry_bind(reg, name, &wl_output_interface,
			version < 4 ? version : 4);
		wl_output_add_listener(o->wl, &output_listener, o);
	} else if (strcmp(iface, zwlr_gamma_control_manager_v1_interface.name) == 0) {
		st->manager = wl_registry_bind(reg, name,
			&zwlr_gamma_control_manager_v1_interface, 1);
	}
}

static void reg_remove(void *d, struct wl_registry *reg, uint32_t name) {
	(void)d; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = reg_global,
	.global_remove = reg_remove,
};

static void pump(struct state *st, int rounds) {
	for (int i = 0; i < rounds; i++) {
		wl_display_flush(st->display);
		wl_display_roundtrip(st->display);
	}
}

static int make_fd(size_t bytes) {
	char path[] = "/tmp/gammaprobe-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) {
		return -1;
	}
	unlink(path);
	if (ftruncate(fd, (off_t)bytes) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* 1D LUT：size 个 uint16，R 全段再 G 再 B。系数 = 满偏时的乘数（< 1 = 压暗
 * 该通道 → 画面偏暖 / 偏冷）。 */
static int send_lut(struct out *o, float kr, float kg, float kb) {
	if (o->size == 0) {
		return 0;
	}
	size_t entries = (size_t)o->size * 3;
	int fd = make_fd(entries * sizeof(uint16_t));
	if (fd < 0) {
		return -1;
	}
	uint16_t *tab = calloc(entries, sizeof(uint16_t));
	if (tab == NULL) {
		close(fd);
		return -1;
	}
	for (uint32_t i = 0; i < o->size; i++) {
		double v = (double)i / (double)(o->size - 1);
		tab[i] = (uint16_t)lround(fmin(1.0, v * kr) * 65535.0);
		tab[o->size + i] = (uint16_t)lround(fmin(1.0, v * kg) * 65535.0);
		tab[2 * o->size + i] = (uint16_t)lround(fmin(1.0, v * kb) * 65535.0);
	}
	if (pwrite(fd, tab, entries * sizeof(uint16_t), 0) < 0) {
		free(tab);
		close(fd);
		return -1;
	}
	free(tab);
	zwlr_gamma_control_v1_set_gamma(o->ctl, fd);
	close(fd);
	return 0;
}

int main(int argc, char **argv) {
	const char *set_name = NULL;
	float kr = 1.0f, kg = 0.8f, kb = 0.8f;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--set") == 0 && i + 1 < argc) {
			set_name = argv[++i];
		} else if (strcmp(argv[i], "--reset") == 0 && i + 1 < argc) {
			set_name = argv[++i];
			kg = 1.0f;
			kb = 1.0f;
		} else if (set_name != NULL && i + 2 < argc &&
				strcmp(argv[i], "--set") != 0) {
			kr = strtof(argv[i], NULL);
			kg = strtof(argv[i + 1], NULL);
			kb = strtof(argv[i + 2], NULL);
			i += 2;
		} else {
			fprintf(stderr, "用法：%s [--set 屏名 [R G B]] [--reset 屏名]\n",
				argv[0]);
			return 1;
		}
	}

	struct state st = {0};
	st.display = wl_display_connect(NULL);
	if (st.display == NULL) {
		fprintf(stderr, "连不上合成器（WAYLAND_DISPLAY 对吗？）\n");
		return 1;
	}
	st.registry = wl_display_get_registry(st.display);
	wl_registry_add_listener(st.registry, &registry_listener, &st);
	pump(&st, 2);

	if (st.manager == NULL) {
		printf("zwlr_gamma_control_manager_v1：**没有**\n");
		printf("  → 校色 / 亮度 / 色温工具（wlr-randr --gamma、icc-brightness、"
			"redshift 类）全部无效。\n");
		return 2;
	}
	printf("zwlr_gamma_control_manager_v1：有\n\n");

	int target = -1;
	for (int i = 0; i < st.n; i++) {
		struct out *o = &st.outs[i];
		o->ctl = zwlr_gamma_control_manager_v1_get_gamma_control(st.manager,
			o->wl);
		zwlr_gamma_control_v1_add_listener(o->ctl, &ctl_listener, o);
	}
	pump(&st, 3);

	for (int i = 0; i < st.n; i++) {
		struct out *o = &st.outs[i];
		printf("  %-10s gamma_size = %u%s\n", o->name, o->size,
			o->size > 0 ? "（KMS 校色 LUT 可用）" : "（不可用）");
		if (set_name != NULL && strcmp(o->name, set_name) == 0) {
			target = i;
		}
	}

	if (set_name != NULL) {
		if (target < 0) {
			fprintf(stderr, "没有叫 %s 的输出（见上面列表）\n", set_name);
			return 1;
		}
		printf("\n给 %s 下发 LUT：R=%.2f G=%.2f B=%.2f\n", set_name, kr, kg,
			kb);
		if (send_lut(&st.outs[target], kr, kg, kb) != 0) {
			fprintf(stderr, "下发失败\n");
			return 1;
		}
		/* set_gamma 之后合成器会在下一帧应用；这里只把请求冲出去。 */
		wl_display_flush(st.display);
		printf("已下发（看屏幕：偏暖/偏冷说明生效；没变化就是没生效）\n");
	}

	return 0;
}
