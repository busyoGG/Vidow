#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <wlr/util/log.h>

/* A config command is a helper, not a job: if it has not answered by now the
 * config falls back to whatever it had before. */
#define EXEC_DEFAULT_TIMEOUT_MS 400
#define EXEC_MAX_BYTES (64 * 1024)

/* One pipe, drained by the compositor's own event loop. */
struct exec_stream {
	int fd;
	struct wl_event_source *src;
	char *buf;
	size_t len, cap;
	bool eof;
};

/* One `sh -c` in flight. stdout and stderr are kept apart: stdout is the
 * value, stderr is the reason a command failed. Nothing here blocks a frame. */
struct vidow_exec {
	struct wl_list link; /* vidow_server.execs */
	struct vidow_server *server;
	struct exec_stream out, err;
	pid_t pid;
	int64_t deadline_ns;
	int status;
	vidow_exec_done_t done;
	void *data;
	bool finished;
};

static int exec_timeout_ms(void) {
	const char *env = getenv("VIDOW_EXEC_TIMEOUT");
	int ms = env != NULL ? atoi(env) : 0;
	return ms > 0 ? ms : EXEC_DEFAULT_TIMEOUT_MS;
}

static void exec_finish(struct vidow_exec *job, bool ok, const char *reason);

/* The timeout is a single shared timer rather than one per job: a timer that
 * removes itself from inside its own callback is a trap. */
static void exec_arm_timer(struct vidow_server *server) {
	if (server->exec_timer == NULL) {
		return;
	}
	int64_t soonest = 0;
	struct vidow_exec *job;
	wl_list_for_each(job, &server->execs, link) {
		if (soonest == 0 || job->deadline_ns < soonest) {
			soonest = job->deadline_ns;
		}
	}
	if (soonest == 0) {
		wl_event_source_timer_update(server->exec_timer, 0);
		return;
	}
	int64_t now = vidow_now_ns();
	int ms = soonest <= now ? 1 : (int)((soonest - now) / 1000000LL) + 1;
	wl_event_source_timer_update(server->exec_timer, ms);
}

/* Both pipes are closed (EOF) or the process was just killed, so this returns
 * immediately -- and it is what gives us the exit code. */
static void exec_reap(struct vidow_exec *job) {
	if (job->pid <= 0) {
		return;
	}
	int status = 0;
	pid_t reaped;
	do {
		reaped = waitpid(job->pid, &status, 0);
	} while (reaped < 0 && errno == EINTR);
	job->pid = -1;

	if (reaped < 0) {
		job->status = -1;
	} else if (WIFEXITED(status)) {
		job->status = WEXITSTATUS(status);
	} else if (WIFSIGNALED(status)) {
		job->status = 128 + WTERMSIG(status);
	} else {
		job->status = -1;
	}
}

static void exec_stream_close(struct exec_stream *st) {
	if (st->src != NULL) {
		wl_event_source_remove(st->src);
		st->src = NULL;
	}
	if (st->fd >= 0) {
		close(st->fd);
		st->fd = -1;
	}
}

static void exec_stream_eof(struct vidow_exec *job, struct exec_stream *st) {
	st->eof = true;
	exec_stream_close(st);
	if (job->out.eof && job->err.eof) {
		exec_finish(job, true, NULL);
	}
}

static void exec_finish(struct vidow_exec *job, bool ok, const char *reason) {
	if (job->finished) {
		return;
	}
	job->finished = true;
	exec_stream_close(&job->out);
	exec_stream_close(&job->err);
	exec_reap(job);
	wl_list_remove(&job->link);

	vidow_exec_done_t done = job->done;
	void *data = job->data;
	char *out = job->out.buf;
	char *errout = job->err.buf;
	int status = job->status;
	job->out.buf = NULL;
	job->err.buf = NULL;
	free(job);

	/* $(...) semantics: the trailing newline of `echo` is not part of the
	 * value. Interior newlines survive, so multi-line output still works. */
	char *streams[] = { out, errout };
	for (size_t i = 0; i < 2; i++) {
		char *text = streams[i];
		if (text != NULL) {
			size_t len = strlen(text);
			while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
				text[--len] = '\0';
			}
		} else if (ok) {
			/* A command that printed nothing still succeeded: "" not NULL. */
			streams[i] = strdup("");
		}
	}
	out = streams[0];
	errout = streams[1];

	if (!ok) {
		/* The reason beats whatever partial output arrived before the kill. */
		free(out);
		free(errout);
		out = NULL;
		errout = reason != NULL ? strdup(reason) : NULL;
	}

	done(data, out, errout, status, ok);
	free(out);
	free(errout);
}

