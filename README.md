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
- **配置**：就是一个 Lua 5.4 文件（`~/.config/vidow/config.lua`）—— 全局名就是配置键，
  需要环境信息时直接 `vidow.exec()` 跑 shell 拿返回值，不阻塞渲染；SIGHUP 热重载
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
  `clipboard_max_bytes`）
- layer-shell：背景/底层/顶层/覆盖四层，支持 exclusive zone（顶栏占位）与键盘独占
- 输入：键盘（xkb + 快捷键）、指针（移动/点击/滚轮/拖动）、虚拟键盘与虚拟指针（可自动化测试）
- 输出：热插拔、`request_state` 尺寸变更后自动重排、`wl_output` + `zxdg_output_manager_v1`
- 协议：wl_compositor、wl_subcompositor、xdg-shell、zxdg-decoration-v1、wlr-layer-shell、wl_data_device、
  primary-selection、virtual-keyboard、virtual-pointer、linux-dmabuf、wlr-screencopy、
  ext-image-copy-capture、wlr-export-dmabuf（grim / slurp / wf-recorder / OBS）
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
- 列宽默认占工作区 50%，可按列循环切换 1/3 → 1/2 → 2/3
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
| `blur` | `enabled` `all` `levels` `strength` `alpha_min` `offset` | `blur = false` |
| `anim` | `open_ms` `close_ms` `move_ms` `layer_open_ms` `layer_close_ms` `easing` `open_easing` `close_easing` `open_alpha_from` `open_slide_px` `open_scale_from` `close_alpha_to` `close_slide_px` `close_scale_to` | — |
| `shader` | `enabled` `dir` `flip_y` `open` `close` `idle` | `shader = false` |
| `clipboard` | `enabled` `primary` `max_bytes` | `clipboard = false` |
| `ipc` | `enabled` `socket` | `ipc = true` |
| `xwayland` | `enabled` `scale` `env` | `xwayland = false` |
| `portal` | `conf_sync` `desktop` | — |

一个 Lua 那边的坑：**同一个键在一个文件里写两遍，只有第一遍生效** —— 第一遍之后它就是个
普通的全局变量了，第二遍不再经过配置层（`vidow.set("gaps", 12)` 每次都能用）。

完整的默认配置（每个键都写着它自己的默认值，抄一份就能改）：

```bash
./build/vidow --default-config > ~/.config/vidow/config.lua
```

仓库里的 `config.lua.example` 就是它的输出；`./test-defaultconfig.sh` 里有一条断言盯着
「这份文件和 `--default-config` 打出来的一模一样」，所以它不会跟代码走散。

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

实测（headless 1280×720、两个 `tools/pattern.py`，读 IPC `state` 的 `geometry`；
列宽 = (1280 − 左右边距) × 0.5，列间距 = 列宽 + 内间距）：

| 配置 | 列宽 | 离屏幕边 | 列间距 |
| --- | --- | --- | --- |
| `gaps 6`（默认） | 634 | 6 | 6 |
| `gaps_inner 20` | 634 | 6 | 20 |
| `gaps 16` | 624 | 16 | 16 |
| `gaps 40` | 600 | 40 | 40 |

同列两个窗口（`move-window-direction -1` 并进一列后）纵向间距跟着 `gaps_inner` 走：
6 → 每格高 351 / 起点 y=6；20 → 344；-50（当 0）→ 354。单边（前置是 `gaps 6`）：
`gaps_left 40` → 左边距 40、列宽 (1280-40-6)/2 = 617、上下仍是 6；再加 `gaps_top 30` →
上边距 30、每格高 (720-30-6-6)/2 = 339；`gaps_left -1` → 左边距回到 6、列宽 634，
`gaps_top 30` 仍然生效。回归脚本：`./test-gap.sh`（33 条断言，全走 IPC，不截屏）。

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
- 交互：`Mod+左键拖动`移动、`Mod+右键拖动`缩放（缩放从右下角开始，最小 64px）
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

工作区是用户建的，键位也是用户绑的 —— 合成器一个工作区快捷键都不预设（内置键位表见
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
| `move up` / `move down` | 搬到相邻的那块工作区 |
| `move-workspace up` / `down` | 整块工作区在这块屏上上/下移一位 |
| `workspace-name <名字>` | 给当前这块工作区起名（之后它就不会被自动收掉；不带参数 = 去掉名字） |
| `special <名字>` | 开/关那个特殊工作区（名字没建过就现建） |
| `focus <-1\|1>` | 焦点左/右移一列 |
| `mousemove` | 把光标所在的那块屏当作「聚焦的屏」（特殊工作区跟的就是它） |
| `none` | 空绑定，用来屏蔽某个键 |

`vidow.bind` 的键名大小写不敏感，修饰键认 `Mod`/`Super`/`Logo`/`Win`、`Shift`、`Alt`/`Mod1`、
`Ctrl`/`Control`；键名可以是单字符（字母、数字）或 X11 惯用名（`grave`、`minus`、`equal`、
`tab`、`space`、`escape`、`bracketleft`…）。**必须带修饰键**：裸键不抢（抢了客户端就没法
打字了），配置写错会在 `config.lua` 那行报错并打一行日志。

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
./test-workspace.sh   # 59 项断言：切工作区只显示那一块（日志 + 四象限像素）、
                        # 配置建工作区 / 特殊工作区不进 list、特殊工作区跟随聚焦的屏、
                        # 拔屏时工作区跟列走、键位绑定表
