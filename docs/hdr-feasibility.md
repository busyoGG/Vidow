# HDR 可行性调研（vidow / wlroots 0.20.2）

调研时间：2026-10-06。结论先写，证据在后；每一条都标了 wlroots 源码位置，方便下次核对。

**TL;DR**：wlroots 0.20.2 的 DRM 后端 **HDR 输出通路是齐的**（EDID 探测 → image
description → 内核 `HDR_OUTPUT_METADATA` / `Colorspace` / `max bpc`）。唯一挡路的是
**GLES2 渲染器不认 color transform**（`features.output_color_transform = false`），而
vidow 的自定义着色器管线（`fx.c`，2330 行原生 GL）正绑死在 GLES2 上。所以：

- **整屏 HDR 容器**（SDR 内容统一编码成 PQ/BT2020 输出）：可做，vidow 自己出最后一趟
  编码，约 1–2 天。
- **客户端 HDR 内容**（mpv 的 PQ 视频、游戏）：这套栈上做不了（GLES2 也没有
  `input_color_transform`），要么等上游，要么整个渲染器换 Vulkan（1–2 周量级）。
- 硬门槛是那块屏**自己报没报** BT2020 + ST2084_PQ（见文末「硬件门槛」）。

---

## 0. 更正记录

本次调研推翻了 2026-10-06 早先版本 README 里的一段说法：

> ~~「wlroots 0.20.2 里没有任何后端会填 `supported_primaries` / `supported_transfer_functions`，
> DRM 后端全文没有 colorspace / `HDR_OUTPUT_METADATA` 相关的代码」~~

**这是错的。** 那是 headless 后端的现象，被当成了全部后端。DRM 后端两者都填、两者都用
（证据见 §2）。错误的依据来自一次只看 headless 的实测 + 一次范围不对的 grep；教训是
「某个后端不支持」不能写成「wlroots 不支持」。

已经改掉的地方：`README.md` 的 HDR 段（现在指向本文）、`vidowmsg outputs` 新增
`primaries` / `transfer_functions` / `hdr_capable`（提交 `b0eb182`）。

---

## 1. 现状：vidow 今天已经能用的

- `vidow.output{ depth = 10 }` → 输出缓冲换成 `DRM_FORMAT_XRGB2101010`
  （`src/output.c` 的 `vidow_output_set_depth`，先查 `wlr_output_get_primary_formats`
  再 commit）。这是 10 位 **SDR**，不是 HDR，但是 HDR 的必要前置：DRM 后端的
  `max bpc` 会按缓冲格式自动选 10（见 §2）。
- `vidowmsg outputs` 报 `depth` / `render_format` / `vrr_supported` / 以及新增的
  `primaries` / `transfer_functions` / `hdr_capable`。

没有的：image description（primaries / 传输函数 / mastering 亮度）从来没被设过，
`wlr_color_manager_v1` 全局也没创建。

---

## 2. 上游证据：DRM 后端这条链是通的（wlroots 0.20.2）

以下全部来自 0.20.2 源码树（获取方式见 §8）。

### 2.1 EDID 探测 → 填 `supported_primaries` / `supported_transfer_functions`

`backend/drm/util.c:97-106`（`parse_edid()`，由 `backend/drm/drm.c:1742` 在连接器拿到
EDID 时调用）：

```c
const struct di_supported_signal_colorimetry *colorimetry =
	di_info_get_supported_signal_colorimetry(info);
bool has_bt2020 = colorimetry->bt2020_cycc || colorimetry->bt2020_ycc || colorimetry->bt2020_rgb;
if (conn->props.colorspace != 0 && has_bt2020) {
	output->supported_primaries |= WLR_COLOR_NAMED_PRIMARIES_BT2020;
}

const struct di_hdr_static_metadata *hdr_static_metadata = di_info_get_hdr_static_metadata(info);
if (conn->props.hdr_output_metadata != 0 && hdr_static_metadata->type1 && hdr_static_metadata->pq) {
	output->supported_transfer_functions |= WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ;
}
```

