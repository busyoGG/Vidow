#include "server.h"
#include "ipc.h"

#include <math.h>
#include <stdlib.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

static struct wlr_scene_tree *layer_tree(struct vidow_server *server,
		uint32_t layer) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		return server->layer_background;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		return server->layer_bottom;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		return server->layer_top;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
		return server->layer_overlay;
	default:
		return server->layer_top;
	}
}

/* Position every layer surface and recompute each output's usable area.
 * Called whenever the set of layer surfaces changes.
 *
 * 注意这里**不**打 backdrop 标记：这个函数在 layer 开关动画里是每帧调的
 * （layer_anim_update），而每打一次标记，所有窗口的模糊金字塔就要重建一遍
 * —— 重新抓一屏、重跑 4 级 kawase、再重新分配 7 个 GBM 缓冲。实测那样一帧
 * 一次（143 次/秒），动画直接不丝滑。真正改变底衬的地方自己打标记：layer
 * 出现 / 消失 / 换屏 / 内容重画 / 动画落定。 */
void vidow_arrange_layers(struct vidow_server *server) {
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_box full_area;
		wlr_output_layout_get_box(server->output_layout, output->wlr_output,
			&full_area);
		struct wlr_box usable_area = full_area;

		for (uint32_t layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
				layer <= ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; layer++) {
			struct vidow_layer_surface *ls;
			/* newest first, matching sway's exclusive-zone ordering */
			wl_list_for_each_reverse(ls, &server->layers, link) {
				if (ls->output != output || ls->layer != layer) {
					continue;
				}
				/* wlroots asserts unless the client has committed once;
				 * the initial configure is sent from the commit handler. */
				if (!ls->layer_surface->initialized) {
					continue;
				}
				wlr_log(WLR_DEBUG, "configure layer %s layer=%u",
					ls->layer_surface->namespace ?: "?", layer);
				/* configure() writes the node position itself; clear our
				 * animation offset first so it cannot accumulate. */
				wlr_scene_node_set_position(&ls->scene->tree->node, 0, 0);
				wlr_scene_layer_surface_v1_configure(ls->scene, &full_area,
					&usable_area);

				int lx = 0, ly = 0;
				wlr_scene_node_coords(&ls->scene->tree->node, &lx, &ly);
				struct wlr_box box = {
					.x = lx, .y = ly,
					.width = ls->layer_surface->surface->current.width,
					.height = ls->layer_surface->surface->current.height,
				};
				/* 位置 / 尺寸真的变了才作废背后的模糊：这个函数在开关动画里
				 * 每帧都调，而 anim_off 是加在场景节点上的、geom 不含它 ——
				 * 所以动画期间这里不会每帧作废。 */
				if (memcmp(&box, &ls->geom, sizeof(box)) != 0) {
					ls->geom = box;
					vidow_layer_blur_invalidate(ls);
				}
				wlr_scene_node_set_position(&ls->scene->tree->node,
					lx + (int)lround(ls->anim_off_x),
					ly + (int)lround(ls->anim_off_y));
				vidow_fx_set_alpha(&ls->scene->tree->node, ls->anim_alpha);
			}
		}

		output->usable_area = usable_area;
	}
}

/* ------------------------------------------------------------- keyboard */

static void layer_focus(struct vidow_layer_surface *ls) {
	struct vidow_server *server = ls->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);

	server->keyboard_layer = ls;
	if (kb != NULL) {
		wlr_seat_keyboard_notify_enter(seat, ls->layer_surface->surface,
			kb->keycodes, kb->num_keycodes, &kb->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(seat, ls->layer_surface->surface,
			NULL, 0, NULL);
	}
	vidow_im_focus(server, ls->layer_surface->surface);
}

static void layer_unfocus(struct vidow_layer_surface *ls) {
	struct vidow_server *server = ls->server;

	if (server->keyboard_layer != ls) {
		return;
	}
	server->keyboard_layer = NULL;

	/* Hand the keyboard back to the last focused toplevel, if any. */
	struct vidow_view *view = server->focused;
	if (view != NULL) {
		wlr_seat_keyboard_notify_clear_focus(server->seat);
		vidow_focus_view(view);
	}
}

