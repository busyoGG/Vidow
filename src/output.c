#include "server.h"

#include <drm_fourcc.h> /* DRM_FORMAT_XRGB8888 / XRGB2101010（输出缓冲的位深） */
#include <pixman.h>      /* damage_ring 的损伤区域（渲染统计探针） */
#include <strings.h> /* strcasecmp（方向的名字） */

#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h> /* access()：统计探针的文件开关 */

#include "ipc.h"
#include "workspace.h"
#include "sessionlock.h"

#include <wlr/backend/headless.h>
#include <wlr/util/log.h>

/* 环境变量只问一次。
 *
 * 这两个变量以前是**每帧**问一遍（渲染统计两次、帧追踪钩子一次），而 getenv 要
 * 遍历 environ 做 strcmp —— 实测 profile 里它占 0.39%，比我们自己一帧的模糊代码
 * （vidow_blur_update 0.26%）还显眼。进程起来之后环境不会变，缓存住就行。 */
/* 文件开关：/tmp/vidow-stats-on 一出现就当 VIDOW_RENDER_STATS=1，而且统计行用
 * INFO 级打出去（真会话的日志只收 INFO 以上；环境变量那条路是 DEBUG，只有
 * --debug 跑的时候才看得见）。这样不用改登录环境就能在真实会话里量基线。
 *
 * 每 500ms 才 stat 一次：这个函数一帧要问好几遍，而 stat 也是 syscall。 */
static bool render_stats_file_on(void) {
	static int64_t last_ns = 0;
	static bool on = false;
	int64_t now = vidow_now_ns();
	if (last_ns == 0 || now - last_ns > 500 * 1000000LL) {
		last_ns = now;
		on = access("/tmp/vidow-stats-on", F_OK) == 0;
	}
	return on;
}

static bool render_stats_on(void) {
	static int on = -1;
	if (on < 0) {
		on = getenv("VIDOW_RENDER_STATS") != NULL ? 1 : 0;
	}
	return on != 0 || render_stats_file_on();
}

/* 卡顿探针：VIDOW_STUTTER=1 开，VIDOW_STUTTER_MS 改阈值（默认 100ms）。
 * 只问一次环境变量（和上面那两个探针一个规矩）。 */
static bool stutter_on(void) {
	static int done = 0, on = 0;
	if (!done) {
		on = getenv("VIDOW_STUTTER") != NULL ? 1 : 0;
		done = 1;
	}
	return on != 0;
}

static int64_t stutter_threshold_ns(void) {
	static int64_t ns = -1;
	if (ns < 0) {
		const char *v = getenv("VIDOW_STUTTER_MS");
		int ms = v != NULL ? atoi(v) : 0;
		ns = (int64_t)(ms > 0 ? ms : 100) * 1000000LL;
	}
	return ns;
}

/* 每帧重烘探针：VIDOW_PUMP_TRACE=1 开。
 *
 * 和上面几个一个规矩 —— **只问一次**。这条以前写在「每帧 × 每个窗口」的
 * 循环体里，原神那种 160fps 的满速提交就是每秒上千次 environ 线性扫描。 */
static bool pump_trace_on(void) {
	static int done = 0, on = 0;
	if (!done) {
		on = getenv("VIDOW_PUMP_TRACE") != NULL ? 1 : 0;
		done = 1;
	}
	return on != 0;
}

/* 帧事件掐断的测试钩子（VIDOW_TEST_STALL_OUTPUT=<屏名>），同样只问一次。 */
static const char *stall_output_name(void) {
	static int done = 0;
	static const char *name = NULL;
	if (!done) {
		name = getenv("VIDOW_TEST_STALL_OUTPUT");
		done = 1;
	}
	return name;
}

/* Advance the animation clock and present one frame. */
void vidow_output_render(struct vidow_output *output) {
	struct vidow_server *server = output->server;

	/* **关着的屏不画**。少了这条，`output-power off` 之后看门狗 / 动画定时器
	 * 还在每帧调进来，wlr_scene_output_commit() 去给一块没开的屏建交换链 ——
	 * 失败后 wlroots 每帧刷一行
	 *   Swapchain for output 'DP-3' failed test
	 * 实测两屏都关掉之后刷了上千行，而且把 DRM 后端拖进坏状态：之后按键唤醒
	 * （key_press_enables_dpms）只开回来一块屏，另一块的 commit 直接失败。
	 * 关着的屏本来就不该产生帧。 */
	if (!output->wlr_output->enabled) {
		return;
	}

	/* 出帧了：看门狗按这个时刻给自己限速（见 watchdog_interval_ns）。放在最前面，
	 * 中途哪条早退都不影响「这一帧确实画过了」。 */
	output->last_render_ns = vidow_now_ns();

	server->output_w = output->wlr_output->width;
	server->output_h = output->wlr_output->height;

	/* 装饰着色器工作在「输出的物理像素」空间里（散景贴图也是物理分辨率），
	 * 所以窗口位置要减掉本输出的布局原点、再乘缩放。多输出下这是必须的：
	 * 少了原点，第二块屏上的模糊会按第一块屏的坐标采样。 */
	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
	/* 顺手记一份：拔线时 wlr_output_layout 可能已经先把这块屏忘掉了，
	 * 热插拔要靠这份存着的位置算「挪到别的屏」的平移量。
	 * 位置真的变了就打一行：拔掉一块屏时 wlroots 会重新摆放剩下的输出
	 * （实测：拔掉左边那块，右边那块从 x=1280 挪到了 x=0），这件事用户和
	 * 测试都需要看得见。 */
	if (box.x != output->box.x || box.y != output->box.y ||
			box.width != output->box.width ||
			box.height != output->box.height) {
		if (output->box.width > 0) {
			wlr_log(WLR_DEBUG, "output %s: 布局位置 %d,%d %dx%d -> %d,%d %dx%d",
				output->wlr_output->name, output->box.x, output->box.y,
				output->box.width, output->box.height, box.x, box.y,
				box.width, box.height);
		}
		output->box = box;
		/* 会话锁的黑幕跟着这块屏的布局框走（位置/尺寸都变了）。 */
		vidow_sessionlock_output_update(output);
	}
	double scale = output->wlr_output->scale;
	server->output_x = (int)lround(box.x * scale);
	server->output_y = (int)lround(box.y * scale);

	/* 探针：拖拽的「指针事件 -> 出帧」延迟（这一帧要出帧了，把从最后一个事件到现在
	 * 的时间记一笔）。嵌套在别的合成器里时，光标是**上层**画的、窗口是我们画的，
	 * 这个延迟直接决定「跟不跟手」。 */
	if (render_stats_on() && server->grab_ev_ns != 0 &&
			(server->grab_dirty || server->arrange_dirty)) {
		int64_t lat = vidow_now_ns() - server->grab_ev_ns;
		vidow_stats.grab_lat_ns += lat;
		if (lat > vidow_stats.grab_lat_max_ns) {
			vidow_stats.grab_lat_max_ns = lat;
		}
		vidow_stats.grab_lat_n++;
		server->grab_ev_ns = 0;
	}

	/* overview 开着的话先把变换算好、把窗口按它摆一遍（缩放里每帧都要摆，
	 * 因为客户端 commit 会把场景尺寸改回去）。 */
	vidow_overview_update(server);

	/* 浮动窗口拖拽：一帧里来过的那些移动事件在这里一次性重排（位置取最后一个）。 */
	if (server->arrange_dirty) {
		server->arrange_dirty = false;
		vidow_arrange(server);
	}

	/* 拖动跟手：一帧里来过的那些移动事件在这里一次性摆到位（位置取最后一个）。
	 * 中间那些位置屏幕上从来没出现过，所以「从上一帧画在哪到这一帧画在哪」记一笔
	 * 损伤就够 —— 拖得快的时候一帧能跨过一大截，只记新位置会漏。 */
	if (server->grab_dirty && server->grab_view != NULL) {
		struct vidow_view *gv = server->grab_view;
		server->grab_dirty = false;
		/* 果冻画的是整窗 + 四周形变余量，比窗口框大一圈：上一帧那块按果冻
		 * 实际占过的范围记（grab_last_box 存的是窗口框，扩在用的地方）。
		 * 漏了这圈，松开、收尾之后留白 / 投影那一圈没人擦。 */
		struct wlr_box old = server->grab_last_box;
		int jpad = (int)ceil(vidow_jelly_pad(gv)) + 8;
		old.x -= jpad;
		old.y -= jpad;
		old.width += 2 * jpad;
		old.height += 2 * jpad;
		vidow_backdrop_damage_box(server, &old);
		/* 只有这个窗口自己动了：记在它头上，它自己的私有底衬（抓的时候它藏着）
		 * 不该因为这一笔就过期。 */
		vidow_backdrop_damage_view(gv);
		vidow_view_apply_anim(gv);
		server->grab_last_box = vidow_view_screen_box(gv);
	}

	/* 平铺拖拽的**贴边自动滚条带**（niri 的 Dnd 手感）：光标压在工作区左右边缘
	 * 那条带里就按秒速推视口 —— 放在这里是因为「停着不动也要继续滚」，只有每帧
	 * 才推得动。滚了的话它自己会再要一帧，所以压着边缘不动就能一直滚下去，直到
	 * 光标离开边缘或者松手。 */
	vidow_tile_drag_scroll_step(server);

	/* 先把动画推进到**这一帧**的位置，再抓底衬。
	 *
	 * 顺序反了就是「模糊慢半拍」：vidow_blur_update() 按上一帧的 anim_off
	 * 抓的区域，渲染却用这一帧的新位置，窗口动一下、底衬就落后一帧 —— 看起来
	 * 像背景跟着窗口一起弹。所有依赖 anim_off 的东西（区域原点、UV）必须和
	 * 这一帧画出来的位置同一个时刻。 */
	int64_t st_t0 = vidow_now_ns();
	/* **卡顿探针**（VIDOW_STUTTER=1）：逐帧看「上一帧到现在隔了多久」。
	 *
	 * 为什么不能只看渲染统计：那个每 500ms 聚合一次，一次 300ms 的卡顿被
	 * 平均成「60fps 正常」，看不出来。用户报的「突然卡一下」正是这种 ——
	 * 单帧异常，平均无感。
	 *
	 * 这里只报超阈值的帧，附带上一帧各阶段耗时（能区分「GPU 慢」和
	 * 「主循环根本没被唤醒」：后者说明卡在别处，不是渲染）。 */
	if (stutter_on()) {
		static int64_t prev_ns = 0;
		if (prev_ns != 0) {
			int64_t gap_ns = st_t0 - prev_ns;
			if (gap_ns >= stutter_threshold_ns()) {
				wlr_log(WLR_INFO,
					"卡顿: 距上一帧 %.1fms（阈值 %.0fms）"
					" | 上帧: 果冻 %.2f 模糊 %.2f 装饰 %.2f 提交 %.2f "
					"| 帧数 %d 趟 %d 重烘 %d",
					(double)gap_ns / 1e6,
					(double)stutter_threshold_ns() / 1e6,
					(double)vidow_stats.jelly_ns / 1e6,
					(double)vidow_stats.blur_ns / 1e6,
					(double)vidow_stats.decor_ns / 1e6,
					(double)vidow_stats.commit_ns / 1e6,
					vidow_stats.frames, vidow_stats.passes,
					vidow_stats.bakes);
			}
		}
		prev_ns = st_t0;
	}
	vidow_anim_tick(server);
	/* 动画**最后一帧**推完就把自己从链表里摘了（anim.c），于是下面第 599 行
	 * 那个「动画还在跑就再要一帧」的判据当场失效 —— 不会再有任何帧来消费
	 * 它刚才挂上的 arrange_dirty。上面的 arrange_dirty（第 109 行）在本帧
	 * 开头就消费过了，赶不上这一次。
	 *
	 * 症状：resize 补间的最后一帧不落屏，窗口停在倒数第二帧的宽度上，
	 * 切一下窗口（来了一帧新的）才补上。
	 *
	 * 在 tick 之后再消费一次，让动画这一帧算出来的布局当帧生效。 */
	if (server->arrange_dirty) {
		server->arrange_dirty = false;
		vidow_arrange(server);
	}
	/* 拖窗口的果冻：积分弹簧、把弯了的窗口重画一遍（见 jelly.c）。 */
	vidow_jelly_tick(server, vidow_now_ns());
	int64_t st_t1 = vidow_now_ns();

	/* 临时探针（VIDOW_RENDER_STATS=1）：每帧各阶段耗时 + 贴图导入/重建次数 */
	vidow_blur_update(output);
	int64_t st_t2 = vidow_now_ns();

	/* A blurred decoration is only redrawn when this frame's backdrop differs
	 * from the one already baked into its buffer. On a still screen nothing
	 * moves behind the windows, so this loop does no work at all. */
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->fx.active && view->fx.blur_drawn != view->fx.blur_ver) {
			if (getenv("VIDOW_DMG_WHY") != NULL) {
				static int64_t rb_last;
				int64_t rb_now = vidow_now_ns();
				if (rb_now - rb_last > 200000000LL) {
					rb_last = rb_now;
					wlr_log(WLR_INFO, "REBAKE %s drawn=%u ver=%u",
						vidow_view_title(view) != NULL ?
							vidow_view_title(view) : "(无题)",
						view->fx.blur_drawn, view->fx.blur_ver);
				}
			}
			if (pump_trace_on()) {
				wlr_log(WLR_INFO, "PUMPTRACE 每帧重烘: %s drawn=%u ver=%u "
					"rev=%u",
					vidow_view_title(view) != NULL ?
						vidow_view_title(view) : "(无题)",
					view->fx.blur_drawn, view->fx.blur_ver,
					view->fx.bake_revision);
			}
			vidow_view_apply_anim(view);
		}
	}
	int64_t st_t3 = vidow_now_ns();

	/* 屏幕这一帧就是屏幕这一帧：抓帧是 vidow 自己另外渲染的一遍（capture.c /
	 * screencopy.c），和这里没有任何关系 —— capture = "hide" 的窗口 / layer
	 * 屏幕上照常显示，只在抓帧里没有。 */
	/* 这一帧所有离屏趟（模糊链 / 装饰 / 果冻）的命令在这里一次性提交，
	 * 而不是每趟一次 —— 见 fx.c 的 end_pass。 */
	vidow_fx_flush(server);
	/* **真正把这些命令提交到屏幕上**。
	 *
	 * 这行之前被删过：删掉之后 vidow_output_render() 只剩发 frame_done，
	 * 输出从来不 commit —— wlr_output 的 events.commit 不响，
	 * screencopy 的 frame_handle_output_commit 一次都不跑，抓帧客户端
	 * （wf-recorder / 远程工具）拿不到 ready：录出来的文件 0 字节，
	 * 前端一直「未起播」。屏幕看着正常是因为别处还有提交路径兜着，
	 * 但抓帧这条依赖 commit 信号的链路就断了。 */
	/* 探针：这一帧到底重画了多大面积（见 struct vidow_frame_stats 的
	 * damage_px 说明）。damage_ring.current 是「自上次 commit 以来累积的
	 * 损伤」，commit 内部会把 current rotate 到 buffer damage 再清空 ——
	 * 所以必须在这行之前读，之后读到的永远是空的。 */
	if (render_stats_on() && output->scene_output != NULL) {
		pixman_region32_t *dmg = &output->scene_output->damage_ring.current;
		int ndmg = 0;
		pixman_box32_t *rects = pixman_region32_rectangles(dmg, &ndmg);
		int64_t px = 0;
		for (int i = 0; i < ndmg; i++) {
			int64_t dw = (int64_t)rects[i].x2 - rects[i].x1;
			int64_t dh = (int64_t)rects[i].y2 - rects[i].y1;
			if (dw > 0 && dh > 0) {
				px += dw * dh;
			}
		}
		int64_t full = (int64_t)output->wlr_output->width *
			(int64_t)output->wlr_output->height;
		if (getenv("VIDOW_DMG_WHY") != NULL) {
			static int dmps;
			dmps++;
			if (dmps > 200 && dmps % 40 == 0) {
				char rb[512];
				size_t ro = 0;
				for (int i = 0; i < ndmg && ro < sizeof(rb) - 40; i++) {
					ro += snprintf(rb + ro, sizeof(rb) - ro, "[%d,%d %dx%d]",
						rects[i].x1, rects[i].y1,
						rects[i].x2 - rects[i].x1,
						rects[i].y2 - rects[i].y1);
				}
				rb[ro] = '\0';
				wlr_log(WLR_INFO, "DMGRECT %s n=%d %s", output->wlr_output->name,
					ndmg, rb);
			}
		}
		vidow_stats.damage_px += px;
		vidow_stats.damage_area += full;
		vidow_stats.damage_frames++;
		if (full > 0 && px >= full) {
			vidow_stats.damage_full++;
		}
	}
	wlr_scene_output_commit(output->scene_output, NULL);
	int64_t st_t4 = vidow_now_ns();
	if (render_stats_on()) {
		vidow_stats.jelly_ns += st_t1 - st_t0;
		vidow_stats.blur_ns += st_t2 - st_t1;
		vidow_stats.decor_ns += st_t3 - st_t2;
		vidow_stats.commit_ns += st_t4 - st_t3;
		vidow_stats.frames++;
		static int64_t win_ns = 0;
		if (win_ns == 0) {
			win_ns = st_t0;
		}
		if (st_t4 - win_ns >= 500 * 1000000LL) {
			double f = vidow_stats.frames > 0 ? (double)vidow_stats.frames : 1.0;
			wlr_log(render_stats_file_on() ? WLR_INFO : WLR_DEBUG,
				"渲染统计: %d 帧/帧窗(%.0fms) %.1f fps | 模糊 %.2fms/帧"
				"(shared %d own %d under %d chain %d) | 装饰 %.2fms/帧(%d 次重烘) | "
				"果冻 %.2fms/帧 | 提交 %.2fms/帧 | 贴图 +%d/-%d | 趟 %d | "
				"重烘 %.2fms/帧 | 拖拽延迟 %.2f/%.2fms(%d) | "
				"损伤 %.1f%%/帧(整屏 %d/%d 帧) | 源 事件%d 狗%d 踢%d 抓%d | 标脏 bump%d note%d",
				vidow_stats.frames, (double)(st_t4 - win_ns) / 1e6,
				1000.0 * f / ((double)(st_t4 - win_ns) / 1e6),
				(double)vidow_stats.blur_ns / 1e6 / f,
				vidow_stats.shared, vidow_stats.own, vidow_stats.under,
				vidow_stats.chains,
				(double)vidow_stats.decor_ns / 1e6 / f, vidow_stats.bakes,
				(double)vidow_stats.jelly_ns / 1e6 / f,
				(double)vidow_stats.commit_ns / 1e6 / f,
				vidow_stats.tex_new, vidow_stats.tex_del, vidow_stats.passes,
				(double)vidow_stats.bake_ns / 1e6 / f,
				vidow_stats.grab_lat_n > 0 ?
					(double)vidow_stats.grab_lat_ns / 1e6 / vidow_stats.grab_lat_n : 0.0,
				(double)vidow_stats.grab_lat_max_ns / 1e6,
				vidow_stats.grab_lat_n,
				vidow_stats.damage_area > 0 ?
					100.0 * (double)vidow_stats.damage_px /
						(double)vidow_stats.damage_area : 0.0,
				vidow_stats.damage_full, vidow_stats.damage_frames,
				vidow_stats.render_from_event, vidow_stats.render_from_watchdog,
				vidow_stats.render_from_kick, vidow_stats.render_from_capture,
				vidow_stats.dmg_bumps, vidow_stats.dmg_notes);
			vidow_fx_dump_blur_ver_hits();
			/* 影子调度：这一窗口里最早的那个「该醒时刻」离现在我们有多远。
			 * 它和实际定时器唤醒时间对得上，才说明「四合一」不会漏来源。 */
			if (vidow_stats.sched_notes > 0) {
				struct vidow_output *so;
				int64_t best = 0;
				const char *why = "-";
				int64_t now2 = vidow_now_ns();
				wl_list_for_each(so, &server->outputs, link) {
					if (so->sched_deadline_ns != 0 &&
							(best == 0 || so->sched_deadline_ns < best)) {
						best = so->sched_deadline_ns;
						why = so->sched_why != NULL ? so->sched_why : "-";
					}
				}
				double fired = vidow_stats.sched_fired > 0 ?
					(double)vidow_stats.sched_fired : 1.0;
				wlr_log(WLR_DEBUG,
					"影子调度: 登记 %llu 次 / 结算 %llu 次，平均晚 %.2fms（最多 %.2fms）早 %.2fms｜最近截止 %.1fms 后（%s）",
					(unsigned long long)vidow_stats.sched_notes,
					(unsigned long long)vidow_stats.sched_fired,
					(double)vidow_stats.sched_late_ns / 1e6 / fired,
					(double)vidow_stats.sched_late_max_ns / 1e6,
					(double)vidow_stats.sched_early_ns / 1e6 / fired,
					best != 0 ? (double)(best - now2) / 1e6 : -1.0, why);
			}
			{
				struct vidow_output *so;
				wl_list_for_each(so, &server->outputs, link) {
					so->sched_deadline_ns = 0;
					so->sched_why = NULL;
				}
			}
			/* 重叠判据实测成本：次数 × 平均每次纳秒数。加了它才能回答
			 * 「1700 次/秒到底贵不贵」。 */
			{
				uint64_t ovns = 0, ovn = 0;
				vidow_fx_take_overlap_cost(&ovns, &ovn);
				if (ovn > 0) {
					wlr_log(WLR_DEBUG,
						"重叠判据: %llu 次/%.0fms，共 %.2fms，平均 %.0fns/次",
						(unsigned long long)ovn,
						(double)(st_t4 - win_ns) / 1e6,
						(double)ovns / 1e6,
						(double)ovns / (double)ovn);
				}
			}
			vidow_stats = (struct vidow_frame_stats){0};
			win_ns = st_t4;
		}
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(output->scene_output, &now);
	/* **X11 视图要单独补一次。**
	 *
	 * 上面那句只给「场景认为可见」的节点发回调，而 FX 管线把真实表面的
	 * opacity 强制设成 0（内容由 decor 烘焙图来画），wlroots 于是认为它
	 * 看不见、跳过它 —— XWayland 永远等不到 frame callback，交一帧空白
	 * 之后就停住了（实测 Steam 重开：40 秒只有 1 次 commit，屏幕全黑）。
	 *
	 * 照 Hyprland 的做法：画完一帧就无条件发，不看可见性。 */
	vidow_views_send_x11_frame_done(server, output);
	/* 这块屏画完一帧了：会话锁的锁定握手在这里记账（每块屏都画过一帧才发
	 * locked，见 sessionlock.c）。 */
	vidow_sessionlock_frame(output);

	/* 视口外的窗口要不要保活（窗口规则 / 录屏预览）。放在场景补帧之后：已经画在
	 * 画面上的窗口场景那边已经给过回调了，这里只补没画到的那些。 */
	vidow_view_render_pump(server, output,
		(uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000));

	/* debug：右上角实测帧率（debug_show_refresh，默认关）。放最后——它只读
	 * 本帧统计、不动任何客户端状态。 */
	vidow_debugrate_tick(server, output);
}

