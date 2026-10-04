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

/* 这个 mime 类型是不是文本（IPC 决定报 text 还是只报 base64）。 */
bool vidow_clipboard_mime_is_text(const char *mime);

/* 这个 X 窗口是不是剪贴板桥自己的（它永远不映射，但 xwm 会给它建一个 surface，
 * 别当成客户端窗口收进视图模型 —— 否则会白占一个布局位置）。 */
bool vidow_clipboard_owns_xwindow(struct vidow_server *server,
	uint32_t window);

#endif
