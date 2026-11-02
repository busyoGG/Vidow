/* fx 的 GL 后端：着色器源码、程序构建、离屏缓冲 / 贴图、绘制原语。
 *
 * 这一层只认「缓冲、纹理、GL 状态」，不认窗口 / layer / 弹出物。上面三层
 * （fx-layer.c / fx-popup.c / fx.c 的窗口那一路）拿这里的东西拼自己那一帧：
 * begin_pass 开一块离屏目标 → bind_tex / draw_quad / decor shader 画 → end_pass。
 *
 * 和 wlroots 的 render pass 各管一半：场景合成走 wlr_render_pass，fx 自己的
 * kawase 金字塔和装饰着色器要控制 FBO 与采样，所以直接走 GLES2。 */
#include "server.h"
#include "fx-internal.h"
#include "fx-gl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <drm_fourcc.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

/* wlroots 0.20 exports this symbol but the Arch package ships no prototype. */
extern const struct wlr_drm_format_set *wlr_renderer_get_render_formats(
	struct wlr_renderer *r, uint32_t buffer_caps);

/* wlroots exports these helpers but wlroots-0.20 does not install their
 * unstable prototypes. Keep the declarations local, as with the render
 * format helper above. */
extern EGLSyncKHR wlr_egl_create_sync(struct wlr_egl *egl, int fence_fd);
extern void wlr_egl_destroy_sync(struct wlr_egl *egl, EGLSyncKHR sync);
extern bool wlr_egl_wait_sync(struct wlr_egl *egl, EGLSyncKHR sync);

#define MAX_SHADER_SIZE (256 * 1024)

/* ------------------------------------------------------------ GL plumbing */

static bool gl_make_current(struct vidow_server *server,
		struct wlr_egl **out_egl) {
	/* 先自己判一次渲染器类型，**不能**直接问 wlr_gles2_renderer_get_egl()：
	 * 那个函数第一行就是 assert(wlr_renderer_is_gles2(renderer))，渲染器不是
	 * GLES2（软件兜底的 pixman、或者显式 WLR_RENDERER=pixman）时它不是返回
	 * NULL，是**当场 abort**。core dump 里那条
	 *   #4 __assert_fail  #5 libwlroots+0x224c4  #6 libwlroots+0x2255f
	 *   #7 gl_make_current  #8 vidow_fx_flush  #9 vidow_output_render
	 * 就是这个（0x224c4 处那条断言的字面量是 wlr_renderer_is_gles2(wlr_renderer)，
	 * 文件 render/gles2/renderer.c，函数 gles2_get_renderer）。 */
	if (!wlr_renderer_is_gles2(server->renderer)) {
		return false;
	}
	struct wlr_egl *egl = wlr_gles2_renderer_get_egl(server->renderer);
	if (egl == NULL) {
		return false;
	}
	EGLDisplay display = wlr_egl_get_display(egl);
	EGLContext context = wlr_egl_get_context(egl);
	if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT) {
		return false;
	}
	/* wlroots creates a surfaceless context, so EGL_NO_SURFACE is correct. */
	if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
		wlr_log(WLR_ERROR, "fx: eglMakeCurrent failed (0x%x)", eglGetError());
		return false;
	}
	*out_egl = egl;
	return true;
}

