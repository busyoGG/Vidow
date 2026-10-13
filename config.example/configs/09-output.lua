-- 缩放与输出 —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/09-output.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。
-- 缩放是一个键；每块屏的缩放 / 位置 / 模式用 vidow.output{…} 声明

-- ================== 缩放与输出
-- 所有输出的默认缩放：auto = 按 EDID 的 ppi 推，也可以写死 "1" / "1.5"；单块屏点名用 vidow.output{ name = …, scale = … }
scale = "auto"
-- 指针一动就把「关着的屏」打开（Hyprland 的 misc { mouse_move_enables_dpms }）；默认 false = 不唤醒（以前的样子：关掉的屏只有再发一条 vidowmsg output-power 才回来）
mouse_move_enables_dpms = false
-- 按一下键就把「关着的屏」打开（Hyprland 的 misc { key_press_enables_dpms }）；默认 false = 不唤醒
key_press_enables_dpms = false

-- 显示器（输出）是另一类写法：**一块屏一组** —— 缩放、摆在哪儿（逻辑坐标）、
-- 什么模式、开不开 VRR、几位色、方向、开不开，都是这块屏的事，写在一行里
-- （写几行就是几块屏；只写要管的字段，没写的 vidow 一概不动）：
--
--     vidow.output{ name = "DP-3", scale = 2, x = 0, y = 0 }
--     vidow.output{ name = "DP-2", scale = 2, x = 1920, y = 0,
--                   mode = "3840x2160@144", vrr = true, depth = 10 }
--     vidow.output{ name = "HDMI-A-1", enabled = false }
--
-- 能写的字段（只有 name 必写，其余不写就是「不管这一项」）：
--     name       连接器名（`vidowmsg outputs` 里那个，比如 DP-3）
--     enabled    这块屏开不开（false = 关掉，拔线那套记忆不受影响）
--     scale      "auto"（按 EDID 的 ppi 推）/ "off"（不缩放）/ 一个正数（2 = 两倍）
--     x, y       摆在布局的哪个逻辑坐标（写哪个算哪个；只写 x = 0 = 摆到最左边）
--     mode       "3840x2160@144" / "3840x2160"（刷新率不挑）/ "…@59.94"（小数也行）
--     vrr        true / false（自适应同步，屏不支持就忽略）
--     depth      位深，只能 8 或 10
--     transform  方向：normal / 90 / 180 / 270 / flipped / flipped-90 / flipped-180 /
--                flipped-270（也认 "0" 当 normal）
--
-- 模式有哪些先看 `vidowmsg outputs`。老写法（outputs / output_positions）已经
-- 没了，写了会在日志里喊一声。
