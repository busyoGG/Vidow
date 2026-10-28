/* 自唤醒 fd 统管（实现在 fdwatch.c，和 fd 泄漏看门狗放在同一个文件里）。
 *
 * 合成器自己的自唤醒 fd —— Xwayland 的 ready 管道、配置监听的 inotify、exec
 * 子进程的退出管道、IPC 的连接 / 监听 socket、剪贴板拉取管道、录制给客户端的
 * pipe —— **不挂** wl_event_loop_add_fd：libwayland 会先 dup 一份藏进
 * /proc/self/fd 里那个不让非特权用户读的目录（`lsof -p` 连 fd 都看不到），
 * 而且这些 fd 只要不读干净就是常驻自唤醒。改由一个 16ms 的 timerfd 节拍统一
 * poll：没人有事 = 这一拍什么也不做。
 *
 * 用法（详细语义见 server.h 里那段注释）：
 *
 *   vidow_fdwatch_register(fd, kind, handler, data)  // 可读 → handler，返回 1 = 读完
 *   vidow_fdwatch_peek(fd, kind, poke, data)         // 可读 → poke(data)，自己决定读多少
 *   vidow_fdwatch_deregister(fd, kind, data)         // 关了 fd 记得注销
 *
 * 注册失败 = fdwatch 没起来，调用方自己退回 wl_event_loop_add_fd。
 */
#ifndef VIDOW_FDWATCH_H
#define VIDOW_FDWATCH_H

#include <stdbool.h>
#include <stddef.h>

#include "server.h"

/* 登记表。定义放这儿是为了让同一个编译单元（fdwatch.c）里的函数先看见结构体。 */
struct fdwatch_entry {
	struct wl_list link; /* vidow_fdwatch.entries */
	int fd;
	enum vidow_fdwatch_kind kind;
	vidow_fdwatch_handler handler;
	void *data;
	bool gone;     /* 回调里把自己 dereg 了（fd 已关）：那之后不要再碰 */
	bool on_stack; /* 正在回调里跑（这时候 dereg 不能 free） */
	bool retry;    /* 上一拍说了「还没读完」 */
};

struct fdwatch_peek {
	struct wl_list link; /* vidow_fdwatch.peeks */
	int fd;
	enum vidow_fdwatch_kind kind;
	vidow_fdwatch_poke_cb poke;
	void *data;
	bool gone;
};

struct vidow_fdwatch {
	struct wl_event_source *source;
	int timer_fd;
	bool wake; /* 「本拍末尾马上再来一轮」（poked / 没读完） */
	struct wl_list entries; /* struct fdwatch_entry */
	struct wl_list peeks;   /* struct fdwatch_peek */
};

/* 已经起来了吗（没起来 = register/peek 一律 false）。 */

#endif /* VIDOW_FDWATCH_H */
