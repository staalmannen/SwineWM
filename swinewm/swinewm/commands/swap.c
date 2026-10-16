// swap, ported from sway/commands/swap.c (MIT, Copyright (c) 2016-2017
// Drew DeVault).
#include <stdlib.h>
#include <string.h>
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/view.h"

static const char expected_syntax[] =
	"Expected 'swap container with id|con_id|mark <arg>'";

static bool test_con_id(struct swine_container *container, void *data) {
	size_t *con_id = data;
	return container->node.id == *con_id;
}

// "id" is the X11 window id in sway; here it is the native window handle,
// as in the criteria.
static bool test_id(struct swine_container *container, void *data) {
	unsigned long long *wid = data;
	return container->view &&
		(unsigned long long)(size_t)container->view->handle == *wid;
}

static bool test_mark(struct swine_container *container, void *mark) {
	return container_has_mark(container, mark);
}

struct cmd_results *cmd_swap(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "swap", EXPECTED_AT_LEAST, 4))) {
		return error;
	}
	if (!root->outputs->length) {
		return cmd_results_new(CMD_INVALID,
			"Can't run this command while there's no outputs connected.");
	}

	if (swine_strcasecmp(argv[0], "container") ||
			swine_strcasecmp(argv[1], "with")) {
		return cmd_results_new(CMD_INVALID, "%s", expected_syntax);
	}

	struct swine_container *current = config->handler_context.container;
	struct swine_container *other = NULL;

	char *value = join_args(argv + 3, argc - 3);
	if (swine_strcasecmp(argv[2], "id") == 0) {
		unsigned long long id = strtoull(value, NULL, 0);
		other = root_find_container(test_id, &id);
	} else if (swine_strcasecmp(argv[2], "con_id") == 0) {
		size_t con_id = (size_t)strtoull(value, NULL, 10);
		other = root_find_container(test_con_id, &con_id);
	} else if (swine_strcasecmp(argv[2], "mark") == 0) {
		other = root_find_container(test_mark, value);
	} else {
		free(value);
		return cmd_results_new(CMD_INVALID, "%s", expected_syntax);
	}

	if (!other) {
		error = cmd_results_new(CMD_FAILURE,
			"Failed to find %s '%s'", argv[2], value);
	} else if (!current) {
		error = cmd_results_new(CMD_FAILURE,
			"Can only swap with containers and views");
	} else if (current == other) {
		error = cmd_results_new(CMD_FAILURE,
			"Cannot swap a container with itself");
	} else if (container_has_ancestor(current, other)
			|| container_has_ancestor(other, current)) {
		error = cmd_results_new(CMD_FAILURE,
			"Cannot swap ancestor and descendant");
	}

	free(value);

	if (error) {
		return error;
	}

	container_swap(current, other);
	return cmd_results_new(CMD_SUCCESS, NULL);
}
