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
static bool ws_empty(const struct vidow_workspace *ws);
static void ws_destroy(struct vidow_server *server, struct vidow_workspace *ws);

/* 按名字找（具名工作区 / 特殊工作区）。没名字的动态工作区不在这里 —— 它们靠
 * 「那块屏上的第几块」寻址，名字只是显示用的（niri：无名工作区没有名字）。 */
static struct vidow_workspace *find_by_name(struct vidow_server *server,
		const char *name, bool special) {
	if (name == NULL || name[0] == '\0') {
		return NULL;
	}
	struct vidow_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (ws->special == special && ws->name[0] != '\0' &&
				strcmp(ws->name, name) == 0) {
			return ws;
		}
	}
	return NULL;
}

/* 把还没挂进链表的工作区插到 output 那块屏的第 idx0 位（0 起；越界 = 那组的最后）。
 * 同屏工作区之间的先后就是序号，所以只要保证组内相对顺序对就行。 */
static void ws_splice(struct vidow_workspace *ws, struct wlr_output *output,
		int idx0) {
	struct vidow_server *server = ws->server;
	struct vidow_workspace *it;
	struct wl_list *before = NULL; /* 插在它前面；NULL = 追加到那组后面 */
	int seen = 0;
	wl_list_for_each(it, &server->workspaces, link) {
		if (it->special) {
			break; /* 普通工作区到这儿就结束了 */
		}
		if (it->output != output) {
			continue;
		}
		if (before == NULL && seen == idx0) {
			before = &it->link;
		}
		seen++;
	}
	/* 注意 wl_list_insert(pos, elm) 是「插在 pos **之后**」，所以插到 before
	 * 前面要写 wl_list_insert(before->prev, elm)。 */
	if (before != NULL) {
		wl_list_insert(before->prev, &ws->link);
	} else {
		struct wl_list *special = NULL;
		wl_list_for_each(it, &server->workspaces, link) {
			if (it->special) {
				special = &it->link;
				break;
			}
		}
		if (special != NULL) {
			wl_list_insert(special->prev, &ws->link); /* 普通工作区之后 */
		} else {
			wl_list_insert(server->workspaces.prev, &ws->link); /* 追加 */
		}
	}
	ws->output = output;
}

/* ------------------------------------------------- 按屏的纵向工作区栈（序号）

 * 序号 = 它在**自己那块屏**上的位置（1 起）—— 就是 niri 的
 * `focus-workspace N` 里的 N：位置，不是身份。具名工作区靠名字认，动态的靠位置认。 */

static bool ws_on_output(const struct vidow_workspace *ws,
		struct wlr_output *output) {
	return ws != NULL && !ws->special && ws->output == output;
}

int vidow_workspace_count(struct vidow_server *server,
		struct wlr_output *output) {
	int n = 0;
	struct vidow_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (ws_on_output(ws, output)) {
			n++;
		}
	}
	return n;
}

struct vidow_workspace *vidow_workspace_at(struct vidow_server *server,
		struct wlr_output *output, int idx) {
	if (server == NULL || output == NULL || idx < 1) {
		return NULL;
	}
	int i = 0;
	struct vidow_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (!ws_on_output(ws, output)) {
			continue;
		}
		if (++i == idx) {
			return ws;
		}
	}
	return NULL;
}

int vidow_workspace_index(const struct vidow_workspace *ws) {
	if (ws == NULL || ws->special || ws->output == NULL || ws->server == NULL) {
		return 0;
	}
	int i = 0;
	struct vidow_workspace *it;
	wl_list_for_each(it, &ws->server->workspaces, link) {
		if (!ws_on_output(it, ws->output)) {
			continue;
		}
		i++;
		if (it == ws) {
			return i;
		}
	}
	return 0;
}

static uint64_t next_ws_id = 1;
/* 用户手动切过工作区没有 —— 「栈顶那块该不该亮着」只在这之前才自动纠正。 */
static bool ws_switched_by_user = false;

/* 启动时那块自动建出来的空工作区先占了 active；配置里声明的具名工作区插到它上面
 * 之后要把 active 让回去（niri 的 active_workspace_idx 一开始就是 0）。只在用户
 * 还没手动切过工作区的时候做，免得把人从当前工作区弹走。 */
static void ws_promote_first(struct vidow_server *server,
		struct wlr_output *output) {
	if (ws_switched_by_user || server == NULL || output == NULL) {
		return;
	}
	struct vidow_workspace *active =
		vidow_workspace_active_for(server, output);
	if (active == NULL || active->named || !ws_empty(active)) {
		return;
	}
	struct vidow_workspace *first = vidow_workspace_at(server, output, 1);
	if (first == NULL || first == active) {
		return;
	}
	active->active = false;
	first->active = true;
	wlr_log(WLR_DEBUG, "workspace: %s 上让栈顶那块（%s）亮着",
		output->name, first->named ? first->name : "(没名字)");
}

static struct vidow_workspace *workspace_new(struct vidow_server *server,
		const char *name, struct wlr_output *output, int idx0, bool special) {
	struct vidow_workspace *ws = calloc(1, sizeof(*ws));
	if (ws == NULL) {
		return NULL;
	}
	ws->server = server;
	ws->id = next_ws_id++;
	ws->named = name != NULL && name[0] != '\0';
	snprintf(ws->name, sizeof(ws->name), "%s",
		name != NULL ? name : "");
	ws->special = special;
	if (!special && output != NULL) {
		ws->home = vidow_output_memory_for(&server->output_memories,
			output->name);
	}
	wl_list_init(&ws->columns);
	wl_list_init(&ws->link);
	if (special) {
		/* 特殊工作区不进普通工作区那串（它的 output 只是「现在显示在哪块屏上」） */
		ws->output = output;
		wl_list_insert(server->workspaces.prev, &ws->link);
	} else {
		ws_splice(ws, output, idx0);
	}
	vidow_ipc_emit_workspace(server, "workspace-created", ws);
	return ws;
}

