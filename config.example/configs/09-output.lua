-- 缩放与输出 —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/09-output.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。
-- 缩放是一个键；每块屏的缩放 / 位置 / 模式用 vidow.output{…} 声明

-- ================== 缩放与输出
-- 所有输出的默认缩放：auto = 按 EDID 的 ppi 推，也可以写死 "1" / "1.5"；单块屏点名用 vidow.output{ name = …, scale = … }
scale = "auto"

-- 显示器（输出）是另一类写法：**一块屏一组** —— 缩放、摆在哪儿（逻辑坐标）、
-- 什么模式、开不开 VRR、几位色、方向、开不开，都是这块屏的事，写在一行里
-- （写几行就是几块屏；只写要管的字段，没写的 vidow 一概不动）：
--
--     vidow.output{ name = "DP-3", scale = 2, x = 0, y = 0 }
--     vidow.output{ name = "DP-2", scale = 2, x = 1920, y = 0, vrr = true, depth = 10 }
--     vidow.output{ name = "HDMI-A-1", enabled = false }
--
-- 模式有哪些先看 `vidowmsg outputs`。老写法（outputs / output_positions）已经
-- 没了，写了会在日志里喊一声。
