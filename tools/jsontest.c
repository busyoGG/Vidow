/* JSON 的离线自检：不启动合成器，直接对着写出 / 读入的结果验。
 *   meson compile -C build jsontest && ./build/jsontest
 * 退出码 0 = 全过。
 *
 * 为什么值得单测：IPC 的每一条消息都从这儿过，而它是**外部输入**的第一道门 ——
 * 解析器必须能拒绝，不能崩。 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "json.h"

static int failures;

static void check(bool ok, const char *what) {
	if (!ok) {
		failures++;
		printf("FAIL %s\n", what);
	} else {
		printf("ok   %s\n", what);
	}
}

static void check_str(const char *got, const char *want, const char *what) {
	if (got == NULL || strcmp(got, want) != 0) {
		failures++;
		printf("FAIL %s\n     得到: %s\n     期望: %s\n", what,
			got != NULL ? got : "(null)", want);
	} else {
		printf("ok   %s\n", what);
	}
}

/* ------------------------------------------------------------ 写出 */

static void test_write(void) {
	printf("== 写出 ==\n");
	struct jbuf b;

	jb_init(&b, 8); /* 故意小：逼它自己扩容 */
	jb_obj(&b);
	jb_kv_str(&b, "title", "演示");
	jb_kv_int(&b, "id", -12);
	jb_kv_uint(&b, "big", 18446744073709551615ull);
	jb_kv_double(&b, "scale", 1.5);
	jb_kv_bool(&b, "floating", true);
	jb_kv_null(&b, "parent");
	jb_key(&b, "geometry");
	jb_obj(&b);
	jb_kv_int(&b, "x", 6);
	jb_kv_int(&b, "y", 8);
	jb_obj_end(&b);
	jb_key(&b, "windows");
	jb_arr(&b);
	jb_int(&b, 1);
	jb_int(&b, 2);
	jb_arr_end(&b);
	jb_key(&b, "empty_obj");
	jb_obj(&b);
	jb_obj_end(&b);
	jb_key(&b, "empty_arr");
	jb_arr(&b);
	jb_arr_end(&b);
	jb_key(&b, "nested");
	jb_arr(&b);
	jb_arr(&b);
	jb_obj(&b);
	jb_kv_bool(&b, "deep", false);
	jb_obj_end(&b);
	jb_arr_end(&b);
	jb_arr_end(&b);
	jb_obj_end(&b);

	check(jb_ok(&b), "写出没出错");
	check_str(b.buf,
		"{\"title\":\"演示\",\"id\":-12,\"big\":18446744073709551615,"
		"\"scale\":1.5,\"floating\":true,\"parent\":null,"
		"\"geometry\":{\"x\":6,\"y\":8},\"windows\":[1,2],"
		"\"empty_obj\":{},\"empty_arr\":[],"
		"\"nested\":[[{\"deep\":false}]]}",
		"一个完整的对象：键序、逗号、空容器都对");
	jb_free(&b);

	/* 转义 */
	jb_init(&b, 8);
	jb_obj(&b);
	jb_kv_str(&b, "s", "引号\" 反斜杠\\ 换行\n 制表\t 控制\x01");
	jb_kv_str(&b, "u", "中文 ok");
	jb_obj_end(&b);
	check_str(b.buf,
		"{\"s\":\"引号\\\" 反斜杠\\\\ 换行\\n 制表\\t 控制\\u0001\","
		"\"u\":\"中文 ok\"}",
		"字符串转义（引号 / 反斜杠 / 换行 / 制表 / 控制字符）");
	jb_free(&b);

	/* 非有限数当 null（JSON 没有 NaN / Inf） */
	jb_init(&b, 8);
	jb_arr(&b);
	jb_double(&b, 0.0 / 0.0);
	jb_double(&b, 1.0 / 0.0);
	jb_arr_end(&b);
	check_str(b.buf, "[null,null]", "NaN / Inf 写成 null");
	jb_free(&b);

	/* 复用：reset 之后从头拼 */
	jb_init(&b, 8);
	jb_obj(&b);
	jb_kv_int(&b, "a", 1);
	jb_obj_end(&b);
	jb_reset(&b);
	jb_obj(&b);
	jb_kv_int(&b, "b", 2);
	jb_obj_end(&b);
	check_str(b.buf, "{\"b\":2}", "reset 之后能复用同一块缓冲");
	jb_free(&b);
}

