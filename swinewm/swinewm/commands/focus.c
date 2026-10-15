#include <limits.h>
#include <stdlib.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/commands_util.h"
#include "swinewm/config.h"
#include "swinewm/input/seat.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/workspace.h"

bool parse_direction(const char *name, enum swine_direction *out) {
	if (swine_strcasecmp(name, "left") == 0) {
		*out = DIR_LEFT;
	} else if (swine_strcasecmp(name, "right") == 0) {
		*out = DIR_RIGHT;
	} else if (swine_strcasecmp(name, "up") == 0) {
		*out = DIR_UP;
	} else if (swine_strcasecmp(name, "down") == 0) {
		*out = DIR_DOWN;
	} else {
		return false;
	}
	return true;
}

bool layout_parallel_to_direction(enum swine_container_layout layout,
		enum swine_direction dir) {
	switch (layout) {
	case L_TABBED:
	case L_HORIZ:
		return dir == DIR_LEFT || dir == DIR_RIGHT;
	case L_STACKED:
	case L_VERT:
		return dir == DIR_UP || dir == DIR_DOWN;
	default:
		return false;
	}
}

struct swine_output *output_by_name_or_direction(const char *name,
		struct swine_output *from) {
	enum swine_direction dir;
	if (parse_direction(name, &dir)) {
		return output_get_in_direction(from, dir);
	}
	return output_by_name(name);
}

static struct swine_node *get_node_in_output_direction(
		struct swine_output *output, enum swine_direction dir) {
	struct swine_workspace *ws = output_get_active_workspace(output);
	if (!swine_assert(ws, "Expected output to have a workspace")) {
		return NULL;
	}
	if (ws->fullscreen) {
		return seat_get_focus_inactive(&ws->fullscreen->node);
	}
	struct swine_container *container = NULL;
	if (ws->tiling->length > 0) {
		switch (dir) {
		case DIR_LEFT:
			if (ws->layout == L_HORIZ || ws->layout == L_TABBED) {
				// get most right child of new output
				container = ws->tiling->items[ws->tiling->length - 1];
			} else {
				container = seat_get_focus_inactive_tiling(ws);
			}
			break;
		case DIR_RIGHT:
			if (ws->layout == L_HORIZ || ws->layout == L_TABBED) {
				// get most left child of new output
				container = ws->tiling->items[0];
			} else {
				container = seat_get_focus_inactive_tiling(ws);
			}
			break;
		case DIR_UP:
			if (ws->layout == L_VERT || ws->layout == L_STACKED) {
				container = ws->tiling->items[ws->tiling->length - 1];
			} else {
				container = seat_get_focus_inactive_tiling(ws);
			}
			break;
		case DIR_DOWN:
			if (ws->layout == L_VERT || ws->layout == L_STACKED) {
				container = ws->tiling->items[0];
			} else {
				container = seat_get_focus_inactive_tiling(ws);
			}
			break;
		}
	}
	if (container) {
		container = seat_get_focus_inactive_view(&container->node);
		return &container->node;
	}
	return seat_get_focus_inactive(&ws->node);
}

static struct swine_node *node_get_in_direction_tiling(
		struct swine_container *container, enum swine_direction dir,
		bool descend) {
	struct swine_container *wrap_candidate = NULL;
	struct swine_container *current = container;
	while (current) {
		if (current->fullscreen_mode != FULLSCREEN_NONE) {
			// Fullscreen container with a direction - go straight to outputs
			struct swine_output *output = current->workspace->output;
			struct swine_output *new_output = output_get_in_direction(output, dir);
			if (!new_output) {
				return NULL;
			}
			return get_node_in_output_direction(new_output, dir);
		}

		bool can_move = false;
		int desired = 0;
		int idx = container_sibling_index(current);
		enum swine_container_layout parent_layout = container_parent_layout(current);
		list_t *siblings = container_get_siblings(current);

		if (dir == DIR_LEFT || dir == DIR_RIGHT) {
			if (parent_layout == L_HORIZ || parent_layout == L_TABBED) {
				can_move = true;
				desired = idx + (dir == DIR_LEFT ? -1 : 1);
			}
		} else {
			if (parent_layout == L_VERT || parent_layout == L_STACKED) {
				can_move = true;
				desired = idx + (dir == DIR_UP ? -1 : 1);
			}
		}

		if (can_move) {
			if (desired < 0 || desired >= siblings->length) {
				int len = siblings->length;
				if (config->focus_wrapping != WRAP_NO && !wrap_candidate
						&& len > 1) {
					if (desired < 0) {
						wrap_candidate = siblings->items[len - 1];
					} else {
						wrap_candidate = siblings->items[0];
					}
					if (config->focus_wrapping == WRAP_FORCE) {
						struct swine_container *c =
							seat_get_focus_inactive_view(&wrap_candidate->node);
						return &c->node;
					}
				}
			} else {
				struct swine_container *desired_con = siblings->items[desired];
				if (!descend) {
					return &desired_con->node;
				}
				struct swine_container *c =
					seat_get_focus_inactive_view(&desired_con->node);
				return &c->node;
			}
		}
		current = current->parent;
	}

	// Check a different output
	struct swine_output *output = container->workspace->output;
	struct swine_output *new_output = output_get_in_direction(output, dir);
	if (config->focus_wrapping != WRAP_WORKSPACE && new_output) {
		return get_node_in_output_direction(new_output, dir);
	}
	// If there is a wrap candidate, return its focus inactive view
	if (wrap_candidate) {
		struct swine_container *wrap_inactive =
			seat_get_focus_inactive_view(&wrap_candidate->node);
		return &wrap_inactive->node;
	}
	return NULL;
}

