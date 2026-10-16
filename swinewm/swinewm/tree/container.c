#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/input/seat.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/view.h"
#include "swinewm/tree/workspace.h"

struct swine_container *container_create(struct swine_view *view) {
	struct swine_container *con = calloc(1, sizeof(*con));
	node_init(&con->node, N_CONTAINER, con);
	con->view = view;
	con->layout = L_NONE;
	con->prev_split_layout = L_NONE;
	con->marks = create_list();
	if (view) {
		view->container = con;
	} else {
		con->children = create_list();
	}
	seat_node_created(&con->node);
	return con;
}

void container_destroy(struct swine_container *con) {
	swine_assert(!con->children || con->children->length == 0,
		"Destroying a container that still has children");
	// Let the seat pick a new focus while the tree links are still intact.
	seat_node_destroyed(&con->node);
	if (con->workspace || con->parent) {
		container_detach(con);
	}
	if (con->scratchpad) {
		root_scratchpad_remove_container(con);
	}
	if (con->view && con->view->container == con) {
		con->view->container = NULL;
	}
	list_free(con->children);
	list_free_items_and_destroy(con->marks);
	free(con);
}

static void set_workspace(struct swine_container *con, void *data) {
	(void)data;
	con->workspace = con->parent ? con->parent->workspace : NULL;
}

void container_for_each_child(struct swine_container *con,
		void (*f)(struct swine_container *con, void *data), void *data) {
	if (!con->children) {
		return;
	}
	for (int i = 0; i < con->children->length; ++i) {
		struct swine_container *child = con->children->items[i];
		f(child, data);
		container_for_each_child(child, f, data);
	}
}

bool container_has_ancestor(struct swine_container *descendant,
		struct swine_container *ancestor) {
	while (descendant) {
		descendant = descendant->parent;
		if (descendant == ancestor) {
			return true;
		}
	}
	return false;
}

list_t *container_get_siblings(struct swine_container *con) {
	if (con->parent) {
		return con->parent->children;
	}
	if (!con->workspace) {
		return NULL;
	}
	if (list_find(con->workspace->tiling, con) != -1) {
		return con->workspace->tiling;
	}
	return con->workspace->floating;
}

int container_sibling_index(struct swine_container *child) {
	list_t *siblings = container_get_siblings(child);
	return siblings ? list_find(siblings, child) : -1;
}

enum swine_container_layout container_parent_layout(struct swine_container *con) {
	if (con->parent) {
		return con->parent->layout;
	}
	if (con->workspace) {
		return con->workspace->layout;
	}
	return L_NONE;
}

bool container_is_floating(struct swine_container *con) {
	return !con->parent && con->workspace &&
		list_find(con->workspace->floating, con) != -1;
}

struct swine_container *container_toplevel_ancestor(struct swine_container *con) {
	while (con->parent) {
		con = con->parent;
	}
	return con;
}

bool container_is_floating_or_child(struct swine_container *con) {
	return container_is_floating(container_toplevel_ancestor(con));
}

bool container_is_scratchpad_hidden(struct swine_container *con) {
	return con->scratchpad && !con->workspace;
}

bool container_is_sticky(struct swine_container *con) {
	return con->is_sticky && container_is_floating(con);
}

bool container_is_sticky_or_child(struct swine_container *con) {
	return container_is_sticky(container_toplevel_ancestor(con));
}

void container_detach(struct swine_container *child) {
	struct swine_workspace *old_ws = child->workspace;
	if (old_ws && old_ws->fullscreen &&
			(old_ws->fullscreen == child ||
			container_has_ancestor(old_ws->fullscreen, child))) {
		old_ws->fullscreen = NULL;
	}
	list_t *siblings = container_get_siblings(child);
	if (siblings) {
		int index = list_find(siblings, child);
		if (index != -1) {
			list_del(siblings, index);
		}
	}
	child->parent = NULL;
	child->workspace = NULL;
	container_for_each_child(child, set_workspace, NULL);
}

