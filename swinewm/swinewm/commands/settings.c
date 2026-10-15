// Config settings that only store a value: default_orientation,
// workspace_layout, focus_wrapping, focus_follows_mouse, floating_modifier,
// workspace_auto_back_and_forth and include.
#include <stdlib.h>
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/input/keyboard.h"

struct cmd_results *cmd_default_orientation(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "default_orientation", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	if (swine_strcasecmp(argv[0], "horizontal") == 0) {
		config->default_orientation = L_HORIZ;
	} else if (swine_strcasecmp(argv[0], "vertical") == 0) {
		config->default_orientation = L_VERT;
	} else if (swine_strcasecmp(argv[0], "auto") == 0) {
		config->default_orientation = L_NONE;
	} else {
		return cmd_results_new(CMD_INVALID,
			"Expected 'default_orientation <horizontal|vertical|auto>'");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_workspace_layout(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "workspace_layout", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	if (swine_strcasecmp(argv[0], "default") == 0) {
		config->default_layout = L_NONE;
	} else if (swine_strcasecmp(argv[0], "stacking") == 0) {
		config->default_layout = L_STACKED;
	} else if (swine_strcasecmp(argv[0], "tabbed") == 0) {
		config->default_layout = L_TABBED;
	} else {
		return cmd_results_new(CMD_INVALID,
			"Expected 'workspace_layout <default|stacking|tabbed>'");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_focus_wrapping(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "focus_wrapping", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	if (swine_strcasecmp(argv[0], "force") == 0) {
		config->focus_wrapping = WRAP_FORCE;
	} else if (swine_strcasecmp(argv[0], "workspace") == 0) {
		config->focus_wrapping = WRAP_WORKSPACE;
	} else if (parse_boolean(argv[0], config->focus_wrapping == WRAP_YES)) {
		config->focus_wrapping = WRAP_YES;
	} else {
		config->focus_wrapping = WRAP_NO;
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_focus_follows_mouse(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "focus_follows_mouse", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	config->focus_follows_mouse = swine_strcasecmp(argv[0], "always") == 0 ||
		parse_boolean(argv[0], config->focus_follows_mouse);
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_floating_modifier(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "floating_modifier", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	if (swine_strcasecmp(argv[0], "none") == 0) {
		config->floating_mod = 0;
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	uint32_t mod = 0;
	list_t *parts = split_string(argv[0], "+");
	for (int i = 0; i < parts->length; ++i) {
		uint32_t m = get_modifier_mask_by_name(parts->items[i]);
		if (!m) {
			list_free_items_and_destroy(parts);
			return cmd_results_new(CMD_INVALID, "Invalid modifier");
		}
		mod |= m;
	}
	list_free_items_and_destroy(parts);
	config->floating_mod = mod;
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_ws_auto_back_and_forth(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "workspace_auto_back_and_forth",
			EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	config->auto_back_and_forth =
		parse_boolean(argv[0], config->auto_back_and_forth);
	return cmd_results_new(CMD_SUCCESS, NULL);
}

// swinewm extension: "explorer_taskbar autohide|show". While swinewm runs
// next to Explorer, auto-hide Explorer's taskbar (restored on exit).
struct cmd_results *cmd_explorer_taskbar(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "explorer_taskbar", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	if (swine_strcasecmp(argv[0], "autohide") == 0) {
		config->explorer_taskbar_autohide = true;
	} else if (swine_strcasecmp(argv[0], "show") == 0) {
		config->explorer_taskbar_autohide = false;
	} else if (swine_strcasecmp(argv[0], "toggle") == 0) {
		config->explorer_taskbar_autohide = !config->explorer_taskbar_autohide;
	} else {
		return cmd_results_new(CMD_INVALID,
			"Expected 'explorer_taskbar autohide|show|toggle'");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_include(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "include", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	char *path = join_args(argv, argc);
	bool ok = load_include_config(path);
	error = ok ? cmd_results_new(CMD_SUCCESS, NULL) :
		cmd_results_new(CMD_FAILURE, "Unable to include '%s'", path);
	free(path);
	return error;
}
