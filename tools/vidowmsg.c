/* vidowmsg —— 跟 vidow 的 IPC socket 说话的命令行客户端（一行一个 JSON）。
 *
 *   vidowmsg state                 # 全量状态（窗口 / 工作区 / 输出 / layer / 配置…）
 *   vidowmsg windows               # 只要窗口列表
 *   vidowmsg outputs
 *   vidowmsg layers
 *   vidowmsg focused
 *   vidowmsg pick-window 640 360   # 这个坐标上是哪个窗口
 *   vidowmsg focus-workspace 2
 *   vidowmsg toggle-floating --app-id mpv
 *   vidowmsg set-window-size 800 600 --title 演示
 *   vidowmsg output-power DP-1 off
 *   vidowmsg output-scale DP-1 1.5
 *   vidowmsg action "workspace 2"
 *   vidowmsg set-config window_open_ms 60
 *   vidowmsg reload-config
 *   vidowmsg watch                 # 订阅事件，一行一条打出来（Ctrl-C 退出）
 *   vidowmsg --json '{"cmd":"windows"}'
 *
 * 选项：--socket PATH（默认 $VIDOW_SOCKET / $XDG_RUNTIME_DIR/vidow-$WAYLAND_DISPLAY.sock）、
 *       --raw（不排版，原样打印）、--timeout SEC（默认 5）。
 * 退出码：0 = 成功，1 = 服务端说不行（错误打到 stderr），2 = 连不上 / 超时。
 *
 * 依赖只有 POSIX socket：这个文件不引 wayland、不引 json.c（它只负责排版缩进）。 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_ARGS 64
/* 一条应答的上限（vidow 那边一条请求是 1 MiB；全量 state 可能大一些）。 */
#define MSG_MAX (16u << 20)

struct arg {
	char key[64];
	const char *value;
};

static void usage(void) {
	fputs(
"用法：vidowmsg <命令> [参数…] [--选项]\n"
"\n"
"查询：state / version / focused / windows / columns / workspaces / outputs /\n"
"      layers / keybinds / rules / config [键] / seats / input / cursor /\n"
"      tokens / memories / anims / im / overview / xwayland / pick-window x y\n"
"窗口：focus-window / activate-window / close-window / toggle-floating /\n"
"      minimize-window / unminimize-window（不带参数 = 最近收起来的那个） /\n"
"      toggle-minimize / set-floating true|false / toggle-fullscreen /\n"
"      set-fullscreen true|false /\n"
"      move-window-to-workspace 名字 / move-window-to-special 名字 /\n"
"      move-window-to-output 输出 / set-window-size 宽 高 /\n"
"      set-window-position x y / center-window / set-window-force-render on|off /\n"
"      focus-window-direction -1|1 / move-window-direction -1|1\n"
"列：  focus-column -1|1 / move-column -1|1 / cycle-column-width /\n"
"      set-column-width 0.5 / expand-column-to-available-width / center-column /\n"
"      toggle-maximize-column / set-maximize-column true|false\n"
"工作区：focus-workspace 序号|名字|next|prev|previous / focus-workspace-previous /\n"
"      toggle-special [名字] / create-workspace [名字] /\n"
"      move-workspace-up|down [序号|名字] / set-workspace-name 名字 [序号|名字] /\n"
"      unset-workspace-name [序号|名字] / move-window-to-workspace-up|down /\n"
"      move-column-to-workspace 序号|名字 / move-workspace-to-output 工作区 输出\n"
"输出：output-power 输出 on|off|toggle / output-scale 输出 1.5|auto /\n"
"      output-transform 输出 normal|90|… / output-mode 输出 宽 高 [刷新率] /\n"
"      output-position 输出 x y / output-focus 输出\n"
"合成器：action \"工作区动作\" / set-config 键 值 / get-config 键 / reload-config /\n"
"      toggle-overview / move-cursor x y [--relative] / cursor-shake / quit\n"
"注入：move-cursor x y / pointer-button click|press|release [键] [连点数] /\n"
"      pointer-scroll dx dy / send-key [tap|press|release] 组合键 / type-text 文字 /\n"
"      （合成鼠标键盘事件，等于操控整个桌面；按键只给客户端，不触发 vidow 的\n"
"        快捷键。默认开着，见 config 的 input_injection）\n"
"订阅：watch [事件名…]\n"
"\n"
"指认目标：--id N / --app-id 名字 / --title 子串 / --output 输出名 / --workspace 名字\n"
"选项：--socket PATH --raw --timeout SEC --json '<JSON>' / --button 键 --count N\n", stderr);
}