```

黑盒测试不用假装敲键盘：`VIDOW_TEST_ACTION="600:workspace 2;1200:special scratch"`
按毫秒时间表执行动作；`VIDOW_TEST_ACTION="file:/tmp/actions"` 则是听一个 FIFO，往里写
一行执行一行（`test-workspace.sh` 用的就是它，时序可控且和真实按键走同一个入口
`vidow_action_run()`）。

## 视口外强制渲染（窗口规则 / 录屏 / 窗口预览）

Wayland 的帧回调（`wl_surface.frame`）是合成器「我要用你的画面了」的**许可**：客户端
（mpv、游戏、Qt、GTK）都拿它做节流。窗口滚出视口、或者所在工作区被藏起来时，场景里
什么都没画到它，就没有回调 —— 客户端停帧，切回来是一张冻住的画面，录屏录到的也是它。

用 `tools/frames.c`（一个「只按许可出帧」的窗口）量出来：

| 情况 | 出帧 |
| --- | --- |
| 窗口在视口里 | ~60 帧/秒 |
| 滚出视口，没有规则、没人录 | **0 帧/秒**（停死） |
| 滚出视口，配了 `force_render` | ~60 帧/秒（接着跑；速率可配，见下） |

两条路让它继续出帧，都不用改布局：

```lua
-- 全局节流：视口外补帧每秒最多几帧（0 = 不限速，跟输出的帧节奏走）
force_render_hz = 60          -- 也可以用 VIDOW_FORCE_RENDER_HZ 覆盖

-- ① 窗口规则：哪些窗口值得保活、各补多少帧，由你说了算（子串匹配，大小写不敏感）
vidow.rule{ app_id = "mpv", force_render = true }              -- 用全局的 60Hz
vidow.rule{ title = "预览", force_render = true, hz = 15 }     -- 这个窗口只补 15Hz
vidow.rule{ class = "Steam", force_render = false }            -- 后面的覆盖前面的
vidow.rule{ app_id = "vlc", hz = 0 }                           -- 只调节流：不限速

-- 一条规则至少写一个作用（force_render / hz），都不写会在配置那一行报错。

-- 查回来（状态栏 / 自检用；空字段按 * 显示是调用方的事）
local function star(v) return (v == nil or v == "") and "*" or v end
for i, r in ipairs(vidow.rules()) do
  vidow.log(string.format("rule %d: app_id=%s title=%s class=%s force_render=%s hz=%s",
    i, star(r.app_id), star(r.title), star(r["class"]), tostring(r.force_render),
    r.hz == nil and "配置默认" or tostring(r.hz)))
end
```

| 项 | 说明 |
| --- | --- |
| 匹配字段 | `app_id` / `title` / `class`（X11 的 `WM_CLASS`），都是**子串、大小写不敏感**；留空 = 不限。三个都空 = 匹配所有窗口（能用，但日志里会打成 `* / *` 提醒你） |
| 覆盖顺序 | 按配置里的先后逐条套用，**后面的覆盖前面的**（同一个窗口命中多条时以最后一条为准） |
| 生效时机 | 建窗口时判一次；标题 / class 变了再判一次；`SIGHUP` 重载规则后把已经开着的窗口全部重判 |
| 节流 | `force_render_hz = <每秒几帧>`（默认 60，`0` = 不限速）；规则里写 `hz = <n>` 可以逐窗口覆盖。补帧要主动排下一帧，不限速时会一直出帧（headless 后端上量到过 2500fps），所以默认按刷新率给 |
| ② 录屏 / 窗口预览 | **自动**：有人为某个窗口建了截图源（xdg-desktop-portal 的窗口录制、预览缩略图走的就是这条路），那个窗口立刻进入强制渲染；截图源没了、或者录屏客户端断开（崩了 / 被关掉）就自动退出，不会一直补帧 |

机制（`src/view.c` 的 `vidow_view_render_pump`）：每帧在场景补帧之后，把「规则要求保活」
或者「正被人拿着」但**没画到**的窗口挑出来，给它的整棵表面树补一次
`wlr_surface_send_frame_done()`，并主动排下一帧（不然画面没变化时 wlroots 就不给帧事件，
补帧链自己就断了）。两个细节：

- **只在没画到的时候补**：窗口在画面上时场景那边已经给过回调，再补一次会让客户端白画一遍
  （判断用「所在工作区显示着」+「矩形和这块屏有交集」）。
- **节流可配**（`force_render_hz`，规则可逐窗口覆盖 `hz`；0 = 不限速）：补帧会自己排
  下一帧，不节流的话在没 vsync 的后端（headless、嵌套）上会空转到几千 fps —— 一个
  「保活」把 CPU 跑满就没意义了。到点的唤醒用一个**闹钟**（`wl_event_source` 定时器）
  来做：只靠「客户端提交了再推」是不行的 —— 提交一次之后没人再推，30Hz / 15Hz 会断在
  半路（实测全局 30 补出来 0 帧，改成闹钟之后是 30 帧/秒）。

### 测试

```bash
./test-force-render.sh   # 28 项断言：控制组（不配就停帧）→ 规则（~60 帧/秒）→ 回到画面
                         # 上就停 → 藏起来的工作区照样补帧 → 录屏（抓到的画面在变）→
                         # 录屏客户端断开后不再补帧 → 节流可配置（全局 30Hz / 规则 15Hz
                         # / hz = 0 不限速，各量一遍帧率）
```

两个测试客户端也是这次加的：`tools/frames.c`（只按帧回调出帧，每帧打印一行，把「客户端
还在不在出帧」变成可断言的输出）、`tools/capture.c`（走 ext-image-copy-capture +
窗口截图源抓某个窗口，打印每帧的像素校验和 —— 校验和不变就说明录到的是冻住的一张）。

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
```

