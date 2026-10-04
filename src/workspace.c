#include "workspace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "server.h"
#include "ipc.h"

/* What follows is one page of bookkeeping: workspaces own views (columns are just
 * an ordered box of views), the layout asks the workspace whether a view shows,
 * and switching a workspace only flips scene-node visibility. No view is ever
 * destroyed by a switch, which is why switching back is instant. */

static struct vidow_workspace *find_by_name(struct vidow_server *server,
		const char *name, bool special);
static void ws_set_output(struct vidow_server *server,
		struct vidow_workspace *ws, struct wlr_output *output);

/* 空名字的工作区自动编号。**名字必须全局唯一**：名字是身份，重名会让「按名字建」
 * 找回另一块屏上那块工作区，于是每次 arrange 都把它在两块屏之间拽来拽去
 * （真踩过：一块工作区在两个输出之间反复横跳，窗口在两边轮流画一遍）。 */
static void auto_name(struct vidow_server *server, struct wlr_output *output,
		char *buf, size_t len) {
	(void)output;
	for (int n = 1; n < 10000; n++) {
		snprintf(buf, len, "%d", n);
		if (find_by_name(server, buf, false) == NULL) {
			return;
		}
	}
	buf[0] = '\0';
}

static struct vidow_workspace *find_by_name(struct vidow_server *server,
		const char *name, bool special) {
	struct vidow_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (ws->special == special && strcmp(ws->name, name) == 0) {
			return ws;
		}
	}
	return NULL;
}

static struct vidow_workspace *workspace_new(struct vidow_server *server,
		const char *name, struct wlr_output *output, bool special) {
	struct vidow_workspace *ws = calloc(1, sizeof(*ws));
	if (ws == NULL) {
		return NULL;
	}
	snprintf(ws->name, sizeof(ws->name), "%s", name);
	ws->special = special;
	ws->output = output;
	if (!special && output != NULL) {
		ws->home = vidow_output_memory_for(&server->output_memories,
			output->name);
	}
	wl_list_init(&ws->columns);
	wl_list_insert(server->workspaces.prev, &ws->link);
	vidow_ipc_emit_workspace(server, "workspace-created", ws);
	return ws;
}

struct vidow_workspace *vidow_workspace_create(struct vidow_server *server,
		const char *name, struct wlr_output *output) {
	if (server == NULL) {
		return NULL;
	}
	/* 没有名字 = 「随便给我一块」：编号保证唯一，直接建新的（不去按名字找，
	 * 否则会跟别的屏上那块同名工作区撞上）。 */
	if (name == NULL || name[0] == '\0') {
		char auto_buf[sizeof(((struct vidow_workspace *)0)->name)];
		auto_name(server, output, auto_buf, sizeof(auto_buf));
		struct vidow_workspace *fresh = workspace_new(server, auto_buf, output,
			false);
		if (fresh != NULL) {
			wlr_log(WLR_INFO, "workspace: 自动建工作区 %s（%s）", auto_buf,
				output != NULL ? output->name : "(还没屏)");
		}
		return fresh;
	}
	struct vidow_workspace *ws = find_by_name(server, name, false);
	if (ws != NULL) {
		if (output != NULL && ws->output != output) {
			/* 配置里重新指派了这块工作区：整块搬过去（列的输出也跟着改） */
			ws_set_output(server, ws, output);
		}
		return ws;
	}
	ws = workspace_new(server, name, output, false);
	if (ws != NULL) {
		wlr_log(WLR_INFO, "workspace: 建工作区 %s（%s）", name,
			output != NULL ? output->name : "(还没屏)");
	}
	return ws;
}

struct vidow_workspace *vidow_workspace_create_special(
		struct vidow_server *server, const char *name) {
	if (server == NULL) {
		return NULL;
	}
	if (name == NULL || name[0] == '\0') {
		name = "special";
	}
	struct vidow_workspace *ws = find_by_name(server, name, true);
	if (ws != NULL) {
		return ws;
	}
	ws = workspace_new(server, name, NULL, true);
	if (ws != NULL) {
		wlr_log(WLR_INFO, "workspace: 建特殊工作区 %s（不进列表，跟随激活的屏）",
			name);
	}
	return ws;
}

struct vidow_workspace *vidow_workspace_lookup(struct vidow_server *server,
		const char *name) {
	if (name == NULL) {
		return NULL;
	}
	return find_by_name(server, name, false);
}

