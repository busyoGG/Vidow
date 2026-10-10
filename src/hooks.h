/* 事件钩子：配置里写「这件事发生了就跑一条命令 / 跑一个 Lua 函数」。见 hooks.c。
 *
 * 和 niri / Hyprland 的关系：这俩都只有「外挂脚本订阅 socket」一条路（Hyprland 的
 * socket2、niri 的 IPC 事件流），配置里没有钩子。vidow 的配置本身就是 Lua，所以
 * 钩子直接写在配置里 —— 但**底层是同一套事件**：IPC 订阅者拿到的那条 JSON 就是
 * 钩子拿到的那个载荷（见 ipc.h 的事件一节），事件发出去时两边一起喂。
 *
 *   vidow.on("window-opened", "notify-send \"$VIDOW_APP_ID\"")   -- 起一条命令
 *   vidow.on("window-opened", function(e) vidow.log(e.app_id) end) -- 跑一段 Lua
 *   vidow.on("*", "…")                                            -- 全部事件
 */
#ifndef VIDOW_HOOKS_H
#define VIDOW_HOOKS_H

#include <stdbool.h>
#include <stddef.h>

struct vidow_server;
struct jbuf;
struct vidow_hook;

/* 一条钩子的命令最长多少（Lua 那边也按这个截）。 */
#define VIDOW_HOOK_CMD_MAX 1024

/* 待派发的事件队列上限：钩子跑得比事件慢的时候（Lua 里做重活、命令 fork 不过来）
 * 宁可丢新的并打日志，也不能无声地吃内存。 */
#define VIDOW_HOOK_QUEUE_MAX 256

bool vidow_hooks_init(struct vidow_server *server);
void vidow_hooks_finish(struct vidow_server *server);

/* 配置重载：配置里声明的钩子全部作废（新配置会重新声明一遍）。 */
void vidow_hooks_clear(struct vidow_server *server);

/* 挂 / 摘钩子。event 写 "*" = 全部事件。成功返回 id（> 0，vidow.off 用），失败 0。
 * lua 那条走 vidow_script_hook_run()（实现在 script.c：那边才有 Lua 状态机）。 */
int vidow_hook_add_cmd(struct vidow_server *server, const char *event,
		const char *cmd);
int vidow_hook_add_lua(struct vidow_server *server, const char *event,
		void *lua_state, int lua_ref);
bool vidow_hook_remove(struct vidow_server *server, int id);

/* 有钩子吗（事件发射前的快速通道）/ 某个事件有没有人听 / 现存的钩子数。 */
bool vidow_hooks_any(const struct vidow_server *server);
bool vidow_hooks_wanted(const struct vidow_server *server, const char *event);
/* 有多少条钩子会收到这个事件（IPC 的 emit 命令报出去）。 */
int vidow_hooks_matching(const struct vidow_server *server, const char *event);
int vidow_hooks_count(const struct vidow_server *server);

/* 派发一个事件：json 是事件信封的完整 JSON（一行，带结尾换行，和给 IPC 订阅者
 * 的那条一模一样）。**只入队**，真正执行在事件循环的 idle 里 —— 钩子里能做的事
 * 和 IPC 命令一样多（改配置、关窗口），不能站在合成的半路上跑。 */
void vidow_hooks_dispatch(struct vidow_server *server, const char *event,
		const char *json);

/* IPC {"cmd":"hooks"} 的载荷。 */
void vidow_hooks_json(struct jbuf *b, struct vidow_server *server);

/* 全部事件名（IPC {"cmd":"events"}、README、测试用）。返回个数，名字写进 *out。 */
size_t vidow_event_names(const char *const **out);
/* 这个名字是个事件吗（自定义事件 —— IPC 的 emit 命令 —— 不在名单里）。 */
bool vidow_event_known(const char *event);

#endif /* VIDOW_HOOKS_H */
