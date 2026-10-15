// "output": only the background (swaybg) options are implemented. Display
// mode, position, scale and the rest are Windows display settings and are
// accepted and ignored so that sway configs load.
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"

static bool is_bg_mode(const char *mode) {
	static const char *modes[] = {
		"stretch", "fill", "fit", "center", "tile", "solid_color",
	};
	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
		if (strcmp(mode, modes[i]) == 0) {
			return true;
		}
	}
	return false;
}

static void set_str(char **field, const char *value) {
	free(*field);
	*field = value ? swine_strdup(value) : NULL;
}

struct cmd_results *cmd_output(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "output", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	if (argc == 1) {
		// "output <name> { ... }" with an empty block
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	struct output_config *oc = config_find_output_config(argv[0]);
	bool created = false;
	if (!oc) {
		oc = calloc(1, sizeof(*oc));
		oc->name = swine_strdup(argv[0]);
		created = true;
	}

	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "bg") != 0 && strcmp(argv[i], "background") != 0) {
			continue;
		}
		if (i + 1 >= argc) {
			error = cmd_results_new(CMD_INVALID, "Missing background");
			break;
		}
		const char *what = argv[i + 1];
		const char *mode = i + 2 < argc ? argv[i + 2] : NULL;
		if (mode && strcmp(mode, "solid_color") == 0) {
			if (what[0] != '#') {
				error = cmd_results_new(CMD_INVALID,
					"solid_color expects a color, got '%s'", what);
				break;
			}
			set_str(&oc->background, what);
			set_str(&oc->background_mode, "solid_color");
			set_str(&oc->background_fallback, NULL);
			i += 2;
			continue;
		}
		if (!mode || !is_bg_mode(mode)) {
			error = cmd_results_new(CMD_INVALID,
				"Expected 'output <name> bg <file> "
				"<stretch|fill|fit|center|tile> [<fallback color>]' or "
				"'output <name> bg <#color> solid_color'");
			break;
		}
		char *path = config_resolve_path(what);
		set_str(&oc->background, path);
		free(path);
		set_str(&oc->background_mode, mode);
		i += 2;
		if (i + 1 < argc && argv[i + 1][0] == '#') {
			set_str(&oc->background_fallback, argv[++i]);
		} else {
			set_str(&oc->background_fallback, NULL);
		}
	}

	if (error) {
		if (created) {
			free(oc->name);
			free(oc->background);
			free(oc->background_mode);
			free(oc->background_fallback);
			free(oc);
		}
		return error;
	}
	if (created) {
		list_add(config->output_configs, oc);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}
