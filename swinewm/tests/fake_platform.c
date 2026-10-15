#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fake_platform.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/platform.h"
#include "swinewm/server.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/view.h"

struct fake_platform_state fake;

void fake_reset(void) {
	memset(&fake, 0, sizeof(fake));
}

bool platform_init(void) { return true; }
int platform_run(void) { return 0; }
void platform_quit(void) { fake.quit = true; }
void platform_finish(void) {}
void platform_config_changed(void) {}
int platform_recover_windows(void) { return 0; }

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

bool platform_view_activate(struct swine_view *view) {
	fake.activate_calls++;
	fake.last_activated = view;
	return true;
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

list_t *platform_config_paths(void) {
	list_t *paths = create_list();
	if (fake.config_dir) {
		list_add(paths, format_str("%s/config", fake.config_dir));
	}
	return paths;
}

char *platform_expand_path(const char *path) {
	return swine_strdup(path);
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
