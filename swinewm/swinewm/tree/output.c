#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/config.h"
#include "swinewm/platform.h"
#include "swinewm/input/seat.h"
#include "swinewm/tree/container.h"
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

// Ported from sway/tree/output.c.
static void restore_workspaces(struct swine_output *output) {
	// Workspace output priority
	for (int i = 0; i < root->outputs->length; i++) {
		struct swine_output *other = root->outputs->items[i];
		if (other == output) {
			continue;
		}
		for (int j = 0; j < other->workspaces->length; j++) {
			struct swine_workspace *ws = other->workspaces->items[j];
			struct swine_output *highest =
				workspace_output_get_highest_available(ws);
			if (highest == output) {
				workspace_detach(ws);
				output_add_workspace(output, ws);
				j--;
			}
		}
		if (other->workspaces->length == 0) {
			char *next = workspace_next_name(other);
			struct swine_workspace *ws = workspace_create(other, next);
			free(next);
			other->active_workspace = ws;
		} else if (!other->active_workspace) {
			other->active_workspace = other->workspaces->items[0];
		}
	}

	// Saved workspaces
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
	output_sort_workspaces(output);
}

void output_enable(struct swine_output *output) {
	if (output->enabled) {
		return;
	}
	output->enabled = true;
	list_add(root->outputs, output);
	root_update_box();

	restore_workspaces(output);

	if (!output->workspaces->length) {
		char *name = workspace_next_name(output);
		struct swine_workspace *ws = workspace_create(output, name);
		free(name);
		output->active_workspace = ws;
		// The first output to appear takes focus.
		if (!seat_get_focus()) {
			seat_set_raw_focus(&ws->node);
		}
	} else if (!output->active_workspace) {
		// Show the workspace that was last focused on this output.
		struct swine_node *focus = seat_get_focus_inactive(&output->node);
		struct swine_workspace *ws = NULL;
		if (focus && focus->type == N_WORKSPACE) {
			ws = focus->workspace;
		} else if (focus && focus->type == N_CONTAINER) {
			ws = focus->container->workspace;
		}
		output->active_workspace = ws ? ws : output->workspaces->items[0];
	}
	swine_log(SWINE_INFO, "Output %s enabled (%dx%d at %d,%d)", output->name,
		output->box.width, output->box.height, output->box.x, output->box.y);
	// A monitor plugged in later gets its configuration now; at startup the
	// whole config is applied once it has been read.
	if (config && config->active) {
		output_apply_config(output);
	}
}

static void evacuate_sticky(struct swine_workspace *old_ws,
		struct swine_output *new_output) {
	struct swine_workspace *new_ws = output_get_active_workspace(new_output);
	if (!swine_assert(new_ws, "New output does not have a workspace")) {
		return;
	}
	while (old_ws->floating->length) {
		struct swine_container *sticky = old_ws->floating->items[0];
		container_detach(sticky);
		workspace_add_floating(new_ws, sticky);
		container_floating_move_to_center(sticky);
	}
	workspace_detect_urgent(new_ws);
}

static void output_evacuate(struct swine_output *output) {
	struct swine_output *fallback_output = root->outputs->length ?
		root->outputs->items[0] : NULL;
	while (output->workspaces->length) {
		struct swine_workspace *ws = output->workspaces->items[0];
		workspace_detach(ws);

		struct swine_output *new_output =
			workspace_output_get_highest_available(ws);
		if (!new_output) {
			new_output = fallback_output;
		}
		if (!new_output) {
			new_output = root->fallback_output;
		}
		struct swine_workspace *new_output_ws =
			output_get_active_workspace(new_output);

		if (workspace_is_empty(ws)) {
			// If the new output has an active workspace (the fallback output
			// may not have one), move all sticky containers to it
			if (new_output_ws && workspace_num_sticky_containers(ws) > 0) {
				evacuate_sticky(ws, new_output);
			}
			if (workspace_num_sticky_containers(ws) == 0 &&
					ws != seat_get_focused_workspace()) {
				workspace_destroy(ws);
				continue;
			}
		}

		workspace_output_add_priority(ws, new_output);
		output_add_workspace(new_output, ws);
		if (!new_output->active_workspace) {
			new_output->active_workspace = ws;
		}
	}
}

