#include <unistd.h>
#include "server.h"
#include "env.h"
#include "hooks.h"
#include "ipc.h"
#include "gesture.h"
#include "sessionlock.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>

static enum wlr_log_importance log_level_from_env(void) {
	const char *level = getenv("VIDOW_LOG");
	if (level == NULL) {
		return WLR_INFO;
	}
	if (strcmp(level, "debug") == 0) {
		return WLR_DEBUG;
	}
	if (strcmp(level, "error") == 0) {
		return WLR_ERROR;
	}
	return WLR_INFO;
}

/* 退出前把挂在 wlroots 全局对象上的监听器摘下来。
 *
 * 为什么必须摘：wlroots 的几个全局对象在销毁时会断言「没人还挂在我这儿」——
 *   vidow: types/xdg_shell/wlr_xdg_shell.c:134: handle_display_destroy:
 *   Assertion `wl_list_empty(&xdg_shell->events.new_toplevel.listener_list)'
 * 以前 vidow 只有「被 SIGTERM 杀掉」这一种退法（信号默认动作直接结束进程，
 * 走不到析构），所以这条断言从没露过面；IPC 的 quit 让正常退出成了常规路径，
 * 它才冒出来（退出时 SIGABRT + core dump）。
 *
 * 这里只摘「挂在 wlroots 对象上、由我们自己加的」那些：对象内部的监听器
 * （窗口 / 输出 / layer 的）各自在 destroy 回调里已经摘干净了。 */

/* SIGTERM / SIGINT：走正常退出路径（wl_display_terminate），不用默认动作直接
 * 结束进程。默认动作不跑清理，环境还原（portalconf.c）也就没了 —— 用户管理器
 * 里会一直留着 vidow 的 socket，回到宿主合成器以后新拉起来的 portal 后端连的是
 * 一个已经消失的 socket。Ctrl+C 和 pkill 都走这条。 */
static int terminate_signal(int signo, void *data) {
	(void)signo;
	struct vidow_server *server = data;
	wlr_log(WLR_INFO, "收到退出信号，收尾");
	wl_display_terminate(server->display);
	return 0;
}

/* 会话从「切走了」回到「回来了」：VT 切回来（Ctrl+Alt+F3 再切回来）时，屏幕上的
 * framebuffer 已经是别人的了，场景里的东西一个像素都没变，但必须整屏重画一遍 ——
 * 不然切回来看到的是一张旧图 / 花屏。wlroots 的 DRM 后端自己管「不活动时别往 DRM
 * 上画」，这里只管「回来了重画」。
 *
 * 还有一件事只有这里能做：DRM 后端切走时会把**所有输出销毁**，切回来再重新建
 * （handle_session_active → scan_drm_connectors）。新建的输出要等一次 commit 才
 * 有画面，而这时候场景里一点 damage 都没有、后端也不欠我们 frame 事件 —— 光靠
 * 等 frame 就是「切回来卡死」。所以这里主动催一帧（vidow_output_kick_all）。 */
static void session_active(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_server *server =
		wl_container_of(listener, server, session_active);
	bool active = server->session != NULL && server->session->active;
	int n_outputs = 0;
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		n_outputs++;
	}
	wlr_log(WLR_INFO, "会话: %s（%s；现在 %d 块屏）",
		active ? "回来了" : "被切走了",
		active ? "整屏重画一遍" : "wlroots 会停止往 DRM 上画，等切回来",
		n_outputs);
	if (active) {
		vidow_backdrop_damage(server, true);
		vidow_arrange(server);
		vidow_output_kick_all(server);
	}
}

/* ------------------------------------------------ 空闲通知 / 空闲抑制 */