| 项 | 说明 |
| --- | --- |
| `focus_on_open` | 这个应用**开新窗口**时把焦点给它吗（默认 `true`）。写 `false` 之后新窗口不抢键盘；**但当前一个焦点窗口都没有时还是会给它**（否则没人收键盘） |
| `focus_on_activate` | 别人**请求激活**它（点通知、任务栏按钮、portal 顶窗口、X11 的 `_NET_ACTIVE_WINDOW`）时理不理（默认 `true`） |
| `honor_invalid_serial` | 它的激活请求**即使令牌里的 serial 校验不过**也照样理（默认 `false`）—— 全局开关的**按应用**版本，见「serial 校验不过的请求」那节 |
| 查回来 | `vidow.rules()` 里 `focus_on_open` / `focus_on_activate` 没写就是 `nil` —— 「这条规则没管它」和「管了、给了默认值」是两回事 |
| 一条规则至少写一个作用 | `force_render` / `hz` / `focus_on_open` / `focus_on_activate` / `honor_invalid_serial`，都不写会在配置那一行报错 |

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
./test-focus.sh   # 61 项断言：基线（新窗口默认抢焦点）→ focus_on_open = false（不抢键盘
                  # 但窗口照常画出来，像素为证）→ 激活请求把藏起来的工作区端出来（像素：
                  # 激活前黑、激活后画出来）→ 特殊工作区同理 → 令牌过期 vs 不过期（对照）
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
`egl` + `glesv2`（装饰 / 模糊 / 自定义 shader 走 GLES2）、`lua5.4`。
运行 X11 程序还需要 `xorg-xwayland`（Arch 的 `xorg-xwayland`）。

注意：Arch 的 `wlroots0.20` 包没有安装 `wlr-layer-shell-unstable-v1-protocol.h`（而
`wlr/types/wlr_layer_shell_v1.h` 会 include 它），所以协议 XML 放在
`protocols/` 里，构建时用 `wayland-scanner` 现场生成头文件。

## 运行

嵌套在现有 Wayland 会话里跑（推荐，随时可关）：

```bash
WLR_BACKENDS=wayland ./build/vidow
```

一键验证（嵌套启动 + 3 个客户端 + waybar 顶栏）：

