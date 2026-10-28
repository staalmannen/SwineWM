#ifndef _SWINE_CSS_H
#define _SWINE_CSS_H

#include <stdbool.h>
#include <stdint.h>

// The subset of GTK CSS that Waybar themes use most: type/#id/.class
// selectors with descendant (and '>') combinators, @define-color, and the
// properties below. Unsupported selectors (pseudo-classes, sibling
// combinators) never match; unsupported properties are ignored.

struct swine_color {
	uint8_t r, g, b, a;   // a = 255 is opaque
};

struct css_edge {
	int width;
	struct swine_color color;
};

struct css_box {
	int top, right, bottom, left;
};

struct swine_style {
	struct swine_color background;   // a = 0 means transparent
	struct swine_color color;
	char font_family[128];           // comma-separated list as in CSS
	double font_size;                // px
	bool bold;
	bool italic;
	struct css_box padding;
	struct css_box margin;
	struct css_edge border_top, border_right, border_bottom, border_left;
	// "box-shadow: inset 0 -3px #fff" is how Waybar themes underline the
	// focused workspace; it is drawn as an inner bottom (or top) line.
	int inset_bottom;
	int inset_top;
	struct swine_color inset_color;
	int min_width;
	double opacity;
};

// The class an element has while the pointer is over it: ":hover" in a
// selector requires it (no real class can contain a colon).
#define CSS_HOVER ":hover"

// Element being styled, with a link to its parent for descendant matching.
struct css_element {
	const char *type;              // "window", "box", "label", "button"
	const char *id;                // without '#', may be NULL
	const char *const *classes;    // NULL-terminated, may be NULL
	const struct css_element *parent;
};

struct css_stylesheet;

struct css_stylesheet *css_parse(const char *text);
void css_free(struct css_stylesheet *sheet);
// Root defaults: white text, transparent background, 13px sans-serif.
void css_default_style(struct swine_style *style);
// Computes the style of element. Inherited properties (color, font) come
// from parent_style; the rest start from defaults.
void css_compute(const struct css_stylesheet *sheet,
		const struct css_element *element,
		const struct swine_style *parent_style, struct swine_style *out);
bool css_parse_color(const struct css_stylesheet *sheet, const char *text,
		struct swine_color *out);
// Blends a translucent color over an opaque one.
struct swine_color color_blend(struct swine_color over,
		struct swine_color under);

#endif
