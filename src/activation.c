/*
 * xdg-activation-v1 —— 服务端自己实现，不用 wlroots 那份（wlr_xdg_activation_v1）。
 *
 * 为什么不用现成的：wlroots 把 serial 校验写死了。客户端 set_serial 给的 serial 要是
 * 「从来没发给过这个客户端」（工具包偷懒写 0、或者拿的是别的进程 / 别的 seat 的
 * serial），commit 时它就不登记这个令牌（日志 "Rejecting token commit request:
 * serial 0 was never given to client"），后面 activate 就当成未知令牌丢掉
 * （"Rejecting activate request: unknown token"）—— 表现就是「点了通知，窗口不动」。
 * niri 为这种情况留了个开关 honor-xdg-activation-with-invalid-serial（在它配置的
 * debug 段里）。vidow 要有同样的开关，就只能自己接管这个协议：校验规则在自己手里。
 *
 * 语义对齐 niri（也是 wlroots 现在的行为，除了能不能关）：
 *   - commit 一律回 done(token)：协议要求发，客户端不该知道令牌有没有被登记；
 *   - serial 校验不过：默认（开关关）令牌照发但以后不理它 → activate 被忽略；
 *     全局开关打开（honor_xdg_activation_with_invalid_serial = true）→ 所有窗口都照样
 *     激活；窗口规则 honor_invalid_serial = true（见 rule.c）→ 只有那个应用照样激活
 *     （要开小口子用这个，代价小）；
 *   - 没给 seat / seat 不认识 → 不做校验（等于没给 serial，照常理）；
 *   - 令牌按 activation_token_timeout 作废（0 = 不过期），创建它的客户端一断就作废；
 *   - activate 里带的 surface 才是要顶的那个（守护进程式：通知守护进程建令牌时
 *     不带 surface，被激活的应用自己在 activate 里带）—— 没带才退回 set_surface 那个。
 *
 * 拿到的窗口交给 vidow_view_activate()：窗口规则 focus_on_activate、跨屏 / 特殊
 * 工作区端出来、还没上屏就记账，都在那里（见 view.c）。
 */

#include "server.h"

#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "xdg-activation-v1-server-protocol.h"

/* 令牌结构体（struct vidow_activation_token）在 server.h —— 跟别的模块一样，
 * 那边的结构体是这个项目的"内部 API 目录"，IPC 也要把它报出来。 */

/* 客户端手上那个 xdg_activation_token_v1 对象（还没 commit 的攒参数阶段）。 */
struct vidow_token_build {
	struct vidow_server *server;
	struct wl_client *client;
	struct wl_resource *resource;

	struct wlr_surface *surface;
	struct wl_listener surface_destroy;
	bool surface_listener_added;

	char *app_id;
	uint32_t serial;
	bool has_serial;
	bool serial_ok;
	bool committed;
};

static const struct xdg_activation_token_v1_interface token_impl;

/* ------------------------------------------------------------- 小工具 */

static struct vidow_activation_token *token_find(struct vidow_server *server,
		const char *name) {
	struct vidow_activation_token *token;
	wl_list_for_each(token, &server->activation_tokens, link) {
		if (strcmp(token->name, name) == 0) {
			return token;
		}
	}
	return NULL;
}

/* 令牌出生到现在多少秒（小数）。 */
static double token_age(struct vidow_activation_token *token) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)(now.tv_sec - token->created.tv_sec) +
		(double)(now.tv_nsec - token->created.tv_nsec) / 1e9;
}

/* 过期了吗。activation_token_timeout = 0（默认）= 不过期。 */
static bool token_expired(struct vidow_server *server,
		struct vidow_activation_token *token) {
	int timeout = server->config.activation_token_timeout;
	return timeout > 0 && token_age(token) >= (double)timeout;
}

static void token_free(struct vidow_activation_token *token) {
	wl_list_remove(&token->link);
	wl_list_remove(&token->client_destroy.link);
	if (token->surface_listener_added) {
		wl_list_remove(&token->surface_destroy.link);
	}
	free(token->name);
	free(token->app_id);
	free(token);
}

/* 建令牌的客户端断了：这个令牌没人能再用了。 */
static void token_client_gone(struct wl_listener *listener, void *data) {
	struct vidow_activation_token *token =
		wl_container_of(listener, token, client_destroy);
	(void)data;
	token_free(token);
}

