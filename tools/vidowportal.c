/* vidowportal.c — vidow 自己的 xdg-desktop-portal 后端（ScreenCast）
 *
 * 为什么不用 xdg-desktop-portal-kde：它只在 XDG_CURRENT_DESKTOP 整串等于 KDE 时
 * 才注册 ScreenCast（desktopportal.cpp 里是整串比较，实测 "KDE:Vidow" 也不行），
 * 于是要么骗整个会话「你在 Plasma 里」（GTK/Qt 主题、Chromium 的对话框选择全跟着
 * 变），要么给它一个 systemd 用户级 drop-in。两条路都把 vidow 的录屏绑死在 KDE 的
 * 私有实现上。这里自己当后端：
 *
 *   ~/.local/share/xdg-desktop-portal/portals/vidow.portal
 *     [portal]
 *     DBusName=org.freedesktop.impl.portal.desktop.vidow   ← 就是这个进程
 *     Interfaces=org.freedesktop.impl.portal.ScreenCast;
 *   ~/.config/xdg-desktop-portal/vidow-portals.conf
 *     org.freedesktop.impl.portal.ScreenCast=vidow          ← vidow 写的那份
 *
 * 前端（xdg-desktop-portal）只做 DBus 转发，PipeWire 的 fd 也是它自己连的
 * （OpenPipeWireRemote 在前端里，`nm -D` 能看到 pw_context_connect / pw_core_steal_fd），
 * 所以这个后端只要干三件事：弹对话框让用户选源、让合成器建出 PipeWire node、
 * 把 node id 报回去。
 *
 * 流由合成器建：走 zkde_screencast_unstable_v1（vidow 自己实现的那份，见
 * src/screencast.c）—— 它本来就是「客户端要一路流，合成器建 node 再告诉客户端」。
 * KDE 的后端是这么用的，这里也这么用，只是客户端换成了自己人，所以合成器那边
 * 一行都不用改。
 *
 * 对话框：GTK3（和 xdg-desktop-portal-gtk 同一个套路），每个输出一行：
 * 实时缩略图（zwlr_screencopy_unstable_v1）+ 输出名（wl_output.name，vidow 报的是
 * DP-3 这种）。挂到发起请求的窗口上（xdg-foreign，前端给的 parent_window）。
 *
 * 用法（一般不用手敲，vidow 会自己起）：
 *   vidow-portal            # 占住总线名，等前端来调
 *   vidow-portal --probe    # 自检：连上合成器、列一遍输出、退出
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <gdk/gdkwayland.h>

#include <wayland-client.h>

#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "zkde-screencast-unstable-v1-client-protocol.h"

#define BUS_NAME "org.freedesktop.impl.portal.desktop.vidow"
#define OBJECT_PATH "/org/freedesktop/portal/desktop"

/* 缩略图多大（像素）。抓回来的是整屏原图，缩到这么大再给 GTK。 */
#define THUMB_W 320
#define THUMB_H 200
/* 缩略图刷新间隔。KDE 的选择器也是这个量级（每秒一两帧）。 */
#define PREVIEW_MS 500
/* 等合成器把 node 建出来最多等多久。 */
#define STREAM_TIMEOUT_MS 4000

/* portal 的响应码（org.freedesktop.portal.Request.Response）。 */
#define RESPONSE_SUCCESS 0u
#define RESPONSE_CANCELLED 1u
#define RESPONSE_OTHER 2u

/* AvailableSourceTypes：只报 MONITOR。窗口源（WINDOW=2）还没有，报上去等于骗客户端。 */
#define SOURCE_TYPE_MONITOR 1u
/* AvailableCursorModes：hidden=1 | embedded=2。metadata=4 合成器那边还没实现。 */
#define CURSOR_MODES (1u | 2u)
/* impl ScreenCast 的接口版本：2 = 有 cursor_mode，没有 restore_data / persist_mode。 */
#define SC_VERSION 2u

/* ------------------------------------------------------------ 输出 */

struct output {
	struct wl_output *wl;
	char name[64];
	int x, y;      /* 布局坐标（逻辑） */
	int w, h;      /* 逻辑尺寸 = 模式像素 / scale */
	int px_w, px_h;
	int scale;

	/* 缩略图：一次抓一帧，抓完再抓下一帧（screencopy 是一次性的） */
	GtkWidget *image;
	GtkWidget *toggle;
	struct zwlr_screencopy_frame_v1 *frame;
	bool busy;
	struct wl_buffer *buffer;
	void *map;
	size_t map_size;
	int fd;
	int bw, bh, stride;
	uint32_t format;
	bool y_invert;
};

struct stream {
	struct zkde_screencast_stream_unstable_v1 *obj;
	struct output *out;
	uint32_t node;
	bool created;
	bool failed;
};

struct session {
	char *path;
	char *app_id;
	uint32_t types;
	bool multiple;
	uint32_t cursor_mode;
	GPtrArray *streams; /* struct stream * */
	bool started;
	bool closed;
	guint reg_id;
};

struct request {
	char *path;
	struct session *session;
	GDBusMethodInvocation *invocation;
	char *parent_window;
	guint reg_id;
	bool done;
	/* 用户点了「共享」、流已经在建了。之前不能去碰 start_maybe_finish：
	 * 那时候 streams 是空的，「全都settled」会真空成立，直接把 Start 回成失败
	 * （踩过：对话框刚弹出来就被自己回掉了）。 */
	bool starting;
};

/* ------------------------------------------------------------ 全局 */

static struct wl_display *wl;
static struct wl_registry *registry;
static struct wl_shm *shm;
static struct zwlr_screencopy_manager_v1 *scmgr;
static struct zkde_screencast_unstable_v1 *zkde;
static GList *outputs; /* struct output * */
static GDBusConnection *bus;

static struct request *pending; /* 正在弹对话框的那个 Start */
static GtkWidget *dialog;
static GtkWidget *flow;
static GtkWidget *share_button;
static guint preview_timer;
static guint stream_timer;

