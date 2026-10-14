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
 * 本文件到 S3 为止：能锁、能黑屏、能握手，lock surface 生命周期（建 / 输出尺寸变化
 * 重新 configure / 输出拔掉清账）和输出热插拔（新屏立刻补黑幕、进握手账）都做了，
 * 输入路由（键盘 / 指针只给 lock surface，见本文件末尾）也做了。解锁 / 客户端崩溃
 * 恢复（S4）不在这里。 */

#include "server.h"
#include "sessionlock.h"

#include <stdlib.h>

#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
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

/* 把这块屏的黑 rect 和它上面的 lock surface 对齐到一个布局框（逻辑坐标）。
 * 尺寸变了就重新 configure lock surface —— 客户端只认最后一次 configure 的尺寸
 * （协议要求 commit 的尺寸等于最后一次 ack 的 configure），不重发它就永远按旧尺寸画。
 * box 从外面传进来：output->box 是渲染时每帧刷新的，输出刚 commit 完那一刻它还是旧的
 * （见 vidow_sessionlock_output_sync）。 */
static void lock_output_apply_box(struct vidow_output *output,
		const struct wlr_box *box) {
	if (box->width <= 0 || box->height <= 0) {
		return;
	}
	if (output->lock_blank != NULL) {
		wlr_scene_rect_set_size(output->lock_blank, box->width, box->height);
		wlr_scene_node_set_position(&output->lock_blank->node, box->x, box->y);
	}
	struct vidow_lock_surface *ls;
	wl_list_for_each(ls, &output->server->lock_surfaces, link) {
		if (ls->output != output) {
			continue;
		}
		wlr_scene_node_set_position(&ls->scene_surface->buffer->node,
			box->x, box->y);
		if (ls->cfg_w == box->width && ls->cfg_h == box->height) {
			continue;
		}
		/* 首次 configure 由 new_surface 那边打日志（它才说得清「谁的 surface
		 * 来了」）；这里只管「尺寸变了，重发一遍」。 */
		bool first = ls->cfg_w == 0 && ls->cfg_h == 0;
		ls->cfg_w = box->width;
		ls->cfg_h = box->height;
		wlr_session_lock_surface_v1_configure(ls->lock_surface,
			(uint32_t)box->width, (uint32_t)box->height);
		if (!first) {
			wlr_log(WLR_INFO, "sessionlock: %s 现在是 %dx%d，"
				"lock surface 重新 configure 了", output->wlr_output->name,
				box->width, box->height);
		}
	}
}

void vidow_sessionlock_output_update(struct vidow_output *output) {
	lock_output_apply_box(output, &output->box);
}

void vidow_sessionlock_output_sync(struct vidow_output *output) {
	struct vidow_server *server = output->server;
	if (output->lock_blank == NULL) {
		return;
	}
	/* 直接问布局要当前的框：output->box 要等下一帧渲染才刷新。 */
	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
	if (box.width == output->box.width && box.height == output->box.height &&
			box.x == output->box.x && box.y == output->box.y) {
		/* 没变（绝大多数 commit 都是这种）：什么都不做，别打扰客户端。 */
		return;
	}
	wlr_log(WLR_INFO, "sessionlock: %s 提交了新状态（%dx%d @ %d,%d），"
		"黑 rect / lock surface 跟着对齐", output->wlr_output->name,
		box.width, box.height, box.x, box.y);
	lock_output_apply_box(output, &box);
}

