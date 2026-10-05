#include "server.h"
#include "ipc.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

/* The config file is Lua. Two things make it feel like a config file and not
 * like a script:
 *
 *   border_width = 2                       -- a global name *is* a config key
 *   local accent = vidow.exec("theme.sh accent")   -- looks synchronous
 *
 * The second one is the interesting part: the whole chunk runs inside a
 * coroutine. vidow.exec() yields the command, the compositor runs it through
 * the event loop, and the coroutine is resumed with the output. The render
 * loop never waits, and the config never has to be written with callbacks.
 *
 * vidow.exec()        -> out            (exit 0), or nil, reason
 * vidow.exec_status() -> out, code, err (always three values)
 */
struct vidow_script {
	struct vidow_server *server;
	lua_State *L;  /* main state; owns the coroutine */
	lua_State *co; /* the coroutine running the chunk */
	struct vidow_exec *job;
	int nargs;        /* arguments for the next resume */
	bool want_status; /* the pending yield came from vidow.exec_status() */
	int execs;        /* commands run so far, for the log */
	bool finished;
};

static struct vidow_script *script_of(lua_State *L) {
	return lua_touserdata(L, lua_upvalueindex(1));
}

/* --------------------------------------------------------------- values */

/* Turn a Lua value into the text the plain config parser understands. */
static bool value_as_text(lua_State *L, int index, char *buf, size_t size) {
	switch (lua_type(L, index)) {
	case LUA_TNUMBER:
		snprintf(buf, size, "%.17g", lua_tonumber(L, index));
		return true;
	case LUA_TBOOLEAN:
		snprintf(buf, size, "%s", lua_toboolean(L, index) ? "true" : "false");
		return true;
	case LUA_TSTRING:
		snprintf(buf, size, "%s", lua_tostring(L, index));
		return true;
	default:
		return false;
	}
}

/* `gaps = { top = 30, inner = 20 }`：一张表按「配置族」展开成平铺键
 * （gaps_top / gaps_inner）。表里的项名不认识就报错并列出能写哪些 —— 这类
 * 拼写错误悄悄生效最难受。 */
static int expand_group(lua_State *L, struct vidow_config *cfg,
		const char *group, int table_index) {
	table_index = lua_absindex(L, table_index);
	lua_pushnil(L);
	while (lua_next(L, table_index) != 0) {
		/* 键在 -2、值在 -1 */
		if (lua_type(L, -2) != LUA_TSTRING) {
			return luaL_error(L, "%s = {…}: 项名得是字符串", group);
		}
		const char *field = lua_tostring(L, -2);
		const char *flat = vidow_config_group_field(group, field);
		if (flat == NULL) {
			const char *names[16];
			size_t n = vidow_config_group_fields(group, names, 16);
			char list[192] = "";
			for (size_t i = 0; i < n; i++) {
				snprintf(list + strlen(list), sizeof(list) - strlen(list),
					"%s%s", i > 0 ? " / " : "", names[i]);
			}
			return luaL_error(L, "%s = {…}: 没有 '%s' 这一项（能写：%s）",
				group, field, list);
		}
		char buf[256];
		if (!value_as_text(L, -1, buf, sizeof(buf))) {
			return luaL_error(L, "%s = { %s = … }: 不能用 %s 当配置值",
				group, field, lua_typename(L, lua_type(L, -1)));
		}
		if (!vidow_config_set(cfg, flat, buf)) {
			return luaL_error(L, "%s = '%s' is not a valid value", flat, buf);
		}
		lua_pop(L, 1);
	}
	return 0;
}

/* 一个配置键的赋值。值是标量就照平铺键存；是表就说明这个名字是一个「族」，
 * 表里每项展开成族里的平铺键（`border = { width = 0 }` → border_width = 0）。 */
static int assign_config(lua_State *L, struct vidow_config *cfg,
		const char *key, int value_index) {
	if (lua_type(L, value_index) == LUA_TTABLE) {
		if (!vidow_config_group_has(key)) {
			return luaL_error(L, "%s = {…}: %s 不是一个「族」，这里不能写表",
				key, key);
		}
		return expand_group(L, cfg, key, value_index);
	}
	/* `blur = false` 这种：族名本身也能当值用（落到族里指定的那个平铺键）。 */
	const char *flat = vidow_config_has(key) ? key :
		vidow_config_group_scalar(key);
	if (flat == NULL) {
		/* 只有表的族（border / shadow / anim / portal）：光一个值没法解释。 */
		const char *names[16];
		size_t n = vidow_config_group_fields(key, names, 16);
		return luaL_error(L, "%s 得写成一张表，比如 %s = { %s = … }",
			key, key, n > 0 ? names[0] : "…");
	}
	char buf[256];
	if (!value_as_text(L, value_index, buf, sizeof(buf))) {
		return luaL_error(L, "%s: cannot use a %s as a config value", key,
			lua_typename(L, lua_type(L, value_index)));
	}
	if (!vidow_config_set(cfg, flat, buf)) {
		return luaL_error(L, "%s = '%s' is not a valid value", flat, buf);
	}
	return 0;
}