struct vidow_workspace *vidow_workspace_create(struct vidow_server *server,
		const char *name, struct wlr_output *output) {
	if (server == NULL) {
		return NULL;
	}
	/* 没名字 = 动态工作区（niri 的无名工作区）：插在那块屏的栈尾，就是底部那个
	 * 「新工作区」位。 */
	if (name == NULL || name[0] == '\0') {
		int n = vidow_workspace_count(server, output);
		struct vidow_workspace *fresh = workspace_new(server, NULL, output,
			n, false);
		if (fresh != NULL) {
			wlr_log(WLR_DEBUG, "workspace: 在 %s 的栈尾建一块没名字的工作区"
				"（现在 %d 块）",
				output != NULL ? output->name : "(还没屏)", n + 1);
		}
		return fresh;
	}
	struct vidow_workspace *ws = find_by_name(server, name, false);
	if (ws != NULL) {
		/* 已经在了：只有它还没归属过屏时才认领（配置重载不该把用户手动挪到
		 * 别的屏上的具名工作区拽回来 —— niri 的具名工作区就是「钉」在原屏上）。 */
		if (ws->output == NULL && output != NULL) {
			ws_set_output(server, ws, output);
		}
		return ws;
	}
	/* 新建的具名工作区插在**底部那块空工作区之前**：niri 里具名工作区按声明顺序
	 * 排在上面，底部那块空的永远是最后一块。 */
	int n = vidow_workspace_count(server, output);
	int idx0 = n > 0 ? n - 1 : 0;
	ws = workspace_new(server, name, output, idx0, false);
	if (ws != NULL) {
		wlr_log(WLR_INFO, "workspace: 建具名工作区 %s（%s，第 %d 块）", name,
			output != NULL ? output->name : "(还没屏)",
			vidow_workspace_index(ws));
		ws_promote_first(server, output);
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
	ws = workspace_new(server, name, NULL, 0, true);
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

/* 这块屏还在不在（还在 server->outputs 里 = 还插着）。
 *
 * 热插拔的先后顺序是「先从 server->outputs 摘掉，再把东西挪走」（见 output.c 的
 * output_destroy），所以拔线过程中 ws->output 有可能指着一块已经摘掉、马上要
 * 释放的屏。这种时候只能比指针，不能碰它。 */
static bool output_present(struct vidow_server *server,
		struct wlr_output *output) {
	struct vidow_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output == output) {
			return true;
		}
	}
	return false;
}

/* 列 / 列里的窗口、跟着这块工作区走的浮窗，output 全部设成 output（NULL 也照设，
 * 见下面 ws_set_output_full 里那段注释）。认领一块**现在没屏**的工作区时也要走一遍：
 * 工作区认领了、里面的窗口还指着 NULL 的话，屏幕上什么都看不到。 */
static void ws_propagate_output(struct vidow_server *server,
		struct vidow_workspace *ws, struct wlr_output *output) {
	struct vidow_column *col;
	wl_list_for_each(col, &ws->columns, link) {
		column_set_output(col, output);
	}
	if (output == NULL) {
		return; /* 没屏了：下面浮窗那段要按新屏原点算平移，没屏没什么可算的 */
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
	if (ws->special) {
		ws->output = output; /* 特殊工作区不绑屏：home 一直是 NULL */
	} else if (ws->output != output) {
		/* 换屏 = 在新屏的栈里重新排位：插到底部那块空工作区**之前**
		 * （niri 的 append_workspaces 就是这个位置）。 */
		struct wlr_output *old = ws->output;
		if (!wl_list_empty(&ws->link)) {
			wl_list_remove(&ws->link);
		}
		int n = vidow_workspace_count(server, output);
		ws_splice(ws, output, n > 0 ? n - 1 : 0);
		if (old != NULL) {
			/* 老屏上要是没工作区了，给它补一块空的（不然那块屏没东西可显示）。
			 * 但老屏**已经没了**的时候（拔线：这块屏正在被释放）绝不能补 ——
			 * 补出来的那块工作区 output 指着马上要释放的 wlr_output，屏插回来
			 * 按 home 认领它的时候就是读野指针（coredump 3062966：workspace_new
			 * 里 output->name 已经是 0x60）。 */
			if (output_present(server, old)) {
				vidow_workspace_ensure_bottom(server, old);
			}
		}
	}
	if (deliberate && !ws->special && output != NULL) {
		ws->home = vidow_output_memory_for(&server->output_memories,
			output->name);
	}
	/* 列 / 列里的窗口跟着工作区换屏 —— **output == NULL 时也要清**。
	 * 以前这里提前 return，于是「最后一块屏也没了」的时候（关掉嵌套会话的窗口、
	 * 拔掉唯一一块显示器），列和列里的窗口继续指着那块已经被释放的屏：IPC 里
	 * 输出名是一串乱码，之后谁读 view->output / col->output 谁拿到垃圾，XWayland
	 * 的 X 屏幕也跟着变成 0x0 —— X11 客户端看不见也点不动（用户报的
	 * 「xwayland 无法点击交互」就是这条路）。清成 NULL：那块屏回来时按 home 认领。 */
	ws_propagate_output(server, ws, output);
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

/* niri 的 has_windows_or_name()：有窗口或者有名字的工作区不会被自动收掉。 */
static bool ws_keeps(const struct vidow_workspace *ws) {
	return ws->named || !ws_empty(ws);
}

void vidow_workspace_adopt_homeless(struct vidow_server *server,
		struct wlr_output *output) {
	if (server == NULL || output == NULL) {
		return;
	}
	/* 配置在屏出现之前就跑完了：那时候声明的具名工作区还没归属。按声明顺序
	 * 认领到这块屏上（niri：具名工作区按声明顺序出现）。 */
	int k = 0;
	struct vidow_workspace *ws, *tmp;
	wl_list_for_each_safe(ws, tmp, &server->workspaces, link) {
		if (ws->special || ws->output != NULL) {
			continue;
		}
		ws_splice(ws, output, k++);
		/* 列和列里的窗口的 output 也要跟上：这块工作区可能是在「一块屏都没有」
		 * 的时候被新窗口占上的（view_ensure_column 的兜底），光认领工作区的话
		 * 窗口还指着 NULL —— 屏回来了，屏幕上却什么都没有。 */
		ws_propagate_output(server, ws, output);
		if (ws->home == NULL) {
			ws->home = vidow_output_memory_for(&server->output_memories,
				output->name);
		}
	}
	if (k > 0) {
		wlr_log(WLR_INFO, "workspace: %s 认领了 %d 块还没归属的具名工作区"
			"（按声明顺序排在上面）", output->name, k);
	}
}

/* 现在**没有屏**的工作区里挑一块。见 workspace.h 的说明。 */
struct vidow_workspace *vidow_workspace_homeless(struct vidow_server *server,
		struct vidow_output_memory *home) {
	if (server == NULL) {
		return NULL;
	}
	struct vidow_workspace *first = NULL;
	struct vidow_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (ws->special || ws->output != NULL) {
			continue;
		}
		if (home != NULL && ws->home == home) {
			return ws;
		}
		if (first == NULL) {
			first = ws;
		}
	}
	return first;
}

void vidow_workspace_normalize(struct vidow_server *server) {
	/* 配置跑完之后调：认领还没归属的具名工作区、每块屏补上底部空位、收掉
	 * 中间那些空的没名字的工作区。 */
	if (server == NULL) {
		return;
	}
	struct vidow_output *out;
	if (!wl_list_empty(&server->outputs)) {
		struct vidow_output *first =
			wl_container_of(server->outputs.next, first, link);
		vidow_workspace_adopt_homeless(server, first->wlr_output);
	}
	wl_list_for_each(out, &server->outputs, link) {
		vidow_workspace_ensure_bottom(server, out->wlr_output);
	}
	vidow_workspace_cleanup(server);

	/* 第一次跑完配置：让每块屏的**第一块**亮着（niri 的 active_workspace_idx
	 * 一开始就是 0）。启动时那块自动建出来的空工作区先占了 active，配置里声明的
	 * 具名工作区插到它上面之后得把 active 让回去，不然一上来站在一块没名字的
	 * 空工作区上。只在第一次做，免得配置重载把人从当前工作区弹走。 */
	wl_list_for_each(out, &server->outputs, link) {
		ws_promote_first(server, out->wlr_output);
	}
}

void vidow_workspace_ensure_bottom(struct vidow_server *server,
		struct wlr_output *output) {
	if (server == NULL || output == NULL) {
		return;
	}
	int n = vidow_workspace_count(server, output);
	if (n > 0 && ws_empty(vidow_workspace_at(server, output, n))) {
		return; /* 底部那块还空着 */
	}
	if (vidow_workspace_create(server, NULL, output) != NULL) {
		wlr_log(WLR_DEBUG, "workspace: %s 的栈尾补一块空的（现在 %d 块）",
			output->name, vidow_workspace_count(server, output));
	}
}

static void ws_destroy(struct vidow_server *server,
		struct vidow_workspace *ws) {
	wlr_log(WLR_INFO, "workspace: 收掉空工作区（%s 上第 %d 块%s）",
		ws->output != NULL ? ws->output->name : "-",
		vidow_workspace_index(ws), ws->named ? "，具名的不该走到这儿" : "");
	vidow_ipc_emit_workspace(server, "workspace-removed", ws);
	/* 空工作区不该还有窗口指着它，但指针是裸的：清一遍再 free，免得谁拿着
	 * 悬空指针接着用（关窗那条路上 view->ws 还留着）。 */
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->ws == ws) {
			view->ws = NULL;
		}
		if (view->floats_ws == ws) {
			view->floats_ws = NULL;
		}
	}
	/* 正被某个过渡推着（理论上去不到：空的才收，空的也不起过渡 —— 留一手）：
	 * 先把那条过渡收干净，别留一个指向 free 掉的工作区的指针。 */
	struct vidow_output *out;
	wl_list_for_each(out, &server->outputs, link) {
		if (out->slide_from == ws && out->slide_anim != NULL) {
			vidow_anim_cancel(out->slide_anim);
			out->slide_anim = NULL;
		}
	}
	wl_list_remove(&ws->link);
	free(ws);
}

