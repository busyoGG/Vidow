#include "env.h"
#include "hooks.h"
#include "json.h"
#include "server.h"
#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
 * vidow.exec(cmd)                     -> out            (exit 0), or nil, reason
 * vidow.exec(cmd, { status = true })  -> out, code, err (always three values)
 * vidow.exec(cmd, { wait = false })   -> true           (撒手跑，不等)
 *
 * 只有两个入口：vidow.exec()（跑一条，等不等 / 要不要退出码看选项）和
 * vidow.exec_once()（一个 vidow 生命周期只起一次）。原来的 vidow.spawn() 和
 * vidow.exec_status() 已经并成 vidow.exec() 的选项 —— 老写法会报一句人话
 * （见 l_api_index）。
 */
struct vidow_script {
	struct vidow_server *server;
	lua_State *L;  /* main state; owns the coroutine */
	lua_State *co; /* the coroutine running the chunk */
	struct vidow_exec *job;
	int nargs;        /* arguments for the next resume */
	bool want_status; /* 这次 yield 是 vidow.exec(cmd, { status = true }) 发的 */
	int execs;        /* commands run so far, for the log */
	bool finished;
};

static struct vidow_script *script_of(lua_State *L) {
	return lua_touserdata(L, lua_upvalueindex(1));
}

/* 定义在下面（配置里起子进程之前先对一次环境：缩放 / DISPLAY）。 */
static void script_sync_env_for_child(struct vidow_server *server);

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

/* vidow.exec() 的第二个参数（选项表）：
 *
 *   { wait = false }    撒手跑：不等它、不要输出（原来单独一个 vidow.spawn()）
 *   { status = true }   要退出码：返回 out, code, err（原来单独一个 vidow.exec_status()）
 *
 * 两个都能给。不认识的项名当场报错并列出能写哪些 —— 拼错的选项悄悄不生效最难受。
 * def_wait 是「没写 wait 时算不算等」：vidow.exec() 默认等，vidow.exec_once() 默认
 * 不等（它本来就是撒手跑的）。 */
static void exec_opts(lua_State *L, int index, bool def_wait, bool *wait,
		bool *status) {
	*wait = def_wait;
	*status = false;
	/* index <= 0 = 调用方说「这次没有选项表」。**不能拿 0 当「没有」再交给
	 * lua_isnoneornil 判**：0 不是合法索引，读的是栈顶那个槽位 —— 有时是 nil
	 * （看着没事），有时是上一次操作留下的残值，于是同一个 vidow.exec_once()
	 * 在一个配置里好使、在另一个配置里报「第二个参数得是张选项表」。 */
	if (index <= 0 || lua_isnoneornil(L, index)) {
		return;
	}
	if (!lua_istable(L, index)) {
		luaL_error(L, "第二个参数得是张选项表，比如 "
			"{ wait = false } / { status = true }");
		return;
	}
	index = lua_absindex(L, index);
	lua_pushnil(L);
	while (lua_next(L, index) != 0) {
		const char *key = lua_tostring(L, -2);
		bool val = lua_toboolean(L, -1);
		if (key == NULL) {
			luaL_error(L, "选项表的键得是字符串（wait / status）");
			return;
		}
		if (strcmp(key, "wait") == 0) {
			*wait = val;
		} else if (strcmp(key, "status") == 0) {
			*status = val;
		} else {
			luaL_error(L, "不认识的选项 '%s'（能写：wait / status）", key);
			return;
		}
		lua_pop(L, 1);
	}
}

/* vidow.exec("命令")            等它跑完，返回 out（退出码非 0 时 nil + 原因）
 * vidow.exec("命令", { status = true })   返回 out, code, err
 * vidow.exec("waybar", { wait = false })  撒手：不等、不要输出（起常驻程序用这个）
 *
 * 等的那条路：yield 给合成器（命令在事件循环里跑），拿到输出再 resume —— 从 Lua
 * 看是同步的，渲染一点没停。 */
