-- 背景模糊 —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/03-blur.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。

-- ================== 背景模糊
-- 背景模糊总开关
blur_enabled = true
-- 平铺窗口也模糊身后的壁纸；关掉只模糊浮动窗口后面
blur_all = true
-- 重叠时也用整屏那份底衬（壁纸 + layer 那层），忽略别的窗口 —— 糊的是壁纸不是压着的那个窗口（Hyprland 的 blur:xray 同款）。省很多：不建私有底衬，重叠场景每帧从 N×8 趟降到 ~3 趟、私有底衬的显存全免
blur_xray = false
-- Kawase 模糊的层级数，越大越糊。每多一级 = 每个模糊元素每帧多两趟离屏渲染（实测 2560x1440 缩放 2、四个模糊元素：3 级 25 趟/帧、6.3% 单核；2 级 17 趟、5.8%；4 级 33 趟、6.7%），糊的半径约 blur_offset × (3×2^级数 − 5) 设备像素（offset=1.5 时 3 级 ≈29px、4 级 ≈65px）
blur_levels = 3
-- 模糊强度倍数
blur_strength = 1
-- 窗口内容透明度低于这个值的地方不参与模糊（省 GPU）
blur_alpha_min = 0.15
-- 模糊采样偏移，越大越散
blur_offset = 1.5
-- 这一族也能写成一张表，只写要改的项（blur = { enabled = true, all = true, xray = false, levels = 3, strength = 1, alpha_min = 0.15, offset = 1.5 }）
--     不写表也行：blur = true（一个值管全部）