```bash
./run-nested.sh              # 默认 kitty，可换 ./run-nested.sh alacritty
./test-keys.sh               # wtype 注入按键，验证焦点/滚动/列宽
python3 test-float.py        # 三个真实 GTK 窗口，验证浮动模式自动识别
./build/vdrag 7000 3000 9500 5000   # 虚拟指针拖拽（归一化 0..10000 坐标）
./test-portal.sh switch-gtk          # 从 ~/.config/vidow/portals.conf 走生成流程再验证
./test-portal.sh switch-kde          # 同上，换成 KDE
./test-portal.sh raw-gtk             # vidow 不碰 portal 配置，手写一份单独验证协议
./test-portal.sh raw-kde             # 同上，换成 KDE
./test-dpi.sh                        # 混合 DPI + XWayland（全 headless，15 项断言）
./test-focus.sh                      # 焦点规则 + 激活请求（全 headless，61 项断言）
./test-ipc.sh                        # IPC：状态输出 / 命令 / 事件 / 多屏（全 headless，162 项断言）
./test-config.sh                     # 配置系统：一个 Lua 文件 + 改了就自动重载（全 headless，15 项断言）
./test-defaultconfig.sh              # 完整默认配置（--default-config）+ 配置族的表写法（全 headless，20 项断言）
./test-gap.sh                        # 间距：一个数管全部 + 单边/内间距覆盖（全 headless，33 项断言）
./test-layerblur.sh                  # 半透明状态栏背后的模糊（全 headless，4 项断言）
./test-blurscale.sh                  # 模糊在高 DPI 倍率下的底衬覆盖 + 对位（1/1.7/2 倍，7 项断言）
./test-blurlevel.sh                  # 半径随级数扩展（2/4/6 级各跑一轮，3 项断言）
./test-layerghost.sh                 # 会被模糊的 layer surface 不进自己的底衬（3 项断言）
./test-layerwin.sh                   # top 层模糊里含窗口（纯蓝窗口 + 压在它上面的面板，4 项断言）
./test-layermask.sh                  # 按元素自己的 alpha 剔除（透明内边距里的亮点不糊，3 项断言）
./test-openanim.sh                   # 开窗动画画的是装饰后的窗口本身（不是一块更大的矩形，4 项断言）
./test-bounce.sh                     # 入场过冲 / 关闭回弹（q 弹曲线真的落到屏幕上了，5 项断言）
./test-dpi.sh                        # 混合 DPI + XWayland：装饰缓冲按**窗口几何**算（见 SSD 那节）
./build/jsontest                     # 只看 JSON 写出/读入（离线，30 项）
./build/scaletest                    # 只看缩放计算（24 项）
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
| `VIDOW_TERMINAL` | `Super+Enter` 启动的命令，默认 `kitty` |
| `VIDOW_BLUR_DUMP=N` | 前 N 份底衬 / 模糊图 / layer 区域抓取 dump 成 `/tmp/vidow-{bd,blur,lbd,lblur}-*`（裸 ARGB，测试用） |
| `VIDOW_FX_DUMP=N` | 前 N 份装饰离屏缓冲 dump 成 `/tmp/vidow-fx-*` |
| `VIDOW_MASK_DUMP=N` | 前 N 份透明度剔除的 mask（layer 自己的内容）dump 成 `/tmp/vidow-mask-*` |

## 快捷键

`Super` = `Mod`（`WLR_MODIFIER_LOGO`）。

| 按键 | 动作 |
| --- | --- |
| `Mod+H` / `Mod+L` | 焦点左/右移一列（视口跟随滚动） |
| `Mod+J` / `Mod+K` | 焦点在列内下/上移；单窗口列则跳到下/上一列 |
| `Mod+Shift+H` / `Mod+Shift+L` | 当前列左/右移一位 |
| `Mod+Shift+J` / `Mod+Shift+K` | 列内窗口下/上移；单窗口列则塞进下/上一列 |
| `Mod+V` / `Mod+Shift+Space` | 当前窗口 平铺 ⇄ 浮动 |
| `Mod+左键拖动` / `Mod+右键拖动` | 移动 / 缩放浮动窗口 |
| `Mod+R` | 列宽循环 1/3 → 1/2 → 2/3 |
| `Mod+F` | 当前窗口全屏切换 |
| `Mod+O` | niri 风格全景（overview）：整条列带缩小铺满这块屏，点谁去谁 |
| `Mod+Q` | 关闭当前窗口 |
| `Mod+Enter` | 启动终端 |
| `Mod+滚轮` | 焦点左/右移一列 |
| `Mod+Escape` | 退出合成器 |
| `Mod+U` / `Mod+I` | 工作区上/下切一块（niri 的默认键位） |
| `Mod+Shift+U` / `Mod+Shift+I` | 把当前窗口搬到上/下一块工作区 |
| `Mod+Ctrl+U` / `Mod+Ctrl+I` | 整块工作区上/下移一位（重排） |
| `Mod+1`…`Mod+9` | 切到这块屏上第 n 块（默认配置里给的绑定，可删；越界夹到最后一块） |
| — | 特殊工作区 / 具名工作区的键位不预设，由 `config.lua` 里 `vidow.bind()` 自己绑 |

鼠标左键点击窗口即聚焦并滚动到该列；点击 layer 表面（启动器）时按需交出键盘焦点。

overview 里：左键点窗口 = 选中它并缩回去，点空白 = 取消，`Esc` 退出，滚轮左右平移相机
（`Mod+H/L` 照样换列，换列会把相机平移清零、改回跟着焦点走）。

普通滚轮事件转发给客户端，不用于滚动视口（用 `Mod+滚轮` 或 `Mod+H/L`）。

## 混合 DPI 与 XWayland

目标很具体：**同一个逻辑尺寸，在两块 DPI 不同的屏上是同一个物理尺寸，而且都清晰。**
缩放就是这一条：逻辑像素 = 1/96 英寸。

```lua
-- ~/.config/vidow/config.lua
scale = "auto"                      -- 缩放 = ppi / 96
outputs = "DP-1:2 HDMI-A-1:1.25"    -- 按输出名覆盖（也可以写 auto / off）
xwayland = true                     -- 起 XWayland（懒启动：真有人连 X 才 fork）
xwayland_scale = "auto"             -- X 的全局缩放 = ceil(最密那块屏的缩放)
xwayland_env = true                 -- 给 X11 客户端下发 GDK_SCALE / QT_SCALE_FACTOR
```

`scale = auto` 按 EDID 的物理尺寸算 `ppi / 96`：4K 27 寸 ≈ 1.70、1440p 27 寸 ≈ 1.15、
1080p 24 寸 ≈ 0.96（夹到 1.0）。量化到 0.05 再夹进 1.0～8.0 —— `ppi/96` 会算出
1.7037037 这种值，直接拿去当缩放每帧取整都不一样。

`./build/scaletest` 是不启动合成器的离线自检（24 项），其中一项就是实算「同一个逻辑
尺寸在两块屏上的物理尺寸差多少」：

| 两块屏 | 200 逻辑像素的物理尺寸 | 差 |
| --- | --- | --- |
| 4K 27" + 1080p 24"，`auto`（1.70 / 1.00） | 2.081 in / 2.178 in | 4.65% |
| 同上，手写整数缩放（2 / 1） | 2.449 in / 2.178 in | 11.0% |

| 键 | 默认 | 作用 |
| --- | --- | --- |
| `scale` | `auto` | 所有输出的默认策略：`auto` = 按 ppi 推，或者写一个数（`1.75`） |
| `outputs` | 空 | 按输出名覆盖：`outputs = DP-1:2 HDMI-A-1:1.25 eDP-1:auto` |
| `xwayland` | `true` | 起不起 XWayland（`0` = 完全不碰 X 那一摊） |
| `xwayland_scale` | `auto` | `auto` = `ceil(最密那块屏的缩放)`；`off` = 一律 1:1；也可以写死一个数 |
| `xwayland_env` | `true` | 给 X11 客户端下发 `GDK_SCALE` / `QT_SCALE_FACTOR` / `XCURSOR_SIZE`（用户自己显式设过的不抢） |

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

`GDK_SCALE` / `QT_SCALE_FACTOR = S` 发给 X11 客户端：认这两个变量的 GTK/Qt 按 S 倍
绘制（于是和 Wayland 程序一样大）；不认的老程序（xterm、xeyes）保持 1:1 绘制 ——
**物理尺寸在两块屏上依然一致、依然清晰，但相对 Wayland 程序偏小**。这是 X11 的硬限制：
要么放大（一样大但糊），要么不放大（清晰但小）。想要后一种取舍就把 `xwayland_scale`
设成 `off`。

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
| 模糊 backdrop | 浮动窗口有 | 没有 |
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
./build/scaletest   # 24 项纯计算断言
```

`test-dpi.sh` 从日志里量出来的（每次 commit 记一行缓冲区 / 落屏像素）：

| 谁 | 在哪块屏 | 缓冲区 | 落屏设备像素 | 结论 |
| --- | --- | --- | --- | --- |
| X11（xterm） | 2 倍屏 | 628×696 | 628×696 | 1:1 |
| X11（xterm） | 1 倍屏 | 1268×1416 | 634×708 | 缩小 2 倍（不放大） |
| Wayland（GTK3 zenity） | 1 倍屏 | 340×264 | 340×264 | 1:1 |

