/* 把内部状态变成 JSON（见 ipcstate.h）。这一份**只读**：不发命令、不改状态，
 * 除了往订阅者的连接上写事件。 */
#include "clipboard.h"
#include "gesture.h"
#include "ipcstate.h"
#include "outputconf.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h> /* DRM_FORMAT_XRGB8888 / XRGB2101010（vidow.output 的 depth） */

#include <wayland-server.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "hooks.h"
#include "ipc.h"
#include "rule.h"
#include "server.h"

/* ------------------------------------------------------------------ 小工具 */

/* 颜色（四个 0..1 的分量）→ "#rrggbbaa"，和配置里读回来的写法一致。 */
static void jb_kv_color(struct jbuf *b, const char *key, const float c[4]) {
	char buf[16];
	snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x",
		(int)(c[0] * 255.0f + 0.5f), (int)(c[1] * 255.0f + 0.5f),
		(int)(c[2] * 255.0f + 0.5f), (int)(c[3] * 255.0f + 0.5f));
	jb_kv_str(b, key, buf);
}

static int64_t now_ms(void) {
	return vidow_now_ns() / 1000000;
}

/* 指针还活着吗（记忆 / 工作区里存的是裸指针，可能早就关掉了）。 */
static bool view_alive(struct vidow_server *server, struct vidow_view *want) {
	if (want == NULL) {
		return false;
	}
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view == want) {
			return true;
		}
	}
	return false;
}

static const char *float_mode_name(enum vidow_float_mode mode) {
	switch (mode) {
	case VIDOW_FLOAT_FIXED:
		return "fixed";
	case VIDOW_FLOAT_FOLLOW:
		return "follow";
	case VIDOW_FLOAT_NONE:
	default:
		return "none";
	}
}

static const char *transform_name(enum wl_output_transform t) {
	switch (t) {
	case WL_OUTPUT_TRANSFORM_90:
		return "90";
	case WL_OUTPUT_TRANSFORM_180:
		return "180";
	case WL_OUTPUT_TRANSFORM_270:
		return "270";
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		return "flipped";
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		return "flipped-90";
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		return "flipped-180";
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		return "flipped-270";
	case WL_OUTPUT_TRANSFORM_NORMAL:
	default:
		return "normal";
	}
}

static const char *layer_name(enum zwlr_layer_shell_v1_layer layer) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		return "background";
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		return "bottom";
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		return "top";
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
		return "overlay";
	default:
		return "unknown";
	}
}

static const char *kb_interactivity_name(
		enum zwlr_layer_surface_v1_keyboard_interactivity k) {
	switch (k) {
	case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE:
		return "exclusive";
	case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND:
		return "on-demand";
	case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE:
	default:
		return "none";
	}
}

/* 动画进度：0..1（动画自己的时钟，不是帧时钟）。 */
static bool anim_progress(struct vidow_anim *anim, double *out) {
	if (anim == NULL || anim->duration_ns <= 0) {
		return false;
	}
	double t = (double)(vidow_now_ns() - anim->start_ns) /
		(double)anim->duration_ns;
	*out = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
	return true;
}

static void json_box(struct jbuf *b, int x, int y, int w, int h) {
	jb_obj(b);
	jb_kv_int(b, "x", x);
	jb_kv_int(b, "y", y);
	jb_kv_int(b, "width", w);
	jb_kv_int(b, "height", h);
	jb_obj_end(b);
}

static void json_anim(struct jbuf *b, struct vidow_anim *anim) {
	double t = 0.0;
	if (!anim_progress(anim, &t)) {
		jb_null(b);
		return;
	}
	jb_obj(b);
	jb_kv_str(b, "tag", anim->tag);
	jb_kv_double(b, "progress", t);
	jb_kv_int(b, "duration_ms", (long long)(anim->duration_ns / 1000000));
	jb_kv_int(b, "elapsed_ms",
		(long long)((vidow_now_ns() - anim->start_ns) / 1000000));
	jb_kv_int(b, "easing", anim->easing.kind);
	/* 曲线名连参数一起给：自定义贝塞尔光看一个数字看不出形状。 */
	char curve[64];
	jb_kv_str(b, "curve",
		vidow_ease_text(&anim->easing, curve, sizeof(curve)));
	jb_obj_end(b);
}

/* --------------------------------------------------------------- 窗口 */

/* 「真正的 toplevel」：bar / 任务栏只该看见这些。
 *
 * 排除：
 *   - xdg_popup（菜单 / tooltip / 输入法候选框那类，view->popup）；
 *   - 还没上屏的（mapped = false；Steam 的 CEF 辅助表面经常挂在这里）；
 *   - X11 的 override-redirect / 菜单 / tooltip / utility / dock 类型
 *     （Steam 的弹出表面大多落在这几类，它们不是窗口）。
 * 保留 DIALOG：文件选择框、设置对话框是真正的顶层窗口，任务栏该显示。
 */
static bool window_is_toplevel(const struct vidow_view *view) {
	if (view == NULL || view->popup || !view->mapped || view->focus_helper) {
		return false;
	}
	if (view->kind != VIDOW_VIEW_X11 || view->x11 == NULL) {
		return true;
	}
	if (view->x11->override_redirect) {
		return false;
	}
	static const int skip[] = {
		VIDOW_X11_WT_UTILITY,
		VIDOW_X11_WT_MENU,
		VIDOW_X11_WT_DROPDOWN_MENU,
		VIDOW_X11_WT_POPUP_MENU,
		VIDOW_X11_WT_TOOLTIP,
		VIDOW_X11_WT_COMBO,
		VIDOW_X11_WT_DND,
		VIDOW_X11_WT_NOTIFICATION,
		VIDOW_X11_WT_DOCK,
		VIDOW_X11_WT_DESKTOP,
	};
	for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); ++i) {
		if (vidow_x11_surface_has_window_type(view->x11, skip[i])) {
			return false;
		}
	}
	return true;
}

void vidow_ipc_json_column(struct jbuf *b, struct vidow_server *server,
		struct vidow_column *col) {
	(void)server;
	if (col == NULL) {
		jb_null(b);
		return;
	}
	jb_obj(b);
	jb_kv_uint(b, "id", col->id);
	jb_kv_double(b, "width_fraction", col->width_fraction);
	jb_kv_bool(b, "maximized", col->maximized);
	jb_kv_int(b, "width", col->width);
	jb_kv_int(b, "offset", col->offset);
	jb_kv_str(b, "workspace", col->ws != NULL ? col->ws->name : NULL);
	jb_kv_str(b, "output", col->output != NULL ? col->output->name : NULL);
	int index = 0;
	if (col->ws != NULL) {
		struct vidow_column *other;
		wl_list_for_each(other, &col->ws->columns, link) {
			if (other == col) {
				break;
			}
			index++;
		}
	}
	jb_kv_int(b, "index", index);
	jb_key(b, "windows");
	jb_arr(b);
	struct vidow_view *view;
	wl_list_for_each(view, &col->views, column_link) {
		if (window_is_toplevel(view)) {
			jb_uint(b, view->id);
		}
	}
	jb_arr_end(b);
	/* **列里到底挂了几个 view**（不只是 toplevel）。
	 *
	 * 上面的 windows 只报 window_is_toplevel 的窗口 —— popup / 没上屏的 /
	 * override-redirect 全被排除。于是「列看着是空的、cleanup 却不肯删」
	 * 这种状态在 IPC 里完全看不出来（vidow_workspace_cleanup 用的是
	 * wl_list_empty(&col->views)，那个链表非空就不删）。诊断这类僵尸列必须
	 * 能看见真实长度，否则只能靠猜。 */
	jb_kv_int(b, "view_count", (long long)wl_list_length(&col->views));
	jb_obj_end(b);
}