void vidow_workspace_cleanup(struct vidow_server *server) {
	/* niri 的 clean_up_workspaces()：中间那些**空的、没名字的**工作区（不是当前
	 * 亮着的那块、也不是栈底那块）→ 收掉。「切走就消失」说的就是这里。 */
	if (server == NULL) {
		return;
	}
	struct vidow_workspace *ws, *tmp;
	wl_list_for_each_safe(ws, tmp, &server->workspaces, link) {
		if (ws->special || ws->active || ws->output == NULL || ws_keeps(ws)) {
			continue;
		}
		if (vidow_workspace_index(ws) ==
				vidow_workspace_count(server, ws->output)) {
			continue; /* 栈底那块留着：那是「新工作区」位 */
		}
		ws_destroy(server, ws);
	}
}

bool vidow_workspace_set_name(struct vidow_server *server,
		struct vidow_workspace *ws, const char *name) {
	if (server == NULL || ws == NULL || ws->special) {
		return false;
	}
	if (name != NULL && name[0] != '\0') {
		struct vidow_workspace *other = find_by_name(server, name, false);
		if (other != NULL && other != ws) {
			wlr_log(WLR_ERROR, "workspace: 已经有叫 %s 的工作区了", name);
			return false;
		}
	}
	snprintf(ws->name, sizeof(ws->name), "%s", name != NULL ? name : "");
	ws->named = ws->name[0] != '\0';
	wlr_log(WLR_INFO, "workspace: %s 上的第 %d 块工作区%s%s",
		ws->output != NULL ? ws->output->name : "-",
		vidow_workspace_index(ws),
		ws->named ? "起名 " : "去掉名字",
		ws->named ? ws->name : "");
	vidow_ipc_emit_workspace(server, "workspace-changed", ws);
	vidow_workspace_cleanup(server); /* 去掉名字之后可能就该消失了 */
	return true;
}

bool vidow_workspace_focus_index(struct vidow_server *server,
		struct wlr_output *output, int idx) {
	if (output == NULL) {
		output = server->focused_output;
	}
	if (output == NULL) {
		return false;
	}
	int n = vidow_workspace_count(server, output);
	if (n == 0) {
		return false;
	}
	/* niri：序号越界就夹到最后一 块（不是「现建一块」）。 */
	if (idx < 1) {
		idx = 1;
	}
	if (idx > n) {
		idx = n;
	}
	struct vidow_workspace *ws = vidow_workspace_at(server, output, idx);
	if (ws == NULL) {
		return false;
	}
	if (server->focused_output != output) {
		server->focused_output = output; /* 人跟着工作区走：切到别的屏上的那块 */
	}
	return vidow_workspace_activate(server, ws);
}

bool vidow_workspace_focus_step(struct vidow_server *server, int direction) {
	if (server == NULL || direction == 0) {
		return false;
	}
	struct wlr_output *output = server->focused_output;
	if (output == NULL) {
		return false;
	}
	struct vidow_workspace *active = vidow_workspace_active_for(server, output);
	int idx = vidow_workspace_index(active);
	if (idx == 0) {
		return false;
	}
	return vidow_workspace_focus_index(server, output, idx + direction);
}

bool vidow_workspace_focus_previous(struct vidow_server *server) {
	if (server == NULL) {
		return false;
	}
	struct vidow_output *out = NULL;
	struct vidow_output *it;
	wl_list_for_each(it, &server->outputs, link) {
		if (it->wlr_output == server->focused_output) {
			out = it;
			break;
		}
	}
	if (out == NULL || out->prev_ws_id == 0) {
		return false;
	}
	uint64_t id = out->prev_ws_id;
	struct vidow_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (!ws->special && ws->id == id && ws->output == out->wlr_output) {
			return vidow_workspace_activate(server, ws);
		}
	}
	return false;
}

