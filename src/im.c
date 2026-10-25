/* 输入法中继：zwp_text_input_v3 <-> zwp_input_method_v2。
 *
 * 两个协议是一对，合成器夹在中间搬数据：
 *
 *   客户端(GTK/Qt/浏览器) --text-input-v3--> 合成器 --input-method-v2--> fcitx5
 *   客户端(GTK/Qt/浏览器) <--text-input-v3-- 合成器 <--input-method-v2-- fcitx5
 *
 * 客户端说「我有输入框、光标在这、周围文字是这些」，输入法回 preedit 和
 * commit。除此之外还有两件协议规定只能由合成器干的事：
 *   - 候选框：输入法开的 popup surface 由我们渲染并摆位，光标位置得我们喂回去
 *   - 抢键盘：输入法要拿原始按键；抢到之后按键不再进 seat，由输入法决定
 *     消费还是用虚拟键盘回注
 */

#include "server.h"

#include <stdlib.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

/* --------------------------------------------------------------- helpers */

/* wlroots 把客户端 commit 的状态搬进 current、还是只留在 pending，各版本
 * 不一样。谁有数据用谁，两种都成立。 */
static const struct wlr_text_input_v3_state *input_state(
		const struct wlr_text_input_v3 *input) {
	if (input->current.surrounding.text != NULL ||
			!wlr_box_empty(&input->current.cursor_rectangle)) {
		return &input->current;
	}
	return &input->pending;
}

/* 输入法回给我们的状态同理。 */
static const struct wlr_input_method_v2_state *method_state(
		const struct wlr_input_method_v2 *method) {
	if (method->current.preedit.text != NULL ||
			method->current.commit_text != NULL ||
			method->current.delete.before_length > 0 ||
			method->current.delete.after_length > 0) {
		return &method->current;
	}
	return &method->pending;
}

/* 真正在等输入的那个输入框：客户端说要输入，且它的窗口有键盘焦点。 */
static struct vidow_text_input *active_text_input(struct vidow_server *server) {
	struct vidow_text_input *ti;
	wl_list_for_each(ti, &server->text_inputs, link) {
		if (ti->enabled && ti->input->focused_surface != NULL) {
			return ti;
		}
	}
	return NULL;
}

/* 唤醒 / 让输入法睡觉。输入法只在有输入框时才该吃键。 */
static void im_set_active(struct vidow_server *server, bool active) {
	struct vidow_im *im = &server->im;
	if (im->method == NULL || im->active == active) {
		return;
	}
	im->active = active;
	if (active) {
		wlr_input_method_v2_send_activate(im->method);
	} else {
		wlr_input_method_v2_send_deactivate(im->method);
	}
	wlr_log(WLR_DEBUG, "input method %s", active ? "activated" : "deactivated");
}

/* --------------------------------------------------------------- 候选框 */

static void im_popup_clear(struct vidow_im_popup *popup) {
	if (popup->popup == NULL) {
		if (popup->blur != NULL) {
			vidow_popup_blur_unregister(popup->blur, true);
			popup->blur = NULL;
		}
		return;
	}
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	/* 模糊节点挂在 layer_im 上，不跟着候选框的表面节点走：先自己收掉。 */
	if (popup->blur != NULL) {
		vidow_popup_blur_unregister(popup->blur, true);
		popup->blur = NULL;
	}
	if (popup->scene != NULL) {
		wlr_scene_node_destroy(&popup->scene->buffer->node);
	}
	popup->popup = NULL;
	popup->scene = NULL;
}