`test-dpi.sh` 还会验：`GDK_SCALE=2` 真的进了子进程（`config.lua` 里
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
| 背景 | 一块遮罩把壁纸压暗（`layer_overview_dim`：夹在壁纸之上、窗口之下），缩小的窗口浮在上面 |
| 模糊 | overview 里不采样背景模糊（那套 UV 按窗口自己的位置算，缩小之后对不上） |

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
./test-overview.sh   # 26 项：日志 + 像素
```

判据：`overview: 进入（… N 个窗口，占 WxH 缩到 S，铺在工作区 …）` 加每个窗口一行
「窗口 X -> 屏幕 x,y wxh（缩放 S）」；overview 里的尺寸 = 原尺寸 × S（±2px）、列间距同样
按 S 缩、顺序不变、所有窗口都在屏内；按日志坐标采样窗口四象限颜色（红绿蓝白）正确且
**没有被压暗**；壁纸被压暗了；`Esc` 退出后画面回到原样（只允许取整带来的零星像素差）；
点某个窗口 → `overview: 选中 X` + 退出 + 那一列被滚进视野；两块屏时只缩当前那块、另一块
屏上的窗口淡出。

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
| `windows` | 每个窗口：`id`、`kind`（xdg / x11）、`title`、`app_id`、`pid`、`geometry`（布局坐标）、`float_geometry`、`surface`（表面尺寸 / 缓冲 / scale）、`column`（列 id / 宽比 / 偏移 / 序号）、`workspace`、`output`、`home`、`floating` + `float_mode`、`fullscreen`、`mapped` / `visible`、`rules`（三条规则的实际取值）、`size_hints`、`anim_*`、`force_render`、`capture_holders`、X11 还多一份 `x11` 子对象 |
| `columns` | 每条列：`id`、`width_fraction`、算出来的 `width` / `offset`、序号、里面的窗口 |
| `workspaces` | 每块工作区：名字、`special`、`active` / `visible` / `shown`、住哪块屏 / 归属哪块屏、滚动位置、列与窗口、离开时的焦点窗口（**特殊工作区也在列表里**，`special = true`；Lua 的 `vidow.workspace_list()` 不给它） |
| `outputs` | 每块屏：连接器名 / 描述 / make / model、`enabled`、`scale`、`transform`、刷新率、物理尺寸、布局矩形、`usable_area`、`viewport_x`、活动 / 显示着的工作区、它上面的工作区 / 窗口 / layer、**所有可用模式**、模糊贴图状态 |
| `layers` | 每个 layer surface：`namespace`、`layer`、`output`、`mapped`、`exclusive_zone`、`keyboard_interactivity`、`anchor`、`margin`、想要 / 实际尺寸、几何 |
| `config` | **配置表的全部键值**（跟 `config.lua` 里 `vidow.get` 读到的是同一个东西）；`config` + `key` 只取一个 |
| `keybinds` / `rules` | 键位表（触发串 + 动作串）、窗口规则（含「这一条到底写没写」的 null 语义） |
| `seats` / `input` | seat 的能力列表、当前键盘的布局 / repeat / LED、所有键盘（含虚拟键盘）、光标位置、当前的交互式 grab、拿着键盘的 layer surface |
| `tokens` | 活着的 xdg-activation 令牌（app_id、serial 校验过没过、活了多久） |
| `memories` | 热插拔记忆（每块见过的屏：在不在、上次的布局位置、离开时的滚动位置和焦点） |
| `anims` | 正在跑的动画（tag / 进度 / 时长 / 缓动）+ 正在淡出的关闭快照 |
| `im` / `overview` / `xwayland` / `cursor` / `focused` | 输入法状态、全景状态（相机 / 缩放）、XWayland 的开关与全局缩放、光标坐标、焦点三件套 |

几何是**布局坐标**，跟 `place` 日志、grim 的像素坐标是同一套（单屏时就是屏幕坐标）。

### 命令

| 类别 | 命令 |
| --- | --- |
| 窗口 | `focus-window`、`activate-window`（= 点通知：先把工作区端出来再给焦点）、`close-window`、`toggle-floating`、`set-floating`、`toggle-fullscreen`、`set-fullscreen`、`set-window-size`、`set-window-position`、`center-window`、`set-window-force-render`、`focus-window-direction`、`move-window-direction`、`move-window-to-workspace`、`move-window-to-special`、`move-window-to-output` |
| 列 | `focus-column`、`move-column`、`cycle-column-width`、`set-column-width`（`fraction` 绝对值 / `delta` 相对）、`expand-column-to-available-width`、`center-column` |
| 工作区 | `focus-workspace`（名字 / `next` / `prev` / `index`）、`toggle-special`（= `focus-special`）、`create-workspace`、`move-workspace-to-output` |
| 输出 | `output-power`（`on` / `off` / `toggle`）、`output-enable` / `output-disable` / `output-toggle`、`output-scale`（数字或 `auto`）、`output-transform`、`output-mode`（宽 高 [刷新率]）、`output-position`、`output-focus` |
| 剪贴板 | `clipboard`（查状态）、`clipboard-get`（取数据：文本或 base64）、`clipboard-set`（放数据：文本 / base64 / 指定 mime / 指定选区）、`clipboard-clear` |
| 合成器 | `action`（键位那套动作字符串）、`set-config` / `get-config`、`reload-config`、`toggle-overview`、`move-cursor`、`quit` |
| 订阅 | `subscribe`（可带 `events` 过滤器）、`unsubscribe` |

几条语义值得记一下：

- **关屏**（`output-power off`）只是「不画了」：窗口、焦点、工作区都不动，打开就照原样
  回来；关掉的要是当前聚焦的那块屏，焦点会自动挪到还开着的屏上。
- **`set-window-size` 只对浮动窗口有效**（平铺尺寸由布局定），而且是「请求」：客户端可以
  拒绝（固定尺寸的对话框）。我们要的尺寸会一直保持到它认下来 —— 一个迟到的旧尺寸提交
  不能把这条命令吃掉（踩过这个坑，见下）。
- **`set-config` 是运行时改**：改完立刻生效（外观类的东西下一帧就是新的），但**不落盘**，
  SIGHUP / `reload-config` 之后按配置文件重来。
- **`quit` 是正常退出**：走完整的析构路径（顺带修掉了一个一直藏着的 wlroots 断言，见下）。
- **不提供 shell 命令**：IPC 只能操作合成器自己的功能，不能执行命令（能跑 shell 的是
  `config.lua`，那是你自己写的文件）。安全边界就是 socket 的权限：`0600`，只有本人能连。

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
| `workspace-created` / `workspace-activated` / `workspace-changed` / `workspace-special-changed` | 工作区建了 / 切了 / 换屏了 / 特殊工作区开关了 |
| `output-added` / `output-removed` / `output-changed` | 屏插拔 / 开关 / 缩放 / 方向 / 模式 / 位置变了 |
| `layer-mapped` / `layer-unmapped` / `layer-closed` | layer surface 上屏 / 下屏 / 没了 |
| `clipboard-changed` / `primary-changed` | 剪贴板 / 选区的内容或状态变了（载荷是那一刻的状态对象，跟 `clipboard` 命令一样） |
| `overview-toggled` / `config-reloaded` | 全景开关、配置重载（没有载荷，去查状态） |

`subscribe` 可以带过滤器：`{"cmd":"subscribe","events":["window-opened"]}`，只收订了的
那几种。没有订阅者时事件那条路完全不走（`vidow_ipc_wanted()` 先挡一下），不订阅就不花
这份钱。

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
```

