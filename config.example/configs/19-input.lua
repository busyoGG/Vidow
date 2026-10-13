-- 输入设备（libinput） —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/19-input.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。
-- 触控板 / 鼠标这些设备自己的设置（照 niri 的 input {} 那一节）。默认全是 "auto" = 不碰，用设备自己的默认；改了立刻套到已经连上的设备上，热插拔也照套

-- ================== 输入设备（libinput）
-- 点按触控板 = 左键（niri 的 input { touchpad { tap } }）；"auto"（默认）= 不碰，用设备自己的默认
tap = "auto"
-- 打字的时候临时关掉触控板（disable-while-typing）；"auto"（默认）= 不碰
dwt = "auto"
-- 反向滚动（手指往下滑、内容往上走）；"auto"（默认）= 不碰
natural_scroll = "auto"
-- 左手模式：左右键对调；"auto"（默认）= 不碰
left_handed = "auto"
-- 左右键同时按 = 中键；"auto"（默认）= 不碰
middle_emulation = "auto"
-- 点按拖拽时松开手指不立刻放手（libinput 的 tap drag lock）；"auto"（默认）= 不碰
drag_lock = "auto"
-- 指针加速曲线：adaptive（自适应）/ flat（不加速）；留空（默认）= 不碰，用设备自己的默认
accel_profile = ""
-- 指针加速强度，-1.0 .. 1.0（niri 的 accel-speed，0.0 = 不加速也不减速）；"auto"（默认）= 不碰
accel_speed = "auto"
-- 什么时候把指针移动当滚动：no-scroll / two-finger / edge / on-button-down（niri 同名的四个值）；留空（默认）= 不碰，用设备自己的默认
scroll_method = ""
-- 触控板的按键怎么划分：button-areas（整块按下去）/ clickfinger（几根手指 = 哪个键）；留空（默认）= 不碰
click_method = ""
-- 滚动倍率（niri 的 scroll-factor）：0.5 = 半速，负数反向。libinput 没有这个接口，是 vidow 自己在滚轮事件上乘的；1.0（默认）= 不缩放，跟以前一样
scroll_factor = 1
-- 这一族也能写成一张表，只写要改的项（touchpad = { tap = "auto", dwt = "auto", natural_scroll = "auto", left_handed = "auto", middle_emulation = "auto", drag_lock = "auto", accel_profile = "", accel_speed = "auto", scroll_method = "", click_method = "", scroll_factor = 1 }）
