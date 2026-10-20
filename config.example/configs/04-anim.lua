-- 开关动画 —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/04-anim.lua")
--
-- 动画配置只有两个大类：window 和 layer。
--   window = { … }  窗口（打开 / 关闭 / 移动 / 工作区切换 / 列宽）
--   layer  = { … }  layer-shell（状态栏、通知、挂件）
--
-- 每一类都能写 ms / easing 作为统一默认值，再写具体项覆盖：
--     window = { ms = 300, easing = "out_cubic", workspace_ms = 500 }
-- 也可以只写具体项，不写 ms / easing。
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。

-- ================== 窗口
window = {
	-- 统一时长 / 曲线：写了就铺到打开、关闭、移动、工作区、列宽；
	-- 下面具体项再覆盖。这里不写，保留各自的默认值。
	-- ms = 300,
	-- easing = "out_cubic",

	open_ms = 180,             -- 打开
	open_easing = "out_elastic",
	close_ms = 140,            -- 关闭
	close_easing = "out_elastic",
	move_ms = 190,             -- 移动 / 布局重排
	move_easing = "out_cubic",
	workspace_ms = 200,        -- 工作区切换
	workspace_easing = "out_cubic",
	column_width_ms = 200,     -- 列宽变化
	column_width_easing = "out_cubic",
}

-- ================== layer
layer = {
	-- ms = 200,              -- 统一时长（出现 / 消失）
	-- easing = "out_cubic",  -- 统一曲线

	open_ms = 200,             -- 出现
	open_easing = "out_cubic",
	close_ms = 150,            -- 消失
	close_easing = "out_cubic",
}

-- ================== 其它
-- 全局 easing 现在只给 window / layer 之外的动画用（overview、手势吸附等）。
easing = "out_cubic"

-- 打开 / 关闭动画的形状参数（window 类共用）
open_alpha_from = 0
open_slide_px = 22
close_alpha_to = 0
close_slide_px = 14
open_scale_from = 0.82
close_scale_to = 0.82

-- 回弹幅度：out_back 冲过终点多少（0.10 = 10%，0 = 不过冲）
back_overshoot = 0.1

-- 拖窗口时的果冻效果（要窗口有装饰节点，也就是 decor 或 shader 开着）
jelly = true
jelly_strength = 1
