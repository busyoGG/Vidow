-- 桌面背景 —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/18-background.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。
-- 壁纸由 layer-shell 的 background 层客户端画（swaybg / swww / quickshell 都行，见 README「背景（壁纸）归谁画」）；这里只有没有壁纸客户端时垫在最底下的颜色

-- ================== 桌面背景
-- 桌面底色：没有任何壁纸客户端（layer-shell 的 background 层 —— swaybg / swww / shell 都算）时垫在最底下的颜色，有壁纸就看不见它
background_color = "#14161cff"
