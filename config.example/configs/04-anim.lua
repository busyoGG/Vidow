-- 开关动画 —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/04-anim.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。
-- 窗口 / layer 的开关与移动动画，以及拖窗口时的果冻效果（jelly）

-- ================== 开关动画
-- 窗口打开动画时长（毫秒，0 = 不动画）
window_open_ms = 180
-- 窗口关闭动画时长
window_close_ms = 140
-- 窗口移动 / 布局重排的动画时长
window_move_ms = 190
-- 列宽变化的过渡时长（cycle-column-width / 列最大化 / IPC 改宽；0 = 直接到位。拖动改宽不补间 —— 那个必须跟手）
column_width_ms = 200
-- 切工作区的过渡时长（旧的推开、新的从另一边进来；0 = 直接换）
workspace_switch_ms = 200
-- layer-shell（状态栏、通知、挂件）出现的动画时长
layer_open_ms = 200
-- layer-shell 消失的动画时长
layer_close_ms = 150
-- 缓动曲线：linear / out_cubic / out_quint / in_out_cubic / out_back / out_elastic，也可以自己给一条三次贝塞尔 "bezier 0.05 0.9 0.1 1.08"（x1 y1 x2 y2；x 在 0..1，y > 1 的那部分就是过冲幅度）
easing = "out_cubic"
-- 打开动画的起始透明度
open_alpha_from = 0
-- 打开动画的起始位移（像素）
open_slide_px = 22
-- 关闭动画的结束透明度（透明度按线性进度淡：缓动曲线只管位移和缩放，过冲的曲线喂给透明度会让它在动画头 1/4 就掉到 0）
close_alpha_to = 0
-- 关闭动画的结束位移（像素）
close_slide_px = 14
-- 打开动画的缓动曲线（默认 out_elastic：过冲一下再收住；写法同 easing，贝塞尔和 "out_back 0.2" 这种带幅度的也认）
open_easing = "out_elastic"
-- 关闭动画的缓动曲线（管位移 / 缩放；透明度不受它影响）
close_easing = "out_elastic"
-- 回弹幅度：out_back 冲过终点多少（0.10 = 10%，0 = 不过冲）。三条曲线都能用；曲线自己写了幅度的（easing = "out_back 0.2"）以曲线为准
back_overshoot = 0.1
-- 打开动画的起始缩放（1.0 = 不缩；0.82 = 弹一下）
open_scale_from = 0.82
-- 关闭动画的结束缩放（1.0 = 不缩）
close_scale_to = 0.82
-- 拖窗口时的果冻效果：抓点跟着光标走，窗口其余部分像一块布被拖着、松手后晃两下收住（要窗口有装饰节点，也就是 decor 或 shader 开着）
jelly = true
-- 果冻的形变倍率（1.0 = 默认，0.5 = 收敛些，0 = 不弯）
jelly_strength = 1
-- 这一族也能写成一张表，只写要改的项（anim = { open_ms = 180, close_ms = 140, move_ms = 190, column_width = 200, layer_open_ms = 200, layer_close_ms = 150, easing = "out_cubic", open_alpha_from = 0, open_slide_px = 22, close_alpha_to = 0, close_slide_px = 14, open_easing = "out_elastic", close_easing = "out_elastic", open_scale_from = 0.82, close_scale_to = 0.82, jelly = true, jelly_strength = 1 }）
