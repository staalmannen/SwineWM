// "output": the background (swaybg options), and mode, position and
// transform through the platform (Windows display settings). Other
// subcommands are accepted and ignored so that sway configs load.
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/tree/output.h"

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

static struct output_config *new_output_config(const char *name) {
	struct output_config *oc = calloc(1, sizeof(*oc));
	oc->name = swine_strdup(name);
	oc->x = oc->y = INT_MIN;
	oc->transform = -1;
	return oc;
}

// Each subcommand parses its arguments from argv and returns how many it
// used, or -1 with *error set. Ported from sway/commands/output/*.c.
static int output_cmd_background(struct output_config *oc, int argc,
		char **argv, struct cmd_results **error) {
	if (argc < 1) {
		*error = cmd_results_new(CMD_INVALID, "Missing background");
		return -1;
	}
	const char *what = argv[0];
	const char *mode = argc > 1 ? argv[1] : NULL;
	if (mode && strcmp(mode, "solid_color") == 0) {
		if (what[0] != '#') {
			*error = cmd_results_new(CMD_INVALID,
				"solid_color expects a color, got '%s'", what);
			return -1;
		}
		set_str(&oc->background, what);
		set_str(&oc->background_mode, "solid_color");
		set_str(&oc->background_fallback, NULL);
		return 2;
	}
	if (!mode || !is_bg_mode(mode)) {
		*error = cmd_results_new(CMD_INVALID,
			"Expected 'output <name> bg <file> "
			"<stretch|fill|fit|center|tile> [<fallback color>]' or "
			"'output <name> bg <#color> solid_color'");
		return -1;
	}
	char *path = config_resolve_path(what);
	set_str(&oc->background, path);
	free(path);
	set_str(&oc->background_mode, mode);
	if (argc > 2 && argv[2][0] == '#') {
		set_str(&oc->background_fallback, argv[2]);
		return 3;
	}
	set_str(&oc->background_fallback, NULL);
	return 2;
}

static int output_cmd_mode(struct output_config *oc, int argc, char **argv,
		struct cmd_results **error) {
	int used = 0;
	if (argc > 0 && strcmp(argv[0], "--custom") == 0) {
		// Windows only offers the modes the driver lists.
		argv++;
		argc--;
		used++;
	}
	if (!argc) {
		*error = cmd_results_new(CMD_INVALID, "Missing mode argument.");
		return -1;
	}
	char *end;
	oc->width = (int)strtol(*argv, &end, 10);
	oc->refresh_rate = 0;
	if (*end) {
		// Format is 1234x4321
		if (*end != 'x') {
			*error = cmd_results_new(CMD_INVALID, "Invalid mode width.");
			return -1;
		}
		++end;
		oc->height = (int)strtol(end, &end, 10);
		if (*end) {
			if (*end != '@') {
				*error = cmd_results_new(CMD_INVALID, "Invalid mode height.");
				return -1;
			}
			++end;
			oc->refresh_rate = strtof(end, &end);
			if (swine_strcasecmp("Hz", end) != 0) {
				*error = cmd_results_new(CMD_INVALID,
					"Invalid mode refresh rate.");
				return -1;
			}
		}
		return used + 1;
	}
	// Format is 1234 4321
	if (argc < 2) {
		*error = cmd_results_new(CMD_INVALID,
			"Missing mode argument (height).");
		return -1;
	}
	oc->height = (int)strtol(argv[1], &end, 10);
	if (*end) {
		*error = cmd_results_new(CMD_INVALID, "Invalid mode height.");
		return -1;
	}
	return used + 2;
}

static int output_cmd_position(struct output_config *oc, int argc,
		char **argv, struct cmd_results **error) {
	if (!argc) {
		*error = cmd_results_new(CMD_INVALID, "Missing position argument.");
		return -1;
	}
	char *end;
	oc->x = (int)strtol(*argv, &end, 10);
	if (*end) {
		// Format is 1234,4321
		if (*end != ',') {
			*error = cmd_results_new(CMD_INVALID, "Invalid position x.");
			return -1;
		}
		++end;
		oc->y = (int)strtol(end, &end, 10);
		if (*end) {
			*error = cmd_results_new(CMD_INVALID, "Invalid position y.");
			return -1;
		}
		return 1;
	}
	// Format is 1234 4321 (legacy)
	if (argc < 2) {
		*error = cmd_results_new(CMD_INVALID, "Missing position argument (y).");
		return -1;
	}
	oc->y = (int)strtol(argv[1], &end, 10);
	if (*end) {
		*error = cmd_results_new(CMD_INVALID, "Invalid position y.");
		return -1;
	}
	return 2;
}