static void attach_common(struct swine_container *parent,
		struct swine_container *child) {
	child->parent = parent;
	child->workspace = parent ? parent->workspace : child->workspace;
	container_for_each_child(child, set_workspace, NULL);
	if (child->fullscreen_mode == FULLSCREEN_WORKSPACE && child->workspace) {
		if (child->workspace->fullscreen && child->workspace->fullscreen != child) {
			child->fullscreen_mode = FULLSCREEN_NONE;
		} else {
			child->workspace->fullscreen = child;
		}
	}
}

void container_insert_child(struct swine_container *parent,
		struct swine_container *child, int i) {
	if (child->workspace || child->parent) {
		container_detach(child);
	}
	list_insert(parent->children, i, child);
	attach_common(parent, child);
}

void container_add_child(struct swine_container *parent,
		struct swine_container *child) {
	container_insert_child(parent, child, parent->children->length);
}

void container_add_sibling(struct swine_container *fixed,
		struct swine_container *active, bool after) {
	if (active->workspace || active->parent) {
		container_detach(active);
	}
	list_t *siblings = container_get_siblings(fixed);
	int index = list_find(siblings, fixed);
	list_insert(siblings, index + (after ? 1 : 0), active);
	active->workspace = fixed->workspace;
	attach_common(fixed->parent, active);
}

void container_replace(struct swine_container *container,
		struct swine_container *replacement) {
	enum swine_fullscreen_mode fullscreen = container->fullscreen_mode;
	if (fullscreen != FULLSCREEN_NONE) {
		container_set_fullscreen(container, FULLSCREEN_NONE);
	}
	if (container->parent || container->workspace) {
		double width_fraction = container->width_fraction;
		double height_fraction = container->height_fraction;
		container_add_sibling(container, replacement, true);
		container_detach(container);
		replacement->width_fraction = width_fraction;
		replacement->height_fraction = height_fraction;
	}
	if (fullscreen != FULLSCREEN_NONE) {
		container_set_fullscreen(replacement, fullscreen);
	}
}

struct swine_container *container_split(struct swine_container *child,
		enum swine_container_layout layout) {
	// i3 doesn't split singleton H/V containers
	if (child->parent || child->workspace) {
		list_t *siblings = container_get_siblings(child);
		if (siblings->length == 1) {
			enum swine_container_layout current = container_parent_layout(child);
			if (container_is_floating(child)) {
				current = L_NONE;
			}
			if (current == L_HORIZ || current == L_VERT) {
				if (child->parent) {
					child->parent->layout = layout;
				} else {
					child->workspace->layout = layout;
				}
				return child;
			}
		}
	}

	bool set_focus = seat_get_focus() == &child->node;

	struct swine_container *cont = container_create(NULL);
	cont->box = child->box;
	cont->width_fraction = child->width_fraction;
	cont->height_fraction = child->height_fraction;
	cont->layout = layout;

	if (child->parent || child->workspace) {
		container_replace(child, cont);
	}
	container_add_child(cont, child);

	if (set_focus) {
		seat_set_raw_focus(&cont->node);
		seat_set_raw_focus(&child->node);
	}
	return cont;
}

void container_reap_empty(struct swine_container *con) {
	if (con->view) {
		return;
	}
	struct swine_workspace *ws = con->workspace;
	while (con) {
		if (con->children->length) {
			return;
		}
		struct swine_container *parent = con->parent;
		container_destroy(con);
		con = parent;
	}
	if (ws) {
		workspace_consider_destroy(ws);
	}
}

struct swine_container *container_flatten(struct swine_container *container) {
	if (container->view) {
		return NULL;
	}
	while (container && container->children->length == 1) {
		struct swine_container *child = container->children->items[0];
		struct swine_container *parent = container->parent;
		container_replace(container, child);
		container_destroy(container);
		container = parent;
	}
	return container;
}

bool layout_is_parallel(enum swine_container_layout layout,
		enum swine_container_layout other) {
	bool layout_is_horiz = layout == L_HORIZ || layout == L_TABBED;
	bool other_is_horiz = other == L_HORIZ || other == L_TABBED;
	return layout_is_horiz == other_is_horiz;
}