`set-config` / SIGHUP 都能热切换：关掉时会把已经发布的选区收回（`sel_unpublish`），X 侧的
监听窗口与 XFixes 注册一起摘掉，退出路径上不留东西。

### IPC

```bash
vidowmsg clipboard                       # 状态：来源 / X owner / 在取数 / 字节数 / 类型列表
vidowmsg clipboard-get                   # 取剪贴板（文本直接给，别的类型给 base64 + mime）
vidowmsg clipboard-get --selection primary
vidowmsg clipboard-set "hello"           # 放剪贴板（也可以 --mime image/png --base64）
vidowmsg clipboard-clear
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
（`layer_im`）vidow 现在根本没有模糊，所以那边没有可剔的东西。

**底衬不做剔除**（曾经有，撤了）：原来在最后一趟上升里按底衬的 alpha 做过一次
`step` 遮罩（那个程序叫 `kawase_final`），现在整个撤掉 —— 剔除是用来约束**范围**的，
不是用来改糊出来的像素；底衬本身就是预乘的，它透明的地方 alpha 跟着一起淡出，合成的
时候自然透出真实背景。`kawase_final` 这个程序随之消失，最后一趟就是同一个 `kawase_up`。

`blur_alpha_min` 只进装饰着色器、不进模糊金字塔：改它由 `blur_mask_min_changed()`
（`src/fx.c`）把 `blur_ver` 顶一下，帧循环自然重画 —— 不然运行时改配置会看着「没反应」。

配置：`blur_enabled` / `blur_all` / `blur_levels`(2..6) / `blur_strength` / `blur_offset` /
`blur_alpha_min`。默认 `blur_all = true`，平铺窗口也模糊。

模糊结果按帧缓存，但**只要背景变了就立刻重算**，所以视频、滚动、拖动背后都是实时模糊。

### `blur_levels` 是降采样次数

是。`blur_levels = n` 就是「降采样 n 次」：链上第 i 级的分辨率是 `输出 >> (i+1)`，
所以 4 级 = 1/2、1/4、1/8、1/16，最终结果是**半分辨率**（`up[0]`，给装饰着色器采样的
那张）。级数越大模糊半径越大、也越贵（每级一趟 pass）。运行时夹在 2..6
（`MAX_BLUR_LEVELS`，在 `server.h` 里，数组尺寸也用它），并且最小那一级会小于 4px
时自动降一级（`blur_levels_for`）。

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

### 配置：就是一个 Lua 文件

`~/.config/vidow/config.lua`（Lua 5.4）是**唯一**的配置文件 —— 没有第二套语法、
没有加载顺序要记。**改完保存就生效，什么都不用做**（也可以手动
`kill -HUP $(pgrep -x vidow)`）。

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
真变了才重载，顺带把一次保存里的好几个事件合成一次（去抖 150ms）：

| 你怎么改 | 结果 |
| --- | --- |
| 编辑器里改完保存（原地写 / rename 覆盖） | 自动重载 |
| 删掉配置文件 | 自动重载，回默认值 |
| 删掉某个键再保存 | 那个键回默认值 |
| 同一个目录里动别的文件（`portals.conf` / `shaders/`） | 不重载 |
| 配置文件是符号链接（dotfiles 仓库） | 认（盯的是解析后的真身） |

```lua
-- 全局名就是配置键
border_width = 2
corner_radius = 12
scale = "auto"
outputs = "DP-1:2 HDMI-A-1:1.25"

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
（原因优先用 stderr）。需要退出码时用 `vidow.exec_status(cmd)` → `out, code, err`
（永远 3 个值）。stdout 和 stderr 分开抓。

