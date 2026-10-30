/* 弹出物背后的模糊：注册项生命周期 + 每帧那一次重画。
 *
 * 对外入口（vidow_popup_blur_register / unregister / invalidate）在 server.h；
 * 这里只给 fx.c 的调度层用 —— 它每帧遍历 server->popup_blurs，问「这块屏上
 * 该不该画、画哪一份」。 */
#ifndef VIDOW_FX_POPUP_H
#define VIDOW_FX_POPUP_H

#include "server.h"

/* 这个弹出物在哪块屏上（按场景坐标反查；拿不到就是 NULL）。 */
struct vidow_output *popup_blur_owner(struct vidow_popup_blur *pb);

/* 在 output 上重画这个弹出物的模糊（缓存命中就只摆节点）。 */
void popup_blur_frame(struct vidow_server *server,
	struct vidow_output *output, struct vidow_popup_blur *pb);

/* 这个注册项要没了：摘监听、放缓冲、释放 pb。 */
void popup_blur_release(struct vidow_popup_blur *pb);

#endif