static void gl_release(struct wlr_egl *egl) {
	EGLDisplay display = wlr_egl_get_display(egl);
	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

static GLuint compile(GLenum type, const char *src, const char *name) {
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);
	GLint ok = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (ok != GL_TRUE) {
		char log[2048];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		wlr_log(WLR_ERROR, "fx: %s shader '%s' failed to compile:\n%s",
			type == GL_VERTEX_SHADER ? "vertex" : "fragment", name, log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

/* The quad covers the whole target. a_pos is 0..1 with y growing downwards.
 * GL window row 0 is the first row of the dmabuf, which is the top row of a
 * Wayland buffer, so screen-down maps straight onto NDC-down. */
static const char *vertex_src =
	"attribute vec2 a_pos;\n"
	"varying vec2 v_uv;\n"
	"uniform float u_flip_y;\n"
	"void main() {\n"
	"	v_uv = vec2(a_pos.x, u_flip_y > 0.5 ? 1.0 - a_pos.y : a_pos.y);\n"
	"	gl_Position = vec4(a_pos.x * 2.0 - 1.0, a_pos.y * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

/* kawase 那两趟专用：顶点不但能盖住整块目标，还能只盖住目标里的一块（u_rect，
 * 目标像素）。整块跑的时候 u_rect = (0,0,w,h)，跟上面那个顶点着色器**逐像素等价**
 * —— 两条路共用同一份采样代码，「只重跑受损那一小块」（见 blur_chain_rect）才
 * 不用另写一套片元逻辑，也就不会出现「整块和局部算出来不一样」。 */
static const char *kawase_vertex_src =
	"attribute vec2 a_pos;\n"
	"varying vec2 v_uv;\n"
	"uniform vec4 u_rect;\n" /* 目标像素：x, y, w, h */
	"uniform vec2 u_dst;\n"  /* 目标尺寸 */
	"void main() {\n"
	"	vec2 p = u_rect.xy + a_pos * u_rect.zw;\n"
	"	v_uv = p / u_dst;\n"
	"	gl_Position = vec4(p / u_dst * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

/* Uniform set handed to user animation shaders. */
static const char *texture_prefix =
	"precision mediump float;\n"
	"uniform sampler2D u_texture;\n"
	"uniform float u_progress;\n"   /* 0 = effect start, 1 = effect end */
	"uniform int u_phase;\n"        /* 1 = opening, 2 = closing, 3 = idle */
	"uniform vec2 u_size;\n"        /* target size, px */
	"uniform vec2 u_target_size;\n"
	"uniform vec2 u_origin;\n"      /* window position on the output, px */
	"uniform vec2 u_output_size;\n"
	"uniform float u_alpha;\n"
	"uniform float u_time;\n"       /* seconds since the effect started */
	"uniform float u_flip_y;\n"
	"varying vec2 v_uv;\n"
	"\n";

/* ------------------------------------------------------ decoration shader */

/* Rounded corners, border, drop shadow, blurred backdrop and grain in one
 * pass. It only ever writes inside the window's rounded rect (plus the shadow
 * ring), so the sharp scene keeps showing everywhere else. */
static const char *decor_fragment_src =
	/* 能用 highp 就用：mediump 的 16 位范围在大窗口上会把 SDF 的 length()
	 * 和噪点哈希算溢出。不支持 highp 的设备退回 mediump（下面已做防护）。 */
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
	"precision highp float;\n"
	"#else\n"
	"precision mediump float;\n"
	"#endif\n"
	"uniform sampler2D u_texture;\n" /* window content */
	"uniform sampler2D u_blur;\n"    /* blurred backdrop, half resolution */
	"uniform vec2 u_size;\n"         /* content size */
	"uniform vec2 u_target_size;\n"  /* padded size */
	"uniform vec2 u_content_off;\n" /* 内容左上角在目标里的位置（目标像素） */
	"uniform float u_radius;\n"
	/* 圆角的形状指数（配置键 corner_n）：2 = 圆弧、4 = 超椭圆（squircle）。 */
	"uniform float u_corner_n;\n"
	"uniform float u_border_width;\n"
	"uniform vec4 u_border_color;\n" /* premultiplied */
	"uniform vec4 u_shadow_color;\n" /* premultiplied */
	"uniform vec2 u_shadow_offset;\n"
	"uniform float u_shadow_radius;\n"
	"uniform float u_shadow_only;\n"
	"uniform float u_opacity;\n"
	/* 内容那一项的额外乘子：窗口抓帧那条路把它设成 0 —— 内容由场景里那个
	 * 镜像节点画，这张图里只要底衬 / 画框，但**透明度剔除还得按内容自己的
	 * alpha** 走（u_texture 照样得采）。 */
	"uniform float u_content_alpha;\n"
	"uniform float u_noise;\n"
	"uniform float u_alpha;\n"
	"uniform float u_time;\n"
	"uniform vec2 u_origin;\n"
	"uniform vec2 u_output_size;\n"
	/* 动画缩放补偿：节点按 anim_scale 缩放整块装饰缓冲时，模糊采样坐标
	 * 也要按同一倍率展开，否则背景跟着窗口一起放大 / 回弹。 */
	"uniform vec2 u_blur_scale;\n"
	"uniform vec2 u_blur_bias;\n"
	/* 跨屏不在这里做：一次装饰渲染只认一张底衬贴图（见 decor_draw 的说明），
	 * 两块屏各烘各的一份装饰缓冲、各裁到自己那块屏。 */
	"uniform float u_flip_y;\n"
	"uniform float u_blur_strength;\n"
	/* ext-background-effect：客户端指定的模糊区域（窗口局部坐标）。
	 * on <= 0.5 时表示没限制，整个窗口都模糊。 */
	"uniform vec4 u_blur_region;\n"
	"uniform float u_blur_region_on;\n"
	/* 内容贴图在装饰矩形里的采样映射：uv = xy + 内容坐标 * zw（归一化）。
	 * 客户端自己画装饰时源缓冲比可见窗口大一圈（CSD 阴影留白），内容就得偏着采；
	 * 没留白时 xy = 0、zw = 1。 */
	"uniform vec4 u_tex_map;\n"
	/* 元素自己的 alpha（「透明度剔除」）：模糊只画在元素真的有内容的地方。
	 * u_mask_on > 0.5 时取 u_mask，否则取 u_texture。u_mask_min 是阈值（硬 step，
	 * 跟 niri 的 blur_finish 一样；0 = 不剔）。 */
	"uniform sampler2D u_mask;\n"
	"uniform float u_mask_on;\n"
	"uniform float u_mask_min;\n"
	"uniform vec4 u_mask_map;\n"
	"varying vec2 v_uv;\n"
	"\n"
	"float sd_round_box(vec2 p, vec2 half_size, float r, float n) {\n"
	"	vec2 q = abs(p) - half_size + r;\n"
	"	vec2 c = max(q, 0.0);\n"
	"	float d;\n"
	"	float k;\n"
	"	if (n > 2.001) {\n"
	/* 圆弧（n = 2）换成**超椭圆**：|x|^n + |y|^n = r^n 那条曲线（n = 4 就是
	 * squircle）。直边和内侧和圆弧**逐像素一样** —— c 里有个分量是 0 时
	 * 这个范数就退化成另一个分量的绝对值。 */
	"		float s = pow(c.x, n) + pow(c.y, n);\n"
	"		d = pow(s, 1.0 / n);\n"
	/* 它**不是**严格的 SDF：梯度模沿直边是 1、沿对角线涨到 2^(1/n)（n = 4
	 * 时 1.19）。下面这个因子 = 1 / |∇d|：不除的话 smoothstep(-1, 1, d) 那条
	 * 抗锯齿斜坡在角上只有 2 / 1.19 = 1.68px 宽（直边还是 2px），边框 / 阴影
	 * 沿对角线的厚度也跟着细一圈（8px 的边框沿对角线只剩 6.7px）。除完斜坡
	 * 宽度与方向无关，边框厚度处处 ≈ border_width。c 有个分量是 0 时这个因子
	 * 恒等于 1，所以直边和圆弧完全一致。 */
	"		float den = length(vec2(pow(c.x, n - 1.0), pow(c.y, n - 1.0)));\n"
	"		k = den > 1e-4 ? pow(s, (n - 1.0) / n) / den : 1.0;\n"
	"	} else {\n"
	"		d = length(c);\n"
	"		k = 1.0;\n"
	"	}\n"
	"	return (d - r) * k + min(max(q.x, q.y), 0.0);\n"
	"}\n"
	"\n"
	"void main() {\n"
	"	vec2 p = v_uv * u_target_size;\n"
	"	vec2 wp = p - u_content_off;\n"
	"	vec2 half_size = u_size * 0.5;\n"
	"	float r = min(u_radius, min(half_size.x, half_size.y));\n"
	"	float d = sd_round_box(wp - half_size, half_size, r, u_corner_n);\n"
	"	float inside = 1.0 - smoothstep(-1.0, 1.0, d);\n"
	"\n"
	"	vec2 cuv = clamp(wp / u_size, 0.0, 1.0);\n"
	"	vec2 uv = clamp(u_tex_map.xy + cuv * u_tex_map.zw, 0.0, 1.0);\n"
	"	vec4 content = texture2D(u_texture, uv) * u_opacity * u_content_alpha * inside;\n"
	"\n"
	"	float bw = min(u_border_width, min(half_size.x, half_size.y));\n"
	"	float ring = inside - (1.0 - smoothstep(-1.0, 1.0, d + bw));\n"
	"	vec4 border = u_border_color * u_opacity * ring;\n"
	"\n"
	"	float sa = 1.0 - smoothstep(-1.0, u_shadow_radius,\n"
	"		sd_round_box(wp - u_shadow_offset - half_size, half_size, r, u_corner_n));\n"
	/* Only outside the window: otherwise the shadow shows through a
	 * translucent window instead of staying behind it. */
	"	vec4 shadow = u_shadow_color * sa * (1.0 - inside);\n"
	"	if (u_shadow_only > 0.5) {\n"
	"		gl_FragColor = shadow * u_alpha;\n"
	"		return;\n"
	"	}\n"
	"\n"
	"	vec2 o = (u_origin - u_content_off + p * u_blur_scale + u_blur_bias) / u_output_size;\n"
	"	vec4 blurred = texture2D(u_blur, clamp(o, 0.0, 1.0));\n"
	/* 元素自己的 alpha（「透明度剔除」）：模糊只画在元素真的有内容的地方 —— 透明
	 * 内边距、圆角外面、弹窗自带的半透明阴影边都不再透出一圈模糊。窗口 / 弹窗那条
	 * 路内容贴图就是元素自己（u_texture）；layer surface 的模糊底衬不画内容，元素
	 * 内容由 u_mask 传进来。用的是**内容自己的 alpha**，不含 u_opacity：剔除管的是
	 * 形状，半透明窗口该透出来的模糊不能因为 window_opacity 低就整个没了。 */
	"	float elem_a = u_mask_on > 0.5 ?\n"
	/* mask 是元素**自己**那张缓冲，采样坐标用元素局部（恒等映射）：窗口那层
	 * CSD 留白偏移只属于内容贴图，不能带到 mask 上，否则剔除会看错地方。 */
	"		texture2D(u_mask, clamp(u_mask_map.xy + cuv * u_mask_map.zw,\n"
	"			0.0, 1.0)).a : texture2D(u_texture, uv).a;\n"
	"	float cover = 1.0;\n"
	"	if (u_blur_region_on > 0.5) {\n"
	"		vec2 lo = u_blur_region.xy;\n"
	"		vec2 hi = lo + u_blur_region.zw;\n"
	"		vec2 a = smoothstep(lo - 1.0, lo + 1.0, wp);\n"
	"		vec2 b = 1.0 - smoothstep(hi - 1.0, hi + 1.0, wp);\n"
	"		cover = a.x * a.y * b.x * b.y;\n"
	"	}\n"
	"	vec4 out_px = blurred * u_blur_strength * inside * cover *\n"
	"		step(u_mask_min, elem_a);\n"
	"\n"
	/* mediump 只有约 16 位有效范围：大窗口上 dot(p, vec2(12.99, 78.23)) 会
	 * 直接溢出成 Inf，sin(Inf) = NaN，而 NaN * u_noise（哪怕 u_noise = 0）
	 * 仍然是 NaN —— 它把 out_px.rgb 毒掉、alpha 却保留下来，画出来就是右下
	 * 一块**不透明黑色楔形**（边界正好是 12.99x + 78.23y = 65504 这条斜线）。
	 * 所以只在真要噪点时才算，并先把坐标取模压回安全范围。 */
	"	if (u_noise > 0.0) {\n"
	"		vec2 np = mod(p, 512.0);\n"
	"		float n = fract(sin(dot(np, vec2(12.9898, 78.233)) + u_time) * 43758.5453);\n"
	"		out_px.rgb += (n - 0.5) * u_noise * out_px.a;\n"
	"	}\n"
	"\n"
	"	out_px = shadow + out_px * (1.0 - shadow.a);\n"
	"	out_px = content + out_px * (1.0 - content.a);\n"
	"	out_px = border + out_px * (1.0 - border.a);\n"
	"	gl_FragColor = out_px * u_alpha;\n"
	"}\n";

/* ------------------------------------------------------------ kawase blur */

static const char *kawase_prefix =
	"precision mediump float;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec2 u_texel;\n" /* 1 / source size */
	"uniform float u_offset;\n" /* 半径，单位：源 texel */
	"varying vec2 v_uv;\n"
	"\n"
	/* 抽样就是抽样：链上不做任何透明度剔除。底衬本身就是预乘的，直接糊就行 ——
	 * 它透明的地方 alpha 跟着一起淡出，合成的时候自然透出真实背景。
	 *
	 * 曾经在最后一趟（kawase_final）按底衬的 alpha 做过一次剔除，**撤了**（见
	 * README 的 115）：透明度剔除是用来**约束模糊的范围**的（哪儿该糊），不是用来
	 * 改糊出来的像素；底衬该不该糊已经由「谁在它上面、它自己透不透」决定了。现在
	 * 剔除只有一处 —— 装饰着色器里按**元素自己**的 alpha 剔（decor_fragment_src）。 */
	"vec4 tap(vec2 uv, float weight) {\n"
	"	return texture2D(u_tex, uv) * weight;\n"
	"}\n"
	"\n";

/* 降采样：抽样半径 = u_offset 个**源** texel。源每降一级尺寸减半，所以同一个
 * u_offset 换算回原始像素每级翻倍 —— 半径就是这么跟着降采样扩展的。抽样的形状
 * 跟 niri 的 blur_down 一致：中心 4 份 + 四个斜角各 1 份，除以 8。 */
static const char *kawase_down_src =
	"void main() {\n"
	"	vec2 o = u_texel * u_offset;\n"
	"	vec4 sum = tap(v_uv, 4.0);\n"
	"	sum += tap(v_uv + vec2(-o.x, -o.y), 1.0);\n"
	"	sum += tap(v_uv + vec2( o.x, -o.y), 1.0);\n"
	"	sum += tap(v_uv + vec2(-o.x,  o.y), 1.0);\n"
	"	sum += tap(v_uv + vec2( o.x,  o.y), 1.0);\n"
	"	gl_FragColor = sum / 8.0;\n"
	"}\n";

/* 上升：采样单位是**源 texel 的一半**（niri 的 blur_up）。目标比源大一倍，半步
 * 就是 1/4 目标 texel；12 个抽样权重和也是 12，除以 12 就归一了。老代码额外加了
 * 一份同级降采样副本再除以 13（dual filter 的 add）—— 那份是**没这么糊**的图，
 * 等于每一级都掺回一点锐度，实测半径每级只涨 1.5 倍而不是 2 倍，级数一多就「看
 * 不出在变大」。去掉之后级数每加一级半径翻倍，和 niri 对得上。 */
#define KAWASE_UP_TAPS \
	"	vec2 t = u_texel * u_offset * 0.5;\n" \
	"	vec4 sum = vec4(0.0);\n" \
	"	sum += tap(v_uv + vec2(-t.x * 2.0, 0.0), 1.0);\n" \
	"	sum += tap(v_uv + vec2(-t.x, t.y), 2.0);\n" \
	"	sum += tap(v_uv + vec2(0.0, t.y * 2.0), 1.0);\n" \
	"	sum += tap(v_uv + vec2(t.x, t.y), 2.0);\n" \
	"	sum += tap(v_uv + vec2(t.x * 2.0, 0.0), 1.0);\n" \
	"	sum += tap(v_uv + vec2(t.x, -t.y), 2.0);\n" \
	"	sum += tap(v_uv + vec2(0.0, -t.y * 2.0), 1.0);\n" \
	"	sum += tap(v_uv + vec2(-t.x, -t.y), 2.0);\n"

static const char *kawase_up_src =
	"void main() {\n" KAWASE_UP_TAPS
	"	gl_FragColor = sum / 12.0;\n"
	"}\n";

/* ------------------------------------------------------------ mesh shader */

/* 任意四边形网格：顶点属性直接给「渲染目标的物理像素坐标」和「源贴图归一化
 * UV」（jelly.c 的果冻形变）。片元没花样，就是采样 —— 每格两个三角形，相邻
 * 格共用同一对顶点，边界严丝合缝。
 *
 * 以前是一格一块**轴对齐矩形**（wlr_render_pass 只能贴轴对齐的矩形）：剪切形变
 * 下相邻行各自取整，屏幕上就是「窗口像好几条矩形拼出来的」—— 左边缘一段一段
 * 跳、行边界一条错位的竖线，透明窗口那层模糊透过缝看尤其显眼。 */
static const char *mesh_vertex_src =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"uniform vec2 u_size;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"	v_uv = a_uv;\n"
	"	gl_Position = vec4(a_pos / u_size * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

static const char *mesh_fragment_src =
	"precision mediump float;\n"
	"uniform sampler2D u_texture;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"	gl_FragColor = texture2D(u_texture, v_uv);\n"
	"}\n";

/* 果冻的模糊层：a_uv 还是内容 UV（跟着网格形变），a_buv 是**顶点当前屏幕
 * 坐标**映射到 blur 贴图的 UV（不跟网格拉长）。片元按装饰着色器最后那条
 * 公式把内容叠在模糊上：content + blur * (1 - content.a)。一遍画完，不再
 * 需要先画模糊层再画内容层。 */
static const char *mesh_blur_vertex_src =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"attribute vec2 a_buv;\n"
	"uniform vec2 u_size;\n"
	"varying vec2 v_uv;\n"
	"varying vec2 v_buv;\n"
	"varying vec2 v_local;\n"
	"void main() {\n"
	"	v_uv = a_uv;\n"
	"	v_buv = a_buv;\n"
	"	v_local = a_pos;\n"
	"	gl_Position = vec4(a_pos / u_size * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

static const char *mesh_blur_fragment_src =
	"precision mediump float;\n"
	"uniform sampler2D u_texture;\n"
	"uniform sampler2D u_blur;\n"
	"uniform float u_blur_strength;\n"
	/* 窗口可见矩形（目标像素）和圆角：模糊只画在窗口**里面**，阴影 /
	 * 圆角外面的那圈留白不采模糊 —— 否则投影会被底下的模糊染上一层。 */
	"uniform vec2 u_src_size;\n"
	"uniform vec2 u_content_off;\n"
	"uniform vec2 u_content_size;\n"
	"uniform float u_radius;\n"
	"uniform float u_corner_n;\n"
	/* 静态投影：按窗口底座矩形算，不跟着果冻网格形变。 */
	"uniform sampler2D u_shadow;\n"
	"uniform vec2 u_shadow_off;\n"
	"uniform vec2 u_shadow_size;\n"
	"uniform vec2 u_shadow_offset;\n"
	"uniform float u_shadow_radius;\n"
	"uniform vec4 u_shadow_color;\n"
	"uniform float u_shadow_on;\n"
	/* 窗口本身的区域：客户端表面贴图的 alpha。投影 / 阴影不在客户端表面里，
	 * 所以这一刀比按矩形裁更准。 */
	"uniform sampler2D u_mask;\n"
	"uniform float u_mask_on;\n"
	"uniform vec4 u_mask_map;\n"
	"uniform float u_mask_min;\n"
	"varying vec2 v_uv;\n"
	"varying vec2 v_buv;\n"
	"varying vec2 v_local;\n"
	"float sd_round_box(vec2 p, vec2 half_size, float r, float n) {\n"
	"	vec2 q = abs(p) - half_size + r;\n"
	"	vec2 c = max(q, 0.0);\n"
	"	float d;\n"
	"	float k;\n"
	"	if (n > 2.001) {\n"
	"		float s = pow(c.x, n) + pow(c.y, n);\n"
	"		d = pow(s, 1.0 / n);\n"
	"		float den = length(vec2(pow(c.x, n - 1.0), pow(c.y, n - 1.0)));\n"
	"		k = den > 1e-4 ? pow(s, (n - 1.0) / n) / den : 1.0;\n"
	"	} else {\n"
	"		d = length(c);\n"
	"		k = 1.0;\n"
	"	}\n"
	"	return (d - r) * k + min(max(q.x, q.y), 0.0);\n"
	"}\n"
	"void main() {\n"
	"	vec4 content = texture2D(u_texture, v_uv);\n"
	"	vec4 blurred = texture2D(u_blur, v_buv) * u_blur_strength;\n"
	"	vec2 p = v_uv * u_src_size;\n"
	"	vec2 cuv = (p - u_content_off) / u_content_size;\n"
	/* 窗口矩形之外（投影 / 圆角留白）直接不画模糊；矩形里面再按客户端
	 * 表面的 alpha 剔掉透明区域。 */
	"	float in_rect = step(0.0, cuv.x) * step(cuv.x, 1.0) *\n"
	"		step(0.0, cuv.y) * step(cuv.y, 1.0);\n"
	"	vec2 muv = clamp(u_mask_map.xy + clamp(cuv, 0.0, 1.0) * u_mask_map.zw,\n"
	"		0.0, 1.0);\n"
	"	float mask_a = texture2D(u_mask, muv).a;\n"
	"	float inside = in_rect * mix(1.0, step(u_mask_min, mask_a), u_mask_on);\n"
	/* 静态投影：按底座矩形（目标坐标）算，不跟果冻网格形变。 */
	"	vec2 sh = v_local - u_shadow_off - u_shadow_size * 0.5;\n"
	"	float sr = min(u_radius, min(u_shadow_size.x, u_shadow_size.y) * 0.5);\n"
	"	float sd = sd_round_box(sh - u_shadow_offset,\n"
	"		u_shadow_size * 0.5, sr, u_corner_n);\n"
	"	float sa = 1.0 - smoothstep(-1.0, u_shadow_radius, sd);\n"
	"	float window = 1.0 - smoothstep(-1.0, 1.0,\n"
	"		sd_round_box(sh, u_shadow_size * 0.5, sr, u_corner_n));\n"
	"	vec4 shadow = u_shadow_color * sa * (1.0 - window) * u_shadow_on;\n"
	"	vec4 out_px = content + blurred * (1.0 - content.a) * inside;\n"
	"	out_px = shadow + out_px * (1.0 - shadow.a);\n"
	"	gl_FragColor = out_px;\n"
	"}\n";

/* --------------------------------------------------------- program build */

static bool link_program_full(unsigned int *out, const char *vert_src,
		const char *frag_src, const char *name) {
	GLuint vs = compile(GL_VERTEX_SHADER, vert_src, name);
	if (vs == 0) {
		return false;
	}
	GLuint fs = compile(GL_FRAGMENT_SHADER, frag_src, name);
	if (fs == 0) {
		glDeleteShader(vs);
		return false;
	}
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = GL_FALSE;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (ok != GL_TRUE) {
		char log[2048];
		glGetProgramInfoLog(prog, sizeof(log), NULL, log);
		wlr_log(WLR_ERROR, "fx: program '%s' failed to link:\n%s", name, log);
		glDeleteProgram(prog);
		return false;
	}
	*out = prog;
	return true;
}

static bool link_program(unsigned int *out, const char *frag_src,
		const char *name) {
	return link_program_full(out, vertex_src, frag_src, name);
}

static void load_mesh_program(struct vidow_server *server) {
	struct vidow_mesh_prog *m = &server->mesh;
	unsigned int prog = 0;
	if (!link_program_full(&prog, mesh_vertex_src, mesh_fragment_src, "mesh")) {
		return;
	}
	m->program = prog;
	m->u_texture = glGetUniformLocation(prog, "u_texture");
	m->u_size = glGetUniformLocation(prog, "u_size");

	struct vidow_mesh_blur_prog *b = &server->mesh_blur;
	prog = 0;
	if (link_program_full(&prog, mesh_blur_vertex_src, mesh_blur_fragment_src,
			"mesh-blur")) {
		b->program = prog;
		b->u_texture = glGetUniformLocation(prog, "u_texture");
		b->u_blur = glGetUniformLocation(prog, "u_blur");
		b->u_size = glGetUniformLocation(prog, "u_size");
		b->u_blur_strength = glGetUniformLocation(prog, "u_blur_strength");
		b->u_src_size = glGetUniformLocation(prog, "u_src_size");
		b->u_content_off = glGetUniformLocation(prog, "u_content_off");
		b->u_content_size = glGetUniformLocation(prog, "u_content_size");
		b->u_radius = glGetUniformLocation(prog, "u_radius");
		b->u_corner_n = glGetUniformLocation(prog, "u_corner_n");
		b->u_shadow = glGetUniformLocation(prog, "u_shadow");
		b->u_shadow_off = glGetUniformLocation(prog, "u_shadow_off");
		b->u_shadow_size = glGetUniformLocation(prog, "u_shadow_size");
		b->u_shadow_offset = glGetUniformLocation(prog, "u_shadow_offset");
		b->u_shadow_radius = glGetUniformLocation(prog, "u_shadow_radius");
		b->u_shadow_color = glGetUniformLocation(prog, "u_shadow_color");
		b->u_shadow_on = glGetUniformLocation(prog, "u_shadow_on");
		b->u_mask = glGetUniformLocation(prog, "u_mask");
		b->u_mask_on = glGetUniformLocation(prog, "u_mask_on");
		b->u_mask_map = glGetUniformLocation(prog, "u_mask_map");
		b->u_mask_min = glGetUniformLocation(prog, "u_mask_min");
	}
}

static void load_decor_program(struct vidow_server *server) {
	struct vidow_decor_prog *d = &server->decor;
	unsigned int prog = 0;
	if (!link_program(&prog, decor_fragment_src, "decor")) {
		return;
	}
	d->program = prog;
#define LOC(field, name) d->field = glGetUniformLocation(prog, name)
	LOC(u_texture, "u_texture");
	LOC(u_blur, "u_blur");
	LOC(u_size, "u_size");
	LOC(u_target_size, "u_target_size");
	LOC(u_content_off, "u_content_off");
	LOC(u_radius, "u_radius");
	LOC(u_corner_n, "u_corner_n");
	LOC(u_border_width, "u_border_width");
	LOC(u_border_color, "u_border_color");
	LOC(u_shadow_color, "u_shadow_color");
	LOC(u_shadow_offset, "u_shadow_offset");
	LOC(u_shadow_radius, "u_shadow_radius");
	LOC(u_shadow_only, "u_shadow_only");
	LOC(u_opacity, "u_opacity");
	LOC(u_content_alpha, "u_content_alpha");
	LOC(u_noise, "u_noise");
	LOC(u_alpha, "u_alpha");
	LOC(u_origin, "u_origin");
	LOC(u_output_size, "u_output_size");
	LOC(u_blur_scale, "u_blur_scale");
	LOC(u_blur_bias, "u_blur_bias");
	LOC(u_flip_y, "u_flip_y");
	LOC(u_blur_strength, "u_blur_strength");
	LOC(u_blur_region, "u_blur_region");
	LOC(u_blur_region_on, "u_blur_region_on");
	LOC(u_tex_map, "u_tex_map");
	LOC(u_mask, "u_mask");
	LOC(u_mask_on, "u_mask_on");
	LOC(u_mask_min, "u_mask_min");
	LOC(u_mask_map, "u_mask_map");
#undef LOC
}

static void load_kawase_programs(struct vidow_server *server) {
	char buf[4096];
	struct {
		struct vidow_gl_prog *prog;
		const char *body;
		const char *name;
	} pass[2] = {
		{ &server->kawase_down, kawase_down_src, "kawase-down" },
		{ &server->kawase_up, kawase_up_src, "kawase-up" },
	};
	for (size_t i = 0; i < 2; i++) {
		snprintf(buf, sizeof(buf), "%s%s", kawase_prefix, pass[i].body);
		unsigned int prog = 0;
		/* 顶点着色器用 kawase 专用的那个（多两个 uniform，能只盖目标里的一块） */
		if (!link_program_full(&prog, kawase_vertex_src, buf, pass[i].name)) {
			continue;
		}
		pass[i].prog->program = prog;
		pass[i].prog->u_tex = glGetUniformLocation(prog, "u_tex");
		pass[i].prog->u_texel = glGetUniformLocation(prog, "u_texel");
		pass[i].prog->u_offset = glGetUniformLocation(prog, "u_offset");
		pass[i].prog->u_rect = glGetUniformLocation(prog, "u_rect");
		pass[i].prog->u_dst = glGetUniformLocation(prog, "u_dst");
	}
}

/* ---------------------------------------------------------------- shaders */

static bool shader_link(struct vidow_shader *sh, const char *fragment_src) {
	if (!link_program(&sh->program, fragment_src, sh->name)) {
		return false;
	}
	unsigned int prog = sh->program;
	sh->u_texture = glGetUniformLocation(prog, "u_texture");
	sh->u_progress = glGetUniformLocation(prog, "u_progress");
	sh->u_phase = glGetUniformLocation(prog, "u_phase");
	sh->u_size = glGetUniformLocation(prog, "u_size");
	sh->u_origin = glGetUniformLocation(prog, "u_origin");
	sh->u_output_size = glGetUniformLocation(prog, "u_output_size");
	sh->u_alpha = glGetUniformLocation(prog, "u_alpha");
	sh->u_time = glGetUniformLocation(prog, "u_time");
	sh->u_flip_y = glGetUniformLocation(prog, "u_flip_y");
	return true;
}

struct vidow_shader *vidow_shader_get(struct vidow_server *server,
		const char *name) {
	if (name == NULL || name[0] == '\0' || !server->shader_ok) {
		return NULL;
	}

	struct vidow_shader *sh;
	wl_list_for_each(sh, &server->shaders, link) {
		if (strcmp(sh->name, name) == 0) {
			return sh->program != 0 ? sh : NULL; /* cached failure */
		}
	}

	sh = calloc(1, sizeof(*sh));
	if (sh == NULL) {
		return NULL;
	}
	snprintf(sh->name, sizeof(sh->name), "%s", name);
	wl_list_insert(&server->shaders, &sh->link);

	char path[PATH_MAX * 2];
	snprintf(path, sizeof(path), "%s/%s.frag", server->config.shader_dir, name);
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		wlr_log(WLR_ERROR, "fx: cannot open shader %s", path);
		return NULL;
	}
	char *body = malloc(MAX_SHADER_SIZE);
	char *src = malloc(MAX_SHADER_SIZE + 1024);
	if (body == NULL || src == NULL) {
		free(body);
		free(src);
		fclose(f);
		return NULL;
	}
	size_t n = fread(body, 1, MAX_SHADER_SIZE - 1, f);
	fclose(f);
	body[n] = '\0';
	snprintf(src, MAX_SHADER_SIZE + 1024, "%s%s", texture_prefix, body);
	free(body);

	struct wlr_egl *egl = NULL;
	if (!gl_make_current(server, &egl)) {
		free(src);
		return NULL;
	}
	bool ok = shader_link(sh, src);
	gl_release(egl);
	free(src);

	if (!ok) {
		return NULL;
	}
	wlr_log(WLR_INFO, "fx: loaded shader '%s' from %s", sh->name, path);
	return sh;
}

/* --------------------------------------------------- 内部缓冲精度（FP16） */

/* 半浮点纹理的线性过滤：kawase 金字塔和装饰烘图全是线性采样，不能过滤就等于
 * 画面发硬。GLES 3 核心就要求 16F 可过滤；GLES 2 要看扩展。 */
static bool gl_half_float_linear_ok(void) {
	const char *version = (const char *)glGetString(GL_VERSION);
	if (version == NULL) {
		return false;
	}
	int major = 0;
	if (sscanf(version, "OpenGL ES %d", &major) == 1 && major >= 3) {
		return true;
	}
	const char *exts = (const char *)glGetString(GL_EXTENSIONS);
	if (exts == NULL) {
		return false;
	}
	static const char want[] = "GL_OES_texture_half_float_linear";
	size_t want_len = sizeof(want) - 1;
	for (const char *p = exts; *p != '\0'; ) {
		while (*p == ' ') {
			p++;
		}
		const char *end = strchr(p, ' ');
		size_t n = end != NULL ? (size_t)(end - p) : strlen(p);
		if (n == want_len && strncmp(p, want, n) == 0) {
			return true;
		}
		if (end == NULL) {
			break;
		}
		p = end + 1;
	}
	return false;
}

/* color_precision 要 FP16 吗（还要再过一次 fx_fp16_ok）。
 * 10 = FP16 缓冲，不是 10 位整数：2101010 只有 2 位 alpha。 */
static bool fx_config_wants_fp16(struct vidow_server *server) {
	const char *want = server->config.color_precision;
	return want == NULL || strcmp(want, "8") != 0;
}

void vidow_fx_apply_config(struct vidow_server *server) {
	bool want = fx_config_wants_fp16(server);
	bool now = want && server->fx_fp16_ok;
	if (now != server->fx_fp16) {
		wlr_log(WLR_INFO, "fx: 内部缓冲精度 -> %s（color_precision = %s）",
			now ? "FP16" : "8bit",
			server->config.color_precision[0] != '\0' ?
			server->config.color_precision : "auto");
	}
	server->fx_fp16 = now;
}

bool vidow_shader_init(struct vidow_server *server) {
	wl_list_init(&server->shaders);
	server->shader_ok = server->config.shader_enabled &&
		wlr_renderer_is_gles2(server->renderer);
	if (server->config.shader_enabled && !server->shader_ok) {
		wlr_log(WLR_ERROR, "fx: renderer is not GLES2, decorations and blur off");
	}
	if (!server->shader_ok) {
		return false;
	}

	struct wlr_egl *egl = NULL;
	if (!gl_make_current(server, &egl)) {
		server->shader_ok = false;
		return false;
	}

	/* 内部缓冲能不能上 FP16（color.precision）：两样都要有 ——
	 *   1) 渲染器能当渲染目标，且是带 alpha 的 16F（XRGB16F 会让圆角发黑）
	 *   2) 半浮点纹理能线性过滤（kawase 金字塔和装饰烘图全是线性采样；
	 *      不能过滤 = 画面发硬，比 8bit 的色阶更难看）
	 * GLES3 核心就要求 16F 可过滤；GLES2 看 OES_texture_half_float_linear。 */
	const struct wlr_drm_format_set *render_formats =
		wlr_renderer_get_render_formats(server->renderer,
			WLR_BUFFER_CAP_DMABUF);
	bool has_fp16 = render_formats != NULL &&
		wlr_drm_format_set_get(render_formats,
			DRM_FORMAT_ABGR16161616F) != NULL;
	bool linear_ok = gl_half_float_linear_ok();
	server->fx_fp16_ok = has_fp16 && linear_ok;
	/* 配置还没跑（Lua 比渲染器晚）：先按 auto 来，vidow_config_sync 再对账。 */
	server->fx_fp16 = server->fx_fp16_ok;
	if (!server->fx_fp16_ok) {
		wlr_log(WLR_INFO, "fx: 内部缓冲用不了 FP16（%s），走 8bit",
			!has_fp16 ? "渲染器没有 ABGR16161616F" :
			"半浮点不能线性过滤");
	}

	load_decor_program(server);
	load_kawase_programs(server);
	load_mesh_program(server);

	/* A 1x1 transparent texture for samplers that are switched off. */
	static const uint32_t pixel = 0x00000000;
	server->empty_tex = wlr_texture_from_pixels(server->renderer,
		DRM_FORMAT_ARGB8888, 4, 1, 1, &pixel);
	gl_release(egl);

	if (server->decor.program == 0) {
		wlr_log(WLR_ERROR, "fx: decoration shader failed, decorations off");
		server->config.decor_enabled = false;
	}
	if (server->kawase_down.program == 0 || server->kawase_up.program == 0) {
		wlr_log(WLR_ERROR, "fx: kawase shaders failed, blur off");
		server->config.blur_enabled = false;
	}
	return true;
}

/* ---------------------------------------------------------------- targets */

/* Higher is better. 2101010 has only two bits of alpha, which quantises soft
 * shadows and antialiased corners into nothing, so it is a last resort.
 *
 * FP16 (ABGR16161616F) is the best: full float alpha, no banding in gradients,
 * and it is what the blur pyramid / decoration buffers want when
 * color.precision says so. Only the alpha-capable variants count — XRGB 16F
 * would give black rounded corners, same reason XRGB8888 scores lowest. */
static int format_score(struct vidow_server *server, uint32_t format) {
	if (server->fx_fp16) {
		switch (format) {
		case DRM_FORMAT_ABGR16161616F:
		case DRM_FORMAT_ARGB16161616F:
			return 4;
		default:
			break;
		}
	}
	switch (format) {
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_BGRA8888:
		return 3;
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_ABGR2101010:
		return 2;
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_XBGR8888:
		return 1;
	default:
		return 0;
	}
}

/* The output is composited with alpha, so an alpha-capable format is strongly
 * preferred: on XRGB the rounded corners would come out black. */
struct wlr_buffer *vidow_alloc_target(struct vidow_server *server, int w,
		int h) {
	const uint32_t caps[] = { WLR_BUFFER_CAP_DMABUF, WLR_BUFFER_CAP_SHM };
	for (int want = server->fx_fp16 ? 4 : 3; want >= 0; want--) {
		for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); c++) {
			const struct wlr_drm_format_set *set =
				wlr_renderer_get_render_formats(server->renderer, caps[c]);
			if (set == NULL) {
				continue;
			}
			for (size_t i = 0; i < set->len; i++) {
				if (format_score(server, set->formats[i].format) != want) {
					continue;
				}
				struct wlr_buffer *buf = wlr_allocator_create_buffer(
					server->allocator, w, h, &set->formats[i]);
				if (buf != NULL) {
					wlr_log(WLR_DEBUG, "fx-target-alloc: %dx%d fmt=0x%08x -> %p",
						w, h, set->formats[i].format, (void*)buf);
					uint64_t mod = 0;
					struct wlr_dmabuf_attributes attrs;
					if (wlr_buffer_get_dmabuf(buf, &attrs)) {
						mod = attrs.modifier;
					}
					wlr_log(WLR_DEBUG, "fx: target %dx%d format 0x%08x mod 0x%llx",
						w, h, set->formats[i].format,
						(unsigned long long)mod);
					/* **GBM 的 bo 缓存会把刚 drop 掉的同尺寸缓冲原样还回来**
					 * —— 里面是上一任的内容，可能是任何一块屏的旧帧。而
					 * 这里的调用方全是「只重画损伤区、不整块清」的增量渲染
					 * （begin_pass_rect 注释：没碰过的地方保住上一版），
					 * 那个前提只对「同一张缓冲」成立；reason 15/16 的
					 * 释放重建循环让同尺寸的 drop+realloc 高频发生，旧帧
					 * 残片就从模糊节点漏到屏幕上（2026-10-31：悬停 vshell
					 * UI，两块屏随机浮出别的窗口的内容、位置不定、直到
					 * 整块重画才消失 —— 就是它）。分配出来先整块清成透明，
					 * 增量渲染的「上一版」从全透明起步。 */
					struct wlr_egl *clear_egl = NULL;
					GLuint clear_fbo =
						wlr_gles2_renderer_get_buffer_fbo(server->renderer,
							buf);
					if (clear_fbo != 0 && gl_make_current(server, &clear_egl)) {
						glBindFramebuffer(GL_FRAMEBUFFER, clear_fbo);
						glViewport(0, 0, w, h);
						glDisable(GL_BLEND);
						glDisable(GL_SCISSOR_TEST);
						glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
						glClear(GL_COLOR_BUFFER_BIT);
						end_pass(clear_egl);
					}
					return buf;
				}
			}
		}
	}
	return NULL;
}

