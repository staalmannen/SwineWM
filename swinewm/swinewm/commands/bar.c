// "bar" enables swinewm's integrated Waybar-compatible bar. sway's own
// swaybar options are accepted; the few that make sense here are honoured
// and the bar's content and look come from the Waybar config and style.
#include <string.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"

static const char *const ignored_subcommands[] = {
	"binding_mode_indicator", "bindcode", "bindsym", "colors", "font",
	"gaps", "height", "hidden_state", "icon_theme", "id", "modifier",
	"output", "pango_markup", "separator_symbol", "status_command",
	"status_edge_padding", "status_padding", "strip_workspace_name",
	"strip_workspace_numbers", "tray_bindcode", "tray_bindsym",
	"tray_output", "tray_padding", "workspace_buttons",
	"workspace_min_width", "wrap_scroll",
};

static bool is_known(const char *word) {
	if (strcmp(word, "position") == 0 || strcmp(word, "mode") == 0 ||
			strcmp(word, "swaybar_command") == 0) {
		return true;
	}
	for (size_t i = 0; i < sizeof(ignored_subcommands) /
			sizeof(ignored_subcommands[0]); ++i) {
		if (strcmp(word, ignored_subcommands[i]) == 0) {
			return true;
		}
	}
	return false;
}

struct cmd_results *cmd_bar(int argc, char **argv) {
	config->bar_enabled = true;
	if (argc == 0) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	// "bar <id> <subcommand>": skip a bar id such as "bar-0".
	if (argc == 1 && !is_known(argv[0])) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	if (argc > 1 && !is_known(argv[0])) {
		--argc;
		++argv;
	}
	if (strcmp(argv[0], "position") == 0 && argc >= 2) {
		if (strcmp(argv[1], "top") == 0) {
			config->bar_position = 1;
		} else if (strcmp(argv[1], "bottom") == 0) {
			config->bar_position = 2;
		} else {
			return cmd_results_new(CMD_INVALID,
				"Only 'position top|bottom' is supported");
		}
	} else if (strcmp(argv[0], "mode") == 0 && argc >= 2) {
		const char *mode = argv[argc - 1];
		if (strcmp(mode, "invisible") == 0) {
			config->bar_invisible = true;
		} else if (strcmp(mode, "toggle") == 0) {
			config->bar_invisible = !config->bar_invisible;
		} else {
			// dock, hide and overlay all show the bar for now
			config->bar_invisible = false;
		}
	} else if (strcmp(argv[0], "swaybar_command") == 0) {
		// Waybar or swaybar: either way the integrated bar is used.
	} else if (is_known(argv[0])) {
		swine_log(SWINE_DEBUG, "bar %s: style the integrated bar with "
			"waybar/style.css instead", argv[0]);
	} else {
		return cmd_results_new(CMD_INVALID, "Unknown bar subcommand '%s'",
			argv[0]);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}