/* 两个 global + 两处「wlroots 不给、必须自己接」的线：
 *
 *   1. ext-idle-notify-v1（wlr_idle_notifier_v1_create）：swayidle / hypridle
 *      这类「空闲到点就跑命令（锁屏 / 关屏）」的客户端靠它。**活动要自己喂** ——
 *      wlr_idle_notify_v1.h 原文："Compositors should call this function
 *      whenever an input event is triggered on a seat"。不喂的话通知在创建后
 *      timeout 到点就 idled，之后再也不 resumed：swayidle 会一直以为人在发呆
 *      （等于「一直在锁」）。vidow 自己**不**实现「超时后锁屏」—— 那是它们的事。
 *   2. zwp-idle-inhibit-unstable-v1（wlr_idle_inhibit_v1_create）：播视频 / 看
 *      网页的客户端用它说「别锁屏 / 别关屏」。**抑制要自己转达** —— wlroots 那份
 *      只管收客户端的 new_inhibitor，一个字节都不碰 notifier，得合成器自己调
 *      wlr_idle_notifier_v1_set_inhibited()。不接这一步，inhibit 客户端连上了也
 *      等于没连（正是本步要解决的那半边）。
 *
 * 活动信号挂在设备上（backend 的 new_input）：真实键盘 / 鼠标 / 触控板全走这条
 * 路。vidow 里更靠内的单一入口是 input.c 的 input_note_key_press() /
 * vidow_pointer_motion()，但本步范围只让动 main.c（input.c 归会话锁那条线），
 * 所以没往那边挂 —— 代价是虚拟设备（wtype / 输入法）和 IPC 注入的键鼠
 * （vidowmsg send-key 之类）不算「人动了」，真实硬件路径是准的。触屏不在内：
 * vidow 本来就没接 touch（input.c 只认 KEYBOARD / POINTER）。
 * 锁定期间照喂不误：锁是 input.c 自己吞的，喂活动没有坏处。 */

struct vidow_idle_activity {
	struct wl_list link; /* vidow_idle_device.activities */
	struct wl_listener listener;
	struct vidow_server *server;
};

struct vidow_idle_device {
	struct wl_listener destroy;
	struct wl_list activities; /* struct vidow_idle_activity */
};

static void idle_activity(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_idle_activity *activity =
		wl_container_of(listener, activity, listener);
	if (activity->server->idle_notifier != NULL) {
		wlr_idle_notifier_v1_notify_activity(activity->server->idle_notifier,
			activity->server->seat);
	}
}

static void idle_device_activity_add(struct vidow_server *server,
		struct vidow_idle_device *device, struct wl_signal *signal) {
	struct vidow_idle_activity *activity = calloc(1, sizeof(*activity));
	if (activity == NULL) {
		return;
	}
	activity->server = server;
	activity->listener.notify = idle_activity;
	wl_signal_add(signal, &activity->listener);
	wl_list_insert(&device->activities, &activity->link);
}

static void idle_device_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_idle_device *device =
		wl_container_of(listener, device, destroy);
	struct vidow_idle_activity *activity, *tmp;
	wl_list_for_each_safe(activity, tmp, &device->activities, link) {
		/* 设备的 finish 会断言「这些信号上没人挂着」（wlr_touch_finish 那种
		 * assert），所以必须在这里摘干净 —— 现在还在 emit 里，来得及。 */
		wl_list_remove(&activity->listener.link);
		wl_list_remove(&activity->link);
		free(activity);
	}
	wl_list_remove(&device->destroy.link);
	free(device);
}

static void idle_new_input(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, idle_new_input);
	struct wlr_input_device *device = data;
	struct vidow_idle_device *idle = calloc(1, sizeof(*idle));
	if (idle == NULL) {
		return;
	}
	wl_list_init(&idle->activities);
	idle->destroy.notify = idle_device_destroy;
	wl_signal_add(&device->events.destroy, &idle->destroy);
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		idle_device_activity_add(server, idle,
			&wlr_keyboard_from_input_device(device)->events.key);
		break;
	case WLR_INPUT_DEVICE_POINTER: {
		struct wlr_pointer *pointer = wlr_pointer_from_input_device(device);
		idle_device_activity_add(server, idle, &pointer->events.motion);
		idle_device_activity_add(server, idle, &pointer->events.motion_absolute);
		idle_device_activity_add(server, idle, &pointer->events.button);
		idle_device_activity_add(server, idle, &pointer->events.axis);
		break;
	}
	default:
		break;
	}
}

/* 客户端要了一个 inhibitor。销毁走每个 inhibitor 自己的 destroy 信号 —— 不能靠
 * 「manager->inhibitors 空没空」判断：wlroots 是先 emit destroy、之后才把它从那个
 * 链表里摘掉的（见 types/wlr_idle_inhibit_v1.c 的 idle_inhibitor_v1_destroy）。 */
struct vidow_idle_inhibitor {
	struct wl_listener destroy;
	struct vidow_server *server;
};

