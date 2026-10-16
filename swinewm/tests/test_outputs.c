#include <stdlib.h>
#include <string.h>
#include "fake_platform.h"
#include "stringop.h"
#include "swinewm/config.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/input/seat.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/view.h"
#include "swinewm/tree/workspace.h"
#include "test.h"

static bool config_lines(const char *text) {
	config->active = false;
	config->reading = true;
	bool ok = read_config_text(text, "test");
	config->reading = false;
	config->active = true;
	return ok;
}

static struct swine_workspace *focused_ws(void) {
	return seat_get_focused_workspace();
}

static void test_workspace_output(void) {
	fake_setup_two_outputs();
	struct swine_output *right = output_by_name("DISPLAY2");
	right->identifier = swine_strdup("DELL U2720Q");
	CHECK(config_lines(
		"workspace 3 output DISPLAY2\n"
		"workspace 4 output NOSUCH display2\n"
		"workspace 5 output \"DELL U2720Q\"\n"
		"workspace 6 output NOSUCH\n"
		"workspace 7 gaps inner 20\n"
		"workspace 7 gaps outer 5\n"));
	CHECK(!config_lines("workspace output DISPLAY2\n"));
	CHECK(!config_lines("workspace 8 gaps sideways 2\n"));
	CHECK(run("workspace 3"));
	CHECK(focused_ws()->output == right);
	CHECK(run("workspace 1"));
	CHECK(run("workspace 4"));
	CHECK(focused_ws()->output == right);
	CHECK(run("workspace 1; workspace 5"));
	CHECK(focused_ws()->output == right);
	// No assigned output connected: the focused output, like sway.
	CHECK(run("workspace 1; workspace 6"));
	CHECK_STR(focused_ws()->output->name, "DISPLAY1");
	CHECK(run("workspace 7"));
	CHECK_INT(focused_ws()->gaps_inner, 20);
	CHECK_INT(focused_ws()->gaps_outer.left, 5);
	// Moving a window to an assigned workspace creates it there.
	CHECK(run("workspace 1"));
	struct swine_view *a = fake_map_view("a");
	CHECK(run("workspace 2"));   // on DISPLAY2 (focus followed workspace 5)
	CHECK(run("workspace 1"));
	CHECK(run("[title=\"^a$\"] move to workspace 9"));
	CHECK_STR(a->container->workspace->output->name, "DISPLAY1");
	CHECK(config_lines("workspace 10 output DISPLAY2\n"));
	CHECK(run("[title=\"^a$\"] move to workspace 10"));
	CHECK(a->container->workspace->output == right);
	CHECK(output_by_name_or_id("dell u2720q") == right);
	fake_teardown();
}

static void test_next_name(void) {
	fake_setup();
	CHECK(config_lines(
		"bindsym Mod4+1 workspace number 1:web\n"
		"bindsym Mod4+2 workspace mail\n"
		"workspace mail output DISPLAY2\n"
		"workspace chat output DISPLAY3\n"));
	// The first output got "1" before the bindings existed. A new output
	// gets the first bound workspace that may live there.
	struct swine_output *two = fake_add_output("DISPLAY2", 1000, 0, 1000, 800);
	CHECK_STR(((struct swine_workspace *)two->active_workspace)->name, "mail");
	struct swine_output *three = fake_add_output("DISPLAY3", 2000, 0, 1000, 800);
	CHECK_STR(three->active_workspace->name, "chat");
	fake_teardown();
}

static void test_move_workspace_to_output(void) {
	fake_setup_two_outputs();
	struct swine_view *a = fake_map_view("a");
	struct swine_workspace *ws1 = a->container->workspace;
	CHECK(!run("move workspace to output nowhere"));
	CHECK(run("move workspace to output right"));
	CHECK_STR(ws1->output->name, "DISPLAY2");
	CHECK(ws1->output->active_workspace == ws1);
	CHECK(seat_get_focused_container() == a->container);
	CHECK_INT(a->pending.box.x, 1000);
	// The old output got a fresh workspace; the empty one there was dropped.
	struct swine_output *left = output_by_name("DISPLAY1");
	CHECK_INT(left->workspaces->length, 1);
	CHECK(left->active_workspace != NULL);
	CHECK_INT(output_by_name("DISPLAY2")->workspaces->length, 1);
	CHECK(run("move workspace to DISPLAY1"));
	CHECK_STR(ws1->output->name, "DISPLAY1");
	CHECK(run("move workspace output current"));
	CHECK_STR(ws1->output->name, "DISPLAY1");
	fake_teardown();
}

