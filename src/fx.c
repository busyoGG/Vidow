#include "server.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* --------------------------------------------------------- program build */

static bool link_program(unsigned int *out, const char *frag_src,
		const char *name) {
	GLuint vs = compile(GL_VERTEX_SHADER, vertex_src, name);
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
		if (!link_program(&prog, buf, pass[i].name)) {
			continue;
		}
		pass[i].prog->program = prog;
		pass[i].prog->u_tex = glGetUniformLocation(prog, "u_tex");
		pass[i].prog->u_texel = glGetUniformLocation(prog, "u_texel");
		pass[i].prog->u_offset = glGetUniformLocation(prog, "u_offset");
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

static GLuint begin_pass(struct vidow_server *server, struct wlr_buffer *dst,
		int w, int h, struct wlr_egl **egl) {
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
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	return fbo;
}

static void end_pass(struct wlr_egl *egl) {
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
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

/* The client's own buffer: wlr_texture_from_buffer() on a wlr_client_buffer
 * hands back the wrapper's cached texture, which we must not destroy. */
static struct wlr_buffer *render_source(struct wlr_buffer *buffer) {
	struct wlr_client_buffer *client = wlr_client_buffer_get(buffer);
	if (client != NULL && client->source != NULL) {
		return client->source;
	}
	return buffer;
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

static int blur_dump_limit(void) {
	const char *env = getenv("VIDOW_BLUR_DUMP");
	return env != NULL ? atoi(env) : 0;
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
void vidow_backdrop_damage(struct vidow_server *server, bool layers) {
	server->gen_scene++;
	if (layers) {
		server->gen_layer++;
	}
}

int vidow_fx_pad(struct vidow_server *server) {
	struct vidow_config *cfg = &server->config;
	if (!cfg->decor_enabled || !server->shader_ok) {
		return 0;
	}
	int off = abs(cfg->shadow_offset_x) > abs(cfg->shadow_offset_y) ?
		abs(cfg->shadow_offset_x) : abs(cfg->shadow_offset_y);
	int pad = (int)(cfg->shadow_radius + off + 2);
	if (pad < 0) {
		pad = 0;
	}
	if (pad > 96) {
		pad = 96;
	}
	return pad;
}

static void blur_drop(struct vidow_output *output) {
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
	blur_set_drop(set);
	for (int i = 0; i < levels; i++) {
		set->down[i] = vidow_alloc_target(server, w >> (i + 1), h >> (i + 1));
		if (set->down[i] == NULL) {
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
	}
	set->w = w;
	set->h = h;
	set->levels = levels;
	set->offset = server->config.blur_offset;
	return true;
}

static void kawase_pass(struct vidow_server *server,
		struct vidow_gl_prog *prog, struct wlr_buffer *dst, int dst_w, int dst_h,
		struct wlr_texture *src, int src_w, int src_h) {
	if (prog->program == 0 || src == NULL) {
		return;
	}
	struct wlr_egl *egl = NULL;
	if (begin_pass(server, dst, dst_w, dst_h, &egl) == 0) {
		return;
	}
	struct wlr_gles2_texture_attribs sa;
	wlr_gles2_texture_get_attribs(src, &sa);

	glUseProgram(prog->program);
	bind_tex(0, sa.target, sa.tex);
	glUniform1i(prog->u_tex, 0);
	glUniform2f(prog->u_texel, 1.0f / (float)src_w, 1.0f / (float)src_h);
	glUniform1f(prog->u_offset, (float)server->config.blur_offset);
	draw_quad(prog->program);
	glUseProgram(0);
	end_pass(egl);
}

static void blur_chain(struct vidow_server *server, struct vidow_blur_set *set,
		struct wlr_buffer *backdrop, int w, int h) {
	int levels = set->levels;
	if (levels < 2) {
		return;
	}
	struct wlr_texture *tex =
		wlr_texture_from_buffer(server->renderer, backdrop);
	for (int i = 0; i < levels && tex != NULL; i++) {
		int sw = i == 0 ? w : (w >> i), sh = i == 0 ? h : (h >> i);
		kawase_pass(server, &server->kawase_down, set->down[i], w >> (i + 1),
			h >> (i + 1), tex, sw, sh);
		wlr_texture_destroy(tex);
		tex = wlr_texture_from_buffer(server->renderer, set->down[i]);
	}
	if (tex != NULL) {
		wlr_texture_destroy(tex);
	}

	/* 往上走：每一级只把上一级的结果放大一圈（niri 的 blur_up）。目标必须比源大一
	 * 倍，u_texel 传的是**源**的尺寸 —— 放大倍数全靠它算。每一级都是同一趟着色器，
	 * 写 up[0]（半分辨率、给装饰着色器采样的那张）也不例外。 */
	struct wlr_texture *lower =
		wlr_texture_from_buffer(server->renderer, set->down[levels - 1]);
	for (int i = levels - 2; i >= 0 && lower != NULL; i--) {
		kawase_pass(server, &server->kawase_up,
			set->up[i], w >> (i + 1), h >> (i + 1), lower,
			w >> (i + 2), h >> (i + 2));
		wlr_texture_destroy(lower);
		lower = wlr_texture_from_buffer(server->renderer, set->up[i]);
	}
	if (lower != NULL) {
		wlr_texture_destroy(lower);
	}
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
	struct wlr_texture *tex = wlr_texture_from_buffer(ctx->server->renderer,
		render_source(sb->buffer));
	if (tex == NULL) {
		return;
	}
	float alpha = sb->opacity;
	struct wlr_render_texture_options opts = {
		.texture = tex,
		.dst_box = { .x = x1, .y = y1, .width = x2 - x1, .height = y2 - y1 },
		.alpha = &alpha,
		.transform = sb->transform,
		.filter_mode = WLR_SCALE_FILTER_BILINEAR,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	};
	wlr_render_pass_add_texture(ctx->pass, &opts);
	wlr_texture_destroy(tex);
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

/* A window only needs blur when something can show through it. Floating
 * windows always can; tiled ones only when blur_all is set, because their
 * backdrop is the wallpaper and the layer surfaces. */
static bool view_wants_blur(struct vidow_view *view) {
	struct vidow_config *cfg = &view->server->config;
	if (!cfg->blur_enabled || !view->mapped || !view->fx.active) {
		return false;
	}
	/* overview 里不采样背景模糊：UV 是按窗口自己的位置算的，窗口缩小之后对不上。
	 * 注意是「这一刻不给贴图」而不是把 fx->blur 关掉 —— 那个标志是 fx_begin()
	 * 按配置设的，改它的话退出 overview 之后就再也恢复不了（踩过：overview 一趟
	 * 回来，所有窗口的装饰底衬都不模糊了）。 */
	if (vidow_overview_active(view->server)) {
		return false;
	}
	if (cfg->blur_all || view->float_mode != VIDOW_FLOAT_NONE) {
		return true;
	}
	/* 客户端自己开口要的（ext-background-effect），blur_all 关着也得给 */
	return view->fx.blur_region_on;
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
}

static struct wlr_box view_screen_box(struct vidow_view *view) {
	struct wlr_box box = view->geom;
	box.x += (int)lround(view->anim_off_x);
	box.y += (int)lround(view->anim_off_y);
	return box;
}

/* A window whose box intersects another window cannot use the shared
 * wallpaper-only backdrop: it would lose the window underneath it. Those get
 * a private capture instead. Tiled windows never overlap, so the shared one
 * covers the whole scrolling layout. */
static bool view_overlaps_other(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	struct wlr_box a = view_screen_box(view);
	struct vidow_view *other;
	wl_list_for_each(other, &server->views, link) {
		if (other == view || !other->mapped || !other->placed ||
				other->output != view->output) {
			continue;
		}
		struct wlr_box b = view_screen_box(other);
		if (a.x < b.x + b.width && b.x < a.x + a.width &&
				a.y < b.y + b.height && b.y < a.y + a.height) {
			return true;
		}
	}
	return false;
}

/* Point a window at a blurred backdrop. Nothing is redrawn while the texture
 * identity stays the same, which is what keeps a static screen free. */
static void blur_assign(struct vidow_fx_target *fx, struct wlr_texture *tex,
		bool owned, uint64_t id) {
	if (fx->blur_tex_id == id) {
		return;
	}
	if (fx->blur_owned && fx->blur_tex != NULL) {
		wlr_texture_destroy(fx->blur_tex);
	}
	fx->blur_tex = tex;
	fx->blur_owned = owned;
	fx->blur_tex_id = id;
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

/* Release an owned texture before its pyramid buffer is reused. */
static void blur_own_drop(struct vidow_fx_target *fx) {
	blur_assign(fx, NULL, false, 0);
	fx->blur_own_tex = NULL; /* freed above when it was the owned one */
	fx->blur_own_id = 0;
}

/* Drop every blur resource of a target. blur_assign() only frees what the
 * target owns, so the server's shared wallpaper texture survives. */
static void blur_target_release(struct vidow_fx_target *fx) {
	blur_own_drop(fx);
	blur_set_drop(&fx->blur_set);
}

/* layer surface 背后的模糊（实现在下面「draw」之后）：先声明，抓底衬的时候
 * 要用它把节点藏起来。 */
static bool layer_blur_wanted(struct vidow_server *server,
		struct vidow_layer_surface *ls);
/* 抓底衬时要不要把这个 layer surface 自己藏起来（见下面的定义）。 */
static bool layer_blur_frame(struct vidow_server *server,
		struct vidow_output *output, struct vidow_layer_surface *ls);
static void layer_blur_capture_visible(struct vidow_server *server, bool on);

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
	if (output->blur_tex != NULL) {
		wlr_texture_destroy(output->blur_tex);
		output->blur_tex = NULL;
	}
	if (output->backdrop_tex != NULL) {
		wlr_texture_destroy(output->backdrop_tex);
		output->backdrop_tex = NULL;
	}
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

	/* The old texture wraps a buffer the pyramid is about to reuse. */
	shared_blur_drop(output);
	/* 未模糊的底衬也留一张贴图：被窗口遮住的 layer surface 拿它当自己那一小块
	 * 区域底衬的基座（比每次重抓一遍壁纸便宜太多，见 layer_region_build）。 */
	output->backdrop_tex = wlr_texture_from_buffer(server->renderer,
		output->backdrop);
	if (!blur_set_alloc(server, &output->blur_set, w, h, levels)) {
		return false;
	}
	blur_chain(server, &output->blur_set, output->backdrop, w, h);
	if (output->blur_set.up[0] != NULL) {
		output->blur_tex = wlr_texture_from_buffer(server->renderer,
			output->blur_set.up[0]);
	}
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
	output->blur_id = server->blur_serial++;
	output->blur_gen = server->gen_layer;
	output->blur_levels = levels;
	output->blur_offset = server->config.blur_offset;
	wlr_log(WLR_DEBUG, "blur: shared rebuild %dx%d levels=%d offset=%.2f", w, h,
		levels, output->blur_offset);
	return true;
}

/* Capture the scene with exactly one window removed and blur it. Only used for
 * windows that overlap another one; the pyramid is cached per window and only
 * rebuilt when the scene or the window's own box moved. */
static bool blur_own_rebuild(struct vidow_server *server,
		struct vidow_output *output, struct vidow_view *view,
		int w, int h, int levels) {
	view_set_capture_visible(view, false);
	layer_blur_capture_visible(server, false);
	bool ok = backdrop_render(server, output, w, h);
	layer_blur_capture_visible(server, true);
	view_set_capture_visible(view, true);
	if (!ok) {
		return false;
	}
	blur_own_drop(&view->fx);
	if (!blur_set_alloc(server, &view->fx.blur_set, w, h, levels)) {
		return false;
	}
	blur_chain(server, &view->fx.blur_set, output->backdrop, w, h);
	if (view->fx.blur_set.up[0] != NULL) {
		view->fx.blur_own_tex = wlr_texture_from_buffer(server->renderer,
			view->fx.blur_set.up[0]);
	}
	if (view->fx.blur_own_tex == NULL) {
		return false;
	}
	view->fx.blur_set.gen = server->gen_scene;
	view->fx.blur_set.valid = true;
	view->fx.blur_own_id = server->blur_serial++;
	struct wlr_box box = view_screen_box(view);
	wlr_log(WLR_DEBUG, "blur: own rebuild %s at %d,%d",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		box.x, box.y);
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
		blur_assign(&view->fx, NULL, false, 0);
	}
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

	static int dumped = 0;
	wl_list_for_each(view, &server->views, link) {
		if (!view_wants_blur(view) || view->fx.node == NULL) {
			if (view->fx.blur_own_tex != NULL) {
				blur_own_drop(&view->fx);
			}
			blur_assign(&view->fx, NULL, false, 0);
			continue;
		}

		blur_mask_min_changed(&view->fx, server->config.blur_alpha_min);

		if (!view_overlaps_other(view)) {
			/* Tiled and non-overlapping floating windows all share the
			 * wallpaper backdrop: one capture and one chain for all of them.
			 * Only drop a private pyramid if there really is one, otherwise
			 * the identity would change every frame and force a redraw. */
			if (view->fx.blur_own_tex != NULL) {
				blur_own_drop(&view->fx);
			}
			blur_assign(&view->fx, output->blur_tex, false, output->blur_id);
			continue;
		}

		if (!shared_ok) {
			blur_assign(&view->fx, NULL, false, 0);
			continue;
		}

		struct wlr_box box = view_screen_box(view);
		/* The capture hides this window, so its own position is irrelevant:
		 * only other windows, the output size and the level count matter. */
		(void)box;
		bool fresh = view->fx.blur_set.valid &&
			view->fx.blur_set.gen == server->gen_scene &&
			view->fx.blur_set.w == w && view->fx.blur_set.h == h &&
			view->fx.blur_set.levels == levels &&
			fabs(view->fx.blur_set.offset - server->config.blur_offset) < 1e-6 &&
			view->fx.blur_own_tex != NULL;
		if (!fresh && !blur_own_rebuild(server, output, view, w, h, levels)) {
			blur_assign(&view->fx, NULL, false, 0);
			continue;
		}
		if (dumped < blur_dump_limit()) {
			dump_buffer(server, output->backdrop, w, h, "vidow-bd", dumped);
			dumped++;
		}
		blur_assign(&view->fx, view->fx.blur_own_tex, true,
			view->fx.blur_own_id);
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
}

/* ------------------------------------------------------------------- draw */

static void decor_draw(struct vidow_server *server,
		struct vidow_fx_target *fx, struct wlr_buffer *dst,
		struct wlr_texture *src, struct wlr_texture *mask,
		const float border[4], int ox, int oy,
		double alpha, double scale) {
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
	glUniform1f(d->u_radius, (float)(cfg->corner_radius * scale));
	glUniform1f(d->u_border_width, (float)(cfg->border_width * scale));
	/* The decoration shader works in premultiplied alpha; the config keeps
	 * straight values so a script can read them back. */
	float border_pm[4] = { border[0] * border[3], border[1] * border[3],
		border[2] * border[3], border[3] };
	float shadow_pm[4] = { cfg->shadow_color[0] * cfg->shadow_color[3],
		cfg->shadow_color[1] * cfg->shadow_color[3],
		cfg->shadow_color[2] * cfg->shadow_color[3], cfg->shadow_color[3] };
	glUniform4fv(d->u_border_color, 1, border_pm);
	glUniform4fv(d->u_shadow_color, 1, shadow_pm);
	glUniform2f(d->u_shadow_offset, (float)(cfg->shadow_offset_x * scale),
		(float)(cfg->shadow_offset_y * scale));
	glUniform1f(d->u_shadow_radius, (float)(cfg->shadow_radius * scale));
	glUniform1f(d->u_opacity, (float)cfg->window_opacity);
	glUniform1f(d->u_noise, (float)cfg->noise);
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
	float strength = fx->blur && fx->blur_tex != NULL ?
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
		double alpha, bool focused, double scale, double ov) {
	if (!fx->active || fx->node == NULL || src == NULL || src->buffer == NULL) {
		return false;
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
	 * 内容和以前逐像素一样。 */
	fx->tex_off_x = geo->src_w > 0 ? (float)geo->off_x / (float)geo->src_w : 0.0f;
	fx->tex_off_y = geo->src_h > 0 ? (float)geo->off_y / (float)geo->src_h : 0.0f;
	fx->tex_scale_x = geo->src_w > 0 ? (float)geo->w / (float)geo->src_w : 1.0f;
	fx->tex_scale_y = geo->src_h > 0 ? (float)geo->h / (float)geo->src_h : 1.0f;
	int pad = vidow_fx_pad(server);
	double px_w = ((double)cw + 2.0 * pad) * (scale > 0.0 ? scale : 1.0);
	double px_h = ((double)ch + 2.0 * pad) * (scale > 0.0 ? scale : 1.0);
	if (px_w > MAX_TARGET_PX || px_h > MAX_TARGET_PX) {
		return false;
	}
	if (!ensure_targets(server, fx, cw, ch, scale)) {
		return false;
	}

	struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer,
		render_source(src->buffer));
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

	struct wlr_buffer *displayed = NULL;
	struct wlr_buffer *decorated = NULL;
	const float *border = NULL; /* 重画装饰用（动画那趟失败时） */
	if (server->config.decor_enabled && server->decor.program != 0) {
		struct wlr_buffer *target = pick_free(fx->buf);
		if (target != NULL) {
			border = focused ? server->config.border_color_focus :
				server->config.border_color;
			/* 有动画那趟时装饰按满 alpha 画：整块的淡入淡出由动画着色器统一乘
			 * 一次，两边各乘一次就成了 alpha²（淡得比设定的快）。 */
			double decor_alpha = anim != NULL ? 1.0 : alpha;
			decor_draw(server, fx, target, tex, NULL, border, ox, oy,
				decor_alpha, scale);
			displayed = target;
			decorated = target;
		} else {
			/* 两块装饰目标都还在飞（上一两帧还没上完屏）：这一帧干脆不画，
			 * 让上帧的画面多停一帧，别拿没装饰的内容顶上去。 */
			wlr_texture_destroy(tex);
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
				alpha, scale);
		}
		if (atex != tex) {
			wlr_texture_destroy(atex);
		}
	}
	wlr_texture_destroy(tex);

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
	wlr_scene_node_set_position(&fx->node->node,
		x - pad_ov + (dst_w0 - dst_w) / 2,
		y - pad_ov + (dst_h0 - dst_h) / 2);
	wlr_scene_buffer_set_dest_size(fx->node, dst_w, dst_h);
	/* 被抓帧遮挡压着的窗口，这一帧也照样压着（抓帧随时可能在两帧之间渲染）。
	 * 正在被拖出果冻形变的窗口也一样：内容由 jelly.c 那个节点画（同一块缓冲，
	 * 只是弯了），这一块必须让位，否则两张图叠在一起。 */
	wlr_scene_buffer_set_opacity(fx->node,
		(fx->masked || fx->jelly) ? 0.0f : 1.0f);

	/* Full-buffer damage: the node is re-rendered every frame of the effect. */
	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, dst_w, dst_h);
	wlr_scene_buffer_set_buffer_with_damage(fx->node, displayed, &damage);
	pixman_region32_fini(&damage);

	/* This buffer now contains this blur version: the frame loop only has to
	 * redraw when a later frame hands the window a different backdrop. */
	fx->blur_drawn = fx->blur_ver;

	if (getenv("VIDOW_FX_DUMP") != NULL) {
		dump_target(server, fx, displayed);
	}
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
		wlr_output_schedule_frame(view->output);
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
static bool layer_blur_wanted(struct vidow_server *server,
		struct vidow_layer_surface *ls) {
	if (!server->shader_ok || !server->config.blur_enabled) {
		return false;
	}
	/* 壁纸自己（background 层）不模糊：模糊的底衬里就有它，垫一层等于画重。 */
	if (ls->layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND) {
		return false;
	}
	return ls->geom.width > 0 && ls->geom.height > 0 && ls->scene != NULL;
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
		if (ls->bd != NULL) {
			wlr_buffer_drop(ls->bd);
			ls->bd = NULL;
			ls->bd_w = 0;
			ls->bd_h = 0;
		}
		ls->bd = vidow_alloc_target(server, rw, rh);
		if (ls->bd == NULL) {
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

	/* 旧贴图裹着的缓冲马上要被金字塔复用。 */
	blur_own_drop(&ls->fx);
	if (!blur_set_alloc(server, &ls->fx.blur_set, rw, rh, levels)) {
		return false;
	}
	blur_chain(server, &ls->fx.blur_set, ls->bd, rw, rh);
	if (ls->fx.blur_set.up[0] != NULL) {
		ls->fx.blur_own_tex = wlr_texture_from_buffer(server->renderer,
			ls->fx.blur_set.up[0]);
	}
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
		bool fresh = ls->bd != NULL && fx->blur_own_tex != NULL &&
			ls->bd_w == px_w && ls->bd_h == px_h &&
			ls->bd_x == rx && ls->bd_y == ry &&
			ls->bd_gen_scene == server->gen_scene &&
			ls->bd_gen_layer == server->gen_layer &&
			fx->blur_set.w == px_w && fx->blur_set.h == px_h &&
			fx->blur_set.levels == levels &&
			fabs(fx->blur_set.offset - server->config.blur_offset) < 1e-6;
		if (!fresh && !layer_region_build(server, output, ls, rx, ry, px_w,
				px_h, levels)) {
			return false; /* 抓不出来就留着上一帧那张，别闪 */
		}
		fx->blur_tex_x = rx;
		fx->blur_tex_y = ry;
		fx->blur_tex_w = px_w;
		fx->blur_tex_h = px_h;
		blur_assign(fx, fx->blur_own_tex, true, fx->blur_own_id);
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
		fx->blur_tex_w = 0; /* 0 = 整屏那张 */
		fx->blur_tex_h = 0;
		blur_assign(fx, output->blur_tex, false, output->blur_id);
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
	decor_draw(server, fx, target, server->empty_tex, mask, no_border,
		ls->geom.x, ls->geom.y, 1.0, scale);
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

static void layer_blur_capture_visible(struct vidow_server *server, bool on) {
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		if (ls->fx.node != NULL) {
			wlr_scene_node_set_enabled(&ls->fx.node->node, on);
		}
		/* 光藏模糊节点不够：**会被模糊的那个 surface 本身**也不能进底衬。
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
		wlr_output_schedule_frame(ls->output->wlr_output);
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
	if (ls->bd != NULL) {
		wlr_buffer_drop(ls->bd);
		ls->bd = NULL;
		ls->bd_w = 0;
		ls->bd_h = 0;
		ls->bd_gen_scene = 0;
		ls->bd_gen_layer = 0;
	}
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
	/* 回弹的曲线让 t 稍微越过 1：位移和缩放跟着过冲（那才是回弹）。 */
	closing->off_y = (double)closing->slide_px * t;
	closing->fx.anim_scale = 1.0 + (closing->anim_scale_to - 1.0) * t;
	closing->fx.progress = t;
	vidow_backdrop_damage(closing->server, false);

	int y = closing->y + (int)lround(closing->off_y);
	if (closing->fx.active) {
		vidow_fx_frame(closing->server, &closing->fx, closing->node,
			&closing->geo, closing->x, y, closing->x, y, closing->alpha,
			false, closing->scale, 1.0);
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
		double scale, enum vidow_easing easing, double anim_scale_to) {
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