/* ------------------------------------------------------------ 读入 */

static struct jval *parse_ok(const char *text, const char *what) {
	char err[128];
	struct jval *v = jparse(text, err, sizeof(err));
	if (v == NULL) {
		failures++;
		printf("FAIL %s（不该失败：%s）\n", what, err);
	}
	return v;
}

static void parse_bad(const char *text, const char *what) {
	char err[128];
	struct jval *v = jparse(text, err, sizeof(err));
	if (v != NULL) {
		failures++;
		printf("FAIL %s（本该拒绝，却收了）\n", what);
		jfree(v);
		return;
	}
	if (err[0] == '\0') {
		failures++;
		printf("FAIL %s（拒绝了但没给原因）\n", what);
		return;
	}
	printf("ok   %s（%s）\n", what, err);
}

static void test_read(void) {
	printf("\n== 读入 ==\n");

	struct jval *v = parse_ok("{\"cmd\":\"windows\",\"id\":3,"
		"\"app_id\":\"mpv\",\"f\":true,\"n\":null,\"a\":[1,2,3],"
		"\"o\":{\"x\":-1.5}}", "一个正常的请求");
	if (v != NULL) {
		check_str(jget_str(v, "cmd"), "windows", "取字符串");
		check(jget_str(v, "id") == NULL, "类型不对（数字当字符串取）返回 NULL");
		long long id = 0;
		check(jget_int(v, "id", &id) && id == 3, "取整数");
		bool f = false;
		check(jget_bool(v, "f", &f) && f, "取布尔");
		double x = 0.0;
		struct jval *o = jget(v, "o");
		check(o != NULL && jget_double(o, "x", &x) && x == -1.5,
			"取嵌套对象里的负数");
		struct jval *a = jget(v, "a");
		check(a != NULL && a->t == J_ARR && a->n == 3 &&
			a->items[2]->t == J_NUM && a->items[2]->num == 3.0,
			"取数组和元素");
		check(jget(v, "没有这个") == NULL, "没有的键返回 NULL");
		jfree(v);
	}

	/* 空白、指数、负零 */
	v = parse_ok("  \n\t { \"a\" : [ 1e3 , -0.5 ] }  ", "前后空白 / 指数 / 负数");
	if (v != NULL) {
		struct jval *a = jget(v, "a");
		check(a != NULL && a->n == 2 && a->items[0]->num == 1000.0 &&
			a->items[1]->num == -0.5, "指数和负数解析对");
		jfree(v);
	}

	/* 转义与 \u */
	v = parse_ok("{\"s\":\"a\\\"b\\\\c\\nd\\u4e2d\\u6587\"}", "转义和 \\u");
	if (v != NULL) {
		check_str(jget_str(v, "s"), "a\"b\\c\nd中文", "\\u4e2d\\u6587 -> 中文");
		jfree(v);
	}
	v = parse_ok("{\"s\":\"\\ud83d\\ude00\"}", "代理对");
	if (v != NULL) {
		check_str(jget_str(v, "s"), "\xf0\x9f\x98\x80", "代理对拼成一个码点");
		jfree(v);
	}
	v = parse_ok("{\"s\":\"\\ud800\"}", "落单的高位代理");
	if (v != NULL) {
		check_str(jget_str(v, "s"), "\xef\xbf\xbd", "落单代理 -> 替换字符");
		jfree(v);
	}

	/* 空容器 */
	v = parse_ok("{\"a\":[],\"b\":{}}", "空数组 / 空对象");
	if (v != NULL) {
		check(jget(v, "a")->n == 0 && jget(v, "b")->n == 0, "空容器解析成 0 个元素");
		jfree(v);
	}

	/* 拒绝的东西 */
	parse_bad("", "空输入");
	parse_bad("   ", "只有空白");
	parse_bad("{\"a\":1", "对象没闭合");
	parse_bad("{\"a\" 1}", "键后面没有冒号");
	parse_bad("{\"a\":1,}", "多一个逗号");
	parse_bad("[1 2]", "数组元素之间没有逗号");
	parse_bad("{\"a\":\"没闭合}", "字符串没闭合");
	parse_bad("{\"a\":\"\\x\"}", "认不出来的转义");
	parse_bad("{\"a\":\"\x01\"}", "字符串里裸的控制字符");
	parse_bad("{\"a\":\"\\u12\"}", "\\u 后面位数不够");
	parse_bad("{\"a\":1} 尾巴", "一个值后面还有东西");
	parse_bad("nan", "认不出来的字面量");
	parse_bad("{\"a\":1}{}", "两条消息拼在一起（一条请求只能有一个值）");

	/* 嵌套深度：深到超过上限要被拒绝，而不是把栈吃穿 */
	char deep[512];
	size_t n = 0;
	for (int i = 0; i < 40 && n + 1 < sizeof(deep); i++) {
		deep[n++] = '[';
	}
	for (int i = 0; i < 40 && n + 1 < sizeof(deep); i++) {
		deep[n++] = ']';
	}
	deep[n] = '\0';
	parse_bad(deep, "40 层嵌套（上限 32）");

	/* 恰好在上限内的还能过 */
	n = 0;
	for (int i = 0; i < 8; i++) {
		deep[n++] = '[';
	}
	for (int i = 0; i < 8; i++) {
		deep[n++] = ']';
	}
	deep[n] = '\0';
	v = parse_ok(deep, "8 层嵌套");
	if (v != NULL) {
		jfree(v);
	}

	/* 写出 -> 读入 回环 */
	printf("\n== 回环 ==\n");
	struct jbuf b;
	jb_init(&b, 8);
	jb_obj(&b);
	jb_kv_str(&b, "title", "带\"引号\"和\\反斜杠 的标题");
	jb_kv_int(&b, "n", -3);
	jb_kv_double(&b, "d", 0.125);
	jb_kv_bool(&b, "t", true);
	jb_key(&b, "arr");
	jb_arr(&b);
	jb_str(&b, "中文");
	jb_null(&b);
	jb_arr_end(&b);
	jb_obj_end(&b);
	v = parse_ok(b.buf, "自己写出来的能读回来");
	if (v != NULL) {
		check_str(jget_str(v, "title"), "带\"引号\"和\\反斜杠 的标题",
			"转义过的字符串读回来一模一样");
		long long n2 = 0;
		double d = 0.0;
		check(jget_int(v, "n", &n2) && n2 == -3, "整数回环");
		check(jget_double(v, "d", &d) && d == 0.125, "小数回环");
		struct jval *arr = jget(v, "arr");
		check(arr != NULL && arr->n == 2 && arr->items[1]->t == J_NULL,
			"数组里的 null 回环");
		jfree(v);
	}
	jb_free(&b);
}

