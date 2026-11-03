# 脱离 wlroots + Vulkan 渲染（保留 OpenGL 回退与自定义 shader）

## 约束（用户明确要求）

- 完全不再依赖 wlroots（含后端、渲染器、协议实现、scene 图、工具函数）。
- 主渲染路径用 Vulkan，保留 OpenGL(ES) 回退。
- **保留自定义 shader 能力**（当家的装饰 / 模糊 / 扭曲 / 调色后处理）。
- **现有功能一条都不能丢**（见下面「功能基线」）。
- 分阶段可验证推进，每阶段结束 Vidow 都能正常起、跑回归。

## 功能基线（必须逐条守住）

1. 输出：DRM/KMS 多屏、缩放、倍率、位深(8/10bit)、VRR、色深切换、hotplug、headless、nested(Wayland/X11 后端)、direct scanout。
2. 渲染：场景合成、模糊金字塔(levels / strength / alpha_min / xray)、装饰(圆角/描边/阴影/噪点)、果冻/拖拽跨屏、overview、缩略图、光标(硬件/软件)、动画曲线。
3. 特性：HDR 输出(FP16 离屏 + PQ + dither)、调色 grade.*(KMS 3×1D LUT + shader)、色彩管理 wp_color_manager_v1、gamma control、screencopy / ext-image-copy / wlr-export-dmabuf / 录屏、XWayland、input-method、layer-shell、session-lock、idle、剪贴板、IPC。
4. 协议面：现在导出的所有全局（用 `wayland-info` 抓一份做基准）。

## 架构目标

```
Vidow
├─ 平台层（新）   backend: DRM/KMS · libinput · libseat · headless · nested
├─ 协议层（新）   自实现 wl_* / zwlr_* / wp_* / ext_* 协议
├─ 渲染抽象层     gpu.h  —— 一套接口，两套实现
│   ├─ gpu-vk.c   Vulkan（主）
│   └─ gpu-gl.c   GLES2（回退）
├─ 场景层（新）   自写 scene 图 / 损伤 / 帧调度
└─ fx 层（保留）  自定义 shader：SPIR-V 为源，GL 走 glslang 回落
```

### 渲染抽象层是成败关键

- 接口只暴露：`gpu_pass`、`gpu_texture`、`gpu_buffer`、`gpu_pipeline`、`gpu_shader`。
- 两套实现都要能跑同一份 fx 着色器。
- **着色器源统一用 Vulkan GLSL / SPIR-V**：Vulkan 直接用；GL 后端用 glslang 在构建期反编译回 GLSL（或用 `glslangValidator` 生成 GL 30 兼容源）。
- 不允许在 fx 层出现 `gl*` 裸调用（现在有 39 个不同 gl* 符号、fx-gl.c 2063 行）。

## Steps

- [x] 0. 摸底与基线：抓 `wayland-info` 全局清单、跑全套测试、记录每块屏的直通 / 帧率基线；把测试脚本归类进「必须过」清单。
- [ ] 1. 抽出渲染抽象层：新增 `src/gpu.h` + `gpu-gl.c`（把 fx-gl.c / fx.c / output.c / capture.c 的 GL 调用收进去），行为零变化，全套测试仍过。
- [ ] 2. 着色器资产化：把内联 GLSL 字符串提到 `shaders/*.vert/.frag`，构建期生成 SPIR-V + GL 回退源码；建立「同一份源，两个后端像素一致」的比对测试。
- [ ] 3. 写 `gpu-vk.c`（设备选择、分配器、命令缓冲、管线缓存落盘、dmabuf 导入导出、多趟 render pass）。
- [ ] 4. 把场景合成 / 模糊 / 装饰 / 后处理 / 抓帧逐块切到 `gpu.h`，用环境变量 `VIDOW_RENDERER=vulkan|gl` 切换，双后端跑同一套像素测试。
- [ ] 5. 自研平台层后端（先 DRM/KMS + libinput + libseat），复刻 wlroots DRM 行为：atomic commit、plane 分配、直通、page flip、VRR、色深、gamma/调色 LUT。
- [ ] 6. 自研场景图 + 帧调度 + 损伤跟踪，替掉 `wlr_scene`。
- [ ] 7. 逐协议自实现：wl_compositor/subcompositor、xdg-shell、layer-shell、XWayland、session-lock、input-method、idle、screencopy、ext-copy、dmabuf、ptr-constraints、decoration、gamma-control、color-management、foreign-toplevel…（先列清单，按依赖排序）。
- [ ] 8. 删除 wlroots 依赖，保留 `backend/` 兼容层一个版本周期做对照回归。
- [ ] 9. 收尾：文档 / changelog / README 对账，性能与直通对比报告。

## 基线（第 0 步已完成）

`docs/migration-baseline/`：117 个测试（PASS 80 / FAIL 34 / MANUAL 3）、49 个 Wayland
全局、渲染层规模（39 个 GL 符号 / fx-gl.c 2063 行）、已知红分类。34 条红里抽样 12 条
在旧提交 `5016b89` 上同样红，不是迁移引入的。

## 关键风险与对策

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| Vulkan 上游渲染器拒绝 3×1D 色彩转换 | 客户端 gamma / 夜间模式失效 | 自研渲染器时自己实现（这正是脱离的收益） |
| 驱动一致性（NVIDIA / Intel / AMD 三家） | 黑屏 / 花屏 | 每阶段在三家开 CI 式 headless 冒烟 |
| 自研 DRM 的 atomic / plane 逻辑复杂 | 直通丢失、闪烁 | 先复刻 wlroots 行为（可读其源码作参考），用现有 `test-scanout.sh` 把关 |
| 协议太多，回归面大 | 某个客户端坏了 | 每次只换一类协议，`wayland-info` + 实机客户端清单回归 |
| 双后端维护成本翻倍 | 长期负担 | `gpu.h` 接口面尽量小；GL 回退只在「无 Vulkan」时用 |

## 建议做事顺序（先拿收益，后拆地基）

1. 先把渲染抽象层 + Vulkan 渲染器做出来（第 0–4 步）——这是「vulkan + 保留 shader」的直接目标，且可独立验证。
2. 再做平台层与场景图（第 5–6 步）——脱离 wlroots 的真正成本所在。
3. 最后逐协议搬（第 7 步），但**协议层可以先于平台层并行推进**，因为协议实现彼此独立、风险可控。

## 需要你先定的三件事

1. 渲染器优先级：Vulkan 主 / GL 回退是吗？还是反过来？
2. 协议层自实现范围：是全量搬，还是只搬「现在真的在用」的那批？
3. 时间预算：是想在几个月内完成，还是可以分几个大版本？（决定第 7 步的粒度）