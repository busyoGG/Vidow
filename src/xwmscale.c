/* X11 has its own uniform per-output virtual space. Wayland layout/scale are
 * never changed here. Positions are affine, sizes are linear, independently
 * on each axis; Xft.dpi is rendering policy, not a coordinate multiplier. */
#include "server.h"

#include <float.h>
#include <math.h>
#include <string.h>

/* Equal Wayland origins share a row/column. Distinct origins are ordered and
 * packed into uniform cells, including layouts with negative coordinates. */
static int output_axis_rank(struct wlr_output_layout *layout, int origin,
		bool horizontal) {
	int rank = 0;
	struct wlr_output_layout_output *lo;
	wl_list_for_each(lo, &layout->outputs, link) {
		int start = horizontal ? lo->x : lo->y;
		if (start >= origin) {
			continue;
		}
		bool duplicate = false;
		struct wlr_output_layout_output *other;
		wl_list_for_each(other, &layout->outputs, link) {
			if (other == lo) {
				break;
			}
			if ((horizontal ? other->x : other->y) == start) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate) {
			rank++;
		}
	}
	return rank;
}

bool vidow_xwm_output_boxes(const struct vidow_server *server,
		struct wlr_output *output, struct wlr_box *wayland,
		struct wlr_box *native) {
	if (wayland != NULL) {
		*wayland = (struct wlr_box){0};
	}
	if (native != NULL) {
		*native = (struct wlr_box){0};
	}
	if (server == NULL || server->output_layout == NULL || output == NULL ||
			wlr_output_layout_get(server->output_layout, output) == NULL) {
		return false;
	}
	struct wlr_box logical = {0};
	wlr_output_layout_get_box(server->output_layout, output, &logical);
	if (logical.width <= 0 || logical.height <= 0) {
		return false;
	}
	int width = 0, height = 0;
	struct wlr_output_layout_output *lo;
	wl_list_for_each(lo, &server->output_layout->outputs, link) {
		int w = 0, h = 0;
		/* Current transformed mode in native pixels, before output scale. */
		wlr_output_transformed_resolution(lo->output, &w, &h);
		if (w > width) {
			width = w;
		}
		if (h > height) {
			height = h;
		}
	}
	if (width <= 0 || height <= 0) {
		return false;
	}
	if (wayland != NULL) {
		*wayland = logical;
	}
	if (native != NULL) {
		*native = (struct wlr_box){
			.x = output_axis_rank(server->output_layout, logical.x, true) * width,
			.y = output_axis_rank(server->output_layout, logical.y, false) * height,
			.width = width, .height = height,
		};
	}
	return true;
}

bool vidow_xwm_native_union(const struct vidow_server *server,
		struct wlr_box *box) {
	if (box == NULL) {
		return false;
	}
	*box = (struct wlr_box){0};
	if (server == NULL || server->output_layout == NULL) {
		return false;
	}
	bool found = false;
	struct wlr_output_layout_output *lo;
	wl_list_for_each(lo, &server->output_layout->outputs, link) {
		struct wlr_box native;
		if (!vidow_xwm_output_boxes(server, lo->output, NULL, &native)) {
			continue;
		}
		if (native.x + native.width > box->width) {
			box->width = native.x + native.width;
		}
		if (native.y + native.height > box->height) {
			box->height = native.y + native.height;
		}
		found = true;
	}
	return found;
}

struct wlr_output *vidow_xwm_output_at_native(const struct vidow_server *server,
		double x, double y) {
	if (server == NULL || server->output_layout == NULL ||
			!isfinite(x) || !isfinite(y)) {
		return NULL;
	}
	struct wlr_output *best = NULL;
	double best_distance = DBL_MAX;
	struct wlr_output_layout_output *lo;
	wl_list_for_each(lo, &server->output_layout->outputs, link) {
		struct wlr_box native;
		if (!vidow_xwm_output_boxes(server, lo->output, NULL, &native)) {
			continue;
		}
		if (x >= native.x && x < native.x + native.width &&
				y >= native.y && y < native.y + native.height) {
			return lo->output;
		}
		double dx = fmax(fmax(native.x - x, 0.0), x - native.x - native.width);
		double dy = fmax(fmax(native.y - y, 0.0), y - native.y - native.height);
		double distance = dx * dx + dy * dy;
		/* Stable tie-break independent of output insertion order. */
		if (distance < best_distance || (distance == best_distance &&
				best != NULL && strcmp(lo->output->name, best->name) < 0)) {
			best = lo->output;
			best_distance = distance;
		}
	}
	return best;
}

