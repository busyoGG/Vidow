-- 工作区 —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/22-workspace.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。
-- 切工作区的规矩（照 niri 的 workspace-auto-back-and-forth）

-- ================== 工作区
-- 切到「当前就在的」那块工作区时改成切回上一个（niri 的 workspace-auto-back-and-forth）；默认 false = 什么都不做（以前的样子）
workspace_auto_back_and_forth = false
