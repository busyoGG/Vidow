#include "keybind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <xkbcommon/xkbcommon.h>

#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "server.h"

#include <linux/input-event-codes.h>
#include "workspace.h"

/* 键位表：名字 -> keysym。字母和数字是「同一个键」的两种写法（大小写都收），
 * 其余是 X11 惯用的那些名字。 */
static const struct {
	const char *name;
	xkb_keysym_t sym;
} key_names[] = {
	{ "return", XKB_KEY_Return },
	{ "enter", XKB_KEY_Return },
	{ "escape", XKB_KEY_Escape },
	{ "esc", XKB_KEY_Escape },
	{ "tab", XKB_KEY_Tab },
	{ "space", XKB_KEY_space },
	{ "grave", XKB_KEY_grave },
	{ "tilde", XKB_KEY_grave },
	{ "minus", XKB_KEY_minus },
	{ "equal", XKB_KEY_equal },
	{ "plus", XKB_KEY_plus },
	{ "comma", XKB_KEY_comma },
	{ "period", XKB_KEY_period },
	{ "slash", XKB_KEY_slash },
	{ "semicolon", XKB_KEY_semicolon },
	{ "apostrophe", XKB_KEY_apostrophe },
	{ "bracketleft", XKB_KEY_bracketleft },
	{ "bracketright", XKB_KEY_bracketright },
	{ "backslash", XKB_KEY_backslash },
	{ "backspace", XKB_KEY_BackSpace },
	{ "delete", XKB_KEY_Delete },
	{ "home", XKB_KEY_Home },
	{ "end", XKB_KEY_End },
	{ "pageup", XKB_KEY_Page_Up },
	{ "pagedown", XKB_KEY_Page_Down },
	{ "prior", XKB_KEY_Prior },
	{ "next", XKB_KEY_Next },
	{ "insert", XKB_KEY_Insert },
	{ "up", XKB_KEY_Up },
	{ "down", XKB_KEY_Down },
	{ "left", XKB_KEY_Left },
	{ "right", XKB_KEY_Right },
	{ "print", XKB_KEY_Print },
	{ "menu", XKB_KEY_Menu },
};

static bool ascii_key(char c, xkb_keysym_t *out) {
	if (c >= 'a' && c <= 'z') {
		*out = (xkb_keysym_t)XKB_KEY_a + (xkb_keysym_t)(c - 'a');
		return true;
	}
	if (c >= 'A' && c <= 'Z') {
		*out = (xkb_keysym_t)XKB_KEY_a + (xkb_keysym_t)(c - 'A');
		return true;
	}
	if (c >= '0' && c <= '9') {
		*out = (xkb_keysym_t)XKB_KEY_0 + (xkb_keysym_t)(c - '0');
		return true;
	}
	return false;
}

static bool key_sym(const char *name, xkb_keysym_t *out) {
	if (name == NULL || name[0] == '\0') {
		return false;
	}
	for (size_t i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++) {
		if (strcasecmp(key_names[i].name, name) == 0) {
			*out = key_names[i].sym;
			return true;
		}
	}
	return name[1] == '\0' && ascii_key(name[0], out);
}

