/* 调试开关：环境变量或文件皆可打开，文件方式不用重启。
 *
 * 为什么要文件版：SDDM 起会话时不传环境变量，以前加完诊断只能改
 * wayland-sessions/*.desktop 再重启，一次部署等两分钟。现在
 *     touch /tmp/vidow-x11-trace     # 开
 *     rm    /tmp/vidow-x11-trace     # 关
 * 立刻生效。检查结果缓存 2 秒，免得每个 X 事件都做一次 stat。 */
#ifndef VIDOW_XTRACE_H
#define VIDOW_XTRACE_H

#include <stdbool.h>

/* X11/XWM 全链路追踪。 */
bool vidow_xtrace_on(void);
/* 光标链路的专属探针（文件 /tmp/vidow-cursor-trace 或环境变量
 * VIDOW_CURSOR_TRACE）。和 X11 全量 trace 分开：那个开着的时候游戏里每次
 * pointer motion 都写一行，复现「出去回来光标不重绘」这种几秒钟的事会被
 * 淹掉。这个只记光标账本的进出（set_cursor 请求是否被收、fx 补画走了哪
 * 一支、默认箭头什么时候盖上去）。 */
bool vidow_cursor_trace_on(void);

/* 拖拽跟手探针：位置这条链（grab -> place -> bake -> fx 摆节点）逐帧写进
 *     /tmp/vidow-grab-trace.log
 * **不走 wlr_log**：这个会话的 stderr 重定向到 wayland-session.log，stdio 全缓冲
 * —— 拖动时那几十行可能很久都不落盘，排查时看着像「什么都没发生」（实测 03:39
 * 之后一行没有）。这里每次 append 打开 / 关闭，写完就在文件里，tail -f 就能看。
 * 调用点自己判 vidow_xtrace_on()，所以 touch /tmp/vidow-x11-trace 就是全开。 */
void vidow_gtrace(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

/* 客户端交帧探针（view_commit 里每 60 次 commit 一行）。
 *
 * 原神这类满速提交的 X11 游戏跑 160fps，无条件打就是每秒 2.7 行、一天
 * 二十多万行往 session log 里写。默认关，要看时：
 *     touch /tmp/vidow-commit-trace    # 开
 *     rm    /tmp/vidow-commit-trace    # 关
 * 或者 VIDOW_COMMIT_TRACE=1 起会话。 */
bool vidow_commit_trace_on(void);

#endif /* VIDOW_XTRACE_H */