/* 把候选框摆到光标下面，并把光标在候选框坐标系里的位置告诉输入法。 */
static void im_popup_update(struct vidow_server *server) {
	struct vidow_im_popup *popup = &server->im_popup;
	if (popup->popup == NULL || popup->scene == NULL) {
		return;
	}
	struct vidow_text_input *ti = active_text_input(server);
	if (ti == NULL || ti->input->focused_surface == NULL) {
		return;
	}
	struct vidow_view *view =
		vidow_view_from_surface(server, ti->input->focused_surface);
	if (view == NULL) {
		return;
	}
	int vx, vy;
	if (!wlr_scene_node_coords(&view->scene_tree->node, &vx, &vy)) {
		return;
	}
	struct wlr_box rect = input_state(ti->input)->cursor_rectangle;
	/* 光标矩形是窗口 surface 的局部坐标，加上窗口在场景里的位置才是屏幕坐标 */
	int px = vx + view->fx_dx + rect.x;
	int py = vy + view->fx_dy + rect.y + rect.height;

	/* **把候选框夹在当前显示器范围内**（用户：「Fcitx5弹窗现在会跨屏，
	 * 应该夹到当前显示器范围内」）。
	 *
	 * 以前这里直接摆到光标正下方就完事，没有任何边界处理：光标在屏幕右半边
	 * / 底部时光标正下方放不下，候选框就伸出本屏、画到隔壁显示器上（场景图
	 * 是全局坐标的，隔壁那块屏照画落在自己坐标里的所有节点）。
	 *
	 * 夹取策略（纯夹，不翻）：
	 *   1. 右 / 下越界：往左 / 上挪回来，挪到屏幕边上为止；
	 *   2. 实在放不下（候选框比屏还宽 / 高）：贴左上角。
	 * 不做「翻到光标上方」—— 那要猜候选框高度、而且每次输入都会跳，纯夹
	 * 更稳（代价是光标贴屏幕最底时候选框会盖住光标那一行，可接受）。 */
	struct wlr_box obox = {0};
	struct wlr_output *out = view->output;
	if (out != NULL) {
		wlr_output_layout_get_box(server->output_layout, out, &obox);
	}
	if (obox.width > 0 && obox.height > 0) {
		/* 用**表面逻辑尺寸**，不是 wlr_buffer 的 width/height —— 后者是
		 * 物理像素，2 倍屏上会大出一倍，夹取就会过严（候选框明明放得下
		 * 也被往左上拽）。 */
		int pw = popup->scene->surface != NULL ?
			popup->scene->surface->current.width : 0;
		int ph = popup->scene->surface != NULL ?
			popup->scene->surface->current.height : 0;
		if (pw > 0 && ph > 0) {
			int max_x = obox.x + obox.width - pw;
			int max_y = obox.y + obox.height - ph;
			if (px > max_x) {
				px = max_x;
			}
			if (py > max_y) {
				py = max_y;
			}
			/* 左 / 上：挪到头就别再挪了（候选框比屏宽时 max_x < obox.x） */
			if (px < obox.x) {
				px = obox.x;
			}
			if (py < obox.y) {
				py = obox.y;
			}
		}
	}

	wlr_scene_node_set_position(&popup->scene->buffer->node, px, py);
	/* 候选框挪了：背后那块模糊的采样坐标 / 区域跟着变，下一帧重画。 */
	if (popup->blur != NULL) {
		vidow_popup_blur_invalidate(popup->blur);
	}
	/* 输入法只认候选框自己的坐标系：给它相对候选框左上角的光标矩形 */
	struct wlr_box rel = {
		.x = -rect.x,
		.y = -rect.y - rect.height,
		.width = rect.width,
		.height = rect.height,
	};
	wlr_input_popup_surface_v2_send_text_input_rectangle(popup->popup, &rel);
}

/* ---------------------------------------------------------- 客户端输入框 */

/* 把输入框的当前状态推给输入法：周围文字、内容类型、光标。 */
static void im_push_state(struct vidow_text_input *ti) {
	struct wlr_input_method_v2 *method = ti->server->im.method;
	if (method == NULL) {
		return;
	}
	const struct wlr_text_input_v3_state *state = input_state(ti->input);
	if (state->surrounding.text != NULL) {
		wlr_input_method_v2_send_surrounding_text(method,
			state->surrounding.text, state->surrounding.cursor,
			state->surrounding.anchor);
	}
	wlr_input_method_v2_send_content_type(method, state->content_type.hint,
		state->content_type.purpose);
	wlr_input_method_v2_send_text_change_cause(method, state->text_change_cause);
	wlr_input_method_v2_send_done(method);
}

