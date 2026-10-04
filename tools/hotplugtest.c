/* 显示器热插拔的离线自检：不启动合成器，直接验「记住 / 还原」的那点纯逻辑。
 *   meson compile -C build hotplugtest && ./build/hotplugtest
 * 退出码 0 = 全过。
 *
 * 合成器里那层（哪些列属于哪块屏、浮动窗口怎么平移）在 test-hotplug.sh 里用真
 * headless 输出拔插来验；这里只管纯函数：输出身份、记忆台账、挑接收的屏、稳定重排。 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hotplug.h"

static int failures;

static void check(bool ok, const char *what) {
	if (!ok) {
		failures++;
		printf("FAIL %s\n", what);
	} else {
		printf("ok   %s\n", what);
	}
}

static void test_key(void) {
	printf("== 输出身份 ==\n");
	char buf[VIDOW_OUTPUT_KEY_MAX];

	vidow_output_key("DP-1", buf, sizeof(buf));
	check(strcmp(buf, "DP-1") == 0, "连接器名原样用（DP-1）");

	vidow_output_key("HEADLESS-2", buf, sizeof(buf));
	check(strcmp(buf, "HEADLESS-2") == 0, "headless 输出也一样（HEADLESS-2）");

	vidow_output_key(NULL, buf, sizeof(buf));
	check(buf[0] == '\0', "没有名字 -> 空串（记不住，也就不还原）");

	vidow_output_key("", buf, sizeof(buf));
	check(buf[0] == '\0', "空名字 -> 空串");

	char long_name[200];
	memset(long_name, 'x', sizeof(long_name));
	long_name[sizeof(long_name) - 1] = '\0';
	vidow_output_key(long_name, buf, sizeof(buf));
	check(strlen(buf) == VIDOW_OUTPUT_KEY_MAX - 1 && buf[0] == 'x',
		"超长的名字截断，不会溢出（63 个字符 + 结尾）");

	char tiny[8];
	vidow_output_key("DP-1", tiny, sizeof(tiny));
	check(strcmp(tiny, "DP-1") == 0, "buffer 小也照常写，只要放得下");
	tiny[1] = '\0';
	vidow_output_key("DP-1", tiny, 1);
	check(tiny[0] == '\0', "len = 1 -> 只写结尾的 0");
}

static void test_memory(void) {
	printf("== 记忆台账 ==\n");
	struct wl_list memories;
	wl_list_init(&memories);

	struct vidow_output_memory *a =
		vidow_output_memory_for(&memories, "DP-1");
	struct vidow_output_memory *b =
		vidow_output_memory_for(&memories, "HDMI-A-1");
	check(a != NULL && b != NULL, "第一次见的屏会新建一条记忆");
	check(a != b && strcmp(a->key, "DP-1") == 0 && strcmp(b->key, "HDMI-A-1") == 0,
		"两条记忆各自记得自己的名字");
	check(!a->present && a->output == NULL && a->viewport_x == 0.0,
		"新记忆的初始状态：不在场、没有输出、滚动位置 0");

	struct vidow_output_memory *again =
		vidow_output_memory_for(&memories, "DP-1");
	check(again == a,
		"同一个名字再来拿到同一条（拔了又插回来认的就是它）");

	struct vidow_output_memory *a2 =
		vidow_output_memory_for(&memories, "DP-1");
	check(a2 == a, "连问三次还是同一条，不会越记越多");

	int n = 0;
	struct vidow_output_memory *m;
	wl_list_for_each(m, &memories, link) {
		n++;
	}
	check(n == 2, "台账里现在正好两条");

	check(vidow_output_memory_for(&memories, NULL) == NULL,
		"没名字的屏不给记忆（省得每块无名屏都占一条）");
	check(vidow_output_memory_for(&memories, "") == NULL, "空名字也一样");

	/* 记忆活多久：合成器不删它，只有进程退出才没 —— 这里只验它不是临时对象 */
	check(vidow_output_memory_for(&memories, "HDMI-A-1") == b,
		"后来插进来的屏不影响别的记忆");
}

