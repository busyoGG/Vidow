/* float 模块的内部常量（只在 src/float*.c 之间用）。
 *
 * 对外 API 在 server.h（vidow_float_* / vidow_mini_*）；这里只是三块实现
 * （模式判定 / 几何与拖拽 / 迷你小窗）都必须认同的几个数。 */
#ifndef VIDOW_FLOAT_INTERNAL_H
#define VIDOW_FLOAT_INTERNAL_H

/* 浮动窗口的尺寸下限：拖动、吸附、跨屏都不许小于它。 */
#define FLOAT_MIN_SIZE 64

/* 浮动链的最大深度（FOLLOW 模式的窗口跟着父窗口浮动，允许套几层）。 */
#define FLOAT_MAX_DEPTH 8

#endif