static int l_exec(lua_State *L) {
	struct vidow_script *s = script_of(L);
	size_t len = 0;
	const char *cmd = luaL_checklstring(L, 1, &len);
	bool wait, status;
	exec_opts(L, 2, true, &wait, &status);

	if (len == 0) {
		if (status) {
			lua_pushstring(L, "");
			lua_pushinteger(L, -1);
			lua_pushstring(L, "empty command");
			return 3;
		}
		lua_pushnil(L);
		lua_pushstring(L, "empty command");
		return 2;
	}

	if (!wait) {
		/* 撒手跑：和配置里起的其它子进程一样，先对一次环境（缩放 / DISPLAY）。 */
		char *copy = strndup(cmd, len);
		if (copy == NULL) {
			return luaL_error(L, "out of memory");
		}
		script_sync_env_for_child(s->server);
		vidow_exec_detached(copy);
		s->execs++;
		free(copy);
		lua_pushboolean(L, 1);
		return 1;
	}

	lua_pushlstring(L, cmd, len);
	lua_pushboolean(L, status);
	return lua_yield(L, 2);
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

/* ------------------------------------------------------------ 事件钩子 */

/* 一个 Lua 钩子的这次调用（见下面 l_on 和 vidow_script_hook_run）。
 *
 * 和配置文件那套协程一样能 await vidow.exec()（yield 一条命令、回来接着跑），但
 * 两边的「跑完了干什么」不一样：配置跑完要 apply 整个配置，钩子跑完什么都不用
 * 做。所以状态单独存一份，队列在 server->hook_calls 里。配置重载会换一代 Lua
 * 状态，那些还挂着的调用跟着扔掉（命令本身由 vidow_exec_cancel_all 收掉）。 */
static void script_sync_env_for_child(struct vidow_server *server);

struct hook_call {
	struct wl_list link;   /* vidow_server.hook_calls */
	struct vidow_server *server;
	int id;                /* 钩子的 id，只为日志（钩子本身可能已经没了） */
	lua_State *L;          /* 主状态（协程在主状态的注册表里留着） */
	lua_State *co;
	int co_ref;
	int nargs;
	bool want_status;
};

bool vidow_script_alive(const struct vidow_server *server, void *lua_state) {
	return server != NULL && server->script != NULL &&
		server->script->L == lua_state;
}

void vidow_script_hook_unref(void *lua_state, int ref) {
	if (lua_state != NULL && ref != 0) {
		luaL_unref((lua_State *)lua_state, LUA_REGISTRYINDEX, ref);
	}
}

/* JSON -> Lua 表：钩子的参数（和 IPC 订阅者看到的那条 JSON 同构，字段名一样）。 */
static void json_to_lua(lua_State *L, const struct jval *v) {
	switch (v->t) {
	case J_NULL:
		lua_pushnil(L);
		break;
	case J_BOOL:
		lua_pushboolean(L, v->b);
		break;
	case J_NUM:
		lua_pushnumber(L, v->num);
		break;
	case J_STR:
		lua_pushstring(L, v->str != NULL ? v->str : "");
		break;
	case J_ARR:
		lua_createtable(L, (int)v->n, 0);
		for (size_t i = 0; i < v->n; i++) {
			json_to_lua(L, v->items[i]);
			lua_rawseti(L, -2, (lua_Integer)i + 1);
		}
		break;
	case J_OBJ:
		lua_createtable(L, 0, (int)v->n);
		for (size_t i = 0; i < v->n; i++) {
			json_to_lua(L, v->items[i]);
			lua_setfield(L, -2, v->keys[i]);
		}
		break;
	}
}

static void hook_call_free(struct hook_call *hc) {
	wl_list_remove(&hc->link);
	if (hc->L != NULL && hc->co_ref != 0) {
		luaL_unref(hc->L, LUA_REGISTRYINDEX, hc->co_ref);
	}
	free(hc);
}

/* 配置重载 / 退出：还挂着的钩子调用全扔掉（Lua 状态要关，命令已经收过）。 */
static void hook_calls_abort(struct vidow_server *server) {
	struct hook_call *hc, *tmp;
	wl_list_for_each_safe(hc, tmp, &server->hook_calls, link) {
		hook_call_free(hc);
	}
}

static void hook_call_resume(struct hook_call *hc);

static void hook_call_done(void *data, const char *out, const char *errout,
		int status, bool ok) {
	struct hook_call *hc = data;
	lua_State *co = hc->co;
	if (hc->want_status) {
		lua_pushstring(co, out != NULL ? out : "");
		lua_pushinteger(co, ok ? status : -1);
		lua_pushstring(co, errout != NULL ? errout : "");
		hc->nargs = 3;
	} else if (!ok || status != 0) {
		char buf[512];
		if (errout != NULL && errout[0] != '\0') {
			snprintf(buf, sizeof(buf), "%s", errout);
		} else {
			snprintf(buf, sizeof(buf), "exit %d", status);
		}
		lua_pushnil(co);
		lua_pushstring(co, buf);
		hc->nargs = 2;
	} else {
		lua_pushstring(co, out != NULL ? out : "");
		hc->nargs = 1;
	}
	hook_call_resume(hc);
}

static void hook_call_resume(struct hook_call *hc) {
	int nres = 0;
	int status = lua_resume(hc->co, NULL, hc->nargs, &nres);
	hc->nargs = 0;

	if (status == LUA_YIELD) {
		/* vidow.exec()：yield 出来的是 (命令, 要不要状态) */
		bool want_status = lua_toboolean(hc->co, -1) != 0;
		size_t len = 0;
		const char *cmd = lua_tolstring(hc->co, -2, &len);
		lua_pop(hc->co, 2);
		if (cmd == NULL || len == 0) {
			hook_call_free(hc);
			return;
		}
		char *copy = strndup(cmd, len);
		if (copy == NULL) {
			hook_call_free(hc);
			return;
		}
		hc->want_status = want_status;
		script_sync_env_for_child(hc->server);
		/* 失败时 vidow_exec_run 会**先**调一次 hook_call_done（里面可能把 hc
		 * 放掉了），所以这之后不能再碰 hc。 */
		if (vidow_exec_run(hc->server, copy, hook_call_done, hc) == NULL) {
			free(copy);
			return;
		}
		free(copy);
		return;
	}

	if (status != LUA_OK) {
		const char *msg = lua_tostring(hc->co, -1);
		wlr_log(WLR_ERROR, "钩子 #%d 出错：%s", hc->id,
			msg != NULL ? msg : "unknown error");
	}
	hook_call_free(hc);
}

bool vidow_script_hook_run(struct vidow_server *server, struct vidow_hook *hook,
		const char *json) {
	struct vidow_script *s = server != NULL ? server->script : NULL;
	if (s == NULL || s->L == NULL || hook == NULL) {
		return false;
	}
	/* 上一代配置留下的钩子（Lua 状态已经换了）：不碰它 */
	if (hook->lua_state != s->L || hook->lua_ref == 0) {
		return false;
	}
	lua_State *L = s->L;
	lua_rawgeti(L, LUA_REGISTRYINDEX, hook->lua_ref);
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 1);
		return false;
	}

	struct hook_call *hc = calloc(1, sizeof(*hc));
	lua_State *co = lua_newthread(L);
	if (hc == NULL || co == NULL) {
		lua_pop(L, 2); /* 函数 + 线程 */
		free(hc);
		return false;
	}
	hc->server = server;
	hc->id = hook->id;
	hc->L = L;
	hc->co = co;
	/* 线程得留个引用（不然被 GC 掉），函数搬进它的栈里当参数。 */
	hc->co_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_xmove(L, co, 1);

	struct jval *env = jparse(json, NULL, 0);
	lua_newtable(co);
	if (env != NULL) {
		const char *fields[] = { "event", "time_ms", "data" };
		for (size_t i = 0; i < 3; i++) {
			const struct jval *v = jget(env, fields[i]);
			if (v != NULL) {
				json_to_lua(co, v);
				lua_setfield(co, -2, fields[i]);
			}
		}
	}
	jfree(env);
	/* 第二个参数：整条 JSON —— 以后事件加了字段，老配置也拿得到 */
	lua_pushstring(co, json);

	wl_list_insert(server->hook_calls.prev, &hc->link);
	hc->nargs = 2;
	hook_call_resume(hc); /* 里面有可能是同步跑完并 free(hc) */
	return true;
}

/* 加钩子：vidow.on(事件, 命令字符串 或 函数)，也认一张表（一次挂好几个）。
 * 返回 id（vidow.off 用）；给表就返回一串 id。 */
