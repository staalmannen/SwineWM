#include <stdint.h>
#include <stdio.h>
#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#endif
#include <stdlib.h>
#include <string.h>
#include "fake_platform.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/input/cursor.h"
#include "swinewm/platform.h"
#include "swinewm/server.h"
#include "swinewm/sysinfo.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/view.h"

struct fake_platform_state fake;

void fake_reset(void) {
	memset(&fake, 0, sizeof(fake));
	fake.ipc_input_capabilities =
		PLATFORM_IPC_INPUT_KEYBOARD | PLATFORM_IPC_INPUT_POINTER;
}
void platform_swaynag_show(const char *details) {
	fake.swaynag_show_calls++;
	fake.swaynag_visible = details != NULL;
	snprintf(fake.swaynag_details, sizeof(fake.swaynag_details), "%s",
		details ? details : "");
}

void platform_setup_environment(void) {}
void platform_set_shell_mode(int mode) { (void)mode; }
bool platform_is_shell(void) { return false; }
int platform_set_login_shell(bool enable) { (void)enable; return 0; }
int platform_set_autostart(bool enable) { (void)enable; return 0; }
bool platform_init(void) { return true; }
int platform_run(void) { return 0; }
void platform_quit(void) { fake.quit = true; }
void platform_restart(void) { fake.restart = true; }
void platform_finish(void) {}
void platform_ipc_event(uint32_t type, const char *payload) {
	fake.ipc_event_count++;
	fake.ipc_last_event_type = type;
	snprintf(fake.ipc_last_event, sizeof(fake.ipc_last_event), "%s",
		payload ? payload : "");
	const char *change = payload ? strstr(payload, "\"change\":\"") : NULL;
	if (change) {
		change += strlen("\"change\":\"");
		size_t used = strlen(fake.ipc_changes);
		size_t len = strcspn(change, "\"");
		if (used + len + 2 < sizeof(fake.ipc_changes)) {
			memcpy(fake.ipc_changes + used, change, len);
			fake.ipc_changes[used + len] = ',';
			fake.ipc_changes[used + len + 1] = '\0';
		}
	}
}
uint32_t platform_ipc_input_capabilities(void) {
	return fake.ipc_input_capabilities;
}
void platform_config_changed(void) {}
int platform_recover_windows(void) { return 0; }
int platform_watch(unsigned long process_id, bool shell_mode) {
	(void)process_id;
	(void)shell_mode;
	return 0;
}

void platform_view_set_geometry(struct swine_view *view,
		const struct swine_box *box) {
	(void)view;
	(void)box;
	fake.geometry_calls++;
}

void platform_view_set_visible(struct swine_view *view, bool visible) {
	(void)view;
	(void)visible;
	fake.visibility_calls++;
}

void platform_view_set_decoration(struct swine_view *view,
		const struct view_decoration *deco) {
	(void)view;
	(void)deco;
	fake.decoration_calls++;
}

void platform_titlebars_update(list_t *titlebars) {
	fake.titlebar_updates++;
	fake.titlebar_count = titlebars->length;
}

void platform_set_windows_theme(int mode) {
	fake.windows_theme = mode;
}

int platform_font_height(const char *font) {
	(void)font;
	return 15;
}

bool platform_view_activate(struct swine_view *view) {
	fake.activate_calls++;
	fake.last_activated = view;
	return true;
}

bool platform_view_has_visible_owned_window(struct swine_view *view) {
	(void)view;
	return fake.has_visible_owned_window;
}

uint64_t platform_script_start(const char *cmdline, const char *output_name) {
	if (fake.script_start_fails) {
		return 0;
	}
	fake.script_starts++;
	snprintf(fake.last_script, sizeof(fake.last_script), "%s", cmdline);
	snprintf(fake.last_script_output, sizeof(fake.last_script_output), "%s",
		output_name ? output_name : "");
	fake.last_script_id = (uint64_t)fake.script_starts;
	return fake.last_script_id;
}
void platform_script_stop(uint64_t id) {
	fake.script_stops++;
	fake.last_stopped_script = id;
}
int platform_keyboard_layouts(list_t *layouts) {
	static const char *const table[][4] = {
		{"English (United States)", "us", "en", ""},
		{"Swedish (Sweden)", "se", "sv", ""},
		{"English (United States, Dvorak)", "us", "en", "Dvorak"},
	};
	for (int i = 0; i < fake.kb_layout_count && i < 3; ++i) {
		struct platform_keyboard_layout *l = calloc(1, sizeof(*l));
		l->name = swine_strdup(table[i][0]);
		l->short_name = swine_strdup(table[i][1]);
		l->description = swine_strdup(table[i][2]);
		l->variant = swine_strdup(table[i][3]);
		list_add(layouts, l);
	}
	return fake.kb_layout_count ? fake.kb_active : -1;
}
void platform_keyboard_set_layout(int index) {
	fake.kb_set_calls++;
	fake.kb_active = index;
}
void platform_queue_command(const char *command) {
	run(command);
}

bool platform_cursor_position(int *x, int *y) {
	*x = fake.cursor_x;
	*y = fake.cursor_y;
	return true;
}

void platform_cursor_warp(int x, int y) {
	fake.cursor_x = x;
	fake.cursor_y = y;
	fake.warp_calls++;
}

