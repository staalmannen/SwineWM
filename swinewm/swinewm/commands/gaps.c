#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/workspace.h"

#define MAX_GAP (INT_MAX / 4)

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
	errno = 0;
	long n = strtol(s, &end, 10);
	if (errno == ERANGE || end == s || (*end && strcmp(end, "px") != 0) ||
			n < -MAX_GAP || n > MAX_GAP) {
		return false;
	}
	*amount = (int)n;
	return true;
}

static bool apply_op(int current, enum gaps_op op, int amount, int *result) {
	long long value = current;
	switch (op) {
	case GAPS_OP_SET: value = amount; break;
	case GAPS_OP_ADD: value = (long long)current + amount; break;
	case GAPS_OP_SUBTRACT: value = (long long)current - amount; break;
	case GAPS_OP_TOGGLE: value = current ? 0 : amount; break;
	}
	if (value < 0) {
		value = 0;
	}
	if (value > MAX_GAP) {
		return false;
	}
	*result = (int)value;
	return true;
}

static bool apply_gaps(int *inner, struct side_gaps *outer,
		enum gaps_type type, enum gaps_op op, int amount) {
	int new_inner = *inner;
	struct side_gaps new_outer = *outer;
	switch (type) {
	case GAPS_INNER:
		if (!apply_op(new_inner, op, amount, &new_inner)) {
			return false;
		}
		break;
	case GAPS_OUTER:
		if (!apply_op(new_outer.top, op, amount, &new_outer.top) ||
				!apply_op(new_outer.right, op, amount, &new_outer.right) ||
				!apply_op(new_outer.bottom, op, amount, &new_outer.bottom) ||
				!apply_op(new_outer.left, op, amount, &new_outer.left)) {
			return false;
		}
		break;
	case GAPS_HORIZONTAL:
		if (!apply_op(new_outer.right, op, amount, &new_outer.right) ||
				!apply_op(new_outer.left, op, amount, &new_outer.left)) {
			return false;
		}
		break;
	case GAPS_VERTICAL:
		if (!apply_op(new_outer.top, op, amount, &new_outer.top) ||
				!apply_op(new_outer.bottom, op, amount, &new_outer.bottom)) {
			return false;
		}
		break;
	case GAPS_TOP:
		if (!apply_op(new_outer.top, op, amount, &new_outer.top)) {
			return false;
		}
		break;
	case GAPS_RIGHT:
		if (!apply_op(new_outer.right, op, amount, &new_outer.right)) {
			return false;
		}
		break;
	case GAPS_BOTTOM:
		if (!apply_op(new_outer.bottom, op, amount, &new_outer.bottom)) {
			return false;
		}
		break;
	case GAPS_LEFT:
		if (!apply_op(new_outer.left, op, amount, &new_outer.left)) {
			return false;
		}
		break;
	}
	*inner = new_inner;
	*outer = new_outer;
	return true;
}

struct gaps_args {
	enum gaps_type type;
	enum gaps_op op;
	int amount;
	bool validate_only;
	bool valid;
};

static void apply_to_workspace(struct swine_workspace *ws, void *data) {
	struct gaps_args *args = data;
	int inner = ws->gaps_inner;
	struct side_gaps outer = ws->gaps_outer;
	if (!apply_gaps(&inner, &outer, args->type, args->op, args->amount)) {
		args->valid = false;
		return;
	}
	if (!args->validate_only) {
		ws->gaps_inner = inner;
		ws->gaps_outer = outer;
	}
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
		if (!apply_gaps(&config->gaps_inner, &config->gaps_outer,
				args.type, args.op, args.amount)) {
			return cmd_results_new(CMD_INVALID, "Gap amount exceeds the supported range");
		}
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
		args.valid = true;
		args.validate_only = true;
		root_for_each_workspace(apply_to_workspace, &args);
		if (!args.valid) {
			return cmd_results_new(CMD_INVALID,
				"Gap operation exceeds the supported range");
		}
		args.validate_only = false;
		root_for_each_workspace(apply_to_workspace, &args);
	} else if (config->handler_context.workspace) {
		args.valid = true;
		apply_to_workspace(config->handler_context.workspace, &args);
		if (!args.valid) {
			return cmd_results_new(CMD_INVALID,
				"Gap operation exceeds the supported range");
		}
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}
