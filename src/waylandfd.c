/* Wayland 客户端「被断连」的取证探针。
 *
 * 起因（2026-11-02）：Element / kitty 会莫名其妙被 vidow 踢掉 —— 客户端日志
 * 最后一行是 "Fatal Wayland communication error: 连接被对方重置 / 断开的管道"，
 * vidow 这边只留一句
 *     [wayland] error in client communication (pid N)
 * 就完了。libwayland 那条路是**静默**的：
 *
 *   wl_resource_post_event() → handle_array()
 *     → wl_closure_send() → wl_connection_write() → wl_connection_queue()
 *         → 写不出去（sendmsg 失败）→ 返回 -1
 *     → handle_array 只把 client->error 置位，**不打任何日志**
 *   → 这一轮请求派发结束后 wl_client_connection_data() 才打一句
 *     "error in client communication (pid N)" 并把连接销毁。
 *
 * 也就是说：真正的原因（sendmsg 到底为什么失败）从来没进过日志。整个
 * libwayland 里只有 "Data too big for buffer"（E2BIG）和
 * "failed to flush client connection" 这两条会说话，别的 errno 全是哑的。
 *
 * 这个文件在 vidow 进程里拦截 sendmsg（--dynamic-list 只导出一个符号，
 * 不会把 vidow 自己的全局符号泄进动态符号表），失败且 errno 不是 EAGAIN /
 * EINTR 时打一行 ERROR，带上：
 *   - fd、errno 名字、对端 pid（SO_PEERCRED，直接对上 vidow 日志里的 pid）
 *   - 这次要写多少字节、带了几个 fd（SCM_RIGHTS）
 *   - fd 现在指向什么（/proc/self/fd/N 的链接；EBADF 时为空）
 *
 * EBADF = 有人把不属于自己的 fd 关了（fd 号复用），那是 vidow 自己的 bug；
 * EPIPE / ECONNRESET = 对端先关了（客户端自己的事）；ENOBUFS / ENOMEM =
 * 内核内存吃紧。三种情况的修法完全不同，所以必须把 errno 记下来。 */
#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include <wlr/util/log.h>

typedef ssize_t (*sendmsg_fn)(int, const struct msghdr *, int);

/* RTLD_NEXT：拿 libc 里那个真的 sendmsg。第一次调用时解析（此时还不在
 * sendmsg 里面，安全）。解析失败就退回裸 syscall，功能一样、只是少了
 * glibc 的线程取消点语义 —— libwayland 不用取消点。 */
static sendmsg_fn real_sendmsg;

static ssize_t call_real_sendmsg(int fd, const struct msghdr *msg, int flags) {
	if (real_sendmsg == NULL) {
		real_sendmsg = (sendmsg_fn)dlsym(RTLD_NEXT, "sendmsg");
	}
	if (real_sendmsg != NULL) {
		return real_sendmsg(fd, msg, flags);
	}
	return (ssize_t)syscall(SYS_sendmsg, fd, msg, flags);
}

/* 这条连接的对端是谁。失败时也照样问 —— 只要 fd 还合法就有答案；
 * EBADF 时 getsockopt 也会失败，那就报 -1。 */
static pid_t peer_pid(int fd) {
	struct ucred cred;
	socklen_t len = sizeof cred;
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
		return -1;
	}
	return cred.pid;
}

/* SCM_RIGHTS 里带了几个 fd —— keymap、data offer、dmabuf 都走这条，
 * 数量本身就是线索（keymap 事件只带 1 个）。 */
static int count_cmsg_fds(struct msghdr *msg) {
	int n = 0;
	for (struct cmsghdr *c = CMSG_FIRSTHDR(msg); c != NULL;
			c = CMSG_NXTHDR(msg, c)) {
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
			n += (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
		}
	}
	return n;
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags) {
	ssize_t ret = call_real_sendmsg(fd, msg, flags);
	if (ret >= 0) {
		return ret;
	}

	int err = errno;
	if (err == EAGAIN || err == EINTR) {
		/* 正常：socket 暂时写不出去，libwayland 自己会等可写事件 */
		return ret;
	}

	size_t bytes = 0;
	for (size_t i = 0; i < msg->msg_iovlen; i++) {
		bytes += msg->msg_iov[i].iov_len;
	}
	int fds = count_cmsg_fds((struct msghdr *)msg);
	pid_t pid = peer_pid(fd);

	char target[256] = "";
	char link[64];
	snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
	ssize_t n = readlink(link, target, sizeof target - 1);
	if (n > 0) {
		target[n] = '\0';
	} else {
		snprintf(target, sizeof target, "（fd 已经无效）");
	}

	/* 用 wlr_log：和合成器其他日志同一个出口、同一个时间戳。
	 * 这一行是「客户端为什么被踢」的唯一直接证据，ERROR 级别值得。 */
	wlr_log(WLR_ERROR,
		"wayland: 给客户端写事件失败：sendmsg(fd=%d) = %s（errno=%d），"
		"本轮 %zu 字节 / %d 个 fd，对端 pid=%d，fd 指向 %s"
		" —— libwayland 会因此静默销毁这条连接",
		fd, strerror(err), err, bytes, fds, (int)pid, target);

	errno = err;
	return ret;
}