bool vidow_layer_focus_surface(struct vidow_server *server,
		struct wlr_surface *surface) {
	if (surface == NULL) {
		return false;
	}
	struct wlr_layer_surface_v1 *layer_surface =
		wlr_layer_surface_v1_try_from_wlr_surface(surface);
	if (layer_surface == NULL) {
		return false;
	}

	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		if (ls->layer_surface != layer_surface) {
			continue;
		}
		if (layer_surface->current.keyboard_interactive ==
				ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
			return false;
		}
		layer_focus(ls);
		return true;
	}
	return false;
}

/* ------------------------------------------------------------ animation */

/* Which way a layer surface should fly in from, based on its anchors. */
static void layer_slide_dir(struct vidow_layer_surface *ls, int *dx, int *dy) {
	uint32_t anchor = ls->layer_surface->current.anchor;
	*dx = 0;
	*dy = 0;
	if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) {
		*dy = -1;
	} else if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) {
		*dy = 1;
	} else if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) {
		*dx = -1;
	} else if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) {
		*dx = 1;
	} else {
		*dy = -1;
	}
}

static void layer_anim_update(void *data, double t) {
	struct vidow_layer_surface *ls = data;
	struct vidow_config *cfg = &ls->server->config;
	double k = 1.0 - t;

	ls->anim_off_x = ls->anim_from_x * k;
	ls->anim_off_y = ls->anim_from_y * k;
	ls->anim_alpha = cfg->open_alpha_from + (1.0 - cfg->open_alpha_from) * t;
	/* 动画中间不重做模糊底衬（一帧一次太贵），落定时在 layer_anim_done 补一次 */
	vidow_arrange_layers(ls->server);
}

static void layer_anim_done(void *data, bool cancelled) {
	struct vidow_layer_surface *ls = data;
	ls->anim = NULL;
	if (!cancelled) {
		ls->anim_off_x = 0.0;
		ls->anim_off_y = 0.0;
		ls->anim_alpha = 1.0;
	}
	/* 动画停在最终位置上了，现在重做一次模糊，把这一帧的底衬固化下来 */
	vidow_backdrop_damage(ls->server, true);
	vidow_arrange_layers(ls->server);
}

static void layer_open_anim(struct vidow_layer_surface *ls) {
	struct vidow_config *cfg = &ls->server->config;
	int dx = 0, dy = 0;
	layer_slide_dir(ls, &dx, &dy);

	int w = ls->geom.width > 0 ? ls->geom.width : 64;
	int h = ls->geom.height > 0 ? ls->geom.height : 64;
	ls->anim_from_x = (double)(dx * w);
	ls->anim_from_y = (double)(dy * h);
	ls->anim_off_x = ls->anim_from_x;
	ls->anim_off_y = ls->anim_from_y;
	ls->anim_alpha = cfg->open_alpha_from;

	vidow_anim_cancel(ls->anim);
	ls->anim = vidow_anim_start(ls->server, "layer-open", cfg->layer_open_ms,
		cfg->easing, layer_anim_update, layer_anim_done, ls);
	if (ls->anim == NULL) {
		ls->anim_off_x = 0.0;
		ls->anim_off_y = 0.0;
		ls->anim_alpha = 1.0;
	}
}

/* The layer surface has no buffer once unmap fires: animate a snapshot. */
static void layer_snapshot_close(struct vidow_layer_surface *ls) {
	struct vidow_server *server = ls->server;
	struct vidow_config *cfg = &server->config;
	if (ls->last_buffer == NULL || cfg->layer_close_ms <= 0 ||
			ls->geom.width <= 0 || ls->geom.height <= 0) {
		return;
	}
	int dx = 0, dy = 0;
	layer_slide_dir(ls, &dx, &dy);
	int slide = cfg->close_slide_px;

	/* layer surface 没有「窗口几何」和表面之分：整块就是内容。 */
	struct vidow_fx_geo geo = {
		.off_x = 0,
		.off_y = 0,
		.w = ls->geom.width,
		.h = ls->geom.height,
		.src_w = ls->geom.width,
		.src_h = ls->geom.height,
	};
	vidow_closing_start(server, layer_tree(server, ls->layer), ls->last_buffer,
		&geo, ls->geom.x + (int)lround(ls->anim_off_x),
		ls->geom.y + (int)lround(ls->anim_off_y),
		ls->geom.width, ls->geom.height, cfg->layer_close_ms,
		dy * slide, NULL,
		ls->output != NULL && ls->output->wlr_output != NULL ?
			(double)ls->output->wlr_output->scale : 1.0);
}

