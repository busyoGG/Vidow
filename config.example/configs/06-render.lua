-- 刷新与补帧 —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/06-render.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。

-- ================== 刷新与补帧
-- 滚出视口 / 被藏起来 / 最小化的窗口每秒补多少帧（0 = 不限），配合规则的 force_render
force_render_hz = 60
-- 在每块屏右上角显示实测帧率（debug 用；静止桌面应接近 0，顶到刷新率说明有空转）
debug_show_refresh = false
