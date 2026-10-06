# UI 元素树输出：可行性调研（vidow / AT-SPI2）

调研时间：2026-10-06。**状态：只记录，不实现**（结论是"这样做功能不完整"）。
本文档所有"实测"条目都在本机跑过，命令见 §11，可重跑。

**TL;DR**：合成器**拿不到**应用的控件树——Wayland 协议里只有 `wl_surface`，没有角色、
没有文本、没有控件语义，wlroots 0.20.2 的公开头文件里也没有任何无障碍相关接口（0 处）。
控件树只有一条路：**AT-SPI2**（Linux 无障碍总线，D-Bus）。实测这条路上：

- **能拿到**：角色 + 名字 + 矩形，连 Chromium 的 web DOM 无障碍树都能读到
  （`heading 'Hi'` / `button 'Login'` / `entry` / `link 'Docs'`）。
- **坐标是窗口相对的**：把窗口从 `(6,6)` 挪到 `(300,150)`，AT-SPI 报的坐标**一个像素没变**。
  → 全局坐标 = AT-SPI 窗口内偏移 **+** 合成器知道的窗口屏幕位置。**这后半截只有合成器能给，
  也是 vidow 在这个方案里唯一不可替代的价值。**
- **覆盖率不完整**：实测会话总线上只有 3 个应用，Vivaldi / Discord / QQ（Chromium 系）
  **一个都不在**。Chromium 系默认关无障碍，实测要 `ACCESSIBILITY_ENABLED=1` 才上总线
  （`--force-renderer-accessibility` 单独跑的那次**不够**）。纯自绘的 Wayland 应用
  （游戏、SDL、部分 Flutter/Rust GUI）**永远拿不到**。
- **操作路径没验证成功**：`Atspi.Action.do_action()` 返回 `True`，但 checkbox 的 `CHECKED`
  状态没变。要操作得先查清楚，或退回合成输入（vidow 已有 `wlr_virtual_keyboard_manager_v1`
  / `wlr_virtual_pointer_manager_v1`）。

所以"**所有**应用的 UI 元素树输出"这个目标，靠 AT-SPI 单独完不成；缺口和补齐路线见 §0、§8。

---

## 0. 「功能不完整」具体缺在哪

| # | 缺口 | 影响 | 现状 |
|---|------|------|------|
| 1 | 应用必须**实现了无障碍**才有树 | 游戏、SDL、纯自绘 GUI = 0 覆盖 | 无法绕过（协议层就不存在） |
| 2 | Chromium/Electron 系**默认不上总线** | 浏览器、Discord、QQ、VSCode 类全缺 | 可用环境变量/启动参数补（§5） |
| 3 | Qt / Firefox / Java / Wine **未实测** | 覆盖率未知 | 待测（§13） |
| 4 | 沙箱应用（Flatpak）需放行 `org.a11y.Bus` | 沙箱内应用全缺 | 需按应用放行 |
| 5 | AT-SPI **坐标是窗口相对的** | 单靠 AT-SPI 拿不到屏幕坐标 | 合成器补偏移（vidow 能做） |
| 6 | 无窗口的应用级树**合并**逻辑 | 树是散的，没有"一个桌面一棵树" | 侧车要做（§9） |
| 7 | 每个属性一次 D-Bus round-trip | 整树 dump 慢，实时性差 | 需缓存 + 事件订阅（§10） |
| 8 | **操作**路径未验证 | 只能"看"不能"点" | 待验证 / 退回合成输入 |
| 9 | a11y 总线**会话级无鉴权** | 谁都能读全部 UI 文本并触发动作 | 无法避免，需知情 |

**结论**：AT-SPI 给的是"**能读的应用里，控件级的树**"；合成器表面树给的是
"**所有应用，但只有窗口级的矩形**"。两者都做（§8 的 A+B+C）才是完整方案，缺一不可。

---

## 1. 合成器为什么给不出 UI 元素树

- Wayland 的核心对象只有 `wl_surface` / `wl_subsurface` / `xdg_toplevel` / `xdg_popup`
  / layer-shell。它们描述**几何和角色**（toplevel / popup / layer），没有控件语义：
  没有"按钮"、没有"输入框"、没有文本内容。
- 合成器手里只有 buffer（像素）+ 输入区域 + 协议角色。控件语义属于**无障碍域**，
  不属于合成器域——这是设计上的分层，不是实现缺失。
- 证据：wlroots 0.20.2 公开头文件里搜 `atspi` / `atk` / `accessib` → **0 处**
  （`grep -ril 'atspi\|atk\|accessib' /usr/include/wlr/`）；vidow 的 `src/`、`tools/`
  里同样 0 处。
