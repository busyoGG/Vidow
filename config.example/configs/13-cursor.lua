-- 指针 —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/13-cursor.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。

-- ================== 指针
-- 摇晃指针就放大一下（快速来回晃几下，指针变大一会儿，好找）
cursor_shake = true
-- 每一圈加多少倍：2 = 第一圈就到大一倍，之后每圈再 +1（2x → 3x → 4x …，线性加不是乘）。放大走硬件光标那条路（放得下就不进截图），放不下会被降级成软光标，所以实际倍数会先按硬件光标尺寸上限夹住
cursor_shake_scale = 2
-- 指针悬停到哪个窗口就聚焦哪个（niri 的 input { focus-follows-mouse }）；默认 false = 不跟，和以前一样
focus_follows_mouse = false
-- 悬停聚焦允许带出多少滚动：工作区宽度的百分比，照 niri 的 max-scroll-amount（"10%"）；"0%" = 只有已经整个在屏上的窗口才跟；留空 = 不限（niri 不写子项就是这个意思）
focus_follows_mouse_max_scroll_amount = ""
-- 键盘换焦点时把光标挪到新窗口上（niri 的 input { warp-mouse-to-focus }）；鼠标悬停聚焦不挪（光标本来就在那儿）。默认 false
warp_mouse_to_focus = false
-- 光标挪到哪，照 niri 的 mode：留空 = X / Y 分别判断（只往需要的那个轴挪）；center-xy = 光标在窗口外就挪到窗口正中；center-xy-always = 永远挪到正中
warp_mouse_to_focus_mode = ""
-- 打字的时候把光标藏起来，指针一动就回来（niri 的 cursor { hide-when-typing }）；默认 false = 不藏，和以前一样
hide_when_typing = false
-- 指针静止这么多毫秒就把光标藏起来，一动就回来（niri 的 cursor { hide-after-inactive-ms }）；0（默认）= 不藏
hide_after_inactive_ms = 0
-- 这一族也能写成一张表，只写要改的项（focus_follows_mouse = { max_scroll_amount = "" }）
--     不写表也行：focus_follows_mouse = false（一个值管全部）
-- 这一族也能写成一张表，只写要改的项（warp_mouse_to_focus = { mode = "" }）
--     不写表也行：warp_mouse_to_focus = false（一个值管全部）
