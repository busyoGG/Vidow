/* Per-client zxdg_output_manager_v1 (based on wlroots' MIT implementation).
 *
 * Wayland clients receive the untouched layout position/effective resolution.
 * Xwayland alone receives uniform virtual output rectangles from xwmscale.c:
 * all cells use the largest current transformed native resolution, arranged
 * in Wayland output order (e.g. DP-3 @0, DP-2 @3840, each 3840x2160).
 *
 * Do not use _XWAYLAND_GLOBAL_OUTPUT_SCALE: it also changes wl_surface
 * buffer_scale, so 1x1 helper windows can fail wlroots' divisibility check.
 * Changing only Xwayland's xdg-output geometry avoids that protocol error.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>
#include <wlr/xwayland/server.h>
#include <wlr/xwayland/xwayland.h>

#include "server.h"
#include "xdg-output-unstable-v1-server-protocol.h"

#define OUTPUT_MANAGER_VERSION 3
#define OUTPUT_DONE_DEPRECATED_SINCE_VERSION 3
#define OUTPUT_DESCRIPTION_MUTABLE_SINCE_VERSION 3

struct vidow_xdo_output {
	struct vidow_xdo_manager *manager;
	struct wlr_output_layout_output *layout_output;

	/* 逻辑尺寸（所有 Wayland 客户端看到的） */
	int x, y, width, height;
	/* X-only uniform virtual rectangle; never the Wayland layout box. */
	int dev_width, dev_height;
	int dev_x, dev_y;

	struct wl_list resources; /* wl_resource.link */
	struct wl_list link;

	struct wl_listener destroy;
	struct wl_listener description;
};

struct vidow_xdo_manager {
	struct wl_global *global;
	struct wlr_output_layout *layout;
	/* Xwayland 的 wl_client；没起来 / 已经没了就是 NULL。 */
	struct wl_client *xwayland_client;
	/* 兜底：Xwayland 可能在 set_xwayland_client 之前就先绑了 xdg-output
	 * （它一起来就查屏幕尺寸，而 ready 信号比那晚）。所以每次发事件时
	 * 再实时问一遍「这个 client 是不是 Xwayland 的」，别只信缓存。 */
	struct vidow_server *server;

	struct wl_list outputs; /* vidow_xdo_output.link */

	struct wl_listener layout_add;
	struct wl_listener layout_change;
	struct wl_listener layout_destroy;
	struct wl_listener display_destroy;
};

static struct vidow_xdo_manager *xdo_mgr = NULL;

static void output_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zxdg_output_v1_interface output_implementation = {
	.destroy = output_handle_destroy,
};

static void output_handle_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

/* 关键的那一处：按客户端决定报哪一套尺寸。 */
static void output_send_details(struct vidow_xdo_output *o,
		struct wl_resource *resource) {
	struct wl_client *cli = wl_resource_get_client(resource);
	struct wl_client *xw = xdo_mgr != NULL ? xdo_mgr->xwayland_client : NULL;
	/* 实时再问一遍：Xwayland 一起来就会绑 xdg-output，而它的 ready 信号比那
	 * 晚 —— 只信缓存的话，X 会拿**逻辑尺寸**把屏建好，之后再怎么重发都不会
	 * 改（实测：单屏 scale=2 时 X 屏停在 640，应该是 1280）。
	 *
	 * 测试钩子 VIDOW_TEST_XWAYLAND_UNPATCHED 照旧能把它当陌生人（验降级路）。 */
	if (xdo_mgr != NULL && getenv("VIDOW_TEST_XWAYLAND_UNPATCHED") == NULL &&
			xdo_mgr->server != NULL &&
			xdo_mgr->server->xwayland_server != NULL) {
		xw = xdo_mgr->server->xwayland_server->client;
	}
	bool is_xwayland = xw != NULL && cli == xw;

	zxdg_output_v1_send_logical_position(resource,
		is_xwayland ? o->dev_x : o->x,
		is_xwayland ? o->dev_y : o->y);
	zxdg_output_v1_send_logical_size(resource,
		is_xwayland ? o->dev_width : o->width,
		is_xwayland ? o->dev_height : o->height);
	if (wl_resource_get_version(resource) <
			OUTPUT_DONE_DEPRECATED_SINCE_VERSION) {
		zxdg_output_v1_send_done(resource);
	}
}

/* Wayland geometry remains untouched. Only Xwayland sees the uniform virtual
 * rectangles shared with configure, input mapping and the root-size probe. */
static void output_measure(struct vidow_xdo_output *o) {
	struct wlr_output_layout_output *lo = o->layout_output;
	o->x = lo->x;
	o->y = lo->y;
	wlr_output_effective_resolution(lo->output, &o->width, &o->height);
	struct wlr_box native = {0};
	if (vidow_xwm_output_boxes(o->manager->server, lo->output, NULL, &native)) {
		o->dev_x = native.x;
		o->dev_y = native.y;
		o->dev_width = native.width;
		o->dev_height = native.height;
	} else {
		o->dev_x = o->x;
		o->dev_y = o->y;
		o->dev_width = o->width;
		o->dev_height = o->height;
	}
}