/* ---------------------------------------------------------------- 帧追踪
 *
 * VIDOW_TRACE_FRAMES=1 时开：每次「要帧」记一笔（谁要的、要了几次），帧事件回来
 * 就销账；250ms 一轮检查，发现「要了帧但 300ms 没帧事件」就喊一行 —— 那块屏的
 * 帧链断了（后端没给帧事件）。用户报过「某块屏上的窗口停在最后一帧、拖到另一块屏
 * 就活」，那种只在真 DRM 上出现的问题就靠这几行定案。默认不开。 */
struct vidow_output *vidow_output_lookup(struct vidow_server *server,
		struct wlr_output *wlr_output) {
	if (server == NULL || wlr_output == NULL) {
		return NULL;
	}
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->wlr_output == wlr_output) {
			return output;
		}
	}
	return NULL;
}

static int trace_timer_cb(void *data) {
	struct vidow_server *server = data;
	int64_t now = vidow_now_ns();
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		int64_t gap = (now - output->last_frame_ns) / 1000000;
		if (gap < 300) {
			continue;
		}
		bool stale_content = output->content_ns > output->last_frame_ns;
		if (!stale_content && !output->want_frame) {
			continue;
		}
		wlr_log(WLR_ERROR, "trace: %s %lldms 没出帧（vidow 主动要过 %d 帧，"
			"最近一次：%s）%s", output->wlr_output->name, (long long)gap,
			output->want_frame_n,
			output->want_frame_why != NULL ? output->want_frame_why : "没要过",
			stale_content ? "；客户端还在交帧，屏幕却不更新 ← 帧链断了" : "");
		output->want_frame = false; /* 记一次就够，别刷屏 */
		output->content_ns = output->last_frame_ns;
	}
	wl_event_source_timer_update(server->trace_timer, 250);
	return 0;
}

/* 要一帧。记账只为诊断（VIDOW_TRACE_FRAMES=1 时 trace_timer_cb 会用它），
 * 找不到对应的 vidow_output 也照样排帧 —— 诊断不许改变行为。 */
/* 要一帧。记账只为诊断（VIDOW_TRACE_FRAMES=1 时 trace_timer_cb 会用它），
 * 找不到对应的 vidow_output 也照样排帧 —— 诊断不许改变行为。 */
void vidow_output_schedule_frame(struct vidow_server *server,
		struct wlr_output *wlr_output, const char *why) {
	if (wlr_output == NULL) {
		return;
	}
	struct vidow_output *output = vidow_output_lookup(server, wlr_output);
	bool already_queued = false;
	/* 临时探针（VIDOW_FRAME_WHY=1）：谁在每帧要帧。静止桌面 300fps 时这行
	 * 每 200ms 报一次，看 why 就知道是 damage / animation / cursor / timer
	 * 哪一路在推。定位完删掉。 */
	if (getenv("VIDOW_FRAME_WHY") != NULL) {
		static int64_t why_last;
		int64_t why_now = vidow_now_ns();
		if (why_now - why_last > 200000000LL) {
			why_last = why_now;
			wlr_log(WLR_INFO, "帧请求 why=%s out=%s",
				why != NULL ? why : "(null)", wlr_output->name);
		}
	}
	if (output != NULL) {
		output->want_frame = true;
		output->want_frame_n++;
		output->want_frame_why = why;
		output->redraw_requests++;
		output->redraw_reason = why;
		/* 帧调度（见 server.h 的 enum vidow_redraw_state）：一轮里第一次
		 * 要帧才真的向后端排，后面的只记账 —— 一帧里几十个窗口各要一次，
		 * 排一次就够。渲染期间（RENDERING）要帧会置 redraw_again，见
		 * output_frame 的收口。 */
		if (output->redraw == VIDOW_REDRAW_RENDERING) {
			output->redraw_again = true;
		}
		if (output->redraw == VIDOW_REDRAW_QUEUED) {
			/* 已经排过：同一轮里只记账。但**排了 50ms 还没出帧**说明
			 * 后端/宿主吞了 frame 事件（嵌套在别的合成器里时上层忙起来
			 * 就是这样），这一轮必须重新排 —— 否则状态机把帧链锁死在
			 * QUEUED，看门狗再怎么催都没用（用户报过的「停在最后一帧」）。 */
			int64_t waited = vidow_now_ns() - output->redraw_queued_ns;
			if (waited < 50 * 1000000LL) {
				already_queued = true;
			}
		}
		if (!already_queued) {
			output->redraw = VIDOW_REDRAW_QUEUED;
			output->redraw_queued_ns = vidow_now_ns();
		}
	}
	if (!already_queued) {
		wlr_output_schedule_frame(wlr_output);
	}
}

/* 客户端交帧了：帧追踪记一笔（没开追踪就什么都不做，零开销）。 */
void vidow_output_trace_commit(struct vidow_output *output) {
	if (output == NULL || output->server->trace_timer == NULL) {
		return;
	}
	output->content_ns = vidow_now_ns();
}

/* 补帧闹钟到点：催一帧出来（渲染里会走到 vidow_view_render_pump，到点的窗口就补上）。
 * 只在这里催，不在「还没到点」的时候催 —— 那样会变成每帧都渲染却什么都不补的空转。 */
