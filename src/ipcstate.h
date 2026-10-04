/* IPC 的「状态视图」：把合成器内部的东西序列化成 JSON，以及把变化推给订阅者。
 * 传输 / 协议 / 命令在 ipc.c，这里只负责「内部状态 -> JSON」这一半。 */
#ifndef VIDOW_IPCSTATE_H
#define VIDOW_IPCSTATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "json.h"

struct vidow_server;
struct vidow_view;
struct vidow_clip_selection;
struct vidow_column;
struct vidow_layer_surface;
struct vidow_workspace;
struct vidow_output;
struct wlr_output;

/* 单个对象。view / ls 为 NULL 时写出 null（少一层判断）。 */
void vidow_ipc_json_window(struct jbuf *b, struct vidow_server *server,
		struct vidow_view *view);
void vidow_ipc_json_column(struct jbuf *b, struct vidow_server *server,
		struct vidow_column *col);
void vidow_ipc_json_workspace(struct jbuf *b, struct vidow_server *server,
		struct vidow_workspace *ws);
void vidow_ipc_json_output(struct jbuf *b, struct vidow_server *server,
		struct vidow_output *output);
void vidow_ipc_json_layer(struct jbuf *b, struct vidow_server *server,
		struct vidow_layer_surface *ls);
void vidow_ipc_json_config(struct jbuf *b, struct vidow_server *server);
void vidow_ipc_json_keybinds(struct jbuf *b, struct vidow_server *server);
void vidow_ipc_json_rules(struct jbuf *b, struct vidow_server *server);
void vidow_ipc_json_seats(struct jbuf *b, struct vidow_server *server);
void vidow_ipc_json_input(struct jbuf *b, struct vidow_server *server);
void vidow_ipc_json_tokens(struct jbuf *b, struct vidow_server *server);
void vidow_ipc_json_memories(struct jbuf *b, struct vidow_server *server);
void vidow_ipc_json_anims(struct jbuf *b, struct vidow_server *server);
void vidow_ipc_json_im(struct jbuf *b, struct vidow_server *server);
void vidow_ipc_json_overview(struct jbuf *b, struct vidow_server *server);
void vidow_ipc_json_xwayland(struct jbuf *b, struct vidow_server *server);
/* 剪贴板：一份选择（CLIPBOARD / PRIMARY）的完整状态。what = 为什么发的这次
 * 事件（x11 / wayland-set / clear / ...），IPC 里能看出来是谁在动。 */
void vidow_ipc_json_clipboard(struct jbuf *b, struct vidow_server *server,
		struct vidow_clip_selection *sel, const char *what);
void vidow_ipc_json_windows(struct jbuf *b, struct vidow_server *server);
void vidow_ipc_json_workspaces(struct jbuf *b, struct vidow_server *server);
void vidow_ipc_json_outputs(struct jbuf *b, struct vidow_server *server);
void vidow_ipc_json_layers(struct jbuf *b, struct vidow_server *server);
void vidow_ipc_json_columns(struct jbuf *b, struct vidow_server *server);
void vidow_ipc_json_focused(struct jbuf *b, struct vidow_server *server);
/* 全量：上面所有的东西 + 计数 + 版本。 */
void vidow_ipc_json_state(struct jbuf *b, struct vidow_server *server);

/* ipc.c 提供：把一整行（含结尾 '\n'）推给所有订阅者。event 用来对订阅时写的
 * 过滤器（订阅者只订了某几种事件时）。没有订阅者时立刻返回。 */
void vidow_ipc_broadcast(struct vidow_server *server, const char *event,
		const char *line, size_t len);

#endif /* VIDOW_IPCSTATE_H */
