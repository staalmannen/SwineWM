// The tray items shown by the bar's "tray" module. See bar/tray.h.
#include <stdlib.h>
#include <string.h>
#include "stringop.h"
#include "swinewm/bar/tray.h"

static list_t *items = NULL;

list_t *tray_get_items(void) {
	if (!items) {
		items = create_list();
	}
	return items;
}

static struct tray_item *find_item(uint64_t id, int *index) {
	list_t *list = tray_get_items();
	for (int i = 0; i < list->length; ++i) {
		struct tray_item *item = list->items[i];
		if (item->id == id) {
			if (index) {
				*index = i;
			}
			return item;
		}
	}
	return NULL;
}

static void free_item(struct tray_item *item) {
	free(item->app_id);
	free(item->title);
	free(item);
}

static void set_string(char **field, const char *value) {
	if (*field && value && strcmp(*field, value) == 0) {
		return;
	}
	free(*field);
	*field = value ? swine_strdup(value) : NULL;
}

void tray_item_set(uint64_t id, const char *app_id, const char *title,
		bool passive) {
	struct tray_item *item = find_item(id, NULL);
	if (!item) {
		item = calloc(1, sizeof(*item));
		item->id = id;
		list_add(tray_get_items(), item);
	}
	set_string(&item->app_id, app_id);
	set_string(&item->title, title);
	item->passive = passive;
}

bool tray_item_remove(uint64_t id) {
	int index = 0;
	struct tray_item *item = find_item(id, &index);
	if (!item) {
		return false;
	}
	list_del(items, index);
	free_item(item);
	return true;
}

void tray_clear(void) {
	if (!items) {
		return;
	}
	for (int i = 0; i < items->length; ++i) {
		free_item(items->items[i]);
	}
	list_free(items);
	items = NULL;
}
