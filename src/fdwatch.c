/* fd 泄漏看门狗 —— 只在 VIDOW_FD_WATCH=1 时建。
 *
 * 起因（2026-10-21）：连上 sunshine 之后 vidow 的 fd 在一分钟内从 4 涨到 700+
 * （871 个匿名 dmabuf），打满 1024 的软上限之后 wlroots 报
 * `dup failed: Too many open files`，IPC 全断、sunshine 再也连不上。整个过程
 * vidow 自己**一句话都没打** —— 那些 ERROR 来自 wlroots 的 render/egl.c，混在
 * 一堆噪音里根本看不出来是「fd 用光了」。等发现时现场早过去了。
 *
 * 这个看门狗补的就是那段沉默：
 *
 *   - 每 3 秒数一次 /proc/self/fd（读目录，不占 fd —— 用 opendir 拿 DIR*，
 *     计数时只数条目名，不 open 每一个）
 *   - 只在这个数**比上次高**、且超过软上限 80% 的时候才报（不然空闲时每 3 秒
 *     一条把日志冲爆）
 *   - 报的时候按「指向什么」分类：dmabuf / socket / eventfd / 普通文件……，
 *     dmabuf 再按 inode 数一遍「多少块缓冲被 dup 了几份」—— 那次泄漏的指纹
 *     就是「305 块 dmabuf 被 dup 成 871 个 fd」，一眼能认出来
 *   - 最多报 5 次。之后就算继续涨也闭嘴：真到打满那一刻，wlroots 自己会喊。
 *
 * 数 fd 用的是 readdir 拿 d_name 再 readlink 分类 —— readlink 不占 fd，
 * 所以看门狗自己不会把数推高。
 */

#include <dirent.h>
#include <limits.h>
#include <sys/resource.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/util/log.h>

#include "server.h"
#include "fdwatch.h"

/* 多久数一次。3 秒：那次泄漏一分钟涨 700 个，3 秒的粒度足够在打满之前抓住，
 * 又不至于让日志太吵。 */
#define FD_WATCH_INTERVAL_MS 3000

/* 报几次就闭嘴。 */
#define FD_WATCH_MAX_REPORTS 5

/* 软上限的百分之多少开始报。 */
#define FD_WATCH_WARN_PCT 80

/* 但软上限可能是 524288（systemd 默认）—— 拿它的 80% 当警戒线等于永远不报。
 * 真正的危险线是**这个数附近**：fd 涨到几千时，wlroots / mesa / 各个客户端
 * 各自 open 会开始零星失败，症状是随机的 dup failed / 渲染报错，很难联想到
 * fd。所以警戒线取 min(软上限 80%, 这个绝对值)。
 *
 * 4000 的依据：那次泄漏是 871 个匿名 dmabuf；正常跑一整天在 300~600。4000
 * 留了接近 7 倍余量 —— 只会在真的失控时才响。 */
#define FD_WATCH_ABS_WARN 4000

/* 分类桶。够用就行 —— 目的是「一眼看出是哪类在涨」，不是精确统计。 */
enum fd_kind {
	FD_KIND_DMABUF,
	FD_KIND_SOCKET,
	FD_KIND_EVENTFD,
	FD_KIND_EPOLL,
	FD_KIND_TIMER,
	FD_KIND_SIGNAL,
	FD_KIND_INOTIFY,
	FD_KIND_SYNC,
	FD_KIND_MEMFD,
	FD_KIND_FILE,
	FD_KIND_OTHER,
	FD_KIND_COUNT,
};

static const char *fd_kind_name(enum fd_kind k) {
	switch (k) {
	case FD_KIND_DMABUF: return "dmabuf";
	case FD_KIND_SOCKET: return "socket";
	case FD_KIND_EVENTFD: return "eventfd";
	case FD_KIND_EPOLL: return "epoll";
	case FD_KIND_TIMER: return "timerfd";
	case FD_KIND_SIGNAL: return "signalfd";
	case FD_KIND_INOTIFY: return "inotify";
	case FD_KIND_SYNC: return "sync_file";
	case FD_KIND_MEMFD: return "memfd";
	case FD_KIND_FILE: return "普通文件";
	case FD_KIND_OTHER: return "其它";
	default: return "?";
	}
}

