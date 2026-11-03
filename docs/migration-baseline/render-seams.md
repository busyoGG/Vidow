# 渲染层接缝（第 1 步的输入）

从基线量出来的现状：GL 调用不是均匀分布的，集中在三个文件。第 1 步的边界就按这个切。

## 现在谁在摸 GL

| 文件 | `gl*` 调用 | 直接摸 GLES2 内部 | 该不该进 `gpu.h` |
| --- | ---: | ---: | --- |
| `src/fx-gl.c` | 178 | 18 | 是（全部） |
| `src/fx.c` | 64 | 10 | 是（纹理导入 / 模糊抓取 / 直方图） |
| `src/capture.c` | 4 | 0 | 是（抓帧 pass） |
| `src/screencopy.c` | 4 | 0 | 是（读像素） |
| `src/fx-popup.c` | 0 | 2 | 只取 `wlr_gles2_texture_attribs`，换成 `gpu_texture` 句柄 |

`fx-gl.h` 现在把这堆半成品直接暴露给调用方（`begin_pass_rect` / `bind_tex` /
`draw_quad` / `GLuint` 参数都露在外面），这是要收掉的第一层。

## `gpu.h` 只暴露这些（草案）

```c
/* 设备 / 会话 */
struct gpu;
struct gpu *gpu_create(struct wlr_renderer *r, struct wlr_allocator *a);
void gpu_destroy(struct gpu *);

/* 表面：一次渲染的输入 / 输出 */
struct gpu_target;                       /* 交换链缓冲 / 离屏 RT */
struct gpu_target *gpu_target_import(struct gpu *, struct wlr_buffer *);
struct gpu_target *gpu_target_create(struct gpu *, int w, int h, enum gpu_fmt);

/* 一次全屏 / 矩形绘制 */
struct gpu_pass *gpu_pass_begin(struct gpu *, struct gpu_target *dst);
void gpu_pass_set_pipeline(struct gpu_pass *, struct gpu_pipeline *);
void gpu_pass_bind_texture(struct gpu_pass *, int unit, struct gpu_texture *);
void gpu_pass_uniform1f(struct gpu_pass *, const char *name, float);
void gpu_pass_uniform3f(struct gpu_pass *, const char *name, float, float, float);
void gpu_pass_uniform_mat3(struct gpu_pass *, const char *name, const float *);
void gpu_pass_draw_quad(struct gpu_pass *);
void gpu_pass_end(struct gpu_pass *);

/* 着色器：源码是 Vulkan GLSL，GL 后端由构建期产物回退 */
struct gpu_pipeline *gpu_pipeline_get(struct gpu *, const char *name);
struct gpu_texture  *gpu_texture_from_buffer(struct gpu *, struct wlr_buffer *);
```

原则：

1. **不出现 `GLuint` / `VkImage` / `GLenum`**，调用方拿不到后端类型。
2. uniform 按名字传（GL 走 `glGetUniformLocation` 缓存，Vulkan 走反射表）——
   这样两套后端能共用同一份 fx 代码，不用为每个后端写一遍 uniform 偏移。
3. pipeline 用**名字**查，不暴露 handle；名字就是着色器资产的文件名。
4. 纹理只有「导入」和「离屏创建」两个入口，同步语义由后端负责（GL 的隐式等待 /
   Vulkan 的 fence + layout transition）。
5. `gpu_pass_end` 才提交（GL 是 flush，Vulkan 是 submit + 等待可复用）。

## 第 1 步的验收

- `src/` 里除了 `gpu-gl.c`（和将来的 `gpu-vk.c`）不再出现 `gl*` 调用：
  `grep -rn '\bgl[A-Z]' src/*.c` 只剩 `gpu-*.c`。
- 像素零变化：`test-grade.sh`、`test-gradelut.sh`、`test-blur*`、`test-scanout.sh`
  全过。
- `VIDOW_RENDERER=gl` 显式指定也照旧。

## 第 2 步的输入

`fx-gl.c` 里的内联 GLSL：（见 `code-metrics.txt` 的清单）

- `vertex_src` / `kawase_vertex_src` / `mesh_vertex_src` / `mesh_blur_vertex_src` /
  `post_vertex_src`
- `decor_fragment_src`、`kawase_down_src` / `kawase_up_src`（每级前缀拼接）、
  `mesh_fragment_src` / `mesh_blur_fragment_src`、`post_fragment_src`

第 1 步先原样搬进 `gpu-gl.c`（字符串不动），第 2 步再资产化 —— 两步分开，
出问题时能一眼看出是「搬家搬坏了」还是「着色器转换坏了」。
