/*
 * cursorprobe.c — 当一路 screencast 流的消费者，把缓冲里的光标元数据
 * （SPA_META_Cursor）读出来打印。用来验证 vidow 录屏的 pointer_mode=metadata。
 *
 * 为什么不能用 gst-launch：GStreamer 的 pipewiresrc 确实会声明 SPA_META_Cursor，但它只把
 * 光标位置转成一条 ROI meta（名字叫 cursor），位图压根不看（`gstpipewiresrc.c` 里就三行）。
 * 要验证「位置 + 热点 + 位图」就得自己当消费者。
 *
 * 用法：
 *   cursorprobe <node-id> [--declare] [--buffers N] [--timeout SEC]
 * --declare：像 GStreamer 的 pipewiresrc 那样声明要 SPA_META_Cursor（给范围）。
 *   元数据是**双方协商**的，不声明的话缓冲里压根没有它（pw_buffers_negotiate 里
 *   是「消费者声明的参数在生产者那边过滤一遍」）。
 * 打印（每行一个缓冲）：
 *   meta=1 buffers=1 id=1 pos=X,Y hotspot=X,Y bitmap=1 fmt=... size=WxH stride=S hash=0x...
 *   meta=0   缓冲里压根没有这个元数据（生产者没协商它 —— hidden / embedded 就是这样）
 *   id=0     有元数据但当前没有可见光标
 * 读到第一个带位图的缓冲就退（exit 0）；超时 / 一直没有位图 exit 1。
 */
#define _GNU_SOURCE

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/raw.h>
#include <spa/param/param.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

struct state {
	struct pw_main_loop *loop;
	struct pw_stream *stream;
	unsigned long buffers;
	unsigned long max_buffers;
	bool saw_meta;
	bool saw_bitmap;
};

static void on_process(void *data) {
	struct state *st = data;
	struct pw_buffer *buffer = pw_stream_dequeue_buffer(st->stream);
	if (buffer == NULL) {
		return;
	}
	struct spa_buffer *buf = buffer->buffer;
	st->buffers++;

	struct spa_meta_cursor *cur = spa_buffer_find_meta_data(buf, SPA_META_Cursor,
		sizeof(*cur));
	if (cur == NULL) {
		if (!st->saw_meta) {
			printf("meta=0 buffers=%lu n_metas=%u", st->buffers, buf->n_metas);
			for (uint32_t i = 0; i < buf->n_metas; i++) {
				printf(" [type=%u size=%u]", buf->metas[i].type,
					buf->metas[i].size);
			}
			printf("\n");
			fflush(stdout);
		}
	} else {
		st->saw_meta = true;
		printf("meta=1 buffers=%lu id=%u pos=%d,%d hotspot=%d,%d",
			st->buffers, cur->id, cur->position.x, cur->position.y,
			cur->hotspot.x, cur->hotspot.y);
		if (cur->id != 0 && cur->bitmap_offset >= sizeof(struct spa_meta_cursor)) {
			struct spa_meta_bitmap *bm = SPA_PTROFF(cur, cur->bitmap_offset,
				struct spa_meta_bitmap);
			const uint8_t *px = SPA_PTROFF(bm, bm->offset, uint8_t);
			size_t n = (size_t)bm->stride * bm->size.height;
			uint32_t hash = 2166136261u; /* FNV-1a */
			for (size_t i = 0; i < n; i++) {
				hash = (hash ^ px[i]) * 16777619u;
			}
			printf(" bitmap=1 fmt=%u size=%ux%u stride=%d hash=0x%08x",
				bm->format, bm->size.width, bm->size.height, bm->stride, hash);
			st->saw_bitmap = true;
		} else {
			printf(" bitmap=0");
		}
		printf("\n");
		fflush(stdout);
	}

	pw_stream_queue_buffer(st->stream, buffer);
	if (st->saw_bitmap || st->buffers >= st->max_buffers) {
		pw_main_loop_quit(st->loop);
	}
}

static void on_timeout(void *data, uint64_t expirations) {
	(void)expirations;
	struct state *st = data;
	fprintf(stderr, "cursorprobe: 超时（看了 %lu 个缓冲，%s）\n", st->buffers,
		st->saw_meta ? "有元数据但没位图" : "连元数据都没有");
	pw_main_loop_quit(st->loop);
}

int main(int argc, char **argv) {
	struct state st = {0};
	st.max_buffers = 200;
	double timeout = 8.0;
	bool declare = false;
	const char *node = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--buffers") == 0 && i + 1 < argc) {
			st.max_buffers = strtoul(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
			timeout = atof(argv[++i]);
		} else if (strcmp(argv[i], "--declare") == 0) {
			declare = true;
		} else if (node == NULL) {
			node = argv[i];
		}
	}
	if (node == NULL) {
		fprintf(stderr, "用法：cursorprobe <node-id> [--declare] [--buffers N] [--timeout SEC]\n");
		return 2;
	}

	pw_init(NULL, NULL);
	st.loop = pw_main_loop_new(NULL);
	if (st.loop == NULL) {
		fprintf(stderr, "cursorprobe: 起不了 PipeWire 主循环\n");
		return 1;
	}
	struct pw_loop *loop = pw_main_loop_get_loop(st.loop);
	struct spa_source *timer = pw_loop_add_timer(loop, on_timeout, &st);
	struct timespec value = {
		.tv_sec = (time_t)timeout,
		.tv_nsec = (long)((timeout - (double)(time_t)timeout) * 1e9),
	};
	pw_loop_update_timer(loop, timer, &value, NULL, false);

	st.stream = pw_stream_new_simple(loop, "vidow-cursorprobe",
		pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Screen", NULL),
		&(struct pw_stream_events){
			PW_VERSION_STREAM_EVENTS,
			.process = on_process,
		}, &st);
	if (st.stream == NULL) {
		fprintf(stderr, "cursorprobe: 建不了流\n");
		return 1;
	}
	const struct spa_pod *params[1];
	int n_params = 0;
	uint8_t pod_buffer[256];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer,
		sizeof(pod_buffer));
	if (declare) {
		/* 和 GStreamer 的 pipewiresrc 一样：给个范围（默认 384×384，最小只要求
		 * 结构体本身）。生产者报的是固定值，双方取交集 —— 两边都给死值又不一样
		 * 的话这个元数据会被整个丢掉（pw_buffers_negotiate 的 param_filter）。 */
		int max = (int)(sizeof(struct spa_meta_cursor) +
			sizeof(struct spa_meta_bitmap) + 384 * 384 * 4);
		params[n_params++] = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
			SPA_PARAM_META_size,
				SPA_POD_CHOICE_RANGE_Int(max, sizeof(struct spa_meta_cursor),
					max));
	}
	pw_stream_connect(st.stream, PW_DIRECTION_INPUT, (uint32_t)strtoul(node, NULL, 0),
		PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, n_params);

	pw_main_loop_run(st.loop);

	printf("RESULT=%s buffers=%lu\n",
		st.saw_bitmap ? "bitmap" : (st.saw_meta ? "meta" : "none"), st.buffers);
	fflush(stdout);
	pw_stream_destroy(st.stream);
	pw_loop_destroy_source(loop, timer);
	pw_main_loop_destroy(st.loop);
	pw_deinit();
	return st.saw_bitmap ? 0 : 1;
}