static void text_input_enable(struct vidow_text_input *ti) {
	ti->enabled = true;
	ti->input->current_enabled = true;
	/* 窗口可能早就有键盘焦点了：补一次 enter，否则输入法拿不到周围文字 */
	vidow_im_focus(ti->server, ti->server->seat->keyboard_state.focused_surface);
	im_set_active(ti->server, true);
	im_push_state(ti);
	im_popup_update(ti->server);
	wlr_log(WLR_DEBUG, "text input enabled");
}

static void text_input_disable(struct vidow_text_input *ti) {
	ti->enabled = false;
	ti->input->current_enabled = false;
	/* 这里**不发 leave**：enter/leave 只跟 seat 的键盘焦点走（vidow_im_focus
	 * 那边管）。客户端自己说 disable 不代表它的表面丢了焦点；反过来说，
	 * 客户端收到 leave 就会回 disable + commit —— 我们发 leave、它回 disable、
	 * 我们 disable 又发 leave …… 和 wlroots 的 commit 事件凑成一个自激环
	 * （见 README 258）。 */
	if (active_text_input(ti->server) == NULL) {
		im_set_active(ti->server, false);
	}
	wlr_log(WLR_DEBUG, "text input disabled");
}

static void text_input_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_text_input *ti = wl_container_of(listener, ti, commit);

	/* 用我们自己的 enabled 判断开/关，不依赖 wlroots 有没有同步
	 * pending_enabled -> current_enabled */
	if (ti->input->pending_enabled != ti->enabled) {
		if (ti->input->pending_enabled) {
			text_input_enable(ti);
		} else {
			text_input_disable(ti);
		}
	}
	if (!ti->enabled) {
		return;
	}
	im_push_state(ti);
	im_popup_update(ti->server);
}

static void text_input_enable_event(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_text_input *ti = wl_container_of(listener, ti, enable);
	if (!ti->enabled) {
		text_input_enable(ti);
	}
}

static void text_input_disable_event(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_text_input *ti = wl_container_of(listener, ti, disable);
	if (ti->enabled) {
		text_input_disable(ti);
	}
}

static void text_input_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_text_input *ti = wl_container_of(listener, ti, destroy);
	struct vidow_server *server = ti->server;
	bool was_enabled = ti->enabled;

	wl_list_remove(&ti->enable.link);
	wl_list_remove(&ti->commit.link);
	wl_list_remove(&ti->disable.link);
	wl_list_remove(&ti->destroy.link);
	wl_list_remove(&ti->link);
	free(ti);

	if (was_enabled && active_text_input(server) == NULL) {
		im_set_active(server, false);
	}
}

static void server_new_text_input(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, new_text_input);
	struct wlr_text_input_v3 *input = data;

	struct vidow_text_input *ti = calloc(1, sizeof(*ti));
	if (ti == NULL) {
		return;
	}
	ti->server = server;
	ti->input = input;

	ti->enable.notify = text_input_enable_event;
	wl_signal_add(&input->events.enable, &ti->enable);
	ti->commit.notify = text_input_commit;
	wl_signal_add(&input->events.commit, &ti->commit);
	ti->disable.notify = text_input_disable_event;
	wl_signal_add(&input->events.disable, &ti->disable);
	ti->destroy.notify = text_input_destroy;
	wl_signal_add(&input->events.destroy, &ti->destroy);

	wl_list_insert(&server->text_inputs, &ti->link);
}

/* --------------------------------------------------------------- 输入法侧 */

static void im_popup_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_im_popup *popup = wl_container_of(listener, popup, commit);
	im_popup_update(popup->server);
}

static void im_popup_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_im_popup *popup = wl_container_of(listener, popup, destroy);
	im_popup_clear(popup);
}

