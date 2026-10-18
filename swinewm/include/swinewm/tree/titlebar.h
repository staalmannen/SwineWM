#ifndef _SWINE_TITLEBAR_H
#define _SWINE_TITLEBAR_H

#include <stdbool.h>
#include "list.h"
#include "swinewm/config.h"
#include "swinewm/tree/node.h"

struct swine_container;

// Title bars of tabbed and stacked containers. sway draws a title bar for
// every container; on Windows a window with "border normal" keeps its own,
// so swinewm only draws the tabs of tabbed and stacked containers (where
// each child's title bar is its tab).

struct swine_tab {
	struct swine_container *con;   // the child the tab stands for
	char *title;                   // its title, or "H[app app]" for a split
	struct border_colors colors;   // client.* class of the child
};

struct swine_titlebar {
	struct swine_box box;          // the whole title area
	bool stacked;                  // one row per child, else side by side
	int tab_height;                // pixels
	double scale;                  // of the output, for fonts and padding
	list_t *tabs;                  // struct swine_tab *
};

// Height of one title bar in pixels at scale 1: the font height plus the
// vertical padding, as sway's container_titlebar_height.
int container_titlebar_height(void);

// The title bars of every visible tabbed or stacked container, rebuilt by
// arrange_root (struct swine_titlebar *, owned by the core).
list_t *titlebars_get(void);
void titlebars_clear(void);
void titlebars_add(struct swine_node *parent, list_t *children,
		const struct swine_box *box, bool stacked, int tab_height,
		double scale);

// sway's container_build_representation: "H[app1 app2]" (a window: its
// title). Caller frees.
char *container_representation(struct swine_container *con);
// The title shown on a tab: title_format with %title, %app_id, %class,
// %instance and %shell filled in (sway's parse_title_format; no Pango
// markup). Caller frees.
char *container_formatted_title(struct swine_container *con);

#endif
