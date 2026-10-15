#ifndef _SWINE_VIEW_H
#define _SWINE_VIEW_H

#include <stdbool.h>
#include "swinewm/tree/node.h"

// A view is one managed native window (an HWND on Windows). The platform layer
// creates views and reports their properties; the tree decides placement.
struct swine_view {
	struct swine_container *container;
	void *handle;              // HWND
	void *platform;            // platform-private per-window state
	char *title;               // UTF-8
	char *app_id;              // executable base name, e.g. "notepad.exe"
	char *class_name;          // window class
	unsigned long pid;
	bool natural_floating;     // dialogs, fixed-size windows, ...
	struct swine_box natural;  // geometry when first seen

	// What the arrange pass wants...
	struct {
		struct swine_box box;
		bool visible;
	} pending;
	// ...and what was last pushed to the platform.
	struct {
		struct swine_box box;
		bool visible;
		bool valid;
	} current;
};

struct swine_view *view_create(void *handle);
// Inserts the view into the tree at the focused position (like view_map in
// sway) and focuses it.
void view_map(struct swine_view *view);
// Removes the view from the tree; the view itself stays allocated.
void view_unmap(struct swine_view *view);
void view_destroy(struct swine_view *view);
struct swine_view *view_from_handle(void *handle);
void view_set_title(struct swine_view *view, const char *title);
bool view_is_visible(struct swine_view *view);

#endif