static int pump_timer_cb(void *data) {
	struct vidow_output *output = data;
	vidow_output_deadline_fired(output);
	output->pump_at_ms = 0;
	vidow_output_schedule_frame(output->server, output->wlr_output,
		"补帧闹钟到点");
	return 0;
}

/* A screen capture is answered on an output commit, and a still screen never
 * commits: the nested backend will not even send us a frame callback, because
 * the host only composites on damage. Poll gently while a capture is
 * outstanding and render for it directly.
 *
 * 只在**真的有人等着**的时候转：这个定时器原来是建好就 50ms 一次永远转下去，
 * 空载也要每秒醒 20 次（实测 strace：21 秒里 epoll_wait 返回 427 次、
 * timerfd_settime 852 次 —— 期间一次抓帧都没有）。现在由抓帧请求踢一脚
 * （见 vidow_output_capture_kick），抓完没人等着就自己停 —— 空载零唤醒。 */
/* 抓帧兜底的间隔（毫秒）。capture_fps = 60 → 16ms；<=0 → 0 表示「不续期」。
 *
 * 原来这里硬编码 50（= 20fps），是「别自作聪明替后端画帧」的保守值。
 * 远程控制要跟手，20fps 的步进肉眼就是卡顿，所以改成可配：默认 60fps，
 * 想省电/省带宽就调低。下限 1ms 挡住荒谬配置（1000Hz）。 */
/* ---- 帧截止时刻的影子记账（定时器合并的第一步） ----
 *
 * 背景：这块屏上「什么时候该醒」有四个来源，各自上弦各自的定时器：
 *   anim_watchdog（动画/drag，按后端沉默时间算）
 *   capture_watchdog（抓帧客户端要的 fps）
 *   pump_timer（某个窗口的补帧到点，见 view.c）
 *   kick_timer（切 tty / 输出重建，固定 1ms，属于「延后一轮」不是节拍）
 *
 * 前三个本质是同一件事：**某个时刻要一帧**。合并成一个调度器 = 取最早的那个
 * 截止时刻上弦。风险不在时序，而在**漏登记一个来源**（漏了就永远不醒）。
 *
 * 所以先做影子：全部来源都往这里登记，只记账、只统计，不驱动任何东西。
 * 统计行会给出「这一窗口里最早截止在多少毫秒之后、谁提的」，和实际唤醒对比，
 * 一致了再拆。 */
void vidow_output_note_deadline(struct vidow_output *output, int64_t at_ns,
		const char *why) {
	if (output == NULL || at_ns <= 0) {
		return;
	}
	if (output->sched_deadline_ns == 0 || at_ns < output->sched_deadline_ns) {
		output->sched_deadline_ns = at_ns;
		output->sched_why = why;
	}
	vidow_stats.sched_notes++;
}

/* 定时器醒了：把「登记的截止」和「实际醒的时刻」对账。
 *
 * delta > 0 = 醒晚了，< 0 = 醒早了。合并成一个「取最早截止」的调度器之后，
 * 每次醒来都应该是 delta ≈ 0（早一点点或正好）。这个数字是判断「能不能合并」
 * 的直接证据：只要所有来源的 delta 都接近 0，说明没有来源被漏掉、也没有哪个
 * 需要在「非截止时刻」被叫醒。 */
void vidow_output_deadline_fired(struct vidow_output *output) {
	if (output == NULL || output->sched_deadline_ns == 0) {
		return;
	}
	int64_t d = vidow_now_ns() - output->sched_deadline_ns;
	if (d > 0) {
		vidow_stats.sched_late_ns += d;
		if (d > vidow_stats.sched_late_max_ns) {
			vidow_stats.sched_late_max_ns = d;
		}
	} else {
		vidow_stats.sched_early_ns += -d;
	}
	vidow_stats.sched_fired++;
	output->sched_deadline_ns = 0;
	output->sched_why = NULL;
}

static int capture_interval_ms(struct vidow_server *server) {
	int fps = server != NULL ? server->capture_fps : 0;
	if (fps <= 0) {
		return 0; /* 不续期：兜底关掉 */
	}
	int ms = 1000 / fps;
	return ms < 1 ? 1 : ms;
}

/* 光标抓帧（ext-image-copy-capture 的 pointer cursor session）记下来的「这块屏
 * 上有人在等一帧」。见 vidow_output_capture_kick_output。 */
static bool capture_kick_pending(struct vidow_output *output) {
	return output != NULL && output->capture_pending;
}

/* 这一块屏要不要推一帧：有 screencopy 帧在等它的下一次 commit，或者光标抓帧
 * 点了名。**逐屏判**是这里的关键：以前是「全局有帧 → 所有输出整屏标脏重画」，
 * 双屏上抓 A 屏时 B 屏每 16ms 白画一屏（整屏标脏 = 模糊链 / 装饰 / 提交全跑）。 */
static bool capture_wants_frame(struct vidow_server *server,
		struct vidow_output *output) {
	if (output == NULL || output->wlr_output == NULL) {
		return false;
	}
	if (capture_kick_pending(output)) {
		return true;
	}
	return vidow_screencopy_has_pending_for_output(server->screencopy,
		output->wlr_output);
}

/* 给这一块屏推一帧：先整屏标脏再渲染。
 *
 * **整屏标脏不能省**，否则这一帧白推：wlr_scene_output_commit() 在「没有损伤」时
 * 直接返回 false、压根不提交输出 —— 于是 wlr_output 的 commit 信号不响，
 * screencopy.c 的 frame_handle_output_commit 永远等不到 WLR_OUTPUT_STATE_BUFFER，
 * 逐帧抓屏的客户端（wf-recorder、远程工具）就一直卡在「等下一帧」：后台录到的是
 * 0 字节，只有退出收尾那一帧才有内容（实测：t=1..4s 文件一直是 0）。
 *
 * 屏幕静止时本来就没有损伤，可这里要的恰恰是「静止也出帧」—— 抓帧兜底的全部意义
 * 就在这。所以只对**确实有人等着**的那块屏标脏：多画一帧的代价（~1ms）换一个能推
 * 的帧流，但不替别的屏付这个代价。 */
/* **渲染只有一个入口。**
 *
 * 这块屏上「该画一帧了」在代码里就是四个来源：后端帧事件（output_frame）、
 * 动画看门狗、抓帧看门狗、切 tty / 输出重建时的 kick（kick_timer_cb）。
 * 以前每处自己调 vidow_output_render()、自己加计数，于是「这一帧是谁推的」
 * 只有统计行能回答，加新来源得记着去加一处计数。
 *
 * 别把「首帧」和「渲染期间又有人要帧（redraw_again）」也数成来源 ——
 * 它们最终都回到后端帧事件那一条路上来（见 output_frame 收口那段），
 * 这里写六个来源是我 2026-10-28 的笔误，对着 grep 核过：只有四个调用点。
 *
 * 现在全部走这里，原因顺手带进统计。函数本身只做记账 + 转发，不改任何时序 ——
 * 定时器的节奏、看门狗什么时候接管，仍旧由各自的调用点决定（那套时序在
 * anim_watchdog 的注释里有一整段踩坑记录，别在这里动）。 */
enum vidow_render_reason {
	VIDOW_RENDER_EVENT,     /* 后端给的帧事件 */
	VIDOW_RENDER_WATCHDOG,  /* 动画看门狗补帧 */
	VIDOW_RENDER_CAPTURE,   /* 抓帧等不及了，先画一帧 */
	VIDOW_RENDER_KICK,      /* 切 tty / 输出重建，主动画一次 */
};

static void render_for(struct vidow_output *output,
		enum vidow_render_reason why) {
	if (render_stats_on()) {
		switch (why) {
		case VIDOW_RENDER_EVENT:
			vidow_stats.render_from_event++;
			break;
		case VIDOW_RENDER_WATCHDOG:
			vidow_stats.render_from_watchdog++;
			break;
		case VIDOW_RENDER_CAPTURE:
			vidow_stats.render_from_capture++;
			break;
		case VIDOW_RENDER_KICK:
			vidow_stats.render_from_kick++;
			break;
		}
	}
	vidow_output_render(output);
}

static void capture_render_output(struct vidow_output *output) {
	wlr_log(WLR_DEBUG, "capture watchdog: render %s", output->wlr_output->name);
	if (output->scene_output != NULL) {
		wlr_damage_ring_add_whole(&output->scene_output->damage_ring);
	}
	/* vidow_output_render() 内部会 wlr_scene_output_commit()，那一句才是触发
	 * screencopy 的 events.commit 的东西（标脏 + 提交 = 抓帧客户端拿到 ready）。
	 * 关着的屏由它自己早退（见 vidow_output_render 开头）。 */
	render_for(output, VIDOW_RENDER_CAPTURE);
}

/* 抓帧看门狗的本体：有屏在等帧就推一帧并返回下次间隔，没人等就返回 0。 */
static int capture_watchdog_body(struct vidow_server *server) {
	{
		struct vidow_output *o;
		wl_list_for_each(o, &server->outputs, link) {
			vidow_output_deadline_fired(o);
		}
	}
	/* 光标抓帧那条账是**一次性**的：这一轮用掉就清，下一次
	 * cursor_source_request_frame 才会再记（客户端每次 capture 都会先 request_frame，
	 * 而那时它的帧还欠着、下一轮提交就会把它发出去）。只靠它的时候看门狗转一次
	 * 就停，不会变成空转定时器。 */
	bool any = false;
	struct vidow_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &server->outputs, link) {
		if (!capture_wants_frame(server, output)) {
			continue; /* 这块屏没人等着：一帧都不推 */
		}
		output->capture_pending = false;
		any = true;
		capture_render_output(output);
		/* 渲染会派发事件（客户端 destroy、输出拔线），output 可能已经没了 ——
		 * 用 _safe 遍历拿到的 tmp 只是下一个节点，这里不再碰 output。 */
	}
	if (!any) {
		return 0; /* 没人等着：不续期（下一轮不会再被排上） */
	}
	{
		int ms = capture_interval_ms(server);
		if (ms > 0) {
			struct vidow_output *o;
			wl_list_for_each(o, &server->outputs, link) {
				vidow_output_note_deadline(o,
					vidow_now_ns() + (int64_t)ms * 1000000LL,
					"抓帧");
			}
		}
		return ms;
	}
}

static int64_t anim_watchdog_body(struct vidow_server *server);

/* ---- 帧调度器：动画看门狗 + 抓帧看门狗合成一个 event source ----
 *
 * 两条路本质是同一件事：**某个时刻要一帧**。以前各排各的定时器，event loop 里
 * 两套到期判断、两处续期、两处「什么时候该撤」的语义。现在各记一个截止时刻，
 * 这里取最早的排一次。
 *
 * 没并进来的两个（有意）：
 *   pump_timer  每块屏一个闹钟（某个窗口的补帧到点），天然 per-output，并进来
 *               反而要维护 per-output 的堆；
 *   kick_timer  是「延后一轮事件派发」，不是帧节拍。
 *
 * 两条路都到期（或者都没有）时的行为要和原来逐个定时器时完全一致：
 * 到期的那条跑本体、本体返回下次间隔（0 = 不再排），然后重新取最早。 */
static void frame_rearm(struct vidow_server *server) {
	if (server->frame_timer == NULL) {
		return;
	}
	int64_t now = vidow_now_ns();
	int64_t best = 0;
	if (server->frame_anim_at_ns > 0) {
		best = server->frame_anim_at_ns;
	}
	if (server->frame_capture_at_ns > 0 &&
			(best == 0 || server->frame_capture_at_ns < best)) {
		best = server->frame_capture_at_ns;
	}
	if (best == 0) {
		wl_event_source_timer_update(server->frame_timer, 0); /* 撤 */
		return;
	}
	int64_t ms = (best - now) / 1000000;
	if (ms < 1) {
		ms = 1; /* 已到期也要排一次，不能 0 —— 0 是「撤除」 */
	}
	wl_event_source_timer_update(server->frame_timer, (int)ms);
}

static int frame_timer_cb(void *data) {
	struct vidow_server *server = data;
	int64_t now = vidow_now_ns();
	/* 先取走再跑：本体里可能重新安排自己（动画续期 / 抓帧下一轮），
	 * 也可能把另一条路的时刻改掉。 */
	if (server->frame_anim_at_ns > 0 && server->frame_anim_at_ns <= now) {
		server->frame_anim_at_ns = 0;
		int64_t ms = anim_watchdog_body(server);
		if (ms > 0) {
			server->frame_anim_at_ns = vidow_now_ns() + ms * 1000000LL;
		}
	}
	if (server->frame_capture_at_ns > 0 &&
			server->frame_capture_at_ns <= now) {
		server->frame_capture_at_ns = 0;
		int ms = capture_watchdog_body(server);
		if (ms > 0) {
			server->frame_capture_at_ns = vidow_now_ns() +
				(int64_t)ms * 1000000LL;
		}
	}
	frame_rearm(server);
	return 0;
}

/* 有人要抓帧了：把上面的看门狗叫起来（它是「静止的屏幕永远不提交」的兜底，
 * 见 capture_watchdog）。请求进来时踢一次就够 —— 它会自己续期到抓完。 */
void vidow_output_capture_kick(struct vidow_server *server) {
	if (server == NULL || server->frame_timer == NULL) {
		return;
	}
	int ms = capture_interval_ms(server);
	if (ms <= 0) {
		return; /* 兜底关着：只靠后端自己的提交 */
	}
	server->frame_capture_at_ns = vidow_now_ns() + (int64_t)ms * 1000000LL;
	frame_rearm(server);
}

void vidow_output_capture_kick_output(struct vidow_server *server,
		struct wlr_output *wlr_output) {
	if (server == NULL || wlr_output == NULL) {
		return;
	}
	struct vidow_output *output = vidow_output_lookup(server, wlr_output);
	if (output != NULL) {
		output->capture_pending = true;
	}
	/* 找不到那块屏也照样踢：诊断/记账不许改变「会不会推帧」这件事。 */
	vidow_output_capture_kick(server);
}