/* ------------------------------------------------------------- 参数 */

static bool looks_number(const char *s) {
	char *end = NULL;
	strtod(s, &end);
	return end != NULL && end != s && *end == '\0';
}

static void json_escape(FILE *out, const char *s) {
	fputc('"', out);
	for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
		switch (*p) {
		case '"':
			fputs("\\\"", out);
			break;
		case '\\':
			fputs("\\\\", out);
			break;
		case '\n':
			fputs("\\n", out);
			break;
		case '\r':
			fputs("\\r", out);
			break;
		case '\t':
			fputs("\\t", out);
			break;
		default:
			if (*p < 0x20) {
				fprintf(out, "\\u%04x", *p);
			} else {
				fputc(*p, out);
			}
			break;
		}
	}
	fputc('"', out);
}

/* 值按它的样子写进 JSON：true/false、纯数字当数字，其余当字符串。 */
static void json_value(FILE *out, const char *v) {
	if (strcmp(v, "true") == 0 || strcmp(v, "false") == 0 ||
			looks_number(v)) {
		fputs(v, out);
		return;
	}
	json_escape(out, v);
}

/* data 例外：写 event-data 时给的是一段 JSON（{"hello":"world"}），原样搬进去 ——
 * 引号包起来的话对面收到的就只是一串文本，字段一个都取不到。 */
static void json_value_data(FILE *out, const char *v) {
	size_t n = strlen(v);
	while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t')) {
		n--;
	}
	if (n >= 2 && ((v[0] == '{' && v[n - 1] == '}') ||
			(v[0] == '[' && v[n - 1] == ']'))) {
		fwrite(v, 1, n, out);
		return;
	}
	json_value(out, v);
}

/* 一条命令的位置参数怎么摆进 JSON。 */
static const char *positional_key(const char *cmd, int index) {
	static const struct {
		const char *cmd;
		const char *keys[4];
	} maps[] = {
		{ "focus-workspace", { "workspace", NULL } },
		{ "create-workspace", { "name", NULL } },
		{ "move-window-to-workspace", { "workspace", NULL } },
		{ "move-window-to-special", { "name", NULL } },
		{ "move-window-to-output", { "output", NULL } },
		{ "move-workspace-to-output", { "workspace", "output", NULL } },
		{ "move-workspace-up", { "workspace", NULL } },
		{ "move-workspace-down", { "workspace", NULL } },
		{ "set-workspace-name", { "name", "workspace", NULL } },
		{ "unset-workspace-name", { "workspace", NULL } },
		{ "move-window-to-workspace-up", { NULL } },
		{ "move-window-to-workspace-down", { NULL } },
		{ "move-column-to-workspace", { "workspace", NULL } },
		{ "set-window-size", { "width", "height", NULL } },
		{ "set-window-position", { "x", "y", NULL } },
		{ "set-window-force-render", { "on", NULL } },
		{ "set-floating", { "floating", NULL } },
		{ "set-fullscreen", { "fullscreen", NULL } },
		{ "set-column-width", { "fraction", NULL } },
		{ "set-maximize-column", { "maximized", NULL } },
		{ "set-config", { "key", "value", NULL } },
		{ "get-config", { "key", NULL } },
		{ "config", { "key", NULL } },
		{ "emit", { "event", NULL } },
		{ "focus-column", { "direction", NULL } },
		{ "move-column", { "direction", NULL } },
		{ "focus-window-direction", { "direction", NULL } },
		{ "move-window-direction", { "direction", NULL } },
		{ "toggle-special", { "name", NULL } },
		{ "focus-special", { "name", NULL } },
		{ "output-power", { "output", "mode", NULL } },
		{ "output-enable", { "output", NULL } },
		{ "output-disable", { "output", NULL } },
		{ "output-toggle", { "output", NULL } },
		{ "output-scale", { "output", "scale", NULL } },
		{ "output-transform", { "output", "transform", NULL } },
		{ "output-mode", { "output", "width", "height", NULL } },
		{ "output-position", { "output", "x", "y", NULL } },
		{ "output-focus", { "output", NULL } },
		{ "pick-window", { "x", "y", NULL } },
		{ "move-cursor", { "x", "y", NULL } },
		{ "cursor-shake", { NULL } },
		{ "pointer-button", { "state", "button", "count", NULL } },
		{ "pointer-scroll", { "dx", "dy", NULL } },
		{ "send-key", { "state", "keys", NULL } },
		{ "type-text", { "text", NULL } },
	};
	for (size_t i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
		if (strcmp(maps[i].cmd, cmd) != 0) {
			continue;
		}
		for (int k = 0; k < 4; k++) {
			if (maps[i].keys[k] == NULL) {
				return NULL;
			}
			if (k == index) {
				return maps[i].keys[k];
			}
		}
		return NULL;
	}
	return NULL;
}

