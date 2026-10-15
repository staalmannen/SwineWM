#include <ctype.h>
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

struct cmd_results *cmd_workspace(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "workspace", EXPECTED_AT_LEAST, 1))) {
		return error;
	}

	for (int i = 0; i < argc; ++i) {
		if (swine_strcasecmp(argv[i], "output") == 0 ||
				swine_strcasecmp(argv[i], "gaps") == 0) {
			if (i == 0) {
				return cmd_results_new(CMD_INVALID,
					"Expected 'workspace <name> output|gaps ...'");
			}
			// Workspace-to-output assignment and per-workspace gaps are
			// accepted but not implemented yet.
			config->ignored_count++;
			swine_log(SWINE_INFO, "'workspace ... %s' is not implemented yet; "
				"ignored", argv[i]);
			return cmd_results_new(CMD_SUCCESS, NULL);
		}
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
