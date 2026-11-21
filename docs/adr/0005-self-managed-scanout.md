# ADR 0005：直通由自己管，判据换成「看不见」

状态：进行中（2026-11-20）

## 背景

现在直通靠 wlroots 的 `scene_entry_try_direct_scanout`：它要求这一帧的
render list **正好一项**，且这一项铺满整块输出。这条判据太严 —— 场景里还
可能挂着一堆**根本画不出像素**的节点（禁用、在屏外、全透明、被上层不透
明节点整个盖住），它们不会被看见，却让 render list 多出一项，直通直接
被判死。

## 决定

自己管直通，不走 wlroots 那条 `scene` 快路。

### 判据：只看「看得见什么」

`scanout_covers_output(out)`：

1. 遍历这块输出范围内的所有 buffer 节点（用输出盒求交，先剪枝）。
2. 跳过：`!node.enabled`、与输出盒无交集、`opacity == 0`、把残余可见
   面积算下来为 0 的节点。
3. 剩下的集合里必须**恰好一项**，且它是不透明、无 transform / 无缩放、
   和自己区块一致的 DMA-BUF。
4. 任何一层比它高的不透明节点只要露出一像素，就判定失败 —— 保守算：
   算不清就算「露出」。

### 流程：每块输出一台三态机

```
Composited ──(判据通过)──> ScanningOut
     ^                          │
     └────(任何阻塞位置位)──────┘
```

- 状态、目标缓冲、进入时刻都挂在 `struct vidow_output` 上，一屏一份，
  不散落在各个窗口上。
- 每帧只有一个决策点：`vidow_output_scanout_advance(out, state)`。
  「要不要直通」不在这里判，这里只执行已经算好的结论。
- 阻塞用位掩码 `blockers`：`BLOCK_LAYER` / `BLOCK_BLUR` /
  `BLOCK_ROUND` / `BLOCK_JELLY` / `BLOCK_POST` / `BLOCK_DAMAGE` /
  `BLOCK_CURSOR`。谁破坏直通，谁在产生那个效果的地方置位，**不再维护
  「允许清单」**（清单会随功能增加而漏）。
- 退出直通时按位打一行（`VIDOW_SCANOUT_TRACE`）：
  `scanout off: blockers=0x24（圆角|果冻）`。

### 可见性判据的细则（补）

「看得见」要按像素算，不按节点算：

1. **先用 alpha 量测**：每块 surface 缓存一个「可见覆盖盒」（复用 layer
   内容盒那套读回 + 退避，见 fx-layer.c）。覆盖盒为空、或与输出盒不相交
   → 这个节点对这块屏没有任何贡献，直接跳过。这样「全屏但只有一条
   内容」的 bar / dock 不会因为「全屏」就把直通判死。
2. **再用遮挡**：从高到低累加不透明覆盖面积，被完全盖住的节点同样跳过。
3. **剩下还有可见像素的节点**，就是真正的叠加层。这时分两级处理：
   - **一级（默认）**：直通退出，视频 + 叠加层合成一帧。行为正确，代价是
     全屏视频上只要有可见的 bar 就不再直通。
   - **二级（可选，「看得见也不阻塞」）**：把叠加层交给 DRM 叠加平面，
     视频留在主平面，两块都直通。需要在提交路径里自己做主 / 叠加平面分配，
     分配失败就退回一级。
4. 直通 trace 必须能回答「为什么没走二级 / 为什么退出」：
   「scanout off: blockers=0x24（圆角|果冻）」、
   「scanout plain: 叠加层 1 块（bar），平面分配失败」。

### 二级方案（DRM 叠加平面）的可达性：现在够不到

查了装上的头文件（`/usr/include/wlroots-0.20`）：

1. `wlr/types/wlr_output.h` 里 `struct wlr_output_state` **只有一个 `buffer`**
   （外加 `buffer_src_box` / `buffer_dst_box` / `tearing_page_flip`），没有
   「一组平面」。也就是说公共 API 一次只能交一块缓冲，wlroots 自己也不做
   多平面合成。
2. 私有结构（`struct wlr_drm_plane`、`wlr_drm_connector` 的平面表）在
   `backend/drm/drm.h`，而这个头**不安装**——`/usr/include/wlroots-0.20/
   wlr/backend/drm.h` 只有公共接口。
3. 剩下的路只有两条，都不便宜：
   - 自己开 DRM fd、绕过 wlroots 做原子提交：会和 wlroots 的原子状态互相
     打架（它每帧提交自己那份），不现实。
   - 给 wlroots 上游加 `wlr_output_state` 的多平面能力：那是另一个项目的
     工作量。

**结论**：一级（有可见叠加层就退出直通）是当前 API 下能做到的正确上限。
二级留作「等 wlroots 支持多平面」的待办，不写实现代码。