static void idle_inhibitor_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_idle_inhibitor *inhibitor =
		wl_container_of(listener, inhibitor, destroy);
	struct vidow_server *server = inhibitor->server;
	/* wlroots 在 emit 之后断言「没人还挂在这个信号上」 */
	wl_list_remove(&inhibitor->destroy.link);
	free(inhibitor);
	server->idle_inhibitors--;
	if (!server->shutting_down && server->idle_notifier != NULL) {
		wlr_idle_notifier_v1_set_inhibited(server->idle_notifier,
			server->idle_inhibitors > 0);
	}
}

static void idle_new_inhibitor(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, idle_new_inhibitor);
	struct wlr_idle_inhibitor_v1 *wlr_inhibitor = data;
	struct vidow_idle_inhibitor *inhibitor = calloc(1, sizeof(*inhibitor));
	if (inhibitor == NULL) {
		return;
	}
	inhibitor->server = server;
	inhibitor->destroy.notify = idle_inhibitor_destroy;
	wl_signal_add(&wlr_inhibitor->events.destroy, &inhibitor->destroy);
	server->idle_inhibitors++;
	if (server->idle_notifier != NULL) {
		wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, true);
	}
	wlr_log(WLR_INFO, "空闲抑制: 有客户端要求别锁屏（在抑制 %d 个）",
		server->idle_inhibitors);
}

static void idle_init(struct vidow_server *server) {
	/* 两个 global。建不出来就当没这个协议：vidow 其余部分不依赖它们。 */
	/* ext-workspace-v1：shell（noctalia 的 dock 等）靠它看工作区。 */
	vidow_extworkspace_init(server);
	server->idle_notifier = wlr_idle_notifier_v1_create(server->display);
	server->idle_inhibit_mgr = wlr_idle_inhibit_v1_create(server->display);
	if (server->idle_notifier == NULL) {
		wlr_log(WLR_ERROR, "空闲通知: ext-idle-notify-v1 建不出来");
	}
	if (server->idle_inhibit_mgr == NULL) {
		wlr_log(WLR_ERROR, "空闲抑制: zwp-idle-inhibit-v1 建不出来");
	} else {
		server->idle_new_inhibitor.notify = idle_new_inhibitor;
		wl_signal_add(&server->idle_inhibit_mgr->events.new_inhibitor,
			&server->idle_new_inhibitor);
	}
	server->idle_new_input.notify = idle_new_input;
	wl_signal_add(&server->backend->events.new_input, &server->idle_new_input);
}

static void server_finish(struct vidow_server *server) {
	vidow_extworkspace_finish(server);
	if (server->session != NULL) {
		/* 会话销毁时会断言「没人还挂在我的 events.active 上」（和退出时那条
		 * xdg_shell 断言同一类，见文件头的说明）。 */
		wl_list_remove(&server->session_active.link);
	}
	if (server->vt_idle != NULL) {
		/* 排着队还没切的那个 VT 请求：退出时取消掉（不然回调会去碰已经拆了
		 * 一半的会话）。 */
		wl_event_source_remove(server->vt_idle);
		server->vt_idle = NULL;
		server->vt_pending = 0;
	}
	wl_list_remove(&server->new_output.link);
	wl_list_remove(&server->new_input.link);
	/* 空闲通知 / 空闲抑制（见上面的 idle_*）：inhibit manager 销毁时断言「没人还
	 * 挂在 events.new_inhibitor 上」。设备上那些活动监听器不用管 —— 设备自己
	 * 在 backend finish 时销毁，destroy 回调会把它们摘干净。 */
	wl_list_remove(&server->idle_new_input.link);
	if (server->idle_inhibit_mgr != NULL) {
		wl_list_remove(&server->idle_new_inhibitor.link);
	}
	wl_list_remove(&server->new_virtual_keyboard.link);
	if (server->virtual_pointer != NULL) {
		wl_list_remove(&server->new_virtual_pointer.link);
	}
	wl_list_remove(&server->new_toplevel.link);
	wl_list_remove(&server->new_popup.link);
	wl_list_remove(&server->new_layer_surface.link);
	wl_list_remove(&server->new_decoration.link);
	if (server->kde_decoration_manager != NULL) {
		wl_list_remove(&server->new_kde_decoration.link);
	}
	wl_list_remove(&server->new_text_input.link);
	wl_list_remove(&server->new_input_method.link);
	if (server->ftl_capture != NULL) {
		wl_list_remove(&server->new_capture_request.link);
	}
	if (server->xwayland != NULL) {
		wl_list_remove(&server->xwayland_ready.link);
		wl_list_remove(&server->xwayland_destroy.link);
		wl_list_remove(&server->new_xwayland_surface.link);
	}
	wl_list_remove(&server->cursor_motion.link);
	wl_list_remove(&server->cursor_motion_absolute.link);
	wl_list_remove(&server->cursor_button.link);
	wl_list_remove(&server->cursor_axis.link);
	wl_list_remove(&server->cursor_frame.link);
	wl_list_remove(&server->request_cursor.link);
	wl_list_remove(&server->request_set_selection.link);
	wl_list_remove(&server->request_set_primary_selection.link);
	/* 会话锁：manager 销毁时断言「没人还挂在我的 events.new_lock 上」（和上面
	 * xdg_shell 那条同一类）。锁对象自己在 destroy 回调里已经把三个监听器摘了。 */
	if (server->session_lock_mgr != NULL) {
		wl_list_remove(&server->new_lock.link);
	}
	if (server->lock_timeout != NULL) {
		wl_event_source_remove(server->lock_timeout);
		server->lock_timeout = NULL;
	}

	/* 事件源（定时器 / 信号 / FIFO）：loop 没了它们也活不成，主动撤掉干净。 */
	if (server->anim_timer != NULL) {
		wl_event_source_remove(server->anim_timer);
		server->anim_timer = NULL;
	}
	if (server->capture_timer != NULL) {
		wl_event_source_remove(server->capture_timer);
		server->capture_timer = NULL;
	}
	if (server->reload_sig != NULL) {
		wl_event_source_remove(server->reload_sig);
		server->reload_sig = NULL;
	}
	if (server->exec_timer != NULL) {
		wl_event_source_remove(server->exec_timer);
		server->exec_timer = NULL;
	}
	if (server->test_pipe != NULL) {
		wl_event_source_remove(server->test_pipe);
		server->test_pipe = NULL;
	}
	if (server->activation_timer != NULL) {
		wl_event_source_remove(server->activation_timer);
		server->activation_timer = NULL;
	}
}

