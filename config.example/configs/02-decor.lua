-- 窗口装饰 —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/02-decor.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。
-- 圆角、边框、阴影、不透明度 —— 这一类的开关是 decor_enabled

-- ================== 窗口装饰
-- 圆角/边框/阴影这套装饰的总开关
decor_enabled = true
-- 让客户端别画自己的装饰（CSD）：开启后连只认 KDE 老协议的客户端（GTK3）也听得见 —— 客户端把标题栏一起收回去，窗口只剩 vidow 的边框
prefer_no_csd = false
-- 圆角半径（逻辑像素，0 = 直角）
corner_radius = 12
-- 边框宽度（0 = 不画边框）
border_width = 2
-- 未聚焦窗口的边框颜色
border_color = "#4d546bff"
-- 聚焦窗口的边框颜色
border_color_focus = "#7aa6f7ff"
-- 阴影半径（0 = 不画阴影）
shadow_radius = 8
-- 阴影颜色，alpha 管浓淡
shadow_color = "#0000008c"
-- 阴影横向偏移（像素）
shadow_offset_x = 0
-- 阴影纵向偏移（像素）
shadow_offset_y = 2
-- 窗口（含装饰）的不透明度，0.5 = 半透明
window_opacity = 1
-- 往装饰里掺的噪点强度，0 = 不加
noise = 0
-- 这一族也能写成一张表，只写要改的项（decor = { enabled = true, corner_radius = 12, opacity = 1, noise = 0 }）
--     不写表也行：decor = true（一个值管全部）
-- 这一族也能写成一张表，只写要改的项（border = { width = 2, color = "#4d546bff", color_focus = "#7aa6f7ff" }）
-- 这一族也能写成一张表，只写要改的项（shadow = { radius = 8, color = "#0000008c", offset_x = 0, offset_y = 2 }）
