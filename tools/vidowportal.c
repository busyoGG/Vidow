/* vidowportal.c — vidow 自己的 xdg-desktop-portal 后端（ScreenCast + Screenshot）
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
 *     Interfaces=org.freedesktop.impl.portal.ScreenCast;org.freedesktop.impl.portal.Screenshot;
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
 * KDE 的后端是这么用的，这里也这么用，只是客户端换成了自己人。
 *
 * 四种来源（和 KDE 的选择器一样，按前端给的 types 分组显示）：
 *   屏幕      每块输出一行，实时缩略图（zwlr_screencopy）
 *   窗口      每个窗口一行（ext-foreign-toplevel-list 拿列表，实时缩略图走
 *             ext-image-copy-capture 的窗口源），选中后 stream_window(identifier)
 *   虚拟屏幕  一行「新建一块虚拟屏幕」，合成器会现加一块 headless 输出
 *   区域      前端不通过 ScreenCast 要区域（Screenshot 的交互式截图才用），
 *             所以这里不显示，走 stream_region 的是截图那条路
 *
 * 对话框：GTK3（和 xdg-desktop-portal-gtk 同一个套路），挂到发起请求的窗口上
 * （xdg-foreign，前端给的 parent_window）。
 *
 * 截图（Screenshot / PickColor）不经过合成器的 PipeWire 那一套：后端自己就是客户端，
 * 抓一帧像素、写个 PNG、把文件 URI 回给前端。交互式截图 / 取色把那一帧冻住铺满屏，
 * 让用户框一块 / 点一下。
 *
 * 用法（一般不用手敲，vidow 会自己起）：
 *   vidow-portal            # 占住总线名，等前端来调
 *   vidow-portal --probe    # 自检：连上合成器、列一遍输出和窗口、退出
 *   vidow-portal --dialog   # 不经过 DBus 直接把选择器弹出来（看排版 / 点着玩）
 *   vidow-portal --shot     # 不经过 DBus 直接跑一次交互式截图
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
#include <time.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <gdk/gdkwayland.h>
#include <png.h>

#include <wayland-client.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
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

/* AvailableSourceTypes：屏幕 / 窗口 / 虚拟屏幕都做了（和 KDE 一样报 7）。 */
#define SOURCE_TYPE_MONITOR 1u
#define SOURCE_TYPE_WINDOW 2u
#define SOURCE_TYPE_VIRTUAL 4u
#define SOURCE_TYPES (SOURCE_TYPE_MONITOR | SOURCE_TYPE_WINDOW | SOURCE_TYPE_VIRTUAL)
/* AvailableCursorModes：hidden=1 | embedded=2。metadata=4 合成器那边还没实现。 */
#define CURSOR_MODES (1u | 2u)
/* impl ScreenCast 的接口版本：2 = 有 cursor_mode，没有 restore_data / persist_mode。 */
#define SC_VERSION 2u

/* 一行（对话框里一个可选项）。 */
enum row_kind {
	ROW_OUTPUT = 0,
	ROW_WINDOW,
	ROW_VIRTUAL,
};

struct row {
	enum row_kind kind;
	void *ptr;      /* ROW_OUTPUT: struct output *，ROW_WINDOW: struct toplevel * */
	int vw, vh;     /* ROW_VIRTUAL：要建的虚拟屏幕逻辑尺寸 */
	GtkWidget *image;
	GtkWidget *toggle;
};

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
	/* 报给前端的 position / size / source_type（按这一路录的是什么填）。 */
	int x, y, w, h;
	char label[96];
	uint32_t source_type;
	uint32_t node;
	bool created;
	bool failed;
};

/* 一个窗口（ext-foreign-toplevel-list-v1 报的）。identifier 是录屏时回给合成器的
 * 「这个窗口是谁」（vidow 拿它反查自己的 view，见 screencast.c 的
 * view_by_identifier）。 */
struct toplevel {
	struct ext_foreign_toplevel_handle_v1 *handle;
	char *identifier;
	char *title;
	char *app_id;
	bool done;
	struct win_shot *shot; /* 这个窗口的抓帧会话（缩略图用，常驻复用） */
	/* 最近一张缩略图的像素尺寸（报给前端的 size 用：窗口列表协议里没有几何，
	 * 抓帧会话才知道窗口多大）。没抓到过就是 0。 */
	int w, h;
};

/* 一个窗口的抓帧会话（缩略图用它）。
 *
 * **一个窗口一个会话、反复要帧**，不是「每抓一张就新建一个会话」：合成器那边
 * 每个窗口抓帧源都会配一块 scene_output（wlroots 的窗口抓帧源就是这么实现的），
 * 而一个场景最多 64 块 —— 每 500ms 建一个新会话的话，半分钟就把上限撞爆，
 * wlroots 当场断言（踩过：`wlr_scene_output_create: Assertion scene_output->index < 64
 * failed`，vidow 直接崩，用户的会话跟着没了）。 */
