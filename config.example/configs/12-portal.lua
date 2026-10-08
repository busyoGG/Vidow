-- xdg-desktop-portal —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/12-portal.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。

-- ================== xdg-desktop-portal
-- 把 XDG_CURRENT_DESKTOP 设成下面这个名字
portal_conf_sync = true
-- XDG_CURRENT_DESKTOP 的值（xdg-desktop-portal 按它选 <名字>-portals.conf）
portal_desktop = "Vidow"
-- 录屏 / 截图交给谁：auto（默认）/ vidow / kde / wlr / hyprland / none
portal_capture = "auto"
-- 这一族也能写成一张表，只写要改的项（portal = { conf_sync = true, desktop = "Vidow", capture = "auto" }）
