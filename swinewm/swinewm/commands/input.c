// input <identifier> <option>: sway's input configuration. Windows owns the
// keyboard and pointer settings, so only xkb_switch_layout (switching
// between the layouts installed in Windows) has an effect; the other
// options are accepted and ignored so that sway configs load cleanly.
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/input/keyboard.h"
#include "swinewm/platform.h"

// swinewm has one keyboard, "swinewm:keyboard" in get_inputs.
static bool matches_keyboard(const char *identifier) {
	return strcmp(identifier, "*") == 0 ||
		strcmp(identifier, "type:keyboard") == 0 ||
		strcmp(identifier, "swinewm:keyboard") == 0;
}

// sway's input_cmd_xkb_switch_layout
static struct cmd_results *input_cmd_xkb_switch_layout(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "xkb_switch_layout", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	if (config->reading || !config->active) {
		return cmd_results_new(CMD_DEFER, NULL);
	}
	const char *layout_str = argv[0];
	int relative = 0, layout = 0;
	if (strcmp(layout_str, "next") == 0) {
		relative = 1;
	} else if (strcmp(layout_str, "prev") == 0) {
		relative = -1;
	} else {
		char *end;
		long parsed = strtol(layout_str, &end, 10);
		if (layout_str[0] == '\0' || end[0] != '\0') {
			return cmd_results_new(CMD_FAILURE, "Invalid argument.");
		} else if (parsed < 0 || parsed > 1000) {
			return cmd_results_new(CMD_FAILURE, "Invalid layout index.");
		}
		layout = (int)parsed;
	}
	// Ask Windows, not the cache: the user may have switched with
	// Windows' own hotkey since the last refresh.
	keyboard_layouts_refresh();
	list_t *layouts = keyboard_layouts_get();
	int count = layouts ? layouts->length : 0;
	if (count == 0) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	if (relative) {
		int current = keyboard_layouts_active();
		if (current < 0) {
			current = 0;
		}
		layout = (current + count + relative) % count;
	} else if (layout >= count) {
		// sway leaves a keyboard alone when it has no such layout
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	platform_keyboard_set_layout(layout);
	keyboard_layouts_refresh();
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_input(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "input", EXPECTED_AT_LEAST, 2))) {
		return error;
	}
	if (strcmp(argv[1], "xkb_switch_layout") == 0) {
		if (!matches_keyboard(argv[0])) {
			return cmd_results_new(CMD_SUCCESS, NULL);
		}
		return input_cmd_xkb_switch_layout(argc - 2, argv + 2);
	}
	return cmd_ignored(argc, argv);
}
