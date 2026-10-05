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
	if (!conf->has_scale && !conf->has_pos) {
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
