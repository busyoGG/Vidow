project('vidow', 'c',
  version: '0.1.0',
  license: 'MIT',
  default_options: ['c_std=c11', 'warning_level=2'])

cc = meson.get_compiler('c')

# Arch ships versioned wlroots packages: wlroots-0.19.pc / wlroots-0.20.pc
wlroots_dep = dependency('wlroots-0.20', version: '>=0.20.0')
xkbcommon_dep = dependency('xkbcommon')
wayland_server_dep = dependency('wayland-server')
math_dep = cc.find_library('m', required: false)
# 自定义 shader 走 GLES2：需要 EGL 和 GLES2 头/库
egl_dep = dependency('egl')
gles2_dep = dependency('glesv2')
pixman_dep = dependency('pixman-1')
# XWayland：wlr_xwayland_get_xwm_connection() 给的是 xcb 连接，诊断和 Xresources
# 都直接用 xcb 说话。
xcb_dep = dependency('xcb')
# 输出缓冲的格式常量（DRM_FORMAT_XRGB8888 / XRGB2101010）：vidow.output 的 depth。
drm_dep = dependency('libdrm')
# 录屏（KDE 的 portal 要合成器自己建 PipeWire 流，见 src/screencast.c）
pipewire_dep = dependency('libpipewire-0.3', version: '>=0.3.60')
# 剪贴板桥（clipboard.c）靠 XFixes 监听 X 侧的 owner 变化。
xcb_xfixes_dep = dependency('xcb-xfixes')
# 我们自己的 XWM（xwm_x11.c）给 X 的根窗口设光标要用 Render 扩展：
# 32 位 ARGB 的 pixmap → render picture → cursor（照 Hyprland 的
# CXWM::setCursor()）。
xcb_render_dep = dependency('xcb-render')
# libinput 设备配置（input.c 的 vidow_input_apply_libinput）：点按 / 反向滚动 /
# 加速曲线那些设备属性直接调 libinput。wlroots 把 libinput 放在 Requires.private
# 里，链接时不会自动带过来，所以自己声明一份（版本跟着 wlroots 0.20 的要求）。
libinput_dep = dependency('libinput', version: '>=1.19.0')
# 配置语言：Lua 5.4（Arch 的 lua54 包）。没有就退回系统默认 lua。
lua_dep = dependency('lua5.4', required: false)
if not lua_dep.found()
  lua_dep = dependency('lua', required: true)
endif

add_project_arguments(
  '-D_POSIX_C_SOURCE=200809L',
  '-DWLR_USE_UNSTABLE',
  language: 'c')

wayland_scanner = find_program('wayland-scanner')
wayland_client_dep = dependency('wayland-client')

