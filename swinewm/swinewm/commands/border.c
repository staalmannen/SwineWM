// Decoration commands, ported from sway/commands/border.c, default_border.c,
// default_floating_border.c, client.c, smart_borders.c, hide_edge_borders.c,
// opacity.c, font.c, titlebar_padding.c, titlebar_border_thickness.c and
// title_align.c (Copyright (c) 2016-2017 Drew DeVault, MIT).
//
// How they look on Windows is decided in tree/view.c (view_update_decoration)
// and the platform: the border is the window's own DWM border in the
// client.* child_border colour, "normal" keeps the window's own title bar.
#include <stdlib.h>
#include <string.h>
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/tree/container.h"

static void border_toggle(struct swine_container *con) {
	switch (con->border) {
	case B_NONE:
		con->border = B_PIXEL;
		break;
	case B_PIXEL:
		con->border = B_NORMAL;
		break;
	default:
		con->border = B_NONE;
		break;
	}
}

struct cmd_results *cmd_border(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "border", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	struct swine_container *container = config->handler_context.container;
	if (!container || !container->view) {
		return cmd_results_new(CMD_INVALID, "Only views can have borders");
	}
	if (strcmp(argv[0], "none") == 0) {
		container->border = B_NONE;
	} else if (strcmp(argv[0], "normal") == 0) {
		container->border = B_NORMAL;
	} else if (strcmp(argv[0], "pixel") == 0) {
		container->border = B_PIXEL;
	} else if (strcmp(argv[0], "csd") == 0) {
		return cmd_results_new(CMD_INVALID,
			"This window doesn't support client side decorations");
	} else if (strcmp(argv[0], "toggle") == 0) {
		border_toggle(container);
	} else {
		return cmd_results_new(CMD_INVALID,
			"Expected 'border <none|normal|pixel|csd|toggle>' "
			"or 'border pixel <px>'");
	}
	if (argc == 2) {
		container->border_thickness = atoi(argv[1]);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

static struct cmd_results *set_default_border(int argc, char **argv,
		const char *name, enum swine_container_border *border, int *thickness) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, name, EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	if (strcmp(argv[0], "none") == 0) {
		*border = B_NONE;
	} else if (strcmp(argv[0], "normal") == 0) {
		*border = B_NORMAL;
	} else if (strcmp(argv[0], "pixel") == 0) {
		*border = B_PIXEL;
	} else {
		return cmd_results_new(CMD_INVALID, "Expected '%s <none|normal|pixel>' "
			"or '%s <normal|pixel> <px>'", name, name);
	}
	if (argc == 2) {
		*thickness = atoi(argv[1]);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_default_border(int argc, char **argv) {
	return set_default_border(argc, argv, "default_border", &config->border,
		&config->border_thickness);
}

struct cmd_results *cmd_default_floating_border(int argc, char **argv) {
	return set_default_border(argc, argv, "default_floating_border",
		&config->floating_border, &config->floating_border_thickness);
}

static struct cmd_results *handle_client(int argc, char **argv,
		const char *cmd_name, struct border_colors *class,
		const char *default_indicator) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, cmd_name, EXPECTED_AT_LEAST, 3)) ||
			(error = checkarg(argc, cmd_name, EXPECTED_AT_MOST, 5))) {
		return error;
	}
	struct border_colors colors = {0};
	const char *ind_hex = argc > 3 ? argv[3] : default_indicator;
	const char *child_hex = argc > 4 ? argv[4] : argv[1];  // the background
	struct {
		const char *name;
		const char *hex;
		uint32_t *rgba;
	} properties[] = {
		{ "border", argv[0], &colors.border },
		{ "background", argv[1], &colors.background },
		{ "text", argv[2], &colors.text },
		{ "indicator", ind_hex, &colors.indicator },
		{ "child_border", child_hex, &colors.child_border },
	};
	for (size_t i = 0; i < sizeof(properties) / sizeof(properties[0]); i++) {
		if (!parse_color(properties[i].hex, properties[i].rgba)) {
			return cmd_results_new(CMD_INVALID, "Invalid %s color %s",
				properties[i].name, properties[i].hex);
		}
	}
	*class = colors;
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_client_focused(int argc, char **argv) {
	return handle_client(argc, argv, "client.focused",
		&config->border_colors.focused, "#2e9ef4ff");
}

struct cmd_results *cmd_client_focused_inactive(int argc, char **argv) {
	return handle_client(argc, argv, "client.focused_inactive",
		&config->border_colors.focused_inactive, "#484e50ff");
}

struct cmd_results *cmd_client_unfocused(int argc, char **argv) {
	return handle_client(argc, argv, "client.unfocused",
		&config->border_colors.unfocused, "#292d2eff");
}

struct cmd_results *cmd_client_urgent(int argc, char **argv) {
	return handle_client(argc, argv, "client.urgent",
		&config->border_colors.urgent, "#900000ff");
}