static void log_info(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fputs("vidow-portal: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* ------------------------------------------------------------ 抓帧缓冲 */

static int create_shm_file(size_t size) {
	int fd = memfd_create("vidow-portal", MFD_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void output_drop_buffer(struct output *out) {
	if (out->buffer != NULL) {
		wl_buffer_destroy(out->buffer);
		out->buffer = NULL;
	}
	if (out->map != NULL) {
		munmap(out->map, out->map_size);
		out->map = NULL;
	}
	if (out->fd >= 0) {
		close(out->fd);
		out->fd = -1;
	}
	out->bw = out->bh = 0;
}

/* 按 screencopy 报的尺寸/步幅建一块 shm 缓冲。尺寸没变就复用。 */
static bool output_make_buffer(struct output *out, uint32_t format, int w, int h, int stride) {
	if (out->buffer != NULL && out->bw == w && out->bh == h &&
			out->stride == stride && out->format == format) {
		return true;
	}
	output_drop_buffer(out);

	size_t size = (size_t)stride * (size_t)h;
	int fd = create_shm_file(size);
	if (fd < 0) {
		return false;
	}
	void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return false;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int)size);
	if (pool == NULL) {
		munmap(map, size);
		close(fd);
		return false;
	}
	out->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, format);
	wl_shm_pool_destroy(pool);
	if (out->buffer == NULL) {
		munmap(map, size);
		close(fd);
		return false;
	}
	out->map = map;
	out->map_size = size;
	out->fd = fd;
	out->bw = w;
	out->bh = h;
	out->stride = stride;
	out->format = format;
	return true;
}

/* shm 里的像素 → RGB 的 GdkPixbuf。
 *
 * XRGB8888 / ARGB8888 在内存里都是 B,G,R,X/A（小端），两者的 R 都在 bit16-23，
 * 所以一个循环就够了。缩略图不需要 alpha，直接丢掉。 */
static GdkPixbuf *output_to_pixbuf(struct output *out) {
	int w = out->bw, h = out->bh;
	if (out->map == NULL || w < 1 || h < 1) {
		return NULL;
	}
	GdkPixbuf *pix = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
	if (pix == NULL) {
		return NULL;
	}
	guchar *dst = gdk_pixbuf_get_pixels(pix);
	int dst_stride = gdk_pixbuf_get_rowstride(pix);
	for (int y = 0; y < h; y++) {
		int sy = out->y_invert ? (h - 1 - y) : y;
		const uint32_t *src = (const uint32_t *)((const uint8_t *)out->map +
			(size_t)sy * (size_t)out->stride);
		guchar *row = dst + (size_t)y * (size_t)dst_stride;
		for (int x = 0; x < w; x++) {
			uint32_t p = src[x];
			row[x * 3 + 0] = (guchar)((p >> 16) & 0xff);
			row[x * 3 + 1] = (guchar)((p >> 8) & 0xff);
			row[x * 3 + 2] = (guchar)(p & 0xff);
		}
	}
	return pix;
}

/* 缩到缩略图大小（保持比例，不放大）。 */
static GdkPixbuf *thumb_of(GdkPixbuf *pix) {
	int w = gdk_pixbuf_get_width(pix);
	int h = gdk_pixbuf_get_height(pix);
	double sx = (double)THUMB_W / (double)w;
	double sy = (double)THUMB_H / (double)h;
	double s = sx < sy ? sx : sy;
	if (s > 1.0) {
		s = 1.0;
	}
	int tw = (int)(w * s), th = (int)(h * s);
	if (tw < 1) tw = 1;
	if (th < 1) th = 1;
	return gdk_pixbuf_scale_simple(pix, tw, th, GDK_INTERP_BILINEAR);
}

/* ------------------------------------------------------------ screencopy */

static void frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
	struct output *out = data;
	(void)frame;
	if (!output_make_buffer(out, format, (int)width, (int)height, (int)stride)) {
		log_info("%s: 建 shm 缓冲失败（%ux%u）", out->name, width, height);
	}
}

static void frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t flags) {
	struct output *out = data;
	(void)frame;
	out->y_invert = (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
}

static void frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
	struct output *out = data;
	(void)tv_sec_hi;
	(void)tv_sec_lo;
	(void)tv_nsec;
	if (out->map != NULL && out->image != NULL) {
		GdkPixbuf *pix = output_to_pixbuf(out);
		if (pix != NULL) {
			GdkPixbuf *thumb = thumb_of(pix);
			g_object_unref(pix);
			if (thumb != NULL) {
				gtk_image_set_from_pixbuf(GTK_IMAGE(out->image), thumb);
				g_object_unref(thumb);
			}
		}
	}
	out->busy = false;
	zwlr_screencopy_frame_v1_destroy(frame);
	out->frame = NULL;
}

static void frame_failed(void *data, struct zwlr_screencopy_frame_v1 *frame) {
	struct output *out = data;
	out->busy = false;
	zwlr_screencopy_frame_v1_destroy(frame);
	out->frame = NULL;
}

static void frame_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *frame) {
	struct output *out = data;
	if (out->buffer != NULL) {
		zwlr_screencopy_frame_v1_copy(frame, out->buffer);
	} else {
		zwlr_screencopy_frame_v1_destroy(frame);
		out->frame = NULL;
		out->busy = false;
	}
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = frame_buffer,
	.flags = frame_flags,
	.ready = frame_ready,
	.failed = frame_failed,
	.buffer_done = frame_buffer_done,
};

