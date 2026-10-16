// mark and unmark, ported from sway/commands/mark.c and unmark.c (MIT,
// Copyright (c) 2016-2017 Drew DeVault).
#include <stdlib.h>
#include <string.h>
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/view.h"

// mark foo                      Same as mark --replace foo
// mark --add foo                Add this mark to view's list
// mark --replace foo            Replace view's marks with this single one
// mark --add --toggle foo       Toggle current mark and persist other marks
// mark --replace --toggle foo   Toggle current mark and remove other marks

struct cmd_results *cmd_mark(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "mark", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	struct swine_container *container = config->handler_context.container;
	if (!container) {
		return cmd_results_new(CMD_INVALID, "Only containers can have marks");
	}

	bool add = false, toggle = false;
	while (argc > 0 && has_prefix(*argv, "--")) {
		if (strcmp(*argv, "--add") == 0) {
			add = true;
		} else if (strcmp(*argv, "--replace") == 0) {
			add = false;
		} else if (strcmp(*argv, "--toggle") == 0) {
			toggle = true;
		} else {
			return cmd_results_new(CMD_INVALID,
				"Unrecognized argument '%s'", *argv);
		}
		++argv;
		--argc;
	}

	if (!argc) {
		return cmd_results_new(CMD_INVALID,
			"Expected '[--add|--replace] [--toggle] <identifier>'");
	}

	char *mark = join_args(argv, argc);
	bool had_mark = container_has_mark(container, mark);

	if (!add) {
		// Replacing
		container_clear_marks(container);
	}

	container_find_and_unmark(mark);

	if (!toggle || !had_mark) {
		container_add_mark(container, mark);
	}

	free(mark);
	if (container->view) {
		view_execute_criteria(container->view);
	}

	return cmd_results_new(CMD_SUCCESS, NULL);
}

static void remove_all_marks_iterator(struct swine_container *con,
		void *data) {
	(void)data;
	container_clear_marks(con);
}

// unmark                  Remove all marks from all views
// unmark foo              Remove single mark from whichever view has it
// [criteria] unmark       Remove all marks from matched view
// [criteria] unmark foo   Remove single mark from matched view

struct cmd_results *cmd_unmark(int argc, char **argv) {
	// Determine the container
	struct swine_container *con = NULL;
	if (config->handler_context.node_overridden) {
		con = config->handler_context.container;
	}

	// Determine the mark
	char *mark = NULL;
	if (argc > 0) {
		mark = join_args(argv, argc);
	}

	if (con && mark) {
		// Remove the mark from the given container
		if (container_has_mark(con, mark)) {
			container_find_and_unmark(mark);
		}
	} else if (con && !mark) {
		// Clear all marks from the given container
		container_clear_marks(con);
	} else if (!con && mark) {
		// Remove mark from whichever container has it
		container_find_and_unmark(mark);
	} else {
		// Remove all marks from all containers
		root_for_each_container(remove_all_marks_iterator, NULL);
	}
	free(mark);

	return cmd_results_new(CMD_SUCCESS, NULL);
}