# Arch 的 wlroots0.20 包没有安装 wlr-layer-shell-unstable-v1-protocol.h
# （wlroots 头文件里 #include 了它），所以从协议 XML 现场生成。
layer_shell_header = custom_target('wlr-layer-shell-unstable-v1-protocol.h',
  input: 'protocols/wlr-layer-shell-unstable-v1.xml',
  output: 'wlr-layer-shell-unstable-v1-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])

# ext-background-effect-v1：wlroots 0.20 没实现，服务端自己生成
bge_xml = 'protocols/ext-background-effect-v1.xml'
bgeffect_header = custom_target('ext-background-effect-v1-server-protocol.h',
  input: bge_xml,
  output: 'ext-background-effect-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
bgeffect_code = custom_target('ext-background-effect-v1-protocol.c',
  input: bge_xml,
  output: 'ext-background-effect-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

# ext-workspace-v1：wlroots 0.20 没实现，服务端自己生成（见 src/extworkspace.c）。
# 新 noctalia（C++ 原生 Wayland）判断「工作区占满没有」只能走这个协议 —— 它的文档：
# "Workspace integration works through compositor-native backends where needed,
#  or through ext-workspace-v1 on compositors that implement it"。
ws_xml = 'protocols/ext-workspace-v1.xml'
extws_server_header = custom_target('ext-workspace-v1-server-protocol.h',
  input: ws_xml,
  output: 'ext-workspace-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
extws_code = custom_target('ext-workspace-v1-protocol.c',
  input: ws_xml,
  output: 'ext-workspace-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

# xdg-activation-v1：服务端**自己实现**（见 src/activation.c —— wlroots 那份把
# serial 校验写死了，配不了「invalid serial 也认」），所以服务端头 + 代码自己生成。
xa_xml = 'protocols/xdg-activation-v1.xml'
xa_server_header = custom_target('xdg-activation-v1-server-protocol.h',
  input: xa_xml,
  output: 'xdg-activation-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
xa_server_code = custom_target('xdg-activation-v1-server-protocol.c',
  input: xa_xml,
  output: 'xdg-activation-v1-server-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

# 输出抓帧：两个协议都**自己实现**（见 src/capture.c 与 src/screencopy.c ——
# wlroots 那两份都是把屏幕上那一帧的缓冲直接拷给客户端，capture = "hide" 的窗口
# 藏不掉），所以服务端头 + 代码自己生成。
#   - ext-image-capture-source-v1：只要输出抓帧源管理器（ext_image_copy_capture
#     的会话 / 约束 / dmabuf feedback 仍用 wlroots 那份）。
#   - wlr-screencopy-unstable-v1：老协议，整份自己实现（wf-recorder 走它）。
eics_server_header = custom_target('ext-image-capture-source-v1-server-protocol.h',
  input: 'protocols/ext-image-capture-source-v1.xml',
  output: 'ext-image-capture-source-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
eics_server_code = custom_target('ext-image-capture-source-v1-server-protocol.c',
  input: 'protocols/ext-image-capture-source-v1.xml',
  output: 'ext-image-capture-source-v1-server-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
sc_xml = 'protocols/wlr-screencopy-unstable-v1.xml'
sc_server_header = custom_target('wlr-screencopy-unstable-v1-server-protocol.h',
  input: sc_xml,
  output: 'wlr-screencopy-unstable-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
sc_server_code = custom_target('wlr-screencopy-unstable-v1-server-protocol.c',
  input: sc_xml,
  output: 'wlr-screencopy-unstable-v1-server-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

# 录屏（KWin 私有协议 zkde_screencast_unstable_v1）：xdg-desktop-portal-kde 的
# ScreenCast 建在它上面 —— 拿不到这个 global，它连 org.freedesktop.impl.portal.ScreenCast
# 接口都不注册（实测：Hyprland 上也没有），所以非 KWin 的合成器上「KDE 的录屏」根本
# 不存在。协议要求**合成器自己建 PipeWire 流**并把 node 告诉客户端，所以这个模块
# 同时是个 PipeWire 生产者（见 src/screencast.c）。
zkde_xml = 'protocols/zkde-screencast-unstable-v1.xml'
zkde_server_header = custom_target('zkde-screencast-unstable-v1-server-protocol.h',
  input: zkde_xml,
  output: 'zkde-screencast-unstable-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
zkde_server_code = custom_target('zkde-screencast-unstable-v1-protocol.c',
  input: zkde_xml,
  output: 'zkde-screencast-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

# zwp_xwayland_keyboard_grab_manager_v1：wlroots 0.20 里没有（连头文件都没有），
# 服务端自己实现（见 src/keyboardgrab.c），所以头 + 代码都现场生成。
xkg_xml = 'protocols/xwayland-keyboard-grab-unstable-v1.xml'
xkg_server_header = custom_target('xwayland-keyboard-grab-unstable-v1-server-protocol.h',
  input: xkg_xml,
  output: 'xwayland-keyboard-grab-unstable-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
xkg_server_code = custom_target('xwayland-keyboard-grab-unstable-v1-protocol.c',
  input: xkg_xml,
  output: 'xwayland-keyboard-grab-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

sources = files(
  'src/main.c',
  'src/output.c',
  'src/outputconf.c',
  'src/view.c',
  'src/flipprobe.c',
  'src/xwm.c',
  'src/xwm_x11.c',
  'src/xwmscale.c',
  'src/input.c',
  'src/cursorfx.c',
  'src/capturemask.c',
  'src/layer.c',
  'src/float.c',
  'src/drag.c',
  'src/pointerconstraint.c',
  'src/keyboardgrab.c',
  'src/config.c',
  'src/scale.c',
  'src/hotplug.c',
  'src/workspace.c',
  'src/keybind.c',
  'src/gesture.c',
  'src/rule.c',
  'src/overview.c',
  'src/mru.c',
  'src/anim.c',
  'src/fx.c',
  'src/exec.c',
  'src/script.c',
  'src/configwatch.c',
  'src/fdwatch.c',
  'src/im.c',
  'src/bgeffect.c',
  'src/portal.c',
  'src/decoration.c',
  'src/activation.c',
  'src/json.c',
  'src/env.c',
  'src/hooks.c',
  'src/ipc.c',
  'src/clipboard.c',
  'src/ipcstate.c',
  'src/portalconf.c',
  'src/xwayland.c',
  'src/xdgoutput.c',
  'src/sessionlock.c',
  'src/capture.c',
  'src/jelly.c',
  'src/screencopy.c',
  'src/extworkspace.c',
  'src/screencast.c',
  'src/wincapture.c',
) + [layer_shell_header, bgeffect_header, xa_server_header, extws_server_header, extws_code,
     eics_server_header, sc_server_header, zkde_server_header,
     xkg_server_header]

# 测试工具：用虚拟指针模拟鼠标拖拽
vp_xml = 'protocols/wlr-virtual-pointer-unstable-v1.xml'
vp_header = custom_target('wlr-virtual-pointer-unstable-v1-client-protocol.h',
  input: vp_xml,
  output: 'wlr-virtual-pointer-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
vp_code = custom_target('wlr-virtual-pointer-unstable-v1-protocol.c',
  input: vp_xml,
  output: 'wlr-virtual-pointer-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

executable('vdrag',
  ['tools/vdrag.c', vp_header, vp_code],
  dependencies: [wayland_client_dep],
  include_directories: include_directories('.'),
  install: false)

# 测试工具：假输入法，验证 input-method-v2 中继
im_xml = 'protocols/input-method-unstable-v2.xml'
im_header = custom_target('input-method-unstable-v2-client-protocol.h',
  input: im_xml,
  output: 'input-method-unstable-v2-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
im_code = custom_target('input-method-unstable-v2-protocol.c',
  input: im_xml,
  output: 'input-method-unstable-v2-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

executable('fakeim',
  ['tools/fakeim.c', im_header, im_code],
  dependencies: [wayland_client_dep],
  include_directories: include_directories('.'),
  install: false)

# 测试工具：ext-background-effect 客户端（顺带要 xdg-shell 客户端协议）
xs_xml = 'protocols/xdg-shell.xml'
xs_header = custom_target('xdg-shell-client-protocol.h',
  input: xs_xml,
  output: 'xdg-shell-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
xs_code = custom_target('xdg-shell-protocol.c',
  input: xs_xml,
  output: 'xdg-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
bge_client_header = custom_target('ext-background-effect-v1-client-protocol.h',
  input: bge_xml,
  output: 'ext-background-effect-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

# 测试工具：帧回调计数器（只按合成器的许可出帧，打印每一帧）
# 测试工具：窗口录屏 / 预览的最小客户端（ext-image-copy-capture + 窗口截图源）
etl_xml = 'protocols/ext-foreign-toplevel-list-v1.xml'
etl_header = custom_target('ext-foreign-toplevel-list-v1-client-protocol.h',
  input: etl_xml,
  output: 'ext-foreign-toplevel-list-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
etl_code = custom_target('ext-foreign-toplevel-list-v1-protocol.c',
  input: etl_xml,
  output: 'ext-foreign-toplevel-list-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

eics_xml = 'protocols/ext-image-capture-source-v1.xml'
eics_header = custom_target('ext-image-capture-source-v1-client-protocol.h',
  input: eics_xml,
  output: 'ext-image-capture-source-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
eics_code = custom_target('ext-image-capture-source-v1-protocol.c',
  input: eics_xml,
  output: 'ext-image-capture-source-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

eicc_xml = 'protocols/ext-image-copy-capture-v1.xml'
eicc_header = custom_target('ext-image-copy-capture-v1-client-protocol.h',
  input: eicc_xml,
  output: 'ext-image-copy-capture-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
eicc_code = custom_target('ext-image-copy-capture-v1-protocol.c',
  input: eicc_xml,
  output: 'ext-image-copy-capture-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

executable('capture',
  ['tools/capture.c', etl_header, etl_code, eics_header, eics_code,
   eicc_header, eicc_code],
  dependencies: [wayland_client_dep],
  include_directories: include_directories('.'),
  install: false)

executable('frames',
  ['tools/frames.c', xs_header, xs_code],
  dependencies: [wayland_client_dep],
  include_directories: include_directories('.'),
  install: false)

# 测试工具：激活请求客户端（xdg-activation-v1：窗口自己请求被顶上来）
xa_header = custom_target('xdg-activation-v1-client-protocol.h',
  input: xa_xml,
  output: 'xdg-activation-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
xa_code = custom_target('xdg-activation-v1-protocol.c',
  input: xa_xml,
  output: 'xdg-activation-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

executable('activate',
  ['tools/activate.c', xs_header, xs_code, xa_header, xa_code],
  dependencies: [wayland_client_dep],
  include_directories: include_directories('.'),
  install: false)

# 测试工具：状态请求抢在第一次 commit 之前发（set_maximized / set_minimized /
# set_fullscreen）—— wlroots 对「给还没 initialized 的 surface 排 configure」用的
# 是 assert，合成器得自己挡住（Qt/KDE 恢复最大化窗口就是这么开窗的）
executable('preinit',
  ['tools/preinit.c', xs_header, xs_code],
  dependencies: [wayland_client_dep],
  include_directories: include_directories('.'),
  install: false)

# 测试工具：声明一个比表面还大的窗口几何（越界几何要按整块表面算）
executable('geowin',
  ['tools/geowin.c', xs_header, xs_code],
  dependencies: [wayland_client_dep],
  include_directories: include_directories('.'),
  install: false)

executable('bgblur',
  ['tools/bgblur.c', xs_header, xs_code, bge_client_header, bgeffect_code],
  dependencies: [wayland_client_dep],
  include_directories: include_directories('.'),
  install: false)

# 测试工具：xdg-foreign 导出端（portal 的 parent_window 从这儿来）
xfv1_xml = 'protocols/xdg-foreign-unstable-v1.xml'
xfv2_xml = 'protocols/xdg-foreign-unstable-v2.xml'
xfv1_header = custom_target('xdg-foreign-unstable-v1-client-protocol.h',
  input: xfv1_xml,
  output: 'xdg-foreign-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
xfv1_code = custom_target('xdg-foreign-unstable-v1-protocol.c',
  input: xfv1_xml,
  output: 'xdg-foreign-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
xfv2_header = custom_target('xdg-foreign-unstable-v2-client-protocol.h',
  input: xfv2_xml,
  output: 'xdg-foreign-unstable-v2-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
xfv2_code = custom_target('xdg-foreign-unstable-v2-protocol.c',
  input: xfv2_xml,
  output: 'xdg-foreign-unstable-v2-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

executable('foreign',
  ['tools/foreign.c', xs_header, xs_code, xfv1_header, xfv1_code,
   xfv2_header, xfv2_code],
  dependencies: [wayland_client_dep],
  include_directories: include_directories('.'),
  install: false)

# vidow 自己的 portal 后端（录屏选择器）：GTK3 画界面 + 一条自己的 wayland 连接
# 抓缩略图（wlr-screencopy）和要流（zkde_screencast）。这两个协议的服务端在 src/，
# 客户端侧的代码这里现生成 —— vidow-portal 就是个普通 wayland 客户端，和 KDE 那个
# 后端一样，只是自己人。
sc_client_header = custom_target('wlr-screencopy-unstable-v1-client-protocol.h',
  input: sc_xml,
  output: 'wlr-screencopy-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
sc_client_code = custom_target('wlr-screencopy-unstable-v1-client-protocol.c',
  input: sc_xml,
  output: 'wlr-screencopy-unstable-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
zkde_client_header = custom_target('zkde-screencast-unstable-v1-client-protocol.h',
  input: zkde_xml,
  output: 'zkde-screencast-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
zkde_client_code = custom_target('zkde-screencast-unstable-v1-client-protocol.c',
  input: zkde_xml,
  output: 'zkde-screencast-unstable-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
# 输出的**逻辑位置**（布局坐标）只在 xdg-output 里，wl_output.geometry 那对 x/y
# wlroots 一直发 0,0 —— 选择器要报「这块屏在布局里的哪儿」（录屏的 position、
# 框选区域的坐标都要它），所以这个协议也得给客户端生成一份。
xdo_xml = 'protocols/xdg-output-unstable-v1.xml'
xdo_client_header = custom_target('xdg-output-unstable-v1-client-protocol.h',
  input: xdo_xml,
  output: 'xdg-output-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
xdo_client_code = custom_target('xdg-output-unstable-v1-client-protocol.c',
  input: xdo_xml,
  output: 'xdg-output-unstable-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
# 合成器侧也要这个协议：src/xdgoutput.c 自己实现 zxdg_output_manager_v1，
# 只为 Xwayland 客户端把 logical_size 报成设备像素（见那里的长注释）。
# 代码表（marshal 函数）服务端和客户端共用同一份 xdo_client_code —— 
# private-code 生成的符号与用途无关，只需要一份，别生成两份（会重复定义）。
xdo_server_header = custom_target('xdg-output-unstable-v1-server-protocol.h',
  input: xdo_xml,
  output: 'xdg-output-unstable-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])

gtk3_dep = dependency('gtk+-3.0')
# 截图要写 PNG（portal 的 Screenshot 接口回的是个文件 URI）。
png_dep = dependency('libpng')
# 窗口列表（选窗口录屏）和按窗口抓帧（缩略图 / 截窗口）的客户端协议头已经给
# tools/capture.c 生成过了（etl_* / eics_* / eicc_*），这里直接复用。
executable('vidow-portal',
  ['tools/vidowportal.c', sc_client_header, sc_client_code,
   zkde_client_header, zkde_client_code, xdo_client_header, xdo_client_code,
   etl_header, etl_code, eics_header, eics_code, eicc_header, eicc_code],
  dependencies: [wayland_client_dep, gtk3_dep, png_dep, math_dep],
  include_directories: include_directories('.'),
  install: false)

# IPC 客户端：往 vidow 的 unix socket 上发一行 JSON，把应答打出来
# （纯 socket 客户端，不依赖 wayland，也不依赖 json.c —— 它只做漂亮的缩进）。
executable('vidowmsg',
  ['tools/vidowmsg.c'],
  install: false)

# 测试工具：当 screencast 流的消费者，把 SPA_META_Cursor 读出来（GStreamer 不解它）。
# 验证 pointer_mode=metadata 用：./build/cursorprobe <node-id>。
executable('cursorprobe',
  ['tools/cursorprobe.c'],
  dependencies: [pipewire_dep],
  install: false)

# 离线自检：JSON 写出 / 读入（纯函数，不开合成器）
executable('jsontest',
  ['tools/jsontest.c', 'src/json.c'],
  include_directories: include_directories('src'),
  install: false)

# 离线自检：混合 DPI 的缩放计算（纯函数，不开合成器）
executable('scaletest',
  ['tools/scaletest.c', 'src/scale.c'],
  dependencies: [math_dep],
  include_directories: include_directories('src'),
  install: false)

# 离线自检：热插拔记忆的纯逻辑（输出身份 / 记忆台账 / 挑接收的屏 / 稳定重排）
executable('hotplugtest',
  ['tools/hotplugtest.c', 'src/hotplug.c'],
  dependencies: [wayland_server_dep],
  include_directories: include_directories('src'),
  install: false)

# 测试工具：按绝对坐标造 X11 窗口（弹出物分类 / 定位 / 不许被布局搬走）
xcb_icccm_dep = dependency('xcb-icccm')
executable('xpopup',
  ['tools/xpopup.c'],
  dependencies: [xcb_dep, xcb_icccm_dep],
  install: false)

# 测试工具：X11 的 confine 抓取（Xwayland 会把它变成 zwp_confined_pointer_v1）
executable('xgrab',
  ['tools/xgrab.c'],
  dependencies: [xcb_dep],
  install: false)

# 测试工具：指针约束客户端（锁鼠标 / 夹光标 + 相对位移）
pc_xml = 'protocols/pointer-constraints-unstable-v1.xml'
pc_header = custom_target('pointer-constraints-unstable-v1-client-protocol.h',
  input: pc_xml,
  output: 'pointer-constraints-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
pc_code = custom_target('pointer-constraints-unstable-v1-protocol.c',
  input: pc_xml,
  output: 'pointer-constraints-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
rp_xml = 'protocols/relative-pointer-unstable-v1.xml'
rp_header = custom_target('relative-pointer-unstable-v1-client-protocol.h',
  input: rp_xml,
  output: 'relative-pointer-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
rp_code = custom_target('relative-pointer-unstable-v1-protocol.c',
  input: rp_xml,
  output: 'relative-pointer-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

executable('pointerlock',
  ['tools/pointerlock.c', pc_header, pc_code, rp_header, rp_code,
   xs_header, xs_code],
  dependencies: [wayland_client_dep],
  include_directories: include_directories('.'),
  install: false)

# 测试工具：会话锁客户端（ext-session-lock-v1）。本机一个锁屏客户端都没有
# （swaylock / waylock / gtklock / hyprlock / i3lock 全缺），所以自己写一个最小
# 的：lock → 每块屏 get_lock_surface → 收 configure 就 ack + 贴纯色 shm →
# 收 locked 打 LOCKED（测试脚本拿这行当同步点）。见 tools/locktest.c。
# XML 用系统的（wayland-protocols 1.49 有 staging/ext-session-lock/），不 vendor
# 进 protocols/；生成的头/代码也在 build 目录里，不进仓库。
wp_dep = dependency('wayland-protocols', version: '>= 1.27')
wp_datadir = wp_dep.get_variable(pkgconfig: 'pkgdatadir')
sl_xml = wp_datadir + '/staging/ext-session-lock/ext-session-lock-v1.xml'
sl_header = custom_target('ext-session-lock-v1-client-protocol.h',
  input: sl_xml,
  output: 'ext-session-lock-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
sl_code = custom_target('ext-session-lock-v1-protocol.c',
  input: sl_xml,
  output: 'ext-session-lock-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

executable('locktest',
  ['tools/locktest.c', sl_header, sl_code],
  dependencies: [wayland_client_dep],
  include_directories: include_directories('.'),
  install: false)

# 测试工具：键盘抑制客户端（zwp_keyboard_shortcuts_inhibit_manager_v1 —— 客户端
# 说「这窗口在的时候别吃我的快捷键」，虚拟机的窗口就这么要键）
ksi_xml = 'protocols/keyboard-shortcuts-inhibit-unstable-v1.xml'
ksi_header = custom_target('keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h',
  input: ksi_xml,
  output: 'keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
ksi_code = custom_target('keyboard-shortcuts-inhibit-unstable-v1-protocol.c',
  input: ksi_xml,
  output: 'keyboard-shortcuts-inhibit-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

executable('keyinhibit',
  ['tools/keyinhibit.c', ksi_header, ksi_code, xs_header, xs_code],
  dependencies: [wayland_client_dep],
  include_directories: include_directories('.'),
  install: false)

# 测试工具：X11 的键盘抓取（Xwayland 会把它变成 zwp_xwayland_keyboard_grab_v1）
executable('xkeygrab',
  ['tools/xkeygrab.c'],
  dependencies: [xcb_dep],
  install: false)

executable('vidow',
  # etl_code：ext-image-capture-source-v1 的服务端代码要引
  # ext_foreign_toplevel_handle_v1_interface（窗口抓帧源的参数类型），那个
  # interface 结构在这个协议的 private-code 里（libwlroots 没导出这些符号）。
  sources + [bgeffect_code, xa_server_code, eics_server_code, sc_server_code,
             etl_code, zkde_server_code, xkg_server_code,
             # xdg-output 服务端头 + 代码表：src/xdgoutput.c 自己实现
             # zxdg_output_manager_v1（只为 Xwayland 报设备像素）。
             xdo_server_header, xdo_client_code],
  dependencies: [wlroots_dep, xkbcommon_dep, wayland_server_dep, math_dep,
                 egl_dep, gles2_dep, pixman_dep, lua_dep, xcb_dep,
                 xcb_xfixes_dep, xcb_render_dep, drm_dep, pipewire_dep,
                 libinput_dep],
  include_directories: include_directories('.'),
  install: false)

# 示例 shader 装到 ~/.config/vidow/shaders/
install_data(
  'shaders/zoom.frag',
  'shaders/genie.frag',
  'shaders/wave.frag',
  'shaders/round.frag',
  install_dir: join_paths(get_option('prefix'), 'share/vidow/shaders'))

# portal 后端选择的示例（拷成 ~/.config/vidow/portals.conf）
install_data('portals.conf.example',
  install_dir: join_paths(get_option('prefix'), 'share/vidow'))
