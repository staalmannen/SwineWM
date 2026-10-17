#ifndef _SWINE_OUTPUT_H
#define _SWINE_OUTPUT_H

#include <stdbool.h>
#include "list.h"
#include "swinewm/tree/node.h"

enum swine_direction {
	DIR_LEFT,
	DIR_RIGHT,
	DIR_UP,
	DIR_DOWN,
};

struct swine_output {
	struct swine_node node;
	char *name;               // e.g. "DISPLAY1"
	char *description;
	// sway's "make model serial"; here the monitor's name as Windows shows
	// it in the display settings (e.g. "DELL U2720Q"), or NULL.
	char *identifier;
	void *handle;             // platform handle (HMONITOR on Windows)
	struct swine_box box;     // full monitor rectangle
	struct swine_box usable;  // box minus app bars / taskbar (work area)
	struct side_gaps reserved; // space taken by swinewm's own bar
	double scale;
	int transform;            // current rotation, quarter turns clockwise
	int refresh_mhz;          // current refresh rate in mHz, 0 if unknown
	bool enabled;
	bool primary;
	// A window swinewm does not manage (a game, a presentation) covers the
	// whole monitor and is in front: the bar gets out of its way.
	bool external_fullscreen;
	list_t *workspaces;       // struct swine_workspace *
	struct swine_workspace *active_workspace;
};

struct swine_output *output_create(const char *name, void *handle);
// Enables the output: attaches it to the root and gives it a workspace,
// stealing workspaces from the fallback output if there are any.
void output_enable(struct swine_output *output);
// Evacuates workspaces to another output (or the fallback) and destroys it.
void output_destroy(struct swine_output *output);
struct swine_output *output_by_name(const char *name);
// Matches the name or, failing that, the identifier (ignoring case).
struct swine_output *output_by_name_or_id(const char *name_or_id);
struct swine_output *output_by_handle(void *handle);
// The output whose monitor contains the point, or NULL.
struct swine_output *output_at_point(int x, int y);
// Applies the mode, position and transform from the "output" configs to
// every output (after the config has been read, and on hot-plug).
void output_apply_configs(void);
void output_apply_config(struct swine_output *output);
struct swine_workspace *output_get_active_workspace(struct swine_output *output);
// Area available to windows: the work area minus swinewm's own bar.
struct swine_box output_get_usable_area(struct swine_output *output);
void output_add_workspace(struct swine_output *output,
		struct swine_workspace *ws);
struct swine_output *output_get_in_direction(struct swine_output *output,
		enum swine_direction dir);
void output_sort_workspaces(struct swine_output *output);

#endif