static void im_new_popup(struct wl_listener *listener, void *data) {
	struct vidow_im *im = wl_container_of(listener, im, new_popup);
	struct vidow_server *server = im->server;
	struct wlr_input_popup_surface_v2 *surface = data;
	struct vidow_im_popup *popup = &server->im_popup;

	im_popup_clear(popup);
	popup->popup = surface;
	popup->scene = wlr_scene_surface_create(server->layer_im, surface->surface);
	if (popup->scene == NULL) {
		popup->popup = NULL;
		return;
	}
	popup->commit.notify = im_popup_commit;
	wl_signal_add(&surface->surface->events.commit, &popup->commit);
	popup->destroy.notify = im_popup_destroy;
	wl_signal_add(&surface->events.destroy, &popup->destroy);
	/* 候选框背后的模糊：节点挂在 layer_im 上，垫在候选框表面下面（位置每帧
	 * 按绝对坐标摆，见 im_popup_update）。 */
	popup->blur = vidow_popup_blur_register(server, server->layer_im,
		&popup->scene->buffer->node, surface->surface, NULL, "im", NULL);
	wlr_log(WLR_DEBUG, "input method popup surface");
	im_popup_update(server);
}

static void im_detach_method(struct vidow_im *im,
		struct wlr_input_method_v2 *method);

/* 输入法回话：把 preedit / commit / 删除请求转给客户端输入框。 */
static void im_commit(struct wl_listener *listener, void *data) {
	/* 注意：这个信号不带 data（wlroots 0.20 传 NULL），要用自己存的那个 */
	(void)data;
	struct vidow_im *im = wl_container_of(listener, im, commit);
	struct vidow_server *server = im->server;
	struct wlr_input_method_v2 *method = im->method;
	if (method == NULL) {
		return;
	}
	struct vidow_text_input *ti = active_text_input(server);
	if (ti == NULL) {
		/* 没有输入框在等：告诉它这次白说了，别攒着。
		 *
		 * **只在输入法还活跃时发**：wlroots 的 wlr_input_method_v2_send_unavailable()
		 * 里第一行就是 assert(input_method->active)。Proton 游戏全屏抢键盘会让
		 * 输入法失活，这时客户端还送过来一次 commit，我们再发 unavailable 就是
		 * 一次 SIGABRT（实测 2026-10-18 23:49:51 崩过一次，栈顶正是这里）。 */
		/* 这里**绝不能** send_unavailable：那会在 commit 信号的回调里
		 * 把 method 自己销毁掉（input_method_destroy 还要断言各条
		 * listener 已经摘干净），实测 2026-10-20 00:35 就是这么崩的
		 * （SIGABRT，栈顶 input_method_destroy 的 commit.listener_list
		 * 断言）。没有输入框就静默丢掉这一次 commit，输入法还在，
		 * 下一个输入框聚焦时会重新 activate。 */
		return;
	}
	struct wlr_text_input_v3 *input = ti->input;
	const struct wlr_input_method_v2_state *state = method_state(method);

	wlr_log(WLR_DEBUG, "im commit: preedit=[%s] commit=[%s] del=%u/%u",
		state->preedit.text != NULL ? state->preedit.text : "",
		state->commit_text != NULL ? state->commit_text : "",
		state->delete.before_length, state->delete.after_length);

	if (state->preedit.text != NULL) {
		wlr_text_input_v3_send_preedit_string(input, state->preedit.text,
			state->preedit.cursor_begin, state->preedit.cursor_end);
	}
	if (state->delete.before_length > 0 || state->delete.after_length > 0) {
		wlr_text_input_v3_send_delete_surrounding_text(input,
			state->delete.before_length, state->delete.after_length);
	}
	if (state->commit_text != NULL) {
		wlr_text_input_v3_send_commit_string(input, state->commit_text);
	}
	wlr_text_input_v3_send_done(input);
}

static void im_grab_destroy(struct wl_listener *listener, void *data);

/* 当前抓着键盘的输入法 grab（没有就是 NULL）。
 *
 * ⚠️ grab 期间键事件**必须**发给它 —— 以前 im->grab 只被赋值和清空，从来没用过，
 * 于是 fcitx5 一抓键盘所有按键就被丢掉（用户报的「启动了 Fcitx5 之后键盘都不能
 * 用了」）。这里存一份静态指针，键 / 修饰键的出口在 input.c 里查它。 */
static struct wlr_input_method_keyboard_grab_v2 *active_grab;

