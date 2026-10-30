#include "x11surface.h"
#include "server.h"
#include "clipboard.h"
#include "ipc.h"
#include "ipcstate.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <wlr/util/log.h>
#include <xcb/xcb.h>
#include <xcb/xcb_event.h>
#include <xcb/xfixes.h>

/* 剪贴板桥：X11(Xwayland) 与 Wayland 共用一份内容。为什么要自己接管、拦的是哪几条
 * 路、以及为什么不跟 wlroots 的 xwm 抢 X 侧 owner —— 见 server.h 里 struct
 * vidow_clipboard 上面那段。这里只说数据怎么走：
 *
 *   X11 -> Wayland
 *     XFixes 通知「owner 换了」→ 我们向那个窗口 convert TARGETS → 按 TARGETS 里的
 *     顺序一个类型一个类型 convert（大块走 INCR）→ 字节进内存 → 把一份源发布到
 *     seat 上（Wayland 客户端立刻能粘，数据没到全就先攥着它的 fd）。X 那边因为
 *     xwm 已经抢到 owner，请求送到它的代理窗口 —— 我们拦下来，从同一份内存里答。
 *
 *   Wayland -> X11
 *     客户端 set_selection → 我们马上把它的数据「拉」进内存（每个 mime 一条管道，
 *     边读边缓存）→ xwm 照旧去抢 X owner → X11 客户端来粘时请求送到代理窗口 →
 *     我们拦下来，从内存里答（大块走 INCR）。
 *
 *   源程序退出
 *     两边的内容都已经在我们内存里，所以哪边退出都不影响粘贴。Wayland 客户端退出
 *     时 seat 会把选择清掉，这时候我们把缓存重新发布一遍。
 *
 * 上限：clipboard_max_bytes 是「每个 mime 类型」的上限。超了就不缓存那个类型（日志
 * 里会说）——「源程序退出后还能粘」靠的就是这份内存副本。
 */

/* ============================================================== 小工具 */

static uint64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* 取内容时按这个挑（越小越优先）。文本优先，其次图片，其它垫底。 */
static int mime_rank(const char *mime) {
	if (strcmp(mime, "text/plain;charset=utf-8") == 0) return 0;
	if (strcmp(mime, "text/plain") == 0) return 1;
	if (strcmp(mime, "UTF8_STRING") == 0) return 2;
	if (strcmp(mime, "STRING") == 0) return 3;
	if (strcmp(mime, "text/uri-list") == 0) return 4;
	/* 文件复制的「真身」：文件管理器认这个才是复制文件，历史里得跟着 uri-list
	 * 一起留下（所以给同一个档位，别让它掉出前面几份）。 */
	if (strcmp(mime, "x-special/gnome-copied-files") == 0) return 4;
	if (strncmp(mime, "text/", 5) == 0) return 5;
	if (strcmp(mime, "image/png") == 0) return 6;
	if (strcmp(mime, "image/bmp") == 0) return 7;
	return 20;
}

static bool mime_is_text(const char *mime) {
	return strncmp(mime, "text/", 5) == 0 ||
		strcmp(mime, "UTF8_STRING") == 0 || strcmp(mime, "STRING") == 0;
}

static xcb_atom_t intern_atom(xcb_connection_t *conn, const char *name) {
	xcb_intern_atom_cookie_t cookie =
		xcb_intern_atom(conn, 0, strlen(name), name);
	xcb_intern_atom_reply_t *reply =
		xcb_intern_atom_reply(conn, cookie, NULL);
	if (reply == NULL) {
		return XCB_ATOM_NONE;
	}
	xcb_atom_t atom = reply->atom;
	free(reply);
	return atom;
}

static char *atom_name(xcb_connection_t *conn, xcb_atom_t atom) {
	xcb_get_atom_name_cookie_t cookie = xcb_get_atom_name(conn, atom);
	xcb_get_atom_name_reply_t *reply =
		xcb_get_atom_name_reply(conn, cookie, NULL);
	if (reply == NULL) {
		return NULL;
	}
	char *name = strndup(xcb_get_atom_name_name(reply),
		(size_t)xcb_get_atom_name_name_length(reply));
	free(reply);
	return name;
}

/* mime <-> X 侧 target：两条特例跟 wlroots 一致（别的实现也这么认），其余按字面
 * intern —— 于是 text/html、image/png、text/uri-list 这些都能原样过桥。 */
static xcb_atom_t mime_to_atom(struct vidow_clipboard *clip, const char *mime) {
	if (strcmp(mime, "text/plain;charset=utf-8") == 0) {
		return clip->atom_utf8;
	}
	if (strcmp(mime, "text/plain") == 0) {
		return clip->atom_text;
	}
	return intern_atom(clip->conn, mime);
}

static char *atom_to_mime(struct vidow_clipboard *clip, xcb_atom_t atom) {
	if (atom == clip->atom_utf8) {
		return strdup("text/plain;charset=utf-8");
	}
	if (atom == clip->atom_text) {
		return strdup("text/plain");
	}
	return atom_name(clip->conn, atom);
}

static size_t clip_max(struct vidow_server *server) {
	if (server->config.clipboard_max_bytes <= 0) {
		return SIZE_MAX; /* 配成 0/负数 = 不限（自己担着内存） */
	}
	return (size_t)server->config.clipboard_max_bytes;
}

/* ---------------------------------------------------------- 类型表 */

static struct vidow_clip_mime *sel_find(struct vidow_clip_selection *sel,
		const char *mime) {
	for (int i = 0; i < sel->nmimes; ++i) {
		if (strcmp(sel->mimes[i].mime, mime) == 0) {
			return &sel->mimes[i];
		}
	}
	return NULL;
}

static struct vidow_clip_mime *sel_add(struct vidow_clip_selection *sel,
		const char *mime, xcb_atom_t atom) {
	if (sel->nmimes >= VIDOW_CLIP_MIMES_MAX) {
		return NULL;
	}
	struct vidow_clip_mime *m = &sel->mimes[sel->nmimes++];
	m->mime = strdup(mime);
	m->atom = atom;
	m->data = NULL;
	m->len = 0;
	m->complete = false;
	m->overflow = false;
	return m;
}

static void mime_clear(struct vidow_clip_mime *m) {
	free(m->mime);
	m->mime = NULL;
	free(m->data);
	m->data = NULL;
	m->len = 0;
	m->complete = false;
	m->overflow = false;
}

static void sel_reset(struct vidow_clip_selection *sel) {
	for (int i = 0; i < sel->nmimes; ++i) {
		mime_clear(&sel->mimes[i]);
	}
	sel->nmimes = 0;
}

static void staging_reset(struct vidow_clip_selection *sel) {
	for (int i = 0; i < sel->nstaging; ++i) {
		mime_clear(&sel->staging[i]);
	}
	sel->nstaging = 0;
}

static struct vidow_clip_mime *staging_add(struct vidow_clip_selection *sel,
		const char *mime, xcb_atom_t atom) {
	if (sel->nstaging >= VIDOW_CLIP_MIMES_MAX) {
		return NULL;
	}
	struct vidow_clip_mime *m = &sel->staging[sel->nstaging++];
	m->mime = strdup(mime);
	m->atom = atom;
	m->data = NULL;
	m->len = 0;
	m->complete = false;
	m->overflow = false;
	return m;
}

/* 取全了：把暂存的那一份换上去（旧的那份这时候才释放）。 */
static void staging_commit(struct vidow_clip_selection *sel) {
	sel_reset(sel);
	for (int i = 0; i < sel->nstaging; ++i) {
		sel->mimes[i] = sel->staging[i];
		sel->staging[i] = (struct vidow_clip_mime){0};
	}
	sel->nmimes = sel->nstaging;
	sel->nstaging = 0;
}

/* 这份内容里有没有「能拿出来用」的字节。 */
static bool sel_has_data(struct vidow_clip_selection *sel) {
	for (int i = 0; i < sel->nmimes; ++i) {
		if (sel->mimes[i].complete && sel->mimes[i].len > 0) {
			return true;
		}
	}
	return false;
}

/* 从一张类型表里挑一份：指定 mime 就找那个，否则按 mime_rank 挑最顺手的。
 * 只认「取全了、没超上限、真有字节」的类型。历史条目（数组）和当前内容（sel）
 * 共用这一份规则。 */
static struct vidow_clip_mime *mime_pick(struct vidow_clip_mime *mimes, int n,
		const char *want) {
	if (want != NULL) {
		for (int i = 0; i < n; ++i) {
			if (strcmp(mimes[i].mime, want) != 0) {
				continue;
			}
			return mimes[i].complete && !mimes[i].overflow &&
				mimes[i].len > 0 ? &mimes[i] : NULL;
		}
		return NULL;
	}
	struct vidow_clip_mime *best = NULL;
	int best_rank = 1000;
	for (int i = 0; i < n; ++i) {
		struct vidow_clip_mime *m = &mimes[i];
		if (!m->complete || m->overflow || m->len == 0) {
			continue;
		}
		int rank = mime_rank(m->mime);
		if (rank < best_rank) {
			best_rank = rank;
			best = m;
		}
	}
	return best;
}

/* 挑一份内容：指定类型就找那个，否则按 mime_rank 挑最顺手的。 */
static struct vidow_clip_mime *sel_pick(struct vidow_clip_selection *sel,
		const char *want) {
	return mime_pick(sel->mimes, sel->nmimes, want);
}

/* 往一份内容后面接字节。超过上限就把这份整个丢掉（留着「取不全」的残片没意义）。 */
static void mime_append(struct vidow_clip_mime *m, const void *data, size_t len,
		size_t max) {
	if (m->overflow) {
		return;
	}
	if (len > max || m->len > max - len) {
		wlr_log(WLR_INFO, "剪贴板: %s 超过 clipboard_max_bytes，这个类型不缓存了",
			m->mime);
		free(m->data);
		m->data = NULL;
		m->len = 0;
		m->overflow = true;
		return;
	}
	unsigned char *next = realloc(m->data, m->len + len);
	if (next == NULL) {
		m->overflow = true;
		return;
	}
	m->data = next;
	memcpy(m->data + m->len, data, len);
	m->len += len;
}

/* ==================================================== 发布给 Wayland 的源 */

static void waiter_destroy(struct vidow_clip_waiter *w);
static void waiter_flush(struct vidow_clip_waiter *w);
static int waiter_writable(int fd, uint32_t mask, void *data);
static void out_destroy(struct vidow_clip_out *out);
static void sel_flush_waiters(struct vidow_clip_selection *sel);
static void sel_flush_outs(struct vidow_clip_selection *sel);
static void sel_tick_arm(struct vidow_clip_selection *sel);
static void sel_emit(struct vidow_clip_selection *sel, const char *what);
static void history_push(struct vidow_clip_selection *sel);
static void history_evict(struct vidow_server *server);
static void sel_unpublish(struct vidow_clip_selection *sel);
static void fetch_reset(struct vidow_clip_selection *sel);
static void pull_destroy(struct vidow_clip_pull *pull);
static void out_send(struct vidow_clip_out *out);
/* X 侧连接对账：还活着就返回它，死了（Xwayland 退出后重启前）当场摘桥并返回
 * NULL —— 定义在 x11_attach() 前面，见那里的注释。 */
static xcb_connection_t *x11_conn(struct vidow_clipboard *clip);

/* 内容/归属变了：推一条事件出去（没人订阅就什么都不做）。 */
static void sel_emit(struct vidow_clip_selection *sel, const char *what) {
	vidow_ipc_emit_clipboard(sel->server, sel, what);
}

/* 把一份内容写进 fd。写不完（管道满）就挂个可写事件接着写；对面关了就当写完。 */
static void fd_write_some(struct vidow_clip_waiter *w) {
	struct vidow_clip_mime *m = sel_find(w->sel, w->mime);
	if (m == NULL && mime_is_text(w->mime)) {
		m = sel_pick(w->sel, NULL);
		if (m != NULL && !mime_is_text(m->mime)) {
			m = NULL;
		}
	}
	if (m == NULL || m->overflow) {
		/* 类型没了 / 超过上限：空管道就是「没有」。 */
		waiter_destroy(w);
		return;
	}
	if (!m->complete) {
		/* Wayland 客户端可能在我们拉取源数据完成前就来粘；保留 fd，
		 * pull_finish() 会通过 sel_flush_waiters() 继续写。 */
		return;
	}
	while (w->offset < m->len) {
		ssize_t n = write(w->fd, m->data + w->offset, m->len - w->offset);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (w->source == NULL) {
					w->source = wl_event_loop_add_fd(
						wl_display_get_event_loop(w->sel->server->display),
						w->fd, WL_EVENT_WRITABLE, waiter_writable, w);
				}
				return; /* 等可写 */
			}
			waiter_destroy(w); /* 对面关了 */
			return;
		}
		if (n == 0) {
			return;
		}
		w->offset += (size_t)n;
	}
	waiter_destroy(w);
}