static enum fd_kind classify(const char *target) {
	if (strstr(target, "dmabuf") != NULL) {
		return FD_KIND_DMABUF;
	}
	if (strstr(target, "socket:") != NULL) {
		return FD_KIND_SOCKET;
	}
	if (strstr(target, "eventfd") != NULL) {
		return FD_KIND_EVENTFD;
	}
	if (strstr(target, "eventpoll") != NULL) {
		return FD_KIND_EPOLL;
	}
	if (strstr(target, "timerfd") != NULL) {
		return FD_KIND_TIMER;
	}
	if (strstr(target, "signalfd") != NULL) {
		return FD_KIND_SIGNAL;
	}
	if (strstr(target, "inotify") != NULL) {
		return FD_KIND_INOTIFY;
	}
	if (strstr(target, "sync_file") != NULL) {
		return FD_KIND_SYNC;
	}
	if (strstr(target, "memfd") != NULL) {
		return FD_KIND_MEMFD;
	}
	if (target[0] == '/') {
		return FD_KIND_FILE;
	}
	return FD_KIND_OTHER;
}

/* 读 /proc/self/fdinfo/<n> 里的 `ino:` 行。返回 -1 = 读不到。
 *
 * 这里**必须** open 一次（fdinfo 只能这么读），但读完立刻关，所以对总数的
 * 影响是 +1 常量，不会累积。只对 dmabuf 调 —— 别的类别不需要 inode。 */
static long fd_inode(int fd) {
	char path[64];
	snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
	FILE *f = fopen(path, "re");
	if (f == NULL) {
		return -1;
	}
	long ino = -1;
	char line[256];
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, "ino:", 4) == 0) {
			ino = strtol(line + 4, NULL, 10);
			break;
		}
	}
	fclose(f);
	return ino;
}

/* 拿**软**上限。别用 sysconf(_SC_OPEN_MAX)：它返回的是硬上限（这台机器上
 * 524288），拿它算 80% 永远不会触发 —— 实测栽过。软上限只有 getrlimit 给。 */
static long fd_soft_limit(void) {
	struct rlimit rl;
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
			rl.rlim_cur > 0) {
		return (long)rl.rlim_cur;
	}
	return 1024; /* 常见默认，聊胜于无 */
}

