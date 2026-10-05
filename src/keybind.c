#include "keybind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <xkbcommon/xkbcommon.h>

#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "server.h"
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

	wlr_log(WLR_ERROR, "action: 不认识的动作 '%s'", action);
	return false;
}