/* `border_width = 2` in the config file. Unknown names stay ordinary globals,
 * so helpers and locals keep working. */
static int script_newindex(lua_State *L) {
	struct vidow_script *s = script_of(L);
	const char *key = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : NULL;
	if (key == NULL ||
			(!vidow_config_has(key) && !vidow_config_group_has(key))) {
		const char *hint = NULL;
		if (key != NULL && vidow_config_legacy_hint(key, &hint)) {
			wlr_log(WLR_ERROR, "config.lua: %s 这个键没有了（现在用 %s），"
				"这一行不生效", key, hint);
		}
		lua_rawset(L, 1);
		return 0;
	}
	(void)assign_config(L, &s->server->config, key, 3); /* 出错时 luaL_error 直接抛 */
	lua_rawset(L, 1); /* keep the global readable afterwards */
	return 0;
}
static int script_index(lua_State *L) {
	struct vidow_script *s = script_of(L);
	if (lua_type(L, 2) != LUA_TSTRING) {
		lua_pushnil(L);
		return 1;
	}
	const char *key = lua_tostring(L, 2);
	const char *value = vidow_config_has(key) ?
		vidow_config_get(&s->server->config, key) : NULL;
	if (value == NULL) {
		lua_pushnil(L);
		return 1;
	}
	char *end = NULL;
	double num = strtod(value, &end);
	if (end != NULL && end != value && *end == '\0') {
		lua_pushnumber(L, num);
	} else if (strcmp(value, "true") == 0) {
		lua_pushboolean(L, 1);
	} else if (strcmp(value, "false") == 0) {
		lua_pushboolean(L, 0);
	} else {
		lua_pushstring(L, value);
	}
	return 1;
}

/* ------------------------------------------------------------ vidow.* */

/* Yields the command; the compositor runs it and resumes us with the output,
 * which becomes the return value. From Lua this reads as a blocking call. */
static int l_exec_yield(lua_State *L, bool want_status) {
	size_t len = 0;
	const char *cmd = luaL_checklstring(L, 1, &len);
	if (len == 0) {
		if (want_status) {
			lua_pushstring(L, "");
			lua_pushinteger(L, -1);
			lua_pushstring(L, "empty command");
			return 3;
		}
		lua_pushnil(L);
		lua_pushstring(L, "empty command");
		return 2;
	}
	lua_pushlstring(L, cmd, len);
	lua_pushboolean(L, want_status);
	return lua_yield(L, 2);
}

static int l_exec(lua_State *L) {
	return l_exec_yield(L, false);
}

static int l_exec_status(lua_State *L) {
	return l_exec_yield(L, true);
}

static int l_set(lua_State *L) {
	struct vidow_script *s = script_of(L);
	const char *key = luaL_checkstring(L, 1);
	if (!vidow_config_has(key) && !vidow_config_group_has(key)) {
		return luaL_error(L, "unknown config key '%s'", key);
	}
	/* 表也一样认：vidow.set("border", { width = 0 }) */
	return assign_config(L, &s->server->config, key, 2);
}

static int l_get(lua_State *L) {
	struct vidow_script *s = script_of(L);
	const char *key = luaL_checkstring(L, 1);
	const char *name = vidow_config_has(key) ? key :
		vidow_config_group_scalar(key);
	if (name == NULL) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushstring(L, vidow_config_get(&s->server->config, name));
	return 1;
}

static int l_log(lua_State *L) {
	const char *msg = luaL_tolstring(L, 1, NULL);
	wlr_log(WLR_INFO, "config.lua: %s", msg != NULL ? msg : "");
	lua_pop(L, 1);
	return 0;
}

/* A blocking call in the config would stall the whole compositor, so the
 * footguns go away: use vidow.exec() instead, which never blocks. */