bool vidow_workspace_move_step(struct vidow_server *server,
		struct vidow_workspace *ws, int direction) {
	if (server == NULL || ws == NULL || ws->special || direction == 0 ||
			ws->output == NULL) {
		return false;
	}
	int idx = vidow_workspace_index(ws);
	int n = vidow_workspace_count(server, ws->output);
	int want = idx + direction;
	if (idx == 0 || want < 1 || want > n) {
		return false;
	}
	wl_list_remove(&ws->link);
	wl_list_init(&ws->link);
	ws_splice(ws, ws->output, want - 1);
	wlr_log(WLR_INFO, "workspace: %s 上的工作区 %d ⇄ %d",
		ws->output->name, idx, want);
	vidow_ipc_emit_workspace(server, "workspace-changed", ws);
	vidow_arrange(server);
	return true;
}

struct vidow_workspace *vidow_workspace_active_for(struct vidow_server *server,
		struct wlr_output *output) {
	/* 每块屏各自一份工作区栈（niri），所以这块屏上：
	 *   ① 已经亮着的那块 → 就是它；
	 *   ② 有工作区但没亮的（刚插上屏 / 刚搬过来）→ 第一块亮起来（niri 的
	 *      active_workspace_idx = 0）；
	 *   ③ 一块都没有 → 建一块空的，并保证底部还有一个空位。 */
	if (server == NULL || output == NULL) {
		return NULL;
	}
	struct vidow_workspace *ws;
	struct vidow_workspace *first = NULL;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (!ws_on_output(ws, output)) {
			continue;
		}
		if (ws->active) {
			return ws;
		}
		if (first == NULL) {
			first = ws;
		}
	}
	if (first == NULL) {
		/* 还没归属过屏的具名工作区（配置先跑、屏后到）：认领过来。 */
		vidow_workspace_adopt_homeless(server, output);
		wl_list_for_each(ws, &server->workspaces, link) {
			if (ws_on_output(ws, output)) {
				first = ws;
				break;
			}
		}
	}
	if (first == NULL) {
		first = vidow_workspace_create(server, NULL, output);
		if (first == NULL) {
			return NULL;
		}
	}
	first->active = true;
	vidow_workspace_ensure_bottom(server, output);
	return first;
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

static struct vidow_output *output_of(struct vidow_server *server,
		struct wlr_output *wlr_output);

