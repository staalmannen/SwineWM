#include <stdlib.h>
#include <string.h>
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

static const char expected_syntax[] =
	"Expected 'move <left|right|up|down> <[px] px>' or "
	"'move [--no-auto-back-and-forth] <container|window> [to] workspace "
	"<name|next|prev|next_on_output|prev_on_output|current|back_and_forth>' "
	"or 'move <container|window|workspace> [to] output <name|direction>' or "
	"'move <container|window> [to] mark <mark>' or "
	"'move [container|window] [to] scratchpad' or "
	"'move [window|container] [to] [absolute] position center'";

static void reset_fractions(struct swine_container *con) {
	con->width_fraction = con->height_fraction = 0;
}

static void container_move_to_container_from_direction(
		struct swine_container *container, struct swine_container *destination,
		enum swine_direction move_dir) {
	if (destination->view) {
		if (destination->parent == container->parent &&
				destination->workspace == container->workspace) {
			swine_log(SWINE_DEBUG, "Swapping siblings");
			list_t *siblings = container_get_siblings(container);
			int container_index = list_find(siblings, container);
			int destination_index = list_find(siblings, destination);
			list_swap(siblings, container_index, destination_index);
		} else {
			swine_log(SWINE_DEBUG, "Promoting to sibling of cousin");
			int offset = move_dir == DIR_LEFT || move_dir == DIR_UP;
			int index = container_sibling_index(destination) + offset;
			if (destination->parent) {
				container_insert_child(destination->parent, container, index);
			} else {
				workspace_insert_tiling(destination->workspace, container, index);
			}
			reset_fractions(container);
			workspace_squash(destination->workspace);
		}
		return;
	}

	if (layout_parallel_to_direction(destination->layout, move_dir)) {
		swine_log(SWINE_DEBUG, "Reparenting container (parallel)");
		int index = move_dir == DIR_RIGHT || move_dir == DIR_DOWN ?
			0 : destination->children->length;
		container_insert_child(destination, container, index);
		reset_fractions(container);
		workspace_squash(destination->workspace);
		return;
	}

	swine_log(SWINE_DEBUG, "Reparenting container (perpendicular)");
	struct swine_node *focus_inactive =
		seat_get_active_tiling_child(&destination->node);
	if (!focus_inactive || focus_inactive == &destination->node) {
		// The container has no children
		container_add_child(destination, container);
		return;
	}
	// Try again but with the child
	container_move_to_container_from_direction(container,
		focus_inactive->container, move_dir);
}

static void container_move_to_workspace_from_direction(
		struct swine_container *container, struct swine_workspace *workspace,
		enum swine_direction move_dir) {
	reset_fractions(container);
	if (layout_parallel_to_direction(workspace->layout, move_dir)) {
		swine_log(SWINE_DEBUG, "Reparenting container (parallel)");
		int index = move_dir == DIR_RIGHT || move_dir == DIR_DOWN ?
			0 : workspace->tiling->length;
		workspace_insert_tiling(workspace, container, index);
		return;
	}
	swine_log(SWINE_DEBUG, "Reparenting container (perpendicular)");
	struct swine_container *focus_inactive =
		seat_get_focus_inactive_tiling(workspace);
	if (!focus_inactive) {
		// The workspace has no tiling children
		workspace_add_tiling(workspace, container);
		return;
	}
	while (focus_inactive->parent) {
		focus_inactive = focus_inactive->parent;
	}
	container_move_to_container_from_direction(container, focus_inactive,
		move_dir);
}

static void container_move_to_workspace(struct swine_container *container,
		struct swine_workspace *workspace) {
	if (container->workspace == workspace) {
		return;
	}
	if (container_is_floating(container)) {
		struct swine_output *old_output = container->workspace ?
			container->workspace->output : NULL;
		container_detach(container);
		workspace_add_floating(workspace, container);
		if (old_output != workspace->output && workspace->output) {
			container_floating_move_to_center(container);
		}
	} else {
		container_detach(container);
		if (workspace_is_empty(workspace) && container->children) {
			workspace_unwrap_children(workspace, container);
			container_reap_empty(container);
		} else {
			reset_fractions(container);
			workspace_add_tiling(workspace, container);
		}
	}
}

