/* 指针约束：zwp_pointer_constraints_v1 + zwp_relative_pointer_manager_v1。
 *
 * 为什么缺不得（用户报的「xwayland 缺了什么协议」就是这两个）：
 *   - `zwp_relative_pointer_manager_v1` 是**相对位移**那条路。Xwayland 拿它做
 *     XInput2 的 raw motion：X11 的游戏（Steam 上 Proton 那批全是 X11）转视角
 *     要的就是「鼠标走了多少」而不是「鼠标在哪」。合成器不提供它，Xwayland 只能
 *     靠绝对坐标反推 —— 鼠标一顶到屏幕边（或者被别的窗口吃掉）就再也转不动，
 *     表现就是「视角卡住 / 抖 / 转不动」。
 *   - `zwp_pointer_constraints_v1` 是**锁定 / 夹住**。游戏锁鼠标（`locked_pointer`）
 *     要它；绘图软件把画笔夹在画布上（`confined_pointer`）也要它。没有它客户端
 *     只能自己 XWarpPointer 兜。
 * 两个都查过本机 `/usr/bin/Xwayland` 的字符串：它 bind 的就是这两个（外加
 * `zwp_keyboard_shortcuts_inhibit_manager_v1`、`zwp_xwayland_keyboard_grab_manager_v1`，
 * 那两个 wlroots 0.20 也没实现，是另一件事）。
 *
 * wlroots 只给协议对象（建 global、按表面查约束、发 activated/deactivated），
 * **夹住 / 钉住光标是合成器的活**。这里的策略（和 sway 一致）：
 *
 *   1. 指针底下的表面 == 约束那个表面时才激活；指针移开（或表面没了）就解除。
 *   2. 区域：客户端给了 region 就用它和表面输入区的交集，没给就用整个输入区。
 *   3. locked：光标钉在激活那一刻的位置（客户端给了 cursor_hint 就按 hint 摆），
 *      一步都不动；位移照常从 relative_pointer 发给客户端 —— 这就是「锁住」。
 *   4. confined：把这一步位移夹进区域（`wlr_region_confine`）；旧位置已经在区域
 *      外（窗口挪走了 / 区域刚改小）就先把它挪回区域中心。
 *
 * 光标位置是**后置**处理的：vidow 的指针入口（vidow_pointer_motion）在设备已经
 * 把光标挪过之后才被调用，所以这里拿到的是「已经走了一步」的位置，位移靠和上一次
 * 处理过的位置相减得到，夹住则是把光标再挪回去（wlr_cursor_warp 只挪位置、不发
 * 事件，不会递归）。
 *
 * 判据：./test-pointer-lock.sh（Wayland 客户端锁 + 夹；X11 客户端 XGrabPointer
 * 的 confine 走同一条路）。 */

#include <math.h>
#include <stdlib.h>

#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>

#include "json.h"
#include "server.h"

/* 和 hover / grab 共用指定表面的实际内容节点映射。场景只有平移及轴向缩放，
 * 采样得到独立 XY 比例及其逆映射，供 surface 局部的 hint / region 使用。 */
static bool constraint_origin(struct vidow_server *server,
		struct wlr_pointer_constraint_v1 *c, double *ox, double *oy,
		double *hit_x, double *hit_y) {
	struct vidow_view *view = vidow_view_from_surface(server, c->surface);
	double sx = 0, sy = 0, tx = 0, ty = 0;
	if (!vidow_view_surface_coords(view, c->surface, 0, 0, &sx, &sy) ||
			!vidow_view_surface_coords(view, c->surface, 1, 1, &tx, &ty)) {
		return false;
	}
	*hit_x = tx - sx;
	*hit_y = ty - sy;
	if (!isfinite(*hit_x) || !isfinite(*hit_y) ||
			!(*hit_x > 0.0) || !(*hit_y > 0.0)) {
		return false;
	}
	*ox = -sx / *hit_x;
	*oy = -sy / *hit_y;
	return isfinite(*ox) && isfinite(*oy);
}