void drop_targets(struct vidow_fx_target *fx) {
	for (int i = 0; i < 2; i++) {
		if (fx->buf[i] != NULL) {
			wlr_buffer_drop(fx->buf[i]);
			fx->buf[i] = NULL;
		}
		if (fx->anim_buf[i] != NULL) {
			wlr_buffer_drop(fx->anim_buf[i]);
			fx->anim_buf[i] = NULL;
		}
	}
	fx->w = 0;
	fx->h = 0;
	fx->content_w = 0;
	fx->content_h = 0;
	fx->dst_w = 0;
	fx->dst_h = 0;
	fx->px_pad = 0;
	fx->scale = 0.0;
}

/* 渲染目标的设备像素数取成**输出缩放**的整数倍：节点尺寸是逻辑单位，而
 * wlr_scene_buffer_set_dest_size() 只收整数 —— 只有 w / os 正好是整数时，
 * 这张烘好的图才会 1:1 落到设备像素上。差 1 个设备像素（0.46 倍缩略图上
 * 经常就差这一个），wlroots 就按 1.00x 之外的倍率再双线性采一次：内容看着
 * 就是「糊」的，而且像素对不上（缩略图丢约 1/4 的梯度对比度，见 README 250）。
 * os <= 0（拿不到输出缩放）时原样返回。 */