static int waiter_writable(int fd, uint32_t mask, void *data) {
	(void)fd;
	(void)mask;
	struct vidow_clip_waiter *w = data;
	fd_write_some(w);
	return 0;
}

static struct vidow_clip_waiter *waiter_new(struct vidow_clip_selection *sel,
		const char *mime, int fd) {
	struct vidow_clip_waiter *w = calloc(1, sizeof(*w));
	if (w == NULL) {
		close(fd);
		return NULL;
	}
	w->sel = sel;
	w->mime = strdup(mime);
	w->fd = fd;
	w->offset = 0;
	w->deadline_ms = now_ms() + VIDOW_CLIP_TIMEOUT_MS;
	wl_list_insert(&sel->waiters, &w->link);
	sel_tick_arm(sel);
	return w;
}

static void waiter_destroy(struct vidow_clip_waiter *w) {
	if (w == NULL) {
		return;
	}
	if (w->source != NULL) {
		wl_event_source_remove(w->source);
	}
	close(w->fd);
	wl_list_remove(&w->link);
	free(w->mime);
	free(w);
}

/* 类型表变了：把等着这个类型的 fd 都喂一遍。 */
static void sel_flush_waiters(struct vidow_clip_selection *sel) {
	struct vidow_clip_waiter *w, *tmp;
	wl_list_for_each_safe(w, tmp, &sel->waiters, link) {
		fd_write_some(w);
	}
}

static void waiter_flush(struct vidow_clip_waiter *w) {
	fd_write_some(w);
}

/* Wayland 客户端来粘：数据齐了直接写，没齐先攥着 fd。 */
static void serve_to_fd(struct vidow_clip_selection *sel, const char *mime,
		int fd) {
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) {
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	}

	struct vidow_clip_mime *m = sel_find(sel, mime);
	if (m == NULL && mime_is_text(mime)) {
		m = sel_pick(sel, NULL);
		if (m != NULL && !mime_is_text(m->mime)) {
			m = NULL;
		}
	}
	if (m == NULL || m->overflow) {
		close(fd); /* 没有这个类型 = 空 */
		return;
	}
	struct vidow_clip_waiter *w = waiter_new(sel, mime, fd);
	if (w != NULL) {
		waiter_flush(w);
	}
}

/* 发布给 Wayland 的源。每次发布 calloc 一个新的：seat 换选择时会把旧的
 * destroy 掉，而 wl_array_release 会把 mime_types.data 设成毒指针（0x4）——
 * 复用同一个结构体的话第二次 destroy 就是在毒指针上 free，直接段错误。 */
struct vidow_clip_source {
	struct wlr_data_source base;
	struct vidow_clip_selection *sel;
};

struct vidow_clip_psource {
	struct wlr_primary_selection_source base;
	struct vidow_clip_selection *sel;
};

static void wl_source_send(struct wlr_data_source *base, const char *mime_type,
		int32_t fd) {
	struct vidow_clip_source *src = wl_container_of(base, src, base);
	serve_to_fd(src->sel, mime_type, fd);
}

static void wl_source_destroy(struct wlr_data_source *base) {
	struct vidow_clip_source *src = wl_container_of(base, src, base);
	struct vidow_clip_selection *sel = src->sel;
	wlr_log(WLR_DEBUG, "剪贴板: %s 发布的源 %p 被销毁", sel->name, (void *)base);
	if (sel->published == base) {
		sel->published = NULL;
	}
	/* mime_types 不用管：wlr_data_source_destroy() 在调我们这个 destroy 之前
	 * 已经把它里面的字符串和数组都释放了（再释放一次就是双重释放 —— 数组里
	 * 会留下毒指针 0x4，下一次遍历直接段错误）。 */
	free(src);
}

static const struct wlr_data_source_impl wl_source_impl = {
	.send = wl_source_send,
	.destroy = wl_source_destroy,
};

static void wl_psource_send(struct wlr_primary_selection_source *base,
		const char *mime_type, int fd) {
	struct vidow_clip_psource *src = wl_container_of(base, src, base);
	serve_to_fd(src->sel, mime_type, fd);
}

static void wl_psource_destroy(struct wlr_primary_selection_source *base) {
	struct vidow_clip_psource *src = wl_container_of(base, src, base);
	struct vidow_clip_selection *sel = src->sel;
	if (sel->published_primary == base) {
		sel->published_primary = NULL;
	}
	/* mime_types 同 wl_source_destroy：wlroots 已经释放过了 */
	free(src);
}

static const struct wlr_primary_selection_source_impl wl_psource_impl = {
	.send = wl_psource_send,
	.destroy = wl_psource_destroy,
};

/* 关掉这一份选择：撤掉发布的源、清缓存、取消取数、给等待者收摊。 */
static void sel_shutdown_selection(struct vidow_clip_selection *sel) {
	sel_unpublish(sel);
	fetch_reset(sel);
	staging_reset(sel);
	sel_reset(sel);
	sel->have_last = false;
	struct vidow_clip_pull *pull, *ptmp;
	wl_list_for_each_safe(pull, ptmp, &sel->pulls, link) {
		pull_destroy(pull);
	}
	struct vidow_clip_waiter *w, *wtmp;
	wl_list_for_each_safe(w, wtmp, &sel->waiters, link) {
		waiter_destroy(w);
	}
	struct vidow_clip_out *out, *otmp;
	wl_list_for_each_safe(out, otmp, &sel->out, link) {
		out->waiting = false;
		out_destroy(out);
	}
}

/* Wayland/IPC 内容也必须成为 X selection owner，否则 X11 客户端的
 * SelectionRequest 不会进入我们的 event hook。 */
static void x11_claim_selection(struct vidow_clip_selection *sel) {
	struct vidow_clipboard *clip = sel->clip;
	xcb_connection_t *conn = x11_conn(clip);
	if (conn == NULL) {
		return;
	}
	xcb_set_selection_owner(conn, clip->window, sel->atom, XCB_CURRENT_TIME);
	sel->x_owner = clip->window;
	xcb_flush(conn);
}

static void x11_release_selection(struct vidow_clip_selection *sel) {
	struct vidow_clipboard *clip = sel->clip;
	xcb_connection_t *conn = x11_conn(clip);
	if (conn == NULL || sel->x_owner != clip->window) {
		return;
	}
	xcb_set_selection_owner(conn, XCB_WINDOW_NONE, sel->atom, XCB_CURRENT_TIME);
	sel->x_owner = XCB_WINDOW_NONE;
	xcb_flush(conn);
}

/* 把当前缓存发布到 seat 上（Wayland 客户端就能粘了）。类型表为空 = 撤掉。 */
static void sel_publish_serial(struct vidow_clip_selection *sel,
		uint32_t serial) {
	struct vidow_server *server = sel->server;
	if (!server->config.clipboard) {
		return;
	}
	if (sel->primary && !server->config.clipboard_primary) {
		return;
	}
	if (sel->nmimes == 0) {
		sel_unpublish(sel);
		return;
	}
	x11_claim_selection(sel);
	wlr_log(WLR_DEBUG, "剪贴板: 把 %s 的 %d 个类型发布到 Wayland", sel->name,
		sel->nmimes);

	if (sel->primary) {
		struct vidow_clip_psource *src = calloc(1, sizeof(*src));
		if (src == NULL) {
			return;
		}
		src->sel = sel;
		wlr_primary_selection_source_init(&src->base, &wl_psource_impl);
		for (int i = 0; i < sel->nmimes; ++i) {
			if (sel->mimes[i].overflow) {
				continue;
			}
			char **slot =
				wl_array_add(&src->base.mime_types, sizeof(*slot));
			if (slot != NULL) {
				*slot = strdup(sel->mimes[i].mime);
			}
		}
		sel->published_primary = &src->base;
		wlr_seat_set_primary_selection(server->seat, &src->base, serial);
	} else {
		struct vidow_clip_source *src = calloc(1, sizeof(*src));
		if (src == NULL) {
			return;
		}
		src->sel = sel;
		wlr_data_source_init(&src->base, &wl_source_impl);
		for (int i = 0; i < sel->nmimes; ++i) {
			if (sel->mimes[i].overflow) {
				continue;
			}
			char **slot =
				wl_array_add(&src->base.mime_types, sizeof(*slot));
			if (slot != NULL) {
				*slot = strdup(sel->mimes[i].mime);
			}
		}
		sel->published = &src->base;
		wlr_seat_set_selection(server->seat, &src->base, serial);
	}
}

/* Explicit new selections (X11/IPC/history) get a new ordering serial. */
static void sel_publish(struct vidow_clip_selection *sel) {
	sel_publish_serial(sel, wl_display_next_serial(sel->server->display));
}

/* 撤掉我们发布的那一份（seat 上的选择清空）。 */
static void sel_unpublish(struct vidow_clip_selection *sel) {
	struct vidow_server *server = sel->server;
	x11_release_selection(sel);
	wlr_log(WLR_DEBUG, "剪贴板: 撤掉 %s 发布的源（%p/%p）", sel->name,
		(void *)sel->published, (void *)sel->published_primary);
	if (sel->primary) {
		if (sel->published_primary != NULL &&
				server->seat->primary_selection_source ==
				sel->published_primary) {
			wlr_seat_set_primary_selection(server->seat, NULL,
				wl_display_next_serial(server->display));
		}
	} else {
		if (sel->published != NULL &&
				server->seat->selection_source == sel->published) {
			wlr_seat_set_selection(server->seat, NULL,
				wl_display_next_serial(server->display));
		}
	}
}

/* ============================================== 把 Wayland 客户端的数据拉进内存 */

static void pull_destroy(struct vidow_clip_pull *pull) {
	if (pull->source != NULL) {
		wl_event_source_remove(pull->source);
	}
	vidow_fdwatch_deregister(pull->fd, VIDOW_FDWATCH_CLIPBOARD, pull);
	close(pull->fd);
	wl_list_remove(&pull->link);
	free(pull->mime);
	free(pull);
}

/* 一次拉取结束：拿到字节就算这个类型有了；一个字节都没拿到（客户端不给 / 超时）
 * 就把这个类型标成「拿不到」—— 不能让它一直挂着 incomplete，否则 X11 那边来问的
 * 请求会白等 5 秒（踩过：日志里「数据还没取全，先攥着」，5 秒后只能回「没有」）。 */
static void derive_uri_list(struct vidow_clip_selection *sel);

static void pull_finish(struct vidow_clip_pull *pull, const char *why) {
	struct vidow_clip_selection *sel = pull->sel;
	struct vidow_clip_mime *m = sel_find(sel, pull->mime);
	if (m != NULL && !m->overflow) {
		if (m->len > 0) {
			m->complete = true;
		} else {
			wlr_log(WLR_DEBUG, "剪贴板: 客户端没给 %s 的数据（%s），这个类型不提供",
				pull->mime, why);
			m->overflow = true;
		}
	}
	pull_destroy(pull);
	/* 最后一个类型也拉完了：这份内容齐了，记一条历史。中途的快照会存下半截内容，
	 * 所以只在 pulls 空了以后记。 */
	if (wl_list_empty(&sel->pulls)) {
		derive_uri_list(sel);
		history_push(sel);
	}
	sel_flush_waiters(sel);
	sel_flush_outs(sel);
	sel_emit(sel, why);
}

static int pull_readable(int fd, uint32_t mask, void *data) {
	struct vidow_clip_pull *pull = data;
	struct vidow_clip_selection *sel = pull->sel;
	size_t max = clip_max(sel->server);
	unsigned char buf[64 * 1024];

	/* 注意：**不能**看到 HANGUP 就直接收摊。客户端常常是「把数据写进管道、
	 * 马上关掉写端」，这时 epoll 一次就报 EPOLLIN|EPOLLHUP，管道里还有数据没读
	 * 走 —— 先读干净，读到 0（真 EOF）才算完。踩过这个：五个类型里随机两三个被
	 * 当成「客户端没给数据」，X11 那边就粘不到东西。 */
	while (true) {
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n > 0) {
			struct vidow_clip_mime *m = sel_find(sel, pull->mime);
			if (m != NULL) {
				mime_append(m, buf, (size_t)n, max);
			}
			continue;
		}
		if (n == 0) {
			pull_finish(pull, "wayland-pull-done");
			return 1;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
				/* 写端关了却没读到 EOF：不常见，当完 */
				pull_finish(pull, "wayland-pull-hup");
			}
			return 1;
		}
		pull_finish(pull, "wayland-pull-error");
		return 1;
	}
}

/* fdwatch 版：可读了就来拉一轮。返回 1 = 这一轮读完了（或者已经收摊）。 */
static int pull_fdw(int fd, void *data) {
	return pull_readable(fd, WL_EVENT_READABLE, data);
}

