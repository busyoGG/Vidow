/* flipprobe.h — 状态翻转探针（VIDOW_FLIP=1 时生效）
 *
 * 目的是**实测**哪些状态在反复切。反复切 = 白烧 GPU：每次翻转都要重画、
 * 重烘底衬、重发 frame_done。没有实测就只能靠猜。
 *
 * 用法：在状态赋值前调 vidow_flip_note("名字", 对象指针, 新值)。每 2 秒
 * 结算一次，同一对象同一状态翻 >= 8 次就打一行 WLR_INFO。
 */
#ifndef VIDOW_FLIPPROBE_H
#define VIDOW_FLIPPROBE_H

#include <stdbool.h>

void vidow_flip_note(const char *what, void *obj, bool now);

/* 探针开着吗（VIDOW_FLIP=1）。
 *
 * **调用方在热路径上（每帧 / 每次摆放）时用这个，别自己 getenv**：
 * getenv 要遍历 environ 做 strcmp，原神那种 160fps 的满速提交下
 * 每秒上千次，实测在 profile 里能占到 0.39%。环境变量进程起来之后不会变，
 * 这里问一次就缓存住。 */
bool vidow_flip_on(void);

#endif