- 所以合成器能给的最多是**表面树**：toplevel / popup / subsurface / layer-shell 的
  层级 + 几何 + pid（§8 路线 A）。这是"窗口级识别"，不是"UI 元素树"。

---

## 2. 唯一的路：AT-SPI2

- 位置：会话级 D-Bus 总线（`org.a11y.Bus` → 本机 `unix:path=/run/user/1000/at-spi/bus_0`）。
- 谁实现：工具包（GTK 的 atk-bridge、Qt 的 accessibility、Chromium 的 AXPlatformNode、
  Gecko、java-atk-wrapper…）把**自己的控件树**注册上去。应用不实现就没有。
- 本机现状（实测）：

  | 项 | 值 |
  |----|----|
  | 发行版 | EndeavourOS |
  | `at-spi2-core` | 2.60.6-1.1（`libatk-bridge-2.0.so.0` 也由它提供） |
  | a11y 总线 | `/run/user/1000/at-spi/bus_0` 在跑 |
  | systemd user unit | `at-spi-dbus-bus.service` = `active` |
  | 总线可激活 | `org.a11y.Bus` 在 `ListActivatableNames` 里 |
  | Python 绑定 | `gi.repository.Atspi` 可用（**没有** `pyatspi`，不要用那个名字） |

- 好消息：**vidow 侧不需要任何依赖**。读树的是另一个进程（侧车），vidow 只需要多暴露
  一点几何信息。给 vidow 引 GLib / D-Bus 是没必要的（它现在只依赖 wlroots + wayland +
  pixman 这一档）。

---

## 3. 实测：能拿到什么

在 headless vidow 里跑 `chromium --app='data:text/html,<h1>Hi</h1><button>Login</button><input value=x><a>Docs</a>'`
（`ACCESSIBILITY_ENABLED=1` + `--force-renderer-accessibility`），dump AT-SPI 树：

```
application    'Chromium'                (0,0,0x0)
  frame        'data:text/html,<h1>Hi<'  (0,0,634x708)
    panel      ''                        (0,0,634x708)
      button   '最小化'                    (0,0,0x0)     ← Chromium 自己的窗口按钮
      button   '最大化'                    (0,0,0x0)
      button   '恢复'                      (0,0,0x0)
      button   '关闭'                      (0,0,0x0)
      entry    '地址和搜索栏'                (0,0,0x0)
      heading  'Hi'                       (8,8,618x37)  ← 网页内容
        static 'Hi'                       (8,8,34x36)
      button   'Login'                    (8,66,53x22)  ← 网页内容
      entry    ''                         (60,66,188x22)
      link     'Docs'                     (247,67,34x18)
        static 'Docs'                     (247,67,34x18)
```

GTK3 应用（Lutris）也一样上总线：

```
application  'net.lutris.Lutris'
  frame      ''        (0,0,946x1019)
    panel    ''        (0,0,946x47)
    panel    ''        (0,47,946x972)
```

要点：

1. **角色 + 名字 + 矩形**都有 → 足以做 UI 识别（找按钮、找输入框、读标签文本）。
2. **文本内容在 `name` 里**；`entry` 的当前值要用 `Text` / `EditableText` 接口取
   （`name` 对输入框是标签，不是值）。
3. **名字是本地化的**（`最小化`/`最大化`/`恢复`/`关闭`、`地址和搜索栏`）→ 靠名字做匹配
   不可靠，**要用角色 + 几何 + 层级**。
4. **`0x0` 和 `-2147483648`（`G_MININT`）的 extents** 很常见：前者是"窗口按钮/工具栏项
   这类非布局节点"，后者是"未实现/不可见"。**不能把 extents 当"有像素"用**，要过滤。
5. 树深度可观：这次 dump 走了 196 个节点，只截到第 2 层 + 关心的角色。

---

## 4. 实测：坐标是窗口相对的（本方案的关键）

**实验**：headless vidow 里放一个 Chromium 窗口，`toggle-floating` 后
`vidowmsg set-window-position 300 150 --id 1`，再读同一批节点的 AT-SPI extents。

| | vidow IPC 报的窗口几何 | AT-SPI `Hi` | AT-SPI `Login` |
|---|---|---|---|
| 移动前 | `{x:6, y:6, 634x708}` | `(8,8,618x37)` | `(8,66,53x22)` |
| 移动后 | `{x:300, y:150, 634x708}` | `(8,8,618x37)` | `(8,66,53x22)` |

