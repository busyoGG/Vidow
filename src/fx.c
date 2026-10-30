#include "server.h"
#include "fx-internal.h"
#include "fx-layer.h"
#include "xtrace.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

/* 临时探针：blur_ver++ 是哪一个调用点推的（索引 = 源码行号）。静止桌面
 * 每帧重烘全部装饰时，这几个计数直接指出是「贴图换了」还是「视野变了」。 */
int blur_ver_hits[6400];

void vidow_fx_dump_blur_ver_hits(void) {
	char buf[512];
	size_t off = 0;
	static const int watch[] = {2665, 2686, 2695, 3276, 4415, 4431, 5360, 5954,
		5990, 6194, 6347};
	for (size_t i = 0; i < sizeof(watch) / sizeof(watch[0]); i++) {
		int n = blur_ver_hits[watch[i]];
		if (n == 0) {
			continue;
		}
		off += snprintf(buf + off, sizeof(buf) - off, "%d:%d ", watch[i], n);
		blur_ver_hits[watch[i]] = 0;
	}
	buf[off] = '\0';
	if (off > 0) {
		wlr_log(WLR_DEBUG, "BLURVER %s", buf);
	}
}

/* ---- 渲染统计（VIDOW_RENDER_STATS=1，见 output.c）：每帧各阶段耗时 + 重建次数。
 * 计数是机器无关的硬指标 —— 「有背景时拖拽卡」这类问题，先在真机上跑
 * `VIDOW_RENDER_STATS=1 vidow` 看每帧干了多少活，再谈优化。
 *
 * tex_new/tex_del 数的是 vidow **自己那些目标缓冲**的贴图导入/销毁：真后端上
 * （GBM/dmabuf）每次都是一次 EGLImage 创建 + BO 映射，每帧十几次就够呛。客户端
 * 缓冲走的是 wlr_client_buffer 自己那张，不算在内。 */
struct vidow_frame_stats vidow_stats;
struct wlr_texture *stat_tex_new(struct wlr_texture *t) {
	if (t != NULL) {
		vidow_stats.tex_new++;
	}
	return t;
}
void stat_tex_del(struct wlr_texture *t) {
	if (t != NULL) {
		vidow_stats.tex_del++;
	}
	wlr_texture_destroy(t);
}
/* ------------------------------------------------------------ scene alpha */

/* 透明度一律夹在 [0,1]：wlroots 那边是 assert(opacity >= 0 && opacity <= 1)，
 * 直接 abort。缓动曲线是**会过冲**的（easing = "out_back 0.18" 冲到 1.18，
 * out_elastic 更夸张），把过冲的进度喂进透明度就当场炸 —— 2026-10-13 20:39 那次
 * core dump 就是这条路：#6 alpha_iter ← #8 vidow_fx_set_alpha ← #9 layer_apply_anim
 * （layer 的入场/消失动画把缓动进度当 alpha 用）。透明度本身不该过冲。 */
static float clamp01(double v) {
	if (v < 0.0) return 0.0f;
	if (v > 1.0) return 1.0f;
	return (float)v;
}

static void alpha_iter(struct wlr_scene_buffer *buffer, int sx, int sy,
		void *data) {
	(void)sx;
	(void)sy;
	wlr_scene_buffer_set_opacity(buffer, clamp01(*(double *)data));
}

/* Multiply every buffer of a subtree by `alpha`. Scene-graph opacity is a
 * compositor-side property, so this never disturbs what clients draw. */
void vidow_fx_set_alpha(struct wlr_scene_node *node, double alpha) {
	if (alpha < 0.0) alpha = 0.0;
	if (alpha > 1.0) alpha = 1.0;
	wlr_scene_node_for_each_buffer(node, alpha_iter, &alpha);
}

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
 * shadows and antialiased corners into nothing, so it is a last resort. */