static int hook_add_one(lua_State *L, struct vidow_script *s, const char *event,
		int index) {
	if (lua_type(L, index) == LUA_TSTRING) {
		int id = vidow_hook_add_cmd(s->server, event,
			lua_tostring(L, index));
		if (id == 0) {
			return luaL_error(L, "vidow.on('%s', …)：命令是空的 / "
				"事件名太长", event);
		}
		lua_pushinteger(L, id);
		return 1;
	}
	if (lua_type(L, index) == LUA_TFUNCTION) {
		lua_pushvalue(L, index);
		int ref = luaL_ref(L, LUA_REGISTRYINDEX);
		int id = vidow_hook_add_lua(s->server, event, s->L, ref);
		if (id == 0) {
			luaL_unref(L, LUA_REGISTRYINDEX, ref);
			return luaL_error(L, "vidow.on('%s', …)：挂不上", event);
		}
		lua_pushinteger(L, id);
		return 1;
	}
	return luaL_error(L, "vidow.on('%s', …)：第二个参数得是命令字符串"
		"（\"notify-send $VIDOW_APP_ID\"）或者一个函数", event);
}

static int l_on(lua_State *L) {
	struct vidow_script *s = script_of(L);
	const char *event = luaL_checkstring(L, 1);
	if (lua_istable(L, 2)) {
		lua_newtable(L);
		int n = 0;
		for (lua_pushnil(L); lua_next(L, 2) != 0; lua_pop(L, 1)) {
			int idx = lua_absindex(L, -1);
			hook_add_one(L, s, event, idx);
			lua_rawseti(L, -3, ++n);
		}
		return 1;
	}
	return hook_add_one(L, s, event, 2);
}

static int l_off(lua_State *L) {
	struct vidow_script *s = script_of(L);
	lua_pushboolean(L, vidow_hook_remove(s->server,
		(int)luaL_checkinteger(L, 1)));
	return 1;
}

/* vidow.exec_once("waybar")：**整个 vidow 生命周期只起一次**（配置重载不重跑）——
 * Hyprland 的 exec-once。命令先排队，等这份配置跑完再撒出去（那时环境、缩放、
 * DISPLAY 都定了，见 vidow_config_sync）。认一个字符串，也认一串 / 一张表：
 *
 *   vidow.exec_once("waybar")
 *   vidow.exec_once{ "swaybg -i 壁纸.png", "mako" }
 *
 * 第二个参数可以是选项表，但这里只认「不等」这个默认：它是撒手跑的，`wait = true`
 * 和 `status = true` 都当场报错（要输出 / 要退出码就用 vidow.exec）。
 */
static void exec_once_queue(struct vidow_server *server, const char *cmd) {
	if (cmd == NULL || cmd[0] == '\0') {
		return;
	}
	struct vidow_exec_once *item = calloc(1, sizeof(*item));
	if (item == NULL) {
		return;
	}
	item->cmd = strdup(cmd);
	if (item->cmd == NULL) {
		free(item);
		return;
	}
	wl_list_insert(server->exec_once.prev, &item->link);
}

static int l_exec_once(lua_State *L) {
	struct vidow_script *s = script_of(L);
	int n = lua_gettop(L);
	/* 第二项是表 = 选项（以前第二项只能是命令字符串，所以不冲突）。 */
	bool opts_at2 = n >= 2 && lua_istable(L, 2);
	bool wait, status;
	exec_opts(L, opts_at2 ? 2 : 0, false, &wait, &status);
	if (wait || status) {
		return luaL_error(L, "vidow.exec_once 是撒手跑的（等这份配置跑完才起）："
			"等不了、也没有输出和退出码 —— 要等就用 vidow.exec(cmd)");
	}
	if (n == 1 && lua_istable(L, 1)) {
		lua_pushnil(L);
		while (lua_next(L, 1) != 0) {
			if (lua_isstring(L, -1)) {
				exec_once_queue(s->server, lua_tostring(L, -1));
			}
			lua_pop(L, 1);
		}
		lua_pushboolean(L, 1);
		return 1;
	}
	for (int i = 1; i <= n; i++) {
		if (i == 2 && opts_at2) {
			continue;
		}
		exec_once_queue(s->server, luaL_checkstring(L, i));
	}
	lua_pushboolean(L, 1);
	return 1;
}

/* 环境变量：vidow 起的子进程（exec / 事件钩子 / exec_once）继承的就是它。
 *
 *   vidow.env("GDK_SCALE")                 -- 读
 *   vidow.env("MOZ_ENABLE_WAYLAND", "1")   -- 写（顺手推给 systemd）
 *   vidow.env("FOO", false)                -- 删
 *   vidow.env{ A = "1", B = false }        -- 一次一串
 *
 * 改过的变量记着原值，vidow 退出时还原（见 env.c）。 */
static int l_env(lua_State *L) {
	struct vidow_script *s = script_of(L);
	if (lua_istable(L, 1)) {
		lua_pushnil(L);
		while (lua_next(L, 1) != 0) {
			int vi = lua_absindex(L, -1);
			char err[192] = "";
			const char *name = lua_tostring(L, -2);
			if (name == NULL) {
				return luaL_error(L, "vidow.env{…}: 变量名得是字符串");
			}
			bool ok;
			const char *value = lua_tostring(L, vi);
			if (value == NULL) {
				ok = vidow_env_unset(s->server, name, err, sizeof(err));
			} else {
				ok = vidow_env_set(s->server, name, value, err, sizeof(err));
			}
			if (!ok) {
				return luaL_error(L, "%s", err);
			}
			lua_pop(L, 1);
		}
		lua_pushboolean(L, 1);
		return 1;
	}

	const char *name = luaL_checkstring(L, 1);
	if (lua_gettop(L) == 1) {
		const char *value = getenv(name);
		if (value != NULL) {
			lua_pushstring(L, value);
		} else {
			lua_pushnil(L);
		}
		return 1;
	}
	char err[192] = "";
	bool ok;
	if (lua_isboolean(L, 2) && !lua_toboolean(L, 2)) {
		ok = vidow_env_unset(s->server, name, err, sizeof(err));
	} else {
		ok = vidow_env_set(s->server, name, luaL_checkstring(L, 2), err,
			sizeof(err));
	}
	if (!ok) {
		return luaL_error(L, "%s", err);
	}
	lua_pushboolean(L, 1);
	return 1;
}

/* vidow.env_export()：立刻把环境推给 systemd / dbus（平时是自动的、下一次 idle
 * 里推；想在起某个 systemd 服务之前确保它已经推过去了，就显式调一下）。 */
static int l_env_export(lua_State *L) {
	vidow_env_export_now(script_of(L)->server);
	lua_pushboolean(L, 1);
	return 1;
}

/* ------------------------------------------------------------ 工作区 / 键位 */