/* ------------------------------------------------------------- 排版 */

/* 把一段 JSON 排成给人看的样子（字符串里的东西一个字节都不动）。 */
static void pretty_print(const char *line, size_t len) {
	int depth = 0;
	bool in_string = false;
	bool escaped = false;
	const char *end = line + len;
	for (const char *p = line; p < end; p++) {
		char c = *p;
		if (in_string) {
			fputc(c, stdout);
			if (escaped) {
				escaped = false;
			} else if (c == '\\') {
				escaped = true;
			} else if (c == '"') {
				in_string = false;
			}
			continue;
		}
		switch (c) {
		case '"':
			in_string = true;
			fputc(c, stdout);
			break;
		case '{':
		case '[': {
			char close = c == '{' ? '}' : ']';
			if (p + 1 < end && p[1] == close) { /* 空容器：不换行 */
				fputs(close == '}' ? "{}" : "[]", stdout);
				p++;
				break;
			}
			fputc(c, stdout);
			depth++;
			fputc('\n', stdout);
			for (int i = 0; i < depth; i++) {
				fputs("  ", stdout);
			}
			break;
		}
		case '}':
		case ']':
			if (depth > 0) {
				depth--;
			}
			fputc('\n', stdout);
			for (int i = 0; i < depth; i++) {
				fputs("  ", stdout);
			}
			fputc(c, stdout);
			break;
		case ',':
			fputc(c, stdout);
			fputc('\n', stdout);
			for (int i = 0; i < depth; i++) {
				fputs("  ", stdout);
			}
			break;
		case ':':
			fputs(": ", stdout);
			break;
		default:
			fputc(c, stdout);
			break;
		}
	}
	fputc('\n', stdout);
}

/* ------------------------------------------------------------- socket */

static int connect_socket(const char *path) {
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		fprintf(stderr, "vidowmsg: socket(): %s\n", strerror(errno));
		return -1;
	}
	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		fprintf(stderr, "vidowmsg: 连不上 %s：%s\n", path, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

/* WAYLAND_DISPLAY 没设的时候：按 vidow-wayland-N.sock 的顺序试一遍（vidow 的
 * socket 名就是它自己的 WAYLAND_DISPLAY）。 */
static bool find_socket(char *buf, size_t len) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime == NULL || runtime[0] == '\0') {
		return false;
	}
	for (int i = 0; i < 16; i++) {
		char candidate[512];
		snprintf(candidate, sizeof(candidate), "%s/vidow-wayland-%d.sock",
			runtime, i);
		if (access(candidate, F_OK) == 0) {
			snprintf(buf, len, "%s", candidate);
			return true;
		}
	}
	return false;
}

static bool resolve_socket(char *buf, size_t len) {
	const char *explicit_path = getenv("VIDOW_SOCKET");
	if (explicit_path != NULL && explicit_path[0] != '\0') {
		snprintf(buf, len, "%s", explicit_path);
		return true;
	}
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *display = getenv("WAYLAND_DISPLAY");
	if (runtime != NULL && runtime[0] != '\0' && display != NULL &&
			display[0] != '\0') {
		snprintf(buf, len, "%s/vidow-%s.sock", runtime, display);
		return true;
	}
	return find_socket(buf, len);
}