static int format_score(uint32_t format) {
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
	for (int want = 3; want >= 0; want--) {
		for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); c++) {
			const struct wlr_drm_format_set *set =
				wlr_renderer_get_render_formats(server->renderer, caps[c]);
			if (set == NULL) {
				continue;
			}
			for (size_t i = 0; i < set->len; i++) {
				if (format_score(set->formats[i].format) != want) {
					continue;
				}
				struct wlr_buffer *buf = wlr_allocator_create_buffer(
					server->allocator, w, h, &set->formats[i]);
				if (buf != NULL) {
					uint64_t mod = 0;
					struct wlr_dmabuf_attributes attrs;
					if (wlr_buffer_get_dmabuf(buf, &attrs)) {
						mod = attrs.modifier;
					}
					wlr_log(WLR_DEBUG, "fx: target %dx%d format 0x%08x mod 0x%llx",
						w, h, set->formats[i].format,
						(unsigned long long)mod);
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
static bool fx_node_rejects_input(struct wlr_scene_buffer *buffer,
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
	 * while the new pair is allocated and pick_free can never make progress. */
	if (sl != NULL && sl->node != NULL) {
		wlr_scene_node_set_enabled(&sl->node->node, false);
		wlr_scene_buffer_set_buffer(sl->node, NULL);
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
static GLuint begin_pass_rect(struct vidow_server *server, struct wlr_buffer *dst,
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
static GLuint begin_pass(struct vidow_server *server, struct wlr_buffer *dst,
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
static void end_pass(struct wlr_egl *egl) {
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
static void bind_tex(GLuint unit, GLenum target, GLuint id) {
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(target, id);
	glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void draw_quad(unsigned int program) {
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
		const float shadow_color[4], bool shadow_on) {
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
	if (begin_pass(server, dst, w, h, &egl) == 0) {
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
static struct wlr_buffer *render_source(struct wlr_buffer *buffer) {
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
static struct wlr_texture *client_texture(struct wlr_renderer *renderer,
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
static bool client_buffer_wait(struct vidow_server *server,
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

static struct wlr_texture *client_scene_texture(struct vidow_server *server,
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

static void dump_target(struct vidow_server *server,
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

/* -------------------------------------------------------------- backdrop */

/* Everything that can end up inside a blurred backdrop bumps one of these two
 * counters. Cheap, and it is what lets a still desktop skip the whole kawase
 * chain: an unchanged generation means the cached pyramid is still correct. */
/* ------------------------------------------------- 损伤区域（私有底衬的重抓判据） */

/* 两个盒子有没有重叠（宽高 <= 0 的空盒子永远不重叠）。用 64 位算，因为「整屏都
 * 算」那个盒子故意填得很大。 */
static bool blur_box_touch(const struct wlr_box *a, const struct wlr_box *b) {
	if (a->width <= 0 || a->height <= 0 || b->width <= 0 || b->height <= 0) {
		return false;
	}
	int64_t ax2 = (int64_t)a->x + a->width, ay2 = (int64_t)a->y + a->height;
	int64_t bx2 = (int64_t)b->x + b->width, by2 = (int64_t)b->y + b->height;
	return (int64_t)a->x < bx2 && (int64_t)b->x < ax2 &&
		(int64_t)a->y < by2 && (int64_t)b->y < ay2;
}

/* 把 src 并进 dst（dst 为空就直接是 src）。 */
void blur_box_add(struct wlr_box *dst, const struct wlr_box *src) {
	if (src->width <= 0 || src->height <= 0) {
		return;
	}
	if (dst->width <= 0 || dst->height <= 0) {
		*dst = *src;
		return;
	}
	int x1 = dst->x < src->x ? dst->x : src->x;
	int y1 = dst->y < src->y ? dst->y : src->y;
	int x2 = dst->x + dst->width > src->x + src->width ?
		dst->x + dst->width : src->x + src->width;
	int y2 = dst->y + dst->height > src->y + src->height ?
		dst->y + dst->height : src->y + src->height;
	*dst = (struct wlr_box){ .x = x1, .y = y1, .width = x2 - x1, .height = y2 - y1 };
}

/* 「整屏都算」用的盒子：故意大到碰得到任何东西（不参与算术，只被 blur_box_touch
 * 比大小，那边是 64 位）。 */
static const struct wlr_box blur_box_all = {
	.x = -0x40000000, .y = -0x40000000,
	.width = 0x7fffffff, .height = 0x7fffffff,
};

static bool dmg_box_empty(const struct wlr_box *b) {
	return b->width <= 0 || b->height <= 0;
}

static int64_t dmg_box_area(const struct wlr_box *b) {
	return dmg_box_empty(b) ? 0 : (int64_t)b->width * (int64_t)b->height;
}

/* b 完全在 a 里面。 */
static bool dmg_box_inside(const struct wlr_box *a, const struct wlr_box *b) {
	return a->x <= b->x && a->y <= b->y &&
		a->x + a->width >= b->x + b->width &&
		a->y + a->height >= b->y + b->height;
}

/* 攒着的损伤区域：**不是一个包围盒**。两个客户端在同一帧里各动窗口的一角（一个
 * 在左上、一个在右下），包围盒会把中间那一大片**没变**的地方也算进去，压着它们的
 * 那个窗口底下的模糊就白重抓了 —— 损伤盒子本身做小之后，这个「并集吃掉中间」的
 * 代价反而变成了主要开销。所以留几个盒子（VIDOW_DMG_BOXES 个），装不下了才合并，
 * 合并到「并进去之后总面积最小」的那个上。 */
static void dmg_add(struct vidow_dmg_set *set, const struct wlr_box *box) {
	if (box == NULL || dmg_box_empty(box)) {
		return;
	}
	for (int i = 0; i < set->n; i++) {
		if (dmg_box_inside(&set->box[i], box)) {
			return; /* 已经算进去了 */
		}
	}
	if (set->n < VIDOW_DMG_BOXES) {
		set->box[set->n++] = *box;
		return;
	}
	int best = 0;
	int64_t best_grow = -1;
	for (int i = 0; i < set->n; i++) {
		struct wlr_box merged = set->box[i];
		blur_box_add(&merged, box);
		int64_t grow = dmg_box_area(&merged) - dmg_box_area(&set->box[i]);
		if (best_grow < 0 || grow < best_grow) {
			best_grow = grow;
			best = i;
		}
	}
	blur_box_add(&set->box[best], box);
}

/* 这块区域跟攒下来的任何一笔损伤碰不碰得到。 */
static bool dmg_touch(const struct vidow_dmg_set *set,
		const struct wlr_box *region) {
	for (int i = 0; i < set->n; i++) {
		if (blur_box_touch(&set->box[i], region)) {
			return true;
		}
	}
	return false;
}

/* 攒下来的损伤的包围盒：只给日志用（真正的判据是上面那个逐盒子的碰法）。 */
struct wlr_box dmg_bounds(const struct vidow_dmg_set *set) {
	struct wlr_box b = {0};
	for (int i = 0; i < set->n; i++) {
		blur_box_add(&b, &set->box[i]);
	}
	return b;
}

void dmg_clear(struct vidow_dmg_set *set) {
	set->n = 0;
}

/* 把「刚变了的这块区域」（布局逻辑坐标）记到每个需要私有底衬的元素头上；skip 那
 * 个元素自己不记（它自己的变化不该让它自己的底衬过期 —— 抓的时候它是藏着的）。
 * 记进来的盒子一直攒着，直到那个元素真的重抓一次才清掉：这样「损伤在屏幕另一头」
 * 攒再多也不会让它重抓，而一旦有一笔碰到它那一块，攒下来的这笔账仍然算数。 */
/* 临时探针（VIDOW_DMG_WHY=1）：谁在每帧标脏、标的多大一块。
 *
 * 静止桌面 300fps、损伤 96%/帧，说明有一个「每帧整屏标脏」的调用者：整屏标
 * 脏 → scene commit 有活 → 渲染 → 提交 → 又标脏，自己驱自己。日志里给的
 * 是**调用者的返回地址**（本进程 PIE，偏移 = 地址 - /proc/<pid>/maps 里
 * vidow 的映射起点），用 `addr2line -e build/vidow -f <偏移>` 反解。 */
static void dmg_why_probe(const char *what, const struct wlr_box *box,
		uintptr_t ra) {
	static int64_t last;
	int64_t now = vidow_now_ns();
	if (now - last < 200000000LL) {
		return;
	}
	last = now;
	wlr_log(WLR_INFO, "DMGWHY %s box=%dx%d%+d%+d ra=%#lx", what,
		box != NULL ? box->width : 0, box != NULL ? box->height : 0,
		box != NULL ? box->x : 0, box != NULL ? box->y : 0,
		(unsigned long)ra);
}

static void blur_note_damage(struct vidow_server *server,
		const struct wlr_box *box, const struct vidow_view *skip) {
	if (box == NULL || box->width <= 0 || box->height <= 0) {
		return;
	}
	vidow_stats.dmg_notes++;
	if (getenv("VIDOW_DMG_WHY") != NULL) {
		dmg_why_probe("blur_note_damage", box,
			(uintptr_t)__builtin_return_address(0));
	}
	/* arrange 期间顺手攒一份「这一趟真实变过的世界区域」：arrange 收口时用它
	 * 代替整屏（见 view.c 的 vidow_arrange 和 server.h 的 arrange_damage）。
	 * 整屏哨兵盒（blur_box_all）攒进来没意义 —— 那本来就是「不知道变哪」，
	 * 直接置 force_full，收口时回退整屏。 */
	if (server->arrange_in_progress) {
		if (box->width >= 0x40000000 || box->height >= 0x40000000) {
			server->arrange_force_full = true;
		} else if (server->arrange_damage_any) {
			blur_box_add(&server->arrange_damage, box);
		} else {
			server->arrange_damage = *box;
			server->arrange_damage_any = true;
		}
	}
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view == skip) {
			continue;
		}
		dmg_add(&view->fx.pending_damage, box);
	}
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		dmg_add(&ls->fx.pending_damage, box);
	}
	/* 弹出物背后的模糊也要跟着过期：菜单 / 候选框底下的窗口一动，它自己
	 * 那一块底衬就旧了。弹出物自己交帧时（skip == 它的 view）不记 —— 抓的
	 * 时候它本来就藏着。 */
	struct vidow_popup_blur *pb;
	wl_list_for_each(pb, &server->popup_blurs, link) {
		if (skip != NULL && pb->view == skip) {
			continue;
		}
		dmg_add(&pb->fx.pending_damage, box);
	}
}

void vidow_backdrop_damage_box(struct vidow_server *server,
		const struct wlr_box *box) {
	blur_note_damage(server, box, NULL);
}

/* 只记给**窗口**的底衬，不记给 layer surface。
 *
 * 视口过渡每帧平移整条带子，每个窗口都要报「我换位置了」—— 别的窗口的底衬里
 * 画着它，得重抓，这必须记。但 layer surface（noctalia 的 panel / 通知）**不
 * 跟着视口走**，它们的位置和底衬一点没变；以前一并记给它们，每帧几十笔 × 每个
 * layer，panel 的模糊底衬永远追不上，越堆越多最后卡死（用户：「layer 动画会
 * 卡，而且会死掉（noctalia」）。
 *
 * 而完全不记（5b7d441 的做法）又会漏图：窗口横移之后上一帧画在隔壁屏上的那一
 * 块没人擦，别的窗口底衬里留着它的残影（用户：「窗口聚焦移动的时候也会漏一点
 * 内容到隔壁显示器」）。两头都不对，所以要的是「记给窗口、不记给 layer」。 */
void vidow_backdrop_damage_box_views_only(struct vidow_server *server,
		const struct wlr_box *box) {
	if (box == NULL || box->width <= 0 || box->height <= 0) {
		return;
	}
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		dmg_add(&view->fx.pending_damage, box);
	}
	struct vidow_popup_blur *pb;
	wl_list_for_each(pb, &server->popup_blurs, link) {
		dmg_add(&pb->fx.pending_damage, box);
	}
}

/* 私有底衬要不要因为这次损伤重抓：损伤盒子空 = 没损伤（不用）；否则看它跟
 * 「这张底衬管得着的那块区域」（布局逻辑坐标）碰不碰得到。区域本身已经把模糊
 * 半径算进去了（见 view_region_rect），所以碰到区域就是碰到能影响它的内容。 */
bool blur_damage_hits(const struct vidow_fx_target *fx,
		const struct wlr_box *region) {
	return dmg_touch(&fx->pending_damage, region);
}

/* 窗口「现在画在哪」：布局几何 + 动画偏移（拖拽 / 开合 / overview 都算在这上面）。 */
struct wlr_box vidow_view_screen_box(const struct vidow_view *view) {
	struct wlr_box box = view->geom;
	box.x += (int)lround(view->anim_off_x);
	box.y += (int)lround(view->anim_off_y);
	return box;
}

/* 世界代次前进（损伤区域由调用方决定记哪一块）。 */
void damage_bump(struct vidow_server *server, bool layers) {
	vidow_stats.dmg_bumps++;
	if (getenv("VIDOW_DMG_WHY") != NULL) {
		dmg_why_probe(layers ? "damage_bump(layers)" : "damage_bump(scene)",
			NULL, (uintptr_t)__builtin_return_address(0));
	}
	server->gen_scene++;
	if (layers) {
		server->gen_layer++;
		struct vidow_output *output;
		wl_list_for_each(output, &server->outputs, link) {
			output->backdrop_revision++;
		}
	}
}

/* Layer repaints have an output: preserve the shared caches on other outputs.
 * Keep global scene/layer generations for legacy consumers, but record bounded
 * damage so unrelated private backdrop regions can keep their pixels too. */
void vidow_backdrop_damage_output(struct vidow_output *output) {
	if (output == NULL) {
		return;
	}
	struct vidow_server *server = output->server;
	server->gen_scene++;
	server->gen_layer++;
	output->backdrop_revision++;
	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
	blur_note_damage(server, &box, NULL);
}

/* 只知道「哪块屏、哪一块区域」变了（layer surface 上屏 / 下屏 / 落位 / 内容重画）：
 * 共享底衬必须整块重抓 —— 它是**整屏一份**，没有局部版本，重建判据是
 * output->backdrop_revision（见 fx.c:4381）；但**别人的私有底衬**只在这块盒子真的
 * 碰到它时才过期。
 *
 * 这是把 layer 事件从「整屏标脏」降到「一块盒子」的入口。为什么值得：一条通知
 * 弹出、一个菜单、vshot 的冻结覆盖层上屏，原来都让整屏每个窗口的私有底衬一起
 * 作废重抓。
 *
 * 代价模型（2026-10-28 对着代码核过，别凭印象写）：
 *   - 共享底衬：整屏一份，一次重抓 = 一次全屏 backdrop_render + 整条金字塔，
 *     但它每屏每帧最多一次，所有窗口分摊（shared_blur_rebuild，fx.c:3690）。
 *   - 私有底衬：**不是**再抓一遍屏幕，而是从自己那块 own_bd_tex 上跑
 *     blur_chain / blur_chain_rect，代价正比于窗口矩形（fx.c:4155 一带）。
 *     所以「整屏标脏」真正的代价是**每个窗口各跑一遍自己的金字塔** —— 窗口越
 *     多、2x 屏上越大，越亏。
 *
 * 屏上的真实损伤（要不要重画这一帧）不归这里管：那是 wlroots scene 按节点自己
 * 记的，layer 的 buffer 一交上去 scene 就会标脏。这里只喂模糊那套账本。 */
void vidow_backdrop_damage_output_box(struct vidow_output *output,
		const struct wlr_box *box) {
	if (output == NULL || box == NULL || box->width <= 0 ||
			box->height <= 0) {
		/* 说不清是哪一块：退回整屏，宁可多抓一次也不能糊着旧的 */
		if (output != NULL) {
			vidow_backdrop_damage_output(output);
		}
		return;
	}
	struct vidow_server *server = output->server;
	server->gen_scene++;
	server->gen_layer++;
	output->backdrop_revision++;
	blur_note_damage(server, box, NULL);
}

void vidow_backdrop_damage(struct vidow_server *server, bool layers) {
	damage_bump(server, layers);
	/* 不知道具体变的是哪一块（配置、输出、layer、overview、重排这些）：整屏
	 * 都算。宁可多抓一次，不能糊着旧的 —— 精确的损伤区域见下面两个入口。 */
	blur_note_damage(server, &blur_box_all, NULL);
}

void vidow_backdrop_damage_region(struct vidow_server *server,
		const struct wlr_box *box) {
	damage_bump(server, false);
	blur_note_damage(server, box, NULL);
}

/* 只有某个窗口自己变了的那些改动（它被拖、内容重画、开合动画、果冻形变）：
 * 世界代次照常前进，另外在这个窗口头上单独记一笔。私有底衬抓的是「世界减掉这
 * 个窗口」（抓的时候它自己藏着），所以那张图只跟 gen_scene - self_gen 有关 ——
 * 不记这一笔，拖一个重叠窗口时它自己的底衬每帧都被判过期，白重抓一遍。
 *
 * box == NULL：整块窗口都变了（老行为）。box != NULL：变的**只有**这一块 ——
 * 客户端只重画了窗口里一小块时（见 vidow_view_surface_damage），别人的底衬也
 * 只该因为这一小块过期：屏幕另一头那条 300px 宽的视频进度条，不该让整个窗口
 * 底下那片模糊重抓一遍。 */
void vidow_backdrop_damage_view_rect(struct vidow_view *view,
		const struct wlr_box *box) {
	struct wlr_box b = box != NULL ? *box : vidow_view_screen_box(view);
	/* 果冻在画的时候，屏幕上这一块比窗口框大一圈（留白 + 形变 + 投影）：
	 * 报小了，别人底衬里那一圈就一直是上一版的，松手 / 收尾之后看着像
	 * 滞留像素（见 jelly.c 的 vidow_jelly_pad）。 */
	if (view->jelly != NULL) {
		int pad = (int)ceil(vidow_jelly_pad(view)) + 8;
		b.x -= pad;
		b.y -= pad;
		b.width += 2 * pad;
		b.height += 2 * pad;
	}
	view->self_gen++;
	damage_bump(view->server, false);
	/* 变的是它自己这一块：别人（底衬里画着它）得重抓，它自己不用。 */
	blur_note_damage(view->server, &b, view);
	/* 一次 arrange 里只要有一个「不是正被拖的那个」窗口报了自己变了，这一趟就
	 * 不能整笔算在被拖的那个头上（见 vidow_arrange）。 */
	if (view->server->arrange_self_only && view->server->grab_view != view) {
		view->server->arrange_self_only = false;
	}
}

void vidow_backdrop_damage_view(struct vidow_view *view) {
	vidow_backdrop_damage_view_rect(view, NULL);
}

int vidow_fx_pad(struct vidow_server *server) {
	struct vidow_config *cfg = &server->config;
	if (!cfg->decor_enabled || !server->shader_ok) {
		return 0;
	}
	int off = abs(cfg->shadow_offset_x) > abs(cfg->shadow_offset_y) ?
		abs(cfg->shadow_offset_x) : abs(cfg->shadow_offset_y);
	double radius = cfg->shadow_radius;
	/* 窗口规则可以把某个窗口的阴影调大：内边距按「所有规则里最大的那个」留
	 * （多留一点只是离屏缓冲大一圈，少留就是阴影被裁掉）。规则没写 shadow_*
	 * 的时候这一步等于没做。 */
	const struct vidow_rule *rule;
	wl_list_for_each(rule, &server->rules, link) {
		if (rule->aset[VRA_SHADOW_RADIUS] &&
				rule->aval[VRA_SHADOW_RADIUS].d > radius) {
			radius = rule->aval[VRA_SHADOW_RADIUS].d;
		}
		if (rule->aset[VRA_SHADOW_OFFSET_X]) {
			long long v = rule->aval[VRA_SHADOW_OFFSET_X].i;
			if (labs(v) > off) {
				off = (int)labs(v);
			}
		}
		if (rule->aset[VRA_SHADOW_OFFSET_Y]) {
			long long v = rule->aval[VRA_SHADOW_OFFSET_Y].i;
			if (labs(v) > off) {
				off = (int)labs(v);
			}
		}
	}
	int pad = (int)(radius + off + 2);
	if (pad < 0) {
		pad = 0;
	}
	if (pad > 96) {
		pad = 96;
	}
	return pad;
}

/* 这一帧的序号：共享的 layer 底衬「同一帧只抓一次」。每跑一遍 blur update（每块屏
 * 每帧一次）加一。 */
uint64_t blur_frame_seq = 0;

static void layer_under_drop(struct vidow_output *output) {
	if (output->under_tex != NULL) {
		stat_tex_del(output->under_tex);
		output->under_tex = NULL;
	}
	if (output->under_bd != NULL) {
		wlr_buffer_drop(output->under_bd);
		output->under_bd = NULL;
	}
	output->under_seq = 0;
	output->under_box = (struct wlr_box){0};
	output->under_share = false;
}

void blur_drop(struct vidow_output *output) {
	layer_under_drop(output);
	if (output->backdrop_tex != NULL) {
		stat_tex_del(output->backdrop_tex);
		output->backdrop_tex = NULL;
	}
	if (output->backdrop != NULL) {
		wlr_buffer_drop(output->backdrop);
		output->backdrop = NULL;
		output->backdrop_w = 0;
		output->backdrop_h = 0;
	}
}

void blur_set_drop(struct vidow_blur_set *set) {
	for (int i = 0; i < MAX_BLUR_LEVELS; i++) {
		if (set->down[i] != NULL) {
			wlr_buffer_drop(set->down[i]);
			set->down[i] = NULL;
		}
		if (set->up[i] != NULL) {
			wlr_buffer_drop(set->up[i]);
			set->up[i] = NULL;
		}
		if (set->down_tex[i] != NULL) {
			stat_tex_del(set->down_tex[i]);
			set->down_tex[i] = NULL;
		}
		if (set->up_tex[i] != NULL) {
			stat_tex_del(set->up_tex[i]);
			set->up_tex[i] = NULL;
		}
	}
	set->w = 0;
	set->h = 0;
	set->levels = 0;
	set->gen = 0;
	set->offset = 0.0;
	set->valid = false;
}

bool blur_set_alloc(struct vidow_server *server,
		struct vidow_blur_set *set, int w, int h, int levels) {
	/* 形状没变就**复用**那几块缓冲：拖窗口时重叠的窗口每帧都要重建金字塔
	 * （另一个窗口在它的底衬里动），而 vidow_alloc_target() 走的是
	 * wlr_allocator_create_buffer() —— 真后端上每次都是内核层的 GBM 分配。
	 * 每帧重新分十来块，光分配就够卡了（用户报的「有背景时拖拽卡顿」）。
	 * 复用是安全的：整块重跑时每一趟都会把每块缓冲整个写一遍（begin_pass 先
	 * clear），局部重跑（blur_chain_rect）时**故意**不动那一块以外的像素 ——
	 * 那正是「其余从上一版拷过来」的省法。调用方重画之前已经放掉了攥着 up[0]
	 * 的那张贴图。 */
	if (set->w == w && set->h == h && set->levels == levels &&
			set->down[0] != NULL) {
		set->offset = server->config.blur_offset;
		return true;
	}
	blur_set_drop(set);
	for (int i = 0; i < levels; i++) {
		set->down[i] = vidow_alloc_target(server, w >> (i + 1), h >> (i + 1));
		if (set->down[i] == NULL) {
			blur_set_drop(set);
			return false;
		}
		set->down_tex[i] = stat_tex_new(wlr_texture_from_buffer(
			server->renderer, set->down[i]));
		if (set->down_tex[i] == NULL) {
			blur_set_drop(set);
			return false;
		}
	}
	/* up[i] 和 down[i] 同尺寸：up[i] 是「把 up[i+1]（或者最底层 down[levels-1]）
	 * 放大一圈」的结果，up[0]（半分辨率）就是最终产物，也是给装饰着色器采样的
	 * 那张。down[0..levels-2] 只是链条中途的暂存。 */
	for (int i = 0; i < levels - 1; i++) {
		set->up[i] = vidow_alloc_target(server, w >> (i + 1), h >> (i + 1));
		if (set->up[i] == NULL) {
			blur_set_drop(set);
			return false;
		}
		set->up_tex[i] = stat_tex_new(
			wlr_texture_from_buffer(server->renderer, set->up[i]));
		if (set->up_tex[i] == NULL) {
			blur_set_drop(set);
			return false;
		}
	}
	set->w = w;
	set->h = h;
	set->levels = levels;
	set->offset = server->config.blur_offset;
	return true;
}

/* 一趟 kawase。rect == NULL：整块目标都画（u_rect = 整个目标）；rect != NULL：
 * 只画目标里这一块（别处的像素一个都不碰，见 blur_chain_rect）。 */
static void kawase_pass_rect(struct vidow_server *server,
		struct vidow_gl_prog *prog, struct wlr_buffer *dst, int dst_w, int dst_h,
		struct wlr_texture *src, int src_w, int src_h,
		const struct wlr_box *rect) {
	if (prog->program == 0 || src == NULL) {
		return;
	}
	struct wlr_egl *egl = NULL;
	if (begin_pass_rect(server, dst, dst_w, dst_h, rect, &egl) == 0) {
		return;
	}
	struct wlr_gles2_texture_attribs sa;
	wlr_gles2_texture_get_attribs(src, &sa);

	glUseProgram(prog->program);
	bind_tex(0, sa.target, sa.tex);
	glUniform1i(prog->u_tex, 0);
	glUniform2f(prog->u_texel, 1.0f / (float)src_w, 1.0f / (float)src_h);
	glUniform1f(prog->u_offset, (float)server->config.blur_offset);
	glUniform2f(prog->u_dst, (float)dst_w, (float)dst_h);
	if (rect != NULL) {
		glUniform4f(prog->u_rect, (float)rect->x, (float)rect->y,
			(float)rect->width, (float)rect->height);
	} else {
		glUniform4f(prog->u_rect, 0.0f, 0.0f, (float)dst_w, (float)dst_h);
	}
	draw_quad(prog->program);
	glUseProgram(0);
	end_pass(egl);
}

/* 把 src 坐标系里的矩形换算到 dst 坐标系（尺寸比就是缩放比，这里是 2 或 1/2），
 * 往外取整、再放 margin 一圈、夹进目标里。往外取整和 margin 都是「宁可多算一点，
 * 不能少算」：少算一像素就是新旧模糊值之间一道缝。 */
static struct wlr_box kawase_rect_scale(const struct wlr_box *r,
		int sw, int sh, int dw, int dh, int margin) {
	if (sw <= 0 || sh <= 0) {
		return (struct wlr_box){0};
	}
	double fx = (double)dw / (double)sw;
	double fy = (double)dh / (double)sh;
	int x1 = (int)floor((double)r->x * fx) - margin;
	int y1 = (int)floor((double)r->y * fy) - margin;
	int x2 = (int)ceil((double)(r->x + r->width) * fx) + margin;
	int y2 = (int)ceil((double)(r->y + r->height) * fy) + margin;
	if (x1 < 0) {
		x1 = 0;
	}
	if (y1 < 0) {
		y1 = 0;
	}
	if (x2 > dw) {
		x2 = dw;
	}
	if (y2 > dh) {
		y2 = dh;
	}
	if (x2 <= x1 || y2 <= y1) {
		return (struct wlr_box){0};
	}
	return (struct wlr_box){
		.x = x1, .y = y1, .width = x2 - x1, .height = y2 - y1,
	};
}

/* 跑一遍链条。rect == NULL：整块（老行为）。rect != NULL：只有这块区域里的
 * **像素内容变了**，别处保持上一版 —— 每一级只重画「上一级变了的那一块缩一半再
 * 放一圈」的矩形，其余像素不动。
 *
 * margin 用的是整条链的采样半径按级别折算（blur_reach_px >> (i+1)）+ 4：真实需要
 * 的半径小得多（u_offset 个源 texel，1.5 个像素级），这是**够用且宽松**的上界 ——
 * 局部重跑绝不能因为margin 算少了而在新旧模糊值之间留一道缝。 */
void blur_chain_rect(struct vidow_server *server,
		struct vidow_blur_set *set, struct wlr_texture *backdrop_tex,
		int w, int h, const struct wlr_box *rect, int reach) {
	int levels = set->levels;
	if (levels < 2 || backdrop_tex == NULL) {
		return;
	}
	vidow_stats.chains++;
	struct wlr_box r = rect != NULL ? *rect : (struct wlr_box){0};
	struct wlr_texture *tex = backdrop_tex;
	for (int i = 0; i < levels; i++) {
		int sw = i == 0 ? w : (w >> i), sh = i == 0 ? h : (h >> i);
		int dw = w >> (i + 1), dh = h >> (i + 1);
		struct wlr_box dst_rect = {0};
		if (rect != NULL) {
			dst_rect = kawase_rect_scale(&r, sw, sh, dw, dh,
				(reach >> (i + 1)) + 4);
			if (dst_rect.width <= 0 || dst_rect.height <= 0) {
				return; /* 越缩越小：下面几级也没东西可算 */
			}
		}
		kawase_pass_rect(server, &server->kawase_down, set->down[i], dw, dh,
			tex, sw, sh, rect != NULL ? &dst_rect : NULL);
		/* 这一趟的结果就是下一趟的源：贴图是常驻的（见 vidow_blur_set），
		 * 直接换指针 —— 以前这里每趟导入一张、用完销毁，真后端上每帧十几二十次
		 * EGLImage + BO 映射，拖重叠窗口时的卡顿就是这个。 */
		tex = set->down_tex[i];
		r = dst_rect;
	}

	/* 往上走：每一级只把上一级的结果放大一圈（niri 的 blur_up）。目标必须比源大一
	 * 倍，u_texel 传的是**源**的尺寸 —— 放大倍数全靠它算。每一级都是同一趟着色器，
	 * 写 up[0]（半分辨率、给装饰着色器采样的那张）也不例外。 */
	struct wlr_texture *lower = set->down_tex[levels - 1];
	for (int i = levels - 2; i >= 0; i--) {
		int dw = w >> (i + 1), dh = h >> (i + 1);
		int sw = w >> (i + 2), sh = h >> (i + 2);
		struct wlr_box dst_rect = {0};
		if (rect != NULL) {
			dst_rect = kawase_rect_scale(&r, sw, sh, dw, dh,
				(reach >> (i + 1)) + 4);
			if (dst_rect.width <= 0 || dst_rect.height <= 0) {
				return;
			}
		}
		kawase_pass_rect(server, &server->kawase_up, set->up[i], dw, dh,
			lower, sw, sh, rect != NULL ? &dst_rect : NULL);
		lower = set->up_tex[i];
		r = dst_rect;
	}
}

void blur_chain(struct vidow_server *server, struct vidow_blur_set *set,
		struct wlr_texture *backdrop_tex, int w, int h) {
	blur_chain_rect(server, set, backdrop_tex, w, h, NULL, 0);
}

/* Render the layer trees below the toplevels into the backdrop buffer. Doing
 * this by hand instead of via wlr_scene_output_build_state() keeps the output's
 * damage ring untouched and lets us pick exactly which trees contribute. */

/* 局部更新要求「只画一块」这件事对每个要抓的 buffer 都成立：转过的客户端缓冲
 * （wl_surface.set_buffer_transform）得把 dst 子矩形映射回转置过的 src 框，不值得
 * 为它写一套。碰到就整块重跑。 */
void backdrop_clip_check(struct wlr_scene_buffer *sb, int sx, int sy,
		void *data) {
	(void)sx;
	(void)sy;
	bool *ok = data;
	if (sb->buffer != NULL && sb->transform != WL_OUTPUT_TRANSFORM_NORMAL) {
		*ok = false;
	}
}

void backdrop_iter(struct wlr_scene_buffer *sb, int sx, int sy,
		void *data) {
	struct backdrop_ctx *ctx = data;
	if (sb->buffer == NULL) {
		return;
	}
	bool skip_self = false;
	if (ctx->skip != NULL && ctx->skip->scene_tree != NULL) {
		for (struct wlr_scene_node *n = &sb->node; n != NULL;
				n = n->parent != NULL ? &n->parent->node : NULL) {
			if (n == &ctx->skip->scene_tree->node) {
				skip_self = true;
				break;
			}
		}
	}
	if (ctx->skip != NULL && (skip_self ||
			sb == ctx->skip->fx.node ||
			(ctx->skip->scene_surface != NULL &&
			 sb == ctx->skip->scene_surface->buffer))) {
		return;
	}
	/* 节点尺寸优先用场景给的目标尺寸（逻辑像素）；客户端缓冲自己带倍率的
	 * 情况下（缓冲 1280x720、逻辑 640x360）退到 surface 的逻辑尺寸，
	 * 别把设备像素当逻辑像素用。 */
	int w = sb->dst_width > 0 ? sb->dst_width : 0;
	int h = sb->dst_height > 0 ? sb->dst_height : 0;
	if (w <= 0 || h <= 0) {
		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
		if (ss != NULL) {
			w = ss->surface->current.width;
			h = ss->surface->current.height;
		} else {
			w = sb->buffer->width;
			h = sb->buffer->height;
		}
	}
	if (w <= 0 || h <= 0) {
		return;
	}
	/* 按**边**取整（而不是位置 + 宽度各自取整），相邻的两块 buffer 之间就不会
	 * 因为各自四舍五入而露出一像素缝。 */
	int x1 = (int)lround((double)sx * ctx->scale) - ctx->ox;
	int y1 = (int)lround((double)sy * ctx->scale) - ctx->oy;
	int x2 = (int)lround((double)(sx + w) * ctx->scale) - ctx->ox;
	int y2 = (int)lround((double)(sy + h) * ctx->scale) - ctx->oy;
	if (x2 <= x1 || y2 <= y1) {
		return;
	}
	if (ctx->clip_w > 0 &&
			(x1 >= ctx->clip_x + ctx->clip_w || x2 <= ctx->clip_x ||
			 y1 >= ctx->clip_y + ctx->clip_h || y2 <= ctx->clip_y)) {
		return; /* 整块都在区域外：省下一次建贴图 */
	}
	/* 也是客户端/别人的缓冲：同样借 wrapper 的贴图（见 client_texture 的说明，
	 * dmabuf 客户端对 source 另建的那张会一直是旧内容）。 */
	bool tex_owned = true;
	struct wlr_texture *tex = client_scene_texture(ctx->server, sb, &tex_owned);
	if (tex == NULL) {
		return;
	}
	float alpha = sb->opacity;
	/* 只采客户端声明的有效区域（wp_viewport 的 src 框，没设就是整块缓冲）。
	 * 不采的话，缓冲比有效内容大一圈的客户端（chromium 分数缩放）会把没画过的
	 * 那圈像素也画进底衬 —— 后面模糊采到的就是那圈黑。 */
	struct wlr_fbox sbox = sb->src_box;
	if (sbox.width <= 0.0 || sbox.height <= 0.0) {
		sbox = (struct wlr_fbox){0, 0, (double)sb->buffer->width,
			(double)sb->buffer->height};
	}
	/* 只画 clip 里那一块。局部更新（clip = 受损那一小块）时必须真的夹住：底衬
	 * 缓冲里 clip 外面是上一版的内容、没清过，整块重画一遍压在它上面，半透明的
	 * 东西（窗口自己的阴影、薄纱窗口）就会被叠第二次 —— 屏幕上就是「局部更新之后
	 * 那一圈变深了」。夹 dst 框就得按同样的比例夹 src 框，不然画面会跟着缩。 */
	if (ctx->clip_w > 0 &&
			(x1 < ctx->clip_x || y1 < ctx->clip_y ||
			 x2 > ctx->clip_x + ctx->clip_w ||
			 y2 > ctx->clip_y + ctx->clip_h)) {
		int cx1 = x1 > ctx->clip_x ? x1 : ctx->clip_x;
		int cy1 = y1 > ctx->clip_y ? y1 : ctx->clip_y;
		int cx2 = x2 < ctx->clip_x + ctx->clip_w ? x2 : ctx->clip_x + ctx->clip_w;
		int cy2 = y2 < ctx->clip_y + ctx->clip_h ? y2 : ctx->clip_y + ctx->clip_h;
		if (cx2 <= cx1 || cy2 <= cy1) {
			if (tex_owned) {
				wlr_texture_destroy(tex);
			}
			return;
		}
		double fx1 = (double)(cx1 - x1) / (double)(x2 - x1);
		double fx2 = (double)(cx2 - x1) / (double)(x2 - x1);
		double fy1 = (double)(cy1 - y1) / (double)(y2 - y1);
		double fy2 = (double)(cy2 - y1) / (double)(y2 - y1);
		sbox.x += fx1 * sbox.width;
		sbox.y += fy1 * sbox.height;
		sbox.width *= fx2 - fx1;
		sbox.height *= fy2 - fy1;
		x1 = cx1;
		y1 = cy1;
		x2 = cx2;
		y2 = cy2;
	}
	struct wlr_render_texture_options opts = {
		.texture = tex,
		.src_box = sbox,
		.dst_box = { .x = x1, .y = y1, .width = x2 - x1, .height = y2 - y1 },
		.alpha = &alpha,
		.transform = sb->transform,
		.filter_mode = WLR_SCALE_FILTER_BILINEAR,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	};
	wlr_render_pass_add_texture(ctx->pass, &opts);
	if (tex_owned) {
		wlr_texture_destroy(tex);
	}
}

bool backdrop_render(struct vidow_server *server,
		struct vidow_output *output, int w, int h) {
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, output->backdrop, NULL);
	if (pass == NULL) {
		return false;
	}
	struct wlr_render_rect_options clear = {
		.box = { .x = 0, .y = 0, .width = w, .height = h },
		.color = { .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f },
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	};
	wlr_render_pass_add_rect(pass, &clear);

	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	struct backdrop_ctx ctx = {
		.pass = pass,
		.server = server,
		.scale = scale,
		.ox = (int)lround((double)output->box.x * scale),
		.oy = (int)lround((double)output->box.y * scale),
	};
	struct wlr_scene_tree *trees[] = {
		server->layer_background, server->layer_bottom, server->layer_toplevels,
	};
	for (size_t i = 0; i < sizeof(trees) / sizeof(trees[0]); i++) {
		wlr_scene_node_for_each_buffer(&trees[i]->node, backdrop_iter, &ctx);
	}
	return wlr_render_pass_submit(pass);
}

/* 窗口的内容是不是**整块不透明**。两种确定的情形：
 *   1. 格式里没 alpha（XRGB / RGBX）：每个像素的 alpha 都是 1；
 *   2. 客户端声明了盖住可见窗口那一块的 opaque region（GTK / Qt 那套）：opaque
 *      region 就是「这里 alpha == 1」的承诺，坐标是表面局部（wlroots 在
 *      types/scene/surface.c 里按表面尺寸设的），所以拿窗口在表面里的那块比。
 * 两个地方要问这个：直通（vidow_view_scanout_ok —— 底下画什么一个像素都透不出来）
 * 和模糊（view_blur_visible —— 模糊被 content.a 乘成 0）。 */
static bool view_content_opaque(struct vidow_view *view,
		const struct vidow_fx_geo *geo) {
	struct wlr_scene_buffer *src = view->scene_surface != NULL ?
		view->scene_surface->buffer : NULL;
	if (src == NULL || src->buffer == NULL) {
		return false;
	}
	if (wlr_buffer_is_opaque(src->buffer)) {
		return true;
	}
	pixman_box32_t want = {
		.x1 = geo->off_x, .y1 = geo->off_y,
		.x2 = geo->off_x + geo->w, .y2 = geo->off_y + geo->h,
	};
	return pixman_region32_contains_rectangle(&src->opaque_region, &want) ==
		PIXMAN_REGION_IN;
}

/* A window only needs blur when something can show through it. Floating
 * windows always can; tiled ones only when blur_all is set, because their
 * backdrop is the wallpaper and the layer surfaces. */
static bool view_wants_blur_ever(struct vidow_view *view) {
	struct vidow_look look;
	vidow_view_look(&view->server->config, &view->rule_eff, view->fullscreen,
		&look);
	/* blur 那条规则在这里生效：false = 这个窗口背后不模糊（视频、终端那种
	 * 「糊了更难看」的），true = 平铺也模糊（等于给这一个窗口开 blur_all）。 */
	if (!look.blur || !view->mapped) {
		return false;
	}
	/* overview 里不采样背景模糊：UV 是按窗口自己的位置算的，窗口缩小之后对不上。
	 * 注意是「这一刻不给贴图」而不是把 fx->blur 关掉 —— 那个标志是 fx_begin()
	 * 按配置设的，改它的话退出 overview 之后就再也恢复不了（踩过：overview 一趟
	 * 回来，所有窗口的装饰底衬都不模糊了）。 */
	if (view->rule_eff.set[VRA_BLUR] ||
			view->server->config.blur_all ||
			view->float_mode != VIDOW_FLOAT_NONE) {
		return true;
	}
	/* 客户端自己开口要的（ext-background-effect），blur_all 关着也得给 */
	return view->fx.blur_region_on;
}

/* 给 drag.c 用：这个窗口会不会有模糊底衬。拖拽时模糊窗口保持 alpha 1.0 ——
 * 降到 0.75 会让烘好的模糊和未模糊的场景混成「一层雾」（用户报过）。
 * 判据不能只看 fx.blur_tex：刚 map / 刚换屏时那张贴图可能还没分配，浮窗
 * 拖动正好会踩到，所以按配置 / 规则算。 */
bool vidow_view_blur_wanted(struct vidow_view *view) {
	return view != NULL && view->fx.active && view_wants_blur_ever(view);
}

/* 这笔模糊看得见吗 —— 看不见就别算（整屏 Kawase 金字塔 + 私有底衬抓取 +
 * 「底衬版本变了就重烘这个窗口」全是白花的钱）。
 *
 * 装饰着色器最后两句是 out_px = content + out_px * (1 - content.a)，而模糊那一项
 * 还乘着 inside 和 step(u_mask_min, elem_a)：**内容不透明的地方（content.a == 1）
 * 模糊被乘成 0，内容全透明的地方又被剔除**（模糊只画在元素真有内容的地方）。所以
 * 模糊只在**内容半透明**的窗口上看得见 —— 终端背景 0.8、播放器控制条、半透明
 * 圆角窗口那些才是真的要看模糊的。
 *
 * 判据保守：**拿不准就当看得见**（照旧算模糊，行为不变）。判成看不见的只有
 * view_content_opaque() 那两种确定情形（XRGB/RGBX，或整块 opaque region）—— 它们
 * 保证窗口里每个像素 alpha == 1。客户端只声明了部分 opaque region 的（圆角窗口
 * 四个角、CSD 阴影留白）照旧算模糊：那几处其实也是全透明、模糊照样被剔掉，但不读
 * 像素就没法确认，不值得为它冒险。 */
static bool view_blur_visible(struct vidow_view *view) {
	/* 形状被改过（果冻网格）/ 抓图正压着：看不清内容是不是不透明。 */
	if (view->jelly != NULL || view->mask_opacity) {
		return true;
	}
	/* 自定义 shader（开 / 关 / idle）可能把内容画淡，装饰也归它画。 */
	if (view->fx.shader != NULL) {
		return true;
	}
	/* 动画期间内容是快照 / 被缩放，alpha 也未必是 1。 */
	if (view->anim_open != NULL || view->anim_move != NULL) {
		return true;
	}
	/* alpha 恰好 0 = 一个像素都画不出来：整条抓取 + 链条都是白烧的。
	 * 0 和 0.001 不一样 —— 后者还要留（模糊会从里面透出来一点）。 */
	if (view->anim_alpha <= 0.0) {
		return false;
	}
	if (view->anim_alpha < 1.0) {
		return true;
	}
	struct vidow_look look;
	vidow_view_look(&view->server->config, &view->rule_eff, view->fullscreen,
		&look);
	/* 整个窗口被画淡了（window_opacity）：模糊从里面透出来。 */
	if (look.opacity <= 0.0) {
		return false;
	}
	if (look.opacity < 1.0) {
		return true;
	}
	if (view->scene_surface == NULL || view->scene_surface->buffer == NULL) {
		return true;
	}
	struct vidow_fx_geo geo = vidow_view_fx_geo(view);
	return !view_content_opaque(view, &geo);
}

/* 这个窗口现在要不要采样模糊底衬。上面那份不看 fx 开没开 —— 全屏直通的判据要在
 * 「fx 已经被收掉」的时候也能问出答案（见 vidow_view_scanout_ok），所以分开。 */
/* 被一个**内容不透明**的窗口整块盖住了吗（z 序在上、把它的框完全盖住）。
 *
 * 完全盖住 = 它的模糊一个像素都透不出来：装饰着色器只在内容 alpha < 1 的地方
 * 采样底衬，而那些地方上面那个不透明的窗口挡着。最有用的现场是「全屏游戏盖住
 * 底下一排浮窗」—— 以前那些窗口的私有底衬每帧照抓、kawase 照跑，重叠的 N 个
 * 窗口各抓一份本来就是 O(n²)，这条把「根本看不见的那些」直接摘掉。
 *
 * 判据宁严：不透明 + 完全覆盖 + z 序在上，三条都成立才算。判错的代价是「本该
 * 模糊的地方不糊」，拿不准一律当没被盖住。 */
static struct vidow_view *view_from_scene_node(struct wlr_scene_node *node) {
	if (node == NULL || node->data == NULL) {
		return NULL;
	}
	struct vidow_view_tag *tag = node->data;
	if (tag->magic != VIDOW_VIEW_TAG_MAGIC) {
		return NULL;
	}
	return tag->view;
}

static bool view_fully_occluded(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	if (!view->mapped || !view->placed || view->popup ||
			view->scene_tree == NULL || view->mini != VIDOW_MINI_NONE) {
		return false;
	}
	if (server->layer_toplevels == NULL) {
		return false;
	}
	struct wlr_box mine = vidow_view_screen_box(view);
	struct wlr_scene_node *n;
	/* wlroots 的 children 是「先画在下、后画在上」（raise_to_top 是 append），
	 * 所以倒着走就是从最上面往下：**碰到自己之前**走过的那些才在自己上层。
	 * 碰到自己就停 —— 后面都是被自己挡住的，盖不盖住自己都看不见。 */
	wl_list_for_each_reverse(n, &server->layer_toplevels->children, link) {
		if (n == &view->scene_tree->node) {
			break;
		}
		if (!n->enabled) {
			continue;
		}
		struct vidow_view *o = view_from_scene_node(n);
		if (o == NULL || !o->mapped || !o->placed ||
				o->mini != VIDOW_MINI_NONE || o->anim_alpha < 1.0 ||
				o->mask_opacity || o->jelly != NULL) {
			continue;
		}
		struct wlr_box b = vidow_view_screen_box(o);
		if (b.x > mine.x || b.y > mine.y ||
				b.x + b.width < mine.x + mine.width ||
				b.y + b.height < mine.y + mine.height) {
			continue;
		}
		struct vidow_fx_geo geo = vidow_view_fx_geo(o);
		if (view_content_opaque(o, &geo)) {
			return true;
		}
	}
	return false;
}

/* 这个窗口现在要不要采样模糊底衬。上面那份不看 fx 开没开 —— 全屏直通的判据要在
 * 「fx 已经被收掉」的时候也能问出答案（见 vidow_view_scanout_ok），所以分开。 */
static bool view_wants_blur(struct vidow_view *view) {
	return view->fx.active && view_wants_blur_ever(view) &&
		view_blur_visible(view) && !view_fully_occluded(view);
}

/* 直通诊断：环境变量 VIDOW_SCANOUT_TRACE=1，或者 /tmp/vidow-scanout-trace 这个
 * 文件出现（文件方式不用重启会话，见 xtrace.c 那一套）。开着的时候每秒给每个
 * 窗口一行：判据卡在哪一项。游戏到底有没有直通扫屏，看这个就知道 —— 没直通
 * 就是每帧一次整屏合成 + 一次装饰离屏渲染，那才是游戏里真正的浪费。 */
static bool scanout_trace_on(void) {
	static int64_t last_ns = 0;
	static bool on = false;
	int64_t now = vidow_now_ns();
	if (last_ns == 0 || now - last_ns > 1000 * 1000000LL) {
		last_ns = now;
		on = getenv("VIDOW_SCANOUT_TRACE") != NULL ||
			access("/tmp/vidow-scanout-trace", F_OK) == 0;
	}
	return on;
}

/* VIDOW_NO_SCANOUT=1：一律走合成那条路（探针，见 vidow_view_scanout_ok）。 */
bool scanout_off(void) {
	static int off = -1;
	if (off < 0) {
		off = getenv("VIDOW_NO_SCANOUT") != NULL ? 1 : 0;
	}
	return off == 1;
}

/* 这个窗口这一帧能不能「直通」：屏幕上该显示的像素就是客户端缓冲自己 —— 全屏、
 * 装饰参数全零、内容整块不透明、没有动画 / 果冻 / 遮罩、没有裁剪。满足的话 view.c 就
 * 把装饰节点收起来（不烘那一趟离屏渲染），场景里只剩客户端表面这一个节点，
 * wlroots 的 scene 就有机会把它的缓冲直接扫上屏（见 wlr_scene.c 的
 * scene_entry_try_direct_scanout：要求这一帧的 render list 正好一项、且它是
 * 盖住整块输出的 buffer）。
 *
 * **判据宁严不宽**：漏判只是没省下那两笔 GPU（每帧一次整屏离屏渲染 + 一次输出
 * 合成），误判会让画面变样 —— 装饰、模糊、CSD 留白、半透明、动画里任何一项在
 * 直通下都会露出来（合成那条路把它们画进了离屏缓冲，直通扫的是原始缓冲）。
 *
 * 切换本身是无感的：判据成立时「合成出来的像素」和「客户端缓冲本身」一样，
 * 所以进出直通两个方向都看不出变化（细节见 view.c 的 view_scanout_switch）。 */
bool vidow_view_scanout_ok(struct vidow_view *view) {
	if (view == NULL || !view->mapped || view->popup) {
		return false;
	}
	/* 关掉直通的开关（和 VIDOW_RENDER_STATS 一样的探针）：怀疑「全屏画面和
	 * 以前不一样」时先拿它排除直通这条路 —— 两个方向应该一个像素都不差。 */
	if (scanout_off()) {
		return false;
	}
	/* 小窗（mini window，S3）：一律走合成。直通的定义是「屏幕上该显示的像素就是
	 * 缓冲自己」，而小窗要的正是「把原生 buffer 缩进一个小盒子」—— 两者互斥。
	 *
	 * **必须放在 fullscreen 判断之前**：小窗态刻意保留 fullscreen 标志（不重配
	 * 客户端、不发 set_fullscreen(false)，见 float.c 的 on_commit），全屏游戏进
	 * 小窗之后这条判据照样成立 —— 不挡住的话场景会把 2560x1440 的原生 buffer
	 * 整块扫上屏，小窗化完全失效。 */
	if (view->mini != VIDOW_MINI_NONE) {
		return false;
	}
	/* 只做全屏：只有它铺满整块输出（wlroots 要求节点的框正好等于输出框）。 */
	if (!view->fullscreen || view->scene_parked) {
		return false;
	}
	/* 全景里窗口是被缩小的（ov 那个系数写在 view_anim_place 的缩放里，而那条路
	 * 挂在 fx.active 上）—— 直通里没人给它缩放，全屏窗口会以原尺寸糊在全景上。 */
	if (vidow_overview_active(view->server)) {
		return false;
	}
	if (view->scene_tree == NULL || view->scene_surface == NULL) {
		return false;
	}
	struct wlr_scene_buffer *src = view->scene_surface->buffer;
	if (src == NULL || src->buffer == NULL) {
		return false;
	}
	/* 动画 / 果冻 / 遮罩期间内容不是「缓冲自己」，得走合成那条路。 */
	if (view->anim_open != NULL || view->anim_move != NULL ||
			view->jelly != NULL || view->mask_opacity) {
		return false;
	}
	if (view->anim_alpha < 1.0 || view->anim_scale != 1.0) {
		return false;
	}
	if (view->fx.shader != NULL) {
		return false;
	}
	/* 裁过（跨屏那一半 / overview 里缩放）：这条**不能删**，但也不能像
	 * 以前那样一律退回合成管线 —— 无装饰的窗口（Steam 主窗口就是）退回
	 * 去以后 fx 那条路拿不到 decorated 图，整块画不出来，屏幕上就是黑的。
	 *
	 * 实测：Steam 首开 clip_on=0 走直通、画面正常；关掉再开 clip_on=1
	 * 被踢到合成管线，纹理有 3808x2050 内容（x11-scene 里 tex 非空、
	 * dst 也对），屏幕上却什么都没有 —— 而且 XWayland 只交 1 帧就停，
	 * 因为没人给它 frame_done。
	 *
	 * 直通其实能裁：把裁剪框写到场景节点的裁剪属性上就行，不用绕离屏
	 * 缓冲。下面 crop 那段就是干这个的。 */
	/* 只在**真的要裁掉一块**时才拒绝直通（窗口整块在屏内时裁不裁无所谓，
	 * 硬退回合成管线纯属自找黑屏）。 */
	if (view->fx.clip_on && view->fx.clip.width > 0 &&
			view->fx.clip.height > 0) {
		struct wlr_box vis = {0};
		struct wlr_box node_box = {
			.x = view->geom.x, .y = view->geom.y,
			.width = view->geom.width,
			.height = view->geom.height,
		};
		if (wlr_box_intersection(&vis, &node_box, &view->fx.clip) &&
				(vis.width < node_box.width ||
					vis.height < node_box.height)) {
			if (vidow_xtrace_on()) {
				wlr_log(WLR_INFO, "scanout: %s 拒绝直通 —— 真被切掉一块 "
					"vis=%d,%d %dx%d node=%d,%d %dx%d clip=%d,%d %dx%d",
					vidow_view_title(view) != NULL ?
						vidow_view_title(view) : "(无题)",
					vis.x, vis.y, vis.width, vis.height,
					node_box.x, node_box.y,
					node_box.width, node_box.height,
					view->fx.clip.x, view->fx.clip.y,
					view->fx.clip.width, view->fx.clip.height);
			}
			return false; /* 真的被切掉一块：直通画不了 */
		}
	}
	struct vidow_look look;
	vidow_view_look(&view->server->config, &view->rule_eff, view->fullscreen,
		&look);
	if (look.corner_radius > 0.0 || look.border_width > 0.0 ||
			look.shadow_radius > 0.0 || look.noise > 0.0) {
		return false;
	}
	if (look.opacity < 1.0) {
		return false;
	}
	/* 客户端自己画装饰（GTK 那套 CSD 阴影）时表面比窗口大一圈：合成那条路按
	 * 窗口几何裁掉留白，直通扫的是整块缓冲 —— 那圈留白会露出来。 */
	struct vidow_fx_geo geo = vidow_view_fx_geo(view);
	if (geo.off_x != 0 || geo.off_y != 0) {
		return false;
	}
	if (geo.w != geo.src_w || geo.h != geo.src_h) {
		return false;
	}
	/* 缓冲的设备像素得和这块输出对上（1:1 上屏）。客户端没吃下输出缩放（1x 的
	 * 客户端放在 2x 屏上）时，合成那条路是**着色器**把 1x 贴图放大到 2x、直通
	 * 那条路是**场景**自己放大 —— 两条路的采样未必一模一样，这一步不省。 */
	struct wlr_output *out = view->output;
	if (out == NULL) {
		return false;
	}
	int ow = out->width, oh = out->height;
	if (out->transform % 2 == 1) {
		int t = ow;   /* 90 / 270：长宽对调 */
		ow = oh;
		oh = t;
	}
	if (src->buffer->width != ow || src->buffer->height != oh) {
		return false;
	}
	/* 内容得**整块不透明**：全屏窗口盖住整块屏，只有内容真的不透明，底衬和模糊
	 * 才一个像素都透不出来 —— 合成那条路也只是拿 content 盖住它（装饰着色器里
	 * content.a == 1 时 blurred 那一项被乘成 0），所以直通少了模糊那一趟也看不
	 * 出差别。默认配置 blur_all 开着（平铺窗口也糊壁纸），全屏窗口照样被算成
	 * 「要模糊」，可那笔模糊一个像素都透不出来，为它留着合成不值。
	 *
	 * 两种「不透明」：格式里就没 alpha（XRGB / RGBX，播放器和游戏的常规选择），
	 * 或者客户端自己声明了盖住整块表面的 opaque region（GTK 那套：格式带 alpha，
	 * 但声明整块不透明）。后者 wlroots 判「这个节点盖住了下面的东西」用的也是它
	 * （见 wlr_scene.c 的 scene_node_opaque_region）—— 它不认，直通也轮不到。 */
	bool ok = view_content_opaque(view, &geo);
	if (scanout_trace_on()) {
		static int64_t last_ns = 0;
		int64_t now = vidow_now_ns();
		if (now - last_ns > 1000 * 1000000LL) {
			last_ns = now;
			wlr_log(WLR_INFO, "直通: %s -> %s | 全屏=%d mini=%d 停放=%d "
				"overview=%d 动画=%d 果冻=%d 遮罩=%d shader=%d 裁剪=%d "
				"look(r=%.1f bw=%.1f sr=%.1f noise=%.1f op=%.2f) "
				"留白=%d,%d 几何=%dx%d 缓冲=%dx%d 输出=%s %dx%d 不透明=%d",
				vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
				ok ? "直通" : "合成",
				view->fullscreen, (int)view->mini, view->scene_parked,
				vidow_overview_active(view->server),
				view->anim_open != NULL || view->anim_move != NULL,
				view->jelly != NULL, view->mask_opacity,
				view->fx.shader != NULL, view->fx.clip_on,
				look.corner_radius, look.border_width, look.shadow_radius,
				look.noise, look.opacity, geo.off_x, geo.off_y,
				geo.w, geo.h, src->buffer->width, src->buffer->height,
				out->name != NULL ? out->name : "?",
				out->width, out->height, ok);
		}
	}
	if (vidow_xtrace_on()) {
		wlr_log(WLR_INFO, "scanout: %s 结果=%d（走%s）clip_on=%d "
			"look: r=%.1f bw=%.1f sr=%.1f noise=%.1f",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)", ok,
			ok ? "直通" : "合成管线", view->fx.clip_on,
			look.corner_radius, look.border_width,
			look.shadow_radius, look.noise);
	}
	return ok;
}

/* Hide/show a toplevel for a backdrop capture. A decorated window is drawn
 * from its fx node and its real surface already sits at zero opacity, so
 * hiding the fx node is enough; an undecorated one draws from the surface. */
static void view_set_capture_visible(struct vidow_view *view, bool on) {
	if (view->fx.active && view->fx.node != NULL) {
		wlr_scene_node_set_enabled(&view->fx.node->node,
			on && view->fx.blur_slot == NULL);
		for (struct vidow_blur_slot *sl = view->fx.blur_slot; sl; sl = sl->next) {
			if (sl->node == NULL) continue;
			if (!on) sl->capture_enabled = sl->node->node.enabled;
			wlr_scene_node_set_enabled(&sl->node->node,
				on && sl->capture_enabled);
		}
	} else if (view->scene_tree != NULL) {
		wlr_scene_node_set_enabled(&view->scene_tree->node, on && view->mapped);
	}
	/* 拖着果冻的时候，窗口的内容由果冻节点画 —— 它也得跟着藏，否则抓出来的
	 * 底衬里带着这个窗口自己上一帧的形变图（透明窗口一拖：背景里错位的竖线、
	 * 重影，模糊看着像没了）。 */
	vidow_jelly_capture_visible(view, on);
}

/* A window whose box intersects another window cannot use the shared
 * wallpaper-only backdrop: it would lose the window underneath it. Those get
 * a private capture instead. Tiled windows never overlap, so the shared one
 * covers the whole scrolling layout. */
/* 重叠判据的实测成本（VIDOW_RENDER_STATS 里报）。
 *
 * 只统计 + 只在读的时候清零，不改变任何行为。为什么要有它：这个判据每帧
 * 每个走模糊的窗口各一次，历史上被当成「可能很贵」反复讨论，但一直没有
 * 数字。有了 ns/次 和 次/秒，谈优化才有依据。 */
uint64_t ov_ns;
uint64_t ov_n;

/* 只统计开着统计时才量：每次 clock_gettime 两回，量级和判据本身同阶（实测
 * 判据 ~20-40ns，两次 clock_gettime 再加 ~40ns），常态挂着就是白付一倍。 */
bool ov_stats_on(void) {
	static int on = -1;
	if (on < 0) {
		on = getenv("VIDOW_RENDER_STATS") != NULL ? 1 : 0;
	}
	return on != 0;
}

void vidow_fx_take_overlap_cost(uint64_t *ns, uint64_t *n) {
	*ns = ov_ns;
	*n = ov_n;
	ov_ns = 0;
	ov_n = 0;
}

static bool view_overlaps_other(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	struct wlr_box a = vidow_view_screen_box(view);
	struct vidow_view *other;
	wl_list_for_each(other, &server->views, link) {
		if (other == view || !other->mapped || !other->placed) {
			continue;
		}
		/* **不能按「归属哪块屏」筛**：跨屏拖动时窗口已经画在对面那块屏上了，
		 * 而 view->output 还是原来那块（布局归属要等松手才换）。按归属筛的话，
		 * 它压在对面那块屏的窗口上时，对面这些窗口全被跳过，判定成「不重叠」，
		 * 于是切去共享的整屏壁纸底衬 —— 模糊采不到对方的内容，只剩壁纸
		 * （用户报的「拖拽背景模糊跨显示器没有采样另一个显示器的内容」）。
		 *
		 * vidow_view_screen_box() 给的本来就是**场景全局坐标**，跨屏天然可比，
		 * 直接判相交就够了。 */
		struct wlr_box b = vidow_view_screen_box(other);
		if (a.x < b.x + b.width && b.x < a.x + a.width &&
				a.y < b.y + b.height && b.y < a.y + a.height) {
			return true;
		}
	}
	return false;
}

/* Point a window at a blurred backdrop. Nothing is redrawn while the texture
 * identity stays the same, which is what keeps a static screen free.
 *
 * region 是这张贴图盖住的那一块（设备像素，相对输出原点），NULL = 整屏那张
 * （fx->blur_tex_w == 0 就是「整屏」的记号，装饰着色器按它算采样 UV，见
 * decor_draw 里 u_origin / u_output_size 那两行）。
 *
 * 区域必须**跟着贴图一起换**：窗口从「和别的窗口重叠」（贴图是它自己那块
 * 768x768 的私有区域）变成「不重叠」（贴图换成共享的整屏那张）时只换贴图不换
 * 区域，着色器就会拿整屏那张贴图按 768x768 的区域去采样 —— 屏幕上是一块被
 * 缩放 + 错位的壁纸模糊。用户报的「窗口不重叠时模糊背景是错的」就是这个：
 * 拖动 / 滚动 / 开窗动画期间重叠过一下，之后就一直是错的，直到它再次重叠。
 * （layer surface 那条路本来就在切回共享贴图时把 blur_tex_w/h 清 0，窗口这条路
 * 漏了 —— 现在两边都从这一个入口走。） */
/* 找这块屏对应的那一格；need=true 时找不到就占一格空的。
 *
 * 两格里按「哪块屏」认：跨屏期间同一块屏每一帧来的都是同一个 struct
 * wlr_output *，占住就不会被另一块屏挤掉。两格都占满又来了第三块屏（三屏拼
 * 角那种极端布局）就退回第一格 —— 那时宁可让它多糊一下，也不能让它去改别人
 * 正在用的那一格。 */
static void instance_node_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_blur_slot *sl = wl_container_of(listener, sl, node_destroy);
	wl_list_remove(&sl->node_destroy.link);
	sl->node = NULL;
}

void instance_drop(struct vidow_blur_slot *sl) {
	if (sl->node != NULL) {
		wlr_scene_node_destroy(&sl->node->node);
	}
	for (int i = 0; i < 2; i++) {
		if (sl->dec[i]) wlr_buffer_drop(sl->dec[i]);
		if (sl->anim[i]) wlr_buffer_drop(sl->anim[i]);
	}
	if (sl->owned && sl->tex) wlr_texture_destroy(sl->tex);
	blur_set_drop(&sl->blur_set);
	if (sl->own_bd_tex) stat_tex_del(sl->own_bd_tex);
	if (sl->own_bd) wlr_buffer_drop(sl->own_bd);
	free(sl);
}

struct vidow_blur_slot *blur_slot_for(struct vidow_fx_target *fx,
		struct wlr_output *out, bool need) {
	if (out == NULL) {
		return NULL;
	}
	for (struct vidow_blur_slot *sl = fx->blur_slot; sl; sl = sl->next) {
		if (sl->out == out) {
			return sl;
		}
	}
	if (!need) {
		return NULL;
	}
	struct vidow_blur_slot *sl = calloc(1, sizeof(*sl));
	if (sl == NULL) {
		return NULL;
	}
	sl->out = out;
	sl->next = fx->blur_slot;
	fx->blur_slot = sl;
	return sl;
}

/* 这一次烘该用哪张底衬贴图：优先取**这块屏那一格**（跨屏时两块屏各一格），
 * 没占格子再退回单份的老字段（layer / 预览 / 关闭快照那些路没有输出上下文）。 */
static struct wlr_texture *fx_blur_tex_now(struct vidow_fx_target *fx) {
	struct vidow_blur_slot *sl = blur_slot_for(fx, fx->bake_output, false);
	if (sl != NULL && sl->tex != NULL) {
		return sl->tex;
	}
	return fx->blur_tex;
}

void blur_assign(struct vidow_fx_target *fx, struct wlr_output *out,
		struct wlr_texture *tex, bool owned, uint64_t id,
		const struct wlr_box *region) {
	int rx = region != NULL ? region->x : 0;
	int ry = region != NULL ? region->y : 0;
	int rw = region != NULL ? region->width : 0;
	int rh = region != NULL ? region->height : 0;
	/* 写进**这块屏自己的那一格**（见 server.h 的 struct vidow_blur_slot）：
	 * 窗口骑在两块屏中间时，两块屏的采样原点和区域都不一样，共用一份字段就是
	 * 互相覆盖 —— 先渲染的那块屏拿到后渲染那块屏的坐标，屏幕上一半正常、一半
	 * 被拉成横条。 */
	/* **只有真有「这次烘给哪块屏」上下文的目标才分格。**
	 *
	 * 格子是给窗口装饰用的：ensure_targets 按 bake_output 选缓冲，fx_blur_tex_now
	 * 也按 bake_output 取贴图 —— 两边必须是同一个判据。layer 的底衬、overview
	 * 那张糊壁纸、关闭快照这些路没有 bake_output（它们一次只用一份贴图），
	 * 写成格子的话 fx_blur_tex_now 永远找不到它（查的是 bake_output 那一格），
	 * 着色器拿不到贴图 → u_blur_strength 归零 → 整块画成全透明
	 * （实测 overview_blur 开了和没开一个像素都不差）。 */
	struct vidow_blur_slot *slot = fx->bake_output != NULL ?
		blur_slot_for(fx, out, true) : NULL;
	if (slot != NULL) {
		if (slot->id == id && slot->rx == rx && slot->ry == ry &&
				slot->rw == rw && slot->rh == rh &&
				slot->tex == tex) {
			return; /* 这一格已经是这一版了 */
		}
		if (slot->owned && slot->tex != NULL && slot->tex != tex) {
			wlr_texture_destroy(slot->tex);
		}
		slot->tex = tex;
		slot->owned = owned;
		slot->id = id;
		slot->rx = rx;
		slot->ry = ry;
		slot->rw = rw;
		slot->rh = rh;
		fx->blur_ver++;
		blur_ver_hits[2665]++;
		return;
	}
	/* 兜底（没有输出上下文的路：layer、快照、预览）：走老的单份字段。 */
	/* 区域也算进身份：同一张贴图换了区域，装饰里烘的还是上一版的 UV，
	 * 得让它重烘（blur_ver++）。 */
	if (fx->blur_tex_id == id && fx->blur_tex_x == rx &&
			fx->blur_tex_y == ry && fx->blur_tex_w == rw &&
			fx->blur_tex_h == rh) {
		return;
	}
	if (fx->blur_owned && fx->blur_tex != NULL) {
		wlr_texture_destroy(fx->blur_tex);
	}
	fx->blur_tex = tex;
	fx->blur_owned = owned;
	fx->blur_tex_id = id;
	fx->blur_tex_x = rx;
	fx->blur_tex_y = ry;
	fx->blur_tex_w = rw;
	fx->blur_tex_h = rh;
	fx->blur_ver++;
	blur_ver_hits[2686]++;
	if (getenv("VIDOW_DMG_WHY") != NULL) {
		dmg_why_probe("blur_assign-legacy", region,
			(uintptr_t)__builtin_return_address(0));
	}
}

/* 剔除阈值（blur_alpha_min）只进装饰着色器，不进模糊金字塔：它变了得让帧循环知道
 * 「已经烘进那张缓冲里的那一版」过期了，否则运行时改配置（重载 / set-config）看着
 * 就是「没反应」（这条坑的底衬版本在 README 的 ⓪ 里）。 */
void blur_mask_min_changed(struct vidow_fx_target *fx, double alpha_min) {
	if (fabs((double)fx->mask_min_used - alpha_min) > 1e-6) {
		fx->mask_min_used = (float)alpha_min;
		fx->blur_ver++;
		blur_ver_hits[2695]++;
	}
}

/* 私有底衬那块区域缓冲 + 包着它的贴图一起收（跟 layer 的 layer_bd_drop 一个
 * 套路）。金字塔那几块缓冲归 blur_set，这里只放区域底衬。 */
void own_bd_drop(struct vidow_fx_target *fx) {
	if (fx->own_bd_tex != NULL) {
		stat_tex_del(fx->own_bd_tex);
		fx->own_bd_tex = NULL;
	}
	if (fx->own_bd != NULL) {
		wlr_buffer_drop(fx->own_bd);
		fx->own_bd = NULL;
	}
	fx->own_bd_w = 0;
	fx->own_bd_h = 0;
}

/* Release an owned texture before its pyramid buffer is reused. */
void blur_own_drop(struct vidow_fx_target *fx) {
	blur_assign(fx, NULL, NULL, false, 0, NULL);
	fx->blur_own_tex = NULL; /* 贴图归 blur_set，上面那句只放指针 */
	fx->blur_own_id = 0;
}

/* Drop every blur resource of a target. blur_assign() only frees what the
 * target owns, so the server's shared wallpaper texture survives. */
void blur_target_release(struct vidow_fx_target *fx) {
	while (fx->blur_slot != NULL) {
		struct vidow_blur_slot *sl = fx->blur_slot;
		fx->blur_slot = sl->next;
		instance_drop(sl);
	}
	fx->cur_slot = NULL;
	fx->bake_output = NULL;
	blur_own_drop(fx);
	blur_set_drop(&fx->blur_set);
	own_bd_drop(fx);
}

/* layer surface 背后的模糊（实现在下面「draw」之后）：先声明，抓底衬的时候
 * 要用它把节点藏起来。 */
/* layer 那块区域底衬（缓冲 + 包着它的贴图）一起收。贴图是包的缓冲、不是包的像素，
 * 复用缓冲的时候贴图也跟着复用，只有尺寸变了才重建 —— 链条要拿它当第一趟的源，
 * 以前每趟导入一次、用完销毁，真后端上每帧就是十几次 EGLImage + BO 映射。 */
void layer_bd_drop(struct vidow_layer_surface *ls) {
	if (ls->bd_tex != NULL) {
		stat_tex_del(ls->bd_tex);
		ls->bd_tex = NULL;
	}
	if (ls->bd != NULL) {
		wlr_buffer_drop(ls->bd);
		ls->bd = NULL;
	}
	ls->bd_w = 0;
	ls->bd_h = 0;
	ls->bd_gen_scene = 0;
	ls->bd_gen_layer = 0;
}



static bool decor_draw(struct vidow_server *server,
		struct vidow_fx_target *fx, struct wlr_buffer *dst,
		struct wlr_texture *src, struct wlr_texture *mask,
		const float border[4], int ox, int oy,
		double alpha, double scale, const struct vidow_look *look);
static bool anim_draw(struct vidow_server *server,
		struct vidow_fx_target *fx, struct wlr_buffer *dst,
		struct wlr_texture *src, int ox, int oy, double alpha, double scale);

/* 没画成的原因出口（定义在下面，layer_blur_wanted 也要用）。 */
/* 抓底衬时要不要把这个 layer surface 自己藏起来（见下面的定义）。 */
bool vidow_fx_frame(struct vidow_server *server, struct vidow_fx_target *fx,
		struct wlr_scene_buffer *src, const struct vidow_fx_geo *geo,
		int x, int y, int ox, int oy,
		double alpha, bool focused, bool fullscreen, double scale, double ov,
		const struct vidow_rule_eff *eff) {
	if (!fx->active || fx->node == NULL || src == NULL || src->buffer == NULL) {
		return false;
	}
	/* 这个窗口 / layer 现在该长什么样：全局配置压上窗口规则（见 rule.c 的
	 * vidow_view_look / vidow_rule_look）。eff = NULL 就是纯全局。 */
	struct vidow_blur_slot *instance = NULL;
	struct wlr_scene_buffer *node = fx->node;
	if (fx->view != NULL && fx->bake_output != NULL) {
		instance = blur_slot_for(fx, fx->bake_output, true);
		if (instance == NULL) {
			return false;
		}
		if (instance->node == NULL) {
			instance->node = wlr_scene_buffer_create(fx->node->node.parent, NULL);
			if (instance->node == NULL) {
				return false;
			}
			instance->node->point_accepts_input = fx_node_rejects_input;
			instance->node_destroy.notify = instance_node_destroy;
			wl_signal_add(&instance->node->node.events.destroy,
				&instance->node_destroy);
		}
		node = instance->node;
		wlr_scene_node_set_enabled(&fx->node->node, false);
		wlr_scene_node_set_enabled(&node->node, true);
		double output_scale = fx->bake_output->scale;
		if (output_scale > 0.0) {
			scale = output_scale * ov;
		}
	}
	struct vidow_look look;
	vidow_view_look(&server->config, eff, fullscreen, &look);
	/* 预览（缩略图：overview 的格子 / MRU 卡片）里**不叠那层模糊**。
	 *
	 * ov 就是「这块缩略图相对原尺寸的缩放」：overview 那一趟传的是格子缩放
	 * （1 → 0.46 的动画值，见 view.c 的 vidow_view_anim_bake），MRU 卡片那一趟
	 * 传的是卡片的 zoom（见 mru.c 的 mru_card_thumb），桌面那条路恒等于 1。
	 * 缩略图里画的是窗口的**真实缓冲**（客户端自己那份，原样采过来），内容已经
	 * 按 0.4x 缩过一道；再在它底下垫一层按**屏幕**坐标抓的模糊底衬，看着就是
	 * 「预览里的窗口内容是糊的」（用户报的「overview 和 mru 的预览不应该导致
	 * 窗口内容变模糊」）。模糊是给屏幕上的窗口用的，缩略图不该有。
	 *
	 * 只关**这一次渲染**的 look.blur（decor_draw 按它决定 u_blur_strength），
	 * **别动 fx->blur** —— 那个标志是 vidow_fx_begin() 按配置设的、
	 * vidow_blur_update() 也要读它，改了退出预览就再也恢复不了（README 1723）。
	 *
	 * 阈值取 0.98 而不是「< 1」：overview 进出场动画的头尾几帧 ov 是 0.999 /
	 * 0.998 这种值，那几帧窗口还是原尺寸、模糊是对的，不该跳一下。 */
	/* **但 overview 里的窗口要保留模糊。**
	 *
	 * 上面那条阈值原本一刀切掉所有 ov < 0.98 的渲染，overview 的窗口正好
	 * 落在这个范围（ov = 0.46）—— 于是 overview 里窗口背后那圈模糊直接没有
	 * 了。用户 2026-10-26：「窗口模糊效果没关掉 但对不上了 几乎消失」。
	 *
	 * 两种情况得分开：
	 *   - MRU / overview 的**缩略图**（mru.c 的 mru_card_thumb）：画的是窗口
	 *     缓冲缩过一道的副本，垫一层按屏幕坐标抓的模糊就是「预览内容发糊」，
	 *     该关；
	 *   - overview 里**窗口自己**：它就是屏幕上那扇窗口的缩小版，背后本来就
	 *     该糊。采样坐标已经按 ov 修好了（见下面 blur_k 那段），所以能给。
	 *
	 * 判据用 fx->view：缩略图那条路的 target 没有挂 view（mru.c 自己建
	 * vidow_fx_target，只设 anim_scale / content_box），窗口那条路一定有。 */
	bool is_thumb = fx->view == NULL;
	if (ov < 0.98 && (is_thumb || !vidow_overview_active(server))) {
		look.blur = false;
	}
	/* 果冻期间内容图里**不烘模糊**：模糊由 jelly.c 的网格单独一层按屏幕坐标
	 * 采样，跟着形状弯但不被拉长。贴图还没建好时保留烘进去的那一版，别让窗口
	 * 在起手第一帧突然没模糊。 */
	if (fx->jelly && fx->blur && fx->blur_tex != NULL &&
			!fx->blur_region_on && look.blur) {
		look.blur = false;
	}
	/* 果冻期间投影单独画（jelly.c 的 shadow 节点），内容图里不烘投影：
	 * 投影跟着网格拉伸就会变成「描边外面一圈模糊」。 */
	/* 果冻期间投影仍然烘在内容图里：它和窗口共用同一张网格，形状完全
	 * 跟着窗口弯；模糊由 jelly 网格单独一层按窗口区域裁，不会把投影圈进去。 */
	if (geo == NULL || geo->w <= 0 || geo->h <= 0) {
		wlr_log(WLR_DEBUG, "fx-frame ret: geo0");
		return false;
	}

	/* 内容尺寸 = **可见窗口**，不是整块表面：客户端自己画装饰时（GTK 那套 CSD
	 * 阴影）缓冲比窗口大一圈，边框/圆角/阴影围着缓冲画就离窗口差出那圈留白 ——
	 * 屏幕上正是「边框 / 一圈背景 / 窗口」。niri 的 LayoutElement::size() 用的
	 * 也是窗口几何，原因一样（见 struct vidow_fx_geo）。 */
	int cw = geo->w, ch = geo->h;
	/* 源缓冲里「可见窗口」在哪（归一化）。窗口几何 == 表面时是 0 / 1，采出来的
	 * 内容和以前逐像素一样。
	 *
	 * 优先用 geo->tex_*（vidow_view_fx_geo 按「客户端缓冲像素 / wp_viewport 的
	 * 有效区域」算好的）：逻辑尺寸和缓冲像素在分数缩放 + viewporter 下不是一回事，
	 * 按逻辑尺寸算出来的是整块缓冲 —— 客户端没画过的那圈会一起铺进窗口（见
	 * struct vidow_fx_geo 的说明）。全 0 的时候（层表面、关闭快照这些自己拼几何的
	 * 地方）退回老公式。 */
	if (geo->tex_du > 0.0f && geo->tex_dv > 0.0f) {
		fx->tex_off_x = geo->tex_u0;
		fx->tex_off_y = geo->tex_v0;
		fx->tex_scale_x = geo->tex_du;
		fx->tex_scale_y = geo->tex_dv;
	} else {
		fx->tex_off_x = geo->src_w > 0 ? (float)geo->off_x / (float)geo->src_w : 0.0f;
		fx->tex_off_y = geo->src_h > 0 ? (float)geo->off_y / (float)geo->src_h : 0.0f;
		fx->tex_scale_x = geo->src_w > 0 ? (float)geo->w / (float)geo->src_w : 1.0f;
		fx->tex_scale_y = geo->src_h > 0 ? (float)geo->h / (float)geo->src_h : 1.0f;
	}
	int pad = vidow_fx_pad(server);
	double px_w = ((double)cw + 2.0 * pad) * (scale > 0.0 ? scale : 1.0);
	double px_h = ((double)ch + 2.0 * pad) * (scale > 0.0 ? scale : 1.0);
	if (px_w > MAX_TARGET_PX || px_h > MAX_TARGET_PX) {
		wlr_log(WLR_DEBUG, "fx-frame ret: toobig");
		return false;
	}
	/* 输出缩放 = 设备像素 / 逻辑像素。scale 是烘图用的倍率（= 输出缩放 × ov），
	 * ov <= 0 时按 1 算（调用方在这之前已经把 ov 归一到 > 0，这里只是兜底）。 */
	double out_scale = ov > 0.0 ? scale / ov : scale;
	if (!(out_scale > 0.0)) {
		out_scale = 1.0;
	}
	if (vidow_xtrace_on()) {
		wlr_log(WLR_INFO, "fx-trace: scale=%.2f ov=%.2f out_scale=%.2f "
			"fx->w=%d cw=%d -> dst=%d view=%s", scale, ov, out_scale,
			fx->w, cw, (int)lround((double)fx->w / out_scale),
			fx->view != NULL && vidow_view_title(fx->view) != NULL ?
				vidow_view_title(fx->view) : "(非窗口)");
	}
	if (!ensure_targets(server, fx, cw, ch, scale, out_scale)) {
		wlr_log(WLR_DEBUG, "fx-frame ret: ensure");
		return false;
	}
	/* 开窗动画的缩放只作用在场景节点上（离屏缓冲照旧原尺寸烘）。模糊已经
	 * 烘在缓冲里，节点一缩，背景也跟着缩 —— 看起来是「桌面背景在弹」。
	 * 把采样坐标按同一倍率展开：p 是缓冲像素，节点把缓冲映射到屏幕时乘了
	 * anim_scale，所以模糊 UV 里的 p 也乘 anim_scale，再补上中心缩放的
	 * 平移量。静止时 scale=1 / bias=0，一个像素都不变。 */
	/* **模糊 UV 要跟着「缓冲怎么映射到屏幕」走。**
	 *
	 * 着色器算的是 o = (borg - content_off + p * blur_scale + blur_bias) / bsize
	 * —— p 是**缓冲像素**，borg 是窗口在屏幕上的原点。缓冲里这个像素最终落在
	 * 屏幕的哪个点，取决于节点把它缩放到多大：anim_scale（开合动画）和 ov
	 * （overview / 小窗 / 预览的缩略倍率）都会缩。
	 *
	 * 以前只乘了 anim_scale。于是 overview 里窗口缩到 0.46、位置也换了，采样
	 * 坐标却还按原尺寸算 —— 采到的模糊跟窗口现在盖住的那块屏幕完全不是一回
	 * 事（窗口缩小后大多采到贴图边缘之外，clamp 之后几乎全透明）。用户
	 * 2026-10-26 的原话：「窗口模糊效果没关掉 但对不上了 几乎消失」。
	 *
	 * 把 ov 一起乘进去：节点按 ov 缩小，采样半径就按 1/ov 展开 —— 窗口左边
	 * 那个像素去采屏幕上它现在真正盖住的位置。bias 用同一条式子：中心缩放
	 * 的平移量。
	 *
	 * 注意 ov 是**调用方传进来的那个**（view.c 的 fx_ov = ov * mini_scale），
	 * 和 out_scale 用的 ov 是同一个值 —— out_scale = scale / ov 里两者同乘，
	 * 输出倍率一点没变，所以这里只影响采样坐标，不影响烘图分辨率。 */
	double blur_as = fx->anim_scale > 0.0 ? fx->anim_scale : 1.0;
	double blur_ov = ov > 0.0 ? ov : 1.0;
	double blur_k = blur_as * blur_ov;
	fx->blur_scale_x = (float)blur_k;
	fx->blur_scale_y = (float)blur_k;
	fx->blur_bias_x = 0.0f;
	fx->blur_bias_y = 0.0f;
	if (fabs(blur_k - 1.0) > 1e-4) {
		fx->blur_bias_x = (float)((1.0 - blur_k) *
			((double)fx->px_pad + (double)fx->w * 0.5));
		fx->blur_bias_y = (float)((1.0 - blur_k) *
			((double)fx->px_pad + (double)fx->h * 0.5));
	}

	bool tex_owned = true;
	struct wlr_texture *tex = client_scene_texture(server, src, &tex_owned);
	if (tex == NULL) {
		wlr_log(WLR_DEBUG, "fx-frame ret: texnull");
		return false;
	}

	/* 动画那一趟要的缓冲先占好：它决定了装饰那趟要不要自己乘 alpha（见下）。
	 * 占不到就只画装饰，这一帧照旧自己淡出，不会白闪一下。
	 *
	 * 两块都得要：上屏的那块被场景锁着，只有一块的话动画着色器隔一帧才跑得起来
	 * —— 屏幕上就是「缩放过的窗口」和「没缩放只有淡入的窗口」交替闪，看着正
	 * 好像一块大矩形一闪一闪。 */
	struct wlr_buffer *anim = NULL;
	if (fx->shader != NULL) {
		for (int i = 0; i < 2; i++) {
			if ((instance != NULL ? instance->anim : fx->anim_buf)[i] == NULL) {
				(instance != NULL ? instance->anim : fx->anim_buf)[i] = vidow_alloc_target(server, fx->w, fx->h);
			}
		}
		anim = pick_free(instance != NULL ? instance->anim : fx->anim_buf);
	}

	vidow_stats.bakes++;
	/* **这块屏自己那一对**装饰目标（见 server.h 的 struct vidow_blur_slot
	 * 的 dec*）：跨屏时两块屏各有一对，各按自己的倍率烘。没有 bake_output
	 * 的路（layer / 预览）用老的那一对。 */
	struct vidow_blur_slot *dsl = fx->cur_slot;
	struct wlr_buffer **dbufs = dsl != NULL ? dsl->dec : fx->buf;
	struct wlr_buffer *displayed = NULL;
	struct wlr_buffer *decorated = NULL;
	const float *border = NULL; /* 重画装饰用（动画那趟失败时） */
	if (look.decor && server->decor.program != 0) {
		struct wlr_buffer *target = pick_free(dbufs);
		if (target != NULL) {
			border = focused ? look.border_color_focus :
				look.border_color;
			/* 有动画那趟时装饰按满 alpha 画：整块的淡入淡出由动画着色器统一乘
			 * 一次，两边各乘一次就成了 alpha²（淡得比设定的快）。 */
			double decor_alpha = anim != NULL ? 1.0 : alpha;
			if (!decor_draw(server, fx, target, tex, NULL, border, ox, oy,
					decor_alpha, scale, &look)) {
				if (tex_owned) {
					wlr_texture_destroy(tex);
				}
				return false;
			}
			displayed = target;
			decorated = target;
		} else {
			/* 两块装饰目标都还在飞（上一两帧还没上完屏）：这一帧干脆不画，
			 * 让上帧的画面多停一帧，别拿没装饰的内容顶上去。 */
			if (tex_owned) {
				wlr_texture_destroy(tex);
			}
			return false;
		}
	}

	/* 动画着色器跑在**装饰结果**上，不是原始内容：渲染目标是「窗口 + 阴影留白」
	 * 那么大一块画布，而内容贴图只占中间那块 —— 直接把它铺满整张画布，放大缩小
	 * 的就是一张被拉伸、没有圆角没有边框没有模糊的硬边大矩形，比窗口大一圈，动
	 * 画一结束装饰才接上，看着就是「先冒出一块大矩形再缩回窗口」。喂装饰结果
	 * 才是 1:1：整块带边框/圆角/模糊的窗口跟着缩放淡入淡出，收尾正好落在窗口
	 * 自己该在的位置上。 */
	if (anim != NULL) {
		struct wlr_texture *atex = NULL;
		if (decorated != NULL) {
			atex = wlr_texture_from_buffer(server->renderer, decorated);
		}
		if (atex == NULL) {
			/* 装饰结果当不成贴图（导入失败）：退回原始内容。装饰那趟已经是按满
			 * alpha 画的，这一帧要是不跑动画，窗口就会「啪」地全亮一下。 */
			atex = tex;
		}
		if (anim_draw(server, fx, anim, atex, ox, oy, alpha, scale)) {
			displayed = anim;
		} else if (decorated != NULL) {
			/* 动画那趟没画成（FBO 拿到就没了）：装饰还在，把它按真正的 alpha
			 * 重画一遍补上淡入淡出，别让窗口整块亮一帧。 */
			if (!decor_draw(server, fx, decorated, tex, NULL, border, ox, oy,
					alpha, scale, &look)) {
				if (tex_owned) {
					wlr_texture_destroy(tex);
				}
				return false;
			}
		}
		if (atex != tex) {
			wlr_texture_destroy(atex);
		}
	}
	if (tex_owned) {
		wlr_texture_destroy(tex);
	}
	tex_owned = false; /* 借来的那张已经还了，后面别再碰 */

	if (displayed == NULL) {
		wlr_log(WLR_DEBUG, "fx-frame ret: displayed");
		return false;
	}

	/* ov < 1 = 在 overview 里：整块装饰（连内边距）跟着缩，画到屏幕上的尺寸
	 * 正好等于渲染它用的设备像素（不糊也不浪费）。 */
	if (!(ov > 0.0)) {
		ov = 1.0;
	}
	/* 动画缩放：整块（连内边距）按 anim_scale 缩，围绕窗口中心 —— 所以位置
	 * 要挪回一半的差值，不然看着是从左上角往右下缩。静止时 anim_scale 恒为
	 * 1，这两项正好是 0，一个像素都不动。 */
	double as = fx->anim_scale > 0.0 ? fx->anim_scale : 1.0;
	/* 节点尺寸 = 烘图那张缓冲的**设备像素数** ÷ 输出缩放，正好 1:1 落到设备
	 * 像素上（ensure_targets 已经把缓冲取成输出缩放的整数倍，所以除得尽）。
	 * 别再写回 lround(fx->dst_w * ov)：那中间隔着两次 lround，会漂一个逻辑
	 * 像素 —— 差这一个像素，wlroots 就得把烘好的图按 1.00x 之外的倍率再双
	 * 线性采一次，预览里那层糊就是这么来的（见 ensure_targets 上面的注释）。 */
	int dst_w0 = (int)lround((double)fx->w / out_scale);
	int dst_h0 = (int)lround((double)fx->h / out_scale);
	int pad_ov = (int)lround((double)pad * ov * as);
	int dst_w = (int)lround((double)dst_w0 * as);
	int dst_h = (int)lround((double)dst_h0 * as);
	if (dst_w < 1) {
		dst_w = 1;
	}
	if (dst_h < 1) {
		dst_h = 1;
	}
	int node_x = x - pad_ov + (dst_w0 - dst_w) / 2;
	int node_y = y - pad_ov + (dst_h0 - dst_h) / 2;
	/* 越界裁剪：这块窗口只有一部分在自己那块屏里（另一半跨过了屏边界，场景图是
	 * 全局坐标，不裁就会被隔壁那块屏画出来）。裁的是**显示**：离屏缓冲照旧按整块
	 * 窗口烘，只是这张图上只采「框里的那一块」、铺到框里的那点地方。 */
	bool cropped = false;
	struct wlr_fbox crop_src = {0};
	/* overview 开着：**在这儿**裁到 overview 那块屏的工作区。
	 *
	 * 为什么不靠 view.c 的 fx->clip_on/clip：实测那条管线在 overview 里根本
	 * 不被调用（日志锚点 `overview clip:` 0 行），所以条带比屏宽的那些列一直
	 * 照画到隔壁屏上（用户：「overview 窗口太多会跨屏」）。裁剪真正生效的地方
	 * 就是下面这段（crop_src / node_x），在这儿算最可靠。
	 *
	 * 只裁画：不碰 wlr_scene_subsurface_tree_set_clip（它连**输入**一起裁，裁了
	 * 预览就命不中 —— test-overview 的「点某个窗口 = 选中它」当场红）。 */
	bool ov_clip = false;
	struct wlr_box ov_clip_box = {0};
	if (fx->view != NULL && vidow_overview_active(fx->view->server) &&
			fx->view->mini == VIDOW_MINI_NONE && fx->view->mapped) {
		/* 裁到**显示器**，不是行矩形。
		 *
		 * 用户要的语义（2026-10-25）：「工作区内窗口可以溢出工作区，但不会
		 * 溢出显示器」。overview 把每块工作区画成一块缩小的屏，行矩形只是
		 * 那块「缩小屏」的边框 —— 拿它当裁剪框等于把窗口裁在工作区里，
		 * 窗口稍微探出自己那一行就整块被切掉，而屏幕上明明还有地方。
		 *
		 * 以前这里裁的是行矩形（更早是整块屏），两种都试过。行矩形那版的
		 * 问题就是上面这条：行与行之间本来有间隙，间隙属于显示器、不属于
		 * 任何工作区，窗口探进间隙应该照常显示（就像正常桌面里窗口探出
		 * 工作区边界一样），不该被切。
		 *
		 * 所以裁剪边界回到显示器：行内随便溢出，出屏才切。隔壁显示器上
		 * 不会画到 —— 场景图是全局坐标的，这块屏的节点裁在自己屏框里，
		 * 就落不到别人那块上。
		 *
		 * 行矩形仍然有用（命中测试、动画目标），只是不再当裁剪框。 */
		/* ⚠️ 用**显示器**的框，不是 ov->out_×。
		 *
		 * ov->out_× 存的是 vidow_work_area()（工作区 = 屏框扣掉 panel /
		 * gap 之后的可用区）。拿它当裁剪框，窗口探进 panel 那一条还是会被
		 * 切 —— 而用户要的是「溢出工作区可以，溢出显示器不行」，panel
		 * 那块属于显示器、不属于工作区，应该照常显示。
		 *
		 * 屏框是布局坐标里的真值，直接问 output_layout。 */
		/* 用**窗口自己所在那块屏**的框（每块屏都进 overview，各自裁自己
		 * 那块屏的范围）。以前固定用 overview.output，别的屏上的窗口被裁到
		 * 主屏矩形里。 */
		/* **不再在这里裁。**
		 *
		 * 这里原本把裁剪框设成「窗口归属那块显示器的布局框」，可 overview
		 * 里节点画的是**行坐标**（每块工作区缩成一条，见 server.h 的
		 * vidow_ov_row：screen = 行的 content_x/y + 局部 * scale），
		 * 跟布局里的显示器框压根不是一个坐标系。两者一相减，窗口只要探出
		 * 显示器一点点，交集就空了 —— 下面那条「整个身子都在框外」的分支
		 * 把节点甩到 -1000000，窗口在 overview 里整个消失（用户 2026-10-27
		 * 报的「overview 的窗口超出一点显示器就消失」）。
		 *
		 * 不裁也不会画到隔壁屏上：场景图是全局坐标，每块屏自己的节点被
		 * wlroots 裁在那块屏的框里，落不到别人那块上。这本来就是上面注释
		 * 里写的语义 —— 「行内随便溢出，出屏才切」，而出屏那一刀由输出
		 * 自己的裁剪天然完成，不需要我们在这儿再算一遍。
		 *
		 * 行矩形（row->rect_*）仍然有用：命中测试、动画目标都靠它，
		 * 只是不再当裁剪框。 */
		(void)ov_clip_box;
	}
	/* **果冻（拖拽形变）期间不裁这一份。**
	 *
	 * 这个节点的显示范围被裁到「这块屏里那一半」时，裁剪会把**节点位置**
	 * 改成可见部分的起点（下面 `node_x = inter.x`）：窗口往左拖出屏幕，
	 * 节点就被钉在这块屏的左边框上。可果冻（jelly.c）正是拿这个节点的
	 * 坐标当「窗口左上角」去摆它那张整窗的形变图 —— 于是整块窗口被画到
	 * 屏的左边缘、不再跟着光标（用户 2026-10-30 报的「第一次完全跨过去
	 * 之前会卡在 DP-2」，探针实测：tree=1287 而 src=1920 = DP-2 左边缘，
	 * rel=633 全是这一刀裁出来的偏移）。
	 *
	 * 果冻期间这一份本来就不显示：`vidow_fx_frame` 末尾把它的透明度压成 0
	 * （内容由果冻节点画）。所以这里跳过裁剪只影响「节点坐标」这一个用途，
	 * 显示的按屏裁仍然由果冻那一层 + 输出自己的裁剪完成 —— 两块屏各画
	 * 自己那一半的效果不变。 */
	if ((instance != NULL || fx->clip_on || ov_clip) && !fx->jelly) {
		/* fx->clip 是**布局坐标**里的那块屏；这个节点的位置是相对父树的
		 * （父树就是窗口的场景树，摆在窗口的位置上）—— 减掉父树的原点才
		 * 在同一个坐标系里。 */
		struct wlr_box clip = ov_clip ? ov_clip_box : fx->clip;
		if (instance != NULL && !ov_clip) {
			/* 这块屏的布局框：vidow_blur_update() 本帧开头刚写进
			 * instance->layout_box（它先于渲染趟跑），直接拿来用。
			 * 别再查 wlr_output_layout_get_box —— 那是链表遍历，
			 * 而这个函数每帧 × 每个窗口都要走一遍。 */
			struct wlr_box output_clip = instance->layout_box;
			if (output_clip.width <= 0 || output_clip.height <= 0) {
				/* 这个槽位还没被 vidow_blur_update 填过（刚建 / 模糊关着）：
				 * 现查一次，别拿空框去裁 —— 空框会让下面判成「整个身子都在
				 * 屏外」，节点被甩到 -1000000，窗口整块消失。 */
				wlr_output_layout_get_box(server->output_layout,
					instance->out, &output_clip);
			}
			/* **每块屏只画自己那一半**（照 Hyprland 的 renderMonitor）。
			 *
			 * 装饰缓冲是按屏各一份的（slot->dec*），每份都用**那块屏自己**
			 * 的模糊底衬烘好，所以这里的裁剪框必须是**这一格自己的屏框**，
			 * 不是布局总框。
			 *
			 * 用布局总框的话两份装饰会整窗重叠在同一个位置、只是倍率不同：
			 * 最后画的那份把另一份盖住 —— 2x 屏上看到 1x 烘的图（放大糊），
			 * 1x 屏上看到 2x 烘的图（缩下去倒是清楚）。两块屏轮流重烘时，
			 * 盖在上面的那一份每帧换人，就是「高分辨率那块屏背景一直闪」。
			 *
			 * 拖拽中也一样：另一半由**对面那块屏**自己那份节点画（它裁在
			 * 对面那块屏里），拼起来才是完整窗口。以前这条豁免是给「只有
			 * 一份装饰缓冲」的旧模型留的，现在每屏一份，豁免反而把两份叠
			 * 在一起。 */
			if (fx->clip_on) {
				struct wlr_box visible = {0};
				wlr_box_intersection(&visible, &clip, &output_clip);
				clip = visible;
			} else {
				clip = output_clip;
			}
		}
		if (node->node.parent != NULL) {
			clip.x -= node->node.parent->node.x;
			clip.y -= node->node.parent->node.y;
		}
		struct wlr_box node_box = {.x = node_x, .y = node_y,
			.width = dst_w, .height = dst_h};
		struct wlr_box inter;
		if (wlr_box_intersection(&inter, &node_box, &clip) &&
				(inter.width < dst_w || inter.height < dst_h)) {
			double kx = (double)fx->w / (double)dst_w;
			double ky = (double)fx->h / (double)dst_h;
			crop_src = (struct wlr_fbox){
				.x = (double)(inter.x - node_x) * kx,
				.y = (double)(inter.y - node_y) * ky,
				.width = (double)inter.width * kx,
				.height = (double)inter.height * ky,
			};
			node_x = inter.x;
			node_y = inter.y;
			dst_w = inter.width;
			dst_h = inter.height;
			cropped = true;

			if (ov_clip) {
				wlr_log(WLR_DEBUG,
					"overview clip: view %p 裁到 %d,%d %dx%d（源 %d,%d %dx%d → 显示 %dx%d）",
					(void *)fx->view, clip.x, clip.y,
					clip.width, clip.height,
					(int)crop_src.x, (int)crop_src.y,
					(int)crop_src.width, (int)crop_src.height,
					inter.width, inter.height);
			}
		} else if (!wlr_box_intersection(&inter, &node_box, &clip)) {
			/* 这一帧整个身子都在自己那块屏外面：一个像素都不该画（画了就是
			 * 画到隔壁屏上 —— 场景图是全局坐标，那边正画着这块框）。把落点
			 * 甩到所有输出框外面去：整块缓冲照旧传进去（空的源框 wlroots 会
			 * 当成「整块」，那是另一个意思），但没有任何一块屏的框里有它。
			 * 别指望「没停到屏外就一定有交集」：判据比的是**这一帧画在哪儿**，
			 * 这里用的是节点自己的落点，两者在动画帧里可能错开一拍 —— 错过
			 * 一拍就是一块窗口闪在隔壁屏上（用户报的「回来的瞬间」）。 */
			node_x = -1000000;
			node_y = -1000000;
			dst_w = 1;
			dst_h = 1;
			/* 跟手探针（touch /tmp/vidow-x11-trace 开）：拖拽期间把这一步的决定
			 * 打出来 —— 用的是哪块屏自己的那一格（instance）、裁剪到哪个框、
			 * 最后把装饰节点摆到哪儿、有没有被裁成 1x1 甩到屏外。 */
			if (vidow_xtrace_on() && fx->view != NULL &&
					(fx->view->dragging ||
					fx->view->server->grab_view == fx->view)) {
				vidow_gtrace("fx v=%d out=%s node=%d,%d %dx%d inst=%s "
					"clip_on=%d clip=%d,%d %dx%d outclip=%d,%d %dx%d "
					"crop=%d jelly=%d op=%.2f",
					fx->view->id,
					instance != NULL && instance->out != NULL ?
						instance->out->name : "-",
					node_x, node_y, dst_w, dst_h,
					instance != NULL ? "yes" : "no",
					fx->clip_on, fx->clip.x, fx->clip.y,
					fx->clip.width, fx->clip.height,
					clip.x, clip.y, clip.width, clip.height,
					cropped ? 1 : 0, fx->jelly ? 1 : 0,
					(fx->masked || fx->jelly) ? 0.0 : 1.0);
			}
		}
	}
	wlr_scene_node_set_position(&node->node, node_x, node_y);

	wlr_scene_buffer_set_dest_size(node, dst_w, dst_h);
	/* 被抓帧遮挡压着的窗口，这一帧也照样压着（抓帧随时可能在两帧之间渲染）。
	 * 正在被拖出果冻形变的窗口也一样：内容由 jelly.c 那个节点画（同一块缓冲，
	 * 只是弯了），这一块必须让位，否则两张图叠在一起。 */
	wlr_scene_buffer_set_opacity(node,
		(fx->masked || fx->jelly) ? 0.0f : 1.0f);

	/* Full-buffer damage: the node is re-rendered every frame of the effect. */
	/* 损伤按**缓冲像素**记（wlroots 的约定），裁过之后 dst_w/dst_h 只是源框那
	 * 一块的尺寸 —— 整块缓冲都可能变，就照整块缓冲报。 */
	pixman_region32_t damage;
	if (cropped) {
		pixman_region32_init_rect(&damage, 0, 0, fx->w, fx->h);
	} else {
		pixman_region32_init_rect(&damage, 0, 0, dst_w, dst_h);
	}
	wlr_scene_buffer_set_buffer_with_damage(node, displayed, &damage);
	pixman_region32_fini(&damage);
	/* 源框要**最后**设：换缓冲（这一路每帧在两张目标之间来回换）会把它抹掉。 */
	wlr_scene_buffer_set_source_box(node, cropped ? &crop_src : NULL);

	/* This buffer now contains this blur version: the frame loop only has to
	 * redraw when a later frame hands the window a different backdrop. */
	fx->blur_drawn = fx->blur_ver;
	if (instance != NULL) {
		instance->dec_valid = true;
		instance->dec_revision = fx->bake_revision;
		instance->dec_blur_ver = fx->blur_ver;
		instance->dec_output_scale = instance->out->scale;
		wlr_output_layout_get_box(server->output_layout, instance->out,
			&instance->dec_output_box);
	}

	/* dump_target 自己记得「dump 过几份、上限多少」（VIDOW_FX_DUMP 没设就立刻返回），
	 * 这里不再每帧问一次 getenv。 */
	dump_target(server, fx, displayed);
	return true;
}
/* 区域之类「只影响着色器输出、贴图没换」的变化要显式失效一次，
 * 否则帧循环看到 blur_drawn == blur_ver 就不重画了。 */
void vidow_view_blur_invalidate(struct vidow_view *view) {
	if (!view->fx.active) {
		return;
	}
	view->fx.blur_ver++;
	blur_ver_hits[3276]++;
	if (view->output != NULL) {
		vidow_output_schedule_frame(view->server, view->output,
			"装饰模糊失效");
	}
}

void vidow_fx_end(struct vidow_server *server, struct vidow_fx_target *fx) {
	(void)server;
	if (fx->node != NULL) {
		wlr_scene_node_set_enabled(&fx->node->node, false);
		wlr_scene_buffer_set_buffer(fx->node, NULL);
	}
	fx->shader = NULL;
	fx->phase = VIDOW_SHADER_NONE;
	fx->active = false;
	fx->anim_scale = 1.0;
	blur_target_release(fx);
	drop_targets(fx);
}

/* Drop everything except the scene node, which the parent tree owns. Use this
 * when the node may already have been destroyed with its tree. */
void vidow_fx_release(struct vidow_fx_target *fx) {
	drop_targets(fx);
	blur_target_release(fx);
	fx->shader = NULL;
	fx->phase = VIDOW_SHADER_NONE;
	fx->active = false;
	fx->anim_scale = 1.0;
	fx->node = NULL;
}

/* --------------------------------------------------------- closing windows */



static bool layer_blur_frame(struct vidow_server *server,
		struct vidow_output *output, struct vidow_layer_surface *ls);
static void layer_blur_capture_visible(struct vidow_server *server, bool on);
/* 弹出物背后的模糊（窗口的 xdg_popup / X11 菜单 / 输入法候选框）。 */
static void popup_blur_frame(struct vidow_server *server,
		struct vidow_output *output, struct vidow_popup_blur *pb);
static void popup_blur_release(struct vidow_popup_blur *pb);
static struct vidow_output *popup_blur_owner(struct vidow_popup_blur *pb);
static void popup_blur_node_destroy(struct wl_listener *listener, void *data);
static void popup_blur_owner_destroy(struct wl_listener *listener, void *data);
/* overview 里那张糊掉的壁纸（整屏，垫在压暗遮罩下面）。 */
static void vidow_overview_blur_frame(struct vidow_server *server,
		struct vidow_output *output);

int blur_levels_for(struct vidow_server *server, int w, int h) {
	int levels = server->config.blur_levels;
	if (levels < 2) {
		levels = 2;
	}
	if (levels > MAX_BLUR_LEVELS) {
		levels = MAX_BLUR_LEVELS;
	}
	while (levels > 2 && ((w >> levels) < 4 || (h >> levels) < 4)) {
		levels--;
	}
	return levels;
}

static void shared_blur_drop(struct vidow_output *output) {
	/* 这两张贴图现在归别人管：blur_tex 是金字塔 up[0] 那张常驻贴图（blur_set_drop
	 * 收），backdrop_tex 跟着底衬缓冲走（blur_drop 收）。这里只把指针放下，
	 * 顺手把共享金字塔收掉。 */
	output->blur_tex = NULL;
	output->backdrop_tex = NULL;
	blur_set_drop(&output->blur_set);
	output->blur_id = 0;
	output->blur_gen = 0;
	output->blur_levels = 0;
}

/* Capture the scene with every toplevel hidden and run the kawase chain on it.
 * The result depends only on the wallpaper and the layer surfaces, so it stays
 * valid across client repaints, window moves and viewport scrolling. */
static bool shared_blur_rebuild(struct vidow_server *server,
		struct vidow_output *output, int w, int h, int levels) {
	struct vidow_view *view;
	struct vidow_closing *closing;

	wl_list_for_each(view, &server->views, link) {
		view_set_capture_visible(view, false);
	}
	wl_list_for_each(closing, &server->closing, link) {
		if (closing->fx.node != NULL) {
			wlr_scene_node_set_enabled(&closing->fx.node->node, false);
		}
	}
	/* layer 的模糊节点也得藏：bottom 层的那个就在被抓的场景里，不藏就是
	 * 「模糊结果里混进上一帧的模糊」。 */
	layer_blur_capture_visible(server, false);
	bool ok = backdrop_render(server, output, w, h);
	layer_blur_capture_visible(server, true);
	wl_list_for_each(view, &server->views, link) {
		view_set_capture_visible(view, true);
	}
	wl_list_for_each(closing, &server->closing, link) {
		if (closing->fx.node != NULL) {
			wlr_scene_node_set_enabled(&closing->fx.node->node, true);
		}
	}
	if (!ok) {
		return false;
	}

	/* 底衬贴图（backdrop_tex）跟着底衬缓冲一起建、一起收（见 vidow_blur_update），
	 * 金字塔的贴图跟着金字塔缓冲走（见 blur_set_alloc）：这里一张都不用导入，
	 * blur_tex 就是 up[0] 那张常驻贴图。 */
	/* blur_set_alloc() 在形状变化时会先 blur_set_drop()，旧的 up[0] 贴图
	 * 随之销毁；output->blur_tex 还指着它。这里先把指针放下，分配失败时
	 * 后面就不会拿一张已释放的贴图去采样。 */
	output->blur_tex = NULL;
	output->blur_id = 0;
	output->blur_gen = 0;
	output->blur_levels = 0;
	if (!blur_set_alloc(server, &output->blur_set, w, h, levels)) {
		return false;
	}
	blur_chain(server, &output->blur_set, output->backdrop_tex, w, h);
	output->blur_tex = output->blur_set.up_tex[0];
	if (output->blur_tex == NULL) {
		return false;
	}
	static int dumped = 0;
	if (dumped < blur_dump_limit()) {
		dump_buffer(server, output->backdrop, w, h, "vidow-bd", dumped);
		if (output->blur_set.up[0] != NULL) {
			dump_buffer(server, output->blur_set.up[0], w >> 1, h >> 1,
				"vidow-blur", dumped);
		}
		dumped++;
	}
	vidow_stats.shared++;
	output->blur_id = server->blur_serial++;
	output->blur_gen = output->backdrop_revision;
	output->blur_levels = levels;
	output->blur_offset = server->config.blur_offset;
	wlr_log(WLR_DEBUG, "blur: shared rebuild %s %dx%d levels=%d offset=%.2f",
		output->wlr_output->name, w, h, levels, output->blur_offset);
	return true;
}

/* 链条采样到底能摸到多远（原始像素）：降采样每一趟的抽样半径是 blur_offset 个
 * **源** texel，源每降一级尺寸减半，换算回原图每级翻倍（offset * (2^levels - 1)）；
 * 往上那几趟同理，只是从第 2 级起（offset * (2^(levels+1) - 4)）。两者相加就是
 * offset * (3 * 2^levels - 5)。区域要在窗口外面留出这么一圈，不然窗口边缘附近的
 * 模糊值是被区域边界 clamp 出来的（看着就是边上发暗）。 */
int blur_reach_px(struct vidow_server *server, int levels) {
	double off = server->config.blur_offset;
	if (!(off > 0.0)) {
		off = 0.0;
	}
	if (levels < 2) {
		levels = 2;
	}
	double span = 3.0 * (double)(1u << levels) - 5.0;
	int reach = (int)ceil(off * span) + 8;
	return reach < 16 ? 16 : reach;
}

/* 这块窗口的私有底衬区域（设备像素，相对这块屏的原点）。levels 出参是这块区域
 * 该用几级金字塔 —— 跟整屏那套**同一个数**：级数决定模糊半径（物理像素），各算
 * 各的话小窗口会比大窗口糊得轻。 */
static bool view_region_rect(struct vidow_server *server,
		struct vidow_output *output, struct vidow_view *view,
		int *rx, int *ry, int *rw, int *rh, int *levels) {
	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	int pad = vidow_fx_pad(server);
	struct wlr_box box = vidow_view_screen_box(view);
	int lv = blur_levels_for(server,
		output->wlr_output->width, output->wlr_output->height);
	int margin = blur_reach_px(server, lv);
	/* **这块屏自己的**布局原点（设备像素），不是 server->output_x/y。
	 *
	 * server->output_* 是「最后渲染的那块屏」留下的全局量：两屏同帧刷新时后
	 * 渲染的会把前面的覆盖掉，先渲染的那块屏上的窗口就拿错了原点 —— 区域整体
	 * 横移一屏宽，底衬采到的是隔壁屏的内容 / 屏外的空像素。用户 2026-10-23
	 * 报的「跨屏拖拽时右边跨过去的部分没有采样右边屏幕背景、还按左屏采样」
	 * 里，至少有一半是这个：窗口压在边界上时，两趟重烘互相把对方的原点写进
	 * server->output_*，谁后写谁赢。
	 *
	 * 下面 blur 那一段（decor_pass 之前的 origin_x 那几行）早就改成按
	 * output->wr_output 现算了，注释里也写着「不是 server->output_x/y」——
	 * 这里是同一个坑漏掉的一处。 */
	struct wlr_box obox = {0};
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &obox);
	int my_out_x = (int)lround((double)obox.x * scale);
	int my_out_y = (int)lround((double)obox.y * scale);
	int my_out_w = output->wlr_output->width;
	int my_out_h = output->wlr_output->height;
	/* 装饰矩形 = 可见窗口 ± pad（逻辑单位），再往外放一圈模糊半径。 */
	int left = (int)lround((double)(box.x - pad) * scale) - my_out_x - margin;
	int top = (int)lround((double)(box.y - pad) * scale) - my_out_y - margin;
	int span_w = (int)lround((double)(box.width + 2 * pad) * scale) + 2 * margin;
	int span_h = (int)lround((double)(box.height + 2 * pad) * scale) + 2 * margin;
	const int grid = VIDOW_BLUR_REGION_GRID;
	/* 原点对齐到网格（见上面那个宏）：窗口在格子里挪动时区域一个字都不变。 */
	int gx1 = (int)floor((double)left / grid) * grid;
	int gy1 = (int)floor((double)top / grid) * grid;
	/* 尺寸**只跟窗口自己的大小有关，跟位置无关**（多留一格给「窗口在格子里挪
	 * 动」）：拖窗口时区域整体平移，缓冲尺寸不变 —— 金字塔那几块缓冲才能一直
	 * 复用（尺寸一变就得整批重建 + 重新导入贴图，那正是上一版修掉的卡顿）。 */
	int size_w = (int)ceil((double)(span_w + grid) / grid) * grid;
	int size_h = (int)ceil((double)(span_h + grid) / grid) * grid;
	if (size_w < 1 || size_h < 1 || size_w > MAX_TARGET_PX ||
			size_h > MAX_TARGET_PX) {
		return false;
	}
	/* 原点再夹一下，让整块区域留在屏内：屏外没有像素可抓（那就是透明的），
	 * 贴边窗口的边缘会被链条混进黑。夹的是原点、不是尺寸 —— 尺寸照样只跟窗口
	 * 大小有关，缓冲不会因为窗口移动而重建。窗口比屏幕还大的极端情况下夹不住，
	 * 那圈就还是透明的（layer 那一路一直是这个行为）。
	 *
	 * 夹的边界是**这块屏自己的**尺寸（my_out_w/h），不是 server->output_w/h：
	 * 后者是最后渲染的那块屏的，窗口跨在边界上时两趟互相覆盖，先渲染的那趟
	 * 会拿隔壁屏的尺寸来夹 —— 区域被截到屏边缘，模糊就只剩一条（用户报的
	 * 「跨屏那一半是横条」）。 */
	int max_x = my_out_w - size_w;
	int max_y = my_out_h - size_h;
	if (max_x < 0) {
		max_x = 0;
	}
	if (max_y < 0) {
		max_y = 0;
	}
	if (gx1 < 0) {
		gx1 = 0;
	} else if (gx1 > max_x) {
		gx1 = max_x;
	}
	if (gy1 < 0) {
		gy1 = 0;
	} else if (gy1 > max_y) {
		gy1 = max_y;
	}
	*rx = gx1;
	*ry = gy1;
	*rw = size_w;
	*rh = size_h;
	*levels = lv;
	return true;
}

/* 「只重跑受损那一小块」的开关：默认开。设了 VIDOW_BLUR_NO_PARTIAL 就整块重跑；
 * VIDOW_BLUR_NO_PARTIAL_FILE 指的文件一出现也整块重跑（跑着跑着切过去，用来比对
 * 「同一个场景、同一个客户端、同一个进程」下局部和整块的输出 —— 见
 * test-blurpartial.sh）。都只给 A/B 比对用。
 *
 * 两个 getenv 各只问一次（每次重抓都要问一遍：一帧里四五个元素重抓就是十来次；
 * getenv 在 profile 里比我们自己的模糊代码还显眼）。**文件存在性每次都看** ——
 * test-blurpartial.sh 就是跑着跑着把文件建出来切过去的。 */
/* 渲染热路径上的两个探针开关，只问一次环境变量。
 *
 * 它们都在「每帧 × 每个窗口」的函数里（view_region_build 的模糊重跑、
 * decor_draw 的 span），原神那种 160fps 满速提交下 getenv 的 environ
 * 线性扫描每秒上千次。环境变量进程起来之后不会变，缓存住就行 —— 和
 * 本文件上面 scanout_off / blur_no_partial 一个规矩。 */
static bool blur_trace_on(void) {
	static int done = 0, on = 0;
	if (!done) {
		on = getenv("VIDOW_BLUR_TRACE") != NULL ? 1 : 0;
		done = 1;
	}
	return on != 0;
}

static bool span_trace_on(void) {
	static int done = 0, on = 0;
	if (!done) {
		on = getenv("VIDOW_SPAN_TRACE") != NULL ? 1 : 0;
		done = 1;
	}
	return on != 0;
}

static bool blur_no_partial(void) {
	static int on = -1;
	static int file_read = 0;
	static const char *file = NULL;
	if (on < 0) {
		on = getenv("VIDOW_BLUR_NO_PARTIAL") != NULL ? 1 : 0;
	}
	if (!file_read) {
		file = getenv("VIDOW_BLUR_NO_PARTIAL_FILE");
		file_read = 1;
	}
	if (on != 0) {
		return true;
	}
	return file != NULL && access(file, F_OK) == 0;
}

/* 这次重抓**只需要动**的那一块（区域局部设备像素）：攒下来的损伤盒子跟区域相交
 * 的那部分的并集（往外放 2 像素，抵掉逻辑->设备的取整）。返回 false = 用不上，
 * 调用方整块重跑：
 *   - 一笔都没相交（不该走到这儿，防御）；
 *   - 损伤盖住了区域的一大半（>= 60%）：局部省不了多少，整块重跑还简单些。 */
static bool region_update_rect(struct vidow_server *server,
		struct vidow_fx_target *fx, struct vidow_output *output,
		int rx, int ry, int rw, int rh, struct wlr_box *out) {
	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	const struct vidow_dmg_set *set = &fx->pending_damage;
	struct wlr_box u = {0};
	for (int i = 0; i < set->n; i++) {
		const struct wlr_box *b = &set->box[i];
		/* 布局逻辑坐标 -> 这块屏的设备像素（减去这块屏的布局原点）-> 区域局部 */
		double lx = (double)(b->x - output->box.x) * scale;
		double ly = (double)(b->y - output->box.y) * scale;
		double hx = (double)(b->x + b->width - output->box.x) * scale;
		double hy = (double)(b->y + b->height - output->box.y) * scale;
		struct wlr_box one = {
			.x = (int)floor(lx) - rx - 2,
			.y = (int)floor(ly) - ry - 2,
			.width = (int)ceil(hx) - (int)floor(lx) + 4,
			.height = (int)ceil(hy) - (int)floor(ly) + 4,
		};
		blur_box_add(&u, &one);
	}
	if (u.width <= 0 || u.height <= 0) {
		return false;
	}
	bool clip_ok = true;
	wlr_scene_node_for_each_buffer(&server->layer_toplevels->node,
		backdrop_clip_check, &clip_ok);
	if (!clip_ok) {
		return false;
	}
	int x1 = u.x < 0 ? 0 : u.x;
	int y1 = u.y < 0 ? 0 : u.y;
	int x2 = u.x + u.width > rw ? rw : u.x + u.width;
	int y2 = u.y + u.height > rh ? rh : u.y + u.height;
	if (x2 <= x1 || y2 <= y1) {
		return false;
	}
	if ((int64_t)(x2 - x1) * (y2 - y1) * 10 >= (int64_t)rw * rh * 6) {
		return false; /* 盖住大半了：整块重跑 */
	}
	*out = (struct wlr_box){
		.x = x1, .y = y1, .width = x2 - x1, .height = y2 - y1,
	};
	return true;
}

/* 抓「世界减掉这个窗口」，只抓它自己那一小块区域，然后跑一遍链条。
 *
 * 基座是共享底衬（壁纸 + background / bottom 层）里对应的那一块，1:1 拷过来，
 * 别的窗口再画上去 —— 这就是「模糊里得有窗口」；而这个窗口自己在抓的时候是藏起来
 * 的（它不该出现在自己的底衬里）。以前这里是整屏重画一遍（连壁纸一起），每帧每块
 * 重叠的窗口都要来一次；现在代价只跟这块区域的面积成正比。 */
static bool view_region_build(struct vidow_server *server,
		struct vidow_output *output, struct vidow_view *view,
		int rx, int ry, int rw, int rh, int levels,
		const struct wlr_box *partial) {
	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}

	/* partial != NULL：只有这块（**区域局部设备像素**）的像素内容变了，其余保持
	 * 上一版 —— 抓也只抓这一块、链条也只重跑这一块（见 blur_chain_rect）。
	 * partial == NULL：整块重跑（第一次 / 形状变了 / 损伤盖满整块）。 */
	int px = 0, py = 0, pw = rw, ph = rh;
	if (partial != NULL) {
		px = partial->x < 0 ? 0 : partial->x;
		py = partial->y < 0 ? 0 : partial->y;
		int px2 = partial->x + partial->width;
		int py2 = partial->y + partial->height;
		if (px2 > rw) {
			px2 = rw;
		}
		if (py2 > rh) {
			py2 = rh;
		}
		if (px2 <= px || py2 <= py) {
			return false;
		}
		pw = px2 - px;
		ph = py2 - py;
	}

	int x1 = rx < 0 ? 0 : rx, y1 = ry < 0 ? 0 : ry;
	int x2 = rx + rw, y2 = ry + rh;
	if (x2 > server->output_w) {
		x2 = server->output_w;
	}
	if (y2 > server->output_h) {
		y2 = server->output_h;
	}
	if (x2 <= x1 || y2 <= y1) {
		return false; /* 整块在屏外，没东西可抓 */
	}
	if (partial != NULL) {
		/* 要抓的那一块也得夹进屏内（区域原点可能被夹过，右/下边可能是屏外）。 */
		int cx1 = rx + px, cy1 = ry + py;
		int cx2 = cx1 + pw, cy2 = cy1 + ph;
		if (cx1 < x1) {
			cx1 = x1;
		}
		if (cy1 < y1) {
			cy1 = y1;
		}
		if (cx2 > x2) {
			cx2 = x2;
		}
		if (cy2 > y2) {
			cy2 = y2;
		}
		if (cx2 <= cx1 || cy2 <= cy1) {
			return false;
		}
		px = cx1 - rx;
		py = cy1 - ry;
		pw = cx2 - cx1;
		ph = cy2 - cy1;
	}

	struct vidow_fx_target *fx = &view->fx;
	struct vidow_blur_slot *sl = blur_slot_for(fx, output->wlr_output, true);
	if (sl == NULL) return false;
	/* 这一版是被哪一块损伤触发的（只给下面那行日志用：pending_damage 这里就要
	 * 清掉了）。修「客户端只动了一小块，整片模糊却重抓」时，看的就是这个数。 */
	struct wlr_box dmg = dmg_bounds(&fx->pending_damage);
	if (sl->own_bd == NULL || sl->own_bd_w != rw || sl->own_bd_h != rh) {
		if (sl->own_bd_tex) stat_tex_del(sl->own_bd_tex);
		sl->own_bd_tex = NULL;
		if (sl->own_bd) wlr_buffer_drop(sl->own_bd);
		sl->own_bd = NULL;
		sl->own_bd = vidow_alloc_target(server, rw, rh);
		if (sl->own_bd == NULL) {
			return false;
		}
		sl->own_bd_tex = stat_tex_new(
			wlr_texture_from_buffer(server->renderer, sl->own_bd));
		if (sl->own_bd_tex == NULL) {
			wlr_buffer_drop(sl->own_bd);
			sl->own_bd = NULL;
			return false;
		}
		sl->own_bd_w = rw;
		sl->own_bd_h = rh;
	}

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, sl->own_bd, NULL);
	if (pass == NULL) {
		return false;
	}
	struct wlr_render_rect_options clear = {
		.box = { .x = px, .y = py, .width = pw, .height = ph },
		.color = { .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f },
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	};
	wlr_render_pass_add_rect(pass, &clear);

	/* 基座：共享底衬（壁纸 + background / bottom 层）里对应的那一块，1:1 捞过来
	 * （不插值、不混合 —— 这块缓冲刚清成全透明，直接覆盖写最便宜）。源框必须
	 * 夹在底衬里面：wlroots 的 render pass 会断言源框越界（伸到屏外的那圈只能
	 * 留透明，所以 view_region_rect 把区域原点夹在屏内）。 */
	int bx1 = rx + px, by1 = ry + py;
	if (bx1 < x1) {
		bx1 = x1;
	}
	if (by1 < y1) {
		by1 = y1;
	}
	int bx2 = bx1 + pw, by2 = by1 + ph;
	if (bx2 > x2) {
		bx2 = x2;
	}
	if (by2 > y2) {
		by2 = y2;
	}
	float one = 1.0f;
	struct wlr_render_texture_options base = {
		.texture = output->backdrop_tex,
		.src_box = {
			.x = (float)bx1, .y = (float)by1,
			.width = (float)(bx2 - bx1), .height = (float)(by2 - by1),
		},
		.dst_box = {
			.x = (float)(bx1 - rx), .y = (float)(by1 - ry),
			.width = (float)(bx2 - bx1), .height = (float)(by2 - by1),
		},
		.alpha = &one,
		.transform = WL_OUTPUT_TRANSFORM_NORMAL,
		.filter_mode = WLR_SCALE_FILTER_NEAREST,
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	};
	wlr_render_pass_add_texture(pass, &base);

	/* 窗口画在基座上：这个窗口自己藏起来，别的照画。只画 toplevels —— 弹出物那
	 * 一层在窗口**上面**，不该进窗口的底衬（共享底衬那趟也是这么做的）。 */
	/* 私有底衬抓的是「世界减掉这个窗口」：整棵窗口树一起关，比只关装饰 /
	 * 果冻节点更硬 —— 客户端表面、阴影、任何漏网的子节点都不会混进来。 */
	bool tree_on = view->scene_tree != NULL &&
		view->scene_tree->node.enabled;
	if (tree_on) {
		wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	}
	view_set_capture_visible(view, false);
	layer_blur_capture_visible(server, false);
	struct backdrop_ctx ctx = {
		.pass = pass,
		.server = server,
		.scale = scale,
		.ox = server->output_x + rx,
		.oy = server->output_y + ry,
		/* 只画要抓的那一块（partial 时就是损伤那一小块）：底衬别的像素一个都
		 * 不碰，其余部分还是上一版的内容。 */
		.clip_x = bx1 - rx,
		.clip_y = by1 - ry,
		.clip_w = bx2 - bx1,
		.clip_h = by2 - by1,
		.skip = view,
	};
	wlr_scene_node_for_each_buffer(&server->layer_toplevels->node, backdrop_iter,
		&ctx);
	layer_blur_capture_visible(server, true);
	view_set_capture_visible(view, true);
	if (view->scene_tree != NULL) {
		/* 恢复抓之前的状态，而不是一律 true：隐藏工作区里的窗口本来就是
		 * 关着的，抓完不能把它点亮。 */
		wlr_scene_node_set_enabled(&view->scene_tree->node, tree_on);
	}
	if (!wlr_render_pass_submit(pass)) {
		return false;
	}

	vidow_stats.own++;
	dmg_clear(&fx->pending_damage); /* 这一版的账已经算清了 */
	sl->tex = NULL;
	sl->blur_own_tex = NULL;
	if (!blur_set_alloc(server, &sl->blur_set, rw, rh, levels)) {
		return false;
	}
	if (partial != NULL) {
		blur_chain_rect(server, &sl->blur_set, sl->own_bd_tex, rw, rh,
			&(struct wlr_box){ .x = px, .y = py,
				.width = pw, .height = ph },
			blur_reach_px(server, levels));
	} else {
		blur_chain(server, &sl->blur_set, sl->own_bd_tex, rw, rh);
	}
	sl->blur_own_tex = sl->blur_set.up_tex[0];
	if (sl->blur_own_tex == NULL) {
		return false;
	}
	/* 缓存键是「世界减掉这个窗口」的代次（见 vidow_backdrop_damage_view）。 */
	sl->blur_set.gen = server->gen_scene - view->self_gen;
	sl->blur_set.valid = true;
	sl->blur_own_id = server->blur_serial++;
	/* 这块区域本身（blur_tex_x/y/w/h）不在这儿登记：它跟着贴图一起由
	 * blur_assign() 换（见那里的注释），一处管一处，免得两边不一致。 */

	static int dumped = 0;
	if (dumped < blur_dump_limit()) {
		dump_buffer(server, sl->own_bd, rw, rh, "vidow-bd", dumped);
		/* 测试要知道这份底衬是谁的：文件名保持 vidow-bd-<n>（别的测试按它
		 * 排序取最后一份），owner 单独一个 sidecar，不碰 .txt 的格式。 */
		char opath[128];
		snprintf(opath, sizeof(opath), "/tmp/vidow-bd-%d.owner", dumped);
		FILE *of = fopen(opath, "w");
		if (of != NULL) {
			fprintf(of, "%s\n", vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "?");
			fclose(of);
		}
		if (sl->blur_set.up[0] != NULL && rw > 1 && rh > 1) {
			dump_buffer(server, sl->blur_set.up[0], rw >> 1, rh >> 1,
				"vidow-blur", dumped);
		}
		dumped++;
	}
	/* 这一行每次模糊重跑一行（滚动 / 动画里每帧每个窗口都可能触发）。
	 * 日常使用不该刷，但排查「模糊重跑为什么这么频繁 / 损伤框算错」时它是
	 * 唯一的线索 —— 用 VIDOW_BLUR_TRACE=1 开，别常驻。 */
	if (blur_trace_on()) {
		wlr_log(WLR_DEBUG, "blur: own region %s %dx%d at %d,%d levels=%d"
			"（损伤 %dx%d at %d,%d，%s %dx%d）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
			rw, rh, rx, ry, levels,
			dmg.width, dmg.height, dmg.x, dmg.y,
			partial != NULL ? "局部重跑" : "整块重跑", pw, ph);
	}
	return true;
}

/* Free everything this output owns. Called when the output goes away or its
 * size/scale changes.
 *
 * 借出去的贴图也在这里销毁（共享的那份是所有不重叠窗口一起用的），所以每个借过它
 * 的窗口都必须松手：热插拔把窗口挪到另一块屏时，紧接着就会重摆 + 重画，手里要是
 * 还攥着已经销毁的贴图，GLES2 那边直接是 `wlr_texture_is_gles2` 断言崩溃。
 * 下一帧 blur_update() 会把该借的重新借回来，所以这里清空不丢东西。 */
void vidow_blur_output_finish(struct vidow_output *output) {
	shared_blur_drop(output);
	blur_drop(output);

	struct vidow_view *view;
	wl_list_for_each(view, &output->server->views, link) {
		struct vidow_blur_slot **link = &view->fx.blur_slot;
		while (*link != NULL) {
			struct vidow_blur_slot *sl = *link;
			if (sl->out != output->wlr_output) {
				link = &sl->next;
				continue;
			}
			*link = sl->next;
			if (view->fx.cur_slot == sl) view->fx.cur_slot = NULL;
			instance_drop(sl);
		}
		if (view->fx.bake_output == output->wlr_output)
			view->fx.bake_output = NULL;
		if (view->fx.blur_owned) {
			continue;
		}
		blur_assign(&view->fx, NULL, NULL, false, 0, NULL);
	}
}

/* 这块窗口的私有模糊底衬归哪块屏。
 *
 * **必须和装饰离屏缓冲的倍率用同一个判据**（view.c 的 view_scale 走的是
 * view_home_output：窗口 -> 工作区 -> 屏，拖动中也不破例 —— 窗口还没换工作区就
 * 还在原来那块屏上）。底衬的尺寸和原点（blur_out_*）是按这块屏的设备像素记的，
 * 装饰着色器再拿它算采样 UV；两边判据一旦不一致：
 *
 *   - 倍率按旧屏（1x，离屏缓冲 2076 宽）、底衬按新屏（2x，区域按 3840 记），
 *     UV 的除数和实际贴图对不上 —— 屏幕上是一条一条的横道（用户报的
 *     「跨屏拖拽背景全是横条」）；
 *   - 反过来（倍率按新屏、底衬按旧屏）就是整块底衬被拉成中间一小块。
 *
 * 以前这里用的是 vidow_view_display_output（「看得见的那块屏」，会给正在跨屏
 * 拖的窗口换成对面那块屏），注释里写着「和 view_scale 用同一个判据」，可实际上
 * 两者根本不是一回事。现在改成取同一个来源。 */
static struct wlr_output *view_blur_owner(struct vidow_view *view) {
	struct wlr_output *out = vidow_view_display_output(view);
	return out != NULL ? out : view->output;
}

/* 窗口和这块屏有没有交集 —— 也就是「这块屏这一趟要不要管它的底衬」。
 *
 * 照 Hyprland 的 shouldRenderWindow(pWindow, pMonitor)（Renderer.cpp）：它没有
 * 「跨屏的窗口」这个概念，主循环对每块显示器各问一次「窗口和我的交集是不是
 * 空的」，非空就在这块屏上画一次、用这块屏自己的模糊纹理（m_blurFB 每屏一张）。
 *
 * 以前这里只认 view_blur_owner（窗口**归属**的那一块屏），于是窗口骑在两块屏
 * 中间时，跨过去的那半边没有对面屏的底衬可用 —— 屏幕上就是用户 2026-10-23
 * 报的「一半正常圆斑、另一半被拉成横条」。
 *
 * 判据用 screen_box（含动画偏移，和裁剪 / 倍率同一份几何）。 */
/* ob = 这块屏的布局框，由调用方算好传进来。
 *
 * **别在这儿自己查**：这是个每帧 × 每个窗口都会走到的判据，
 * wlr_output_layout_get_box 是一次输出布局链表遍历。以前它就在函数体里，
 * N 个窗口每帧 N 次同样的遍历 —— 而调用方（vidow_blur_update）本来就要
 * 这个值（它要按屏的原点算底衬 UV），算一遍传下来就行。 */
static bool view_blur_hits_output(struct vidow_view *view,
		struct vidow_output *output, const struct wlr_box *ob) {
	if (view->mapped && view->scene_tree != NULL) {
		struct wlr_box vb = vidow_view_screen_box(view);
		struct wlr_box hit;
		if (wlr_box_intersection(&hit, &vb, ob)) {
			return true;
		}
	}
	/* 不相交的窗口仍旧归它自己那块屏管（收尾 / 释放），否则跨屏结束那一下
	 * 没人来把已经用不上的底衬收掉。 */
	struct wlr_output *own = view_blur_owner(view);
	if (own == output->wlr_output) {
		/* 别在这儿打日志：这是每帧 × 每个窗口都会走到的判据，之前留了一行
		 * DEBUG 在这儿，日常使用直接刷出 8.6GB 日志（实测占全部日志的 93%，
		 * 而且经常同一毫秒重复好几行）。要查跨屏模糊归属就用 blur 那条统计行
		 * 或者临时加回这一行，别常驻。 */
		return true;
	}
	return false;
}

/* Called once per frame before anything is drawn, so the decorations can
 * sample this frame's backdrop. Everything is cached: a frame where nothing
 * moved behind a window touches no GPU buffer at all. */
void vidow_blur_update(struct vidow_output *output) {
	struct vidow_server *server = output->server;
	struct vidow_view *view;

	/* 这一帧的序号（共享抓取按它判断「同一帧只抓一次」）。放在最前面：早退
	 * 的那一帧也往前走一格，下一帧真要画时就不会复用上一帧的共享图。 */
	blur_frame_seq++;

	/* 这块屏的布局框：只跟 output 有关，算一次给下面所有窗口共用
	 * （以前每个窗口每帧各查一遍，见 view_blur_hits_output 的注释）。 */
	struct wlr_box out_box = {0};
	wlr_output_layout_get_box(server->output_layout,
		output->wlr_output, &out_box);

	int w = output->wlr_output->width, h = output->wlr_output->height;
	bool usable = server->shader_ok && server->config.blur_enabled && w > 0 &&
		h > 0 && w <= MAX_TARGET_PX && h <= MAX_TARGET_PX;

	/* **这一帧什么都没变就直接走人。**
	 *
	 * gen_scene 是「世界里有多少东西动过」的计数器（damage_bump 每标一次
	 * +1）：窗口交帧、拖动、配置重载、layer 上下屏、输出重排全都会让它前进。
	 * 它没动 = 上一帧算好的账这一帧还成立 —— 下面那一长串遍历（views ×
	 * layers × popups，每一项都要解几何、查槽位、比 mask）纯属每帧白跑。
	 *
	 * 只在 usable 这条路上早退：!usable 那条是「收回已经建出来的资源」，
	 * 配置刚把模糊关掉的那一刻 gen_scene 不一定动过，必须每帧都能跑到。
	 *
	 * 记账放在判断**之后**、遍历之前：遍历中途 return 掉也已经记上了，因为
	 * 这一帧的结论对整帧都成立。 */
	if (usable && output->blur_seen_scene == server->gen_scene &&
			output->blur_seen_layer == server->gen_layer) {
		return;
	}
	if (usable) {
		output->blur_seen_scene = server->gen_scene;
		output->blur_seen_layer = server->gen_layer;
	}

	if (!usable) {
		wl_list_for_each(view, &server->views, link) {
			if (!view_blur_hits_output(view, output, &out_box)) {
				continue; /* 别的屏上的窗口归那一趟收 */
			}
			blur_target_release(&view->fx);
		}
		struct vidow_layer_surface *off;
		wl_list_for_each(off, &server->layers, link) {
			/* 只收**这块屏**的：上面的 width/height 是 per-output 的，
			 * 一块屏尺寸越界（超过 MAX_TARGET_PX）不代表别的屏也不能画。
			 * 不筛的话，那块超尺寸的屏一帧就能把另一块屏上正常工作的
			 * layer 模糊节点销毁掉，而另一块屏的 usable 是 true、根本不会
			 * 走到这个分支来重建 —— 那块屏的模糊就此消失。 */
			if (off->output != output) {
				continue;
			}
			vidow_layer_blur_release(off);
		}
		struct vidow_popup_blur *pb;
		wl_list_for_each(pb, &server->popup_blurs, link) {
			struct vidow_output *owner = popup_blur_owner(pb);
			if (owner == output || owner == NULL) {
				popup_blur_release(pb);
			}
		}
		shared_blur_drop(output);
		blur_drop(output);
		return;
	}

	/* Keep caches allocated, but do not capture/blur a wallpaper nobody can
	 * sample. Be conservative for popups and overview; stale generations are
	 * retained so the first returning consumer rebuilds before sampling. */
	bool consumer = vidow_overview_active(server);
	wl_list_for_each(view, &server->views, link) {
		if (view_wants_blur(view) && view_blur_hits_output(view, output, &out_box)) {
			consumer = true;
			break;
		}
	}
	struct vidow_layer_surface *consumer_layer;
	wl_list_for_each(consumer_layer, &server->layers, link) {
		if (consumer_layer->output == output &&
				layer_blur_wanted(server, consumer_layer)) {
			consumer = true;
			break;
		}
	}
	struct vidow_popup_blur *consumer_popup;
	wl_list_for_each(consumer_popup, &server->popup_blurs, link) {
		if (popup_blur_owner(consumer_popup) == output) {
			consumer = true;
			break;
		}
	}
	bool shared_ok = false;
	if (!consumer) {
		/* Still run consumer cleanup and decoration updates below. */
		goto update_consumers;
	}

	int levels = blur_levels_for(server, w, h);

	if (output->backdrop == NULL || output->backdrop_w != w ||
			output->backdrop_h != h) {
		blur_drop(output);
		shared_blur_drop(output); /* the pyramid is sized for the old output */
		output->backdrop = vidow_alloc_target(server, w, h);
		if (output->backdrop == NULL) {
			return;
		}
		/* 未模糊的底衬也留一张贴图：被窗口遮住的 layer surface 拿它当自己那一
		 * 小块区域底衬的基座（比每次重抓一遍壁纸便宜太多，见 layer_region_build）。
		 * 贴图是包的缓冲，不包像素 —— 底衬每帧重画也不影响它，建一次就够。 */
		output->backdrop_tex = stat_tex_new(
			wlr_texture_from_buffer(server->renderer, output->backdrop));
		if (output->backdrop_tex == NULL) {
			blur_drop(output);
			return;
		}
		output->backdrop_w = w;
		output->backdrop_h = h;
	}

	/* 缓存键必须把所有影响这张贴图的参数都算上：级数、采样步长。
	 * 漏了 blur_offset 的时候，运行时改它看着「没反应」—— 贴图是旧参数算的，谁也
	 * 没让它重算。（blur_alpha_min 原来也在这儿，现在它只进装饰着色器、不进金字塔，
	 * 改它由 blur_mask_min_changed() 直接让帧循环重画。） */
	shared_ok = output->blur_tex != NULL &&
		output->blur_gen == output->backdrop_revision &&
		output->blur_levels == levels &&
		fabs(output->blur_offset - server->config.blur_offset) < 1e-6 &&
		output->blur_set.w == w && output->blur_set.h == h;
	if (!shared_ok) {
		shared_ok = shared_blur_rebuild(server, output, w, h, levels);
	}

update_consumers:
	/* **这块屏自己的**布局原点（设备像素），不是 server->output_x/y。
	 * 那两个是「最后渲染的那块屏」留下的全局量：两屏同帧刷新时，
	 * 后渲染的屏会把前面的覆盖掉，先渲染的屏那批窗口就拿错原点 ——
	 * 底衬整块横向错位，屏幕上就是用户报的「跨屏模糊一直是一条条横纹」。
	 * 布局原点按设备像素算：output->wlr_output 的布局框是逻辑坐标，
	 * 乘自己的倍率才是底衬贴图所在的那个像素空间。
	 *
	 * 提到循环外：这组值只跟 output 有关、跟 view 无关，以前放在窗口循环体内，
	 * 每个窗口每帧都重算一次 wlr_output_layout_get_box（那是一次输出布局链表
	 * 遍历）。N 个窗口就是 N 次同样的遍历，纯浪费。 */
	double oscale_o = output->wlr_output->scale > 0.0 ?
		output->wlr_output->scale : 1.0;
	/* 布局框在函数开头已经查过一次了（out_box），别再查一遍。 */
	const struct wlr_box *obox = &out_box;
	int my_out_x = (int)lround((double)obox->x * oscale_o);
	int my_out_y = (int)lround((double)obox->y * oscale_o);

	wl_list_for_each(view, &server->views, link) {
		/* 私有底衬是「**这块屏**上的这一块区域」，别的屏上的窗口不归这一趟管。
		 *
		 * 少了这一句，两块屏会轮流给同一个窗口建金字塔：每块屏都按自己的
		 * scale / 尺寸算区域（1 倍屏把 280x380 的窗口算成 640x768，2 倍屏算成
		 * 896x1152），而金字塔是挂在窗口身上的（view->fx.blur_set），于是两趟
		 * 每帧互相把对方的缓冲丢掉重建 —— 一次十几块 GBM 缓冲（最大那块
		 * 2304x2560 = 23 MB），拖窗口时每秒几千次分配，屏幕上一顿一顿的。
		 * 顺带也修了「区域和贴图归哪块屏」：以前最后写进 fx 的是哪一趟算的
		 * 就归谁，跨屏的窗口可能采到另一块屏的底衬。
		 *
		 * 归属判据和装饰离屏缓冲的倍率是**同一个**（view_blur_owner ——
		 * 「窗口现在画在哪块屏上」）：两个地方各算各的话，拖到对面那块屏上时
		 * 倍率对不上，模糊贴图的采样坐标就落错地方（那块区域是按底衬这块屏的
		 * 设备像素记的）。 */
		/* 窗口归哪块屏就用哪块屏的底衬。
		 *
		 * 试过「和这块屏相交就建」（照 Hyprland 的 per-monitor 模型改）——
		 * 不行：窗口的私有底衬是**一份**（view->fx 里 blur_tex_x/y/w/h 那几个
		 * 字段是单值），两块屏同帧各建一份、后写的把先写的覆盖掉，结果整窗
		 * 都变成横条，比改之前更糟。真要走那条路，得把 fx 那组字段改成每屏
		 * 一份，是另一层重构（见 server.h 的 blur_set 注释）。 */
		if (!view_blur_hits_output(view, output, &out_box)) {
			continue;
		}
		/* 这张贴图代表的是**这块屏**的视野：尺寸和布局原点跟着贴图一起记在 fx
		 * 上（见 server.h 那组 blur_out_*）。采样只认这一组，不现场读
		 * server->output_*：重烘那趟会给别的屏上的窗口也烘一次，那一刻的
		 * server->output_w 是那块屏的，按它算 UV 就是「只取中间一块铺满整窗」。 */
		/* 视野几何变了就得重烘：UV 是在**烘的那一刻**按这一组算的（见下面
		 * decor_pass 之前那几个 origin_x / out_size_x），烘好之后光改字段
		 * 不会让已经烘进去的 UV 跟着变。
		 *
		 * 跨屏拖动正是「贴图身份没变、视野换了」的那一类：同倍率的两块屏
		 * 之间拖，模糊贴图的尺寸和内容只差一个原点，blur_assign() 判
		 * blur_id 相同就早退、blur_ver 不动 —— 装饰拿着旧屏的 origin 去采
		 * 新屏的贴图，整块底衬错位 / 拉花（用户报的「跨屏后背景模糊纹理
		 * 会出问题」）。所以视野这四个数变了就自己把版本号推一格。 */
		/* **这块屏自己的**布局原点（设备像素），不是 server->output_x/y。
		 * 那两个是「最后渲染的那块屏」留下的全局量：两屏同帧刷新时，
		 * 后渲染的屏会把前面的覆盖掉，先渲染的屏那批窗口就拿错原点 ——
		 * 底衬整块横向错位，屏幕上就是用户报的「跨屏模糊一直是一条条横纹」。
		 *
		 * 值在循环外算好了（见 update_consumers 上面那一段）：它只跟 output
		 * 有关，跟 view 无关，放循环里就是每个窗口重算一遍。 */
		/* 视野写进**这块屏自己的那一格**（跨屏时另一格是另一块屏的，不能碰）。 */
		struct vidow_blur_slot *oslot = blur_slot_for(&view->fx,
			output->wlr_output, true);
		if (oslot != NULL) {
			if (oslot->ow != output->wlr_output->width ||
					oslot->oh != output->wlr_output->height ||
					oslot->ox != my_out_x || oslot->oy != my_out_y) {
				view->fx.blur_ver++;
				blur_ver_hits[4415]++;
			}
			oslot->layout_box = *obox;
			oslot->ow = output->wlr_output->width;
			oslot->oh = output->wlr_output->height;
			oslot->ox = my_out_x;
			oslot->oy = my_out_y;
		}
		/* Per-output instances already checked their own geometry above.
		 * Updating legacy fallback fields must not invalidate both instances
		 * whenever two different outputs take turns rendering this view. */
		if (oslot == NULL &&
				(view->fx.blur_out_w != output->wlr_output->width ||
				view->fx.blur_out_h != output->wlr_output->height ||
				view->fx.blur_out_x != my_out_x ||
				view->fx.blur_out_y != my_out_y)) {
			view->fx.blur_ver++;
			blur_ver_hits[4431]++;
		}
		view->fx.blur_out_w = output->wlr_output->width;
		view->fx.blur_out_h = output->wlr_output->height;
		view->fx.blur_out_x = my_out_x;
		view->fx.blur_out_y = my_out_y;
		/* **bake_output 是「这个窗口按哪块屏的倍率烘」，不是「这一趟在哪块屏
		 * 上画」—— 别每帧覆盖它。**
		 *
		 * 这里原来无条件写 output->wlr_output。两块屏同帧各跑一遍
		 * vidow_blur_update，于是它每帧被改写两次，而它正是下面这些判据的
		 * 一环：view_bake_for_output 里 dec_output_scale / dec_output_box
		 * 跟着它取，vidow_fx_frame 也拿它选 instance。结果每块屏的装饰缓存
		 * 下一帧必然对不上，dec_valid 永远「要重烘」—— 实测静止画面
		 * 3268 次 bakeout / 6 秒，blur_ver 每秒涨 172。整机白转。
		 *
		 * 也让「第一次跨屏拖拽卡住」有了根：拖到两块屏中间那几帧，谁最后
		 * 跑就把 bake_output 抢走，窗口下一帧按别的屏的倍率烘。
		 *
		 * 只在**归属真的变了**的时候写：判据用 view_blur_owner（跟倍率、
		 * 底衬归属同一个真值，见它的注释）。跨屏那一半的临时借用由
		 * view_bake_for_output 自己存/恢复，不经过这里。 */
		struct wlr_output *owner_out = view_blur_owner(view);
		if (owner_out != NULL) {
			view->fx.bake_output = owner_out;
		} else if (view->fx.bake_output == NULL) {
			/* 没有归属（浮窗 / 弹窗）：退回「这一趟画的这块屏」——
			 * 但只在从没设过时写一次，不跟着每帧翻。 */
			view->fx.bake_output = output->wlr_output;
		}
		if (!view_wants_blur(view) || view->fx.node == NULL) {
			if (view->fx.blur_own_tex != NULL) {
				blur_own_drop(&view->fx);
			}
			own_bd_drop(&view->fx); /* 这块屏上不再需要私有底衬了 */
			blur_assign(&view->fx, NULL, NULL, false, 0, NULL);
			continue;
		}

		blur_mask_min_changed(&view->fx, server->config.blur_alpha_min);

		struct vidow_look look;
		vidow_view_look(&server->config, &view->rule_eff,
			view->fullscreen, &look);
		bool overlaps = true;
		if (ov_stats_on()) {
			struct timespec t0, t1;
			clock_gettime(CLOCK_MONOTONIC, &t0);
			overlaps = view_overlaps_other(view);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			ov_ns += (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull +
				(uint64_t)(t1.tv_nsec - t0.tv_nsec);
			ov_n++;
		} else {
			overlaps = view_overlaps_other(view);
		}
		if (look.blur_xray || !overlaps) {
			/* Tiled and non-overlapping floating windows all share the
			 * wallpaper backdrop: one capture and one chain for all of them.
			 * Only drop a private pyramid if there really is one, otherwise
			 * the identity would change every frame and force a redraw.
			 *
			 * blur_xray 也走这条路：重叠时照样用整屏那份（壁纸 + layer 那层），
			 * 忽略别的窗口 —— 糊的是壁纸而不是压着的那个窗口（Hyprland 的
			 * blur:xray 同款）。私有底衬一块不建：重叠场景每帧的趟数从
			 * N×(1+2·级数−1) 掉到 ~3，那块显存也全免。 */
			if (view->fx.blur_own_tex != NULL) {
				blur_own_drop(&view->fx);
			}
			own_bd_drop(&view->fx); /* 不再重叠：私有底衬那块区域可以还了 */
			blur_assign(&view->fx, output->wlr_output, output->blur_tex, false, output->blur_id, NULL);
			continue;
		}

		if (!shared_ok) {
			blur_assign(&view->fx, NULL, NULL, false, 0, NULL);
			continue;
		}

		if (oslot == NULL) continue;
		int rx, ry, rw, rh, rlevels;
		if (!view_region_rect(server, output, view, &rx, &ry, &rw, &rh,
				&rlevels)) {
			blur_assign(&view->fx, NULL, NULL, false, 0, NULL);
			continue;
		}
		/* 抓的时候这个窗口是藏起来的，所以它自己的位置和内容都不进这张图：
		 * 世界代次只是粗筛，真正的判据是「这次损伤有没有碰到我这一块」—— 别的
		 * 窗口在屏幕另一头重画（视频 / 浏览器每帧都在交帧）不该让我重抓。
		 * 区域本身已经把模糊半径算进去了（见 view_region_rect）。 */
		double oscale = output->wlr_output->scale;
		if (!(oscale > 0.0)) {
			oscale = 1.0;
		}
		struct wlr_box region_logical = {
			.x = output->box.x + (int)floor((double)rx / oscale),
			.y = output->box.y + (int)floor((double)ry / oscale),
			.width = (int)ceil((double)rw / oscale) + 1,
			.height = (int)ceil((double)rh / oscale) + 1,
		};
		bool key_changed =
			oslot->blur_set.gen != server->gen_scene - view->self_gen;
		bool touched = key_changed &&
			blur_damage_hits(&view->fx, &region_logical);
		/* 形状（区域位置尺寸 / 级数 / 半径）没变：那张金字塔就是上一版的，可以
		 * 只更新受损的那一块（见 view_region_build 的 partial）。 */
		bool shape_ok = oslot->blur_set.valid &&
			oslot->blur_own_tex != NULL &&
			oslot->blur_set.w == rw && oslot->blur_set.h == rh &&
			oslot->blur_set.levels == rlevels &&
			fabs(oslot->blur_set.offset - server->config.blur_offset) < 1e-6 &&
			oslot->rx == rx && oslot->ry == ry &&
			oslot->rw == rw && oslot->rh == rh &&
			oslot->own_bd != NULL && oslot->own_bd_w == rw &&
			oslot->own_bd_h == rh;
		bool fresh = shape_ok && !touched;
		if (!fresh) {
			struct wlr_box upd;
			bool partial = shape_ok && !blur_no_partial() &&
				region_update_rect(view->server, &view->fx, output,
					rx, ry, rw, rh, &upd);
			if (!view_region_build(server, output, view, rx, ry, rw, rh,
					rlevels, partial ? &upd : NULL)) {
				blur_assign(&view->fx, NULL, NULL, false, 0, NULL);
				continue;
			}
		}
		/* owned = false：这张贴图归 oslot->blur_set（blur_set_drop 收），
		 * 这里只是指过去采样。区域跟着一起报（见 blur_assign）。 */
		blur_assign(&view->fx, output->wlr_output, oslot->blur_own_tex, false,
			oslot->blur_own_id,
			&(struct wlr_box){ .x = rx, .y = ry, .width = rw, .height = rh });
	}

	/* layer surface 背后的模糊（状态栏 / 面板那层）。bottom 层用共享的那份底衬
	 * （壁纸 + background 层），一次抓取都不用；top / overlay 层底下有窗口，各
	 * 自抓自己那一小块（见 layer_region_build）。 */
	/* 这一帧要不要把「layer 之下」抓一次给所有层共用（只有区域互相压住才
	 * 值得，见 layer_under_plan）。 */
	layer_under_plan(server, output);
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		if (ls->output != output) {
			continue;
		}
		if (!layer_blur_wanted(server, ls)) {
			vidow_layer_blur_release(ls);
			continue;
		}
		/* overview 里 top / overlay 那几棵树整体被关掉了，这层模糊没人看得见：
		 * 别每帧白抓一份（overview 的动画每帧都在动世界代次）。 */
		if (vidow_overview_active(server) &&
				ls->layer != ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM) {
			continue;
		}
		/* 这一帧画不出来（两个目标都还在飞 / 底衬还没建好）就留着上一帧的
		 * 节点：别一帧有、一帧没有地闪。 */
		(void)layer_blur_frame(server, output, ls);
	}

	/* 弹出物背后的模糊（xdg_popup / X11 菜单 / 输入法候选框）：跟 layer 那条
	 * 路同一套，每块屏只管「中心落在自己身上」的那些。 */
	struct vidow_popup_blur *pb;
	wl_list_for_each(pb, &server->popup_blurs, link) {
		popup_blur_frame(server, output, pb);
	}

	/* overview：整屏一张糊掉的壁纸（niri 那样），垫在压暗遮罩下面。 */
	vidow_overview_blur_frame(server, output);

	/* **跨屏拖拽：这块屏把自己那一半按自己的倍率画一遍。**
	 *
	 * 照 Hyprland（Renderer.cpp 的 renderMonitor 逐屏循环）：没有「跨屏的
	 * 窗口」，每块屏各渲染一遍、各裁到自己那块屏、各用自己那块屏的模糊
	 * 纹理（m_blurFB 每屏一张）。两块屏各画一半，拼起来看着像跨屏。
	 *
	 * Vidow 的窗口装饰是离屏烘的一张图，图上烘死了采样坐标和倍率，所以
	 * 「每屏一遍」落地成「每块屏各烘一份自己的装饰缓冲」（见
	 * struct vidow_blur_slot 的 dec*）。只在窗口真的骑在两块屏中间时才
	 * 多烘一份。 */
	view_bake_for_output(server, output);
}

/* **这块屏把自己那一半画一遍**（照 Hyprland 的 renderMonitor 逐屏循环）。
 *
 * 只在窗口真的骑在两块屏中间时才多烘一份：窗口完全落在某块屏里的时候，
 * 那一趟已经在 vidow_view_anim_bake 里烘过了（bake_output 就是它）。
 *
 * 这里做的只是「换一块屏、同一个窗口、再烘一遍」——几何、倍率、底衬全都取
 * 那一块屏自己的（ensure_targets 按 bake_output 选缓冲，decor_draw 按
 * bake_output 取底衬槽），所以两半各自按所在屏的缩放渲染。
 *
 * 显示的裁剪交给场景：装饰节点的 clip（fx->clip，见 view_clip_box）已经
 * 收紧到工作区 / 屏，超出去的那块根本不画。两块屏各显示自己那一半。 */
static void view_bake_for_output(struct vidow_server *server,
		struct vidow_output *output) {
	struct vidow_view *view;
	/* 这块屏的布局框只跟 output 有关，提在循环外算一次：放在循环体内就是每个
	 * 窗口每帧一次 wlr_output_layout_get_box（输出布局链表遍历）。 */
	struct wlr_box ob = {0};
	wlr_output_layout_get_box(server->output_layout,
		output->wlr_output, &ob);
	wl_list_for_each(view, &server->views, link) {
		if (!view->mapped || view->popup || view->scene_tree == NULL ||
				!view->fx.active || view->fx.node == NULL) {
			continue;
		}
		/* **overview 里没有「跨屏的另一半」。**
		 *
		 * 全景把每一行画成一块缩小版的屏，行本身就在窗口自己那块屏里；
		 * 一份装饰缓冲就够了。更不能拿下面那个 vb（**布局坐标**的 geom）
		 * 去判「跟这块屏相不相交」：overview 里节点画的是**行坐标**
		 * （vidow_overview_map：行的 content_x/y + 局部 × scale），和布局
		 * 里的 geom 差着视口 / 缩放。窗口在屏幕上明明还露着半块，vb 却落在
		 * 屏外 —— 这一趟就把槽位节点关掉，窗口整个消失（用户报的
		 * 「overview 的窗口超出一点显示器就消失」）。
		 *
		 * 归属那块屏的那一份由 vidow_view_apply_anim 每帧烘好、摆好
		 * （overview_apply 对每个窗口都调一次），这里只把**别的屏上残留**
		 * 的槽位收掉：进 overview 之前跨屏拖过的话，对面那格还停在老位置，
		 * 不收就是一块残影。归属不明的（owner == NULL）别动 —— 收错了就是
		 * 整块不画。 */
		if (vidow_overview_active(server)) {
			struct wlr_output *own = view_blur_owner(view);
			struct vidow_blur_slot *sl = blur_slot_for(&view->fx,
				output->wlr_output, false);
			if (own != NULL && own != output->wlr_output &&
					sl != NULL && sl->node != NULL) {
				wlr_scene_node_set_enabled(&sl->node->node, false);
			}
			continue;
		}
		/* Output instances also need updating after release and during layout
		 * animations; otherwise the non-owner half retains an old buffer. */
		struct wlr_box vb = vidow_view_screen_box(view);
		/* **先问「它现在到底看得见吗」，再问「跟这块屏相交吗」。**
		 *
		 * 平铺窗口的 geom 里含着滚动视口的偏移，而那是**布局绝对坐标**：
		 * DP-2 上两列宽 1909、视口滚到第二列时，第一列的
		 * geom.x = 1920 + 0 - 1901 = 19 —— 它在 DP-2 左侧屏外（被
		 * view_clip_box 裁光了），可那个坐标**落在 DP-3 的范围里**
		 * （0..1920）。直接拿 vb 跟这块屏的框做交集，就会判成「这个窗口
		 * 骑在 DP-3 上」，每帧给它白烘一份 DP-3 的装饰 —— 实测静止 6 秒
		 * 3459 次 bakeout，view=7 明明在 DP-2 上却有 869 次 out=DP-3，
		 * 而且 rev 永远不命中缓存（DP-2 那趟刚烘完，DP-3 这趟又把
		 * dec_screen_box 改掉，下一帧又反过来）。
		 *
		 * fx->clip 就是 view_clip_box 的结果（见 view.c 的
		 * vidow_view_apply_clip）：滚出视口的窗口裁完是空的，跟它一比
		 * 就现原形。裁剪关着（浮窗 / 正在拖）时才退回按 vb 判。 */
		if (view->fx.clip_on) {
			struct wlr_box visible = {0};
			if (!wlr_box_intersection(&visible, &vb, &view->fx.clip)) {
				struct vidow_blur_slot *sl = blur_slot_for(&view->fx,
					output->wlr_output, false);
				if (sl != NULL && sl->node != NULL)
					wlr_scene_node_set_enabled(&sl->node->node, false);
				continue;
			}
			vb = visible;
		}
		struct wlr_box hit;
		if (!wlr_box_intersection(&hit, &vb, &ob)) {
			struct vidow_blur_slot *sl = blur_slot_for(&view->fx,
				output->wlr_output, false);
			if (sl != NULL && sl->node != NULL)
				wlr_scene_node_set_enabled(&sl->node->node, false);
			continue;
		}
		/* An output frame is not a content update. Reuse its decoration until
		 * a commit/placement/effect update, backdrop change or output change.
		 * This also avoids rebaking the owner immediately after apply_anim. */
		struct vidow_blur_slot *sl = blur_slot_for(&view->fx,
			output->wlr_output, false);
		if (sl != NULL && sl->dec_valid && sl->node != NULL &&
				sl->node->node.enabled &&
				sl->dec_revision == view->fx.bake_revision &&
				sl->dec_blur_ver == view->fx.blur_ver &&
				sl->dec_output_scale == output->wlr_output->scale &&
				sl->dec_output_box.x == ob.x && sl->dec_output_box.y == ob.y &&
				sl->dec_output_box.width == ob.width &&
				sl->dec_output_box.height == ob.height) {
			if (vidow_xtrace_on()) {
				wlr_log(WLR_INFO, "bake: %s 缓存命中跳过重烘 "
					"rev=%u/%u blur=%u/%u",
					vidow_view_title(view) != NULL ?
						vidow_view_title(view) : "(无题)",
					sl->dec_revision, view->fx.bake_revision,
					sl->dec_blur_ver, view->fx.blur_ver);
			}
			continue;
		}
		if (vidow_xtrace_on()) {
			wlr_log(WLR_INFO, "bake: %s 要重烘 dec_valid=%d en=%d "
				"rev=%u/%u blur=%u/%u",
				vidow_view_title(view) != NULL ?
					vidow_view_title(view) : "(无题)",
				sl != NULL ? sl->dec_valid : -1,
				sl != NULL && sl->node != NULL ?
					sl->node->node.enabled : -1,
				sl != NULL ? sl->dec_revision : 0,
				view->fx.bake_revision,
				sl != NULL ? sl->dec_blur_ver : 0, view->fx.blur_ver);
		}
		/* **烘完把 bake_output 还回去。**
		 *
		 * 这一趟是「这块屏借这个窗口再烘一份自己的那一半」，是临时的。
		 * 可 bake_output 是单值，vidow_view_anim_bake 下一帧会拿它当
		 * 「这个窗口该按哪块屏的倍率烘」—— 留着不回，窗口就永久按最后
		 * 那趟借它的屏来烘了。
		 *
		 * 实测症状（用户 2026-10-27）：窗口在 2x 的 DP-2 上，第一次往
		 * 1x 的 DP-3 拖，跨屏那一帧 DP-3 把 bake_output 抢过去，之后
		 * 每帧都按 DP-3 的 1x 烘 —— 窗口卡在 DP-2 左边缘，拖得越远背景
		 * 横条越多，超过一个窗口宽才突然跳过去；之后再拖就正常了（那时
		 * bake_output 已经稳定在 DP-3）。 */
		struct wlr_output *saved_bake_output = view->fx.bake_output;
		view->fx.bake_output = output->wlr_output;
		/* 跟手探针：跨屏那一半到底有没有被对面那块屏烘一次（一行 = 一次重烘）。 */
		if (vidow_xtrace_on() &&
				(view->dragging || server->grab_view == view)) {
			vidow_gtrace("bake v=%d out=%s vb=%d,%d %dx%d vm=%d,%d -> bake_scale=%.2f",
				view->id, output->wlr_output->name, vb.x, vb.y,
				vb.width, vb.height,
				(int)lround(view->anim_off_x), (int)lround(view->anim_off_y),
				output->wlr_output->scale);
		}
		if (vidow_xtrace_on()) {
			wlr_log(WLR_INFO, "bakeout: view=%d out=%s vb=%d,%d %dx%d "
				"cached[rev=%u blur=%u scale=%.2f box=%d,%d %dx%d] "
				"now[rev=%u blur=%u scale=%.2f box=%d,%d %dx%d]",
				view->id, output->wlr_output->name, vb.x, vb.y,
				vb.width, vb.height,
				sl != NULL ? sl->dec_revision : 0,
				sl != NULL ? sl->dec_blur_ver : 0,
				sl != NULL ? sl->dec_output_scale : 0.0,
				sl != NULL ? sl->dec_output_box.x : 0,
				sl != NULL ? sl->dec_output_box.y : 0,
				sl != NULL ? sl->dec_output_box.width : 0,
				sl != NULL ? sl->dec_output_box.height : 0,
				view->fx.bake_revision, view->fx.blur_ver,
				output->wlr_output->scale,
				ob.x, ob.y, ob.width, ob.height);
		}
		vidow_view_anim_bake(view);
		view->fx.bake_output = saved_bake_output;
	}
}

/* ------------------------------------------------------------------- draw */

/* 装饰那一趟的几何（全部是**渲染目标像素**口径）。屏幕那条路（decor_draw）和
 * 窗口抓帧那条路（vidow_fx_capture_decor）只有这一份不一样，uniform 那几十行
 * 共用同一段代码 —— 两处各写一遍必然走散，某处忘了乘 scale 就是「录出来跟屏上
 * 长得不一样」。
 *
 * off_x/off_y：内容（窗口可见区）左上角在渲染目标里的位置。屏幕那条路是内边距
 * px_pad（内容居中），窗口抓帧那条路是窗口在取景框里的位置（抓帧框常常就是窗口
 * 本身，于是是 0）。
 * origin_x / origin_y 与 out_size_x / out_size_y：内容左上角在**底衬贴图**里的
 * 像素坐标，以及那张贴图的尺寸。
 * 装饰着色器按它算模糊采样的 UV（见 decor_fragment_src 的 u_origin /
 * u_output_size）。 */
struct decor_geo {
	int dst_w, dst_h;
	int content_w, content_h;
	float off_x, off_y;
	float origin_x, origin_y;
	float out_size_x, out_size_y;
	/* 内容那一项的乘子：屏幕那条路是 1，窗口抓帧那条路是 0（内容由场景节点
	 * 画，这张图里只要底衬 / 画框，但剔除仍按内容 alpha 走）。 */
	float content_alpha;
};


static bool decor_pass(struct vidow_server *server,
		struct vidow_fx_target *fx, struct wlr_buffer *dst,
		const struct decor_geo *g,
		struct wlr_texture *src, struct wlr_texture *mask,
		const float border[4], double alpha, double scale,
		const struct vidow_look *look, bool shadow_only) {
	struct vidow_decor_prog *d = &server->decor;
	struct vidow_config *cfg = &server->config;

	struct wlr_gles2_texture_attribs sa;
	wlr_gles2_texture_get_attribs(src, &sa);

	struct wlr_egl *egl = NULL;
	if (begin_pass(server, dst, g->dst_w, g->dst_h, &egl) == 0) {
		return false;
	}

	glUseProgram(d->program);
	glUniform1f(d->u_shadow_only, shadow_only ? 1.0f : 0.0f);
	bind_tex(0, sa.target, sa.tex);
	glUniform1i(d->u_texture, 0);

	struct wlr_texture *blurt = fx_blur_tex_now(fx);
	struct wlr_texture *blur = blurt != NULL ? blurt : server->empty_tex;
	if (blur != NULL) {
		struct wlr_gles2_texture_attribs ba;
		wlr_gles2_texture_get_attribs(blur, &ba);
		bind_tex(1, ba.target, ba.tex);
	}
	glUniform1i(d->u_blur, 1);

	/* 元素自己的内容（透明度剔除用）。mask == NULL 时用 u_texture 那张内容贴图
	 * （窗口 / 弹窗：内容贴图就是元素自己）；layer surface 的模糊底衬不画内容，
	 * 调用方把 layer 自己的缓冲当 mask 递进来。采样器不能空着，配上 empty_tex
	 * 也无所谓：u_mask_on = 0 时着色器根本不采它。 */
	struct wlr_texture *mtex = mask != NULL ? mask : server->empty_tex;
	struct wlr_gles2_texture_attribs ma;
	wlr_gles2_texture_get_attribs(mtex, &ma);
	bind_tex(2, ma.target, ma.tex);
	glUniform1i(d->u_mask, 2);
	glUniform1f(d->u_mask_on, mask != NULL ? 1.0f : 0.0f);
	/* 阈值：客户端用 ext-background-effect 自己要了区域就不按元素 alpha 剔 ——
	 * 那块区域就是它画出来的边界（niri 的 need_ignore_alpha 同一个判断）。layer
	 * 底衬拿不到元素内容时也不能剔，不然整块模糊都没了。0 = step(0, a) 恒为 1，
	 * 也就是不剔。 */
	float mask_min = fx->mask_min > 0.0f ?
		fx->mask_min : (float)cfg->blur_alpha_min;
	if (fx->blur_region_on || (mask == NULL && fx->blur_only)) {
		mask_min = 0.0f;
	}
	glUniform1f(d->u_mask_min, mask_min);
	/* mask 的采样映射：默认恒等；xdg popup 只糊窗口体时采表面缓冲里的
	 * geometry 子矩形（见 popup_blur_frame）。 */
	float mask_ox = fx->mask_scale_x > 0.0f ? fx->mask_off_x : 0.0f;
	float mask_oy = fx->mask_scale_y > 0.0f ? fx->mask_off_y : 0.0f;
	float mask_sx = fx->mask_scale_x > 0.0f ? fx->mask_scale_x : 1.0f;
	float mask_sy = fx->mask_scale_y > 0.0f ? fx->mask_scale_y : 1.0f;
	glUniform4f(d->u_mask_map, mask_ox, mask_oy, mask_sx, mask_sy);

	/* 着色器全程在「渲染目标的物理像素」里算：配置里的圆角、边框、阴影都是
	 * 逻辑单位，乘上倍率才是像素。 */
	glUniform2f(d->u_size, (float)g->content_w, (float)g->content_h);
	glUniform2f(d->u_target_size, (float)g->dst_w, (float)g->dst_h);
	glUniform2f(d->u_content_off, g->off_x, g->off_y);
	glUniform1f(d->u_radius, (float)(look->corner_radius * scale));
	/* 圆角的形状指数：2 = 圆弧、4 = 超椭圆。夹一道（配置 / 环境变量 / IPC /
	 * 窗口规则四条路都夹过，这里只是「进着色器的值一定是合法的」）。 */
	glUniform1f(d->u_corner_n, (float)vidow_corner_n_clamp(look->corner_n));
	glUniform1f(d->u_border_width, (float)(look->border_width * scale));
	/* The decoration shader works in premultiplied alpha; the config keeps
	 * straight values so a script can read them back. */
	float border_pm[4] = { border[0] * border[3], border[1] * border[3],
		border[2] * border[3], border[3] };
	float shadow_pm[4] = { look->shadow_color[0] * look->shadow_color[3],
		look->shadow_color[1] * look->shadow_color[3],
		look->shadow_color[2] * look->shadow_color[3],
		look->shadow_color[3] };
	glUniform4fv(d->u_border_color, 1, border_pm);
	glUniform4fv(d->u_shadow_color, 1, shadow_pm);
	glUniform2f(d->u_shadow_offset, (float)(look->shadow_offset_x * scale),
		(float)(look->shadow_offset_y * scale));
	glUniform1f(d->u_shadow_radius, (float)(look->shadow_radius * scale));
	glUniform1f(d->u_opacity, (float)look->opacity);
	glUniform1f(d->u_content_alpha, g->content_alpha);
	glUniform1f(d->u_noise, (float)look->noise);
	glUniform1f(d->u_alpha, (float)alpha);
	glUniform1f(glGetUniformLocation(d->program, "u_time"),
		(float)(vidow_now_ns() % 1000000000000LL) / 1e9f);
	/* 窗口在输出上的位置（物理像素，输出原点已减掉）：散景贴图是按输出分辨
	 * 率抓的，采样坐标必须落在这块屏自己的坐标系里。被模糊的 layer surface
	 * 那张贴图只盖住它自己那一小块区域（fx->blur_tex_*），坐标就再减掉区域
	 * 原点、除数也换成区域尺寸 —— 不减就是整块错位。
	 *
	 * 那块屏是哪一块，认**跟着这张贴图一起记的** blur_out_*（见 server.h）：
	 * 重烘会落在别的屏正在渲染的那一帧上（output.c 帧循环那趟不看归属），
	 * 现场读 server->output_* 拿到的是那块屏的尺寸/原点 —— 整屏底衬只取中间
	 * 一块铺满整窗，看着就是「静止时底衬啪一下换挡」（中心放大 + 变糊）。 */
	/* 底衬采样的映射：全部由调用方算好（屏幕那条路按窗口在输出上的位置算，
	 * 窗口抓帧那条路按取景框算）—— 两边共用这一份 uniform，别在这儿现场读
	 * 全局量（跨屏底衬采样的坑，见 server.h 的 blur_out_*）。 */
	/* 跨屏没有「分界 / 第二套坐标」：一次渲染只采这一块屏的底衬，另一半由
	 * 对面那块屏自己那份装饰缓冲画（见 vidow_fx_frame 的按屏裁剪）。 */
	glUniform2f(d->u_origin, g->origin_x, g->origin_y);
	glUniform2f(d->u_output_size, g->out_size_x, g->out_size_y);
	float bsx = fx->blur_scale_x > 0.0f ? fx->blur_scale_x : 1.0f;
	float bsy = fx->blur_scale_y > 0.0f ? fx->blur_scale_y : 1.0f;
	glUniform2f(d->u_blur_scale, bsx, bsy);
	glUniform2f(d->u_blur_bias, fx->blur_bias_x, fx->blur_bias_y);
	glUniform1f(d->u_flip_y, cfg->shader_flip_y ? 1.0f : 0.0f);
	/* 模糊那层要不要叠：除了「有贴图」（fx->blur_tex）和全局开关（fx->blur），
	 * 还得看**这一次渲染自己的 look** —— 预览（缩略图）那一趟在
	 * vidow_fx_frame 里把 look.blur 关掉，于是预览里不叠模糊（见那里的注释）。 */
	float strength = fx->blur && blurt != NULL && look->blur ?
		(float)cfg->blur_strength : 0.0f;

	glUniform1f(d->u_blur_strength, strength);
	/* 客户端用 ext-background-effect 指定了区域就只模糊那块 */
	glUniform1f(d->u_blur_region_on, fx->blur_region_on ? 1.0f : 0.0f);
	/* 区域是 surface 局部坐标（逻辑），着色器在物理像素里比对。 */
	glUniform4f(d->u_blur_region, (float)(fx->blur_region[0] * scale),
		(float)(fx->blur_region[1] * scale),
		(float)(fx->blur_region[2] * scale),
		(float)(fx->blur_region[3] * scale));
	/* 内容贴图里哪一块是可见窗口（归一化）。没留白的元素是 0 / 1 —— layer 底衬、
	 * X11、以及不自己画装饰的客户端都走这一支，和以前逐像素一样。 */
	glUniform4f(d->u_tex_map, fx->tex_off_x, fx->tex_off_y,
		fx->tex_scale_x, fx->tex_scale_y);

	if (fx->blur_only) {
		/* 只要那一层模糊：内容、阴影、边框、圆角、噪点全关（layer surface 的
		 * 底衬用；内容由 wlroots 自己的场景节点画在上面）。 */
		glUniform1f(d->u_radius, 0.0f);
		glUniform1f(d->u_border_width, 0.0f);
		glUniform4f(d->u_border_color, 0.0f, 0.0f, 0.0f, 0.0f);
		glUniform4f(d->u_shadow_color, 0.0f, 0.0f, 0.0f, 0.0f);
		glUniform1f(d->u_noise, 0.0f);
	}

	draw_quad(d->program);
	glUseProgram(0);
	end_pass(egl);
	return true;
}
static bool decor_draw(struct vidow_server *server,
		struct vidow_fx_target *fx, struct wlr_buffer *dst,
		struct wlr_texture *src, struct wlr_texture *mask,
		const float border[4], int ox, int oy,
		double alpha, double scale, const struct vidow_look *look) {
	/* 窗口在输出上的位置（物理像素，输出原点已减掉）：散景贴图是按输出分辨
	 * 率抓的，采样坐标必须落在这块屏自己的坐标系里。被模糊的 layer surface
	 * 那张贴图只盖住它自己那一小块区域（fx->blur_tex_*），坐标就再减掉区域
	 * 原点、除数也换成区域尺寸 —— 不减就是整块错位。
	 *
	 * 那块屏是哪一块，认**跟着这张贴图一起记的** blur_out_*（见 server.h）：
	 * 重烘会落在别的屏正在渲染的那一帧上（output.c 帧循环那趟不看归属），
	 * 现场读 server->output_* 拿到的是那块屏的尺寸/原点 —— 整屏底衬只取中间
	 * 一块铺满整窗，看着就是「静止时底衬啪一下换挡」（中心放大 + 变糊）。 */
	/* **这块窗口在这一块屏上**用的那一份底衬（见 server.h 的 struct
	 * vidow_blur_slot）：窗口骑在两块屏中间时，两块屏各有一格，各自记着自己
	 * 那份贴图的原点 / 区域 / 视野。取错格的后果就是「一半正常、一半横条」。 */
	int out_x = fx->blur_out_w > 0 ? fx->blur_out_x : server->output_x;
	int out_y = fx->blur_out_h > 0 ? fx->blur_out_y : server->output_y;
	int out_w = fx->blur_out_w > 0 ? fx->blur_out_w : server->output_w;
	int out_h = fx->blur_out_h > 0 ? fx->blur_out_h : server->output_h;
	int rg_x = fx->blur_tex_x, rg_y = fx->blur_tex_y;
	int rg_w = fx->blur_tex_w, rg_h = fx->blur_tex_h;
	struct vidow_blur_slot *bs = fx->cur_slot;
	if (bs != NULL) {
		out_x = bs->ox; out_y = bs->oy;
		out_w = bs->ow; out_h = bs->oh;
		rg_x = bs->rx; rg_y = bs->ry;
		rg_w = bs->rw; rg_h = bs->rh;
	}

	/* 尺寸取**这一帧那一格**（跨屏时两块屏各一对，尺寸差一倍）。 */
	int dw2 = fx->cur_slot != NULL ? fx->cur_slot->dec_w : fx->w;
	int dh2 = fx->cur_slot != NULL ? fx->cur_slot->dec_h : fx->h;
	int dcw2 = fx->cur_slot != NULL ? fx->cur_slot->dec_content_w : fx->content_w;
	int dch2 = fx->cur_slot != NULL ? fx->cur_slot->dec_content_h : fx->content_h;
	int dpad2 = fx->cur_slot != NULL ? fx->cur_slot->dec_px_pad : fx->px_pad;
	if (span_trace_on() && bs != NULL) {
		wlr_log(WLR_INFO, "span: out=%s scale=%.2f target=%dx%d content=%dx%d origin=%d,%d viewport=%d,%d %dx%d region=%d,%d %dx%d tex=%p",
			bs->out->name, scale, dw2, dh2, dcw2, dch2, ox, oy,
			out_x, out_y, out_w, out_h, rg_x, rg_y, rg_w, rg_h, (void *)bs->tex);
	}
	struct decor_geo g = {
		.dst_w = dw2, .dst_h = dh2,
		.content_w = dcw2, .content_h = dch2,
		.off_x = (float)dpad2, .off_y = (float)dpad2,
		.origin_x = (float)((int)lround((double)ox * scale) - out_x - rg_x),
		.origin_y = (float)((int)lround((double)oy * scale) - out_y - rg_y),
		.out_size_x = (float)(rg_w > 0 ? rg_w : out_w),
		/* 判据要看**自己那个轴**的 w：写成 blur_tex_w 查 blur_tex_h 的话，
		 * 只要两者不同时为 0 就看不出来，一旦某帧只设了其中一个，
		 * V 方向的除数就成了 0 —— UV 恒定，「全是横条」。 */
		.out_size_y = (float)(rg_h > 0 ? rg_h : out_h),
		.content_alpha = 1.0f,
	};

	/* **跨屏两半各自采自己那块屏的底衬 —— 但不是在这一次渲染里。**
	 *
	 * 这里曾经按「另一块屏的布局左边界」算一个分界，越过它就把 origin /
	 * output_size 换成另一块屏那套。那套只有在**一张装饰图能同时拿到两块屏
	 * 的底衬贴图**时才成立，而装饰着色器一次只绑一张 u_blur —— 换了坐标换
	 * 不了贴图，右半边只是拿这一张贴图按另一块屏的原点 / 尺寸去采，UV 直接
	 * 飞出区域，clamp 到边缘像素（「跨屏那一半是横条 / 整块糊」的来源）。
	 *
	 * 现在跨屏是**每块屏各烘一份装饰缓冲**（view_bake_for_output + 每屏一个
	 * vidow_blur_slot，各自带自己那块屏的私有模糊贴图），每份只画自己那块屏
	 * 那一半（见 vidow_fx_frame 里按 instance->layout_box 的裁剪）。一次渲染
	 * 只认一张底衬，采样坐标也就只需要这一组。
	 *
	 * 坐标口径：
	 *   p          = 目标像素，原点在装饰缓冲左上角
	 *   p.x        = off_x + (窗口内容左边 - ox) * scale   （off_x = px_pad）
	 *   采样       = (origin - off + p * blur_scale + bias) / out_size
	 */

	return decor_pass(server, fx, dst, &g, src, mask, border, alpha, scale, look,
		false);
}

/* 窗口抓帧那条路（wincapture.c）：把「窗口内容」按屏上的样子合成出来 —— 模糊底衬
 * + 圆角 + 边框 + 投影 + 颗粒，和屏幕上**同一个着色器、同一遍**（不是另写一套）。
 *
 * 和屏幕那条路的差别只有几何：
 *   - 渲染目标是**取景框**（这一帧的流），不是窗口的离屏缓冲，内容摆在哪由调用方
 *     给（content_x/y）；
 *   - 底衬采样按「内容左上角的布局坐标 × 取景框缩放」算：取景框的缩放常常和那块
 *     屏的缩放不是一回事（窗口等比缩进流里），所以先按布局坐标换算到那块屏的设备
 *     像素，再乘目标像素/底衬像素那个比值（k）。取景框就是窗口本身时 k = 1，
 *     公式跟屏幕那条路逐项一致。
 *
 * 取景框比窗口大（窗口缩进流、四周留黑）时，装饰只画在窗口那块；外面的留黑由
 * 调用方保证（这一趟不动那儿的像素）。 */
/* 客户端缓冲 → 贴图（借来那张或自己建一张）。窗口抓帧那条路要拿它当「元素
 * 内容」贴图（采样映射 + 透明度剔除），和屏幕上那条路共用这一份。owned 非空时
 * 告诉调用方要不要自己销毁。 */
struct wlr_texture *vidow_fx_client_texture(struct vidow_server *server,
		struct wlr_buffer *buffer, struct wlr_scene_buffer *scene_buffer,
		bool *owned) {
	if (server == NULL || buffer == NULL) {
		return NULL;
	}
	if (scene_buffer != NULL && !client_buffer_wait(server, scene_buffer)) {
		return NULL;
	}
	bool own = true;
	struct wlr_texture *tex = client_texture(server->renderer, buffer, &own);
	if (owned != NULL) {
		*owned = own;
	}
	return tex;
}

bool vidow_fx_capture_decor(struct vidow_server *server,
		const struct vidow_capture_decor *c) {
	if (server == NULL || c == NULL || c->dst == NULL || c->look == NULL ||
			c->dst_w <= 0 || c->dst_h <= 0 || server->decor.program == 0) {
		return false;
	}
	double frame_scale = c->frame_scale > 0.0 ? c->frame_scale : 1.0;
	double out_scale = c->out_scale > 0.0 ? c->out_scale : frame_scale;
	/* 目标像素 / 底衬像素。两者一样时（窗口源那条路就是这样：出图尺寸 =
	 * 窗口逻辑尺寸 × 那块屏的缩放）下面这两行就是屏幕那条路的公式。 */
	double k = frame_scale / out_scale;
	int bw = c->blur_w > 0 ? c->blur_w : c->out_w;
	int bh = c->blur_h > 0 ? c->blur_h : c->out_h;
	if (bw <= 0 || bh <= 0) {
		bw = 1;
		bh = 1;
	}

	/* fx 只用来告诉 decor_pass「底衬是哪张、内容贴图怎么采」：几何全在 g 里。
	 * 模糊只有「底衬那一趟」画（edge 那趟画的是画框，叠在内容上面，再糊一次
	 * 就是错的）。 */
	struct vidow_fx_target fx = {0};
	fx.blur_tex = c->blur;
	fx.blur = !c->edge && c->blur != NULL;
	fx.tex_off_x = c->tex_u0;
	fx.tex_off_y = c->tex_v0;
	fx.tex_scale_x = c->tex_du;
	fx.tex_scale_y = c->tex_dv;

	struct decor_geo g = {
		.dst_w = c->dst_w, .dst_h = c->dst_h,
		.content_w = c->content_w, .content_h = c->content_h,
		.off_x = (float)c->content_x, .off_y = (float)c->content_y,
		.origin_x = (float)(((double)c->content_layout_x - (double)c->out_x) *
			frame_scale - (double)c->blur_x * k),
		.origin_y = (float)(((double)c->content_layout_y - (double)c->out_y) *
			frame_scale - (double)c->blur_y * k),
		.out_size_x = (float)((double)bw * k),
		.out_size_y = (float)((double)bh * k),
		/* 内容不画进这张图（由场景里那个镜像节点画），但 u_texture 照样得
		 * 采 —— 透明度剔除按它的 alpha 走（和屏上同一条判据）。 */
		.content_alpha = 0.0f,
	};

	return decor_pass(server, &fx, c->dst, &g,
		c->content != NULL ? c->content : server->empty_tex, NULL,
		c->border, 1.0, frame_scale, c->look, false);
}

static bool anim_draw(struct vidow_server *server,
		struct vidow_fx_target *fx, struct wlr_buffer *dst,
		struct wlr_texture *src, int ox, int oy, double alpha, double scale) {
	struct vidow_shader *sh = fx->shader;
	struct wlr_gles2_texture_attribs sa;
	wlr_gles2_texture_get_attribs(src, &sa);

	struct wlr_egl *egl = NULL;
	if (begin_pass(server, dst, fx->w, fx->h, &egl) == 0) {
		return false;
	}

	glUseProgram(sh->program);
	bind_tex(0, sa.target, sa.tex);
	glUniform1i(sh->u_texture, 0);
	/* 过冲的曲线（out_elastic / out_back）让 t 稍微越过 1：几何量过冲是我
	 * 们要的，着色器的 u_progress 不是 —— 它的契约是 0..1，> 1 的进度会让
	 * zoom 这类 shader 采到贴图外面去。这里夹住。 */
	double progress = fx->progress;
	if (!(progress > 0.0)) {
		progress = 0.0;
	} else if (progress > 1.0) {
		progress = 1.0;
	}
	glUniform1f(sh->u_progress, (float)progress);
	glUniform1i(sh->u_phase, (int)fx->phase);
	glUniform2f(sh->u_size, (float)fx->w, (float)fx->h);
	glUniform2f(glGetUniformLocation(sh->program, "u_target_size"),
		(float)fx->w, (float)fx->h);
	glUniform2f(sh->u_origin,
		(float)((int)lround((double)ox * scale) - server->output_x),
		(float)((int)lround((double)oy * scale) - server->output_y));
	glUniform2f(sh->u_output_size, (float)server->output_w,
		(float)server->output_h);
	glUniform1f(sh->u_alpha, (float)alpha);
	glUniform1f(sh->u_time, (float)(vidow_now_ns() - fx->start_ns) / 1e9f);
	glUniform1f(sh->u_flip_y, server->config.shader_flip_y ? 1.0f : 0.0f);

	draw_quad(sh->program);
	glUseProgram(0);
	end_pass(egl);
	return true;
}

/* -------------------------------------------------------------- debugging */

/* ------------------------------------------------------------- public API */

bool vidow_fx_set_shader(struct vidow_server *server,
		struct vidow_fx_target *fx, const char *name,
		enum vidow_shader_phase phase) {
	struct vidow_shader *sh = vidow_shader_get(server, name);
	if (sh == NULL) {
		return false;
	}
	fx->shader = sh;
	fx->phase = phase;
	fx->progress = 0.0;
	fx->start_ns = vidow_now_ns();
	return true;
}

/* 装饰画面是一个普通的 scene buffer，而普通 buffer 默认「接受输入」。它盖在
 * 客户端自己的 surface 节点上面，于是会把所有点击都吃掉 —— 窗口就点不着了。
 * 返回 false 让场景的命中测试继续往下走，落到客户端表面上。 */
static bool fx_node_rejects_input(struct wlr_scene_buffer *buffer, double *sx,
		double *sy) {
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

bool vidow_fx_begin(struct vidow_server *server, struct vidow_fx_target *fx,
		struct wlr_scene_tree *parent, const char *shader_name) {
	bool decorated = server->config.decor_enabled && server->decor.program != 0;
	bool animated = shader_name != NULL && shader_name[0] != '\0';
	if (!decorated && !animated) {
		return false;
	}
	if (fx->node == NULL) {
		fx->node = wlr_scene_buffer_create(parent, NULL);
		if (fx->node == NULL) {
			return false;
		}
		fx->node->point_accepts_input = fx_node_rejects_input;
	}
	/* X11 withdraw/reassociate creates a new scene tree while its FX nodes
	 * can still belong to the disabled old tree. Move every output instance
	 * with the owner, not just the legacy node, before hiding the real surface. */
	if (fx->node->node.parent != parent) {
		wlr_scene_node_reparent(&fx->node->node, parent);
		for (struct vidow_blur_slot *sl = fx->blur_slot; sl; sl = sl->next) {
			if (sl->node != NULL) {
				wlr_scene_node_reparent(&sl->node->node, parent);
			}
			sl->dec_valid = false;
		}
	}
	wlr_scene_node_raise_to_top(&fx->node->node);
	wlr_scene_node_set_enabled(&fx->node->node, true);
	fx->active = true;
	fx->blur = server->config.blur_enabled;
	/* 默认恒等：内容贴图就是整块元素。窗口那一路每帧按窗口几何覆盖它。 */
	fx->tex_off_x = 0.0f;
	fx->tex_off_y = 0.0f;
	fx->tex_scale_x = 1.0f;
	fx->tex_scale_y = 1.0f;
	/* mask 默认恒等：没有几何偏移。 */
	fx->mask_off_x = 0.0f;
	fx->mask_off_y = 0.0f;
	fx->mask_scale_x = 1.0f;
	fx->mask_scale_y = 1.0f;
	fx->mask_min = 0.0f;
	fx->anim_scale = 1.0;
	if (animated) {
		vidow_fx_set_shader(server, fx, shader_name, VIDOW_SHADER_OPEN);
	}
	return true;
}

static bool layer_blur_frame(struct vidow_server *server,
		struct vidow_output *output, struct vidow_layer_surface *ls) {
	struct vidow_fx_target *fx = &ls->fx;

	if (ls->scene == NULL) {
		return layer_blur_bail(ls, 1, "没有场景节点");
	}

	/* 恒等映射：layer 的元素就是整块缓冲，没有「窗口几何」那层偏移（窗口那一
	 * 路走 vidow_fx_frame，每帧按可见窗口覆盖这几个字段）。这一路不经过
	 * vidow_fx_begin，字段默认是 0 —— 不设就是拿 texel (0,0) 当整块用。 */
	fx->tex_off_x = 0.0f;
	fx->tex_off_y = 0.0f;
	fx->tex_scale_x = 1.0f;
	fx->tex_scale_y = 1.0f;
	fx->mask_off_x = 0.0f;
	fx->mask_off_y = 0.0f;
	fx->mask_scale_x = 1.0f;
	fx->mask_scale_y = 1.0f;
	fx->mask_min = 0.0f;
	blur_mask_min_changed(fx, server->config.blur_alpha_min);
	/* bottom 层先于窗口画，它底下没有窗口：共享的那张底衬就是它要的全部。top /
	 * overlay 层底下有窗口，得自己抓一小块（见 layer_region_build）。 */
	bool with_windows = ls->layer != ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
	if (with_windows ? output->backdrop_tex == NULL : output->blur_tex == NULL) {
		return layer_blur_bail(ls, 2, with_windows ?
			"这块屏的共享底衬（backdrop_tex）还没建" :
			"这块屏的壁纸底衬（blur_tex）还没建");
	}
	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	int logical_w = ls->geom.width, logical_h = ls->geom.height;
	int px_w = (int)lround((double)logical_w * scale);
	int px_h = (int)lround((double)logical_h * scale);
	if (px_w < 1 || px_h < 1 || px_w > MAX_TARGET_PX || px_h > MAX_TARGET_PX) {
		return layer_blur_bail(ls, 3, "设备像素尺寸越界");
	}

	if (fx->node == NULL) {
		fx->node = wlr_scene_buffer_create(ls->scene->tree, NULL);
		if (fx->node == NULL) {
			return false;
		}
		fx->node->point_accepts_input = fx_node_rejects_input;
		/* 垫在 layer surface 自己那个节点下面。 */
		wlr_scene_node_lower_to_bottom(&fx->node->node);
	}
	fx->blur_only = true;
	fx->active = true;
	/* 着色器里 u_blur_strength 看的是 fx->blur（窗口那条路是 vidow_fx_begin
	 * 设的），这儿得自己开：不开就是垫了一张全透明的东西，什么都看不见。 */
	fx->blur = true;

	if (fx->buf[0] == NULL || fx->w != px_w || fx->h != px_h ||
			fabs(fx->scale - scale) > 1e-3) {
		drop_targets(fx);
		for (int i = 0; i < 2; i++) {
			fx->buf[i] = vidow_alloc_target(server, px_w, px_h);
			if (fx->buf[i] == NULL) {
				drop_targets(fx);
				return layer_blur_bail(ls, 5, "分配离屏目标失败");
			}
		}
		fx->w = px_w;
		fx->h = px_h;
		fx->content_w = px_w;
		fx->content_h = px_h;
		fx->px_pad = 0;
		fx->dst_w = logical_w;
		fx->dst_h = logical_h;
		fx->scale = scale;
		fx->blur_drawn = 0; /* 尺寸变了，必须重画 */
	}

	if (with_windows) {
		/* **区域只取真正要模糊的那一块**（见 layer_region_rect）：以前是整块
		 * geom —— quickshell 给菜单预留的透明画布也一起抓、一起跑金字塔。
		 * rx/ry 是**这块屏**的设备像素（减的是这块屏自己的布局原点，不是
		 * server->output_x/y：后者是最后渲染那块屏留下的全局量）。 */
		int rx = 0, ry = 0, rw_px = px_w, rh_px = px_h, levels = 0;
		if (!layer_region_rect(server, output, ls, &rx, &ry, &rw_px,
				&rh_px, &levels)) {
			return layer_blur_bail(ls, 6, "模糊区域算不出来（整块在屏外 / 太大）");
		}
		/* 缓存键：区域（含输出原点，多屏下会动）、世界代次（窗口重画 / layer
		 * 变动）、级数、还有那两套直接影响结果的参数。 */
		/* 同窗口那条路：世界代次只是粗筛，损伤盒子没碰到这一块就不重抓。
		 *
		 * 碰的是**区**域那块（rx/ry/rw/rh），不是整块 geom：quickshell 的
		 * bar 画布 438 高、真正有内容的只有顶上 38px —— 拿 geom 判的话，
		 * 菜单预留区里有窗口动一下照样整块重抓，区域收窄就白做了。 */
		struct wlr_box region_logical = {
			.x = output->box.x + (int)floor((double)rx / scale),
			.y = output->box.y + (int)floor((double)ry / scale),
			.width = (int)ceil((double)rw_px / scale) + 1,
			.height = (int)ceil((double)rh_px / scale) + 1,
		};
		bool key_changed = ls->bd_gen_scene != server->gen_scene;
		bool touched = key_changed &&
			blur_damage_hits(fx, &region_logical);
		if (getenv("VIDOW_DMG_WHY") != NULL) {
			static int64_t lz_last;
			int64_t lz_now = vidow_now_ns();
			if (lz_now - lz_last > 200000000LL) {
				lz_last = lz_now;
				wlr_log(WLR_INFO, "LAYERBLUR geom=%dx%d%+d%+d px=%dx%d "
					"touched=%d key=%d out=%s", logical_w, logical_h,
					ls->geom.x, ls->geom.y, px_w, px_h, (int)touched,
					(int)key_changed, output->wlr_output->name);
			}
		}
		/* **判据里不再看 gen_layer**（只有损伤盒子算数）。
		 *
		 * 原来这里还要求「抓的时候的 gen_layer == 现在的 gen_layer」，而
		 * gen_layer 是**全局**的：任何一次 layer 变化（上屏 / 下屏 / 壁纸换 /
		 * %background% 重画）都会让它 +1，于是屏幕上**每一块** layer 的私有
		 * 底衬一起作废、一起重抓。
		 *
		 * 代价实测：noctalia 那 8 块 32x32 的角层（noctalia-screen-corner）
		 * 每帧都在重抓 —— BLURVER 探针统计 ~2000 次/秒的 blur_ver 推进，
		 * 渲染统计里「贴图 +12/-0」的纹理反复建删就是它，静止桌面白烧 CPU。
		 *
		 * 而 gen_layer 真正想表达的那些变化**都已经记了损伤**：
		 * vidow_backdrop_damage / vidow_backdrop_damage_output 都调
		 * blur_note_damage，把整屏（或那块屏）的盒子记到每个 layer 头上 ——
		 * 所以 touched 那一条已经覆盖了它们。留下 gen_layer 只是纯churn。
		 *
		 * 换句话说：**判据必须是「我这一块底下的像素变了吗」，不是「世界
		 * 有没有动过」**。全局代次回答的是后一个问题，用它就会把「屏幕另一头
		 * 某个 32x32 角上什么都没变」也判成过期。 */
		/* 区域和缓冲尺寸都是**块感**（按 128 对齐），尺寸跟内容大小有关、跟位置
		 * 无关：移动面板不会让缓冲重建（见 layer_region_rect）。 */
		bool fresh = ls->bd != NULL && fx->blur_own_tex != NULL && !touched &&
			ls->bd_w == rw_px && ls->bd_h == rh_px &&
			ls->bd_x == rx && ls->bd_y == ry &&
			ls->bd_levels == levels &&
			fx->blur_set.w == rw_px && fx->blur_set.h == rh_px &&
			fx->blur_set.levels == levels &&
			fabs(fx->blur_set.offset - server->config.blur_offset) < 1e-6;
		if (!fresh) {
			/* 形状（区域位置尺寸 / 级数）没变：只重算受损的那一小块（见
			 * region_update_rect）。以前 layer 这条路没有局部更新，面板底下
			 * 一条 20px 的进度条在动，整块区域每帧重抓 + 5 趟 kawase。 */
			bool shape_ok = ls->bd != NULL && !touched &&
				ls->bd_w == rw_px && ls->bd_h == rh_px &&
				ls->bd_x == rx && ls->bd_y == ry &&
				ls->bd_levels == levels &&
				fx->blur_set.w == rw_px && fx->blur_set.h == rh_px &&
				fx->blur_set.levels == levels &&
				fabs(fx->blur_set.offset - server->config.blur_offset) < 1e-6;
			struct wlr_box upd;
			bool partial = shape_ok && !blur_no_partial() &&
				region_update_rect(server, fx, output, rx, ry, rw_px, rh_px,
					&upd);
			if (!layer_region_build(server, output, ls, rx, ry, rw_px,
					rh_px, levels, partial ? &upd : NULL)) {
				/* 抓不出来就留着上一帧那张，别闪 */
				return layer_blur_bail(ls, 4, "私有底衬抓不出来");
			}
			/* **底衬重抓了 = 内容变了，必须重画这一版。**
			 *
			 * 别指望下面的 blur_assign() 顺手把 blur_ver 推上去：它只在
			 * 「贴图 / id / 区域」变了时才 ++，而重抓底衬恰恰不换这两样
			 * （同一张 blur_own_tex、同一块区域，只是里面的像素是新的）。
			 * 不在这儿显式 ++ 的话，后面 blur_drawn == blur_ver 立刻成立、
			 * 直接 return true —— 底下窗口 / 壁纸变了，layer 背后的模糊还是
			 * 旧的那一张（用户报的「layer 的 blur 好像很容易不更新」）。
			 *
			 * 窗口那条路没这个问题：view_region_build 走完，blur_assign
			 * 拿到的是新的 blur_own_id（每次重抓都换号），自然会 ++。 */
			fx->blur_ver++;
			blur_ver_hits[5360]++;
		}
		blur_assign(fx, NULL, fx->blur_own_tex, false, fx->blur_own_id,
			&(struct wlr_box){ .x = rx, .y = ry,
				.width = rw_px, .height = rh_px });
	} else {
		/* bottom 层：用共享的那张。`set_layer` 把层改过来的话，之前抓的区域底衬
		 * 就没用了 —— 连着金字塔一起放掉，别攥着几 MB 不放。
		 *
		 * 加 if 守卫：这个分支**每帧**都走，无守卫就是每帧白跑一遍 6 次赋值
		 * （把已经归零的字段再写一遍）。它真正要干的只有「刚从 top/overlay
		 * 切过来」那一次。 */
		if (fx->blur_own_tex != NULL) {
			blur_own_drop(fx);
		}
		/* 缓冲和包着它的贴图要一起收：只 drop buffer 会把 ls->bd_tex
		 * 留成一张悬空贴图（layer 从 top/overlay 切到 bottom 时漏的就是它）。 */
		if (ls->bd != NULL || ls->bd_tex != NULL) {
			layer_bd_drop(ls);
		}
		blur_assign(fx, output->wlr_output, output->blur_tex, false, output->blur_id, NULL);
	}
	/* 采样原点必须记**这块屏自己**的（设备像素）：装饰着色器拿 out_* 和
	 * blur_tex_* 算 UV，而 server->output_* 是「最后渲染的那块屏」留下的全局
	 * 量 —— 两块屏上各有一块 bar 的时候，后渲染的那块会把原点写给先渲染的，
	 * 先渲染那块屏上的模糊就整体错一屏宽。区域收窄之后这里错一下更明显
	 * （会采到区域外面去），所以跟着一起补上。 */
	fx->blur_out_w = output->wlr_output->width;
	fx->blur_out_h = output->wlr_output->height;
	fx->blur_out_x = (int)lround((double)output->box.x * scale);
	fx->blur_out_y = (int)lround((double)output->box.y * scale);
	if (ls->blur_fail != 0) {
		ls->blur_fail = 0; /* 又能画了：下次出问题要重新报 */
	}
	if (fx->blur_drawn == fx->blur_ver) {
		return true; /* 已经画的就是这一版 */
	}

	struct wlr_buffer *target = pick_free(fx->buf);
	if (target == NULL) {
		return false;
	}
	static const float no_border[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	/* 元素自己的内容，给透明度剔除当 mask：模糊只画在 layer surface 真的有内容的
	 * 地方，它自己透明的内边距 / 圆角外不再透出一圈模糊。取的是 surface 当前的
	 * 缓冲；层的 geom 就是 surface 的尺寸（layer.c 的 arrange 就那么算的），所以
	 * uv 跟内容那趟完全一致，不用另算映射。拿不到（还没 commit 过）就让 mask_min
	 * 归零 —— 也就是整块都糊，退回老行为而不是把模糊整块剔没。
	 *
	 * 代价：这是 surface 的**主**缓冲，不含子表面 —— 一个用子表面画内容的层，那
	 * 部分会被剔掉（少见；要精确就得像 niri 那样把整棵子树离屏渲一张 alpha 纹理）。 */
	struct wlr_texture *mask = NULL;
	struct wlr_client_buffer *cbuf = ls->layer_surface->surface->buffer;
	if (cbuf != NULL) {
		mask = wlr_texture_from_buffer(server->renderer,
			render_source(&cbuf->base));
	}
	/* VIDOW_MASK_DUMP=N：把前 N 份 mask（就是下面递给着色器的这张 layer 自己的
	 * 内容）dump 成裸数据，用来量「元素自己透明的地方到底剔没剔」。 */
	static int mask_dumped = 0;
	static int mask_limit = -1;
	if (mask_limit < 0) {
		const char *env = getenv("VIDOW_MASK_DUMP");
		mask_limit = env != NULL ? atoi(env) : 0;
	}
	if (mask != NULL && mask_dumped < mask_limit) {
		dump_buffer(server, &cbuf->base, px_w, px_h, "vidow-mask",
			mask_dumped);
		mask_dumped++;
	}
	struct vidow_look ls_look;
	vidow_rule_look(&server->config, &ls->rule_eff, &ls_look);
	decor_draw(server, fx, target, server->empty_tex, mask, no_border,
		ls->geom.x, ls->geom.y, 1.0, scale, &ls_look);
	if (mask != NULL) {
		wlr_texture_destroy(mask);
	}

	wlr_scene_node_set_position(&fx->node->node, 0, 0);
	wlr_scene_buffer_set_dest_size(fx->node, logical_w, logical_h);
	/* 同上：layer surface 也会被抓帧遮挡压着。 */
	wlr_scene_buffer_set_opacity(fx->node, fx->masked ? 0.0f : 1.0f);
	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, logical_w, logical_h);
	wlr_scene_buffer_set_buffer_with_damage(fx->node, target, &damage);
	pixman_region32_fini(&damage);
	wlr_scene_node_set_enabled(&fx->node->node, true);

	fx->blur_drawn = fx->blur_ver;
	return true;
}

/* ------------------------------------------------------- 弹出物的背景模糊 */

/* 窗口的 xdg_popup / X11 菜单提示 / 输入法候选框背后的模糊。
 *
 * 抄的是上面 layer_blur_frame 的做法：在弹出物自己的场景树里垫一个只画模糊的
 * 节点（blur_only），底衬按它自己那一小块区域重抓 —— 基座从共享底衬上 1:1
 * 拷一块，窗口 / 别的弹出物再画上去。区别只有几何来源：
 *   - xdg / X11 的节点挂在弹出物自己的树上，相对坐标恒等 0,0；
 *   - 输入法候选框挂在 layer_im 上，位置每帧按绝对坐标摆。
 * 抓底衬时把弹出物自己藏起来（pb->hide），否则模糊里会浮出它自己的影子。 */

static bool popup_blur_geom(struct vidow_popup_blur *pb, int *x, int *y,
		int *w, int *h) {
	if (pb->tree == NULL || pb->hide == NULL || pb->surface == NULL) {
		return false;
	}
	if (!wlr_scene_node_coords(pb->hide, x, y)) {
		return false;
	}
	if (pb->view != NULL) {
		struct wlr_box g = vidow_view_geometry(pb->view);
		*w = g.width;
		*h = g.height;
	} else {
		/* GTK 的 xdg geometry 不一定等于看得见的窗口体（CSD 阴影 / 主题
		 * 不同都会让 6,5 这种偏移对不上）。输入区域是客户端自己声明的
		 * 「可点的窗口体」，GTK 菜单实测就是 6,5 157x203，比 geometry 更
		 * 可信；拿它当模糊范围，客户端换主题也不会偏。 */
		pixman_box32_t *ir = pixman_region32_extents(
			&pb->surface->input_region);
		if (ir != NULL && ir->x2 > ir->x1 && ir->y2 > ir->y1) {
			*x += ir->x1;
			*y += ir->y1;
			*w = ir->x2 - ir->x1;
			*h = ir->y2 - ir->y1;
		} else {
			*w = pb->surface->current.width;
			*h = pb->surface->current.height;
		}
	}
	if (*w <= 0 || *h <= 0) {
		/* 还没 commit 过 / 几何还没算出来：退回表面尺寸。 */
		*w = pb->surface->current.width;
		*h = pb->surface->current.height;
	}
	return *w > 0 && *h > 0;
}

/* 这块弹出物现在归哪块屏管（中心点落在谁身上）。NULL = 整块在屏外。 */
static struct vidow_output *popup_blur_owner(struct vidow_popup_blur *pb) {
	int x, y, w, h;
	if (!popup_blur_geom(pb, &x, &y, &w, &h)) {
		return NULL;
	}
	struct wlr_output *wo = wlr_output_layout_output_at(
		pb->server->output_layout, x + w / 2, y + h / 2);
	if (wo == NULL) {
		return NULL;
	}
	return vidow_output_lookup(pb->server, wo);
}

/* 内容整块不透明（格式里没 alpha）时，底下的模糊一个像素都透不出来 —— 跟
 * layer_blur_visible 同一个判据，只是弹出物没有规则 / 动画要额外看。 */
static bool popup_blur_visible(struct vidow_popup_blur *pb) {
	if (pb->view != NULL &&
			(pb->view->mask_opacity || pb->view->anim_alpha < 1.0)) {
		return true;
	}
	if (pb->surface->buffer == NULL) {
		return true; /* 让 wanted 去报「还没缓冲」 */
	}
	return !wlr_buffer_is_opaque(&pb->surface->buffer->base);
}

static bool popup_blur_bail(struct vidow_popup_blur *pb, int reason,
		const char *what) {
	if (pb->blur_fail != reason) {
		pb->blur_fail = reason;
		wlr_log(WLR_DEBUG, "弹出物模糊没画: %s（%s）", what,
			pb->tag != NULL ? pb->tag : "?");
	}
	return false;
}

static bool popup_blur_wanted(struct vidow_server *server,
		struct vidow_popup_blur *pb) {
	if (!server->shader_ok || !server->config.blur_enabled) {
		return popup_blur_bail(pb, 10, "全局模糊关了 / 着色器没就绪");
	}
	if (!server->config.popup_blur_enabled) {
		return popup_blur_bail(pb, 12, "popup_blur_enabled = false");
	}
	if (pb->tree == NULL || pb->hide == NULL || pb->surface == NULL) {
		return popup_blur_bail(pb, 1, "场景 / 表面已经没了");
	}
	if (!pb->hide->enabled) {
		return popup_blur_bail(pb, 2,
			"弹出物节点关着（没映射 / 在 overview 里 / 工作区藏起来了）");
	}
	if (pb->surface->buffer == NULL) {
		return popup_blur_bail(pb, 3, "表面还没有缓冲");
	}
	if (!popup_blur_visible(pb)) {
		return popup_blur_bail(pb, 4, "整块不透明（缓冲没有 alpha）");
	}
	return true;
}

/* 抓底衬时把弹出物自己藏起来。只藏「本来开着」的，回填时也别把本来就该藏着
 * 的东西点亮（跟 layer_blur_capture_visible 一个规矩）。 */
static void popup_blur_hide(struct vidow_popup_blur *pb, bool hide) {
	if (hide) {
		if (pb->hide != NULL && pb->hide->enabled) {
			wlr_scene_node_set_enabled(pb->hide, false);
			pb->capture_hidden = true;
		}
	} else if (pb->capture_hidden) {
		pb->capture_hidden = false;
		if (pb->hide != NULL) {
			wlr_scene_node_set_enabled(pb->hide, true);
		}
	}
}

/* 这块弹出物该用多大一块区域抓底衬（设备像素，相对输出原点）。跟
 * view_region_rect 同一套：往外放一圈模糊半径、原点对齐网格、尺寸只跟弹出物
 * 大小有关（移动时缓冲不重建），再夹在屏内。 */
static bool popup_region_rect(struct vidow_server *server,
		struct vidow_output *output, int x, int y, int w, int h,
		int *rx, int *ry, int *rw, int *rh, int *levels) {
	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	int lv = blur_levels_for(server, server->output_w, server->output_h);
	int margin = blur_reach_px(server, lv);
	int left = (int)lround((double)x * scale) - server->output_x - margin;
	int top = (int)lround((double)y * scale) - server->output_y - margin;
	int span_w = (int)lround((double)w * scale) + 2 * margin;
	int span_h = (int)lround((double)h * scale) + 2 * margin;
	const int grid = VIDOW_BLUR_REGION_GRID;
	int gx1 = (int)floor((double)left / grid) * grid;
	int gy1 = (int)floor((double)top / grid) * grid;
	int size_w = (int)ceil((double)(span_w + grid) / grid) * grid;
	int size_h = (int)ceil((double)(span_h + grid) / grid) * grid;
	if (size_w < 1 || size_h < 1 || size_w > MAX_TARGET_PX ||
			size_h > MAX_TARGET_PX) {
		return false;
	}
	int max_x = server->output_w - size_w;
	int max_y = server->output_h - size_h;
	if (max_x < 0) {
		max_x = 0;
	}
	if (max_y < 0) {
		max_y = 0;
	}
	if (gx1 < 0) {
		gx1 = 0;
	} else if (gx1 > max_x) {
		gx1 = max_x;
	}
	if (gy1 < 0) {
		gy1 = 0;
	} else if (gy1 > max_y) {
		gy1 = max_y;
	}
	*rx = gx1;
	*ry = gy1;
	*rw = size_w;
	*rh = size_h;
	*levels = lv;
	return true;
}

/* 抓「弹出物盖住的那一块」：基座从共享底衬上 1:1 拷一块（壁纸 + background /
 * bottom 层），再把窗口 / 别的弹出物 / top / overlay 层画上去。跟
 * layer_region_build 的区别只有一处：抓的时候藏起来的是弹出物自己。 */
static bool popup_region_build(struct vidow_server *server,
		struct vidow_output *output, struct vidow_popup_blur *pb,
		int rx, int ry, int rw, int rh, int levels) {
	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	int x1 = rx < 0 ? 0 : rx, y1 = ry < 0 ? 0 : ry;
	int x2 = rx + rw, y2 = ry + rh;
	if (x2 > server->output_w) {
		x2 = server->output_w;
	}
	if (y2 > server->output_h) {
		y2 = server->output_h;
	}
	if (x2 <= x1 || y2 <= y1) {
		return false; /* 整块在屏外，没东西可抓 */
	}
	if (output->backdrop_tex == NULL) {
		return false;
	}

	if (pb->fx.own_bd == NULL || pb->fx.own_bd_w != rw ||
			pb->fx.own_bd_h != rh) {
		own_bd_drop(&pb->fx);
		pb->fx.own_bd = vidow_alloc_target(server, rw, rh);
		if (pb->fx.own_bd == NULL) {
			return false;
		}
		pb->fx.own_bd_tex = stat_tex_new(
			wlr_texture_from_buffer(server->renderer, pb->fx.own_bd));
		if (pb->fx.own_bd_tex == NULL) {
			own_bd_drop(&pb->fx);
			return false;
		}
		pb->fx.own_bd_w = rw;
		pb->fx.own_bd_h = rh;
	}

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, pb->fx.own_bd, NULL);
	if (pass == NULL) {
		return false;
	}
	struct wlr_render_rect_options clear = {
		.box = { .x = 0, .y = 0, .width = rw, .height = rh },
		.color = { .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f },
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	};
	wlr_render_pass_add_rect(pass, &clear);

	/* 基座：共享底衬里对应的那一块，1:1 捞过来（别插值）。 */
	float one = 1.0f;
	struct wlr_render_texture_options base = {
		.texture = output->backdrop_tex,
		.src_box = {
			.x = (float)x1, .y = (float)y1,
			.width = (float)(x2 - x1), .height = (float)(y2 - y1),
		},
		.dst_box = {
			.x = x1 - rx, .y = y1 - ry,
			.width = x2 - x1, .height = y2 - y1,
		},
		.alpha = &one,
		.transform = WL_OUTPUT_TRANSFORM_NORMAL,
		.filter_mode = WLR_SCALE_FILTER_NEAREST,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	};
	wlr_render_pass_add_texture(pass, &base);

	/* 弹出物自己不能进自己的底衬。 */
	popup_blur_hide(pb, true);
	struct backdrop_ctx ctx = {
		.pass = pass,
		.server = server,
		.scale = scale,
		.ox = (int)lround((double)output->box.x * scale) + x1,
		.oy = (int)lround((double)output->box.y * scale) + y1,
		.clip_x = x1 - rx,
		.clip_y = y1 - ry,
		.clip_w = x2 - x1,
		.clip_h = y2 - y1,
	};
	/* 只抓**弹出物底下**的那几层。弹出物可能挂在窗口那层（layer_popups），
	 * 也可能挂在某块 layer surface 的树里（状态栏菜单，跟着 layer_top /
	 * overlay 走）；挂在 bottom 层的弹出物底下就是共享底衬，一层都不用补抓。
	 * 多抓上面的层会把「其实盖在弹出物上面」的东西画进它的底衬 —— 半透明的
	 * 上面那层一透，就能看见本不该在背后的内容。 */
	bool under_bottom = false, under_top = false, under_overlay = false;
	bool under_im = false;
	struct wlr_scene_node *branch = NULL;
	for (struct wlr_scene_node *n = &pb->tree->node; n != NULL;
			n = n->parent != NULL ? &n->parent->node : NULL) {
		if (n == &server->layer_bottom->node ||
				n == &server->layer_background->node) {
			under_bottom = true;
			break;
		}
		if (n == &server->layer_top->node) {
			under_top = true;
		}
		if (n == &server->layer_overlay->node) {
			under_overlay = true;
		}
		if (n == &server->layer_im->node) {
			under_im = true;
		}
		branch = n;
	}
	struct wlr_scene_tree *trees[4];
	size_t ntrees = 0;
	if (under_bottom) {
		/* bottom / background 层的弹出物：共享底衬里那块 layer surface 可能
		 * 因为「它自己也要模糊」被藏起来了（见 layer_blur_capture_visible），
		 * 所以只补抓它这一支（父表面 + 弹出物，弹出物已经藏着）。 */
		struct wlr_scene_tree *bt =
			branch != NULL ? wlr_scene_tree_from_node(branch) : NULL;
		if (bt != NULL) {
			trees[ntrees++] = bt;
		}
	} else {
		trees[ntrees++] = server->layer_toplevels;
		trees[ntrees++] = server->layer_popups;
		if (under_top || under_overlay || under_im) {
			trees[ntrees++] = server->layer_top;
		}
		if (under_overlay || under_im) {
			trees[ntrees++] = server->layer_overlay;
		}
	}
	for (size_t i = 0; i < ntrees; i++) {
		if (trees[i] != NULL) {
			wlr_scene_node_for_each_buffer(&trees[i]->node,
				backdrop_iter, &ctx);
		}
	}
	popup_blur_hide(pb, false);
	if (!wlr_render_pass_submit(pass)) {
		return false;
	}

	dmg_clear(&pb->fx.pending_damage); /* 这一版的账已经算清了 */
	/* 旧贴图裹着的缓冲马上要被金字塔复用。 */
	blur_own_drop(&pb->fx);
	if (!blur_set_alloc(server, &pb->fx.blur_set, rw, rh, levels)) {
		return false;
	}
	blur_chain(server, &pb->fx.blur_set, pb->fx.own_bd_tex, rw, rh);
	pb->fx.blur_own_tex = pb->fx.blur_set.up_tex[0];
	if (pb->fx.blur_own_tex == NULL) {
		return false;
	}
	pb->fx.blur_set.valid = true;
	pb->fx.blur_own_id = server->blur_serial++;
	pb->bd_x = rx;
	pb->bd_y = ry;
	pb->bd_w = rw;
	pb->bd_h = rh;
	pb->bd_output = output;
	pb->bd_gen_scene = server->gen_scene;
	pb->bd_gen_layer = server->gen_layer;
	wlr_log(WLR_DEBUG, "blur: popup region %s %dx%d at %d,%d levels=%d",
		pb->tag != NULL ? pb->tag : "?", rw, rh, rx, ry, levels);
	return true;
}

/* 把模糊资源还掉，但保留注册项和场景节点（下一帧还要用）。 */
static void popup_blur_release(struct vidow_popup_blur *pb) {
	if (pb == NULL) {
		return;
	}
	/* 上一次抓底衬要是半路退出（正常路径都会回填），这里兜底把弹出物放出来。 */
	popup_blur_hide(pb, false);
	if (pb->fx.node != NULL) {
		/* 只在真的有东西的时候碰节点：这个函数每帧都会被叫（弹出物整块
		 * 不透明 / 还没缓冲时），反复 set_buffer(NULL) 会一直报损伤。 */
		if (pb->fx.node->node.enabled) {
			wlr_scene_node_set_enabled(&pb->fx.node->node, false);
		}
		if (pb->fx.node->buffer != NULL) {
			wlr_scene_buffer_set_buffer(pb->fx.node, NULL);
		}
	}
	blur_target_release(&pb->fx);
	drop_targets(&pb->fx);
	pb->fx.blur_drawn = 0;
	pb->bd_w = 0;
	pb->bd_h = 0;
	pb->bd_x = 0;
	pb->bd_y = 0;
	pb->bd_output = NULL;
	pb->bd_gen_scene = 0;
	pb->bd_gen_layer = 0;
}

static void popup_blur_frame(struct vidow_server *server,
		struct vidow_output *output, struct vidow_popup_blur *pb) {
	struct vidow_fx_target *fx = &pb->fx;

	if (!popup_blur_wanted(server, pb)) {
		if (fx->node != NULL && fx->node->node.enabled) {
			wlr_scene_node_set_enabled(&fx->node->node, false);
		}
		popup_blur_release(pb);
		return;
	}

	int x, y, w, h;
	if (!popup_blur_geom(pb, &x, &y, &w, &h)) {
		popup_blur_bail(pb, 5, "几何还是 0 / 节点不在场景里");
		if (fx->node != NULL && fx->node->node.enabled) {
			wlr_scene_node_set_enabled(&fx->node->node, false);
		}
		return;
	}
	/* 归别的屏那一趟管：别在这儿建 / 拆缓冲（两块屏会互相把对方的金字塔丢掉）。 */
	struct vidow_output *owner = popup_blur_owner(pb);
	if (owner != output) {
		if (owner == NULL) {
			if (fx->node != NULL && fx->node->node.enabled) {
				wlr_scene_node_set_enabled(&fx->node->node, false);
			}
			popup_blur_release(pb);
		}
		return;
	}
	if (output->backdrop_tex == NULL) {
		popup_blur_bail(pb, 6, "这块屏的共享底衬还没建");
		return;
	}

	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	int px_w = (int)lround((double)w * scale);
	int px_h = (int)lround((double)h * scale);
	if (px_w < 1 || px_h < 1 || px_w > MAX_TARGET_PX ||
			px_h > MAX_TARGET_PX) {
		popup_blur_bail(pb, 7, "设备像素尺寸越界");
		return;
	}

	if (fx->node == NULL) {
		fx->node = wlr_scene_buffer_create(pb->tree, NULL);
		if (fx->node == NULL) {
			return;
		}
		fx->node->point_accepts_input = fx_node_rejects_input;
		/* 垫在弹出物表面（和它的子菜单）下面。 */
		wlr_scene_node_lower_to_bottom(&fx->node->node);
		pb->node_destroy.notify = popup_blur_node_destroy;
		wl_signal_add(&fx->node->node.events.destroy, &pb->node_destroy);
		pb->node_destroy_added = true;
	}
	fx->blur_only = true;
	fx->active = true;
	fx->blur = true;
	fx->tex_off_x = 0.0f;
	fx->tex_off_y = 0.0f;
	fx->tex_scale_x = 1.0f;
	fx->tex_scale_y = 1.0f;
	/* mask 默认整张表面；xdg popup 用输入区域（窗口体）当 mask 范围，
	 * 把 CSD 阴影剔掉。阈值 0.35 是给半透明菜单体留的余量（GTK 菜单体
	 * alpha ≈ 0.45），比全局 0.15 更不容易把阴影/圆角过渡糊进去。 */
	fx->mask_off_x = 0.0f;
	fx->mask_off_y = 0.0f;
	fx->mask_scale_x = 1.0f;
	fx->mask_scale_y = 1.0f;
	fx->mask_min = 1.0f / 255.0f;
	/* 配置里给了统一阈值就用它（0 = 保持各自动的默认值：普通菜单只剔
	 * alpha=0，输入法候选框 0.35）。 */
	if (server->config.popup_blur_alpha_min > 0.0) {
		fx->mask_min = (float)server->config.popup_blur_alpha_min;
		if (fx->mask_min > 1.0f) {
			fx->mask_min = 1.0f;
		}
	}
	if (pb->view == NULL) {
		int sw = pb->surface->current.width;
		int sh = pb->surface->current.height;
		pixman_box32_t *ir = pixman_region32_extents(
			&pb->surface->input_region);
		if (sw > 0 && sh > 0 && ir != NULL && ir->x2 > ir->x1 &&
				ir->y2 > ir->y1) {
			fx->mask_off_x = (float)ir->x1 / (float)sw;
			fx->mask_off_y = (float)ir->y1 / (float)sh;
			fx->mask_scale_x = (float)(ir->x2 - ir->x1) / (float)sw;
			fx->mask_scale_y = (float)(ir->y2 - ir->y1) / (float)sh;
			if (server->config.popup_blur_alpha_min <= 0.0) {
				fx->mask_min = 0.35f;
			}
		}
	}
	blur_mask_min_changed(fx, fx->mask_min);

	/* 节点相对 tree 的坐标：xdg / X11 的树原点就是弹出物原点（0,0）；候选框
	 * 挂在 layer_im 上，得按绝对坐标摆。 */
	int tx = 0, ty = 0;
	if (wlr_scene_node_coords(&pb->tree->node, &tx, &ty)) {
		wlr_scene_node_set_position(&fx->node->node, x - tx, y - ty);
	}

	if (fx->buf[0] == NULL || fx->w != px_w || fx->h != px_h ||
			fabs(fx->scale - scale) > 1e-3) {
		drop_targets(fx);
		for (int i = 0; i < 2; i++) {
			fx->buf[i] = vidow_alloc_target(server, px_w, px_h);
			if (fx->buf[i] == NULL) {
				drop_targets(fx);
				popup_blur_bail(pb, 8, "分配离屏目标失败");
				return;
			}
		}
		fx->w = px_w;
		fx->h = px_h;
		fx->content_w = px_w;
		fx->content_h = px_h;
		fx->px_pad = 0;
		fx->dst_w = w;
		fx->dst_h = h;
		fx->scale = scale;
		fx->blur_drawn = 0;
	}

	/* 位置 / 尺寸变了：模糊的采样坐标跟着变，节点也得重画。 */
	if (pb->last_x != x || pb->last_y != y || pb->last_w != w ||
			pb->last_h != h) {
		pb->last_x = x;
		pb->last_y = y;
		pb->last_w = w;
		pb->last_h = h;
		fx->blur_ver++;
		blur_ver_hits[5954]++;
	}

	int rx, ry, rw, rh, levels;
	if (!popup_region_rect(server, output, x, y, w, h,
			&rx, &ry, &rw, &rh, &levels)) {
		popup_blur_bail(pb, 9, "区域尺寸越界");
		return;
	}

	struct wlr_box region_logical = {
		.x = output->box.x + (int)floor((double)rx / scale),
		.y = output->box.y + (int)floor((double)ry / scale),
		.width = (int)ceil((double)rw / scale) + 1,
		.height = (int)ceil((double)rh / scale) + 1,
	};
	bool key_changed = pb->bd_gen_scene != server->gen_scene;
	bool touched = key_changed && blur_damage_hits(fx, &region_logical);
	/* 同 layer_blur_frame 上面那段：**判据里不看 gen_layer**。
	 * 弹出物（菜单 / 输入法候选框）一样会因为「世界动过」而重抓整块底衬，
	 * 而它真正该看的是「我底下那一块像素变没变」= touched。 */
	bool fresh = fx->own_bd != NULL && fx->blur_own_tex != NULL && !touched &&
		pb->bd_output == output &&
		pb->bd_w == rw && pb->bd_h == rh && pb->bd_x == rx &&
		pb->bd_y == ry &&
		fx->blur_set.w == rw && fx->blur_set.h == rh &&
		fx->blur_set.levels == levels &&
		fabs(fx->blur_set.offset - server->config.blur_offset) < 1e-6;
	if (!fresh) {
		if (!popup_region_build(server, output, pb, rx, ry, rw, rh,
				levels)) {
			(void)popup_blur_bail(pb, 11, "私有底衬抓不出来");
			return; /* 抓不出来就留着上一帧那张，别闪 */
		}
		/* 底衬重抓了 = 内容变了，必须重画这一版。同 layer 那条路：
		 * popup_region_build 不换 blur_own_tex / blur_own_id，blur_assign
		 * 逐字段比对全部命中就早退、不推进 blur_ver，下面
		 * blur_drawn == blur_ver 立刻成立 —— 弹出物背后的模糊会一直停在
		 * 第一次画的那张上。 */
		fx->blur_ver++;
		blur_ver_hits[5990]++;
	}
	blur_assign(fx, NULL, fx->blur_own_tex, false, fx->blur_own_id,
		&(struct wlr_box){ .x = rx, .y = ry,
			.width = rw, .height = rh });
	fx->blur_out_w = output->wlr_output->width;
	fx->blur_out_h = output->wlr_output->height;
	fx->blur_out_x = server->output_x;
	fx->blur_out_y = server->output_y;
	if (pb->blur_fail != 0) {
		pb->blur_fail = 0;
	}

	if (fx->blur_drawn == fx->blur_ver) {
		/* 透明度每帧都对齐：抓帧遮挡（mask_opacity）可能在两帧之间变，不能
		 * 因为「模糊图没变」就把一块不透明的模糊留在截屏里。 */
		wlr_scene_buffer_set_opacity(fx->node,
			(pb->view != NULL && pb->view->mask_opacity) ? 0.0f : 1.0f);
		wlr_scene_node_set_enabled(&fx->node->node, true);
		return; /* 已经画的就是这一版 */
	}

	struct wlr_buffer *target = pick_free(fx->buf);
	if (target == NULL) {
		return;
	}
	/* 元素自己的 alpha 当剔除 mask：模糊只画在弹出物真有内容的地方（圆角
	 * 外面 / 自带的透明阴影边不再透出一圈模糊）。 */
	struct wlr_texture *mask = NULL;
	bool mask_owned = false;
	struct wlr_client_buffer *cbuf = pb->surface->buffer;
	if (cbuf != NULL) {
		mask = client_texture(server->renderer, &cbuf->base, &mask_owned);
	}
	/* Diagnostics only: keep the existing draw path, and bound unexpected
	 * texture reports across all popups so a bad buffer cannot flood logs. */
	static unsigned int mask_target_reports = 0;
	if (mask != NULL && mask_target_reports < 8) {
		if (!wlr_texture_is_gles2(mask)) {
			mask_target_reports++;
			wlr_log(WLR_ERROR, "popup mask: %s is not GLES2 (%u/8)",
				pb->tag, mask_target_reports);
		} else {
			struct wlr_gles2_texture_attribs attrs;
			wlr_gles2_texture_get_attribs(mask, &attrs);
			if (attrs.target != GL_TEXTURE_2D) {
				mask_target_reports++;
				wlr_log(WLR_ERROR,
					"popup mask: %s target=0x%x, expected GL_TEXTURE_2D (%u/8)",
					pb->tag, (unsigned int)attrs.target, mask_target_reports);
			}
		}
	}
	static const float no_border[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	struct vidow_look look = {0};
	look.opacity = 1.0;
	look.blur = true;
	look.decor = false;
	decor_draw(server, fx, target, server->empty_tex, mask, no_border,
		x, y, 1.0, scale, &look);
	if (mask_owned && mask != NULL) {
		wlr_texture_destroy(mask);
	}

	wlr_scene_buffer_set_dest_size(fx->node, w, h);
	wlr_scene_buffer_set_opacity(fx->node,
		(pb->view != NULL && pb->view->mask_opacity) ? 0.0f : 1.0f);
	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, w, h);
	wlr_scene_buffer_set_buffer_with_damage(fx->node, target, &damage);
	pixman_region32_fini(&damage);
	wlr_scene_node_set_enabled(&fx->node->node, true);
	fx->blur_drawn = fx->blur_ver;
}

/* 节点跟着场景树一起没了（xdg popup 的树 / X11 的表面树销毁）：把注册项摘掉，
 * 别再拿着一个已经释放的指针。 */
static void popup_blur_node_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_popup_blur *pb =
		wl_container_of(listener, pb, node_destroy);
	pb->node_destroy_added = false;
	wl_list_remove(&pb->node_destroy.link);
	pb->fx.node = NULL;
	vidow_popup_blur_unregister(pb, false);
}

/* hide 那个节点没了。**必挂**（见 server.h 里 hide_destroy 的注释）：
 * 它是弹出物的场景节点，弹出物一关就先没，而 pb 可能还活着。
 *
 * 处理方式跟 node_destroy 一样 —— 整个 pb 摘掉，不要再试图用它：hide 没了就
 * 意味着弹出物本体没了，模糊也就没有意义了（留着只会继续拿野指针去算几何）。
 * 注意 pb->fx.node 是**模糊节点**，跟 hide 是两个东西，交给 unregister 去清。 */
static void popup_blur_hide_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_popup_blur *pb =
		wl_container_of(listener, pb, hide_destroy);
	pb->hide_destroy_added = false;
	wl_list_remove(&pb->hide_destroy.link);
	/* 先自己清指针：unregister 里不会再碰它。 */
	pb->hide = NULL;
	vidow_popup_blur_unregister(pb, false);
}

static void popup_blur_owner_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_popup_blur *pb =
		wl_container_of(listener, pb, owner_destroy);
	pb->owner_destroy_added = false;
	wl_list_remove(&pb->owner_destroy.link);
	vidow_popup_blur_unregister(pb, false);
}

static void popup_blur_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_popup_blur *pb = wl_container_of(listener, pb, commit);
	vidow_popup_blur_invalidate(pb);
}

struct vidow_popup_blur *vidow_popup_blur_register(struct vidow_server *server,
		struct wlr_scene_tree *tree, struct wlr_scene_node *hide,
		struct wlr_surface *surface, struct vidow_view *view, const char *tag,
		struct wl_signal *owner_destroy) {
	if (server == NULL || tree == NULL || hide == NULL || surface == NULL) {
		return NULL;
	}
	struct vidow_popup_blur *pb = calloc(1, sizeof(*pb));
	if (pb == NULL) {
		return NULL;
	}
	pb->server = server;
	pb->tree = tree;
	pb->hide = hide;
	pb->surface = surface;
	pb->view = view;
	pb->tag = tag != NULL ? tag : "?";
	pb->last_w = -1; /* 第一帧一定重画 */
	pb->last_h = -1;
	wl_list_insert(&server->popup_blurs, &pb->link);
	if (owner_destroy != NULL) {
		pb->owner_destroy.notify = popup_blur_owner_destroy;
		wl_signal_add(owner_destroy, &pb->owner_destroy);
		pb->owner_destroy_added = true;
	}
	/* hide 节点的销毁必须盯住：它是弹出物的场景节点，弹出物关掉时先没，
	 * 而 pb 可能还活着（宿主是另一个对象）。不盯就是野指针 —— 见
	 * server.h 里 hide_destroy 的注释和 2026-10-21 那次 kate 崩溃。 */
	pb->hide_destroy.notify = popup_blur_hide_destroy;
	wl_signal_add(&hide->events.destroy, &pb->hide_destroy);
	pb->hide_destroy_added = true;

	pb->commit.notify = popup_blur_commit;
	wl_signal_add(&surface->events.commit, &pb->commit);
	pb->commit_added = true;
	wlr_log(WLR_DEBUG, "popup blur: 登记 %s", pb->tag);
	return pb;
}

void vidow_popup_blur_unregister(struct vidow_popup_blur *pb,
		bool destroy_node) {
	if (pb == NULL) {
		return;
	}
	/* 先把宿主手里的指针摘掉：节点 / 表面先没的时候，宿主之后还会拿它来用。 */
	if (pb->view != NULL && pb->view->popup_blur == pb) {
		pb->view->popup_blur = NULL;
	}
	if (pb->server != NULL && pb->server->im_popup.blur == pb) {
		pb->server->im_popup.blur = NULL;
	}
	if (pb->commit_added) {
		wl_list_remove(&pb->commit.link);
		pb->commit_added = false;
	}
	if (pb->owner_destroy_added) {
		wl_list_remove(&pb->owner_destroy.link);
		pb->owner_destroy_added = false;
	}
	if (pb->hide_destroy_added) {
		wl_list_remove(&pb->hide_destroy.link);
		pb->hide_destroy_added = false;
	}
	if (pb->node_destroy_added) {
		wl_list_remove(&pb->node_destroy.link);
		pb->node_destroy_added = false;
	}
	if (pb->fx.node != NULL) {
		if (destroy_node) {
			wlr_scene_node_destroy(&pb->fx.node->node);
		} else {
			/* 节点归场景树（xdg popup）：只放资源，别碰它 —— 树可能正跟着
			 * 表面一起拆。 */
			pb->fx.node = NULL;
		}
	}
	vidow_fx_release(&pb->fx);
	wl_list_remove(&pb->link);
	free(pb);
}

void vidow_popup_blur_invalidate(struct vidow_popup_blur *pb) {
	if (pb == NULL) {
		return;
	}
	pb->fx.blur_ver++;
	blur_ver_hits[6194]++;
	struct vidow_output *out = popup_blur_owner(pb);
	if (out != NULL) {
		vidow_output_schedule_frame(pb->server, out->wlr_output,
			"弹出物模糊失效");
	}
}

/* overview 里那张糊掉的壁纸。
 *
 * 跟 bottom 层 layer surface 的底衬是同一套（共享底衬 output->blur_tex + blur_only
 * 那次装饰渲染），区别只有两点：它摊满整块屏，而且画在 layer_overview_dim 那棵树
 * 里（壁纸之上、窗口之下）—— 全景里缩小的窗口底下就是一整片糊壁纸，niri 那个味道。
 *
 * 拿不到共享底衬（没开模糊 / 这屏还没有底衬）就什么都不画，退回「只压暗」：遮罩
 * 还在，不至于变成一块没压过的壁纸。 */
static void vidow_overview_blur_frame(struct vidow_server *server,
		struct vidow_output *output) {
	struct vidow_overview *g = &server->overview;
	/* **每块屏各有一份底衬**（以前是单实例、只画进 overview 时聚焦的那块屏，
	 * 别的屏上一片空白）。 */
	struct vidow_ov_screen *ov = &output->ov;
	struct vidow_fx_target *fx = &ov->fx;

	/* 每块屏画自己那份，而且得真在展开（t > 0）。 */
	bool want = g->t > 0.002 &&
		server->config.overview_blur && server->config.blur_enabled;
	if (!want || output->blur_tex == NULL) {
		if (fx->node != NULL) {
			wlr_scene_node_set_enabled(&fx->node->node, false);
		}
		ov->fx_on = false;
		return;
	}

	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
	if (box.width < 1 || box.height < 1) {
		ov->fx_on = false;
		return;
	}
	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	int logical_w = box.width, logical_h = box.height;
	int px_w = (int)lround((double)logical_w * scale);
	int px_h = (int)lround((double)logical_h * scale);
	if (px_w < 1 || px_h < 1 || px_w > MAX_TARGET_PX || px_h > MAX_TARGET_PX) {
		ov->fx_on = false;
		return;
	}

	if (fx->node == NULL) {
		fx->node = wlr_scene_buffer_create(server->layer_overview_dim, NULL);
		if (fx->node == NULL) {
			ov->fx_on = false;
			return;
		}
		fx->node->point_accepts_input = fx_node_rejects_input;
		/* 垫在压暗遮罩下面（遮罩比它先建）。 */
		wlr_scene_node_lower_to_bottom(&fx->node->node);
	}
	fx->blur_only = true;
	fx->active = true;
	fx->blur = true;
	/* 恒等映射：这块元素就是整块屏，没有「窗口几何」那层偏移（跟 layer 底衬
	 * 一样，这一路不经过 vidow_fx_begin，字段得自己填）。 */
	fx->tex_off_x = 0.0f;
	fx->tex_off_y = 0.0f;
	fx->tex_scale_x = 1.0f;
	fx->tex_scale_y = 1.0f;

	if (fx->buf[0] == NULL || fx->w != px_w || fx->h != px_h ||
			fabs(fx->scale - scale) > 1e-3) {
		drop_targets(fx);
		for (int i = 0; i < 2; i++) {
			fx->buf[i] = vidow_alloc_target(server, px_w, px_h);
			if (fx->buf[i] == NULL) {
				drop_targets(fx);
				ov->fx_on = false;
				return;
			}
		}
		fx->w = px_w;
		fx->h = px_h;
		fx->content_w = px_w;
		fx->content_h = px_h;
		fx->px_pad = 0;
		fx->dst_w = logical_w;
		fx->dst_h = logical_h;
		fx->scale = scale;
		fx->blur_drawn = 0; /* 尺寸变了，必须重画 */
	}
	/* region = NULL：这张贴图盖的就是整块屏，着色器按「元素在输出上的位置」
	 * 采样（u_origin = ox*scale - 输出原点），跟 bottom 层那条路一样。 */
	blur_assign(fx, output->wlr_output, output->blur_tex, false, output->blur_id, NULL);
	if (fx->blur_drawn == fx->blur_ver) {
		ov->fx_on = true; /* 已经画的就是这一版 */
		wlr_scene_node_set_enabled(&fx->node->node, true);
		return;
	}

	struct wlr_buffer *target = pick_free(fx->buf);
	if (target == NULL) {
		return; /* 两个目标都还在飞：留着上一帧那张，别闪 */
	}
	static const float no_border[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	struct vidow_look look = {0};
	look.opacity = 1.0;
	look.blur = true;
	look.decor = false;
	/* mask = NULL：整块屏都要糊，不按元素 alpha 剔（blur_only 且 mask == NULL
	 * 时着色器自己把阈值归零，见 decor_draw）。 */
	decor_draw(server, fx, target, server->empty_tex, NULL, no_border,
		box.x, box.y, 1.0, scale, &look);
	wlr_scene_node_set_position(&fx->node->node, box.x, box.y);
	wlr_scene_buffer_set_dest_size(fx->node, logical_w, logical_h);
	wlr_scene_buffer_set_opacity(fx->node, fx->masked ? 0.0f : 1.0f);
	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, logical_w, logical_h);
	wlr_scene_buffer_set_buffer_with_damage(fx->node, target, &damage);
	pixman_region32_fini(&damage);
	wlr_scene_node_set_enabled(&fx->node->node, true);
	fx->blur_drawn = fx->blur_ver;
	ov->fx_on = true;
}

static void layer_blur_capture_visible(struct vidow_server *server, bool on) {
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		if (ls->fx.node != NULL) {
			wlr_scene_node_set_enabled(&ls->fx.node->node, on);
		}		/* 光藏模糊节点不够：**会被模糊的那个 surface 本身**也不能进底衬。
		 * bottom 层的面板（dock 这类）自己也在抓的树里，它要的那份底衬如果含它
		 * 自己，模糊里就会浮出一个自己的影子（半透明的地方透出「自己的糊版」）。
		 * 藏之前记一笔，免得回填的时候把本来就该藏着的东西点亮。 */
		if (!on) {
			if (ls->scene != NULL && layer_blur_wanted(server, ls) &&
					ls->scene->tree->node.enabled) {
				wlr_scene_node_set_enabled(&ls->scene->tree->node, false);
				ls->capture_hidden = true;
			}
		} else if (ls->capture_hidden) {
			ls->capture_hidden = false;
			wlr_scene_node_set_enabled(&ls->scene->tree->node, true);
		}
	}
}

/* 位置 / 尺寸变了（或者底衬换了）：作废已画的那一版，下一帧重画。
 * 距离和采样坐标都在着色器里，节点自己动了也得重算。 */
void vidow_layer_blur_invalidate(struct vidow_layer_surface *ls) {
	ls->fx.blur_ver++;
	blur_ver_hits[6347]++;
	if (ls->output != NULL) {
		vidow_output_schedule_frame(ls->server, ls->output->wlr_output,
			"layer 模糊失效");
	}
}

void vidow_layer_blur_release(struct vidow_layer_surface *ls) {
	if (ls->fx.node == NULL && ls->fx.buf[0] == NULL &&
			ls->bd == NULL && ls->bd_tex == NULL) {
		return;
	}
	if (ls->fx.node != NULL) {
		/* 这行是给 test-layerdestroy.sh 数数用的：layer 没了，垫在它下面的
		 * 模糊节点得当场收掉（不收就是每次面板重建漏一张整块大小的缓冲）。 */
		wlr_log(WLR_DEBUG, "layer blur release %dx%d",
			ls->geom.width, ls->geom.height);
		wlr_scene_node_destroy(&ls->fx.node->node);
		ls->fx.node = NULL;
	}
	vidow_fx_release(&ls->fx);
	ls->fx.blur_drawn = 0;
	ls->fx.blur_tex_w = 0;
	ls->fx.blur_tex_h = 0;
	/* 抓来的那块区域底衬也得跟着走，否则 layer 没了还占着一张整块大小的缓冲。 */
	layer_bd_drop(ls);
}

static void closing_update(void *data, double t, double linear) {
	struct vidow_closing *closing = data;
	struct vidow_config *cfg = &closing->server->config;

	/* 透明度走**线性进度**，不走曲线：过冲的曲线（out_elastic 在 t≈0.26 就
	 * 到 1、t≈0.45 冲到 1.09）喂给「1 → close_alpha_to」这条斜坡，透明度在动画
	 * 头 1/4 就掉到 0 并夹住 —— 剩下的 3/4 只在动位移和缩放，屏幕上什么都没有，
	 * 看起来就是「没有关闭动画」（默认配置 close_easing = out_elastic、140ms，
	 * 实际可见时间只有 35ms）。位移和缩放照旧用曲线，q 弹还在。 */
	closing->alpha = 1.0 + (cfg->close_alpha_to - 1.0) * linear;
	if (closing->alpha < 0.0) {
		closing->alpha = 0.0;
	} else if (closing->alpha > 1.0) {
		closing->alpha = 1.0;
	}
	/* 规则给的 opacity 在这条路上也得算：装饰着色器那条路自己会乘（decor_draw
	 * 的 u_opacity），没走着色器的（layer 的快照、关掉装饰的窗口）就得在这儿乘
	 * —— 不然「规则说这个窗口半透明」，它的关闭动画却是全不透明地淡出。 */
	if (!closing->fx.active) {
		closing->alpha *= vidow_rule_opacity(&closing->rule_eff);
	}
	/* 回弹的曲线让 t 稍微越过 1：位移和缩放跟着过冲（那才是回弹）。 */
	closing->off_y = (double)closing->slide_px * t;
	closing->fx.anim_scale = 1.0 + (closing->anim_scale_to - 1.0) * t;
	closing->fx.progress = t;
	/* 变的就是这张快照（滑下去 + 缩放），别整屏都算损伤 —— 关窗动画每帧都在动，
	 * 整屏都算的话别人的底衬会跟着每帧重抓。 */
	{
		struct wlr_box box = {
			.x = closing->x, .y = closing->y,
			.width = closing->w, .height = closing->h,
		};
		vidow_backdrop_damage_region(closing->server, &box);
	}

	int y = closing->y + (int)lround(closing->off_y);
	if (closing->fx.active) {
		vidow_fx_frame(closing->server, &closing->fx, closing->node,
			&closing->geo, closing->x, y, closing->x, y, closing->alpha,
			false, closing->fullscreen, closing->scale, 1.0,
			&closing->rule_eff);
		return;
	}
	if (closing->node != NULL) {
		/* 没画装饰：快照自己上屏。它是一整块源缓冲，可见窗口在里面偏着 ——
		 * 偏移不进位置，CSD 窗口的关闭动画就整个斜出那圈留白。 */
		wlr_scene_node_set_position(&closing->node->node,
			closing->x - closing->geo.off_x, y - closing->geo.off_y);
		wlr_scene_buffer_set_opacity(closing->node, clamp01(closing->alpha));
	}
}

static void closing_done(void *data, bool cancelled) {
	(void)cancelled;
	struct vidow_closing *closing = data;
	struct vidow_server *server = closing->server;

	vidow_fx_end(server, &closing->fx);
	if (closing->fx.node != NULL) {
		wlr_scene_node_destroy(&closing->fx.node->node);
		closing->fx.node = NULL;
	}
	if (closing->node != NULL) {
		wlr_scene_node_destroy(&closing->node->node);
	}
	if (closing->buffer != NULL) {
		wlr_buffer_unlock(closing->buffer);
	}
	wl_list_remove(&closing->link);
	free(closing);
}

void vidow_closing_start(struct vidow_server *server,
		struct wlr_scene_tree *parent, struct wlr_buffer *snapshot,
		const struct vidow_fx_geo *geo, int x, int y, int w, int h,
		int duration_ms, int slide_px, bool decorate, const char *shader_name,
		double scale, struct vidow_ease easing, double anim_scale_to,
		const struct vidow_rule_eff *eff, bool fullscreen) {
	if (snapshot == NULL || duration_ms <= 0 || w <= 0 || h <= 0) {
		return;
	}

	struct vidow_closing *closing = calloc(1, sizeof(*closing));
	if (closing == NULL) {
		return;
	}
	closing->server = server;
	closing->buffer = wlr_buffer_lock(snapshot);
	closing->x = x;
	closing->y = y;
	closing->w = w;
	closing->h = h;
	closing->alpha = 1.0;
	closing->slide_px = slide_px;
	closing->anim_scale_to = anim_scale_to > 0.0 ? anim_scale_to : 1.0;
	closing->easing = easing;
	closing->scale = scale > 0.0 ? scale : 1.0;
	if (eff != NULL) {
		closing->rule_eff = *eff;
	}
	closing->fullscreen = fullscreen;
	closing->geo = (struct vidow_fx_geo){0};
	if (geo != NULL) {
		closing->geo = *geo;
	}
	if (closing->geo.w <= 0 || closing->geo.h <= 0) {
		/* 没给几何（layer surface 之类）：整块快照就是窗口。 */
		closing->geo.w = w;
		closing->geo.h = h;
		closing->geo.src_w = w;
		closing->geo.src_h = h;
	}
	wl_list_insert(&server->closing, &closing->link);

	closing->node = wlr_scene_buffer_create(parent, closing->buffer);
	if (closing->node == NULL) {
		wl_list_remove(&closing->link);
		wlr_buffer_unlock(closing->buffer);
		free(closing);
		return;
	}
	wlr_scene_node_set_position(&closing->node->node,
		x - closing->geo.off_x, y - closing->geo.off_y);
	wlr_scene_buffer_set_dest_size(closing->node, w, h);

	/* Decorate the outgoing window too, but never blur behind it: the scene it
	 * would sample is already changing underneath.
	 *
	 * layer surface 的快照不装饰（decorate = false）：装饰是**窗口**才有的东西
	 * （vidow 给窗口画边框 / 阴影 / 圆角），layer 自己就是内容、形状由客户端画。
	 * 给面板的快照套上窗口装饰，淡出的尾巴上就剩一圈「比面板大一点的空心矩形」
	 * —— 面板自己那点半透明在模糊底衬上本来就没什么对比，先看不见了；边框和阴影
	 * 压在没糊过的壁纸上反而更显眼（阴影还往面板外扩 shadow_radius 那圈）。开
	 * 动画那边本来就没装饰，这样两边也一致。 */
	if (decorate && vidow_fx_begin(server, &closing->fx, parent, shader_name)) {
		closing->fx.blur = false;
		if (shader_name != NULL && shader_name[0] != '\0') {
			closing->fx.phase = VIDOW_SHADER_CLOSE;
		}
		wlr_scene_node_set_enabled(&closing->node->node, false);
	}

	closing->anim = vidow_anim_start(server, "close", duration_ms, easing,
		closing_update, closing_done, closing);
	if (closing->anim == NULL) {
		closing_done(closing, true);
	}
}