static void restrict_stdlib(lua_State *L) {
	lua_pushnil(L);
	lua_setglobal(L, "io");

	static const char *os_blocked[] = {
		"execute", "exit", "remove", "rename", "tmpname", "setlocale",
	};
	lua_getglobal(L, "os");
	if (lua_istable(L, -1)) {
		for (size_t i = 0; i < sizeof(os_blocked) / sizeof(os_blocked[0]); i++) {
			lua_pushnil(L);
			lua_setfield(L, -2, os_blocked[i]);
		}
	}
	lua_pop(L, 1);

	lua_getglobal(L, "package");
	if (lua_istable(L, -1)) {
		lua_pushnil(L); /* loadlib() would dodge everything above */
		lua_setfield(L, -2, "loadlib");
	}
	lua_pop(L, 1);
}

/* ------------------------------------------------------------ 工作区 / 键位 */

static int l_bind(lua_State *L) {
	struct vidow_script *s = script_of(L);
	const char *trigger = luaL_checkstring(L, 1);
	const char *action = luaL_checkstring(L, 2);
	char err[160];
	if (vidow_keybind_add(s->server, trigger, action, err, sizeof(err)) == NULL) {
		return luaL_error(L, "%s", err);
	}
	lua_pushboolean(L, 1);
	return 1;
}

/* 按输出名找一块屏（配置可以按名字把工作区指到某块屏上）。 */
static struct wlr_output *output_named(struct vidow_server *server,
		const char *name) {
	if (name == NULL || name[0] == '\0') {
		return NULL;
	}
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (strcmp(output->wlr_output->name, name) == 0) {
			return output->wlr_output;
		}
	}
	return NULL;
}

/* vidow.rule{ app_id = "mpv", title = "预览", class = "Mpv", force_render = true }
 * 三个匹配字段（app_id / title / class）都是「子串、大小写不敏感」，可以只写一个。 */
static const char *table_string(lua_State *L, int index, const char *key) {
	lua_getfield(L, index, key);
	const char *value = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
	lua_pop(L, 1);
	return value;
}

/* 表里有没有写这个布尔字段（nil = 没写 = 不动） */
static bool table_bool(lua_State *L, int index, const char *key, bool *out) {
	lua_getfield(L, index, key);
	bool has = !lua_isnil(L, -1);
	if (has) {
		*out = lua_toboolean(L, -1) != 0;
	}
	lua_pop(L, 1);
	return has;
}

static int l_rule(lua_State *L) {
	struct vidow_script *s = script_of(L);
	if (!lua_istable(L, 1)) {
		return luaL_error(L, "vidow.rule 要一个表，比如 "
			"vidow.rule{ app_id = \"mpv\", force_render = true }");
	}
	struct vidow_rule_spec spec = { 0 };
	spec.has_force_render = table_bool(L, 1, "force_render",
		&spec.force_render);

	/* hz = 这个窗口每秒补多少帧（0 = 不限）；不写就用配置里的 force_render_hz */
	lua_getfield(L, 1, "hz");
	spec.has_hz = !lua_isnil(L, -1);
	spec.hz = spec.has_hz ? (int)lua_tointeger(L, -1) : 0;
	lua_pop(L, 1);

	/* 焦点行为：新窗口抢不抢焦点 / 别人请求激活它时理不理 */
	spec.has_focus_on_open = table_bool(L, 1, "focus_on_open",
		&spec.focus_on_open);
	spec.has_focus_on_activate = table_bool(L, 1, "focus_on_activate",
		&spec.focus_on_activate);
	/* 它的激活请求即使令牌 serial 校验不过也照样理（默认不理） */
	spec.has_honor_invalid_serial = table_bool(L, 1, "honor_invalid_serial",
		&spec.honor_invalid_serial);

	/* 这个窗口的 X11 缩放桥（1 = 客户端没按 GDK_SCALE 画，别除）。只对 X11
	 * 窗口有意义：Wayland 客户端自己声明 buffer_scale。 */
	lua_getfield(L, 1, "x11_scale");
	spec.has_scale = !lua_isnil(L, -1);
	spec.scale = spec.has_scale ? lua_tonumber(L, -1) : 0.0;
	lua_pop(L, 1);

	if (!spec.has_force_render && !spec.has_hz && !spec.has_focus_on_open &&
			!spec.has_focus_on_activate && !spec.has_honor_invalid_serial &&
			!spec.has_scale) {
		return luaL_error(L, "vidow.rule 至少写一个作用：force_render = true / "
			"hz = <每秒补几帧，0 = 不限> / focus_on_open = false / "
			"focus_on_activate = false / honor_invalid_serial = true / "
			"x11_scale = <X11 缩放桥，1 = 不除>"
			"（匹配字段：app_id / title / class）");
	}

	spec.app_id = table_string(L, 1, "app_id");
	spec.title = table_string(L, 1, "title");
	spec.klass = table_string(L, 1, "class");
	struct vidow_rule *rule = vidow_rule_add(s->server, &spec);
	if (rule == NULL) {
		return luaL_error(L, "cannot add a window rule");
	}
	lua_pushboolean(L, 1);
	return 1;
}