```lua
-- 一个值，可以直接嵌进调用
corner_radius = tonumber(vidow.exec("echo 16")) or 12

-- 失败时拿得到真正的原因
local v, why = vidow.exec("cat /nonexistent")
-- v = nil, why = "cat: /nonexistent: 没有那个文件或目录"

-- 要看退出码（grim 只写文件、不打印）
local out, code, err = vidow.exec_status("grim /tmp/shot.png")
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
| `portal_desktop` | `Vidow` | vidow 设给自己的 `XDG_CURRENT_DESKTOP`，也是 `<name>-portals.conf` 的那个 name。留空 = 完全不碰 portal |
| `portal_conf_sync` | `true` | 把生效的 portals.conf 写到 portal 的配置目录并重启它；`false` = 只算不写 |
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

`vidow_portal_conf_sync()` 只在生成内容和现有文件**逐字节相同**时跳过，否则才写盘并重启
portal —— 不会每次开机都白重启一遍、打断正在进行的 portal 请求。重启是双 fork + setsid
的，异步、不阻塞渲染。

两个环境变量（给测试/排查用）：`VIDOW_PORTAL_NO_RESTART=1` 只写文件不重启；
`VIDOW_CONFIG=/path` 换配置文件位置（`VIDOW_CONFIG_LUA` 是同一个东西的旧名字，还认）。

后端是被 DBus 激活起来的，继承的是 systemd user session 的环境，所以重启前必须
`import-environment`（否则对话框会画到宿主桌面上）。vidow 自动做这一步：

```bash
systemctl --user import-environment XDG_CURRENT_DESKTOP WAYLAND_DISPLAY XDG_RUNTIME_DIR
systemctl --user restart xdg-desktop-portal
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
`linear` / `out_cubic` / `out_quint` / `in_out_cubic` / `out_back` / `out_elastic`。

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

## 代码结构