void output_destroy(struct swine_output *output) {
	int index = list_find(root->outputs, output);
	if (index != -1) {
		list_del(root->outputs, index);
	}
	root_update_box();
	output_evacuate(output);
	seat_node_destroyed(&output->node);
	list_free(output->workspaces);
	free(output->name);
	free(output->description);
	free(output->identifier);
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

struct swine_output *output_by_name_or_id(const char *name_or_id) {
	struct swine_output *output = output_by_name(name_or_id);
	for (int i = 0; i < root->outputs->length && !output; ++i) {
		struct swine_output *o = root->outputs->items[i];
		if (o->identifier && swine_strcasecmp(o->identifier, name_or_id) == 0) {
			output = o;
		}
	}
	return output;
}

struct swine_output *output_at_point(int x, int y) {
	for (int i = 0; i < root->outputs->length; ++i) {
		struct swine_output *output = root->outputs->items[i];
		const struct swine_box *b = &output->box;
		if (x >= b->x && x < b->x + b->width &&
				y >= b->y && y < b->y + b->height) {
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

struct swine_box output_get_usable_area(struct swine_output *output) {
	struct swine_box box = output->usable;
	box.x += output->reserved.left;
	box.y += output->reserved.top;
	box.width -= output->reserved.left + output->reserved.right;
	box.height -= output->reserved.top + output->reserved.bottom;
	return box;
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

static void merge_display_config(struct output_display_config *cfg,
		const char *name) {
	struct output_config *oc = name ? config_find_output_config(name) : NULL;
	if (!oc) {
		return;
	}
	if (oc->width > 0 && oc->height > 0) {
		cfg->width = oc->width;
		cfg->height = oc->height;
		cfg->refresh_rate = oc->refresh_rate;
		cfg->any = true;
	}
	if (oc->x != INT_MIN && oc->y != INT_MIN) {
		cfg->x = oc->x;
		cfg->y = oc->y;
		cfg->any = true;
	}
	if (oc->transform >= 0) {
		cfg->transform = oc->transform;
		cfg->any = true;
	}
}

// Like sway, "*" applies first, then the identifier, then the name.
static struct output_display_config display_config_for(
		struct swine_output *output) {
	struct output_display_config cfg = {
		.x = INT_MIN, .y = INT_MIN, .transform = -1,
	};
	merge_display_config(&cfg, "*");
	merge_display_config(&cfg, output->identifier);
	merge_display_config(&cfg, output->name);
	return cfg;
}

static struct swine_output *primary_output(void) {
	for (int i = 0; i < root->outputs->length; ++i) {
		struct swine_output *output = root->outputs->items[i];
		if (output->primary) {
			return output;
		}
	}
	return NULL;
}

static void apply_config(struct swine_output *output,
		struct swine_output *primary) {
	struct output_display_config cfg = display_config_for(output);
	// Windows keeps the primary monitor at 0,0: positions are relative to
	// where the config puts the primary monitor.
	if (cfg.x != INT_MIN && primary) {
		struct output_display_config pcfg = display_config_for(primary);
		if (pcfg.x != INT_MIN) {
			cfg.x -= pcfg.x;
			cfg.y -= pcfg.y;
		}
	}
	if (!platform_output_configure(output, &cfg)) {
		swine_log(SWINE_ERROR, "Failed to apply the configuration of "
			"output %s", output->name);
	}
}

void output_apply_config(struct swine_output *output) {
	if (config && output->enabled) {
		apply_config(output, primary_output());
	}
}

void output_apply_configs(void) {
	if (!config) {
		return;
	}
	struct swine_output *primary = primary_output();
	for (int i = 0; i < root->outputs->length; ++i) {
		apply_config(root->outputs->items[i], primary);
	}
}
