-- 最近窗口（Alt+Tab） —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/15-mru.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。
-- Alt+Tab 的窗口切换器（niri 的 recent windows）：列哪些窗口、怎么排版、选中框长什么样。按住 Alt 期间输入是模态的（从按下 Alt 那一刻算起）：滚轮 / 方向键选上一个下一个（到头停住、不循环），别的键位 / 鼠标 / 手势都不透给窗口

-- ================== 最近窗口（Alt+Tab）
-- Alt+Tab 的窗口切换器（niri 的 recent windows，见 src/mru.c）：关掉就不记账，那两个动作也不认。面板开着期间输入是**模态**的 —— 从**按下 Alt**那一刻算起（不等面板显形）：键盘 / 鼠标 / 手势都不透给窗口，只认切换器自己的键（Esc / Enter / 方向键）
mru_enabled = true
-- 焦点要「待够」多久才算真的用过（毫秒）：键盘一路导航碰到的窗口只算路过，0 = 立刻记账
mru_debounce_ms = 750
-- 按住多久面板才显形（毫秒）：快速点一下 Alt+Tab 只想切窗口、不该整屏闪一下，0 = 立刻显形。按住期间滚轮 = 选中项 ±1、方向键 Left/Up = 前一个 / Right/Down = 后一个，**到头停住、不循环** —— 故意和 Alt+Tab 的「到头绕回」不一样，别当 bug 改回去
mru_open_delay_ms = 150
-- 列哪些窗口：workspace（默认，当前工作区）/ all（所有屏所有工作区）/ output（当前这块屏）
mru_scope = "workspace"
-- 排版：strip = 一行，装不下横向滚；grid = 不滚，按 mru_per_row 换行铺在屏幕里（装不下就整体缩小预览）
mru_layout = "strip"
-- grid 一行最多几个（窗口再多就换行）
mru_per_row = 5
-- 预览之间留多少（逻辑像素）
mru_gap = 16
-- grid 行与行之间留多少（逻辑像素）
mru_row_gap = 16
-- 预览最多缩到多少（0.5 = 缩一半，1 = 原尺寸）
mru_max_scale = 0.5
-- 预览高度上限（逻辑像素）：大屏上别让一个窗口占满整屏
mru_max_height = 480
-- 再挤也不能比它更小（grid 里窗口特别多时的下限）
mru_min_scale = 0.05
-- 切换器后面那层压暗的浓度（0 = 不压暗）
mru_backdrop_alpha = 0.6
-- 每一项背后那张卡片比预览大多少（逻辑像素）：预览到卡片边缘那圈。比 mru_card_radius 小时按半径算（不然窗口会把圆角盖掉）
mru_card_padding = 8
-- 卡片的圆角半径（逻辑像素，超椭圆 —— 和 portal 的选择器同一种圆角）。半径会把内边距顶到至少这么大，圆弧才不会被窗口盖住
mru_card_radius = 12
-- 卡片的底色（RRGGBB 或 RRGGBBAA）
mru_card_color = "#2f3336e6"
-- 卡片描边的宽度（逻辑像素，0 = 不描边）
mru_card_border = 1
-- 卡片描边的颜色；鼠标底下那张换成 mru_highlight_color（半透明）
mru_card_border_color = "#3b4045ff"
-- 鼠标底下那张的描边宽度（逻辑像素，0 = 不描边）；比选中那张淡一点
mru_hover_border = 2
-- 鼠标底下那张的描边颜色（默认 = 强调色半透明，所以比选中那张淡）
mru_hover_color = "#3daee980"
-- 选中的那张卡片的描边宽度（0 = 不画描边）
mru_highlight_border = 2
-- 强调色：选中那张卡片的描边、以及它的底色（底 = 卡片底混 12% 这个色）
mru_highlight_color = "#3daee9ff"
-- 这一族也能写成一张表，只写要改的项（mru = { enabled = true, debounce_ms = 750, open_delay_ms = 150, scope = "workspace", layout = "strip", per_row = 5, gap = 16, row_gap = 16, max_scale = 0.5, max_height = 480, min_scale = 0.05, backdrop_alpha = 0.6, card_padding = 8, card_radius = 12, card_color = "#2f3336e6", card_border = 1, card_border_color = "#3b4045ff", hover_border = 2, hover_color = "#3daee980", highlight_border = 2, highlight_color = "#3daee9ff" }）
--     不写表也行：mru = true（一个值管全部）
