/* capturemask.c — 抓帧遮挡：窗口规则 capture = "hide" 让一个窗口 / layer surface
 * 在别人截图、录屏的时候不出现。
 *
 * 怎么藏的：**把透明度压成 0**，不是把场景节点关掉。
 *   关节点（wlr_scene_node_set_enabled）也能让它不进画面，但场景的命中测试会跳过
 *   关掉的节点 —— 窗口就点不着了（vidow 的输入焦点和 vidowmsg view-at 都走
 *   wlr_scene_node_at）。透明度只影响合成，命中测试不看它，所以压成 0 既不出现在
 *   抓帧里，又照常能点、能输入。
 *
 * 什么时候藏：**只在渲染抓帧那一瞬间**（引用计数：vidow_capturemask_hold /
 *   _release）。vidow 自己出帧（capture.c 的输出抓帧源、screencopy.c）时压上，
 *   渲染完就放开 —— 屏幕那一帧完全不受影响，藏着的东西屏幕上照常显示。
 *   这条路上不需要「抓帧检测」：抓帧的渲染是 vidow 自己发起的，它自己知道。
 *
 * 唯一的例外：ext-image-copy-capture 的**窗口**抓帧源是 wlroots 那份（直接渲染
 * 窗口的场景子树），vidow 接不进去。有人在抓一个 capture = "hide" 的窗口时，就
 * 只能整段时间把它从屏幕上藏起来（会话开始压、结束放）—— 那条路上「录屏的时候
 * 窗口也从屏幕上消失」的取舍还在，见 capture_new_session。
 *
 * 已知漏点：
 *   - zwlr_export_dmabuf_manager_v1 导出的是**屏幕那一帧**，藏不掉（老协议）。
 *   - 窗口自己弹的 popup（菜单 / 下拉框）是另一个场景节点，不在遮挡范围内。
 */
#include "server.h"

#include <stdlib.h>

#include <wlr/util/log.h>

/* 规则说了要在抓帧里「处理掉」的（藏起来 / 打马赛克）都要先把节点压下去：
 * 藏起来就完事，马赛克则在抓帧渲染完再把块状版本画上去（见 capture.c 的
 * vidow_capture_paint_mosaic）。 */
static bool wants_mask(int capture_mode) {
	return capture_mode == VIDOW_CAPTURE_HIDE ||
		capture_mode == VIDOW_CAPTURE_MOSAIC;
}

/* ------------------------------------------------------- 会话（ext-copy） */

static void mask_apply(struct vidow_server *server, bool off);

/* 压着的层数（抓帧渲染期间压着，可能嵌套：一边录屏一边截图）。0 就是没压。 */
static int mask_holds;
static bool mask_on;

static void capture_session_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_capture_session *s = wl_container_of(listener, s, destroy);
	struct vidow_server *server = s->server;
	wl_list_remove(&s->destroy.link);
	wl_list_remove(&s->link);
	free(s);
	vidow_capturemask_release(server);
}

static void capture_new_session(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, new_capture_session);
	struct wlr_ext_image_copy_capture_session_v1 *session = data;
	/* vidow 自己出帧的会话（输出抓帧源）不用管：它的帧是我们自己渲染的，
	 * 渲染期间自己压遮挡，屏幕那一帧不受影响。只有 wlroots 那份窗口抓帧源
	 * 才需要「整段时间从屏幕上藏起来」。 */
	if (vidow_capture_source_is_ours(session->source)) {
		return;
	}
	struct vidow_capture_session *s = calloc(1, sizeof(*s));
	if (s == NULL) {
		return;
	}
	s->destroy.notify = capture_session_destroy;
	s->server = server;
	wl_signal_add(&session->events.destroy, &s->destroy);
	wl_list_insert(&server->capture_sessions, &s->link);
	wlr_log(WLR_DEBUG, "抓帧遮挡: 有人在抓窗口，从现在起藏着");
	/* 客户端建完会话紧接着就 attach buffer + capture，而 capture 是同步渲染的
	 * —— 晚一步那一张里窗口还在。所以会话一建立就压。 */
	vidow_capturemask_hold(server);
}

void vidow_capturemask_init(struct vidow_server *server) {
	wl_list_init(&server->capture_sessions);
	if (server->ext_capture == NULL) {
		return;
	}
	server->new_capture_session.notify = capture_new_session;
	wl_signal_add(&server->ext_capture->events.new_session,
		&server->new_capture_session);
}

void vidow_capturemask_finish(struct vidow_server *server) {
	if (server->new_capture_session.notify == NULL) {
		return;
	}
	/* 这个钩子必须自己摘掉：wlroots 在 display 销毁时有一条
	 * `assert(wl_list_empty(&manager->events.new_session.listener_list))`
	 * （types/wlr_ext_image_copy_capture_v1.c），而那个断言就发生在
	 * wl_display_destroy() 里面、而且是**先于**我们挂上去的任何 display
	 * 监听器跑的 —— 光挂个 display_destroy 回调来不及。所以退出前显式摘。
	 * （不摘的话：每次 quit 都在收尾最后一步 SIGABRT，核心转储 + 退出码 134。） */
	wl_list_remove(&server->new_capture_session.link);
	server->new_capture_session.notify = NULL;
}

