#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"

// Longest names first, so that $mod_shift is never replaced as $mod + _shift.
static void insert_sorted(list_t *symbols, struct swine_variable *var) {
	size_t len = strlen(var->name);
	int i = 0;
	for (; i < symbols->length; ++i) {
		struct swine_variable *other = symbols->items[i];
		if (strlen(other->name) < len) {
			break;
		}
	}
	list_insert(symbols, i, var);
}

struct cmd_results *cmd_set(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "set", EXPECTED_AT_LEAST, 2))) {
		return error;
	}
	char *name;
	if (argv[0][0] != '$') {
		swine_log(SWINE_INFO, "Warning: variable '%s' doesn't start with $",
			argv[0]);
		name = format_str("$%s", argv[0]);
	} else {
		name = swine_strdup(argv[0]);
	}

	for (int i = 0; i < config->symbols->length; ++i) {
		struct swine_variable *var = config->symbols->items[i];
		if (strcmp(var->name, name) == 0) {
			list_del(config->symbols, i);
			free(var->name);
			free(var->value);
			free(var);
			break;
		}
	}
	struct swine_variable *var = calloc(1, sizeof(*var));
	var->name = name;
	var->value = join_args(argv + 1, argc - 1);
	insert_sorted(config->symbols, var);
	return cmd_results_new(CMD_SUCCESS, NULL);
}
