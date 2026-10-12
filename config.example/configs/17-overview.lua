-- 全景（overview） —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/17-overview.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。
-- Mod+O 把整条列带缩到一屏里看全景；配的是壁纸之上、窗口之下那层遮罩

-- ================== 全景（overview）
-- 全景里压在壁纸上的那层遮罩的颜色（RRGGBB 或 RRGGBBAA）：alpha 就是压多暗，默认 #05050899（60%）；0 = 不压暗，壁纸原样，缩小的窗口直接浮在上面
overview_dim_color = "#05050899"
-- 全景里把壁纸糊掉（niri 那样）：整屏一张模糊的壁纸垫在遮罩下面。要blur_enabled 也开着（模糊那套是共用的）；拿不到底衬就退回只压暗
overview_blur = true
