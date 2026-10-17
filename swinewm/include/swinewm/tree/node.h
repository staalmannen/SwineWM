#ifndef _SWINE_NODE_H
#define _SWINE_NODE_H

#include <stdbool.h>
#include <stddef.h>
#include "list.h"

struct swine_root;
struct swine_output;
struct swine_workspace;
struct swine_container;

// Plain integer rectangle in virtual-desktop pixels (Windows physical pixels
// with per-monitor DPI awareness). Equivalent of struct wlr_box in sway.
struct swine_box {
	int x, y, width, height;
};

// Edges of a container (sway uses enum wlr_edges); may be combined.
enum swine_edges {
	SWINE_EDGE_NONE = 0,
	SWINE_EDGE_TOP = 1 << 0,
	SWINE_EDGE_BOTTOM = 1 << 1,
	SWINE_EDGE_LEFT = 1 << 2,
	SWINE_EDGE_RIGHT = 1 << 3,
};

struct side_gaps {
	int top, right, bottom, left;
};

enum swine_node_type {
	N_ROOT,
	N_OUTPUT,
	N_WORKSPACE,
	N_CONTAINER,
};

// Every tree object embeds one of these, as in sway. The seat's focus stack
// stores node pointers so any level of the tree can hold focus.
struct swine_node {
	enum swine_node_type type;
	size_t id;
	union {
		struct swine_root *root;
		struct swine_output *output;
		struct swine_workspace *workspace;
		struct swine_container *container;
	};
};

void node_init(struct swine_node *node, enum swine_node_type type, void *thing);
const char *node_type_to_str(enum swine_node_type type);
struct swine_node *node_get_parent(struct swine_node *node);
list_t *node_get_children(struct swine_node *node);
bool node_has_ancestor(struct swine_node *node, struct swine_node *ancestor);
struct swine_output *node_get_output(struct swine_node *node);
bool box_equal(const struct swine_box *a, const struct swine_box *b);
bool box_contains_point(const struct swine_box *box, int x, int y);

#endif