static int l_bind(lua_State *L) {
	struct vidow_script *s = script_of(L);
	const char *trigger = luaL_checkstring(L, 1);
	const char *action = luaL_checkstring(L, 2);
	/* 第三个参数（可选）：{ inhibited = true } = 焦点那个表面抑制着快捷键时
	 * 这条绑定照样生效（逃生口，见 keybind.h / keyboardgrab.c）。 */
	bool inhibited = false;
	/* { on = "release" } = 松开才触发（默认按下触发；单独一个修饰键的绑定默认
	 * 就是松开触发，见 keybind.c）。 */
	bool on_release = false;
	if (lua_istable(L, 3)) {
		lua_getfield(L, 3, "inhibited");
		inhibited = lua_toboolean(L, -1) != 0;
		lua_pop(L, 1);
		lua_getfield(L, 3, "on");
		if (lua_isstring(L, -1)) {
			const char *on = lua_tostring(L, -1);
			if (strcmp(on, "release") == 0 || strcmp(on, "up") == 0) {
				on_release = true;
			} else if (strcmp(on, "press") == 0 ||
					strcmp(on, "down") == 0) {
				on_release = false;
			} else {
				lua_pop(L, 1);
				return luaL_error(L, "vidow.bind 的 on 只能是 "
					"\"press\"（默认）或 \"release\"，给的是 '%s'",
					on);
			}
		}
		lua_pop(L, 1);
	}
	char err[160];
	if (vidow_keybind_add(s->server, trigger, action, inhibited, on_release,
			err, sizeof(err)) == NULL) {
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

/* vidow.rule{ … }：一条窗口规则 / 图层规则。字段表在 rule.c 的
 * vidow_rule_fields[]（匹配 + 作用一起），这里只负责「从 Lua 表里按类型取值」。
 *
 * 字符串字段 = POSIX 扩展正则（不锚定、大小写不敏感），前面加 ! 取反；literal =
 * true 就整条规则退回子串匹配。 */
static int l_rule(lua_State *L) {
	struct vidow_script *s = script_of(L);
	if (!lua_istable(L, 1)) {
		return luaL_error(L, "vidow.rule 要一个表，比如 "
			"vidow.rule{ app_id = \"mpv\", force_render = true }");
	}
	char err_buf[256];
	struct vidow_rule *rule = vidow_rule_new();
	if (rule == NULL) {
		return luaL_error(L, "vidow.rule: 内存不够了");
	}
	/* 先扫一遍 literal：它管的是所有字符串字段的匹配方式，得在写字段之前知道。 */
	lua_getfield(L, 1, "literal");
	if (!lua_isnil(L, -1)) {
		vidow_rule_set_literal(rule, lua_toboolean(L, -1) != 0);
	}
	lua_pop(L, 1);

	lua_pushnil(L); /* 第一个 key */
	while (lua_next(L, 1) != 0) {
		const char *key = lua_tostring(L, -2);
		const struct vidow_rule_field *field =
			key != NULL ? vidow_rule_field_find(key) : NULL;
		if (key == NULL) {
			vidow_rule_discard(rule);
			return luaL_error(L, "vidow.rule 的键只能是字符串");
		}
		if (strcmp(key, "literal") == 0) {
			lua_pop(L, 1);
			continue;
		}
		if (field == NULL) {
			vidow_rule_discard(rule);
			return luaL_error(L, "vidow.rule 没有 %s 这个字段。能写的：%s",
				key, vidow_rule_all_names(0));
		}
		char err[256];
		err[0] = '\0';
		bool ok = true;
		switch (field->kind) {
		case RK_STR: {
			const char *text = lua_type(L, -1) == LUA_TSTRING ?
				lua_tostring(L, -1) : NULL;
			if (text == NULL) {
				ok = false;
				snprintf(err, sizeof(err), "要一个字符串（正则）");
			} else {
				ok = vidow_rule_set_str(rule, field->slot, text, err,
					sizeof(err));
			}
			break;
		}
		case RK_BOOL:
		case RK_ACT_BOOL: {
			if (lua_isnil(L, -1)) {
				ok = false;
				snprintf(err, sizeof(err), "要 true 或 false");
			} else {
				bool value = lua_toboolean(L, -1) != 0;
				if (field->kind == RK_BOOL) {
					ok = vidow_rule_set_bool(rule, field->slot, value);
				} else {
					ok = vidow_rule_set_act_bool(rule, field->slot,
						value);
				}
			}
			break;
		}
		case RK_NUM:
		case RK_ACT_INT: {
			if (!lua_isnumber(L, -1)) {
				ok = false;
				snprintf(err, sizeof(err), "要一个整数");
			} else if (field->kind == RK_NUM) {
				ok = vidow_rule_set_num(rule, field->slot,
					(long long)lua_tointeger(L, -1));
			} else {
				ok = vidow_rule_set_act_int(rule, field->slot,
					(long long)lua_tointeger(L, -1));
			}
			break;
		}
		case RK_ACT_DOUBLE: {
			if (!lua_isnumber(L, -1)) {
				ok = false;
				snprintf(err, sizeof(err), "要一个数");
			} else {
				ok = vidow_rule_set_act_double(rule, field->slot,
					lua_tonumber(L, -1));
			}
			break;
		}
		case RK_ACT_STR: {
			const char *text = lua_type(L, -1) == LUA_TSTRING ?
				lua_tostring(L, -1) : NULL;
			if (text == NULL) {
				ok = false;
				snprintf(err, sizeof(err), "要一个字符串");
			} else {
				ok = vidow_rule_set_act_str(rule, field->slot, text, err,
					sizeof(err));
			}
			break;
		}
		case RK_ACT_COLOR: {
			const char *text = lua_type(L, -1) == LUA_TSTRING ?
				lua_tostring(L, -1) : NULL;
			if (text == NULL) {
				ok = false;
				snprintf(err, sizeof(err),
					"要一个颜色串，比如 \"ff0000\"");
			} else {
				ok = vidow_rule_set_act_color(rule, field->slot, text,
					err, sizeof(err));
			}
			break;
		}
		case RK_ACT_CAPTURE: {
			const char *text = lua_type(L, -1) == LUA_TSTRING ?
				lua_tostring(L, -1) : NULL;
			if (text == NULL) {
				ok = false;
				snprintf(err, sizeof(err), "要 \"off\" / \"hide\" / "
					"\"mosaic\"");
			} else {
				ok = vidow_rule_set_capture(rule, field->slot, text, err,
					sizeof(err));
			}
			break;
		}
		default:
			ok = false;
			snprintf(err, sizeof(err), "不认识的字段类型");
			break;
		}
		lua_pop(L, 1); /* 值（key 还留着给 lua_next） */
		if (!ok) {
			vidow_rule_discard(rule);
			return luaL_error(L, "vidow.rule 的 %s：%s", key, err);
		}
	}

	if (!vidow_rule_commit(s->server, rule, err_buf, sizeof(err_buf))) {
		vidow_rule_discard(rule);
		return luaL_error(L, "vidow.rule: %s", err_buf);
	}
	lua_pushboolean(L, 1);
	return 1;
}

/* 一个字段的值 → Lua 栈（没写的字段推 nil：「这条规则没管它」和「管了、给的是
 * 默认值」是两回事）。 */
static void push_rule_field(lua_State *L, const struct vidow_rule *rule,
		const struct vidow_rule_field *field) {
	if (!vidow_rule_has_field(rule, field)) {
		lua_pushnil(L);
		return;
	}
	switch (field->kind) {
	case RK_STR: {
		/* 原样推回去（带取反的 !），写进去什么样读出来什么样。 */
		const struct vidow_rule_match *m = &rule->str[field->slot];
		if (m->negate) {
			char buf[VIDOW_RULE_PAT + 1];
			snprintf(buf, sizeof(buf), "!%s", m->pat);
			lua_pushstring(L, buf);
		} else {
			lua_pushstring(L, m->pat);
		}
		break;
	}
	case RK_BOOL:
		lua_pushboolean(L, rule->bval[field->slot]);
		break;
	case RK_NUM:
		lua_pushinteger(L, (lua_Integer)rule->nval[field->slot]);
		break;
	case RK_ACT_BOOL:
		lua_pushboolean(L, rule->aval[field->slot].b);
		break;
	case RK_ACT_INT:
		lua_pushinteger(L, (lua_Integer)rule->aval[field->slot].i);
		break;
	case RK_ACT_DOUBLE:
		lua_pushnumber(L, rule->aval[field->slot].d);
		break;
	case RK_ACT_STR:
		lua_pushstring(L, rule->aval[field->slot].s);
		break;
	case RK_ACT_COLOR: {
		const float *c = rule->aval[field->slot].color;
		char buf[16];
		snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x",
			(int)(c[0] * 255.0f + 0.5f), (int)(c[1] * 255.0f + 0.5f),
			(int)(c[2] * 255.0f + 0.5f), (int)(c[3] * 255.0f + 0.5f));
		lua_pushstring(L, buf);
		break;
	}
	case RK_ACT_CAPTURE:
		lua_pushstring(L, rule->aval[field->slot].i == VIDOW_CAPTURE_HIDE ?
			"hide" : rule->aval[field->slot].i == VIDOW_CAPTURE_MOSAIC ?
			"mosaic" : "off");
		break;
	default:
		lua_pushnil(L);
		break;
	}
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
		if (rule->literal) {
			lua_pushboolean(L, 1);
			lua_setfield(L, -2, "literal");
		}
		for (size_t j = 0; j < vidow_rule_field_count; j++) {
			const struct vidow_rule_field *field = &vidow_rule_fields[j];
			if (!vidow_rule_has_field(rule, field)) {
				continue;
			}
			push_rule_field(L, rule, field);
			lua_setfield(L, -2, field->name);
		}
		lua_settable(L, -3);
	}
	return 1;
}