static void test_fallback(void) {
	printf("== 拔线时挑哪块屏接收 ==\n");
	/* 三屏横排：A 在 0、B 在 1280、C 在 1920。
	 * 调用的时候被拔的那块已经从 server->outputs 里摘掉了（所以这里也不在表里），
	 * live=false 那几项用来验「已经拔掉的屏不参与挑选」。 */
	size_t got;

	struct vidow_hotplug_slot no_b[2] = {
		{ .x = 0,    .live = true },
		{ .x = 1920, .live = true },
	};
	got = vidow_hotplug_pick_fallback(no_b, 2, 1280);
	check(got == 0, "拔中间的屏 -> 接左边最近的那块（视觉上接着原来的位置）");

	struct vidow_hotplug_slot no_a[2] = {
		{ .x = 1280, .live = true },
		{ .x = 1920, .live = true },
	};
	got = vidow_hotplug_pick_fallback(no_a, 2, 0);
	check(got == 0, "拔最左边 -> 左边没有，取右边最近的");

	struct vidow_hotplug_slot no_c[2] = {
		{ .x = 0,    .live = true },
		{ .x = 1280, .live = true },
	};
	got = vidow_hotplug_pick_fallback(no_c, 2, 1920);
	check(got == 1, "拔最右边 -> 左边最近的是中间那块");

	struct vidow_hotplug_slot all[3] = {
		{ .x = 0,    .live = true },
		{ .x = 1280, .live = true },
		{ .x = 1920, .live = true },
	};
	got = vidow_hotplug_pick_fallback(all, 3, -5000);
	check(got == 0, "拔的屏在所有活屏左边 -> 取最靠左的那块");

	struct vidow_hotplug_slot same[2] = {
		{ .x = 700, .live = true },
		{ .x = 900, .live = true },
	};
	check(vidow_hotplug_pick_fallback(same, 2, 700) == 0,
		"原点一样（重叠）的活屏算「左边」，距离 0 优先");

	struct vidow_hotplug_slot mixed[4] = {
		{ .x = 0,    .live = true },
		{ .x = 1000, .live = false }, /* 已经拔掉的那块 */
		{ .x = 2000, .live = true },
		{ .x = 3000, .live = false },
	};
	check(vidow_hotplug_pick_fallback(mixed, 4, 2500) == 2,
		"已经拔掉的屏不参与挑选（挑到活着的 2000 那块，而不是隔壁拔掉的 3000）");

	struct vidow_hotplug_slot one[1] = { { .x = 100, .live = true } };
	check(vidow_hotplug_pick_fallback(one, 1, 9999) == 0,
		"只剩一块屏：不管在哪都给它");

	struct vidow_hotplug_slot none[2] = {
		{ .x = 0,    .live = false },
		{ .x = 1280, .live = false },
	};
	check(vidow_hotplug_pick_fallback(none, 2, 0) == SIZE_MAX,
		"一块活的都没有 -> SIZE_MAX（东西先悬着，等有屏了再安家）");
	check(vidow_hotplug_pick_fallback(NULL, 0, 0) == SIZE_MAX, "空列表也一样");
}

static bool is_permutation(const size_t *order, size_t n) {
	bool seen[16] = {0};
	if (n > 16) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if (order[i] >= n || seen[order[i]]) {
			return false;
		}
		seen[order[i]] = true;
	}
	return true;
}

static void test_regroup(void) {
	printf("== 稳定重排（把一组列整组挪到末尾） ==\n");
	size_t order[8];

	/* 一块屏的列和另一块屏的列在同一条列表里交替出现：把它自己的那组挪到末尾，
	 * 两组的左右顺序都不能乱 —— 这决定了插回来之后列的排列。 */
	bool flagged[5] = { true, false, true, false, true };
	memset(order, 0xEE, sizeof(order));
	vidow_hotplug_regroup(flagged, 5, order);
	check(is_permutation(order, 5), "重排结果是个真排列（不重不漏）");
	check(order[0] == 1 && order[1] == 3, "不属于这组的列保持原来的先后");
	check(order[2] == 0 && order[3] == 2 && order[4] == 4,
		"属于这组的列整组挪到末尾，内部先后不变");

	bool none[4] = { false, false, false, false };
	vidow_hotplug_regroup(none, 4, order);
	check(order[0] == 0 && order[1] == 1 && order[2] == 2 && order[3] == 3,
		"一组都没有 -> 原样");

	bool all[4] = { true, true, true, true };
	vidow_hotplug_regroup(all, 4, order);
	check(order[0] == 0 && order[1] == 1 && order[2] == 2 && order[3] == 3,
		"全都是这组的 -> 顺序不变（挪到末尾=原地）");

	bool single[1] = { true };
	vidow_hotplug_regroup(single, 1, order);
	check(order[0] == 0, "只有一列也照常");

	vidow_hotplug_regroup(NULL, 0, order);
	check(order[0] == 0, "空列表：什么都不写");
}

int main(void) {
	test_key();
	test_memory();
	test_fallback();
	test_regroup();

	printf("\n%s（%d 项失败）\n", failures == 0 ? "全部通过" : "有失败", failures);
	return failures == 0 ? 0 : 1;
}