/* 按客户端给的 cursor_hint 把光标摆过去（hint 是表面局部坐标），并把它当成
 * 新的钉子位置。协议里 hint 的用处是「锁生效时把光标摆过去」和「解锁时摆回来」，
 * 锁着的时候**不跟** —— Xwayland 会不停更新 hint 来表达「X 那边的指针在哪」，
 * 跟上去光标就满屏跑了。 */
static void apply_cursor_hint(struct vidow_server *server,
		struct wlr_pointer_constraint_v1 *c,
		const struct wlr_pointer_constraint_v1_state *state) {
	double ox = 0, oy = 0, hit_x = 1.0, hit_y = 1.0;
	if (!constraint_origin(server, c, &ox, &oy, &hit_x, &hit_y)) {
		return;
	}
	double px = ox + state->cursor_hint.x / hit_x;
	double py = oy + state->cursor_hint.y / hit_y;
	server->pointer_lock_x = px;
	server->pointer_lock_y = py;
	wlr_cursor_warp(server->cursor, NULL, px, py);
	server->pointer_last_x = px;
	server->pointer_last_y = py;
}

/* 区域：confined 才有意义（locked 走「钉住」那条路，区域留空 = 位移全挡掉）。 */
static void constraint_region_update(struct vidow_server *server) {
	struct wlr_pointer_constraint_v1 *c = server->active_constraint;
	if (c == NULL || c->type != WLR_POINTER_CONSTRAINT_V1_CONFINED) {
		pixman_region32_clear(&server->confine);
		return;
	}
	if (pixman_region32_not_empty(&c->current.region)) {
		pixman_region32_intersect(&server->confine, &c->surface->input_region,
			&c->current.region);
	} else {
		/* 客户端没给区域：整个输入区就是围栏（协议里就是这个意思）。 */
		pixman_region32_copy(&server->confine, &c->surface->input_region);
	}
}

/* 光标现在不在围栏里（窗口挪走了 / 区域刚改小）就挪到第一块区域的正中。 */
static void confine_warp_into_region(struct vidow_server *server) {
	struct wlr_pointer_constraint_v1 *c = server->active_constraint;
	if (c == NULL || c->type != WLR_POINTER_CONSTRAINT_V1_CONFINED) {
		return;
	}
	double ox = 0, oy = 0, hit_x = 1.0, hit_y = 1.0;
	if (!constraint_origin(server, c, &ox, &oy, &hit_x, &hit_y)) {
		return;
	}
	int n = 0;
	pixman_box32_t *boxes = pixman_region32_rectangles(&server->confine, &n);
	if (n == 0) {
		return; /* 空区域：没地方可去（协议里空区域 = 一步都不许动） */
	}
	double sx = (server->cursor->x - ox) * hit_x;
	double sy = (server->cursor->y - oy) * hit_y;
	if (pixman_region32_contains_point(&server->confine, (int)floor(sx),
			(int)floor(sy), NULL)) {
		return;
	}
	double cx = (boxes[0].x1 + boxes[0].x2) / 2.0;
	double cy = (boxes[0].y1 + boxes[0].y2) / 2.0;
	wlr_cursor_warp(server->cursor, NULL, ox + cx / hit_x, oy + cy / hit_y);
	server->pointer_last_x = server->cursor->x;
	server->pointer_last_y = server->cursor->y;
}

static void constraint_listeners_del(struct vidow_server *server) {
	if (!server->constraint_listeners) {
		return;
	}
	wl_list_remove(&server->constraint_destroy.link);
	wl_list_remove(&server->constraint_set_region.link);
	wl_list_remove(&server->constraint_commit.link);
	server->constraint_listeners = false;
}

