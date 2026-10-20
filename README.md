# vidow

用 wlroots 0.20 写的最小可用窗口管理器原型（C / meson）。

- **滚动平铺（niri 式）**：窗口按列从左到右排布，视口横向滚动跟随焦点；
  一列可纵向堆叠多个窗口
- **浮动窗口**：固定位置浮动 + 跟随父窗口浮动两种模式，按客户端提示自动识别
- **工作区 / 特殊工作区**：工作区由配置自己建、键位自己绑（合成器不预设）；特殊工作区
  不绑屏、不进工作区列表，跟随当前聚焦的那块屏
- **焦点与激活**：窗口规则控制「这个应用开窗口抢不抢焦点」「别人请求激活它时理不理」；
  激活（点通知 / 任务栏 / X11 的 `_NET_ACTIVE_WINDOW`）先把窗口那块工作区端出来再给焦点
- **xdg-activation 自己实现**（`src/activation.c`）：令牌过期时间可配、serial 校验不过的
  请求也能配成「照样理」（niri 的 `honor-xdg-activation-with-invalid-serial`，vidow 同名
  选项）—— wlroots 那份把这个校验写死了，配不动
- **配置**：一个 Lua 5.4 入口（`~/.config/vidow/config.lua`）+ `configs/` 下按类别分的一份份
  —— 全局名就是配置键，入口用 `vidow.include("configs/01-gaps.lua")` 把各类读进来（也能
  全写在一份里）；需要环境信息时直接 `vidow.exec()` 跑 shell 拿返回值，不阻塞渲染；
  SIGHUP 热重载，改哪一份都自动重载
- **IPC**（`src/ipc.c` / `src/ipcstate.c`）：unix socket 上的一行一个 JSON 请求/应答。
  状态输出**尽量全**（窗口的标题/坐标/尺寸/列/工作区/输出/规则、layer surface、
  工作区、输出与模式、配置全量、键位、规则、激活令牌、动画、输入法、光标…），
  操作**尽量全**（焦点/工作区/浮动/全屏/尺寸/位置/列宽、显示器开关/缩放/方向/模式/位置、
  运行时改配置、重载、overview、退出…），还有事件订阅（窗口开关/移动/焦点、
  layer 上屏、工作区切换、输出变化、配置重载）。客户端是 `vidowmsg`
- **混合 DPI**：每块屏按 EDID 反推缩放（`ppi/96`），客户端和装饰管线都按设备像素渲染；
  X11 走 XWayland，全局缩放 = `ceil(最密那块屏)`
- **XWayland**：真实 X11 程序能跑，SSD 与 Wayland 同一条管线（同样的圆角 / 边框 /
  阴影 / 模糊 / 动画 / 焦点色），平铺与浮动都支持，`xwayland_shell_v1` 只对 Xwayland 自己开放
- **剪贴板桥**（`src/clipboard.c`）：自己接管的 X11 ↔ Wayland 剪贴板 / PRIMARY 同步，
  **不看焦点**（wlroots 那份只在 X11 窗口聚焦时才同步）、两边各留一份内存副本（源应用退出
  之后照样能粘贴）、INCR 双向支持；开关与上限都在配置里（`clipboard` / `clipboard_primary` /
  `clipboard_max_bytes`）；带**历史**（`clipboard_history` 条数 + `clipboard_history_bytes`
  字节预算，复制过的都能翻回来，文件复制连 `uri-list` + `gnome-copied-files` 一起留着）和
  一个 GUI（`tools/vidowclip.py`，GTK3）
- layer-shell：背景/底层/顶层/覆盖四层，支持 exclusive zone（顶栏占位）与键盘独占
- 输入：键盘（xkb + 快捷键）、指针（移动/点击/滚轮/拖动）、虚拟键盘与虚拟指针（可自动化测试）
- 输出：热插拔、`request_state` 尺寸变更后自动重排、`wl_output` + `zxdg_output_manager_v1`
- 协议：wl_compositor、wl_subcompositor、xdg-shell、zxdg-decoration-v1、wlr-layer-shell、wl_data_device、
  primary-selection、pointer-constraints（锁鼠标 / 夹光标）、relative-pointer（相对位移，
  Xwayland 的 XInput2 raw motion 靠它）、keyboard-shortcuts-inhibit（客户端说「别吃我的
  快捷键」：虚拟机的窗口要原样收 Super / Alt+Tab）、xwayland-keyboard-grab（X11 的
  `XGrabKeyboard`，wlroots 没实现，服务端自己写的）、virtual-keyboard、virtual-pointer、
  linux-dmabuf、wlr-screencopy、ext-image-copy-capture、wlr-export-dmabuf
  （grim / slurp / wf-recorder / OBS）
- 输入法：text-input-v3 + input-method-v2（fcitx5 / ibus），候选框由合成器渲染摆位
- 背景效果：ext-background-effect-v1（客户端自己指定哪块要模糊）
- 渲染：`wlr_scene` 场景图（损伤跟踪 + 硬件光标）

## 布局模型

```
视口（工作区，已扣掉顶栏等 exclusive zone）
┌──────────────┬──────────────┬──────────────┐
│  列1 (50%)   │  列2 (50%)   │  列3 (50%)   │  ← 超出视口，横向滚动
│ ┌──────────┐ │ ┌──────────┐ │              │
│ │  窗口 A  │ │ │  窗口 C  │ │              │
│ ├──────────┤ │ └──────────┘ │              │
│ │  窗口 B  │ │              │              │
│ └──────────┘ │              │              │
└──────────────┴──────────────┴──────────────┘
```

- 新窗口在**焦点列的右侧**新开一列
- 焦点移动时，视口按「最小滚动量」保证焦点列完整可见
- 列宽默认占工作区 50%，可按列循环切换 1/3 → 1/2 → 2/3（**默认那三档，可配**：
  `preset_column_widths` / `default_column_width`，见编号 261）；比例分的是**连 gap 一起算**的
  那块区域（窗口 = 那块区域 − 一个 gap），所以几条列的比例加起来 = 1 时正好铺满可用区 ——
  两条 50% 不会被中间的 gap 挤出屏外（见「间距（gap）」一节）
- 列可以**最大化**（`Mod+Shift+F`）：整列撑满工作区宽度，再按一次还原成**原来那个比例**
  （不是写死的 50%）；客户端 `set_maximized` 也映射到它（vidow 没有窗口级最大化 —— 列是
  布局的最小单位），见「列最大化」一节
- 窗口在列内纵向均分高度

### 配置键与配置族

一个概念一个键：键是平的（`gaps_inner` / `border_width`），但一族配置可以写成一个键 +
一张表，表里只写要改的项：

```lua
gaps   = 10                                  -- 一个值管全部
gaps   = { top = 30, inner = 20 }            -- 只盖上边距和窗口之间，其余跟随
border = { width = 0, color = "#2a2e3aff" }  -- 不用去写 border_width / border_color
anim   = { open_ms = 220, easing = "out_quint" }
```

表写法和铺平是一个意思（表里每项展开成对应的平铺键），平铺键照旧能用 —— 环境变量
`VIDOW_<键名大写>`、`vidowmsg set-config <键> <值>`、老配置都走平铺键。表里写错项名、
或者给只认表的族（`border` / `shadow` / `anim` / `portal`）写一个光秃秃的值，都会在
`config.lua` 那一行当场报错并列出能写哪些项 —— 不会静默生效。

| 族 | 表里能写的项 | 一个值管全部 |
| --- | --- | --- |
| `gaps` | `top` `bottom` `left` `right` `inner` | `gaps = 10` |
| `decor` | `enabled` `corner_radius` `opacity` `noise` | `decor = false` |
| `border` | `width` `color` `color_focus` | —（光写一个值没法解释） |
| `shadow` | `radius` `color` `offset_x` `offset_y` | — |
| `blur` | `enabled` `all` `xray` `levels` `strength` `alpha_min` `offset` | `blur = false` |
| `anim` | `open_ms` `close_ms` `move_ms` `column_width` `layer_open_ms` `layer_close_ms` `easing` `open_easing` `close_easing` `open_alpha_from` `open_slide_px` `open_scale_from` `close_alpha_to` `close_slide_px` `close_scale_to` `jelly` `jelly_strength` | — |
| `shader` | `enabled` `dir` `flip_y` `open` `close` `idle` | `shader = false` |
| `clipboard` | `enabled` `primary` `max_bytes` | `clipboard = false` |
| `ipc` | `enabled` `socket` | `ipc = true` |
| `xwayland` | `enabled` `scale` `env` | `xwayland = false` |
| `portal` | `conf_sync` `desktop` `capture` | — |

显示器（输出）是另一种写法：**一块屏一组**，用 `vidow.output{…}` 声明，因为「哪块屏、
按几倍、摆在哪」是同一件事 —— 摊成平铺键（老的 `outputs` + `output_positions`）要在
两处各写一遍屏的名字，改一块屏改两行、只改一行还看不出来。写法见
[混合 DPI 与 XWayland](#混合-dpi-与-xwayland)；`outputs` / `output_positions` 这两个
老键名会在日志里喊一声「现在用 vidow.output{…}」，不静默失效。

一个 Lua 那边的坑：**同一个键在一个文件里写两遍，只有第一遍生效** —— 第一遍之后它就是个
普通的全局变量了，第二遍不再经过配置层（`vidow.set("gaps", 12)` 每次都能用）。

完整的默认配置（每个键都写着它自己的默认值，抄一份就能改）：

```bash
./build/vidow --write-config          # 入口 + configs/ 一整套写进 ~/.config/vidow（已有不动）
./build/vidow --default-config        # 只打入口（只有 include 行）
./build/vidow --default-config all    # 全部分类文件拼在一起（include 就是就地展开，能当单文件配置用）
./build/vidow --default-config 02-decor.lua   # 其中一份（也认 --default-config decor）
```

仓库里的 `config.example/` 就是 `--write-config` 写出来的那一套（`config.lua` + `configs/`）；
`./test-defaultconfig.sh` 里有一组断言盯着「这套文件和 `--default-config` 打出来的一模一样，
文件清单也得一致」，所以它不会跟代码走散。

**这份打出来的配置就是配置项的手册，它得全**：每个平铺键（`config.c` 的 `entries[]`）、
每个族里的项（`*_fields[]`）、每个 `vidow.*` 函数、以及 `vidow.rule{}` / `vidow.output{}`
读的每个字段，都得在 `config.example/` 里出现过 —— 那些表字段最容易漏（它们不是配置键，
`entries[]` 那套「键都在」的检查盯不到），所以 `test-defaultconfig.sh` 另有一组断言直接从
源码里把这些名字抠出来逐个去找，少一个就红。

### 间距（gap）：一个数管全部

```lua
gaps = 10                          -- 屏幕边到窗口 10px、窗口之间也是 10px
gaps = { top = 30, inner = 20 }    -- 只盖这两项，其余跟着 gaps
```

| 键 | 作用 |
| --- | --- |
| `gaps` | 一个数管全部：屏幕边到窗口、以及窗口之间都用它 |
| `gaps_top` / `gaps_bottom` / `gaps_left` / `gaps_right` | 单独盖某一边的边距，`-1` = 跟随 `gaps` |
| `gaps_inner` | 单独盖「窗口 ↔ 窗口」（列间距、同列上下），`-1` = 跟随 `gaps` |

- 以前是照 niri 抄的四个值（`workspace_gaps_inner/outer` + `window_gaps_inner/outer`）
  再加一个 `gaps <n>` 简写一起设：九个键说一件「间距」的事，而且 `gaps = 10` 实际得到的
  是**离屏幕边 20px**（内外两份相加）。现在一个数就是一个数
- 单独设过的项盖住统一值（`gaps = 16` **不会**把 `gaps_inner = 20` 冲掉），写回 `-1`
  才回到「跟随 `gaps`」
- 负值一律当 0（负 gap 会让工作区比屏幕还大）；`-1` 是「没设」的哨兵，不当负值算
- 浮动窗口的活动范围、overview 的相机框、`set-window-size` 的夹取都走同一个
  `vidow_work_area`，所以它们跟着一起变
- 默认 `gaps 6` 的效果 = 离屏幕边 6px、窗口之间 6px（**和加 gap 之前的样子一样**）

```bash
vidowmsg set-config gaps 16                 # 屏幕边和窗口之间一起变
vidowmsg set-config gaps_inner 20           # 只改窗口之间
vidowmsg set-config gaps_left 40            # 只把左边距设成 40（其余跟随）
vidowmsg set-config gaps_left -1            # 左边界回到「跟随统一值」
```

**列宽是「连 gap 一起分」的**（niri 的 `resolve_column_width`）：比例分的是包括中间
那个 gap 在内的一块区域，窗口 = 那块区域减掉一个 gap：

```
列宽 = (可用宽 + 内间距) × 比例 − 内间距        可用宽 = 屏宽 − 左右边距
```

所以 Σ比例 = 1 时几条列连着中间的 gap **正好铺满**可用区 —— 两条 50% 不会被 gap 挤出
屏外（以前是 `可用宽 × 比例`，两条 50% 加起来比可用区多一个 gap，第二条得滚一下才看得
全）。`比例 = 1` 时两种算法一样，所以单列占满的行为没变。纵向本来就是这个口径：
n 块窗口的高度 = `(可用高 − (n−1) × 内间距) / n`。

实测（headless 1280×720、两个 `tools/pattern.py`，读 IPC `state` 的 `geometry`）：

| 配置 | 列宽 | 离屏幕边 | 列间距 |
| --- | --- | --- | --- |
| `gaps 6`（默认） | 631 | 6 | 6 |
| `gaps_inner 20` | 624 | 6 | 20 |
| `gaps 16` | 616 | 16 | 16 |
| `gaps 40` | 580 | 40 | 40 |

同列两个窗口（`move-window-direction -1` 并进一列后）纵向间距跟着 `gaps_inner` 走：
6 → 每格高 351 / 起点 y=6；20 → 344；-50（当 0）→ 354。单边（前置是 `gaps 6`）：
`gaps_left 40` → 左边距 40、列宽 (1234+6)/2 − 6 = 614、上下仍是 6；再加 `gaps_top 30` →
上边距 30、每格高 (720-30-6-6)/2 = 339；`gaps_left -1` → 左边距回到 6、列宽 631，
`gaps_top 30` 仍然生效。回归脚本：`./test-gap.sh`（34 条断言，全走 IPC，不截屏；
其中一条专门盯「两条 50% 正好装进一屏」）。

### 浮动窗口

两种模式，都不占列位：

| 模式 | 位置语义 | 视口滚动时 | 自动识别条件 |
| --- | --- | --- | --- |
| `FIXED` 固定位置 | 输出上的绝对坐标 | **不动** | 客户端把 min/max 尺寸钉死（对话框常见） |
| `FOLLOW` 跟随窗口 | 相对父窗口平铺格的偏移 | **跟着父窗口一起走** | `xdg_toplevel` 设置了 parent（子弹窗/对话框） |

- 自动识别在**首次 map 时**判定：客户端通常在第一轮 configure 之后才发尺寸提示
- `FOLLOW` 的子窗初始居中于父窗口格；拖动它改的是相对偏移，父窗口一动它就跟
- 手动切换：`Mod+V` / `Mod+Shift+Space`（平铺 ⇄ 浮动）；拖动平铺窗口的标题栏也会把它
  变成浮动（客户端发 `xdg_toplevel.move` 请求）
- 交互：`Mod+左键拖动`移动、`Mod+右键拖动`缩放（浮动窗口从右下角开始，最小 64px；
  平铺窗口改的是**这一列的宽度** —— 列宽的右边缘跟着光标走，niri 的 interactive
  resize 那条，见 changelog 185）
- **层级**：浮动窗口**永远**在平铺窗口之上，浮窗之间靠**点击**换层级（点哪个哪个上来）；
  全屏压在最后（平铺 < 浮动 < 全屏，见 changelog 167 与 `vidow_restack()`）
- 父窗口销毁时，子窗留在原地转成 `FIXED`，不会跟着消失

## 工作区与特殊工作区

工作区是**装列的一层**：一条列属于一块工作区，一块工作区在某一刻住在某块屏上。同一
时刻每块屏显示一块工作区，其余原地藏着 —— 藏起来的窗口**没有死**（客户端照常在跑、
尺寸照常算），只是场景节点被关掉，所以切回来是瞬时的，一行布局都不用重算。

```
        屏 HEADLESS-1（亮着工作区 1）        屏 HEADLESS-2（亮着工作区 3）
       ┌─────────────┬─────────────┐      ┌─────────────┬─────────────┐
工作区 │  列1 窗口A  │  列2 窗口B  │      │  列1 窗口C  │             │
1 / 3  │             │             │      │             │             │
       └─────────────┴─────────────┘      └─────────────┴─────────────┘
工作区 2、4：藏着。列、窗口、列宽、滚动位置、焦点全留着，切回来原样。
特殊工作区：**不进工作区列表**，打开时显示在聚焦的那块屏上，独占那块屏。
```

**每块屏各自一份工作区栈**（niri 的模型）：序号 = 在这块屏上从上往下第几块，序号是
**位置**不是身份（搬走一块，后面的序号跟着变）。三条不变量：

* **栈底永远留着一块空的**：窗口开在它上面，底下立刻再补一块；
* 中间那些**空的、没名字的**工作区，切走就自动消失；
* **具名工作区**（配置里 `vidow.workspace_create("chat")`，或 `set-workspace-name`）永远
  存在，空了也不消失，按声明顺序排在栈顶。

所以一般**不用预先建工作区**：`Mod+U/I` 上下切、`Mod+3` 直接跳（序号越界夹到最后一块），
用着用着就长出来了。想「往中间塞一块新的」：先在栈底那块空的上面开个窗口，再
`Mod+Ctrl+I`（整块工作区往上挪）。

| 概念 | 语义 |
| --- | --- |
| 序号寻址 | `workspace 3` = 这块屏上第 3 块（1 起，越界夹到最后一块）。**不按名字建**：具名工作区只在配置里建 |
| 新窗口落点 | 落在**现在显示着**的那块工作区；落的是栈底那块空的，就再补一块空的 |
| 切换 | 藏旧的、亮新的，还原这块工作区的滚动位置和离开时的焦点；离开的那块空了就收掉 |
| 搬窗口 | 整条列（或浮窗）搬过去；搬进没显示的工作区 = 它不见了（还在跑），切过去才看得见 |
| 每块屏各亮一块 | 两块屏可以同时亮着两块不同的工作区，各有各的栈。拔掉一块屏时，它名下的工作区整块挪到剩下的屏上（追加在栈底那块空的前面），插回来再按归属认领回去 |
| 整块换屏 | `move-workspace-to-output`：插到目标屏活动工作区后面，并在那边亮起来 |
| 特殊工作区 | 不绑屏、不进列表、不进序号：打开时显示在**聚焦的那块屏**上（别的屏照旧显示各自的普通工作区）；再开一次收起，窗口留在里面，下次打开还在 |
| 弹出物 | 不进工作区，跟父窗口走：父窗口所在的工作区被切走（或换成特殊工作区），菜单 / 候选框跟着收起来 |

### 配置怎么写

工作区是用户建的，键位也是用户绑的 —— 合成器一个快捷键都不预设（默认那套见
「快捷键」一节，那里全是窗口/列的操作）：

```lua
-- ~/.config/vidow/config.lua
-- 具名工作区（想建才建：它们永远在，不会自己消失）
vidow.workspace_create("chat")
vidow.workspace_special("scratch")

for i = 1, 9 do
  vidow.bind("Mod+" .. i, "workspace " .. i)       -- 切到这块屏上第 i 块（越界夹住）
  vidow.bind("Mod+Shift+" .. i, "move " .. i)      -- 把当前窗口搬过去
end
vidow.bind("Mod+C", "workspace chat")      -- 按名字切
vidow.bind("Mod+grave", "special scratch")       -- 开/关特殊工作区
vidow.bind("Mod+Shift+grave", "move scratch")    -- 把当前窗口搬进特殊工作区

-- 状态栏 / 自己的选择器：只列普通工作区（没名字的动态工作区 name 是空串，用 idx）
for _, ws in ipairs(vidow.workspace_list()) do
  print(ws.idx, ws.name, ws.named, ws.active, ws.views, ws.output)
end
```

动作字符串是开放集合（`vidow.bind(键, 动作)` 里的第二个参数）：

| 动作 | 意思 |
| --- | --- |
| `workspace <n>` | 切到这块屏上的第 `<n>` 块（1 起，越界夹到最后一块） |
| `workspace <名字>` | 切到那块具名工作区（没建过就报错，不现建） |
| `workspace up` / `down`（`next` / `prev` 同义） | 上/下切一块（到头就停住） |
| `workspace previous` | 回到上一块切过的工作区 |
| `move <n>` / `move <名字>` | 把焦点窗口（列 / 浮窗）搬到那块工作区 |
| `move up` / `move down` | 搬到相邻的那块工作区（**不跟过去**：人留在原来那块，焦点落到剩下的窗口上） |
| `move up follow` / `move down follow` | 同上，但**跟着走**：搬完连人带屏一起切到那块工作区，焦点还给搬过去的那个窗口（默认键位 `Alt+Shift+滚轮` 用的就是这条）。`follow` 只跟 `up` / `down` 搭 |
| `move-workspace up` / `down` | 整块工作区在这块屏上上/下移一位 |
| `workspace-name <名字>` | 给当前这块工作区起名（之后它就不会被自动收掉；不带参数 = 去掉名字） |
| `special <名字>` | 开/关那个特殊工作区（名字没建过就现建） |
| `focus <-1\|1>` | 焦点左/右移一列 |
| `minimize-window`（`minimize` 同义） | 收起当前窗口（留在原列里，只是不占位；见「最小化」一节） |
| `restore-window`（`unminimize-window` 同义） | 放回最近收起来的那个窗口，并把焦点跟过去 |
| `toggle-minimize` | 当前窗口收了就放回、没收就收起 |
| `mousemove` | 把光标所在的那块屏当作「聚焦的屏」（特殊工作区跟的就是它） |
| `none` | 空绑定，用来屏蔽某个键 |
| `focus-column <方向>` / `move-column <方向>` | 焦点左/右移一列 / 当前列左/右移一位 |
| `focus-window <方向>` / `move-window <方向>` | 列内焦点下/上移 / 列内窗口下/上移 |
| `expel-window-from-column [left\|right]`（`expel-window` 同义） | 把焦点窗口从它那条列里拎出来、单独成一条新列（niri 的 expel-window-from-column；不写方向 = 右边）。列里只剩它一个 / 它是浮窗时什么都不做。**同一个动作拖拽也能触发**：把窗口拖到列带的缝上 / 空白处就是「取出来成新列」（见 `Mod+左键拖动`） |
| `swap-window <left\|right>` | 和相邻那列里的窗口**换个位置**：两个窗口各自住进对方那一格，焦点跟着自己走（列里只有一个窗口时 = 两条列的内容对调）。和谁换：对方那列里**同一层**那个窗口（层数不够就取最后一个），整列都最小化了的列跳过。浮窗 / 那个方向没有别的列时什么都不做 |
| `close-window` | 关掉焦点窗口 |
| `toggle-floating` | 焦点窗口 平铺 ⇄ 浮动 |
| `toggle-fullscreen`（`fullscreen` 同义） | 焦点窗口全屏开关 |
| `cycle-column-width` | 列宽循环（默认 1/3 → 1/2 → 2/3，按 `preset_column_widths` 配的那几档，见编号 261） |
| `maximize-column`（`toggle-maximize-column` 同义） | 整列最大化：撑满工作区宽度，再按一次还原成原来那个比例 |
| `toggle-overview`（`overview` 同义） | niri 风格全景开关 |
| `spawn <命令>`（`exec` 同义） | 撒手跑一条 shell 命令（双 fork、不等它、不阻塞渲染） |
| `switch-vt <1..12>` | 切 tty（没有会话时只记一行日志，交给宿主） |
| `quit`（`exit` 同义） | 退出合成器 |

`<方向>` = `-1` / `1`，也认 `left` / `right` / `up` / `down` / `prev` / `next`。

`vidow.bind` 的键名大小写不敏感，修饰键认 `Mod`/`Super`/`Logo`/`Win`、`Shift`、`Alt`/`Mod1`、
`Ctrl`/`Control`；键名可以是单字符（字母、数字）、X11 惯用名（`grave`、`minus`、`equal`、
`tab`、`space`、`escape`、`bracketleft`…），**或者任何 libxkbcommon 认识的 keysym 名字** ——
表里没有的名字按 keysym 解（大小写不敏感），所以 `XF86AudioRaiseVolume`、`XF86MonBrightnessUp`、
`XF86Tools`、`XF86Calculator` 这些媒体键 / 亮度键直接就能绑，不必在表里一个个列。

**裸键（不带修饰键）默认拒绝**：抢下来客户端就没法打字了，配置写错会在 `config.lua` 那行报错
并打一行日志。两个例外：① **XF86 段**（`0x10080000..0x1008FFFF`）自动放行 —— 媒体键天生没
修饰键、也没有别的消费者；② 显式写 `{ bare = true }` —— 给 `print` / `F2` 这种「不参与打字、
但客户端可能自己要用」的键留的口子，要就拿。

### 浮窗的位置是「相对屏幕」记的

浮动窗口的位置存成**相对它现在那块屏原点**的偏移，绝对坐标只在摆放那一刻算出来
（`src/float.c` 里唯一的换算点）。这样拔 / 插屏导致 wlroots 重排输出时（撞见过右边那块
从 `x=1280` 挪到 `x=0`），浮窗跟着屏走而不是留在原地。窗口记录里的 `home_dx/home_dy`
就是这份偏移，拖完窗口、客户端改完尺寸都会刷新一次。

### 三层归属（和热插拔的关系）

工作区按**名字**认，列跟着工作区走。一块屏拔掉时：

1. 它名下（正在显示 + 藏着）的工作区整块交给接收的屏 —— 列一列都不用动；
2. 接收的屏上如果有**同名**工作区，两者合并（列按名字归并进同一块）；
3. 屏插回来时按同一组名字搬回去，列的顺序、列宽、滚动位置都还在。

### 测试

```bash
./test-workspace.sh   # 58 项断言：切工作区只显示那一块（日志 + 四象限像素）、
                        # 配置建工作区 / 特殊工作区不进 list、特殊工作区跟随聚焦的屏、
                        # 拔屏时工作区跟列走、键位绑定表
```

黑盒测试不用假装敲键盘：`VIDOW_TEST_ACTION="600:workspace 2;1200:special scratch"`
按毫秒时间表执行动作；`VIDOW_TEST_ACTION="file:/tmp/actions"` 则是听一个 FIFO，往里写
一行执行一行（`test-workspace.sh` 用的就是它，时序可控且和真实按键走同一个入口
`vidow_action_run()`）。

## 最小化：窗口留在原列里，只是不占位

```bash
vidowmsg minimize-window            # 收起当前窗口
vidowmsg minimize-window --id 3     # 按 id 收起某个
vidowmsg unminimize-window          # 放回最近收起来的那个（键位要的就是这个）
vidowmsg toggle-minimize --id 3     # 收了就放回、没收就收起
```

键位默认 `Mod+M` 收起、`Mod+Shift+M` 放回；也能绑成动作串
（`vidow.bind("Mod+M", "minimize-window")`，见「工作区」那节的动作表）。

**窗口还在原来的列里** —— 列宽、同列上下顺序、滚动位置、归属工作区、归属屏全都留着，
所以放回来是瞬时的，而且位置就是原来那个（不需要「记住它原来在哪」这种账）。只是：

- **不占位**：布局跳过它；同列剩下的窗口自己撑满；一条列里的窗口**全**收起来时，那条列
  从条带里消失，后面的列合拢（跟 niri 的条带「有东西才占宽度」一致）；
- **不显示**：场景节点关掉（和「工作区被藏起来」同一套），不参与焦点；
- **不补帧**：没有规则时客户端就此停帧（省电）。**但规则 `force_render` 的窗口、以及
  正在被人录屏 / 预览的窗口照样补帧** —— 补帧那条路的判据是「没画在画面上」，最小化正好
  让它成立，所以录屏录到的还是新画面（日志会写「它最小化了，照样补帧回调」）。
- **不收工作区**：整块工作区里的窗口全收起来时它不算空（不然窗口的住处会被拆掉）。

三边都接上了：

| 谁 | 怎么进来 | 怎么知道状态 |
| --- | --- | --- |
| 键位 / IPC | `Mod+M`、`minimize-window` | `vidowmsg windows` 里的 `minimized`、`window-minimized` / `window-unminimized` 事件 |
| Wayland 客户端 | `xdg_toplevel.set_minimized`（xdg-shell v6） | 回一次 configure（协议要求状态请求必须有应答） |
| X11 客户端 | `WM_CHANGE_STATE` 的 IconicState / `_NET_WM_STATE_HIDDEN`（托盘图标的「收起来」走这条） | 写 `WM_STATE=IconicState` + `_NET_WM_STATE_HIDDEN`（`xprop -id <窗口> WM_STATE` 能看到） |
| 任务栏（waybar 等） | `wlr-foreign-toplevel-management` 的 `set_minimized` / `unset_minimized` | 同样的协议回 `minimized` 状态；点一下那个条目就是 `request_activate`，vidow 会先放回再给焦点 |

X11 客户端一启动就是图标化状态（托盘类程序会这么开）时，vidow 直接按最小化收起来，不抢
焦点、不占位置。

### 测试

`./test-minimize.sh`：IPC 与动作串、条带合拢、同列撑满、焦点交接、放回原位、浮窗、
整列收起、客户端自己请求（`tools/frames.c` 的 `VIDOW_TEST_MINIMIZE_MS`）、
`force_render` 的窗口收起来照样出帧、SIGHUP 不改状态、X11 的 `WM_STATE` 来回切。

## 列最大化（niri 的 `maximize-column`）

`Mod+Shift+F`（动作 `maximize-column`，IPC `toggle-maximize-column` / `set-maximize-column`）
把焦点列撑满工作区宽度，再按一次还原成**原来那个比例**（记在 `vidow_column.maximized_saved`
里）—— 0.25 改成最大化再还原还是 0.25，不是回到默认的 0.5。状态在 `columns` 的 `maximized`
里，变化时发 `column-maximize-changed`。

vidow **没有窗口级最大化**：列是布局的最小单位（窗口在列内纵向均分高度），所以「最大化一个
窗口」只能是「最大化它所在的那条列」。手动改宽度（`cycle-column-width` / `set-column-width` /
`expand-column-to-available-width`）就是退出这个状态 —— 那几个动作只改宽度、不记原值。

### 客户端 `set_maximized` 怎么映射

| 客户端做的事 | vidow 做的事 |
| --- | --- |
| `xdg_toplevel.set_maximized(true)` / X11 的 `_NET_WM_STATE_MAXIMIZED_*` 立起来 | 把它那条列最大化，并记下「这次是它要的」；回一个 `maximized = true` 的 configure（X11 那边写 `_NET_WM_STATE_MAXIMIZED_VERT` + `HORZ`） |
| 同一个客户端再请求撤销（`false`） | 列还原 —— **只有它自己要来的才理** |
| **从来没要过**的客户端发 `set_maximized(false)` | 列是用户按出来的，不理它的撤销：照当前实际状态回话（列最大化的照样答「最大化」），不然客户端和布局各说各话 |
| 浮窗（没有列）请求最大化 | 回一个「不最大化」（浮窗的尺寸是它自己定的，vidow 不替它撑满） |

第一次 commit **之前**就 `set_maximized` 的客户端（Qt/KDE 恢复「上次是最大化」的窗口就是这么
开窗的，kate 首当其冲）在 `view_commit` 的 `initial_commit` 分支里兑现 —— 那时候列才建出来，
而且早于第一次 arrange，所以它拿到的**第一个** configure 就带 `maximized`（晚一步就按「没
最大化」排过一遍，客户端还会先收到一个 `maximized=false`）。这条路上不能直接调
`wlr_xdg_toplevel_set_maximized()`：surface 还没 initialized 时它内部是
`assert(surface->initialized)`（wlroots 0.20.2：`types/xdg_shell/wlr_xdg_surface.c:168`），
当场 abort —— 和全屏那条（changelog 144 / `test-preinit.sh`）是同一个坑。

X11 那边多一层小心：`wlr_xwayland_surface_set_maximized()` 写下去之后，客户端可能把
`_NET_WM_STATE` 回写过来又触发一次 `request_maximize`（X11 没有「请求-应答」，状态就是属性
本身）。所以 X11 那条路**状态一致就直接返回**，不给自己回声的机会（不然就是「我们设属性 →
客户端回写 → 信号又来 → 再设一次」转圈）。

### 和全屏的区别

|  | `Mod+F` 全屏 | `Mod+Shift+F` 列最大化 |
| --- | --- | --- |
| 几何 | 占满**整块屏**（不吃 gap、也不给状态栏留位置）、跟着条带滚动、置顶压过别的列 | 整列撑满工作区宽度（其他列还在条带上，被推到视口外） |
| 客户端知道吗 | 知道（`set_fullscreen`：播放器会真全屏、藏 UI） | 知道（`set_maximized`，但客户端仍按普通窗口画） |
| 同列别的窗口 | 让位（不参与高度均分） | 照旧（列宽变了，高度还是均分） |

两个状态互不干扰，可以叠着用（全屏的窗口同时属于一条最大化的列）。

### 测试

`./test-maximize.sh`（60 项断言，28 秒）：IPC 开关与几何、还原回原比例、手动改宽度退出
最大化、键位动作名、客户端上屏之后请求 / 抢跑请求（第一个 configure 就带 `maximized`）、
用户按出来的最大化客户端撤不掉、客户端自己要来的撤销就理、`column-maximize-changed` 事件、
浮窗当场报错、X11 的 `wmctrl` 请求与 `_NET_WM_STATE` 来回切。

## 视口外强制渲染（窗口规则 / 录屏 / 窗口预览）

Wayland 的帧回调（`wl_surface.frame`）是合成器「我要用你的画面了」的**许可**：客户端
（mpv、游戏、Qt、GTK）都拿它做节流。窗口滚出视口、或者所在工作区被藏起来时，场景里
什么都没画到它，就没有回调 —— 客户端停帧，切回来是一张冻住的画面，录屏录到的也是它。
**最小化的窗口也一样**（它同样「没画在画面上」）：没有规则就停帧，配了 `force_render`
或者正被人录屏 / 预览就照旧出帧 —— 日志会写「它最小化了，照样补帧回调」。

用 `tools/frames.c`（一个「只按许可出帧」的窗口）量出来：

| 情况 | 出帧 |
| --- | --- |
| 窗口在视口里 | ~60 帧/秒 |
| 滚出视口，没有规则、没人录 | **0 帧/秒**（停死） |
| 滚出视口，配了 `force_render` | ~60 帧/秒（接着跑；速率可配，见下） |
| **最小化**，没有规则、没人录 | **0 帧/秒**（停死） |
| **最小化**，配了 `force_render` | ~60 帧/秒（接着跑） |

两条路让它继续出帧，都不用改布局：

```lua
-- 全局节流：视口外补帧每秒最多几帧（0 = 不限速，跟输出的帧节奏走）
force_render_hz = 60          -- 也可以用 VIDOW_FORCE_RENDER_HZ 覆盖

-- ① 窗口规则：哪些窗口值得保活、各补多少帧，由你说了算
--    （匹配是 POSIX 正则：不锚定 = 子串语义、大小写不敏感；前面加 ! 取反）
vidow.rule{ app_id = "mpv", force_render = true }              -- 用全局的 60Hz
vidow.rule{ title = "^预览", force_render = true, hz = 15 }    -- 这个窗口只补 15Hz
vidow.rule{ class = "Steam", force_render = false }            -- 后面的覆盖前面的
vidow.rule{ app_id = "vlc", hz = 0 }                           -- 只调节流：不限速
vidow.rule{ exe = "^wechat$", x11_scale = 1 }                  -- 没有 app_id 的按进程认
vidow.rule{ app_id = "^mpv$", workspace = "media", fullscreen = true }
vidow.rule{ title = "^登录", float = true, width = 480, height = 320, center = true }

-- 一条规则至少写一个作用，都不写会在配置那一行报错；字段名写错了也报错，
-- 而且把能写的字段名全列出来。

-- 查回来（状态栏 / 自检用；没管的字段是 nil —— 「没管它」和「管了、给了默认值」
-- 是两回事，JSON 那边（vidowmsg rules）对应的是 null）
for i, r in ipairs(vidow.rules()) do
  vidow.log(string.format("rule %d: app_id=%s title=%s exe=%s force_render=%s hz=%s",
    i, tostring(r.app_id), tostring(r.title), tostring(r.exe),
    tostring(r.force_render), r.hz == nil and "配置默认" or tostring(r.hz)))
end
```

| 项 | 说明 |
| --- | --- |
| 匹配方式 | 字符串字段都是 **POSIX 扩展正则**（`regcomp(REG_EXTENDED \| REG_ICASE)`），**不锚定** = 找到就算命中 —— 所以普通字符串写进去就是原来的「子串、大小写不敏感」。含元字符（`. * + ? [ ] ( ) { } \| ^ $ \`）的串要转义：`title = "v1\\.0"`；整条规则写 `literal = true` 退回纯子串匹配。字段前面加 `!` = 取反（`title = "!^预览"` = 不是预览窗口），要匹配字面的 `!` 就写 `\\!`。**Lua 风格的 `%` 转义也认**：`app_id = "^org%.kde%.kate$"` 等价于 `"^org\\.kde\\.kate$"`；`%%` 是字面 `%`，`%` 后面跟字母数字（`%d` 这类）原样按字面匹配 |
| 匹配字段（窗口） | `app_id`（Wayland 的 app_id / X11 的 WM_CLASS class 段）、`title`、`class`、`instance`、`role`、`initial_app_id`、`initial_title`、`initial_class`、`initial_instance`（上屏那一刻的名字 —— 浏览器、终端会改标题，认「刚开的那一屏」用它）、`on_output`（现在在哪块屏）、`on_workspace`（现在在哪块工作区） |
| 匹配字段（窗口和图层都认） | `pid`、`exe`、`cmdline` —— 从 `/proc/<pid>/…` 读，没有 `app_id` 可认的老程序、包一层启动脚本的，按进程认（`exe` 是 `/proc/<pid>/exe` 的最后一段，flatpak 那种是 `bwrap`，用 `cmdline` 更准） |
| 匹配字段（图层） | `namespace`（waybar / mako / notifications …）、`layer`（`background` / `bottom` / `top` / `overlay`）—— 写了这两个里任何一个就是**图层规则**。窗口字段和图层字段写进同一条规则 = 这条谁也不匹配（不静默失效，日志和 `vidowmsg rules` 都看得见） |
| 匹配字段（状态） | `is_` 开头的是「现在是不是这样」：`is_xwayland`、`is_popup`（菜单 / 提示这类弹出物）、`is_floating`、`is_fullscreen`、`is_maximized`（所在列整列最大化）、`is_minimized`、`is_fixed_size`（客户端声明了固定尺寸，微信登录窗那种）。写成 `true` / `false`（不写 = 不管它），比的是**判定那一刻**的状态 —— 判定发生在建窗口、标题 / class 变化、重载规则这三处 |
| 作用（一直在） | `force_render`、`hz`、`focus_on_open`、`focus_on_activate`、`fake_focus`、`honor_invalid_serial`、`x11_scale`、`capture`、`capture_mosaic`、`anim`（`false` = 不做开合动画，窗口 / 图层都认）、`open_ms` / `close_ms`（开合动画时长，毫秒，覆盖 `window_open_ms` / `layer_open_ms` 那一对全局键；`0` = 不做这个动画） |
| 作用（外观） | `decor`（`false` = 不画装饰）、`opacity`、`corner_radius`、`border_width`、`border_color`、`border_color_focus`、`shadow_radius`、`shadow_color`、`shadow_offset_x`、`shadow_offset_y`、`noise`、`blur`（`false` = 它背后不模糊，`true` = 平铺窗口也模糊）、`blur_xray`。没写的用全局配置；**全局开关（`decor_enabled` / `blur_enabled`）是总闸**，规则只能收窄不能突破。`vidowmsg windows` 的 `look` 那一块就是「这个窗口现在长什么样」—— 规则有没有生效看它，不用量像素 |
| 作用（开窗时兑现一次） | `float`（`true` 强制浮动 / `false` 强制平铺）、`workspace`（具名工作区，没有就建 / `special:名字` / 序号）、`output`（开在哪块屏）、`width` / `height`（浮动窗口开多大）、`center`、`fullscreen`、`maximize`、`minimize`、`column_width`（这个窗口新开的列有多宽）。**重载规则不搬已经开着的窗口，也不改它们的尺寸** —— 「开出来什么样」和「现在什么样」是两回事 |
| 覆盖顺序 | 按配置里的先后逐条套用，**后面的覆盖前面的**；一条规则只动它写了的字段，谁都没写的字段用默认值 |
| 图层规则能用的作用 | `capture`、`capture_mosaic`、`overview`（进全景的时候露不露：`"always"`（默认，照常）/ `"only"`（只在全景里显示）/ `"never"`（全景里不出现）—— **全景里换一张壁纸就靠它**，见「全景里换一张壁纸」那节）、`anim`、`open_ms`、`close_ms`、`opacity`（面板**自己的内容**多透 —— 底衬那份模糊不跟着淡，和窗口那边 `u_opacity` 只乘内容 + 边框是同一个语义）、`blur`。其余作用都是窗口专有，写进图层规则 = 这条规则谁也不匹配（当场看得出来，不会悄悄生效）。`vidowmsg layers` 的 `rule` 那一块是算出来的 `anim` / `open_ms` / `close_ms` / `opacity`，另有 `overview` / `shown` 两个字段报「全景里怎么露面」和「现在画没画」 |
| 生效时机 | 建窗口时判一次；标题 / class 变了再判一次；`SIGHUP` 重载规则后把已经开着的窗口全部重判 |
| 节流 | `force_render_hz = <每秒几帧>`（默认 60，`0` = 不限速）；规则里写 `hz = <n>` 可以逐窗口覆盖。补帧要主动排下一帧，不限速时会一直出帧（headless 后端上量到过 2500fps），所以默认按刷新率给 |
| X11 缩放桥 | `x11_scale = <n>`：这个 X11 窗口的缓冲区按 `n` 倍声明（逻辑尺寸 = 缓冲 / `n`）。`x11_scale = 1` = 「这个客户端没按 `GDK_SCALE` 画，别除」—— 微信登录窗那类固定尺寸、不认 env 的程序在 2 倍屏上就不会只剩 1/4 面积。见「X11 的 HiDPI」那节，回归判据 `test-x11scale.sh` |
| ② 录屏 / 窗口预览 | **自动**：有人为某个窗口建了截图源（xdg-desktop-portal 的窗口录制、预览缩略图走的就是这条路），那个窗口立刻进入强制渲染；截图源没了、或者录屏客户端断开（崩了 / 被关掉）就自动退出，不会一直补帧 |

机制（`src/view.c` 的 `vidow_view_render_pump`）：每帧在场景补帧之后，把「规则要求保活」
或者「正被人拿着」但**没画到**的窗口挑出来，给它的整棵表面树补一次
`wlr_surface_send_frame_done()`，并主动排下一帧（不然画面没变化时 wlroots 就不给帧事件，
补帧链自己就断了）。两个细节：

- **只在没画到的时候补**：窗口在画面上时场景那边已经给过回调，再补一次会让客户端白画一遍
  （判断用「所在工作区显示着」+「矩形和这块屏有交集」）。
- **半块越界的窗口裁到自己那块屏里**：还看得见一部分、另一部分跨过屏边界的那种列，
  越出去的那一块按屏框裁掉（`vidow_view_apply_clip`，fx 那块离屏缓冲只采交集那一块）——
  不裁的话隔壁那块屏会把它画出来（用户报的「滚动后超出视口的窗口跑到另一个显示器里」）。
  浮窗 / 正被拖的窗口 / overview / 停到屏外的窗口不裁（**有人录屏也照裁** —— 抓帧渲染的
  是窗口自己的一份镜像，见改动记录 172）。判据比的是**这一帧画在哪儿**
  （几何 + 动画偏移），不是布局目标位置 —— 动画里窗口从屏边界上扫过去，按目标位置判
  会把「滑回来的路上」当成已经在屏里（见改动记录 171 的 ④）。
- **滚出视口的列停到所有输出框外面**（同一个「矩形和这块屏有交集」的判据）：`wlr_scene`
  是一张**全局坐标**的场景图，每块屏的场景输出画的是「落在自己那块坐标里的所有节点」——
  列往右滚出屏宽之后坐标正好落进右边那块屏的框里，不停到屏外就画到隔壁显示器上了
  （niri 不会：它每块屏各渲染自己那棵）。**不是**关掉场景节点（`enabled = false`）：那会
  把「这个窗口现在在不在画面上」这件事从**位置**换成**开关**，而位置是每帧现算的
  （动画里窗口会从屏边界上扫过去，开关很容易落后一帧、把窗口永久关掉，见改动记录 171）。
  客户端该出帧还出帧（见上一条的补帧）；按窗口录屏 / 预览渲染的是窗口自己的一份镜像
  （`src/wincapture.c`，见改动记录 172），跟节点摆在哪、裁没裁都没关系。三种情况**不停**：浮动窗口、
  正被拖的窗口（它们本来就允许跨屏）、overview（所有窗口都要缩进画面）。回归判据
  `test-scrollcull.sh` + `test-force-render.sh` 的录屏那节 + `test-overview.sh`。
- **节流可配**（`force_render_hz`，规则可逐窗口覆盖 `hz`；0 = 不限速）：补帧会自己排
  下一帧，不节流的话在没 vsync 的后端（headless、嵌套）上会空转到几千 fps —— 一个
  「保活」把 CPU 跑满就没意义了。到点的唤醒用一个**闹钟**（`wl_event_source` 定时器）
  来做：只靠「客户端提交了再推」是不行的 —— 提交一次之后没人再推，30Hz / 15Hz 会断在
  半路（实测全局 30 补出来 0 帧，改成闹钟之后是 30 帧/秒）。

### 测试

```bash
./test-frametrace.sh     # 16 项：帧追踪（健康不误报 / 掐断帧链点名 / 关掉零日志）
./test-force-render.sh   # 28 项断言：控制组（不配就停帧）→ 规则（~60 帧/秒）→ 回到画面
                         # 上就停 → 藏起来的工作区照样补帧 → 录屏（抓到的画面在变）→
                         # 录屏客户端断开后不再补帧 → 节流可配置（全局 30Hz / 规则 15Hz
                         # / hz = 0 不限速，各量一遍帧率）
```

两个测试客户端也是这次加的：`tools/frames.c`（只按帧回调出帧，每帧打印一行，把「客户端
还在不在出帧」变成可断言的输出）、`tools/capture.c`（走 ext-image-copy-capture +
窗口截图源抓某个窗口，打印每帧的像素校验和 —— 校验和不变就说明录到的是冻住的一张）。

## 某块屏的帧链断了：`VIDOW_TRACE_FRAMES=1`

用户报过一条真机症状：**某块屏上的窗口停在最后一帧，拖到另一块屏就活**。帧回调是跟着
输出的帧事件走的（`src/output.c`：`wlr_scene_output_commit` 成功那一帧之后才
`wlr_scene_output_send_frame_done`）—— 所以一块屏的 page-flip 要是不回来了，那块屏上
的**所有**客户端都拿不到帧回调，全停在最后一帧；而输入照旧（键盘 / 点击不走渲染），
看着就像「应用不渲染新画面」。

headless 上模拟不出来（那边的帧事件和 commit 无关，按不住），所以有两样东西：

- `VIDOW_TRACE_FRAMES=1`：开帧追踪。每次「要帧」记一笔（谁要的），帧事件回来销账；
  250ms 一轮，发现「要过帧（或客户端还在交帧）却 300ms 没出帧」就报一行 `WLR_ERROR`，
  点名哪块屏、vidow 要过几帧、最近一次是谁要的。默认关，不开的时候整条路径不做任何事。
- `VIDOW_TEST_STALL_OUTPUT=<屏名>`：测试钩子，把那块屏的帧**事件**按住 = 复现这个症状。

```bash
VIDOW_TRACE_FRAMES=1 VIDOW_LOG=debug ./build/vidow > /tmp/v.log 2>&1
# 复现（窗口停在最后一帧）之后：
grep -a "trace:" /tmp/v.log | tail
#   [ERROR] trace: DP-2 997ms 没出帧（vidow 主动要过 1 帧，最近一次：脚本/配置改了状态）；
#           客户端还在交帧，屏幕却不更新 ← 帧链断了
```

回归判据见 `./test-frametrace.sh`（16 项，headless：健康时不误报 + 掐断帧链时点名 + 追踪
关掉时零日志）。

## 截图 / 录屏里不出现：`capture = "hide"`（窗口规则 / layer 规则）

给密码管理器、聊天窗口、内部面板这类东西用：**截图、录屏里不出现，屏幕上照常显示**
（能点、能输入、`vidowmsg windows` 里还是 `visible: true`）。

```lua
-- 窗口：按 app_id / title / class 子串匹配（和别的规则一样）
vidow.rule{ app_id = "keepassxc", capture = "hide" }

-- layer surface：写了 namespace 的规则只管 layer（子串匹配 layer 的 namespace）
vidow.rule{ namespace = "noctalia", capture = "hide" }

-- 不想藏、只是打马赛克（抓帧里是块状，屏幕上还是原样）
vidow.rule{ app_id = "wechat", capture = "mosaic", capture_mosaic = 16 }
```

| `capture` | 抓帧里怎么出现 | 屏幕上 |
| --- | --- | --- |
| 不写 / `"off"` | 照常出现 | 照常 |
| `"hide"` | **不出现** | 照常（能点、能输入） |
| `"mosaic"` | **块状**（块边长 `capture_mosaic`，屏幕像素，默认 16，最小 2） | 照常（原图） |

`capture_mosaic` 单独写不算一条作用（一条规则至少要有 `capture` / `force_render` /
`hz` / `x11_scale` 之一），写了也不影响 `hide`。

### 为什么这件事要自己实现抓帧（`src/capture.c` / `src/screencopy.c`）

wlroots 那两条抓帧路（`zwlr_screencopy_manager_v1` 和 ext-image-copy-capture 的输出抓帧源）
都是**把屏幕上那一帧的缓冲直接拷给客户端**：screencopy 从 `output->swapchain` 抓一张再
blit 进客户端缓冲，ext-copy 的输出源监听 `output->events.commit` 把 `state->buffer` 拷走。
也就是说「抓帧 == 屏幕那一帧」是写死的 —— 藏起来的东西要么在屏幕上也没了，要么在抓帧
里还在，没有第三种可能（实测过：掩码放在屏幕那一遍渲染之前，抓帧和屏幕一起干净；放在
之后，抓帧和屏幕一起还在 —— 两次渲染共用同一遍场景状态，没有第二遍可以钻空子）。

所以 vidow 自己出帧：**抓帧那一帧是自己渲染的一遍场景**（渲染前把要藏的节点透明度压 0），
屏幕上那一帧完全不受影响。渲染用 `wlr_scene_output_build_state()` 写进一块**自己的**
`scene_output`（配一个假输出），不和屏幕抢 damage 账本 —— 共用的话屏幕会少画一块
（屏幕上留上一帧的残影）。

- `src/capture.c`：抓帧引擎 + ext-image-copy-capture 的**输出抓帧源**
  （会话管线 / 约束 / dmabuf feedback 仍用 wlroots 那份）。
- `src/screencopy.c`：`zwlr_screencopy_manager_v1` 整份自己实现（wf-recorder 走它）。
- 两条路都支持 wl_shm 与 linux-dmabuf 目标缓冲、区域抓帧、`copy_with_damage` 的
  damage 事件、以及「把光标画进帧里」（screencopy 的 `overlay_cursor` /
  ext-copy 的 `PAINT_CURSORS`，`grim -c` 就是后者）。
- 出帧节奏跟 wlroots 一样：screencopy 的 `copy` 之后**等下一次输出提交**再出帧
  （当场出帧会让录屏客户端按编码速度空转，实测过 2500fps 那种）；屏幕静止时没有提交，
  靠 `output.c` 的 capture_watchdog 每 50ms 催一帧。

### 马赛克是怎么画的

马赛克不能靠节点自己的属性（场景没有「像素化」这种东西）：先把节点压掉（和 `hide`
一样），抓帧渲染完再把**块状版本**画回原位置 —— 把画在屏幕上的那张图缩小到
`1/capture_mosaic`（线性过滤，等于块内平均），再用 `NEAREST` 放大回原尺寸：一个块里
所有像素取同一个采样点，看着就是马赛克。放大那一步必须 `NEAREST`，不然又糊回原样。

所以马赛克也只影响抓帧：屏幕上那一帧从头到尾没被碰过（`test-capture-hide.sh` 里量了
屏幕的「平均连续同色像素」，开马赛克前后一样）。

### 藏法：压透明度，不是关节点

`wlr_scene_node_set_enabled()` 也能让它不进画面，但场景的命中测试会跳过关掉的节点 ——
窗口就点不着了（输入焦点和 `vidowmsg pick-window` 都走 `wlr_scene_node_at`）。透明度只
影响合成，命中测试不看它。遮挡只在**渲染抓帧那一瞬间**压上（引用计数，见
`src/capturemask.c`），所以平时一个字节都不改，屏幕也不会因为「有人在抓帧」而变化。

### 已知漏点

| 漏点 | 说明 |
| --- | --- |
| `zwlr_export_dmabuf_manager_v1` | 老协议，导出的是**屏幕那一帧**，藏不掉（vidow 仍然提供它，给老 wlrobs / wf-recorder 用） |
| 窗口自己弹的 popup | 菜单 / 下拉框是另一个场景节点，不在遮挡范围内（和窗口装饰一样的道理） |
| 按窗口录屏（`ext-foreign-toplevel` 抓帧源） | 那条路是 wlroots 那份（直接渲染窗口的场景子树），接不进去：有人在抓一个 `capture = "hide"` 的**窗口**时，只能整段时间把它从屏幕上藏起来 —— 这条路仍然是「录屏时窗口也从屏幕上消失」 |
| 光标 | 抓帧图里**没有光标**（抓帧是场景渲染，场景里没有光标）。要光标得显式要：`grim -c`（走 PAINT_CURSORS，我们按屏幕上的样子把光标画进去，放大摇晃那种大光标也一样）。屏幕上那个放大光标不再为抓帧缩回去 |

### 测试

```bash
./test-capture-hide.sh   # 31 项断言：规则字段 → grim 抓帧里没有它（0 个蓝像素）→
                         # 窗口照常能点（pick-window 命中）→ 对照组 capture = "off"
                         # 照样抓得到 → 嵌套跑证明屏幕上还有它 → **录屏期间屏幕上
                         # 照样有它**（wf-recorder / screencopy，这次改动的核心）→
                         # 录像文件里没有它（copy 和 copy_with_damage 各验一遍）→
                         # layer 的 namespace 规则 → 马赛克（抓帧里跑长 ≈ 块边长、
                         # 屏幕和基线一样、块边长可配）→ 配置报错 → 日志干净
```

```bash
./test-tile-resize.sh    # 14 项断言：平铺窗口 Mod+右键拖 = 改列宽（按光标位移、松手留着、
                         # 两头夹在 5%…100%、浮窗那条路没被抢）
```

```bash
./test-clip-stretch.sh   # 10 项断言（两个装饰模式各 5 项）：窗口只有一点在视口里时，
                         # 屏幕上那一条必须是**裁**出来的 —— 三列 0.9/0.7/0.5 装不下屏，
                         # 焦点逐列试到「左边被切掉」的那一列，用 tools/pattern.py 的四象限
                         # 当标尺量：一个黑像素都没有、分界落在窗口中心 ±4px、颜色是可见
                         # 那一段该有的（旧代码：装饰开着那节必挂 —— 表面被拉伸铺满整块
                         # 窗口 + 一条黑边）
```

```bash
./test-portal-queue.sh   # 18 项断言：两个 Start 同时来（OBS 两条「屏幕采集(PipeWire)」源
                         # 开机都去问 portal）→ 第二个**排队**不秒回也不报 denied → 关掉第一个
                         # 它的选择器接着弹、两个都拿到答复 → 排着队时关会话那个请求被收掉、
                         # 不多弹一个对话框（旧代码必挂：先拒了 + RESPONSE_OTHER）
```

```bash
./test-wincapture-scale.sh  # 16 项断言：窗口抓帧源（ext-image-copy-capture 的窗口源）
                         # 的尺寸中途变了怎么办 —— 两块屏（1 倍 + 2 倍）之间挪窗口，
                         # 这一路流的像素尺寸跟着变（逻辑 × scale），合成器重发
                         # buffer_size：验它只回一次 failed（回第二次就是拿 free 掉
                         # 的内存 post_event，合成器当场段错误）、客户端换掉缓冲之后
                         # 接着出帧、合成器没崩也没新 coredump
```

```bash
./test-win-scrolled-output.sh  # 16 项断言：窗口**滚出视口**之后，录屏 / 抓帧那一路仍然
                         # 算在「窗口自己那块屏」上（判据见 wincapture.c 的
                         # vidow_win_grab_output）—— 四列 0.35 铺在 2 倍屏上让条带装不下，
                         # 逐列聚焦找一个「列还在 2 倍屏、画出来的中心已经落到 1 倍屏范围里」
                         # 的窗口，抓它：尺寸必须是「逻辑 × 2」，滚出去 / 滚回来两次一样
                         # （旧代码：滚出去乘了隔壁 1 倍屏的 scale，尺寸正好一半 —— 录屏里
                         # 就是「看不到光标 + 图是糊的」）；顺带钉住「客户端按几倍渲染」
                         # （preferred_buffer_scale 钉在它那块屏上、且表面树真裁到自己那块屏里 ——
                         # 不裁的话 wlroots 会按「可见面积最大的那块输出」把客户端叫去按隔壁屏渲染）
```

## 焦点：谁抢焦点、激活请求理不理（窗口规则）

两件事共用一个开关面：**开窗口时抢不抢焦点**，和**别人请求激活它时理不理**。

```lua
-- Electron 系会为了通知 / 剪贴板开一个看不见的辅助窗口，一开就把焦点从你正在打字的
-- 窗口上抢走：让它别抢。窗口照常出现、照常落在当前工作区，只是不动键盘。
vidow.rule{ app_id = "Slack", focus_on_open = false }

-- 反过来的毛病：某个后台应用动不动把自己顶上来，干脆别理它的激活请求。
vidow.rule{ app_id = "weird-app", focus_on_activate = false }

-- 第三种毛病：它请求激活时令牌里的 serial 不对（工具包偷懒写 set_serial(0, seat)），
-- 默认会被丢掉 —— 只给它开个口子照样理（比全局开关安全，见下面那节）。
vidow.rule{ app_id = "令牌不灵的应用", honor_invalid_serial = true }

-- 一条规则可以同时管几件事，跟 force_render / hz 写在一条里也行
vidow.rule{ app_id = "mpv", force_render = true, hz = 30, focus_on_open = false }

-- 一失焦就暂停 / 收 UI 的程序：让它一直以为焦点还在（只伪装 xdg activated /
-- X11 focused，键盘焦点和任务栏仍然是真实的）
vidow.rule{ app_id = "some-game", fake_focus = true }
```

| 项 | 说明 |
| --- | --- |
| `focus_on_open` | 这个应用**开新窗口**时把焦点给它吗（默认 `true`）。写 `false` 之后新窗口不抢键盘；**但当前一个焦点窗口都没有时还是会给它**（否则没人收键盘） |
| `focus_on_activate` | 别人**请求激活**它（点通知、任务栏按钮、portal 顶窗口、X11 的 `_NET_ACTIVE_WINDOW`）时理不理（默认 `true`） |
| `fake_focus` | 焦点移走时也不给客户端发 deactivated，让它一直以为自己是激活的（默认 `false`）。只伪装 **xdg_toplevel activated / X11 focused**；键盘焦点、合成器焦点、任务栏都还是真的，不会抢别人的输入 |
| `honor_invalid_serial` | 它的激活请求**即使令牌里的 serial 校验不过**也照样理（默认 `false`）—— 全局开关的**按应用**版本，见「serial 校验不过的请求」那节 |
| 查回来 | `vidow.rules()` 里 `focus_on_open` / `focus_on_activate` 没写就是 `nil` —— 「这条规则没管它」和「管了、给了默认值」是两回事 |
| 一条规则至少写一个作用 | `force_render` / `hz` / `focus_on_open` / `focus_on_activate` / `fake_focus` / `honor_invalid_serial`，都不写会在配置那一行报错 |

### 激活 = 先把窗口端出来，再给焦点

`xdg-activation`（点通知）、`foreign-toplevel` 的 activate（任务栏按钮）、X11 的
`_NET_ACTIVE_WINDOW` 走的是同一条路：**先让窗口那块工作区亮起来，再给键盘焦点**。

以前这里只调 `vidow_focus_view()`，于是窗口要是住在藏起来的工作区上（点通知的经典情形：
应用在第 3 块工作区，你在第 1 块），焦点就落在一个**看不见**的窗口上 —— 看起来就是
「点了通知没反应」。现在日志会写清它做了什么：

```
激活: 演示 在藏起来的 2 上，先把它端出来
workspace: 切到 2（2，本区 1 列 / 1 个窗口，全屏 1 个可见）
焦点: a1 -> 演示
```

跨屏同理（`激活: X 在 HEADLESS-2 上，焦点跟过去`），特殊工作区也一样（`激活: X 在特殊
工作区 scratch 上，把它开到 HEADLESS-1`）。窗口还没上屏就收到激活请求（点通知常常比
窗口第一次 commit 还早）会先记下来，等它 map 的时候兑现。

### 激活令牌 30 秒过期：`activation_token_timeout`

wlroots 给 `xdg-activation` 的令牌设了 **30 秒**的作废时间，而「弹通知」和「点通知」之间
常常隔着几分钟：令牌早没了，wlroots 直接把请求丢掉（`Rejecting activate request: unknown
token`），**合成器连信号都收不到** —— 想在窗口规则里补救都没机会。所以默认改成不过期：

```ini
# config.lua 里写同名键 / VIDOW_ACTIVATION_TOKEN_TIMEOUT 覆盖
activation_token_timeout = 0     # 秒，0 = 不过期（默认）；想严格就写个秒数
```

量出来的对照（同一个客户端：上屏时拿令牌、2 秒后才用它请求激活）：

| `activation_token_timeout` | 结果 |
| --- | --- |
| `1` | 令牌作废，请求被丢（日志 `Rejecting activate request: unknown token`，没有 `激活:` 那几行） |
| `0`（默认） | 请求照常到达（`activation: 激活 演示` + `焦点: a1 -> 演示`） |

### serial 校验不过的请求：`honor_xdg_activation_with_invalid_serial`

令牌里那个 `set_serial` 说的是「谁触发的这次激活」，合成器会校验它是不是**真的发给过那个
客户端的输入事件**。工具包偷懒写 `set_serial(0, seat)`（很常见）时校验不过，表现同样是
「点了通知，窗口不动」。

niri 专门有个开关管这种情况（`honor-xdg-activation-with-invalid-serial`，在它配置的
`debug` 段里）。vidow 同名选项，默认跟 niri / wlroots 一样**不理**：

```lua
-- 按应用开小口子（推荐）：只有这几个应用照样理
vidow.rule{ app_id = "令牌不灵的应用", honor_invalid_serial = true }
```

```ini
# 或者全局开（所有应用都理）；config.lua 里写同名键 /
# VIDOW_HONOR_XDG_ACTIVATION_WITH_INVALID_SERIAL 也能覆盖
honor_xdg_activation_with_invalid_serial = 1   # 默认 0 = 不理
```

| 怎么配 | 行为 |
| --- | --- |
| 都不配（默认） | 令牌照发（协议要求回 `done`），但拿它激活时忽略：`令牌 … 的 serial 0 校验不过 → 不理（要理它就把 honor_xdg_activation_with_invalid_serial 打开，或者给这个窗口写一条 honor_invalid_serial = true 的规则）` |
| 规则 `honor_invalid_serial = true` | 匹配上的窗口照样激活：`… 校验不过，但这个窗口的规则写着 honor_invalid_serial = true → 照样理` → `activation: 激活 演示` → `焦点: a1 -> 演示`；**规则外的应用还是不理** |
| 全局开关 `= 1` | 所有窗口都照样激活（令牌刚建时就写 `校验不过：照样理它（honor_xdg_activation_with_invalid_serial = true）`） |

实测三种 `set_serial` 写法：**不带** → 不校验、放行；**`set_serial(0, seat)`** → 校验不过
（默认丢，开关打开就认）；**带真实点击事件的 serial**（`wl_pointer.button` 给的那个）→ 放行。

**代价**：这个校验本来是防「随便哪个客户端抢焦点」的。打开开关，任何拿到令牌串的客户端
都能把你正在打字的窗口顶下去 —— 只给明确需要的应用开。

### 为什么 xdg-activation 是自己实现的（`src/activation.c`）

wlroots 的 `wlr_xdg_activation_v1` 把校验写死在内部：serial 不对，`commit` 时它就不登记
这个令牌（`Rejecting token commit request: serial 0 was never given to client`），后面
`activate` 当成未知令牌丢掉（`Rejecting activate request: unknown token`）—— **合成器侧
没有任何配置点**能改。所以 vidow 把服务端实现收回来自己做（服务端头 / 代码从
`protocols/xdg-activation-v1.xml` 现场生成）：

- `commit` 一律回 `done(token)`：协议要求发，客户端不该知道令牌有没有被登记；
- serial 校验用 wlroots 的 `wlr_seat_client_validate_event_serial()`（跟它内部同一判据），
  但**结果怎么用由配置决定**；
- 令牌按 `activation_token_timeout` 作废（0 = 不过期），建它的客户端一断就作废；
- 激活认的是 **`activate` 请求里带的那个 surface**（通知守护进程建令牌时不带 surface，
  被激活的应用自己在 `activate` 里带 —— 实测 `--no-token-surface` 也照样能激活）；没带
  才退回 `set_surface` 那个；
- 拿到的窗口照样交给 `vidow_view_activate()`：窗口规则 `focus_on_activate`、跨屏 / 特殊
  工作区端出来、还没上屏就记账，一条都没少。

### 测试

```bash
./test-focus.sh   # 60 项断言：基线（新窗口默认抢焦点）→ focus_on_open = false（不抢键盘
                  # 但窗口照常画出来，像素为证）→ 激活请求把藏起来的工作区端出来（像素：
                  # 激活前只有兜底色、激活后画出来）→ 特殊工作区同理 → 令牌过期 vs 不过期（对照）
                  # → focus_on_activate = false（请求被挡下）→ 还没上屏就请求（先记下来）
                  # → 守护进程式激活（令牌不带 surface，认 activate 里那个）→ serial 校验
                  # 不过：默认不理 vs 开关打开就理 vs 窗口规则按应用开口子（同一个客户端
                  # 同一个时序，三组对照）
```

测试客户端 `tools/activate.c` 也是这次加的：一个窗口 + 一次 `xdg-activation` 请求，令牌在
上屏时创建（= 弹通知）、过 N 秒才用它请求激活（= 点通知），serial 可以选「点击事件 /
0 / 不带」三种，`--no-token-surface` 模拟「通知守护进程建令牌时不带 surface」，
`--before-map` 模拟「请求比窗口上屏还早」—— 上面两张对照表都是它量出来的。

**顺手修掉的一个真 bug**：座位能力（`wlr_seat_set_capabilities`）只在真设备和虚拟键盘
挂上来时更新，**虚拟指针挂上来时没更新** —— 于是一个只有虚拟指针的会话里客户端被告知
「这个座位没有指针」，**永远收不到任何指针事件**（点不动、也拿不到点击的 serial，激活
请求就只能带 0）。`tools/activate.c` 第一次跑出来的是 `caps 0`，修完是 `caps 1` +
`activate click 10`。真机上因为有真设备所以看不出来，headless / 只靠虚拟指针的会话里
是必现的。

## 构建

```bash
meson setup build
ninja -C build
```

依赖：`wlroots-0.20`（Arch: `wlroots0.20`）、`wayland-server`、`wayland-client`、
`xkbcommon`、`wayland-scanner`、`xcb`（XWayland 的诊断走 X 连接）、`pixman-1`、
`egl` + `glesv2`（装饰 / 模糊 / 自定义 shader 走 GLES2）、`lua5.4`、
`libpipewire-0.3`（录屏 portal 要合成器自己建 PipeWire 流，见 `src/screencast.c`）、
`gtk+-3.0`（vidow 自己的 portal 后端 `vidow-portal` 用它画选择器，见 `tools/vidowportal.c`）。
运行 X11 程序还需要 `xorg-xwayland`（Arch 的 `xorg-xwayland`）。

注意：Arch 的 `wlroots0.20` 包没有安装 `wlr-layer-shell-unstable-v1-protocol.h`（而
`wlr/types/wlr_layer_shell_v1.h` 会 include 它），所以协议 XML 放在
`protocols/` 里，构建时用 `wayland-scanner` 现场生成头文件。

## 运行

一个脚本起（推荐，两个后端都行，退出时自己收干净）：

```bash
./run.sh                     # 真机会话（tty / DRM 后端；WAYLAND_DISPLAY 没设时的默认）
./run.sh --nested            # 嵌在当前 Wayland 会话里（一个窗口）
./run.sh --portal            # 起来之后跑 ./portaltest.sh（体检 + 一个真请求）
./run.sh --portal-check      # 只体检，不动屏幕
./run.sh --debug             # VIDOW_LOG=debug（日志不过滤，要过滤用 ./dbg.sh）
./run.sh --no-portal-env     # 用临时配置起：不碰 portal（代价：你的 config.lua 也不生效）
./run.sh --print             # 只打印要跑的命令
```

退出时（Ctrl+C、vidow 自己退、脚本被杀）它会做三件事：**收 vidow**（SIGTERM → 5 秒 →
SIGKILL，连进程组）、**扫残留客户端**（环境里 `WAYLAND_DISPLAY` 正好是它那个 socket 的
进程 —— 这类孤儿会把整台机器拖慢，实测两个 `wev` 各 100% CPU 烧了 25 分钟）、**看有没有
新 coredump**（有就把栈的前几帧打出来，免得它悄悄崩）。日志默认 `/tmp/vidow-run.log`，
`tail -f` 跟。

手动起也行。嵌套在现有 Wayland 会话里跑（随时可关）：

```bash
WLR_BACKENDS=wayland WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128 ./build/vidow
```

`WLR_RENDER_DRM_DEVICE` 这一半不能省：wlroots 0.20.2 的 GLES2 渲染器要一个 DRM fd，而
wayland 后端自己不提供 —— 只写 `WLR_BACKENDS=wayland` 会停在
`Cannot create GLES2 renderer: no DRM FD available`（`./run.sh --nested` 会自动补上）。

真机（tty，DRM 后端，不带 `WLR_BACKENDS`）上 **Ctrl+Alt+F1..F12 切 tty**：内核一般
自己就把这个键处理掉了（合成器不 `EVIOCGRAB`，控制台键盘那一侧也收得到），所以这条
通常是空转 —— 但 vidow 自己也接一遍（X 服务器就是自己实现的：它抓了设备，内核收不到
这个键），两种情况下都能用：

```lua
-- 想换别的键（比如 F 键被别的东西占了）：
vidow.bind("Ctrl+Alt+P", "switch-vt 3")
```

命令行也行：`vidowmsg action switch-vt 3`。切换本身**延后一轮事件派发**再做
（`wlr_session_change_vt()` 是同步的，一进去 wlroots 就把所有输出销毁、输入设备挂起 ——
在键盘事件处理中途拆掉半个会话，正是「切走再切回来卡死」最容易踩的那一脚）。
会话不活动时 wlroots 的 DRM 后端自己会停往 DRM 上画；切回来时 DRM 后端会把所有输出
**销毁再重建**，而这时候场景里一点 damage 都没有、后端也不欠我们 frame 事件 ——
所以 vidow 在会话恢复时主动催一帧（`vidow_output_kick_all()`），不然就是「切回来
卡死」。**嵌套 / headless 里没有会话**，
这条动作会明确失败（日志：`VT: 没有会话（嵌套 / headless），切 VT 交给宿主`），而且
Ctrl+Alt+F2 不会被吃掉（那是宿主 Hyprland 的）。回归判据 `test-vt.sh`。

一键验证（嵌套启动 + 3 个客户端 + waybar 顶栏）：

```bash
./run.sh --nested --portal-check     # 起 vidow（嵌套）+ 体检 portal 那条链，退出时收残留客户端
./run-nested.sh              # 默认 kitty，可换 ./run-nested.sh alacritty
./test-keys.sh               # wtype 注入按键，验证焦点/滚动/列宽
python3 test-float.py        # 三个真实 GTK 窗口，验证浮动模式自动识别
./build/vdrag 7000 3000 9500 5000   # 虚拟指针拖拽（归一化 0..10000 坐标）
./test-portal.sh switch-gtk          # 从 ~/.config/vidow/portals.conf 走生成流程再验证
./test-portal.sh switch-kde          # 同上，换成 KDE
./test-portal.sh raw-gtk             # vidow 不碰 portal 配置，手写一份单独验证协议
./test-portal.sh raw-kde             # 同上，换成 KDE
./test-portalenv.sh                 # 启动推进 / 退出还原 systemd 用户管理器环境 + 录屏后端探测（19 项断言）
./test-screencast.sh                 # KDE 的录屏 portal 全链路：协议 → 对话框 → PipeWire node → 真出帧（10 项断言）
VIDOW_TEST_RENDERER=gles2 ./test-screencast.sh
                                     # 同上，但走真 GPU（gles2）：额外验 dmabuf 直通（dmabuf_frames > 0、
                                     # readback_ms == 0）。默认 pixman 那条路没有 dmabuf，走回读
./test-dpi.sh                        # 混合 DPI + XWayland（全 headless，15 项断言；含「QT_SCALE_FACTOR 不许设」）
./test-x11scale.sh                   # X11 逐窗口缩放桥 x11_scale=1（全 headless，16 项断言）
./test-scrollcull.sh                 # 滚出视口的列 / 半块越界的列都不画到隔壁显示器；有人录屏 / 预览时也一样；静止窗口滚到视口外照样出帧（全 headless + grim 逐像素，36 项断言）
./test-floatstack.sh                 # 层级：浮窗永远压平铺、点浮窗换层级（全 headless + grim 逐像素，12 项断言）
./test-fullscreen-scroll.sh          # 全屏占**整块屏**（不吃 gap）+ 参与滚动：滑出去 / 隔壁列并排 / 整块滚出屏藏起 / 退全屏列宽回来（全 headless + grim 逐像素，24 项断言）
./test-fullscreen-2out.sh            # 同上，但在**混合 DPI 两块屏**上：全屏占自己那块整屏（2 倍屏上就是整个 4K）、越界那半块不画到隔壁屏（全 headless + grim 逐像素，16 项断言）
./test-scanout.sh                    # 全屏直通：默认配置下真的扫上屏 / 退全屏回到合成 / 切换全程窗口不消失 / 直通与合成两条路内圈逐像素一样（全 headless + grim 逐像素，11 项断言）
./test-mixdpi.sh                     # 跨屏拖窗口：拖着的那段就按对面那块屏的倍率烘（全 headless，7 项断言）
./test-vt.sh                         # tty 的 VT 切换（Ctrl+Alt+F1..F12 / switch-vt 动作）（全 headless，11 项断言）
./test-focus.sh                      # 焦点规则 + 激活请求（全 headless，60 项断言）
./test-click-focus.sh                # 点另一块屏的空区域：键盘焦点跟着过去（4 项，见 256）
./test-ipc.sh                        # IPC：状态输出 / 命令 / 事件 / 多屏（全 headless，162 项断言）
./test-hooks.sh                      # 事件钩子（config.lua 的 vidow.on）：命令钩子 / Lua 钩子 / "*" / 自定义事件 / 摘钩子 / 出错不连累别人（全 headless，29 项断言）
./test-events.sh                     # 事件的丰富度：keybind / ipc-action / window-activated / 拖拽 / 屏开关 / X11 起来 / 输入设备插拔 / 配置出错（全 headless，28 项断言）
./test-env.sh                        # 会话环境：和 systemd 用户管理器两个方向对账 + exec-once 自启动（全 headless，42 项断言）
./test-input-inject.sh               # 输入注入：鼠标 / 键盘合成事件到客户端（全 headless，54 项断言）
./test-pointer-lock.sh               # 指针约束：锁鼠标 / 夹光标 / 相对位移，X11 的 XGrabPointer 也认（全 headless，27 项断言）
./test-keyboard-grab.sh              # 键盘那两个协议：快捷键抑制（含逃生口）+ X11 的 XGrabKeyboard（全 headless，30 项断言）
./test-maximize.sh                   # 列最大化（niri 的 maximize-column）：客户端 set_maximized 也映射到它（全 headless，60 项断言）
./test-cursor-shake.sh              # 摇晃找光标：线性加 + 补间、晃到屏幕那么大，截图里没有它（全 headless，55 项断言）
./test-geometry.sh                   # 窗口几何和表面尺寸对不上时内容铺满框（chromium 改大小那 1-2 秒）（全 headless，6 项断言）
./test-viewport-crop.sh              # 客户端用 wp_viewport 只声明缓冲的一部分时，装饰按那块采（chromium，5 项断言）
./test-blurcache.sh                  # 拖重叠窗口时只重建「它底下那个」的底衬、且一次贴图都不重新导入（7 项断言）
./test-blurlive.sh                   # 屏幕另一头有窗口不停重画时，别处的模糊底衬不该跟着重抓（4 项断言）
./test-blursmall.sh                  # 客户端只动窗口里一小块时，只有真被它压着的那片模糊该重抓（6 项断言）
./test-blurpartial.sh                # 「模糊链条只重跑受损那一小块」和「整块重跑」逐像素一样（6 项断言）
./test-blurnooverlap.sh              # 窗口不再重叠之后，模糊底衬和「从没重叠过」时逐像素一样（5 项断言）
./test-blurinvisible.sh             # 内容整块不透明的窗口不算模糊（不抓底衬 / 不跑链条 / 不重烘），半透明的照旧（6 项断言）
./test-pixman-renderer.sh            # 软件渲染器（pixman）下不崩：装饰关掉、窗口照常上屏（5 项断言）
./test-blur2out.sh                  # 两块屏（1 倍 + 2 倍）下私有模糊区域的金字塔缓冲每帧都复用（6 项断言）
./test-config.sh                     # 配置系统：一个 Lua 文件 + 改了就自动重载 + vidow.exec 的选项（全 headless，23 项断言）
./test-binds.sh                      # 键位全是配置给的：没配置=没有键位、默认那套能用、换键位、动作字符串（全 headless，35 项断言）
./test-gesture.sh                    # 触控板手势：三指横扫跟手拖视口 / 四指竖扫 / 捏合 / 按住（全 headless，注入，25 项断言）
./test-widthanim.sh                  # 列宽过渡：cycle / 最大化 / IPC 改宽按 column_width_ms 补间，隔壁列不给位移补间（全 headless，30 项断言）
./test-config-include.sh             # 配置拆成好几份：vidow.include() 的路径/顺序/报错 + 改哪份都自动重载（全 headless，24 项断言）
./test-defaultconfig.sh              # 默认配置按类别拆成 configs/（--default-config / --write-config）+ 配置族的表写法 + 说明的完整性（全 headless，44 项断言）
./test-gap.sh                        # 间距：一个数管全部 + 单边/内间距覆盖 + 「两条 50% 正好装进一屏」（全 headless，34 项断言）
./test-layerblur.sh                  # 半透明状态栏背后的模糊（全 headless，4 项断言）
./test-blurscale.sh                  # 模糊在高 DPI 倍率下的底衬覆盖 + 对位（1/1.7/2 倍，7 项断言）
./test-blurlevel.sh                  # 半径随级数扩展（2/4/6 级各跑一轮，3 项断言）
./test-blurxray.sh                   # blur_xray：重叠时用整屏底衬（不建私有底衬）+ 语义真的变了（6 项断言）
./test-layerghost.sh                 # 会被模糊的 layer surface 不进自己的底衬（3 项断言）
./test-layerwin.sh                   # top 层模糊里含窗口（纯蓝窗口 + 压在它上面的面板，4 项断言）
./test-layermask.sh                  # 按元素自己的 alpha 剔除（透明内边距里的亮点不糊，3 项断言）
./test-layerdestroy.sh               # layer 没了要收干净它的模糊节点（反复重建不崩不漏，9 项断言）
./test-layerstorm.sh                 # 一批 layer 面一起上屏：动画每帧不许重跑布局灌 configure（两块屏，6 项断言）
./test-openanim.sh                   # 开窗动画画的是装饰后的窗口本身（不是一块更大的矩形，4 项断言）
./test-bounce.sh                     # 入场过冲 / 关闭回弹（q 弹曲线真的落到屏幕上了，5 项断言）
./test-closeanim.sh                  # 关闭动画：默认曲线下还看得见 + 不带窗口装饰 + 隔壁那块的位移补间中段还在路上（17 项断言）
./test-focusclose.sh                 # 关窗口后焦点落谁身上：右边最近的顶上（21 项断言）
./test-lastoutput.sh                  # 屏没了不许留野指针、没屏时开窗不许崩（拔一块 / 最后一块没了，21 项断言）
./test-actsurface.sh                  # 激活令牌攥着的 surface 先没了也不能崩（8 项断言）
./test-idlewake.sh                   # 空载一次都不该醒（抓帧看门狗按需启动）+ 静止屏幕抓帧照样成（7 项）
./test-frametrace.sh                 # 帧追踪：某块屏的帧链断了（窗口停在最后一帧）要报出来（17 项断言）
./test-dmabuf-update.sh              # dmabuf 客户端（kate）的新画面必须真的上屏（5 项断言）
./test-preinit.sh                    # 状态请求抢在第一次 commit 之前（set_maximized /
                                     #   set_minimized / set_fullscreen）不许把合成器打死（22 项断言）
./test-drag.sh                       # 拖窗口：拖得动 / 跨屏改归属 / 平铺按落点插入（10 项）
./test-mini.sh                       # 小窗模式（mini window，S1–S8）：三态与交互 / 跟屏 / 焦点守卫 / 输入路由与 4px 阈值 / 图标裁剪 / 原生 buffer 与坐标换算 / mini_aspect / 落点区 / 工具条 / 出帧分叉 / 过渡动画与惯性甩出；S7 HiDPI（scale 2：逻辑工作区 / 跨缩放边界裁剪 / 指针坐标换算）；S8 自由小窗（拖动跟手 / 拖动中跨屏换归属贴边 / 全屏可达 / 能拖出屏边一半在外 / resize 真重配客户端）（全 headless 双屏，见 277、289）
./test-corner.sh                     # 圆角形状：corner_n = 4 超椭圆（角区面积 957 / 对角线第一颗第 5）vs 2 圆弧（815 / 第 9）/ 边框环跟同一形状（沿对角线 11 vs 解析 10.75）/ AA 过渡 1~2px（20 项，见 280）
./test-drag-insert.sh                # 拖平铺窗口的落点：插进已有列 / 新开一条列（左右两边的缝都算）/ 落点预览块 / 条带比屏宽时拖到屏边自动滚条带（30 项，见 288）
./test-expel.sh                      # 把窗口从列里拎出来单独成列（niri 的 expel-window-from-column：动作 / IPC / 左右 / 拖到右边与空白处 / 取不出来的两种情形，32 项）
./test-drag-stall.sh                 # 后端不给帧事件时，拖动照样跟手（浮窗 + 平铺两个case，9 项断言）
./test-drag-follow.sh                # 拖动跟手 + 松手后把指针位置补给客户端（8 项断言）
./test-popup2out.sh                  # 不在布局原点上的窗口，弹出物要摆在自己那块屏上（13 项断言）
./test-ws-dynamic.sh                 # 动态工作区：栈底空位 / 空了就收 / 序号夹住 / 具名永存（9 项）
./test-dpi.sh                        # 混合 DPI + XWayland：装饰缓冲按**窗口几何**算（见 SSD 那节）
./test-emptycol-nomap.sh             # 「凭空多出一列」：从没 map 过的 X11 窗口不许占列（同一进程里再放一个上屏的当对照 + 延迟到首次 map 补列）（全 headless + Xwayland，14 项断言，见 249）
./build/jsontest                     # 只看 JSON 写出/读入（离线，30 项）
./build/scaletest                    # 只看缩放计算（31 项）
```

无头冒烟测试（不弹窗口）：

```bash
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1 ./build/vidow
```

直接接管 TTY（会抢 DRM/input，需先退出当前图形会话）：

```bash
./build/vidow
```

环境变量：

| 变量 | 作用 |
| --- | --- |
| `VIDOW_LOG` | `info`（默认）/ `debug` / `error` |
| `VIDOW_NO_SCANOUT` | 关掉全屏直通（一律走合成那条路）。排查「全屏画面和以前不一样」时先拿它排除直通，见 `./test-scanout.sh` |
| `VIDOW_TERMINAL` | `Super+Enter` 启动的命令，默认 `kitty` |
| `VIDOW_BLUR_DUMP=N` | 前 N 份底衬 / 模糊图 / layer 区域抓取 dump 成 `/tmp/vidow-{bd,blur,lbd,lblur}-*`（裸 ARGB，测试用）。`bd`/`blur` 是**被抓的那块区域**那么大（重叠窗口抓的是「窗口 ± 模糊半径」那块，不是整屏；layer 那条路一直是区域大小），每份配一个同名 `.txt` 写宽高 |
| `VIDOW_FX_DUMP=N` | 前 N 份装饰离屏缓冲 dump 成 `/tmp/vidow-fx-*` |
| `VIDOW_MASK_DUMP=N` | 前 N 份透明度剔除的 mask（layer 自己的内容）dump 成 `/tmp/vidow-mask-*` |
| `VIDOW_BLUR_NO_PARTIAL=1` | 关掉「模糊链条只重跑受损那一小块」（强制整块重跑）。只给 A/B 比对用：局部和整块的结果必须逐像素一样，见 `./test-blurpartial.sh` |
| `VIDOW_BLUR_NO_PARTIAL_FILE=路径` | 同上，但那个文件一出现才切过去 —— 同一个进程、同一个场景里就能比对两种方式（test-blurpartial.sh 用的就是这个） |
| `VIDOW_RENDER_STATS=1` | 每 500ms 一行渲染统计：每帧各阶段耗时（模糊 / 装饰 / 果冻 / 提交）、私有底衬重建次数、**vidow 自己那些目标缓冲的贴图导入次数**、离屏趟数（`趟`）、装饰重烘总耗时（`重烘`）、**拖拽延迟（`拖拽延迟 平均/最大ms(次数)`：从最后一个指针移动事件到这一帧出帧）**、帧率。查「拖窗口卡」先在真机上跑这个看每帧干了多少活，再谈优化 |

## 快捷键

`Super` = `Mod`（`WLR_MODIFIER_LOGO`）。

**合成器一个键位都不内置**：下面这一套是默认配置给的（`configs/14-keys.lua`），
改哪个键、删哪个键都由你 —— 没绑的键原样透传给客户端。键位就是配置里的一行：

```lua
vidow.bind("Mod+Q", "close-window")        -- 动作字符串见下面那张表
vidow.bind("Mod+Return", "spawn kitty")    -- spawn 后面跟一条 shell 命令
```

**触发怎么写**（一张表收三类：键盘键、鼠标按键、滚轮）：

```lua
vidow.bind("Mod+Q", "close-window")                     -- 键盘键（按下触发，默认）
vidow.bind("Mod+mouse-left",  "interactive-move")       -- 鼠标左键
vidow.bind("Mod+mouse-right", "interactive-resize")     -- 鼠标右键
vidow.bind("Mod+wheel-up",    "focus-column left")      -- 滚轮（一个方向一条）
vidow.bind("Mod", "toggle-overview")                    -- 单独一个修饰键
vidow.bind("Super_L", "toggle-overview")                -- 只认左边那个 Super
```

鼠标按键写 `mouse-left` / `mouse-middle` / `mouse-back` / `mouse-forward`（也认
`BTN_LEFT` 和 `mouse-274` 这种编号）——**别写 `left`**，那个在键名表里是**方向键**。
滚轮写 `wheel-up` / `wheel-down` / `wheel-left` / `wheel-right`。

**单独一个修饰键**（`vidow.bind("Mod", …)`）是 **tap 语义**：按下武装 → 期间按下任何
别的键或鼠标键就作废 → 松开（且其它修饰键都松了）才触发。不这样定的话 `Super+Q` 会
顺带把「只按了 Super」那条也跑掉。写 `Super_L` / `Super_R` 可以只认某一侧。

第三个参数（可选，选项表）现在有两个字段：

```lua
vidow.bind("Mod+Escape", "quit", { inhibited = true })
vidow.bind("Mod+W", "close-window", { on = "release" })  -- 松开才触发
```

`on = "press"`（默认）/ `"release"`：松开触发的那条**按下那一下也一起吞掉**（只吞松开
的话客户端会以为这个键一直按着）。单独一个修饰键默认就是 `release`。

意思是「焦点那个表面**抑制着快捷键**时这条照样生效」（默认 `false`）。抑制来自
`zwp_keyboard_shortcuts_inhibit_manager_v1` —— 虚拟机的窗口 / 远程桌面客户端会说
「别吃我的快捷键」，好让客人系统收到 Super、Alt+Tab 这些键（见「快捷键抑制」一节）。
抑制期间默认**所有**键位让路（不然那个客户端要抑制就没意义了），所以退出这类救命键
最好留个逃生口 —— 默认配置里 `Mod+Escape` 就是这么绑的。

| 按键（默认配置给的） | 动作 |
| `Mod+H` / `Mod+L` | 焦点左/右移一列（视口跟随滚动） |
| `Mod+J` / `Mod+K` | 焦点在列内下/上移；单窗口列则跳到下/上一列 |
| `Mod+Shift+H` / `Mod+Shift+L` | 当前列左/右移一位 |
| `Mod+Shift+J` / `Mod+Shift+K` | 列内窗口下/上移；单窗口列则塞进下/上一列 |
| `Mod+V` / `Mod+Shift+Space` | 当前窗口 平铺 ⇄ 浮动（窗口正全屏时先把全屏退掉 —— 见 changelog 186） |
| `Mod+左键拖动` | 拖窗口：浮动窗口跟着走（松手在哪块屏上就归那块屏的工作区）；平铺窗口按**落点**归位 —— 落点分两种（niri 的 NewColumn / InColumn）：落在**列带的缝上 / 空白处**（离列的左边缘近）= 把**这个窗口自己**取出来、在那儿新开一条列（它原来那条列连同别的窗口留在原处；列里就它一个时整条列搬过去），落在**窗口身上**（离那条列里的窗口缝近）= 插进那条列、一列里上下叠起来。拖的过程里落点画一块半透明预览（见「拖平铺窗口的落点」一节），拖到别的屏上 = 跨屏搬过去（那条路还是整条列） |
| `Mod+右键拖动` | 浮动窗口：缩放它自己（从右下角开始）；平铺窗口：改这一列的宽度（列宽右边缘跟着光标走，两头夹在 5%…100%） |
| `Mod+R` | 列宽循环（默认 1/3 → 1/2 → 2/3，按 `preset_column_widths` 配的那几档） |
| `Mod+F` | 当前窗口全屏切换 |
| `Mod+Shift+F` | 当前列最大化 ⇄ 还原（见「列最大化」一节） |
| `Mod+O` | niri 风格全景（overview）：整条列带缩小铺满这块屏，点谁去谁 |
| `Alt+Tab` / `Alt+Shift+Tab` | 最近窗口切换器（niri 的 recent windows）：**按住**列出最近用过的窗口预览，连点换一个，**松开**把焦点给选中的那个（见「niri 风格的 Alt+Tab」一节） |
| `Alt+\`` | 只在**当前这个应用**的窗口之间切（`next-window same-app`） |
| `Mod+Q` | 关闭当前窗口 |
| `Mod+M` / `Mod+Shift+M` | 收起当前窗口 / 放回最近收起来的那个（见「最小化」一节） |
| `Mod+Enter` | 启动终端 |
| `Mod+滚轮` | 焦点左/右移一列 |
| `Alt+滚轮` | 工作区上/下切一块 |
| `Alt+Shift+滚轮` | 把当前窗口搬到上/下一块工作区（`move up follow`：**跟着走**） |
| `Ctrl+Alt+滚轮` | 和左右相邻的窗口换位置（滚上 = 和左边那个换） |
| `Mod+Escape` | 退出合成器（绑成 `{ inhibited = true }`：抑制期间也生效，见下节） |
| `Mod+U` / `Mod+I` | 工作区上/下切一块（niri 的默认键位） |
| `Mod+Shift+U` / `Mod+Shift+I` | 把当前窗口搬到上/下一块工作区 |
| `Mod+Ctrl+U` / `Mod+Ctrl+I` | 整块工作区上/下移一位（重排） |
| `Mod+1`…`Mod+9` / `Mod+Shift+1`…`9` | 切到这块屏上第 n 块 / 把窗口搬过去（越界夹到最后一块） |
| `Ctrl+Alt+F1`…`F12` | 切 tty（只有真机、有会话时才接管；嵌套里这组键交给宿主） |
| — | 特殊工作区 / 具名工作区**默认不绑**，要就自己 `vidow.bind("Mod+C", "workspace chat")` |

指针手势（`Mod+左键拖动` / `Mod+右键拖动` / `Mod+滚轮`）**就是键位**（见上面那张表）：
默认配置里绑的是 `interactive-move` / `interactive-resize`（目标 = 光标底下那个窗口）
和 `focus-column left|right`。合成器**一个手势都不内置** —— 不绑就没有，想换键就改
`vidow.bind("Mod+mouse-…")`，想换动作就换成别的动作字符串。`Mod+右键拖动`在浮动窗口
上改的是它自己的几何，在平铺窗口上改的是**列宽**（平铺窗口的高度是布局给的：整块可用
高，没有「单个窗口的高度」这个概念，所以纵向拖不改东西）。

没绑的滚轮照旧转发给客户端（应用里的滚动不受影响）。

鼠标左键点击窗口即聚焦并滚动到该列；点击 layer 表面（启动器）时按需交出键盘焦点。

overview 里：左键点窗口 = 选中它并缩回去，点空白 = 取消，`Esc` 退出，滚轮左右平移相机
（`Mod+H/L` 照样换列，换列会把相机平移清零、改回跟着焦点走）。

切换器开着（按住修饰键）期间：`Tab` 往前走、`Shift+Tab` 往后走、`Esc` 取消、`Enter` 立刻
确认；这期间键盘不给客户端（窗口马上就要换，给谁都可能给错）。

普通滚轮事件转发给客户端，不用于滚动视口（用 `Mod+滚轮` 或 `Mod+H/L`）。

## 触控板手势（`vidow.gesture{}`）

```lua
vidow.gesture{ fingers = 3, swipe = "view" }                     -- 连续：跟手拖视口
vidow.gesture{ fingers = 4, direction = "up",   action = "workspace up" }
vidow.gesture{ fingers = 4, direction = "down", action = "workspace down" }
vidow.gesture{ fingers = 2, pinch = "out",      action = "cycle-column-width" }
vidow.gesture{ fingers = 2, pinch = "in",       action = "cycle-column-width" }
vidow.gesture{ fingers = 3, hold = true,        action = "toggle-overview" }
```

和键位一个规矩：**合成器一个手势都不内置**，动作字符串也是同一套（`vidow_action_run`）。
一条手势 = `fingers` + 「哪种手势」（`swipe` / `direction` / `pinch` / `hold` **写且只写一个**）
+ `action`（`swipe = "view"` 那条不用动作，它自己拖视口）。

| 写法 | 语义 |
| `swipe = "view"` | **连续**：手指拖着条带走，视口跟着动；松手按拖了多远决定「吸附到相邻那条列」（≥ 100px）还是「弹回原位」（160ms 缓动）。拖的过程中**不换焦点**，也不跑 arrange 里那套「把焦点列滚回可见区」——手指说了算 |
| `direction = "left\|right\|up\|down"` | **离散**：整个手势结束后看累计位移的主方向对不对得上，对得上且够远（`distance`，默认 300）才触发 |
| `pinch = "in\|out"` | 捏合：`scale` 相对手势开始偏离 ≥ 0.1 才算 |
| `hold = true` | libinput 的 hold（手指按住不动）：`hold_begin` 当场触发 |

单位：libinput 的手势位移是**归一化到 1000dpi** 的（1 单位 ≈ 0.025mm，手指划 1cm ≈ 394
单位）。`speed`（只对 `swipe = "view"`）是「1 单位 → 多少逻辑像素」，默认 0.3（= 手指
1cm ≈ 屏幕 118px）；`distance`（只对 `direction`）是阈值，单位同上。

libinput 的手势只在**触控板**上发（鼠标 / 触摸屏不发），headless 里也没有 —— 测试走
注入那条路：`vidowmsg gesture swipe-begin 3` / `gesture swipe-update 3 dx dy` /
`gesture swipe-end [--cancel]` / `gesture pinch-begin 2` / `gesture pinch-update 2 scale` /
`gesture pinch-end` / `gesture hold-begin 3` / `gesture hold-end`，注入直接调
`src/gesture.c` 的同一批函数，判据见 `./test-gesture.sh`。

## 快捷键抑制与 X11 的键盘抓取

两个协议，都在 `src/keyboardgrab.c`，判据 `./test-keyboard-grab.sh`（30 项）：

**`zwp_keyboard_shortcuts_inhibit_manager_v1`** —— 客户端说「我这个表面有焦点的时候别吃我的
快捷键」。虚拟机的窗口（looking-glass 那类）、远程桌面客户端要的就是这个：客人系统里按
`Super`、`Alt+Tab`、`Ctrl+Alt+...` 得原样进去。策略：

- 客户端一要就给（`wlr_keyboard_shortcuts_inhibitor_v1_activate`），**但只有抑制的那个表面
  正是键盘焦点时才算数**（协议原话：inhibitors 只在它的表面有焦点时生效）。IPC 里看
  `input.shortcuts_inhibited`。
- 算数的时候 vidow 自己的键位**全部让路**，按键原样送给客户端。要留逃生口就
  `vidow.bind(键, 动作, { inhibited = true })` —— 默认配置里 `Mod+Escape`（退出）就是这么绑的，
  不然一个抑制着的窗口能把用户锁死在里面。

**`zwp_xwayland_keyboard_grab_manager_v1`** —— X11 的 `XGrabKeyboard`。wlroots 0.20 里连头文件
都没有，所以服务端自己写（协议 XML 在 `protocols/xwayland-keyboard-grab-unstable-v1.xml`，
和 ext-background-effect 一样现场生成 server-header + private-code）。策略：

- Xwayland 建一个 grab 对象 → 把**座位键盘焦点**按到它那个表面上（只挪 seat 焦点，
  `server->focused` 和焦点边框不动，和弹出物抓键盘一个规矩）。IPC 里看
  `input.keyboard_grab`（`{type: xwayland, window}`）。
- 之后每次要送按键都再确认一遍：**谁抓谁收，不管焦点在谁身上**（X11 的语义）—— 用户点了别的
  窗口，键还是给抓着键盘的那个 X 客户端。vidow 自己的键位照常生效（协议明说合成器可以过滤
  一部分自己用）。
- 抓取对象销毁（客户端 `XUngrabKeyboard` / 连接断了 / 窗口没了）→ 焦点还给 `server->focused`。
- 访问限制：协议要求「只给 Xwayland」，所以只接受 Xwayland 的表面
  （`wlr_xwayland_surface_try_from_wlr_surface`），别的客户端来要就回协议错误。

## 混合 DPI 与 XWayland

目标很具体：**同一个逻辑尺寸，在两块 DPI 不同的屏上是同一个物理尺寸，而且都清晰。**
缩放就是这一条：逻辑像素 = 1/96 英寸。

```lua
-- ~/.config/vidow/config.lua
scale = "auto"                      -- 所有输出的默认缩放 = ppi / 96

-- 显示器：一块屏一组，缩放和摆在哪写在一起（逻辑坐标）
vidow.output{ name = "DP-3", scale = 2, x = 0,    y = 0 }    -- 左边那块 4K
vidow.output{ name = "DP-2", scale = 2, x = 1920, y = 0 }    -- 右边那块 4K
vidow.output{ name = "HDMI-A-1", scale = 1.25 }              -- 只改缩放，位置自动排

xwayland = true                     -- 起 XWayland（懒启动：真有人连 X 才 fork）
xwayland_scale = "auto"             -- X 的全局缩放 = ceil(最密那块屏的缩放)
xwayland_env = true                 -- 给 X11 客户端下发 GDK_SCALE / QT_SCALE_FACTOR
```

`vidow.output{}` 每写一行就是一块屏，只写要管的字段（`name` 必须有）：

| 字段 | 作用 |
| --- | --- |
| `name` | 哪块屏（连接器名，`vidowmsg outputs` 能看到）。必须写 |
| `scale` | 这块屏的缩放：`auto` / `off` / 一个数，不写就用全局 `scale` |
| `x` / `y` | 摆在布局的哪里，**逻辑**坐标；不写 = 按输出到达顺序自动排（那个顺序是后端说了算，所以「哪块在左边」要确定就得写） |
| `mode` | 模式：`"3840x2160@144"` / `"3840x2160"`（刷新率不挑）/ `"3840x2160@59.94"`。列表里没有就按自定义模式试 |
| `enabled` | 这块屏开不开（`false` = 关掉它，上面的窗口都留着，再打开照原样回来） |
| `vrr` | 可变刷新率（adaptive sync）：`true` / `false`。后端或这块屏不支持时只报一句，别的设置照常生效 |
| `depth` | 输出缓冲的位深：`8`（默认）/ `10`。10 位走 `XRGB2101010`，要显卡 + 显示器 + 渲染器都支持 |
| `transform` | 方向 / 翻转：`"normal"` / `"90"` / `"180"` / `"270"` / `"flipped"` / `"flipped-90"` / `"flipped-180"` / `"flipped-270"` |

名字写错了（`scalee = 2`）、值写错了（`mode = "800x600x2"` / `depth = 12` /
`transform = "sideways"`）都会当场报错并列出能写什么；同一块屏写两行也报错。
每一块屏那一组只写你要管的字段 —— 没写的那些 vidow 一概不动（`scale` 不写就用全局
`scale`，位置不写就自动排，`vrr` / `depth` / `transform` / `mode` 不写就保持现状）。

改完配置不用重启：`kill -HUP $(pgrep -x vidow)`，上面每一项都会对一遍账（只在
真的不一样时才动那块屏）。

`scale = auto` 按 EDID 的物理尺寸算 `ppi / 96`：4K 27 寸 ≈ 1.70、1440p 27 寸 ≈ 1.15、
1080p 24 寸 ≈ 0.96（夹到 1.0）。量化到 0.05 再夹进 1.0～8.0 —— `ppi/96` 会算出
1.7037037 这种值，直接拿去当缩放每帧取整都不一样。

`./build/scaletest` 是不启动合成器的离线自检（31 项），其中一项就是实算「同一个逻辑
尺寸在两块屏上的物理尺寸差多少」：

| 两块屏 | 200 逻辑像素的物理尺寸 | 差 |
| --- | --- | --- |
| 4K 27" + 1080p 24"，`auto`（1.70 / 1.00） | 2.081 in / 2.178 in | 4.65% |
| 同上，手写整数缩放（2 / 1） | 2.449 in / 2.178 in | 11.0% |

| 键 | 默认 | 作用 |
| --- | --- | --- |
| `scale` | `auto` | 所有输出的默认策略：`auto` = 按 ppi 推，或者写一个数（`1.75`）；单块屏点名用 `vidow.output{ name = …, scale = … }` |
| `xwayland` | `true` | 起不起 XWayland（`0` = 完全不碰 X 那一摊） |
| `xwayland_scale` | `auto` | `auto` = `ceil(最密那块屏的缩放)`；`off` = 一律 1:1；也可以写死一个数 |
| `xwayland_env` | `true` | 给 X11 客户端下发 `GDK_SCALE` / `QT_AUTO_SCREEN_SCALE_FACTOR` / `XCURSOR_SIZE`（用户自己显式设过的不抢）。**不设 `QT_SCALE_FACTOR`**：Wayland 的 Qt 也认它，会把所有屏上的 Qt 窗口乘一遍（见下）。**不管** `_XWAYLAND_GLOBAL_OUTPUT_SCALE` 和 `Xft.dpi` —— 那两个是 X 服务自己的事实，vidow 总是自己写 |

### 跨屏拖动：拖着的那一段也按对面那块屏的倍率烘

窗口的装饰 / 着色器是渲进一块**离屏缓冲**再上屏的，那块缓冲按「窗口现在看得见的那些屏里
**最大**的倍率」分配（`src/view.c` 的 `view_scale()`）—— 拖到 4K 屏上那一段就是按 2 倍烘
的，不会「一路糊着、松手才清楚」。

判据是「**画在**哪块屏上」，不是「**属于**哪块屏的工作区」（`view->output`）：后者要到
松手才换，而人已经在对面那块屏上看着了。取最大倍率而不是「面积最大的那块」：客户端一碰到
2 倍屏就按 2 倍重画了，我们按 1 倍烘的话它交上来的 2 倍缓冲还得先缩下去、屏幕上再放大
—— 还是糊；宁可多烘（画到 1 倍屏上时是缩下去，仍然清晰）。私有模糊底衬归哪块屏
（`fx.c` 的 `view_blur_owner()`）走的是同一个判据：一个窗口「现在算在哪块屏上」只有一个
答案，两处各算各的就会让模糊贴图的采样坐标落错地方。回归：`./test-mixdpi.sh`。

### 模式、刷新率、VRR、位深（每块屏那一组里的其余项）

```lua
vidow.output{ name = "DP-3", mode = "3840x2160@144", vrr = true, depth = 10 }
vidow.output{ name = "DP-2", mode = "3840x2160@60", transform = "180" }
vidow.output{ name = "HDMI-A-1", enabled = false }        -- 接着投影仪才开
```

- **模式 / 刷新率**：`mode = "WxH[@Hz]"`。vidow 先在那块屏报出来的模式表里找（`vidowmsg
  outputs` 的 `modes` 就是这张表），找到就按它上；找不到才退回自定义模式（有些后端支持
  自定义模式，有些会拒 —— 拒了日志里会说）。`@Hz` 可以省，省了就是「分辨率对上就行，
  刷新率用那块屏的首选」。
- **VRR**：`vrr = true` 开可变刷新率。**先问再试**：vidow 看 `adaptive_sync_supported`
  （DRM 那边就是连接器的 `vrr_capable`），不支持就直接报一句「这块屏 / 这个后端不支持
  VRR」，不会拿一次注定失败的 commit 去撞。headless 后端在 wlroots 里根本不接受这个
  字段，所以 headless 上永远是那句「不支持」。
- **位深**：`depth = 10` 把输出缓冲换成 `XRGB2101010`。10 位要显卡、显示器、渲染器
  三样都到位；缺一样 vidow 就保持 8 位并在日志里说清楚。`vidowmsg outputs` 里
  `depth` / `render_format` 报的是**现在真的在用**的那个。
- **方向**：`transform` 和 `vidowmsg output_transform` 是同一套名字。
- **开关**：`enabled = false` 关掉一块屏 —— 上面的窗口、工作区、焦点都留着（DRM 那边
  就是关 CRTC），再打开照原样画回来。

**HDR：现在做不了，卡在 GLES2 渲染器 —— 不在 DRM 那边。**

（更正：之前这里写过「wlroots 0.20.2 里没有任何后端会填 `supported_primaries`」，那是
**错的** —— 那只是 headless 后端的现象。下面这段是按 0.20.2 的源码重写的。）

wlroots 0.20.2 的 DRM 后端这条链其实是齐的：

1. `backend/drm/util.c` 读 EDID：连接器有 `Colorspace` 属性 + EDID 报 BT2020 →
   `supported_primaries |= BT2020`；有 `HDR_OUTPUT_METADATA` 属性 + EDID 的 HDR static
   metadata 报 type1 + PQ → `supported_transfer_functions |= ST2084_PQ`。
2. `backend/drm/atomic.c` 把 image description 真的写进内核：`HDR_OUTPUT_METADATA` blob、
   `Colorspace`（BT2020_RGB = 9），并按缓冲格式选 `max bpc`（`XRGB2101010` → 10 位）。
3. `types/scene/wlr_scene.c` 按 image description 现算这一帧的 color transform（sRGB →
   目标 primaries 的绝对比色矩阵 × 亮度缩放，再套目标传输函数的反 EOTF），交给这一帧的
   render pass。

墙在这里：**这个 color transform 只有 Vulkan 渲染器认。** `render/gles2/renderer.c` 里
写死 `features.output_color_transform = false`（master 上也还是 false），GLES2 的
`begin_buffer_pass` 根本不看 `options->color_transform` —— wlr_scene 算好的变换被丢掉，
缓冲里还是 sRGB 编码的像素，DRM 那边却已经按 PQ / BT2020 上了屏：画面发灰。这就是
「假 HDR」的样子，也正因为这样 vidow 不给 `hdr = true` 这种开关。（同理
`input_color_transform = false`：GLES2 也没法把客户端声明成 PQ / BT2020 的内容转回来。）

真要做，两条路，都不小：

- **换 Vulkan 渲染器**：`fx.c` 那 2300 行原生 GL（`glCreateShader` /
  `wlr_gles2_renderer_get_buffer_fbo` / 自己的 FBO 管线）得整个重写。
- **vidow 自己出最后一趟**：场景先渲进离屏缓冲，再用自己的全屏着色器做 sRGB → linear →
  BT2020 → PQ 编码写进输出缓冲，同时给输出设 image description。这能出「整屏 HDR 容器」
  （SDR 内容统一映射上去），但客户端真正的 HDR 内容（mpv 的 PQ 视频）还是过不去 ——
  GLES2 没有 per-surface 的输入变换。

另外 headless / wayland 后端这两个字段都不填，所以 headless 上任何 image description 都被
`Unsupported image description primaries` 拒 —— HDR 这条路只能靠真显卡 + 真 HDR 屏验证。
`vidowmsg outputs` 现在会报每块屏自己声明的 `primaries` / `transfer_functions` /
`hdr_capable`：**这台机器能不能做 HDR，先看这三个字段**（两个都有才是真能做）。

完整调研（每条结论的 wlroots 源码出处、两条路线的步骤与工作量、真机验证清单、复现调研的
命令）见 [`docs/hdr-feasibility.md`](docs/hdr-feasibility.md)。

### 一条链：从输出缩放到清晰的像素

```
output scale s（每块屏一个，按 ppi 算出来）
  ├─ Wayland 客户端：wp_fractional_scale_v1 通知精确比例 + wl_surface.preferred_buffer_scale
  │                  = ceil(s) → 客户端自己按 s 渲染 → 到屏上 1:1
  ├─ 装饰 / 模糊管线：离屏缓冲按「逻辑尺寸 × s」分配，场景节点仍是逻辑尺寸 → 到屏上 1:1
  └─ X11（XWayland）：X 只有一套坐标，只能用一个全局缩放 S = ceil(max s)
                     把 X11 像素声明成「参考设备像素」：缓冲区 / S = 逻辑尺寸
```

装饰管线这一条是必须改的：着色器原来在逻辑尺寸的缓冲上画，再由输出放大 —— 缩放一
变大就糊。现在离屏缓冲按 `逻辑尺寸 × s` 分配（圆角、边框、阴影、内边距也乘 s），
着色器里的 `u_origin` 换成「本输出的物理像素坐标系里、减掉输出原点的窗口位置」。

### X11 那半边

X11 没有 per-window 的缓冲区缩放，所以：

1. X 的全局缩放 `S = ceil(最密那块屏的缩放)`。低于某块屏的缩放，那块屏上的 X11 窗口
   就是被放大糊的 —— 只能按最密的那块来。
2. 告诉 X11 窗口的尺寸和位置都是 `逻辑 × S`，它画的缓冲区就是 `逻辑 × S` 像素。
3. 场景里把这个缓冲区按 `S` 倍声明，于是 `缓冲区 / S = 逻辑尺寸`，落到屏上：
   - 最密的那块屏（`s = S`）：落屏像素 = `逻辑 × S` = 缓冲区像素 → **1:1，最清晰**
   - 低 DPI 的屏（`s < S`）：落屏像素 = `逻辑 × s` < 缓冲区 → **缩小，不糊**
   - 两块屏上的物理尺寸一样（逻辑尺寸一样，而 scale ∝ ppi）
4. 命中坐标按同一比例换回来：`局部 = (布局坐标 - 窗口原点) × S`，点哪儿就是哪儿。
257. **输入法抢键盘后，按住修饰键只认第一次：Mod+滚轮 / Mod+左键第二轮就失效**（用户报「mod 长按只能消费一次的 bug 回归了」「修饰键都有这个问题」）。

     - 根因：`input.c` 的指针绑定用 `wlr_seat_get_keyboard()` 拿 seat 上那把键盘，
       再读它的 xkb 修饰键。fcitx5 一启动就 `grab_keyboard`，seat 上换成它的虚拟
       键盘；物理键盘的修饰键只经 `wlr_seat_keyboard_notify_modifiers()` 发出去，
       虚拟键盘自己的 `xkb_state` 不会变 —— 读设备永远是「没按 Mod」，于是第一下
       滚轮之后就不认了。a3d4dc9 只补了「通知」，没改这里的取值。
     - 修：新增 `pointer_modifiers()`：优先找一把**物理键盘**（`virtual == NULL`
       且不是 `inject`）读它的 xkb 状态；纯 headless / 只有注入时退回 seat 键盘。
       按钮、滚轮、overview 里滚轮三处都走它。
     - Ctrl+Space 不触发是另一件事：配置写的是裸脚本路径
       `vidow.bind("Ctrl+space", "~/文档/…/fcitx5-input-switcher.sh")`，动作解析
       只认 `spawn <命令>`，裸路径被当成「不认识的动作」。已改成
       `spawn ~/文档/…/fcitx5-input-switcher.sh`，实测切换 `keyboard-us → rime`。
     - 判据：`test-input-inject` 54 / `test-drag` 10 / `test-keyboard-grab` 30 /
       `test-click-focus` 4 / `test-ws-dynamic` 9 全过。`test-mini` 的 S3
       「自由小窗拖出屏边」仍有 1 项失败（盒子右边缘停在 2550），与本条无关，
       另查。

256. **点另一块屏的空区域，键盘焦点不跟过去：在 4K 上乱点，动的却是左边窗口**（用户报「4k 那边随便点就能影响到左边」）。

     - 根因：pointer button 只更新 `server->focused_output`，`server->focused` 还停在
       上一块屏的窗口上。之后 `close-window` / `focus-column` 这些键位取的是
       `server->focused`，于是「在右屏点一下、按快捷键，关的是左屏那个窗口」。
     - 修：新增 `vidow_workspace_focus_output()`（`workspace.c`）：换
       `focused_output`，再把焦点交给那块屏活动工作区记住的窗口（没有就第一列 /
       第一个浮窗；再没有就清空）。`input.c` 的 button 路径在
       `view == NULL && surface == NULL` 时调它；点窗口 / layer 的路径不变。
     - 判据：`test-click-focus.sh` 4 项 —— 焦点先给左屏 w1，点右屏空区变 w2，
       点左屏空区变回 w1，日志无 assert。`test-focus` 60 / `test-workspace` 58 /
       `test-drag` 10 / `test-rules` 127 全过。

255. **规则里的 `%` 转义静默失效：Vivaldi 没落到 DP-2，窗口开在 DP-3 上、列却画进 DP-2**（用户报「DP-2 上的窗口操作全变成 DP-3 的」）。

     - 根因：2765aee 把规则匹配换成了 POSIX ERE，但配置沿用的是 Lua 风格
       `^org%.kde%.kate$`。ERE 里 `%` 不是转义符，`%.` 变成「字面 % + 任意字符」，
       整条规则**静默不命中**。用户 `output=DP-2` 的 Vivaldi 规则被吃掉，窗口按
       `view_default_output()` 落在当时的焦点屏 DP-3；DP-3 的列向右延伸，画进了
       DP-2 的坐标区，看起来像「窗口在 DP-2」，点击 / 焦点却按 DP-3 算。
     - 修：`rule_pattern_to_ere()` 编译前把 `%x`（x 非字母数字）翻成 `\x`，
       `%%` 翻成字面 `%`；`%d` 这类字母数字组合原样保留。`m->pat` 仍存原文，
       `vidowmsg rules` 读回来不变。
     - 顺带：`view_map()` 里新窗口的工作区改成按 `view->output`（创建时按光标选的）
       取 `vidow_workspace_shown_for()`，不再用可能停在上一块屏的全局焦点；
       `view_ensure_column()` 的兜底同理。
     - `test-rules.sh` 新增 `cmdline = "bluewin%.py"` 用例；127 项全过。
       `test-workspace` / `test-outputpos` / `test-mixdpi` / `test-drag-insert` 也全过。

254. **窗口拖拽 / 插入的视觉和 niri 对齐**（用户报「应该和 niri 同步」）。

     - 起拖：光标走够 niri 的 16px 阈值后，`view->drag_detached` 让 `view_shows()` 返回假，
       被拖的平铺窗口从布局摘走，后面的列并拢；落点预览按「剩下的这些列」算。只打标记、
       不动物理链表，松手插回去时原来的上下顺序还在。没走够就松手只是一次点击，布局不闪。
     - 被拖的平铺窗口降到 **75% 不透明度**（niri 的 `INTERACTIVE_MOVE_ALPHA`），松手恢复；
       拖动期间视口锁在当前值（`output->drag_scroll/drag_viewport`），摘列后条带变短也不会
       让焦点列把视口拽回去。
     - 松手插回 / 取消 / 变迷你：`drag_restore_view()` 清标记和透明度；插入函数看得到原来的
       列链表，`insert_pos_at()` 看的是并拢后的布局。落点动画从**拖到的位置**补间，不先跳回
       旧 geom 一帧。
     - `test-drag-insert.sh` 的期望跟着改成「摘走后的布局」：w1 插 w2 列、w2 从 w1 列取出来、
       落进 w1 列第 2 位；贴边自动滚的条带变短后视口仍能自己滚到头。30 项全过；
       `test-drag.sh` / `test-drag-follow.sh` / `test-drag-stall.sh` 照旧过。

253. **静止时窗口背后的模糊底衬「啪」一下换挡：采样 UV 认的是「正在渲染的那块屏」，不是贴图代表的那块屏**

     - 现象（用户）：`blur_all=true, blur_xray=false`，窗口背后**只有壁纸**、屏幕上什么都没动，
       底衬却每 1~5 秒整块瞬切一次（1 帧完成、无过渡），两态是「中心放大 + 变糊」的关系
       （x2~x3，均值差 1.4%、std 差 46%，不是 alpha 跳变）。
     - 关键观测（`/tmp/fr2/g522.png` vs `g523.png`）：两帧的差异 bbox = **恰好那一个窗口**
       （x[964,1913] y[46,1071]），而**同一张共享底衬的另一个窗口逐像素完全相同** ——
       贴图内容没变，变的是这一个窗口**怎么采样**它。std 11.44 → 6.15（−46%）也对得上
       「整屏底衬只取中间一块铺满整窗」：金字塔那张 up[0] 是半分辨率，被双线性拉大两倍。
     - 机制：`decor_draw` 的采样 UV 现场读 `server->output_w/h/x/y`（`src/fx.c:2795-2811`），
       而这对全局量是 **「正在渲染的那块屏」**（`src/output.c:47,73` 开头才写）。
       装饰重烘那趟（`src/output.c` 帧循环里 `blur_drawn != blur_ver` 那个循环）**不看归属**：
       别的屏正在渲染时它也会给这块屏上的窗口重烘一次 —— 那一刻 `server->output_w` 是
       另一块屏的（2 倍屏上就是 2 倍宽），于是整屏底衬被当成 2 倍视野采样。
       烘完 `blur_drawn = blur_ver`，这块屏自己那帧再渲染时已经「不脏」，错的那版就留到下一次
       代次前进 —— 这正是「两态来回跳、和屏上活动不同帧」。
     - 修法（`src/fx.c` + `src/server.h`）：底衬贴图代表的是**哪块屏**（尺寸 + 布局原点），
       在 `vidow_blur_update()` 里跟着贴图一起记到 `fx->blur_out_*`（只有那一趟知道归属），
       `decor_draw` 只认这一组，不再现场读 `server->output_*`。私有区域那条路（`blur_tex_w > 0`）
       照旧按区域尺寸算，layer 那一路不碰。
     - 回归：`test-blurscale.sh` 改前 12/12 PASS、改后 12/12 PASS（它只有一块屏，
       这条 bug 要两块不同分辨率的屏才露 —— **本次没跑出双屏对照数字，见「未验证」**）。

248. **过冲的缓动曲线把 vidow 打崩**（2026-10-13 20:39 core dump，noctalia 一打开就崩）

     - 现象：`vidow: types/scene/wlr_scene.c:1133: wlr_scene_buffer_set_opacity: Assertion
       'opacity >= 0 && opacity <= 1' failed.` → SIGABRT（coredump 5.3M）。
     - 栈：`#6 alpha_iter` ← `#8 vidow_fx_set_alpha` ← `#9 layer_apply_anim` ← `layer_anim_update`
       ← `vidow_anim_tick` ← `vidow_output_render`。
     - 根因：layer 的入场/消失动画把**缓动后的进度**当 alpha 用；`out_cubic` 永远不超 1 所以
       一直没事，配成会过冲的曲线（`out_back 0.18` 冲到 1.18，`out_elastic` 更夸张）就当场断言炸。
     - 修法：`src/fx.c` 加 `clamp01()`，在 `alpha_iter()`（唯一的底层调用点，栈里那一帧）、
       `vidow_fx_set_alpha()` 入口、关闭动画的 `closing->alpha` 三处夹到 [0,1]。透明度不该过冲。
     - 验证：`easing = "out_back 0.6"` + `layer_open_ms/window_open_ms = 900` 起 headless，
       开关窗跑一遍，无断言、无新 core dump（此前用户真机上同一条路 20:39 就炸了）。
       结构性论据：所有经过 `vidow_fx_set_alpha` 的值现在都被夹住，越界值到不了 wlroots。
     - 未覆盖：其余直接调 `wlr_scene_buffer_set_opacity` 的地方（fx.c 3124/3581/3699、
       view.c 1749/4662、capturemask.c）没逐个查是否也会吃到缓动进度。

5. **告诉 X 服务它该按设备像素报屏幕**：往 root window 写
   `_XWAYLAND_GLOBAL_OUTPUT_SCALE = S`（CARDINAL）。打过 HiDPI 补丁的 Xwayland
   （`xorg-xwayland-lily` / xorg 的 MR!733）收到之后才会把 X 屏幕和 RandR 模式也
   乘上 S —— 这正是上面第 2 条假设的「X 像素 = 逻辑 × S」。**不写它的后果**：X 屏幕
   只有逻辑尺寸，按屏幕尺寸算布局的客户端（GTK）会把整屏当成一半大小，最大化的窗口
   只占 1/4 面积、固定尺寸的登录窗也和面板对不上。

   回归判据里的「没打补丁」那条路用测试钩子 `VIDOW_TEST_XWAYLAND_UNPATCHED=1` 模拟
   （本机装的正好是补丁版，否则没法验）。

   **这台 Xwayland 认不认？vidow 量一次**：写完属性之后读 root window 的尺寸，跟
   「布局逻辑尺寸 × S」对账。为什么不是量一次就完事：Xwayland 处理属性**之前**屏幕
   也是逻辑尺寸（它就是那样报的），而它是在自己的事件循环里处理 —— 忙的时候要等一会
   儿。所以按 0.12 / 0.25 / 0.5 / 1 秒（共约 1.9 秒）反复量，量到「设备像素」立刻收工。
   对得上 = 补丁版，日志说「这台 Xwayland 认这个属性」；只有逻辑尺寸 = 不认 ——
   这时**自动退回 1 倍**：
   X 屏幕 = 逻辑尺寸、`Xft.dpi = 96`、`GDK_SCALE = 1`，几何全对（不会出现 1/4 的窗口），
   代价是 2 倍屏上 X 客户端糊一点。也就是说 **vidow 不要求你装哪个 Xwayland**：装了
   补丁版就有清晰的 X 客户端，没装也不会摆错。要强制 1 倍就写 `xwayland_scale = off`。
6. **Xft.dpi = 96 × S**（写进 `RESOURCE_MANAGER`，保留别人的资源项）。`GDK_SCALE` /
   `QT_SCALE_FACTOR` 只覆盖 GTK / Qt；Chromium/CEF（微信登录窗就是）、Electron、
   Java、Wine 只认 Xft.dpi。不给，它们按 1 倍画，而 vidow 按 S 除 —— 窗口只剩该有的
   1/4。回归判据见 `test-xwayland-hidpi.sh`（属性、Xft.dpi、X 屏幕 = 逻辑 × S，
   以及端到端：点一下 xev 收到的 root 坐标 = 布局坐标 × S）。

`GDK_SCALE = S` 发给 X11 客户端：认这个变量的 GTK 按 S 倍绘制（于是和 Wayland 程序
一样大）；不认的老程序（xterm、xeyes）保持 1:1 绘制 —— **物理尺寸在两块屏上依然一致、
依然清晰，但相对 Wayland 程序偏小**。这是 X11 的硬限制：要么放大（一样大但糊），要么
不放大（清晰但小）。想要后一种取舍就把 `xwayland_scale` 设成 `off`。

**`QT_SCALE_FACTOR` 绝不设**（这一条是用户报出来的）：它是唯一一个「X11 和 Wayland
都认」的缩放变量 —— Wayland 的 Qt 程序会在输出缩放之外**再乘它一遍**，于是只要有一块
2 倍屏（`S = ceil(最密那块)` = 2），**每块屏**上的 Qt 窗口就都大 2 倍（实测 kate 的
逻辑宽度 634 → 1090；GTK 不受影响：实测 GTK3/GTK4 在 Wayland 下不认 `GDK_SCALE`，声明
的还是 1 倍）。Qt 那边改走 X 自己的 DPI：`QT_AUTO_SCREEN_SCALE_FACTOR=1` + 上面那条
`Xft.dpi = 96×S`（写在 X 的 `RESOURCE_MANAGER` 里，Wayland 客户端根本看不到）。
代价说清楚：混 DPI（一块 1 倍一块 2 倍）时 X 屏幕的 DPI 是两块屏混出来的一个值，Qt 推
出来的倍数可能不是 S —— 这时 X11 的 Qt 程序按 1 倍画，在密屏上会偏小。要精确就单独给
这个程序设：`env QT_SCALE_FACTOR=2 kate`（只影响这一个进程）。

**逐窗口改**：`off` 是全体的取舍，可有些程序（微信登录窗、CEF 系那一类）自己不肯按
`S` 倍画，混在认这套的 GTK/Qt 程序里，全局怎么设都不对。窗口规则可以只给它们改缩放
桥 —— `x11_scale = 1` 就是「这个客户端没按 `S` 倍画，缓冲区别除」：

```lua
vidow.rule{ title = "微信", x11_scale = 1 }     -- 按标题（WM_NAME / 窗口标题）
vidow.rule{ class = "WeChat", x11_scale = 1 }   -- 按 WM_CLASS
```

效果：固定尺寸 300×300 的 X11 窗口在 2 倍屏上，默认（除 2）布局里只剩 150×150 ——
面积是该有的 1/4，看着就是「小得不对劲」；写了 `x11_scale = 1` 之后是 300×300，和同
尺寸的 Wayland 窗口一样大（内容是放大 2 倍，略糊，这是 X11 的代价）。命中坐标一直跟
着渲染走（`局部 = (布局 - 原点) × 实际倍数`），两种设置下都是点哪儿就哪儿。
`vidowmsg rules` 能把 `x11_scale` 查回来；回归判据见 `test-x11scale.sh`（同一个
300×300 的固定尺寸 X11 窗口跑两遍：150×150 vs 300×300）。

改过缩放的窗口还有一个坑（实测撞出来的）：场景的命中判定会问表面自己的输入区域，
而那个区域按「表面声明的逻辑尺寸」卡边界 —— 我们把它画成 2 倍大之后，右边和下边一大片
就成了「看得见、点不着」。所以尺寸和表面声明对不上的时候，vidow 把命中区域交给节点
自己的盒子（`point_accepts_input = NULL`，代价是这类窗口不再遵守 X 的 shape；默认
尺寸一致时照旧）。`test-x11scale.sh` 里两种设置都扫过命中区域。

### X11 的 SSD 和 Wayland 一样

X11 窗口走的是**同一条装饰管线**：同一个圆角/边框/阴影着色器、同一套模糊 backdrop、
同样的开关动画和 shader、同样的焦点边框色。判据是装饰缓冲的尺寸公式：
`(逻辑尺寸 + 2×内边距) × 输出缩放` —— X11 和 Wayland 各算各的，但公式和管线同一条。
`test-dpi.sh` 里对同一个 zenity 对话框分别量过：

| 谁 | 内容（逻辑） | 装饰缓冲（2 倍屏） |
| --- | --- | --- |
| X11（`GDK_BACKEND=x11`） | 300×230 | (300+64)×230+64)×2 = **728×588** |
| Wayland | 340×264 | (340+64)×(264+64)×2 = **808×656** |

（两个数字不同是 GTK 在两种后端下算出的自然尺寸差几十像素，不是装饰的差别；
两边都是「内容 + 32 逻辑像素内边距，再乘输出缩放」。）

### X11 的弹出物和 Wayland 的 xdg_popup 一样

判据是「它是不是一个弹出物」：override-redirect，或者 `_NET_WM_WINDOW_TYPE` 落在
菜单 / 下拉框 / 提示 / 通知那一组（`MENU` / `DROPDOWN_MENU` / `POPUP_MENU` /
`TOOLTIP` / `COMBO` / `DND` / `NOTIFICATION`）。它们在 Wayland 那边就是 `xdg_popup`，
而 vidow 对 xdg_popup 的做法是**不装饰、不跑 shader、不做动画、位置尺寸听客户端的、
不进布局、不抢焦点** —— X11 这一侧逐条对齐：

| 维度 | 普通窗口 | 弹出物（= xdg_popup） |
| --- | --- | --- |
| 装饰（边框/圆角/阴影/噪点） | 有 | 没有（连装饰离屏缓冲都不建） |
| 开关动画 / shader / 移动补间 | 有 | 没有（`window_*_ms` 调多少都一样） |
| 模糊 backdrop | 浮动窗口有 | 半透明时有（不透明的不垫：一个像素都透不出来） |
| 位置 | 布局算（平铺）/ 居中（浮动） | 客户端给的屏幕绝对坐标 ÷ 全局缩放 |
| 尺寸 | 布局给，或客户端声明 | 客户端说多少是多少（不夹工作区） |
| 层级 | `layer_toplevels` | 自己的 `layer_popups`（在所有托管窗口之上） |
| 焦点 | 抢（焦点边框跟着走） | 不抢：只把键盘交给它（菜单要收方向键、Esc） |

「不抢焦点」不是省事，是必须：焦点边框长在父窗口上，弹出物一冒出来就把焦点拿走的话，
父窗口的边框会灭、菜单关掉之后焦点还会被重新算到别的窗口上去。做法是把 seat 的键盘
焦点挪到弹出物表面上，但 `server->focused` 一动不动。

两条容易踩的坑，都写在代码注释里：

- **`_NET_WM_WINDOW_TYPE` 和 `set_hints` 不是一条消息**：wlroots 的 `set_hints` 是
  `WM_HINTS`，窗口类型有它自己的 `set_window_type` 信号。只听前者的话，「映射之后才把
  类型改成菜单」这种客户端永远改不了判（属性比 map 晚到是常态）。vidow 两条都听，
  改判时把场景节点在 `layer_popups` / `layer_toplevels` 之间搬、并让布局重判。
- **改判要连着装饰状态一起换**：装饰开着的时候客户端表面是被隐藏的（内容由装饰着色器
  画进离屏缓冲）。只把 `popup` 置位、不收掉装饰的话，那块按旧尺寸分配的离屏缓冲会继续
  按旧位置画边框和模糊 —— 看起来就是「弹出物糊了/花了」。


`_MOTIF_WM_HINTS`（客户端说「别装饰我」）**故意不听**：Wayland 没有 xdg-decoration
协议，客户端没法要求 vidow 不装饰它，X11 要一样；而这个属性还有个坑 —— XWM 是在窗口
刚建出来时读它的，客户端往往之后才设，听到的永远是初始值。与其按一个不可靠的信号做
两种行为，不如统一成一种。

### 实测

```bash
./test-dpi.sh       # 全 headless：1x + 2x 两块屏 + X11 + Wayland 客户端，20 项断言
./build/scaletest   # 31 项纯计算断言
```

`test-dpi.sh` 从日志里量出来的（每次 commit 记一行缓冲区 / 落屏像素）：

| 谁 | 在哪块屏 | 缓冲区 | 落屏设备像素 | 结论 |
| --- | --- | --- | --- | --- |
| X11（xterm） | 2 倍屏 | 628×696 | 628×696 | 1:1 |
| X11（xterm） | 1 倍屏 | 1268×1416 | 634×708 | 缩小 2 倍（不放大） |
| Wayland（GTK3 zenity） | 1 倍屏 | 340×264 | 340×264 | 1:1 |

`test-dpi.sh` 还会验：`GDK_SCALE=2` 真的进了子进程、`QT_SCALE_FACTOR` 一定**没有**
（`config.lua` 里
`vidow.exec("printenv GDK_SCALE")` 打出来，不是只看合成器自己的日志）、
X11 的 GTK 对话框不会被压成 1×1、两边各自的装饰缓冲尺寸都等于上面的公式。

同一个测试还断言：两块屏的逻辑尺寸 = 像素 / 缩放、X 全局缩放 = 2、
`GDK_SCALE=2` 已下发、指针命中坐标 = `(布局 - 原点) × 2`、日志里没有 assert / abort。

装饰缓冲逐像素也量过（`VIDOW_FX_DUMP=1` 把离屏缓冲 dump 成裸数据）：2 倍屏上
`border_width = 2` 的边框是 **4 个设备像素**、`corner_radius = 12` 的圆角是
**24 个设备像素** —— 都是逻辑值乘 2，证明装饰是按设备像素画的，不是画小再放大。

Xwayland 的行为也量过：`xrandr --query` 报 `HEADLESS-2 connected 640x360+0+0`，也就是
**RANDR 输出 = 模式 ÷ 输出缩放**，所以 X 屏幕和 vidow 的逻辑坐标是同一套；
Xwayland 也不会给窗口设 `wl_surface.set_buffer_scale`（日志里的命中倍数 = 2 说明
`buffer_scale` 一直是 1），所以「按 S 倍声明」这一下必须由合成器做。

### 顺手修掉的几个真 bug

1. **装饰画面把点击全吃掉**：它是场景里的一个普通 buffer 节点，而普通 buffer 默认
   「接受输入」，盖在客户端表面上面。于是**开着装饰的窗口点不着**（键盘还能用，所以
   一直没暴露）。修法是给它一个恒返回 false 的 `point_accepts_input`，让命中测试继续
   往下走到客户端表面。找到它的路径就是 X11 的指针坐标验证（光标移进去，日志里什么都
   没有）。
2. **X11 的浮动窗口被压成 1×1**（GTK 对话框在 X11 上根本看不见）：浮动窗口的语义是
   「尺寸客户端自己定」，xdg 那边回 `set_size(0,0)`；X11 没有这个约定，而
   `configure` 一定要带宽高 —— 拿客户端*当前*的尺寸回过去，等于命令它「保持 1×1」
   （它在 map 之前就是 1×1）。现在：尺寸退化时不发 configure，尺寸从客户端的
   `configure` 请求 / `WM_NORMAL_HINTS`（min == max）里取。还没完 —— 客户端的
   configure 请求常常**早于**「我们认出它是个对话框」（窗口类型/尺寸提示是之后才到的
   属性），所以请求要记在 view 上备用，并且收到 `set_size_hints` 时重判模式。
   修完：`float 信息: 300x230 at 490,245`，装饰缓冲 364×294 = 内容 + 2×32。
3. **自己踩的 `wl_container_of` 坑**：`set_size_hints` 我复用了 `set_hints` 的回调，
   而它是按 listener 字段的地址反推 view 的 —— 算出错的 view 指针，直接段错误
   （`vidow_float_set_mode` 里那句 `wlr_log` 拿着野指针的 title）。两个信号各给一个
   回调就好了。
4. **销毁窗口时撤销动画踩空场景节点**（段错误）：X11 的销毁路径先摘表面
   （`scene_tree` 置 NULL）再撤动画，而撤销动画会回调 `view_open_done()` →
   `vidow_view_apply_anim()` → `wlr_scene_node_set_position(&view->scene_tree->node, …)`
   → 空指针。`vidow_view_apply_anim()` 里加一句「没有场景节点就没什么可摆的」即可。
   暴露它的是新测试把开窗动画拉长到 2500ms：**销毁一个正在跑入场动画的窗口**必崩，
   默认 180ms 的窗口几乎撞不上，所以一直没被发现（`coredumpctl info` 给出的栈就是
   `x11_destroy_view → vidow_anim_cancel → view_open_done → vidow_view_apply_anim`）。
5. **换焦点时把 NULL 当窗口**（段错误）：`vidow_focus_view()` 会把上一个焦点表面
   「取消激活」，但那个表面可能已经不属于任何窗口了 —— 正在销毁的窗口就是先从
   `server->views` 里摘掉、之后才轮到换焦点的。`vidow_view_from_surface()` 返回 NULL
   时直接往下传，`vidow_view_set_activated(NULL, …)` 就崩了。现在 NULL 就跳过
   （顺带覆盖了 layer / 输入法这类本来就不是窗口的表面）。

## 显示器热插拔：记住工作区，插回来还原

工作区也按同一套规矩认归属（`workspace->home` = 属于哪块屏，`output` = 现在住哪块屏）：
屏拔了整块工作区跟着列借住到别的屏上，屏回来按 `home` 整块认领回去 —— 列、列宽、列序、
滚动位置全在工作区里跟着走，不用重建。

拔掉一块屏，它上面的东西不会丢也不会漂到屏幕外：**归属**留在记忆里，**住处**先挪到别的
屏上；同一块屏插回来，按记忆把工作区放回去 —— 列的顺序、每列的宽度、滚动位置、
焦点都留着（niri 的做法）。

```
            home（归属）                 output（现在住在哪）
列 / 平铺窗口  ── 记忆里那块屏 ──┐
浮动窗口      ── 自己的记忆 ─────┼─→  平时就是同一块屏
layer surface ── 记忆里那块屏 ──┘     屏拔了先借别的屏住，屏回来再搬回去
```

| 事件 | 做了什么 |
| --- | --- |
| 屏拔了 | 记下它的身份、布局位置、滚动位置、焦点；列 / 浮动窗口 / layer surface 的 `output` 换成一块还活着的屏（优先左边最近的那块），归属不动 |
| 屏插回来 | 按归属认领：列整组搬回去（顺序不变）、浮动窗口摆回「相对这块屏原点」记着的位置、恢复滚动位置和焦点 |
| 一块屏都不剩 | 东西先悬着（`output = NULL`），等有屏了再安家 —— 期间不会有任何指向已释放 `wlr_output` 的指针 |

身份用的是**连接器名**（`DP-1` / `HDMI-A-1` / `HEADLESS-2`）：同一根线拔了再插回来名字
不变；换个显示器插同一个口名字也一样，那正是「还原工作区」该发生的时候。没用 serial，
因为 docking 站/MST 换名字的情况更常见（这条取舍写在 `src/hotplug.h` 里）。

### 三个坑（都是实测撞出来的）

1. **布局会自己重摆**：拔掉左边那块屏之后，wlroots 把右边那块从 `x=1280` 挪到了 `x=0`
   （位置变了会打一行 `output HEADLESS-1: 布局位置 1280,0 1280x720 -> 0,0 1280x720`）。
   于是缓存的输出位置（我们只在渲染时更新）会差一整块屏：浮动窗口按「平移量」算就会被
   搬到屏幕外。现在一律**现问布局**要坐标，而且浮动窗口记的是「相对自己那块屏原点的
   偏移」（`view->home_dx/dy`），插回来按新原点重算 —— 不依赖任何差值。
2. **借出去的贴图必须收回**：不重叠的窗口共用输出那份模糊 backdrop（借来的指针）。
   屏拔了要销毁它，而热插拔紧接着就会重摆 + 重画 —— 手里还攥着已销毁的贴图，GLES2 那边
   直接 `wlr_texture_is_gles2` 断言崩溃。`vidow_blur_output_finish()` 现在会让所有借过它
   的窗口松手（下一帧 `blur_update()` 会重新借）。
3. **layer surface 的野指针**：`ls->output` 指着 `server->outputs` 里的结构，输出一销毁
   就是野指针（`vidow_arrange_layers` 里那句 `ls->output->wlr_output->scale` 会踩）。
   现在 layer surface 也有归属，屏拔了跟着挪到别的屏上（状态栏会出现在剩下的屏上）。

### 测试

```bash
./build/hotplugtest   # 32 项离线断言：输出身份 / 记忆台账 / 挑接收的屏 / 稳定重排
./test-hotplug.sh     # 24 项：真拔一块 headless 输出，再以同一个名字插回来
```

`test-hotplug.sh` 全 headless：3 列 + 1 个浮动窗口 + 1 个 layer surface 放在左边那块屏上，
量到的判据是「拔线时数清 3 列 / 3 个窗口 / 1 个浮动窗口并挪到右边那块屏」、
「还原的数目和拔线时一致」、「列的先后顺序和间距没变」、「列宽（Mod+R 改成 2/3）还在」、
「每个窗口整体平移的量 = 新原点 - 旧原点（滚动位置和列偏移都还原了）」、
「窗口四象限颜色按日志坐标采样正确」、「画面尺寸随屏的出现/消失变化」、
「接收的那块屏底下是壁纸（layer surface 真的跟着挪了，否则是黑的）」。

拔插由 `VIDOW_TEST_OUTPUTS` 这个**测试钩子**触发（不是产品功能）：

```bash
# 9 秒时拔掉左边那块屏，18 秒时以同一个名字插回来
VIDOW_TEST_OUTPUTS="9000:remove:left,18000:add:1280x720:last" WLR_BACKENDS=headless \
  WLR_HEADLESS_OUTPUTS=2 ./build/vidow
```

它调用的是 `wlr_output_destroy()` 和 `wlr_headless_add_output()`，走 backend 的真实事件
路径，不是模拟状态。名字可以写 `left` / `right`（按布局位置挑）或具体输出名；`add` 那步
写 `last` 就用「上次拔掉那块屏」的名字 —— headless 后端的输出名是自增的
（`HEADLESS-1/2/3…`），不这样就没法在同一进程里验「同一根线又插上了」。别的后端加不出
输出（只有 headless 能凭空造），会拒绝并打一行日志。

## niri 风格的 overview

`Mod+O` 把**当前这块屏、现在显示着的那块工作区**的整条列带缩小铺满屏幕看一眼全景，
点谁去谁，`Esc`（或再按 `Mod+O`）缩回去。不重排、不改客户端尺寸 —— 布局一点没动，只是换了个「世界 -> 屏幕」的
变换：

```
screen = 屏中心 + (world - cam) * scale

t = 0（正常）   scale = 1、cam = 屏中心      → 正好是恒等变换，进出都不会跳
t = 1（展开）   scale = 刚好装下整条列带      → cam = 列带中心（+ 滚轮平移）
```

场景是全局的，所以这个变换是**算一次、所有窗口共用**（`view.c` 的
`vidow_view_apply_anim` 每帧套一次）。t 是动画进度，进/出都走同一套缓动。

| 做什么 | 怎么做 |
| --- | --- |
| 缩放 | 窗口内容的场景尺寸乘 `t` 对应的缩放（`base_dst_w/h` 是 1:1 基线，在 commit 时记一次）；装饰的离屏缓冲按 `scale × 缩放` 渲染、再按缩放显示 —— 屏幕上仍是 1:1 设备像素，不糊也不浪费 |
| 相机 | 跟着焦点列走（`Mod+H/L` 换列 = 平移视图）；滚轮给一个用户平移量，换列时清零 |
| 只看一块屏 | 进 overview 时锁定当时聚焦的那块屏；别的屏上的窗口淡出（`alpha *= 1 - t`），弹出物 / 通知 / 输入法候选框这些屏幕空间的东西收起来 |
| 背景 | 一块遮罩把壁纸压暗（`layer_overview_dim`：夹在壁纸之上、窗口之下），缩小的窗口浮在上面。颜色和浓度可配：`overview_dim_color = "#05050899"`（RRGGBBAA，alpha = 压多暗，`#00000000` = 不压暗） |
| 模糊 | 窗口自己的背景模糊在全景里不采样（那套 UV 按窗口自己的位置算，缩小之后对不上）；壁纸另走一条 —— `overview_blur`（默认开）把**共享底衬**（壁纸 + background / bottom 层跑完 kawase 那张）摊满整块屏，垫在遮罩下面（`fx.c` 的 `vidow_overview_blur_frame`） |

### 进出动画的方向（`t` 永远 0 → 1）

动画引擎（`anim.c`）不认「反向」：`update(data, t, …)` 里的 `t` 永远是从 0 走到 1，
方向得由调用方决定。全景的进度 `ov->t` 因此是**插值出来的**：

```c
ov->t = ov->on ? from + (1.0 - from) * t     /* 进：from → 1 */
                : from * (1.0 - t);          /* 退：from → 0 */
```

`from`（`ov->anim_from`）是这次动画开始时 `ov->t` 的值 —— 由 `overview_start()` 在
`vidow_anim_cancel()` **之前**记下来（cancel 会回调 `overview_done()`，把 `t` 拍成端点值）。
于是：正常进出从干净的两端起，中途反向（进到一半再按一次）从当前进度接着走，都不跳。

以前这里是 `server->overview.t = t`（把引擎的 `t` 直接当进度）：**退出 = 把「进入」重放
一遍**，最后 `overview_done()` 把它拍回 0 —— 先弹回原样、再缩下去、再啪一下弹回去，
就是用户报的「点选之后闪弹一下」（点选走同一条退出路径，而且这时焦点已经换了列、镜头
正在滚，看着更明显）。

### 背景（壁纸）归谁画

**vidow 不画壁纸**：壁纸永远是 layer-shell `background` 层的客户端画的 ——
`swaybg -i 图.png`、`swww`（`swww-daemon` + `swww img`）、quickshell 的一个
`WlrLayershell.layer = Background` 的窗口，或者 `tools/wall.py` 这种几十行的自制品，
谁挂上都一样。vidow 这边保证的是：

| 保证 | 说明 |
| --- | --- |
| 叠放 | `layer_background` 在最底下那几棵树里（`main.c` 按创建顺序叠），窗口、面板、overview 遮罩都在它上面；它下面只有一层 `background_color` 的兜底纯色 |
| 铺满 | 四个边都 anchor 的 layer surface 按输出的完整布局框摆（`vidow_arrange_layers`），跟随输出热插拔 / 改分辨率 |
| 参与模糊 | 窗口的背景模糊把「壁纸 + background / bottom 层」一起当底衬抓（`fx.c` 的共享底衬），所以 `blur_all` / `blur_xray` 糊的就是外部画的壁纸；`background` 层自己跳过（底衬里已经有它，垫一层等于画重） |
| 不做入场动画 | 需要的话给它一条规则：`vidow.rule{ layer = "^background$", anim = false }` |
| overview 里被压暗 / 糊掉 | 遮罩在壁纸之上（`layer_overview_dim`），所以全景里外部壁纸同样被压暗（默认压到 43%）、`overview_blur` 开着时还会被糊掉 |
| 全景里换一张 | 起第二个壁纸客户端（各占一个 layer namespace），给它一条 `vidow.rule{ namespace = …, overview = "only" }` —— 它只在全景里露面。见下 |
| 谁在画 / 变了通知 | `vidowmsg background` 报当前状态（见下），`background-changed` 事件在壁纸上/下屏时发一次 |

**兜底色 `background_color`**（`configs/18-background.lua`，默认 `#14161cff`）：谁都没画
壁纸时垫在最底下的纯色，有壁纸就被完全盖住。所以「没有壁纸客户端」不是一块死黑。

**给 shell / 脚本用的查询**（`vidowmsg background`，别名 `wallpaper`）：

```json
{
  "color": "#14161cff",
  "clients": 1,
  "visible": 1,
  "surfaces": [ { "id": 3, "namespace": "wall", "layer": "background",
                  "output": "HEADLESS-1", "geometry": { "x":0,"y":0,"width":1280,"height":720 },
                  "mapped": true, "overview": "off", "shown": true, … } ],
  "outputs": [ { "name": "HEADLESS-1", "covered": true, "covering": 1 } ]
}
```

- `clients` = background 层的 surface 个数；`visible` = 其中**现在真的画着**的几个（规则
  `overview = "only"` 的那种在全景没开的时候不算 —— 见下面「全景里换一张壁纸」）。
  `surfaces[]` 就是它们的完整 layer 对象，每块多两个字段：`overview`（规则值
  `off` / `only` / `hide`）和 `shown`（现在画没画）。
- `outputs[].covered` = 这块屏的壁纸有人画了（有个 mapped 的 background surface 铺满它）。
  常驻的 shell（quickshell 那种）靠它决定「这屏要不要我上」——只看画着的那几块，所以
  「全景专用」那块不会把 shell 骗过去。
- **事件**：`background-changed`（IPC `watch` 和 `vidow.on()` 都收得到），载荷就是上面
  这一份状态。上屏、下屏、客户端退出各发一次 —— 别去轮询。

```lua
-- 有别的壁纸客户端时就不画，没有就自己上一张：shell 侧大概这么用。
-- 钩子回调拿到 (表, 原始 JSON)：表里有 event / time_ms / data，data 就是上面那份。
vidow.on("background-changed", function(ev)
    for _, o in ipairs(ev.data.outputs) do
        if not o.covered then
            vidow.exec("swaybg -o " .. o.name .. " -i ~/图片/wall.png")
        end
    end
end)
```

**想让 swww / swaybg / Noctalia 来管**：直接起就行，vidow 这边不用配 ——
`vidow.exec_once{ "swww-daemon" }`，然后 `swww img ~/图片/wall.png`。它们走的就是
上面那条 background 层契约（跟 `tools/wall.py` 一模一样）。Noctalia 的壁纸也一样：
它在 `noctalia-wallpaper` 这个 namespace 上画一块 background 层的 surface（每块屏一块、
铺满整屏），vidow 认的就是这一层。

**全景里换一张壁纸**（niri 的 `place-within-backdrop` 那件事）：vidow 的壁纸本来就不跟
全景缩放（缩的只有窗口），所以缺的只是一个「什么时候露面」的开关 —— 图层规则 `overview`。
起**两个**壁纸客户端（各占一个 layer namespace），让其中一块只在全景里露面：

```bash
awww-daemon            &    # 桌面那张：namespace awww-daemon
awww-daemon -n ov      &    # 全景那张：namespace awww-daemonov（-n 的值直接拼在名字后面）
awww img                  ~/图片/桌面.png    # 不写 --namespace = 默认那个
awww img --namespace ov   ~/图片/全景.png
```

```lua
-- configs/14-keys.lua（或者你自己的 config.lua）
vidow.rule{ namespace = "^awww-daemonov", overview = "only" }
```

- `overview = "only"`：这块 surface **只在全景里**画。桌面上它的场景节点是关的 —— 既不挡
  桌面那张壁纸，**也不进窗口的模糊底衬**（共享底衬抓的就是这几棵树）。
- `overview = "never"`：反过来，进全景的时候不出现（比如全景里只想要兜底色）。
- `overview = "always"`（默认，也可以不写）：照常，跟着进全景，被遮罩压暗 /
  `overview_blur` 糊掉。
- 名字读作「全景里：总是 / 只有它 / 从不」。`always` / `only` 对 top / overlay 层没用 ——
  那两棵在全景里是整棵关掉的，逐 surface 点亮也开不回来。
- 规则热改**当场生效**（`vidowmsg reload-config` / `SIGHUP`），不用等下次开关全景。
- 全景里看到的是**共享底衬那一份**：`overview_blur` 开着时是糊过的 + 遮罩压暗到 43%。
  想让全景那张也清清楚楚，把 `overview_blur` 关掉（`configs/17-overview.lua`）。
- `awww` 的 namespace 参数**值别以 `-` 开头**：`awww img` 是 clap，`--namespace -ov` 会被
  当成短选项（报 `a value is required for '--namespace'`），而 `--namespace=-ov` 它认、
  `awww-daemon` 那个手写的解析器反而不认 —— 用 `ov` 这种名字两边都顺。
- `swaybg` 没有 namespace 参数，用不了这招（`awww` / `swww` 有 `-n`）；也不一定非得用
  awww —— 任何能在 background 层、能指定 layer namespace 的客户端都行（`tools/wall.py`
  就是几十行的自制品，`python3 tools/wall.py "#ff00ff" wall2` 换个颜色 + namespace）。

### 两个坑

1. **遮罩层建了两次**：`main.c` 里建好了层（按叠放顺序），`vidow_overview_init` 又
   `wlr_scene_tree_create` 了一次 —— 新层挂在末尾，遮罩跑到窗口**上面**去了，
   overview 里连窗口都被一起压暗。现在只建遮罩、不建层（层由 main.c 按顺序建）。
2. **别动 `fx->blur`**：overview 里想临时关掉模糊，最直觉的写法是 `fx->blur = false` ——
   但这个标志是 `vidow_fx_begin()` 按配置设一次的，改了就再也回不来：**退出 overview
   之后所有窗口的装饰底衬都不模糊了**（像素级验证时才抓到：退出前后差了 24000 个像素，
   全在窗口装饰的内边距上）。正确做法是在 `view_wants_blur()` 里判断「这一刻不给贴图」。

### 测试

```bash
./test-overview.sh   # 63 项：日志 + 像素
./test-background.sh # 19 项：桌面背景（兜底色 / 查询 / 事件）
```

判据：`overview: 进入（… N 个窗口，占 WxH 缩到 S，铺在工作区 …）` 加每个窗口一行
「窗口 X -> 屏幕 x,y wxh（缩放 S）」；overview 里的尺寸 = 原尺寸 × S（±2px）、列间距同样
按 S 缩、顺序不变、所有窗口都在屏内；按日志坐标采样窗口四象限颜色（红绿蓝白）正确且
**没有被压暗**；壁纸被压暗了；`Esc` 退出后画面回到原样（只允许取整带来的零星像素差）；
**退出时 `t` 单调从 1 降到 0、有中间态**（不反弹）；**遮罩配色**（默认压暗到 43% /
`#00000000` 不压暗 / `#ff000099` 染红，三种配色下窗口里都还有纯红 `255,0,0`）；
**壁纸模糊**（跨边缘最大跳变 97 → 6，窗口本身不糊）；
点某个窗口 → `overview: 选中 X` + 退出 + 那一列被滚进视野；两块屏时只缩当前那块、另一块
屏上的窗口淡出。

## niri 风格的 Alt+Tab：最近窗口切换器（`src/mru.c`）

`Alt+Tab` **按住** → 浮出一层面板，列出窗口（缩略图是**活的窗口内容**，不是截图），
连点 `Tab` 在清单上
往前走，**松开修饰键**才把焦点给选中的那个；`Esc` 取消（焦点原地不动）、`Enter` 立刻确认。
`Alt+\`` 只看当前这个应用的窗口（`next-window same-app`）。

两个阶段是分开的：

| 阶段 | 状态 | 画面 |
| --- | --- | --- |
| 按住修饰键、还没到点 | `active = true`、`showing = false` | **一点都不动** —— 快速点一下 Alt+Tab 只是想切窗口，不该整屏闪一下 |
| 过了 `mru_open_delay_ms` | `showing = true` | 上面浮出一层：背景压暗 + 一排卡片（卡片里是各窗口的缩略图），选中那张有强调色描边 |

### 面板开着时输入是模态的

**从按下 `Alt` 那一刻算起**（`active = true`，不等面板显形 —— `open_delay` 那段空窗期也
一样），键盘 / 鼠标 / 手势全部归切换器，一个事件都不透给窗口和应用：

- **能用的只有切换器自己的键**：`Esc` 取消、`Enter`（或小键盘 `Enter`）提交、`Shift+Tab` 往回走。
  别的绑定一律不跑（命中但不放行会在日志里留一条
  `mru: 绑定 <触发>（<动作>）切换器开着期间不生效`）—— 不然面板摆在那儿的时候按
  `Mod+J` 会换工作区、按 `Mod+T` 会起终端，画面和手上的按键对不上。放行的白名单只有一个：
  动作名是 `next-window` / `previous-window` 的那两条（`keybind.c` 的 `bind_mru_allowed`）。
- **鼠标**：移动只点亮底下的卡片、左键点卡片直接切过去、点空白什么都不做；按下 / 松开成对
  吃掉（不漏给客户端 —— 客户端收到一个没有按下配对的松开会一直以为键还按着）。
- **滚轮**（真设备和 `vidowmsg pointer-scroll` 同一条路）：**选中项往下 / 往上一格**，
  往下滚 = 往后一项（和连点 `Tab` 同一个方向），横向滚吃掉但不动选中项。**到边停住、不循环**
  —— 这一条**故意**和 `Alt+Tab` 的「到头绕回」不一样：绕回是「连点一路往前走」的语义，滚轮
  是「来回微调」的语义，到头再滚一下跳到另一头会让人找不着自己在哪。**别当 bug 改回去。**
  这也是修掉的那个真 bug：以前按住 `Alt` 滚滚轮会命中默认配置里
  `Alt+wheel-up/down` = `workspace up/down`（`configs/14-keys.lua`），**面板开着就把工作区
  换了**、清单还不重建。
- **方向键**：`Left` / `Up` = 前一项，`Right` / `Down` = 后一项 —— 和滚轮**同一套 clamp 步进**
  （到边停住，不循环；日志 `mru: 方向键 → #…`、到边 `mru: 方向键到头（#…，不动）`）。
- **手势**（`src/gesture.c`）：滑 / 捏一样不参与（它动的是窗口和视口）。

实现在三处：`input.c` 的指针移动 / 按下 / 滚轮三个入口各自第一行问一句
（切换器活着就归它）、`keybind.c` 的 `keybind_mru_key`（键位白名单）、`gesture.c` 的每个手势
入口。滚轮那条闸**排在小窗档位之前**：不然光标压在迷你 / 图标小窗上时先被「档位状态机」抢走。

### 台账（谁「最近用过」）

**不维护链表**：每个窗口一个时间戳（`vidow_view.mru_ts`），要列的时候现按它排一遍 ——
窗口开 / 关 / 换工作区 / 销毁都不用同步第二份数据，也就没有悬空指针。

**debounce（`mru_debounce_ms`，默认 750ms）**：拿到焦点不等于「用过」。键盘一路导航
（`Mod+H/L` 连按）和 focus-follows-mouse 中途扫过的窗口都只是**路过** —— 不 debounce 的话
`Alt+Tab` 回的是半路上那个，而不是你真正待过的那个。所以焦点一到先记成 `pending`，过了
debounce 才落到时间戳上；往那个窗口打字算明确使用，debounce 立刻结束（日志里能看到
`mru: X 只是路过` / `mru: 提交`）。

### 两个顺序：摆的是屏幕顺序，一进来选的是「最近用过」

- **卡片怎么摆 = 屏幕顺序**：先按屏（左→右）、再按工作区序号，最后按窗口在屏上的位置
  （左→右、一列里上→下）—— 面板摆出来就是你眼睛看到的那一排。连点 `Tab` 也顺着这个顺序
  走（**到头绕回第一格**），所以「面板里往右一格」就是「屏幕上往右一扇」。滚轮和方向键走的
  是同一个顺序，但**到边停住**（见上面「面板开着时输入是模态的」那节 —— 那是故意的）。
- **一进来选中哪一扇 = 最近用过**：清单里第一个不是当前焦点的（`Alt+Shift+Tab` 反过来，
  从最旧的那头起）。所以 `Alt+Tab` 一下还是回到刚才那扇，接着按才顺着屏幕走。

两个顺序是分开的：清单顺序决定排版和 `Tab` 的步进，选中项由使用时间定（`mru_ts`）。
台账那份「最近用过」的顺序仍然照旧（IPC 的 `recent` 就是它）。

**跨工作区**：默认 `mru_scope = "workspace"` —— 只列**当前工作区**的窗口（切换器是「这块
工作区里切窗口」）。想跨工作区就 `mru = { scope = "all" }`（所有屏所有工作区）或
`scope = "output"`（当前这块屏）；也可以只给某一个绑定换范围：
`vidow.bind("Alt+grave", "next-window scope=all")`。

`scope = "all"` 的时候清单按**工作区序号**分组，同一个工作区里再从左到右 —— 当前工作区的窗口
不一定排在最前面（序号比它小的那些排它前面）。别的（藏着的）工作区的窗口照样列，缩略图也是
活的（客户端还在画，只是被停到屏外）。

### 面板是浮层，窗口一个都不动

跟 overview 是**两种做法**：overview 真的把窗口缩小摆一遍（改场景节点的位置和缩放），切换器
**不碰窗口** —— 它自己有一层压在窗口之上（`mru.tree`，建在场景树最后 = 最上面），里面是
压暗背景 + 卡片 + 缩略图。窗口该在哪还在哪、该多大还多大，原来那块屏照旧在底下（被压暗
一层）。这也正是 niri 的窗口切换器那套：面板是新内容，不动原来的排版。

卡片里那张预览走**装饰管线**：`vidow_fx_frame()` 把窗口连装饰一起画进一张离屏图（缩到预览
那么大），再摆到卡片里 —— 和屏幕上那扇窗口是同一条管线、同一份规则，所以**预览的形状就是
窗口的形状**（圆角、边框、阴影、模糊，连「焦点窗口边框亮一档」都一样）。窗口的圆角/边框
是窗口自己的规则给的，这儿不另算一份，改规则两边一起变。

两条兜底（画不出装饰时）：

- **直接摆客户端那份 buffer**（`view->scene_surface->buffer->buffer`）：场景拿它的纹理画，
  等于「把窗口内容缩小一份」，不抓图、不多一趟渲染 —— 样子是方角的，但至少有内容。装饰
  关着（`decor_enabled = false`）/ 客户端还没上屏时走这条。
- 这条路只取「可见窗口」那一块：自己画装饰的客户端（GTK）会把表面做大一圈画阴影，那圈
  留白不要（用 `vidow_view_fx_geo()` 的归一化坐标裁，和装饰着色器同一套）；**别抄场景节点
  自己的 `src_box`** —— 那里面还含着「窗口被裁到屏里」那块（列排到屏外的窗口只剩一条），
  预览要的是整扇窗口。宽高比按 buffer 自己算，塞不下就等比缩到框里居中，不拉伸。

滚出工作区的那些项不画（场景是全局坐标，不画的话它会落到隔壁那块屏上）。

| 配置键 | 默认 | 意思 |
| --- | --- | --- |
| `mru_enabled` | `true` | 总开关：关掉就不记账，那两个动作也不认 |
| `mru_debounce_ms` | `750` | 焦点要「待够」多久才算用过（0 = 立刻） |
| `mru_open_delay_ms` | `150` | 按住多久面板才显形（0 = 立刻） |
| `mru_scope` | `"workspace"` | 列哪些窗口：`workspace`（当前工作区）/ `all`（所有屏所有工作区）/ `output`（当前这块屏） |
| `mru_layout` | `"strip"` | `strip` = 一行，装不下横向滚；`grid` = 不滚，换行铺在屏幕里 |
| `mru_per_row` | `5` | `grid` 一行最多几个 |
| `mru_gap` / `mru_row_gap` | `16` / `16` | 预览之间 / 行与行之间 |
| `mru_max_scale` | `0.5` | 预览最多缩到多少（1 = 原尺寸） |
| `mru_max_height` | `480` | 预览高度上限（大屏上别让一个窗口占满屏） |
| `mru_min_scale` | `0.05` | 再挤也不能比它更小 |
| `mru_backdrop_alpha` | `0.6` | 后面那层压暗的浓度（桌面还看得见） |
| `mru_card_padding` | `8` | 每项背后那张卡片比预览大多少（逻辑像素）；比半径小时按半径算 |
| `mru_card_radius` | `12` | 卡片的圆角半径（超椭圆，和 portal 的选择器同一种圆角） |
| `mru_card_color` | `#2f3336e6` | 卡片底色 |
| `mru_card_border` / `_color` | `1` / `#3b4045ff` | 卡片描边宽度与颜色 |
| `mru_hover_border` / `_color` | `2` / `#3daee980` | 鼠标底下那张的描边宽度与颜色（默认 = 强调色半透明） |
| `mru_highlight_border` / `_color` | `2` / `#3daee9ff` | 选中那张卡片的描边宽度与强调色 |

### 卡片（样式照 portal 的 item）

每一项背后一张卡片：超椭圆圆角、一层淡底、一圈细描边 —— 三种状态，和 portal 的选择器
（`tools/vidowportal.c` 的 `card_draw`）一一对应：

| 状态 | 底 | 描边 |
| --- | --- | --- |
| 选中（键盘在走 / 鼠标点中） | 卡片底混 12% 强调色 | 强调色，`mru_highlight_border` px |
| 鼠标底下 | 卡片底 | `mru_hover_color`（默认 = 强调色半透明），`mru_hover_border` px |
| 其余 | 卡片底 | `mru_card_border_color`，`mru_card_border` px |

鼠标底下那张和选中那张**同宽**，只是颜色淡（默认 50% 强调色）—— 「点亮了、还没选中」这个
区别靠浓度说，不靠粗细说；想换颜色 / 粗细就写 `mru_hover_color` / `mru_hover_border`。

圆角是自己画的：`wlr_scene_rect` 画不出圆角，wlroots 0.20 也没有圆角 API，所以四个角各挂一张
**按覆盖率算出来的小图**（4×4 超采样抗锯齿），中间三条直边用普通矩形铺 —— 九宫格那套。四个角
共用同一张图，靠 transform 翻过去用。像素走自实现的最小 `wlr_buffer`（`begin_data_ptr_access`），
GLES2 / pixman 两条渲染器都吃，不用 GL 上下文。

两个坑（都踩过）：

- **内边距至少给到半径**：预览是个方块，圆弧伸进预览那块区域就会被窗口盖住 —— 看着就是
  「缺角」。所以实际内边距 = `max(card_padding, card_radius)`，半径 r 时圆弧正好贴着预览的角。
- **别拿 `node->buffer` 判「有没有图」**：wlroots 上传完纹理就把 buffer unlock 掉，锁归零 →
  发 `events.release` → 场景把 `scene_buffer->buffer` 清成 NULL（**纹理还在，照样能画**）。
  拿它当判据的话，面板一开要刷好几次，第二次刷新就把角节点关掉、圆角就没了。判据得是卡片
  自己记的 `fill_buf` / `ring_buf`。

### 鼠标

面板显形期间指针归切换器管：**移动 = 点亮底下那一张**（只改样式，**不改选中** —— 选中的那个
还是键盘在走），**左键点一下 = 直接切过去**（不必等松修饰键），点空白什么都不做。命中按卡片算
（比预览大一圈，圆角外面那点内边距也算点得中）。点选那次的按下被吃掉了，对应的松手也一起吃掉。

两种排版（`mru = { layout = … }`）：

- **strip（默认）**：一行，装不下就横向滚 —— 滚到「选中的那个完整可见」为止。
- **grid**：按 `mru_per_row` 换行、不滚动；行数多了**整体缩小**预览，缩到铺进工作区为止
  （下限 `mru_min_scale`）。窗口特别多时缩得比 `mru_max_scale` 还小是正常的 —— 这正是
  「一行限制数量 + 窗口多时自适应变小」那件事。

### 测试

```bash
./test-mru.sh   # 60 项：台账顺序 / 按住不换焦点 / 连点按屏幕顺序走 / 松开提交 / Esc 取消 /
                #        快速点一下不显形 / strip 滚动 / grid 换行与自适应缩小 / 像素 /
                #        卡片几何与描边颜色与圆角（选中 = 强调色、其余 = 卡片描边色）/
                #        鼠标悬停点亮（选中项不动）+ 悬停描边比选中淡 /
                #        鼠标点击当场切窗口 /
                #        浮层：窗口一个都没动 + 卡片里是全亮的窗口内容 + 预览也是圆角 +
                #              桌面那层被压暗
```

## IPC：状态输出与远程操作

一句话：**一行一个 JSON 对象**，问一句答一句；想持续收事件就订阅。客户端是
`tools/vidowmsg.c`（`build/vidowmsg`），但协议就是纯文本，`socat` / Python / 状态栏
脚本都能直接说。

```bash
vidowmsg state                  # 全量状态（窗口 / 工作区 / 输出 / layer / 配置 / …）
vidowmsg windows                # 只要窗口列表（带坐标、尺寸、列、规则）
vidowmsg outputs                # 输出：模式、缩放、方向、可用区、活动工作区、上面有哪些窗口
vidowmsg layers                 # layer surface：namespace、层、锚点、独占区、键盘交互
vidowmsg focused                # 现在谁拿着键盘、在哪块屏、哪块工作区
vidowmsg pick-window 640 360    # 这个坐标上是哪个窗口（命中测试，带表面局部坐标）
vidowmsg focus-workspace 2      # 切工作区（没建过就按名字建一块）
vidowmsg move-window-to-output --title 演示 DP-2
vidowmsg toggle-floating --app-id mpv
vidowmsg set-window-size 800 600 --title 演示
vidowmsg output-power DP-1 off  # 关掉一块屏（窗口留着，只是不画了）
vidowmsg output-scale DP-1 1.5  # 也可以 auto（按配置重算）
vidowmsg set-config window_open_ms 60
vidowmsg reload-config          # 等于 kill -HUP
vidowmsg action "workspace 2"   # 键位绑定里那套动作字符串，原样跑
vidowmsg watch                  # 订阅事件，一行一条打出来
```

### 协议

socket 路径：`$XDG_RUNTIME_DIR/vidow-$WAYLAND_DISPLAY.sock`（`WAYLAND_DISPLAY` 就是
vidow 自己的，比如 `vidow-wayland-1.sock`）；`ipc_socket` 可以写死一个路径。
`config.lua` 里能问 `vidow.ipc_path()` 拿它 —— 起状态栏时把路径喂给子进程，别让它猜；
配置里启动的子进程还会继承 `VIDOW_SOCKET`（vidowmsg 认它），所以
`vidow.exec("vidowmsg watch")` 之类的用法不用带路径。

请求一个、应答一个（都是**一行**，`\n` 结尾）：

```
请求   {"cmd":"windows"}
应答   {"ok":true,"result":[…]}
失败   {"ok":false,"error":"没有 id = 9999 的窗口"}
```

- 命令名里的 `-` 和 `_` 等价（`toggle-floating` = `toggle_floating`）。
- 指认目标：`id`（最准，窗口的稳定编号）> `app_id`（子串，大小写不敏感）> `title`（子串）
  > 不写 = 当前聚焦的窗口。输出用 `output`（连接器名，子串也行），工作区用 `workspace`
  （名字，`next` / `prev` 也认）或 `index`（0 起）。
- 字符串 / 数字 / 布尔都当文本读（`{"workspace":2}` 和 `{"workspace":"2"}` 一样）。

### 状态输出里有什么

`state` 是一份全量快照，单独的命令可以只取一块：

| 命令 | 里面是什么 |
| --- | --- |
| `state` | 下面所有的 + 计数（窗口 / 列 / 输出 / 工作区 / layer / 客户端 / 动画 / 订阅者） |
| `windows` | 每个窗口：`id`、`kind`（xdg / x11）、`title`、`app_id`、`pid`、`geometry`（布局坐标）、`float_geometry`、`surface`（表面尺寸 / 缓冲 / scale / `tex_u0`+`tex_v0`+`tex_du`+`tex_dv` 装饰正在采源缓冲的哪一块 + `frame_width`+`frame_height` 装饰画出来的框——客户端不接受 configure 时会比布局槽位大）、`column`（列 id / 宽比 / 偏移 / 序号）、`workspace`、`output`、`home`、`floating` + `float_mode`、`fullscreen`、`minimized`、`mapped` / `visible`、`rules`（三条规则的实际取值）、`size_hints`、`anim_*`、`force_render`、`capture_holders`、X11 还多一份 `x11` 子对象 |
| `columns` | 每条列：`id`、`width_fraction`、`maximized`（是不是最大化的那条列，最大化时 `width_fraction` 顶到 1.0）、算出来的 `width` / `offset`、序号、里面的窗口 |
| `workspaces` | 每块工作区：名字、`special`、`active` / `visible` / `shown`、住哪块屏 / 归属哪块屏、滚动位置、列与窗口、离开时的焦点窗口（**特殊工作区也在列表里**，`special = true`；Lua 的 `vidow.workspace_list()` 不给它） |
| `outputs` | 每块屏：连接器名 / 描述 / make / model、`enabled`、`scale`、`transform`、刷新率、物理尺寸、布局矩形、`usable_area`、`viewport_x`、活动 / 显示着的工作区、它上面的工作区 / 窗口 / layer、**所有可用模式**、`adaptive_sync` / `vrr_supported`、`depth` / `render_format`、`primaries` / `transfer_functions` / `hdr_capable`（这块屏**自己报**的色彩能力，HDR 可行性就看它）、模糊贴图状态 |
| `layers` | 每个 layer surface：`namespace`、`layer`、`output`、`mapped`、`exclusive_zone`、`keyboard_interactivity`、`anchor`、`margin`、想要 / 实际尺寸、几何、`capture` / `capture_mosaic`（抓帧遮挡规则）、`overview`（进全景怎么露面：`always` / `only` / `never`）+ `shown`（现在画没画）、`rule`（算出来的开合动画 / 不透明度） |
| `config` | **配置表的全部键值**（跟 `config.lua` 里 `vidow.get` 读到的是同一个东西）；`config` + `key` 只取一个 |
| `keybinds` / `gestures` / `rules` | 键位表（触发串 + 动作串）、触控板手势表（触发串 + 动作串 + 手指数 + 哪种手势）、窗口规则（含「这一条到底写没写」的 null 语义） |
| `seats` / `input` | seat 的能力列表、当前键盘的布局 / repeat / LED、所有键盘（含虚拟键盘和 vidow 自己的注入键盘 `vidow-inject`）、光标位置、当前的交互式 grab、指针约束（`pointer_constraint`：`type` / `lifetime` / `window` /
`cursor`）、键盘那两个（`shortcuts_inhibited`：焦点那个表面有没有生效中的抑制；`keyboard_grab`：`null` 或 `{type: xwayland, window}`，X11 抓着键盘时）、拿着键盘的 layer surface |
| `tokens` | 活着的 xdg-activation 令牌（app_id、serial 校验过没过、活了多久） |
| `memories` | 热插拔记忆（每块见过的屏：在不在、上次的布局位置、离开时的滚动位置和焦点） |
| `anims` | 正在跑的动画（tag / 进度 / 时长 / 缓动）+ 正在淡出的关闭快照 |
| `background`（别名 `wallpaper`） | 谁在画桌面背景：`color`（兜底底色）+ `clients` + `surfaces[]`（background 层的 layer 对象）+ 每块屏 `covered`（有人铺满了没有） |
| `im` / `overview` / `xwayland` / `cursor` / `focused` | 输入法状态、全景状态（相机 / 缩放）、XWayland 的开关与全局缩放、光标坐标、焦点三件套 |

几何是**布局坐标**，跟 `place` 日志、grim 的像素坐标是同一套（单屏时就是屏幕坐标）。

### 命令

| 类别 | 命令 |
| --- | --- |
| 窗口 | `move up\|down [follow]`（`follow` = 搬完跟着走）、`focus-window`、`activate-window`（= 点通知：先把工作区端出来再给焦点）、`close-window`、`minimize-window`、`unminimize-window`（不带参数 = 最近收起来的那个）、`toggle-minimize`、`toggle-floating`、`set-floating`、`toggle-fullscreen`、`set-fullscreen`、`set-window-size`、`set-window-position`、`center-window`、`set-window-force-render`、`focus-window-direction`、`move-window-direction`、`move-window-to-workspace`、`move-window-to-special`、`move-window-to-output` |
| 列 | `focus-column`、`move-column`、`cycle-column-width`、`set-column-width`（`fraction` 绝对值 / `delta` 相对）、`expand-column-to-available-width`、`center-column`、`toggle-maximize-column`、`set-maximize-column`（`maximized` true/false）、`expel-window-from-column`（把窗口从列里拎出来单独成列，`direction` 不写 = 右边）、`swap-window`（和相邻列里同层的窗口换位置，`direction` 必写） |
| 工作区 | `focus-workspace`（名字 / `next` / `prev` / `index`）、`toggle-special`（= `focus-special`）、`create-workspace`、`move-workspace-to-output` |
| 输出 | `output-power`（`on` / `off` / `toggle`）、`output-enable` / `output-disable` / `output-toggle`、`output-scale`（数字或 `auto`）、`output-transform`、`output-mode`（宽 高 [刷新率]）、`output-position`、`output-focus` |
| 剪贴板 | `clipboard`（查状态）、`clipboard-get`（取数据：文本或 base64）、`clipboard-set`（放数据：文本 / base64 / 指定 mime / 指定选区）、`clipboard-set-files`（`files` = 路径数组，发 uri-list + gnome-copied-files，见「文件复制」）、`clipboard-clear` |
| 剪贴板历史 | `clipboard-history`（条数 / 字节 / 上限 / 条目表：id、预览、mime、文件表）、`clipboard-history-get`（`id` 或 `index` + 可选 `mime` / `base64`，取全文）、`clipboard-history-use`（拿回剪贴板）、`clipboard-history-delete`、`clipboard-history-clear` |
| 合成器 | `action`（键位那套动作字符串）、`set-config` / `get-config`、`reload-config`、`toggle-overview`、`mru`（最近窗口台账 + 面板几何）、`move-cursor`、`quit` |
| 输入注入 | `pointer-button`（`click` / `press` / `release` + `button` / `count`）、`pointer-scroll`（`dx` / `dy` / `discrete`）、`send-key`（`state` + `keys`）、`type-text`（`text`）；见下面「输入注入」一节 |
| 订阅 | `subscribe`（可带 `events` 过滤器）、`unsubscribe` |
| 事件 | `hooks`（配置里挂着的钩子：谁在听、跑过几次 + 事件名单）、`events`（全部事件名）、`emit`（自己发一个事件） |
| 环境 | `env`（vidow 管着哪些变量、会推哪些给 systemd）、`set-env` / `unset-env`（改一个变量） |

几条语义值得记一下：

- **关屏**（`output-power off`）只是「不画了」：窗口、焦点、工作区都不动，打开就照原样
  回来；关掉的要是当前聚焦的那块屏，焦点会自动挪到还开着的屏上。
- **`set-window-size` 只对浮动窗口有效**（平铺尺寸由布局定），而且是「请求」：客户端可以
  拒绝（固定尺寸的对话框）。我们要的尺寸会一直保持到它认下来 —— 一个迟到的旧尺寸提交
  不能把这条命令吃掉（踩过这个坑，见下）。
- **`set-config` 是运行时改**：改完立刻生效（外观类的东西下一帧就是新的），但**不落盘**，
  SIGHUP / `reload-config` 之后按配置文件重来。
- **`quit` 是正常退出**：走完整的析构路径（顺带修掉了一个一直藏着的 wlroots 断言，见下）。
- **跑 shell 有两条路**：`spawn <命令>` 这个动作串（`vidowmsg action "spawn kitty"`，
  双 fork、不等它、不阻塞渲染），或者 `emit` 一个事件让**配置里**写好的钩子去跑
  （`vidow.on("my-event", "…")`，见下面「事件钩子」）。安全边界就是 socket 的权限：
  `0600`，只有本人能连；输入注入那一套（`pointer-button` / `send-key` / `type-text`）还能
  用 `input_injection = 0` 整个关掉。

### 事件订阅

`{"cmd":"subscribe"}` 之后这条连接上来的全是事件行，不再应答：

```json
{"event":"window-opened","time_ms":123456,"data":{…窗口对象…}}
```

| 事件 | 什么时候 |
| --- | --- |
| `window-opened` / `window-closed` | 窗口上屏 / 消失（关闭事件带标题快照：客户端的标题在销毁信号来的时候已经收走了） |
| `window-title-changed` | 标题变了（规则按标题匹配，所以这时会重套一遍规则） |
| `window-moved` | 布局把它挪了 / 缩了（第一次摆放也报） |
| `window-focus-changed` | 键盘焦点换人（载荷是新的焦点窗口，没有就是 `null`） |
| `window-floating-changed` / `window-fullscreen-changed` / `window-workspace-changed` | 浮动 / 全屏 / 工作区变了 |
| `window-minimized` / `window-unminimized` | 收起 / 放回（载荷是那个窗口） |
| `window-activated` | 应用喊「看我」：xdg-activation 的令牌、X11 的 `_NET_ACTIVE_WINDOW`、IPC 的 `activate-window` 都走这儿 |
| `drag-started` / `drag-ended` | 拖窗口起 / 停（载荷是那个窗口；挪动和改列宽都算，见 `src/drag.c`） |
| `column-maximize-changed` | 那条列的最大化状态变了（载荷是列的完整对象，带 `maximized`） |
| `workspace-created` / `workspace-removed` / `workspace-activated` / `workspace-changed` / `workspace-special-changed` | 工作区建了 / 拆了 / 切了 / 换屏了 / 特殊工作区开关了 |
| `output-added` / `output-removed` / `output-changed` | 屏插拔 / 缩放 / 方向 / 模式 / 位置变了 |
| `output-power-changed` | 某块屏开了或关了（`name` + `enabled`；窗口和布局一件没动，见「关屏」那条） |
| `layer-mapped` / `layer-unmapped` / `layer-closed` | layer surface 上屏 / 下屏 / 没了 |
| `clipboard-changed` / `primary-changed` | 剪贴板 / 选区的内容或状态变了（载荷是那一刻的状态对象，跟 `clipboard` 命令一样） |
| `clipboard-history-changed` | 历史变了（`reason` = `push`（新记一条）/ `dup`（重复，挪到最前）/ `delete` / `clear`，外加那一刻的 `count` / `bytes`） |
| `mru-shown` / `mru-opened` / `mru-committed` / `mru-cancelled` | Alt+Tab 切换器：面板显形 / 打开 / 选定 / 取消（没有载荷，去查 `mru`） |
| `keybind` | 绑好的键被按下（`trigger` = `Mod+1` 这样的触发串、`action` = 那条动作串） |
| `ipc-action` | 从 IPC 来的动作（`action` + `source: "ipc"`）—— 和键位那条分开报，「谁按的」分得清 |
| `input-device-added` / `input-device-removed` | 输入设备插拔（`name` + `type`：keyboard / pointer / touch / tablet…；虚拟键盘也算，名字就是协议给的 `wlr_virtual_keyboard_v1`） |
| `xwayland-ready` | X 服务真的起来了（懒启动，第一个 X 客户端连上来才有这一下）：`display` + `scale` |
| `screencast-started` / `screencast-stopped` | 有人开始 / 结束录屏了这一路（`output` / `kind` = output / window / region / virtual / `streams` = 还剩几路） |
| `background-changed` | 画壁纸的那个 background 层 surface 上屏 / 下屏 / 没了（载荷 = `vidowmsg background` 那一份状态，不是单个 surface） |
| `overview-toggled` / `config-reloaded` / `config-failed` | 全景开关、配置重载、配置没跑成（`config-failed` 带 `error`） |

事件全名单跟着代码走：`vidowmsg events`（现在 43 个）。名字由调用方定的**自定义事件**
（`vidowmsg emit my-event`）不在名单里，订阅和钩子照样能听。

`subscribe` 可以带过滤器：`{"cmd":"subscribe","events":["window-opened"]}`，只收订了的
那几种。没人要的事件完全不发 —— 判断的是「IPC 订阅者**或者**配置里的钩子」
（`vidow_event_wanted()`）：不订阅、也没写 `vidow.on()` 就不花这份钱。

### 配置

```bash
ipc = 1                      # 默认开；关掉就不建 socket（0 = 关）
ipc_socket = /run/user/1000/vidow.sock   # 留空 = 按 WAYLAND_DISPLAY 自动取
```

环境变量覆盖照旧：`VIDOW_IPC=0` / `VIDOW_IPC_SOCKET=…`。

### 测试

`./test-ipc.sh`（全 headless，162 项断言）分四段：

1. **状态输出**：窗口的标题 / app_id / 几何 / 列 / 工作区 / 输出 / 规则，而且几何要跟合成器
   自己的 `place` 日志**对得上**（不是自己编的数）；layer surface 的 namespace / 层 /
   锚点 / 独占区；输出的模式 / 缩放 / 可用区 / 活动工作区；`pick-window` 的命中测试
2. **操作**：每一条命令都**回读状态**确认真的变了；关键一条还要 grim 像素作证（浮动窗口
   真的画在它被摆的地方 —— 取样点按「内容象限中心」算，因为 CSD 的边距在表面里内缩）
3. **事件订阅**：打开 / 移动 / 浮动 / 关闭 / 工作区切换 / 输出变化各收一遍，加上过滤器
   （订了 `window-opened` 就不该收到 `output-changed`）
4. **多屏**：两块 headless 输出上搬窗口 / 搬工作区 / 关掉其中一块（窗口留着、另一块照常）

错误路径同样是判据：未知命令、未知 id、坏 JSON（要指出第几个字符）、一条连接上连发三条
请求（三条都要有应答）、超过 1 MiB 的一行（报错并断开，而不是把内存吃光），之后合成器
还得照常应答。

### 顺手修掉的两个真 bug

1. **正常退出会 abort**（`quit` 暴露的，一直藏着）：wlroots 的几个全局对象在销毁时断言
   「没人还挂在我这儿」——
   `types/xdg_shell/wlr_xdg_shell.c:134: Assertion 'wl_list_empty(&xdg_shell->events.new_toplevel.listener_list)' failed`。
   以前 vidow 只有「被 SIGTERM 杀掉」这一种退法（信号默认动作直接结束进程，走不到析构），
   所以从没露过面；IPC 的 `quit` 让正常退出成了常规路径，它才冒出来（退出时 SIGABRT +
   core dump）。现在退出前统一把挂在 wlroots 对象上的监听器和事件源摘下来
   （`main.c` 的 `server_finish()`）。
2. **改浮动窗口尺寸会被一个迟到的提交吃掉**：`set-window-size` 之后，客户端要是先为别的
   原因提交一次**旧尺寸**，`vidow_float_on_commit()` 会拿旧尺寸把浮动盒改回去，随后 arrange
   又按旧尺寸 configure —— 等于这条命令没发生。现在记着「我让它多大」（`float_want_w/h`），
   在客户端认下来之前不采纳它的旧尺寸，并且再催一次 configure。

### 事件钩子：`vidow.on()`（配置里直接听事件）

事件本来只有一条路：外挂脚本连上 socket 订阅（`subscribe`）。配置里也能听：

```lua
-- 起一条命令（事件字段变成环境变量，见下）
vidow.on("window-opened", "notify-send \"$VIDOW_APP_ID\" \"$VIDOW_TITLE\"")
-- 跑一段 Lua（载荷是一张表：e.event / e.time_ms / e.data.*，跟那条 JSON 同构）
vidow.on("window-opened", function(e)
	if e.data.app_id == "mpv" then
		vidow.log("mpv 来了：" .. tostring(e.data.title))
	end
end)
-- "*" = 全部事件；也能一次挂一串（返回 id 表，vidow.off() 摘）
local ids = vidow.on("*", {
	"echo $VIDOW_EVENT >> /tmp/events.log",
	function(e) vidow.log(e.event) end,
})
vidow.off(ids[2])
```

- **命令钩子**：事件字段变成子进程的环境变量 —— `VIDOW_EVENT`、`VIDOW_EVENT_TIME_MS`、
  `VIDOW_EVENT_JSON`（整个信封的 JSON，要什么有什么），外加 `data` 里每个**标量**字段一条
  `VIDOW_<字段名大写>`（`app_id` → `VIDOW_APP_ID`，`-` 换成 `_`；数组 / 对象跳过，它们在
  `VIDOW_EVENT_JSON` 里）。命令跑完就把这些变量还原，不留在 vidow 自己的环境里。
  跑的是和 `spawn` 同一套：双 fork、不等它、不阻塞渲染。
- **Lua 钩子**：载荷是解码后的表（`e.event` / `e.time_ms` / `e.data`）+ 第二个参数给原始
  JSON。里面**能 await `vidow.exec()`** —— 和配置文件里那套协程一样（yield 一条命令、
  回来接着跑），所以「开窗时查一下什么东西再决定」写得出来。
- 出错只打日志（`钩子 #3 出错：…`），**不连累**同一个事件的其它钩子，也不会把合成器带下去。
- 钩子一律进队、在事件循环的 idle 里跑（钩子能做的事和 IPC 命令一样多 —— 改配置、关窗口、
  开会话，不能站在合成的半路上）。队列上限 256 条：钩子里干的活太重、积到上限就丢新的并
  打一条 error，免得无声吃内存。
- **配置重载 = 钩子重新声明**：旧的整批作废（Lua 引用还回去），新配置里写了什么就是什么。
  所以配置文件里删掉一行 `vidow.on`，它就真的没了。
- 自定义事件：`vidowmsg emit my-event --data '{"n":1}'` 发一个，订阅者和钩子一起收
  （`--data` 给的是 JSON 本身，不是字符串）。脚本之间、脚本和配置之间的一条通道。

### 自启动：`exec` / `exec_once`

**两个入口**：`vidow.exec()` 跑一条命令（等不等、要不要退出码看选项），`vidow.exec_once()`
一个 vidow 生命周期只起一次。原来的 `vidow.spawn()` 和 `vidow.exec_status()` 已经并成
`vidow.exec()` 的选项 —— 老写法会报一句人话告诉你改成什么。

```lua
vidow.exec("fcitx5 -d", { wait = false })    -- 撒手：不等、不要输出（起常驻程序用它）
local host = vidow.exec("hostname")          -- 等它跑完，拿 stdout（400ms 超时）
local out, code, err = vidow.exec("grim /tmp/a.png", { status = true })   -- 要退出码
vidow.exec_once("waybar")                    -- 整个 vidow 生命周期只起一次
vidow.exec_once{ "swaybg -i 壁纸.png", "mako" }
```

| 写法 | 什么时候起 | 等它吗 | 返回值 |
| --- | --- | --- | --- |
| `vidow.exec(cmd)` | 配置跑到那一行就起 | 等（协程让出，不阻塞渲染） | `out`；失败 `nil, 原因` |
| `vidow.exec(cmd, { status = true })` | 同上 | 等 | `out, code, err`（永远三个值） |
| `vidow.exec(cmd, { wait = false })` | 同上 | 不等 | `true` |
| `vidow.exec_once(cmd)` | 这份配置跑完之后起；**重载不重来** | 不等 | `true` |

选项表就两个键（`wait` / `status`），拼错当场报错并列出能写哪些；`exec_once` 只认默认的
撒手跑，传 `wait = true` 会报错（它是等这份配置跑完才起的，等不了）。

- `exec_once` 的命令**先排队**，等这份配置跑完（环境、缩放、`DISPLAY` 都定了）再以撒手方式
  起 —— 所以配置里写多早都不影响它看到的环境。这对应 Hyprland 的 `exec-once`。
- 「起过了」按**命令原文**记：重载之后同一行还是同一行，认得出、不重来；把命令原文改了就
  是一条新命令，会再起一次（和 Hyprland 一个规矩）。
- 名单是「只进不出」的话，删删改改攒到 128 条上限之后就再也记不住新命令了（那时每次重载都
  会把它当没起过、再起一遍）。所以每轮配置跑完对一次账：**这一轮没声明的就从名单里划掉**
  —— 删掉再写回来 = 一条新命令，会再起一次。
- 起子进程的三条路（`exec` 等 / `exec` 撒手 / `exec_once`）都**继承 vidow 的完整环境**：
  `WAYLAND_DISPLAY` 指向 vidow 自己、`DISPLAY`（X 起来之后）、`XDG_CURRENT_DESKTOP`、
  `VIDOW_SOCKET`、以及 `vidow.env{}` 设过的变量 —— 状态栏、通知守护、输入法直接就能连上，
  不用自己猜 socket 路径。
- `vidow.exec()` 默认 400ms 超时（`VIDOW_EXEC_TIMEOUT` 改，超时按进程组 SIGKILL）。要起常驻
  程序别用等的那条（后台进程继承着捕获用的管道，EOF 永远不来 → 400ms 后被杀），用
  `{ wait = false }` 或 `exec_once`。

## 自启动 / 会话环境：和 systemd 用户管理器两个方向对账

从 vidow 里起来的命令继承的是**进程环境**，而由 systemd / dbus 拉起来的应用（xdg-desktop-
portal、`systemd --user` 的服务、桌面文件激活的程序）继承的是**用户管理器里的环境**。两个不
打通就会出现「配置里起的程序连得上 vidow，systemd 起的那个连的是宿主合成器（甚至没有
Wayland）」。所以两个方向都做：

**推（vidow → systemd / dbus）**：`systemd_env_export`（默认开）

```bash
systemctl --user import-environment WAYLAND_DISPLAY DISPLAY XDG_CURRENT_DESKTOP …
dbus-update-activation-environment --systemd …
```

- 推的是：`WAYLAND_DISPLAY`、`DISPLAY`、`XDG_CURRENT_DESKTOP`、`XDG_SESSION_TYPE`、
  `XDG_SESSION_DESKTOP`、`VIDOW_SOCKET` + **用户 `vidow.env{}` 声明过的每一个**（没设的变量
  不推，推空值反而会把好环境擦掉）。
- 时机：socket 定下来之后推一轮、X 起来（`DISPLAY` 变了）再推一轮、配置落地后再推一轮；
  运行时改过变量（`vidow.env` / `vidowmsg set-env`）也在下一个 idle 里推。合并着推：连改
  十个变量也只 fork 一次。
- 推之前先记下「用户管理器里原来是什么样」，**退出时还回去**（这是从 portalconf.c 那套学来
  的规矩）。嵌套跑 / headless 测试时这一步最要紧：不还的话桌面里由 systemd 拉起来的程序
  全指着已经退了的 vidow。
- **只碰「我们够得着的」那个用户管理器**：认 `systemctl --user show-environment` 这一条，够
  不着就什么都不推、也不还原。嵌套跑（vidow 里再起一个 vidow）和 headless 测试时
  `XDG_RUNTIME_DIR` 是临时的，`systemctl --user` 连不上（`Failed to connect to user scope
  bus via local transport`）—— 但 `dbus-update-activation-environment` / `busctl` 走的是会话
  总线（`DBUS_SESSION_BUS_ADDRESS`），照样能改到**宿主会话**的用户管理器上：推上去的是嵌套
  里那个 `WAYLAND_DISPLAY`，退出还原还会把宿主本来就有、我们却以为「原来没有」的变量
  `unset` 掉（实测删掉过宿主 Hyprland 会话的 `WAYLAND_DISPLAY` / `DISPLAY` /
  `XDG_CURRENT_DESKTOP`）。够不着 = 我们不是这个会话的主人，那就不碰。
- **读**还可以退到第二条路（`busctl --user get-property … Manager Environment`）：读不会改
  别人，而且从 tty 起 vidow 时「用户管理器里有什么」正是要吸进来的东西。写只走 systemctl。
- 还原那一步两条路都发：`systemctl --user set-environment` / `unset-environment` **加上**
  `dbus-update-activation-environment --systemd` / `busctl … UnsetEnvironment` —— 前者管
  systemd 用户管理器，后者管 dbus 的激活环境（两套账，都得还）。`busctl` 会等应答；
  `dbus-send` 不等，消息会丢（试过，删不掉）。

**吸（systemd → vidow）**：`systemd_env`（默认开）

- 启动时问一次用户管理器，把**我们自己没有的**变量吸收进来（PATH / `SSH_AUTH_SOCK` /
  `environment.d` 里那些）—— 从 tty 直接起 vidow 时环境很寡，配置里起的子进程才有一份完整的
  会话环境。
- 不吸的：`WAYLAND_DISPLAY` / `WAYLAND_SOCKET` / `DISPLAY` / `XDG_SESSION_TYPE` /
  `XDG_SESSION_DESKTOP` / `XDG_CURRENT_DESKTOP`（「哪个合成器」的身份，必须由 vidow 自己定）、
  以及 `WLR_*` / `VIDOW_*`（会把我们的后端选择搅乱）。我们自己已经有了的也不动 —— 启动 vidow
  的那个 shell / service 说了算。
- 配置里写 `systemd_env = 0` 的话，启动时（那时只有默认值）吸进来的会**还回去**；用户自己
  `vidow.env` 显式设过的不动（那是他显式要的）。
- 环境变量覆盖：`VIDOW_SYSTEMD_ENV=0` / `VIDOW_SYSTEMD_ENV_EXPORT=0`（配置还没跑之前只有环境
  变量能说话，所以启动那一步也认这两个）。另外 `VIDOW_PORTAL_NO_RESTART=1`（portalconf.c 那边
  的老开关，语义是「一次 `systemctl` 都别调」）也管这里：吸 / 推 / 还原全都不做。

**故意不推的**：`XDG_RUNTIME_DIR`（那是会话自己的东西；嵌套跑 / headless 里 vidow 的是临时
目录，推上去等于把所有 systemd 拉起来的程序指到一个不存在的 runtime 目录 —— 踩过这个坑。
portal 那边要连 PipeWire，自己带着快照 / 还原管它，见 `src/portalconf.c`）、`XCURSOR_*`
（会改掉系统里所有程序的指针大小）。GDK / Qt 那几个缩放变量也不推（那是给 X11 客户端的提示，
各家自己会算）。

**vidow 自己的环境**（`vidow.env` / IPC 的 `env`、`set-env`、`unset-env`）：

```lua
vidow.env("GDK_SCALE")                  -- 读（没有就是 nil）
vidow.env("MOZ_ENABLE_WAYLAND", "1")    -- 写（顺手推给 systemd）
vidow.env("FOO", false)                 -- 删
vidow.env{ A = "1", B = false }         -- 一次一串
vidow.env_export()                      -- 立刻推一轮（平时是自动的）
```

- 名字得是 `[A-Za-z_][A-Za-z0-9_]*`，写错当场报错（`1bad` 这类）。
- 改过的变量**记着原值**，vidow 退出时还原 —— vidow 是从别人的 shell / systemd 里起来的，
  改过的东西得还回去。`vidowmsg env` 把账本一条条报出来（谁改的、原值是什么、会推给 systemd
  的是哪些、推过几次）。

```bash
systemctl --user show-environment       # 用户管理器里现在是什么（推成功了没）
./test-env.sh                           # 这一节的回归（42 项断言，假的 systemctl / busctl /
                                        #  dbus-update-activation-environment 记调用，一个字符
                                        #  都不会动你真会话的环境）
```

### 配置

```bash
systemd_env = 1                 # 启动时从 systemd 用户管理器吸环境（我们自己没有的才吸）
systemd_env_export = 1          # 把 vidow 的环境推给 systemd / dbus
```

环境变量覆盖：`VIDOW_SYSTEMD_ENV=0` / `VIDOW_SYSTEMD_ENV_EXPORT=0`。

### 测试

`./test-env.sh`（全 headless + 假的 systemctl / busctl / dbus-update-activation-environment
记调用，42 项断言）八段：吸收（吸哪些 / 不吸哪些）、推送（变量清单 + 两条通道）、`vidow.env`
读写删 + 账本、IPC 的 `env` / `set-env` / `unset-env`、`exec_once` 只起一次（重载不重来、
改原文才重来、配置里删掉的会被忘掉）、两个开关（配置键 + 环境变量覆盖）、退出时把用户管理器
还原、`systemctl --user` 够不着时整个跳过（一条命令都不发）。

**够不着就不碰**：写用户管理器只走 `systemctl --user` 这一条 —— 嵌套跑 / headless 测试里
`XDG_RUNTIME_DIR` 是临时的，`systemctl --user` 连不上（`Failed to connect to user scope bus
via local transport`），这时 `dbus-update-activation-environment` / `busctl` 走会话总线仍然能
改到**宿主会话**的用户管理器上（推的是嵌套里那个 `WAYLAND_DISPLAY`，还原还会 unset 掉宿主本
来就有的变量 —— 实测删掉过宿主 Hyprland 会话的 `WAYLAND_DISPLAY` / `DISPLAY` /
`XDG_CURRENT_DESKTOP`）。所以够不着就整个跳过，日志里写一句为什么。

## 输入注入：IPC 能操控鼠标和键盘

「谁连上 socket 谁就能操控整个桌面」。这是给自动化 / 远程操控 / 测试用的，也是 UI 识别
（[`docs/ui-tree-feasibility.md`](docs/ui-tree-feasibility.md)）的另一半：能看见控件，还得能
点下去 —— 而且对**没有无障碍树**的应用（游戏、自绘 GUI），合成输入是唯一能动手的办法。

### 五条命令

| 命令 | 干什么 |
|------|--------|
| `move-cursor x y [--relative]` | 把指针移到（布局坐标），顺带更新指针焦点 |
| `pointer-button click\|press\|release [键] [连点数]` | 点 / 按住 / 松开；键认 `left` `right` `middle` `back` `forward` 和数字 |
| `pointer-scroll dx dy [--discrete false]` | 滚轮（dx/dy 是**格数**）；`--discrete false` = 触摸板式连续滚动 |
| `send-key [tap\|press\|release] 组合键` | `Return` / `ctrl+shift+t` / `super+Tab` / `XF86AudioMute` |
| `type-text 文字` | 逐字符打（ASCII / Latin-1；中文要走输入法，会明确报错） |

```bash
vidowmsg move-cursor 300 200             # 挪到 (300,200)
vidowmsg pointer-button click            # 左键点一下
vidowmsg pointer-button press left       # 按住不放（拖拽前半段）
vidowmsg move-cursor 100 50 --relative   # 相对挪（拖拽后半段）
vidowmsg pointer-button release left     # 松手
vidowmsg pointer-scroll 0 -3             # 往上滚三格
vidowmsg send-key tap ctrl+c             # 给焦点窗口发 Ctrl+C
vidowmsg type-text "hello world"         # 打一段字
```

### 两条设计原则

1. **走和真设备完全同一条路**。指针走 `vidow_pointer_motion` / `vidow_pointer_button` /
   `vidow_pointer_axis`，按键走 seat 的 `wlr_seat_keyboard_notify_*`。所以注入的点击会像真
   点击一样抢焦点、起拖拽；注入的 super 按住时滚轮是**换列**（不是滚客户端）。测试注入
   （`VIDOW_TEST_ACTION` 的 `pointer-motion` / `pointer-button` 动作）用的就是这条路。
2. **注入的按键只给客户端，不触发 vidow 自己的快捷键**。要操作合成器请用对应的 IPC 命令
   （`focus-window` / `toggle-overview` / `focus-workspace`…）—— 否则「给应用发 Ctrl+C」会被
   绑定的 Ctrl+C 吃掉。测试里专门验了这条。

### 键盘是哪来的

往 seat 上送按键需要一个 `wlr_keyboard`，而 wlroots 没公开「造一个键盘」的接口，只有**键盘组**
能给出一个干净的（自带 keymap 和 xkb_state）。所以 vidow 启动时建一个 `vidow-inject` 键盘
（`vidowmsg input` 里能看到，`inject: true`），keymap 和真键盘同一份（系统默认布局），按键时
借给 seat 用一下 —— 真键盘一按就自己抢回去了。

组合键的键码是**从 keymap 反查**的：`xkb_keymap_key_get_syms_by_level` 扫一遍
keycode × level，找到能打出这个 keysym 的那个键，再按 level 补上 Shift / AltGr
（`A` 要 Shift、`@` 要 Shift+2、`!` 要 Shift+1 都是这么来的）。

headless 会话里这个键盘还有个额外好处：座位因此报出 KEYBOARD 能力，客户端才会去要
`wl_keyboard` —— 否则注入的键没地方送。

### 安全边界

- socket 是 **0600**（只有本用户能连），但连上之后所有命令都能用 —— 包括这五条。
- 默认**开**（`input_injection = true`）。要收紧就写 `false`：五条命令全部拒绝，理由写清楚。
- 每次注入都打一条 **info 日志**（`注入: 指针移到 300,200` / `注入: 按键 ctrl+c（按下并松开）`）。
  指针移动那条**按 100ms 分档**：同一个 100ms 窗口里**第一条走 info、剩下的走 debug** ——
  注入能比真鼠标还快（基准里灌过 20000 条连发），每条一行既刷屏又拖慢注入本身；手点一下、
  脚本挪一下在 info 下照旧看得见，要把连发一条不漏看全就 `VIDOW_LOG=debug`。
- 注入的按键不碰 vidow 自己的快捷键（见上面第 2 条），所以它按不动 vidow 的绑定。

### 测试

`./test-input-inject.sh`（全 headless，54 项断言）拿 **wev**（第三方 Wayland 事件查看器）当
观察者 —— 「事件真的到客户端了」由它说了算，不是合成器自己说：窗口内坐标（窗口在 6,6，
光标到 300,200 → 局部 294,194）、相对移动、左右键的 press/release/连点、滚轮的
`axis_value120`（一格 = 120、delta = 15）与连续滚动的区别、`ctrl+shift+t` 的**顺序**
（修饰键先按后松）、`Hi!` 里每个字符的 Shift、注入 `super+t` 不触发绑定但客户端照样收到、
按住 super 滚轮不转发给客户端、`input_injection = false` 之后全部拒绝。

顺带修掉一个真 bug：`move-cursor` 以前只挪光标位置、**不给客户端发 motion**
（`wlr_cursor_warp` 就是这样，只改坐标不发事件），现在它和真鼠标一样会更新指针焦点 ——
这也是「脚本点了没反应」类问题的根源之一。

## 摇晃找光标：晃两下指针就变大（晃到找着为止）

鼠标找不着的时候，**快速来回晃几下**：晃一下就大一圈，接着晃接着大 —— **线性加**，
不是乘（`cursor_shake_scale = 2` 是「第一圈就到大一倍，之后每圈再 +1」：2x → 3x → 4x → 5x，
乘的话 2、4、8、16 一路炸开，第一下就大得吓人）。**不封顶**，只封到「屏幕那么大」（比屏幕
还大的光标没意义），停下来 700ms 自己**动画**缩回去（200ms 线性补间，不是「啪」地跳）。
也可以随时 `vidowmsg cursor-shake` 手动放大一圈（脚本 / 远程用）。

### 放大的是「光标图」，不是另画一层

放大不是往场景里塞一个节点，而是**换一张更大的光标图**：把当前光标图（客户端设的
`wl_pointer.set_cursor` surface，或者主题里的箭头）放大重画进一张新 buffer，再交给
`wlr_cursor_set_buffer()`。于是它和普通光标走**同一条路**：

- **硬件光标平面放得下就是硬件光标** → 不进合成器的渲染通道 → **截图 / 录屏里没有它**。
  这条是关键：截图里冒出一个巨大的光标，比没这个功能还糟。
- 放不下（真机上放大超出平面，比如平面只有 256x256）wlroots 会降级成**软光标** ——
  软光标是画进画面的。不过 vidow 的抓帧（`capture.c` / `screencopy.c`）是**自己渲染的
  一帧场景**，场景里本来就没有光标：**不管硬件光标还是软光标，普通截图 / 录屏里都没有它**，
  而屏幕上照旧是放大后的光标（连一帧都不用缩回去）。
- 截图应用**显式要光标**时才连放大的一起给（screencopy 的 `overlay_cursor`、新协议的
  光标会话，`grim -c` 就是它）—— 那时 wlroots 会把光标锁成软光标画进画面，正好是
  「你要就给你」。
- 放大图一律从**原尺寸那张**算起（放大开始时自己 1:1 复制一份），不是叠着放大 ——
  不然晃几下就糊成一团。补间的每一帧也是从它重画（每一帧都是清晰的，不是把上一帧
  再放大）。
- 日志把每一步写清楚：`摇晃: 光标放大 8.00x（10x16 → 80x128，软光标：会画进屏幕
  （抓帧是 vidow 自己渲染的，截图里照样没有它））` / `摇晃: 光标开始缩回（8.00x → 1.00x，
  200ms）` / `摇晃: 屏幕只有 1280x720，64.00x 放不下，到 45.00x 为止`。

### 放大和缩小都是补间

尺寸不跳：每一圈从当前倍数**线性**补间到目标倍数（放大 150ms），停手 700ms 之后再用
200ms 线性缩回 1.00x。推手是 `cursorfx.c` 里那个 16ms 的定时器（光标图不像场景，没有每帧
回调可用），每一帧按当前倍数重画一张光标图交给 `wlr_cursor_set_buffer()` —— 所以
`vidowmsg cursor` 的 `scale` 在动画中间是**中间值**，`target` 才是这一圈的终点
（两者相等 = 这一圈走完了）。

连晃几下：目标从 `target` 上累加（不是从当前值），补间还在路上时每一圈也都算数。

嵌套 Wayland / headless 后端没有真正的光标平面（光标是交给上层合成器画的），vidow 按
256x256 估个上限 —— 那种情况下「硬件光标」指的是上层合成器的光标，同样不进 vidow 自己的截图。

### 怎么算「在晃」

700ms 窗口内至少 **4 段、3 次换向**，每段主方向轴上至少走 12 个逻辑像素。慢慢往一个方向甩
不算；拖拽时照样认（拖到一半找不到指针了一样要找）。判定挂在 `vidow_pointer_motion` 上，
所以真实鼠标、触摸板、`vidowmsg move-cursor` 注入的移动都走同一条路。

### 配置

```lua
cursor_shake = true        -- 摇晃指针就放大（默认开）
cursor_shake_scale = 2     -- 每圈加多少倍：2 = 第一圈就到大一倍，之后每圈再 +1（线性加不是乘）
```

### 状态与命令

`vidowmsg cursor` 多报几个字段：`magnified`（现在是不是放大中）、`scale`（当前实际倍数，
补间中间是中间值）、`target`（这一圈的目标倍数）、`image`（当前光标图的物理尺寸，比如
`80x128`）、`hardware`（在不在硬件光标平面上）、`hw_max`（这块屏的硬件光标上限）。
`vidowmsg cursor-shake` 手动放大一圈（放大中再调就是再 +1）。

### 测试

`./test-cursor-shake.sh`（全 headless，55 项断言）拿 **grim** 当裁判 —— 「进没进截图」由像素
说了算：普通截图里**一个光标像素都没有**（放大到 450x720 也一样）；`grim -c` 抓到的外接矩形
正好是放大后的尺寸、箭头尖还钉在原来那个点上（热点按倍数跟着缩放）。再加上：
线性加（2→3→4→5→6→7 每一圈都核对，不是 2→4→8→16）、放大本身是补间（晃完 50ms 量到的
倍数严格在起点和目标之间）、缩回是补间（200ms 里密集采样，必须能量到 1 和 2 之间的中间值，
跳回去的话这一条立刻红）、一路晃到屏幕封顶、慢速不误触、客户端自己设的光标
（`tools/cursorwin.py` 的 crosshair）放大后还原的还是**它自己那张**、`cursor_shake = false` 全拒。

**软光标那条路**（真机上放大超出硬件平面就是它）单独验一遍：`VIDOW_TEST_SOFT_CURSOR=1`
把 headless 的光标钉死在软光标上（headless 的假平面什么都收，`wlr_output_is_headless()`
认的是 impl 指针，没法拿它做假），于是「软光标本来就会进画面」和「放大中抓帧一个光标
像素都没有」两件事都能在 headless 里量出来。

## 剪贴板：X11 ↔ Wayland 同步（自建桥）

### 为什么不用 wlroots 那份

wlroots 0.20 的 XWayland 剪贴板桥把「X11 窗口有没有焦点」当闸门：`xwm->focus_surface == NULL`
时，X11 客户端来要数据就回 `denying read access to selection`，X11 客户端复制了东西也
`denying write access to clipboard` —— 表现是「X11 程序里复制了，在 Wayland 程序里粘不出来，
除非那个 X11 窗口正好有焦点」。闸门写在 `struct wlr_xwm` 里，而这个结构对合成器是不透明的：
关不掉，也没有公开 API 能替它做。所以 vidow 自己接了一份（`src/clipboard.c`）。

### 接管点：`user_event_handler`

`wlr_xwayland->user_event_handler` 是公开 API，**在 xwm 的默认处理之前**被调用，返回 true 就
跳过 wlroots 的处理 —— 我们拦的正是「跳过」这件事：

| 事件 | 拿来干什么 |
| --- | --- |
| `XFixesSelectionNotify`（CLIPBOARD / PRIMARY） | X 那边换 owner 了 → 去把数据取回来 |
| `SelectionRequest` | 有人向 X 选区要数据 → 从我们的缓存回答（TARGETS / TIMESTAMP / DELETE / 各类型转换 / INCR） |
| `SelectionNotify`（requestor 是我们的窗口） | 我们要的 TARGETS / 数据到了（含 INCR 的分块） |
| `PropertyNotify`（我们的窗口 / 出方向 INCR 的 requestor） | INCR 取数的下一块到了；对面把属性删了 → 该发下一块 |

我们和 xwm 在同一台 X 上**都**注册了 XFixes，所以同一件事会来两遍 —— 按 `(owner, timestamp)`
去重，只认第一遍（不去重的话 TARGETS 的应答会被处理两次，缓存直接烂掉）。

### 不抢 X 选区的所有权

Wayland 客户端复制时，wlroots 的 xwm 会**替它**在 X 那边声明选区所有权（声明在它自己的代理
窗口上），于是所有 `SelectionRequest` 都落在那个代理窗口上 —— 正好是我们拦得到的事件。我们
从不自己 `SetSelectionOwner`，所以没有「抢所有权 → SelectionClear 风暴 → 数据来回绕」这回事。

### 两边各留一份内存副本

- **X11 → Wayland**：X 那边换 owner 时就把数据**取回来**（TARGETS，然后逐个类型 convert），
  存进每种 mime 一份的内存副本，再从副本发布到 Wayland 选区。取数是异步的（`wlr_data_source_send`
  + 管道 + 100ms 心跳的超时兜底），但**取完才发布** —— 发布早了 xwm 会先把 X 所有权抢走，把
  在飞的取数打断（踩过，见「已验证」）。
- **Wayland → X11**：Wayland 源同样急着拉一份到内存（`staging[]` 里攒着，取完再 commit 再发布），
  所以**源应用退出之后 X 那边照样能粘贴**；X 侧的请求一律由这份副本回答。
- 上限是每个类型 `clipboard_max_bytes`（默认 32 MiB）：超了就丢掉这个类型并记日志，而不是
  懒流式转发（懒转发意味着 X 侧要一直连着源应用，源应用一退就断，还要绕回 xwm 的闸门）。

### INCR 双向

大于 X 属性上限（我们按 64 KiB 一块发）的数据走 ICCCM 的 INCR 协议，两个方向都实现：
取回来时按 `PropertyNotify` 分块收（`state` 里 `fetching` / `complete` 能看见进度），发出去时
分块写、最后写一块 0 长度的收尾。

### 配置

```bash
clipboard = 1                 # 默认开；0 = 不接管，退回 wlroots 那套（只有 X 窗口聚焦时才同步）
clipboard_primary = 1         # 默认开；0 = 连 PRIMARY（选区）也不碰
clipboard_max_bytes = 33554432   # 每种 mime 的内存副本上限
clipboard_history = 32        # 历史留几条；0 = 不留（当场清空，之后也不记）
clipboard_history_bytes = 67108864   # 历史的字节预算（到顶就从头挤，但至少留一条）
```

`set-config` / SIGHUP 都能热切换：关掉时会把已经发布的选区收回（`sel_unpublish`），X 侧的
监听窗口与 XFixes 注册一起摘掉，退出路径上不留东西。`clipboard_history` / 
`clipboard_history_bytes` 也是热的：改成 0 当场清空历史。

### 历史

`clipboard_history` 条（默认 32），新的在前。一条历史 = 一次复制的内容快照：从 X11 取回来的、
从 Wayland 拉过来的、IPC 塞进去的、以及「从历史里拿回来」的都会记。

- **只留需要的几个类型**：按 `mime_rank`（见下）从前往后留 6 个 —— 够装下「文本 + uri-list +
  gnome-copied-files」这种一份内容带三四种类型的常见情况，又不会为一份复制把几十个类型全留一遍。
- **去重**：内容一样（字节数 + 选区 + 每个类型逐字节比）就不新记，把老的那条挪到最前（事件里
  `reason` 是 `dup`）。所以反复复制同一段文字不会把历史刷爆。
- **淘汰**：超过 `clipboard_history` 条、或总字节超过 `clipboard_history_bytes` 时，从最旧的
  那头删；**至少留一条**（刚复制完就把自己挤掉就没意义了）。
- **快照的时机**：只有数据**取完**才记（X11 侧 `staging` commit 完 / Wayland 侧管道读完），
  不会记半份。
- 拿回来的路径和客户端复制是同一条：`clipboard-history-use` 走 `sel_publish()`，xwm 去 X 那边
  声明所有权，我们的缓存照常应答 X 的 `SelectionRequest` —— 所以「拿回来」之后**谁**来粘都行。

### 文件复制

一份「文件复制」在 X11 / Wayland 里其实是三个类型一起发：

| 类型 | 内容 | 谁认 |
| --- | --- | --- |
| `text/uri-list` | `file:///路径` 每行一条，CRLF 结尾 | 文件管理器、X11 程序 |
| `x-special/gnome-copied-files` | 第一行 `copy`，后面每行一个 URI（LF） | GNOME / Nautilus、Thunar |
| `text/plain;charset=utf-8` | 路径文本，每行一条 | 终端、编辑器 |

```bash
vidowmsg clipboard-set-files /tmp/a.txt "/tmp/带 空格 的名字.txt"
```

路径按 RFC 3986 百分号编码（`空格` → `%20`、中文也编），`file://` 前缀自己补。
因为 `text/plain` 的 rank 排在 `uri-list` 前面，`clipboard-get` / 预览里默认看到的是**路径文本**；
文件管理器看到的仍是 uri-list，两边不打架。历史里这种条目标成 `is_file`，并且
`x-special/gnome-copied-files` 与 `uri-list` 同档保留 —— 从历史拿回来的时候还是**复制文件**，
不会退化成一段路径文本。

### 图形界面：`tools/vidowclip.py`

```bash
python3 tools/vidowclip.py                 # 窗口（GTK3，需要 python3-gi）
python3 tools/vidowclip.py --socket /path/to/vidow-wayland-0.sock
python3 tools/vidowclip.py --check         # 不建窗口，只把 IPC 走一遍（脚本里用）
```

一个独立窗口（不是 layer surface，也不进合成器内部），走的是和 `vidowmsg` 完全相同的 IPC：
订阅 `clipboard-changed` / `primary-changed` / `clipboard-history-changed`，500ms 兜底拉一次
状态（订阅断了也不瞎）。窗口里：

- **状态条**：CLIPBOARD / PRIMARY 现在是谁的（`origin`、X owner、几个类型、多少字节）。
- **历史列表**：时间 / 类型 / 来源 / 一行预览；双击（或选中后按「复制回剪贴板」）= 拿回来。
  文件条目按「复制回剪贴板」是**原样**发布三个类型（文件管理器粘出来是复制文件）；
  想按路径重新拼一份（文件改名/移动后用）用「复制文件」。
- **预览**：文本全文、图片缩略图（`image/*` 直接画）、文件条目列出路径。
- **开关**：同步剪贴板 / 同步 PRIMARY / 历史条数 / 历史内存预算 —— 改一下就是一次 `set-config`。
- **塞内容**：一行文本塞进剪贴板；「清空剪贴板」「清空历史」「删除」各一个按钮。

### IPC

```bash
vidowmsg clipboard                       # 状态：来源 / X owner / 在取数 / 字节数 / 类型列表
vidowmsg clipboard-get                   # 取剪贴板（文本直接给，别的类型给 base64 + mime）
vidowmsg clipboard-get --selection primary
vidowmsg clipboard-set "hello"           # 放剪贴板（也可以 --mime image/png --base64）
vidowmsg clipboard-set-files /tmp/a.txt  # 放「复制文件」（uri-list + gnome-copied-files）
vidowmsg clipboard-clear
vidowmsg clipboard-history               # 历史：条数 / 上限 / 条目（id、预览、mime、文件表）
vidowmsg clipboard-history-get 0         # 第 0 条（0 = 最新）的全文，也可 --id
vidowmsg clipboard-history-use 1         # 第 1 条拿回剪贴板（文件条目拿回来还是文件）
vidowmsg clipboard-history-delete 2      # 删第 2 条（也认 --id）
vidowmsg clipboard-history-clear
```

状态里 `origin` 说明这份数据现在是谁的（`wayland` / `x11` / `none`），`x_owner` 是 X 那边的
窗口 id，`waiting_x11` / `waiting_wayland` 是两边还没对齐的等待数。事件订阅里对应
`clipboard-changed` / `primary-changed`。

### 边界

- X11 那边是**从缓存回答**的，所以 `xclip -o` 只能取到文本类型（它只问文本那几个 atom）；
  要二进制得点名类型：`xclip -selection clipboard -t image/png -o`。
- 大块数据（INCR）在 `clipboard-get` 里也是一次给全，没有流式接口。
- 只管 CLIPBOARD 与 PRIMARY；XDND（拖放）照旧交给 wlroots。

## 服务端窗口装饰（SSD）

每个窗口的内容不再直接进场景，而是先画进一个离屏缓冲，由一个装饰着色器统一处理：

| 特性 | 实现 |
| --- | --- |
| 圆角 | 圆角矩形的 SDF，`smoothstep` 一像素抗锯齿 |
| 描边 | SDF 的环带；聚焦/失焦两种颜色 |
| 阴影 | 另一条偏移的 SDF，只在窗口**外侧**绘制（否则半透明窗口会透出自己的阴影）；默认半径 8px、下移 2px —— 薄薄贴边一圈 |
| 窗口透明度 | `window_opacity`，只作用于内容与描边 |
| 噪点 | 逐像素 hash 噪声，乘进阴影/模糊层 |
| 背景模糊 | kawase 双滤波（见下） |

离屏缓冲比窗口大 `shadow_radius + |shadow_offset|` 圈 padding，所以阴影不会被裁掉。
默认（`shadow_radius = 8`、`shadow_offset_y = 2`）padding 是 12px；想要大光环就往上调，
两块数字要一起看（半径太大而 padding 不够的话，阴影会在离屏图边界上被切出一条直边）。
真实表面节点仍然保留（不透明度置 0）——**点击命中不受影响**，因为输入走的是场景图，
不是像素。

配置在 `~/.config/vidow/config.lua`：`decor_enabled` / `corner_radius` / `border_width` /
`border_color` / `border_color_focus` / `shadow_radius` / `shadow_color` /
`shadow_offset_x,y` / `window_opacity` / `noise` / `prefer_no_csd`。

### 取客户端的贴图：借 `wlr_client_buffer` 自己那张，别对 `source` 另建

装饰管线把窗口画进自己那块离屏缓冲（+边框/圆角/阴影/模糊），采样的是**客户端的表面
缓冲**。这一步以前为了「拿到的贴图能 `destroy`」而绕开 wlrroots 给客户端缓冲包的那层
（`wlr_client_buffer`）、对 `client->source` 再建一张 —— 对 **dmabuf 客户端**（Qt/KDE
那类：每帧重画同一个 dmabuf、只 `commit` 不换缓冲）那张贴图**一直是第一次导入的那版
内容**：客户端在画、装饰缓冲也在重烘，里面却永远是旧画面，屏幕上就是「窗口停在最后一
帧」（kate / dolphin / konsole 都这样；拖到另一块屏会重新烘一次背衬，画面才跟上）。
GTK 在我这儿走 shm，所以看不出问题 —— 用户报的正是「只有 KDE 系」。

现在统一走 `client_texture()`（`src/fx.c`）：**优先借 `wlr_client_buffer->texture`**
（wlroots 画场景用的就是它，跟着客户端每次 attach 走），借来的按 `owned = false` 处理、
**不 destroy**；借不到才自己建一张（那时 `owned = true`）。装饰这条路（窗口 + layer 的
背衬抓取）和 jelly 那几条都按同一套规矩来。

回归判据：`./test-dmabuf-update.sh`（拿 kate 当 dmabuf 客户端，打字前后比屏幕像素：
修之前 0，修之后 ~949；同时比装饰缓冲 dump 的前后两帧）。

### 装饰按「可见窗口」画，不按表面（niri 的 `size()` / `buf_loc()`）

客户端可以用 `xdg_surface.set_window_geometry` 说「表面里哪一块才是窗口」。自己画装饰的
客户端（GTK / kitty）会把表面做大一圈来画自己的阴影：实测 GTK3 报
`表面 680x755 / 窗口 634x708 / 留白偏移 23,21`。装饰矩形以前取的是**整块表面**，于是边框
画在表面边上、真正画出来的窗口往里缩一圈，中间那圈露的是屏幕背景 —— 「边框和窗口之间有缝」。

现在照 niri 的做法（那边 `LayoutElement::size()` = `window.geometry().size`、
`buf_loc()` = `-geometry().loc`）：

| 谁 | 按什么算 |
| --- | --- |
| 布局（列宽、高度、`place`） | 窗口几何（本来就是这样） |
| 装饰矩形（圆角 / 边框 / 阴影 / 离屏缓冲尺寸） | 窗口几何 |
| 内容贴图采样 | 按 `(geometry.x, geometry.y)` 偏移 —— 归一化后交给着色器（`u_tex_map`） |
| `ext-background-effect` 的模糊区域 | 减掉留白偏移（客户端给的是表面局部坐标） |
| 关闭动画的快照 | 同一份几何：快照节点按 `-偏移` 摆，装饰矩形还是窗口 |
| 关闭动画「怎么没有」 | 三条路都能让它不出现，`VIDOW_LOG=debug` 下会明说是哪条：`关闭动画跳过 <窗口>：弹出物（菜单 / 提示 / 下拉框）不做关闭动画` / `手里没有最后一帧快照` / `window_close_ms = 0（配置里关了）`。窗口是不是被当成了弹出物，`vidowmsg windows` 里查 `popup` / `float_mode` |

留白那圈（客户端自己的阴影）落在装饰矩形**外面**，vidow 不画它（内容被圆角矩形裁掉）。
没留白的客户端（X11、不画装饰的 Wayland 客户端、`prefer_no_csd` 打开之后）留白是 0，
这条路径逐像素等于以前。

- 顺带修掉一个时机坑：装饰节点的位置原来是从场景坐标里算的（`sx - vx`），而 wlroots 要等
  客户端 commit 才把表面子节点挪到 `-geometry`。map 那一瞬间算出来是 0，于是第一帧的装饰
  摆在视图原点而不是表面原点 —— **录屏里看到的那条缝就是这个**（grim 截图是之后重绘的，
  所以同一时刻看不到）。现在定位只依赖 `xdg_surface.geometry`，不看场景坐标。
- 量法：`place` 给出的窗口矩形是 `6,6 634x708`，连拍 12 张 grim，从窗口出现的**第一张**
  起都是 `x0..4 壁纸 | x6..7 边框 | x8.. 内容`（`VIDOW_FX_DUMP` 里装饰缓冲 642x716 =
  (634+708) + 2×4 padding ✓ 按窗口算）。

### `prefer_no_csd`：让客户端别画 CSD

默认 `false`。打开后 vidow 明确告诉客户端「装饰由合成器画」：`zxdg_decoration_manager_v1`
回 `SERVER_SIDE`，并且新建 `org_kde_kwin_server_decoration_manager`（默认模式 `Server`）。

为什么非要建那个老协议：**GTK3 只认它** —— libgdk-3 里根本没有
`zxdg_decoration_manager_v1`，只有 `org_kde_kwin_server_decoration_manager` 和那句
"Compositor prefers decoration mode"。没有它，GTK3 永远自己画 CSD、带一圈 20px 阴影留白：
默认配置下实测 `表面 680x755 / 窗口 634x708`；`prefer_no_csd = true` 之后同一个窗口报
`表面 634x708 / 偏移 0,0`（CSD 整个收回去了）。wlroots 头文件里写着这个协议 obsolete，
但客户端还在用，所以照给（建不出来只报一句，别的客户端不受影响）。

代价：GTK3 窗口连**标题栏一起**收回去，只剩 vidow 的边框（半屏 / 关闭走快捷键）。这个键
只对**之后**才连上的客户端彻底生效 —— KDE 老协议的默认模式是客户端绑定的时候发一次的
事件，已经在跑的窗口要重启客户端，和 niri 的 `prefer-no-csd` 一样。

## 全屏直通（direct scanout）

全屏窗口只要**不需要装饰**，vidow 就不烘那趟离屏渲染，把它整块交给场景 —— 场景里只剩
客户端表面这一个节点，wlroots 才有机会把客户端的缓冲**直接扫上屏**（`wlr_scene.c` 的
`scene_entry_try_direct_scanout` 要求这一帧的 render list 正好一项、且它是盖住整块输出的
buffer）。省掉的是每帧「一次整屏离屏渲染 + 一次输出合成」：全屏看视频 / 打游戏是白拿的
帧率，4K 全屏还少一趟 GPU 带宽。

判据在 `src/fx.c` 的 `vidow_view_scanout_ok`，**宁严不宽** —— 漏判只是没省下那两笔，
误判会让画面变样：

| 条件 | 为什么 |
| --- | --- |
| 全屏、已 map、不是弹出物、没被 park、不在 overview 里 | 只有全屏铺满整块输出（wlroots 要求节点的框正好等于输出框）；overview 里窗口是缩小的，那笔缩放在合成那条路上 |
| 表面节点有缓冲，且**整块不透明** | 全屏窗口盖住整块屏，只有内容真的不透明，底衬 / 模糊才一个像素都透不出来。两种「不透明」：格式里没 alpha（XRGB / RGBX，播放器和游戏的常规选择），或者客户端声明了盖住整块表面的 opaque region（GTK 那套）。后者 wlroots 判「这个节点盖住了下面的东西」用的也是它（`scene_node_opaque_region`）—— 它不认，直通也轮不到 |
| 装饰参数全零、`opacity = 1` | 全屏窗口本来就不画 SSD（`vidow_view_look` 把边框 / 圆角 / 阴影 / 噪点归零），但规则里写了 opacity / 圆角就得走合成 |
| 不在动画 / 果冻 / 遮罩里，`anim_alpha == 1`、`anim_scale == 1` | 那些帧画出来的不是「缓冲自己」 |
| 没有自定义 shader、没有越界裁剪 | shader 会改像素；裁剪只做在合成那条路上（直通会把越界那半块画到隔壁屏上）|
| CSD 留白 = 0（`off_x / off_y == 0`）、窗口框 == 表面框 | 客户端自己画的阴影留白在合成那条路上被裁掉，直通扫的是整块缓冲 |
| 缓冲的设备像素 == 这块输出的设备像素 | 客户端没吃下输出缩放（1x 客户端放在 2x 屏上）时，合成那条路是**着色器**放大、直通那条路是**场景**自己放大，两条路的采样未必一样 |

**切换看不出变化**（用户要求：「回退的时候屏幕不要黑一下」）。`view.c` 的
`view_scanout_switch` 在重烘之前问一次判据，只在翻转那一帧做事：

* 进直通：先把真实表面露出来（内容一样，装饰节点这一帧还盖在上面），同一帧把装饰节点收掉；
* 退直通：先把装饰那条路接回来，下面照常烘一帧，**烘成了才**把表面藏起来（`view_anim_bake`
  尾那句 `set_opacity`）。

判据成立时「合成出来的像素」和「客户端缓冲本身」是同一个东西，所以两个方向都没有中间帧
是空的。量过：`./test-scanout.sh` 拿同一个全屏窗口跑两遍（默认 / `VIDOW_NO_SCANOUT=1`），
grim 两张图**内圈逐像素一样**；差的只有最外 1px（3996 个像素、最大 37/255）—— 装饰着色器
的边缘抗锯齿（`radius = 0` 时 `smoothstep(-1, 1, d)` 让最外那圈落在 0.84 上），全屏窗口
贴到屏幕边，这圈就落在屏幕最外沿。两个方向都是静态的 1px，不是闪一下。

- 切换那一帧的损伤是**整块输出**：改透明度、开关节点都会让场景节点自己报出整块，实测
  切换帧的 `scene_output->damage_ring` 正好是 `0,0..1280,720`（整屏），两个方向都是 ——
  所以不会留下上一帧的残影（那是「跳一下」的来源）。
- 真 GPU 上一样：`WLR_RENDERER=gles2 ./test-scanout.sh`（EGL 1.5 / radeonsi）11 项全过，
  像素差的还是那 3996 个边缘像素（最大 37/255）—— 和 pixman 一个数，说明那圈差是着色器
  边缘抗锯齿、不是渲染器差异。
- 真机自查（你自己的 tty）：`VIDOW_LOG=debug ./run.sh` → 全屏一个视频 → 日志里
  `grep -a "Direct scan-out"` 应该看到 `enabled`（退全屏 `disabled`），并且 `scanout:`
  两行就在它前面。
- 日志（`VIDOW_LOG=debug`）：`scanout: <窗口> 交给场景直画（全屏 + 装饰全零）` /
  `scanout: <窗口> 回到合成（装饰 / 模糊 / 动画又要了）`，紧跟着 wlroots 自己那句
  `Direct scan-out enabled` / `disabled`（`types/scene/wlr_scene.c`）。
- `VIDOW_NO_SCANOUT=1`：一律走合成那条路（探针 / 排查用）。
- 已知边界：**全屏窗口上面还有可见的东西**（状态栏那类 layer surface、通知）时 render list
  就不止一项，wlroots 不直通 —— vidow 这边仍是「不烘装饰」的状态（画出来的像素和直通一样），
  只是省不下那两笔。niri / Hyprland 同样如此（「全屏时自动藏状态栏」是个独立功能，vidow 没做）。

## Kawase 背景模糊

半透明窗口后面是真的模糊过的场景，不是假的半透明叠加。流程：

1. 把场景渲染进一张输出大小的离屏缓冲
2. 降采样链：1/2 → 1/4 → 1/8 → …，每级 5 抽样（中心 4 份 + 四个斜角各 1 份，除以 8）。
   抽样半径是 `blur_offset` 个**源** texel —— 源每降一级尺寸减半，所以同一个 offset
   换算回原始像素**每级翻倍**：半径就是这么跟着降采样扩展的
3. 升采样链：每级 12 抽样（轴线 ±2 半步各 1 份、斜角 ±1 半步各 2 份，除以 12），
   半步 = **源 texel 的一半**（跟 niri 的 blur_up 一致）。写 `up[0]` 的那一趟也是同一个
   着色器，链上不做任何透明度剔除（见下）
4. 装饰着色器按输出坐标采样最终结果，只画在窗口圆角矩形**内部**，其余地方让真实场景透出来

这一版的降采样 / 升采样抽样形状与步长照搬 niri 的 `blur_down` / `blur_up`（它那边是
`blur.offset` + `blur.passes`）：级数每加一级，半径（原始像素）翻倍。

### 两种 backdrop，各算一次

| 用谁 | 捕获时藏掉 | 谁在用 |
| --- | --- | --- |
| **共享**（壁纸层） | 所有窗口 | 平铺窗口 + 不与任何窗口重叠的浮动窗口 |
| **私有**（每窗一份） | 只有它自己 | 与别的窗口重叠的浮动窗口 |

平铺窗口互不重叠，所以它们的「背后」是同一个东西 —— 整屏只做**一次**捕获、**一次** kawase
链，所有平铺窗口共用结果（`blur_all = true` 因此几乎没有额外开销）。只有真的叠在别人身上的
浮动窗口才需要单独捕获，因为它的背后必须保留被它压住的那个窗口。

`blur_xray = true` 把第二行也并进第一行：重叠时照样用整屏那份（**忽略别的窗口**）—— 观感上
「糊的是壁纸，不是压着的那个窗口」（Hyprland 的 `blur:xray` 同款），换来的是**一块私有底衬
都不建**。实测同一场景（一块纯红浮窗 + 压在它上面的半透明白浮窗）：`chain` 累计 **48 → 3**、
`own` **45 → 0**，白窗那块区域的 `mean(R−B)` 从 **+148.5** 变成 **−37.6**（红色不再渗进模糊）。
默认关 —— 真背景模糊才是这个合成器的卖点；弱 GPU / 4K 高刷想再压一压就打开它。

### 背景不动就不重算

`gen_layer` / `gen_scene` 两个世代计数分别记录「壁纸+layer 变了」和「窗口也变了」。缓存命中时
整条链一步都不跑：

- 静止画面：装饰缓冲**一次都不重绘**（实测静置 4s，重绘增量 = 0）
- 窗口移动/滚动/客户端重绘：只作废私有的那份，共享的那份仍然有效
- 壁纸换图：只重算共享的那份

私有 backdrop 捕获时会把该窗口藏起来，所以它自己的位置不影响结果 —— 缓存键里**不含**该窗口的
几何，拖动浮动窗口不会反复重算它自己那一份。

**透明度剔除只干一件事：约束模糊的范围（哪儿该糊）**。它按**元素自己**的 alpha 剔，底衬
完全不剔。

元素自己透明的地方不糊 —— 这是治「一圈透明模糊」的：模糊是垫在元素节点下面的一整块
矩形，没有这一层的话，面板留的透明内边距、圆角外面、弹窗 / 窗口自带的那圈半透明阴影边
也会透出糊来（实测：面板留 40px 全透明内边距时，背后亮点的峰值 **1.000 → 0.322**）。

实现在装饰着色器里（`decor_fragment_src`），跟 niri 的 `blur_finish` 一样是**硬阈值**
`step(u_mask_min, texture2D(u_mask, uv).a)`：

- 窗口 / 弹窗那条路：内容贴图就是元素自己（`u_texture`），直接取它的 alpha
- layer surface 的模糊底衬那条路：那条路不画内容（`u_texture` 是空贴图），调用方把
  layer surface 自己的缓冲当 mask 递进来（`u_mask`；层表面的 geom 就等于 surface 尺寸，
  所以 uv 跟内容那趟完全一致）
- 两条路用的都是**内容自己的 alpha、不含 `window_opacity`** —— 剔除管的是「形状」，
  半透明窗口该透出来的模糊不能因为 `window_opacity` 调低就整个没了
- 客户端用 ext-background-effect 要了模糊区域时**不剔**（那块区域就是它画出来的边界，
  niri 的 `need_ignore_alpha = !blur_region.is_some_and(|r| !r.is_empty())` 同一个判断）；
  拿不到 mask（surface 还没 commit 过）时也不剔（`mask_min = 0` ⇒ `step(0, a) ≡ 1`），
  退回老行为而不是把模糊整块剔没

端上实测（`./test-layermask.sh`：一块留 40px 透明内边距的 top 层面板 + 两个亮点，一个
落在内边距里、一个落在面板 50% 那块底下）：

| | 内边距里的亮点 | 面板块底下的亮点 |
| --- | --- | --- |
| `blur_alpha_min = 0.15` | **1.000**（原样，不糊） | 0.167（块 alpha 0.5 > 0.15，照糊） |
| `blur_alpha_min = 0`（不剔） | 0.322（被糊平） | 0.167 |

局限：mask 取的是 surface 的**主**缓冲，不含子表面 —— 一个用子表面画内容的层，那部分
会被剔掉（要精确得像 niri 那样把整棵子树离屏渲一张 alpha 纹理）。输入法候选框
（`layer_im`）和弹出物（xdg_popup / X11 菜单）现在也走同一套模糊（见 fx.c 的
`popup_blur_frame`），所以那边同样按主缓冲 alpha 剔除。

**底衬不做剔除**（曾经有，撤了）：原来在最后一趟上升里按底衬的 alpha 做过一次
`step` 遮罩（那个程序叫 `kawase_final`），现在整个撤掉 —— 剔除是用来约束**范围**的，
不是用来改糊出来的像素；底衬本身就是预乘的，它透明的地方 alpha 跟着一起淡出，合成的
时候自然透出真实背景。`kawase_final` 这个程序随之消失，最后一趟就是同一个 `kawase_up`。

`blur_alpha_min` 只进装饰着色器、不进模糊金字塔：改它由 `blur_mask_min_changed()`
（`src/fx.c`）把 `blur_ver` 顶一下，帧循环自然重画 —— 不然运行时改配置会看着「没反应」。

配置：`blur_enabled` / `blur_all` / `blur_xray` / `blur_levels`(2..6) / `blur_strength` /
`blur_offset` / `blur_alpha_min`。默认 `blur_all = true`（平铺窗口也模糊）、
`blur_xray = false`（重叠时各自算私有底衬 = 真背景模糊）、`blur_levels = 3`（≈29 设备像素
半径；4 级 ≈65px 更糊，但每个模糊元素每帧多两趟离屏渲染）。

模糊结果按帧缓存，但**只要背景变了就立刻重算**，所以视频、滚动、拖动背后都是实时模糊。

### 看不见的模糊不算

装饰着色器最后两句是 `out_px = content + out_px * (1 - content.a)`，模糊那一项还乘着
`inside` 和 `step(u_mask_min, elem_a)`：**内容不透明的地方（`content.a == 1`）模糊被乘成
0，内容全透明的地方又被剔除**。所以模糊只在**内容半透明**的窗口上看得见 —— 终端背景
0.8、播放器控制条、半透明面板那些才是真的要看模糊的。

内容整块不透明的窗口（终端、浏览器、大多数 GTK / X11 窗口：格式里没 alpha 的 XRGB /
RGBX，或者声明了盖住整块表面的 opaque region）vidow 现在**不给它算模糊**：整屏 kawase
金字塔、私有底衬抓取、「底衬版本变了就重烘这个窗口」全省。以前这笔账是白算的 —— 一个
不透明的静态窗口压在一个每帧都在变的窗口上面，它每帧都要重抓一遍私有底衬 + 跑一遍
kawase + 重烘整块，而画出来的像素和没有模糊时一模一样（只差最外 1px 那圈边缘抗锯齿）。

判据在 `src/fx.c` 的 `view_blur_visible`，**保守**：拿不准就当看得见。

- 判成「看不见」只有两种确定情形：`wlr_buffer_is_opaque()`（XRGB / RGBX），或者客户端的
  opaque region 盖住了可见窗口那一块 —— 两者都保证窗口里每个像素 alpha == 1
- 其余一律照旧算模糊：果冻 / 抓图遮挡 / 自定义 shader / 开合动画 / `window_opacity < 1` /
  `anim_alpha < 1`，以及**只声明了部分 opaque region** 的（圆角窗口的四个角、CSD 阴影
  留白 —— 那几处其实也是全透明、模糊照样被剔掉，但不读像素没法确认，不值得为它冒险）

实测（`./test-blurinvisible.sh`：headless 1280x720@60，`blur_all = true`，
`VIDOW_NO_SCANOUT=1` 逼它走合成那条路；一个每帧换色的窗口当底衬，上面压一个静态窗口）：

| 上面那个窗口 | 重烘次数/帧 | 私有底衬 own | kawase 链条 | 渲染趟数 |
| --- | --- | --- | --- | --- |
| 不透明（改之前） | 2.00 | 372 | 372 | 3348 |
| 不透明（改之后） | **1.00** | **0** | **0** | **372** |
| 半透明（照旧） | 2.00 | 372 | 372 | 3348 |

「重烘 1.00 次/帧」= 只剩底下那个每帧在变的窗口自己在烘（那是该干的活）。半透明那两行
是守门的：判据没把该模糊的一起砍掉。

layer surface（状态栏 / 面板）走同一条判据，实现在 `layer_blur_visible`：整块不透明的
状态栏底下垫的那层模糊也是一个像素都透不出来，于是它那块私有底衬的抓取、kawase 链条、
重烘全省。实测（`./test-layerblur.sh` 最后一节：一个每帧换色的窗口压在状态栏那块区域里，
headless 1280x720@60，`blur_levels = 3`，5s 稳定窗口）：

| 状态栏 | layer region 重抓 | kawase 链条 |
| --- | --- | --- |
| 半透明（alpha 0.5） | 304 | 279 |
| 整块不透明（alpha 1.0） | **0** | **0** |

注意这条判据只能看**客户端自己声明的**不透明：GTK 拿 RGBA visual +
`set_app_paintable(True)` 画出来的状态栏（`tools/bar.py` 的默认样子），哪怕内容一个像素
都不透明，缓冲区格式照样带 alpha、也没声明 opaque region —— 这种 vidow 照旧给它算模糊
（保守，宁可白算）。真客户端里不透明的状态栏该用不透明格式（不拿 RGBA visual），或者自己
`wl_surface.set_opaque_region` 声明一块盖满的 opaque region。

### `blur_levels` 是降采样次数

是。`blur_levels = n` 就是「降采样 n 次」：链上第 i 级的分辨率是 `输出 >> (i+1)`，
所以 4 级 = 1/2、1/4、1/8、1/16，最终结果是**半分辨率**（`up[0]`，给装饰着色器采样的
那张）。级数越大模糊半径越大、也越贵（每级一趟 pass）。**默认 3 级**（≈29 设备像素半径）；
运行时夹在 2..6（`MAX_BLUR_LEVELS`，在 `server.h` 里，数组尺寸也用它），并且最小那一级
会小于 4px 时自动降一级（`blur_levels_for`）。

**半径**（按下表的抽样数学在 numpy 里跑一个 delta 响应量 σ；`blur_offset = 2`）：

| `blur_levels` | 等效高斯 σ（原始像素） |
| --- | --- |
| 2 | ≈3.3 |
| 3 | ≈6.9 |
| 4 | ≈14 |
| 5 | ≈28 |
| 6 | ≈56 |

4K / 高 DPI 屏上底衬是**设备像素**，所以同样的级数在逻辑像素里看着更弱（niri 也是
物理像素）。想要 niri 那种大糊就往上加级数。

端上实测看 `./test-blurlevel.sh`：`tools/wall.py` 壁纸的硬边，2 / 4 / 6 级的最大梯度
0.127 / 0.027 / 0.0039（老代码 6 级被夹成 4 级，最后两档一模一样：0.0458）。

### layer surface 背后的模糊

状态栏 / 面板（bottom / top / overlay 层的 layer surface）也有背景模糊 —— 客户端是
半透明的（waybar 的 `rgba(...)`、大部分面板都是）就直接透出来。

做法和窗口不同：窗口是把内容和装饰一起画进一张离屏图再让场景节点画那张图；layer
surface 的内容渲染归 wlroots 的 scene 管（subsurface 树、buffer scale、damage 都在
它手里），接管过来等于重写一遍。所以在它**下面垫一个节点**就够 —— 一块和它一样大的
场景节点，用同一个装饰着色器画，但把内容 / 阴影 / 边框 / 圆角 / 噪点全关掉
（`fx->blur_only`），只留模糊。半透明的地方透出来，不透明的地方盖住，等于没画。

- 抓的是**这个 layer 盖住的东西**（跟 niri 一致：它就地抓 framebuffer 里已经画好的
  那部分）：
  - `bottom` 层的 surface 先于窗口画，它底下只有壁纸和 background 层 —— 正好就是共享
    的那张底衬，直接拿来用，一次抓取都不用
  - `top` / `overlay` 层反过来：**窗口就在它底下，模糊里必须有窗口**（压在窗口上的
    半透明面板透出来的是窗口的模糊）。这时按它自己那一小块区域重抓一份：基座是从共享
    底衬上 1:1 拷过来的那块（壁纸 + background / bottom 层），窗口连它的弹出物一起
    再画上去。代价跟**区域面积**成正比而不是整屏
  - 缓存键 = 区域 + `gen_scene` / `gen_layer` + 级数 + `blur_offset`（剔除阈值不进金字塔，
    它只影响装饰着色器，由 `blur_mask_min_changed()` 单独顶 `blur_ver`）：
    屏幕不动一次都不重抓（实测空转 3 秒 0 次），窗口每出一帧重抓一次（实测 3 秒 173
    帧 → 173 次，区域是一块 400x200 的面板那么大）
  - `overview` 里 `top` / `overlay` 那几棵树整体关着、这层模糊没人看得见，直接跳过
- `background` 层（壁纸自己）跳过：模糊的底衬里就有它，垫一层等于画重
- 抓**共享底衬**（壁纸那份）的时候这些节点要先藏起来（bottom 层的那个就在被抓的场景里，
  不藏就是「模糊结果里混进上一帧的模糊」）
- 同理，**会被模糊的那个 surface 本身**也要从共享底衬里藏掉：bottom 层的面板（dock 这类）
  自己就在被抓的那几棵树里，它要的那份底衬要是含它自己，模糊里就会浮出一个自己的影子
  （半透明的地方透出「自己的糊版」）。实测 bottom 层 50% 红面板（`tools/bpanel.py`）在
  底衬 dump 里占 100% 的红像素 → 修后 0%，见 `./test-layerghost.sh`（区域抓取那条路不
  用藏：它只画共享底衬 + 窗口，layer 自己本来就不在里面）
- 位置 / 尺寸变了要作废重画（采样坐标在着色器里，节点动了就得重算），见
  `vidow_layer_blur_invalidate()`。区域抓取那张贴图只盖住这一小块，所以采样坐标要减掉
  区域原点（`fx->blur_tex_x/y/w/h`）—— 不减就是整块错位

实测（`tools/wall.py` 壁纸 + `tools/bar.py`：一块 50% 透明的 top 层状态栏，量状态栏
那 40px 里的最大梯度；壁纸本身的硬边是 107.7）：

| | 状态栏内最大梯度 |
| --- | --- |
| `blur_enabled = false` | 74.0（透出来的是硬边，只是被 50% 黑压暗了） |
| `blur_levels = 2` | 5.33 |
| `blur_levels = 4` | 2.67 |

`./test-layerblur.sh` 就是这个靶子（4 项断言）。`./test-layerwin.sh`（4 项）专门盯
「面板底下的模糊里有没有窗口」：`tools/bluewin.py` 一个纯蓝窗口 + `tools/panel.py` 一块
top 层面板压在它上面 —— 面板那块截屏的 B-R 均值 0.445（老代码 0.027，那是壁纸的灰）。

### 顺手修掉的真 bug（模糊）

**⓪ 改 `blur_offset` / `blur_alpha_min` 看着「没反应」**：这两个键是在 kawase 的
着色器里当 uniform 用的，缓存的贴图不会跟着变 —— 而共享金字塔的缓存键里
**只有 `levels`**，没有它们，于是运行时（`set-config` 或改配置重载）改这两个键，
只要背景没别的变化，用旧参数算的那张图就一直被复用。缓存键现在把 `levels` /
`offset` / `alpha_min` 都算上了（窗口自己那份私有金字塔同理）。

**① 升采样每级把亮度乘一遍（过曝）**：`kawase_up` 原来是

```glsl
gl_FragColor = sum / 12.0 + texture2D(u_add, v_uv);
```

8 个抽样的权重是 1+2+1+2+1+2+1+2 = 12，`sum/12` 本身没错，但**加进来的 `u_add`
没有参与平均** —— 常量输入下结果是 `c + c = 2c`。半透明窗口后面那层底整整亮一倍，
看着就是「过曝、颜色不对」。实测（headless 1280×720 + `tools/wall.py` 壁纸，
`VIDOW_BLUR_DUMP` 同时导出 backdrop 与最终模糊图，比平均色）：

| | backdrop 平均 RGB | 模糊图平均 RGB | 亮度比 |
| --- | --- | --- | --- |
| 修前 | (82.4, 74.7, 88.9) | (148.9, 141.8, 177.9) | **1.905** |
| 修后 | (82.4, 74.7, 88.9) | (81.8, 74.3, 88.8) | **0.995** |

改成 `gl_FragColor = (sum + texture2D(u_add, v_uv)) / 13.0;` —— 12 份抽样 + 1 份
`u_add` 一起除以 13，模糊核的和才是 1（模糊必须保亮度，否则每级都在加亮）。

**② 升采样链的目标尺寸写反了**：`up[i]` 原来按 `w >> (levels-1-i)` 分配，于是 `up[0]`
是最小的、`up[levels-2]` 才是半分辨率；而循环从 `i = levels-2` 往下走 —— 真正被采样的
那张（`up[levels-2]`）是**第一个**算出来的，后面两级纯属白算，`u_texel` 也跟着对不上
（i=1 那一步拿 w/2 的图、按 w/8 的 texel 抽样）。现象是模糊「糊成一团」：结果只由最糊
的那两级拼出来，细一级的降采样全丢了。改成 `up[i]` 与 `down[i]` 同尺寸（`w >> (i+1)`）、
最终取 `up[0]`：这样每一步的 `u_add` 正好和目标是同一分辨率（dual filter 的本意），
四级的结果才真的逐级叠上去。

**③ 底衬捕获漏了输出倍率（高 DPI 上整个模糊是坏的）**：场景图里的一切（节点坐标、
`dst_width`）都是**逻辑像素**，而底衬缓冲是**设备像素** —— `backdrop_iter()` 直接把
逻辑坐标当设备坐标用，于是 1.7 / 2 倍屏上底衬只在左上角画了一块**缩小**的场景，
其余全是透明（2 倍屏实测 alpha 均值 0.25，1.7 倍屏 0.345 ≈ 1/1.7²）。这一条同时
解释了三件看起来无关的事：模糊采到的是空像素、级数越多越透明（「越糊越没模糊感」）；
layer 背后的模糊采样位置对不上、整块错位；以及「`blur_levels` 怎么调都没区别」。
1 倍屏上完全看不出来，所以一直没暴露。修法：按输出倍率放大、再减掉这块屏自己的
布局原点（按**边**取整，避免相邻 buffer 之间露缝）。实测 2 倍屏底衬 alpha 0.25 → 1.00、
layer 背后的亮斑 σ 16.0 → 23.5、对位偏差 ≤ 0.6 像素（`./test-blurscale.sh` 在
1 / 1.7 / 2 三档倍率上都有断言，用老二进制跑会挂 6 条）。

**④⑤⑨ 这一簇都是「按底衬 alpha 剔」的坑 —— 那个特性整个撤了**（见 115：剔除只该约束
范围，不该改糊出来的像素），留着当记录。

**④ 透明度剔除是一刀切**：`step()` 在阈值等值线上留一道硬边，低于阈值的整块背景直接
消失（用一个 alpha 斜坡背景层实测逐像素跳变 0.102）—— 这条后来被 ⑤ 换成软阈值。
但软阈值这条路后来又被撤了：它的**有效阈值掉到了 α_min/2**，见 ⑨。

**⑤ 透明度剔除撒在每一趟抽样里（越糊越透明）**：剔除原来写在 `tap()` 里 —— 每一趟的
每个抽样都乘一次，逐趟相乘等于按趟数取幂。实测（均匀 alpha 0.45 的底衬、
`blur_alpha_min = 0.5`）过完整条链 alpha 只剩 **0.0000**、颜色也一起没了；级数越多掉得
越狠，看起来就是「级数调大了半径也没变大，只是更淡了」。改成只在 `kawase_final` 做一次
遮罩。当时新增的 `./test-blurcull.sh`（拿老二进制跑，这条判据挂）已随底衬剔除一起删了。

**⑥ 会被模糊的 layer surface 自己进了自己的底衬**：底衬抓的是 `background` / `bottom` /
窗口三棵树，而 `bottom` 层的面板自己也要模糊 —— 那份底衬里就含它自己，于是模糊里浮出
一个自己的影子。实测（`tools/bpanel.py`：bottom 层 50% 红面板）底衬 dump 里面板那块
**100% 是红的** → 修后 0%。新增 `./test-layerghost.sh`。

**⑦ 半径每级只涨 1.5 倍（级数一多就饱和）**：升采样每级还加回一份同级降采样副本
（dual filter 的 `add`），那份是**没这么糊**的图，等于每级掺回一点锐度：numpy 里量的
σ 是 5.6 / 8.3 / 12.8 / 18.6 / 26.1（2..6 级，每级 ~1.5 倍），niri 的链是**每级翻倍**。
再加上 `MAX_BLUR_LEVELS = 4` 封顶，「级数调大」这件事在 4 级到头、而且每级只涨一点 ——
看着就是「半径不跟着降采样扩展」。现在照 niri 重写（去掉 `add`、升采样半步、除以 12、
上限放到 6）：σ = 3.3 / 6.9 / 14 / 28 / 56，端上硬边最大梯度 0.127 / 0.027 / 0.0039
（2 / 4 / 6 级）。新增 `./test-blurlevel.sh`（拿老二进制跑挂 3 条）。

**⑨ `blur_alpha_min` 看着「没用」（软阈值的有效阈值在 α_min/2）**：⑤ 把它挪到最后一趟
的时候顺手改成了软过渡 `smoothstep(α_min/2, α_min, a)`（当时是为了不在等值线上留硬边）。
问题是**低于 α_min 的底衬照样被糊**，只是压一点：设 0.5 时 alpha 0.45 的底衬还剩 90%
（实测 0.4510 → **0.4039**）。要它真的「低于阈值就不糊」就得是硬阈值 —— 现在跟 niri 的
`blur_finish` 一样是 `step(u_alpha_min, a)`：0.45 的底衬 + 阈值 0.5 → **0.0000**（颜色
一起归零，不留半透明黑），0.9 的底衬 + 阈值 0.5 → 0.9020 一点不衰减。
（当时的三条判据量这三件事，拿软过渡那版二进制跑挂 2 条；那个测试已随底衬剔除一起删了。）

**⑩ 模糊会漏进 layer surface 自己透明的部分**：模糊是垫在 layer surface 节点下面的**一整
块矩形**，所以面板留透明内边距 / 圆角外面那圈也会透出模糊 —— 本该原样显示背景。实测
（`tools/padpanel.py`：留 40px 全透明内边距的 top 层面板 + `tools/dot.py` 在 (20,150)
放一个亮点）：没面板时亮点峰值 **1.000**，有面板时同一处只剩 **0.285**（被糊平了）。
niri 的 `ignore-alpha` 就是治这个的：`blur_finish` 拿**元素自己**渲染出来的 alpha 纹理做
`step(ignore_alpha, alpha)`，元素自己透明的地方不糊。vidow 原来没有这条路（只按底衬的
alpha 剔），**已修**：装饰着色器多了一个 mask 采样器（`u_mask`，见上面透明度剔除那节），
layer 的模糊底衬把 layer surface 自己的缓冲当 mask 传进去，窗口 / 弹窗那条路直接用内容
贴图的 alpha。修后同一处的峰值 **0.322 → 1.000**（原样显示背景），面板 50% 那块底下照糊
（0.167）。新增 `./test-layermask.sh`（3 项判据，拿老二进制跑挂 1 条）。

**⑧ layer 的模糊底下没有窗口**：原来所有 layer surface 都拿共享底衬（窗口被藏起来），
于是压在窗口上的半透明面板透出来的是**壁纸**的模糊，不是窗口的模糊。niri 不是这么做的：
它就地在 framebuffer 里抓「已经画在它底下」的那部分，对 `top` / `overlay` 层来说里面
**有窗口**。现在照这个语义来：`top` / `overlay` 层按自己那块区域重抓一份（基座 = 共享
底衬里对应的那块 + 窗口），`bottom` 层保持不变（它先于窗口画，底下本来就没窗口）。
实测（`tools/bluewin.py` 纯蓝窗口 + `tools/panel.py` 一块 top 层面板压在它上面）：面板那块
截屏的 B-R 均值 **0.027 → 0.445**，面板底下的区域抓取里 85.6% 是窗口的蓝。新增
`./test-layerwin.sh`（拿老二进制跑挂 3 条）。缓存按区域 + 世界代次：空转 3 秒 0 次重抓。

## 动画与自定义 shader

所有动画的进度都来自 `CLOCK_MONOTONIC`，**不是帧计数**：系统卡顿、掉帧、GPU 慢，
只会让动画少几帧，绝不会把它拉长。实测（headless 后端 62Hz）：

| 动画 | 配置时长 | 实测墙钟 | 帧数 |
| --- | --- | --- | --- |
| 打开 | 180ms | 185.9ms | 11 |
| 位移 | 190ms | 201.9ms | 12 |
| layer 出现 | 200ms | 201.8ms | 12 |
| 关闭 | 140ms | 146.4ms | 10 |

把合成器人为冻住（每 130ms 里 `SIGSTOP` 70ms），同一个 180ms 打开动画变成
**216ms / 6 帧** —— 帧数掉了一半，时长没有翻倍，这就是「时长不受运行速度影响」。

### 开窗动画为什么曾经「闪一下再弹一下」

`anim_off_x/y` 这一组偏移**同时**被入场动画和位移补间写。新窗口 map 的同一帧里
`vidow_arrange` 会跑好几遍（建列、抢焦点、视口滚动各一次）：

1. 第一次摆放：`placed == false` → 只记位置，不起补间
2. 第二次摆放：列偏移变了（646 → 640）→ 满足 `moved`，而**这时入场动画还没开始**
   （`vidow_view_open_anim` 在 map 处理函数里排在摆放之后）→ 位移补间起来了
3. 入场动画开始：设 `anim_alpha = 0`、`anim_off_y = open_slide_px`，但没管已经起步的
   位移补间
4. 两个动画每帧互相覆盖：位移补间把 `off_y` 抹成 0（上滑没了），入场动画又把 `off_x`
   留在补间的位置 → 窗口在「淡入上滑」和「横移到位」两个位置之间来回跳

修法：入场动画开始时**撤掉位移补间**并接管 `anim_off_x/y`（`vidow_view_open_anim`
里 `vidow_anim_cancel(anim_move)` + 清零），`vidow_place_view` 在入场动画进行期间只
更新 `geom`，不再开补间、也不碰 offset。

实测（`vidow B` 嵌在 headless 的 `vidow A` 里跑：B 的输出有真实的 frame callback 节奏，
和用户那套嵌套一样；用临时 trace 打每帧的 t/alpha/off_y/落点）：

| | 同一帧里的两次更新 | 渲染出来的落点 |
| --- | --- | --- |
| 修前 | `off_y=22` 与 `off_y=0` 交替 | `640,28` / `646,6` 来回跳 |
| 修后 | 只剩入场动画的 `off_y` | `640,28 → 640,24 → … → 640,6` 单调上滑 |

（trace 里另一条 `move update` 是隔壁 w1 的：视口滚了 6px，它本来就该平滑跟过去。）

### 配置：一个入口 + 一类一份

配置是 Lua 5.4。入口是 `~/.config/vidow/config.lua`，它通常只有 `vidow.include()` 行，
把 `configs/` 下的各类读进来：

```
~/.config/vidow/
├── config.lua                   入口：只有 include 行
└── configs/
    ├── 01-gaps.lua              间距
    ├── 02-decor.lua             圆角 / 边框 / 阴影 / 不透明度
    ├── 03-blur.lua              背景模糊
    ├── 04-anim.lua              开关动画 + 果冻（jelly）
    ├── 05-shader.lua            自定义着色器
    ├── 06-render.lua            刷新与补帧
    ├── 07-clipboard.lua         剪贴板
    ├── 08-ipc.lua               IPC
    ├── 09-output.lua            缩放 + 每块屏的 vidow.output{…}
    ├── 10-xwayland.lua          XWayland
    ├── 11-activation.lua        窗口激活
    ├── 12-portal.lua            xdg-desktop-portal
    ├── 13-cursor.lua            指针
    ├── 14-keys.lua              键位 + 触控板手势（合成器都不内置，默认这一套）+ 工作区 / 窗口规则
    ├── 15-mru.lua               Alt+Tab 的最近窗口切换器
    ├── 16-env.lua               会话环境（和 systemd 用户管理器对账）/ 自启动 / 事件钩子
    ├── 17-overview.lua          全景（overview）的遮罩配色 + 壁纸模糊
    └── 18-background.lua        桌面底色（没壁纸客户端时的兜底；壁纸归 background 层）
```

`vidow.include("configs/01-gaps.lua")` 就是「把那份文件就地展开」，和抄在这里一个意思：

- **相对路径按「正在 include 的那个文件所在目录」解析**（跟 C 的 `#include "…"` 一样），
  所以 `configs/` 里的文件互相 include 写 `vidow.include("04-d.lua")` 就行；绝对路径照用
- include 的先后 = 键生效的先后（同一个键只有第一次写的算数），文件名的数字前缀就是这个顺序
- 文件不在 / 语法写错 / 自己 include 自己，都**当场报错**（带上写 include 那行的行号），
  不静默；报错只带走它后面那些键，前面已经读进来的照样生效
- 想删掉某一类：把入口里那一行 include 删掉就行（文件留着不碍事）
- 不想要这一套也行：全写在一份 `config.lua` 里照样跑，`--default-config all > config.lua`
  就是那个样子

**改完保存就生效，什么都不用做**（也可以手动 `kill -HUP $(pgrep -x vidow)`）。

- 文件不在 = 全用内置默认值（默认值本身是能用的配置）
- 路径可以用 `$VIDOW_CONFIG` 指到别处；`$XDG_CONFIG_HOME` 也认
- 任意键都能用环境变量覆盖：`VIDOW_WINDOW_OPEN_MS=90 ./build/vidow`（优先级最高）
- 重载 = **重新读一遍**：先回默认值再跑文件，所以删掉的行会真的消失（运行时
  `set-config` 改的值也会被抹掉，这正是重载该有的意思）
- 旧的那个 `key = value` 文件（`~/.config/vidow/config`）**已经不再读取**；
  它还在的话启动时会喊一声，把键搬进 `config.lua` 即可

**自动重载**（`src/configwatch.c`）：inotify 盯的是配置文件的**目录**，不是文件本身 ——
编辑器保存文件的方式不止一种，直接写进去（同 inode）和写临时文件再 rename 过来
（换 inode）都得认；盯文件的话第二种会把 watch 留在那个已经被删掉的 inode 上，
之后再也收不到事件。收到事件后自己 `stat()` 目标文件，比对 `(dev, ino, mtime, size)`
真变了才重载，顺带把一次保存里的好几个事件合成一次（去抖 150ms）。

盯的不是一个文件，而是**配置真正读了哪些文件**：入口 + 每一份 `vidow.include()` 进来的
（读不成的也登记 —— 文件补上 / 改对的那一刻要能发现）。重载 = 重新读一遍，所以这一轮
include 了哪些就盯哪些，从入口里删掉的 include 也就不再盯了。

| 你怎么改 | 结果 |
| --- | --- |
| 编辑器里改完保存（原地写 / rename 覆盖） | 自动重载 |
| 改 `configs/` 下任何一份（入口 include 进来的） | 自动重载，日志里点名是哪一份 |
| 删掉配置文件 | 自动重载，回默认值 |
| 删掉某个键再保存 | 那个键回默认值 |
| 删掉一份 include 进来的文件 | 自动重载 + 报「打不开 <那份>」（补回来就又好了） |
| 从入口里删掉一行 include | 那一类的键回默认值，那份文件以后改了也不重载 |
| 同一个目录里动别的文件（`portals.conf` / `shaders/`） | 不重载 |
| 配置文件是符号链接（dotfiles 仓库） | 认（盯的是解析后的真身） |

```lua
-- 全局名就是配置键
border_width = 2
corner_radius = 12
scale = "auto"
vidow.output{ name = "DP-1", scale = 2 }
vidow.output{ name = "HDMI-A-1", scale = 1.25, x = 3840, y = 0 }

-- 需要 shell 结果时用 vidow.exec()：看起来是同步的，实际不阻塞渲染
local accent = vidow.exec("theme.sh accent")   -- 拿到 stdout
border_color_focus = "#" .. accent

-- 一门真语言：条件、循环、函数、字符串都可用
if vidow.exec("cat /sys/class/power_supply/BAT0/status") == "Discharging" then
	blur_levels = 3
	shadow_radius = 16
end
```

`vidow.exec(cmd)` 成功（退出码 0）返回 stdout 一个值；失败返回 `nil, 原因`
（原因优先用 stderr）。需要退出码时加 `{ status = true }` → `out, code, err`
（永远 3 个值）。stdout 和 stderr 分开抓。

```lua
-- 一个值，可以直接嵌进调用
corner_radius = tonumber(vidow.exec("echo 16")) or 12

-- 失败时拿得到真正的原因
local v, why = vidow.exec("cat /nonexistent")
-- v = nil, why = "cat: /nonexistent: 没有那个文件或目录"

-- 要看退出码（grim 只写文件、不打印）
local out, code, err = vidow.exec("grim /tmp/shot.png", { status = true })
if code ~= 0 then vidow.log("截图失败: " .. err) end
```

命令连的是 **vidow 自己**（`WAYLAND_DISPLAY` 在加载配置前就指向了自己的 socket，
并且后端已经起来了），所以 `grim` / `slurp` / `wf-recorder` 截的是 vidow 的画面。

**加载时机**：配置文件要等合成器起得来（输出、seat 都在了）才跑 —— 配置里的
`vidow.workspace_create("1", "DP-1")`、`vidow.workspace_list()`、`vidow.ipc_path()`
都得有真东西可看。代价是 `ipc` / `xwayland` / `scale` 这几个键的消费时机比配置更早：
启动时先按默认值建好，配置跑完（带 `vidow.exec()` 的配置是分几步跑完的）再按配置
对一次账（`vidow_config_sync()`）—— 所以 `ipc = false` 在配置里照样有效，只是
socket 先建后拆。

**`ipc` / `xwayland` 在重载时也对账**：把 `ipc = false` 那行删掉再 SIGHUP，socket
会回来；`xwayland = false` 同理（拆掉时已连上的 X 客户端会被断开，和启动时就关掉等效）。

portal 相关的两个键：

| 键 | 默认 | 作用 |
| --- | --- | --- |
| `portal_desktop` | `Vidow` | vidow 设给自己的 `XDG_CURRENT_DESKTOP`，也是 `<name>-portals.conf` 的那个 name。留空 = 完全不碰 portal。**只影响「会话叫什么」**，选哪个录屏后端看下面那个键 |
| `portal_capture` | `auto` | 录屏 / 截图交给谁：`auto`（默认：自己那个 `vidow-portal` 在二进制边上就用它，否则装了哪个外部后端用哪个，wlr > hyprland）/ `vidow` / `kde` / `wlr` / `hyprland` / `none`。写 `kde` 还要 `portal_desktop = "KDE"`（见下面「录屏」那节） |
| `portal_conf_sync` | `true` | 把生效的 portals.conf 写到 portal 的配置目录，并每次把环境 import 进 systemd 用户管理器（前端环境不对才重启它）；`false` = 只算不写、不碰环境、不起后端 |
实测：配置里 `grim /tmp/shot-cfg.png` → `code=0`，产出的 1280x720 PNG 四象限颜色正确；
`grim --nope` → `code=1`，`err="grim: invalid option -- '-'"`。

**不会阻塞渲染**：整个配置文件在一个协程里跑，`vidow.exec` 把命令 yield 出去，合成器用
自己的事件循环（`pipe` + `wl_event_loop_add_fd`）跑完再把结果 resume 回去。

实测（headless）：配置里 `vidow.exec("sleep 0.8; echo 999")` 挂着的期间，
0.425s 时一个新客户端连上来并完成了窗口映射，0.839s 命令才返回并应用 —— 渲染一刻没停。

其他 API：`vidow.set(k, v)` / `vidow.get(k)`（键名是动态的时候用）、`vidow.log(...)`。
`io` 和 `os.execute` / `os.exit` / `package.loadlib` 被移除：会阻塞或杀掉合成器的东西不该出现在配置里。

超时默认 400ms（`VIDOW_EXEC_TIMEOUT`），超时会 `SIGKILL` 整个进程组，避免留下孤儿进程。

### 截图 / 录屏

四个协议，覆盖新老客户端：

| 协议 | 谁在用 | 创建方式 |
| --- | --- | --- |
| `zwlr_screencopy_manager_v1` | grim / slurp / wf-recorder / OBS | `wlr_screencopy_manager_v1_create()` |
| `ext_image_copy_capture_v1` + `ext_output_image_capture_source_v1` | 新版 grim / wf-recorder / OBS（dmabuf 目标缓冲、逐帧捕获） | `wlr_ext_*_manager_v1_create(display, 1)` |
| `zwlr_export_dmabuf_manager_v1` | 老版 wf-recorder、OBS 的 wlrobs 插件 | `wlr_export_dmabuf_manager_v1_create()` |
| `zwp_linux_dmabuf_v1` | 上面几个的零拷贝路径 | **别自己建**：`wlr_renderer_init_wl_display()` 已经建好了（v4），再来一次会多出一个同名 global |

一个坑：**截图是在输出提交时完成的，而静止画面永远不提交** —— 嵌套后端下宿主更是只在有
损伤时才合成，连 frame 回调都不会发给我们，`grim` 会一直挂到有别的窗口动一下为止。所以有
一个 50ms 的看门狗：只要还有没处理完的捕获，就直接驱动一次渲染。实测静止画面上
`grim` 立即完成（`exit 0`，1280x720 PNG），空闲 5s 的 CPU tick 仍然是 **0**。

录屏实测：`wf-recorder -f /tmp/rec.mp4 -c h264_vaapi` 在 headless 下正常出流
（h264 / 1280x720 / nv12），停录后文件可被 `ffprobe` 正常解析。

**给 portal 用的录屏（vidow 自己的后端）**：上面四个协议是给「客户端自己抓帧」用的。
桌面 portal 那条路（浏览器 / OBS 的「共享屏幕」）走的是另一套 —— 应用问
`org.freedesktop.portal.ScreenCast`，**后端**负责弹选择器、建 PipeWire 流。vidow 现在自己
就是那个后端（`tools/vidowportal.c` → `build/vidow-portal`），**默认就用它**：

| 谁 | 干什么 |
| --- | --- |
| vidow 启动时 | 写 `~/.local/share/xdg-desktop-portal/portals/vidow.portal`（总线名 + 接口清单）、把 `<会话名>-portals.conf` 里的 ScreenCast / Screenshot 指向 `vidow`、把 `vidow-portal` 起起来占住那个总线名 |
| 前端（xdg-desktop-portal） | 只做转发；PipeWire 的 fd 也是它自己连的（`OpenPipeWireRemote` 在前端里，`nm -D` 能看到 `pw_context_connect` / `pw_core_steal_fd`） |
| vidow-portal | 实现 `org.freedesktop.impl.portal.ScreenCast`（CreateSession / SelectSources / Start）和 `Screenshot`（Screenshot / PickColor）。Start 时弹 GTK3 选择器，**按来源分三个 tab**：屏幕（每块屏一张卡，`zwlr_screencopy_manager_v1` 抓帧的实时缩略图 + `wl_output.name`；虚拟屏幕那张卡也在这页）、窗口（`ext-foreign-toplevel-list-v1` 拿列表、`ext-image-copy-capture` 的窗口源抓缩略图）、区域（框一块屏）；点「共享」后按行发 `stream_output` / `stream_window` / `stream_virtual_output` / `stream_region`，等 `created(node)` 把 node id 报回前端。报给前端的 `position` 是**布局逻辑坐标**，从 `xdg-output` 的 `logical_position` 读（`wl_output.geometry` 的 x/y 在 wlroots 下恒为 0,0，以前就是拿它当位置 = 两块屏都报 0,0） |
| vidow | 建一路 PipeWire 流（memfd 缓冲、BGRx），抓帧走 vidow 自己那条路：整块输出 / 输出上的任意矩形（`stream_region`）/ 单个窗口（渲染时把别的窗口和 layer 关掉）/ 现场加的虚拟输出（headless 后端 + 一块真输出，能摆窗口、进布局） |

这样录屏**不依赖 KDE**、也不用把 `XDG_CURRENT_DESKTOP` 说成 KDE —— 会话名还是 `Vidow`，
别的应用一点都不受影响。想换后端就配置里写一行：

```lua
portal_capture = "kde"   -- 借 KDE 的：要配 portal_desktop = "KDE"（代价是所有应用以为在 Plasma 里）
portal_capture = "wlr"   -- 借 xdg-desktop-portal-wlr：没有选择器，直接给第一块屏
portal_capture = "none"  -- ScreenCast / Screenshot 两行都不写
```

**会话持久化（`persist_mode` / `restore_data`，impl 接口 v4）**：以前每录一次都要重新弹选择器 ——
后端只声明到 v2，前端根本不会把持久化那两项给它（用户：「录屏 portal 需要持久化」）。现在
`SC_VERSION = 4`（**只到 4**：v5 的 `mapping_id`、v6 的 `pipewire-serial` 是流属性，我们不在
`Start` 的 streams 里给，报了就是给消费者制造期待）：

- `SelectSources` 里解析 `persist_mode`（0/1/2）和 `restore_data`（`(suv)` 三元组：vendor /
  私有版本号 / 私有数据）；`Start` 的 results 里回 `restore_data` + `persist_mode`（原样回，
  没做「降级」那套 UI）；
- 持久化数据**存在前端**（权限库，按 app_id 存），后端只做两件事：生成一份**能自证**的、收到时
  认得出来就**跳过选择器**直接建流。vendor 用 `vidow`（和 KDE / GNOME 一个做法），私有版本号从
  1 起，私有数据是个**自描述字符串**（不用指针 / 时间戳这种没法复现的东西）：

  ```
  kind|标识符|cursor_mode[;kind|标识符|cursor_mode]…
  kind：monitor（标识符 = 输出名）/ window（= ext-foreign-toplevel 的 identifier）/ virtual（= WxH）
  例：monitor|HEADLESS-1|1        ← 一块叫 HEADLESS-1 的屏、隐藏光标
      window|ad28e827…524|4       ← 某个窗口、光标走 metadata
  ```
  （区域那条路是当场框出来的、没有稳定标识符，不进 `restore_data` —— 全是区域的会话就不回它，
  等于这次没持久化，日志里会说。）
- **不可信输入**：`restore_data` 是前端原样转回来的 ——格式不合法（vendor 不是 vidow / 版本号对不上 /
  私有数据不是字符串 / 解析不出来）就**忽略 + 照常弹选择器**；合法但**源不在了**（窗口关了 / 输出
  的名字对不上 / types 这次没要这种源）也**回退弹选择器**（不是报错），半路建起来的流全收掉；「源没了」有两条路都能走到、日志会说清是哪条：
  还没建流就发现窗口不在了（`ext_foreign_toplevel` 的 `closed`），或者建流被**合成器**拒掉
  （`vidow: 没有这个窗口（它可能已经关了）`）—— 后者才是权威信号，因为那个列表里可能还挂着已经关掉的窗口；
  光标模式按存下来的那份恢复（这次请求带的另一份不覆盖它，两份不一样会记一行日志）。
- 日志一眼能当判据：`restore_data：收到 …` / `判定合法（N 个源，光标 …）` / `判定非法（原因），
  忽略，照常弹选择器` / `恢复成 N 路流，跳过选择器` / `输出 … 不在了，回退弹选择器` /
  `合成器不认这些源（…），回退弹选择器` / `Start：回 restore_data（…）`。
- 前端那层是**翻译**：它把后端的 `(suv)` 换成自己的 `restore_token`（字符串）发给客户端
  （`Replacing restore data received from portal impl with a token`），客户端下次把 token 带回来
  时再翻译回 `restore_data` 喂给后端 —— 所以「客户端看到的 token」和「后端看到的私有数据」不是
  一回事，`./test-portal-own.sh` 的 6f 一节两层都验（客户端带 token → 后端收到 restore_data）。
- 一个已知边界：**输出被拔掉**这条路后端看不出来（`outputs` 列表只加不删，
  `registry_global_remove` 是空的）—— 窗口没了有确切信号（`ext_foreign_toplevel` 的 `closed`），
  持久化恢复里已经按它判「源还在不在」；输出列表的增删是另一件事，这次没动。

**选择器分三个 tab：屏幕 / 窗口 / 区域**（用户：「屏幕/窗口/区域 共享分为三个tab」）。顶上一条
分段控件（`GtkStackSwitcher`，选中那段填强调色），下面一页一个网格；**只列应用真的在要的那几种
来源**，只剩一个 tab 时那条控件收起来。虚拟屏幕并进「屏幕」页（它本来就是「再来一块屏」）。
「区域」页**那张卡自己就是按钮**（没有单独的按钮）：点它 = 去框一块 —— 铺一层**全屏透明浮层**，
在**正在动的画面上**拖一个框（不冻屏 —— 冻屏是截图那条路），松手记下这块区域（卡片上换成那块区域的
实时缩略图），按「共享」就录它；还没框成又按 Esc 的话这张卡跟着取消选中（别让「共享」为一个空区域
亮着）；坐标按协议
要的口径换算：`stream_region` 要**布局逻辑坐标**（相对那块屏的坐标 + 那块屏在布局里的位置），
报给前端的 `size` 是**像素**尺寸（逻辑 × scale）。

**选择器长什么样**（`tools/vidowportal.c`，用户：「portal 样式弄得好看一点现代一点，更接近 kde
一点」）：配色走 Breeze —— 底 `#eff0f1`、白卡片、强调色取 KDE 的 `DecorationFocus`
（`~/.config/kdeglobals` 的 `[Colors:Selection]`，没配就是 Breeze 蓝 `#3daee9`），深色 GTK 主题
自动跟着换；卡片圆角 + 悬停描边，选中 = 强调色 2px 描边 + 底衬淡染（缩略图右上角那个对勾去掉了，
用户：「选中的勾去掉」），缩略图自己切圆角并描一圈细边；标题栏是「共享屏幕」+「谁在要录屏」的副标题；底栏一条分隔线 + 灰色
「取消」+ 强调色「共享」。虚拟屏幕那张卡没有画面可预览，用虚线框 + 显示器图标占位。**圆角是超椭圆（squircle）**：
卡片 / 按钮 / tab 的底、虚线占位框、缩略图的角都走同一条 `squircle_path`（`|x/a|^n + |y/b|^n = 1`，
n=4，GTK 的 `border-radius` 只会画圆弧），CSS 里对应的背景 / 边框让开（透明 + 无边框），悬停 /
选中 / 禁用三种状态在各自的 `draw` 里填色；半径现在是缩略图 10、卡片 / 按钮 / tab 16（底栏那两个
按钮只有 ~34px 高，16 已经撞到「不超过高度一半」的上限 = 胶囊形，再往上调不变）；tab 高亮那块的大小
跟着 `stackswitcher button` 的 padding 走（现在 `14px 44px`，分段控件 570 宽、每段 188；高亮块在段内四边再内缩 5px → 178x41）。**切 tab 的
过渡动画在高亮块上**（用户：「切换动画是tab高亮，不是item」）：高亮不是按钮自己画的，是**分段控件的
容器**画的（容器先画、按钮后画 → 高亮在文字底下），换页时按 `notify::visible-child` 从上一段滑到
新的一段（`TAB_ANIM_MS` 200ms，ease-out cubic，`gtk_widget_add_tick_callback` 逐帧插值）；
页内容不滑（`GtkStack` 仍是 `TRANSITION_NONE`），三页高度对齐所以尺寸不动。样式全在 CSS 里（`GtkCssProvider`，结构那边只挂 class 名），不引新依赖；想换色：`VIDOW_PORTAL_ACCENT=#rrggbb`、
`VIDOW_PORTAL_DARK=0|1`。**对话框高度 = 内容高度**（滚动区把内容的自然高度顶给窗口），装不下时才
滚动 —— 别设 `max_content_height`，那个数比内容大就会凭空把窗口撑高（踩过：多出 53px 空白）。
**键盘焦点不画 GTK 那圈虚线**：Adwaita 的焦点指示器是虚线，叠在选中的卡片上就成了「两个高亮」
（用户报的「第一个 tab 一个奇怪的虚线框」）。现在卡片 / tab 的焦点不画（选中态本身就是指示），
未选中的卡片给 2px 强调色实线（`outline-offset: -3px`），底栏按钮给 2px 强调色实线（-7px）。
`vidow-portal --dialog` 可以脱离 DBus 直接看排版：`VIDOW_PORTAL_TYPES=1|2|4|7` 只看某一类来源，
`VIDOW_PORTAL_PARENT=wayland:<handle>` 当挂到应用窗口上（handle 从 `tools/foreign-app.py` 拿）——
带父窗口时合成器把它当**浮窗**、尺寸由客户端自己定，这才是真机上的样子；没父窗口时它就是个普通
平铺窗口，会被铺满一整列。

**发帧节奏 = min(消费者要的帧率, 屏幕刷新率)**（`src/screencast.c` 的 `stream_send_due`）：
抓一帧是**整块离屏渲染 + 一次整块回读**，而且是在合成器的主线程上做的（PipeWire 的 loop
挂在 Wayland 的 event loop 上）。以前只要输出提交就抓一帧 —— 144Hz 的屏上每秒 144 趟整屏
渲染（消费者手上还有空缓冲时每次提交都再回读一整块），主线程被占满：屏幕上所有动画 / 拖动
跟着掉到 60fps（用户报的「portal 录制导致显示器帧率被限制到 60」）。现在抓帧只在**消费者
真的来要一帧、而且到点了**的时候发生：输出提交只把手上那一帧作废并催一次，消费者手上没空
缓冲时一次都不抓。上限取输出的**标称刷新率**（不用量到的帧间隔：那个会被「录制把帧率拖下去」
反过来影响 —— 帧间隔变大 → 上限放宽 → 抓得更多，是个正反馈），消费者自己协商了更低的帧率
就听消费者的；输出没报刷新率的（headless / 虚拟输出）才退回量到的帧间隔。
`vidowmsg state` 的 `screencast` 那一块把这件事摆出来：`frames`（发出去多少帧）/ `grabs`
（抓了多少次整帧）/ `commits`（看见多少次输出提交）/ `throttled`（消费者来要但被上限挡下
多少次）/ `cap_ms`（上限）—— 修好之后 `grabs ≈ frames`，而 `commits` 可以远大于它俩。

**一帧的主线程成本**也报出来了（同一个 `screencast` 块）：`grab_ms` / `grab_ms_max`（整块
离屏渲染，把场景画进抓帧缓冲）、`readback_ms`（从那张缓冲回读到 PipeWire 缓冲 —— GPU→CPU
的同步拷贝）、`sync_ms`（直通那条路上「等 GPU 把这一帧画完」）。走直通时 `readback_ms == 0`
且 `dmabuf_frames > 0`；退回回读时 `dmabuf_frames == 0`。

**「录制时整个桌面（连光标）都顿」的根子就是那笔回读**：它在**合成器主线程**上做（PipeWire
的 loop 挂在 Wayland 的 event loop 上），实测 2560x1440 一帧 6.2ms、4K 一帧 ~14ms —— 每发
一帧桌面就冻这么久，上限（`cap_ms`）压着它只能减少次数、去不掉单次成本。Hyprland /
xdg-desktop-portal-wlr 那边这笔拷贝在 **portal 进程**里做、合成器只交一张 dmabuf，所以它们
不顿；vidow 的 zkde 流是**合成器自己建的**（协议就是这么定的），只能在合成器这边把它消掉。

**现在是 dmabuf 直通（零拷贝）**：抓帧渲染进一张 **linear** 交换链，缓冲由**我们**分配
（`PW_STREAM_FLAG_ALLOC_BUFFERS`），fd 在 `add_buffer` 里交给消费者，像素一帧都不落 CPU。
实测 1280x720 一帧的主线程成本：**7.6ms（渲染 1.4 + 回读 6.2）→ 0.33ms**（只剩「等 GPU 把
这一帧画完」）。

踩过的两个坑（写在这儿，免得下次重走）：

1. **缓冲必须由生产者分配**。PipeWire 默认是**消费者**分配缓冲：它建池那一刻就把内存包死了
   （GStreamer 的 `gstpipewirepool` 只 wrap 一次：`wrap data (MemFd 2) 4096 3686400`），之后
   我们每帧往 `spa_data` 里塞什么 fd 它都看不见 —— 实测塞 dmabuf 出去，消费者读的还是它自己
   那张 memfd，六帧全黑。所以要用 `PW_STREAM_FLAG_ALLOC_BUFFERS` + `add_buffer` 事件把内存
   填进去。另外**一个槽一张、fd 从头到尾不变**：消费者只认建池时包进去的那张。
2. **报修饰符会把 GStreamer 的 caps 协商整死**（实测 `handle_format_change` 直接断言失败、
   一帧都拿不到），而**不报**修饰符时消费者的默认假设就是 linear（modifier 0）—— 所以抓帧
   直接渲染进 linear 缓冲，两边默认值就对上了，一个修饰符都不用谈。抓帧自己那张缓冲是
   tiled + DCC 两平面的（实测 `mod=0x200000028a6bf04 n=2`），交给消费者它也没法 import。

还有一笔省不掉的账：**交出去之前要等 GPU 把这一帧画完**（`sync_ms`，实测 0.3ms）。消费者
（OBS / 浏览器 / gst）多半是把这块内存 mmap 下来在 CPU 上读，它没有任何办法知道 GPU 写完
没有 —— 不等就是读到还没画进去的那张（实测全白/全黑）。真要做到不等，得给消费者带**显式
同步**（dma-fence / syncobj，`SPA_DATA_SyncObj`），那是另一件事。

消费者不认 dmabuf（协商出来是 memfd）时，我们自己建一张 memfd + mmap、回读进去；连 linear
交换链都建不出来的会话（`WLR_RENDERER=pixman`，缓冲是 shm）就完全退回老路（消费者分配 memfd
+ 回读）。`VIDOW_SC_NO_DMABUF=1` 强制走回读那条路（A/B 用）。

实测（`./test-portal-own.sh`，92 项：64 项老判据 + 6f 的 27 项持久化 + 1 项已知偶发）：私有 dbus + headless vidow → `vidow.portal` 落地、
`vidow-portal` 占住总线名 → 前端把 ScreenCast / Screenshot 指到它 → 选择器画在 vidow 上
（标题「共享屏幕」，第一行默认选中）→ 回车确认 → `Start` 返回 `node` / `size`：
整屏（1280x720）、**窗口**（`source_type=2`，尺寸报窗口的）、**虚拟输出**（合成器里真多出
一块 `HEADLESS-N`，流结束又拔掉）三种都出帧；`Screenshot` 回一个真 PNG（1280x720）、
`PickColor` 回 `(ddd)` 颜色；光标元数据那条路消费者能拿到 `SPA_META_Cursor`（整屏和
**窗口**两条路都验：光标放在窗口中间时元数据里的位置就在流的中间，见 173）。

截图（`Screenshot` / `PickColor`）**不走 PipeWire**：后端自己就是客户端，抓一帧像素、
写个 PNG、把文件 URI 回给前端（写在 `$XDG_CACHE_HOME/vidow-portal/`）。
`options.interactive = true`（或者 `PickColor`）会把那一帧冻住铺满屏，让用户框一块 / 点一下。

光标三种模式都支持（`AvailableCursorModes` 报 7）：`hidden` 不带光标、`embedded` 按屏幕上的
样子画进画面（放大摇晃那种大光标也一样）、`metadata` **不进画面**，改成每帧往 PipeWire 缓冲的
`SPA_META_Cursor` 里写位置 + 热点，光标图变了才附上位图（`bitmap_offset=0` = 沿用上一次那张），
消费者自己合成 —— 光标永远清晰、跟着消费者的缩放走，Firefox / OBS 这类客户端就是这么要的。
注意元数据是**双方协商**的：消费者得自己声明 `SPA_META_Cursor`（GStreamer 的 `pipewiresrc`
声明的是范围，我们的固定值落在里面），否则缓冲里压根没有这个元数据（验证工具见
`./build/cursorprobe --declare`）。KDE 后端那条路（`portal_capture = "kde"`）仍然可用，
见 `./test-screencast.sh`。

**注意**：`zkde_screencast_unstable_v1` 是对所有客户端开放的，也就是说不经过 portal 的客户端
也能自己绑它录屏 —— **这是有意和 KDE 保持一致的策略**，KWin 那份也一样开放（它只用客户端的
可执行路径当 HACK，判断要不要把 portal / plasmashell 自己的窗口从录屏里藏掉，不是权限检查）。
想真收紧得连 screencopy / ext-image-copy-capture 一起收，那会把 grim / wf-recorder / OBS
全废掉，不值；真要边界得靠「别的进程拿不到你的 wayland socket」那一层（sandbox），不是合成器。

### 输入法（text-input-v3 + input-method-v2）

两个协议是一对，合成器夹在中间搬数据：

```
客户端(GTK/Qt/kitty) --text-input-v3--> 合成器 --input-method-v2--> fcitx5
客户端(GTK/Qt/kitty) <--text-input-v3-- 合成器 <--input-method-v2-- fcitx5
```

客户端说「我有输入框、光标在这、周围文字是这些」，输入法回 preedit / commit。除此之外
还有两件只有合成器能干的事：候选框（`zwp_input_popup_surface_v2`）由我们渲染并摆到光标
下面，抢键盘（`grab_keyboard`）之后按键不再进 seat、由输入法决定消费还是回注。

`src/im.c` 里的五个坑，都是实测踩出来的：

1. **`events.commit` 不带 data**（wlroots 0.20 传 NULL）。照着 sway 的老代码写
   `struct wlr_input_method_v2 *method = data;` 会拿到 NULL，每次 commit 都被静默丢掉 ——
   表现是输入法看着一切正常（activate、grab 都成功），但客户端一个字都收不到。用自己存的
   `im->method`。
2. **虚拟键盘的 keymap 不能覆盖**。协议要求客户端自己送 keymap；我们用默认 us 布局覆盖掉
   输入法回注用的 keymap，结果它想发 'a' 的键码被解释成 ESC，直接把对话框关了。
3. **输入法抢键盘时要同步键盘**。抢的那一刻 seat 上可能一个键盘都没有（上一个虚拟键盘刚
   销毁），输入法拿不到 keymap 就不会回注。键盘一有变化就 `wlr_input_method_keyboard_grab_v2_set_keyboard()`
   重发一次。
4. **回注的按键不能再喂回去**。输入法用 `zwp_virtual_keyboard_v1` 回注按键，如果无差别
   转发给 grab 就是自己喂自己，死循环。判据是「这个虚拟键盘的 client 是不是输入法的 client」，
   不是「是不是同一个 wlr_keyboard」—— 后者会漏掉插拔和 wtype 这类第二个键盘。
5. **换窗口要先 leave 再 enter**。`wlr_text_input_v3_send_enter()` 要求
   `focused_surface == NULL`（wlroots 0.20 里是 assert），同一个客户端从窗口 A 切到
   窗口 B 时不能直接 enter，得先 `send_leave()`。这个坑是 KDE portal 的 Qt 对话框
   逼出来的：它一上来就有两个窗口在切。

我们自己的 `Super+...` 快捷键在转发之前处理，所以输入法抢着键盘时快捷键照旧能用。

实测（headless + `tools/fakeim.c` 假输入法）：

- 假输入法：`activate` → `surrounding_text` → `grab keymap` → 每个按键 commit 一段文字 →
  客户端 `zenity --entry` 里真的出现 `<1><2><3>`（按键 → 合成器 → 输入法 → 合成器 → 客户端全通）
- 真输入法 fcitx5：连上 → `text input enabled` → `input method activated` → `grabbed the keyboard`
  → 英文直通（按键被输入法回注，zenity 收到 `abc`），全程无回环

没验到的：中文转换。fcitx5 在这套 headless 环境里切不到 rime（无 session bus、rime 部署状态
不明），按键一路直通，没产生 commit。这是输入法侧的事，与合成器无关 —— commit 中继本身用
假输入法验过了。

### ext-background-effect：客户端自己要求模糊

wlroots 0.20 没实现这个协议（没有对应头文件），服务端手写在 `src/bgeffect.c`：

- 全局 `ext_background_effect_manager_v1`，bind 时告诉客户端支持 blur
- 每个 `wl_surface` 最多挂一个 `ext_background_effect_surface_v1`（重复挂报
  `background_effect_exists`）
- `set_blur_region` 是双缓冲的：surface commit 之后才生效

合成器侧的语义：

- 客户端**请求了区域** → 只模糊那块（着色器里做矩形裁剪），且**不管 `blur_all` 开没开**
- 客户端**没请求** → 走原来的策略（`blur_all` / 浮动窗口）

三个坑，都是实测踩出来的：

1. **区域要每次 commit 重新裁**。客户端一般在第一次 configure 之前就 `set_blur_region`
   了，那时 surface 还是 0x0，按协议裁到 surface 尺寸就裁成空的，而且之后再也回不来。
   原始区域单独存一份，每次 commit 从它重建再裁。
2. **「变了没有」要比较裁剪后的盒**。客户端每帧都会 commit，不比较就会每帧逼合成器重绘
   装饰，静态桌面的零开销就没了。
3. **只影响着色器输出的变化要显式失效**。区域只改着色器里的裁剪，贴图身份没变（共享
   backdrop 还是那一张），帧循环看到 `blur_drawn == blur_ver` 就不重画，改了也看不见。

验证工具 `tools/bgblur.c`：半透明窗口 + 只给中间一条请求模糊，背后是 wall.py 的硬边
方块，截图数「强边数」（相邻像素差 > 25 的个数）：

| | 带内 y160-300 | 带外 y400-600 |
| --- | --- | --- |
| 请求了区域 | **0**（糊了） | 500（清晰） |
| 没请求（对照） | 420（清晰） | 500（清晰） |

`blur_all=true`（默认配置）时同样是带内 0 / 带外 500：客户端指定的区域盖过全局策略。

### xdg-desktop-portal（GTK / KDE 后端）

portal 前端（`xdg-desktop-portal`）自己不是 Wayland 客户端，它只做 DBus 转发；真正连到
合成器的是**后端进程** —— `xdg-desktop-portal-gtk`（GTK3）和 `xdg-desktop-portal-kde`（Qt6）。
所以「支持 portal」= 让这两个后端能把界面画对、把窗口关系说清楚。`src/portal.c` 补了四组协议：

| 协议 | 谁在用 | 缺了会怎样 |
| --- | --- | --- |
| `xdg_activation_v1` | portal 弹窗要焦点、通知点击后把窗口顶到前面 | 对话框拿不到键盘焦点（要手动点一下） |
| `zxdg_exporter/importer_v1` + `_v2` | 文件选择器挂到发起请求的那个窗口上 | 对话框变成孤立的顶层窗口，不居中、不跟随父窗口 |
| `zwlr_foreign_toplevel_manager_v1` | waybar 任务栏、`wlrctl`、按窗口录屏 | 客户端看不到任何窗口 |
| `ext_foreign_toplevel_list_v1` + `ext_foreign_toplevel_image_capture_source_v1` | 新版窗口枚举 / 逐窗口录屏 | 同上 |

**选哪个后端不是合成器决定的**，是 `~/.config/xdg-desktop-portal/<desktop>-portals.conf`
（会话名取 `XDG_CURRENT_DESKTOP` 里冒号分隔的每一段，全小写）。`gtk.portal` 带
`UseIn=gnome`、`kde.portal` 带 `UseIn=KDE`，会话名不匹配时 xdg-desktop-portal 只能
「last-resort」地随便挑一个并打警告；`[preferred]` 段里的显式名字会盖过 `UseIn`。

#### 换文件选择器：改一行就行

那个 `-portals.conf` 属于 portal 的地盘，而且内容不该逼你手写。所以 vidow 自己管这份，
分三层（`src/portalconf.c`）：

```
内置默认（src/portalconf.c 的 default_lines）      GTK 打底，GTK 没有的接口给 KDE
     ↓ 被下面这份「逐键」覆盖 —— 只写你想改的那行，其余保持默认
~/.config/vidow/portals.conf                      ← 你改这里（模板：portals.conf.example）
     ↓ vidow 启动时同步，内容变了才写
$XDG_CONFIG_HOME/xdg-desktop-portal/vidow-portals.conf   ← xdg-desktop-portal 真正读的
     ↓ 写完顺手重启 portal（先 import-environment）
```

所以**切换文件选择器 = 改 `~/.config/vidow/portals.conf` 里的一行，然后重启 vidow**：

```ini
[preferred]
org.freedesktop.impl.portal.FileChooser=kde   # gtk = GNOME 风格，kde = Dolphin 风格
```

逐键合并意味着上面这一行就够了，不用把整份配置抄一遍。其他几条规则：

| 想干的事 | 怎么写 |
| --- | --- |
| 换 GTK 没有的接口的实现 | `org.freedesktop.impl.portal.Wallpaper=kde` |
| 去掉某个接口的实现 | 值留空，例如 `org.freedesktop.impl.portal.Clipboard=` |
| 加内置默认里没有的接口 | 直接写一行，会被追加进去 |
| 整个通道关掉 | `portal_desktop=`（vidow 连 `XDG_CURRENT_DESKTOP` 都不设） |
| 只算不写（自己管那个文件） | `portal_conf_sync=false` |

`vidow_portal_conf_sync()` 只在生成内容和现有文件**逐字节相同**时跳过写盘和重启
portal —— 不会每次开机都白重启一遍、打断正在进行的 portal 请求。但**环境那一步不跟着
跳过**：后端是 dbus 按需激活的，环境取自 systemd 用户管理器，而那里留的是**上一个**
合成器的值（Hyprland 之类），文件没变不代表环境是对的。所以现在是「每次都 import 环境，
只在配置真的变了时才重启前端」。重启是双 fork + setsid 的，异步、不阻塞渲染。

**退出时会把环境放回去**（`vidow_portal_env_snapshot()` / `vidow_portal_env_restore()`）：
启动时先问一次 `systemctl --user show-environment`，把 `XDG_CURRENT_DESKTOP` /
`WAYLAND_DISPLAY` / `XDG_RUNTIME_DIR` 记下来（注意要问用户管理器，不能问 vidow 自己的
环境 —— 从 tty 起来时那里的 `WAYLAND_DISPLAY` 本来就是空的），退出时 `set-environment`
写回、原来就没有的 `unset-environment` 掉，再重启一次前端。不还的话：回到宿主合成器
以后新拉起来的后端连的是**已经消失的 vidow socket**，portal 全线失效；而且
`Linger=yes` 时登出也救不回来（用户管理器不跟着会话退出，Hyprland 自己的启动逻辑也只
import 环境、**不重启** `xdg-desktop-portal` —— 这一点在它的二进制里查过）。退出路径包括
IPC 的 `quit`、键位退出，以及 `SIGTERM` / `SIGINT`（Ctrl+C、`pkill`）—— 这两个信号现在
走 `wl_display_terminate` 的正常退出，不再用默认动作直接结束进程。

两个环境变量（给测试/排查用）：`VIDOW_PORTAL_NO_RESTART=1` 只写文件，环境快照 / import /
还原一概不碰（`test-portalenv.sh` 就靠它做隔离）；
`VIDOW_CONFIG=/path` 换配置文件位置（`VIDOW_CONFIG_LUA` 是同一个东西的旧名字，还认）。

后端是被 DBus 激活起来的，继承的是 systemd user session 的环境，所以重启前必须
`import-environment`（否则对话框会画到宿主桌面上）。vidow 自动做这一步：

```bash
systemctl --user import-environment XDG_CURRENT_DESKTOP WAYLAND_DISPLAY XDG_RUNTIME_DIR
systemctl --user restart xdg-desktop-portal
```

#### 录屏 / 截图：内置默认会自己挑，但**不是 KDE**

`ScreenCast` / `Screenshot` 这两个接口，GTK 后端**根本没有**；KDE 后端有，但它的实现建在
KWin 的私有 wayland 协议上（`zkde_screencast_unstable_v1`，`xdg-desktop-portal-kde` 的
二进制里就写着 `zkde_screencast_unstable_v1 does not seem to be available`），vidow 不实现
它，指过去只会让客户端以为支持、一调用就失败。真能在这台合成器上干活的是：

| 后端 | 走什么 | 本机装了吗 |
| --- | --- | --- |
| `wlr` | `zwlr_screencopy_manager_v1`，专为 wlroots 写的 | 没装（`xdg-desktop-portal-wlr`） |
| `hyprland` | 非 Hyprland 的 wlroots 合成器上同样退到 `zwlr_screencopy_manager_v1`（`UseIn` 里就写着 `wlroots;sway;Wayfire;river`） | 装了 |

vidow 自己实现了 `zwlr_screencopy_manager_v1`（`src/screencopy.c`，实测 `wayland-info`
报 `zwlr_screencopy_manager_v1 version 3`），所以这两个后端都能用。于是默认**探测装了哪个
用哪个**（`capture_backend()`，`XDG_DATA_DIRS` 里找 `xdg-desktop-portal/portals/{wlr,hyprland}.portal`，
wlr 优先），一个都没装就一个字都不写 —— 写了等于骗客户端。想强行指定就在
`~/.config/vidow/portals.conf` 里覆盖：

```ini
[preferred]
org.freedesktop.impl.portal.ScreenCast=hyprland
org.freedesktop.impl.portal.Screenshot=hyprland
```

（`[preferred]` 里的显式名字会盖过后端自己的 `UseIn` —— 实测：会话名是 `Vidow`、后端写
`UseIn=KDE` 时，xdg-desktop-portal 照样按配置去激活那个后端。`RemoteDesktop` 同理，
但它只有 wlr 后端有实现，hyprland 后端没有，所以默认不写。）

在 tty 上排查「portal 现在指向谁」（换了会话 / 从别的桌面切过来之后最容易踩）：

```bash
./portaltest.sh           # 体检 + 发一个真的文件选择器请求（需要 vidow 在跑）
./portaltest.sh --check   # 只体检，不动你的屏幕
```

它逐项报出：session bus 在不在、vidow 的 wayland socket 是哪个、systemd 用户管理器里的会话
环境、**每个 portal 进程各自连着哪块屏**（不在当前这条总线上的不算）、生成的 portals.conf，
最后起一个 GTK 客户端（`GTK_USE_PORTAL=1`）真的弹一次文件选择器，验证对话框画在 vidow 上、
挂在发起窗口上（xdg-foreign）、拿到了键盘焦点（xdg-activation）。

最常见的一种 FAIL：从别的桌面（Hyprland / niri）切到 tty 上的 vidow 之后，**portal 后端进程
还活着、还连着旧合成器**（它们是被 dbus 激活起来的常驻进程，`restart` 前端不会重启它们），
于是对话框画到另一个会话里。收掉它们即可（下次用到时 dbus 会按新环境重新拉起）：

```bash
pkill -f xdg-desktop-portal-gtk; pkill -f xdg-desktop-portal-kde; pkill -f xdg-desktop-portal-hyprland
```

也正因为 vidow 会设 `XDG_CURRENT_DESKTOP=Vidow`，配置里 `vidow.exec` 启动的客户端
看到的就是 `Vidow`（实测子进程 `env` 里是 `XDG_CURRENT_DESKTOP=Vidow`）。

四个坑，都是实测踩出来的：

1. **`XDG_DESKTOP_PORTAL_DIR` 一设，portals.conf 就去那儿找**。默认（不设）才读
   `$XDG_CONFIG_HOME/xdg-desktop-portal/<desktop>-portals.conf`。为了「指定 portal 目录」
   顺手设了这个变量，结果是配置文件被静默忽略，所有接口退化成 last-resort。
2. **`parent_window` 必须带 `wayland:` 前缀**（`wayland:<handle>`）。不带的话
   xdg-desktop-portal-gtk 报 `Unhandled parent window type <handle>` 然后挂不上。
3. **xdg-foreign v1 和 v2 必须共用同一个 `wlr_xdg_foreign_registry`**。GTK3 的
   `gdk_wayland_window_set_transient_for_exported()` 走 v1 importer，GTK4 / Qt6 走 v2；
   不共用一个 registry，v2 导出的 handle 在 v1 那边就是「查无此物」。
4. **wlroots 会拒绝 `set_serial(0)` 的 activation token**（`serial 0 was never given to client`）。
   GTK / Qt 的 portal 对话框恰恰就是这么发的 —— 不影响可用性，因为我们 map 新窗口时
   无条件给焦点，对话框照样是聚焦的。`activate` 真正不可替代的场景是「应用已经开着，
   要把旧窗口顶到前面」，那条路上的 token 带的是真实输入 serial，会被接受。

portal 还顺带踩出两个 vidow 自己的 abort（都是 wlroots 0.20 把警告改成 assert 之后才致命的）：

- `wlr_xdg_surface_schedule_configure: Assertion 'surface->initialized' failed` ——
  Qt/KDE 的对话框在**第一次 commit 之前**就把 parent 设好了，于是
  `view_set_parent → vidow_float_update_mode` 在 surface 还没 initialized 时就调了
  `wlr_xdg_toplevel_set_size`。修法：`vidow_view_configure_ready()` 挡住所有早期
  configure，初始 configure 统一由 `view_commit` 的 `initial_commit` 分支发
  （浮动窗口那一支也得自己发一次，否则客户端等不到 configure 就不上屏）。
- `wlr_text_input_v3_send_enter: Assertion 'text_input->focused_surface == NULL' failed` ——
  同一个客户端从一个窗口切到另一个窗口时，`vidow_im_focus()` 直接 `send_enter(新窗口)`，
  但旧的 focused_surface 还在。修法：先 `send_leave` 再 `send_enter`。

验证（`test-portal.sh`，headless vidow + 私有 dbus + 真 portal 前端）：

| | GTK 后端 | KDE 后端 |
| --- | --- | --- |
| portal 读到的配置 | `vidow-portals.conf` | 同 |
| 选中的实现 | `Using gtk.portal for ...FileChooser (config)` | `Using kde.portal for ...FileChooser (config)` |
| `OpenFile` 返回 | 请求句柄 | 请求句柄 |
| vidow 里出现的窗口 | `map vidow portal test: 674x748` | `map vidow portal test: 646x513` |
| 拿到键盘焦点 | `im focus -> 0x...` | 同 |
| 带 parent_window 的对话框挂上了 | 2/2 | 2/2 |
| 合成器存活 | 是 | 是 |

`xdg-foreign` 单独用 `tools/foreign.c` 验：它导出一个 handle，把这个 handle 当
`parent_window` 传给 GTK3 的 portal，vidow 日志出现
`portal: 父子 vidow portal test -> foreign-app`，portal 侧「挂不上父窗口」的警告 **0 条**。
v1 和 v2 导出的 handle 各试一遍都能挂上 —— 这就是共用 registry 的意义。

`zwlr_foreign_toplevel_manager_v1` 用真客户端 `wlrctl` 验：

```
$ wlrctl toplevel list            # : zen-one / : zen-two
$ wlrctl toplevel focus title:zen-one   # request_activate → 焦点转移
$ wlrctl toplevel close title:zen-two   # request_close → 只剩 zen-one
```

### 内置效果

不写 shader 时的默认效果：淡入淡出 + 垂直滑动 + 缩放弹跳（`open_alpha_from` /
`open_slide_px` / `open_scale_from` / `close_alpha_to` / `close_slide_px` /
`close_scale_to`），缓动可选
`linear` / `out_cubic` / `out_quint` / `in_out_cubic` / `out_back` / `out_elastic`，
也可以自己写一条三次贝塞尔（`easing = "bezier 0.05 0.9 0.1 1.08"`，四个数是 x1 y1 x2 y2，
y > 1 的部分就是过冲幅度）或者单独调回弹幅度（`easing = "out_back 0.25"`；全局
`back_overshoot` 管所有没单独写幅度的 `out_back`，默认 0.10 = 冲过终点 10%）。

**默认是「q 弹」的**：入场从 0.82 弹到 1.0（`open_easing = "out_elastic"`，冲到
1.02 再收住），关闭缩到 0.82 之前先缩过头一点再涨回来。`out_elastic` 是阻尼余弦
`1 - e^(-5.5t)·cos(7t)`：t = 0 处正好是 0（曲线必须从起点出发），t≈0.2 到终点、
t≈0.35 冲过头 11%（乘上 `open_scale_from` 到 1.0 那 0.18 的区间就是 2%）、之后单调
收敛。缩放作用在**场景节点**上、围绕窗口中心，离屏缓冲照旧按原尺寸画（为几个百分点
的过冲每帧重分配缓冲不值）；几何量跟着过冲，透明度和 shader 的 `u_progress` 夹在
0..1（负 alpha 在场景节点上没定义，进度 > 1 会让 `zoom` 这类 shader 采到贴图外面）。

`easing` 仍然只管位移补间 / layer surface / overview：布局重排要的是「快而稳」，
过冲反而晃眼。入场和关闭各有自己的曲线（`open_easing` / `close_easing`）。

### 自定义 shader

`shader_open` / `shader_close` / `shader_idle` 指向 `shader_dir` 下的 `<名字>.frag`。
片段着色器自己写 `main()`，可用的 uniform：

```glsl
uniform sampler2D u_texture;    // **整个窗口**（含边框/圆角/模糊底衬，预乘 alpha）
uniform float u_progress;       // 0 = 动画开始，1 = 结束
uniform int   u_phase;          // 1 打开 / 2 关闭 / 3 常驻
uniform vec2  u_size;           // 窗口像素尺寸
uniform vec2  u_origin;         // 窗口在输出上的位置
uniform vec2  u_output_size;
uniform float u_alpha;          // 淡入淡出系数，需自行乘进结果
uniform float u_time;           // 效果开始至今的秒数（墙钟）
varying vec2  v_uv;             // 0..1，左上为原点
```

输出必须是**预乘 alpha**。管线是：先把窗口连装饰（边框 / 圆角 / 模糊底衬 / 阴影）画进一块
离屏缓冲 → 把**这块装饰结果**当 `u_texture` 喂给你的 shader 画到第二块离屏缓冲 → 作为
scene buffer 合成，所以 shader 可以任意改写几何（缩放、挤压、抖动）。注意 `u_texture` 是
「整个窗口」而不是「光秃秃的内容」：缩放/挤压时边框、圆角和模糊跟着一起动，收尾正好落在
窗口自己该在的位置上；`u_alpha` 由这一段统一乘（装饰那趟按满 alpha 画），两边各乘一次就成
了 alpha²、淡得比设定的快。

自带 4 个示例（`shaders/`）：`zoom`（缩放淡入）、`genie`（关闭时压成底边窄带）、
`wave`（正弦抖动）、`round`（圆角，适合做 `shader_idle` 常驻）、`passthru`（恒等，用于验证管线）。

限制：
- 只作用于 **xdg-toplevel 的表面本身**，子表面/弹出菜单不参与变形（弹出菜单仍正常显示，不被扭曲）
- 需要 GLES2 渲染器（启动时会强制 `WLR_RENDERER=gles2`，除非你自己设了别的）
- 窗口关闭时表面已经没了，动画用的是最后一次 commit 的**快照**（我们持锁保住那份像素）
- 每帧要多一次离屏往返（装饰 → 动画）+ 把装饰结果再当贴图导入一次，比内置效果贵；
  `shader_idle` 只在布局变化时重绘

## 拖平铺窗口的落点（niri 的 insert hint）

`Mod+左键` 拖一个平铺窗口时，屏幕上会跟着光标画一块**半透明的橙色预览块**，标的就是
松手之后它会落在哪儿（niri 的 insert hint，颜色也照抄 niri 的默认值 `#ffc87f80`）。

落点只有两种，怎么算是照 niri 来的（`src/layout/scrolling.rs` 的 `insert_position`）：

| | 怎么判 | 落下来的结果 | 预览块长什么样 |
|---|---|---|---|
| 新开一条列（NewColumn） | 光标离**最近的一条列缝**更近 | 在那个缝上插一条新列（原来那条列不动） | 一张 300 宽、整块可用高的「卡片」，骑在那个缝上 |
| 插进某条列（InColumn） | 光标离那条列里某个**窗口缝**更近 | 插进那条列，一列里上下叠起来 | 那条列里的一条横条（贴最上面/最下面 150 高，骑在中间的缝上 300 高） |

「列缝」= 一条列的**左边缘**，而且**左右两边的缝都算**：光标在哪条列的右半边时，离它最近
的那条缝是**它右边那条列的左边缘** —— 也就是「插到这两条列中间」（niri 的 `vert_dist` 量的
是 `column_xs` 里**最近**那一项的距离，不是「光标底下这条列的左边缘」）。列带最右边那条缝
（最后一条列的右边缘 + 一个 gap）也算一条。

两个距离（横向到最近的列缝、纵向到最近的窗口缝）谁小听谁的 —— niri 就是一句
`if vert_dist <= hor_dist`。所以：贴着某条列的左/右边缘、或者列带两头拖 = 新开一条列；
拖到窗口身上、离它的上/下边缘近（列窄的时候得上/下边缘附近，列宽的时候窗口中部也算）= 并进
它那条列。落点里那个槽位数是「把被拖的那个拿掉之后 `剩下这些`」之间的位置：拖自己那条列里
的窗口 = 在自己这条列里换上下顺序，拖到别的列 = 插进那条列的第几位。

拖到别的屏/别的工作区上、落点是「插进某条列」时搬的是**窗口自己**（niri 拖的一直是窗口），
原来那条列里还有别人就留在原处、只剩它一个才把空列拆掉；落点是「新开一条列」时搬的是
**整条列**（vidow 的平铺拖拽动的一直是列）。

### 哪块代码

- `src/drag.c` 的 `insert_pos_at()`：算落点（上面那张表）+ 预览块的位置尺寸；
  `vidow_insert_hint_update()` 把它摆出来（拖的每个指针事件都算一遍，很便宜），
  `vidow_drag_init()` 建那块预览（`layer_toplevels` 里的一块 `wlr_scene_rect`，平时关着）。
- `src/view.c` 的 `vidow_view_insert_into_column()`：插进某条列（同一条列 = 就地换位置；
  别的列 = 从原来那条列里摘出来再插进去）。
- `src/workspace.c` 的 `vidow_workspace_move_view_into_column()`：目标列在**别的屏/别的
  工作区**上那条路（搬窗口自己，不是整条列）。
- 位置一律取**屏幕上看得见的那块**：`vidow_column.screen_x / slot_width` 是 arrange 记下来
  的（全屏那列占一整屏、整列最小化的宽度是 0 这些规矩只有 arrange 知道），窗口的 `geom`
  就是现在这一块 —— 落点这里不重算条带数学。

### 和 niri 对齐的拖动视觉

1. **走够 16px 才把窗口从布局里摘走**（niri 的 `INTERACTIVE_MOVE_START_THRESHOLD`）：
   `view->drag_detached` 让 `view_shows()` 返回假，后面的列当场并拢；落点预览按「剩下的
   这些列」算，被拖窗口原来的空位在拖动期间**不留**。没走够就松手只是一次点击，布局不闪。
   松手才插回去，原来的上下顺序还在（只打标记，不动物理链表）。
2. **被拖的平铺窗口降到 75% 不透明度**（niri 的 `INTERACTIVE_MOVE_ALPHA`），松手恢复；
   拖动期间视口锁在当前值，摘列后条带变短也不会让焦点列把视口拽回去。
3. **预览块画在被拖窗口之下**：被拖的那个抬到预览块之上（niri 的移动窗口也在提示块之上），
   层级靠拖动时抬两次收口（`raise_node()`，已经对了就一个节点都不碰）。
4. **跨屏 / 跨工作区**那条路搬的还是整条列（同屏内落在缝上 / 空白处时只搬被拖的那个窗口，
   和 niri 一样）—— 要整条列换位置用 `move-column left|right`。

颜色是**预乘**的：`wlr_scene_rect` 的颜色直接进渲染通道的预乘混合，所以 rgb 得先乘 alpha
（不乘的话叠在亮色窗口上会变成加法 —— 实测叠在纯红上量到 `(255,200,127)`，乘过之后是 niri
那个 `(127,100,63)`；`test-drag-insert.sh` 用「128,100,63 + 0.498×底下那层」这个式子量的
就是它）。

## 拖窗口时的果冻效果（`jelly`）

拖一个窗口的时候，抓点（光标底下那一点）跟手走，窗口其余部分像一块布一样被拖着 —— 而且
**越靠下越软**：上面跟着手走、下面拖后腿，松手后晃两下收住。KDE 的 wobbly windows 那一套。

```lua
jelly = true            -- 默认开着；false 关掉
jelly_strength = 1.0    -- 形变倍率：0.5 收敛些，0 完全不弯（只留位置跟手）
```

只作用在**移动**拖拽上（`Mod+左键` 拖平铺窗口、拖浮动窗口）；`Mod+右键`改大小不弯 ——
改大小每帧换源缓冲尺寸，网格对不上（见 `src/jelly.c` 顶上那段）。

### 形变是「自己渲染」出来的一遍

wlroots 的场景节点只会把一块缓冲整块贴上去：一张贴图、一个矩形，没法弯。所以果冻不用
窗口本来的那个节点，而是：

1. 拿窗口**现在显示的那块装饰离屏图**（fx 节点正在显示的缓冲，带边框/圆角/阴影）当源。
   果冻期间 `vidow_fx_frame` 把 `look.blur` 关掉，所以这张源里**不烘模糊**；
2. 把它按网格**一格一格**贴进一块自己的缓冲，每格的落点由弹簧状态算出来；
3. 模糊单独一层：同一份网格，内容 UV 跟着网格走，模糊 UV 按顶点**当前屏幕坐标**
   映射到 blur 贴图 —— 形变只拉内容，背景不被网格拉长；
4. 用一个场景节点显示这块缓冲，同时把窗口本来的装饰节点透明度压成 0（`fx->jelly`，
   跟抓帧遮挡一样是标志位 —— `vidow_fx_frame` 每帧都会把透明度写回去）。

源和结果都是同一块缓冲里切出来的，所以交接时**内容一模一样**，看不到接缝。

模糊那一层用 `mesh_blur` 着色器一遍合成：`content + blur * (1 - content.a)`，和装饰
着色器最后那条公式一致；内容不透明的地方看不到模糊，半透明的地方透出的是**屏幕上
那块背景**，不是被果冻拉长的旧图。模糊还按**窗口本身的区域**裁一刀：内容 UV 里的矩形 + 客户端表面的 alpha。
果冻期间投影仍烘在窗口内容图里，和窗口共用同一张网格，**形状完全跟着窗口弯**，
并且位于窗口下面；模糊只按内容矩形 + 客户端表面 alpha 裁，投影不进模糊。`test-jelly.sh` 有一段专门拿「底下四象限、上面
半透明」的窗口往左拖：左半边的红色必须在，右边才是绿的 —— 采样若跟着窗口走，左边
也会是绿色。

### 弹簧：抓点钉死，「该落后多少」按行号算

每个网格节点是一根**独立**的弹簧振子，但它该落后多少不是靠「拖拽力」推出来的，而是
**按行号算出来**的（顶行 0、底行 1）：

```
l_i' = (base - l_i) / τ_i          底座位置按各自的 τ 滤一遍（跟得有多慢）
d_i  = -(base - l_i)              该落后多少 = 底座走到哪 − 它以为底座在哪
o''  = -ω²·(o_i - d_i) - c·o_i'   弹簧把偏移拉向 d_i
```

- `τ_i = τ_min + (JELLY_TAU - τ_min) · g_i`（`g` = 行号 / 总行数，0 = 顶行）：越往下
  跟底座跟得越慢、落后越多 —— 顶行几乎不落后、底行落后 `V·τ` 最多，中间线性过渡。
  看着就是「上边被揪着走、下边垂着」的一块布。
- `ω = ω0 / √(1 + GRADE·g)` 决定晃的频率（顶行 19 rad/s ≈ 3Hz，底行 ≈ 1.7Hz ——
  频率也渐变，收尾时是一道从上往下荡出去的波），阻尼比 ζ = 0.35 决定弹两下收住。
- **梯度只看行号，跟抓点在哪儿无关**：抓标题栏还是抓底边，该软的地方一样软。
  早先是按「到抓点的距离」算的（抓哪儿哪儿硬、向四周散开），拖起来像被揪着一个点甩，
  而且抓点一换整块布的软硬分布就跟着变 —— 手感飘，抓底边时梯度甚至是**反的**
  （顶行落后最多）。`test-jelly.sh` 拿这个当判据：抓标题栏和抓底边，顶部都必须在前。
- 抓点那个节点偏移钉死为 0：抓点永远精确在光标下面，不会有「整窗弹性跟随」那种输入
  延迟。窗口位置本身照旧归布局管（`drag.c` 一个字都没改），果冻只加形变 —— 所以拖到
  哪一列、跨不跨屏这些语义一点没变。

**为什么不给每个节点加一个「拖拽力」**：那样每个节点受力一样，匀速拖动时整张网**一起**
平移（形变是均匀的，看着像整窗拖后腿而不是果冻），而加速的那一小段时间里软节点反而
落后得**少**（响应慢）—— 梯度是反的。实测截图里就是这样：左边一条竖边上下只差 1px。
按距离算「该落后多少」则天然渐变，方向也对。

一开始试的是 KDE 那种「相邻节点弹簧」的真网格，撞了两个坑：想让形变铺满整窗就得让
回正弹簧很软，而软回正 + 硬相邻 = 要 1/8000 秒的子步才不发散（1/240 直接炸）；而且
它在匀速拖动下的形变几乎就是均匀平移，只在抓点周围一小圈里有梯度。改成「每节点独立
+ 按距离给滞后」之后：每个节点都是一根独立的二阶振子（ω ≤ ω0，1/240 秒的子步绰绰
有余），数值上稳，形变天然渐变。

调参口诀：`JELLY_TAU` 管底行落后多少（22ms → 1500px/s 的拖拽落后约 33px）、
`JELLY_W0` 管晃的频率、`JELLY_ZETA` 管弹几下收住、`JELLY_GRADE` 管「越往下越软」的
程度。

### 什么时候交接（松手之后）

松手时解掉抓点（`pin = -1`），整张网自己晃回去。位置照旧归布局 —— 该插到哪一列、跨不跨屏
都不受影响（`test-drag.sh` 那些落点判据全绿）。

交接的判据：所有节点都静下来（偏移 < 1px）**或者**松手超过 700ms（之后偏移按 60ms 的
时间常数指数收掉）。收尾必须是「晃到 0 再交接」，不然窗口会「啪」地跳一下 —— 交接时
装饰节点重新露出来，内容一模一样，所以看不到接缝。

**踩过的坑（一）**：交接后窗口会整个消失。帧循环只在「模糊底衬变了」的时候才去跑
`apply_anim`，光排一帧是不够的 —— 装饰节点的透明度会一直停在 0 上。现在 `jelly_free()`
里当场跑一次 `vidow_view_apply_anim()`。

**踩过的坑（二）**：2 倍屏上拖起来整块窗口偏 72 逻辑像素、松手才回位（看着像「拖拽时窗口
坐标偏移」，半透明窗口还会跟着「背景错位」—— 整张图连烤进去的模糊底衬一起偏）。原因是
那个场景节点的位置：**场景图是逻辑像素**，而形变缓冲四周那圈留白是**设备像素**，节点
左上角该减的是 `JELLY_MARGIN`（逻辑）而不是 `margin_px = JELLY_MARGIN × scale`。1 倍屏上
两者相等，所以只有 HiDPI 会露出来 —— `test-jelly.sh` 现在专门有一段 2 倍屏的判据
（按住不动，窗口位移必须是 0 设备像素），外加一段半透明 + 模糊窗口的逐像素比对。

### 网格分辨率

格子按 `JELLY_CELL`（40 逻辑像素）定，每轴最多 24 格。这个数不是随便定的：相邻两行的边界
各自取整之后会差 1px，一格就是一条**台阶** —— 网格太粗（64px）时高对比的窗口内容上能看
出一条明显的锯齿（截图量过：一条竖缝上 10 级台阶），40px 之后看不出来了。

一帧的成本：`min(24, 宽/40) × min(24, 高/40)` 次贴图绘制（654×724 的窗口是 16×18 = 288
次），实测 1080p 下拖动期间帧间隔 16~19ms（60fps 不掉）。

物理在**逻辑像素**里算，渲染时乘输出倍率，所以 HiDPI 上手感一样。

### 什么情况下不做

- `decor_enabled = false` 且没有任何 shader：窗口没有装饰离屏图（fx 节点），没东西可弯
- 窗口的入场动画还在缩放（`anim_scale != 1`）或正在 overview 里：装饰节点的位置尺寸每帧
  都在变，网格对不上
- 拖动中途源换了尺寸（窗口改了大小 / 输出换了倍率）：直接收掉，不交接
- 形变用**自己的网格着色器**画（每格两个三角形、相邻格共用顶点，节点坐标不取整）：
  老实现是「一格一块轴对齐矩形」（`wlr_render_pass` 只能贴轴对齐的矩形），剪切形变
  下相邻行各自取整 —— 屏幕上窗口像好几条矩形拼的、左边缘一段一段跳，透明窗口那层
  模糊透过缝看就是「错位的竖线」
- 抓底衬（装饰的模糊）时这个窗口的**果冻节点也要藏起来**：它是窗口内容的另一份拷贝，
  只关装饰节点的话，底衬里会留着它自己上一帧的形变图（透明窗口一拖就是重影 / 错位，
  模糊看着像没了）

### 测试

```bash
./test-jelly.sh      # 起手/形变（梯度只看行号）/交接/开关/2 倍屏不跑位/半透明底衬对位，
                     # 判据是截图里的像素；另有三段：**左边缘逐行步进**（网格是三角形拼的、
                     # 相邻行最多差 1px —— 一格一块矩形时是一段一段跳的 3px）、
                     # **抓底衬不许混进被拖窗口自己**（按 dump 的 owner 判，绿色自己的
                     # 底衬里不许有绿色；品红底衬里可以带绿色的屏幕空间模糊）、
                     # **模糊按屏幕坐标采样**（四象限底图 + 半透明薄纱，拖到左红右绿）
./test-geometry.sh   # 窗口几何比表面大（chromium 改大小的中间态）：框不动、内容铺满
```

## 代码结构

```
src/server.h   共享类型（view / column / output / layer / keyboard）与前向声明
src/main.c     启动：display/backend/renderer/scene/seat/layer-shell/vkbd/vptr、socket、事件循环
src/output.c   output 创建、frame 提交、request_state、usable_area 初始化、动画/捕获看门狗
src/view.c     列模型、滚动布局、焦点/移动/改宽/列最大化（vidow_column_set_maximized，
               客户端 set_maximized 也走它）、激活（vidow_view_activate：先把窗口那块
               工作区端出来再给焦点）、窗口生命周期（xdg-toplevel 和 X11 两种壳，差别收在
               vidow_view_* 访问器里）、X11 缓冲区倍数声明、popup
src/float.c    浮动模式识别与放置、交互式移动/缩放 grab
src/layer.c    layer surface 生命周期、层级树、exclusive zone、键盘独占、出现/消失动画
src/decoration.c zxdg_decoration_manager_v1：按 decor 配置告诉客户端「装饰谁画」。
                  不给这个全局，kitty / GTK / Qt 就自己画标题栏，跟我们的圆角打架
src/input.c    键盘/指针/滚轮/拖动、虚拟键盘与虚拟指针、光标、剪贴板（键位不在这里：
               一个都不内置，全由 config.lua 的 vidow.bind() 声明）
src/config.c   配置键表（键名/类型/范围/分节）+ 默认值 + VIDOW_* 环境变量覆盖 + 配置「族」
               的表写法 + 默认配置按类别拆成 configs/ 那一套（--default-config / --write-config）
src/scale.c    缩放计算（纯函数，不碰 wlroots）：ppi→scale、量化夹取、每输出覆盖、
               优先级。tools/scaletest.c 直接拿这份源码编译来自检
src/hotplug.c  热插拔的纯逻辑（不碰 wlroots）：输出身份、记忆台账、拔线时挑哪块屏
               接收、把一组列稳定挪到末尾。tools/hotplugtest.c 拿它离线自检
src/rule.c     窗口规则：app_id / 标题 / X11 class 子串匹配，作用是 force_render（视口外
               也补帧回调）、hz（这个窗口的补帧节流）、focus_on_open（开窗口抢不抢焦点）、
               focus_on_activate（激活请求理不理）、honor_invalid_serial（无效 serial 的
               激活请求也理）。建窗口、标题变化、SIGHUP 重载时套用
src/workspace.c 工作区：每块屏各自亮一块、切换时只翻场景节点的可见性（窗口不销毁）、
               特殊工作区（不绑屏 / 不进列表，跟随聚焦的屏）、按名字跨屏搬家
src/keybind.c  键位绑定：解析 "Mod+Shift+1" 这类触发串、命中就执行动作字符串
               （动作 = 字符串开放集合，workspace / move / special / focus / …）
src/overview.c niri 风格全景：世界→屏幕的缩放变换、相机（跟焦点 + 滚轮平移）、压暗遮罩
               （颜色可配）、进出动画的方向插值（anim_from）、点选/退出。窗口那一侧由
               view.c 的 vidow_view_apply_anim 套用
src/xwayland.c XWayland：懒启动服务、seat、xwayland_shell 只给 Xwayland 自己、
               X 全局缩放（= ceil(最密那块屏)）、X11 客户端环境（GDK_SCALE 等）
src/exec.c     异步 `sh -c`：pipe + wl_event_loop_add_fd + 共享超时定时器，绝不阻塞渲染
src/script.c   配置语言：Lua 5.4，协程化的 vidow.exec、全局名→配置键、vidow.include()
               （一类一份配置就地展开）、SIGHUP 重载、vidow.on() 的事件钩子（Lua 那一半）、
               vidow.env{} / vidow.exec_once() / vidow.exec() 的 wait / status 选项
src/hooks.c    事件钩子：config.lua 里 vidow.on() 声明的那些（命令 / Lua 两种），事件
               *名单*（vidowmsg events 就报它）、统一入队 + idle 派发、命令钩子的环境变量
src/env.c      会话环境账本：和 systemd 用户管理器两个方向对账（吸 / 推 + 退出还原）、
               vidow.env{} 改过的变量记着原值、VIDOW_SYSTEMD_ENV{,_EXPORT} 开关、
               只碰 systemctl --user 够得着的那个管理器（嵌套 / 测试里够不着就整个跳过）
src/configwatch.c 配置自动重载：inotify 盯目录 + stat 比对，盯的是「配置真正读了哪些文件」
               （入口 + 每一份 include 的），改哪一份都重载
src/anim.c     动画引擎：单调时钟进度、缓动函数、每帧 tick
src/fx.c       SSD 装饰着色器、kawase 模糊链（共享/私有 backdrop + 世代缓存）、离屏渲染、关闭快照
src/im.c       输入法中继：text-input-v3 <-> input-method-v2、候选框摆位、键盘抢占用与回注防环
src/pointerconstraint.c 指针约束：zwp_pointer_constraints_v1 + zwp_relative_pointer_manager_v1
               （游戏锁鼠标 / 夹光标要的那两个）。wlroots 只给协议对象，钉住 / 夹住光标是合成器
               的活：跟随指针焦点激活、locked 钉住位置 + 相对位移照发、confined 用
               wlr_region_confine 夹进区域、cursor_hint 只在锁生效和解除时用
src/keyboardgrab.c 键盘那两个协议：zwp_keyboard_shortcuts_inhibit_manager_v1（wlroots 有对象，
               策略是合成器的活：焦点那个表面抑制着时自己的键位全部让路，`{ inhibited = true }`
               的绑定是逃生口）+ zwp_xwayland_keyboard_grab_manager_v1（wlroots 没实现，服务端
               自己写：X11 的 XGrabKeyboard → 按键一律送给抓着键盘的那个表面，对象没了焦点还给
               焦点窗口；只服务 Xwayland 的表面）
src/bgeffect.c ext-background-effect-v1 服务端（wlroots 没实现）：区域双缓冲、按区域裁模糊
src/activation.c xdg-activation-v1 服务端**自己实现**（不用 wlroots 那份）：令牌生命周期
               （按 activation_token_timeout 作废、客户端一断就作废）、serial 校验结果怎么
               用由 honor_xdg_activation_with_invalid_serial 定、activate 里带的 surface
               才是要顶的那个 —— 详见「为什么 xdg-activation 是自己实现的」
src/ipc.c      IPC 的传输与命令：监听 socket、每连接一个缓冲区、一行一条请求/应答、
               把命令派发到合成器功能上（查询 + 操作）、事件广播给订阅者
src/ipcstate.c IPC 的状态视图：把内部状态序列化成 JSON（窗口/列/工作区/输出/layer/配置/
               输入/令牌/动画…）、组装事件信封。只读，不改状态
src/json.c     极简 JSON（纯 C，不碰 wlroots）：写出（jb_*）+ 读入（jparse），tools/jsontest.c
               直接拿这份源码编译来自检。IPC 的每条消息都从这儿过，所以它自己也有测试
src/clipboard.c X11 <-> Wayland 剪贴板桥（自己接管，不用 wlroots 那份带焦点闸门的）+ 历史
                （按 mime_rank 留前 6 个类型、去重挪到最前、超条数/字节从最旧的淘汰）：
               拦 xwm 连接上的选区事件、两边各留一份内存副本、INCR 双向 —— 详见「剪贴板」
src/portal.c   portal 要的协议：xdg-foreign v1+v2（共用 registry）、
               foreign-toplevel-management、ext-foreign-toplevel-list + 截图源
               （xdg-activation 在 activation.c）
src/screencast.c  zkde_screencast_unstable_v1（KWin 私有协议，KDE 的 portal 和 vidow
               自己的 portal 都靠它要流）+ 合成器侧的 PipeWire 生产者
               （见 README 的「截图 / 录屏」）
src/portalconf.c  portal 后端选择：内置默认 → ~/.config/vidow/portals.conf 逐键覆盖
               → 同步到 portal 的配置目录；录屏 / 截图后端按 portal_capture 挑
               （auto 优先自己那个 vidow-portal），顺手写 vidow.portal、把它起起来；
               启动时把环境 import 进 systemd 用户管理器、退出时还原（先快照）
shaders/       示例片段着色器（zoom / genie / wave / round / passthru）
tools/vidowportal.c  vidow 自己的 xdg-desktop-portal 后端（ScreenCast）：GTK3 选择器
               （每块屏一行实时缩略图，zwlr_screencopy 抓帧）+ 跟合成器要 PipeWire 流
               （zkde_screencast）；`--probe` 自检、`--dialog` 直接弹选择器看排版
tools/vdrag.c  虚拟指针拖拽测试工具
tools/xpopup.c 按绝对坐标造 X11 窗口（owner / or / tooltip / menu / combo / utility），
               可指定 WM_TRANSIENT_FOR、晚设窗口类型、自声明固定尺寸、延后自己挪位 ——
               弹出物分类与定位的测试客户端
tools/xnomap.py 造两个 X11 窗口跑同一套属性动作（声明成对话框 → 删掉类型，逼 vidow
               把「浮动 → 平铺」重判一遍）：A **一次都不 map**、B 正常上屏当对照 ——
               盯「没上屏的窗口不许占列」（见 249 / `./test-emptycol-nomap.sh`）
tools/scaletest.c  缩放计算的离线自检（解析 / ppi 推算 / 量化 / 优先级 / 物理尺寸差 /
                   X 屏幕对账）
tools/hotplugtest.c 热插拔纯逻辑的离线自检（身份 / 记忆 / 挑接收的屏 / 稳定重排）
tools/jsontest.c  JSON 写出 / 读入的离线自检（转义 / 嵌套 / 代理对 / 各种拒绝路径）
tools/vidowmsg.c  IPC 客户端：把命令行参数拼成一行 JSON、把应答排版打出来；
                  watch 订阅事件。纯 socket，不依赖 wayland
tools/frames.c    帧回调计数器：只按合成器的许可出帧，每帧打印一行（用来量「客户端
                  还在不在出帧」—— 窗口滚出视口后有没有停帧一眼看出来）
tools/capture.c   窗口录屏 / 预览的最小客户端：ext-image-copy-capture + 窗口截图源，
                  每抓一帧打印像素校验和（不变 = 录到的是冻住的一张）
tools/activate.c  激活请求客户端：一个窗口 + 一次 xdg-activation（令牌上屏时创建、N 秒后
                  才用它请求激活；serial 可选「点击事件 / 0 / 不带」三种）
tools/popup.py    最小 Wayland xdg_popup 客户端（窗口 + Gtk.Menu）：专门撞
                  「unconstrain 只能等 popup 第一次 commit」这条断言
tools/preinit.c   状态请求抢跑客户端：surface 还没 initialized（一次都没 commit）就发
                  set_maximized / set_minimized / set_fullscreen；`--after-commit` 是
                  对照组（上屏之后再发）—— 专门撞 schedule_configure 里那句
                  `assert(surface->initialized)`
tools/vidowclip.py 剪贴板 GUI（GTK3）：状态 / 历史 / 预览 / 拿回来 / 热开关，
                  走的是和 vidowmsg 同一条 IPC；`--check` 不建窗口只走 IPC
tools/pattern.py  四象限测试窗口（验证 shader 管线方向与颜色）
tools/glass.py    半透明定尺寸窗口（验证窗口透明度与背景模糊）
                   VIDOW_TEST_EMPTY=1 时不画内容，只剩装饰 + 模糊
tools/wall.py     layer-shell 背景（渐变 + 硬边方块），给模糊测试提供可模糊的内容
tools/bluewin.py  一个纯蓝的普通窗口：跟 tools/panel.py 一起验证「top 层模糊里含不
                  含窗口」（壁纸是灰的，蓝得扎眼，量 B-R 就行）
tools/bpanel.py   一块 bottom 层 50% 红面板：盯「被模糊的 layer 自己进了底衬」
tools/padpanel.py top 层「面板」，向外留 40px 全透明内边距：盯模糊会不会漏进元素自己
                  透明的部分（`test-layermask.sh`：修前 1.000 → 0.322，修后原样 1.000）
tools/fakeim.c    假输入法（连 input-method-v2，抢键盘，每个按键 commit 一段文字），
                  用来验证中继链路，不依赖 fcitx5 的配置
tools/bgblur.c    半透明窗口 + 指定模糊区域，用来验证 ext-background-effect
tools/foreign.c   xdg-foreign 导出端：打印 v1 / v2 两个 handle，给 portal 当 parent_window
tools/foreign-app.py  GTK4 客户端版本（export_handle + set_transient_for_exported），
                  验证真实客户端的 xdg-foreign 路径
tools/cursorprobe.c 当 screencast 流的消费者，把缓冲里的 SPA_META_Cursor 读出来
                  （GStreamer 不解这个元数据）：验证 pointer_mode=metadata
test-portal.sh    私有 dbus 里起 portal + 真实 FileChooser 请求
                  gtk/kde 验证协议链路，switch-gtk/switch-kde 验证配置生成链路
test-portalenv.sh 假 systemctl 记下每次调用：启动 import 环境 / 配置没变也 import /
                  退出把环境还原（原来没有的就 unset）/ NO_RESTART 不碰用户管理器 /
                  auto 优先自己那个后端并写 vidow.portal / 没有自己人时退回探测外部后端 /
                  portal_capture 写死与 none / 前端环境不对就重启它（查 MainPID）
test-portal-own.sh vidow 自己的录屏 portal（不依赖 KDE）：vidow.portal + portals.conf
                  落地 → vidow-portal 占住总线名 → 前端把 ScreenCast 指给它 → 选择器
                  对话框画在 vidow 上 → 点「共享」→ Start 返回 node → gst 消费者拿到会变的帧；
                  窗口 / 虚拟输出 / 截图取色 / 光标元数据（含缩放 2 倍的坐标换算与 hidden 反证）
                  —— 窗口那一轮的流尺寸要和 portal 报的一致（消费者按报的尺寸协商时，
                  对不上就是一帧都拿不到，见 172 的 ⑥）；窗口 + metadata（OBS 那个组合）
                  那轮光标元数据要有 id=1 + 位图 + 位置落在流的中间（见 173）（47 项）
test-screencast.sh KDE 的录屏 portal（portal_capture = "kde" 那条路）：vidow 暴露 zkde_screencast_unstable_v1 →
                  XDG_CURRENT_DESKTOP=KDE 时后端才注册 ScreenCast → 点选择器 →
                  Start 返回 PipeWire node → gst 消费者拿到会变的帧
tools/screencastprobe.py  发一次真的 ScreenCast 请求（CreateSession → SelectSources
                  → Start），把每步的 Response / 流尺寸 / node id 打出来；
                  --hold N 拿到流后别退出（留时间让消费者连上来）
test-dpi.sh       1x + 2x 两块 headless 屏，量 X11 / Wayland 客户端的缓冲区与
                  落屏设备像素（只准缩小不准放大）、X 全局缩放、指针命中坐标
test-xwayland-popup.sh  弹出物：识别（含属性晚到）、绝对定位、不套 toplevel 样式
                  （装饰缓冲 / 像素级无阴影 / 无动画）、不抢焦点、层级、混合 DPI
test-x11scale.sh  固定尺寸的 X11 窗口（WM_NORMAL_HINTS min==max）在 2 倍屏上的
                  逐窗口缩放桥：不带规则 300x300 → 150x150（面积 1/4），
                  vidow.rule{ x11_scale = 1 } → 300x300；命中坐标两边各自对得上
test-scrollcull.sh  两块并排的屏 + 三列装不下：滚出视口的列被停到屏外（修之前它的
                  坐标落进隔壁屏的框里，就画在隔壁显示器上），再滚回来又显示，
                  浮动 / 拖拽不受这条规则管，藏起来的窗口按规则照旧补帧；半块越界
                  的那种列只画自己那块屏上的一半；**挂着窗口截图源（选择器预览 /
                  OBS 录屏）时隔壁屏照样一个像素都没有，同时录屏那头拿得到新画面**
                  —— 三种顺序都测（先挂再滚出去 / 已经在屏外才挂 / 工作区被藏起来），
                  挂着截图源的半块越界窗口也不串屏；**静止的窗口滚到视口外照样出帧**
                  （按损伤出帧的老毛病：3 秒只有 1 帧 → 89 帧）
test-outputstate.sh 每块屏那一组的其余项：`mode`（分辨率 / 刷新率）、`enabled`、
                  `vrr`（headless 上不支持 → 只报一句，别的照常生效）、`depth = 10`
                  （输出缓冲真的换成 XRGB2101010）、`transform`，SIGHUP 后每项都跟着
                  改，写错的值当场报错；色彩能力（`primaries` /
                   `transfer_functions` / `hdr_capable`）如实上报 —— headless 什么都不报
test-outputpos.sh  显示器配置（`vidow.output{ name = …, scale = …, x = …, y = … }`）
                   按名字摆（自动排的顺序是后端给的，headless 上实测是反的）、
                  只写 x 不写 y 也认、项名写错当场报错、拔插后回到配置说的位置、
                  SIGHUP 改配置立刻生效
test-dmabuf-update.sh
                   dmabuf 客户端（Qt/KDE）的新画面必须上屏：装饰管线取客户端贴图要借
                   wlr_client_buffer 自己那张，对 source 另建的那张对 dmabuf 客户端
                   会一直是旧内容（kate 打字后屏幕 0 像素变化 → 现在 ~949）
test-frametrace.sh  帧追踪：某块屏的帧链断了（page-flip 不回来）—— 那块屏上的窗口全停在
                   最后一帧、拖到另一块屏就活。headless 上用 VIDOW_TEST_STALL_OUTPUT
                   把帧事件按住复现，判据是 VIDOW_TRACE_FRAMES=1 的那行 ERROR（点名屏、
                   要过几帧、谁要的、客户端还在交帧）
test-preinit.sh    状态请求抢跑：客户端在第一次 commit **之前**就 set_maximized /
                   set_minimized / set_fullscreen（xdg-shell 允许），合成器不许在
                   `assert(surface->initialized)` 上 abort；抢跑的最大化**兑现**成列最大化
                   （第一个 configure 就带 maximized，见 changelog 177）、抢跑的最小化真的
                   收起来、抢跑的全屏**兑现**（两边状态一致）
test-minimize.sh   最小化：窗口留在原列里（不占位 / 不显示 / 不参与焦点），条带合拢、
                   同列剩下的撑满、焦点交给邻居、放回回原位、浮窗也认、整列收起、
                   客户端自己请求（`xdg_toplevel.set_minimized`）、`force_render` 的窗口
                   收起来照样出帧、SIGHUP 不改状态、X11 的 `WM_STATE` Iconic ⇄ Normal
test-widthanim.sh  列宽过渡（`column_width_ms`）：`cycle-column-width` / 列最大化 / IPC 改宽都是
                   **补间过去**（逐帧量到中间宽度、单调、落在目标像素、右边那条列每一帧
                   都贴住左边），过渡期间**不给隔壁列起位移补间**（不然它吊在后面）；
                   `column_width_ms = 0` 直接到位、改配置真的改时长；拖动改宽不补间
                   （跟手那条由 `./test-tile-resize.sh` 兜）
test-maximize.sh   列最大化（niri 的 `maximize-column`）：整列撑满工作区宽度、再按一次还原成
                   **原来那个比例**（0.25 改了再最大化再还原还是 0.25）、手动改宽度就是退出
                   最大化状态、`maximize-column` 动作名；客户端 `set_maximized` 映射到它 ——
                   上屏之后请求的、第一次 commit **之前**抢跑请求的（第一个 configure 就带
                   `maximized`，合成器不许在 wlroots 那句 assert 上 abort）、**用户**按出来的
                   最大化客户端随手撤销撤不掉、客户端自己要来的撤销就理；`column-maximize-changed`
                   事件、浮窗当场报错、X11 的 `wmctrl` 请求与 `_NET_WM_STATE` 来回切
test-input-inject.sh 输入注入：`move-cursor` / `pointer-button` / `pointer-scroll` /
                  `send-key` / `type-text` 合成的事件**真的到客户端了**（拿 wev 当观察者）：
                  窗口内坐标、相对移动、连点、滚轮的 value120 与连续滚动的区别、
                  `ctrl+shift+t` 的顺序、`Hi!` 里每个字符的 Shift、注入 super+t 不触发
                  绑定但客户端照样收到、按住 super 滚轮不转发给客户端、
                  `input_injection = false` 之后全部拒绝
test-pointer-lock.sh 指针约束（游戏锁鼠标 / 夹画笔要的那两个协议）：合成器日志里两个 global 都
                  在、客户端 bind 得到；锁定时客户端收到 `locked` + 相对位移（`reltotal 120.0
                  80.0`）而合成器光标和客户端的绝对坐标都不动（钉住）；夹住时把光标往区域外推
                  400,400，客户端停在区域中心、合成器光标停在区域里、相对位移照发；
                  锁之前给的 `cursor_hint` 在锁生效时把光标摆过去；X11 那边 `XGrabPointer`
                  之后 IPC 里真的出现一个约束（就是「Xwayland bind 上了协议」的直接证据），
                  光标钉住的同时 X 客户端照样收到位移；客户端退出后约束解除、光标能动
test-keyboard-grab.sh 键盘那两个协议（虚拟机的窗口要原样收 Super / Alt+Tab 那件事）：对照组
                  （同一个窗口、没有抑制）Mod+F 的键位生效、窗口全屏；客户端要了抑制之后
                  IPC `shortcuts_inhibited = true`、同一个 Mod+F **不再生效**而按键到了客户端
                  （keycount 涨）；配置里 `{ inhibited = true }` 的那条绑定在抑制期间照样生效
                  （逃生口）；客户端销毁 inhibitor → 回到 false、键位又生效；X11 那边
                  `XGrabKeyboard` 之后 IPC `keyboard_grab = {type: xwayland, window}`（就是
                  「Xwayland bind 上了协议」的直接证据），**把焦点点回 Wayland 窗口**之后按键
                  还是送给抓着键盘的 X 客户端（X11 的语义：谁抓谁收），撤销抓取后焦点和按键
                  都还给焦点窗口
test-cursor-shake.sh 摇晃找光标：来回晃一会儿才触发 / 慢速不误触 / 顺手一甩不触发 / 到点缩回（补间）/ 线性加
                  （2→4→8→16→32→45，屏幕尺寸封顶）；**截图**由像素作证 —— grim 不带 -c
                  时画面里没有光标（放大到 450x720 也没有），`grim -c` 抓到的外接矩形是
                  放大后的尺寸且箭头尖还在原点；软光标那条路（`VIDOW_TEST_SOFT_CURSOR=1`）
                  单独验「抓帧让位」；客户端自己设的光标放大后还原的还是它自己那张；
                  放大中**切到另一个窗口**（新客户端 set_cursor）不重置放大、放大图跟着换成新的
test-geometry.sh  窗口几何（xdg_surface.set_window_geometry）比表面还大时的中间态：窗口框留在
                  声明尺寸上、内容铺满框（老行为是被 clamp 成「画面小一圈、右边一条糊边」；
                  靶子 tools/geowin 左红中绿右蓝 + 正中一条白线，白线位置就是判据）
test-xwayland-hidpi.sh  XWayland 的 HiDPI 告知：`_XWAYLAND_GLOBAL_OUTPUT_SCALE`
                  让 X 屏幕 / RandR 变成设备像素（补丁生效时），`Xft.dpi = 96×S`，
                  以及没打补丁时量到屏幕没跟着缩放、自动退回 1 倍（钩子模拟），
                  端到端点一下 xev 收到的 root 坐标 = 布局坐标 × S，改缩放跟着变
test-vt.sh        tty 的 VT 切换：headless 下验「认得 Ctrl+Alt+F2（F1..F12 + Ctrl+Alt
                  就是 VT 键）、不接管、不吃键」，`switch-vt` 动作 / 越界号优雅失败，
                  config 里绑的 switch-vt 走同一条路（真机上生效的就是它）
test-clipboard.sh 剪贴板桥：接管、X11->Wayland、Wayland->X11、PRIMARY、INCR（大块）、
                  错误路径与事件、配置热切换（关掉退回 wlroots 那套）、退出与清理，
                  外加历史（去重 / 淘汰 / 拿回来 / 删除 / 关掉当场清空）、文件复制
                  （uri-list + gnome-copied-files 逐字节核对）、`vidowclip.py --check`
test-hotplug.sh   真拔一块 headless 输出再以同名插回来：列 / 浮动窗口 / layer surface
                  的归属与还原（顺序、列宽、滚动位置、平移量、四象限像素）
test-layerwin.sh  top / overlay 层的模糊里必须含窗口（纯蓝窗口 + 压在它上面的面板，
                  量面板那块截屏的 B-R 与区域底衬 dump）
test-overview.sh  Mod+O 全景：缩放比例/位置/顺序、四象限像素、遮罩只压背景不压窗口、
                  Esc 退出后画面还原、退出时 t 单调不反弹、遮罩配色（默认/不压暗/染色）、
                  壁纸模糊（硬边摊开）、点选、两块屏时只看当前那块、
                  全景另一张壁纸（规则 overview = "only"：桌面不画 / 全景才露 / 热改当场生效）
test-background.sh
                  桌面背景归谁画：没壁纸客户端时整屏是 background_color（像素）、
                  查询的 clients / covered / surface 几何、壁纸客户端退出后回到兜底色、
                  background-changed 上屏下屏各一次（载荷带 clients）、background_color 热改
test-mru.sh       Alt+Tab 的最近窗口切换器：台账顺序、按住期间焦点不动、连点往前走、
                  松开提交、Esc 取消、快速点一下（按住 < open_delay）不显形但照样切窗口、
                  strip 装不下横向滚且选中的完整可见、grid 换行 / 每行上限 / 装不下整体
                  缩小、面板显形时壁纸被压暗（像素）、每项背后的卡片（几何 / 相邻不重叠 /
                  选中那张描边是强调色、其余是卡片描边色，都量像素）、鼠标悬停点亮底下那张
                  （选中项不动）、鼠标点击当场切窗口
portals.conf.example  拷成 ~/.config/vidow/portals.conf
protocols/     wlr-layer-shell / wlr-virtual-pointer / input-method-v2 /
               ext-background-effect-v1 / xdg-shell / xdg-foreign v1+v2 协议 XML
               （构建时用 wayland-scanner 生成头文件）
test-viewport-crop.sh
                  客户端的 wp_viewport 只声明了缓冲的一部分（chromium / KDE 在分数缩放下
                  会把缓冲开得比窗口大一圈，只画其中一块、src 框指过去）：装饰必须按场景
                  节点的 src_box（缓冲像素）采 —— 按整块缓冲采，那圈没画过的像素会一起
                  铺进窗口，屏幕上就是「框到位了、画面小一圈、右边/下边一条黑边」，而且
                  会一直那样。没装 chromium 就跳过
test-blurlive.sh  壁纸 + 两个互相重叠的静止浮窗 + 屏幕另一头一个**不停重画**的窗口
                  （tools/spin.py，每 16ms 一帧、内容每帧不同，模拟视频 / 浏览器）。判据：
                  那个窗口真的在重画（两张截图 6.7 万像素不同）、2.4 秒里渲染 ≥20 帧、
                  **重叠窗口的私有底衬重建 < 30 次**（修前每帧 2 次 ≈ 296 次，把损伤区域
                  那条判据退回「只看世界代次」跑同一条 → 296 次当场红）、vidow 活着
test-blursmall.sh 壁纸 + 一个半透明浮窗 A（它背后是模糊，模糊里画着 B）+ 跟它重叠的浮窗 B
                  （tools/spin.py small：每帧只让窗口里 80x40 那一小块失效，模拟「只有一条
                  进度条 / 一个光标在动」）。判据：A 真的建过私有底衬（前提）、B 真的只动了
                  那一小块（两张截图的变化像素全在它窗口框里、只有几千个）、**那一小块在 A
                  的区域外时 A 重抓 0 次**（修前按整块窗口算：155 次 / 154 帧，退回源码跑
                  同一条 → 155 次当场红）、同一小块挪到 A 的区域里时 A 确实重抓（167 次 /
                  186 帧，别「优化」成漏账）、vidow 活着
test-blurpartial.sh
                  **同一个进程、同一个场景**里比对「局部重跑」和「整块重跑」：壁纸 + 半透明
                  浮窗 A + 压在 A 上的浮窗 B（tools/spin.py step：每帧只让窗口里 80x40 那一
                  小块失效，内容什么时候变由测试控制 —— SPIN_STEP_FILE 一出现就变一次、
                  之后一直不动）。先让 A 一直走局部重跑、截一张，再建出
                  `VIDOW_BLUR_NO_PARTIAL_FILE` 指的文件（下一次重抓就整块重跑）、再截一张。
                  判据：切换前 A 每帧都在重抓且日志写着「局部重跑」、画面静止后每次动的块
                  都 ≤ 400 像素（实测平均 13776 像素 = 164x84，整块区域 118 万像素，86 倍）、
                  切换后全是整块重跑、**两张截图逐像素比对差 > 2 的像素 0 个**（把链条那几趟
                  的采样半径改成 0 再跑一遍：216 个像素差 > 2，当场红）
test-blurcache.sh 拖一个和别的窗口**重叠**的浮动窗口时，模糊金字塔每帧都要重建（另一个窗口
                  在它的底衬里动），但金字塔的缓冲和贴图都得复用：老写法链条每一趟
                  `wlr_texture_from_buffer()` 导入一张、用完销毁，真后端上（GBM/dmabuf）
                  每次都是一次 EGLImage + BO 映射 —— 基准实测每帧 23 次。判据看
                  `VIDOW_RENDER_STATS=1` 那行统计：拖拽期间私有底衬重建 ≥10 次（前提复现）、
                  贴图导入 **0 次**（把 reuse 关掉跑一遍：518 次 → 当场红）。**重建次数 / 指针
                  步数 < 1.5**（只有「它底下那个窗口」该重建；被拖的那个自己的底衬抓的时候
                  把它藏起来了，只在区域跨网格时才重建 —— 修前 1.96 次/步、修后 1.01，
                  退回源码跑同一条测试当场红）
```

## 已验证（wlroots 0.20.2 / AMD radeonsi / EGL 1.5，日志逐条比对坐标）

1. 嵌套启动；`wayland-info` 可见 `wl_shm`、`zwp_linux_dmabuf_v1`、`zwlr_layer_shell_v1`(v4)、
   `wl_output`、`zwp_virtual_keyboard_manager_v1`、`zwlr_virtual_pointer_manager_v1` 等全局
2. 3 个 kitty → 3 列各 464 宽；焦点在第 3 列时视口滚到 `vp=475`：
   `wm-a x=-469 / wm-b x=1 / wm-c x=471`
3. `Mod+H/L` 焦点左右移动并反向滚动；`Mod+R` 列宽 464 → 619；
   `Mod+Shift+L` 列换位；`Mod+Shift+J` 把窗口塞进右列（同列两窗各 485 高）
4. `Mod+F` 全屏 `929x977` 再恢复；`Mod+Q` 关窗后剩余列回弹到 `x=6`
5. `Mod+V` 切换浮动：`float mode wm-c: 0 -> 1`，再按 `1 -> 0`
6. 浮动自动识别（真实 GTK 窗口）：
   - `set_transient_for` 的对话框 → `float mode float-dialog: 0 -> 2`（FOLLOW），
     居中于父窗口格（父 471,6 → 子 520,357）
   - `set_resizable(False)` 的窗口 → `float mode float-fixed: 0 -> 1`（FIXED）
7. 滚动时两种模式行为正确：`Mod+H` 后父窗口 `471 → 1416`，跟随窗同步 `517 → 1462`
   （偏移恒为 +46），固定窗保持 `941` 不动
8. 交互式移动全链路（真实 GTK 标题栏拖拽 + 虚拟指针）：
   `grab move: float-dialog` → 窗口跟随指针 → `grab end`，合成器存活无卡死
9. layer-shell：waybar `layer=2 exclusive=30` 让位（`y=36 h=977`），退出后恢复；
   fuzzel `layer=3 kb_interactive=1`
10. 动画时长 = 墙钟（见上一节表格），人为 `SIGSTOP` 制造掉帧后时长不变、帧数减半
11. shader 管线方向/颜色：`passthru` 渲染四象限测试窗口后读回像素，
    左上 `(1023,0,0)` 红、右上 `(0,1023,0)` 绿、左下 `(0,0,1023)` 蓝、右下白 —— 无翻转、无串色
12. shader 生命周期：打开动画结束后 `fx=NULL`、表面不透明度回到 `1.00`、shader 节点已禁用
13. 关闭动画走快照：客户端进程被杀后仍能取到最后一帧并跑完 `genie`（140ms / 10 帧）
14. 嵌套 Wayland 后端下 host 停止投递 frame 回调时，看门狗定时器接管，
    动画仍然按时跑完（`185.9ms / 11 帧`）
15. SSD 装饰逐像素核对（读回装饰缓冲）：
    - 圆角：内容第 0 行第一个不透明像素在 x=7，第 6 行起在 x=0（半径 12 的圆弧）
    - 描边：窗口边缘 2px 处是聚焦强调色 `(103,140,209)`，alpha 237
    - 阴影：窗口下方 alpha 158 → 140 → 113 → 66 → 17 → 0（24px 内平滑衰减），
      左侧 117 → 70 → 23 → 0
    - 透明角落 alpha 0
16. kawase 模糊：直接读回模糊缓冲，边缘最大梯度从锐利的 `182` 降到 `28`（4 级 + offset 1.5）
17. 透明度剔除：背景窗口不透明度 0.3 时，`blur_alpha_min=0.15`（不剔除）与 `0.50`
    （剔除）两版装饰缓冲有 **60.4% 的像素不同** —— 阈值确实在起作用（这是当时按**底衬**
    alpha 剔的那版量的；那层剔除 115 撤了，现在阈值只按元素自己的 alpha 剔，见
    `./test-layermask.sh`）
18. 性能：headless 下 3 个 kitty + 1 个半透明模糊浮动窗，2000ms 动画跑出 125 帧（≈60fps），
    合成器 CPU 2.5%
19. 模糊缓存：静置 4s，装饰缓冲重绘次数**增量为 0**，共享 backdrop 重建次数也不变；
    壁纸提交/换图才作废共享的那一份，窗口移动只作废私有的那一份
20. 平铺窗口真的在模糊：内容不透明度设 0（只剩装饰）后读回装饰缓冲 ——
    关闭模糊时窗口内部 alpha 全 0，开启模糊时全 255 且 RGB 是被抹开的壁纸
    （`(255,255,108)` 就是被糊开的黄色方块）
21. 共享 vs 私有 backdrop：壁纸 + 平铺窗口 → 只重建共享份（`own=0`）；
    再叠一个浮动窗口 → 立刻出现私有份（`own=24`），静置后不再增长
22. 配置语言：`config.lua` 里 `vidow.exec("echo 7aa2f7")` → `border_color_focus = "#7aa2f7"`；
    循环算出的 `border_width = 6`、条件里的 `corner_radius = 20` 都落到日志里
23. 配置不阻塞渲染：`vidow.exec("sleep 0.8; echo 999")` 挂着时，0.425s 处新客户端
    完成窗口映射，0.839s 命令返回并应用
24. 配置超时：`VIDOW_EXEC_TIMEOUT=300` + `sleep 5` → 0.336s 处拿到 `nil, "timeout"`，
    脚本继续跑完，合成器存活
25. SIGHUP 重载：改文件后 `kill -HUP` → 日志出现新的 `radius=31`，且装饰缓冲几何随之变化
    （圆角采样点 0 → 25），不需要重启
26. wlr-screencopy：`grim /tmp/shot3.png` → `exit 0`，1280x720 PNG，四象限颜色
    `(320,240)=红 / (640,360)=绿`，与 pattern.py 画的一致
27. 配置里跑 grim：`vidow.exec_status("grim /tmp/shot-cfg.png")` → `out="" code=0 err=""`，
    文件确实产出；`grim --nope` → `code=1 err="grim: invalid option -- '-''"`
28. 配置里跑命令连的是 vidow 自己：`WAYLAND_DISPLAY` 在加载配置前已指向自己的 socket
    且后端已启动，所以 grim 截的是 vidow 画面而不是宿主桌面
29. 退出码透传：`test -d /tmp` → 0，`test -d /nope` → 1；
    `cat /nonexistent` → `nil, "cat: /nonexistent: 没有那个文件或目录"`
30. 嵌套下静止画面截图：不加看门狗时 `grim` 一直挂（超时 124），加 50ms 捕获看门狗后
    立刻 `exit 0` 并产出 941x1019 PNG（壁纸渐变颜色正确），空闲 CPU 仍为 0
31. 录屏协议：`wayland-info` 里能看到 `ext_image_copy_capture_manager_v1` /
    `ext_output_image_capture_source_manager_v1` / `zwlr_export_dmabuf_manager_v1`，
    且 `zwp_linux_dmabuf_v1` 只有一个（renderer 建的那个，v4）；
    `wf-recorder -f /tmp/rec.mp4 -c h264_vaapi` 出流正常（h264 / 1280x720 / nv12），
    `ffprobe` 能解析
32. 输入法协议：`zwp_text_input_manager_v3` / `zwp_input_method_manager_v2` 出现在
    `wayland-info` 里；fcitx5 连上后日志依次 `input method connected` →
    `text input enabled` → `input method activated` → `input method grabbed the keyboard`
33. 输入法中继（假输入法）：按键 → 合成器 → 输入法 → 合成器 → 客户端，
    `zenity --entry` 里出现 `<1><2><3>`
34. 输入法回注不转圈：fcitx5 抢着键盘时英文直通，`zenity --entry` 收到 `abc`
    （按键喂给 fcitx5，fcitx5 用虚拟键盘回注，回注不再被喂回去）
35. 输入法抢键盘时自己的快捷键仍可用：`Super+...` 在转发给输入法之前处理
36. ext-background-effect：`wayland-info` 里出现 `ext_background_effect_manager_v1`；
    bind 后客户端收到 `capabilities(blur=1)`
37. 区域生效：`blur_all=false` 时，请求了区域的窗口带内强边 0（糊）/ 带外 500（清晰），
    没请求的对照组带内 420（清晰）—— 模糊是协议带来的，不是全局策略
38. 区域盖过全局：`blur_all=true` 时同一窗口仍是带内 0 / 带外 500
39. portal 全局：`wayland-info` 里出现 `xdg_activation_v1`、`zxdg_exporter_v1/v2`、
    `zxdg_importer_v1/v2`、`zwlr_foreign_toplevel_manager_v1`、`ext_foreign_toplevel_list_v1`、
    `ext_foreign_toplevel_image_capture_source_manager_v1`（全局数 20 → 27）
40. GTK portal 端到端（headless vidow + 私有 dbus + `xdg-desktop-portal` +
    `xdg-desktop-portal-gtk`，`raw-gtk`）：portal 读到 `vidow-portals.conf`，
    `Using gtk.portal for ...FileChooser (config)`，`OpenFile` 返回请求句柄，
    vidow 里 `map vidow portal test: 674x748` 且 `im focus -> 0x...`（对话框拿到键盘焦点）
41. KDE portal 端到端（同一条链路，后端换 `xdg-desktop-portal-kde`，`raw-kde`）：
    `Using kde.portal for ...FileChooser (config)`，Qt 对话框在 vidow 里 `map 646x513` 并聚焦
42. xdg-foreign 跨版本：`tools/foreign.c` 分别用 `zxdg_exporter_v1` / `_v2` 导出 handle，
    把 handle 以 `wayland:<handle>` 当 `parent_window` 交给 GTK3 的 portal —— 两种 handle
    都能挂上（vidow 日志 `portal: 父子 vidow portal test -> foreign-app`），
    portal 侧 `Failed to associate portal window with parent window` 警告 0 条
43. foreign-toplevel-management 真客户端：`wlrctl toplevel list` 列出两个窗口，
    `wlrctl toplevel focus title:zen-one` 触发 `request_activate` 并转移焦点，
    `wlrctl toplevel close title:zen-two` 触发 `request_close` 且只关掉目标窗口
44. 修掉两个会 abort 合成器的 wlroots 0.20 assert（都是 portal 用例逼出来的）：
    - `wlr_xdg_surface_schedule_configure`：Qt/KDE 的对话框在第一次 commit 前就设好
      parent，早期路径直接排 configure。修完 KDE 的两次带 parent 请求都是
      `map … 646x513` 且合成器存活（此前直接 abort）
    - `wlr_text_input_v3_send_enter`：同一客户端换窗口时没先 leave。修完后
      `test-portal.sh` 两个后端都跑完，且假输入法中继仍然通
      （`zenity --entry` 收到 `<1><1><1>`，三次 commit 各一次）
45. portal 后端选择由 vidow 管起来，逐条实测：
    - 首次启动生成 `$XDG_CONFIG_HOME/xdg-desktop-portal/vidow-portals.conf`
      （7 行，`default=gtk` + 6 条 KDE），日志 `portal: 写入 …（7 行）`
    - 幂等：内容没变时 `portal: … 内容没变，跳过`，文件 mtime 不变
    - 逐键覆盖：`~/.config/vidow/portals.conf` 里只写 `FileChooser=gtk`，
      生成结果里只有这一行变成 gtk，其余 5 条 KDE 保持不变
    - 值留空 = 删掉：`Usb=` → 生成结果里没有这一项
    - `portal_desktop=` → `不碰 portals.conf`，不生成文件
    - `portal_conf_sync=false` → `只算不写（… 需要 7 行）`，不生成文件
    - 环境：`config.lua` 里 `vidow.exec("env")` 拿到的子进程环境是
      `XDG_CURRENT_DESKTOP=Vidow` + `WAYLAND_DISPLAY=wayland-0`
46. 「写一行换文件选择器」整链验证（`./test-portal.sh switch-gtk|switch-kde`）：
    `~/.config/vidow/portals.conf` 写 `FileChooser=gtk` → vidow 生成
    `FileChooser=gtk` → 真 portal `Using gtk.portal for …FileChooser (config)`；
    换成 `kde` 则 `Using kde.portal …`。两次都是 4 个窗口 map、2 个对话框
    成功挂到应用窗口上、合成器 0 次 abort
47. XWayland 跑真实 X11 程序：`xterm` 连上后 `x11: 新窗口 …（xterm，平铺）`，
    被平铺进列并在两块屏上都能显示；`xrandr --query` 报
    `HEADLESS-2 connected 640x360+0+0`（= 模式 ÷ 输出缩放），即 X 屏幕与 vidow 的
    逻辑坐标是同一套；Xwayland 不设 `wl_surface.set_buffer_scale`（命中倍数 = 2 说明
    它一直是 1），所以缓冲区倍数只能由合成器声明
48. 混合 DPI 整链（`./test-dpi.sh`，1x + 2x 两块 headless 屏，15 项断言全过）：
    - 两块屏各自 `scale 1.00 / 2.00`，逻辑尺寸 = 像素 ÷ 缩放（1280/2=640、1280/1=1280）
    - X 全局缩放升到 2 = `ceil(最密那块屏)`，并给 X11 客户端下发 `GDK_SCALE=2`
    - X11 窗口在 2 倍屏上 `缓冲 628x696 -> 设备 628x696`（1:1）；
      同一个窗口在 1 倍屏上 `缓冲 1268x1416 -> 设备 634x708`（缩小 2 倍，从不放大）
    - Wayland 客户端 `缓冲 340x264 == 设备像素 340x264`（跟着输出缩放渲染，1:1）
    - 指针命中：光标布局坐标 (154,216) 在窗口原点 (6,6) 上 → 表面局部 (295,420)
      =（光标−原点）×2，误差 1 像素
49. 装饰按设备像素渲染（`VIDOW_FX_DUMP=1` 把 756×824 的离屏缓冲 dump 出来逐个像素量）：
    2 倍屏上 `border_width=2` 的边框 = 4 个设备像素、`corner_radius=12` 的圆角 = 24 个
    设备像素、内边距 = 64 设备像素，都是逻辑值 ×2；离屏缓冲 756×824 = 客户端缓冲
    628×696 + 上下左右各 64，与场景节点尺寸 378×412 正好是 2 倍关系
50. 缩放计算离线自检 31 项（`./build/scaletest`）：`auto` 反推 1.7016 / 1.1344 / 0.9564，
    量化 1.7037037→1.70、0.956→夹到 1.0、上限 8.0，这块屏自己点名的压过全局，
    `1.5x` / `abc` / `0` / `2x` 被拒，`off` → 1.0，X 屏幕对账（设备像素 / 只有逻辑
    尺寸 / 量不出来）三种判据
51. 配置边界：`xwayland=0` → `配置里关了，不注册 X 服务`，合成器照常跑；
    `xwayland_scale=off` → `GDK_SCALE=1`、不再升到 2、X11 窗口按 1 倍声明（HiDPI 屏上
    是放大糊的那个取舍）；`xwayland_env=0` → GDK_SCALE / QT_SCALE_FACTOR 这类环境变量
    一个都不碰（`_XWAYLAND_GLOBAL_OUTPUT_SCALE` 和 `Xft.dpi` 照旧写：那是 X 服务自己的
    事实，不是给客户端的环境变量）
52. 修掉一个把点击吃光的真 bug：装饰画面的场景节点是普通 buffer，默认接受输入，盖在
    客户端表面上，于是开着装饰的窗口点不着。给它一个恒返回 false 的
    `point_accepts_input` 后，`./build/vdrag` 把光标移进 X11 窗口能正常命中并算出局部坐标
53. 回归：`./test-portal.sh switch-gtk` 在重构后仍然 4 个窗口 map、2 个对话框挂上父窗口、
    0 次 abort（两种窗口共用的访问器层没有破坏 xdg 那条路）
54. X11 的 SSD 与 Wayland 一致（`./test-dpi.sh` 里同一个 zenity 对话框各跑一次）：
    X11 `GDK_BACKEND=x11` 内容 300×230 → 装饰缓冲 728×588 = (300+64)×(230+64)×2；
    Wayland 内容 340×264 → 装饰缓冲 808×656 = (340+64)×(264+64)×2。同一条装饰管线、
    同一套判据，只有「覆盖窗口」不装饰（对应 Wayland 的 xdg_popup）
55. X11 浮动窗口的尺寸链：GTK3 对话框（`zenity --info`）不再被压成 1×1 ——
    `float 信息: 300x230 at 490,245`（居中在 1280×720 工作区），装饰缓冲 364×294；
    X 侧 `xwininfo` 报 `300x230+490+245`；带上 `GDK_SCALE=2` 时 X 窗口是 600×460，
    声明 2 倍后逻辑 300×230，与 Wayland 同一个对话框一样大
56. `GDK_SCALE` 真的进了子进程：`config.lua` 里 `vidow.exec("printenv GDK_SCALE")`
    拿到 `2`（不是只看合成器自己打的日志）。`vidow.exec` 返回 stdout 在前，退出码要
    用 `vidow.exec_status`
57. X11 弹出物（`./test-xwayland-popup.sh`，67 项断言全过）：四类弹出物
    （`or` / `TOOLTIP` / `POPUP_MENU` / `COMBO` + `WM_TRANSIENT_FOR`）都被识别成
    「弹出物」、都摆到 `place xp-popup -> 200,300 200x120`（客户端说的绝对坐标）、
    都没有对应尺寸的装饰离屏缓冲、都**没有**被浮动布局接管；像素上「弹出物外扩 20 像素
    那一圈两图完全一致（0 个像素不符）」，关掉之后也不留残影；后开的窗口盖不住它
58. 弹出物不碰父窗口的焦点：开、关两个时刻父窗口边框那一圈的平均色不变
    （`['35','48','198'] → ['35','48','198']`，最大差 0.0）；开窗/关窗/移动动画都调成
    2500ms，也拍不到弹出物有动画（它是「啪」地出现的）
59. 弹出物改判与混合 DPI：`late=600` 的 `_NET_WM_WINDOW_TYPE` 到达后
    `归类为弹出物` + `float mode xp-popup: 0 -> 1`，之后客户端自己挪到 500,500 原样
    生效且没有装饰缓冲；2 倍屏上 X (2800,400) 200×120 → 逻辑 (1400,200) 100×60，
    命中坐标 = (布局−原点)×2 = 40,40

60. 热插拔记忆（`./build/hotplugtest`，32 项离线断言全过）：输出身份只认连接器名、
    记忆台账按名字去重（拔了不删、插回来还是同一条）、挑接收的屏优先左边最近的活屏
    （没有左邻居就取右边最近的、已经拔掉的不参与挑选、一块活的都没有返回「没有」）、
    「把一组列稳定挪到末尾」是真排列且两组内部顺序都不变
61. 热插拔还原（`./test-hotplug.sh`，24 项断言全过）：拔掉左边那块屏时
    `hotplug: 拔掉 HEADLESS-2（3 列 / 3 个窗口 / 1 个浮动窗口），先挪到 HEADLESS-1 上住，
    归属记着` + `1 个 layer surface 挪到 HEADLESS-1 上住`；插回来时
    `HEADLESS-2 回来了，还原 3 列 / 3 个窗口 / 1 个浮动窗口（列宽、滚动位置都留着）`。
    三个窗口的先后顺序、列间距（列宽 2/3 = 845 与默认 634）、y/宽/高完全一致，
    整体平移量 = 新原点 - 旧原点；浮动窗口按「相对自己那块屏的偏移」摆回原处；
    四象限颜色按日志坐标采样正确；grim 的画面从 2560x720 → 1280x720 → 2560x720
62. 顺带修掉两个热插拔必崩的真 bug：借出去的共享模糊贴图在输出销毁后没收回
    （下一帧拿已销毁的贴图去画 → GLES2 断言崩溃）；layer surface 的 `output` 指着已被
    `free()` 的 `vidow_output`（野指针）。另外 `output->box` 这类缓存也换成了「现问布局」：
    拔掉一块屏时 wlroots 会把剩下的屏重新摆放（实测右边那块从 x=1280 挪到 x=0）

63. niri 风格 overview（`./test-overview.sh`，26 项断言全过）：`Mod+O` 进入时日志给出
    缩放（3 个窗口、列带 1914x708 缩到 0.609、铺进 1268x708 的工作区）和每个窗口的屏幕
    矩形；overview 里窗口尺寸 = 原尺寸 × 0.609（±2px）、列间距 640 → 390、顺序不变、
    都在屏内；按日志坐标采样四象限颜色正确且**没被压暗**（遮罩夹在壁纸和窗口之间）；
    壁纸被压暗；`Esc` 退出后画面回到原样（18 个像素差，全是取整）；点窗口 →
    `overview: 选中 p-a` + 退出 + 那一列滚进视野、没选的列滚出画面；两块屏时只缩当前
    那块（另一块屏上的窗口淡出）
64. overview 里两个自找的坑都写进 README 了：遮罩层建两次导致遮罩跑到窗口上面（overview
    里窗口被一起压暗）；临时改 `fx->blur` 导致退出之后装饰底衬再也不模糊（退出前后差
    24000 个像素，全在窗口内边距上）—— 改成在 `view_wants_blur()` 里判断「这一刻不给
    贴图」，退出即恢复

65. 工作区与特殊工作区（`./test-workspace.sh`，59 项断言全过）：配置里 `vidow.workspace_create`
    建出 1–4 和特殊工作区 `scratch`（`vidow.workspace_list()` 只列 1–4，`scratch` 不在里面）；
    工作区 1 上 1 个窗口、工作区 2 上 2 列 —— 切过去只有它的窗口画在屏上（第 2 列那块区域在
    工作区 1 的截图里是壁纸、在工作区 2 里是 pattern 四象限），切回来原来那个还在（窗口没被
    杀、列数没丢，日志 `本区 2 列 / 2 个窗口`）；特殊工作区把窗口搬进去后普通工作区看不到它，
    打开时显示在**聚焦的那块屏**上（日志 `显示在 HEADLESS-1 上，本区 1 个窗口，全屏 2 个可见`）、
    另一块屏照旧显示自己的普通工作区，再开一次收起、窗口留在里面；拔掉一块屏时
    `HEADLESS-1 上 1 块工作区跟着列挪到 HEADLESS-2 上住`，那块屏上的窗口跟着搬家且仍然可见
    （另一块屏原来那块的窗口切回工作区 1 就回来）；键位全部由配置绑（`keybind: Mod+1 -> workspace 1`
    等 6 条），合成器一个工作区键都不预设
66. 工作区踩过的三个坑（都写进 README 了）：①工作区一开始按屏各建一套 → 两块屏上「亮着的
    工作区」和窗口所在的工作区对不上（日志 `打开特殊工作区 … 0 个窗口`）；改成工作区全局、
    `output` 只是「现在住哪块屏」，缺的时候挪一块**空着的**过来（不抢别的屏上开着窗口的）；
    ②列搬工作区时只改了列的 `ws`，视图自己的 `ws` 还是旧的 → 可见性判断把它藏在错的地方；
    ③视图可见性只看「聚焦那块屏的活动工作区」→ 另一块屏的窗口全被隐藏；改成按窗口自己
    那块屏找活动工作区（每块屏各自亮一块）

67. 工作区改动带出的三个回归（都修了，也写进 README 了）：①**弹出物被工作区可见性
    关掉了场景节点** —— `vidow_workspace_view_visible()` 对弹出物返回 false，于是 X11 菜单
    再也命中不到（`test-xwayland-popup.sh` 的「命中坐标」那条抓到的）；改成「弹出物跟父
    窗口走」（父窗口在工作区里显示着，它才显示）。②自动编号的工作区**重名**：两块屏各自
    自动建的工作区都叫 "1"，`按名字建` 于是找回另一块屏上那块，每次 arrange 都把它在两块
    屏之间拽来拽去（日志里同一个工作区指针先 output=HEADLESS-1 再 =HEADLESS-2），窗口在
    两边轮流画；改成编号全局唯一。③浮窗位置**按平移量算**在拔插时不准（拔线那一刻问到的
    目的屏布局位置还是旧的，撞见 1 次平移被算成 3 次、浮窗跑到 4163）：改成位置存「相对
    当前屏原点的偏移」、只在摆放时换算一次
68. 回归（工作区改动之后重跑）：`./test-hotplug.sh` 全部通过、`./test-xwayland-popup.sh`
    OK、`./test-dpi.sh` 全部通过、`./test-overview.sh` OK、`./test-workspace.sh` 59 项全过、
    `./build/hotplugtest` / `./build/scaletest` 离线自检照旧

69. 视口外强制渲染（`./test-force-render.sh`，19 项断言全过）：控制组 —— 没有规则、没人录屏时，
    窗口滚出视口后 `tools/frames.c` **停帧**（0 帧/2 秒；日志 `place 心跳 -> -640,6` 证明它真的
    在视口外）；配上 `vidow.rule{ title = "心跳", force_render = true }` 之后同一位置
    **125 帧/2 秒**（≈60fps，日志 `rule: 心跳 强制渲染（视口外补帧）开` +
    `强制渲染: 心跳 在视口外/藏起来，照样补帧回调（窗口规则要求）`）；滚回画面里就停
    （`强制渲染: 心跳 停（它已经画在画面上）`）；切到别的工作区把它藏起来照样 125 帧/2 秒；
    `vidow.rules()` 能查回两条规则（一条按标题、一条按 app_id）
70. 录屏 / 窗口预览自动保活（同一个测试）：`tools/capture.c`（ext-image-copy-capture + 窗口截图源，
    和 xdg-desktop-portal 的窗口录制同一条路）挂上截图源之后日志 `portal: 有人要录 / 预览 心跳，
    它开始强制渲染（视口外也出帧）`，窗口在视口外客户端照样 184 帧/2 秒，而且**抓到的最近 5 帧
    像素校验和 5 个都不同**（录到的是新画面，不是冻住的一张）；录屏客户端断开后日志
    `portal: 录屏 / 预览的客户端断了，心跳 不再强制渲染`，补帧跟着停
71. 这个功能里踩的三个坑（都写进注释和 README 了）：①补帧会自己排下一帧，不节流的话在
    headless 后端上跑到 2500fps（一个「保活」把 CPU 跑满）→ 按 16ms 节流；②`tools/capture.c`
    一开始把帧回调排在 commit **之后**，客户端和合成器互相等（一帧都出不来），而且
    `ext_foreign_toplevel_list_v1.finished` 不是「枚举完了」而是「合成器收摊了」（协议里写着）；
    ③「有人拿着窗口」最早只等截图源销毁，可是截图源的生命周期挂在窗口的场景节点上 ——
    **录屏程序崩了窗口会一直补帧**，改成同时听 `wl_client` 的销毁

72. 补帧节流可配置（`./test-force-render.sh`，28 项断言全过）：全局 `force_render_hz = 30`
    时视口外补帧 **61 帧/2 秒**（≈30Hz，日志 `…照样补帧回调（窗口规则要求，节流 30Hz）`）；
    规则里写 `hz = 15` 覆盖全局 → **31 帧/2 秒**；`hz = 0` → 不限速（125 帧/2 秒，headless
    后端的帧节奏就是上限）；`vidow.rules()` 里能查回 `hz`
73. 节流踩的坑：一开始只按「补帧→客户端提交→再补」这条链子推进，节流一旦大于客户端的
    提交延迟，链子就断在半路（全局 30Hz 量出来 **0 帧**）—— 改成用 `wl_event_source` 闹钟
    在到点时叫醒一帧（`output->pump_timer`），30Hz / 15Hz 就都准了
74. 焦点规则（`./test-focus.sh`，61 项断言全过）：基线 —— 新窗口默认抢焦点（日志
    `焦点: (无) -> a1` / `焦点: a1 -> a2`）；`vidow.rule{ app_id = "demo", focus_on_open
    = false }` 之后同一个窗口上屏，日志 `map: 演示 不抢焦点（窗口规则 focus_on_open =
    false）`、全程没有一行把焦点给它、**但窗口照常画在屏幕上**（截图采样 (255,0,0)）；
    `vidow.rules()` 里查得回 `focus_on_open=false`、没写的 `focus_on_activate` 是 `nil`
75. 激活请求（同一个测试，`tools/activate.c` 发的 xdg-activation）：窗口搬到工作区 2 再切回
    1（截图采样 (0,0,0)，确实藏起来了）之后它请求激活 → 日志 `activation: 激活 演示` +
    `激活: 演示 在藏起来的 2 上，先把它端出来` + `workspace: 切到 2` + `焦点: … -> 演示`，
    截图采样又变回 (255,0,0)（**窗口真的画出来了**）。特殊工作区同理（`激活: 演示 在特殊
    工作区 scratch 上，把它开到 HEADLESS-1` + `workspace: 打开特殊工作区 scratch（显示在
    HEADLESS-1 上…）`）。`focus_on_activate = false` 时日志 `激活: 演示 不理（窗口规则
    focus_on_activate = false）`、之后焦点没再动过。窗口**还没上屏**就请求激活时先记下来
    （`激活: 演示 还没上屏，等它出来再聚焦`），map 之后才兑现（日志顺序可验）
76. 激活令牌的过期时间可配（同一个测试，对照实验）：`activation_token_timeout = 1` 时，
    客户端上屏拿到令牌、2 秒后才用它请求激活 → 请求被丢掉（日志 `令牌 … 过期了
    （activation_token_timeout = 1 秒，已经 2.0 秒）→ 不理`，一行 `激活:` 都没有）；
    改成默认的 `0`（不过期），同一个客户端同一个时序 → `activation: 激活 演示` +
    `焦点: … -> 演示`。这条对照就是「点了通知窗口不动」的根因和修法
77. 顺手修掉的一个真 bug：`wlr_seat_set_capabilities()` 只在真设备 / 虚拟键盘挂上来时更新，
    **虚拟指针挂上来时漏了** —— 于是只有虚拟指针的会话里客户端拿到的能力是 `0`（没有指针），
    永远收不到任何指针事件（点不动，也拿不到点击的 serial，激活请求只能带 0）。
    `tools/activate.c` 第一次跑出来的就是 `caps 0`；补上之后 `caps 1` + 点一下就
    `activate click 10`。真机上有真设备所以看不出来，headless / 只靠虚拟指针的会话里必现
78. 回归（焦点 / 激活改动之后重跑）：`./test-workspace.sh` 59 项全过、`./test-overview.sh`
    OK、`./test-hotplug.sh` 全部通过、`./test-dpi.sh` 全部通过、`./test-xwayland-popup.sh`
    OK、`./test-force-render.sh` 28 项全过、`./test-focus.sh` 61 项全过、
    `./build/hotplugtest` / `./build/scaletest` 照旧
79. 测试稳定性：`test-hotplug.sh` 的「插回来后窗口又画在那块屏上」在机器忙的时候会拍到
    移动补间「正在飞」的那一帧（机器忙时补间拖得更久）—— 改成拍两张（间隔 0.4 秒），
    **任意一张对得上就算过**（判据没放松：两张都对不上还是 FAIL）
80. serial 校验不过的请求可配（`./test-focus.sh` 第 9/10 组，对照实验）：客户端写
    `set_serial(0, seat)` 时，默认（`honor_xdg_activation_with_invalid_serial = 0`）令牌
    照样发到客户端手里（`activate token …` —— 协议要求回 `done`，不像 wlroots 那样让它
    干等），但激活被忽略：`令牌的 serial 0 不是发给这个客户端的` + `的 serial 0 校验不过
    → 不理`，之后焦点没再动过；同一个客户端、同一个时序，把开关打开（`= 1`）→ `校验不过：
    照样理它` + `activation: 激活 演示` + `焦点: a1 -> 演示`。真实点击事件的 serial
    （`wl_pointer.button` 给的）两条路径都放行
81. 激活认的是 `activate` 请求里带的 surface（同一个测试第 8 组）：客户端建令牌时不带
    `set_surface`（通知守护进程就是这么干的：它只把令牌塞进子进程的环境变量），被激活的
    应用自己在 `activate` 里带自己的 surface → 照样激活，`焦点: a1 -> 演示`。这条把
    「认哪个 surface」钉住了：认错的话通知点击永远顶不上来（顶的是守护进程自己的窗口）
82. 自己实现 xdg-activation 服务端（`src/activation.c`，服务端头 / 代码从
    `protocols/xdg-activation-v1.xml` 现场生成）：`commit` 一律回 `done`、serial 判据用
    `wlr_seat_client_validate_event_serial()` 但**怎么用由配置决定**、令牌按
    `activation_token_timeout` 作废、建令牌的客户端一断就作废、`activate` 里没带 surface
    才退回 `set_surface` 那个。拿到的窗口照样交给 `vidow_view_activate()`（窗口规则 /
    跨屏 / 特殊工作区 / 还没上屏那几条全在）
83. `test-hotplug.sh` 里「插回来后窗口又画在那块屏上」的采样坐标修对了：grim 拍的是**整个
    布局**（几块屏拼起来那张），`place` 日志给的也是布局坐标，所以采样要直接索引；以前减了
    一次「屏的布局原点」，只有在**那块屏恰好落在 x=0** 时才碰巧对 —— 插回来的屏由
    `wlr_output_layout_add_auto` 放在最右（原点 1280），减完就采到隔壁屏的窗口边界上，
    于是误报。改对之后这条断言从「1 个窗口在画面里」变成**3 个都验到**（更严了），
    并且确认了合成器侧本来就是对的（在正确坐标上探像素：红/绿/蓝/白四象限全对）

84. 窗口规则 `honor_invalid_serial = true`（同一个测试第 11 组，按应用开口子）：规则
    `vidow.rule{ app_id = "demo", honor_invalid_serial = true }` + 全局开关**关着**时，
    `demo` 的 `set_serial(0, seat)` 请求照样激活（`… 校验不过，但这个窗口的规则写着
    honor_invalid_serial = true → 照样理` + `activation: 激活 演示` + `焦点: a1 -> 演示`），
    而同一个时序里规则外的应用（`other` / 别家）还是走「不理」那条 —— 日志里两条路径都
    看得见，证明它是**按应用**而不是「全开」。`vidow.rules()` 查得回 `honor_invalid_serial`，
    规则加载日志写了 `无效serial也理=on`
85. `test-focus.sh` 的清理逻辑改稳了：`stop_clients()` 原来只按命令行匹配
    （`*build/activate*` 之类）杀进程，从别的脚本里调这个测试时**外面那层 shell 的命令行
    里就带着这几个字**，结果把 wrapper 自己 SIGTERM 掉了（测试跑到一半神秘死掉）—— 现在
    先沿父进程链确认「是本测试起的」才杀（`is_ours()`）

86. IPC 状态输出（`./test-ipc.sh` 第 2/3 组）：`windows` 报的 `geometry` 跟合成器自己的
    `place` 日志**逐字段相等**（x / y / 宽），表面尺寸跟 `map` 日志在 ±16 以内（map 是
    那一刻的快照，之后布局的 configure 会改它）；layer surface 的 namespace / 层 / 四边锚点 /
    独占区 / 键盘交互 / 铺满 1280x720 全部报对；输出报出模式、缩放、可用区、活动工作区、
    它上面的 layer 与窗口；`pick-window` 在窗口上返回那个窗口、在空工作区上返回 null
87. IPC 操作（同一个测试第 5/6/7/8 组，每一条都回读状态确认）：焦点（`focus-window` /
    `activate-window` / `focus-column`）、浮动（`set-floating` / `toggle-floating`，浮窗没有列）、
    尺寸与位置（`set-window-size 800x600` 之后客户端**真的**按新尺寸提交了，
    `set-window-position` / `center-window` 落在 (240,60)）、列宽（0.25 → 布局里算出来 317 ≈
    0.25×1268）、工作区（切 / 纯数字自动建 / `prev` / `index` / 搬窗口 / 特殊工作区开关）、
    显示器（关 → `enabled:false`，开回来；缩放 1.5 → 逻辑宽 853；`auto` 回 1；转 90° →
    布局宽高对调；模式、位置、聚焦）、配置（`set-config` 之后 `get-config` / `config` /
    `state.config` 三处都是新值）、`reload-config` 真的重跑了 config.lua、`action` 走键位
    那套入口、`toggle-overview` 状态翻转、`quit` 之后进程退出且 socket 被清掉
88. IPC 事件订阅（同一个测试第 10 组）：一次运行里收到 `window-opened` /
    `window-title-changed` / `window-moved` / `window-floating-changed` / `window-closed` /
    `workspace-activated` / `output-changed`，载荷里带着标题与 app_id；`window-opened` 带
    完整窗口对象（含几何）；`window-closed` 带的是**标题快照**（销毁信号来的时候客户端的
    标题已经收走了，所以 map / 改标题时留了一份）；过滤器只放行订了的事件
    （订 `window-opened` 时收不到 `output-changed`）
89. IPC 错误路径（同一个测试第 11 组）：未知命令 / 没有 `cmd` / `cmd` 类型不对 / 未知 id /
    平铺窗口不让改尺寸 / 认不出的输出与方向 —— 全部 `ok:false` 且原因能读；坏 JSON 指出
    第几个字符；一条连接上连发三条请求各答一次；超过 1 MiB 的一行被拒（报错并断开），
    之后合成器照常应答
90. IPC 多屏（同一个测试第 12 组）：两块 headless 输出下，`outputs` 报两块、只有一块
    `focused`、布局自动排开（x = 0 / 1280）；`move-window-to-output` 把窗口搬到另一块屏
    （回读确认）、`move-workspace-to-output` 把整块工作区搬过去（那块屏的活动工作区跟着变）、
    `output-power off` 关掉其中一块之后**窗口还在**（只是不画了）而另一块照常开着
91. IPC 浮窗尺寸的竞态（同一个测试第 5 组逼出来的真 bug）：`set-window-size` 之后客户端
    要是先提交一次旧尺寸，浮动盒会被它改回去（等于命令没发生）—— 现在记着「我让它多大」，
    客户端认下来之前不采纳旧尺寸并再催一次 configure，测试里回读的是**客户端提交后的**
    表面尺寸（±48 的 CSD 边距）
92. 正常退出路径的 wlroots 断言（`quit` 暴露的）：以前只有 SIGTERM 这一种退法，走不到析构，
    所以 `wlr_xdg_shell` 的 `wl_list_empty(&xdg_shell->events.new_toplevel.listener_list)`
    断言从没露过面；现在退出前统一摘监听器与事件源（`main.c` 的 `server_finish()`），
    `quit` 干净退出（测试里验了进程退出 + socket 清掉）

93. 剪贴板桥（`./test-clipboard.sh`，68 项断言）：接管（XFixes / SelectionRequest 两条路都拦到）、
    X11 → Wayland（`xclip -i` 之后 Wayland 侧拿到同样的字节，类型表对得上）、
    Wayland → X11（`wl-copy` 之后 `xclip -o` 拿到、二进制走 `-t <mime>`）、PRIMARY 单独一条路、
    INCR 双向（200 KiB 往返字节完全一致）、错误路径与 `clipboard-changed` / `primary-changed`
    事件、配置热切换（`clipboard=0` 之后确实退回 wlroots 那套、X 侧的监听窗口与 XFixes 注册
    都摘掉）、退出与清理（进程退出码 0，不留 socket / 不留 X 窗口）
94. 加剪贴板桥撞出来的三个真 bug（都修了）：
    ① 关窗崩溃：`x11_drop_surface` 里动了一个 wlroots 已经销毁的场景节点
    （`view->scene_surface->buffer->node.data`）——`clipboard=0` 也能复现，是**原来就有的**，
    退出路径第一次真的走完才露出来；现在 `quit` 干净退 0
    ② 幽灵窗口：xwm 自己那条连接上的内部窗口（1x1 的 `selection_window`）也被 `new_surface`
    收进了视图模型，白占一格布局；按「X 资源 id 槽位」过滤掉（`vidow_clipboard_owns_xwindow`）
    ③ Xwayland 启动竞态：`x11_attach` 里十几次同步 round-trip 会把事件循环卡住几毫秒，
    正好撞上「触发 Xwayland 启动的那个 X 客户端」map 窗口 —— 那个窗口的 MapWindow 落在 xwm
    订上 root 之前就没有 MapRequest（X 服务器上有、合成器里没有）；接管推后 100ms 再做。
    另外补了「表面已经 map 过、view 才建出来」时补调一次 `view_map`（`src/view.c`），
    这样成批到达的事件顺序再乱也不会出现「node 一直 disabled、窗口不显示也不摆放」
95. 已知（未在本机复跑确认）：上面 ③ 的两个改动落地之后 `./test-xwayland-popup.sh`
    （67 项）没再整套复跑过；改之前它 4 次里 3 次挂在混合 DPI 那节。
96. 嵌套会话里实机撞出来的两个 bug，都修了并单独验过：
    ① **任何 Wayland 弹出物都能打死合成器**（noctalia 状态栏的提示 / GTK 菜单 / Qt 菜单）。
    两个独立的坑叠在一起：
    - `wlr_xdg_popup_unconstrain_from_box()` 内部会 `wlr_xdg_surface_schedule_configure()`，
      那里第一句是 `assert(surface->initialized)` —— 而 `new_popup` 是客户端 `get_popup`
      那一刻发的，popup 自己的 surface 还没 commit 过 → **SIGABRT**。正确做法是等 popup
      第一次 commit 再 unconstrain（sway 也是这么干的），现在抽成
      `vidow_popup_unconstrain_on_commit()`，view.c / layer.c 共用
    - `popup->parent` 可能还是 NULL（wlroots 在 xdg_shell 的 new_popup 里还没填上，
      到 layer surface 的 new_popup 时才填），而 unconstrain 上来就沿 parent 链解引用
      （`wlr_xdg_surface_try_from_wlr_surface` 的 `cmp %rax,0x2b8(%rdi)`，没有空指针检查）
      → **SIGSEGV**（core dump 里直接读出 `popup->parent == 0x0`）。现在判空跳过
    回归客户端：`tools/popup.py`（GTK3 窗口 + Gtk.Menu）。旧构建上它稳定复现
    `Assertion surface->initialized failed`；修好之后 `./build/vidow` 活着、
    断言 0 次、日志里 `popup: unconstrain 完成（父面 0x...，可用区 0,0 1280x720）`
    顺带修了同一个 layer 弹出物被两个 new_popup 处理器各建一个场景节点（重影）——
    挂 layer surface 上的现在只由 layer.c 建
    ② 动画不丝滑：`vidow_arrange_layers()` 无条件打 backdrop 标记，而它在 layer 开关动画
    里是每帧调的 —— 每帧重建一遍模糊金字塔（重抓一屏 + 4 级 kawase + 重新分配 7 个 GBM
    缓冲 + 4 个 FBO），实测 143 次/秒（一次 200ms 动画 31 次）。现在只有真正改变底衬的
    地方才打标记（map / unmap / destroy / 首次 commit / 换屏 / 动画落定）：同一条
    layer-open 动画从 ~30 次重建降到 **2 次**，帧率也回到稳定的 60fps
97. 装饰：实现 zxdg_decoration_manager_v1（`src/decoration.c`）。之前没有这个全局，
    kitty / GTK / Qt 只能退回自己画 CSD 标题栏 —— 屏幕上就是一个和窗口圆角对不上的方角
    标题栏（kitty 的 glfw-wayland 里就带着整套 CSD）。现在按 `decor` 配置下发
    SERVER_SIDE / CLIENT_SIDE，headless 里起 kitty 实测日志
    `decor: 客户端想要 服务端装饰，按配置给 服务端装饰`、窗口正常摆放；`wayland-info`
    能看到 `zxdg_decoration_manager_v1` v1。坑：
    `wlr_xdg_toplevel_decoration_v1_set_mode()` 内部 `schedule_configure` 里有
    `assert(surface->initialized)`，在 new_toplevel_decoration 里直接下发会当场 SIGABRT
    （踩过，已修），所以只在 surface 初始化之后下发、没就绪就等第一次 commit
98. 窗口移动补间（`vidow_place_view`）起算点取错：函数开头就把 `view->geom` 覆盖成新位置，
    后面再用 `geom + anim_off` 去算「现在画在哪」，结果恒等于**新位置减新位置 = 0**，
    补间从 0 走到 0 —— 日志里 `anim move start: 190ms` → `done ... 30 frames` 一帧不落，
    画面上一动不动（现象就是「移动窗口完全没有过渡动画」，但动画其实在跑）。现在在覆盖
    `geom` 之前先把 `drawn_x/drawn_y` 存下来当起点。headless 实测（`focus-window-direction`
    驱动一次 640px 的滚动，窗口用四象限 `tools/pattern.py`）：截图里红块左边缘
    前 6 → **中 161** → 后 0（另一个窗口 670 → 801 → 640），中途确实停在两端之间；
    日志 `move tween p2: 644,6 -> 0,6（1500ms）`。量的时候要 `set-config blur_enabled 0`：
    默认 `blur_all` 开着，窗口是磨砂玻璃、底衬从窗口里透出来，按颜色找窗口边界会失效

99. 装饰着色器的噪点哈希在 `mediump` 下溢出，把整块内容画成**不透明黑色楔形**
    （`decor_fragment_src`）：`dot(p, vec2(12.9898, 78.233))` 在 `p` 是大窗口的
    右下角时（如 504×1047 的渲染目标）算到 8 万多，超过 mediump 的 65504 →
    `Inf` → `sin(Inf)` = `NaN` → `(NaN - 0.5) * u_noise` **哪怕 `u_noise = 0` 还是
    `NaN`**，`out_px.rgb += NaN` 把 RGB 毒掉，而 `alpha` 从 `content` 原样保留 →
    写出去就是 (0,0,0,255)。边界正好是 `12.99x + 78.23y + u_time = 65504` 这条
    直线，斜率 `dx/dy = -78.233/12.9898 = -6.02 px/行`，和实测的 -6.0 一致；因为
    装饰图是缓存的（只在底衬变化时重画），这条斜线会**定格**在触发的那一帧上，
    所以「移动窗口/改配置/重建底衬」都刷不掉，只有重启 vidow 才能清掉。
    触发条件是窗口（渲染目标）够大：`12.99w + 78.23h > 65504`，小窗口（400×300
    → 2.9 万）永远不触发，所以表现为「窗口一变大/一改尺寸就坏」。
    修法：①`#ifdef GL_FRAGMENT_PRECISION_HIGH` 下用 `precision highp float`；
    ②噪点整段包进 `if (u_noise > 0.0)`，并且先把坐标 `mod(p, 512.0)` 压回安全
    范围（`u_noise = 0` 时连 `sin` 都不算，顺带省掉开销）。
    定位过程（headless + `output-mode` 改尺寸复现）：`VIDOW_FX_DEBUG=uv` 输出
    `vec4(v_uv,0,1)` 是**完美线性渐变**（排除四边形/插值/回读）；`=inside` 输出
    SDF 覆盖率是干净的 504×1047 矩形（排除 SDF）；`=content` 直接采样贴图无楔形
    （排除贴图与绑定）；把 `blurred * u_blur_strength * inside * cover` 换成
    `vec4(0.0)` 后楔形消失、把 `content` 换 `vec4(0.0)` 则整块透明 —— 楔形只在
    「blur 项是**运行时值**」时才出现（常量 `0.0` 会被编译器把 `x * 0` 折成 0，
    `NaN` 因此消失），指向噪点那一行。修完复测：resize 前后 18 帧、`noise=0.4`、
    `blur_enabled=1` 三种情况下「不透明黑」占比都是 **0.000**

100. 升采样把亮度乘了一遍（模糊过曝）：`kawase_up` 是 `sum / 12.0 + u_add`，12 份抽样
     除对了、但加进来的 `u_add` 没参与平均 → 常量输入下 `c + c = 2c`。半透明窗口后面
     那层底整整亮一倍。改成 `(sum + u_add) / 13.0`。实测见「Kawase 背景模糊」一节：
     亮度比 1.905 → 0.995

101. 升采样链的目标尺寸写反（模糊糊成一团）：`up[i]` 按 `w >> (levels-1-i)` 分配，于是
     循环第一个算出来的 `up[levels-2]` 才是被采样的那张，后面两级白算、`u_texel` 也对
     不上。改成 `up[i]` 与 `down[i]` 同尺寸、最终取 `up[0]`

102. 开窗动画和位移补间抢同一组 `anim_off_x/y`：新窗口 map 的同一帧里 arrange 先跑过
     一遍（带着列偏移的变化），位移补间就起来了；入场动画随后开始却没撤掉它 → 两个
     动画每帧互相覆盖，上滑被抹成 0、还横着挪几像素，看着就是「闪一下再弹一下」。
     修法：入场动画开始时撤掉位移补间并接管 offset，`vidow_place_view` 在入场动画
     期间只更新 `geom`。实测见「动画与自定义 shader」一节

103. 配置系统收敛成一个 Lua 文件（原来是 `config` key = value + `config.lua` 两层）：
     去掉 `src/config.c` 里那套行解析（`vidow_config_load` → `vidow_config_reset`，
     只剩默认值 + 环境变量），文件由 `vidow_script_load()` 跑，路径认
     `$VIDOW_CONFIG` / `$VIDOW_CONFIG_LUA` / `$XDG_CONFIG_HOME`。两个副作用要处理：
     ① 重载现在**先回默认值**再跑文件（删掉的行才会真的消失）；
     ② `ipc` / `xwayland` / `scale` 的消费时机早于配置文件 → 新增
     `vidow_config_sync()`，在配置真正跑完（`script_apply`）或配置缺失 / 语法错时
     按配置对一次账，`vidow_ipc_apply_config()` 能建能拆、`vidow_xwayland_apply_config()`
     能建能拆。旧的 `config` 文件还在的话启动时打一行 ERROR 提示搬走。
     11 个测试脚本里的 key = value 配置同步改成 Lua

104. 配置改完自动重载（`src/configwatch.c`，不用再手动 SIGHUP）：inotify 盯的是
     配置文件的**目录**而不是文件本身 —— 编辑器保存要么原地写（同 inode）、要么写
     临时文件再 rename（换 inode），盯文件的话第二种会把 watch 留在已经被删掉的
     inode 上，之后再也收不到事件。收到事件后自己 `stat()` 比对
     `(dev, ino, mtime, size)`，真变了才 `vidow_config_reload()`（和 SIGHUP 同一个
     入口），顺便把一次保存里的好几个事件合成一次（去抖 150ms，慢速编辑器分几段
     写也只重载一次）。配置文件是符号链接时先 `realpath()` 解析，盯真身。
     实测：原地写 / rename 覆盖 / 删文件（回默认值）都认，同目录里动 `portals.conf`
     不重载。`./test-config.sh`（15 项断言）

105. layer surface 背后的模糊（状态栏 / 面板）：在 layer surface 自己的场景节点**下面**
     垫一块和它一样大的节点，用同一个装饰着色器画、但把内容/阴影/边框/圆角/噪点全关
     （`fx->blur_only`），只留模糊。半透明处透出来，不透明处盖住。用共享底衬（壁纸 +
     background/bottom 层），background 层跳过；抓底衬时这些节点要先藏起来（不然
     bottom 层的会自己糊自己）。实测状态栏内最大梯度 74.0（关）→ 4.67（levels=2）→
     2.33（levels=4），壁纸硬边是 107.7。`./test-layerblur.sh`

106. `blur_offset` / `blur_alpha_min` 没进模糊缓存键：它们是 kawase 着色器的 uniform，
     改完缓存的贴图不会跟着变，而共享金字塔的缓存键只有 `levels` —— 于是运行时改这
     两个键看着「怎么调都没反应」。缓存键补上这两个（私有金字塔同理）。
     另外把「平均梯度」这个测量方法记下来当反面教材：它对降采样几乎不敏感，会给出
     「4 级和 2 级一样」的错误结论；同一批图换成「和哪档高斯最像」，量出来是
     8 / 16 / 28 屏幕像素，逐级翻倍

107. 阴影默认改薄：`shadow_radius` 24 → 8、`shadow_offset_y` 6 → 2。24px 的衰减
     晕开太散，看着像窗口浮在一团灰里；8px 正好是沿窗口边缘一圈，偏移收到 2 之后
     左右几乎对称、只留一点方向感。副作用是装饰离屏图的 padding 从 32 降到 12
     （`vidow_fx_pad()` 按同一个公式算），那块缓冲也跟着小了。实测（同一块壁纸 +
     一个不透明窗口，截图对比）：旧的在窗口右下铺开 ~30px 的暗区，新的是一条 8px
     的暗边，紧挨着就能看到壁纸的方块。`test-dpi.sh` 里写死的 `PAD = 32` 跟着改成 12

108. 底衬捕获按输出倍率换算（高 DPI 上模糊是坏的）：`backdrop_iter()` 原来把场景图的
     逻辑像素直接当设备像素画进底衬，1.7 / 2 倍屏上只有左上角 1/倍率 ² 那一块有内容
     （2 倍屏实测底衬 alpha 均值 0.25），其余全透明 —— 模糊在空像素上采样，级数越多
     越透明，layer 背后的模糊整块错位。按倍率放大 + 减掉这块屏的布局原点（按边取整）
     之后：底衬 alpha 0.25 → 1.00，layer 背后的亮斑 σ 16.0 → 23.5，对位偏差 ≤ 0.6 像素。
     新增 `./test-blurscale.sh`（1 / 1.7 / 2 三档倍率，7 项断言）—— 关键判据是
     「底衬铺满整块屏」，拿修前的二进制跑会挂。新工具：`tools/dot.py`（背景层画亮点，
     模糊不改变它的重心，所以量重心就能量对位）、`tools/panel.py`（右下角一块 50%
     透明的面板，不是贴着 (0,0) 的，专治对位问题）

109. 模糊的透明度剔除从硬阈值改成软阈值 + 整核归一化：`step(u_alpha_min, a)` 在阈值
     等值线上是一刀切（用一个 alpha 斜坡背景层实测逐像素跳变 0.102，斜坡左半
     整块被吃掉）。先试了「按被采纳的那几份归一化」，反而在「刚好有一个抽样被采纳」
     的位置把那一份放大到满值、又造一道硬边（跳变 0.161）；最终是
     `smoothstep(α_min/2, α_min, α)` + 仍然除以整个核：0.027，斜坡在阈值以上与
     `blur_alpha_min = 0` 完全一致（alpha 均值 0.448 vs 0.500，差的就是阈值以下的淡出）

110. 模糊链照 niri 重写 + 透明度剔除只做一次：降采样抽样（中心 + 四斜角）、升采样半步
     12 抽样、去掉 dual filter 的 `add`（它每级掺回一点锐度，σ 每级只涨 1.5 倍而不是
     niri 的 2 倍），`MAX_BLUR_LEVELS` 4 → 6；剔除从「每趟每个抽样」挪到 `kawase_final`
     的最后一次遮罩（每趟相乘的话，均匀 alpha 0.45 过完链只剩 0.0000）。实测硬边最大
     梯度 2/4/6 级 = 0.127 / 0.027 / 0.0039。新增 `./test-blurlevel.sh`（`./test-blurcull.sh`
     与 `tools/soft.py` 当时也是为这条路加的，后来随底衬剔除一起删了，见 115）
111. 会被模糊的 layer surface 自己从底衬里藏掉（bottom 层面板原来在底衬 dump 里占 100%
     红像素，模糊里浮出它自己的影子）。新增 `./test-layerghost.sh`、`tools/bpanel.py`
     （`MAX_BLUR_LEVELS` 也顺手从 fx.c 挪到 server.h —— 改级数上限时忘了改
     `vidow_blur_set` 里写死的 `down[4]`，数组越界直接段错误）
112. layer 的模糊改成含窗口（niri 语义：抓的是它盖住的东西）。`top` / `overlay` 层按自己
     那块区域重抓一份：基座是共享底衬上对应的一小块（1:1 拷贴图，壁纸不用重传）+ 窗口
     （连窗口弹出物）画上去，再走 kawase；`bottom` 层不变（先于窗口画，底下本来就没窗口）。
     缓存键 = 区域 + `gen_scene` / `gen_layer` + 级数 + offset / alpha_min（空转 3 秒 0 次
     重抓；窗口每出一帧重抓一次，3 秒 173 帧 = 173 次，区域只有面板那么大）；采样坐标
     减掉区域原点（`fx->blur_tex_x/y/w/h`）。实测面板底下的 B-R 均值 0.027 → 0.445。
     新增 `./test-layerwin.sh`、`tools/bluewin.py`（`tools/panel.py` 加了第三个参数 `tl`：
     贴左上角 —— 缩放布局里一个窗口只占左边一条，右下角多半是空的）
113. `blur_alpha_min` 从软过渡改回**硬阈值**（`step`，跟 niri 的 `blur_finish` 一样）：
     软那版 `smoothstep(α_min/2, α_min, a)` 的**有效阈值在 α_min/2**，设 0.5 时 alpha
     0.45 的底衬还剩 90% 的模糊（0.4510 → 0.4039），看着就是「这个键没用」。现在
     0.45 底衬 + 阈值 0.5 → 0.0000（颜色一起归零），0.9 底衬 + 阈值 0.5 → 0.9020
     一点不衰减。`./test-blurcull.sh` 重写成 3 个相位 5 条判据（拿软过渡那版跑挂 2 条；
     这个测试连同它量的那层底衬剔除在 115 里一起删了）。
     另查明「按元素自己的 alpha 掐模糊」（niri 的 ignore-alpha 本意）vidow 根本没有：
     layer 的模糊是一整块矩形，会漏进面板自己透明的内边距里（亮点峰值 1.000 → 0.285，
     探针 `tools/padpanel.py`）。
114. 按**元素自己的 alpha** 剔除模糊（niri 的 `ignore-alpha` 本意，治「一圈透明模糊」）。
     装饰着色器多了一个 mask 采样器：窗口 / 弹窗那条路直接用内容贴图的 alpha，layer 的
     模糊底衬那条路由调用方把 layer surface 自己的缓冲递进来（层表面的 geom 就等于
     surface 尺寸，uv 跟内容那趟一致）；两条路都用**内容自己的 alpha、不含
     `window_opacity`**（否则 `window_opacity` 一调低模糊就整个没了）。客户端用
     ext-background-effect 要了区域就不剔（niri 的 `need_ignore_alpha` 同一个判断），
     拿不到 mask 时也不剔（退回老行为）。实测（`tools/padpanel.py` 40px 透明内边距 +
     亮点）：阈值 0.15 时内边距里的亮点峰值 **0.322 → 1.000**（原样），面板 50% 那块底下
     照糊（0.167）→ 修后 0.322。新增 `./test-layermask.sh`（3 项，拿老二进制跑挂 1 条）、
     `VIDOW_MASK_DUMP`。`./test-layerblur.sh` 的第 2 条判据改用最大梯度（窗口自带的那圈
     半透明边框也被剔了，平均梯度会跟着动，原来的 0.9 比值太脆）。

115. **底衬不做透明度剔除了**（撤掉 109 / 110 / 113 那条路）：剔除是用来**约束模糊的
     范围**的（哪儿该糊），不是用来改糊出来的像素 —— 底衬该不该糊已经由「谁在它上面、
     它自己透不透」决定了，再按它的 alpha 二次加工只会引入阈值边上的一刀、逐趟相乘、
     软阈值有效值减半这一串坑（④⑤⑨）。`kawase_final` 这个程序整个删掉，最后一趟上升
     跟其它趟是同一个 `kawase_up`；`blur_alpha_min` 也从金字塔的缓存键里拿掉（它现在只
     进装饰着色器），改它由新加的 `blur_mask_min_changed()` 顶一下 `blur_ver`，帧循环
     照样立刻重画 —— 不然运行时改配置会看着「没反应」（⓪ 那类）。相关探针与测试
     （`./test-blurcull.sh`、`tools/soft.py`、`tools/ramp.py`）一起删了：上面几条历史里
     提到的这三个名字都指它们。

116. **入场 / 关闭动画的着色器喂「装饰结果」，不是原始内容**（治「开窗动画一上来冒出一块
     比窗口大的大矩形」）。动画那一趟原来把**原始内容贴图**铺在「窗口 + 阴影留白」那么大
     一块渲染目标上：内容被拉伸到整块画布（每边多出 shadow_radius 那么宽的留白），而且那
     一趟根本不走装饰着色器 —— 没圆角、没边框、没模糊，也不吃 `window_opacity`（半透明
     窗口在动画期间是一块**不透明的纯色**大矩形）。改成先画装饰、再把装饰结果当贴图喂给
     动画着色器：整块带边框/圆角/模糊的窗口跟着缩放淡入淡出，1:1 落在窗口该在的位置上。
     顺带修掉一个老毛病：`anim_buf` 只分配了一块，而正在上屏的那块被场景锁着 → 动画着色器
     **隔一帧才跑得起来**，屏幕上就是「跑过着色器的窗口」和「只跑了装饰淡入的窗口」交替
     闪，看着正好像大矩形一闪一闪；现在两块都分配。实测（`tools/wall.py` + `bluewin.py` +
     zoom + `window_opacity = 0.5` + `shadow_radius = 24` 且阴影全透明，按日志里的
     `anim open start` 对时取样）：动画后段三帧的可见窗口 / 中心红通道 = 662x704 / 1
     （老二进制）→ 三帧都 ≤ 窗口 640x687 / 28。新增 `./test-openanim.sh`（老二进制连跑
     3 次都挂 2 条）。关闭动画（genie）走同一条路，一起好了（原来压下去的是没装饰的原始
     快照）。

119. **窗口动画「q 弹」：入场过冲、关闭回弹**。新增缓动 `out_elastic`（阻尼余弦
     `1 - e^(-5.5t)·cos(7t)`：t≈0.35 冲过头 11% 再单调收敛；t = 0 处正好是 0，曲线
     必须从起点出发）和两个缩放键 `open_scale_from` / `close_scale_to`（默认 0.82），
     入场/关闭各用自己的曲线 `open_easing` / `close_easing`（默认 `out_elastic`）。
     `easing` 保持 `out_cubic` 只管位移补间 / layer / overview —— 布局重排过冲晃眼。
     缩放作用在场景节点上、围绕窗口中心（`anim_scale` 在 `vidow_fx_frame` 里乘到目标
     尺寸上，位置补回一半差值；静止时恒为 1，一个像素都不动），离屏缓冲照旧按原尺寸
     画。几何量跟着过冲，透明度和 `u_progress` 夹在 0..1（负 alpha 在场景节点上没定义；
     进度 > 1 会让 `zoom` 这类 shader 采到贴图外面）。layer 的关闭沿用 `easing` 且不缩放
     （状态栏「缩着淡出」很怪），所以 `vidow_closing_start()` 多了 `easing` /
     `anim_scale_to` 两个参数。新增 `./test-bounce.sh`：按日志里的 `anim open start` /
     `anim close start` 对时，在曲线上几个特征时刻连拍量蓝色窗口的包围盒 ——
     起步 62%、过冲 104%、关闭低点 46%、回弹 +28px（配置里把缩放拉到 0.5、动画拉到 4s，
     快门几十毫秒的延迟才不至于吃掉判据）。`test-openanim.sh` 锁 `open_easing = "linear"`
     + `open_scale_from = 1.0`：它测的是「动画期间画的是什么」，曲线一过冲，`u_progress`
     一上来就是 1.0，中段那帧就测不出「缩着」了。回归：21 个测试全过。

120. **工作区换成 niri 那套：每块屏各自一份纵向工作区栈，动态增删**。序号 = 在那块屏上的
     位置（1 起，`focus-workspace N` 越界夹到最后一块，**不再按名字现建**）；栈底永远留着
     一块空的（窗口开在它上面就再补一块，`ensure_bottom`）；中间那些空的、没名字的工作区
     切走就收掉（`cleanup`，niri 的 `clean_up_workspaces`）；具名工作区（配置里
     `vidow.workspace_create()` / `set-workspace-name`）永远在、按声明顺序排栈顶。全局链表
     按屏分组、组内按序号排（`ws_splice`：注意 `wl_list_insert(pos, elm)` 是插在 pos **之后**，
     第一版搞反了，具名工作区跑到空工作区下面去了）。工作区还多了 `id`（稳定编号，IPC 用）
     和 `idx`/`named` 字段；`name` 对动态工作区是空的（不再自动编号 —— 名字是身份，位置是
     序号，两者不能混）。热插拔：拔线整块挪到剩下的屏、追加在栈底空位前面（`_safe` 遍历，
     因为遍历时链表位置会变），插回来按 `home` 认领。新增 `move-workspace-up/down`（重排）、
     `set-workspace-name`/`unset-workspace-name`、`focus-workspace-previous`、
     `move-window-to-workspace-up/down`、`move-column-to-workspace`（niri 的名字）；
     默认键位补上 niri 的 `Mod+U/I`（切）、`Mod+Shift+U/I`（搬窗口）、`Mod+Ctrl+U/I`（重排），
     默认配置里给了 `Mod+1..9` 直跳的绑定（可删）。`test-workspace.sh` 的日志断言跟着新格式
     改；`test-ipc.sh`/`test-focus.sh` 同步。回归：defaultconfig / gap / config / workspace /
     ipc / focus / overview / hotplug / clipboard / layerwin / xwayland-popup 全过。

130. **键位一个都不内置了：全由 `vidow.bind()` 声明，默认那套写在 `configs/14-keys.lua`**。
     `src/input.c` 里原来有一张写死的键位表（`Mod+Return` 起终端、`Mod+Q` 关窗、
     `Mod+H/J/K/L` 挪焦点、`Mod+V` 浮动、`Mod+R` 列宽、`Mod+M` 最小化、`Mod+O` 全景、
     `Mod+F` 全屏、`Mod+U/I` 工作区、`Mod+Escape` 退出、`Ctrl+Alt+F1..F12` 切 tty），
     而且它排在 `vidow.bind()` **前面** —— 想换键位是换不了的，只能改代码。现在整张表
     没了：那些行为都成了动作字符串（`spawn` / `close-window` / `focus-column` /
     `focus-window` / `move-column` / `move-window` / `toggle-floating` /
     `toggle-fullscreen` / `cycle-column-width` / `toggle-overview` / `quit` /
     `switch-vt`，`<方向>` 认 `-1`/`1` 也认 `left`/`right`/`up`/`down`），默认配置把它们
     绑在原来的键上，删一行就还给客户端。顺带修掉两个老毛病：`focus -1` 一直是坏的
     （`parse_number()` 不收负数，而 README 的动作用的是 `focus <-1|1>`），现在统一走
     `parse_direction()`；键名表里没有 F1..F12，`Ctrl+Alt+F2` 这种绑不上去，现在
     `F1..F35`（以及 `xf86switch_vt_1..12`）都能写。`spawn` 用的是现成的
     `vidow_exec_detached()`（双 fork、不阻塞），`Mod+Return` 仍认 `$VIDOW_TERMINAL`。
     没绑的键原样透传给客户端 —— **没有配置文件 = 一个键位都没有**，这是刻意的。
     新增 `./test-binds.sh`（35 项）：没配置时按 `Mod+Q`/`Mod+V` 什么都不发生、
     `--write-config` 那套真的能用（关窗 / 浮动 / 列宽 / 挪列 / 最小化 / 全景）、
     `Ctrl+Alt+F1..F12` 绑得上、动作字符串逐条跑、参数写错当场报错、把 `Mod+Q` 换成
     `Mod+W` 后新旧键的行为都跟着变、`quit` 正常退出。`test-vt.sh` / `test-overview.sh` /
     `test-hotplug.sh` 改成在自己配置里绑上要按的那几个键（它们原来吃的是内置表）。

131. **果冻的软硬梯度改成「从上到下」，2 倍屏上不再跑位；摇晃放大改成线性加 + 补间**。
     三件事一起：

     ① **果冻的梯度只看行号**（`src/jelly.c`）：原来是按「到抓点的距离」算的（`g = 到抓点
     的距离 / 最大距离`），抓哪儿哪儿硬、向四周散开 —— 手感飘，而且抓底边时梯度是**反的**
     （顶行落后最多、底行几乎不落后）。现在 `g = 行号 / (总行数 - 1)`：顶行几乎不落后、
     底行落后 `V·τ` 最多，中间线性过渡，跟抓点在哪儿无关。抓点那个节点照样钉死（跟手
     1:1）。`test-jelly.sh` 新增判据：抓标题栏和抓底边，左边缘都必须「顶部在前」（旧代码
     抓底边时是负的，实测过）。

     ② **2 倍屏上拖起来整块窗口偏 72 逻辑像素**（松手才回位，半透明窗口连烤进去的模糊
     底衬一起偏 —— 看着像「背景绘制错位」）。原因：果冻那个场景节点的位置用
     `margin_px`（= `JELLY_MARGIN × scale`，**设备**像素）去减，而场景图是**逻辑**像素
     —— 该减的是 `JELLY_MARGIN` 本身。1 倍屏上两者相等，所以只有 HiDPI 露出来。
     新增两段判据：2 倍屏上「按住不动 → 窗口位移必须是 0 设备像素」（旧代码 144px），
     以及半透明 + 模糊窗口按住前后逐像素比对（旧代码平均差 193、82% 的采样点差得离谱；
     现在平均差 0.6、0.0%），外加一个哨兵（pattern 左红右绿）确认那块里真有窗口。

     ③ **摇晃放大改成线性加 + 补间**（`src/cursorfx.c`）：原来是「每晃一次乘
     `cursor_shake_scale`」（默认 2 → 2、4、8、16 一路炸开，第一下就大得吓人），而且到点
     还原是**跳**回去的。现在每圈**加** `cursor_shake_scale - 1`（2 = 第一圈到大一倍，
     之后 3x、4x、5x…），放大和缩小都是**线性补间**（放大 150ms、缩小 200ms，16ms 一帧，
     每帧从原尺寸那张重画，所以每一帧都是清晰的）。`vidowmsg cursor` 多报一个 `target`
     （这一圈的目标倍数，`scale` 是补间中间值）。`test-cursor-shake.sh` 改成 50 项断言：
     线性加（2→3→4→5→6→7 逐圈核对）、放大本身是补间（晃完 50ms 的倍数严格在起点和目标
     之间）、缩回是补间（200ms 里密集采样，必须量到 1 和 2 之间的中间值）。

     回归：42 个测试脚本全过（`test-jelly.sh` 28 项、`test-cursor-shake.sh` 50 项）。

132. **一块屏都没有的时候开窗口不再崩**（用户报的「vidow 打开 kate 直接崩」）。崩在
     `view_ensure_column()`（`src/view.c`）：`ws = view->ws` 和 `vidow_workspace_active()`
     都拿不到工作区的时候（`server->outputs` 空、`focused_output` 为 NULL —— 嵌套会话把
     外层窗口关了、真机上最后一块屏拔了），后面那行 `if (!ws->special && …)` 直接解引用
     NULL，SEGV 落在 `view_map` 里。**窗口照样会 map**：XWayland 还活着，X 客户端一连上
     来就 map（`test-lastoutput.sh` 的 xev 就是；用户那边是 kate）。

     修法：这种状态下给新窗口安置一块**现在没屏**的工作区 —— 先找一块现成的
     （`vidow_workspace_homeless()`：优先 home 就是这块窗口归属的那块屏的），一块都没有
     就现建一块（`output == NULL`）；`ensure_bottom` 那一行加 `ws != NULL` 的判据。屏回来
     的时候这块工作区会被 `vidow_workspace_adopt_homeless()` 认领回去，窗口跟着一起回来
     —— 为此把 `ws_set_output_full()` 里「列 / 列里的窗口 / 跟着这块工作区的浮窗的
     output 全部改掉」那一段抽成 `ws_propagate_output()`，认领那条路也要走一遍：光认领
     工作区的话窗口还指着 NULL，屏回来了屏幕上却什么都没有（实测：画面全黑）。

     `test-lastoutput.sh` 补 7 项（15 → 22）：①没屏时 X 客户端 map 上来之后 vidow 必须
     还活着、窗口必须进列表（原来这条**测不到**：崩发生在最后一次 `kill -0` 之前，判据
     写晚了）；②新增一段「三块屏全拔光 → 开两个窗口 → 插回来一块」，断言每个窗口的
     output 都变成插回来那块屏（没屏期间收下的窗口跟着工作区一起回来）。把修复回退掉
     跑一遍，这两条都会红（`段错误（核心已转储）`）。

133. **状态请求抢在第一次 commit 之前 → wlroots 断言 → SIGABRT**（用户报的「tty 起 vidow，
     开 kate 直接崩」，tty 上打的就是那句 assert：`surface initialized failed`）。崩在
     `view_request_maximize()`（`src/view.c`）：它转手调
     `wlr_xdg_toplevel_set_maximized()`，而那里面是
     `wlr_xdg_surface_schedule_configure()` —— 第一句就是 `assert(surface->initialized)`
     （wlroots 0.20.2：`types/xdg_shell/wlr_xdg_surface.c:168`）。xdg-shell 允许客户端建好
     `xdg_toplevel` 之后、交第一帧之前就 `set_maximized`，wlroots 收到请求**立刻**发
     `events.request_maximize`（`xdg_toplevel_handle_set_maximized` 里直接
     `wl_signal_emit_mutable`），那时候 surface 还没 initialized → 合成器 abort。
     Qt/KDE 恢复「上次是最大化」的窗口就是这么开窗的（kate 首当其冲）。

     用户那边的证据：`coredumpctl` 里 4 个 vidow 核心转储（21:31、21:34、21:40、23:01）
     栈顶全是 `view_request_maximize`，`session-3`（tty）跑的 `./build/vidow`。

     修法（三条路都过一遍 `vidow_view_configure_ready()`）：
     ①`view_request_maximize`：surface 没 initialized 就先不应答 —— 初始 configure 由
     `view_commit` 的 `initial_commit` 分支发，那一次本来就是 `maximized=false`，等于把
     「不接受最大化」这个答复捎过去了（xdg-shell 只要求有应答）；
     ②`vidow_view_set_minimized`：同一句 assert，客户端第一帧还没来就
     `set_minimized` 的话（xdg-shell v6 起有）先不排 configure，状态照记；
     ③`view_request_fullscreen` 反过来是**兑现**：没 ready 就先不记状态（记了就是「布局
     当它全屏、客户端以为自己不是全屏」），改到 `initial_commit` 分支里、arrange **之前**
     补上，这样第一次 configure 就带 fullscreen。

     新增 `tools/preinit.c`（`--maximize` / `--minimize` / `--fullscreen`，可叠加；
     `--after-commit` 是对照组 —— 上屏之后再发，这条时序本来就好）+ `./test-preinit.sh`
     （22 项）：①抢跑 set_maximized：vidow 活着、请求确实发在 commit 之前（request 行在
     mapped 行前面）、客户端拿到 configure 且里面**没有** maximized、尺寸不是 0x0、窗口
     正常进列表；②对照组一样活着、一样答「不最大化」；③抢跑 set_minimized：vidow 活着、
     `vidowmsg windows` 里 `minimized=true`、日志有「最小化: 收起 收起」；④抢跑
     set_fullscreen：窗口 `fullscreen=true` 且客户端拿到的 configure 里带 fullscreen；
     ⑤日志无 assert / abort、收尾一个 vidow 都不剩。把修复回退掉跑一遍：①②③④ 都红
     （`Assertion 'surface->initialized' failed`，vidow 当场被带走）。

134. **所有屏上的 Qt 窗口都大 2 倍**（用户报的「kate 在 scale 2 的屏上又 X2 了 ——
     其实所有屏都 X2」）。原因不是渲染，是**环境变量泄漏**：`vidow_xwayland_env()`
     把 `GDK_SCALE` / `QT_SCALE_FACTOR` 设成 `S = ceil(最密那块屏的缩放)`（用户那边
     DP-2 是 2 倍，所以 S = 2），这两个变量是给 X11 客户端用的 —— 但 vidow 只能改
     **自己**的环境，于是 vidow 拉起来的进程链（键位拉起的终端，以及那个终端里启动
     的程序）全都继承它，而 Wayland 的 Qt **也认 `QT_SCALE_FACTOR`**：在输出缩放之外
     再乘一遍 → 每块屏上的 Qt 窗口都大 2 倍。kate 正是 Qt。

     证据：①`coredumpctl` 之外先看配置 —— 用户 `10-xwayland.lua` 是 `xwayland_env =
     true` + `xwayland_scale = "auto"`，`09-output.lua` 里 DP-2 scale 2 → S = 2；
     ②`test-dpi.sh` 早就有 `childenv GDK_SCALE=2` 这条断言（子进程真的拿到）；
     ③实测（headless 2 倍屏）：kate 干净 env → `缓冲 628x696 声明 2 倍 -> 逻辑
     314x348`（1:1 正确），加上 `QT_SCALE_FACTOR=2` → `缓冲 2180x1132`，逻辑尺寸翻倍
     = 所有屏都大 2 倍；④1 倍屏上更直观：干净 634x708 → 带变量 1090x708（宽度翻倍）。

     修法：**不再设 `QT_SCALE_FACTOR`**，Qt 那边改走 X 自己的 DPI
     （`QT_AUTO_SCREEN_SCALE_FACTOR=1` + 早就写好的 `Xft.dpi = 96×S`，那是 X 的
     `RESOURCE_MANAGER` 属性，Wayland 客户端看不到）。`GDK_SCALE` 留着 —— 实测
     GTK3/GTK4 在 Wayland 下都不认它（1 倍屏上给 `GDK_SCALE=2`，声明还是 1 倍），
     所以它只影响 X11 的 GTK。代价如实写进 README：混 DPI 时 X 屏幕的 DPI 是两块屏
     混出来的值，Qt 推出来的倍数可能不是 S，那时 X11 的 Qt 程序按 1 倍画、密屏上偏小；
     要精确就单独给那个程序 `env QT_SCALE_FACTOR=2 kate`。

     `test-dpi.sh` 补 3 项（12 → 15）：①子进程**没有** `QT_SCALE_FACTOR`（`printenv`
     打出来是 nil）；②子进程拿到 `QT_AUTO_SCREEN_SCALE_FACTOR=1`；③日志里不许出现
     `GDK_SCALE=QT_SCALE_FACTOR` 这种下发。把修复回退掉跑一遍，这三条都红
     （`childenv QT_SCALE_FACTOR=2`）✓。回归：`test-dpi.sh` 15/15、
     `test-xwayland-hidpi.sh` 20/20、`test-x11scale.sh` 18/18、`test-preinit.sh` 22/22。

135. **帧追踪：哪块屏的帧链断了，日志点名**（用户报的「KDE 的窗口不渲染新画面」）：
     kate / dolphin 的窗口停在最后一帧、**拖到另一块屏就活**、输入照旧能点。这个组合只有
     一个机制能解释：帧回调是跟着**输出的帧事件**走的（`src/output.c`：
     `wlr_scene_output_commit` 成功那一帧之后才 `send_frame_done`），所以某块屏的
     page-flip 不回来了，那块屏上的所有客户端就都拿不到帧回调 —— 全停在最后一帧，而
     键盘 / 点击不走渲染，照旧能用。

     我没法在 headless 上复现它（那边的帧事件和 commit 无关，按不住），所以这轮给的是
     **能定案的诊断**，不是盲改：

     - `VIDOW_TRACE_FRAMES=1`：要帧时记一笔（谁要的、几次），帧事件回来销账；250ms 一轮，
       发现「要过帧（或客户端还在交帧）却 300ms 没出帧」就报一行 `WLR_ERROR`，点名哪块屏、
       vidow 要过几帧、最近一次谁要的。默认关；不开的时候 `vidow_output_trace_commit()`
       第一行就返回，正常路径零开销。顺带把六处 `wlr_output_schedule_frame()` 收口到
       `vidow_output_schedule_frame(server, wlr_output, why)`（唯一的排帧出口）。
     - `VIDOW_TEST_STALL_OUTPUT=<屏名>`：测试钩子，把那块屏的帧**事件**按住 = 复现症状。
     - 顺手修的一个自己踩的坑：输出刚建出来时 `last_frame_ns` 是 0，而 `vidow_now_ns()`
       是开机以来的纳秒数 —— 不初始化的话第一帧会被算成「停了 3.8 小时」（实测），
       现在建输出时就打上基准。

     新增 `./test-frametrace.sh`（16 项）：①健康 + 追踪开：窗口内容上屏、**一行 trace 都
     没有**（不误报）；②掐断帧链 + 追踪开：客户端确实交过帧，追踪报出 ERROR 且点名
     HEADLESS-1、写明「客户端还在交帧，屏幕却不更新」；③掐断帧链 + 追踪**关**：一条 trace
     日志都没有、vidow 照样活着（诊断不改变行为）。

166. **录屏 portal 的光标元数据（`cursor_mode=metadata`）：光标不进画面，走 PipeWire 的
     `SPA_META_Cursor` 交给消费者自己合成**。

     165 里「仍然没做」的第一项。做完才知道卡在哪：**能不能拿到这个元数据不是生产者一个人
     说了算的**。

     - **合成器侧**（`src/capture.c` / `src/screencast.c`）：`pointer_mode=metadata` 时画面里
       不带光标（和 hidden 一样不画），改成每帧往缓冲的 `SPA_META_Cursor` 里写位置 + 热点，
       光标图变了才附上位图（`bitmap_offset=0` = 「沿用上一次那张」，和 KWin 一样；指纹用读回来
       的像素算 —— 拿 `wlr_output_cursor` 的贴图指针当身份会撞上「释放再分配」）。
       位图从贴图读回（`wlr_texture_read_pixels` + 字节序换算成预乘 RGBA8888），缩放屏上按
       输出像素尺寸重采样；`oc->transform` 不是 NORMAL 时不报位图（只报位置，旋转过的光标图
       自己转回来容易转错方向）。上限 256×256（`VIDOW_CURSOR_MAX_PX`），比这大的光标只报位置。
     - **两个真坑**：
       ① **元数据要等格式协商完再报**（`pw_stream_update_params`，`param_changed` 里）——
          `pw_stream_connect` 时一起报的 meta 进不了缓冲（缓冲是消费者连上来、格式定下来的
          那一刻按双方参数一起分配的）。xdpw / KWin 都是这个顺序。
       ② **消费者也得要**：`pw_buffers_negotiate` 里 meta 是「消费者声明的参数在生产者那边
          过滤一遍」（`param_filter`，`src/pipewire/buffers.c`），两边都给**固定值**又不一样
          的话这个元数据会被整个丢掉 —— 表现是消费者缓冲里连 `SPA_META_Cursor` 都没有。
          GStreamer 的 `pipewiresrc` 声明的是**范围**（默认 384×384、最小只要结构体），我们的
          固定值落在范围里所以对得上；只给死值的客户端（写死 64×64 之类）拿不到，这反而是
          安全的：我们声明多少就写多少，不会溢出。
     - **`AvailableCursorModes` 现在报 7**（hidden|embedded|metadata），helper 按前端给的
       `cursor_mode` 映射到协议的 `pointer`（未知值按 hidden，不透传）。
     - **新工具 `build/cursorprobe`**：GStreamer **不解** `SPA_META_Cursor`（`libgstpipewire.so`
       里连这个名字都没有），所以验证得自己当消费者。它按 gst 那样声明元数据，把每帧的
       `id / pos / hotspot / 位图尺寸 / 像素指纹` 打出来。`--declare` 就是那个声明。
     - **顺手修掉两个真 bug**：
       ① `wlr_output_cursor` 的 `x/y` 是**这块屏自己的像素坐标**（wlroots 在
          `wlr_output_cursor_move` 里已经乘过 scale 了），而抓帧框是**布局坐标**。旧代码按
          「逻辑 × scale」算，于是：光标在第二块屏上时整个画不出来（算出来远在框外）、缩放
          2 倍的屏上位置翻倍。现在统一走 `cursor_dst_pos()`（先减布局原点再乘 scale），嵌入
          光标和元数据位置共用它。`vidow_capture_grab_view()` 的取景也抽成
          `vidow_capture_view_box()`，窗口流的元数据位置和渲染取景用的是同一份换算。
          （**173 把这条推翻了**：172 ⑥ 之后窗口流改走取景器，`vidow_capture_view_box`
          的换算跟流对不上 —— 窗口流的光标元数据因此永远是 `id=0`，两个函数一起删了。）
       ② **screencopy 的引用计数**（`src/screencopy.c`）：`capture_output()` 里 `sc_client->ref++`
          排在「输出不可用就回 failed」**后面**，可 `frame_destroy()` 一律会 unref —— 抓到一块
          正在销毁的输出（比如录屏对话框的缩略图赶上虚拟输出被拔掉）就白扣一次，把 manager
          资源还指着的那个 client 提前 free 掉；下一次 `capture_output` 直接段错误在
          `wl_list_insert(&manager->frames)` 上（core dump 的栈就是它）。改成本轮新增的
          metadata 那一轮**必崩**（多一轮对话框预览 = 多一次机会），修完全套 41 项过。
     - `./test-portal-own.sh` 从 32 项扩到 41 项：`AvailableCursorModes=7`、metadata 流里
       消费者拿到位图（`id=1` / `pos=100,100` / 有尺寸有内容）、**缩放 2 倍时位置 = 200,200**、
       以及反证（hidden 模式下同一个消费者连元数据都拿不到）。
     - **165 里「仍然没做」的另一项（bind 收紧）决定不做**：`zkde_screencast_unstable_v1`
       保持对所有客户端开放 —— 和 KDE 的策略一致（KWin 也不检查客户端，它只用可执行路径当
       HACK 判断要不要把 portal / plasmashell 自己的窗口从画面里藏掉）。收紧它挡不住真正的
       口子（screencopy / ext-image-copy-capture 同样对所有客户端开放，grim / OBS 靠它），
       而要收就得连它一起收 —— 那会把 grim / wf-recorder / OBS 全废掉。要边界得靠 sandbox
       那一层（别的进程拿不到 wayland socket），不是合成器。

203. **portal 录制不再把显示器帧率拖到 60**（用户报「portal 录制会导致显示器帧率被限制到
      60，哪怕录制的是窗口，portal 录制帧率应该上限允许到显示器帧率上限」）

      录制这一路以前是**每次输出提交都抓一整帧**：`stream_handle_output_commit()` 里直接
      `stream_grab()`（整块离屏渲染）再把缓冲交给 PipeWire。抓帧和回读都在合成器的**主线程**
      上做（PipeWire 的 loop 挂在 Wayland 的 event loop 上，`pw_loop_ready()`），144Hz 的屏上
      就是每秒 144 趟整屏渲染 —— 消费者手上还有空缓冲时每次提交都再回读一整块（4K 上 33MB）。
      主线程被占满，屏幕上所有动画 / 拖动跟着掉到 60fps：用户说的「被限制到 60」是这么来的，
      跟录的是窗口还是整屏无关。

      - 抓帧挪进 `pw_stream_process()`：**消费者真的来要一帧**（`pw_stream_dequeue_buffer()`
        拿得到空缓冲）**而且到点了**才抓。输出提交只做两件便宜事：把手上那一帧作废（录屏要的是
        最新画面）并催一次流。
      - 新增 `stream_send_due()` / `stream_send_interval_ns()`：发帧节奏上限 =
        **min(消费者协商的帧率, 屏幕刷新率)** —— 消费者要的帧率来自 `SPA_FORMAT_VIDEO_framerate`
        （0/1 = 不限），屏幕那一侧取输出的**标称** `refresh`（不用量到的 `frame_gap_ns`：那个会被
        「录制把帧率拖下去」反过来影响，帧间隔变大 → 上限放宽 → 抓得更多，是个正反馈），
        输出没报刷新率的（headless / 虚拟输出）才退回帧间隔；两个都拿不到就不设上限。
        下限 4ms（250Hz），免得怪值把节奏锁死。
      - 光标推帧（`vidow_screencast_cursor_moved()`）跟着同一条上限走：以前写死 16ms（60Hz），
        144Hz 的屏上「指针跟手」就只到 60fps。
      - 新增 `vidowmsg state` 的 `screencast` 那一块：`frames` / `grabs` / `commits` /
        `throttled` / `cap_ms` / `consumer_fps` —— 「是不是又在每次提交都抓整帧」一眼看得出来
        （修好之后 `grabs ≈ frames`，`commits` 可以远大于它俩）。`struct vidow_sc_stream_stats`
        + `vidow_screencast_stream_stats()` 是那一份私有状态对外的口子。
      - 判据：`test-portal-own.sh` 新增 3 条（抓帧次数跟发帧次数一个量级、输出提交次数远多于
        发帧次数、节奏有上限）；53 项全过。

202. **全屏窗口不画服务端装饰（SSD），浮动全屏也占满整块屏**（用户报「全屏窗口应该没有 ssd，
      直接独占显示器区域」）

      - 新增 `vidow_view_look()`（`rule.c`）：`vidow_rule_look()` 的结果上再按全屏收一道 ——
        边框 / 圆角 / 阴影 / 噪点全归零，其余（模糊、不透明度、内容合成）照旧。**不是**关掉
        fx：装饰着色器照跑，只是那几笔参数是 0。
      - 渲染那几处（`view_wants_blur()`、模糊区域选择、`vidow_fx_frame()`）和 IPC（`vidowmsg
        windows` 的 `look`）用的是同一份，自检不用量像素。
      - `vidow_fx_frame()` / `vidow_closing_start()` 多一个 `fullscreen` 参数：**关窗动画的快照**
        也得按同一份参数画（全屏窗口的淡出不该突然冒出一圈边框）。layer 表面一律传 false。
      - 摆位：浮动的全屏窗口以前摆在**工作区**（`work`，状态栏占位之外那一块），现在和列里的
        全屏一样取**整块输出框**（`full_box`）—— 「独占显示器区域」。
      - 判据：`test-fullscreen-scroll.sh` 新增 9 条（非全屏的 look 有边框 2 / 圆角 12 / 阴影 8，
        全屏之后都是 0；左上角那一点是全屏窗口自己的内容而不是圆角外面的背景；浮动的全屏
        geometry = 0,0 1280x720、退出全屏回到原来的浮动尺寸）；47 项全过。

223. **全景里可以换一张壁纸（图层规则 `overview = "only"`，niri 的 `place-within-backdrop` 那件事）**
      （用户：「overview 的背景可以设置成不一样的吗 用 swww（好像改名了，但我忘了软件改成什么名字」
      → 「不能用 layer 吗 我看 niri 有办法」→ 「命名改的直观一点」，取值从 off / only / hide
      改成了 always / only / never）

      - **先说 niri 是怎么干的**：它有一条 `layer-rule { match namespace="^wallpaper$";
        place-within-backdrop true }`，把某块 background 层的 surface 放进 overview 的底衬里，
        官方文档原话是「你可以跑两个壁纸工具（比如 swaybg 和 awww），一个给底衬、一个给普通工作区
        背景」。vidow 这边不需要「搬进底衬」—— **壁纸本来就不跟全景缩放**（缩的只有窗口，
        `vidow_view_apply_anim`），缺的只是一个「什么时候露面」的开关。
      - 新增图层规则作用 `overview`（`src/rule.h` 的 `enum vidow_overview_mode`、`VRA_OVERVIEW`、
        `RK_ACT_OVERVIEW`）：`"always"`（默认，照常）/ `"only"`（只在全景里显示）/ `"never"`（全景里
        不出现）—— 名字读作「全景里：总是 / 只有它 / 从不」。`only` 就是用户要的那件事：**跑第二个壁纸客户端、按 namespace 匹配**。
      - 落在 `overview.c` 的 `overview_layers_apply()`：盖在原来那几刀「整棵树一起开关」
        （popups / top / overlay / im）**上面**，逐个 surface 改自己那棵树的 `enabled`。规则改完
        当场生效（`apply_layer()` 里调 `vidow_overview_refresh_layers()`），不用等下次开关全景。
      - **两个坑**（都是先写错、像素量出来的）：
        1. 开关变了得**重抓底衬**：全景里看到的那张壁纸是**共享底衬那一份**（`overview_blur` 的模糊
           节点画在遮罩下面，盖住 live 的 layer），不 `vidow_backdrop_damage(server, true)` 就还是
           上一张 —— 表现是「进全景了，但糊的还是桌面那张」。第一次跑测试量到 s3 全是渐变色调
           （(20,20)=(71,62,26)），补上这一笔之后是压暗过的洋红 100%。
        2. 规则从 `only` 改回 `always` 时**必须把节点点亮回来**（第一版 `always` 那一支直接 `continue`，那块
           壁纸就永远关着了）。另外 `off` 那一支不能抢 `fx.c` 的 `capture_hidden`（抓底衬时它会
           临时关掉某些 surface 的节点，那一笔不是我们的）。
      - 查询接口跟着修：`vidowmsg background` 多报一个 `visible`（**现在真的画着**几块），
        `outputs[].covered` 只看画着的那几块 —— 不然「全景专用」那块会把常驻 shell 骗过去
        （它会以为这屏有人管了，自己不画）。`vidowmsg layers` 每块多 `overview` / `shown` 两个字段。
      - **证据**（headless 1280x720，两块 background 层 surface：wall.py 渐变 + 纯洋红）：
        桌面 洋红 0% / 金色 16%；进全景 压暗过的洋红 100%；退出 洋红 0% / 金色 16%；
        规则热改成 `always` 当场 洋红 100%（盖住桌面）、改回 `only` 又 0%。
        `test-overview.sh` 50 → 64 项；`tools/wall.py` 顺手支持「颜色 + namespace」两个参数
        （`python3 tools/wall.py "#ff00ff" wall2`），默认行为一个字没动。
      - **真客户端也验过一遍**（`awww` 0.12.1，两个 daemon + 两张纯色图）：`layers` 报出
        `awww-daemon` / `awww-daemonov` 两块，后者 `overview=only shown=False`；桌面金色 100% /
        洋红 0%，进全景 中心 (107,5,110)（压暗 + 糊过的洋红），退出又是金色 100%，规则热改成
        `off` 之后洋红 100% 盖住桌面。
      - 顺带查清的两件事：**`swww` 2025-10-29 改名成了 `awww`**（仓库迁到 codeberg，
        `LGFae/swww` 已归档）。`awww-daemon -n <后缀>` 给的 layer namespace 是
        `awww-daemon<后缀>`（**直接拼**，不是 `-` 连起来）、socket 是
        `$WAYLAND_DISPLAY-awww-daemon.<后缀>.socket`，客户端用同一个值 `awww img --namespace <后缀>`
        —— 但**值别以 `-` 开头**：`awww img` 是 clap，`--namespace -ov` 会被当成短选项
        （报 `a value is required`），`--namespace=-ov` 它认而 `awww-daemon` 那个手写的解析器
        不认（`Unrecognized command line argument`）。所以文档里一律用 `-n ov`。
        `swaybg` 没有 namespace 参数，用不了这招。
      - 另一个踩到的坑（不是 vidow 的）：`awww img` 默认带过渡动画，动画没跑完就进全景，抓到的
        底衬是**半透明混合**的（量到 (107,66,38)，金色和洋红混在一起）。验证 / 截图时加
        `--transition-type none`。

249. **「凭空多出一列」的第二个入口：从没 map 过的窗口也会被塞进一条列**（用户：「proton 游戏还是有一列空列」，2026-10-13 20:48 现场 dump）

     - 现场（用户真机 `vidowmsg windows`，DP-2）：窗口 50、标题 `Input`、`mapped=false`、
       `visible=false`、`float_mode=none`、`placed=true`，可 `column` 在、几何 `2883,6 951x1068`
       —— 一条占满整列的空列，正卡在 lutris（1926–2877）和崩铁（3834–5754）中间。
     - 跟 237 是同一种病、**不是同一个入口**：237 修的是 `view_unmap()`（映射过之后又消失），
       这个窗口**从来没映射过**，`view_unmap()` 一次都没进过。X11 窗口的属性（WM_TRANSIENT_FOR /
       WM_NORMAL_HINTS / `_NET_WM_WINDOW_TYPE`）在它没 map 的时候照样会变 —— proton / Steam
       那批辅助窗就撞在这条路上。
     - 建列的那一步：`src/float.c:85` 的「浮动 → 平铺」重判（`vidow_float_set_mode()` 的
       `VIDOW_FLOAT_NONE` 分支）→ `vidow_view_join_layout()`（`src/view.c:44`）→
       `view_ensure_column()`（`src/view.c:782`）。触发者是 X11 的属性回调（`x11_set_parent` /
       `x11_set_hints` / `x11_set_window_type` / `x11_set_size_hints` → `vidow_float_update_mode()`），
       这一串里没有一个看窗口上没上屏。（规则那条入口 `src/rule.c:1092` 的 `float false` 同样走
       `vidow_float_set_mode()`，被同一道闸拦下。）
     - 为什么占着列却画不出东西、宽度还照占：占用判据是 `view_shows()`（`src/view.c:731`）=
       `!view->minimized` —— **只排掉最小化**，没排掉「没上屏」，于是这条列照算宽度、那个没上屏的
       视图还被 arrange 摆了一遍（所以 `placed=true`、几何是整格）。
     - 修法：`vidow_view_join_layout()` 里 `if (!view->mapped) return;` —— **没上屏的窗口不给列**，
       延迟到 `view_map()`（`src/view.c:3906`）自己补回来。判据只能是「**现在**在不在屏上」，不能是
       「以前上过屏没有」：窗口 unmap 之后再翻一次浮动 → 平铺，照样不该建列（否则空列又回来）。
     - **别把这条判据搬进 `view_ensure_column()`**：xdg 的初始 commit（`src/view.c:4199`）是
       **故意**在 map 之前建列的 —— 客户端要拿着「平铺格的尺寸」这个 configure 才上屏
       （`arrange_columns` → `vidow_place_view` → `vidow_view_set_size`）。拦掉它，每个窗口都会先按
       自己的尺寸冒出来再跳一下，抢跑的最大化请求也兑现在那儿。
     - 验证：新加 `test-emptycol-nomap.sh` + `tools/xnomap.py`（X11 探针，同一进程里两个窗口跑同一
       套属性动作：A **一次都不 map**、B 正常上屏当对照）。**修之前**：A 占着一条列（几何 631 宽）、
       屏上两条列、对照组 B 被挤到 `x=643` —— 就是用户那条空列；**修之后**：A `column=null`、
       `width=0`、屏上只剩一条列、B 还在第一格（6,6 631），A 后来自己 map 上来列又补回来了
       （延迟，不是不给）。`test-fullscreen-scroll.sh` / `test-fullscreen-2out.sh` /
       `test-workspace.sh` 跑前跑后都全过。
     - 未覆盖：**只做初始 commit 却永远不 map 的 xdg 窗口**还是会留下一条空列 —— 那条路不敢拦
       （理由见上一条）。用户现场那个窗口是 x11，不是这一条；另外真机那条空列宽 951（用户屏的
       0.5 档），headless 复现出来是 631（1280x720），数值不同、形状一致。

250. **HiDPI 屏上预览（overview / MRU）的缩略图糊：烘出来的缓冲差一个设备像素，落屏那一步又被双线性采了一次**（用户：「overview窗口显示不清晰的问题还是没解决」；症状是「缩略图里的窗口内容发软，不如屏上那扇窗锐」，MRU 卡片同理）

     - 先量后改（headless 1280x720，`scale = 2`、`gaps_inner = 8`、两个 kitty 同一份文本，
       `grim` 拍「桌面 1:1」和「overview 0.46x」两张，按日志里的窗口框做 1:1 裁切对齐）：
         - 现状：格子里的内容与「把桌面那份内容单步双线性缩到格子尺寸」的模拟图差 **3.72**（同一场景换
           一个窗口宽度，差 5.09），笔画对比 **191.4 / 205.1**，边宽（10-90%）**1.66 / 1.68** 设备像素；
         - 把窗口宽度换成一个让烘图尺寸**正好整除输出缩放**的值：差 **0.34**，笔画对比 **234.6**，
           边宽 **1.46** —— 与「理想 1:1 落屏」的 234.5 / 1.42 一致。
         - 结论：内容那一步（单步 `GL_LINEAR` 缩 2.17:1）没病，病在**烘好的缩略图落屏那一步不是 1:1**。
     - 机制：`ensure_targets()` 烘的缓冲是**设备像素**（`w = lround(cw × scale) + 2 × lround(pad × scale)`，
       `scale = 输出缩放 × ov`），而落屏尺寸是 `dst_w0 = lround(lround(w / scale) × ov)` 个**逻辑像素**
       —— 中间隔着两次 `lround`。输出缩放 2 时只要 `w` 是奇数（`lround(cw × 0.92)` 的奇偶，约一半的窗口
       宽度都踩），`dst_w0 × 2 = w ± 1`，wlroots 就得把这张烘好的图按 0.997x 再双线性采一次，相位沿着
       窗口宽度漂一整个像素 —— 漂到 0.5 像素相位的那一半地方，笔画被抹平。
         - 代价量过：把缩略图缓冲取成差 1 个设备像素、再双线性放大回来（真内容上算），笔画对比
           **234.7 → 176.9（-25%）**、边宽 **1.44 → 1.68**；烘好的图自己没病，是这一步在抹。
         - 为什么以前量不出来：输出缩放 1 时两次 `lround` 正好抵消
           （`lround(lround(w / 0.46) × 0.46) == w`），headless 测试全是 `scale = 1`；用户那块 4K 屏是
           `scale = 2`，这条才露出来。也解释了为什么「预览不叠模糊」（README 246）改完还是糊：
           那一条管的是模糊贴图，跟这一条不是同一个病。
     - 修法（`src/fx.c`，只动烘图尺寸和落屏尺寸这两处）：
         - 烘图前把目标尺寸取成**输出缩放的整数倍**（`snap_to_output_scale(px, os)`，`os = scale / ov`
           由调用点传下来），落屏尺寸直接 `dst_w0 = lround(fx->w / os)` —— 不再走那两次 `lround`；
         - `content_w = w - 2 × px_pad`（`snap` 补上的那一两个像素算进内容，否则着色器里内容右边会多
           一条没画到的缝）；
         - 桌面那条路（`ov = 1`）逐像素不变：`ov = 1` 时 `w = cw × os + 2 × px_pad` 本来就整除，
           `snap` 原样返回，`dst_w0` 也还是 `w / os`。
         - 小数倍率（1.25 / 1.5 / 1.7）**只能救回一部分**：要 1:1 得 `w` 是 `os` 的整数倍，而 `os = 1.7`
           的整数倍每隔 17 个设备像素才有一个、搜索窗只有 8 —— 找不到就原样返回（那种屏上整块屏本来
           就都在重采样，不是这一条独有的）。
     - 改前 / 改后（同一场景：`scale = 2`、`gaps_inner = 8`、窗口 310 逻辑像素宽 → 烘图 299 设备像素，
       奇数）：缩略图与理想 1:1 的差 **3.72 → 0.34**、笔画对比 **205.1 → 234.6**（理想 234.5）、
       边宽 **1.68 → 1.46**（理想 1.42）；`scale = 1` 的老场景改前改后**逐像素一致**（笔画对比 188.0，
       与模拟图差 0.30）。截图与量测脚本在 `/tmp/ovlab/`（`s2odd` = 改前、`s2odd_fix` = 改后、
       `base`/`base_fix` = scale 1 对照）。
     - 覆盖：`test-overview.sh` / `test-blurscale.sh`（scale 1.0 / 2.0 / 1.7 三档）/ `test-scrollcull.sh`
       改前改后各跑一次，全 PASS、退出码 0。

251. **滚一列，窗口被瞬移到屏幕边再滑回来 —— 全屏窗口看着「钉在右边那一列 / 从最左边移入」**（用户：「proton 游戏窗口滚动的时候从最左边移入显示的 bug」「窗口还是会在我滚动的时候一直占着右边那一列（自己和其他窗口交换了位置），且还是会从左边移出来」，2026-10-13；现场录屏 `/tmp/proton-game.mp4`，3840x2160@120）

     - 现场：全屏的游戏窗口自己一条列（DP-2，1920x1080 逻辑）。滚一列它**不跟着条带平移**，而是整块
       消失、再从屏幕某一侧飞进来：往右滚（条带左移）它从**右边**进 —— 看着就是「一直占着右边那一列、
       像跟别的窗口交换了位置」；往左滚它从**最左边**进。242 那条补丁（`ee769a7`）就是为这个症状写的，
       但它没治好，反而把**正常滚动**也一起弄坏了。
     - 机制：补间起点 `drawn_x = view->geom.x + anim_off_x`（`vidow_place_view()`，`src/view.c`），
       242 在后面加了一段：**起点落在输出框里**就把它推到框的一侧 ——
       `drawn_x = x > drawn_x ? box.x - w : box.x + box.width`。
       病在判据：正常滚动时起点**本来就在框里**（它明明画在屏上，只是被布局挪了一格），于是每滚一列，
       每个窗口都被瞬移到屏幕边再滑回来：
         - 往左滚（新位置在旧位置左边）→ 起点 = `box.x + box.width` = **屏幕右缘**；
         - 往右滚 → 起点 = `box.x - w`，而全屏视图的 `w == 整屏宽`，这个值正好是**屏幕最左边**。
       全屏窗口宽 == 整屏，瞬移的位移最大（复现里它飞 1788px、条带只走 508px，3.5 倍速），所以只有
       它显眼；普通列一样被推（复现里 C / D 的起点也被推成 1280），只是窄、飞一下看不出来。
     - 修法：把 242 那段整块删掉，起点回到「上一帧它画在哪」。**parked 的窗口不用特判**：park 只把
       节点摆到屏外的停放位、不动 `geom`/`anim_off`（`view_anim_place()`），而 park 的判据正是
       「画出来的盒子跟自己的输出框一点都不沾」（`vidow_view_should_park()`）—— 所以滑出去时这个值
       必然落在屏外**它进来的那一侧**上，起点本来就对：滚回来它跟条带一起平移，位移跟别的列一模一样。
       不按 fullscreen 分支。
     - 验证（① 用户真机形状：两块屏，`HEADLESS-1` 在 x=0、`HEADLESS-2` 在 x=1280，全屏的 A 在第二块
       上（`1280,0 1280x720`）、它左边还有一条普通列；`window_move_ms = 190`）。修前的二进制是
       `git worktree add /tmp/vidow-prefix HEAD`（= `25c34c3`）单独编出来的，量日志里
       `move tween A: <起点>,.. -> <目标>,..` 的**起点**（`A` 是全屏那条）：
         - 往左滚到 B（A `1280 → 1788`）：修前起点 **0** —— **全局最左边**（第一块屏的左边缘，离它
           自己该在的位置 1280px，正是用户那句「从屏幕最左边移入」）→ 修后 **1280**（它刚才画的地方）
         - 滚回 A（A `1788 → 1280`）：修前 **2560**（第二块屏的右缘，整块在屏外右侧）→ 修后 **1788**
         - 往右滚到 C（A `1280 → 772`）：修前 **2560** → 修后 **1280**
         - 滚到 D / E（A `772 → 264 → -244`）：修前 = 修后 = `772` / `264`（补丁够不着它，本来没坏）
         - 滚到 E 让 A 整块出屏（`visible=false`）再一步滚回 A（`-244 → 1280`）：修前 = 修后 = `-244`
       ② 单屏 1280x720 / scale 1，列序 `B | A(全屏 1280x720) | C | D | E | F`，同样的量法：
         - 往左滚到 B（A `0 → 508`）：修前起点 **-1280**（屏幕最左边 − 整屏宽，整块在屏外左侧）→ 修后 **0**
         - 滚回 A（A `508 → 0`）：修前 **1280**（屏幕右缘，整块在屏外右侧）→ 修后 **508**
         - 往右滚到 C（A `0 → -508`）：修前 **1280** → 修后 **0**
         - 滚到 D（A `-508 → -1016`）：修前 -508 → 修后 -508（同 ①，这条补丁够不着它）
         - 滚到 F 让 A 整块出屏（`visible=false`）再一步滚回 A：修前 -1524 → 修后 -1524
       ③ 逐帧截图（`grim` 连拍，判据 = 窗口内容红/绿交界 − 640 = 它左边缘 x），②那个场景往左滚一步：
       修前 `整屏黑（只有左边 4px）→ -806 → -458 → -180 → 38 → 205`（前两帧屏上根本没有窗口，
       它从屏外左侧飞进来）；修后 `0 → 122 → 221 → 301 → 372 → 421 → 508`（跟条带同速平移，
       屏上一直有窗口）。两边终点都是 508。
     - 覆盖：`test-fullscreen-scroll.sh` / `test-fullscreen-2out.sh` / `test-scrollcull.sh` /
       `test-workspace.sh` 改前（`/tmp/vidow-prefix`）改后各跑一次，全 PASS、退出码 0。
       注意这几个脚本里 `window_move_ms = 0`（不起补间），所以它们**量不到**这条 bug ——
       上面的数字全靠那个 headless 复现（`move tween` 那行日志 + `grim` 连拍）。

254. **全屏游戏滚出视口时「突然从左边平移到右边，且跨显示器了」：属性一抖它就被判成浮动，浮动窗口既不裁也不停**（用户：「proton 游戏移出视口的时候会突然从左边平移到右边（且跨显示器了）」，2026-10-13；现场日志 `/tmp/vidow-run.log`）

     - 机制（哪一行）：proton 游戏**进全屏时把 `WM_NORMAL_HINTS` 钉成 `min == max`（等于它全屏的尺寸）**，离开全屏又松开。
       `src/view.c` 里那串属性回调（`x11_set_hints` / `x11_set_size_hints` / `x11_set_window_type` / `x11_request_configure`，
       加上 `view_map` 和 xdg 的初次 commit）每次变动都调 `vidow_float_update_mode()` →
       `vidow_x11_detect_mode()` 的 `x11_has_fixed_size()`（`src/view.c:4550`）见 min==max 就返回 `VIDOW_FLOAT_FIXED`
       —— **不看全屏状态**（全屏的尺寸是我们定的，客户端那套提示在这时候不代表「它是个对话框」）。
       全屏窗口一变成浮动，两样东西一起丢：
       ① 它那条列（`vidow_float_set_mode()` → `vidow_view_leave_layout()`）→ 条带塌一格，整屏跟着跳；
       ② 「停到屏外 / 裁到自己那块屏」两道闸 —— `vidow_view_should_park()`（`src/view.c:1524`）和
          `view_clip_box()`（`src/view.c:1141`）对 `float_mode != NONE` 一律放行（浮窗本来就允许跨屏，
          见 `README` 的浮动窗口那节）。于是它**按条带上的坐标整块画出来**：1920 宽的全屏窗口在 1920 那块屏上
          滚出视口时，x 正好落在 `0` / `6` / `963` 这种跨两块屏的位置上（`963 = 1920 - 957`，957 = 951 + gaps 6）。
     - 现场：`/tmp/vidow-run.log` 里同一个全屏游戏 `float mode 崩坏：星穹铁道: 0 -> 1` / `1 -> 0` **翻了 175 次**，
       每次都在滚动前后。例：`00:03:12.213` 判成浮动 → `12.214 clip: 崩坏：星穹铁道 不裁（屏框 0,0 0x0，窗口 963,0 1920x1080）`
       → `12.214 全屏: 关` → `12.219 float mode: 1 -> 0`（又回平铺）。窗口盒 `963,0 1920x1080` 正好压在左边那块屏上。
     - 先量后改（headless 双屏 `HEADLESS-1 1920x1080@1 x=0` + `HEADLESS-2 3840x2160@2 x=1920`，`gaps 6`、
       `easing = "out_back 0.18"`、`window_move_ms = 320`，仿 proton 的 X11 客户端：固定尺寸提示 + `_NET_WM_STATE_FULLSCREEN`
       + 一直交帧 + 每 0.4s 重设一次属性）：滚 2 格把游戏滚出视口，`grim` 连拍两块屏各 40 帧，
       数**左边那块屏**上有没有游戏的红 ——
       - 改前：**11/40 帧拍到**（帧 3 红 `x=1782..1919`、149040 像素；帧 9 / 14 / 19 / 20 / 24 / 25 / 30 / 35 / 40 同）
       - 改后：**0/40 帧**（日志里 `float mode game` 一行都没有，几何照旧跟着条带 `963 → 6 → -951 → 1920` 走）
     - 修法（`src/float.c` 的 `vidow_float_update_mode()` 开头一条）：**全屏的窗口不接受自动识别**（规则里的
       `float` 和 Mod+V 那两条显式的路照旧，所以条件里带 `!view->rule_eff.set[VRA_FLOAT]`）。
       这里**保持现状**、不硬掰成平铺：显式浮动起来的窗口全屏之后还得是浮动的（`test-fullscreen-scroll.sh` 那条
       「浮动的全屏窗口也盖住整块屏」就是它）。列就是列：全屏窗口照旧在条带里跟着滚、滚出去照旧停到屏外。
     - 回归：`test-fullscreen-scroll.sh` / `test-fullscreen-2out.sh` / `test-scrollcull.sh` / `test-workspace.sh`
       改前（`/tmp/vidow-park-bug/vidow-before`）改后各跑一次，全过。注意这四个脚本 `window_move_ms = 0`，
       量不到这条（症状不是补间起点，是「该藏起来的窗口没藏」），数字全靠上面那个 headless 复现 + `grim` 连拍。
     - 未覆盖：**非全屏**的真·钉死尺寸的 X11 窗口照旧判浮动（那是它本来的语义）—— 那种窗口不在条带里、不参与滚动，
       位置是它自己的浮动位置，不会跟着视口跨屏。

255. **普通窗口和全屏游戏换位置「动了一下又弹回来」**（用户：「普通窗口无法和游戏窗口交换，不过一直按交换快捷键可以有一定概率卡过去」；追问后补充「窗口动了一下又弹回来」「普通窗口回到原位，游戏窗口还是在视口外」；现场日志 `/tmp/vidow-run.log` 里 `03:14.765 action: swap-window right -> 和相邻列里的窗口换位置`）

     - 结论：**跟 254 是同一条根因**，换位置这件事本身没错（`vidow_swap_window()` 每次都成功、`vidow_arrange()` 也照换后的列序摆了一遍），
       是**换完之后游戏又自己抖了一次属性**，被 254 那条路判成浮动 → `vidow_view_leave_layout()` 把它从换过去的那条列里摘掉 →
       下一次属性抖动又判回平铺 → `vidow_view_join_layout()` 重新入列时**它落在条带末尾（新开一条列）**，不是它换过去的位置。
       于是「换过去的那个普通窗口」被留在原地、游戏跑回条带末尾（视口外）—— 看着就是「弹回来」。
     - 复现（headless 双屏 + 仿 proton 的 X11 游戏：固定尺寸提示 + 全屏 + 一直交帧 + 每 0.4s 重设属性；
       条带顺序 `w2 | w3 | game`，焦点 w3，游戏那一列在视口右边外面，然后 `action "swap-window right"`）：
       - 改前（`/tmp/vidow-park-bug/swap_before2.log`）：换完 0.4s 时 `game` 在列 index 1（x=963，视口里）、`w3` 在 index 2（x=2883）——
         **换成功了**；再过 1.6s（游戏属性抖了一轮）变成 `w3` 回 index 1（x=2883）、`game` 回 index 2（x=3834，视口外）——
         **整个换位被撤销**。日志那一轮：`09.205 float mode game: 0 -> 1` → `09.606 float mode game: 1 -> 0`
         → `09.606 place game -> 3834,0`（条带末尾）→ `09.703 workspace: game 藏起（停到屏外）`。
       - 改后（`/tmp/vidow-park-bug/swap_after.log`）：0.4s 和 2s 两次 dump 都是 `game` index 1（x=963）、`w3` index 2（x=2883），
         日志里 `float mode game` 一行都没有 —— 换位站住了。
     - 改动：**没有新代码**，就是 254 那一处（`src/float.c` 的 `vidow_float_update_mode()`：全屏窗口不接受自动识别）。
     - 未覆盖：`vidow_view_join_layout()` 重新入列时**落在条带末尾**这条语义本身没动（它只在这条路上被误触发）——
       如果以后有别的路把窗口摘出布局再放回来，还是会落在末尾。要改的话是另一个决定（入列位置记在哪儿）。

256. **焦点跟随光标 + 键盘换焦点把光标带过去（niri 的 `focus-follows-mouse` / `warp-mouse-to-focus`）**（用户要求照 niri 补这两个开关；键名 / 取值 / 默认值都照 niri 的 `input` 那一节抄）

     - niri 那边是 `input { focus-follows-mouse }`（布尔，也可以写成带 `max-scroll-amount` 子项的表）和
       `input { warp-mouse-to-focus }`（布尔，也可以写成带 `mode` 子项的表）—— 出处是 niri 配置文档的 `input` 一节
       （`niri-config` 的 `Input`：`focus-follows-mouse` / `max-scroll-amount` / `warp-mouse-to-focus` / `mode`）。
     - vidow 里一一对应，**默认都不开**（`false` / 空串）—— 也就是现有行为一个字没变（niri 的默认配置里这两个节点
       也是注释着的）：

       ```lua
       focus_follows_mouse = true                       -- 指针悬停到哪个窗口就聚焦哪个（默认 false = 不跟）
       focus_follows_mouse_max_scroll_amount = "10%"    -- 悬停聚焦允许带出多少滚动（默认 "" = 不限）
       warp_mouse_to_focus = true                       -- 键盘换焦点时把光标挪到新窗口上（默认 false）
       warp_mouse_to_focus_mode = "center-xy"           -- 光标挪到哪（默认 "" = X / Y 分别判断）
       ```
       族写法（照 niri 的节点样子，都能当平铺键的另一种写法）：`focus_follows_mouse = { max_scroll_amount = "10%" }`、
       `warp_mouse_to_focus = { mode = "center-xy" }`。和 vidow 别的族一个规矩，**表写法不会顺手把开关打开**
       （`border = { width = 0 }` 也不开边框），要开就再写一行标量。
     - 取值就是 niri 那套：`max_scroll_amount` 只认百分比写法（结尾必须就是 `%`，`"10%"` = 工作区宽度的 10%），
       `"0%"` = 只有已经整个在屏上的窗口才跟，留空 = 不限；`mode` 只认 `center-xy` / `center-xy-always`，
       留空 = X / Y 分别判断（只往需要的那个轴挪，光标本来就在窗口里就整个不动）。写错当场 WLR_ERROR +
       该行不生效（值保持不变，不静默退回默认）。两个值 `vidowmsg config <键>` 都能原样读回来。
     - 鼠标悬停聚焦**不给全屏窗口开特例**：全屏也是窗口，指针压上去照跟（「状态变化不许抢焦点」那条规矩管的是
       窗口自己变全屏时的那次状态变更，不是这里）。键盘换焦点带光标只走**键盘 / 动作**那条路（keybind.c 的动作 +
       MRU 切换器 + IPC 的 `focus-column` / `focus-window-direction`）；悬停聚焦不带光标 —— 光标本来就在窗口里。
     - 验证（headless 1280x720 一块屏，两扇窗 A=`6,6,631,708` / B=`643,6,631,708`，开窗后焦点在 B；用 `wev`
       当窗口是因为它的 surface 填满窗口，命中测试才覆盖整扇窗 —— `build/frames` 的 surface 只有 320x200 被拉满，
       只有左上角那块能点中）：
       - `focus_follows_mouse = true`：`move-cursor 321 360`（A 中心）→ `vidowmsg focused` = A；再 `move-cursor
         958 360`（B 中心）→ 焦点回 B。跟了。
       - 默认（不开）：光标初始 `100,100` 就压在 A 上，焦点仍是 B；悬停 A 中心、再悬停 B 中心，焦点一直是 B。
         没变。
       - `warp_mouse_to_focus = true`：光标先放 B 中心 `958.5,360`，`focus-column left` → 焦点 A、`vidowmsg cursor`
         = `321.5,360`（= A 的窗口中心）；`focus-column right` → 焦点 B、光标 = `958.5,360`（= B 的中心）。
         `action "focus-column left"`（键位走的那条路）给的是同一个值。
       - `warp_mouse_to_focus_mode = "center-xy-always"`：光标放在 A 里的 `26,26`，再 `focus-column left`
         （A 已经最左、焦点没变）→ 光标照样挪到 A 中心 `321.5,360`；默认模式（留空）这一下光标停在 `26,26` 不动。
       - 全屏：B `toggle-fullscreen` 后 `0,0,1280,720`，`focus-column left` 聚焦 A（B 滚到 `637,0`，右半屏还在屏上），
         悬停 `1037,360`（在 B 的可见区、A 之外）→ 焦点回到 B。全屏照跟，没特例。
     - 顺手补的一处：`vidowmsg focus-column` / `focus-window-direction` 这两条 IPC 命令原来直接调
       `vidow_focus_column()` / `vidow_focus_window()`，没经过 keybind.c 那条动作路，所以「键盘换焦点带光标」在
       IPC 这个入口上不生效（`vidowmsg action "focus-column left"` 生效、`vidowmsg focus-column left` 不生效）。
       在 ipc.c 那两个命令里各补一行 `vidow_warp_cursor_to_focus()`，两个入口就一致了 —— 开关没开时它第一行就
       return，零行为变化。
     - 回归：`test-config.sh` / `test-ipc.sh` 改前改后都全过；`test-defaultconfig.sh` 补文档**之前是红的**
       （4 个平铺键 + 族项 `max_scroll_amount` 没写进 config.example），把 `13-cursor.lua` 按
       `--default-config 13-cursor.lua` 重新生成、并给 `section_groups[]` 补上这两族的表写法提示之后全过。


257. **Xwayland 退出重启后剪贴板桥全废：X 侧一复制就 SIGSEGV（`clip->conn` 是野指针）+ Wayland→X 彻底不通（`xwayland->seat` 被 xwm 清空）**（用户报「Xwayland 重启后 X 那边复制就崩、Wayland 复制的 X 客户端拿不到」；2026-10-14；复现脚本 `/tmp/clipsync/restart.sh`）

     - 背景：wlroots 给 Xwayland 传的是 `-terminate`，**最后一个 X 客户端一走它就退出**；懒启动模式下
       wlroots 复用的是同一个 `wlr_xwayland_server` 对象，下一个 X 客户端连上来时再 fork 一个新的 Xwayland
       （日志 `[xwayland/server.c:215] Restarting Xwayland (lazy)`）。剪贴板双向同步本身没问题
       （X11→Wayland / Wayland→Xwayland 逐字节一致、不循环），坏的是「重启」这一下。
     - 现象 ①：重启后**下一次 X 侧 owner 变化就段错误**。core 栈：`xcb_convert_selection` ← `fetch_start`
       （改前 `src/clipboard.c:909`）← `x11_event`（改前 `src/clipboard.c:1454`）。
     - 现象 ②：重启后 `wl-copy` 复制，`xclip -o` 是空的；日志持续刷
       `not handling selection events: no seat assigned to xwayland`。
     - 根因 ①：`clip->conn` 指着 xwm 那条 xcb 连接，Xwayland 退出时 wlroots 在 `xwm_destroy()` 里
       `xcb_disconnect()` 掉它 —— `clip->conn` 当场成了野指针；而 `x11_attach()`（改前 `src/clipboard.c:1581`）
       第一行的 `if (clip->conn != NULL ...) return;` 让重启后再也不接管（日志里「接管 X11…」全程只出现 **1 次**），
       可事件钩子 `user_event_handler` 还挂着：新 xwm 的事件一进来就拿着旧连接发请求。
       **关键事实：懒启动模式下 wlroots 不为「Xwayland 进程退出」发任何信号** ——
       `wlr_xwayland_server::events.destroy` 和 `wlr_xwayland::events.destroy` 都只在 `wlr_xwayland_destroy` /
       display destroy 时发（上面那条日志是 `handle_client_destroy()` 里重启懒启动打的，跟 destroy 无关）。
       所以「什么时候摘桥」只能在**使用点对账**，不能靠信号。
     - 根因 ②：wlroots 的 `xwm_destroy()` 会 `wlr_xwayland_set_seat(xwayland, NULL)` 清空 `xwayland->seat`
       （顺带把 seat 上那份 X 来的选区撤掉），而 vidow 只在 `vidow_xwayland_apply_config()` 建的时候设过一次 ——
       重启后 `xwayland->seat` 一直是 NULL，xwm 于是拒掉所有选区事件。
     - 修法 ①（`src/clipboard.c`）：
       - 把 `vidow_clipboard_finish()` 里那段拆解抽成 `vidow_clipboard_x11_detach()`（`src/clipboard.c:1612`）：
         放掉 `clip->conn`、摘 `user_event_handler`、把 X 侧还等着的 `out` 全收掉（`waiting=false`，`out_destroy`
         就不会去碰死连接）、`fetch_reset()` + `x_owner = NONE`（老 X 服务上的窗口号留着会误判）。
         **只有「当前 xwm 用的就是这条连接」时才对它发 `xcb_destroy_window`** —— 否则碰它就是 UAF。
         不动 `clip->active`：那是「配置里开着桥」的账，跟 X 侧连接的死活无关。
       - 新增 `x11_conn(clip)`（`src/clipboard.c:1655`）：`clip->conn` 和当前
         `wlr_xwayland_get_xwm_connection()` 对不上就当场摘桥并返回 NULL。所有会碰 X 侧的路都先过它：
         `x11_event`（事件入口，段错误那一处）、`sel_flush_outs` / `sel_tick`（Wayland 复制和定时器也会往 X
         回数据，`out_send` 里要 `intern_atom`）、`vidow_clipboard_apply_config`。
       - `x11_attach()` 改成「连接不一样就先摘掉再接管」；`xwayland_destroy()`（`src/xwayland.c:515`，X 服务真被
         拆掉时）也调一次摘桥。
     - 修法 ②（`src/xwayland.c:434` 的 `xwayland_ready()` 里一行）：重启后补一次
       `wlr_xwayland_set_seat(server->xwayland, server->seat)`。wlroots 的 `wlr_xwayland_set_seat` 会先
       `wl_list_remove` 旧监听，重入安全；此时 `xwayland->seat` 已是 NULL，不会重复挂 listener。
     - 复现与验证（`/tmp/clipsync/restart.sh`：headless（`WLR_BACKENDS=headless`、独立 `XDG_RUNTIME_DIR`、
       等日志 "vidow ready"），三阶段 = 有 X 客户端时双向 → 杀掉所有 X 客户端等 14s（Xwayland `-terminate`
       自己退）→ 重启后再测；每阶段 `wl-paste` / `xclip -o` 各读一次）：
       - 改前：阶段 1 双向正常（`WL-BEFORE` / `X11-BEFORE`）；阶段 3 `wl-copy` 后 `X=[]`（不通），
         接着 `xclip -i` 直接 SEGV（`coredumpctl list` 里两条 `/home/busyo/Dev/Vidow/build/vidow` 的 SIGSEGV，
         00:43:34 / 00:47:01），日志刷 `no seat assigned to xwayland`。
       - 改后：三阶段全通 —— 阶段 1 `WL=[WL-BEFORE] X=[WL-BEFORE]`，再 X11 复制
         `WL=[X11-BEFORE] X=[X11-BEFORE]`；阶段 2 `vidow 活? yes`；阶段 3 `WL=[WL-AFTER] X=[WL-AFTER]`，
         再 X11 复制 `WL=[X11-AFTER] X=[X11-AFTER]`，`vidow 活? yes`。日志里
         `no seat assigned to xwayland` **0 行**，「接管 X11…」出现**第 2 次**（`00:00:22.764`），
         摘桥那行 `剪贴板: X 侧连接已摘（等 Xwayland 起来再接管）` 在 `00:00:22.673`（新 Xwayland 刚起来、
         第一个事件进来时）；`coredumpctl list` 没有新增 core。
     - 回归：`./test-clipboard.sh` 改前 111 PASS / 0 FAIL（EXIT=0）→ 改后 111 PASS / 0 FAIL（EXIT=0）。
     - 未覆盖：Xwayland **起来不到 5 秒就死**的话 wlroots 不安排懒重启（`handle_client_destroy()` 里
       `time(NULL) - server_start > 5` 才重启），那种情况下桥会一直摘着，直到配置 SIGHUP（`apply_config`
       会重新接管）或下一个 X 客户端把 Xwayland 再拉起来。

261. **列宽预设 / 新列默认宽度 / 焦点列居中策略可配了（`configs/20-layout.lua`）**（用户：「照 niri 语义补 `center-focused-column` + 可配列宽预设」；2026-10-14）

     - 改前：这三件事**全写死在 `src/view.c`** —— 列宽预设 `static const double presets[] = { 1.0/3.0, 0.5, 2.0/3.0 }`
       （`vidow_cycle_column_width`）、新列宽度 `#define DEFAULT_COLUMN_WIDTH 0.5`（`view_ensure_column`）、
       焦点列居中 `output_arrange` 的 Pass 2 只做「最小滚动」（焦点列贴视口左 / 右边缘）。README 第 13 条边界
       （「列宽、按键、间隙仍是硬编码」）里那条，现在只剩按键。
     - niri 那边的确切取值与出处（[Configuration: Layout](https://github.com/niri-wm/niri/wiki/Configuration:-Layout)，
       以及仓库里的 `resources/default-config.kdl` 逐字核对）：
       - `center-focused-column` 三个取值：`"never"`（**niri 的默认**，注释原文：「focusing an off-screen column will keep
         at the left or right edge of the screen」）/ `"always"`（the focused column will always be centered）/
         `"on-overflow"`（focusing a column will center it if it doesn't fit together with the previously focused column）。
       - `preset-column-widths { proportion 0.33333 / proportion 0.5 / proportion 0.66667 }` —— 默认档位就是
         「The default preset widths are 1/3, 1/2 and 2/3 of the output」；每项还能写 `fixed 1920`（固定逻辑像素）。
       - `default-column-width { proportion 0.5; }`；**空括号 `{}` = 让窗口自己决定初始宽度**。
     - vidow 侧三个新键（snake_case，取值 / 语义照 niri 抄）：
       ```lua
       center_focused_column = "never"                                  -- never / always / on-overflow
       preset_column_widths  = { 0.3333333333, 0.5, 0.6666666667 }      -- cycle-column-width 循环的那几档
       default_column_width  = 0.5                                      -- 新列多宽（窗口规则 column_width 优先）
       ```
       **默认值 = vidow 以前的行为，不配置零变化**：三个默认数就是原来写死的那三个。比例写成字面量
       （`0.3333333333`）而不是 `1.0/3.0`，是为了「打出来的默认配置读回去还是同一个 double」。
     - 三条写路径都吃（照「可配置缓动」508239e 那一套）：config.lua（`preset_column_widths` 写成 **Lua 数组**，
       新增 `K_LIST` 类型 + script.c 的 `list_as_text()`）/ `VIDOW_CENTER_FOCUSED_COLUMN=always`、
       `VIDOW_PRESET_COLUMN_WIDTHS=0.25,0.5,0.75`、`VIDOW_DEFAULT_COLUMN_WIDTH=0.6` / `vidowmsg set-config
       preset_column_widths "0.25, 0.5"`；查询 `vidowmsg config <键>`（数表打回 `"0.3333333333, 0.5, 0.6666666667"`，
       逗号分隔的那种文本，拷回配置文件 / 环境变量都能原样再读一遍）。写错当场 `WLR_ERROR` + 该行不生效
       （`center_focused_column = "sometimes"`、`{ 0.25, 1.5 }` 越界、空表、超过 16 档都拒）。
     - 居中策略落在 `output_arrange` 的 Pass 2，抽成 `focus_viewport_target()`（`src/view.c`）：
       `always` 把焦点列中心对到视口中心；`on-overflow` 看「焦点列 + **上一个焦点列**」合起来的跨度
       （`server->last_focused_col`，换焦点时更新、列被 free 时清空）能不能整个放进视口，放不下才居中；
       `never` 就是原来的最小滚动。三种最后都夹到 `0..max_viewport`（条带两头滚不动，跟 niri 的夹取一样），
       所以最左 / 最右那条列居中不了 —— 这是条带边界，不是特例分支。
       `vidow_view_scroll_amount_to_activate()`（focus-follows-mouse 的 `max-scroll-amount` 拿它判阈值）改成调同一个
       `focus_viewport_target()`：**预测和实际必须是同一件事**，否则居中一开那条判据全错。
     - 列宽预设：`vidow_cycle_column_width()` 读 `server->config.preset_column_widths`（+ `_count`），
       「现在是第几档」仍然按**目标** `width_fraction` 数（连按不因动画没跑完原地打转）；表为空兜一个 0.5。
       `view_ensure_column()` 用 `config.default_column_width` 取代原来的 `DEFAULT_COLUMN_WIDTH` 宏。
     - 验证（headless 1280x720 一块屏、`gaps = 10` → 工作区 1260 宽、视口中心 640；脚本
       `/tmp/vidow-layout-verify.sh` + `/tmp/vidow-overflow-verify.sh`，数字是「焦点列中心 − 视口中心」）：
       - 4 条列（0.5 = 625px，offset 0/635/1270/1905）：`never` 全程 ±317.5（贴边，改前就是这个数）；
         `always` 第 2、3 列 **0.0**（正中），第 1、4 列 ∓317.5（条带两头夹住，滚不动）；
         `on-overflow` 三条都是 ±317.5 —— 两条相邻 0.5 正好 1260 = 视口宽，**放得下**，不该居中（niri 的判据）。
       - `on-overflow` 专项（3 条列、`default_column_width = 0.6` → 752px，两条相邻 = 1514 > 1260）：从最左跳到第 2 列
         **中心差 0.0**（居中，视口 508）、第 3 列 +254（夹到 max_viewport 1016）、从第 3 列回第 2 列又 **0.0**；
         同一个布局下 `never` 是 +254 / +254（不居中）。0.6 的 `never` 与 `on-overflow` 差异就是「溢出才居中」这一条。
       - 预设：`preset_column_widths = { 0.25, 0.5, 0.75 }` → `cycle-column-width` 循环 0.75 → 0.25 → 0.5 → 0.75
         （宽度 942 / 307 / 625 / 942）；默认配置下是 0.6667 → 0.3333 → 0.5 → 0.6667（836 / 413 / 625 / 836）——
         和改前写死那三档一致。`default_column_width = 0.6` → 新列宽 752 = `0.6 × (1260+10) − 10`。
       - 三个键 `vidowmsg config` 都查得回来（默认分别是 `never` / `0.3333333333, 0.5, 0.6666666667` / `0.5`）。
     - 回归：`test-widthanim.sh` 全过（39 PASS，含「列还原停在 836 / `width_fraction` 0.666667」）、`test-workspace.sh` 全过、
       `test-defaultconfig.sh` 全过（新增的 `20-layout.lua` 与 `--default-config` 逐字节一致，114 个平铺键都在）。
     - 未做：niri 的 `fixed 1920`（固定像素档）、`preset-window-heights`、`default-column-width {}`（让客户端自己定宽）、
       `always-center-single-column`；窗口规则级的 per-rule 预设覆盖。

290. **媒体键能绑了：键名解析加 keysym 兜底；裸键开 XF86 段 + `{ bare = true }`；锁屏放行 `{ locked = true }`**（用户：「媒体键怎么解决」；2026-10-15）
     - **根因**：键名只有一张固定表 + `F1..F35` + `XF86Switch_VT_1..12`，`XF86AudioRaiseVolume` 这种名字落在表外 → 报
       `unknown token`；就算解出来，紧接着还有一道**裸键拒绝**（不带修饰键的键盘触发一律不抢），媒体键天生没修饰键，照样绑不上。
     - **修法**：① `key_sym()` 表查不到时交给 `xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE)` —— libxkbcommon 自己的
       keysym 表认识全部 XF86 名字（媒体键 / 亮度键 / `XF86Tools` / `XF86Calculator`…），不必在 vidow 里抄一份（抄漏一个就是一个
       「这个键绑不了」）；解不出来照旧报 `unknown token`，不静默。② 裸键拒绝加两个例外：**XF86 段自动放行**（`0x10080000..0x1008FFFF`，
       没有别的消费者），其它键要裸绑得显式写 `{ bare = true }`（`print` / `F2` / `F9` / `F13` 这种 —— 默认不动别人的键）。③ 新增绑定选项
       `{ locked = true }`：会话锁定期间照样生效（音量 / 亮度 / 播放控制），`keybind.c` 的锁屏白名单从「只有 switch-vt」变成
       「switch-vt + 标了 locked 的」。按住连发不用管：`repeatable` 默认 true，和 Hyprland 的 `repeating` 同义。
     - 判据：`vidow.bind("XF86AudioRaiseVolume", …)` / `XF86AudioPlay` / `XF86MonBrightnessUp` / `XF86Tools` 全部注册成功（`vidowmsg keybinds`
       里 `locked` 字段跟着报出来），`print` / `F2` / `F9` / `F13` 加 `{ bare = true }` 也注册成功；不带 `{ bare }` 的裸 `print` 仍然报
       「refusing to grab a bare key; 要裸绑就写 { bare = true }」。
     - 落点：`src/keybind.c`（`key_sym` 兜底、裸键例外、`bind_locked_allowed`）、`src/keybind.h`（`locked` 字段 + `vidow_keybind_add` 多两个参数
       `locked` / `bare`）、`src/script.c`（`vidow.bind` 选项解析）、`src/ipcstate.c`（`keybinds` 报 `locked`）。
289. **小窗拖拽：屏幕左右那两片区域进不去 —— 盒子被夹在工作区里（用户：「我说的是小窗在屏幕上拖拽的时候 左右屏幕区域会把小窗卡住不让过去」+「卡在离屏幕边缘还有一段距离的地方」+ 期望「能拖出屏幕边缘（允许一半在屏外）」；2026-10-15）**
     - **根因**：FREE 的位置语义是**比例锚点**（唯一换算点 `vidow_mini_resolve_geom`），而两处夹取把盒子锁在工作区里 ——
       ① `float.c` 的 `mini_free_anchor_from()` 把 FREE 的锚点夹在 `0..1`；② `vidow_mini_resolve_geom()` 的兜底又把盒子夹回工作区（FREE 只是 margin = 0）。于是盒子**左边缘**最多停在 `work.x + span`（span = 工作区宽 − 盒子宽），
       离屏边还差**一整个盒子宽**：实测 1920 屏（工作区 1908、盒子 763）光标顶到 1919，盒子还卡在 `1151 = 1908 - 763` —— 用户说的「左右屏幕区域」就是那两片永远进不去的区域。
     - **修法**：同一块屏内拖动允许**出屏半个盒子**。锚点范围从 `[0, 1]` 放开到 `[-w/(2*span), 1 + w/(2*span)]`，上限再夹 0.5 跟 resolve 的 `-0.5..1.5` 对齐（盒子接近满屏时 `span` 很小，`w/(2*span)` 会远超 0.5 ——
       存一个 resolve 摆不出来的锚点，拖动就会「盒子先停、锚点还在涨」，往回拖要先空转掉那一段）；`vidow_mini_resolve_geom` 的兜底同步放开；拖动夹取（`float.c` 的 MOVE 分支）对小窗再放开 `cw/2`。
       **跨屏改归属那一支照旧夹 `0..1`**（上一轮用户要的「跨显示器小窗应该移动到临近边」：进新屏贴新屏边缘，不是弹到远边）；改大小那一支放开（拖出屏之后再改大小不该弹回屏里）。普通浮窗照旧整个夹在布局包围盒里（看不见的窗口找不回来）。
     - 判据：盒子实测能从 `-375` 拖到 `1532`（工作区 1908、盒子 763 —— 两边都正好一半在外）；`test-mini.sh` 新增「拖到屏右：右边缘 2802 越过工作区右边界 2550 共 252px ≈ 半个盒子」；跨屏那两条（拖动中改归属 + 贴新屏左边缘）仍绿。
     - 判据：`test-mini.sh` **163 PASS / 0 FAIL**（新增 1 条）、`test-drag.sh` **10 PASS / 0 FAIL**。
     - 对照动图：`docs/mini-drag-offscreen.gif`（上 = 修复前 `90d9e46` 的二进制：拖到屏边就停住，右边缘最多贴到红线 1274；下 = 修复后：右边缘到 1527，出屏 253px ≈ 半个盒子。红线 = 工作区右边界）。
288. **小窗左右「死区」= 命中盒只有内容那一条；顺带补上平铺拖拽的贴边自动滚条带**（用户：「不对 不是滚条带啊（不过滚条带有 bug 也要修复） 是小窗拖拽范围有左右死区，拖不过去，窗口拖拽会直接卡在范围内」；2026-10-15）
     - **A 真凶：迷你态的命中盒 = 内容（contain 之后）那一条，不是整个盒子**。内容是按比例摆在盒子**中间**的（实测盒 353x196、内容 544x355 → 缩进后只占中间约 300 宽，左右各留 ~26 像素；内容更窄时留白能到 130 像素），而命中测试只认内容节点 ⇒
       左右留白按下去不算窗口：`pick-window` 网格扫描 11 列只有中间 5 列命中（改之前），拖也拖不动 —— 用户说的「左右死区 / 窗口直接卡在范围内」就是这条。修法：`vidow_view_at()` 里迷你态单独判一次**整盒**命中
       （`src/view.c` 的 `mini_box_view_at`，盒子取 `float_geom + anim_off`，和摆位、和拖拽夹取同源），命中坐标照旧从内容节点换算（`vidow_view_hit_scale`）；**全景打开时不算这一笔**（那时小窗画在全景网格里、`float_geom` 还是全景之前的盒子 ——
       test-overview 的「格子右上内缩 4px 命中它自己」就是这么挂的）。判据：贴左边缘 3px / 盒心 / 贴右边缘 3px 三处全命中；按在左留白（盒 x+6）上拖，盒子跟手 `2189 → 2089`（差 100 ✓）。
     - **B 平铺拖拽补上 niri 的「贴边自动滚条带」**。条带比屏宽时，屏幕外那些列（和它们之间的落点）光靠推光标永远够不到：光标顶到屏幕最右，最近的那条缝还是原处 ⇒ 松手落回原位（用户报的「拖过去被弹回」）。
       现在光标压进工作区左右边缘那条带（40 逻辑像素）就按秒速推视口（压在最边上 1600 px/s，按压入深度线性），**停着不动也继续滚**（推进在每帧 —— `src/output.c` 的渲染里调 `vidow_tile_drag_scroll_step`，滚完自己再要一帧，直到光标离开边缘或松手）；
       这期间视口归拖拽（`src/drag.c` 的 `vidow_tile_drag_autoscroll` + `src/server.h` 的 `drag_scroll / drag_viewport / drag_scroll_speed`），arrange 不跑「把焦点列滚回可见区」（照跑的话刚滚出去看落点就被拽回来），松手交回 arrange 并按新的焦点列重算；
       顺手把所有屏清一遍 —— 哪条退出路径漏了清都不会把视口永久锁在拖拽手里。判据：光标停在左边缘**不动**，视口 `635 → 282 → 35 → 0`；松手落在第 1 条列；反向同理（`0 → 635`、落在第 5 条列）；松手后焦点一动视口跟着走。
     - **C 测试侧**：`test-drag-insert.sh` 新加的那一节自己踩了两个坑 —— ① `wins` 里的 `x` 是**屏上**坐标（已经减过视口），我按条带坐标算按下点 ⇒ 光标被 warp 到屏外（`wlr_cursor_warp` 不认屏外点、原地不动）⇒ 抓到的是光标底下那个窗口；
       ② 条带比屏宽时新窗口是插在**最前面**的，不能按起窗口的顺序猜谁在最左。改成「从布局反查标题 + 用屏坐标 + 条带位置自己加回视口」；另外 `start_drag` 的第 4/5 个参数（抓点偏移）在 `set -u` 下不能省。
       还有一处基础设施：`exec 9>&-`（关测试 FIFO）原本在像素那节之前就关了，**后面任何一节都再注入不了输入**（合成器收到 HANGUP 就把事件源摘了，重开 FIFO 也没人读）—— 挪到文件末尾。
     - 判据：`test-mini.sh` **162 PASS / 0 FAIL**（新增 2 条）、`test-drag-insert.sh` **30 PASS / 0 FAIL**（新增 7 条）；回归 `test-overview.sh` 104/0、`test-drag.sh` 10/0、`test-mru.sh` 103/0。
287. **自由小窗（FREE）：半个窗口「看得见点不着」+ 跨屏拖动被旧尺寸夹住 + 三条判据自己写错**（用户：「小窗移动范围受限，没有全屏幕移动，左右有不可移动区域」+「跨显示器小窗应该移动到临近边 现在会弹到同向边」+「自由小窗默认应该不限制比例，kitty 测试改变大小的时候被限制了比例」；2026-10-15）
     - **A 自由小窗是「正常浮窗」的语义**（本轮主体，`mini_preserve_buffer` / `mini_aspect` / `mini_edge_margin` 三处语义跟着收窄）：
       `mini_preserve_buffer` 只管**迷你 / 图标**态（缩略图才需要保留原生 buffer、合成器缩放显示）；FREE **总是按盒子尺寸 configure 客户端**，客户端跟着重新排版 ——
       用户报的「改大小被限制了比例」就是保留 buffer 那条路（改盒子只改留白、内容比例一动不动）。`mini_aspect` 默认从 `"auto"` 改成 **`"free"`**；
       `mini_edge_margin` 不吃（FREE 要能贴到工作区的边上，留白只留给迷你态）；拖动中跨屏**当场**换归属（不等松手），锚点按「光标在新屏工作区里的相对位置 + 抓取偏移不变」算 ——
       从右边进新屏贴**新**屏左边缘，不再是「按旧屏算出的锚点被夹到远边」那个「弹到同向边」。resize 分支回写 `mini_wf/hf` + `float_want_w/h` + 重配客户端。
       判据：`665x740 → 544x355`（客户端真被重配）、surface `345x318 → 445x318`（宽 +100）、内容比例 `1.085 → 1.399`（不是只改留白）。
     - **B FREE 的内容节点没被钉成 1:1 ⇒ 命中盒只有盒子的一半**（`src/view.c` 的 `view_anim_place()`）：那一支只在「**离开**缩放态」（`else if (view->mini_scaled)`）时把内容节点的 `dst` 钉成表面自己的逻辑尺寸；
       A 之后 FREE 不再保留 buffer（`view_mini_scale()` 直接 1.0）⇒ 这一支永远不进，`dst` 交给 wlroots 每次 commit 按「缓冲像素 ÷ 缓冲缩放」重算 —— 实测逻辑 `544x355` 的表面被设成 **277x355**。
       屏上画的是装饰图（fx）所以看着没错，但**命中测试认的是内容节点**：盒子 `766..1270`，命中盒只有 `746..1023`（277 宽）⇒ 点盒心 `1018,167` 命中不到窗口，客户端一个 motion / 滚轮都收不到（「左右有不可移动区域」）。
       修法：条件放宽成「离开缩放态**或**本来就在小窗态」（位置还原仍只在 `was_scaled` 时做 —— FREE 的位置归 wlroots 按 CSD 留白摆，别动）。
       判据：`dst=544x355`、网格扫描命中盒 = 整个盒子（11/11 列）、FREE 态客户端收到 motion 且坐标 `272,177` = 盒心 640,360 − 盒原点 388,203 + CSD 20px。
     - **C 跨屏拖动时夹取用的是按下那一刻的盒子尺寸**（`src/float.c` 的 `vidow_float_grab_update()` MOVE 分支）：位置夹在「所有输出的包围盒」里时用的是 `server->grab_geom`（按下时的尺寸），
       而拖动中跨屏会当场换归属、盒子按新屏重算（`504x315 → 248x153`）⇒ 旧尺寸把盒子卡在半路：实测右边缘停在 `1416 = 1920 - 504`，离布局右边界还差 248 像素。
       修法：夹取改用盒子**当前**尺寸（位置本身照旧按 `grab_geom` 推，抓取偏移不变才是「拎着走」）。判据：光标 `1900` → 盒原点 `1648 = 1900 - 252`。
     - **D `test-mini.sh` 三条判据自己错了**（产品行为是对的，判据量错了东西）：① 读 `probe: configure` 读的是**合成器**日志 —— 客户端收到的事件不在那儿，于是永远 0 条、判据空转，改用探针日志 `psince`；
       ② scale 2 那段把光标注入到 `y=360`，而 scale 2 屏的逻辑高度只有 360 ⇒ 这个点不在**任何**输出上，`wlr_cursor_warp` 直接不认（光标原地不动，之后每次 motion 都等于没动，看着就像「拖不动」）—— 真鼠标到不了这种点，改用 `y=180`；
       ③ 「进屏贴左边缘」的读数挪到**刚进屏那一刻**（再往下走盒子会跟着跟手，那是下一条判据），盒尺寸期望改成「记住的比例 × 新工作区」`199x135`（`mini_remember_size` 记的是比例不是像素）。
     - 判据：`test-mini.sh` **160 PASS / 0 FAIL**（本轮开头 151 PASS / 8 FAIL）；回归 `test-drag.sh` 10/0、`test-overview.sh` 104/0、`test-mru.sh` 103/0、`test-defaultconfig.sh` 47/0。
286. **overview：滚轮一次一格（巡航到一个工作区）+ 全景里点小窗没焦点 + 有 mini 同屏时命中盒吞掉邻列**（用户：「overview滚轮操作应该是巡航一次滚到一个工作区。而且为什么overview切窗口焦点的操作无效」；2026-10-14）
     - **A 滚轮以前是像素平移，而且不看方向轴**：`vidow_pointer_axis` 里写死 `vidow_overview_scroll(server, ±60)` —— 实测一格 `570.7 → 630.7`（+60，撞上 max），
       而行距是 `row_h + OV_ROW_GAP` = **337.7**，要滚 6 下才走一行，滚到哪儿停下全看滚了几格、永远对不齐行；横向轴（滚轮左右倾）也滚同一个列表。
       修法：`vidow_overview_scroll(server, int steps)` 改成**走几行**，落点先把当前位置吸附到行距整数倍再走 `steps` 步（到顶 / 到底夹在 `[0, max_scroll]`）；
       `src/input.c` 只认 `WL_POINTER_AXIS_VERTICAL_SCROLL`，横轴吃掉不动作（概览里没有横向可滚的东西）。
       判据：`630.72 → 337.68`（吸附，正好一个行距）→ `0`；往下滚到底停在 `630.72` 不再涨；横轴一格不动。
     - **B 全景里点 mini / 图标态窗口点不出焦点**：`src/overview.c` 的 `vidow_overview_click()` 用的是 `vidow_focus_view()`，而它开头就把 `MINI` / `ICON` 挡回去
       （不变量：`focused` 永远不指小窗）—— 于是点上去只有日志 `选中 <title>`、焦点一点没动（实测点 wm 盒心后 `focused` 仍是 wn1；退出 overview 后点同一位置就正常换焦点）。
       修法：换成 `vidow_focus_view_expand_mini()`（和 Alt+Tab 提交、IPC `focus-window` 同一个入口、同一个语义：先摊成普通窗口再给焦点）。
     - **C 屏上有 mini 时，命中盒和画出来的格子不一致（点右列命中左列）**：真因**不是** `src_box`（实测一直是整块缓冲 `0,0 665x740`），
       而是**内容节点的 `dst` 没跟着概览缩放走** —— `view_anim_place()` 里那一支要求 `view->base_dst_w/h > 0`，而那个 1:1 基线**xdg(Wayland) 从来没记过**
       （只在 X11 的 commit 和「退出小窗」两处写，恒为 0），守卫根本进不去 ⇒ 内容节点在概览里一直是 1:1。
       实测 3 列时最后一列的命中盒从 `646` 一路伸到 `1291`（矩形只到 `934`，≈ 整行宽 × 0.46），把邻列整片吞掉。
       修法：1:1 尺寸改走 `view_mini_native()`（唯一按 kind 分派的来源：非 X11 读表面当前逻辑尺寸、X11 读 `base_dst_*`）⇒ **X11 行为一个字没变**；
       内容节点的偏移（xdg 的 CSD 留白）也按 `ov` 缩（X11 的几何 x/y 是 0，这条对它天然是空操作）；MINI / ICON 不进这一支（那两态的内容几何由小窗机制管）。
       没改 `base_dst_*` 的记账规则：它还挂在 `mru.c`（卡片内容尺寸）和 `float.c`（小窗内容比例）上，让 xdg 开始记它等于顺带改那两处取值。
     - **判据**：`test-overview.sh` **66 → 100 PASS / 0 FAIL**（+31 条：mini / 图标态点选、每个格子的中心与边缘都命中它自己、空白处不命中、scale 1 / 2 各一遍、滚轮踏步与横轴）。

285. **Alt+Tab 切换器（MRU）：鼠标滚轮控制选中项 + 面板开着时输入完全模态**（用户：「MRU鼠标滚轮控制预览滚动，且打开MRU的时候阻止所有和MRU无关的交互」；2026-10-14）
     - **决策**：滚轮 = 选中项前后一格，**到边停住、不循环**（和 Alt+Tab 键本身的循环语义**故意不同**，日志写明）；模态从**按下 Alt 那一刻**算起（含 `mru_open_delay_ms` 那段还没显形的空窗期），只有 MRU 自己的键（`next-window` / `previous-window` / 方向键 / Esc / Return）和卡片点击能过。
     - **A 滚轮**：`vidow_pointer_axis` 里根本没有 MRU 分支 —— 按住 Alt 滚滚轮时先被**小窗档位状态机**抢走（光标压在迷你 / 图标小窗上时），再往下命中绑定表
       （默认配置 `Alt+wheel-up/down` = 换工作区）⇒ 实测「按着 Alt 想切窗口，**工作区真的跳了**」（`focus-column` 动作真的跑了、布局变了）。
       修法：`src/mru.c` 拆出 `mru_order_index()`（清单坐标系）/ `mru_step_index(dir, wrap)` / `vidow_mru_step()`（wrap=true，Tab 语义逐字不变）/ `vidow_mru_wheel()`（到边停住）/ `vidow_mru_step_clamp()`（方向键用，日志词不同）；
       `src/input.c` 的滚轮闸**排在小窗档位之前**。
     - **B 模态**：原来的闸只挡「面板已显形」（`vidow_mru_showing`），按下 Alt 之后、面板显形之前的那段空窗期里**按键 / 点击 / 滚轮全都照旧穿透**
       （实测：点击穿透进客户端 `0 → 2` 次、`Mod+滚轮` 真的换了列）。修法：闸改从 `vidow_mru_active()`（按下 Alt 即 true）算起 ——
       `input.c` 移动闸 / 按下闸（吃 press + `swallow_release`）、`keybind.c` 白名单（只有 `next-window` / `previous-window` 能过，其余打日志说明为什么被挡）、`gesture.c` 五处入口。
       闸的位置**必须在 `held_note()` / `pending_release()` 之后**：否则 Alt 的释放收不到，切换器会卡住。
     - **判据**：`test-mru.sh` 用**改前二进制**跑同一份脚本 = **88 PASS / 15 FAIL** → 改后 **103 PASS / 0 FAIL**（老脚本 60 条基线不变）；
       `test-mini.sh` 132/0、`test-overview.sh` 66/0、`test-defaultconfig.sh` 47/0 都不变。
     - **遗留**：小工具条读字节（`input.c`）与 `mini_grab_view` 块排在按下闸之前 —— 「迷你小窗已显形时、切换器开着点它的工具条」仍会漏（闸前移会拆散 grab 的按下 / 松开配对）；
       手势只有代码级挡（五处入口），`test-mru.sh` 的配置没声明手势，判不了。

284. **小窗模式三个真机 bug：自由小窗拖不动/一拖变原生、退出与关闭闪原生、工具条四个空格子且偏左**（用户：「自由小窗的时候没有拖拽的方式……一拖大小会变成原始大小且拖不动 / 关闭小窗的时候也会显示原始大小 / 工具条没有内容只有四个格子且位置偏左」；2026-10-14）
     - **A 自由小窗拖不动 + 一拖变回原始大小**（两个独立根因）
       - 拖不动：`src/float.c` 的 `vidow_float_grab_update()` MOVE 分支只写 `float_geom.x/y`，而**小窗的位置语义是比例锚点**（`mini_ax/mini_ay`）——
         下一次 arrange 走 `float_place_recursive()` 的小窗分支会把位置按锚点算回来，等于丢掉。实测：FREE 盒 `1668,203 504x315`，注入 `Mod+左键` 拖 +200,+140 → 盒逐字段不变。
         修法：MOVE 分支加小窗支路，把拖出来的位置回写锚点再 `vidow_mini_resolve_geom()`（和 `input.c` 的 `mini_grab_update()`、以及同函数 resize 分支回写 `mini_wf/hf` 同一规矩：**拖出来的东西必须回写锚点**，而且放在夹取之后）。改后盒 `1868,343` = 注入的 +200,+140。
       - 一拖变大：拖动会起果冻（jelly），而 `jelly.c` 的落点换算 `dst = buf / fx->scale` 里 `fx->scale` **已经含 contain 系数**
         （`view.c` 烘图传 `view_scale * fx_ov`），被除第二遍 → 内容放大 `1/ms` 倍。实测拖拽中内容屏幕脚印 `641x507`（≈ 原生 665x740）→ 修后 `272x304`（= 静止那一份）。
         真正通用的修法在 `jelly.c`（本次不动），改法：**缩放态不起果冻**（contain 系数 = 1.0 才起），注释里写清为什么。
     - **B 退出 / 关闭小窗那一瞬间显示原始大小**（两个根因，同一个「补间起点不能是推出来的量」）
       - `mini_transition_start()` 的起点缩放原来取 `sqrt(盒面积比)` —— 只在「内容正好填满盒子」时才等于屏上真正画的 contain 系数，也漏了内容在盒子里的居中偏移。
         实测自由小窗还原成列那趟：起点算出 `0.602`，真值 `0.4257` → **第一帧大 41%**；图标态 `0.097` vs `0.0865`。修法：`view.c` 导出 `vidow_view_mini_scale_export()` /
         `vidow_view_mini_content_offset()`（与 `view_anim_place` 的 `view_mini_scale`、`mini_cx/cy` 同源），起点取「上一帧内容缩放 ÷ 新状态 contain」+ 内容居中偏移。改后 `0.4570`（差 0.0001）、起点坐标 `1787,212`（+119,+9 = 内容原点）。
       - `view_snapshot_close()` 完全不管 `view->mini`：关闭一个 `504x315` 的自由小窗时按原生 `649x724` 铺开。修法：画布 / 节点尺寸乘 `ms`、位置挪到内容原点、显式写纹理映射。改后画布 `290x322`、第一帧脚印 `280x311`（静止 `272x304`）。
         （遗留：装饰留白 pad 仍按原生逻辑像素画，140ms 关闭动画里边框比静止略厚 —— 要修得动 `fx.c` 的 pad 语义。）
     - **C 工具条没有内容、只有四个格子、位置偏左**：`src/drag.c` 的 `mini_toolbar_layout()` 横向 `x = g.x` 左对齐（迷你盒 353 宽、条 151 宽 → 偏 **-101px**）；
       `vidow_mini_ui_init()` 只建了 4 个纯色 `wlr_scene_rect`，**一个字形节点都没有**（实测每按钮 `0/884` 非底色像素）。
       修法：横向改 `x = g.x + (g.width - w)/2`（保留「优先上方 → 放不下摆下方 → 夹进工作区」）；加 5x5 像素字模（放大=上箭头 / 收图标=小方块 / 还原成列=三竖条 / 关闭=叉），
       每个亮点一个 3x3 scene rect（15x15 居中放在 34x26 里），节点在 init 建好、show 时摆位、hide 时全关（数量上限 `VIDOW_MINI_TOOLBAR_GLYPH_MAX`，字模画满会当场报错而不是悄悄少画）。改后偏移 `0px`、字形像素 `81/81/90/81`。
     - **icon 态：「显示的是应用的图标」这条不是 bug**：全仓库没有任何 icon-theme / app-icon 查询（`grep app_icon|icon_theme|xdg_icon` 为空），
       画的就是**这个窗口自己最后一帧的缩略图**（`view.c` 的 `mini_snapshot_place()` 把 `view->last_buffer` 塞进快照节点）。64x64 实测：78% 是客户端自己的底色 `#F6F5F4` + 它自己的标题栏，
       58x64 按 contain 进 64x64、比例没拉伸 —— 用户看到的是「缩小后的窗口画面」，尺寸印象像图标。
     - **判据**：新增 8 条（工具条偏 -101 → 0 / 四个按钮空 → 有字形 / 拖拽中内容 `292x326→296x439` / 拖了没动 / `free→none` 起点缩放 `0.6240≠0.4571` / `icon→none` `0.0970≠0.0865` /
       关闭动画某帧外接框 `496 > 258×1.5`）；**用改前二进制跑同一份脚本 = 125 PASS / 7 FAIL**，逐条对应上面各根因。
       `test-mini.sh` **120 → 132 PASS / 0 FAIL**；`test-overview.sh` 66 PASS / 0 FAIL 不变（共享裁剪路径）。
     - **遗留**（都不在本次范围）：`jelly.c` 的双计数通用修法；关闭动画的装饰留白不随 `ms` 缩；icon 态点击命中区没走 `hit_scale`（test-mini.sh 里已文档化的既有问题）。

283. **还债：生成的配置里字符串型枚举漏了引号（`20-layout.lua` 打出 `center_focused_column = never`）**（用户「新增的配置应该给我本地的配置加上」时顺手撞出来的；2026-10-14）
     - **怎么撞上的**：把用户本地 `~/.config/vidow`（旧快照，缺 7 个分类文件）和仓库当前默认对齐、headless 起来验一遍 —— 日志里一行
       `[ERROR] … config.lua: …/20-layout.lua:9: center_focused_column: cannot use a nil as a config value`。
     - **根因**：`src/config.c` 那个「打默认值」的函数有一份**加引号白名单**（`K_STRING` / `K_COLOR` / `K_EASING` / `K_MRU_SCOPE` / `K_MRU_LAYOUT` /
       `K_TRISTATE` / `K_OPT_DOUBLE`），**漏了 `K_CENTER_FOCUSED`** —— 它也是字符串型枚举（`never` / `always` / `on-overflow`），于是打成裸词，
       Lua 眼里是 `nil`。影响所有 `--default-config` 用户：拷这份生成物进自己配置就吃一个 ERROR，而且**照文档把 `never` 改成 `always` 会静默不生效**
       （报错被吞成一行日志，值退回内置默认）。
     - **修法**：白名单补上 `K_CENTER_FOCUSED`（注释里写明「字符串型枚举也必须加引号，判据 = 生成物必须能被原样读回来」）；重生成后**只有** `20-layout.lua`
       这一处变化（`never` → `"never"`，逐个文件 `diff` 核过）。
     - **新判据**（`test-defaultconfig.sh` 第 7 节，3 条）：① `--default-config all` 的文本里不许有裸词值（正则扫 `^key = 裸词$`，白名单只留 `true/false/auto`）；
       ② 整份生成物**真当配置加载一遍**能起来；③ 加载日志里没有 `ERROR.*config` / `cannot use` / `不认识` / `未知`。
       `test-defaultconfig.sh` **44 PASS → 47 PASS / 0 FAIL**。
     - 顺带：和它同一批对齐的还有用户的本地配置本身（缺 `16-env` / `19-input` / `20-layout` / `21-keyboard` / `22-workspace` / `23-wincapture` / `24-mini`
       七份文件、`15-mru.lua` 还是老的表语法 `mru = { … }`、`02-decor.lua` 缺 `corner_n`、`09-output.lua` 缺 3 个键、`13-cursor.lua` 缺 6 个键、`14-keys.lua` 缺 11 个键位）——
       已按「只补缺失、不动你已有值」补上，整份备份在 `~/.config/vidow.bak-<时间戳>`。

282. **修一个会打死合成器的崩溃：过冲缓动曲线（`easing = "out_back …"`）下按 `Mod+O` 进 overview**（用户：「overview现在一打开就崩溃」；2026-10-14）
     - **现象**：用户真机上 `easing = "out_back 0.18"`（全局缓动曲线，过冲）时一进 overview 整个合成器就死，日志最后一行是
       `vidow: types/scene/wlr_scene.c:1133: wlr_scene_buffer_set_opacity: Assertion 'opacity >= 0 && opacity <= 1' failed.`
     - **复现**：把用户那整份 `~/.config/vidow`（含他自己改过的 `04-anim.lua`）拷进临时 `HOME`，headless 起 vidow + 三个 `tools/pattern.py` 窗口，
       `wtype -M logo -k o` —— 3.98s 处断言命中，进程死。**用仓库默认配置不崩**，所以是「配置里那个值」触发的（不是超椭圆那次改动：它只碰着色器与 `corner_n`）。
     - **根因**：`src/overview.c` 的 `overview_update()` 把动画引擎缓动后的 `t` 当**进度**直接算进 `ov->t`，而 `ov->t` 会被当**透明度**喂给场景
       （`row_apply` 里 `wlr_scene_buffer_set_opacity(row->bg, ov->t)`，`overview.c:327`）、乘遮罩 alpha（`:349`）、当各种 lerp 系数（`:746`）。
       函数注释里那句「动画引擎给的 t 永远是 0 → 1」只在**不过冲**的曲线下成立 —— `out_back` / `out_elastic` / `"bezier … y>1"` 都会让 `t` 冲过 1
       （实测 `out_back 0.18` 冲到 1.18），于是 `ov->t = 1.18` → wlroots 断言 → 合成器死。
     - **修法**：在 `overview_update()` 里把算出来的进度夹到 `0..1`（`!(v >= 0.0)` 那一支同时吃掉 NaN），并在注释里写清「`ov->t` 是进度不是位置，所以必须夹」。
       过冲段的效果 = 「提前到 1 然后停住」，也就是那条曲线在这件事上没有过冲 —— 本该如此（进度没有过冲的余地）。**只夹这一个变量**：
       全仓库其余 `set_opacity` 调用点都是 0/1 或已经 `clamp01`（`fx.c:62` / `fx.c:3361` / `catchmask.c` / `layer.c`，逐处核过）。
     - **回归判据**（进 `test-overview.sh` 末尾新一节，4 条）：`set-config easing "out_back 0.18"` → `wtype -M logo -k o` → ① 进程活着；
       ② `vidowmsg overview` 状态照常（`active=true scale=0.46 rows=1`）；③ 这段日志没有 assert/abort；④ Esc 退出来也还活着。
       `test-overview.sh` **62 PASS → 66 PASS / 0 FAIL**。

281. **还债：`24-mini.lua` 忘了重生成（`test-defaultconfig.sh` 3 条既有 FAIL）**（用户：无直接要求，是超椭圆那次改动顺手报出来的既有账；2026-10-14）
     - 三条 FAIL 同一根因：S5 把 `mini_fling_speed` 加进 `entries[]`（`src/config.c:611`，类别 23）时
       **忘了重新生成** `config.example/configs/24-mini.lua`（那份是 `--default-config 24-mini.lua` 的**生成物**）。
     - 修法：先 `--default-config 24-mini.lua` 出到临时文件 `diff`，确认**只有** `mini_fling_speed` 那一块是新增（无别的漂移），再覆盖回去。
     - 结果：`test-defaultconfig.sh` **42 PASS / 3 FAIL → 44 PASS / 0 FAIL**（rc=0）。与超椭圆改动无关（改前改后 FAIL 行逐字相同）。

280. **所有圆角统一成超椭圆（squircle n=4）：新增 `corner_n`（默认 4.0，2.0 = 圆弧退路）**（用户：「我还有个问题 现在的ssd圆角是否超椭圆」→「我需要所有圆角都是超椭圆」；2026-10-14）
     - 普查结论：**全仓库只有一处是圆弧** —— `src/fx.c` 的着色器函数 `sd_round_box()`（同一函数在片段着色器里管三件事：
       内容遮罩 `inside` / 边框环 `ring` / 阴影 `sa`），外加 `shaders/round.frag`（常驻 `shader_idle` 效果的独立着色器，同一条公式）。
       `src/mru.c`（MRU 卡片 `MRU_SQUIRCLE_N 4.0`）和 `tools/vidowportal.c`（选择器卡片 `SQUIRCLE_N 4.0`）**本来就是 n=4** ——
       所以「窗口角和卡片角本来就不一样」；老条目里那句「卡片圆角（超椭圆）」说的是 `mru_card_radius`，不是 `corner_radius`。
       所有窗口（Wayland / XWayland / 小窗 / 概览）都走同一条 fx 装饰管线，所以改那一处覆盖全部。
     - 改法：把 `length(max(q, 0.0))` 换成超椭圆范数 `pow(pow(c.x, n) + pow(c.y, n), 1.0 / n)`，n 走新 uniform `u_corner_n`，
       并乘 **`1/|∇d|` 归一化**（超椭圆不是严格 SDF：直边梯度 1、对角线 `2^(1/n) = 1.19`；不除的话角上 AA 斜坡只剩 1.68px、
       8px 边框沿对角线只剩 6.7px）。`c` 有 0 分量时该系数 ≡ 1，所以**直边与内侧与原来逐像素一致**；`n <= 2.001` 直接走 `length(c)`。
     - 四个入口（越界一律夹 2..8）：`config.lua` 的 `corner_n`（含 `decor = { corner_n = … }` 表项）/ 自动派生的 `VIDOW_CORNER_N` /
       IPC `vidowmsg config|set-config corner_n` / **窗口规则字段** `corner_n`（`rule.c` 字段表 + 范围检查 + `ipcstate.c` 的 `windows` JSON `look.corner_n`，优先级同 `corner_radius`）。
     - 实测（新脚本 `test-corner.sh`：`corner_radius = 32`、scale 1、纯蓝窗压纯黑底，覆盖率 = `B/255`）：
       - n=4：角区面积 **957.1**（期望 949.3 = `r²·Γ(1+1/4)²/Γ(1+1/2)`，+0.8%）；对角线第一颗 `α ≥ 0.5` = 第 **5** 颗（期望 5.09 = `0.159r`）。
       - n=2：面积 **815.4**（期望 804.2 = `πr²/4`）；对角线第 **9** 颗（期望 9.37 = `0.293r`）→ **形状是唯一变量**，没顺手改坏别的。
       - 边框环 `border_width = 8`：沿直边 8px（两档都是）；沿对角线内边界 15（圆弧）/ 11（超椭圆）= 各自平行曲线的解析值 ±0.3 步 → 环跟同一形状 + 归一化生效。
       - AA：直边部分覆盖 2 行、对角线 0~1 个对角步 = 过渡 1~2px，无硬边。规则 `corner_n = 8` → 面积 1005.6（更接近正方形上界 1024）。
     - 回归：`test-mini.sh` 120/0 → **120/0**；`test-overview.sh` 62/0 → **62/0**；`test-clip-stretch.sh` 10/0；`test-mru.sh` 60/0；
       `config.example/configs/02-decor.lua` 与 `14-keys.lua` 都是生成物，重生成后与原文件 **diff IDENTICAL**（没手改）。
     - 未做：阴影**没有**独立的像素判据（黑阴影压黑底量不出信号），只做了「和 ring 走同一处 `sd_round_box` 调用」的代码路径确认。

279. **录屏 portal 会话持久化：impl ScreenCast 接口 2 → 4（`persist_mode` / `restore_data`）—— 选过一次的屏 / 窗口，以后不再弹选择器**（用户：「仓库 /home/busyo/Dev/Vidow（C，wlroots 0.20 的 niri 风格滚动平铺合成器 vidow）。用户要求：**录屏 portal 需要持久化**」；2026-10-14）

     - **现象**：Chromium / OBS / Firefox 每次录屏都要重新弹选择器，`persist_mode=2` 的授权存不下来。根因在 `tools/vidowportal.c`：`SC_VERSION` 是 2（只有 `cursor_mode`），`SelectSources` 不读 `persist_mode` / `restore_data`、`Start` 也不回 —— 而前端（xdg-desktop-portal 1.20.4）**只在后端版本 ≥ 4 时**才把持久化那两项转下来，所以「授权」这一步压根没发生过。
     - **改了什么**（`tools/vidowportal.c`，接口版本只声明到 4 —— v5 的 `mapping_id`、v6 的 `pipewire-serial` 是流属性，我们不在 `Start` 的 streams 里给，报了就是给消费者制造期待）：
       - `SC_VERSION = 4`（`tools/vidowportal.c:157`）；`SelectSources` 解析 `persist_mode`(`u`) 和 `restore_data`(`(suv)`)；`Start` 的 results 回 `restore_data` + `persist_mode`（原样回，没做「降级」那套 UI）。
       - 持久化数据存在**前端**（权限库、按 app_id 存），后端只做两件事：生成一份**能自证**的、和「收到时认得出来就跳过选择器」。vendor `"vidow"`、私有版本 `1`、私有数据是**自描述字符串**（不用指针 / 时间戳这种没法复现的东西）：`kind|标识符|cursor_mode[;kind|标识符|cursor_mode]…`，`kind` = `monitor`（标识符 = 输出名）/ `window`（= ext-foreign-toplevel 的 identifier）/ `virtual`（= `WxH`）。区域那种当场框出来的源没有稳定标识符，不进 `restore_data`（全是区域的会话就不回，等于这次没持久化，日志里会说）。
       - **回退的两条路**：① 建流之前就发现源不在（窗口的 `ext_foreign_toplevel` `handle` 空了 / 输出名对不上 / 这次 types 没要这种源）；② 建了流但**被合成器拒掉**（`没有这个窗口（它可能已经关了）`）—— 后者才是权威信号（列表里可能还挂着已经关掉的窗口）。两条路都是「把半路建起来的流收掉 → 丢掉这份 restore_data → 弹选择器」，不是回失败（`req->restoring` 那个标记就是干这个的）。
       - **不可信输入**：`restore_data` 是前端原样转回来的 —— 格式非法（vendor / 版本 / `v` 里不是字符串 / 解析不出来）**忽略 + 照常弹选择器**；合法但源不在了（窗口关了 / 输出名对不上 / 这次 types 没要这种源）**回退弹选择器**（不是报错），半路建起来的流全收掉。光标模式按**存下来的那份**恢复，这次请求带的另一份不覆盖它（不一样会记一行）。
       - 建流那段抽成 `stream_add_output / window / region / virtual`：对话框点「共享」和 restore 恢复走**同一份**代码（`src_ kind` / `src_id` 记在 struct stream 上，Start 成功后按它拼新的 restore_data）。
       - 日志每步一行（测试直接拿它们当判据）：`restore_data：收到 …` / `判定合法（N 个源，光标 …）` / `判定非法（原因），忽略，照常弹选择器` / `恢复成 N 路流，跳过选择器` / `输出 … 不在了，回退弹选择器` / `Start：回 restore_data（persist_mode=… vendor=… 版本=… 数据=…）`。
     - **实测的前端行为（写下来免得下次又猜）**：前端是两层数据之间的**翻译层** —— 后端的 `(suv)` 被它换成自己的 **`restore_token`（字符串）** 发给客户端（前端日志 `Replacing restore data received from portal impl with a token`），客户端下次把 token 带回来时才翻译回 `restore_data` 喂给后端；客户端自己往 `SelectSources` 里塞 `restore_data` 是无效的（它会丢掉，那不是客户端的字段）。所以判据得这么写：判据 1 看**后端日志**里那份三元组，判据 2/3 走真链路（客户端带 `restore_token`），判据 4 的类型级垃圾只能**直接问后端**（前端按自己的协议先挡一遍）。
     - **测试**（`./test-portal-own.sh` 新增 6f 一节，27 项；客户端探针**写进脚本里**（`$R/persistprobe.py`，不新增 tools 文件），和 `tools/screencastprobe.py` 同一条路：同一套 handle_token + Response 订阅）：判据 1 `persist_mode=2` → results 里回 `(suv)`（vendor=vidow / 版本 1 / 数据 `monitor|HEADLESS-2|1`）、前端把它换成 token 发给客户端；判据 2 同一个 app_id 第二次带 token 请求 → **没弹选择器窗口**、日志 `判定合法 → 恢复成 1 路流，跳过选择器`、size/position 和第一轮逐字相同（2560,1440 @ 1280,0）；判据 3 录一个窗口 → 把那个窗口关掉 → 再带 token 请求 → **回退弹选择器**（实测走的是「建流被合成器拒掉」那条路，日志 `restore_data：合成器不认这些源（…），回退弹选择器`；回退之后照常出流、不崩）；判据 4 五种字符串级垃圾（错 vendor / 错版本 / 空数据 / 结构不对 / 光标模式不认识）各自给了原因并被忽略、五种**类型级**垃圾（`'junk'` / `(suu)` / `v` 里是 uint32 / 前两段类型不对 / 少一段）没把后端打崩、以及一条完整的「CreateSession → SelectSources（垃圾）→ Start = 照常弹选择器」。基线（改前）`./test-portal-own.sh` 64 PASS / 1 FAIL → 改后 91 PASS / 1 FAIL（那 1 条是**改前就有**的偶发：6b2 窗口流的光标位置判据 `pos=630,708` vs 流中心 `671,748`，差 41 超了 ±40 的容差；改前基线同一位置、同一个数，跟这次改动无关）。
     - **顺手加固**（`test-portal-own.sh` 的 `cleanup()`）：以前是 `pgrep -x vidow` **全杀** —— 在用户真跑着 vidow 的机器上跑这个测试会把人会话干掉。现在只收「自己的」vidow（临时 HOME 或 `WLR_BACKENDS=headless` 那两个判据），桌面上的那个一个都不碰。
     - **没做到 / 边界**：**输出被拔掉**这条路后端看不出来（`outputs` 列表只加不删，`registry_global_remove` 是空的），所以「拔了屏 → 回退」只在窗口那条路上验了（窗口没了有确切信号：`ext_foreign_toplevel` 的 `closed`）。输出列表的增删是另一件事（要小心处理行 / 缩略图里的悬垂指针），这次没动。

278. **小窗 HiDPI 修一条真 bug：运行时把输出调到 scale 2 之后重进小窗，盒里的客户端内容只有该有的一半大**（用户：「仓库 /home/busyo/Dev/Vidow …… 先定性，再决定改不改」；2026-10-14）

     - **现象**：S7 那节里 scale 2 屏上迷你盒（逻辑 174x95，物理 348x190）里客户端内容的物理脚印只有 **86x95**（该 172x190 = contain 几何的 2 倍），两侧各 131 物理像素露壁纸。对照：**启动配置里就写 `scale = 2`** 时同一位置是满的（168x190）。
     - **定性（先把「是不是小窗特有」问清楚）**：单屏 headless，同一份 harness / 同一个 GTK 探针，量「盒内内容的外接框（物理 px）」：
       - 普通**平铺**窗口：scale 1 时 636x696（盒 640x720 逻辑）→ 运行时改成 scale 2 后 632x683 物理（盒变 320x360 逻辑）= **2 倍物理像素，正确**；
       - 普通**浮动**窗口：同上 632x683 物理，**正确**；
       - 小窗：**进小窗时输出已经是 2** → 42x46 物理（该 168x190），**只有小窗错**；而「先进小窗、再改 scale」那条反而是对的（168x190）—— 问题不在「运行时改 scale 这条公共路」，在小窗自己那份缓存。
       - 客户端收到的 configure 序列三条路一样（先新逻辑尺寸、再小窗自己的原生尺寸），不是 configure 发错。
     - **根因（两处，都在 `src/view.c`）**：
       1. `view_dst_capture()`（688 行那一族）：节点 dst 是 0 时兜底取 `src->buffer->width/height` —— 那是**缓冲像素**，而这个字段（`base_dst_*`）的语义是**场景单位**里的 1:1 尺寸；输出缩放 2 的客户端缓冲是逻辑的两倍，基线记大了一倍。取证（临时 `DBG` 行，取证后删净）：`node_dst=0,0 buf=1360,1520 surface=680,760 scale=2 -> base 1360,1520`。
       2. `view_mini_native()`：优先读 `base_dst_*`，而 xdg 那条路只在**退出小窗那一刻**记一次（`view_commit` 里只有 X11 分支调 `view_dst_capture`）→「退出小窗 → 客户端还没按新逻辑尺寸交帧 → 记下旧尺寸 → 永久过期」；重进小窗时 contain 系数按过期原生尺寸算 → 内容一半大。另注：`wlr_scene_buffer_set_dest_size(sb, 0, 0)` 在 wlroots 里是「按缓冲像素当场景单位」，正好就是退出那一刻被记下的值 —— 两条凑一起才是「逻辑当物理画」的味道。
     - **修（只动 `src/view.c` 三处，不加特例）**：① `view_dst_capture()` 兜底改成表面自己的逻辑尺寸（拿不到表面才退回缓冲像素）；② `view_mini_native()` 对**非 X11** 直接读表面当前逻辑尺寸（小窗 / 图标态不下发 configure，客户端那一份一直是当前值），`base_dst_*` 只留给 X11 那条「场景尺寸 ≠ 表面尺寸」的路；③ 退出小窗还原 dst 时设成表面逻辑尺寸而不是 0（0 = 按缓冲像素画，2 倍缓冲的客户端会先被画大一倍，且它下一次 commit 之前一直错）。悬停预览盒那一份 contain 也改走同一个 `view_mini_native()`（同一条算法，免得再留一份过期的）。
     - **回归**：`./test-mini.sh` 120 PASS / 0 FAIL（S7 那条判据从放宽的「盒里有内容 > 5%」**恢复严格值**：非壁纸像素外接框 = contain 几何的 2 倍物理像素 ±12%；修前 86x95 物理、修后 172x190 物理）；`test-overview.sh` 全过（共享裁剪路径，改前改后各一次）。
     - **本来就没问题的**：小窗盒尺寸按逻辑工作区算、逻辑坐标、跨缩放边界裁剪、指针坐标换算 —— S7 里那几条判据本来就对。

277. **小窗模式 S6：验证脚本收进仓库（`test-mini.sh`）+ 小窗模式文档定稿 —— 下面这一段就是小窗的参考手册**（用户：「做方案 §7 的 S6（测试 + 文档），只做测试与文档、不改功能代码」；方案 `/tmp/vidow-miniwin-plan.md` §7 S6；2026-10-14）

     - **测试进仓库**：散在 `/tmp` 的 7 个临时脚本（anim / fling / preview / frames-run / ghost / prevdeath / flip）并成 `./test-mini.sh`（headless 双屏、独立临时 `HOME` / `XDG_RUNTIME_DIR`、`PASS/FAIL` 计数、最后按 `FAILED` 给退出码；收尾 `kill -TERM -- "-$PGID"` 自己的进程组，`pgrep -a vidow` 干净）。**顺手修掉两个「让脚本假通过」的坑**：
       - `grim` 的几何参数是 **`"x,y WxH"`**，原来写成 niri 那套 `"WxH+X+Y"` → 截出 0 字节的文件，而「两张空图逐像素相同」永远成立（静止那条判据假通过）；
       - `vidowmsg` 的子命令是 **`set-mini-state --id N --state icon`**，原来写的 `set_mini_state --state` 是错的（IPC 报「不认识」），形态压根没变、后面的断言照样过。
     - **客户端探针**（脚本里现场生成 `$RUNDIR/probe.py`，不用新增 tools 文件）：纯红底 + **每帧都在动**的白条（纯色窗口两帧永远逐像素相同 —— 「在不在出帧」那条会假通过）+ 每个 configure / motion / scroll / key 事件打一行。输入路由（迷你 / 图标态客户端一个事件都收不到、FREE 态收得到）和指针坐标换算（客户端收到的坐标 = 原生对应点 ±3px）都靠它。
     - **覆盖**（脚本里一层一个 mark）：形态 IPC / 三态循环 / `mini-windows` / 还原成列；跟屏（切工作区 geometry 逐字段不变）；焦点守卫（`focused` 不指向小窗；`focus_follows_mouse = true` 时指针压上去也不抢；`focus-window` 对迷你态是「先摊成自由小窗再聚焦」）；输入路由 + 4px 单击 / 拖拽阈值；图标贴边裁剪（相邻屏那条「本该溢出」的条带非背景像素 = 0）；原生 buffer（`surface` 尺寸不变 + 新 configure 的尺寸仍是原生；规则 `mini_preserve_buffer = false` 反向验一次：真的被重配小了）；`mini_aspect` 三值（全局 free / lock / auto + 运行时 `toggle-mini-aspect` 压过全局，判据是**改宽拖动时高跟不跟**——进小窗那一刻盒子照旧按 `mini_free_w/h` 摆，锁比例管的是拖拽那条路）；落点区（区内松手 → mini、区外松手不误触、区外落点区不亮）；内边缘 → 收图标 / 跨屏边 → 换屏保持形态；滚轮降档（一次一格、到底停住、上滚升档、FREE 态上滚转发给客户端）；悬停工具条（悬停露出 → 点「还原成列」按钮）；`force_render` 分叉（关 = 客户端 0 新帧 + 图标与预览两帧逐像素相同；开 = 客户端恢复出帧 + 预览两份截图不同）；过渡动画（相邻位移 ≤ 屏宽 10%、全程单调、第一帧在起点、结束偏移归零）；惯性甩出（速度阈值、方向取最近边、不过冲、时长 < 普通转换、<4px 快甩的逃生口）；MRU（Alt+Tab）提交选中图标态小窗 → 展开成自由小窗并拿到焦点。；**新增 S7（HiDPI scale 2）一节**：同一套 harness / 同一个探针（不另起一次运行），运行时用 `vidowmsg output-scale HEADLESS-2 2` + `output-position` 换成「scale 2 屏占逻辑 1280..1920、scale 1 的邻屏贴着它右边缘」那张布局（布局坐标只能这么写死：`outputs` 的 JSON 里没有布局坐标字段），验四条 —— 输出的物理 1280x720 → 逻辑 640x360；小窗几何按**逻辑**工作区（620x340）的 28% 算（盒 174x95、落在 1280..1920 × 0..360 内）、`grim -g`（逻辑坐标、不乘 scale）截回来正好 348x190 = 2 倍物理像素；图标贴右边缘溢出 16 逻辑 px 到邻屏，判据是**同一条带的像素差分**（邻屏没有壁纸 —— `wall.py` 只盖它所在那块屏，拿「非背景像素」判漏画会 100% 假阳性）图标态 vs 正常态差 **0/3200**；FREE 态往盒心注入指针 → 客户端收到 172.5/190.9（期望 surface 中心 172/190，±3px）。另记一条线索（**没**写进断言）：scale 2 屏上迷你盒里客户端内容的实测 footprint只有 contain 几何的一半（86x95 物理像素，该是 172x190；非壁纸像素 7446/66120 = 11.3%），所以「盒里有内容」那条门槛放宽到 5%，不拿它断言内容缩放比（免得贴阈值偶发 FAIL）。
     - **没进脚本的**（时序太敏感，宁可留注释也不写会偶发 FAIL 的断言）：X11 `PAspect` 那两条（「全局 free + X11 PAspect 不再自动锁」「改回 auto 自动锁」需要一个能设 aspect hints 的 X11 客户端，本机手工验过、见 274）、触摸板滑移手势。文件头有明确注释。
     - **小窗模式参考手册**（用户向的完整说明，S1–S5 的细节见 271–276）：

       **三态与交互**。小窗是浮动窗口上的一个**形态**：`none`（不在小窗态）/ `free`（自由小窗）/ `mini`（迷你）/ `icon`（贴边图标）。交互全在这几条：
       - **拖窗口进落点区**（每块屏工作区右上角 `mini_drop_zone_w × mini_drop_zone_h`，默认 15%×20%；**只在光标底下那块屏亮**）松手 = 迷你小窗；松手在区外照旧走原来的平铺 / 浮窗落点（不误触）；
       - **单击** = 放大一档（图标 → 迷你 → 自由小窗，自由小窗再单击给焦点）；**拖拽** = 移动（判据：从按下点起的累计位移 > `mini_drag_threshold_px`，单向不回锁）；
       - **内边缘**（那条边外侧没有相邻输出）拖动松手 → 收成图标；**跨屏边** → 换屏并保持形态。**这两条只对迷你 / 图标态**：自由小窗是自由摆放的正常窗户，拖到哪就摆在哪 —— 拖到内边缘松手**不**收图标（收图标另有入口：落点区 / 工具条 / 滚轮 / 甩出）；
       - **自由小窗的拖动**与普通浮窗同权：跟光标走、能跨屏，**光标一跨过屏边界就当场改归属**（不等到松手），锚点按光标在新屏工作区里的相对位置算（从右边进新屏 → 贴新屏**左**边缘，不会弹到远边）；可达范围就是整块工作区（四个方向都顶得到边，不吃 `mini_edge_margin` 那 8px）；松手落在光标处；
       - **自由小窗的改大小**（`Mod+右键`）**真的**把客户端 configure 成那个大小（客户端跟着重新排版，和普通浮窗的 `interactive-resize` 一样），不是「只改留白」；抓取命中区就是盒子本身（按盒子边缘起手就抓得到）；
       - **滚轮** = 形态档位（下滚降一档、上滚升一档，到底 / 到顶停住；FREE 态的上滚照旧转发给客户端滚内容）；
       - **悬停工具条**（迷你 / 图标态悬停露出：放大 / 收成图标 / 还原成列 / 关闭）；**悬停图标** `mini_icon_hover_ms` 后出实时预览（朝屏内展开，只读但吞点击）；
       - **惯性甩出**：快速甩向边缘（松手速度 ≥ `mini_fling_speed`）→ 那条边外侧有相邻输出就换屏保持形态，没有就收成图标；时长由速度推（越快越短，夹在 110ms..`mini_transition_ms`），位置曲线不过冲；
       - **键位**：`Mod+Shift+W` = `cycle-mini-state`（三态循环）、`Mod+Shift+Up` = `mini-expand`、`Mod+Shift+Down` = `mini-collapse`、`Mod+Shift+Return` = `unmini-window`（还原成列）；另有 `toggle-mini-aspect`（把当前自由小窗的宽高比锁 / 不锁翻过来，**只作用于运行时、不写回配置**）。

       **配置键**（`configs/24-mini.lua` 那一族，默认全开；尺寸都是「相对工作区的百分比」）：

       | 键 | 默认 | 说明 |
       | --- | --- | --- |
       | `mini_enabled` | `true` | 小窗模式总开关（false = 拖到落点区什么都不发生，零行为变化） |
       | `mini_drop_zone` | `true` | 拖窗口时亮落点区 |
       | `mini_drop_zone_w` / `mini_drop_zone_h` | `"15%"` / `"20%"` | 落点区宽 / 高（占工作区） |
       | `mini_w` / `mini_h` | `"28%"` / `"28%"` | 迷你态尺寸（钉死跟配置，客户端改不动） |
       | `mini_free_w` / `mini_free_h` | `"40%"` / `"45%"` | 自由小窗初始尺寸（之后记在窗口自己身上） |
       | `mini_icon_size` | `64` | 图标边长（逻辑像素，正方形） |
       | `mini_icon_overflow` | `0.35` | 图标沿贴的那条边出屏多少（占自己比例，0..0.6） |
       | `mini_icon_hover_ms` | `250` | 悬停多久出预览 |
       | `mini_icon_hover_leave_ms` | `300` | 离开图标 / 预览多久收起来 |
       | `mini_icon_hover_grace_ms` | `200` | 「从图标移向预览」的宽限期（跨空隙不闪） |
       | `mini_icon_preview` | `true` | 悬停预览开关 |
       | `mini_toolbar` | `true` | 悬停工具条开关 |
       | `mini_edge_snap_px` | `24` | 拖到屏边多少像素内算「贴边」 |
       | `mini_fling_speed` | `800` | 甩出阈值（px/s）：慢拖通常 < 300、随手一甩 > 1500，800 取中间偏保守 |
       | `mini_transition_ms` | `260` | 形态转换时长（0 = 不做动画） |
       | `mini_hover_fade_in_ms` / `mini_hover_fade_out_ms` | `120` / `150` | 预览淡入 / 淡出 |
       | `mini_remember_size` | `true` | 记住自由小窗的尺寸（false = 每次都用 `mini_free_w/h`） |
       | `mini_edge_margin` | `8` | **迷你**小窗离工作区边缘的最小留白（自由小窗不吃它 —— 它是自由摆放的正常窗户） |
       | `mini_preserve_buffer` | `true` | **迷你 / 图标态**不重配客户端（保留原生 buffer，合成器缩放显示）；false = 按小窗尺寸重配（省显存，但有的游戏一 resize 就重置成窗口模式）。自由小窗**不受它影响**：总按盒子尺寸 configure 客户端 |
       | `mini_aspect` | `"free"` | 自由小窗宽高比策略：`"free"`（默认，不锁）/ `"auto"`（按自动信号：X11 `PAspect` / `min==max`）/ `"lock"`（强制按内容比例锁）；三个取值都是强制语义，优先级 键位 > 窗口规则 > 这个全局键 |
       | `mini_drag_threshold_px` | `4` | 「单击 = 放大」vs「拖拽 = 移动」的累计位移阈值 |

       **IPC**（`vidowmsg`）：`mini-window [--state free|mini|icon]`（默认迷你）、`set-mini-state --id N --state free|mini|icon|none`（显式设形态，含还原）、`unmini-window --id N`（还原成列）、`cycle-mini-state --id N`（三态循环）、`mini-windows`（当前小窗清单）。指认目标照通用那套：`--id N` / `--app-id` / `--title 子串` / `--output` / `--workspace`。

       **窗口规则**（4 个字段，`vidow.rule{ … }`）：`mini_window = true`（开出来就是迷你小窗）、`mini_state = "free"|"mini"|"icon"`（更细的初始形态）、`mini_aspect = "auto"|"lock"|"free"`（这个窗口的策略，压过全局键）、`mini_preserve_buffer = true|false`（这个窗口保不保留原生分辨率）。规则是形态的唯一权威，键位（运行时）压过规则、规则压过全局键。

       **已知限制**：① 悬停预览在窗口换屏 / 跨屏移动时不跟随（预览是图标那一份镜子的临时节点，换屏那一帧先收掉再重开）；② 落点区只在光标底下那块屏亮（其它屏不预告，盲拖到角落走的是原来的平铺落点）；③ 图标态出帧按 `force_render` 分叉（关 = 停成快照那一帧、开 = 活镜面一直出帧）；④ 全屏游戏进**迷你 / 图标态**不重配客户端（`mini_preserve_buffer = true` 那条：合成器缩放 + 指针坐标换算，客户端完全不知道自己在小窗里；代价是它照旧按原生分辨率出帧）；自由小窗不保留 buffer（按盒子尺寸 configure，客户端知道自己的大小）。
       **`test-mini.sh` 抓到的两个真 bug（都已修，判据留在脚本里当回归）**：
       - **图标漏画到相邻屏**（贴右边缘溢出的那 16×64 条带：修前 832/1024 非背景像素、其中 172 个是客户端纯红；修后 **0/1024**，图标在屏内那部分照旧 3072/3072）。根因两处，都是「缩放过的节点不能借 wlroots 那套 1:1 语义的裁剪」：① `src/view.c` 新增 `view_scaled_crop()` —— 缩放态（图标 / 迷你 / 保留原生 buffer 的自由小窗、也含过渡动画那几帧）不再走 `wlr_scene_subsurface_tree_set_clip()`（它内部把 clip 当节点位置用、src_box 只按表面尺寸比例缩、dst 一个像素不动：缩放态下内容会被挪位 + 拉伸，越界那半块照样画），改成「源框（缓冲像素）取可见那一块、目标框（场景尺寸）也取可见那一块」，缩放比不变；② `src/view.c` 的 `mini_snapshot_place()` —— 图标快照那份节点挂在**全局**的 `layer_toplevels` 上（不在窗口自己的场景树里），以前一个裁剪都没有，现在按**自己那块屏的框**把框外那部分裁掉（判据不能用 `view_clip_box()`：它对停到停车位的窗口按设计就是「不裁」）。顺带更正 273 的读数：那条「相邻屏非背景像素 = 0」是踩了 `grim` 几何参数写错那个坑（截出 0 字节文件，空图比空图当然一样）。
       - **工具条点过一次按钮之后，迷你窗口上的下一对按下 / 松开被吞**（`src/input.c` 的 `vidow_pointer_button()`）：松手那一支以前写在 `if (server->mini_toolbar_view != NULL)` 里面，而按下那条路紧接着 `vidow_mini_toolbar_hide()` 就把 `mini_toolbar_view` 清成 NULL —— 标志永远留在 true。症状：悬停着工具条的迷你窗口上**按一下没反应**（形态不变，`mini_grab` 也没人收尾），紧接着一次**不带按键**的 `pointer-motion` 就把窗口拖走（修前现场 `place w1 -> 464,506`）。修法：松手那一半**无条件**清标志，并且标志被吞过、grab 还挂着时接着往下走，让小窗那条松手把它一起收掉。回归判据：S4「单击迷你 → 放大（工具条点过按钮之后也一样）」+「不带按键的 pointer-motion 不拖窗口」。

276. **小窗模式 S5：形态转换过渡动画（起点取「上一帧画在哪」）+ 图标悬停实时预览 + 惯性甩出 —— 小窗开始「像安卓那样」动了**（用户：「做方案 §7 的 S5（过渡动画 + 悬停预览 + 图标出帧）」+ 追加「要像安卓那样惯性甩出」；方案 `/tmp/vidow-miniwin-plan.md` §1.4 / §7 S5 / §8 判据 16–19；2026-10-14）

     - **形态转换的过渡动画**（`src/float.c` 的 `mini_transition_start` / `mini_anim_apply` / `mini_anim_update` / `mini_anim_done`，挂在 `vidow_mini_set_state()` 的最后一步）。两条硬规矩写在代码顶上：
       ① **起点严格取「上一帧画在哪」**：`geom + anim_off`（和 `vidow_place_view()` 里算 `drawn_x/drawn_y` 的那两行**同源**），在**改几何之前**取一次、存成「起点 − 目标」的常量差值；`update()` 里**绝不重读 `view->geom`**（转换中途夹着 `arrange`，geom 会被改写）。起点**不做任何推断** —— 哪怕它在屏外也照用，这正是 `daeba31` 的教训（那次按「起点落在输出框里就推到框的一侧」，正常滚动时也命中 → 每滚一列窗口都从屏幕外飞入）。
       ② **位置曲线一律单调**（`out_cubic`），`out_back` **只用在缩放**上 —— 「中间帧位置单调」因此是构造性保证，测试不用给过冲开例外。缩放走现成的 `view->anim_scale`（`fx.c` 按窗口中心缩，静止恒为 1），起点 = 新旧盒的面积比开方；展开（变小 → 变大）给一点回弹（幅度取全局 `back_overshoot` 反解出来的常数），收拢用 `out_cubic` 收住。
       一条 anim 里要用两条曲线，所以 `src/anim.c` 加了个 `vidow_ease_value()` 出口（别为第二条曲线再起一条 anim）。`vidow_place_view()` 里加了一条守卫：`mini_anim` 跑着时**不起位移补间、也不清 `anim_off`**（清零 = 中间帧瞬移到终点）。`mini_transition_ms = 0` 时照旧瞬时切换。
     - **图标悬停预览**（`src/view.c` 的 `vidow_mini_preview_motion/hide/hit` + `preview_show` / `preview_box_for` / `preview_fade_*`）：悬停图标 `mini_icon_hover_ms`（250ms）之后，在**朝屏内**那一侧展开一块迷你尺寸 ×0.8 的**实时预览**（贴右边 → 预览在它左边，贴左 → 右边，贴上 → 下方，贴下 → 上方；间距 `mini_edge_margin`，整体夹进工作区）。渲染是**同一张表面的第二份镜面**（`wlr_scene_subsurface_tree_create`，先例 `src/wincapture.c:676`），挂 `layer_top` 里并在露出时 `raise_to_top`（它在窗口之上、会话锁 / overview 会整棵关掉它、**不在模糊底衬抓取的遍历范围里** —— 挂 `layer_toplevels` 会被当成一个窗口抓进底衬、留下鬼影） —— 不另造抓帧 / 上屏管线，客户端出一帧预览跟着动。
       **只读**（不抢 pointer / 键盘焦点、不进 MRU、不参与 `vidow_view_at` 命中 —— 预览节点没有 view tag，命中那条路认不出它）但**吞点击**（按下 / 松手成对吞，日志「吞掉点击」）：看到指针底下有块「像窗口的东西」、点下去却激活**背后**的窗口是明显的错。时间轴用 `vidow_anim_start` 当定时器（不新开 timer，它自己 kick watchdog 保证帧照出）：延迟 250ms → 出；既不在图标也不在预览 → `mini_icon_hover_leave_ms`（300ms）→ 收；**指针进到预览矩形里就把计时清掉**（这就是宽限期：从图标移到预览要跨那 8px 空隙）。淡入 / 淡出 120 / 150ms（`out_cubic` 不行 —— 透明度用线性进度）。
     - **惯性甩出**（`src/float.c` 的 `vidow_mini_fling` / `vidow_mini_fling_velocity` / `vidow_mini_fling_sample` + `src/input.c` 的松手挂点）：把迷你小窗**快速甩向边缘**就飞过去收成图标，不用慢慢拖到底。三条设计决定：① 速度按**最近 ~100ms 的采样**算（8 格采样环，`vidow_pointer_motion` 每次推一格），**不是单帧增量**（高刷新率下小得量不出意图、低刷新率下抖得离谱），至少 3 个点、且最后一点离松手 ≤80ms（停住再松手不算甩）；② 方向取**最近的那条边**（±45° 容差：横向速度占优取左 / 右），落点判据沿用已定的那条 —— **那条边外侧有相邻输出 → 换屏保持形态；没有 → 收成图标贴那条边**（沿边那一轴按窗口现在的位置记锚点）；③ **不做真物理**：时长由速度推（`180000/speed`，夹在 110ms..`mini_transition_ms`），位置曲线照旧 `out_cubic`（单调、**不过冲**）—— 边缘是吸附目标不是自由物体的落点。
       ⚠️ **4px 单击 / 拖拽阈值的逃生口**：一次很快的小幅甩动可能位移不到 `mini_drag_threshold_px`，不处理就会被判成「单击 = 放大」，跟甩出直接冲突 —— 所以**松手速度超阈值 → 即使位移 < 4px 也算拖拽 / 甩出**（速度优先于位移）。配置键 `mini_fling_speed`（逻辑像素 / 秒，默认 **800**：有意定位的慢拖通常 < 300 px/s、随手一甩 > 1500 px/s，取中间偏保守的一侧 —— 甩错了会收成图标，代价比「甩了没反应」大；0 = 关掉）。
     - **实测**（headless 1280x720 @1x，`HOME`/`XDG_RUNTIME_DIR` 指临时目录；注入走 `VIDOW_TEST_ACTION` 的 `pointer-motion` / `pointer-button`，和真鼠标同一条路；位置采样取 `mini anim:` 那行调试日志里的「画在 x,y」= `geom + anim_off`）：
       - 过渡动画（`mini_transition_ms = 600`，每帧采样）：`none → mini` 起点 `10,10` → 目标 `909,18`，**40 帧相邻位移最大 71px**（屏宽 10% = 128px）、全程单调、**第一帧就画在起点 `10,10`**（差 0,0）；`mini → icon` 起点 `909,18` → `1206,10`（最大 23px）；`icon → free` 起点 `1206,10` → `758,18`（最大 35px）；`free → none` 起点 `758,18` → `10,10`（最大 59px）。收尾偏移归零、缩放回 1。
       - 悬停预览：悬停图标 100ms 时**还没有**；260ms 后日志「预览在 `938,10 282x157`（图标 `1228,10 64x64`，贴 **右** 边 → 朝屏内展开）」—— 预览整个落在图标**左边**（屏内那一侧）；悬停期间 `focused` 不变、MRU 条目数 = 窗口数（预览没进去）；点预览 → 日志「吞掉点击」、背后窗口 `focused` 不变、形态仍是 `icon`；从图标跨 8px 空隙移进预览**不闪**（宽限期生效）；移开后 300ms 日志「收起来」。
       - 甩出：向左快甩（8818 px/s）→ `mini → icon`，日志「甩向 左 边（内边缘）→ 收成图标，110ms」，14 帧采样里**最小 x = 目标 x**（一点没越过，不过冲）且单调向左；向右快甩（8819 px/s，外侧是另一块屏）→ `output` 换了、形态仍是 `mini`；把阈值临时降到 30 px/s 后，位移只有 3px（< 4px 阈值）的甩动也走「甩出 → 收成图标」而不是「单击 → 放大」（逃生口生效）。
     - **改动面**：`src/server.h`（`mini_anim` / `mini_from_dx,dy` / `mini_scale_from` / `mini_scale_ease` + 甩出采样环与时长覆盖 + 预览的一整套状态与声明）、`src/float.c`（过渡动画 + 甩出三条 + `mini_transition_start` 接在 `vidow_mini_set_state` 两处出口 + `vidow_float_end_grab` 的甩出挂点）、`src/view.c`（`vidow_place_view` 的 mini_anim 守卫 + 悬停预览整节 + 销毁 / 解映射时收预览与取消补间）、`src/input.c`（指针采样、松手甩出与 4px 逃生口、预览状态机挂点与吞点击）、`src/anim.c`（`vidow_ease_value()`）、`src/config.c`（**只加了 `mini_fling_speed` 一个键**：`entries[]` + 默认值 —— 新需求点名要配置键，原先「别碰 config.c」的口径在这里让位）。`workspace.c` / `overview.c` / `im.c` / `gesture.c` / `main.c` / `wincapture.c` 一行没改，**没有全屏特例**。
     - **图标态的出帧策略按「有效 `force_render`」分叉**（S5 的第三项，`src/view.c` 的 `vidow_mini_uses_snapshot()` / `mini_snapshot_place()` / `vidow_mini_frames_sync()` / `vidow_mini_frames_recheck()` + 预览那份快照 `server->mini_preview_snap` / 字段 `view->mini_snapshot`）：`force_render` 开 → 图标就是窗口自己那块活节点（`render_pump` 照旧保活）+ 预览是表面的第二份镜面，两边都是**活的**；关 → 真实节点停到停车位（客户端**停帧**）、屏上由快照节点画 `view->last_buffer` 的**静止最后一帧**。图标和预览**共用一条判据**（不是各判一份）—— 所以不会出现「图标静止、预览还在动」。翻面的挂点三条：`vidow_mini_set_state()`（形态定下来）、`view_commit`（换最后一帧）、指针一动 / 每帧的 `render_pump`（IPC / 规则刚改完就接上，幂等）。退化情形 `last_buffer == NULL`（客户端一帧都没交过）→ 退回活节点并在进图标态那次打 INFO（`src/float.c`）。
     - **悬停宽限期 `mini_icon_hover_grace_ms`（200ms）现在真生效**（`src/view.c` 的 `preview_in_corridor()` + `vidow_mini_preview_motion()`）：指针在「图标 → 预览」那条空隙走廊里（朝预览走）用 200ms 且**每动一下重起**；移向别处才用离开的 `mini_icon_hover_leave_ms`（300ms，已经起着就不重起）。
     - **实测**（headless 1280x720 @1x，`HOME`/`XDG_RUNTIME_DIR` 指临时目录；`grim` 逐像素比两帧、间隔 300–350ms；客户端出帧数取 `tools/heartbeat.py` 的 `frame` 行）：
       - `force_render` 关：进图标态后 **0.8s 内客户端 0 新帧**（进图标态前累计 103 帧）、图标两次截图**逐像素相同**；悬停预览期间仍是 **0 新帧**且预览两次截图**逐像素相同**（图标静止、预览也静止 —— 两条路一致，没有「图标静止、预览还在动」）。
       - `force_render` 开：同一条测试里 0.8s **50 新帧**、图标两帧差 **32.1%** 像素、预览两帧差 **43.4%**（两边都是活的）；日志「快照（force_render 关，静止的最后一帧）」/「活镜面（force_render 开，动的）」跟着一起翻面。
       - 宽限期：指针进空隙走廊后 150ms 内移进预览**没被收走**；移向别处 220ms 还没收、再等 250ms（共 ~470ms）收起来（走的是离开那 300ms）。
     - **顺手修的一条**：图标态快照节点挂在 `layer_toplevels` 上、**不跟着 `view->scene_tree` 走**，原先窗口 unmap / 销毁时没人收它 —— 实测「图标态关掉客户端前后 **0.0%** 像素变化」（窗口数已经是 0，屏上那块还在）= 一块冻着最后一帧的**鬼影**。`view_unmap` / `view_destroy` / `x11_destroy_view` 三处加 `mini_snapshot_free()`，快照判据补 `view->mapped`；预览那份快照淡出完也把客户端缓冲松开。修完同一个测试前后差异 **86.1%**（背景回来了），force_render 开那条路照旧 90.8%。
     - **没做**：预览换屏 / 跨屏时不做跟随。

275. **小窗模式 S4：拖拽落点区（拖到右上角松手 = 迷你小窗）+ 内边缘收图标 / 跨屏边换屏 + 悬停工具条 + 滚轮档位 + 键位动作 —— 小窗终于有鼠标入口了**（用户：「做方案 §7 的 S4（落点区 + 形态入口 + 工具条 + 滚轮收图标 + 键位动作）」；方案 `/tmp/vidow-miniwin-plan.md` §2 / §7 S4；2026-10-14）

     - **落点区**（`src/drag.c` 的 `vidow_mini_drop_zone_box/update/hide/hit`）：每块屏工作区**右上角** `mini_drop_zone_w × mini_drop_zone_h`（默认 15%×20%），两块半透明矩形 —— 底板 = 落点区本身，preview = **松手会得到的那个迷你小窗**（`mini_w × mini_h` 贴右上角，裁进落点区里，所以它比落点区大时不会糊出去）。**只在拖拽中出现**（用户点名的那条）：光标不在落点区里时两块矩形都是关着的，所以「盲拖到角落松手」走的是现有的平铺落点，不会误触。
       两个预览块**互斥**：落点区亮着时 `vidow_insert_hint_update()` 第一句就把插入提示块收掉（同时画两个，用户不知道松手会得到哪个）。三条拖拽路（平铺 / 浮窗 / 形态拖拽）都走同一份判据，松手落在里面 = `vidow_mini_set_state(view, VIDOW_MINI_MINI)`。
     - **内边缘 vs 跨屏边**（`vidow_mini_output_across()` / `vidow_mini_edge_under_cursor()` / `vidow_mini_drag_finish()`，`src/float.c`）：判据写死成**「这条边的外侧有没有相邻输出」**。探针沿边的法线取 1px（`wlr_output_layout_output_at`），并要求与目标屏在**那条边的方向上非零区间重叠** —— 对角相接的两块屏不算相邻边（只有角上那一点探得到）。没有相邻输出（内边缘）→ 收成图标，贴那条边（沿边那一轴按光标位置记锚点，图标就贴在「拖到哪儿」那一段）；有（跨屏边）→ **换屏、保持形态**（`vidow_mini_move_to_output()`：改 `view->output` + `mini_home`，锚点是相对工作区的比例，位置尺寸一个字都不用换算）。
       拖拽的收尾只有一份实现：浮窗拖拽（`vidow_float_end_grab`）和形态拖拽（`input.c` 的 `mini_pointer_release`）都调 `vidow_mini_drag_finish()` —— 两条路各写一遍必然漂移。
     - **滚轮档位**（`src/input.c` 的 `vidow_pointer_axis`）：在小窗上滚轮 = 形态档位，一次一格。往下滚降一档（FREE → MINI → ICON，到底停住），往上滚升一档（ICON → MINI → FREE）。**唯一的例外**：FREE 已经在最上面那一档，它的**上滚照旧转发给客户端**（自由小窗是可交互态，要能滚内容）；其余情形一律吞掉不转发。
     - **悬停工具条**（`src/drag.c` 的 `vidow_mini_toolbar_show/hide/hit` + `src/input.c`）：迷你 / 图标态悬停就露出四个按钮（放大 / 收成图标 / 还原成列 / 关闭）。它**不是表面**（合成器自绘的矩形），命中靠矩形判定 —— 绘制和命中共用 `server->mini_toolbar_btn_box` 一份坐标，不会各算一套。摆在小窗上方（上面放不下摆下面），整体夹进工作区。只读原则照 S2：不抢焦点、不进 MRU、不拿键盘，点中按钮是合成器自己的形态动作。
       两个坑都踩过并写进注释：① 指针从窗口往按钮上走时命中的是 `NULL`（工具条不是表面），不特殊处理的话「一离开窗口工具条就没了 → 永远点不中」；② 按钮可能压在窗口上，所以工具条命中要排在形态拖拽那条之前。按钮的那一对按下 / 松手成对吃掉（客户端两边都没见过）。
     - **键位动作**（`src/keybind.c`）：`cycle-mini-state`（三态循环：不在小窗态 / 自由 → 迷你 → 图标 → 自由 → 迷你 …，和 IPC 那份同一个顺序）、`mini-expand`（单步放大）、`mini-collapse`（单步收拢）、`unmini-window`（还原成列）。**动哪个窗口**：迷你 / 图标态**不拿键盘焦点**（S2 的死规矩），`server->focused` 指不到它们 —— 所以聚焦窗口不是小窗时退到「最近一个小窗」（`server->mini_last_id`，存 id 不存指针：窗口随时可能被关掉，指针会悬空）。
     - **实测**（headless 双屏 1280x720 @1x、`gaps = 10`，`HOME`/`XDG_RUNTIME_DIR` 指临时目录；注入走 `VIDOW_TEST_ACTION` 的 `pointer-motion` / `pointer-button`，和真鼠标同一条路）：
       - 落点区：拖平铺窗口到 `1180,60`（工作区 10,10 1260x700 → 落点区 1080,10 189x140）→ 松手 → `mini = "mini"`、`float_geometry = 909,18 353x196`（= 28% × 1260 / 28% × 700，贴右上角）；拖到 `400,400` 松手 → `mini` 仍是 `none`（**不误触**）。
       - 内边缘：迷你态拖到左边（外侧没有屏）松手 → `icon`，`float_geometry = -12,116 64x64`（`mini_icon_overflow = 0.35` → 左侧出屏 22px、屏内 42px），日志「拖到内边缘（左，外侧没有相邻输出）→ 收成图标」。
       - 跨屏边：自由小窗从 `HEADLESS-2` 拖过 x=1280 那条边松手 → `output = HEADLESS-1`、`mini` 仍是 `free`（**没被误收成图标**），geometry `1298,74 504x315`，日志「从 HEADLESS-2 挪到 HEADLESS-1（形态 free 不变）」。
       - 滚轮：`vidowmsg pointer-scroll 0 1` → `mini → icon`；`0 -1` → `icon → mini`；再 `0 -1` → `mini → free`。
       - 工具条：悬停露出（日志给了整块 `1298,58 151x32` 和四个按钮坐标），注入点 `1348,71` 按「收成图标」→ `mini → icon`。
       - 键位：`vidowmsg action cycle-mini-state` 连按四次 → `none → mini → icon → free → mini`；`mini-collapse` 两次 → `icon`、`icon`（到底停住），`mini-expand` → `mini`；`unmini-window` → `none`（还原成列）。
     - **改动面**：`src/server.h`（落点区 / 工具条的场景节点与命中状态 + `mini_last_id` + 声明）、`src/drag.c`（落点区 + 工具条 + 拖拽更新 / 松手挂点）、`src/float.c`（`vidow_mini_output_across` / `vidow_mini_edge_under_cursor` / `vidow_mini_drag_finish` / `vidow_mini_move_to_output` / `vidow_mini_toolbar_press`）、`src/input.c`（工具条命中与悬停、滚轮档位、拖拽松手判定）、`src/keybind.c`（四个动作 + `mini_action_view`）。`workspace.c` / `overview.c` / `im.c` / `gesture.c` / `main.c` / `config.c` 一行没改，**没有全屏特例**。
     - **键位示例落地了**：`config.example/configs/14-keys.lua` 与内嵌默认配置（`src/config.c` 的 `print_keys_lua()`）同时加上了那四行 —— `Mod+Shift+W` → `cycle-mini-state`、`Mod+Shift+Up` → `mini-expand`、`Mod+Shift+Down` → `mini-collapse`、`Mod+Shift+Return` → `unmini-window`，外加鼠标那条入口的说明（落点区 / 内边缘 / 跨屏边）。两份必须逐字一致（`test-defaultconfig.sh` 判据 3），只改一份就是漂移。
     - **没做**：S5 的过渡动画 / 悬停预览 / 图标出帧策略仍未做（S4 里形态切换是瞬时的）；落点区只在**光标底下那块屏**亮（要「每屏同时亮」得每屏一对矩形）。

274. **小窗模式 S3b：`mini_aspect` 三值生效（键位 > 规则 > 全局键）+ X11 `PAspect` 真的读了 + 拖拽锁比例 + contain 居中 —— 设了 `PAspect` 的游戏不再「关不掉锁」**（用户：「做方案 §11.3 + §7 S3 的宽高比那半」；方案 `/tmp/vidow-miniwin-plan.md` §11.3 + §8 判据 35–38b/43；2026-10-14）

     - **`mini_aspect` 是「有效值」算出来的，不是散在各处判的**：`vidow_mini_aspect_effective()`（`src/float.c:824`）—— 键位（运行时 `view->mini_aspect_runtime`）> 窗口规则（`mini_aspect_rule`）> 全局键（`config.mini_aspect`），**三级都是显式选择**（`auto` 也是「我选择按自动信号走」）。
       自动信号只在 `auto` 那一级看：X11 `PAspect` 优先，其次 `min == max` 的尺寸提示（`vidow_view_fixed_size_hint()`，Wayland 也认）；都没命中 → 不锁（Wayland 协议没有比例提示，所以那边 `auto` 等同 `free`）。
       `lock` / `free` 是**强制**：全局写 `free` 时，一个设了 `PAspect = 16/9` 的 X11 窗口**不再**自动锁（这正是用户报过的反直觉行为）。判「锁不锁 + 锁的比值」只有一个出口：`vidow_mini_aspect_lock()`（`src/float.c:840`），比值取内容比例（`base_dst`，没有才退回表面逻辑尺寸 —— 不混 X11 的缩放桥）。
     - **X11 `PAspect` 以前一条都没读**（`grep -rn aspect src/` 是空的）：新增 `vidow_view_aspect_hint()`（`src/view.c:4985`，紧挨 `vidow_view_fixed_size_hint()`）—— `XCB_ICCCM_SIZE_HINT_P_ASPECT` 且 min/max 两边都设了且**交叉相乘相等**才算「一个确定的比值」（只设一边是区间，不是锁定）。
       重判点用现成的 `x11_set_size_hints` 监听器（`src/view.c` 的 `x11_set_size_hints()` 里调 `vidow_mini_aspect_changed()`，`src/float.c:884`）：客户端常常**映射之后**才设它（游戏进全屏那一刻），那时进 FREE 态的盒子还是配置的初始比例 —— 这个函数当场按宽度重排出锁定比例，并在日志里说清「有效值来自哪一级、PAspect 有没有读到」。
     - **拖拽锁比例**：`vidow_float_grab_update()` 的 resize 分支（`src/float.c:626`）**宽度驱动**（`h = round(w / ratio)`，和 `vidow_tile_resize_*` 的手感一致，「拖右边」和「拖右下角」结果相同）。另加一条**必须**的回写：FREE 的盒子每次 arrange 都会被 `vidow_mini_resolve_geom()` 按 `mini_wf/mini_hf` 重算，拖出来的尺寸不回写锚点就是「拖了等于没拖」（实测确认）。
     - **contain 居中（修 S3a 报的偏差）**：以前内容左上对齐贴盒，信件箱全堆在右下。两条渲染路都要改，只改一条等于没改：
       ① 表面节点（`src/view.c:1867-1889`）—— 记下内容节点原来的位置（xdg 的窗口几何留白可能挂在父节点上），居中偏移减掉父节点那层，每帧重设（wlroots 每次 commit 会按自己的规则重算节点位置）；
       ② **装饰节点**（`src/view.c:2049-2056` 把 `mini_cx/mini_cy` 传给 `vidow_fx_frame()` 的 `x/y`）—— 默认开着装饰时屏幕上画的是它，表面节点那份是藏起来的（只留给命中测试）。S3a 那条「信件箱点不着」就是这里。
     - **迷你 / 图标态不受 `mini_aspect` 影响**：`vidow_mini_aspect_lock()` 第一句就是 `view->mini != VIDOW_MINI_FREE → false`；那两个形态永远按内容比例 `contain`（识别度问题，不给逃生口）。
     - **逃生口（顺手做了 S4 的一半）**：`toggle-mini-aspect`（`src/keybind.c:1294`，键位或 `vidowmsg action toggle-mini-aspect`）把当前自由小窗翻成 `lock` / `free`，**只作用于运行时、不写回配置**（`vidowmsg config mini_aspect` 照旧）；还原成列（`mini = NONE`）时忘掉这次翻转。
     - **实测**（headless 双屏 1280x720 @1x，`HOME`/`XDG_RUNTIME_DIR` 指临时目录；X11 客户端 `/tmp/xsolid`（纯色 + 可选 `PAspect`），Wayland 用 `tools/geowin`）：
       - **居中**：X11 625×700 内容进 FREE 盒 `388,203 504x315` → grim 逐像素量内容包围盒 `500,204 279x313`，**留白左 112 / 右 113**（旧行为是左 0 / 右 225），上 1 / 下 1；盒中心 `pick-window` 命中该窗口；内容比例 0.8922 → 0.8914（没拉伸）。
       - **规则 `lock`**：拖右下角 +120,+60 → `504x315`（比 1.6000）→ **`624x699`（比 0.8927，锁的就是内容比例 0.8929，偏差 0.02%）**；再横向拖 −60 → `564x632`（比 0.8924，偏差 0.03%）。
       - **规则 `free`**：同一拖法 → `664x335`（比 1.9821，比例变了 **23.9%**）。
       - **判据 38b**（全局 `free` + 不带规则 + X11 `PAspect = 16/9`）：拖 +150,+30 → `504x315` → `654x345`（比 1.8957，**变了 18.5%** = 全局 `free` 压过自动信号）；`vidowmsg set-config mini_aspect auto` 之后同一窗口再拖 → `774x435`（比 **1.7793**，16:9 = 1.7778，偏差 0.08% = 自动锁上了）。日志两行分别是「全局 free → 不锁」「全局 auto + PAspect 有 → 锁（比值 1.7778）」。
       - **Wayland**：`geowin` 400×300 在全局 `auto` 下拖 → 比例变了 16.7%（协议没有比例提示 → 等同 `free`）。
       - **迷你态**：`mini cw mini` → 盒 `464,262 353x196`，内容包围盒 `554,263 173x194`（比例 0.8918 ≈ 原生 0.8922），留白 **左 90 / 右 90**。
     - **改动面**：`src/float.c`（`vidow_mini_aspect_effective/lock/changed` + resize 锁比例 + 锚点回写 + 进 FREE 时那行日志）、`src/view.c`（`vidow_view_aspect_hint()`、`x11_set_size_hints` 重判、表面节点与装饰节点两处居中）、`src/server.h`（`mini_aspect_runtime` / `mini_base_dx,dy` 两个字段 + 四个声明）、`src/keybind.c`（`toggle-mini-aspect`）。`src/input.c` / `workspace.c` / `overview.c` / `config.c` 一行没改，**没有全屏特例**。
     - **没做**：`mini_icon_pause_render` 快照方案、悬停预览 / 工具条（S5）；「盒子本身按内容比例收」（MINI / ICON 的盒子仍是配置的 `mini_w x mini_h`，内容是 `contain` 居中，两条都满足「按内容比例显示」，但盒子没有跟着变形状）。

273. **小窗模式 S3a：原生分辨率保留（合成器缩放显示）+ 图标贴边裁剪 + 指针坐标换算 + 直通 / 基线两个守卫 —— 小窗化**不**重配客户端**（用户：「做方案 §7 的 S3 里『正确性』那半，宽高比留给 S3b」；方案 `/tmp/vidow-miniwin-plan.md` §7 S3 + §11.1/§11.2/§11.7；2026-10-14）

     - **原则**：小窗化只改**呈现**，不改客户端 —— 不 configure、不重建 swapchain、不触发客户端的 resize 路径（有的游戏一 resize 就重置成窗口模式）。
       默认 `mini_preserve_buffer = true`（S1 已有这个键 + 同名窗口规则），客户端从头到尾不知道自己进了小窗。
     - **一处挡口，三处生效**：`vidow_view_set_size()`（`src/view.c:395`）最前面加 `mini != NONE && 保留` → return。进小窗（`float.c` 的 `vidow_mini_set_state`）、arrange 摆位（`vidow_place_view`）、拖拽改盒子全走这个出口，
       所以挡一处就够。`vidow_float_on_commit()`（`src/float.c:289`）在保留时也直接 return：**不采纳客户端尺寸**（尺寸只由形态 / 锚点定）。
       关掉保留（`mini_preserve_buffer = false`）走原来那条：把客户端 configure 成小窗尺寸。
     - **缩放复用 overview 那条 `set_dest_size`**：`view_mini_scale()`（`src/view.c`，新）＝ `contain`（`min(盒宽/原生宽, 盒高/原生高)`，原生取 `base_dst`，没有就取表面逻辑尺寸），
       在 `view_anim_place()` 里把**场景节点**缩到 `原生 × ms` —— 放在 `fx.active` 判断**之前**（装饰全关 + 全屏直通正是「全屏游戏进小窗」最可能的状态，写在里面等于不生效）。
       装饰那条路跟着缩：`vidow_view_anim_bake()` 把 `view_scale(view) * ov` 和 `ov` **同乘** ms 传给 `vidow_fx_frame()`（`out_scale = scale/ov` 不变，画布和节点一起小）。
       副作用一个：`ov < 0.98` 会关掉这一次渲染的底衬模糊 —— 缩到几十像素的小窗本来也不该叠一层按原尺寸算的模糊（overview / MRU 缩略图同理）。
     - **每次 commit 都要重设**：wlroots 的 `surface_reconfigure()`（`types/scene/surface.c`）在**每次** commit 时把 dst 重置成表面自己的尺寸。我们的 commit 监听器排在它后面（xdg：`wlr_scene_xdg_surface_create` 先注册；X11：见 `x11_attach_surface`），所以设得住 ——
       但 `view_commit()` 里那句 `apply_anim` 原来挂在 `fx.active` 上，装饰全关的小窗就会每交一帧弹回原生尺寸，现在对迷你态也调（`view.c` 两处 commit 分支各改一个条件）。
     - **指针坐标换算（唯一的口子，泛化）**：`vidow_view_hit_scale()`（`src/view.c:585`）原来只对 X11 生效，现在对**所有**窗口算「表面逻辑宽 / 节点 dst 宽」；尺寸一致时返回 1.0（非小窗 / 非缩放路径逐字节不变）。
       `vidow_view_at()` 在返回前乘它，于是指针 motion / button / `vidowmsg pick-window` / overview 点击全部自动正确；drag 的取景框只用场景坐标，一行没动。
     - **两个必须挡的地方**：`vidow_view_scanout_ok()`（`src/fx.c:1791`）在 `fullscreen` 判断**之前**排除小窗（小窗刻意保留 `fullscreen` 标志不重配客户端，不挡就会把 2560x1440 的原生 buffer 整块扫上屏 = 小窗化失效）；
       `view_dst_capture()`（`src/view.c:663`）在小窗态**不记基线**（记了就把小窗尺寸当成 1:1 基线，退出小窗后 overview 里那个窗口永久变小一圈），退出小窗时 `view_anim_place` 还原节点尺寸并重记一次。
     - **图标贴边裁剪**：`view_clip_box()`（`src/view.c:1242`）加小窗分支，**强制裁到自己那块屏** —— 放在「浮动窗口一律不裁」那条放行**之前**（小窗态 `float_mode` 是 FIXED，晚一步就被放走）。
       这和浮窗允许跨屏是**相反**的语义，注释里写清了别合并（合并会搞坏跨屏拖拽 / `test-mixdpi`）。整块都在屏里时不裁（省一次重烘）。
     - **保活**：`vidow_view_render_pump()` 的 `want` 加 `mini != NONE`（被停到屏外 / 所在屏关了时补帧，否则游戏自己停帧、点开是死画面）；`view_on_screen()` 为真时它照样 `continue`，正常情况一分钱不花。
     - **实测**（headless 双屏 1280x720 @1x，`HOME`/`XDG_RUNTIME_DIR` 指临时目录；pattern.py 625×700 原生）：
       - **裁剪**：图标 64x64 贴 HEADLESS-1 右边、`mini_icon_overflow = 0.4` → 盒 `1232,10 64x64`，出屏 26px（其中 16px 落在屏 2）。
         屏 1 图标区非背景像素 **3000**（内容包围盒 `1231..1279 × 9..71`，可见那 48 列都在）；**屏 2 上「本该溢出的 16×64 条」非背景像素 = 0、整屏非背景像素 = 0**。
       - **缩放**：进图标态前后客户端收到的 `xdg_toplevel.configure` 条数 **2 → 3，三条全是 `625, 700`**（第三条只是 `set_tiled` 的状态位变化，尺寸没变；`vidowmsg windows` 的 `surface` 字段全程 `665x740` 缓冲 = 原生的 GTK CSD 尺寸，`geometry` 才是 64x64 的小窗盒）。
         grim 里图标内四个象限原色都在（`(1238,20)=(255,0,0)`、`(1270,20)=(0,255,0)`、`(1238,60)=(0,0,255)`、`(1270,60)=(255,255,255)`）= 625×700 的内容按比例缩进了 64px 的盒子。
       - **坐标换算**：wev（625×700 原生）进 FREE 态 → 盒 `1668,203 504x315`，`ms = 0.45`、`hit_scale = 2.2222`。光标注入到盒内 `(252,157)` → **wev 自己打印 `motion: x, y = 560.500000, 349.199219`**（期望 `560, 349`，差 < 1px）；`vidowmsg pick-window` 同一口子回 `(560.498, 349.199)`。盒内 `(126,78)` → `(280.249, 173.488)`（期望 `280,173`）。
     - **已知偏差（留给 S3b）**：`contain` 不居中（内容顶部左对齐贴在盒里），所以 FREE 盒里**信件箱那一条点不着**（上面 `0.75` 那个探针落在 281px 宽的内容盒右边 → 无命中）。盒子尺寸本身跟着内容比例走（「迷你 / 图标态永远按内容比例显示」）是宽高比那半的事。
     - **没碰**：宽高比（`mini_aspect` / X11 `PAspect` / 拖拽锁比例）、`mini_icon_pause_render` 快照方案、悬停预览 / 工具条 —— 全是 S3b/S5。`src/input.c` / `keybind.c` / `workspace.c` / `overview.c` 一行没改，**没有全屏特例**。
     - **改动面**：`src/view.c`（hit_scale 泛化、view_mini_scale / view_mini_native、anim_place 缩放 + 还原、anim_bake 传 `ov*ms`、clip_box 小窗分支、set_size 挡口、render_pump 保活、commit 两处条件）、`src/float.c`（`vidow_mini_preserve_buffer()` + on_commit 提前 return）、`src/fx.c`（scanout 排除小窗）、`src/server.h`（`mini_scaled` 字段 + 一个声明）。

272. **小窗模式 S2：可见性旁路（跟屏不跟工作区）+ 焦点守卫 + 输入路由 —— 小窗现在真的留在屏上、不抢焦点、收不到输入**（用户：「做方案 §7 的 S2」；方案 `/tmp/vidow-miniwin-plan.md` §3 / §4 / §5.3 + §7 S2；2026-10-14）

     - **可见性旁路（本次最重要的一条，做完小窗才留在屏上）**：`vidow_workspace_view_visible()`（`src/workspace.c:731`）在「最小化不算可见」那条**之后**加了一句 `if (view->mini != VIDOW_MINI_NONE) return view->output != NULL;` —— 和 overview 那条旁路同一个位置、同一个思想（可见性只有这一个判据口，所以旁路也只开在这儿）。为什么必须开：进小窗时 `view->ws = NULL`（S1 定的「小窗是屏级对象」），下面那句 `view->ws == active` 必然为假。改这一处，`vidow_view_should_park()` / `view_on_screen()` / `vidow_workspace_apply_visibility()` 三处**自动跟着对**（它们都问这个函数）。
     - **焦点守卫**（入口逐个）：① `vidow_focus_view()`（`src/view.c:3059`）紧挨 `if (view->minimized) return;` 加一条同形状的守卫 —— 迷你 / 图标态直接 return。不变量：`server->focused` 永远不指向 MINI / ICON 的窗口。② **进 MINI / ICON 之前先把焦点交出去**：`vidow_mini_set_state()` 调新函数 `vidow_view_yield_focus_for_mini()`（`src/view.c:3343`），规矩照抄最小化那条（`focus_after_close()` → 邻居顶上，没邻居就 `focus_nearest_column()`），顺带清掉指针焦点、把 `activated` 摘掉；必须**在摘列之前**调（`focus_after_close` 要靠窗口还挂在列里才找得到邻居）。③ `focus_follows_mouse()`（`src/input.c:846`）在 `if (server->locked) return;` 紧后面加一条同形状的跳过 —— 用户把它配成 `true` 也照样不跟小窗。
     - **输入路由**（入口逐个，全在 `src/input.c`）：① 指针移动 `vidow_pointer_motion()`（分流点 `:1093`）：命中迷你 / 图标 → `wlr_seat_pointer_notify_clear_focus()` 并 return（客户端收不到 enter / motion）；形态拖拽中走 `mini_grab_update()`（`:1085`）。② 指针按下 / 松开 `vidow_pointer_button()`：在**绑定表之前**分流（`:1317`；不然 `Mod+左键` 的 `interactive-move` 会顺着 `view_under_cursor` 抓到小窗、走浮窗那套 grab，那条路松手会把窗口重新认领回某块工作区，和小窗「跟屏不跟工作区」冲突），`mini_pointer_press()` / `mini_pointer_release()` 全程吞事件（`:1225` 那条管松手）。③ 滚轮 `vidow_pointer_axis()`（`:1516`）：命中迷你 / 图标 → return（「滚到底收图标」那条形态入口留给 S4）。④ 键盘：**一行都没改** —— 迷你 / 图标拿不到键盘焦点，按键自然透给真正的焦点窗口。⑤ 注入（`vidowmsg move-cursor / pointer-button / pointer-scroll / send-key`）和真设备同一条路，天然覆盖。
     - **单击 vs 拖拽**：判据在 `mini_grab_update()`（`src/input.c:922`）—— 用**从按下点起的累计位移** `hypot(dx,dy) > mini_drag_threshold_px`（默认 4 逻辑像素；**不是**每帧增量：慢速拖拽每帧只动 1px，按增量永远跨不过阈值），而且**单向不回退**（判成拖拽就锁到松手，否则慢速拖出去再拖回来会在松手那一刻突然放大）。≤ 阈值松手 = 放大（图标 → 迷你、迷你 → 自由小窗，自由小窗再给焦点）；> 阈值 = 移动（把光标位置折回**比例锚点** `mini_ax/ay` 再 `vidow_mini_resolve_geom()`，不直接写 `float_geom` —— 那样下一次 arrange 会被锚点算回来）。小窗拖拽中窗口被关掉 → `view_destroy` 里清 `mini_grab_view`（防悬空指针）。
     - **`vidow_view_at()` 为什么仍能命中它**：一个字都没改。它是场景图查询（`wlr_scene_buffer` → 往上找 `vidow_view_tag`），和「焦点 / MRU / IME / 悬停聚焦 / 客户端转发」那几条是两回事；迷你 / 图标态的场景节点在旁路之后是**开着**的，所以点得中。这正是和锁屏（S3）**方向相反**的地方：锁屏是「入口第一行整条短路」（连命中测试都不跑），小窗是「只吞转给客户端那一份，命中照跑」—— 单击要能放大就靠它。
     - **实测**（headless 双屏 `WLR_HEADLESS_OUTPUTS=2`，工作区 10,10,1260,700，临时脚本 `/tmp/vidow-s2-check.sh` 全过）：平铺窗口 `mini-window --state mini` → **911,14 355x198**（28% 工作区、锚点 1,0 贴右上）、`visible=true`、`workspace=null`、`mini_output=HEADLESS-2`；`grim -o HEADLESS-2` 抓到窗口内容（红色象限 **913,51 175x80**，14000 像素）；切到工作区 2 → geometry **逐字段不变**、红块同一位置；切回工作区 1 也一样；`focused` 是 `null`（不是小窗）；`focus_follows_mouse=true` 时光标压到小窗中心（1088,113）焦点也不变；注入指针移动 / 滚轮 / 按键 → 客户端探针**一行事件都没多**；单击（0 位移）→ `mini=free` **759,14 507x319**；拖 120,60 → `mini=mini` **791,74 355x198**（形态不变、位置变了）；切到 `free` 后同一个探针立刻收到 enter / 按键（证明不是「一律不给」）。
     - **回归**：`test-workspace.sh`（58 PASS）/ `test-focus.sh`（60 PASS）/ `test-ipc.sh`（162 PASS）全过，0 FAIL（`test-float.sh` 不存在，没跑）。
     - **未做**：S3（三态几何 / 图标贴边与裁剪 / 原生 buffer 缩放 / 宽高比）、S4（落点区 + 工具条 + 滚轮收图标 + 键位动作）、S5（过渡动画 + 悬停预览 + 图标出帧策略）。已知缺口两条：迷你 / 图标仍会出现在 Alt+Tab 列表里（`mru_listable()` 那一行按本次范围没动 —— `src/mru.c` 不在可改清单；选中它不会给焦点，被 `vidow_focus_view` 的守卫挡住）；最小化一个迷你 / 图标态窗口还没做「先展开成 FREE 再最小化」的归一化。

271. **小窗模式 S1：数据模型（三态形态 + 比例锚点）+ 配置键 + IPC —— 形态还不会自动切换**（用户：「先做方案 §7 的 S1，配置 + IPC 骨架先跑通」；方案 `/tmp/vidow-miniwin-plan.md` §6.1 / §6.2 / §6.5 / §6.6 + §7 S1；2026-10-14）

     - **小窗不是新的浮动模式**，是浮动窗口上的一个**形态**字段：`enum vidow_mini_state` = `NONE` / `FREE`（自由小窗）/ `MINI`（迷你）/ `ICON`（图标，贴边出屏一点）。进小窗 = 复用现成那套（`float_mode = VIDOW_FLOAT_FIXED` + `float_geom` / `home_dx,dy` / `float_place_recursive()` / `vidow_float_set_size()`），**没有**新开一条浮动路径。
     - **位置不记绝对坐标**，记「相对自己那块输出工作区的比例锚点」`view->mini_ax / mini_ay`（0 = 贴左 / 上，1 = 贴右 / 下；图标允许 −0.5..1.5 好出屏）。换算只有一处：`vidow_mini_resolve_geom()`（`src/float.c`）—— 改工作区尺寸 / 改缩放 / 拔插屏之后比例锚点天然跟着走，不用在每个热插拔路径里补换算。尺寸：`FREE` 记比例 `mini_wf / mini_hf`（`mini_remember_size` = 记住，客户端 resize 会回写比例），`MINI` / `ICON` 永远跟配置（改配置当场生效）。
     - **小窗是屏级对象**（像安卓的画中画）：进小窗时 `view->ws = NULL` 并从原来那块工作区摘账，`mini_home` 按输出名记着它属于哪块屏（拔屏再插回来按它认领）。
     - **⚠️ 记账那条有坑（本次修掉）**：`ws->floats` 的 `+1` 只在 `view_map()`（`src/view.c`，`float_mode != NONE` 那条）与 `ws_adopt_view()` 里记 —— `vidow_view_leave_column()` 自己**不动** floats（平铺窗口的账在 `columns` 上）。所以摘账必须**只对「本来就是浮窗」的那个减**：平铺窗口进小窗时再减一下就是减**别人**的账。实测（浮窗 `miniwin` + 平铺 `tilewin` 进小窗）：无条件减会把 `miniwin` 的计数吃掉（floats 1 → 0），修后全程稳定在 1。
     - **配置键 22 个**（`configs/24-mini.lua`，新文件 + 入口 include 行）：`mini_enabled` / `mini_drop_zone` / `mini_drop_zone_w,h` / `mini_w,h` / `mini_free_w,h` / `mini_icon_size` / `mini_icon_overflow` / `mini_icon_hover_ms` / `mini_icon_hover_leave_ms` / `mini_icon_hover_grace_ms` / `mini_icon_preview` / `mini_toolbar` / `mini_edge_snap_px` / `mini_transition_ms` / `mini_hover_fade_in_ms` / `mini_hover_fade_out_ms` / `mini_remember_size` / `mini_edge_margin` / `mini_preserve_buffer`（默认 `true`）/ `mini_aspect`（默认 `"auto"`）/ `mini_drag_threshold_px`（默认 `4`）。
     - **`mini_aspect` 是三个取值、三个都是显式选择**：`"auto"`（默认，让自动信号决定）/ `"lock"` / `"free"`，优先级 **键位（运行时）> 窗口规则 > 全局键**；`lock` / `free` 压过自动信号。只对自由小窗有效（迷你 / 图标态永远按内容比例）。读 X11 `PAspect` 的判定属于 S3，本次没做。
     - **窗口规则 4 个**（`rule.c` 的 `vidow_rule_fields[]`）：`mini_window` / `mini_state`（`"free"` / `"mini"` / `"icon"`）/ `mini_aspect` / `mini_preserve_buffer`。字符串字段取值当场校验（`mini_state = "miny"` 直接报错，不留到开窗才发现）。兑现放在 `vidow_rule_placement()` 里、**在 `VRA_MINIMIZE` 之前**（先定形态，再看要不要收起来）。
     - **三条写路径自动全吃**（照 `508239e` 的模式）：`mini_icon_size = 48`（config.lua）/ `VIDOW_MINI_ICON_SIZE=48 ./build/vidow`（环境变量）/ `vidowmsg set-config mini_icon_size 48`（IPC）；查询 `vidowmsg config mini_icon_size`。写错当场 `WLR_ERROR` 且**值不变**（`508239e` 的规矩）：`mini_aspect = "maybe"` 与 `mini_w = "60"`（缺 `%`）都被拒，旧值原样。
     - **IPC**：`mini-window [--state free|mini|icon]`（默认迷你）/ `unmini-window`（还原成列）/ `set-mini-state free|mini|icon|none` / `cycle-mini-state` / `mini-windows`（清单）。`vidowmsg windows` 每个窗口多 `mini` / `mini_output` / `mini_home` / `mini_aspect_rule` / `mini_anchor{x,y,wf,hf,size_known}`；事件 `window-mini-changed`（进了 `hooks.c` 的事件白名单）。
     - **验证**（headless，`WLR_HEADLESS_OUTPUTS=2`，脚本 `/tmp/vidow-mini-verify.sh`，工作区 = 1280x720 内缩 `gaps=10` → 10,10,1260,700）：
       - 规则：`vidow.rule{ title="^rulewin$", mini_state="icon" }` → 开出来就是 `icon`，几何 **1228,10,64x64**（= 工作区右缘 1270 − 64 + 出屏 `0.35×64=22`）✓
       - IPC：平铺窗口 `mini-window --state mini` → **909,18,353x196**（28% 工作区，锚点 1,0 贴右上，`mini_edge_margin=8` 夹回），`view->ws = null`、`mini_home = HEADLESS-1` ✓
       - 三态互切**保留锚点**（位置是用户拖出来的）：`free` → **758,18,504x315**（40%×45%，锚点还是 1,0，被 margin 夹回来）；`icon` → **1206,10,64x64**（`mini_edge` 只在「从 NONE 进 ICON」时才给，互切不重给 → 这次不贴边）。贴边那条由规则路径（NONE → ICON）验 ✓
       - 还原成列：`mini=none`、`floating=false`、几何回到条带上（宽 625 = `0.5 × (1260+10) − 10`）✓
       - `floats` 记账：真浮窗 `miniwin` 进小窗 → floats 减掉，还原 → 还回来；**平铺**窗口进小窗 → floats 不动（没吃掉别人的账）✓
     - **回归**：`test-defaultconfig.sh` 全过（新增的 `24-mini.lua` 与 `--default-config` 逐字节一致；`14-keys.lua` 里的规则字段说明块在 `config.c` 的生成器里同步加了 4 条）、`test-config.sh` 全过、`test-ipc.sh` 全过。
     - **未做（S2–S5）**：可见性旁路 / 焦点守卫 / 输入路由 / 渲染 / 过渡动画 / 落点区 / 悬停预览 / 键位动作 —— 所以小窗态现在会被现有可见性判据当成「工作区不亮着」停到屏外，这是预期。`mini_preserve_buffer` 目前只是数据 + 配置（按小窗尺寸重配客户端是 S3）；`mini_aspect` 的自动信号（X11 `PAspect`）读取也是 S3。

270. **窗口抓帧录出来就是「这个窗口在屏上的样子」：背后的模糊底衬 + 那圈画框（默认开，一个开关能关掉）**（用户：「能否混合背景（包括模糊）和 ssd」→ 澄清为「我是说窗口录制的时候」；2026-10-14）

     - **改前**：窗口抓帧（`src/wincapture.c` 的私有镜面场景 + 假输出，渲染在 `vidow_win_grab_render()`）画的是**客户端表面本身** —— 没有装饰、模糊、圆角、阴影（照 Hyprland 的窗口导出那个口径）。屏幕上的画框 / 底衬是 `src/fx.c` 的 `decor_pass()`（圆角 + 边框 + 投影 + 模糊底衬 + 颗粒，一个着色器一遍画完）画的，画在**正常场景**的装饰节点上，镜面场景里没有 —— 所以录不到。vidow 不画真标题栏（`src/decoration.c` 只做 CSD 协商），所以「录进去的 SSD」就是这圈画框。
     - **接法（两趟离屏 + 一趟场景，`src/wincapture.c` 的 `vidow_win_grab_render()`）**：
       ① 背后的模糊底衬（`vidow_fx_capture_decor(edge = false)`）烘成一张取景框那么大的图，挂在**镜像下面**的场景节点 `grab->decor_blur` 上；
       ② 圆角 / 边框 / 投影 / 颗粒（`edge = true`）烘成另一张，挂在**镜像上面**的 `grab->decor_edge` 上。
       两张图都走**屏幕上那同一个 `decor_pass()`**（`src/fx.c:2745`），差别只有几何：渲染目标是取景框、底衬采样按取景框的缩放换算（`k = frame_scale / 底衬那块屏的缩放`，见 `vidow_fx_capture_decor`）。中间夹着窗口内容那个镜像节点 —— 屏上的叠放次序就是「底衬 < 内容 < 画框」，一张图做不到（画框要压在内容上、模糊要垫在下面）。
     - **内容**由镜像节点自己画，所以**子表面（chromium 放视频那种）照旧在**；装饰图里只把内容贴图当「透明度剔除」的依据（着色器新加的 `u_content_alpha`：内容那一项乘 0 = 不画进这张图，但剔除仍按内容自己的 alpha 走）—— 半透明窗口该透出底衬的地方跟屏上逐像素一致。
     - **画框贴的是可见窗口**，不是整块表面：客户端自己画装饰时（CSD）表面比窗口大一圈透明留白，贴到表面边上录出来的框会比屏上那圈大一圈。用的是屏幕上同一份 `vidow_view_fx_geo()`（`tex_u0/v0/du/dv` 就是 `u_tex_map`）。
     - **底衬采样不许读全局量**：认**跟着贴图一起记的** `blur_out_*`（`server.h`），跨屏时拿窗口自己那块屏的缩放去算就是错位（提交 `f022b9e` 修过那个坑）；`k` 只有「取景框缩放 ≠ 那块屏缩放」时才不是 1。
     - **开关**：`wincapture_decor`（默认 `true`）。三条写路径都吃（config.lua / `VIDOW_WINCAPTURE_DECOR` / `vidowmsg set-config wincapture_decor false`），`vidowmsg config wincapture_decor` 查得回来，`config.example/configs/23-wincapture.lua` 是新的默认值示例（`--default-config` 与仓库里那一套逐字节一致）。关掉 = 改前那个样子（只有客户端表面本身，给 OBS 窗口源这类自己再加边框的用家）。
     - **锁定期间照旧不泄漏**：`server->locked` 时**两趟装饰都不跑**、镜像也关这一趟，回黑帧（提交 `078e9e2` 补的洞）。
     - **实测**（headless 双屏 + 软件渲染；`tools/wall.py "#00ff00"` 纯绿壁纸（background 层）+ 一个 50% 不透明的蓝窗（466×404）；`tools/capture.c` 抓窗口，`VIDOW_CAPTURE_DUMP` 出 PPM，472×409 = 193048 像素）：

       | | 窗口内容（蓝） | 底衬（绿透出来） | 边框（焦点色） | 黑（圆角外 / CSD 留白） |
       |---|---|---|---|---|
       | 改后（默认开） | 159073（82.40%） | **141190（73.14%）** | **3118（1.62%）** | 32110（16.63%） |
       | 关掉开关（= 改前） | 159119（82.42%） | 0（0.00%） | 0（0.00%） | 33640（17.43%） |

       逐像素对照**屏幕**（`grim` 同一时刻那一帧）：窗口那块屏上是 `(0,127,128)`（50% 蓝窗 + 绿壁纸的模糊）**140120 px**，抓帧里 `(0,127,128)` **141190 px** —— 底衬的颜色和位置跟屏上一致。两张图：`/tmp/wincapture-decor-on.png`、`/tmp/wincapture-decor-off.png`。
     - **回归**：`./test-wincapture-lock.sh` OK（8 PASS / 0 FAIL：锁前能抓 / 锁定期间 0 像素且 0 failed / 解锁后回来）；`./test-sessionlock.sh` **47 PASS / 0 FAIL**；`./test-defaultconfig.sh` OK（126 个键、新增的 `23-wincapture.lua` 与 `--default-config` 逐字节一致）。
     - **取舍**：取景框就是窗口那块（`vidow_win_grab_frame_box` 那套没动），所以投影落在框外那部分照旧被裁掉（屏上看得见、录进去看不见）；圆角外那四个角是透明的（PPM 里是黑），屏幕上那里是壁纸。**没有全屏特例**（全屏窗口的 look 本来就全是 0，和屏上一致）。

269. **空闲通知 + 空闲抑制两个 global（`ext-idle-notify-v1` / `zwp-idle-inhibit-v1`）：swayidle / hypridle 这类自动锁屏能接上了、播视频的客户端也能「别锁屏」（实测抑制真的生效）**（用户：「给 vidow 建这两个 global（各一行 create 的量级）；先核实 API 与版本；如果 wlroots 侧要求额外接线，照它的要求做」；方案 `/tmp/vidow-sessionlock-plan.md` §4 末尾 + §5 S5（独立可选步）；2026-10-14）

     - **API 照头文件核过（本机 wlroots 0.20.2）**：`/usr/include/wlroots-0.20/wlr/types/wlr_idle_notify_v1.h` →
       `struct wlr_idle_notifier_v1 *wlr_idle_notifier_v1_create(struct wl_display *display)`（**名字是 notifier，不是 notify** —— 所以
       `wayland-info | grep idle_notify` 永远不中，global 的接口名是 `ext_idle_notifier_v1`）；
       `/usr/include/wlroots-0.20/wlr/types/wlr_idle_inhibit_v1.h` → `struct wlr_idle_inhibit_manager_v1 *wlr_idle_inhibit_v1_create(struct wl_display *display)`。
       两个协议 XML 本机都有（`/usr/share/wayland-protocols/staging/ext-idle-notify/ext-idle-notify-v1.xml`、
       `/usr/share/wayland-protocols/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml`），但**两份都是 wlroots 自带实现**，
       不用像 `xdg-activation-v1` / `ext-background-effect-v1` 那样自己生成 + 自己实现。
     - **只建 global 不够 —— 两处接线 wlroots 不给，必须合成器自己接**（读 0.20.0 的 `types/wlr_idle_notify_v1.c` /
       `types/wlr_idle_inhibit_v1.c` 确认）：
       ① **活动要自己喂**：`wlr_idle_notifier_v1_notify_activity()`，头文件原话 "Compositors should call this function whenever an
       input event is triggered on a seat"。不喂的话通知在创建后 timeout 到点就 `idled`、之后再也不 `resumed` —— swayidle 会
       一直以为人在发呆（等于「一直在锁」）。vidow 自己**不**实现「超时后锁屏」，那是 swayidle / hypridle 的事。
       ② **抑制要自己转达**：`wlr_idle_inhibit_v1_create()` 只管收客户端的 `new_inhibitor`，一个字节都不碰 notifier；合成器得
       自己数着有几个 inhibitor 再调 `wlr_idle_notifier_v1_set_inhibited()`。计数不能靠「`manager->inhibitors` 空没空」判断 ——
       wlroots 是先 emit destroy、之后才把它从那个链表里摘掉。
     - **改了两个文件**：`src/main.c` 加一段 `idle_*`（两个 create + 活动监听 + inhibitor 计数 + `idle_init()` 调用 +
       `server_finish()` 里摘监听器）+ 4 个 include；`src/server.h` 加 5 个字段。`src/view.c` / `src/input.c` 等一行没动。
       活动信号挂在设备上（backend 的 `new_input`：键盘 `key`、指针 `motion` / `motion_absolute` / `button` / `axis`），
       设备销毁时把监听器摘干净 —— `wlr_input_device_finish()` 是先 emit destroy、再断言 `listener_list` 为空，所以只能在
       destroy 回调里摘（否则退出时 abort）。
     - **实测 global**（headless 双屏，`WLR_HEADLESS_OUTPUTS=2` + 软件渲染，临时 `HOME` / `XDG_RUNTIME_DIR`）：
       `wayland-info` 里 `ext_idle_notifier_v1` **v2**（name 39）+ `zwp_idle_inhibit_manager_v1` **v1**（name 40），两条都在。
     - **实测抑制 —— 真客户端，不是自造的一对**：本机有 `hypridle` v0.1.8（真 `ext-idle-notify` 客户端）当观察者，另写一个
       60 行的最小 inhibit 客户端（`/tmp/vidow-idle-test/inhibit.c`，**不进仓库**）当抑制者，`hypridle.conf` 里 `listener { timeout = 3;
       on-timeout = touch …/idled }`：
       ① 只有 hypridle → 3s 后 `on-timeout` 跑了（`idled` 文件出现）= `ext-idle-notify` 端到端通；
       ② 抑制着（inhibit 客户端先起）→ 7s 都没 `idled`（hypridle 的 3s 早过了）= **抑制生效**；
       ③ 抑制客户端 14s 后释放 → 3s 后 `idled` 出现 = 计数归零后通知恢复正常。vidow 日志同时有
       `空闲抑制: 有客户端要求别锁屏（在抑制 1 个）`。
     - **已知边界**：活动只挂**真设备**，所以虚拟设备（wtype / 输入法回注）和 IPC 注入的键鼠（`vidowmsg send-key` 之类）不算
       「人动了」—— 本步范围只让动 `main.c`；更靠内的单一入口是 `src/input.c` 的 `input_note_key_press()` 与
       `vidow_pointer_motion()`，把那两条接上就全覆盖（一行调用的事）。触屏没接（vidow 本来就没接 touch）。
       没有全屏特例。

268. **会话锁补漏：锁定期间窗口抓帧不再泄漏被藏窗口的内容（镜面场景只关这一趟、回黑帧而不是 failed）**（用户：「锁定期间窗口抓帧不能泄漏被藏窗口内容」；2026-10-14）

     - **洞在哪**：窗口抓帧走的是私有镜面场景 `grab->scene`（`wlr_scene_subsurface_tree_create`，`src/wincapture.c:398`）+ 自己的假输出 `grab->scene_output`（`:304`/`:344`），跟锁屏时被 `set_enabled(false)` 的那棵正常树**没有任何关系** —— 输出抓帧（`src/capture.c`/`screencopy.c`/`screencast.c` 输出源）已实测不泄，只有窗口这条路照样把被藏窗口画出来。入口：`tools/capture.c`（ext-image-copy-capture + ext-foreign-toplevel-list 的窗口源，和 xdg-desktop-portal 选择器 / OBS 窗口源同一条路）→ `src/portal.c:113` 的 `capture_request` → `vidow_win_capture_source_create()`（`src/wincapture.c:728`）→ `source_request_frame()`（`:616`）→ `source_render()`（`:571`）→ **`vidow_win_grab_render()`（`src/wincapture.c:264`）**。
     - **方案（最小，7 行）**：渲染前判 `grab->server->locked`，是就把镜像节点 `wlr_scene_node_set_enabled(&grab->mirror->node, false)` 关一趟、`build_state` 之后立刻开回来。这一帧就是空场景 → 纯黑（wlroots 按 damage 铺黑底，`types/scene/wlr_scene.c:2574`）。判据**每帧现读**，不存状态、不挂锁定/解锁信号，解锁后下一次调用自动照旧。
     - **为什么不直接拒绝、不 `return NULL`**：回 NULL 等于回 failed，客户端得反复换缓冲重试（`source_copy_frame()` → `..._fail`），录屏的窗口流（`src/screencast.c:1063` 是同一个 `vidow_win_grab_render`）也会卡住。黑帧是协议允许的「这一帧没内容」，客户端不用换缓冲。**没有全屏特例。**
     - **实测**（headless 双屏 + 软件渲染，`tools/locktest.c` 上锁；客户端抓 `tools/bluewin.py` 的纯蓝窗，671×748，29 帧/3 秒）：锁前蓝窗 **423401 px（84.36%）**；锁定期间 **改前 423401 px（与锁前逐像素相同 —— 洞）→ 改后 0 px（0.00%）、非黑 0 px**，29 帧 0 failed；解锁后 **423401 px** 回来。（改前数据是临时把 `hide_mirror` 改成 `false &&` 重编译实测的，已还原并重编译。）
     - **顺带修的测试工具 bug**：`tools/capture.c` 的 PPM dump 原来写死按 XRGB8888 读，而合成器给窗口源报的 shm 格式是 **XBGR8888**（`shm_format 0x34324258`）—— 蓝窗读出来是红的、脚本的「窗口内容」判据永远数不到（基线先 FAIL）。现按 `session_shm_format` 分流。
     - **回归**：`./test-wincapture-lock.sh` OK（锁前能抓 / 锁定期间 0 像素且 0 failed / 解锁后回来 / 没崩没 assert）；`./test-sessionlock.sh` **47 PASS / 0 FAIL**。唯一取舍：锁定期间被藏窗口仍会收到帧回调、还会继续画（内容到不了抓帧客户端），要一并省掉是另一处改动。
267. **会话锁 S4：能解锁了（桌面恢复 + 焦点还给窗口）、客户端崩溃保持锁定、锁定期间抓帧不泄桌面（实测）**（用户：「接着 S3 做方案 §5 的 S4，先做解锁（最缺的）再做抓帧审计；别做多余的无用测试」；方案 `/tmp/vidow-sessionlock-plan.md` §2 环节 6 + §3 F/H + §4 风险 3 + §5 S4；2026-10-14）

     - **解锁**（S3 遗留的最大缺口：以前锁上只能杀客户端或切 VT）：`src/sessionlock.c` 新增 `lock_restore_desktop()`，在 `lock_handle_unlock()`（`lock->events.unlock`，
       也就是客户端的 `unlock_and_destroy`）里调，做三件和「锁上」严格对称的事 —— ① 正常树（`layer_backdrop`…`layer_im`）`set_enabled(true)`、
       `layer_lock_blank` / `layer_lock` 关掉；② `vidow_focus_view(server->focused)` 把键盘焦点还回原来那个窗口（`focused` 为空就 `vidow_seat_focus_surface(seat, NULL)` 清焦点，
       别留在一个马上要被销毁的 lock surface 上）；③ `vidow_arrange(server)` 重排一次（锁定期间可能插拔过输出）+ 给每块屏主动要一帧。
       恢复放在 unlock 事件里而不是等 destroy：两者在同一次事件循环分发里跑完（wlroots 的 `lock_handle_unlock_and_destroy` 先 emit `unlock` 再销毁 resource），
       中间不渲染，所以不存在「桌面回来了但 lock surface 还画着」的中间帧。
     - **客户端崩溃 / 被 kill → 不解锁**（协议 XML:111-113）：`lock_handle_destroy()` 现在按 `server->locked` 分流 —— `true` = 没走 unlock 就没了，
       什么都不恢复（树继续关、每块屏的黑 rect 继续开着、`locked` 继续吞输入），打一行 ERROR `lock 对象没了但没有 unlock ——客户端崩溃 / 被 kill，按协议保持锁定（黑屏死锁，只剩 VT 切换能出去）`；
       `false` = 解锁流程收尾。黑 rect 是锁上那一刻建的（不依赖 lock 对象），所以这条路上屏只会退回协议允许的「纯色」兜底，**绝不会漏桌面**。
       崩溃后如果又来一个锁屏客户端，`new_lock` 会照常接管（协议允许「新客户端接手解锁」，`session_lock` 已经是 NULL）。
     - **`src/view.c` 一行没动**（方案 §4 明确要求）：重聚焦直接复用现成的 `vidow_focus_view()`。`capture.c` / `screencopy.c` / `screencast.c` 也**一行没改**（审计下来零泄漏，见下）。
     - **抓帧审计（方案 §4 风险 3，最高优先）—— 实测，不是推断**。vidow 的输出抓帧是**自己渲染一帧**，三条输出路径都汇到同一个渲染引擎：
       ① `zwlr_screencopy`（`grim` / `wf-recorder`）→ `src/screencopy.c:388` 的 `vidow_capture_grab()`；
       ② `ext-image-copy-capture` 输出源（xdg-desktop-portal 截图）→ `src/capture.c:226` 的 `wlr_scene_output_build_state()`（渲染的是 `server->scene` **整棵树**，
       `set_enabled(false)` 的树天然不参与，和屏幕那一帧同一个场景图）；
       ③ KDE screencast 输出流 → `src/screencast.c:1048,1069` 的 `vidow_capture_grab_into()`（同一个函数）。
       ① ② 用真客户端抓真像素（`test-sessionlock.sh` 的「抓帧审计」两步）：
       - **grim**（zwlr_screencopy）锁定期抓 HEADLESS-1：**锁屏色 100.00%（容差 ±2）、纯黑 0.00%、桌面像素 0（0.00%）**；
       - **wf-recorder** 锁定期录 2 秒（screencopy 的连续帧 / `copy_with_damage` 那条路，`-o HEADLESS-1`）→ `ffmpeg` 抽第 1 帧：
         **锁屏色 100.00%、与锁前桌面逐像素相同 0.00%**（容差 ±24 —— 视频帧过一趟 YUV420 往返有色偏 ±10 量级，所以这条路的容差要放宽，
         同时拿锁前桌面图当对照：真漏了的话「与锁前桌面相同」会接近 100%）。
       ③ 没有便宜的免 portal 客户端（本机没装 `ext-image-copy-capture` 输出源客户端），只给代码路径证据（同一个 `vidow_capture_grab_into`）。**结论：三条输出路径都没泄漏，零改动。**
     - 验证（`test-sessionlock.sh`，headless 两块屏；A/B/C/G1/G2/D/E 仍全绿，只补方案 §3 的 F/H + 抓帧审计）：
       - **F**（锁定期 `kill -9` locktest）：vidow 活着；日志有上面那行「客户端崩溃 / 被 kill」；`grim` 抓 HEADLESS-1 = **纯黑 100.00%、桌面像素 0**（没自动解锁）；
         **真键盘**（`wtype -M logo -k j`，走 `input.c` 的 `keyboard_key`）按已绑定的 `Mod+J` → 工作区 / 焦点前后一致 + 日志仍有「锁定期间不生效」。
       - **H**（locktest stdin 写 `u` → `unlock_and_destroy`）：日志「解锁了，桌面树恢复（focused 已重新聚焦）」；两块屏 `grim` 与锁前**逐像素相同 100.00%、最大差 0、平均差 0.000**
         （对照：锁中那张与锁前最大差 126）；解锁后同一个 `wtype -M logo -k j` → **工作区真的切了**（`active=[(1,None),(1,None)]` → `[(1,None),(2,None)]`，即 HEADLESS-1 切到 2 号），
         且不再新增「锁定期间不生效」；解锁后屏上锁屏色占比 0.0%（`layer_lock` 那两棵树确实关掉了）。
     - **顺手修掉一个测试自身的坑**：`test-sessionlock.sh` 的配置里那个绑定以前写的是 `vidow.bind("Mod+J", "focus-workspace 2")` —— `focus-workspace` **不是动作**
       （vidow 会打 `action: 不认识的动作 'focus-workspace 2'`），D 那条断言照样成立（白名单在动作解析之前就挡下了），但 H 要「解锁后键位真的生效」就必须用真动作名，
       已改成 README 动作表里的 `"workspace 2"`。同理 F/H 不用 `vidowmsg send-key` 而用 `wtype`：**注入那条路平时本来就不触发键位表**（`src/input.c` 的 `inject_key`），
       只有锁定期才被特意接到白名单上，拿它量不了「解锁后键位恢复」。
     - **会话锁现在的准确状态**：**能锁、能解锁**（客户端 `unlock_and_destroy` → 桌面 + 焦点 + 布局都回来）、**客户端崩溃保持锁定**（黑屏死锁，只剩 VT 切换能出去，
       `vidowmsg` 故意没有强制解锁后门）、**锁定期间输出抓帧 / 录屏不含桌面**（上面三组像素数字）。回归 `test-sessionlock.sh` 全绿。
     - **还没做 / 已知边界**（如实记）：
       - **自动锁屏（idle）没做**：`wlr_idle_notify_v1` / `wlr_idle_inhibit_v1` 两个 global 都没建，`swayidle` / `hypridle` 这类「空闲触发锁屏」的客户端现在还接不上（方案 §5 的 S5，独立可选步）。
       - `vidowmsg pick-window x y` 这类**只读探测**锁定期间仍会做命中测试（`src/ipc.c:1901`）—— 它不改输入路由，但会暴露「被藏起来的窗口在哪」。本次范围外，没动。
       - **窗口**抓帧（`src/wincapture.c` 的 `vidow_win_grab_render()`，给 `ext-image-copy-capture` 的 toplevel 源 / KDE 的窗口录屏用）走的是**单独一棵镜面场景**，
         不经过被关掉的那棵正常树 —— 锁定期间它仍然能抓到被藏起来的那个窗口的内容。这不在方案 §4 风险 3（「输出抓帧漏桌面」）的范围里，`wincapture.c` 也不在本次允许改的文件里，**只记不修**，要处理得单独定策（合成器对 toplevel 源有没有权限模型是另一个问题）。
       - 锁定期间 lock surface 的键盘焦点策略仍是「光标所在那块屏」（S3 定的），多屏手感的调优没做。

266. **会话锁 S3：输入路由 —— 锁定期间键盘 / 指针只给 lock surface，键位只放行白名单（VT 切换）**（用户：「接着 S2 做方案 §5 的 S3，输入路由是最高危的一步；45 分钟时间盒，先把最高危的漏点做完」；方案 `/tmp/vidow-sessionlock-plan.md` §2 环节 3 + §3 D/E + §5 S3；2026-10-14）

     - 落点：`src/sessionlock.{c,h}`（新增三个查询 / 一个聚焦：`vidow_sessionlock_keyboard_surface()` / `_pointer_surface()` / `_focus()`，
       外加锁定瞬间收掉在跑的手势 `lock_cancel_input_gestures()`）；`src/input.c`（`lock_key_route()` / `lock_pointer_route()` 两条唯一的路，
       六个输入入口第一行短路）；`src/keybind.c`（白名单 `bind_locked_allowed()` = 只有 `switch-vt`）；`src/drag.c` / `src/gesture.c` /
       `src/overview.c` / `src/im.c`（各自入口第一行守卫）；`tools/locktest.c`（补 wl_seat：把收到的键 / 指针事件打出来，S3 需要「锁屏那一侧」的观察者）；
       `test-sessionlock.sh`（补 D / E + 两条语义）。**`view.c` / `fx.c` / `clipboard.c` / `xwayland.c` / `cursorfx.c` 一行没动**，没加任何全屏特例。
     - **策略**（协议 XML:246-250 允许合成器自定）：键盘焦点和指针焦点都给「**光标所在那块屏**的 lock surface」（光标不在任何屏上 / 那块屏还没 lock surface 时
       退回第一块有 lock surface 的屏）。指针局部坐标 = 光标位置 − 那块屏的布局框原点。一块 lock surface 都没有时指针焦点**清掉**（`notify_clear_focus`），绝不留给被藏起来的窗口。
     - **短路清单**（锁判断在每个入口第一行，方案 §4 风险 1 的「单一 choke point」）：键盘 `keyboard_key` / `keyboard_modifiers`；指针 `vidow_pointer_motion` /
       `vidow_pointer_button` / `vidow_pointer_axis`；`focus_follows_mouse`；`vidow_view_at`（在 input.c 的两个调用点 + `keybind.c` 的 `view_under_cursor()` +
       `overview.c` 的 `vidow_overview_click()` 前短路 —— `view.c` 里的函数本身不动）；`drag.c` 四个入口；`gesture.c` 五个 begin/update（end 放行，好让锁定前开着的手势自己收尾）；
       `overview.c` toggle/click/scroll；`im.c`（`vidow_im_owns_keyboard()` 直接返回 false、`vidow_im_key()` / `_modifiers()` 不进、`vidow_im_focus()` 给所有输入框发 leave）。
       锁定瞬间还会**主动收掉**正在跑的拖拽 / overview（`lock_cancel_input_gestures()`）—— 留着一个半截的拖拽到解锁之后才发作，比当场收掉糟得多。
       **手势配置表不碰**：`vidow_gesture_clear()` 清的是 `config.lua` 绑的那张表（配置重载用的），锁定瞬间调它等于把用户绑的手势全删了（写完第一版就踩到了，
       `test-sessionlock.sh` 现在拿 `vidowmsg state` 的 `gestures` 当回归判据：锁定前后都是 `4 指竖扫`）。正在跑的那个手势由它自己的 end 事件收尾。
     - **键位白名单**（`src/keybind.c`）：`vidow_keybind_key()` 一进门就判 `server->locked`，锁定期间只走 `keybind_locked_key()` —— 绑定表里只认
       `switch-vt`（VT 切换是客户端死掉时用户唯一的逃生口，方案 §4 风险 5），其余绑定**命中也不跑**，并且打一行
       `sessionlock: 绑定 Mod+J（focus-workspace 2）锁定期间不生效 —— 白名单只有 VT 切换`（测试 D 就拿这行当判据：有它 = 键确实走到了绑定表，只是被挡住，
       不是「注入本来就不触发键位」）。鼠标键 / 滚轮绑定同样有第二道闸（`vidow_keybind_button()` / `_wheel()` 锁定期间直接吞）。
     - **两条语义写死，不靠默认值**（方案 §4 风险 2）：
       - `mouse_move_enables_dpms` / `key_press_enables_dpms` **继续生效** —— 锁判断放在 `input_note_key_press()` 和 DPMS 唤醒**之后**。
         否则屏关着就没法输密码。安全性靠「唤醒后画的是黑 rect / lock surface」保证，不靠不唤醒。
       - `focus_follows_mouse` **必须不生效** —— 用户配成 `true` 也照样不跟（否则会把键盘焦点给被藏起来的窗口）。
     - 验证（`test-sessionlock.sh`，headless 两块屏，A/B/C/G 仍全绿，只补方案 §3 的 D/E）：
       - **D**（锁定期间 `vidowmsg send-key tap super+j`，`Mod+J` 已绑定到 `focus-workspace 2`）：工作区 `active=[(1, None) × 3]` **前后一致**、
         焦点 `focused=2(bluewin)` **前后一致**；日志出现上面那行白名单拒绝；`locktest` 打出 `key 125 down / key 36 down / key 36 up / key 125 up` ×2
         （键送到了锁屏界面）；
       - **E**（`move-cursor` 到「没拿到焦点的那个窗口」的几何中心 + `pointer-button press/release left` + `pointer-scroll 0 60`）：
         焦点仍是 `focused=2(bluewin)`（`focus_follows_mouse = true` 也不跟被藏起来的窗口）；`locktest` 收到 `pfocus` / `button 272 down` / `button 272 up` / `axis 0 900.0`
         （指针只给了锁屏界面）；
       - **语义①**：锁定期 `vidowmsg output-power HEADLESS-1 off` → `enabled=[False]` → `move-cursor 20 20` → 日志
         `输入唤醒: 打开 1 块关着的屏（dpms）`、`enabled=[True]`；
       - **语义②**：`vidowmsg config focus_follows_mouse` 查回来是 `true`（先证明这条开关真的开着，E 的判据才成立）。
     - **仍是 S3，别对外宣称支持会话锁**：**解锁（S4）还没做** —— 锁定期间客户端请求 `unlock_and_destroy` 只记一行日志、客户端死掉也不许解锁（协议 XML:111-113），
       所以**现在锁上之后只能杀掉客户端进程、或者切 VT 退出**（没有别的出口，`vidowmsg` 也**故意**没有强制解锁后门）。S4 的其余部分（解锁恢复桌面 + 重新聚焦 + rearrange、
       客户端崩溃的「死锁」态定稿、`capture.c` / `screencopy.c` / `screencast.c` 锁定期不泄桌面的审计）也没做。
       另外两条 S3 遗留（如实记）：① `vidowmsg pick-window x y` 这类**只读探测**命令锁定期间仍会做命中测试（`src/ipc.c:1901`，本次范围外）——
       它不改输入路由，但会暴露「被藏起来的窗口在哪个位置」；② `vidowmsg send-key` 的注入路在锁定期间已和真键盘同一条路（白名单 + lock surface），
       但**未锁**时的注入照旧不触发键位表（老行为，没动）。
       所以 `test-sessionlock.sh` 全过只说明「能锁、能黑屏、能握手、热插拔不漏桌面、输入不漏」，**不说明能当锁屏用**。

265. **会话锁 S2：lock surface 生命周期（尺寸变了重 configure）+ 输出热插拔（锁定期插屏立刻补黑幕 / 拔屏清账）**（用户：「接着 S1 做方案 §5 的 S2，别推倒重写、别做多余的无用测试」；方案 `/tmp/vidow-sessionlock-plan.md`；2026-10-14）

     - 落点：`src/sessionlock.c` / `src/sessionlock.h`（`lock_output_apply_box()` 统一「把黑 rect + 这块屏的 lock surface 对齐到一个布局框」，
       外加 `vidow_sessionlock_output_sync()` / `_added()` / `_removed()` 三个入口）；`src/output.c`（输出 commit 监听、新屏记账、拔屏清账）；
       `src/server.h`（每输出一个 `commit` 监听器）；`test-sessionlock.sh`（补 G1/G2）。**S1 的握手语义一行没改**：`send_locked` 还是只发一次，
       后续新屏的黑 rect 不依赖它。**没加任何全屏特例**，`view.c` 的列逻辑照旧一行不动。
     - **尺寸变化 → 重新 configure**：S1 只在 `new_surface` 时 configure 一次。现在每输出挂 `wlr_output->events.commit`：
       锁定期间一旦提交新状态（`vidowmsg output-scale`、SIGHUP 换缩放、换模式、热插拔重排），立刻按 `wlr_output_layout_get_box()` 的当前框
       对齐黑 rect 并重发 configure（客户端只认最后一次 configure —— 协议要求 commit 的尺寸等于最后一次 ack 的）。
       **为什么不等下一帧**：`output->box` 是渲染时每帧刷新的（`src/output.c:1023`），而锁定期间没人动的话可能一直不出帧，那客户端就永远按旧尺寸画。
       所以 commit 回调里直接问布局要框，**不碰 `output->box`**（那条日志 `布局位置 a,b WxH -> c,d WxH` 是 `test-hotplug.sh` 在解析的，不能抢它的活）。
       实测：锁后 `vidowmsg output-scale HEADLESS-1 2` → 客户端收到第二次 `configure HEADLESS-1 640x360`，
       日志 `sessionlock: HEADLESS-1 现在是 640x360，lock surface 重新 configure 了`。
     - **锁定期间插屏**：`server_new_output` 里黑 rect 是照旧建的（挂在 `layer_lock_blank` 下，那棵树锁定期间开着），所以新屏从第一帧起就是黑的，
       桌面漏不出去；新增的 `vidow_sessionlock_output_added()` 打一行日志给测试当判据，并在 `locked` 还没发时把这块屏记进握手账（它可能一帧都还没画过，
       不等它就等于把「可能还有桌面残留」的屏放行了）。lock surface 由客户端随后建（走和别的屏一样的 `new_surface`）。
     - **锁定期间拔屏**：`output_destroy` 里 `vidow_sessionlock_output_removed()` 清掉这块屏的 lock surface 记账（摘自己的 destroy 监听再释放，
       和 wlroots 那条 `output->events.destroy` 销毁 lock surface 的路谁先跑都不会二次回调）和「还没呈现锁后第一帧」的账
       （一块永远出不了帧的屏不能把 `locked` 一直拖着）；黑 rect 照旧在 `output_destroy` 里收。
     - 验证（`test-sessionlock.sh`，headless 两块屏，只跑方案 §3 的 C/G）：
       - A/B/C 复跑全绿（B：`LOCKED` <5s、两块屏各一次 `configure`；C：锁后两块屏 `#226699` **100.0%**，锁前 `HEADLESS-2` 上 445621 个蓝窗像素）；
       - **G1 插屏**（`VIDOW_TEST_OUTPUTS="12000:add:1280x720"`，锁定期间）：日志
         `sessionlock: 锁定期间新屏 HEADLESS-3 到了，黑 rect 已就位（1280x720 @ 2560,0）`；`locktest` 为新屏建了 surface 并收到第 3 次
         `configure HEADLESS-3 1280x720`；`grim -o HEADLESS-3` 量出来 **锁屏色 100.0% / 纯黑 0.0% / 桌面像素 0**；
       - **G2 拔屏**（另起一遍 vidow，`VIDOW_TEST_OUTPUTS="12000:remove:HEADLESS-2"`，锁定期间）：日志
         `sessionlock: 输出 HEADLESS-2 拔了，清掉 1 块 lock surface 记账（还有 0 块屏没呈现锁后第一帧）`；vidow 不崩；
         `grim -o HEADLESS-2` 失败（真没了）；剩下那块屏 **锁屏色 100.0% / 桌面像素 0**；日志里没有 sessionlock 的错误行。
     - **仍是 S2，别对外宣称支持会话锁**：**输入路由（S3）还没做** —— 锁上之后键盘 / 指针 / 触摸 / 拖拽 / 手势 / overview / IME
       仍然会漏给未锁客户端，锁形同虚设。S4（解锁恢复桌面、客户端崩溃保持死锁、抓帧审计）也没做。
       所以 `test-sessionlock.sh` 全过只说明「能锁、能黑屏、能握手、热插拔不漏桌面」，不说明能当锁屏用。

264. **会话锁 S1：`ext-session-lock-v1` 的 global + 涂黑 + `locked` 握手（新建 `src/sessionlock.{c,h}`、`tools/locktest.c`、`test-sessionlock.sh`）**（用户：「先做方案 §5 的 S1，别做多余的无用测试」；方案 `/tmp/vidow-sessionlock-plan.md`；2026-10-14）

     - **协议不用自己实现**：wlroots 0.20（本机 0.20.2）自带 `wlr_session_lock_v1`，
       一行 `wlr_session_lock_manager_v1_create()` + 听 `new_lock` 就有 `ext_session_lock_manager_v1` 这个 global。
     - 落点：新 `src/sessionlock.c` / `src/sessionlock.h`；`src/server.h`（`locked` / `session_lock` 等字段 +
       `layer_lock_blank` / `layer_lock` 两棵树 + 每输出 `lock_blank` rect 与 `lock_frame_pending`）；
       `src/main.c`（建两棵树、init 链加一行、退出时摘 `new_lock` 监听器）；`src/output.c`（建/收黑 rect、
       box 变了同步、帧回调里给握手记账）；`meson.build`（源列表 + 生成 `ext-session-lock-v1` 客户端协议 + `locktest`）。
       **`src/view.c` / `overview.c` / `fx.c` / `input.c` / `keybind.c` 一行没动** —— 锁是**会话级状态**（场景图层 + 输入路由），
       不是窗口、不进列、不参与焦点/MRU/工作区，所以没有「给全屏加特例」这回事（方案 §2.2）。
     - 涂黑：锁定瞬间把 `layer_backdrop`…`layer_im` 全部 `wlr_scene_node_set_enabled(false)`，开 `layer_lock_blank`
       （每输出一块不透明黑 rect，节点常开、由那棵树统一开关）与 `layer_lock`（放 lock surface，建在 `layer_im` 之后 = 最顶）。
       协议 XML:77-82 要求锁定期间只渲染 lock surface，所以「把别的藏起来」不是可选的美化。
     - **最容易踩的坑**：wlroots **不会**自动发首个 configure（读 0.20 的 `types/wlr_session_lock_v1.c`：`new_surface` 只建对象就 emit）。
       所以 `new_surface` 里必须主动 `wlr_session_lock_surface_v1_configure(输出尺寸)`，否则客户端永远收不到尺寸、不 ack、不 commit
       （协议要求 commit 前必须先 ack 首个 configure）= 锁定握手死锁。实测不 configure 就是客户端一直不 commit。
     - `locked` 握手：锁上后给每块屏记一笔 `lock_frame_pending`，在已有的每输出帧回调里销账（`src/output.c:182` 的
       `send_frame_done` 之后），**数齐了才 `wlr_session_lock_v1_send_locked()`**（XML:87-107 不许提前发）。
       某块屏一直不出帧就 **500ms 超时兜底**照样发（XML:96-98 的 "reasonable time limit"），日志会写明还剩几块屏没画过。
     - 测试客户端 `tools/locktest.c`：本机 `swaylock`/`wayland`/`gtklock`/`hyprlock`/`i3lock` **一个都没有**，
       所以照 `tools/pointerlock.c` 的骨架写了个最小的（lock → 每输出 `get_lock_surface` → 收 configure 就 ack + 贴纯色 shm +
       commit → 收 `locked` 打 `LOCKED`；stdin 输 `u` 解锁 / `q` 退出）。协议 XML 用系统的（`wayland-protocols` 1.49 的
       `staging/ext-session-lock/`），不往 `protocols/` 里 vendor。
     - 验证（新 `test-sessionlock.sh`，headless 两块屏 `WLR_HEADLESS_OUTPUTS=2`；只跑方案 §3 的 A/B/C）：
       - A `wayland-info | grep -c ext_session_lock_manager_v1` = **1**；
       - B `locktest` 起后 **<5s 内打出 `LOCKED`**（实测同一毫秒内两块屏各收到一次 `configure 1280x720`）；
       - C `grim` 前后比对：两块屏锁后都是 `locktest` 的纯色 `#226699` **100.0%**，锁前 `HEADLESS-2` 上有 445621 个蓝窗像素
         （`tools/bluewin.py`），前后最大差 213 / 126 —— 桌面和窗口确实被藏起来了；
       - 超时兜底专项（`VIDOW_TEST_STALL_OUTPUT=HEADLESS-1` 掐掉那块屏的帧事件）：日志出现「等了 500ms 还有 1 块屏没呈现
         锁后第一帧，按超时兜底发 locked」，客户端仍在 **508ms** 拿到 `LOCKED`。
     - **未做（后续步骤，别对外宣称支持会话锁）**：S2 输出热插拔 / 尺寸变化重 configure、**S3 输入路由**（所以现在锁上之后
       键盘/指针**还会漏给未锁客户端**，锁形同虚设）、S4 解锁恢复桌面 / 客户端崩溃保持死锁 / 抓帧审计。
       本次**故意**不做解锁：客户端退出后合成器按协议 XML:111-113 保持黑屏锁定态（要恢复桌面得等 S4）。

263. **每窗口按需 VRR（niri 的 `variable-refresh-rate on-demand`）+ 每屏 `layout {}`（`configs/09-output.lua` / `20-layout.lua`）**（用户：「调研 top5 ④」；2026-10-14）

     - 顺手改的键位选项名：`["repeat"]` → **`repeatable`**（`repeat` 是 Lua 保留字，写起来只能 `["repeat"]`）。主名
       `repeatable`（bool，默认 `true` = 改前行为），**老写法 `["repeat"]` 继续认**（别名；两个都写以 `repeatable` 为准），
       README 262 里已经写过的那种用户配置一个字不用改。`vidowmsg keybinds` 现在报 `repeatable` + `repeat`（同一个值，
       老脚本不改）+ `cooldown_ms`。落点：`src/script.c`（`l_bind` 的选项解析）、`src/keybind.c` /
       `src/keybind.h`（字段改名）、`src/ipcstate.c`（多报一个字段名）。**`src/config.c` 没动** —— 绑定选项的说明本来就不在
       默认配置里（14-keys.lua 只讲规则字段）。
     - **每窗口按需 VRR**（niri 的窗口规则 `variable-refresh-rate on-demand=true` + 规则 `variable-refresh-rate true`）：
       ```lua
       vrr_on_demand = true                                     -- 全局：所有屏都按需（默认 false = 不碰 VRR）
       vidow.output{ name = "eDP-1", vrr_on_demand = true }     -- 每屏覆盖（不写继承全局）
       vidow.rule{ app_id = "^gamescope$", vrr = true }         -- 这个窗口要 VRR
       ```
       语义：这块屏是「按需」模式时，屏上**显示着**要 VRR 的窗口（属于该屏当前亮着的工作区、没被最小化）→ 把 adaptive
       sync 提上去；那种窗口一个都不显示了（切工作区 / 关窗 / 最小化 / 挪走）→ 降回来。**静态 `vrr = true/false` 优先**
       （那种屏整块恒定，不按需）；两边都没写 = 一个 VRR 字段都不碰，和改前一样。实现：`src/output.c` 的
       `vidow_output_vrr_on_demand_update()`，调用点 = `output_arrange` 末尾（开窗 / 关窗 / 切工作区 / 换屏都会 arrange）
       + 规则重放时（`src/rule.c` 的 `apply_view`）+ 配置改了（`vidow_output_apply_state_all`）；先比一遍现状再 commit，
       不会每帧提交。**没加任何 fullscreen 特例分支** —— 判据就是「显示着」= 工作区 + `!minimized`。
     - **每屏 `layout {}`**（niri 25.11 的 `output "DP-3" { layout { … } }`）：一块屏覆盖全局 layout 参数，
       **没写的继承全局**（不是继承内置默认 —— 全局改了它跟着变）：
       ```lua
       vidow.output{ name = "DP-3", layout = { gaps = 20, center_focused_column = "always" } }
       output_layout = "DP-3: gaps=20; center_focused_column=always | DP-2: gaps=8"   -- 平铺键写法
       ```
       能覆盖的就是 20-layout.lua 那几个：`gaps` / `gaps_top` / `gaps_bottom` / `gaps_left` / `gaps_right` / `gaps_inner` /
       `center_focused_column` / `preset_column_widths` / `default_column_width`（最多 16 块屏）。两条写路径共用一套解析
       （`vidow_output_layout_apply_text`，值走全局键那套校验，报错话术一字不差）：Lua 表 → 翻成文本 → 解析；文本直接解析。
       生效点全在 `vidow_layout_for(server, 输出, &lay)`：`vidow_work_area`（四边 gap）、`output_arrange`（列宽 / 内间距）、
       `focus_viewport_target`（居中策略）、`view_ensure_column`（新列默认宽）、`vidow_cycle_column_width`（预设档位）、
       `vidow_workspace_preview_layout`。默认一条覆盖都没有 → 每个键都走全局那份，**不配置零变化**。
     - 三条写路径都吃（照 508239e）：config.lua / `VIDOW_OUTPUT_LAYOUT="…"`、`VIDOW_VRR_ON_DEMAND=true` /
       `vidowmsg set-config output_layout "HEADLESS-1: gaps=12"`；查询 `vidowmsg config output_layout` / `vrr_on_demand`
       （写进去当场规范化，读回来是同一个写法）。写错当场 `WLR_ERROR` + 该行不生效（没有冒号 / 项名不认识 / 一块屏写两遍 /
       值不合法都拒，旧值不动）。`vidowmsg outputs` 多两个字段：`vrr_on_demand`（这块屏是不是按需模式）、
       `vrr_on_demand_want`（上一次算出来要不要提刷新率）。
     - 验证（headless 两块屏 `WLR_HEADLESS_OUTPUTS=2`，脚本 `/tmp/vd/t263.sh`、`/tmp/vd/t263b.sh`）：
       - **不配 `output_layout`**：两块屏逐字相同 —— 都是 `gaps 6`、窗口 `631x708`、焦点列 `x=6`（屏内）/`643`，`center_focused_column`
         都是全局的 `never`。
       - **只给 `HEADLESS-1` 配** `gaps=40; center_focused_column=always; preset_column_widths=0.25,0.5,0.75; default_column_width=0.8`：
         HEADLESS-1 的窗口 `y=40`、宽 `580`（gap 40 的工作区），HEADLESS-2 一个数字都没变（`6` / `631` / `643`）—— 继承全局。
       - 居中策略对比（两块屏同样 3 条列，只有 HEADLESS-1 是 `always`）：HEADLESS-1 焦点列（中间那条，宽 580）屏内 `x=350`
         → **中心 640 = 屏中心**；HEADLESS-2（`never`，继承全局）焦点列屏内 `x=643` → 中心 958.5（贴右边，不居中）。
       - 列宽预设按屏不同：HEADLESS-1（`0.25/0.5/0.75`）`cycle-column-width` 一次 → 宽 `952`；HEADLESS-2（继承全局 `1/3,1/2,2/3`）
         同样一次 → 宽 `418`。
       - `vidow.output{ layout = { gaps=20, center_focused_column="always", default_column_width=0.25 } }` 表写法：
         `vidowmsg config output_layout` 查回 `HEADLESS-2: gaps=20; center_focused_column=always; default_column_width=0.25`；
         HEADLESS-2 的窗口 `20,20 610x680`，HEADLESS-1 仍继承全局（`x=6`）。`set-config output_layout "HEADLESS-1: gaps=12"`
         → 查回同一串、窗口立刻变 `12,12`；写错（`HEADLESS-9: nope=1`）→ `没有 'nope' 这一项` + 查回**不变**。
       - 键位改名：`{ repeatable = false, cooldown_ms = 200 }` / `{ ["repeat"] = false }` 两条都生效（`keybinds` 里
         `repeatable=false repeat=false cooldown_ms=200` / `repeatable=false repeat=false`）；`{ repeatable = 1 }` 当场报错
         `vidow.bind 的 repeatable（老名字 repeat，得写 ["repeat"]）只能是 true / false`；两个都写以 `repeatable` 为准。
       - 默认值：什么都不配时 `vidowmsg config` 里 `output_layout` = `""`、`vrr_on_demand` = `false`，与改前逐字相同。
     - **VRR 在 headless 里验不到端到端**（后端不吃 adaptive sync），所以只能给到这三层：① 能力探测 `vidowmsg outputs` 报
       `vrr_supported=false`（真机 DRM 看连接器的 `vrr_capable`）；② 状态机真的动了 —— 给 HEADLESS-1 配
       `vrr_on_demand=true` + `vidow.rule{ title="^w1$", vrr=true }` 后，`vrr_on_demand=true`、`vrr_on_demand_want=true`
       （HEADLESS-2 两个都是 `false`）；③ 日志逐条：`rule: w1 要 VRR 是` → `output HEADLESS-1: 按需 VRR -> 提上去` →
       `这块屏 / 这个后端不支持 VRR（adaptive sync），开不生效`（`adaptive_sync` 因此仍是 `false`）。真机上提上去那一步走的是
       原来的 `vidow_output_set_vrr()`，没改。
     - 回归：`test-config.sh` / `test-defaultconfig.sh` 改前改后都全过（默认配置与 `--default-config` 逐字节一致，新键都在）。
     - 边界（没做 / 已知）：① `src/drag.c` 的拖拽落点预览和平铺改宽起拖宽仍读全局 `gaps_inner` / 列宽换算 —— 它不在这次
       的可碰清单里，屏上覆盖了 gap 时那块预览会差一个 gap；② 新列的 `default_column_width` 按「**建列那一刻**窗口所在的
       屏」取（新窗口先落在聚焦屏、再被规则搬到目标屏），所以被规则搬到另一块屏的窗口，列宽还是按原屏那份算；
       ③ 没做 niri 的 `output { layout { struts } }`、每屏 `focus-at-startup`、`variable-refresh-rate` 的「全屏才开」变体。

262. **光标隐藏 / 键盘参数 / 输入唤醒 / 键位限制这一批小开关（`configs/21-keyboard.lua`、`22-workspace.lua`）**（用户：「把调研第 5 节剩下的 S 成本项打包做完」；2026-10-14）

     - 新增 9 个配置键 + 2 个键位选项 + 1 个动作，**默认值全部 = vidow 改前的行为**（不配置零变化）：
       ```lua
       hide_when_typing = false                  -- 打字时藏光标（niri 的 cursor { hide-when-typing }）
       hide_after_inactive_ms = 0                -- 指针静止多久藏（0 = 不藏；niri 的 hide-after-inactive-ms）
       repeat_delay = 600                        -- 改前 input.c 里写死的 600
       repeat_rate  = 25                         -- 改前 input.c 里写死的 25
       numlock = false                           -- 起手点亮 NumLock（niri 默认 true，vidow 改前不碰）
       mod_key = "super"                         -- 键位里的 `Mod` 是哪个键（niri 的 mod-key）：super / alt
       mouse_move_enables_dpms = false           -- 指针一动唤醒关掉的屏（Hyprland 的 misc{}）
       key_press_enables_dpms  = false           -- 按一下键唤醒关掉的屏
       workspace_auto_back_and_forth = false     -- 切到「当前就在的」工作区改成切回上一个（niri 同名键）
       vidow.bind("Mod+Q", "close-window", { repeatable = false, cooldown_ms = 200 })
       vidow.bind("Mod+R", "toggle-touchpad")    -- 动作：触控板整体关 / 开
       ```
     - 三条写路径都吃（照「可配置缓动」508239e）：config.lua / `VIDOW_NUMLOCK=true`、`VIDOW_MOD_KEY=alt`、
       `VIDOW_HIDE_AFTER_INACTIVE_MS=1200` / `vidowmsg set-config <键> <值>`；查询 `vidowmsg config <键>`。
       写错当场 `WLR_ERROR` + 该行不生效（`mod_key = "ctrl"` 拒，旧值不动）；`mod_key` 归一化成 `super` / `alt` 两个词。
     - 光标隐藏（`cursorfx.c` 的 `vidow_cursor_fx_set_hidden()`，调用点在 `input.c`）：藏 = 把 wlr_cursor 的图设成 NULL；
       显示 = 把「这期间该显示的那张」重新设上（客户端 `set_cursor` 给的那张，或者主题箭头）—— 客户端在藏着的时候
       `set_cursor` 只记账不上图。指针一动就显示回来并重新计时；`hide_after_inactive_ms` 用 `wl_event_loop` 定时器。
       **这条改动了 `src/cursorfx.c`（不在原定可碰清单里）**：只有它知道「当前该显示哪张光标图」。
     - 键盘：`repeat_delay` / `repeat_rate` 走 `wlr_keyboard_set_repeat_info()`（就是原来那两个写死的数，现在能配），
       插键盘和配置改了都重套；`numlock` 用 xkb 的 `XKB_MOD_NAME_NUM` 锁定位 + `wlr_seat_keyboard_notify_modifiers()`
       （wlroots 0.20 没有 keyboard 级的 notify），只对 seat 正拿着的那把键盘发。**注意：自动重复是客户端自己做的**
       （wlroots 0.20 不在合成器里重复，`repeat_info` 只是把速率报给客户端），所以 `repeatable = false` / `cooldown_ms`
       这两个绑定选项今天基本看不到效果 —— 它们按「同一个 keysym 没松开又来一次按下 = 重复」实现（`held_syms[]`），
       留着给虚拟键盘 / 输入法回注 / 以后 wlroots 自己重复。绑定选项写在 `vidow.bind()` 第三个参数里，
       **选项名 2026-10-14 改成了 `repeatable`**（`repeat` 是 Lua 保留字，只能写成 `["repeat"]`，见 263）：
       `["repeat"]` 作为别名继续认；`vidowmsg keybinds` 的 `repeatable` / `repeat` / `cooldown_ms` 三个字段都报出来。
     - 输入唤醒关掉的屏（`output.c` 的 `vidow_output_wake_disabled()`）：vidow 没有单独的 DPMS 状态，
       「关掉的输出」本身就是那个状态，所以唤醒 = 把所有关着的输出打开（笔记本合盖 / `output-power off` 都算）。
       `input.c` 里指针一动 / 按键一下各看一眼开关；注入的键也算「人按了键」（headless 只有这条路能验）。
     - `workspace_auto_back_and_forth` 落在 `keybind.c` 的动作那条路（`workspace <序号>` / `workspace <名字>`）：
       要切过去的就是现在这块 → 改成切回 `server->last_workspace`；记账只记**动作切过去的**那一次（IPC / 钩子切工作区
       不污染「上一个」）。`toggle-touchpad` 动作只认 libinput 的触控板（有 tap 能力的设备），全开着就全关、全关着就全开。
     - 验证（headless 1280x720 一块屏，脚本 `/tmp/vidow-check262b.sh`、`/tmp/t4.sh`、`/tmp/t5.sh`、`/tmp/t6.sh`）：
       - 9 个键不配置时 `vidowmsg config` 全是上面那组默认值；配置里写了就原样查得回来；`VIDOW_*` / `set-config` 同样。
       - 光标：`hide_after_inactive_ms = 400` 静止 0.9s → 日志 `光标: 藏起来`；`move-cursor` → `光标: 显示回来`；
         `hide_when_typing = true` + 注入一个按键 → `光标: 藏起来`。
       - DPMS：`output-power headless-1 off` + `move-cursor` → `输入唤醒: 打开 1 块关着的屏`；再关屏 + 按键 → 第二次唤醒；
         两个开关 `set-config` 成 false 后同样操作**一次都不唤醒**，`state` 里那块屏仍是 `enabled: false`。
       - `mod_key = "alt"` → `vidowmsg keybinds` 里 `Mod+Q` 的 `mods` 从 64（LOGO）变成 8（ALT）；`repeatable=false` /
         `cooldown_ms=250` 两个字段也报出来。
       - 工作区往返（两块具名工作区 chat / mail）：`workspace mail` → mail；再按一次 → 日志
         `workspace: 就在这块上，回到上一块（chat）` 且真的回到 chat；`workspace chat` 再按一次同样回到 mail。
       - `toggle-touchpad`：headless 没有触控板 → `动作 'toggle-touchpad' 没做成` + 日志说清原因，**不崩**（真机上的
         「关掉 / 打开」这条路没验，如实说）。
     - 回归：`test-config.sh` 全过、`test-defaultconfig.sh` 全过（123 个平铺键都在，`21-keyboard.lua` /
       `22-workspace.lua` 与 `--default-config` 逐字节一致）。
     - 未做：**每屏 `focus_at_startup`**（要动 `outputconf.c` 的每屏配置 + 输出创建时的聚焦顺序，时间盒到点了，
       连配置面都没铺）；niri 的 `mod-key-nested`（嵌套里换另一个 Mod）；真机上的触控板开关 / NumLock LED。
     - 改了：`src/config.c`（2 个分节 + 9 个键 + 校验）、`src/server.h`（字段 + 声明）、`src/input.c`（键盘参数 /
       numlock / 光标隐藏定时器 / DPMS 唤醒 / toggle-touchpad）、`src/output.c`（`vidow_output_wake_disabled`）、
       `src/keybind.c` + `src/keybind.h`（repeatable / cooldown_ms / mod_key / 两个动作）、`src/script.c`（绑定选项解析）、
       `src/ipcstate.c`（keybinds 报 repeatable / repeat / cooldown_ms）、`src/cursorfx.c`（藏 / 显示）、
       `config.example/`（重新生成，新增两份分类文件）。

260. **libinput 设备配置族：触控板 / 鼠标那十一个设置终于能配了（`configs/19-input.lua`）**（用户：「照 niri 的 `input {}` 那套，给 vidow 加 libinput 设备配置族」；2026-10-14）

     - 改前：vidow **一个 libinput 设置都不能调**（全仓 grep `accel_profile` / `scroll_method` / `natural_scroll` / `tap_to_click` = 0 命中）——
       笔记本用户第一件事（开点按、打字时关触控板、反向滚动）在 vidow 上无解。
     - 新增 11 个键（键名照 niri 的 `input { touchpad { … } }`，转成 vidow 的 snake_case）：`tap` / `dwt` /
       `natural_scroll` / `left_handed` / `middle_emulation` / `drag_lock` / `accel_profile` / `accel_speed` /
       `scroll_method` / `click_method` / `scroll_factor`。
     - **默认值 = vidow 以前的行为**：前十个默认是 `"auto"` / 空串 = **一个 libinput 设置都不碰**，用设备自己的默认 ——
       libinput 的默认值按设备 / udev 规则走（同一台机器上触控板和鼠标的 tap 默认就不是一回事），照抄一个常数等于替用户
       改了他没配的东西，所以「零变化」只能靠「不设置」本身（`enum vidow_tristate` + `struct vidow_opt_double`，
       config.c 新增 `K_TRISTATE` / `K_OPT_DOUBLE` 两种类型）。`scroll_factor` 是 vidow 自己乘的倍率
       （libinput 没有这个接口），默认 1.0 = 不缩放。
     - 三条写路径都吃：config.lua（`touchpad = { tap = true, … }`，新增的族 —— vidow 的族只有一层，所以省掉 niri 外面的
       `input`）/ `VIDOW_TAP` 这类环境变量 / `vidowmsg set-config tap true`；查询 `vidowmsg config tap`
       （打出来能原样抄回配置）。写错当场报错、该键不生效（`tap = "maybe"`、`accel_speed = 5`、
       `scroll_method = "two-fingered"` 都拒，旧值不被改坏）。
     - **设备插入时也套**（不只是启动时）：`input.c` 的 `server_new_input`（热插拔走同一条路）调
       `vidow_input_apply_libinput()`；设备手柄走 wlroots 0.20 的 `wlr_input_device_is_libinput()` +
       `wlr_libinput_get_device_handle()`（`<wlr/backend/libinput.h>`），虚拟键盘 / 虚拟指针不是 libinput 的，先滤掉
       （wlroots 那个取手柄的函数对它们会 assert）。设备不支持某一项就跳过并在日志里说清楚（比如 `scroll_method` 要过
       设备支持的方法位掩码）。
     - 配置改了要重套（设置是存在**设备**上的，重载不会自己跟过去）：config.c 新增 `vidow_config_observe()`，挂在
       `apply()` 上 —— 三条写路径最后都过那儿，一处盖全；`key == NULL` = 配置重载先恢复默认值那一步（不走单个键），
       所以配置里**删掉**一行也能回到默认。
     - `scroll_factor` 乘在 `vidow_pointer_axis()` 的入口（真设备和 IPC 注入本来就走这一条路，倍率在这儿一处管全）；
       离散值（老鼠标一格一格那种）一起缩，缩到 0 至少留一格。
     - 日志：配置那一行 `config: 输入设备 tap=auto … scroll_factor=1.00（auto / 空 = 不碰设备…）`；真设了东西的设备打一行
       `input: <设备名>：tap=on …`（默认全是「不碰」，所以不打一串 auto）。
     - 验证（headless，脚本 `/tmp/vidow-input-verify.sh`）：11 个键的默认值逐个查回 = 现状（`auto` / 空 / `1`）；三条写路径
       各验一遍（配置文件 + 表写法 + 自动重载、`VIDOW_TAP=true`、`set-config tap true`）；四种写错的取值都被拒且旧值没被
       改坏；配置里删掉那一行回到 `auto`。`./test-config.sh` 与 `./test-defaultconfig.sh` 改后全过（后者含 config.example
       与 `--default-config` 逐字节一致，新增的 `19-input.lua` 也在内）。
       **headless 下没有真 libinput 设备**（`WLR_LIBINPUT_NO_DEVICES=1`，headless 后端本来也不建 libinput），所以
       「把设置套到真设备上」那一段只有读代码 + 日志行做论证，**没有端到端实测**（真机插拔 + `libinput debug-events`
       看 tap 是否真生效是没做的那一步）。
     - 未做：niri 同一节里剩下的 `dwtp`（disable-while-trackpointing）、`scroll_button` / `scroll-button-lock`、
       `tap-button-map`、`pinch-sensitivity`、`disabled-on-external-mouse`、`off`（整台设备不发事件）；每台设备单独配
       （niri 也没有）；`tablet` / `touch` 那两节；`scroll-factor` 的横向 / 纵向分开给（niri 25.08 才有）。
     - 依赖：`meson.build` 加了一份 `libinput_dep`（wlroots 把它放在 `Requires.private` 里，链接时不会自动带过来）。

259. **overview 每个工作区画成「缩小版的屏」：矩形按屏幕长宽比 + 里面画该工作区实际的背景图**（用户：「overview 工作区矩形应该是缩小版的屏幕矩形而不是一整条，工作区矩形填充的是实际的工作区背景图」；2026-10-14）

     - 机制（改前）：一行就是**铺满工作区宽的一条横带** —— `plate_apply()` 把板子铺成 `out_w` 宽、`row_h` 高，里面只有一块压暗的板子，没有背景图；缩略比例横向还按「最宽的条带铺得进一行」算
       （`fit_w = 0.92 × 工作区宽 / 最宽条带`），那个式子属于「整条带铺满一行」的旧模型（README 235）。
     - 修法（都在 `src/overview.c`）：矩形高仍是行高（固定 `OV_ROW_FIXED_SCALE` 0.46，README 245），
       **宽 = 高 × 该工作区此刻所在那块输出的逻辑长宽比**（新增 `row_rect_w()`：取 `ws->output`，没归属就退回 overview 锁的那块屏 = 进全景时聚焦的那块），横向居中 —— 一行就是一块缩小 0.46 的屏。
     - 背景图从哪来：vidow 不画壁纸（壁纸归 background 层客户端），合成器手上没有单独的壁纸贴图；用的是 fx.c 抓的那张**底衬** `output->backdrop`
       （把窗口全藏起来渲染的一帧 = 壁纸 + background / bottom 层，跟窗口模糊底衬同一张），新增 `row_bg_apply()` 拿一块 `wlr_scene_buffer` 按 `dest_size` 缩进矩形里。
       它画在板子**上面、遮罩上面**（矩形就是缩小版的屏，得跟屏幕上一样亮），跟着 `t` 淡入淡出；`point_accepts_input` 返回 false，不然这块 buffer 会把点击全吃掉。
       模糊那份 `blur_tex` 不能用 —— 那是整屏垫在遮罩下面的糊壁纸。
     - 行内横向滚动跟着改了：`content_x = rect_x - 视口 × scale`（视口 0 = 条带左边缘贴着矩形左边，也就是「屏幕上看到的那一屏」）；原来那套
       `slack = 工作区宽 - 条带宽 × scale` 的居中夹取是「条带铺满一行」的算法，配矩形就对不上了。横向 `fit_w` 一并去掉：条带比屏宽的那些列本来就该落在矩形（= 屏）外面。
     - 踩到的坑：`wlr_scene_buffer_set_buffer()` 会**锁住**那张缓冲 —— 退出全景时得显式 `set_buffer(NULL)`（`overview_done()` 里做），
       不然一张整屏大的底衬（4K 上 33MB）被我们锁着，fx.c 换了新的一代也放不掉。
     - 验证：`./test-overview.sh` 改前 **62 PASS / 0 FAIL** → 改后 **62 PASS / 0 FAIL**（逐条比对只有两处数值动了：点选目标的 x 55 → 57，
       遮罩那几条的均值 42% → 43% —— 矩形里那块背景图是原亮的壁纸，采样点里多了一小块没压暗的）。headless 3 块工作区（2/3/2 个窗口、列宽 0.25，`/tmp/ov-shot.sh`）截图：
       改前 `/tmp/ov-before.png`（三条铺满宽的暗带，背景是糊过 + 压暗的壁纸，看不出「一块屏」）、改后 `/tmp/ov-after.png`（三块 **579x326** 的矩形居中排开 —— 0.46 × 1268x708、
       长宽比 1.78 就是屏幕的比例，矩形里是那块屏自己的壁纸：金色方块清晰、按 0.46 缩过，矩形外仍是糊 + 压暗的那张，日志 `overview: 工作区 w2 的矩形 350,197 579x326（HEADLESS-1 的屏，背景图 有）`）。
     - 还没做：**矩形外的列不裁**（wlroots 的场景图没有裁剪节点）—— 条带比屏宽的工作区，屏外那几列会画到矩形外面（屏幕上它们本来在屏外，观感上是「窗口飘在矩形边上」）；
       没开 `blur_enabled` 就没有那张底衬，矩形里退回只有板子（没有第二套「只抓壁纸」的路径）。

258. **kitty + 虚拟键盘下 IM 层自激：10 秒刷 106 万行日志（`text input enabled/disabled` + wlroots 的 `Text input commit received without focus`）**（剪贴板测试顺带撞到，与剪贴板无关；2026-10-14；复现脚本 `/tmp/imfix-final.sh`，触发窗用 `geowin trigger 1 1 1 1 --linger=0.2`）

     - 现象：headless vidow + kitty，只要有**另一扇窗 map 完立刻消失**（哪怕 1x1、200ms 就没），
       日志当场进死循环：10 秒窗口 **1,061,522 行** —— `im.c:151 text input enabled`、
       `im.c:163 text input disabled`、`im.c:440 im focus ->`、wlroots `types/wlr_text_input_v3.c:190
       Text input commit received without focus` 各 **266,468** 次（4 行一轮，约 10.6 万轮/秒，CPU 空转）。
     - 链路（日志 + wlroots 源码坐实的，不是猜）：
       ① 焦点变化（触发窗 map / 关）让 `vidow_im_focus()` 给 kitty 发 `leave`、紧接着又发 `enter`
          （`src/view.c:2983` → `src/im.c:439`）；
       ② kitty 对 `leave` 回 `disable()` + `commit()`、对 `enter` 回 `enable()` + `commit()`
          （`WAYLAND_DEBUG=1` 实录：`<- leave` / `-> disable()` / `-> commit()` 一路交替）；
       ③ wlroots 的 commit 处理（`types/wlr_text_input_v3.c:174-196`）拿这次 `pending_enabled` 和
          上一次 `current_enabled` 比跳变：客户端的 enable/disable 是**交替**的，于是**每一次 commit
          都算一次跳变**，wlroots 就发 `events.enable` / `events.disable`；
       ④ vidow 的 `text_input_enable_event` / `text_input_disable_event` 接住 → `text_input_enable()`
          （`src/im.c:143`）里补的那次 `vidow_im_focus()` 又给客户端发 `enter`、
          `text_input_disable()`（`src/im.c:154`）里给客户端发 `leave`；
       ⑤ 回到 ②：客户端又回 enable/disable + commit …… ②③④ 互为因果，转起来就停不下。
     - 根因：**`enter`/`leave` 是「表面的文本输入焦点」事件，只能跟着 seat 的键盘焦点走；
       客户端自己说 `enable`/`disable` 并不代表它的表面丢了焦点。** `text_input_disable()` 里发
       `leave` 违反了这条：客户端收到 `leave` 必然回 `disable` + `commit`，那个 commit 又被 wlroots
       当成一次跳变 → 我们又 `disable` → 又发 `leave`。`text_input_enable()` 里补的那次
       `vidow_im_focus()` 本身没错（「客户端在窗口已经有焦点之后才 enable」这条得靠它补 `enter`），
       但上面那次 `leave` 把 `focused_surface` 清成了 NULL，于是它每次都真的再发一次 `enter`，
       正好把环闭上。
     - 修法（`src/im.c` 的 `text_input_disable()`，**删掉发 leave 那三行**）：`enter`/`leave` 只由
       `vidow_im_focus()`（seat 焦点变化那几条路）发，`disable` 只改自己的 `ti->enabled` /
       `im_set_active`。`focused_surface` 于是只被真正的焦点变化清掉，`text_input_enable()` 里那次
       `vidow_im_focus()` 变成幂等（同一个 surface 直接 `continue`），环断开。没碰 `src/view.c` 的
       焦点逻辑：`2178894`（全屏窗口状态变化不许抢焦点）和 `e6e2ea5`（焦点跟随光标）原样，
       也没加任何 fullscreen 特例（列就是列）。
     - 量到的（同一条复现，10 秒窗口，`VIDOW_LOG=debug`）：

       | | vidow 日志行数 | enabled | disabled | without focus | im focus | kitty 侧 enable/disable/commit |
       |---|---|---|---|---|---|---|
       | 改前 | **1,061,522** | 266,468 | 266,468 | 266,468 | 266,468 | 286,869 / 291,827 / 593,919 |
       | 改后 | **1** | 0 | 0 | 0 | 0 | 3 / 3 / 7 |

       改后 kitty 那 3 次 enable 就是三次真实焦点变化（kitty 上屏、触发窗 map、触发窗消失）；
       vidow 那 1 行是无关的渲染日志。
     - 功能没修死：同一个 headless 里 `vidowmsg focus-column left` 把焦点落回 kitty（`vidowmsg focused`
       报 `"title": "wlterm"`），`wtype "hello"` + `wtype -k Return` 敲进去，kitty 里跑的
       `cat > typed.txt` 收到 `hello`（改后 OK）。
     - 回归：`test-focus.sh` / `test-input-inject.sh` / `test-ipc.sh` 改前改后各一次，全绿。
     - 未覆盖：只有「客户端收到 enter/leave 会回 enable/disable + commit」这一类客户端（kitty、GTK 系）
       才会自激；协议上不这么回的客户端本来就不触发，改法对它们零影响。


247. **缓动曲线能自己写了：自定义三次贝塞尔 + 回弹幅度可调**（用户：「回弹幅度应该允许配置 回弹的时间也允许配置 类似 hyprland 那样」）

     - 时间本来就能配（`window_move_ms` / `workspace_switch_ms` / `column_width_ms`），缺的是
       **幅度**：`out_back` 的过冲常数写死在 `src/anim.c` 的 `ease()` 里（`c1 = 1.70158`），
       用户改不了。现在曲线是配置里写的数据，不再是一个枚举。
     - 三种写法，老那六个名字（`linear` / `out_cubic` / `out_quint` / `in_out_cubic` /
       `out_back` / `out_elastic`）一个字没变，默认值也没变：

       ```lua
       easing = "out_back"                     -- 老写法：回弹幅度跟全局走（默认 0.10 = 10%）
       easing = "out_back 0.25"                -- 这条曲线冲过终点 25%
       easing = "bezier 0.05 0.9 0.1 1.08"    -- 自定义三次贝塞尔：x1 y1 x2 y2
       back_overshoot = 0.10                   -- 全局回弹幅度：三条曲线里没单独写幅度的跟它
       ```
       `bezier` 的 x 必须在 0..1（x 管进度，要单调）、y 在 -1..3，**y > 1 的那部分就是过冲
       幅度**（1.08 ≈ 冲过 1.7%，1.5 ≈ 冲过 18%）；`back_overshoot` 和曲线里的幅度取 0..1。
       超出范围、少给参数、名字拼错都当场报错并说清楚错在哪，**不静默退回默认曲线**。
     - 贝塞尔是**按 x 求 y**（先解 x(t) = x 再取 y(t)），不是拿进度当参数 t 直接取 y ——
       后者等于「进度按 t 均匀」，曲线形状完全不对（该冲的地方不冲）。x(t) 单调，二分 32 次
       就够（比要求的 1e-4 精度高几个数量级），不引牛顿迭代：x'(t) 在 x1 = 1 / x2 = 0 这类
       边上会退化。`out_back` 的幅度按峰值 `s = 4c1³/(27(c1+1)²)` 反解 c1，配置解析时算一次
       （默认 0.10 反解回 1.701540，和原来写死的 1.70158 差 4e-5 → 曲线差 ~4e-7，看不出来）。
     - 量到的过冲（headless 1280x720，`column_width_ms = 600` 走 `cycle-column-width`，
       每 4ms 采一次 `vidowmsg columns` 的 width）：`linear` 0.0%、`out_back`（默认 0.10）
       10.0%、`out_back 0.30` 30.3%、`back_overshoot = 0.50` 50.2%、`bezier 0.05 0.9 0.1 1.08`
       1.9%、`bezier 0.05 0.9 0.1 1.50` 18.5% —— 跟按曲线算出来的 1.71% / 18.12% 对得上
       （差值来自 4ms 采样和像素取整）。
     - `vidowmsg config easing` 打回来的就是能直接抄回配置的写法（`"bezier 0.05 0.9 0.1 1.08"`、
       `"out_back 0.2"`），往返一致；`vidowmsg anims` 每条动画多一个 `curve` 字段（曲线名连参数）。
     - 回归：`test-config.sh`（新增第 9 节：默认值不变 / 三种写法 / 8 种写错和越界都当场报错
       且值不变）、`test-defaultconfig.sh`（config.example 逐字节一致、96 个键和内置默认值
       一样）、`test-widthanim.sh` / `test-openanim.sh` / `test-ipc.sh` 全过。
       `test-closeanim.sh` 有一项「位移补间 30% 处它还在路上」是**改之前就红**的（拿 HEAD
       干净副本编出来跑，数值一模一样：754 vs 判据的 <500），跟这次改动无关。

246. **预览（overview / MRU）里不叠那层模糊 —— 模糊只画在屏幕上那扇窗口上**（用户：「overview
      和 mru 的预览不应该导致窗口内容变模糊」；期望是「预览里画的是窗口的**真实缓冲**，
      即使窗口按配置开了模糊，预览里也**不要再叠那层模糊**」）

     - 先量后改：现在这棵树里预览那两趟**手里根本没有模糊贴图** —— overview 有
       `view_wants_blur_ever()` 里 `vidow_overview_active()` 那道闸（早先为「窗口缩小之后按窗口
       位置算的 UV 对不上」加的），MRU 卡片那个 `fx` 谁也没给它塞过贴图。所以默认态改前 / 改后
       **逐像素相同**（headless 1280x720，overview 与 MRU 两张截图都是 0 px 差异）——
       「预览不叠模糊」只是被上游那两处**顺带**保证着，**不在预览渲染的地方**。
     - 但装饰着色器是照「有贴图 + `fx->blur`」决定 `u_blur_strength` 的：只要预览那一趟手里
       有贴图（README 216 当年写的「预览里模糊也一样」、上一帧留下的贴图、以后别处给预览塞
       贴图），它就会按**屏幕坐标**把糊过的底衬采出来垫在内容底下 —— 内容半透明的窗口
       （终端 `background_opacity 0.8` 那种）底下那层透出来，看着正是「预览里窗口内容糊了」。
     - 修法（`src/fx.c`，两处）：`vidow_fx_frame()` 里按 `ov` 判「这是不是预览那一趟」——
       `ov` 就是这块缩略图相对原尺寸的缩放（overview 传格子缩放、MRU 卡片传 `zoom`、桌面那条路
       恒等于 1）—— 是就把**这一次渲染**的 `look.blur` 关掉；`decor_draw()` 的 `u_blur_strength`
       多认一个 `look->blur`。**不动 `fx->blur`**：那个标志是 `vidow_fx_begin()` 按配置设的、
       `vidow_blur_update()` 也要读它，改了退出预览就再也恢复不了（README 1723 那条坑）。
       阈值取 0.98 而不是「< 1」：进出场动画头尾那几帧 `ov` 是 0.999 / 0.998，那时候窗口还是
       原尺寸、模糊是对的。
     - 证据（headless 1280x720，两块半透明 kitty，`blur_all` 开着）：用探针把模糊底衬塞给预览
       那一趟（闸门可用环境变量绕过）—— overview 窗口内容区 **51210 px** 变了（该处均值
       `(17.5,18.4,16.7)` → `(9.1,9.2,8.9)`：有贴图时窗口底子是糊过的壁纸金色），MRU 卡片
       **15224 px** 变了（`(36.8,33.3,15.1)` → `(11.1,12.8,14.3)`）；同一组探针下**桌面那张
       0 px 变化**（屏幕上的模糊一点没动）。
     - 回归：`test-overview.sh` 62 项、`test-scrollcull.sh` 34 项，改前 / 改后都全过（overview
       那份逐字节相同，scrollcull 只有时间戳和帧数 ±1 的抖动）。
     - 顺带把 README 216 里「圆角、边框、阴影、模糊…都一样」那句改掉：预览**不叠**模糊，其余
       （圆角 / 边框 / 阴影 / 焦点描边）照旧一样。

244. **挂在 layer surface 上的弹出物再弹二级（状态栏菜单的子菜单）摆到屏幕左上角**（用户报的这条；README 238 当时记的「还没修」）

     - 现场（headless 1920x1080，新靶子 `tools/menuprobe-layer.py`：gtk-layer-shell 的状态栏
       + 8 项一级菜单 + 第一项带 2 项子菜单）：一级菜单 275x268 在 **301,17**（对），二级
       114x82 在 **0,0**（屏幕左上角）—— 正确位置是 562,17（= 一级菜单右边缘 576 减 GTK 子菜单
       跟父菜单的 14px 重叠），错 (562, 17) px。
     - 根因**两条**，都在「wlroots 怎么把 layer 弹出物交给我们」这件事上（`types/xdg_shell/
       wlr_xdg_popup.c` + `types/wlr_layer_shell_v1.c`）：
       1. **layer 弹出物是两步建的**：客户端先 `xdg_wm_base.get_xdg_popup(parent=NULL)` —— 这时
          wlroots 只发 `xdg_shell` 那条 `new_popup`，**`popup->parent` 还是 NULL**；随后
          `zwlr_layer_surface_v1.get_popup` 才把父面设成那块 layer 表面，并且**只**发 layer 那条
          `new_popup`。所以一级弹出物归 `layer.c` 的 `layer_surface_new_popup()` 管，而**二级弹出
          物（父面是一级弹出物）只有 `xdg_shell` 那条信号** → 归 `view.c` 的 `server_new_popup()`
          管。238 猜的「`layer_surface_new_popup()` 没进登记表」方向对了一半：**它根本收不到二级
          弹出物的信号**，二级的节点被 `server_new_popup()` 挂在 fallback `server->layer_toplevels`
          上（`view_from_surface()` 找不到窗口），于是「相对一级弹出物」的坐标被当成「相对屏幕
          原点」，整段丢掉一级菜单的位置。
       2. **可用区（unconstrain）算成 0x0**：`server_new_popup()` 只在「父面是窗口」时算可用区盒，
          二级弹出物 `view == NULL` → box 是 `{0,0,0,0}`。wlroots 的
          `wlr_xdg_popup_unconstrain_from_box()` 会把它沿父链换算成「相对父弹出物」的约束盒
          （`0−301, 0−17, 0x0`），positioner 于是把子菜单摆到那个点上 —— 节点最终落在 0,0，
          就是「子菜单跑到左上角」。**这条是量出来的**：修了第 1 条、没修第 2 条时子菜单仍在 0,0。
     - 修法（两处，都不推翻 7768fe7 的思路）：
       - 建树的规则收进一个函数 `vidow_popup_scene_create(server, fallback, popup)`：父面是弹出物
         就按 `popup_scenes` 登记表挂到**父弹出物**的树上，否则挂 `fallback`；`view.c`（窗口的弹出
         物，fallback = 窗口的树）和 `layer.c`（layer 的弹出物，fallback = 那块 layer 的树）都调
         它 —— 一条规则两个调用点，7768fe7 只改了窗口那个，layer 那个漏了。
       - 可用区：把 `layer.c` 里「输出盒平移到这块 layer 表面自己的坐标系」抽成
         `vidow_popup_box_to_layer_space()`，`view.c` 在 `view == NULL` 时先沿父面链爬到根 layer
         表面（`popup_root_layer_surface()`）再调它。
     - 验证：新 `test-popup-layer.sh`（`tools/menuprobe-layer.py` 当靶子；判据 = 两个弹出物在场景里
       **相邻**、纵向相交、都在屏里、真的画出来；两轮 scale 1 / 2）。修前 `FAIL scale1/scale2:
       二级弹出物没贴着父弹出物：0,0 114x82 与 301,17 275x268（边缘差 187 / 576 px）`、exit 1；
       修后全 PASS（边缘差 14px，就是那 14px 重叠）、exit 0。相关套件改前 / 改后都是全过：
       `test-popup-nested.sh` / `test-popup2out.sh` / `test-layerblur.sh`。
     - 顺带量到、**这次没动**（另案）：一级弹出物的场景树被建了**两份** —— `xdg_shell` 那条
       `new_popup` 到达时 `popup->parent` 还是 NULL，`server_new_popup()` 开头那句
       `wlr_layer_surface_v1_try_from_wlr_surface(popup->parent) != NULL` 的守卫**判断不出来**
       （注释里想防的「同一个 surface 两个场景节点」没防住），`layer_toplevels` 上于是也挂了一份。
       单屏 + layer 表面在布局原点时两份重合、看不出来；两块屏（layer 表面不在原点）时那份会画在
       错的位置上。

243. **「2 倍屏上窗口里的模糊底衬被放大了一倍」—— 量下来没有这个 bug**（用户：「同一坐标上，壁纸里的 16px 随机块在底衬里明显更大」，并明确要求别再拿互相关 / FFT 量位移：那种细节壁纸上峰值只有 0.07~0.19，上一轮就是被它误导成「模糊正常」的）

     - 靶子：`tools/cells.py`（新增）—— 16 **逻辑**像素的非周期随机灰块 + 三条彩色横带（逻辑
       80 / 360 / 640）。随机块没有周期（**周期图案放大 2 倍之后看着还是同一个图案**，这正是上一轮
       互相关读不出来的根源）；三条横带是各向同性模糊的**重心不变量**，糊得多厉害重心都钉在原处，
       所以能当尺子。横带位置都落在 16 的整数倍上，客户端按倍率渲染时正好压在设备像素网格上。
     - 判据用**同一坐标、同一张截图**的对照，不需要任何「参考图」：一张图里，窗口**右边**是没被盖住的
       壁纸（屏上原样），窗口**里面**是窗口自己的底衬（糊过）。量到的横带行（设备像素）：
         scale=1：屏上 82.5 / 362.5 / 642.5，底衬 82.6 / 362.9 / 643.1（差 +0.1 / +0.4 / +0.6）
         scale=2：屏上 165.5 / 725.5 / 1285.5，底衬 165.7 / 726.6 / 1286.7（差 +0.2 / +1.1 / +1.2）
       底衬里横带间距 = 280.4 / 280.2（scale=1）、560.9 / 560.1（scale=2）= **倍率 × 280 逻辑像素**。
       另一张图（把窗口挪走再截）做 A/B 同坐标对照：40 个亮点的重心一一配对，
       `A = 1.00055·B + 0.5`（内容真放大一倍的话斜率会是 2.0）。
     - 底衬缓冲本身（`VIDOW_BLUR_DUMP` 转储）跟壁纸纹理 × scale 逐像素比：**平均差 0.00/255**
       （同坐标、同一张纹理）；底衬里的块宽 = 16（scale=1）/ 32（scale=2）设备像素 = 倍率 × 16 逻辑
       像素。也就是说底衬里的内容和**屏上原样**是 1:1 的 —— 底衬没被放大。
     - 所以「大了一倍」不是底衬被放大，是**参考系**：底衬和屏上的壁纸都是**设备像素**
       （README 2849 / internals ③ / 108 条），16 逻辑像素的块在 2 倍屏上本来就是 32 设备像素；
       拿来比的是那张 1280x720 的**逻辑**纹理（16px），一比自然「大了一倍」。反过来说，真按逻辑像素
       画底衬就会和屏上的壁纸对不上（`test-blurscale.sh` 判据 4 钉的就是这个），所以这里**不改**。
     - 回归：`test-blurtexture.sh`（14 条：底衬尺寸 / 底衬内容 == 壁纸 × scale / 块宽 == scale × 16 /
       三条横带与屏上同坐标 / 横带间距 == scale × 280）。它的红绿都验过：
         * 把 `backdrop_render()` 的 `.scale` 写死成 `1.0`（108 条修之前的写法）→ scale=1 那轮 7 条
           全过、scale=2 那轮 4 条红（内容平均差 112.73/255、块宽 16 ≠ 32、红带错位 -70 设备像素、
           带间距 630 ≠ 560）—— 正是用户说的「1 倍正常、2 倍不正常」的形状。
         * 把倍率改成 `scale * 2.0`（内容**真的**放大一倍）→ 块宽 64 ≠ 32、内容平均差 73.24/255，
           当场红。

245. **overview 每个工作区的格子尺寸固定**（用户：「每个工作区的大小应该固定，现在开启多个工作区的时候一直缩小」）

     - 机制：`src/overview.c` 里 `fit_h = 0.92 / (double)ov->n_rows` —— 行高按工作区数量平分，
       行数一多每行就被摊薄（0.92 → 0.46 → 0.31 → 0.23，到 `OV_ROW_MIN_SCALE` 0.25 才兜底）。
     - 修法：纵向改成固定比例 `OV_ROW_FIXED_SCALE = 0.46`（横向仍按"最宽的带铺得进"算 `fit_w`，
       两者取小、封顶 1.0）。行数超过一屏排不下就靠列表滚动看（`list_scroll` / `rows_total`，
       README 235 那套 row-table 模型本来就支持）。
     - 验证：`test-overview.sh` 全过（里面 4 处硬编码采样区不受影响）；headless 量 `vidowmsg overview`
       的 `row_h` = 325.68 = 0.46 × 708（可用高度），与工作区数无关。**未做多工作区的对照量测**
       （harness 只造出 1 个工作区，`focus-workspace N` 不会新建），也没截图。

242. **滚出去的列滚回来时，窗口从屏幕最左边飞进来**（用户：「proton 游戏窗口滚动的时候从最左边移入」）

     - 现场：窗口早就落位了，**每次滚出去再滚回来**都从屏幕最左边滑入。用户还纠正了我一次方向：
       「列就是列，和全不全屏有什么关系，统一视为列」—— 对，修法不许按 fullscreen 分支。
     - 机制：滑入起点取的是「它现在画在哪」= `view->geom.x + anim_off_x`（`vidow_place_view()`，
       `src/view.c`）。但**被 park 的窗口上一帧根本没画在屏上**，这个值只是 park 之前那一次的 geom
       —— 列里每个窗口都这样，而全屏视图的 `geom.x` 恒等于输出框左边（0，见
       `arrange_columns(..., full_box, ...)`），于是起点成了「屏幕最左边」。普通列没露出来，只是
       因为它们 park 前的 `geom.x` 本来就在屏外、看着像"从正确的一侧进来"。
     - 修法：判据 = 这个起点**落在输出框里面**（说明上一帧它并不在屏上）→ 按新位置相对旧值往哪边
       走，把它推到那一侧的框外（往右走 = 从左边进 → `box.x - w`；反之 `box.x + box.width`）。
       真滑到屏外的窗口起点本来就在框外，不动它。**不按 fullscreen 分支**。
     - 验证：`test-fullscreen-scroll.sh`（24 条，README 191「全屏参与滚动」）+
       `test-fullscreen-2out.sh`（16 条）全过。**未做视觉复现**（`zz-fswin` 全屏 X11 窗口来回滚、
       截动画中间帧那条没跑），需要在真机上确认一次。

241. **layer 面的入场动画每帧重跑布局 → configure 洪水把客户端灌断**（用户：noctalia 日志「Wayland display closed during failed to read Wayland events; shutting down (display_error=32 (断开的管道))」）

     - 现场（用户机器：两块屏，1 倍 + 2 倍；noctalia 每块屏摆壁纸 / 状态栏 / 停靠栏 / 四个角，共 14 块
       layer 面，一起上屏）。vidow 日志在客户端断开那一刻是：
          00:00:07.693 [INFO] [wayland] Data too big for buffer (4092 + 12 > 4096).
          00:00:07.713 [INFO] [wayland] error in client communication (pid 3178905)
       `4092 + 12 > 4096` 是 libwayland 服务端的**出口缓冲**（`WL_BUFFER_DEFAULT_MAX_SIZE` = 4096）：
       里面已经攒了 4092 字节没送出去，下一条事件（12 字节 —— 一个只有头的 configure 正好 12 字节）
       放不下 → `wl_connection_queue()` 置 `client->error` → `destroy_client_with_error()`，
       **整个客户端被销毁**；客户端那边看到的就是 EPIPE —— noctalia 那句 `display_error=32（断开的管道）`。
       注意这条和 240 不是一回事：240 是 vidow 自己崩了（所有客户端一起 EPIPE），这里 vidow 活着
       （日志在断连之后还接着写了十几万字节），只掐了这一个客户端。
     - 机制：`layer_anim_update()`（layer 入场动画的每帧回调）每帧调一次 `vidow_arrange_layers()`，
       而那是**布局**：它给每一块 layer 面都发一次 `wlr_layer_surface_v1_configure()`（wlroots 的
       `wlr_scene_layer_surface_v1_configure()` 无条件发包）。动画是「每块正在动的面各跑一趟」，
       于是每帧 configure = 正在动的面数 × 面数 = 面数²：14 块面一起上屏就是每帧 196 条。
       实测（两块 headless 屏 + `tools/layerstorm.py` 摆 14 块面，在布局完全静止的 0.5 秒里量）：
       **9408 条 configure**（≈450 KB/s），而客户端 socket 的上限是 213504 字节 —— 客户端启动那几秒
       读不过来（或者像测试里那样被冻住），socket 灌满 → 上面那条 E2BIG → 客户端被掐。
       单屏不炸是量正好减半（7 块面 = 每帧 49 条），差一点没灌满，所以「只有两块屏才复现」。
     - 修法：动画只动**自己这一块** —— 新增 `layer_apply_anim()`（位置 = 布局定下的 `ls->base_x/base_y`
       + 动画偏移，再写 alpha 和 fx 透明度），`layer_anim_update()` / `layer_anim_done()` /
       `layer_open_anim()` 的「瞬间到位」那一支都改调它，不再整趟重排。布局该跑的地方照旧跑
       （map / unmap / 初始 commit / 输出变化），语义和以前一模一样，只是不再每帧重发 configure。
     - 验证：新增 `test-layerstorm.sh`（两块屏 1 倍 + 2 倍，`tools/layerstorm.py` 每屏 7 块面、
       `layer_open_ms = 3000`，客户端冻住 2 秒）。修前 FAIL：
       `布局静止的 0.5 秒里 configure = 9408 次（面数 14，判据上限 14）`、
       `客户端被 vidow 断开了（进程没了）`、`Data too big for buffer (4096 + 20 > 4096)` +
       `error in client communication (pid …)`，exit 1；修后全 PASS（同样的 0.5 秒里 configure = 0），exit 0。
       要求的三个套件改前 / 改后都全过：`test-layerblur.sh` / `test-blur2out.sh` / `test-popup2out.sh`；
       另外顺带跑了 `test-layerdestroy.sh` / `test-layerghost.sh`（改的就是动画那条路），也全过。
     - 还没做：`vidow_arrange_layers()` 每次重排仍然给**每一块** layer 面发 configure（哪怕尺寸没变）——
       一次 map 是 4 趟重排 × 面数条。量不大（14 块面 ≈ 800 条、19 KB，灌不满 socket），这次没动；
       真要收干净得自己算一遍 wlroots 那套摆位（full_area / usable_area + 锚点 + 边距），
       或者只对「状态真的变了」的面发。

240. **窗口在动画没跑完时被销毁 → 动画回调拿着已释放的 view（防御性修复，未复现）**

     - 现场：用户机器上 `coredumpctl list vidow` 有 5 个 SIGSEGV 核心转储（2026-10-12 23:20–23:47，
       几分钟一次）。栈：`#0 野地址 0x29b000004ec <- #1 vidow_output_render <- #2 output_frame <- 帧回调`；
       偏移对应源码是 `src/output.c:130/132`（那圈 blur 装饰循环 + 紧跟的 `vidow_anim_tick`）。
       vidow 一崩，所有客户端收到 `display_error=32（断开的管道）` —— 用户报的「noctalia 爆这个错误」
       就是这么来的（noctalia 日志 00:00:07.7，vidow 侧同一时刻只有一行 `error in client communication`）。
     - 机制（代码阅读定位）：动画回调的 `data` 就是 view 本身（`view_move_update` / `view_move_done`），
       而**两条销毁路径**（`view_destroy()` / X11 那条）都没取消动画 —— 其它所有路径都规规矩矩
       `vidow_anim_cancel(view->anim_move)`。窗口在动画中途没了 → 下一次 tick 拿已释放的 view
       写内存并调 `vidow_view_apply_anim()` → 野地址跳转。
     - 修法：两条销毁路径在 `wl_list_remove(&view->link)` 前补上 `anim_move` / `anim_open` 的取消。
     - **诚实说明**：harness 没能复现触发条件（入场动画 3 秒、map 后 0.6 秒杀窗口、12 次循环，
       修前修后都活），所以这条是**防御性修复**：它消除的是一个确实存在的生命周期漏洞，
       但"就是那 5 个核心转储的根因"尚未证实。要证实需要用户在真实负载下（Proton/X11 窗口 +
       切工作区）再跑，下次崩溃 `run.sh` 会打印栈的前几帧（见 `run.sh` 注释第 24 条），
       `/tmp/vidow-run.log` 也会留着。

238. **二级（嵌套）弹出物错位：子菜单按顶层窗口摆，丢掉了父菜单的偏移**（用户：「我发现 popup 的二级菜单会错位」）

     - 现场（headless 1920x1080，GTK3 菜单：一级 8 项、第一项带子菜单，`tools/menuprobe2.py`）：
       窗口 300,200、点 360,260 → 一级菜单 275x268 摆在 **773,731**（对），二级菜单 114x82 被摆到
       **561,200**（窗口左上角，= 窗口原点 + 二级相对一级的偏移 261,0），正确位置是 1034,731
       （一级菜单右边缘 1048 减 GTK 子菜单跟父菜单的 14px 重叠）—— 错 (473, 531) px。
     - 机制：`server_new_popup()`（`src/view.c`）用 `view_from_surface(server, popup->parent)` 找
       「父面所属的**顶层窗口**」，把每个弹出物的场景树都挂在**顶层窗口**的树上。而 wlroots 给的节点
       坐标（`popup->current.geometry`，见 `types/scene/xdg_shell.c` 的
       `scene_xdg_surface_update_position`）是相对**父表面**算的：一级弹出物的父表面就是顶层窗口，
       所以对；二级弹出物的父表面是**一级弹出物**，挂到顶层窗口的树上等于把「一级菜单在窗口里的
       位置」整段丢掉 —— 二级菜单平移 (父菜单.x − 窗口.x, 父菜单.y − 窗口.y)。X11 那条路
       （override-redirect 菜单走 X 的绝对坐标，`vidow_view_x11_popup_box`）本来就没这问题，所以
       只有 Wayland 客户端（GTK / Qt / kitty）的二级菜单错，一级看着一直正常。
     - 修法：新增 `struct vidow_popup_scene` + 一张登记表（`vidow_server.popup_scenes`）：建弹出物
       场景树时按「surface → 树」登记（`popup_scene_register()`，表面销毁时摘掉），开新弹出物时
       父面要是弹出物就按 surface 查出父树、挂上去（`popup_scene_tree_for()`）；查不到就退回原来的
       挂法。wlroots 没有 surface → 场景节点的反查接口，只能自己记一份（sway 是给每个弹出物挂一个
       父表面的 `new_popup` 监听器；vidow 收的是 shell 的全局信号，加登记表这条改动最小）。
     - 验证：新 `test-popup-nested.sh`（`tools/menuprobe2.py` 当靶子；判据 = 两个弹出物在场景里
       **相邻**、纵向相交、都在屏里、真的画出来；两轮 scale 1 / 2）。修前 `FAIL scale1/scale2:
       二级弹出物没贴着父弹出物：271,10 114x82 与 71,71 275x268（边缘差 314 / 75 px）`、exit 1；
       修后全 PASS（边缘差 14px，就是那 14px 重叠）、exit 0。相关套件改前 / 改后都是全过：
       `test-popup2out.sh` / `test-xwayland-popup.sh` / `test-layerwin.sh`。
     - 当时没修、**已由 244 修掉**：挂在 layer surface 上的弹出物再弹二级（状态栏菜单的子菜单）
       当时还是老样子。那条路不止「`layer.c` 的 `layer_surface_new_popup()` 也把弹出物挂在 layer
       那棵树上」这一个坑：二级弹出物的信号根本不到 `layer_surface_new_popup()`（wlroots 只发
       `xdg_shell` 那条），可用区还会算成 0x0 —— 详见 **244**。

237. **平铺窗口 unmap 之后还占着一条列（「凭空出现一列」）**（用户：「lutris 和游戏窗口之间凭空出现了一列，什么窗口都没有但占据了一列的空间」）

     - 现场（用户给的 `vidowmsg windows` dump）：id 59、标题 `Input`、`mapped=false`、`visible=false`、
       `float_mode=none`（平铺），可 `column` 还在、几何 `2883,6 951x1068` —— 正好夹在 lutris（1926–2877）
       和游戏（3834–5754）两条列中间。那就是"空列"：屏上什么都不画，列宽却照占。
     - 机制：`view_unmap()` 只处理了浮窗（`vidow_workspace_track(..., false)`），注释还写着「列里的窗口
       跟着列，不用记账」—— 但列是**按 `view_shows()`（只排除 minimized）**算宽的，一个 unmap 了的平铺
       窗口照样算"还占着位置"，于是那条列一直留着一列宽。proton 游戏就会这样：它的 `Input` 输入法辅助
       窗口先被平铺进一条列，随后 unmap（连接不断、surface 还在）。
     - 修法：`view_unmap()` 里平铺（`float_mode == NONE`）且 `column != NULL` 的窗口走
       `vidow_view_leave_layout()` 摘出列；它回来时 `view_map()` 会走 `view_ensure_column()` 重新归位。
     - 验证：新 harness（`/tmp/zz-emptycol2.sh` + 一个 5 秒后自己 `hide()` 的 GTK 窗口）：藏之前
       `vidowmsg windows` 2 条列（邻居 6,6 631x708 / 会藏的 643,6 631x708），藏之后 **1 条列**。
       `test-fullscreen-scroll.sh` / `test-minimize.sh` / `test-scrollcull.sh` 全过。
     - 排查过程（记一笔，别重复走）：先猜全屏抢焦点（就是 233，错）、再猜全屏列占整屏宽（错）、
       再猜「X11 窗口 unmap 后 surface 被摘」（用自造客户端量到 surface 不摘，误判成"vidow 收不到
       unmap"）—— 直到用户给了真实现场 dump 才看见 `mapped=false` + `column` 还在。**自造客户端
       复现不出真实现的窗口行为，别拿它下结论。**

235. **全景改成「所有工作区上下排」**（用户：「overview 应该显示所有工作区（上下排布）」「滚轮上下控制
      工作区列表的滚动预览」「移动窗口焦点的热键控制工作区内窗口的滚动预览」「overview 要绘制每个工作区的
      区域和背景」）

     - 现场：旧的全景是「**一条列带 + 一个相机**」——`strip_box()` 只统计**当前显示着的那块工作区**里
       可见窗口的几何，别的工作区根本不在场；滚轮走 `vidow_overview_pan()` **平移相机 x**，列带比屏宽，
       一直滚就把整条带滚出屏外 —— 用户说的「滚轮一直滚会左右滚动窗口，会跨屏」就是它。
     - 模型换成**每块工作区一行**（`server.h` 的 `struct vidow_ov_row`）：行表按 `server->workspaces`
       的顺序取这块屏上的工作区（当前这块 + 有窗口的），行间距写死 12px；所有行**共用一个 scale**
       （`fit_w = 0.92×工作区宽 / 最宽那条带`、`fit_h = 0.92 / 行数`，下限 0.25 —— 行多了列表就比屏高），
       行高 = 工作区高 × scale；排得下就上下居中，排不下就按 `list_scroll` 滚。**空的工作区不摆行**：
       vidow 每块屏的栈尾永远留一块空的（`vidow_workspace_ensure_bottom()`），摆上就是白占半屏
       （当前这块例外，空也摆 —— 用户就在那儿）。
     - 三件事各归各的输入：
       1. **滚轮 → 工作区列表**（`vidow_overview_scroll()`，一次 60 屏幕像素，`input.c` 那条分支改调它）。
       2. **「移动窗口焦点的键」→ 那一行自己横向滚**：行内偏移跟着**那块工作区自己的视口**走（显示着的用
          `output->viewport_x`，它本来就跟着焦点动；藏起来的用 `ws->viewport_x`），并且夹在
          `slack = 工作区宽 - 条带宽×scale` 里 —— 条带永远不会滑出这一行的区域，所以所有窗口始终在屏内。
       3. **每行一块背景板**（`plate_apply()`，和遮罩同一层、铺满工作区宽），浓度跟着遮罩 alpha 走
          （`dim[3] × t × 0.25`，写死、不另加配置项；`overview_dim_color` 的 alpha = 0 时它跟着一起没有，
          壁纸像素原样）。
     - 藏起来的工作区从来没被 `arrange` 过（`col->offset` / `view->geom` 是上次显示时的老值，照着画会叠在
       一起），所以新增 `vidow_workspace_preview_layout()`（`view.c`）：按列宽比例**现算**一遍摆位，
       只回调、不写 `view->geom`。`t = 0` 仍是恒等变换（`vidow_overview_map()` 从窗口自己的
       `geom + 动画偏移`出发，展开那一端算好，中间按 `t` 插值）—— 退出动画不会重放进入。
     - 踩到的坑（像素量出来的）：`wlr_scene_rect` 的颜色是**预乘**的，第一版按直乘给，板子不减暗、反而
       每通道 +5（`dim.rgb×255` 直接加上去）；改成 `dim[i]×a` 才对（跟 `fx.c` 那套着色器一个规矩）。
     - 验证：`test-overview.sh` 62 项全过（和改动前逐条比对，只有「点选目标」的 x 从 57 变 6 —— 行内
       左对齐，以及遮罩采样点数因板子边缘少了 1000 来个）；`test-scrollcull.sh` / `test-workspace.sh` /
       `test-minimize.sh` / `test-fullscreen-scroll.sh` 全过。headless 5 块工作区（3/2/2/2/1 个窗口）截图
       量到：4 行图案上下排（第 5 行得滚下去看）、行里比行间距暗 13%（板子）、滚轮 10 格把 `list_scroll`
       推到上限 225（列表 933 > 工作区 708）而同一行的窗口 x 一个像素没动、`focus-column` 让那一行整体
       横移 80px 而别的行纹丝不动。
     - 还没做的：空工作区到底要不要也摆一行（用户要的是「所有工作区」，摆的话默认就永远是 2 行：当前 +
       栈尾空位，真正要看的东西平白小一半）；浮窗不跟条带滚（用它自己的 `geom` 减工作区原点，藏起来的
       工作区里浮窗是老值）；`vidowmsg overview` 不再报 `pan` / `camera`，改成 `rows` / `row_h` /
       `list_scroll`（没有测试和文档依赖旧字段）。

234. **全屏不抢焦点**（用户：「全屏窗口一直 fullscreen 抢焦点，需要一个忽略全屏窗口焦点事件的方法，类似 hyprland」）

     - 233 那条修法（全屏立刻拿焦点）方向错了，回退。游戏会在**关 / 开之间反复切全屏**
       （用户日志：04:24.600 关 → 04:24.650 开，04:27 / 04:31 各一轮），每开一次就把焦点
       从用户手里抢回去一次 —— 用户报的「一直抢焦点」就是它。
     - 现在的规矩（Hyprland 那条）：**全屏请求不改变焦点**。要抢焦点的客户端自己发
       `_NET_ACTIVE_WINDOW` / `xdg_activation`，那条路照旧（见 `activation.c`）。
     - 复现 / 回归工具：`/tmp/zz-fswin proton`（不请求焦点、map 后自己进全屏、之后每 1 秒
       关一次再开一次全屏，模拟 proton 游戏）。修前：用户把焦点切到别人那条列，游戏每轮
       「开」都把焦点抢回去；修后：9 轮开关、焦点一直待在用户放的那条列上。
     - 还没解决的是「空列」：全屏那块在条带上占**一整屏宽**（191 那条），不聚焦时它跟着
       视口滑出去，那块屏上就空出一整屏宽的位置。**这条还没修** —— 已排除四个假设：全屏抢焦点
       （就是 233）、全屏列占整屏宽、X11 窗口 unmap 之后还占着列（实测 vidow 根本收不到
       `XUnmapWindow`）、以及「全屏 + 失焦」这个静态状态本身（headless 双屏 + proton 式客户端
       截图否掉的：游戏 `2877,0 1920x1080`、lutris `1926,6 951x1068` 时那块屏是满的）。差一张现场图。

233. **没焦点的窗口自己进全屏 → 摆在半屏外**（用户：「proton 游戏打开时还是半屏空隙，游戏一直在最右边，
       半屏好像是有交互的，不是覆盖是交换了窗口」）

     - 现场（用户日志 `/tmp/vidow-run.log`，游戏窗口 1920x1080 被摆到 `2877,0` / `3840,6` / `963,0`）：
       全屏那块在条带上占**一整屏宽**，而视口钉在**聚焦那条列**上。一个**没有焦点**的窗口自己
       进全屏（X11 游戏最常见：启动器把它拉起来时焦点还在终端上，或者它失焦后又自己进全屏），
       就会被摆在旁边半屏外 —— 右半屏是游戏、左半屏是别人（终端是黑的，看着像「空隙」，点上去
       还有交互），用户看到的就是「游戏一直在最右边、好像两个窗口交换了位置」。
     - 复现：`/tmp/zz-fswin unfoc`（map 之后 2.5 秒、焦点已经被挪走时才发 `_NET_WM_STATE_FULLSCREEN`
       的 ClientMessage）→ 修前几何 `637,0 1280x720`（整块屏是 `0,0 1280x720`）。
     - 修法（`src/view.c` 的 `view_apply_fullscreen()`）：**全屏的窗口立刻拿焦点**（只对画在屏幕上、
       没最小化的窗口，别的工作区里的窗口自己进全屏不把人拽过去）。聚焦它 = 视口对准它，整块屏归它。
     - 验证：同一个复现脚本 → 修后 `0,0 1280x720`，整块屏归它。`test-fullscreen-scroll.sh`、
       `test-fullscreen-2out.sh`、`test-focus.sh`、`test-scanout.sh`、`test-x11scale.sh` 全过。
     - 剩下的是客户端自己的行为：这颗游戏**失焦时会自己发 `_NET_WM_STATE` 的 REMOVE 退全屏**
       （日志 `全屏: 崩坏：星穹铁道 关（X11 请求）`），退回半屏一列是它要求的；再拿回焦点时会重新
       进全屏 —— 现在会整块屏盖住，不再停在半屏外。

232. **快速切工作区崩在动画 tick 里**（用户：「工作区快速滚动会崩溃」）

     - 现场：`coredumpctl` 里 23:20 / 23:21 / 23:21 / 23:23 连着四个 SIGSEGV（`./build/vidow`），
       栈是 `output_frame → vidow_output_render`（跳到一个垃圾地址，比如
       `0x2500000f00`）和 `output_frame → vidow_output_render → vidow_anim_tick`。
       垃圾地址 = **函数指针取自已经 free 掉的内存**。
     - 为什么：`vidow_anim_tick()` 用 `wl_list_for_each_safe` 迭代 `server->anims`，
       `tmp` 是「下一个」的裸指针。而回调里会**取消别的 anim** —— 最典型就是切工作区的
       `done` 回调：`slide_done` → `vidow_arrange()` → `vidow_place_view()` →
       `vidow_anim_cancel(anim_move)`（窗口的移动动画）。`vidow_anim_cancel()` 只有
       「anim 自己正在 update」那一层保护（`anim->running`），从 **done 回调**里转进来的
       这次取消不在保护范围内：当场 `wl_list_remove` + `free`，`tmp` 立刻变野指针，下一轮
       迭代就跳进已释放的 `update`。窗口有动画（`window_move_ms` / `column_width_ms`）时
       快速切工作区 = 每次都撞上，所以「滚快一点就崩」。
     - 修法：`struct vidow_server` 加 `anim_ticking`（`src/server.h`），`vidow_anim_tick()`
       进出时置位/还原；`vidow_anim_cancel()` 在 tick 期间**只标 `cancelled`**，交给 tick
       自己收尾（和它原来处理「自己正在 update」那条路一个语义：done 照样只跑一次，只是
       挪到这一轮 tick 的末尾）。
     - 验证（临时脚本 `/tmp/zz-animcrash.sh`，测完删掉：4 块工作区各 3 个窗口、
       `window_open_ms/close_ms/move_ms/column_width_ms/workspace_switch_ms` 全开、
       每 30ms 一次 `workspace up/down`）：**修之前第 1 次切换就 SIGSEGV**（coredump 栈和
       用户那份一模一样），修之后 120 次快滚活下来、日志 0 条崩溃痕迹。
     - `test-closeanim.sh`、`test-workspace.sh` 全过。

231. **X11 客户端自己改 `_NET_WM_STATE` 要全屏，vidow 收不到**（用户：「proton 游戏启动的
     时候会有半屏空位，然后窗口不是全画幅，而是半屏大小（带窗口装饰），然后会一直在屏幕的
     右半屏」）

     - 现场（用户的 `/tmp/vidow-run.log`，`崩坏：星穹铁道`）：窗口 951x1024（半屏一列的宽度
       + 装饰）、x 在 2883 / 3840（右半屏、甚至半块屏外），日志里**一条 `全屏:` 都没有**
       —— 也就是「客户端要全屏」这件事从头到尾没传到 vidow。
     - 为什么：X11 的全屏请求有两条路。① 客户端发 `_NET_WM_STATE` 的 **ClientMessage** →
       wlroots 发 `request_fullscreen` 信号 → 我们处理（这条一直好着）。② 客户端直接
       **改属性**（`XChangeProperty`，wine / Proton 的游戏就是这条）→ wlroots 的
       `read_surface_net_wm_state()` 只把 `xsurface->fullscreen = true` 赋值，
       **一个字都不发**（`xwayland/xwm.c`）—— 信号没有，vidow 就永远不知道，游戏被当普通
       平铺窗口摆成半屏一列。
     - 修法：`view_sync_x11_fullscreen()`（`src/view.c`）在**交帧**和 **map** 两处把
       `view->x11->fullscreen` 和 `view->fullscreen` 对一下（就是个指针比较），不一致就
       走 `view_apply_fullscreen()`，日志写「X11 的 _NET_WM_STATE 属性变了」。
     - 验证（临时 X11 客户端 `/tmp/zz-fswin.c`，三种时序，测完删掉）：
       `prop`（map 之后改属性 = 游戏那条路）**旧代码：631x708 半屏一列、`全屏=False`；
       新代码：0,0 1280x720、`全屏=True`**，日志有那一行；`late`（ClientMessage）没回归；
       `early`（map 之前就把属性挂上）**仍然不认** —— 用 `xprop` 查出来原因是
       wlroots 自己在焦点变化时会重写 `_NET_WM_STATE`（只剩 `_NET_WM_STATE_FOCUSED`），
       客户端抢跑写进去的 FULLSCREEN 位被抹掉了，跟 vidow 无关；这条留在 README 里当已知
       边界（真要治得在 wlroots 侧改，或者建窗前自己查一次属性）。
     - `test-x11scale.sh` 全过。

230. **切换器清单的预览画到隔壁显示器上**（用户：「开启大量窗口的时候，滚动列表，
     超出视口的窗口会在另一个显示器显示」）

     - `mru_layout()` 末尾那条「滚出去 / 挤出去的不画」判的是**缩略图**和
       **工作区**有没有交集；卡片的底、描边、四个圆角是各自独立的场景节点
       （`wlr_scene_rect` / 自己的小 buffer），裁不了 —— 半张卡片露在工作区外的
       那一半就画到隔壁屏上了。
     - 改成判**整张卡片**（`cx/cy/cw/ch`）是不是被工作区**包住**
       （`wlr_box_contains_box`）。代价：贴着视口边缘那半张预览现在直接不画了
       （以前露一半），滚起来是「整张进出」而不是「滑一半进来」。
     - 验证（headless 两块 1280x800 并排，屏 1 上 8 个窗口，临时脚本，不进仓库）：
       改之前逐步滚清单时，第 2~4 步隔壁屏上有 **22627 个绿色像素**（屏 2 上本来
       一个窗口都没有）；改之后每一步都是 0，屏 1 上的预览照旧
       （红/绿各 88077 个像素），0 条 ERROR。`test-mru.sh` 全过。

229. **切工作区加过渡（纵向滑动）**（用户：「切换工作区需要过渡动画」）

     - 以前一块屏上只有一块普通工作区亮着（`vidow_workspace_shown_for`），
       「切工作区」是瞬间换人：`output_arrange` 只摆当前那块，旧的直接不摆了。
     - 现在按 niri 的样子做成**纵向滑动**：旧的那块往上（或往下）推开，新的从另一边
       进来，距离是整块屏高。方向看两块在栈里的前后（新的靠下 = 从下面进来）。
     - 做法：过渡期间**两块工作区都在场景里**，各自的纵向偏移存在 `ws->slide_y`
       里；`output_arrange` 把 Pass 3 抽成 `arrange_columns()`，按各自的偏移摆两遍
       （旧的用**它自己**的滚动位置 `ws->viewport_x` —— 视口已经先换成新的了，
       不这么做旧的那块会横着跳一下）。「可见」本身**不含**正在滑走的那块
       （`vidow_workspace_view_visible` 照旧只答「属于现在显示着的工作区吗」），
       放行的是**画**：`vidow_view_should_park` 里多一条 `view_sliding_out()`
       —— 一上来写成「可见也放行」的时候，`test-ipc.sh` 的「空工作区上
       pick-window 是 null」当场翻车（过渡里那半屏旧窗口被当成可见的挑出来了）。
       到位时旧的那块正好整块落在屏外，`!in_output_box` 把它停到停车位。
     - 窗口的位置**是过渡推出来的**（每帧 `vidow_arrange`），所以过渡期间
       `vidow_place_view` 不给每个窗口起位移补间（新的计数器 `workspace_slide_n`，
       和列宽过渡那条 `column_anim_n` 一个道理）：不然窗口会吊在过渡后面飘。
     - 新配置键 `workspace_switch_ms`（默认 200，0 = 直接换），在 `04-anim.lua`
       那一族里；`config.example/configs/04-anim.lua` 已重新生成
       （`test-defaultconfig.sh` 全过）。
     - 边角：连着切两次工作区，上一条过渡先掐掉（它自己的 done 会把偏移清零、把
       计数减回去）；旧的那块要是空的就不滑（空工作区可能当场被
       `vidow_workspace_cleanup` 收掉）；`ws_destroy` 里另加一道「把指向它的过渡
       收干净」的保险；**overview 打开时先把过渡收住**（`vidow_workspace_slide_cancel`）
       —— 它按每块工作区自己的几何算缩略图，正滑着的那块会摆歪。
     - 验证（headless 1280x800，临时脚本，不进仓库）：切 1→2 时日志里
       `place a1 -> 6,6 … 6,-794`（正好一整块屏高、out_cubic 先快后慢）、
       `place b1 -> 6,350 … 6,6`（从下面进来、停在正常位置），
       `anim 切工作区 done: 211ms / 14 帧`；`window_move_ms = 190` 那次
       **`move tween` 一条都没有**（早退那条生效）；连拍 11 帧里 5~6 对相邻帧不同、
       末帧和「不动画」时的画面一样；0 条 ERROR。过渡途中开 overview：
       过渡当场收住、窗口回到正常位置、0 条 ERROR。两块工作区在过渡里都画着，
       日志里看得到：`a1 显示（1）`（过渡开始放行）→ 1.6 秒后 `a1 藏起（停到屏外）`
       —— 中间那段 a1 / b1 都是「显示」。
     - `test-workspace.sh` / `test-ipc.sh` 的配置里加了 `workspace_switch_ms = 0`：
       这两个测的是**稳定状态**（前者数日志里「全屏 N 个可见」、后者切完就
       `pick-window`），过渡里旧的那块还画着会把数字带偏。过渡本身是动画，
       按「UI 调整不写测试」的规矩不进测试。

228. **动画看门狗在真机上多推一帧 → 后端把这一帧拒掉**（用户：「第一个 kitty 窗口的透明度在仿佛变化」）

     - 先排除的：vidow 里没有透明度动画（配置里没有 opacity/fade 这类键，
       `opacity` 只是 `window_opacity` 的别名），用户配置里 `window_opacity = 1`。
       他那个 kitty 是 `background_opacity 0.8` + `background_blur 1`（kitty 自己
       的配置），所以「透明度在变」看到的其实是**它背后那一块**在变。
     - 复现：headless + 用户整套配置 + 半透明 kitty，静止连拍 24 帧逐像素比 ——
       除了 kitty 自己闪的终端光标（36 像素）**一像素不差**；把另一个窗口压上来
       再拿开、回到同一布局，也还是一像素不差。所以静止 / 重叠切换这条路是干净的，
       问题要么在真机的提交路径上，要么得看他屏幕上到底什么样。
     - 真机日志里唯一的异常：51 条
       `connector DP-3: Failed to page-flip output: a page-flip is already pending`，
       而且**全部紧跟在 `swap-window` / 切工作区后面**（= 动画在跑 = 看门狗刚被叫
       起来）。wlroots 那边这条是 `goto out`：这一帧直接不提交。
     - 根因：`anim_watchdog` 是给**嵌套**后端兜底的（宿主合成器只在有损伤时才给
       帧），但它无条件按「帧间隔的 1.25 倍」自己推帧。真机（DRM）上帧事件本来每
       vblank 就来，它推的这一帧偶尔和帧事件撞进同一个 vblank → 一帧里提交两次 →
       第二次被拒。用户日志里那些是 3 条挤在 13ms 里。
     - 修法：判据改成「后端沉默」。上一帧事件过去还没超过 2 个间隔（`2 * iv`）就
       不自己推，等它；超过才算后端不给帧了，看门狗接管（原来「窗口停在最后一帧」
       那条路照旧有人管）。等待时长也跟着改成「等后端沉默够久」而不是「等渲染
       间隔」，否则会 1ms 一轮空转。
     - 补充：这一帧不是丢了 —— wlroots 的 `pending_commit_damage` 要等提交**成功**
       才清（`scene_output_handle_commit`），所以损伤会留到下一帧重提交。屏幕上的
       表现是这一帧晚一个 vblank，也就是动画里一顿。
     - 验证：`test-drag-stall.sh`（看门狗接管那条路）、`test-blur2out.sh`、
       `test-blurnooverlap.sh`、`test-blurcache.sh`、`test-closeanim.sh` 全过。
       真机上「还有没有 page-flip 报错」得等他重启一次看。

227. **窗口还没有表面时给焦点 → 合成器 abort**（用户：「启动 proton 游戏会导致 vidow 崩溃」）

     - 现场：`coredumpctl` 里 22:08 那个 SIGABRT（7.1M），栈是
       `server_cursor_axis → vidow_pointer_axis → vidow_keybind_wheel → vidow_action_run
       → vidow_focus_column → vidow_focus_view`，最后一句是
       `vidow: types/seat/wlr_seat_keyboard.c:309: wlr_seat_keyboard_notify_enter:
       Assertion 'surface' failed.` —— 滚一下滚轮（`Mod+滚轮` = focus-column）换焦点时，
       目标窗口的 `vidow_view_surface()` 是 NULL，wlroots 那个函数第一行就是
       `assert(surface)`，于是 abort。
     - 为什么是 wine / Proton：X11 的窗口先建出来、也能拿到焦点，但表面还没到（或者
       刚销毁）—— 日志里那一段就是 `HSR (wine) 的日志` 那个窗口 `float mode 0 -> 1 -> 0`
       来回跳、`clip: … 不裁（屏框 0,0 0x0）`，全是这半秒钟的过渡态。
     - 修法：加 `vidow_seat_focus_surface(seat, surface)`（`src/input.c`），**所有**
       给窗口键盘焦点的地方都从这里走；surface 是 NULL 就
       `wlr_seat_keyboard_notify_clear_focus()`（语义就是「这会儿没人该拿键盘」），
       不往 wlroots 那个 assert 里送。顺带把 `vidow_xwayland_grab_assert` 也换成它。
     - 验证（临时故障注入，测完删掉）：让 `vidow_focus_view` 对某一个窗口假装
       「表面还没到」，然后滚轮换焦点 —— 旧代码复现出一模一样的
       `Assertion 'surface' failed`（把合成器打死），新代码活下来、焦点正常落在
       那个窗口上、日志里 0 条 assertion。注入代码没留下。
     - 另一件事：21:13 那个 SIGSEGV 不是这条 —— 栈是
       `cmd_clipboard_history_get → jget_bool`，是当时那个还没修的剪贴板 IPC bug
       （`jget_bool` 第三个参数传了 `false` 当出参），已经在本轮 225 里修掉了。

226. **滚轮 + 修饰键那三组绑定**（用户：「还需要 alt + 滚轮切换工作区 alt+shift+滚轮切换窗口工作区
     ctrl+alt+滚轮交换窗口位置」）

     - `Alt+滚轮` → `workspace up` / `down`、`Alt+Shift+滚轮` → `move up` / `down`：这两组
       都是现成动作，写进默认键位（`configs/14-keys.lua`）就行。修饰键是**精确匹配**
       （`mods_match` 比的是整个「有意思的修饰键」掩码），所以 `Alt+滚轮` 不会顺带把
       `Mod+滚轮` 那条也跑掉 —— 实测按住 Alt 滚一格只切工作区、焦点列一点没动。
     - `Ctrl+Alt+滚轮` 要的新动作 **`swap-window left|right`**：以前没有「交换」这个动作
       （只有 `move-column` 挪列、`move-window` 列内挪窗口）。语义 = 和相邻那列里的窗口
       换个位置，两个窗口各自住进对方那一格、焦点跟着自己走；列里只有一个窗口时（常见的
       那种）就是两条列的内容对调。和谁换：对方那列里**同一层**那个（层数不够取最后一个），
       这样一列里叠了几个窗口时换完不会跳层；整列都最小化了的列跳过（它不占条带）。
       浮窗 / 那个方向没有别的列 → 什么都不做，日志报人话。
     - 拿真事件量过：注入 `send-key press alt` + `pointer-scroll`（走的就是真鼠标那条
       `vidow_pointer_axis`），三组都按预期生效；`Ctrl+Alt+滚轮上` 在 `[a1] | [a2] | [a3]`
       上把 a2 和 a1 对调、焦点跟着 a2 走，滚回来还原。
     - `Alt+Shift+滚轮` 要**跟着走**：`move up/down` 本来的语义是不跟（人留在原工作区，
       和 `Mod+Shift+U/I` 一致），所以给 `move` 加了个 `follow` 词 ——
       `move up follow` / `move down follow` = 搬完连人带屏切到那块工作区、焦点还给搬过去的
       那个窗口。默认键位就用这条；老写法一个字节没动（`Mod+Shift+U/I` 还是搬完不跟）。
       `follow` 只跟 `up` / `down` 搭，拼错（`move down folow` / `move chat follow`）会报人话
       并且什么都不做。

225. **剪贴板加历史 + 一个 GUI**（用户：「内置的剪贴板同步需要一个 GUI」，以及「需要能从剪贴板
     历史里面复制内容，剪贴板历史里如果是文件路径（file 前缀）则复制文件」）

     - **历史**（`src/clipboard.c` + `src/server.h`）：`clipboard_history` 条（默认 32）+
       `clipboard_history_bytes` 字节预算（默认 64 MiB），新的在前。快照只在数据**取完**时才记
       （X11 侧 `staging` commit 完 / Wayland 侧管道读完），一条按 `mime_rank` 留前 6 个类型
       （够装「文本 + uri-list + gnome-copied-files」这种一份内容带好几个类型的情况）。
       去重（逐类型比字节）只把老的那条挪到最前、**不新增**；超条数或超字节从最旧的删，
       但**至少留一条**。两个配置都是热的（改成 0 当场清空，之后不再记）。
     - **拿回来**：`clipboard-history-use` 走的是和客户端复制**同一条** `sel_publish()` 路
       （xwm 去 X 那边声明所有权、我们的内存副本应答 `SelectionRequest`），所以拿回来之后
       谁粘都行，不挑焦点。
     - **文件复制**（用户那条要求的正解）：`clipboard-set-files <路径…>` 一次发三个类型 ——
       `text/uri-list`（CRLF）、`x-special/gnome-copied-files`（第一行 `copy`，LF）、
       `text/plain;charset=utf-8`（路径文本）；路径按 RFC 3986 百分号编码（空格 `%20`、中文也编）。
       因为 `text/plain` 的 rank 更靠前，`clipboard-get` / 预览默认看到的是路径文本，文件管理器
       看到的仍是 uri-list，两边不打架。历史里这种条目标 `is_file`，并且把
       `gnome-copied-files` 提到和 `uri-list` 同一档 rank —— 它不会被挤出前 6，从历史拿回来
       还是**复制文件**，不会退化成一段路径文本。
     - **GUI**（`tools/vidowclip.py`，GTK3 / `python3-gi`）：状态条（CLIPBOARD / PRIMARY 各自的
       origin、X owner、类型数、字节数）、历史列表（双击 = 拿回来）、预览（文本全文 / 图片缩略图 /
       文件路径表）、热开关（剪贴板 / PRIMARY / 历史条数 / 历史预算）、塞一行文本与三个清空按钮。
       走的是和 `vidowmsg` **完全相同**的 IPC（订阅 `clipboard-changed` / `primary-changed` /
       `clipboard-history-changed`，500ms 兜底拉一次），没有第二条私有通道；`--check` 不建窗口、
       只把 IPC 走一遍（脚本里判断能不能用）。
     - **顺手修掉一个老 bug**（GUI 第一次跑就崩在它上面）：`ipc.c` 里两处
       `jget_bool(req, "base64", false)` —— 第三个参数是**写哪**，传 `false` 就是 NULL；
       客户端只要明写 `{"base64":false}`（JSON 布尔，不是字符串），合成器就写到空指针上。
       `cmd_clipboard_get` 那处是原来就有的，两处一起改成局部变量。
     - 判据：`./test-clipboard.sh` 从 68 项加到 **111 项**（新增 H 段 43 项：X11 与 IPC 两条路
       都进历史、去重不新增且 id 不变、上限到了淘汰最旧的、`use` / `delete` / 越界报错、
       文件复制三个类型逐字节核对（含 `%20` 与中文的百分号编码）、`gnome-copied-files` 里
       uri-list 还在、`clipboard_history=0` 当场清空且不再记、`clipboard-history-changed` 事件、
       `vidowclip.py --check`），**全过**。GUI 的样子在 headless 上截过图逐张看过（文本 / 图片 /
       文件三种预览、开关与列表）。

224. **测试套件清一遍：删掉 25 处「测了等于没测」和死代码，顺手修好两条一直是红的判据**
      （用户：「检查一下测试项目，去掉无用和没必要的测试」）

      - **85 个 `test-*.sh` 全过了一遍**：每个文件在 README 里都有独立的场景靶子，**没有整文件
        冗余**（blur 家族那 11 个文件的靶子也逐个对过）。删掉的是三类：
        ① **恒真判据 10 处** —— `check(True, ...)` 只打印数字、无条件的 `pass` 6 条；
        `test-drag-stall.sh` 里「grep 日志里有没有 `vidow ready`」（日志只增不减，启动时已经
        `exit 1` 过一遍，永远为真）；`test-config.sh` 里「别的文件动了不重载」（那个文件写在
        `$RUNDIR`，根本不在被监视的目录里，而且真重载了值也还是 33）；
        ② **同文件重复 5 处** —— 条件被另一条蕴含：`TWEENS==0` → `LAG==0`、
        `focus_on_open=false` 被更长的 `… focus_on_activate=nil` 包含、`W2=2×W1` → `S2≠S1`、
        背靠背查同一个进程的存活、同一个 `getcfg gaps` 断言两遍；
        ③ **死代码 10 处** —— `DISPLAY_NOW` 全仓只出现一次且分支体是空语句、
        `pc`/`pd`/`OWN1`/`OWN2`/`pt`/`rt`/`CASE_FAILED` 算了没人用、写了没人读的 `keys.txt`
        与 `log-N.txt`、8 个定义了没人调的函数、12 个文件里没用上的 `pass()`/`fail()`。
      - **其中两条判据一直在骗自己**，一并修掉：`test-defaultconfig.sh` 的
        `check("corner_radius = 7" in log or table["corner_radius"] == "7")` —— `or` 的后半已被
        上一条断言保证，而日志里根本没有带空格的 `corner_radius = 7`（打出来的是
        `corner_radius=7）`），整条一直靠 `or` 活着，删掉；`test-focus.sh` 的两条像素判据拿
        `(0,0,0)` 当「窗口没画出来」，可兜底色默认是 `#14161c`（= 20,22,28，比容差 24 差一点点
        超出去）——**这两条是真·红的**（改动前跑也是红的），改成 `DESKTOP = (20, 22, 28)`。
      - **`config.example/config.lua` 跟 `--default-config` 走散了**：`18-background.lua` 那行的
        注释被手改长过、`17-overview.lua` 那行少一格对齐。按 README 说的「`config.example/` 就是
        `--write-config` 写出来的那一套」重新生成，`test-defaultconfig.sh` 的逐字比对恢复。
      - 判据：改到的 35 个测试逐个重跑（serially），**全过**；`test-events.sh` 头一次报
        「Mod+T 没生效」是偶发，复跑过。README 里各测试的项数跟着减（-1 到 -2）。

222. **全景里的壁纸可以糊掉（`overview_blur`，默认开）**（用户：「给 overview 也加背景模糊（niri 那种糊壁纸）」）

      - 做法：复用 layer 那条底衬路 —— 共享底衬（`output->blur_tex`，就是「壁纸 + background /
        bottom 层」跑完 kawase 那张）+ `blur_only` 那次装饰渲染，只是把目标摊成整块屏，画在
        `layer_overview_dim` 那棵树里（壁纸之上、窗口之下）、垫在压暗遮罩下面。跟 layer 的
        bottom 层底衬是同一套代码路径（`fx.c` 的 `vidow_overview_blur_frame`）。
      - **一帧只画一次**：模糊结果跟着 `blur_ver` 走（底衬变了才重画），全景动画期间那张缓冲
        一直复用 —— 每帧只是把已经画好的 buffer 挂上去。
      - 拿不到共享底衬（没开 `blur_enabled` / 这屏还没有底衬）就什么都不画，退回「只压暗」：
        遮罩还在，不会变成一块没压过的壁纸。退出全景时把缓冲放掉（4K 上两份 33MB）。
      - **证据**（`wall.py` 的金色方块，跨边缘扫一条线量最大跳变）：关掉 97（硬边）→ 打开 6；
        窗口本身还是纯红 / 纯蓝（没跟着糊）。`test-overview.sh` 43 → 50 项。

221. **桌面背景归谁画：兜底色 + 查询 + 事件**（用户：「背景允许其他软件配置 例如 swww 或 shell，需要一个方案」）

      - **契约没变**（vidow 不画壁纸，壁纸永远是 layer-shell `background` 层的客户端画的：
        swaybg / swww / quickshell / Noctalia / `tools/wall.py`），补的是三件缺口：
      - **兜底色 `background_color`**（`configs/18-background.lua`，默认 `#14161cff`）：新建一层
        `layer_backdrop` 垫在 `layer_background` **下面**，尺寸取整个输出布局的包围盒。以前谁都
        没画壁纸时屏幕是渲染器的清屏色（一块死黑），现在至少是底色。
      - **`vidowmsg background`**（别名 `wallpaper`）：报 `color` + `clients` + `surfaces[]`
        （background 层的完整 layer 对象）+ 每块屏 `covered`（有个 mapped 的 background surface
        铺满它）。常驻 shell 靠它决定「这屏要不要我上」。
      - **`background-changed` 事件**：background 层的 surface 上屏 / 下屏 / 没了各发一次，载荷
        就是上面那份状态（不是单个 surface）。IPC `watch` 和 `vidow.on()` 都能听。
      - **三个坑**：
        1. 退出时 `wl_display_destroy()` 会把输出一块块拆掉，每块都走一遍 `vidow_arrange()`，
           而那时 `wlr_output_layout` 已经销毁 —— 兜底色那次 `wlr_output_layout_get_box` 直接
           **SIGBUS**（gdb 里抓到的：`wlr_output_layout_get_box` ← `vidow_background_apply` ←
           `vidow_arrange` ← `output_destroy`）。加 `shutting_down` 那道门（跟 output.c 同一招）。
        2. 事件载荷是**当场现数**的：`layer-closed` 在 `wl_list_remove(&ls->link)` 之前发，那时候
           数 `clients` 会把这块已经死掉的 surface 数进去（事件说 1、查询说 0）。画壁纸的那一块
           改成摘链 + free 之后再发。
        3. 查询里 `covered` 要求 surface 是 **mapped** 且几何铺满整块屏：只看「有没有 surface」
           会把正在退出（已 unmapped）的那块算成有壁纸，shell 就不会去接管了。
      - **测试**：新增 `test-background.sh`（19 项）：没客户端时整屏是 `background_color`（量像素）、
        查询的 clients / covered / surface 几何、客户端退出后回到兜底色、`background-changed`
        上屏下屏各一次且载荷带 `clients`、`background_color` 热改（含 alpha 0）。
      - **Noctalia 的壁纸能用**（用户问）：查了正在跑的 Hyprland 会话，它的壁纸就是
        `namespace: noctalia-wallpaper` 的一块 **background 层** surface（每块屏一块、铺满整屏，
        `hyprctl layers` 里在 layer level 0）—— 跟 swaybg 走的是同一条契约，vidow 认。

220. **全景的遮罩颜色可配（`overview_dim_color`）**（用户：「overview背景也允许配置」）

      - 以前是写死的 `{0.02, 0.02, 0.03, 0.60}`。现在是一个带 alpha 的颜色键
        （`#05050899` = 默认），alpha 就是「压多暗」，`#00000000` = 不压暗（壁纸原样，
        缩小的窗口直接浮在上面）。遮罩还是夹在壁纸之上、窗口之下（`layer_overview_dim`），
        所以换什么色都不影响窗口本身。
      - 配置从 `overview` 这一族单开一份 `configs/17-overview.lua`（文件名只能往后加：
        14/15/16 早就写进用户的 config.lua 了）。
      - 壁纸本身**不**在这个键里 —— 那是 layer-shell 的 background 层，谁画都行
        （swaybg / swww / quickshell），见下面「背景（壁纸）归谁画」。
      - **测试**：`test-overview.sh` 26 → 43 项，补一轮「遮罩颜色」：拿一张只有壁纸的
        截图当基准，金色方块 255,217,51 —— 默认压到 106,92,28（43%）；`#00000000` 时
        253,215,51（原样）；`#ff000099` 时 253,87,20（染红）；三种配色下窗口里都还有纯红
        `255,0,0`（遮罩没盖到窗口上）。非法颜色被拒。

219. **修掉全景退出时的「闪弹一下」**（用户：「点选之后会闪弹一下」）

      - **原因**：动画引擎给的 `t` 永远是 0 → 1（`anim.c`），方向得由调用方决定。而
        `overview_update()` 直接把 `t` 当进度写进 `ov->t` —— 于是**退出 = 把「进入」重放
        一遍**：先跳回原样（t=0）、再缩下去（t→1）、最后 `overview_done()` 把它拍成 0
        啪一下弹回去。点选退出走的是同一条路（`vidow_focus_view` + `vidow_overview_exit`），
        而且这时候焦点已经换了列、镜头也在滚，看起来就是「闪弹」。
      - **改法**：`overview_update()` 按 `on` 决定方向、从 `ov->anim_from`（这次动画开始时
        的 `t`）插值到目标端；`overview_start()` 在 cancel 之前先把当前 `t` 记下来，所以
        中途反向（进到一半再按一次）也是从当前进度接着走；`overview_done(cancelled)` 不再
        拍端点（t / output 归紧接着的新动画管）。
      - **证据**（`window_move_ms = 600`，每 50ms 采一次 `vidowmsg overview` 的 t）：
        改前 Esc 之后 t 是 `1.00 → 0.00 → 0.09 → 0.26 → … → 1.00 → 0.00`（重放 + 跳变）；
        改后 `1.000 0.697 0.459 0.320 0.181 0.090 0.036 0.014 0.002 0.000`，点选那条路
        一样单调（`1.000 0.774 0.572 0.410 0.279 0.175 0.102 0.052 0.022 0.007 0.000`）。
        中途反向：`0 → 0.89 → 0`，单帧最大跳变 0.19（就是动画本身的速度）。
      - **测试**：`test-overview.sh` 补一轮「退出不反弹」（t 单调往下、有中间态、末尾收干净）。

218. **切换器的默认范围收成「当前工作区」**（用户问「mru是否按工作区显示」→ 选「默认改成
      workspace（只看当前工作区）」）

      - 范围一直是可配的（`mru_scope`：`all` / `workspace` / `output`，也能只给某个绑定换：
        `next-window scope=all`），但默认是 `all`。用户要的是「这块工作区里切窗口」，所以
        默认改成 `workspace`。
      - **测试**：`test-mru.sh` 57 → 60 项，补一轮「工作区范围」：在第 2 块工作区开一个 p6，
        回到工作区 1 开面板 → 默认（workspace）清单 5 个（不含 p6）；`set-config mru_scope
        all` 之后清单 6 个（p6 进来）。（这个范围之前一条测试都没有。）
      - 跨工作区（`scope = "all"`）时的顺序：按工作区序号分组、组内左→右 —— 当前工作区不保证
        在最前，README 里写清楚了。

217. **切换器的清单改成屏幕顺序（一进来选中的还是「最近用过」那扇）**（用户：「顺序保持和
      窗口顺序一致」→ 选的是「屏幕顺序排 + 一开始选中最近用过的那扇，之后按屏幕顺序走」）

      - **以前**：清单按 `mru_ts` 排（最近用过的在最前），所以面板里卡片的顺序和屏幕上窗口
        的顺序对不上 —— 卡片宽度和它正下方那扇窗口的宽度看着「反了」（其实各自都对，只是
        不是同一扇）。用户要面板当屏幕的小地图。
      - **现在**：`mru_build_order()` 用 `mru_cmp_screen()` 排 —— 先按屏（`wlr_output_layout`
        的 x）、再按工作区序号、最后按 `vidow_view_screen_box()` 的 x/y（左→右、一列里
        上→下）。`Tab` 的步进走的就是这份清单（到头绕回第一格）。
      - **一进来选中哪扇没变**：还是「最近用过」——`mru_pick_initial()` 现从
        `vidow_mru_collect()`（那份仍然是按使用时间排的）里挑第一个不是当前焦点的，
        `Alt+Shift+Tab` 从最旧的那头起。两个顺序分开：清单管排版 + 步进，`mru_ts` 管「先落
        在哪一扇」。
      - **测试**：`test-mru.sh` 55 → 57 项。第 2 轮加「卡片从左到右按屏幕顺序排」；第 4 轮
        改成「顺着屏幕走」：先把 5 张卡片收小到全在屏里（不然「滚到选中的可见」会把边上那张
        挤出清单，清单长度会跟着选中项变 —— 那是第 7 轮专门测的），然后断言清单 = `p1 p2 p3
        p4 p5`、初始选中 = 最近用过的 `p5`（清单最后一格）、再走一格绕回 `p1`。

216. **切换器的预览改走装饰管线（形状和真窗口一样）+ 悬停描边可配**（用户：「预览窗口
      形状要和原来窗口一样」+「鼠标放上去也要描边，比激活窗口描边淡一点（可配置样式）」）

      - **预览形状**：原来预览是「客户端那份 buffer 直接摆进框里」—— 方角、没边框、没阴影，
        和屏幕上的窗口长得不一样。现在改走**装饰管线**：`vidow_fx_frame()` 把窗口连装饰
        一起画进一张离屏图（缩到预览那么大）再摆进卡片，和屏幕上那扇窗口是同一条管线、
        同一份规则（`view->rule_eff`）—— 圆角、边框、阴影、连「焦点窗口边框亮一档」都一样，
        改规则两边一起变，这儿不另算一份。**只有模糊不一样：预览里不叠那层模糊**（缩略图
        那么小的一块，再垫一层按屏幕坐标抓的底衬，看着就是「窗口内容糊了」，见 246）。
        缩略图节点仍然留着当兜底：装饰关着
        （`decor_enabled = false`）或客户端还没上屏时走它。
      - **悬停描边**：原来悬停是「强调色 50% + `mru_card_border`（1px）」，和选中那张既
        不同宽也不同色、还偏细。现在两个键独立可配：`mru_hover_border`（默认 2，和选中
        同宽）、`mru_hover_color`（默认 `#3daee980` = 强调色半透明）——「点亮了、还没选中」
        这个区别靠**浓度**说，不靠粗细说。
      - **测试**：`test-mru.sh` 52 → 55 项。新增：预览角尖那点必须是切掉的（角尖暗、里面
        亮）—— 直接摆客户端 buffer 的话角尖就是内容本身，这条正好把两条路分开；悬停那张的
        描边量像素（`57 110 141`）+ 断言它比选中那张（`61 174 233`）淡。
      - 顺带核对了「预览大小和窗口对不上」那件事：量下来是**对的** —— 同一份缩放，v1/v2/v3
        三个窗口 `159x300 / 224x300 / 180x300`，各自除以自己的窗口宽（371/523/421）都是
        0.428。面板里 v2 比 v3 宽是对的（v2 的列宽 0.42 > v3 的 0.34）；看着「反了」是因为
        面板按**最近用过**排（v1 v3 v2），和屏幕上从左到右的 v1 v2 v3 不是一个顺序。

215. **切换器改成浮层：窗口一个都不动，面板里放缩略图**（用户：「2.mru应该是叠加显示的
      背景半透明 现在是把窗口缩小」+「三个都要 就是和niri的一样 mru是新的内容 不影响到原来
      的窗口」）

      - **以前**：切换器跟 overview 一样，真的把窗口**缩小摆进预览条 / 网格**（`vidow_mru_place()`
        改场景节点的位置和缩放）。窗口一被搬，原来那块屏就空了 —— 用户要的是「面板叠在上面，
        原来的窗口别动」。
      - **现在**：切换器自己有一层压在窗口之上（`mru.tree`，场景树最后建的 = 最上面），里面
        是压暗背景 + 卡片 + 缩略图；窗口的位置 / 尺寸 / 可见性一个都不碰。`vidow_mru_place()`、
        `vidow_mru_holds()` 连同 view.c 里那三处（摆位、停车、裁剪）一起删了 —— 「切换器不改
        排版」现在是结构上的事实，不是靠约定。
      - **缩略图**：直接摆**客户端那份 buffer**（`view->scene_surface->buffer->buffer`），场景拿
        它的纹理画，等于「把窗口内容缩小一份」—— 不抓图、不多一趟渲染，还是活的。只取
        `vidow_view_fx_geo()` 那套「可见窗口」的裁剪（不要客户端自己画的阴影留白），并且**不抄
        场景节点的 `src_box`**（那里面含「窗口被裁到屏里」那块，列排到屏外的窗口只剩一条，
        缩略图要整扇）。宽高比按 buffer 算，塞不下等比缩到框里居中。
      - **压暗层**：`mru_backdrop_alpha` 默认 0.8 → 0.6（半透明，桌面还看得见）；压暗层现在
        盖的是**窗口**（以前盖的是壁纸，因为窗口都被搬走了）。
      - **测试**：`test-mru.sh` 49 → 52 项，新增三条：① 面板开着期间每个窗口的几何一字不变
        （`windows` 的 `geometry` 前后对照）；② 卡片里那点是**全亮**的窗口内容（255 0 0）——
        既证明缩略图在画，也证明它画在压暗层上面；③ 卡片外面、窗口自己那块地被压暗了 ——
        证明面板确实压在窗口之上（而不是跑到窗口下面）。
      - 顺手：`mru_close` 里那两趟「把所有窗口重摆一遍」也没了（窗口没动过，没什么好恢复的）。

214. **修掉切换器卡片的「缺角」**（用户：「1.圆角缺角了」）

      - **症状**：卡片四角那段圆弧没画出来（四条直边还在），看着就是圆角被啃掉一块。
      - **原因一：角节点被误关掉**。wlroots 上传完纹理就把 `wlr_buffer` unlock 掉，锁归零 →
        发 `events.release` → `scene_buffer_handle_buffer_release` 把 `scene_buffer->buffer`
        清成 NULL（**纹理还留着，照样能画**）。而 `mru_buf_put()` 原来拿 `node->buffer == NULL`
        当「没图」的判据 —— 面板一开要刷好几次，第二次刷新就把角节点关掉了。改成拿卡片自己记的
        `card->fill_buf / ring_buf` 当判据（有没有图是卡片说了算，不是节点说了算）。
      - **原因二：半径比内边距大**。预览是个方块，圆弧一旦伸进预览那块区域就被窗口盖住。现在
        实际内边距 = `max(mru_card_padding, mru_card_radius)`（`mru_card_pad()`），半径 r 时圆弧
        正好贴着预览的角。默认半径从 16 收到 12（和窗口自己的 `corner_radius` 同值），内边距
        默认还是 8 → 实际内边距 12。
      - **测试**：`test-mru.sh` 补了 3 条**量像素**的断言（46 → 49）：圆角那段弧所在的
        `(cx, cy+6)`（直边矩形从 `cy+12` 才开始，所以这一点只有角节点能画）必须是描边色；
        角尖 `(cx, cy)` 必须是空的（既不是描边也不是底）。之前那 46 条只量了直边，这个 bug
        才溜过去。

213. **切换器的每一项改成 portal 那种卡片，并且能用鼠标点**（用户：「mru样式也调整一下，也改的像
      portal的item类似，且需要支持鼠标点击功能」）

      - **样式照 portal 的选择器**（`tools/vidowportal.c` 的 `card_draw`）：每项背后一张卡片 ——
        超椭圆圆角、一层淡底、一圈细描边。三种状态：**选中** = 底色混 12% 强调色（portal 的
        `accent_soft` 同配方）+ 强调色 2px 描边；**鼠标底下** = 强调色 50% 的 1px 描边、底不动
        （portal 的 hover 同此）；其余 = 卡片底 + 卡片描边色。默认调色板取 portal 深色那套
        （底 `#2f3336e6`、描边 `#3b4045ff`），强调色默认 `#3daee9`（KDE Breeze 蓝，和 portal
        从 `kdeglobals` 读到的那个同一个值）。
      - **排版按卡片算**：卡片 = 预览 + 内边距（`mru_card_padding`，默认 8），相邻卡片之间隔的
        还是 `mru_gap` —— 居中、「装不下就整体缩」、滚动可见性量的都是卡片的边，预览摆在卡片里
        缩进内边距。`mru_highlight_padding` 因此没用了（那张「板」就是卡片），删掉。
      - **圆角只能自己画**：`wlr_scene_rect` 是实心矩形，wlroots 0.20 的 `wlr_scene_buffer` 也没有
        `corner_radius`。所以四角各挂一个小节点，里面是**按覆盖率算出来的像素**（`|x/r|⁴+|y/r|⁴ ≤ 1`
        的 4×4 超采样，和 portal 的 `SQUIRCLE_N = 4` 同一种圆角），中间三条直边用普通矩形铺 ——
        九宫格：角不拉伸、边拉伸。四个角共用同一张图，靠 `wlr_scene_buffer_set_transform`
        翻过去（右上水平翻、左下垂直翻、右下转 180°）。
      - **像素怎么进场景**：节点只认 `wlr_buffer`，而 wlroots 没有「从 CPU 像素造 buffer」的 API
        （`wlr_texture_from_pixels` 给的是纹理，喂不进节点）。这里自己实现一个最小 `wlr_buffer`
        （像素在堆上，`begin_data_ptr_access` 指过去）—— GLES2 和 pixman 两条渲染器都是从这条路
        上传纹理的，和客户端递上来的 shm 缓冲同一条，不用 memfd、不用 GL 上下文、不碰渲染线程。
        角图很小（半径 × 半径），尺寸 / 样式没变就不重画；面板收起时把图放掉，下次显形重画。
      - **鼠标**：面板显形期间指针归切换器管（`vidow_pointer_motion` 里和 overview 同一处拦下，
        窗口都摆到预览位置上去了，命中坐标对不上，别转给客户端）。移动 = **点亮鼠标底下那一张**
        （只改样式、**不改选中** —— 不然「按住 Alt 时指针恰好停在某一项上」会莫名其妙把键盘选中的
        那个顶掉）；左键点一下 = **直接切过去**（不用等松修饰键），点空白什么都不做。点选那次的
        按下被吃掉了，对应的松手也一起吃掉（`mru.swallow_release`，和 overview 同一套）。
        悬停变化没有动画在推帧，得自己 `vidow_output_schedule_frame` 催一帧。
      - 命中按**卡片**算（`slot->cx/cy/cw/ch`，比预览大一圈）：圆角外面那点内边距也算点得中，
        和 portal 的卡片一个手感。IPC 的 `mru` 多输出 `hover`、每项多 `cx/cy/cw/ch`。
      - 判据：`./test-mru.sh` 从 32 项加到 46 项 —— 新增「卡片 = 预览 + 内边距（每边 12）」「相邻
        卡片不重叠」「选中那张的描边是强调色 61,174,233 / 没选中的是卡片描边色 59,64,69（都是量
        像素）」「鼠标悬停点亮指针底下那张且选中项不动」「挪到空白处悬停清掉」「点哪张卡片就切到
        哪个窗口 + 面板当场收起 + 松手被吃掉」。相关回归：`./test-defaultconfig.sh`、`./test-config.sh`
        （`mru_highlight_padding` 删了、`mru_card_*` 五个新键、默认配置重新生成）。

212. **列宽变化也走过渡动画**（用户：「列宽变化的时候也需要过渡动画」）

      - **补间的是「画出来的宽度」，不是逻辑值**：列上多了 `width_shown`（这一帧拿来算像素）
        和 `anim_width`（在跑的补间），`width_fraction` 一直是逻辑目标。`arrange` 的 Pass 1 读
        `column_layout_fraction(col)` —— 补间在跑就是中间值。这么分是因为「现在是第几档」得按
        **目标**数：连按 `cycle-column-width` 要一档一档往上走，不能从画到一半的地方数。
      - **一个入口**：`vidow_column_set_width(col, fraction, animate)`，`cycle-column-width` /
        列最大化 / `set-column-width` / `expand-column-to-available-width` 全走它；
        `animate = false` 那一档**当场 `vidow_arrange`**（不是只挂 `arrange_dirty`）—— 没有过渡
        就没有动画在推帧，静态屏上下一帧可能几百毫秒后才来。
      - **拖动改宽（Mod+右键拖）不补间**：`animate = false`，跟手是硬要求（顺带把还在跑的过渡
        掐掉，不然它接着往旧目标走）。每个指针事件的重排次数没变（重排由 setter 做掉，drag.c
        不再单独调一次）。
      - **过渡期间不给隔壁列起位移补间**（`server->column_anim_n`，见 `vidow_place_view`）：列一变宽，
        右边那几条列每帧都在动，而 `vidow_place_view` 对「位置变了」一律起一条 `window_move_ms`
        的补间 —— 每帧重起一次的话它们永远吊在过渡后面、过渡结束后再自己飘到位。过渡期间直接
        落位，跟着宽度的曲线走（整条带子刚性跟着变）。收尾那一帧的 1px 取整会照常起一条补间，
        那是布局重排自己的事。
      - **配置**：`column_width_ms`（默认 200，`anim = { column_width = … }` 也认），`0` = 直接
        到位；`easing` 复用布局那条（默认 `out_cubic`，不过冲 —— 宽度过冲会把右边那几条列顶出去）。
      - **IPC 语义**：`width_fraction` 当场就是新值，`width`（像素）是**过渡中间值**，要终值得等
        `column_width_ms`（`./test-ipc.sh` 那条断言因此加了 0.4s 等待，`./test-maximize.sh` 本来
        就带 sleep）。
      - 判据：`./test-widthanim.sh`（新，30 项断言）—— 默认 200ms 量到 10 个中间宽度、单调、
        191ms 到位、停在目标像素 836、右边那条列每一帧 `offset = 宽 + gap`、过渡期间只有
        `column-width` 一个动画；`column_width_ms = 0` 一个中间值都没有、`= 500` 真的走 452ms；
        列最大化 / 还原同一条路。相关回归：`./test-maximize.sh`、`./test-ipc.sh`、
        `./test-tile-resize.sh`、`./test-defaultconfig.sh`、`./test-config.sh` 全过。
      - 顺手补两处**上一批改动漏下的测试漂移**：`./test-ipc.sh` 的 `state` 键表少了 `gestures`；
        `./test-defaultconfig.sh` 的「说明完整性」缺手势的 `distance`（在 `14-keys.lua` 的说明里
        补一句，默认配置跟着重新生成）。

211. **选择器那条分段控件收小**（用户：「portal的tab缩小一点，不要占太多地方」）

      - 原来是 `padding: 14px 44px` + `min-height: 22px`：整条 ≈50 高、一段 ≈197 宽（三个 tab 时
        ≈570 宽），比正文的卡片还大。现在 `8px 22px` + `min-height: 18px` + 字号 12：整条 ≈34 高、
        一段 ≈81 宽。
      - **光调 padding 收不下去**：主题给 `stackswitcher button` 留了约 100px 的 `min-width` 地板 ——
        padding 44 / 30 / 10 三档都量过，每段一律卡在 ≈160 宽。得在 CSS 里 `min-width: 0` 把它按掉，
        宽度才真的跟着 padding 走。
      - 高亮块（`tab_hl`）不用改：它是**相对按钮**内缩 `TAB_HL_INSET` 画的，按钮变小它跟着变小。

210. **绑定系统重做：鼠标按键 / 滚轮 / 单独一个修饰键 / 松开触发 + 触控板手势**（用户：「指针手势
      也要允许配置 快捷键允许单独一个修饰键（看看什么方案比较好）」，补一句「和普通键位绑定一样，
      只不过加入鼠标按键」+「一起做连续版，顺便所有键位绑定的逻辑改一下，分为按下触发和释放触发」）

      - **一张表收三类触发**：`vidow.bind` 现在收键盘键、鼠标按键（`mouse-left` / `mouse-middle`
        / `mouse-back` / `mouse-forward`，也认 `BTN_LEFT` 和编号）、滚轮（`wheel-up/down/left/right`）
        —— 动作字符串还是同一套（`vidow_action_run`）。**`left` / `right` 不能拿来写鼠标键**：
        键名表里它们已经是方向键了，直接改会悄悄改掉方向键的绑定。
      - **两种触发时机**：`{ on = "press" }`（默认）/ `{ on = "release" }`。松开触发的那条连
        按下那一下也一起吞（只吞松开的话客户端会以为键一直按着）；按下时把命中的那条记在
        `pending[]` 里，松开时按**按下那一刻**的修饰键掩码再匹配一次。
      - **单独一个修饰键**（`vidow.bind("Mod", …)`）：**tap 语义** —— 按下武装、期间按下任何别的
        键或鼠标键就作废、松开且其它修饰键都松了才触发。为什么不能「按下就触发」：`Super+Q` 会
        顺带把「只按了 Super」那条也跑掉。`Super_L` / `Super_R` 可以只认某一侧。默认时机是
        `release`（单独一个修饰键写 `press` 没意义）。
      - **三个指针手势改成「动作 + 绑定」**：原来写死在 `input.c`（`MOD_KEY` + 按钮硬编码）的
        `Mod+左键拖` / `Mod+右键拖` / `Mod+滚轮` 现在是两个新动作 `interactive-move` /
        `interactive-resize`（目标 = **光标底下那个窗口**，浮窗改几何、平铺拖动 / 改列宽）+ 滚轮换列
        用现成的 `focus-column`，默认配置里给了这四条绑定。**没绑就没有**（和键位同一条规矩），
        没绑的滚轮照旧转发给客户端。测试注入的 `pointer-button … mod`（force_mod）现在参与绑定匹配
        —— 测试环境没有真键盘，拖拽要求按着 Mod。
      - **触控板手势**（`vidow.gesture{}`，新文件 `src/gesture.c`）：`swipe = "view"`（**连续**：
        手指拖着条带走，松手按拖了多远吸附到相邻那条列 / 弹回原位）、`direction = …`（离散，松手按
        主方向触发，阈值 `distance`）、`pinch = "in|out"`、`hold = true`。挂在 `wlr_pointer` 的
        swipe / pinch / hold 信号上（libinput 只对触控板发这些）。连续那条的关键是 `arrange` 的
        Pass 2：手指拖的时候视口由 `output->gesture_viewport` 说了算，**不跑**「把焦点列滚回可见区」
        那套（不然刚把焦点列拖出屏幕就被它拽回来），松手再用 160ms 缓动滑到「不做手势时该在的位置」。
        位移单位是 libinput 的归一化单位（1000dpi），`speed` 默认 0.3 = 手指 1cm ≈ 屏幕 118px。
      - **注入钩子**：`vidowmsg gesture swipe-begin 3` / `swipe-update 3 dx dy` / `swipe-end [--cancel]`
        / `pinch-begin|update|end` / `hold-begin|end` —— 直接调 gesture.c 的同一批函数（这台机器上
        没有触控板，真机手势没法量）。`vidowmsg state` 里多了 `gestures` 数组。
      - 验证：`./test-gesture.sh`（5 轮 21 项）—— 没配置就没手势（注入了也不跑动作）、四指上扫够远
        才触发 / 方向没绑不算 / 太短不算 / cancelled 不算、捏合 scale 过阈值才算、按住当场触发、
        三指横扫拖动中视口跟手（1270 → 910）且焦点不动、松手吸附到相邻那条列（焦点换成前一条、
        视口停在它的左边缘）、太短和 cancelled 都弹回原位；`./test-binds.sh`、`./test-defaultconfig.sh`
        照旧全过（11 个吃写死手势的测试配置里补上了那四条绑定）。

209. **选择器两个虚线框**（用户：「第一个tab存在一个奇怪的虚线框，选中item的时候item的名字还预览图
      一开始的时候会有两个单独的高亮」）

      - **键盘焦点的虚线框**：这就是用户看到的那两个现象 —— Adwaita 的焦点指示器是**虚线**，
        出现在卡片 / tab 上；而「选中的卡片」本来就有强调色 2px 描边 + 底衬淡染，两者叠在一起就是
        「两个单独的高亮」。现在：卡片 / tab 的焦点**不画**（选中态本身就是指示），未选中的卡片给
        2px 强调色实线（`outline-offset: -3px`），底栏「取消 / 共享」给 2px 强调色实线（-7px，
        它们没有别的焦点指示）。
      - **占位虚线框叠在真缩略图上**：`region_row_apply()` 只把 `placeholder` 这个 class 摘掉，
        而虚线是 `placeholder_draw` 这个 draw 回调画的 —— 摘 class 摘不掉回调，于是框好区域之后
        真缩略图上还留着一个虚线框（用户报的第一个现象）。改成 `placeholder_draw` 自己问一句
        `gtk_style_context_has_class()`，没有 class 就不画（自守，不用管谁挂的谁摘的）。
      - 验证：headless + grim —— 连按 Tab 逐帧比：焦点落在**未选中**卡片上 → 出现强调色实线框
        （与「点完」那帧差 1420 像素，落在虚拟屏幕那张卡上）；落在**已选中**的卡片上 → **0 像素变化**
        （不再叠第二个高亮）；对话框刚出现那帧干净（没有虚线框）。框选完成那张卡的截图：虚线框没了，
        只剩真缩略图。`test-portal-own.sh` 全过。

208. **选择器：去掉对勾、tab 再胖一点、切 tab 加过渡动画**（用户：「选中的勾去掉。tab的padding
      再大点，切换的时候给个过渡动画」；上一轮还说「圆角改大一点 先试试两倍」）

      - **选中对勾删了**：缩略图右上角那个 ✓（`GtkOverlay` + `label.badge`）整块去掉 —— 选没选中
        只看卡片自己（强调色 2px 描边 + 底衬淡染）。`struct row.badge`、那条 CSS 规则、`on_toggle`
        里的显隐都清了，卡片里的图不再包一层 overlay。
      - **高亮块内缩（外框不动）**：用户第三轮说清楚了 —— 「外框大小不变，高亮变小啊，padding 啊，
        不是让你把高亮里面 padding 大」。所以按钮的 padding（`stackswitcher button`）**保持 `14px 44px`
        不动**（外框 570 宽 / 每段 188 就定在这儿），高亮块自己在按钮框里**四边内缩 `TAB_HL_INSET`
        = 5px**（`tab_hl_rect()` 里统一缩，动画的起点 / 终点都走这条路）→ 高亮 178x41，外框和它之间
        留出 8px（3px 控件 padding + 5px 内缩）、相邻两段之间 10px。悬停底色同一圈内缩（`tab_draw`），
        不然悬停是满格、选中是内缩，两副样子。**要改的是这一个数**（`TAB_HL_INSET`），不是 CSS 的
        padding —— 那个动一下外框就跟着变。
      - **过渡动画改在高亮块上**（用户第二轮：「切换动画是tab高亮，不是item」）：先把上一版加的
        页内容滑动（`GtkStack` `SLIDE_LEFT_RIGHT`）撤掉，回到 `TRANSITION_NONE`；高亮块改由**分段
        控件的容器**画 —— 容器先画、按钮后画，所以高亮正好压在文字底下（`switcher_draw` 里先填外框
        再填高亮块；`tab_draw` 从此只画悬停底色）。换页时 `notify::visible-child` → 记下起点 / 终点
        → `gtk_widget_add_tick_callback` 逐帧插值（200ms，ease-out cubic），到点收工。不在动画里时
        每帧直接跟真实布局走（窗口改尺寸、第一次画都算这种）。
      - 踩的两个坑（都在坐标上）：① `gtk_widget_get_allocation()` 拿到的按钮矩形**不在父容器坐标系
        里**（容器才 57 高，`allocation.y` 却是 102 → 高亮块画到容器外面，看着就是「高亮没了」），
        得 `gtk_widget_translate_coordinates()` 翻一遍；② 别只在「第一次画」时取一次坐标，那次可能
        还在布局中间，取到的是中间态 —— 改成不在动画里就每帧刷新。另外给分段控件挂了 `destroy`
        回调把 tick 回调和高亮块状态清掉。
      - **圆角按 2 倍调**：缩略图 5 → 10，卡片 / 按钮 / tab 8 → 16，标题栏那个 × 6px → 12px。
        注意 `squircle_path` 会把半径夹到 `h/2`：底栏那两个按钮只有 ~34px 高，16 已经到顶 → 它们
        现在是完全的胶囊形，再调大不会有变化（要更方得往下调）；tab 高亮块 51px 高，16 还没到顶。
      - 验证：headless + grim —— 点一下「窗口」tab 后连抓 5 帧，量高亮块的 x：37 → 93 → 154 →
        193 → 214 → 225（停稳），单调收敛、宽一直 176；中间那一帧能直接看到高亮块骑在「屏幕 / 窗口」
        中间，而下面的内容**已经整页换完**（要的就是这个：动的是高亮不是 item）。停稳帧与「把光标
        移开」那一帧逐字节相同 —— 顺带排掉一个假警报：未选中 tab 那层灰不是 hover 残留，是分段控件
        自己的底色。浅色 / 深色各截一张；`test-portal-own.sh` 连跑两遍全过（64 条；tab 落点在
        `y + 97`，而高亮块现在是 `y + 82..132`，还在里面）。

207. **选择器再收两处**（用户：「选择区域可以直接点item，没必要单独一个按钮」+「UI的圆角用超椭圆，
      tab高亮加点padding」）

      - **「区域」那张卡自己就是按钮**：点它 = 去框一块（还没有区域时）/ 取消选中（已经有区域时）；
        还没框成就按 Esc 的话，这张卡跟着取消选中 —— 点它的本意是「选它」，没框成不该算选上
        （不然「共享」会为一个空区域亮着）。单独那个「选择区域…」按钮删了。
      - **圆角换成超椭圆（squircle）**：卡片 / 按钮 / tab 的底和虚线占位框都是自己用 cairo 画的
        （`squircle_path`，n=4；GTK 的 `border-radius` 只会画圆弧），CSS 里对应的背景和边框让开
        （透明 + 无边框），缩略图的圆角也走同一条路径。悬停 / 选中 / 禁用三种状态在各自的 `draw`
        里填色（`card_draw` / `pill_draw` / `switcher_draw` / `tab_draw` / `placeholder_draw`）。
      - **tab 高亮加了 padding**：`stackswitcher button` 从 `5px 16px` 放到 `8px 22px`（高亮那块
        跟着长），外框 `padding` 2px → 3px；未选中的 tab 补了悬停底色。
      - 顺手：CSS 里那些不再用到的调色板参数去掉了（`g_strdup_printf` 的位置参数改成顺序参数，
        之前删一条规则就得重新编号）。
      - 验证：headless + grim 逐版截图（浅色 / 深色 / 悬停 / 选中 / 三个 tab / 框选浮层 / 框完的卡片）；
        `test-portal-own.sh`（区域那轮 8 项，落点改成「点卡片」）、`test-screencast.sh` pixman + gles2、
        `test-portal-queue.sh` 全过。

206. **选择器分三个 tab：屏幕 / 窗口 / 区域**（用户：「屏幕/窗口/区域 共享分为三个tab」）

      顶上一条分段控件（`GtkStackSwitcher`，选中那段填强调色），下面一页一个网格；**只列应用真的
      在要的那几种来源**，只剩一个 tab 时那条控件收起来；虚拟屏幕并进「屏幕」页（它本来就是「再来
      一块屏」）。三页高度对齐（`vhomogeneous`），切 tab 时对话框不会一跳一跳。

      **「区域」页**：点「选择区域…」铺一层全屏透明浮层，在**正在动的画面上**拖一个框 —— 用户要的
      「点区域选择拖框 但不要冻屏」，所以浮层是 RGBA + CSS `.transparent`（画上去的只有框外那层压暗、
      框线和尺寸标签），底下照常刷新（headless 里量过：浮层挂着时动一下光标，两帧不一样）。
      松手记下这块区域、卡片上换成那块区域的**实时缩略图**（`capture_output_region`），按「共享」录它。
      坐标口径：`stream_region` 要**布局逻辑坐标**（相对那块屏 + 那块屏在布局里的位置）、scale 一起
      给过去，报给前端的 `size` 是**像素**尺寸。

      **顺手挖出来一个真 bug**：选择器报给前端的 `position` 一直是 0,0 —— 它读的是
      `wl_output.geometry` 的 x/y，而 wlroots 那对**恒为 0,0**（`wayland-info` 实测两块屏都是 0,0，
      而 xdg-output 的 `logical_position` 是 0,0 / 1280,0）。现在按标准读 `xdg-output`（新 vendor 一份
      `protocols/xdg-output-unstable-v1.xml` + meson 生成客户端代码），录屏 position 和框选区域的
      坐标都对上了。**这个 bug 以前就影响显示器那一路的 position**（第二块屏录出来位置是 0,0）。

      验证：`test-portal-own.sh` 新增 6c2 那一轮 8 项（tab 切得过去、浮层弹出来/关得掉、区域坐标
      120,100 500x300、合成器收到的是布局坐标 1400,100 500x300、报给前端的 size 1000,600 = ×2 缩放、
      真出帧），外加 6a 新增一项钉住 position（1280, 0）；全套仍只剩那条老账（`光标位置不对`，改之前
      就红）。headless + grim 逐版截图：三个 tab、框选浮层、框完的卡片、深色版都看过。

205. **选择器换成 Breeze 那一套样子**（用户：「portal 样式弄得好看一点现代一点，更接近 kde 一点」）

      配色不再吃 GTK 主题的默认灰：底 `#eff0f1` / 白卡片 / 强调色取 KDE 的
      `[Colors:Selection] DecorationFocus`（真机上是 `#3daee9`），深色主题自动换一套；卡片圆角、
      悬停描边、选中 = 强调色 2px 描边 + 底衬淡染 + 缩略图右上角一个对勾（后来去掉了，见 208），
      缩略图自己切圆角再描一圈
      细边；标题栏换成 headerbar（标题 + 「谁在要录屏」副标题）；底栏一条分隔线 + 灰色「取消」+
      强调色「共享」；虚拟屏幕那张卡补上虚线框 + 显示器图标占位（以前是一块空白）。样式全在
      `GtkCssProvider` 的 CSS 里，结构那边只挂 class 名，没有新依赖。

      顺手两件事：**对话框高度 = 内容高度**（滚动区把内容的自然高度顶给窗口，装不下才滚动）——
      以前固定 520 起步、底下留一大块空白，而且 `max_content_height` 只要比内容大就会把窗口撑高；
      交互式截图的框选也统一到强调色（选区外压暗一层、边框强调色、左上角一个「宽 × 高」标签）。

      调试口子：`VIDOW_PORTAL_TYPES=1|2|4|7` 只看某一类来源、`VIDOW_PORTAL_PARENT=wayland:<handle>`
      当挂到应用窗口上（handle 从 `tools/foreign-app.py` 拿）。**注意**：没父窗口时合成器把选择器当
      普通平铺窗口、会被铺满一整列（尺寸不归它定）—— 真机上它带父窗口，是浮窗。

      验证：headless + grim 逐版截图（浅色 / 深色 / 浮窗 / 框选都看过）；`test-portal-own.sh` 全过
      （含「双击第一张卡片的预览直接确认」，落点没跑）、`test-screencast.sh` pixman 11 项、
      `VIDOW_TEST_RENDERER=gles2 ./test-screencast.sh` 13 项、`test-portal-queue.sh` 全过。

204. **录屏 dmabuf 直通（零拷贝）：主线程一帧 7.6ms → 0.33ms**（用户：「hyprland portal 录屏
     就不会这样，肯定哪里代码写错了」）

      用户说得对，203 那个「dmabuf 做不了」的结论错了。真正的错在两处：

      1. 缓冲得由**生产者**分配（`PW_STREAM_FLAG_ALLOC_BUFFERS` + `add_buffer` 事件里把内存
         填进去）。默认那条路是**消费者**分配：它建池那一刻就把内存包死了（GStreamer 的
         `gstpipewirepool` 只 wrap 一次：`wrap data (MemFd 2) 4096 3686400`），我们之后每帧往
         `spa_data` 里塞什么 fd 它都看不见 —— 实测塞 dmabuf 出去，消费者读的还是它自己那张
         memfd，六帧全黑。而且**一个槽得一张、fd 从头到尾不变**（消费者只认建池时包进去的那张）。
      2. 得让抓帧渲染进一张 **linear** 缓冲：报 `SPA_FORMAT_VIDEO_modifier` 会把 gst 的 caps
         协商整死，不报时消费者的默认假设就是 linear（modifier 0）；而抓帧自己那张是 tiled +
         DCC 两平面的（实测 `mod=0x200000028a6bf04 n=2`），交给消费者它也没法 import。

      做法：`vidow_grab_target_new()`（capture.c）建一张 linear XRGB8888 交换链；
      screencast 给每个 PipeWire 缓冲槽配一张（`struct vidow_sc_slot` —— 一槽一张、fd 不变），
      抓帧渲染进它，fd 在 `add_buffer` 里交给消费者；**交出去之前 `vidow_grab_sync()` 等 GPU
      把这一帧画完**（消费者多半是 mmap 下来在 CPU 上读，它没法知道 GPU 写完没有 —— 不等就是
      读到还没画进去的那张，实测全白）。消费者不认 dmabuf 就自己建一张 memfd + mmap 回读进去；
      连 linear 交换链都建不出来（`WLR_RENDERER=pixman`，缓冲是 shm）完全退回老路。
      `VIDOW_SC_NO_DMABUF=1` 强制走回读（A/B 对比用）。

      - `vidowmsg state` 的 `screencast` 多两个字段：`sync_ms`（等 GPU 那笔）和
        `dmabuf_frames`（直通发了多少帧，> 0 才算这条路活着）；`readback_ms` 只摊在真回读过
        的帧上（直通帧不摊进去，不然平均值会假的好看）。
      - `test-screencast.sh` 多一个渲染器开关（`VIDOW_TEST_RENDERER=gles2`）和 3 条直通断言，
        并把消费者的帧率限到 5fps —— 不限速时帧是**爆发式**的（消费者一还缓冲我们就抓一张），
        六帧全在同一瞬间抓，「屏幕变了帧跟着变」那条断言就成了掷骰子（gles2 下必红）。pixman
        那条路（默认，不需要 GPU）照样全过，11 项。
      - 实测（headless 真 GPU，1280x720 的流）：`readback_ms` **0**、`sync_ms` **0.33ms**、
        `dmabuf_frames` 6/6 —— 也就是主线程每帧只剩「等 GPU 把这一帧画完」这一笔（2560x1440
        上回读那笔本来是 6.2ms）。

203. **录屏的一帧成本量出来了：回读是大头**（用户补充「我说的录制掉帧是整个桌面，我观察的
     是光标」）

      202 之前那条「发帧节奏」只解决了**次数**（以前每次输出提交都抓一整帧），单次成本没量过。
      这一条把一帧拆成两半量出来，`vidowmsg state` 的 `screencast` 里多三个字段：

      | 字段 | 是什么 |
      | --- | --- |
      | `grab_ms` / `grab_ms_max` | 整块离屏渲染（把场景画进抓帧缓冲）的平均 / 最大耗时 |
      | `readback_ms` | 从抓帧缓冲回读到 PipeWire 缓冲（GPU→CPU 同步拷贝）的平均耗时 |
      | `sync_ms` | dmabuf 直通那条路上「等 GPU 把这一帧画完」的平均耗时 |
      | `dmabuf_frames` | 走 dmabuf 直通（零拷贝）发出去的帧数，> 0 才算直通是活的 |

      - 实测（真 GPU、headless 2560x1440 的流）：**回读 6.19ms、渲染 1.41ms** —— 拷贝是渲染的
        4.4 倍，4K 上按面积算回读 ~14ms。「录屏时整个桌面（光标）都顿」剩下那部分就是它。
      - 这一版试过 dmabuf 直通，当时判断「没做成」（PipeWire 这层的缓冲是**消费者**分配的，
        我们报 `SPA_DATA_DmaBuf` 之后消费者的分配器建不出来：`error alloc buffers: 不支持的操作`）。
        **那个判断是错的**，204 里做成了 —— 缓冲得由**生产者**分配（`ALLOC_BUFFERS`）。两条
        仍然成立的结论：缓冲必须生产者分配、`SPA_FORMAT_VIDEO_modifier` 不能报。
      - 顺带一个坑：给 EnumFormat 报 `SPA_FORMAT_VIDEO_modifier` 会让 GStreamer 的
        pipewiresrc 协商不出固定 caps（`handle_format_change` 断言失败、一帧都拿不到），
        所以**不报**（不报 = 修饰符 0 = LINEAR）。

202. **拖到右边 / 空白处也能把窗口从列里取出来**（用户报「窗口放出需要在右边、或者没窗口的
     地方也能实现」）

      201 把「取出来」做成了动作 / IPC，但**拖拽**那条路还是老样子：落点算成「新开一条列」
      的时候搬的是整条列 —— 所以把一个窗口从两窗口的列里往右一拖，整条列都跟着走了，窗口
      还留在列里，「拖出去」这个手势根本做不到。

      - `vidow_view_expel_new_column(view, ws, at, reason)`（`view.c`）：201 那套「拎出来
        单独成列」拆成可复用的内核，插在列带里 `at` 那个位置之前；`vidow_view_expel_from_column()`
        （动作 / IPC 那条路）变成它的一层薄壳。拖拽落点和动作走的是**同一份**「第几位」数法。
      - `drag.c`：落点是「新开一条列」而这条窗口在一条**多窗口**的列里 → 把它自己取出来成
        新列（niri 拖的一直是窗口），原来那条列连同别的窗口留在原处；列里就它一个 / 是浮窗
        → 取不出来，照旧整条列搬过去（老行为没坏）。跨屏 / 跨工作区那条路不变。
      - 整条列换位置还有 `move-column left|right`（`Mod+Shift+H/L`），不是只能靠拖。
      - 判据：`test-expel.sh` 新增 6 条（拖到最右边那片空白 → `[a1] | [a3] | [a2]`、日志是
        「从列里取出来」；列里就它一个时拖 → 整条列搬到最右边、日志是「挪到第 3 位」）。

201. **窗口能从列里取出来**（用户报「窗口放入列后无法取出」）

      并进一列有两条路（拖拽落点 / `move-window-direction`），取出来以前一条都没有 —— niri 的
      `expel-window-from-column` 在 vidow 里是缺的，README 里还把它写成「故意跟 niri 不一样」。

      - `vidow_view_expel_from_column(view, direction)`（`view.c`）：把窗口从它那条列里摘出来、
        单独成一条新列，`direction > 0` 插在原来那条列右边（niri 的默认），`< 0` 左边。
        新列继承原来那条列的 `width_fraction`（取出来是为了单独挪它，尺寸突变没有意义），
        **不带**列最大化状态（新列不该一出生就满宽）。
      - 列里只剩它一个（那它本来就是单独一条列）、浮窗 / 弹出物、收起来的窗口 —— 这几种返回
        false，调用方报一句人话（不是默默什么都不做）。
      - 动作 `expel-window-from-column [left|right]`（别名 `expel-window`）+ 默认键位
        `Mod+Period`（取到右边）/ `Mod+Shift+Period`（取到左边）+ IPC
        `expel-window-from-column`（`direction` 不写 = 右边）。
      - 判据：新测试 `test-expel.sh`（26 项）：并起来 → 取出来（右边 / 左边 / 宽度继承 / 焦点
        不换 / 日志）→ 两种取不出来的情形报人话且结构一点没动 → 动作串那条路。

200. **图层规则补齐：开合动画、动画时长、不透明度**（「覆盖 vidow 所有能改的地方」的收尾）

      197–199 之后，图层规则（`namespace` / `layer`）能改的只有 `capture` / `capture_mosaic` /
      `blur` 三项 —— 面板的开合动画和不透明度还是全局的（`layer_open_ms` / `layer_close_ms`）。
      这一条把最后那块补上：

      | 作用 | 窗口 | 图层 | 怎么落地 |
      | --- | --- | --- | --- |
      | `anim` | ✅ | ✅ | `false` = 开、关都瞬间到位（时长按 0 算） |
      | `open_ms` | ✅ | ✅ | 覆盖 `window_open_ms` / `layer_open_ms`；`0` = 不做开启动画 |
      | `close_ms` | ✅ | ✅ | 覆盖 `window_close_ms` / `layer_close_ms`；`0` = 不做关闭动画 |
      | `opacity` | ✅ | ✅ | 窗口走装饰着色器（`u_opacity`）；图层走场景节点 alpha（`vidow_fx_set_alpha`） |

      - 算「这个目标该用多长」只有一处：`vidow_rule_anim()`（`rule.c`）—— 全局时长打底、
        规则的 `open_ms` / `close_ms` 压上去、`anim = false` 把两个都归零，然后负值夹到 0。
        调用点两处：`view.c` 的 `vidow_view_open_anim` / `view_snapshot_close`，
        `layer.c` 的 `layer_open_anim` / `layer_snapshot_close`。**「时长 ≤ 0 = 不做动画」
        是配置里本来就有的一条路**（`window_close_ms = 0` 就是这么关的），规则只是接上它，
        没有第二条代码路径。
      - `open_ms` / `close_ms` 顺带也给了窗口：`window_open_ms` / `window_close_ms` 原本
        只有全局键，现在能逐窗口调（`anim = false` 是它的粗粒度版本，两个都留着）。
      - 图层的不透明度乘在**它自己的内容**上，**底衬那份模糊不跟着淡**：`fx` 节点是这块
        场景树的子节点，`vidow_fx_set_alpha()` 会连它一起乘，所以 `arrange_layers()` 乘完
        再按 `fx->masked` 把它放回 1.0 —— 半透明面板透出来的应该是糊过的背景，不是没糊的
        壁纸（窗口那边 `u_opacity` 也只乘内容 + 边框，blur 那一项不乘，语义对齐）。
      - 关窗 / 关面板的快照也认 `opacity`：装饰着色器那条路自己会乘，没走着色器的
        （图层快照、关掉装饰的窗口）在 `closing_update()` 里乘一次 —— 不然「规则说半透明」
        的窗口，关闭动画却是全不透明地淡出。
      - **顺手修掉一个真 bug**：`anim = false` / `open_ms = 0` 的窗口以前会连 **fx
        （装饰 / 模糊那条路）都不建** —— 边框和背景模糊都挂在 fx 节点上，不建就是「这个
        窗口没有边框、也永远不模糊」，跟用户写的那句「不做动画」不是一个意思。现在「不做
        动画」走 `view_open_done()`（没有入场动画时的开窗收尾：换 idle shader、摆到位），
        fx 照建。`window_open_ms = 0` 本来走的就是这条路 —— `test-blurpartial.sh` 拿它当
        默认配置跑，正是它把这件事抖出来的（改坏的那一版，所有窗口的私有底衬一次都没抓过）。
      - 判据：`test-rules.sh` 第 8 节（图层 `anim = false` 当场没有 `anim-open`、`open_ms`
        真的写进 `anim.duration_ms`、`close_ms = 0` 不出现关闭快照、`opacity = 0.3` 拿 grim
        量 dot 那个白点的蓝通道 255 → 76）+ 第 6c 节（规则给红边框 + `anim = false`，
        量窗口左边缘往里 2px：R=255 G=0 —— fx 没建的话那里是客户端自己的蓝）+ 第 2 节加
        `open_ms = -1` 越界校验。

199. **规则能改外观**（「覆盖 vidow 所有能改的地方」里的装饰着色器那一块）

      198 把布局 / 行为那些作用做完了，剩下的是「这个窗口长什么样」—— 圆角、边框、
      阴影、噪点、不透明度、背后模不模糊，这些原本只有全局配置键（`configs/02-decor.lua`、
      `03-blur.lua`）。现在每个窗口都能单独盖：

      - 新作用 13 项：`decor`、`opacity`、`corner_radius`、`border_width`、
        `border_color`、`border_color_focus`、`shadow_radius`、`shadow_color`、
        `shadow_offset_x`、`shadow_offset_y`、`noise`、`blur`、`blur_xray`。
      - 合成一份「现在长什么样」（`struct vidow_look` + `vidow_rule_look()`，`rule.c`）：
        全局配置打底、规则压上去。渲染（`fx.c` 的 `decor_draw` / `view_wants_blur` /
        `layer_blur_wanted`）和 IPC（`vidowmsg windows` 的 `look`）用的是同一份 ——
        「规则有没有生效」不用去量像素。
      - **全局开关是总闸**：`decor_enabled = false` 时规则写 `decor = true` 也开不回来
        （关装饰多半是为了性能 / 兼容，被一条规则悄悄打开更意外）；`blur` 同理。
      - `shadow_radius` / `shadow_offset_*` 会改离屏装饰缓冲的内边距（`vidow_fx_pad`），
        所以它按「全局值」和「所有规则里最大的那个」取大 —— 多留一圈只是缓冲大一点，
        少留就是阴影被裁掉。
      - 关窗动画的快照也带上一份规则作用（`vidow_closing_start` 多一个参数）：不带的
        话，关窗动画一出来装饰就变回全局配置的样子。
      - 判据：`test-rules.sh` 第 6 节 15 项（每一项都拿 `look` 对：单项改、全局压规则、
        规则压全局）+ 一条**真的量像素**的判据（`opacity = 0.3` 时窗口中心的蓝通道
        255 → 97，用 grim 抓帧 + PIL 读）；第 2 节另加 `opacity = 2` /
        `corner_radius = -3` 两条越界校验。

198. **规则的作用覆盖到 vidow 能改的地方**（用户要求「规则允许修改的项目覆盖 vidow 所有能改的地方」）

      197 只做了匹配那一半。这一条把「一条规则能对它做什么」从 8 项扩到 20 项，并且
      分成两类 —— 这个分法本身就是设计的一部分：

      | 类别 | 什么时候套 | 有哪些 |
      | --- | --- | --- |
      | **一直在** | 建窗口 / 标题变了 / 重载规则，每次都套 | `force_render`、`hz`、`focus_on_open`、`focus_on_activate`、`honor_invalid_serial`、`x11_scale`、`capture`、`capture_mosaic`、`anim` |
      | **开窗时一次** | 只在 `view_map` 里兑现一次 | `float`、`workspace`、`output`、`width`、`height`、`center`、`fullscreen`、`maximize`、`minimize`、`column_width` |

      为什么分两类：`SIGHUP` / 自动重载会**重放**所有规则，如果「搬哪块工作区」「开多大」
      也在重放里，那改一行别的配置就会把已经摆好的窗口全搬一遍。所以重载只更新「这个
      窗口该是什么样」（行为 / 外观），不重演「它开出来是什么样」。

      - 规则算出来的那一份作用存在窗口 / layer 自己身上（`view->rule_eff`，`rule.c` 的
        `vidow_rule_apply` 填）：渲染和布局那边直接看它，不用每条规则各判一遍。
        `vidow_float_update_mode()` 因此也认规则（`float` 写了就是规则说了算，尺寸提示
        晚到的对话框不会把它推翻）。
      - 布尔**匹配**字段全部改名成 `is_` 开头（`is_floating` / `is_fullscreen` /
        `is_maximized` / `is_minimized` / `is_popup` / `is_xwayland` / `is_fixed_size`）。
        起因是个真 bug：`fullscreen` 同时是匹配字段和作用字段，字段表里重名，查找只认
        第一个 —— 于是 `vidow.rule{ fullscreen = true }` 被当成「匹配全屏窗口」，
        报「至少要写一个作用」。**现在字段表重名会在启动时喊出来**（`vidow_rule_init`），
        这类坑不该靠人记住。
      - `workspace` 的取值：具名工作区（没有就建）、`special:名字`（特殊工作区）、序号
        `"2"`。**没有**直接用 `vidow_workspace_move_view_to_name()`：那个只按特殊工作区
        的名字找（IPC 的老规矩），具名工作区得自己 lookup / create。
      - 判据：`test-rules.sh` 第 5 节 14 项 —— 强制浮动、尺寸、居中（窗口中心和屏幕中心
        比）、全屏、整列最大化、收起来（`minimized` 且不抢焦点）、列宽、开在别的工作区、
        开在指定的屏、`anim = false`（数日志里的 `anim open start`，对照组照样有）、
        `float` 跟着重载走；另有 3 项新校验（`column_width` 越界、`width = 0`、
        `workspace = ""`）。

197. **规则的匹配换成正则、匹配字段从 3 个扩到 24 个**（用户要求「匹配允许正则匹配，匹配的项目增加，尽可能覆盖更多情况」）

      原来只有 app_id / title / class 三个字段、都是子串匹配，套用逻辑是一长串手写的
      `if (rule->has_xxx)`。现在字段表和套用都改成表驱动（`vidow_rule_fields[]`，`src/rule.c`）：
      加一个字段只加一行，Lua 解析（`script.c` 的 `l_rule` / `l_rules`）和 IPC
      （`ipcstate.c` 的 `vidow_ipc_json_rules`）自动跟上，不会再三处各写一遍。

      - **匹配是 POSIX 扩展正则**（`REG_EXTENDED | REG_ICASE | REG_NOSUB`），不锚定 = 找到就
        算命中 —— 所以 `app_id = "mpv"` 还是原来的子串语义，`title = "^预览"` 就是锚定；
        整条规则写 `literal = true` 退回纯子串。字段前面加 `!` 取反。
      - 新增匹配字段 21 个：`instance` / `role` / `initial_app_id` / `initial_title` /
        `initial_class` / `initial_instance` / `exe` / `cmdline` / `pid` / `on_output` /
        `on_workspace` / `layer` / `xwayland` / `popup` / `floating` / `fullscreen` /
        `maximized` / `minimized` / `fixed_size`。`exe` / `cmdline` 读 `/proc/<pid>/…`
        （每个窗口抓一次就缓存，而且只有规则里真写了才去读）—— 没有 app_id 可认的老程序、
        包一层启动脚本的，按进程认。
      - `initial_*` 是**上屏那一刻**的名字快照（`view.c` 的 `vidow_view_note_initial_names`，
        只填空着的：X11 的 WM_CLASS / WM_NAME 有时比 map 还晚到）。浏览器、终端都会改标题，
        「认刚开的那一屏」用它。
      - **字段跟目标对不上 = 这条规则不匹配**（给图层规则写 `app_id` 这种）：宁可没生效，
        也别因为「这个字段没意义」就悄悄匹配上一切。
      - 字段名写错**当场报错**并列出所有能写的字段名（以前是静默忽略：`appid = "mpv"` 会
        变成一条「匹配一切」的规则）；正则编译不过也当场报错，带字段名和 `regerror` 的话。
      - `vidowmsg rules` 每个字段都给（没写的是 `null`），`vidow.rules()` 里没写的是 `nil`；
        窗口 JSON 的 `rules` 里多了 `capture` / `capture_mosaic` / `x11_scale`（自检脚本的判据）。
      - 顺带修文档：`app_id` 在 X11 上取的是 WM_CLASS 的 **class 段（第二段）**，旧手册写成
        「第一段」（ICCCM 里第一段是 instance，现在有单独的 `instance` 字段可以匹配）。
      - 判据：新测试 `test-rules.sh`（77 项：读回、报错、窗口上的正则 / 取反 / literal / pid /
        exe / cmdline / 布尔字段 / AND / 覆盖顺序，图层上的 namespace / layer）；
        `test-defaultconfig.sh` 那条「表字段都得写进配置说明」现在从 `rule.c` 抠字段名（39 项）；
        `test-capture-hide.sh` / `test-focus.sh` 跟着新的报错文本和日志格式改。

196. **exec 那四个名字并成两个**（用户要求「exec 这里优化一下，有些重复功能的方法就合并了」）

      四个入口里真正重复的是名字，不是逻辑：`exec` / `exec_status` 早就是同一个实现
      （`l_exec_yield(L, want_status)`，只差返回形态），`spawn` / `exec_once` 也都是同一个
      `vidow_exec_detached()`，只差「等不等 / 要不要输出 / 起几次」。所以并成：

      | 现在 | 原来的名字 |
      | --- | --- |
      | `vidow.exec(cmd)` | `vidow.exec(cmd)`（没变） |
      | `vidow.exec(cmd, { status = true })` | `vidow.exec_status(cmd)` |
      | `vidow.exec(cmd, { wait = false })` | `vidow.spawn(cmd)` |
      | `vidow.exec_once(cmd)` | `vidow.exec_once(cmd)`（没变） |

      决定留这两个名字（用户定的：一个 `exec`、一个 `exec_once`）—— 「等不等 / 要不要退出码」
      是同一个动作的参数，不值得各占一个名字；「一个生命周期只起一次」是另一件事，值得一个
      名字。选项表只认 `wait` / `status`，拼错当场报错并列出能写哪些（和配置族那张表一个规矩）。

      - 老名字**不静默**：`vidow` 表上挂了 `__index`，`vidow.spawn` / `vidow.exec_status`
        会报「已经并进 vidow.exec：写成 vidow.exec(cmd, { wait = false })」—— 比
        `attempt to call a nil value` 有用得多，而且不用在 API 表里留两个墓碑函数（那样
        `test-defaultconfig.sh` 的「说明完整性」还得把它们写进手册）。
      - `exec_once` 也收选项表，但只认默认的撒手跑：`{ wait = true }` 当场报错（它是等这份
        配置跑完才起的，等不了；要输出就用 `vidow.exec`）。
      - `{ wait = false }` 那条路比原来的 `spawn` 多做一件事：起之前 `script_sync_env_for_child()`
        对一次环境（缩放 / `DISPLAY`），和等的那条路一致。
      - 回归：`test-config.sh` 新增一节 8 项（`{ status = true }` 拿到 `out=hi code=3`、
        不给选项时照旧 `nil + 原因`、`{ wait = false }` 立刻返回 true 且命令真跑了、两个老名字
        的报错都带「并进 vidow.exec」、拼错的选项名报错、`exec_once` 的 `wait = true` 报错），
        16 → **24 项**。`test-hooks.sh` 里三处 `vidow.spawn()` 改成新写法（顺手验了撒手那条路）。

195. **事件钩子（`vidow.on`）、自启动（`exec_once`）、和 systemd 用户管理器两个方向对账**
      （用户要求「参考 hyprland 实现事件系统，自启动，窗口打开时之类的。执行 shell 需要继承
      vidow 的环境变量，有些 systemd 起来的应用也要继承 vidow 变量，也就是说 vidow 需要支持
      systemd 环境变量」+「支持的事件尽可能丰富」）。

      三件事一起做的，因为它们是同一件事的三面：**事件**要能挂动作、动作要能起来就自启动、
      起来的进程要有对的环境。

      **1）事件钩子（`src/hooks.c` / `src/hooks.h`）**。以前事件只有一条路：外挂脚本连
      socket 订阅（`{"cmd":"subscribe"}`）。现在配置里也能听：

      ```lua
      vidow.on("window-opened", "notify-send \"$VIDOW_APP_ID\"")   -- 起一条命令
      vidow.on("window-opened", function(e) vidow.log(e.data.title) end) -- 跑一段 Lua
      vidow.on("*", { "echo $VIDOW_EVENT >> /tmp/e.log", fn })          -- 全部 / 一串
      ```

      设计上的选择：

      - **一条事件两个出口**：`emit()`（ipcstate.c）建好那条 JSON 之后，既广播给订阅者，也
        交给钩子（`vidow_hooks_dispatch`）。所以两边的载荷**永远同构**，文档写一遍就够。
      - 「没人要就不发」的门槛从 `vidow_ipc_wanted()`（只数订阅者）换成
        `vidow_event_wanted()`（订阅者**或**钩子）—— 不然配置里挂的钩子会因为「没人订阅」
        而收不到事件。
      - 钩子**一律进队、在 idle 里跑**（`hook_queue` + 一个 idle 源）：钩子里能做的事和 IPC
        命令一样多（关窗口、改配置、开会话），不能站在合成的半路上跑 Lua。队列上限 256，
        积满了丢新的并打一条 error。
      - **命令钩子**把事件字段变成环境变量（`VIDOW_EVENT` / `VIDOW_EVENT_TIME_MS` /
        `VIDOW_EVENT_JSON` + `data` 里每个标量字段一条 `VIDOW_<大写>`，值截 512 字节，跑完
        还原）—— 这样 `"notify-send \"$VIDOW_APP_ID\""` 就能用，不用自己解析 JSON。
      - **Lua 钩子**能 await `vidow.exec()`：和配置文件那套协程同一套「yield 一条命令、回来
        接着跑」（`hook_call` + `vidow_exec_run`，见 script.c），所以「开窗时先查一下」写得
        出来。回调出错只打日志，不连累同事件的其它钩子。
      - 配置重载 = 钩子重新声明（旧的整批作废、Lua 引用还回去），和键位 / 规则 / 每块屏的
        配置一个规矩。
      - 自定义事件：`{"cmd":"emit","event":"…","data":{…}}`（`vidowmsg emit my-event --data
        '{"n":1}'`），订阅者和钩子一起收 —— 脚本之间、脚本和配置之间的一条通道。`vidowmsg`
        的 `--data` 原样当 JSON 搬进去（被引号包成字符串的话对面只能拿到一串文本），服务端
        也容错：收到字符串形式的 data 会再解一次。

      **2）自启动（Hyprland 的 `exec-once` / `exec`）**：`vidow.exec_once(cmd)`（整个 vidow
      生命周期只起一次，重载不重来）、`vidow.exec(cmd, { wait = false })`（每次配置加载都
      起，撒手）、`vidow.exec(cmd)` 照旧（要输出、等它）。`exec_once` 的命令**先排队**，等这份配置跑完
      （环境、缩放、`DISPLAY` 都定了）再起 —— 这和 `script_sync_env_for_child()` 是同一个
      理由：配置是分步跑的，命令起来时环境可能还没落地。「起过了」按**命令原文**记（不是行号
      / 下标）：重载后同一行还是同一行、认得出，改过原文就算新命令（和 Hyprland 一个规矩）。
      每轮配置跑完还会把**这一轮没声明的**从名单里划掉（不然删删改改攒到 128 条上限之后就
      再也记不住新命令了 —— 那时每次重载都会把它当没起过、再起一遍）。对账时比的是自己拷的
      一份字符串：队列项连同命令原文在起完那一刻就 `free` 了，指着它们比就是在读已释放内存
      （tcache 的 `next` / `key` 正好盖在字符串头上），刚起的命令会当场被判成「配置里没了」、
      下一轮重载又起一遍 —— 第一版就是这个 bug，`test-env.sh` 5b 逮住的。

      **3）和 systemd 用户管理器两个方向对账（`src/env.c` / `src/env.h`，新文件）**。这是
      需求里「systemd 起来的应用也要继承 vidow 变量」那一半：

      - **推**（`systemd_env_export`，默认开）：`systemctl --user import-environment` +
        `dbus-update-activation-environment --systemd`，推 `WAYLAND_DISPLAY` / `DISPLAY` /
        `XDG_CURRENT_DESKTOP` / `XDG_SESSION_TYPE` / `XDG_SESSION_DESKTOP` / `VIDOW_SOCKET`
        + 用户 `vidow.env{}` 声明过的每一个。时机：socket 定下来、X 起来（`DISPLAY` 变了）、
        配置落地，各推一轮；运行时改变量也在下一个 idle 里推（合并：连改十个也只 fork 一次）。
      - **吸**（`systemd_env`，默认开）：启动时问一次用户管理器，把**我们自己没有的**吸进来
        （PATH / `SSH_AUTH_SOCK` / `environment.d` 里那些）。`WAYLAND_DISPLAY` / `DISPLAY` /
        `XDG_CURRENT_DESKTOP` 这类「哪个合成器」的身份、`WLR_*` / `VIDOW_*` 一律不吸；我们
        自己有的也不覆盖（启动 vidow 的那个 shell / service 说了算）。
      - **别赖账**：`vidow.env` 改过的变量记着原值，退出时还原；推给用户管理器的那些也先记
        快照、退出时还回去（嵌套跑 / headless 测试时这一步最要紧 —— 不还的话桌面里由 systemd
        拉起来的程序全指着已经退了的 vidow）。配置里 `systemd_env = 0` 的话，启动时按默认值
        吸进来的会还回去（用户 `vidow.env` 显式设过的不动）。环境变量覆盖：
        `VIDOW_SYSTEMD_ENV` / `VIDOW_SYSTEMD_ENV_EXPORT`。

        踩过的坑：
        - 推的清单里**不能有 `XDG_RUNTIME_DIR`**（第一版带了，测试跑完一看用户管理器里
          `XDG_RUNTIME_DIR=/tmp/vidow-env-test/run` —— 那个目录早没了）。它是会话自己的东西，
          嵌套/headless 里 vidow 的是临时的；portal 那边要连 PipeWire，自己带着快照 / 还原管
          它，不归这一份。`XCURSOR_*` 同理（会改掉系统里所有程序的指针大小）。
        - **只碰「我们够得着的」那个管理器**：认 `systemctl --user show-environment` 这一条，
          够不着就什么都不推、也不还原。这条是踩出来的 —— 嵌套跑 / headless 测试时
          `XDG_RUNTIME_DIR` 是临时的，`systemctl --user` 连不上，可 `dbus-update-activation-
          environment` / `busctl` 走的是会话总线，照样改得到**宿主会话**的用户管理器：推上去
          的是测试里那个 `WAYLAND_DISPLAY`，退出还原还会把宿主本来就有、我们却以为「原来没
          有」的变量 `unset` 掉（实测把宿主 Hyprland 会话的 `WAYLAND_DISPLAY` / `DISPLAY` /
          `XDG_CURRENT_DESKTOP` 删掉过）。**读**仍然可以退到 `busctl --user get-property …
          Manager Environment`（读不改别人，而且从 tty 起 vidow 时正要把管理器的环境吸进来）。
        - **还原得走两条路**：`systemctl --user set-environment` / `unset-environment` 管
          systemd 用户管理器，`dbus-update-activation-environment --systemd` /
          `busctl … UnsetEnvironment` 管 dbus 的激活环境（两套账，都得还）。
        - `dbus-send` **不等应答**就退出，消息会丢（试过：`UnsetEnvironment` 看着成功，变量还
          在）。换成 `busctl`（它等应答）才删得掉。
        - `vidowmsg --raw` 打得少，测试里解析 JSON 要认准「pretty 模式剥了信封、raw 是整封」。
        - 顺手修掉一个自己在测里踩出来的坑：`echo a|b >> f` 写在 `vidow.on()` 的命令串里，
          `|` 会被 `sh` 当管道（命令就成了一串「找不到的命令」）—— 文档里的例子都带了引号。

      **4）事件本身也补了一批**（用户说「尽可能丰富」）：`keybind`（绑好的键被按下，带
      trigger + action）、`ipc-action`（从 IPC 来的动作，带 source）、`window-activated`
      （xdg-activation / `_NET_ACTIVE_WINDOW` / IPC `activate-window`）、`drag-started` +
      `drag-ended`（挪动和改列宽都算）、`output-power-changed`、`input-device-added` +
      `input-device-removed`（含虚拟键盘）、`xwayland-ready`、`screencast-started` +
      `screencast-stopped`、`config-failed`。事件总数 31 → **42**，权威名单是
      `vidowmsg events`（hooks.c 里一张表）。新事件的载荷都是「几个标量字段」
      （`vidow_ipc_emit_simple()` + `VIDOW_EV_STR/NUM/BOOL`），不用为每条写一个 payload 函数。

      **测试**：`./test-hooks.sh`（30 项断言）、`./test-events.sh`（28 项断言）、
      `./test-env.sh`（42 项断言）。三个都是全 headless，各带自己的靶子（钩子看日志文件、
      事件看 `*` 钩子写的事件流水、环境看假的 `systemctl` / `busctl` /
      `dbus-update-activation-environment` 记下的调用）。`test-env.sh` 特意把三条命令都做成
      脚本 —— 真实跑一遍会把**你真会话**的环境改掉，测完还得手动收拾（这一次是自己踩了才
      知道的，见上面 `XDG_RUNTIME_DIR` 那条）。

      **没做的**：X11 的 urgency（`window-urgent`）—— wlroots 0.20 的 `wlr_xwayland_surface`
      没有 urgency 信号也没有 `urgent` 字段（只有 ext-workspace 协议里有），要做得像 wlroots
      那样自己读 `WM_HINTS`（xcb-icccm）。留个口子，等有人真要「窗口在喊我」再加。

194. **列宽改成「连 gap 一起分」：两条 50% 不再被中间的 gap 挤出屏外**（用户报「优化一下布局，
      两个 50 50 的窗口会因为 gap 导致一个屏装不下。现在 gap 应该是内部的区域，一个窗口区域
      减去内部 gap 才是实际窗口大小」）。

      以前列宽 = `可用宽 × 比例`：比例加起来等于 1 的时候，几条列**再加中间的 gap** 就比可用区
      宽出一个 gap —— 两条 50% 各 630（可用宽 1260），加起来 1270 > 1260，第二条的右边缘顶出屏
      外 10px，得滚动一下才看得全。niri 不是这么分的（`src/layout/scrolling.rs` 的
      `resolve_column_width`）：

      ```rust
      ColumnWidth::Proportion(proportion)
          => (working_size.w - gaps) * proportion - gaps - extra.w
      ```

      比例分的是**连 gap 一起算**的那块区域，窗口 = 那块区域减掉一个 gap。niri 的
      `working_area` 没有预先扣 gap（它在摆的时候才 `origin += working_area.loc + gaps`），
      vidow 的可用区（`vidow_work_area`）已经把左右两道 gap 扣掉了，所以同一个式子换算到
      vidow 的坐标系里就是：

      ```
      列宽 = (可用宽 + 内间距) × 比例 − 内间距
      ```

      两者其实是一个式子，只差左右各一个 gap 的原点：比例 = 1 时两种算法结果一样，所以
      「一条列占满工作区」的行为没变，变的只有分数比例那几条路。纵向从第一天起就是这个口径
      （niri：`gaps_left = gaps × (n+1)` → 每格 `(可用高 − (n−1) × gap) / n`），这次只是把横向
      对齐过去 —— 于是「比例分区域、窗口 = 区域 − gap」在横竖两边一致了。

      改动就两个新函数（`src/server.h`，arrange 和拖动改宽共用一份换算）：

      - `vidow_column_width_px(配置, 可用宽, 比例)` —— 比例 → 像素。arrange 的 Pass 1
        （`src/view.c`）和改宽起拖时的列宽（`src/drag.c`）都改走它；
      - `vidow_column_fraction_of_px(配置, 可用宽, 像素)` —— 像素 → 比例，是上面那条的**逆**
        （niri 的 `set_column_width` 里 `proportion = (px + gaps) / (working_width - gaps)` 是
        同一个式子的另一头）。两条互逆，所以 Mod+右键拖改宽时右边缘还是跟着光标停，不会因为
        gap 差一格。

      实测（headless 1280x720、`gaps 10`、可用宽 1260）：两条 0.5 档 = 各 625 宽、落在 10 / 645
      → 第二条右边缘 1270 = 屏右边那道 10px 的 gap，**正好一屏装下**（以前是 630 宽、落在 10 /
      650，右边缘顶到 1280 屏幕边上 —— 右边那道 gap 被吃掉了，还得滚一下）。`gaps 6` 那套数字跟着变成 631 / 624（`gaps_inner 20`）/ 616（`gaps 16`）/ 580
      （`gaps 40`），单边 `gaps_left 40` → 614。

      回归：`test-gap.sh` 加了「两条 50% 正好装进一屏：第二条右边缘贴到右边距」（33 → 34 项），
      并重算了上面那组列宽；跟着重算的还有 `test-drag-insert.sh`（0.3 档 → 371，三条列 10 / 391
      / 772）、`test-fullscreen-scroll.sh`（0.4 档 → 498，滑出位置 -508 / -1016 / -1524）、
      `test-fullscreen-2out.sh`（2 倍屏上 498，全屏滑到 264）、`test-maximize.sh`（0.25 → 312、
      0.5 → 631）、`test-hotplug.sh`（2/3 → 843）—— 全部过；`test-tile-resize.sh`（Mod+右键拖
      改宽）不用动、照旧过（它量的是「拖了多少像素、宽度就变多少」，两条互逆的换算正好抵消）。

193. **一列里能插进另一个窗口了，落点还有了预览块**（用户报「一列里面允许插入另一个窗口，
      像 niri 那样。窗口拖拽的时候要在落点显示预览，和 niri 一样」）。

      两件事其实是一件事：niri 的拖拽落点只有两种（`src/layout/scrolling.rs` 的
      `insert_position`）—— 光标离**最近的一条列缝**（一条列的左边缘，左右两边的缝都算：
      光标在哪条列的右半边时最近的就是它右边那条列的左边缘）更近 = 在那个缝上新开一条列
      （NewColumn）；离那条列里某个**窗口缝**更近 = 插进那条列、一列里上下叠起来
      （InColumn）；两个距离谁小听谁的（`if vert_dist <= hor_dist`）。vidow 以前只认第一种，
      而且判断用的是「列的**中心**在光标左边还是右边」—— 拖到窗口身上只会插在它**旁边**，
      永远并进不去，屏幕上也没有任何提示（`src/drag.c` 顶上那句「niri 的 insert hint 语义，
      没画提示条而已」）。

      第一版把 `vert_dist` 量成了「到光标底下那条列**左边缘**的距离」（离右边那条缝多近都
      不算），用户看完就指出来了（「插入新列应该左右都判定吧」）；niri 量的是 `column_xs` 里
      **最近**那一项的距离，所以右半边那条缝也得算 —— 改完「贴着某条列的右边缘拖 = 插到它和
      右边那条列中间」，那条断言做成 `test-drag-insert.sh` 的第一节：把 `hd` 退回旧写法，
      这一节连同后面几节一共 FAIL 8 条。

      改完：

      - `src/drag.c` 的 `insert_pos_at()` 按 niri 那套算落点（横向到列左边缘 / 纵向到窗口缝，
        包括 niri 那两条例外：列带左边那条外层 gap 里 = 落最前面、光标在别的列里 == 进那条列），
        落点那块地方同时给出预览块的位置尺寸（niri 的 `insert_hint_area`：新列是 300 宽、
        整块可用高的「卡片」，进列是那条列里的一条横条 —— 贴最上面/最下面 150 高、骑在中间的
        缝上 300 高）。位置一律用**屏幕上看得见的那块**：列的 `screen_x / slot_width` 由
        arrange 记下来（全屏那列占一整屏、整列最小化的列不占位这些规矩只有 arrange 知道），
        窗口的 `geom` 就是现在这一块 —— 落点这里不重算条带数学。
      - `src/view.c` 的 `vidow_view_insert_into_column()`：插进某条列（同一条列 = 就地换上下
        顺序；别的列 = 从原来那条列里摘出来再插进去，原列空了就拆掉）；
        `src/workspace.c` 的 `vidow_workspace_move_view_into_column()`：目标列在别的屏 /
        别的工作区上那条路 —— 搬的是**窗口自己**（niri 拖的一直是窗口），原来那条列里还有
        别人就留在原处。
      - 预览块：`layer_toplevels` 里一块 `wlr_scene_rect`（半透明橙，平时关着），拖动时每个
        指针事件重算一次并摆好；只在光标底下那块屏里画（贴着屏边裁掉一半，跟窗口一样不许糊到
        隔壁显示器上）。层级收口成「所有窗口之上、被拖的那个之下」（拖动时抬两次，已经在正确
        位置就一个节点都不碰）。颜色是**预乘**的 —— `wlr_scene_rect` 的颜色直接进预乘混合，
        一开始忘了乘，叠在纯红窗口上量到 `(255,200,127)`（加法，不是半透明），乘过之后才是
        niri 那个 `(127,100,63)`。
      - 三处故意跟 niri 不一样（README「拖平铺窗口的落点」一节里有）：①被拖窗口原来那个空位
        在拖动期间留着（niri 一按下去就并拢，vidow 松手才重排），所以预览是按屏幕上现在的
        位置画的；②「新开一条列」搬的是**整条列**（vidow 的平铺拖拽一直动的是列），所以 niri
        里「从两窗口的列里拎一个出来单独成列」得用 `move-window-direction` 挤进旁边那条列；
        ③被拖的窗口画在预览块之上（niri 的移动窗口也在提示块之上）。

      - **新增** `test-drag-insert.sh`（23 项）：三条 378 宽的列打底 —— ①贴**左边那条列的
        右半边**（离右边那条缝 43px）→ 新开一条列插到两条列中间（右边的缝也算数：挪到第 2 位、
        w3 x 786→398、w2 x 398→786）；②贴目标列**上边缘** → 插进那条列的最上面（两个窗口
        对半劈 345 高、原来那条空列没了、列数 3 → 2）；③贴列带**最左边**（外层 gap 里）→
        新开一条列插到最前面（列的顺序换过来）；④贴目标列**下边缘** → 落在那条列最下面
        （三个窗口三等分 226 高、列数 2 → 1）。落点预览块另用像素量：拿「同一块地方、同一份
        布局、还没拖」那张图当底，算 `128,100,63 + 0.498×底` 必须等于拖动中量到的值（容差
        12）—— 一次把「画在哪儿」「多透明」「颜色对不对（忘了预乘差一大截）」全钉住；再验
        卡片的宽度（左边到 243、右边到 160 那两条）和「松手就收掉」。
      - `test-drag.sh`（11 → 12 项）：落点判断换成 niri 那套之后，原来那条「拖到最左边 → 插到
        w1 前面」的拖点得**真的落在条带左边那条外层 gap 里**（它原来离 w1 左边缘 30px，按
        niri 的算法那是「插进 w1 那条列」）—— 同一节改成拖 w1 去换到最前面，并加一条落点
        日志断言。`test-jelly.sh` 的平铺那段改量**右边缘**：预览块正好落在窗口原来那个空位上
        （屏幕左边），量左边缘会量到预览块（形变判据两边一样）。
      - 相关那几套照旧过：`test-drag-follow.sh`、`test-drag-stall.sh`、`test-mixdpi.sh`、
        `test-blurcache.sh`、`test-closeanim.sh`、`test-jelly.sh`、`test-workspace.sh`。

192. **关一个窗口的时候，隔壁那块会「瞬移」到位**（用户报「窗口关闭的时候窗口移动动画不够
      自然，好像有点瞬移」）。

      根因在 `vidow_place_view()` 里那条「目标位置没变」（`!moved`）的分支：它一律
      `cancel` 掉位移补间 + 把 `anim_off` 清零。而**同一帧里 arrange 会跑好几遍** —— 关一个
      窗口那条路实测 3 遍（unmap 一次、摘列一次、换焦点一次）：第一遍给那块本来要滑过来的
      窗口起了补间（日志 `move tween B: 524,10 -> 10,10（190ms）`），第二遍看到目标位置没变
      （geom 已经是 10,10）就把刚起的补间掐掉、偏移归零 —— 它当场落到终点。

      逐帧量出来的（headless + grim，扫 y=200 那行第一个红色像素 = pattern 的左边缘）：

          修之前：t=22ms 就在 12（= 终点）   ← 补间一帧都没跑
          修之后：t=22ms 524 → 95ms 155 → 169ms 18 → 239ms 12

      改法：`!moved` 而**手里正跑着一个位移补间**时别动它 —— 那个补间就是朝这个位置跑的；
      没有补间在跑时照旧把残留偏移清掉（原语义）。弹出物 / `window_move_ms = 0` / 正被鼠标
      拖着 / 入场动画那几条路都在它上面的分支里挡着，行为不变。

      - **回归**：`test-closeanim.sh` 新增一节（12 → 17 项）：蓝窗口在左、pattern 在右，
        关掉左边那条列之后断言 pattern 的左边缘在位移补间 30% 处**还在路上**（既不是起点
        也不是终点）、30% → 75% → 走完单调往左、最后落在自己那条列上。把 `src/view.c` 退回
        旧代码跑这一节是 FAIL（30% 处已经是终点 8），改回来就 PASS —— 这条断言抓的就是它。
        `test-openanim.sh`、`test-drag-follow.sh`、`test-bounce.sh`、`test-jelly.sh`、
        `test-scrollcull.sh`、`test-fullscreen-scroll.sh` 照旧过。

191. **全屏的窗口现在参与滚动，占的也是整块屏**（用户报「全屏的窗口也要参与滚动。现在的全屏
      好像不是真全屏，之前 obs 不小心全屏的时候看起来和普通窗口也差不多」）。

      两条都是 niri 的规矩（官方文档 Fullscreen and Maximize）：一是「Fullscreen windows cover
      the entire screen」，二是「fullscreen and maximized windows remain a normal participant of
      the layout: you can scroll left and right from them and see other windows」。vidow 以前两条
      都不对：

      - **几何给的是工作区**（`work` = 可用区再缩进一圈 gap）——只有一条列、列宽 1.0 的时候，
        它和普通平铺窗口**一模一样**（都是 10,10 1260x700 那种），这就是用户看到的「看起来和
        普通窗口也差不多、不是真全屏」。
      - **摆好之后钉在屏幕上不动**（`continue` 掉、摆在 work 上），滚动只挪别人 —— 全屏窗口
        盖着整块屏，怎么滚都还是它，看着像「滚动没反应」。

      改法都在 `output_arrange()`：

      - 全屏的那块按**整块屏**摆（`wlr_output_layout_get_box()`：不吃 gap、也不给状态栏的占位
        留空间），左边缘跟着条带走 = `full_box.x + col->offset - viewport`。落在自己那条列上
        时视口正好等于它的 offset —— 左边缘 0、严丝合缝盖住整屏。
      - 那条列在条带上占**一整屏宽**（`column_has_fullscreen()`）：Pass 1 推进 cursor、Pass 2
        算「聚焦那条列要多宽」都用 `work.width`。于是往旁边滚一列，全屏那块整块滑出去、隔壁的
        列并排露出来（niri 文档里那张「全屏和别的窗口并排」的图）。列自己的 `width_fraction`
        不动 —— 退出全屏回来还是那么宽。
      - 滚到屏外那半块照样**裁在自己那块屏里**（`view_clip_box()`，多屏时不会画到隔壁显示器
        上）；整块滚出屏 → 停到停车位（日志 `A 藏起（停到屏外）`），滚回来又原样出现。
      - 尾巴上那句「浮动的全屏占满工作区」以前对**列里的**全屏也照摆一遍（把它刚摆好的整屏
        几何盖回工作区）—— 现在只摆浮动的那个。浮动的全屏本身没变（仍是占满工作区 + 永远压
        在最上面；niri 是把它收进滚动布局，这一版没跟）。

      - **回归**：新增两个测试，一共 49 项断言（都在真 GLES2 那条路上跑：`shader=on`、AMD
        GPU，装饰 / 阴影 / 模糊那些像素都是真的渲出来的）：

        * `test-fullscreen-scroll.sh` 33 项（headless 1280x720、gaps 10、四条 0.4 宽的列）：
          全屏占 `0,0 1280x720` 且屏幕最左边那条 gap 被盖上（`x=4` 那颗从背景黑变成窗口
          自己的内容）；右移一列 → 全屏 `-514`、隔壁列并排 `766`（左边取样点绿、右边红，
          两块同时在屏上）；再右移 → `-1028` → `-1542` 整块滚出屏并「藏起」；左移回来又
          严丝合缝；退全屏 → 列宽回 `504`、后面的列合拢；滑出去那半块有
          `clip: … 裁到自己那块屏里（窗口 -514,0 1280x720）` 那行；最后一节是**客户端自己**
          要的全屏（`preinit --after-commit --fullscreen`，OBS 按 F11 那条路）—— 同样整屏、
          同样跟着条带走。
        * `test-fullscreen-2out.sh` 16 项（用户那台机器的几何：1 倍屏在 0,0、2 倍屏
          `2560x1440` 摆在 x=1280）：全屏的盒子是**整块逻辑屏** `1280,0 1280x720`（物理
          2560x1440 就是它的两倍）；2 倍屏的截图里贴着屏幕边那颗不是背景（没有 gap 的缝，
          内容在逻辑 20,200 那点判红）；滚到 `252` 之后**隔壁 1 倍屏右边缘那点还是它自己的
          窗口内容**（越界那半块裁在自己那块屏里，没画过去）；滚回来又严丝合缝。

        `test-floatstack.sh`（全屏压浮窗那一节）、`test-workspace.sh`、`test-maximize.sh`
        （条带 / 视口那套判据共用）、`test-scrollcull.sh` 都照旧过（rc=0）。

190. **窗口滚出视口之后，客户端被叫去按隔壁那块屏的缩放重画**（用户报「窗口滚动出去后，
      又按照另一个屏幕的缩放变化了」）。

      这是 189 的续集：189 把「录屏那一路挂在哪块屏」掰回来了（光标出来了、尺寸也对了），
      用户接着发现**画面本身**在窗口滚出视口之后变成按隔壁那块屏的缩放渲染 —— 2 倍屏上
      录它一直半分辨率糊着，滚回视口才清楚。他给的日志里就是那条来回跳的线：

          15.410 surface obs ~/D/Vidow: 缓冲 954x1068 声明 1 倍（输出 2.00x…）
          16.216 surface obs ~/D/Vidow: 缓冲 1908x2136 声明 2 倍（输出 2.00x…）
          16.224 …1 倍   16.237 …2 倍     ← 窗口扫过屏边界时来回跳

      根因不在我们发的那点元数据（189 修的就是它），在 **wlroots 的 wlr_scene_surface 会
      替合成器发 `preferred_buffer_scale`**：判据是「**可见面积最大**的那块输出」（见
      wlr_scene.h 里 wlr_scene_surface_create 的说明）。平铺窗口的几何框里含着滚动视口的
      偏移，滚出视口之后框跨在隔壁屏上 —— 那边算出来的可见面积更大，客户端就被叫去按那块
      屏的缩放重画。

      而**让它跨过去**的正是 187：那次为了避免 set_clip 改写 src_box 把装饰采样带偏（表面
      被拉伸铺满 + 黑边），装饰开着（fx.active）时干脆**不裁**了 —— 少掉的那一刀，正好是
      告诉 wlroots「这块窗口只在自己那块屏上」的那句话。

      这一条把两者解耦，然后重新裁上：

      - `fx_geo_tex_map()` 不再读场景节点的 `src_box`（裁过的就是它），改读
        `view->fx.content_box` —— **没裁过**的那一版，在每次 commit、每次下刀之前抄一份
        （`view_snapshot_content_box()`：撤掉上一刀 → wlroots 按客户端声明的 viewport /
        缓冲重算 → 读出来 → 再补回去）。
      - `vidow_view_apply_clip()` 对装饰开着的窗口**照样裁**（撤掉 187 那次「让开」）。
      - 「算在哪块屏上」的判据从「几何框」改成 **`view_visible_box()` = 看得见的那一块**
        （停到停车位 → 空；裁着 → 几何框 ∩ 自己那块屏；其余 → 几何框），一点都看不见时
        回退到「它属于哪块屏」（`vidow_view_owner_output()`：平铺的看它那条列挂哪）。
        倍率、模糊底衬、按窗口录屏的取景框都走它。
        拖着窗口跨屏那一段**不裁**（view_clip_box 对浮窗 / 正被拖的窗口返回 false），
        所以那时候判据仍然是「画在哪块屏上」—— 拖到 4K 屏上照样清楚（test-mixdpi 钉着
        这一条；这一版一开始就是改成「一律按归属」被它先抓住的）。
      - 客户端的倍率也**钉**在它那块屏上（`view_pin_scale()`：wl_surface.preferred_buffer_scale
        = 它那块屏的倍率，子表面一起钉）。停到停车位之后窗口不在任何输出上了，光靠 wlroots
        那套会掉回 1 倍 —— kitty 这类客户端认 preferred（`integer_scale.preferred` 顶替
        `deduced`）。带窗口编号记一行日志，便于「谁被钉成几倍」对账。
      - `clip:` 那行状态从 DEBUG 提到 INFO（跟「藏起 / 显示」一样是个状态，测试也照它断言）。

      - **回归**：`test-win-scrolled-output.sh` 16 项（新增两条：滚出视口时
        `钉住 preferred_buffer_scale = 2` 且之后不出现 `= 1`、表面树确实「裁到自己那块屏里」）；
        `test-mixdpi.sh` 7 项（跨屏拖动仍按对面那块屏烘）；`test-clip-stretch.sh` 10 项
        （装饰采样没被裁剪带偏，pixman / gles2 两种渲染器都过）；`test-wincapture-scale.sh`
        16 项；`test-capture-hide.sh` 30 项；`test-scrollcull.sh` 34 项；`test-viewport-crop.sh` 5 项。

189. **窗口滚出视口之后，录屏那一路挂到了隔壁那块屏上**（用户报「窗口在 0×0 屏幕就能看到
      光标，显然是光标坐标问题」）。

      这是上一条（188）之后的续集：188 把「指针动了要推帧」补齐了，用户重测还是「另一块屏
      上录不到光标」。日志里三行连起来看就露了：

      - `screencast: 请求录窗口「obs ~/D/Vidow」（kitty，DP-3 scale 1.00 → 937x1068）`
        —— 这一路流挂的是 **DP-3**（0,0 那块 1080p）；
      - `surface obs ~/D/Vidow: 缓冲 1874x2136 声明 2 倍 -> 逻辑 937x1068（输出 2.00x…）`
        —— 窗口其实**画在 DP-2 上**（4K 那块，缩放 2、布局 1920,0；kitty 按 2 倍渲染）；
      - `光标不在这一路里 —— 这块屏（DP-3）上没有可见光标（指针在别的屏上？）`
        —— 指针明明压在窗口上（在 DP-2 上），却去 DP-3 的光标列表里找 → 永远找不到 →
        元数据里一直 `id=0` → 消费者（OBS）那边就是「有画面、光标不进来」。

      根因在 `vidow_win_grab_output()`（`src/wincapture.c`，portal 窗口流和窗口抓帧源共用
      这一份）：它按**「现在画在哪块屏上」**反查布局（`vidow_view_screen_box()` 的中心）。
      但平铺窗口的 geom 里含着**滚动视口的偏移**（`view.c` 的 `output_arrange`：
      `x = work.x + col->offset - viewport`）—— 用户当时正把别的列滚进视口，被录的窗口滚出
      视口、画的那些像素落在隔壁屏的范围里（日志里那串 62Hz 的
      `强制渲染: obs ~/D/Vidow 它滚出视口了` 就是它），中心反查出来就是隔壁那块屏。而流一旦
      建好就**钉**在那块屏上：窗口后来滚回自己那块屏、重新对齐，这一路也不会换回来。

      选错屏的后果三件（这一条一次修掉）：① 光标去错那块屏的光标列表里找 → `id=0`；
      ② 流尺寸乘的是错那块屏的 scale（用户这一路是 937x1068，本来该是 1874x2136 —— 2 倍屏上
      分辨率只有一半）；③ 输出提交那条抓帧路盯的也是错那块屏。

      - 判据改成**跟滚动无关**的那一个：平铺窗口看它**那条列**挂在哪块屏
        （`view->column->output`）；浮动 / 弹窗不参与滚动（geom 就是它自己的绝对位置），
        继续按画在哪块屏上反查 —— 拖着跨屏时 `view->output` 还停在原来那块屏上，
        反查才是「用户现在看到它在哪」。
      - **回归**：新增 `./test-win-scrolled-output.sh`（两块屏 1 倍 + 2 倍；四列 0.35 让条带
        装不下；逐列聚焦找一个「列在 2 倍屏上、画出来的中心却落在 1 倍屏范围里」的窗口，
        抓它）：尺寸必须是「逻辑 × 2」，而且**滚出去 / 滚回来两次一样**。旧代码必挂 ——
        实测滚出去 483x748（乘了隔壁 1 倍屏的 scale）、滚回来 966x1496。
      - 顺带跑的：`test-portal-own.sh` 51 项、`test-wincapture-scale.sh`、
        `test-capture-hide.sh` 30 项、`test-clip-stretch.sh` 10 项、`test-scrollcull.sh` 34 项、
        `test-viewport-crop.sh`、`test-mixdpi.sh`、`test-dpi.sh` 全过（rc=0）。

188. **指针移动本身也推一帧**（光标不进画面时，光标的位置 / 位图就是这一帧的内容）。

      用户接着报「光标好像出现了，现在又不能捕获到光标了（OBS 预览里也没有，采集源的
      「捕获光标」勾着）」。查下来 vidow 这一侧发的东西是对的：

      - 合成器日志里 `光标在这一路里 —— 流里 661,630（热点 12,6，位图 24x24…）` 说明
        指针压在被录窗口上时**确实报了** id=1 + 位置 + 热点 + 位图；
      - `build/cursorprobe`（照 GStreamer 那样声明 `SPA_META_Cursor` 的独立消费者）
        在**窗口流**上也拿得到 `bitmap=1 / id=1`，位置换算对（`test-portal-own.sh` 6b2）；
      - OBS 32.2.2 那条路逐行读过（`plugins/linux-pipewire/pipewire.c`）：它声明的是
        `SPA_META_Cursor` 的**范围**（1×1…1024×1024，首选 64×64），我们报的固定值
        262192 落在里面；`bitmap_offset=0` 时它**沿用上一次那张贴图**；`spa_meta_cursor_is_valid`
        就是 `id != 0`；`SelectSources` 里只要有 METADATA 就一律要 metadata（「捕获光标」
        那个勾只管它**画不画**，默认勾着）。所以「没光标」不能只赖 OBS。

      但这里有一个**真的缺口**，这条就是修它：metadata 模式下光标不画进画面，它的位置 /
      位图是**每一帧的元数据** —— 跟画面内容一样属于「这一帧长什么样」。而帧的推手只有
      两个：**输出提交**（屏幕上有东西重画）和 **1 秒一次的心跳**。指针在**静止的窗口**
      上移动时这两个都不发生（硬件光标移动不产生场景损伤、也就不产生输出提交），于是
      消费者手里一直停在上一次那一帧的光标状态上 —— 那一次很可能正好是「光标在取景框
      外面」（`id=0` → 不画）或者位置早就过期了。表现就是「光标时有时无 / 跟不上手」。

      - 新增 `vidow_screencast_cursor_moved()`（`src/screencast.c`），在
        `vidow_pointer_motion()` 最前面调（设备 / 注入 / warp 三条路都走它，而且拖着东西、
        摇晃那些分支提前 return 也一样催到）。只催 `pointer_mode != hidden` 的流（画面里
        有光标的 embedded 同理；hidden 不用），**每条流节流到 ~60Hz**（指针事件可以几百 Hz，
        每个都抓一整帧没必要）。没有这种流时一次链表遍历都不做。
      - 顺手把「发没发位图」变成日志：位图真的附上去时记一条
        `光标位图发过去了（24x24，第 1 张，第一次）`（只在变了的时候发，不会刷屏），
        流结束时那条汇总也带上 `（其中 N 帧是指针动了催的）`。下次「消费者那边没光标」
        的报告，一眼能分出是「我们没发位图」还是「消费者没画」。
      - **回归**：`test-portal-own.sh` 从 50 项到 51 项 —— 窗口流那段之后，把指针在
        **静止的窗口**上来回挪两下，断言合成器日志里出现「指针动了 —— 这一路开始跟着
        指针推帧」。全部 51 项过。

187. **窗口只有一点在视口里的时候，表面被横向拉伸 + 一条黑边**（用户报「如果一个窗口在
      视口外（但有一点在视口内）则会窗口表面横向拉伸变扁扁的，且会随着窗口在视口内的范围
      大小出现不同程度的黑边」）。

      - 根因是**越界裁剪那一刀**和**装饰采样**抢同一个字段：`vidow_view_apply_clip()`
        （`src/view.c`）裁「跨出自己那块屏」的窗口时走 wlroots 的
        `wlr_scene_subsurface_tree_set_clip()`，而它是靠改场景节点的 **`src_box`**（+ 显示
        尺寸）实现的；装饰管线（fx）采源缓冲的哪一块**正是从 `src_box` 算出来的**
        （`fx_geo_tex_map()` 把它当成「客户端用 wp_viewport 声明的有效区」—— 那是 165 那会儿
        为 chromium 修的）。于是裁完之后，装饰把「裁剩下的一小条」当成整块有效内容，把那一小条
        **拉伸铺满整块窗口**：可见的那一条里内容横向拉扁，剩下的是那圈没画过的 CSD 留白（黑）。
        在视口里的范围越窄 → 采样到的条越窄 → 黑边越大，「不同程度的黑边」就是这么来的。
        （以前那儿写着「裁了也无害」—— 害就在 `src_box` 这一条上。）
      - 修法：装饰管线在画这个窗口的时候（`view->fx.active`）表面那一份是**隐藏的**（0 透明度，
        `view_anim_bake` 里那次 `set_opacity`），那就**不裁**它 —— 装饰那份离屏缓冲本来就按
        同一个框裁（`fx.c` 的 `crop_src`），屏幕上的裁剪一点不少。装饰关掉的时候（表面自己画）
        才走 `set_clip` 那条路；装饰是后接上的（改 `decor_enabled` / 弹出物变回普通窗口）时
        把之前那一刀撤掉（`clip_local` 记着有没有裁过）。
      - **回归**：新增 `./test-clip-stretch.sh`（10 项，装饰开 / 关各 5 项）：三列
        0.9 / 0.7 / 0.5 装不下屏（2688 > 1280）→ 焦点逐列试一遍，直到出现「左边被切掉、右边
        还在屏里」的那一列（只有这种越界才真的把表面树裁窄；配比这么挑是为了让「视口夹在布局
        末尾」那个偏移量 1408 落在第二列里，跟三列的先后顺序无关）→ 用 `tools/pattern.py` 的
        四象限当标尺量屏幕上那一条：**一个黑像素都没有**、四象限分界落在窗口中心 ±4px、上下
        两半的颜色是「可见的那一段窗口」该有的。旧代码：装饰开着那节必挂（实测 112/240 个黑
        像素 + 分界找不到），装饰关掉那节照过（表面自己画那条路本来就是真裁）。

186. **全屏的窗口按 `Mod+V` 变浮窗时先把全屏退掉**（用户报「OBS 窗口一直叠在最上面，不管
      平铺还是浮动」）。

      - 层级是「平铺 < 浮动 < 全屏」（`vidow_restack`），全屏的窗口永远压在所有窗口上面。
        用户那个窗口是**全屏**状态（Mod+F 过 / 客户端要的 / 上一版留下的），`Mod+V` 只换了
        `float_mode`，`view->fullscreen` 还在 —— 于是「不管平铺还是浮动」都压着别人。
      - `vidow_float_toggle()`：变浮窗之前先 `vidow_view_unfullscreen(view, "变浮窗")`
        （和最小化那条路一个做法，见 `vidow_view_set_minimized`）。
      - 顺手两处相关的：
        * **全屏状态现在有日志**（`全屏: X 开/关（谁要的）`）。这条状态以前只有 IPC 事件、
          run.sh 的日志里一个字都没有 —— 「某个窗口一直压着别人」这种报告第一件要看的就是它
          是不是全屏，现在一眼看得出来。所有改全屏状态的地方（Mod+F / 客户端请求 / X11 /
          开窗抢跑 / 最小化 / 变浮窗）都收口到 `view_apply_fullscreen()` 一个入口。
        * 浮动的全屏窗口也摆成整屏（以前只有「列里的全屏」会摆，浮动的那种「全屏」在层级上
          压着所有人、画面上却还是它自己那一小块）；全屏期间不采纳客户端尺寸，退出全屏之后
          浮窗回到原来的大小（不然 Mod+F 开一下关一下，浮窗就变成整屏那么大）。
      - **回归**：`test-floatstack.sh` 新增一节（全屏 → Mod+V → 全屏标志清掉 + 日志 + 缩回去
        之后底下的浮窗露出来）。**旧代码这一节必挂**（`fullscreen=true` 还在，后面几个取样点
        全被它盖住）。test-floatstack 从 13 项到 18 项全过。

185. **平铺窗口的 `Mod+右键拖动` = 改列宽（niri 的 interactive resize）**（用户报「右键
      resize 窗口的快捷方式没有绑定」）。

      - 以前 `Mod+右键拖动` 只对**浮动**窗口有效（`src/input.c` 里那条 `mod && button ==
        BTN_RIGHT && view->float_mode != VIDOW_FLOAT_NONE`），平铺窗口按下去什么都不发生。
      - 新增 `VIDOW_GRAB_RESIZE_TILE`（`src/drag.c` 的 `vidow_tile_resize_begin/update/end`）：
        按住时记下光标 x 和那一列的像素宽，之后每步 `新宽 = 起拖宽 + 光标位移` → 换算成
        `width_fraction`（夹在 0.05…1.0）→ `vidow_arrange()`，窗口跟着手走；松手留着宽度。
        **按增量算**而不是「右边缘 = 光标 x」：列一变宽视口就会跟着滚（focused 那列要整个
        可见），按绝对位置算下一帧光标底下的右边缘就跑了，手会抖。手动给宽度 = 退出「列最大
        化」（和 `set-column-width` 一个口径）。
      - 只横向：平铺窗口的高度是布局给的（整块可用高），vidow 没有「单个窗口的高度」这个
        概念（只有浮窗有自己的几何），纵向拖不改东西、松手也不留副作用。浮窗那条路照旧
        （先匹配浮窗分支，平铺这条接不到浮窗上）。光标图标换 `ew-resize`；IPC 的
        `state.grab.mode` 报 `resize-column`。
      - **回归**：新增 `test-tile-resize.sh`（14 项）：拖 +200/+400 像素列宽正好 ±200/±400、
        拖动期间 `grab.mode = resize-column`、松手宽度留着、两头夹在 100% / 5%、浮窗那条路
        没被抢（拖浮窗改的是它自己的几何、列宽一个字不动）。相关：test-drag 11 项、
        test-floatstack 13 项、test-ipc 162 项、test-binds 35 项全过。

184. **两个 Start 同时来：排队，不拒**（用户报「第一次无法录制 / 第一次弹出来的选择器不管选
      什么都黑屏」）。

      - **根因**：OBS 的场景里有两条「屏幕采集(PipeWire)」源（`VTS` 和 `屏幕采集(PipeWire)`），
        开机时两条都去问 portal。OBS 自己的日志就是现场：两条 `[pipewire] Screencast session
        created` + 两条 `Asking for monitor and window`，然后一条
        `Failed to start screencast, denied or cancelled by user` —— 被拒的那条源就黑着，得用户
        手动再点一次「选择屏幕」。vidow-portal 以前是「已经有一个对话框开着，先拒了」，回
        `RESPONSE_OTHER`，前端翻成「denied or cancelled by user」。
      - 修：**排队**（KDE 的 portal 也是排队）：第二个 Start 挂在 `queued_starts` 里不答复，
        当前这个选完 / 关掉之后接着弹下一个（`start_show_next`）。配套三处：`request_finish`
        只在**正在弹的那个**请求上拆对话框 / 停建流计时器（排队里那个被收掉时不能顺手把别人
        的对话框拆了 —— 拆了那个请求就收不到答复）；请求对象上的 `Close` 按注册时挂的
        `user_data` 认是**哪个**请求（以前不看，排队的一被 Close 就把正在弹的收掉）；
        `session_close` 把该会话排着队的请求先摘下来再收（`request_finish` 会动队列，边遍历
        边收要踩链）。
      - 光标那边**加诊断**（还没定论）：`stream_fill_cursor` 以前只在「第一次查光标」时打一条
        INFO，现在改成**状态一变就打**（`光标在这一路里 —— 流里 x,y…` / `光标不在这一路里 ——
        这块屏上没有可见光标（指针在别的屏上？）` / `…在取景框外面…` / `…取景框算不出来（窗口
        停到屏外了）`）。用户那次 run 日志里是「这块屏（DP-3）上没有可见光标」—— 录的是 DP-3
        上的窗口、鼠标当时在 DP-2 上；按窗口录屏的语义（KWin 同）光标只在它压在**被录的那个
        窗口**上时才进画面，这一条待跟用户确认要哪种。
      - **回归**：新增 `test-portal-queue.sh`（直接调后端 impl 的 DBus 接口：两个会话同时
        Start → 第二个排队不秒回；关掉第一个 → 第二个的选择器接着弹、两个都拿到答复；排着队
        的时候关会话 → 那个请求被收掉、不多弹一个对话框）。18 项全过；**旧代码跑同一套必挂**
        （`已经有一个对话框开着，先拒了` + `(uint32 2, …)`）。相关：test-portal-own 50 项、
        test-screencast 10 项、test-portal 全过。

183. **窗口抓帧源尺寸中途变了 = 合成器段错误（`copy_buffer` 之后二次 `fail` 的 use-after-free）**
      （用户报「直接崩溃」，`./dbg.sh` 的 coredump 栈：`wl_resource_post_event ←
      wlr_ext_image_copy_capture_frame_v1_fail ← source_copy_frame ← source_request_frame`）。

      - **根因**：`wlr_ext_image_copy_capture_frame_v1_copy_buffer()` 在**尺寸对不上**时自己就
        `fail(frame)` + `return false`，而 `fail` 会把 frame 销毁掉（wlroots
        `types/wlr_ext_image_copy_capture_v1.c` 三条失败路都是 `fail(); return false;` ——
        返回 false 就等于「我已经回过了」）。`src/wincapture.c` 的 `source_copy_frame` 看到返回
        false 又补了一句 fail：拿一块已经 free 掉的内存去 `wl_resource_post_event`，si_code
        SEGV_MAPERR，当场崩。触发条件是 182 之后才有的：窗口源的尺寸是「窗口逻辑尺寸 × 那块屏的
        scale」，窗口在缩放不同的两块屏之间挪一下尺寸就变（用户日志里的现场：新窗口一开 → 焦点
        变 → 布局重排 → obs 那个窗口从 4K 缩放 2 那块屏挪到 1 倍那块，尺寸 1908x2136 → 954x1068，
        紧接着一次新的 `WINDOW-CAPTURE` 交换链分配），选择器那一路缩略图抓帧手上那块缓冲立刻
        对不上 → 必崩。
      - 修：`source_copy_frame` 拷不进去时**什么都不做**（客户端已经收到 failed 了），只在尺寸
        对不上时先记一条 INFO（`wincapture: X 这一帧 WxH，客户端缓冲 WxH —— 尺寸变了，这一帧回
        failed`），免得日志里只剩一句「拷不进去」。合成器那三个抓帧源（窗口源 / 输出源 / 光标源）
        都过了一遍，只有窗口源这一处多补了 fail。
      - 客户端侧也得跟着换缓冲（不然合成器每一帧都回 failed）：`tools/vidowportal.c` 的选择器
        缩略图会话以前只在「还没缓冲」时建（约束变了不换），预览就一直是空的（用户说的黑屏）。
        现在 `win_shot_alloc_buffer/free_buffer` 抽出来，`done` 里发现尺寸变了就换 —— 有帧在飞就
        等它回来再换（那块缓冲还挂在它身上）；`tools/capture.c`（测试客户端）同样在
        `capture_next` 里按新约束换缓冲。
      - **回归**：新增 `test-wincapture-scale.sh`（两块屏 1 倍 + 2 倍，窗口在两者之间挪）：验
        「合成器重发 `buffer_size`」「2 倍屏上的尺寸正好是 1 倍屏的两倍」「整个换屏只回一次
        failed」「failed 之后接着出帧（换缓冲自愈）」「合成器没崩、没新 coredump」。**修之前这一套
        必挂**（跑了一遍旧代码：换屏那一刻段错误 + 新 coredump），现在 17 项全过。相关：
        test-force-render 28 项、test-scrollcull 34 项、test-capture-hide 30 项、test-portal-own
        50 项全过，test-screencast / test-portal / test-mixdpi 全 rc=0。
      - 还没验的：真实 OBS 会话里把窗口在 DP-3 / DP-2 之间拖一次（选择器的缩略图那条路）。

182. **录屏：portal 报的流尺寸改成「像素」，跟合成器开的缓冲一个口径**（用户报「2 倍屏上录屏
      看不到光标、OBS 第一次弹选择器不管选什么都黑屏」）。

      - **根因**：`vidow-portal` 报给前端的 `size` 是**逻辑**尺寸（monitor 流 `out->w = px_w /
        scale`、窗口流用缩略图的逻辑尺寸），而合成器给这一路流开的缓冲是**设备像素**
        （monitor 流 = `output->width/height`；窗口流 = 窗口逻辑尺寸 × 那块屏的 scale）。缩放 2
        的屏上正好差一倍。光标的 `SPA_META_Cursor` 位置按协议就是**流像素**（对的那一边），
        消费者拿 size 当元数据换算 / 判越界 —— 2 倍屏上位置判成越界就不画（挪到 1 倍那块屏
        就正常），协商缓冲那条路对不上就是黑屏。用户日志里两个数并排摆着：`框 1920,0
        1920x1080 缩放 2.000`、流 `3840x2160`、portal 却报 `1920x1080`。
      - `tools/vidowportal.c`：monitor 流报 `px_w/px_h`（`x/y` 仍是布局逻辑坐标，那是前端要的
        口径）。窗口流那条：窗口抓帧源（`ext-image-copy-capture`，portal 拿它的约束当窗口 size）
        以前按**逻辑**尺寸 1:1 渲染，现在按「逻辑 × 窗口那块屏的 scale」渲染并声明约束
        （`src/wincapture.c` 的 `win_source_size`）—— 顺带把窗口截图 / 缩略图从「缩放屏上糊的」
        变成原生像素。选哪块屏的判据抽成 `vidow_win_grab_output()` 并**和 `handle_stream_window`
        共用一份**（两边选屏不一样时报出去的 size 又会漂）。
      - 诊断：录屏第一次查光标的结果走 INFO（`metadata 第一次：…`：有没有光标 / 位置 / 位图 /
        流尺寸 / 取景框 / 缩放），`cursor_dst_pos` 的每个输入走 DEBUG；`./dbg.sh` 退出时自动打
        「退出码 + 新 coredump 的栈 + 录屏日志」，并忽略 SIGPIPE（Ctrl+C 不再被当成「141 崩了」）；
        `run.sh` 的 coredump 块 8 帧 → 26 帧，带录屏日志。
      - **退出时的段错误**：`server.shutting_down`（`wl_display_run()` 一返回就立牌子）。原因是
        `wl_display_destroy()` 会一块块 finish 后端，键盘 / 输出那些 finish 还会再发一轮事件进
        我们的监听器，而 seat / 工作区链那时已经拆过一半（coredump：`keyboard_key →
        wlr_seat_set_keyboard`、`output_gone → vidow_workspace_count` 读 `link.next`，si_addr 都
        是 0x8）。
      - **回归**：`test-portal-own.sh` 的 HEADLESS-2 改成「2560x1440 模式 + 缩放 2」（= 用户那台
        机器的几何），新增「报出去的 size == 这一路流的缓冲尺寸」（monitor / 窗口各一条；缩放 1
        时它们也过，缩放 2 时旧代码必挂），光标位置那几条改成按 scale 现算；双击确认加一次重试
        （GTK 双击判定窄，漏一次后面的轮次会连着报 code=2）。test-portal-own 50 项全过；
        test-capture-hide / test-screencast / test-portal / test-ipc / test-workspace /
        test-hotplug / test-binds / test-mru / test-dmabuf-update 全 rc=0。

181. **Alt+Tab 的最近窗口切换器（niri 的 recent windows，新增 `src/mru.c`）**（用户要「niri
      样式」：按住显示、连点 Tab 换窗口、松开提交焦点；样式可配，包括「超出范围不滚动、
      换行铺在屏幕里、一行限制窗口数量、窗口多时预览自适应变小」）。

      - **先纠正一件事**：niri 的 Alt+Tab **不是** overview。它有自己独立的一套（25.11 起
        自带：wiki `Configuration:-Recent-Windows`、源码 `src/ui/mru.rs`）—— 一行 MRU 预览条 +
        选中框 + 80% 黑底 + scope 面板，`Alt+Tab` / `Alt+Shift+Tab` 连点换、松修饰键提交、
        `open-delay-ms 150`（快速点一下只切窗口、不闪屏）、`debounce-ms 750`（路过的窗口不进
        台账）、`Alt+\`` 同应用过滤。我先前说「niri 没有 MRU 切换器、overview 就是它的
        alt+tab」是错的；核了三处（wiki / 源码 / 本机 niri 26.04 的二进制）之后按它抄。
      - **台账不维护链表**：每个窗口一个 `mru_ts` 时间戳，要列的时候现排一遍 —— 窗口开关 /
        换工作区 / 销毁都不用同步第二份数据（也就没有悬空指针）。debounce 用「pending +
        一次性定时器」实现，往窗口里打字立刻落账。
      - **预览 = 真实窗口 + 缩放变换**（跟 overview 同一套），不抓图、不多一趟渲染；跨工作区 /
        跨屏的窗口临时拉进场，清单外的窗口停到屏外，滚出工作区的预览不画（场景是全局坐标，
        不画的话它会落到隔壁屏上）。摆位（`view_anim_place`）和重烘（`view_anim_bake`）都问
        同一个 `vidow_mru_place()`，保证「画在哪」和「按哪个位置烘」是同一份。
      - **交互按「松开才提交」**：按住期间焦点**先不动**（`active` / `showing` 分开：`open_delay`
        到点前面板不显形、画面一点都不动），松开修饰键（modifiers 监听 + 按键路径双保险）才把
        焦点给选中的那个；`Esc` 取消、`Enter` 确认；切换器开着期间键盘归它管。
      - **两种排版**：strip（一行 + 横向滚到「选中的那个完整可见」）和 grid（按 `mru_per_row`
        换行 + 装不下就整体缩小，下限 `mru_min_scale`；取整会溢一两个像素，所以留了 2px 余量）。
      - **配置**：`mru = { … }` 一族 15 个键（新的一节 `configs/15-mru.lua` —— 排在 `14-keys.lua`
        之后：那个文件名早就写进用户的 `config.lua` 了，不能往前插），两个枚举（`scope` /
        `layout`）在 config.c 里新加了 `K_MRU_SCOPE` / `K_MRU_LAYOUT` 两种 kind，写错当场报错。
        默认键位加进 `configs/14-keys.lua`：`Alt+Tab` / `Alt+Shift+Tab` / `Alt+\``。
      - `./test-mru.sh`（新，32 项断言，约 40 秒）：台账顺序、按住时焦点不动、连点往前走、
        松开提交、`Esc` 取消、快速点一下（按住 < open_delay）不显形但窗口照样切、strip 滚动后
        选中的那个完整可见、grid 3 行 / 每行 ≤2 / 整体缩小 / 全在屏内、面板显形时壁纸被压暗。
      - IPC 加了 `mru`（台账 + 面板几何，测试的几何断言靠它）。
      - 顺带：`view_clip_box` / `vidow_view_should_park` 各加了一条「切换器里的预览不按自己
        那块屏裁 / 不按工作区藏」的判据 —— 否则跨屏、跨工作区的预览会被切掉或根本不画。

180. **`blur_xray`（Hyprland 的 `blur:xray` 同款）+ 默认 `blur_levels` 4 → 3**（用户问「模糊
      路线哪一种比较好」，听完对照后拍板：加这个开关、默认 pass 改成 3）。

      ① **先把三条路线摆清楚**（都能落到证据上）：

      | 路线 | 谁在用 | 每帧成本 | 模糊源里有什么 |
      | --- | --- | --- | --- |
      | 整屏预模糊、所有窗口共用一份 | Hyprland 的窗口那路（`CPreBlurElement` → `m_blurFB`） | 1~2 趟整屏，不随窗口数翻倍 | 到「平铺层画完」为止的整屏；`xray = true` 连平铺窗口都不要 |
      | 每个元素私有底衬（场景减掉自己） | vidow / niri | 每个**重叠**元素 1 抓取 + 2·级数−1 趟 | 真·身后内容：重叠窗口的模糊里看得见对方 |
      | 每个 layer surface 各自当场糊 | Hyprland 的 BACKGROUND / BOTTOM 层（`blockBlurOptimization = true`） | 每 surface 一趟，顺序渲染 | 前一个 surface 的内容会被吃进后一个的模糊源 —— 就是那个接缝 bug（[#15125](https://github.com/hyprwm/Hyprland/discussions/15125)） |

      Hyprland 的默认值取向本身就是「省」：`passes = 1`、`size = 8`、`new_optimizations = true`
      （wiki 原话「massively improve performance」）、`xray`（「will reduce overhead on floating
      blur significantly」，[config-options](https://wiki.hypr.land/configuring/core/config-options/)）。
      两边的趟数不能直接比：vidow 默认的模糊半径比 `size = 8` + 一趟大一个量级，贵的那部分
      有一半是「糊得更狠」换来的。

      ② **vidow 本来就是混合路线**：不重叠（`view_overlaps_other()` 为假）时所有窗口共用整屏
      那条链，只在壁纸 / layer 变代次时重算 —— 纯平铺用户付的就是第一条路线的钱。只有真的
      叠在别人身上的元素才各自开私有底衬。

      ③ **加 `blur_xray`（默认 `false`）**：`fx.c` 里那个 `if (!view_overlaps_other(view))`
      多一个 `|| server->config.blur_xray` —— 重叠时也用整屏那份底衬（壁纸 + layer 那层），
      **忽略别的窗口**。观感上「糊的是壁纸，不是压着的那个窗口」（Hyprland 的 `blur:xray`
      同款），换来的是**一块私有底衬都不建**。默认关：真背景模糊是这个合成器的卖点，弱 GPU /
      4K 高刷想再压一压的人自己打开。

      ④ **默认 `blur_levels` 4 → 3**（≈65px → ≈29 设备像素半径）：每个模糊元素每帧少两趟
      离屏渲染（实测四个重叠元素：33 趟/帧 → 25 趟，6.7% → 6.3% 单核）。要更糊写 4 就行。
      （179 里我说「没动默认值」—— 那时是；这次用户拍板改了。）

      ⑤ **判据 `./test-blurxray.sh`（新，6 项断言）**：同一场景跑两遍，只差 `blur_xray` ——
      一块纯红浮窗 + 压在它上面的半透明白浮窗。**关**：`own` 累计 **45**、`chain` **48**；
      **开**：`own` **0**、`chain` **3**（只剩整屏那条链）。语义也真的变了：白窗压着红窗那块
      区域的 `mean(R−B)` 从 **+148.5** 变成 **−37.6**（红色不再渗进模糊）。也就是说这条开关
      在这个场景里把链的趟数砍掉 **94%**，而观感代价就是「那圈糊的是壁纸」。

179. **性能热点复查：vidow 自己的代码只占 2.7%，剩下能省的是「空载每秒醒 20 次」**（用户让
      「检查还有什么性能热点可以优化」，看完点名要改 1 / 2 / 4 / 5 四条）。

      ① **先量，再说话**。本机 `perf_event_paranoid=2` 挡掉了 `-g`（调用栈）也挡掉了 attach
      模式（`perf stat -p` 全是 `<not counted>`）—— 只有**启动模式**能采：`perf record -e
      cycles:u -F 999 -- setsid ./build/vidow`。场景：headless 2560x1440 缩放 2，4 个 kitty +
      两个互相重叠的模糊浮窗 + 一个不停重画的窗口（`tools/spin.py`），稳在 59fps。**进程 CPU
      的分布**：

      | 谁 | 占进程 CPU |
      | --- | --- |
      | libgallium（mesa 驱动） | 42.2% |
      | libc（`0x1b1fa0` 那段 `vmovdqu` = AVX2 memcpy） | 19.9% |
      | 未符号化 | 12.6% |
      | libwlroots（场景遍历 / addon / box） | 8.2% |
      | libpixman（`region32_intersect` / `init` / `union`） | 5.8% |
      | **vidow 自己** | **2.7%**（最高一项 `vidow_blur_update` 0.26%） |

      也就是：**我们自己一帧的代码里没有大热点**，CPU 都花在驱动提交（按趟数走）、驱动侧的
      整块拷贝和 wlroots 的场景/损伤记账上。vidow 自身 6.7% 单核、GPU busy 7.6%（4K@160 的
      7900XT 远没吃饱）。**量不到的部分如实记着**：没有调用栈，所以那 20% 的 memcpy 只能定位到
      「库」这一层；pixman 那 5.8% 是 wlroots 场景损伤算的（我们的 `bgeffect.c` 在这套场景里
      根本没被触发）。

      ② **改掉的三条**：

      - **空载唤醒：50ms 常开的抓帧看门狗改成按需启动**（`src/output.c` 的 `capture_watchdog`
        + `vidow_output_capture_kick`，`src/screencopy.c` 插入帧时踢一脚）。它原来是「建好就
        50ms 一次永远转下去」—— 静止的屏幕永远不提交、抓帧要靠一次提交应答，所以得在；但没
        人在抓帧的时候它也是每秒醒 20 次。实测（strace，空载 21 秒）：`epoll_wait` 返回 **427
        次**、`timerfd_settime` **852 次**，期间一次抓帧都没有。改成按需之后：**10 秒 0 次**。
        空载 CPU 本来就只有 0.05%，所以这条省的不是 CPU，是**进不了深睡**（笔记本 / 掌机上的
        续航和待机）。
      - **每帧的 `getenv` 缓存**（`src/output.c` / `src/fx.c`）：`VIDOW_RENDER_STATS`（每帧两次）、
        `VIDOW_TEST_STALL_OUTPUT`（每帧一次）、`VIDOW_BLUR_NO_PARTIAL`（每次重抓两次）、
        `VIDOW_BLUR_DUMP` / `VIDOW_FX_DUMP`（重抓路径上）—— 一帧十来次，而 profile 里
        `getenv` 一个人占 **0.39%**，比我们自己的模糊代码还显眼。进程起来之后环境不会变，缓存住。
        `VIDOW_BLUR_NO_PARTIAL_FILE` 的**文件存在性每次照看**（`test-blurpartial.sh` 就是跑着跑着
        把文件建出来切过去的）。
      - **注入的指针移动日志按 100ms 分档**（`src/input.c`）：同一个 100ms 窗口里第一条走
        **info**、剩下的走 **debug**。注入是给脚本 / 测试用的，能比真鼠标还快（基准里灌过
        20000 条连发），每条一行 info 既刷屏又拖慢注入本身。实测：连发 5 次 `move-cursor`，
        `VIDOW_LOG=info` 多 **1** 行、`VIDOW_LOG=debug` 多 **5** 行（一条不漏）。

      ③ **判据 `./test-idlewake.sh`（新，7 项断言）**：用 `/proc/<pid>/status` 的
      `voluntary_ctxt_switches` 数唤醒 —— 空载 10 秒增量必须 < 20。**反证**：把看门狗临时改回
      「建好就排、每次都续期」再跑，同一台机器同一个场景量到的是 **200**（正好 20Hz）；改回按需
      之后是 **0**。另外三条守着「按需启动没把抓帧弄坏」：从没提交过一帧的空屏上 `grim` 照样抓
      得到（而且确实是全黑）、静止屏幕上连续两次抓帧逐像素一样、抓完之后 5 秒增量仍然 < 10
      （看门狗自己停了）。

      ④ **第 2 条（每帧 33 趟离屏渲染）查完之后没有安全改法，只把杠杆写进了配置说明**。趟数
      = 「有多少个互相重叠的模糊元素」×（1 趟抓取 + 级数 + 级数−1 趟 Kawase），实测 A/B
      （同场景）：4 级 33 趟/帧、6.7% 单核；3 级 25 趟、6.3%；2 级 17 趟、5.8%。唯一能直接降
      的是 `blur_levels`，但它**明显改观感**（半径 ≈ `blur_offset × (3×2^级数 − 5)` 设备像素：
      offset=1.5 时 4 级 ≈65px、3 级 ≈29px），所以没动默认值，而是把这个账写进了
      `blur_levels` 的配置说明（`src/config.c` + 重新生成的 `config.example/configs/03-blur.lua`）。
      「不重叠的窗口共用整屏那条链」本来就有（`view_overlaps_other`）；再往下要省只能重构
      「场景抓取在多元素之间共享」，那是另一件事。

      ⑤ 回归：`test-idlewake.sh` 7/7、`test-input-inject.sh` 54/54、`test-blur*` 全绿、
      `test-frametrace.sh` / `test-drag-stall.sh` / `test-capture-hide.sh` 全绿。全套 67 项里
      **66 项 rc=0**，唯一一条红的是 `test-maximize.sh` 最后那句「收尾还剩 1 个 vidow」——
      **单跑 60/60 全过**，把可能挨着的那一串重跑一遍（`test-lastoutput` → `layerblur` /
      `layerdestroy` / `layerghost` / `layermask` / `layerwin` → `test-maximize`，每步都数
      `pgrep -x vidow`）**全程 0 残留**，复现不出来。这条断言是「进程计数」，和这次改的
      抓帧看门狗 / getenv / 注入日志三条路都不搭界（它们不碰列最大化），先按 harness 的偶发
      记在这儿。

178. **配置说明补全：打出来的那份默认配置就是配置项的手册，它得全**（用户报的「配置里面的
      说明不完整，没有列出所有配置项，需要调整」）。

      ① **先量了一遍**：`config.example/` 里 64 个平铺键一个不缺 —— 缺的全是**不是配置键**的
      那些，一共 **22 项**：12 个 `vidow.*` 函数（`exec` / `exec_status` / `get` / `log` /
      `ipc_path` / `rules` / `workspace_active` / `workspace_list` / `workspace_move` /
      `workspace_special` / `workspace_switch` / `workspace_toggle_special`）、
      `vidow.rule{…}` 的 8 个字段（`class` / `namespace` / `focus_on_open` /
      `focus_on_activate` / `honor_invalid_serial` / `hz` / `x11_scale` / `capture_mosaic`）、
      `vidow.output{…}` 的 2 个字段（`mode` / `transform`）。它们不在 `entries[]` 里，所以原来
      那套「打出来的键 == IPC 的 config 键」的检查盯不到 —— 这就是漏掉的机制。

      ② **改法**（都在生成配置的 C 字符串里，`src/config.c`）：
        - `configs/14-keys.lua`：窗口规则那块从「一行例子」扩成完整的字段表（4 个匹配字段 +
          8 个作用，每个作用带上取值和「至少写一个」这条规矩），下面再加一张 `vidow.*` 函数
          总表（配置键 / 窗口规则 / 工作区 / 输出 / 跑命令与日志，**18 个函数一个不少**，
          每个一句话说明，工作区那几个写清是「按名字」而不是序号）；
        - `configs/09-output.lua`：`vidow.output{…}` 从 3 行例子扩成 9 个字段的说明
          （`name` / `enabled` / `scale` / `x` / `y` / `mode` / `vrr` / `depth` / `transform`，
          含 `transform` 的 8 种取值、`mode` 的三种写法、`depth` 只能 8 或 10）。
        改完 `config.example/` 重新生成（还是 `--write-config` 写出来的那一套，动的就是这两份）。

      ③ **加一组判据，免得再漂**：`test-defaultconfig.sh` 新增一节，直接从源码里抠名字 ——
      `config.c` 的 `entries[]`（64 个平铺键）、`*_fields[]`（49 个族项）、`script.c` 里注册的
      `vidow.*`（18 个）、以及 `vidow.rule` / `vidow.output` 读的表字段（19 个，`lua_getfield` /
      `table_bool` / `table_string` 三种读法都算上）—— 逐个去 `config.example/` 里找，少一个就红。
      **反证**：改之前这一节正好 22 项红（就是①里那 22 项）。README 的族表也顺手跟代码对了一遍：
      `portal` 那行少了 `capture`，补上（其余 10 个族一致）。

      ④ 全套 66 项 rc=0；`test-defaultconfig.sh` 从 41 项涨到 **45 项**。

177. **补上列最大化（niri 的 `maximize-column`）**（用户问「现在有没有窗口全屏的功能？顺便补上
      窗口最大化」）。

      ① **全屏本来就有，最大化确实没有**：`toggle-fullscreen`（`Mod+F` / IPC）走的是客户端
      `set_fullscreen` + 占满工作区 + 置顶；而最大化的两个信号（`view_request_maximize` /
      `x11_request_maximize`）是**一律回绝**的（回一个「不最大化」，只为了让客户端别卡在等待里）。
      缺了的后果：想「吃满整块工作区、但别让播放器真的进全屏模式（藏 UI / 换模式）」这件事没别的
      办法；kate 那种「上次是最大化」的窗口每次开回来都是半宽。

      ② **语义选了 niri 的列最大化**（用户选的）：列是 vidow 布局的最小单位（窗口在列内纵向
      均分高度），所以「最大化一个窗口」只能是「最大化它所在的那条列」——整列撑满工作区宽度，
      再按一次还原成**原来那个比例**（`maximized_saved` 记着，0.25 → 最大化 → 还原还是 0.25，
      不是回到默认的 0.5）。状态入口只有一个：`vidow_column_set_maximized()`（`src/view.c`），
      IPC `toggle-maximize-column` / `set-maximize-column`、动作 `maximize-column`（默认键位
      `Mod+Shift+F`，和 `Mod+F` 全屏挨着）、`columns` 里多一个 `maximized`、事件
      `column-maximize-changed`。**手动改宽度 = 退出这个状态**（`cycle-column-width` /
      `set-column-width` / `expand-column-to-available-width` 那几个只改宽度、不记原值）。
      列里每个客户端都会收到 `set_maximized` 的应答（xdg 的 configure / X11 的
      `_NET_WM_STATE_MAXIMIZED_VERT`+`HORZ`），新开进一条已最大化列的窗口也补一个
      —— 客户端以为自己是普通尺寸、布局却按整列宽度摆它，那种「一半一半」的状态最难看。

      ③ **客户端 `set_maximized` 照办，但分两种撤销**（用户选的「照办，映射成同一个状态」）：
      要最大化就最大化它那条列，并记下「这次是它要的」；同一个客户端再撤销就理（列还原）。
      而**从来没要过**的客户端发 `set_maximized(false)`（有些工具包开完窗顺手来一下）不理它
      —— 列是用户按 `Mod+Shift+F` 要来的，照当前实际状态回话就行。浮窗（没有列）照旧回
      「不最大化」。X11 那边还多一层：`wlr_xwayland_surface_set_maximized()` 写下去之后客户端
      可能把 `_NET_WM_STATE` 回写、又触发一次 `request_maximize`（X11 没有请求-应答，状态就是
      属性本身），所以 X11 那条路**状态一致就直接返回**，不给自己回声的机会。

      ④ **判据 `./test-maximize.sh`（新，60 项断言，28 秒）**：headless 一块 1280x720 屏，
      `tools/pattern.py` 造一条对照列，`tools/preinit.c` 当「会报告自己被 configure 成什么状态」
      的客户端（它本来就有 `--maximize` / `--after-commit`，这轮加了 `--none` = 什么请求都不发、
      `--unmaximize-after=毫秒` = 上屏之后再补一个 `set_maximized(false)`）。实测：
        - 0.5 x 1268 = 634 的列按一下 → `maximized=true`、`width_fraction` 1.0、`width` 1268、
          窗口 place 从 `6,6 634x708` 变成 `6,6 1268x708`；再按一次回到 `6,6 634x708`；
        - 先 `set-column-width 0.25` 再最大化再还原 → 回到 0.25（317），不是 0.5；
        - `cycle-column-width` 在最大化状态下按 → `maximized=false` 且宽度是 1/3（满宽再循环
          回到最窄那一档）；
        - 客户端上屏之后请求：列最大化 + 它**第一帧**那个 configure 不带 `maximized`、请求之后
          收到一个带的；抢跑请求（第一次 commit 之前）：合成器没崩，而且**第一个** configure
          就带 `maximized`；
        - 用户（IPC）最大化之后，一个从没要过最大化的客户端发 `set_maximized(false)` → 列
          **还是**最大化的，客户端被告知「你仍然是最大化的」；客户端自己要来的那个撤销 → 列还原、
          之后的 configure 不带 `maximized`；
        - X11：`wmctrl -b add,maximized_vert,maximized_horz` → 列最大化 + `xprop` 里
          `_NET_WM_STATE_MAXIMIZED_VERT/HORZ`；`remove` → 列回到 634、属性撤掉；反向（IPC 最大化
          X 窗口的列）→ X 那边也知道自己最大化了。
      反证（把 `vidow_column_set_maximized()` 改成进门就 return，等于这个功能没实现）：60 项
      只剩 **36 项**通过。

      ⑤ **顺手修掉的三个自己踩的坑**：
        - `vidow_column_set_maximized()` 一开始会把列里所有窗口的 `maximize_granted` 清掉，而
          「客户端要的」那条路正是**先立标记再调它** —— 于是客户端自己的撤销请求被当成「没要过」
          挡回去了（第 9 项红的就是它）。改成只在**关掉**最大化时作废这笔账；
        - X11 的 `request_maximize` 会读到「我们刚设下去的属性」（客户端回写触发），不设
          「状态一致就返回」的话就是无限回声；
        - `vidowmsg` 的位置参数表里没有 `set-maximize-column`，`set-maximize-column true` 的
          `true` 会掉在地上（服务端报「要 maximized = true / false」，测试里被 `2>&1` 吞了）。

176. **键盘那两个协议也接上：`zwp_keyboard_shortcuts_inhibit_manager_v1` + `zwp_xwayland_keyboard_grab_manager_v1`**
      （用户接着 175 说「键盘的也接上」）。

      ① **缺的是什么、怎么确认的**：还是 `strings /usr/bin/Xwayland` —— 175 里补掉的那两个之外，
      剩下的两个就是它们（`zwp_keyboard_shortcuts_inhibit_manager_v1`、`zwp_xwayland_keyboard_grab_manager_v1`，
      也是 175 的注释里写的「那两个 wlroots 0.20 也没实现，是另一件事」）。缺了的后果：
        - **抑制传不出去**：虚拟机的窗口 / 远程桌面客户端要不到「别吃我的快捷键」，
          `Super` / `Alt+Tab` 全被合成器的键位吃掉，进不了客人系统；
        - **X11 的 `XGrabKeyboard` 传不出去**：Xwayland 只能在 X 那边假装抓到了，焦点一换
          （用户点了别的窗口）按键就跑到别处去，抓着键盘的那个客户端再也收不到键。

      ② **两个协议在 wlroots 0.20 里情况不一样**，所以做法也不一样：
        - 抑制：wlroots **有**（`wlr_keyboard_shortcuts_inhibit_v1_create`），它管 global 和
          active/inactive 事件，「抑制到底抑制了什么」是合成器的活 —— 新文件
          `src/keyboardgrab.c` 里接上键位那条路；
        - X11 抓取：wlroots 里**连头文件都没有**，所以服务端自己写：协议 XML 进
          `protocols/`（wayland-protocols 里的那份），meson 现场生成 server-header +
          private-code（和 `ext-background-effect-v1` 一个做法），global 自己建。

      ③ **策略**（都在 `src/keyboardgrab.c`，细节见 README「快捷键抑制与 X11 的键盘抓取」一节）：
        - 抑制：客户端一要就给，但**只有抑制那个表面正是键盘焦点时才算数**（协议原话），
          IPC 报 `input.shortcuts_inhibited`；算数时 vidow 自己的键位全部让路，按键原样给客户端。
        - **逃生口**：`vidow.bind(键, 动作, { inhibited = true })` 的那条绑定在抑制期间照样生效
          —— 不给这个口子，一个抑制着的窗口就能把用户锁死在里面（连关窗都关不了）。默认配置里
          的 `Mod+Escape`（退出）就绑成了逃生口（`configs/14-keys.lua` 与 `--default-config`
          两处同步改，`test-defaultconfig.sh` 会逐字比这两份）。
        - X11 抓取：把**座位键盘焦点**按到抓取那个表面上（只挪 seat 焦点，`server->focused`
          和焦点边框不动 —— 和弹出物抓键盘一个规矩）；之后每次要送按键都再确认一遍，**谁抓谁收，
          不管焦点在谁身上**（X11 的语义）；对象销毁就把焦点还给 `server->focused`。vidow 自己的
          键位照常生效（协议明说合成器可以过滤一部分自己用）。访问限制：只接受 Xwayland 的表面
          （协议要求「只给 Xwayland」），别的客户端来要就回协议错误。
        - IPC 的 `input` 多了 `shortcuts_inhibited`（bool）与 `keyboard_grab`
          （`null` 或 `{type: xwayland, window}`）。

      ④ **判据 `./test-keyboard-grab.sh`（新，30 项，16.6 秒）**：headless 一块 1280x720 屏，两个新
      客户端 `tools/keyinhibit.c`（Wayland：要抑制 / 收按键 / 打 keycount）、`tools/xkeygrab.c`
      （X11：`XGrabKeyboard` + 收按键），真按键用 `wtype` 走虚拟键盘造（注入的键不触发键位，
      量不了抑制）。实测：
        - 对照组（同一个窗口、没有抑制）：`Mod+F` 生效 → 窗口全屏，再按一次退回来；
        - 抑制：客户端收到 `active`、IPC `shortcuts_inhibited = true`、同一个 `Mod+F`
          **不再生效**（还是不全屏）而按键到了客户端（keycount 0 → 2）；
        - 逃生口：抑制期间 `Mod+O`（绑成 `{ inhibited = true }`）照样生效（floating false → true）；
        - 撤销：客户端销毁 inhibitor → IPC 回到 false、`Mod+F` 又生效；
        - X11：`XGrabKeyboard` 之后 IPC `keyboard_grab = {type: xwayland, window: 5}`（就是
          「Xwayland bind 上了这个协议」的直接证据）；**把焦点点回 Wayland 窗口**（#3）之后
          注入 `Return`，X 客户端照样收到（keycount 0 → 2、X 键码 36 = evdev 28 + 8）、焦点窗口
          一个键都没收到；撤销抓取 → IPC 回到 `null`、焦点回到 #3、按键也回到 #3（keycount 0 → 2）。
      反证（把 `vidow_keyboard_grab_init` 那一句摘掉重跑同一条）：30 项只剩 **5 项**通过。

      ⑤ **顺手修掉的两个自己踩的坑**：
        - `wlr_keyboard_shortcuts_inhibit_manager_v1` 在 display 销毁时同样会断言
          `events.new_inhibitor` 上没人挂着（175 里指针约束踩过一次），所以 manager 的
          `events.destroy` 里把监听摘干净；
        - grab 对象销毁时我一开始先 `grab_restore_focus()` 再从名单上摘自己 —— 于是
          「还有没有别的 grab」把自己算进去了，焦点留在刚没的那个表面上不还给焦点窗口
          （测试第 26 项红的就是它）。改成**先摘再算**。

175. **补上 XWayland 缺的那两个协议：`zwp_pointer_constraints_v1` + `zwp_relative_pointer_manager_v1`**
      （用户报的「xwayland 我记得之前说好像缺了什么协议 补一下」）。

      ① **缺的是什么、怎么确认的**：看本机 `/usr/bin/Xwayland` 的字符串 —— 它 bind 的就是这两个
      （`lock_pointer` / `confine_pointer` 两个请求 + `zwp_relative_pointer_manager_v1`），而
      vidow 之前一个都没建（`src/main.c` 的 global 清单里只有 compositor / xdg-shell /
      layer-shell / linux-dmabuf / screencopy 那一串）。缺了的后果分两条：
        - **相对位移没了**：Xwayland 的 XInput2 raw motion 建在 `zwp_relative_pointer_v1` 上，
          没有它，X11 游戏（Steam 上 Proton 那批基本都是 X11）转视角只能靠绝对坐标反推 ——
          鼠标顶到屏幕边、或者被别的窗口吃掉，视角就转不动 / 抖；
        - **锁 / 夹没了**：`XGrabPointer`（游戏锁鼠标、绘图软件把画笔夹在画布上）在 Xwayland
          那边就是 `lock_pointer` / `confine_pointer`，合成器不提供，请求发都发不出去。
      wlroots 只给协议对象（建 global、按表面查、发 activated/deactivated），**钉住 / 夹住光标
      是合成器的活**，所以新开了 `src/pointerconstraint.c`（策略和 sway 一致）：指针底下的表面
      == 约束那个表面才激活、焦点走了就解除；locked 钉在激活那一刻的位置、位移走
      `relative_pointer` 发给客户端；confined 用 `wlr_region_confine` 夹进区域（旧位置已经在
      区域外就先挪回区域中心）；`cursor_hint` 只在「锁生效那一刻」和「解除」时用 —— 锁着的时候
      不跟（Xwayland 会不停更新 hint 来表达「X 那边的指针在哪」，跟上去光标就满屏跑，实测过）。

      ② **判据 `./test-pointer-lock.sh`（新，27 项）**：headless 一块 1280x720 屏，两个新客户端
      工具 —— `tools/pointerlock.c`（Wayland：锁 / 夹 + 收相对位移）和 `tools/xgrab.c`
      （X11：`XGrabPointer`）。实测数据：
        - **锁定**：客户端收到 `locked`、IPC `input.pointer_constraint = {type: locked,
          window: 1, lifetime: persistent}`；`move-cursor 120 80 --relative` 之后客户端
          `reltotal 120.0 80.0`（相对位移**到客户端了**），而合成器光标还在 `206,156`、
          客户端那边的绝对坐标还在 `200,150` —— 三条一起才是「锁住了」；
        - **夹住**：区域 `100x100+50+50`，往区域外推 400,400 → 客户端的 motion 停在
          `100,100`（区域中心）、合成器光标停在 `106,106`（= 窗口原点 6,6 + 100,100）、
          相对位移照发（`reltotal 400.0 400.0`）；
        - **cursor_hint**：锁之前给 `--hint=40,60`，锁生效时光标落在 `46,66`（= 窗口原点 + hint）；
        - **X11**：`XGrabPointer` 之后 IPC 里**真的**出现了一个约束（`window` = xgrab 那个窗口，
          `locked`）—— 这就是「Xwayland bind 上了协议」的直接证据；把光标往窗口外推，合成器
          光标钉在 `323,360` 不动、而 X 客户端照样收到位移（`317,354 → 617,554`，只有相对
          指针那条路通了才可能出现）。
      反证（把 `vidow_pointer_constraint_init` 那一句摘掉重跑同一条）：27 项只剩 9 项通过 ——
      B/C/D 全红（没有 locked / confined、光标跟着动、hint 不生效），X11 那条「出现约束」也红。

      ③ 顺手修掉一个自己踩的坑：`wlr_pointer_constraints_v1` 在 display 销毁时会断言
      `events.new_constraint` 上没人挂着，我们挂了自己的监听却没人摘 —— 退出时直接 abort
      （`types/wlr_pointer_constraints_v1.c:339`）。修法：在协议对象的 `events.destroy` 里摘干净。

      ④ IPC 的 `input` 多了 `pointer_constraint`（`type` / `lifetime` / `window` / `cursor`）：
      `vidowmsg input` 一眼能看出现在谁锁着指针。

174. **窗口在非 0 那块屏上时录屏取错了输出**（光标 / 分辨率 / 抓帧触发一起错），
      选择器改成「预览在上、名字在下、双击确认」（用户报的三条：「1. 窗口打开录屏光标在非 0
      显示器采集不到，可能需要加显示器坐标。2. 第一次窗口录屏采集失败黑屏，第二次选择就可以。
      3. 选择器样式改一改，一个组合，上面显示预览，下面显示名称，选择是点击预览，双击直接
      确认。预览调小一点」）。

      ① **根因：`handle_stream_window` 拿 `vidow_view_geometry()` 当布局坐标去反查输出。**
      那个函数返回的是客户端**声明**的窗口几何（`xdg_toplevel->base->geometry`），它的 x/y 是
      **表面内部**的偏移（CSD 的阴影边距，一般 20,20），根本不是窗口在屏幕上的位置。拿它去
      `wlr_output_layout_output_at()` 反查，落点永远是**布局左上角那一块** —— 窗口在非 0 那块
      屏上时，`stream->output` 就取成了另一块，于是三件事一起错：
        - **光标元数据在错的那块屏上找光标**（`vidow_capture_cursor_info` 只扫
          `stream->output` 的 `wlr_output_cursor` 列表）→ 元数据里永远 `id=0`，消费者一个
          光标都不画。这就是用户报的「光标在非 0 显示器采集不到」；
        - **流尺寸乘的是错那块屏的 `scale`**（缩放屏上分辨率不对：实测窗口在 1 倍屏、错屏是
          2 倍时，流被按 2 倍算）；
        - **输出提交那条抓帧路盯的也是错那块屏**（`stream_handle_output_commit`）：窗口内容
          变了提交的是它自己那块屏，盯错屏 = 只剩 1Hz 心跳在喂帧。
      实测（headless 两块 1280 屏并排，窗口 `move-window-to-output` 搬到 x=1280 那块）：
      修之前日志是 `请求录窗口「pattern」（pattern.py，HEADLESS-2 scale 1.00 → 674x748）`
      （窗口明明在 HEADLESS-1）+ 元数据 `id=0 pos=0,0 bitmap=0`；修之后
      `HEADLESS-1 scale 1.00 → 674x748` + `id=1 pos=317,354 bitmap=1 24x24`。修法：用
      `vidow_view_screen_box()`（布局几何 + 动画偏移）的中心去反查；日志里把选中的输出和
      scale 也打出来，下次一眼看得见。

      ② **「第一次黑屏、第二次就好」这轮没能在 headless 上复现**：真 portal 链路 + 真
      PipeWire 消费者，同一个会话里连着开两路窗口流，两路的帧完全一样（674x748、非黑 89%、
      12% 是信箱黑边），光标元数据也一样。①里的后两条都在这条路上（错屏的 scale / 错屏的
      提交），先按它修掉了。为了让下次一眼能定位，加了两个 INFO：每路流**第一帧**报
      `第一帧 WxH（非黑 X%）`、流结束报 `这路流给了 N 帧` —— 「我们发的就是黑的」还是
      「消费者没画 / 帧不够」，看这两行就分得开。

      ③ **选择器改成「一张卡片 = 一个开关按钮，里面竖着摆：预览在上、名字在下」**
      （`tools/vidowportal.c` 的 `add_card`）：整张卡片就是那个 toggle，**点预览（或者名字）
      就是选中这一格**；**双击直接确认**（`on_card_press` 收 `GDK_2BUTTON_PRESS` → 先把它
      显式选上再走 `dialog_accept()` —— 双击那一下 GTK 也会当成一次点击，对话框当场关掉，
      后面那个 release 落不到东西上）；缩略图 240x150 → **176x110**（用户要预览小一点），
      对话框跟着窄一圈。实测：截图里卡片是预览在上、名字在下；在 pattern 那张卡的预览上
      `pointer-button click left 2`（注入器连点两下，间隔 40ms，正好是双击）→ 对话框关了、
      helper 日志 `录窗口：identifier=…` + `Start 成功，1 路流`。

      ④ 回归判据：`test-portal-own.sh` 从 1 块屏改成**2 块**（配置里把位置钉死
      `HEADLESS-1 x=0` / `HEADLESS-2 x=1280`，自动排的话谁在 0 是不确定的），47 项 → 49 项：
        - 6b2 那一轮**先把窗口 `move-window-to-output` 搬到非 0 那块屏**，再走「双击第一张
          卡片的预览」确认，然后断言**流选的输出 = 窗口所在那块屏**（`请求录窗口「pattern」
          （pattern.py，HEADLESS-2 scale 1.00 → 674x748）`）+ 光标元数据 `id=1` —— 这就是
          ①那条 bug 的回归判据（修之前：流选 HEADLESS-1、元数据 `id=0`）；
        - 6e（光标元数据）原来写死「光标放逻辑 100,100、流就是 HEADLESS-1」——2 块屏之后
          第一张卡片是哪块屏跟谁在 0 无关，改成按日志里的输出名现查（`请求录 <名字>`），
          光标放到**流那块屏**上，缩放的子检查也跟着用那块屏。
      顺带两处测试自身的坑：`wait_dialog` 拿到的几何可能是客户端「居中」**之前**的位置
      （实测 1286 → 1920），点之前得重读一遍；落点还要做一次「在不在对话框里」的检查，
      不然偏移写错了会静默点到别的窗口上。实测那一轮日志：`请求录窗口「pattern」
      （pattern.py，HEADLESS-2 scale 1.00 → 674x748）`、`第一帧 674x748（非黑 89%）`
      —— 新加的诊断也顺带在这条测试里验了。

173. **按窗口录屏的光标元数据修好了**（OBS 那个 `cursor_mode=4`）：有画面了，光标不进来
      （用户报的「有画面了，但光标没进来，无法控制光标的显示隐藏」）。

      ① **根因：窗口流的取景框还在用旧的坐标系。** OBS 的 SelectSources 里
      `cursor_mode=4`（metadata）= 光标不进画面，位置 / 热点 / 位图走 PipeWire 缓冲的
      `SPA_META_Cursor`，消费者（OBS）自己合成。我们这边算「光标在流里的哪儿」用的是
      `vidow_capture_view_box`（**窗口的布局几何 + 屏的缩放**，172 ⑥ 之前那条
      「把别的窗口关掉、按布局框渲染一屏」的取景）。172 ⑥ 之后窗口流改成了取景器
      （窗口**等比缩放进流**），两个坐标系就分家了：674×748 的流里，取景框被算成
      944×708、缩放 0.42，光标位置一算就落到流外面 —— 元数据里 `id=0`
      （spa 里 `id=0` 就是「没有可见光标」），OBS 一个光标都不画。
      实测（真 portal 链路 + `tools/cursorprobe` 当消费者，指针放在窗口中间）：
      修之前 `meta=1 buffers=1 id=0 pos=0,0 hotspot=0,0 bitmap=0`。
      **OBS 那边的行为也查清了**（`plugins/linux-pipewire/screencast-portal.c`）：
      `if (available_cursor_modes & PORTAL_CURSOR_MODE_METADATA) → 要 metadata` —— 只要
      合成器**报得出** metadata（我们报 7 = hidden|embedded|metadata），OBS 就**永远**
      选 metadata，跟它自己那个「显示光标」（`settings["ShowCursor"]`）无关；那个开关
      只管它**要不要把元数据里的光标画上去**（`cursor.visible`）。所以「无法控制光标的
      显示隐藏」正是这条：报得出 metadata 就得给对，不然勾上勾下都没光标。

      ② **修法：让取景器自己报框。** 新增 `vidow_win_grab_frame_box()`，和
      `vidow_win_grab_render` 里那次「等比摆进 out_w × out_h」是**同一套算法**：返回
      「流像素 0..out_w / 0..out_h 对应的那块布局矩形」+「布局逻辑 → 流像素」的缩放，
      于是 `(布局坐标 - 框原点) × 缩放 = 流里的像素位置`（那圈黑边也算在框里，所以
      光标落在信箱边上照样报得出来）。窗口停到屏外（藏起来的工作区 / 滚出视口）返回
      false —— 那种窗口不在屏上，没有「光标指着它」这回事。

      ③ **光标坐标的换算也修了**（`capture.c` 的 `cursor_dst_pos`）：原来把
      `wlr_output_cursor` 的 **输出像素** 坐标直接当布局坐标用（`oc->x + (obox.x -
      box.x) * scale`），只有在「抓帧缩放 == 屏的缩放」时才对 —— 整屏 / 区域抓帧正好
      是这样，所以一直没暴露。现在先把光标换回布局坐标、再乘抓帧的缩放。
      实测（test-portal-own 6e 那条「缩放 2 倍时逻辑 100,100 → 像素 200,200」照旧过）。

      ④ 顺带两件：**embedded 模式的窗口流以前根本不画光标**（代码里写着「窗口是等比
      缩放进流的，光标跟窗口不在一个坐标系里」—— 现在有坐标系了），一起补上；
      `stream_grab` 里 `win_grab == NULL`（录着的时候把窗口关了）补了守卫 —— 抓帧那条
      路（输出提交 / 1Hz 心跳）还会再来问一次，不挡的话下一次输出提交就在
      `mirror_extents` 里空指针崩（coredump 栈：`mirror_extents ← vidow_win_grab_render
      ← stream_grab ← stream_handle_output_commit`；反证：把守卫摘掉重跑，关窗口后
      vidow 当场挂，带上守卫「还活着」）。

      ⑤ 顺手清掉死代码：`vidow_capture_grab_view` / `vidow_capture_view_box` 和那套
      `solo_begin/solo_end`（渲染那一瞬间把别的窗口和 layer 全关掉）—— 172 ⑥ 之后
      窗口抓帧都走取景器，没人用了。

      ⑥ 回归判据（`test-portal-own.sh` 从 42 项到 47 项）：新增 6b2「按窗口录屏 +
      cursor_mode=4」（复用 6b 那个窗口，等上一路流收摊再开），断言 `id=1` + 有位图 +
      位置落在流的中间。实测：`meta=1 buffers=1 id=1 pos=317,354 hotspot=3,1 bitmap=1
      fmt=11 size=24x24 stride=96`（流 674×748，中心 337,374 —— 指针放在窗口格子中心，
      差那 20px 是客户端阴影边距的一半：表面 674×748 比窗口几何 634×708 大 40）。
      **坑**：6b2 一开始自己又开了一个 pattern 窗口，而 6b 那个刚被 kill（关窗有动画，
      还在窗口列表里）—— 选择器默认选中正在消失的那个，Start 一直不回，后面虚拟输出
      那轮也跟着 `code=2`；改成复用同一个窗口 + 等上一路流退出就稳了。

172. **按窗口录屏 / 预览换成 vidow 自己的截图源**：要一帧就渲染一帧，跟窗口在屏幕上
      摆在哪没关系（用户报的「窗口录制还是不对：状态 streaming → pause → streaming，
      error 是 none」+「还是有窗口没有预览，哪怕移动到视口内也不行，怀疑初始状态在
      视口外导致预览损坏」+「窗口还是串屏了」）。

      ① **根因是 wlroots 那份窗口截图源（`wlr_ext_image_capture_source_v1_create_with_
      scene_node`）本身**，不是我们哪一步写错了。它给窗口的子树在**同一张场景图**里、
      按节点的绝对坐标摆一个虚拟输出出来渲染（`types/ext_image_capture_source_v1/
      scene.c`），三个后果：
        - **按损伤出帧**：`source_handle_output_frame()` 里只有 `pending_commit_damage`
          非空才渲染（"We can only emit frames with damage"）。窗口不动、客户端也不
          重绘的时候（一个静止的终端），客户端 `capture` 要的那一帧**永远不来**。
          headless 实测：静止窗口（tools/bluewin.py，纯色画完就歇）滚到视口外，抓 3 秒
          **只出 1 帧**（`source_start` 那一帧）；同一个位置换成一直在动的
          tools/heartbeat.py，同样 3 秒 89 帧。选择器那边于是卡在「等第一帧」——
          `win_shot` 的 `busy` 一直是 true、之后不再重试，所以**把窗口挪回视口也还是
          没预览**；OBS 那边就是一会儿 streaming 一会儿 paused，error 是 none（协议里
          没这个错，就是没帧）。
        - **抓的是「这块坐标里的一切」**：压在窗口上的别的东西、隔壁屏伸过来的一条，
          都会进画面（实测：静止窗口的抓帧校验和每帧都在变，因为它那块框里套着隔壁
          那列一直在动的窗口）。
        - **虚拟输出跟着节点走**：节点一动（我们让滚出视口的窗口停到屏外）尺寸 / 位置
          就变，客户端得重建会话，正好撞上它在等第一帧。

      ② **新实现 `src/wincapture.c`（约 420 行）照 Hyprland 那条路**
      （`src/managers/screenshare/ScreenshareFrame.cpp` 的 `renderWindow(…, standalone)`：
      把窗口**单独渲染进自己的缓冲**，跟它在屏幕上摆在哪、被谁压着都没关系）：
        - 一棵**私有场景**（`wlr_scene_create()`），里面只有这个窗口的一份**镜像**
          （`wlr_scene_subsurface_tree_create`，同一张表面挂两棵树是允许的：场景表面的
          addon 挂在节点上、子表面那棵树按 (表面, 父树) 认，互不干扰）；
        - 一个假输出 + 假 scene_output（同 `capture.c` 那套：不提交、只用来给
          `wlr_scene_output_build_state` 落脚），原点钉在 (0,0)，框 = 镜像的外接框
          （镜像整体挪到外接框左上，子表面可能带负偏移）；
        - **客户端要一帧就渲染一帧**（`source_request_frame` 里同步渲染 + 拷进客户端
          缓冲），不看损伤；渲染前把整块标脏（损伤环里没脏东西时渲染那一趟一个像素都
          不画，拿回来的可能是两张交换链缓冲里更旧的那张）；渲染完给被抓的窗口发帧回调
          （它才会接着画下一帧）；
        - 渲染出来的是**客户端表面本身**：没有我们的装饰 / 模糊 / 圆角 / 阴影 —— 和
          Hyprland 的窗口导出一样。
      约束（尺寸 / 格式 / 设备）从假输出的交换链来，只在**尺寸变了**的时候重发
      （`constraints_update` 是无条件发的，每帧发一遍客户端会不停重建会话）；渲染失败或
      拷不进去都会回一个 `failed`（客户端于是会重试，而不是永远等下去）。

      ③ **顺带把「有人录屏就不裁」那条取舍做掉了**（171 ⑤）：抓帧既然渲染的是窗口自己
      的镜像，屏幕上那一份就可以照裁 —— `view_clip_box()` 里不再因为
      `vidow_view_capture_held()` 跳过，跨屏那半块在录屏时也不画到隔壁屏上（
      test-scrollcull 第 5b / 6e 节：隔壁屏 0 个窗口色像素，修之前是「已知取舍」）。

      ④ **两个坑**（都在 headless 上复现过、都修了）：
        - **假输出必须给 `wlr_output_init` 一个 `commit`**（它断言 `impl->commit`
          非空），我们那份永远不真提交，给个返回 true 的壳（同 `capture.c`）；
        - **源的销毁挂在表面上，不是挂在镜像节点上**：镜像那棵子树是 wlroots 建的，
          表面一销毁它就在自己的 destroy 信号里销毁，那时候再去销毁整棵私有场景会
          **递归**回到正在销毁的节点上（`wlr_scene_node_destroy` 断言节点的 destroy
          监听表非空，当场 abort —— 关掉一个正在被录的窗口就崩）。表面这个信号上
          wlroots 那两个监听都注册在我们前面，轮到我们时镜像早摘干净了。另外那张
          「这一帧的缓冲」的解锁路径有三条（下一次抓帧 / 会话停 / 源销毁），源销毁
          会连锁触发会话的 stop —— 解锁后必须置空，不然解两次（`wlr_buffer_unlock`
          断言 `n_locks > 0`，同一个场景下也 abort 过）。

      ⑤ 回归判据（`test-scrollcull.sh` 从 29 项到 36 项）：新增「静止窗口滚到视口外
      照样出帧」（3 秒 ≥ 30 帧，修之前 1 帧）、「挂着截图源的半块越界窗口不串屏」、
      6e 那条从「只断言录屏没坏」改成**断言隔壁屏 0 个窗口色像素**。

      ⑥ **OBS 那条路（portal 的 `stream_window`）单独还有两个 bug**，同一个「取景器」
      一起修掉了（用户报的「点共享之后没有流 / 就是黑的」「状态 streaming → pause →
      streaming，error 是 none」）：
        - **流的尺寸和 portal 报的尺寸对不上**：`handle_stream_window` 按窗口的
          **布局几何**算（634×708），而选择器给 portal 报的是预览缩略图的尺寸
          （= 窗口**表面**那块，含客户端自己的阴影边距，674×748）。消费者（OBS /
          浏览器）按报出来的尺寸协商时，我们这边只有另一个尺寸 —— 协商整个失败。
          实测：让 gst 按 portal 报的 674×748 协商，直接 `flow-return=-4`、**一帧都
          拿不到**（按我们报的尺寸协商则正常）。现在流的尺寸取窗口表面那块（和预览
          一致），并且**跟着消费者协商出来的尺寸走**（`param_changed` 里改
          `stream->out_w/h`，按它渲染）—— 消费者要什么尺寸都给得出来。
        - **按布局坐标抓**：那条路原来是「把别的窗口和 layer 全关掉、按窗口的布局框
          渲染一屏」（`vidow_capture_grab_view`）。窗口滚出视口 / 停到屏外之后，那个
          坐标里什么都没有 —— 录到的是**一片黑**。现在窗口流也用同一个取景器
          （`vidow_win_grab_*`，私有场景 + 镜像），跟窗口在屏幕上摆在哪没关系。
        实测（真 portal 链路 + 真 PipeWire 消费者）：修之前帧是 634×708、强制按
        674×748 协商 0 帧；修之后 674×748、强制协商 3 帧、画面 84% 是窗口的蓝。

171. **「藏起来的窗口」不再关场景节点，改成停到所有输出框外面**：录屏 / 预览终于两头都对
      （用户报的「开了 OBS 然后 portal 选择开启的时候，视口外的窗口就完全没藏住」+
      「选择器里有些窗口没有预览」+「点共享之后没有流，就是黑的」）。

      ① **以前藏窗口 = `wlr_scene_node_set_enabled(scene_tree, false)`**，而按窗口录屏 /
      选择器里的预览走的是 wlroots 的窗口截图源
      （`wlr_ext_image_capture_source_v1_create_with_scene_node(view->scene_tree)`）：它
      自己开一个**虚拟输出**，位置取节点自己的框，然后把场景画进那块框里 —— 节点一关，
      录到 / 预览到的就是**一片黑**（headless 实测：窗口所在工作区切走之后录它，只拿到
      1 帧、校验和 `00000000`）。更糟的是当时为了「别把要录的窗口藏没了」，剔除那条路
      对**有人拿着的窗口整个跳过**：于是挂着截图源的窗口滚出视口之后，整个身子又画到隔壁
      屏上了（实测隔壁屏 26383 个窗口色像素 —— 用户看到的就是这个）。

      ② 现在「不画」= 把节点**停到所有输出框外面的停车位**（`view.c` 的 `view_park_pos`：
      所有输出框的左上角再往外 64K，按窗口编号横着一格 16384 摆开 —— 挨着摆的话，截图的
      虚拟输出框会互相套进去，录一个连旁边那个一起录）。哪块屏的框里都没有它，屏幕上照样
      看不见；截图源的虚拟输出是**跟着节点位置走**的，节点停哪儿它跟到哪儿 —— 录到的是
      完整窗口。判据（`test-scrollcull.sh` 第 6 节，三种顺序都测）：
        - 先挂截图源再把窗口滚出视口：隔壁屏 **0** 个窗口色像素（修之前 26383），
          同时录到 37 帧 / 37 种校验和；
        - 窗口已经在视口外才挂截图源：20 帧 / 20 种校验和（修之前这条也能过，因为
          「有人拿着」那条路会把它重新打开 —— 但那样就串屏，见上一条）；
        - **窗口所在工作区被藏起来**（切到别的工作区）：20 帧 / 20 种校验和
          （修之前 1 帧、全黑）—— 这条就是「选择器里有些窗口没有预览」。

      ③ 两个坑：**判据必须每帧现算**（`vidow_view_should_park`，`view_anim_place` 里问
      它）—— 只在 arrange 里存一个标志的话，滚动 / 开窗的动画帧会自己摆位置（不经过
      arrange），窗口从屏边界上扫过去的时候标志就过期了：摆回屏里之后还停在停车位上，
      那个窗口**再也不出现**（test-scrollcull 第 1 节抓到过：开窗动画里 cull-3 有一瞬
      正好停在 x=1280 上被停了一次，之后就再没被放回来）。翻转 / 日志 / 损伤收在一个
      入口（`vidow_view_set_parked`），状态行 `workspace: <标题> 藏起（停到屏外）/ 显示`
      每次翻转都写 —— 测试和人都按它认当下状态。**弹出物照旧关节点**：它不在父窗口那棵
      树里（X11 的在单独一层上）、位置按父窗口的**布局坐标**算，父窗口停到屏外了它还会
      留在原地，所以父窗口停着的时候它也不画（判据里带上 `!parent->scene_parked`）。
      另外 `vidow_view_apply_clip` 对停着的窗口不裁：裁的是节点的显示范围，截图源渲染的是
      同一棵树，一裁录到的窗口就少一块。IPC 的 `visible` 也跟着改成「节点开着 **且** 没
      停到屏外」（`vidowmsg windows`）—— 光看那个 `enabled` 现在是恒真的。

      ④ **判据按「这一帧画在哪儿」算，不是按布局目标位置**：滚动 / 开窗动画里窗口是从
      屏边界上扫过去的，按目标位置判的话，滑回来的那一路（目标已经落在屏里、画面还在
      屏外）会被当成「在屏里」，越界的那一块就画到隔壁屏上了 —— 用户报的「只有 kitty 时
      视口外的窗口回来的瞬间会显示在另一个显示器上」（test-scrollcull 第 6d 节：动画调到
      900ms，中途连抓四张隔壁屏，修之前是 13860 → 4341 → 501 → 0 个窗口色像素，修之后
      四张全是 0）。同理，`fx.c` 里那块离屏缓冲的裁剪多了一档「这一帧整个身子都在屏外」
      —— 之前那种情况（节点框和屏框**没有交集**）走的是「不裁」，整块窗口照画，正好画在
      隔壁屏上；现在把落点甩到所有输出框外面去。

      ⑤ **已知的取舍**：窗口**正被人录屏 / 预览**（有截图源挂着）的时候不裁（见上）——
      它跨在屏边界上（滚了一半停住）的那一段，录屏拿到的是完整窗口，代价是隔壁屏上会
      看到跨过去的那一块（第 6e 节把隔壁屏的像素数打出来当参考，只断言「录屏没被搞坏」）。
      要两头都干净得把「显示的那一份」和「录屏的那一份」拆成两棵场景树（截图源指一个
      不停在输出框里的镜像节点），那是另一件事 —— 现在没有做。
      → **172 把这条做掉了**：截图源改成渲染窗口自己的镜像（私有场景 + 假输出），
      屏幕上那一份照裁，6e 节改成断言隔壁屏 0 个像素。

170. **半块越界的窗口不再画到隔壁屏上；录屏 portal 的选择器改成 KDE 那种卡片**。

     ①「滚动后超出视口的窗口会跑到另一个显示器里面」（用户报的）。场景图是一张**全局
     坐标**的图，每块屏的场景输出画的是「落在自己那块坐标里的所有节点」—— 之前只有
     **整块**滚出自己那块屏的列才被摘掉（`vidow_view_in_output_box`），还看得见一部分、
     另一部分跨过屏边界的那种列，越出去的那半块照样被隔壁那块屏画出来（headless 实测：
     两块 1280 宽的屏并排，第二列 646..1491，隔壁屏的截图里 x=0..205 全是它的右半块
     —— 绿上白下，正是那个窗口）。现在按「窗口**连装饰内边距**整块都在自己那块屏里吗」
     判一次，不在就把**显示**裁到自己那块屏里（`src/view.c` 的 `vidow_view_apply_clip`）：
     装饰那块离屏缓冲（fx 节点）改成只采交集那一块、铺到交集那点地方
     （`wlr_scene_buffer_set_source_box` + `set_dest_size`），几何、客户端、录屏抓帧
     一个都不动。回归判据 `test-scrollcull.sh` 第 5 节：隔壁屏截图里窗口色像素 **0** 个，
     而自己那块屏上那半块还在（裁的是越界那一块，不是整个窗口）。

     三个坑记在这：①裁剪必须在**重烘那一帧**算（`vidow_view_anim_bake` 开头）—— 烘完
     再算就晚一帧，而静止下来之后没有下一帧来纠正，屏幕上就一直挂着那一块（查了半天：
     `clip_on` 明明是 true，fx 那边却看不见）；②客户端表面那一份要调
     `wlr_scene_subsurface_tree_set_clip()`，它**只认 subsurface 树**，X11 窗口和弹出物
     那两棵树上找不到就直接 assert（`found` failed，vidow 当场 abort，test-dpi.sh 抓到
     两次）。所以表面那一份只给 Wayland 的 xdg toplevel；X11 / 弹出物靠 fx 那块离屏
     缓冲（默认配置装饰开着，窗口画的就是它）；③裁剪会改场景节点的显示尺寸，而
     **窗口截图源**（portal 的按窗口录屏走 `wlr_ext_image_capture_source_v1_create_with_
     scene_node(view->scene_tree)`）盯着这棵树 —— 节点一变它就重发约束，客户端那边正好
     在等第一帧，撞上 wlroots 的「session already has a frame object」直接断线（录到的
     帧数 0）。所以判据收成「窗口**本体**跨过屏边界才裁」：贴着屏边时装饰阴影内边距探出
     去的那十几个像素不裁（那点阴影越界先留着），常见情形下节点一动不动，录屏不受影响。
     顺带把 `tools/capture.c` 的收帧逻辑补严：约束重发时手上还有一帧在飞就先不建新帧
     （协议只允许一帧在飞），缓冲尺寸变了就重开一块。

     ②录屏 portal 的选择器原来是一行「缩略图 + 右边一个大按钮」的列表，和 KDE 的选择器
     长得不一样（用户报的「变成左边预览右边按钮了」）。改成 KDE 那种一格一格的卡片：
     预览占主要面积、名字（就是选择开关）在预览上面、下面一行小字写分辨率 / app_id，
     一行三张（`tools/vidowportal.c` 的 `add_card` / `card_place`，缩略图 320×200 缩到
     240×150；KDE 那边是 `Kirigami.AbstractCard`，header 放名字 + 单选、content 放预览）。

169. **`./run.sh`：一键起 vidow（tty / 嵌套），顺手验 portal，退出时收干净**。

     以前起法散在三处：真机手敲 `./build/vidow`、嵌套手敲 `WLR_BACKENDS=wayland …`、调试
     用 `./dbg.sh`。这个脚本把它们合成一条路，并且补上「起来之后」和「退出之后」两件事
     —— 后者才是重点：

     - **起来之后**：`--portal` / `--portal-check` 直接跑 `./portaltest.sh`（体检 + 一个真
       的文件选择器请求），并先把「现在 portal 指向谁」打一行出来。
     - **退出之后**：① 收 vidow（SIGTERM → 5 秒 → SIGKILL，连进程组）；② **扫残留客户端**
       —— 环境里 `WAYLAND_DISPLAY` 正好是它那个 socket 的进程。这类孤儿是真事故：vidow 死
       了它们本该跟着走，可 `wev` 不会，**两个进程各 100% CPU 空转了 25 分钟**，同一台机器
       上另一个会话里的游戏掉帧、加载变慢就是它（`test-input-inject.sh` 的收尾按 cmdline
       匹配 `*/wev*`，而进程实际是 `stdbuf -o0 -e0 wev` —— 匹配不上，漏杀。脚本按环境变量
       收，SIGTERM 不走才 SIGKILL）；③ 看有没有新 coredump（`coredumpctl --since "@启动
       时刻"`），有就把栈的前几帧打出来 —— vidow 退出时段错误是老 bug（一次 2MB，机器上
       已经堆了一百多个），不看着它就会一直悄悄崩。
     - **`--nested` 自动补 `WLR_RENDER_DRM_DEVICE`**：少了它 wlroots 0.20.2 起不了 GLES2
       渲染器（见上面「运行」那节）。
     - **`--no-portal-env`**：临时 `XDG_CONFIG_HOME`（vidow 没有 `--config` 参数，只能换
       目录）里放一份 `portal = { conf_sync = false, desktop = "" }`，用于「只想跑 vidow、
       不碰 portal 环境」的场合；代价是那份临时配置里没有你的 `config.lua`。

168. **跨屏拖窗口「先模糊后清晰」：离屏缓冲按「画在哪块屏上」的倍率烘，不是按归属**。

     用户报的：窗口从 1080p（1 倍）拖到 4K（2 倍）上，一路上糊着，松手才清楚。

     - **根因**：装饰 / 着色器的离屏缓冲按 `view->output`（窗口**属于**哪块屏的工作区）算
       倍率，而布局归属要到**松手**才换 —— 拖到 4K 屏上的那一段，缓冲还是 1 倍（实测
       2076x1236），整块窗口被放大到 2 倍屏上。客户端那边其实**早就**切了（它按
       `wl_surface.enter` / `wl_output.scale` 重画成 2 倍），我们却还按 1 倍烘，两头对不上
       —— 屏幕上是放大出来的糊。
     - **改法**（`src/view.c` 的 `view_scale()`）：按「窗口**看得见**的那些屏里**最大**的
       倍率」烘。取最大而不是「面积最大的那块」：客户端一碰到 2 倍屏就按 2 倍重画了，我们
       按 1 倍烘的话它交上来的 2 倍缓冲还得先缩下去、屏幕上再放大 —— 还是糊。宁可多烘
       （画到 1 倍屏上时是缩下去，仍然清晰），不能少烘（画到 4K 屏上是放大，糊）。
     - **实测**（headless：1920x1080@1x + 3840x2160@2x，两块屏逻辑尺寸都是 1920x1080，
       正是「1080p + 4K@2x」那套）：修之前拖拽中 `fx: target 2076x1236`（1 倍），松手才变
       `3864x2184`；修之后拖到对面屏上就是 `3864x2184`，而且比客户端自己那块 2 倍缓冲还
       早到（日志 6.616s vs 客户端 6.788s）。
     - **新测试 `./test-mixdpi.sh`（7 项）**：起点确认 1 倍屏上是 1 倍缓冲；拖到 2 倍屏上、
       **还没松手**时断言「归属仍是 1 倍屏」（要测的就是这一段）且缓冲已经按 2 倍烘；松手
       后归属换成 2 倍屏、缓冲仍是 2 倍；再拖回 1 倍屏时缓冲回到 1 倍（不是一直按 2 倍烘）。
       把改动 stash 掉重跑：拖拽那一条红（`还是 2076 宽（1 倍）`）✓。
     - **`test-blur2out.sh` 跟着改了一处**：它原来把窗口从 2 倍屏一路挪到隔壁 1 倍屏上，
       那会换掉烘窗口的倍率、重建一次缓冲 —— 那一下现在是**该发生的**，不在它的判据里。
       挪动范围收到窗口自己那块屏里（判据「移动期间 0 次分配」不变），跨屏换倍率归
       `test-mixdpi.sh` 管。
     - **顺带把「私有模糊底衬归哪块屏」对齐到同一个判据**（`fx.c` 的 `view_blur_owner()`）：
       它原来按 `view->output`（归属）筛，于是拖到对面那块屏上时，底衬是按旧屏的倍率抓
       的、装饰缓冲是按新屏的倍率烘的 —— 模糊贴图的采样坐标（按底衬那块屏的设备像素记）
       就落错地方。现在两处都走 `vidow_view_display_output()`：一个窗口「现在算在哪块屏
       上」只有一个答案。
     - 全套 63 个测试绿。

167. **层级：浮动窗口永远在平铺窗口之上，浮窗之间靠点击换层级**。

     以前没有「谁该在谁上面」这条规矩，层级等于「谁最后被动过」—— 两个方向都会坏：
     ① 新窗口是 `wlr_scene_tree_create()` 建的 = 挂到场景树最后 = 永远在最上面，所以
     **新开一个平铺窗口会盖住已有的浮窗**；② 聚焦那边是无差别 `raise_to_top`，点一下
     （或者用 IPC / 键盘焦点切到）平铺窗口，**浮窗就被压下去**。

     - **一条不变量**（`vidow_restack()`，`src/view.c`）：`平铺 < 浮动 < 全屏`。场景树里
       同一层的先后就是层级（命中测试也是从最上面那个往下找），所以「重排」= 按现在的
       顺序把每一组抬一遍：平铺的那个 → 浮窗（按它们现在的顺序）→ 全屏。`top` 参数 =
       「这一个刚被抬起来」，它在自己那一组里最后抬，于是落在组的最上面 —— **浮窗之间
       就是靠这一条换层级的**（点哪个哪个上来）。
     - **只在真有人站错队时才动手**：站对了就一个节点都不碰（`raise` 会让那块区域重画，
       而点击那条路走得很频繁）。层级真动了才补一次 `vidow_backdrop_damage()` —— 底下
       那些窗口的私有底衬里画着它，谁在谁后面变了它们的底衬就过期了。
     - **四个接点**：① `vidow_focus_view()` 里那句无差别 `raise_to_top` 换成它；② 每次
       `vidow_arrange()` 收口一次（新窗口、浮 ⇄ 平铺切换、全屏进出都在这一趟里摆正）；
       ③ 鼠标点击（`src/input.c`）—— **焦点没变也要抬**，否则「已经拿着键盘的浮窗被后开
       的浮窗压着」时点它等于没反应；④ 平铺拖拽（`src/drag.c`）：飘起来的平铺窗口压住
       别的平铺窗口，但浮窗照旧在它上面。
     - **全屏单列一组压在最后**：老代码在 arrange 里抬全屏，这里接着保证它一直是最上面
       那个（全屏是「占满整屏」，不该被浮窗盖住）。
     - **踩到的一个坑（`test-blurcache.sh` 抓出来的）**：平铺 → 浮动那个瞬间，窗口的场景
       节点还停在「它平铺时」的位置 —— 平铺的节点本来就在浮窗**下面**，所以它翻上来之后
       是浮窗组里**最低**的那个；它平铺的那一小会儿 arrange 里的收口还会把别的浮窗抬到它
       上面。结果就是「切成浮动了，却还被另一个浮窗压着」（`test-blurcache` 里表现为
       拖错了窗口：`grab move: wback` 而不是 `wtop`）。修法是在 `vidow_float_set_mode()`
       里补一次 `vidow_restack(server, view)`：刚变成浮动的那个正是用户刚动过的，它该在
       浮窗组最上面。arrange 里的收口只保证「浮动在平铺之上」，组内的先后得有这一下。
     - **新测试 `./test-floatstack.sh`（12 项，全 headless + grim 逐像素）**：取样点选在
       浮窗中心（= 浮窗 A / B 的重叠处），「那儿最上面的是谁」就是谁的颜色（按一小块里
       最多的颜色判，`sheet.py` 身上那层 1px 白网格不会骗过它）。基线先确认那儿是平铺
       窗口的纯红象限，然后：浮窗压上去 → 蓝；IPC 把焦点给平铺窗口 → 还是蓝；鼠标点平铺
       窗口 → 还是蓝；平铺窗口进全屏 → 不是蓝，退出来 → 又蓝；新开一个平铺窗口 → 还是
       蓝；再开一个浮窗 B 压上去 → 红；点 A 露出来的那块 → 蓝；点 B 露出来的那块 → 红；
       焦点在平铺窗口上时点 A → 蓝。**把改动 stash 掉重跑，6 项红**（聚焦 / 点击 / 退出
       全屏 / 新开的平铺窗口 / 两次点浮窗）✓。全套 62 个测试绿。

165. **录屏 portal 补齐来源（窗口 / 区域 / 虚拟输出）+ 截图接口，顺手修了三个真 bug**。

     上一轮 164 里「没做的」那几项这轮全补上了；补的过程踩出来的 bug 比新功能本身更值钱。

     - **合成器侧四种抓帧来源**（`src/capture.c` / `src/screencast.c`）：
       `vidow_capture_grab_box()` 渲染场景里的任意矩形（`stream_region` 用它，输出尺寸 =
       round(矩形 × 缩放)）；`vidow_capture_grab_view()` 只渲染一个窗口 —— 渲染那一瞬间把别的
       窗口和各个 layer `wlr_scene_node_set_enabled(false)`（同步渲染，中间不派发输入事件，
       所以没人观察得到），再等比缩放填进流，剩下的边是黑的；虚拟输出是现建 headless 后端 +
       一块**真输出**（进布局、能摆窗口、能当窗口的主输出），这一路流结束时拔掉。
       `stream_window` 的 `window_uuid` 就是 `ext-foreign-toplevel-list-v1` 的 identifier，
       合成器拿它反查 view；窗口关掉时给客户端发 `closed`。
     - **选择器按来源分组**：屏幕（screencopy 缩略图）/ 窗口（窗口抓帧缩略图）/ 虚拟屏幕三段，
       `AvailableSourceTypes` 报 1|2|4，单选/多选看 `multiple`，**第一行默认选中**。
     - **截图接口**：`Screenshot` / `PickColor` **不走 PipeWire** —— 后端自己当客户端抓一帧、
       写 PNG（libpng）、回文件 URI（`$XDG_CACHE_HOME/vidow-portal/`）；`interactive = true`
       或者取色会把那一帧冻住铺满屏，让用户框一块 / 点一下（按控件尺寸换算回像素再裁）。
     - **修掉的真 bug**：
       ① **libwayland 的 NULL listener**：`zwlr_screencopy_frame_v1` 有 7 个事件，helper 只填了
          5 个，GLES2 会话上合成器一发 `linux_dmabuf` 就 `abort`（libwayland 的原话是
          "listener function for opcode %u of %s is NULL"）—— 之前所有测试都在 pixman 上跑
          （没有 dmabuf、不发那个事件），真机上开一次对话框就崩。
       ② **一次性抓帧的请求没 flush**：`copy` 是在「收到事件」的回调里发的，而 libwayland 只在
          下一次 dispatch 时 flush —— 没有下一轮事件就永远等不到 `ready`（缩略图那条路靠 500ms
          的 tick 顺带 flush 才没露）。
       ③ **缩略图每 500ms 新建一个窗口抓帧会话**：合成器那边每个会话配一块 scene_output，而一个
          场景最多 64 块 → 半分钟撞上限，`wlr_scene_output_create: Assertion scene_output->index
          < 64 failed`，vidow 当场崩（用户的会话跟着没了）。现在一个窗口一个会话、反复要帧。
       还有几个小的：shm 格式要按 DRM fourcc 认（GLES2 上报的是 `XBGR8888`，只认 0/1 的话
       缩略图和截图红蓝颠倒）、impl `Screenshot` 的签名没有 `session_handle`（多写一个，前端
       转发过来就是「类型不匹配」直接 code=2）、`dialog_accept` 不幂等（连点两下「共享」会建
       两路流，虚拟输出那种还会多留一块屏）。
     - `./test-portal-own.sh` 从 15 项扩到 32 项：整屏 / 窗口 / 虚拟输出三种来源各自出帧、
       虚拟输出用完拔掉、`Screenshot` 回真 PNG（1280x720）、`PickColor` 回颜色。确认选择器改成
       **回车**（点坐标会随内容飘：对话框的窗口几何和按钮实际位置差着几十像素）。
     - **仍然没做**：光标模式 `metadata`（`AvailableCursorModes` 只报 1|2）、
       `zkde_screencast_unstable_v1` 的 bind 还没收紧到「只给自己那个后端」。

164. **录屏 portal 改成 vidow 自己做 —— 效果和 KDE 一样，但谁都不骗**。

     上一轮把 KDE 的录屏 portal 跑通，代价是 `portal_desktop = "KDE"`：所有应用都以为自己在
     Plasma 会话里（GTK / Qt 主题、Chromium 的对话框选择都跟着变）。用户问「能不能只用 KDE
     录屏 portal 而不影响别的」，评估了三条路（全局改环境 / 只给 KDE 后端一个 systemd
     drop-in / 自己出一个后端），选了最后一条：**vidow 自己当 portal 后端**
     （`tools/vidowportal.c` → `build/vidow-portal`）。

     **链路**（每一环都能单独验证）：

     - vidow 启动时写 `~/.local/share/xdg-desktop-portal/portals/vidow.portal`
       （`DBusName=org.freedesktop.impl.portal.desktop.vidow` + 支持哪些接口），把
       `<会话名>-portals.conf` 里的 ScreenCast / Screenshot 指向 `vidow`，再把
       `vidow-portal` 起起来（和 vidow 二进制同目录，`/proc/self/exe` 反推）占住那个总线名。
     - 前端（xdg-desktop-portal）只做转发。**PipeWire 的 fd 是它自己连的** ——
       `OpenPipeWireRemote` 在前端实现里（`nm -D` 能看到 `pw_context_connect` /
       `pw_core_steal_fd`），所以后端只要「报 node id」。
     - `vidow-portal` 实现 `org.freedesktop.impl.portal.ScreenCast` 三个方法
       （CreateSession / SelectSources / Start，接口版本报 2：有 `cursor_mode`，不碰
       restore_data / persist_mode）+ Request / Session 两个小对象（Close / Closed）。
       Start 时弹 GTK3 选择器：每块屏一行**实时缩略图**（`zwlr_screencopy_manager_v1` 抓帧，
       500ms 一轮）+ 输出名（`wl_output.name`，vidow 报的是 `HEADLESS-1` 这种）+ 单选/多选
       （按 `multiple`），第一块屏默认选中；挂到发起请求的窗口上（xdg-foreign，GDK 自己 import）。
     - 点「共享」→ `stream_output(stream, wl_output, pointer_mode)` → 等 `created(node)` →
       把 `streams = [(node, {position, size, source_type})]` 回给前端。**合成器那边一行没改**
       （`src/screencast.c` 的 zkde_screencast 本来就是「客户端要流、合成器建 node」）。

     **顺手修掉的真 bug**：

     - **`zwlr_screencopy_manager_v1` 的格式比较是错的，而且错法挑客户端**：wl_shm 的枚举值
       只有 `ARGB8888 = 0` / `XRGB8888 = 1` 是协议特例，**其余格式的值就是 DRM fourcc**
       （`WL_SHM_FORMAT_XBGR8888 == DRM_FORMAT_XBGR8888 == 0x34324258`）；而
       `wlr_buffer_begin_data_ptr_access()` 给的 format 一律是 DRM 的。老代码直接拿两个数比：
       渲染器读回格式是 XBGR8888 时（GLES2，实测 wf-recorder 的 `buffer` 事件报的就是
       `0x34324258`）值恰好相同 → 蒙对；是 XRGB8888 时（pixman）1 vs 0x34325258 → 永远不等，
       客户端被 `invalid buffer format` 拒掉。我们的选择器缩略图正好跑在 pixman 上，一上来
       就被拒。修法是两边都按「特例那两个换一下、其余同值」处理（`shm_format_to_drm()` /
       `drm_format_to_shm()`），并把两个值都写进错误日志。
       **第一版修反了**：只把「wl_shm 枚举 → DRM」这半边换过去、把另外半边当非法，于是 GLES2
       那侧（走 shm 的 wf-recorder）反而红了 —— `test-capture-hide.sh` 报「录像文件 ffprobe
       读不出时长」（帧格式中途变化，ffmpeg 的 filtergraph 直接报错退出，MKV 没写完）。
       顺手把「`shm_format == 0` 当没有」那个守卫也改了：ARGB8888 在 wl_shm 里就是 0，那个
       守卫会把合法的 ARGB8888 读回当成失败（现在看 `read_format == DRM_FORMAT_INVALID`）。
     - **前端要不要重启，判据以前是错的**：前端的 `portals.conf` 是**进程起来那一刻**读的、
       读一次缓存住，而它的环境也是被 systemd 激活那一刻定死的。以前只在「配置内容变了」时
       重启它 —— 从宿主合成器切过来时配置往往没变（文件是上一轮写的），于是前端还按宿主那份
       `<host>-portals.conf` 选后端，录屏会被指到 hyprland 上去。现在改成：配置变了，**或者
       正在跑的那个前端的 `XDG_CURRENT_DESKTOP` / `WAYLAND_DISPLAY` 不是我们的**（查
       `/proc/<MainPID>/environ`）就重启；前端没在跑就不管（下次被激活时环境就是新的）。
     - **自己踩的三个坑**（都记在代码注释里了）：① 对话框刚弹出来就被自己回成失败 ——
       `wl_ready` 每个 wayland 事件都会调 `start_maybe_finish()`，那时 streams 是空的，
       「全都 settled」真空成立，直接回了 code=2；现在有 `starting` 标志。② 把 streams 数组
       当结果字典回 —— GDBus 当场 `g_variant_new: expected GVariant of type 'a{sv}' but
       received value has type 'a(ua{sv})'` 然后 abort，前端只看到「对端没回就断线」。
       ③ 没选任何输出时「共享」是灰的（KDE 的选择器进来就选中第一块屏），点它等于没点。

     **新增 `./test-portal-own.sh`（15 项断言全绿，不依赖 xdg-desktop-portal-kde）**：
     `vidow.portal` 内容 / `ScreenCast=vidow` / 没写任何 systemd drop-in → `vidow-portal`
     占住总线名 → 前端把 ScreenCast 指给它（`gdbus introspect` 两边都查）→ 选择器真出现在
     vidow 的窗口列表里（`vidowmsg windows` 查到标题「共享屏幕」）→ 点「共享」→ `Start`
     返回 `node=114`、`size=(1280, 720)` → gst 消费者 6 帧 6 种画面、静止画面靠 1Hz 心跳也有帧
     → 全程没出现过 `XDG_CURRENT_DESKTOP=KDE`。`test-portalenv.sh` 也跟着扩了：
     auto 优先自己人 / 没有自己人时退回探测 / `portal_capture` 写死与 `none` / 前端环境不对
     就重启（用假 systemctl 回答 MainPID）。`test-screencast.sh`（KDE 那条路）照旧全绿。

     **没做的**：按窗口 / 按区域 / 虚拟输出录屏（回 `failed`）、光标模式 `metadata`、
     Screenshot 接口（这两行现在写的是 `vidow`，Screenshot 调用会失败 —— 下一步补）、
     以及把 `zkde_screencast_unstable_v1` 的 bind 收紧到「只给自己那个后端」（现在和 KWin
     一样对所有客户端开放）。

163. **KDE 的录屏 portal 在 vidow 上跑通了：实现 KWin 私有协议 `zkde_screencast_unstable_v1`**。

     用户要「KDE 的录屏 portal」。上一轮我判断做不到（KDE 后端的 ScreenCast 建在 KWin 私有
     协议上），这轮把两条路都走通了 —— 一条在 vidow 里，一条在配置里，缺一不可。

     **① vidow 现在提供 `zkde_screencast_unstable_v1`（v6）**，`src/screencast.c` 新文件，
     协议 XML 抄自 `plasma-wayland-protocols`（本机 `/usr/share/plasma-wayland-protocols/`，
     `protocols/zkde-screencast-unstable-v1.xml`）。协议的关键点：**PipeWire 流是合成器建的**，
     `created(node)` 里那个 node id 指的是合成器进程里的节点，客户端拿它
     `pw_stream_connect(PW_DIRECTION_INPUT, node)` 消费。所以这个模块同时是个 PipeWire 生产者：

     - PipeWire 的 loop 直接挂进 `wl_event_loop`（`pw_loop_get_fd` + `pw_loop_iterate`）——
       全程合成器主线程，不用锁、不存在「从 PipeWire 线程往 Wayland 资源上写事件」这种麻烦；
     - 一路流一个 `pw_stream`（输出方向、`PW_STREAM_FLAG_DRIVER`、memfd 缓冲、格式写死
       `SPA_VIDEO_FORMAT_BGRx`），`created` 之前按 v6 的要求先发 `serial`（object serial 从
       registry 的 global 事件里拿，node id 会复用）；
     - 抓帧走 vidow 自己那条路（`vidow_capture_grab`：`capture = "hide"` 的窗口照样藏得掉），
       输出提交就抓一帧推进去。

     踩到两个坑，都是「客户端一直黑屏 / 一个缓冲都拿不到」那一类：
     - **格式给成范围，消费者会挑最小值** —— `SPA_FORMAT_VIDEO_size` 用
       `SPA_POD_CHOICE_RANGE_Rectangle` 时实测协商出 **1x1**，喂帧全被「缓冲太小」挡掉；改成
       写死的 `SPA_POD_Rectangle` 之后是 1280x720。要别的尺寸让消费者自己加 filter。
     - **只在损伤时推帧，静止的屏幕一帧都没有** —— 消费者连上来之后一直是黑的（gst 那边
       0 个缓冲）。现在图转起来而手上没有帧时当场抓一帧，保证「连上就有画面」。

     **② 还要 `XDG_CURRENT_DESKTOP` 整串等于 `KDE`** —— 这条在 `xdg-desktop-portal-kde` 的
     `src/desktopportal.cpp` 里写死了：

     ```cpp
     if (qgetenv("XDG_CURRENT_DESKTOP").compare("KDE", Qt::CaseInsensitive) == 0) {
         ... ScreenCastPortal / ScreenshotPortal / RemoteDesktopPortal / Clipboard / Wallpaper ...
     }
     ```

     是**整串相等**，不是「包含」：实测 `KDE:Vidow`、`Vidow:KDE`、`Vidow:kde`、`VidowXKDE`
     全都拿不到 ScreenCast 接口（`gdbus introspect` 数出来的）。所以在 vidow 里用 KDE 那套
     portal，配置写一行就够：

     ```lua
     portal_desktop = "KDE"
     ```

     这一行同时把「录屏 / 截图交给谁」也定了：会话名是 KDE 时 `capture_backend()` 直接选
     `kde`（vidow 这边已经提供它要的协议了），生成的配置是
     `$XDG_CONFIG_HOME/xdg-desktop-portal/kde-portals.conf`。代价要知道：**所有应用都会以为
     自己在 Plasma 会话里**（GTK / Qt 的主题、Chromium 的文件对话框选择都会跟着变），不想要
     就别设。

     **实测（新增 `./test-screencast.sh`，9 项断言全绿）**：私有 dbus + headless vidow
     （真 `XDG_RUNTIME_DIR`，portal 要连用户会话里的 PipeWire）→ KDE 后端导出 ScreenCast →
     选择器对话框画在 vidow 上（截图确认过：标题「选择要与发出请求的应用程序共享的屏幕：」，
     卡片里还有**屏幕缩略图**，那也是走我们这条协议抓的）→ 点卡片 → `Start` 返回
     `node=57`、`size=(1280, 720)` → `gst-launch-1.0 pipewiresrc path=57` 拿到 **6 帧、
     6 种不同画面**（开一个纯蓝窗口 + 开关一次 overview 制造变化）。

     **还没做的**（都回 `failed`，不是静默失败）：按窗口 / 按区域 / 虚拟输出录屏。按窗口那条
     还差一层：KDE 的窗口列表来自 `org_kde_plasma_window_management`（KWin 私有），vidow 没
     实现，所以对话框里那栏写的是「没有打开的窗口」。光标模式 `hidden` / `embedded` 都实现了，
     `metadata`（把光标位置当 PipeWire 元数据发）还没做。

     还有两件小事，都是「只在没有 PipeWire 的会话里才露头」的（测试环境正好就是）：

     - **不该在日志里留 ERROR**。第一版把「连不上 PipeWire」打成了 ERROR，
       `test-blur2out.sh` 立刻红了（它断言日志里没有 ERROR）—— 录屏是可选功能，起不来是
       INFO，不是 ERROR。
     - **心跳定时器的 data 是 `pw`，失败路径里 `free(pw)` 之后它还在响** —— 野指针，
       一秒后必崩（`test-ipc.sh` 报「段错误」，`coredumpctl` 里栈顶就是 `pw_tick`）。
       失败路径里必须把定时器也摘掉。

     **新增依赖**：`libpipewire-0.3`（meson 里 `dependency('libpipewire-0.3', version: '>=0.3.60')`）。

162. **退出 vidow 要把 portal 的环境还回去；录屏后端不是 KDE**。

     两件事都是用户问出来的：①在 tty3 上测完 portal，回 Hyprland 要不要重启 portal？
     ②vidow 默认的录屏 portal 应该是 KDE 吧？答案分别是「要，而且现在不用手动」和「不是」。

     **① 环境是 vidow 改的，就得 vidow 还**。后端（`xdg-desktop-portal-gtk` 等）是 dbus
     **按需激活**的，环境取自 systemd 用户管理器；vidow 启动时把 `XDG_CURRENT_DESKTOP` /
     `WAYLAND_DISPLAY` / `XDG_RUNTIME_DIR` 换成自己的（不导入的话后端连的还是宿主合成器，
     对话框画到别的桌面上）。退出后不还，回到 Hyprland 以后新拉起来的后端连的是**已经消失
     的 vidow socket** —— portal 全线失效。而且这台机器上 `Linger=yes`，登出也救不回来：
     用户管理器不跟着会话退出，Hyprland 自己的启动逻辑只 `unset-environment` + 重新
     import，**不重启** `xdg-desktop-portal`（`strings /usr/bin/Hyprland` 里就那两条，没有
     restart）。用户自己那个 `~/.config/hypr/scripts/import-kde-portal-env.sh` 也只 restart
     了 `plasma-xdg-desktop-portal-kde`，前端没人管。

     顺手挖出来的一个真 bug：`vidow_portal_conf_sync()` 在生成内容和现有文件**逐字节相同**
     时直接 return —— 连 `import-environment` 一起跳过了。也就是**第二次**启动 vidow（配置
     文件已经是对的）时，用户管理器里留的还是 Hyprland 的值，后端连的是 `wayland-1`。现在
     拆成两步：**每次都 import 环境**，只有配置真的变了才重启前端（重启会打断进行中的 portal
     请求，能省则省）。

     新增（`src/portalconf.c`）：`vidow_portal_env_snapshot()` 启动时问一次
     `systemctl --user show-environment`，把三个变量记下来 —— 要问用户管理器，不能问
     vidow 自己的环境（从 tty 起来时 `WAYLAND_DISPLAY` 本来就是空的）；
     `vidow_portal_env_restore()` 退出时 `set-environment` 写回、原来就没有的
     `unset-environment` 掉，再重启一次前端（值是单引号包起来再拼进 sh 的）。还顺手把
     `SIGTERM` / `SIGINT` 接成 `wl_display_terminate`（走正常退出路径）：以前是默认动作直接
     结束进程，Ctrl+C 和 `pkill` 都不跑清理，环境还原也就没了。实测正常退出
     **21ms**（SIGTERM → 进程消失）。

     验证：新增 `./test-portalenv.sh`（19 项断言），用**假 `systemctl`** 把「谁在什么时候被
     怎么调」记下来 —— 不碰你真实会话的用户管理器。四轮：环境齐全 + 首次写配置 / 同一份
     配置（内容没变）/ 用户管理器里本来没有那两个变量 / `VIDOW_PORTAL_NO_RESTART=1`。
     revert-red：把 `main.c` 里那一行 `vidow_portal_env_restore()` 注释掉重编，**7 项红**
     （退出没还原 / 重启次数 1 而不是 2 / 还原顺序 / 日志缺还原记录 / 第二轮退出没还原 /
     该 unset 没 unset / set 的内容不对），恢复后 19/19 绿。

     **② 录屏不是 KDE**。GTK 后端**根本没有** ScreenCast / Screenshot 这两个接口；KDE 后端
     有，但实现建在 KWin 的私有 wayland 协议上 —— `strings /usr/lib/xdg-desktop-portal-kde`
     里有 `zkde_screencast_unstable_v1`，还有一句现成的报错
     `zkde_screencast_unstable_v1 does not seem to be available`。vidow 不实现它，指过去只会
     让客户端以为支持、一调用就失败。真能干活的只有两个：

     | 后端 | 走什么 | 本机 |
     | --- | --- | --- |
     | `wlr` | `zwlr_screencopy_manager_v1`，专为 wlroots 写 | **没装** |
     | `hyprland` | 非 Hyprland 的 wlroots 合成器上同样退到 `zwlr_screencopy_manager_v1`（`hyprland.portal` 的 `UseIn` 里就写着 `wlroots;Hyprland;sway;Wayfire;river`，二进制里也有那条退路的文案） | 装了 |

     vidow 自己实现了 `zwlr_screencopy_manager_v1`（`src/screencopy.c`；`wayland-info` 实测
     报 `zwlr_screencopy_manager_v1 version 3`），所以两个都能用。于是默认**探测装了哪个用
     哪个**（`capture_backend()`：在 `XDG_DATA_DIRS` 里找
     `xdg-desktop-portal/portals/{wlr,hyprland}.portal`，wlr 优先），一个都没装就一个字都不写
     —— 写了等于骗客户端。用户覆盖照旧能改这两行。

     实测（私有 dbus + headless vidow，会话名 `Vidow`）：前端日志
     `Using hyprland.portal for org.freedesktop.impl.portal.ScreenCast (config)` +
     `... for org.freedesktop.impl.portal.Screenshot (config)`，`FileChooser` 仍是 `kde.portal`。
     这顺便证实了 `[preferred]` 里的显式名字**盖过**后端自己的 `UseIn`（`hyprland.portal` 写
     的是 `UseIn=wlroots;…`，跟会话名 `Vidow` 并不匹配，照样被激活）。`RemoteDesktop` 同理，
     但只有 wlr 后端有实现，所以默认不写。revert-red：`test-portalenv.sh` 的 E 节拿改动前的
     二进制跑就是红的（`本机装的是 hyprland` / 日志没说明为什么不写）。

161. **在 tty 上怎么知道 portal 指向谁：`./portaltest.sh`**。

     用户问「我能否在 tty3 测试 portal」。能 —— portal 走的是**每个用户一条** session bus
     （`/run/user/$UID/bus`），跟你在哪个 tty 无关；坑在「它现在指向谁」：xdg-desktop-portal
     的前端和后端都是 **dbus 激活起来的常驻进程**，环境来自 systemd 用户管理器，跟你在哪个
     tty 起 vidow 没关系。从别的桌面切到 tty 上的 vidow 之后，那些后端还连着旧合成器 ——
     对话框会画到另一个会话里。实测你这台机器上的状态就是这样：4 个 portal 进程
     （前端 + gtk/kde/hyprland 三个后端）全指向 `wayland-1`，用户管理器里
     `XDG_CURRENT_DESKTOP=Hyprland`。

     新增 `./portaltest.sh`（体检 + 真请求）和 `tools/portalprobe.py`（GTK3 探针，
     `GTK_USE_PORTAL=1` 弹 `Gtk.FileChooserNative`）。体检六项：session bus 在不在、vidow 的
     wayland socket 是哪个（从 `$XDG_RUNTIME_DIR/vidow-wayland-*.sock` 找，不依赖 shell 的
     `WAYLAND_DISPLAY`）、systemd 用户管理器里的会话环境、**每个 portal 进程各自连着哪块屏**
     （只算当前这条总线上的，机器上别的会话不算）、生成的 `vidow-portals.conf`，最后发一次
     真请求：对话框画在 vidow 上、挂在发起窗口上（xdg-foreign）、拿到键盘焦点（xdg-activation）。

     写这个探针时踩的两件事（都不是 vidow 的问题，但会让人误判）：①原生（走 portal）对话框的
     `hide()` 里有一句 `assert(data != NULL)`，请求没建起来时收尾直接 abort（GTK 3 的
     `gtkfilechoosernativeportal.c:532`）—— 探针改成 `os._exit()` 绕开析构；②`send-key` 要两个
     参数（`send-key tap Escape`），只给一个的话那一个会被当成 `state`，命令静默失败。

     验证：私有 dbus 会话 + headless vidow 里跑一遍全绿（会话总线 / vidow socket / portals.conf /
     对话框出现在 vidow 上 / 挂在探针窗口上 / 拿到焦点，共 6 条 PASS）；真机上跑 `--check`
     如实报出「4/4 个 portal 进程还连着别的合成器」并给出收掉它们的命令。

160. **合成器自己造的指针事件没有「帧尾」：客户端把事件扣在缓冲里，等到下一个事件才处理**。

     查 159 时撞上的：headless 里注入一次按下，客户端（GTK）**5 秒都没收到**，一动鼠标才和
     下一个事件一起收下。strace 一看合成器**当场就写了**（`sendmsg` 里就是那条
     `wl_pointer.button`）—— 事件到了 socket，是客户端不处理它。

     机制：wl_pointer v5 起事件按帧分组，客户端要等到 `wl_pointer.frame` 才处理这一批。真设备
     每个帧都由 libinput 的帧事件补上（`server_cursor_frame` → `notify_frame`），而**注入**和
     **合成器自己造的**那几路没有设备帧：IPC 的 move-cursor / pointer-button、
     `VIDOW_TEST_ACTION` 的 pointer-motion / pointer-button、以及松手时补的那对（位置 + 松开）。
     滚轮注入一直是补的（`vidow_inject_pointer_scroll` 里那句 `notify_frame`），其余几处漏了。

     后果两层：①测试里注入的时序全被带偏 —— 按下 / 松手 / 移动要等到**下一个**事件才生效，
     看起来像「这条动作没执行」（我前两轮查 159 时就被它带偏过一次，把 0.6~9.5 秒的延迟当成了
     合成器没 flush）；②真实会话里 159 补的那对（位置 + 松开）会被客户端扣住，用户照样得
     「移出去再移回来」。所以这不是测试专属问题。

     改法：注入的三条路（IPC move-cursor / IPC pointer-button / FIFO 的 pointer-motion 与
     pointer-button）和松手补的那对，各自补一句 `wlr_seat_pointer_notify_frame()`。

     回归：`./test-titlebar-drag.sh` 里按下当场到客户端（修之前要等下一个事件，实测 ~13 秒），
     拖拽从「只跟到 360,224」变成 400,230（1:1）。回退 `src/keybind.c` 那两句跑同一条：三条
     断言红、拖拽只跟到 360,224 ✓。

./test-titlebar-drag.sh              # 客户端发起的拖拽（CSD 标题栏）松手要还给它（12 项断言）
159. **「用标题栏拖一次之后窗口就不听使唤」：客户端那次「按下」永远等不到「松开」**。

     用户报「微信登录界面用 win 加左键拖拽是正常的，**直接通过标题区域拖拽**就会出问题」。
     两条路只差一处：Mod+左键那条**按下是合成器吃掉的**，客户端压根没见过；标题栏拖拽是
     **客户端自己发起**的（CSD 标题栏 → `xdg_toplevel.move`；X11 是 `_NET_WM_MOVERESIZE` →
     `request_move`），它的按下在 `vidow_pointer_button` 末尾已经 `notify_button(PRESSED)`
     转给客户端了。

     而松手那条分支（`if (!pressed) { *_end_grab(); return; }`）把松开一起吃掉 —— 客户端手里
     那次按下永远等不到配对的松开。GTK 的 `in_move_drag`、Chromium 的鼠标键状态都靠松开复位，
     于是它**一直以为左键按着**：鼠标一动它还在拖，之后第一下点击被当成「按下」而不是
     「点一下」，得再点一次才能恢复（用户描述的「点一下界面、移出去再移回来再点一次」正是这个）。

     改法：`server->grab_client_press` —— `vidow_float_begin_grab()` 多一个 `from_client`
     参数，客户端发起的那四条路（xdg 的 request_move/request_resize、x11 的
     request_move/request_resize）传 true，Mod+拖拽和平铺拖拽传 false；松手时把这次按下
     **还**给客户端，顺序放在补位置那次 motion **之后**（motion 先把焦点和局部坐标刷成当前值，
     松开才落在对的位置上）。两边对称：客户端没见过的按下，松手也不转给它。

     新增 `./test-titlebar-drag.sh`（12 项）：探针是 tools/movedrag.py（GTK3，自己定尺寸
     320x240 → vidow 直接当浮窗，顶部 30px 当自定义标题栏，按在那里就 `begin_move_drag`）。
     判据：按下真的到了客户端、客户端发了 move 请求、合成器起了 grab、拖拽 1:1 跟手、
     **松手后客户端收到松开**、落点没跑、松手后移出去再移回来收得到位置；再加一条反向对照：
     Mod+左键那条路客户端**既没收到按下也没收到松开**（状态对称）、照样跟手。回退
     `src/input.c` 那一句跑同一条：只有「收到松开」这条红（其余 11 条全绿）✓。

158. **微信弹窗不显示的真凶：unconstrain 的盒子给的是「布局坐标」，而 wlroots 要的是「表面局部坐标」**。

     157 把定位做到了「弹出物交了帧、节点开着」，用户那份日志里弹出物的场景节点坐标是
     **4800,115** —— 而屏幕只有 0..3840：**菜单画在屏幕外**。

     机制：`wlr_xdg_popup_unconstrain_from_box()` 要的是**根 toplevel 表面自己的坐标系**里
     的可用区（它内部会减掉 popup 的 toplevel 坐标，见 wlroots 的 `wlr_xdg_popup.c`），而
     vidow 直接把自己那块屏的 `wlr_output_layout_get_box()`（**布局坐标**）递进去了。窗口不在
     布局原点 (0,0) 上时两者差着一整个窗口原点：客户端于是以为「可见区」在它右边一整块屏的
     地方 —— 微信主窗口在 DP-2 上（x=2880），菜单就落到 2880 + 1920 = **4800**（1920 就是那块
     屏的 x）。窗口贴在 (0,0) 附近时这项差值是 0，所以**单屏 + 窗口在左上角永远测不出来**；
     Hyprland 正常，是因为它减了 `t1ParentCoords`（`CPopup::unconstrain`）。

     改法：新增 `vidow_popup_box_to_toplevel_space()`，把输出盒平移回表面原点
     （`wlr_scene_node_coords(view->scene_surface->buffer->node)`），`view.c`（窗口上的弹出物）
     和 `layer.c`（状态栏上的弹出物）两条路都改；顺带按窗口**实际落在**的那块屏算可用区
     （IPC 挪位置 / 跨屏拖拽之后 `view->output` 可能还是旧的那块，浮窗位置是绝对坐标、不依赖它）。

     新增 `./test-popup2out.sh`（13 项）：两块 headless 屏（HEADLESS-2 在 0,0、HEADLESS-1 在
     1920,0），客户端是 tools/menuprobe.py（GTK3，按一下弹菜单）。两轮跑同一套流程 ——
     窗口放 2200,300（1920 那块屏）是主判据，窗口放 200,200（原点那块屏）是反向对照。每轮
     断言：unconstrain 盒子 == 输出原点 − 窗口原点、弹出物节点落在窗口那块屏的盒子里、
     窗口那块屏像素变化 ≥ 500 而另一块屏 0 像素变化。回退 `src/view.c` 那一句跑同一条：
     主判据三条全红（盒子 1920,0、节点 **4120,831**、那块屏 **0 像素变化**），对照轮照旧全绿
     （原点处差值本来就是 0）✓；补回改动：盒子 -280,-300、节点 2676,831、23760 像素变化。

157. **诊断：每个 xdg_popup 的 commit 一行一条（「弹窗不显示」到底卡在哪一步）**。

     用户报「微信弹窗不显示」，而且**别的合成器下正常**。先把它从「xwayland 弹窗」里摘出来：
     微信主窗口在 vidow 里是 **Wayland 客户端**（`kind=xdg`，登录窗才是 X11），所以它的
     右键菜单是 **xdg_popup**。用户那份日志里点得很清楚：

     ```
     00:01:16.660 点击 button 273 在 3273,154：命中 微信（局部 393,148）…
     00:01:16.663 New wlr_surface 0x…f230 / new xdg_surface 0x…4500
     00:01:16.663 popup: unconstrain 完成（父面 0x…8c30，可用区 1920,0 1920x1080）
     ```

     —— 弹出物建出来了、configure 也发了，然后**日志里再没有一个字**。

     但本地拿 Wayland 客户端复现不了：Chromium（`--ozone-platform=wayland`）右键菜单在
     vidow 里**画得好好的**（截图为证，1 倍屏、2 倍屏、两块屏都试了），所以不是「vidow 不会
     画 xdg_popup」。缺的是「弹出物自己交没交帧」这一步的信息，于是给 `server_new_popup()`
     挂了一条诊断：

     ```
     popup: commit 缓冲 461x463 逻辑 461x463 节点开着 在 300,301（表面 mapped=1 initialized=1）
     ```

     一行一条（弹出物本来就不常 commit），三种情况一眼分开：**没有这一行** = 客户端在等
     configure、压根没交缓冲；**有行但节点关着 / 在屏外** = 节点没使能或位置算错；**有行、
     节点开着在屏内** = 画了但被别的节点压住（层级问题）。Chromium 那条路上它连打 8 行，
     节点在 300,301 开着 —— 和截图对得上。

     顺带修了 `tools/popup.py`：GTK 3.22 起 `popup_at_*` 要一个触发事件，原来传 None 只留一句
     「no trigger event for menu popup」然后把菜单吞掉（这个工具是专门用来撞 unconstrain 那条
     assert 的，菜单压根没弹过）。现在传真事件，没有就合成一个 BUTTON_PRESS。

156. **拖完第一下点击点到别处：松手之后要把指针位置补给客户端**。

     用户报「微信登录页面拖拽后点击一下界面、再把鼠标移出去再移回来、再点一次好像就能
     交互了」—— 「移出去再移回来就好了」这句就是答案：那是 `enter` 事件在刷新客户端的
     指针位置。

     机制：拖拽期间每一次移动都被 grab 吃掉（`vidow_pointer_motion` 里
     `grab_mode != NONE` 直接转给 grab 然后 return），所以**客户端手里那份指针位置还是
     起拖之前的**。窗口跟着光标挪了 D 像素之后，客户端按旧位置算出来的局部坐标就偏了
     **D 像素**（拖 300 像素 = 第一下点击落在 300 像素之外）—— 屏幕上「点不着」。
     松手那一刻 vidow 也没补：`vidow_pointer_button` 的 grab 分支处理完 `*_end_grab()`
     就 return 了。

     改法：松手、收完 grab 之后补一次 `vidow_pointer_motion(server, time_msec)` —— 和
     真实移动走同一条路，于是「指针在窗口里」就补一次 motion、「已经不在窗口里」就
     `notify_clear_focus`，两种情况客户端都当场对上。

     `./test-drag-follow.sh` 加了一节（共 9 项断言）：拿 tools/xpopup 当探针（只有 X11
     窗口会打「x11: 指针」那行日志）。先把它拖起来（Mod+左键 + 4 次移动）再松手，断言
     **松手之后立刻有 ≥1 条「x11: 指针」**。回退 `src/input.c` 跑同一条：一条都没有，当场
     红；补回改动：松手后 1 条。

155. **拖动「不跟手」：浮动窗口的位移补间被每一次指针移动重起，窗口一直落在光标后面**。

     同一条线报的另一半（「很不跟手，感觉是更新的问题，不是性能问题」）—— 用户这句直觉是对的。

     那份 74 秒的日志里把五段拖拽的补间逐帧拆开（日志里写着「画在哪儿 -> 该在哪儿」）：
     拖 273px/s 时**中位滞后 199px、最大 387px**；起拖的头 125ms 窗口干脆一动不动
     （补间被每帧重起一次，而缓动在 t≈0 那一段几乎不走）。

     机制：浮动窗口拖动时，指针每动一下就 `float.c` 改 `float_geom` → `arrange` →
     `vidow_place_view()`，而那里对「布局把它挪了」一律起一个 `window_move_ms` 的位移补间
     （用户配的是 190ms）。平铺窗口的拖拽（`drag.c`）本来就是「只动场景节点、布局一行都不
     重排」，1:1 跟手 —— 浮动窗口没理由不跟。

     改法：`vidow_place_view()` 里认出「这个窗口正被鼠标拖着」（`server->grab_view == view
     && grab_mode == VIDOW_GRAB_MOVE`）就不起补间、把 `anim_off` 清零 —— 位置直接跟光标。
     果冻（jelly.c）照旧，那是形变反馈，不是位置滞后。

     新增 `./test-drag-follow.sh`（6 项）：靶子是 tools/pattern.py（左上角纯红、没有壁纸 =
     纯黑背景），先截一张静止图量出「客户区相对窗口盒」的固定偏移（实测 +2,+37 = 标题栏），
     再按住 Mod 拖 320 逻辑像素。判据：①拖拽真的起来了 ②拖动期间**一条位移补间都没有**
     ③补间日志里「画在哪儿 vs 该在哪儿」的最大差 == 0 ④窗口盒跟着光标走了 320 像素
     ⑤松手后落点没跑 ⑥vidow 活着。回退 `src/view.c` 跑同一条：**8 条补间、最大落后
     184px**（②③ 当场红）；补回改动：0 条补间、0px。

154. **拖拽「一顿一顿」的真凶：两块屏轮流给同一个窗口重建模糊金字塔（每帧十几块 GBM 缓冲）**。

     用户报「拖拽还是会卡，多拖拽一会儿还是会卡」。这次没靠猜：`./dbg.sh` 录了一份 74 秒的
     debug 日志（63066 行），把拖动拆成帧来算。

     **症状（同一份日志里量出来的）**：五段拖拽，423 条补间记录，相邻帧间隔中位数 6-7ms
     （160Hz 的屏，说明帧是出来的），但 **≥8ms 的卡顿帧有 18 个**，最大三个 153ms / 111ms /
     84ms。153ms 那个窗口里日志一行都没有 —— 主循环整个被按住了 150ms。

     **真凶**：`fx: target` 这行日志是**分配**缓冲时打的（`vidow_alloc_target`），拖拽那 2 秒里
     它出现了 **7444 次**，按帧切开是每帧 16-32 块。再看 `blur: own region`：同一个 280x380 的
     微信窗口，**每帧出现两次、尺寸还不一样** —— `640x768 at 1280,128` 和 `896x1152 at
     1024,256`。拿公式反推（pad 12、margin 73、网格 128）就认出来了：这两个尺寸是**同一块屏
     的两种 scale** —— 1 倍屏算出来 640x768，2 倍屏算出来 896x1152。

     机制：`vidow_blur_update(output)` 里那个窗口循环**没筛屏**（旁边 layer 的循环是筛的：
     `if (ls->output != output) continue`），于是每块屏都按自己的 scale / 尺寸给**同一个窗口**
     算私有区域，而那块金字塔挂在窗口身上（`view->fx.blur_set`）：两趟每帧互相把对方的缓冲
     丢掉重建。一次重建 = 1 块区域底衬 + 4 块 down + 3 块 up = 8 块 GBM 缓冲，最大那块
     2304x2560 = 23 MB。全日志 51060 次分配 / 6402 次重抓，正好 8:1。

     两处都补上筛屏（`view->output != output->wlr_output` → 跳过）。顺带修掉一个一直藏着的
     正确性问题：以前「区域和贴图归哪块屏」取决于最后跑的是哪一趟，跨屏的窗口可能采到另一块
     屏的底衬。

     新增 `./test-blur2out.sh`（6 项）：两块 headless 屏（1 倍 + 2 倍），壁纸 + 三个半透明浮窗
     （wA 在 1 倍屏上、wB/wC 在 2 倍屏上互相压着），每步挪 130 逻辑像素（> 128 的网格，保证
     区域真的换格子）动 10 步。判据：①前提：wB 真走了私有区域那条路（重抓 ≥3 次）②这段
     时间里**分配 0 块缓冲**③同一个窗口只有**一种**区域尺寸④区域尺寸跟它所在屏的倍率对得上
     ⑤vidow 活着。回退 `src/fx.c` 跑同一条：**432 块分配 / 32 次重抓**、两种区域尺寸
     （`1152x1024` + `768x768`）—— ②③ 当场红；补回改动：0 块分配、一种尺寸。

153. **崩溃：渲染器不是 GLES2 时，每帧那次 GL 提交会踩 wlroots 的 assert（从 core dump 里挖出来的）**。

     用户报「微信登陆直接导致 vidow 崩溃」。`coredumpctl` 里有三份 vidow 的 core（20:00 / 21:16 /
     21:56，全是 SIGABRT），栈一模一样：

     ```
     #4 __assert_fail
     #5 libwlroots-0.20.so + 0x224c4      ← 0x224c4 处那条断言的字面量是
     #6 libwlroots-0.20.so + 0x2255f         wlr_renderer_is_gles2(wlr_renderer)，
     #7 gl_make_current (vidow)              文件 render/gles2/renderer.c，函数 gles2_get_renderer
     #8 vidow_fx_flush (vidow)
     #9 vidow_output_render
     #10 output_frame
     ```

     断言的字面量是反汇编 0x224b6/0x224be 两条指令读出来的（rdi = `wlr_renderer_is_gles2
     (wlr_renderer)`、rsi = `render/gles2/renderer.c`、rcx = `gles2_get_renderer`）。

     机制：`wlr_gles2_renderer_get_egl()` 第一行就是那条断言 —— 渲染器不是 GLES2 时它**不是
     返回 NULL，是当场 abort**。而 `vidow_fx_flush()`（152 那条「一帧的离屏趟只提交一次」的
     收尾，d4973b5 加的）原来是无条件调的：装饰 / 模糊那条路本来就在 `shader_ok` 底下
     （渲染器不是 GLES2 时压根不跑），只有这个新加的每帧提交漏了。于是「软件渲染兜底
     （pixman）」这种完全合法的情况下，第一帧就 abort。

     两处一起收：`gl_make_current()` 自己先判 `wlr_renderer_is_gles2()`（不把「判类型」交给
     那个会 abort 的函数）；`vidow_fx_flush()` 在 `!server->shader_ok` 时直接走人。

     新增 `./test-pixman-renderer.sh`（5 项）：`WLR_RENDERER=pixman` + headless，断言
     ①真的在用 pixman ②日志有「renderer is not GLES2, decorations and blur off」③进程活着
     ④日志里没有 Assertion ⑤窗口照样上屏（纯蓝像素 425414 个）。回退 `src/fx.c` 跑：③④⑤ 全红，
     日志里就是 `vidow: render/gles2/renderer.c:38: gles2_get_renderer: Assertion ... failed.`
     + 核心已转储。

152. **窗口「不再重叠」之后模糊底衬变成一块被缩放 + 错位的壁纸：区域要跟着贴图一起换**。

     用户报「窗口没有重叠的情况下，模糊背景是错误的」。headless 上按「重叠过 → 又分开」
     造了一遍（壁纸 + 半透明浮窗 A + 压上来的浮窗 B，B 再挪走），两张截图逐像素比 ——

     | | A 客户区里 412x349 那块 |
     | --- | --- |
     | 修之前（「A 单独」vs「B 挪走之后」） | **138834 / 143788 个像素不一样，最大差 140** |
     | 修之后 | **0 个像素不一样（最大差 0）** |

     修之前的截图一眼能看出来：模糊里那排黄方块被纵向拉长、整块图案是错的（缩放 + 错位）。

     机制：窗口用哪张模糊贴图由「它有没有和别的窗口重叠」决定（`view_overlaps_other`）——
     不重叠就用**共享**那张（整屏壁纸，`fx->blur_tex_w == 0` 是「整屏」的记号），重叠就用
     它自己那块**私有区域**（768x768，`fx->blur_tex_x/y/w/h` = 区域原点尺寸）。装饰着色器
     按这两个字段算采样 UV（`decor_draw` 里 `u_origin` / `u_output_size`）。而窗口从「重叠」
     变回「不重叠」时只换了贴图、**没换区域** —— 着色器于是拿整屏那张贴图按 768x768 的区域
     去采样，屏幕上就是一块被缩放 + 错位的壁纸。拖动 / 滚动 / 开窗动画期间窗口重叠过一下
     之后就一直是错的，直到它再次重叠（所以用户看到的是「不重叠的窗口模糊不对」这个静态
     现象）。layer surface 那条路本来就在切回共享贴图时把 `blur_tex_w/h` 清 0，窗口这条路
     漏了 —— 现在两边都从 `blur_assign(fx, tex, owned, id, region)` 一个入口走：区域是参数
     的一部分，也进「贴图身份」的比较（同贴图换了区域也要 `blur_ver++`，不然装饰里烘的还是
     上一版的 UV）。`view_region_build` 不再自己登记这四个字段（一处管一处）。

     新增 `./test-blurnooverlap.sh`（5 项）：①前提：A 重叠时真的建过私有区域底衬；②反向
     对照：B 压着 A 时 A 的底衬里看得到 B 的蓝（证明私有区域不是白建的）；③A 的客户区在
     「从没重叠」和「重叠过后又分开」两张截图里**逐像素一致**（只比客户区：标题栏 / 焦点
     边框的颜色会因为焦点变了而变）；④分开之后不再重建私有区域；⑤vidow 全程活着。回退
     `src/fx.c` 跑：第 ③ 条红（101928 个像素里 98028 个不一样，最大差 93）。

151. **拖动不再「一顿一顿」：看门狗按后端自己的帧间隔补帧，而且不再被指针事件饿死**。

     接 150（拖动冻住）之后用户接着报「拖拽还是会卡，一顿一顿的，很不跟手，感觉是更新的
     问题，不是性能问题」——**用户判断对了**。在用户的 Hyprland 会话里嵌套跑，按住帧事件
     （`VIDOW_TEST_STALL_OUTPUT=WL-1` = 复现「上层合成器不给帧」）注入 2 秒匀速拖拽
     （400 个事件 / 200Hz）——

     | | 拖动期间的出帧 | 事件→出帧延迟 |
     | --- | --- | --- |
     | 修之前 | **2.0 fps（6 秒 12 帧，其中拖动帧 1 个）** | 24.01ms |
     | 修之后 | **88.9 fps**（45 帧/506ms，拖动帧 45 个） | 平均 2.6~3.4ms / 最大 5.7ms |

     后端正常出帧时**一帧都不多推**：同一场景 `STALL=none` 量出来还是 144.0 fps（和修之前
     逐位相同），拖拽延迟 2.8/5.6ms —— 看门狗只在后端迟到时才补。

     两个真问题叠在一起：

     - **看门狗被指针事件饿死**：`wl_event_source_timer_update` 是「重排」不是「没排才排」，
       而拖动每个指针事件都会 `vidow_output_kick_watchdog()` —— 1000Hz 的鼠标每 1ms 把 24ms
       的闹钟往后推一次，闹钟永远不响（实测 6 秒只出了 1 个拖动帧）。现在用
       `server->watchdog_armed` 记住「已经排上」，kick 只负责启动。
     - **看门狗一次就停**：它只在「动画 / 果冻在跑」时续期，拖动不在里面 —— 响一次之后要等
       新的指针事件来 kick。现在拖动也续期，而且按「这个间隔还剩多久」精确排下一次
       （固定 4ms 空转的话 10ms 的目标节奏会落成 12ms，白丢 20% 的帧）。
     - 顺带把那条写死的 40ms 换成**自适应**：`watchdog_interval_ns()` 跟后端帧间隔的滑动平均
       （`output->frame_gap_ns`，只喂 < 25ms 的健康间隔，免得后端自己慢就把目标越拉越长），
       比它晚 1/4 才补一帧，钳在 [6ms, 40ms]；量到过正常间隔就按它的 1.25 倍，拖动期间还没
       量到就按 8ms（~125Hz）猜、动画按 16ms 猜。`last_render_ns`（后端给的帧**或**看门狗
       推的帧）专门给看门狗限速用，不并进 `last_frame_ns` —— 那个是「后端最后一次给帧」，
       帧追踪（`VIDOW_TRACE_FRAMES`）靠它判帧链断没断，看门狗推的帧不能算进去。

     `./test-drag-stall.sh` 里那条「平铺拖拽期间出了 N 帧」从 7 帧涨到 69 帧（同一套断言，
     判据是「≥5 帧」，所以测试照样过 —— 它抓不住这个，量出来的数才是证据）。

150. **后端不给帧事件的时候，拖动不再冻住：保帧看门狗把「正在拖」也算进来**。

     用户报「hyprland 嵌套启动的 vidow，拖拽非常不跟手」。在用户的 Hyprland 会话里嵌套
     跑起来量（`./build/vidow` + `WLR_BACKENDS=wayland`，注入拖拽，`VIDOW_RENDER_STATS=1`
     + `VIDOW_TRACE_FRAMES=1`）——

     | 时间 | 出帧 |
     | --- | --- |
     | 起来头 0.5s | 143.9 fps |
     | 拖之前 | 22.2 fps（1126ms 里 25 帧） |
     | **拖的这 2.9 秒** | **0.3 fps（1 帧）** |
     | 松手之后 | 144 fps |

     帧追踪那两行点得很清楚：`trace: WL-1 1540ms 没出帧（vidow 主动要过 597 帧，最近一次：
     拖浮动窗口）` —— 要了 597 次帧，一帧都没来。原因在嵌套后端：帧事件只由**上层合成器**
     送来（`wl_surface.frame` 回调，wlroots 的 wayland 后端在每次 commit 时申请），
     上层一旦不给（窗口被别的窗口盖住 / 那台合成器自己闲着），`frame_pending` 就一直挂着，
     之后所有 `wlr_output_schedule_frame()` 全是空转。

     vidow 本来就有对付这件事的看门狗（`src/output.c` 的 `anim_watchdog`：40ms 没帧就自己
     推一帧），但它的条件只有「动画 / 果冻在跑」——**拖动不在里面**，而且它只在动画开始时
     武装。于是：拖的时候帧事件一断，没人再推帧，而浮动拖拽的落位是**推迟到帧里**做的
     （见 float.c 的 `arrange_dirty`），帧停了它就一点都不动 —— 屏幕上就是「光标在走、
     窗口不动」。

     改法（三处，都在「有活在飞」这条线上）：
     - `anim_watchdog` 的条件加上「正在拖」（`server->grab_view != NULL`）；
     - 两个拖动更新处（`vidow_tile_grab_update` / `vidow_float_grab_update`）要帧之后顺手
       武装看门狗（要帧那一下本身可能被「后端已经欠着一帧」挡掉）；
     - `output_frame` 收尾那一支同样把「正在拖」算进来，并在追踪里写成「拖动在跑」。

     验证（新增 `./test-drag-stall.sh`，10 项断言；用现成的测试钩子
     `VIDOW_TEST_STALL_OUTPUT=HEADLESS-1` 把帧事件按住，headless 上就能复现）：
     把修复回退掉跑一遍 —— 按着左键拖 240x120，窗口**一动不动**（`geometry` 还是 100,100），
     拖动期间 0 帧；带上修复：位置跟着走到 340,220，拖动期间 7 帧（看门狗推的），
     平铺拖拽那条 28 帧。嵌套实测：拖的那两秒 144~210 fps，vidow 侧的
     「事件 → 出帧」延迟 0.9~2.6ms（最大 6.7ms）。

     顺带给 `VIDOW_RENDER_STATS=1` 加了一项 `拖拽延迟 平均/最大ms(次数)`：它就是上面那个
     延迟，嵌套在别的合成器里时「跟不跟手」看这一项。另记一条排查结论：嵌套模式下**光标
     是上层合成器画的**（vidow 没有光标平面），所以上层一旦不给我们帧，光标照走、窗口不走，
     看着就是「完全不跟手」—— 这也是这条修复要治的。

149. **一帧的离屏趟只提交一次：每趟一次 `eglMakeCurrent(EGL_NO_CONTEXT)` / `glFlush` 是白付的内核提交**。

     查「还有没有性能问题」时先量了一遍最坏情况（一个不停重画的窗口压在三层模糊窗口上、
     和它们互相重叠，2560x1440 缩放 2）：**一帧 25 趟离屏渲染**（模糊链 + 每个窗口一趟装饰）。
     给 `end_pass()` 加了个计时探针，把一趟拆开看 ——

     | 一趟里的一段 | 一帧合计 |
     | --- | --- |
     | `eglMakeCurrent`（进入时绑定） | 0.01ms |
     | `glBindFramebuffer(0)` + `glFlush()` + `eglMakeCurrent(NO_CONTEXT)`（收尾） | **0.40ms** |
     | 整趟（含画） | 0.56ms |

     收尾那 0.40ms 就是每趟一次的内核提交（amdgpu 上一次 ~40µs）。单独把 `glFlush()` 去掉
     没用（解绑上下文自己就会 flush，实测 0.43ms → 0.41ms）；把**解绑**去掉才有效。

     改法：`end_pass()` 只解绑 FBO，不 flush、不解绑上下文；新增 `vidow_fx_flush(server)`，
     在 `wlr_scene_output_commit` **之前**把这一帧所有离屏趟的命令一次性提交（放 commit 前是
     因为那之后 wlroots 可能走直扫，一个 GL 调用都不发、没人替我们 flush）。安全性：同一个
     GL 上下文里的绘制顺序由 GL 保证（装饰缓冲被场景当贴图采样，不 flush 也一定读到新内容）；
     wlroots 的 `wlr_egl_make_current()` 每次都会真的 `eglMakeCurrent`（不缓存），上下文留
     在这儿绑着不会让谁算错；换上下文时 EGL 自己会 flush 掉旧的那个。

     实测（同一套场景，A/B）：

     | 场景 | 指标 | 改前 | 改后 |
     | --- | --- | --- | --- |
     | 最坏情况（25 趟/帧，模糊 + 装饰 + 提交） | 每帧 | 1.38ms | **0.64ms** |
     | 同上 | 进程 CPU | 9.3~10.2% | **5.3%** |
     | 8 个平铺窗滚动（9.8 趟/帧） | 重烘 | 0.56ms/帧 | **0.11ms/帧** |
     | 同上 | 果冻 | 0.47ms/帧 | **0.10ms/帧** |
     | 同上 | 进程 CPU | 6.8% | **4.8%** |
     | 同上 | 帧率 | 54.1 fps | 58.1 fps |

     顺带给 `VIDOW_RENDER_STATS=1` 那行加了两项：`趟 N`（这一帧开了几趟离屏渲染）和
     `重烘 X.XXms/帧`（所有装饰重烘加起来多少钱 —— 拖动 / 滚动时每个在动的窗口每帧烘一次，
     所以这项随窗口数线性涨；次数仍看前面的「N 次重烘」）。

     回归：`test-blurcache` / `test-blurpartial` / `test-blursmall` / `test-blurlive` 全绿，
     全量 51 条全绿。

148. **拖动跟手：一帧里来过的那些移动事件只做一次活**。

     用户说的方向 3（帧节奏 / 事件合并）。先量：写个探针，往动作 FIFO 里灌两万条
     `pointer-motion`（和真实鼠标同一条路），量 vidow 处理完要多久 ——

     | | 20000 次移动 | 每次 |
     | --- | --- | --- |
     | 不拖拽（只是移动指针） | 0.14 秒 | **7 µs** |
     | 拖拽中（浮动窗口跟手） | 1.47 秒 | **73 µs** |

     一帧里指针比刷新快的时候会来好几个事件（1000Hz 鼠标 @160Hz ≈ 6 个，8000Hz ≈ 50 个），
     中间那些位置**屏幕上从来没出现过** —— 以前每个事件都整排一次（所有窗口重新 place，
     每个都要重烘装饰图：整块窗口那么大的离屏渲染），实测每个事件 70µs。8000Hz 鼠标就是
     每帧 3.5ms 白算（160Hz 的帧预算一共才 6.25ms）。

     **做法**：把「每个事件都干、但只有最后一帧才有人看」的活推到出帧之前做一次。
     - 平铺拖拽（`vidow_tile_grab_update`）：位置**立刻**摆到场景节点上（便宜，而且节点一
       动就自带损伤、帧也排上了），装饰图的重烘 + 记损伤推到帧回调里（`output.c` 那段），
       一帧一次。`vidow_view_apply_anim` 因此拆成「摆位置」（`view_anim_place`）和「重烘」
       （`vidow_view_anim_bake`）两半
     - 浮动拖拽（`vidow_float_grab_update`）：原来每个事件都 `vidow_arrange()`（整排一次），
       现在只置 `arrange_dirty`，帧回调里排一次
     - 损伤照旧从「上一帧画在哪」算到「这一帧画在哪」：中间那些没上屏的位置本来就不用记，
       跨过的那一大截照样算数（拖得快也不会留残影）
     - 要帧那一下是幂等的（`wlr_output_schedule_frame` 里有 `frame_pending` 挡着），
       所以一帧最多催出一次渲染，不会变成「每个事件渲染一遍」

     **证据**（同一个探针，20000 次移动）：

     | | 改前 | 改后 |
     | --- | --- | --- |
     | 拖拽中 | 1.47 秒（73 µs/次） | **0.135 秒（6.7 µs/次）** |
     | 每帧装饰重烘 | ≈ 200 次 | **≈ 2 次** |

     即每个事件的额外开销 ≈ 0（和不拖拽时一样），拖拽期间每帧最多一次重排 + 一次重烘。
     回归全过（52 条，含 test-drag / test-blurcache / test-jelly / test-bounce /
     test-cursor-shake / test-overview）。

147. **链条也按损伤只跑那一小块 —— 其余从上一版拷过来**（146 那节列的方向 2）。

     146 把「重抓」的范围缩到了受损那一小块，但**金字塔那 7 趟还是整块区域重跑**：区域
     1152x1024 的时候，就算只有 80x40 那一块变了，每一趟也要把整块（或者半块、四分之一
     块…）重新算一遍。这一轮让链条也只跑受损那一块，其余像素**一个都不碰** —— 它们还是
     上一版的结果，而上一版的结果是对的（变的就是那一小块）。

     **做法**（`src/fx.c`）：
     - kawase 那两趟换了个顶点着色器：多两个 uniform（`u_rect` = 目标里的一块、`u_dst` =
       目标尺寸），整块跑时 `u_rect = (0,0,w,h)`，跟原来那个顶点着色器**逐像素等价** ——
       两条路共用同一份采样代码，不会出现「整块和局部算出来不一样」
     - `blur_chain_rect()`：从受损那一块出发逐级算「这一趟要重画的矩形」= 上一级的矩形
       缩一半（往上那几趟是放大一倍）+ 这一级的采样半径。半径用整条链的采样半径按级折算
       （`blur_reach_px >> (i+1)` + 4）—— 比真实需要的大得多，是**够用且宽松**的上界：
       局部重跑绝不能因为半径算少了而在新旧模糊值之间留一道缝
     - 抓取也一样：只清受损那一块、只拷共享底衬对应的那一块、窗口也只画进那一块（**真的
       夹住**，不只是剔除 —— 不夹的话整块窗口会重画一遍压在保留的像素上，半透明的东西
       （窗口自己的阴影、薄纱窗口）被叠第二次，屏幕上就是「局部更新之后那一圈变深了」；
       这个是写完之后用逐像素比对测出来的）
     - 抓取那块四边形和链条那几趟的四边形都正好盖满各自的目标矩形（片元每个像素都写、
       混合关着），所以局部更新**不用清屏** —— 清反而会把要保住的那部分清掉
     - 转过的客户端缓冲（`wl_surface.set_buffer_transform`）没法用同一套「夹 dst 框 + 按
       比例夹 src 框」的办法，碰到就整块重跑（极少见）
     - 开关 `VIDOW_BLUR_NO_PARTIAL=1` 强制整块重跑，只给 A/B 比对用

     **证据**（headless 2560x1440@2，壁纸 + 半透明浮窗 A + 压在 A 上的浮窗 B，B 每帧只让
     窗口里 80x40 那一小块失效；量 6 秒）：
     - 切换前 1.2 秒里 79 次重抓**全部**走局部：每次动的块平均 **13776 像素**（164x84），
       整块区域是 118 万像素 —— **86 倍**
     - 切到整块之后那 1.2 秒：37 次全是整块重跑、局部 0 次
     - 两张截图（整幅 3686400 像素）逐像素比对：**差 > 2 的像素 0 个**（最大差 2，是浮点
       UV 的 1 ulp）；把链条那几趟的采样半径改成 0 再跑一遍：**216 个像素差 > 2、当场红**
       —— 这条测试真的抓得住「缝」
     - 新增 `./test-blurpartial.sh`（6 项）：**同一个进程、同一个场景**里先走局部、再
       建出 `VIDOW_BLUR_NO_PARTIAL_FILE` 切到整块，两张截图逐像素比（客户端、位置、内容
       一个字都没变，变的只有重抓的方式）；`tools/spin.py` 多一个 `step` 模式（内容什么时候
       变由 `SPIN_STEP_FILE` 控制，变完就停住）
     - 回归全过（52 条，含新的 test-blurpartial.sh）

146. **损伤精确到「客户端真动过的那一块」—— 窗口里只有一条进度条在动，不该让整片模糊重抓**。

     上一节（145）把损伤做成了「按区域作废」，但某个窗口**报了损伤**时算的还是**整块窗口**：
     一个 1000x800 的窗口里只有一条 300x20 的进度条在动，压着它的那个窗口底下那片模糊照样
     整块重抓。wlroots 手里有客户端交上来的表面损伤（`wlr_surface.buffer_damage`），这一轮
     把它接上了。

     **做法**（`src/view.c` / `src/fx.c` / `src/server.h`）：
     - `vidow_view_surface_damage()`：把 `surface->buffer_damage`（**缓冲像素**坐标，wlroots
       在 `surface_update_damage()` 里已经把 surface/buffer damage、viewport 的 src 偏移、
       scale、transform 全算进去了）除以「缓冲像素 / 表面逻辑像素」得到表面局部坐标，再加上
       场景节点在布局里的位置（`wlr_scene_node_coords`）就是布局逻辑坐标。三种结果：
       - `BOX`：就是这一块（大多数客户端每帧只报一小块）
       - `NONE`：**一个像素都没变**（只有帧回调 / ack_configure 的那种 commit）—— 世界代次
         都不用动，别人缓存的底衬就还是对的（以前这种 commit 也白记了一笔损伤）
       - `WHOLE`：说不清，退回整块窗口（宁可多抓一次，不能糊着旧的）。说不清的情形：有
         transform / viewport 缩放（chromium 那类）、表面带 subsurface（它们的 commit 我们
         没在听，拿不到损伤区域）、场景节点被改成别的显示尺寸（overview / X11 缩放）、
         overview 正在跑（那时节点位置是映射过的）、换了新缓冲却没报损伤
     - 只算表面里**看得见**的那一块：客户端可以声明窗口几何比表面小（GTK 的 CSD 留白 ——
       装饰着色器只采窗口几何那一块，见 `vidow_view_fx_geo`），损伤全落在留白里 = 屏幕上
       什么都没变
     - 顺带修掉一个「并集吃掉中间」的坑：攒下来的损伤原来存成**一个包围盒**，两个客户端在
       同一帧里各动窗口的一角（一个左上、一个右下）时，包围盒会把中间**没变**的那一大片也
       算成损伤 —— 损伤盒子本身做小之后，这个代价反而成了主要开销（实测就是它让
       test-blursmall 红着）。现在存**4 个盒子**（`struct vidow_dmg_set`），装不下了才合并到
       「并进去之后总面积最小」的那个上

     **证据**（headless 2560x1440@2，壁纸 + 半透明浮窗 A + 跟它重叠的浮窗 B，B 每帧只让
     窗口里 80x40 那一小块失效）：

     | | A 的私有底衬重建 | 渲染帧数 |
     | --- | --- | --- |
     | 那一小块在 A 的区域**外**：改前（按整块窗口算损伤） | **155 次**（1 次/帧） | 154 |
     | 同上，改后 | **0 次** | 157 |
     | 同一小块挪到 A 的区域**里**（改后，该重抓） | **167 次** | 186 |

     - 新增 `./test-blursmall.sh`（6 项）：前提（A 真的建过私有底衬）、B 真的只动了一小块
       （截图变化像素全在它窗口框里）、区域外 0 次重抓、区域里确实重抓、vidow 活着
     - 把损伤判据退回「整块窗口」跑同一条：**155 次 → 当场红** ✓
     - 回归全过（50 条，含新的 test-blursmall.sh）
     - 145 那节列的方向里，第 1 条（损伤精确到表面的一小块）到此做完

145. **「3 也做」：按损伤区域作废 —— 屏幕另一头在重画，不该让别处的模糊底衬重抓**。

     **先把 ③ 的字面做法算了账**（原本说的是「一帧只画一遍世界 + 一条链，所有模糊元素
     共享」）：144 的 ① 之后这笔账不成立了 ——
     - 链条共享不了：每个元素的底衬是「世界减掉它自己」，源各不相同；
     - 场景重画在 ① 之后已经是**按区域裁着画**的（区域 = 窗口 ± 模糊半径），所以「一帧只
       画一遍世界」（整屏）只有在「各元素区域面积之和 > 屏幕面积」时才更省 —— 两个大窗口
       刚好在临界附近，窗口小反而更亏；
     - 它还会绕开「世界没变就不重画」这个缓存（每帧都要画一遍世界），静止屏幕上白花。
     真正值钱的那部分是 **KWin 那条路：按损伤区域作废**（它给每个窗口持常驻 FBO，只在窗口
     受损的区域里重渲染）。vidow 之前是「世界代次一涨，所有私有底衬全部重抓」—— 屏幕另一
     头有个视频 / 浏览器在交帧，别处的模糊底衬就跟着每帧重抓。这一轮做的是这个。

     **做法**（`src/fx.c` / `src/server.h`）：
     - `struct vidow_fx_target` 多一个 `pending_damage`（**布局逻辑坐标**的盒子）：从上次
       重建到现在、落在「这张底衬管得着的那块区域」里的损伤。攒着不清，直到真的重抓一次
       —— 屏幕另一头的损伤攒再多也不会让它重抓；而一旦有一笔碰到它，攒下来的账仍然算数
     - 世界代次只当粗筛：`touched = 代次变了 && 损伤碰到我这块区域`。区域本身已经把模糊
       半径算进去了（见 view_region_rect），碰到区域就是碰到能影响它的内容
     - 记账入口四个：`vidow_backdrop_damage_view()`（窗口自己变了：记它这一块、**跳过它
       自己** —— 它自己的变化不该让它自己的底衬过期）、`vidow_backdrop_damage_box()`（只记
       一笔区域：窗口挪走之后**老位置**也得算损伤，不然别的窗口底衬里会留着它的残影）、
       `vidow_backdrop_damage_region()`（世界代次 + 就是这一块：关窗动画）、
       `vidow_backdrop_damage()`（不知道是哪一块：整屏都算 —— 配置 / 输出 / layer /
       overview / 重排走这条，宁可多抓一次，不能糊着旧的）
     - 挪动类的记账都带**新旧两块**：place_view、拖拽、移动补间、开窗动画，果冻按弯出去的
       范围（max_off）放大一圈
     - 新增测试客户端 `tools/spin.py`：每 16ms 重画一帧、内容每帧都不同（截图比对看得出它在动）

     **证据**（headless 2560x1440@2，壁纸 + 两个互相重叠的静止浮窗 + 屏幕另一头一个不停
     重画的窗口，量 2.4 秒）：

     | | 重叠窗口的私有底衬重建 | 渲染帧数 |
     | --- | --- | --- |
     | 改前（世界代次一涨就全重抓） | **296 次**（2 次/帧） | 148 |
     | 改后 | **0 次** | 157 |

     - 新增 `./test-blurlive.sh`（4 项）；把损伤区域那条判据退回「只看世界代次」跑同一条：
       **296 次 → 当场红** ✓
     - 挪进 / 挪出模糊窗口底衬的截图逐像素比对（挪进去之前 / 压在上面 / 再挪出去，三个状态
       各一张）：改前 vs 改后 **max diff 0、差 >2 的像素 0** —— 损伤区域没记漏（漏了就是
       模糊底衬里留着残影，这里会当场差出来）；挪进去前后差 77.7 万像素，说明模糊确实跟着
       重抓了
     - 回归全过（43 条，含新的 test-blurlive.sh）

     **还能往下做的（按收益排）**：
     1. 损伤再精确一点：现在一个窗口重画（光标闪烁、小动画）就算**整块窗口**是损伤。wlroots
        手里有表面的 damage 区域，用它可以让「窗口里一小块在动」只作废对应的一小块底衬
     2. 链条也按损伤只跑一部分：现在是整块区域重跑 7 趟，可以只对损伤那一小块重跑、其余从
        上一版拷过来（KWin 就是在 FBO 里只重渲染受损区域）
     3. ③ 的字面版（一帧只画一遍世界）：只在「区域面积之和 > 屏幕面积」（两三个大窗口）时
        才赚，要重排整条渲染顺序（装饰渲染和世界渲染交织在一起），改动大、收益小
     4. 全屏直接扫描出（direct scanout）：全屏一个窗口且不用模糊时走 overlay plane，省掉
        整个合成 —— 收益大，但和模糊无关

144. **拖拽卡顿剩下那两笔账：私有底衬按区域抓 + 「只有我自己动了」不算世界变了**（用户说
     「1 2 可做」，指上一节 143 列的那两件；顺手查出一件更值钱的事）。

     **先查出来的那件：拖动时每一步都在给每个窗口重发 configure。** `vidow_place_view()`
     每次都调 `vidow_view_set_size()`（→ `wlr_xdg_toplevel_set_size`）和
     `vidow_view_set_tiled()`，两个都是无条件发的；而拖动时每动一下都会 `vidow_arrange()`
     一次。客户端收到 configure 就重画 + 交一帧 —— 实测（headless 2560x1440@2，壁纸 + 两个
     浮动窗口，200 步拖拽）：**每个窗口 200 次 configure、200 次 commit，两个窗口一共 400
     次**。也就是说「世界」每帧都在变：所有私有底衬每帧重抓，客户端也在陪着白画。
     修法：`vidow_view_set_size()` / `vidow_view_set_tiled()` 记下最后一次**真的发出去**的
     状态，一样就不再发（X11 的移动就是 configure，所以位置一起记）。拖拽期间客户端 commit
     **400 → 0 次**。

     **① 私有底衬只抓窗口那一小块**（`view_region_build()`，替掉 `blur_own_rebuild()`）：
     - 以前：每次重建都**整屏重画一遍**（壁纸 + background / bottom 层 + 所有窗口），再跑
       一整条整屏链条
     - 现在：基座从共享底衬上 **1:1 拷一块**（区域面积），窗口只画进这块区域，链条也只跑
       这块区域；装饰着色器按区域采样（复用 layer 那条路已有的 `blur_tex_x/y/w/h` +
       `u_origin` / `u_output_size`，观感逐像素不变）
     - 区域原点对齐到 **128 网格**、尺寸**只跟窗口大小有关**（拖窗口时区域整体平移，缓冲
       尺寸不变 → 金字塔缓冲一直复用、贴图不重导入）；原点夹在屏内（屏外没像素可抓，伸
       出去那圈会被链条混成黑边）
     - 区域 = 窗口装饰矩形 ± 模糊半径，半径按链条实际采样范围算：
       `blur_offset * (3 * 2^levels - 5)`（降采样那几趟 + 上升那几趟）
     - 顺带修掉一个隐患：以前重叠窗口的整屏抓取会把**共享底衬**那块缓冲覆盖掉（而 layer
       的区域底衬正拿它当基座），现在两者分开，基座永远是「壁纸 + background / bottom 层」

     **② 「世界变了」拆成「我自己动了」/「别人动了」**：
     - 新增 `vidow_backdrop_damage_view(view)` + `view->self_gen`：窗口自己的变化（被拖、
       内容重画、开合动画、果冻形变、布局挪它、显隐翻转、置顶、映射 / 销毁）既推进世界
       代次，也在它自己头上记一笔；私有底衬的缓存键从 `gen_scene` 改成
       **`gen_scene - view->self_gen`** —— 抓的时候这个窗口本来就藏着，它自己的变化不该
       让那张图过期
     - `vidow_arrange()` 不再无条件推世界代次：拖动时每动一下都会重摆一次，这种「只有被拖
       的那个在动」的重摆不该算「世界变了」。判断办法是让每一处真的变了的地方自己报账
       （`place_view` 的 moved / resized、可见性翻转、置顶），只要有一个不是被拖的那个就
       退回「世界变了」（`server->arrange_self_only`）—— 宁可多抓一次，不能糊着旧的

     **证据**（headless 2560x1440@2，壁纸 + 两个**静止**浮动窗口，200 步拖拽，同一脚本）：

     | | 拖拽期间私有底衬重建 | 客户端 commit |
     | --- | --- | --- |
     | 改前（1ead9df） | **446 次（2.23 次/步）**：被拖的 223 + 另一个 223 | 400 |
     | 改后 | **104 次（0.52 次/步）**：另一个 101（必须：它底下真的在变）+ 被拖的 **3**（只在区域跨网格时） | **0** |

     再加上每次重建的填充面积从整屏 3.69M px（还要重画一遍壁纸）降到区域 2.16M px + 一块
     1:1 拷贝 —— 窗口越小省得越多。
     - 截图逐像素比对（同一场景改前 / 改后各截一张）：**max diff 2、差 >2 的像素 0 / 3686400**
       （同一版本跑两次是完全相同的 0 差，所以这个对比是有意义的）
     - `test-blurcache.sh` 多了一条判据：拖拽期间重建次数 / 指针步数 < 1.5（步长用 20ms，
       让每一步都渲染出一帧，不然帧数比步数少、比值被稀释 —— 4ms 步长下修前只有 0.79）。
       把源码退回 1ead9df、只保留新测试跑同一条：**1.96 次/步 → 当场红**；现在 1.01 ✓
     - 回归全过（42 条）：blurscale / blurlevel / layerblur / layermask / layerghost /
       layerwin / layerdestroy / geometry / jelly / drag / ipc / dpi / dmabuf-update /
       viewport-crop / blurcache / openanim / closeanim / minimize / workspace / ws-dynamic /
       overview / scrollcull / frametrace / force-render / hotplug / outputpos / outputstate /
       lastoutput / cursor-shake / bounce / focus / focusclose / gap / input-inject /
       capture-hide / actsurface / config / defaultconfig / preinit / binds / keys / clipboard
     - `test-jelly.sh` 里按日志找「own rebuild」那条判据跟着日志改名成「own region」

     **③ 还没做（用户问「3 是什么意思」）**：现在是**每个**需要模糊的元素各自抓一份底衬
     （各自重画一遍世界）+ 各自跑一条 kawase 链 —— 有几个模糊元素就几遍场景渲染。③ 是把
     它换成 niri 那种「按画家顺序来」：**一帧只画一遍世界**（画进一张离屏图，画到谁就用
     谁底下那一块算模糊），场景重画从 N 遍变 1 遍。窗口在背景里这件事不会丢 —— 恰恰相反，
     「谁底下已经画好了什么」天然就在那张图里，所以它永远实时、跟着窗口动；链条还是每个
     元素一条（源不一样，共享不了）。纯性能改动，不动观感；视频 / 每帧都在重画的客户端
     （浏览器）背后的模糊要一直实时，靠的就是这条路。

143. **「有背景的时候拖拽还是会卡」**（用户报的「可能是果冻+模糊共同造成的？还是渲染效率
     比较差（我 7900XT 理应性能足够）」，并且「现在没条件测试，源码里面应该有办法知道是什么
     卡」）。

     **先说结论：不是渲染吞吐不够，是拖「与别的窗口重叠」的窗口时，每帧在做一件不该做
     的事 —— 把模糊金字塔的每一级缓冲反复导入成贴图再销毁。** 一个 7900XT 上 4K 一遍模糊
     是白送的；每帧 23 次 EGLImage 创建 + BO 映射/解映射不是。

     **怎么查出来的**（全部 headless，一台机器就能量）：给帧循环加了个 `VIDOW_RENDER_STATS=1`
     开关（每 500ms 一行：各阶段耗时 + 私有底衬重建次数 + **vidow 自己那些目标缓冲的贴图
     导入次数** + 帧率），再用输入注入脚本真拖一个浮动窗口（`pointer-motion` → `pointer-button
     press` → 20 步 × 30ms → `release`），四种组合各跑一遍（3840x2160@2，壁纸 + 两个浮动窗口）：

     | 场景（拖一个浮动窗口） | 私有底衬重建/帧 | 贴图导入/帧 | 模糊 CPU | 帧率 |
     | --- | --- | --- | --- | --- |
     | 两个窗口**不重叠** | **0** | **0** | 0 ms | 62 fps |
     | 重叠（无模糊） | 0 | 0 | 0 ms | 62 fps |
     | 重叠 + 模糊（无果冻） | 1.5 | 16 | 0.58 ms | 66 fps |
     | 重叠 + 模糊 + 果冻 | 2.0 | **23** | 0.77 ms | 59 fps |

     即：**不重叠时模糊一次都不重算**（共享金字塔缓存命中了，见 README 上面那节）；一重叠，
     每个重叠窗口每帧都要重抓一份「把它自己藏起来」的底衬 + 跑一遍 kawase 链（各自都是
     **整个屏幕**的场景重绘，不是窗口那一小块），而链条的每一趟都在
     `wlr_texture_from_buffer()` 导入、用完 `wlr_texture_destroy()`：真后端上（GBM/dmabuf）
     一次 import = EGLImage 创建 + BO 映射，一次 destroy = 解映射 —— 一帧二十几次，还是
     在指针每动一下都要走的路上。果冻本身便宜（一次 mesh 绘制，0.08 ms/帧），**不是它的锅**。

     **KDE / niri 是怎么做的**（用户让参考 KDE）：KWin 的模糊（`BlurEffect`）为每个窗口
     持有**常驻的 GLTexture/FBO**，只在窗口受损时渲染进去，合成时用 KPipeline 采样，**全程
     不重新导入自己的目标**；「窗口后面的模糊」是直接拿当前帧已经合成好的 framebuffer 内容
     来做（`blitFromFramebuffer`），不是再渲染一遍场景。niri 更彻底：**每帧只抓一次**底衬到
     一张 atlas，整张跑一遍链条，所有需要模糊的元素都从那上面采 —— 一个屏幕一帧只有一套
     抓取 + 一条链，和有几个模糊元素无关。vidow 现在是「每个元素一套抓取 + 一条链」，这是
     下面还要做的那两件事。

     **这一轮做了什么**（`src/fx.c` / `src/server.h`，行为逐像素不变）：
     - 金字塔的每块缓冲配一张**常驻贴图**（`vidow_blur_set` 的 `down_tex[]`/`up_tex[]`），
       建缓冲时一起建、收缓冲时一起收；链条直接换指针，不再每趟导入/销毁
     - 输出底衬的贴图（`backdrop_tex`）跟着底衬缓冲走（建一次，不再每次重建都导入）；
       layer 区域底衬（`ls->bd_tex`）同办，收口统一到 `layer_bd_drop()`
     - `VIDOW_RENDER_STATS=1` 作为**常驻**诊断留在源码里（真机上跑一遍就能看见每帧干了
       多少活、导入了多少贴图）

     **证据**：
     - 基准（同一条拖拽）贴图导入 **每帧 23 次 → 0 次**（30 帧窗口：+690/-690 → +0/-0）
     - 模糊金字塔 dump 逐字节比对：`VIDOW_BLUR_DUMP` 出 `vidow-blur-*.raw`（半分辨率最终
       结果，装饰采样的就是它）在「改前 / 改后 / 改后再跑一遍」三次里 **sha256 完全相同**；
       `vidow-bd-*`（底衬）在**两次同版本**运行之间就有 0.06% 像素差（拾取时机不同），
       说明差异来自运行噪声、不是这次改动（对照实验做了）
     - 新增 `./test-blurcache.sh`（5 项）：拖重叠窗口时私有底衬重建 ≥10 次（前提复现）、
       贴图导入 **0 次**、vidow 活着；把金字塔复用那行关掉（`if (0 && …)`）重跑：导入 518
       次 → 当场红 ✓
     - 回归：`test-blurscale` / `test-blurlevel` / `test-layerblur` / `test-layermask` /
       `test-layerghost` / `test-layerwin` / `test-layerdestroy` / `test-geometry` /
       `test-jelly` / `test-drag` 全过

     **还没做的（按收益排，都需要真机数据）**（1 和 2 已经做了，见上面的 144；3 还没做）：
     1. 私有底衬的那次场景重绘只画窗口那一块（+ 模糊半径），别整屏重绘 —— 机制已经有了
        （layer 那条路就是按区域抓的，`backdrop_ctx` 里有 `clip_x/y/w/h`）
     2. 拖自己被拖的那个窗口时，它的私有底衬**本来没变**（抓的时候把自己藏起来了），
        现在却被 `gen_scene` 一涨就重算 —— 一帧 2 次重建里有 1 次是白做的（要把「世界
        变了」拆成「我自己动了」和「别人动了」）
     3. 学 niri：一帧只抓一次、一条链，所有模糊元素共享（上限最高，改动也最大）

142. **chromium 系窗口「框到位了、里面小一圈、右边/下边一条黑边」**（用户报的
     「chromium 系的应用 bug 截图在 /tmp/chromium.png」，并怀疑「和 KDE 那个一样，也是
     没更新渲染」）。

     根因不是「没更新」：装饰管线换算「源缓冲里哪一块是窗口」那一步，拿**逻辑尺寸**的
     比例去采**客户端缓冲像素**，只有「缓冲 = 逻辑尺寸 × 整数倍、而且整块都有效」时才
     等价。chromium 在分数缩放下会把缓冲开得比窗口大一圈、再用 `wp_viewport` 的 src 框
     声明「哪一块有效」——headless 一块 3840x2160@2 上实测：窗口 954x1068、缓冲
     2304x2560、`viewport src = 0,0 1908x2136`（它先按更大的尺寸把缓冲开好，窗口收小
     之后接着用那块缓冲），有效区只占缓冲宽度的 **82.8%**。按整块缓冲采，那圈没画过的
     像素（黑 / 透明）就一起铺进窗口：屏幕上「蓝色边框 949x1068、画面只占左上
     802x901、其余纯黑」—— 和用户截图量出来的 84.9% 铺满比例逐项对上。wlroots 自己的
     场景节点手里就是对的：`wlr_scene_buffer.src_box`（缓冲像素，实测 `0,0 1908x2136`）。

     修法：`vidow_view_fx_geo()` 多算一组归一化的贴图坐标 `tex_u0/tex_v0/tex_du/tex_dv`
     （按场景节点的 `src_box` + 表面逻辑尺寸换算；声明的框超出有效区时，把手里那块铺满
     窗口，别去采边界外面），装饰着色器用它们（`fx.c` 的 `u_tex_map`）；`off_x/w/src_w`
     那几个整数量继续给布局、命中、关闭快照用。底衬（模糊的源）那条路取贴图时也带上
     `src_box`。X11、layer surface、自己拼几何的关闭快照没有这四个值 → 退回老公式，
     逐像素和以前一样。

     证据：headless `3840x2160@2` + chromium 截图量出来 —— 修前白色内容
     `x 16..1591 / y 16..1793`（= 缓冲有效区 82.8% 那个位置），修后 `x 16..1915 /
     y 16..2143`，铺满 954x1068 的框（边框在 12..15 / 1916..1919）✓。

     回归：`./test-viewport-crop.sh`（5 项，headless `scale=2` + chromium，没装 chromium
     就跳过）——①真的复现了「缓冲比窗口大一圈」的窗口（没复现就 SKIP，不当成通过）
     ②装饰采的是有效区域（`vidowmsg windows` 新加的 `surface.tex_du`/`tex_dv` 落在
     0..1 内）③窗口中心有内容、右边缘 / 下边缘不是黑边（修前实测 `0.0 / 0.0`，修后
     `248 / 245`，中心 `249`）④vidow 全程活着。把 `fx.c` 里用 `tex_*` 那一行停掉
     （装作没有这个修复）重跑：②仍过、③红 ✓。

     顺带：`vidowmsg windows` 的 `surface` 对象多了 `tex_u0/tex_v0/tex_du/tex_dv` 和
     `frame_width/frame_height`（装饰真正画出来的那块框 —— 客户端不接受 configure 时会
     比布局槽位大），排查这类「框和画面不一般大」不用再靠截图猜。

141. **摇晃放大太容易触发**（用户报的「光标放大太容易触发了，应该来回晃动过一会儿才放大」）。

     老门槛是「800ms 窗口里 3 次换向、每段 ≥12 逻辑像素」：正常用鼠标定位时随手来回
     甩两下就够（一次几十毫秒的摆动就凑齐 3 次换向）。

     改成「**来回晃一会儿**」：窗口内 ≥5 次换向、每段 ≥16px、**而且**这一串摇晃从第一次
     换向起跨了 ≥250ms；再加一条「两次换向间隔 >300ms 就算这一串断了」—— 顺手甩两下
     （换向挤在几十毫秒里）不算，甩完隔一会儿再动一下也不和甩的那两下拼成一次摇晃。

     `./test-cursor-shake.sh` 新增第 2b 段（2 项）：同样 6 段、5 次换向，但挤在几十毫秒里
     —— 不许放大、光标图尺寸不变（另加日志一行「摇晃: N 次换向、跨了 Xms，触发放大」，
     方便调门槛）。把修复回退掉跑一遍：2b 两项全红（甩一下就变 20x32）✓。测试里的
     `shake()` 也跟着改成「6 段 + 段间 70ms」。

140. **光标放大在焦点切换时被重置**（用户报的「光标放大在焦点切换的时候会直接重置」）。
     摇晃放大期间客户端一 `set_cursor`（焦点切到别的窗口，新窗口的客户端就会设自己的
     光标），老代码 `vidow_cursor_fx_set_surface()` 直接把放大整个收掉 —— 手上还攥着
     一个放大的光标，一切焦点就缩回原尺寸。

     改成：放大中**不收摊**，只记下「真实光标该是什么」；新图设上去之后紧接着调
     `vidow_cursor_fx_cursor_changed()`，把新那张重新复制 + 放大一遍（倍数、保持时间
     照旧）。这里有个坑：`wl_cursor_set_surface` 之后 wlroots 的 output cursor **当场
     还没有 texture**（要等下一次渲染才建），所以第一次探不到就挂个 16ms 的重试
     （`refresh_retry`，最多 8 次）；保持期里也把定时器夹到 16ms，不然换图要等七百毫秒
     才跟着换。指针从「客户端设的光标」移到没窗口的地方也走同一条路（先设回主题箭头、
     再重新放大）。

     `./test-cursor-shake.sh` 新增 3 项（第 9b 段，两个自己设光标的客户端）：切过去之后
     `magnified` 还是 true、放大**目标**倍数没变、日志里有「光标图换了，放大图跟着换」。
     把修复回退掉跑一遍：三项全红（magnified 变 false、倍数掉回 1、没有那条日志）✓。

139. **窗口几何比表面还大的那一两秒：内容被挤到左上角、右边一条糊边**（用户报的
     「窗口会先渲染小一点然后才按照缩放显示」，追问后确认是 chromium 系：**窗口框已经
     到位，只有里面的画面小一圈、边上留空**，持续 1–2 秒）。

     wlroots 只在客户端 `set_window_geometry` **那一刻**把声明框夹进当时的表面；之后
     表面缩小（chromium 改大小的那 1–2 秒：表面已经换成新尺寸、声明的几何还是旧的
     大尺寸）它不管。vidow 的装饰管线照旧按声明框采内容，`uv = u_tex_map.xy + cuv *
     u_tex_map.zw` 里的 `zw = 声明/表面 > 1` → UV 跑到 1 外面被 `clamp` → 内容只占
     窗口左上角一块、右边/下边一条**边缘像素拉出来的糊边**；窗口框（装饰缓冲）却是按
     声明框来的，所以看着就是「框到位了、画面小一圈」。

     修法（`src/view.c` 的 `vidow_view_fx_geo()`）：声明框落在表面外面时，当**整块表面
     就是窗口内容**（`off = 0`、`src_w/h = 声明宽高` → 采样映射成 1:1），内容铺满这块框，
     等客户端把新尺寸画上来自然恢复 1:1。窗口框的大小不变（还是声明尺寸）。

     新增 `./test-geometry.sh`（6 项）+ 靶子 `tools/geowin.c`（左红中绿右蓝 + 正中间一条
     白线，`--shrink=WxH` 复现「表面后缩、几何不动」）：判据是**装饰缓冲 dump 里白线的
     相对位置**（修好 0.500 / 老行为 0.333），另有三条护栏（框留在声明尺寸、哨兵帧、
     没有 assert）。回退跑一遍：两条红 ✓。

138. **拖着透明窗口时，模糊底衬里混进了这个窗口自己上一帧的形变图**（用户报的「拖拽的
     时候透明窗口背景会有奇怪的错位竖线」「浮动窗口的背景模糊会变得不模糊」），外加
     **拖拽时每帧重新分配十来块 GBM 缓冲**（同一份报告里的「有背景的时候卡顿严重」）。

     ①抓底衬时只藏了**装饰节点**（`view_set_capture_visible`），而拖着果冻的时候窗口
     内容是由**果冻节点**画的（装饰节点透明度是 0）—— 那个节点带着形变留在抓出来的底衬
     里，于是「窗口自己的上一帧」变成了自己的背景：错位、重影，模糊看着也像没了。
     修法：`vidow_jelly_capture_visible()`（jelly.c）跟着一起藏。

     ②重叠的窗口一拖起来，**每帧**都要重建两个金字塔（另一个窗口在它的底衬里动），而
     `blur_set_alloc()` 每次都先 `drop` 再分配：`vidow_alloc_target()` 走的是
     `wlr_allocator_create_buffer()` —— 真后端上每块都是内核层的 GBM 分配。实测一次拖拽
     （1280x720，两个重叠窗口，168 次重建）：**修之前新分配 1082 块缓冲，修之后 18 块**。
     修法：形状（宽高 + 级数）没变就复用那几块（链条每趟都会整块重写，调用方重画前已经
     把攥着 `up[0]` 的贴图放掉了）。

     `./test-jelly.sh` 新增一段（第 11 节）+ `tools/sheet.py`（半透明纯色薄纱）：故意**不
     放壁纸**、两个重叠窗口（底下品红不透明、上面半透明绿），拖着绿的抓底衬，每份
     `VIDOW_BLUR_DUMP` 的 dump 只许有一种颜色：修好「混进自己的 0 份」，回退跑一遍
     「32 份同时有自己和别的窗口」✓。

137. **果冻形变的几何：窗口像好几条矩形拼出来的、左边缘一段一段跳**（用户报的
     「拖拽的时候窗口的几何像是好几条矩形拼起来，锯齿感严重」）。

     老实现一格一块**轴对齐矩形**（`wlr_render_pass` 只能贴轴对齐的矩形），剪切形变下
     相邻行各自取整、再把每格往外撑 1px 补缝 —— 屏幕上就是左边缘一段一段跳（实测每
     38px 跳 2–3px，一个窗口十来处）、行边界一条错位竖线，透明窗口那层模糊透过缝看
     尤其显眼。

     修法：加一个**网格着色器**（`src/fx.c` 的 mesh program + `vidow_fx_draw_mesh()`），
     顶点直接给「渲染目标物理像素坐标 + 源贴图归一化 UV」，每格两个三角形、相邻格共用
     同一对顶点，节点坐标**不取整**（小数直接进光栅器）。`src/jelly.c` 只负责把网格顶点
     算出来（`jelly->verts`）。

     `./test-jelly.sh` 新增一条判据：拖动中途按行量**左边缘轮廓**，逐行步进必须 ≤ 1px、
     不能有 ≥2px 的跳变。修好「步进最大 1px、跳变 0 处」，回退跑一遍「最大 3px、跳变
     10 处」✓。

136. **KDE 系（Qt/KDE）的窗口「不渲染新画面」**（用户报的：kate 停在最后一帧、dolphin
     内容不出来，**其他软件正常、包括普通 Qt**）。真机追下来是**装饰管线取客户端贴图的
     那一步**：

     `vidow_fx_frame()` 把窗口画进自己的离屏缓冲（边框/圆角/阴影/模糊），采样的是客
     户端的表面缓冲。原来那儿为了「拿到的贴图能 `destroy`」而绕开 wlroots 给客户端缓冲
     包的那层 `wlr_client_buffer`、用 `render_source()` 对 `client->source` **另建**一张
     （注释写着「在 wlr_client_buffer 上会拿到缓存好的贴图，不能销毁」）。问题就在这张
     另建的贴图上：对 **dmabuf 客户端**（Qt/KDE：每帧重画同一个 dmabuf、只 `commit` 不
     换缓冲）它一直是**第一次导入的那版内容** —— 客户端在画、装饰缓冲也在重烘，里面却
     永远是旧画面，屏幕上就是「窗口停在最后一帧」；拖到另一块屏会重新烘一次背衬，画面
     才跟上（用户描述的正是这个）。GTK（我这儿走 shm）看不出来。

     证据链（全在 headless 上复现）：
     ①kate：打字后标题变 `hotplug.c *`（文档真改了）而屏幕**0 像素变化**；同一套 vidow
     里 nautilus（GTK）打字后变了 24696 像素 —— 和用户「只有 KDE 系」一致；
     ②`VIDOW_FX_DUMP` 把装饰缓冲 dump 出来：打字之后重烘的那张里，编辑区还是**旧文本**
     （没有打进去的字、标签栏也没有修改标记）；
     ③把 `wlr_output_schedule_frame` 收口 + 每帧 commit 的 DEBUG 追踪（见 135）先证明
     「客户端确实在交帧、输出也在出帧」—— 于是范围就缩到「烘进去的是旧贴图」这一处。

     修法：统一走 `client_texture()`（`src/fx.c`）——**优先借 `wlr_client_buffer->texture`**
     （wlroots 画场景用的就是它，跟着每次 attach 走），借来的 `owned = false`、**不
     destroy**；借不到才自己建一张。窗口那条路、layer 背衬抓取那条路（同一份 trick）
     都改过来。

     新增 `./test-dmabuf-update.sh`（5 项，headless，没装 kate 就跳过）：拿 kate 当
     dmabuf 客户端，打字前后比**屏幕像素**（修之前 0、修之后 ~949）+ 比**装饰缓冲 dump**
     的前后两帧（把「谁没更新」钉在装饰管线那一层）+ vidow 全程活着 + 收尾干净。把
     `src/fx.c` 的修复回退掉跑一遍：两条像素断言都红（0 像素）✓。

     回归：`test-dmabuf-update.sh` 5/5、`test-layerblur.sh` 4/4、`test-layermask.sh` 3/3、
     `test-layerwin.sh` 4/4、`test-blurlevel.sh` 3/3、`test-jelly.sh` 28/28、
     `test-capture-hide.sh` 30/30、`test-openanim.sh` 5/5、`test-closeanim.sh` 13/13、
     `test-dpi.sh` 15/15、`test-frametrace.sh` 17/17、`test-preinit.sh` 22/22、
     `test-drag.sh` 11/11。

129. **配置按类别拆开：入口 `config.lua` + `configs/` 下一类一份**（`vidow.include()`）。
     原来那份「一份两三百行、什么都写在一起」的默认配置，改一个圆角要先滚过间距、模糊、
     动画、剪贴板……于是拆成一类一个文件，入口只剩 include 行。`vidow.include(path)`
     是「就地展开」：**相对路径按正在 include 的那个文件所在目录解析**（像 C 的
     `#include "…"`），include 的先后就是键生效的先后。实现上分两层是有原因的：include
     进来的那段可能调 `vidow.exec()`，而它是 yield 出来的（整份配置跑在 coroutine 里），
     从 C 里 `lua_call` 出去的调用不能 yield（"attempt to yield across a C-call
     boundary"）—— 所以 C 侧只留一个「把文件读成函数」的助手，调用交给一层薄薄的 Lua，
     报错再 `error(msg, 2)` 算回配置里写 include 的那一行。`configwatch` 跟着改成盯
     **「配置真正读了哪些文件」**（入口 + 每一份 include 的，读不成的也登记 —— 文件补上
     的那一刻要能发现）：改 `configs/` 下任何一份都自动重载，日志里点名是哪一份；
     重载会重新登记，从入口里删掉的 include 以后改了也不重载。命令行跟着变：
     `--default-config` 打入口、`--default-config all` 打全部拼一起（能当单文件配置用）、
     `--default-config 02-decor.lua` 打其中一份（也认 `decor`）、`--write-config` 写一整套
     （入口 + `configs/`，已有的不动，除非 `--force`）。仓库里的 `config.lua.example`
     换成 `config.example/` 一整套，`test-defaultconfig.sh` 逐份比对 + 文件清单比对
     （41 项）；新增 `./test-config-include.sh`（25 项）：路径解析（相对 / 绝对 / 嵌套）、
     顺序即优先级、include 里跑 `vidow.exec()`、改 / 删 / 补一份都自动重载、文件不在 /
     语法错 / 自己 include 自己都当场报错且只带走后面那些键。

128. **激活令牌攥着的 surface 先没了 → wlroots 断言 → SIGABRT**（用户的会话就这么被带走的：
     `vidow: types/wlr_compositor.c:736: surface_handle_resource_destroy: Assertion
     \`wl_list_empty(&surface->events.destroy.listener_list)' failed.`）。
     `src/activation.c` 里两处把监听器挂到客户端 `wlr_surface->events.destroy` 上
     （造令牌的 `build_handle_set_surface`、令牌本身的 `build_handle_commit`），回调
     `token_surface_gone()` / `build_surface_gone()` 只把 surface 清成 NULL、把「挂过监听」
     的标志清成 false，**没把自己从那条信号上摘下来**。wlroots 在 surface 资源销毁时是先
     `wl_signal_emit_mutable(&surface->events.destroy)` 再断言链表已空 —— 于是 emit 完
     我们的 link 还在，直接 abort。修法：两个回调里都 `wl_list_remove(&...->surface_destroy.link)`
     （在信号回调里摘自己那条 link 是 libwayland 明确支持的用法）。
     用户那边触发它的正是微信：启动时请求激活（令牌上 set_surface 指着自己的登录窗，
     日志里 `焦点: wm-b -> 微信` 就是它被兑现了），随后登录窗销毁 → 断言炸。
     新增 `./test-actsurface.sh`（8 项）：给 `tools/activate.c` 加了 `--drop-surface=<秒>`
     （用令牌请求激活之后把窗口表面销毁、客户端**不断开连接**）→ 断言合成器不崩、日志里
     没有那条断言、IPC 还通、之后还能开窗口、客户端断开也不崩。拿修之前的二进制跑，
     复现出的是**一模一样**的那行断言。

127. **屏没了（尤其是「最后一块也没了」）之后，列和列里的窗口留着野指针** —— 用户报的
     「xwayland 无法点击交互，但 CSD 区域可以拖拽（测试的是微信）」就是这条路。
     他们的 vidow 是嵌套跑的（WLR_BACKENDS=wayland）：把 vidow 那个窗口关掉 = 外层那块
     wl_output 没了 = vidow 唯一的屏没了。日志里只有一句
     `hotplug: 拔掉 WL-1（3 列 / 3 个窗口 / 0 个浮动窗口），先挪到 (没有屏了) 上住`，
     之后布局就是 0x0，XWayland 的 X 屏幕跟着变成 **0x0** —— X11 客户端看不见也点不动
     （X 屏幕多大 = 输出的并集，一块屏都没有时就是 0x0）。这不是输入路径的 bug：
     拿 xev 量过，健康的 vidow 里 X11 客户端在 1 倍 / 2 倍输出下都能收到点击、局部坐标
     也对（(布局-原点)×xwayland_scale）。
     真正的 bug 在 `ws_set_output_full()`（`src/workspace.c`）：
         if (output == NULL) return;                      ← 提前返回
         wl_list_for_each(col, &ws->columns, link) column_set_output(col, output);
     工作区「现在没有屏」的时候直接 return 了，于是列和列里的窗口的 output 继续指着那块
     **已经被释放**的 wlr_output：`vidowmsg windows` 的 output 字段是一串乱码
     （用户那边是 `p\xc4ş\x04V`，我们的复现里每次都不同 —— 典型的野指针），工作区列表
     里也一样，之后谁读 `view->output` / `col->output` 谁拿到垃圾。修法：先让列跟着
     工作区换屏（NULL 也要清），浮窗那段（要按新屏原点算平移）才 return。
     另外两处顺手补上：① overview 锁着的那块屏没了就把它收掉（不然下一帧在读已经释放的
     屏）② XWayland 起来时要是布局是 0x0，明确报一行「一块屏都没有 → X11 客户端看不见也
     点不动」—— 不然人只会去查输入路径。
     新增 `./test-lastoutput.sh`（15 项）：①两块屏各一个窗口，真 `wlr_output_destroy` 拔掉
     一块 → 挪过来的窗口 output 必须是活着的那个名字（不是乱码）、插回来再认领回去
     ②嵌套会话：外层窗口关掉（最后一块屏没了）→ 每个窗口 output 是 null、工作区 output 是
     null 但 home 还记得那块屏、记忆里 present=false、日志明说 X 屏幕 0x0，全程不崩。
     第②组的「output 是乱码」那条拿修之前的二进制跑必挂。

126. **关掉一个窗口之后，焦点落到「右边最近的那个」**（niri 的规矩：位置不动，谁滑到空出来
     的位置上就是谁）。原来只处理了一半：列里还有窗口时看**下面**那个（`column_link.next`），
     其余情况全丢给 `focus_nearest_column()` —— 而那个函数是「从左往右找第一个**有窗口的
     列**」，于是关掉中间那个窗口，焦点直接蹦到最左边那列去了（屏幕滚动位置也跟着跳）。
     修法：`focus_after_close()`（`src/view.c`）按三步挑人 —— ①列里还留着窗口：关掉那个
     的位置由它下面那个顶上；关的是最下面那个就往上找一个。②整列没了：右边最近的那一列
     顶上。③右边没有了才往左找最近的一列（关掉最右边那列时 niri 也这样）。要**在
     `view_leave_column()` 之前**算：列一摘空就自己 free 了，邻居得趁它还挂在工作区链表
     上才找得到（`view_unmap` / `view_destroy` / `x11_destroy_view` 三条路都改成先算人、
     再摘列 / free）。另外跨列找只在「它那块工作区正摆在这块屏上」时才做 —— 藏着的
     工作区里的窗口不该拿到键盘，那种情况照旧走兜底（挑块看得见的）。
     浮窗也要走这条规矩：浮窗不在列里、没有列序可看，但「往右」一样成立 ——
     `focus_nearest_by_x()` 按屏幕位置挑（x 比它大的里面取最小的，右边没有了取 x 最大的；
     平铺窗口归到它那列最上面那个）。另外平铺关完了但还剩浮窗时，焦点落到浮窗上，
     不是把键盘清空（niri 的「平铺关完落到浮窗上」）。
     新增 `./test-focusclose.sh`（21 项）：a|b|c 三列关中间的 b → 焦点是 c（不是最左的 a）；
     关最右边的 c → 焦点是左边的 a；列里叠两个（e 上 d 下）关下面的 d → 焦点往上顶到 e；
     浮窗 f1 夹在 a 和 f2 中间、关掉它 → 焦点是右边的 f2；平铺关完只剩浮窗 → 焦点落到
     浮窗上；关最后一个 → `focused.window` 变 null。关窗口的两条路都测到了（客户端自己退
     = unmap，客户端被 kill = destroy）。列序那两条和浮窗那两条都拿修之前的二进制跑过，
     都会挂。

125. **关闭动画的两条底线：看得见、不带窗口装饰**。
     ① 窗口「没有关闭动画」：默认的关闭曲线是 `out_elastic`（q 弹），它冲过 1（t≈0.26
     到 1、t≈0.45 冲到 1.09）—— 把这条曲线喂给「1 → close_alpha_to」那条透明度斜坡，
     透明度在动画头 1/4 就掉到 0 并夹住，剩下 3/4 只在动位移和缩放，屏幕上什么都没有。
     默认 140ms 时窗口实际只可见 35ms，看起来就是「关掉就没了」。这是上一轮加 q 弹时
     引进的（当时只把 alpha 夹在 0..1，没想到夹住就等于提前结束）。修法：动画回调同时
     拿到**曲线值**和**线性进度**（`vidow_anim` 的 update 多一个参数），位移/缩放用曲线，
     透明度用线性进度 —— q 弹还在，淡出铺满整段。实测（4000ms 的关闭、量窗口正中那个
     像素）：修之前 400ms 处 α≈0.25、1000ms 之后一路 0；修之后 400/1000/2000/3000ms
     是 0.94/0.73/0.37/0.02。
     ② 面板关闭动画「最后冒出一个比面板大一点的空心矩形」：layer 的快照被套上了**窗口**
     装饰（边框 + 阴影 + 圆角），而 layer 自己就是内容、形状由客户端画（开动画那边也没
     装饰）。淡出尾巴上面板那点半透明在模糊底衬上本来就没对比、先看不见了，边框和阴影
     压在没糊过的壁纸上反而更显眼 —— 边框环比面板自己亮 17，阴影圈还往面板外扩几个
     像素（修之前量到 -4.4）。修法：`vidow_closing_start()` 多一个 `decorate` 参数，layer
     那条路传 false，快照按原样淡出。
     新增 `./test-closeanim.sh`（12 项）：窗口部分量中心像素的 α（30% 处 > 0.5、50% 处
     > 0.25 且单调下降），layer 部分量面板外那几个像素（阴影圈）和面板最边上那几像素
     （边框环），另外断言动画结束后里外都是干净壁纸。拿两个回归版本跑过：透明度走曲线
     的那版挂在 α=0；layer 带装饰的那版挂在阴影圈 / 边框环。

124. **拔线后插回来必崩：给「已经不在了的那块屏」补空工作区**。`ws_set_output_full()` 换屏时
     会给老屏补一块空的（不然那块屏没东西可显示），但拔线那一路老屏**已经没了** ——
     `output_destroy()` 是先 `wl_list_remove(&output->link)` 再调 `vidow_view_output_gone()`，
     于是这一步在正在被释放的 wlr_output 上建了一块工作区（`ws->output` 指着马上要 free 的
     结构，`home` 还记着那块屏的名字）。屏插回来时 `vidow_workspace_output_added()` 按 home
     认领它，`old = ws->output` 已经是野指针，`vidow_workspace_ensure_bottom(old)` 里
     `workspace_new()` 读 `output->name` 就是 `0x60` → SIGSEGV（coredump 3062966，和 19:39
     那次同一个签名）。修法：补空工作区之前先确认那块屏还在（`output_present()` 扫
     `server->outputs`，只比指针不解引用）。`./test-hotplug.sh` 之前是「偶尔挂」，其实是这条
     野指针在拼运气（释放过的块里那个字由分配器决定），现在连跑 3 遍 24 项全过。

123. **layer surface 被销毁时的段错误（noctalia 重建面板必崩）**。垫在 layer 背后的那份
     模糊（`ls->fx.node`）是挂在 wlroots 自己的场景树 `ls->scene->tree` 里的，而
     `wlr_scene_layer_surface_v1_create()` **内部也**往 `layer_surface->events.destroy` 上挂了
     一个监听：销毁整棵树（连子树）、再释放 `scene_layer_surface` 结构本身。`wl_signal_emit`
     按挂的顺序回调，我们的 destroy 监听挂在它后面 —— 轮到时 `ls->scene` 和 `ls->fx.node` 都
     已经是释放过的内存，`vidow_layer_blur_release()` 拿着野指针调 `wlr_scene_node_destroy()`，
     后者先顺着 `node->parent` 往上找 scene，读到的正是 glibc tcache 的 key（coredump 2853800：
     `rax=0xaa7f5cd93dea7ef1`，非规范地址）→ SIGSEGV。崩不崩还得看运气：释放过的块里 parent
     那个字由分配器决定，所以表现是「用着用着崩」而不是「每次都崩」。修法：把 `ls->destroy` 的
     注册**挪到 `wlr_scene_layer_surface_v1_create()` 之前**（create 失败的路径要
     `wl_list_remove` 摘掉），我们就成了第一个回调 —— 先收自己的模糊节点，再让 wlroots 拆树。
     顺带加两行 DEBUG 日志（`layer closed` / `layer blur release`）。新增
     `./test-layerdestroy.sh`（9 项）：状态栏上屏 → 画模糊 → 退出，反复重建 5 轮，每轮都断言
     vidow 还活着、模糊节点被收掉，最后量 RSS 确认没漏缓冲；拿修之前的二进制跑，第 1 轮常常
     侥幸活下来、第 2 轮必崩（所以测试要跑 5 轮）。

122. **动态工作区的收编时机 + `./test-ws-dynamic.sh`**。`cleanup` 原来挂在 unmap 里，
     但那时候列还没摘掉 —— 工作区还不算空，于是「关掉窗口后那块空的、没名字的工作区」
     要等到下次切工作区才收。挪到 `view_leave_column()` 之后（unmap 和 destroy 两条路
     都挪），关掉窗口当场就收、序号跟着往前挪（实测：`1:-(1) 2:-(1) 3:-*` 关掉第 1 块
     的窗口 → `1:-(1) 2:-*`）。新增 `./test-ws-dynamic.sh`（9 项）：栈底空位什么时候冒
     出来、越界夹住、空的中段当场收掉、具名工作区空了也不消失且按声明顺序排栈顶。

121. **鼠标拖窗口：浮动跨屏改归属，平铺新增 Mod+左键拖拽**（niri 的 interactive move）。
     平铺拖拽只动场景节点（`anim_off` 跟着光标，`vidow_place_view` 在 `view->dragging`
     期间让位），布局一行都不重排；松手才结算：光标在哪块屏上就归那块屏**显示着**的
     工作区（跨屏走 `vidow_workspace_move_view`，整条列搬），并按落点在列带里的位置
     重排（列的位置是 arrange 算的 `offset`，从可用区左边算起，所以加回可用区原点才
     是屏幕坐标）。顺带修掉两个真 bug：① `vidow_float_grab_update()` 只改绝对盒、不刷
     `home_dx/home_dy`，下一次 arrange 的 `float_place_recursive()` 按旧偏移把它摆回去
     —— 浮动窗口**拖了等于没拖**（同文件 138 行的注释早就写明了这条规矩）；② 平铺转
     浮动时种子只给了 `float_geom` 没给 `home_dx/home_dy`，第一次摆放就被按 0 偏移覆盖
     成屏幕左上角（`float mode w1: 0 -> 1` 之后 `place w1 -> 0,0`）。跨屏落点那一步
     还要**先**按新屏刷新偏移再搬：搬那步会 arrange，用旧偏移配新屏的 box 会整块平移
     一个屏宽（实测落在 x=2490，本该 1210）。测试：新增 `./test-drag.sh`（11 项）——
     headless 里没有真鼠标，给 `VIDOW_TEST_ACTION` 加了 `pointer-motion x y` 和
     `pointer-button left|right|middle|数字 press|release [mod]` 两个动作，走的是和真实
     输入完全同一条路（`vidow_pointer_motion` / `vidow_pointer_button`；`mod` = 假装按着
     Mod）。`test-ipc.sh` 那条 `window-moved` 断言原来靠的是 ② 那个「跳角落」的 bug 才
     触发的，现在改成显式 `set-window-position` 挪一下。回归：21 个测试全过。

118. **装饰按「可见窗口」画（niri 的 `size()` / `buf_loc()`），新增 `prefer_no_csd`**。
     客户端用 `xdg_surface.set_window_geometry` 声明的那块才是窗口：装饰矩形 / 离屏缓冲
     尺寸 / 模糊区域 / 关闭动画快照全按它算，内容贴图按留白偏移采样（着色器加 `u_tex_map`，
     归一化所以和倍率无关；mask 剔除仍用元素局部坐标，layer 那一路走恒等映射）。
     以前装饰按整块表面画 —— GTK3 报 `表面 680x755 / 窗口 634x708 / 偏移 23,21` 的时候，
     边框画在表面边上、窗口往里缩 20px，中间那圈露屏幕背景，就是「边框和窗口有缝」。
     顺带修掉真凶之一：装饰节点的位置原来从场景坐标算（`sx - vx`），而 wlroots 要等 commit
     才把表面子节点挪到 `-geometry` —— map 那一瞬间算出 0，第一帧装饰摆在视图原点，**录屏
     里那条缝**就是这么来的（grim 之后重绘所以看不到）。现在只看 `xdg_surface.geometry`。
     新增 `prefer_no_csd`（默认 `false`）：打开后额外建 `org_kde_kwin_server_decoration_manager`
     并回 `Server` —— GTK3 只认这个老协议（libgdk-3 里没有 zxdg_decoration_manager_v1），
     实测同一个 GTK3 窗口从 `表面 680x755` 变成 `表面 634x708`（CSD 收回去，代价是连标题栏
     一起收）。`test-dpi.sh` 的 Wayland 断言改成按窗口几何算（取 `place` 的 w/h，不是 commit
     那行的表面尺寸）。回归：`test-gap.sh` / `test-defaultconfig.sh` / `test-openanim.sh` /
     `test-dpi.sh` / `test-layermask.sh`（这条抓到了 layer 那一路没走 `vidow_fx_begin`、
     恒等映射没设，mask 被采成 texel (0,0)）全过。

117. **配置面收敛：`gaps` 一个数管全部（外加单边 / 内间距覆盖），一族配置可以写成一张表；
     新增 `vidow --default-config`**。gap 原来是照 niri 抄的四个值（`workspace_gaps_inner/outer`
     + `window_gaps_inner/outer`）再叠一个 `gaps <n>` 简写：九个键说一件「间距」的事，而且
     `gaps = 10` 实际得到的是**离屏幕边 20px**（内外两份相加，跟 niri 的 `gaps` 语义也不符）。
     现在只剩 `gaps`（一个数管全部：屏幕边和窗口之间都用它）+ `gaps_top/bottom/left/right/inner`
     （`-1` = 跟随统一值，其它负值照旧当 0），`gaps = { top = 30, inner = 20 }` 和那两个平铺键
     完全等价。同一个机制铺到别的族：`decor` / `border` / `shadow` / `blur` / `anim` / `shader` /
     `clipboard` / `ipc` / `xwayland` / `portal` 都能写成一张表（表里只写要改的项，其余跟着默认
     值），平铺键照旧能用（环境变量 `VIDOW_*`、`vidowmsg set-config`、老配置都走平铺键）。表里
     项名拼错、给只认表的族（`border`/`shadow`/`anim`/`portal`）写一个光秃秃的值，都当场报错并
     列出能写哪些项；删掉的那四个老键名会喊一声「这个键没有了，现在用 xxx」——「配置写了没反应」
     最难查。`vidow --default-config` 打一份完整默认配置（每个键都写着自己的默认值），仓库里的
     `config.lua.example` 就是它的输出（129 之后换成 `config.example/` 一整套），
     `./test-defaultconfig.sh` 里有一条断言盯着这两者一致，
     所以文档不会跟代码走散（`shader_dir` 那种跟着配置目录走的默认值打的是注释，不把 HOME 写进去）。
     实测：默认布局不变（离屏幕边 6 / 窗口之间 6、列宽 634）；`gaps 16` 从「离屏幕边 32 + 列宽 608」
     变成「16 + 624」；`gaps 40` → 边距 40、列宽 600。`./test-gap.sh` 按新语义重写成 33 条断言，
     新增 `./test-defaultconfig.sh`（20 条：默认配置回灌后逐键等于内置默认值 / 表写法只盖写了的项 /
     报错信息 / 老键名提示 / 示例文件不漂移）。

## 尚未实现（原型边界）

1. 强制渲染的边界：
   - 规则的作用现在有五个（`force_render` / `hz` / `focus_on_open` / `focus_on_activate` /
     `honor_invalid_serial`），但没有 opacity / 位置 / 尺寸这类规则
   - 节流是固定频率（`force_render_hz` / 规则 `hz`），不会自动跟输出的刷新率
     （144Hz 屏上默认也只补 60Hz；想要更多就自己调高或者写 0 不限速）
   - 录屏 / 预览的「保活」靠窗口截图源的存在（portal 的窗口录制路径）；如果哪天有
     别的协议直接抓窗口而不建截图源，那条路不会自动保活
   - 窗口在视口外时客户端照常出帧 = 照常耗电（这正是它想要的效果，但要有意识地开）
2. 工作区的边界：
   - 工作区是**全局**的一叠（不是「每块屏自己做一套编号」）：切过去的那块屏就是它现在
     住的地方，跨屏切换会把整块工作区搬过去
   - 切工作区没有动效（也没有窗口「飞向新位置」的连续布局动画），切回来是瞬时的
   - 工作区本身不持久化（重启后只剩 `config.lua` 里声明的那几块，窗口不会自动回去 ——
     和热插拔记忆一样是进程内的）
   - 把窗口拖到别的工作区只支持动作/键位（`move`），没有鼠标拖拽手势
   - 特殊工作区同一时间只显示一块；每屏各一块它也不做（跟随聚焦的屏）
3. 焦点 / 激活的边界：
   - 令牌里 serial 不对（客户端写了 `set_serial(0, seat)`，或者拿的是别的客户端的
     serial）现在能治了：窗口规则 `honor_invalid_serial = true`（按应用）或者全局
     `honor_xdg_activation_with_invalid_serial = 1`（所有应用）。默认都不开，跟
     niri / wlroots 一样。**代价**是这个校验本来是防随便抢焦点的 —— 开了之后拿到令牌串
     的客户端就能把你正在打字的窗口顶下去，所以默认关、按应用开
   - 令牌本身还是「谁拿到串谁就能用」：我们只校验 serial，不校验「请求方是不是建令牌的
     那个人」（协议就是这么设计的：令牌本来就要跨进程传）
   - `focus_on_open = false` 的窗口，如果这块屏上**一个焦点窗口都没有**，还是会拿到焦点
     （不然没人收键盘）；「绝对不聚焦」这个语义没做
   - 没有「新窗口一律不抢焦点」的全局开关（只有规则里的 `focus_on_open` / `focus_on_activate`；
     「焦点跟随鼠标」`focus-follows-mouse` 与「键盘换焦点把光标带过去」`warp-mouse-to-focus`
     已经实现，见上面的编号 256）
     （要的话写一条不带匹配字段的规则：`vidow.rule{ focus_on_open = false }`，它会匹配所有
     窗口 —— 但连你自己的终端也会不抢，谨慎）
   - 激活请求只在「窗口已经存在」时能兑现：应用还没启动 / 窗口还没建出来时点通知，
     我们不负责把它拉起来（那需要 xdg-activation 的启动令牌 + 会话恢复）
2. XWayland 的边界（真实 X11 程序能跑，但下面这些没做）：
   - 没有 `_NET_WM_STRUT` 面板预留：X11 面板不会把可用区挤出来（最大化 / 最小化已经在做，
     见 changelog 177 与「列最大化」「最小化」两节：X11 客户端请求最大化 -> 它那条列最大化 +
     写 `_NET_WM_STATE_MAXIMIZED_VERT`/`HORZ`）
   - X11 弹出物（菜单 / 提示 / 通知）不装饰、不做动画、不进布局、不抢焦点，按客户端
     给的绝对位置显示，层级在所有托管窗口之上（和 Wayland 的 xdg_popup 一致）；
     `_MOTIF_WM_HINTS` 故意不听，见上文
   - 客户端按「屏幕边界」夹弹出菜单时可能偏：X 屏幕（客户端看到的尺寸）是**逻辑**尺寸，
     而我们告诉窗口的坐标是 `逻辑 × S`。窗口内部的相对定位是对的（用命中坐标验过），
     但按屏幕边缘做裁剪的客户端会算错
   - 分数缩放下 GTK3 不认 fractional-scale，会按 `ceil` 渲染再被我们缩小（仍然清晰，
     只是多花像素）；GTK4 / Qt6 认 fractional-scale，按 1.70 精确渲染
   - Xft 系（GTK2、老 Motif）不认 `GDK_SCALE`：保持 1:1（清晰但相对偏小）。`Xft.dpi`
     vidow 已经写了（= 96 × S），GTK2 这类会按它放大字号；要自己接管就在 config.lua 里
     跑 `xrdb -merge` —— 但别和 `GDK_SCALE` 一起用，会叠成 4 倍字号
   - 没接 `wlr_xwayland_surface_offer_focus`（ICCCM 的 GLOBAL 输入模型），只发 activate
3. 热插拔的边界：
   - 记忆在进程里（`vidow_output_memories`），**重启不保留**；要「重启之后还把应用放回
     原来的屏」得自己做会话保存/恢复（记住 app_id + 列序，重启后按它拉起/摆放）
   - 只按连接器名认身份，不看 EDID serial：同一个口换一块显示器也当同一块屏（要按屏认
     人得把 `wlr_output.serial` 也纳入身份）
   - 插回来落在哪个位置由 wlroots 的 `add_auto` 决定（不保证回到原来的位置）；窗口是相对
     自己那块屏还原的，所以位置变了也会跟着走
   - 焦点只还原到「离开时那个窗口还活着」这一层；它被关了就不会挑替代
   - 浮动窗口被手动拖到别的屏上不会改变归属，所以拔插会把它带回归属的那块屏
4. overview 的边界（对着 niri 的还差这些）：
   - 看到的是**现在显示着那块工作区**的列：藏起来的工作区不出现在全景里，也没有 niri 那种
     「把所有工作区排成一行缩略图、在上面拖动窗口跨工作区」的整体视图（重排请用
     `Mod+Shift+H/L`，跨工作区用 `move` 动作）
   - 只缩当前那块屏：别的屏上的窗口是淡出而不是也缩成一个「小显示器」摆进来
   - 缩放的上下限：t = 1 时最多 1:1（只有一两列时不放大），也不做「缩到一定程度就换
     布局」那种分级
   - 没有键盘选择框（overview 里没有「选中项」的独立高亮，焦点即选中）
   - 动画只有缩放 + 压暗，没有 niri 那种「窗口飞向新位置」的连续布局动画
5. 浮动窗口的 `Mod+拖动` 与缩放分支未经自动化测试（只覆盖了客户端发起的移动路径，
   两者共用同一套 grab 代码）；缩放固定从右下角开始，没有边缘判定
5. layer surface 的键盘独占只在 map 时授予，未处理 on-demand 的失焦回收
6. popup 只在创建时下发一次约束盒，父窗口移动后不会重新约束
7. 动画只位移/改透明度，不做缩放：缩放要逐缓冲改 dest size，会被客户端 commit 覆盖，
   这类几何变形交给自定义 shader
8. `shader_idle` 只在布局变化时重绘，所以常驻 shader 里 `u_time` 不会连续推进
9. 装饰只做窗口，不做 layer surface；layer 只有滑入/淡出动画
10. 模糊只包含 background/bottom/toplevels 三层；top/overlay 层不参与背景
11. 模糊用的是**上一帧**的场景快照（本帧开头采样），窗口快速移动时模糊内容有一帧延迟
12. 装饰缓冲固定按 `shadow_radius + |shadow_offset|` 留边，改大阴影半径要重启
13. 按键仍是硬编码（动画/装饰/模糊/shader/列宽走配置文件 —— 列宽那三档见编号 261）
14. 配置里的 shell 命令只在启动和 SIGHUP 时跑，没有定时轮询 / inotify 触发
15. `vidow.exec` 成功时返回 stdout、失败时返回 `nil, 原因`（stderr 优先）；
    要退出码用 `vidow.exec_status`。stderr 单独抓，不混进返回值
16. 两个浮动窗口叠在一起时，两份私有 backdrop 都要重建；动画期间每帧各一次
    （正确但偏贵，可以按「只有自己动了」再细分一代）
17. 输入法：同一时间只服务一个（第二个连上来会被 `unavailable` 劝退）；
    候选框只在焦点窗口是 xdg toplevel 时摆位（layer surface 上的输入框不摆，
    子表面的偏移也没算）
18. 输入法的中文转换没在测试环境里跑通（fcitx5 切不到 rime），只验到 commit 中继本身
19. 没有 `zwp_input_method_v1` / GTK 的老 IME 路径
20. ext-background-effect 只支持 xdg toplevel（layer surface 上的输入框/面板请求了不生效）；
    区域取包围盒，任意形状（比如挖洞的 region）会被当成它的外接矩形
21. 客户端指定区域后，窗口其余部分就不模糊了（哪怕 `blur_all` 开着）。想要「区域只做
    补充、其余照旧模糊」的话得把两者取并集，目前是客户端说了算
22. portal 的 `ScreenCast` / `Screenshot` 现在是 **vidow 自己的后端**（`vidow-portal`，见
    「录屏 portal」那节）：屏幕 / 窗口 / 虚拟屏幕 / 区域四种来源、`hidden` / `embedded` /
    `metadata` 三种光标模式、`Screenshot` / `PickColor` 都通了。剩下的口子：
    `stream_region` 合成器侧实现了，但前端不通过 ScreenCast 要区域（只有交互式截图走它）。
    `zkde_screencast_unstable_v1` 对所有客户端开放是**故意的**（和 KDE 一致，见上面那节的
    「注意」）。
23. portal 的 activation：`set_serial(0)` 的 token 被 wlroots 拒绝，而 GTK / Qt 的
    portal 对话框就是这么发的（能聚焦是因为我们 map 新窗口时无条件给焦点）。
    另外没有给 vidow 自己启动的子进程发 `XDG_ACTIVATION_TOKEN`
24. foreign-toplevel（任务栏 / waybar）的 `set_maximized` 请求没接：vidow 现在有「列最大化」
    了（changelog 177），但那条协议上的请求还没接，任务栏上的最大化按钮点了没反应
    （`set_minimized` / `request_activate` / `request_fullscreen` 都接了，就差它）
25. `ext_foreign_toplevel_image_capture_source_v1` 的截图源建得出来，但没跑过真实的
    逐窗口录屏客户端（同样缺 xdg-desktop-portal-wlr）
26. `portal_conf_sync` 那一步是**会话级**的：`import-environment` +
    `restart xdg-desktop-portal` 影响的是整个 systemd user session。vidow 当会话
    合成器时这是对的；但**嵌套在别的桌面里跑会把宿主的 portal 也指到 vidow**，
    那种场合应该 `portal_desktop=` 关掉，或者接受「宿主的对话框也画进 vidow」
27. 只支持 `[preferred]` 一个段（portals.conf 目前也就只有这个段）；用户覆盖里
    段名行会被跳过，其他段的键会被合并进 `[preferred]`
28. 重启 portal 走 `systemctl --user`；没有 systemd user session 的环境下这一步
    静默失败（文件还是会写，重启要自己来）

29. IPC 的边界：
   - 只有**事件推送**，没有「断线重连后补发」：订阅那一刻之前发生的事收不到（要快照就
     `state`，之后靠事件跟）
   - 没有 i3/sway 那种**命令行解析**（`vidowmsg` 只是把参数拼成 JSON）；也没有 niri 的
     `--json` 之外的输出格式
   - 命令覆盖到的是「合成器自己的功能」，没有 shell 执行（能跑 shell 的只有 config.lua）、
     没有窗口规则的运行时增删（要改规则就改 config.lua + `reload-config`）
   - 输出设置（缩放 / 方向 / 模式 / 位置）是**运行时**的，不写回配置文件：SIGHUP 之后按
     配置重算（想让它持久就写进 config.lua 的 `scale` / `outputs`）
   - 没有权限分级：socket 是 0600（只有本人能连），但连上之后所有命令都能用
   - 事件没有背压策略之外的流控：订阅者卡住（不读 socket）攒到 8 MiB 会被踢掉

30. UI 元素树（给 UI 识别用）：合成器只能给**表面树**（窗口 / 弹层 / 子表面的矩形 + pid），
    控件级的树（角色 / 名字 / 文本 / 动作）**只有 AT-SPI 一条路**，而且只覆盖**实现了无障碍**
    的应用（Chromium / Electron 系还要 `ACCESSIBILITY_ENABLED=1` 才上总线，纯自绘应用永远没有）。
    另外 AT-SPI 报的坐标是**窗口相对的**，全局坐标要合成器补窗口位置 —— 这是 vidow 在这件事里
    唯一不可替代的部分。调研、实测数据（含覆盖率与「操作路径未验证」）与补齐路线见
    [`docs/ui-tree-feasibility.md`](docs/ui-tree-feasibility.md)

31. ~~指针约束只做了鼠标那对，键盘那两个还没接~~ **已做（changelog 176）**：
    `zwp_keyboard_shortcuts_inhibit_manager_v1` 接进了键位那条路（焦点那个表面抑制着时让路，
    `{ inhibited = true }` 的绑定是逃生口），`zwp_xwayland_keyboard_grab_manager_v1` 像
    `src/bgeffect.c` 那样自己写了服务端（X11 的 `XGrabKeyboard`，谁抓谁收）。判据
    `./test-keyboard-grab.sh`。本机 `/usr/bin/Xwayland` 两个都会 bind。

## 下一步建议顺序

1. 浮动窗口的八向缩放边缘 + 双击标题栏最大化（列最大化已经有了，见「列最大化」一节）
2. layer surface 的装饰与模糊（状态栏/启动器也想要圆角）
3. workspace / 多显示器列组
4. `_NET_WM_STRUT`（面板预留），以及按屏幕边界夹菜单的客户端（把 X 屏幕也改成 `逻辑 × S`）
