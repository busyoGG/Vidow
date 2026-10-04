/* 工作区：一块屏上的一叠列 / 窗口。只有一块当前可见，其余原地藏着 —— 藏着的东西
 * 仍然活着（客户端照常跑、尺寸照常算），只是场景节点被关掉，所以切回来是瞬时的。
 *
 * 两个来源、一个模型：
 *   普通工作区 —— 用户在 config.lua 里 vidow.workspace.create() 建，归属某块屏；
 *   特殊工作区 —— vidow.workspace.special() 建，**不跟屏绑定**：哪块屏是当前
 *                 聚焦的屏，它就显示在哪块屏上（跟随激活的显示器），其余屏上看不到；
 *                 它不出现在 vidow.workspace.list() 里（列表只给普通工作区）。
 *
 * 工作区按名字认，不按下标：配置里写名字，热插拔拔了屏回来还是按同一个名字把东西
 * 放回去（屏拔了它的工作区跟着列一起借住在别的屏上 —— workspace->output 只是
 * 「现在摆在哪」，名字才是身份）。 */
#ifndef VIDOW_WORKSPACE_H
#define VIDOW_WORKSPACE_H

#include <stdbool.h>
#include <stddef.h>

#include <wayland-server-core.h>

#include <wayland-util.h>

struct vidow_server;
struct vidow_view;
struct vidow_column;
struct vidow_output_memory;
struct wlr_output;

struct vidow_workspace {
	struct wl_list link;  /* vidow_server.workspaces */
	char name[32];

	/* 特殊工作区：不进列表、不绑屏、跟随当前聚焦的屏。 */
	bool special;
	/* 归属（home）和住处（output）是两件事，跟列 / 窗口一样：
	 *   home   —— 这块工作区「属于」哪块屏（按连接器名认，屏拔了也不忘）；
	 *   output —— 它现在被摆在哪块屏上（自己那块屏拔了就借别的屏住）。
	 * 特殊工作区不绑屏：home 一直是 NULL，output 跟着聚焦的屏走。 */
	struct wlr_output *output;
	struct vidow_output_memory *home;

	struct wl_list columns; /* vidow_column.link，从左到右 */
	bool active;            /* 轮到这里了 = 这块屏上现在可见的普通工作区 */
	bool visible;           /* 特殊工作区：现在正显示着 */

	/* 离开时的滚动位置：换回来还停在原处 */
	double viewport_x;
	/* 离开时的焦点窗口（可能已经关了，用之前必须验活） */
	struct vidow_view *focused;

	/* 这块工作区里有几个浮窗（浮窗不进列，数出来才能算「空不空」） */
	int floats;
};

/* 建（或取）名字为 name 的普通工作区，归到 output 那块屏上；已经存在就原样返回。
 * name 为空时按屏自动编个号（"1"、"2"…）。 */
struct vidow_workspace *vidow_workspace_create(struct vidow_server *server,
		const char *name, struct wlr_output *output);

/* 建（或取）一个特殊工作区：不绑屏，跟随当前聚焦的屏。 */
struct vidow_workspace *vidow_workspace_create_special(
		struct vidow_server *server, const char *name);

/* 按名字找（只看普通工作区；特殊工作区要名字就去 specials 里找，见下） */
struct vidow_workspace *vidow_workspace_lookup(struct vidow_server *server,
		const char *name);

/* 当前聚焦那块屏上的普通工作区（没建过就建默认那块）。新窗口落在这里。 */
struct vidow_workspace *vidow_workspace_active(struct vidow_server *server);

/* 指定那块屏上的普通工作区（每块屏各自有一块亮着的）。 */
struct vidow_workspace *vidow_workspace_active_for(struct vidow_server *server,
		struct wlr_output *output);

/* 现在真正显示的东西：有特殊工作区开着就是它，否则是普通活动工作区。
 * 视图可见性、新窗口落点、overview 都看这一个函数。 */
