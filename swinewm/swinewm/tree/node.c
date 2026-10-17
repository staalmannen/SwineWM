#include <stdlib.h>
#include "swinewm/tree/container.h"
#include "swinewm/tree/node.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/workspace.h"

static size_t next_id = 1;

void node_init(struct swine_node *node, enum swine_node_type type, void *thing) {
	node->type = type;
	node->id = next_id++;
	node->root = thing; // all union members are pointers
}

const char *node_type_to_str(enum swine_node_type type) {
	switch (type) {
	case N_ROOT: return "root";
	case N_OUTPUT: return "output";
	case N_WORKSPACE: return "workspace";
	case N_CONTAINER: return "container";
	}
	return "";
}

struct swine_node *node_get_parent(struct swine_node *node) {
	switch (node->type) {
	case N_CONTAINER: {
		struct swine_container *con = node->container;
		if (con->parent) {
			return &con->parent->node;
		}
		if (con->workspace) {
			return &con->workspace->node;
		}
		return NULL;
	}
	case N_WORKSPACE: {
		struct swine_workspace *ws = node->workspace;
		return ws->output ? &ws->output->node : NULL;
	}
	case N_OUTPUT:
		return &root->node;
	case N_ROOT:
		return NULL;
	}
	return NULL;
}

list_t *node_get_children(struct swine_node *node) {
	switch (node->type) {
	case N_CONTAINER:
		return node->container->children;
	case N_WORKSPACE:
		return node->workspace->tiling;
	case N_OUTPUT:
		return node->output->workspaces;
	case N_ROOT:
		return root->outputs;
	}
	return NULL;
}

bool node_has_ancestor(struct swine_node *node, struct swine_node *ancestor) {
	struct swine_node *parent = node_get_parent(node);
	while (parent) {
		if (parent == ancestor) {
			return true;
		}
		parent = node_get_parent(parent);
	}
	return false;
}

struct swine_output *node_get_output(struct swine_node *node) {
	switch (node->type) {
	case N_CONTAINER: {
		struct swine_workspace *ws = node->container->workspace;
		return ws ? ws->output : NULL;
	}
	case N_WORKSPACE:
		return node->workspace->output;
	case N_OUTPUT:
		return node->output;
	case N_ROOT:
		return NULL;
	}
	return NULL;
}

bool box_equal(const struct swine_box *a, const struct swine_box *b) {
	return a->x == b->x && a->y == b->y &&
		a->width == b->width && a->height == b->height;
}

bool box_contains_point(const struct swine_box *box, int x, int y) {
	return x >= box->x && x < box->x + box->width &&
		y >= box->y && y < box->y + box->height;
}
