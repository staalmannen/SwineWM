#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/input/seat.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/workspace.h"

struct swine_output *output_create(const char *name, void *handle) {
	struct swine_output *output = calloc(1, sizeof(*output));
	node_init(&output->node, N_OUTPUT, output);
	output->name = swine_strdup(name);
	output->handle = handle;
	output->scale = 1.0;
	output->workspaces = create_list();
	return output;
}

void output_enable(struct swine_output *output) {
	if (output->enabled) {
		return;
	}
	output->enabled = true;
	list_add(root->outputs, output);
	root_update_box();

	// Adopt workspaces that were parked on the fallback output.
	struct swine_output *fallback = root->fallback_output;
	struct swine_workspace *fallback_active = fallback->active_workspace;
	while (fallback->workspaces->length) {
		struct swine_workspace *ws = fallback->workspaces->items[0];
		workspace_detach(ws);
		output_add_workspace(output, ws);
	}
	fallback->active_workspace = NULL;
	if (fallback_active) {
		output->active_workspace = fallback_active;
	}

	if (!output->workspaces->length) {
		char *name = workspace_next_name();
		struct swine_workspace *ws = workspace_create(output, name);
		free(name);
		output->active_workspace = ws;
		// The first output to appear takes focus.
		if (!seat_get_focus()) {
			seat_set_raw_focus(&ws->node);
		}
	} else if (!output->active_workspace) {
		output->active_workspace = output->workspaces->items[0];
	}
	swine_log(SWINE_INFO, "Output %s enabled (%dx%d at %d,%d)", output->name,
		output->box.width, output->box.height, output->box.x, output->box.y);
}

void output_destroy(struct swine_output *output) {
	int index = list_find(root->outputs, output);
	if (index != -1) {
		list_del(root->outputs, index);
	}
	root_update_box();

	struct swine_output *dest = root->outputs->length ?
		root->outputs->items[0] : root->fallback_output;
	while (output->workspaces->length) {
		struct swine_workspace *ws = output->workspaces->items[0];
		workspace_detach(ws);
		if (workspace_is_empty(ws) && ws != seat_get_focused_workspace()) {
			workspace_destroy(ws);
			continue;
		}
		output_add_workspace(dest, ws);
	}
	if (!dest->active_workspace && dest->workspaces->length) {
		dest->active_workspace = dest->workspaces->items[0];
	}
	seat_node_destroyed(&output->node);
	list_free(output->workspaces);
	free(output->name);
	free(output->description);
	free(output);
}

struct swine_output *output_by_name(const char *name) {
	for (int i = 0; i < root->outputs->length; ++i) {
		struct swine_output *output = root->outputs->items[i];
		if (swine_strcasecmp(output->name, name) == 0) {
			return output;
		}
	}
	return NULL;
}

struct swine_output *output_by_handle(void *handle) {
	for (int i = 0; i < root->outputs->length; ++i) {
		struct swine_output *output = root->outputs->items[i];
		if (output->handle == handle) {
			return output;
		}
	}
	return NULL;
}

struct swine_workspace *output_get_active_workspace(struct swine_output *output) {
	return output ? output->active_workspace : NULL;
}

static int sort_workspace_cmp(const void *a, const void *b) {
	const struct swine_workspace *wa = *(void *const *)a;
	const struct swine_workspace *wb = *(void *const *)b;
	int na = workspace_number(wa->name);
	int nb = workspace_number(wb->name);
	if (na >= 0 && nb >= 0) {
		return na - nb;
	}
	if (na >= 0) {
		return -1;
	}
	if (nb >= 0) {
		return 1;
	}
	return strcmp(wa->name, wb->name);
}

void output_sort_workspaces(struct swine_output *output) {
	qsort(output->workspaces->items, (size_t)output->workspaces->length,
		sizeof(void *), sort_workspace_cmp);
}

void output_add_workspace(struct swine_output *output,
		struct swine_workspace *ws) {
	if (ws->output) {
		workspace_detach(ws);
	}
	list_add(output->workspaces, ws);
	ws->output = output;
	output_sort_workspaces(output);
}

struct swine_output *output_get_in_direction(struct swine_output *output,
		enum swine_direction dir) {
	if (!output) {
		return NULL;
	}
	// Pick the closest output whose centre lies in the requested direction
	// and which overlaps on the other axis, preferring overlap.
	int cx = output->box.x + output->box.width / 2;
	int cy = output->box.y + output->box.height / 2;
	struct swine_output *best = NULL;
	long long best_dist = 0;
	for (int i = 0; i < root->outputs->length; ++i) {
		struct swine_output *o = root->outputs->items[i];
		if (o == output) {
			continue;
		}
		int ox = o->box.x + o->box.width / 2;
		int oy = o->box.y + o->box.height / 2;
		bool ok = false;
		long long dist = 0;
		switch (dir) {
		case DIR_LEFT:
			ok = o->box.x + o->box.width <= output->box.x + 1 &&
				o->box.y < output->box.y + output->box.height &&
				o->box.y + o->box.height > output->box.y;
			dist = cx - ox;
			break;
		case DIR_RIGHT:
			ok = o->box.x >= output->box.x + output->box.width - 1 &&
				o->box.y < output->box.y + output->box.height &&
				o->box.y + o->box.height > output->box.y;
			dist = ox - cx;
			break;
		case DIR_UP:
			ok = o->box.y + o->box.height <= output->box.y + 1 &&
				o->box.x < output->box.x + output->box.width &&
				o->box.x + o->box.width > output->box.x;
			dist = cy - oy;
			break;
		case DIR_DOWN:
			ok = o->box.y >= output->box.y + output->box.height - 1 &&
				o->box.x < output->box.x + output->box.width &&
				o->box.x + o->box.width > output->box.x;
			dist = oy - cy;
			break;
		}
		if (ok && (!best || dist < best_dist)) {
			best = o;
			best_dist = dist;
		}
	}
	return best;
}