struct vidow_workspace *vidow_workspace_shown(struct vidow_server *server);

/* 某块屏上现在显示着的工作区（那块屏上开着特殊工作区就是它，否则是它的活动工作区）。 */
struct vidow_workspace *vidow_workspace_shown_for(struct vidow_server *server,
		struct wlr_output *output);

/* 现在正显示着的特殊工作区（没开 = NULL） */
struct vidow_workspace *vidow_workspace_special_shown(
		struct vidow_server *server);

/* 切到某块工作区（切的是它现在所在的那块屏）：藏旧的、亮新的、还原滚动位置和焦点。 */
bool vidow_workspace_activate(struct vidow_server *server,
		struct vidow_workspace *ws);

/* 开关特殊工作区（跟随当前聚焦的屏）。再调一次 = 关掉。 */
bool vidow_workspace_toggle_special(struct vidow_server *server,
		const char *name);

/* 把焦点窗口（列 / 浮窗）搬到名字为 name 的工作区。返回是否搬动了。 */
bool vidow_workspace_move_focused(struct vidow_server *server,
		const char *name);

/* 把某个窗口搬到某块工作区（IPC 用：不一定是焦点窗口）。列里的窗口整列搬。 */
bool vidow_workspace_move_view(struct vidow_server *server,
		struct vidow_view *view, struct vidow_workspace *target);
/* 按名字搬：特殊工作区按名字找，找不到就当普通工作区建一块。 */
bool vidow_workspace_move_view_to_name(struct vidow_server *server,
		struct vidow_view *view, const char *name);
/* 整块工作区换屏（列 + 浮窗一起过去，home 也跟着改：这是人做的决定）。 */
bool vidow_workspace_move_to_output(struct vidow_server *server,
		struct vidow_workspace *ws, struct wlr_output *output);

/* 一块屏拔了：它名下的工作区跟着列借住到别处（to 可以是 NULL = 暂时无家可归）。 */
void vidow_workspace_output_gone(struct vidow_server *server,
		struct wlr_output *dead, struct wlr_output *to);

/* 一块屏回来了：按 home 把属于它的工作区整块认领回去（列跟着工作区走）。 */
void vidow_workspace_output_added(struct vidow_server *server,
		struct vidow_output_memory *memory, struct wlr_output *back);

/* 把这块特殊工作区显示到指定那块屏上（激活里面的窗口时用窗口所在的那块屏，
 * 而不是「现在聚焦的屏」）。已经开着的另一块特殊工作区会让位。 */
bool vidow_workspace_show_special(struct vidow_server *server,
		struct vidow_workspace *ws, struct wlr_output *output);

/* 视图进/出工作区时记账（浮窗数、当前焦点），view.c 调。 */
void vidow_workspace_track(struct vidow_workspace *ws,
		struct vidow_view *view, bool enter);
void vidow_workspace_note_focus(struct vidow_workspace *ws,
		struct vidow_view *view);

/* 现在这个视图该不该显示（它所在的工作区是不是这块屏上显示着的那块）。
 * view.c 在摆放时用它决定要不要开场景节点。 */
bool vidow_workspace_view_visible(struct vidow_server *server,
		struct vidow_view *view);

/* 把所有视图的场景节点按可见性对齐一遍（切工作区 / 特殊工作区开关后调）。
 * 返回新变成可见的视图数。 */
int vidow_workspace_apply_visibility(struct vidow_server *server);

/* 给配置语言用：名字、是否特殊、是否激活、有几个窗口。 */
const char *vidow_workspace_name(const struct vidow_workspace *ws);
bool vidow_workspace_is_special(const struct vidow_workspace *ws);
bool vidow_workspace_is_active(const struct vidow_workspace *ws);
bool vidow_workspace_is_visible(const struct vidow_workspace *ws);
int vidow_workspace_view_count(const struct vidow_workspace *ws);

bool vidow_workspace_init(struct vidow_server *server);

#endif