static int exec_drain(struct vidow_exec *job, struct exec_stream *st,
		uint32_t mask) {
	if (!(mask & WL_EVENT_READABLE)) {
		if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
			exec_stream_eof(job, st);
		}
		return 0;
	}

	char buf[4096];
	for (;;) {
		ssize_t n = read(st->fd, buf, sizeof(buf));
		if (n == 0) {
			exec_stream_eof(job, st);
			return 0;
		}
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return 0;
			}
			exec_stream_eof(job, st);
			return 0;
		}
		/* Keep draining past the cap so the child never blocks on a full
		 * pipe, but stop growing the buffer. */
		if (st->len + (size_t)n + 1 > EXEC_MAX_BYTES) {
			continue;
		}
		if (st->len + (size_t)n + 1 > st->cap) {
			size_t cap = (st->len + (size_t)n + 1) * 2;
			char *grown = realloc(st->buf, cap);
			if (grown == NULL) {
				continue;
			}
			st->buf = grown;
			st->cap = cap;
		}
		memcpy(st->buf + st->len, buf, (size_t)n);
		st->len += (size_t)n;
		st->buf[st->len] = '\0';
	}
}

static int exec_out_readable(int fd, uint32_t mask, void *data) {
	(void)fd;
	return exec_drain(data, &((struct vidow_exec *)data)->out, mask);
}

static int exec_err_readable(int fd, uint32_t mask, void *data) {
	(void)fd;
	return exec_drain(data, &((struct vidow_exec *)data)->err, mask);
}

static int exec_timeout_cb(void *data) {
	struct vidow_server *server = data;
	int64_t now = vidow_now_ns();

	struct vidow_exec *job, *tmp;
	wl_list_for_each_safe(job, tmp, &server->execs, link) {
		if (now < job->deadline_ns) {
			continue;
		}
		wlr_log(WLR_DEBUG, "exec: pid %d timed out after %dms, killed",
			(int)job->pid, exec_timeout_ms());
		kill(-job->pid, SIGKILL); /* the whole group: no orphaned children */
		exec_finish(job, false, "timeout");
	}
	exec_arm_timer(server);
	return 0;
}

static bool exec_stream_open(struct vidow_exec *job, struct exec_stream *st,
		int fd, wl_event_loop_fd_func_t handler) {
	st->fd = fd;
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) {
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	}
	st->src = wl_event_loop_add_fd(wl_display_get_event_loop(job->server->display),
		fd, WL_EVENT_READABLE, handler, job);
	return st->src != NULL;
}

