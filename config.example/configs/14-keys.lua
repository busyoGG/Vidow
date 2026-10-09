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
vidow.bind("Mod+Shift+F", "maximize-column")
vidow.bind("Mod+R", "cycle-column-width")
vidow.bind("Mod+M", "minimize-window")
vidow.bind("Mod+Shift+M", "restore-window")
vidow.bind("Mod+O", "toggle-overview")
-- Alt+Tab：按住修饰键列出最近用过的窗口（一行预览），连点 Tab 换一个，松开就把
-- 焦点给选中的那个；Esc 取消。样式 / 排版见 configs/15-mru.lua。
vidow.bind("Alt+Tab", "next-window")
vidow.bind("Alt+Shift+Tab", "previous-window")
-- Alt+`（反引号）：只在**当前这个应用**的窗口之间切（niri 的 Alt+` 同此）
vidow.bind("Alt+grave", "next-window same-app")
-- 第三个参数 { inhibited = true }：焦点那个表面抑制着快捷键（虚拟机的窗口要原样
-- 收 Super / Alt+Tab，见 README 的「快捷键抑制」）时这条照样生效。退出这条留个
-- 逃生口，不然抑制着的窗口能把用户锁死在里面。
vidow.bind("Mod+Escape", "quit", { inhibited = true })

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

-- 窗口规则（vidow.rule{…}）：一条规则 = 匹配哪些窗口 + 对它们做什么
--
--     匹配字段（可叠加；一个都不写 = 匹配所有窗口）：
--         app_id     Wayland 的 app_id / X11 的 WM_CLASS 第一段
--         title      标题的子串
--         class      X11 的 WM_CLASS（Wayland 窗口没有这个）
--         namespace  写了它这条规则就管 layer surface（状态栏 / 通知），不是窗口
--     作用（**至少写一个**，都不写会在这一行报错）：
--         force_render = true        滚出视口 / 被藏起来 / 最小化了也照样给它补帧
--                                    （录屏、窗口预览要的是新画面；每秒几帧看
--                                     configs/06-render.lua 的 force_render_hz）
--         hz = 30                    只给这个窗口定补帧频率（0 = 不限）
--         focus_on_open = false      它开窗的时候不抢焦点（Electron 系那种看不见的
--                                    辅助窗口会开个窗口把自己顶上来）
--         focus_on_activate = false  别人请求激活它（点通知 / 任务栏按钮）时不理
--         honor_invalid_serial = true  它的激活请求即使令牌 serial 校验不过也照样理
--         x11_scale = 1              X11 缩放桥：1 = 这个客户端没按 GDK_SCALE 画，
--                                    缓冲区不除（只对 X11 窗口有意义）
--         capture = "hide"           截图 / 录屏里怎么出现："off"（默认，照常）/
--                                    "hide"（不出现）/ "mosaic"（打马赛克）
--         capture_mosaic = 24        马赛克的块边长（屏幕像素，默认 16，最小 2）；只在
--                                    capture = "mosaic" 时有用，单独写不算一个作用
--
--     vidow.rule{ app_id = "mpv", title = "预览", force_render = true }
--     vidow.rule{ class = "Steam", focus_on_open = false }
--     vidow.rule{ namespace = "notifications", capture = "hide" }
--     读回来：vidow.rules()（没管的字段是 nil：「没管它」和「管了、给了默认值」
--     是两回事）

-- vidow.* 的函数（配置就是一门 Lua，下面这些是 vidow 注册进去的，全在这一份里）
--
--     配置键：
--         vidow.get("gaps")                     读一个键（和 vidowmsg config 同一个值）
--         vidow.set("gaps", 12)                 运行时改一个键（不落盘）。写法同配置键：
--                                                vidow.set("border", { width = 0 }) 也认
--         vidow.include("configs/02-decor.lua")  就地读另一个文件（相对路径按当前文件
--                                                所在目录解析）
--     窗口规则：
--         vidow.rule{ … }                       加一条规则（字段见上）
--         vidow.rules()                         读回所有规则
--     工作区：
--         vidow.workspace_create("chat")         建一块具名工作区（永远在；第二个参数
--                                                指定哪块屏，比如 "DP-1"）
--         vidow.workspace_special("scratch")    建一块特殊工作区（跟焦点屏走）
--         vidow.workspace_list()                列出普通工作区（特殊工作区不在里面）
--         vidow.workspace_active()              当前这块屏的活动工作区（名字）
--         vidow.workspace_switch("chat")         切到那块具名工作区（无名工作区用动作串
--                                                "workspace 2"）
--         vidow.workspace_toggle_special("scratch")  开 / 关那块特殊工作区
--         vidow.workspace_move("chat")           把当前窗口搬到那块具名工作区
--     输出：
--         vidow.output{ … }                     声明一块屏（字段见 configs/09-output.lua）
--     跑命令与日志：
--         vidow.exec("命令")                     跑一条 shell 命令，拿它的 stdout（看着是同步
--                                                的，实际不阻塞渲染）；失败返回 nil, 原因
--         vidow.exec_status("命令")              要退出码时用它：返回 out, code, err
--         vidow.log("消息")                     往合成器日志打一行（VIDOW_LOG=debug 看得见）
--         vidow.ipc_path()                      当前 IPC socket 的路径（没开 IPC 时是 nil）

-- 指针手势（Mod+左键拖窗口 / Mod+右键缩放 / Mod+滚轮切列）不是键位，改不了键。
