#include <stdlib.h>
#include "log.h"
#include "swinewm/input/seat.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/view.h"
#include "swinewm/tree/workspace.h"

struct swine_seat *seat = NULL;

struct swine_seat *seat_create(void) {
	struct swine_seat *s = calloc(1, sizeof(*s));
	s->focus_stack = create_list();
	seat = s;
	return s;
}

void seat_destroy(struct swine_seat *s) {
	list_free(s->focus_stack);
	free(s->prev_workspace_name);
	free(s);
	if (seat == s) {
		seat = NULL;
	}
}

void seat_node_created(struct swine_node *node) {
	if (seat && list_find(seat->focus_stack, node) == -1) {
		list_add(seat->focus_stack, node);
	}
}

static void stack_raise(struct swine_node *node) {
	int index = list_find(seat->focus_stack, node);
	if (index != -1) {
		list_del(seat->focus_stack, index);
	}
	list_insert(seat->focus_stack, 0, node);
}

void seat_set_raw_focus(struct swine_node *node) {
	if (!seat || !node) {
		return;
	}
	// Raise ancestors first so that the node itself ends up on top and each
	// ancestor sits directly below its most recently focused descendant.
	struct swine_node *chain[256];
	int n = 0;
	for (struct swine_node *it = node; it && n < 256; it = node_get_parent(it)) {
		if (it->type == N_ROOT || it->type == N_OUTPUT) {
			break;
		}
		chain[n++] = it;
	}
	for (int i = n - 1; i >= 0; --i) {
		stack_raise(chain[i]);
	}
}

void seat_set_focus(struct swine_node *node) {
	if (!node) {
		return;
	}
	struct swine_workspace *ws = NULL;
	if (node->type == N_WORKSPACE) {
		ws = node->workspace;
	} else if (node->type == N_CONTAINER) {
		ws = node->container->workspace;
	}
	struct swine_workspace *old_ws = seat_get_focused_workspace();
	if (ws && ws->output) {
		ws->output->active_workspace = ws;
	}
	seat_set_raw_focus(node);
	if (node->type == N_CONTAINER && node->container->view &&
			node->container->view->urgent) {
		view_set_urgent(node->container->view, false);
	}
	// Move sticky containers to the new workspace
	if (ws && old_ws && ws != old_ws) {
		for (int i = 0; i < old_ws->floating->length; ++i) {
			struct swine_container *floater = old_ws->floating->items[i];
			if (container_is_sticky(floater)) {
				container_detach(floater);
				workspace_add_floating(ws, floater);
				--i;
			}
		}
	}
	if (old_ws && old_ws != ws) {
		workspace_consider_destroy(old_ws);
	}
}

void seat_set_focus_container(struct swine_container *con) {
	seat_set_focus(con ? &con->node : NULL);
}

void seat_set_focus_workspace(struct swine_workspace *ws) {
	seat_set_focus(ws ? &ws->node : NULL);
}

struct swine_node *seat_get_focus(void) {
	if (!seat || !seat->focus_stack->length) {
		return NULL;
	}
	return seat->focus_stack->items[0];
}

struct swine_container *seat_get_focused_container(void) {
	struct swine_node *focus = seat_get_focus();
	return focus && focus->type == N_CONTAINER ? focus->container : NULL;
}

struct swine_workspace *seat_get_focused_workspace(void) {
	struct swine_node *focus = seat_get_focus();
	if (!focus) {
		return NULL;
	}
	if (focus->type == N_WORKSPACE) {
		return focus->workspace;
	}
	if (focus->type == N_CONTAINER) {
		return focus->container->workspace;
	}
	return NULL;
}

struct swine_node *seat_get_focus_inactive(struct swine_node *ancestor) {
	if (!seat) {
		return NULL;
	}
	if (ancestor->type == N_CONTAINER && ancestor->container->view) {
		return ancestor;
	}
	for (int i = 0; i < seat->focus_stack->length; ++i) {
		struct swine_node *node = seat->focus_stack->items[i];
		if (node_has_ancestor(node, ancestor)) {
			// Hidden scratchpad windows are not focusable.
			if (node->type == N_CONTAINER && !node->container->workspace) {
				continue;
			}
			return node;
		}
	}
	// Like sway: an empty workspace is its own focus.
	if (ancestor->type == N_WORKSPACE) {
		return ancestor;
	}
	return NULL;
}