/* 每 500ms 给每个输出要一帧。只抓没在抓的那些（screencopy 一次一个）。 */
static gboolean preview_tick(gpointer data) {
	(void)data;
	if (scmgr == NULL) {
		return G_SOURCE_CONTINUE;
	}
	for (GList *it = outputs; it != NULL; it = it->next) {
		struct output *out = it->data;
		if (out->busy || out->image == NULL) {
			continue;
		}
		out->busy = true;
		out->frame = zwlr_screencopy_manager_v1_capture_output(scmgr, 0, out->wl);
		if (out->frame == NULL) {
			out->busy = false;
			continue;
		}
		zwlr_screencopy_frame_v1_add_listener(out->frame, &frame_listener, out);
	}
	wl_display_flush(wl);
	return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------ wl_output */

static void output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
		int32_t physical_width, int32_t physical_height, int32_t subpixel,
		const char *make, const char *model, int32_t transform) {
	struct output *out = data;
	(void)wl_output;
	(void)physical_width;
	(void)physical_height;
	(void)subpixel;
	(void)make;
	(void)model;
	(void)transform;
	out->x = x;
	out->y = y;
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
		int32_t width, int32_t height, int32_t refresh) {
	struct output *out = data;
	(void)wl_output;
	(void)refresh;
	if ((flags & WL_OUTPUT_MODE_CURRENT) != 0) {
		out->px_w = width;
		out->px_h = height;
	}
}

static void output_done(void *data, struct wl_output *wl_output) {
	struct output *out = data;
	(void)wl_output;
	int scale = out->scale > 0 ? out->scale : 1;
	out->w = out->px_w / scale;
	out->h = out->px_h / scale;
	if (out->toggle != NULL) {
		char text[128];
		snprintf(text, sizeof(text), "%s\n%d × %d",
			out->name[0] != '\0' ? out->name : "输出", out->w, out->h);
		gtk_button_set_label(GTK_BUTTON(out->toggle), text);
	}
}

static void output_scale(void *data, struct wl_output *wl_output, int32_t factor) {
	struct output *out = data;
	(void)wl_output;
	out->scale = factor > 0 ? factor : 1;
}

static void output_name(void *data, struct wl_output *wl_output, const char *name) {
	struct output *out = data;
	(void)wl_output;
	snprintf(out->name, sizeof(out->name), "%s", name != NULL ? name : "");
}

static void output_description(void *data, struct wl_output *wl_output, const char *description) {
	(void)data;
	(void)wl_output;
	(void)description;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

/* ------------------------------------------------------------ registry */

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
		const char *interface, uint32_t version) {
	(void)data;
	(void)version;
	if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct output *out = calloc(1, sizeof(*out));
		if (out == NULL) {
			return;
		}
		out->scale = 1;
		out->fd = -1;
		out->wl = wl_registry_bind(reg, name, &wl_output_interface,
			version < 4 ? version : 4);
		wl_output_add_listener(out->wl, &output_listener, out);
		outputs = g_list_append(outputs, out);
	} else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
		scmgr = wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface, 3);
	} else if (strcmp(interface, zkde_screencast_unstable_v1_interface.name) == 0) {
		zkde = wl_registry_bind(reg, name, &zkde_screencast_unstable_v1_interface,
			version < 6 ? version : 6);
	}
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
	(void)data;
	(void)reg;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ------------------------------------------------------------ 流 */

static bool stream_all_settled(struct session *s) {
	for (guint i = 0; i < s->streams->len; i++) {
		struct stream *st = g_ptr_array_index(s->streams, i);
		if (!st->created && !st->failed) {
			return false;
		}
	}
	return true;
}

static void request_finish(struct request *req, uint32_t response, GVariant *results);
static void session_close(struct session *s, const char *why);

static void stream_created(void *data, struct zkde_screencast_stream_unstable_v1 *obj, uint32_t node) {
	struct stream *st = data;
	(void)obj;
	st->node = node;
	st->created = true;
}

static void stream_closed(void *data, struct zkde_screencast_stream_unstable_v1 *obj) {
	struct stream *st = data;
	(void)obj;
	log_info("%s: 合成器关掉了这路流", st->out != NULL ? st->out->name : "?");
}

static void stream_failed(void *data, struct zkde_screencast_stream_unstable_v1 *obj,
		const char *message) {
	struct stream *st = data;
	(void)obj;
	st->failed = true;
	log_info("%s: 合成器拒绝了这路流（%s）", st->out != NULL ? st->out->name : "?",
		message != NULL ? message : "没给原因");
}

static void stream_serial(void *data, struct zkde_screencast_stream_unstable_v1 *obj,
		uint32_t hi, uint32_t lo) {
	(void)data;
	(void)obj;
	(void)hi;
	(void)lo;
}

static const struct zkde_screencast_stream_unstable_v1_listener stream_listener = {
	.created = stream_created,
	.closed = stream_closed,
	.failed = stream_failed,
	.serial = stream_serial,
};

/* ------------------------------------------------------------ 对话框 */

static void session_streams_destroy(struct session *s) {
	if (s->streams == NULL) {
		return;
	}
	for (guint i = 0; i < s->streams->len; i++) {
		struct stream *st = g_ptr_array_index(s->streams, i);
		if (st->obj != NULL) {
			/* close 是 destructor 请求：调完代理就没了，别再 destroy。 */
			zkde_screencast_stream_unstable_v1_close(st->obj);
			st->obj = NULL;
		}
		free(st);
	}
	g_ptr_array_set_size(s->streams, 0);
}

static void session_free(struct session *s) {
	if (s == NULL) {
		return;
	}
	if (s->reg_id != 0 && bus != NULL) {
		g_dbus_connection_unregister_object(bus, s->reg_id);
		s->reg_id = 0;
	}
	session_streams_destroy(s);
	if (s->streams != NULL) {
		g_ptr_array_free(s->streams, TRUE);
		s->streams = NULL;
	}
	free(s->path);
	free(s->app_id);
	free(s);
}

/* Start 的结果字典 a{sv}，里面 "streams" 是 a(ua{sv})。
 * 别把 streams 数组当结果字典直接回 —— GDBus 会当场 g_variant_new 报致命错
 * （踩过：g_variant_new: expected GVariant of type 'a{sv}' but received
 * value has type 'a(ua{sv})'，进程 abort，前端只看到「对端没回就断线」）。 */
static GVariant *start_results(struct session *s);

