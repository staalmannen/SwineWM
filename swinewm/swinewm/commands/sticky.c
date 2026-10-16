// sticky, ported from sway/commands/sticky.c (MIT, Copyright (c) 2016-2017
// Drew DeVault).
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/workspace.h"

struct cmd_results *cmd_sticky(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "sticky", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	struct swine_container *container = config->handler_context.container;

	if (container == NULL) {
		return cmd_results_new(CMD_FAILURE, "No current container");
	}

	container->is_sticky = parse_boolean(argv[0], container->is_sticky);

	if (container_is_sticky_or_child(container) &&
			!container_is_scratchpad_hidden(container)) {
		// move container to active workspace
		struct swine_workspace *active_workspace =
			output_get_active_workspace(container->workspace->output);
		if (!swine_assert(active_workspace,
					"Expected output to have a workspace")) {
			return cmd_results_new(CMD_FAILURE,
				"Expected output to have a workspace");
		}
		if (container->workspace != active_workspace) {
			struct swine_workspace *old_workspace = container->workspace;
			container_detach(container);
			workspace_add_floating(active_workspace, container);
			workspace_consider_destroy(old_workspace);
		}
	}

	return cmd_results_new(CMD_SUCCESS, NULL);
}
