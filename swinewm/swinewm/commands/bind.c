#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/input/cursor.h"
#include "swinewm/input/keyboard.h"

// Parses "Mod4+Shift+q" into modifiers and a key code, or "Mod4+button3"
// into modifiers and a mouse button (*mouse set).
static struct cmd_results *parse_combo(const char *combo, uint32_t *modifiers,
		uint32_t *keycode, bool *mouse) {
	*modifiers = 0;
	*mouse = false;
	bool have_key = false;
	list_t *parts = split_string(combo, "+");
	struct cmd_results *error = NULL;
	for (int i = 0; i < parts->length; ++i) {
		const char *part = parts->items[i];
		uint32_t mod = get_modifier_mask_by_name(part);
		if (mod) {
			*modifiers |= mod;
			continue;
		}
		if (have_key) {
			error = cmd_results_new(CMD_INVALID,
				"Only one key may be bound in '%s'", combo);
			break;
		}
		char *button_error = NULL;
		uint32_t button = get_mouse_button(part, &button_error);
		if (button_error) {
			error = cmd_results_new(CMD_INVALID, "%s", button_error);
			free(button_error);
			break;
		}
		if (button) {
			*keycode = button;
			*mouse = true;
		} else if (!keyname_to_keycode(part, keycode)) {
			error = cmd_results_new(CMD_INVALID,
				"Unknown key '%s' in '%s'", part, combo);
			break;
		}
		have_key = true;
	}
	if (!error && !have_key) {
		error = cmd_results_new(CMD_INVALID, "No key given in '%s'", combo);
	}
	list_free_items_and_destroy(parts);
	return error;
}

static int find_binding(list_t *bindings, struct swine_binding *needle) {
	for (int i = 0; i < bindings->length; ++i) {
		struct swine_binding *b = bindings->items[i];
		uint32_t conflict_flags = BINDING_RELEASE | BINDING_BORDER |
			BINDING_CONTENTS | BINDING_TITLEBAR;
		if (b->keycode == needle->keycode && b->modifiers == needle->modifiers &&
				(b->flags & conflict_flags) == (needle->flags & conflict_flags)) {
			return i;
		}
	}
	return -1;
}

static struct cmd_results *cmd_bindsym_or_unbind(int argc, char **argv,
		bool unbind) {
	const char *name = unbind ? "unbindsym" : "bindsym";
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, name, EXPECTED_AT_LEAST, unbind ? 1 : 2))) {
		return error;
	}

	struct swine_binding *binding = calloc(1, sizeof(*binding));
	bool exclude_titlebar = false;
	while (argc > 0 && has_prefix(argv[0], "--")) {
		if (strcmp(argv[0], "--release") == 0) {
			binding->flags |= BINDING_RELEASE;
		} else if (strcmp(argv[0], "--locked") == 0) {
			binding->flags |= BINDING_LOCKED;
		} else if (strcmp(argv[0], "--no-repeat") == 0) {
			binding->flags |= BINDING_NOREPEAT;
		} else if (strcmp(argv[0], "--inhibited") == 0) {
			binding->flags |= BINDING_INHIBITED;
		} else if (strcmp(argv[0], "--whole-window") == 0) {
			binding->flags |= BINDING_BORDER | BINDING_CONTENTS | BINDING_TITLEBAR;
		} else if (strcmp(argv[0], "--border") == 0) {
			binding->flags |= BINDING_BORDER;
		} else if (strcmp(argv[0], "--exclude-titlebar") == 0) {
			exclude_titlebar = true;
		} else if (strcmp(argv[0], "--to-code") == 0 ||
				strcmp(argv[0], "--no-warn") == 0 ||
				has_prefix(argv[0], "--input-device=")) {
			// Accepted for config compatibility; no effect on Windows yet.
		} else {
			free_swine_binding(binding);
			return cmd_results_new(CMD_INVALID,
				"Invalid %s flag '%s'", name, argv[0]);
		}
		--argc;
		++argv;
	}
	if ((error = checkarg(argc, name, EXPECTED_AT_LEAST, unbind ? 1 : 2))) {
		free_swine_binding(binding);
		return error;
	}

	bool mouse = false;
	if ((error = parse_combo(argv[0], &binding->modifiers, &binding->keycode,
			&mouse))) {
		free_swine_binding(binding);
		return error;
	}
	binding->input = swine_strdup(argv[0]);
	// As in sway: a mouse binding is for the title bar unless flags say
	// otherwise; --whole-window and friends make any binding a mouse one.
	if (binding->flags & (BINDING_BORDER | BINDING_CONTENTS | BINDING_TITLEBAR)) {
		mouse = true;
	}
	if (exclude_titlebar) {
		binding->flags &= ~(uint32_t)BINDING_TITLEBAR;
	} else if (mouse) {
		binding->flags |= BINDING_TITLEBAR;
	}

	list_t *bindings = mouse ? config->current_mode->mouse_bindings :
		config->current_mode->keysym_bindings;
	int existing = find_binding(bindings, binding);
	if (unbind) {
		free_swine_binding(binding);
		if (existing == -1) {
			return cmd_results_new(CMD_FAILURE, "Could not find binding '%s'",
				argv[0]);
		}
		free_swine_binding(bindings->items[existing]);
		list_del(bindings, existing);
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	binding->command = join_args(argv + 1, argc - 1);
	binding->order = bindings->length;
	if (existing != -1) {
		struct swine_binding *old = bindings->items[existing];
		swine_log(SWINE_INFO, "Overwriting binding '%s' for mode '%s' "
			"('%s' -> '%s')", argv[0], config->current_mode->name,
			old->command, binding->command);
		free_swine_binding(old);
		bindings->items[existing] = binding;
	} else {
		list_add(bindings, binding);
	}
	swine_log(SWINE_DEBUG, "%s - Bound %s to command '%s'",
		config->current_mode->name, argv[0], binding->command);
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_bindsym(int argc, char **argv) {
	return cmd_bindsym_or_unbind(argc, argv, false);
}

struct cmd_results *cmd_unbindsym(int argc, char **argv) {
	return cmd_bindsym_or_unbind(argc, argv, true);
}
