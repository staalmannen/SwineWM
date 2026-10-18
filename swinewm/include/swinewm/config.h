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
	BINDING_BORDER = 1 << 4,    // mouse only: on a container's border
	BINDING_CONTENTS = 1 << 5,  // mouse only: on a container's contents
	BINDING_TITLEBAR = 1 << 6,  // mouse only: on a container's title bar
};

struct swine_binding {
	int order;
	uint32_t flags;
	uint32_t modifiers;
	uint32_t keycode; // key: virtual-key code; mouse: button (cursor.h)
	char *input;      // the original key combination text
	char *command;
};

struct swine_mode {
	char *name;
	list_t *keysym_bindings;   // struct swine_binding *
	list_t *mouse_bindings;    // struct swine_binding *
};

// "output <name|identifier|*> ..." (swaybg options and the display mode).
struct output_config {
	char *name;               // output name, identifier or "*"
	char *background;         // image path (resolved), or a #color
	char *background_mode;    // stretch|fill|fit|center|tile|solid_color
	char *background_fallback; // #color shown around/instead of the image
	int width, height;        // "mode", in the unrotated frame; 0 = unset
	float refresh_rate;       // Hz; 0 = the highest for the mode
	int x, y;                 // "position"; INT_MIN = unset
	int transform;            // quarter turns clockwise; -1 = unset
	float scale;              // accepted; 0 = unset (not applied, see doc)
};

// "workspace <name> output <outputs...>" and "workspace <name> gaps ..."
struct workspace_config {
	char *workspace;
	list_t *outputs;          // char *: output names/identifiers, in order
	int gaps_inner;           // INT_MIN when not set
	struct side_gaps gaps_outer;
};

struct swine_variable {
	char *name;
	char *value;
};

enum focus_follows_mouse_mode {
	FOLLOWS_NO,
	FOLLOWS_YES,
	FOLLOWS_ALWAYS,
};

enum mouse_warping_mode {
	WARP_NO,
	WARP_OUTPUT,
	WARP_CONTAINER,
};

// Colours of one client.* class, as 0xRRGGBBAA.
struct border_colors {
	uint32_t border;
	uint32_t background;
	uint32_t text;
	uint32_t indicator;
	uint32_t child_border;
};

enum edge_border_types {
	E_NONE,
	E_VERTICAL,
	E_HORIZONTAL,
	E_BOTH,
};

enum smart_borders_mode {
	ESMART_OFF,
	ESMART_ON,
	ESMART_NO_GAPS,
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
	list_t *output_configs;    // struct output_config *
	list_t *criteria;          // struct criteria *: for_window, assign, no_focus
	list_t *workspace_configs; // struct workspace_config *
	list_t *config_chain;      // char *: files read (main config, includes)
	list_t *errors;            // char *: for swaynag (see swaynag.h)

	// Integrated Waybar-compatible bar, enabled by a sway "bar { }" block or
	// by "exec waybar". Everything else comes from the Waybar config.
	bool explorer_taskbar_autohide;  // swinewm extension, see settings.c
	int windows_theme;           // swinewm extension: -1 none, 0 light, 1 dark, 2 auto
	bool bar_enabled;
	bool bar_invisible;          // "bar mode invisible"
	int bar_position;            // 0 = unset, 1 = top, 2 = bottom

	enum swine_container_layout default_orientation;  // L_NONE = auto
	enum swine_container_layout default_layout;       // workspace_layout
	int gaps_inner;
	struct side_gaps gaps_outer;
	enum focus_wrapping_mode focus_wrapping;
	enum focus_follows_mouse_mode focus_follows_mouse;
	enum mouse_warping_mode mouse_warping;
	bool auto_back_and_forth;
	uint32_t floating_mod;
	bool floating_mod_inverse;   // move with the right button, resize left
	bool tiling_drag;
	int tiling_drag_threshold;

	// Decorations
	enum swine_container_border border;            // default_border
	int border_thickness;
	enum swine_container_border floating_border;   // default_floating_border
	int floating_border_thickness;
	enum edge_border_types hide_edge_borders;
	enum smart_borders_mode hide_edge_borders_smart;
	bool hide_lone_tab;
	struct {
		struct border_colors focused;
		struct border_colors focused_inactive;
		struct border_colors focused_tab_title;
		struct border_colors unfocused;
		struct border_colors urgent;
		struct border_colors placeholder;
		uint32_t background;
	} border_colors;
	bool has_focused_tab_title;
	char *font;                  // "pango:DejaVu Sans Mono 10", as given
	int font_height;             // its height in pixels at scale 1; 0: unknown
	int titlebar_border_thickness;
	int titlebar_h_padding;
	int titlebar_v_padding;
	int title_align;             // 0 left, 1 center, 2 right

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
		bool node_overridden;   // criteria or an explicit container
	} handler_context;
};

extern struct swine_config *config;

struct swine_config *config_create(void);
void free_config(struct swine_config *config);
void free_workspace_config(struct workspace_config *wsc);
// Returns the first existing file among platform_config_paths(name), or
// NULL. Caller frees.
char *find_config_file(const char *name);
// Reads a whole file (UTF-8 path). Caller frees; NULL on error.
char *read_file(const char *path);
// Loads (or reloads) the main config. On failure the previous config stays.
bool load_main_config(const char *path, bool is_active, bool validating);
// Reads config text into the active config. Used by include and tests.
bool read_config_text(const char *text, const char *name);
// "include <path>": every file matching the path (wildcards in the file
// name), sorted, each at most once; relative to the including file.
void load_include_configs(const char *path);
// Runs queued exec/exec_always commands collected while reading.
void config_run_deferred(void);

char *do_var_replacement(char *str);   // may realloc; returns the new pointer
// Expands ~ and %VAR%; a relative path is taken relative to the directory
// of the config file being read. Caller frees.
char *config_resolve_path(const char *path);
struct output_config *config_find_output_config(const char *name);
// The config whose background applies to an output: its own if it sets a
// background, else the "*" one, else NULL.
struct output_config *output_config_for_name(const char *output_name);
void free_output_config(struct output_config *oc);
void free_swine_binding(struct swine_binding *binding);
struct swine_mode *config_find_mode(const char *name);
struct swine_mode *config_add_mode(const char *name);

#endif