**窗口移动了 294/144 像素，AT-SPI 的坐标一个像素没动。** 原因很清楚：Wayland 客户端
不知道自己在屏幕上的位置，工具包只能按"窗口左上角 = 0,0"报。

→ 所以：

```
全局坐标 = AT-SPI 的窗口内偏移 + 合成器知道的窗口屏幕位置
```

**这是合成器在整个方案里唯一不可替代的贡献。** 反过来也说明：光有 AT-SPI 侧车不够，
必须和 vidow 的窗口几何对齐（§9）。

（未验证：GTK 在 **X11** 下是否报真·屏幕坐标——X11 客户端知道自己在哪。这条会影响
Xwayland 窗口的合并逻辑，见 §13。）

---

## 5. 实测：覆盖率（"所有应用"做不到）

**实测当时的会话**：

| 应用 | 类型 | 在 a11y 总线上？ |
|------|------|------------------|
| `xdg-desktop-portal-gtk` | GTK3 | ✅（0 个子节点） |
| `sunshine` | ? | ✅（0 个子节点） |
| `net.lutris.Lutris` | GTK3 (Python) | ✅（有树） |
| `vivaldi-bin` | Chromium 系 | ❌ |
| `electron43`（×2） | Electron | ❌ |
| Discord | Electron | ❌ |
| QQ | Electron | ❌ |

**总线上一共 3 个应用，实际在跑的 GUI 应用远多于此。** 结论：

1. **GTK 应用自己就上总线**（本机 a11y 总线已就绪，无需任何环境变量）。
2. **Chromium / Electron 系默认不上**。实测三种组合（每次全新 `--user-data-dir`）：

   | 启动条件 | 上总线？ |
   |----------|----------|
   | 只有 `--force-renderer-accessibility` | ❌（22 s 内没上） |
   | 只有 `ACCESSIBILITY_ENABLED=1` | ✅ |
   | 两个都设 | ✅（1 s 内） |

   ⚠️ 每种组合只跑了一次，"flag 单独不够"这条**要再验一次**（§13）。保守做法：两个都设。
   已实测 Chromium **不直接链接** `libatk`/`libatk-bridge`（`ldd` 为空），它是 `dlopen`
   的——所以"库在不在"不是判据，**开关**才是。
3. **纯自绘应用永远拿不到**：没有 atk-bridge 就没有树。这是协议层的缺口，不是配置问题。
4. 沙箱（Flatpak）要放行 `org.a11y.Bus`，否则沙箱内应用同样缺席。

---

## 6. 实测：关联键（pid 现成）

| 来源 | 字段 | 实测值 |
|------|------|--------|
| vidow `windows` IPC | `pid` | `2364924` |
| AT-SPI 应用节点 | `get_process_id()` | `2364924` |

**相等。** 所以关联用 **pid**，比 `app_id` / `title` 稳得多（`--app=data:` 那种窗口
`app_id` 是一长串 URL 变形，`title` 就是 URL）。

vidow 侧 `pid` 的来源（已实现，`src/ipcstate.c:293-306`）：

- xdg：`wl_client_get_credentials(view->xdg_toplevel->base->client->client, &pid, …)`
- X11：`view->x11->pid`

**待解**：Chromium 这类多窗口共用一个 pid（浏览器进程唯一）。消歧要用尺寸 + 顺序，
或让侧车用 AT-SPI 的 `frame` 节点与 vidow 的窗口几何做匹配。

---

## 7. 实测：操作路径（未成功）

AT-SPI 除了读，还有 `Action` 接口（"按"某个控件），以及 `EditableText`（直接设文本）
——比合成鼠标/键盘输入准得多。实测：

| 项 | 结果 |
|----|------|
| 正确的计数 API | `acc.get_n_actions()`（**不是** `get_action_count`，GI 绑定里没这个名） |
| 模块级函数 | `Atspi.Action.do_action` / `get_action_name` / `get_action_description` / `get_action_key_binding` / `get_localized_name` |
| 对象上的接口方法 | `do_action` / `get_action` / `get_action_iface` / `get_n_actions` / `is_action` |
| `Atspi.Action.do_action(checkbox, 0)` | 返回 `True` |
| 之后读 checkbox 的 `CHECKED` 状态 | **`False` → `False`（没生效）** |

**所以"能点"这件事没被证实。** 可能是调用方式不对（`get_action_iface()` 再调），可能是
Chromium 的 Action 实现有前置条件，也可能只是这次实验的观测点不好。要用之前先查清；
兜底是合成输入——vidow 已经有 `wlr_virtual_keyboard_manager_v1` /
`wlr_virtual_pointer_manager_v1`（`src/input.c:306`、`src/input.c:576`），以及
`vidowmsg move-cursor x y`。