要点：**两个条件都是「连接器有对应属性」且「EDID 自己声明支持」**。缺一个就什么都不填，
于是 image description 会被 `types/output/output.c:700-712` 的校验拒掉，日志是
`Unsupported image description primaries` / `... transfer function`。

### 2.2 image description → 真的写进内核

`backend/drm/atomic.c`：

- `create_hdr_output_metadata_blob()`（:207-243）用 `drmModeCreatePropertyBlob()` 造
  `HDR_OUTPUT_METADATA` blob：`hdmi_metadata_type1.eotf`、display primaries、
  `max_display_mastering_luminance`、`min_display_mastering_luminance`（注意单位换算
  `× 0.0001`）、`max_cll` / `max_fall`。`img_desc == NULL` 时 blob = 0（= 关掉）。
- `convert_primaries_to_colorspace()`（:250-258）：BT2020 → `9`（`BT2020_RGB`），0 →
  `0`（Default）。
- `max_bpc_for_format()`（:260-275）：`XRGB2101010` 等 → 10 位，`*16161616F` / `*16161616`
  → 16 位，其余 8 位。
- 提交时（:505-517）把 `max bpc`（仅 modeset 时）、`Colorspace`、`HDR_OUTPUT_METADATA`
  加进 atomic request；`drm_atomic_connector_apply_commit()`（:397-429）在提交成功后
  更新 `conn->colorspace` / `conn->hdr_output_metadata`，失败走 rollback（blob 不泄漏）。

### 2.3 wlr_scene 会按 image description 现算 color transform

`types/scene/wlr_scene.c:2243-2290`（`scene_output_combine_color_transforms()`）：

```c
wlr_color_primaries_transform_absolute_colorimetric(&primaries_srgb, &primaries, matrix);
/* 再乘 sRGB → 目标传输函数的亮度缩放 */
color_matrix = wlr_color_transform_init_matrix(matrix);
inv_eotf = wlr_color_transform_init_linear_to_inverse_eotf(img_desc->transfer_function);
```

算好的变换在 `wlr_scene.c:2523` 交给这一帧的 render pass
（`.color_transform = scene_output->combined_color_transform`）。

### 2.4 前提：必须是 atomic KMS

`backend/drm/drm.c:890-893`：

```c
if ((state->committed & WLR_OUTPUT_STATE_IMAGE_DESCRIPTION) &&
		conn->backend->iface != &atomic_iface) {
	wlr_log(WLR_DEBUG, "Image descriptions are only supported by the atomic interface");
	return false;
}
```

现代 GPU 都是 atomic（`drmModeAtomicCommit`），但老驱动 / 被强制 legacy 的情况下这条路直接关。

---

## 3. 真正的墙：GLES2 不认 color transform

`render/gles2/renderer.c:529`：

```c
renderer->wlr_renderer.features.output_color_transform = false;
```

- GLES2 的 `gles2_begin_buffer_pass()`（`render/gles2/renderer.c:241-266`）**完全不读**
  `options->color_transform` —— wlr_scene 算好的变换被丢掉。
- 只有 Vulkan 侧认：`render/vulkan/renderer.c:2532-2533` 把
  `input_color_transform` / `output_color_transform` 都设成 `true`，
  `render/vulkan/pass.c:266-275` 真正消费 `pass->color_transform`。
- `include/wlr/render/wlr_renderer.h:49-53` 是这两个 feature 的定义；全树只有
  `types/wlr_color_management_v1.c:1118,1143` 读 `input_color_transform`，而且只是**用来
  决定要不要对外声明支持**，不参与渲染。
- **master 上也还是 `false`**（2026-10-06 查的 `master/render/gles2/renderer.c:533`）。
  0.20.2 是当前最新 release（2026-07-07 打的 tag），所以这不是「等下一版就好」的事。

**假 HDR 长什么样**：给输出设了 PQ/BT2020 的 image description，但缓冲里还是 sRGB 编码的
像素（GLES2 没做变换）→ DRM 按 PQ 上屏 → 画面发灰、暗部塌。正因为这样 vidow 不给
`hdr = true` 这种开关：宁可不做，也不做一个「看起来开了」的假开关。

