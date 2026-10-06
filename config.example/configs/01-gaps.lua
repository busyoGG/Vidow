-- 间距（gap） —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/01-gaps.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。

-- ================== 间距（gap）
-- 屏幕边到窗口、以及窗口之间的间距（一个数全管）
gaps = 6
-- 单设上边距，-1 = 跟随 gaps（其余负值当 0）
gaps_top = -1
-- 单设下边距，-1 = 跟随 gaps（其余负值当 0）
gaps_bottom = -1
-- 单设左边距，-1 = 跟随 gaps（其余负值当 0）
gaps_left = -1
-- 单设右边距，-1 = 跟随 gaps（其余负值当 0）
gaps_right = -1
-- 单设窗口之间（列与列、同列上下）的间距，-1 = 跟随 gaps（其余负值当 0）
gaps_inner = -1
-- 这一族也能写成一张表，只写要改的项（gaps = { top = -1, bottom = -1, left = -1, right = -1, inner = -1 }）
--     不写表也行：gaps = 6（一个值管全部）