/* 把 stream 数组变成 Start 要的 a(ua{sv})。 */
static GVariant *streams_variant(struct session *s) {
	GVariantBuilder arr;
	g_variant_builder_init(&arr, G_VARIANT_TYPE("a(ua{sv})"));
	for (guint i = 0; i < s->streams->len; i++) {
		struct stream *st = g_ptr_array_index(s->streams, i);
		if (!st->created) {
			continue;
		}
		GVariantBuilder props;
		g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
		int x = st->out != NULL ? st->out->x : 0;
		int y = st->out != NULL ? st->out->y : 0;
		int w = st->out != NULL ? st->out->w : 0;
		int h = st->out != NULL ? st->out->h : 0;
		g_variant_builder_add(&props, "{sv}", "position", g_variant_new("(ii)", x, y));
		g_variant_builder_add(&props, "{sv}", "size", g_variant_new("(ii)", w, h));
		g_variant_builder_add(&props, "{sv}", "source_type",
			g_variant_new_uint32(SOURCE_TYPE_MONITOR));
		g_variant_builder_add_value(&arr,
			g_variant_new("(u@a{sv})", st->node, g_variant_builder_end(&props)));
	}
	return g_variant_builder_end(&arr);
}

static GVariant *start_results(struct session *s) {
	GVariantBuilder rb;
	g_variant_builder_init(&rb, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&rb, "{sv}", "streams", streams_variant(s));
	return g_variant_builder_end(&rb);
}

static void preview_stop(void) {
	if (preview_timer != 0) {
		g_source_remove(preview_timer);
		preview_timer = 0;
	}
}

static void dialog_destroy(void) {
	preview_stop();
	if (dialog != NULL) {
		gtk_widget_destroy(dialog);
		dialog = NULL;
		flow = NULL;
		share_button = NULL;
	}
	for (GList *it = outputs; it != NULL; it = it->next) {
		struct output *out = it->data;
		out->image = NULL;
		out->toggle = NULL;
		if (out->frame != NULL) {
			zwlr_screencopy_frame_v1_destroy(out->frame);
			out->frame = NULL;
		}
		out->busy = false;
	}
}

static void request_finish(struct request *req, uint32_t response, GVariant *results) {
	if (req == NULL || req->done) {
		return;
	}
	req->done = true;

	if (req->reg_id != 0 && bus != NULL) {
		g_dbus_connection_unregister_object(bus, req->reg_id);
		req->reg_id = 0;
	}
	dialog_destroy();
	if (stream_timer != 0) {
		g_source_remove(stream_timer);
		stream_timer = 0;
	}

	if (response != RESPONSE_SUCCESS) {
		/* 取消 / 失败：把已经建起来的流收掉，别漏一路录屏。 */
		if (req->session != NULL) {
			session_streams_destroy(req->session);
		}
	}

	if (results == NULL) {
		GVariantBuilder empty;
		g_variant_builder_init(&empty, G_VARIANT_TYPE("a{sv}"));
		results = g_variant_builder_end(&empty);
	}
	if (req->invocation != NULL) {
		g_dbus_method_invocation_return_value(req->invocation,
			g_variant_new("(u@a{sv})", response, results));
	}

	if (pending == req) {
		pending = NULL;
	}
	free(req->path);
	free(req->parent_window);
	free(req);
}

/* 所有流都有结果了（成或败）→ 回 Start。 */
static void start_maybe_finish(void) {
	struct request *req = pending;
	if (req == NULL || req->done || req->session == NULL || !req->starting) {
		return;
	}
	struct session *s = req->session;
	if (s->streams->len == 0 || !stream_all_settled(s)) {
		return;
	}
	bool any = false;
	for (guint i = 0; i < s->streams->len; i++) {
		struct stream *st = g_ptr_array_index(s->streams, i);
		if (st->created) {
			any = true;
		}
	}
	if (!any) {
		log_info("没有一路流建成功，回失败");
		request_finish(req, RESPONSE_OTHER, NULL);
		return;
	}
	s->started = true;
	log_info("%s: Start 成功，%u 路流", s->app_id != NULL ? s->app_id : "?", s->streams->len);
	request_finish(req, RESPONSE_SUCCESS, start_results(s));
}

static gboolean stream_timeout(gpointer data) {
	(void)data;
	stream_timer = 0;
	if (pending != NULL && !pending->done) {
		log_info("等合成器建流超时");
		request_finish(pending, RESPONSE_OTHER, NULL);
	}
	return G_SOURCE_REMOVE;
}

/* 用户点了「共享」：给每个选中的输出要一路流。 */
static void dialog_accept(void) {
	struct request *req = pending;
	if (req == NULL || req->session == NULL) {
		return;
	}
	struct session *s = req->session;

	int wanted = 0;
	for (GList *it = outputs; it != NULL; it = it->next) {
		struct output *out = it->data;
		if (out->toggle == NULL ||
				!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(out->toggle))) {
			continue;
		}
		wanted++;
	}
	if (wanted == 0) {
		log_info("没选任何输出，不放行");
		return;
	}
	if (zkde == NULL) {
		log_info("合成器没有 zkde_screencast_unstable_v1");
		request_finish(req, RESPONSE_OTHER, NULL);
		return;
	}

	gtk_widget_hide(dialog);
	preview_stop();

	uint32_t pointer = s->cursor_mode == 2 ? ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED
		: ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_HIDDEN;
	for (GList *it = outputs; it != NULL; it = it->next) {
		struct output *out = it->data;
		if (out->toggle == NULL ||
				!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(out->toggle))) {
			continue;
		}
		struct stream *st = calloc(1, sizeof(*st));
		if (st == NULL) {
			continue;
		}
		st->out = out;
		st->obj = zkde_screencast_unstable_v1_stream_output(zkde, out->wl, pointer);
		if (st->obj == NULL) {
			free(st);
			continue;
		}
		zkde_screencast_stream_unstable_v1_add_listener(st->obj, &stream_listener, st);
		g_ptr_array_add(s->streams, st);
		log_info("%s: 要一路流（cursor=%s）", out->name,
			s->cursor_mode == 2 ? "embedded" : "hidden");
	}
	wl_display_flush(wl);

	if (s->streams->len == 0) {
		request_finish(req, RESPONSE_OTHER, NULL);
		return;
	}
	req->starting = true;
	stream_timer = g_timeout_add(STREAM_TIMEOUT_MS, stream_timeout, NULL);
	start_maybe_finish();
}