struct win_shot {
	struct toplevel *tl;
	struct ext_image_capture_source_v1 *source;
	struct ext_image_copy_capture_session_v1 *session;
	struct ext_image_copy_capture_frame_v1 *frame;
	struct wl_buffer *buffer;
	void *map;
	size_t map_size;
	int fd;
	int w, h, stride;
	uint32_t format;
	bool have_size, have_format;
	bool ready;   /* 约束齐了（buffer_size + shm_format），可以要帧了 */
	bool busy;    /* 手上有一帧还没回来 */
	bool failed;  /* 会话停了（窗口关了 / 合成器不给），下次重建 */
	void (*on_pixbuf)(GdkPixbuf *pix, void *user);
	void *user;
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
static struct ext_foreign_toplevel_list_v1 *tlist;
static struct ext_foreign_toplevel_image_capture_source_manager_v1 *win_src_mgr;
static struct ext_image_copy_capture_manager_v1 *cap_mgr;
static GList *outputs; /* struct output * */
static GList *toplevels; /* struct toplevel * */
static GList *rows;    /* struct row *：对话框里当前排着的行 */
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

/* shm 像素 → GdkPixbuf。缩略图 / 窗口抓帧 / 截图三条路都走它。
 *
 * 格式值有两套：0 / 1 是 wl_shm 的协议特例（ARGB8888 / XRGB8888），其余的
 * **就是 DRM fourcc**（wl_shm 允许把 fourcc 当格式值报）。vidow 在 GLES2 上
 * 报的是 XBGR8888（0x34324258，屏幕缓冲本来就是它），pixman 上是 XRGB8888。
 *
 * 两套的内存字节序不一样，搞错了就是红蓝互换：
 *   XRGB8888 / ARGB8888 → 内存里是 B,G,R,X（小端）
 *   XBGR8888 / ABGR8888 → 内存里是 R,G,B,X
 * 只认前两个的话，GLES2 会话里缩略图和截图全是蓝红颠倒的（踩过）。 */
#define DRM_FORMAT_ARGB8888 0x34325241u
#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_FORMAT_ABGR8888 0x34324241u
#define DRM_FORMAT_XBGR8888 0x34324258u

/* 这个格式能不能当「每像素 4 字节的 8888」处理？*rgb_order = 内存里是不是
 * R,G,B 的顺序。不认识的格式（565 / 10bit）返回 false。 */
static bool shm_format_8888(uint32_t format, bool *rgb_order) {
	switch (format) {
	case WL_SHM_FORMAT_ARGB8888:
	case WL_SHM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
		*rgb_order = false;
		return true;
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
		*rgb_order = true;
		return true;
	default:
		return false;
	}
}

static GdkPixbuf *pixbuf_from_shm(void *map, int w, int h, int stride,
		uint32_t format, bool y_invert) {
	bool rgb_order = false;
	if (map == NULL || w < 1 || h < 1) {
		return NULL;
	}
	if (!shm_format_8888(format, &rgb_order)) {
		log_info("抓帧：不认识的 shm 格式 %u（只认 8888 那一档）", format);
		return NULL;
	}
	GdkPixbuf *pix = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
	if (pix == NULL) {
		return NULL;
	}
	guchar *dst = gdk_pixbuf_get_pixels(pix);
	int dst_stride = gdk_pixbuf_get_rowstride(pix);
	for (int y = 0; y < h; y++) {
		int sy = y_invert ? (h - 1 - y) : y;
		const uint32_t *src = (const uint32_t *)((const uint8_t *)map +
			(size_t)sy * (size_t)stride);
		guchar *row = dst + (size_t)y * (size_t)dst_stride;
		for (int x = 0; x < w; x++) {
			uint32_t p = src[x];
			row[x * 3 + 0] = (guchar)(rgb_order ? (p & 0xff) : ((p >> 16) & 0xff));
			row[x * 3 + 1] = (guchar)((p >> 8) & 0xff);
			row[x * 3 + 2] = (guchar)(rgb_order ? ((p >> 16) & 0xff) : (p & 0xff));
		}
	}
	return pix;
}

/* 输出的缩略图缓冲 → pixbuf。 */
static GdkPixbuf *output_to_pixbuf(struct output *out) {
	return pixbuf_from_shm(out->map, out->bw, out->bh, out->stride, out->format,
		out->y_invert);
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

/* ------------------------------------------------- 窗口抓帧（缩略图 / 截窗口）
 *
 * 走 ext-image-copy-capture-v1 的**窗口源**：合成器把那个窗口的场景子树渲染成
 * 一帧，我们拷进自己的 shm 缓冲。vidow 那边这个源是 wlroots 那份（见
 * src/capture.c 的说明），vidow-portal 就是个普通客户端。
 */

static void win_shot_release(struct win_shot *s) {
	if (s == NULL) {
		return;
	}
	if (s->tl != NULL && s->tl->shot == s) {
		s->tl->shot = NULL;
	}
	if (s->frame != NULL) {
		ext_image_copy_capture_frame_v1_destroy(s->frame);
		s->frame = NULL;
	}
	if (s->session != NULL) {
		ext_image_copy_capture_session_v1_destroy(s->session);
		s->session = NULL;
	}
	if (s->source != NULL) {
		ext_image_capture_source_v1_destroy(s->source);
		s->source = NULL;
	}
	if (s->buffer != NULL) {
		wl_buffer_destroy(s->buffer);
		s->buffer = NULL;
	}
	if (s->map != NULL) {
		munmap(s->map, s->map_size);
		s->map = NULL;
	}
	if (s->fd >= 0) {
		close(s->fd);
		s->fd = -1;
	}
	free(s);
}

static void win_shot_request_frame(struct win_shot *s);

static void win_shot_frame_ready(void *data,
		struct ext_image_copy_capture_frame_v1 *frame) {
	struct win_shot *s = data;
	GdkPixbuf *pix = pixbuf_from_shm(s->map, s->w, s->h, s->stride, s->format,
		false);
	void (*cb)(GdkPixbuf *, void *) = s->on_pixbuf;
	void *user = s->user;
	s->on_pixbuf = NULL;
	s->user = NULL;
	s->busy = false;
	s->frame = NULL;
	ext_image_copy_capture_frame_v1_destroy(frame);
	if (cb != NULL) {
		cb(pix, user);
	}
	if (pix != NULL) {
		g_object_unref(pix);
	}
}

static void win_shot_frame_failed(void *data,
		struct ext_image_copy_capture_frame_v1 *frame, uint32_t reason) {
	struct win_shot *s = data;
	(void)reason;
	s->frame = NULL;
	s->busy = false;
	s->on_pixbuf = NULL;
	s->user = NULL;
	ext_image_copy_capture_frame_v1_destroy(frame);
}

static void win_shot_frame_transform(void *data,
		struct ext_image_copy_capture_frame_v1 *frame, uint32_t transform) {
	(void)data; (void)frame; (void)transform;
}

static void win_shot_frame_damage(void *data,
		struct ext_image_copy_capture_frame_v1 *frame, int32_t x, int32_t y,
		int32_t w, int32_t h) {
	(void)data; (void)frame; (void)x; (void)y; (void)w; (void)h;
}

static void win_shot_frame_time(void *data,
		struct ext_image_copy_capture_frame_v1 *frame, uint32_t sec_hi,
		uint32_t sec_lo, uint32_t nsec) {
	(void)data; (void)frame; (void)sec_hi; (void)sec_lo; (void)nsec;
}

static const struct ext_image_copy_capture_frame_v1_listener win_shot_frame_listener = {
	.transform = win_shot_frame_transform,
	.damage = win_shot_frame_damage,
	.presentation_time = win_shot_frame_time,
	.ready = win_shot_frame_ready,
	.failed = win_shot_frame_failed,
};

/* 要一帧（会话已经 ready、缓冲已经建好）。 */
static void win_shot_request_frame(struct win_shot *s) {
	if (s->session == NULL || s->buffer == NULL || s->frame != NULL) {
		return;
	}
	s->frame = ext_image_copy_capture_session_v1_create_frame(s->session);
	if (s->frame == NULL) {
		s->busy = false;
		return;
	}
	ext_image_copy_capture_frame_v1_add_listener(s->frame,
		&win_shot_frame_listener, s);
	ext_image_copy_capture_frame_v1_attach_buffer(s->frame, s->buffer);
	ext_image_copy_capture_frame_v1_damage_buffer(s->frame, 0, 0, s->w, s->h);
	ext_image_copy_capture_frame_v1_capture(s->frame);
	/* 立刻送出去：请求是在「收到事件」的回调里发的，不 flush 就等不到下一轮
	 * 事件（libwayland 只在下一次 dispatch 时 flush）。 */
	wl_display_flush(wl);
}

static void win_shot_session_done(void *data,
		struct ext_image_copy_capture_session_v1 *session) {
	struct win_shot *s = data;
	(void)session;
	if (!s->have_size || !s->have_format) {
		log_info("窗口抓帧：约束不齐（尺寸 %d，格式 %d）", s->have_size,
			s->have_format);
		s->failed = true;
		return;
	}
	if (s->buffer == NULL) {
		s->stride = s->w * 4;
		size_t size = (size_t)s->stride * (size_t)s->h;
		s->fd = create_shm_file(size);
		if (s->fd < 0) {
			s->failed = true;
			return;
		}
		s->map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
		if (s->map == MAP_FAILED) {
			s->map = NULL;
			s->failed = true;
			return;
		}
		s->map_size = size;
		struct wl_shm_pool *pool = wl_shm_create_pool(shm, s->fd, (int)size);
		s->buffer = wl_shm_pool_create_buffer(pool, 0, s->w, s->h, s->stride,
			s->format);
		wl_shm_pool_destroy(pool);
		if (s->buffer == NULL) {
			s->failed = true;
			return;
		}
	}
	s->ready = true;
	/* 有人在等图（对话框刚开、或者这一轮 tick）→ 马上抓一帧。 */
	if (s->on_pixbuf != NULL && !s->busy) {
		s->busy = true;
		win_shot_request_frame(s);
	}
}

static void win_shot_session_size(void *data,
		struct ext_image_copy_capture_session_v1 *session, uint32_t width,
		uint32_t height) {
	struct win_shot *s = data;
	(void)session;
	s->w = (int)width;
	s->h = (int)height;
	s->have_size = true;
}

static void win_shot_session_format(void *data,
		struct ext_image_copy_capture_session_v1 *session, uint32_t format) {
	struct win_shot *s = data;
	(void)session;
	bool rgb_order = false;
	if (shm_format_8888(format, &rgb_order)) {
		s->format = format;
		s->have_format = true;
	} else {
		log_info("窗口抓帧：合成器只给了 shm 格式 %u，不认识", format);
	}
}

static void win_shot_session_dmabuf_device(void *data,
		struct ext_image_copy_capture_session_v1 *session,
		struct wl_array *device) {
	(void)data; (void)session; (void)device;
}

static void win_shot_session_dmabuf_format(void *data,
		struct ext_image_copy_capture_session_v1 *session, uint32_t format,
		struct wl_array *modifiers) {
	(void)data; (void)session; (void)format; (void)modifiers;
}

static void win_shot_session_stopped(void *data,
		struct ext_image_copy_capture_session_v1 *session) {
	struct win_shot *s = data;
	(void)session;
	/* 窗口关了 / 合成器不给了：标成 failed，下次要用的时候重建。 */
	s->failed = true;
	s->ready = false;
	if (s->on_pixbuf != NULL) {
		void (*cb)(GdkPixbuf *, void *) = s->on_pixbuf;
		void *user = s->user;
		s->on_pixbuf = NULL;
		s->user = NULL;
		cb(NULL, user);
	}
}

static const struct ext_image_copy_capture_session_v1_listener win_shot_session_listener = {
	.buffer_size = win_shot_session_size,
	.shm_format = win_shot_session_format,
	.dmabuf_device = win_shot_session_dmabuf_device,
	.dmabuf_format = win_shot_session_dmabuf_format,
	.done = win_shot_session_done,
	.stopped = win_shot_session_stopped,
};

/* 给这个窗口要一帧画面（会话没有就建一个，之后一直复用）。cb 拿到 pixbuf
 * （可能 NULL）。 */
static void win_shot_capture(struct toplevel *tl,
		void (*cb)(GdkPixbuf *pix, void *user), void *user) {
	if (tl == NULL || tl->handle == NULL || cap_mgr == NULL ||
			win_src_mgr == NULL) {
		if (cb != NULL) {
			cb(NULL, user);
		}
		return;
	}
	struct win_shot *s = tl->shot;
	if (s == NULL || s->failed) {
		if (s != NULL) {
			win_shot_release(s);
		}
		s = calloc(1, sizeof(*s));
		if (s == NULL) {
			cb(NULL, user);
			return;
		}
		s->tl = tl;
		s->fd = -1;
		tl->shot = s;
		s->source =
			ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
				win_src_mgr, tl->handle);
		if (s->source == NULL) {
			win_shot_release(s);
			cb(NULL, user);
			return;
		}
		s->session = ext_image_copy_capture_manager_v1_create_session(cap_mgr,
			s->source, 0);
		if (s->session == NULL) {
			win_shot_release(s);
			cb(NULL, user);
			return;
		}
		ext_image_copy_capture_session_v1_add_listener(s->session,
			&win_shot_session_listener, s);
	}
	s->on_pixbuf = cb;
	s->user = user;
	if (!s->ready || s->busy) {
		/* 约束还没齐 / 上一帧还在路上：等 done 或 ready 里那一下。 */
		wl_display_flush(wl);
		return;
	}
	s->busy = true;
	win_shot_request_frame(s);
}

/* ------------------------------------------------------------ 窗口列表 */

static void toplevel_title(void *data, struct ext_foreign_toplevel_handle_v1 *h,
		const char *title) {
	struct toplevel *tl = data;
	(void)h;
	g_free(tl->title);
	tl->title = g_strdup(title != NULL ? title : "");
}

static void toplevel_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *h,
		const char *app_id) {
	struct toplevel *tl = data;
	(void)h;
	g_free(tl->app_id);
	tl->app_id = g_strdup(app_id != NULL ? app_id : "");
}