void vidow_sessionlock_output_added(struct vidow_output *output) {
	struct vidow_server *server = output->server;
	if (!server->locked) {
		return;
	}
	/* 黑 rect 是 output.c 在 server_new_output 里刚建的（挂在 layer_lock_blank
	 * 底下，那棵树锁定期间开着）—— 所以新屏从第一帧起就是黑的，桌面漏不出去。
	 * 这里只记账 + 打一行日志，测试拿它当「新屏补了黑幕」的判据。 */
	if (output->lock_blank == NULL) {
		wlr_log(WLR_ERROR, "sessionlock: 锁定期间新屏 %s 没拿到黑 rect"
			"（layer_lock_blank 没建出来？）", output->wlr_output->name);
		return;
	}
	wlr_log(WLR_INFO, "sessionlock: 锁定期间新屏 %s 到了，黑 rect 已就位"
		"（%dx%d @ %d,%d）", output->wlr_output->name, output->box.width,
		output->box.height, output->box.x, output->box.y);
	/* locked 还没发的话，这块屏的帧也要进握手账 —— 它可能一帧都还没画过，
	 * 不等它就 send_locked 等于把「可能还有桌面残留」的屏放行了。 */
	if (!server->lock_locked_sent) {
		if (!output->lock_frame_pending) {
			output->lock_frame_pending = true;
			server->lock_frames_pending++;
		}
		vidow_output_schedule_frame(server, output->wlr_output, "锁定期间新屏");
	}
}

void vidow_sessionlock_output_removed(struct vidow_output *output) {
	struct vidow_server *server = output->server;
	/* 这块屏的 lock surface 记账收干净：wlroots 随后也会销毁这些 lock surface
	 * （它挂在同一个 output->events.destroy 上，比我们晚注册），但那条路上
	 * ls->output 已经指着要释放的 vidow_output 了。这里先收：摘掉自己的 destroy
	 * 监听再释放，两边谁先跑都不会二次回调。 */
	unsigned cleaned = 0;
	struct vidow_lock_surface *ls, *tmp;
	wl_list_for_each_safe(ls, tmp, &server->lock_surfaces, link) {
		if (ls->output != output) {
			continue;
		}
		wl_list_remove(&ls->destroy.link);
		wl_list_remove(&ls->link);
		if (ls->scene_surface != NULL) {
			wlr_scene_node_destroy(&ls->scene_surface->buffer->node);
		}
		free(ls);
		cleaned++;
	}
	/* 这块屏永远画不出「锁后第一帧」了：销掉它的账，别让 locked 一直等着。 */
	bool was_pending = output->lock_frame_pending;
	if (was_pending) {
		output->lock_frame_pending = false;
		if (server->lock_frames_pending > 0) {
			server->lock_frames_pending--;
		}
	}
	if (server->locked) {
		wlr_log(WLR_INFO, "sessionlock: 输出 %s 拔了，清掉 %u 块 lock surface 记账%s"
			"（还有 %u 块屏没呈现锁后第一帧）", output->wlr_output->name, cleaned,
			was_pending ? " + 一块待呈现的账" : "", server->lock_frames_pending);
	}
	if (was_pending && server->lock_frames_pending == 0) {
		lock_send_locked(server);
	}
}

/* --------------------------------------------------------------- 输入路由
 *
 * 锁定期间键盘和指针**只能**给 lock surface（协议 XML:77-82 / 246-250，方案 §2
 * 环节 3）。策略取「光标所在那块屏的 lock surface」：光标不在任何屏上、或那块屏
 * 还没有 lock surface 时退回到第一块有 lock surface 的屏 —— 一块都不剩就返回
 * NULL，调用方（input.c）把事件吞掉，绝不漏给被藏起来的窗口。
 *
 * 这里只算「给谁、局部坐标多少」，seat 的 notify_* 由 input.c 发。 */

/* 锁定期间光标所在那块屏的 lock surface；顺便把这块屏的布局框带回 *box。 */
static struct vidow_lock_surface *lock_surface_at_cursor(
		struct vidow_server *server, struct wlr_box *box) {
	struct wlr_output *under = NULL;
	if (server->cursor != NULL && server->output_layout != NULL) {
		under = wlr_output_layout_output_at(server->output_layout,
			server->cursor->x, server->cursor->y);
	}
	struct vidow_lock_surface *ls, *fallback = NULL;
	wl_list_for_each(ls, &server->lock_surfaces, link) {
		if (fallback == NULL) {
			fallback = ls;
		}
		if (under != NULL && ls->output != NULL &&
				ls->output->wlr_output == under) {
			fallback = ls;
			break;
		}
	}
	if (fallback == NULL || fallback->output == NULL) {
		return NULL;
	}
	if (box != NULL) {
		box->x = box->y = 0;
		box->width = box->height = 0;
		wlr_output_layout_get_box(server->output_layout,
			fallback->output->wlr_output, box);
	}
	return fallback;
}