static int snap_to_output_scale(int px, double os) {
	if (!(os > 0.0)) {
		return px;
	}
	for (int i = 0; i < 8; i++) {
		double q = (double)(px + i) / os;
		if (fabs(q - (double)llround(q)) < 1e-3) {
			return px + i;
		}
	}
	return px;
}

/* content_w/h 是逻辑单位；缓冲按 scale 倍分配，所以 HiDPI 屏上装饰和窗口内容
 * 都是按设备像素画的（不是先画小再放大）。pad 也要跟着乘，否则边框会内缩。
 * os 是**输出缩放**（设备像素 / 逻辑像素，= scale / ov）：见 snap_to_output_scale。 */
/* 前置声明（实现在下面 blur_assign 那一带）。 */
struct vidow_blur_slot *blur_slot_for(struct vidow_fx_target *fx,
		struct wlr_output *out, bool need);
static void view_bake_for_output(struct vidow_server *server,
		struct vidow_output *output);
bool fx_node_rejects_input(struct wlr_scene_buffer *buffer,
		double *sx, double *sy);

bool ensure_targets(struct vidow_server *server,
		struct vidow_fx_target *fx, int content_w, int content_h, double scale,
		double os) {
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	int pad = vidow_fx_pad(server);
	int px_pad = (int)lround((double)pad * scale);
	int w = snap_to_output_scale(
		(int)lround((double)content_w * scale) + 2 * px_pad, os);
	int h = snap_to_output_scale(
		(int)lround((double)content_h * scale) + 2 * px_pad, os);
	if (w < 1) {
		w = 1;
	}
	if (h < 1) {
		h = 1;
	}
	/* **按屏各一对缓冲**（见 server.h 的 struct vidow_blur_slot 的 dec*）：
	 * 窗口骑在两块屏中间时，两块屏的倍率不同（1x / 2x），缓冲尺寸差一倍。
	 * 共用一对的话，两屏轮流烘就是每帧在两套尺寸之间扔掉重建，而且先烘的
	 * 那块屏的图会被后烘的直接覆盖 —— 屏幕上「一半正常、一半糊」。
	 *
	 * 没有 bake_output 的路径（layer / 预览 / 关闭快照）继续用老的那一对。 */
	struct vidow_blur_slot *sl = fx->bake_output != NULL ?
		blur_slot_for(fx, fx->bake_output, true) : NULL;
	fx->cur_slot = sl;
	struct wlr_buffer **bufs = sl != NULL ? sl->dec : fx->buf;
	int *cw = sl != NULL ? &sl->dec_w : &fx->w;
	int *chh = sl != NULL ? &sl->dec_h : &fx->h;
	double *cscale = sl != NULL ? &sl->dec_scale : &fx->scale;
	/* The active bake's geometry must match its selected buffers, including
	 * cache hits. Legacy consumers (animation, scene size and damage) still
	 * read these fields; leaving them zero hides an otherwise valid buffer. */
	if (sl != NULL) {
		fx->w = w;
		fx->h = h;
		fx->scale = scale;
		fx->content_w = sl->dec_content_w = w - 2 * px_pad;
		fx->content_h = sl->dec_content_h = h - 2 * px_pad;
		fx->px_pad = sl->dec_px_pad = px_pad;
		fx->dst_w = sl->dec_dst_w = (int)lround((double)w / scale);
		fx->dst_h = sl->dec_dst_h = (int)lround((double)h / scale);
	}
	if (bufs[0] != NULL && *cw == w && *chh == h &&
			fabs(*cscale - scale) < 1e-3) {
		return true;
	}
	/* The scene node owns a lock on its current decoration buffer. Detach it
	 * before replacing the target pair, otherwise the old buffer remains locked
	 * while the new pair is allocated and pick_free can never make progress.
	 *
	 * **两条路径的显示节点都要摘**：跨屏（1x/2x）拖拽时尺寸来回跳，每换一次
	 * 尺寸就把旧的一对 dec/anim 丢掉重建。节点上还挂着旧缓冲（n_locks > 0）
	 * 的话 drop 只减生产者引用、块本身不还 —— 每跳一次漏一批全屏大的 dmabuf
	 * （实测 Steam 会话攒出 34 MiB × 150、游戏 128 MiB × 192 无限涨）。 */
	if (sl != NULL && sl->node != NULL) {
		wlr_scene_node_set_enabled(&sl->node->node, false);
		wlr_scene_buffer_set_buffer(sl->node, NULL);
	}
	if (fx->node != NULL && (sl == NULL || fx->node->buffer != NULL)) {
		wlr_scene_buffer_set_buffer(fx->node, NULL);
	}
	for (int i = 0; i < 2; i++) {
		if (sl != NULL && sl->anim[i] != NULL) {
			wlr_buffer_drop(sl->anim[i]);
			sl->anim[i] = NULL;
		}
		if (bufs[i] != NULL) {
			wlr_buffer_drop(bufs[i]);
			bufs[i] = NULL;
		}
	}
	for (int i = 0; i < 2; i++) {
		bufs[i] = vidow_alloc_target(server, w, h);
		if (bufs[i] == NULL) {
			wlr_log(WLR_ERROR, "fx: cannot allocate a %dx%d render target", w, h);
			for (int j = 0; j < 2; j++) {
				if (bufs[j] != NULL) {
					wlr_buffer_drop(bufs[j]);
					bufs[j] = NULL;
				}
			}
			return false;
		}
	}
	*cw = w;
	*chh = h;
	*cscale = scale;
	if (sl != NULL) {
		sl->dec_content_w = w - 2 * px_pad;
		sl->dec_content_h = h - 2 * px_pad;
		sl->dec_px_pad = px_pad;
		sl->dec_dst_w = (int)lround((double)w / scale);
		sl->dec_dst_h = (int)lround((double)h / scale);
		return true;
	}
	/* 内容 = 目标减掉两边内边距。取差值而不是再算一次 lround：snap 补上的那
	 * 一两个像素要算进内容里，否则着色器里内容右边会多出一条没画到的缝。 */
	fx->content_w = w - 2 * px_pad;
	fx->content_h = h - 2 * px_pad;
	fx->px_pad = px_pad;
	/* 场景节点尺寸用逻辑单位：渲染目标除以倍率，正好是 1:1 落到设备像素上。 */
	fx->dst_w = (int)lround((double)w / scale);
	fx->dst_h = (int)lround((double)h / scale);
	return true;
}

