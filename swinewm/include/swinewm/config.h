#ifndef _SWINE_CONFIG_H
#define _SWINE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include "list.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/workspace.h"

enum binding_flags {
	BINDING_RELEASE = 1 << 0,
	BINDING_LOCKED = 1 << 1,
	BINDING_NOREPEAT = 1 << 2,
	BINDING_INHIBITED = 1 << 3,
};

struct swine_binding {
	int order;
	uint32_t flags;
	uint32_t modifiers;
	uint32_t keycode;
	char *input;      // the original key combination text
	char *command;
};

struct swine_mode {
	char *name;
	list_t *keysym_bindings;   // struct swine_binding *
};

struct swine_variable {
	char *name;
	char *value;
};

enum focus_wrapping_mode {
	WRAP_NO,
	WRAP_YES,
	WRAP_FORCE,
	WRAP_WORKSPACE,
};

struct swine_config {
	list_t *symbols;           // struct swine_variable *
	list_t *modes;             // struct swine_mode *
	struct swine_mode *current_mode;
	list_t *cmd_queue;         // deferred exec commands (char *)

	enum swine_container_layout default_orientation;  // L_NONE = auto
	enum swine_container_layout default_layout;       // workspace_layout
	int gaps_inner;
	struct side_gaps gaps_outer;
	enum focus_wrapping_mode focus_wrapping;
	bool focus_follows_mouse;
	bool auto_back_and_forth;
	uint32_t floating_mod;

	char *current_config_path;
	char *current_config;      // full text of the main config file
	int current_line;
	const char *current_file;
	bool active;               // false during the very first load
	bool reloading;
	bool validating;
	bool reading;
	int ignored_count;

	// Set by execute_command for the duration of one command.
	struct {
		struct swine_node *node;
		struct swine_container *container;
		struct swine_workspace *workspace;
	} handler_context;
};

extern struct swine_config *config;

struct swine_config *config_create(void);
void free_config(struct swine_config *config);
// Locates the config file using platform_config_paths(). Caller frees.
char *find_config_path(void);
// Loads (or reloads) the main config. On failure the previous config stays.
bool load_main_config(const char *path, bool is_active, bool validating);
// Reads config text into the active config. Used by include and tests.
bool read_config_text(const char *text, const char *name);
bool load_include_config(const char *path);
// Runs queued exec/exec_always commands collected while reading.
void config_run_deferred(void);

char *do_var_replacement(char *str);   // may realloc; returns the new pointer
void free_swine_binding(struct swine_binding *binding);
struct swine_mode *config_find_mode(const char *name);
struct swine_mode *config_add_mode(const char *name);

#endif