struct vidow_exec *vidow_exec_run(struct vidow_server *server,
		const char *cmd, vidow_exec_done_t done, void *data) {
	int outpipe[2], errpipe[2];
	if (pipe(outpipe) != 0 || pipe(errpipe) != 0) {
		done(data, NULL, NULL, -1, false);
		return NULL;
	}

	pid_t pid = fork();
	if (pid < 0) {
		close(outpipe[0]);
		close(outpipe[1]);
		close(errpipe[0]);
		close(errpipe[1]);
		done(data, NULL, NULL, -1, false);
		return NULL;
	}

	if (pid == 0) {
		/* Child. Only async-signal-safe calls from here to execl(). */
		setpgid(0, 0);
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, 0);
		}
		dup2(outpipe[1], 1);
		dup2(errpipe[1], 2);
		if (devnull > 2) {
			close(devnull);
		}
		close(outpipe[0]);
		close(outpipe[1]);
		close(errpipe[0]);
		close(errpipe[1]);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127); /* never exit(): that would run the compositor's atexit */
	}

	close(outpipe[1]);
	close(errpipe[1]);

	struct vidow_exec *job = calloc(1, sizeof(*job));
	if (job == NULL) {
		close(outpipe[0]);
		close(errpipe[0]);
		kill(-pid, SIGKILL);
		waitpid(pid, NULL, 0);
		done(data, NULL, NULL, -1, false);
		return NULL;
	}
	job->server = server;
	job->pid = pid;
	job->out.fd = -1;
	job->err.fd = -1;
	job->done = done;
	job->data = data;
	job->deadline_ns = vidow_now_ns() + (int64_t)exec_timeout_ms() * 1000000LL;

	if (!exec_stream_open(job, &job->out, outpipe[0], exec_out_readable) ||
			!exec_stream_open(job, &job->err, errpipe[0], exec_err_readable)) {
		exec_stream_close(&job->out);
		exec_stream_close(&job->err);
		kill(-pid, SIGKILL);
		waitpid(pid, NULL, 0);
		free(job);
		done(data, NULL, NULL, -1, false);
		return NULL;
	}

	wl_list_insert(&server->execs, &job->link);
	exec_arm_timer(server);
	wlr_log(WLR_DEBUG, "exec: pid %d: sh -c %s", (int)pid, cmd);
	return job;
}

void vidow_exec_cancel_all(struct vidow_server *server) {
	struct vidow_exec *job, *tmp;
	wl_list_for_each_safe(job, tmp, &server->execs, link) {
		job->done = NULL; /* the caller is going away */
		if (job->pid > 0) {
			kill(-job->pid, SIGKILL);
		}
		job->finished = true;
		exec_stream_close(&job->out);
		exec_stream_close(&job->err);
		exec_reap(job);
		wl_list_remove(&job->link);
		free(job->out.buf);
		free(job->err.buf);
		free(job);
	}
	exec_arm_timer(server);
}

/* 跑一条 shell 命令然后立刻撒手：双 fork + setsid，中间那层马上退出，所以
 * vidow 不会被 waitpid 卡住，也不用管 SIGCHLD。输出和 stdin 都接到 /dev/null。
 * 给「同步一下 Xresources」「重启 portal」这种一次性小活。 */
void vidow_exec_detached(const char *script) {
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log(WLR_ERROR, "exec: fork 失败: %s", strerror(errno));
		return;
	}
	if (pid == 0) {
		setsid();
		if (fork() != 0) {
			_exit(0);
		}
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO) {
				close(devnull);
			}
		}
		/* ① 信号屏蔽字复位。
		 * vidow 自己会屏蔽一批信号（事件循环那一套），子进程**继承**过去 ——
		 * 应用一旦等被屏蔽的信号（SIGCHLD 最典型）就永久卡住，表现就是用户报的
		 * 「自启动应用会自杀 / 感觉像是被卡死一样」。fork+exec 之间只允许
		 * async-signal-safe 的调用，sigprocmask 正好是。 */
		sigset_t empty;
		sigemptyset(&empty);
		sigprocmask(SIG_SETMASK, &empty, NULL);
		/* ② 继承来的 fd 全关掉（vidow 的 DRM fd / Wayland socket / epoll /
		 * 客户端缓冲）。撒手的进程不该攥着合成器的 fd —— 有应用会因此判定
		 * 「我在某个会话里」而行为异常，也会拖住 socket 的释放。
		 * close_range 没有就用 close 循环兜底。 */
#if defined(__linux__) && defined(SYS_close_range)
		if (syscall(SYS_close_range, 3U, ~0U, 0U) != 0)
#endif
		{
			for (int fd = 3; fd < 1024; fd++) {
				close(fd);
			}
		}
		execl("/bin/sh", "sh", "-c", script, (char *)NULL);
		_exit(127);
	}
	int status;
	waitpid(pid, &status, 0);
}

bool vidow_exec_init(struct vidow_server *server) {
	wl_list_init(&server->execs);
	server->exec_timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server->display), exec_timeout_cb, server);
	if (server->exec_timer == NULL) {
		wlr_log(WLR_ERROR, "failed to create the exec timeout timer");
		return false;
	}
	return true;
}
