#include "outputconf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/util/log.h>

#include "server.h"

bool vidow_output_conf_init(struct vidow_server *server) {
	wl_list_init(&server->output_confs);
	return true;
}

void vidow_output_conf_clear(struct vidow_server *server) {
	struct vidow_output_conf *conf, *tmp;
	wl_list_for_each_safe(conf, tmp, &server->output_confs, link) {
		wl_list_remove(&conf->link);
		free(conf);
	}
}

/* 一句话说清这组配置管了什么（日志和错误信息都用它）。 */
static void describe(const struct vidow_output_conf *conf, char *buf,
		size_t len) {
	size_t n = (size_t)snprintf(buf, len, "vidow.output{ %s }", conf->name);
	if (conf->has_scale) {
		n += (size_t)snprintf(buf + (n < len ? n : len - 1),
			len - (n < len ? n : len - 1), " scale=%s", conf->scale);
	}
	if (conf->has_pos) {
		n += (size_t)snprintf(buf + (n < len ? n : len - 1),
			len - (n < len ? n : len - 1), " 位置=%d,%d", conf->x, conf->y);
	}
	if (conf->has_mode) {
		n += (size_t)snprintf(buf + (n < len ? n : len - 1),
			len - (n < len ? n : len - 1), " 模式=%dx%d",
			conf->mode_w, conf->mode_h);
		if (conf->mode_hz > 0.0) {
			n += (size_t)snprintf(buf + (n < len ? n : len - 1),
				len - (n < len ? n : len - 1), "@%.2fHz", conf->mode_hz);
		}
	}
	if (conf->has_vrr) {
		n += (size_t)snprintf(buf + (n < len ? n : len - 1),
			len - (n < len ? n : len - 1), " vrr=%s", conf->vrr ? "on" : "off");
	}
	if (conf->has_vrr_on_demand) {
		n += (size_t)snprintf(buf + (n < len ? n : len - 1),
			len - (n < len ? n : len - 1), " 按需VRR=%s",
			conf->vrr_on_demand ? "on" : "off");
	}
	if (conf->has_depth) {
		n += (size_t)snprintf(buf + (n < len ? n : len - 1),
			len - (n < len ? n : len - 1), " %d位色", conf->depth);
	}
	if (conf->has_transform) {
		n += (size_t)snprintf(buf + (n < len ? n : len - 1),
			len - (n < len ? n : len - 1), " 方向=%d", (int)conf->transform);
	}
	if (conf->has_enabled) {
		n += (size_t)snprintf(buf + (n < len ? n : len - 1),
			len - (n < len ? n : len - 1), " %s", conf->enabled ? "开" : "关");
	}
	if (conf->has_hdr) {
		n += (size_t)snprintf(buf + (n < len ? n : len - 1),
			len - (n < len ? n : len - 1), " HDR=%s", conf->hdr ? "开" : "关");
		if (conf->has_sdr_white_nits) {
			n += (size_t)snprintf(buf + (n < len ? n : len - 1),
				len - (n < len ? n : len - 1), " 白点=%.0fnits",
				conf->sdr_white_nits);
		}
	}
	if (!conf->has_scale && !conf->has_pos && !conf->has_mode &&
			!conf->has_vrr && !conf->has_vrr_on_demand &&
			!conf->has_depth && !conf->has_transform &&
			!conf->has_hdr && !conf->has_sdr_white_nits &&
			!conf->has_enabled) {
		snprintf(buf + (n < len ? n : len - 1),
			len - (n < len ? n : len - 1), "（什么也没写：缩放和位置都自动）");
	}
}

struct vidow_output_conf *vidow_output_conf_add(struct vidow_server *server,
		const struct vidow_output_spec *spec, char *err, size_t errlen) {
	if (spec->name == NULL || spec->name[0] == '\0') {
		snprintf(err, errlen, "vidow.output{…} 得写 name = \"DP-3\"（哪块屏）");
		return NULL;
	}
	if (vidow_output_conf_for(server, spec->name) != NULL) {
		snprintf(err, errlen, "vidow.output{ name = \"%s\" } 写了两遍"
			"（一块屏一组，写一行就够）", spec->name);
		return NULL;
	}
	struct vidow_output_conf *conf = calloc(1, sizeof(*conf));
	if (conf == NULL) {
		snprintf(err, errlen, "内存不够了");
		return NULL;
	}
	snprintf(conf->name, sizeof(conf->name), "%s", spec->name);
	conf->has_scale = spec->has_scale;
	if (spec->scale != NULL) {
		snprintf(conf->scale, sizeof(conf->scale), "%s", spec->scale);
	}
	conf->has_pos = spec->has_pos;
	conf->x = spec->x;
	conf->y = spec->y;
	conf->has_mode = spec->has_mode;
	conf->mode_w = spec->mode_w;
	conf->mode_h = spec->mode_h;
	conf->mode_hz = spec->mode_hz;
	conf->has_vrr = spec->has_vrr;
	conf->vrr = spec->vrr;
	conf->has_vrr_on_demand = spec->has_vrr_on_demand;
	conf->vrr_on_demand = spec->vrr_on_demand;
	conf->has_depth = spec->has_depth;
	conf->depth = spec->depth;
	conf->has_hdr = spec->has_hdr;
	conf->hdr = spec->hdr;
	conf->has_sdr_white_nits = spec->has_sdr_white_nits;
	conf->sdr_white_nits = spec->sdr_white_nits;
	conf->has_transform = spec->has_transform;
	conf->transform = spec->transform;
	conf->has_enabled = spec->has_enabled;
	conf->enabled = spec->enabled;
	wl_list_insert(server->output_confs.prev, &conf->link);

	char what[160];
	describe(conf, what, sizeof(what));
	wlr_log(WLR_INFO, "output 配置: %s", what);
	return conf;
}

const struct vidow_output_conf *vidow_output_conf_for(
		const struct vidow_server *server, const char *name) {
	if (name == NULL) {
		return NULL;
	}
	const struct vidow_output_conf *conf;
	wl_list_for_each(conf, &server->output_confs, link) {
		if (strcmp(conf->name, name) == 0) {
			return conf;
		}
	}
	return NULL;
}
