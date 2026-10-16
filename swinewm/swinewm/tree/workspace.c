#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/config.h"
#include "swinewm/input/seat.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/view.h"
#include "swinewm/tree/workspace.h"

enum swine_container_layout output_get_default_layout(struct swine_output *output) {
	if (config && config->default_orientation != L_NONE) {
		return config->default_orientation;
	}
	if (output && output->box.height > output->box.width) {
		return L_VERT;
	}
	return L_HORIZ;
}

struct swine_workspace *workspace_create(struct swine_output *output,
		const char *name) {
	if (!output) {
		// Like sway, new workspaces appear on the focused output.
		struct swine_workspace *focused = seat_get_focused_workspace();
		if (focused && focused->output) {
			output = focused->output;
		} else {
			output = root->outputs->length ?
				root->outputs->items[0] : root->fallback_output;
		}
	}
	struct swine_workspace *ws = calloc(1, sizeof(*ws));
	node_init(&ws->node, N_WORKSPACE, ws);
	ws->name = swine_strdup(name);
	ws->tiling = create_list();
	ws->floating = create_list();
	ws->layout = output_get_default_layout(output);
	ws->prev_split_layout = L_NONE;
	if (config) {
		ws->gaps_inner = config->gaps_inner;
		ws->gaps_outer = config->gaps_outer;
	}
	output_add_workspace(output, ws);
	seat_node_created(&ws->node);
	swine_log(SWINE_DEBUG, "Created workspace %s on %s", ws->name,
		output->name);
	return ws;
}

void workspace_destroy(struct swine_workspace *ws) {
	swine_log(SWINE_DEBUG, "Destroying workspace %s", ws->name);
	if (ws->output) {
		workspace_detach(ws);
	}
	seat_node_destroyed(&ws->node);
	list_free(ws->tiling);
	list_free(ws->floating);
	free(ws->name);
	free(ws);
}

void workspace_consider_destroy(struct swine_workspace *ws) {
	if (!ws || ws->tiling->length || ws->floating->length) {
		return;
	}
	if (ws->output && ws->output->active_workspace == ws) {
		return;
	}
	struct swine_node *focus = seat_get_focus_inactive(&root->node);
	if (focus == &ws->node) {
		return;
	}
	workspace_destroy(ws);
}

void workspace_detach(struct swine_workspace *ws) {
	struct swine_output *output = ws->output;
	if (!output) {
		return;
	}
	int index = list_find(output->workspaces, ws);
	if (index != -1) {
		list_del(output->workspaces, index);
	}
	if (output->active_workspace == ws) {
		output->active_workspace = NULL;
	}
	ws->output = NULL;
}

int workspace_number(const char *name) {
	if (!name || !isdigit((unsigned char)name[0])) {
		return -1;
	}
	char *end = NULL;
	long n = strtol(name, &end, 10);
	if (n < 0 || n > 1000000) {
		return -1;
	}
	return (int)n;
}

static bool test_name(struct swine_workspace *ws, void *data) {
	return swine_strcasecmp(ws->name, data) == 0;
}

struct swine_workspace *workspace_by_name(const char *name) {
	struct swine_workspace *current = seat_get_focused_workspace();
	if (swine_strcasecmp(name, "prev") == 0) {
		return workspace_prev(current);
	} else if (swine_strcasecmp(name, "next") == 0) {
		return workspace_next(current);
	} else if (swine_strcasecmp(name, "prev_on_output") == 0) {
		return workspace_output_prev(current);
	} else if (swine_strcasecmp(name, "next_on_output") == 0) {
		return workspace_output_next(current);
	} else if (swine_strcasecmp(name, "current") == 0) {
		return current;
	} else if (swine_strcasecmp(name, "back_and_forth") == 0) {
		return seat->prev_workspace_name ?
			root_find_workspace(test_name, seat->prev_workspace_name) : NULL;
	}
	return root_find_workspace(test_name, (void *)name);
}

static bool test_number(struct swine_workspace *ws, void *data) {
	return workspace_number(ws->name) == *(int *)data;
}

struct swine_workspace *workspace_by_number(const char *name) {
	int number = workspace_number(name);
	if (number < 0) {
		return NULL;
	}
	return root_find_workspace(test_number, &number);
}

char *workspace_next_name(void) {
	for (int n = 1;; ++n) {
		if (!root_find_workspace(test_number, &n)) {
			return format_str("%d", n);
		}
	}
}

struct ws_collect {
	list_t *list;
};

static void collect_ws(struct swine_workspace *ws, void *data) {
	list_add(((struct ws_collect *)data)->list, ws);
}

static struct swine_workspace *workspace_step(struct swine_workspace *current,
		bool same_output, int step) {
	if (!current) {
		return NULL;
	}
	list_t *list;
	if (same_output) {
		if (!current->output) {
			return NULL;
		}
		list = current->output->workspaces;
	} else {
		// Like sway, cycle through outputs in order, then workspaces.
		list = create_list();
		struct ws_collect c = {list};
		for (int i = 0; i < root->outputs->length; ++i) {
			struct swine_output *o = root->outputs->items[i];
			for (int j = 0; j < o->workspaces->length; ++j) {
				collect_ws(o->workspaces->items[j], &c);
			}
		}
	}
	struct swine_workspace *result = NULL;
	int index = list_find(list, current);
	if (index != -1 && list->length) {
		int next = (index + step + list->length) % list->length;
		result = list->items[next];
	}
	if (!same_output) {
		list_free(list);
	}
	return result;
}

struct swine_workspace *workspace_output_next(struct swine_workspace *current) {
	return workspace_step(current, true, 1);
}

