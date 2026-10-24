#ifndef _SWINE_FORMAT_H
#define _SWINE_FORMAT_H

#include <stdbool.h>
#include <time.h>

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

#endif
