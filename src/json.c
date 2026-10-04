#include "json.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==================================================================== 写出 */

void jb_init(struct jbuf *b, size_t cap) {
	memset(b, 0, sizeof(*b));
	if (cap < 256) {
		cap = 256;
	}
	b->buf = malloc(cap);
	if (b->buf == NULL) {
		b->fail = true;
		return;
	}
	b->cap = cap;
	b->buf[0] = '\0';
}

void jb_free(struct jbuf *b) {
	free(b->buf);
	memset(b, 0, sizeof(*b));
}

void jb_reset(struct jbuf *b) {
	b->len = 0;
	b->depth = 0;
	b->pending = false;
	b->fail = b->buf == NULL;
	if (b->buf != NULL) {
		b->buf[0] = '\0';
	}
}

bool jb_ok(const struct jbuf *b) {
	return !b->fail && b->buf != NULL;
}

static bool jb_grow(struct jbuf *b, size_t need) {
	if (b->fail) {
		return false;
	}
	if (b->len + need + 1 <= b->cap) {
		return true;
	}
	size_t cap = b->cap;
	while (cap < b->len + need + 1) {
		cap *= 2;
	}
	char *p = realloc(b->buf, cap);
	if (p == NULL) {
		b->fail = true;
		return false;
	}
	b->buf = p;
	b->cap = cap;
	return true;
}

static void jb_put(struct jbuf *b, const char *s, size_t n) {
	if (!jb_grow(b, n)) {
		return;
	}
	memcpy(b->buf + b->len, s, n);
	b->len += n;
	b->buf[b->len] = '\0';
}

static void jb_putc(struct jbuf *b, char c) {
	jb_put(b, &c, 1);
}

/* 一个值前面的逗号。刚写完 key 的那个值不加（pending）。 */
static void jb_prefix(struct jbuf *b) {
	if (b->fail) {
		return;
	}
	if (b->pending) {
		b->pending = false;
		return;
	}
	if (b->depth > 0) {
		if (!b->first[b->depth - 1]) {
			jb_putc(b, ',');
		}
		b->first[b->depth - 1] = false;
	}
}

static bool jb_push(struct jbuf *b) {
	if (b->depth >= JB_MAX_DEPTH) {
		b->fail = true;
		return false;
	}
	b->first[b->depth] = true;
	b->depth++;
	return true;
}

void jb_obj(struct jbuf *b) {
	jb_prefix(b);
	if (jb_push(b)) {
		jb_putc(b, '{');
	}
}

void jb_obj_end(struct jbuf *b) {
	if (b->fail || b->depth == 0) {
		b->fail = true;
		return;
	}
	b->depth--;
	jb_putc(b, '}');
}

void jb_arr(struct jbuf *b) {
	jb_prefix(b);
	if (jb_push(b)) {
		jb_putc(b, '[');
	}
}

void jb_arr_end(struct jbuf *b) {
	if (b->fail || b->depth == 0) {
		b->fail = true;
		return;
	}
	b->depth--;
	jb_putc(b, ']');
}

static void jb_escaped(struct jbuf *b, const char *s) {
	jb_putc(b, '"');
	for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
		switch (*p) {
		case '"':
			jb_put(b, "\\\"", 2);
			break;
		case '\\':
			jb_put(b, "\\\\", 2);
			break;
		case '\n':
			jb_put(b, "\\n", 2);
			break;
		case '\r':
			jb_put(b, "\\r", 2);
			break;
		case '\t':
			jb_put(b, "\\t", 2);
			break;
		case '\b':
			jb_put(b, "\\b", 2);
			break;
		case '\f':
			jb_put(b, "\\f", 2);
			break;
		default:
			if (*p < 0x20) {
				char tmp[8];
				snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
				jb_put(b, tmp, 6);
			} else {
				jb_putc(b, (char)*p);
			}
			break;
		}
	}
	jb_putc(b, '"');
}

