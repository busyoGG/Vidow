-- IPC —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/08-ipc.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。

-- ================== IPC
-- 开 IPC socket（vidowmsg、状态栏、脚本靠它）
ipc = true
-- socket 路径，留空 = 按 WAYLAND_DISPLAY 自动取
ipc_socket = ""
-- IPC 能不能合成鼠标 / 键盘事件（move-cursor / pointer-button / pointer-scroll / send-key / type-text）：等于「谁连上 socket 谁就能操控整个桌面」。默认开，每次注入都打一条 info 日志
input_injection = true
-- 这一族也能写成一张表，只写要改的项（ipc = { enabled = true, socket = "" }）
--     不写表也行：ipc = true（一个值管全部）
