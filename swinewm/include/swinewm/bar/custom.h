#ifndef _SWINE_BAR_CUSTOM_H
#define _SWINE_BAR_CUSTOM_H

#include <stdbool.h>
#include <stdint.h>
#include "list.h"

// The scripts of Waybar's custom/* modules (waybar-custom(5)): "exec" run
// every "interval", once and on a signal, or continuously (one update per
// line), with "exec-if", "restart-interval" and "exec-on-event". The
// platform runs the processes (platform_script_start); this part decides
// when, and parses what they print.

struct json_value;
struct bar_script;

// What a module shows, from its script's last output.
struct bar_script_result {
	bool hidden;          // no output yet, empty output or a failed run
	char *text;
	char *alt;
	char *tooltip;
	list_t *classes;      // char *
	int percentage;
};

// NULL when the module has neither "exec" nor "exec-if" (static text).
struct bar_script *bar_script_create(const struct json_value *cfg,
		const char *output_name);
// Stops the script's process, if it runs.
void bar_script_destroy(struct bar_script *script);
// The last output, parsed per "return-type" and "escape".
void bar_script_result(struct bar_script *script, struct bar_script_result *out);
void bar_script_result_finish(struct bar_script_result *result);
// A click or scroll on the module: run the script again ("exec-on-event").
void bar_script_event(struct bar_script *script);

// Driven by the platform: starts the scripts that are due.
void bar_scripts_tick(long long now_ms);
// When bar_scripts_tick should run next (platform_now_ms), or 0.
long long bar_scripts_next_due(void);
// Waybar's SIGRTMIN+n ("pkill -RTMIN+n waybar"): runs the scripts with
// "signal": n.
void bar_scripts_signal(int signal);
// From the process of platform_script_start, on the main thread: a line it
// printed (without the line break), and its end.
void bar_script_line(uint64_t id, const char *line);
void bar_script_exited(uint64_t id, int status);

// i3blocks-style lines (text, tooltip, class) or one JSON object per line.
void bar_script_parse(const char *output, bool json, bool escape,
		struct bar_script_result *out);

#endif