void jb_key(struct jbuf *b, const char *key) {
	if (b->fail) {
		return;
	}
	if (b->depth == 0) {
		b->fail = true; /* key 只能写在对象里 */
		return;
	}
	if (!b->pending && !b->first[b->depth - 1]) {
		jb_putc(b, ',');
	}
	b->first[b->depth - 1] = false;
	jb_escaped(b, key);
	jb_putc(b, ':');
	b->pending = true;
}

void jb_str(struct jbuf *b, const char *s) {
	if (s == NULL) {
		jb_null(b);
		return;
	}
	jb_prefix(b);
	jb_escaped(b, s);
}

void jb_raw(struct jbuf *b, const char *raw) {
	if (raw == NULL) {
		jb_null(b);
		return;
	}
	jb_prefix(b);
	jb_put(b, raw, strlen(raw));
}

void jb_int(struct jbuf *b, long long v) {
	char tmp[32];
	snprintf(tmp, sizeof(tmp), "%lld", v);
	jb_prefix(b);
	jb_put(b, tmp, strlen(tmp));
}

void jb_uint(struct jbuf *b, unsigned long long v) {
	char tmp[32];
	snprintf(tmp, sizeof(tmp), "%llu", v);
	jb_prefix(b);
	jb_put(b, tmp, strlen(tmp));
}

void jb_double(struct jbuf *b, double v) {
	if (!isfinite(v)) {
		jb_null(b); /* JSON 没有 NaN / Inf */
		return;
	}
	char tmp[40];
	/* %.6g：坐标和缩放都够用，也不会把 0.1 写成 0.10000000000000001 */
	snprintf(tmp, sizeof(tmp), "%.6g", v);
	jb_prefix(b);
	jb_put(b, tmp, strlen(tmp));
}

void jb_bool(struct jbuf *b, bool v) {
	jb_prefix(b);
	if (v) {
		jb_put(b, "true", 4);
	} else {
		jb_put(b, "false", 5);
	}
}

void jb_null(struct jbuf *b) {
	jb_prefix(b);
	jb_put(b, "null", 4);
}

void jb_kv_str(struct jbuf *b, const char *key, const char *v) {
	jb_key(b, key);
	jb_str(b, v);
}

void jb_kv_int(struct jbuf *b, const char *key, long long v) {
	jb_key(b, key);
	jb_int(b, v);
}

void jb_kv_uint(struct jbuf *b, const char *key, unsigned long long v) {
	jb_key(b, key);
	jb_uint(b, v);
}

void jb_kv_double(struct jbuf *b, const char *key, double v) {
	jb_key(b, key);
	jb_double(b, v);
}

void jb_kv_bool(struct jbuf *b, const char *key, bool v) {
	jb_key(b, key);
	jb_bool(b, v);
}

void jb_kv_null(struct jbuf *b, const char *key) {
	jb_key(b, key);
	jb_null(b);
}

/* ==================================================================== 读入 */

#define JP_MAX_DEPTH 32
/* 一条消息里字符串的长度上限（也是整条消息的上限，见 ipc.c）。 */
#define JP_MAX_STRING (1u << 20)

struct jp {
	const char *s;
	size_t i;
	size_t n;
	int depth;
	bool fail;
};

static void jp_err(struct jp *p, char *err, size_t errlen, const char *what) {
	if (!p->fail && err != NULL) {
		snprintf(err, errlen, "第 %zu 个字符：%s", p->i + 1, what);
	}
	p->fail = true;
}

static void jp_ws(struct jp *p) {
	while (p->i < p->n) {
		char c = p->s[p->i];
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			p->i++;
		} else {
			return;
		}
	}
}

static struct jval *jp_value(struct jp *p, char *err, size_t errlen);

static struct jval *jval_new(enum jtype t) {
	struct jval *v = calloc(1, sizeof(*v));
	if (v != NULL) {
		v->t = t;
	}
	return v;
}

