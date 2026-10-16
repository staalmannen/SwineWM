// assign, ported from sway/commands/assign.c (MIT).
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/criteria.h"

struct cmd_results *cmd_assign(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "assign", EXPECTED_AT_LEAST, 2))) {
		return error;
	}
	char *err_str = NULL;
	struct criteria *criteria = criteria_parse(argv[0], &err_str);
	if (!criteria) {
		error = cmd_results_new(CMD_INVALID, "%s", err_str);
		free(err_str);
		return error;
	}
	--argc; ++argv;

	if (has_prefix(*argv, "\xE2\x86\x92")) {   // "→"
		if (argc < 2) {
			criteria_destroy(criteria);
			return cmd_results_new(CMD_INVALID, "Missing workspace");
		}
		--argc; ++argv;
	}

	if (strcmp(*argv, "output") == 0) {
		criteria->type = CT_ASSIGN_OUTPUT;
		--argc; ++argv;
	} else {
		if (strcmp(*argv, "workspace") == 0) {
			--argc; ++argv;
		}
		if (argc > 0 && strcmp(*argv, "number") == 0) {
			--argc; ++argv;
			if (argc == 0 || argv[0][0] < '0' || argv[0][0] > '9') {
				criteria_destroy(criteria);
				return cmd_results_new(CMD_INVALID,
					"Invalid workspace number '%s'", argc ? argv[0] : "");
			}
			criteria->type = CT_ASSIGN_WORKSPACE_NUMBER;
		} else {
			criteria->type = CT_ASSIGN_WORKSPACE;
		}
	}
	if (argc == 0) {
		criteria_destroy(criteria);
		return cmd_results_new(CMD_INVALID, "Missing workspace or output");
	}
	criteria->target = join_args(argv, argc);
	list_add(config->criteria, criteria);
	swine_log(SWINE_DEBUG, "assign: '%s' -> '%s' added", criteria->raw,
		criteria->target);
	return cmd_results_new(CMD_SUCCESS, NULL);
}
