/* 剪贴板桥：X11(Xwayland) 与 Wayland 共用一份内容。
 *
 * 状态在 server.h 的 struct vidow_clipboard（跟别的模块一样，结构体都放那儿），
 * 这里只放接口。为什么要自己接管、接管了哪几条路、以及为什么不跟 wlroots 的
 * xwm 抢 X 侧 owner —— 见 server.h 里 struct vidow_clipboard 上面那段注释。
 *
 * 生命周期：
 *   vidow_clipboard_init()         —— 启动时（Wayland 侧：seat 监听 + 缓存）
 *   vidow_clipboard_x11_ready()    —— Xwayland 就绪后（X 侧：XFixes + 事件钩子）
 *   vidow_clipboard_apply_config() —— 配置变了（SIGHUP / IPC set-config）
 *   vidow_clipboard_finish()       —— 退出前
 */
#ifndef VIDOW_CLIPBOARD_H
#define VIDOW_CLIPBOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct vidow_server;

/* 下面 kind 参数用 server.h 里的 enum vidow_clip_kind：
 * VIDOW_CLIP_CLIPBOARD / VIDOW_CLIP_PRIMARY。 */

bool vidow_clipboard_init(struct vidow_server *server);
void vidow_clipboard_x11_ready(struct vidow_server *server);
void vidow_clipboard_apply_config(struct vidow_server *server);
void vidow_clipboard_finish(struct vidow_server *server);

/* IPC 用：取一份内容（挑最合适的类型，除非指定 mime）、塞一份内容、清空。
 * 取出来的 data 是 malloc 的，调用方 free；mime_out 指向内部字符串不用 free。 */
bool vidow_clipboard_get(struct vidow_server *server, int kind, const char *mime,
	unsigned char **data, size_t *len, const char **mime_out);
bool vidow_clipboard_set(struct vidow_server *server, int kind, const char *mime,
	const void *data, size_t len);
bool vidow_clipboard_clear(struct vidow_server *server, int kind);

/* ---- 历史（GUI / IPC 用；见 clipboard.c 的 history_push）----
 *
 * 桥本身只保一份「当前内容」，历史是另存的一串副本。条目结构见 server.h 的
 * struct vidow_clip_hist_entry，id 单调递增（下标会随新条目飘，指认条目用 id）。 */

struct vidow_clip_hist_entry;

/* 第 index 条（0 = 最新）；越界返回 NULL。 */
struct vidow_clip_hist_entry *vidow_clipboard_history_at(
	struct vidow_server *server, int index);
/* 把某条历史变成当前内容（Wayland 发布 + X11 应答都换成它）。 */
bool vidow_clipboard_history_use(struct vidow_server *server, uint64_t id);
bool vidow_clipboard_history_delete(struct vidow_server *server, uint64_t id);
void vidow_clipboard_history_clear(struct vidow_server *server);
/* 取一条历史里的内容（同 clipboard_get：挑最合适的类型，除非指定 mime）。 */
bool vidow_clipboard_history_get(struct vidow_server *server, uint64_t id,
	const char *mime, unsigned char **data, size_t *len,
	const char **mime_out);

/* 把一批文件路径放进剪贴板（text/uri-list + x-special/gnome-copied-files +
 * text/plain）：文件管理器粘出来是「复制这些文件」，终端粘出来是路径文本。 */
bool vidow_clipboard_set_files(struct vidow_server *server, int kind,
	char *const *paths, int npaths);

/* 这个 mime 类型是不是文本（IPC 决定报 text 还是只报 base64）。 */
bool vidow_clipboard_mime_is_text(const char *mime);

/* 这个 X 窗口是不是剪贴板桥自己的（它永远不映射，但 xwm 会给它建一个 surface，
 * 别当成客户端窗口收进视图模型 —— 否则会白占一个布局位置）。 */
bool vidow_clipboard_owns_xwindow(struct vidow_server *server,
	uint32_t window);

#endif
