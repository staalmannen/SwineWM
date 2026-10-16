// for_window, ported from sway/commands/for_window.c (MIT).
#include <stdlib.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/criteria.h"

struct cmd_results *cmd_for_window(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "for_window", EXPECTED_AT_LEAST, 2))) {
		return error;
	}
	char *err_str = NULL;
	struct criteria *criteria = criteria_parse(argv[0], &err_str);
	if (!criteria) {
		error = cmd_results_new(CMD_INVALID, "%s", err_str);
		free(err_str);
		return error;
	}
	criteria->type = CT_COMMAND;
	criteria->cmdlist = join_args(argv + 1, argc - 1);
	if (criteria_already_exists(criteria)) {
		swine_log(SWINE_DEBUG, "for_window already exists: '%s' -> '%s'",
			criteria->raw, criteria->cmdlist);
		criteria_destroy(criteria);
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	list_add(config->criteria, criteria);
	swine_log(SWINE_DEBUG, "for_window: '%s' -> '%s' added", criteria->raw,
		criteria->cmdlist);
	return cmd_results_new(CMD_SUCCESS, NULL);
}