### 各后端能力矩阵（0.20.2）

| 后端 | `supported_primaries` / `supported_transfer_functions` | 备注 |
|---|---|---|
| DRM（atomic） | 由 EDID + 连接器属性决定（§2.1） | 唯一能真正上屏 HDR 元数据的 |
| DRM（legacy） | — | image description 直接拒（§2.4） |
| headless | 永远是 0 | 所以 headless 上任何 image description 都被拒 |
| wayland（嵌套） | 永远是 0 | 嵌套在 Hyprland 里测不了 HDR |

后果：**HDR 这条路无法在本机 headless 环境端到端验证**，只能验证「拒绝路径」和着色器数学，
真验证要靠真显卡 + 真 HDR 屏（§7）。

---

## 4. 方案 A（推荐）：vidow 自己出最后一趟编码

思路：绕开 GLES2 的 feature 缺失，由 vidow 自己做「SDR → PQ/BT2020」这一趟。

```
wlr_scene 渲染  ──►  离屏缓冲（XRGB2101010 或 FP16）
                        │
                        ▼  vidow 自己的全屏着色器：sRGB 解码 → linear → BT2020 矩阵 → PQ 编码
                     输出缓冲（DRM_FORMAT_XRGB2101010）
                        │
                        ▼  wlr_output_state_set_image_description(PQ, BT2020, mastering 亮度) + commit
                     DRM：HDR_OUTPUT_METADATA + Colorspace=BT2020_RGB + max bpc=10
```

能得到的：**整屏 HDR 容器** —— 所有 SDR 内容统一映射到 HDR 容器里（参考白取 ~203 cd/m²，
峰值亮度按配置）。屏幕色彩更饱和、可用亮度更高。

拿不到的：客户端真 HDR 内容（mpv 的 PQ 视频）。原因见 §6。

### 4.1 要做的事（按依赖顺序）

1. **配置**：`vidow.output{ hdr = true }`（+ 可选 `hdr_peak = 1000` / `hdr_ref_white = 203`）。
   落在 `src/outputconf.c` 的 `struct vidow_output_conf`、`src/script.c` 的 `l_output`
   解析与字段白名单、`src/output.c` 的 apply 里。
2. **image description + 位深**：`wlr_output_state_set_render_format(XRGB2101010)` +
   `wlr_output_state_set_image_description()`，提交前**先探测**：`supported_primaries &
   BT2020` 和 `supported_transfer_functions & ST2084_PQ` 都有才做，否则日志说清楚
   （和现在 VRR / depth 一样的「先问再试」风格，且每项单独 commit）。
3. **编码那一趟**：场景渲进离屏缓冲 → vidow 自己的全屏 pass 写进输出缓冲。
   `fx.c` 里已经有全部零件：`wlr_gles2_renderer_get_buffer_fbo`、自己的
   `glCreateShader` / program 管线、离屏缓冲（`output->backdrop` 就是这么用的）。
   着色器：`rgb → (sRGB EOTF 解码) → linear → 3x3 BT2020 矩阵 → PQ(ST2084) OETF`。
   可参考 wlroots 的 `render/color.c`（`wlr_color_transform_eval` /
   `wlr_color_transform_init_linear_to_inverse_eotf`）对齐公式，别自己拍。
4. **三处收尾（真正的麻烦）**：
   - **damage ring**：现在 `wlr_scene_output_commit()`（`src/output.c:69`）让 wlroots 拿输出
     缓冲转 damage ring；两段式之后最终缓冲不是 scene 渲的那个，得自己管（或退化成一趟全屏
     编码 + 全屏 damage）。`wlr_damage_ring_rotate_buffer` 的语义要求「这个 buffer 就是上一帧
     上屏的那个」。
   - **直扫（direct scanout）**：`types/scene/wlr_scene.c:2446` 只在「无 color transform、
     单节点」时才直扫；HDR 模式下必然全屏合成 —— 全屏视频时功耗会上去（而这正是最想省电的
     场景）。
   - **光标**：`types/output/cursor.c:526-533` 是 wlroots 自己处理光标的 color transform
     路径，vidow 的两段式里光标画在哪一趟要想清楚（跟着最后一趟编码走才颜色正确）。