static bool transform(const struct vidow_server *server, struct wlr_output *output,
		double x, double y, double *tx, double *ty, bool inverse, bool size) {
	/* A missing layout safely leaves coordinates unchanged. */
	if (tx != NULL) {
		*tx = x;
	}
	if (ty != NULL) {
		*ty = y;
	}
	struct wlr_box logical, native;
	if (!vidow_xwm_output_boxes(server, output, &logical, &native)) {
		return false;
	}
	struct wlr_box from = inverse ? native : logical;
	struct wlr_box to = inverse ? logical : native;
	if (tx != NULL) {
		*tx = (size ? x : x - from.x) * to.width / from.width +
			(size ? 0.0 : to.x);
	}
	if (ty != NULL) {
		*ty = (size ? y : y - from.y) * to.height / from.height +
			(size ? 0.0 : to.y);
	}
	return true;
}

bool vidow_xwm_to_native_point(const struct vidow_server *server,
		struct wlr_output *output, double x, double y, double *nx, double *ny) {
	return transform(server, output, x, y, nx, ny, false, false);
}
bool vidow_xwm_from_native_point(const struct vidow_server *server,
		struct wlr_output *output, double x, double y, double *lx, double *ly) {
	return transform(server, output, x, y, lx, ly, true, false);
}
bool vidow_xwm_to_native_size(const struct vidow_server *server,
		struct wlr_output *output, double w, double h, double *nw, double *nh) {
	return transform(server, output, w, h, nw, nh, false, true);
}
bool vidow_xwm_from_native_size(const struct vidow_server *server,
		struct wlr_output *output, double w, double h, double *lw, double *lh) {
	return transform(server, output, w, h, lw, lh, true, true);
}

/* Legacy scalar functions are X-axis LENGTH conversions only. Positions and
 * heights must use the explicit point/size API above (no integer scale exists
 * in the general mixed-DPI case). */
int vidow_xwm_scale(const struct vidow_view *view) {
	double scale = 1.0;
	if (view != NULL) {
		vidow_xwm_to_native_size(view->server, view->output, 1, 1, &scale, NULL);
	}
	return (int)fmax(1.0, round(scale));
}
int vidow_xwm_to_native(const struct vidow_view *view, int v) {
	double result = v;
	if (view != NULL) {
		vidow_xwm_to_native_size(view->server, view->output, v, 0, &result, NULL);
	}
	return (int)lround(result);
}
int vidow_xwm_from_native(const struct vidow_view *view, int v) {
	double result = v;
	if (view != NULL) {
		vidow_xwm_from_native_size(view->server, view->output, v, 0, &result, NULL);
	}
	return (int)lround(result);
}

struct wlr_box vidow_xwm_surface_box(const struct vidow_view *view) {
	struct wlr_box box = {0};
	if (view == NULL || view->x11 == NULL) {
		return box;
	}
	struct wlr_output *output = view->output;
	if (output == NULL) {
		output = vidow_xwm_output_at_native(view->server, view->x11->x,
			view->x11->y);
	}
	double x, y, w, h;
	vidow_xwm_from_native_point(view->server, output, view->x11->x,
		view->x11->y, &x, &y);
	vidow_xwm_from_native_size(view->server, output, view->x11->width,
		view->x11->height, &w, &h);
	return (struct wlr_box){(int)lround(x), (int)lround(y),
		(int)lround(w), (int)lround(h)};
}

void vidow_xwm_configure(struct vidow_view *view, int x, int y, int w, int h) {
	if (view == NULL || view->x11 == NULL) {
		return;
	}
	struct wlr_output *output = view->output;
	if (output == NULL && view->server->output_layout != NULL) {
		output = wlr_output_layout_output_at(view->server->output_layout, x, y);
	}
	double nx, ny, nw, nh;
	vidow_xwm_to_native_point(view->server, output, x, y, &nx, &ny);
	vidow_xwm_to_native_size(view->server, output, w, h, &nw, &nh);
	/* X ConfigureWindow uses signed 16-bit positions and unsigned sizes. */
	vidow_x11_surface_configure(view->x11,
		(int16_t)lround(fmax(INT16_MIN, fmin(INT16_MAX, nx))),
		(int16_t)lround(fmax(INT16_MIN, fmin(INT16_MAX, ny))),
		(uint16_t)lround(fmax(1, fmin(UINT16_MAX, nw))),
		(uint16_t)lround(fmax(1, fmin(UINT16_MAX, nh))));
}