void vidow_ipc_json_window(struct jbuf *b, struct vidow_server *server,
		struct vidow_view *view) {
	if (view == NULL) {
		jb_null(b);
		return;
	}
	jb_obj(b);
	jb_kv_uint(b, "id", view->id);
	jb_kv_str(b, "kind", view->kind == VIDOW_VIEW_X11 ? "x11" : "xdg");
	jb_kv_str(b, "title", vidow_view_title(view));
	jb_kv_str(b, "app_id", vidow_view_app_id(view));
	jb_kv_bool(b, "mapped", view->mapped);
	jb_kv_bool(b, "configured", view->configured);
	jb_kv_bool(b, "placed", view->placed);
	jb_kv_bool(b, "popup", view->popup);
	jb_kv_bool(b, "fullscreen", view->fullscreen);
	/* 列宽比例（0.5 = 半屏，1.0 = 占满整块工作区）。dock / 任务栏靠它判断
	 * 「当前工作区是不是被占满了」—— vidow 没有单独的 maximize，niri 也是拿
	 * 整列宽度当的（见 src/view.c 里同步 foreign-toplevel 的那段）。 */
	jb_kv_double(b, "column_width",
		view->column != NULL ? view->column->width_fraction : 0.0);
	/* 最小化：窗口还在原来的列里（列宽 / 顺序 / 工作区都留着），只是不占位、不显示、
	 * 不参与焦点。任务栏 / 状态栏拿它决定画不画那个「收起来」的条目。 */
	jb_kv_bool(b, "minimized", view->minimized);
	jb_kv_bool(b, "focused", server->focused == view);
	/* 画在屏幕上没有：滚出视口 / 工作区藏起来的窗口现在是「停到屏外」而不是
	 * 关节点（见 server.h 的 scene_parked），所以不能再看节点那个 enabled。 */
	jb_kv_bool(b, "visible", view->scene_tree != NULL &&
		view->scene_tree->node.enabled && !view->scene_parked);
	jb_kv_str(b, "output", view->output != NULL ? view->output->name : NULL);
	jb_kv_str(b, "workspace", view->ws != NULL ? view->ws->name : NULL);
	/* 稳定编号。`workspace` 是**名字**，动态工作区没名字就是空串 —— 而
	 * noctalia 的 workspaceKeyMatchesAssignment() 见到空串直接 return false，
	 * 于是 dock 的 smart_auto_hide 永远认为活动工作区是空的（用户报的
	 * 「dock 不自动隐藏」）。给它一个永远非空、和工作区列表 id 对得上的值。 */
	if (view->ws != NULL) {
		jb_kv_int(b, "workspace_id", (long long)view->ws->id);
	}
	jb_kv_bool(b, "workspace_special", view->ws != NULL && view->ws->special);
	jb_kv_str(b, "float_mode", float_mode_name(view->float_mode));
	jb_kv_bool(b, "floating", view->float_mode != VIDOW_FLOAT_NONE);
	/* 小窗（mini window，S1）：形态 + 它跟的那块屏。小窗是**屏级对象**，不属于
	 * 任何工作区（上面的 workspace 是 null），位置记的是相对那块屏工作区的比例
	 * 锚点（mini_anchor）。形态：none / free / mini / icon。 */
	jb_kv_str(b, "mini", vidow_mini_state_name(view->mini));
	jb_kv_str(b, "mini_output",
		view->output != NULL ? view->output->name : NULL);
	jb_kv_str(b, "mini_home",
		view->mini_home != NULL ? view->mini_home->key : NULL);
	jb_kv_str(b, "mini_aspect_rule",
		view->mini_aspect_rule != VIDOW_MINI_ASPECT_UNSET ?
			vidow_mini_aspect_name(view->mini_aspect_rule) : NULL);
	if (view->mini != VIDOW_MINI_NONE) {
		jb_key(b, "mini_anchor");
		jb_obj(b);
		jb_kv_double(b, "x", view->mini_ax);
		jb_kv_double(b, "y", view->mini_ay);
		jb_kv_double(b, "wf", view->mini_wf);
		jb_kv_double(b, "hf", view->mini_hf);
		jb_kv_bool(b, "size_known", view->mini_size_known);
		jb_obj_end(b);
	}

	jb_key(b, "geometry");
	json_box(b, view->geom.x, view->geom.y, view->geom.width, view->geom.height);
	jb_key(b, "float_geometry");
	if (view->float_mode != VIDOW_FLOAT_NONE) {
		json_box(b, view->float_geom.x, view->float_geom.y,
			view->float_geom.width, view->float_geom.height);
	} else {
		jb_null(b);
	}
	jb_kv_int(b, "home_dx", view->home_dx);
	jb_kv_int(b, "home_dy", view->home_dy);

	struct wlr_surface *surface = vidow_view_surface(view);
	jb_key(b, "surface");
	if (surface != NULL) {
		jb_obj(b);
		jb_kv_int(b, "width", surface->current.width);
		jb_kv_int(b, "height", surface->current.height);
		jb_kv_int(b, "buffer_width", surface->current.buffer_width);
		jb_kv_int(b, "buffer_height", surface->current.buffer_height);
		jb_kv_int(b, "scale", surface->current.scale);
		jb_kv_bool(b, "mapped", surface->mapped);
		/* 装饰管线正在采源缓冲的哪一块（归一化）。du/dv < 1 = 客户端用
		 * wp_viewport 只声明了缓冲的一部分有效（chromium / KDE 在分数缩放下会
		 * 把缓冲开得比窗口大一圈）：排查「框到位了、画面小一圈、一条黑边」时，
		 * 这个值和 geometry 摆在一起就能看出是采错区域还是客户端没画。 */
		struct vidow_fx_geo fgeo = vidow_view_fx_geo(view);
		jb_kv_double(b, "tex_u0", (double)fgeo.tex_u0);
		jb_kv_double(b, "tex_v0", (double)fgeo.tex_v0);
		jb_kv_double(b, "tex_du", (double)fgeo.tex_du);
		jb_kv_double(b, "tex_dv", (double)fgeo.tex_dv);
		/* 装饰真正画出来的那块框（逻辑尺寸 = 可见窗口）。float 尺寸由客户端
		 * 定，和「按布局算出来的 geometry」不是一回事：客户端不接受 configure
		 * 时会比槽位大，排查「框和画面不一般大」要看这对。 */
		jb_kv_int(b, "frame_width", fgeo.w);
		jb_kv_int(b, "frame_height", fgeo.h);
		/* Runtime popup diagnostics: distinguish hidden/transparent nodes from
		 * a valid X drawable rendered through a stale scene texture. */
		jb_kv_bool(b, "tree_enabled", view->scene_tree != NULL &&
			view->scene_tree->node.enabled);
		jb_kv_bool(b, "scene_parked", view->scene_parked);
		jb_kv_bool(b, "fx_active", view->fx.active);
		/* **还在等几个 frame callback**（诊断「0fps / 卡死」的决定性数字）。
		 *
		 * > 0 = 客户端请求了下一帧、正在等合成器发 frame_done。此时如果
		 * 输出在正常出帧，就是合成器漏发了（bug 在我们这边）；如果输出
		 * 静止不出帧，就是没人推那一帧（补帧链断了）。
		 * 0 = 客户端根本没请求下一帧，它不打算画 —— 卡死不在渲染路径上，
		 * 要去查输入 / 客户端自己。
		 *
		 * 用户报「Element 用一阵子后 0fps + 不可交互」时，卡住的那一刻跑
		 * 一次 `vidowmsg windows`，这个数就够定性了。 */
		jb_kv_int(b, "frame_callbacks",
			vidow_view_pending_frame_callbacks(view));
		/* 装饰离屏缓冲（per-output）：窗口骑在两块屏中间时每块屏各有一份
		 * （见 server.h 的 struct vidow_blur_slot 的 dec*）。跨屏倍率问题
		 * 的回归判据就靠这一组 —— 以前测试靠 grep 日志里的 `fx: target`，
		 * 那是**所有**离屏缓冲（含模糊金字塔的 480x270 中间级）的日志，
		 * 抓到的常常是金字塔而不是窗口的装饰缓冲。
		 *
		 * bake_output 是「这一帧的装饰是给哪块屏烘的」：判据要连它一起看
		 * （1 倍屏上就该 < 2500，2 倍屏上就该 > 3000）。 */
		jb_kv_str(b, "bake_output", view->fx.bake_output != NULL ?
			view->fx.bake_output->name : "");
		/* **每块屏各一份**（跨屏时窗口骑在两块屏中间）：1 倍屏那份该是
		 * ~窗口逻辑宽，2 倍屏那份该是两倍。测试就按这个判「两块屏是不是
		 * 各按自己的倍率烘」，而不是看 cur_slot（那只代表最后烘的那块）。 */
		jb_key(b, "dec_slots");
		jb_arr(b);
		for (struct vidow_blur_slot *ds = view->fx.blur_slot;
				ds != NULL; ds = ds->next) {
			if (ds->out == NULL) {
				continue;
			}
			jb_obj(b);
			jb_kv_str(b, "output", ds->out->name);
			jb_kv_int(b, "width", ds->dec_w);
			jb_kv_int(b, "height", ds->dec_h);
			jb_kv_double(b, "scale", ds->dec_scale);
			jb_obj_end(b);
		}
		jb_arr_end(b);
		if (view->fx.cur_slot != NULL) {
			jb_kv_int(b, "dec_width", view->fx.cur_slot->dec_w);
			jb_kv_int(b, "dec_height", view->fx.cur_slot->dec_h);
			jb_kv_double(b, "dec_scale", view->fx.cur_slot->dec_scale);
		} else {
			jb_kv_int(b, "dec_width", 0);
			jb_kv_int(b, "dec_height", 0);
			jb_kv_double(b, "dec_scale", 0.0);
		}
		if (view->scene_surface != NULL && view->scene_surface->buffer != NULL) {
			struct wlr_scene_buffer *sb = view->scene_surface->buffer;
			jb_kv_double(b, "node_opacity", sb->opacity);
			jb_kv_int(b, "dst_width", sb->dst_width);
			jb_kv_int(b, "dst_height", sb->dst_height);
			jb_kv_double(b, "source_x", sb->src_box.x);
			jb_kv_double(b, "source_y", sb->src_box.y);
			jb_kv_double(b, "source_width", sb->src_box.width);
			jb_kv_double(b, "source_height", sb->src_box.height);
		}
		jb_obj_end(b);
	} else {
		jb_null(b);
	}

	jb_key(b, "column");
	vidow_ipc_json_column(b, server, view->column);
	jb_key(b, "parent");
	if (view->parent != NULL) {
		jb_uint(b, view->parent->id);
	} else {
		jb_null(b);
	}
	jb_kv_int(b, "parent_depth", view->parent != NULL ? 1 : 0);
	jb_key(b, "anim_move");
	json_anim(b, view->anim_move);
	jb_key(b, "anim_open");
	json_anim(b, view->anim_open);
	jb_kv_double(b, "alpha", view->anim_alpha);
	/* 这个窗口现在该用多长的开合动画（规则写了 anim / open_ms / close_ms 就是
	 * 规则的那份，没写就是全局配置）：0 = 不做那个动画。 */
	{
		struct vidow_rule_anim ra;
		vidow_rule_anim(&server->config, &view->rule_eff, false, &ra);
		jb_kv_bool(b, "anim", ra.on);
		jb_kv_int(b, "open_ms", ra.open_ms);
		jb_kv_int(b, "close_ms", ra.close_ms);
	}
	jb_kv_bool(b, "force_render", view->force_render);
	jb_kv_int(b, "pump_interval_ms", view->pump_interval_ms);
	jb_kv_int(b, "capture_holders",
		(long long)wl_list_length(&view->capture_sources));
	jb_kv_bool(b, "pending_activate", view->pending_activate);

	jb_key(b, "rules");
	jb_obj(b);
	jb_kv_bool(b, "focus_on_open", view->focus_on_open);
	jb_kv_bool(b, "focus_on_activate", view->focus_on_activate);
	jb_kv_bool(b, "fake_focus", view->fake_focus);
	jb_kv_bool(b, "honor_invalid_serial", view->honor_invalid_serial);
	/* 抓帧遮挡 / X11 缩放桥也是规则给的（见 rule.c）：屏幕上、抓帧里各是什么样
	 * 只看得出「现在」，自检脚本要的是「规则有没有套上」。 */
	jb_kv_str(b, "capture", view->capture_mode == VIDOW_CAPTURE_HIDE ? "hide" :
		view->capture_mode == VIDOW_CAPTURE_MOSAIC ? "mosaic" : "off");
	jb_kv_int(b, "capture_mosaic", view->capture_mosaic);
	jb_kv_double(b, "x11_scale", view->x11_scale);
	jb_obj_end(b);

	/* 这个窗口现在该长什么样：全局配置压上窗口规则，再按全屏收一道（rule.c 的
	 * vidow_view_look）。渲染用的就是这一份 —— 自检脚本拿它当判据，不用去量像素。 */
	struct vidow_look look;
	vidow_view_look(&server->config, &view->rule_eff, view->fullscreen, &look);
	jb_key(b, "look");
	jb_obj(b);
	jb_kv_double(b, "opacity", look.opacity);
	jb_kv_double(b, "corner_radius", look.corner_radius);
	jb_kv_double(b, "corner_n", look.corner_n);
	jb_kv_double(b, "border_width", look.border_width);
	jb_kv_color(b, "border_color", look.border_color);
	jb_kv_color(b, "border_color_focus", look.border_color_focus);
	jb_kv_double(b, "shadow_radius", look.shadow_radius);
	jb_kv_color(b, "shadow_color", look.shadow_color);
	jb_kv_int(b, "shadow_offset_x", look.shadow_offset_x);
	jb_kv_int(b, "shadow_offset_y", look.shadow_offset_y);
	jb_kv_double(b, "noise", look.noise);
	jb_kv_bool(b, "decor", look.decor);
	jb_kv_bool(b, "blur", look.blur);
	jb_kv_bool(b, "blur_xray", look.blur_xray);
	jb_obj_end(b);

	/* 客户端自己要的尺寸边界（xdg 的 min/max，X11 读 WM_NORMAL_HINTS）。 */
	jb_key(b, "size_hints");
	{
		int min_w = 0, min_h = 0, max_w = 0, max_h = 0;
		if (view->kind == VIDOW_VIEW_XDG && view->xdg_toplevel != NULL) {
			min_w = view->xdg_toplevel->current.min_width;
			min_h = view->xdg_toplevel->current.min_height;
			max_w = view->xdg_toplevel->current.max_width;
			max_h = view->xdg_toplevel->current.max_height;
		} else if (view->kind == VIDOW_VIEW_X11 && view->x11 != NULL &&
				view->x11->size_hints != NULL) {
			min_w = view->x11->size_hints->min_width;
			min_h = view->x11->size_hints->min_height;
			max_w = view->x11->size_hints->max_width;
			max_h = view->x11->size_hints->max_height;
		}
		if (min_w > 0 || min_h > 0 || max_w > 0 || max_h > 0) {
			jb_obj(b);
			jb_kv_int(b, "min_width", min_w);
			jb_kv_int(b, "min_height", min_h);
			jb_kv_int(b, "max_width", max_w);
			jb_kv_int(b, "max_height", max_h);
			jb_obj_end(b);
		} else {
			jb_null(b);
		}
	}

	long long pid = 0;
	if (view->kind == VIDOW_VIEW_XDG && view->xdg_toplevel != NULL &&
			view->xdg_toplevel->base != NULL &&
			view->xdg_toplevel->base->client != NULL) {
		pid_t p = 0;
		uid_t u = 0;
		gid_t g = 0;
		wl_client_get_credentials(view->xdg_toplevel->base->client->client,
			&p, &u, &g);
		pid = p;
	} else if (view->kind == VIDOW_VIEW_X11 && view->x11 != NULL) {
		pid = view->x11->pid;
	}
	jb_kv_int(b, "pid", pid);

	if (view->kind == VIDOW_VIEW_X11 && view->x11 != NULL) {
		jb_key(b, "x11");
		jb_obj(b);
		jb_kv_uint(b, "window_id", view->x11->window_id);
		jb_kv_bool(b, "override_redirect", view->x11->override_redirect);
		jb_kv_str(b, "role", view->x11->role);
		jb_kv_str(b, "instance", view->x11->instance);
		jb_kv_str(b, "class", view->x11->class);
		jb_kv_int(b, "x", view->x11->x);
		jb_kv_int(b, "y", view->x11->y);
		jb_kv_int(b, "width", view->x11->width);
		jb_kv_int(b, "height", view->x11->height);
		jb_kv_int(b, "window_type_count", (long long)view->x11->window_type_count);
		jb_obj_end(b);
	}
	jb_obj_end(b);
}