static int output_cmd_transform(struct output_config *oc, int argc,
		char **argv, struct cmd_results **error) {
	if (!argc) {
		*error = cmd_results_new(CMD_INVALID, "Missing transform argument.");
		return -1;
	}
	int transform;
	bool flipped = has_prefix(*argv, "flipped");
	const char *angle = *argv;
	if (flipped) {
		angle += strlen("flipped");
		angle += *angle == '-' ? 1 : 0;
		if (!*angle) {
			angle = "0";
		}
	}
	if (strcmp(angle, "normal") == 0 || strcmp(angle, "0") == 0) {
		transform = 0;
	} else if (strcmp(angle, "90") == 0) {
		transform = 1;
	} else if (strcmp(angle, "180") == 0) {
		transform = 2;
	} else if (strcmp(angle, "270") == 0) {
		transform = 3;
	} else {
		*error = cmd_results_new(CMD_INVALID, "Invalid output transform.");
		return -1;
	}
	if (flipped) {
		// Windows can rotate a display but not mirror it.
		swine_log(SWINE_INFO, "output %s: Windows cannot flip a display; "
			"using transform %d", oc->name, transform * 90);
	}
	if (argc > 1 && (strcmp(argv[1], "clockwise") == 0 ||
			strcmp(argv[1], "anticlockwise") == 0)) {
		if (config->reading) {
			*error = cmd_results_new(CMD_INVALID,
				"Relative transforms cannot be used in the configuration file");
			return -1;
		}
		if (strcmp(oc->name, "*") == 0) {
			*error = cmd_results_new(CMD_INVALID,
				"Cannot apply relative transform to all outputs.");
			return -1;
		}
		struct swine_output *output = output_by_name_or_id(oc->name);
		if (output == NULL) {
			*error = cmd_results_new(CMD_INVALID,
				"Cannot apply relative transform to unknown output %s",
				oc->name);
			return -1;
		}
		if (strcmp(argv[1], "anticlockwise") == 0) {
			transform = (4 - transform) % 4;
		}
		oc->transform = (output->transform + transform) % 4;
		return 2;
	}
	oc->transform = transform;
	return 1;
}

static int output_cmd_scale(struct output_config *oc, int argc, char **argv,
		struct cmd_results **error) {
	if (!argc) {
		*error = cmd_results_new(CMD_INVALID, "Missing scale argument.");
		return -1;
	}
	char *end;
	oc->scale = strtof(*argv, &end);
	if (*end) {
		*error = cmd_results_new(CMD_INVALID, "Invalid scale.");
		return -1;
	}
	return 1;
}

// Accepted so that sway configs load; they have no Windows counterpart
// here (see doc/compatibility.md). The number is how many arguments each
// takes.
static const struct {
	const char *name;
	int args;
} ignored_subcommands[] = {
	{ "adaptive_sync", 1 },
	{ "allow_tearing", 1 },
	{ "disable", 0 },
	{ "dpms", 1 },
	{ "enable", 0 },
	{ "hdr", 1 },
	{ "max_render_time", 1 },
	{ "modeline", 11 },
	{ "power", 1 },
	{ "render_bit_depth", 1 },
	{ "scale_filter", 1 },
	{ "subpixel", 1 },
	{ "toggle", 0 },
	{ "unplug", 0 },
};

static void merge_output_config(struct output_config *dst,
		struct output_config *src) {
	if (src->background) {
		set_str(&dst->background, src->background);
		set_str(&dst->background_mode, src->background_mode);
		set_str(&dst->background_fallback, src->background_fallback);
	}
	if (src->width > 0) {
		dst->width = src->width;
		dst->height = src->height;
		dst->refresh_rate = src->refresh_rate;
	}
	if (src->x != INT_MIN) {
		dst->x = src->x;
		dst->y = src->y;
	}
	if (src->transform >= 0) {
		dst->transform = src->transform;
	}
	if (src->scale > 0) {
		dst->scale = src->scale;
	}
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
	// Like sway, parse into a new config and merge it into the stored one
	// only if the whole line is valid.
	struct output_config *oc = new_output_config(argv[0]);
	bool display_changed = false;

	for (int i = 1; i < argc && !error; ) {
		const char *sub = argv[i];
		int rest = argc - i - 1;
		char **args = argv + i + 1;
		int used = -1;
		if (swine_strcasecmp(sub, "bg") == 0 ||
				swine_strcasecmp(sub, "background") == 0) {
			used = output_cmd_background(oc, rest, args, &error);
		} else if (swine_strcasecmp(sub, "mode") == 0 ||
				swine_strcasecmp(sub, "resolution") == 0 ||
				swine_strcasecmp(sub, "res") == 0) {
			used = output_cmd_mode(oc, rest, args, &error);
			display_changed = true;
		} else if (swine_strcasecmp(sub, "position") == 0 ||
				swine_strcasecmp(sub, "pos") == 0) {
			used = output_cmd_position(oc, rest, args, &error);
			display_changed = true;
		} else if (swine_strcasecmp(sub, "transform") == 0) {
			used = output_cmd_transform(oc, rest, args, &error);
			display_changed = true;
		} else if (swine_strcasecmp(sub, "scale") == 0) {
			used = output_cmd_scale(oc, rest, args, &error);
		} else if (swine_strcasecmp(sub, "color_profile") == 0) {
			used = rest > 0 && strcmp(args[0], "icc") == 0 ? 2 : 1;
		} else {
			for (size_t j = 0; j < sizeof(ignored_subcommands) /
					sizeof(ignored_subcommands[0]); ++j) {
				if (swine_strcasecmp(sub, ignored_subcommands[j].name) == 0) {
					used = ignored_subcommands[j].args;
					break;
				}
			}
			if (used < 0) {
				error = cmd_results_new(CMD_INVALID,
					"Invalid output subcommand: %s.", sub);
			}
		}
		if (used > rest) {
			error = error ? error : cmd_results_new(CMD_INVALID,
				"Missing argument for output %s", sub);
		}
		i += 1 + (used > 0 ? used : 0);
	}

	if (error) {
		free_output_config(oc);
		return error;
	}
	struct output_config *stored = config_find_output_config(argv[0]);
	if (stored) {
		merge_output_config(stored, oc);
		free_output_config(oc);
	} else {
		list_add(config->output_configs, oc);
	}
	// At runtime the change applies now; the config is applied as a whole
	// once it has been read.
	if (display_changed && !config->reading && config->active) {
		output_apply_configs();
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}