static void toplevel_identifier(void *data,
		struct ext_foreign_toplevel_handle_v1 *h, const char *identifier) {
	struct toplevel *tl = data;
	(void)h;
	g_free(tl->identifier);
	tl->identifier = g_strdup(identifier != NULL ? identifier : "");
}

static void toplevel_closed(void *data, struct ext_foreign_toplevel_handle_v1 *h) {
	struct toplevel *tl = data;
	(void)h;
	/* 会话会因为窗口没了收到 stopped；这里只标一下，下次别再要了。 */
	tl->handle = NULL;
}

static void toplevel_done(void *data, struct ext_foreign_toplevel_handle_v1 *h) {
	struct toplevel *tl = data;
	(void)h;
	tl->done = true;
}

static const struct ext_foreign_toplevel_handle_v1_listener toplevel_listener = {
	.title = toplevel_title,
	.app_id = toplevel_app_id,
	.identifier = toplevel_identifier,
	.closed = toplevel_closed,
	.done = toplevel_done,
};

static void list_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *list,
		struct ext_foreign_toplevel_handle_v1 *handle) {
	(void)data;
	(void)list;
	struct toplevel *tl = calloc(1, sizeof(*tl));
	if (tl == NULL) {
		return;
	}
	tl->handle = handle;
	tl->title = g_strdup("");
	tl->app_id = g_strdup("");
	tl->identifier = g_strdup("");
	ext_foreign_toplevel_handle_v1_add_listener(handle, &toplevel_listener, tl);
	toplevels = g_list_append(toplevels, tl);
}

static void list_finished(void *data, struct ext_foreign_toplevel_list_v1 *list) {
	(void)data;
	(void)list;
	/* 「合成器不再发事件了」（不是「枚举完了」—— 那个是每个 handle 的 done）。
	 * 这里不当成致命错误：没有窗口列表就只是选不了窗口。 */
	log_info("窗口列表结束（合成器收摊了）");
}

static const struct ext_foreign_toplevel_list_v1_listener list_listener = {
	.toplevel = list_toplevel,
	.finished = list_finished,
};

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
		/* 立刻送出去：请求是在「收到事件」的回调里发的，而 libwayland 只在
		 * 下一次 dispatch 时才会 flush —— 没有下一轮事件就发不出去，
		 * 合成器那边等于没收到（缩略图那条路靠 preview_tick 的 flush 兜着，
		 * 一次性抓帧没这个兜底，会卡死在这儿）。 */
		wl_display_flush(wl);
	} else {
		zwlr_screencopy_frame_v1_destroy(frame);
		out->frame = NULL;
		out->busy = false;
	}
}

/* zwlr_screencopy_frame_v1 一共有 7 个事件，**每一个都得有处理函数**：libwayland
 * 是按事件 opcode 从这张表里取函数指针的，取到 NULL 会当场 abort
 * （"listener function for opcode %u of %s is NULL"）。踩过：只填了 5 个，
 * 结果一开对话框、合成器发来 linux_dmabuf（GLES2 上必有）就把整个后端打崩了 ——
 * 之前测试全在 pixman 上跑（pixman 没有 dmabuf，那个事件不发），所以一直没露。 */
static void frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
	(void)data;
	(void)frame;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
}

static void frame_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t format, uint32_t width, uint32_t height) {
	/* 我们只走 shm 那条路（buffer 事件里建的），dmabuf 的就不理它。 */
	(void)data;
	(void)frame;
	(void)format;
	(void)width;
	(void)height;
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = frame_buffer,
	.flags = frame_flags,
	.ready = frame_ready,
	.failed = frame_failed,
	.damage = frame_damage,
	.linux_dmabuf = frame_linux_dmabuf,
	.buffer_done = frame_buffer_done,
};

/* 一行对应哪个窗口（缩略图回来了要找到那一行）。 */
static struct row *row_for_toplevel(struct toplevel *tl) {
	for (GList *it = rows; it != NULL; it = it->next) {
		struct row *row = it->data;
		if (row->kind == ROW_WINDOW && row->ptr == tl) {
			return row;
		}
	}
	return NULL;
}

/* 窗口缩略图回来了：塞进那一行，放开「这一轮在抓」的标记。 */
static void window_thumb_ready(GdkPixbuf *pix, void *user) {
	struct toplevel *tl = user;
	if (pix == NULL) {
		return;
	}
	tl->w = gdk_pixbuf_get_width(pix);
	tl->h = gdk_pixbuf_get_height(pix);
	struct row *row = row_for_toplevel(tl);
	if (row == NULL || row->image == NULL) {
		return;
	}
	GdkPixbuf *thumb = thumb_of(pix);
	if (thumb != NULL) {
		gtk_image_set_from_pixbuf(GTK_IMAGE(row->image), thumb);
		g_object_unref(thumb);
	}
}

/* 每 500ms 给每个输出 / 窗口要一帧缩略图。一次只抓一帧（screencopy 和窗口抓帧
 * 都是一次性的），抓完再抓下一帧。 */