void vidow_ipc_json_windows(struct jbuf *b, struct vidow_server *server) {
	jb_arr(b);
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (!window_is_toplevel(view)) {
			continue;
		}
		vidow_ipc_json_window(b, server, view);
	}
	jb_arr_end(b);
}

/* ------------------------------------------------------------ 工作区 */

void vidow_ipc_json_workspace(struct jbuf *b, struct vidow_server *server,
		struct vidow_workspace *ws) {
	if (ws == NULL) {
		jb_null(b);
		return;
	}
	jb_obj(b);
	jb_kv_int(b, "id", (long long)ws->id);
	/* 序号 = 它在自己那块屏上的位置（1 起）—— 就是 `focus-workspace N` 里的 N。
	 * 特殊工作区没有位置，报 0。 */
	jb_kv_int(b, "idx", vidow_workspace_index(ws));
	jb_kv_bool(b, "named", ws->named);
	jb_kv_str(b, "name", ws->name[0] != '\0' ? ws->name : NULL);
	jb_kv_bool(b, "special", ws->special);
	jb_kv_bool(b, "active", ws->active);
	jb_kv_bool(b, "visible", ws->visible);
	jb_kv_bool(b, "shown", vidow_workspace_shown_for(server,
		ws->output != NULL ? ws->output : server->focused_output) == ws);
	jb_kv_str(b, "output", ws->output != NULL ? ws->output->name : NULL);
	jb_kv_str(b, "home", ws->home != NULL ? ws->home->key : NULL);
	jb_kv_double(b, "viewport_x", ws->viewport_x);
	jb_kv_int(b, "floats", ws->floats);
	{
		int n = 0;
		struct vidow_column *c;
		wl_list_for_each(c, &ws->columns, link) {
			struct vidow_view *v;
			wl_list_for_each(v, &c->views, column_link) {
				if (window_is_toplevel(v)) {
					n++;
				}
			}
		}
		if (!ws->special) {
			struct vidow_view *v;
			wl_list_for_each(v, &server->views, link) {
				if (v->ws == ws && v->column == NULL &&
						window_is_toplevel(v)) {
					n++;
				}
			}
		}
		jb_kv_int(b, "window_count", n);
	}
	jb_key(b, "focused_window");
	if (view_alive(server, ws->focused)) {
		jb_uint(b, ws->focused->id);
	} else {
		jb_null(b);
	}
	jb_key(b, "columns");
	jb_arr(b);
	struct vidow_column *col;
	wl_list_for_each(col, &ws->columns, link) {
		vidow_ipc_json_column(b, server, col);
	}
	jb_arr_end(b);
	jb_key(b, "windows");
	jb_arr(b);
	wl_list_for_each(col, &ws->columns, link) {
		struct vidow_view *view;
		wl_list_for_each(view, &col->views, column_link) {
			if (window_is_toplevel(view)) {
				jb_uint(b, view->id);
			}
		}
	}
	if (!ws->special) {
		/* 浮窗不进列：单独数一遍（挂在 ws_link 上）。 */
		struct vidow_view *view;
		wl_list_for_each(view, &server->views, link) {
			if (view->ws == ws && view->column == NULL &&
					window_is_toplevel(view)) {
				jb_uint(b, view->id);
			}
		}
	}
	jb_arr_end(b);
	jb_obj_end(b);
}

void vidow_ipc_json_workspaces(struct jbuf *b, struct vidow_server *server) {
	jb_arr(b);
	struct vidow_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		vidow_ipc_json_workspace(b, server, ws);
	}
	jb_arr_end(b);
}

void vidow_ipc_json_columns(struct jbuf *b, struct vidow_server *server) {
	jb_arr(b);
	struct vidow_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		struct vidow_column *col;
		wl_list_for_each(col, &ws->columns, link) {
			vidow_ipc_json_column(b, server, col);
		}
	}
	jb_arr_end(b);
}

/* -------------------------------------------------------------- 输出 */

void vidow_ipc_json_output(struct jbuf *b, struct vidow_server *server,
		struct vidow_output *output) {
	if (output == NULL) {
		jb_null(b);
		return;
	}
	struct wlr_output *o = output->wlr_output;
	jb_obj(b);
	jb_kv_str(b, "name", o->name);
	jb_kv_str(b, "description", o->description);
	jb_kv_str(b, "make", o->make);
	jb_kv_str(b, "model", o->model);
	jb_kv_str(b, "serial", o->serial);
	jb_kv_bool(b, "enabled", o->enabled);
	jb_kv_double(b, "scale", o->scale);
	jb_kv_str(b, "transform", transform_name(o->transform));
	jb_kv_double(b, "refresh_hz", o->refresh / 1000.0);
	jb_kv_int(b, "width", o->width);
	jb_kv_int(b, "height", o->height);
	jb_kv_int(b, "logical_width", (int)lround(o->width / o->scale));
	jb_kv_int(b, "logical_height", (int)lround(o->height / o->scale));
	jb_kv_int(b, "physical_width_mm", o->phys_width);
	jb_kv_int(b, "physical_height_mm", o->phys_height);
	jb_kv_int(b, "subpixel", (int)o->subpixel);
	jb_kv_bool(b, "adaptive_sync", o->adaptive_sync_status ==
		WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED);
	jb_kv_bool(b, "vrr_supported", o->adaptive_sync_supported);
	/* 按需 VRR（niri 的 variable-refresh-rate on-demand）：这块屏是不是按需
	 * 模式、上一次算出来「要不要提刷新率」。真机上前者开 + 后者 true 时
	 * adaptive_sync 才会是 true（headless 后端不支持，只会看到这两个字段动）。 */
	{
		const struct vidow_output_conf *conf =
			vidow_output_conf_for(server, o->name);
		bool on_demand = conf != NULL && conf->has_vrr_on_demand ?
			conf->vrr_on_demand :
			(conf != NULL && conf->has_vrr ? false :
				server->config.vrr_on_demand);
		jb_kv_bool(b, "vrr_on_demand", on_demand);
		jb_kv_bool(b, "vrr_on_demand_want", output->vrr_on_demand_on);
	}
	{
		/* 输出缓冲的位深（vidow.output 的 depth）：10 位走 XRGB2101010。 */
		const char *name = "?";
		switch (o->render_format) {
		case DRM_FORMAT_XRGB8888:
			name = "xrgb8888";
			break;
		case DRM_FORMAT_XRGB2101010:
			name = "xrgb2101010";
			break;
		}
		char hex[16];
		snprintf(hex, sizeof(hex), "0x%08x", o->render_format);
		jb_kv_str(b, "render_format", strcmp(name, "?") == 0 ? hex : name);
		jb_kv_int(b, "depth", o->render_format == DRM_FORMAT_XRGB2101010 ?
			10 : 8);
	}
	{
		/* 这块屏**自己报**出来的色彩能力：wlroots 从 EDID + 连接器属性填的
		 * （DRM 那边是 Colorspace 属性 + EDID 的 BT2020 / HDR static metadata，
		 * 见 backend/drm/util.c）。vidow 现在不拿它做 HDR，但「这台机器能不能
		 * 做 HDR」就取决于这里有没有 bt2020 + st2084_pq —— 两个都有才轮得到
		 * image description 不被拒。headless / wayland 后端一律是空的。 */
		jb_key(b, "primaries");
		jb_arr(b);
		if (o->supported_primaries & WLR_COLOR_NAMED_PRIMARIES_SRGB) {
			jb_str(b, "srgb");
		}
		if (o->supported_primaries & WLR_COLOR_NAMED_PRIMARIES_BT2020) {
			jb_str(b, "bt2020");
		}
		jb_arr_end(b);
		jb_key(b, "transfer_functions");
		jb_arr(b);
		if (o->supported_transfer_functions &
				WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ) {
			jb_str(b, "st2084_pq");
		}
		jb_arr_end(b);
		jb_kv_bool(b, "hdr_capable",
			(o->supported_primaries & WLR_COLOR_NAMED_PRIMARIES_BT2020) != 0 &&
			(o->supported_transfer_functions &
				WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ) != 0);
	}
	jb_kv_bool(b, "focused", server->focused_output == o);
	jb_kv_str(b, "memory", output->memory != NULL ? output->memory->key : NULL);
	jb_key(b, "layout");
	json_box(b, output->box.x, output->box.y, output->box.width,
		output->box.height);
	jb_key(b, "usable_area");
	json_box(b, output->usable_area.x, output->usable_area.y,
		output->usable_area.width, output->usable_area.height);
	jb_kv_double(b, "viewport_x", output->viewport_x);
	jb_key(b, "active_workspace");
	{
		struct vidow_workspace *ws = vidow_workspace_active_for(server, o);
		jb_str(b, ws != NULL ? ws->name : NULL);
	}
	jb_key(b, "shown_workspace");
	{
		struct vidow_workspace *ws = vidow_workspace_shown_for(server, o);
		jb_str(b, ws != NULL ? ws->name : NULL);
	}
	jb_key(b, "workspaces");
	jb_arr(b);
	struct vidow_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (!ws->special && ws->output == o) {
			jb_str(b, ws->name);
		}
	}
	jb_arr_end(b);
	jb_key(b, "windows");
	jb_arr(b);
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->output == o && window_is_toplevel(view)) {
			jb_uint(b, view->id);
		}
	}
	jb_arr_end(b);
	jb_key(b, "layers");
	jb_arr(b);
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		if (ls->output != NULL && ls->output->wlr_output == o) {
			jb_uint(b, ls->id);
		}
	}
	jb_arr_end(b);
	jb_key(b, "modes");
	jb_arr(b);
	struct wlr_output_mode *mode;
	wl_list_for_each(mode, &o->modes, link) {
		jb_obj(b);
		jb_kv_int(b, "width", mode->width);
		jb_kv_int(b, "height", mode->height);
		jb_kv_double(b, "refresh_hz", mode->refresh / 1000.0);
		jb_kv_bool(b, "preferred", mode->preferred);
		jb_obj_end(b);
	}
	jb_arr_end(b);
	jb_key(b, "blur");
	jb_obj(b);
	jb_kv_int(b, "levels", output->blur_levels);
	jb_kv_uint(b, "texture_id", output->blur_id);
	jb_kv_uint(b, "generation", output->blur_gen);
	jb_kv_bool(b, "backdrop_valid", output->backdrop != NULL);
	jb_obj_end(b);
	jb_obj_end(b);
}