void platform_drop_indicator(const struct swine_box *box) {
	fake.drop_indicator_shown = box != NULL;
	if (box) {
		fake.drop_indicator = *box;
	}
}

void platform_view_close(struct swine_view *view) {
	fake.close_calls++;
	fake.last_closed = view;
}

bool platform_spawn(const char *cmdline) {
	fake.spawn_calls++;
	snprintf(fake.last_spawn, sizeof(fake.last_spawn), "%s", cmdline);
	return true;
}

list_t *platform_config_paths(const char *name) {
	list_t *paths = create_list();
	if (fake.config_dir) {
		list_add(paths, format_str("%s/%s", fake.config_dir, name));
	}
	return paths;
}

void platform_ui_refresh(void) {
	fake.ui_refreshes++;
}

char *platform_pictures_dir(void) {
	return swine_strdup("C:\\Users\\pig\\Pictures");
}

bool platform_output_configure(struct swine_output *output,
		const struct output_display_config *cfg) {
	// Only calls that change something are interesting to tests.
	if (cfg->any) {
		fake.output_configure_calls++;
		snprintf(fake.last_configured_output,
			sizeof(fake.last_configured_output), "%s", output->name);
		fake.last_output_config = *cfg;
	}
	return true;
}

bool platform_screenshot(const struct swine_box *box, const char *path,
		bool jpeg, char **error) {
	(void)jpeg;
	(void)error;
	fake.screenshot_calls++;
	fake.last_screenshot = *box;
	snprintf(fake.last_screenshot_path, sizeof(fake.last_screenshot_path),
		"%s", path);
	return true;
}

long long platform_now_ms(void) {
	return fake.now_ms;
}

// The real directory, for include tests (with test files on disk).
list_t *platform_list_dir(const char *dir) {
	list_t *names = NULL;
#ifdef _WIN32
	char *pattern = format_str("%s\\*", dir);
	struct _finddata_t data;
	intptr_t find = _findfirst(pattern, &data);
	free(pattern);
	if (find == -1) {
		return NULL;
	}
	names = create_list();
	do {
		if (strcmp(data.name, ".") != 0 && strcmp(data.name, "..") != 0) {
			list_add(names, swine_strdup(data.name));
		}
	} while (_findnext(find, &data) == 0);
	_findclose(find);
#else
	DIR *d = opendir(dir);
	if (!d) {
		return NULL;
	}
	names = create_list();
	struct dirent *entry;
	while ((entry = readdir(d))) {
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
			list_add(names, swine_strdup(entry->d_name));
		}
	}
	closedir(d);
#endif
	return names;
}

// Expands only %SWINEWM_DATA%, to where an installed swinewm.exe would be.
char *platform_expand_path(const char *path) {
	const char *var = "%SWINEWM_DATA%";
	const char *at = strstr(path, var);
	if (!at) {
		return swine_strdup(path);
	}
	return format_str("%.*sC:\\Program Files\\SwineWM%s", (int)(at - path),
		path, at + strlen(var));
}

bool sysinfo_battery(struct sysinfo_battery *out) {
	if (!fake.has_battery) {
		return false;
	}
	out->capacity = fake.battery_capacity;
	out->charging = fake.battery_charging;
	out->plugged = fake.battery_charging;
	return true;
}

bool sysinfo_memory(struct sysinfo_memory *out) {
	out->total_gib = 16;
	out->used_gib = 16 * fake.memory_percentage / 100.0;
	out->avail_gib = out->total_gib - out->used_gib;
	out->percentage = fake.memory_percentage;
	return true;
}

bool sysinfo_cpu(int *usage) {
	*usage = fake.cpu_usage;
	return true;
}

struct swine_output *fake_add_output(const char *name, int x, int y, int w, int h) {
	struct swine_output *output = output_create(name, NULL);
	output->box = (struct swine_box){x, y, w, h};
	output->usable = output->box;
	output_enable(output);
	return output;
}

static void setup_common(void) {
	fake_reset();
	cursor_reset();
	server_init();
	config = config_create();
	config->active = true;
}

void fake_setup(void) {
	setup_common();
	fake_add_output("DISPLAY1", 0, 0, 1000, 800);
}

void fake_setup_two_outputs(void) {
	setup_common();
	fake_add_output("DISPLAY1", 0, 0, 1000, 800);
	fake_add_output("DISPLAY2", 1000, 0, 1000, 800);
}

void fake_teardown(void) {
	server_fini();
	free_config(config);
	config = NULL;
}

struct swine_view *fake_map_view(const char *title) {
	static int next_handle = 0x100;
	struct swine_view *view = view_create((void *)(size_t)next_handle++);
	view->title = swine_strdup(title);
	view->app_id = swine_strdup("test.exe");
	view_map(view);
	transaction_commit_dirty();
	return view;
}

bool run(const char *command) {
	char *copy = swine_strdup(command);
	list_t *res_list = execute_command(copy, NULL);
	free(copy);
	bool ok = res_list->length > 0;
	for (int i = 0; i < res_list->length; ++i) {
		struct cmd_results *res = res_list->items[i];
		if (res->status != CMD_SUCCESS) {
			fprintf(stderr, "  command '%s' failed: %s\n", command,
				res->error ? res->error : "");
			ok = false;
		}
	}
	free_cmd_results_list(res_list);
	transaction_commit_dirty();
	return ok;
}
