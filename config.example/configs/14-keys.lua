-- 键位 / 工作区 / 规则 —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/14-keys.lua")
--
-- 这一份没有配置键，全是 vidow.* 的调用 —— 删掉一行就是不要它。
-- 键位是配置给的（合成器不内置），工作区与窗口规则也在这一份里

-- ================== 键位（默认一套，随便改 / 删）
--
-- 合成器**一个键位都不内置**：下面这些是默认配置给的，删掉就还给客户端，换成
-- 别的键也行。动作字符串见 README 的「动作」表；`spawn` 后面跟一条 shell 命令。
--
-- 窗口与列（H/J/K/L 挪焦点，加 Shift 挪列 / 挪窗口本身）
vidow.bind("Mod+Return", "spawn " .. (os.getenv("VIDOW_TERMINAL") or "kitty"))
vidow.bind("Mod+Q", "close-window")
vidow.bind("Mod+H", "focus-column left")
vidow.bind("Mod+L", "focus-column right")
vidow.bind("Mod+Shift+H", "move-column left")
vidow.bind("Mod+Shift+L", "move-column right")
vidow.bind("Mod+J", "focus-window down")
vidow.bind("Mod+K", "focus-window up")
vidow.bind("Mod+Shift+J", "move-window down")
vidow.bind("Mod+Shift+K", "move-window up")
vidow.bind("Mod+V", "toggle-floating")
vidow.bind("Mod+Shift+Space", "toggle-floating")
vidow.bind("Mod+F", "toggle-fullscreen")
vidow.bind("Mod+R", "cycle-column-width")
vidow.bind("Mod+M", "minimize-window")
vidow.bind("Mod+Shift+M", "restore-window")
vidow.bind("Mod+O", "toggle-overview")
vidow.bind("Mod+Escape", "quit")

-- 工作区（niri 那套：动态、按屏、按序号）
--
-- 每块屏各自一份纵向的工作区栈，序号 = 在这块屏上从上往下第几块。栈底永远是
-- 一块空的：窗口开在它上面，底下立刻再补一块；中间那些空的、没名字的，切走就
-- 自动消失。所以**不用预先建工作区**，直接切就行（序号越界会夹到最后一块）。
vidow.bind("Mod+U", "workspace down")
vidow.bind("Mod+I", "workspace up")
vidow.bind("Mod+Shift+U", "move down")
vidow.bind("Mod+Shift+I", "move up")
vidow.bind("Mod+Ctrl+U", "move-workspace down")
vidow.bind("Mod+Ctrl+I", "move-workspace up")
-- 数字键直跳（工作区是动态的：Mod+3 会夹到当前最后一块，用着用着就长出来了）
for i = 1, 9 do
	vidow.bind("Mod+" .. i, "workspace " .. i)
	vidow.bind("Mod+Shift+" .. i, "move " .. i)
end

-- 切 tty（只有真机、有会话时才接管；嵌套 / headless 里这组键交给宿主）
for i = 1, 12 do
	vidow.bind("Ctrl+Alt+F" .. i, "switch-vt " .. i)
end

-- 想要「永远在、空了也不消失」的工作区，就用具名工作区（niri 的 named
-- workspace），它在屏上的位置按声明顺序排，底部那块空的永远在最后：
--
--     vidow.workspace_create("chat")             -- 建一块叫 chat 的（永远在）
--     vidow.bind("Mod+C", "workspace chat")      -- 切过去
--     vidow.bind("Mod+Shift+C", "move chat")     -- 把当前窗口搬进去
--
-- 窗口规则也写在这一份里：
--     vidow.rule{ app_id = "mpv", title = "预览", force_render = true }
--
-- 指针手势（Mod+左键拖窗口 / Mod+右键缩放 / Mod+滚轮切列）不是键位，改不了键。