void vidow_ipc_json_outputs(struct jbuf *b, struct vidow_server *server) {
	jb_arr(b);
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		vidow_ipc_json_output(b, server, output);
	}
	jb_arr_end(b);
}

/* ------------------------------------------------------- layer surface */

void vidow_ipc_json_layer(struct jbuf *b, struct vidow_server *server,
		struct vidow_layer_surface *ls) {
	if (ls == NULL) {
		jb_null(b);
		return;
	}
	struct wlr_layer_surface_v1 *l = ls->layer_surface;
	jb_obj(b);
	jb_kv_uint(b, "id", ls->id);
	jb_kv_str(b, "namespace", l->namespace);
	jb_kv_str(b, "layer", layer_name(ls->layer));
	jb_kv_str(b, "output", ls->output != NULL ? ls->output->wlr_output->name :
		(l->output != NULL ? l->output->name : NULL));
	jb_kv_str(b, "home", ls->home != NULL ? ls->home->key : NULL);
	jb_kv_bool(b, "mapped", l->surface != NULL && l->surface->mapped);
	jb_kv_bool(b, "configured", l->configured);
	jb_kv_int(b, "exclusive_zone", l->current.exclusive_zone);
	jb_kv_str(b, "keyboard_interactivity",
		kb_interactivity_name(l->current.keyboard_interactive));
	jb_kv_int(b, "desired_width", (int)l->current.desired_width);
	jb_kv_int(b, "desired_height", (int)l->current.desired_height);
	jb_kv_int(b, "actual_width", (int)l->current.actual_width);
	jb_kv_int(b, "actual_height", (int)l->current.actual_height);
	jb_key(b, "anchor");
	jb_obj(b);
	jb_kv_bool(b, "top", (l->current.anchor &
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) != 0);
	jb_kv_bool(b, "bottom", (l->current.anchor &
		ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) != 0);
	jb_kv_bool(b, "left", (l->current.anchor &
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) != 0);
	jb_kv_bool(b, "right", (l->current.anchor &
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) != 0);
	jb_obj_end(b);
	jb_key(b, "margin");
	jb_obj(b);
	jb_kv_int(b, "top", l->current.margin.top);
	jb_kv_int(b, "right", l->current.margin.right);
	jb_kv_int(b, "bottom", l->current.margin.bottom);
	jb_kv_int(b, "left", l->current.margin.left);
	jb_obj_end(b);
	jb_key(b, "geometry");
	json_box(b, ls->geom.x, ls->geom.y, ls->geom.width, ls->geom.height);
	jb_kv_double(b, "alpha", ls->anim_alpha);
	jb_key(b, "anim");
	json_anim(b, ls->anim);
	/* 图层规则给的抓帧遮挡（见 rule.c）。 */
	jb_kv_str(b, "capture", ls->capture_mode == VIDOW_CAPTURE_HIDE ? "hide" :
		ls->capture_mode == VIDOW_CAPTURE_MOSAIC ? "mosaic" : "off");
	jb_kv_int(b, "capture_mosaic", ls->capture_mosaic);
	/* 图层规则给的「全景里露不露」（见 rule.c 的 overview）。only 的那种现在
	 * 多半是关着的 —— 它只在全景里出现，别拿它当「桌面上的壁纸」。 */
	jb_kv_str(b, "overview", ls->overview_mode == VIDOW_OVERVIEW_ONLY ? "only" :
		ls->overview_mode == VIDOW_OVERVIEW_NEVER ? "never" : "always");
	jb_kv_bool(b, "shown", ls->scene != NULL && ls->scene->tree->node.enabled);
	/* 规则算出来的开合动画与不透明度：这块面板现在「该」怎么样看这里，
	 * 不用去量像素（alpha 是动画那一刻的值，动画停了恒为 1）。 */
	jb_key(b, "rule");
	jb_obj(b);
	{
		struct vidow_rule_anim ra;
		vidow_rule_anim(&server->config, &ls->rule_eff, true, &ra);
		jb_kv_bool(b, "anim", ra.on);
		jb_kv_int(b, "open_ms", ra.open_ms);
		jb_kv_int(b, "close_ms", ra.close_ms);
	}
	jb_kv_double(b, "opacity", vidow_rule_opacity(&ls->rule_eff));
	jb_obj_end(b);
	jb_obj_end(b);
}

void vidow_ipc_json_layers(struct jbuf *b, struct vidow_server *server) {
	jb_arr(b);
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		vidow_ipc_json_layer(b, server, ls);
	}
	jb_arr_end(b);
}

/* 这个 layer surface 现在真的画在屏上吗：mapped + 场景节点是开着的。规则
 * overview = "only" 的在全景没开的时候节点是关的（见 overview.c），所以这一个
 * 判据同时管住了「被规则收起来的」和「被全景整棵收起来的」。 */
static bool layer_drawn(const struct vidow_layer_surface *ls) {
	return ls->layer_surface != NULL && ls->layer_surface->surface != NULL &&
		ls->layer_surface->surface->mapped && ls->scene != NULL &&
		ls->scene->tree->node.enabled;
}

/* 谁在画桌面背景（壁纸）。壁纸**不**是 vidow 画的：它是 layer-shell 的
 * background 层（swaybg / swww / quickshell 都走这条路），vidow 只在谁都没画
 * 的时候垫一层 background_color。这个查询就是给 shell 用的：「这屏的壁纸有人
 * 管了吗，我要不要自己上」。 */
void vidow_ipc_json_background(struct jbuf *b, struct vidow_server *server) {
	char hex[10];
	snprintf(hex, sizeof(hex), "#%02x%02x%02x%02x",
		(int)(server->config.background_color[0] * 255.0f + 0.5f),
		(int)(server->config.background_color[1] * 255.0f + 0.5f),
		(int)(server->config.background_color[2] * 255.0f + 0.5f),
		(int)(server->config.background_color[3] * 255.0f + 0.5f));

	jb_obj(b);
	jb_kv_str(b, "color", hex);

	/* 画背景的那几个 surface（background 层，按 vidow_server.layers 的顺序）。
	 * visible = 现在真的画在屏上的那几个：规则 overview = "only" 的在全景没开
	 * 的时候是关着的（见 overview.c），别把它算成「桌面上的壁纸」。 */
	int n = 0;
	int visible = 0;
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		if (ls->layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND) {
			n++;
			if (layer_drawn(ls)) {
				visible++;
			}
		}
	}
	jb_kv_int(b, "clients", n);
	jb_kv_int(b, "visible", visible);

	jb_key(b, "surfaces");
	jb_arr(b);
	wl_list_for_each(ls, &server->layers, link) {
		if (ls->layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND) {
			vidow_ipc_json_layer(b, server, ls);
		}
	}
	jb_arr_end(b);

	/* 每块屏一行：这块屏的壁纸有没有人画（有个 mapped 的 background surface
	 * 铺满它就算有）。shell 常驻画背景时靠这个判断「是不是轮到我了」。 */
	jb_key(b, "outputs");
	jb_arr(b);
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, output->wlr_output,
			&box);
		int covering = 0;
		wl_list_for_each(ls, &server->layers, link) {
			if (ls->layer != ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND ||
					ls->output != output || !layer_drawn(ls)) {
				continue;
			}
			if (ls->geom.width >= box.width && ls->geom.height >= box.height) {
				covering++;
			}
		}
		jb_obj(b);
		jb_kv_str(b, "name", output->wlr_output->name);
		jb_kv_bool(b, "covered", covering > 0);
		jb_kv_int(b, "covering", covering);
		jb_obj_end(b);
	}
	jb_arr_end(b);
	jb_obj_end(b);
}

/* ------------------------------------------------------- 录屏（screencast） */

static const char *sc_kind_name(enum vidow_sc_kind kind) {
	switch (kind) {
	case VIDOW_SC_OUTPUT:
		return "output";
	case VIDOW_SC_WINDOW:
		return "window";
	case VIDOW_SC_REGION:
		return "region";
	case VIDOW_SC_VIRTUAL:
		return "virtual";
	default:
		return "?";
	}
}

/* 录屏那几路流现在什么状态：发帧节奏（cap_ms = min(消费者帧率, 屏幕刷新率)）、
 * 发了多少帧、整帧抓了多少次、看见多少次输出提交、一帧的主线程成本（grab_ms =
 * 整块离屏渲染，readback_ms = 回读到 PipeWire 的 memfd）。
 *
 * 「抓帧次数 ≈ 输出提交次数」就是以前那个 bug 的样子（每次提交都抓一整帧，
 * 144Hz 的屏上主线程被占满，屏幕上所有动画跟着掉到 60fps）；修完之后它应该
 * 跟 frames 一个量级（消费者要一帧才抓一帧）。
 *
 * 「录制的时候整个桌面卡」再往下查就看 grab_ms / readback_ms 哪一半贵：
 * 4K 一帧 33MB，readback_ms 是大头 → 该给消费者 dmabuf（像素不落 CPU）。
 *
 * 现在 readback_ms 是 0 才对：帧走 dmabuf 直通交给消费者（dmabuf_frames 数它），
 * 一帧都不落 CPU —— 那条路要抓帧渲染进一张 linear 缓冲（见 capture.c 的
 * vidow_grab_target_get）。dmabuf_frames == 0 且 frames > 0 = 退回回读那条路
 * （pixman / 没有 dmabuf 的会话，或者消费者不认 dmabuf）。 */