static void dialog_cancel(void) {
	if (pending != NULL) {
		log_info("用户取消");
		request_finish(pending, RESPONSE_CANCELLED, NULL);
	}
}

static void on_share_clicked(GtkButton *button, gpointer data) {
	(void)button;
	(void)data;
	dialog_accept();
}

static void on_cancel_clicked(GtkButton *button, gpointer data) {
	(void)button;
	(void)data;
	dialog_cancel();
}

static gboolean on_dialog_key(GtkWidget *widget, GdkEventKey *event, gpointer data) {
	(void)widget;
	(void)data;
	if (event->keyval == GDK_KEY_Escape) {
		dialog_cancel();
		return TRUE;
	}
	if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
		dialog_accept();
		return TRUE;
	}
	return FALSE;
}

static void on_dialog_destroy(GtkWidget *widget, gpointer data) {
	(void)widget;
	(void)data;
	/* 被合成器关掉（或者我们自己 destroy）时也要把请求收干净。 */
	if (pending != NULL && !pending->done) {
		dialog_cancel();
	}
}

static void on_toggle(GtkToggleButton *button, gpointer data) {
	(void)button;
	(void)data;
	/* 也当「刷新按钮可用性」用，所以允许不带按钮调（见 dialog_show 末尾）。 */
	int n = 0;
	for (GList *it = outputs; it != NULL; it = it->next) {
		struct output *out = it->data;
		if (out->toggle != NULL &&
				gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(out->toggle))) {
			n++;
		}
	}
	if (share_button != NULL) {
		gtk_widget_set_sensitive(share_button, n > 0);
	}
}

/* 弹对话框。多选（multiple）时用复选，否则用单选。 */
static void dialog_show(struct request *req) {
	struct session *s = req->session;
	GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), "共享屏幕");
	gtk_window_set_default_size(GTK_WINDOW(win), 720, 560);
	gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER_ALWAYS);
	gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DIALOG);
	gtk_container_set_border_width(GTK_CONTAINER(win), 12);

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_container_add(GTK_CONTAINER(win), box);

	char title[256];
	snprintf(title, sizeof(title), "%s 想要录屏，选一块屏幕：",
		s->app_id != NULL && s->app_id[0] != '\0' ? s->app_id : "某个应用");
	GtkWidget *label = gtk_label_new(title);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

	GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
		GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	gtk_container_add(GTK_CONTAINER(scroll), grid);

	GSList *group = NULL;
	int row = 0;
	int first = 1;
	for (GList *it = outputs; it != NULL; it = it->next, row++) {
		struct output *out = it->data;

		GtkWidget *image = gtk_image_new();
		gtk_widget_set_size_request(image, THUMB_W, THUMB_H);
		gtk_grid_attach(GTK_GRID(grid), image, 0, row, 1, 1);
		out->image = image;

		char text[128];
		snprintf(text, sizeof(text), "%s\n%d × %d",
			out->name[0] != '\0' ? out->name : "输出", out->w, out->h);
		GtkWidget *toggle = gtk_toggle_button_new_with_label(text);
		gtk_widget_set_size_request(toggle, 220, -1);
		/* 第一块屏默认选中：KDE 的选择器也是这样，而且「共享」按钮在没选东西时
		 * 是灰的 —— 进来就灰着等于让人多点一下。 */
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), first ? TRUE : FALSE);
		(void)group;
		g_signal_connect(toggle, "toggled", G_CALLBACK(on_toggle), NULL);
		gtk_grid_attach(GTK_GRID(grid), toggle, 1, row, 1, 1);
		out->toggle = toggle;
		first = 0;
	}

	GtkWidget *buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_button_box_set_layout(GTK_BUTTON_BOX(buttons), GTK_BUTTONBOX_END);
	gtk_box_set_spacing(GTK_BOX(buttons), 8);
	gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);

	GtkWidget *cancel = gtk_button_new_with_label("取消");
	GtkWidget *share = gtk_button_new_with_label("共享");
	gtk_widget_set_sensitive(share, FALSE);
	gtk_container_add(GTK_CONTAINER(buttons), cancel);
	gtk_container_add(GTK_CONTAINER(buttons), share);
	share_button = share;

	g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel_clicked), NULL);
	g_signal_connect(share, "clicked", G_CALLBACK(on_share_clicked), NULL);
	g_signal_connect(win, "key-press-event", G_CALLBACK(on_dialog_key), NULL);
	g_signal_connect(win, "destroy", G_CALLBACK(on_dialog_destroy), NULL);

	dialog = win;
	gtk_widget_show_all(win);
	on_toggle(NULL, NULL);

	/* 挂到发起请求的窗口上（xdg-foreign）。父窗口在别的连接上，GDK 自己 import。 */
	if (req->parent_window != NULL && g_str_has_prefix(req->parent_window, "wayland:")) {
		GdkWindow *gdkwin = gtk_widget_get_window(win);
		if (gdkwin != NULL && GDK_IS_WAYLAND_WINDOW(gdkwin)) {
			gboolean ok = gdk_wayland_window_set_transient_for_exported(gdkwin,
				req->parent_window + strlen("wayland:"));
			log_info("父窗口 %s：%s", req->parent_window, ok ? "挂上了" : "挂不上");
		}
	}

	preview_timer = g_timeout_add(PREVIEW_MS, preview_tick, NULL);
	preview_tick(NULL);
}

/* ------------------------------------------------------------ Session / Request */

static const char session_xml[] =
	"<node>"
	" <interface name='org.freedesktop.impl.portal.Session'>"
	"  <method name='Close'/>"
	"  <signal name='Closed'/>"
	" </interface>"
	"</node>";

