/* 会话锁 ext-session-lock-v1（协议由 wlroots 0.20 自带，vidow 只接上）。
 *
 * 锁上以后做三件事：
 *   1. 把整棵「正常」场景树关掉（layer_backdrop … layer_im），开 layer_lock_blank
 *      （每块屏一块不透明黑）+ layer_lock（lock surface，建在 layer_im 之后 = 最顶）。
 *      协议 XML:77-82 要求锁定期间只渲染 lock surface，所以这不是「可选的美化」。
 *   2. 客户端为某块屏建 lock surface 时：挂进 layer_lock，并**立刻主动 configure**
 *      输出尺寸 —— wlroots 不替我们发首个 configure，不发客户端就永远不 ack /
 *      commit（协议要求 commit 前必须先 ack 首个 configure），锁定握手直接死锁。
 *   3. 等每块屏各呈现一帧后发 locked（XML:87-107）。某块屏一直不出帧就超时兜底
 *      （XML:96-98 的 "reasonable time limit"），不能无限等。
 *
 * 本文件是 S1：只做「能锁、能黑屏、能握手」。解锁 / 客户端崩溃恢复（S4）、
 * 输入路由（S3）、输出热插拔完善（S2）都不在这里。 */

#include "server.h"
#include "sessionlock.h"

#include <stdlib.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/util/log.h>

/* 等「每块屏一帧」的上限。headless / DRM 正常都是一两帧（16~33ms）内的事，500ms
 * 已经是「这块屏的帧链断了」的量级；再等下去客户端就一直看不到锁屏界面。 */
#define LOCK_FRAME_TIMEOUT_MS 500

/* 锁定期间要关掉的那些「正常」树（顺序同 main.c 的创建顺序）。
 * layer_lock_blank / layer_lock 反过来：只在锁定期间开。 */
static void set_desktop_enabled(struct vidow_server *server, bool enabled) {
	struct wlr_scene_tree *trees[] = {
		server->layer_backdrop,
		server->layer_background,
		server->layer_overview_dim,
		server->layer_bottom,
		server->layer_toplevels,
		server->layer_popups,
		server->layer_top,
		server->layer_overlay,
		server->layer_im,
	};
	for (size_t i = 0; i < sizeof(trees) / sizeof(trees[0]); i++) {
		if (trees[i] != NULL) {
			wlr_scene_node_set_enabled(&trees[i]->node, enabled);
		}
	}
}

static void lock_send_locked(struct vidow_server *server) {
	if (server->session_lock == NULL || server->lock_locked_sent) {
		return;
	}
	server->lock_locked_sent = true;
	if (server->lock_timeout != NULL) {
		wl_event_source_timer_update(server->lock_timeout, 0);
	}
	wlr_session_lock_v1_send_locked(server->session_lock);
	wlr_log(WLR_INFO, "sessionlock: locked 已发（还有 %u 块屏没呈现锁后第一帧）",
		server->lock_frames_pending);
}

/* 超时兜底：还有屏没呈现锁后第一帧（后端不给帧 / 帧链断了），不能无限等下去。 */
static int lock_timeout_cb(void *data) {
	struct vidow_server *server = data;
	if (!server->lock_locked_sent) {
		wlr_log(WLR_ERROR, "sessionlock: 等了 %dms 还有 %u 块屏没呈现锁后第一帧，"
			"按超时兜底发 locked（那块屏可能还是黑的）",
			LOCK_FRAME_TIMEOUT_MS, server->lock_frames_pending);
		lock_send_locked(server);
	}
	return 0;
}

void vidow_sessionlock_frame(struct vidow_output *output) {
	struct vidow_server *server = output->server;
	if (!server->locked || !output->lock_frame_pending) {
		return;
	}
	output->lock_frame_pending = false;
	if (server->lock_frames_pending > 0) {
		server->lock_frames_pending--;
	}
	if (server->lock_frames_pending == 0) {
		lock_send_locked(server);
	}
}

void vidow_sessionlock_output_update(struct vidow_output *output) {
	if (output->lock_blank == NULL) {
		return;
	}
	if (output->box.width <= 0 || output->box.height <= 0) {
		return;
	}
	wlr_scene_rect_set_size(output->lock_blank, output->box.width,
		output->box.height);
	wlr_scene_node_set_position(&output->lock_blank->node, output->box.x,
		output->box.y);
}

/* ---------------------------------------------------------- lock surface */

static void lock_surface_handle_destroy(struct wl_listener *listener,
		void *data) {
	(void)data;
	struct vidow_lock_surface *ls =
		wl_container_of(listener, ls, destroy);
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->link);
	if (ls->scene_surface != NULL) {
		wlr_scene_node_destroy(&ls->scene_surface->buffer->node);
	}
	free(ls);
}