/* 给一份 Wayland 源建一条管道把数据拉过来。ds / ps 二选一。 */
static void pull_start(struct vidow_clip_selection *sel, struct wlr_data_source *ds,
		struct wlr_primary_selection_source *ps, const char *mime) {
	int fds[2];
	if (pipe(fds) == -1) {
		wlr_log_errno(WLR_ERROR, "剪贴板: pipe() 失败");
		return;
	}
	struct vidow_clip_pull *pull = calloc(1, sizeof(*pull));
	if (pull == NULL) {
		close(fds[0]);
		close(fds[1]);
		return;
	}
	pull->sel = sel;
	pull->mime = strdup(mime);
	pull->fd = fds[0];
	pull->deadline_ms = now_ms() + VIDOW_CLIP_TIMEOUT_MS;
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);
	wl_list_insert(&sel->pulls, &pull->link);
	/* 自唤醒 fd 不挂 add_fd（见 fdwatch.c）；起不来才退回事件循环。 */
	if (!vidow_fdwatch_register(pull->fd, VIDOW_FDWATCH_CLIPBOARD, pull_fdw, pull)) {
		pull->source = wl_event_loop_add_fd(
			wl_display_get_event_loop(sel->server->display), pull->fd,
			WL_EVENT_READABLE, pull_readable, pull);
	}
	sel_tick_arm(sel);

	if (ps != NULL) {
		wlr_primary_selection_source_send(ps, mime, fds[1]);
	} else {
		wlr_data_source_send(ds, mime, fds[1]);
	}
}

/* 客户端刚 set_selection：把它的每个类型都拉进内存（这样它退出了也还能粘）。 */
static void pull_all(struct vidow_clip_selection *sel, struct wlr_data_source *ds,
		struct wlr_primary_selection_source *ps) {
	struct wl_array *mime_types =
		ps != NULL ? &ps->mime_types : &ds->mime_types;
	char **mime_type_ptr;
	wl_array_for_each(mime_type_ptr, mime_types) {
		const char *mime = *mime_type_ptr;
		if (mime == NULL) {
			continue;
		}
		struct vidow_clip_mime *m = sel_find(sel, mime);
		if (m != NULL) {
			continue; /* wl-copy 常重复 offer 同一 MIME，只拉一次 */
		}
		m = sel_add(sel, mime, XCB_ATOM_NONE);
		if (m == NULL) {
			wlr_log(WLR_INFO, "剪贴板: 类型太多，%s 不跟了", mime);
			continue;
		}
		pull_start(sel, ds, ps, mime);
	}
}

/* ====================================================== seat 那一侧的监听 */

static void sel_handle_seat(struct vidow_clip_selection *sel) {
	struct vidow_server *server = sel->server;
	struct wlr_seat *seat = server->seat;

	if (!server->config.clipboard) {
		return;
	}

	wlr_log(WLR_DEBUG, "剪贴板: %s 收到 seat 事件（src=%p published=%p has_data=%d）",
		sel->name,
		(void *)(sel->primary ? (void *)seat->primary_selection_source
			: (void *)seat->selection_source),
		(void *)(sel->primary ? (void *)sel->published_primary
			: (void *)sel->published),
		sel_has_data(sel));

	if (sel->primary) {
		if (!server->config.clipboard_primary) {
			return;
		}
		struct wlr_primary_selection_source *src =
			seat->primary_selection_source;
		if (src != NULL && src == sel->published_primary) {
			return; /* 我们自己刚发布的 */
		}
		/* 客户端退出 / 显式清空：缓存还在就重新发布一遍，这样两边都还能粘 */
		if (src == NULL) {
			if (sel_has_data(sel)) {
				/* Restoring the same contents must not supersede a copy request
				 * already queued behind the old source's destroy request. */
				sel_publish_serial(sel, seat->primary_selection_serial);
			} else {
				sel_unpublish(sel);
			}
			return;
		}
		sel_reset(sel);
		sel->from_x11 = false;
		x11_claim_selection(sel);
		pull_all(sel, NULL, src);
		sel_emit(sel, "wayland-set");
		return;
	}

	struct wlr_data_source *src = seat->selection_source;
	/* 注意：src 和 published 都是 NULL 时**不算**「我们自己发布的」—— 那是
	 * 「客户端退出了」，接下来要拿缓存重新发布一份（这个判断踩过：写成
	 * `src == sel->published` 的话两个 NULL 相等，客户端一退出就直接 return，
	 * 缓存里的东西再也发不出去）。 */
	if (src != NULL && src == sel->published) {
		return;
	}
	if (src == NULL) {
		if (sel_has_data(sel)) {
			/* This is persistence, not a new selection: retain its serial. */
			sel_publish_serial(sel, seat->selection_serial);
		} else {
			sel_unpublish(sel);
		}
		return;
	}
	sel_reset(sel);
	sel->from_x11 = false;
	x11_claim_selection(sel);
	pull_all(sel, src, NULL);
	sel_emit(sel, "wayland-set");
}

static void handle_seat_set_selection(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_clip_selection *sel =
		wl_container_of(listener, sel, seat_set);
	sel_handle_seat(sel);
}

static void handle_seat_set_primary_selection(struct wl_listener *listener,
		void *data) {
	(void)data;
	struct vidow_clip_selection *sel =
		wl_container_of(listener, sel, seat_set);
	sel_handle_seat(sel);
}

/* ==================================================== 兜底：超时与重发 */

static int sel_tick(void *data) {
	struct vidow_clip_selection *sel = data;
	uint64_t now = now_ms();
	bool busy = false;

	if (sel->fetch.active && now - sel->fetch.started_ms > VIDOW_CLIP_TIMEOUT_MS) {
		wlr_log(WLR_INFO, "剪贴板: %s 从 X11 取数超时（对面不响应）",
			sel->name);
		sel->fetch.active = false;
		staging_reset(sel);
		sel_flush_waiters(sel);
		sel_flush_outs(sel);
		sel_emit(sel, "x11-timeout");
	}

	struct vidow_clip_pull *pull, *ptmp;
	wl_list_for_each_safe(pull, ptmp, &sel->pulls, link) {
		if (now > pull->deadline_ms) {
			pull_finish(pull, "wayland-pull-timeout");
		} else {
			busy = true;
		}
	}

	struct vidow_clip_waiter *w, *wtmp;
	wl_list_for_each_safe(w, wtmp, &sel->waiters, link) {
		if (now > w->deadline_ms) {
			waiter_destroy(w);
		} else {
			busy = true;
		}
	}

	/* X 请求的超时账（下面那段 out_send / out_destroy 是往 X 侧发请求的）：
	 * 先对账 —— Xwayland 退出后连接没了，摘桥会把 out 全清掉。 */
	xcb_connection_t *conn = x11_conn(sel->clip);
	struct vidow_clip_out *out, *otmp;
	if (conn != NULL) {
		wl_list_for_each_safe(out, otmp, &sel->out, link) {
			if (now > out->deadline_ms) {
				if (out->waiting) {
					/* 它要的那个类型一直没拿到：如果手里有别的文本类型，
					 * 拿那份顶上（老 X11 程序要 STRING / COMPOUND_TEXT 是常事） */
					struct vidow_clip_mime *fb = sel_pick(sel, NULL);
					if (fb != NULL && mime_is_text(fb->mime)) {
						free(out->mime);
						out->mime = strdup(fb->mime);
						out->waiting = false;
						wlr_log(WLR_DEBUG, "剪贴板: %s 要的类型没拿到，"
							"拿 %s 顶上", sel->name, out->mime);
						out_send(out);
						continue;
					}
					wlr_log(WLR_DEBUG, "剪贴板: %s 一直没取到，先跟对面说没有",
						sel->name);
				} else {
					wlr_log(WLR_DEBUG, "剪贴板: %s 的 INCR 对面一直不取，收摊",
						sel->name);
				}
				out_destroy(out);
			} else {
				busy = true;
			}
		}
	}

	if (busy || sel->fetch.active) {
		wl_event_source_timer_update(sel->tick, 100);
	}
	return 0;
}

static void sel_tick_arm(struct vidow_clip_selection *sel) {
	if (sel->tick != NULL) {
		wl_event_source_timer_update(sel->tick, 100);
	}
}

/* ================================================== X11 -> 我们（取数） */

static void fetch_reset(struct vidow_clip_selection *sel) {
	sel->fetch.active = false;
	sel->fetch.next = -1;
	sel->fetch.expect_atom = XCB_ATOM_NONE;
	free(sel->fetch.targets);
	sel->fetch.targets = NULL;
	sel->fetch.ntargets = 0;
	sel->fetch.incr = false;
}

static void fetch_next(struct vidow_clip_selection *sel) {
	struct vidow_clipboard *clip = sel->clip;
	while (sel->fetch.next < sel->nstaging) {
		struct vidow_clip_mime *m = &sel->staging[sel->fetch.next];
		if (m->complete || m->overflow) {
			sel->fetch.next++;
			continue;
		}
		sel->fetch.incr = false;
		sel->fetch.expect_atom = m->atom;
		xcb_convert_selection(clip->conn, clip->window, sel->atom, m->atom,
			sel->property, sel->timestamp);
		xcb_flush(clip->conn);
		return;
	}

	/* 全取完了：换上去，然后才发布（发布会让 xwm 去抢 X owner） */
	sel->fetch.active = false;
	sel->fetch.next = 0;
	int n = 0;
	size_t bytes = 0;
	for (int i = 0; i < sel->nstaging; ++i) {
		if (sel->staging[i].complete) {
			n++;
			bytes += sel->staging[i].len;
		}
	}
	staging_commit(sel);
	/* X11 源也可能只给 x-special/gnome-copied-files（QQ 图片就是这条），
	 * 同样派生 text/uri-list，否则 Wayland 目标只要 uri-list 就粘不出来。 */
	derive_uri_list(sel);
	/* X11 来的这一份现在成了当前内容：记一条历史（见下面的 history_push）。 */
	history_push(sel);
	sel->from_x11 = true;
	sel_publish(sel);
	wlr_log(WLR_INFO, "剪贴板: 从 X11 取到 %s 的 %d 个类型（共 %zu 字节）",
		sel->name, n, bytes);
	sel_flush_waiters(sel);
	sel_flush_outs(sel);
	sel_emit(sel, "x11");
}

static void fetch_start(struct vidow_clip_selection *sel, xcb_timestamp_t time) {
	struct vidow_clipboard *clip = sel->clip;
	if (sel->fetch.active) {
		fetch_reset(sel);
	}
	/* 取数期间**不动**对外那一份，也不发布 —— 一发布 xwm 就去抢 X owner，
	 * 原 owner 就答不了我们的取数请求了（取全了在 commit 里一起换）。 */
	staging_reset(sel);
	sel->timestamp = (uint32_t)time;
	sel->fetch.active = true;
	sel->fetch.next = -1;
	sel->fetch.incr = false;
	/* 在等 TARGETS 这一份 */
	sel->fetch.expect_atom = clip->atom_targets;
	sel->fetch.started_ms = now_ms();
	wlr_log(WLR_DEBUG, "剪贴板: 向 X11 要 %s 的 TARGETS（requestor=%u "
		"selection=%u target=%u property=%u time=%u）", sel->name,
		clip->window, sel->atom, clip->atom_targets, sel->property,
		(unsigned)time);
	xcb_convert_selection(clip->conn, clip->window, sel->atom,
		clip->atom_targets, sel->property, time);
	xcb_flush(clip->conn);
	sel_tick_arm(sel);
}

/* TARGETS 回来了：定下类型表，发布出去（数据随后到，粘的时候等一会儿就是）。 */
static void fetch_targets(struct vidow_clip_selection *sel,
		xcb_get_property_reply_t *reply) {
	struct vidow_clipboard *clip = sel->clip;
	int n = xcb_get_property_value_length(reply) / 4;
	xcb_atom_t *atoms = xcb_get_property_value(reply);

	staging_reset(sel);
	for (int i = 0; i < n; ++i) {
		xcb_atom_t atom = atoms[i];
		if (atom == clip->atom_targets || atom == clip->atom_timestamp ||
				atom == clip->atom_multiple ||
				atom == clip->atom_delete ||
				atom == clip->atom_save_targets ||
				atom == XCB_ATOM_NONE) {
			continue; /* 这些是元数据，不是内容 */
		}
		char *mime = atom_to_mime(clip, atom);
		if (mime == NULL) {
			continue;
		}
		staging_add(sel, mime, atom);
		free(mime);
	}

	if (sel->nstaging == 0) {
		wlr_log(WLR_INFO, "剪贴板: X11 那边没给可用的类型");
		sel->fetch.active = false;
		sel_emit(sel, "x11-empty");
		return;
	}

	sel->fetch.next = 0;
	fetch_next(sel);
}

