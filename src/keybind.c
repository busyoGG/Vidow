#include "keybind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <xkbcommon/xkbcommon.h>

#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "ipc.h"
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
	/* F1..F35（切 tty 的默认绑定就是 Ctrl+Alt+F1..F12）。表里不一个个列，
	 * 这里算出来。 */
	if (name[1] != '\0' && (name[0] == 'f' || name[0] == 'F')) {
		char *end = NULL;
		long n = strtol(name + 1, &end, 10);
		if (end != name + 1 && *end == '\0' && n >= 1 && n <= 35) {
			*out = (xkb_keysym_t)XKB_KEY_F1 + (xkb_keysym_t)(n - 1);
			return true;
		}
	}
	/* XF86Switch_VT_1..12：自定义 keymap 里可能直接给这个 keysym
	 * （默认 keymap 给的是 F1..F12 + ctrl|alt，见 README 的「切 tty」）。 */
	if (strncasecmp(name, "xf86switch_vt_", 14) == 0) {
		char *end = NULL;
		long n = strtol(name + 14, &end, 10);
		if (end != name + 14 && *end == '\0' && n >= 1 && n <= 12) {
			*out = (xkb_keysym_t)XKB_KEY_XF86Switch_VT_1 +
				(xkb_keysym_t)(n - 1);
			return true;
		}
	}
	return name[1] == '\0' && ascii_key(name[0], out);
}

/* 触发解析的结果：一类触发源 + 修饰键掩码 + 那类源自己的参数。 */
struct trigger_parsed {
	uint32_t mods;
	enum vidow_bind_src src;
	xkb_keysym_t sym;           /* VIDOW_SRC_KEY：主键（单独修饰键时是 NoSymbol） */
	uint32_t button;            /* VIDOW_SRC_MOUSE */
	enum wl_pointer_axis axis;  /* VIDOW_SRC_WHEEL */
	int axis_dir;
	bool modifier_only;
};

/* 修饰键 keysym -> WLR_MODIFIER_* 的位。`vidow.bind("Super_L", …)` 只认左边那个。 */
static uint32_t modifier_bit(xkb_keysym_t sym) {
	switch (sym) {
	case XKB_KEY_Super_L:
	case XKB_KEY_Super_R:
		return WLR_MODIFIER_LOGO;
	case XKB_KEY_Alt_L:
	case XKB_KEY_Alt_R:
		return WLR_MODIFIER_ALT;
	case XKB_KEY_Control_L:
	case XKB_KEY_Control_R:
		return WLR_MODIFIER_CTRL;
	case XKB_KEY_Shift_L:
	case XKB_KEY_Shift_R:
		return WLR_MODIFIER_SHIFT;
	default:
		return 0;
	}
}

/* 鼠标按键的名字：`mouse-left` / `mouse-middle` / `mouse-back` / `mouse-forward`，
 * 也认 `BTN_LEFT` 这种原名和 `mouse-274` 这种裸编号。
 *
 * 为什么加 `mouse-` 前缀：`left` / `right` 在键名表里已经是**方向键**了，直接拿
 * 来当鼠标左键会把方向键的绑定悄悄改掉。 */
static bool mouse_button_name(const char *token, uint32_t *out) {
	const char *name = NULL;
	if (strncasecmp(token, "mouse-", 6) == 0) {
		name = token + 6;
	} else if (strncasecmp(token, "btn_", 4) == 0) {
		name = token;
	}
	if (name == NULL) {
		return false;
	}
	uint32_t btn = vidow_pointer_button_from_name(name);
	if (btn == 0) {
		return false;
	}
	*out = btn;
	return true;
}

/* 滚轮方向：wheel-up / wheel-down / wheel-left / wheel-right（一个方向一条绑定）。 */
static bool wheel_name(const char *token, enum wl_pointer_axis *axis, int *dir) {
	if (strcasecmp(token, "wheel-up") == 0) {
		*axis = WL_POINTER_AXIS_VERTICAL_SCROLL;
		*dir = -1;
		return true;
	}
	if (strcasecmp(token, "wheel-down") == 0) {
		*axis = WL_POINTER_AXIS_VERTICAL_SCROLL;
		*dir = 1;
		return true;
	}
	if (strcasecmp(token, "wheel-left") == 0) {
		*axis = WL_POINTER_AXIS_HORIZONTAL_SCROLL;
		*dir = -1;
		return true;
	}
	if (strcasecmp(token, "wheel-right") == 0) {
		*axis = WL_POINTER_AXIS_HORIZONTAL_SCROLL;
		*dir = 1;
		return true;
	}
	return false;
}