/* A buffer the scene is still displaying has n_locks > 0: never draw into it. */
struct wlr_buffer *pick_free(struct wlr_buffer *bufs[2]) {
	for (int i = 0; i < 2; i++) {
		if (bufs[i] != NULL && bufs[i]->n_locks == 0) {
			return bufs[i];
		}
	}
	return NULL;
}


/* ------------------------------------------------------------ gl helpers */

/* 开始往 dst 里画。rect == NULL：整块清成透明；rect != NULL：**不清** —— 那一趟画
 * 的四边形正好盖满 rect（片元每个像素都写、混合关着），清是多余的；而 rect 外面的
 * 像素正是要保住的（「只重跑受损那一小块」的底子：缓冲是复用的，没被碰过的地方还是
 * 上一版的内容）。 */
GLuint begin_pass_rect(struct vidow_server *server, struct wlr_buffer *dst,
		int w, int h, const struct wlr_box *rect, struct wlr_egl **egl) {
	GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(server->renderer, dst);
	if (fbo == 0) {
		wlr_log(WLR_ERROR, "fx: no FBO for a render target");
		return 0;
	}
	if (!gl_make_current(server, egl)) {
		return 0;
	}
	/* 本帧开过离屏趟：vidow_fx_flush() 那一次 glFlush 才有意义
	 * （静止画面一个趟都不开，别每帧白付一次 eglMakeCurrent）。 */
	server->fx_passed = true;
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, w, h);
	glDisable(GL_BLEND);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glDisable(GL_SCISSOR_TEST);
	if (rect == NULL) {
		glClear(GL_COLOR_BUFFER_BIT);
	}
	return fbo;
}