/* set_surface 那个 surface 没了：兜底路径没了而已，令牌照旧有效。 */
static void token_surface_gone(struct wl_listener *listener, void *data) {
	struct vidow_activation_token *token =
		wl_container_of(listener, token, surface_destroy);
	(void)data;
	token->surface = NULL;
	token->surface_listener_added = false;
}

/* 32 位十六进制随机串。撞了就重来（实际上不可能撞）。 */static void token_make_name(struct vidow_server *server, char out[33]) {
	static const char hex[] = "0123456789abcdef";
	for (int try = 0; try < 8; try++) {
		unsigned char raw[16];
		if (getrandom(raw, sizeof(raw), 0) != (ssize_t)sizeof(raw)) {
			/* 退化路径：只在极早期拿不到随机数时走 */
			uint64_t seed = (uint64_t)time(NULL) * 1000003u ^
				(uint64_t)getpid() * 2654435761u;
			for (size_t i = 0; i < sizeof(raw); i++) {
				seed = seed * 6364136223846793005ull + 1442695040888963407ull;
				raw[i] = (unsigned char)(seed >> 33);
			}
		}
		for (size_t i = 0; i < sizeof(raw); i++) {
			out[i * 2] = hex[raw[i] >> 4];
			out[i * 2 + 1] = hex[raw[i] & 0x0f];
		}
		out[32] = '\0';
		if (token_find(server, out) == NULL) {
			return;
		}
	}
}

/* ------------------------------------------------- 令牌：客户端那个对象 */

static void build_surface_gone(struct wl_listener *listener, void *data) {
	struct vidow_token_build *build =
		wl_container_of(listener, build, surface_destroy);
	(void)data;
	build->surface = NULL;
	build->surface_listener_added = false;
}

static void build_already_used(struct wl_resource *resource) {
	wl_resource_post_error(resource,
		XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED,
		"令牌已经 commit 过了（再 set_* 不认）");
}

static void build_handle_set_serial(struct wl_client *client,
		struct wl_resource *resource, uint32_t serial,
		struct wl_resource *seat_resource) {
	(void)client;
	struct vidow_token_build *build = wl_resource_get_user_data(resource);

	if (build->committed) {
		build_already_used(resource);
		return;
	}

	/* 校验「这个 serial 是不是真发给你过」：seat 不认识（inert）就当作没给。
	 * 校验本身用 wlroots 的（它按 seat 客户端的 serial 环比对，跟 wlroots 那份
	 * xdg-activation 用的是同一个判据）—— 我们只是**拿到结果之后能自己决定**
	 * 理不理，这正是 wlroots 那份给不了的。 */
	struct wlr_seat_client *seat_client = seat_resource != NULL ?
		wlr_seat_client_from_resource(seat_resource) : NULL;
	build->serial = serial;
	build->has_serial = true;
	build->serial_ok = seat_client == NULL ||
		wlr_seat_client_validate_event_serial(seat_client, serial);
	if (!build->serial_ok) {
		wlr_log(WLR_DEBUG, "activation: 令牌的 serial %u 不是发给这个客户端的"
			"（seat %s）", serial, seat_client == NULL ? "不认识" : "认识");
	}
}

static void build_handle_set_app_id(struct wl_client *client,
		struct wl_resource *resource, const char *app_id) {
	(void)client;
	struct vidow_token_build *build = wl_resource_get_user_data(resource);

	if (build->committed) {
		build_already_used(resource);
		return;
	}
	free(build->app_id);
	build->app_id = strdup(app_id);
}

static void build_handle_set_surface(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *surface_resource) {
	(void)client;
	struct vidow_token_build *build = wl_resource_get_user_data(resource);

	if (build->committed) {
		build_already_used(resource);
		return;
	}
	if (build->surface_listener_added) {
		wl_list_remove(&build->surface_destroy.link);
		build->surface_listener_added = false;
	}
	build->surface = surface_resource != NULL ?
		wlr_surface_from_resource(surface_resource) : NULL;
	if (build->surface != NULL) {
		build->surface_destroy.notify = build_surface_gone;
		wl_signal_add(&build->surface->events.destroy, &build->surface_destroy);
		build->surface_listener_added = true;
	}
}