/* 一个类型的数据回来了（也可能只是 INCR 的开场白）。 */
static void fetch_data(struct vidow_clip_selection *sel,
		xcb_get_property_reply_t *reply, bool is_property_notify) {
	struct vidow_clipboard *clip = sel->clip;
	size_t max = clip_max(sel->server);
	struct vidow_clip_mime *m = NULL;
	if (sel->fetch.next >= 0 && sel->fetch.next < sel->nstaging) {
		m = &sel->staging[sel->fetch.next];
	}
	if (m == NULL) {
		return;
	}

	if (!is_property_notify && m->complete) {
		/* 这个 target 已经收完一份完整数据，又来一份完整回复：重复（owner
		 * 答了两遍 / 队列里剩的旧回复），追加就是把两份内容拼在一起 ——
		 * 丢掉。 */
		wlr_log(WLR_INFO, "剪贴板: %s 的 %s 重复回复，丢掉后到的", sel->name,
			m->mime);
		return;
	}

	size_t len = (size_t)xcb_get_property_value_length(reply);
	void *value = xcb_get_property_value(reply);

	if (is_property_notify) {
		/* INCR 的一块。0 长度 = 完事了 */
		if (len == 0) {
			if (!m->overflow) {
				m->complete = true;
			}
			xcb_delete_property(clip->conn, clip->window, sel->property);
			xcb_flush(clip->conn);
			sel->fetch.incr = false;
			sel->fetch.next++;
			fetch_next(sel);
			return;
		}
		mime_append(m, value, len, max);
		xcb_delete_property(clip->conn, clip->window, sel->property);
		xcb_flush(clip->conn);
		return;
	}

	if (reply->type == clip->atom_incr) {
		/* INCR：属性里是总长度，真数据要一块一块等 PropertyNotify */
		sel->fetch.incr = true;
		if (len >= 4) {
			uint32_t total;
			memcpy(&total, value, 4);
			wlr_log(WLR_DEBUG, "剪贴板: %s 走 INCR，共 %u 字节", m->mime,
				total);
		}
		xcb_delete_property(clip->conn, clip->window, sel->property);
		xcb_flush(clip->conn);
		return;
	}

	mime_append(m, value, len, max);
	if (!m->overflow) {
		m->complete = true;
	}
	xcb_delete_property(clip->conn, clip->window, sel->property);
	xcb_flush(clip->conn);
	sel->fetch.next++;
	fetch_next(sel);
}

static xcb_get_property_reply_t *get_selection_property(
		struct vidow_clip_selection *sel, bool delete) {
	struct vidow_clipboard *clip = sel->clip;
	xcb_get_property_cookie_t cookie = xcb_get_property(clip->conn, delete,
		clip->window, sel->property, XCB_GET_PROPERTY_TYPE_ANY, 0,
		0x1fffffff);
	return xcb_get_property_reply(clip->conn, cookie, NULL);
}

/* ================================================== 我们 -> X11（应答） */

static void out_destroy(struct vidow_clip_out *out) {
	struct vidow_clip_selection *sel = out->sel;
	/* 没发成 / 没等到的，给对面一个「没有」让它别干等 */
	xcb_selection_notify_event_t notify = {
		.response_type = XCB_SELECTION_NOTIFY,
		.time = XCB_CURRENT_TIME,
		.requestor = out->requestor,
		.selection = sel->atom,
		.target = out->target,
		.property = XCB_ATOM_NONE,
	};
	if (out->waiting) {
		xcb_send_event(sel->clip->conn, 0, out->requestor,
			XCB_EVENT_MASK_NO_EVENT, (const char *)&notify);
		xcb_flush(sel->clip->conn);
	}
	wl_list_remove(&out->link);
	free(out->mime);
	free(out);
}

static void send_notify(struct vidow_clip_selection *sel,
		xcb_selection_request_event_t *req, bool success) {
	xcb_selection_notify_event_t notify = {
		.response_type = XCB_SELECTION_NOTIFY,
		.time = req->time,
		.requestor = req->requestor,
		.selection = req->selection,
		.target = req->target,
		.property = success ? req->property : XCB_ATOM_NONE,
	};
	xcb_send_event(sel->clip->conn, 0, req->requestor,
		XCB_EVENT_MASK_NO_EVENT, (const char *)&notify);
	xcb_flush(sel->clip->conn);
}

static void out_send(struct vidow_clip_out *out) {
	struct vidow_clip_selection *sel = out->sel;
	struct vidow_clip_mime *m = sel_find(sel, out->mime);
	if (m == NULL || !m->complete || m->overflow) {
		return; /* 还没齐，等着 */
	}
	out->waiting = false;
	xcb_atom_t target = mime_to_atom(sel->clip, m->mime);

	if (m->len <= VIDOW_CLIP_INCR_CHUNK) {
		xcb_change_property(sel->clip->conn, XCB_PROP_MODE_REPLACE,
			out->requestor, out->property, target, 8, m->len, m->data);
		xcb_selection_notify_event_t notify = {
			.response_type = XCB_SELECTION_NOTIFY,
			.time = XCB_CURRENT_TIME,
			.requestor = out->requestor,
			.selection = sel->atom,
			.target = out->target,
			.property = out->property,
		};
		xcb_send_event(sel->clip->conn, 0, out->requestor,
			XCB_EVENT_MASK_NO_EVENT, (const char *)&notify);
		xcb_flush(sel->clip->conn);
		wl_list_remove(&out->link);
		free(out->mime);
		free(out);
		return;
	}

	/* 大块走 INCR：先给一个「总长度」的属性，之后每收到一次属性删除就发一块，
	 * 最后发一块 0 长度的收尾。 */
	out->deadline_ms = now_ms() + VIDOW_CLIP_TIMEOUT_MS;
	uint32_t total = (uint32_t)m->len;
	xcb_change_property(sel->clip->conn, XCB_PROP_MODE_REPLACE, out->requestor,
		out->property, sel->clip->atom_incr, 32, 1, &total);
	/* 对面删属性我们才知道它读完了 —— 得盯着它那个窗口 */
	uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(sel->clip->conn, out->requestor,
		XCB_CW_EVENT_MASK, &mask);
	xcb_selection_notify_event_t notify = {
		.response_type = XCB_SELECTION_NOTIFY,
		.time = XCB_CURRENT_TIME,
		.requestor = out->requestor,
		.selection = sel->atom,
		.target = out->target,
		.property = out->property,
	};
	xcb_send_event(sel->clip->conn, 0, out->requestor,
		XCB_EVENT_MASK_NO_EVENT, (const char *)&notify);
	xcb_flush(sel->clip->conn);
	out->incr = true;
	out->offset = 0;
}

/* 对面删了属性 = 上一块读完了，发下一块（或收尾）。 */
static void out_next_chunk(struct vidow_clip_out *out) {
	struct vidow_clip_selection *sel = out->sel;
	struct vidow_clip_mime *m = sel_find(sel, out->mime);
	if (m == NULL || !m->complete) {
		out_destroy(out);
		return;
	}
	xcb_atom_t target = mime_to_atom(sel->clip, m->mime);
	if (out->sent_end) {
		out_destroy(out);
		return;
	}
	if (out->offset >= m->len) {
		xcb_change_property(sel->clip->conn, XCB_PROP_MODE_REPLACE,
			out->requestor, out->property, target, 8, 0, NULL);
		xcb_flush(sel->clip->conn);
		out->sent_end = true;
		return;
	}
	size_t chunk = m->len - out->offset;
	if (chunk > VIDOW_CLIP_INCR_CHUNK) {
		chunk = VIDOW_CLIP_INCR_CHUNK;
	}
	xcb_change_property(sel->clip->conn, XCB_PROP_MODE_REPLACE, out->requestor,
		out->property, target, 8, chunk, m->data + out->offset);
	xcb_flush(sel->clip->conn);
	out->offset += chunk;
	out->deadline_ms = now_ms() + VIDOW_CLIP_TIMEOUT_MS;
}

static void sel_flush_outs(struct vidow_clip_selection *sel) {
	/* Wayland 侧复制时也会走到这里（sel_publish）。Xwayland 已经退出的话
	 * out_send 会去 intern_atom / xcb_change_property —— 那是条死连接，
	 * 先对账摘掉（摘桥会把 sel->out 清空，见 vidow_clipboard_x11_detach）。 */
	if (x11_conn(sel->clip) == NULL) {
		return;
	}
	struct vidow_clip_out *out, *tmp;
	wl_list_for_each_safe(out, tmp, &sel->out, link) {
		if (out->waiting) {
			out_send(out);
		}
	}
}

/* X11 客户端来粘。target 找不到就回「没有」，不让它干等。 */
static void serve_request(struct vidow_clip_selection *sel,
		xcb_selection_request_event_t *req) {
	struct vidow_clipboard *clip = sel->clip;

	if (req->requestor == clip->window) {
		/* 我们自己的取数请求绕回来了（X 侧 owner 是 xwm 的代理窗口时会发生）：
		 * 直接回「没有」，让取数立刻失败，别干等超时。 */
		xcb_selection_notify_event_t notify = {
			.response_type = XCB_SELECTION_NOTIFY,
			.time = req->time,
			.requestor = req->requestor,
			.selection = req->selection,
			.target = req->target,
			.property = XCB_ATOM_NONE,
		};
		xcb_send_event(clip->conn, 0, req->requestor,
			XCB_EVENT_MASK_NO_EVENT, (const char *)&notify);
		xcb_flush(clip->conn);
		return;
	}
	/* 太老的请求不理（跟 wlroots 一个规矩）：owner 是刚换的，旧请求里的数据
	 * 已经不是它想要的那份了。 */
	if (req->time != XCB_CURRENT_TIME && req->time < sel->timestamp) {
		/* 太老：数据已经不是它想要的那一份了。但**必须回一条**「没有」，
		 * 不然对面（xclip 这类）会一直等下去。 */
		wlr_log(WLR_DEBUG, "剪贴板: 拒掉过期的 SelectionRequest（%u < %u）",
			req->time, sel->timestamp);
		send_notify(sel, req, false);
		return;
	}

	if (req->property == XCB_ATOM_NONE) {
		/* 「你能不能给这个 target」——只问不取，回个成功/失败就行 */
		bool can = false;
		if (req->target == clip->atom_targets ||
				req->target == clip->atom_timestamp) {
			can = true;
		} else {
			char *mime = atom_to_mime(clip, req->target);
			can = mime != NULL && sel_find(sel, mime) != NULL;
			free(mime);
		}
		send_notify(sel, req, can);
		return;
	}

	if (req->target == clip->atom_targets) {
		xcb_atom_t targets[VIDOW_CLIP_MIMES_MAX + 2];
		int n = 0;
		targets[n++] = clip->atom_timestamp;
		targets[n++] = clip->atom_targets;
		for (int i = 0; i < sel->nmimes && n < VIDOW_CLIP_MIMES_MAX + 2; ++i) {
			if (sel->mimes[i].overflow) {
				continue;
			}
			targets[n++] = mime_to_atom(clip, sel->mimes[i].mime);
		}
		xcb_change_property(clip->conn, XCB_PROP_MODE_REPLACE, req->requestor,
			req->property, XCB_ATOM_ATOM, 32, n, targets);
		send_notify(sel, req, true);
		return;
	}

	if (req->target == clip->atom_timestamp) {
		xcb_change_property(clip->conn, XCB_PROP_MODE_REPLACE, req->requestor,
			req->property, XCB_ATOM_INTEGER, 32, 1, &sel->timestamp);
		send_notify(sel, req, true);
		return;
	}

	if (req->target == clip->atom_delete) {
		send_notify(sel, req, true);
		return;
	}

	char *mime = atom_to_mime(clip, req->target);
	struct vidow_clip_mime *m = mime != NULL ? sel_find(sel, mime) : NULL;
	/* 要的类型有、但拿不到（客户端没给 / 超了上限）：当成没有，走下面的兜底 */
	if (m != NULL && m->overflow) {
		m = NULL;
	}
	/* 老 X11 程序会要 STRING / COMPOUND_TEXT / TEXT，我们只有 UTF8 的时候拿
	 * 文本类型顶上（总比空手强）。 */
	if (m == NULL && mime != NULL && mime_is_text(mime)) {
		m = sel_pick(sel, NULL);
		if (m != NULL && !mime_is_text(m->mime)) {
			m = NULL;
		}
	}
	if (m == NULL) {
		wlr_log(WLR_DEBUG, "剪贴板: %s 没有 X11 要的类型 %s", sel->name,
			mime != NULL ? mime : "(未知)");
		free(mime);
		send_notify(sel, req, false);
		return;
	}

	struct vidow_clip_out *out = calloc(1, sizeof(*out));
	if (out == NULL) {
		free(mime);
		send_notify(sel, req, false);
		return;
	}
	out->sel = sel;
	out->requestor = req->requestor;
	out->property = req->property;
	out->target = req->target;
	out->mime = strdup(m->mime);
	out->waiting = !m->complete;
	out->deadline_ms = now_ms() + VIDOW_CLIP_TIMEOUT_MS;
	wl_list_insert(&sel->out, &out->link);
	free(mime);
	sel_tick_arm(sel);
	if (out->waiting) {
		wlr_log(WLR_DEBUG, "剪贴板: %s 数据还没取全（%s len=%zu complete=%d "
			"overflow=%d），先攥着 X11 的请求", sel->name, out->mime, m->len,
			m->complete, m->overflow);
		return;
	}
	out_send(out);
}

