/* ext-workspace-v1 —— 让 shell（noctalia / waybar / …）看见 vidow 的工作区。
 *
 * 为什么要有这个文件：noctalia 的 dock 自动隐藏判据是「**活动工作区**是不是空的」
 * （src/shell/dock/dock_instance.h 的 smart_auto_hide），而它拿工作区的唯一通用
 * 途径就是 ext-workspace-v1（README 原话："Workspace integration works through
 * compositor-native backends where needed, or through ext-workspace-v1"）。
 * vidow 不是它的 native backend → 没有这个协议，dock 永远不知道工作区空不空，
 * 于是永远不隐藏（用户报的「noctalia 无法判断当前是否窗口占满工作区」）。
 *
 * 最小集：manager global + 每块输出一个 group + 每个工作区一个 handle，
 * 状态只发 active / hidden。activate / deactivate 等请求按 no-op 处理
 * （vidow 的激活走自己的 IPC 和键位，客户端不该从这里改工作区）。
 */
#include "server.h"

#include <wlr/util/log.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>

#include "ext-workspace-v1-server-protocol.h"

struct extws_group {
	struct wl_list link;
	struct wl_resource *resource;
	struct wlr_output *output;
};

struct extws_ws {
	struct wl_list link;
	struct wl_resource *resource;
	struct vidow_workspace *ws;
	uint32_t last_state;
	bool has_last;
};

struct extws_manager {
	struct wl_list link;
	struct wl_resource *resource;
};

struct extws {
	struct vidow_server *server;
	struct wl_global *global;
	struct wl_list managers;
	struct wl_list groups;
	struct wl_list workspaces;
};

static struct extws *extws_of(struct vidow_server *server);

/* ---------------------------------------------------------------- 资源表 */

static void group_resource_destroy(struct wl_resource *r) {
	struct extws_group *g = wl_resource_get_user_data(r);
	if (g != NULL) {
		wl_list_remove(&g->link);
		free(g);
	}
}

static void ws_resource_destroy(struct wl_resource *r) {
	struct extws_ws *w = wl_resource_get_user_data(r);
	if (w != NULL) {
		wl_list_remove(&w->link);
		free(w);
	}
}

static void manager_resource_destroy(struct wl_resource *r) {
	struct extws_manager *m = wl_resource_get_user_data(r);
	if (m != NULL) {
		wl_list_remove(&m->link);
		free(m);
	}
}

/* ---------------------------------------------------------------- 请求 */

static void handle_commit(struct wl_client *client, struct wl_resource *r) {
	(void)client;
	(void)r;
	/* 客户端确认收下了这一轮状态；vidow 没有「延迟提交」的模型，无事可做。 */
}

static void handle_stop(struct wl_client *client, struct wl_resource *r) {
	(void)client;
	wl_resource_destroy(r);
}

static const struct ext_workspace_manager_v1_interface manager_impl = {
	.commit = handle_commit,
	.stop = handle_stop,
};

static void group_create_workspace(struct wl_client *client, struct wl_resource *r,
		const char *name) {
	(void)client;
	(void)r;
	(void)name;
	/* 不支持从 shell 建工作区（capabilities 里不报 create_workspace）：
	 * 请求忽略即可 —— 协议允许服务端不报这个能力。 */
}

static void group_destroy_req(struct wl_client *client, struct wl_resource *r) {
	(void)client;
	wl_resource_destroy(r);
}

static const struct ext_workspace_group_handle_v1_interface group_impl = {
	.create_workspace = group_create_workspace,
	.destroy = group_destroy_req,
};

static void ws_destroy_req(struct wl_client *client, struct wl_resource *r) {
	(void)client;
	wl_resource_destroy(r);
}

/* activate / deactivate / assign / remove：vidow 的工作区由自己的 IPC 和键位
 * 管，不接受 shell 端修改 —— 一律忽略（协议允许：capabilities 里不报这些位）。 */
static void ws_noop(struct wl_client *client, struct wl_resource *r) {
	(void)client;
	(void)r;
}

static void ws_assign_noop(struct wl_client *client, struct wl_resource *r,
		struct wl_resource *workspace) {
	(void)client;
	(void)r;
	(void)workspace;
}

static const struct ext_workspace_handle_v1_interface ws_impl = {
	.destroy = ws_destroy_req,
	.activate = ws_noop,
	.deactivate = ws_noop,
	.assign = ws_assign_noop,
	.remove = ws_noop,
};

/* ---------------------------------------------------------------- 同步 */

static void send_handle_state(struct extws_ws *w) {
	uint32_t state = 0;
	if (w->ws->active || w->ws->visible) {
		state |= EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE;
	} else {
		state |= EXT_WORKSPACE_HANDLE_V1_STATE_HIDDEN;
	}
	if (w->has_last && w->last_state == state) {
		return;
	}
	w->last_state = state;
	w->has_last = true;
	ext_workspace_handle_v1_send_state(w->resource, state);
}

/* 把当前 server 的工作区 / 输出状态推给所有 manager 客户端。幂等：状态没变
 * 就不发 state 事件；done 每次都发（协议要求一轮事件以 done 收尾）。 */