struct cmd_results *cmd_client_focused_tab_title(int argc, char **argv) {
	struct cmd_results *result = handle_client(argc, argv,
		"client.focused_tab_title", &config->border_colors.focused_tab_title,
		"#2e9ef4ff");
	if (result && result->status == CMD_SUCCESS) {
		config->has_focused_tab_title = true;
	}
	return result;
}

struct cmd_results *cmd_smart_borders(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "smart_borders", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	if (strcmp(argv[0], "no_gaps") == 0) {
		config->hide_edge_borders_smart = ESMART_NO_GAPS;
	} else {
		config->hide_edge_borders_smart = parse_boolean(argv[0], true) ?
			ESMART_ON : ESMART_OFF;
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_hide_edge_borders(int argc, char **argv) {
	const char *expected_syntax = "Expected 'hide_edge_borders [--i3] "
		"none|vertical|horizontal|both|smart|smart_no_gaps";
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "hide_edge_borders", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	bool hide_lone_tab = false;
	if (strcmp(*argv, "--i3") == 0) {
		hide_lone_tab = true;
		++argv;
		--argc;
	}
	if (!argc) {
		return cmd_results_new(CMD_INVALID, "%s", expected_syntax);
	}
	if (strcmp(argv[0], "none") == 0) {
		config->hide_edge_borders = E_NONE;
	} else if (strcmp(argv[0], "vertical") == 0) {
		config->hide_edge_borders = E_VERTICAL;
	} else if (strcmp(argv[0], "horizontal") == 0) {
		config->hide_edge_borders = E_HORIZONTAL;
	} else if (strcmp(argv[0], "both") == 0) {
		config->hide_edge_borders = E_BOTH;
	} else if (strcmp(argv[0], "smart") == 0) {
		config->hide_edge_borders = E_NONE;
		config->hide_edge_borders_smart = ESMART_ON;
	} else if (strcmp(argv[0], "smart_no_gaps") == 0) {
		config->hide_edge_borders = E_NONE;
		config->hide_edge_borders_smart = ESMART_NO_GAPS;
	} else {
		return cmd_results_new(CMD_INVALID, "%s", expected_syntax);
	}
	config->hide_lone_tab = hide_lone_tab;
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_opacity(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "opacity", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	struct swine_container *con = config->handler_context.container;
	if (con == NULL) {
		return cmd_results_new(CMD_FAILURE, "No current container");
	}
	char *err;
	float val = strtof(argc == 1 ? argv[0] : argv[1], &err);
	if (*err) {
		return cmd_results_new(CMD_INVALID, "opacity float invalid");
	}
	if (swine_strcasecmp(argv[0], "plus") == 0) {
		val = con->alpha + val;
	} else if (swine_strcasecmp(argv[0], "minus") == 0) {
		val = con->alpha - val;
	} else if (argc > 1 && swine_strcasecmp(argv[0], "set") != 0) {
		return cmd_results_new(CMD_INVALID,
			"Expected: set|plus|minus <0..1>: %s", argv[0]);
	}
	if (val < 0 || val > 1) {
		return cmd_results_new(CMD_FAILURE, "opacity value out of bounds");
	}
	con->alpha = val;
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_font(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "font", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	char *font = join_args(argv, argc);
	free(config->font);
	config->font = font;
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_titlebar_border_thickness(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "titlebar_border_thickness",
			EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	char *inv;
	long value = strtol(argv[0], &inv, 10);
	if (*inv != '\0' || value < 0 || value > config->titlebar_v_padding) {
		return cmd_results_new(CMD_FAILURE, "Invalid size specified");
	}
	config->titlebar_border_thickness = (int)value;
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_titlebar_padding(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "titlebar_padding", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	char *inv;
	long h_value = strtol(argv[0], &inv, 10);
	if (*inv != '\0' || h_value < 0 || h_value < config->titlebar_border_thickness) {
		return cmd_results_new(CMD_FAILURE, "Invalid size specified");
	}
	long v_value;
	if (argc == 1) {
		v_value = h_value;
	} else {
		v_value = strtol(argv[1], &inv, 10);
		if (*inv != '\0' || v_value < 0 ||
				v_value < config->titlebar_border_thickness) {
			return cmd_results_new(CMD_FAILURE, "Invalid size specified");
		}
	}
	config->titlebar_v_padding = (int)v_value;
	config->titlebar_h_padding = (int)h_value;
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_title_align(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "title_align", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	if (strcmp(argv[0], "left") == 0) {
		config->title_align = 0;
	} else if (strcmp(argv[0], "center") == 0) {
		config->title_align = 1;
	} else if (strcmp(argv[0], "right") == 0) {
		config->title_align = 2;
	} else {
		return cmd_results_new(CMD_INVALID,
			"Expected 'title_align left|center|right'");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}