static gboolean preview_tick(gpointer data) {
	(void)data;
	for (GList *it = rows; it != NULL; it = it->next) {
		struct row *row = it->data;
		if (row->image == NULL) {
			continue;
		}
		if (row->kind == ROW_OUTPUT) {
			struct output *out = row->ptr;
			if (scmgr == NULL || out->busy) {
				continue;
			}
			out->busy = true;
			out->frame = zwlr_screencopy_manager_v1_capture_output(scmgr, 0,
				out->wl);
			if (out->frame == NULL) {
				out->busy = false;
				continue;
			}
			zwlr_screencopy_frame_v1_add_listener(out->frame, &frame_listener,
				out);
		} else if (row->kind == ROW_WINDOW) {
			struct toplevel *tl = row->ptr;
			if (tl->handle == NULL) {
				continue;
			}
			win_shot_capture(tl, window_thumb_ready, tl);
		}
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
	} else if (strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
		tlist = wl_registry_bind(reg, name, &ext_foreign_toplevel_list_v1_interface, 1);
		/* 监听要在 bind 之后**立刻**挂上：toplevel 事件跟在这一轮的
		 * roundtrip 里就可能到，晚一步挂就一个窗口都收不到。 */
		ext_foreign_toplevel_list_v1_add_listener(tlist, &list_listener, NULL);
	} else if (strcmp(interface,
			ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) == 0) {
		win_src_mgr = wl_registry_bind(reg, name,
			&ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
	} else if (strcmp(interface,
			ext_image_copy_capture_manager_v1_interface.name) == 0) {
		cap_mgr = wl_registry_bind(reg, name,
			&ext_image_copy_capture_manager_v1_interface, 1);
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
	log_info("%s: 合成器关掉了这路流", st->label);
}

static void stream_failed(void *data, struct zkde_screencast_stream_unstable_v1 *obj,
		const char *message) {
	struct stream *st = data;
	(void)obj;
	st->failed = true;
	log_info("%s: 合成器拒绝了这路流（%s）", st->label,
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
	/* close 也是「在回调里发的请求」：不 flush 就发不出去（libwayland 只在下一次
	 * dispatch 时 flush，而这时候通常没有下一轮事件了）—— 合成器那边以为流还活着，
	 * 虚拟输出这种「跟着流存在」的东西就永远不拔。 */
	wl_display_flush(wl);
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
		g_variant_builder_add(&props, "{sv}", "position",
			g_variant_new("(ii)", st->x, st->y));
		g_variant_builder_add(&props, "{sv}", "size",
			g_variant_new("(ii)", st->w, st->h));
		g_variant_builder_add(&props, "{sv}", "source_type",
			g_variant_new_uint32(st->source_type));
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
	for (GList *it = toplevels; it != NULL; it = it->next) {
		struct toplevel *tl = it->data;
		if (tl->shot != NULL) {
			win_shot_release(tl->shot);
		}
	}
	g_list_free_full(rows, g_free);
	rows = NULL;
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
	/* 已经受理过了就别再来一遍：连点两下「共享」、或者回车敲两下，会建两路流
	 * （虚拟屏幕那种「现场加一块输出」的来源还会多出一块屏）。 */
	if (req->starting || req->done) {
		return;
	}
	struct session *s = req->session;

	int wanted = 0;
	for (GList *it = rows; it != NULL; it = it->next) {
		struct row *row = it->data;
		if (row->toggle != NULL &&
				gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(row->toggle))) {
			wanted++;
		}
	}
	if (wanted == 0) {
		log_info("没选任何东西，不放行");
		return;
	}
	if (zkde == NULL) {
		log_info("合成器没有 zkde_screencast_unstable_v1");
		request_finish(req, RESPONSE_OTHER, NULL);
		return;
	}

	gtk_widget_hide(dialog);
	preview_stop();

	uint32_t pointer = s->cursor_mode == 2
		? ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED
		: ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_HIDDEN;
	for (GList *it = rows; it != NULL; it = it->next) {
		struct row *row = it->data;
		if (row->toggle == NULL ||
				!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(row->toggle))) {
			continue;
		}
		struct stream *st = calloc(1, sizeof(*st));
		if (st == NULL) {
			continue;
		}
		switch (row->kind) {
		case ROW_OUTPUT: {
			struct output *out = row->ptr;
			st->obj = zkde_screencast_unstable_v1_stream_output(zkde, out->wl,
				pointer);
			st->x = out->x;
			st->y = out->y;
			st->w = out->w;
			st->h = out->h;
			st->source_type = SOURCE_TYPE_MONITOR;
			snprintf(st->label, sizeof(st->label), "%s", out->name);
			break;
		}
		case ROW_WINDOW: {
			struct toplevel *tl = row->ptr;
			if (tl->identifier == NULL || tl->identifier[0] == '\0') {
				log_info("窗口「%s」还没有 identifier，先跳过", tl->title);
				free(st);
				continue;
			}
			/* window_uuid = ext-foreign-toplevel-list 的 identifier：
			 * 合成器拿它反查自己的 view（screencast.c）。 */
			st->obj = zkde_screencast_unstable_v1_stream_window(zkde,
				tl->identifier, pointer);
			/* 尺寸报最近一张缩略图的大小（窗口列表协议里没有几何，只有
			 * 抓帧会话知道）；位置不知道就报 0。前端拿这两个数当元数据，
			 * 真正的大小看 PipeWire 协商出来的格式。 */
			st->w = tl->w;
			st->h = tl->h;
			st->source_type = SOURCE_TYPE_WINDOW;
			snprintf(st->label, sizeof(st->label), "窗口「%s」",
				tl->title != NULL && tl->title[0] != '\0' ? tl->title : "(没标题)");
			log_info("录窗口：identifier=%s（%s）", tl->identifier,
				tl->app_id != NULL ? tl->app_id : "");
			break;
		}
		case ROW_VIRTUAL: {
			char name[64];
			snprintf(name, sizeof(name), "vidow-virtual-%d", row->vw);
			st->obj = zkde_screencast_unstable_v1_stream_virtual_output(zkde,
				name, row->vw, row->vh, wl_fixed_from_int(1), pointer);
			st->w = row->vw;
			st->h = row->vh;
			st->source_type = SOURCE_TYPE_VIRTUAL;
			snprintf(st->label, sizeof(st->label), "虚拟屏幕 %dx%d", row->vw,
				row->vh);
			break;
		}
		default:
			free(st);
			continue;
		}
		if (st->obj == NULL) {
			free(st);
			continue;
		}
		zkde_screencast_stream_unstable_v1_add_listener(st->obj, &stream_listener, st);
		g_ptr_array_add(s->streams, st);
		log_info("%s: 要一路流（cursor=%s）", st->label,
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
		/* 没选东西时回车不该当「共享」（按钮这会儿是灰的，回车绕过了那层）。 */
		if (share_button != NULL &&
				gtk_widget_get_sensitive(share_button)) {
			dialog_accept();
		} else {
			log_info("回车了但没选任何东西，不理");
		}
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
	(void)data;
	/* 也当「刷新按钮可用性」用，所以允许不带按钮调（见 dialog_show 末尾）。 */
	bool multiple = pending != NULL && pending->session != NULL &&
		pending->session->multiple;
	int n = 0;
	for (GList *it = rows; it != NULL; it = it->next) {
		struct row *r = it->data;
		if (r->toggle == NULL) {
			continue;
		}
		bool on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(r->toggle));
		if (on && button != NULL && GTK_WIDGET(button) == r->toggle && !multiple) {
			/* 单选：点了一个就把别的都取消（KDE 的选择器也是这样）。 */
			for (GList *other = rows; other != NULL; other = other->next) {
				struct row *o = other->data;
				if (o == r || o->toggle == NULL) {
					continue;
				}
				if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(o->toggle))) {
					gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(o->toggle), FALSE);
				}
			}
		}
		if (on) {
			n++;
		}
	}
	if (share_button != NULL) {
		gtk_widget_set_sensitive(share_button, n > 0);
	}
}

/* 加一行（缩略图 + 可选的标签）。grid 的第 row 行。 */
static struct row *add_row(GtkWidget *grid, int row, enum row_kind kind, void *ptr,
		const char *text) {
	struct row *r = calloc(1, sizeof(*r));
	if (r == NULL) {
		return NULL;
	}
	r->kind = kind;
	r->ptr = ptr;

	GtkWidget *image = gtk_image_new();
	gtk_widget_set_size_request(image, THUMB_W, THUMB_H);
	gtk_grid_attach(GTK_GRID(grid), image, 0, row, 1, 1);
	r->image = image;

	GtkWidget *toggle = gtk_toggle_button_new_with_label(text);
	gtk_widget_set_size_request(toggle, 220, -1);
	g_signal_connect(toggle, "toggled", G_CALLBACK(on_toggle), NULL);
	gtk_grid_attach(GTK_GRID(grid), toggle, 1, row, 1, 1);
	r->toggle = toggle;

	rows = g_list_append(rows, r);
	return r;
}

/* 一段的标题（"屏幕" / "窗口" / "虚拟屏幕"）。 */
static void add_section(GtkWidget *grid, int row, const char *title) {
	char text[128];
	snprintf(text, sizeof(text), "<b>%s</b>", title);
	GtkWidget *label = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(label), text);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_grid_attach(GTK_GRID(grid), label, 0, row, 2, 1);
}