/* ================================================== X 侧事件钩子 */

static bool event_is_ours(struct vidow_clipboard *clip, xcb_generic_event_t *event,
		struct vidow_clip_selection **sel_out) {
	struct vidow_clip_selection *sel = NULL;
	switch (event->response_type & XCB_EVENT_RESPONSE_TYPE_MASK) {
	case XCB_SELECTION_REQUEST: {
		xcb_selection_request_event_t *req =
			(xcb_selection_request_event_t *)event;
		if (req->selection == clip->atom_clipboard) {
			sel = &clip->clip;
		} else if (req->selection == clip->atom_primary &&
				clip->server->config.clipboard_primary) {
			sel = &clip->primary;
		}
		break;
	}
	case XCB_SELECTION_NOTIFY: {
		xcb_selection_notify_event_t *ev =
			(xcb_selection_notify_event_t *)event;
		if (ev->requestor != clip->window) {
			break;
		}
		if (ev->selection == clip->atom_clipboard) {
			sel = &clip->clip;
		} else if (ev->selection == clip->atom_primary &&
				clip->server->config.clipboard_primary) {
			sel = &clip->primary;
		}
		/* 野回复的甄别在 x11_event 的取数分支里做（那里有日志和丢包），
		 * 这里只按 selection 路由。 */
		break;
	}
	case XCB_PROPERTY_NOTIFY: {
		xcb_property_notify_event_t *ev = (xcb_property_notify_event_t *)event;
		if (ev->window == clip->window) {
			/* 我们自己在取数：INCR 的一块到了 */
			if (ev->atom == clip->clip.property) {
				sel = &clip->clip;
			} else if (ev->atom == clip->primary.property) {
				sel = &clip->primary;
			}
			break;
		}
		/* 出方向 INCR：对面把属性删了，该发下一块了 */
		struct vidow_clip_selection *list[2] = {&clip->clip, &clip->primary};
		for (int i = 0; i < 2; ++i) {
			struct vidow_clip_out *out;
			wl_list_for_each(out, &list[i]->out, link) {
				if (out->incr && ev->window == out->requestor &&
						ev->atom == out->property) {
					sel = list[i];
					break;
				}
			}
			if (sel != NULL) {
				break;
			}
		}
		break;
	}
	default:
		if ((event->response_type - clip->xfixes_first) ==
				XCB_XFIXES_SELECTION_NOTIFY) {
			xcb_xfixes_selection_notify_event_t *ev =
				(xcb_xfixes_selection_notify_event_t *)event;
			if (ev->selection == clip->atom_clipboard) {
				sel = &clip->clip;
			} else if (ev->selection == clip->atom_primary &&
					clip->server->config.clipboard_primary) {
				sel = &clip->primary;
			}
		}
		break;
	}
	*sel_out = sel;
	return sel != NULL;
}

/* wlr_xwayland->user_event_handler：在 xwm 默认处理之前被调用，返回 true 就跳过
 * wlroots 的处理 —— 我们接管的正是「跳过」这件事。 */
static bool x11_event(void *data, xcb_generic_event_t *event) {
	struct vidow_server *server = data;
	if (server == NULL) {
		return false;
	}
	struct vidow_clipboard *clip = &server->clipboard;
	/* x11_conn()：Xwayland 重启过的话这条事件来自新 xwm 的连接，手上那条已经
	 * 死了 —— 当场摘桥（绝不去碰它），这一条交给 wlroots 默认处理，100ms 后
	 * x11_attach() 会用新连接重新接管（README 252）。 */
	if (!clip->active || x11_conn(clip) == NULL) {
		return false;
	}

	struct vidow_clip_selection *sel = NULL;
	if (!event_is_ours(clip, event, &sel)) {
		return false; /* 不认识的（含 DND）原样交给 wlroots */
	}

	switch (event->response_type & XCB_EVENT_RESPONSE_TYPE_MASK) {
	case XCB_SELECTION_REQUEST:
		serve_request(sel, (xcb_selection_request_event_t *)event);
		return true;

	case XCB_SELECTION_NOTIFY: {
		xcb_selection_notify_event_t *ev =
			(xcb_selection_notify_event_t *)event;
		if (ev->requestor == clip->window && sel->fetch.active &&
				ev->target != sel->fetch.expect_atom) {
			/* 野回复：上一次复制在途的 convert 现在才应答（典型：
			 * chromium/x-source-url，那里面是 app:// 链接）。它的字节要是
			 * 落进这一次的 staging，就是「链接拼着我复制的内容」一起粘
			 * 出去（用户 2026-10-30 报的那个多余前缀）。直接丢。 */
			wlr_log(WLR_INFO, "剪贴板: %s 丢一份野回复（target 对不上：等 %u "
				"来了 %u）", sel->name,
				(unsigned)sel->fetch.expect_atom, (unsigned)ev->target);
			return true;
		}
		if (ev->property == XCB_ATOM_NONE) {
			wlr_log(WLR_INFO, "剪贴板: %s 从 X11 取数失败（convert 被拒）",
				sel->name);
			sel->fetch.active = false;
			staging_reset(sel);
			sel_flush_waiters(sel);
			sel_flush_outs(sel);
			sel_emit(sel, "x11-denied");
			return true;
		}
		xcb_get_property_reply_t *reply = get_selection_property(sel, false);
		if (reply == NULL) {
			return true;
		}
		if (ev->target == clip->atom_targets && sel->fetch.next < 0) {
			fetch_targets(sel, reply);
		} else {
			fetch_data(sel, reply, false);
		}
		free(reply);
		return true;
	}

	case XCB_PROPERTY_NOTIFY: {
		xcb_property_notify_event_t *ev = (xcb_property_notify_event_t *)event;
		if (ev->window == clip->window) {
			if (!sel->fetch.active) {
				return true;
			}
			if (!sel->fetch.incr) {
				/* 非 INCR 阶段的属性通知不是取数的数据：正常流程是 owner
				 * 写好属性再发 SelectionNotify，数据在那一趟读。野的往里
				 * 拼就是又一个「多余前缀」的来源（上一次 INCR 的尾巴掉进
				 * 这一次的新条目）。 */
				return true;
			}
			xcb_get_property_reply_t *reply =
				get_selection_property(sel, false);
			if (reply != NULL) {
				fetch_data(sel, reply, true);
				free(reply);
			}
			return true;
		}
		struct vidow_clip_out *out;
		wl_list_for_each(out, &sel->out, link) {
			if (out->incr && ev->window == out->requestor &&
					ev->atom == out->property &&
					ev->state == XCB_PROPERTY_DELETE) {
				out_next_chunk(out);
				return true;
			}
		}
		return false;
	}

	default: { /* XFIXES_SELECTION_NOTIFY */
		xcb_xfixes_selection_notify_event_t *ev =
			(xcb_xfixes_selection_notify_event_t *)event;
		/* 同一个连接上，我们自己的窗口和 xwm 的代理窗口都注册了 XFixes，
		 * 所以同一件事会来两遍 —— 只认第一遍。 */
		if (sel->have_last && sel->last_owner == ev->owner &&
				sel->last_time == ev->timestamp) {
			return true;
		}
		sel->have_last = true;
		sel->last_owner = ev->owner;
		sel->last_time = ev->timestamp;
		sel->x_owner = ev->owner;
		if (ev->owner == clip->window) {
			return true;
		}
		if (ev->owner == clip->proxy_clipboard ||
				ev->owner == clip->proxy_primary) {
			/* xwm 抢 owner 抢到的是它自己的代理窗口：那是「我们自己刚发到
			 * X 那边的数据」，取回来只会把自己绕一圈 */
			return true;
		}
		if (ev->owner == XCB_WINDOW_NONE) {
			/* X 侧没人了：内容留在缓存里（就当个剪贴板管理器） */
			wlr_log(WLR_DEBUG, "剪贴板: %s 的 X 侧 owner 没了，缓存留着",
				sel->name);
			sel_emit(sel, "x11-clear");
			return true;
		}
		wlr_log(WLR_DEBUG, "剪贴板: %s 的 X 侧换 owner 了（window=%u），取一份",
			sel->name, ev->owner);
		fetch_start(sel, ev->timestamp);
		return true;
	}
	}
	return false;
}

/* xwm 自己有两个代理窗口（CLIPBOARD / PRIMARY 各一个：10x10、没映射、
 * InputOutput、不是 override-redirect —— 见 wlroots 的 xwm_selection_init）。
 * 它们出现在 owner 通知里要跳过，否则会把我们刚发到 X 那边的数据再取回来。
 * 识别办法：CLIPBOARD_MANAGER 的 owner 一定是 CLIPBOARD 那个（xwm 自己抢的），
 * 形状相同的另一个就是 PRIMARY 的。我们自己那个窗口是 override-redirect 的
 * 1x1，不会被认错。 */
static void find_proxies(struct vidow_clipboard *clip) {
	xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(clip->conn));
	xcb_window_t root = it.data != NULL ? it.data->root : XCB_WINDOW_NONE;
	if (root == XCB_WINDOW_NONE) {
		return;
	}

	xcb_get_selection_owner_cookie_t mgr_cookie =
		xcb_get_selection_owner(clip->conn,
			intern_atom(clip->conn, "CLIPBOARD_MANAGER"));
	xcb_get_selection_owner_reply_t *mgr =
		xcb_get_selection_owner_reply(clip->conn, mgr_cookie, NULL);
	if (mgr != NULL) {
		clip->proxy_clipboard = mgr->owner;
		free(mgr);
	}

	xcb_query_tree_cookie_t tree_cookie = xcb_query_tree(clip->conn, root);
	xcb_query_tree_reply_t *tree =
		xcb_query_tree_reply(clip->conn, tree_cookie, NULL);
	if (tree == NULL) {
		return;
	}
	int n = xcb_query_tree_children_length(tree);
	xcb_window_t *children = xcb_query_tree_children(tree);
	xcb_window_t found[8];
	int nfound = 0;
	for (int i = 0; i < n && nfound < 8; ++i) {
		xcb_get_window_attributes_cookie_t attr_cookie =
			xcb_get_window_attributes(clip->conn, children[i]);
		xcb_get_window_attributes_reply_t *attr =
			xcb_get_window_attributes_reply(clip->conn, attr_cookie, NULL);
		if (attr == NULL) {
			continue;
		}
		bool candidate = attr->map_state != XCB_MAP_STATE_VIEWABLE &&
			attr->override_redirect == 0 &&
			attr->_class == XCB_WINDOW_CLASS_INPUT_OUTPUT;
		free(attr);
		if (!candidate) {
			continue;
		}
		xcb_get_geometry_cookie_t geo_cookie =
			xcb_get_geometry(clip->conn, children[i]);
		xcb_get_geometry_reply_t *geo =
			xcb_get_geometry_reply(clip->conn, geo_cookie, NULL);
		if (geo == NULL) {
			continue;
		}
		if (geo->width == 10 && geo->height == 10) {
			found[nfound++] = children[i];
		}
		free(geo);
	}
	free(tree);

	bool have_clip = false;
	for (int i = 0; i < nfound; ++i) {
		if (found[i] == clip->proxy_clipboard) {
			have_clip = true;
		}
	}
	if (!have_clip && nfound >= 2) {
		/* 认不出 CLIPBOARD_MANAGER（xwm 还没抢）：两个候选里 xwm 先建
		 * CLIPBOARD 的代理窗口，所以 id 小的那个是它。 */
		clip->proxy_clipboard = found[0] < found[1] ? found[0] : found[1];
		clip->proxy_primary = found[0] < found[1] ? found[1] : found[0];
	} else if (!have_clip && nfound == 1) {
		clip->proxy_clipboard = found[0];
	}
	if (have_clip) {
		for (int i = 0; i < nfound; ++i) {
			if (found[i] != clip->proxy_clipboard &&
					clip->proxy_primary == XCB_WINDOW_NONE) {
				clip->proxy_primary = found[i];
			}
		}
	}
	wlr_log(WLR_INFO, "剪贴板: xwm 代理窗口 CLIPBOARD=%u PRIMARY=%u"
		"（owner 通知里要跳过它们）",
		clip->proxy_clipboard, clip->proxy_primary);
}

/* ================================================== 初始化 / 收摊 */