/* 看门狗自己推帧的节奏：跟着后端**自己**的帧间隔走，比它晚 1/4 才补一帧。
 *
 * 后端正常出帧时一帧都不多推 —— 白推一帧就是白画一屏（模糊链 + 装饰 + 提交，
 * 4K@2 上 ~1ms，嵌套时还会多插一次 commit 打乱上层的帧链）。后端停帧时才按这个
 * 节奏补上（嵌套在别的合成器里，上层一忙就不给帧）。
 *
 * 上限 40ms 是原来那条硬编码的兜底（后端 25Hz 就认了，别自作聪明替它画）；
 * 下限 6ms 是「至少 ~160Hz」—— 拖动是跟着指针走的，25fps 的步进肉眼就是
 * 「一顿一顿」（用户报的），所以拖动期间宁可多推也不能卡。
 * frame_gap_ns == 0（还没量到过一次正常间隔）按 60Hz 猜，拖动期间按 8ms（~125Hz）
 * 猜 —— 拖动时「宁可多推」优先，动画不用那么急。 */
static int64_t watchdog_interval_ns(struct vidow_output *output, bool dragging) {
	int64_t gap = output->frame_gap_ns > 0 ? output->frame_gap_ns
		: (dragging ? 8 : 16) * 1000000LL;
	int64_t iv = gap + gap / 4;
	if (iv < 6 * 1000000LL) {
		iv = 6 * 1000000LL;
	}
	if (iv > 40 * 1000000LL) {
		iv = 40 * 1000000LL;
	}
	return iv;
}

/* The nested Wayland backend only delivers a frame event once the host
 * compositor has presented our surface, and the host only presents on damage:
 * that deadlocks an animation that is waiting for its next frame. If frames
 * dry up while an animation is in flight, drive it from a timer instead. */
/* 动画看门狗的本体：算出「下一次该醒在多少毫秒后」，0 = 不用再醒。
 * 上弦交给 frame_timer（见 frame_timer_cb 的说明）。 */
static int64_t anim_watchdog_body(struct vidow_server *server) {
	server->watchdog_armed = false;
	{
		struct vidow_output *o;
		wl_list_for_each(o, &server->outputs, link) {
			vidow_output_deadline_fired(o);
		}
	}
	/* 拖着窗口也算「有活在飞」：浮动拖拽的落位是推迟到帧里做的（见 float.c 的
	 * arrange_dirty），帧一停它就**一点都不动** —— 上层合成器一旦不给帧事件
	 * （嵌套在别的合成器里、窗口被盖住 / 那台合成器自己闲着），拖动就冻住。 */
	bool dragging = server->grab_view != NULL;
	if (!vidow_anim_pending(server) && !vidow_jelly_pending(server) &&
			!dragging) {
		return 0;
	}
	int64_t now = vidow_now_ns();
	int64_t wait_ms = 0;
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		int64_t iv = watchdog_interval_ns(output, dragging);
		int64_t since = now - output->last_render_ns;
		/* 后端还活着（帧事件还在按自己的节奏来）就别自己推。
		 *
		 * 这个看门狗本来只该给**嵌套**后端兜底（宿主合成器只在有损伤时才给我们
		 * 帧）。真机上（DRM）它按帧间隔的 1.25 倍自己推，偶尔就会和帧事件撞在
		 * 同一个 vblank 里 —— 那一帧提交上去后端直接拒掉（
		 * "Failed to page-flip output: a page-flip is already pending"），屏幕
		 * 上永远看不到它，动画里就是一顿一卡。用户日志里 51 条这个错误，全部
		 * 紧跟在 swap-window / 切工作区后面（= 动画在跑 = 看门狗刚被叫起来）。
		 *
		 * 判据用「上一帧事件过去多久」：超过 2 个间隔没来，才算后端不给帧了，
		 * 这时才接管（原来「窗口停在最后一帧」那条路照旧有人管）。 */
		int64_t since_frame = output->last_frame_ns != 0 ?
			now - output->last_frame_ns : INT64_MAX;
		bool backend_live = since_frame <= 2 * iv;
		if (since >= iv && !backend_live) {
			/* 后端欠着这一帧，自己补上 */
			render_for(output, VIDOW_RENDER_WATCHDOG);
			since = 0;
			iv = watchdog_interval_ns(output, dragging);
		}
		/* 下一轮就在「该干活的那一刻」之后 —— 按剩余时间排，而不是固定
		 * 4ms / 16ms 地空转：4ms 一轮的话，10ms 的目标节奏实际会落成 12ms
		 * （4 的整数倍），白丢 20% 的帧。后端还活着的时候等的不是「渲染间隔」
		 * 而是「它沉默够久」，不然 1ms 一轮空转。 */
		int64_t left_ns = backend_live ? (2 * iv - since_frame)
			: (iv - since);
		if (left_ns < 0) {
			left_ns = 0;
		}
		int64_t left = left_ns / 1000000 + 1;
		if (wait_ms == 0 || left < wait_ms) {
			wait_ms = left;
		}
	}
	/* 还有活在飞就继续自己推。这条续期是关键：不续的话看门狗只响一次，下次
	 * 得等新的指针事件来 kick（拖动时后端要是这段时间一直不给帧，就又冻住了）。 */
	server->watchdog_armed = true;
	{
		int ms = wait_ms < 1 ? 1 : (int)wait_ms;
		wl_list_for_each(output, &server->outputs, link) {
			vidow_output_note_deadline(output, now + ms * 1000000LL,
				"动画");
		}
		return ms;
	}
}

/* Armed whenever an animation starts or a frame is presented, so the
 * watchdog is running even if the backend never sends another frame.
 * 已经排上就不再排 —— 拖动时每个指针事件都会调到这儿（见 server.h 的
 * watchdog_armed）。 */
void vidow_output_kick_watchdog(struct vidow_server *server) {
	if (server->frame_timer == NULL || server->watchdog_armed) {
		return;
	}
	server->watchdog_armed = true;
	server->frame_anim_at_ns = vidow_now_ns() + 8 * 1000000LL;
	frame_rearm(server);
}

/* 立刻给每块屏画一帧。切 tty 切回来的路上（DRM 后端会把所有输出销毁再重建）
 * 和输出刚建出来的时候，场景里没有任何 damage，后端也不欠我们 frame 事件 ——
 * 不主动画一次，屏幕就一直停在上一帧（用户看到的「卡死」）。
 * 走 1ms 定时器而不是当场画：调用点在 wlroots 的会话处理里面，等这一轮派发
 * 结束再画干净。 */
static int kick_timer_cb(void *data) {
	struct vidow_server *server = data;
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		render_for(output, VIDOW_RENDER_KICK);
	}
	return 0;
}

void vidow_output_kick_all(struct vidow_server *server) {
	if (server->kick_timer == NULL) {
		kick_timer_cb(server); /* 没有定时器（极早期）就当场画 */
		return;
	}
	wl_event_source_timer_update(server->kick_timer, 1);
}

static void output_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_output *output = wl_container_of(listener, output, commit);
	/* 会话锁（见 sessionlock.c）：锁定期间这块屏刚提交了新状态（换模式 / 改缩放 /
	 * 热插拔重排），逻辑尺寸可能变了 —— 立刻对齐黑 rect + 重新 configure 它的
	 * lock surface。不能等下一帧：锁定期间没人动的话可能一直不出帧。
	 * 非锁定期间不用管：output->box 在渲染里每帧都会刷新。 */
	if (output->server->locked) {
		vidow_sessionlock_output_sync(output);
	}
}

static void output_frame(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_output *output = wl_container_of(listener, output, frame);
	struct vidow_server *server = output->server;

	/* 测试钩子：当作「后端不再给这块屏帧事件」= 用户报过的「这块屏上的窗口停在
	 * 最后一帧，拖到另一块屏就活」。真机上只有 DRM 的 page-flip 不回来才会这样
	 * （headless 的帧事件和 commit 无关，按不住）；帧追踪的判据靠它验。 */
	const char *stall = stall_output_name();
	if (stall != NULL && strcmp(stall, output->wlr_output->name) == 0) {
		return;
	}

	int64_t now_ns = vidow_now_ns();
	if (output->want_frame && server->trace_timer != NULL) {
		int64_t gap = (now_ns - output->last_frame_ns) / 1000000;
		if (gap >= 300) {
			wlr_log(WLR_DEBUG, "trace: %s 又出帧了（停了 %lldms，这期间被要了"
				" %d 帧）", output->wlr_output->name, (long long)gap,
				output->want_frame_n);
		}
	}
	output->want_frame = false;
	output->want_frame_n = 0;
	/* 后端帧间隔的滑动平均（看门狗的目标节奏，见 watchdog_interval_ns）。
	 * 只喂「看着正常」的间隔（< 25ms = 40Hz 以上）：后端自己就慢或者刚停过帧的
	 * 那些长间隔喂进去，会把目标节奏越拉越长，看门狗跟着一起变慢 —— 拖动就又
	 * 开始一顿一顿。真正的停帧（几百 ms）本来就该由看门狗按健康的节奏补上。 */
	int64_t gap_ns = now_ns - output->last_frame_ns;
	if (output->last_frame_ns != 0 && gap_ns > 0 &&
			gap_ns < 25 * 1000000LL) {
		output->frame_gap_ns = output->frame_gap_ns > 0 ?
			(output->frame_gap_ns * 3 + gap_ns) / 4 : gap_ns;
	}
	output->last_frame_ns = now_ns;
	/* X11 游戏退出全屏可能只改属性、不再交帧：每帧对一遍，别让状态栏一直
	 * 被当成「还在全屏底下」藏着。 */
	vidow_views_sync_x11_fullscreen(server);
	/* 帧调度：进渲染前清掉「渲染期间又有人要帧」的标记，渲染里
	 * vidow_output_schedule_frame 会重新置它（模糊失效、视口外补帧）。 */
	output->redraw_again = false;
	output->redraw = VIDOW_REDRAW_RENDERING;
	render_for(output, VIDOW_RENDER_EVENT);
	/* 收口：这一帧没人再要就回 IDLE（下一帧直接复用这次提交的内容，一个
	 * GPU 命令都不发）；渲染期间有人要过就保持 QUEUED，那一帧已经排上了。 */
	if (!output->redraw_again && output->redraw == VIDOW_REDRAW_RENDERING) {
		output->redraw = VIDOW_REDRAW_IDLE;
	}

	/* An animating node usually damages the scene, but not always (a pure
	 * opacity change on a fully occluded window). Ask for the next frame
	 * anyway, and arm the watchdog in case the request goes nowhere.
	 * 拖着果冻也一样：网格在动，帧不能停。 */
	if (vidow_anim_pending(server) || vidow_jelly_pending(server) ||
			server->grab_view != NULL) {
		vidow_output_schedule_frame(server, output->wlr_output,
			server->grab_view != NULL ? "拖动在跑" : "动画/果冻在跑");
		vidow_output_kick_watchdog(server);
	}
}

static void output_request_state(struct wl_listener *listener, void *data) {
	struct vidow_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;

	if (wlr_output_commit_state(output->wlr_output, event->state)) {
		/* Output size changed (e.g. the host resized our nested window):
		 * recompute the tiles and throw away the output-sized backdrops. */
		vidow_backdrop_damage(output->server, true);
		vidow_arrange(output->server);
	}
}

/* 方向的名字 -> 枚举（config 和 IPC 共用）。 */
bool vidow_output_parse_transform(const char *text, enum wl_output_transform *out) {
	static const struct {
		const char *name;
		enum wl_output_transform t;
	} names[] = {
		{ "normal", WL_OUTPUT_TRANSFORM_NORMAL },
		{ "0", WL_OUTPUT_TRANSFORM_NORMAL },
		{ "90", WL_OUTPUT_TRANSFORM_90 },
		{ "180", WL_OUTPUT_TRANSFORM_180 },
		{ "270", WL_OUTPUT_TRANSFORM_270 },
		{ "flipped", WL_OUTPUT_TRANSFORM_FLIPPED },
		{ "flipped-90", WL_OUTPUT_TRANSFORM_FLIPPED_90 },
		{ "flipped-180", WL_OUTPUT_TRANSFORM_FLIPPED_180 },
		{ "flipped-270", WL_OUTPUT_TRANSFORM_FLIPPED_270 },
	};
	if (text == NULL) {
		return false;
	}
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (strcasecmp(names[i].name, text) == 0) {
			*out = names[i].t;
			return true;
		}
	}
	return false;
}

/* 模式字符串 -> 分辨率 + 刷新率："3840x2160@144" / "3840x2160"（刷新率不挑）/
 * "3840x2160@59.94"。@ 后面的刷新率可以带小数。 */
bool vidow_output_parse_mode(const char *text, int *w, int *h, double *hz) {
	if (text == NULL) {
		return false;
	}
	char *end = NULL;
	long width = strtol(text, &end, 10);
	if (end == text || (*end != 'x' && *end != 'X')) {
		return false;
	}
	const char *rest = end + 1;
	long height = strtol(rest, &end, 10);
	if (end == rest || width <= 0 || height <= 0) {
		return false;
	}
	double refresh = 0.0;
	if (*end == '@') {
		const char *hz_text = end + 1;
		refresh = strtod(hz_text, &end);
		if (end == hz_text || refresh <= 0.0) {
			return false;
		}
	}
	if (*end != '\0') {
		return false; /* "3840x2160x2" 这种不认 */
	}
	*w = (int)width;
	*h = (int)height;
	*hz = refresh;
	return true;
}

/* 在输出报的模式表里找 w×h[+hz]。hz <= 0 = 刷新率不挑（优先那块屏的首选模式）。
 * 找不到返回 NULL（调用方可以退回自定义模式）。 */
static struct wlr_output_mode *output_find_mode(struct wlr_output *wlr_output,
		int w, int h, double hz) {
	struct wlr_output_mode *match = NULL;
	struct wlr_output_mode *mode;
	wl_list_for_each(mode, &wlr_output->modes, link) {
		if (mode->width != w || mode->height != h) {
			continue;
		}
		if (hz > 0.0 && fabs(mode->refresh / 1000.0 - hz) > 0.6) {
			continue;
		}
		match = mode;
		if (mode->preferred) {
			break;
		}
	}
	return match;
}

/* 「逻辑尺寸统一」的基准：所有屏的逻辑分辨率对齐到同一套。
 *
 * **基准 = 所有屏里物理面积最大的那块的分辨率**，与各自配的 scale 无关。
 *
 * 用户那套场景（1080p + 4K）：
 *   4K 屏   : 3840x2160  → 基准 3840x2160 逻辑
 *   1080p 屏: 也要 3840x2160 逻辑 → scale = 1920/3840 = 0.5
 *             （1920 物理像素横向取样 3840 逻辑像素 = 降采样，
 *              用户说的「1x 屏相当于超采样」就是这条）
 *
 * X 屏幕 = 各屏设备像素累加 = 3840(4K) + 1920(1080p) = 5760
 *    —— 注意 X 侧看到的是**设备像素**，和逻辑尺寸无关；逻辑统一只影响
 *    Wayland 侧的坐标空间。两屏按 3840x2160 逻辑并排时布局是 7680x2160
 *    逻辑，这是 Wayland 客户端看到的坐标系。 */
