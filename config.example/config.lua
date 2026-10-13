-- vidow 的配置入口 —— 下面每一行 include 把一类配置读进来，就这一件事。
--
-- 一类一个文件，都在 configs/ 下。相对路径按**这个文件所在目录**解析（跟 C 的
-- #include "…" 一个意思），所以在 configs/ 里的文件互相 include 也一样好使。
-- include 的先后 = 键生效的先后（同一个键只有第一次写的算数），别打乱。
-- 想删掉某一类：把那一行 include 删掉就行（文件留着不碍事）。
--
-- 整份默认配置（每行都是内置默认值）：
--     vidow --default-config                 这个入口
--     vidow --default-config all             全部分类文件拼在一起（能当单文件配置用）
--     vidow --default-config 02-decor.lua    其中一份（也认 --default-config decor）
--     vidow --write-config                   一步写到配置目录（入口 + configs/）
--
-- 优先级：环境变量 VIDOW_<键名大写> > 配置文件 > 内置默认值。
-- 改哪一份都行，改完不用重启（config.lua 和 include 进来的文件都盯着）：
--     kill -HUP $(pgrep -x vidow)
-- 运行时改：vidowmsg set-config <键> <值>（不落盘）。

vidow.include("configs/01-gaps.lua")       -- 间距（gap）
vidow.include("configs/02-decor.lua")      -- 窗口装饰
vidow.include("configs/03-blur.lua")       -- 背景模糊
vidow.include("configs/04-anim.lua")       -- 开关动画
vidow.include("configs/05-shader.lua")     -- 自定义着色器
vidow.include("configs/06-render.lua")     -- 刷新与补帧
vidow.include("configs/07-clipboard.lua")  -- 剪贴板
vidow.include("configs/08-ipc.lua")        -- IPC
vidow.include("configs/09-output.lua")     -- 缩放与输出
vidow.include("configs/10-xwayland.lua")   -- XWayland
vidow.include("configs/11-activation.lua") -- 窗口激活
vidow.include("configs/12-portal.lua")     -- xdg-desktop-portal
vidow.include("configs/13-cursor.lua")     -- 指针
vidow.include("configs/14-keys.lua")       -- 键位 / 工作区 / 规则
vidow.include("configs/15-mru.lua")        -- 最近窗口（Alt+Tab）
vidow.include("configs/16-env.lua")        -- 会话环境 / 自启动 / 事件钩子
vidow.include("configs/17-overview.lua")   -- 全景（overview）
vidow.include("configs/18-background.lua") -- 桌面背景
vidow.include("configs/19-input.lua")      -- 输入设备（libinput）
vidow.include("configs/20-layout.lua")     -- 布局（列宽 / 居中）