/* 数一遍，打一份分类快照。返回总数（数不出来返回 -1）。 */
static int snapshot(struct vidow_server *server, int report_no) {
	DIR *d = opendir("/proc/self/fd");
	if (d == NULL) {
		wlr_log(WLR_ERROR, "fd 看门狗：打不开 /proc/self/fd（%m）");
		return -1;
	}

	int counts[FD_KIND_COUNT] = {0};
	int total = 0;
	/* dmabuf 的 inode 去重计数：内存里只留一块定长数组，塞满就算了（够诊断）。
	 * 那次泄漏是 305 块，2048 个名额绰绰有余。 */
	enum { MAX_INOS = 2048 };
	static long inos[MAX_INOS];
	int n_inos = 0;

	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		char *end = NULL;
		long n = strtol(e->d_name, &end, 10);
		if (end == e->d_name || *end != '\0') {
			continue; /* "." ".." 以及 opendir 自己那个 fd 的目录项 */
		}
		total++;

		char target[PATH_MAX];
		char link_path[64];
		snprintf(link_path, sizeof(link_path), "/proc/self/fd/%ld", n);
		ssize_t len = readlink(link_path, target, sizeof(target) - 1);
		if (len < 0) {
			/* 数着数着自己关了（看门狗和主循环抢）—— 不算错。 */
			continue;
		}
		target[len] = '\0';

		enum fd_kind k = classify(target);
		if (k < FD_KIND_COUNT) {
			counts[k]++;
		}
		if (k == FD_KIND_DMABUF && n_inos < MAX_INOS) {
			long ino = fd_inode((int)n);
			if (ino >= 0) {
				inos[n_inos++] = ino;
			}
		}
	}
	closedir(d);

	wlr_log(WLR_ERROR, "fd 看门狗 #%d：%d 个 fd 在用（软上限的 80%% 以上，"
		"再涨就要打满了）", report_no, total);

	/* 分类明细：只打非零的，一眼看出是哪类在涨。 */
	char line[512];
	size_t off = 0;
	for (int k = 0; k < FD_KIND_COUNT; k++) {
		if (counts[k] == 0) {
			continue;
		}
		int wrote = snprintf(line + off, sizeof(line) - off, "%s%s=%d",
			off == 0 ? "" : "、", fd_kind_name((enum fd_kind)k), counts[k]);
		if (wrote <= 0 || (size_t)wrote >= sizeof(line) - off) {
			break;
		}
		off += (size_t)wrote;
	}
	wlr_log(WLR_ERROR, "fd 看门狗：%s", line);

	/* dmabuf 的去重指纹：多少块缓冲、被 dup 成几份。那次泄漏就是
	 * 「305 块 → 871 个 fd」，平均每块 2.86 份（EGLImage / GBM BO / 纹理各一份
	 * 的时候正好是 3）。这个数比单纯的总数有信息量得多。 */
	if (n_inos > 0) {
		/* 插入排序：去重化 + 数重复。n_inos 顶天两千，规模够小。 */
		for (int i = 1; i < n_inos; i++) {
			long v = inos[i];
			int j = i - 1;
			while (j >= 0 && inos[j] > v) {
				inos[j + 1] = inos[j];
				j--;
			}
			inos[j + 1] = v;
		}
		int uniq = 0, max_dup = 0, dup3 = 0;
		for (int i = 0; i < n_inos;) {
			int j = i;
			while (j < n_inos && inos[j] == inos[i]) {
				j++;
			}
			int dup = j - i;
			uniq++;
			if (dup > max_dup) {
				max_dup = dup;
			}
			if (dup == 3) {
				dup3++;
			}
			i = j;
		}
		wlr_log(WLR_ERROR, "fd 看门狗：dmabuf %d 个 fd ← %d 块唯一缓冲"
			"（dup 3 份的有 %d 块，单块最多 %d 份）",
			counts[FD_KIND_DMABUF], uniq, dup3, max_dup);
	}

	/* 顺手报一下软上限，方便对照。 */
	FILE *lim = fopen("/proc/self/limits", "re");
	if (lim != NULL) {
		char l2[512];
		while (fgets(l2, sizeof(l2), lim) != NULL) {
			if (strncmp(l2, "Max open files", 14) == 0) {
				wlr_log(WLR_ERROR, "fd 看门狗：%s", l2);
				break;
			}
		}
		fclose(lim);
	}

	(void)server;
	return total;
}

/* ---------------------------------------------------------- 自唤醒 fd 统管
 *
 * 合成器每隔一会儿就要读一堆「自唤醒 fd」：Xwayland ready 的管道、配置监听的
 * inotify、exec 子进程的退出管道、IPC 连接 / 监听 socket、剪贴板拉取管道、屏幕
 * 录制给客户端的 pipe。这里统一轮询这些 fd，限制每轮回调次数。
 *
 * 这里用一个 16ms 周期的 timerfd 当节拍：每拍 poll(2) 一遍登记在册的 fd，
 * 只有真有事的那几个才回调。没人有事 = 这一拍什么都不做（空闲时一秒也就 60
 * 次空 poll，最多几百个 fd）。
 *
 * 两种模式：
 *   - 回调（vidow_fdwatch_register）：可读 → handler(fd, data)。返回 1 = 读完
 *     了；返回 0 = 「我读了一部分、还有新的一轮没处理」（比如子进程退出管道
 *     SIGCHLD 比数据先到），下一拍还会再叫，**返回 0 必须真的读走一点东西**，
 *     不然就是白转一整拍。
 *   - 看一眼（vidow_fdwatch_peek）：可读 → poke(data)，干什么由 poke 自己定
 *     （配置监听在这里读取并筛选 inotify 事件，再启动去抖 stat）。
 *   重试在下一次周期节拍处理。 */
static struct vidow_server *fdwatch_server;
bool vidow_fdwatch_register(int fd, enum vidow_fdwatch_kind kind,
		vidow_fdwatch_handler handler, void *data) {
	struct vidow_server *server = fdwatch_server;
	if (server == NULL || server->fdwatch == NULL || fd < 0 || handler == NULL) {
		return false;
	}
	struct fdwatch_entry *e = calloc(1, sizeof(*e));
	if (e == NULL) {
		return false;
	}
	e->fd = fd;
	e->kind = kind;
	e->handler = handler;
	e->data = data;
	wl_list_insert(&server->fdwatch->entries, &e->link);
	wlr_log(WLR_INFO, "fdwatch: 接管 fd %d（kind %d），改由定时器轮询",
		fd, (int)kind);
	return true;
}