---

## 8. 补齐路线（按覆盖 × 成本）

| 路线 | 覆盖 | 内容 | 成本 |
|------|------|------|------|
| **A. 表面树** | 100% | vidow 自己输出 toplevel/popup/subsurface/layer-shell 的层级 + 几何 + pid（只有窗口级，无控件语义） | 2–3 小时 |
| **B. AT-SPI 侧车** | 看应用 | `vidow-a11y` 进程读总线，输出控件级树 | ~1 天 |
| **C. A+B 合并** | A∪B | 全局坐标（A 的窗口位置 + B 的窗口内偏移）、pid 关联、统一 JSON | ~半天 |
| **D. 像素兜底** | 100%（不精确） | 截图 + OCR / 视觉模型，补 A/B 都拿不到的窗口内部 | 取决于模型，慢 |
| **E. 操作** | — | AT-SPI `Action`/`EditableText`（未验证）或 vidow 合成输入（已有） | 半天（含验证） |

**A+B+C ≈ 2 天**，得到"能读的应用有控件级树，其余至少到窗口级"——仍然不是"所有应用的
UI 元素树"。要真正逼近"所有"，D 是唯一兜底，但它不是树。

---

## 9. 架构草案

```
        ┌───────────────┐   IPC（几何 / pid）   ┌──────────────┐
        │     vidow     │ ───────────────────▶ │  vidow-a11y  │
        │  (合成器)      │ ◀─────────────────── │  (侧车)       │
        └───────────────┘   合并后的统一 JSON    └──────┬───────┘
                 │                                     │ AT-SPI（D-Bus）
                 │ 表面树（A）                          ▼
                 ▼                            ┌──────────────────┐
            窗口级矩形                        │ 会话 a11y 总线    │
                                             │ GTK / Chromium / │
                                             │ Qt / Gecko …     │
                                             └──────────────────┘
```

- **vidow 侧**：新增 `surfaces` IPC（表面树 + 几何 + pid）。复用已有的 `windows`
  （`pid` / `geometry` / `app_id` / `title` / `minimized` / `workspace` 都在）。
- **侧车**：Python + `gi.repository.Atspi`（本机已验证可用）。不给 vidow 引依赖。
  职责：连总线 → 按 pid 关联 vidow 窗口 → 把窗口内偏移加窗口屏幕坐标 → 输出 JSON。
- **输出草案**：

```json
{
  "window": { "id": 1, "pid": 2364924, "app_id": "chromium", "title": "…",
              "geometry": { "x": 300, "y": 150, "width": 634, "height": 708 },
              "output": "HEADLESS-1", "workspace": "", "minimized": false },
  "source": "atspi",
  "nodes": [
    { "role": "heading", "name": "Hi",    "rect": { "x": 308, "y": 158, "width": 618, "height": 37 } },
    { "role": "button",  "name": "Login", "rect": { "x": 308, "y": 216, "width": 53,  "height": 22 },
      "actions": ["click"] },
    { "role": "entry",   "name": "",      "rect": { "x": 360, "y": 216, "width": 188, "height": 22 },
      "text": "x" }
  ]
}
```

（上面的 `rect` 是"窗口内偏移 + 窗口位置"之后的**全局坐标**，这才是可用的形态。）

---

## 10. 性能与安全

- **性能**：AT-SPI 每个属性是一次 D-Bus round-trip。整树 dump（本次 196 节点）要几百次
  调用，**不可能每帧做**。要做：缓存树 + 订阅事件
  （`object:children-changed`、`object:state-changed`、`object:text-changed`）+ 只读关心的角色。
- **安全**：a11y 总线是**会话级、无鉴权**的——同一会话里任何进程都能读走**所有**应用的
  UI 文本（密码框除外，那类节点会被屏蔽），并触发控件动作。做成常驻服务 = 扩大了这个面。
  这是方案自带的性质，不是实现缺陷，但要有意识地接受。

---

## 11. 怎么复现这次调研

全程在 headless vidow 里，不碰桌面：

