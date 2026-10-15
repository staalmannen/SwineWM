#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/workspace.h"

#define MIN_SANE_W 100
#define MIN_SANE_H 60

enum resize_unit {
	UNIT_DEFAULT,
	UNIT_PX,
	UNIT_PPT,
};

// Parses "<n> [px|ppt]" from argv. Returns the number of arguments used.
static int parse_amount(int argc, char **argv, int *amount,
		enum resize_unit *unit) {
	*amount = 10;
	*unit = UNIT_DEFAULT;
	if (argc < 1) {
		return 0;
	}
	char *end = NULL;
	long n = strtol(argv[0], &end, 10);
	if (end == argv[0]) {
		return 0;
	}
	*amount = (int)n;
	if (*end) {
		if (strcmp(end, "px") == 0) {
			*unit = UNIT_PX;
		} else if (strcmp(end, "ppt") == 0) {
			*unit = UNIT_PPT;
		}
		return 1;
	}
	if (argc > 1) {
		if (swine_strcasecmp(argv[1], "px") == 0) {
			*unit = UNIT_PX;
			return 2;
		} else if (swine_strcasecmp(argv[1], "ppt") == 0) {
			*unit = UNIT_PPT;
			return 2;
		}
	}
	return 1;
}

static struct swine_container *find_resize_parent(struct swine_container *con,
		bool horizontal) {
	enum swine_container_layout parallel = horizontal ? L_HORIZ : L_VERT;
	while (con) {
		list_t *siblings = container_get_siblings(con);
		if (container_parent_layout(con) == parallel && siblings &&
				siblings->length > 1) {
			return con;
		}
		con = con->parent;
	}
	return NULL;
}

// Port of sway's container_resize_tiled for the "width"/"height" axes:
// the container grows and all its siblings shrink by an equal share.
static bool resize_tiled(struct swine_container *con, bool horizontal,
		int amount) {
	con = find_resize_parent(con, horizontal);
	if (!con) {
		return false;
	}
	list_t *siblings = container_get_siblings(con);
	int sibling_amount = (int)ceil((double)amount / (siblings->length - 1));
	double child_total = 0;
	for (int i = 0; i < siblings->length; ++i) {
		struct swine_container *sibling = siblings->items[i];
		int len = horizontal ? sibling->box.width : sibling->box.height;
		int change = sibling == con ? amount : -sibling_amount;
		if (len + change < (horizontal ? MIN_SANE_W : MIN_SANE_H)) {
			return false;
		}
		child_total += len;
	}
	if (child_total <= 0) {
		return false;
	}
	// Snap fractions to the current pixel sizes to avoid rounding drift.
	for (int i = 0; i < siblings->length; ++i) {
		struct swine_container *sibling = siblings->items[i];
		double len = horizontal ? sibling->box.width : sibling->box.height;
		*(horizontal ? &sibling->width_fraction : &sibling->height_fraction) =
			len / child_total;
	}
	double amount_fraction = amount / child_total;
	double sibling_fraction = amount_fraction / (siblings->length - 1);
	for (int i = 0; i < siblings->length; ++i) {
		struct swine_container *sibling = siblings->items[i];
		double *f = horizontal ?
			&sibling->width_fraction : &sibling->height_fraction;
		*f += sibling == con ? amount_fraction : -sibling_fraction;
	}
	return true;
}

static int ppt_to_px(struct swine_container *con, bool horizontal, int ppt) {
	enum swine_container_layout want = horizontal ? L_HORIZ : L_VERT;
	struct swine_container *parent = con->parent;
	while (parent && parent->layout != want) {
		parent = parent->parent;
	}
	int base;
	if (parent) {
		base = horizontal ? parent->box.width : parent->box.height;
	} else {
		base = horizontal ? con->workspace->box.width : con->workspace->box.height;
	}
	return base * ppt / 100;
}

struct cmd_results *cmd_resize(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "resize", EXPECTED_AT_LEAST, 2))) {
		return error;
	}
	struct swine_container *con = config->handler_context.container;
	if (!con || !con->workspace) {
		return cmd_results_new(CMD_INVALID, "Cannot resize nothing");
	}
	if (swine_strcasecmp(argv[0], "set") == 0) {
		return cmd_results_new(CMD_FAILURE, "'resize set' is not implemented yet");
	}
	int sign;
	if (swine_strcasecmp(argv[0], "grow") == 0) {
		sign = 1;
	} else if (swine_strcasecmp(argv[0], "shrink") == 0) {
		sign = -1;
	} else {
		return cmd_results_new(CMD_INVALID,
			"Expected 'resize <shrink|grow> <width|height> [<amount>] [px|ppt]'");
	}
	bool horizontal;
	const char *axis = argv[1];
	if (swine_strcasecmp(axis, "width") == 0 ||
			swine_strcasecmp(axis, "horizontal") == 0) {
		horizontal = true;
	} else if (swine_strcasecmp(axis, "height") == 0 ||
			swine_strcasecmp(axis, "vertical") == 0) {
		horizontal = false;
	} else {
		return cmd_results_new(CMD_INVALID,
			"Resizing individual edges (up/down/left/right) is not "
			"implemented yet");
	}
	int amount;
	enum resize_unit unit;
	parse_amount(argc - 2, argv + 2, &amount, &unit);

	if (container_is_floating(con)) {
		if (unit == UNIT_PPT) {
			struct swine_box *ws_box = &con->workspace->box;
			amount = (horizontal ? ws_box->width : ws_box->height) * amount / 100;
		}
		int delta = sign * amount;
		if (horizontal) {
			int width = con->floating_box.width + delta;
			if (width < MIN_SANE_W) {
				return cmd_results_new(CMD_FAILURE, "Cannot resize any further");
			}
			con->floating_box.x -= delta / 2;
			con->floating_box.width = width;
		} else {
			int height = con->floating_box.height + delta;
			if (height < MIN_SANE_H) {
				return cmd_results_new(CMD_FAILURE, "Cannot resize any further");
			}
			con->floating_box.y -= delta / 2;
			con->floating_box.height = height;
		}
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	if (unit != UNIT_PX) {
		amount = ppt_to_px(con, horizontal, amount);
	}
	if (!resize_tiled(con, horizontal, sign * amount)) {
		return cmd_results_new(CMD_INVALID, "Cannot resize any further");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}
