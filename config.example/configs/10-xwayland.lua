-- XWayland —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/10-xwayland.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。

-- ================== XWayland
-- 开 XWayland（有人连 X 才启动服务）
xwayland = true
-- XWayland 的缩放：auto = 所有输出里最大的那个
xwayland_scale = "auto"
-- 给客户端设 XDG_SESSION_TYPE 这类 X11 环境变量
xwayland_env = true
-- 这一族也能写成一张表，只写要改的项（xwayland = { enabled = true, scale = "auto", env = true }）
--     不写表也行：xwayland = true（一个值管全部）