static void sel_init(struct vidow_clipboard *clip,
		struct vidow_clip_selection *sel, enum vidow_clip_kind kind,
		xcb_atom_t atom, const char *name, bool primary) {
	sel->server = clip->server;
	sel->clip = clip;
	sel->kind = kind;
	sel->atom = atom;
	sel->name = name;
	sel->primary = primary;
	sel->nmimes = 0;
	sel->nstaging = 0;
	sel->property = XCB_ATOM_NONE;
	sel->x_owner = XCB_WINDOW_NONE;
	sel->from_x11 = false;
	sel->published = NULL;
	sel->published_primary = NULL;
	wl_list_init(&sel->out);
	wl_list_init(&sel->waiters);
	wl_list_init(&sel->pulls);
	fetch_reset(sel);
	sel->tick = wl_event_loop_add_timer(
		wl_display_get_event_loop(clip->server->display), sel_tick, sel);
}

/* ------------------------------------------------------ 摘桥 / 连接对账 */

/* 放掉 X 侧：clip->conn 这条「xwm 的连接」不再归我们。
 *
 * 为什么必须单独有这一步（README 252）：wlroots 给 Xwayland 传的是 -terminate，
 * 最后一个 X 客户端一走它就退出，而退出时 wlroots 会把 xwm 的 xcb 连接
 * xcb_disconnect 掉（xwm_destroy）—— clip->conn 当场变成野指针，下一次
 * xcb_convert_selection 就是段错误。
 *
 * 而且懒启动模式下 wlroots 复用的是同一个 wlr_xwayland_server 对象，Xwayland
 * 进程退出**不发任何信号**（日志里只有 "Restarting Xwayland (lazy)"；
 * server->events.destroy 只在 wlr_xwayland_destroy / display destroy 时发，
 * xwayland->events.destroy 也一样）。所以「什么时候摘」只能在使用点对账：
 * 见 x11_conn()。
 *
 * 不动 clip->active：那是「配置里开着剪贴板桥」的账，跟 X 侧连接的死活无关 ——
 * 摘掉之后 x11_attach() 会用新连接重新接管。 */
void vidow_clipboard_x11_detach(struct vidow_server *server) {
	struct vidow_clipboard *clip = &server->clipboard;
	if (clip->conn == NULL) {
		return;
	}

	/* X 侧还等着的请求（INCR 大块传输中途 Xwayland 就退了）：连接已经没了，
	 * 通知发不出去，直接收摊 —— waiting=false 让 out_destroy 不去碰 conn
	 * （跟 sel_shutdown_selection 一个道理）。 */
	struct vidow_clip_selection *list[2] = {&clip->clip, &clip->primary};
	for (int i = 0; i < 2; ++i) {
		struct vidow_clip_out *out, *otmp;
		wl_list_for_each_safe(out, otmp, &list[i]->out, link) {
			out->waiting = false;
			out_destroy(out);
		}
		fetch_reset(list[i]);
		/* 老 X 服务上的窗口号，重启后是别人的号，留着会误判 */
		list[i]->x_owner = XCB_WINDOW_NONE;
	}

	struct vidow_x11_server *x11 = vidow_x11_server_get();
	if (x11 != NULL) {
		if (x11->event_hook == x11_event) {
			x11->event_hook = NULL;
			x11->event_hook_data = NULL;
		}
		/* 只有「当前 XWM 用的就是这条连接」时才敢对它发请求：Xwayland 已经
		 * 退出过的话它早被释放了，碰它就是 UAF。 */
		if (x11->conn == clip->conn) {
			xcb_destroy_window(clip->conn, clip->window);
			xcb_flush(clip->conn);
		}
	}
	clip->conn = NULL;
	wlr_log(WLR_DEBUG, "剪贴板: X 侧连接已摘（等 Xwayland 起来再接管）");
}

/* 手上这条 X 连接还活着吗？Xwayland 退出后（重启前）它已经没了 —— 一发现就当场
 * 摘桥并返回 NULL，绝不让调用方拿着野指针去发 xcb 请求。
 *
 * 为什么不用信号：见 vidow_clipboard_x11_detach() 的注释（懒启动模式下
 * Xwayland 退出不发信号）。所有会碰 X 侧的地方都先过这里对账：x11_event（事件
 * 入口）、sel_flush_outs / sel_tick（Wayland 复制时往 X 回数据）、apply_config。 */
static xcb_connection_t *x11_conn(struct vidow_clipboard *clip) {
	if (clip->conn == NULL) {
		return NULL;
	}
	struct vidow_server *server = clip->server;
	if (server->xwayland_server == NULL ||
			vidow_x11_connection() != clip->conn) {
		vidow_clipboard_x11_detach(server);
		return NULL;
	}
	return clip->conn;
}

/* 接上 X 侧：拿 xwm 的连接、开自己的窗口、注册 XFixes、装上事件钩子。 */
static void x11_attach(struct vidow_server *server) {
	struct vidow_clipboard *clip = &server->clipboard;
	if (server->xwayland_server == NULL) {
		return;
	}
	xcb_connection_t *conn = vidow_x11_connection();
	if (conn == NULL) {
		return; /* Xwayland 还没起来 */
	}
	if (clip->conn == conn) {
		return; /* 已经接管这条连接了 */
	}
	if (clip->conn != NULL) {
		/* Xwayland 重启过：手上这条连接已经死了（xwm_destroy 里
		 * xcb_disconnect 掉了），先摘掉再用新连接接管。 */
		vidow_clipboard_x11_detach(server);
	}

	const xcb_query_extension_reply_t *xfixes =
		xcb_get_extension_data(conn, &xcb_xfixes_id);
	if (xfixes == NULL || !xfixes->present) {
		wlr_log(WLR_INFO, "剪贴板: X 侧没有 XFixes，不接管 X11 剪贴板"
			"（退回 wlroots 那套：只有 X11 窗口聚焦时才同步）");
		return;
	}
	xcb_xfixes_query_version_cookie_t version_cookie =
		xcb_xfixes_query_version(conn, 2, 0);
	xcb_xfixes_query_version_reply_t *version =
		xcb_xfixes_query_version_reply(conn, version_cookie, NULL);
	if (version == NULL || version->major_version < 2) {
		wlr_log(WLR_INFO, "剪贴板: XFixes 2.0 不可用，不接管 X11 剪贴板");
		free(version);
		return;
	}
	wlr_log(WLR_DEBUG, "剪贴板: XFixes %u.%u", version->major_version, version->minor_version);
	free(version);
	clip->xfixes_first = xfixes->first_event;
	clip->conn = conn;

	clip->atom_clipboard = intern_atom(conn, "CLIPBOARD");
	clip->atom_primary = intern_atom(conn, "PRIMARY");
	clip->atom_targets = intern_atom(conn, "TARGETS");
	clip->atom_timestamp = intern_atom(conn, "TIMESTAMP");
	clip->atom_utf8 = intern_atom(conn, "UTF8_STRING");
	clip->atom_text = intern_atom(conn, "TEXT");
	clip->atom_incr = intern_atom(conn, "INCR");
	clip->atom_wl_selection = intern_atom(conn, "_VIDOW_SELECTION");
	clip->atom_wl_primary = intern_atom(conn, "_VIDOW_PRIMARY");
	clip->clip.property = clip->atom_wl_selection;
	clip->primary.property = clip->atom_wl_primary;
	clip->clip.atom = clip->atom_clipboard;
	clip->primary.atom = clip->atom_primary;
	clip->atom_multiple = intern_atom(conn, "MULTIPLE");
	clip->atom_save_targets = intern_atom(conn, "SAVE_TARGETS");
	clip->atom_delete = intern_atom(conn, "DELETE");
	clip->atom_string = intern_atom(conn, "STRING");
	clip->atom_compound_text = intern_atom(conn, "COMPOUND_TEXT");

	xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(conn));
	xcb_window_t root = it.data != NULL ? it.data->root : XCB_WINDOW_NONE;
	clip->window = xcb_generate_id(conn);
	uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, clip->window, root, -1, -1,
		1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
		it.data != NULL ? it.data->root_visual : XCB_COPY_FROM_PARENT,
		XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, (uint32_t[]){1, mask});
	xcb_flush(conn);

	uint32_t xmask = XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
		XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
		XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
	xcb_xfixes_select_selection_input(conn, clip->window, clip->atom_clipboard,
		xmask);
	if (server->config.clipboard_primary) {
		xcb_xfixes_select_selection_input(conn, clip->window,
			clip->atom_primary, xmask);
	}
	xcb_flush(conn);

	wlr_log(WLR_DEBUG, "剪贴板: atoms CLIPBOARD=%u PRIMARY=%u TARGETS=%u "
		"TIMESTAMP=%u UTF8=%u TEXT=%u INCR=%u WL_SEL=%u WL_PRI=%u",
		clip->atom_clipboard, clip->atom_primary, clip->atom_targets,
		clip->atom_timestamp, clip->atom_utf8, clip->atom_text,
		clip->atom_incr, clip->atom_wl_selection, clip->atom_wl_primary);
	wlr_log(WLR_DEBUG, "剪贴板: window=%u property=%u/%u", clip->window,
		clip->clip.property, clip->primary.property);

	find_proxies(clip);
	/* 挂到我们自己的 XWM 事件流上。语义和原来的
	 * wlr_xwayland::user_event_handler 完全一样：认的事件吃掉（返回 true），
	 * 剩下的交给窗口管理。 */
	struct vidow_x11_server *x11 = vidow_x11_server_get();
	if (x11 != NULL) {
		x11->event_hook = x11_event;
		x11->event_hook_data = server;
	}

	/* X 那边可能已经有人占着剪贴板了（配置热切换回来时）：先取一份 */
	struct vidow_clip_selection *list[2] = {&clip->clip, &clip->primary};
	for (int i = 0; i < 2; ++i) {
		struct vidow_clip_selection *sel = list[i];
		if (sel->atom == XCB_ATOM_NONE) {
			continue;
		}
		if (sel->primary && !server->config.clipboard_primary) {
			continue;
		}
		xcb_get_selection_owner_cookie_t cookie =
			xcb_get_selection_owner(conn, sel->atom);
		xcb_get_selection_owner_reply_t *reply =
			xcb_get_selection_owner_reply(conn, cookie, NULL);
		if (reply == NULL) {
			continue;
		}
		xcb_window_t owner = reply->owner;
		free(reply);
		sel->x_owner = owner;
		if (owner != XCB_WINDOW_NONE && owner != clip->window &&
				owner != clip->proxy_clipboard &&
				owner != clip->proxy_primary) {
			fetch_start(sel, XCB_CURRENT_TIME);
		}
	}

	wlr_log(WLR_INFO, "剪贴板: 接管 X11(Xwayland) <-> Wayland 剪贴板"
		"（window=%u，不看焦点；primary=%s，每个类型上限 %d 字节）",
		clip->window, server->config.clipboard_primary ? "on" : "off",
		server->config.clipboard_max_bytes);
}

bool vidow_clipboard_init(struct vidow_server *server) {
	struct vidow_clipboard *clip = &server->clipboard;
	memset(clip, 0, sizeof(*clip));
	clip->server = server;
	clip->window = XCB_WINDOW_NONE;
	clip->proxy_clipboard = XCB_WINDOW_NONE;
	clip->proxy_primary = XCB_WINDOW_NONE;
	clip->active = server->config.clipboard;

	wl_list_init(&clip->history);

	sel_init(clip, &clip->clip, VIDOW_CLIP_CLIPBOARD, XCB_ATOM_NONE,
		"clipboard", false);
	sel_init(clip, &clip->primary, VIDOW_CLIP_PRIMARY, XCB_ATOM_NONE,
		"primary", true);

	clip->clip.seat_set.notify = handle_seat_set_selection;
	wl_signal_add(&server->seat->events.set_selection,
		&clip->clip.seat_set);
	clip->primary.seat_set.notify = handle_seat_set_primary_selection;
	wl_signal_add(&server->seat->events.set_primary_selection,
		&clip->primary.seat_set);

	if (!server->config.clipboard) {
		wlr_log(WLR_INFO, "剪贴板: 配置里关了，不接管"
			"（退回 wlroots 那套：只有 X11 窗口聚焦时才同步）");
	}
	return true;
}

static int attach_timer_cb(void *data) {
	struct vidow_server *server = data;
	server->clipboard.attach_timer = NULL;
	x11_attach(server);
	return 0;
}

