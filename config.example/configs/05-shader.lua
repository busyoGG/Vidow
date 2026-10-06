-- 自定义着色器 —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/05-shader.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。

-- ================== 自定义着色器
-- 自定义 shader 总开关（需要 GLES2 渲染器）
shader_enabled = true
-- shader 目录，加载 <目录>/<名字>.frag
-- 默认跟着配置目录走（<配置目录>/shaders，$XDG_CONFIG_HOME/vidow/shaders 或 ~/.config/vidow/shaders）
-- shader_dir = "<配置目录>/shaders"   ← 要写死就自己换成绝对路径
-- 采样时翻转 y（自己写的 shader 上下反了才需要）
shader_flip_y = false
-- 窗口打开时用的 shader 名字，留空 = 不用
shader_open = ""
-- 窗口关闭时用的 shader 名字，留空 = 不用
shader_close = ""
-- 窗口空闲（一直开着不动画）时用的 shader 名字，留空 = 不用
shader_idle = ""
-- 这一族也能写成一张表，只写要改的项（shader = { enabled = true, dir = "<配置目录>/shaders", flip_y = false, open = "", close = "", idle = "" }）
--     不写表也行：shader = true（一个值管全部）
