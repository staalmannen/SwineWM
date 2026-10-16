#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/input/seat.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/workspace.h"

static struct swine_workspace *workspace_auto_back_and_forth(
		struct swine_workspace *ws) {
	struct swine_workspace *active = seat_get_focused_workspace();
	if (config->auto_back_and_forth && active == ws &&
			seat->prev_workspace_name) {
		struct swine_workspace *prev = workspace_by_name(seat->prev_workspace_name);
		return prev ? prev : workspace_create(NULL, seat->prev_workspace_name);
	}
	return ws;
}

static struct workspace_config *workspace_config_find_or_create(
		const char *ws_name) {
	struct workspace_config *wsc = workspace_find_config(ws_name);
	if (wsc) {
		return wsc;
	}
	wsc = calloc(1, sizeof(*wsc));
	wsc->workspace = swine_strdup(ws_name);
	wsc->outputs = create_list();
	wsc->gaps_inner = INT_MIN;
	wsc->gaps_outer.top = INT_MIN;
	wsc->gaps_outer.right = INT_MIN;
	wsc->gaps_outer.bottom = INT_MIN;
	wsc->gaps_outer.left = INT_MIN;
	list_add(config->workspace_configs, wsc);
	return wsc;
}

void free_workspace_config(struct workspace_config *wsc) {
	free(wsc->workspace);
	list_free_items_and_destroy(wsc->outputs);
	free(wsc);
}

static void prevent_invalid_outer_gaps(struct workspace_config *wsc) {
	int *sides[] = {
		&wsc->gaps_outer.top, &wsc->gaps_outer.right,
		&wsc->gaps_outer.bottom, &wsc->gaps_outer.left,
	};
	for (size_t i = 0; i < sizeof(sides) / sizeof(sides[0]); ++i) {
		if (*sides[i] != INT_MIN && wsc->gaps_inner != INT_MIN &&
				*sides[i] < -wsc->gaps_inner) {
			*sides[i] = -wsc->gaps_inner;
		}
	}
}

static struct cmd_results *cmd_workspace_gaps(int argc, char **argv,
		int gaps_location) {
	const char expected[] = "Expected 'workspace <name> gaps "
		"inner|outer|horizontal|vertical|top|right|bottom|left <px>'";
	if (gaps_location == 0) {
		return cmd_results_new(CMD_INVALID, "%s", expected);
	}
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "workspace", EXPECTED_EQUAL_TO,
					gaps_location + 3))) {
		return error;
	}
	char *ws_name = join_args(argv, argc - 3);
	struct workspace_config *wsc = workspace_config_find_or_create(ws_name);
	free(ws_name);

	char *end;
	int amount = (int)strtol(argv[gaps_location + 2], &end, 10);
	if (strlen(end)) {
		return cmd_results_new(CMD_FAILURE, "%s", expected);
	}

	bool valid = false;
	char *type = argv[gaps_location + 1];
	if (!swine_strcasecmp(type, "inner")) {
		valid = true;
		wsc->gaps_inner = (amount >= 0) ? amount : 0;
	} else {
		if (!swine_strcasecmp(type, "outer") ||
				!swine_strcasecmp(type, "vertical") ||
				!swine_strcasecmp(type, "top")) {
			valid = true;
			wsc->gaps_outer.top = amount;
		}
		if (!swine_strcasecmp(type, "outer") ||
				!swine_strcasecmp(type, "horizontal") ||
				!swine_strcasecmp(type, "right")) {
			valid = true;
			wsc->gaps_outer.right = amount;
		}
		if (!swine_strcasecmp(type, "outer") ||
				!swine_strcasecmp(type, "vertical") ||
				!swine_strcasecmp(type, "bottom")) {
			valid = true;
			wsc->gaps_outer.bottom = amount;
		}
		if (!swine_strcasecmp(type, "outer") ||
				!swine_strcasecmp(type, "horizontal") ||
				!swine_strcasecmp(type, "left")) {
			valid = true;
			wsc->gaps_outer.left = amount;
		}
	}
	if (!valid) {
		return cmd_results_new(CMD_INVALID, "%s", expected);
	}

	// Prevent invalid gaps configurations.
	if (wsc->gaps_inner != INT_MIN && wsc->gaps_inner < 0) {
		wsc->gaps_inner = 0;
	}
	prevent_invalid_outer_gaps(wsc);
	return NULL;
}

