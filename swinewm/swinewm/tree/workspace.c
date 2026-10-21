#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ipc.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/config.h"
#include "swinewm/input/seat.h"
#include "swinewm/ipc-json.h"
#include "swinewm/platform.h"
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

struct workspace_config *workspace_find_config(const char *ws_name) {
	if (!config) {
		return NULL;
	}
	for (int i = 0; i < config->workspace_configs->length; ++i) {
		struct workspace_config *wsc = config->workspace_configs->items[i];
		if (strcmp(wsc->workspace, ws_name) == 0) {
			return wsc;
		}
	}
	return NULL;
}

struct swine_output *workspace_get_initial_output(const char *name) {
	// Check workspace configs for a workspace<->output pair
	struct workspace_config *wsc = workspace_find_config(name);
	if (wsc) {
		for (int i = 0; i < wsc->outputs->length; i++) {
			struct swine_output *output =
				output_by_name_or_id(wsc->outputs->items[i]);
			if (output) {
				return output;
			}
		}
	}
	// Otherwise try to put it on the focused output
	struct swine_node *focus = seat_get_focus_inactive(&root->node);
	if (focus && focus->type == N_WORKSPACE && focus->workspace->output) {
		return focus->workspace->output;
	} else if (focus && focus->type == N_CONTAINER &&
			focus->container->workspace &&
			focus->container->workspace->output) {
		return focus->container->workspace->output;
	}
	// Fallback to the first output or the headless output
	return root->outputs->length ?
		root->outputs->items[0] : root->fallback_output;
}

struct swine_workspace *workspace_create(struct swine_output *output,
		const char *name) {
	if (!output) {
		output = workspace_get_initial_output(name);
	}
	struct swine_workspace *ws = calloc(1, sizeof(*ws));
	node_init(&ws->node, N_WORKSPACE, ws);
	ws->name = swine_strdup(name);
	ws->tiling = create_list();
	ws->floating = create_list();
	ws->output_priority = create_list();
	ws->layout = output_get_default_layout(output);
	ws->prev_split_layout = L_NONE;
	if (config) {
		ws->gaps_inner = config->gaps_inner;
		ws->gaps_outer = config->gaps_outer;
	}
	struct workspace_config *wsc = workspace_find_config(name);
	if (wsc) {
		if (wsc->gaps_outer.top != INT_MIN) {
			ws->gaps_outer.top = wsc->gaps_outer.top;
		}
		if (wsc->gaps_outer.right != INT_MIN) {
			ws->gaps_outer.right = wsc->gaps_outer.right;
		}
		if (wsc->gaps_outer.bottom != INT_MIN) {
			ws->gaps_outer.bottom = wsc->gaps_outer.bottom;
		}
		if (wsc->gaps_outer.left != INT_MIN) {
			ws->gaps_outer.left = wsc->gaps_outer.left;
		}
		if (wsc->gaps_inner != INT_MIN) {
			ws->gaps_inner = wsc->gaps_inner;
		}
		// Add output priorities
		for (int i = 0; i < wsc->outputs->length; ++i) {
			char *output_name = wsc->outputs->items[i];
			if (strcmp(output_name, "*") != 0) {
				list_add(ws->output_priority, swine_strdup(output_name));
			}
		}
	}
	// If not already added, add the output to the lowest priority
	workspace_output_add_priority(ws, output);

	output_add_workspace(output, ws);
	seat_node_created(&ws->node);
	char *event = ipc_json_event_workspace("init", ws, NULL);
	platform_ipc_event((uint32_t)IPC_EVENT_WORKSPACE, event);
	free(event);
	swine_log(SWINE_DEBUG, "Created workspace %s on %s", ws->name,
		output->name);
	return ws;
}