bool vidow_workspace_view_visible(struct vidow_server *server,
		struct vidow_view *view) {
	if (view == NULL || view->popup) {
		return false; /* 弹出物不进工作区，跟父窗口走 */
	}
	/* 最小化的窗口不算可见 —— 它是**唯一**的判据口，别的判断都从这儿问：
	 *   场景节点（apply_visibility）、浮窗摆放（float.c）、overview 收集、
	 *   补帧那条路的 view_on_screen()（最小化让「画在画面上」变假 → 规则要求
	 *   保活 / 有人录屏的窗口照样出帧，见 view.c 的 vidow_view_render_pump）。 */
	if (view->minimized) {
		return false;
	}
	/* 小窗（mini window，S2）**跟屏不跟工作区**（像安卓 PiP）：切工作区它不消失。
	 * 和 overview 那条旁路（view.c 的 vidow_view_should_park）同一个位置、同一个
	 * 思想：可见性只有这一个判据口，所以旁路也只开在这儿。进小窗态时 view->ws
	 * 已经摘成 NULL（float.c 的 vidow_mini_set_state），下面那句
	 * `view->ws == active` 必然为假 —— 必须在这儿提前放行。
	 *
	 * 这一处改完，三处自动跟着对：vidow_view_should_park()（它调这个函数）、
	 * view_on_screen()（view.c）、vidow_workspace_apply_visibility()（走 should_park）。
	 * 判据只看「它有没有屏」—— 小窗是屏级对象。 */
	if (view->mini != VIDOW_MINI_NONE) {
		return view->output != NULL;
	}
	/* 「这块窗口住哪块屏」：工作区说在哪就在哪。view->output 是住址，热插拔 /
	 * 规则搬屏的中间态里会短暂分叉；可见性按它判的话，窗口在新屏上不显示、
	 * 在旧屏上又因为工作区不对而不显示，两边都没有。 */
	struct wlr_output *output =
		view->ws != NULL && view->ws->output != NULL ?
		view->ws->output : view->output;
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
	if (active != NULL && view->ws == active) {
		return true;
	}
	/* 切工作区的过渡里，正在被推开的那块**也画**，但那条不算「可见」——
	 * 这里只答「它属于现在显示着的那块工作区吗」。要画的那一份在
	 * view.c 的 vidow_view_should_park() 里单独放行（pick-window / 切换器 /
	 * 录屏这些问「可见」的地方不能被过渡里那半屏旧窗口带偏）。 */
	return false;
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

/* 把「窗口 / 列住哪块屏」对齐到「它所在的工作区住哪块屏」。
 *
 * 唯一真值是 ws->output：工作区在屏上，窗口在工作区里，所以窗口就在那块屏上。
 * view->output / col->output 只是缓存下来的住址，切工作区、规则搬屏、热插拔、
 * 拖动跨屏都会短暂改到一半；只要有一处没跟上，滚动越界时「按几何框算归属」的
 * 那条路就会把窗口算到隔壁屏的工作区里。每次 arrange 收口时对齐一遍，任何路径
 * 都不需要自己记得同步。
 *
 * 例外：小窗（mini）是**屏级对象**，view->ws 是 NULL，它自己就是最终住址；
 * 弹窗不进布局，位置由客户端给死，也不动。 */
void vidow_workspace_sync_outputs(struct vidow_server *server) {
	if (server == NULL) {
		return;
	}
	struct vidow_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		struct vidow_column *col;
		wl_list_for_each(col, &ws->columns, link) {
			if (ws->output != NULL && col->output != ws->output) {
				col->output = ws->output;
			}
			struct vidow_view *view;
			wl_list_for_each(view, &col->views, column_link) {
				if (ws->output != NULL && view->output != ws->output) {
					view->output = ws->output;
				}
			}
		}
	}
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->popup || view->ws == NULL || view->mini != VIDOW_MINI_NONE) {
			continue;
		}
		if (view->ws->output != NULL && view->output != view->ws->output) {
			view->output = view->ws->output;
		}
	}
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
			/* 弹出物跟父窗口：父窗口藏起来了（换了工作区），菜单也得收。父窗口
			 * 滚出自己那块屏（停到停车位上）时也一样收 —— 弹出物不在父窗口那棵
			 * 树里（X11 的在单独一层上），位置是按父窗口的**布局坐标**算的，
			 * 父窗口都停到屏外了它还会留在原地。 */
			if (view->parent == NULL) {
				continue;
			}
			want = view->mapped &&
				vidow_workspace_view_visible(server, view->parent) &&
				!view->parent->scene_parked &&
				vidow_view_in_output_box(server, view);
		} else {
			/* 滚出自己那块屏的列不画：场景图是全局坐标，它的坐标会落进隔壁
			 * 那块屏的框里 —— 于是就画到隔壁显示器上了。浮动窗口、正被拖的
			 * 窗口本来就允许跨屏（拖过去就是要跨），overview 里所有窗口都被
			 * 缩进画面 —— 这三种照旧显示。判据就这一份（vidow_view_should_park，
			 * view_anim_place 每帧也问它）。
			 *
			 * 「不画」= 停到停车位（scene_parked），**不是**关节点：关了的话按
			 * 窗口录屏 / 选择器里的预览（它们渲染的就是这棵子树）拿到的是黑的。
			 * 见 server.h 的 scene_parked。 */
			want = !vidow_view_should_park(server, view);
		}
		if (want) {
			shown++;
		}
		/* 越界的那一块裁掉：上面那条是「整块滚出自己那块屏」，这里是「还看得见
		 * 一部分、另一部分跨过了屏边界」—— 跨过去的那半块不能画到隔壁屏上。 */
		vidow_view_apply_clip(view);
		if (view->popup) {
			/* 弹出物照旧关节点：它们不是截图源（窗口截图源指的一定是 toplevel
			 * 那棵树），关掉不影响录屏；而且它们挂在单独一层上，跟着父窗口
			 * 停车那套走不了。 */
			if (node_enabled(view->scene_tree) != want) {
				vidow_backdrop_damage_view(view);
				wlr_log(WLR_DEBUG, "workspace: 弹出物 %s %s",
					vidow_view_title(view) != NULL ?
						vidow_view_title(view) : "(无题)",
					want ? "显示" : "藏起");
				wlr_scene_node_set_enabled(&view->scene_tree->node, want);
			}
			continue;
		}
		if (view->scene_parked == want) {
			/* 该画却停着（或该停着却还画着）：立刻摆到位、**并且把这一帧的装饰
			 * 重烘掉** —— 滚回视口的那一瞬间节点还在停车位上（下一帧才摆回来的
			 * 话屏幕上会闪一下「没有」），而且半块越界的那种窗口必须在同一帧里
			 * 就裁好，否则它整个身子会先画在隔壁屏上、等下一帧才收回来（就是
			 * 那个「回来的瞬间跑到另一块屏上」）。翻转本身（含日志 / 损伤）在
			 * place 那一份判据里做，见 vidow_view_set_parked。 */
			vidow_view_apply_anim(view);
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
	 * 否则找这块工作区里的第一列，再找浮窗，最后才把焦点放空。
	 *
	 * 「找浮窗」这一档是补的：一块工作区上的窗口**全是浮窗 / 自由小窗**时
	 * （用户桌上那堆小窗就是），只翻列的话一个都找不到 —— 焦点被清空，键盘
	 * 打进虚空。现象就是「切过去之后焦点没跟过来」。小窗（MINI / ICON）跳过：
	 * 不变量是 focused 永远不指小窗（要给焦点得先摊开，见
	 * vidow_focus_view_expand_mini，那条路是 Alt+Tab / 全景点选 / IPC 走
	 * 的，不该在切工作区时替用户把窗口改形态）。 */
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
	struct vidow_view *fv;
	wl_list_for_each(fv, &server->views, link) {
		if (fv->ws != ws || !fv->mapped || fv->minimized || fv->popup ||
				fv->mini != VIDOW_MINI_NONE) {
			continue;
		}
		vidow_focus_view(fv);
		return;
	}
	server->focused = NULL;
	wlr_seat_keyboard_notify_clear_focus(server->seat);
	vidow_im_focus(server, NULL);
	vidow_arrange(server);
}

bool vidow_workspace_focus_output(struct vidow_server *server,
		struct wlr_output *output) {
	if (server == NULL || output == NULL) {
		return false;
	}
	struct vidow_workspace *ws = vidow_workspace_active_for(server, output);
	if (ws == NULL) {
		return false;
	}
	server->focused_output = output;
	focus_workspace(server, ws);
	return true;
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
	/* **无条件**同步（包括 col->output 已经等于 target->output 的情况）：
	 * 列里的窗口可能有个别 view->output 落在别的屏上（规则搬屏 / 热插拔的
	 * 中间态），只看列自己的 output 会漏掉它们，之后 view_home_output 和
	 * vidow_view_in_output_box 一个按工作区、一个按住址，裁切和停车位就会
	 * 各算各的。 */
	if (target->output != NULL) {
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

/* ----------------------------------------------- 切工作区的过渡（纵向滑动） */

/* 一块屏上只有一块普通工作区亮着（见 vidow_workspace_shown_for），所以「切工作
 * 区」本来是瞬间换人。这里给它一个过渡：旧的那块往上/下推开，新的从另一边进来 ——
 * niri 的纵向滑动。
 *
 * 做法：过渡期间两块工作区**都在场景里**，各自的纵向偏移存在 ws->slide_y 里；
 * 摆位在 view.c 的 output_arrange（它按各自的偏移把两块都摆一遍），可见性由
 * vidow_workspace_view_visible 放行。推开的距离是整块屏高 —— 到位时旧的那块
 * 正好整块落在屏外，被 vidow_view_should_park 停到停车位（不画）。
 *
 * 一帧一帧地重摆（每帧 vidow_arrange），所以窗口的位置**是过渡推出来的**：过渡
 * 期间 vidow_place_view 不给每个窗口起位移补间（见 server.h 的 workspace_slide_n），
 * 整块工作区刚性跟着曲线走。 */
struct ws_slide {
	struct vidow_server *server;
	struct vidow_output *output;
	struct vidow_workspace *from;
	struct vidow_workspace *to;
	struct vidow_anim *anim;
	int height; /* 推开多远：整块屏高 */
	int dir;    /* +1 = 新的从下面进来（旧往上走），-1 = 反过来 */
	/* 起手那一刻**这两块**画在哪（快速连切时不是 0，见 slide_start）。插值从
	 * 这儿走到目标，而不是永远从 0 起步 —— 从 0 起步就是「先啪地跳回去再滑」，
	 * 用户报的「工作区快速切换的动画不够丝滑」。 */
	double from0, to0;
};

/* 过渡期间旧工作区压在新工作区上面：新工作区只能从旧工作区让出来的
 * 下方露出来，不能反过来先盖住旧工作区。 */
static void slide_raise_from(struct vidow_server *server,
		struct vidow_output *output) {
	if (server == NULL || output == NULL || output->slide_from == NULL) {
		return;
	}
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		/* 旧工作区在过渡里**不算可见**（vidow_workspace_view_visible 只认
		 * 当前工作区），所以这里不能拿它当判据 —— 用它的话整个函数永远
		 * 不抬任何节点，新工作区（focus 后还会再被 restack 抬一次）就压在
		 * 旧工作区上面：滑动时看到的是新窗口盖着旧窗口，而不是旧工作区把
		 * 新的从下面拉进来。 */
		if (view->ws == output->slide_from && view->scene_tree != NULL &&
				view->mapped && !view->minimized && !view->popup) {
			wlr_scene_node_raise_to_top(&view->scene_tree->node);
		}
	}
}

/* focus / restack 会改节点的层级：每次 arrange 之后把过渡里正滑走的旧工作区
 * 重新抬到上面（见 slide_raise_from）。没有过渡时这是一次空遍历。 */
void vidow_workspace_slide_raise(struct vidow_server *server) {
	if (server == NULL) {
		return;
	}
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		slide_raise_from(server, output);
	}
}

