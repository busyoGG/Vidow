# 基线期已知红（34 条）

`tests.tsv` 里剩下的 34 个 FAIL。**先别把它们当迁移的锅**：其中 12 个代表性问题在
迁移开始**之前**的提交 `5016b89`（调色功能还没进）上逐个重跑，**12/12 同样失败**：

```
test-binds test-blurinvisible test-blurlevel test-emptycol-nomap test-config
test-x11scale test-pump-throttle test-maximize test-cursor-shake test-idlewake
test-hooks test-sessionlock
```

结论：这批红是这台机器 + 当前 HEAD 的既有状态，不是脱 wlroots / Vulkan 的前置条件，
也不是调色改动引入的。迁移期间只要求「不让原本 PASS 的变红」，这 34 条单独排期。

## 分类

### A. 缺测试资产 / 外部环境（13）

| 测试 | 首条失败 | 初步原因 |
| --- | --- | --- |
| `test-gtk-menu-hover` | GTK 窗口没起来 | 依赖 `/tmp/gtk-menu.py`，这个文件不存在 |
| `test-screencast` | 没等到 KDE 的选择器对话框 | 需要 KDE portal + 真实选择器 |
| `test-portalenv` | 重启次数 1，期望 2 | 依赖 systemd 用户管理器 / portal 重启 |
| `test-clipboard` | `wl-paste` 读到空 | Wayland 剪贴板客户端环境 |
| `test-input-inject` | wev 没拿到 keymap | headless 座位没有键盘能力 |
| `test-dpi` | X11 GTK 对话框没被摆位 | Xwayland / GTK 环境 |
| `test-drag-follow` | X11 客户端一条指针位置都没收到 | 同上 |
| `test-lastoutput` | X 屏幕 0x0 时日志没说明白 | 同上 |
| `test-x11scale` | 布局里它是 300x300 | Xwayland 缩放 / 规则路径 |
| `test-xwayland-hidpi` | 缓冲 626x700 = 几何 311x348 × 2 | Xwayland HiDPI 告知 |
| `test-xwayland-popup` | menu 没摆到 200,300 | 弹出物 unconstrain |
| `test-config-include` | exec 的值没落地 | 配置 include + exec 组合 |
| `test-dmabuf-update` | 收尾还剩 1 个 vidow | 测试清理没杀干净 |

### B. 时序 / 性能断言（16）

这些判据是「重烘多少次」「几帧内完成」「唤醒几次」「动画第几帧」这类，headless
下帧节奏和真机不同，容易红。不代表功能坏了，但也别在迁移中引用它们做像素回归。

| 测试 | 首条失败 |
| --- | --- |
| `test-blurinvisible` | 半透明窗口的私有底衬只剩 0 次 |
| `test-blurlevel` | levels=6：金字塔是空的 |
| `test-blursmall` | A 从来没建过私有底衬 |
| `test-idlewake` | 空载 10 秒醒了 40 次 |
| `test-minimize` | 最小化之后停了（3 帧/2.5 秒） |
| `test-maximize` | 它那条列最大化了（期望 true，实际 false） |
| `test-mini` | 还被卡在屏里（右边缘 2550） |
| `test-mru` | 预览角没切（可能退回方角） |
| `test-overview` | overview_blur 时壁纸没糊开 |
| `test-scrollcull` | 没抓到动画（没有新的 move tween） |
| `test-force-render` | 日志里没有「强制渲染」 |
| `test-pump-throttle` | 补帧根本没启动 |
| `test-preinit` | 第一个 configure 里没有 maximized |
| `test-pointer-lock` | 客户端坐标跑到区域外了 |
| `test-cursor-shake` | 普通截图里有 921600 个非背景像素 |
| `test-config` | OPT 日志比 `vidow ready` 晚 ~300ms（测试自身的 grep 竞争） |

### C. 待查的功能问题（5）

| 测试 | 首条失败 | 备注 |
| --- | --- | --- |
| `test-hooks` | **段错误（核心已转储）** | 钩子抛错后合成器倒了；旧提交同样复现 |
| `test-sessionlock` | 锁定期间键位白名单没生效 | 旧提交同样复现 |
| `test-binds` | Mod+Q 把窗口关了 —— 还有内置键位？ | 旧提交同样复现 |
| `test-emptycol-nomap` | 探针窗口数不对（count=1） | 旧提交同样复现 |
| `test-clip-stretch` | 可见那一条里有黑边（32/496） | 裁剪后装饰拉伸 |

## 对迁移的含义

- **C 类里的 `test-hooks` 段错误要在迁移前修掉**：自研后端/场景图之后更难定位。
- A 类先补环境或改成显式跳过，别让它们长期混在红里。
- B 类不进「必须过」集合；需要性能判据时用真机会话，不用 headless 计数。
- 「必须过」= `tests.tsv` 里当前 PASS 的 80 条 + 修好后的 `test-config` /
  `test-defaultconfig`。
