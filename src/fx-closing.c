/* 关窗动画：把窗口最后那一帧冻成快照，再让它缩着淡出。
 *
 * 和窗口模糊是两条路：关闭快照不再跟着世界变（客户端已经销毁了，surface 也
 * 可能已经没了），所以它不抓底衬、不重跑模糊链，只把快照贴上去。它借的是
 * 同一套装饰着色器（fx-gl.c 的 decor_draw / begin_pass）。
 *
 * 生命周期只有「开 → 每帧推 t → 动画结束释放」，没有缓存可命中的东西。 */
#include "server.h"
#include "fx-internal.h"
#include "fx-gl.h"
#include "xtrace.h"

#include <math.h>
#include <stdlib.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

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
	 * 整屏都算的话别人的底衬会跟着每帧重抓。
	 *
	 * 盒子要盖住**整段动画范围**：快照往下滑，底缘每帧新露出来的那条在原始盒
	 * 外面；回弹把缩放顶过 1 时四周也会探出几像素。局部重抓（
	 * region_update_rect 只补账本里有的盒子，账本每次重抓后就清空）补不到
	 * 没记的那块 —— 别人底衬里这条带就停在「快照还没盖到」的上一版。 */
	{
		struct wlr_box box = {
			.x = closing->x, .y = closing->y,
			.width = closing->w, .height = closing->h,
		};
		int slack = closing->slide_px + 24;
		box.x -= slack;
		box.y -= slack;
		box.width += 2 * slack;
		box.height += 2 * slack;
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