void jfree(struct jval *v) {
	if (v == NULL) {
		return;
	}
	free(v->str);
	for (size_t i = 0; i < v->n; i++) {
		jfree(v->items[i]);
	}
	if (v->keys != NULL) {
		for (size_t i = 0; i < v->n; i++) {
			free(v->keys[i]);
		}
		free(v->keys);
	}
	free(v->items);
	free(v);
}

/* 往动态数组里塞一个（对象用 keys，数组不用）。 */
static bool jpush(struct jval *v, char *key, struct jval *item) {
	if (v->n == SIZE_MAX / sizeof(void *)) {
		return false;
	}
	struct jval **items = realloc(v->items, (v->n + 1) * sizeof(*items));
	if (items == NULL) {
		return false;
	}
	v->items = items;
	v->items[v->n] = item;
	if (key != NULL || v->t == J_OBJ) {
		char **keys = realloc(v->keys, (v->n + 1) * sizeof(*keys));
		if (keys == NULL) {
			return false;
		}
		v->keys = keys;
		v->keys[v->n] = key;
	}
	v->n++;
	return true;
}

static int hex4(const char *s) {
	int out = 0;
	for (int i = 0; i < 4; i++) {
		char c = s[i];
		int d;
		if (c >= '0' && c <= '9') {
			d = c - '0';
		} else if (c >= 'a' && c <= 'f') {
			d = c - 'a' + 10;
		} else if (c >= 'A' && c <= 'F') {
			d = c - 'A' + 10;
		} else {
			return -1;
		}
		out = out * 16 + d;
	}
	return out;
}

/* 追加一个字节（原样的字节，不是码点 —— 输入里的 UTF-8 要一个字节一个字节
 * 搬过去，不能拿它当码点再编码一遍）。 */
static void jp_byte(char **buf, size_t *len, size_t *cap, unsigned char c) {
	if (*len + 2 >= *cap) {
		size_t want = *cap * 2 + 8;
		char *p = realloc(*buf, want);
		if (p == NULL) {
			return;
		}
		*buf = p;
		*cap = want;
	}
	(*buf)[(*len)++] = (char)c;
}

static void utf8_put(char **buf, size_t *len, size_t *cap, unsigned int cp) {
	if (*len + 4 >= *cap) {
		size_t want = *cap * 2 + 8;
		char *p = realloc(*buf, want);
		if (p == NULL) {
			return;
		}
		*buf = p;
		*cap = want;
	}
	char *b = *buf;
	if (cp < 0x80) {
		b[(*len)++] = (char)cp;
	} else if (cp < 0x800) {
		b[(*len)++] = (char)(0xc0 | (cp >> 6));
		b[(*len)++] = (char)(0x80 | (cp & 0x3f));
	} else if (cp < 0x10000) {
		b[(*len)++] = (char)(0xe0 | (cp >> 12));
		b[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3f));
		b[(*len)++] = (char)(0x80 | (cp & 0x3f));
	} else {
		b[(*len)++] = (char)(0xf0 | (cp >> 18));
		b[(*len)++] = (char)(0x80 | ((cp >> 12) & 0x3f));
		b[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3f));
		b[(*len)++] = (char)(0x80 | (cp & 0x3f));
	}
}

