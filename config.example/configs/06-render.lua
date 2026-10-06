-- 刷新与补帧 —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/06-render.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。

-- ================== 刷新与补帧
-- 滚出视口 / 被藏起来 / 最小化的窗口每秒补多少帧（0 = 不限），配合规则的 force_render
force_render_hz = 60