### 提交

- 进入：`wlr_output_commit_state(out, &state)`，state 里只放
  `WLR_OUTPUT_STATE_BUFFER` + 客户端的 `wlr_buffer`。
- 退出：立刻合成一帧完整内容（不能只补损伤 —— 直通期间屏上的像素不是
  我们画的，损伤账本对不上）。
- 直通期间所有 `wlr_scene_output` 的损伤记录要继续攒，用于退出那一帧。

## 后果

- 好处：场景里挂多少不可见节点都不影响直通；新增效果只要置位，不需要
  改判据。
- 代价：接管 commit 路径，需要自己处理 `frame_done` / `present` 事件、
  以及「退出后第一帧必须全屏重画」。
- 回退开关：`VIDOW_NO_SCANOUT=1` 仍然一律走合成。

## 后续补齐（2026-11-21）

四条从 niri / Hyprland 最新提交里抄来的缺口，都已落地：

### 1) 平面格式（YUV）缓冲不许直通

Hyprland `monitor: block direct scanout of yuv buffers`（2026-11）。它们照样
advertise yuv dmabuf，只是不进直通 —— 理由同上：平面格式得先经 shader 转成
RGB 才能跟我们的装饰合成。

实现在 `vidow_output_scanout_buffer()`（`src/fx.c`）：dmabuf 的 fourcc 走
**白名单**（ARGB/XRGB/ABGR/XBGR/RGBA/RGBX/BGRA/BGRX 8bit、RGB565/BGR565、
ARGB/XRGB/ABGR/XBGR 2101010），其余一律 `return NULL` 并在 DEBUG 记一行
`scanout off: … 缓冲格式 0x… 不能直通`。shm 缓冲不查（Wayland 的 shm 格式里
没有平面格式）。

测试：`test-scanout.sh` 第 6 段用 `VIDOW_TEST_SCANOUT_YUV=1`（测试钩子，把任意
缓冲当成平面格式）验「这道闸真的拦得住」—— 真拿 NV12 客户端来测，测试环境里
造不出来。

### 2) 离开直通那一帧换主交换链

Hyprland `monitor: update swapchain when leaving DS to avoid short flickers`
（`98b642e`）。直通时送上屏的是客户端那张缓冲：格式、修饰都可能跟我们自己的
交换链不一样（8bit 客户端 → 10bit/HDR 桌面是典型）。沿用旧交换链，第一帧会露
一次格式不符的闪烁。

实现：`src/output.c` 的渲染函数里记 `bool was_scanout = output->scanout_self;`，
直通块之后若 `was_scanout && !output->scanout_self` 就
`wlr_output_configure_primary_swapchain(output->wlr_output, &state, &output->wlr_output->swapchain)`。

### 3) 手势松手居中吸附 —— 做了又撤了

niri `layout/scrolling: Add center snap to view gesture`（2026-11-18）：手势拖完松手时让
焦点列停在可用区正中。

实现过一版（`src/gesture.c` 的 `view_snap()`：`fc->offset - (work_w - fc->width) / 2`，
夹在 `[0, 条带总宽 - 可用区宽]`），**用户明确不要**（2026-11-21「我不需要居中」），
已 revert（`f57128f`）。现在松手仍是「刚好让焦点列全部可见」的最小滚动量 ——
要居中请用 `layout { center-focused-column }` 那条配置，它是长期行为、不是手势特例。

### 4) `stop-cast`：按编号收掉一路录制

niri `ipc: cast tracking (CastKind / pw_node_id / stop-cast)`。现场的用法：录屏
客户端被强杀，portal 那一路没回收，桌面上一直挂着一路按帧抓屏（白烧 GPU），
以前只能重启合成器。

实现：
- `struct vidow_screencast_stream` 加 `uint64_t id`（单调递增）与 `pid_t pid`
  （`wl_client_get_credentials`，排「哪个程序在抓帧」用）。
- `unsigned vidow_screencast_stop(struct vidow_server *, uint64_t id)`：`id == 0`
  收全部；收之前给客户端发 `closed`。
- IPC：`screencast`（信息）与 `stop_cast` / `stop-cast`（`{"id": N}`，不写 = 全部），
  JSON 里多了 `id` / `pid`。

### 没做的两条，以及为什么

- niri `Add workaround for redrawing invisible screencasted windows`：**Vidow 早就
  有** `vidow_view_capture_held()`（`src/fx.c` 里 `(!view->scene_parked || vidow_view_capture_held(view))`），
  录屏中的窗口即便滚出视口也照常点亮渲染。
- Hyprland `815e3a8`（top layer 压全屏）与 `42a7574`（小 surface 按逻辑尺寸）：
  与直通无关，留给后续。
