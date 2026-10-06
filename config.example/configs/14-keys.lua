-- 工作区 / 键位 / 规则 —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/14-keys.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。
-- 这一份没有配置键，全是 vidow.bind() / vidow.workspace_create() / vidow.rule() 这类调用

-- ================== 工作区（niri 那套：动态、按屏、按序号）
--
-- 每块屏各自一份纵向的工作区栈，序号 = 在这块屏上从上往下第几块。栈底永远是
-- 一块空的：窗口开在它上面，底下立刻再补一块；中间那些空的、没名字的，切走就
-- 自动消失。所以**不用预先建工作区**，直接切就行（序号越界会夹到最后一块）。
--
-- 想要「永远在、空了也不消失」的工作区，就用具名工作区（niri 的 named
-- workspace），它在屏上的位置按声明顺序排，底部那块空的永远在最后：
--
--     vidow.workspace_create("chat")             -- 建一块叫 chat 的（永远在）
--     vidow.bind("Mod+C", "workspace chat")      -- 切过去
--     vidow.bind("Mod+Shift+C", "move chat")     -- 把当前窗口搬进去
--
-- 内置键位：Mod+U / Mod+I 上下切工作区，Mod+Shift+U/I 把窗口搬到上/下一块，
-- Mod+Ctrl+U/I 整块工作区上下重排。下面 18 行是「数字键直跳」的绑定，不想要
-- 就删掉（工作区是动态的，Mod+3 会夹到当前最后一块，用着用着就长出来了）。
--
-- 窗口规则 / 键位也写在这一份里：
--     vidow.rule{ app_id = "mpv", title = "预览", force_render = true }
--     vidow.bind("Mod+M", "minimize-window")   -- 动作字符串见 README 的动作表
for i = 1, 9 do
	vidow.bind("Mod+" .. i, "workspace " .. i)
	vidow.bind("Mod+Shift+" .. i, "move " .. i)
end