/* 视图 / 窗口搬到别的工作区时输出也可能要跟着改（那块屏上才是它的新家）。 */
static void column_set_output(struct vidow_column *col,
		struct wlr_output *output) {
	col->output = output;
	struct vidow_view *view;
	wl_list_for_each(view, &col->views, column_link) {
		view->output = output;
	}
}

/* 工作区换屏：整块工作区（列 + 浮窗）跟着挪过去。
 *
 * deliberate = 这是「人做的决定」（切换工作区、在某块屏上开了新窗口），于是 home 也
 * 跟着改；拔线时是 false —— 那只是借住，home 留着，屏回来还要按名字认领回去。 */
static void ws_set_output_full(struct vidow_server *server,
		struct vidow_workspace *ws, struct wlr_output *output,
		bool deliberate) {
	if (ws == NULL) {
		return;
	}
	ws->output = output;
	if (deliberate && !ws->special && output != NULL) {
		ws->home = vidow_output_memory_for(&server->output_memories,
			output->name);
	}
	if (output == NULL) {
		return;
	}
	struct vidow_column *col;
	wl_list_for_each(col, &ws->columns, link) {
		column_set_output(col, output);
	}
	/* 浮窗（不在列里）跟着工作区走：换屏时按两块屏原点的差平移，别让它留在
	 * 上一块屏的坐标上（工作区换屏不该把它弄丢或者甩到屏外）。 */
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->ws != ws || view->column != NULL || view->popup) {
			continue;
		}
		view->output = output;
		/* 位置不用在这里算：float.c 的摆放统一把「相对偏移」换算成新屏上的
		 * 绝对位置（见那里的注释）。 */
	}
}

static void ws_set_output(struct vidow_server *server,
		struct vidow_workspace *ws, struct wlr_output *output) {
	ws_set_output_full(server, ws, output, true);
}

static bool ws_empty(const struct vidow_workspace *ws) {
	if (ws->floats > 0) {
		return false;
	}
	return wl_list_empty(&ws->columns);
}

struct vidow_workspace *vidow_workspace_active_for(struct vidow_server *server,
		struct wlr_output *output) {
	/* 「这块屏上的活动工作区」：
	 *   ① 已经在这块屏上亮着的（active）那块；
	 *   ② 这块屏上有别的（不亮的）工作区 —— 挑一块亮起来；
	 *   ③ 别的屏上有空着的工作区 —— 整块挪过来（窗口开在哪儿，工作区就跟到哪儿）；
	 *   ④ 都没有就现建一块。
	 * 「空的」那条是关键：不许把别的屏上还开着窗口的工作区拽过来（那是抢东西）。
	 * 每块屏各自有一块亮着的：active 是「在这块屏上亮着」，不是全局唯一。 */
	struct vidow_workspace *ws;
	struct vidow_workspace *mine = NULL, *spare = NULL;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (ws->special) {
			continue;
		}
		if (ws->output == output) {
			if (ws->active) {
				return ws;
			}
			if (mine == NULL) {
				mine = ws;
			}
		} else if (spare == NULL && !ws->active && ws_empty(ws)) {
			spare = ws;
		}
	}
	ws = mine != NULL ? mine : spare;
	if (ws == NULL) {
		ws = vidow_workspace_create(server, NULL, output);
		if (ws == NULL) {
			return NULL;
		}
	} else if (ws->output != output) {
		ws_set_output(server, ws, output);
	}
	ws->active = true;
	return ws;
}

struct vidow_workspace *vidow_workspace_active(struct vidow_server *server) {
	struct wlr_output *output = server->focused_output;
	if (output == NULL && !wl_list_empty(&server->outputs)) {
		struct vidow_output *first =
			wl_container_of(server->outputs.next, first, link);
		output = first->wlr_output;
	}
	return vidow_workspace_active_for(server, output);
}

struct vidow_workspace *vidow_workspace_special_shown(
		struct vidow_server *server) {
	struct vidow_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (ws->special && ws->visible) {
			return ws;
		}
	}
	return NULL;
}

struct vidow_workspace *vidow_workspace_shown_for(struct vidow_server *server,
		struct wlr_output *output) {
	struct vidow_workspace *special = vidow_workspace_special_shown(server);
	if (special != NULL && special->output == output) {
		return special;
	}
	return vidow_workspace_active_for(server, output);
}

