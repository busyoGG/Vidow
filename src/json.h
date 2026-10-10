/* 极简 JSON：写出（jbuf）+ 读入（jval）。IPC 的每条消息都走这里。
 *
 * 为什么自己写：需要的东西很少（对象/数组/字符串/数字/布尔），但不引第三方库才好
 * 控制大小上限、转义和失败行为 —— IPC 是外部输入的第一道门，解析器必须能拒绝而不是
 * 崩溃。这一份**不碰 wlroots**，所以 tools/jsontest.c 能直接编进去对着断言跑。
 *
 * 写出永远是"拼一个文本"：jb_* 系列在失败（内存不够）之后全部变成空操作，
 * 最后问 jb_ok()。读入是一次性解析整段文本，失败给出行列位置。 */
#ifndef VIDOW_JSON_H
#define VIDOW_JSON_H

#include <stdbool.h>
#include <stddef.h>

/* --------------------------------------------------------------- 写出 */

/* 嵌套层数上限。IPC 的消息都是扁平的，留 24 层足够，也免得恶意输入把栈吃穿。 */
#define JB_MAX_DEPTH 24

struct jbuf {
	char *buf;      /* NUL 结尾，len 不含结尾那个 0 */
	size_t len;
	size_t cap;
	int depth;
	/* 当前这层容器里还没写过东西吗（决定要不要补逗号） */
	bool first[JB_MAX_DEPTH];
	/* 刚写完一个 key，下一个值是它的值：不要再补逗号 */
	bool pending;
	bool fail;      /* 出过错：之后全是空操作 */
};

void jb_init(struct jbuf *b, size_t cap);
void jb_free(struct jbuf *b);
/* 内容清空，留着已分配的内存（一条连接反复用）。 */
void jb_reset(struct jbuf *b);
bool jb_ok(const struct jbuf *b);

void jb_obj(struct jbuf *b);
void jb_obj_end(struct jbuf *b);
void jb_arr(struct jbuf *b);
void jb_arr_end(struct jbuf *b);
void jb_key(struct jbuf *b, const char *key);
void jb_str(struct jbuf *b, const char *s); /* NULL → null */
/* 已经拼好的 JSON 片段（不转义、不检查）—— 用来把一个 jbuf 嵌进另一个。 */
void jb_raw(struct jbuf *b, const char *raw);
void jb_int(struct jbuf *b, long long v);
void jb_uint(struct jbuf *b, unsigned long long v);
void jb_double(struct jbuf *b, double v);
void jb_bool(struct jbuf *b, bool v);
void jb_null(struct jbuf *b);

void jb_kv_str(struct jbuf *b, const char *key, const char *v);
void jb_kv_int(struct jbuf *b, const char *key, long long v);
void jb_kv_uint(struct jbuf *b, const char *key, unsigned long long v);
void jb_kv_double(struct jbuf *b, const char *key, double v);
void jb_kv_bool(struct jbuf *b, const char *key, bool v);
void jb_kv_null(struct jbuf *b, const char *key);

/* --------------------------------------------------------------- 读入 */

enum jtype {
	J_NULL = 0,
	J_BOOL,
	J_NUM,
	J_STR,
	J_ARR,
	J_OBJ,
};

struct jval {
	enum jtype t;
	bool b;
	double num;
	char *str;             /* J_STR */
	struct jval **items;   /* J_ARR / J_OBJ */
	char **keys;           /* J_OBJ */
	size_t n;
};

/* 解析一整段文本（一条消息）。成功返回新对象（用完 jfree），失败返回 NULL 并把
 * 原因写进 err（不超过 errlen）。 */
struct jval *jparse(const char *text, char *err, size_t errlen);
void jfree(struct jval *v);

/* 取字段：没有这个字段 / 类型不对 → NULL（jget_* 返回 false）。 */
struct jval *jget(const struct jval *obj, const char *key);
const char *jget_str(const struct jval *obj, const char *key);
bool jget_bool(const struct jval *obj, const char *key, bool *out);
bool jget_int(const struct jval *obj, const char *key, long long *out);
bool jget_double(const struct jval *obj, const char *key, double *out);

/* 把一个解析出来的值再写回 JSON 文本（IPC 的 emit 命令要原样转发调用方给的
 * data）。写进已有的 jbuf，失败走 jb_* 那套（最后问 jb_ok）。 */
void jval_json(struct jbuf *b, const struct jval *v);

#endif /* VIDOW_JSON_H */
