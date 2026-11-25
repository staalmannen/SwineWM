#ifndef _SWINE_FORMAT_H
#define _SWINE_FORMAT_H

#include <stdbool.h>
#include <time.h>
#include "list.h"
#include "swinewm/bar/css.h"

// Waybar format strings use {fmt} syntax: "{name}", "{}" (first argument),
// "{used:0.1f}" (number with a printf-like spec), "{:%H:%M}" (strftime on
// the time argument), "{{" and "}}" for literal braces.
// How an argument is formatted. FMT_POW_* are Waybar's pow_format: num
// scaled with an SI or binary prefix and str as the unit ("2.5GiB",
// "1.2Mb/s"), with its {:>} {:=} {:k} {:u} {:i} ... specs.
enum fmt_type {
	FMT_STRING = 0,          // str
	FMT_NUMBER = 1,          // num
	FMT_POW,                 // decimal prefixes (k, M, G)
	FMT_POW_BINARY,          // binary prefixes (Ki, Mi, Gi)
	FMT_POW_SKIP_DECIMAL,    // no ".0" on exact values (linkSpeed)
	FMT_POW_COMPACT,         // no decimals below M (bandwidth*Compact)
};

struct fmt_arg {
	const char *name;
	int type;                // enum fmt_type; true/false: number/string
	const char *str;
	double num;
};

// Returns a newly allocated string. tm may be NULL.
char *format_waybar(const char *fmt, const struct fmt_arg *args, int nargs,
		const struct tm *tm);

// Escapes text for Pango markup (&amp; &lt; &gt; &quot; &apos;).
char *markup_escape(const char *text);

// Removes Pango markup tags and decodes the XML entities Pango accepts.
char *markup_strip(const char *text);

// A run of text with one set of Pango attributes.
struct markup_span {
	char *text;
	bool bold, italic, underline, strikethrough;
	bool has_color;              // foreground=, color=
	struct swine_color color;
	int alpha;                   // alpha=, fgalpha= (0-255) of the colour
	                             // (the label's without color=), or -1
	bool has_background;         // background=
	struct swine_color background;
	double scale;                // of the label's font size (<big>, size=large)
	double size_pt;              // an absolute size (size=, font=), or 0
	char font_family[64];        // font_family=, face=, font=; "" = the label's
	bool monospace;              // <tt>
};

// Pango markup as runs: <b>, <i>, <u>, <s>, <big>, <small>, <tt>, <sub>,
// <sup> and <span> with weight, style, underline, strikethrough,
// foreground (color), background, alpha, size, font_family (face) and
// font; the entities decoded. Other tags and attributes are ignored,
// their text kept. Returns struct markup_span * items, merged where the
// attributes are the same; free with markup_spans_free.
list_t *markup_spans(const char *text);
void markup_spans_free(list_t *spans);
// A span of plain text (no attributes). Takes a copy of text.
struct markup_span *markup_span_create(const char *text);
// Whether a span has no attributes: it looks like plain text.
bool markup_span_is_plain(const struct markup_span *span);
// The spans' text, without markup. Caller frees.
char *markup_spans_text(const list_t *spans);
// Keeps the first max_chars characters (UTF-8) and adds U+2026 (...) if
// anything was cut, as Waybar's max-length does. max_chars <= 0: all.
void markup_spans_truncate(list_t *spans, int max_chars);

#endif