static char *jp_string(struct jp *p, char *err, size_t errlen) {
	if (p->i >= p->n || p->s[p->i] != '"') {
		jp_err(p, err, errlen, "期望一个字符串");
		return NULL;
	}
	p->i++;
	size_t cap = 32;
	size_t len = 0;
	char *out = malloc(cap);
	if (out == NULL) {
		jp_err(p, err, errlen, "内存不够");
		return NULL;
	}
	while (p->i < p->n) {
		unsigned char c = (unsigned char)p->s[p->i];
		if (c == '"') {
			p->i++;
			out[len] = '\0';
			return out;
		}
		if (c == '\\') {
			p->i++;
			if (p->i >= p->n) {
				break;
			}
			char e = p->s[p->i++];
			switch (e) {
			case '"':
				jp_byte(&out, &len, &cap, '"');
				break;
			case '\\':
				jp_byte(&out, &len, &cap, '\\');
				break;
			case '/':
				jp_byte(&out, &len, &cap, '/');
				break;
			case 'b':
				jp_byte(&out, &len, &cap, '\b');
				break;
			case 'f':
				jp_byte(&out, &len, &cap, '\f');
				break;
			case 'n':
				jp_byte(&out, &len, &cap, '\n');
				break;
			case 'r':
				jp_byte(&out, &len, &cap, '\r');
				break;
			case 't':
				jp_byte(&out, &len, &cap, '\t');
				break;
			case 'u': {
				if (p->i + 4 > p->n) {
					jp_err(p, err, errlen, "\\u 后面要跟 4 位十六进制");
					free(out);
					return NULL;
				}
				int cp = hex4(p->s + p->i);
				if (cp < 0) {
					jp_err(p, err, errlen, "\\u 后面要跟 4 位十六进制");
					free(out);
					return NULL;
				}
				p->i += 4;
				/* 代理对：高位 + 低位拼成一个码点，单独的半个当替换字符。 */
				if (cp >= 0xd800 && cp <= 0xdbff && p->i + 6 <= p->n &&
						p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
					int lo = hex4(p->s + p->i + 2);
					if (lo >= 0xdc00 && lo <= 0xdfff) {
						cp = 0x10000 + ((cp - 0xd800) << 10) +
							(lo - 0xdc00);
						p->i += 6;
					}
				} else if (cp >= 0xd800 && cp <= 0xdfff) {
					cp = 0xfffd;
				}
				utf8_put(&out, &len, &cap, (unsigned int)cp);
				break;
			}
			default:
				jp_err(p, err, errlen, "认不出来的转义");
				free(out);
				return NULL;
			}
			continue;
		}
		if (c < 0x20) {
			jp_err(p, err, errlen, "字符串里有没转义的控制字符");
			free(out);
			return NULL;
		}
		jp_byte(&out, &len, &cap, c);
		p->i++;
		if (len > JP_MAX_STRING) {
			jp_err(p, err, errlen, "字符串太长了");
			free(out);
			return NULL;
		}
	}
	jp_err(p, err, errlen, "字符串没有收尾的引号");
	free(out);
	return NULL;
}

static struct jval *jp_number(struct jp *p, char *err, size_t errlen) {
	const char *start = p->s + p->i;
	char *end = NULL;
	double v = strtod(start, &end);
	if (end == start) {
		jp_err(p, err, errlen, "期望一个数字");
		return NULL;
	}
	p->i += (size_t)(end - start);
	struct jval *out = jval_new(J_NUM);
	if (out == NULL) {
		jp_err(p, err, errlen, "内存不够");
		return NULL;
	}
	out->num = v;
	return out;
}

static struct jval *jp_value(struct jp *p, char *err, size_t errlen) {
	jp_ws(p);
	if (p->i >= p->n) {
		jp_err(p, err, errlen, "还没写完就结束了");
		return NULL;
	}
	char c = p->s[p->i];

