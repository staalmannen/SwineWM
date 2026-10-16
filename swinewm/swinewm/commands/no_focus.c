// no_focus, ported from sway/commands/no_focus.c (MIT).
#include <stdlib.h>
#include "log.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/criteria.h"

struct cmd_results *cmd_no_focus(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "no_focus", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	char *err_str = NULL;
	struct criteria *criteria = criteria_parse(argv[0], &err_str);
	if (!criteria) {
		error = cmd_results_new(CMD_INVALID, "%s", err_str);
		free(err_str);
		return error;
	}
	criteria->type = CT_NO_FOCUS;
	if (criteria_already_exists(criteria)) {
		swine_log(SWINE_DEBUG, "no_focus already exists: '%s'", criteria->raw);
		criteria_destroy(criteria);
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	list_add(config->criteria, criteria);
	return cmd_results_new(CMD_SUCCESS, NULL);
}