static void session_method_call(GDBusConnection *conn, const gchar *sender,
		const gchar *object_path, const gchar *interface_name, const gchar *method_name,
		GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data) {
	(void)conn;
	(void)sender;
	(void)object_path;
	(void)interface_name;
	(void)parameters;
	struct session *s = user_data;
	if (g_strcmp0(method_name, "Close") == 0) {
		session_close(s, "客户端关了会话");
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}
	g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
		G_DBUS_ERROR_UNKNOWN_METHOD, "没有 %s 这个方法", method_name);
}

static const GDBusInterfaceVTable session_vtable = {
	.method_call = session_method_call,
};

static const char request_xml[] =
	"<node>"
	" <interface name='org.freedesktop.impl.portal.Request'>"
	"  <method name='Close'/>"
	" </interface>"
	"</node>";

static void request_method_call(GDBusConnection *conn, const gchar *sender,
		const gchar *object_path, const gchar *interface_name, const gchar *method_name,
		GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data) {
	(void)conn;
	(void)sender;
	(void)object_path;
	(void)interface_name;
	(void)parameters;
	(void)user_data;
	if (g_strcmp0(method_name, "Close") == 0) {
		g_dbus_method_invocation_return_value(invocation, NULL);
		if (pending != NULL && !pending->done) {
			log_info("客户端撤回了请求");
			request_finish(pending, RESPONSE_CANCELLED, NULL);
		}
		return;
	}
	g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
		G_DBUS_ERROR_UNKNOWN_METHOD, "没有 %s 这个方法", method_name);
}

static const GDBusInterfaceVTable request_vtable = {
	.method_call = request_method_call,
};

static void session_close(struct session *s, const char *why) {
	if (s == NULL) {
		return;
	}
	if (pending != NULL && pending->session == s && !pending->done) {
		request_finish(pending, RESPONSE_CANCELLED, NULL);
	}
	if (s->started) {
		/* 会话真开过 → 告诉前端它没了（前端再转告客户端）。 */
		GVariant *sig = g_variant_new("()");
		g_dbus_connection_emit_signal(bus, NULL, s->path,
			"org.freedesktop.impl.portal.Session", "Closed", sig, NULL);
	}
	session_streams_destroy(s);
	log_info("会话 %s 结束（%s）", s->path, why);
	s->closed = true;
}

/* ------------------------------------------------------------ impl ScreenCast */

static const char screencast_xml[] =
	"<node>"
	" <interface name='org.freedesktop.impl.portal.ScreenCast'>"
	"  <method name='CreateSession'>"
	"   <arg type='o' name='handle' direction='in'/>"
	"   <arg type='o' name='session_handle' direction='in'/>"
	"   <arg type='s' name='app_id' direction='in'/>"
	"   <arg type='a{sv}' name='options' direction='in'/>"
	"   <arg type='u' name='response' direction='out'/>"
	"   <arg type='a{sv}' name='results' direction='out'/>"
	"  </method>"
	"  <method name='SelectSources'>"
	"   <arg type='o' name='handle' direction='in'/>"
	"   <arg type='o' name='session_handle' direction='in'/>"
	"   <arg type='s' name='app_id' direction='in'/>"
	"   <arg type='a{sv}' name='options' direction='in'/>"
	"   <arg type='u' name='response' direction='out'/>"
	"   <arg type='a{sv}' name='results' direction='out'/>"
	"  </method>"
	"  <method name='Start'>"
	"   <arg type='o' name='handle' direction='in'/>"
	"   <arg type='o' name='session_handle' direction='in'/>"
	"   <arg type='s' name='app_id' direction='in'/>"
	"   <arg type='s' name='parent_window' direction='in'/>"
	"   <arg type='a{sv}' name='options' direction='in'/>"
	"   <arg type='u' name='response' direction='out'/>"
	"   <arg type='a{sv}' name='results' direction='out'/>"
	"  </method>"
	"  <property name='AvailableSourceTypes' type='u' access='read'/>"
	"  <property name='AvailableCursorModes' type='u' access='read'/>"
	"  <property name='version' type='u' access='read'/>"
	" </interface>"
	"</node>";

/* 每会话一份：session_handle 是前端给的路径（/org/freedesktop/portal/desktop/session/...）。
 * 前端拿 app_id 记权限，我们只存着打日志 / 给对话框用。 */
static GHashTable *sessions; /* path -> struct session * */

static GVariant *empty_results(void) {
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	return g_variant_builder_end(&b);
}

static void on_create_session(GDBusConnection *conn, const gchar *sender,
		const gchar *object_path, const gchar *interface_name, const gchar *method_name,
		GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data) {
	(void)conn;
	(void)sender;
	(void)object_path;
	(void)interface_name;
	(void)method_name;
	(void)user_data;

	const char *handle = NULL, *session_path = NULL, *app_id = NULL;
	GVariant *options = NULL;
	g_variant_get(parameters, "(&o&o&s@a{sv})", &handle, &session_path, &app_id, &options);
	g_variant_unref(options);

	struct session *s = calloc(1, sizeof(*s));
	if (s == NULL) {
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(u@a{sv})", RESPONSE_OTHER, empty_results()));
		return;
	}
	s->path = strdup(session_path);
	s->app_id = strdup(app_id);
	s->types = SOURCE_TYPE_MONITOR;
	s->cursor_mode = 1; /* 默认 hidden，和接口文档一致 */
	s->streams = g_ptr_array_new();

	GError *error = NULL;
	GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(session_xml, &error);
	if (info == NULL) {
		log_info("session XML 坏了: %s", error->message);
		g_error_free(error);
		session_free(s);
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(u@a{sv})", RESPONSE_OTHER, empty_results()));
		return;
	}
	s->reg_id = g_dbus_connection_register_object(bus, session_path,
		info->interfaces[0], &session_vtable, s, NULL, &error);
	g_dbus_node_info_unref(info);
	if (s->reg_id == 0) {
		log_info("注册会话对象失败: %s", error != NULL ? error->message : "?");
		if (error != NULL) {
			g_error_free(error);
		}
		session_free(s);
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(u@a{sv})", RESPONSE_OTHER, empty_results()));
		return;
	}
	g_hash_table_insert(sessions, g_strdup(session_path), s);

	GVariantBuilder results;
	g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&results, "{sv}", "session_id", g_variant_new_string("vidow"));
	log_info("%s: CreateSession", app_id);
	g_dbus_method_invocation_return_value(invocation,
		g_variant_new("(u@a{sv})", RESPONSE_SUCCESS, g_variant_builder_end(&results)));
}