/* --------------------------------------------------------------- 藏 / 放 */

/* 把一个 buffer 的透明度压成 0（on = true）/ 还原（false）。saved 是压之前的值。 */
static void opacity_mask(struct wlr_scene_buffer *buf, float *saved, bool on) {
	if (buf == NULL) {
		return;
	}
	if (on) {
		*saved = buf->opacity;
		if (buf->opacity != 0.0f) {
			wlr_scene_buffer_set_opacity(buf, 0.0f);
		}
	} else if (buf->opacity == 0.0f && *saved != 0.0f) {
		wlr_scene_buffer_set_opacity(buf, *saved);
	}
}

/* 找一棵（子）树里第一个真正画 surface 的 buffer 节点。layer 的内容节点不是
 * ls->scene->tree 的直接子节点 —— wlroots 把 layer surface 包在
 * wlr_scene_subsurface_tree_create() 建的一棵树里（见 types/scene/layer_shell_v1.c），
 * 所以要往下递归找。树里还有 vidow 自己的 fx 节点（模糊底衬），那不是 surface。 */
static struct wlr_scene_buffer *find_surface_buffer(struct wlr_scene_node *node) {
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
		return wlr_scene_surface_try_from_buffer(sb) != NULL ? sb : NULL;
	}
	if (node->type != WLR_SCENE_NODE_TREE) {
		return NULL;
	}
	struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
	struct wlr_scene_node *child;
	wl_list_for_each(child, &tree->children, link) {
		struct wlr_scene_buffer *sb = find_surface_buffer(child);
		if (sb != NULL) {
			return sb;
		}
	}
	return NULL;
}

struct wlr_scene_buffer *vidow_layer_surface_buffer(
		struct wlr_scene_layer_surface_v1 *scene) {
	if (scene == NULL || scene->tree == NULL) {
		return NULL;
	}
	return find_surface_buffer(&scene->tree->node);
}

/* 一个窗口 / layer 上「画在屏幕上的那些节点」：装饰过的画在 fx 节点上（表面自己
 * 是透明的），没装饰的画在表面上 —— 两个都压，哪种都不漏。 */
static void mask_apply(struct vidow_server *server, bool off) {
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (!wants_mask(view->capture_mode)) {
			continue;
		}
		/* 标志位先立起来：vidow_fx_frame 每帧都会把节点透明度写回 1.0，只戳一下
		 * 的话，抓帧在两帧之间渲染时看到的还是亮的。 */
		view->fx.masked = off;
		view->mask_opacity = off;
		opacity_mask(view->fx.node, &view->mask_saved_fx, off);
		for (struct vidow_blur_slot *sl = view->fx.blur_slot; sl; sl = sl->next) {
			opacity_mask(sl->node, &sl->mask_saved_opacity, off);
		}
		opacity_mask(view->scene_surface != NULL ?
			view->scene_surface->buffer : NULL, &view->mask_saved_surf,
			off);
		/* 拖着果冻的时候，画在屏幕上的既不是表面也不是装饰节点，而是果冻那几
		 * 个节点（每块屏一个；内容一样，只是弯了）：它们也得压，不然「截图里
		 * 不出现」的窗口一拖就露出来了。 */
		vidow_jelly_set_opacity(view, off ? 0.0f : 1.0f);
		view->mask_opacity = off;
	}
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		if (!wants_mask(ls->capture_mode)) {
			continue;
		}
		ls->fx.masked = off;
		ls->mask_opacity = off;
		opacity_mask(ls->fx.node, &ls->mask_saved_fx, off);
		opacity_mask(vidow_layer_surface_buffer(ls->scene), &ls->mask_saved_surf,
			off);
		ls->mask_opacity = off;
	}
}

/* 这块屏上有没有规则说了要藏的窗口 / layer（没有就不用白走一趟）。 */
static bool anything_maskable(struct vidow_server *server) {
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (wants_mask(view->capture_mode)) {
			return true;
		}
	}
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		if (wants_mask(ls->capture_mode)) {
			return true;
		}
	}
	return false;
}

/* 真的压 / 放。幂等（已经压过的节点再压一次什么都不做），所以每次抓帧都整表走
 * 一遍：抓帧渲染期间新出现的窗口也会被压上。只在状态真的变了的时候记日志。 */
static void mask_update(struct vidow_server *server, bool hide) {
	if (!anything_maskable(server)) {
		return;
	}
	if (hide != mask_on) {
		mask_on = hide;
		wlr_log(WLR_DEBUG, "抓帧遮挡: 规则说了要藏的东西 %s",
			hide ? "从抓帧里拿掉（屏幕上照常显示）" : "回到画面里");
	}
	mask_apply(server, hide);
}

void vidow_capturemask_hold(struct vidow_server *server) {
	mask_holds++;
	mask_update(server, true);
}

void vidow_capturemask_release(struct vidow_server *server) {
	if (mask_holds > 0) {
		mask_holds--;
	}
	mask_update(server, mask_holds > 0);
}