/* vidow.output{ name = "DP-3", scale = 2, x = 0, y = 0, mode = "3840x2160@144" }
 *
 * 一块屏一组：这块屏的缩放、摆在布局的哪个位置、上什么模式、开不开 VRR、
 * 几位色、什么方向、开不开 —— 全是它的。没写的字段 = 不管这一项。
 *
 * 见 outputconf.c：为什么不是一堆平铺键。 */
static int l_output(lua_State *L) {
	struct vidow_script *s = script_of(L);
	if (!lua_istable(L, 1)) {
		return luaL_error(L, "vidow.output 要一个表，比如 "
			"vidow.output{ name = \"DP-3\", scale = 2, x = 0, y = 0 }");
	}
	struct vidow_output_spec spec = { 0 };
	spec.name = table_string(L, 1, "name");

	char scale_text[32];
	lua_getfield(L, 1, "scale");
	if (!lua_isnil(L, -1)) {
		struct vidow_scale_spec parsed;
		if (!value_as_text(L, -1, scale_text, sizeof(scale_text)) ||
				!vidow_scale_parse(scale_text, &parsed)) {
			lua_pop(L, 1);
			return luaL_error(L, "vidow.output{ name = \"%s\" }: scale 认不出来"
				"（写 auto / off / 一个正数，比如 2）",
				spec.name != NULL ? spec.name : "?");
		}
		spec.has_scale = true;
		spec.scale = scale_text;
	}
	lua_pop(L, 1);

	const char *who = spec.name != NULL ? spec.name : "?";

	/* 位置：x / y 写哪个算哪个（只写 x = 0 就是摆在最左边那一行的 0 位）。 */
	const struct { const char *key; int *slot; } coords[] = {
		{ "x", &spec.x }, { "y", &spec.y },
	};
	for (size_t i = 0; i < sizeof(coords) / sizeof(coords[0]); i++) {
		lua_getfield(L, 1, coords[i].key);
		if (!lua_isnil(L, -1)) {
			if (!lua_isnumber(L, -1)) {
				lua_pop(L, 1);
				return luaL_error(L, "vidow.output{ name = \"%s\" }: %s 得是"
					"个数（逻辑坐标，整数）", who, coords[i].key);
			}
			*coords[i].slot = (int)lua_tointeger(L, -1);
			spec.has_pos = true;
		}
		lua_pop(L, 1);
	}

	/* 模式："3840x2160@144" / "3840x2160"（刷新率不挑） */
	lua_getfield(L, 1, "mode");
	if (!lua_isnil(L, -1)) {
		const char *text = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
		if (text == NULL || !vidow_output_parse_mode(text, &spec.mode_w,
				&spec.mode_h, &spec.mode_hz)) {
			lua_pop(L, 1);
			return luaL_error(L, "vidow.output{ name = \"%s\" }: mode 认不出来"
				"（写 \"3840x2160@144\"，@刷新率可省）", who);
		}
		spec.has_mode = true;
	}
	lua_pop(L, 1);

	/* 开关屏 */
	lua_getfield(L, 1, "enabled");
	if (!lua_isnil(L, -1)) {
		spec.has_enabled = true;
		spec.enabled = lua_toboolean(L, -1) != 0;
	}
	lua_pop(L, 1);

	/* VRR（adaptive sync / 可变刷新率） */
	lua_getfield(L, 1, "vrr");
	if (!lua_isnil(L, -1)) {
		spec.has_vrr = true;
		spec.vrr = lua_toboolean(L, -1) != 0;
	}
	lua_pop(L, 1);

	/* 位深：8 / 10（10 位要显卡 + 显示器 + 渲染器都支持） */
	lua_getfield(L, 1, "depth");
	if (!lua_isnil(L, -1)) {
		if (!lua_isnumber(L, -1) || (lua_tointeger(L, -1) != 8 &&
				lua_tointeger(L, -1) != 10)) {
			const char *shown = luaL_tolstring(L, -1, NULL); /* 数字也能打出来 */
			char text[32];
			snprintf(text, sizeof(text), "%s", shown != NULL ? shown : "?");
			lua_pop(L, 2); /* tolstring 压了一个值进去 */
			return luaL_error(L, "vidow.output{ name = \"%s\" }: depth 只能是 "
				"8 或 10（现在写的是 %s）", who, text);
		}
		spec.has_depth = true;
		spec.depth = (int)lua_tointeger(L, -1);
	}
	lua_pop(L, 1);

	/* 方向 / 翻转 */
	lua_getfield(L, 1, "transform");
	if (!lua_isnil(L, -1)) {
		const char *text = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
		if (text == NULL || !vidow_output_parse_transform(text,
				&spec.transform)) {
			lua_pop(L, 1);
			return luaL_error(L, "vidow.output{ name = \"%s\" }: transform 认不出来"
				"（normal / 90 / 180 / 270 / flipped / flipped-90 / flipped-180 /"
				" flipped-270）", who);
		}
		spec.has_transform = true;
	}
	lua_pop(L, 1);

	/* 项名写错（scalee / pos）不该悄悄不生效：列一遍能写什么。 */
	static const char *const fields[] = {
		"name", "scale", "x", "y", "mode", "enabled", "vrr", "depth",
		"transform",
	};
	lua_pushnil(L);
	while (lua_next(L, 1) != 0) {
		const char *field = lua_type(L, -2) == LUA_TSTRING ?
			lua_tostring(L, -2) : NULL;
		bool known = false;
		for (size_t i = 0; field != NULL && i < sizeof(fields) / sizeof(fields[0]);
				i++) {
			if (strcmp(field, fields[i]) == 0) {
				known = true;
				break;
			}
		}
		if (!known) {
			lua_pop(L, 2);
			return luaL_error(L, "vidow.output{…}: 没有 '%s' 这一项"
				"（能写：name / scale / x / y / mode / enabled / vrr / depth / "
				"transform）", field != NULL ? field : "?");
		}
		lua_pop(L, 1);
	}

	char err[160];
	if (vidow_output_conf_add(s->server, &spec, err, sizeof(err)) == NULL) {
		return luaL_error(L, "%s", err);
	}
	lua_pushboolean(L, 1);
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

/* ---------------------------------------------------------------- include
 *
 * vidow.include("configs/01-gaps.lua") —— 把另一份配置读进来，就地展开，等价于
 * 把那个文件的内容抄在这里。配置按类别拆成好几个文件之后，入口 config.lua 就
 * 只剩这些 include 行。
 *
 * 相对路径按**正在 include 的那个文件所在目录**解析（像 C 的 #include "…"），
 * 所以入口里写 "configs/01-gaps.lua"、configs 里的文件互相引用写 "02-decor.lua"，
 * 两边都对；绝对路径照用。$VIDOW_CONFIG 把入口指到别处时这条规则也跟着走。
 *
 * 为什么调用动作交给一段 Lua（见 include_lua）：include 进来的那段可能调
 * vidow.exec()，而 vidow.exec() 是 yield 出来的（整份配置跑在一个 coroutine
 * 里）。从 C 里 lua_call 出去的调用不能 yield（"attempt to yield across a
 * C-call boundary"），从 Lua 里调就没这个问题 —— 所以 C 侧只留一个「把文件读
 * 成函数」的助手，调不调、什么时候调，由下面那段 Lua 决定。 */

/* 相对路径 → 绝对路径。from 是 debug.getinfo(2).source，形如 "@/path/config.lua"；
 * 不是文件（[string "…"] / =stdin）时退回配置目录。 */
static void include_path(char *buf, size_t size, const char *name,
		const char *from) {
	if (name[0] == '/') {
		snprintf(buf, size, "%s", name);
		return;
	}
	char dir[PATH_MAX] = "";
	if (from != NULL && from[0] == '@') {
		snprintf(dir, sizeof(dir), "%s", from + 1);
		char *slash = strrchr(dir, '/');
		if (slash != NULL) {
			*slash = '\0';
		} else {
			dir[0] = '\0';
		}
	}
	if (dir[0] == '\0') {
		vidow_config_dir(dir, sizeof(dir));
	}
	snprintf(buf, size, "%s/%s", dir, name);
}

/* include 的助手：把文件读成一个函数（不调用）。成功返回那个函数，失败返回
 * nil + 一句人话（哪个路径、为什么）。 */
static int l_include_load(lua_State *L) {
	struct vidow_script *s = script_of(L);
	const char *name = luaL_checkstring(L, 1);
	const char *from = luaL_optstring(L, 2, NULL);

	char path[PATH_MAX];
	include_path(path, sizeof(path), name, from);

	/* 登记给 configwatch（它盯的是「配置真正读了哪些文件」，不是配置目录里有
	 * 什么）：改了这份就自动重载。**读不成也要登记** —— 文件不在的时候登记的
	 * 是「这个路径 + 它所在的目录」，等文件补上（或改对）了才知道要重载。 */
	vidow_configwatch_add(s->server, path);

	/* 先自己开一下：文件不在和语法写错是两回事，报错得分得清。 */
	FILE *probe = fopen(path, "r");
	if (probe == NULL) {
		lua_pushnil(L);
		lua_pushfstring(L, "vidow.include: 打不开 %s：%s", path,
			strerror(errno));
		return 2;
	}
	fclose(probe);

	if (luaL_loadfile(L, path) != LUA_OK) {
		const char *why = lua_tostring(L, -1);
		char msg[PATH_MAX + 64];
		snprintf(msg, sizeof(msg), "vidow.include: %s",
			why != NULL ? why : "读不了");
		lua_settop(L, 1); /* 丢掉 loadfile 那条，留我们自己这条 */
		lua_pushnil(L);
		lua_pushstring(L, msg);
		return 2;
	}

	wlr_log(WLR_INFO, "config.lua: include %s", path);
	return 1;
}

/* vidow.include 的实现：调 l_include_load 拿函数，再调用它。
 *
 * error(…, 2) 是故意的：这一层是 C 的助手和配置之间的一层薄壳，报错要算在
 * 配置里写 include 的那一行上（level 2），不是算在这段内置代码上。 */
static const char include_lua[] =
	"local load, getinfo = ..., debug.getinfo\n"
	"local depth = 0\n"
	"return function(name)\n"
	"	if type(name) ~= \"string\" or name == \"\" then\n"
	"		error(\"vidow.include: 要一个文件名\", 2)\n"
	"	end\n"
	"	depth = depth + 1\n"
	"	if depth > 32 then\n"
	"		error(\"vidow.include: include 套得太深了（是不是自己 include 自己？）\", 2)\n"
	"	end\n"
	"	local chunk, err = load(name, getinfo(2, \"S\").source)\n"
	"	if chunk == nil then\n"
	"		error(err, 2)\n"
	"	end\n"
	"	chunk()\n"
	"	depth = depth - 1\n"
	"end\n";

/* vidow 表上「合并掉的那两个名字」的报错（挂在 __index 上，见 register_api）。
 * 现在只有两个入口：vidow.exec（跑，能等 / 能要退出码 / 能撒手）和 vidow.exec_once
 * （一个生命周期只起一次）。 */
static int l_api_index(lua_State *L) {
	const char *key = lua_tostring(L, 2);
	if (key == NULL) {
		lua_pushnil(L);
		return 1;
	}
	if (strcmp(key, "spawn") == 0) {
		return luaL_error(L, "vidow.spawn 已经并进 vidow.exec："
			"写成 vidow.exec(cmd, { wait = false })（撒手跑，不等、不要输出）");
	}
	if (strcmp(key, "exec_status") == 0) {
		return luaL_error(L, "vidow.exec_status 已经并进 vidow.exec："
			"写成 vidow.exec(cmd, { status = true })（返回 out, code, err）");
	}
	lua_pushnil(L);
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
		{ "exec_once", l_exec_once },
		{ "env", l_env },
		{ "env_export", l_env_export },
		{ "on", l_on },
		{ "off", l_off },
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
		{ "output", l_output },
	};
	for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
		lua_pushlightuserdata(L, s);
		lua_pushcclosure(L, fns[i].fn, 1);
		lua_setfield(L, -2, fns[i].name);
	}
	lua_pushstring(L, "vidow 0.1 (lua " LUA_VERSION ")");
	lua_setfield(L, -2, "version");

	/* vidow.include()：C 侧的助手 + 上面那段 Lua（为什么要分两层见注释）。 */
	if (luaL_loadstring(L, include_lua) == LUA_OK) {
		lua_pushlightuserdata(L, s);
		lua_pushcclosure(L, l_include_load, 1);
		if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
			lua_setfield(L, -2, "include");
		} else {
			wlr_log(WLR_ERROR, "vidow.include 起不来：%s",
				lua_tostring(L, -1) != NULL ? lua_tostring(L, -1) : "?");
			lua_pop(L, 1);
		}
	} else {
		wlr_log(WLR_ERROR, "vidow.include 起不来：%s",
			lua_tostring(L, -1) != NULL ? lua_tostring(L, -1) : "?");
		lua_pop(L, 1);
	}
	lua_setglobal(L, "vidow");

	/* 合并掉的那两个名字：vidow 表上挂个 __index，只在这两个名字上给一句人话
	 * （别的还是 nil）。不挂的话报的是 "attempt to call a nil value" —— 看的人
	 * 只知道配置坏了，不知道改成什么。 */
	lua_getglobal(L, "vidow");
	lua_newtable(L);
	lua_pushlightuserdata(L, s);
	lua_pushcclosure(L, l_api_index, 1);
	lua_setfield(L, -2, "__index");
	lua_setmetatable(L, -2);
	lua_pop(L, 1);

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
/* vidow.exec_once() 排队的命令：这份配置跑完了才撒出去。
 *
 * 为什么不在配置跑的那一刻就起：配置是分步跑的，命令起来的时候缩放 / DISPLAY /
 * 用户 vidow.env{} 设的变量可能还没落地（配置里 exec_once 写在前面的情况）。
 * 等这份配置跑完再起，子进程看到的是「最终环境」。
 *
 * 「已经起过」按**命令原文**记（不是按行号 / 下标）：配置重载之后同一行还是同一行，
 * 认得出是它；改过命令原文就是一条新命令，会再起一次（和 Hyprland 的 exec-once
 * 一个规矩）。空闲（配置里删掉了那条）的命令下次重载会被忘掉。 */