/* 整块清：绝大多数调用点要的就是这个（新缓冲 / 形状变了 / 整块重跑）。 */
GLuint begin_pass(struct vidow_server *server, struct wlr_buffer *dst,
		int w, int h, struct wlr_egl **egl) {
	return begin_pass_rect(server, dst, w, h, NULL, egl);
}

/* 一趟画完：只把 FBO 解绑，**不 flush、也不解绑 EGL 上下文**。
 *
 * 这两件事以前是每趟都做的，而它们是整条装饰链里最贵的一步：eglMakeCurrent
 * (EGL_NO_CONTEXT) 会把这批命令提交给内核（实测 amdgpu 上一次 ~40µs），
 * glFlush 也一样。一帧里「模糊链 + 每个窗口一趟装饰」轻松十几到二十几趟，
 * 于是每帧白付 0.4~1.0ms 的内核提交 —— 而同一份工作整帧只提交一次就够了。
 *
 * 为什么攒到一帧一次是安全的：同一个 GL 上下文里的绘制顺序由 GL 自己保证
 * （装饰缓冲被场景当贴图采样，不 flush 也一定读到新内容）；wlroots 的
 * wlr_egl_make_current() 每次都会真的 eglMakeCurrent（它不缓存），所以上下文
 * 留在这儿绑着不会让谁算错。提交由 vidow_fx_flush() 在每帧提交前做一次，
 * wlroots 自己在 renderer_end / 换上下文时也会 flush。
 *
 * 实测（8 个平铺窗滚动，2560x1440 缩放 2）：每趟一次内核提交 → 每帧一次，
 * 「重烘」0.56ms/帧 → 0.12ms/帧，CPU 6.8% → 4.8%。 */