void vidow_im_grab_send_key(uint32_t time_msec, uint32_t keycode, bool pressed) {
	if (active_grab == NULL) {
		return;
	}
	wlr_input_method_keyboard_grab_v2_send_key(active_grab, time_msec, keycode,
		pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
}

static void im_grab_keyboard(struct wl_listener *listener, void *data) {
	struct vidow_im *im = wl_container_of(listener, im, grab_keyboard);
	struct wlr_input_method_keyboard_grab_v2 *grab = data;

	im->grab = grab;
	active_grab = grab;
	im->grab_destroy.notify = im_grab_destroy;
	wl_signal_add(&grab->events.destroy, &im->grab_destroy);

	/* 把当前键盘交给它（它会收到 keymap），并同步一次修饰键 */
	vidow_im_set_keyboard(im->server, wlr_seat_get_keyboard(im->server->seat));
	wlr_log(WLR_DEBUG, "input method grabbed the keyboard");
}

static void im_grab_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_im *im = wl_container_of(listener, im, grab_destroy);
	wl_list_remove(&im->grab_destroy.link);
	im->grab = NULL;
	active_grab = NULL;
	/* 把键盘**还给**焦点客户端。
	 *
	 * grab 期间键事件全给了输入法，客户端那边等于「键盘被拔了」；grab 一没，
	 * 必须重新 notify_enter（带当前修饰键）它才会继续收键 —— 少了这一步就是
	 * 用户报的「Fcitx5 启动的时候键盘无法获取输入」：fcitx5 起来时抓一次键盘、
	 * 抓完放掉，放掉之后没人还给客户端。 */
	{
		struct vidow_server *server = im->server;
		struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
		struct wlr_surface *focused =
			server->seat->keyboard_state.focused_surface;
		if (kb != NULL && focused != NULL) {
			wlr_seat_keyboard_notify_enter(server->seat, focused,
				kb->keycodes, kb->num_keycodes, &kb->modifiers);
		}
	}
	wlr_log(WLR_DEBUG, "input method released the keyboard");
}

/* 把一个 method 从 im 上摘干净（监听器全部摘掉、候选框收掉）。
 *
 * 必须在 wlr_input_method_v2_send_unavailable() **之前**调：wlroots 的
 * input_method_destroy() 会断言 commit / new_popup / grab_keyboard / destroy
 * 四条 listener_list 都为空，而我们的 listener 不摘就还在上面 —— 那就是
 * SIGABRT。 */
static void im_detach_method(struct vidow_im *im,
		struct wlr_input_method_v2 *method) {
	if (im == NULL || method == NULL || im->method != method) {
		return;
	}
	im_popup_clear(&im->server->im_popup);
	if (im->grab != NULL) {
		wl_list_remove(&im->grab_destroy.link);
		im->grab = NULL;
		active_grab = NULL;
	}
	wl_list_remove(&im->commit.link);
	wl_list_remove(&im->new_popup.link);
	wl_list_remove(&im->grab_keyboard.link);
	wl_list_remove(&im->destroy.link);
	wl_list_init(&im->commit.link);
	wl_list_init(&im->new_popup.link);
	wl_list_init(&im->grab_keyboard.link);
	wl_list_init(&im->destroy.link);
	im->method = NULL;
	im->active = false;
}

static void im_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_im *im = wl_container_of(listener, im, destroy);
	im_detach_method(im, im->method);
	wlr_log(WLR_INFO, "input method disconnected");
}

static void server_new_input_method(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, new_input_method);
	struct wlr_input_method_v2 *method = data;
	struct vidow_im *im = &server->im;

	if (im->method != NULL) {
		/* 一个就够：后来的直接拒绝。
		 *
		 * 这里**不发 send_unavailable**：wlroots 0.20 在输入法重连这条路上
		 * 会在 send_unavailable 里 assert（实测 2026-10-20 00:35 的 core，
		 * 栈顶正是 wlr_input_method_v2_send_unavailable）。第二个输入法
		 * 本来就不该接管，静默拒绝即可；发 unavailable 只是礼貌，不值得崩。 */
		wlr_log(WLR_INFO, "another input method tried to connect, refusing");
		return;
	}
	im->method = method;

	im->commit.notify = im_commit;
	wl_signal_add(&method->events.commit, &im->commit);
	im->new_popup.notify = im_new_popup;
	wl_signal_add(&method->events.new_popup_surface, &im->new_popup);
	im->grab_keyboard.notify = im_grab_keyboard;
	wl_signal_add(&method->events.grab_keyboard, &im->grab_keyboard);
	im->destroy.notify = im_destroy;
	wl_signal_add(&method->events.destroy, &im->destroy);

	wlr_log(WLR_INFO, "input method connected");
	/* 输入法晚于输入框启动：立刻把它唤醒 */
	im_set_active(server, active_text_input(server) != NULL);
}

