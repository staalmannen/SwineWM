#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/criteria.h"
#include "swinewm/input/seat.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/workspace.h"

// Must stay sorted (case-insensitively) for bsearch. Commands that make no
// sense on Windows, or are not implemented yet but are common in sway
// configs, map to cmd_ignored so existing configs load cleanly; see
// doc/compatibility.md.
static const struct cmd_handler handlers[] = {
	{ "allow_tearing", cmd_ignored, 0 },
	{ "assign", cmd_assign, CMD_CONFIG_ONLY },
	{ "bar", cmd_bar, 0 },
	{ "bindcode", cmd_ignored, 0 },
	{ "bindgesture", cmd_ignored, 0 },
	{ "bindswitch", cmd_ignored, 0 },
	{ "bindsym", cmd_bindsym, 0 },
	{ "border", cmd_border, 0 },
	{ "client.background", cmd_ignored, 0 },
	{ "client.focused", cmd_client_focused, 0 },
	{ "client.focused_inactive", cmd_client_focused_inactive, 0 },
	{ "client.focused_tab_title", cmd_client_focused_tab_title, 0 },
	{ "client.placeholder", cmd_ignored, 0 },
	{ "client.unfocused", cmd_client_unfocused, 0 },
	{ "client.urgent", cmd_client_urgent, 0 },
	{ "default_border", cmd_default_border, 0 },
	{ "default_floating_border", cmd_default_floating_border, 0 },
	{ "default_orientation", cmd_default_orientation, CMD_CONFIG_ONLY },
	{ "exec", cmd_exec, 0 },
	{ "exec_always", cmd_exec_always, 0 },
	{ "exit", cmd_exit, CMD_RUNTIME_ONLY },
	{ "explorer_taskbar", cmd_explorer_taskbar, 0 },
	{ "floating", cmd_floating, CMD_RUNTIME_ONLY },
	{ "floating_maximum_size", cmd_ignored, 0 },
	{ "floating_minimum_size", cmd_ignored, 0 },
	{ "floating_modifier", cmd_floating_modifier, 0 },
	{ "focus", cmd_focus, CMD_RUNTIME_ONLY },
	{ "focus_follows_mouse", cmd_focus_follows_mouse, 0 },
	{ "focus_on_window_activation", cmd_ignored, 0 },
	{ "focus_wrapping", cmd_focus_wrapping, 0 },
	{ "font", cmd_font, 0 },
	{ "for_window", cmd_for_window, 0 },
	{ "force_display_urgency_hint", cmd_ignored, 0 },
	{ "fullscreen", cmd_fullscreen, CMD_RUNTIME_ONLY },
	{ "gaps", cmd_gaps, 0 },
	{ "grim", cmd_grim, CMD_RUNTIME_ONLY },
	{ "hide_edge_borders", cmd_hide_edge_borders, 0 },
	{ "include", cmd_include, CMD_CONFIG_ONLY },
	{ "inhibit_idle", cmd_ignored, 0 },
	{ "input", cmd_ignored, 0 },
	{ "kill", cmd_kill, CMD_RUNTIME_ONLY },
	{ "layout", cmd_layout, CMD_RUNTIME_ONLY },
	{ "makoctl", cmd_makoctl, CMD_RUNTIME_ONLY },
	{ "mark", cmd_mark, CMD_RUNTIME_ONLY },
	{ "max_render_time", cmd_ignored, 0 },
	{ "mode", cmd_mode, 0 },
	{ "mouse_warping", cmd_mouse_warping, 0 },
	{ "move", cmd_move, CMD_RUNTIME_ONLY },
	{ "new_float", cmd_ignored, 0 },
	{ "new_window", cmd_ignored, 0 },
	{ "no_focus", cmd_no_focus, 0 },
	{ "nop", cmd_nop, 0 },
	{ "notify-send", cmd_notify_send, CMD_RUNTIME_ONLY },
	{ "opacity", cmd_opacity, CMD_RUNTIME_ONLY },
	{ "output", cmd_output, 0 },
	{ "popup_during_fullscreen", cmd_ignored, 0 },
	{ "primary_selection", cmd_ignored, 0 },
	{ "reload", cmd_reload, CMD_RUNTIME_ONLY },
	{ "rename", cmd_rename, CMD_RUNTIME_ONLY },
	{ "resize", cmd_resize, CMD_RUNTIME_ONLY },
	{ "restart", cmd_restart, CMD_RUNTIME_ONLY },
	{ "scratchpad", cmd_scratchpad, CMD_RUNTIME_ONLY },
	{ "seat", cmd_ignored, 0 },
	{ "set", cmd_set, 0 },
	{ "shortcuts_inhibitor", cmd_ignored, 0 },
	{ "show_marks", cmd_ignored, 0 },
	{ "smart_borders", cmd_smart_borders, 0 },
	{ "smart_gaps", cmd_ignored, 0 },
	{ "split", cmd_split, CMD_RUNTIME_ONLY },
	{ "splith", cmd_splith, CMD_RUNTIME_ONLY },
	{ "splitt", cmd_splitt, CMD_RUNTIME_ONLY },
	{ "splitv", cmd_splitv, CMD_RUNTIME_ONLY },
	{ "sticky", cmd_sticky, CMD_RUNTIME_ONLY },
	{ "swap", cmd_swap, CMD_RUNTIME_ONLY },
	{ "swaybg_command", cmd_ignored, CMD_CONFIG_ONLY },
	{ "swaynag_command", cmd_ignored, CMD_CONFIG_ONLY },
	{ "tiling_drag", cmd_tiling_drag, 0 },
	{ "tiling_drag_threshold", cmd_tiling_drag_threshold, 0 },
	{ "title_align", cmd_title_align, 0 },
	{ "title_format", cmd_title_format, CMD_RUNTIME_ONLY },
	{ "titlebar_border_thickness", cmd_titlebar_border_thickness, 0 },
	{ "titlebar_padding", cmd_titlebar_padding, 0 },
	{ "unbindsym", cmd_unbindsym, 0 },
	{ "unmark", cmd_unmark, CMD_RUNTIME_ONLY },
	{ "urgent", cmd_urgent, CMD_RUNTIME_ONLY },
	{ "workspace", cmd_workspace, 0 },
	{ "workspace_auto_back_and_forth", cmd_ws_auto_back_and_forth, 0 },
	{ "workspace_layout", cmd_workspace_layout, CMD_CONFIG_ONLY },
	{ "xwayland", cmd_ignored, CMD_CONFIG_ONLY },
};

