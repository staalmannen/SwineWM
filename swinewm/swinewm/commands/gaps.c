#include <stdlib.h>
#include <string.h>
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/workspace.h"

enum gaps_op {
	GAPS_OP_SET,
	GAPS_OP_ADD,
	GAPS_OP_SUBTRACT,
	GAPS_OP_TOGGLE,
};

enum gaps_type {
	GAPS_INNER,
	GAPS_OUTER,
	GAPS_HORIZONTAL,
	GAPS_VERTICAL,
	GAPS_TOP,
	GAPS_RIGHT,
	GAPS_BOTTOM,
	GAPS_LEFT,
};

static bool parse_type(const char *s, enum gaps_type *type) {
	static const char *names[] = {
		"inner", "outer", "horizontal", "vertical",
		"top", "right", "bottom", "left",
	};
	for (int i = 0; i < 8; ++i) {
		if (swine_strcasecmp(s, names[i]) == 0) {
			*type = (enum gaps_type)i;
			return true;
		}
	}
	return false;
}

static bool parse_amount(const char *s, int *amount) {
	char *end = NULL;
	long n = strtol(s, &end, 10);
	if (end == s || (*end && strcmp(end, "px") != 0)) {
		return false;
	}
	*amount = (int)n;
	return true;
}

static int apply_op(int current, enum gaps_op op, int amount) {
	int value = current;
	switch (op) {
	case GAPS_OP_SET: value = amount; break;
	case GAPS_OP_ADD: value = current + amount; break;
	case GAPS_OP_SUBTRACT: value = current - amount; break;
	case GAPS_OP_TOGGLE: value = current ? 0 : amount; break;
	}
	return value < 0 ? 0 : value;
}

static void apply_gaps(int *inner, struct side_gaps *outer,
		enum gaps_type type, enum gaps_op op, int amount) {
	switch (type) {
	case GAPS_INNER:
		*inner = apply_op(*inner, op, amount);
		break;
	case GAPS_OUTER:
		outer->top = apply_op(outer->top, op, amount);
		outer->right = apply_op(outer->right, op, amount);
		outer->bottom = apply_op(outer->bottom, op, amount);
		outer->left = apply_op(outer->left, op, amount);
		break;
	case GAPS_HORIZONTAL:
		outer->right = apply_op(outer->right, op, amount);
		outer->left = apply_op(outer->left, op, amount);
		break;
	case GAPS_VERTICAL:
		outer->top = apply_op(outer->top, op, amount);
		outer->bottom = apply_op(outer->bottom, op, amount);
		break;
	case GAPS_TOP: outer->top = apply_op(outer->top, op, amount); break;
	case GAPS_RIGHT: outer->right = apply_op(outer->right, op, amount); break;
	case GAPS_BOTTOM: outer->bottom = apply_op(outer->bottom, op, amount); break;
	case GAPS_LEFT: outer->left = apply_op(outer->left, op, amount); break;
	}
}

struct gaps_args {
	enum gaps_type type;
	enum gaps_op op;
	int amount;
};

static void apply_to_workspace(struct swine_workspace *ws, void *data) {
	struct gaps_args *args = data;
	apply_gaps(&ws->gaps_inner, &ws->gaps_outer, args->type, args->op,
		args->amount);
}

static const char expected_syntax[] =
	"Expected 'gaps inner|outer|horizontal|vertical|top|right|bottom|left "
	"[current|all set|plus|minus|toggle] <px>'";

// gaps <type> <amount>                       (default for new workspaces)
// gaps <type> all|current set|plus|minus|toggle <amount>   (runtime)
struct cmd_results *cmd_gaps(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "gaps", EXPECTED_AT_LEAST, 2))) {
		return error;
	}
	struct gaps_args args = { .op = GAPS_OP_SET };
	if (!parse_type(argv[0], &args.type)) {
		return cmd_results_new(CMD_INVALID, expected_syntax);
	}
	if (argc == 2) {
		if (!parse_amount(argv[1], &args.amount)) {
			return cmd_results_new(CMD_INVALID, expected_syntax);
		}
		apply_gaps(&config->gaps_inner, &config->gaps_outer, args.type,
			args.op, args.amount);
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	if (argc != 4 || config->reading) {
		return cmd_results_new(CMD_INVALID, expected_syntax);
	}
	bool all;
	if (swine_strcasecmp(argv[1], "all") == 0) {
		all = true;
	} else if (swine_strcasecmp(argv[1], "current") == 0) {
		all = false;
	} else {
		return cmd_results_new(CMD_INVALID, expected_syntax);
	}
	if (swine_strcasecmp(argv[2], "set") == 0) {
		args.op = GAPS_OP_SET;
	} else if (swine_strcasecmp(argv[2], "plus") == 0) {
		args.op = GAPS_OP_ADD;
	} else if (swine_strcasecmp(argv[2], "minus") == 0) {
		args.op = GAPS_OP_SUBTRACT;
	} else if (swine_strcasecmp(argv[2], "toggle") == 0) {
		args.op = GAPS_OP_TOGGLE;
	} else {
		return cmd_results_new(CMD_INVALID, expected_syntax);
	}
	if (!parse_amount(argv[3], &args.amount)) {
		return cmd_results_new(CMD_INVALID, expected_syntax);
	}
	if (all) {
		root_for_each_workspace(apply_to_workspace, &args);
	} else if (config->handler_context.workspace) {
		apply_to_workspace(config->handler_context.workspace, &args);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}