struct wlr_surface *vidow_sessionlock_keyboard_surface(
		struct vidow_server *server) {
	struct vidow_lock_surface *ls = lock_surface_at_cursor(server, NULL);
	return ls != NULL ? ls->lock_surface->surface : NULL;
}

struct wlr_surface *vidow_sessionlock_pointer_surface(
		struct vidow_server *server, double *sx, double *sy) {
	struct wlr_box box = {0};
	struct vidow_lock_surface *ls = lock_surface_at_cursor(server, &box);
	if (ls == NULL) {
		return NULL;
	}
	if (sx != NULL) {
		*sx = server->cursor->x - box.x;
	}
	if (sy != NULL) {
		*sy = server->cursor->y - box.y;
	}
	return ls->lock_surface->surface;
}

void vidow_sessionlock_focus(struct vidow_server *server) {
	struct wlr_surface *target = vidow_sessionlock_keyboard_surface(server);
	if (target == NULL ||
			server->seat->keyboard_state.focused_surface == target) {
		return;
	}
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	/* keycodes 传 NULL：锁屏界面不需要知道「锁之前按着哪些键」，也不该继承
	 * 那份状态（被藏起来的窗口的按键状态不该泄给 lock 客户端）。 */
	wlr_seat_keyboard_notify_enter(server->seat, target, NULL, 0,
		kb != NULL ? &kb->modifiers : NULL);
}

/* 锁定瞬间把还在进行的输入手势收掉：拖拽 / 概览操作的都是马上要被藏起来的东西，
 * 锁上以后它们既收不到输入（各入口的守卫会把入口短路）也没法自己收尾 —— 留着一个
 * 「半截的拖拽」到解锁之后才发作，比现在就收掉糟得多。
 *
 * 手势（gesture.c）**不在这里清**：vidow_gesture_clear() 清的是配置表（config.lua
 * 重载时用的那份），在这儿调等于把用户绑的手势全删了。正在跑的那个手势由它自己的
 * end 事件收尾（begin / update 已经被守卫挡住，锁上以后开不了新的）。 */
static void lock_cancel_input_gestures(struct vidow_server *server) {
	if (server->grab_mode == VIDOW_GRAB_TILE) {
		vidow_tile_grab_end(server);
	} else if (server->grab_mode == VIDOW_GRAB_RESIZE_TILE) {
		vidow_tile_resize_end(server);
	} else if (server->grab_mode != VIDOW_GRAB_NONE) {
		vidow_float_end_grab(server);
	}
	vidow_overview_exit(server);
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
	ls->destroy.notify = lock_surface_handle_destroy;
	wl_signal_add(&lock_surface->events.destroy, &ls->destroy);
	wl_list_insert(&server->lock_surfaces, &ls->link);

	/* 首个 configure 必须我们自己发（见文件头第 2 条）。 */
	lock_output_apply_box(output, &output->box);
	/* 键盘焦点也跟着挪过来：光标停在这块屏上时，锁屏界面该当场能收键盘
	 * （不挪的话键盘要等到下一次指针 / 键盘事件才找到它）。 */
	vidow_sessionlock_focus(server);
	wlr_log(WLR_INFO, "sessionlock: %s 的 lock surface 来了，已 configure %dx%d",
		output->wlr_output->name, ls->cfg_w, ls->cfg_h);
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

	/* 还在进行的拖拽 / 手势 / 概览先收掉（见 lock_cancel_input_gestures）。 */
	lock_cancel_input_gestures(server);

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