/* "Mod+Shift+Q" -> 修饰键掩码 + 键。大小写不敏感，修饰键名字认几个别名。 */
static bool parse_trigger(const char *trigger, uint32_t *mods,
		xkb_keysym_t *sym, char *err, size_t errlen) {
	char buf[sizeof(((struct vidow_keybind *)0)->trigger)];
	snprintf(buf, sizeof(buf), "%s", trigger);

	uint32_t m = 0;
	bool have_key = false;
	const char *token = NULL;
	char *save = NULL;
	for (token = strtok_r(buf, "+", &save); token != NULL;
			token = strtok_r(NULL, "+", &save)) {
		if (strcasecmp(token, "mod") == 0 || strcasecmp(token, "super") == 0 ||
				strcasecmp(token, "logo") == 0 ||
				strcasecmp(token, "win") == 0) {
			m |= WLR_MODIFIER_LOGO;
		} else if (strcasecmp(token, "shift") == 0) {
			m |= WLR_MODIFIER_SHIFT;
		} else if (strcasecmp(token, "alt") == 0 ||
				strcasecmp(token, "mod1") == 0) {
			m |= WLR_MODIFIER_ALT;
		} else if (strcasecmp(token, "ctrl") == 0 ||
				strcasecmp(token, "control") == 0) {
			m |= WLR_MODIFIER_CTRL;
		} else if (key_sym(token, sym)) {
			have_key = true;
		} else {
			snprintf(err, errlen, "trigger '%s': unknown token '%s'",
				trigger, token);
			return false;
		}
	}
	if (!have_key) {
		snprintf(err, errlen, "trigger '%s' has no key (e.g. Mod+1)", trigger);
		return false;
	}
	if (m == 0) {
		snprintf(err, errlen, "trigger '%s' has no modifier "
			"(refusing to grab a bare key)", trigger);
		return false;
	}
	*mods = m;
	return true;
}

bool vidow_keybind_init(struct vidow_server *server) {
	wl_list_init(&server->keybinds);
	return true;
}

struct vidow_keybind *vidow_keybind_add(struct vidow_server *server,
		const char *trigger, const char *action, char *err, size_t errlen) {
	if (err != NULL && errlen > 0) {
		err[0] = '\0';
	}
	if (trigger == NULL || action == NULL || trigger[0] == '\0' ||
			action[0] == '\0') {
		if (err != NULL) {
			snprintf(err, errlen, "bind needs both a key and an action");
		}
		return NULL;
	}

	uint32_t mods = 0;
	xkb_keysym_t sym = XKB_KEY_NoSymbol;
	if (!parse_trigger(trigger, &mods, &sym, err, errlen)) {
		return NULL;
	}

	struct vidow_keybind *bind = calloc(1, sizeof(*bind));
	if (bind == NULL) {
		if (err != NULL) {
			snprintf(err, errlen, "out of memory");
		}
		return NULL;
	}
	snprintf(bind->trigger, sizeof(bind->trigger), "%s", trigger);
	snprintf(bind->action, sizeof(bind->action), "%s", action);
	bind->mods = mods;
	bind->keysym = sym;
	wl_list_insert(server->keybinds.prev, &bind->link);
	wlr_log(WLR_INFO, "keybind: %s -> %s", bind->trigger, bind->action);
	return bind;
}

void vidow_keybind_clear(struct vidow_server *server) {
	struct vidow_keybind *bind, *tmp;
	wl_list_for_each_safe(bind, tmp, &server->keybinds, link) {
		wl_list_remove(&bind->link);
		free(bind);
	}
}

bool vidow_keybind_key(struct vidow_server *server, xkb_keysym_t sym,
		uint32_t mods) {
	const uint32_t interesting = WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT |
		WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT;
	xkb_keysym_t lower = xkb_keysym_to_lower(sym);

	struct vidow_keybind *bind;
	wl_list_for_each(bind, &server->keybinds, link) {
		if ((mods & interesting) != bind->mods) {
			continue;
		}
		if (sym != bind->keysym && lower != bind->keysym) {
			continue;
		}
		vidow_action_run(server, bind->action);
		return true;
	}
	return false;
}

/* ------------------------------------------------------------- 动作 */

static bool parse_number(const char *arg, int *out) {
	if (arg == NULL || arg[0] == '\0') {
		return false;
	}
	char *end = NULL;
	long value = strtol(arg, &end, 10);
	if (end == arg || *end != '\0' || value < 0 || value > 9999) {
		return false;
	}
	*out = (int)value;
	return true;
}

/* `workspace <序号>` / `workspace <名字>` / `workspace up|down|previous`。
 *
 * 序号 = **这块屏上从上往下第几块**（niri：序号是位置，不是身份），越界夹到最后
 * 一块；名字 = 具名工作区（配置里建的、set-workspace-name 给的）。 */
