/* drmprobe —— 合成器正在跑的时候，从外面看这块屏的「硬件那一侧」到底支持什么。
 *
 * 只读：不拿 DRM master，不提交任何 atomic 状态，不动画面。合成器占着 CRTC 也
 * 能安全读（主 plane 的格式表、connector 的色彩属性都是查询，不是修改）。
 *
 * 用途（计划第 1 步 / 第 2 步的验收）：
 *   - 主 plane 有没有 XRGB2101010 → 决定 10bit 扫描输出能不能做
 *   - connector 的 max bpc 现在是多少 → XRGB2101010 进了 framebuffer，链路仍可能
 *     是 8bpc，屏幕上根本看不出 10bit；只有这一项到 10 才算真的 10bit
 *   - Colorspace 枚举里有没有 BT2020_RGB、HDR_OUTPUT_METADATA 存不存在
 *     → 决定 HDR 那一步有没有落脚点
 *
 * 用法：sudo 不需要，只要对 /dev/dri/cardN 有读权限。
 *   ./build/drmprobe            # 自动挑第一块能打开的卡
 *   ./build/drmprobe /dev/dri/card1
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static const char *conn_type_name(uint32_t type) {
	switch (type) {
	case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
	case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
	case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
	case DRM_MODE_CONNECTOR_eDP: return "eDP";
	case DRM_MODE_CONNECTOR_VIRTUAL: return "Virtual";
	default: return "?";
	}
}

static const char *fmt_name(uint32_t fmt) {
	switch (fmt) {
	case DRM_FORMAT_XRGB8888: return "XRGB8888";
	case DRM_FORMAT_XBGR8888: return "XBGR8888";
	case DRM_FORMAT_ARGB8888: return "ARGB8888";
	case DRM_FORMAT_ABGR8888: return "ABGR8888";
	case DRM_FORMAT_RGBA8888: return "RGBA8888";
	case DRM_FORMAT_BGRA8888: return "BGRA8888";
	case DRM_FORMAT_RGBX8888: return "RGBX8888";
	case DRM_FORMAT_BGRX8888: return "BGRX8888";
	case DRM_FORMAT_XRGB2101010: return "XRGB2101010";
	case DRM_FORMAT_XBGR2101010: return "XBGR2101010";
	case DRM_FORMAT_ARGB2101010: return "ARGB2101010";
	case DRM_FORMAT_ABGR2101010: return "ABGR2101010";
	case DRM_FORMAT_XRGB16161616: return "XRGB16161616";
	case DRM_FORMAT_XBGR16161616: return "XBGR16161616";
	case DRM_FORMAT_ARGB16161616: return "ARGB16161616";
	case DRM_FORMAT_ABGR16161616: return "ABGR16161616";
	case DRM_FORMAT_XRGB16161616F: return "XRGB16161616F";
	case DRM_FORMAT_XBGR16161616F: return "XBGR16161616F";
	case DRM_FORMAT_ARGB16161616F: return "ARGB16161616F";
	case DRM_FORMAT_ABGR16161616F: return "ABGR16161616F";
	case DRM_FORMAT_NV12: return "NV12";
	case DRM_FORMAT_P010: return "P010";
	default: return NULL;
	}
}

/* connector 的色彩属性：这三项是 HDR / 10bit 的全部落脚点。 */
static void print_connector_color_props(int fd, uint32_t conn_id) {
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, conn_id,
		DRM_MODE_OBJECT_CONNECTOR);
	if (props == NULL) {
		return;
	}
	for (uint32_t k = 0; k < props->count_props; k++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[k]);
		if (p == NULL) {
			continue;
		}
		uint64_t v = props->prop_values[k];
		if (strcmp(p->name, "Colorspace") == 0) {
			printf("    Colorspace = %llu", (unsigned long long)v);
			for (int e = 0; e < p->count_enums; e++) {
				if ((uint64_t)p->enums[e].value == v &&
						v != 0) {
					printf("（%s）", p->enums[e].name);
				}
			}
			printf("\n");
			printf("      可选：");
			for (int e = 0; e < p->count_enums; e++) {
				printf("%s%s", e ? " / " : "", p->enums[e].name);
			}
			printf("\n");
		} else if (strcmp(p->name, "HDR_OUTPUT_METADATA") == 0) {
			printf("    HDR_OUTPUT_METADATA blob = %llu%s\n",
				(unsigned long long)v,
				v != 0 ? "（已设）" : "（未设）");
		} else if (strcmp(p->name, "max bpc") == 0) {
			/* **最容易被忽略的一项。** XRGB2101010 只说明 framebuffer 是
			 * 10 位；链路发几 bpcc 由它决定，默认常常还是 8。此值只有在
			 * modeset 提交里才会被 wlroots 改写（atomic.c:510）。 */
			printf("    max bpc = %llu（范围 %llu..%llu）\n",
				(unsigned long long)v,
				(unsigned long long)(p->values ? p->values[0] : 0),
				(unsigned long long)(p->values ? p->values[1] : 0));
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
}