static void slide_update(void *data, double t, double linear) {
	(void)t;
	struct ws_slide *sl = data;
	if (sl->from == NULL || sl->to == NULL || sl->output == NULL) {
		return;
	}
	/* 两块工作区当成一条连续的带子一起走：旧工作区底边和新工作区顶边
	 * 始终贴在一起，中间不露背景。旧工作区在场景里压在新工作区上面
	 * （slide_raise_from），所以看起来是旧工作区把新的从下面拉进来，
	 * 而不是新的先盖住旧的。 */
	double from_end = sl->dir > 0 ? -(double)sl->height
				      : (double)sl->height;
	double to_start = sl->dir > 0 ? (double)sl->height
				      : -(double)sl->height;
	double p = linear;
	if (p < 0.0) {
		p = 0.0;
	} else if (p > 1.0) {
		p = 1.0;
	}
	double k = vidow_ease_value(&sl->server->config.workspace_easing, p);
	double from_y = sl->from0 + (from_end - sl->from0) * k;
	double to_y = to_start + (0.0 - to_start) * k;
	sl->from->slide_y = from_y;
	sl->to->slide_y = to_y;
	vidow_arrange(sl->server);
	slide_raise_from(sl->server, sl->output);
	vidow_output_schedule_frame(sl->server, sl->output->wlr_output,
		"切工作区过渡");
}

static void slide_done(void *data, bool cancelled) {
	(void)cancelled;
	struct ws_slide *sl = data;
	if (sl->from != NULL) {
		sl->from->slide_y = 0.0;
	}
	if (sl->to != NULL) {
		sl->to->slide_y = 0.0;
	}
	if (sl->server->workspace_slide_n > 0) {
		sl->server->workspace_slide_n--;
	}
	if (sl->output != NULL) {
		if (sl->output->slide_from == sl->from) {
			sl->output->slide_from = NULL;
		}
		if (sl->output->slide_anim == sl->anim) {
			sl->output->slide_anim = NULL;
		}
	}
	if (sl->from != NULL && sl->to != NULL) {
		vidow_arrange(sl->server);
	}
	free(sl);
}

/* 这块工作区里有没有「看得见的东西」（列里的窗口 / 浮窗）。空的就没必要滑 ——
 * 而且空工作区可能当场就被 vidow_workspace_cleanup 收掉，别留着它的指针。 */
static bool ws_has_something(struct vidow_workspace *ws) {
	if (ws->floats > 0) {
		return true;
	}
	struct vidow_column *col;
	wl_list_for_each(col, &ws->columns, link) {
		struct vidow_view *view;
		wl_list_for_each(view, &col->views, column_link) {
			if (view->mapped && !view->minimized && !view->popup) {
				return true;
			}
		}
	}
	return false;
}

/* 这块屏上换工作区了：起一条过渡（config 的 workspace_switch_ms，0 = 不起）。 */
static void slide_start(struct vidow_server *server, struct vidow_output *output,
		struct vidow_workspace *from, struct vidow_workspace *to) {
	if (output == NULL || from == NULL || to == NULL || from == to) {
		return;
	}
	/* 快速连切（1 → 2 → 3，上一条还没滑完）：屏幕上正停在中间，而下面掐掉上
	 * 一条时它的 done 会把**所有**偏移清零 —— 所以先把这次要用的这两块**当前
	 * 位置**记下来，让新的这条从那儿接着走。不记的话就是「先啪地跳回原位、再从
	 * 原位滑一遍」，用户报的「工作区快速切换的动画不够丝滑」。 */
	double from0 = from->slide_y;
	double to0 = to->slide_y;
	/* 还在滑的那条先掐掉：它的 done 会把偏移清零、把计数减回去。 */
	if (output->slide_anim != NULL) {
		vidow_anim_cancel(output->slide_anim);
		output->slide_anim = NULL;
	}
	if (server->config.workspace_switch_ms <= 0 || !ws_has_something(from)) {
		return;
	}
	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
	int height = box.height > 0 ? box.height : 1080;

	struct ws_slide *sl = calloc(1, sizeof(*sl));
	if (sl == NULL) {
		return;
	}
	sl->server = server;
	sl->output = output;
	sl->from = from;
	sl->to = to;
	sl->height = height;
	/* 新的在栈里比旧的靠下 → 它从下面进来（旧往上走）。 */
	sl->dir = vidow_workspace_index(to) > vidow_workspace_index(from) ? 1 : -1;
	sl->from0 = from0;
	sl->to0 = to0;
	/* 第一帧 tick 之前就把新工作区放到旧工作区下面，别让它先在屏上闪一帧。 */
	to->slide_y = sl->dir > 0 ? (double)height : -(double)height;

	output->slide_from = from;
	server->workspace_slide_n++;
	sl->anim = vidow_anim_start(server, "切工作区",
		server->config.workspace_switch_ms, (struct vidow_ease){
			.kind = VIDOW_EASE_LINEAR,
		}, slide_update, slide_done, sl);
	if (sl->anim == NULL) {
		to->slide_y = 0.0;
		output->slide_from = NULL;
		server->workspace_slide_n--;
		free(sl);
		return;
	}
	output->slide_anim = sl->anim;
	wlr_log(WLR_DEBUG, "workspace: 过渡 %s -> %s（%dms，%s）",
		from->named ? from->name : "(没名字)",
		to->named ? to->name : "(没名字)",
		server->config.workspace_switch_ms, sl->dir > 0 ? "新的从下面进来" : "新的从上面进来");
}