static const char *current_name = "";

const char *cmd_current_name(void) {
	return current_name;
}

static int handler_compare(const void *_a, const void *_b) {
	const struct cmd_handler *a = _a;
	const struct cmd_handler *b = _b;
	return swine_strcasecmp(a->command, b->command);
}

const struct cmd_handler *find_handler(const char *line,
		const struct cmd_handler *table, size_t table_size) {
	if (!table || !table_size) {
		return NULL;
	}
	const struct cmd_handler query = { .command = line };
	return bsearch(&query, table,
		table_size / sizeof(struct cmd_handler),
		sizeof(struct cmd_handler), handler_compare);
}

static const struct cmd_handler *find_core_handler(const char *name,
		struct cmd_results **error) {
	const struct cmd_handler *handler =
		find_handler(name, handlers, sizeof(handlers));
	if (!handler) {
		*error = cmd_results_new(CMD_INVALID,
			"Unknown/invalid command '%s'", name);
		return NULL;
	}
	if (config->reading && (handler->flags & CMD_RUNTIME_ONLY)) {
		*error = cmd_results_new(CMD_INVALID,
			"Command '%s' can only be used at runtime", name);
		return NULL;
	}
	if (!config->reading && (handler->flags & CMD_CONFIG_ONLY)) {
		*error = cmd_results_new(CMD_INVALID,
			"Command '%s' can only be used in the config file", name);
		return NULL;
	}
	return handler;
}

struct cmd_results *checkarg(int argc, const char *name,
		enum expected_args type, int val) {
	const char *error_name = NULL;
	switch (type) {
	case EXPECTED_AT_LEAST:
		if (argc < val) {
			error_name = "at least ";
		}
		break;
	case EXPECTED_AT_MOST:
		if (argc > val) {
			error_name = "at most ";
		}
		break;
	case EXPECTED_EQUAL_TO:
		if (argc != val) {
			error_name = "";
		}
	}
	return error_name ? cmd_results_new(CMD_INVALID,
		"Invalid %s command (expected %s%d argument%s, got %d)", name,
		error_name, val, val != 1 ? "s" : "", argc) : NULL;
}