/* 规则里的布尔开关：没写就推 nil（「这条规则没管它」和「这条规则管了、给的是
 * 默认值」是两回事）。 */
static void push_rule_bool(lua_State *L, bool has, bool value,
		const char *key) {
	if (has) {
		lua_pushboolean(L, value);
	} else {
		lua_pushnil(L);
	}
	lua_setfield(L, -2, key);
}

static int l_rules(lua_State *L) {
	struct vidow_script *s = script_of(L);
	lua_newtable(L);
	for (size_t i = 0; i < vidow_rule_count(s->server); i++) {
		const struct vidow_rule *rule = vidow_rule_at(s->server, i);
		if (rule == NULL) {
			continue;
		}
		lua_pushinteger(L, (lua_Integer)i + 1);
		lua_newtable(L);
		lua_pushstring(L, rule->app_id);
		lua_setfield(L, -2, "app_id");
		lua_pushstring(L, rule->title);
		lua_setfield(L, -2, "title");
		lua_pushstring(L, rule->klass);
		lua_setfield(L, -2, "class");
		if (rule->has_force_render) {
			lua_pushboolean(L, rule->force_render);
		} else {
			lua_pushnil(L);
		}
		lua_setfield(L, -2, "force_render");
		if (rule->has_hz) {
			lua_pushinteger(L, rule->hz);
		} else {
			lua_pushnil(L);
		}
		lua_setfield(L, -2, "hz");
		push_rule_bool(L, rule->has_focus_on_open, rule->focus_on_open,
			"focus_on_open");
		push_rule_bool(L, rule->has_focus_on_activate, rule->focus_on_activate,
			"focus_on_activate");
		push_rule_bool(L, rule->has_honor_invalid_serial,
			rule->honor_invalid_serial, "honor_invalid_serial");
		if (rule->has_scale) {
			lua_pushnumber(L, rule->scale);
		} else {
			lua_pushnil(L);
		}
		lua_setfield(L, -2, "x11_scale");
		lua_settable(L, -3);
	}
	return 1;
}

static int l_workspace_create(lua_State *L) {
	struct vidow_script *s = script_of(L);
	const char *name = luaL_optstring(L, 1, NULL);
	const char *on = luaL_optstring(L, 2, NULL);
	struct wlr_output *output = output_named(s->server, on);
	if (on != NULL && on[0] != '\0' && output == NULL) {
		return luaL_error(L, "no output named '%s'", on);
	}
	if (output == NULL) {
		output = s->server->focused_output;
	}
	struct vidow_workspace *ws = vidow_workspace_create(s->server, name, output);
	if (ws == NULL) {
		return luaL_error(L, "cannot create a workspace");
	}
	lua_pushstring(L, vidow_workspace_name(ws));
	return 1;
}

static int l_workspace_special(lua_State *L) {
	struct vidow_script *s = script_of(L);
	const char *name = luaL_optstring(L, 1, "special");
	struct vidow_workspace *ws =
		vidow_workspace_create_special(s->server, name);
	if (ws == NULL) {
		return luaL_error(L, "cannot create a special workspace");
	}
	lua_pushstring(L, vidow_workspace_name(ws));
	return 1;
}

/* 普通工作区列表（特殊工作区**不进这个列表**）：给状态栏和用户自己的选择器用。 */
static int l_workspace_list(lua_State *L) {
	struct vidow_script *s = script_of(L);
	lua_newtable(L);
	int n = 0;
	struct vidow_workspace *ws;
	wl_list_for_each(ws, &s->server->workspaces, link) {
		if (ws->special) {
			continue;
		}
		n++;
		lua_pushinteger(L, n);
		lua_newtable(L);
		/* 没名字的动态工作区：name 是空串，序号（idx）才是它的「门牌」 */
		lua_pushstring(L, vidow_workspace_name(ws));
		lua_setfield(L, -2, "name");
		lua_pushinteger(L, vidow_workspace_index(ws));
		lua_setfield(L, -2, "idx");
		lua_pushinteger(L, (lua_Integer)ws->id);
		lua_setfield(L, -2, "id");
		lua_pushboolean(L, ws->named);
		lua_setfield(L, -2, "named");
		lua_pushboolean(L, vidow_workspace_is_active(ws));
		lua_setfield(L, -2, "active");
		lua_pushboolean(L, vidow_workspace_is_visible(ws));
		lua_setfield(L, -2, "visible");
		lua_pushinteger(L, vidow_workspace_view_count(ws));
		lua_setfield(L, -2, "views");
		if (ws->output != NULL) {
			lua_pushstring(L, ws->output->name);
		} else {
			lua_pushnil(L);
		}
		lua_setfield(L, -2, "output");
		lua_settable(L, -3);
	}
	return 1;
}