static void constraint_clear(struct vidow_server *server) {
	constraint_listeners_del(server);
	server->active_constraint = NULL;
	pixman_region32_clear(&server->confine);
	server->pointer_last_x = server->cursor->x;
	server->pointer_last_y = server->cursor->y;
}

/* 客户端自己把约束销毁了（松开鼠标 / 窗口关了）。对象正在销毁，不再回调
 * send_deactivated —— wlroots 那边已经在拆了。 */
static void handle_constraint_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_server *server =
		wl_container_of(listener, server, constraint_destroy);
	if (server->active_constraint == NULL) {
		return;
	}
	/* 客户端销毁约束 = 它认为锁没了：把光标摆到它最后说的位置（和解除同一条收尾）。 */
	if (server->active_constraint->current.cursor_hint.enabled) {
		apply_cursor_hint(server, server->active_constraint,
			&server->active_constraint->current);
	}
	constraint_clear(server);
	wlr_log(WLR_INFO, "指针约束: 客户端销毁了约束，光标恢复");
}

static void handle_constraint_set_region(struct wl_listener *listener,
		void *data) {
	(void)data;
	struct vidow_server *server =
		wl_container_of(listener, server, constraint_set_region);
	constraint_region_update(server);
	confine_warp_into_region(server);
}

/* 区域是跟着表面 commit 一起到的：先激活、后收到区域是常态。 */
static void handle_constraint_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_server *server =
		wl_container_of(listener, server, constraint_commit);
	constraint_region_update(server);
	confine_warp_into_region(server);
}

static void constraint_deactivate(struct vidow_server *server) {
	struct wlr_pointer_constraint_v1 *c = server->active_constraint;
	if (c == NULL) {
		return;
	}
	bool locked = c->type == WLR_POINTER_CONSTRAINT_V1_LOCKED;
	/* 客户端给过 cursor_hint：解锁时把光标挪回它说的位置（协议要求的收尾）。 */
	if (c->current.cursor_hint.enabled) {
		apply_cursor_hint(server, c, &c->current);
	}
	constraint_clear(server);
	/* 这一句可能当场把约束销毁（oneshot 的 lifetime）—— 监听已经摘干净了。 */
	wlr_pointer_constraint_v1_send_deactivated(c);
	wlr_log(WLR_INFO, "指针约束: %s 解除", locked ? "锁定" : "夹住");
}

static void constraint_activate(struct vidow_server *server,
		struct wlr_pointer_constraint_v1 *c) {
	server->active_constraint = c;
	server->pointer_lock_x = server->cursor->x;
	server->pointer_lock_y = server->cursor->y;
	constraint_region_update(server);

	/* 客户端在锁之前说了「光标该在哪」：现在就摆过去，并且以后钉在那儿。
	 * 「建约束 → set_cursor_position_hint → commit」是标准顺序，而激活发生在
	 * 请求处理那一刻（commit 之前），所以还看得见 pending —— 两个都看（协议：
	 * 「锁生效之前设的 hint」要在锁生效时把光标摆过去）。锁**之后**再来的 hint
	 * 一律不管，只等解除时用（见 apply_cursor_hint 的说明）。 */
	if (c->current.cursor_hint.enabled) {
		apply_cursor_hint(server, c, &c->current);
	} else if (c->pending.cursor_hint.enabled) {
		apply_cursor_hint(server, c, &c->pending);
	}
	confine_warp_into_region(server);

	server->constraint_destroy.notify = handle_constraint_destroy;
	wl_signal_add(&c->events.destroy, &server->constraint_destroy);
	server->constraint_set_region.notify = handle_constraint_set_region;
	wl_signal_add(&c->events.set_region, &server->constraint_set_region);
	server->constraint_commit.notify = handle_constraint_commit;
	wl_signal_add(&c->surface->events.commit, &server->constraint_commit);
	server->constraint_listeners = true;

	wlr_pointer_constraint_v1_send_activated(c);
	struct vidow_view *view = vidow_view_from_surface(server, c->surface);
	wlr_log(WLR_INFO, "指针约束: %s 生效（%s，%s）",
		c->type == WLR_POINTER_CONSTRAINT_V1_LOCKED ? "锁定" : "夹住",
		view != NULL && vidow_view_title(view) != NULL ?
			vidow_view_title(view) : "(无题)",
		c->lifetime == ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT ?
			"persistent" : "oneshot");
}

