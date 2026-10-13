#include "server.h"

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
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

/* wlroots 0.20 exports this symbol but the Arch package ships no prototype. */
extern const struct wlr_drm_format_set *wlr_renderer_get_render_formats(
	struct wlr_renderer *r, uint32_t buffer_caps);

#define MAX_SHADER_SIZE (256 * 1024)
#define MAX_TARGET_PX 8192

/* ---- 渲染统计（VIDOW_RENDER_STATS=1，见 output.c）：每帧各阶段耗时 + 重建次数。
 * 计数是机器无关的硬指标 —— 「有背景时拖拽卡」这类问题，先在真机上跑
 * `VIDOW_RENDER_STATS=1 vidow` 看每帧干了多少活，再谈优化。
 *
 * tex_new/tex_del 数的是 vidow **自己那些目标缓冲**的贴图导入/销毁：真后端上
 * （GBM/dmabuf）每次都是一次 EGLImage 创建 + BO 映射，每帧十几次就够呛。客户端
 * 缓冲走的是 wlr_client_buffer 自己那张，不算在内。 */
struct vidow_frame_stats vidow_stats;
static struct wlr_texture *stat_tex_new(struct wlr_texture *t) {
	if (t != NULL) {
		vidow_stats.tex_new++;
	}
	return t;
}
static void stat_tex_del(struct wlr_texture *t) {
	if (t != NULL) {
		vidow_stats.tex_del++;
	}
	wlr_texture_destroy(t);
}
/* ------------------------------------------------------------ scene alpha */

static void alpha_iter(struct wlr_scene_buffer *buffer, int sx, int sy,
		void *data) {
	(void)sx;
	(void)sy;
	double alpha = *(double *)data;
	wlr_scene_buffer_set_opacity(buffer, (float)alpha);
}

/* Multiply every buffer of a subtree by `alpha`. Scene-graph opacity is a
 * compositor-side property, so this never disturbs what clients draw. */
void vidow_fx_set_alpha(struct wlr_scene_node *node, double alpha) {
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
	"uniform float u_pad;\n"
	"uniform float u_radius;\n"
	"uniform float u_border_width;\n"
	"uniform vec4 u_border_color;\n" /* premultiplied */
	"uniform vec4 u_shadow_color;\n" /* premultiplied */
	"uniform vec2 u_shadow_offset;\n"
	"uniform float u_shadow_radius;\n"
	"uniform float u_opacity;\n"
	"uniform float u_noise;\n"
	"uniform float u_alpha;\n"
	"uniform float u_time;\n"
	"uniform vec2 u_origin;\n"
	"uniform vec2 u_output_size;\n"
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
	"varying vec2 v_uv;\n"
	"\n"
	"float sd_round_box(vec2 p, vec2 half_size, float r) {\n"
	"	vec2 q = abs(p) - half_size + r;\n"
	"	return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
	"}\n"
	"\n"
	"void main() {\n"
	"	vec2 p = v_uv * u_target_size;\n"
	"	vec2 wp = p - u_pad;\n"
	"	vec2 half_size = u_size * 0.5;\n"
	"	float r = min(u_radius, min(half_size.x, half_size.y));\n"
	"	float d = sd_round_box(wp - half_size, half_size, r);\n"
	"	float inside = 1.0 - smoothstep(-1.0, 1.0, d);\n"
	"\n"
	"	vec2 cuv = clamp(wp / u_size, 0.0, 1.0);\n"
	"	vec2 uv = clamp(u_tex_map.xy + cuv * u_tex_map.zw, 0.0, 1.0);\n"
	"	vec4 content = texture2D(u_texture, uv) * u_opacity * inside;\n"
	"\n"
	"	float bw = min(u_border_width, min(half_size.x, half_size.y));\n"
	"	float ring = inside - (1.0 - smoothstep(-1.0, 1.0, d + bw));\n"
	"	vec4 border = u_border_color * u_opacity * ring;\n"
	"\n"
	"	float sa = 1.0 - smoothstep(-1.0, u_shadow_radius,\n"
	"		sd_round_box(wp - u_shadow_offset - half_size, half_size, r));\n"
	/* Only outside the window: otherwise the shadow shows through a
	 * translucent window instead of staying behind it. */
	"	vec4 shadow = u_shadow_color * sa * (1.0 - inside);\n"
	"\n"
	"	vec2 o = (u_origin - u_pad + p) / u_output_size;\n"
	"	vec4 blurred = texture2D(u_blur, clamp(o, 0.0, 1.0));\n"
	/* 元素自己的 alpha（「透明度剔除」）：模糊只画在元素真的有内容的地方 —— 透明
	 * 内边距、圆角外面、弹窗自带的半透明阴影边都不再透出一圈模糊。窗口 / 弹窗那条
	 * 路内容贴图就是元素自己（u_texture）；layer surface 的模糊底衬不画内容，元素
	 * 内容由 u_mask 传进来。用的是**内容自己的 alpha**，不含 u_opacity：剔除管的是
	 * 形状，半透明窗口该透出来的模糊不能因为 window_opacity 低就整个没了。 */
	"	float elem_a = u_mask_on > 0.5 ?\n"
	/* mask 是元素**自己**那张缓冲，采样坐标用元素局部（恒等映射）：窗口那层
	 * CSD 留白偏移只属于内容贴图，不能带到 mask 上，否则剔除会看错地方。 */
	"		texture2D(u_mask, cuv).a : texture2D(u_texture, uv).a;\n"
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
	LOC(u_pad, "u_pad");
	LOC(u_radius, "u_radius");
	LOC(u_border_width, "u_border_width");
	LOC(u_border_color, "u_border_color");
	LOC(u_shadow_color, "u_shadow_color");
	LOC(u_shadow_offset, "u_shadow_offset");
	LOC(u_shadow_radius, "u_shadow_radius");
	LOC(u_opacity, "u_opacity");
	LOC(u_noise, "u_noise");
	LOC(u_alpha, "u_alpha");
	LOC(u_origin, "u_origin");
	LOC(u_output_size, "u_output_size");
	LOC(u_flip_y, "u_flip_y");
	LOC(u_blur_strength, "u_blur_strength");
	LOC(u_blur_region, "u_blur_region");
	LOC(u_blur_region_on, "u_blur_region_on");
	LOC(u_tex_map, "u_tex_map");
	LOC(u_mask, "u_mask");
	LOC(u_mask_on, "u_mask_on");
	LOC(u_mask_min, "u_mask_min");
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
					wlr_log(WLR_DEBUG, "fx: target %dx%d format 0x%08x",
						w, h, set->formats[i].format);
					return buf;
				}
			}
		}
	}
	return NULL;
}