struct vidow_workspace *vidow_workspace_shown(struct vidow_server *server) {
	struct vidow_workspace *special = vidow_workspace_special_shown(server);
	if (special != NULL) {
		return special;
	}
	return vidow_workspace_active(server);
}

bool vidow_workspace_view_visible(struct vidow_server *server,
		struct vidow_view *view) {
	if (view == NULL || view->popup) {
		return false; /* 弹出物不进工作区，跟父窗口走 */
	}
	struct wlr_output *output = view->output;
	if (output == NULL) {
		return false;
	}
	struct vidow_workspace *special = vidow_workspace_special_shown(server);
	if (special != NULL && special->output == output) {
		/* 特殊工作区开着，而且它正显示在这块屏上：这块屏被它顶掉，只有它自己的
		 * 窗口可见（Hyprland 的 special workspace 就是这个语义）。**别的屏照旧**
		 * 显示各自的普通工作区 —— 特殊工作区只占一块屏。 */
		return view->ws == special;
	}
	struct vidow_workspace *active =
		vidow_workspace_active_for(server, output);
	return active != NULL && view->ws == active;
}

/* wlr_output 找它对应的 vidow_output：滚动偏移住在那儿（按输出名记的）。 */
static struct vidow_output *output_of(struct vidow_server *server,
		struct wlr_output *wlr_output) {
	if (wlr_output == NULL) {
		return NULL;
	}
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->wlr_output == wlr_output) {
			return output;
		}
	}
	return NULL;
}

static bool node_enabled(struct wlr_scene_tree *tree) {
	return tree != NULL && tree->node.enabled;
}

int vidow_workspace_apply_visibility(struct vidow_server *server) {
	int shown = 0;
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->scene_tree == NULL) {
			continue;
		}
		bool want;
		if (view->popup) {
			/* 弹出物跟父窗口：父窗口藏起来了（换了工作区），菜单也得收。 */
			if (view->parent == NULL) {
				continue;
			}
			want = view->mapped &&
				vidow_workspace_view_visible(server, view->parent);
		} else {
			want = view->mapped && vidow_workspace_view_visible(server, view);
		}
		if (want) {
			shown++;
		}
		if (node_enabled(view->scene_tree) != want) {
			wlr_log(WLR_DEBUG, "workspace: %s %s（%s）",
				vidow_view_title(view) != NULL ?
					vidow_view_title(view) : "(无题)",
				want ? "显示" : "藏起",
				view->ws != NULL ? view->ws->name : "-");
			wlr_scene_node_set_enabled(&view->scene_tree->node, want);
		}
	}
	return shown;
}

/* 指针还活着吗 —— 工作区记的是裸指针，窗口可能早关掉了，用之前验一遍。 */
static bool view_alive(struct vidow_server *server, struct vidow_view *want) {
	if (want == NULL) {
		return false;
	}
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view == want) {
			return true;
		}
	}
	return false;
}

static void focus_workspace(struct vidow_server *server,
		struct vidow_workspace *ws) {
	/* 先回到上次离开时那个窗口（还在、还映射着、还属于这块工作区才算），
	 * 否则找这块工作区里的第一列，再没有就把焦点放空。 */
	if (ws->focused != NULL && view_alive(server, ws->focused) &&
			ws->focused->mapped && ws->focused->ws == ws) {
		vidow_focus_view(ws->focused);
		return;
	}
	struct vidow_column *col;
	wl_list_for_each(col, &ws->columns, link) {
		if (wl_list_empty(&col->views)) {
			continue;
		}
		struct vidow_view *view = wl_container_of(col->views.next, view,
			column_link);
		vidow_focus_view(view);
		return;
	}
	server->focused = NULL;
	wlr_seat_keyboard_notify_clear_focus(server->seat);
	vidow_im_focus(server, NULL);
	vidow_arrange(server);
}

static void ws_adopt_column(struct vidow_workspace *target,
		struct vidow_column *col) {
	if (col->ws != NULL && col->ws != target) {
		wl_list_remove(&col->link);
	}
	wl_list_insert(target->columns.prev, &col->link);
	col->ws = target;
	/* 列里的窗口跟着列走：它们自己的 ws 也要改（可见性是按窗口看的）。 */
	struct vidow_view *view;
	wl_list_for_each(view, &col->views, column_link) {
		view->ws = target;
	}
	if (target->output != NULL && col->output != target->output) {
		column_set_output(col, target->output);
	}
}