static void test_hotplug(void) {
	fake_setup_two_outputs();
	CHECK(run("focus output right"));
	struct swine_view *b = fake_map_view("b");
	struct swine_workspace *ws2 = b->container->workspace;
	CHECK_STR(ws2->output->name, "DISPLAY2");
	CHECK(run("focus output left"));
	struct swine_view *a = fake_map_view("a");

	// Unplugging moves the workspace to the remaining output...
	output_destroy(output_by_name("DISPLAY2"));
	transaction_commit_dirty();
	CHECK_STR(ws2->output->name, "DISPLAY1");
	CHECK(a->pending.visible);
	CHECK(!b->pending.visible);
	// ...and plugging the monitor back in returns it.
	struct swine_output *two = fake_add_output("DISPLAY2", 1000, 0, 1000, 800);
	transaction_commit_dirty();
	CHECK(ws2->output == two);
	CHECK(two->active_workspace == ws2);
	CHECK(b->pending.visible);
	CHECK_INT(b->pending.box.x, 1000);
	CHECK(seat_get_focused_container() == a->container);

	// A workspace moved by hand prefers its new output from then on.
	CHECK(run("focus output right; move workspace to output left"));
	CHECK_STR(ws2->output->name, "DISPLAY1");
	output_destroy(output_by_name("DISPLAY2"));
	fake_add_output("DISPLAY2", 1000, 0, 1000, 800);
	CHECK_STR(ws2->output->name, "DISPLAY1");
	fake_teardown();
}

static void test_output_display_config(void) {
	fake_setup_two_outputs();
	struct swine_output *left = output_by_name("DISPLAY1");
	struct swine_output *right = output_by_name("DISPLAY2");
	left->primary = true;
	right->identifier = swine_strdup("DELL U2720Q");
	config->active = false;
	config->reading = true;
	CHECK(read_config_text(
		"output * bg #000000 solid_color\n"
		"output DISPLAY1 mode 2560x1440@143.912Hz position 1000,0\n"
		"output \"DELL U2720Q\" {\n"
		"    resolution 3840 2160\n"
		"    pos 4560 0 transform 90 scale 1.5\n"
		"    adaptive_sync on subpixel rgb color_profile icc x.icc\n"
		"}\n", "test"));
	CHECK(!read_config_text("output DISPLAY1 mode 1920y1080\n", "x"));
	CHECK(!read_config_text("output DISPLAY1 mode 1920x1080@60\n", "x"));
	CHECK(!read_config_text("output DISPLAY1 position 10\n", "x"));
	CHECK(!read_config_text("output DISPLAY1 transform 45\n", "x"));
	CHECK(!read_config_text("output DISPLAY1 frobnicate\n", "x"));
	CHECK(!read_config_text("output DISPLAY1 transform 90 clockwise\n", "x"));
	config->reading = false;
	config->active = true;

	fake.output_configure_calls = 0;
	output_apply_configs();
	CHECK_INT(fake.output_configure_calls, 2);
	CHECK_STR(fake.last_configured_output, "DISPLAY2");
	struct output_display_config *c = &fake.last_output_config;
	CHECK_INT(c->width, 3840);
	CHECK_INT(c->height, 2160);
	CHECK_INT(c->transform, 1);
	// Windows keeps the primary at 0,0: positions shift with it.
	CHECK_INT(c->x, 3560);
	CHECK_INT(c->y, 0);

	// The background of "*" still applies to an output whose own config
	// sets no background.
	CHECK_STR(output_config_for_name("DISPLAY1")->name, "*");

	// At runtime the change is applied at once; relative transforms.
	right->transform = 1;
	CHECK(run("output DISPLAY2 transform 90 anticlockwise"));
	CHECK_INT(fake.last_output_config.transform, 0);
	CHECK(run("output DISPLAY2 transform 180 clockwise"));
	CHECK_INT(fake.last_output_config.transform, 3);
	CHECK(!run("output * transform 90 clockwise"));
	fake.output_configure_calls = 0;
	CHECK(run("output DISPLAY1 bg #112233 solid_color"));
	CHECK_INT(fake.output_configure_calls, 0);
	fake_teardown();
}

void test_outputs_all(void) {
	test_output_display_config();
	test_workspace_output();
	test_next_name();
	test_move_workspace_to_output();
	test_hotplug();
}