/* 弹对话框。多选（multiple）时用复选，否则单选（点一个就把别的取消）。 */
static void dialog_show(struct request *req) {
	struct session *s = req->session;
	uint32_t types = s->types != 0 ? s->types : SOURCE_TYPE_MONITOR;
	GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), "共享屏幕");
	gtk_window_set_default_size(GTK_WINDOW(win), 720, 620);
	gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER_ALWAYS);
	gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DIALOG);
	gtk_container_set_border_width(GTK_CONTAINER(win), 12);

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_container_add(GTK_CONTAINER(win), box);

	char title[256];
	snprintf(title, sizeof(title), "%s 想要录屏，选要共享的内容：",
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

	g_list_free_full(rows, g_free);
	rows = NULL;

	int row = 0;
	struct row *first = NULL;
	if ((types & SOURCE_TYPE_MONITOR) != 0 && outputs != NULL) {
		add_section(grid, row++, "屏幕");
		for (GList *it = outputs; it != NULL; it = it->next) {
			struct output *out = it->data;
			char text[128];
			snprintf(text, sizeof(text), "%s\n%d × %d",
				out->name[0] != '\0' ? out->name : "输出", out->w, out->h);
			(void)add_row(grid, row++, ROW_OUTPUT, out, text);
		}
	}
	if ((types & SOURCE_TYPE_WINDOW) != 0) {
		int n = 0;
		for (GList *it = toplevels; it != NULL; it = it->next) {
			struct toplevel *tl = it->data;
			if (tl->handle == NULL) {
				continue;
			}
			if (n == 0) {
				add_section(grid, row++, "窗口");
			}
			char text[192];
			snprintf(text, sizeof(text), "%s\n%s",
				tl->title != NULL && tl->title[0] != '\0' ? tl->title : "(没标题)",
				tl->app_id != NULL ? tl->app_id : "");
			add_row(grid, row++, ROW_WINDOW, tl, text);
			n++;
		}
	}
	if ((types & SOURCE_TYPE_VIRTUAL) != 0) {
		/* 虚拟屏幕：合成器会现加一块 headless 输出（能摆窗口、进布局）。
		 * 尺寸取最大那块屏的逻辑尺寸 —— 用户想要的是「再来一块能用的屏」。 */
		int vw = 1920, vh = 1080;
		for (GList *it = outputs; it != NULL; it = it->next) {
			struct output *out = it->data;
			if (out->w > vw || out->h > vh) {
				vw = out->w;
				vh = out->h;
			}
		}
		add_section(grid, row++, "虚拟屏幕");
		char text[128];
		snprintf(text, sizeof(text), "新建一块虚拟屏幕\n%d × %d", vw, vh);
		struct row *r = add_row(grid, row++, ROW_VIRTUAL, NULL, text);
		if (r != NULL) {
			r->vw = vw;
			r->vh = vh;
		}
	}

	/* 默认选中第一行（不管是屏幕、窗口还是虚拟屏幕）：KDE 的选择器也是这样，
	 * 而且「共享」按钮在没选东西时是灰的 —— 进来就灰着等于让人多点一下。
	 * 只列窗口（types=2）时第一行是窗口，这里也得管上（踩过：只给屏幕那一段
	 * 设默认，types=2 进来一个都没选，点「共享」等于没点）。 */
	if (first == NULL && rows != NULL) {
		first = rows->data;
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
	/* 屏幕 / 窗口缩略图靠 row->image，行建完才让 preview_tick 动。 */
	for (GList *it = rows; it != NULL; it = it->next) {
		struct row *r = it->data;
		if (r->kind == ROW_OUTPUT) {
			((struct output *)r->ptr)->image = r->image;
			((struct output *)r->ptr)->toggle = r->toggle;
		}
	}
	if (first != NULL) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(first->toggle), TRUE);
	}
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
	/* 想看只列窗口 / 只列虚拟屏幕的排版：VIDOW_PORTAL_TYPES=2 / 4。 */
	const char *types_env = getenv("VIDOW_PORTAL_TYPES");
	s->types = types_env != NULL ? (uint32_t)strtoul(types_env, NULL, 0)
		: SOURCE_TYPES;
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
		/* 屏幕 / 窗口 / 虚拟屏幕。窗口要窗口列表协议在（没有的话报了也是空的）。 */
		uint32_t types = SOURCE_TYPE_MONITOR | SOURCE_TYPE_VIRTUAL;
		if (tlist != NULL && win_src_mgr != NULL && cap_mgr != NULL) {
			types |= SOURCE_TYPE_WINDOW;
		}
		return g_variant_new_uint32(types);
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

/* ------------------------------------------------------------ 截图
 *
 * Screenshot / PickColor 两条接口。**不走 PipeWire**：这个后端自己就是客户端，
 * 抓一帧像素（zwlr-screencopy 抓输出、ext-image-copy-capture 的窗口源抓窗口）、
 * 写个 PNG、把文件 URI 回给前端。比绕一圈合成器的录屏流简单得多，也不要求会话里
 * 有 PipeWire（截图本来就不需要）。
 *
 * 交互式（options 里 interactive=true）和取色：把抓到的那一帧**冻住**铺满屏，
 * 让用户框一块 / 点一下。冻住的那张图是像素尺寸，框选按比例换算回像素再裁。
 */

/* 一次抓帧（截图用）。缩略图那条路走的是 struct output 上的复用缓冲，这里是一次
 * 性的：抓完调回调，然后自己收摊。 */
struct shot_capture {
	struct wl_output *wl;
	struct zwlr_screencopy_frame_v1 *frame;
	struct wl_buffer *buffer;
	void *map;
	size_t map_size;
	int fd;
	int w, h, stride;
	uint32_t format;
	bool y_invert;
	bool have_buffer;
	void (*cb)(GdkPixbuf *pix, void *user);
	void *user;
};

static void shot_capture_free(struct shot_capture *c) {
	if (c->frame != NULL) {
		zwlr_screencopy_frame_v1_destroy(c->frame);
		c->frame = NULL;
	}
	if (c->buffer != NULL) {
		wl_buffer_destroy(c->buffer);
		c->buffer = NULL;
	}
	if (c->map != NULL) {
		munmap(c->map, c->map_size);
		c->map = NULL;
	}
	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}
	free(c);
}

static GdkPixbuf *shot_capture_pixbuf(struct shot_capture *c) {
	return pixbuf_from_shm(c->map, c->w, c->h, c->stride, c->format, c->y_invert);
}

static void shot_capture_ready(void *data,
		struct zwlr_screencopy_frame_v1 *frame, uint32_t tv_sec_hi,
		uint32_t tv_sec_lo, uint32_t tv_nsec) {
	struct shot_capture *c = data;
	(void)tv_sec_hi;
	(void)tv_sec_lo;
	(void)tv_nsec;
	GdkPixbuf *pix = shot_capture_pixbuf(c);
	void (*cb)(GdkPixbuf *, void *) = c->cb;
	void *user = c->user;
	c->frame = NULL;
	c->buffer = NULL;
	c->map = NULL;
	c->fd = -1;
	zwlr_screencopy_frame_v1_destroy(frame);
	shot_capture_free(c);
	if (cb != NULL) {
		cb(pix, user);
	}
	if (pix != NULL) {
		g_object_unref(pix);
	}
}

static void shot_capture_failed(void *data,
		struct zwlr_screencopy_frame_v1 *frame) {
	struct shot_capture *c = data;
	void (*cb)(GdkPixbuf *, void *) = c->cb;
	void *user = c->user;
	c->frame = NULL;
	zwlr_screencopy_frame_v1_destroy(frame);
	shot_capture_free(c);
	if (cb != NULL) {
		cb(NULL, user);
	}
}

static void shot_capture_buffer(void *data,
		struct zwlr_screencopy_frame_v1 *frame, uint32_t format, uint32_t width,
		uint32_t height, uint32_t stride) {
	struct shot_capture *c = data;
	(void)frame;
	c->w = (int)width;
	c->h = (int)height;
	c->stride = (int)stride;
	c->format = format;
	size_t size = (size_t)stride * (size_t)height;
	c->fd = create_shm_file(size);
	if (c->fd < 0) {
		return;
	}
	c->map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, 0);
	if (c->map == MAP_FAILED) {
		c->map = NULL;
		return;
	}
	c->map_size = size;
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, c->fd, (int)size);
	c->buffer = wl_shm_pool_create_buffer(pool, 0, (int)width, (int)height,
		(int)stride, format);
	wl_shm_pool_destroy(pool);
	c->have_buffer = c->buffer != NULL;
}

