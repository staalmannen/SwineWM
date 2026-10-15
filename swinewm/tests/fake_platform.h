#ifndef _SWINE_FAKE_PLATFORM_H
#define _SWINE_FAKE_PLATFORM_H

#include <stdbool.h>
#include "swinewm/tree/node.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/view.h"

// Records what the core asked the "OS" to do.
struct fake_platform_state {
	int geometry_calls;
	int visibility_calls;
	int activate_calls;
	int close_calls;
	int spawn_calls;
	bool quit;
	struct swine_view *last_activated;
	struct swine_view *last_closed;
	char last_spawn[256];
	const char *config_dir;
	int ui_refreshes;
	long long now_ms;
	// sysinfo readings
	bool has_battery;
	int battery_capacity;
	bool battery_charging;
	int cpu_usage;
	int memory_percentage;
};

extern struct fake_platform_state fake;

void fake_reset(void);
// Starts a fresh server with one 1000x800 output (usable area identical).
void fake_setup(void);
// Same, with a second output to the right of the first.
void fake_setup_two_outputs(void);
void fake_teardown(void);
struct swine_view *fake_map_view(const char *title);
struct swine_output *fake_add_output(const char *name, int x, int y, int w, int h);
// Runs a command line, commits, and returns true if every command succeeded.
bool run(const char *command);

#endif
