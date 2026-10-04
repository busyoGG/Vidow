/* ext-background-effect-v1：客户端告诉合成器「我背后这块要模糊」。
 *
 * wlroots 0.20 没实现这个协议（没有 wlr_ext_background_effect_v1.h），所以
 * 服务端在这里手写：
 *   - 全局 ext_background_effect_manager_v1，bind 时告诉客户端支持 blur
 *   - 每个 wl_surface 最多挂一个 ext_background_effect_surface_v1
 *   - set_blur_region 是双缓冲的：客户端 commit 之后才生效
 *
 * 渲染侧：区域取包围盒交给装饰着色器，模糊只画在这个盒子里；而且只要客户端
 * 请求了模糊就模糊，不看 blur_all（那个开关是给「没请求的窗口」用的）。
 */

#include "server.h"

#include <stdlib.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

#include "ext-background-effect-v1-server-protocol.h"

struct vidow_bgeffect {
	struct wl_resource *resource;
	struct vidow_server *server;
	struct wlr_surface *surface;

	/* 挂在 surface 上，用于「这个 surface 有没有请求模糊」的查询。
	 * surface 死亡时 addon 的 destroy 回调会替我们收尾。 */
	struct wlr_addon addon;
	bool addon_active;

	struct wl_listener commit;
	bool commit_removed;

	/* raw 是客户端刚设的（下一次 commit 才生效，天然双缓冲），current 是
	 * 裁到 surface 尺寸之后真正用的那份。 */
	pixman_region32_t raw;
	pixman_region32_t current;
	bool have;
	struct wlr_box box; /* current 的包围盒，用来判断「变了没有」 */
};

static const struct ext_background_effect_surface_v1_interface surface_impl;
static const struct wlr_addon_interface addon_impl;

/* ------------------------------------------------------------- 生命周期 */

static void bgeffect_commit_detach(struct vidow_bgeffect *bg) {
	if (!bg->commit_removed) {
		wl_list_remove(&bg->commit.link);
		bg->commit_removed = true;
	}
}

/* surface 没了：协议说这个对象变 inert，客户端不会再收到东西。直接销毁资源，
 * 让客户端那边的 proxy 也变成 inert。 */
static void bgeffect_addon_destroy(struct wlr_addon *addon) {
	struct vidow_bgeffect *bg = wl_container_of(addon, bg, addon);
	bg->addon_active = false;
	wlr_addon_finish(addon);
	bg->surface = NULL;
	bgeffect_commit_detach(bg);
	if (bg->resource != NULL) {
		wl_resource_destroy(bg->resource); /* 析构里 free(bg) */
	} else {
		pixman_region32_fini(&bg->raw);
		pixman_region32_fini(&bg->current);
		free(bg);
	}
}

static const struct wlr_addon_interface addon_impl = {
	.name = "vidow-bgeffect",
	.destroy = bgeffect_addon_destroy,
};

static void bgeffect_resource_destroy(struct wl_resource *resource) {
	struct vidow_bgeffect *bg = wl_resource_get_user_data(resource);
	if (bg == NULL) {
		return;
	}
	wl_resource_set_user_data(resource, NULL);
	if (bg->addon_active) {
		/* 这里不会回调 addon 的 destroy（那个只有 addon set 析构时才走），
		 * 所以先摘掉，免得 surface 之后再死一次踩到已释放的 bg */
		bg->addon_active = false;
		wlr_addon_finish(&bg->addon);
	}
	bgeffect_commit_detach(bg);
	pixman_region32_fini(&bg->raw);
	pixman_region32_fini(&bg->current);
	free(bg);
}

/* --------------------------------------------------------------- 请求 */

static void bgeffect_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	(void)client;
	wl_resource_destroy(resource);
}

static void bgeffect_set_blur_region(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *region) {
	(void)client;
	struct vidow_bgeffect *bg = wl_resource_get_user_data(resource);
	if (bg == NULL || bg->surface == NULL) {
		wl_resource_post_error(resource,
			EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED,
			"the surface is gone");
		return;
	}

	pixman_region32_clear(&bg->raw);
	if (region != NULL) {
		const pixman_region32_t *r = wlr_region_from_resource(region);
		if (r != NULL) {
			pixman_region32_union(&bg->raw, &bg->raw, r);
		}
	}
}

static const struct ext_background_effect_surface_v1_interface surface_impl = {
	.destroy = bgeffect_destroy,
	.set_blur_region = bgeffect_set_blur_region,
};