static bool container_is_squashable(struct swine_container *con,
		struct swine_container *child) {
	enum swine_container_layout gp_layout = container_parent_layout(con);
	return (con->layout == L_HORIZ || con->layout == L_VERT) &&
		(child->layout == L_HORIZ || child->layout == L_VERT) &&
		!layout_is_parallel(con->layout, child->layout) &&
		layout_is_parallel(gp_layout, child->layout);
}

static void container_squash_children(struct swine_container *con) {
	for (int i = 0; i < con->children->length; i++) {
		struct swine_container *child = con->children->items[i];
		i += container_squash(child);
	}
}

int container_squash(struct swine_container *con) {
	if (!con->children) {
		return 0;
	}
	if (con->children->length != 1) {
		container_squash_children(con);
		return 0;
	}
	struct swine_container *child = con->children->items[0];
	int idx = container_sibling_index(con);
	int change = 0;
	if (child->children && container_is_squashable(con, child)) {
		// con and child are a redundant H/V pair. Destroy them.
		while (child->children->length) {
			struct swine_container *current = child->children->items[0];
			container_detach(current);
			if (con->parent) {
				container_insert_child(con->parent, current, idx);
			} else {
				workspace_insert_tiling_direct(con->workspace, current, idx);
			}
			change++;
		}
		// This will also destroy con because child was its only child
		container_reap_empty(child);
		change--;
	} else {
		container_squash_children(con);
	}
	return change;
}

static struct swine_box workspace_output_area(struct swine_workspace *ws) {
	if (ws && ws->output && ws->output->enabled) {
		return output_get_usable_area(ws->output);
	}
	if (root->outputs->length) {
		struct swine_output *o = root->outputs->items[0];
		return output_get_usable_area(o);
	}
	return (struct swine_box){0, 0, 1280, 720};
}

void container_floating_move_to_center(struct swine_container *con) {
	struct swine_box area = workspace_output_area(con->workspace);
	con->floating_box.x = area.x + (area.width - con->floating_box.width) / 2;
	con->floating_box.y = area.y + (area.height - con->floating_box.height) / 2;
}

static void container_init_floating_box(struct swine_container *con) {
	struct swine_box area = workspace_output_area(con->workspace);
	int width = con->box.width, height = con->box.height;
	if (con->view && con->view->natural.width > 0) {
		width = con->view->natural.width;
		height = con->view->natural.height;
	}
	if (width <= 0 || height <= 0) {
		width = area.width / 2;
		height = area.height / 2;
	}
	if (width > area.width) {
		width = area.width;
	}
	if (height > area.height) {
		height = area.height;
	}
	con->floating_box.width = width;
	con->floating_box.height = height;
	container_floating_move_to_center(con);
}

void container_set_floating(struct swine_container *con, bool enable) {
	if (container_is_floating(con) == enable) {
		return;
	}
	struct swine_workspace *ws = con->workspace;
	if (!ws) {
		return;
	}
	if (enable) {
		struct swine_container *old_parent = con->parent;
		container_detach(con);
		workspace_add_floating(ws, con);
		container_init_floating_box(con);
		if (old_parent) {
			container_reap_empty(old_parent);
		}
	} else {
		struct swine_container *reference = seat_get_focus_inactive_tiling(ws);
		container_detach(con);
		if (reference) {
			if (reference->view) {
				container_add_sibling(reference, con, true);
			} else {
				container_add_child(reference, con);
			}
		} else {
			workspace_add_tiling(ws, con);
		}
		con->width_fraction = con->height_fraction = 0;
	}
}

void container_set_fullscreen(struct swine_container *con,
		enum swine_fullscreen_mode mode) {
	struct swine_workspace *ws = con->workspace;
	if (mode == FULLSCREEN_GLOBAL) {
		// Spanning every monitor is not supported yet; treat it like the
		// per-output fullscreen that sway calls "workspace" fullscreen.
		mode = FULLSCREEN_WORKSPACE;
	}
	if (mode == FULLSCREEN_NONE) {
		if (ws && ws->fullscreen == con) {
			ws->fullscreen = NULL;
		}
		con->fullscreen_mode = FULLSCREEN_NONE;
		return;
	}
	if (ws) {
		if (ws->fullscreen && ws->fullscreen != con) {
			ws->fullscreen->fullscreen_mode = FULLSCREEN_NONE;
		}
		ws->fullscreen = con;
	}
	con->fullscreen_mode = mode;
}