void vidow_fdwatch_deregister(int fd, enum vidow_fdwatch_kind kind,
		void *data) {
	struct vidow_server *server = fdwatch_server;
	if (server == NULL || server->fdwatch == NULL) {
		return;
	}
	struct fdwatch_entry *e, *tmp;
	wl_list_for_each_safe(e, tmp, &server->fdwatch->entries, link) {
		if (e->fd == fd && (kind == VIDOW_FDWATCH_ANY || e->kind == kind) &&
				(data == NULL || e->data == data)) {
			e->gone = true;
			wl_list_remove(&e->link);
			/* 正在回调里（on_stack）的那份由 tick 自己收尸，不能现在 free。 */
			if (!e->on_stack) {
				free(e);
			}
		}
	}
	struct fdwatch_peek *p, *ptmp;
	wl_list_for_each_safe(p, ptmp, &server->fdwatch->peeks, link) {
		if (p->fd == fd && (kind == VIDOW_FDWATCH_ANY || p->kind == kind) &&
				(data == NULL || p->data == data)) {
			p->gone = true;
			wl_list_remove(&p->link);
			free(p);
		}
	}
}

bool vidow_fdwatch_peek(int fd, enum vidow_fdwatch_kind kind,
		vidow_fdwatch_poke_cb poke, void *data) {
	struct vidow_server *server = fdwatch_server;
	if (server == NULL || server->fdwatch == NULL || fd < 0) {
		return false;
	}
	struct fdwatch_peek *p = calloc(1, sizeof(*p));
	if (p == NULL) {
		return false;
	}
	p->fd = fd;
	p->kind = kind;
	p->poke = poke;
	p->data = data;
	wl_list_insert(&server->fdwatch->peeks, &p->link);
	return true;
}
/* 这个 fd 在不在册（同一种 kind 至少挂了一份）。 */
bool vidow_fdwatch_tracks(int fd, enum vidow_fdwatch_kind kind) {
	struct vidow_server *server = fdwatch_server;
	if (server == NULL || server->fdwatch == NULL) {
		return false;
	}
	struct fdwatch_entry *e;
	wl_list_for_each(e, &server->fdwatch->entries, link) {
		if (e->fd == fd && e->kind == kind) {
			return true;
		}
	}
	struct fdwatch_peek *p;
	wl_list_for_each(p, &server->fdwatch->peeks, link) {
		if (p->fd == fd && p->kind == kind) {
			return true;
		}
	}
	return false;
}

static void fdwatch_add_poll(struct pollfd *pfds, size_t max, size_t *n,
		int fd) {
	for (size_t i = 0; i < *n; i++) {
		if (pfds[i].fd == fd) {
			return;
		}
	}
	if (*n >= max) {
		return;
	}
	pfds[*n].fd = fd;
	pfds[*n].events = POLLIN;
	pfds[*n].revents = 0;
	(*n)++;
}

