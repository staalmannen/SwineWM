#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/workspace.h"

static struct cmd_results *do_split(enum swine_container_layout layout) {
	struct swine_container *con = config->handler_context.container;
	struct swine_workspace *ws = config->handler_context.workspace;
	if (con) {
		if (container_is_scratchpad_hidden(con)) {
			return cmd_results_new(CMD_FAILURE,
				"Cannot split a hidden scratchpad container");
		}
		container_split(con, layout);
	} else if (ws) {
		workspace_split(ws, layout);
	} else {
		return cmd_results_new(CMD_FAILURE, "Nothing to split");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

static struct cmd_results *do_unsplit(void) {
	struct swine_container *con = config->handler_context.container;
	if (con && con->parent && con->parent->children->length == 1) {
		container_flatten(con->parent);
	} else {
		return cmd_results_new(CMD_FAILURE,
			"Can only flatten a child container with no siblings");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_split(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "split", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	if (swine_strcasecmp(argv[0], "v") == 0 ||
			swine_strcasecmp(argv[0], "vertical") == 0) {
		return do_split(L_VERT);
	} else if (swine_strcasecmp(argv[0], "h") == 0 ||
			swine_strcasecmp(argv[0], "horizontal") == 0) {
		return do_split(L_HORIZ);
	} else if (swine_strcasecmp(argv[0], "t") == 0 ||
			swine_strcasecmp(argv[0], "toggle") == 0) {
		struct swine_container *focused = config->handler_context.container;
		if (focused && container_parent_layout(focused) == L_VERT) {
			return do_split(L_HORIZ);
		}
		return do_split(L_VERT);
	} else if (swine_strcasecmp(argv[0], "n") == 0 ||
			swine_strcasecmp(argv[0], "none") == 0) {
		return do_unsplit();
	}
	return cmd_results_new(CMD_FAILURE,
		"Invalid split command (expected either horizontal or vertical).");
}

struct cmd_results *cmd_splitv(int argc, char **argv) {
	(void)argv;
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "splitv", EXPECTED_EQUAL_TO, 0))) {
		return error;
	}
	return do_split(L_VERT);
}

struct cmd_results *cmd_splith(int argc, char **argv) {
	(void)argv;
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "splith", EXPECTED_EQUAL_TO, 0))) {
		return error;
	}
	return do_split(L_HORIZ);
}

struct cmd_results *cmd_splitt(int argc, char **argv) {
	(void)argv;
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "splitt", EXPECTED_EQUAL_TO, 0))) {
		return error;
	}
	struct swine_container *con = config->handler_context.container;
	if (con && container_parent_layout(con) == L_VERT) {
		return do_split(L_HORIZ);
	}
	return do_split(L_VERT);
}
