#ifndef _SWINE_FAKE_PLATFORM_H
#define _SWINE_FAKE_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>
#include "swinewm/platform.h"
#include "swinewm/grim.h"
#include "swinewm/sysinfo.h"
#include "swinewm/tree/node.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/view.h"

// Records what the core asked the "OS" to do.
struct fake_platform_state {
	int geometry_calls;
	int visibility_calls;
	int activate_calls;
	bool has_visible_owned_window;
	int close_calls;
	int spawn_calls;
	bool quit;
	bool restart;
	struct swine_view *last_activated;
	struct swine_view *last_closed;
	char last_spawn[256];
	int output_configure_calls;
	char last_configured_output[32];
	struct output_display_config last_output_config;
	int screenshot_calls;
	struct swine_box last_screenshot;
	char last_screenshot_path[256];
	bool last_screenshot_clipboard;
	char clipboard_text[256];
	int slurp_calls;
	struct slurp_options last_slurp;   // its choices list is not owned
	const char *config_dir;
	int ui_refreshes;
	long long now_ms;
	// sysinfo readings
	bool has_battery;
	int battery_capacity;
	bool battery_charging;
	int battery_seconds;
	int cpu_usage;
	int memory_percentage;
	// decorations
	int decoration_calls;
	int titlebar_updates;
	int titlebar_count;
	int windows_theme;
	// pointer
	int cursor_x, cursor_y;
	int warp_calls;
	bool drop_indicator_shown;
	struct swine_box drop_indicator;
	int ipc_event_count;
	uint32_t ipc_last_event_type;
	uint32_t ipc_input_capabilities;
	char ipc_last_event[8192];
	// The "change" of every event, in order: "new,focus,"
	char ipc_changes[1024];
	// sysinfo for the network, pulseaudio, disk, temperature, idle_inhibitor
	// and keyboard-state modules
	bool has_disk;
	struct sysinfo_disk disk;
	char last_disk_path[64];
	bool has_temperature;
	double temperature;
	struct sysinfo_network network;
	struct sysinfo_audio audio;
	double last_volume_set;
	bool last_volume_source;
	int mute_calls;
	bool idle_inhibited;
	char power_profile[32];   // "" for none
	bool caps_lock, num_lock, scroll_lock;
	// bar scripts: platform_script_start returns 1, 2, ...
	int script_starts;
	int script_stops;
	uint64_t last_script_id;
	uint64_t last_stopped_script;
	char last_script[256];
	char last_script_output[32];
	bool script_start_fails;
	// keyboard layouts: the first kb_layout_count of fake_platform.c's list
	int kb_layout_count;
	int kb_active;
	int kb_set_calls;
	uint32_t last_closed_notification;
	int last_close_reason;
	uint32_t last_action_notification;
	char last_action[64];
	int swaynag_show_calls;
	bool swaynag_visible;
	char swaynag_details[2048];
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