#define EXEC_ONCE_RAN_MAX 128

static bool exec_once_already_ran(const struct vidow_server *server,
		const char *cmd) {
	for (size_t i = 0; i < server->exec_once_ran_n; i++) {
		if (strcmp(server->exec_once_ran[i], cmd) == 0) {
			return true;
		}
	}
	return false;
}

static void exec_once_note_ran(struct vidow_server *server, const char *cmd) {
	if (server->exec_once_ran_n >= EXEC_ONCE_RAN_MAX) {
		return; /* 到上限就不再记账：宁可多起一次，也别吃掉刚记的 */
	}
	char **p = realloc(server->exec_once_ran,
		(server->exec_once_ran_n + 1) * sizeof(*p));
	if (p == NULL) {
		return;
	}
	server->exec_once_ran = p;
	server->exec_once_ran[server->exec_once_ran_n] = strdup(cmd);
	if (server->exec_once_ran[server->exec_once_ran_n] == NULL) {
		return;
	}
	server->exec_once_ran_n++;
}

/* 把这一轮配置里声明的 exec_once 落地：没起过的起掉，起过的（含上一轮的）跳过；
 * 最后把「配置里已经删掉的那些」从名单里忘掉 —— 不然删删改改攒到 128 条上限之后
 * 就再也记不住新命令了（那样会每重载一次起一次，最难受的那种）。 */