5. **测试**：headless 只能验「拒绝路径」（没有 primaries → 日志说清楚、不崩、其他设置照常
   生效）；编码着色器的数学可以拿 `tools/` 里的小工具单独验（像 `scaletest` 那样）。

工作量：**1–2 天**（编码着色器半天；剩下全在第 4 条）。

---

## 5. 方案 B：换 Vulkan 渲染器

Vulkan 侧 feature 齐全（§3），也能做 per-surface 的输入变换。但 vidow 的自定义着色器管线
`src/fx.c`（2330 行）是**原生 GL**：`glCreateShader` / `link_program` /
`wlr_gles2_renderer_get_egl` / `wlr_gles2_renderer_get_buffer_fbo` / 自己的 FBO 与
uniform 管理（模糊的 Kawase 上下采样、装饰着色器、窗口开关动画都挂在上面）。
wlroots 0.20 的渲染器接口里**没有**通用的自定义着色器 API（`include/wlr/render/*.h` 里
没有 custom shader 入口），所以换 Vulkan = 把整条 FX 管线重写成 Vulkan。

工作量：**1–2 周**，且中途会丢现在的手写 shader 特性。只有在「一定要 HDR 内容」时才值得。

---

## 6. 方案 C（不做）：客户端 HDR 内容

要让 mpv / 游戏的真 HDR 缓冲正确显示，需要：

1. **接住客户端的声明**：创建 `wlr_color_manager_v1` 全局
   （`types/wlr_color_management_v1.c`，`wlr_color_manager_v1_options` 里要传 render intents
   至少含 perceptual、TF / primaries 列表）。
   **但注意**：`wlr_color_manager_v1_*_list_from_renderer()`（:1115-1155）在
   `input_color_transform == false` 时返回**空列表** —— GLES2 下 vidow 没法诚实地对外声明
   「我支持 PQ / BT2020」。硬塞列表 = 骗客户端把 HDR 内容发过来，然后显示错。
2. **转换**：把每个表面的 PQ/BT2020 采样成输出空间。wlr_scene 侧只有「整帧一个变换」，
   per-surface 的输入变换在 0.20.2 里**没有实现**（`input_color_transform` 只用于协议声明）。
   也就是说，就算用 Vulkan，也得 vidow 自己手写整帧渲染（`fx.c` 的 `backdrop_iter` 那种
   按 `wlr_scene_buffer` 遍历的路子 + 按表面声明的 TF 选 EOTF），等于放弃 wlr_scene 的
   剔除 / damage / 直扫。

结论：**不做**。等上游给 GLES2 补 input/output color transform，或走方案 B。

---

## 7. 硬件门槛与验证

### 7.1 先看这三个字段（1 分钟）

```bash
./build/vidowmsg outputs | python3 -c 'import json,sys
for o in json.load(sys.stdin):
    print(o["name"], "hdr_capable=%s" % o["hdr_capable"], o["primaries"], o["transfer_functions"])'
```

- 两个数组都空 → 这块屏（或这个后端）没声明 BT2020 + PQ，**HDR 是白费劲**，
  `depth = 10` 就是这条链上的终点。
- 报出 `bt2020` + `st2084_pq` → 方案 A 有戏。

（在真机 DRM 会话里跑；嵌套在 Hyprland 里的 wayland 后端永远是空的。）

### 7.2 真机验证清单（实现之后）

