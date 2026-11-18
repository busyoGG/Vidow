# ADR 0004：果冻不改成着色器现算

- 状态：**否决**（2026-11-18）
- 背景：docs/adr/0002 量到「装饰离屏图每帧重烘 12 次」（busy 场景），其中一个起因是用户每帧都动的窗口（动画 / 果冻）会让整块装饰图失效。当时的想法是：让果冻不再读「烘好的装饰图」，直接把客户端内容贴图当源，圆角 / 边框 / 投影 / 底衬模糊全部在 mesh_blur 着色器里现算。

## 做了什么

1. vidow_fx_draw_mesh_blur 加参数 opacity / border_width / border_color，片元着色器里用 sd_round_box 现算圆角与边框环，合成顺序改成 阴影 < 边框 < 内容+模糊。
2. jelly_render 的源从「那块屏的装饰缓冲」换成客户端内容（vidow_fx_client_texture(view->scene_surface->buffer)）。
3. jelly_slot 里用 dec_content_w/h、dec_px_pad 代替原来的 struct wlr_buffer *src。

## 为什么否决

- 屏幕上窗口**不弯**：顶点数组带着弹簧位移（GLVERTS v0=72 last=680），弹簧也在动（maxox=12.7），但回读渲染目标得到的仍是直矩形。
- 关掉装饰节点、把客户端表面 opacity 压到 0、硬剪切 27px 绕开弹簧 —— **画面都不变**，说明看的那一层不是网格结果；多层的 z 序 / 坐标系在 wlroots 的 scene 结构里难以稳定控制。
- 果冻的形变依赖「装饰与内容在同一张图里」。HEAD 的果冻正是这样（slot->src = 装饰缓冲，UV 0..1 覆盖整张图），与 KWin 的 wobbly windows 同构：一张 pixmap + 网格顶点，所以一弯全弯。把装饰拆进着色器等于把这张图拆成两层，形变只作用在其中一层。

## 与 KWin 的差别（真正的开销来源）

| | 我们（HEAD） | KWin |
|---|---|---|
| 变形方式 | 一张图 + 网格 | 同 |
| 装饰位置 | 烘进同一张图 | 同（画进 window pixmap） |
| 那张图何时更新 | 内容一动就**整块重烘** | **按损伤增量**更新 |
| 画到哪 | 先画进窗口大小的离屏缓冲 | 直接画到屏幕 |

我们改不了「直接画到屏幕」（wlroots 的 scene 不给外部注入几何），能改的只有「重烘频率」。

## 顺带量到的天花板

busy 场景（6 窗、双屏、每帧重画）实测：装饰重烘 **12 次/帧**、**0.14 ms/帧**。整帧 16.6 ms，其中 jelly+blur+decor+commit 合计 0.31 ms —— 大头是 60 Hz 节流等待，不是合成。

所以「按损伤重烘」和「把每窗每帧的两次烘焙合成一次」两条路，天花板加起来不到 0.15 ms/帧（**整帧的 1%**），不值得冒回归风险去做。

## 结论

- 果冻保持 HEAD 做法（装饰烘在图里 + 网格）。已回退全部试验代码，test-jelly.sh / test-corner.sh / test-drag.sh 全绿。
- 这个方向不再投入；性能上真正值钱的仍是 docs/adr/0002 里的「装饰直画（draw-pass）」和「被遮挡跳过合成」。


## 后续（2026-11-18）：只做「那一行能改的」

ADR 说得很清楚，能改的只有「重烘频率」。KWin 的做法是 `Window::updateWindowPixmap`
按损伤增量更新，而且动画期间那张图基本不动。照这个思路做了一条最小改动：

- `vidow_view_anim_bake()` 里，果冻跑着的时候**冻住这个窗口那一份装饰图**：
  只要这次烘的屏（`fx->bake_output`）就是冻住的那块（`fx->jelly_frozen_out`），
  直接 return，不重烘。果冻收手（`vidow_jelly_end`）时解冻。
- 为什么安全：果冻期间窗口的尺寸 / 倍率 / 内容尺寸都不变（动的只有 `anim_off`，
  由 `view_anim_place` 摆节点），所以那张图逐像素不变；而内容一动 `bake_revision`
  就变，缓存判据必然不中 —— 原来每帧白烘一次。

实测（`tools/perf-scene.sh drag --secs 6`，同一台机、相邻两次）：

| | 帧 | 装饰重烘 | 重烘/帧 |
|---|---|---|---|
| 改动前 | 419 | 650 | 1.55 |
| 改动后 | 456 | 542 | 1.19 |

拖拽场景重烘降 23%。回归：test-jelly / test-drag / test-corner / test-titlebar-drag /
test-floatstack / test-drag-insert / test-drag-stall / test-tile-resize /
test-viewport-spring / test-wincapture-lock / test-scanout / test-overview 全部 0 FAIL。

「按损伤增量重烘」没有做：天花板 0.14 ms/帧，风险（损伤坐标要映射进装饰画布的
设备像素、双缓冲轮转时得保证只增量更新最新的那一份）远大于收益。
