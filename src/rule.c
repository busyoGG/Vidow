#include "rule.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <wlr/util/log.h>

#include "server.h"

/* 子串匹配、大小写不敏感（自己写：strcasestr 要 _GNU_SOURCE，本项目按 POSIX 编）。
 * pattern 为空 = 不限（匹配一切）。 */
static bool contains(const char *haystack, const char *pattern) {
	if (pattern == NULL || pattern[0] == '\0') {
		return true;
	}
	if (haystack == NULL) {
		return false;
	}
	size_t n = strlen(pattern);
	for (const char *p = haystack; *p != '\0'; p++) {
		if (strncasecmp(p, pattern, n) == 0) {
			return true;
		}
	}
	return false;
}

bool vidow_rule_init(struct vidow_server *server) {
	wl_list_init(&server->rules);
	return true;
}

struct vidow_rule *vidow_rule_add(struct vidow_server *server,
		const struct vidow_rule_spec *spec) {
	struct vidow_rule *rule = calloc(1, sizeof(*rule));
	if (rule == NULL) {
		return NULL;
	}
	if (spec->app_id != NULL) {
		snprintf(rule->app_id, sizeof(rule->app_id), "%s", spec->app_id);
	}
	if (spec->title != NULL) {
		snprintf(rule->title, sizeof(rule->title), "%s", spec->title);
	}
	if (spec->klass != NULL) {
		snprintf(rule->klass, sizeof(rule->klass), "%s", spec->klass);
	}
	rule->has_force_render = spec->has_force_render;
	rule->force_render = spec->force_render;
	rule->has_hz = spec->has_hz;
	rule->hz = spec->hz;
	rule->has_focus_on_open = spec->has_focus_on_open;
	rule->focus_on_open = spec->focus_on_open;
	rule->has_focus_on_activate = spec->has_focus_on_activate;
	rule->focus_on_activate = spec->focus_on_activate;
	rule->has_honor_invalid_serial = spec->has_honor_invalid_serial;
	rule->honor_invalid_serial = spec->honor_invalid_serial;
	wl_list_insert(server->rules.prev, &rule->link);

	char hz_text[32];
	if (!spec->has_hz) {
		snprintf(hz_text, sizeof(hz_text), "配置默认");
	} else if (spec->hz <= 0) {
		snprintf(hz_text, sizeof(hz_text), "不限");
	} else {
		snprintf(hz_text, sizeof(hz_text), "%dHz", spec->hz);
	}
	wlr_log(WLR_INFO, "rule: app_id=%s title=%s class=%s -> force_render=%s "
		"补帧=%s 开窗口抢焦点=%s 激活理它=%s 无效serial也理=%s",
		rule->app_id[0] != '\0' ? rule->app_id : "*",
		rule->title[0] != '\0' ? rule->title : "*",
		rule->klass[0] != '\0' ? rule->klass : "*",
		spec->has_force_render ?
			(spec->force_render ? "on" : "off") : "不变",
		hz_text,
		spec->has_focus_on_open ?
			(spec->focus_on_open ? "on" : "off") : "不变",
		spec->has_focus_on_activate ?
			(spec->focus_on_activate ? "on" : "off") : "不变",
		spec->has_honor_invalid_serial ?
			(spec->honor_invalid_serial ? "on" : "off") : "不变");
	return rule;
}

void vidow_rule_clear(struct vidow_server *server) {
	struct vidow_rule *rule, *tmp;
	wl_list_for_each_safe(rule, tmp, &server->rules, link) {
		wl_list_remove(&rule->link);
		free(rule);
	}
}

bool vidow_rule_matches(const struct vidow_rule *rule,
		const struct vidow_view *view) {
	const char *app_id = vidow_view_app_id(view);
	const char *title = vidow_view_title(view);
	/* X11 的 WM_CLASS 就在 x11 结构里（Wayland 那边没有对应物）。 */
	const char *klass = view->kind == VIDOW_VIEW_X11 && view->x11 != NULL ?
		view->x11->class : NULL;

	if (!contains(app_id, rule->app_id)) {
		return false;
	}
	if (!contains(title, rule->title)) {
		return false;
	}
	if (!contains(klass, rule->klass)) {
		return false;
	}
	/* 三个字段全空的规则匹配所有窗口 —— 允许，但日志里看得出来（打成 * / *）。 */
	return true;
}

bool vidow_rule_apply(struct vidow_server *server, struct vidow_view *view) {
	if (view == NULL || view->popup) {
		return false;
	}
	bool want = false;
	bool has_hz = false;
	int hz = 0;
	/* 焦点行为：规则没写就回到默认（都抢 / 都理）—— 「最后一条匹配的规则说了算」
	 * 意味着没写的字段回到默认值，而不是留着上一条规则给的值。 */
	bool focus_on_open = true;
	bool focus_on_activate = true;
	/* 这个的默认值是 false（不理）：规则没写就是不理。 */
	bool honor_invalid_serial = false;
	struct vidow_rule *rule;
	wl_list_for_each(rule, &server->rules, link) {
		if (!vidow_rule_matches(rule, view)) {
			continue;
		}
		/* 后面的规则覆盖前面的（跟配置里的先后顺序一致）。 */
		if (rule->has_force_render) {
			want = rule->force_render;
		}
		if (rule->has_hz) {
			has_hz = true;
			hz = rule->hz;
		}
		if (rule->has_focus_on_open) {
			focus_on_open = rule->focus_on_open;
		}
		if (rule->has_focus_on_activate) {
			focus_on_activate = rule->focus_on_activate;
		}
		if (rule->has_honor_invalid_serial) {
			honor_invalid_serial = rule->honor_invalid_serial;
		}
	}
	/* 没写 hz 就用全局的 force_render_hz（0 = 不限）。 */
	if (!has_hz) {
		hz = server->config.force_render_hz;
	}
	int interval = 0;
	if (hz > 0) {
		interval = 1000 / (hz > 1000 ? 1000 : hz);
		if (interval < 1) {
			interval = 1;
		}
	}

	view->focus_on_open = focus_on_open;
	view->focus_on_activate = focus_on_activate;
	view->honor_invalid_serial = honor_invalid_serial;

	bool changed = view->force_render != want;
	view->force_render = want;
	view->pump_interval_ms = interval;
	if (!changed) {
		return false;
	}
	wlr_log(WLR_INFO, "rule: %s 强制渲染（视口外补帧）%s%s",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
		want ? "开" : "关",
		interval > 0 ? "" : "（不限速）");
	return true;
}

void vidow_rule_apply_all(struct vidow_server *server) {
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		vidow_rule_apply(server, view);
	}
}

size_t vidow_rule_count(const struct vidow_server *server) {
	size_t n = 0;
	const struct vidow_rule *rule;
	wl_list_for_each(rule, &server->rules, link) {
		n++;
	}
	return n;
}

const struct vidow_rule *vidow_rule_at(const struct vidow_server *server,
		size_t index) {
	size_t i = 0;
	const struct vidow_rule *rule;
	wl_list_for_each(rule, &server->rules, link) {
		if (i == index) {
			return rule;
		}
		i++;
	}
	return NULL;
}
