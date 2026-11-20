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
