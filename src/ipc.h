/* IPC：unix socket 上的 JSON 请求 / 应答 + 事件订阅（见 ipc.c）。
 *
 * 目的：把合成器内部的状态**尽可能完整**地吐出来（窗口 / 工作区 / 输出 / layer
 * surface / 键位 / 规则 / 配置 / 焦点 / 光标 / 激活令牌 / 动画 …），并且允许从外面
 * 操作其中大部分功能（焦点、工作区、浮动、全屏、显示器开关 / 缩放 / 模式 / 位置、
 * 运行时改配置、重载配置、退出…）。脚本和状态栏不必再去解析日志。
 *
 * 协议：一条连接上一行一个 JSON 对象（'\n' 结尾），请求一个、应答一个：
 *
 *   请求   {"cmd":"windows"}
 *   应答   {"ok":true,"result":[...]}
 *   失败   {"ok":false,"error":"..."}
 *
 * 订阅事件：请求 {"cmd":"subscribe"}，之后这条连接上来的全是事件行：
 *
 *   {"event":"window-opened","time_ms":1234,"data":{...}}
 *
 * 客户端工具是 tools/vidowmsg.c（vidowmsg windows / vidowmsg action "workspace 2"
 * / vidowmsg watch）。完整命令清单见 README 的 IPC 一节。 */
#ifndef VIDOW_IPC_H
#define VIDOW_IPC_H

#include <stdbool.h>
#include <stdint.h>

struct vidow_server;
struct vidow_view;
struct vidow_column;
struct vidow_clip_selection;
struct vidow_layer_surface;
struct vidow_workspace;
struct jval;
struct wlr_output;

/* 建 socket、开始听（配置里 ipc = false 时什么都不做）。失败只打日志：IPC 坏了
 * 不该拦住合成器起来。 */
bool vidow_ipc_init(struct vidow_server *server);
/* 配置落地后调：按 ipc / ipc_socket 建、拆或换 socket。配置是 Lua，跑得比 IPC
 * 初始化晚，所以这里得能对账（见 script.c 的 vidow_config_sync()）。 */
bool vidow_ipc_apply_config(struct vidow_server *server);
/* 关掉监听和所有连接（退出时调）。 */
void vidow_ipc_finish(struct vidow_server *server);
/* socket 路径（没起来的话是空串）。 */
const char *vidow_ipc_path(const struct vidow_server *server);

/* ------------------------------------------------------------- 事件 */

/* 有订阅者吗？事件发射前先问一句，省得白建 JSON。 */
bool vidow_ipc_wanted(const struct vidow_server *server);

/* 这个事件有人要吗 —— IPC 订阅者**或者**配置里挂的钩子（vidow.on，见 hooks.c）。
 * 事件发出来之前先问这个：没人要就别白建 JSON 了。 */
bool vidow_event_wanted(const struct vidow_server *server, const char *event);

/* 自定义事件：IPC 的 {"cmd":"emit","event":"名字","data":{…}}。名字由调用方定，
 * data 原样带上（NULL 也行）。脚本之间、脚本和配置钩子之间的一条自定义通道：
 *
 *   vidowmsg emit my-event --data '{"hello":"world"}'
 *   -- config.lua 里：vidow.on("my-event", "notify-send \"$VIDOW_HELLO\"")
 */
void vidow_ipc_emit_custom(struct vidow_server *server, const char *event,
		const struct jval *data);

/* 窗口事件：window-opened / window-title-changed / window-moved /
 * window-floating-changed / window-fullscreen-changed / window-workspace-changed
 * —— 载荷都是那个窗口的完整对象。 */
void vidow_ipc_emit_window(struct vidow_server *server, const char *event,
		struct vidow_view *view);
/* 窗口没了：这时候结构马上要释放，所以只带 id / 标题 / app_id。 */
void vidow_ipc_emit_window_closed(struct vidow_server *server, uint64_t id,
		const char *title, const char *app_id);
/* 键盘焦点换了（view 可以是 NULL = 焦点落空了）。 */
void vidow_ipc_emit_focus(struct vidow_server *server, struct vidow_view *view);

/* workspace-created / workspace-activated / workspace-special-changed */
void vidow_ipc_emit_workspace(struct vidow_server *server, const char *event,
		struct vidow_workspace *ws);
/* output-added / output-removed / output-changed（拔线时只带名字） */
void vidow_ipc_emit_output(struct vidow_server *server, const char *event,
		struct wlr_output *output);
/* layer-mapped / layer-unmapped / layer-closed */
void vidow_ipc_emit_layer(struct vidow_server *server, const char *event,
		struct vidow_layer_surface *ls);
/* column-maximize-changed：载荷是那条列的完整对象（列最大化的列宽会变成 1.0）。 */
void vidow_ipc_emit_column(struct vidow_server *server, const char *event,
		struct vidow_column *col);
/* clipboard-changed / primary-changed：剪贴板内容或归属变了。 */
void vidow_ipc_emit_clipboard(struct vidow_server *server,
		struct vidow_clip_selection *sel, const char *what);

/* 一条「只有几个标量字段」的事件（keybind / ipc-action / output-power-changed
 * 这类）。fields 以 VIDOW_EV_END 结尾；value = NULL 写成 null。 */
enum vidow_event_type {
	VIDOW_EV_TEXT,
	VIDOW_EV_NUM,
	VIDOW_EV_BOOL,
};
struct vidow_event_field {
	const char *key;
	enum vidow_event_type type;
	const char *text;   /* VIDOW_EV_TEXT */
	long long num;      /* VIDOW_EV_NUM */
	bool boolean;       /* VIDOW_EV_BOOL */
};
#define VIDOW_EV_STR(k, v)  { (k), VIDOW_EV_TEXT, (v), 0, false }
#define VIDOW_EV_NUM(k, v)  { (k), VIDOW_EV_NUM, NULL, (v), false }
#define VIDOW_EV_BOOL(k, v) { (k), VIDOW_EV_BOOL, NULL, 0, (v) }
#define VIDOW_EV_NULL(k)    { (k), VIDOW_EV_TEXT, NULL, 0, false }
#define VIDOW_EV_END        { NULL, VIDOW_EV_TEXT, NULL, 0, false }

void vidow_ipc_emit_simple(struct vidow_server *server, const char *event,
		const struct vidow_event_field *fields);

/* 没有载荷的事件：config-reloaded / overview-toggled */
void vidow_ipc_emit_note(struct vidow_server *server, const char *event);

#endif /* VIDOW_IPC_H */
