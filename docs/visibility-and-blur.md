# 可见性（place）与模糊开销：2026-10-31 这批改动的结论

一天里修「UI 带出别的窗口内容（碎片）」引出来的整条线，最后落到三件事上：
**可见性判据只剩一份（place）**、**模糊只在真有内容的地方做**、**看不见的
客户端不参与渲染**。这里把判据、探针和实测数字写下来，下次排查先看这页。

---

## 1. place：可见性的唯一答案

`src/place.c`（`struct vidow_place`）＝ `home`（归属屏）+ `world`（世界框）+
`hits[]`（纯几何：和哪几块屏有交集）+ `resident` / `reason`（策略）。

- 平铺 / 全屏渲染 → 只认 `vidow_place_home_hit()`（不许溢出到隔壁屏）；
- 补帧 / 冻结 → 认任意一块（`vidow_place_any()`）；
- 浮窗 / 拖拽 / 弹窗 → 认全部 hits（跨屏浮窗每屏画自己那一半）。

判据只有一份，差别只在访问器。layer 也有对等的一份（`vidow_place_of_layer`）。

## 2. 排帧判据也走 place（6ad5f7c）

`view_commit_needs_frame()`：客户端交帧要不要叫醒渲染，只读 place——
`resident` / `UNMAPPED` / 还没建场景节点这三种排帧，其余（隐藏工作区 / 最小化 /
小窗图标态 / 滚出屏外）一律不排。变回可见时由 arrange 补帧。

开关：`/tmp/vidow-no-commit-skip` 退回「任何交帧都排帧」。

## 3. 模糊：抓的框就是画的框（490a2c7）

用户报的「UI 带出别的窗口内容」由三个毛病叠成，全部修掉：

1. **区域收窄只收窄了抓取**，着色器不知道。现在 `fx->blur_region` 把「真抓了
   哪一块」告诉着色器，外面不画。
2. **`blur_region_on` 不再一刀切关掉 alpha 剔除**——那只对没递 mask 的窗口 /
   弹窗成立；layer 递了 mask，继续按内容 alpha 剔。
3. **内容包围盒**（`layer_content_scan`，`/tmp/vidow-no-alpha-scan` 可关）：
   合成器自己读回面板缓冲，按 **alpha 通道**（不是「32 位非零」，ARGB 里
   alpha=0 的像素可以有非零 RGB）量出真画出来的那一块，区域按它收。
   读回在事件循环空闲时跑，只在客户端交过新帧（`content_dirty`）且距上次
   ≥200ms；结果没变不推帧（否则 gen_scene++ 让所有底衬作废，空闲也重画）。

另有一条独立的坑：layer 底衬里不许有面板自己（`backdrop_ctx.below_layer`，
按层号筛 buffer，**不开场景节点**——开关节点每帧记损伤，等于每帧整屏脏）。

### 实测（真机 DP-2 2x / DP-3 1x）

| 面板 | 改动前 | 改动后 | 省 |
|---|---|---|---|
| bar（2x） | 3840×876 | 3840×256 | −71% |
| dock（2x） | 3840×744 | 256×256 | −98% |

拖动场景：面板模糊现在只占 vidow CPU 约 0.6 个百分点（模糊全关 7.1% vs
开 11.0%，其中还含窗口自己的模糊）。

## 4. 损伤记账

arrange 收口用这一趟攒下的真实区域（`server->arrange_damage`，逐目标的
`pending_damage` 盒子集），没有变化就什么都不做。剩下的
`vidow_backdrop_damage(server, true)` 全量广播都改在「真·全变了」的调用点
（配置重载 / 热插拔 / 会话切换）。`VIDOW_NO_ARRANGE_REGION=1` 回退整屏。

## 5. 量「空闲在画什么」：render_stats（6219c96）

`touch /tmp/vidow-stats-on`（或 `VIDOW_RENDER_STATS=1`）后：

```
vidowmsg state | jq .render_stats
```

`frames / empty_frames（损伤 0 就出帧）/ lifted_frames（损伤 ≤ 整屏 5%）/
full_frames / damage_px / from_event…`。累计值，采样做差即得帧率。

实测（真机，装完 6ad5f7c）：9 秒 202 帧 ≈ 22 fps，**empty 占 99%**——
剩余的帧就是可见面板（bar 频谱/时钟）的合法交帧，合成器本身按需、无空转。
（改动前旧会话是 240 fps、损伤 0%。）

## 6. 日志（b46ea57）

高频路径全部降 DEBUG：`fdwatch: 接管 fd`（vshot 回放待机每 250ms 连一次
IPC；单 fd 一天 2000+ 行）、`x11: 新窗口 / 先回 configure`、
`xwm: create/destroy`（steamwebhelper 一类每秒成对建 1x1/5x5 又销毁）、
`drop-surface`。排查时 `VIDOW_LOG=debug` 全量都在。真会话日志 IO 归零。

## 7. 探针开关一览

| 文件 / 环境变量 | 作用 |
|---|---|
| `/tmp/vidow-stats-on` | 打开 render_stats 计数（运行时，不用重启） |
| `/tmp/vidow-no-commit-skip` | 退回「任何交帧都排帧」 |
| `/tmp/vidow-no-alpha-scan` | 跳过内容包围盒读回（区域回退到声明区域） |
| `/tmp/vidow-no-arrange-region` | arrange 收口回退整屏损伤 |
| `/tmp/vidow-place-check` | place 与旧判据对账探针（不一致打 PLACECHK） |
| `VIDOW_DMG_WHY=1` | 损伤从哪来 / LAYERBLUR / ALPHASCAN 探针 |
| `VIDOW_FRAME_WHY=1` / `VIDOW_COMMIT_WHY=1` | 谁在要帧 / 谁在交帧 |

## 8. 已知遗留

- 某客户端（steamwebhelper 一类）每秒成对建销 1x1/5x5 的 X 窗口——客户端
  行为，vidow 侧已确认无泄漏（fd 表稳定），日志已降噪。
- vshot 回放待机每 250ms 连一次 IPC 属设计行为，开销可忽略。
- 剩余的 empty 帧（22fps）来源未逐条定位，量级无害；要追用
  `from_event/from_watchdog` 计数。
