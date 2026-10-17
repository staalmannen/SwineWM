// resize, ported from sway/commands/resize.c (Copyright (c) 2016-2017 Drew
// DeVault, MIT): grow/shrink by width, height or one edge (up, down, left,
// right), with "or" fallback units, and "resize set".
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/workspace.h"

#define MIN_SANE_W 100
#define MIN_SANE_H 60

// sway's defaults for floating_minimum_size / floating_maximum_size.
#define FLOATING_MIN_W 75
#define FLOATING_MIN_H 50

enum {
	EDGE_NONE = SWINE_EDGE_NONE,
	EDGE_TOP = SWINE_EDGE_TOP,
	EDGE_BOTTOM = SWINE_EDGE_BOTTOM,
	EDGE_LEFT = SWINE_EDGE_LEFT,
	EDGE_RIGHT = SWINE_EDGE_RIGHT,
};
#define AXIS_HORIZONTAL (EDGE_LEFT | EDGE_RIGHT)
#define AXIS_VERTICAL (EDGE_TOP | EDGE_BOTTOM)

enum movement_unit {
	UNIT_PX,
	UNIT_PPT,
	UNIT_DEFAULT,
	UNIT_INVALID,
};

struct movement_amount {
	int amount;
	enum movement_unit unit;
};

// "<n> [px|ppt]" or "<n>px": returns the arguments used (unit INVALID if
// argv does not start with a number), as sway's parse_movement_amount.
static int parse_movement_amount(int argc, char **argv,
		struct movement_amount *amount) {
	amount->unit = UNIT_INVALID;
	amount->amount = 0;
	if (argc < 1) {
		return 0;
	}
	char *end = NULL;
	long n = strtol(argv[0], &end, 10);
	if (end == argv[0]) {
		return 0;
	}
	amount->amount = (int)n;
	if (*end) {
		if (swine_strcasecmp(end, "px") == 0) {
			amount->unit = UNIT_PX;
		} else if (swine_strcasecmp(end, "ppt") == 0) {
			amount->unit = UNIT_PPT;
		}
		return 1;
	}
	if (argc > 1) {
		if (swine_strcasecmp(argv[1], "px") == 0) {
			amount->unit = UNIT_PX;
			return 2;
		} else if (swine_strcasecmp(argv[1], "ppt") == 0) {
			amount->unit = UNIT_PPT;
			return 2;
		}
	}
	amount->unit = UNIT_DEFAULT;
	return 1;
}

static unsigned parse_resize_axis(const char *axis) {
	if (swine_strcasecmp(axis, "width") == 0 ||
			swine_strcasecmp(axis, "horizontal") == 0) {
		return AXIS_HORIZONTAL;
	}
	if (swine_strcasecmp(axis, "height") == 0 ||
			swine_strcasecmp(axis, "vertical") == 0) {
		return AXIS_VERTICAL;
	}
	if (swine_strcasecmp(axis, "up") == 0) {
		return EDGE_TOP;
	}
	if (swine_strcasecmp(axis, "down") == 0) {
		return EDGE_BOTTOM;
	}
	if (swine_strcasecmp(axis, "left") == 0) {
		return EDGE_LEFT;
	}
	if (swine_strcasecmp(axis, "right") == 0) {
		return EDGE_RIGHT;
	}
	return EDGE_NONE;
}

static bool is_horizontal(unsigned axis) {
	return (axis & (EDGE_LEFT | EDGE_RIGHT)) != 0;
}

struct swine_container *container_find_resize_parent(
		struct swine_container *con, unsigned axis) {
	enum swine_container_layout parallel_layout =
		is_horizontal(axis) ? L_HORIZ : L_VERT;
	bool allow_first = axis != EDGE_TOP && axis != EDGE_LEFT;
	bool allow_last = axis != EDGE_RIGHT && axis != EDGE_BOTTOM;
	while (con) {
		list_t *siblings = container_get_siblings(con);
		int index = container_sibling_index(con);
		if (container_parent_layout(con) == parallel_layout && siblings &&
				siblings->length > 1 && (allow_first || index > 0) &&
				(allow_last || index < siblings->length - 1)) {
			return con;
		}
		con = con->parent;
	}
	return NULL;
}

static bool hidden_scratchpad_or_child(struct swine_container *con) {
	return container_is_scratchpad_hidden(container_toplevel_ancestor(con));
}