/* timerfd 的可读回调：source 是 fd 源，不是 Wayland timer 源。 */
static int fdwatch_tick(int fd, uint32_t mask, void *data) {
	(void)mask;
	struct vidow_server *server = data;
	struct vidow_fdwatch *w = server->fdwatch;
	if (w == NULL || fd != w->timer_fd) {
		return 0;
	}

	/* 一次读取合并积压的节拍，不追赶式连跑。 */
	uint64_t ticks;
	ssize_t len;
	do {
		len = read(w->timer_fd, &ticks, sizeof(ticks));
	} while (len < 0 && errno == EINTR);
	w->wake = false;
	struct fdwatch_entry *etmp;
	struct fdwatch_peek *ptmp;

	enum { MAX_FDS = 512 };
	struct pollfd pfds[MAX_FDS];
	size_t n = 0;
	struct fdwatch_entry *e;
	wl_list_for_each(e, &w->entries, link) {
		fdwatch_add_poll(pfds, MAX_FDS, &n, e->fd);
	}
	struct fdwatch_peek *p;
	wl_list_for_each(p, &w->peeks, link) {
		fdwatch_add_poll(pfds, MAX_FDS, &n, p->fd);
	}

	/* 超时 0：上一拍之后该跑的程序（钩子、idle 队列、IPC 请求）已经跑完，
	 * 这里只问一次内核「有没有新东西」。 */
	int rc = n > 0 ? poll(pfds, n, 0) : 0;
	if (rc < 0 && errno != EINTR) {
		wlr_log_errno(WLR_ERROR, "fdwatch: poll 失败");
		rc = 0;
	}

	/* 先看一眼那批，只调用 poll 报告就绪的 fd。 */
	wl_list_for_each_safe(p, ptmp, &w->peeks, link) {
		bool hot = false;
		for (size_t i = 0; rc > 0 && i < n; i++) {
			if (pfds[i].fd == p->fd) {
				hot = pfds[i].revents != 0;
				break;
			}
		}
		if (hot && p->poke != NULL) {
			/* 回调可注销当前项，返回后不再访问 p。 */
			p->poke(p->data);
		}
	}

	/* 回调那批。上一拍明确说「还没读完」的，这一拍无论 poll 报不报都再叫一遍：
	 * 内核那份状态已经读掉了一半，poll 不一定还会报。 */
	wl_list_for_each_safe(e, etmp, &w->entries, link) {
		bool hot = e->retry;
		if (!hot) {
			for (size_t i = 0; rc > 0 && i < n; i++) {
				if (pfds[i].fd == e->fd) {
					hot = pfds[i].revents != 0;
					break;
				}
			}
		}
		if (!hot) {
			continue;
		}
		e->on_stack = true;
		int done = e->handler(e->fd, e->data);
		e->on_stack = false;
		if (e->gone) {
			free(e); /* dereg 发生在回调里：那时候不好 free，现在收尸 */
			continue;
		}
		e->retry = !done;
		if (!done) {
			w->wake = true;
		}
	}

	/* 重试由下一次周期 timerfd 节拍处理；source 不能用于 timer_update。 */
	return 0;
}

static int fd_watch_tick(void *data) {
	struct vidow_server *server = data;

	/* 先重新排上：下一轮还要跑。 */
	wl_event_source_timer_update(server->fd_watch_timer, FD_WATCH_INTERVAL_MS);

	int total = -1;
	/* 先粗数一遍（不分类、不读 inode），只在超过阈值时再做完整快照 ——
	 * 常态下每 3 秒只读一次目录，几乎不花钱。 */
	DIR *d = opendir("/proc/self/fd");
	if (d == NULL) {
		return 0;
	}
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] >= '0' && e->d_name[0] <= '9') {
			n++;
		}
	}
	closedir(d);
	total = n;

	if (total < 0) {
		return 0;
	}

	long soft = fd_soft_limit();
	long warn_at = soft * FD_WATCH_WARN_PCT / 100;
	if (warn_at > FD_WATCH_ABS_WARN) {
		warn_at = FD_WATCH_ABS_WARN;
	}

	/* 只在这个数「比上次高」且过了警戒线时才报。 */
	if (total < warn_at || total <= server->fd_watch_last) {
		server->fd_watch_last = total;
		return 0;
	}
	server->fd_watch_last = total;

	if (server->fd_watch_reports >= FD_WATCH_MAX_REPORTS) {
		return 0; /* 报够了，别再刷 */
	}
	server->fd_watch_reports++;
	snapshot(server, server->fd_watch_reports);

	if (server->fd_watch_reports == FD_WATCH_MAX_REPORTS) {
		wlr_log(WLR_ERROR, "fd 看门狗：已经报满 %d 次，之后不再提示"
			"（fd 真打满时 wlroots 会自己报 dup failed）",
			FD_WATCH_MAX_REPORTS);
	}
	return 0;
}

/* ------------------------------------------------------------- 节拍 */

/* 多久扫一遍。16ms ≈ 60Hz：idle 唤醒的代价是「一拍内起来的活儿」，16ms 的粒度
 * 够快，又不至于空闲时每毫秒都醒一次。 */