```bash
cd ~/Dev/Vidow
RUNDIR=$(mktemp -d /tmp/a11y-XXXXXX)
HOME=$HOME XDG_RUNTIME_DIR="$RUNDIR" WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
  WLR_LIBINPUT_NO_DEVICES=1 VIDOW_LOG=info ./build/vidow > "$RUNDIR/vidow.log" 2>&1 &
WD=$(grep -o 'WAYLAND_DISPLAY=[a-z0-9-]*' "$RUNDIR/vidow.log" | head -1 | cut -d= -f2)

URL=$(python3 -c "import urllib.parse;print('data:text/html,'+urllib.parse.quote(
  '<h1>Hi</h1><button onclick=\"document.title=%27CLICKED%27\">Login</button><input value=x>', safe=''))")
ACCESSIBILITY_ENABLED=1 WAYLAND_DISPLAY="$WD" XDG_RUNTIME_DIR="$RUNDIR" \
  chromium --ozone-platform=wayland --force-renderer-accessibility --app="$URL" \
  --user-data-dir="$RUNDIR/uc" --no-first-run --disable-gpu --no-default-browser-check \
  >/dev/null 2>&1 &
```

看总线上有哪些应用（**20 秒，随时可跑**）：

```bash
python3 -c "import gi;gi.require_version('Atspi','2.0');from gi.repository import Atspi;Atspi.init();d=Atspi.get_desktop(0);print([d.get_child_at_index(i).get_name() for i in range(d.get_child_count())])"
```

看 vidow 侧的窗口几何 / pid / 移动窗口（坐标实验）：

```bash
V(){ WAYLAND_DISPLAY="$WD" XDG_RUNTIME_DIR="$RUNDIR" ./build/vidowmsg "$@"; }
V windows | python3 -c "import sys,json;w=json.load(sys.stdin)[0];print(w['id'],w['pid'],w['geometry'])"
V toggle-floating --id 1
V set-window-position 300 150 --id 1     # 平铺窗口会拒绝：位置由布局定
```

dump 树 + 读 extents（`CoordType.SCREEN`）：

```python
import gi; gi.require_version('Atspi','2.0'); from gi.repository import Atspi
Atspi.init(); d = Atspi.get_desktop(0)
def walk(n, dep=0):
    if dep > 10: return
    try:
        e = n.get_extents(Atspi.CoordType.SCREEN)
        print('  '*dep, n.get_role_name(), repr(n.get_name()), (e.x, e.y, e.width, e.height))
    except Exception: pass
    for i in range(n.get_child_count()):
        try: walk(n.get_child_at_index(i), dep+1)
        except Exception: pass
for i in range(d.get_child_count()):
    a = d.get_child_at_index(i)
    if 'hromium' in a.get_name(): walk(a)
```

收尾：`kill` 两个后台进程、`rm -rf "$RUNDIR"`（探针脚本都带 `trap` 自清理）。

---

## 12. 相关代码位置（vidow 侧）

| 文件 | 位置 | 和这件事的关系 |
|------|------|----------------|
| `src/ipcstate.c` | `293-306` | `pid` 的来源（xdg `wl_client_get_credentials` / x11 `x11->pid`）——关联键 |
| `src/ipcstate.c` | 窗口 JSON 其余字段 | `geometry` / `app_id` / `title` / `output` / `workspace` / `minimized` |
| `src/ipc.c` | `1747` | `windows` 命令；新增 `surfaces` 挂这里 |
| `src/ipc.c` | `1461` | `pick_window`（`vidow_view_at`）——已有的命中测试，可复用作"光标下是什么" |
| `src/input.c` | `306` / `576` | 虚拟键盘 / 虚拟指针管理器——操作路径的兜底 |
| `tools/vidowmsg.c` | — | IPC 命令行；`--id` / `--app-id` / `--title` 指认目标 |
| `docs/hdr-feasibility.md` | — | 同格式的调研存档，本文档照它的写法 |

---

## 13. 未验证 / 不确定清单

1. **Qt5/6、Firefox、Java、Wine** 的覆盖率——本机没测（Qt 大概率要
   `QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1`，Gecko 有 a11y 但没验）。
2. **`--force-renderer-accessibility` 单独够不够**——只跑了一次（结果：不够）。
3. **Action 为什么不生效**——`do_action` 返回 `True` 但状态没变；要试
   `get_action_iface()`、换控件类型、换观测点（`EditableText.set_text_content`）。
4. **GTK 在 X11 下是否报真·屏幕坐标**——影响 Xwayland 窗口的合并逻辑（若报真屏幕坐标，
   这类窗口不能加偏移，要识别出来）。
5. **同 pid 多窗口的消歧**——尺寸匹配在"两个同尺寸窗口"时会歧义。
6. **大树的延迟实测**——只有节点数（196），没测端到端耗时。
7. **`AT-SPI` 的 `Text` 接口能否稳定读出输入框当前值**（本次只验证了 `name`）。
8. **合成器表面树的具体形状**——vidow 现在没暴露；`wlr_scene` 节点树是内部结构，
   要新写一份遍历（路线 A 的实际工作量点）。