void workspace_destroy(struct swine_workspace *ws) {
	swine_log(SWINE_DEBUG, "Destroying workspace %s", ws->name);
	char *event = ipc_json_event_workspace("empty", ws, NULL);
	platform_ipc_event((uint32_t)IPC_EVENT_WORKSPACE, event);
	free(event);
	if (ws->output) {
		workspace_detach(ws);
	}
	seat_node_destroyed(&ws->node);
	list_free(ws->tiling);
	list_free(ws->floating);
	list_free_items_and_destroy(ws->output_priority);
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

static bool workspace_valid_on_output(struct swine_output *output,
		const char *ws_name) {
	struct workspace_config *wsc = workspace_find_config(ws_name);
	if (!wsc) {
		return true;
	}
	for (int i = 0; i < wsc->outputs->length; i++) {
		struct swine_output *ws_output =
			output_by_name_or_id(wsc->outputs->items[i]);
		if (ws_output) {
			return ws_output == output;
		}
	}
	return false;
}

static char *workspace_target_from_binding_command(char *command) {
	char *cmd = argsep(&command, " ", NULL);
	if (!cmd || !command) {
		return NULL;
	}
	if (strcmp(cmd, "workspace") == 0) {
		return argsep(&command, ",;", NULL);
	}
	if (strcmp(cmd, "move") != 0) {
		return NULL;
	}

	char *arg = argsep(&command, " ", NULL);
	if (!arg) {
		return NULL;
	}
	if (strcmp(arg, "container") == 0 || strcmp(arg, "window") == 0) {
		arg = argsep(&command, " ", NULL);
		if (!arg || strcmp(arg, "to") != 0) {
			return NULL;
		}
	} else if (strcmp(arg, "to") != 0) {
		return NULL;
	}
	arg = argsep(&command, " ", NULL);
	if (!arg || strcmp(arg, "workspace") != 0) {
		return NULL;
	}
	return argsep(&command, ",;", NULL);
}

// Ported from sway/tree/workspace.c.
static void workspace_name_from_binding(const struct swine_binding *binding,
		struct swine_output *output, int *min_order, char **earliest_name) {
	char *cmdlist = swine_strdup(binding->command);
	char *dup = cmdlist;
	char *name = workspace_target_from_binding_command(cmdlist);

	if (name && *name) {
		char *target = swine_strdup(name);
		target = do_var_replacement(target);
		strip_quotes(target);

		// Make sure that the command references an actual workspace
		// not a command about workspaces
		if (strcmp(target, "next") == 0 ||
				strcmp(target, "prev") == 0 ||
				strcmp(target, "next_on_output") == 0 ||
				strcmp(target, "prev_on_output") == 0 ||
				strcmp(target, "number") == 0 ||
				strcmp(target, "back_and_forth") == 0 ||
				strcmp(target, "current") == 0) {
			free(target);
			free(dup);
			return;
		}

		// If the command is workspace number <name>, isolate the name
		if (has_prefix(target, "number ")) {
			char *temp = swine_strdup(target + strlen("number "));
			free(target);
			target = temp;
			// Make sure the workspace number doesn't already exist
			if (isdigit((unsigned char)target[0]) &&
					workspace_by_number(target)) {
				free(target);
				free(dup);
				return;
			}
		}

		// Make sure that the workspace doesn't already exist, and that it
		// can appear on the given output
		if (workspace_by_name(target) ||
				!workspace_valid_on_output(output, target)) {
			free(target);
			free(dup);
			return;
		}

		if (binding->order < *min_order) {
			*min_order = binding->order;
			free(*earliest_name);
			*earliest_name = target;
		} else {
			free(target);
		}
	}
	free(dup);
}

char *workspace_next_name(struct swine_output *output) {
	// Scan for available workspace names by looking through output-workspace
	// assignments primarily, falling back to bindings and numbers.
	int order = INT_MAX;
	char *target = NULL;
	if (config && output) {
		struct swine_mode *mode = config->current_mode;
		for (int i = 0; i < mode->keysym_bindings->length; ++i) {
			workspace_name_from_binding(mode->keysym_bindings->items[i],
				output, &order, &target);
		}
		for (int i = 0; i < config->workspace_configs->length; ++i) {
			// Unlike with bindings, this does not guarantee order
			const struct workspace_config *wsc =
				config->workspace_configs->items[i];
			if (workspace_by_name(wsc->workspace)) {
				continue;
			}
			bool found = false;
			for (int j = 0; j < wsc->outputs->length; ++j) {
				struct swine_output *ws_output =
					output_by_name_or_id(wsc->outputs->items[j]);
				if (ws_output) {
					if (ws_output == output) {
						found = true;
						free(target);
						target = swine_strdup(wsc->workspace);
					}
					break;
				}
			}
			if (found) {
				break;
			}
		}
	}
	if (target != NULL) {
		return target;
	}
	// As a fall back, use the next available number
	for (int n = 1;; ++n) {
		if (!root_find_workspace(test_number, &n)) {
			return format_str("%d", n);
		}
	}
}

static int find_output(const void *id1, const void *id2) {
	return swine_strcasecmp(id1, id2);
}

static int workspace_output_get_priority(struct swine_workspace *ws,
		struct swine_output *output) {
	int index_id = output->identifier ? list_seq_find(ws->output_priority,
		find_output, output->identifier) : -1;
	int index_name = list_seq_find(ws->output_priority, find_output,
		output->name);
	return index_name < 0 || (index_id >= 0 && index_id < index_name) ?
		index_id : index_name;
}

// Outputs are remembered by name: Windows' "DISPLAY<n>" stays with the
// connector, the identifier (monitor model) is shared by identical monitors.
void workspace_output_raise_priority(struct swine_workspace *ws,
		struct swine_output *old_output, struct swine_output *output) {
	int old_index = workspace_output_get_priority(ws, old_output);
	if (old_index < 0) {
		return;
	}
	int new_index = workspace_output_get_priority(ws, output);
	if (new_index < 0) {
		list_insert(ws->output_priority, old_index, swine_strdup(output->name));
	} else if (new_index > old_index) {
		char *name = ws->output_priority->items[new_index];
		list_del(ws->output_priority, new_index);
		list_insert(ws->output_priority, old_index, name);
	}
}

void workspace_output_add_priority(struct swine_workspace *ws,
		struct swine_output *output) {
	if (output == root->fallback_output) {
		return;
	}
	if (workspace_output_get_priority(ws, output) < 0) {
		list_add(ws->output_priority, swine_strdup(output->name));
	}
}

struct swine_output *workspace_output_get_highest_available(
		struct swine_workspace *ws) {
	for (int i = 0; i < ws->output_priority->length; i++) {
		struct swine_output *output =
			output_by_name_or_id(ws->output_priority->items[i]);
		if (output) {
			return output;
		}
	}
	return NULL;
}

void workspace_move_to_output(struct swine_workspace *ws,
		struct swine_output *output) {
	if (ws->output == output) {
		return;
	}
	struct swine_output *old_output = ws->output;
	workspace_detach(ws);
	struct swine_workspace *new_output_old_ws =
		output_get_active_workspace(output);
	if (!swine_assert(new_output_old_ws, "Expected output to have a workspace")) {
		return;
	}

	output_add_workspace(output, ws);
	// sway derives an output's active workspace from the focus order.
	struct swine_node *focus = seat_get_focus_inactive(&output->node);
	if (focus) {
		struct swine_workspace *active = focus->type == N_WORKSPACE ?
			focus->workspace : focus->container->workspace;
		if (active) {
			output->active_workspace = active;
		}
	}

	// If moving the last workspace from the old output, create a new workspace
	// on the old output
	if (old_output->workspaces->length == 0) {
		char *ws_name = workspace_next_name(old_output);
		struct swine_workspace *new_ws = workspace_create(old_output, ws_name);
		free(ws_name);
		old_output->active_workspace = new_ws;
		seat_set_raw_focus(&new_ws->node);
	} else if (old_output->active_workspace == NULL) {
		old_output->active_workspace = old_output->workspaces->items[0];
	}

	workspace_consider_destroy(new_output_old_ws);

	output_sort_workspaces(output);
	workspace_output_raise_priority(ws, old_output, output);
	char *event = ipc_json_event_workspace("move", ws, NULL);
	platform_ipc_event((uint32_t)IPC_EVENT_WORKSPACE, event);
	free(event);
}

static void count_tiling_views(struct swine_container *con, void *data) {
	if (con->view && !container_is_floating_or_child(con)) {
		size_t *count = data;
		*count += 1;
	}
}

size_t workspace_num_tiling_views(struct swine_workspace *ws) {
	size_t count = 0;
	workspace_for_each_container(ws, count_tiling_views, &count);
	return count;
}

static void count_sticky_containers(struct swine_container *con, void *data) {
	if (container_is_sticky(con)) {
		size_t *count = data;
		*count += 1;
	}
}

size_t workspace_num_sticky_containers(struct swine_workspace *ws) {
	size_t count = 0;
	workspace_for_each_container(ws, count_sticky_containers, &count);
	return count;
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
	bool was_urgent = ws->urgent;
	bool urgent = false;
	workspace_for_each_container(ws, find_urgent, &urgent);
	ws->urgent = urgent;
	if (was_urgent != urgent) {
		char *event = ipc_json_event_workspace("urgent", ws, NULL);
		platform_ipc_event((uint32_t)IPC_EVENT_WORKSPACE, event);
		free(event);
	}
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

bool workspace_has_single_visible_container(struct swine_workspace *ws) {
	struct swine_container *focus = seat_get_focus_inactive_tiling(ws);
	if (focus && !focus->view) {
		focus = seat_get_focus_inactive_view(&focus->node);
	}
	return focus && focus->view && view_ancestor_is_only_visible(focus->view);
}