static void on_select_sources(GDBusConnection *conn, const gchar *sender,
		const gchar *object_path, const gchar *interface_name, const gchar *method_name,
		GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data) {
	(void)conn;
	(void)sender;
	(void)object_path;
	(void)interface_name;
	(void)method_name;
	(void)user_data;

	const char *handle = NULL, *session_path = NULL, *app_id = NULL;
	GVariant *options = NULL;
	g_variant_get(parameters, "(&o&o&s@a{sv})", &handle, &session_path, &app_id, &options);

	struct session *s = g_hash_table_lookup(sessions, session_path);
	uint32_t types = SOURCE_TYPE_MONITOR;
	bool multiple = false;
	uint32_t cursor = 1;
	if (options != NULL) {
		GVariant *v = g_variant_lookup_value(options, "types", G_VARIANT_TYPE_UINT32);
		if (v != NULL) {
			types = g_variant_get_uint32(v);
			g_variant_unref(v);
		}
		v = g_variant_lookup_value(options, "multiple", G_VARIANT_TYPE_BOOLEAN);
		if (v != NULL) {
			multiple = g_variant_get_boolean(v);
			g_variant_unref(v);
		}
		v = g_variant_lookup_value(options, "cursor_mode", G_VARIANT_TYPE_UINT32);
		if (v != NULL) {
			cursor = g_variant_get_uint32(v);
			g_variant_unref(v);
		}
		g_variant_unref(options);
	}
	if (s != NULL) {
		s->types = types;
		s->multiple = multiple;
		s->cursor_mode = cursor;
	}
	log_info("%s: SelectSources types=0x%x multiple=%d cursor=%u",
		app_id, types, multiple ? 1 : 0, cursor);
	g_dbus_method_invocation_return_value(invocation,
		g_variant_new("(u@a{sv})", RESPONSE_SUCCESS, empty_results()));
}

static void on_start(GDBusConnection *conn, const gchar *sender,
		const gchar *object_path, const gchar *interface_name, const gchar *method_name,
		GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data) {
	(void)conn;
	(void)sender;
	(void)object_path;
	(void)interface_name;
	(void)method_name;
	(void)user_data;

	const char *handle = NULL, *session_path = NULL, *app_id = NULL, *parent = NULL;
	GVariant *options = NULL;
	g_variant_get(parameters, "(&o&o&s&s@a{sv})", &handle, &session_path, &app_id, &parent,
		&options);
	if (options != NULL) {
		g_variant_unref(options);
	}

	struct session *s = g_hash_table_lookup(sessions, session_path);
	if (s == NULL) {
		log_info("%s: Start 但会话 %s 不认识", app_id, session_path);
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(u@a{sv})", RESPONSE_OTHER, empty_results()));
		return;
	}

	if (pending != NULL && !pending->done) {
		/* 一次只弹一个对话框。KDE 会排队，这里先直接拒 —— 客户端会重试。 */
		log_info("%s: 已经有一个对话框开着，先拒了", app_id);
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(u@a{sv})", RESPONSE_OTHER, empty_results()));
		return;
	}

	struct request *req = calloc(1, sizeof(*req));
	if (req == NULL) {
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(u@a{sv})", RESPONSE_OTHER, empty_results()));
		return;
	}
	req->path = strdup(handle);
	req->session = s;
	req->invocation = invocation;
	req->parent_window = strdup(parent != NULL ? parent : "");

	GError *error = NULL;
	GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(request_xml, &error);
	if (info != NULL) {
		req->reg_id = g_dbus_connection_register_object(bus, handle,
			info->interfaces[0], &request_vtable, req, NULL, &error);
		g_dbus_node_info_unref(info);
	} else {
		log_info("request XML 坏了: %s", error->message);
		g_error_free(error);
	}
	if (req->reg_id == 0) {
		log_info("注册请求对象失败（%s），照常弹对话框", handle);
	}

	pending = req;
	log_info("%s: Start，弹选择器（parent=%s）", app_id,
		req->parent_window[0] != '\0' ? req->parent_window : "无");

	if (g_list_length(outputs) == 0) {
		log_info("一块输出都没有，没法选");
		request_finish(req, RESPONSE_OTHER, NULL);
		return;
	}
	dialog_show(req);
}

/* GDBus 一个对象一个 vtable，三个方法在这里分发。 */
static void screencast_method_call(GDBusConnection *conn, const gchar *sender,
		const gchar *object_path, const gchar *interface_name, const gchar *method_name,
		GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data) {
	if (g_strcmp0(method_name, "CreateSession") == 0) {
		on_create_session(conn, sender, object_path, interface_name, method_name,
			parameters, invocation, user_data);
	} else if (g_strcmp0(method_name, "SelectSources") == 0) {
		on_select_sources(conn, sender, object_path, interface_name, method_name,
			parameters, invocation, user_data);
	} else if (g_strcmp0(method_name, "Start") == 0) {
		on_start(conn, sender, object_path, interface_name, method_name,
			parameters, invocation, user_data);
	} else {
		g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
			G_DBUS_ERROR_UNKNOWN_METHOD, "没有 %s 这个方法", method_name);
	}
}

static GVariant *screencast_get_property(GDBusConnection *conn, const gchar *sender,
		const gchar *object_path, const gchar *interface_name, const gchar *property_name,
		GError **error, gpointer user_data) {
	(void)conn;
	(void)sender;
	(void)object_path;
	(void)interface_name;
	(void)user_data;
	(void)error;
	if (g_strcmp0(property_name, "AvailableSourceTypes") == 0) {
		return g_variant_new_uint32(SOURCE_TYPE_MONITOR);
	}
	if (g_strcmp0(property_name, "AvailableCursorModes") == 0) {
		return g_variant_new_uint32(CURSOR_MODES);
	}
	if (g_strcmp0(property_name, "version") == 0) {
		return g_variant_new_uint32(SC_VERSION);
	}
	return NULL;
}

