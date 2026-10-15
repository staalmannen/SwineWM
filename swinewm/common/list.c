#include <stdlib.h>
#include <string.h>
#include "list.h"

list_t *create_list(void) {
	list_t *list = calloc(1, sizeof(list_t));
	if (!list) {
		return NULL;
	}
	list->capacity = 10;
	list->items = malloc(sizeof(void *) * list->capacity);
	if (!list->items) {
		free(list);
		return NULL;
	}
	return list;
}

static void list_resize(list_t *list) {
	if (list->length == list->capacity) {
		int capacity = list->capacity * 2;
		void **items = realloc(list->items, sizeof(void *) * capacity);
		if (!items) {
			abort();
		}
		list->capacity = capacity;
		list->items = items;
	}
}

void list_free(list_t *list) {
	if (!list) {
		return;
	}
	free(list->items);
	free(list);
}

void list_add(list_t *list, void *item) {
	list_resize(list);
	list->items[list->length++] = item;
}

void list_insert(list_t *list, int index, void *item) {
	if (index < 0) {
		index = 0;
	}
	if (index > list->length) {
		index = list->length;
	}
	list_resize(list);
	memmove(&list->items[index + 1], &list->items[index],
		sizeof(void *) * (list->length - index));
	list->length++;
	list->items[index] = item;
}

void list_del(list_t *list, int index) {
	if (index < 0 || index >= list->length) {
		return;
	}
	list->length--;
	memmove(&list->items[index], &list->items[index + 1],
		sizeof(void *) * (list->length - index));
}

void list_cat(list_t *list, const list_t *source) {
	for (int i = 0; i < source->length; ++i) {
		list_add(list, source->items[i]);
	}
}

int list_find(const list_t *list, const void *item) {
	for (int i = 0; i < list->length; ++i) {
		if (list->items[i] == item) {
			return i;
		}
	}
	return -1;
}

int list_seq_find(const list_t *list,
		int compare(const void *item, const void *cmp_to), const void *cmp_to) {
	for (int i = 0; i < list->length; ++i) {
		if (compare(list->items[i], cmp_to) == 0) {
			return i;
		}
	}
	return -1;
}

void list_swap(list_t *list, int src, int dest) {
	void *tmp = list->items[src];
	list->items[src] = list->items[dest];
	list->items[dest] = tmp;
}

void list_move_to_end(list_t *list, void *item) {
	int i = list_find(list, item);
	if (i == -1) {
		return;
	}
	list_del(list, i);
	list_add(list, item);
}

void list_free_items_and_destroy(list_t *list) {
	if (!list) {
		return;
	}
	for (int i = 0; i < list->length; ++i) {
		free(list->items[i]);
	}
	list_free(list);
}