/* "Mod+Shift+Q" -> 修饰键掩码 + 触发源。大小写不敏感，修饰键名字认几个别名。
 *
 * 三类都能写：
 *   Mod+Q                     键盘键
 *   Mod+mouse-left            鼠标按键（BTN_LEFT）
 *   Mod+wheel-up              滚轮（纵向，往上）
 *   Mod                       单独一个修饰键（没有主键） */
static bool parse_trigger(const char *trigger, struct trigger_parsed *out,
		char *err, size_t errlen) {
	char buf[sizeof(((struct vidow_keybind *)0)->trigger)];
	snprintf(buf, sizeof(buf), "%s", trigger);

	*out = (struct trigger_parsed){ .src = VIDOW_SRC_KEY };
	uint32_t m = 0;
	bool have_key = false;
	bool have_pointer = false;
	const char *token = NULL;
	char *save = NULL;
	for (token = strtok_r(buf, "+", &save); token != NULL;
			token = strtok_r(NULL, "+", &save)) {
		uint32_t btn = 0;
		enum wl_pointer_axis axis;
		int dir = 0;
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
		} else if (wheel_name(token, &axis, &dir)) {
			if (have_pointer || have_key) {
				snprintf(err, errlen, "trigger '%s': 滚轮不能再和别的键"
					"写在一起", trigger);
				return false;
			}
			out->src = VIDOW_SRC_WHEEL;
			out->axis = axis;
			out->axis_dir = dir;
			have_pointer = true;
		} else if (mouse_button_name(token, &btn)) {
			if (have_pointer || have_key) {
				snprintf(err, errlen, "trigger '%s': 鼠标按键不能再和别的键"
					"写在一起", trigger);
				return false;
			}
			out->src = VIDOW_SRC_MOUSE;
			out->button = btn;
			have_pointer = true;
		} else if (key_sym(token, &out->sym)) {
			have_key = true;
		} else {
			snprintf(err, errlen, "trigger '%s': unknown token '%s'",
				trigger, token);
			return false;
		}
	}
	out->mods = m;

	if (have_pointer) {
		/* 鼠标按键 / 滚轮允许不带修饰键（中键直接绑个动作是常见需求）。 */
		return true;
	}
	if (have_key) {
		if (m == 0) {
			snprintf(err, errlen, "trigger '%s' has no modifier "
				"(refusing to grab a bare key)", trigger);
			return false;
		}
		return true;
	}
	/* 只剩修饰键：单独一个修饰键的绑定（`vidow.bind("Mod", …)`）。 */
	if (m == 0) {
		snprintf(err, errlen, "trigger '%s' has no key (e.g. Mod+1)", trigger);
		return false;
	}
	out->modifier_only = true;
	/* `Super_L` / `Super_R` 这种写法的具体某一侧留在 sym 里（NoSymbol = 两侧都认）。 */
	if (modifier_bit(out->sym) == 0) {
		out->sym = XKB_KEY_NoSymbol;
	}
	return true;
}

bool vidow_keybind_init(struct vidow_server *server) {
	wl_list_init(&server->keybinds);
	return true;
}