static void set_handler_context(struct swine_node *node) {
	config->handler_context.node = node;
	config->handler_context.container = NULL;
	config->handler_context.workspace = NULL;
	config->handler_context.node_overridden = false;
	if (!node) {
		return;
	}
	switch (node->type) {
	case N_CONTAINER:
		config->handler_context.container = node->container;
		config->handler_context.workspace = node->container->workspace;
		break;
	case N_WORKSPACE:
		config->handler_context.workspace = node->workspace;
		break;
	case N_ROOT:
	case N_OUTPUT:
		break;
	}
}

list_t *execute_command(char *_exec, struct swine_container *con) {
	char matched_delim = ';';
	char *exec = swine_strdup(_exec);
	char *head = exec;
	list_t *res_list = create_list();
	list_t *containers = NULL;
	bool using_criteria = false;

	do {
		for (; isspace((unsigned char)*head); ++head) {}
		// Extract criteria (valid for this command list only).
		if (matched_delim == ';') {
			using_criteria = false;
			if (*head == '[') {
				char *error = NULL;
				struct criteria *criteria = criteria_parse(head, &error);
				if (!criteria) {
					list_add(res_list, cmd_results_new(CMD_INVALID, "%s", error));
					free(error);
					goto cleanup;
				}
				list_free(containers);
				containers = criteria_get_containers(criteria);
				head += strlen(criteria->raw);
				criteria_destroy(criteria);
				using_criteria = true;
				for (; isspace((unsigned char)*head); ++head) {}
			}
		}
		char *cmd = argsep(&head, ";,", &matched_delim);
		for (; isspace((unsigned char)*cmd); ++cmd) {}
		if (strcmp(cmd, "") == 0) {
			continue;
		}
		swine_log(SWINE_INFO, "Handling command '%s'", cmd);
		int argc;
		char **argv = split_args(cmd, &argc);
		if (strcmp(argv[0], "exec") != 0 &&
				strcmp(argv[0], "exec_always") != 0 &&
				strcmp(argv[0], "mode") != 0) {
			for (int i = 1; i < argc; ++i) {
				if (*argv[i] == '\"' || *argv[i] == '\'') {
					strip_quotes(argv[i]);
				}
			}
		}
		struct cmd_results *error = NULL;
		const struct cmd_handler *handler = find_core_handler(argv[0], &error);
		if (!handler) {
			list_add(res_list, error);
			free_argv(argc, argv);
			goto cleanup;
		}
		// Var replacement, for all but first argument of set
		for (int i = handler->handle == cmd_set ? 2 : 1; i < argc; ++i) {
			argv[i] = do_var_replacement(argv[i]);
		}
		current_name = handler->command;
		if (!using_criteria) {
			set_handler_context(con ? &con->node :
				seat_get_focus_inactive(&root->node));
			config->handler_context.node_overridden = con != NULL;
			struct cmd_results *res = handler->handle(argc - 1, argv + 1);
			list_add(res_list, res);
			if (res->status == CMD_INVALID) {
				current_name = "";
				free_argv(argc, argv);
				goto cleanup;
			}
		} else if (containers->length == 0) {
			list_add(res_list,
				cmd_results_new(CMD_FAILURE, "No matching node."));
		} else {
			struct cmd_results *fail_res = NULL;
			for (int i = 0; i < containers->length; ++i) {
				struct swine_container *container = containers->items[i];
				set_handler_context(&container->node);
				config->handler_context.node_overridden = true;
				struct cmd_results *res = handler->handle(argc - 1, argv + 1);
				if (res->status == CMD_SUCCESS) {
					free_cmd_results(res);
				} else {
					// last failure will take precedence
					if (fail_res) {
						free_cmd_results(fail_res);
					}
					fail_res = res;
					if (res->status == CMD_INVALID) {
						list_add(res_list, fail_res);
						current_name = "";
						free_argv(argc, argv);
						goto cleanup;
					}
				}
			}
			list_add(res_list,
				fail_res ? fail_res : cmd_results_new(CMD_SUCCESS, NULL));
		}
		current_name = "";
		free_argv(argc, argv);
	} while (head);
cleanup:
	free(exec);
	list_free(containers);
	return res_list;
}

struct cmd_results *config_command(char *exec, char **new_block) {
	struct cmd_results *results = NULL;
	int argc;
	char **argv = split_args(exec, &argc);