/* vidow.ipc_path() —— IPC socket 的路径（没起来就是空串）。config.lua 里要
 * 起状态栏 / 脚本时用得着：把路径喂给子进程，别让它自己猜。 */
static int l_ipc_path(lua_State *L) {
	struct vidow_script *s = script_of(L);
	lua_pushstring(L, vidow_ipc_path(s->server));
	return 1;
}

static int l_workspace_active(lua_State *L) {
	struct vidow_script *s = script_of(L);
	struct vidow_workspace *ws =
		vidow_workspace_active(s->server);
	if (ws == NULL) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushstring(L, vidow_workspace_name(ws));
	return 1;
}

static int l_workspace_switch(lua_State *L) {
	struct vidow_script *s = script_of(L);
	const char *name = luaL_checkstring(L, 1);
	struct vidow_workspace *ws = vidow_workspace_lookup(s->server, name);
	if (ws == NULL) {
		lua_pushboolean(L, 0);
		return 1;
	}
	lua_pushboolean(L, vidow_workspace_activate(s->server, ws));
	return 1;
}

static int l_workspace_toggle_special(lua_State *L) {
	struct vidow_script *s = script_of(L);
	const char *name = luaL_optstring(L, 1, "special");
	lua_pushboolean(L, vidow_workspace_toggle_special(s->server, name));
	return 1;
}

static int l_workspace_move(lua_State *L) {
	struct vidow_script *s = script_of(L);
	const char *name = luaL_checkstring(L, 1);
	lua_pushboolean(L, vidow_workspace_move_focused(s->server, name));
	return 1;
}

static void register_api(struct vidow_script *s) {
	lua_State *L = s->L;
	lua_newtable(L);
	struct {
		const char *name;
		lua_CFunction fn;
	} fns[] = {
		{ "exec", l_exec },
		{ "exec_status", l_exec_status },
		{ "set", l_set },
		{ "get", l_get },
		{ "log", l_log },
		{ "bind", l_bind },
		{ "workspace_create", l_workspace_create },
		{ "workspace_special", l_workspace_special },
		{ "workspace_list", l_workspace_list },
		{ "workspace_active", l_workspace_active },
		{ "workspace_switch", l_workspace_switch },
		{ "workspace_toggle_special", l_workspace_toggle_special },
		{ "workspace_move", l_workspace_move },
		{ "ipc_path", l_ipc_path },
		{ "rule", l_rule },
		{ "rules", l_rules },
	};
	for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
		lua_pushlightuserdata(L, s);
		lua_pushcclosure(L, fns[i].fn, 1);
		lua_setfield(L, -2, fns[i].name);
	}
	lua_pushstring(L, "vidow 0.1 (lua " LUA_VERSION ")");
	lua_setfield(L, -2, "version");
	lua_setglobal(L, "vidow");

	/* Config keys as globals. */
	lua_pushglobaltable(L);
	lua_newtable(L);
	lua_pushlightuserdata(L, s);
	lua_pushcclosure(L, script_newindex, 1);
	lua_setfield(L, -2, "__newindex");
	lua_pushlightuserdata(L, s);
	lua_pushcclosure(L, script_index, 1);
	lua_setfield(L, -2, "__index");
	lua_setmetatable(L, -2);
	lua_pop(L, 1);
}

/* ------------------------------------------------------------- lifecycle */

/* 配置落地之后对一次账。
 *
 * 有几个键是在「配置还跑不了」的时候就被消费掉的：ipc / xwayland 的初始化早于
 * 配置文件的执行（IPC socket 必须在配置之前就在，配置里才拿得到
 * vidow.ipc_path()；X 服务的 DISPLAY 也必须在配置里的子进程之前设好），而缩放
 * 是在建输出的时候读的。所以启动时先按默认值建好，等配置跑完再由这里按配置
 * 修正一遍。全是幂等的，没变就什么都不做。 */
