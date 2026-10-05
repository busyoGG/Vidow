#include "server.h"
#include "ipc.h"

#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
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
static void server_finish(struct vidow_server *server) {
	wl_list_remove(&server->new_output.link);
	wl_list_remove(&server->new_input.link);
	wl_list_remove(&server->new_virtual_keyboard.link);
	if (server->virtual_pointer != NULL) {
		wl_list_remove(&server->new_virtual_pointer.link);
	}
	wl_list_remove(&server->new_toplevel.link);
	wl_list_remove(&server->new_popup.link);
	wl_list_remove(&server->new_layer_surface.link);
	wl_list_remove(&server->new_decoration.link);
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
	/* 只有一个参数：打印完整默认配置。不需要合成器起得来 —— 它是给
	 * 「第一次写配置」和「想看看都有哪些键」用的。 */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--default-config") == 0) {
			vidow_config_default_lua(stdout);
			return 0;
		}
		if (strcmp(argv[i], "--help") == 0 ||
				strcmp(argv[i], "-h") == 0) {
			fprintf(stderr,
				"用法: vidow [选项]\n"
				"  --default-config  打印一份完整的默认配置（Lua），"
				"抄进 ~/.config/vidow/config.lua 即用\n"
				"  --help            这个\n");
			return 0;
		}
		fprintf(stderr, "vidow: 不认识的参数 '%s'（--help 看用法）\n",
			argv[i]);
		return 2;
	}

	wlr_log_init(log_level_from_env(), NULL);

	struct vidow_server server = {0};
	wl_list_init(&server.outputs);
	wl_list_init(&server.views);
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

	/* The custom-shader path drives raw GL, which only exists on the GLES2
	 * renderer. 配置跑得比渲染器创建晚，所以这里无条件设一次：setenv 的
	 * overwrite = 0 表示不覆盖 —— 用户自己在环境里指定了渲染器就听他的。 */
	setenv("WLR_RENDERER", "gles2", 0);

	server.display = wl_display_create();
	if (server.display == NULL) {
		wlr_log(WLR_ERROR, "failed to create wl_display");
		return 1;
	}

	server.backend = wlr_backend_autocreate(
		wl_display_get_event_loop(server.display), NULL);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create backend");
		wl_display_destroy(server.display);
		return 1;
	}

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create renderer");
		return 1;
	}
	wlr_renderer_init_wl_display(server.renderer, server.display);
	vidow_shader_init(&server);

	/* Async shell commands for the config: pipe + the display's own event
	 * loop, so a slow command never blocks a frame. */
	if (!vidow_exec_init(&server)) {
		return 1;
	}
	vidow_workspace_init(&server);
	vidow_keybind_init(&server);
	vidow_rule_init(&server);
	vidow_script_init(&server);

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
	/* Screen capture: grim / slurp / wf-recorder / OBS all bind this. */
	server.screencopy = wlr_screencopy_manager_v1_create(server.display);
	/* 新标准截图/录屏（ext-image-copy-capture）：新版的 grim / wf-recorder /
	 * OBS 走这条路，比 screencopy 多支持 dmabuf 目标缓冲和逐帧捕获。 */
	wlr_ext_output_image_capture_source_manager_v1_create(server.display, 1);
	wlr_ext_image_copy_capture_manager_v1_create(server.display, 1);
	/* 老协议 wlr-export-dmabuf：老版 wf-recorder、OBS 的 wlrobs 插件 */
	wlr_export_dmabuf_manager_v1_create(server.display);
	/* zwp_linux_dmabuf_v1 由 renderer 建（wlr_renderer_init_wl_display），
	 * 别在这里再来一次：会多出一个同名 global。 */
	server.scene = wlr_scene_create();
	server.scene_layout =
		wlr_scene_attach_output_layout(server.scene, server.output_layout);
	/* Scene trees are stacked in creation order: bottom-most first. */
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

	server.xdg_shell = wlr_xdg_shell_create(server.display, 6);

	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	wlr_xcursor_manager_load(server.cursor_mgr, 1);
	wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "left_ptr");

	server.seat = wlr_seat_create(server.display, "seat0");

	if (!vidow_output_init(&server) || !vidow_input_init(&server) ||
			!vidow_view_init(&server) || !vidow_layer_init(&server) ||
			!vidow_decoration_init(&server) ||
			!vidow_overview_init(&server) ||
			!vidow_im_init(&server) || !vidow_bgeffect_init(&server) ||
			!vidow_portal_init(&server) || !vidow_xwayland_init(&server) ||
			!vidow_clipboard_init(&server)) {
		return 1;
	}

	const char *socket = wl_display_add_socket_auto(server.display);
	if (socket == NULL) {
		wlr_log(WLR_ERROR, "failed to add wayland socket");
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.display);
		return 1;
	}
	/* Children spawned from keybindings must talk to us, not the host. This
	 * has to happen before the config runs: a config command like grim must
	 * see our socket, not the session we are nested in. */
	setenv("WAYLAND_DISPLAY", socket, 1);
	wlr_log(WLR_INFO, "vidow ready on WAYLAND_DISPLAY=%s", socket);

	/* IPC 在 socket 名定下来之后才起（默认路径带 WAYLAND_DISPLAY），但要在
	 * config.lua 之前：脚本里起的状态栏要能连上它。失败只打日志，不拦启动。 */
	vidow_ipc_init(&server);
	/* 子进程（状态栏 / 脚本）不用自己猜 socket 路径：vidowmsg 认这个变量。 */
	if (server.ipc_path[0] != '\0') {
		setenv("VIDOW_SOCKET", server.ipc_path, 1);
	}

	if (!wlr_backend_start(server.backend)) {
		wlr_log(WLR_ERROR, "failed to start backend");
		wl_display_destroy(server.display);
		return 1;
	}

	/* 配置文件是 Lua，加载得最晚：它要能看见输出和 seat（配置里的
	 * vidow.workspace_create(name, output) / workspace_list() / ipc_path() 都得
	 * 有真东西可看），而配置里跑的命令（grim 之类）也要连得上我们自己。
	 * 带 vidow.exec() 的配置是分几步跑完的 —— 这里返回时可能还没跑完，
	 * 剩下的部分在合成器已经开始渲染之后落地。 */
	vidow_script_load(&server);

	/* 改完文件自动重载（不用再 kill -HUP）。放在配置加载之后：初始化那一下
	 * 记的是文件当前状态，不该被当成「变了」。 */
	vidow_configwatch_init(&server);

	wl_display_run(server.display);

	vidow_configwatch_finish(&server);
	vidow_ipc_finish(&server);
	vidow_clipboard_finish(&server);
	server_finish(&server);
	vidow_script_finish(&server);
	vidow_exec_cancel_all(&server);
	wl_display_destroy_clients(server.display);
	wl_display_destroy(server.display);
	return 0;
}