static void shot_capture_flags(void *data,
		struct zwlr_screencopy_frame_v1 *frame, uint32_t flags) {
	struct shot_capture *c = data;
	(void)frame;
	c->y_invert = (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
}

static void shot_capture_buffer_done(void *data,
		struct zwlr_screencopy_frame_v1 *frame) {
	struct shot_capture *c = data;
	if (c->buffer != NULL) {
		zwlr_screencopy_frame_v1_copy(frame, c->buffer);
		wl_display_flush(wl); /* 同上：不 flush 这一帧就永远不来 */
		return;
	}
	/* 没建出缓冲：当成失败收掉（不然回调永远不来）。 */
	void (*cb)(GdkPixbuf *, void *) = c->cb;
	void *user = c->user;
	c->frame = NULL;
	zwlr_screencopy_frame_v1_destroy(frame);
	shot_capture_free(c);
	if (cb != NULL) {
		cb(NULL, user);
	}
}

static void shot_capture_damage(void *data,
		struct zwlr_screencopy_frame_v1 *frame, uint32_t x, uint32_t y,
		uint32_t w, uint32_t h) {
	(void)data;
	(void)frame;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
}

static void shot_capture_linux_dmabuf(void *data,
		struct zwlr_screencopy_frame_v1 *frame, uint32_t format, uint32_t width,
		uint32_t height) {
	(void)data;
	(void)frame;
	(void)format;
	(void)width;
	(void)height;
}

/* 同上：7 个事件一个都不能少（NULL 会被 libwayland 当场 abort）。 */
static const struct zwlr_screencopy_frame_v1_listener shot_capture_listener = {
	.buffer = shot_capture_buffer,
	.flags = shot_capture_flags,
	.ready = shot_capture_ready,
	.failed = shot_capture_failed,
	.damage = shot_capture_damage,
	.linux_dmabuf = shot_capture_linux_dmabuf,
	.buffer_done = shot_capture_buffer_done,
};

/* 抓一块屏（像素尺寸的原图）。cb 收到 pixbuf（可能 NULL）。 */
static void shot_capture_output(struct output *out,
		void (*cb)(GdkPixbuf *pix, void *user), void *user) {
	if (scmgr == NULL || out == NULL || cb == NULL) {
		if (cb != NULL) {
			cb(NULL, user);
		}
		return;
	}
	struct shot_capture *c = calloc(1, sizeof(*c));
	if (c == NULL) {
		cb(NULL, user);
		return;
	}
	c->fd = -1;
	c->wl = out->wl;
	c->cb = cb;
	c->user = user;
	c->frame = zwlr_screencopy_manager_v1_capture_output(scmgr, 0, out->wl);
	if (c->frame == NULL) {
		shot_capture_free(c);
		cb(NULL, user);
		return;
	}
	zwlr_screencopy_frame_v1_add_listener(c->frame, &shot_capture_listener, c);
	wl_display_flush(wl);
}

/* ------------------------------------------------------------ PNG */

static bool write_png(const char *path, GdkPixbuf *pix) {
	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		log_info("写不了 %s：%s", path, strerror(errno));
		return false;
	}
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop info = png != NULL ? png_create_info_struct(png) : NULL;
	if (png == NULL || info == NULL) {
		fclose(f);
		if (png != NULL) {
			png_destroy_write_struct(&png, NULL);
		}
		return false;
	}
	if (setjmp(png_jmpbuf(png))) {
		png_destroy_write_struct(&png, &info);
		fclose(f);
		return false;
	}
	png_init_io(png, f);
	int w = gdk_pixbuf_get_width(pix);
	int h = gdk_pixbuf_get_height(pix);
	int channels = gdk_pixbuf_get_n_channels(pix);
	png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8,
		PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
		PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);
	/* pixbuf 的行里可能还有 alpha（我们建的是 3 通道，但 gdk 可能给 4 通道），
	 * 逐行喂给 libpng 时按 3 字节走。 */
	guchar *pixels = gdk_pixbuf_get_pixels(pix);
	int stride = gdk_pixbuf_get_rowstride(pix);
	png_bytep row = malloc((size_t)w * 3);
	if (row == NULL) {
		png_destroy_write_struct(&png, &info);
		fclose(f);
		return false;
	}
	for (int y = 0; y < h; y++) {
		const guchar *src = pixels + (size_t)y * (size_t)stride;
		for (int x = 0; x < w; x++) {
			row[x * 3 + 0] = src[x * channels + 0];
			row[x * 3 + 1] = src[x * channels + 1];
			row[x * 3 + 2] = src[x * channels + 2];
		}
		png_write_row(png, row);
	}
	free(row);
	png_write_end(png, NULL);
	png_destroy_write_struct(&png, &info);
	fclose(f);
	return true;
}

/* 截图往哪写：$XDG_CACHE_HOME/vidow-portal/（默认 ~/.cache/vidow-portal）。
 * 用缓存目录而不是 /tmp：应用拿到 URI 之后可能过一会儿才去读。 */
static char *screenshot_dir(void) {
	const char *cache = getenv("XDG_CACHE_HOME");
	char path[512];
	if (cache != NULL && cache[0] != '\0') {
		snprintf(path, sizeof(path), "%s/vidow-portal", cache);
	} else {
		const char *home = getenv("HOME");
		snprintf(path, sizeof(path), "%s/.cache/vidow-portal",
			home != NULL ? home : "/tmp");
	}
	g_mkdir_with_parents(path, 0700);
	return g_strdup(path);
}

static char *screenshot_write(GdkPixbuf *pix) {
	char *dir = screenshot_dir();
	char *path = g_strdup_printf("%s/截图-%ld.png", dir,
		(long)time(NULL));
	g_free(dir);
	if (!write_png(path, pix)) {
		g_free(path);
		return NULL;
	}
	char *uri = g_strdup_printf("file://%s", path);
	g_free(path);
	return uri;
}

/* ------------------------------------------------------------ 截图 / 取色请求 */

struct shot {
	char *path;
	GDBusMethodInvocation *inv;
	char *app_id;
	char *parent;
	bool pick_color;
	bool interactive;
	struct output *out;      /* 冻住的是哪块屏 */
	GdkPixbuf *frozen;       /* 冻住的那一帧（像素尺寸） */
	int frozen_x, frozen_y;  /* 这块屏在布局里的位置（逻辑） */
	int frozen_scale;
	GtkWidget *win;
	GtkWidget *area;
	bool dragging;
	int x0, y0, x1, y1;
};

static struct shot *pending_shot;

static void shot_finish(struct shot *sh, uint32_t response, GVariant *results) {
	if (sh == NULL || sh->inv == NULL) {
		return;
	}
	g_dbus_method_invocation_return_value(sh->inv,
		g_variant_new("(u@a{sv})", response,
			results != NULL ? results : empty_results()));
	sh->inv = NULL;
}

static GVariant *shot_uri_results(const char *uri) {
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	if (uri != NULL) {
		g_variant_builder_add(&b, "{sv}", "uri", g_variant_new_string(uri));
	}
	return g_variant_builder_end(&b);
}

static void shot_destroy(struct shot *sh) {
	if (sh == NULL) {
		return;
	}
	if (sh->win != NULL) {
		gtk_widget_destroy(sh->win);
		sh->win = NULL;
	}
	if (sh->frozen != NULL) {
		g_object_unref(sh->frozen);
		sh->frozen = NULL;
	}
	g_free(sh->path);
	g_free(sh->app_id);
	g_free(sh->parent);
	if (pending_shot == sh) {
		pending_shot = NULL;
	}
	free(sh);
}

/* 用户框完了：把冻住的那一帧按框裁出来（框是控件坐标，按比例换算回像素）。 */
static void shot_accept_region(struct shot *sh) {
	int x0 = sh->x0 < sh->x1 ? sh->x0 : sh->x1;
	int y0 = sh->y0 < sh->y1 ? sh->y0 : sh->y1;
	int x1 = sh->x0 > sh->x1 ? sh->x0 : sh->x1;
	int y1 = sh->y0 > sh->y1 ? sh->y0 : sh->y1;
	int iw = gdk_pixbuf_get_width(sh->frozen);
	int ih = gdk_pixbuf_get_height(sh->frozen);
	int aw = gtk_widget_get_allocated_width(sh->area);
	int ah = gtk_widget_get_allocated_height(sh->area);
	if (aw < 1 || ah < 1) {
		return;
	}
	/* 图是按「等比放进控件」画的（见 shot_draw），这里用同一个比例换回去。 */
	double k = (double)iw / (double)aw;
	double kh = (double)ih / (double)ah;
	if (kh < k) {
		k = kh;
	}
	int rx = (int)(x0 * k), ry = (int)(y0 * k);
	int rw = (int)((x1 - x0) * k), rh = (int)((y1 - y0) * k);
	if (rw < 2 || rh < 2) {
		return;
	}
	if (rx < 0) rx = 0;
	if (ry < 0) ry = 0;
	if (rx + rw > iw) rw = iw - rx;
	if (ry + rh > ih) rh = ih - ry;
	GdkPixbuf *crop = gdk_pixbuf_new_subpixbuf(sh->frozen, rx, ry, rw, rh);
	char *uri = crop != NULL ? screenshot_write(crop) : NULL;
	if (crop != NULL) {
		g_object_unref(crop);
	}
	log_info("区域截图 %d,%d %dx%d（控件里是 %d,%d %dx%d）→ %s", rx, ry, rw, rh,
		x0, y0, x1 - x0, y1 - y0, uri != NULL ? uri : "写不出去");
	shot_finish(sh, uri != NULL ? RESPONSE_SUCCESS : RESPONSE_OTHER,
		shot_uri_results(uri));
	g_free(uri);
	shot_destroy(sh);
}