/* ------------------------------------------------------------------- api */

bool vidow_im_owns_keyboard(struct vidow_server *server,
		struct vidow_keyboard *kb) {
	struct vidow_im *im = &server->im;
	/* 会话锁（S3）：锁定期间输入法不许抢键盘 —— 它抢到的按键会被回注给
	 * 「还在打字」的那个输入框，而那是个被藏起来的窗口（方案 §2 环节 3）。 */
	if (server->locked) {
		return false;
	}
	if (im->grab == NULL) {
		return false;
	}
	/* 输入法自己回注的按键不能再喂回去：那是它决定「这个键我不消费」的
	 * 产物，喂回去就自己喂自己，死循环。 */
	if (kb->virtual != NULL && im->method != NULL &&
			wl_resource_get_client(kb->virtual->resource) ==
			wl_resource_get_client(im->method->resource)) {
		return false;
	}
	return true;
}

void vidow_im_set_keyboard(struct vidow_server *server,
		struct wlr_keyboard *kb) {
	struct vidow_im *im = &server->im;
	if (im->grab == NULL || kb == NULL) {
		return;
	}
	/* 输入法靠这个 keymap 把回注的按键翻译回去，抢键盘时 seat 上可能还
	 * 没有键盘（第一个键盘刚被销毁），所以每次键盘变了都要重发一次 */
	wlr_input_method_keyboard_grab_v2_set_keyboard(im->grab, kb);
	wlr_input_method_keyboard_grab_v2_send_modifiers(im->grab, &kb->modifiers);
	wlr_log(WLR_DEBUG, "input method keyboard updated");
}

void vidow_im_key(struct vidow_server *server, uint32_t time_msec,
		uint32_t keycode, uint32_t state) {
	/* 会话锁（S3）：锁定期间一个按键都不给输入法（第二道闸，见
	 * vidow_im_owns_keyboard）。 */
	if (server->locked || server->im.grab == NULL) {
		return;
	}
	wlr_input_method_keyboard_grab_v2_send_key(server->im.grab, time_msec,
		keycode, state);
}

void vidow_im_modifiers(struct vidow_server *server, struct wlr_keyboard *kb) {
	/* 会话锁（S3）：同上，修饰键也不给。 */
	if (server->locked || server->im.grab == NULL) {
		return;
	}
	wlr_input_method_keyboard_grab_v2_send_modifiers(server->im.grab,
		&kb->modifiers);
}