```
src/server.h   共享类型（view / column / output / layer / keyboard）与前向声明
src/main.c     启动：display/backend/renderer/scene/seat/layer-shell/vkbd/vptr、socket、事件循环
src/output.c   output 创建、frame 提交、request_state、usable_area 初始化、动画/捕获看门狗
src/view.c     列模型、滚动布局、焦点/移动/改宽、激活（vidow_view_activate：先把窗口那块
               工作区端出来再给焦点）、窗口生命周期（xdg-toplevel 和 X11 两种壳，差别收在
               vidow_view_* 访问器里）、X11 缓冲区倍数声明、popup
src/float.c    浮动模式识别与放置、交互式移动/缩放 grab
src/layer.c    layer surface 生命周期、层级树、exclusive zone、键盘独占、出现/消失动画
src/decoration.c zxdg_decoration_manager_v1：按 decor 配置告诉客户端「装饰谁画」。
                  不给这个全局，kitty / GTK / Qt 就自己画标题栏，跟我们的圆角打架
src/input.c    键盘/指针/滚轮/拖动、快捷键、虚拟键盘与虚拟指针、光标、剪贴板
src/config.c   配置解析（文件 + VIDOW_* 环境变量覆盖，含颜色/布尔/枚举）
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
src/overview.c niri 风格全景：世界→屏幕的缩放变换、相机（跟焦点 + 滚轮平移）、压暗遮罩、
               点选/退出。窗口那一侧由 view.c 的 vidow_view_apply_anim 套用
src/xwayland.c XWayland：懒启动服务、seat、xwayland_shell 只给 Xwayland 自己、
               X 全局缩放（= ceil(最密那块屏)）、X11 客户端环境（GDK_SCALE 等）
src/exec.c     异步 `sh -c`：pipe + wl_event_loop_add_fd + 共享超时定时器，绝不阻塞渲染
src/script.c   配置语言：Lua 5.4，协程化的 vidow.exec、全局名→配置键、SIGHUP 重载
src/anim.c     动画引擎：单调时钟进度、缓动函数、每帧 tick
src/fx.c       SSD 装饰着色器、kawase 模糊链（共享/私有 backdrop + 世代缓存）、离屏渲染、关闭快照
src/im.c       输入法中继：text-input-v3 <-> input-method-v2、候选框摆位、键盘抢占用与回注防环
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
src/clipboard.c X11 <-> Wayland 剪贴板桥（自己接管，不用 wlroots 那份带焦点闸门的）：
               拦 xwm 连接上的选区事件、两边各留一份内存副本、INCR 双向 —— 详见「剪贴板」
src/portal.c   portal 要的协议：xdg-foreign v1+v2（共用 registry）、
               foreign-toplevel-management、ext-foreign-toplevel-list + 截图源
               （xdg-activation 在 activation.c）
src/portalconf.c  portal 后端选择：内置默认 → ~/.config/vidow/portals.conf 逐键覆盖
               → 同步到 portal 的配置目录并重启它
shaders/       示例片段着色器（zoom / genie / wave / round / passthru）
tools/vdrag.c  虚拟指针拖拽测试工具
tools/xpopup.c 按绝对坐标造 X11 窗口（owner / or / tooltip / menu / combo / utility），
               可指定 WM_TRANSIENT_FOR、晚设窗口类型、自声明固定尺寸、延后自己挪位 ——
               弹出物分类与定位的测试客户端
tools/scaletest.c  缩放计算的离线自检（解析 / ppi 推算 / 量化 / 优先级 / 物理尺寸差）
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
test-portal.sh    私有 dbus 里起 portal + 真实 FileChooser 请求
                  gtk/kde 验证协议链路，switch-gtk/switch-kde 验证配置生成链路
test-dpi.sh       1x + 2x 两块 headless 屏，量 X11 / Wayland 客户端的缓冲区与
                  落屏设备像素（只准缩小不准放大）、X 全局缩放、指针命中坐标
test-xwayland-popup.sh  弹出物：识别（含属性晚到）、绝对定位、不套 toplevel 样式
                  （装饰缓冲 / 像素级无阴影 / 无动画）、不抢焦点、层级、混合 DPI
test-clipboard.sh 剪贴板桥：接管、X11->Wayland、Wayland->X11、PRIMARY、INCR（大块）、
                  错误路径与事件、配置热切换（关掉退回 wlroots 那套）、退出与清理
test-hotplug.sh   真拔一块 headless 输出再以同名插回来：列 / 浮动窗口 / layer surface
                  的归属与还原（顺序、列宽、滚动位置、平移量、四象限像素）
test-layerwin.sh  top / overlay 层的模糊里必须含窗口（纯蓝窗口 + 压在它上面的面板，
                  量面板那块截屏的 B-R 与区域底衬 dump）
test-overview.sh  Mod+O 全景：缩放比例/位置/顺序、四象限像素、遮罩只压背景不压窗口、
                  Esc 退出后画面还原、点选、两块屏时只看当前那块
portals.conf.example  拷成 ~/.config/vidow/portals.conf
protocols/     wlr-layer-shell / wlr-virtual-pointer / input-method-v2 /
               ext-background-effect-v1 / xdg-shell / xdg-foreign v1+v2 协议 XML
               （构建时用 wayland-scanner 生成头文件）
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
50. 缩放计算离线自检 24 项（`./build/scaletest`）：`auto` 反推 1.7016 / 1.1344 / 0.9564，
    量化 1.7037037→1.70、0.956→夹到 1.0、上限 8.0，每输出覆盖压过全局，
    `1.5x` / `abc` / `0` 被拒，`off` → 1.0
51. 配置边界：`xwayland=0` → `配置里关了，不注册 X 服务`，合成器照常跑；
    `xwayland_scale=off` → `GDK_SCALE=1`、不再升到 2、X11 窗口按 1 倍声明（HiDPI 屏上
    是放大糊的那个取舍）；`xwayland_env=0` → 一个环境变量都不碰
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
     `config.lua.example` 就是它的输出，`./test-defaultconfig.sh` 里有一条断言盯着这两者一致，
     所以文档不会跟代码走散（`shader_dir` 那种跟着配置目录走的默认值打的是注释，不把 HOME 写进去）。
     实测：默认布局不变（离屏幕边 6 / 窗口之间 6、列宽 634）；`gaps 16` 从「离屏幕边 32 + 列宽 608」
     变成「16 + 624」；`gaps 40` → 边距 40、列宽 600。`./test-gap.sh` 按新语义重写成 33 条断言，
     新增 `./test-defaultconfig.sh`（20 条：默认配置回灌后逐键等于内置默认值 / 表写法只盖写了的项 /
     报错信息 / 老键名提示 / `config.lua.example` 不漂移）。

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
   - 没有「焦点跟随鼠标」（`focus-follows-mouse`）、没有「新窗口一律不抢焦点」的全局开关
     （要的话写一条不带匹配字段的规则：`vidow.rule{ focus_on_open = false }`，它会匹配所有
     窗口 —— 但连你自己的终端也会不抢，谨慎）
   - 激活请求只在「窗口已经存在」时能兑现：应用还没启动 / 窗口还没建出来时点通知，
     我们不负责把它拉起来（那需要 xdg-activation 的启动令牌 + 会话恢复）
2. XWayland 的边界（真实 X11 程序能跑，但下面这些没做）：
   - 没有 `_NET_WM_STATE` 最大化 / 最小化、没有 `_NET_WM_STRUT` 面板预留：任务栏上的
     最大化按钮点了没反应（请求会被回一个「不最大化」），X11 面板也不会把可用区挤出来
   - X11 弹出物（菜单 / 提示 / 通知）不装饰、不做动画、不进布局、不抢焦点，按客户端
     给的绝对位置显示，层级在所有托管窗口之上（和 Wayland 的 xdg_popup 一致）；
     `_MOTIF_WM_HINTS` 故意不听，见上文
   - 客户端按「屏幕边界」夹弹出菜单时可能偏：X 屏幕（客户端看到的尺寸）是**逻辑**尺寸，
     而我们告诉窗口的坐标是 `逻辑 × S`。窗口内部的相对定位是对的（用命中坐标验过），
     但按屏幕边缘做裁剪的客户端会算错
   - 分数缩放下 GTK3 不认 fractional-scale，会按 `ceil` 渲染再被我们缩小（仍然清晰，
     只是多花像素）；GTK4 / Qt6 认 fractional-scale，按 1.70 精确渲染
   - Xft 系（GTK2、老 Motif）不认 `GDK_SCALE`：保持 1:1（清晰但相对偏小）。要它们也变大
     就自己在 config.lua 里跑 `xrdb -merge` 写 `Xft.dpi` —— 但别和 `GDK_SCALE` 一起用，
     会叠成 4 倍字号
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
13. 列宽、按键、间隙仍是硬编码（动画/装饰/模糊/shader 走配置文件）
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
22. portal 的 `ScreenCast` / `Screenshot` 接口没接上：KDE 的实现走 KWin 私有 IPC，
    在 vidow 上必然失败。vidow 实现的是 wlr-screencopy + ext-image-copy-capture，
    这两个接口该交给 `xdg-desktop-portal-wlr`（当前机器上没装，装了把
    `vidow-portals.conf` 里那两行注释打开即可）
23. portal 的 activation：`set_serial(0)` 的 token 被 wlroots 拒绝，而 GTK / Qt 的
    portal 对话框就是这么发的（能聚焦是因为我们 map 新窗口时无条件给焦点）。
    另外没有给 vidow 自己启动的子进程发 `XDG_ACTIVATION_TOKEN`
24. foreign-toplevel 的 `set_maximized` 请求没接（vidow 本来就不支持最大化），
    任务栏上的最大化按钮点了没反应
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

## 下一步建议顺序

1. 浮动窗口的八向缩放边缘 + 双击标题栏最大化
2. layer surface 的装饰与模糊（状态栏/启动器也想要圆角）
3. workspace / 多显示器列组
4. X11 的 `_NET_WM_STATE`（最大化/最小化）与 `_NET_WM_STRUT`（面板预留），
   以及按屏幕边界夹菜单的客户端（把 X 屏幕也改成 `逻辑 × S`）
5. 装 `xdg-desktop-portal-wlr`，把 portal 的 ScreenCast / Screenshot 接到 vidow 已有的
   screencopy 上（`vidow-portals.conf` 里留了位置）