static void exec_once_flush(struct vidow_server *server) {
	/* 这一轮声明了哪些（下面「忘掉」按它判）。必须自己拷一份：下面起完就把队列项
	 * 连同命令原文 free 了，指着它们比 = 读已释放的内存（tcache 的 next/key 正好
	 * 盖在字符串头上，于是刚起的命令当场被判成「配置里没了」，重载就又起一遍 ——
	 * 恰好是这个功能要防的那件事）。 */
	char *declared[EXEC_ONCE_RAN_MAX];
	size_t ndeclared = 0;
	bool declared_complete = true;
	struct vidow_exec_once *scan;
	wl_list_for_each(scan, &server->exec_once, link) {
		if (ndeclared == EXEC_ONCE_RAN_MAX) {
			/* 声明的比名单记得下的还多：这一轮干脆什么都不忘 */
			declared_complete = false;
			break;
		}
		declared[ndeclared] = strdup(scan->cmd);
		if (declared[ndeclared] == NULL) {
			declared_complete = false;
			break;
		}
		ndeclared++;
	}

	struct vidow_exec_once *item, *tmp;
	wl_list_for_each_safe(item, tmp, &server->exec_once, link) {
		wl_list_remove(&item->link);
		if (exec_once_already_ran(server, item->cmd)) {
			wlr_log(WLR_DEBUG, "exec-once：%s 起过了，跳过", item->cmd);
			free(item->cmd);
			free(item);
			continue;
		}
		/* 和配置里起的其它子进程一样：先对一次环境（缩放 / DISPLAY / 输出） */
		vidow_xwayland_scale_apply(server);
		vidow_portal_desktop_env(&server->config);
		wlr_log(WLR_INFO, "exec-once：起 %s", item->cmd);
		vidow_exec_detached(item->cmd);
		exec_once_note_ran(server, item->cmd);
		free(item->cmd);
		free(item);
	}

	if (declared_complete) {
		for (size_t i = 0; i < server->exec_once_ran_n;) {
			bool still = false;
			for (size_t k = 0; k < ndeclared; k++) {
				if (strcmp(server->exec_once_ran[i], declared[k]) ==
					0) {
					still = true;
					break;
				}
			}
			if (still) {
				i++;
				continue;
			}
			wlr_log(WLR_DEBUG, "exec-once：%s 配置里没了，忘掉它",
				server->exec_once_ran[i]);
			free(server->exec_once_ran[i]);
			memmove(&server->exec_once_ran[i],
				&server->exec_once_ran[i + 1],
				(server->exec_once_ran_n - i - 1) *
					sizeof(*server->exec_once_ran));
			server->exec_once_ran_n--;
		}
	}
	for (size_t k = 0; k < ndeclared; k++) {
		free(declared[k]);
	}
}