struct swine_workspace *workspace_output_prev(struct swine_workspace *current) {
	return workspace_step(current, true, -1);
}

struct swine_workspace *workspace_next(struct swine_workspace *current) {
	return workspace_step(current, false, 1);
}

struct swine_workspace *workspace_prev(struct swine_workspace *current) {
	return workspace_step(current, false, -1);
}

bool workspace_switch(struct swine_workspace *ws) {
	struct swine_workspace *active = seat_get_focused_workspace();
	if (active && active != ws) {
		free(seat->prev_workspace_name);
		seat->prev_workspace_name = swine_strdup(active->name);
	}
	swine_log(SWINE_DEBUG, "Switching to workspace %s", ws->name);
	struct swine_node *next = seat_get_focus_inactive(&ws->node);
	seat_set_focus(next ? next : &ws->node);
	return true;
}

bool workspace_is_visible(struct swine_workspace *ws) {
	return ws->output && ws->output->enabled &&
		ws->output->active_workspace == ws;
}

static void find_urgent(struct swine_container *con, void *data) {
	if (con->view && con->view->urgent) {
		*(bool *)data = true;
	}
}

void workspace_detect_urgent(struct swine_workspace *ws) {
	bool urgent = false;
	workspace_for_each_container(ws, find_urgent, &urgent);
	ws->urgent = urgent;
}

bool workspace_is_empty(struct swine_workspace *ws) {
	if (ws->tiling->length) {
		return false;
	}
	// Sticky views are not considered to be part of this workspace
	for (int i = 0; i < ws->floating->length; ++i) {
		struct swine_container *floater = ws->floating->items[i];
		if (!container_is_sticky(floater)) {
			return false;
		}
	}
	return true;
}

static void set_workspace(struct swine_container *con, void *data) {
	con->workspace = con->parent ? con->parent->workspace : data;
}

static void workspace_attach_tiling(struct swine_workspace *ws,
		struct swine_container *con, int index) {
	list_insert(ws->tiling, index, con);
	con->workspace = ws;
	con->parent = NULL;
	container_for_each_child(con, set_workspace, ws);
}

struct swine_container *workspace_add_tiling(struct swine_workspace *ws,
		struct swine_container *con) {
	if (con->workspace) {
		struct swine_container *old_parent = con->parent;
		container_detach(con);
		if (old_parent) {
			container_reap_empty(old_parent);
		}
	}
	if (config && config->default_layout != L_NONE) {
		con = container_split(con, config->default_layout);
	}
	workspace_attach_tiling(ws, con, ws->tiling->length);
	return con;
}

void workspace_insert_tiling_direct(struct swine_workspace *ws,
		struct swine_container *con, int index) {
	if (con->workspace) {
		container_detach(con);
	}
	workspace_attach_tiling(ws, con, index);
}

struct swine_container *workspace_insert_tiling(struct swine_workspace *ws,
		struct swine_container *con, int index) {
	if (con->workspace) {
		container_detach(con);
	}
	if (config && config->default_layout != L_NONE) {
		con = container_split(con, config->default_layout);
	}
	workspace_attach_tiling(ws, con, index);
	return con;
}

void workspace_add_floating(struct swine_workspace *ws,
		struct swine_container *con) {
	if (con->workspace) {
		container_detach(con);
	}
	list_add(ws->floating, con);
	con->workspace = ws;
	con->parent = NULL;
	container_for_each_child(con, set_workspace, ws);
}

struct swine_container *workspace_wrap_children(struct swine_workspace *ws) {
	struct swine_container *fs = ws->fullscreen;
	struct swine_container *middle = container_create(NULL);
	middle->layout = ws->layout;
	while (ws->tiling->length) {
		struct swine_container *child = ws->tiling->items[0];
		container_detach(child);
		container_add_child(middle, child);
	}
	workspace_attach_tiling(ws, middle, ws->tiling->length);
	ws->fullscreen = fs;
	return middle;
}

void workspace_unwrap_children(struct swine_workspace *ws,
		struct swine_container *wrap) {
	if (!swine_assert(workspace_is_empty(ws), "Target workspace must be empty")) {
		return;
	}
	ws->layout = wrap->layout;
	while (wrap->children->length) {
		struct swine_container *child = wrap->children->items[0];
		container_detach(child);
		workspace_insert_tiling_direct(ws, child, ws->tiling->length);
	}
}

struct swine_container *workspace_split(struct swine_workspace *ws,
		enum swine_container_layout layout) {
	if (ws->tiling->length == 0) {
		ws->prev_split_layout = ws->layout;
		ws->layout = layout;
		return NULL;
	}
	enum swine_container_layout old_layout = ws->layout;
	struct swine_container *middle = workspace_wrap_children(ws);
	ws->layout = layout;
	middle->layout = old_layout;
	struct swine_container *focus = seat_get_focused_container();
	if (focus) {
		seat_set_raw_focus(&middle->node);
		seat_set_raw_focus(&focus->node);
	}
	return middle;
}

void workspace_squash(struct swine_workspace *ws) {
	for (int i = 0; i < ws->tiling->length; i++) {
		struct swine_container *child = ws->tiling->items[i];
		i += container_squash(child);
	}
}

void workspace_for_each_container(struct swine_workspace *ws,
		void (*f)(struct swine_container *con, void *data), void *data) {
	for (int i = 0; i < ws->tiling->length; ++i) {
		struct swine_container *con = ws->tiling->items[i];
		f(con, data);
		container_for_each_child(con, f, data);
	}
	for (int i = 0; i < ws->floating->length; ++i) {
		struct swine_container *con = ws->floating->items[i];
		f(con, data);
		container_for_each_child(con, f, data);
	}
}