/* 取色：点在哪就取哪个像素，回 (ddd)（0..1 的 RGB）。 */
static void shot_pick_at(struct shot *sh, int wx, int wy) {
	int iw = gdk_pixbuf_get_width(sh->frozen);
	int ih = gdk_pixbuf_get_height(sh->frozen);
	int aw = gtk_widget_get_allocated_width(sh->area);
	int ah = gtk_widget_get_allocated_height(sh->area);
	if (aw < 1 || ah < 1) {
		return;
	}
	double k = (double)iw / (double)aw;
	double kh = (double)ih / (double)ah;
	if (kh < k) {
		k = kh;
	}
	int px = (int)(wx * k), py = (int)(wy * k);
	if (px < 0) px = 0;
	if (py < 0) py = 0;
	if (px >= iw) px = iw - 1;
	if (py >= ih) py = ih - 1;
	guchar *pixels = gdk_pixbuf_get_pixels(sh->frozen);
	int stride = gdk_pixbuf_get_rowstride(sh->frozen);
	int channels = gdk_pixbuf_get_n_channels(sh->frozen);
	guchar *p = pixels + (size_t)py * (size_t)stride + (size_t)px * channels;
	double r = p[0] / 255.0, g = p[1] / 255.0, b = p[2] / 255.0;
	log_info("取色：控件 %d,%d → 像素 %d,%d = %.3f %.3f %.3f", wx, wy, px, py, r,
		g, b);
	GVariantBuilder rb;
	g_variant_builder_init(&rb, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&rb, "{sv}", "color", g_variant_new("(ddd)", r, g, b));
	shot_finish(sh, RESPONSE_SUCCESS, g_variant_builder_end(&rb));
	shot_destroy(sh);
}

static gboolean shot_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
	struct shot *sh = data;
	int aw = gtk_widget_get_allocated_width(widget);
	int ah = gtk_widget_get_allocated_height(widget);
	int iw = gdk_pixbuf_get_width(sh->frozen);
	int ih = gdk_pixbuf_get_height(sh->frozen);
	double k = (double)aw / iw;
	double kh = (double)ah / ih;
	if (kh < k) {
		k = kh;
	}
	int dw = (int)(iw * k), dh = (int)(ih * k);
	int dx = (aw - dw) / 2, dy = (ah - dh) / 2;
	/* 图是「等比放进控件、居中」画的；shot_accept_region / shot_pick_at 里换算
	 * 回像素时用的是同一个比例（居中那点偏移最多半个像素，先不计）。 */
	GdkPixbuf *scaled = gdk_pixbuf_scale_simple(sh->frozen, dw, dh,
		GDK_INTERP_BILINEAR);
	if (scaled != NULL) {
		gdk_cairo_set_source_pixbuf(cr, scaled, dx, dy);
		cairo_paint(cr);
		g_object_unref(scaled);
	}
	if (sh->dragging || (sh->x1 != 0 && sh->y1 != 0)) {
		int x0 = sh->x0 < sh->x1 ? sh->x0 : sh->x1;
		int y0 = sh->y0 < sh->y1 ? sh->y0 : sh->y1;
		int x1 = sh->x0 > sh->x1 ? sh->x0 : sh->x1;
		int y1 = sh->y0 > sh->y1 ? sh->y0 : sh->y1;
		cairo_set_source_rgba(cr, 0.0, 0.6, 1.0, 0.9);
		cairo_set_line_width(cr, 2);
		cairo_rectangle(cr, x0 + 0.5, y0 + 0.5, x1 - x0, y1 - y0);
		cairo_stroke(cr);
	}
	return TRUE;
}

static gboolean shot_button_press(GtkWidget *widget, GdkEventButton *event,
		gpointer data) {
	struct shot *sh = data;
	(void)widget;
	if (event->button != 1) {
		return TRUE;
	}
	if (sh->pick_color) {
		shot_pick_at(sh, (int)event->x, (int)event->y);
		return TRUE;
	}
	sh->dragging = true;
	sh->x0 = sh->x1 = (int)event->x;
	sh->y0 = sh->y1 = (int)event->y;
	gtk_widget_queue_draw(sh->area);
	return TRUE;
}

static gboolean shot_motion(GtkWidget *widget, GdkEventMotion *event,
		gpointer data) {
	struct shot *sh = data;
	(void)widget;
	if (!sh->dragging) {
		return TRUE;
	}
	sh->x1 = (int)event->x;
	sh->y1 = (int)event->y;
	gtk_widget_queue_draw(sh->area);
	return TRUE;
}

static gboolean shot_button_release(GtkWidget *widget, GdkEventButton *event,
		gpointer data) {
	struct shot *sh = data;
	(void)widget;
	if (event->button != 1 || !sh->dragging) {
		return TRUE;
	}
	sh->dragging = false;
	sh->x1 = (int)event->x;
	sh->y1 = (int)event->y;
	shot_accept_region(sh);
	return TRUE;
}

static gboolean shot_key(GtkWidget *widget, GdkEventKey *event, gpointer data) {
	struct shot *sh = data;
	(void)widget;
	if (event->keyval == GDK_KEY_Escape) {
		log_info("截图：用户按了 Esc，取消");
		shot_finish(sh, RESPONSE_CANCELLED, NULL);
		shot_destroy(sh);
		return TRUE;
	}
	return FALSE;
}

static gboolean shot_fullscreen_now(gpointer data) {
	gtk_window_fullscreen(GTK_WINDOW(data));
	return G_SOURCE_REMOVE;
}

/* 把冻住的那一帧铺满屏（框选 / 取色）。挂到发起请求的窗口上。 */
static void shot_show_picker(struct shot *sh) {
	GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), sh->pick_color ? "取色" : "框选区域");
	gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
	GtkWidget *area = gtk_drawing_area_new();
	gtk_widget_set_events(area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
		GDK_POINTER_MOTION_MASK);
	gtk_container_add(GTK_CONTAINER(win), area);
	g_signal_connect(area, "draw", G_CALLBACK(shot_draw), sh);
	g_signal_connect(area, "button-press-event", G_CALLBACK(shot_button_press), sh);
	g_signal_connect(area, "button-release-event", G_CALLBACK(shot_button_release),
		sh);
	g_signal_connect(area, "motion-notify-event", G_CALLBACK(shot_motion), sh);
	g_signal_connect(win, "key-press-event", G_CALLBACK(shot_key), sh);
	sh->win = win;
	sh->area = area;
	gtk_widget_show_all(win);
	/* 全屏要等窗口真的上屏之后再要：客户端在**第一次 commit 之前**发的
	 * set_fullscreen 会被合成器丢掉（vidow 那边要等窗口 initialized 才认），
	 * 结果就是「要了全屏但还是平铺着」。 */
	g_timeout_add(80, shot_fullscreen_now, win);
	/* 光标要能点到（不然用户看不见指针在哪）。 */
	GdkWindow *gw = gtk_widget_get_window(win);
	if (gw != NULL) {
		GdkCursor *cursor = gdk_cursor_new_for_display(
			gdk_display_get_default(), GDK_CROSSHAIR);
		gdk_window_set_cursor(gw, cursor);
		g_object_unref(cursor);
	}
	if (sh->parent != NULL && g_str_has_prefix(sh->parent, "wayland:")) {
		GdkWindow *gdkwin = gtk_widget_get_window(win);
		if (gdkwin != NULL && GDK_IS_WAYLAND_WINDOW(gdkwin)) {
			gdk_wayland_window_set_transient_for_exported(gdkwin,
				sh->parent + strlen("wayland:"));
		}
	}
	log_info("截图：冻住 %s（%dx%d 像素），等用户%s", sh->out->name,
		gdk_pixbuf_get_width(sh->frozen), gdk_pixbuf_get_height(sh->frozen),
		sh->pick_color ? "点一下取色" : "框一块");
}