/* 指针焦点换了：新表面有约束就激活，没有就解除。 */
void vidow_pointer_constraint_focus(struct vidow_server *server,
		struct wlr_surface *surface) {
	if (server->pointer_constraints == NULL) {
		return;
	}
	struct wlr_pointer_constraint_v1 *active = server->active_constraint;
	if (active != NULL && active->surface == surface) {
		return; /* 没变 */
	}
	constraint_deactivate(server);
	if (surface == NULL) {
		return;
	}
	struct wlr_pointer_constraint_v1 *c =
		wlr_pointer_constraints_v1_constraint_for_surface(
			server->pointer_constraints, surface, server->seat);
	if (c != NULL) {
		constraint_activate(server, c);
	}
}

/* 客户端刚建了一个约束：指针已经在它身上就当场生效（不然要等下一次移动）。 */
static void handle_new_constraint(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, new_constraint);
	struct wlr_pointer_constraint_v1 *c = data;
	if (c->seat != server->seat) {
		return;
	}
	struct wlr_surface *focus = server->seat->pointer_state.focused_surface;
	if (focus != NULL && focus == c->surface) {
		constraint_activate(server, c);
	}
}

/* 合成器要退出（display 销毁）了：wlroots 在拆指针约束协议的时候会断言
 * `events.new_constraint` 上没人挂着 —— 自己挂的监听得自己摘。 */
static void handle_constraints_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_server *server =
		wl_container_of(listener, server, constraints_destroy);
	wl_list_remove(&server->new_constraint.link);
	wl_list_remove(&server->constraints_destroy.link);
	constraint_listeners_del(server);
	server->active_constraint = NULL;
	server->pointer_constraints = NULL;
}

/* 指针动了一下（设备、注入、warp 都走 vidow_pointer_motion 这一条路）：
 * 先按上一步的位置算出位移 → 发给 relative_pointer → 锁定的钉回去、夹住的夹住。
 * 必须在命中测试之前调：位置定下来，指针焦点 / enter / motion 才对得上。 */
void vidow_pointer_constraint_motion(struct vidow_server *server,
		uint32_t time_msec) {
	double x = server->cursor->x;
	double y = server->cursor->y;
	double dx = x - server->pointer_last_x;
	double dy = y - server->pointer_last_y;

	/* 相对位移：锁定的指针就靠它（位置不动、位移照发）。协议里时间单位是微秒，
	 * wl_pointer 那条路用的是毫秒。 */
	if (server->relative_pointer != NULL && (dx != 0.0 || dy != 0.0)) {
		wlr_relative_pointer_manager_v1_send_relative_motion(
			server->relative_pointer, server->seat,
			(uint64_t)time_msec * 1000, dx, dy, dx, dy);
	}

	struct wlr_pointer_constraint_v1 *c = server->active_constraint;
	if (c != NULL) {
		double ox = 0, oy = 0, hit_x = 1.0, hit_y = 1.0;
		if (!constraint_origin(server, c, &ox, &oy, &hit_x, &hit_y)) {
			/* 表面还没摆进场景（刚 map）或者不是窗口：不夹，别把光标冻住。 */
		} else if (c->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
			/* 钉死在激活那一刻的位置（客户端给的 cursor_hint 只用来在
			 * **解除**的时候把光标摆回去，见 constraint_deactivate —— 锁着
			 * 的时候跟着 hint 走就不是锁了：Xwayland 会不停更新 hint 来
			 * 表达「X 那边的指针在哪」，跟上去光标就满屏跑）。 */
			double px = server->pointer_lock_x;
			double py = server->pointer_lock_y;
			if (x != px || y != py) {
				wlr_cursor_warp(server->cursor, NULL, px, py);
			}
		} else {
			/* cursor 已经是本次终点；围栏从上次处理后的位置出发，不能
			 * 再把本次位移加到终点上，否则会多走一步并提前撞到边界。 */
			double sx = (server->pointer_last_x - ox) * hit_x;
			double sy = (server->pointer_last_y - oy) * hit_y;
			double tx = 0, ty = 0;
			if (wlr_region_confine(&server->confine, sx, sy,
					(x - ox) * hit_x, (y - oy) * hit_y, &tx, &ty)) {
				double lx = ox + tx / hit_x;
				double ly = oy + ty / hit_y;
				if (lx != x || ly != y) {
					wlr_cursor_warp(server->cursor, NULL, lx, ly);
				}
			} else {
				/* 这一步的起点就不在围栏里：位移丢掉，把光标挪回围栏。 */
				confine_warp_into_region(server);
			}
		}
	}

	server->pointer_last_x = server->cursor->x;
	server->pointer_last_y = server->cursor->y;
}