static void drop_targets(struct vidow_fx_target *fx) {
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

/* content_w/h 是逻辑单位；缓冲按 scale 倍分配，所以 HiDPI 屏上装饰和窗口内容
 * 都是按设备像素画的（不是先画小再放大）。pad 也要跟着乘，否则边框会内缩。 */
static bool ensure_targets(struct vidow_server *server,
		struct vidow_fx_target *fx, int content_w, int content_h, double scale) {
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	int pad = vidow_fx_pad(server);
	int w = (int)lround((double)content_w * scale) +
		2 * (int)lround((double)pad * scale);
	int h = (int)lround((double)content_h * scale) +
		2 * (int)lround((double)pad * scale);
	if (w < 1) {
		w = 1;
	}
	if (h < 1) {
		h = 1;
	}
	if (fx->buf[0] != NULL && fx->w == w && fx->h == h &&
			fabs(fx->scale - scale) < 1e-3) {
		return true;
	}
	drop_targets(fx);
	for (int i = 0; i < 2; i++) {
		fx->buf[i] = vidow_alloc_target(server, w, h);
		if (fx->buf[i] == NULL) {
			wlr_log(WLR_ERROR, "fx: cannot allocate a %dx%d render target", w, h);
			drop_targets(fx);
			return false;
		}
	}
	fx->w = w;
	fx->h = h;
	fx->content_w = (int)lround((double)content_w * scale);
	fx->content_h = (int)lround((double)content_h * scale);
	fx->px_pad = (int)lround((double)pad * scale);
	/* 场景节点尺寸用逻辑单位：渲染目标除以倍率，正好是 1:1 落到设备像素上。 */
	fx->dst_w = (int)lround((double)w / scale);
	fx->dst_h = (int)lround((double)h / scale);
	fx->scale = scale;
	return true;
}

/* A buffer the scene is still displaying has n_locks > 0: never draw into it. */
static struct wlr_buffer *pick_free(struct wlr_buffer *bufs[2]) {
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

/* VIDOW_FX_DUMP=N writes the first N rendered targets to /tmp so the effect
 * can be eyeballed without a screenshot protocol. VIDOW_BLUR_DUMP does the
 * same for the backdrop and the blurred result. */
static void dump_buffer(struct vidow_server *server, struct wlr_buffer *buffer,
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
static int blur_dump_limit(void) {
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
static void blur_box_add(struct wlr_box *dst, const struct wlr_box *src) {
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
static struct wlr_box dmg_bounds(const struct vidow_dmg_set *set) {
	struct wlr_box b = {0};
	for (int i = 0; i < set->n; i++) {
		blur_box_add(&b, &set->box[i]);
	}
	return b;
}

static void dmg_clear(struct vidow_dmg_set *set) {
	set->n = 0;
}

/* 把「刚变了的这块区域」（布局逻辑坐标）记到每个需要私有底衬的元素头上；skip 那
 * 个元素自己不记（它自己的变化不该让它自己的底衬过期 —— 抓的时候它是藏着的）。
 * 记进来的盒子一直攒着，直到那个元素真的重抓一次才清掉：这样「损伤在屏幕另一头」
 * 攒再多也不会让它重抓，而一旦有一笔碰到它那一块，攒下来的这笔账仍然算数。 */
static void blur_note_damage(struct vidow_server *server,
		const struct wlr_box *box, const struct vidow_view *skip) {
	if (box == NULL || box->width <= 0 || box->height <= 0) {
		return;
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
}

void vidow_backdrop_damage_box(struct vidow_server *server,
		const struct wlr_box *box) {
	blur_note_damage(server, box, NULL);
}

/* 私有底衬要不要因为这次损伤重抓：损伤盒子空 = 没损伤（不用）；否则看它跟
 * 「这张底衬管得着的那块区域」（布局逻辑坐标）碰不碰得到。区域本身已经把模糊
 * 半径算进去了（见 view_region_rect），所以碰到区域就是碰到能影响它的内容。 */
static bool blur_damage_hits(const struct vidow_fx_target *fx,
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
static void damage_bump(struct vidow_server *server, bool layers) {
	server->gen_scene++;
	if (layers) {
		server->gen_layer++;
	}
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

static void blur_drop(struct vidow_output *output) {
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

static void blur_set_drop(struct vidow_blur_set *set) {
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

static bool blur_set_alloc(struct vidow_server *server,
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
static void blur_chain_rect(struct vidow_server *server,
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

static void blur_chain(struct vidow_server *server, struct vidow_blur_set *set,
		struct wlr_texture *backdrop_tex, int w, int h) {
	blur_chain_rect(server, set, backdrop_tex, w, h, NULL, 0);
}

/* Render the layer trees below the toplevels into the backdrop buffer. Doing
 * this by hand instead of via wlr_scene_output_build_state() keeps the output's
 * damage ring untouched and lets us pick exactly which trees contribute. */
struct backdrop_ctx {
	struct wlr_render_pass *pass;
	struct vidow_server *server;
	/* 场景图里的一切（节点坐标、dst 尺寸）都是**逻辑像素**，底衬缓冲却是设备
	 * 像素 —— 少了这个倍率，1.7 倍屏上底衬只在左上角 1/1.7 的一小块里画了内容
	 * （缩小的），其余全空：模糊采到的是空像素，越糊越透明，layer 背后的底衬
	 * 也跟着错位。原点同理：多屏时得减掉这块屏自己的布局原点。 */
	double scale;
	int ox, oy; /* 这块屏在布局里的原点，换算成设备像素 */
	/* 只要这块目标区域（设备像素，目标缓冲的坐标系）。抓整屏时是 0（不管），抓
	 * layer 背后那一小块时，落在外面的 buffer 连贴图都不用建。 */
	int clip_x, clip_y, clip_w, clip_h;
};

/* 局部更新要求「只画一块」这件事对每个要抓的 buffer 都成立：转过的客户端缓冲
 * （wl_surface.set_buffer_transform）得把 dst 子矩形映射回转置过的 src 框，不值得
 * 为它写一套。碰到就整块重跑。 */
static void backdrop_clip_check(struct wlr_scene_buffer *sb, int sx, int sy,
		void *data) {
	(void)sx;
	(void)sy;
	bool *ok = data;
	if (sb->buffer != NULL && sb->transform != WL_OUTPUT_TRANSFORM_NORMAL) {
		*ok = false;
	}
}

static void backdrop_iter(struct wlr_scene_buffer *sb, int sx, int sy,
		void *data) {
	struct backdrop_ctx *ctx = data;
	if (sb->buffer == NULL) {
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
	struct wlr_texture *tex = client_texture(ctx->server->renderer, sb->buffer,
		&tex_owned);
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

static bool backdrop_render(struct vidow_server *server,
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
	if (vidow_overview_active(view->server)) {
		return false;
	}
	if (view->rule_eff.set[VRA_BLUR] ||
			view->server->config.blur_all ||
			view->float_mode != VIDOW_FLOAT_NONE) {
		return true;
	}
	/* 客户端自己开口要的（ext-background-effect），blur_all 关着也得给 */
	return view->fx.blur_region_on;
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
	if (view->anim_alpha < 1.0) {
		return true;
	}
	struct vidow_look look;
	vidow_view_look(&view->server->config, &view->rule_eff, view->fullscreen,
		&look);
	/* 整个窗口被画淡了（window_opacity）：模糊从里面透出来。 */
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
static bool view_wants_blur(struct vidow_view *view) {
	return view->fx.active && view_wants_blur_ever(view) &&
		view_blur_visible(view);
}

/* VIDOW_NO_SCANOUT=1：一律走合成那条路（探针，见 vidow_view_scanout_ok）。 */
static bool scanout_off(void) {
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
	/* 裁过（跨屏那一半 / overview 里缩放）：直通没法裁，画面会画到隔壁屏上。 */
	if (view->fx.clip_on) {
		return false;
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
	return view_content_opaque(view, &geo);
}

/* Hide/show a toplevel for a backdrop capture. A decorated window is drawn
 * from its fx node and its real surface already sits at zero opacity, so
 * hiding the fx node is enough; an undecorated one draws from the surface. */
static void view_set_capture_visible(struct vidow_view *view, bool on) {
	if (view->fx.active && view->fx.node != NULL) {
		wlr_scene_node_set_enabled(&view->fx.node->node, on);
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
static bool view_overlaps_other(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	struct wlr_box a = vidow_view_screen_box(view);
	struct vidow_view *other;
	wl_list_for_each(other, &server->views, link) {
		if (other == view || !other->mapped || !other->placed ||
				other->output != view->output) {
			continue;
		}
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
static void blur_assign(struct vidow_fx_target *fx, struct wlr_texture *tex,
		bool owned, uint64_t id, const struct wlr_box *region) {
	int rx = region != NULL ? region->x : 0;
	int ry = region != NULL ? region->y : 0;
	int rw = region != NULL ? region->width : 0;
	int rh = region != NULL ? region->height : 0;
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
}

/* 剔除阈值（blur_alpha_min）只进装饰着色器，不进模糊金字塔：它变了得让帧循环知道
 * 「已经烘进那张缓冲里的那一版」过期了，否则运行时改配置（重载 / set-config）看着
 * 就是「没反应」（这条坑的底衬版本在 README 的 ⓪ 里）。 */
static void blur_mask_min_changed(struct vidow_fx_target *fx, double alpha_min) {
	if (fabs((double)fx->mask_min_used - alpha_min) > 1e-6) {
		fx->mask_min_used = (float)alpha_min;
		fx->blur_ver++;
	}
}

/* 私有底衬那块区域缓冲 + 包着它的贴图一起收（跟 layer 的 layer_bd_drop 一个
 * 套路）。金字塔那几块缓冲归 blur_set，这里只放区域底衬。 */
static void own_bd_drop(struct vidow_fx_target *fx) {
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
static void blur_own_drop(struct vidow_fx_target *fx) {
	blur_assign(fx, NULL, false, 0, NULL);
	fx->blur_own_tex = NULL; /* 贴图归 blur_set，上面那句只放指针 */
	fx->blur_own_id = 0;
}

/* Drop every blur resource of a target. blur_assign() only frees what the
 * target owns, so the server's shared wallpaper texture survives. */
static void blur_target_release(struct vidow_fx_target *fx) {
	blur_own_drop(fx);
	blur_set_drop(&fx->blur_set);
	own_bd_drop(fx);
}

/* layer surface 背后的模糊（实现在下面「draw」之后）：先声明，抓底衬的时候
 * 要用它把节点藏起来。 */
/* layer 那块区域底衬（缓冲 + 包着它的贴图）一起收。贴图是包的缓冲、不是包的像素，
 * 复用缓冲的时候贴图也跟着复用，只有尺寸变了才重建 —— 链条要拿它当第一趟的源，
 * 以前每趟导入一次、用完销毁，真后端上每帧就是十几次 EGLImage + BO 映射。 */
static void layer_bd_drop(struct vidow_layer_surface *ls) {
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

static bool layer_blur_wanted(struct vidow_server *server,
		struct vidow_layer_surface *ls);
/* 抓底衬时要不要把这个 layer surface 自己藏起来（见下面的定义）。 */
static bool layer_blur_frame(struct vidow_server *server,
		struct vidow_output *output, struct vidow_layer_surface *ls);
static void layer_blur_capture_visible(struct vidow_server *server, bool on);
/* overview 里那张糊掉的壁纸（整屏，垫在压暗遮罩下面）。 */
static void vidow_overview_blur_frame(struct vidow_server *server,
		struct vidow_output *output);

static int blur_levels_for(struct vidow_server *server, int w, int h) {
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
	output->blur_gen = server->gen_layer;
	output->blur_levels = levels;
	output->blur_offset = server->config.blur_offset;
	wlr_log(WLR_DEBUG, "blur: shared rebuild %dx%d levels=%d offset=%.2f", w, h,
		levels, output->blur_offset);
	return true;
}

/* 私有底衬那块区域按这个格对齐（设备像素）：窗口在格子里挪动时区域一个字都不
 * 变，缓存才真的能命中 —— 否则拖窗口时区域每帧跟着平移，每帧都得重抓。 */
#define VIDOW_BLUR_REGION_GRID 128

/* 链条采样到底能摸到多远（原始像素）：降采样每一趟的抽样半径是 blur_offset 个
 * **源** texel，源每降一级尺寸减半，换算回原图每级翻倍（offset * (2^levels - 1)）；
 * 往上那几趟同理，只是从第 2 级起（offset * (2^(levels+1) - 4)）。两者相加就是
 * offset * (3 * 2^levels - 5)。区域要在窗口外面留出这么一圈，不然窗口边缘附近的
 * 模糊值是被区域边界 clamp 出来的（看着就是边上发暗）。 */
static int blur_reach_px(struct vidow_server *server, int levels) {
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
	int lv = blur_levels_for(server, server->output_w, server->output_h);
	int margin = blur_reach_px(server, lv);
	/* 装饰矩形 = 可见窗口 ± pad（逻辑单位），再往外放一圈模糊半径。 */
	int left = (int)lround((double)(box.x - pad) * scale) - server->output_x - margin;
	int top = (int)lround((double)(box.y - pad) * scale) - server->output_y - margin;
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
	 * 那圈就还是透明的（layer 那一路一直是这个行为）。 */
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

/* 「只重跑受损那一小块」的开关：默认开。设了 VIDOW_BLUR_NO_PARTIAL 就整块重跑；
 * VIDOW_BLUR_NO_PARTIAL_FILE 指的文件一出现也整块重跑（跑着跑着切过去，用来比对
 * 「同一个场景、同一个客户端、同一个进程」下局部和整块的输出 —— 见
 * test-blurpartial.sh）。都只给 A/B 比对用。
 *
 * 两个 getenv 各只问一次（每次重抓都要问一遍：一帧里四五个元素重抓就是十来次；
 * getenv 在 profile 里比我们自己的模糊代码还显眼）。**文件存在性每次都看** ——
 * test-blurpartial.sh 就是跑着跑着把文件建出来切过去的。 */
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
static bool region_update_rect(struct vidow_output *output,
		struct vidow_view *view,
		int rx, int ry, int rw, int rh, struct wlr_box *out) {
	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	const struct vidow_dmg_set *set = &view->fx.pending_damage;
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
	wlr_scene_node_for_each_buffer(&view->server->layer_toplevels->node,
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
	/* 这一版是被哪一块损伤触发的（只给下面那行日志用：pending_damage 这里就要
	 * 清掉了）。修「客户端只动了一小块，整片模糊却重抓」时，看的就是这个数。 */
	struct wlr_box dmg = dmg_bounds(&fx->pending_damage);
	if (fx->own_bd == NULL || fx->own_bd_w != rw || fx->own_bd_h != rh) {
		own_bd_drop(fx);
		fx->own_bd = vidow_alloc_target(server, rw, rh);
		if (fx->own_bd == NULL) {
			return false;
		}
		fx->own_bd_tex = stat_tex_new(
			wlr_texture_from_buffer(server->renderer, fx->own_bd));
		if (fx->own_bd_tex == NULL) {
			own_bd_drop(fx);
			return false;
		}
		fx->own_bd_w = rw;
		fx->own_bd_h = rh;
	}

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, fx->own_bd, NULL);
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
	};
	wlr_scene_node_for_each_buffer(&server->layer_toplevels->node, backdrop_iter,
		&ctx);
	layer_blur_capture_visible(server, true);
	view_set_capture_visible(view, true);
	if (!wlr_render_pass_submit(pass)) {
		return false;
	}

	vidow_stats.own++;
	dmg_clear(&fx->pending_damage); /* 这一版的账已经算清了 */
	blur_own_drop(fx);
	if (!blur_set_alloc(server, &fx->blur_set, rw, rh, levels)) {
		return false;
	}
	if (partial != NULL) {
		blur_chain_rect(server, &fx->blur_set, fx->own_bd_tex, rw, rh,
			&(struct wlr_box){ .x = px, .y = py,
				.width = pw, .height = ph },
			blur_reach_px(server, levels));
	} else {
		blur_chain(server, &fx->blur_set, fx->own_bd_tex, rw, rh);
	}
	fx->blur_own_tex = fx->blur_set.up_tex[0];
	if (fx->blur_own_tex == NULL) {
		return false;
	}
	/* 缓存键是「世界减掉这个窗口」的代次（见 vidow_backdrop_damage_view）。 */
	fx->blur_set.gen = server->gen_scene - view->self_gen;
	fx->blur_set.valid = true;
	fx->blur_own_id = server->blur_serial++;
	/* 这块区域本身（blur_tex_x/y/w/h）不在这儿登记：它跟着贴图一起由
	 * blur_assign() 换（见那里的注释），一处管一处，免得两边不一致。 */

	static int dumped = 0;
	if (dumped < blur_dump_limit()) {
		dump_buffer(server, fx->own_bd, rw, rh, "vidow-bd", dumped);
		if (fx->blur_set.up[0] != NULL && rw > 1 && rh > 1) {
			dump_buffer(server, fx->blur_set.up[0], rw >> 1, rh >> 1,
				"vidow-blur", dumped);
		}
		dumped++;
	}
	wlr_log(WLR_DEBUG, "blur: own region %s %dx%d at %d,%d levels=%d"
		"（损伤 %dx%d at %d,%d，%s %dx%d）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		rw, rh, rx, ry, levels,
		dmg.width, dmg.height, dmg.x, dmg.y,
		partial != NULL ? "局部重跑" : "整块重跑", pw, ph);
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
		if (view->fx.blur_owned) {
			continue; /* 自己的那份跟这块屏无关 */
		}
		blur_assign(&view->fx, NULL, false, 0, NULL);
	}
}

/* 这块窗口的私有模糊底衬归哪块屏：它现在**画在**哪块屏上（跨屏拖动时布局归属还没
 * 换，人已经在对面那块屏上了）；看不见任何屏就退回归属那块。和装饰离屏缓冲的倍率
 * （view.c 的 view_scale）用同一个判据 —— 倍率和区域必须落在同一块屏的坐标系里。 */
static struct wlr_output *view_blur_owner(struct vidow_view *view) {
	struct wlr_output *out = vidow_view_display_output(view);
	return out != NULL ? out : view->output;
}

/* Called once per frame before anything is drawn, so the decorations can
 * sample this frame's backdrop. Everything is cached: a frame where nothing
 * moved behind a window touches no GPU buffer at all. */
void vidow_blur_update(struct vidow_output *output) {
	struct vidow_server *server = output->server;
	struct vidow_view *view;

	int w = output->wlr_output->width, h = output->wlr_output->height;
	bool usable = server->shader_ok && server->config.blur_enabled && w > 0 &&
		h > 0 && w <= MAX_TARGET_PX && h <= MAX_TARGET_PX;
	if (!usable) {
		wl_list_for_each(view, &server->views, link) {
			if (view_blur_owner(view) != output->wlr_output) {
				continue; /* 别的屏上的窗口归那一趟收 */
			}
			blur_target_release(&view->fx);
		}
		struct vidow_layer_surface *off;
		wl_list_for_each(off, &server->layers, link) {
			vidow_layer_blur_release(off);
		}
		shared_blur_drop(output);
		blur_drop(output);
		return;
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
	bool shared_ok = output->blur_tex != NULL &&
		output->blur_gen == server->gen_layer &&
		output->blur_levels == levels &&
		fabs(output->blur_offset - server->config.blur_offset) < 1e-6 &&
		output->blur_set.w == w && output->blur_set.h == h;
	if (!shared_ok) {
		shared_ok = shared_blur_rebuild(server, output, w, h, levels);
	}

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
		if (view_blur_owner(view) != output->wlr_output) {
			continue;
		}
		if (!view_wants_blur(view) || view->fx.node == NULL) {
			if (view->fx.blur_own_tex != NULL) {
				blur_own_drop(&view->fx);
			}
			own_bd_drop(&view->fx); /* 这块屏上不再需要私有底衬了 */
			blur_assign(&view->fx, NULL, false, 0, NULL);
			continue;
		}

		blur_mask_min_changed(&view->fx, server->config.blur_alpha_min);

		struct vidow_look look;
		vidow_view_look(&server->config, &view->rule_eff,
			view->fullscreen, &look);
		if (look.blur_xray || !view_overlaps_other(view)) {
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
			blur_assign(&view->fx, output->blur_tex, false, output->blur_id, NULL);
			continue;
		}

		if (!shared_ok) {
			blur_assign(&view->fx, NULL, false, 0, NULL);
			continue;
		}

		int rx, ry, rw, rh, rlevels;
		if (!view_region_rect(server, output, view, &rx, &ry, &rw, &rh,
				&rlevels)) {
			blur_assign(&view->fx, NULL, false, 0, NULL);
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
			view->fx.blur_set.gen != server->gen_scene - view->self_gen;
		bool touched = key_changed &&
			blur_damage_hits(&view->fx, &region_logical);
		/* 形状（区域位置尺寸 / 级数 / 半径）没变：那张金字塔就是上一版的，可以
		 * 只更新受损的那一块（见 view_region_build 的 partial）。 */
		bool shape_ok = view->fx.blur_set.valid &&
			view->fx.blur_own_tex != NULL &&
			view->fx.blur_set.w == rw && view->fx.blur_set.h == rh &&
			view->fx.blur_set.levels == rlevels &&
			fabs(view->fx.blur_set.offset - server->config.blur_offset) < 1e-6 &&
			view->fx.blur_tex_x == rx && view->fx.blur_tex_y == ry &&
			view->fx.blur_tex_w == rw && view->fx.blur_tex_h == rh &&
			view->fx.own_bd != NULL && view->fx.own_bd_w == rw &&
			view->fx.own_bd_h == rh;
		bool fresh = shape_ok && !touched;
		if (!fresh) {
			struct wlr_box upd;
			bool partial = shape_ok && !blur_no_partial() &&
				region_update_rect(output, view, rx, ry, rw, rh, &upd);
			if (!view_region_build(server, output, view, rx, ry, rw, rh,
					rlevels, partial ? &upd : NULL)) {
				blur_assign(&view->fx, NULL, false, 0, NULL);
				continue;
			}
		}
		/* owned = false：这张贴图归 view->fx.blur_set（blur_set_drop 收），
		 * 这里只是指过去采样。区域跟着一起报（见 blur_assign）。 */
		blur_assign(&view->fx, view->fx.blur_own_tex, false,
			view->fx.blur_own_id,
			&(struct wlr_box){ .x = rx, .y = ry, .width = rw, .height = rh });
	}

	/* layer surface 背后的模糊（状态栏 / 面板那层）。bottom 层用共享的那份底衬
	 * （壁纸 + background 层），一次抓取都不用；top / overlay 层底下有窗口，各
	 * 自抓自己那一小块（见 layer_region_build）。 */
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

	/* overview：整屏一张糊掉的壁纸（niri 那样），垫在压暗遮罩下面。 */
	vidow_overview_blur_frame(server, output);
}

/* ------------------------------------------------------------------- draw */

static void decor_draw(struct vidow_server *server,
		struct vidow_fx_target *fx, struct wlr_buffer *dst,
		struct wlr_texture *src, struct wlr_texture *mask,
		const float border[4], int ox, int oy,
		double alpha, double scale, const struct vidow_look *look) {
	struct vidow_decor_prog *d = &server->decor;
	struct vidow_config *cfg = &server->config;

	struct wlr_gles2_texture_attribs sa;
	wlr_gles2_texture_get_attribs(src, &sa);

	struct wlr_egl *egl = NULL;
	if (begin_pass(server, dst, fx->w, fx->h, &egl) == 0) {
		return;
	}

	glUseProgram(d->program);
	bind_tex(0, sa.target, sa.tex);
	glUniform1i(d->u_texture, 0);

	struct wlr_texture *blur = fx->blur_tex != NULL ?
		fx->blur_tex : server->empty_tex;
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
	float mask_min = (float)cfg->blur_alpha_min;
	if (fx->blur_region_on || (mask == NULL && fx->blur_only)) {
		mask_min = 0.0f;
	}
	glUniform1f(d->u_mask_min, mask_min);

	/* 着色器全程在「渲染目标的物理像素」里算：配置里的圆角、边框、阴影都是
	 * 逻辑单位，乘上倍率才是像素。 */
	glUniform2f(d->u_size, (float)fx->content_w, (float)fx->content_h);
	glUniform2f(d->u_target_size, (float)fx->w, (float)fx->h);
	glUniform1f(d->u_pad, (float)fx->px_pad);
	glUniform1f(d->u_radius, (float)(look->corner_radius * scale));
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
	glUniform1f(d->u_noise, (float)look->noise);
	glUniform1f(d->u_alpha, (float)alpha);
	glUniform1f(glGetUniformLocation(d->program, "u_time"),
		(float)(vidow_now_ns() % 1000000000000LL) / 1e9f);
	/* 窗口在输出上的位置（物理像素，输出原点已减掉）：散景贴图是按输出分辨
	 * 率抓的，采样坐标必须落在这块屏自己的坐标系里。被模糊的 layer surface
	 * 那张贴图只盖住它自己那一小块区域（fx->blur_tex_*），坐标就再减掉区域
	 * 原点、除数也换成区域尺寸 —— 不减就是整块错位。 */
	glUniform2f(d->u_origin,
		(float)((int)lround((double)ox * scale) - server->output_x -
			fx->blur_tex_x),
		(float)((int)lround((double)oy * scale) - server->output_y -
			fx->blur_tex_y));
	glUniform2f(d->u_output_size,
		(float)(fx->blur_tex_w > 0 ? fx->blur_tex_w : server->output_w),
		(float)(fx->blur_tex_w > 0 ? fx->blur_tex_h : server->output_h));
	glUniform1f(d->u_flip_y, cfg->shader_flip_y ? 1.0f : 0.0f);
	/* 模糊那层要不要叠：除了「有贴图」（fx->blur_tex）和全局开关（fx->blur），
	 * 还得看**这一次渲染自己的 look** —— 预览（缩略图）那一趟在
	 * vidow_fx_frame 里把 look.blur 关掉，于是预览里不叠模糊（见那里的注释）。 */
	float strength = fx->blur && fx->blur_tex != NULL && look->blur ?
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
	wlr_scene_node_raise_to_top(&fx->node->node);
	wlr_scene_node_set_enabled(&fx->node->node, true);
	fx->active = true;
	fx->blur = server->config.blur_enabled;
	/* 默认恒等：内容贴图就是整块元素。窗口那一路每帧按窗口几何覆盖它。 */
	fx->tex_off_x = 0.0f;
	fx->tex_off_y = 0.0f;
	fx->tex_scale_x = 1.0f;
	fx->tex_scale_y = 1.0f;
	fx->anim_scale = 1.0;
	if (animated) {
		vidow_fx_set_shader(server, fx, shader_name, VIDOW_SHADER_OPEN);
	}
	return true;
}

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
	if (ov < 0.98) {
		look.blur = false;
	}
	if (geo == NULL || geo->w <= 0 || geo->h <= 0) {
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
		return false;
	}
	if (!ensure_targets(server, fx, cw, ch, scale)) {
		return false;
	}

	bool tex_owned = true;
	struct wlr_texture *tex = client_texture(server->renderer, src->buffer,
		&tex_owned);
	if (tex == NULL) {
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
			if (fx->anim_buf[i] == NULL) {
				fx->anim_buf[i] = vidow_alloc_target(server, fx->w, fx->h);
			}
		}
		anim = pick_free(fx->anim_buf);
	}

	vidow_stats.bakes++;
	struct wlr_buffer *displayed = NULL;
	struct wlr_buffer *decorated = NULL;
	const float *border = NULL; /* 重画装饰用（动画那趟失败时） */
	if (look.decor && server->decor.program != 0) {
		struct wlr_buffer *target = pick_free(fx->buf);
		if (target != NULL) {
			border = focused ? look.border_color_focus :
				look.border_color;
			/* 有动画那趟时装饰按满 alpha 画：整块的淡入淡出由动画着色器统一乘
			 * 一次，两边各乘一次就成了 alpha²（淡得比设定的快）。 */
			double decor_alpha = anim != NULL ? 1.0 : alpha;
			decor_draw(server, fx, target, tex, NULL, border, ox, oy,
				decor_alpha, scale, &look);
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
			decor_draw(server, fx, decorated, tex, NULL, border, ox, oy,
				alpha, scale, &look);
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
	int dst_w0 = (int)lround((double)fx->dst_w * ov);
	int dst_h0 = (int)lround((double)fx->dst_h * ov);
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
	if (fx->clip_on) {
		/* fx->clip 是**布局坐标**里的那块屏；这个节点的位置是相对父树的
		 * （父树就是窗口的场景树，摆在窗口的位置上）—— 减掉父树的原点才
		 * 在同一个坐标系里。 */
		struct wlr_box clip = fx->clip;
		if (fx->node->node.parent != NULL) {
			clip.x -= fx->node->node.parent->node.x;
			clip.y -= fx->node->node.parent->node.y;
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
		}
	}
	wlr_scene_node_set_position(&fx->node->node, node_x, node_y);
	wlr_scene_buffer_set_dest_size(fx->node, dst_w, dst_h);
	/* 被抓帧遮挡压着的窗口，这一帧也照样压着（抓帧随时可能在两帧之间渲染）。
	 * 正在被拖出果冻形变的窗口也一样：内容由 jelly.c 那个节点画（同一块缓冲，
	 * 只是弯了），这一块必须让位，否则两张图叠在一起。 */
	wlr_scene_buffer_set_opacity(fx->node,
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
	wlr_scene_buffer_set_buffer_with_damage(fx->node, displayed, &damage);
	pixman_region32_fini(&damage);
	/* 源框要**最后**设：换缓冲（这一路每帧在两张目标之间来回换）会把它抹掉。 */
	wlr_scene_buffer_set_source_box(fx->node, cropped ? &crop_src : NULL);

	/* This buffer now contains this blur version: the frame loop only has to
	 * redraw when a later frame hands the window a different backdrop. */
	fx->blur_drawn = fx->blur_ver;

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

/* ------------------------------------------------------- layer 的背景模糊 */

/* 状态栏 / 面板这些 layer surface 背后的模糊。
 *
 * 做法和窗口不一样：窗口是把内容和装饰一起画进一张离屏图，再让场景节点画那张图；
 * layer surface 的内容渲染归 wlroots 的 scene 管（subsurface 树、buffer scale、
 * damage 都在它手里），接管过来等于重写一遍。所以在它**下面垫一层**就够 —— 一个
 * 和它一样大的场景节点，里面只有模糊。客户端半透明的地方透出来就是模糊的底衬，
 * 不透明的地方把它盖住，等于没画。
 *
 * 底衬抓的是「本来会画在它底下的东西」（跟随 niri：它就地抓 framebuffer 里已经
 * 画好的部分），也就是这个 layer 盖住的内容：
 *   - bottom 层的 surface 先于窗口画，它底下只有壁纸和 background 层 —— 正好是共享
 *     的那张底衬，直接拿来用，一次抓取都不用（dock 那种）。
 *   - top / overlay 层反过来：窗口就在它底下，模糊里**得有窗口**。这时按它自己那
 *     一小块区域重抓一份：基座是从共享底衬上 1:1 拷过来的那块（壁纸 + background /
 *     bottom 层），窗口再画上去。代价跟这块区域的面积成正比，不是整屏。 */
/* 在子树里找第一个挂着 wlr_scene_surface 的 buffer 节点。 */
static struct wlr_scene_surface *find_scene_surface(struct wlr_scene_node *node) {
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		return wlr_scene_surface_try_from_buffer(
			wlr_scene_buffer_from_node(node));
	}
	if (node->type != WLR_SCENE_NODE_TREE) {
		return NULL;
	}
	struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
	struct wlr_scene_node *child;
	wl_list_for_each(child, &tree->children, link) {
		struct wlr_scene_surface *ss = find_scene_surface(child);
		if (ss != NULL) {
			return ss;
		}
	}
	return NULL;
}

/* 找这块 layer 的 wlr_scene_surface。wlr_scene_layer_surface_v1 只把 tree 暴露出来，
 * 表面在那棵树**里面**（wlroots 0.20 给它套了一层 subsurface 树，不是直接子节点），
 * 旁边还躺着我们自己垫的那张模糊节点的 buffer —— 所以要递归找、且要认得出哪个才是
 * 客户端的表面（find_scene_surface 只认挂得起 scene_surface 的）。 */
static struct wlr_scene_surface *layer_scene_surface(
		struct vidow_layer_surface *ls) {
	if (ls->scene == NULL || ls->scene->tree == NULL) {
		return NULL;
	}
	struct wlr_scene_node *node;
	wl_list_for_each(node, &ls->scene->tree->children, link) {
		struct wlr_scene_surface *ss = find_scene_surface(node);
		if (ss != NULL) {
			return ss;
		}
	}
	return NULL;
}

/* 这块 layer 的模糊看得见吗 —— 和窗口那条路同一个道理（见 view_blur_visible）：
 * 内容整块不透明的状态栏 / 面板，底下垫一层模糊一个像素都透不出来（装饰着色器里
 * 模糊被 content.a 乘成 0），那趟私有底衬抓取 + kawase 链条全是白算的。
 *
 * 判据保守：拿不准就当看得见。 */
static bool layer_blur_visible(struct vidow_layer_surface *ls) {
	/* 入场 / 退场动画期间内容在淡入淡出（layer.c 把 anim_alpha 乘在内容上）。 */
	if (ls->anim != NULL || ls->anim_alpha < 1.0) {
		return true;
	}
	/* 图层规则写了 opacity：内容被画淡，模糊从里面透出来。 */
	if (vidow_rule_opacity(&ls->rule_eff) < 1.0) {
		return true;
	}
	/* 抓图遮挡正盖着这一块（画出来的不是客户端的内容）。 */
	if (ls->mask_opacity) {
		return true;
	}
	struct wlr_scene_surface *ss = layer_scene_surface(ls);
	if (ss == NULL || ss->buffer == NULL || ss->buffer->buffer == NULL) {
		return true;
	}
	if (wlr_buffer_is_opaque(ss->buffer->buffer)) {
		return false; /* 格式里没 alpha（XRGB / RGBX）：整块 alpha == 1 */
	}
	/* 客户端声明了盖住整块 layer 的 opaque region（坐标是表面局部，wlroots 按
	 * 表面尺寸设的）。只声明了一部分的（圆角、留白）照旧算模糊。 */
	pixman_box32_t want = {
		.x1 = 0, .y1 = 0,
		.x2 = ls->geom.width, .y2 = ls->geom.height,
	};
	return pixman_region32_contains_rectangle(&ss->buffer->opaque_region,
		&want) != PIXMAN_REGION_IN;
}

static bool layer_blur_wanted(struct vidow_server *server,
		struct vidow_layer_surface *ls) {
	if (!server->shader_ok || !server->config.blur_enabled) {
		return false;
	}
	/* 图层规则 blur = false：这块 layer 底下不垫模糊（全局开关仍然是总闸）。 */
	if (ls->rule_eff.set[VRA_BLUR] && !ls->rule_eff.val[VRA_BLUR].b) {
		return false;
	}
	/* 壁纸自己（background 层）不模糊：模糊的底衬里就有它，垫一层等于画重。 */
	if (ls->layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND) {
		return false;
	}
	return ls->geom.width > 0 && ls->geom.height > 0 && ls->scene != NULL &&
		layer_blur_visible(ls);
}

/* 抓一块「这个 layer 盖住的东西」，含窗口。
 *
 * 只抓它自己那块区域（rx, ry 是它在输出里的位置，设备像素）：基座从共享底衬上
 * 1:1 拷一块过来，再把窗口画上去 —— 壁纸那么大的贴图不用每帧重新上传，代价跟区域
 * 面积成正比而不是整屏。区域外没有像素可抓，所以只抓屏内那一块（缓冲仍按整块
 * layer 分配，屏内那块画在自己的偏移上），采样坐标由 fx->blur_tex_* 负责对齐。 */
static bool layer_region_build(struct vidow_server *server,
		struct vidow_output *output, struct vidow_layer_surface *ls,
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

	if (ls->bd == NULL || ls->bd_w != rw || ls->bd_h != rh) {
		layer_bd_drop(ls);
		ls->bd = vidow_alloc_target(server, rw, rh);
		if (ls->bd == NULL) {
			return false;
		}
		ls->bd_tex = stat_tex_new(
			wlr_texture_from_buffer(server->renderer, ls->bd));
		if (ls->bd_tex == NULL) {
			layer_bd_drop(ls);
			return false;
		}
		ls->bd_w = rw;
		ls->bd_h = rh;
	}

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, ls->bd, NULL);
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

	/* 窗口画在基座上：绘制顺序上它们就在 top / overlay 层底下，连窗口的弹出物
	 * 一起（窗口树 → 窗口弹出物树）。 */
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
	struct wlr_scene_tree *trees[] = {
		server->layer_toplevels,
		server->layer_popups,
	};
	for (size_t i = 0; i < sizeof(trees) / sizeof(trees[0]); i++) {
		wlr_scene_node_for_each_buffer(&trees[i]->node, backdrop_iter, &ctx);
	}
	if (!wlr_render_pass_submit(pass)) {
		return false;
	}

	dmg_clear(&ls->fx.pending_damage); /* 这一版的账已经算清了 */
	/* 旧贴图裹着的缓冲马上要被金字塔复用。 */
	blur_own_drop(&ls->fx);
	if (!blur_set_alloc(server, &ls->fx.blur_set, rw, rh, levels)) {
		return false;
	}
	blur_chain(server, &ls->fx.blur_set, ls->bd_tex, rw, rh);
	ls->fx.blur_own_tex = ls->fx.blur_set.up_tex[0];
	if (ls->fx.blur_own_tex == NULL) {
		return false;
	}
	ls->fx.blur_own_id = server->blur_serial++;
	ls->bd_x = rx;
	ls->bd_y = ry;
	ls->bd_gen_scene = server->gen_scene;
	ls->bd_gen_layer = server->gen_layer;

	static int dumped = 0;
	if (dumped < blur_dump_limit()) {
		dump_buffer(server, ls->bd, rw, rh, "vidow-lbd", dumped);
		if (ls->fx.blur_set.up[0] != NULL && rw > 1 && rh > 1) {
			dump_buffer(server, ls->fx.blur_set.up[0], rw >> 1, rh >> 1,
				"vidow-lblur", dumped);
		}
		dumped++;
	}
	wlr_log(WLR_DEBUG, "blur: layer region %dx%d at %d,%d levels=%d (含窗口)",
		rw, rh, rx, ry, levels);
	return true;
}

static bool layer_blur_frame(struct vidow_server *server,
		struct vidow_output *output, struct vidow_layer_surface *ls) {
	struct vidow_fx_target *fx = &ls->fx;

	if (ls->scene == NULL) {
		return false;
	}

	/* 恒等映射：layer 的元素就是整块缓冲，没有「窗口几何」那层偏移（窗口那一
	 * 路走 vidow_fx_frame，每帧按可见窗口覆盖这几个字段）。这一路不经过
	 * vidow_fx_begin，字段默认是 0 —— 不设就是拿 texel (0,0) 当整块用。 */
	fx->tex_off_x = 0.0f;
	fx->tex_off_y = 0.0f;
	fx->tex_scale_x = 1.0f;
	fx->tex_scale_y = 1.0f;
	blur_mask_min_changed(fx, server->config.blur_alpha_min);
	/* bottom 层先于窗口画，它底下没有窗口：共享的那张底衬就是它要的全部。top /
	 * overlay 层底下有窗口，得自己抓一小块（见 layer_region_build）。 */
	bool with_windows = ls->layer != ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
	if (with_windows ? output->backdrop_tex == NULL : output->blur_tex == NULL) {
		return false;
	}
	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	int logical_w = ls->geom.width, logical_h = ls->geom.height;
	int px_w = (int)lround((double)logical_w * scale);
	int px_h = (int)lround((double)logical_h * scale);
	if (px_w < 1 || px_h < 1 || px_w > MAX_TARGET_PX || px_h > MAX_TARGET_PX) {
		return false;
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
				return false;
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
		int rx = (int)lround((double)ls->geom.x * scale) - server->output_x;
		int ry = (int)lround((double)ls->geom.y * scale) - server->output_y;
		int levels = blur_levels_for(server, px_w, px_h);
		/* 缓存键：区域（含输出原点，多屏下会动）、世界代次（窗口重画 / layer
		 * 变动）、级数、还有那两套直接影响结果的参数。 */
		/* 同窗口那条路：世界代次只是粗筛，损伤盒子没碰到这一块就不重抓。 */
		bool key_changed = ls->bd_gen_scene != server->gen_scene;
		bool touched = key_changed && blur_damage_hits(fx, &ls->geom);
		bool fresh = ls->bd != NULL && fx->blur_own_tex != NULL && !touched &&
			ls->bd_w == px_w && ls->bd_h == px_h &&
			ls->bd_x == rx && ls->bd_y == ry &&
			ls->bd_gen_layer == server->gen_layer &&
			fx->blur_set.w == px_w && fx->blur_set.h == px_h &&
			fx->blur_set.levels == levels &&
			fabs(fx->blur_set.offset - server->config.blur_offset) < 1e-6;
		if (!fresh && !layer_region_build(server, output, ls, rx, ry, px_w,
				px_h, levels)) {
			return false; /* 抓不出来就留着上一帧那张，别闪 */
		}
		blur_assign(fx, fx->blur_own_tex, false, fx->blur_own_id,
			&(struct wlr_box){ .x = rx, .y = ry,
				.width = px_w, .height = px_h });
	} else {
		/* bottom 层：用共享的那张。`set_layer` 把层改过来的话，之前抓的区域底衬
		 * 就没用了 —— 连着金字塔一起放掉，别攥着几 MB 不放。 */
		if (fx->blur_own_tex != NULL) {
			blur_own_drop(fx);
		}
		if (ls->bd != NULL) {
			wlr_buffer_drop(ls->bd);
			ls->bd = NULL;
			ls->bd_w = 0;
			ls->bd_h = 0;
			ls->bd_gen_scene = 0;
			ls->bd_gen_layer = 0;
		}
		blur_assign(fx, output->blur_tex, false, output->blur_id, NULL);
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
	struct vidow_overview *ov = &server->overview;
	struct vidow_fx_target *fx = &ov->fx;

	/* 只画 overview 锁的那块屏，而且得真在展开（t > 0）。 */
	bool want = ov->output == output->wlr_output && ov->t > 0.002 &&
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
	blur_assign(fx, output->blur_tex, false, output->blur_id, NULL);
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
	if (ls->output != NULL) {
		vidow_output_schedule_frame(ls->server, ls->output->wlr_output,
			"layer 模糊失效");
	}
}

void vidow_layer_blur_release(struct vidow_layer_surface *ls) {
	if (ls->fx.node == NULL && ls->fx.buf[0] == NULL && ls->bd == NULL) {
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
		wlr_scene_buffer_set_opacity(closing->node, (float)closing->alpha);
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