static const GDBusInterfaceVTable screencast_vtable = {
	.method_call = screencast_method_call,
	.get_property = screencast_get_property,
};

/* ------------------------------------------------------------ 总线 */

static void on_bus_acquired(GDBusConnection *conn, const gchar *name, gpointer data) {
	(void)name;
	(void)data;
	bus = conn;
	GError *error = NULL;
	GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(screencast_xml, &error);
	if (info == NULL) {
		log_info("ScreenCast XML 坏了: %s", error->message);
		exit(1);
	}
	guint id = g_dbus_connection_register_object(conn, OBJECT_PATH, info->interfaces[0],
		&screencast_vtable, NULL, NULL, &error);
	g_dbus_node_info_unref(info);
	if (id == 0) {
		log_info("注册 ScreenCast 失败: %s", error != NULL ? error->message : "?");
		exit(1);
	}
	log_info("已占住 %s", BUS_NAME);
}

static void on_name_lost(GDBusConnection *conn, const gchar *name, gpointer data) {
	(void)data;
	if (conn == NULL) {
		log_info("连不上会话总线，退");
	} else {
		log_info("%s 这个名字被别人占了，退", name);
	}
	exit(1);
}

/* ------------------------------------------------------------ wayland 事件 */

static gboolean wl_ready(GIOChannel *source, GIOCondition cond, gpointer data) {
	(void)source;
	(void)data;
	if ((cond & (G_IO_ERR | G_IO_HUP)) != 0) {
		log_info("合成器断了，退");
		gtk_main_quit();
		return FALSE;
	}
	if (wl_display_dispatch(wl) < 0) {
		log_info("wayland 连接出错，退");
		gtk_main_quit();
		return FALSE;
	}
	/* 合成器那边刚给了流的结果（created / failed）→ 可能可以把 Start 回了。 */
	start_maybe_finish();
	return TRUE;
}

static void wl_watch(void) {
	GIOChannel *ch = g_io_channel_unix_new(wl_display_get_fd(wl));
	g_io_channel_set_encoding(ch, NULL, NULL);
	g_io_add_watch(ch, G_IO_IN | G_IO_ERR | G_IO_HUP, wl_ready, NULL);
	g_io_channel_unref(ch);
	wl_display_flush(wl);
}

/* ------------------------------------------------------------ 调试：直接弹选择器 */

/* 不经过 DBus 把选择器弹出来（看预览、看排版、点「共享」建流）。
 * 没有前端就没有真正的 Start 回复，点「共享」只会建流 + 打日志。 */
static void debug_dialog(void) {
	struct session *s = calloc(1, sizeof(*s));
	struct request *req = calloc(1, sizeof(*req));
	if (s == NULL || req == NULL) {
		exit(1);
	}
	s->path = strdup("/debug");
	s->app_id = strdup("debug");
	s->types = SOURCE_TYPE_MONITOR;
	s->cursor_mode = 1;
	s->streams = g_ptr_array_new();
	req->path = strdup("/debug/request");
	req->session = s;
	req->invocation = NULL;
	req->parent_window = strdup("");
	pending = req;
	dialog_show(req);
}

/* ------------------------------------------------------------ 自检 */

static int probe(void) {
	wl = wl_display_connect(NULL);
	if (wl == NULL) {
		log_info("连不上合成器（WAYLAND_DISPLAY=%s）",
			getenv("WAYLAND_DISPLAY") != NULL ? getenv("WAYLAND_DISPLAY") : "(没设)");
		return 1;
	}
	registry = wl_display_get_registry(wl);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(wl);
	wl_display_roundtrip(wl);

	log_info("wl_shm=%s screencopy=%s zkde_screencast=%s 输出=%u",
		shm != NULL ? "有" : "没有",
		scmgr != NULL ? "有" : "没有",
		zkde != NULL ? "有" : "没有",
		g_list_length(outputs));
	for (GList *it = outputs; it != NULL; it = it->next) {
		struct output *out = it->data;
		log_info("  %s %dx%d @ %d,%d scale=%d",
			out->name[0] != '\0' ? out->name : "(没名字)",
			out->w, out->h, out->x, out->y, out->scale);
	}
	bool ok = shm != NULL && scmgr != NULL && zkde != NULL && outputs != NULL;
	return ok ? 0 : 1;
}

/* ------------------------------------------------------------ main */

int main(int argc, char **argv) {
	if (argc > 1 && strcmp(argv[1], "--probe") == 0) {
		return probe();
	}
	bool want_dialog = argc > 1 && strcmp(argv[1], "--dialog") == 0;

	/* 名字决定窗口的 app_id（vidow 那边靠它认这个对话框）。 */
	g_set_prgname("vidow-portal");
	g_set_application_name("vidow-portal");
	/* 必须走 wayland：窗口要挂到请求方的窗口上（xdg-foreign），也必须在 vidow 上。 */
	g_setenv("GDK_BACKEND", "wayland", TRUE);

	wl = wl_display_connect(NULL);
	if (wl == NULL) {
		log_info("连不上合成器，退");
		return 1;
	}
	registry = wl_display_get_registry(wl);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(wl);
	wl_display_roundtrip(wl);
	if (shm == NULL || scmgr == NULL || zkde == NULL) {
		log_info("合成器缺东西：shm=%d screencopy=%d zkde=%d", shm != NULL,
			scmgr != NULL, zkde != NULL);
		return 1;
	}
	log_info("连上合成器：%u 块输出", g_list_length(outputs));

	sessions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)session_free);

	gtk_init(&argc, &argv);
	wl_watch();

	if (want_dialog) {
		debug_dialog();
		gtk_main();
		return 0;
	}

	g_bus_own_name(G_BUS_TYPE_SESSION, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
		on_bus_acquired, NULL, on_name_lost, NULL, NULL);

	gtk_main();
	return 0;
}