static bool output_unify_target(struct vidow_server *server, int *w, int *h) {
	int best = 0, best_w = 0, best_h = 0;
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_output *o = output->wlr_output;
		if (!o->enabled) {
			continue;
		}
		/* 配置里点名了 mode 就用它（配置跑得比模式切换早，这时候 o->width
		 * 还是后端的默认值）；没点名才退回当前模式 / preferred。 */
		int pw = 0, ph = 0;
		const struct vidow_output_conf *conf =
			vidow_output_conf_for(server, o->name);
		if (conf != NULL && conf->has_mode &&
				conf->mode_w > 0 && conf->mode_h > 0) {
			pw = conf->mode_w;
			ph = conf->mode_h;
		}
		if (pw <= 0 || ph <= 0) {
			struct wlr_output_mode *m = wlr_output_preferred_mode(o);
			pw = m != NULL ? m->width : o->width;
			ph = m != NULL ? m->height : o->height;
		}
		if (pw <= 0 || ph <= 0) {
			continue;
		}
		/* **物理面积**排序：4K 屏 3840x2160 > 1080p 屏 1920x1080，
		 * 所以基准永远是 4K 那块的原生分辨率。不除 scale —— 除的话
		 * 4K@2 会变成 1920x1080，和 1080p 打平，基准就不确定了。 */
		int area = pw * ph;
		if (area > best) {
			best = area;
			best_w = pw;
			best_h = ph;
		}
	}
	if (best <= 0) {
		return false;
	}
	*w = best_w;
	*h = best_h;
	return true;
}

/* 一个输出该用多大缩放，以及为什么。w / h 是它将要上的那个模式的分辨率
 * （还没上模式时就是现在这个）。 */
static double output_pick_scale(struct vidow_server *server,
		struct wlr_output *wlr_output, int w, int h,
		char *why, size_t why_len) {
	struct vidow_config *cfg = &server->config;

	/* 这块屏自己那一组（vidow.output{ name = … }）里写了 scale 就按它来，
	 * 没写才用全局的 scale。 */
	const struct vidow_output_conf *conf =
		vidow_output_conf_for(server, wlr_output->name);
	struct vidow_scale_spec spec;
	bool named = conf != NULL && conf->has_scale &&
		vidow_scale_parse(conf->scale, &spec);
	if (!named && !vidow_scale_parse(cfg->scale, &spec)) {
		spec.kind = VIDOW_SCALE_AUTO;
	}

	double scale = vidow_scale_resolve(named ? &spec : NULL, cfg->scale, w, h,
		wlr_output->phys_width, wlr_output->phys_height);

	/* **逻辑尺寸统一**（cfg.logical_uniform）：所有屏的逻辑分辨率对齐到
	 * 基准（= 现有配置下逻辑面积最大的那块，见 output_unify_target）。
	 * 每块屏的 scale = 自己的物理尺寸 / 基准逻辑。
	 *
	 * 用户场景（1080p + 4K，4K 配 scale=2）：
	 *   基准 = 4K 屏的逻辑 = 1920x1080
	 *   4K 屏   : 3840/1920 = 2.0  → 保持 2，逻辑 1920x1080 ✓
	 *   1080p 屏: 1920/1920 = 1.0  → 原生 1:1，逻辑 1920x1080 ✓
	 *   X 屏幕  = 1920(1x) + 3840(2x) = 5760x2160
	 *
	 * 两屏逻辑完全相同（1920x1080），窗口跨屏不跳大小；1080p 屏是原生
	 * 1:1 渲染（scale=1，没有超采样浪费）。
	 *
	 * 如果基准比这块屏的物理尺寸还大（1080p 屏要对齐到 4K 的 3840 逻辑），
	 * 那 scale 会小于 1 —— 夹到 1.0 就做不到「逻辑相同」了。那种情况如实
	 * 告诉用户：这块屏物理上不够，只能按 1:1 出。 */
	if (cfg->logical_uniform && !wl_list_empty(&server->outputs)) {
		/* 这块屏「将要成为」的物理尺寸：配置里点名了 mode 就用它，否则用
		 * 传进来的 w/h（当前模式）。配置跑得比模式切换早，创建那一刻
		 * w/h 还是后端的默认值（headless 1280x720），拿它算会得到错的倍率。 */
		int phys_w = w, phys_h = h;
		if (conf != NULL && conf->has_mode &&
				conf->mode_w > 0 && conf->mode_h > 0) {
			phys_w = conf->mode_w;
			phys_h = conf->mode_h;
		}
		int target_w = 0, target_h = 0;
		bool have_target = output_unify_target(server, &target_w, &target_h);
		if (have_target && target_w > 0 && target_h > 0 &&
				phys_w > 0 && phys_h > 0) {
			/* **逻辑尺寸和物理尺寸无关**：每块屏的逻辑宽高都等于基准。
			 * 一块 1920 宽的屏要装下 3840 逻辑宽 → scale = 1920/3840 = 0.5。
			 * 这个 scale 让 wl_output 报 3840 逻辑，而缓冲只有 1920 ——
			 * 客户端按 3840 排版、我们按 0.5 缩下去取样（超采样 / 降采样）。
			 *
			 * 下限不再是 1.0：1.0 的下限会让「小屏对齐到大逻辑」根本做不到
			 * （那正是用户要的 7680 布局）。改成 0.1 —— 再小就没有实际意义了。 */
			double s_unify = (double)phys_w / (double)target_w;
			double s_quant = vidow_scale_quantize(s_unify, 0.1, 8.0);
			/* **无条件返回**：逻辑统一打开时，配置里写的 scale 让位。
			 * 否则 4K 屏配了 auto（→1.0）会把统一算出来的 1.0 当成「不变」
			 * 从而不覆盖，别的屏又已经按 0.5 走了 —— 两屏逻辑尺寸就对不上。
			 * 每块屏的逻辑尺寸必须都由这一个公式决定，才谈得上「统一」。 */
			snprintf(why, why_len,
				"逻辑统一：物理 %dx%d / 基准 %dx%d = %.2f",
				phys_w, phys_h, target_w, target_h, s_unify);
			return s_quant;
		}
	}

	double ppi = 0.0;
	if (wlr_output->phys_width > 0 && wlr_output->phys_height > 0) {
		double diag_px = sqrt((double)w * w + (double)h * h);
		double diag_in = sqrt((double)wlr_output->phys_width *
			(double)wlr_output->phys_width + (double)wlr_output->phys_height *
			(double)wlr_output->phys_height) / 25.4;
		ppi = diag_in > 0.0 ? diag_px / diag_in : 0.0;
	}

	if (spec.kind == VIDOW_SCALE_FIXED) {
		if (named) {
			snprintf(why, why_len, "vidow.output{ %s } scale=%s",
				conf->name, conf->scale);
		} else {
			snprintf(why, why_len, "scale=%s", cfg->scale);
		}
	} else if (spec.kind == VIDOW_SCALE_OFF) {
		snprintf(why, why_len, "xwayland_scale=off（1:1）");
	} else if (ppi > 0.0) {
		snprintf(why, why_len, "auto：%.1fppi / 96", ppi);
	} else {
		snprintf(why, why_len, "auto：没有 EDID 尺寸，按 1:1");
	}
	return scale;
}

/* 把「关着的」屏全打开（mouse_move_enables_dpms / key_press_enables_dpms，
 * 见 server.h）。返回打开了几个 —— 0 = 当时没有关着的屏，调用者别打日志。
 *
 * 语义照 Hyprland 那两个开关：屏是「被关掉省电 / 被 output-power off 关掉」的，
 * 有人动了（指针 / 按键）就亮回来。vidow 这边没有单独的 DPMS 状态，
 * 「关掉的输出」本身就是那个状态，所以唤醒 = 把所有关着的输出打开。 */
int vidow_output_wake_disabled(struct vidow_server *server) {
	int n = 0;
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->wlr_output->enabled) {
			continue;
		}
		if (vidow_output_set_enabled(server, output->wlr_output, true)) {
			n++;
		}
	}
	if (n > 0) {
		wlr_log(WLR_INFO, "输入唤醒: 打开 %d 块关着的屏（dpms）", n);
	}
	return n;
}

/* 这块屏上现在挂着几个窗口（关屏的日志用：说清楚「东西还在，只是不画了」）。 */
static int count_output_views(struct vidow_server *server,
		struct wlr_output *output) {
	int n = 0;
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->output == output) {
			n++;
		}
	}
	return n;
}

/* 这块 wlr_output 对应的 vidow_output（IPC / 运行时改状态用）。 */
static struct vidow_output *output_of(struct vidow_server *server,
		struct wlr_output *wlr_output) {
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->wlr_output == wlr_output) {
			return output;
		}
	}
	return NULL;
}

/* 输出状态（缩放 / 方向 / 模式 / 开关）变过之后要重做的东西：布局按新逻辑尺寸
 * 算、背景缓存按新分辨率重建、光标主题按新缩放取、可用区域重问一次。 */
static void output_state_applied(struct vidow_server *server,
		struct vidow_output *output) {
	wlr_xcursor_manager_load(server->cursor_mgr, output->wlr_output->scale);
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");

	/* 贴图按旧的物理分辨率建的，布局按旧逻辑尺寸算的，都要重来。 */
	vidow_blur_output_finish(output);
	output->backdrop = NULL;
	output->backdrop_w = 0;
	output->backdrop_h = 0;
	wlr_output_layout_get_box(server->output_layout, output->wlr_output,
		&output->usable_area);
	output->box = output->usable_area;
	vidow_backdrop_damage(server, true);
	vidow_arrange_layers(server);
	vidow_arrange(server);
	/* 逻辑尺寸统一：这块屏的模式刚变过，「基准」可能跟着变了（比如新屏是
	 * 4K、刚切上来），别的屏的缩放也要重算。放在 arrange 之后是因为
	 * 基准要读最新的 width/height。递归风险：apply_scale_all 只在倍率真的
	 * 变了时才走到这里，链断了。 */
	if (server->config.logical_uniform) {
		struct vidow_output *other;
		wl_list_for_each(other, &server->outputs, link) {
			if (other != output) {
				vidow_output_apply_scale(server, other);
			}
		}
	}
	/* 这块屏的缩放可能变了（IPC output_scale / 配置 SIGHUP）：X 那边的全局缩放
	 * 是「所有屏里最大的那个」，跟着重算一遍 —— 它是 X 屏幕尺寸、RandR 模式和
	 * Xft.dpi 的依据。没变的话这个调用自己就返回了。 */
	vidow_xwayland_scale_apply(server);
	vidow_ipc_emit_output(server, "output-changed", output->wlr_output);
}

/* 缩放变了要重做的东西：散景贴图按输出分辨率缓存、布局按逻辑尺寸算、
 * 光标主题按缩放取大小。 */
void vidow_output_apply_scale(struct vidow_server *server,
		struct vidow_output *output) {
	struct wlr_output *wlr_output = output->wlr_output;
	/* 用**当前实际尺寸**，不是 preferred_mode：IPC 的 output-mode / 配置里
	 * 点名的自定义模式都跑在 preferred 之外（headless 的 preferred 永远是
	 * 1280x720），拿 preferred 算「物理 / 基准」会算成错的倍率。 */
	int w = wlr_output->width;
	int h = wlr_output->height;
	char why[64];
	double scale = output_pick_scale(server, wlr_output, w, h, why, sizeof(why));
	if (fabs(scale - (double)wlr_output->scale) < 1e-3) {
		return;
	}
	vidow_output_set_scale(server, wlr_output, scale, why);
}

/* SIGHUP / config.lua 改完 scale/outputs 之后调这个。 */
void vidow_output_apply_scale_all(struct vidow_server *server) {
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		vidow_output_apply_scale(server, output);
	}
}

/* ------------------------------------------- 运行时改输出（IPC 用）

 * 每一件都会 commit 一次，然后走 output_state_applied() 把跟着变的东西重做一遍，
 * 最后推一个 output-changed 事件。失败（后端不认）返回 false，调用者去报错。 */

bool vidow_output_set_scale(struct vidow_server *server,
		struct wlr_output *wlr_output, double scale, const char *why) {
	struct vidow_output *output = output_of(server, wlr_output);
	if (output == NULL) {
		return false;
	}
	double before = wlr_output->scale;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_scale(&state, (float)scale);
	bool ok = wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		wlr_log(WLR_ERROR, "output %s: 设置缩放 %.2f 失败", wlr_output->name,
			scale);
		return false;
	}
	/* 和「上屏那一刻」用同一套字段：日志里每块屏的最后一行永远是这个屏现在的
	 * 样子（配置是 Lua、跑得比输出创建晚，所以运行时换缩放是常态 —— 只打
	 * 「24 -> 2.00」的话，从日志里看不出它现在到底是几倍、逻辑尺寸多少）。 */
	wlr_log(WLR_INFO, "output %s: %dx%d @ %.3fHz scale %.2f（%s），逻辑 %dx%d，"
		"布局 %d,%d（缩放 %.2f -> %.2f）",
		wlr_output->name, wlr_output->width, wlr_output->height,
		(float)wlr_output->refresh / 1000.0f, wlr_output->scale,
		why != NULL ? why : "运行时改的",
		(int)lround(wlr_output->width / wlr_output->scale),
		(int)lround(wlr_output->height / wlr_output->scale),
		output->box.x, output->box.y, before, scale);
	output_state_applied(server, output);
	return true;
}

bool vidow_output_set_transform(struct vidow_server *server,
		struct wlr_output *wlr_output, enum wl_output_transform transform) {
	struct vidow_output *output = output_of(server, wlr_output);
	if (output == NULL) {
		return false;
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_transform(&state, transform);
	bool ok = wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		wlr_log(WLR_ERROR, "output %s: 设置方向 %d 失败", wlr_output->name,
			(int)transform);
		return false;
	}
	wlr_log(WLR_INFO, "output %s: 方向 -> %d（逻辑尺寸 %dx%d）", wlr_output->name,
		(int)transform, (int)lround(wlr_output->width / wlr_output->scale),
		(int)lround(wlr_output->height / wlr_output->scale));
	output_state_applied(server, output);
	return true;
}