struct vidow_keybind *vidow_keybind_add(struct vidow_server *server,
		const char *trigger, const char *action, bool inhibited,
		bool on_release, char *err, size_t errlen) {
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

	struct trigger_parsed t;
	if (!parse_trigger(trigger, &t, err, errlen)) {
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
	bind->mods = t.mods;
	bind->keysym = t.sym;
	bind->inhibited = inhibited;
	bind->src = t.src;
	bind->button = t.button;
	bind->axis = t.axis;
	bind->axis_dir = t.axis_dir;
	bind->modifier_only = t.modifier_only;
	/* 单独一个修饰键默认**松开才触发**：按下就触发的话 `Super+Q` 会顺带把
	 * 全景也打开（那条绑定本来只想在「只按了 Super」时生效）。 */
	bind->on = on_release || t.modifier_only ? VIDOW_BIND_RELEASE
						 : VIDOW_BIND_PRESS;
	wl_list_insert(server->keybinds.prev, &bind->link);
	wlr_log(WLR_INFO, "keybind: %s -> %s%s%s%s", bind->trigger, bind->action,
		bind->src == VIDOW_SRC_MOUSE ? "（鼠标按键）" :
			bind->src == VIDOW_SRC_WHEEL ? "（滚轮）" : "",
		bind->on == VIDOW_BIND_RELEASE ? "（松开触发）" : "",
		inhibited ? "（抑制期间也生效）" : "");
	return bind;
}

void vidow_keybind_clear(struct vidow_server *server) {
	struct vidow_keybind *bind, *tmp;
	wl_list_for_each_safe(bind, tmp, &server->keybinds, link) {
		wl_list_remove(&bind->link);
		free(bind);
	}
}

/* 按下还没松开的那些键。「松开才触发」的绑定要记住按下那一下，「单独一个
 * 修饰键」还要记住武装期间有没有别的键插进来。 */
struct pending_press {
	bool used;
	enum vidow_bind_src src;
	xkb_keysym_t sym;
	uint32_t button;
	uint32_t mods;        /* 按下那一刻的修饰键掩码（松开时按它匹配） */
	bool inhibited;       /* 按下那一刻的抑制状态 */
	bool swallow;         /* 按下那一下已经吞了（松开也跟着吞） */
	bool tap;             /* 单独一个修饰键：武装中 */
	uint32_t tap_mods;    /* 武装时按着的那组修饰键 */
};
#define PENDING_MAX 8
static struct pending_press pendings[PENDING_MAX];

#define INTERESTING_MODS (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT | \
	WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT)

static struct pending_press *pending_new(void) {
	for (size_t i = 0; i < PENDING_MAX; i++) {
		if (!pendings[i].used) {
			pendings[i] = (struct pending_press){ .used = true };
			return &pendings[i];
		}
	}
	return NULL;
}

static struct pending_press *pending_find(enum vidow_bind_src src,
		xkb_keysym_t sym, uint32_t button) {
	for (size_t i = 0; i < PENDING_MAX; i++) {
		struct pending_press *p = &pendings[i];
		if (!p->used || p->src != src) {
			continue;
		}
		if (src == VIDOW_SRC_KEY ? p->sym == sym : p->button == button) {
			return p;
		}
	}
	return NULL;
}

/* 按了别的键 / 点了鼠标：单独一个修饰键那种绑定就作废（Super+Q 不该顺带把
 * 「只按了 Super」那条也跑掉）。 */
static void pending_disarm_taps(void) {
	for (size_t i = 0; i < PENDING_MAX; i++) {
		if (pendings[i].used && pendings[i].tap) {
			pendings[i].used = false;
		}
	}
}

static bool mods_match(uint32_t mods, uint32_t want) {
	return (mods & INTERESTING_MODS) == want;
}

/* 这条绑定和这次事件对得上吗（只看「触发源 + 修饰键 + 键」，不看时机）。 */
static bool bind_hits(const struct vidow_keybind *bind, enum vidow_bind_src src,
		xkb_keysym_t sym, uint32_t button, uint32_t mods) {
	if (bind->src != src || !mods_match(mods, bind->mods)) {
		return false;
	}
	if (src == VIDOW_SRC_MOUSE) {
		return bind->button == button;
	}
	if (src != VIDOW_SRC_KEY) {
		return false;
	}
	return sym == bind->keysym || xkb_keysym_to_lower(sym) == bind->keysym;
}

static void bind_run(struct vidow_server *server, struct vidow_keybind *bind) {
	vidow_action_run(server, bind->action);
	/* 事件（见 hooks.c）：谁按了什么键、跑了哪条动作 —— 脚本要「跟着按键
	 * 做点别的」（录屏时打标记、状态栏闪一下）不用自己抢键。 */
	const struct vidow_event_field fields[] = {
		VIDOW_EV_STR("trigger", bind->trigger),
		VIDOW_EV_STR("action", bind->action),
		VIDOW_EV_END,
	};
	vidow_ipc_emit_simple(server, "keybind", fields);
}

/* 按下 / 松开时在表里找一条：src 对应、时机对得上、修饰键和键都对得上。
 * inhibited = 现在有生效中的快捷键抑制（只认标记了 inhibited 的那几条）。 */
