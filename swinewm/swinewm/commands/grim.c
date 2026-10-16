// grim, the screenshot tool sway users bind to Print. There is no grim on
// Windows, so like notify-send it is a swinewm command, and "exec grim ..."
// in a sway config is routed here. Follows grim(1): the whole desktop, an
// output (-o) or a region (-g "x,y wxh"), PNG or JPEG (-t), saved to the
// given file or to $GRIM_DEFAULT_DIR / the Pictures folder as
// <date>_grim.png.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/platform.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"

static bool parse_geometry(const char *text, struct swine_box *box) {
	char extra;
	return sscanf(text, "%d,%d %dx%d%c", &box->x, &box->y, &box->width,
		&box->height, &extra) == 4 && box->width > 0 && box->height > 0;
}

// All outputs together, as grim captures by default.
static struct swine_box desktop_box(void) {
	struct swine_box box = {0};
	for (int i = 0; i < root->outputs->length; ++i) {
		struct swine_output *o = root->outputs->items[i];
		if (i == 0) {
			box = o->box;
			continue;
		}
		int x2 = box.x + box.width, y2 = box.y + box.height;
		int ox2 = o->box.x + o->box.width, oy2 = o->box.y + o->box.height;
		box.x = o->box.x < box.x ? o->box.x : box.x;
		box.y = o->box.y < box.y ? o->box.y : box.y;
		box.width = (ox2 > x2 ? ox2 : x2) - box.x;
		box.height = (oy2 > y2 ? oy2 : y2) - box.y;
	}
	return box;
}

static char *default_path(bool jpeg) {
	char dir_buf[1024];
	const char *dir = getenv("GRIM_DEFAULT_DIR");
	char *pictures = NULL;
	if (!dir || !*dir) {
		pictures = platform_pictures_dir();
		dir = pictures ? pictures : ".";
	}
	snprintf(dir_buf, sizeof(dir_buf), "%s", dir);
	free(pictures);
	char name[64];
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	if (!tm || !strftime(name, sizeof(name), "%Y%m%d_%Hh%Mm%Ss_grim", tm)) {
		snprintf(name, sizeof(name), "grim");
	}
	const char *sep = strchr(dir_buf, '\\') ? "\\" : "/";
	return format_str("%s%s%s.%s", dir_buf, sep, name, jpeg ? "jpeg" : "png");
}

struct cmd_results *cmd_grim(int argc, char **argv) {
	struct swine_box box = desktop_box();
	bool jpeg = false;
	const char *file = NULL;
	for (int i = 0; i < argc; ++i) {
		const char *arg = argv[i];
		bool has_value = i + 1 < argc;
		if (strcmp(arg, "-g") == 0 && has_value) {
			if (!parse_geometry(argv[++i], &box)) {
				return cmd_results_new(CMD_INVALID,
					"Invalid geometry '%s' (expected \"x,y wxh\")", argv[i]);
			}
		} else if (strcmp(arg, "-o") == 0 && has_value) {
			struct swine_output *output = output_by_name_or_id(argv[++i]);
			if (!output) {
				return cmd_results_new(CMD_INVALID, "Unknown output '%s'",
					argv[i]);
			}
			box = output->box;
		} else if (strcmp(arg, "-t") == 0 && has_value) {
			++i;
			if (swine_strcasecmp(argv[i], "jpeg") == 0 ||
					swine_strcasecmp(argv[i], "jpg") == 0) {
				jpeg = true;
			} else if (swine_strcasecmp(argv[i], "png") != 0) {
				return cmd_results_new(CMD_INVALID,
					"Unsupported image type '%s' (png or jpeg)", argv[i]);
			}
		} else if ((strcmp(arg, "-q") == 0 || strcmp(arg, "-l") == 0 ||
				strcmp(arg, "-s") == 0) && has_value) {
			++i;   // quality, compression level, scale: defaults are used
		} else if (strcmp(arg, "-c") == 0) {
			// The cursor is never part of the capture.
		} else if (strcmp(arg, "-h") == 0) {
			return cmd_results_new(CMD_SUCCESS,
				"Usage: grim [-t png|jpeg] [-g \"x,y wxh\"] [-o output] "
				"[output-file]");
		} else if (arg[0] == '-' && arg[1] != '\0') {
			return cmd_results_new(CMD_INVALID, "Unsupported grim option '%s'",
				arg);
		} else if (!file) {
			file = arg;
		} else {
			return cmd_results_new(CMD_INVALID, "Too many arguments to grim");
		}
	}
	if (file && strcmp(file, "-") == 0) {
		return cmd_results_new(CMD_INVALID,
			"grim: writing to standard output is not supported");
	}
	if (box.width <= 0 || box.height <= 0) {
		return cmd_results_new(CMD_FAILURE, "Nothing to capture");
	}
	char *path = file ? platform_expand_path(file) : default_path(jpeg);
	if (file) {
		size_t len = strlen(path);
		jpeg = jpeg || (len > 4 && (swine_strcasecmp(path + len - 4, ".jpg") == 0 ||
			(len > 5 && swine_strcasecmp(path + len - 5, ".jpeg") == 0)));
	}
	char *error = NULL;
	if (!platform_screenshot(&box, path, jpeg, &error)) {
		struct cmd_results *res = cmd_results_new(CMD_FAILURE,
			"Unable to save screenshot %s: %s", path, error ? error : "");
		free(error);
		free(path);
		return res;
	}
	swine_log(SWINE_INFO, "Saved screenshot %s", path);
	free(path);
	return cmd_results_new(CMD_SUCCESS, NULL);
}
