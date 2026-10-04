/* 显示器热插拔：一块屏拔掉之后，它上面的列 / 窗口不能丢，也不能再指着已经释放的
 * wlr_output；插回来时还要回到原来的样子（列的顺序和宽度、滚动位置、焦点）。
 *
 * 这份头文件里的东西**不碰 wlroots**（只需要 wayland-util 的 wl_list），所以可以
 * 脱离合成器做离线自检：tools/hotplugtest.c 直接把 hotplug.c 编进去跑。
 *
 * 归属（home）和住处（output）是两件事：
 *   home   —— 这条列 / 这个窗口「属于」哪块屏，记在 vidow_output_memory 里；
 *   output —— 它现在被摆在哪块屏上（那块屏拔了，就借别的屏先住）。
 * 拔线时只动 output，home 留着；插回来时按 home 把东西认领回去。 */
#ifndef VIDOW_HOTPLUG_H
#define VIDOW_HOTPLUG_H

#include <stdbool.h>
#include <stddef.h>

#include <wayland-util.h>

struct wlr_output;
struct vidow_view;

/* 自己的小矩形：不引 wlroots 的头文件，这份头文件就能脱离合成器编译（离线自检）。 */
struct vidow_hotplug_box {
	int x, y, width, height;
};

static inline struct vidow_hotplug_box vidow_hotplug_box(int x, int y, int w,
		int h) {
	return (struct vidow_hotplug_box){ .x = x, .y = y, .width = w, .height = h };
}

/* 输出名（连接器名）最长就这么长。 */
#define VIDOW_OUTPUT_KEY_MAX 64

/* 输出身份 = 连接器名。同一条线拔了再插回来名字不变；换个显示器插同一个口，
 * 名字也一样 —— 那正是「还原工作区」该发生的时候。所以只用名字，不掺 serial。 */
void vidow_output_key(const char *name, char *buf, size_t len);

/* ------------------------------------------------------------- 记忆 */

/* 一块见过的屏。出现时建，拔掉之后**不删** —— 它就等着那块屏回来。 */
struct vidow_output_memory {
	struct wl_list link; /* vidow_server.output_memories */
	char key[VIDOW_OUTPUT_KEY_MAX];

	bool present;                  /* 现在有没有对应的实际输出 */
	struct wlr_output *output;     /* present 时有效 */
	struct vidow_hotplug_box last_box; /* 上次已知的布局位置（逻辑坐标），算平移量 */
	double viewport_x;             /* 离开时的滚动偏移，回来时还原 */
	struct vidow_view *focused;    /* 离开时的焦点窗口（可能已经关了，用之前要验） */
};

/* 按名字找记忆，没有就建一条（calloc；失败返回 NULL）。 */
struct vidow_output_memory *vidow_output_memory_for(struct wl_list *memories,
		const char *name);

/* --------------------------------------------------- 挑接收的屏 / 重排 */

/* 一块屏的候选：布局原点（逻辑坐标）和它还在不在。 */
struct vidow_hotplug_slot {
	int x;
	bool live;
};

/* 拔掉一块屏（原点 dead_x）时，它的列挪到哪块活着的屏上：优先左边最近的那块
 * （视觉上接着原来的位置），没有就取右边最近的，一块活的都没有返回 SIZE_MAX。 */
size_t vidow_hotplug_pick_fallback(const struct vidow_hotplug_slot *slots,
		size_t n, int dead_x);

/* 稳定重排：把 flagged[i] 为真的那些项整组挪到末尾，两组内部顺序都不变。
 * 结果写进 order（长度 n 的下标序列：order[k] 是「新序列第 k 个来自原来的第几个」）。 */
void vidow_hotplug_regroup(const bool *flagged, size_t n, size_t *order);

#endif