void vidow_config_sync(struct vidow_server *server) {
	vidow_ipc_apply_config(server);
	vidow_xwayland_apply_config(server);
	/* 配置可能改了 scale / outputs / xwayland_scale：改到的输出立刻换缩放，
	 * XWayland 的全局缩放也跟着重算。 */
	vidow_output_apply_scale_all(server);
	vidow_xwayland_scale_apply(server);
	/* clipboard / clipboard_primary 可能被改过：重新评估接不接管 */
	vidow_clipboard_apply_config(server);
	/* 激活令牌的作废定时器是在 init 里按当时的配置建的 */
	vidow_activation_apply_config(server);
	/* decor 开关改了：客户端要不要自己画标题栏得重新通知（见 decoration.c） */
	vidow_decoration_apply_config(server);
	/* portal 那两个键改了也要跟上：XDG_CURRENT_DESKTOP 重设一遍，生效的
	 * portals.conf 重新算一遍（内容没变的话什么都不做，不会白重启 portal）。 */
	vidow_portal_desktop_env(&server->config);
	vidow_portal_conf_sync(&server->config);
	/* 配置里 vidow.workspace_create() 声明的具名工作区可能是在还没有屏的时候
	 * 建的（output == NULL）：现在认领到第一块屏上，按声明顺序排在栈顶。 */
	vidow_workspace_normalize(server);
}

static void script_apply(struct vidow_script *s) {
	struct vidow_server *server = s->server;

	/* The environment still wins over the file, so tests stay reproducible. */
	vidow_config_apply_env(&server->config);
	vidow_config_log(&server->config);

	/* 有些键的消费时机早于配置文件的执行（ipc / xwayland / scale / 剪贴板），
	 * 这里按最终的配置修正一遍。 */
	vidow_config_sync(server);

	/* 规则可能按 app_id / 标题匹配已经开着的窗口：重载之后重新判一遍。 */
	vidow_rule_apply_all(server);

	vidow_backdrop_damage(server, true);
	vidow_arrange(server);

	/* Decorations are only redrawn on commit or layout change, so a config
	 * that changes the radius or the colours has to push them through. */
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		vidow_view_apply_anim(view);
	}

	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		wlr_output_schedule_frame(output->wlr_output);
	}
}

static void script_done(struct vidow_script *s, const char *why) {
	if (s->finished) {
		return;
	}
	s->finished = true;
	if (why != NULL) {
		wlr_log(WLR_ERROR, "config.lua: %s", why);
	} else {
		wlr_log(WLR_INFO, "config.lua: applied (%d command%s)", s->execs,
			s->execs == 1 ? "" : "s");
	}
	script_apply(s);
}

static void script_resume(struct vidow_script *s);

/* 配置里跑 shell 命令之前，先把「子进程会继承的东西」落到环境和输出上。
 *
 * 配置是分步跑完的：每次 vidow.exec() 让回事件循环，命令在事件循环里执行。而
 * scale / outputs 要到配置跑完才对上输出，XWayland 的全局缩放又跟着输出走 ——
 * 配置里第一条命令（尤其是 X11 的）就会拿到**启动那一刻**的 GDK_SCALE /
 * QT_SCALE_FACTOR / DISPLAY。所以每条命令之前先对一次账：这几个调用都是幂等的，
 * 没变就什么都不做。 */
static void script_sync_env_for_child(struct vidow_server *server) {
	vidow_output_apply_scale_all(server);
	vidow_xwayland_scale_apply(server);
	vidow_portal_desktop_env(&server->config);
}

static void script_exec_done(void *data, const char *out, const char *errout,
		int status, bool ok) {
	struct vidow_script *s = data;
	s->job = NULL;
	if (s->finished) {
		return;
	}
	lua_State *co = s->co;
	bool full = s->want_status;
	s->want_status = false;

	if (full) {
		/* vidow.exec_status(): always three values, never nil for out. */
		lua_pushstring(co, out != NULL ? out : "");
		lua_pushinteger(co, ok ? status : -1);
		lua_pushstring(co, errout != NULL ? errout : "");
		s->nargs = 3;
	} else if (!ok) {
		lua_pushnil(co);
		lua_pushstring(co, errout != NULL ? errout : "failed");
		s->nargs = 2;
	} else if (status != 0) {
		/* A command that failed is not a value. Its own message is the most
		 * useful thing to report; fall back to the exit code. */
		char buf[512];
		if (errout != NULL && errout[0] != '\0') {
			snprintf(buf, sizeof(buf), "%s", errout);
		} else {
			snprintf(buf, sizeof(buf), "exit %d", status);
		}
		lua_pushnil(co);
		lua_pushstring(co, buf);
		s->nargs = 2;
	} else {
		/* Exactly one value, so the common case can be used directly:
		 *   corner_radius = tonumber(vidow.exec("echo 12")) */
		lua_pushstring(co, out != NULL ? out : "");
		s->nargs = 1;
	}
	script_resume(s);
}