/* 开关显示器。关掉 = 这块屏不再出画面（DRM 那边就是关 CRTC），上面的窗口都留着：
 * 位置、焦点、工作区都不动，屏回来（或者打开）就照原样画回去。 */
bool vidow_output_set_enabled(struct vidow_server *server,
		struct wlr_output *wlr_output, bool enabled) {
	struct vidow_output *output = output_of(server, wlr_output);
	if (output == NULL) {
		return false;
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, enabled);
	bool ok = wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		wlr_log(WLR_ERROR, "output %s: %s 失败", wlr_output->name,
			enabled ? "打开" : "关掉");
		return false;
	}
	wlr_log(WLR_INFO, "output %s: %s（%dx%d，缩放 %.2f，上面还有 %d 个窗口留着）",
		wlr_output->name, enabled ? "打开" : "关掉", wlr_output->width,
		wlr_output->height, (double)wlr_output->scale,
		count_output_views(server, wlr_output));
	/* 事件：屏开 / 关（笔记本盖屏、eDP 省电、`vidowmsg output-power` 都走这里）。
	 * 窗口和布局一件没动，只是「这块屏还画不画」。 */
	const struct vidow_event_field fields[] = {
		VIDOW_EV_STR("name", wlr_output->name),
		VIDOW_EV_BOOL("enabled", enabled),
		VIDOW_EV_END,
	};
	vidow_ipc_emit_simple(server, "output-power-changed", fields);
	if (!enabled && server->focused_output == wlr_output) {
		/* 聚焦的屏关掉了：焦点挪到还开着的一块屏上，别对着黑屏打字。 */
		struct vidow_output *other;
		wl_list_for_each(other, &server->outputs, link) {
			if (other != output && other->wlr_output->enabled) {
				server->focused_output = other->wlr_output;
				wlr_log(WLR_INFO, "output %s: 焦点挪到 %s",
					wlr_output->name, other->wlr_output->name);
				break;
			}
		}
	}
	output_state_applied(server, output);
	return true;
}

/* VRR（adaptive sync）。这块屏 / 这个后端不支持就只报一句：headless 后端在
 * wlroots 里根本不吃这个字段（提交会被拒），DRM 那边看连接器的 vrr_capable。 */
bool vidow_output_set_vrr(struct vidow_server *server,
		struct wlr_output *wlr_output, bool enabled) {
	struct vidow_output *output = output_of(server, wlr_output);
	if (output == NULL) {
		return false;
	}
	bool now = wlr_output->adaptive_sync_status !=
		WLR_OUTPUT_ADAPTIVE_SYNC_DISABLED;
	if (now == enabled) {
		return true;
	}
	if (!wlr_output->adaptive_sync_supported) {
		wlr_log(WLR_INFO, "output %s: 这块屏 / 这个后端不支持 VRR"
			"（adaptive sync），%s不生效", wlr_output->name,
			enabled ? "开" : "关");
		return false;
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_adaptive_sync_enabled(&state, enabled);
	bool ok = wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		wlr_log(WLR_INFO, "output %s: VRR %s 失败（后端拒了）", wlr_output->name,
			enabled ? "开" : "关");
		return false;
	}
	wlr_log(WLR_INFO, "output %s: VRR -> %s（现在 %s）", wlr_output->name,
		enabled ? "开" : "关",
		wlr_output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED ?
		"生效" : "后端没接");
	return true;
}

/* 按需 VRR（niri 的 `variable-refresh-rate on-demand=true` + 窗口规则
 * `variable-refresh-rate true`）：
 *
 *   屏那边写了 vrr_on_demand = true（或者全局 vrr_on_demand = true）→ 这块屏是
 *   「按需」模式：只要屏上**显示着**要 VRR 的窗口（窗口规则 vrr = true），就把
 *   adaptive sync 提上去；那种窗口一个都不显示了（切走工作区 / 关窗 / 最小化 /
 *   挪到别的屏）就降回来。
 *
 * 静态写死的 vrr（vidow.output{ vrr = true/false }）优先：那种屏整块恒定开 / 关，
 * 不看窗口（由 output_apply_conf_extra 一次定死）。两边都没写 = 什么都不碰，
 * 和改前一样。
 *
 * 每次 arrange 都会调一次（开窗 / 关窗 / 切工作区 / 换屏都会 arrange），但先比
 * 一遍现状：模式和要求都没变就立刻返回，不会每帧 commit。 */
void vidow_output_vrr_on_demand_update(struct vidow_server *server,
		struct wlr_output *wlr_output) {
	if (server == NULL || wlr_output == NULL) {
		return;
	}
	struct vidow_output *output = output_of(server, wlr_output);
	if (output == NULL || !wlr_output->enabled) {
		return;
	}
	const struct vidow_output_conf *conf =
		vidow_output_conf_for(server, wlr_output->name);
	if (conf != NULL && conf->has_vrr) {
		return; /* 静态开 / 关：这块屏不按需 */
	}
	bool mode = conf != NULL && conf->has_vrr_on_demand ?
		conf->vrr_on_demand : server->config.vrr_on_demand;

	bool want = false;
	if (mode) {
		/* 「显示着」= 它属于这块屏现在亮着的那块工作区，而且没被收起来。 */
		struct vidow_workspace *shown =
			vidow_workspace_shown_for(server, wlr_output);
		struct vidow_view *view;
		wl_list_for_each(view, &server->views, link) {
			if (!view->vrr || !view->mapped || view->minimized ||
					view->output != wlr_output) {
				continue;
			}
			if (shown != NULL && view->ws != shown) {
				continue;
			}
			want = true;
			break;
		}
	}
	if (mode == output->vrr_on_demand_mode &&
			want == output->vrr_on_demand_on) {
		return;
	}
	bool was_on = output->vrr_on_demand_on;
	output->vrr_on_demand_mode = mode;
	output->vrr_on_demand_on = want;
	if (!want && !was_on) {
		return; /* 本来也没提上去过：不用 commit 一次去「降」 */
	}
	wlr_log(WLR_DEBUG, "output %s: 按需 VRR -> %s（%s）", wlr_output->name,
		want ? "提上去" : "降回来",
		want ? "屏上显示着要 VRR 的窗口" : "没有要 VRR 的窗口了");
	vidow_output_set_vrr(server, wlr_output, want);
}

/* 渲染位深：10 位 = DRM_FORMAT_XRGB2101010 的输出缓冲。
 *
 * 「支不支持」不猜：先看后端报的格式表（能问就问），再直接提交试一次（后端
 * 没报格式表的时候只能这样）。失败就保持 8 位，日志里说清楚 —— 10 位要显卡、
 * 显示器、渲染器三样都到位，缺一样就只剩「本来就不生效」。 */
bool vidow_output_set_depth(struct vidow_server *server,
		struct wlr_output *wlr_output, int depth) {
	struct vidow_output *output = output_of(server, wlr_output);
	if (output == NULL) {
		return false;
	}
	uint32_t want = depth >= 10 ? DRM_FORMAT_XRGB2101010 : DRM_FORMAT_XRGB8888;
	if (wlr_output->render_format == want) {
		return true;
	}
	const struct wlr_drm_format_set *formats = wlr_output_get_primary_formats(
		wlr_output, server->allocator != NULL ?
		server->allocator->buffer_caps : WLR_BUFFER_CAP_DATA_PTR);
	/* 格式表里有这个格式就行，modifier 不挑（wlr_drm_format_set_has 是「一模一样的
	 * modifier」，拿 MOD_INVALID 去问会永远说不支持）。 */
	if (formats != NULL && formats->len > 0 &&
			wlr_drm_format_set_get(formats, want) == NULL) {
		wlr_log(WLR_INFO, "output %s: 这块屏不支持 %d 位色（输出缓冲没有 %s），"
			"保持 %d 位", wlr_output->name, depth,
			depth >= 10 ? "XRGB2101010" : "XRGB8888",
			wlr_output->render_format == DRM_FORMAT_XRGB2101010 ? 10 : 8);
		return false;
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_render_format(&state, want);
	bool ok = wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		wlr_log(WLR_INFO, "output %s: 设成 %d 位色失败（后端 / 渲染器不支持），"
			"保持 %d 位", wlr_output->name, depth,
			wlr_output->render_format == DRM_FORMAT_XRGB2101010 ? 10 : 8);
		return false;
	}
	wlr_log(WLR_INFO, "output %s: 输出缓冲 -> %s（%d 位色）", wlr_output->name,
		depth >= 10 ? "XRGB2101010" : "XRGB8888", depth);
	output_state_applied(server, output);
	return true;
}

bool vidow_output_set_mode(struct vidow_server *server,
		struct wlr_output *wlr_output, int width, int height,
		double refresh_hz) {
	struct vidow_output *output = output_of(server, wlr_output);
	if (output == NULL) {
		return false;
	}
	struct wlr_output_mode *match = output_find_mode(wlr_output, width, height,
		refresh_hz);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (match != NULL) {
		wlr_output_state_set_mode(&state, match);
	} else {
		/* 没这个模式：按自定义模式试一把（有些后端支持，有些会拒绝）。 */
		wlr_output_state_set_custom_mode(&state, width, height,
			refresh_hz > 0.0 ? (int32_t)(refresh_hz * 1000.0) : 0);
	}
	bool ok = wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		wlr_log(WLR_ERROR, "output %s: 设成 %dx%d@%.2f 失败", wlr_output->name,
			width, height, refresh_hz);
		return false;
	}
	wlr_log(WLR_INFO, "output %s: 模式 -> %dx%d @ %.2fHz（%s）", wlr_output->name,
		wlr_output->width, wlr_output->height, wlr_output->refresh / 1000.0,
		match != NULL ? "列表里的" : "自定义");
	output_state_applied(server, output);
	return true;
}

bool vidow_output_set_position(struct vidow_server *server,
		struct wlr_output *wlr_output, int x, int y) {
	struct vidow_output *output = output_of(server, wlr_output);
	if (output == NULL) {
		return false;
	}
	struct wlr_output_layout_output *layout_output =
		wlr_output_layout_add(server->output_layout, wlr_output, x, y);
	if (layout_output == NULL) {
		return false;
	}
	output->layout_output = layout_output;
	wlr_log(WLR_INFO, "output %s: 布局位置 -> %d,%d", wlr_output->name, x, y);
	output_state_applied(server, output);
	return true;
}

/* 配置里给这块屏指定的布局位置（vidow.output{ name = …, x = 0, y = 0 }，逻辑
 * 坐标）。没写 / 没点名就返回 false = 不摆，按后端给的顺序自动排。 */
static bool config_position_for(struct vidow_server *server, const char *name,
		int *x, int *y) {
	const struct vidow_output_conf *conf =
		vidow_output_conf_for(server, name);
	if (conf == NULL || !conf->has_pos) {
		return false;
	}
	*x = conf->x;
	*y = conf->y;
	return true;
}

/* 配置里那块屏的位置改过（SIGHUP / 配置刚跑完）：按新配置把每块屏再摆一次。
 * 位置没变的什么都不做（vidow_output_set_position 会把布局、背景、光标重做一遍，
 * 白做一次没必要）。 */
void vidow_output_apply_positions_all(struct vidow_server *server) {
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		int x = 0, y = 0;
		if (!config_position_for(server, output->wlr_output->name, &x, &y)) {
			continue;
		}
		if (output->box.x == x && output->box.y == y) {
			continue;
		}
		wlr_log(WLR_INFO, "output %s: 按配置（vidow.output）摆到 %d,%d"
			"（现在 %d,%d）", output->wlr_output->name, x, y, output->box.x,
			output->box.y);
		vidow_output_set_position(server, output->wlr_output, x, y);
	}
}

/* 配置里除了缩放 / 位置之外的几项（模式、方向、开关、VRR、位深）落到这块屏上。
 *
 * 每一项单独 commit：VRR / 位深这类「后端可能不认」的东西要是和开关屏、上模式
 * 挤在同一次 commit 里，一个不认就整次失败 —— 屏直接不亮，而原因只是一个可选
 * 项。分开做，谁不认就谁不生效，日志里点名。
 *
 * create = true 时只做「建屏那次 commit 没做的事」：模式 / 方向 / 开关已经在
 * 初始 state 里了，这里只补 VRR 和位深（它们不能在关着的屏上设，也不适合和
 * 上模式挤在一起）。 */
static void output_apply_conf_extra(struct vidow_server *server,
		struct vidow_output *output) {
	struct wlr_output *wlr_output = output->wlr_output;
	if (!wlr_output->enabled) {
		return; /* 关着的屏不能设 VRR / 位深（wlroots 会直接拒） */
	}
	const struct vidow_output_conf *conf =
		vidow_output_conf_for(server, wlr_output->name);
	if (conf == NULL) {
		return;
	}
	if (conf->has_vrr) {
		vidow_output_set_vrr(server, wlr_output, conf->vrr);
	}
	if (conf->has_depth) {
		vidow_output_set_depth(server, wlr_output, conf->depth);
	}
}

/* 配置里那块屏的开关 / 模式 / 方向改过（SIGHUP、配置刚跑完）：按新配置对一遍账。
 * 每一项都先比一遍现状，没变的什么都不做（每次 set_xxx 都会重排、重画背景）。 */
void vidow_output_apply_state_all(struct vidow_server *server) {
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_output *wlr_output = output->wlr_output;
		const struct vidow_output_conf *conf =
			vidow_output_conf_for(server, wlr_output->name);
		if (conf == NULL) {
			continue;
		}
		if (conf->has_enabled && wlr_output->enabled != conf->enabled) {
			wlr_log(WLR_INFO, "output %s: 按配置（vidow.output）%s",
				wlr_output->name, conf->enabled ? "打开" : "关掉");
			vidow_output_set_enabled(server, wlr_output, conf->enabled);
		}
		if (conf->has_mode && wlr_output->enabled &&
				((int)wlr_output->width != conf->mode_w ||
				 (int)wlr_output->height != conf->mode_h ||
				 (conf->mode_hz > 0.0 &&
				  fabs(wlr_output->refresh / 1000.0 - conf->mode_hz) > 0.6))) {
			wlr_log(WLR_INFO, "output %s: 按配置（vidow.output）换模式",
				wlr_output->name);
			vidow_output_set_mode(server, wlr_output, conf->mode_w,
				conf->mode_h, conf->mode_hz);
		}
		if (conf->has_transform && wlr_output->transform != conf->transform) {
			vidow_output_set_transform(server, wlr_output, conf->transform);
		}
		output_apply_conf_extra(server, output);
		/* 按需 VRR：配置改了（全局开关 / 这块屏的 vrr_on_demand / 静态 vrr）
		 * 也对一遍账 —— 该提的提、该降的降。 */
		vidow_output_vrr_on_demand_update(server, wlr_output);
	}
}