static struct vidow_keybind *bind_lookup(struct vidow_server *server,
		enum vidow_bind_src src, enum vidow_bind_on on, xkb_keysym_t sym,
		uint32_t button, uint32_t mods, bool inhibited) {
	struct vidow_keybind *bind;
	wl_list_for_each(bind, &server->keybinds, link) {
		if (bind->on != on || bind->modifier_only) {
			continue;
		}
		if (inhibited && !bind->inhibited) {
			continue;
		}
		if (bind_hits(bind, src, sym, button, mods)) {
			return bind;
		}
	}
	return NULL;
}

/* 「单独一个修饰键」：找一条 modifier_only 的绑定，修饰键对得上，而且（写了
 * `Super_L` 那种具体一侧的话）就是这一侧。 */
static struct vidow_keybind *tap_lookup(struct vidow_server *server,
		uint32_t tap_mods, xkb_keysym_t sym, bool inhibited) {
	struct vidow_keybind *bind;
	wl_list_for_each(bind, &server->keybinds, link) {
		if (!bind->modifier_only || bind->mods != tap_mods) {
			continue;
		}
		if (inhibited && !bind->inhibited) {
			continue;
		}
		if (bind->keysym == XKB_KEY_NoSymbol || bind->keysym == sym) {
			return bind;
		}
	}
	return NULL;
}

/* 按下时问一句：这条键（鼠标按键）有没有「松开才触发」的绑定？有就记下来，
 * 顺便把按下那一下也吞掉 —— 只吞松开的话客户端会以为这个键一直按着。 */
static bool pending_record(struct vidow_server *server, enum vidow_bind_src src,
		xkb_keysym_t sym, uint32_t button, uint32_t mods, bool inhibited) {
	struct vidow_keybind *bind = bind_lookup(server, src, VIDOW_BIND_RELEASE,
		sym, button, mods, inhibited);
	if (bind == NULL) {
		return false;
	}
	struct pending_press *p = pending_new();
	if (p == NULL) {
		/* 表满了：这一下不记（松开时自然不会触发）。 */
		return false;
	}
	p->src = src;
	p->sym = sym;
	p->button = button;
	p->mods = mods;
	p->inhibited = inhibited;
	p->swallow = true;
	return true;
}

/* 松开：把按下时记下来的那条跑掉。返回是否吞掉这次松开。 */
static bool pending_release(struct vidow_server *server,
		enum vidow_bind_src src, xkb_keysym_t sym, uint32_t button,
		uint32_t mods) {
	struct pending_press *p = pending_find(src, sym, button);
	if (p == NULL) {
		return false;
	}
	bool swallow = p->swallow;
	bool tap = p->tap;
	uint32_t pmods = p->mods;
	uint32_t tap_mods = p->tap_mods;
	bool pinhibited = p->inhibited;
	p->used = false;

	if (tap) {
		/* 还按着别的修饰键就不算「只按了一下」：等最后一个松开再说。 */
		if ((mods & INTERESTING_MODS) != 0) {
			return swallow;
		}
		struct vidow_keybind *bind =
			tap_lookup(server, tap_mods, sym, pinhibited);
		if (bind != NULL) {
			bind_run(server, bind);
			return true;
		}
		return swallow;
	}
	struct vidow_keybind *bind = bind_lookup(server, src, VIDOW_BIND_RELEASE,
		sym, button, pmods, pinhibited);
	if (bind != NULL) {
		bind_run(server, bind);
		return true;
	}
	return swallow;
}

bool vidow_keybind_key(struct vidow_server *server, xkb_keysym_t sym,
		bool pressed, uint32_t mods, bool inhibited) {
	if (!pressed) {
		return pending_release(server, VIDOW_SRC_KEY, sym, 0, mods);
	}

	uint32_t bit = modifier_bit(sym);
	if (bit == 0) {
		/* 普通键：单独一个修饰键那种绑定作废（这是「组合键」了）。 */
		pending_disarm_taps();
	} else {
		/* 修饰键按下：有「单独一个修饰键」的绑定就武装起来。
		 * tap_mods 用「当前按着的 + 刚按下的这个」（wlroots 是先更新 xkb 状态
		 * 再发按键事件，所以 mods 一般已经含这一位；补上一位两种情况都对）。 */
		uint32_t held = (mods & INTERESTING_MODS) | bit;
		if (tap_lookup(server, held, sym, inhibited) != NULL &&
				pending_find(VIDOW_SRC_KEY, sym, 0) == NULL) {
			struct pending_press *p = pending_new();
			if (p != NULL) {
				p->src = VIDOW_SRC_KEY;
				p->sym = sym;
				p->mods = mods;
				p->inhibited = inhibited;
				p->swallow = true;
				p->tap = true;
				p->tap_mods = held;
			}
			return true;
		}
	}

	struct vidow_keybind *bind = bind_lookup(server, VIDOW_SRC_KEY,
		VIDOW_BIND_PRESS, sym, 0, mods, inhibited);
	if (bind != NULL) {
		bind_run(server, bind);
		return true;
	}
	if (pending_record(server, VIDOW_SRC_KEY, sym, 0, mods, inhibited)) {
		return true;
	}
	return false;
}