/* --------------------------------------------------------------- events */

static void layer_surface_map(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_layer_surface *ls = wl_container_of(listener, ls, map);
	struct vidow_server *server = ls->server;

	wlr_log(WLR_INFO, "layer map %s layer=%u exclusive=%d kb_interactive=%u",
		ls->layer_surface->namespace != NULL ? ls->layer_surface->namespace : "?",
		ls->layer, ls->layer_surface->current.exclusive_zone,
		ls->layer_surface->current.keyboard_interactive);

	if (ls->layer_surface->current.keyboard_interactive !=
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
		layer_focus(ls);
	}

	/* 一块 layer surface 上屏：底衬里多了一个东西 */
	vidow_backdrop_damage(server, true);
	vidow_arrange_layers(server); /* gives ls->geom, used to pick the slide */
	layer_open_anim(ls);
	vidow_arrange_layers(server);
	vidow_arrange(server);
	vidow_ipc_emit_layer(server, "layer-mapped", ls);
}

static void layer_surface_unmap(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_layer_surface *ls = wl_container_of(listener, ls, unmap);
	struct vidow_server *server = ls->server;

	layer_snapshot_close(ls);
	layer_unfocus(ls);
	vidow_backdrop_damage(server, true);
	vidow_arrange_layers(server);
	vidow_arrange(server);
	vidow_ipc_emit_layer(server, "layer-unmapped", ls);
}

static void layer_surface_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_layer_surface *ls = wl_container_of(listener, ls, commit);
	struct vidow_server *server = ls->server;

	/* The first configure must reflect the state the client just committed
	 * (anchor, exclusive zone, desired size). */
	if (ls->layer_surface->initial_commit) {
		vidow_backdrop_damage(server, true);
		vidow_arrange_layers(server);
		vidow_arrange(server);
	}

	struct wlr_client_buffer *client = ls->layer_surface->surface->buffer;
	if (client != NULL && client->source != NULL &&
			client->source != ls->last_buffer) {
		wlr_buffer_unlock(ls->last_buffer);
		ls->last_buffer = wlr_buffer_lock(client->source);
	}

	/* A repaint of the wallpaper or a bottom bar changes the backdrop of
	 * every window; the upper layers are not part of it. */
	if (ls->layer <= ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM) {
		vidow_backdrop_damage(server, true);
	}
}

static void layer_surface_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_layer_surface *ls = wl_container_of(listener, ls, destroy);
	struct vidow_server *server = ls->server;

	vidow_ipc_emit_layer(server, "layer-closed", ls);
	layer_unfocus(ls);
	vidow_layer_blur_release(ls);
	vidow_anim_cancel(ls->anim);
	ls->anim = NULL;
	wlr_buffer_unlock(ls->last_buffer);
	ls->last_buffer = NULL;

	wl_list_remove(&ls->map.link);
	wl_list_remove(&ls->unmap.link);
	wl_list_remove(&ls->commit.link);
	wl_list_remove(&ls->new_popup.link);
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->link);
	free(ls);

	vidow_backdrop_damage(server, true);
	vidow_arrange_layers(server);
	vidow_arrange(server);
}

static void layer_surface_new_popup(struct wl_listener *listener, void *data) {
	struct vidow_layer_surface *ls = wl_container_of(listener, ls, new_popup);
	struct vidow_server *server = ls->server;
	struct wlr_xdg_popup *popup = data;

	wlr_scene_xdg_surface_create(ls->scene->tree, popup->base);

	struct wlr_box box = {0};
	if (ls->output != NULL) {
		wlr_output_layout_get_box(server->output_layout,
			ls->output->wlr_output, &box);
	}
	/* 跟 view.c 一样：unconstrain 要等 popup 自己第一次 commit（wlroots 里
	 * schedule_configure 有 assert(surface->initialized)） */
	vidow_popup_unconstrain_on_commit(server, popup, &box);
}

/* 一块屏拔了：它的 layer surface（状态栏这类）挪到接收它的那块屏上 —— 跟窗口
 * 一样，不能让 ls->output 指着已经被释放的 vidow_output。归属留在记忆里。 */
