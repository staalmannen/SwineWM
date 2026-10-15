#ifndef _SWINE_FORMAT_H
#define _SWINE_FORMAT_H

#include <stdbool.h>
#include <time.h>

// Waybar format strings use {fmt} syntax: "{name}", "{}" (first argument),
// "{used:0.1f}" (number with a printf-like spec), "{:%H:%M}" (strftime on
// the time argument), "{{" and "}}" for literal braces.
struct fmt_arg {
	const char *name;
	bool is_number;
	const char *str;
	double num;
};

// Returns a newly allocated string. tm may be NULL.
char *format_waybar(const char *fmt, const struct fmt_arg *args, int nargs,
		const struct tm *tm);

// Removes Pango markup tags and decodes the XML entities Pango accepts.
char *markup_strip(const char *text);

#endif
