#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/platform.h"

struct cmd_results *cmd_exec_always(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "exec_always", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	if (!config->active || config->validating) {
		return cmd_results_new(CMD_DEFER, NULL);
	}
	if (strcmp(argv[0], "--no-startup-id") == 0) {
		--argc;
		++argv;
		if ((error = checkarg(argc, "exec_always", EXPECTED_AT_LEAST, 1))) {
			return error;
		}
	}
	char *cmd;
	if (argc == 1 && (argv[0][0] == '\'' || argv[0][0] == '"')) {
		cmd = swine_strdup(argv[0]);
		strip_quotes(cmd);
	} else {
		cmd = join_args(argv, argc);
	}
	struct cmd_results *builtin = exec_builtin(cmd, false);
	if (builtin) {
		free(cmd);
		return builtin;
	}
	swine_log(SWINE_INFO, "Executing %s", cmd);
	bool ok = platform_spawn(cmd);
	struct cmd_results *res = ok ? cmd_results_new(CMD_SUCCESS, NULL) :
		cmd_results_new(CMD_FAILURE, "Unable to start '%s'", cmd);
	free(cmd);
	return res;
}

struct cmd_results *cmd_exec(int argc, char **argv) {
	if (!config->active) {
		return cmd_results_new(CMD_DEFER, NULL);
	}
	if (config->reloading) {
		char *args = join_args(argv, argc);
		// "exec waybar" still means "show the bar" after a reload.
		struct cmd_results *builtin = exec_builtin(args, true);
		if (builtin) {
			free(args);
			return builtin;
		}
		swine_log(SWINE_DEBUG, "Ignoring 'exec %s' due to reload", args);
		free(args);
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	return cmd_exec_always(argc, argv);
}