void vidow_clipboard_x11_ready(struct vidow_server *server) {
	if (!server->config.clipboard) {
		return;
	}
	struct vidow_clipboard *clip = &server->clipboard;
	if (clip->attach_timer != NULL) {
		wl_event_source_remove(clip->attach_timer);
	}
	/* 等 100ms 再接管，别在 Xwayland 刚 ready 的这一刻做同步 round-trip。
	 *
	 * x11_attach 里有十几次同步 round-trip（intern atom、找代理窗口），这几毫秒
	 * 事件循环是卡住的；而「触发 Xwayland 启动的那个 X 客户端」恰好在这几毫秒里
	 * 建好并 map 自己的窗口 —— 它的 MapWindow 会落在 xwm 订上 root 的
	 * SubstructureRedirect 之前，于是根本没有 MapRequest 送给 xwm：X 服务器上有
	 * 这个窗口、合成器里没有它（没有 surface、没有视图、弹出物整个消失、父窗口
	 * 那张图全黑）。
	 *
	 * 实测（本机 wlroots 0.20.2）：ready 时立刻接管 → test-xwayland-popup.sh
	 * 4/4 次挂；推后 100ms → 3/3 次全过（X 服务器上的窗口树、xwininfo 的
	 * override-redirect 与坐标都对照过）。100ms 是「让 Xwayland 把启动那一波
	 * 请求处理完」的量级，不是精确值；配置关掉 / Xwayland 重启都会把这个定时器
	 * 换掉或取消。 */
	clip->attach_timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server->display), attach_timer_cb, server);
	wl_event_source_timer_update(clip->attach_timer, 100);
}

void vidow_clipboard_apply_config(struct vidow_server *server) {
	struct vidow_clipboard *clip = &server->clipboard;
	bool want = server->config.clipboard;
	bool want_primary = want && server->config.clipboard_primary;

	/* 历史条数改了：调小（或关掉）就得马上腾地方，不然「关掉历史」之后内存还
	 * 攥着几百 MB 不放。 */
	history_evict(server);

	if (!want_primary && (clip->primary.published != NULL ||
			clip->primary.published_primary != NULL ||
			clip->primary.nmimes > 0 || clip->primary.fetch.active)) {
		wlr_log(WLR_INFO, "剪贴板: primary 关了，不再同步选区");
		sel_shutdown_selection(&clip->primary);
	}

	if (!want) {
		if (clip->attach_timer != NULL) {
			wl_event_source_remove(clip->attach_timer);
			clip->attach_timer = NULL;
		}
		if (clip->active) {
			wlr_log(WLR_INFO, "剪贴板: 关了，交出剪贴板"
				"（退回 wlroots 那套）");
		}
		clip->active = false;
		sel_shutdown_selection(&clip->clip);
		sel_shutdown_selection(&clip->primary);
		struct vidow_x11_server *x11 = vidow_x11_server_get();
		if (x11 != NULL && x11->event_hook == x11_event) {
			x11->event_hook = NULL;
			x11->event_hook_data = NULL;
		}
		return;
	}

	if (!clip->active) {
		clip->active = true;
		wlr_log(WLR_INFO, "剪贴板: 开了，接管 X11 <-> Wayland 剪贴板");
	}
	/* 手上这条连接死了（Xwayland 退出后重启前）也算「没接上」：x11_conn 会把它
	 * 摘掉，下面 x11_attach 再用新连接接管。 */
	if (x11_conn(clip) == NULL) {
		x11_attach(server);
	}
	if (clip->conn != NULL) {
		struct vidow_x11_server *x11 = vidow_x11_server_get();
		if (x11 != NULL && x11->event_hook == NULL) {
			x11->event_hook = x11_event;
			x11->event_hook_data = server;
		}
		/* primary 中途打开：seat 上现成的那份（如果有）拉回来，X 那边的
		 * 通知也补注册一次 */
		if (server->config.clipboard_primary &&
				clip->primary.nmimes == 0 && !clip->primary.fetch.active) {
			struct wlr_primary_selection_source *psrc =
				server->seat->primary_selection_source;
			if (psrc != NULL && psrc != clip->primary.published_primary) {
				pull_all(&clip->primary, NULL, psrc);
				sel_emit(&clip->primary, "wayland-set");
			}
		}
		if (server->config.clipboard_primary &&
				clip->primary.property != XCB_ATOM_NONE) {
			uint32_t xmask =
				XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
				XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
				XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
			xcb_xfixes_select_selection_input(clip->conn, clip->window,
				clip->atom_primary, xmask);
			xcb_flush(clip->conn);
		}
		/* X 那边现在是谁在占着？占着就取一份过来 */
		struct vidow_clip_selection *list[2] = {&clip->clip, &clip->primary};
		for (int i = 0; i < 2; ++i) {
			struct vidow_clip_selection *sel = list[i];
			if (sel->fetch.active || sel->nmimes > 0) {
				continue;
			}
			xcb_get_selection_owner_cookie_t cookie =
				xcb_get_selection_owner(clip->conn, sel->atom);
			xcb_get_selection_owner_reply_t *reply =
				xcb_get_selection_owner_reply(clip->conn, cookie, NULL);
			if (reply == NULL) {
				continue;
			}
			xcb_window_t owner = reply->owner;
			free(reply);
			if (owner != XCB_WINDOW_NONE && owner != clip->window &&
					owner != clip->proxy_clipboard &&
					owner != clip->proxy_primary) {
				fetch_start(sel, XCB_CURRENT_TIME);
			}
		}
	}
}

void vidow_clipboard_finish(struct vidow_server *server) {
	struct vidow_clipboard *clip = &server->clipboard;

	if (clip->attach_timer != NULL) {
		wl_event_source_remove(clip->attach_timer);
		clip->attach_timer = NULL;
	}
	sel_unpublish(&clip->clip);
	sel_unpublish(&clip->primary);
	vidow_clipboard_history_clear(server);

	struct vidow_clip_selection *list[2] = {&clip->clip, &clip->primary};
	for (int i = 0; i < 2; ++i) {
		struct vidow_clip_selection *sel = list[i];
		wl_list_remove(&sel->seat_set.link);
		struct vidow_clip_pull *pull, *ptmp;
		wl_list_for_each_safe(pull, ptmp, &sel->pulls, link) {
			pull_destroy(pull);
		}
		struct vidow_clip_waiter *w, *wtmp;
		wl_list_for_each_safe(w, wtmp, &sel->waiters, link) {
			waiter_destroy(w);
		}
		struct vidow_clip_out *out, *otmp;
		wl_list_for_each_safe(out, otmp, &sel->out, link) {
			out->waiting = false; /* 退出路上不用再回通知了 */
			out_destroy(out);
		}
		if (sel->tick != NULL) {
			wl_event_source_remove(sel->tick);
			sel->tick = NULL;
		}
		fetch_reset(sel);
		sel_reset(sel);
		staging_reset(sel);
	}

	/* X 侧那条连接（自己的窗口、事件钩子）一起放掉，见 x11_detach。 */
	vidow_clipboard_x11_detach(server);
	clip->active = false;
}

/* ============================================================== 历史 */

/* 为什么要有：剪贴板桥本身只保一份「当前内容」，复制第二遍第一份就没了。GUI
 * （tools/vidowclip.py）和 `vidowmsg clipboard-history*` 要的是「刚才那几份还在，
 * 点一下能拿回来」—— 那就是这份环形缓存。
 *
 * 内存：每条按 mime_rank 留最多 VIDOW_CLIP_HIST_MIMES 份内容（浏览器复制一次带
 * html + plain + png 好几份，全留太占），总量由 clipboard_history（条数）和
 * clipboard_history_bytes（字节）两个上限一起管，超了从最旧那头淘汰。 */

static void hist_entry_free(struct vidow_clip_hist_entry *e) {
	if (e == NULL) {
		return;
	}
	for (int i = 0; i < e->nmimes; ++i) {
		mime_clear(&e->mimes[i]);
	}
	free(e);
}

static struct vidow_clip_hist_entry *hist_find(struct vidow_clipboard *clip,
		uint64_t id) {
	struct vidow_clip_hist_entry *e;
	wl_list_for_each(e, &clip->history, link) {
		if (e->id == id) {
			return e;
		}
	}
	return NULL;
}

/* 把当前内容拷成一条独立的历史条目。挑不出东西（全是空的）返回 NULL。 */
static struct vidow_clip_hist_entry *hist_snapshot(
		struct vidow_clip_selection *sel) {
	bool used[VIDOW_CLIP_MIMES_MAX] = {false};
	struct vidow_clip_hist_entry *e = calloc(1, sizeof(*e));
	if (e == NULL) {
		return NULL;
	}
	for (int k = 0; k < VIDOW_CLIP_HIST_MIMES; ++k) {
		int best = -1, best_rank = 1000;
		for (int i = 0; i < sel->nmimes; ++i) {
			struct vidow_clip_mime *m = &sel->mimes[i];
			if (used[i] || !m->complete || m->overflow || m->len == 0) {
				continue;
			}
			int rank = mime_rank(m->mime);
			if (rank < best_rank) {
				best_rank = rank;
				best = i;
			}
		}
		if (best < 0) {
			break;
		}
		used[best] = true;
		struct vidow_clip_mime *src = &sel->mimes[best];
		struct vidow_clip_mime *dst = &e->mimes[e->nmimes];
		dst->mime = strdup(src->mime);
		dst->data = malloc(src->len);
		if (dst->mime == NULL || dst->data == NULL) {
			/* dst 还没算进 e->nmimes，hist_entry_free() 看不到它，
			 * 得先把这一格自己清掉。 */
			free(dst->mime);
			free(dst->data);
			hist_entry_free(e);
			return NULL;
		}
		memcpy(dst->data, src->data, src->len);
		dst->len = src->len;
		dst->complete = true;
		dst->atom = XCB_ATOM_NONE;
		e->nmimes++;
		e->bytes += src->len;
	}
	if (e->nmimes == 0) {
		hist_entry_free(e);
		return NULL;
	}
	e->time_ms = now_ms();
	e->primary = sel->primary;
	e->from_x11 = sel->from_x11;
	return e;
}

/* 两份内容一模一样吗（类型名、长度、字节全比）。挑出来的类型顺序是确定的
 * （按 mime_rank），所以直接按下标比。 */
static bool hist_same(struct vidow_clip_hist_entry *a,
		struct vidow_clip_hist_entry *b) {
	if (a->nmimes != b->nmimes) {
		return false;
	}
	for (int i = 0; i < a->nmimes; ++i) {
		if (strcmp(a->mimes[i].mime, b->mimes[i].mime) != 0 ||
				a->mimes[i].len != b->mimes[i].len ||
				memcmp(a->mimes[i].data, b->mimes[i].data,
					a->mimes[i].len) != 0) {
			return false;
		}
	}
	return true;
}

/* 超了条数 / 内存预算就从最旧那头淘汰。兜底留一条：单条自己就超预算时也留着，
 * 否则「复制了一张 40MB 的图」会当场把整个历史清空。 */
static void history_evict(struct vidow_server *server) {
	struct vidow_clipboard *clip = &server->clipboard;
	int max = server->config.clipboard_history;
	if (max < 0) {
		max = 0;
	}
	size_t budget = server->config.clipboard_history_bytes > 0
		? (size_t)server->config.clipboard_history_bytes : SIZE_MAX;
	while (clip->hist_count > max ||
			(clip->hist_bytes > budget && clip->hist_count > 1)) {
		if (wl_list_empty(&clip->history)) {
			break;
		}
		struct vidow_clip_hist_entry *e =
			wl_container_of(clip->history.prev, e, link);
		wl_list_remove(&e->link);
		clip->hist_count--;
		clip->hist_bytes -= e->bytes;
		hist_entry_free(e);
	}
}

/* 当前内容换人了：记一条历史。去重（同一份内容再复制一次 = 把那条挪到最前面，
 * 不新增），并且只在数据齐了以后记 —— Wayland 那边还在拉的时候先不记，否则会
 * 存下半截内容。 */
static void history_push(struct vidow_clip_selection *sel) {
	struct vidow_server *server = sel->server;
	struct vidow_clipboard *clip = &server->clipboard;
	if (!server->config.clipboard || server->config.clipboard_history <= 0) {
		return;
	}
	if (!wl_list_empty(&sel->pulls) || !sel_has_data(sel)) {
		return;
	}
	struct vidow_clip_hist_entry *e = hist_snapshot(sel);
	if (e == NULL) {
		return;
	}
	struct vidow_clip_hist_entry *it;
	wl_list_for_each(it, &clip->history, link) {
		if (it->bytes != e->bytes || it->primary != e->primary ||
				!hist_same(it, e)) {
			continue;
		}
		hist_entry_free(e);
		wl_list_remove(&it->link);
		it->time_ms = now_ms();
		wl_list_insert(&clip->history, &it->link);
		vidow_ipc_emit_clipboard_history(server, "dup");
		return;
	}
	e->id = ++clip->hist_next_id;
	wl_list_insert(&clip->history, &e->link);
	clip->hist_count++;
	clip->hist_bytes += e->bytes;
	history_evict(server);
	vidow_ipc_emit_clipboard_history(server, "push");
}