void vidow_im_focus(struct vidow_server *server, struct wlr_surface *surface) {
	wlr_log(WLR_DEBUG, "im focus -> %p", (void *)surface);
	if (server->text_input_mgr == NULL) {
		return;
	}
	/* 会话锁（S3）：锁定期间不给任何输入框发 enter，而且把已经 enter 过的一律
	 * leave 掉 —— 输入法连着「被藏起来的那个输入框」，键盘 / 候选词就还漏着
	 * （方案 §2 环节 3 的 IME 短路）。 */
	if (server->locked) {
		struct vidow_text_input *it;
		wl_list_for_each(it, &server->text_inputs, link) {
			if (it->input->focused_surface != NULL) {
				wlr_text_input_v3_send_leave(it->input);
			}
		}
		im_set_active(server, false);
		return;
	}
	/* 焦点换到**另一个**输入框时，要让输入法整个重新激活一次。
	 *
	 * fcitx5 把「当前是中文还是英文」记在**它自己**那边，只在收到 activate
	 * 时重置回默认态。原来这里只做 enter/leave：两个输入框之间切换时
	 * active 一直是 true，im_set_active() 里 `im->active == active` 直接返回，
	 * 一次 activate / deactivate 都不发 —— 于是前一个窗口里切到的中文
	 * 被带进后一个窗口，本该是英文的输入框打出中文（用户 2026-10-21 报的
	 * 「fcitx5 当前输入法状态会从一个窗口被带到另一个窗口」）。
	 *
	 * 做法：先 deactivate 再 activate，中间不进 im->active 的早退分支。
	 * 只在「确实从一个输入框换到另一个」时做 —— 同一个 surface 重复 enter
	 * 是常态（客户端 focus_in/out 抖动），每次都重置会把用户正在打的中文
	 * 状态无故清掉。 */
	struct wlr_surface *prev = active_text_input(server) != NULL ?
		active_text_input(server)->input->focused_surface : NULL;
	bool switching = prev != NULL && surface != NULL && prev != surface;

	struct vidow_text_input *ti;
	wl_list_for_each(ti, &server->text_inputs, link) {
		/* 输入框属于某个客户端，那个客户端的窗口拿到焦点就 enter。注意不能
		 * 等客户端先 enable：客户端正是收到 enter 之后才会 enable（它有焦点
		 * 了才需要输入法），等它先开口就是死锁。 */
		bool mine = surface != NULL &&
			wl_resource_get_client(ti->input->resource) ==
			wl_resource_get_client(surface->resource);
		if (mine) {
			if (ti->input->focused_surface == surface) {
				continue; /* 已经停在这个 surface 上了 */
			}
			/* wlr_text_input_v3_send_enter() 要求 focused_surface 是 NULL，
			 * 而且 wlroots 0.20 里这是 assert。同一个客户端从一个窗口切到
			 * 另一个窗口时，得先 leave 再 enter —— 少这一步会直接把合成器
			 * abort 掉（Qt/KDE 的 portal 对话框就撞上了）。 */
			if (ti->input->focused_surface != NULL) {
				wlr_text_input_v3_send_leave(ti->input);
			}
			wlr_text_input_v3_send_enter(ti->input, surface);
		} else if (ti->input->focused_surface != NULL) {
			wlr_text_input_v3_send_leave(ti->input);
		}
	}
	im_set_active(server, active_text_input(server) != NULL);
	/* 换输入框：把输入法的自身状态机重置一次（见上面 switching 的说明）。
	 * im->active 这时已经是 true，im_set_active() 会因为「状态没变」早退，
	 * 所以这里直接发协议消息，并且把顺序摆成 deactivate -> activate。 */
	if (switching && server->im.method != NULL) {
		wlr_input_method_v2_send_deactivate(server->im.method);
		wlr_input_method_v2_send_activate(server->im.method);
		wlr_log(WLR_DEBUG, "im: 换输入框 → 重置输入法状态"
			"（前一个 %p，新的 %p）", (void *)prev, (void *)surface);
	}
	im_popup_update(server);
}

bool vidow_im_init(struct vidow_server *server) {
	wl_list_init(&server->text_inputs);
	server->im.server = server;
	server->im_popup.server = server;

	if (getenv("VIDOW_NO_TEXT_INPUT") == NULL) { /* TEMP 实验开关 */
	server->text_input_mgr = wlr_text_input_manager_v3_create(server->display);
	if (server->text_input_mgr == NULL) {
		wlr_log(WLR_ERROR, "failed to create text-input-manager-v3");
		return false;
	}
	server->new_text_input.notify = server_new_text_input;
	wl_signal_add(&server->text_input_mgr->events.new_text_input,
		&server->new_text_input);
	}

	server->input_method_mgr =
		wlr_input_method_manager_v2_create(server->display);
	if (server->input_method_mgr == NULL) {
		wlr_log(WLR_ERROR, "failed to create input-method-manager-v2");
		return false;
	}
	server->new_input_method.notify = server_new_input_method;
	wl_signal_add(&server->input_method_mgr->events.new_input_method,
		&server->new_input_method);

	return true;
}
