#ifndef _SWINE_ROOT_H
#define _SWINE_ROOT_H

#include "list.h"
#include "swinewm/tree/node.h"

struct swine_root {
	struct swine_node node;
	list_t *outputs;          // struct swine_output *, enabled outputs only
	// Holds workspaces while no output is connected (sway: root->fallback_output)
	struct swine_output *fallback_output;
	list_t *scratchpad;       // struct swine_container *
	struct swine_box box;     // bounding box of all outputs
};

extern struct swine_root *root;

struct swine_root *root_create(void);
void root_destroy(struct swine_root *root);
void root_update_box(void);

void root_scratchpad_add_container(struct swine_container *con);
void root_scratchpad_remove_container(struct swine_container *con);
void root_scratchpad_show(struct swine_container *con);
void root_scratchpad_hide(struct swine_container *con);

struct swine_workspace *root_find_workspace(
		bool (*test)(struct swine_workspace *ws, void *data), void *data);
void root_for_each_workspace(
		void (*f)(struct swine_workspace *ws, void *data), void *data);
void root_for_each_container(
		void (*f)(struct swine_container *con, void *data), void *data);

#endif
