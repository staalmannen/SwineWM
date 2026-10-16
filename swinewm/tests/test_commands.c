#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fake_platform.h"
#include "ipc.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/ipc-server.h"
#include "swinewm/notify.h"
#include "test.h"

static void test_chains_and_errors(void) {
	fake_setup();
	fake_map_view("a");
	char *cmd = swine_strdup("splitv; focus left, nop; nonsense; kill");
	list_t *res = execute_command(cmd, NULL);
	free(cmd);
	// Execution stops at the first invalid command.
	CHECK_INT(res->length, 4);
	struct cmd_results *last = res->items[3];
	CHECK_INT(last->status, CMD_INVALID);
	char *json = cmd_results_to_json(res);
	CHECK(strstr(json, "\"success\":false") != NULL);
	CHECK(strstr(json, "Unknown/invalid command 'nonsense'") != NULL);
	free(json);
	free_cmd_results_list(res);
	CHECK_INT(fake.close_calls, 0);
	CHECK(!run("[app_id=foo] kill"));
	CHECK(run("kill"));
	CHECK_INT(fake.close_calls, 1);
	CHECK(run("exit"));
	CHECK(fake.quit);
	fake_teardown();
}

static void test_ipc(void) {
	fake_setup();
	fake_map_view("hello");
	uint32_t type;
	const char *cmd = "workspace 4";
	char *reply = ipc_handle_request(IPC_COMMAND, cmd, (uint32_t)strlen(cmd), &type);
	CHECK_INT(type, IPC_COMMAND);
	CHECK_STR(reply, "[{\"success\":true}]");
	free(reply);

	reply = ipc_handle_request(IPC_GET_WORKSPACES, "", 0, &type);
	CHECK(strstr(reply, "\"name\":\"1\"") != NULL);
	CHECK(strstr(reply, "\"name\":\"4\",\"visible\":true,\"focused\":true") != NULL);
	free(reply);

	reply = ipc_handle_request(IPC_GET_TREE, "", 0, &type);
	CHECK(strstr(reply, "\"title\":\"hello\"") != NULL);
	CHECK(strstr(reply, "__i3_scratch") != NULL);
	free(reply);

	reply = ipc_handle_request(IPC_GET_VERSION, "", 0, &type);
	CHECK(strstr(reply, "\"variant\":\"swinewm\"") != NULL);
	free(reply);

	reply = ipc_handle_request(IPC_GET_BINDING_STATE, "", 0, &type);
	CHECK_STR(reply, "{\"name\":\"default\"}");
	free(reply);

	CHECK(ipc_handle_request(12345, "", 0, &type) == NULL);
	fake_teardown();
}

static void test_every_command_is_found(void) {
	// The handler table is binary-searched; an unsorted entry would make
	// some commands "unknown". Every documented name must resolve.
	static const char *names[] = {
		"allow_tearing", "assign", "bar", "bindcode", "bindgesture",
		"bindswitch", "bindsym", "border", "client.background",
		"client.focused", "client.focused_inactive", "client.focused_tab_title",
		"client.placeholder", "client.unfocused", "client.urgent",
		"default_border", "default_floating_border", "default_orientation",
		"exec", "exec_always", "exit", "explorer_taskbar", "floating", "floating_maximum_size",
		"floating_minimum_size", "floating_modifier", "focus",
		"focus_follows_mouse", "focus_on_window_activation", "focus_wrapping",
		"font", "for_window", "force_display_urgency_hint", "fullscreen",
		"gaps", "grim", "hide_edge_borders", "include", "inhibit_idle", "input",
		"kill", "layout", "makoctl", "max_render_time", "mode", "mouse_warping", "move",
		"new_float", "new_window", "no_focus", "nop", "notify-send", "output",
		"popup_during_fullscreen", "primary_selection", "reload", "resize",
		"scratchpad", "seat", "set", "shortcuts_inhibitor", "show_marks",
		"smart_borders", "smart_gaps", "split", "splith", "splitt", "splitv",
		"swaybg_command", "swaynag_command", "tiling_drag",
		"tiling_drag_threshold", "title_align", "titlebar_border_thickness",
		"titlebar_padding", "unbindsym", "urgent", "workspace",
		"workspace_auto_back_and_forth", "workspace_layout", "xwayland",
	};
	fake_setup();
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
		char *line = swine_strdup(names[i]);
		list_t *res = execute_command(line, NULL);
		struct cmd_results *r = res->items[0];
		bool unknown = r->error && strstr(r->error, "Unknown/invalid") != NULL;
		if (unknown) {
			fprintf(stderr, "  handler not found: %s\n", names[i]);
		}
		CHECK(!unknown);
		free_cmd_results_list(res);
		free(line);
	}
	fake_teardown();
}

// grim is built in: there is no grim on Windows.
static void test_grim(void) {
	fake_setup();
	CHECK(run("grim"));
	CHECK_INT(fake.screenshot_calls, 1);
	CHECK_INT(fake.last_screenshot.width, 1000);
	CHECK_INT(fake.last_screenshot.height, 800);
	const char *path = fake.last_screenshot_path;
	CHECK(strncmp(path, "C:\\Users\\pig\\Pictures\\", 22) == 0);
	size_t len = strlen(path);
	CHECK(len > 9 && strcmp(path + len - 9, "_grim.png") == 0);

	CHECK(run("grim -g \"10,20 300x200\" C:/shot.png"));
	CHECK_INT(fake.last_screenshot.x, 10);
	CHECK_INT(fake.last_screenshot.y, 20);
	CHECK_INT(fake.last_screenshot.width, 300);
	CHECK_STR(fake.last_screenshot_path, "C:/shot.png");
	CHECK(!run("grim -g nonsense"));
	CHECK(!run("grim -o NOSUCH"));
	CHECK(!run("grim -t gif"));
	CHECK(!run("grim -"));
	CHECK_INT(fake.screenshot_calls, 2);

	// exec, and sh-style chains of built-ins, as in sway configs
	CHECK(run("exec grim && notify-send Screenshot \"Saved\""));
	CHECK_INT(fake.screenshot_calls, 3);
	CHECK_INT(fake.spawn_calls, 0);
	CHECK_INT(notify_active()->length, 1);
	CHECK(!run("exec grim -t gif && notify-send never"));
	CHECK_INT(notify_active()->length, 1);
	// A chain with a real program is started as a whole.
	CHECK(run("exec grim && notepad.exe"));
	CHECK_INT(fake.spawn_calls, 1);
	CHECK_INT(fake.screenshot_calls, 3);
	fake_teardown();
}

void test_commands_all(void) {
	test_every_command_is_found();
	test_chains_and_errors();
	test_ipc();
	test_grim();
}
