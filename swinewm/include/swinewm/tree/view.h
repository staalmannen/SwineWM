#ifndef _SWINE_VIEW_H
#define _SWINE_VIEW_H

#include <stdbool.h>
#include <stdint.h>
#include "swinewm/tree/node.h"

// How a window is decorated (border none|pixel|normal, client.* colours,
// opacity), worked out by view_update_decoration for the platform.
struct view_decoration {
	uint32_t border_color;      // 0xRRGGBBAA; alpha 0: no border
	bool titlebar;              // the window keeps its own title bar
	uint32_t title_color;       // its colour (client.* background), or 0
	uint32_t title_text_color;  // client.* text, or 0
	bool square_corners;        // tiled windows
	float alpha;                // opacity
};

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
	bool urgent;               // the application asked for attention
	long long urgent_since;    // platform_now_ms() when it became urgent
	bool urgency_denied;       // "urgent deny"
	bool fullscreen_requested; // the application made itself fullscreen
	struct swine_box natural;  // geometry when first seen
	list_t *executed_criteria; // for_window criteria already run (pointers)

	// What the arrange pass wants...
	struct {
		struct swine_box box;
		bool visible;
		struct view_decoration deco;
	} pending;
	// ...and what was last pushed to the platform.
	struct {
		struct swine_box box;
		bool visible;
		bool valid;
		struct view_decoration deco;
		bool deco_valid;
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
// Returns true if for_window rules ran (the tree may have changed).
bool view_set_title(struct swine_view *view, const char *title);
// Runs the for_window rules that match and have not run for this view.
void view_execute_criteria(struct swine_view *view);
bool view_is_visible(struct swine_view *view);
// Works out view->pending.deco from the container's border, the focus and
// the client.* colours (sway: container_get_current_colors and the smart
// borders part of view_autoconfigure).
void view_update_decoration(struct swine_view *view);
bool view_decoration_equal(const struct view_decoration *a,
		const struct view_decoration *b);
// Like sway: urgency set by the application (on Windows: flashing its
// taskbar button). Ignored for the focused view; cleared when focused.
void view_set_urgent(struct swine_view *view, bool enable);
// The application entered or left fullscreen by itself (e.g. F11 in a
// browser), like an xdg-toplevel fullscreen request in sway.
void view_request_fullscreen(struct swine_view *view, bool enable);

#endif