static void ws_adopt_view(struct vidow_workspace *target,
		struct vidow_view *view) {
	if (view->ws != NULL) {
		vidow_workspace_track(view->ws, view, false);
	}
	view->ws = target;
	vidow_workspace_track(target, view, true);
	if (target->output != NULL) {
		view->output = target->output;
	}
}

bool vidow_workspace_activate(struct vidow_server *server,
		struct vidow_workspace *ws) {
	if (server == NULL || ws == NULL || ws->special) {
		return false;
	}

	/* 有特殊工作区开着就先收起来：切普通工作区 = 回到正常桌面。 */
	struct vidow_workspace *special = vidow_workspace_special_shown(server);
	if (special != NULL) {
		special->visible = false;
		if (special->output != NULL && server->focused != NULL &&
				server->focused->ws == special) {
			special->focused = server->focused;
		}
		special->output = NULL;
	}

	struct vidow_workspace *prev = vidow_workspace_active(server);
	if (prev == ws && ws->output == server->focused_output) {
		if (ws->output != NULL) {
			server->focused_output = ws->output;
		}
		vidow_workspace_apply_visibility(server);
		vidow_arrange(server);
		vidow_ipc_emit_workspace(server, "workspace-activated", ws);
		return true;
	}
	if (prev != NULL) {
		struct vidow_output *prev_output = output_of(server, prev->output);
		if (prev_output != NULL) {
			prev->viewport_x = prev_output->viewport_x;
		}
		if (server->focused != NULL && server->focused->ws == prev) {
			prev->focused = server->focused;
		}
		prev->active = false;
	}

	struct wlr_output *target_output = server->focused_output;
	if (target_output == NULL && server->focused != NULL) {
		target_output = server->focused->output;
	}
	if (ws->active && ws->output != NULL && ws->output != target_output) {
		/* 它已经在**别的屏**上亮着：那就把焦点挪过去（人跟着工作区走），
		 * 而不是把它从那块屏上拽过来 —— 那块屏上还有它自己的窗口。 */
		target_output = ws->output;
		server->focused_output = target_output;
		/* 那块屏上原来亮着的（如果有）让位 */
		struct vidow_workspace *other;
		wl_list_for_each(other, &server->workspaces, link) {
			if (!other->special && other != ws && other->active &&
					other->output == target_output) {
				other->active = false;
			}
		}
	} else {
		/* 没在别的屏上亮着：切到哪块屏就让它住哪块屏（整块工作区一起挪）。 */
		if (target_output != NULL) {
			ws_set_output(server, ws, target_output);
		}
	}
	ws->active = true;
	if (ws->output != NULL) {
		server->focused_output = ws->output;
		struct vidow_output *out = output_of(server, ws->output);
		if (out != NULL) {
			out->viewport_x = ws->viewport_x;
		}
	}

	int shown = vidow_workspace_apply_visibility(server);
	if (server->focused != NULL && server->focused->ws != ws) {
		/* 焦点窗口随旧工作区一起藏了：下面 focus_workspace 重新挑一个。 */
		server->focused = NULL;
	}

	int columns = 0;
	struct vidow_column *col;
	wl_list_for_each(col, &ws->columns, link) {
		columns++;
	}
	wlr_log(WLR_INFO, "workspace: 切到 %s（%s，本区 %d 列 / %d 个窗口，"
		"全屏 %d 个可见）", ws->name,
		ws->output != NULL ? ws->output->name : "(没屏)", columns,
		vidow_workspace_view_count(ws), shown);

	focus_workspace(server, ws);
	vidow_arrange(server);
	vidow_ipc_emit_workspace(server, "workspace-activated", ws);
	return true;
}

