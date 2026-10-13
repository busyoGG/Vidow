-- 键盘（NumLock / 重复） —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/21-keyboard.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。
-- 键盘自己的参数：NumLock 起手点不点亮、按住键多久开始重复、每秒重复几次（这两个数传给客户端，自动重复是客户端自己做的），以及键位里的 Mod 是哪个键

-- ================== 键盘（NumLock / 重复）
-- 按住键多久开始自动重复（毫秒）。默认 600 —— 改前 input.c 里写死的值。这个数通过 wl_keyboard.repeat_info 报给客户端，**重复本身是客户端做的**（wlroots 0.20 不在合成器里重复）
repeat_delay = 600
-- 自动重复每秒几次（0 = 关掉重复）。默认 25 —— 改前 input.c 里写死的值
repeat_rate = 25
-- 启动时把 NumLock 点亮（niri 的 input { keyboard { numlock } }，niri 默认 true）；vidow 默认 false = 不碰键盘状态，和以前一样
numlock = false
-- 键位里写的 `Mod` 是哪个键：super（默认，= 改前行为）/ alt（niri 的 mod-key）。niri 还有个 mod-key-nested（嵌套里的 Mod 换另一个键），没做
mod_key = "super"
