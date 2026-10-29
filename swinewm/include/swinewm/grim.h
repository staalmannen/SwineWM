#ifndef _SWINE_GRIM_H
#define _SWINE_GRIM_H

#include <stdbool.h>
#include "list.h"
#include "swinewm/bar/css.h"
#include "swinewm/tree/node.h"

// The built-in grim, slurp, wl-copy and grimshot (commands/grim.c). They
// run when sway configs exec them; shell forms such as
// grim -g "$(slurp)" - | wl-copy are understood without a shell.

// A predefined rectangle slurp offers (its standard input, -o).
struct slurp_choice {
	struct swine_box box;
	char *label;
};

// slurp's options (slurp(1)); the platform draws the selection.
struct slurp_options {
	struct swine_color background;   // -b, default #FFFFFF40
	struct swine_color border;       // -c, #000000FF
	struct swine_color selection;    // -s, #00000000
	struct swine_color choice;       // -B, #FFFFFF40
	int border_weight;               // -w, 2
	bool dimensions;                 // -d
	bool single_point;               // -p
	bool restrict_choice;            // -r
	bool crosshairs;                 // -x
	char font_family[64];            // -F, sans-serif
	int aspect_w, aspect_h;          // -a w:h, 0 = free
	list_t *choices;                 // struct slurp_choice *
};

// Called by the platform when the selection ends: ok false if cancelled
// (Escape). label is the chosen predefined rectangle's, or NULL.
void grim_slurp_done(bool ok, const struct swine_box *box, const char *label);
bool grim_slurp_active(void);

// Runs one "&&"-separated part of an exec line if it is grim, slurp,
// grimshot or wl-copy, possibly with "|" between them. Returns NULL if it
// is not. When a selection has to be made first, *deferred is set and
// rest (the remaining "&&" parts, or NULL) runs once it succeeded.
struct cmd_results *grim_exec(int argc, char **argv, const char *rest,
		bool *deferred);
// Whether grim_exec would run this part.
bool grim_exec_handles(int argc, char **argv);

// slurp's -f format, for tests: %x %y %w %h %X %Y %W %H %l %o.
char *slurp_format(const char *format, const struct swine_box *box,
		const char *label);

void grim_finish(void);

#endif
