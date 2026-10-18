// Config settings that only store a value: default_orientation,
// workspace_layout, focus_wrapping, focus_follows_mouse, mouse_warping,
// floating_modifier, tiling_drag, tiling_drag_threshold,
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
	if (swine_strcasecmp(argv[0], "always") == 0) {
		config->focus_follows_mouse = FOLLOWS_ALWAYS;
	} else if (parse_boolean(argv[0], config->focus_follows_mouse == FOLLOWS_YES)) {
		config->focus_follows_mouse = FOLLOWS_YES;
	} else {
		config->focus_follows_mouse = FOLLOWS_NO;
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_mouse_warping(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "mouse_warping", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	if (swine_strcasecmp(argv[0], "container") == 0) {
		config->mouse_warping = WARP_CONTAINER;
	} else if (swine_strcasecmp(argv[0], "output") == 0) {
		config->mouse_warping = WARP_OUTPUT;
	} else if (swine_strcasecmp(argv[0], "none") == 0) {
		config->mouse_warping = WARP_NO;
	} else {
		return cmd_results_new(CMD_FAILURE,
			"Expected 'mouse_warping output|container|none'");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_tiling_drag(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "tiling_drag", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	config->tiling_drag = parse_boolean(argv[0], config->tiling_drag);
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_tiling_drag_threshold(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "tiling_drag_threshold", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	char *end = NULL;
	long value = strtol(argv[0], &end, 10);
	if (*end || value < 0) {
		return cmd_results_new(CMD_INVALID, "Invalid threshold specified");
	}
	config->tiling_drag_threshold = (int)value;
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
	if (argc == 1 || swine_strcasecmp(argv[1], "normal") == 0) {
		config->floating_mod_inverse = false;
	} else if (swine_strcasecmp(argv[1], "inverse") == 0) {
		config->floating_mod_inverse = true;
	} else {
		return cmd_results_new(CMD_INVALID,
			"Usage: floating_modifier <mod> [inverse|normal]");
	}
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
	char *files = join_args(argv, argc);
	// We don't care if the included config(s) fails to load.
	load_include_configs(files);
	free(files);
	return cmd_results_new(CMD_SUCCESS, NULL);
}

// swinewm extension: "windows_theme dark|light|auto|none". Switches
// Windows' own light/dark mode (which applications follow) while swinewm
// runs; "auto" follows the brightness of the bar's background. "none"
// leaves the user's setting alone (the default).
struct cmd_results *cmd_windows_theme(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "windows_theme", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	if (swine_strcasecmp(argv[0], "dark") == 0) {
		config->windows_theme = 1;
	} else if (swine_strcasecmp(argv[0], "light") == 0) {
		config->windows_theme = 0;
	} else if (swine_strcasecmp(argv[0], "auto") == 0) {
		config->windows_theme = 2;
	} else if (swine_strcasecmp(argv[0], "none") == 0) {
		config->windows_theme = -1;
	} else {
		return cmd_results_new(CMD_INVALID,
			"Expected 'windows_theme dark|light|auto|none'");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}
