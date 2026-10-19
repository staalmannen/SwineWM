#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"

// Must be in order for the bsearch
static const struct cmd_handler mode_handlers[] = {
	{ "bindcode", cmd_bindcode, 0 },
	{ "bindgesture", cmd_ignored, 0 },
	{ "bindswitch", cmd_ignored, 0 },
	{ "bindsym", cmd_bindsym, 0 },
	{ "unbindcode", cmd_unbindcode, 0 },
	{ "unbindsym", cmd_unbindsym, 0 },
};

struct cmd_results *cmd_mode(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "mode", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	if (strcmp(argv[0], "--pango_markup") == 0) {
		--argc;
		++argv;
		if ((error = checkarg(argc, "mode", EXPECTED_AT_LEAST, 1))) {
			return error;
		}
	}
	char *name = swine_strdup(argv[0]);
	strip_quotes(name);

	if (argc > 1) {
		// "mode <name> <command>" comes from a mode { } block in the config.
		if (!config->reading) {
			free(name);
			return cmd_results_new(CMD_INVALID,
				"Can only define modes in the config");
		}
		struct swine_mode *mode = config_add_mode(name);
		free(name);
		struct swine_mode *previous = config->current_mode;
		config->current_mode = mode;
		struct cmd_results *res = config_subcommand(argv + 1, argc - 1,
			mode_handlers, sizeof(mode_handlers));
		config->current_mode = previous;
		return res;
	}

	if (config->reading) {
		free(name);
		return cmd_results_new(CMD_INVALID,
			"Switching modes is only possible at runtime");
	}
	struct swine_mode *mode = config_find_mode(name);
	if (!mode) {
		error = cmd_results_new(CMD_INVALID, "Unknown mode '%s'", name);
		free(name);
		return error;
	}
	free(name);
	config->current_mode = mode;
	swine_log(SWINE_DEBUG, "Switched to mode '%s'", mode->name);
	return cmd_results_new(CMD_SUCCESS, NULL);
}