/* 一块屏的「记忆」：热插拔按输出名认人（详见 hotplug.h）。 */
static struct vidow_output_memory *output_memory(struct vidow_server *server,
		struct wlr_output *wlr_output) {
	if (wlr_output == NULL) {
		return NULL;
	}
	return vidow_output_memory_for(&server->output_memories, wlr_output->name);
}

/* 这块屏拔了，它的东西挪到哪块屏上：优先左边最近的那块（视觉上接着原来的位置），
 * 没有就取右边最近的，一块活的都没有就是 NULL（东西先悬着，等有屏了再安家）。 */
static struct vidow_output *output_pick_fallback(struct vidow_server *server,
		const struct wlr_box *dead_box) {
	size_t n = 0;
	struct vidow_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		n++;
	}
	if (n == 0) {
		return NULL;
	}

	struct vidow_hotplug_slot *slots = calloc(n, sizeof(*slots));
	struct vidow_output **outs = calloc(n, sizeof(*outs));
	if (slots == NULL || outs == NULL) {
		free(slots);
		free(outs);
		return NULL;
	}
	size_t i = 0;
	wl_list_for_each(o, &server->outputs, link) {
		/* 现问布局：拔线会让剩下的屏重新摆放，缓存的 box 可能还没更新 */
		struct wlr_box box = {0};
		wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
		slots[i].live = true;
		slots[i].x = box.x;
		outs[i] = o;
		i++;
	}
	size_t idx = vidow_hotplug_pick_fallback(slots, i, dead_box->x);
	struct vidow_output *picked = idx == SIZE_MAX ? NULL : outs[idx];
	free(slots);
	free(outs);
	return picked;
}

/* 一块屏走了（真拔线，或者测试钩子里的 wlr_output_destroy）。在 free(output)
 * 之前调用：记忆要记下它离开时的视口 / 位置 / 焦点，而 wlr_output 还是活的。 */
static struct vidow_output *output_gone(struct vidow_server *server,
		struct vidow_output *output) {
	/* 正在拆（wl_display_run 已经返回）：wl_display_destroy() 会一块一块 finish
	 * 输出，走到这儿的时候工作区链已经拆过一半了 —— 实测崩在
	 * vidow_workspace_count 里读 link.next（si_addr=0x8）。退出时没有「把东西挪到
	 * 别的屏上」这回事，整段跳过（视图 / 列的指针跟着进程一起消失）。 */
	if (server->shutting_down) {
		return NULL;
	}
	/* Alt+Tab 切换器开着、而且它就摆在这块屏上：先关掉 —— 它攥着
	 * wlr_output 指针，再往下走这块就要被释放了。 */
	if (server->mru.output == output->wlr_output) {
		vidow_mru_cancel(server);
	}
	/* 名字在 add 的时候就存进 memory 了：到这儿 wlr_output->name 已经被
	 * wlroots 收走，按名字现找会拿到空串（这块踩过，热插拔整段不生效）。 */
	struct vidow_output_memory *memory = output->memory;
	struct vidow_output *fallback = output_pick_fallback(server, &output->box);
	if (memory == NULL) {
		/* 连记忆都没有（比如认不出名字的输出）：至少别让谁指着已经释放的
		 * wlr_output —— 照样把东西挪走。 */
		vidow_view_output_gone(server, NULL, output->wlr_output, fallback);
		vidow_layer_output_gone(server, NULL, output->wlr_output, fallback);
		return fallback;
	}

	memory->present = false;
	memory->output = NULL;
	memory->last_box = vidow_hotplug_box(output->box.x, output->box.y,
		output->box.width, output->box.height);
	memory->viewport_x = output->viewport_x;
	memory->focused = (server->focused != NULL &&
		server->focused->output == output->wlr_output) ? server->focused : NULL;

	/* 列 / 浮动窗口 / layer surface 的「住处」改成 fallback，「归属」留在记忆里。 */
	vidow_view_output_gone(server, memory, output->wlr_output, fallback);
	vidow_layer_output_gone(server, memory, output->wlr_output, fallback);
	return fallback;
}

static void output_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_output *output = wl_container_of(listener, output, destroy);
	struct vidow_server *server = output->server;

	if (output->pump_timer != NULL) {
		wl_event_source_remove(output->pump_timer);
		output->pump_timer = NULL;
	}
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->commit.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);

	/* 会话锁（见 sessionlock.c）：这块屏没了，它那块黑 rect 也收掉（不然会留在
	 * 场景里盖住别的地方），它的 lock surface 记账 + 「还没呈现锁后第一帧」的账
	 * 一起清干净。必须在 vidow_output 被释放之前调 —— 记账里存着指向它的指针。 */
	if (output->lock_blank != NULL) {
		wlr_scene_node_destroy(&output->lock_blank->node);
		output->lock_blank = NULL;
	}
	vidow_sessionlock_output_removed(output);

	vidow_blur_output_finish(output);

	/* 先把这块屏上的东西安置好，再放它走：任何 view->output / col->output /
	 * ls->output 都不能指着已经被释放的结构（以前这里会留下野指针）。 */
	struct vidow_output *fallback = output_gone(server, output);

	if (server->focused_output == output->wlr_output) {
		server->focused_output = fallback != NULL ? fallback->wlr_output : NULL;
		if (server->focused_output == NULL) {
			struct vidow_output *next;
			wl_list_for_each(next, &server->outputs, link) {
				server->focused_output = next->wlr_output;
				break;
			}
		}
	}

	/* 事件要带名字，而 wlr_output 在这个信号之后就被释放了 —— 所以在这里发。 */
	vidow_ipc_emit_output(server, "output-removed", output->wlr_output);
	free(output);
	vidow_arrange(server);
	/* 最密的那块屏拔了：X 的全局缩放可以降下来。 */
	vidow_xwayland_scale_apply(server);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	struct vidow_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	if (!wlr_output_init_render(wlr_output, server->allocator, server->renderer)) {
		wlr_log(WLR_ERROR, "failed to init render for output %s", wlr_output->name);
		return;
	}

	/* 配置里这块屏那一组（vidow.output{…}）：开关 / 模式 / 方向都进第一次
	 * commit —— 一起上屏，客户端看到的第一个尺寸就是对的。VRR 和位深不行
	 * （不能在关着的屏上设，也不该和上模式挤同一次 commit），commit 之后由
	 * output_apply_conf_extra() 单独补。 */
	const struct vidow_output_conf *conf =
		vidow_output_conf_for(server, wlr_output->name);
	bool want_enabled = conf != NULL && conf->has_enabled ? conf->enabled : true;

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, want_enabled);
	struct wlr_output_mode *mode = NULL;
	int mode_w = wlr_output->width;
	int mode_h = wlr_output->height;
	if (conf != NULL && conf->has_mode) {
		mode_w = conf->mode_w;
		mode_h = conf->mode_h;
		mode = output_find_mode(wlr_output, mode_w, mode_h, conf->mode_hz);
		if (mode != NULL) {
			wlr_output_state_set_mode(&state, mode);
		} else {
			/* 模式表里没有：按自定义模式试一把（有些后端支持，有些会拒）。 */
			wlr_output_state_set_custom_mode(&state, mode_w, mode_h,
				conf->mode_hz > 0.0 ? (int32_t)(conf->mode_hz * 1000.0) : 0);
			wlr_log(WLR_INFO, "output %s: 模式表里没有 %dx%d%s，按自定义模式试",
				wlr_output->name, mode_w, mode_h,
				conf->mode_hz > 0.0 ? "（带刷新率）" : "");
		}
	} else {
		mode = wlr_output_preferred_mode(wlr_output);
		if (mode != NULL) {
			mode_w = mode->width;
			mode_h = mode->height;
			wlr_output_state_set_mode(&state, mode);
		}
	}
	if (conf != NULL && conf->has_transform) {
		wlr_output_state_set_transform(&state, conf->transform);
	}
	/* 缩放要在第一次 commit 之前定下来：它决定逻辑尺寸（布局坐标的基准），
	 * 也决定客户端拿到的 preferred_buffer_scale。 */
	char why[64];
	double scale = output_pick_scale(server, wlr_output, mode_w, mode_h, why,
		sizeof(why));
	wlr_output_state_set_scale(&state, (float)scale);
	if (!wlr_output_commit_state(wlr_output, &state)) {
		wlr_log(WLR_ERROR, "output %s: 第一次上屏失败（配置里那块屏的模式 / "
			"方向后端不认？）", wlr_output->name);
	}
	wlr_output_state_finish(&state);

	struct vidow_output *output = calloc(1, sizeof(*output));
	if (output == NULL) {
		return;
	}
	output->server = server;
	output->wlr_output = wlr_output;
	/* 帧追踪的基准：第一次出帧之前 last_frame_ns 是 0，而 vidow_now_ns() 是开机
	 * 以来的纳秒数 —— 不初始化的话第一帧会被算成「停了 3.8 小时」（实测）。 */
	output->last_frame_ns = vidow_now_ns();
	/* 看门狗的限速基准同理（它比的是「上一次真出帧」，不是「后端上一次给帧」）。 */
	output->last_render_ns = output->last_frame_ns;
	output->scene_output = wlr_scene_output_create(server->scene, wlr_output);
	/* 配置里点了名的屏摆在它说的位置；没点名就按到达顺序自动排。
	 * 「哪块在左边」不能靠自动排 —— 那个顺序是后端给的（headless 上实测是
	 * 反的），所以确定布局必须写在配置里（vidow.output{ name = …, x = … }）。 */
	int pos_x = 0, pos_y = 0;
	bool placed = config_position_for(server, wlr_output->name, &pos_x, &pos_y);
	if (placed) {
		output->layout_output = wlr_output_layout_add(server->output_layout,
			wlr_output, pos_x, pos_y);
		if (output->layout_output == NULL) {
			wlr_log(WLR_ERROR, "output %s: 摆到 %d,%d 失败，改成自动排",
				wlr_output->name, pos_x, pos_y);
			placed = false;
		}
	}
	if (!placed) {
		output->layout_output =
			wlr_output_layout_add_auto(server->output_layout, wlr_output);
	}
	wlr_output_layout_get_box(server->output_layout, wlr_output, &output->box);
	output->memory = output_memory(server, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout,
		output->layout_output, output->scene_output);

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	/* 输出提交新状态（换模式 / 改缩放）：锁定期间靠它立刻对齐黑 rect 和
	 * lock surface，不等下一帧（见 sessionlock.c）。 */
	output->commit.notify = output_commit;
	wl_signal_add(&wlr_output->events.commit, &output->commit);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	/* 视口外补帧的到点闹钟（见 view.c 的 vidow_view_render_pump） */
	output->pump_timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server->display), pump_timer_cb, output);

	wl_list_insert(&server->outputs, &output->link);

	/* 把指针摆到主屏中心（只在它「还没被用过」时做一次）。
	 *
	 * 原来指针一直停在布局原点 (0,0) —— 主屏在右边时用户看到的是「鼠标从最
	 * 左边冒出来」。判据用 cursor->x/y 都是 0（没人动过），所以主屏晚于别的屏
	 * 出现也能补上；之后指针归用户，热插拔不再拽它。 */
	if (server->cursor != NULL && server->cursor->x == 0.0 &&
			server->cursor->y == 0.0) {
		bool is_primary = server->config.primary_output != NULL &&
			strcmp(wlr_output->name, server->config.primary_output) == 0;
		if (is_primary || server->config.primary_output == NULL) {
			struct wlr_box box;
			wlr_output_layout_get_box(server->output_layout, wlr_output,
				&box);
			wlr_cursor_warp(server->cursor, NULL,
				box.x + box.width / 2.0, box.y + box.height / 2.0);
			wlr_log(WLR_INFO, "指针: 摆到 %s 中心 (%d,%d)", wlr_output->name,
				box.x + box.width / 2, box.y + box.height / 2);
		}
	}

	wlr_xcursor_manager_load(server->cursor_mgr, wlr_output->scale);
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");

	/* 测试钩子：把这块屏钉死在软光标上。真机上「放大图超出硬件光标平面」也是
	 * 这条路，而 headless 后端的假光标平面什么都收（wlr_output_is_headless()
	 * 认的就是 impl 指针，没法拿它做假）—— 没有这个钩子就测不到「抓帧让位」
	 * （见 cursorfx.c）。 */
	if (getenv("VIDOW_TEST_SOFT_CURSOR") != NULL) {
		wlr_output_lock_software_cursors(wlr_output, true);
	}

	if (server->focused_output == NULL) {
		server->focused_output = wlr_output;
	}

	/* Usable area starts as the whole output; layer exclusive zones shrink it. */
	wlr_output_layout_get_box(server->output_layout, wlr_output,
		&output->usable_area);
	output->box = output->usable_area;
	/* 会话锁的黑幕（见 sessionlock.c）：每块屏一块不透明黑 rect。节点本身常开，
	 * 由 layer_lock_blank 那棵树统一开关 —— 锁定期间才开。 */
	if (server->layer_lock_blank != NULL) {
		output->lock_blank = wlr_scene_rect_create(server->layer_lock_blank, 1, 1,
			(const float[4]){ 0.0f, 0.0f, 0.0f, 1.0f });
		vidow_sessionlock_output_update(output);
	}
	/* 锁定期间新插的屏：黑 rect 刚建好（上面那段），这里记账 + 打日志；它的
	 * lock surface 等客户端来建（见 sessionlock.c）。 */
	vidow_sessionlock_output_added(output);
	vidow_arrange_layers(server);

	/* 布局原点也打出来：多屏 / 热插拔时「谁在左边」直接看日志，测试也不用猜。 */
	wlr_log(WLR_INFO, "output %s: %dx%d @ %.3fHz scale %.2f（%s），逻辑 %dx%d，"
		"布局 %d,%d（%s）",
		wlr_output->name, wlr_output->width, wlr_output->height,
		(float)wlr_output->refresh / 1000.0f, wlr_output->scale, why,
		(int)lround(wlr_output->width / wlr_output->scale),
		(int)lround(wlr_output->height / wlr_output->scale),
		output->box.x, output->box.y,
		placed ? "配置指定（vidow.output）" : "自动排");

	/* 热插拔：这块屏以前见过的话，把归属它的列 / 窗口 / layer surface 认领回来
	 * （列宽、滚动位置、焦点都留着）。它自己会 arrange。 */
	struct vidow_output_memory *memory = output->memory;
	if (memory != NULL) {
		bool seen_before = memory->last_box.width > 0;
		memory->present = true;
		memory->output = wlr_output;
		vidow_view_output_added(server, memory, output);
		vidow_layer_output_added(server, memory, output);
		memory->last_box = vidow_hotplug_box(output->box.x, output->box.y,
		output->box.width, output->box.height);
		if (!seen_before) {
			wlr_log(WLR_INFO, "hotplug: %s 是一块新屏（记忆里没有），"
				"列按到达顺序排", memory->key);
		}
	}

	vidow_arrange(server);

	/* 配置里那块屏还写了 VRR / 位深的话，现在补上（不能在关着的屏上设，也不该
	 * 和上模式挤同一次 commit）。 */
	output_apply_conf_extra(server, output);

	/* 新屏可能比现有的都密：X 的全局缩放要跟着涨（涨了已存在的 X11 窗口会
	 * 重新声明缓冲区比例）。 */
	vidow_xwayland_scale_apply(server);

	vidow_ipc_emit_output(server, "output-added", wlr_output);
}


