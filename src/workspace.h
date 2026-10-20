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
	struct vidow_server *server;
	/* 稳定编号：工作区在屏上换位置、换屏都不变（niri 的 Workspace.id）。IPC 和
	 * 「上一块工作区」都认它。 */
	uint64_t id;
	/* 名字：空 = 没名字（动态工作区）。有名字的（配置里 vidow.workspace_create()
	 * 建的、或者 set-workspace-name 给的）空了也不消失 —— 这是 niri 的具名工作区。 */
	char name[32];
	bool named;

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

	/* 切工作区过渡里这块工作区被推开的纵向偏移（布局像素，正常是 0）。
	 * 摆位在 view.c 的 output_arrange：过渡期间两块工作区都在场景里，
	 * 各自按这个偏移摆一遍。见 workspace.c 的 slide_start。 */
	double slide_y;
};

/* 建（或取）一块普通工作区：
 *   name 非空 —— 具名工作区（niri 的 named workspace）：永远存在，空了也不消失。
 *                已经存在就返回它（**不搬家**：配置重载不该把用户手动挪到别的屏上
 *                的工作区拽回来），只有它还没归属过屏时才认领 output。
 *   name 为空 —— 没名字的动态工作区，插到那块屏的栈尾（底部那块「新工作区」位）。
 * 配置声明顺序 = 在屏上的先后顺序（niri：具名工作区按声明顺序出现）。 */
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

/* 现在**没有屏**的工作区（屏拔了 / 嵌套会话的外层窗口关了）里挑一块：优先 home
 * 就是这块屏的，其次第一块；一块都没有返回 NULL。新窗口在「一块屏都没有」的时候
 * 得有个地方安置（见 view.c 的 view_ensure_column）。 */
struct vidow_workspace *vidow_workspace_homeless(struct vidow_server *server,
		struct vidow_output_memory *home);

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

/* 把正跑的切工作区过渡收干净（到位、不滑动）。overview 打开时用：它按每块
 * 工作区自己的几何算缩略图，正滑着的那块会摆歪。 */
void vidow_workspace_slide_cancel(struct vidow_server *server);
/* arrange / restack 之后调：把过渡里正滑走的旧工作区重新抬到新工作区上面。 */
void vidow_workspace_slide_raise(struct vidow_server *server);

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
/* 把**一条窗口**（不是整条列）插到别的（屏/工作区）某条列里的第 index 位：niri
 * 的拖拽落点是「进这条列」而目标列在另一块工作区上时走这条。原来那条列里还有别人
 * 就留着，只剩它一个就把空列拆掉。 */
bool vidow_workspace_move_view_into_column(struct vidow_server *server,
		struct vidow_view *view, struct vidow_column *target_col, int index);
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
/* 把窗口 / 列的住址对齐到它们所在工作区的屏（见 workspace.c 的说明）。 */
void vidow_workspace_sync_outputs(struct vidow_server *server);

/* 给配置语言用：名字、是否特殊、是否激活、有几个窗口。 */
const char *vidow_workspace_name(const struct vidow_workspace *ws);
bool vidow_workspace_is_active(const struct vidow_workspace *ws);
bool vidow_workspace_is_visible(const struct vidow_workspace *ws);
int vidow_workspace_view_count(const struct vidow_workspace *ws);

/* --------------------------------------------------- 按屏的纵向工作区栈

 * niri 的模型：**每块屏各自一份工作区栈**，序号 = 它在自己那块屏上的位置（1 起）。
 * 不变量：
 *   I1 每块屏上最多一块 active（普通）工作区；
 *   I2 每块屏的最后一块永远是空的（「新工作区」位）—— 窗口落到它上面就再补一块；
 *   I3 中间那些**空的、没名字的**工作区，切走以后自动消失（niri 的
 *      clean_up_workspaces），具名工作区永远留着；
 *   I4 全局链表按屏分组、组内按序号排列（这样遍历一遍就等于按屏看序号）。 */

/* 这块屏上有几块普通工作区 / 第 idx 块（1 起，越界返回 NULL —— 调用方按 niri
 * 的语义夹到最后一 块）。 */
int vidow_workspace_count(struct vidow_server *server,
		struct wlr_output *output);
struct vidow_workspace *vidow_workspace_at(struct vidow_server *server,
		struct wlr_output *output, int idx);
/* 它在自己那块屏上的序号（1 起）；没屏 / 特殊工作区 = 0。 */
int vidow_workspace_index(const struct vidow_workspace *ws);
/* 这块屏上现在亮着的那块（没有就挑第一块亮起来，一块都没有就建一块）。 */
struct vidow_workspace *vidow_workspace_active_for(struct vidow_server *server,
		struct wlr_output *output);

/* 切到「那块屏上的第 idx 块」（夹到最后一 块，niri 的 focus-workspace N）。 */
bool vidow_workspace_focus_index(struct vidow_server *server,
		struct wlr_output *output, int idx);
/* 上下切一块（±1，夹住）。 */
bool vidow_workspace_focus_step(struct vidow_server *server, int direction);
/* 回到上一块工作区（focus-workspace-previous）。 */
bool vidow_workspace_focus_previous(struct vidow_server *server);
/* 把键盘焦点交到这块屏现在亮着的工作区（点空区域 / 点另一块屏时用）：
 * focused_output 跟着换，焦点窗口换成那块工作区记住的那个（没有就第一列 /
 * 第一个浮窗；再没有就清空）。niri 的 focus_output 在它那套「每屏一份焦点」
 * 里就是这一件事。 */
bool vidow_workspace_focus_output(struct vidow_server *server,
		struct wlr_output *output);
/* 整块工作区在它那块屏上上/下移一位（niri 的 move-workspace-up/down）。 */
bool vidow_workspace_move_step(struct vidow_server *server,
		struct vidow_workspace *ws, int direction);
/* 给工作区起名 / 去掉名字（set-workspace-name / unset-workspace-name）。 */
bool vidow_workspace_set_name(struct vidow_server *server,
		struct vidow_workspace *ws, const char *name);

/* 底部那块「新工作区」位：最后一块不空了就再补一块没名字的空工作区（I2）。 */
void vidow_workspace_ensure_bottom(struct vidow_server *server,
		struct wlr_output *output);
/* 中间那些空的、没名字的、不是当前亮着的工作区 → 删掉（I3）。 */
void vidow_workspace_cleanup(struct vidow_server *server);
/* 把还没归属过屏的具名工作区认领到这块屏上（配置先跑、屏后到的情形）。 */
void vidow_workspace_adopt_homeless(struct vidow_server *server,
		struct wlr_output *output);
/* 配置跑完之后调一遍：认领没归属的、每块屏补底部空位、收掉中间空块。 */
void vidow_workspace_normalize(struct vidow_server *server);

bool vidow_workspace_init(struct vidow_server *server);

#endif