static void script_resume(struct vidow_script *s) {
	int nres = 0;
	int status = lua_resume(s->co, NULL, s->nargs, &nres);
	s->nargs = 0;

	if (status == LUA_YIELD) {
		bool want_status = lua_toboolean(s->co, -1) != 0;
		size_t len = 0;
		const char *cmd = lua_tolstring(s->co, -2, &len);
		if (cmd == NULL) {
			script_done(s, "vidow.exec() wants a string");
			return;
		}
		char *copy = strndup(cmd, len);
		lua_pop(s->co, 2);
		if (copy == NULL) {
			script_done(s, "out of memory");
			return;
		}
		s->execs++;
		s->want_status = want_status;
		script_sync_env_for_child(s->server);
		/* On failure this calls script_exec_done() before returning. */
		s->job = vidow_exec_run(s->server, copy, script_exec_done, s);
		free(copy);
		return;
	}

	if (status != LUA_OK) {
		const char *msg = lua_tostring(s->co, -1);
		char buf[512];
		snprintf(buf, sizeof(buf), "%s", msg != NULL ? msg : "unknown error");
		script_done(s, buf);
		return;
	}
	script_done(s, NULL);
}

/* --------------------------------------------------- 测试钩子 */

/* VIDOW_TEST_ACTION="600:workspace 2;1200:special scratch" —— 起完配置之后按
 * 毫秒时间表跑几条动作，黑盒测试靠它验工作区（不用真按键，也就不受键盘布局影响）。
 * 动作走的是和键位绑定完全相同的入口 vidow_action_run()。 */
struct vidow_test_action {
	struct wl_list link;
	struct vidow_server *server;
	struct wl_event_source *timer;
	char action[128];
};

static int test_action_cb(void *data) {
	struct vidow_test_action *step = data;
	wlr_log(WLR_INFO, "test-action: 执行 '%s'", step->action);
	bool ok = vidow_action_run(step->server, step->action);
	wlr_log(WLR_INFO, "test-action: '%s' %s", step->action,
		ok ? "完成" : "没做成");
	return 0;
}

static void test_actions_clear(struct vidow_server *server) {
	struct vidow_test_action *step, *tmp;
	wl_list_for_each_safe(step, tmp, &server->test_actions, link) {
		wl_list_remove(&step->link);
		wl_event_source_remove(step->timer);
		free(step);
	}
}

/* 动作管道：VIDOW_TEST_ACTION="file:<路径>" —— 那是测试开的一个 FIFO，往里写一行
 * 动作就立刻执行一行（一行一条，同步可判）。一次性定时表和它共用一个入口。 */
static int test_pipe_cb(int fd, uint32_t mask, void *data) {
	struct vidow_server *server = data;
	if (mask & WL_EVENT_READABLE) {
		char buf[512];
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = '\0';
			char *save = NULL;
			for (char *line = strtok_r(buf, "\n", &save); line != NULL;
					line = strtok_r(NULL, "\n", &save)) {
				if (line[0] == '\0') {
					continue;
				}
				bool ok = vidow_action_run(server, line);
				wlr_log(WLR_INFO, "test-action: '%s' %s", line,
					ok ? "完成" : "没做成");
			}
		}
	}
	if (mask & WL_EVENT_HANGUP) {
		wl_event_source_remove(server->test_pipe);
		server->test_pipe = NULL;
	}
	return 0;
}

static void test_actions_start(struct vidow_server *server) {
	const char *spec = getenv("VIDOW_TEST_ACTION");
	if (spec != NULL && strncmp(spec, "file:", 5) == 0) {
		int fd = open(spec + 5, O_RDONLY | O_NONBLOCK);
		if (fd < 0) {
			wlr_log(WLR_ERROR, "test-action: 打不开 %s", spec + 5);
			return;
		}
		server->test_pipe = wl_event_loop_add_fd(
			wl_display_get_event_loop(server->display), fd,
			WL_EVENT_READABLE, test_pipe_cb, server);
		wlr_log(WLR_INFO, "test-action: 听 %s（一行一条动作）", spec + 5);
		return;
	}
	if (spec == NULL || spec[0] == '\0') {
		return;
	}
	struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
	char buf[512];
	snprintf(buf, sizeof(buf), "%s", spec);
	int fallback = 200;
	char *step_save = NULL;
	for (char *item = strtok_r(buf, ";", &step_save); item != NULL;
			item = strtok_r(NULL, ";", &step_save)) {
		char *colon = strchr(item, ':');
		int delay = fallback;
		char *action = item;
		if (colon != NULL) {
			*colon = '\0';
			delay = atoi(item);
			action = colon + 1;
		}
		if (action[0] == '\0' || delay < 0) {
			continue;
		}
		fallback = delay + 400;
		struct vidow_test_action *step = calloc(1, sizeof(*step));
		if (step == NULL) {
			return;
		}
		step->server = server;
		snprintf(step->action, sizeof(step->action), "%s", action);
		step->timer = wl_event_loop_add_timer(loop, test_action_cb, step);
		if (step->timer == NULL) {
			free(step);
			continue;
		}
		wl_event_source_timer_update(step->timer, delay);
		wl_list_insert(server->test_actions.prev, &step->link);
		wlr_log(WLR_INFO, "test-action: %dms 后跑 '%s'", delay, step->action);
	}
}

