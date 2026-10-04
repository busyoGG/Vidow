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

static struct vidow_workspace *adjacent_workspace(struct vidow_server *server,
		int direction) {
	/* 只在同一块屏的工作区之间转（工作区归屏管，跨屏是另一回事）。 */
	struct vidow_workspace *active = vidow_workspace_active(server);
	if (active == NULL) {
		return NULL;
	}
	struct wl_list *node = direction > 0 ? active->link.next :
		active->link.prev;
	while (node != &server->workspaces) {
		struct vidow_workspace *ws = wl_container_of(node, ws, link);
		if (!ws->special && ws->output == active->output) {
			return ws;
		}
		node = direction > 0 ? node->next : node->prev;
	}
	return NULL;
}

static bool action_workspace_switch(struct vidow_server *server,
		const char *arg) {
	int n = 0;
	struct vidow_workspace *ws = NULL;
	if (arg == NULL || arg[0] == '\0') {
		return false;
	}
	if (strcasecmp(arg, "prev") == 0 || strcasecmp(arg, "left") == 0) {
		ws = adjacent_workspace(server, -1);
	} else if (strcasecmp(arg, "next") == 0 ||
			strcasecmp(arg, "right") == 0) {
		ws = adjacent_workspace(server, 1);
	} else if (parse_number(arg, &n)) {
		char name[16];
		snprintf(name, sizeof(name), "%d", n);
		ws = vidow_workspace_create(server, name, server->focused_output);
	} else {
		ws = vidow_workspace_lookup(server, arg);
		if (ws == NULL) {
			wlr_log(WLR_ERROR, "workspace: 没有叫 %s 的工作区", arg);
			return false;
		}
	}
	if (ws == NULL) {
		wlr_log(WLR_INFO, "workspace: 那边没有工作区了（%s）",
			arg != NULL ? arg : "");
		return false;
	}
	return vidow_workspace_activate(server, ws);
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
			wlr_log(WLR_ERROR, "move 要带参数（move 1 / move scratch）");
			return false;
		}
		return vidow_workspace_move_focused(server, arg);
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