static struct swine_node *node_get_in_direction_floating(
		struct swine_container *con, enum swine_direction dir) {
	double ref_x = con->box.x + con->box.width / 2.0;
	double ref_y = con->box.y + con->box.height / 2.0;
	double closest_distance = 1e300;
	struct swine_container *closest_con = NULL;
	list_t *floating = con->workspace->floating;
	for (int i = 0; i < floating->length; i++) {
		struct swine_container *floater = floating->items[i];
		if (floater == con) {
			continue;
		}
		double dx = floater->box.x + floater->box.width / 2.0 - ref_x;
		double dy = floater->box.y + floater->box.height / 2.0 - ref_y;
		double distance;
		switch (dir) {
		case DIR_LEFT: distance = -dx; break;
		case DIR_RIGHT: distance = dx; break;
		case DIR_UP: distance = -dy; break;
		case DIR_DOWN: distance = dy; break;
		default: distance = -1; break;
		}
		if (distance > 0 && distance < closest_distance) {
			closest_distance = distance;
			closest_con = floater;
		}
	}
	return closest_con ? &closest_con->node : NULL;
}

static struct cmd_results *focus_mode(struct swine_workspace *ws, bool floating) {
	struct swine_container *new_focus = floating ?
		seat_get_focus_inactive_floating(ws) :
		seat_get_focus_inactive_tiling(ws);
	if (!new_focus) {
		return cmd_results_new(CMD_FAILURE, "Failed to find a %s container "
			"in workspace", floating ? "floating" : "tiling");
	}
	if (new_focus->children) {
		new_focus = seat_get_focus_inactive_view(&new_focus->node);
	}
	seat_set_focus_container(new_focus);
	return cmd_results_new(CMD_SUCCESS, NULL);
}

static struct cmd_results *focus_output(int argc, char **argv) {
	if (!argc) {
		return cmd_results_new(CMD_INVALID,
			"Expected 'focus output <direction|name>'");
	}
	char *identifier = join_args(argv, argc);
	struct swine_workspace *ws = config->handler_context.workspace;
	struct swine_output *output = output_by_name_or_direction(identifier,
		ws ? ws->output : NULL);
	if (!output) {
		struct cmd_results *res = cmd_results_new(CMD_FAILURE,
			"There is no output with that name or in that direction");
		free(identifier);
		return res;
	}
	free(identifier);
	struct swine_workspace *target = output_get_active_workspace(output);
	if (target) {
		struct swine_node *focus = seat_get_focus_inactive(&target->node);
		seat_set_focus(focus ? focus : &target->node);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

static struct cmd_results *focus_parent(void) {
	struct swine_node *node = config->handler_context.node;
	struct swine_node *parent = node ? node_get_parent(node) : NULL;
	if (parent && (parent->type == N_CONTAINER || parent->type == N_WORKSPACE)) {
		seat_set_focus(parent);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

static struct cmd_results *focus_child(void) {
	struct swine_node *node = config->handler_context.node;
	if (!node) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	struct swine_node *focus = seat_get_active_tiling_child(node);
	if (!focus && node->type == N_WORKSPACE) {
		focus = seat_get_focus_inactive(node);
		if (focus == node) {
			focus = NULL;
		}
	}
	if (focus) {
		seat_set_focus(focus);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_focus(int argc, char **argv) {
	struct swine_node *node = config->handler_context.node;
	struct swine_container *container = config->handler_context.container;
	struct swine_workspace *workspace = config->handler_context.workspace;
	if (!node || !workspace) {
		return cmd_results_new(CMD_FAILURE, "Nothing is focused");
	}

	if (argc == 0) {
		if (container) {
			seat_set_focus_container(container);
		}
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	if (swine_strcasecmp(argv[0], "floating") == 0) {
		return focus_mode(workspace, true);
	} else if (swine_strcasecmp(argv[0], "tiling") == 0) {
		return focus_mode(workspace, false);
	} else if (swine_strcasecmp(argv[0], "mode_toggle") == 0) {
		bool floating = container && container_is_floating_or_child(container);
		return focus_mode(workspace, !floating);
	}

	if (swine_strcasecmp(argv[0], "output") == 0) {
		return focus_output(argc - 1, argv + 1);
	}
	if (swine_strcasecmp(argv[0], "parent") == 0) {
		return focus_parent();
	}
	if (swine_strcasecmp(argv[0], "child") == 0) {
		return focus_child();
	}

	enum swine_direction direction;
	if (!parse_direction(argv[0], &direction)) {
		return cmd_results_new(CMD_INVALID,
			"Expected 'focus <direction|parent|child|mode_toggle|floating|"
			"tiling>' or 'focus output <direction|name>'");
	}

	if (node->type == N_WORKSPACE) {
		// Jump to the next output
		struct swine_output *new_output =
			output_get_in_direction(workspace->output, direction);
		if (!new_output) {
			return cmd_results_new(CMD_SUCCESS, NULL);
		}
		struct swine_node *focus = get_node_in_output_direction(new_output,
			direction);
		seat_set_focus(focus);
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	struct swine_node *next_focus = NULL;
	if (container_is_floating(container) &&
			container->fullscreen_mode == FULLSCREEN_NONE) {
		next_focus = node_get_in_direction_floating(container, direction);
	} else {
		next_focus = node_get_in_direction_tiling(container, direction, true);
	}
	if (next_focus) {
		seat_set_focus(next_focus);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}
