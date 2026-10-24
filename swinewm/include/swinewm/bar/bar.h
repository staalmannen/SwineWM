#ifndef _SWINE_BAR_H
#define _SWINE_BAR_H

#include <stdbool.h>
#include <stdint.h>
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

// One label: a whole module, or one workspace button. Or one tray icon:
// then tray_id is set, text is empty and the platform draws the icon.
struct bar_item {
	char *text;              // markup already stripped
	char *on_click;          // swinewm command for a left click, or NULL
	char *on_click_middle;
	char *on_click_right;
	char *on_scroll_up;
	char *on_scroll_down;
	struct swine_style style;
	uint64_t tray_id;        // struct tray_item id, or 0
	int icon_size;           // logical px, for tray icons
	char *icon_path;         // "icons" override (an .ico file), or NULL
};

struct bar_module_view {
	enum bar_section section;
	int module_index;        // pass to bar_module_clicked()
	struct swine_style style;  // the module's box (#workspaces)
	// true: items are buttons inside a box styled by `style` (workspaces).
	// false: one label that carries its own style (style == item style).
	bool is_box;
	int item_spacing;        // px between items ("spacing" of the tray)
	list_t *items;           // struct bar_item *
};

struct bar_model {
	struct swine_style window;   // window#waybar
	int spacing;                 // px between modules ("spacing")
	list_t *modules;             // struct bar_module_view *
};

// The bar's look, for other tools (get_bar_config, swinemenu): the computed
// styles of window#waybar and of workspace buttons in each state.
struct bar_theme {
	struct swine_style window;
	struct swine_style workspace;   // #workspaces button
	struct swine_style focused;     // #workspaces button.focused
	struct swine_style urgent;      // #workspaces button.urgent
};
void bar_get_theme(struct bar_theme *out);

// (Re)reads the Waybar config and style sheet. Safe to call repeatedly.
void bar_load(void);
// Same, from text (NULL selects the built-in default). For tests.
void bar_load_from_strings(const char *config_text, const char *style_text);
void bar_finish(void);

bool bar_enabled(void);
// Whether the bar has a "tray" module, i.e. should host the tray.
bool bar_has_tray(void);
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
// Reads the system again for every module (after a volume change).
void bar_refresh_readings(void);
// Module-level click handling (e.g. clock "format-alt"); a left click.
void bar_module_clicked(int module_index);
// Another click or a scroll on a module (custom scripts run again).
void bar_module_event(int module_index);
// A scroll on a module without on-scroll-up/down: its own action (the
// pulseaudio volume). Returns true if there was one.
bool bar_module_scroll(int module_index, bool up);

#endif