void vidow_ipc_json_screencast(struct jbuf *b, struct vidow_server *server) {
	jb_arr(b);
	if (server->screencast != NULL) {
		struct vidow_screencast_stream *stream;
		wl_list_for_each(stream, &server->screencast->streams, link) {
			struct vidow_sc_stream_stats st;
			vidow_screencast_stream_stats(stream, &st);
			jb_obj(b);
			jb_kv_str(b, "kind", sc_kind_name(stream->kind));
			jb_kv_str(b, "output", stream->output != NULL ?
				stream->output->name : NULL);
			jb_kv_int(b, "width", stream->out_w);
			jb_kv_int(b, "height", stream->out_h);
			jb_kv_uint(b, "pointer_mode", stream->pointer_mode);
			jb_kv_bool(b, "closed", stream->closed);
			jb_kv_bool(b, "has_pw", st.has_pw);
			jb_kv_double(b, "consumer_fps", st.consumer_fps);
			jb_kv_double(b, "cap_ms", st.cap_ms);
			jb_kv_uint(b, "frames", st.frames);
			jb_kv_uint(b, "grabs", st.grabs);
			jb_kv_uint(b, "commits", st.commits);
			jb_kv_uint(b, "throttled", st.throttled);
			jb_kv_uint(b, "dmabuf_frames", st.dmabuf_frames);
			jb_kv_double(b, "grab_ms", st.grab_ms);
			jb_kv_double(b, "grab_ms_max", st.grab_ms_max);
			jb_kv_double(b, "readback_ms", st.readback_ms);
			jb_kv_double(b, "sync_ms", st.sync_ms);
			jb_kv_uint(b, "cursor_pushes", stream->cursor_pushes);
			jb_kv_int(b, "last_send_ms", st.last_send_ms);
			jb_obj_end(b);
		}
	}
	jb_arr_end(b);
}

/* ------------------------------------------------------------- 其它 */

static void config_entry(void *data, const char *key, const char *value) {
	jb_kv_str(data, key, value);
}

void vidow_ipc_json_config(struct jbuf *b, struct vidow_server *server) {
	jb_obj(b);
	vidow_config_each(&server->config, config_entry, b);
	jb_obj_end(b);
}

void vidow_ipc_json_keybinds(struct jbuf *b, struct vidow_server *server) {
	jb_arr(b);
	struct vidow_keybind *bind;
	wl_list_for_each(bind, &server->keybinds, link) {
		jb_obj(b);
		jb_kv_str(b, "trigger", bind->trigger);
		jb_kv_str(b, "action", bind->action);
		jb_kv_uint(b, "mods", bind->mods);
		/* 绑定上的重复限制（vidow.bind 第三个参数）：repeatable 默认 true、
		 * cooldown_ms 默认 0 —— 两个都报出来，脚本 / 状态栏能看出「这条为什么
		 * 按住不连发」。`repeat` 是老字段名，同一个值照报一份（老脚本不改）。 */
		jb_kv_bool(b, "repeatable", bind->repeatable);
		jb_kv_bool(b, "repeat", bind->repeatable);
		jb_kv_int(b, "cooldown_ms", bind->cooldown_ms);
		/* 会话锁定期间也生效（vidow.bind 的 { locked = true }，默认 false）。 */
		jb_kv_bool(b, "locked", bind->locked);
		jb_obj_end(b);
	}
	jb_arr_end(b);
}

void vidow_ipc_json_gestures(struct jbuf *b, struct vidow_server *server) {
	jb_arr(b);
	struct vidow_gesture *it;
	wl_list_for_each(it, &server->gestures, link) {
		jb_obj(b);
		jb_kv_str(b, "trigger", it->trigger);
		jb_kv_str(b, "action", it->action);
		jb_kv_int(b, "fingers", it->fingers);
		jb_kv_str(b, "kind",
			it->kind == VIDOW_GESTURE_VIEW ? "view" :
			it->kind == VIDOW_GESTURE_SWIPE ? "swipe" :
			it->kind == VIDOW_GESTURE_PINCH ? "pinch" : "hold");
		jb_obj_end(b);
	}
	jb_arr_end(b);
}

void vidow_ipc_json_rules(struct jbuf *b, struct vidow_server *server) {
	jb_arr(b);
	struct vidow_rule *rule;
	wl_list_for_each(rule, &server->rules, link) {
		jb_obj(b);
		/* 字段表在 rule.c 的 vidow_rule_fields[]：匹配和作用都在里面，这里只是
		 * 把「写了哪些」按类型倒出来。没写的字段输出 null —— 「没管它」和
		 * 「管了、给的是默认值」是两回事。 */
		if (rule->literal) {
			jb_kv_bool(b, "literal", true);
		}
		for (size_t i = 0; i < vidow_rule_field_count; i++) {
			const struct vidow_rule_field *f = &vidow_rule_fields[i];
			jb_key(b, f->name);
			/* 没写的字段也出一个 null（键是稳的，外面不用先判有没有）。 */
			if (!vidow_rule_has_field(rule, f)) {
				jb_null(b);
				continue;
			}
			switch (f->kind) {
			case RK_STR: {
				const struct vidow_rule_match *m = &rule->str[f->slot];
				if (m->negate) {
					char buf[VIDOW_RULE_PAT + 1];
					snprintf(buf, sizeof(buf), "!%s", m->pat);
					jb_str(b, buf);
				} else {
					jb_str(b, m->pat);
				}
				break;
			}
			case RK_BOOL:
				jb_bool(b, rule->bval[f->slot]);
				break;
			case RK_NUM:
				jb_int(b, rule->nval[f->slot]);
				break;
			case RK_ACT_BOOL:
				jb_bool(b, rule->aval[f->slot].b);
				break;
			case RK_ACT_INT:
				jb_int(b, rule->aval[f->slot].i);
				break;
			case RK_ACT_DOUBLE:
				jb_double(b, rule->aval[f->slot].d);
				break;
			case RK_ACT_STR:
				jb_str(b, rule->aval[f->slot].s);
				break;
			case RK_ACT_COLOR: {
				const float *c = rule->aval[f->slot].color;
				char buf[16];
				snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x",
					(int)(c[0] * 255.0f + 0.5f),
					(int)(c[1] * 255.0f + 0.5f),
					(int)(c[2] * 255.0f + 0.5f),
					(int)(c[3] * 255.0f + 0.5f));
				jb_str(b, buf);
				break;
			}
			case RK_ACT_CAPTURE:
				jb_str(b, rule->aval[f->slot].i == VIDOW_CAPTURE_HIDE ?
					"hide" :
					rule->aval[f->slot].i == VIDOW_CAPTURE_MOSAIC ?
					"mosaic" : "off");
				break;
			case RK_ACT_OVERVIEW:
				jb_str(b, rule->aval[f->slot].i == VIDOW_OVERVIEW_ONLY ?
					"only" :
					rule->aval[f->slot].i == VIDOW_OVERVIEW_NEVER ?
					"never" : "always");
				break;
			default:
				jb_null(b);
				break;
			}
		}
		jb_obj_end(b);
	}
	jb_arr_end(b);
}

void vidow_ipc_json_seats(struct jbuf *b, struct vidow_server *server) {
	jb_arr(b);
	jb_obj(b);
	jb_kv_str(b, "name", server->seat->name);
	jb_key(b, "capabilities");
	jb_arr(b);
	if (server->seat->capabilities & WL_SEAT_CAPABILITY_POINTER) {
		jb_str(b, "pointer");
	}
	if (server->seat->capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
		jb_str(b, "keyboard");
	}
	if (server->seat->capabilities & WL_SEAT_CAPABILITY_TOUCH) {
		jb_str(b, "touch");
	}
	jb_arr_end(b);
	jb_key(b, "focused_window");
	if (server->focused != NULL) {
		jb_uint(b, server->focused->id);
	} else {
		jb_null(b);
	}
	jb_key(b, "keyboard");
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	if (kb != NULL) {
		jb_obj(b);
		jb_kv_str(b, "device", kb->base.name);
		const char *layout = NULL;
		if (kb->keymap != NULL) {
			layout = xkb_keymap_layout_get_name(kb->keymap,
				kb->modifiers.group);
		}
		jb_kv_str(b, "layout", layout);
		jb_kv_int(b, "group", kb->modifiers.group);
		jb_kv_int(b, "repeat_rate", kb->repeat_info.rate);
		jb_kv_int(b, "repeat_delay", kb->repeat_info.delay);
		jb_kv_uint(b, "leds", kb->leds);
		jb_obj_end(b);
	} else {
		jb_null(b);
	}
	jb_obj_end(b);
	jb_arr_end(b);
}

void vidow_ipc_json_input(struct jbuf *b, struct vidow_server *server) {
	jb_obj(b);
	jb_key(b, "cursor");
	jb_obj(b);
	jb_kv_double(b, "x", server->cursor->x);
	jb_kv_double(b, "y", server->cursor->y);
	/* 摇晃放大的状态：magnified / scale / image / hardware / hw_max */
	vidow_cursor_fx_json(b, server);
	jb_obj_end(b);
	jb_key(b, "keyboards");
	jb_arr(b);
	struct vidow_keyboard *kb;
	wl_list_for_each(kb, &server->keyboards, link) {
		struct wlr_keyboard *k = kb->wlr_keyboard;
		jb_obj(b);
		jb_kv_str(b, "name", k->base.name);
		jb_kv_bool(b, "virtual", kb->virtual != NULL);
		jb_kv_bool(b, "inject", kb->inject);
		const char *layout = NULL;
		if (k->keymap != NULL) {
			layout = xkb_keymap_layout_get_name(k->keymap, k->modifiers.group);
		}
		jb_kv_str(b, "layout", layout);
		jb_kv_int(b, "group", k->modifiers.group);
		jb_kv_int(b, "repeat_rate", k->repeat_info.rate);
		jb_kv_int(b, "repeat_delay", k->repeat_info.delay);
		jb_kv_uint(b, "leds", k->leds);
		/* 修饰键状态（排错用）：depressed 是这块键盘物理按着的修饰位
		 * （xkb mask），wm_mods 是 wlr 那套（WL_MODIFIER）编码 —— 滚轮 /
		 * 键位绑定读的就是它。「Alt+滚轮切完工作区，不按 Alt 也能切」= 这里
		 * 残留了 Alt 位，一眼能看出来是哪块键盘。 */
		jb_kv_uint(b, "depressed", (unsigned)k->modifiers.depressed);
		jb_kv_uint(b, "locked_mods", (unsigned)k->modifiers.locked);
		jb_kv_uint(b, "wm_mods", (unsigned)wlr_keyboard_get_modifiers(k));
		jb_key(b, "keys");
		jb_arr(b);
		for (size_t i = 0; i < k->num_keycodes; i++) {
			jb_uint(b, k->keycodes[i]);
		}
		jb_arr_end(b);
		jb_obj_end(b);
	}
	jb_arr_end(b);
	jb_key(b, "grab");
	jb_obj(b);
	jb_kv_str(b, "mode", server->grab_mode == VIDOW_GRAB_MOVE ? "move" :
		server->grab_mode == VIDOW_GRAB_RESIZE ? "resize" :
		server->grab_mode == VIDOW_GRAB_TILE ? "tile" :
		server->grab_mode == VIDOW_GRAB_RESIZE_TILE ? "resize-column" :
		"none");
	jb_key(b, "window");
	if (view_alive(server, server->grab_view)) {
		jb_uint(b, server->grab_view->id);
	} else {
		jb_null(b);
	}
	jb_obj_end(b);
	/* 指针约束（游戏锁鼠标 / 夹光标）：null = 现在没有生效的约束 */
	jb_key(b, "pointer_constraint");
	vidow_pointer_constraint_json(b, server);
	/* 键盘那两个协议（快捷键抑制 / X11 抓着键盘，见 keyboardgrab.c）：
	 * keyboard_grab = null 表示没有 X 客户端抓着键盘。 */
	vidow_keyboard_grab_json(b, server);
	jb_key(b, "keyboard_layer");
	jb_str(b, server->keyboard_layer != NULL &&
		server->keyboard_layer->layer_surface != NULL ?
		server->keyboard_layer->layer_surface->namespace : NULL);
	jb_obj_end(b);
}

