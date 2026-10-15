#ifndef _SWINE_LIST_H
#define _SWINE_LIST_H

// Growable pointer array, modelled on sway's common/list.c so that code
// ported or cross-read from sway keeps the same shape.
typedef struct {
	int capacity;
	int length;
	void **items;
} list_t;

list_t *create_list(void);
void list_free(list_t *list);
void list_add(list_t *list, void *item);
void list_insert(list_t *list, int index, void *item);
void list_del(list_t *list, int index);
void list_cat(list_t *list, const list_t *source);
// Returns the index of item (pointer equality), or -1.
int list_find(const list_t *list, const void *item);
// Returns the index of the first item for which compare(item, cmp_to) == 0.
int list_seq_find(const list_t *list,
		int compare(const void *item, const void *cmp_to), const void *cmp_to);
void list_swap(list_t *list, int src, int dest);
void list_move_to_end(list_t *list, void *item);
// Calls free() on every item, then frees the list.
void list_free_items_and_destroy(list_t *list);

#endif