/* 第 index 条（0 = 最新）。下标会随新条目飘，GUI 指认条目该用 id（见 ipc.c）。 */
struct vidow_clip_hist_entry *vidow_clipboard_history_at(
		struct vidow_server *server, int index) {
	struct vidow_clipboard *clip = &server->clipboard;
	struct vidow_clip_hist_entry *e;
	int i = 0;
	wl_list_for_each(e, &clip->history, link) {
		if (i++ == index) {
			return e;
		}
	}
	return NULL;
}

void vidow_clipboard_history_clear(struct vidow_server *server) {
	struct vidow_clipboard *clip = &server->clipboard;
	struct vidow_clip_hist_entry *e, *tmp;
	wl_list_for_each_safe(e, tmp, &clip->history, link) {
		wl_list_remove(&e->link);
		hist_entry_free(e);
	}
	clip->hist_count = 0;
	clip->hist_bytes = 0;
	vidow_ipc_emit_clipboard_history(server, "clear");
}

bool vidow_clipboard_history_delete(struct vidow_server *server, uint64_t id) {
	struct vidow_clipboard *clip = &server->clipboard;
	struct vidow_clip_hist_entry *e = hist_find(clip, id);
	if (e == NULL) {
		return false;
	}
	wl_list_remove(&e->link);
	clip->hist_count--;
	clip->hist_bytes -= e->bytes;
	hist_entry_free(e);
	vidow_ipc_emit_clipboard_history(server, "delete");
	return true;
}

bool vidow_clipboard_history_get(struct vidow_server *server, uint64_t id,
		const char *mime, unsigned char **data, size_t *len,
		const char **mime_out) {
	struct vidow_clip_hist_entry *e =
		hist_find(&server->clipboard, id);
	if (e == NULL) {
		return false;
	}
	struct vidow_clip_mime *m = mime_pick(e->mimes, e->nmimes, mime);
	if (m == NULL) {
		return false;
	}
	unsigned char *copy = malloc(m->len > 0 ? m->len : 1);
	if (copy == NULL) {
		return false;
	}
	memcpy(copy, m->data, m->len);
	*data = copy;
	*len = m->len;
	if (mime_out != NULL) {
		*mime_out = m->mime;
	}
	return true;
}

/* 把某条历史变成当前内容（两边都发布出去）：GUI 的「点一下复制回来」走这里。
 * 发布走的是和客户端复制同一条路（sel_publish），所以 X11 那边 xwm 会照旧去抢
 * owner、请求照样归我们答。 */
bool vidow_clipboard_history_use(struct vidow_server *server, uint64_t id) {
	struct vidow_clipboard *clip = &server->clipboard;
	if (!server->config.clipboard) {
		return false;
	}
	struct vidow_clip_hist_entry *e = hist_find(clip, id);
	if (e == NULL) {
		return false;
	}
	struct vidow_clip_selection *sel =
		e->primary ? &clip->primary : &clip->clip;
	if (e->primary && !server->config.clipboard_primary) {
		return false;
	}
	sel_reset(sel);
	sel->from_x11 = e->from_x11;
	for (int i = 0; i < e->nmimes; ++i) {
		struct vidow_clip_mime *src = &e->mimes[i];
		struct vidow_clip_mime *dst = sel_add(sel, src->mime, XCB_ATOM_NONE);
		if (dst == NULL) {
			break;
		}
		mime_append(dst, src->data, src->len, clip_max(server));
		dst->complete = true;
	}
	sel_publish(sel);
	/* 会命中去重：这条挪到最前面（刚用过的东西该在最上面）。 */
	history_push(sel);
	sel_emit(sel, "history-use");
	return true;
}

/* ------------------------------------------------- 文件复制（uri-list） */

/* 拼字符串用的小缓冲（这里要拼三份，长度事先不知道）。 */
struct clip_strbuf {
	char *buf;
	size_t len, cap;
};

static void sb_add(struct clip_strbuf *sb, const char *data, size_t len) {
	if (sb->len + len + 1 > sb->cap) {
		size_t cap = sb->cap > 0 ? sb->cap : 256;
		while (cap < sb->len + len + 1) {
			cap *= 2;
		}
		char *next = realloc(sb->buf, cap);
		if (next == NULL) {
			/* 失败了就把整条路封死（buf=NULL + cap=0）：后面谁再往里塞都塞不进，
			 * 最后统一按「拼不出来」处理。 */
			free(sb->buf);
			sb->buf = NULL;
			sb->cap = 0;
			return;
		}
		sb->buf = next;
		sb->cap = cap;
	}
	memcpy(sb->buf + sb->len, data, len);
	sb->len += len;
	sb->buf[sb->len] = '\0';
}

static void sb_add_str(struct clip_strbuf *sb, const char *s) {
	sb_add(sb, s, strlen(s));
}

/* QQ / GNOME 文件复制只给 x-special/gnome-copied-files（"copy\nfile://..."），
 * 不给 text/uri-list。目标程序（文件管理器、部分聊天窗口）只问 uri-list 时
 * 就会「明明有 URL 却粘不出来」。这里从 gnome 那份派生一份标准 uri-list：
 * 去掉 copy/cut 头，LF 换成 RFC 2483 的 CRLF。已有 uri-list 就不覆盖。 */
static void derive_uri_list(struct vidow_clip_selection *sel) {
	struct vidow_clip_mime *gnome =
		sel_find(sel, "x-special/gnome-copied-files");
	if (gnome == NULL || !gnome->complete || gnome->data == NULL ||
			gnome->len == 0) {
		return;
	}
	if (sel_find(sel, "text/uri-list") != NULL) {
		return;
	}
	const char *p = (const char *)gnome->data;
	const char *end = p + gnome->len;
	/* 第一行是 copy / cut，跳过 */
	const char *nl = memchr(p, '\n', (size_t)(end - p));
	if (nl != NULL) {
		p = nl + 1;
	}
	struct clip_strbuf out = {0};
	sb_add_str(&out, "");
	while (p < end) {
		const char *line_end = memchr(p, '\n', (size_t)(end - p));
		if (line_end == NULL) {
			line_end = end;
		}
		const char *line = p;
		size_t len = (size_t)(line_end - p);
		if (len > 0 && line[len - 1] == '\r') {
			len--;
		}
		if (len > 0 && line[0] != '#') {
			sb_add(&out, line, len);
			sb_add_str(&out, "\r\n");
		}
		p = line_end < end ? line_end + 1 : end;
	}
	if (out.buf == NULL) {
		return;
	}
	struct vidow_clip_mime *m = sel_add(sel, "text/uri-list", XCB_ATOM_NONE);
	if (m != NULL) {
		mime_append(m, out.buf, out.len, clip_max(sel->server));
		m->complete = true;
	}
	free(out.buf);
}


/* 路径 -> file:// URI。按 RFC 3986 只留 unreserved 和 '/'，其余百分号转义
 * （中文路径、空格、# 都在这里过一遍）。 */
static void sb_add_uri(struct clip_strbuf *sb, const char *path) {
	static const char hex[] = "0123456789ABCDEF";
	sb_add_str(sb, "file://");
	char abs[PATH_MAX];
	if (path[0] != '/') {
		char cwd[PATH_MAX];
		if (getcwd(cwd, sizeof(cwd)) != NULL) {
			int n = snprintf(abs, sizeof(abs), "%s/%s", cwd, path);
			if (n < 0 || (size_t)n >= sizeof(abs)) {
				return; /* 拼出来太长：宁可不给，也不给个截断的路径 */
			}
			path = abs;
		}
	}
	for (const unsigned char *p = (const unsigned char *)path; *p != '\0'; p++) {
		bool keep = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
			(*p >= '0' && *p <= '9') || *p == '-' || *p == '.' ||
			*p == '_' || *p == '~' || *p == '/';
		if (keep) {
			sb_add(sb, (const char *)p, 1);
		} else {
			char esc[3] = {'%', hex[*p >> 4], hex[*p & 0xf]};
			sb_add(sb, esc, 3);
		}
	}
}

/* 把一批文件路径放进剪贴板。三份类型：
 *   text/uri-list                  RFC 2483，每行一个 file:// URI
 *   x-special/gnome-copied-files   GNOME / KDE 的文件管理器认这个才真「复制文件」，
 *                                  只有 uri-list 时多半会粘成一段路径文本
 *   text/plain;charset=utf-8       给终端这类只要文本的（一行一个路径）
 * 路径不存在也照放（剪贴板是「客户端说的话」，不该由合成器去校验）。 */
bool vidow_clipboard_set_files(struct vidow_server *server, int kind,
		char *const *paths, int npaths) {
	struct vidow_clipboard *clip = &server->clipboard;
	struct vidow_clip_selection *sel =
		kind == VIDOW_CLIP_PRIMARY ? &clip->primary : &clip->clip;
	if (!server->config.clipboard || npaths <= 0 || paths == NULL) {
		return false;
	}
	if (kind == VIDOW_CLIP_PRIMARY && !server->config.clipboard_primary) {
		return false;
	}
	struct clip_strbuf uri = {0}, gnome = {0}, plain = {0};
	sb_add_str(&uri, "");
	sb_add_str(&gnome, "copy\n");
	sb_add_str(&plain, "");
	for (int i = 0; i < npaths; ++i) {
		if (paths[i] == NULL || paths[i][0] == '\0') {
			continue;
		}
		sb_add_uri(&uri, paths[i]);
		sb_add_str(&uri, "\r\n");
		/* gnome-copied-files 用 '\n' 分隔（不带 uri-list 那种 \r） */
		sb_add_uri(&gnome, paths[i]);
		sb_add_str(&gnome, "\n");
		sb_add_str(&plain, paths[i]);
		sb_add_str(&plain, "\n");
	}
	if (uri.buf == NULL || gnome.buf == NULL || plain.buf == NULL) {
		free(uri.buf);
		free(gnome.buf);
		free(plain.buf);
		return false;
	}
	static const char *mimes[3] = {
		"text/uri-list",
		"x-special/gnome-copied-files",
		"text/plain;charset=utf-8",
	};
	const char *datas[3] = {uri.buf, gnome.buf, plain.buf};
	sel_reset(sel);
	sel->from_x11 = false;
	for (int i = 0; i < 3; ++i) {
		struct vidow_clip_mime *m = sel_add(sel, mimes[i], XCB_ATOM_NONE);
		if (m == NULL) {
			break;
		}
		mime_append(m, datas[i], strlen(datas[i]), clip_max(server));
		m->complete = true;
	}
	free(uri.buf);
	free(gnome.buf);
	free(plain.buf);
	sel_publish(sel);
	history_push(sel);
	sel_emit(sel, "ipc-set-files");
	return true;
}

/* ================================================== IPC 用：取 / 塞 / 清 */

bool vidow_clipboard_get(struct vidow_server *server, int kind, const char *mime,
		unsigned char **data, size_t *len, const char **mime_out) {
	struct vidow_clip_selection *sel = kind == VIDOW_CLIP_PRIMARY
		? &server->clipboard.primary : &server->clipboard.clip;
	struct vidow_clip_mime *m = sel_pick(sel, mime);
	if (m == NULL) {
		return false;
	}
	unsigned char *copy = malloc(m->len > 0 ? m->len : 1);
	if (copy == NULL) {
		return false;
	}
	memcpy(copy, m->data, m->len);
	*data = copy;
	*len = m->len;
	if (mime_out != NULL) {
		*mime_out = m->mime;
	}
	return true;
}

bool vidow_clipboard_set(struct vidow_server *server, int kind, const char *mime,
		const void *data, size_t len) {
	struct vidow_clipboard *clip = &server->clipboard;
	struct vidow_clip_selection *sel = kind == VIDOW_CLIP_PRIMARY
		? &clip->primary : &clip->clip;
	if (!server->config.clipboard) {
		return false;
	}
	if (kind == VIDOW_CLIP_PRIMARY && !server->config.clipboard_primary) {
		return false;
	}
	sel_reset(sel);
	sel->from_x11 = false;
	struct vidow_clip_mime *m = sel_add(sel, mime, XCB_ATOM_NONE);
	if (m == NULL) {
		return false;
	}
	mime_append(m, data, len, clip_max(server));
	m->complete = true;
	sel_publish(sel);
	history_push(sel);
	sel_emit(sel, "ipc-set");
	return true;
}
bool vidow_clipboard_mime_is_text(const char *mime) {
	return mime != NULL && mime_is_text(mime);
}

bool vidow_clipboard_clear(struct vidow_server *server, int kind) {
	struct vidow_clipboard *clip = &server->clipboard;
	struct vidow_clip_selection *sel = kind == VIDOW_CLIP_PRIMARY
		? &clip->primary : &clip->clip;
	sel_reset(sel);
	sel_unpublish(sel);
	sel_emit(sel, "clear");
	return true;
}