static bool action_workspace_switch(struct vidow_server *server,
		const char *arg) {
	int n = 0;
	if (arg == NULL || arg[0] == '\0') {
		return false;
	}
	if (strcasecmp(arg, "prev") == 0 || strcasecmp(arg, "up") == 0) {
		return vidow_workspace_focus_step(server, -1);
	}
	if (strcasecmp(arg, "next") == 0 || strcasecmp(arg, "down") == 0) {
		return vidow_workspace_focus_step(server, 1);
	}
	if (strcasecmp(arg, "previous") == 0 ||
			strcasecmp(arg, "last") == 0) {
		return vidow_workspace_focus_previous(server);
	}
	if (parse_number(arg, &n)) {
		return vidow_workspace_focus_index(server, server->focused_output,
			n);
	}
	struct vidow_workspace *ws = vidow_workspace_lookup(server, arg);
	if (ws == NULL) {
		wlr_log(WLR_ERROR, "workspace: 没有叫 %s 的工作区（具名工作区要先在"
			"配置里 vidow.workspace_create() 建）", arg);
		return false;
	}
	return vidow_workspace_activate(server, ws);
}

/* 焦点窗口搬到相邻的那块工作区（`move up` / `move down`）。 */
static bool action_move_step(struct vidow_server *server, int direction) {
	struct vidow_view *view = server->focused;
	if (view == NULL || view->ws == NULL || view->ws->special ||
			view->ws->output == NULL) {
		return false;
	}
	int idx = vidow_workspace_index(view->ws) + direction;
	int n = vidow_workspace_count(server, view->ws->output);
	if (idx < 1 || idx > n) {
		wlr_log(WLR_INFO, "move: 那边没有工作区了");
		return false;
	}
	struct vidow_workspace *target =
		vidow_workspace_at(server, view->ws->output, idx);
	return target != NULL && vidow_workspace_move_view(server, view, target);
}

static void log_action(const char *action, const char *what) {
	wlr_log(WLR_INFO, "action: %s -> %s", action, what);
}

/* 真正去切：这一步是同步的，而且一进去 wlroots 就会把**所有输出销毁**（DRM 后端
 * 在会话不活动时 wlr_output_destroy 每一块屏）、输入设备挂起。所以只能在没有别人
 * 的事件处理栈上做 —— 见下面的 vt_switch_idle。 */
static bool vt_switch_now(struct vidow_server *server, unsigned vt) {
	wlr_log(WLR_INFO, "VT: 切到 %u 号 tty", vt);
	if (!wlr_session_change_vt(server->session, vt)) {
		wlr_log(WLR_ERROR, "VT: 切到 %u 号 tty 失败", vt);
		return false;
	}
	return true;
}

/* idle 回调：这一轮事件派发（很可能就是键盘按键那一次）走完了，才动会话。 */
static void vt_switch_idle(void *data) {
	struct vidow_server *server = data;
	unsigned vt = server->vt_pending;
	server->vt_pending = 0;
	if (server->vt_idle != NULL) {
		wl_event_source_remove(server->vt_idle);
		server->vt_idle = NULL;
	}
	if (vt == 0) {
		return;
	}
	vt_switch_now(server, vt);
}

/* 切 tty。
 *
 * 延后到 idle 里做，理由：调用点在键盘事件处理里（Ctrl+Alt+F1..F12），而
 * wlr_session_change_vt() 一进去就会让 wlroots 把所有输出销毁、把输入设备挂起 ——
 * 在「正拿着键盘对象派发按键」的时候拆掉半个会话，是「切走再切回来卡死」最容易踩
 * 的那一脚。延后一轮就没有这个交叠了。
 *
 * 按键重复（Ctrl+Alt+F1 还按着）会被合并：排队里只留最后那一个目标 tty。
 * （libseat 没有「现在在哪块 tty」的接口，所以不做「已经在目标 tty 上」的去重。） */