struct cmd_results *cmd_workspace(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "workspace", EXPECTED_AT_LEAST, 1))) {
		return error;
	}

	int output_location = -1;
	int gaps_location = -1;
	for (int i = 0; i < argc; ++i) {
		if (swine_strcasecmp(argv[i], "output") == 0) {
			output_location = i;
			break;
		}
	}
	for (int i = 0; i < argc; ++i) {
		if (swine_strcasecmp(argv[i], "gaps") == 0) {
			gaps_location = i;
			break;
		}
	}
	if (output_location == 0) {
		return cmd_results_new(CMD_INVALID,
			"Expected 'workspace <name> output <output>'");
	} else if (output_location > 0) {
		if ((error = checkarg(argc, "workspace", EXPECTED_AT_LEAST,
						output_location + 2))) {
			return error;
		}
		char *ws_name = join_args(argv, output_location);
		struct workspace_config *wsc = workspace_config_find_or_create(ws_name);
		free(ws_name);
		for (int i = output_location + 1; i < argc; ++i) {
			list_add(wsc->outputs, swine_strdup(argv[i]));
		}
		return cmd_results_new(CMD_SUCCESS, NULL);
	} else if (gaps_location >= 0) {
		if ((error = cmd_workspace_gaps(argc, argv, gaps_location))) {
			return error;
		}
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	if (config->reading || !config->active) {
		return cmd_results_new(CMD_DEFER, NULL);
	} else if (!root->outputs->length) {
		return cmd_results_new(CMD_INVALID,
			"Can't run this command while there's no outputs connected.");
	}

	bool auto_back_and_forth = true;
	while (swine_strcasecmp(argv[0], "--no-auto-back-and-forth") == 0) {
		auto_back_and_forth = false;
		if ((error = checkarg(--argc, "workspace", EXPECTED_AT_LEAST, 1))) {
			return error;
		}
		++argv;
	}

	struct swine_workspace *ws = NULL;
	if (swine_strcasecmp(argv[0], "number") == 0) {
		if (argc < 2) {
			return cmd_results_new(CMD_INVALID, "Expected workspace number");
		}
		if (!isdigit((unsigned char)argv[1][0])) {
			return cmd_results_new(CMD_INVALID,
				"Invalid workspace number '%s'", argv[1]);
		}
		if (!(ws = workspace_by_number(argv[1]))) {
			char *name = join_args(argv + 1, argc - 1);
			ws = workspace_create(NULL, name);
			free(name);
		}
		if (ws && auto_back_and_forth) {
			ws = workspace_auto_back_and_forth(ws);
		}
	} else if (swine_strcasecmp(argv[0], "next") == 0 ||
			swine_strcasecmp(argv[0], "prev") == 0 ||
			swine_strcasecmp(argv[0], "next_on_output") == 0 ||
			swine_strcasecmp(argv[0], "prev_on_output") == 0 ||
			swine_strcasecmp(argv[0], "current") == 0) {
		ws = workspace_by_name(argv[0]);
	} else if (swine_strcasecmp(argv[0], "back_and_forth") == 0) {
		if (!seat->prev_workspace_name) {
			return cmd_results_new(CMD_INVALID, "There is no previous workspace");
		}
		if (!(ws = workspace_by_name(argv[0]))) {
			ws = workspace_create(NULL, seat->prev_workspace_name);
		}
	} else {
		char *name = join_args(argv, argc);
		if (!(ws = workspace_by_name(name))) {
			ws = workspace_create(NULL, name);
		}
		free(name);
		if (ws && auto_back_and_forth) {
			ws = workspace_auto_back_and_forth(ws);
		}
	}
	if (!ws) {
		return cmd_results_new(CMD_FAILURE, "No workspace to switch to");
	}
	workspace_switch(ws);
	return cmd_results_new(CMD_SUCCESS, NULL);
}
