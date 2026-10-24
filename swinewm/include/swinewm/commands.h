#ifndef _SWINE_COMMANDS_H
#define _SWINE_COMMANDS_H

#include <stdbool.h>
#include <stddef.h>
#include "list.h"

struct swine_container;

enum cmd_status {
	CMD_SUCCESS,
	CMD_FAILURE,     // valid command that could not run
	CMD_INVALID,     // unknown command or bad arguments
	CMD_DEFER,       // queue until the config has been read
	CMD_BLOCK,       // start of a { block }
	CMD_BLOCK_END,
};

struct cmd_results {
	enum cmd_status status;
	char *error;
};

enum expected_args {
	EXPECTED_AT_LEAST,
	EXPECTED_AT_MOST,
	EXPECTED_EQUAL_TO,
};

typedef struct cmd_results *swine_cmd(int argc, char **argv);

enum cmd_handler_flags {
	CMD_CONFIG_ONLY = 1 << 0,   // only valid while reading the config
	CMD_RUNTIME_ONLY = 1 << 1,  // only valid at runtime (not in the config)
};

struct cmd_handler {
	const char *command;
	swine_cmd *handle;
	unsigned flags;
};

// Parses and runs a command line (commands separated by ';' or ',').
// Returns a list of struct cmd_results *, one per command.
list_t *execute_command(char *command, struct swine_container *con);
// Runs a single config line (no ';' splitting, variables replaced). When the
// line opens a block, *new_block receives the block prefix.
struct cmd_results *config_command(char *command, char **new_block);
// Name of the command currently being executed (for shared handlers).
const char *cmd_current_name(void);
// Handles "exec" of programs whose job swinewm does itself (waybar, mako,
// makoctl, notify-send, grim). Returns NULL if the command should really run.
// With persistent_only, only programs that stay running (waybar, mako) are
// handled: "exec" lines are skipped on reload, but their effect must remain.
struct cmd_results *exec_builtin(const char *cmdline, bool persistent_only);
struct cmd_results *config_subcommand(char **argv, int argc,
		const struct cmd_handler *handlers, size_t handlers_size);
struct cmd_results *checkarg(int argc, const char *name,
		enum expected_args type, int val);
const struct cmd_handler *find_handler(const char *line,
		const struct cmd_handler *handlers, size_t handlers_size);

struct cmd_results *cmd_results_new(enum cmd_status status,
		const char *error, ...);
void free_cmd_results(struct cmd_results *results);
void free_cmd_results_list(list_t *res_list);
char *cmd_results_to_json(list_t *res_list);

swine_cmd cmd_bar;
swine_cmd cmd_bindcode;
swine_cmd cmd_bindsym;
swine_cmd cmd_border;
swine_cmd cmd_client_focused;
swine_cmd cmd_client_focused_inactive;
swine_cmd cmd_client_focused_tab_title;
swine_cmd cmd_client_unfocused;
swine_cmd cmd_client_urgent;
swine_cmd cmd_default_border;
swine_cmd cmd_default_floating_border;
swine_cmd cmd_font;
swine_cmd cmd_hide_edge_borders;
swine_cmd cmd_opacity;
swine_cmd cmd_smart_borders;
swine_cmd cmd_show_marks;
swine_cmd cmd_smart_gaps;
swine_cmd cmd_title_align;
swine_cmd cmd_title_format;
swine_cmd cmd_titlebar_border_thickness;
swine_cmd cmd_titlebar_padding;
swine_cmd cmd_default_orientation;
swine_cmd cmd_exec;
swine_cmd cmd_exec_always;
swine_cmd cmd_assign;
swine_cmd cmd_exit;
swine_cmd cmd_for_window;
swine_cmd cmd_no_focus;
swine_cmd cmd_explorer_taskbar;
swine_cmd cmd_floating;
swine_cmd cmd_floating_modifier;
swine_cmd cmd_focus;
swine_cmd cmd_focus_follows_mouse;
swine_cmd cmd_mouse_warping;
swine_cmd cmd_tiling_drag;
swine_cmd cmd_tiling_drag_threshold;
swine_cmd cmd_focus_wrapping;
swine_cmd cmd_fullscreen;
swine_cmd cmd_gaps;
swine_cmd cmd_ignored;
swine_cmd cmd_include;
swine_cmd cmd_kill;
swine_cmd cmd_layout;
swine_cmd cmd_makoctl;
swine_cmd cmd_mark;
swine_cmd cmd_mode;
swine_cmd cmd_move;
swine_cmd cmd_nop;
swine_cmd cmd_input;
// pactl, wpctl and pamixer volume commands in exec (commands/audio.c), or
// NULL if the line is not one of them.
struct cmd_results *audio_builtin(const char *name, int argc, char **argv);
swine_cmd cmd_output;
swine_cmd cmd_notify_send;
swine_cmd cmd_grim;
swine_cmd cmd_reload;
swine_cmd cmd_rename;
swine_cmd cmd_restart;
swine_cmd cmd_resize;
swine_cmd cmd_scratchpad;
swine_cmd cmd_set;
swine_cmd cmd_split;
swine_cmd cmd_splith;
swine_cmd cmd_splitt;
swine_cmd cmd_splitv;
swine_cmd cmd_sticky;
swine_cmd cmd_swap;
swine_cmd cmd_unbindsym;
swine_cmd cmd_unbindcode;
swine_cmd cmd_unmark;
swine_cmd cmd_urgent;
swine_cmd cmd_workspace;
swine_cmd cmd_windows_theme;
swine_cmd cmd_workspace_layout;
swine_cmd cmd_ws_auto_back_and_forth;

// From commands/resize.c (as in sway), also used by mouse resizing. axis
// is a combination of enum swine_edges.
struct swine_container *container_find_resize_parent(
		struct swine_container *con, unsigned axis);
void container_resize_tiled(struct swine_container *con, unsigned axis,
		int amount);
void floating_calculate_constraints(struct swine_container *con,
		int *min_width, int *max_width, int *min_height, int *max_height);

#endif