	if (c == '{' || c == '[') {
		if (p->depth >= JP_MAX_DEPTH) {
			jp_err(p, err, errlen, "嵌套太深");
			return NULL;
		}
		bool obj = c == '{';
		p->i++;
		p->depth++;
		struct jval *v = jval_new(obj ? J_OBJ : J_ARR);
		if (v == NULL) {
			jp_err(p, err, errlen, "内存不够");
			return NULL;
		}
		for (;;) {
			jp_ws(p);
			if (p->i >= p->n) {
				jp_err(p, err, errlen, "容器没有收尾");
				jfree(v);
				return NULL;
			}
			if (p->s[p->i] == (obj ? '}' : ']')) {
				p->i++;
				p->depth--;
				return v;
			}
			if (v->n > 0) {
				if (p->s[p->i] != ',') {
					jp_err(p, err, errlen, "元素之间要有个逗号");
					jfree(v);
					return NULL;
				}
				p->i++;
				jp_ws(p);
			}
			char *key = NULL;
			if (obj) {
				key = jp_string(p, err, errlen);
				if (key == NULL) {
					jfree(v);
					return NULL;
				}
				jp_ws(p);
				if (p->i >= p->n || p->s[p->i] != ':') {
					jp_err(p, err, errlen, "key 后面要有个冒号");
					free(key);
					jfree(v);
					return NULL;
				}
				p->i++;
			}
			struct jval *item = jp_value(p, err, errlen);
			if (item == NULL) {
				free(key);
				jfree(v);
				return NULL;
			}
			if (!jpush(v, key, item)) {
				jp_err(p, err, errlen, "内存不够");
				free(key);
				jfree(item);
				jfree(v);
				return NULL;
			}
		}
	}

	if (c == '"') {
		char *s = jp_string(p, err, errlen);
		if (s == NULL) {
			return NULL;
		}
		struct jval *v = jval_new(J_STR);
		if (v == NULL) {
			free(s);
			jp_err(p, err, errlen, "内存不够");
			return NULL;
		}
		v->str = s;
		return v;
	}

	if (strncmp(p->s + p->i, "true", 4) == 0) {
		p->i += 4;
		struct jval *v = jval_new(J_BOOL);
		if (v != NULL) {
			v->b = true;
		}
		return v;
	}
	if (strncmp(p->s + p->i, "false", 5) == 0) {
		p->i += 5;
		struct jval *v = jval_new(J_BOOL);
		if (v != NULL) {
			v->b = false;
		}
		return v;
	}
	if (strncmp(p->s + p->i, "null", 4) == 0) {
		p->i += 4;
		return jval_new(J_NULL);
	}
	if (c == '-' || (c >= '0' && c <= '9')) {
		return jp_number(p, err, errlen);
	}

	jp_err(p, err, errlen, "认不出来的值");
	return NULL;
}

struct jval *jparse(const char *text, char *err, size_t errlen) {
	if (err != NULL && errlen > 0) {
		err[0] = '\0';
	}
	if (text == NULL) {
		if (err != NULL) {
			snprintf(err, errlen, "空的请求");
		}
		return NULL;
	}
	struct jp p = { .s = text, .n = strlen(text) };
	struct jval *v = jp_value(&p, err, errlen);
	if (v == NULL) {
		return NULL;
	}
	jp_ws(&p);
	if (p.i != p.n) {
		jp_err(&p, err, errlen, "一条消息只能有一个值");
		jfree(v);
		return NULL;
	}
	return v;
}

struct jval *jget(const struct jval *obj, const char *key) {
	if (obj == NULL || obj->t != J_OBJ || key == NULL) {
		return NULL;
	}
	for (size_t i = 0; i < obj->n; i++) {
		if (obj->keys[i] != NULL && strcmp(obj->keys[i], key) == 0) {
			return obj->items[i];
		}
	}
	return NULL;
}

const char *jget_str(const struct jval *obj, const char *key) {
	struct jval *v = jget(obj, key);
	return v != NULL && v->t == J_STR ? v->str : NULL;
}

bool jget_bool(const struct jval *obj, const char *key, bool *out) {
	struct jval *v = jget(obj, key);
	if (v == NULL || v->t != J_BOOL) {
		return false;
	}
	*out = v->b;
	return true;
}

bool jget_int(const struct jval *obj, const char *key, long long *out) {
	struct jval *v = jget(obj, key);
	if (v == NULL || v->t != J_NUM) {
		return false;
	}
	*out = (long long)v->num;
	return true;
}

bool jget_double(const struct jval *obj, const char *key, double *out) {
	struct jval *v = jget(obj, key);
	if (v == NULL || v->t != J_NUM) {
		return false;
	}
	*out = v->num;
	return true;
}
