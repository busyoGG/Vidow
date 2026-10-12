-- 剪贴板 —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/07-clipboard.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。

-- ================== 剪贴板
-- 接管 X11(XWayland) ↔ Wayland 的剪贴板同步
clipboard = true
-- PRIMARY（选中即复制）也一起同步
clipboard_primary = true
-- 每种类型在内存里留的副本上限（字节），超了丢这个类型
clipboard_max_bytes = 33554432
-- 剪贴板历史留几条（0 = 不留历史；见 tools/vidowclip.py）
clipboard_history = 32
-- 剪贴板历史总共占多少字节，超了从最旧那条开始丢
clipboard_history_bytes = 67108864
-- 这一族也能写成一张表，只写要改的项（clipboard = { enabled = true, primary = true, max_bytes = 33554432, history = 32, history_bytes = 67108864 }）
--     不写表也行：clipboard = true（一个值管全部）