void vidow_ipc_json_tokens(struct jbuf *b, struct vidow_server *server) {
	jb_arr(b);
	struct vidow_activation_token *token;
	wl_list_for_each(token, &server->activation_tokens, link) {
		jb_obj(b);
		jb_kv_str(b, "name", token->name);
		jb_kv_str(b, "app_id", token->app_id);
		jb_kv_bool(b, "has_serial", token->has_serial);
		jb_kv_bool(b, "serial_ok", token->serial_ok);
		jb_kv_uint(b, "serial", token->serial);
		jb_kv_bool(b, "has_surface", token->surface != NULL);
		long long age_ms = (long long)((vidow_now_ns() / 1000000) -
			((int64_t)token->created.tv_sec * 1000 +
			 token->created.tv_nsec / 1000000));
		jb_kv_int(b, "age_ms", age_ms);
		jb_obj_end(b);
	}
	jb_arr_end(b);
}

void vidow_ipc_json_memories(struct jbuf *b, struct vidow_server *server) {
	jb_arr(b);
	struct vidow_output_memory *memory;
	wl_list_for_each(memory, &server->output_memories, link) {
		jb_obj(b);
		jb_kv_str(b, "key", memory->key);
		jb_kv_bool(b, "present", memory->present);
		jb_key(b, "last_box");
		json_box(b, memory->last_box.x, memory->last_box.y,
			memory->last_box.width, memory->last_box.height);
		jb_kv_double(b, "viewport_x", memory->viewport_x);
		jb_key(b, "focused_window");
		if (view_alive(server, memory->focused)) {
			jb_uint(b, memory->focused->id);
		} else {
			jb_null(b);
		}
		jb_obj_end(b);
	}
	jb_arr_end(b);
}

void vidow_ipc_json_anims(struct jbuf *b, struct vidow_server *server) {
	jb_obj(b);
	jb_key(b, "running");
	jb_arr(b);
	struct vidow_anim *anim;
	wl_list_for_each(anim, &server->anims, link) {
		json_anim(b, anim);
	}
	jb_arr_end(b);
	/* 关闭动画（窗口没了之后还在淡出的那张快照）单独列一份。 */
	jb_key(b, "closing");
	jb_arr(b);
	struct vidow_closing *closing;
	wl_list_for_each(closing, &server->closing, link) {
		jb_obj(b);
		json_box(b, closing->x, closing->y, closing->w, closing->h);
		jb_kv_double(b, "alpha", closing->alpha);
		jb_obj_end(b);
	}
	jb_arr_end(b);
	jb_obj_end(b);
}

void vidow_ipc_json_im(struct jbuf *b, struct vidow_server *server) {
	jb_obj(b);
	jb_kv_bool(b, "active", server->im.active);
	jb_kv_bool(b, "connected", server->im.method != NULL);
	jb_kv_bool(b, "grab", server->im.grab != NULL);
	jb_kv_bool(b, "popup", server->im_popup.popup != NULL);
	jb_kv_int(b, "text_inputs",
		(long long)wl_list_length(&server->text_inputs));
	jb_key(b, "popup_geometry");
	if (server->im_popup.popup != NULL &&
			server->im_popup.popup->surface != NULL &&
			server->im_popup.scene != NULL) {
		struct wlr_surface *s = server->im_popup.popup->surface;
		json_box(b, server->im_popup.scene->buffer->node.x,
			server->im_popup.scene->buffer->node.y,
			s->current.width, s->current.height);
	} else {
		jb_null(b);
	}
	jb_obj_end(b);
}

void vidow_ipc_json_overview(struct jbuf *b, struct vidow_server *server) {
	jb_obj(b);
	jb_kv_bool(b, "active", vidow_overview_active(server));
	jb_kv_bool(b, "target", server->overview.on);
	jb_kv_double(b, "t", server->overview.t);
	jb_kv_double(b, "scale", vidow_overview_scale(server));
	jb_kv_str(b, "output", server->overview.output != NULL ?
		server->overview.output->name : NULL);
	/* 上下排布的工作区列表：几行、一行多高、列表滚到哪儿了（以前报的是相机
	 * 位置 + 滚轮平移，那是「一条列带 + 一个相机」的模型，已经没了）。
	 *
	 * 顶层报的是**主屏**（overview.output，进来时聚焦的那块）那一份 —— 老字段
	 * 保持原样，测试不用改。每块屏各自那一份在下面的 screens 数组里。 */
	struct vidow_output *main_out = server->overview.output != NULL ?
		vidow_output_lookup(server, server->overview.output) : NULL;
	if (main_out != NULL) {
		jb_kv_int(b, "rows", main_out->ov.n_rows);
		jb_kv_double(b, "row_h", main_out->ov.row_h);
		jb_kv_double(b, "list_scroll", main_out->ov.list_scroll);
	} else {
		jb_kv_int(b, "rows", 0);
		jb_kv_double(b, "row_h", 0.0);
		jb_kv_double(b, "list_scroll", 0.0);
	}
	/* 每块屏一份（多显示器时每块屏各摆自己的行表）。 */
	jb_key(b, "screens");
	jb_arr(b);
	struct vidow_output *out;
	wl_list_for_each(out, &server->outputs, link) {
		jb_obj(b);
		jb_kv_str(b, "output", out->wlr_output->name);
		jb_kv_bool(b, "used", out->ov.used);
		jb_kv_int(b, "rows", out->ov.n_rows);
		jb_kv_double(b, "row_h", out->ov.row_h);
		jb_kv_double(b, "scale", out->ov.scale);
		jb_kv_double(b, "list_scroll", out->ov.list_scroll);
		jb_obj_end(b);
	}
	jb_arr_end(b);
	jb_obj_end(b);
}

/* Alt+Tab 的切换器 / 最近用过的窗口（见 mru.c）。台账（recent）和面板上的排版
 * （entries：位置尺寸缩放）都报出来 —— 测试要靠它做几何断言。 */
void vidow_ipc_json_mru(struct jbuf *b, struct vidow_server *server) {
	const struct vidow_mru *mru = &server->mru;
	static const char *scope_names[] = { "all", "workspace", "output" };
	int scope = server->config.mru_scope;
	jb_obj(b);
	jb_kv_bool(b, "enabled", server->config.mru_enabled);
	jb_kv_bool(b, "active", vidow_mru_active(server));
	jb_kv_bool(b, "showing", vidow_mru_showing(server));
	jb_kv_str(b, "layout", server->config.mru_layout == VIDOW_MRU_GRID ?
		"grid" : "strip");
	jb_kv_str(b, "scope", scope_names[scope >= 0 && scope < 3 ? scope : 0]);
	jb_kv_uint(b, "selected", mru->selected != NULL ? mru->selected->id : 0);
	jb_kv_uint(b, "original", mru->original != NULL ? mru->original->id : 0);
	jb_kv_uint(b, "pending", mru->pending != NULL ? mru->pending->id : 0);
	jb_kv_int(b, "count", mru->n);
	jb_kv_int(b, "slots", mru->slot_n);
	jb_kv_int(b, "total_w", mru->total_w);
	jb_kv_int(b, "total_h", mru->total_h);
	jb_kv_double(b, "scroll", mru->scroll);
	/* 鼠标底下那一项（只影响样式，不改选中）。 */
	jb_kv_uint(b, "hover", mru->hover != NULL ? mru->hover->id : 0);
	jb_key(b, "entries");
	jb_arr(b);
	for (int i = 0; i < mru->slot_n; i++) {
		const struct vidow_mru_slot *slot = &mru->slot[i];
		if (slot->view == NULL) {
			continue;
		}
		jb_obj(b);
		jb_kv_uint(b, "id", slot->view->id);
		jb_kv_str(b, "title", vidow_view_title(slot->view));
		jb_kv_str(b, "app_id", vidow_view_app_id(slot->view));
		jb_kv_bool(b, "selected", slot->view == mru->selected);
		jb_kv_bool(b, "hover", slot->view == mru->hover);
		/* 缩略图摆在哪（= 卡片里缩进内边距那块）。 */
		jb_kv_int(b, "x", slot->x);
		jb_kv_int(b, "y", slot->y);
		jb_kv_int(b, "w", slot->w);
		jb_kv_int(b, "h", slot->h);
		/* 背后那张卡片（预览 + 内边距）：鼠标命中按它算。 */
		jb_kv_int(b, "cx", slot->cx);
		jb_kv_int(b, "cy", slot->cy);
		jb_kv_int(b, "cw", slot->cw);
		jb_kv_int(b, "ch", slot->ch);
		jb_kv_double(b, "zoom", slot->zoom);
		jb_obj_end(b);
	}
	jb_arr_end(b);
	/* 台账（不打开切换器也能看）：最近用过的顺序。 */
	jb_key(b, "recent");
	jb_arr(b);
	struct vidow_view *list[64];
	size_t n = vidow_mru_collect(server, NULL, list, 64);
	int64_t now = vidow_mru_now_ns();
	for (size_t i = 0; i < n; i++) {
		jb_obj(b);
		jb_kv_uint(b, "id", list[i]->id);
		jb_kv_str(b, "title", vidow_view_title(list[i]));
		jb_kv_int(b, "ago_ms", list[i]->mru_ts > 0 ?
			(long long)((now - list[i]->mru_ts) / 1000000) : -1);
		jb_obj_end(b);
	}
	jb_arr_end(b);
	jb_obj_end(b);
}

void vidow_ipc_json_xwayland(struct jbuf *b, struct vidow_server *server) {
	jb_obj(b);
	jb_kv_bool(b, "enabled", server->config.xwayland);
	jb_kv_bool(b, "running", server->xwayland_server != NULL);
	jb_kv_double(b, "scale", server->xwayland_scale);
	jb_kv_str(b, "display", getenv("DISPLAY"));
	jb_obj_end(b);
}