struct swine_container *seat_get_focus_inactive_view(struct swine_node *ancestor) {
	for (int i = 0; i < seat->focus_stack->length; ++i) {
		struct swine_node *node = seat->focus_stack->items[i];
		if (node->type == N_CONTAINER && node->container->view &&
				node->container->workspace &&
				(node == ancestor || node_has_ancestor(node, ancestor))) {
			return node->container;
		}
	}
	return ancestor->type == N_CONTAINER ? ancestor->container : NULL;
}

struct swine_container *seat_get_focus_inactive_tiling(struct swine_workspace *ws) {
	if (!ws->tiling->length) {
		return NULL;
	}
	for (int i = 0; i < seat->focus_stack->length; ++i) {
		struct swine_node *node = seat->focus_stack->items[i];
		if (node->type != N_CONTAINER) {
			continue;
		}
		struct swine_container *con = node->container;
		if (con->workspace == ws && !container_is_floating_or_child(con)) {
			return con;
		}
	}
	return NULL;
}

struct swine_container *seat_get_focus_inactive_floating(struct swine_workspace *ws) {
	for (int i = 0; i < seat->focus_stack->length; ++i) {
		struct swine_node *node = seat->focus_stack->items[i];
		if (node->type != N_CONTAINER) {
			continue;
		}
		struct swine_container *con = node->container;
		if (con->workspace == ws && container_is_floating_or_child(con)) {
			return container_toplevel_ancestor(con);
		}
	}
	return NULL;
}

struct swine_node *seat_get_active_tiling_child(struct swine_node *parent) {
	list_t *children = node_get_children(parent);
	if (!children || !children->length) {
		return NULL;
	}
	for (int i = 0; i < seat->focus_stack->length; ++i) {
		struct swine_node *node = seat->focus_stack->items[i];
		if (node->type == N_CONTAINER &&
				list_find(children, node->container) != -1) {
			return node;
		}
	}
	return NULL;
}

static bool is_self_or_descendant(struct swine_node *node,
		struct swine_node *of) {
	return node == of || node_has_ancestor(node, of);
}

void seat_node_destroyed(struct swine_node *node) {
	if (!seat) {
		return;
	}
	bool was_focused = seat_get_focus() == node;
	int index = list_find(seat->focus_stack, node);
	if (index != -1) {
		list_del(seat->focus_stack, index);
	}
	if (!was_focused) {
		return;
	}
	// Focus the most recently focused node near the destroyed one: walk up
	// the tree until an ancestor has another focusable descendant.
	struct swine_node *parent = node_get_parent(node);
	while (parent && parent->type == N_CONTAINER) {
		for (int i = 0; i < seat->focus_stack->length; ++i) {
			struct swine_node *cand = seat->focus_stack->items[i];
			if (cand != parent && node_has_ancestor(cand, parent) &&
					!is_self_or_descendant(cand, node) &&
					cand->type == N_CONTAINER && cand->container->workspace) {
				seat_set_raw_focus(cand);
				return;
			}
		}
		parent = node_get_parent(parent);
	}
	if (parent && parent->type == N_WORKSPACE) {
		struct swine_node *next = NULL;
		for (int i = 0; i < seat->focus_stack->length; ++i) {
			struct swine_node *cand = seat->focus_stack->items[i];
			if (node_has_ancestor(cand, parent) && cand->type == N_CONTAINER &&
					cand->container->workspace &&
					!is_self_or_descendant(cand, node)) {
				next = cand;
				break;
			}
		}
		seat_set_raw_focus(next ? next : parent);
		return;
	}
	// A workspace or output went away: fall back to any visible workspace.
	for (int i = 0; i < seat->focus_stack->length; ++i) {
		struct swine_node *cand = seat->focus_stack->items[i];
		if (cand->type == N_WORKSPACE && workspace_is_visible(cand->workspace)) {
			seat_set_raw_focus(cand);
			return;
		}
	}
}