/* 测试钩子专用：把一块已经加进来的输出「重新认一次身份」。
 *
 * headless 后端的输出名是自增的（HEADLESS-1、2、3…），拔了再插会变成新名字；
 * 真实机器上连接器名是稳定的，所以产品路径不需要这个 —— 测试要验「同一块屏插回来
 * 还原工作区」，只能在加完之后把名字改回原来那个，再重新认一遍归属。
 * 只有 VIDOW_TEST_OUTPUTS 那段代码会调它。 */
void vidow_hotplug_reattach(struct vidow_server *server,
		struct wlr_output *wlr_output) {
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->wlr_output == wlr_output) {
			break;
		}
	}
	if (output == NULL || output->wlr_output != wlr_output) {
		return;
	}
	struct vidow_output_memory *memory = output_memory(server, wlr_output);
	if (memory == NULL || output->memory == memory) {
		return;
	}
	output->memory = memory;
	memory->present = true;
	memory->output = wlr_output;
	vidow_view_output_added(server, memory, output);
	vidow_layer_output_added(server, memory, output);
	memory->last_box = vidow_hotplug_box(output->box.x, output->box.y,
		output->box.width, output->box.height);
}

/* ---------------------------------------------- 测试钩子（不是产品功能）

 * VIDOW_TEST_OUTPUTS="600:remove:HEADLESS-2,1500:add:1280x720"
 * 在启动后的指定毫秒真拔 / 真插一块输出：走的是 backend 的真实事件路径
 * （wlr_output_destroy / wlr_headless_add_output），不是模拟状态 —— 用来在没有
 * 真显示器可拔的环境里验热插拔（见 test-hotplug.sh）。只有 headless 后端能凭空
 * 加输出，别的后端会拒绝并打一行日志。不设这个变量时这段代码不参与。 */

struct test_output_step {
	int at_ms;
	bool add;
	char arg[64]; /* remove: 输出名；add: WxH[:名字] */
	bool done;
	struct test_output_step *next;
};

static struct test_output_step *test_output_steps;
static struct wl_event_source *test_output_timer;
static int64_t test_output_start_ms;      /* 装钩子那一刻，步骤时间是相对它的 */
static struct wlr_backend *test_headless; /* 认出来的 headless 后端（加输出要用） */
static char test_last_removed[VIDOW_OUTPUT_KEY_MAX]; /* 上次拔掉的那块，给 add:last 用 */

static void test_output_free(void) {
	while (test_output_steps != NULL) {
		struct test_output_step *next = test_output_steps->next;
		free(test_output_steps);
		test_output_steps = next;
	}
}

/* name 可以是输出名，也可以是 "left" / "right" —— 按布局位置挑一块，测试脚本就不用
 * 猜 headless 后端先创建的是哪一块。 */
static struct vidow_output *test_pick_output(struct vidow_server *server,
		const char *name) {
	if (strcmp(name, "left") == 0 || strcmp(name, "right") == 0) {
		bool want_left = name[0] == 'l';
		struct vidow_output *best = NULL;
		struct vidow_output *o;
		wl_list_for_each(o, &server->outputs, link) {
			if (best == NULL ||
					(want_left ? o->box.x < best->box.x : o->box.x > best->box.x)) {
				best = o;
			}
		}
		return best;
	}
	struct vidow_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (strcmp(o->wlr_output->name, name) == 0) {
			return o;
		}
	}
	return NULL;
}

static void test_output_remove(struct vidow_server *server, const char *name) {
	struct vidow_output *o = test_pick_output(server, name);
	{
		if (o == NULL) {
			wlr_log(WLR_ERROR, "test: 没有叫 %s 的输出", name);
			return;
		}
		snprintf(test_last_removed, sizeof(test_last_removed), "%s",
			o->wlr_output->name);
		{
			const char *target = o->wlr_output->name;
			wlr_log(WLR_INFO, "test: 拔掉输出 %s（%s）", target, name);
			/* 和真拔线一模一样的一条路：backend 释放 wlr_output，我们收到
			 * events.destroy -> output_destroy。 */
			wlr_output_destroy(o->wlr_output);
		}
	}
}

/* size 可以是 "1280x720"，也可以点名 "1280x720:HEADLESS-2"。
 * 点名这个功能是给测试用的：headless 后端的输出名是自增的（HEADLESS-1、2、3…），
 * 拔了再插会变成 HEADLESS-3，那就永远验不到「插回来还原工作区」—— 真机器上连接器名
 * 是稳定的，所以这里把名字改成指定的那个，模拟「同一根线又插上了」。
 * （wlr_output.name 是 malloc 出来的字符串，wlroots 自己用 free() 释放，strdup 兼容。） */
static void test_output_add(struct vidow_server *server, const char *spec) {
	int w = 1280, h = 720;
	char size[64] = "";
	const char *name = NULL;
	const char *colon = strrchr(spec, ':');
	if (colon != NULL) {
		/* "add:1280x720:last" = 用上次拔掉那块屏的名字（模拟同一根线又插上了） */
		if (strcmp(colon + 1, "last") == 0 && test_last_removed[0] != '\0') {
			name = test_last_removed;
			colon = NULL;
		}
	}
	if (colon != NULL) {
		snprintf(size, sizeof(size), "%.*s", (int)(colon - spec), spec);
		name = colon + 1;
	} else {
		snprintf(size, sizeof(size), "%s", spec);
	}
	if (sscanf(size, "%dx%d", &w, &h) != 2 || w < 1 || h < 1) {
		w = 1280;
		h = 720;
	}
	/* WLR_BACKENDS=headless 时 server->backend 是个 multi backend，
	 * wlr_backend_is_headless() 对它返回 false —— 从现有输出身上认更直接。 */
	if (test_headless == NULL) {
		struct vidow_output *o;
		wl_list_for_each(o, &server->outputs, link) {
			if (wlr_output_is_headless(o->wlr_output)) {
				test_headless = o->wlr_output->backend;
				break;
			}
		}
	}
	if (test_headless == NULL) {
		wlr_log(WLR_ERROR, "test: 边上没有 headless 输出，加不出来，跳过");
		return;
	}
	wlr_log(WLR_INFO, "test: 插上 %dx%d 的 headless 输出%s%s", w, h,
		name != NULL ? "，名字改成 " : "", name != NULL ? name : "");
	struct wlr_output *out = wlr_headless_add_output(test_headless,
		(unsigned)w, (unsigned)h);
	if (out != NULL && name != NULL && name[0] != '\0') {
		free(out->name);
		out->name = strdup(name);
		/* 名字是加进来之后才改的（headless 后端的 new_output 是同步发的），
		 * 所以改完要重新认一次身份 —— 见 vidow_hotplug_reattach。 */
		vidow_hotplug_reattach(server, out);
	}
}

static int test_output_tick(void *data) {
	struct vidow_server *server = data;
	/* 步骤里的毫秒是「启动之后多久」，所以要减掉装钩子那一刻的单调时钟
	 * （vidow_now_ns() 是 CLOCK_MONOTONIC，直接拿它比会全部当成过期）。 */
	int64_t now_ms = vidow_now_ns() / 1000000 - test_output_start_ms;
	int left = 0;
	struct test_output_step *step;

	for (step = test_output_steps; step != NULL; step = step->next) {
		if (step->done) {
			continue;
		}
		if (step->at_ms > now_ms) {
			left++;
			continue;
		}
		step->done = true;
		if (step->add) {
			test_output_add(server, step->arg);
		} else {
			test_output_remove(server, step->arg);
		}
	}

	if (left > 0) {
		wl_event_source_timer_update(test_output_timer, 25);
	} else {
		test_output_free();
		test_output_timer = NULL;
	}
	return 0;
}

/* "600:remove:HEADLESS-2" / "1500:add:1280x720"，逗号分隔。 */
static bool test_output_parse(const char *spec, int *at_ms, bool *add,
		char *arg, size_t arg_len) {
	const char *colon1 = strchr(spec, ':');
	if (colon1 == NULL) {
		return false;
	}
	const char *colon2 = strchr(colon1 + 1, ':');
	if (colon2 == NULL) {
		return false;
	}
	size_t action_len = (size_t)(colon2 - colon1 - 1);
	const char *value = colon2 + 1;
	const char *end = strchr(value, ',');
	size_t value_len = end != NULL ? (size_t)(end - value) : strlen(value);

	*at_ms = atoi(spec);
	*add = action_len == 3 && strncmp(colon1 + 1, "add", 3) == 0;
	snprintf(arg, arg_len, "%.*s", (int)value_len, value);
	return true;
}

static void vidow_output_test_hook(struct vidow_server *server) {
	const char *spec = getenv("VIDOW_TEST_OUTPUTS");
	if (spec == NULL || spec[0] == '\0') {
		return;
	}

	int count = 0;
	const char *p = spec;
	while (p != NULL && *p != '\0') {
		int at_ms = 0;
		bool add = false;
		char arg[64] = "";
		if (!test_output_parse(p, &at_ms, &add, arg, sizeof(arg))) {
			break;
		}
		struct test_output_step *step = calloc(1, sizeof(*step));
		if (step == NULL) {
			break;
		}
		step->at_ms = at_ms;
		step->add = add;
		memcpy(step->arg, arg, sizeof(step->arg));
		step->next = test_output_steps;
		test_output_steps = step;
		count++;

		p = strchr(p, ',');
		if (p != NULL) {
			p++;
		}
	}

	if (count == 0) {
		wlr_log(WLR_ERROR, "test: VIDOW_TEST_OUTPUTS 里没有能识别的步骤");
		test_output_free();
		return;
	}
	test_output_start_ms = vidow_now_ns() / 1000000;
	test_output_timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server->display), test_output_tick, server);
	if (test_output_timer == NULL) {
		test_output_free();
		return;
	}
	wl_event_source_timer_update(test_output_timer, 25);
	wlr_log(WLR_INFO, "test: 输出热插拔钩子装上了（%d 步）", count);
}

bool vidow_output_init(struct vidow_server *server) {
	server->frame_timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server->display), frame_timer_cb, server);
	if (server->frame_timer == NULL) {
		wlr_log(WLR_ERROR, "failed to create the frame timer");
	}
	/* 抓帧兜底节奏：**配置先落**（config.lua 的 capture_fps，默认 60），
	 * 环境变量覆盖它（临时调试 / 脚本场景不用改配置文件）。 */
	server->capture_fps = server->config.capture_fps;
	if (server->capture_fps < 0) {
		server->capture_fps = 0;
	}
	const char *fps_env = getenv("VIDOW_CAPTURE_FPS");
	if (fps_env != NULL && *fps_env != '\0') {
		char *end = NULL;
		long v = strtol(fps_env, &end, 10);
		if (end != fps_env && v >= 0 && v <= 1000) {
			server->capture_fps = (int)v;
			wlr_log(WLR_INFO, "抓帧兜底节奏：%d fps（VIDOW_CAPTURE_FPS 覆盖配置）",
				server->capture_fps);
		} else {
			wlr_log(WLR_ERROR, "VIDOW_CAPTURE_FPS=\"%s\" 不是 0..1000 的整数，"
				"用配置值 %d fps", fps_env, server->capture_fps);
		}
	}
	/* 抓帧这一路不再是独立定时器：它登记到 frame_timer 的截止时刻里
	 * （见上面的 frame_rearm）。这里**不**排第一次 —— 没人在抓帧的时候
	 * 它一次都不该醒，由 vidow_output_capture_kick 按需叫起来。 */
	/* 这里**不**排第一次：没人在抓帧的时候它一次都不该醒（见 capture_watchdog，
	 * 由 vidow_output_capture_kick 按需叫起来）。 */
	server->kick_timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server->display), kick_timer_cb, server);
	if (server->kick_timer == NULL) {
		wlr_log(WLR_ERROR, "failed to create the resume kick timer");
	}
	if (getenv("VIDOW_TRACE_FRAMES") != NULL) {
		server->trace_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server->display), trace_timer_cb, server);
		if (server->trace_timer == NULL) {
			wlr_log(WLR_ERROR, "failed to create the frame trace timer");
		} else {
			wl_event_source_timer_update(server->trace_timer, 250);
			wlr_log(WLR_INFO, "帧追踪: 开（VIDOW_TRACE_FRAMES=1）—— 要了帧"
				"却 300ms 没出帧会报 WLR_ERROR");
		}
	}
	server->new_output.notify = server_new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);
	vidow_output_test_hook(server);
	return true;
}
