#include "hotplug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void vidow_output_key(const char *name, char *buf, size_t len) {
	if (len == 0) {
		return;
	}
	if (name == NULL || name[0] == '\0') {
		buf[0] = '\0';
		return;
	}
	snprintf(buf, len, "%s", name);
}

struct vidow_output_memory *vidow_output_memory_for(struct wl_list *memories,
		const char *name) {
	char key[VIDOW_OUTPUT_KEY_MAX];
	vidow_output_key(name, key, sizeof(key));
	if (key[0] == '\0') {
		return NULL; /* 没名字的屏记不住，也就无所谓还原 */
	}

	struct vidow_output_memory *memory;
	wl_list_for_each(memory, memories, link) {
		if (strcmp(memory->key, key) == 0) {
			return memory;
		}
	}

	memory = calloc(1, sizeof(*memory));
	if (memory == NULL) {
		return NULL;
	}
	memcpy(memory->key, key, sizeof(key));
	wl_list_insert(memories->prev, &memory->link);
	return memory;
}

size_t vidow_hotplug_pick_fallback(const struct vidow_hotplug_slot *slots,
		size_t n, int dead_x) {
	size_t left = SIZE_MAX, right = SIZE_MAX;
	for (size_t i = 0; i < n; i++) {
		if (!slots[i].live) {
			continue;
		}
		if (slots[i].x <= dead_x) {
			/* 左边（含同一位置）：取最靠右的那块，离原来的位置最近 */
			if (left == SIZE_MAX || slots[i].x > slots[left].x) {
				left = i;
			}
		} else {
			/* 右边：取最靠左的那块 */
			if (right == SIZE_MAX || slots[i].x < slots[right].x) {
				right = i;
			}
		}
	}
	if (left != SIZE_MAX) {
		return left;
	}
	if (right != SIZE_MAX) {
		return right;
	}
	return SIZE_MAX;
}

void vidow_hotplug_regroup(const bool *flagged, size_t n, size_t *order) {
	size_t k = 0;
	for (size_t i = 0; i < n; i++) {
		if (!flagged[i]) {
			order[k++] = i;
		}
	}
	for (size_t i = 0; i < n; i++) {
		if (flagged[i]) {
			order[k++] = i;
		}
	}
}
