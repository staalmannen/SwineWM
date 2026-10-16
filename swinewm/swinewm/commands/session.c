// exit, reload, kill and nop: the commands that act on the WM itself.
#include <stdlib.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/swaynag.h"
#include "swinewm/tree/output.h"
#include "swinewm/platform.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/view.h"
#include "swinewm/tree/workspace.h"

struct cmd_results *cmd_exit(int argc, char **argv) {
	(void)argv;
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "exit", EXPECTED_EQUAL_TO, 0))) {
		return error;
	}
	platform_quit();
	return cmd_results_new(CMD_SUCCESS, NULL);
}

// i3's "restart" (sway has none): start the executable again, which picks
// up a new swinewm.exe. Windows cannot overwrite a running .exe but can
// rename it, so an update is: rename, copy the new one, "restart".
struct cmd_results *cmd_restart(int argc, char **argv) {
	(void)argv;
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "restart", EXPECTED_EQUAL_TO, 0))) {
		return error;
	}
	platform_restart();
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_reload(int argc, char **argv) {
	(void)argv;
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "reload", EXPECTED_EQUAL_TO, 0))) {
		return error;
	}
	char *path = swine_strdup(config->current_config_path);
	bool ok = load_main_config(path, true, false);
	free(path);
	config_run_deferred();
	output_apply_configs();
	platform_config_changed();
	swaynag_show_config_errors();
	if (!ok) {
		return cmd_results_new(CMD_FAILURE,
			"Error(s) reloading config; see the log");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

static void close_view(struct swine_container *con, void *data) {
	(void)data;
	if (con->view) {
		platform_view_close(con->view);
	}
}

struct cmd_results *cmd_kill(int argc, char **argv) {
	(void)argc;
	(void)argv;
	struct swine_container *con = config->handler_context.container;
	struct swine_workspace *ws = config->handler_context.workspace;
	if (con) {
		close_view(con, NULL);
		container_for_each_child(con, close_view, NULL);
	} else if (ws) {
		workspace_for_each_container(ws, close_view, NULL);
	} else {
		return cmd_results_new(CMD_FAILURE, "Nothing to kill");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_nop(int argc, char **argv) {
	(void)argc;
	(void)argv;
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_ignored(int argc, char **argv) {
	(void)argc;
	(void)argv;
	config->ignored_count++;
	swine_log(SWINE_INFO, "'%s' has no effect in swinewm (yet); ignored",
		cmd_current_name());
	return cmd_results_new(CMD_SUCCESS, NULL);
}
