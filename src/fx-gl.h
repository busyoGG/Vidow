/* fx 的 GL 绘制原语（实现在 fx-gl.c）。
 *
 * 上层只拿这几样拼自己那一帧：开一块离屏目标 → 绑纹理 / 画四边形 / 跑装饰
 * 着色器 → 收尾。所有 GL 状态（FBO、程序、纹理单元）都封在这一层里，别处
 * 不要直接 gl* 调用 —— 以前 kawase 那条自定义 FBO 路径把 wlroots render pass
 * 的「当前绑定」弄脏过一次，查了两天才定位。 */
#ifndef VIDOW_FX_GL_H
#define VIDOW_FX_GL_H

#include "server.h"

#include <stdbool.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <wlr/types/wlr_buffer.h>

struct wlr_egl;

/* 把 dst 绑成渲染目标，返回它的 FBO。rect 非空时只清那一块（局部重跑）。 */
GLuint begin_pass_rect(struct vidow_server *server, struct wlr_buffer *dst,
	int w, int h, const struct wlr_box *rect, struct wlr_egl **egl);
GLuint begin_pass(struct vidow_server *server, struct wlr_buffer *dst,
	int w, int h, struct wlr_egl **egl);
void end_pass(struct wlr_egl *egl);

/* 绑一张纹理到 unit，顺手设好线性过滤。 */
void bind_tex(GLuint unit, GLenum target, GLuint id);
/* 铺满整个目标的四边形（用 program 的 a_pos）。 */
void draw_quad(unsigned int program);

/* 客户端缓冲的「真正内容」：wlr_client_buffer 的 source（dmabuf 客户端另建的
 * 那张）而不是它自己那张旧贴图。 */
struct wlr_buffer *render_source(struct wlr_buffer *buffer);
/* 等客户端缓冲的 timeline 就绪（false = 这一帧还没有内容可抓）。 */
bool client_buffer_wait(struct vidow_server *server,
	struct wlr_scene_buffer *scene_buffer);
/* 场景节点这一帧能采到的贴图（借或新建，*owned 表示要不要销毁）。 */
struct wlr_texture *client_scene_texture(struct vidow_server *server,
	struct wlr_scene_buffer *scene_buffer, bool *owned);

/* VIDOW_FX_DUMP 探针：把这帧画好的目标倒进 /tmp。 */
void dump_target(struct vidow_server *server, struct vidow_fx_target *fx,
	struct wlr_buffer *target);

#endif