/* 抓到一帧之后：不交互就直接写文件回掉；交互式 / 取色就把图冻住铺满屏。 */
static void shot_got_frame(GdkPixbuf *pix, void *user) {
	struct shot *sh = user;
	if (pix == NULL) {
		log_info("截图：没抓到帧");
		shot_finish(sh, RESPONSE_OTHER, NULL);
		shot_destroy(sh);
		return;
	}
	if (!sh->interactive && !sh->pick_color) {
		char *uri = screenshot_write(pix);
		log_info("整屏截图（%s %dx%d）→ %s", sh->out->name,
			gdk_pixbuf_get_width(pix), gdk_pixbuf_get_height(pix),
			uri != NULL ? uri : "写不出去");
		shot_finish(sh, uri != NULL ? RESPONSE_SUCCESS : RESPONSE_OTHER,
			shot_uri_results(uri));
		g_free(uri);
		shot_destroy(sh);
		return;
	}
	sh->frozen = pix;
	g_object_ref(pix);
	shot_show_picker(sh);
}

/* 光标底下那块屏（没有就用第一块）。KDE 非交互式截图也是「当前那块屏」。 */
static struct output *output_under_pointer(void) {
	if (outputs == NULL) {
		return NULL;
	}
	return outputs->data;
}

static void shot_start(struct shot *sh) {
	sh->out = output_under_pointer();
	if (sh->out == NULL) {
		shot_finish(sh, RESPONSE_OTHER, NULL);
		shot_destroy(sh);
		return;
	}
	log_info("%s：抓 %s（interactive=%d）", sh->app_id != NULL ? sh->app_id : "?",
		sh->out->name, sh->interactive ? 1 : 0);
	shot_capture_output(sh->out, shot_got_frame, sh);
}

/* ------------------------------------------------------------ Screenshot 接口 */

static const char screenshot_xml[] =
	"<node>"
	" <interface name='org.freedesktop.impl.portal.Screenshot'>"
	"  <method name='Screenshot'>"
	"   <arg type='o' name='handle' direction='in'/>"
	"   <arg type='s' name='app_id' direction='in'/>"
	"   <arg type='s' name='parent_window' direction='in'/>"
	"   <arg type='a{sv}' name='options' direction='in'/>"
	"   <arg type='u' name='response' direction='out'/>"
	"   <arg type='a{sv}' name='results' direction='out'/>"
	"  </method>"
	"  <method name='PickColor'>"
	"   <arg type='o' name='handle' direction='in'/>"
	"   <arg type='s' name='app_id' direction='in'/>"
	"   <arg type='s' name='parent_window' direction='in'/>"
	"   <arg type='a{sv}' name='options' direction='in'/>"
	"   <arg type='u' name='response' direction='out'/>"
	"   <arg type='a{sv}' name='results' direction='out'/>"
	"  </method>"
	"  <property name='version' type='u' access='read'/>"
	" </interface>"
	"</node>";

static void on_screenshot(GDBusConnection *conn, const gchar *sender,
		const gchar *object_path, const gchar *interface_name,
		const gchar *method_name, GVariant *parameters,
		GDBusMethodInvocation *invocation, gpointer user_data) {
	(void)conn;
	(void)sender;
	(void)object_path;
	(void)interface_name;
	(void)user_data;

	bool pick = g_strcmp0(method_name, "PickColor") == 0;
	/* 签名是 (o handle, s app_id, s parent_window, a{sv} options)：**没有 session**
	 * （截图是无状态的，前端不会给会话路径）。 */
	const char *handle = NULL, *app_id = NULL, *parent = NULL;
	GVariant *options = NULL;
	g_variant_get(parameters, "(&o&s&s@a{sv})", &handle, &app_id, &parent, &options);
	bool interactive = false;
	if (options != NULL) {
		GVariant *v = g_variant_lookup_value(options, "interactive",
			G_VARIANT_TYPE_BOOLEAN);
		if (v != NULL) {
			interactive = g_variant_get_boolean(v);
			g_variant_unref(v);
		}
		g_variant_unref(options);
	}
	if (pending_shot != NULL) {
		log_info("%s: 已经有一个截图在等着用户了，先拒", app_id);
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(u@a{sv})", RESPONSE_OTHER, empty_results()));
		return;
	}
	struct shot *sh = calloc(1, sizeof(*sh));
	if (sh == NULL) {
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(u@a{sv})", RESPONSE_OTHER, empty_results()));
		return;
	}
	sh->path = g_strdup(handle);
	sh->inv = invocation;
	sh->app_id = g_strdup(app_id);
	sh->parent = g_strdup(parent);
	sh->pick_color = pick;
	/* 取色天生是交互的（要用户点一个像素）。 */
	sh->interactive = interactive || pick;
	pending_shot = sh;
	shot_start(sh);
}

static GVariant *screenshot_get_property(GDBusConnection *conn, const gchar *sender,
		const gchar *object_path, const gchar *interface_name,
		const gchar *property_name, GError **error, gpointer user_data) {
	(void)conn;
	(void)sender;
	(void)object_path;
	(void)interface_name;
	(void)error;
	(void)user_data;
	if (g_strcmp0(property_name, "version") == 0) {
		return g_variant_new_uint32(1);
	}
	return NULL;
}

static const GDBusInterfaceVTable screenshot_vtable = {
	.method_call = on_screenshot,
	.get_property = screenshot_get_property,
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
	/* 截图（Screenshot / PickColor）是另一个接口，同一个对象路径上再注册一个。 */
	GDBusNodeInfo *sinfo = g_dbus_node_info_new_for_xml(screenshot_xml, &error);
	if (sinfo == NULL) {
		log_info("Screenshot XML 坏了: %s", error->message);
		exit(1);
	}
	guint sid = g_dbus_connection_register_object(conn, OBJECT_PATH,
		sinfo->interfaces[0], &screenshot_vtable, NULL, NULL, &error);
	g_dbus_node_info_unref(sinfo);
	if (sid == 0) {
		log_info("注册 Screenshot 失败: %s", error != NULL ? error->message : "?");
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
	/* 想看只列窗口 / 只列虚拟屏幕的排版：VIDOW_PORTAL_TYPES=2 / 4。 */
	const char *types_env = getenv("VIDOW_PORTAL_TYPES");
	s->types = types_env != NULL ? (uint32_t)strtoul(types_env, NULL, 0)
		: SOURCE_TYPES;
	s->cursor_mode = 1;
	s->streams = g_ptr_array_new();
	req->path = strdup("/debug/request");
	req->session = s;
	req->invocation = NULL;
	req->parent_window = strdup("");
	pending = req;
	dialog_show(req);
}

/* 不经过 DBus 直接跑一次截图（看框选窗口、看 PNG 写得对不对）。
 * pick_color=true 就是取色那条路。 */
static void debug_shot(bool pick_color, bool interactive) {
	struct shot *sh = calloc(1, sizeof(*sh));
	if (sh == NULL) {
		exit(1);
	}
	sh->path = strdup("/debug/shot");
	sh->app_id = strdup("debug");
	sh->parent = strdup("");
	sh->pick_color = pick_color;
	sh->interactive = interactive;
	pending_shot = sh;
	shot_start(sh);
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
	/* 窗口列表的 title / app_id / identifier 是 handle 自己的事件，再转两圈收齐。 */
	wl_display_roundtrip(wl);
	wl_display_roundtrip(wl);

	log_info("wl_shm=%s screencopy=%s zkde_screencast=%s 窗口列表=%s 窗口抓帧=%s 输出=%u",
		shm != NULL ? "有" : "没有",
		scmgr != NULL ? "有" : "没有",
		zkde != NULL ? "有" : "没有",
		tlist != NULL ? "有" : "没有",
		(win_src_mgr != NULL && cap_mgr != NULL) ? "有" : "没有",
		g_list_length(outputs));
	for (GList *it = outputs; it != NULL; it = it->next) {
		struct output *out = it->data;
		log_info("  输出 %s %dx%d @ %d,%d scale=%d",
			out->name[0] != '\0' ? out->name : "(没名字)",
			out->w, out->h, out->x, out->y, out->scale);
	}
	for (GList *it = toplevels; it != NULL; it = it->next) {
		struct toplevel *tl = it->data;
		log_info("  窗口 「%s」（%s）identifier=%s", tl->title, tl->app_id,
			tl->identifier);
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
	bool want_shot = argc > 1 && strcmp(argv[1], "--shot") == 0;
	bool want_shot_plain = argc > 1 && strcmp(argv[1], "--shot-plain") == 0;
	bool want_shot_color = argc > 1 && strcmp(argv[1], "--shot-color") == 0;

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
	if (want_shot || want_shot_color || want_shot_plain) {
		debug_shot(want_shot_color, want_shot || want_shot_color);
		gtk_main();
		return 0;
	}

	g_bus_own_name(G_BUS_TYPE_SESSION, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
		on_bus_acquired, NULL, on_name_lost, NULL, NULL);

	gtk_main();
	return 0;
}
