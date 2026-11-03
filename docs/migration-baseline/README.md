# 迁移基线（脱 wlroots + Vulkan 渲染）

`docs/plan-dewlroots-vulkan.md` 第 0 步的产物。目的：在动架构之前，把「现在能做什么、
做到什么程度」量成可对照的数字，之后每一步都拿它比。

重新生成：

```sh
ninja -C build
./tools/baseline.sh            # 环境 + 全局 + 状态 + 117 个测试（串行，十几分钟）
./tools/baseline.sh --no-tests # 只抓环境 / 全局 / 状态
```

测试脚本大多用固定 `/tmp/vidow-*` 路径，必须串行；每个超时 180 秒。

## 文件

| 文件 | 内容 |
| --- | --- |
| `env.txt` | 内核、发行版、CPU / GPU、Mesa、wlroots、Vulkan 驱动、提交号 |
| `inventory.tsv` | 117 个测试脚本 + 第一行说明 + `auto` / `MANUAL` |
| `globals.txt` | headless Vidow 导出的全部 Wayland 全局（`wayland-info` 原文） |
| `globals-source.txt` | 源码里注册的全局 / 创建调用（第 7 步协议清单的起点） |
| `state.json` | `vidowmsg state` 全量（含 `render_stats`、`shaders`） |
| `state-summary.txt` | 从 `state.json` 抽的关键项 |
| `code-metrics.txt` | 渲染层规模：GL 符号、fx-gl.c 内联 GLSL、内建程序 |
| `tests.tsv` | 每个自动测试的结果（PASS / FAIL / TIMEOUT + 秒） |
| `failures/` | 只留失败 / 超时那几个的完整日志 |
| `summary.md` | 人看总表：计数、功能 ↔ 判据、失败清单 |
| `headless.log` | 抓全局那一次的 Vidow 日志 |

## 判据

- **回归**：同一台机器、同一提交族下，`tests.tsv` 里原本 PASS 的变成 FAIL。
- **基线期已知失败**：写进 `summary.md` 的失败清单，先判断是环境（缺依赖 / 要真人会话）
  还是真 bug；确认是环境后，改测试的跳过条件，别让它长期混在失败里。
- **直通**：`test-scanout.sh` 必须过；这是 Vulkan / 自研 DRM 阶段最容易被打破的一条。
- **像素**：模糊 / 装饰 / 调色 / 抓帧类测试都是逐像素比较，双后端阶段要两边都跑。

## 手工项（MANUAL）

| 脚本 | 为什么手动 |
| --- | --- |
| `test-keys.sh` | 要先用 `./run-nested.sh` 起嵌套会话，再用 wtype 按键 |
| `test-steam-fix.sh` | 要真实 Steam + 真机会话 |
| `test-steam-reopen-blackscreen.sh` | 同上 |

这些不进自动回归，但迁移到自研后端后要各跑一次。
