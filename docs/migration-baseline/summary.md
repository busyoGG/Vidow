# 迁移基线（2026-11-03 18:43）

- 分支 / 提交：`wip/vulkan-dewlroots` / `d14bad9`
- 测试：117 个，PASS 80，FAIL 34，MANUAL 3
- 全局：`globals.txt`（49 个 interface）
- 着色器基线：见 `state-summary.txt` / `code-metrics.txt`
- 已知红分类：见 `known-failures.md`（34 条，其中 12 条抽样在旧提交 `5016b89` 上同样红）

## 必须守住的功能 ↔ 判据

| 功能 | 测试 |
| --- | --- |
| 直通 / 合成像素 | test-scanout.sh |
| 多屏 / 缩放 / 位深 / VRR | test-colordepth.sh · test-outputstate.sh · test-outputpos.sh · test-dpi.sh · test-mixdpi.sh |
| 模糊全家 | test-blur*.sh · test-layerblur.sh · test-dragblur-*.sh |
| 装饰 / 圆角 / 阴影 | test-corner.sh · test-openanim.sh · test-closeanim.sh |
| 果冻 / 拖拽 | test-jelly.sh · test-drag*.sh |
| 调色 / 色彩管理 | test-grade.sh · test-gradelut.sh |
| HDR | （没有独立测试，靠 test-colordepth 的位深断言 + 实机） |
| 抓帧 / 录屏 | test-capture-hide.sh · test-screencast.sh · test-portal*.sh · test-wincapture-*.sh |
| XWayland | test-xwayland-*.sh · test-x11*.sh · test-child-coords.sh |
| 会话 / 输入 | test-sessionlock.sh · test-pointer-lock.sh · test-keyboard-grab.sh · test-input-inject.sh |
| 场景 / 布局 | test-workspace.sh · test-ws-dynamic.sh · test-overview.sh · test-fullscreen-*.sh |

## 失败清单

| 测试 | 秒 |
| --- | ---: |
| test-binds.sh | 24 |
| test-blurinvisible.sh | 28 |
| test-blurlevel.sh | 10 |
| test-blursmall.sh | 18 |
| test-clipboard.sh | 49 |
| test-clip-stretch.sh | 6 |
| test-config-include.sh | 30 |
| test-config.sh | 5 |
| test-cursor-shake.sh | 35 |
| test-dmabuf-update.sh | 10 |
| test-dpi.sh | 56 |
| test-drag-follow.sh | 10 |
| test-emptycol-nomap.sh | 7 |
| test-force-render.sh | 58 |
| test-gtk-menu-hover.sh | 14 |
| test-hooks.sh | 18 |
| test-idlewake.sh | 24 |
| test-input-inject.sh | 14 |
| test-lastoutput.sh | 56 |
| test-maximize.sh | 28 |
| test-minimize.sh | 34 |
| test-mini.sh | 174 |
| test-mru.sh | 118 |
| test-overview.sh | 109 |
| test-pointer-lock.sh | 14 |
| test-portalenv.sh | 4 |
| test-preinit.sh | 17 |
| test-pump-throttle.sh | 5 |
| test-screencast.sh | 77 |
| test-scrollcull.sh | 36 |
| test-sessionlock.sh | 44 |
| test-x11scale.sh | 6 |
| test-xwayland-hidpi.sh | 16 |
| test-xwayland-popup.sh | 25 |

分类与初步原因见 `known-failures.md`。

