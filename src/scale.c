#include "scale.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* 逻辑像素的基准密度。1 逻辑像素 = 1/96 英寸，所以 96ppi 的屏幕 scale = 1，
 * 163ppi 的 4K 27 寸 scale ≈ 1.70 —— 两块屏各按自己的 ppi 得到各自的缩放，
 * 同一个逻辑尺寸在两块屏上就是同一个物理尺寸。 */
#define VIDOW_BASE_DPI 96.0

/* 量化步长 1/20。0.05 的粒度足够贴近真实 ppi 比例，又不会让缩放值变成
 * 一串无意义的尾数。 */
#define VIDOW_SCALE_STEP 0.05

#define VIDOW_SCALE_MIN 1.0
#define VIDOW_SCALE_MAX 8.0

bool vidow_scale_parse(const char *text, struct vidow_scale_spec *out) {
	if (text == NULL) {
		return false;
	}
	while (isspace((unsigned char)*text)) {
		text++;
	}
	if (*text == '\0') {
		return false;
	}

	if (strncmp(text, "auto", 4) == 0 && text[4] == '\0') {
		out->kind = VIDOW_SCALE_AUTO;
		out->value = 0.0;
		return true;
	}
	if (strncmp(text, "off", 3) == 0 && text[3] == '\0') {
		out->kind = VIDOW_SCALE_OFF;
		out->value = 1.0;
		return true;
	}
	if (strncmp(text, "none", 4) == 0 && text[4] == '\0') {
		out->kind = VIDOW_SCALE_OFF;
		out->value = 1.0;
		return true;
	}

	char *end = NULL;
	double value = strtod(text, &end);
	if (end == text || value <= 0.0) {
		return false;
	}
	while (isspace((unsigned char)*end)) {
		end++;
	}
	if (*end != '\0') {
		return false; /* "1.5x" 这种不算 */
	}
	out->kind = VIDOW_SCALE_FIXED;
	out->value = value;
	return true;
}

double vidow_scale_from_dpi(int w, int h, int mm_w, int mm_h) {
	if (w <= 0 || h <= 0 || mm_w <= 0 || mm_h <= 0) {
		return 0.0; /* 没有 EDID 尺寸 */
	}
	double diag_px = sqrt((double)w * (double)w + (double)h * (double)h);
	double diag_in = sqrt((double)mm_w * (double)mm_w +
		(double)mm_h * (double)mm_h) / 25.4;
	if (diag_in <= 0.0) {
		return 0.0;
	}
	return (diag_px / diag_in) / VIDOW_BASE_DPI;
}

double vidow_scale_quantize(double scale, double min, double max) {
	if (!(scale > 0.0)) {
		return min;
	}
	double stepped = floor(scale / VIDOW_SCALE_STEP + 0.5) * VIDOW_SCALE_STEP;
	if (stepped < min) {
		stepped = min;
	}
	if (stepped > max) {
		stepped = max;
	}
	return stepped;
}

/* 一个 token 形如 NAME:SCALE。名字里不会有空格或逗号，缩放在最后一段。 */
static bool lookup_token(const char *token, size_t len, const char *name,
		struct vidow_scale_spec *out) {
	if (len == 0) {
		return false;
	}
	const char *colon = NULL;
	for (size_t i = 0; i < len; i++) {
		if (token[i] == ':') {
			colon = token + i;
		}
	}
	if (colon == NULL) {
		return false;
	}
	size_t name_len = (size_t)(colon - token);
	if (strlen(name) != name_len || strncmp(token, name, name_len) != 0) {
		return false;
	}
	char value[48];
	size_t value_len = len - name_len - 1;
	if (value_len == 0 || value_len >= sizeof(value)) {
		return false;
	}
	memcpy(value, colon + 1, value_len);
	value[value_len] = '\0';
	return vidow_scale_parse(value, out);
}

bool vidow_scale_lookup(const char *outputs, const char *name,
		struct vidow_scale_spec *out) {
	if (outputs == NULL || name == NULL || out == NULL) {
		return false;
	}
	const char *p = outputs;
	while (*p != '\0') {
		while (*p != '\0' && (isspace((unsigned char)*p) || *p == ',' ||
				*p == ';')) {
			p++;
		}
		const char *start = p;
		while (*p != '\0' && !isspace((unsigned char)*p) && *p != ',' &&
				*p != ';') {
			p++;
		}
		if (p != start && lookup_token(start, (size_t)(p - start), name, out)) {
			return true;
		}
	}
	return false;
}

double vidow_scale_resolve(const char *name, const char *global,
		const char *outputs, int w, int h, int mm_w, int mm_h) {
	struct vidow_scale_spec spec = { .kind = VIDOW_SCALE_AUTO, .value = 0.0 };

	if (name != NULL && !vidow_scale_lookup(outputs, name, &spec)) {
		if (global == NULL || !vidow_scale_parse(global, &spec)) {
			spec.kind = VIDOW_SCALE_AUTO;
		}
	}

	switch (spec.kind) {
	case VIDOW_SCALE_FIXED:
		return vidow_scale_quantize(spec.value, VIDOW_SCALE_MIN,
			VIDOW_SCALE_MAX);
	case VIDOW_SCALE_OFF:
		return 1.0;
	case VIDOW_SCALE_AUTO:
		break;
	}

	double auto_scale = vidow_scale_from_dpi(w, h, mm_w, mm_h);
	if (auto_scale <= 0.0) {
		return 1.0; /* 没有 EDID：按原生分辨率，1:1 最清晰 */
	}
	return vidow_scale_quantize(auto_scale, VIDOW_SCALE_MIN, VIDOW_SCALE_MAX);
}