static void container_move_to_container(struct swine_container *container,
		struct swine_container *destination) {
	if (container == destination ||
			container_has_ancestor(destination, container)) {
		return;
	}
	if (container_is_floating(container)) {
		container_move_to_workspace(container, destination->workspace);
		return;
	}
	container_detach(container);
	reset_fractions(container);
	if (destination->view) {
		container_add_sibling(destination, container, true);
	} else {
		container_add_child(destination, container);
	}
}

static bool container_move_to_next_output(struct swine_container *container,
		struct swine_output *output, enum swine_direction move_dir) {
	struct swine_output *next_output = output_get_in_direction(output, move_dir);
	if (!next_output) {
		return false;
	}
	struct swine_workspace *ws = output_get_active_workspace(next_output);
	if (!swine_assert(ws, "Expected output to have a workspace")) {
		return false;
	}
	if (container->fullscreen_mode == FULLSCREEN_NONE) {
		container_move_to_workspace_from_direction(container, ws, move_dir);
	} else {
		container_move_to_workspace(container, ws);
	}
	return true;
}

// Returns true if moved. Port of sway's container_move_in_direction.
static bool container_move_in_direction(struct swine_container *container,
		enum swine_direction move_dir) {
	if (container->fullscreen_mode != FULLSCREEN_NONE) {
		return container_move_to_next_output(container,
			container->workspace->output, move_dir);
	}

	int offs = move_dir == DIR_LEFT || move_dir == DIR_UP ? -1 : 1;
	int index = -1;
	int desired = -1;
	list_t *siblings = NULL;
	struct swine_container *target = NULL;

	// Look for a suitable ancestor of the container to move within
	struct swine_container *ancestor = NULL;
	struct swine_container *current = container;
	bool wrapped = false;
	while (!ancestor) {
		// Don't allow containers to move out of their floating parent
		if (current->fullscreen_mode || container_is_floating(current)) {
			return false;
		}
		enum swine_container_layout parent_layout =
			container_parent_layout(current);
		if (!layout_parallel_to_direction(parent_layout, move_dir)) {
			if (!current->parent) {
				// No parallel parent, so we reorient the workspace
				current = workspace_wrap_children(current->workspace);
				current->workspace->layout =
					move_dir == DIR_LEFT || move_dir == DIR_RIGHT ?
					L_HORIZ : L_VERT;
				reset_fractions(container);
				wrapped = true;
			} else {
				// Keep looking for a parallel parent
				current = current->parent;
			}
			continue;
		}

		siblings = container_get_siblings(current);
		index = list_find(siblings, current);
		desired = index + offs;
		target = desired == -1 || desired == siblings->length ?
			NULL : siblings->items[desired];

		// If the move is simple we can complete it here early
		if (current == container) {
			if (target) {
				// Container will swap with or descend into its neighbor
				container_move_to_container_from_direction(container,
					target, move_dir);
				return true;
			} else if (!container->parent) {
				// Container is at workspace level so we move it to the
				// next workspace if possible
				return container_move_to_next_output(container,
					current->workspace->output, move_dir);
			} else {
				// Container has escaped its immediate parallel parent
				current = current->parent;
				continue;
			}
		}
		// We found a suitable ancestor, the loop will end
		ancestor = current;
	}

	if (target) {
		// Container will move in with its cousin
		container_move_to_container_from_direction(container, target, move_dir);
		return true;
	} else if (!wrapped && !container->parent->parent &&
			container->parent->children->length == 1) {
		// Treat singleton children as if they are at workspace level like i3
		return container_move_to_next_output(container,
			ancestor->workspace->output, move_dir);
	} else {
		// Container will be promoted
		struct swine_container *old_parent = container->parent;
		if (ancestor->parent) {
			// Container will move in with its parent
			container_insert_child(ancestor->parent, container,
				index + (offs < 0 ? 0 : 1));
		} else {
			// Container will move to workspace level,
			// may be re-split by workspace_layout
			workspace_insert_tiling(ancestor->workspace, container,
				index + (offs < 0 ? 0 : 1));
		}
		reset_fractions(ancestor);
		if (old_parent) {
			container_reap_empty(old_parent);
		}
		workspace_squash(container->workspace);
		return true;
	}
}

