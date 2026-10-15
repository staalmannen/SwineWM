// floating, fullscreen and scratchpad.
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/input/seat.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/view.h"
#include "swinewm/tree/workspace.h"

struct cmd_results *cmd_floating(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "floating", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	struct swine_container *container = config->handler_context.container;
	if (!container) {
		return cmd_results_new(CMD_INVALID,
			"Can only make a window or container float");
	}
	if (container_is_scratchpad_hidden(container)) {
		return cmd_results_new(CMD_INVALID,
			"Can't change floating on hidden scratchpad container");
	}
	// If the container is in a floating split container, operate on the
	// split container instead of the child.
	if (container_is_floating_or_child(container)) {
		container = container_toplevel_ancestor(container);
	}
	bool wants_floating = parse_boolean(argv[0], container_is_floating(container));
	container_set_floating(container, wants_floating);
	seat_set_focus_container(container);
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_fullscreen(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "fullscreen", EXPECTED_AT_MOST, 2))) {
		return error;
	}
	struct swine_container *container = config->handler_context.container;
	struct swine_workspace *ws = config->handler_context.workspace;
	if (!container) {
		// If the focus is not a container, do nothing successfully
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	if (!ws) {
		return cmd_results_new(CMD_FAILURE,
			"Can't fullscreen a hidden scratchpad container");
	}
	bool is_fullscreen = container->fullscreen_mode != FULLSCREEN_NONE;
	bool global = false;
	bool enable = !is_fullscreen;
	if (argc >= 1) {
		if (swine_strcasecmp(argv[0], "global") == 0) {
			global = true;
		} else {
			enable = parse_boolean(argv[0], is_fullscreen);
		}
	}
	if (argc >= 2) {
		global = swine_strcasecmp(argv[1], "global") == 0;
	}
	container_set_fullscreen(container, enable ?
		(global ? FULLSCREEN_GLOBAL : FULLSCREEN_WORKSPACE) : FULLSCREEN_NONE);
	return cmd_results_new(CMD_SUCCESS, NULL);
}

static void scratchpad_toggle_auto(void) {
	struct swine_container *focus = seat_get_focused_container();
	struct swine_workspace *ws = seat_get_focused_workspace();
	if (focus && focus->scratchpad) {
		root_scratchpad_hide(focus);
		return;
	}
	// A scratchpad window that is visible on the current workspace but not
	// focused gets focused.
	if (ws) {
		for (int i = 0; i < ws->floating->length; ++i) {
			struct swine_container *floater = ws->floating->items[i];
			if (floater->scratchpad && focus != floater) {
				seat_set_focus_container(floater);
				return;
			}
		}
	}
	// Show the first hidden (or elsewhere visible) scratchpad window.
	if (root->scratchpad->length) {
		root_scratchpad_show(root->scratchpad->items[0]);
	}
}

struct cmd_results *cmd_scratchpad(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "scratchpad", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	if (swine_strcasecmp(argv[0], "show") != 0) {
		return cmd_results_new(CMD_INVALID, "Expected 'scratchpad show'");
	}
	if (!root->scratchpad->length) {
		return cmd_results_new(CMD_INVALID, "Scratchpad is empty");
	}
	scratchpad_toggle_auto();
	return cmd_results_new(CMD_SUCCESS, NULL);
}

// urgent enable|disable|allow|deny (sway/commands/urgent.c)
struct cmd_results *cmd_urgent(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "urgent", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	struct swine_container *container = config->handler_context.container;
	if (!container || !container->view) {
		return cmd_results_new(CMD_INVALID, "Only views can be urgent");
	}
	struct swine_view *view = container->view;
	if (swine_strcasecmp(argv[0], "allow") == 0) {
		view->urgency_denied = false;
	} else if (swine_strcasecmp(argv[0], "deny") == 0) {
		view->urgency_denied = true;
	} else {
		view_set_urgent(view, parse_boolean(argv[0], view->urgent));
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}