void vidow_layer_output_gone(struct vidow_server *server,
		struct vidow_output_memory *memory, struct wlr_output *dead,
		struct vidow_output *fallback) {
	size_t moved = 0;
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		if (ls->home != memory && (ls->output == NULL ||
				ls->output->wlr_output != dead)) {
			continue;
		}
		ls->output = fallback;
		if (fallback != NULL) {
			/* 客户端也要知道它现在画在哪块屏上（anchors 要按新屏的尺寸算） */
			ls->layer_surface->output = fallback->wlr_output;
		}
		moved++;
	}
	if (moved == 0) {
		return;
	}
	vidow_backdrop_damage(server, true);
	vidow_arrange_layers(server);
	wlr_log(WLR_INFO, "hotplug: %zu 个 layer surface 挪到 %s 上住", moved,
		fallback != NULL ? fallback->wlr_output->name : "(没有屏了)");
}

/* 一块屏回来了：归属它的 layer surface 挪回去。 */
void vidow_layer_output_added(struct vidow_server *server,
		struct vidow_output_memory *memory, struct vidow_output *output) {
	size_t moved = 0;
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		if (ls->output == output) {
			continue;
		}
		if (ls->home != memory && ls->output != NULL) {
			continue; /* 有别的家，也还在住着，别动它 */
		}
		if (ls->home == NULL) {
			ls->home = memory;
		}
		ls->output = output;
		ls->layer_surface->output = output->wlr_output;
		moved++;
	}
	if (moved > 0) {
		vidow_backdrop_damage(server, true);
		vidow_arrange_layers(server);
	}
}

static void server_new_layer_surface(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, new_layer_surface);
	struct wlr_layer_surface_v1 *layer_surface = data;

	struct vidow_layer_surface *ls = calloc(1, sizeof(*ls));
	if (ls == NULL) {
		return;
	}
	ls->server = server;
	ls->layer_surface = layer_surface;
	ls->id = ++server->next_layer_id;
	ls->layer = layer_surface->pending.layer;
	wlr_log(WLR_DEBUG, "new layer surface %s layer=%u out=%p",
		layer_surface->namespace ?: "?", ls->layer, (void *)layer_surface->output);

	/* Clients may leave the output unset ("pick one for me"). */
	if (layer_surface->output == NULL) {
		layer_surface->output = server->focused_output;
		if (layer_surface->output == NULL && !wl_list_empty(&server->outputs)) {
			struct vidow_output *o =
				wl_container_of(server->outputs.next, o, link);
			layer_surface->output = o->wlr_output;
		}
	}

	struct vidow_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output == layer_surface->output) {
			ls->output = o;
			break;
		}
	}
	if (ls->output == NULL) {
		wlr_log(WLR_ERROR, "layer surface for unknown output, dropping");
		free(ls);
		return;
	}
	/* 归属记在记忆里：这块屏拔了要挪到别的屏上，屏回来再挪回来。 */
	ls->home = vidow_output_memory_for(&server->output_memories,
		layer_surface->output->name);

	ls->anim_alpha = 1.0;
	ls->scene = wlr_scene_layer_surface_v1_create(
		layer_tree(server, ls->layer), layer_surface);
	if (ls->scene == NULL) {
		free(ls);
		return;
	}

	wl_list_insert(&server->layers, &ls->link);

	ls->map.notify = layer_surface_map;
	wl_signal_add(&layer_surface->surface->events.map, &ls->map);
	ls->unmap.notify = layer_surface_unmap;
	wl_signal_add(&layer_surface->surface->events.unmap, &ls->unmap);
	ls->commit.notify = layer_surface_commit;
	wl_signal_add(&layer_surface->surface->events.commit, &ls->commit);
	ls->destroy.notify = layer_surface_destroy;
	wl_signal_add(&layer_surface->events.destroy, &ls->destroy);
	ls->new_popup.notify = layer_surface_new_popup;
	wl_signal_add(&layer_surface->events.new_popup, &ls->new_popup);
}

bool vidow_layer_init(struct vidow_server *server) {
	server->layer_shell = wlr_layer_shell_v1_create(server->display, 4);
	if (server->layer_shell == NULL) {
		return false;
	}
	server->new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&server->layer_shell->events.new_surface,
		&server->new_layer_surface);
	return true;
}