static void output_update(struct vidow_xdo_output *o) {
	int ox = o->x, oy = o->y, ow = o->width, oh = o->height;
	int odw = o->dev_width, odh = o->dev_height;
	int odx = o->dev_x, ody = o->dev_y;
	output_measure(o);
	if (ox == o->x && oy == o->y && ow == o->width && oh == o->height &&
			odw == o->dev_width && odh == o->dev_height &&
			odx == o->dev_x && ody == o->dev_y) {
		return; /* 一个数都没变：别让客户端白重算一次布局 */
	}

	struct wl_resource *resource;
	wl_resource_for_each(resource, &o->resources) {
		output_send_details(o, resource);
	}
	wlr_output_schedule_done(o->layout_output->output);
}

/* Compatibility origin accessor; compute live rather than using event caches. */
void vidow_xdg_output_dev_origin(struct wlr_output *output, int *x, int *y) {
	struct wlr_box native = {0};
	if (xdo_mgr != NULL) {
		vidow_xwm_output_boxes(xdo_mgr->server, output, NULL, &native);
	}
	if (x != NULL) {
		*x = native.x;
	}
	if (y != NULL) {
		*y = native.y;
	}
}

void vidow_xdg_output_set_xwayland_client(struct wl_client *client) {
	if (xdo_mgr == NULL) {
		return;
	}
	/* 测试钩子：本机装的 Xwayland 一定支持 xdg-output，验不了「客户端不支持这条
	 * 协议、X 屏幕只有逻辑尺寸」那条降级路。设了它就把 Xwayland 当陌生人，
	 * 于是 xdgoutput 只报逻辑尺寸 —— 等价于「这块 Xwayland 不认」。
	 * （以前这个钩子叫 VIDOW_TEST_XWAYLAND_UNPATCHED，管的是
	 * _XWAYLAND_GLOBAL_OUTPUT_SCALE 那个原子；现在的开关是这里。） */
	if (getenv("VIDOW_TEST_XWAYLAND_UNPATCHED") != NULL) {
		client = NULL;
	}
	xdo_mgr->xwayland_client = client;
	struct vidow_xdo_output *o;
	wl_list_for_each(o, &xdo_mgr->outputs, link) {
		struct wl_resource *resource;
		wl_resource_for_each(resource, &o->resources) {
			output_send_details(o, resource);
		}
		wlr_output_schedule_done(o->layout_output->output);
	}
}

static void output_destroy(struct vidow_xdo_output *o) {
	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, &o->resources) {
		wl_list_remove(wl_resource_get_link(resource));
		wl_list_init(wl_resource_get_link(resource));
	}
	wl_list_remove(&o->destroy.link);
	wl_list_remove(&o->description.link);
	wl_list_remove(&o->link);
	free(o);
}

static void output_manager_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zxdg_output_manager_v1_interface
	output_manager_implementation;

static void output_manager_handle_get_xdg_output(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		struct wl_resource *output_resource) {
	assert(wl_resource_instance_of(resource, &zxdg_output_manager_v1_interface,
		&output_manager_implementation));

	struct vidow_xdo_manager *manager = wl_resource_get_user_data(resource);
	struct wlr_output *output = wlr_output_from_resource(output_resource);

	struct wl_resource *xdg_output_resource = wl_resource_create(client,
			&zxdg_output_v1_interface, wl_resource_get_version(resource), id);
	if (xdg_output_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(xdg_output_resource, &output_implementation,
		NULL, output_handle_resource_destroy);

	if (output == NULL) {
		wl_list_init(wl_resource_get_link(xdg_output_resource));
		return;
	}

	struct wlr_output_layout_output *layout_output =
		wlr_output_layout_get(manager->layout, output);
	if (layout_output == NULL) {
		wl_list_init(wl_resource_get_link(xdg_output_resource));
		return;
	}

	struct vidow_xdo_output *_o, *o = NULL;
	wl_list_for_each(_o, &manager->outputs, link) {
		if (_o->layout_output == layout_output) {
			o = _o;
			break;
		}
	}
	if (o == NULL) {
		wl_list_init(wl_resource_get_link(xdg_output_resource));
		return;
	}

	wl_list_insert(&o->resources, wl_resource_get_link(xdg_output_resource));

	/* name / description 只在建立时发一次（协议要求）。 */
	uint32_t xdg_version = wl_resource_get_version(xdg_output_resource);
	if (xdg_version >= ZXDG_OUTPUT_V1_NAME_SINCE_VERSION) {
		zxdg_output_v1_send_name(xdg_output_resource, output->name);
	}
	if (xdg_version >= ZXDG_OUTPUT_V1_DESCRIPTION_SINCE_VERSION &&
			output->description != NULL) {
		zxdg_output_v1_send_description(xdg_output_resource,
			output->description);
	}

	output_send_details(o, xdg_output_resource);

	uint32_t wl_version = wl_resource_get_version(output_resource);
	if (wl_version >= WL_OUTPUT_DONE_SINCE_VERSION &&
			xdg_version >= OUTPUT_DONE_DEPRECATED_SINCE_VERSION) {
		wl_output_send_done(output_resource);
	}
}

static const struct zxdg_output_manager_v1_interface
		output_manager_implementation = {
	.destroy = output_manager_handle_destroy,
	.get_xdg_output = output_manager_handle_get_xdg_output,
};

static void output_manager_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	struct vidow_xdo_manager *manager = data;
	struct wl_resource *resource = wl_resource_create(wl_client,
		&zxdg_output_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(resource, &output_manager_implementation,
		manager, NULL);
}

static void handle_output_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_xdo_output *o = wl_container_of(listener, o, destroy);
	output_destroy(o);
}