int main(int argc, char *argv[]) {
	signal(SIGPIPE, SIG_IGN);

	const char *socket_path = NULL;
	const char *json_arg = NULL;
	bool raw = false;
	int timeout_sec = 5;
	bool watch = false;

	char sockbuf[512];
	struct arg args[MAX_ARGS];
	int n_args = 0;
	const char *positional[MAX_ARGS];
	int n_positional = 0;
	const char *watch_events[MAX_ARGS];
	int n_watch = 0;
	const char *cmd = NULL;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (strcmp(a, "--socket") == 0 && i + 1 < argc) {
			socket_path = argv[++i];
		} else if (strncmp(a, "--socket=", 9) == 0) {
			socket_path = a + 9;
		} else if (strcmp(a, "--json") == 0 && i + 1 < argc) {
			json_arg = argv[++i];
		} else if (strcmp(a, "--raw") == 0) {
			raw = true;
		} else if (strcmp(a, "--timeout") == 0 && i + 1 < argc) {
			timeout_sec = atoi(argv[++i]);
		} else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0 ||
				strcmp(a, "help") == 0) {
			usage();
			return 0;
		} else if (a[0] == '-' && a[1] == '-' && a[2] != '\0') {
			/* --key value / --key=value：原样进 JSON（名字里的 - 换成 _） */
			if (n_args >= MAX_ARGS) {
				fprintf(stderr, "vidowmsg: 参数太多了\n");
				return 2;
			}
			const char *value = NULL;
			const char *eq = strchr(a + 2, '=');
			size_t klen = 0;
			if (eq != NULL) {
				klen = (size_t)(eq - (a + 2));
				value = eq + 1;
			} else {
				klen = strlen(a + 2);
				value = i + 1 < argc ? argv[++i] : "true";
			}
			if (klen >= sizeof(args[0].key)) {
				klen = sizeof(args[0].key) - 1;
			}
			memcpy(args[n_args].key, a + 2, klen);
			args[n_args].key[klen] = '\0';
			for (char *p = args[n_args].key; *p != '\0'; p++) {
				if (*p == '-') {
					*p = '_';
				}
			}
			args[n_args].value = value;
			n_args++;
		} else if (cmd == NULL) {
			cmd = a;
			watch = strcmp(a, "watch") == 0;
		} else if (watch) {
			if (n_watch < MAX_ARGS) {
				watch_events[n_watch++] = a;
			}
		} else if (n_positional < MAX_ARGS) {
			positional[n_positional++] = a;
		}
	}

	if (cmd == NULL && json_arg == NULL) {
		usage();
		return 2;
	}
	/* 命令名里的 _ 换成 -（vidow 那边两种都认，但错误信息里用 - 好看） */
	char cmd_buf[64];
	if (cmd != NULL) {
		snprintf(cmd_buf, sizeof(cmd_buf), "%s", cmd);
		for (char *p = cmd_buf; *p != '\0'; p++) {
			if (*p == '_') {
				*p = '-';
			}
		}
		cmd = cmd_buf;
	}

	if (socket_path == NULL) {
		if (!resolve_socket(sockbuf, sizeof(sockbuf))) {
			fprintf(stderr, "vidowmsg: 找不到 vidow 的 socket（用 --socket 指一个，"
				"或者把 WAYLAND_DISPLAY / XDG_RUNTIME_DIR 设好）\n");
			return 2;
		}
		socket_path = sockbuf;
	}

	int fd = connect_socket(socket_path);
	if (fd < 0) {
		return 2;
	}

	/* 拼请求。 */
	char line[8192];
	if (json_arg != NULL) {
		snprintf(line, sizeof(line), "%s\n", json_arg);
	} else {
		FILE *f = fmemopen(line, sizeof(line), "w");
		if (f == NULL) {
			fprintf(stderr, "vidowmsg: 内存不够\n");
			close(fd);
			return 2;
		}
		fputs("{\"cmd\":", f);
		json_escape(f, watch ? "subscribe" : cmd);
		if (watch) {
			if (n_watch > 0) {
				fputs(",\"events\":[", f);
				for (int i = 0; i < n_watch; i++) {
					if (i > 0) {
						fputc(',', f);
					}
					json_escape(f, watch_events[i]);
				}
				fputc(']', f);
			}
		} else {
			for (int i = 0; i < n_args; i++) {
				fputc(',', f);
				json_escape(f, args[i].key);
				fputc(':', f);
				if (strcmp(args[i].key, "data") == 0) {
					json_value_data(f, args[i].value);
				} else {
					json_value(f, args[i].value);
				}
			}
			for (int i = 0; i < n_positional; i++) {
				if (strcmp(cmd, "action") == 0) {
					/* action：后面几个词拼成一条动作字符串 */
					fputs(",\"action\":", f);
					json_escape(f, positional[0]);
					break;
				}
				const char *key = positional_key(cmd, i);
				if (key == NULL) {
					fprintf(stderr, "vidowmsg: %s 用不了第 %d 个位置参数"
						"（用 --key value 形式指认目标）\n", cmd, i + 1);
					fclose(f);
					close(fd);
					return 2;
				}
				fputc(',', f);
				json_escape(f, key);
				fputc(':', f);
				json_value(f, positional[i]);
			}
		}
		fputs("}\n", f);
		long written = ftell(f);
		fclose(f);
		if (written <= 0) {
			fprintf(stderr, "vidowmsg: 请求太长了\n");
			close(fd);
			return 2;
		}
	}

	size_t len = strlen(line);
	if (write(fd, line, len) != (ssize_t)len) {
		fprintf(stderr, "vidowmsg: 发不出去：%s\n", strerror(errno));
		close(fd);
		return 2;
	}

	/* 读应答（watch 一直读下去）。 */
	char *inbuf = malloc(MSG_MAX);
	if (inbuf == NULL) {
		close(fd);
		return 2;
	}
	size_t in_len = 0;
	int status = 0;
	bool done = false;
	while (!done) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int ready = poll(&pfd, 1, watch ? -1 : timeout_sec * 1000);
		if (ready == 0) {
			fprintf(stderr, "vidowmsg: 等应答超时（%d 秒）\n", timeout_sec);
			status = 2;
			break;
		}
		if (ready < 0) {
			if (errno == EINTR) {
				continue;
			}
			fprintf(stderr, "vidowmsg: poll: %s\n", strerror(errno));
			status = 2;
			break;
		}
		ssize_t n = read(fd, inbuf + in_len, MSG_MAX - in_len - 1);
		if (n == 0) {
			break; /* 对面关了 */
		}
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			fprintf(stderr, "vidowmsg: 读不了：%s\n", strerror(errno));
			status = 2;
			break;
		}
		in_len += (size_t)n;
		inbuf[in_len] = '\0';

		char *start = inbuf;
		for (;;) {
			char *nl = memchr(start, '\n', in_len - (size_t)(start - inbuf));
			if (nl == NULL) {
				break;
			}
			*nl = '\0';
			if (start[0] != '\0') {
				if (watch) {
					if (raw) {
						printf("%s\n", start);
					} else {
						pretty_print(start, strlen(start));
					}
					fflush(stdout);
				} else {
					if (strstr(start, "\"ok\":false") != NULL) {
						const char *e = strstr(start, "\"error\":");
						const char *s = e != NULL ? strchr(e + 8, '"') : NULL;
						const char *t = s != NULL ? strchr(s + 1, '"') : NULL;
						if (s != NULL && t != NULL) {
							fprintf(stderr, "%.*s\n", (int)(t - s - 1), s + 1);
						} else {
							fprintf(stderr, "%s\n", start);
						}
						status = 1;
					} else if (raw) {
						printf("%s\n", start);
						fflush(stdout);
					} else {
						/* 应答是 {"ok":true,"result":…}：只排 result
						 * 那一段（末尾那个 '}' 是信封的，去掉）。 */
						const char *r = strstr(start, "\"result\":");
						const char *body = r != NULL ? r + 9 : start;
						const char *end = start + strlen(start);
						if (end > body && end[-1] == '}') {
							end--;
						}
						pretty_print(body, (size_t)(end - body));
					}
					done = true;
					start = nl + 1;
					break;
				}
			}
			start = nl + 1;
		}
		size_t used = (size_t)(start - inbuf);
		memmove(inbuf, start, in_len - used);
		in_len -= used;
	}
	free(inbuf);
	close(fd);
	return status;
}
