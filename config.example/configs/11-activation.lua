-- 窗口激活 —— vidow 配置的一类，由入口 config.lua 读进来：
--     vidow.include("configs/11-activation.lua")
--
-- 下面每行都是内置默认值：改哪行哪行才生效，删掉那行就回到默认值。

-- ================== 窗口激活
-- 激活令牌多久作废（秒），0 = 不过期
activation_token_timeout = 0
-- serial 校验不过的激活请求也理（默认不理）
honor_xdg_activation_with_invalid_serial = false