static void handle_output_description(struct wl_listener *listener,
		void *data) {
	(void)data;
	struct vidow_xdo_output *o = wl_container_of(listener, o, description);
	struct wlr_output *output = o->layout_output->output;
	if (output->description == NULL) {
		return;
	}
	struct wl_resource *resource;
	wl_resource_for_each(resource, &o->resources) {
		if (wl_resource_get_version(resource) >=
				OUTPUT_DESCRIPTION_MUTABLE_SINCE_VERSION) {
			zxdg_output_v1_send_description(resource, output->description);
		}
	}
}

static void add_output(struct vidow_xdo_manager *manager,
		struct wlr_output_layout_output *layout_output) {
	struct vidow_xdo_output *o = calloc(1, sizeof(*o));
	if (o == NULL) {
		return;
	}
	wl_list_init(&o->resources);
	o->manager = manager;
	o->layout_output = layout_output;
	output_measure(o);

	o->destroy.notify = handle_output_destroy;
	wl_signal_add(&layout_output->output->events.destroy, &o->destroy);
	o->description.notify = handle_output_description;
	wl_signal_add(&layout_output->output->events.description, &o->description);

	wl_list_insert(&manager->outputs, &o->link);
}

static void handle_layout_add(struct wl_listener *listener, void *data) {
	struct vidow_xdo_manager *manager =
		wl_container_of(listener, manager, layout_add);
	add_output(manager, data);
}

static void handle_layout_change(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_xdo_manager *manager =
		wl_container_of(listener, manager, layout_change);
	struct vidow_xdo_output *o;
	wl_list_for_each(o, &manager->outputs, link) {
		output_update(o);
	}
}

static void handle_layout_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_xdo_manager *manager =
		wl_container_of(listener, manager, layout_destroy);
	struct vidow_xdo_output *o, *tmp;
	wl_list_for_each_safe(o, tmp, &manager->outputs, link) {
		output_destroy(o);
	}
	wl_list_remove(&manager->layout_add.link);
	wl_list_remove(&manager->layout_change.link);
	wl_list_remove(&manager->layout_destroy.link);
	/* display 的销毁监听也要摘：退出时 output_layout 先于 display 销毁，
	 * 不摘的话 display_destroy 会再摸一遍已经 free 的 manager（ASan 抓到的
	 * heap-use-after-free，退出时段错误）。 */
	wl_list_remove(&manager->display_destroy.link);
	free(manager);
	xdo_mgr = NULL;
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_xdo_manager *manager =
		wl_container_of(listener, manager, display_destroy);

	struct vidow_xdo_output *o, *tmp;
	wl_list_for_each_safe(o, tmp, &manager->outputs, link) {
		output_destroy(o);
	}
	wl_list_remove(&manager->layout_add.link);
	wl_list_remove(&manager->layout_change.link);
	wl_list_remove(&manager->layout_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
	xdo_mgr = NULL;
}

struct vidow_xdo_manager *vidow_xdg_output_manager_create(
		struct wl_display *display, struct wlr_output_layout *layout,
		struct vidow_server *server) {
	struct vidow_xdo_manager *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}
	wl_list_init(&manager->outputs);
	manager->layout = layout;
	manager->server = server;

	manager->global = wl_global_create(display,
		&zxdg_output_manager_v1_interface, OUTPUT_MANAGER_VERSION, manager,
		output_manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	struct wlr_output_layout_output *lo;
	wl_list_for_each(lo, &layout->outputs, link) {
		add_output(manager, lo);
	}

	manager->layout_add.notify = handle_layout_add;
	wl_signal_add(&layout->events.add, &manager->layout_add);
	manager->layout_change.notify = handle_layout_change;
	wl_signal_add(&layout->events.change, &manager->layout_change);
	manager->layout_destroy.notify = handle_layout_destroy;
	wl_signal_add(&layout->events.destroy, &manager->layout_destroy);
	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	xdo_mgr = manager;
	return manager;
}