/* 每块连接着的显示器 + 它能用的主 plane。PD-2 这种一根线两块屏（MST）时，
 * 同一个 CRTC 可能对应多个 connector —— 按 possible_crtcs 交集判归属。 */
static int probe(int fd) {
	drmModeRes *res = drmModeGetResources(fd);
	if (res == NULL) {
		return 1;
	}
	drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
	if (planes == NULL) {
		drmModeFreeResources(res);
		return 1;
	}

	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		if (c == NULL) {
			continue;
		}
		if (c->connection != DRM_MODE_CONNECTED) {
			drmModeFreeConnector(c);
			continue;
		}
		printf("%s-%u（%dx%d）\n", conn_type_name(c->connector_type),
			c->connector_type_id,
			c->modes ? c->modes[0].hdisplay : 0,
			c->modes ? c->modes[0].vdisplay : 0);
		print_connector_color_props(fd, c->connector_id);

		/* 当前挂在这个 connector 上的 CRTC（合成器正在用的那个）。 */
		uint32_t crtc = 0;
		for (int k = 0; k < c->count_encoders && crtc == 0; k++) {
			drmModeEncoder *e = drmModeGetEncoder(fd, c->encoders[k]);
			if (e == NULL) {
				continue;
			}
			if (e->encoder_id == c->encoder_id) {
				crtc = e->crtc_id;
			}
			drmModeFreeEncoder(e);
		}
		int crtc_idx = -1;
		for (int k = 0; k < res->count_crtcs; k++) {
			if (res->crtcs[k] == crtc) {
				crtc_idx = k;
			}
		}

		for (uint32_t i2 = 0; i2 < planes->count_planes; i2++) {
			drmModePlane *p = drmModeGetPlane(fd, planes->planes[i2]);
			if (p == NULL) {
				continue;
			}
			/* 只关心主 plane（type == PRIMARY）。 */
			int type = -1;
			drmModeObjectProperties *pp = drmModeObjectGetProperties(fd,
				p->plane_id, DRM_MODE_OBJECT_PLANE);
			for (uint32_t k = 0; pp && k < pp->count_props; k++) {
				drmModePropertyRes *pr =
					drmModeGetProperty(fd, pp->props[k]);
				if (pr != NULL && strcmp(pr->name, "type") == 0) {
					type = (int)pp->prop_values[k];
				}
				if (pr != NULL) {
					drmModeFreeProperty(pr);
				}
			}
			if (pp != NULL) {
				drmModeFreeObjectProperties(pp);
			}
			if (type != DRM_PLANE_TYPE_PRIMARY ||
					(crtc_idx >= 0 &&
					 !(p->possible_crtcs & (1u << crtc_idx)))) {
				drmModeFreePlane(p);
				continue;
			}

			int has8 = 0, has10 = 0, has_fp16 = 0, has_p010 = 0;
			printf("    主 plane %u 格式（%u 种）：", p->plane_id,
				p->count_formats);
			for (uint32_t k = 0; k < p->count_formats; k++) {
				uint32_t f = p->formats[k];
				has8 |= f == DRM_FORMAT_XRGB8888;
				has10 |= f == DRM_FORMAT_XRGB2101010;
				has_fp16 |= f == DRM_FORMAT_ABGR16161616F;
				has_p010 |= f == DRM_FORMAT_P010;
				const char *n = fmt_name(f);
				if (n != NULL) {
					printf(" %s", n);
				}
			}
			printf("\n      → 8bit %s，10bit %s，FP16 %s，P010 %s\n",
				has8 ? "有" : "无", has10 ? "有" : "无",
				has_fp16 ? "有" : "无", has_p010 ? "有" : "无");
			drmModeFreePlane(p);
			break; /* 一块屏只报它用的那一块主 plane */
		}
		drmModeFreeConnector(c);
	}

	drmModeFreePlaneResources(planes);
	drmModeFreeResources(res);
	return 0;
}

int main(int argc, char **argv) {
	const char *path = argc > 1 ? argv[1] : NULL;
	char buf[64];
	int fd = -1;
	if (path != NULL) {
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			perror(path);
			return 1;
		}
	} else {
		for (int i = 0; i < 4 && fd < 0; i++) {
			snprintf(buf, sizeof(buf), "/dev/dri/card%d", i);
			fd = open(buf, O_RDWR | O_CLOEXEC);
			if (fd < 0) {
				continue;
			}
			drmVersion *v = drmGetVersion(fd);
			if (v != NULL) {
				printf("设备 %s（driver %s）\n", buf, v->name);
				drmFreeVersion(v);
			}
			/* 通用 plane / atomic 属性只有在打开这两个 cap 之后才看得见。 */
			drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
			drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
			if (probe(fd) == 0) {
				close(fd);
				return 0;
			}
			close(fd);
			fd = -1;
		}
		if (fd < 0) {
			fprintf(stderr, "打不开任何 /dev/dri/card*（权限？）\n");
			return 1;
		}
	}

	drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
	int rc = probe(fd);
	close(fd);
	return rc;
}