static void build_handle_commit(struct wl_client *client,
		struct wl_resource *resource) {
	(void)client;
	struct vidow_token_build *build = wl_resource_get_user_data(resource);

	if (build->committed) {
		build_already_used(resource);
		return;
	}
	build->committed = true;

	struct vidow_activation_token *token = calloc(1, sizeof(*token));
	if (token == NULL) {
		wl_client_post_no_memory(build->client);
		return;
	}
	token->name = malloc(33);
	if (token->name == NULL) {
		free(token);
		wl_client_post_no_memory(build->client);
		return;
	}
	token_make_name(build->server, token->name);
	token->client = build->client;
	token->serial = build->serial;
	token->has_serial = build->has_serial;
	token->serial_ok = build->serial_ok;
	token->app_id = build->app_id != NULL ? strdup(build->app_id) : NULL;
	token->surface = build->surface;
	clock_gettime(CLOCK_MONOTONIC, &token->created);

	/* 令牌比资源活得久（协议：收到的令牌继续有效），所以挂在客户端上：
	 * 建令牌的客户端一断，这个令牌就没人能用了。 */
	token->client_destroy.notify = token_client_gone;
	wl_client_add_destroy_listener(build->client, &token->client_destroy);
	if (token->surface != NULL) {
		token->surface_destroy.notify = token_surface_gone;
		wl_signal_add(&token->surface->events.destroy, &token->surface_destroy);
		token->surface_listener_added = true;
	}
	wl_list_insert(&build->server->activation_tokens, &token->link);

	/* 一律回 done：协议要求「compositor shall issue a done event」，客户端拿不到
	 * 令牌就会一直等（wlroots 在 serial 校验不过时就是不给 done，客户端干等）。 */
	xdg_activation_token_v1_send_done(resource, token->name);

	if (token->has_serial && !token->serial_ok) {
		wlr_log(WLR_INFO, "activation: 令牌 %s 的 serial %u 校验不过：%s",
			token->name, token->serial,
			build->server->config.honor_xdg_activation_with_invalid_serial ?
			"照样理它（honor_xdg_activation_with_invalid_serial = true）" :
			"以后它的激活请求会被忽略（全局开关 honor_xdg_activation_with_invalid"
			"_serial = false，除非这个窗口的规则写了 honor_invalid_serial = true）");
	} else {
		wlr_log(WLR_DEBUG, "activation: 令牌 %s 建好（app_id=%s %s）",
			token->name, token->app_id != NULL ? token->app_id : "(无)",
			token->has_serial ? "serial 校验过" : "没给 serial");
	}
}

static void build_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	(void)client;
	wl_resource_destroy(resource);
}

static const struct xdg_activation_token_v1_interface token_impl = {
	.destroy = build_handle_destroy,
	.set_serial = build_handle_set_serial,
	.set_app_id = build_handle_set_app_id,
	.set_surface = build_handle_set_surface,
	.commit = build_handle_commit,
};

static void build_resource_destroy(struct wl_resource *resource) {
	struct vidow_token_build *build = wl_resource_get_user_data(resource);
	if (build == NULL) {
		return;
	}
	if (build->surface_listener_added) {
		wl_list_remove(&build->surface_destroy.link);
	}
	free(build->app_id);
	free(build);
}

/* --------------------------------------------------------- 全局对象 */