/* 写出失败（内存不够）不该崩：用一个一定会失败的小对象模拟不了，跳过；
 * 但空 jbuf 上乱调不该崩。 */
static void test_robust(void) {
	printf("\n== 健壮性 ==\n");
	struct jbuf b;
	jb_init(&b, 256);
	/* 深度不配对的收尾：置 fail 而不是越界写 */
	jb_obj(&b);
	jb_obj_end(&b);
	jb_obj_end(&b);
	check(!jb_ok(&b), "多余的 obj_end 只把 buf 标成失败，不越界");
	jb_free(&b);

	/* 顶层直接写值（没有容器）：合法，也是最简单的一条应答 */
	jb_init(&b, 256);
	jb_int(&b, 7);
	check_str(b.buf, "7", "顶层裸值");
	jb_free(&b);

	/* NULL 字符串当 null */
	jb_init(&b, 256);
	jb_arr(&b);
	jb_str(&b, NULL);
	jb_arr_end(&b);
	check_str(b.buf, "[null]", "jb_str(NULL) -> null");
	jb_free(&b);
}

int main(void) {
	test_write();
	test_read();
	test_robust();
	printf("\n%s（%d 项失败）\n", failures == 0 ? "全部通过" : "有失败", failures);
	return failures == 0 ? 0 : 1;
}
