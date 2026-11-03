-- 刷新与补帧 —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/06-render.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。

-- ================== 刷新与补帧
-- 滚出视口 / 被藏起来 / 最小化的窗口每秒补多少帧（0 = 不限），配合规则的 force_render
force_render_hz = 60
-- 内部离屏缓冲的精度："auto"（默认，能用 FP16 就用）/ "8" / "10"（10 = FP16 缓冲，不是 10 位整数 —— 2101010 只有 2 位 alpha，柔和阴影和圆角会被量化）。只管装饰 / 模糊 / 底衬这些自己渲的缓冲，扫描输出位深是 vidow.output{ depth } 的事。组写法 color = { precision, linear_blend, linear_blur }
color_precision = "auto"
-- 装饰合成在线性光里做（先解码内容 / 底衬，混完再编码）。默认开：阴影和半透明边缘更透、更接近物理叠加（2026-11-02 真机 A/B 后拍板）。想要老观感就设 false。组写法 color.linear_blend
color_linear_blend = true
-- 背景模糊在线性光里算（每级解码 → 加权平均 → 编码回屏幕空间）。默认关：真机上肉眼几乎无差（窗口均值 +0.7），却要给每一级多算一对解码 / 编码；想要理论正确再打开。组写法 color.linear_blur
color_linear_blur = false
-- 输出端调色的饱和度（1 = 不变，0 = 灰度，>1 更艳）。整组写成 grade = { saturation, contrast, temperature, hue, brightness, gamma }
grade_saturation = 1
-- 输出端调色的对比度（1 = 不变，>1 更强，绕线性光 0.18 中灰）
grade_contrast = 1
-- 输出端调色的色温（开尔文，6500 = 不变；>6500 偏冷，<6500 偏暖；亮度保持不变）
grade_temperature = 6500
-- 输出端调色的色相旋转（度，0 = 不变，范围 -180 .. 180）
grade_hue = 0
-- 输出端调色的亮度倍率（1 = 不变，0 = 全黑）
grade_brightness = 1
-- 输出端调色的伽马（1 = 不变，>1 中间调更亮、<1 更暗）
grade_gamma = 1
-- 在每块屏右上角显示实测帧率（debug 用；静止桌面应接近 0，顶到刷新率说明有空转）
debug_show_refresh = false