const char *vidow_pointer_constraint_type(const struct vidow_server *server) {
	if (server->active_constraint == NULL) {
		return NULL;
	}
	return server->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED ?
		"locked" : "confined";
}

/* 现在有没有约束生效、是哪种、钉在哪、围着多大一块（写进当前 JSON 对象）。 */
void vidow_pointer_constraint_json(struct jbuf *b, struct vidow_server *server) {
	const char *type = vidow_pointer_constraint_type(server);
	if (type == NULL) {
		jb_null(b);
		return;
	}
	struct wlr_pointer_constraint_v1 *c = server->active_constraint;
	struct vidow_view *view = vidow_view_from_surface(server, c->surface);
	jb_obj(b);
	jb_kv_str(b, "type", type);
	jb_kv_str(b, "lifetime",
		c->lifetime == ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT ?
			"persistent" : "oneshot");
	jb_key(b, "window");
	if (view != NULL) {
		jb_uint(b, view->id);
	} else {
		jb_null(b);
	}
	jb_key(b, "cursor");
	jb_obj(b);
	jb_kv_double(b, "x", server->cursor->x);
	jb_kv_double(b, "y", server->cursor->y);
	jb_obj_end(b);
	jb_obj_end(b);
}

bool vidow_pointer_constraint_init(struct vidow_server *server) {
	pixman_region32_init(&server->confine);
	server->pointer_last_x = server->cursor->x;
	server->pointer_last_y = server->cursor->y;

	/* 相对指针先建：Xwayland 起来的时候就会 bind 它（XInput2 的 raw motion）。 */
	server->relative_pointer =
		wlr_relative_pointer_manager_v1_create(server->display);
	if (server->relative_pointer == NULL) {
		wlr_log(WLR_ERROR, "相对指针协议（zwp_relative_pointer_v1）建不出来");
		return false;
	}
	server->pointer_constraints =
		wlr_pointer_constraints_v1_create(server->display);
	if (server->pointer_constraints == NULL) {
		wlr_log(WLR_ERROR, "指针约束协议（zwp_pointer_constraints_v1）建不出来");
		return false;
	}
	server->new_constraint.notify = handle_new_constraint;
	wl_signal_add(&server->pointer_constraints->events.new_constraint,
		&server->new_constraint);
	server->constraints_destroy.notify = handle_constraints_destroy;
	wl_signal_add(&server->pointer_constraints->events.destroy,
		&server->constraints_destroy);
	wlr_log(WLR_INFO, "指针约束: zwp_pointer_constraints_v1 + "
		"zwp_relative_pointer_manager_v1 已就位");
	return true;
}
