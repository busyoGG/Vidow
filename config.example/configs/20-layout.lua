-- 布局（列宽 / 居中） —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/20-layout.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。
-- 列宽预设、新列默认宽度、焦点列的居中策略（照 niri 的 layout {} 那一节）

-- ================== 布局（列宽 / 居中）
-- 焦点列在视口里怎么居中：never（默认，也是 vidow 一直以来的行为 —— 换焦点只滚「刚好让它整个露出来」的那点距离）/ always（永远摆到正中）/ on-overflow（跟上一个焦点列能一起完整放下就不动，放不下才居中）（niri 的 layout { center-focused-column }）
center_focused_column = never
-- 列宽预设：cycle-column-width（Mod+R）在这几档之间循环，每个数是「可用宽度的比例」（0.5 = 半宽）。默认 1/3、1/2、2/3 —— 原来写死在 view.c 的那三档（niri 的 layout { preset-column-widths } 里的 proportion）
preset_column_widths = { 0.3333333333, 0.5, 0.6666666667 }
-- 新列的默认宽度，比例（0.5 = 半宽）；窗口规则里写了 column_width 的窗口按规则走。默认 0.5 —— 原来 view.c 里那个 DEFAULT_COLUMN_WIDTH（niri 的 layout { default-column-width }）
default_column_width = 0.5