void vidow_script_finish(struct vidow_server *server) {
	struct vidow_script *s = server->script;
	if (s == NULL) {
		return;
	}
	server->script = NULL;
	s->finished = true;
	vidow_exec_cancel_all(server); /* drops the callbacks with the jobs */
	if (s->L != NULL) {
		lua_close(s->L);
	}
	free(s);
}

bool vidow_script_load(struct vidow_server *server) {
	vidow_script_finish(server);
	test_actions_clear(server);
	/* 重载 = 重新声明一遍键位和窗口规则：清掉旧的，免得 SIGHUP 之后越攒越多。 */
	vidow_keybind_clear(server);
	vidow_rule_clear(server);
	/* 重载 = 重新读一遍配置：先回到默认值（再压上环境变量）。配置文件里删掉的
	 * 行才会真的消失 —— 不然改完配置重载，被删的键还留着旧值。 */
	vidow_config_reset(&server->config);

	char path[PATH_MAX];
	vidow_config_path(path, sizeof(path));

	FILE *probe = fopen(path, "r");
	if (probe == NULL) {
		wlr_log(WLR_INFO, "config.lua: 没有 %s，全部用默认值", path);
		vidow_config_sync(server);
		return false;
	}
	fclose(probe);

	struct vidow_script *s = calloc(1, sizeof(*s));
	if (s == NULL) {
		vidow_config_sync(server);
		return false;
	}
	s->server = server;
	s->L = luaL_newstate();
	if (s->L == NULL) {
		free(s);
		wlr_log(WLR_ERROR, "config.lua: cannot create a Lua state");
		vidow_config_sync(server);
		return false;
	}
	luaL_openlibs(s->L);
	restrict_stdlib(s->L);
	register_api(s);

	if (luaL_loadfile(s->L, path) != LUA_OK) {
		wlr_log(WLR_ERROR, "config.lua: %s: %s", path,
			lua_tostring(s->L, -1) != NULL ? lua_tostring(s->L, -1) : "?");
		lua_close(s->L);
		free(s);
		vidow_config_sync(server);
		return false;
	}

	/* The thread stays on the main stack, which keeps it alive. */
	s->co = lua_newthread(s->L);
	lua_pushvalue(s->L, -2);
	lua_xmove(s->L, s->co, 1);
	lua_remove(s->L, -2);

	server->script = s;
	wlr_log(WLR_INFO, "config.lua: running %s", path);
	script_resume(s);
	test_actions_start(server);
	return true;
}

/* 重载配置（SIGHUP 和 IPC 的 reload-config 共用）。
 *
 * 重新跑 config.lua；跟缩放 / 剪贴板有关的东西不在这里应用 —— 配置带
 * vidow.exec() 时它是分几步跑完的（每次 yield 回事件循环），要等配置真正跑完
 * 才该动输出，那一步在 script_apply() 里（见 vidow_config_sync()）。 */
void vidow_config_reload(struct vidow_server *server) {
	vidow_script_load(server);
	vidow_ipc_emit_note(server, "config-reloaded");
}

static int script_reload_signal(int signo, void *data) {
	(void)signo;
	struct vidow_server *server = data;
	wlr_log(WLR_INFO, "config.lua: reloading (SIGHUP)");
	vidow_config_reload(server);
	return 0;
}

bool vidow_script_init(struct vidow_server *server) {
	server->reload_sig = wl_event_loop_add_signal(
		wl_display_get_event_loop(server->display), SIGHUP,
		script_reload_signal, server);
	if (server->reload_sig == NULL) {
		wlr_log(WLR_ERROR, "failed to install the SIGHUP reload handler");
		return false;
	}
	return true;
}