/* 把所有屏上正跑的过渡收干净（到位、不滑动）：overview 打开时用 —— 它按每块
 * 工作区自己的几何算缩略图，正滑着的那块会摆歪。 */
void vidow_workspace_slide_cancel(struct vidow_server *server) {
	if (server == NULL) {
		return;
	}
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->slide_anim != NULL) {
			vidow_anim_cancel(output->slide_anim);
			output->slide_anim = NULL;
		}
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

	/* 切哪块屏：工作区住在哪块屏就切哪块屏（人跟着工作区走）—— niri 里工作区
	 * 是「某块屏上的一层」，不跨屏搬。 */
	struct wlr_output *target = ws->output;
	if (target == NULL) {
		target = server->focused_output;
		if (target == NULL) {
			return false;
		}
		ws_set_output(server, ws, target);
	}
	server->focused_output = target;
	ws_switched_by_user = true;
	struct vidow_output *out = output_of(server, target);

	/* 这块屏上原来亮着的那块让位（每块屏只有一块亮的）。 */
	struct vidow_workspace *prev = NULL;
	struct vidow_workspace *it;
	wl_list_for_each(it, &server->workspaces, link) {
		if (!it->special && it->output == target && it->active) {
			prev = it;
			break;
		}
	}
	if (prev == ws) {
		if (out != NULL) {
			out->viewport_x = ws->viewport_x;
		}
		vidow_workspace_apply_visibility(server);
		vidow_arrange(server);
		vidow_ipc_emit_workspace(server, "workspace-activated", ws);
		return true;
	}
	if (prev != NULL) {
		prev->active = false;
		if (out != NULL) {
			prev->viewport_x = out->viewport_x;
			out->prev_ws_id = prev->id; /* focus-workspace-previous 用 */
		}
		if (server->focused != NULL && server->focused->ws == prev) {
			prev->focused = server->focused;
		}
	}
	ws->active = true;
	vidow_extworkspace_sync(server);
	if (out != NULL) {
		out->viewport_x = ws->viewport_x;
	}
	/* 旧的那块开始让位（过渡期间它还画着，正从屏上滑出去）。 */
	slide_start(server, out, prev, ws);

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
	wlr_log(WLR_INFO, "workspace: 切到 %s 上的第 %d 块%s（本区 %d 列 / %d 个窗口，"
		"全屏 %d 个可见）", target->name, vidow_workspace_index(ws),
		ws->named ? ws->name : "(没名字)", columns,
		vidow_workspace_view_count(ws), shown);

	focus_workspace(server, ws);
	vidow_arrange(server);
	vidow_workspace_slide_raise(server);
	vidow_ipc_emit_workspace(server, "workspace-activated", ws);
	/* 刚离开的那块要是空的、没名字的、又在栈中间 —— 收掉（niri：切走就消失）。 */
	vidow_workspace_cleanup(server);
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

/* 搬完之后的收尾（整列搬和「一条窗口插到别的列里」共用）：补空工作区、通知外面
 * 的脚本、把焦点交回看得见的地方、重排、收掉搬空了的那块。 */
static void ws_move_tail(struct vidow_server *server, struct vidow_view *view,
		struct vidow_workspace *source, struct vidow_workspace *target) {
	if (!target->special && target->output != NULL) {
		/* 搬到的是栈底那块空的 → 它就是「新工作区」了，底下再补一块 */
		vidow_workspace_ensure_bottom(server, target->output);
	}
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
	/* 搬空了的那块（如果它只是栈中间的一块）收掉 */
	vidow_workspace_cleanup(server);
}

/* 把一个窗口搬到某块工作区（IPC 也走这里：不一定是焦点窗口）。列里的窗口
 * 整列搬过去（列是工作区的东西）。 */
bool vidow_workspace_move_view(struct vidow_server *server,
		struct vidow_view *view, struct vidow_workspace *target) {
	if (server == NULL || view == NULL || target == NULL) {
		return false;
	}
	/* 已经在这块工作区上：不搬列，但**住址要归位**。规则先 workspace 后
	 * output 时会走到这儿（output 规则解析出的活动工作区正好就是 view->ws），
	 * 直接返回 false 的话调用方当成「搬不过去」报错，而 view->output 还停在
	 * 创建时那块屏上 —— 归属和住址就此分叉。 */
	if (target == view->ws) {
		if (target->output != NULL) {
			if (view->column != NULL) {
				column_set_output(view->column, target->output);
			} else {
				view->output = target->output;
			}
		}
		return true;
	}

	struct vidow_workspace *source = view->ws;
	if (view->column != NULL) {
		ws_adopt_column(target, view->column);
	} else {
		ws_adopt_view(target, view);
	}

	if (target->special) {
		wlr_log(WLR_INFO, "workspace: 把 %s 搬到 %s（特殊工作区）",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)", target->name);
	} else {
		wlr_log(WLR_INFO, "workspace: 把 %s 搬到 %s 上的第 %d 块%s",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)",
			target->output != NULL ? target->output->name : "-",
			vidow_workspace_index(target),
			target->named ? target->name : "(没名字)");
	}
	ws_move_tail(server, view, source, target);
	return true;
}

/* 把**一条窗口**（不是整条列）插到别的工作区某条列里的第 index 位：拖拽落点算
 * 出「进这条列」、而目标列在另一块工作区（另一块屏）上时走这条。niri 里被拖的
 * 是窗口自己（不是列），所以原来那条列里还有别人就留在原处，只剩它一个才把空列
 * 拆掉。 */