static struct cmd_results *cmd_move_in_direction(enum swine_direction direction,
		int argc, char **argv) {
	int move_amt = 10;
	if (argc) {
		char *end = NULL;
		move_amt = (int)strtol(argv[0], &end, 10);
		if (end == argv[0] || (*end && strcmp(end, "px") != 0)) {
			return cmd_results_new(CMD_INVALID, "Invalid distance specified");
		}
	}
	struct swine_container *container = config->handler_context.container;
	if (!container) {
		return cmd_results_new(CMD_FAILURE,
			"Cannot move workspaces in a direction");
	}
	if (container_is_floating(container)) {
		if (container->fullscreen_mode) {
			return cmd_results_new(CMD_FAILURE, "Can't move fullscreen floats");
		}
		switch (direction) {
		case DIR_LEFT: container->floating_box.x -= move_amt; break;
		case DIR_RIGHT: container->floating_box.x += move_amt; break;
		case DIR_UP: container->floating_box.y -= move_amt; break;
		case DIR_DOWN: container->floating_box.y += move_amt; break;
		}
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	struct swine_workspace *old_ws = container->workspace;
	struct swine_container *old_parent = container->parent;
	if (!container_move_in_direction(container, direction)) {
		// Container didn't move
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	// clean-up, destroying parents if the container was the last child
	if (old_parent) {
		container_reap_empty(old_parent);
	} else if (old_ws) {
		workspace_consider_destroy(old_ws);
	}
	// Moving to another output makes that output's workspace the focused one
	seat_set_focus_container(container);
	return cmd_results_new(CMD_SUCCESS, NULL);
}

static struct cmd_results *cmd_move_to_scratchpad(void);

static struct swine_output *destination_output(struct swine_node *node) {
	switch (node->type) {
	case N_OUTPUT:
		return node->output;
	case N_WORKSPACE:
		return node->workspace->output;
	case N_CONTAINER:
		return node->container->workspace ?
			node->container->workspace->output : NULL;
	case N_ROOT:
		break;
	}
	return NULL;
}

static struct cmd_results *cmd_move_container(bool no_auto_back_and_forth,
		int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "move container/window", EXPECTED_AT_LEAST, 2))) {
		return error;
	}
	struct swine_node *node = config->handler_context.node;
	struct swine_workspace *workspace = config->handler_context.workspace;
	struct swine_container *container = config->handler_context.container;
	if (!node || !workspace) {
		return cmd_results_new(CMD_FAILURE, "Nothing to move");
	}
	if (node->type == N_WORKSPACE) {
		if (workspace->tiling->length == 0) {
			return cmd_results_new(CMD_FAILURE, "Can't move an empty workspace");
		}
		container = workspace_wrap_children(workspace);
	}

	struct swine_container *old_parent = container->parent;
	struct swine_workspace *old_ws = container->workspace;
	struct swine_output *old_output = old_ws ? old_ws->output : NULL;
	struct swine_node *destination = NULL;

	if (swine_strcasecmp(argv[0], "workspace") == 0) {
		struct swine_workspace *ws = NULL;
		char *ws_name = NULL;
		if (swine_strcasecmp(argv[1], "next") == 0 ||
				swine_strcasecmp(argv[1], "prev") == 0 ||
				swine_strcasecmp(argv[1], "next_on_output") == 0 ||
				swine_strcasecmp(argv[1], "prev_on_output") == 0 ||
				swine_strcasecmp(argv[1], "current") == 0) {
			ws = workspace_by_name(argv[1]);
		} else if (swine_strcasecmp(argv[1], "back_and_forth") == 0) {
			if (!(ws = workspace_by_name(argv[1]))) {
				if (seat->prev_workspace_name) {
					ws_name = swine_strdup(seat->prev_workspace_name);
				} else {
					return cmd_results_new(CMD_FAILURE,
						"No workspace was previously active.");
				}
			}
		} else {
			if (swine_strcasecmp(argv[1], "number") == 0) {
				// move [window|container] [to] "workspace number x"
				if (argc < 3) {
					return cmd_results_new(CMD_INVALID, expected_syntax);
				}
				if (workspace_number(argv[2]) < 0) {
					return cmd_results_new(CMD_INVALID,
						"Invalid workspace number '%s'", argv[2]);
				}
				ws_name = join_args(argv + 2, argc - 2);
				ws = workspace_by_number(ws_name);
			} else {
				ws_name = join_args(argv + 1, argc - 1);
				ws = workspace_by_name(ws_name);
			}
			if (!no_auto_back_and_forth && config->auto_back_and_forth &&
					seat->prev_workspace_name) {
				// auto back and forth move
				if (old_ws && old_ws->name && strcmp(old_ws->name, ws_name) == 0) {
					// if target workspace is the current one
					free(ws_name);
					ws_name = swine_strdup(seat->prev_workspace_name);
					ws = workspace_by_name(ws_name);
				}
			}
		}
		if (!ws) {
			// We have to create the workspace, but if the container is
			// sticky and the workspace is going to be created on the same
			// output, we'll bail out first.
			if (container_is_sticky_or_child(container) &&
					workspace_get_initial_output(ws_name) == old_output) {
				free(ws_name);
				return cmd_results_new(CMD_FAILURE,
					"Can't move sticky container to another workspace "
					"on the same output");
			}
			ws = workspace_create(NULL, ws_name);
		}
		free(ws_name);
		struct swine_container *dst = seat_get_focus_inactive_tiling(ws);
		destination = dst ? &dst->node : &ws->node;
	} else if (swine_strcasecmp(argv[0], "output") == 0) {
		struct swine_output *new_output = output_by_name_or_direction(argv[1],
			old_output);
		if (!new_output) {
			return cmd_results_new(CMD_FAILURE,
				"Can't find output with name/direction '%s'", argv[1]);
		}
		destination = &new_output->node;
	} else if (swine_strcasecmp(argv[0], "mark") == 0) {
		struct swine_container *dest_con = container_find_mark(argv[1]);
		if (dest_con == NULL) {
			return cmd_results_new(CMD_FAILURE,
				"Mark '%s' not found", argv[1]);
		}
		destination = &dest_con->node;
	} else {
		return cmd_results_new(CMD_INVALID, expected_syntax);
	}

	if (destination->type == N_CONTAINER &&
			container_is_scratchpad_hidden(destination->container)) {
		return cmd_move_to_scratchpad();
	}

	if (container_is_sticky_or_child(container) && old_output &&
			destination_output(destination) == old_output) {
		return cmd_results_new(CMD_FAILURE, "Can't move sticky "
			"container to another workspace on the same output");
	}

	struct swine_node *focus = seat_get_focus();

	if (container_is_scratchpad_hidden(container)) {
		root_scratchpad_show(container);
	}
	switch (destination->type) {
	case N_WORKSPACE:
		container_move_to_workspace(container, destination->workspace);
		break;
	case N_OUTPUT: {
		struct swine_workspace *ws =
			output_get_active_workspace(destination->output);
		if (!swine_assert(ws, "Expected output to have a workspace")) {
			return cmd_results_new(CMD_FAILURE,
				"Expected output to have a workspace");
		}
		container_move_to_workspace(container, ws);
		break;
	}
	case N_CONTAINER:
		container_move_to_container(container, destination->container);
		break;
	case N_ROOT:
		break;
	}

	// restore focus
	if (focus == &container->node) {
		focus = NULL;
		if (old_parent) {
			focus = seat_get_focus_inactive(&old_parent->node);
		}
		if (!focus && old_ws) {
			focus = seat_get_focus_inactive(&old_ws->node);
		}
	}
	// The moved container stays the most recent focus on its new workspace.
	seat_set_raw_focus(&container->node);
	seat_set_focus(focus);

	// clean-up, destroying parents if the container was the last child
	if (old_parent) {
		container_reap_empty(old_parent);
	} else if (old_ws) {
		workspace_consider_destroy(old_ws);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

static struct cmd_results *cmd_move_to_scratchpad(void) {
	struct swine_node *node = config->handler_context.node;
	struct swine_container *con = config->handler_context.container;
	struct swine_workspace *ws = config->handler_context.workspace;
	if (!node) {
		return cmd_results_new(CMD_FAILURE, "Nothing to move");
	}
	if (node->type == N_WORKSPACE && ws->tiling->length == 0) {
		return cmd_results_new(CMD_INVALID,
			"Can't move an empty workspace to the scratchpad");
	}
	if (node->type == N_WORKSPACE) {
		// Wrap the workspace's children in a container
		con = workspace_wrap_children(ws);
		ws->layout = L_HORIZ;
	}
	// If the container is in a floating split container,
	// operate on the split container instead of the child.
	if (container_is_floating_or_child(con)) {
		con = container_toplevel_ancestor(con);
	}
	if (!con->scratchpad) {
		root_scratchpad_add_container(con);
	} else if (con->workspace) {
		root_scratchpad_hide(con);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

static struct cmd_results *cmd_move_to_position(int argc, char **argv) {
	struct swine_container *con = config->handler_context.container;
	if (!con || !container_is_floating(con)) {
		return cmd_results_new(CMD_FAILURE,
			"Only floating containers can have their position set");
	}
	if (argc == 1 && swine_strcasecmp(argv[0], "center") == 0) {
		container_floating_move_to_center(con);
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	if (argc >= 2) {
		char *end1 = NULL, *end2 = NULL;
		long x = strtol(argv[0], &end1, 10);
		int next = (*end1 == '\0' && argc >= 3 && strcmp(argv[1], "px") == 0) ? 2 : 1;
		long y = strtol(argv[next], &end2, 10);
		if (end1 != argv[0] && end2 != argv[next]) {
			struct swine_box area = con->workspace->output ?
				output_get_usable_area(con->workspace->output) : (struct swine_box){0};
			con->floating_box.x = area.x + (int)x;
			con->floating_box.y = area.y + (int)y;
			return cmd_results_new(CMD_SUCCESS, NULL);
		}
	}
	return cmd_results_new(CMD_INVALID, expected_syntax);
}

static struct cmd_results *cmd_move_workspace(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "move workspace", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	if (swine_strcasecmp(argv[0], "output") == 0) {
		--argc;
		++argv;
	}
	if (!argc) {
		return cmd_results_new(CMD_INVALID,
			"Expected 'move workspace to [output] <output>'");
	}

	struct swine_workspace *workspace = config->handler_context.workspace;
	if (!workspace) {
		return cmd_results_new(CMD_FAILURE, "No workspace to move");
	}

	struct swine_output *old_output = workspace->output;
	char *name = join_args(argv, argc);
	struct swine_output *new_output = output_by_name_or_direction(name,
		old_output);
	if (!new_output) {
		error = cmd_results_new(CMD_FAILURE,
			"Can't find output with name/direction '%s'", name);
		free(name);
		return error;
	}
	free(name);
	workspace_move_to_output(workspace, new_output);

	struct swine_node *focus = seat_get_focus_inactive(&workspace->node);
	seat_set_focus(focus ? focus : &workspace->node);
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_move(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "move", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	if (!root->outputs->length) {
		return cmd_results_new(CMD_INVALID,
			"Can't run this command while there's no outputs connected.");
	}

	enum swine_direction direction;
	if (parse_direction(argv[0], &direction)) {
		return cmd_move_in_direction(direction, argc - 1, argv + 1);
	} else if (swine_strcasecmp(argv[0], "workspace") == 0 && argc >= 2 &&
			(swine_strcasecmp(argv[1], "to") == 0 ||
			swine_strcasecmp(argv[1], "output") == 0)) {
		return cmd_move_workspace(argc - 2, argv + 2);
	}

	bool no_auto_back_and_forth = false;
	if (strcmp(argv[0], "--no-auto-back-and-forth") == 0) {
		no_auto_back_and_forth = true;
		--argc;
		++argv;
	}
	if (argc > 0 && (swine_strcasecmp(argv[0], "window") == 0 ||
			swine_strcasecmp(argv[0], "container") == 0)) {
		--argc;
		++argv;
	}
	if (argc > 0 && swine_strcasecmp(argv[0], "to") == 0) {
		--argc;
		++argv;
	}
	if (!argc) {
		return cmd_results_new(CMD_INVALID, expected_syntax);
	}

	if (swine_strcasecmp(argv[0], "workspace") == 0 ||
			swine_strcasecmp(argv[0], "output") == 0 ||
			swine_strcasecmp(argv[0], "mark") == 0) {
		return cmd_move_container(no_auto_back_and_forth, argc, argv);
	} else if (swine_strcasecmp(argv[0], "scratchpad") == 0) {
		return cmd_move_to_scratchpad();
	} else if (swine_strcasecmp(argv[0], "position") == 0) {
		return cmd_move_to_position(argc - 1, argv + 1);
	} else if (swine_strcasecmp(argv[0], "absolute") == 0 && argc > 1 &&
			swine_strcasecmp(argv[1], "position") == 0) {
		return cmd_move_to_position(argc - 2, argv + 2);
	}
	return cmd_results_new(CMD_INVALID, expected_syntax);
}