	if (!argc) {
		results = cmd_results_new(CMD_SUCCESS, NULL);
		goto cleanup;
	}
	if (argc > 1 && strcmp(argv[argc - 1], "{") == 0) {
		*new_block = join_args(argv, argc - 1);
		results = cmd_results_new(CMD_BLOCK, NULL);
		goto cleanup;
	}
	if (strcmp(argv[argc - 1], "}") == 0) {
		results = cmd_results_new(CMD_BLOCK_END, NULL);
		goto cleanup;
	}
	// Make sure the command is not stored in a variable
	if (*argv[0] == '$') {
		argv[0] = do_var_replacement(argv[0]);
		char *temp = join_args(argv, argc);
		free_argv(argc, argv);
		argv = split_args(temp, &argc);
		free(temp);
		if (!argc) {
			results = cmd_results_new(CMD_SUCCESS, NULL);
			goto cleanup;
		}
	}

	const struct cmd_handler *handler = find_core_handler(argv[0], &results);
	if (!handler) {
		goto cleanup;
	}

	if (handler->handle == cmd_set && argc > 1 && *argv[1] == '$') {
		// Escape the variable name so it does not get replaced by one shorter
		char *temp = calloc(1, strlen(argv[1]) + 2);
		temp[0] = '$';
		strcpy(&temp[1], argv[1]);
		free(argv[1]);
		argv[1] = temp;
	}
	char *command = do_var_replacement(join_args(argv, argc));
	free_argv(argc, argv);
	argv = split_args(command, &argc);
	free(command);

	for (int i = handler->handle == cmd_set ? 2 : 1; i < argc; ++i) {
		if (handler->handle != cmd_exec && handler->handle != cmd_exec_always
				&& handler->handle != cmd_mode
				&& handler->handle != cmd_bindsym
				&& handler->handle != cmd_set
				&& (*argv[i] == '\"' || *argv[i] == '\'')) {
			strip_quotes(argv[i]);
		}
	}

	set_handler_context(seat_get_focus_inactive(&root->node));
	current_name = handler->command;
	results = handler->handle(argc - 1, argv + 1);
	current_name = "";

cleanup:
	free_argv(argc, argv);
	return results;
}

struct cmd_results *config_subcommand(char **argv, int argc,
		const struct cmd_handler *table, size_t table_size) {
	char *command = join_args(argv, argc);
	swine_log(SWINE_DEBUG, "Subcommand: %s", command);
	free(command);

	const struct cmd_handler *handler = find_handler(argv[0], table,
		table_size);
	if (!handler) {
		return cmd_results_new(CMD_INVALID,
			"Unknown/invalid command '%s'", argv[0]);
	}
	if (handler->handle) {
		return handler->handle(argc - 1, argv + 1);
	}
	return cmd_results_new(CMD_INVALID,
		"The command '%s' is shimmed, but unimplemented", argv[0]);
}

struct cmd_results *cmd_results_new(enum cmd_status status,
		const char *format, ...) {
	struct cmd_results *results = calloc(1, sizeof(*results));
	results->status = status;
	if (format) {
		va_list args;
		va_start(args, format);
		int length = vsnprintf(NULL, 0, format, args);
		va_end(args);
		results->error = malloc((size_t)length + 1);
		va_start(args, format);
		vsnprintf(results->error, (size_t)length + 1, format, args);
		va_end(args);
	}
	return results;
}

void free_cmd_results(struct cmd_results *results) {
	if (!results) {
		return;
	}
	free(results->error);
	free(results);
}

void free_cmd_results_list(list_t *res_list) {
	if (!res_list) {
		return;
	}
	for (int i = 0; i < res_list->length; ++i) {
		free_cmd_results(res_list->items[i]);
	}
	list_free(res_list);
}

char *cmd_results_to_json(list_t *res_list) {
	struct json_writer w;
	json_init(&w);
	json_begin_array(&w);
	for (int i = 0; i < res_list->length; ++i) {
		struct cmd_results *results = res_list->items[i];
		json_begin_object(&w);
		json_key(&w, "success");
		json_bool(&w, results->status == CMD_SUCCESS);
		if (results->error) {
			json_key(&w, "parse_error");
			json_bool(&w, results->status == CMD_INVALID);
			json_key(&w, "error");
			json_string(&w, results->error);
		}
		json_end_object(&w);
	}
	json_end_array(&w);
	return json_finish(&w);
}