void container_resize_tiled(struct swine_container *con,
		unsigned axis, int amount) {
	con = container_find_resize_parent(con, axis);
	if (!con || hidden_scratchpad_or_child(con)) {
		return;
	}
	// For width or height, grow at both sides: every sibling gives up a
	// share. For an edge, only the neighbour on that side does.
	list_t *resize = create_list();
	list_t *siblings = container_get_siblings(con);
	int index = container_sibling_index(con);
	if (axis == AXIS_HORIZONTAL || axis == AXIS_VERTICAL) {
		list_cat(resize, siblings);
	} else if (axis == EDGE_TOP || axis == EDGE_LEFT) {
		list_add(resize, siblings->items[index - 1]);
		list_add(resize, con);
	} else {
		list_add(resize, con);
		list_add(resize, siblings->items[index + 1]);
	}

	bool horizontal = is_horizontal(axis);
	int sibling_amount = (int)ceil((double)amount / (double)(resize->length - 1));
	for (int i = 0; i < resize->length; i++) {
		struct swine_container *sibling = resize->items[i];
		int len = horizontal ? sibling->box.width : sibling->box.height;
		int change = sibling == con ? amount : -sibling_amount;
		if (len + change < (horizontal ? MIN_SANE_W : MIN_SANE_H)) {
			goto cleanup;
		}
	}
	double child_total = 0;
	for (int i = 0; i < siblings->length; ++i) {
		struct swine_container *sibling = siblings->items[i];
		child_total += horizontal ? sibling->box.width : sibling->box.height;
	}
	if (child_total <= 0) {
		goto cleanup;
	}
	// Snap all fractions to full pixels to avoid rounding issues.
	for (int i = 0; i < siblings->length; ++i) {
		struct swine_container *sibling = siblings->items[i];
		double len = horizontal ? sibling->box.width : sibling->box.height;
		*(horizontal ? &sibling->width_fraction : &sibling->height_fraction) =
			len / child_total;
	}
	double amount_fraction = (double)amount / child_total;
	double sibling_amount_fraction = amount_fraction / (double)(resize->length - 1);
	for (int i = 0; i < resize->length; i++) {
		struct swine_container *sibling = resize->items[i];
		double *f = horizontal ?
			&sibling->width_fraction : &sibling->height_fraction;
		*f += sibling == con ? amount_fraction : -sibling_amount_fraction;
	}
cleanup:
	list_free(resize);
}

void floating_calculate_constraints(struct swine_container *con,
		int *min_width, int *max_width, int *min_height, int *max_height) {
	*min_width = FLOATING_MIN_W;
	*min_height = FLOATING_MIN_H;
	struct swine_box area = con->workspace && con->workspace->output ?
		output_get_usable_area(con->workspace->output) :
		(struct swine_box){0, 0, 1 << 16, 1 << 16};
	*max_width = area.width;
	*max_height = area.height;
}

// resize <grow|shrink> for a floating container.
static struct cmd_results *resize_adjust_floating(unsigned axis,
		struct movement_amount *amount) {
	struct swine_container *con = config->handler_context.container;
	struct swine_box *box = &con->floating_box;
	int grow_width = 0, grow_height = 0;
	if (is_horizontal(axis)) {
		grow_width = amount->amount;
	} else {
		grow_height = amount->amount;
	}
	int min_width, max_width, min_height, max_height;
	floating_calculate_constraints(con, &min_width, &max_width,
		&min_height, &max_height);
	if (box->width + grow_width < min_width) {
		grow_width = min_width - box->width;
	} else if (box->width + grow_width > max_width) {
		grow_width = max_width - box->width;
	}
	if (box->height + grow_height < min_height) {
		grow_height = min_height - box->height;
	} else if (box->height + grow_height > max_height) {
		grow_height = max_height - box->height;
	}
	int grow_x = 0, grow_y = 0;
	if (axis == AXIS_HORIZONTAL) {
		grow_x = -grow_width / 2;
	} else if (axis == AXIS_VERTICAL) {
		grow_y = -grow_height / 2;
	} else if (axis == EDGE_TOP) {
		grow_y = -grow_height;
	} else if (axis == EDGE_LEFT) {
		grow_x = -grow_width;
	}
	if (grow_width == 0 && grow_height == 0) {
		return cmd_results_new(CMD_INVALID, "Cannot resize any further");
	}
	box->x += grow_x;
	box->y += grow_y;
	box->width += grow_width;
	box->height += grow_height;
	return cmd_results_new(CMD_SUCCESS, NULL);
}

static int ppt_base(struct swine_container *con, bool horizontal) {
	enum swine_container_layout want = horizontal ? L_HORIZ : L_VERT;
	struct swine_container *parent = con->parent;
	while (parent && parent->layout != want) {
		parent = parent->parent;
	}
	if (parent) {
		return horizontal ? parent->box.width : parent->box.height;
	}
	return horizontal ? con->workspace->box.width : con->workspace->box.height;
}