static void lock_handle_new_surface(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, lock_new_surface);
	struct wlr_session_lock_surface_v1 *lock_surface = data;

	struct vidow_output *output =
		vidow_output_lookup(server, lock_surface->output);
	if (output == NULL) {
		wlr_log(WLR_ERROR, "sessionlock: lock surface 对应的输出不认识了，"
			"不 configure（客户端会一直等）");
		return;
	}

	struct vidow_lock_surface *ls = calloc(1, sizeof(*ls));
	if (ls == NULL) {
		return;
	}
	ls->output = output;
	ls->lock_surface = lock_surface;
	ls->scene_surface = wlr_scene_surface_create(server->layer_lock,
		lock_surface->surface);
	if (ls->scene_surface == NULL) {
		free(ls);
		return;
	}
	/* 场景树是全屏坐标：把这块屏的 lock surface 摆到这块屏的布局原点上。 */
	wlr_scene_node_set_position(&ls->scene_surface->buffer->node,
		output->box.x, output->box.y);
	ls->destroy.notify = lock_surface_handle_destroy;
	wl_signal_add(&lock_surface->events.destroy, &ls->destroy);
	wl_list_insert(&server->lock_surfaces, &ls->link);

	/* 首个 configure 必须我们自己发（见文件头第 2 条）。 */
	wlr_session_lock_surface_v1_configure(lock_surface,
		(uint32_t)output->box.width, (uint32_t)output->box.height);
	wlr_log(WLR_INFO, "sessionlock: %s 的 lock surface 来了，已 configure %dx%d",
		output->wlr_output->name, output->box.width, output->box.height);
}

/* ------------------------------------------------------------------ lock */

static void lock_handle_unlock(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_server *server =
		wl_container_of(listener, server, lock_unlock);
	/* 解锁是 S4 的事：这里只记一笔。接下来客户端会销毁 lock（destroy 事件）。 */
	wlr_log(WLR_INFO, "sessionlock: 客户端请求解锁（恢复桌面是 S4）");
}

static void lock_handle_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_server *server =
		wl_container_of(listener, server, lock_destroy);
	wl_list_remove(&server->lock_destroy.link);
	wl_list_remove(&server->lock_unlock.link);
	wl_list_remove(&server->lock_new_surface.link);
	if (server->lock_timeout != NULL) {
		wl_event_source_timer_update(server->lock_timeout, 0);
	}
	server->session_lock = NULL;
	/* 客户端走了（或解锁了）：协议 XML:111-113 不许合成器自己解锁 —— 保持黑屏 +
	 * 锁定态（「死锁」态），恢复桌面是 S4。 */
	wlr_log(WLR_INFO, "sessionlock: lock 对象没了（客户端退出 / 解锁）；"
		"按协议保持锁定态（恢复桌面见 S4）");
}

static void lock_manager_new_lock(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, new_lock);
	struct wlr_session_lock_v1 *lock = data;

	if (server->session_lock != NULL) {
		/* 已经锁着了：后来这把直接 finished 掉（协议允许合成器拒绝）。 */
		wlr_log(WLR_ERROR, "sessionlock: 已经有一把锁了，拒掉后来这把");
		wlr_session_lock_v1_destroy(lock);
		return;
	}

	server->session_lock = lock;
	server->lock_locked_sent = false;
	server->locked = true;

	server->lock_new_surface.notify = lock_handle_new_surface;
	wl_signal_add(&lock->events.new_surface, &server->lock_new_surface);
	server->lock_unlock.notify = lock_handle_unlock;
	wl_signal_add(&lock->events.unlock, &server->lock_unlock);
	server->lock_destroy.notify = lock_handle_destroy;
	wl_signal_add(&lock->events.destroy, &server->lock_destroy);

	/* 桌面整棵关掉，黑幕开起来。lock surface 由客户端随后建（new_surface）。 */
	set_desktop_enabled(server, false);
	if (server->layer_lock_blank != NULL) {
		wlr_scene_node_set_enabled(&server->layer_lock_blank->node, true);
	}
	if (server->layer_lock != NULL) {
		wlr_scene_node_set_enabled(&server->layer_lock->node, true);
	}

	/* 数「每块屏各呈现一帧」：现有屏全部记账，等帧回调一个个销账。 */
	server->lock_frames_pending = 0;
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		output->lock_frame_pending = true;
		server->lock_frames_pending++;
	}
	if (server->lock_frames_pending == 0) {
		/* 一块屏都没有：没东西可等，直接发。 */
		lock_send_locked(server);
	} else if (server->lock_timeout != NULL) {
		wl_event_source_timer_update(server->lock_timeout, LOCK_FRAME_TIMEOUT_MS);
	}
	/* 主动要一帧：场景刚被关掉、黑幕刚开，damage 是有的，但别赌后端一定给帧。 */
	wl_list_for_each(output, &server->outputs, link) {
		vidow_output_schedule_frame(server, output->wlr_output, "锁屏握手");
	}
	wlr_log(WLR_INFO, "sessionlock: 锁上了（%u 块屏），等各屏一帧后发 locked",
		server->lock_frames_pending);
}

bool vidow_sessionlock_init(struct vidow_server *server) {
	wl_list_init(&server->lock_surfaces);
	server->session_lock_mgr = wlr_session_lock_manager_v1_create(server->display);
	if (server->session_lock_mgr == NULL) {
		wlr_log(WLR_ERROR, "sessionlock: wlr_session_lock_manager_v1_create 失败");
		return false;
	}
	server->new_lock.notify = lock_manager_new_lock;
	wl_signal_add(&server->session_lock_mgr->events.new_lock, &server->new_lock);
	server->lock_timeout = wl_event_loop_add_timer(
		wl_display_get_event_loop(server->display), lock_timeout_cb, server);
	if (server->lock_timeout == NULL) {
		wlr_log(WLR_ERROR, "sessionlock: 超时兜底的定时器建不出来");
		return false;
	}
	return true;
}