void vidow_ipc_json_clipboard(struct jbuf *b, struct vidow_server *server,
		struct vidow_clip_selection *sel, const char *what) {
	struct vidow_clipboard *clip = &server->clipboard;
	size_t bytes = 0;
	int complete = 0, overflow = 0;
	for (int i = 0; i < sel->nmimes; ++i) {
		bytes += sel->mimes[i].len;
		if (sel->mimes[i].complete) {
			complete++;
		}
		if (sel->mimes[i].overflow) {
			overflow++;
		}
	}
	int waiting_x11 = 0, waiting_wayland = 0, pulling = 0;
	struct vidow_clip_out *out;
	wl_list_for_each(out, &sel->out, link) {
		waiting_x11++;
	}
	struct vidow_clip_waiter *w;
	wl_list_for_each(w, &sel->waiters, link) {
		waiting_wayland++;
	}
	struct vidow_clip_pull *pull;
	wl_list_for_each(pull, &sel->pulls, link) {
		pulling++;
	}

	jb_obj(b);
	jb_kv_str(b, "selection", sel->name);
	jb_kv_str(b, "origin", sel->nmimes == 0 ? "none" :
		(sel->from_x11 ? "x11" : "wayland"));
	jb_kv_str(b, "reason", what);
	jb_kv_bool(b, "enabled", server->config.clipboard);
	jb_kv_bool(b, "bridging", clip->conn != NULL && clip->active);
	jb_kv_uint(b, "x_owner", sel->x_owner);
	jb_kv_bool(b, "x_owner_is_ours", sel->x_owner != XCB_WINDOW_NONE &&
		sel->x_owner == clip->window);
	jb_kv_bool(b, "x_owner_is_xwm_proxy",
		sel->x_owner == clip->proxy_clipboard ||
		sel->x_owner == clip->proxy_primary);
	jb_kv_bool(b, "publishing", sel->published != NULL ||
		sel->published_primary != NULL);
	jb_kv_bool(b, "fetching_x11", sel->fetch.active);
	jb_kv_uint(b, "bytes", bytes);
	jb_kv_int(b, "types", sel->nmimes);
	jb_kv_int(b, "types_complete", complete);
	jb_kv_int(b, "types_overflow", overflow);
	jb_kv_int(b, "waiting_x11", waiting_x11);
	jb_kv_int(b, "waiting_wayland", waiting_wayland);
	jb_kv_int(b, "pulling", pulling);
	jb_kv_int(b, "max_bytes", server->config.clipboard_max_bytes);
	jb_key(b, "types_list");
	jb_arr(b);
	for (int i = 0; i < sel->nmimes; ++i) {
		jb_obj(b);
		jb_kv_str(b, "mime", sel->mimes[i].mime);
		jb_kv_uint(b, "size", sel->mimes[i].len);
		jb_kv_bool(b, "complete", sel->mimes[i].complete);
		jb_kv_bool(b, "overflow", sel->mimes[i].overflow);
		jb_obj_end(b);
	}
	jb_arr_end(b);
	jb_obj_end(b);
}

void vidow_ipc_json_focused(struct jbuf *b, struct vidow_server *server) {
	jb_obj(b);
	jb_key(b, "window");
	vidow_ipc_json_window(b, server, server->focused);
	jb_key(b, "window_id");
	if (server->focused != NULL) {
		jb_uint(b, server->focused->id);
	} else {
		jb_null(b);
	}
	jb_kv_str(b, "output", server->focused_output != NULL ?
		server->focused_output->name : NULL);
	jb_key(b, "workspace");
	struct vidow_workspace *ws = vidow_workspace_shown(server);
	jb_str(b, ws != NULL ? ws->name : NULL);
	jb_obj_end(b);
}

/* --------------------------------------------------------------- 全量 */

void vidow_ipc_json_state(struct jbuf *b, struct vidow_server *server) {
	jb_obj(b);
	jb_kv_str(b, "version", "vidow 0.1.0");
	jb_kv_int(b, "pid", (long long)getpid());
	jb_kv_int(b, "time_ms", now_ms());
	jb_kv_str(b, "socket", server->ipc_path);
	jb_kv_str(b, "wayland_display", getenv("WAYLAND_DISPLAY"));
	jb_kv_str(b, "log_level", getenv("VIDOW_LOG"));
	jb_key(b, "config_lua");
	{
		/* 配置文件就是这一份 Lua（$VIDOW_CONFIG / $XDG_CONFIG_HOME 都可能改过
		 * 位置，所以报的是真正算出来的路径，不是环境变量本身）。 */
		char path[PATH_MAX];
		vidow_config_path(path, sizeof(path));
		jb_obj(b);
		jb_kv_bool(b, "loaded", server->script != NULL);
		jb_kv_str(b, "path", path);
		/* 盯上了才会自动重载（inotify 起不来时是 false，那就得手动 SIGHUP） */
		jb_kv_bool(b, "watch", server->configwatch != NULL);
		jb_obj_end(b);
	}

	int mapped = 0;
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->mapped) {
			mapped++;
		}
	}
	jb_key(b, "counts");
	jb_obj(b);
	jb_kv_int(b, "windows", (long long)wl_list_length(&server->views));
	jb_kv_int(b, "windows_mapped", mapped);
	jb_kv_int(b, "columns", (long long)wl_list_length(&server->columns));
	jb_kv_int(b, "outputs", (long long)wl_list_length(&server->outputs));
	jb_kv_int(b, "workspaces", (long long)wl_list_length(&server->workspaces));
	jb_kv_int(b, "layers", (long long)wl_list_length(&server->layers));
	jb_kv_int(b, "keybinds", (long long)wl_list_length(&server->keybinds));
	jb_kv_int(b, "gestures", (long long)wl_list_length(&server->gestures));
	jb_kv_int(b, "rules", (long long)wl_list_length(&server->rules));
	jb_kv_int(b, "clients",
		(long long)wl_list_length(wl_display_get_client_list(server->display)));
	jb_kv_int(b, "animations", (long long)wl_list_length(&server->anims));
	jb_kv_int(b, "subscribers", server->ipc_subscribers);
	jb_obj_end(b);

	jb_key(b, "focused");
	vidow_ipc_json_focused(b, server);
	jb_key(b, "overview");
	vidow_ipc_json_overview(b, server);
	jb_key(b, "mru");
	vidow_ipc_json_mru(b, server);
	jb_key(b, "input");
	vidow_ipc_json_input(b, server);
	jb_key(b, "outputs");
	vidow_ipc_json_outputs(b, server);
	jb_key(b, "workspaces");
	vidow_ipc_json_workspaces(b, server);
	jb_key(b, "windows");
	vidow_ipc_json_windows(b, server);
	jb_key(b, "layers");
	vidow_ipc_json_layers(b, server);
	jb_key(b, "screencast");
	vidow_ipc_json_screencast(b, server);
	jb_key(b, "seats");
	vidow_ipc_json_seats(b, server);
	jb_key(b, "keybinds");
	vidow_ipc_json_keybinds(b, server);
	jb_key(b, "gestures");
	vidow_ipc_json_gestures(b, server);
	jb_key(b, "rules");
	vidow_ipc_json_rules(b, server);
	jb_key(b, "config");
	vidow_ipc_json_config(b, server);
	jb_key(b, "activation_tokens");
	vidow_ipc_json_tokens(b, server);
	jb_key(b, "output_memories");
	vidow_ipc_json_memories(b, server);
	jb_key(b, "anims");
	vidow_ipc_json_anims(b, server);
	jb_key(b, "im");
	vidow_ipc_json_im(b, server);
	jb_key(b, "xwayland");
	vidow_ipc_json_xwayland(b, server);
	jb_key(b, "clipboard");
	vidow_ipc_json_clipboard(b, server, &server->clipboard.clip, "state");
	jb_key(b, "primary_selection");
	vidow_ipc_json_clipboard(b, server, &server->clipboard.primary, "state");
	/* 渲染统计（VIDOW_RENDER_STATS=1 时才计；没开就全 0）。用来从外面量
	 * 「这一块屏多久出一次帧、几乎没损伤的帧占多少」—— 视频 / 光标那种
	 * 「看得见、但没重画多少东西」的帧就藏在这个比例里。 */
	jb_key(b, "render_stats");
	jb_obj(b);
	jb_kv_uint(b, "frames", (unsigned long long)vidow_stats.frames);
	jb_kv_uint(b, "empty_frames",
		(unsigned long long)vidow_stats.damage_empty);
	jb_kv_uint(b, "lifted_frames",
		(unsigned long long)vidow_stats.damage_lifted);
	jb_kv_uint(b, "full_frames", (unsigned long long)vidow_stats.damage_full);
	jb_kv_int(b, "damage_px", (long long)vidow_stats.damage_px);
	jb_kv_int(b, "damage_area", (long long)vidow_stats.damage_area);
	jb_kv_int(b, "from_event", vidow_stats.render_from_event);
	jb_kv_int(b, "from_watchdog", vidow_stats.render_from_watchdog);
	jb_kv_int(b, "from_kick", vidow_stats.render_from_kick);
	jb_kv_int(b, "from_capture", vidow_stats.render_from_capture);
	jb_obj_end(b);
	jb_key(b, "shaders");
	jb_arr(b);
	struct vidow_shader *shader;
	wl_list_for_each(shader, &server->shaders, link) {
		jb_str(b, shader->name);
	}
	jb_arr_end(b);
	jb_obj_end(b);
}

/* 一条历史（GUI 列表 / clipboard-history 命令用）。
 *
 * 挑出来的那份（"mime"）永远是 e->mimes[0]：快照时就是按 mime_rank 从小到大放
 * 进去的（见 clipboard.c 的 hist_snapshot）。"text" 只给前 4KB 预览，要全文或原图
 * 用 clipboard-history-get。"files" 是 text/uri-list 解析出来的（文件复制那一条，
 * GUI 靠它显示文件名、决定点下去是「复制文件」还是「复制文本」）。 */