// resize <grow|shrink> for a tiled container.
static struct cmd_results *resize_adjust_tiled(unsigned axis,
		struct movement_amount *amount) {
	struct swine_container *current = config->handler_context.container;
	if (hidden_scratchpad_or_child(current)) {
		return cmd_results_new(CMD_FAILURE,
			"Cannot resize a hidden scratchpad container");
	}
	if (amount->unit == UNIT_DEFAULT) {
		amount->unit = UNIT_PPT;
	}
	if (amount->unit == UNIT_PPT) {
		amount->amount = ppt_base(current, is_horizontal(axis)) *
			amount->amount / 100;
	}
	double old_width = current->width_fraction;
	double old_height = current->height_fraction;
	container_resize_tiled(current, axis, amount->amount);
	if (current->width_fraction == old_width &&
			current->height_fraction == old_height) {
		return cmd_results_new(CMD_INVALID, "Cannot resize any further");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

// resize set for a tiled container.
static struct cmd_results *resize_set_tiled(struct swine_container *con,
		struct movement_amount *width, struct movement_amount *height) {
	if (hidden_scratchpad_or_child(con)) {
		return cmd_results_new(CMD_FAILURE,
			"Cannot resize a hidden scratchpad container");
	}
	if (width->amount) {
		if (width->unit == UNIT_PPT || width->unit == UNIT_DEFAULT) {
			width->amount = ppt_base(con, true) * width->amount / 100;
			width->unit = UNIT_PX;
		}
		container_resize_tiled(con, AXIS_HORIZONTAL,
			width->amount - con->box.width);
	}
	if (height->amount) {
		if (height->unit == UNIT_PPT || height->unit == UNIT_DEFAULT) {
			height->amount = ppt_base(con, false) * height->amount / 100;
			height->unit = UNIT_PX;
		}
		container_resize_tiled(con, AXIS_VERTICAL,
			height->amount - con->box.height);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

// resize set for a floating container: sizes are px unless "ppt".
static struct cmd_results *resize_set_floating(struct swine_container *con,
		struct movement_amount *width, struct movement_amount *height) {
	int min_width, max_width, min_height, max_height;
	floating_calculate_constraints(con, &min_width, &max_width,
		&min_height, &max_height);
	struct swine_box *box = &con->floating_box;
	if (width->amount) {
		if (width->unit == UNIT_PPT) {
			if (container_is_scratchpad_hidden(con)) {
				return cmd_results_new(CMD_FAILURE,
					"Cannot resize a hidden scratchpad container by ppt");
			}
			width->amount = con->workspace->box.width * width->amount / 100;
		}
		width->amount = (int)fmax(min_width, fmin(width->amount, max_width));
		int grow_width = width->amount - box->width;
		box->x -= grow_width / 2;
		box->width = width->amount;
	}
	if (height->amount) {
		if (height->unit == UNIT_PPT) {
			if (container_is_scratchpad_hidden(con)) {
				return cmd_results_new(CMD_FAILURE,
					"Cannot resize a hidden scratchpad container by ppt");
			}
			height->amount = con->workspace->box.height * height->amount / 100;
		}
		height->amount = (int)fmax(min_height, fmin(height->amount, max_height));
		int grow_height = height->amount - box->height;
		box->y -= grow_height / 2;
		box->height = height->amount;
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

// resize set [width] <width> [px|ppt] [height] <height> [px|ppt]
static struct cmd_results *cmd_resize_set(int argc, char **argv) {
	struct cmd_results *error;
	if ((error = checkarg(argc, "resize", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	const char usage[] = "Expected 'resize set [width] <width> [px|ppt]' or "
		"'resize set height <height> [px|ppt]' or "
		"'resize set [width] <width> [px|ppt] [height] <height> [px|ppt]'";

	struct movement_amount width = {0, UNIT_DEFAULT};
	if (argc >= 2 && !strcmp(argv[0], "width") && strcmp(argv[1], "height")) {
		argc--; argv++;
	}
	if (strcmp(argv[0], "height")) {
		int used = parse_movement_amount(argc, argv, &width);
		argc -= used;
		argv += used;
		if (width.unit == UNIT_INVALID) {
			return cmd_results_new(CMD_INVALID, "%s", usage);
		}
	}

	struct movement_amount height = {0, UNIT_DEFAULT};
	if (argc) {
		if (argc >= 2 && !strcmp(argv[0], "height")) {
			argc--; argv++;
		}
		int used = parse_movement_amount(argc, argv, &height);
		if (argc > used || height.unit == UNIT_INVALID) {
			return cmd_results_new(CMD_INVALID, "%s", usage);
		}
	}

	struct swine_container *con = config->handler_context.container;
	bool floating = container_is_floating(con);
	// 0: leave that dimension as it is
	if (width.amount <= 0) {
		width.amount = floating ? con->floating_box.width : con->box.width;
		width.unit = UNIT_PX;
	}
	if (height.amount <= 0) {
		height.amount = floating ? con->floating_box.height : con->box.height;
		height.unit = UNIT_PX;
	}
	if (floating) {
		return resize_set_floating(con, &width, &height);
	}
	return resize_set_tiled(con, &width, &height);
}

// resize <grow|shrink> <direction> [<amount> px|ppt [or <amount> px|ppt]]
static struct cmd_results *cmd_resize_adjust(int argc, char **argv,
		int multiplier) {
	const char usage[] = "Expected 'resize grow|shrink <direction> "
		"[<amount> px|ppt [or <amount> px|ppt]]'";
	unsigned axis = parse_resize_axis(*argv);
	if (axis == EDGE_NONE) {
		return cmd_results_new(CMD_INVALID, "%s", usage);
	}
	--argc; ++argv;

	struct movement_amount first_amount;
	if (argc) {
		int used = parse_movement_amount(argc, argv, &first_amount);
		argc -= used;
		argv += used;
		if (first_amount.unit == UNIT_INVALID) {
			return cmd_results_new(CMD_INVALID, "%s", usage);
		}
	} else {
		first_amount.amount = 10;
		first_amount.unit = UNIT_DEFAULT;
	}

	if (argc) {
		if (strcmp(*argv, "or") != 0) {
			return cmd_results_new(CMD_INVALID, "%s", usage);
		}
		--argc; ++argv;
	}

	struct movement_amount second_amount;
	if (argc) {
		int used = parse_movement_amount(argc, argv, &second_amount);
		if (argc > used || second_amount.unit == UNIT_INVALID) {
			return cmd_results_new(CMD_INVALID, "%s", usage);
		}
	} else {
		second_amount.amount = 0;
		second_amount.unit = UNIT_INVALID;
	}

	first_amount.amount *= multiplier;
	second_amount.amount *= multiplier;

	struct swine_container *con = config->handler_context.container;
	if (container_is_floating(con)) {
		// Floating containers resize in px: prefer an amount in px, then
		// one without a unit.
		if (first_amount.unit == UNIT_PX) {
			return resize_adjust_floating(axis, &first_amount);
		} else if (second_amount.unit == UNIT_PX) {
			return resize_adjust_floating(axis, &second_amount);
		} else if (first_amount.unit == UNIT_DEFAULT) {
			return resize_adjust_floating(axis, &first_amount);
		} else if (second_amount.unit == UNIT_DEFAULT) {
			return resize_adjust_floating(axis, &second_amount);
		}
		return cmd_results_new(CMD_INVALID,
			"Floating containers cannot use ppt measurements");
	}

	// For tiling, prefer ppt -> default -> px
	if (first_amount.unit == UNIT_PPT) {
		return resize_adjust_tiled(axis, &first_amount);
	} else if (second_amount.unit == UNIT_PPT) {
		return resize_adjust_tiled(axis, &second_amount);
	} else if (first_amount.unit == UNIT_DEFAULT) {
		return resize_adjust_tiled(axis, &first_amount);
	} else if (second_amount.unit == UNIT_DEFAULT) {
		return resize_adjust_tiled(axis, &second_amount);
	}
	return resize_adjust_tiled(axis, &first_amount);
}

struct cmd_results *cmd_resize(int argc, char **argv) {
	struct swine_container *current = config->handler_context.container;
	if (!current || !current->workspace) {
		return cmd_results_new(CMD_INVALID, "Cannot resize nothing");
	}
	struct cmd_results *error;
	if ((error = checkarg(argc, "resize", EXPECTED_AT_LEAST, 2))) {
		return error;
	}
	if (swine_strcasecmp(argv[0], "set") == 0) {
		return cmd_resize_set(argc - 1, &argv[1]);
	}
	if (swine_strcasecmp(argv[0], "grow") == 0) {
		return cmd_resize_adjust(argc - 1, &argv[1], 1);
	}
	if (swine_strcasecmp(argv[0], "shrink") == 0) {
		return cmd_resize_adjust(argc - 1, &argv[1], -1);
	}
	return cmd_results_new(CMD_INVALID, "Expected 'resize <shrink|grow> "
		"<width|height|up|down|left|right> [<amount>] [px|ppt]'");
}
