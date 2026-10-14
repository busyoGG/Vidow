#ifndef VIDOW_SESSIONLOCK_H
#define VIDOW_SESSIONLOCK_H

#include <stdbool.h>

#include <wayland-server-core.h>

/* 会话锁 ext-session-lock-v1（实现见 sessionlock.c）。
 *
 * 锁是**会话级状态**，不是窗口、不进 server->views、不占列、不参与焦点/MRU/
 * 工作区 —— 它作用在场景图（把整棵正常树关掉）和输入路由（S3）上，在布局之上。
 * 所以 view.c 的列逻辑一行都不用改（方案 §2.2）。 */

struct vidow_server;
struct vidow_output;
struct wlr_session_lock_surface_v1;
struct wlr_scene_surface;

/* 客户端为某块屏建的一块锁屏画面。生命周期完全跟着客户端的 surface：它一没，
 * 这个结构就没（见 sessionlock.c 的 lock_surface_handle_destroy）。 */
struct vidow_lock_surface {
	struct wl_list link; /* vidow_server.lock_surfaces */
	struct vidow_output *output;
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wlr_scene_surface *scene_surface;
	struct wl_listener destroy;
};

/* 建 manager global 并听 new_lock（main.c 的 init 链里调一次）。 */
bool vidow_sessionlock_init(struct vidow_server *server);

/* 这块屏的黑幕 rect 对齐到它现在的布局框（建输出时调，位置/尺寸变了也调）。
 * 锁定期间每块屏一块不透明黑，垫在 lock surface 底下。 */
void vidow_sessionlock_output_update(struct vidow_output *output);

/* 这块屏呈现过一帧了（output.c 的帧回调调）：锁定握手靠它数「每块屏都画过一帧
 * 了没」，数齐了才发 locked（协议 XML:87-107）。 */
void vidow_sessionlock_frame(struct vidow_output *output);

#endif