#define FDWATCH_INTERVAL_NS 16000000LL

void vidow_fdwatch_init(struct vidow_server *server) {
	/* 全局指针：不用在每个回调的数据里各揣一份 server。进程里只有一个合成器。 */
	fdwatch_server = server;
	if (server->fdwatch != NULL) {
		return;
	}
	struct vidow_fdwatch *w = calloc(1, sizeof(*w));
	if (w == NULL) {
		wlr_log(WLR_ERROR, "fdwatch: 起不来（内存）");
		return;
	}
	w->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (w->timer_fd < 0) {
		wlr_log_errno(WLR_ERROR, "fdwatch: timerfd_create 失败");
		free(w);
		return;
	}
	struct itimerspec its = {
		.it_interval = {.tv_nsec = FDWATCH_INTERVAL_NS},
		.it_value = {.tv_nsec = FDWATCH_INTERVAL_NS},
	};
	if (timerfd_settime(w->timer_fd, 0, &its, NULL) != 0) {
		wlr_log_errno(WLR_ERROR, "fdwatch: timerfd 起不来");
		close(w->timer_fd);
		free(w);
		return;
	}
	wl_list_init(&w->entries);
	wl_list_init(&w->peeks);
	w->source = wl_event_loop_add_fd(
		wl_display_get_event_loop(server->display), w->timer_fd,
		WL_EVENT_READABLE, fdwatch_tick, server);
	if (w->source == NULL) {
		wlr_log(WLR_ERROR, "fdwatch: timerfd 挂不上事件循环");
		close(w->timer_fd);
		free(w);
		return;
	}
	server->fdwatch = w;
	wlr_log(WLR_INFO, "fdwatch: 每 %lldms 看一遍自唤醒 fd（第一次登记 / 注销时"
		"在日志里留一行）", (long long)(FDWATCH_INTERVAL_NS / 1000000));
}

void vidow_fdwatch_finish(struct vidow_server *server) {
	struct vidow_fdwatch *w = server->fdwatch;
	if (w == NULL) {
		return;
	}
	server->fdwatch = NULL;
	struct fdwatch_entry *e, *etmp;
	wl_list_for_each_safe(e, etmp, &w->entries, link) {
		wl_list_remove(&e->link);
		free(e);
	}
	struct fdwatch_peek *p, *ptmp;
	wl_list_for_each_safe(p, ptmp, &w->peeks, link) {
		wl_list_remove(&p->link);
		free(p);
	}
	if (w->source != NULL) {
		wl_event_source_remove(w->source);
	}
	if (w->timer_fd >= 0) {
		close(w->timer_fd);
	}
	free(w);
}

void vidow_fd_watch_init(struct vidow_server *server) {
	/* 只有显式开了才跑。默认关：正常用户不需要，出问题的时候拿来抓现场。 */
	const char *env = getenv("VIDOW_FD_WATCH");
	if (env == NULL || env[0] == '\0' || strcmp(env, "0") == 0) {
		return;
	}

	struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
	server->fd_watch_timer = wl_event_loop_add_timer(loop, fd_watch_tick,
		server);
	if (server->fd_watch_timer == NULL) {
		wlr_log(WLR_ERROR, "fd 看门狗起不来");
		return;
	}
	server->fd_watch_last = 0;
	server->fd_watch_reports = 0;
	wl_event_source_timer_update(server->fd_watch_timer, FD_WATCH_INTERVAL_MS);

	long soft = fd_soft_limit();
	long warn_at = soft * FD_WATCH_WARN_PCT / 100;
	if (warn_at > FD_WATCH_ABS_WARN) {
		warn_at = FD_WATCH_ABS_WARN;
	}
	wlr_log(WLR_INFO, "fd 看门狗：每 %d 秒数一次，超过 %ld 个就报"
		"（软上限 %ld，取 80%% 和 %d 的较小者；VIDOW_FD_WATCH=0 可关）",
		FD_WATCH_INTERVAL_MS / 1000, warn_at, soft, FD_WATCH_ABS_WARN);
}

void vidow_fd_watch_finish(struct vidow_server *server) {
	if (server->fd_watch_timer != NULL) {
		wl_event_source_remove(server->fd_watch_timer);
		server->fd_watch_timer = NULL;
	}
}
