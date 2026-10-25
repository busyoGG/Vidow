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

#endif