/* pending -> current，然后让这个窗口重画一次（区域只影响着色器输出，
 * 贴图身份没变，不主动失效的话帧循环不会重画）。 */
static void bgeffect_surface_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_bgeffect *bg = wl_container_of(listener, bg, commit);

	/* 每次 commit 都从原始区域重建再裁：第一次 commit 时 surface 还是
	 * 0x0，裁完是空的；之后 resize 了还要跟着裁。所以不能拿上一次裁过的
	 * 结果再裁一次（那会一直空着），得回到 raw 重建。 */
	pixman_region32_clear(&bg->current);
	pixman_region32_union(&bg->current, &bg->current, &bg->raw);
	if (bg->surface != NULL) {
		pixman_region32_t clip;
		pixman_region32_init_rect(&clip, 0, 0,
			bg->surface->current.width, bg->surface->current.height);
		pixman_region32_intersect(&bg->current, &bg->current, &clip);
		pixman_region32_fini(&clip);
	}

	bool have = pixman_region32_not_empty(&bg->current);
	struct wlr_box box = {0};
	if (have) {
		pixman_box32_t *e = pixman_region32_extents(&bg->current);
		box.x = e->x1;
		box.y = e->y1;
		box.width = e->x2 - e->x1;
		box.height = e->y2 - e->y1;
	}
	/* 只在真的变了的时候重画，不然客户端每帧 commit 都会逼我们重绘装饰 */
	if (have == bg->have && box.x == bg->box.x && box.y == bg->box.y &&
			box.width == bg->box.width && box.height == bg->box.height) {
		return;
	}
	bg->have = have;
	bg->box = box;

	struct vidow_view *view =
		vidow_view_from_surface(bg->server, bg->surface);
	if (view != NULL) {
		vidow_view_blur_invalidate(view);
	}
}

static void manager_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	(void)client;
	wl_resource_destroy(resource);
}

static void manager_get_background_effect(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct vidow_server *server = wl_resource_get_user_data(resource);
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	if (server == NULL || surface == NULL) {
		return;
	}
	if (wlr_addon_find(&surface->addons, server, &addon_impl) != NULL) {
		wl_resource_post_error(resource,
			EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
			"surface already has a background effect object");
		return;
	}

	struct vidow_bgeffect *bg = calloc(1, sizeof(*bg));
	if (bg == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	bg->server = server;
	bg->surface = surface;
	pixman_region32_init(&bg->raw);
	pixman_region32_init(&bg->current);

	uint32_t version =
		wl_resource_get_version(resource);
	bg->resource = wl_resource_create(client,
		&ext_background_effect_surface_v1_interface, version, id);
	if (bg->resource == NULL) {
		pixman_region32_fini(&bg->raw);
		pixman_region32_fini(&bg->current);
		free(bg);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(bg->resource, &surface_impl, bg,
		bgeffect_resource_destroy);

	bg->commit.notify = bgeffect_surface_commit;
	wl_signal_add(&surface->events.commit, &bg->commit);
	wlr_addon_init(&bg->addon, &surface->addons, server, &addon_impl);
	bg->addon_active = true;
}

static const struct ext_background_effect_manager_v1_interface manager_impl = {
	.destroy = manager_destroy,
	.get_background_effect = manager_get_background_effect,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version,
		uint32_t id) {
	struct vidow_server *server = data;
	struct wl_resource *resource = wl_resource_create(client,
		&ext_background_effect_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, server, NULL);
	ext_background_effect_manager_v1_send_capabilities(resource,
		EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR);
}

/* ------------------------------------------------------------------- api */

bool vidow_bgeffect_blur_box(struct vidow_server *server,
		struct wlr_surface *surface, struct wlr_box *box) {
	if (server == NULL || surface == NULL) {
		return false;
	}
	struct wlr_addon *addon =
		wlr_addon_find(&surface->addons, server, &addon_impl);
	if (addon == NULL) {
		return false;
	}
	struct vidow_bgeffect *bg = wl_container_of(addon, bg, addon);
	if (!bg->have) {
		return false;
	}
	/* 协议允许任意形状；我们取包围盒（着色器里只做矩形裁剪） */
	*box = bg->box;
	return bg->box.width > 0 && bg->box.height > 0;
}

bool vidow_bgeffect_init(struct vidow_server *server) {
	server->bgeffect = wl_global_create(server->display,
		&ext_background_effect_manager_v1_interface, 1, server, manager_bind);
	if (server->bgeffect == NULL) {
		wlr_log(WLR_ERROR, "failed to create ext-background-effect-v1");
		return false;
	}
	return true;
}