- [ ] `vidowmsg outputs` 的 `depth` = 10、`render_format` = `xrgb2101010`
- [ ] 日志里能看到 image description 被接受（不是 `Unsupported image description ...`）
- [ ] 显示器 OSD 显示 HDR 已开启（多数屏切 PQ 会有提示）
- [ ] 黑场不发灰、白场不塌；灰阶（0–100% 阶梯图）没有明显断带
- [ ] 驱动侧确认：`cat /sys/kernel/debug/dri/*/state` 或 `drm_info` 看
      `HDR_OUTPUT_METADATA` / `Colorspace` / `max bpc` 三个属性真的被设了
- [ ] 关掉 `hdr`（SIGHUP）能干净回退到 SDR，不需要重启

### 7.3 本机验证的边界（诚实说明）

本机（Hyprland 嵌套 / headless）**无法**验证 HDR 上屏。headless 后端 `supported_primaries`
永远是 0，任何 image description 都被拒 —— 能验的只有「拒绝时日志说清楚、不崩、别的设置
照常生效」这一半。真验证必须在真显卡 + 真 HDR 屏上做，做之前所有结论都只是「按源码推断」。

---

## 8. 怎么复现这次调研

github.com 在本机不通；**gitlab.freedesktop.org 通**（raw / archive / API 都行，API 搜索
需要认证）：

```bash
cd /tmp
curl -sS -o wlroots-0.20.2.tar.gz \
  https://gitlab.freedesktop.org/wlroots/wlroots/-/archive/0.20.2/wlroots-0.20.2.tar.gz
tar xzf wlroots-0.20.2.tar.gz && cd wlroots-0.20.2

grep -rn "supported_primaries\|supported_transfer_functions" --include=*.c .   # 谁填这两个位域
grep -rn "HDR_OUTPUT_METADATA\|colorspace\|max_bpc" --include=*.c --include=*.h .
grep -rn "output_color_transform\|input_color_transform" --include=*.c --include=*.h .
grep -rn "image_description\|color_transform" types/scene/wlr_scene.c
```

对照本机头文件（Arch 的 `wlroots0.20` 包）：`/usr/include/wlroots-0.20/wlr/`
（`types/wlr_output.h` 的 `supported_primaries` / `supported_transfer_functions` 注释、
`render/color.h` 的 primaries / TF 枚举、`types/wlr_color_management_v1.h`）。

master 的现状（判断「要不要等上游」）：`https://gitlab.freedesktop.org/wlroots/wlroots/-/raw/master/render/gles2/renderer.c`
→ 搜 `output_color_transform`，2026-10-06 时仍是 `false`。

---

## 9. 相关代码位置（vidow 侧）

| 位置 | 干什么 |
|---|---|
| `src/output.c` `vidow_output_set_depth()` | 今天唯一的「画质」项：8 / 10 位输出缓冲 |
| `src/output.c` `output_apply_conf_extra()` | 每项单独 commit 的模式（VRR / depth 的「先问再试」） |
| `src/outputconf.c` / `src/script.c` | `vidow.output{}` 的字段解析与白名单 |
| `src/ipcstate.c` `vidow_ipc_json_output()` | `primaries` / `transfer_functions` / `hdr_capable` |
| `src/fx.c` | 2330 行原生 GL 管线 —— 方案 A 的零件在这儿，方案 B 的债也在这儿 |
| `src/output.c:69` | `wlr_scene_output_commit()` —— 方案 A 要改的那个入口 |
| `README.md`「模式、刷新率、VRR、位深」一节 | 面向用户的说法（含 HDR 一段，指向本文） |

---

## 10. 未验证 / 不确定清单

- 用户的 DP-3 / DP-2 到底报不报 BT2020 + PQ（§7.1 一条命令就知道）。
- 真机上 10 位 + PQ 的实际观感与灰阶表现（本机没法验）。
- 方案 A 第 4 条的三个收尾（damage ring / 直扫 / 光标）实际有多脏 —— 现在只是按
  wlroots 的接口推断，没写过。
- wlroots 什么时候给 GLES2 补 output color transform（master 还没动）。
- `hdr_peak` / `hdr_ref_white` 取什么值合适：mastering 亮度可以照抄屏的 EDID（wlroots 填了
  `default_primaries`，但没有峰值亮度字段），也可能得让用户手填。