bool vidow_workspace_toggle_special(struct vidow_server *server,
		const char *name) {
	struct vidow_workspace *ws = vidow_workspace_create_special(server, name);
	if (ws == NULL) {
		return false;
	}

	struct vidow_workspace *shown = vidow_workspace_special_shown(server);
	if (shown == ws) {
		/* 关掉：普通工作区露回来，回到它离开时的焦点。 */
		if (server->focused != NULL && server->focused->ws == ws) {
			ws->focused = server->focused;
		}
		ws->visible = false;
		struct vidow_output *out = output_of(server, ws->output);
		if (out != NULL) {
			ws->viewport_x = out->viewport_x;
		}
		ws->output = NULL;

		/* 关掉之后回哪块普通工作区：它自己记着的那个（窗口搬进来之前住过的）。 */
		struct vidow_workspace *back = NULL;
		if (ws->focused != NULL && ws->focused->ws != NULL &&
				ws->focused->ws != ws && !ws->focused->ws->special) {
			back = ws->focused->ws;
		}
		if (back == NULL) {
			back = vidow_workspace_active(server);
		}
		if (server->focused != NULL && server->focused->ws == ws) {
			server->focused = NULL;
		}
		int visible = vidow_workspace_apply_visibility(server);
		wlr_log(WLR_INFO, "workspace: 收起特殊工作区 %s（本区留 %d 个窗口，"
			"全屏 %d 个可见）", ws->name, vidow_workspace_view_count(ws),
			visible);
		if (back != NULL) {
			focus_workspace(server, back);
		}
		vidow_arrange(server);
		return true;
	}

	/* 跟随激活的显示器：显示在**当前聚焦**的那块屏上。没有屏的时候就先挂着，
	 * 等有聚焦的屏了再显示。（已经在显示的另一块特殊工作区由 show_special 让位） */
	struct wlr_output *output = server->focused != NULL ?
		server->focused->output : server->focused_output;
	if (output == NULL) {
		output = server->focused_output;
	}
	return vidow_workspace_show_special(server, ws, output);
}

/* 把这块特殊工作区显示到指定那块屏上（切换时用「聚焦的屏」，激活里面的窗口时
 * 用「窗口所在的那块屏」—— 点通知的时候人在别的屏上也该跳过去）。 */
bool vidow_workspace_show_special(struct vidow_server *server,
		struct vidow_workspace *ws, struct wlr_output *output) {
	if (server == NULL || ws == NULL || !ws->special) {
		return false;
	}
	/* 已经开着的另一块特殊工作区让位（一次只开一块）。 */
	struct vidow_workspace *shown = vidow_workspace_special_shown(server);
	if (shown != NULL && shown != ws) {
		shown->visible = false;
		shown->output = NULL;
	}
	ws_set_output(server, ws, output);
	struct vidow_output *out = output_of(server, output);
	if (out != NULL) {
		out->viewport_x = ws->viewport_x;
	}
	ws->visible = true;

	int count = vidow_workspace_apply_visibility(server);
	vidow_ipc_emit_workspace(server, "workspace-special-changed", ws);
	wlr_log(WLR_INFO, "workspace: 打开特殊工作区 %s（显示在 %s 上，本区 %d 个"
		"窗口，全屏 %d 个可见，不进工作区列表）", ws->name,
		output != NULL ? output->name : "(没有聚焦的屏)",
		vidow_workspace_view_count(ws), count);

	if (server->focused != NULL && server->focused->ws != ws) {
		server->focused = NULL;
	}
	focus_workspace(server, ws);
	vidow_arrange(server);
	return true;
}

/* 把一个窗口搬到某块工作区（IPC 也走这里：不一定是焦点窗口）。列里的窗口
 * 整列搬过去（列是工作区的东西）。 */
bool vidow_workspace_move_view(struct vidow_server *server,
		struct vidow_view *view, struct vidow_workspace *target) {
	if (server == NULL || view == NULL || target == NULL ||
			target == view->ws) {
		return false;
	}

	struct vidow_workspace *source = view->ws;
	if (view->column != NULL) {
		ws_adopt_column(target, view->column);
	} else {
		ws_adopt_view(target, view);
	}

	wlr_log(WLR_INFO, "workspace: 把 %s 搬到 %s%s",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
		target->name, target->special ? "（特殊工作区）" : "");
	vidow_ipc_emit_window(server, "window-workspace-changed", view);

	/* 搬走了就看不见了：焦点交给现在显示着的那块工作区里还在的东西。 */
	if (server->focused == view) {
		server->focused = NULL;
	}
	vidow_workspace_apply_visibility(server);
	if (source != NULL && source->focused == view) {
		source->focused = NULL;
	}
	struct vidow_workspace *back = vidow_workspace_shown(server);
	if (back != NULL) {
		focus_workspace(server, back);
	}
	vidow_arrange(server);
	return true;
}

/* 按名字搬：特殊工作区按名字找，找不到就当普通工作区建一块。 */
bool vidow_workspace_move_view_to_name(struct vidow_server *server,
		struct vidow_view *view, const char *name) {
	if (view == NULL || name == NULL) {
		return false;
	}
	struct vidow_workspace *target = find_by_name(server, name, true);
	if (target == NULL) {
		target = vidow_workspace_create(server, name,
			view->output != NULL ? view->output : server->focused_output);
	}
	return vidow_workspace_move_view(server, view, target);
}