void vidow_extworkspace_sync(struct vidow_server *server) {
	struct extws *e = extws_of(server);
	if (e == NULL || wl_list_empty(&e->managers)) {
		return;
	}

	/* 1) 每块输出一个 group（第一次见到就建，并把当前工作区 enter 进去）。 */
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct extws_group *g = NULL;
		struct extws_group *it;
		wl_list_for_each(it, &e->groups, link) {
			if (it->output == output->wlr_output) {
				g = it;
				break;
			}
		}
		if (g == NULL) {
			continue; /* 没有客户端时为它建资源没有意义 */
		}
	}

	/* 2) 工作区：state 变了就发。 */
	struct extws_ws *w;
	wl_list_for_each(w, &e->workspaces, link) {
		send_handle_state(w);
	}

	struct extws_manager *m;
	wl_list_for_each(m, &e->managers, link) {
		ext_workspace_manager_v1_send_done(m->resource);
	}
}

/* ---------------------------------------------------------------- 绑定 */

static void bind_manager(struct wl_client *client, void *data, uint32_t version,
		uint32_t id) {
	struct vidow_server *server = data;
	struct extws *e = extws_of(server);
	if (e == NULL) {
		return;
	}
	struct wl_resource *res = wl_resource_create(client,
		&ext_workspace_manager_v1_interface, (int)version, id);
	if (res == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	struct extws_manager *m = calloc(1, sizeof(*m));
	if (m == NULL) {
		wl_resource_destroy(res);
		wl_client_post_no_memory(client);
		return;
	}
	m->resource = res;
	wl_list_insert(&e->managers, &m->link);
	wl_resource_set_implementation(res, &manager_impl, m,
		manager_resource_destroy);

	/* 按输出建 group，按工作区建 handle，全部报给这个客户端。 */
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wl_resource *gr = wl_resource_create(client,
			&ext_workspace_group_handle_v1_interface, (int)version, 0);
		if (gr == NULL) {
			continue;
		}
		struct extws_group *g = calloc(1, sizeof(*g));
		if (g == NULL) {
			wl_resource_destroy(gr);
			continue;
		}
		g->resource = gr;
		g->output = output->wlr_output;
		wl_list_insert(&e->groups, &g->link);
		wl_resource_set_implementation(gr, &group_impl, g,
			group_resource_destroy);
		ext_workspace_manager_v1_send_workspace_group(res, gr);
		ext_workspace_group_handle_v1_send_capabilities(gr, 0);
		/* 不发 output_enter：wlr_output 不暴露 wl_output 资源给合成器内部用，
		 * group 仍然可用（客户端按 group 里的 workspace 判断）。 */
	}

	struct vidow_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (ws->special) {
			continue; /* 特殊工作区不进 shell 的列表 */
		}
		struct wl_resource *wr = wl_resource_create(client,
			&ext_workspace_handle_v1_interface, (int)version, 0);
		if (wr == NULL) {
			continue;
		}
		struct extws_ws *w = calloc(1, sizeof(*w));
		if (w == NULL) {
			wl_resource_destroy(wr);
			continue;
		}
		w->resource = wr;
		w->ws = ws;
		wl_list_insert(&e->workspaces, &w->link);
		wl_resource_set_implementation(wr, &ws_impl, w, ws_resource_destroy);
		ext_workspace_manager_v1_send_workspace(res, wr);

		/* id 是字符串（协议规定），用稳定编号，换屏换位置都不变。 */
		char idbuf[32];
		snprintf(idbuf, sizeof(idbuf), "%llu", (unsigned long long)ws->id);
		ext_workspace_handle_v1_send_id(wr, idbuf);

		if (ws->name[0] != '\0') {
			ext_workspace_handle_v1_send_name(wr, ws->name);
		}
		struct wl_array coords;
		wl_array_init(&coords);
		uint32_t *c0 = wl_array_add(&coords, sizeof(uint32_t) * 2);
		if (c0 != NULL) {
			c0[0] = (uint32_t)ws->id;
			c0[1] = 0;
		}
		ext_workspace_handle_v1_send_coordinates(wr, &coords);
		wl_array_release(&coords);

		ext_workspace_handle_v1_send_capabilities(wr, 0);
		send_handle_state(w);
	}

	ext_workspace_manager_v1_send_done(res);
}

/* ---------------------------------------------------------------- 生命周期 */

static struct extws *extws_of(struct vidow_server *server) {
	return server->extworkspace;
}

void vidow_extworkspace_init(struct vidow_server *server) {
	struct extws *e = calloc(1, sizeof(*e));
	if (e == NULL) {
		wlr_log(WLR_ERROR, "ext-workspace: 分配失败，这个协议不提供");
		return;
	}
	e->server = server;
	wl_list_init(&e->managers);
	wl_list_init(&e->groups);
	wl_list_init(&e->workspaces);
	e->global = wl_global_create(server->display,
		&ext_workspace_manager_v1_interface, 1, server, bind_manager);
	if (e->global == NULL) {
		wlr_log(WLR_ERROR, "ext-workspace: global 建不出来");
		free(e);
		return;
	}
	server->extworkspace = e;
	wlr_log(WLR_INFO, "ext-workspace: 已提供（shell 能看见工作区了）");
}

void vidow_extworkspace_finish(struct vidow_server *server) {
	struct extws *e = extws_of(server);
	if (e == NULL) {
		return;
	}
	if (e->global != NULL) {
		wl_global_destroy(e->global);
	}
	free(e);
	server->extworkspace = NULL;
}