static bool find_by_mark_iterator(struct swine_container *con, void *data) {
	return container_has_mark(con, data);
}

struct swine_container *container_find_mark(const char *mark) {
	return root_find_container(find_by_mark_iterator, (void *)mark);
}

bool container_find_and_unmark(const char *mark) {
	struct swine_container *con = container_find_mark(mark);
	if (!con) {
		return false;
	}
	for (int i = 0; i < con->marks->length; ++i) {
		char *con_mark = con->marks->items[i];
		if (strcmp(con_mark, mark) == 0) {
			free(con_mark);
			list_del(con->marks, i);
			return true;
		}
	}
	return false;
}

void container_clear_marks(struct swine_container *con) {
	for (int i = 0; i < con->marks->length; ++i) {
		free(con->marks->items[i]);
	}
	con->marks->length = 0;
}

bool container_has_mark(struct swine_container *con, const char *mark) {
	for (int i = 0; i < con->marks->length; ++i) {
		if (strcmp(con->marks->items[i], mark) == 0) {
			return true;
		}
	}
	return false;
}

void container_add_mark(struct swine_container *con, const char *mark) {
	list_add(con->marks, swine_strdup(mark));
}

// Ported from sway/tree/container.c.
static void swap_places(struct swine_container *con1,
		struct swine_container *con2) {
	struct swine_box box1 = con1->box, floating1 = con1->floating_box;
	double width_fraction1 = con1->width_fraction;
	double height_fraction1 = con1->height_fraction;
	struct swine_container *parent1 = con1->parent;
	struct swine_workspace *ws1 = con1->workspace;
	bool floating1_now = container_is_floating(con1);

	con1->box = con2->box;
	con1->floating_box = con2->floating_box;
	con1->width_fraction = con2->width_fraction;
	con1->height_fraction = con2->height_fraction;
	con2->box = box1;
	con2->floating_box = floating1;
	con2->width_fraction = width_fraction1;
	con2->height_fraction = height_fraction1;

	int index1 = container_sibling_index(con1);
	int index2 = container_sibling_index(con2);
	struct swine_container *parent2 = con2->parent;
	struct swine_workspace *ws2 = con2->workspace;
	bool floating2_now = container_is_floating(con2);
	container_detach(con1);
	container_detach(con2);
	// With both detached, insert the one with the lower index first in a
	// shared list so the other index stays valid.
	bool same_list = parent1 == parent2 && ws1 == ws2 &&
		floating1_now == floating2_now;
	struct swine_container *first = con1, *second = con2;
	struct swine_container *first_parent = parent2, *second_parent = parent1;
	struct swine_workspace *first_ws = ws2, *second_ws = ws1;
	bool first_floating = floating2_now, second_floating = floating1_now;
	int first_index = index2, second_index = index1;
	if (same_list && index1 < index2) {
		first = con2;
		second = con1;
		first_parent = parent1;
		second_parent = parent2;
		first_ws = ws1;
		second_ws = ws2;
		first_floating = floating1_now;
		second_floating = floating2_now;
		first_index = index1;
		second_index = index2;
	}
	struct swine_container *cons[2] = {first, second};
	struct swine_container *parents[2] = {first_parent, second_parent};
	struct swine_workspace *workspaces[2] = {first_ws, second_ws};
	bool floating[2] = {first_floating, second_floating};
	int indexes[2] = {first_index, second_index};
	for (int i = 0; i < 2; ++i) {
		if (parents[i]) {
			container_insert_child(parents[i], cons[i], indexes[i]);
		} else if (floating[i]) {
			workspace_add_floating(workspaces[i], cons[i]);
		} else {
			workspace_insert_tiling_direct(workspaces[i], cons[i], indexes[i]);
		}
	}
}