int main(int argc, char *argv[]) {
	/* 只有参数，不需要合成器起得来：它们是给「第一次写配置」和「想看看都有
	 * 哪些键」用的。 */
	bool write_config = false;
	bool force = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--default-config") == 0) {
			/* 后面跟一个名字就是「打印那一份分类文件」；all = 全部拼一起
			 * （能当单文件配置用）；什么都不跟 = 入口 config.lua。 */
			const char *what = (i + 1 < argc && argv[i + 1][0] != '-')
				? argv[++i] : NULL;
			if (what == NULL) {
				vidow_config_default_lua(stdout);
				return 0;
			}
			if (strcmp(what, "all") == 0) {
				vidow_config_default_all_lua(stdout);
				return 0;
			}
			if (vidow_config_default_section_lua(stdout, what)) {
				return 0;
			}
			fprintf(stderr, "vidow: 没有 '%s' 这一份配置（能写：", what);
			for (int k = 0; k < vidow_config_section_count(); k++) {
				fprintf(stderr, "%s%s", k > 0 ? " / " : "",
					vidow_config_section_file(k));
			}
			fprintf(stderr, "；all = 全部拼一起）\n");
			return 2;
		}
		if (strcmp(argv[i], "--write-config") == 0) {
			write_config = true;
			continue;
		}
		if (strcmp(argv[i], "--force") == 0) {
			force = true;
			continue;
		}
		if (strcmp(argv[i], "--help") == 0 ||
				strcmp(argv[i], "-h") == 0) {
			fprintf(stderr,
				"用法: vidow [选项]\n"
				"  --default-config [名字]  打印默认配置：不带名字 = 配置"
				"入口（config.lua，只有 include 行）；all = 全部分类文件拼"
				"一起；名字 = 其中一份（如 02-decor.lua）\n"
				"  --write-config    把默认配置写进配置目录"
				"（$XDG_CONFIG_HOME/vidow/config.lua + configs/，"
				"已存在的不动）\n"
				"  --force           配合 --write-config：覆盖已有的那几份\n"
				"  --help            这个\n");
			return 0;
		}
		fprintf(stderr, "vidow: 不认识的参数 '%s'（--help 看用法）\n",
			argv[i]);
		return 2;
	}

	if (write_config) {
		char path[PATH_MAX];
		vidow_config_path(path, sizeof(path));
		char err[512];
		if (vidow_config_write_default(path, force, stdout, err,
				sizeof(err)) != 0) {
			fprintf(stderr, "vidow: %s\n", err);
			return 1;
		}
		return 0;
	}

	/* VIDOW_LOG_FILE：把日志同时写一份到文件。
	 *
	 * 为什么要这个：SDDM 启动会话时把 stderr 丢进 /dev/null（journalctl
	 * _COMM=vidow 是空的），而「渲染统计: … fps | 模糊 …ms/帧 | 提交 …ms/帧」
	 * 那行是判断卡顿的唯一直接证据 —— 没有它就只能在猜。 */
	const char *log_file = getenv("VIDOW_LOG_FILE");
	if (log_file != NULL && log_file[0] != '\0') {
		FILE *lf = fopen(log_file, "a");
		if (lf != NULL) {
			dup2(fileno(lf), STDERR_FILENO);
			dup2(fileno(lf), STDOUT_FILENO);
			setvbuf(lf, NULL, _IOLBF, 0);
		}
	}
	wlr_log_init(log_level_from_env(), NULL);

	struct vidow_server server = {0};
	wl_list_init(&server.outputs);
	wl_list_init(&server.views);
	wl_list_init(&server.popup_scenes);
	wl_list_init(&server.keyboards);
	wl_list_init(&server.layers);
	wl_list_init(&server.columns);
	wl_list_init(&server.workspaces);
	wl_list_init(&server.keybinds);
	wl_list_init(&server.rules);
	wl_list_init(&server.test_actions);
	wl_list_init(&server.output_memories);
	wl_list_init(&server.anims);
	wl_list_init(&server.closing);

	/* 默认值 + 环境变量。配置文件是 Lua，跑得比这里晚（见下面的
	 * vidow_script_load）—— 需要配置在更早生效的键由 vidow_config_sync()
	 * 在配置落地后对账。 */
	vidow_config_reset(&server.config);

	/* portal 靠 XDG_CURRENT_DESKTOP 选 <name>-portals.conf，而配置里启动的
	 * 客户端也继承它 —— 所以要在任何子进程之前设好。 */
	vidow_portal_desktop_env(&server.config);
	/* 顺手记下「原来是什么」：启动时会把环境推进 systemd 用户管理器，退出时
	 * 得放回去（见 portalconf.c）。这里问的是用户管理器，不是我们继承到的
	 * 环境 —— 从 tty 起来时后者本来就是空的。 */
	vidow_portal_env_snapshot();

	/* The custom-shader path drives raw GL, which only exists on the GLES2
	 * renderer. 配置跑得比渲染器创建晚，所以这里无条件设一次：setenv 的
	 * overwrite = 0 表示不覆盖 —— 用户自己在环境里指定了渲染器就听他的。 */
	setenv("WLR_RENDERER", "gles2", 0);

	server.display = wl_display_create();
	if (server.display == NULL) {
		wlr_log(WLR_ERROR, "failed to create wl_display");
		return 1;
	}

	/* 要会话指针（真机 tty 上 wlroots 会建一个，嵌套 / headless 是 NULL）：
	 * 切 VT 的键位要用它调 wlr_session_change_vt，不活动 / 回来也要有事件可听。 */
	server.backend = wlr_backend_autocreate(
		wl_display_get_event_loop(server.display), &server.session);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create backend");
		wl_display_destroy(server.display);
		return 1;
	}
	if (server.session != NULL) {
		server.session_active.notify = session_active;
		wl_signal_add(&server.session->events.active, &server.session_active);
		wlr_log(WLR_INFO, "会话: seat=%s %s（切 VT 用 Ctrl+Alt+F1..F12）",
			server.session->seat,
			server.session->active ? "现在是活动的" : "现在不活动");
	} else {
		wlr_log(WLR_INFO, "会话: 没有（嵌套 / headless 后端），切 VT 的键位不接管");
	}

	/* Ctrl+C / pkill 也要走正常退出：环境还原挂在退出路径上。 */
	int signals[] = { SIGTERM, SIGINT };
	for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		if (wl_event_loop_add_signal(wl_display_get_event_loop(server.display),
				signals[i], terminate_signal, &server) == NULL) {
			wlr_log(WLR_ERROR, "failed to install the SIG%d handler", signals[i]);
		}
	}

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create renderer");
		return 1;
	}
	wlr_renderer_init_wl_display(server.renderer, server.display);

	/* **显式同步**（wp_linux_drm_syncobj_v1）。
	 *
	 * 缺这个协议时 GPU 客户端（Chromium / Electron 全家）只能退回隐式同步：
	 * 合成器拿到 buffer 后要等驱动层面的 fence，帧就可能卡在 GPU 上。实测
	 * vidow 广播的 42 个 global 里没有它，而 shm 客户端（tools/frames.c）完全
	 * 不受影响 —— 和用户报的「Electron 明显、其他看不出来」对得上。
	 *
	 * 建不起来（软件渲染 / headless / 驱动不支持）就跳过：协议缺失只会退回
	 * 隐式同步，不会坏。 */
	{
		int drm_fd = wlr_renderer_get_drm_fd(server.renderer);
		if (drm_fd >= 0) {
			if (wlr_linux_drm_syncobj_manager_v1_create(server.display, 1,
					drm_fd) == NULL) {
				wlr_log(WLR_INFO, "显式同步没建起来：GPU 客户端继续走隐式同步");
			}
		}
	}

	/* **帧呈现时刻反馈**（wp_presentation）。Chromium 用它校正自己的帧时序；
	 * 缺了就只能靠 wl_surface.frame 估算。 */
	if (wlr_presentation_create(server.display, server.backend, 1) == NULL) {
		wlr_log(WLR_INFO, "wp_presentation 没建起来");
	}

	vidow_shader_init(&server);

	/* Async shell commands for the config: pipe + the display's own event
	 * loop, so a slow command never blocks a frame. */
	if (!vidow_exec_init(&server)) {
		return 1;
	}
	vidow_workspace_init(&server);
	/* 事件钩子（config.lua 的 vidow.on，见 hooks.c）：要在任何事件发出来之前
	 * 建好 —— 第一块屏「插上」（output-added）就发生在几行之后。 */
	vidow_hooks_init(&server);
	vidow_keybind_init(&server);
	vidow_gesture_init(&server);
	vidow_rule_init(&server);
	vidow_output_conf_init(&server);
	/* 没量过 X 屏幕之前先当它认这个属性（不夹缩放）：量到了再说，见 xwayland.c */
	server.xwayland_scale_ok = true;
	vidow_script_init(&server);
	/* 环境账本 + 从 systemd 用户管理器吸环境（见 env.c）。要在建 socket 之前：
	 * XDG_RUNTIME_DIR 这类要是我们这儿没有、用户管理器里有，吸进来才对得上。 */
	vidow_env_init(&server);
	vidow_env_absorb_systemd(&server);
	wl_list_init(&server.exec_once);

	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create allocator");
		return 1;
	}

	server.compositor = wlr_compositor_create(server.display, 6, server.renderer);
	wlr_subcompositor_create(server.display);
	/* 小数缩放：告诉客户端"这块屏是 1.70 倍"，客户端按 1.70 精确渲染，
	 * 而不是被迫取整到 2 再被合成器缩小（那样字会发虚）。viewporter 是它的
	 * 伴生协议，客户端用 wp_viewport 声明目标尺寸。 */
	wlr_fractional_scale_manager_v1_create(server.display, 1);
	wlr_viewporter_create(server.display);
	wlr_data_device_manager_create(server.display);
	wlr_primary_selection_v1_device_manager_create(server.display);

	server.output_layout = wlr_output_layout_create(server.display);
	/* waybar 等客户端要求 zxdg_output_manager_v1 才能拿到输出信息 */
	wlr_xdg_output_manager_v1_create(server.display, server.output_layout);
	/* 输出抓帧：grim / slurp / wf-recorder / OBS 走的就是下面这两个协议。
	 * 两份都是 **vidow 自己实现**的（src/capture.c / src/screencopy.c）：
	 * wlroots 那两份都是把屏幕上那一帧的缓冲直接拷给客户端，抓到的永远等于
	 * 屏幕，capture = "hide" 的窗口 / layer 藏不掉。自己出帧之后，抓帧图是
	 * vidow 另外渲染的一帧 —— 藏起来的东西屏幕上照常显示，只在抓帧里没有。 */
	server.capture_manager = vidow_capture_manager_create(server.display, &server);
	server.screencopy = vidow_screencopy_create(server.display, &server);
	/* 录屏：KDE 的录屏 portal（xdg-desktop-portal-kde）只认 KWin 这个私有协议，
	 * 拿不到它连 ScreenCast 接口都不注册。见 screencast.c。 */
	server.screencast = vidow_screencast_create(server.display, &server);
	/* 会话管线（约束 / dmabuf feedback / 帧回调）仍用 wlroots 那份，只有
	 * 「输出抓帧源」换成 vidow 的；窗口抓帧源（ext-foreign-toplevel-*）也是
	 * wlroots 的，所以那边还要靠 capturemask.c 的会话监听兜着。 */
	server.ext_capture = wlr_ext_image_copy_capture_manager_v1_create(
		server.display, 1);
	vidow_capturemask_init(&server);
	/* 老协议 wlr-export-dmabuf：老版 wf-recorder、OBS 的 wlrobs 插件 */
	wlr_export_dmabuf_manager_v1_create(server.display);
	/* zwp_linux_dmabuf_v1 由 renderer 建（wlr_renderer_init_wl_display），
	 * 别在这里再来一次：会多出一个同名 global。 */
	server.scene = wlr_scene_create();
	server.scene_layout =
		wlr_scene_attach_output_layout(server.scene, server.output_layout);
	/* Scene trees are stacked in creation order: bottom-most first. */
	/* 桌面底色（background_color）：比壁纸还底下 —— 壁纸是 background 层的客户端
	 * 画的，这个只是「谁都没画」时的兜底（见 layer.c 的 vidow_background_apply）。
	 * 颜色和尺寸由 vidow_background_apply 在配置跑完 / 输出变化时填。 */
	server.layer_backdrop = wlr_scene_tree_create(&server.scene->tree);
	server.backdrop = wlr_scene_rect_create(server.layer_backdrop, 1, 1,
		(const float[4]){ 0.0f, 0.0f, 0.0f, 0.0f });
	if (server.backdrop != NULL) {
		wlr_scene_node_set_enabled(&server.backdrop->node, false);
	}
	server.layer_background = wlr_scene_tree_create(&server.scene->tree);
	/* overview 的压暗遮罩：紧贴壁纸之上、窗口之下 */
	server.layer_overview_dim = wlr_scene_tree_create(&server.scene->tree);
	server.layer_bottom = wlr_scene_tree_create(&server.scene->tree);
	server.layer_toplevels = wlr_scene_tree_create(&server.scene->tree);
	/* X11 的弹出物（菜单/提示）单独一层：X11 里 override-redirect 窗口永远在所有
	 * 托管窗口之上，放在 layer_toplevels 里会被后开的窗口盖住。 */
	server.layer_popups = wlr_scene_tree_create(&server.scene->tree);
	server.layer_top = wlr_scene_tree_create(&server.scene->tree);
	server.layer_overlay = wlr_scene_tree_create(&server.scene->tree);
	/* 输入法候选框：压在所有东西之上，连 overlay 层也盖住 */
	server.layer_im = wlr_scene_tree_create(&server.scene->tree);
	/* 会话锁（ext-session-lock-v1，见 sessionlock.c）：建在 layer_im 之后 = 最顶。
	 * layer_lock_blank 是每块屏一块不透明黑（锁定期间垫底，把被关掉的桌面全盖住），
	 * layer_lock 放 lock surface。这两棵树平时关着 —— 它们只在锁定期间开。 */
	server.layer_lock_blank = wlr_scene_tree_create(&server.scene->tree);
	server.layer_lock = wlr_scene_tree_create(&server.scene->tree);
	if (server.layer_lock_blank != NULL) {
		wlr_scene_node_set_enabled(&server.layer_lock_blank->node, false);
	}
	if (server.layer_lock != NULL) {
		wlr_scene_node_set_enabled(&server.layer_lock->node, false);
	}

	/* 拖动落点的预览块（niri 的 insert hint）：建在窗口那一层里，平时关着。 */
	if (!vidow_drag_init(&server)) {
		return 1;
	}

	server.xdg_shell = wlr_xdg_shell_create(server.display, 6);

	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	wlr_xcursor_manager_load(server.cursor_mgr, 1);
	wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "left_ptr");

	server.seat = wlr_seat_create(server.display, "seat0");

	if (!vidow_output_init(&server) || !vidow_input_init(&server) ||
			!vidow_pointer_constraint_init(&server) ||
			!vidow_keyboard_grab_init(&server) ||
			!vidow_view_init(&server) || !vidow_layer_init(&server) ||
			!vidow_decoration_init(&server) ||
			!vidow_overview_init(&server) || !vidow_mru_init(&server) ||
			!vidow_im_init(&server) || !vidow_bgeffect_init(&server) ||
			!vidow_portal_init(&server) || !vidow_xwayland_init(&server) ||
			!vidow_sessionlock_init(&server) ||
			!vidow_clipboard_init(&server)) {
		return 1;
	}

	/* 空闲通知 / 空闲抑制两个 global（见上面的 idle_*）：要在 backend_start
	 * 之前起 —— 设备插上来的 new_input 信号从那时开始才有，活动监听得先挂着。 */
	idle_init(&server);

	const char *socket = wl_display_add_socket_auto(server.display);
	if (socket == NULL) {
		wlr_log(WLR_ERROR, "failed to add wayland socket");
		wlr_backend_destroy(server.backend);
		vidow_capturemask_finish(&server);
		wl_display_destroy(server.display);
		return 1;
	}
	/* Children spawned from keybindings must talk to us, not the host. This
	 * has to happen before the config runs: a config command like grim must
	 * see our socket, not the session we are nested in. */
	setenv("WAYLAND_DISPLAY", socket, 1);
	wlr_log(WLR_INFO, "vidow ready on WAYLAND_DISPLAY=%s", socket);
	/* 推给 systemd / dbus（见 env.c）：systemd 和 dbus 拉起来的应用（portal、
	 * systemd 服务、桌面文件激活的程序）环境从用户管理器拿，不推它们就看不到
	 * vidow 的 WAYLAND_DISPLAY。配置落地后还会再推一轮（那时用户 vidow.env{}
	 * 设的变量才算齐）。 */
	vidow_env_export_later(&server);

	/* IPC 在 socket 名定下来之后才起（默认路径带 WAYLAND_DISPLAY），但要在
	 * config.lua 之前：脚本里起的状态栏要能连上它。失败只打日志，不拦启动。 */
	vidow_ipc_init(&server);
	/* 子进程（状态栏 / 脚本）不用自己猜 socket 路径：vidowmsg 认这个变量。 */
	if (server.ipc_path[0] != '\0') {
		setenv("VIDOW_SOCKET", server.ipc_path, 1);
	}

	if (!wlr_backend_start(server.backend)) {
		wlr_log(WLR_ERROR, "failed to start backend");
		vidow_capturemask_finish(&server);
		wl_display_destroy(server.display);
		return 1;
	}

	/* 改完文件自动重载（不用再 kill -HUP）。放在配置加载**之前**：配置里
	 * vidow.include() 读进来的每一份（configs/ 下的那些文件）都要当场登记进来，改哪份
	 * 都得自动重载 —— 配置加载完再起来就晚了，那一轮 include 已经过去了。
	 * 初始化这一下记的是文件当前状态，不会被当成「变了」。 */
	vidow_configwatch_init(&server);

	/* 配置文件是 Lua，加载得最晚：它要能看见输出和 seat（配置里的
	 * vidow.workspace_create(name, output) / workspace_list() / ipc_path() 都得
	 * 有真东西可看），而配置里跑的命令（grim 之类）也要连得上我们自己。
	 * 带 vidow.exec() 的配置是分几步跑完的 —— 这里返回时可能还没跑完，
	 * 剩下的部分在合成器已经开始渲染之后落地。 */
	vidow_script_load(&server);

	wl_display_run(server.display);

	/* 从这里往下就是拆：wl_display_destroy() 会 finish 后端，键盘 / 输出那些
	 * finish 还会再发一轮事件进我们的监听器（那时 seat / 工作区链已经拆过一半），
	 * 所以先立牌子：退出路上不再处理设备事件（见 server.h 的注释）。 */
	server.shutting_down = true;

	/* 环境是启动时推进 systemd 用户管理器的，退出前放回去：宿主合成器那边
	 * 的后端才能重新连上它自己的 socket。放在最前面 —— 后面的清理不碰它。 */
	vidow_portal_env_restore();

	vidow_configwatch_finish(&server);
	vidow_capturemask_finish(&server);
	/* 输入设备的账本（见 input.c）：报事件用的那些，设备自己没了就没了 */
	/* 手指还拖着条带时退出：把手势状态清掉（不然 arrange 一直绕过「把焦点列滚回
	 * 可见区」那条，退出前最后一帧可能怪怪的）。 */
	vidow_gesture_finish(&server);
	vidow_input_finish(&server);
	vidow_ipc_finish(&server);
	vidow_clipboard_finish(&server);
	server_finish(&server);
	vidow_script_finish(&server);
	vidow_hooks_finish(&server);
	/* 改过的环境变量（vidow.env）还回原样：别把调用方的 shell 环境改脏 */
	vidow_env_finish(&server);
	vidow_exec_cancel_all(&server);
	wl_display_destroy_clients(server.display);
	wl_display_destroy(server.display);
	return 0;
}