bool vidow_keybind_button(struct vidow_server *server, uint32_t button,
		bool pressed, uint32_t mods, bool inhibited) {
	if (pressed) {
		/* 点了鼠标就不算「只按了一下修饰键」。 */
		pending_disarm_taps();
	} else if (pending_release(server, VIDOW_SRC_MOUSE, XKB_KEY_NoSymbol,
			button, mods)) {
		return true;
	}

	struct vidow_keybind *bind = bind_lookup(server, VIDOW_SRC_MOUSE,
		VIDOW_BIND_PRESS, XKB_KEY_NoSymbol, button, mods, inhibited);
	if (bind != NULL) {
		bind_run(server, bind);
		return true;
	}
	if (pending_record(server, VIDOW_SRC_MOUSE, XKB_KEY_NoSymbol, button,
			mods, inhibited)) {
		return true;
	}
	return false;
}

bool vidow_keybind_wheel(struct vidow_server *server,
		enum wl_pointer_axis axis, double amount, uint32_t mods,
		bool inhibited) {
	if (amount == 0) {
		return false;
	}
	int dir = amount > 0 ? 1 : -1;
	struct vidow_keybind *bind;
	wl_list_for_each(bind, &server->keybinds, link) {
		if (bind->src != VIDOW_SRC_WHEEL || bind->axis != axis ||
				bind->axis_dir != dir) {
			continue;
		}
		if (inhibited && !bind->inhibited) {
			continue;
		}
		if (!mods_match(mods, bind->mods)) {
			continue;
		}
		bind_run(server, bind);
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

/* 方向参数：-1 / 1，也认 left|right|up|down|prev|next（列、窗口都用它）。
 * 单独写一个是因为 parse_number() 不收负数 —— `focus -1` 就是这么废掉的。 */
static bool parse_direction(const char *arg, int *out) {
	if (arg == NULL || arg[0] == '\0') {
		return false;
	}
	if (strcasecmp(arg, "left") == 0 || strcasecmp(arg, "up") == 0 ||
			strcasecmp(arg, "prev") == 0 ||
			strcasecmp(arg, "previous") == 0) {
		*out = -1;
		return true;
	}
	if (strcasecmp(arg, "right") == 0 || strcasecmp(arg, "down") == 0 ||
			strcasecmp(arg, "next") == 0) {
		*out = 1;
		return true;
	}
	char *end = NULL;
	long value = strtol(arg, &end, 10);
	if (end == arg || *end != '\0' || value == 0 ||
			value < -1000 || value > 1000) {
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
static bool action_move_step(struct vidow_server *server, int direction,
		bool follow) {
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
	if (target == NULL || !vidow_workspace_move_view(server, view, target)) {
		return false;
	}
	if (follow) {
		/* 「跟着走」：搬完连人一起过去。先切到那块工作区（屏也跟着走），再把焦点
		 * 放回搬过来的这个窗口 —— activate 只会挑它自己记着的那个，可能是别的。 */
		vidow_workspace_activate(server, target);
		vidow_focus_view(view);
	}
	return true;
}

/* 光标底下那个窗口（指针手势动作用的：按下那一刻谁在光标下面，就拖谁）。 */
static struct vidow_view *view_under_cursor(struct vidow_server *server) {
	double sx, sy;
	struct wlr_surface *surface = NULL;
	return vidow_view_at(server, server->cursor->x, server->cursor->y, &surface,
		&sx, &sy);
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
			wlr_log(WLR_ERROR, "move 要带参数（move 2 / move chat / move up / "
				"move up follow）");
			return false;
		}
		/* `move up follow`：搬完连人一起过去（默认的 `Alt+Shift+滚轮` 就是这条）。
		 * 第一个词是方向，后面那个词只认 follow。 */
		char dir[64];
		size_t i = 0;
		while (arg[i] != '\0' && arg[i] != ' ' && i + 1 < sizeof(dir)) {
			dir[i] = arg[i];
			i++;
		}
		dir[i] = '\0';
		const char *extra = arg + i;
		while (*extra == ' ') {
			extra++;
		}
		bool follow = false;
		if (extra[0] != '\0') {
			if (strcasecmp(extra, "follow") != 0) {
				wlr_log(WLR_ERROR, "move: 只认 `move <方向> follow`，多出来的是 "
					"'%s'", extra);
				return false;
			}
			follow = true;
		}
		if (strcasecmp(dir, "up") == 0) {
			return action_move_step(server, -1, follow);
		}
		if (strcasecmp(dir, "down") == 0) {
			return action_move_step(server, 1, follow);
		}
		if (follow) {
			wlr_log(WLR_ERROR, "move: follow 只跟 up / down 搭（`move up follow`）");
			return false;
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
		int dir = 0;
		if (parse_direction(arg, &dir)) {
			vidow_focus_column(server, dir);
			/* 键盘换焦点之后把光标带过去（warp-mouse-to-focus，默认关）。 */
			vidow_warp_cursor_to_focus(server);
			return true;
		}
		wlr_log(WLR_ERROR, "focus 的参数是列方向（focus -1 / focus 1 / "
			"focus left / focus right）");
		return false;
	}
	/* 窗口 / 布局：原来这些是 input.c 里的内置键位表，现在全走动作 ——
	 * 默认配置里绑好了一套（configs/14-keys.lua），删了就没了。 */
	if (strcmp(name, "focus-column") == 0 ||
			strcmp(name, "move-column") == 0 ||
			strcmp(name, "focus-window") == 0 ||
			strcmp(name, "move-window") == 0) {
		int dir = 0;
		if (!parse_direction(arg, &dir)) {
			wlr_log(WLR_ERROR, "%s 要一个方向（-1 / 1，或者 "
				"left|right|up|down）", name);
			return false;
		}
		if (strcmp(name, "focus-column") == 0) {
			vidow_focus_column(server, dir);
		} else if (strcmp(name, "move-column") == 0) {
			vidow_move_column(server, dir);
		} else if (strcmp(name, "focus-window") == 0) {
			vidow_focus_window(server, dir);
		} else {
			vidow_move_window(server, dir);
		}
		/* move-* 不换焦点，所以这一下在它们身上是空转；focus-* 才真的挪光标
		 * （warp-mouse-to-focus，默认关）。 */
		vidow_warp_cursor_to_focus(server);
		log_action(action, "焦点 / 窗口挪一格");
		return true;
	}
	/* 从列里取出来（niri 的 expel-window-from-column）：默认单独成一条列摆在
	 * 原来那条列右边，参数写 left 就摆左边。列里只剩它一个 = 没什么可取的。 */
	if (strcmp(name, "expel-window-from-column") == 0 ||
			strcmp(name, "expel-window") == 0) {
		int dir = 1;
		if (arg != NULL && arg[0] != '\0' &&
				!parse_direction(arg, &dir)) {
			wlr_log(WLR_ERROR, "%s 的方向参数是 left / right"
				"（不写 = right）", name);
			return false;
		}
		if (server->focused == NULL ||
				!vidow_view_expel_from_column(server->focused, dir)) {
			wlr_log(WLR_INFO, "取不出来：焦点窗口是浮窗、或者它那条列里"
				"就它一个（那它本来就是单独一条列）");
			return false;
		}
		log_action(action, "把窗口从列里取出来");
		return true;
	}
	/* 和相邻那列里的窗口换个位置（左右两个窗口对调；列里只有一个窗口时就是
	 * 两条列的内容对调）。方向只认 left / right —— 上下是列内挪窗口
	 * （move-window），不是这个动作。 */
	if (strcmp(name, "swap-window") == 0 ||
			strcmp(name, "swap") == 0) {
		int dir = 0;
		if (!parse_direction(arg, &dir) || dir == 0) {
			wlr_log(WLR_ERROR, "%s 要一个方向（left / right）", name);
			return false;
		}
		if (!vidow_swap_window(server, dir)) {
			wlr_log(WLR_INFO, "换不了：没焦点窗口、是浮窗、"
				"或者那个方向没有别的列");
			return false;
		}
		log_action(action, "和相邻列里的窗口换位置");
		return true;
	}
	/* 鼠标拖拽：原来写死在 input.c 的指针手势，现在是一条动作 + 一条绑定：
	 *     vidow.bind("Mod+mouse-left",  "interactive-move")
	 *     vidow.bind("Mod+mouse-right", "interactive-resize")
	 * 目标是**光标底下那个窗口**（按下那一刻的位置）：浮窗改自己的几何，平铺
	 * 窗口拖动 / 改列宽。拖起来之后的移动和松手由抓取那套接管（input.c 的
	 * grab_mode），这里只负责「开始拖」。 */
	if (strcmp(name, "interactive-move") == 0 ||
			strcmp(name, "interactive-resize") == 0) {
		bool resize = strcmp(name, "interactive-resize") == 0;
		struct vidow_view *view = view_under_cursor(server);
		if (view == NULL || view->popup) {
			return false;
		}
		if (view->float_mode != VIDOW_FLOAT_NONE) {
			vidow_float_begin_grab(server, view, resize, false);
			if (view != server->focused) {
				vidow_focus_view(view);
			}
		} else if (view->column == NULL) {
			return false;
		} else if (resize) {
			vidow_tile_resize_begin(server, view);
		} else {
			vidow_tile_grab_begin(server, view);
		}
		log_action(action, resize ? "开始改大小（跟着光标）" :
			"开始拖窗口（跟着光标）");
		return true;
	}
	if (strcmp(name, "close-window") == 0) {
		if (server->focused == NULL) {
			return false;
		}
		vidow_view_close(server->focused);
		log_action(action, "关掉焦点窗口");
		return true;
	}
	if (strcmp(name, "toggle-floating") == 0) {
		if (server->focused == NULL) {
			return false;
		}
		vidow_float_toggle(server->focused);
		log_action(action, "平铺 ⇄ 浮动");
		return true;
	}
	if (strcmp(name, "toggle-fullscreen") == 0 ||
			strcmp(name, "fullscreen") == 0) {
		if (server->focused == NULL) {
			return false;
		}
		vidow_view_toggle_fullscreen(server->focused);
		log_action(action, "全屏开关");
		return true;
	}
	if (strcmp(name, "cycle-column-width") == 0) {
		vidow_cycle_column_width(server);
		log_action(action, "列宽 1/3 → 1/2 → 2/3");
		return true;
	}
	if (strcmp(name, "maximize-column") == 0 ||
			strcmp(name, "toggle-maximize-column") == 0) {
		/* niri 的 maximize-column：整列撑满工作区宽度，再按一次还原成原来那个
		 * 比例。没有聚焦窗口 / 浮窗（没有列）时什么也不发生。 */
		if (server->focused != NULL && server->focused->column != NULL) {
			struct vidow_column *col = server->focused->column;
			bool want = !col->maximized;
			vidow_column_toggle_maximized(server);
			log_action(action, want ? "列最大化" : "列还原");
		}
		return true;
	}
	if (strcmp(name, "toggle-overview") == 0 ||
			strcmp(name, "overview") == 0) {
		vidow_overview_toggle(server);
		log_action(action, "全景开关");
		return true;
	}
	/* Alt+Tab 切换器（niri 的 recent windows，见 mru.c）。
	 *   vidow.bind("Alt+Tab", "next-window")
	 *   vidow.bind("Alt+Shift+Tab", "previous-window")
	 * 参数（可选，空格分隔，可叠加）：
	 *   same-app       只看当前窗口那个应用（niri 的 Alt+`）
	 *   scope=workspace / scope=output / scope=all   这次只列这个范围 */
	if (strcmp(name, "next-window") == 0 ||
			strcmp(name, "previous-window") == 0) {
		int dir = strcmp(name, "next-window") == 0 ? 1 : -1;
		const char *app_id = NULL;
		enum vidow_mru_scope scope = server->config.mru_scope;
		if (arg != NULL && arg[0] != '\0') {
			char buf[128];
			snprintf(buf, sizeof(buf), "%s", arg);
			char *save = NULL;
			for (char *tok = strtok_r(buf, " ,", &save); tok != NULL;
					tok = strtok_r(NULL, " ,", &save)) {
				if (strcmp(tok, "same-app") == 0 ||
						strcmp(tok, "same_app") == 0 ||
						strcmp(tok, "app") == 0) {
					app_id = server->focused != NULL ?
						vidow_view_app_id(server->focused) : NULL;
				} else if (strcmp(tok, "scope=workspace") == 0) {
					scope = VIDOW_MRU_WORKSPACE;
				} else if (strcmp(tok, "scope=output") == 0) {
					scope = VIDOW_MRU_OUTPUT;
				} else if (strcmp(tok, "scope=all") == 0) {
					scope = VIDOW_MRU_ALL;
				} else {
					wlr_log(WLR_ERROR, "next-window / previous-window: "
						"不认识的参数 '%s'（能写：same-app / scope=all|"
						"workspace|output）", tok);
					return false;
				}
			}
		}
		vidow_mru_open(server, dir, app_id, scope);
		log_action(action, dir > 0 ? "切到下一个窗口" : "切到上一个窗口");
		return true;
	}
	/* 跑一条 shell 命令（默认配置里 Mod+Return 用的就是它）：
	 *   vidow.bind("Mod+Return", "spawn kitty")
	 * 双 fork、不等它、输出丢掉 —— 不阻塞渲染，也不留僵尸。 */
	if (strcmp(name, "spawn") == 0 || strcmp(name, "exec") == 0) {
		if (arg == NULL || arg[0] == '\0') {
			wlr_log(WLR_ERROR, "spawn 后面要跟一条命令");
			return false;
		}
		vidow_exec_detached(arg);
		log_action(action, "起了一条命令");
		return true;
	}
	if (strcmp(name, "quit") == 0 || strcmp(name, "exit") == 0) {
		wlr_log(WLR_INFO, "action: %s -> 退出合成器", action);
		wl_display_terminate(server->display);
		return true;
	}
	/* 最小化（可绑：vidow.bind("Mod+M", "minimize-window")）：
	 *   minimize-window   —— 收起当前窗口（留在原列里，只是不占位）
	 *   restore-window    —— 放回最近收起来的那个（同 unminimize-window）
	 *   toggle-minimize   —— 当前窗口收了就放回、没收就收起
	 * 按 id 指认某个窗口用 IPC：vidowmsg minimize-window --id 3。 */
	if (strcmp(name, "minimize-window") == 0 || strcmp(name, "minimize") == 0) {
		if (server->focused == NULL) {
			wlr_log(WLR_ERROR, "minimize-window: 现在没有聚焦的窗口");
			return false;
		}
		vidow_view_set_minimized(server->focused, true);
		return true;
	}
	if (strcmp(name, "unminimize-window") == 0 ||
			strcmp(name, "restore-window") == 0) {
		if (!vidow_view_restore_last(server)) {
			wlr_log(WLR_ERROR, "restore-window: 现在没有最小化的窗口");
			return false;
		}
		return true;
	}
	if (strcmp(name, "toggle-minimize") == 0) {
		if (server->focused == NULL) {
			wlr_log(WLR_ERROR, "toggle-minimize: 现在没有聚焦的窗口");
			return false;
		}
		return vidow_view_toggle_minimized(server->focused);
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
		/* 注入没有设备帧，自己收一个：wl_pointer v5+ 的客户端（GTK/GDK）会把
		 * 没收帧的事件扣在缓冲里，等到下一个指针事件才处理 —— 测试里就是这条
		 * 把「按下 / 松手 / 移动」的时序全带偏（实测能差好几秒）。 */
		wlr_seat_pointer_notify_frame(server->seat);
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
		/* 认名字也认数字（BTN_LEFT = 272 这种数字写在测试里没人记得住）。
		 * 名字表在 input.c，和 IPC 的 pointer-button 共用一份。 */
		uint32_t btn = vidow_pointer_button_from_name(key);
		if (btn == 0) {
			wlr_log(WLR_ERROR, "不认识的按钮 '%s'"
				"（left / right / middle / back / forward / 数字）", key);
			return false;
		}
		bool pressed = strcmp(state, "press") == 0;
		vidow_pointer_button(server, btn, pressed,
			(uint32_t)(vidow_now_ns() / 1000000LL),
			strcmp(extra, "mod") == 0);
		/* 同 pointer-motion：注入要自己收帧，不然客户端把这次按下扣着不放。 */
		wlr_seat_pointer_notify_frame(server->seat);
		return true;
	}

	wlr_log(WLR_ERROR, "action: 不认识的动作 '%s'", action);
	return false;
}
