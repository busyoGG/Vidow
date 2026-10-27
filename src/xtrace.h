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

/* 客户端交帧探针（view_commit 里每 60 次 commit 一行）。
 *
 * 原神这类满速提交的 X11 游戏跑 160fps，无条件打就是每秒 2.7 行、一天
 * 二十多万行往 session log 里写。默认关，要看时：
 *     touch /tmp/vidow-commit-trace    # 开
 *     rm    /tmp/vidow-commit-trace    # 关
 * 或者 VIDOW_COMMIT_TRACE=1 起会话。 */
bool vidow_commit_trace_on(void);

#endif /* VIDOW_XTRACE_H */