bool vidow_workspace_move_view_into_column(struct vidow_server *server,
		struct vidow_view *view, struct vidow_column *target_col, int index) {
	if (server == NULL || view == NULL || target_col == NULL ||
			target_col->ws == NULL || target_col->ws == view->ws) {
		return false;
	}
	struct vidow_workspace *source = view->ws;
	struct vidow_workspace *target = target_col->ws;

	/* 顺序要紧：先把窗口放进目标列（目标工作区立刻就算「有东西」了，收尾里那次
	 * cleanup 不会把它当成空工作区收掉），再走收尾。 */
	view->ws = target;
	if (target_col->output != NULL) {
		view->output = target_col->output;
	}
	if (!vidow_view_insert_into_column(view, target_col, index)) {
		view->ws = source;
		return false;
	}

	wlr_log(WLR_INFO, "workspace: 把 %s 插到 %s 上的第 %d 块工作区的列里",
		vidow_view_title(view) != NULL ?
			vidow_view_title(view) : "(无题)",
		target->output != NULL ? target->output->name : "-",
		vidow_workspace_index(target));
	ws_move_tail(server, view, source, target);
	return true;
}

/* 按「名字」或「序号」搬（niri：纯数字 = 那块屏上的第几块，越界夹到最后一 块；
 * 其它 = 具名工作区 / 特殊工作区的名字）。找不到就是找不到 —— 不现建。 */
bool vidow_workspace_move_view_to_name(struct vidow_server *server,
		struct vidow_view *view, const char *name) {
	if (view == NULL || name == NULL || name[0] == '\0') {
		return false;
	}
	char *end = NULL;
	long n = strtol(name, &end, 10);
	if (end != name && *end == '\0' && n > 0) {
		/* 窗口在哪块屏上就按哪块屏的序号算（人在那块屏上操作），窗口不在聚焦
		 * 的屏上时也一样。 */
		struct wlr_output *output = server->focused_output;
		if (view->output != NULL && view->output != output) {
			output = view->output;
		}
		int cnt = vidow_workspace_count(server, output);
		if (cnt == 0) {
			return false;
		}
		int idx = n > cnt ? cnt : (int)n;
		struct vidow_workspace *ws = vidow_workspace_at(server, output, idx);
		return ws != NULL && vidow_workspace_move_view(server, view, ws);
	}
	struct vidow_workspace *target = find_by_name(server, name, true);
	return target != NULL && vidow_workspace_move_view(server, view, target);
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
	bool was_active = ws->active;
	struct wlr_output *from = ws->output;
	ws_set_output_full(server, ws, output, true);
	/* niri：插到那块屏**活动工作区后面**（move_workspace_to_output 里是
	 * `insert_workspace(ws, active_workspace_idx + 1, activate)`）。 */
	struct vidow_workspace *target_active =
		vidow_workspace_active_for(server, output);
	if (target_active != NULL && target_active != ws) {
		int at = vidow_workspace_index(target_active); /* 插在它后面 */
		wl_list_remove(&ws->link);
		wl_list_init(&ws->link);
		ws_splice(ws, output, at);
	}
	{
		/* 搬过去的那块在新屏上亮着（niri 的 move-workspace-to-monitor 搬的就是
		 * 当前那块，所以那边也是「搬过去就看着它」），新屏原来亮着的那块让位，
		 * 老屏自己再挑一块亮起来。 */
		(void)was_active;
		struct vidow_workspace *it;
		wl_list_for_each(it, &server->workspaces, link) {
			if (!it->special && it->active && it->output == output &&
					it != ws) {
				it->active = false;
			}
		}
		ws->active = true;
		server->focused_output = output;
		if (from != NULL) {
			vidow_workspace_active_for(server, from);
		}
	}
	vidow_workspace_ensure_bottom(server, output);
	wlr_log(WLR_INFO, "workspace: 把工作区（%s，%d 个窗口）整块搬到 %s 上，现在是"
		"那边的第 %d 块", ws->named ? ws->name : "没名字",
		vidow_workspace_view_count(ws), output->name,
		vidow_workspace_index(ws));
	vidow_workspace_apply_visibility(server);
	vidow_arrange(server);
	vidow_ipc_emit_workspace(server, "workspace-changed", ws);
	vidow_workspace_cleanup(server);
	return true;
}

void vidow_workspace_output_gone(struct vidow_server *server,
		struct wlr_output *dead, struct wlr_output *to) {
	struct vidow_workspace *ws, *tmp;
	int moved = 0;
	bool took_over = false;
	/* 遍历时会把这些工作区搬进别的屏的栈里（链表位置会变）→ 必须 _safe */
	wl_list_for_each_safe(ws, tmp, &server->workspaces, link) {
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
		vidow_workspace_ensure_bottom(server, to);
		vidow_workspace_cleanup(server);
	}
}

void vidow_workspace_output_added(struct vidow_server *server,
		struct vidow_output_memory *memory, struct wlr_output *back) {
	int home = 0;
	struct vidow_workspace *ws, *tmp;
	wl_list_for_each_safe(ws, tmp, &server->workspaces, link) {
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
	vidow_workspace_adopt_homeless(server, back);
	vidow_workspace_ensure_bottom(server, back);
	vidow_workspace_cleanup(server);
}

void vidow_workspace_track(struct vidow_workspace *ws,
		struct vidow_view *view, bool enter) {
	if (view == NULL) {
		return;
	}
	if (!enter) {
		/* 摘账以「记在哪块工作区」为准，不信调用方传来的 ws：窗口搬家 /
		 * 工作区被收掉时那个参数可能已经过期。 */
		struct vidow_workspace *old = view->floats_ws;
		if (old == NULL) {
			return;
		}
		view->floats_ws = NULL;
		if (old->floats > 0) {
			old->floats--;
		}
		return;
	}
	if (view->column != NULL) {
		return; /* 列里的窗口不算浮窗 */
	}
	if (ws == NULL || view->floats_ws == ws) {
		return; /* 没工作区可记 / 已经记在这块上了 */
	}
	/* 搬到另一块工作区：先把旧账摘掉，再记新的。 */
	struct vidow_workspace *old = view->floats_ws;
	if (old != NULL) {
		view->floats_ws = NULL;
		if (old->floats > 0) {
			old->floats--;
		}
	}
	view->floats_ws = ws;
	ws->floats++;
	if (!ws->special && ws->output != NULL) {
		/* 浮窗落在栈底那块空的上了：底下再补一块（记完账再判「空不空」） */
		vidow_workspace_ensure_bottom(ws->server, ws->output);
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

bool vidow_workspace_is_active(const struct vidow_workspace *ws) {
	return ws != NULL && ws->active;
}

bool vidow_workspace_is_visible(const struct vidow_workspace *ws) {
	return ws != NULL && (ws->active || ws->visible);
}

/* 这块工作区上有几个窗口。**最小化的也算** —— 它还在列里、还指着这块工作区，
 * 数少了就会被 vidow_workspace_cleanup() 当成「空的、没名字的」收掉，把窗口的
 * 住处一起拆了（最小化的窗口要能原样放回来，工作区就得留着）。 */
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