void end_pass(struct wlr_egl *egl) {
	(void)egl;
	vidow_stats.passes++; /* 探针：一帧里开了几趟 */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* 一帧一次的提交（见 end_pass 的说明）：把这一帧所有离屏趟的命令一次性交给内核。
 * 放在 wlr_scene_output_commit 之前 —— 那之后 wlroots 可能走直扫（一个 GL 调用
 * 都不发），没人替我们 flush。 */
void vidow_fx_flush(struct vidow_server *server) {
	/* 一帧一次的提交只对「这一帧真的开过离屏趟」有意义，而那些趟全在
	 * shader_ok 底下（装饰 / 模糊 / 果冻）。渲染器不是 GLES2 时这里必须直接
	 * 走人：gl_make_current 会碰到 wlroots 那条 assert（见它开头那段）。 */
	if (!server->shader_ok) {
		return;
	}
	/* 这一帧一个离屏趟都没开（静止画面最常见）：没有任何命令等着提交，
	 * 不必白付一次 eglMakeCurrent + glFlush（amdgpu 上各 ~40µs）。
	 * 置位在 begin_pass_rect，用掉就清 —— 下一帧重新从「没开过」算起。 */
	if (!server->fx_passed) {
		return;
	}
	server->fx_passed = false;
	struct wlr_egl *egl = NULL;
	if (!gl_make_current(server, &egl)) {
		return;
	}
	glFlush();
	gl_release(egl);
}

/* GLES2 defaults to a mipmapped min filter; without mipmaps the texture is
 * incomplete and every sample returns black. */
void bind_tex(GLuint unit, GLenum target, GLuint id) {
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(target, id);
	glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void draw_quad(unsigned int program) {
	static const GLfloat verts[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
	GLint loc = glGetAttribLocation(program, "a_pos");
	if (loc >= 0) {
		glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, verts);
		glEnableVertexAttribArray(loc);
	}
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	if (loc >= 0) {
		glDisableVertexAttribArray(loc);
	}
}

/* 把一张贴图按任意三角形网格画进 dst：每项 4 个 float，[x, y, u, v]。
 * x/y 是渲染目标里的物理像素坐标（小数直接进光栅器，别自己取整 —— 取整就是
 * 那条锯齿边），u/v 是源贴图的归一化坐标。顶点按 GL_TRIANGLES 顺序给。 */
bool vidow_fx_draw_mesh_blur(struct vidow_server *server,
		struct wlr_buffer *dst, int w, int h, struct wlr_texture *content,
		struct wlr_texture *blur, float blur_strength, const float *verts,
		int n_verts, int content_off_x, int content_off_y, int content_w,
		int content_h, int src_w, int src_h, float radius, float corner_n,
		struct wlr_texture *mask, float mask_ox, float mask_oy,
		float mask_sx, float mask_sy, float mask_min,
		struct wlr_texture *shadow_tex,
		int shadow_off_x, int shadow_off_y, int shadow_w, int shadow_h,
		float shadow_dx, float shadow_dy, float shadow_radius,
		const float shadow_color[4], bool shadow_on, bool clear) {
	struct vidow_mesh_blur_prog *m = &server->mesh_blur;
	if (m->program == 0 || content == NULL || blur == NULL || verts == NULL ||
			n_verts < 3) {
		return false;
	}
	/* **不是 GLES2 纹理就别碰**：下面 wlr_gles2_texture_get_attribs() 第一行
	 * 就是 `assert(wlr_texture_is_gles2(texture))`。客户端来的纹理不一定是
	 * GLES2 的 —— Steam 的 Vulkan 主界面 / 用 wlr_vulkan_renderer 那条路交上来的
	 * DMA-BUF 就是 Vulkan 纹理，断言一响整个合成器 abort（用户报的「打开 Steam
	 * 主界面就崩溃」，core dump 栈是
	 *   __assert_fail ← libwlroots +0x2250a ← vidow_fx_draw_mesh_blur
	 * 断言字面量就是 render/gles2/texture.c:25: wlr_texture_is_gles2(wlr_texture)）。
	 *
	 * 拿不到 GLES2 属性 = 这一帧画不了模糊，返回 false 让调用者走「没有模糊」
	 * 的那条路（画个纯色底）：难看一帧，比崩掉强。 */
	if (!wlr_texture_is_gles2(content) || !wlr_texture_is_gles2(blur)) {
		return false;
	}
	if (mask != NULL && !wlr_texture_is_gles2(mask)) {
		mask = NULL;
	}
	if (shadow_tex != NULL && !wlr_texture_is_gles2(shadow_tex)) {
		shadow_tex = NULL;
	}
	struct wlr_gles2_texture_attribs ca;
	struct wlr_gles2_texture_attribs ba;
	wlr_gles2_texture_get_attribs(content, &ca);
	wlr_gles2_texture_get_attribs(blur, &ba);

	struct wlr_egl *egl = NULL;
	/* clear = false：接着上一趟往下画（跨屏果冻的第二块屏那一半），不清屏。
	 * begin_pass_rect 传个非 NULL 的 rect 就跳过 glClear（scissor 是关的，
	 * 不会真的裁）。 */
	static const struct wlr_box no_clear_hint = {1, 1, 1, 1};
	if (begin_pass_rect(server, dst, w, h,
			clear ? NULL : &no_clear_hint, &egl) == 0) {
		return false;
	}
	glUseProgram(m->program);
	bind_tex(0, ca.target, ca.tex);
	bind_tex(1, ba.target, ba.tex);
	glUniform1i(m->u_texture, 0);
	glUniform1i(m->u_blur, 1);
	glUniform1i(m->u_mask, 2);
	glUniform1i(m->u_shadow, 3);
	glActiveTexture(GL_TEXTURE2);
	if (mask != NULL) {
		struct wlr_gles2_texture_attribs ma;
		wlr_gles2_texture_get_attribs(mask, &ma);
		glBindTexture(ma.target, ma.tex);
		glUniform1f(m->u_mask_on, 1.0f);
	} else {
		glBindTexture(GL_TEXTURE_2D, 0);
		glUniform1f(m->u_mask_on, 0.0f);
	}
	glActiveTexture(GL_TEXTURE3);
	if (shadow_on && shadow_tex != NULL) {
		struct wlr_gles2_texture_attribs sa;
		wlr_gles2_texture_get_attribs(shadow_tex, &sa);
		glBindTexture(sa.target, sa.tex);
	} else {
		glBindTexture(GL_TEXTURE_2D, 0);
	}
	glActiveTexture(GL_TEXTURE0);
	glUniform1f(m->u_blur_strength, blur_strength);
	glUniform2f(m->u_size, (float)w, (float)h);
	glUniform2f(m->u_src_size, (float)src_w, (float)src_h);
	glUniform2f(m->u_content_off, (float)content_off_x,
		(float)content_off_y);
	glUniform2f(m->u_content_size, (float)content_w, (float)content_h);
	glUniform1f(m->u_radius, radius);
	glUniform1f(m->u_corner_n, corner_n);
	glUniform4f(m->u_mask_map, mask_ox, mask_oy, mask_sx, mask_sy);
	glUniform1f(m->u_mask_min, mask_min);
	glUniform2f(m->u_shadow_off, (float)shadow_off_x, (float)shadow_off_y);
	glUniform2f(m->u_shadow_size, (float)shadow_w, (float)shadow_h);
	glUniform2f(m->u_shadow_offset, shadow_dx, shadow_dy);
	glUniform1f(m->u_shadow_radius, shadow_radius);
	glUniform4f(m->u_shadow_color, shadow_color[0], shadow_color[1],
		shadow_color[2], shadow_color[3]);
	glUniform1f(m->u_shadow_on, shadow_on ? 1.0f : 0.0f);

	GLint ap = glGetAttribLocation(m->program, "a_pos");
	GLint au = glGetAttribLocation(m->program, "a_uv");
	GLint ab = glGetAttribLocation(m->program, "a_buv");
	const GLsizei stride = sizeof(float) * 6;
	if (ap >= 0) {
		glVertexAttribPointer(ap, 2, GL_FLOAT, GL_FALSE, stride, verts);
		glEnableVertexAttribArray(ap);
	}
	if (au >= 0) {
		glVertexAttribPointer(au, 2, GL_FLOAT, GL_FALSE, stride, verts + 2);
		glEnableVertexAttribArray(au);
	}
	if (ab >= 0) {
		glVertexAttribPointer(ab, 2, GL_FLOAT, GL_FALSE, stride, verts + 4);
		glEnableVertexAttribArray(ab);
	}
	/* 片元自己已经按预乘 alpha 合成好，直接覆盖写。 */
	glDisable(GL_BLEND);
	glDrawArrays(GL_TRIANGLES, 0, n_verts);
	if (ap >= 0) {
		glDisableVertexAttribArray(ap);
	}
	if (au >= 0) {
		glDisableVertexAttribArray(au);
	}
	if (ab >= 0) {
		glDisableVertexAttribArray(ab);
	}
	glUseProgram(0);
	end_pass(egl);
	return true;
}

bool vidow_fx_draw_mesh(struct vidow_server *server, struct wlr_buffer *dst,
		int w, int h, struct wlr_texture *src, const float *verts,
		int n_verts) {
	struct vidow_mesh_prog *m = &server->mesh;
	if (m->program == 0 || src == NULL || verts == NULL || n_verts < 3) {
		return false;
	}
	struct wlr_gles2_texture_attribs sa;
	wlr_gles2_texture_get_attribs(src, &sa);

	struct wlr_egl *egl = NULL;
	if (begin_pass(server, dst, w, h, &egl) == 0) {
		return false;
	}
	glUseProgram(m->program);
	bind_tex(0, sa.target, sa.tex);
	glUniform1i(m->u_texture, 0);
	glUniform2f(m->u_size, (float)w, (float)h);

	GLint ap = glGetAttribLocation(m->program, "a_pos");
	GLint au = glGetAttribLocation(m->program, "a_uv");
	if (ap >= 0) {
		glVertexAttribPointer(ap, 2, GL_FLOAT, GL_FALSE,
			sizeof(float) * 4, verts);
		glEnableVertexAttribArray(ap);
	}
	if (au >= 0) {
		glVertexAttribPointer(au, 2, GL_FLOAT, GL_FALSE,
			sizeof(float) * 4, verts + 2);
		glEnableVertexAttribArray(au);
	}
	/* 源内容是预乘 alpha 的，混合也按预乘来（和 wlr_render_pass 的
	 * PREMULTIPLIED 一个公式）。 */
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glDrawArrays(GL_TRIANGLES, 0, n_verts);
	glDisable(GL_BLEND);
	if (ap >= 0) {
		glDisableVertexAttribArray(ap);
	}
	if (au >= 0) {
		glDisableVertexAttribArray(au);
	}
	glUseProgram(0);
	end_pass(egl);
	return true;
}

/* The client's own buffer: wlr_texture_from_buffer() on a wlr_client_buffer
 * hands back the wrapper's cached texture, which we must not destroy. */
struct wlr_buffer *render_source(struct wlr_buffer *buffer) {
	struct wlr_client_buffer *client = wlr_client_buffer_get(buffer);
	if (client != NULL && client->source != NULL) {
		return client->source;
	}
	return buffer;
}

/* 采样「客户端自己的表面缓冲」用的贴图。
 *
 * **优先借 wlr_client_buffer 自己那张**（wlr_buffer.h：`texture` 就是这个缓冲的
 * 贴图）—— 它是 wlroots 画场景时用的那张，跟着客户端每次 attach 走。以前的写法
 * 为了「拿到的贴图可以 destroy」而绕开它、对 `client->source` 另建一张：对
 * **dmabuf 客户端**（Qt/KDE 那类：每帧重画同一个 dmabuf、只 commit 不换缓冲）那
 * 张家出来的贴图一直是第一次导入的那版内容 —— 客户端在画、装饰缓冲也在重烘，
 * 里面却永远是旧画面，屏幕上就是「窗口停在最后一帧」（实测 kate：文档改了、标题
 * 带 *、装饰缓冲 dump 出来的还是旧文本，打字前后一像素不差）。
 *
 * 借来的**不许 destroy**（wlroots 的说明就在上面），所以返回 owned 标记：
 * 借不到才退回去自己建一张，那时候 owned = true，用完要 destroy。 */
struct wlr_texture *client_texture(struct wlr_renderer *renderer,
		struct wlr_buffer *buffer, bool *owned) {
	struct wlr_client_buffer *client = wlr_client_buffer_get(buffer);
	if (client != NULL && client->texture != NULL) {
		*owned = false;
		return client->texture;
	}
	*owned = true;
	return wlr_texture_from_buffer(renderer, render_source(buffer));
}

/* FX samples client textures outside wlroots' scene render pass. Mirror the
 * scene-buffer acquire wait before borrowing that texture, otherwise a GPU
 * client can commit a dma-buf whose writes are not visible to the FX bake yet.
 * This is intentionally one wait per source bake, not per shader pass. */
bool client_buffer_wait(struct vidow_server *server,
		struct wlr_scene_buffer *scene_buffer) {
	if (scene_buffer == NULL || scene_buffer->WLR_PRIVATE.wait_timeline == NULL) {
		return true;
	}
	if (!wlr_renderer_is_gles2(server->renderer)) {
		return true;
	}
	/* Export is deliberately non-blocking here. wlroots' GLES pass uses the
	 * same operation: an unmaterialized point returns an error and this bake
	 * keeps the real surface visible for a later retry. Never block the main
	 * loop with a DRM timeline wait. */
	int fence_fd = wlr_drm_syncobj_timeline_export_sync_file(
		scene_buffer->WLR_PRIVATE.wait_timeline,
		scene_buffer->WLR_PRIVATE.wait_point);
	if (fence_fd < 0) {
		wlr_log(WLR_ERROR, "fx: acquire fence export failed for client buffer");
		return false;
	}
	struct wlr_egl *egl = wlr_gles2_renderer_get_egl(server->renderer);
	bool restore_context = false;
	if (egl == NULL || eglGetCurrentContext() != wlr_egl_get_context(egl)) {
		if (!gl_make_current(server, &egl)) {
			close(fence_fd);
			return false;
		}
		restore_context = true;
	}
	EGLSyncKHR sync = wlr_egl_create_sync(egl, fence_fd);
	close(fence_fd);
	bool ok = sync != EGL_NO_SYNC_KHR && wlr_egl_wait_sync(egl, sync);
	if (sync != EGL_NO_SYNC_KHR) {
		wlr_egl_destroy_sync(egl, sync);
	}
	if (restore_context) {
		gl_release(egl);
	}
	if (!ok) {
		wlr_log(WLR_ERROR, "fx: acquire fence wait failed for client buffer");
	}
	return ok;
}

/* 贴图选择探针（VIDOW_FX_TRACE_TEXTURE=1）。这个函数在渲染路径上
 * （vidow_fx_frame / decor_pass 都会调），所以开关只问一次。 */
static bool fx_trace_texture_on(void) {
	static int done = 0, on = 0;
	if (!done) {
		on = getenv("VIDOW_FX_TRACE_TEXTURE") != NULL ? 1 : 0;
		done = 1;
	}
	return on != 0;
}

struct wlr_texture *client_scene_texture(struct vidow_server *server,
		struct wlr_scene_buffer *scene_buffer, bool *owned) {
	if (scene_buffer == NULL || scene_buffer->buffer == NULL ||
			!client_buffer_wait(server, scene_buffer)) {
		return NULL;
	}
	struct wlr_client_buffer *client =
		wlr_client_buffer_get(scene_buffer->buffer);
	struct wlr_texture *texture =
		client_texture(server->renderer, scene_buffer->buffer, owned);
	if (fx_trace_texture_on()) {
		wlr_log(WLR_INFO,
			"fx-texture: sb=%p buffer=%p client=%p source=%p client_tex=%p "
			"selected=%p size=%dx%d owned=%d",
			(void *)scene_buffer, (void *)scene_buffer->buffer,
			(void *)client, client != NULL ? (void *)client->source : NULL,
			client != NULL ? (void *)client->texture : NULL,
			(void *)texture, scene_buffer->buffer->width,
			scene_buffer->buffer->height, owned != NULL ? *owned : -1);
	}
	return texture;
}

/* VIDOW_FX_DUMP=N writes the first N rendered targets to /tmp so the effect
 * can be eyeballed without a screenshot protocol. VIDOW_BLUR_DUMP does the
 * same for the backdrop and the blurred result. */
void dump_buffer(struct vidow_server *server, struct wlr_buffer *buffer,
		int w, int h, const char *tag, int index) {
	struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer, buffer);
	if (tex == NULL) {
		return;
	}
	uint32_t format = wlr_texture_preferred_read_format(tex);
	size_t stride = (size_t)w * 4;
	void *data = malloc(stride * (size_t)h);
	if (data == NULL) {
		wlr_texture_destroy(tex);
		return;
	}
	struct wlr_texture_read_pixels_options opts = {
		.data = data,
		.format = format,
		.stride = (uint32_t)stride,
		.src_box = { .x = 0, .y = 0, .width = w, .height = h },
	};
	if (wlr_texture_read_pixels(tex, &opts)) {
		char path[128];
		snprintf(path, sizeof(path), "/tmp/%s-%d.raw", tag, index);
		FILE *f = fopen(path, "wb");
		if (f != NULL) {
			fwrite(data, 1, stride * (size_t)h, f);
			fclose(f);
		}
		snprintf(path, sizeof(path), "/tmp/%s-%d.txt", tag, index);
		f = fopen(path, "w");
		if (f != NULL) {
			fprintf(f, "%d %d 0x%08x\n", w, h, format);
			fclose(f);
		}
		wlr_log(WLR_INFO, "%s: dumped %dx%d to /tmp/%s-%d.raw", tag, w, h, tag,
			index);
	}
	free(data);
	wlr_texture_destroy(tex);
}

/* VIDOW_BLUR_DUMP=N：把前 N 份底衬 dump 成裸数据（排查「底衬里到底是什么」）。
 * getenv 只问一次 —— 这个函数在重抓路径上（一帧能问十几次）。 */
int blur_dump_limit(void) {
	static int limit = -1;
	if (limit < 0) {
		const char *env = getenv("VIDOW_BLUR_DUMP");
		limit = env != NULL ? atoi(env) : 0;
	}
	return limit;
}

void dump_target(struct vidow_server *server,
		struct vidow_fx_target *fx, struct wlr_buffer *target) {
	static int dumped = -1;
	static int limit = 0;
	if (dumped < 0) {
		const char *env = getenv("VIDOW_FX_DUMP");
		limit = env != NULL ? atoi(env) : 0;
		dumped = 0;
	}
	if (dumped >= limit) {
		return;
	}

	struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer, target);
	if (tex == NULL) {
		return;
	}
	uint32_t format = wlr_texture_preferred_read_format(tex);
	size_t stride = (size_t)fx->w * 4;
	void *data = malloc(stride * (size_t)fx->h);
	if (data != NULL) {
		struct wlr_texture_read_pixels_options opts = {
			.data = data,
			.format = format,
			.stride = (uint32_t)stride,
			.src_box = { .x = 0, .y = 0, .width = fx->w, .height = fx->h },
		};
		if (wlr_texture_read_pixels(tex, &opts)) {
			char path[128];
			snprintf(path, sizeof(path), "/tmp/vidow-fx-%d.raw", dumped);
			FILE *f = fopen(path, "wb");
			if (f != NULL) {
				fwrite(data, 1, stride * (size_t)fx->h, f);
				fclose(f);
			}
			snprintf(path, sizeof(path), "/tmp/vidow-fx-%d.txt", dumped);
			f = fopen(path, "w");
			if (f != NULL) {
				fprintf(f, "%d %d 0x%08x\n", fx->w, fx->h, format);
				fclose(f);
			}
			wlr_log(WLR_INFO,
				"fx: dumped %dx%d (0x%08x) to /tmp/vidow-fx-%d.raw",
				fx->w, fx->h, format, dumped);
			dumped++;
		}
		free(data);
	}
	wlr_texture_destroy(tex);
}
