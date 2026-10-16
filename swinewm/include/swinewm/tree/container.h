#ifndef _SWINE_CONTAINER_H
#define _SWINE_CONTAINER_H

#include <stdbool.h>
#include "list.h"
#include "swinewm/tree/node.h"

struct swine_view;
struct swine_workspace;

enum swine_container_layout {
	L_NONE,
	L_HORIZ,
	L_VERT,
	L_STACKED,
	L_TABBED,
};

enum swine_fullscreen_mode {
	FULLSCREEN_NONE,
	FULLSCREEN_WORKSPACE,
	FULLSCREEN_GLOBAL,
};

// A container is either a leaf holding a view (a native window) or a split
// container holding children. Mirrors struct sway_container without the
// pending/current transaction split: geometry here is the desired layout and
// the view keeps what was last applied to the real window.
struct swine_container {
	struct swine_node node;
	struct swine_view *view;          // NULL for split containers
	struct swine_container *parent;   // NULL at workspace level
	struct swine_workspace *workspace;
	list_t *children;                 // NULL for view containers
	enum swine_container_layout layout;
	enum swine_container_layout prev_split_layout;
	enum swine_fullscreen_mode fullscreen_mode;
	double width_fraction, height_fraction;
	struct swine_box box;             // arranged geometry
	struct swine_box floating_box;    // geometry while floating
	bool scratchpad;
	list_t *marks;                    // char *, as sway's con->marks
};

struct swine_container *container_create(struct swine_view *view);
void container_destroy(struct swine_container *con);

void container_detach(struct swine_container *child);
void container_add_child(struct swine_container *parent,
		struct swine_container *child);
void container_insert_child(struct swine_container *parent,
		struct swine_container *child, int i);
void container_add_sibling(struct swine_container *fixed,
		struct swine_container *active, bool after);
void container_replace(struct swine_container *container,
		struct swine_container *replacement);
struct swine_container *container_split(struct swine_container *child,
		enum swine_container_layout layout);
void container_reap_empty(struct swine_container *con);
struct swine_container *container_flatten(struct swine_container *container);
int container_squash(struct swine_container *con);

list_t *container_get_siblings(struct swine_container *container);
int container_sibling_index(struct swine_container *child);
enum swine_container_layout container_parent_layout(struct swine_container *con);
bool container_has_ancestor(struct swine_container *descendant,
		struct swine_container *ancestor);
bool container_is_floating(struct swine_container *con);
bool container_is_floating_or_child(struct swine_container *con);
bool container_is_scratchpad_hidden(struct swine_container *con);
struct swine_container *container_toplevel_ancestor(struct swine_container *con);
void container_for_each_child(struct swine_container *con,
		void (*f)(struct swine_container *con, void *data), void *data);

void container_set_floating(struct swine_container *con, bool enable);
void container_floating_move_to_center(struct swine_container *con);
void container_set_fullscreen(struct swine_container *con,
		enum swine_fullscreen_mode mode);
const char *container_layout_to_str(enum swine_container_layout layout);
bool layout_is_parallel(enum swine_container_layout layout,
		enum swine_container_layout other);

#endif