void vidow_config_sync(struct vidow_server *server) {
	vidow_ipc_apply_config(server);
	vidow_xwayland_apply_config(server);
	/* 配置可能改了 scale / xwayland_scale，或者给某块屏写了新的一组
	 * vidow.output{…}：改到的输出立刻换缩放，XWayland 的全局缩放也跟着重算。
	 * 布局位置同理。 */
	vidow_output_apply_scale_all(server);
	vidow_output_apply_positions_all(server);
	vidow_output_apply_state_all(server);
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
	/* Alt+Tab 切换器正开着的话：排版 / 外观那些键（mru = {…}）改完立刻重排，
	 * 不用等下次打开。 */
	vidow_mru_update(server);
	/* 环境（systemd_env / systemd_env_export）可能被改过：推给 systemd 的排队
	 * 走一轮（多次改动只 fork 一次）。 */
	vidow_env_apply_config(server);
	/* vidow.exec_once() 排队的命令：这份配置到此为止算跑完了，撒出去。 */
	exec_once_flush(server);
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
		vidow_output_schedule_frame(server, output->wlr_output,
			"脚本/配置改了状态");
	}
}

static void script_done(struct vidow_script *s, const char *why) {
	if (s->finished) {
		return;
	}
	s->finished = true;
	if (why != NULL) {
		wlr_log(WLR_ERROR, "config.lua: %s", why);
		/* 事件：配置没跑成（写错了 / vidow.exec 里出的错）。状态栏 / 通知脚本
		 * 靠它报出来；配置文件里挂的钩子这时候已经作废了，所以只有 IPC
		 * 订阅者收得到 —— 这份配置自己声明的钩子当然也包括在内。 */
		const struct vidow_event_field fields[] = {
			VIDOW_EV_STR("error", why),
			VIDOW_EV_END,
		};
		vidow_ipc_emit_simple(s->server, "config-failed", fields);
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
		/* { status = true }：永远三个值，out 不会是 nil。 */
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
	/* 钩子引用的是这个 Lua 状态里的函数：状态关掉之前先把手里的引用还回去
	 * （vidow.on 声明的钩子跟着配置走 —— 重载 = 重新声明一遍）。 */
	vidow_hooks_clear(server);
	vidow_exec_cancel_all(server); /* drops the callbacks with the jobs */
	hook_calls_abort(server);      /* 在等命令的那些钩子：状态要没了，扔掉 */
	if (s->L != NULL) {
		lua_close(s->L);
	}
	free(s);
}

bool vidow_script_load(struct vidow_server *server) {
	vidow_script_finish(server);
	test_actions_clear(server);
	/* 重载 = 重新声明一遍键位、窗口规则和每块屏的配置：清掉旧的，免得 SIGHUP
	 * 之后越攒越多（删掉的 vidow.output 行要真的消失）。 */
	vidow_keybind_clear(server);
	vidow_rule_clear(server);
	vidow_output_conf_clear(server);
	/* 重载 = 重新读一遍配置：先回到默认值（再压上环境变量）。配置文件里删掉的
	 * 行才会真的消失 —— 不然改完配置重载，被删的键还留着旧值。 */
	vidow_config_reset(&server->config);
	/* 上一轮 include 过哪些文件不算数了：这一轮读到哪些就重新登记哪些，改哪份
	 * 都自动重载（见 configwatch.c）。 */
	vidow_configwatch_begin(server);

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
	wl_list_init(&server->hook_calls);
	server->reload_sig = wl_event_loop_add_signal(
		wl_display_get_event_loop(server->display), SIGHUP,
		script_reload_signal, server);
	if (server->reload_sig == NULL) {
		wlr_log(WLR_ERROR, "failed to install the SIGHUP reload handler");
		return false;
	}
	return true;
}