void vidow_ipc_json_clip_hist_entry(struct jbuf *b, struct vidow_server *server,
		struct vidow_clip_hist_entry *e, int index) {
	(void)server;
	jb_obj(b);
	jb_kv_uint(b, "id", e->id);
	jb_kv_int(b, "index", index);
	jb_kv_str(b, "selection", e->primary ? "primary" : "clipboard");
	jb_kv_str(b, "origin", e->from_x11 ? "x11" : "wayland");
	jb_kv_int(b, "age_ms", (long long)(now_ms() - e->time_ms));
	jb_kv_uint(b, "bytes", e->bytes);
	jb_kv_int(b, "types", e->nmimes);
	jb_key(b, "mimes");
	jb_arr(b);
	int uri = -1;
	for (int i = 0; i < e->nmimes; ++i) {
		jb_str(b, e->mimes[i].mime);
		if (strcmp(e->mimes[i].mime, "text/uri-list") == 0) {
			uri = i;
		}
	}
	jb_arr_end(b);

	struct vidow_clip_mime *pick = e->nmimes > 0 ? &e->mimes[0] : NULL;
	jb_kv_str(b, "mime", pick != NULL ? pick->mime : NULL);
	jb_kv_uint(b, "size", pick != NULL ? pick->len : 0);
	if (pick != NULL && vidow_clipboard_mime_is_text(pick->mime)) {
		size_t n = pick->len > 4096 ? 4096 : pick->len;
		while (n > 0 && pick->data[n - 1] == '\0') {
			n--;
		}
		char *text = malloc(n + 1);
		if (text != NULL) {
			memcpy(text, pick->data, n);
			text[n] = '\0';
			jb_kv_str(b, "text", text);
			free(text);
		}
	}
	jb_kv_bool(b, "is_file", uri >= 0);
	jb_key(b, "files");
	jb_arr(b);
	if (uri >= 0) {
		/* uri-list 的行分隔是 CRLF（RFC 2483），'#' 开头是注释行。 */
		const unsigned char *data = e->mimes[uri].data;
		size_t len = e->mimes[uri].len, start = 0;
		for (size_t i = 0; i <= len; ++i) {
			if (i < len && data[i] != '\n') {
				continue;
			}
			size_t end = i;
			while (end > start && (data[end - 1] == '\r' ||
					data[end - 1] == '\n')) {
				end--;
			}
			if (end > start && data[start] != '#') {
				char *line = malloc(end - start + 1);
				if (line != NULL) {
					memcpy(line, data + start, end - start);
					line[end - start] = '\0';
					jb_str(b, line);
					free(line);
				}
			}
			start = i + 1;
		}
	}
	jb_arr_end(b);
	jb_obj_end(b);
}

/* --------------------------------------------------------------- 事件 */

/* 一个事件的信封：{"event":..., "time_ms":..., "data":{...}} 加换行。 */
static void emit(struct vidow_server *server, const char *event,
		void (*payload)(struct jbuf *, struct vidow_server *, void *),
		void *data) {
	struct jbuf b;
	jb_init(&b, 1024);
	jb_obj(&b);
	jb_kv_str(&b, "event", event);
	jb_kv_int(&b, "time_ms", now_ms());
	jb_key(&b, "data");
	if (payload != NULL) {
		payload(&b, server, data);
	} else {
		jb_obj(&b);
		jb_obj_end(&b);
	}
	jb_obj_end(&b);
	jb_raw(&b, "\n");
	if (jb_ok(&b)) {
		/* 一条事件两条路：IPC 订阅者拿 JSON，配置里的钩子（vidow.on）拿同一份
		 * JSON 的文本（见 hooks.c —— 那边只入队，执行在 idle 里）。 */
		vidow_ipc_broadcast(server, event, b.buf, b.len);
		vidow_hooks_dispatch(server, event, b.buf);
	}
	jb_free(&b);
}

/* 自定义事件：data 原样带上（IPC 的 emit 命令）。 */
void vidow_ipc_emit_custom(struct vidow_server *server, const char *event,
		const struct jval *data) {
	if (server == NULL || event == NULL || event[0] == '\0') {
		return;
	}
	if (!vidow_event_wanted(server, event)) {
		return;
	}
	struct jbuf b;
	jb_init(&b, 512);
	jb_obj(&b);
	jb_kv_str(&b, "event", event);
	jb_kv_int(&b, "time_ms", now_ms());
	jb_key(&b, "data");
	if (data != NULL && (data->t == J_OBJ || data->t == J_ARR ||
			data->t == J_STR || data->t == J_NUM || data->t == J_BOOL ||
			data->t == J_NULL)) {
		jval_json(&b, data);
	} else {
		jb_obj(&b);
		jb_obj_end(&b);
	}
	jb_obj_end(&b);
	jb_raw(&b, "\n");
	if (jb_ok(&b)) {
		vidow_ipc_broadcast(server, event, b.buf, b.len);
		vidow_hooks_dispatch(server, event, b.buf);
	}
	jb_free(&b);
}

static void payload_window(struct jbuf *b, struct vidow_server *server,
		void *data) {
	vidow_ipc_json_window(b, server, data);
}

static void payload_workspace(struct jbuf *b, struct vidow_server *server,
		void *data) {
	vidow_ipc_json_workspace(b, server, data);
}

static void payload_layer(struct jbuf *b, struct vidow_server *server,
		void *data) {
	vidow_ipc_json_layer(b, server, data);
}

static void payload_output(struct jbuf *b, struct vidow_server *server,
		void *data) {
	struct wlr_output *o = data;
	struct vidow_output *found = NULL;
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->wlr_output == o) {
			found = output;
			break;
		}
	}
	if (found != NULL) {
		vidow_ipc_json_output(b, server, found);
		return;
	}
	/* 已经在拆了（output-removed）：只有名字可报。 */
	jb_obj(b);
	jb_kv_str(b, "name", o->name);
	jb_obj_end(b);
}

void vidow_ipc_emit_window(struct vidow_server *server, const char *event,
		struct vidow_view *view) {
	if (!vidow_event_wanted(server, event) || view == NULL) {
		return;
	}
	emit(server, event, payload_window, view);
}

struct closed_info {
	uint64_t id;
	const char *title;
	const char *app_id;
};

static void payload_closed(struct jbuf *b, struct vidow_server *server,
		void *data) {
	struct closed_info *info = data;
	(void)server;
	jb_obj(b);
	jb_kv_uint(b, "id", info->id);
	jb_kv_str(b, "title", info->title);
	jb_kv_str(b, "app_id", info->app_id);
	jb_obj_end(b);
}

void vidow_ipc_emit_window_closed(struct vidow_server *server, uint64_t id,
		const char *title, const char *app_id) {
	if (!vidow_event_wanted(server, "window-closed")) {
		return;
	}
	struct closed_info info = { .id = id, .title = title, .app_id = app_id };
	emit(server, "window-closed", payload_closed, &info);
}

static void payload_background(struct jbuf *b, struct vidow_server *server,
		void *data) {
	(void)data;
	vidow_ipc_json_background(b, server);
}

void vidow_ipc_emit_background(struct vidow_server *server, const char *event) {
	if (!vidow_event_wanted(server, event)) {
		return;
	}
	emit(server, event, payload_background, NULL);
}

static void payload_focus(struct jbuf *b, struct vidow_server *server,
		void *data) {
	struct vidow_view *view = data;
	jb_obj(b);
	jb_key(b, "window");
	vidow_ipc_json_window(b, server, view);
	jb_key(b, "window_id");
	if (view != NULL) {
		jb_uint(b, view->id);
	} else {
		jb_null(b);
	}
	jb_kv_str(b, "output", server->focused_output != NULL ?
		server->focused_output->name : NULL);
	jb_obj_end(b);
}

void vidow_ipc_emit_focus(struct vidow_server *server, struct vidow_view *view) {
	if (!vidow_event_wanted(server, "window-focus-changed")) {
		return;
	}
	emit(server, "window-focus-changed", payload_focus, view);
}

void vidow_ipc_emit_workspace(struct vidow_server *server, const char *event,
		struct vidow_workspace *ws) {
	if (!vidow_event_wanted(server, event) || ws == NULL) {
		return;
	}
	emit(server, event, payload_workspace, ws);
}

void vidow_ipc_emit_output(struct vidow_server *server, const char *event,
		struct wlr_output *output) {
	if (!vidow_event_wanted(server, event) || output == NULL) {
		return;
	}
	emit(server, event, payload_output, output);
}

struct clip_ev {
	struct vidow_clip_selection *sel;
	const char *what;
};

static void payload_clipboard(struct jbuf *b, struct vidow_server *server,
		void *data) {
	struct clip_ev *ev = data;
	vidow_ipc_json_clipboard(b, server, ev->sel, ev->what);
}

void vidow_ipc_emit_clipboard(struct vidow_server *server,
		struct vidow_clip_selection *sel, const char *what) {
	if (!vidow_event_wanted(server, sel != NULL && sel->primary ?
			"primary-changed" : "clipboard-changed") || sel == NULL) {
		return;
	}
	struct clip_ev ev = {.sel = sel, .what = what};
	emit(server, sel->primary ? "primary-changed" : "clipboard-changed",
		payload_clipboard, &ev);
}

/* 历史变了（增 / 删 / 清 / 同一条挪到最前）。payload 只报条数和字节数 —— 列表本身
 * 由客户端收到事件后自己 clipboard-history 拉一遍（省得每条事件都带几十 KB 预览）。 */
static void payload_clip_hist(struct jbuf *b, struct vidow_server *server,
		void *data) {
	jb_obj(b);
	jb_kv_str(b, "reason", data);
	jb_kv_int(b, "count", server->clipboard.hist_count);
	jb_kv_uint(b, "bytes", server->clipboard.hist_bytes);
	jb_obj_end(b);
}

void vidow_ipc_emit_clipboard_history(struct vidow_server *server,
		const char *what) {
	if (server == NULL ||
			!vidow_event_wanted(server, "clipboard-history-changed")) {
		return;
	}
	emit(server, "clipboard-history-changed", payload_clip_hist,
		(void *)what);
}

void vidow_ipc_emit_layer(struct vidow_server *server, const char *event,
		struct vidow_layer_surface *ls) {
	if (!vidow_event_wanted(server, event) || ls == NULL) {
		return;
	}
	emit(server, event, payload_layer, ls);
}

static void payload_column(struct jbuf *b, struct vidow_server *server,
		void *data) {
	vidow_ipc_json_column(b, server, data);
}

void vidow_ipc_emit_column(struct vidow_server *server, const char *event,
		struct vidow_column *col) {
	if (!vidow_event_wanted(server, event) || col == NULL) {
		return;
	}
	emit(server, event, payload_column, col);
}

struct simple_ev {
	const struct vidow_event_field *fields;
};

static void payload_simple(struct jbuf *b, struct vidow_server *server,
		void *data) {
	(void)server;
	struct simple_ev *ev = data;
	jb_obj(b);
	for (const struct vidow_event_field *f = ev->fields;
			f != NULL && f->key != NULL; f++) {
		switch (f->type) {
		case VIDOW_EV_NUM:
			jb_kv_int(b, f->key, f->num);
			break;
		case VIDOW_EV_BOOL:
			jb_kv_bool(b, f->key, f->boolean);
			break;
		case VIDOW_EV_TEXT:
		default:
			jb_kv_str(b, f->key, f->text);
			break;
		}
	}
	jb_obj_end(b);
}

void vidow_ipc_emit_simple(struct vidow_server *server, const char *event,
		const struct vidow_event_field *fields) {
	if (!vidow_event_wanted(server, event) || fields == NULL) {
		return;
	}
	struct simple_ev ev = { .fields = fields };
	emit(server, event, payload_simple, &ev);
}

void vidow_ipc_emit_note(struct vidow_server *server, const char *event) {
	if (!vidow_event_wanted(server, event)) {
		return;
	}
	emit(server, event, NULL, NULL);
}