static void activation_handle_get_token(struct wl_client *client,
		struct wl_resource *resource, uint32_t id) {
	struct vidow_server *server = wl_resource_get_user_data(resource);

	struct vidow_token_build *build = calloc(1, sizeof(*build));
	if (build == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	struct wl_resource *token_resource = wl_resource_create(client,
		&xdg_activation_token_v1_interface,
		wl_resource_get_version(resource), id);
	if (token_resource == NULL) {
		free(build);
		wl_client_post_no_memory(client);
		return;
	}
	build->server = server;
	build->client = client;
	build->resource = token_resource;
	build->serial_ok = true;
	wl_resource_set_implementation(token_resource, &token_impl, build,
		build_resource_destroy);
}

static void activation_handle_activate(struct wl_client *client,
		struct wl_resource *resource, const char *name,
		struct wl_resource *surface_resource) {
	(void)client;
	struct vidow_server *server = wl_resource_get_user_data(resource);

	struct vidow_activation_token *token = token_find(server, name);
	if (token == NULL) {
		wlr_log(WLR_DEBUG, "activation: 令牌 %s 不认识（过期了 / 不是我们发的）"
			"→ 不理", name);
		return;
	}
	if (token_expired(server, token)) {
		wlr_log(WLR_DEBUG, "activation: 令牌 %s 过期了（activation_token_timeout "
			"= %d 秒，已经 %.1f 秒）→ 不理", name,
			server->config.activation_token_timeout, token_age(token));
		token_free(token);
		return;
	}

	/* 要顶的那个 surface：activate 里带的才是（守护进程式激活就是靠这个 ——
	 * 通知守护进程建令牌时不带 surface，被激活的应用自己在 activate 里带）。 */
	struct wlr_surface *surface = surface_resource != NULL ?
		wlr_surface_from_resource(surface_resource) : NULL;
	if (surface == NULL) {
		surface = token->surface;
	}

	struct vidow_view *view = surface != NULL ?
		vidow_view_from_surface(server, surface) : NULL;
	if (view == NULL) {
		/* 弹窗 / layer surface / 已经没了的 surface：没有顶层窗口可以顶。 */
		wlr_log(WLR_DEBUG, "activation: 令牌 %s 指的 surface 不是顶层窗口 → 不理",
			name);
		return;
	}

	/* serial 校验不过时理不理：这个窗口自己的规则（honor_invalid_serial = true）
	 * 优先，其次全局开关。得先找到窗口才能问规则，所以这一段在取到 view 之后。 */
	if (!token->serial_ok) {
		if (view->honor_invalid_serial) {
			wlr_log(WLR_DEBUG, "activation: 令牌 %s 的 serial %u 校验不过，"
				"但这个窗口的规则写着 honor_invalid_serial = true → 照样理",
				name, token->serial);
		} else if (!server->config.honor_xdg_activation_with_invalid_serial) {
			wlr_log(WLR_DEBUG, "activation: 令牌 %s 的 serial %u 校验不过 → 不理"
				"（要理它就把 honor_xdg_activation_with_invalid_serial 打开，"
				"或者给这个窗口写一条 honor_invalid_serial = true 的规则）",
				name, token->serial);
			return;
		}
	}

	const char *title = vidow_view_title(view);
	wlr_log(WLR_DEBUG, "activation: 激活 %s", title != NULL ? title : "(无题)");
	/* 不直接 vidow_focus_view：窗口可能在藏起来的工作区上（点通知的经典情形），
	 * 那样焦点会落在一个看不见的窗口上。vidow_view_activate 会先把它的工作区
	 * 端出来，还没上屏的窗口还会记下来等它出来。 */
	vidow_view_activate(view);
}

static void activation_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	(void)client;
	wl_resource_destroy(resource);
}

static const struct xdg_activation_v1_interface activation_impl = {
	.destroy = activation_handle_destroy,
	.get_activation_token = activation_handle_get_token,
	.activate = activation_handle_activate,
};

static void activation_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&xdg_activation_v1_interface, version < 1 ? (int)version : 1, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &activation_impl, data, NULL);
}

/* 定时清过期令牌：过期判断在 activate 时也会做一次（那条是行为），这个只是别让
 * 列表无限长。timeout = 0（不过期）就不需要定时器。 */
static int activation_sweep(void *data) {
	struct vidow_server *server = data;
	struct vidow_activation_token *token, *tmp;
	wl_list_for_each_safe(token, tmp, &server->activation_tokens, link) {
		if (token_expired(server, token)) {
			token_free(token);
		}
	}
	return server->activation_sweep_ms;
}

/* ------------------------------------------------------------ public API */

void vidow_activation_init(struct vidow_server *server) {
	wl_list_init(&server->activation_tokens);
	server->activation_global = wl_global_create(server->display,
		&xdg_activation_v1_interface, 1, server, activation_bind);
	if (server->activation_global == NULL) {
		wlr_log(WLR_ERROR, "activation: 无法创建 xdg_activation_v1");
		return;
	}

	int timeout = server->config.activation_token_timeout;
	if (timeout > 0) {
		int period = timeout * 1000 / 4;
		if (period < 100) {
			period = 100;
		}
		server->activation_sweep_ms = period;
		struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
		server->activation_timer =
			wl_event_loop_add_timer(loop, activation_sweep, server);
		wl_event_source_timer_update(server->activation_timer, period);
	}

	wlr_log(WLR_INFO, "activation: 激活令牌 %s（xdg-activation 自己实现）",
		timeout > 0 ? "会过期（见 activation_token_timeout）" : "不过期");
	wlr_log(WLR_INFO, "activation: serial 校验不过的激活请求：%s"
		"（honor_xdg_activation_with_invalid_serial）",
		server->config.honor_xdg_activation_with_invalid_serial ?
		"照样理" : "不理");
}
