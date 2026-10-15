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
	void *handle;             // platform handle (HMONITOR on Windows)
	struct swine_box box;     // full monitor rectangle
	struct swine_box usable;  // box minus app bars / taskbar (work area)
	double scale;
	bool enabled;
	bool primary;
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
struct swine_output *output_by_handle(void *handle);
struct swine_workspace *output_get_active_workspace(struct swine_output *output);
void output_add_workspace(struct swine_output *output,
		struct swine_workspace *ws);
struct swine_output *output_get_in_direction(struct swine_output *output,
		enum swine_direction dir);
void output_sort_workspaces(struct swine_output *output);

#endif