static void swap_focus(struct swine_container *con1,
		struct swine_container *con2, struct swine_container *focus) {
	if (focus == con1 || focus == con2) {
		struct swine_workspace *ws1 = con1->workspace;
		struct swine_workspace *ws2 = con2->workspace;
		enum swine_container_layout layout1 = container_parent_layout(con1);
		enum swine_container_layout layout2 = container_parent_layout(con2);
		if (focus == con1 && (layout2 == L_TABBED || layout2 == L_STACKED)) {
			if (workspace_is_visible(ws2)) {
				seat_set_focus(&con2->node);
			}
			seat_set_focus_container(ws1 != ws2 ? con2 : con1);
		} else if (focus == con2 && (layout1 == L_TABBED
					|| layout1 == L_STACKED)) {
			if (workspace_is_visible(ws1)) {
				seat_set_focus(&con1->node);
			}
			seat_set_focus_container(ws1 != ws2 ? con1 : con2);
		} else if (ws1 != ws2) {
			seat_set_focus_container(focus == con1 ? con2 : con1);
		} else {
			seat_set_focus_container(focus);
		}
	} else if (focus) {
		seat_set_focus_container(focus);
	}
}

void container_swap(struct swine_container *con1,
		struct swine_container *con2) {
	if (!swine_assert(con1 && con2, "Cannot swap with nothing")) {
		return;
	}
	if (!swine_assert(!container_has_ancestor(con1, con2)
				&& !container_has_ancestor(con2, con1),
				"Cannot swap ancestor and descendant")) {
		return;
	}
	swine_log(SWINE_DEBUG, "Swapping containers %zu and %zu",
		con1->node.id, con2->node.id);

	bool scratch1 = con1->scratchpad;
	bool hidden1 = container_is_scratchpad_hidden(con1);
	bool scratch2 = con2->scratchpad;
	bool hidden2 = container_is_scratchpad_hidden(con2);
	if (scratch1) {
		if (hidden1) {
			root_scratchpad_show(con1);
		}
		root_scratchpad_remove_container(con1);
	}
	if (scratch2) {
		if (hidden2) {
			root_scratchpad_show(con2);
		}
		root_scratchpad_remove_container(con2);
	}

	enum swine_fullscreen_mode fs1 = con1->fullscreen_mode;
	if (fs1) {
		container_set_fullscreen(con1, FULLSCREEN_NONE);
	}
	enum swine_fullscreen_mode fs2 = con2->fullscreen_mode;
	if (fs2) {
		container_set_fullscreen(con2, FULLSCREEN_NONE);
	}

	struct swine_container *focus = seat_get_focused_container();
	struct swine_workspace *vis1 =
		output_get_active_workspace(con1->workspace->output);
	struct swine_workspace *vis2 =
		output_get_active_workspace(con2->workspace->output);
	if (!swine_assert(vis1 && vis2, "con1 or con2 are on an output without "
				"a workspace. This should not happen")) {
		return;
	}

	char *stored_prev_name = NULL;
	if (seat->prev_workspace_name) {
		stored_prev_name = swine_strdup(seat->prev_workspace_name);
	}

	swap_places(con1, con2);

	if (!workspace_is_visible(vis1)) {
		seat_set_focus(seat_get_focus_inactive(&vis1->node));
	}
	if (!workspace_is_visible(vis2)) {
		seat_set_focus(seat_get_focus_inactive(&vis2->node));
	}

	swap_focus(con1, con2, focus);

	if (stored_prev_name) {
		free(seat->prev_workspace_name);
		seat->prev_workspace_name = stored_prev_name;
	}

	if (scratch1) {
		root_scratchpad_add_container(con2);
		if (!hidden1) {
			root_scratchpad_show(con2);
		}
	}
	if (scratch2) {
		root_scratchpad_add_container(con1);
		if (!hidden2) {
			root_scratchpad_show(con1);
		}
	}

	if (fs1) {
		container_set_fullscreen(con2, fs1);
	}
	if (fs2) {
		container_set_fullscreen(con1, fs2);
	}
}

const char *container_layout_to_str(enum swine_container_layout layout) {
	switch (layout) {
	case L_NONE: return "none";
	case L_HORIZ: return "splith";
	case L_VERT: return "splitv";
	case L_STACKED: return "stacked";
	case L_TABBED: return "tabbed";
	}
	return "none";
}
