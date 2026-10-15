#ifndef _SWINE_BAR_H
#define _SWINE_BAR_H

#include <stdbool.h>
#include <time.h>
#include "list.h"
#include "swinewm/bar/css.h"
#include "swinewm/tree/node.h"

// The integrated bar: a clone of Waybar that reads Waybar's config.jsonc
// and style.css and runs inside swinewm. This part is portable: it loads the
// config, decides what each module shows and computes CSS styles. The
// platform measures, lays out and paints the resulting model.

struct swine_output;

enum bar_section {
	BAR_SECTION_LEFT,
	BAR_SECTION_CENTER,
	BAR_SECTION_RIGHT,
};

// One label: a whole module, or one workspace button.
struct bar_item {
	char *text;              // markup already stripped
	char *on_click;          // swinewm command for a left click, or NULL
	char *on_scroll_up;
	char *on_scroll_down;
	struct swine_style style;
};

struct bar_module_view {
	enum bar_section section;
	int module_index;        // pass to bar_module_clicked()
	struct swine_style style;  // the module's box (#workspaces)
	// true: items are buttons inside a box styled by `style` (workspaces).
	// false: one label that carries its own style (style == item style).
	bool is_box;
	list_t *items;           // struct bar_item *
};

struct bar_model {
	struct swine_style window;   // window#waybar
	int spacing;                 // px between modules ("spacing")
	list_t *modules;             // struct bar_module_view *
};

// (Re)reads the Waybar config and style sheet. Safe to call repeatedly.
void bar_load(void);
// Same, from text (NULL selects the built-in default). For tests.
void bar_load_from_strings(const char *config_text, const char *style_text);
void bar_finish(void);

bool bar_enabled(void);
// Height in logical pixels; multiply by output->scale for device pixels.
int bar_height(void);
bool bar_at_top(void);
// Sets each output's reserved area for the bar. Called before arranging.
void bar_update_reservations(void);
// Where the bar for this output goes; false if it has none or should be
// hidden (fullscreen window on the output).
bool bar_get_rect(struct swine_output *output, struct swine_box *out);

struct bar_model *bar_build(struct swine_output *output, time_t now);
void bar_model_free(struct bar_model *model);
// Module-level click handling (e.g. clock "format-alt").
void bar_module_clicked(int module_index);

#endif