bool vidow_vt_switch(struct vidow_server *server, unsigned vt) {
	if (server->session == NULL) {
		wlr_log(WLR_DEBUG, "VT: 没有会话（嵌套 / headless），切 VT 交给宿主");
		return false;
	}
	if (vt < 1 || vt > 12) {
		wlr_log(WLR_ERROR, "VT: %u 不是有效的 tty 号（1..12）", vt);
		return false;
	}
	server->vt_pending = vt;
	if (server->vt_idle == NULL) {
		server->vt_idle = wl_event_loop_add_idle(
			wl_display_get_event_loop(server->display), vt_switch_idle, server);
		if (server->vt_idle == NULL) {
			/* 建不出 idle 源：直接切（宁可同步，也别按了没反应） */
			server->vt_pending = 0;
			return vt_switch_now(server, vt);
		}
	}
	wlr_log(WLR_INFO, "VT: %u 号 tty 排队（这一轮事件处理完就切）", vt);
	return true;
}

bool vidow_action_run(struct vidow_server *server, const char *action) {
	if (server == NULL || action == NULL || action[0] == '\0') {
		return false;
	}
	char buf[sizeof(((struct vidow_keybind *)0)->action)];
	snprintf(buf, sizeof(buf), "%s", action);

	char *arg = NULL;
	char *name = buf;
	char *space = strchr(buf, ' ');
	if (space != NULL) {
		*space = '\0';
		arg = space + 1;
		while (*arg == ' ') {
			arg++;
		}
	}

	if (strcmp(name, "workspace") == 0 || strcmp(name, "ws") == 0) {
		return action_workspace_switch(server, arg);
	}
	if (strcmp(name, "move") == 0) {
		if (arg == NULL || arg[0] == '\0') {
			wlr_log(WLR_ERROR, "move 要带参数（move 2 / move chat / move up）");
			return false;
		}
		if (strcasecmp(arg, "up") == 0) {
			return action_move_step(server, -1);
		}
		if (strcasecmp(arg, "down") == 0) {
			return action_move_step(server, 1);
		}
		return vidow_workspace_move_focused(server, arg);
	}
	if (strcmp(name, "move-workspace") == 0 ||
			strcmp(name, "movews") == 0) {
		/* 整块工作区在这块屏上上/下移一位（niri 的 move-workspace-up/down：
		 * 「往中间塞一块新工作区」就是先搬到栈底那块空的、再往上挪）。 */
		int dir = 1;
		int n = 0;
		if (arg != NULL && strcasecmp(arg, "up") == 0) {
			dir = -1;
		} else if (arg != NULL && strcasecmp(arg, "down") != 0 &&
				parse_number(arg, &n) && n != 0) {
			dir = n;
		}
		struct vidow_workspace *active = vidow_workspace_active(server);
		if (active == NULL) {
			return false;
		}
		return vidow_workspace_move_step(server, active, dir);
	}
	if (strcmp(name, "workspace-name") == 0 ||
			strcmp(name, "rename-workspace") == 0) {
		struct vidow_workspace *active = vidow_workspace_active(server);
		if (active == NULL) {
			return false;
		}
		const char *want = arg != NULL && arg[0] != '\0' ? arg : NULL;
		return vidow_workspace_set_name(server, active, want);
	}
	if (strcmp(name, "special") == 0 || strcmp(name, "scratch") == 0) {
		const char *want = arg != NULL && arg[0] != '\0' ? arg : "special";
		return vidow_workspace_toggle_special(server, want);
	}
	if (strcmp(name, "focus") == 0) {
		int n = 0;
		if (parse_number(arg, &n)) {
			vidow_focus_column(server, n);
			return true;
		}
		wlr_log(WLR_ERROR, "focus 的参数是列方向（focus -1 / focus 1）");
		return false;
	}
	if (strcmp(name, "mousemove") == 0) {
		/* 光标走到哪儿，哪块屏就是「聚焦的屏」—— 特殊工作区跟随激活的屏就靠它。 */
		struct wlr_output *output = wlr_output_layout_output_at(
			server->output_layout, server->cursor->x, server->cursor->y);
		if (output == NULL) {
			return false;
		}
		if (server->focused_output != output) {
			server->focused_output = output;
			log_action(action, output->name);
		}
		return true;
	}
	if (strcmp(name, "none") == 0) {
		return true; /* 空绑定，用来屏蔽某个键 */
	}
	/* 切 tty：
	 *
	 *   vidow.bind("Ctrl+Alt+F2", "switch-vt 2")
	 *   vidowmsg action switch-vt 2
	 *
	 * 内核一般自己就把 Ctrl+Alt+Fn 处理掉了（合成器不 EVIOCGRAB，控制台键盘那
	 * 一侧也收得到），所以真机上这条通常是空转 —— 但「切不走 tty」是收到过的
	 * 报告，而 X 服务器就是自己实现的（它抓了设备，内核收不到这个键）。两种
	 * 情况下都能用，最省事。没有会话（嵌套 / headless）时不动：那是宿主的事。 */
	if (strcmp(name, "switch-vt") == 0) {
		int vt = arg != NULL ? atoi(arg) : 0;
		if (vt < 1 || vt > 12) {
			wlr_log(WLR_ERROR, "switch-vt 要 1..12 号 tty");
			return false;
		}
		return vidow_vt_switch(server, (unsigned)vt);
	}
	/* 测试用：注入口/指针事件。走的是和真实鼠标完全同一条路
	 * （vidow_pointer_button / wlr_cursor_warp），不另开捷径 —— 拖拽这种
	 * 「按着不放 + 一路移动 + 松手」的行为只有这样才测得出来。
	 *
	 *   pointer-motion <x> <y>
	 *   pointer-button <键> press|release [mod]
	 *
	 * `mod` = 假装按着 Mod（拖拽要求按着 Mod，而 headless 测试里没有真键盘）。 */
	if (strcmp(name, "pointer-motion") == 0) {
		int x = 0, y = 0;
		if (arg == NULL || sscanf(arg, "%d %d", &x, &y) != 2) {
			wlr_log(WLR_ERROR, "pointer-motion 要 x y");
			return false;
		}
		wlr_cursor_warp(server->cursor, NULL, (double)x, (double)y);
		/* wlr_cursor_warp 不一定发 motion 事件（它只是挪位置），所以显式走一遍
		 * 处理：拖拽靠的就是这条路上的 grab 分支。 */
		vidow_pointer_motion(server,
			(uint32_t)(vidow_now_ns() / 1000000LL));
		return true;
	}
	if (strcmp(name, "pointer-button") == 0) {
		char key[16] = "";
		char state[16] = "";
		char extra[16] = "";
		if (arg == NULL ||
				sscanf(arg, "%15s %15s %15s", key, state, extra) < 2) {
			wlr_log(WLR_ERROR, "pointer-button 要 <left|right|middle|数字> "
				"press|release [mod]");
			return false;
		}
		/* 认名字也认数字（BTN_LEFT = 272 这种数字写在测试里没人记得住） */
		uint32_t btn;
		if (strcmp(key, "left") == 0) {
			btn = BTN_LEFT;
		} else if (strcmp(key, "right") == 0) {
			btn = BTN_RIGHT;
		} else if (strcmp(key, "middle") == 0) {
			btn = BTN_MIDDLE;
		} else {
			btn = (uint32_t)atoi(key);
		}
		bool pressed = strcmp(state, "press") == 0;
		vidow_pointer_button(server, btn, pressed,
			(uint32_t)(vidow_now_ns() / 1000000LL),
			strcmp(extra, "mod") == 0);
		return true;
	}

	wlr_log(WLR_ERROR, "action: 不认识的动作 '%s'", action);
	return false;
}