bool vidow_workspace_move_focused(struct vidow_server *server,
		const char *name) {
	if (server->focused == NULL || name == NULL) {
		return false;
	}
	return vidow_workspace_move_view_to_name(server, server->focused, name);
}

/* 整块工作区换屏（列 + 浮窗一起过去，home 也跟着改：这是人做的决定）。 */
bool vidow_workspace_move_to_output(struct vidow_server *server,
		struct vidow_workspace *ws, struct wlr_output *output) {
	if (server == NULL || ws == NULL || ws->special || output == NULL ||
			ws->output == output) {
		return false;
	}
	ws_set_output_full(server, ws, output, true);
	wlr_log(WLR_INFO, "workspace: 把 %s 整块搬到 %s 上（%d 个窗口）", ws->name,
		output->name, vidow_workspace_view_count(ws));
	vidow_workspace_apply_visibility(server);
	vidow_arrange(server);
	vidow_ipc_emit_workspace(server, "workspace-changed", ws);
	return true;
}

void vidow_workspace_output_gone(struct vidow_server *server,
		struct wlr_output *dead, struct wlr_output *to) {
	struct vidow_workspace *ws;
	int moved = 0;
	bool took_over = false;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (ws->output != dead) {
			continue;
		}
		ws_set_output_full(server, ws, to, false); /* 借住：home 留着 */
		moved++;
		/* 拔线前那块屏上亮着的工作区，到了剩下的屏上也该亮着（人跟着屏走：
		 * 拔掉我现在用的这块屏，我的东西应该出现在另一块屏的最前面）。 */
		if (ws->active && to != NULL) {
			struct vidow_workspace *other;
			wl_list_for_each(other, &server->workspaces, link) {
				if (!other->special && other != ws &&
						other->output == to) {
					other->active = false;
				}
			}
			took_over = true;
		}
	}
	if (took_over) {
		wlr_log(WLR_INFO, "workspace: 剩下的屏先显示搬过来的那块工作区");
	}
	if (moved > 0) {
		wlr_log(WLR_INFO, "workspace: %s 上 %d 块工作区跟着列挪到 %s 上住",
			dead != NULL ? dead->name : "?", moved,
			to != NULL ? to->name : "(暂时没屏)");
	}
}

void vidow_workspace_output_added(struct vidow_server *server,
		struct vidow_output_memory *memory, struct wlr_output *back) {
	int home = 0;
	struct vidow_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (ws->special || ws->home != memory || ws->output == back) {
			continue;
		}
		ws_set_output_full(server, ws, back, true);
		home++;
	}
	if (home > 0) {
		wlr_log(WLR_INFO, "workspace: %s 回来了，认领回 %d 块属于它的工作区"
			"（列、列宽、滚动位置都还在）",
			memory != NULL ? memory->key : "?", home);
	}
}

void vidow_workspace_track(struct vidow_workspace *ws,
		struct vidow_view *view, bool enter) {
	if (ws == NULL || view == NULL || view->column != NULL) {
		return; /* 列里的窗口不算浮窗 */
	}
	ws->floats += enter ? 1 : -1;
	if (ws->floats < 0) {
		ws->floats = 0;
	}
}

void vidow_workspace_note_focus(struct vidow_workspace *ws,
		struct vidow_view *view) {
	if (ws != NULL) {
		ws->focused = view;
	}
}

const char *vidow_workspace_name(const struct vidow_workspace *ws) {
	return ws != NULL ? ws->name : "";
}

bool vidow_workspace_is_special(const struct vidow_workspace *ws) {
	return ws != NULL && ws->special;
}

bool vidow_workspace_is_active(const struct vidow_workspace *ws) {
	return ws != NULL && ws->active;
}

bool vidow_workspace_is_visible(const struct vidow_workspace *ws) {
	return ws != NULL && (ws->active || ws->visible);
}

int vidow_workspace_view_count(const struct vidow_workspace *ws) {
	if (ws == NULL) {
		return 0;
	}
	int n = ws->floats;
	struct vidow_column *col;
	wl_list_for_each(col, &ws->columns, link) {
		struct vidow_view *view;
		wl_list_for_each(view, &col->views, column_link) {
			n++;
		}
	}
	return n;
}

bool vidow_workspace_init(struct vidow_server *server) {
	wl_list_init(&server->workspaces);
	return true;
}
