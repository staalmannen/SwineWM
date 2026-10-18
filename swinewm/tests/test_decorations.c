// Decorations: border, default_border, client.* colours, smart_borders and
// opacity, as worked out by view_update_decoration (compare with sway's
// container_get_current_colors and view_autoconfigure).
#include <stdlib.h>
#include <string.h>
#include "fake_platform.h"
#include "stringop.h"
#include "swinewm/config.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/ipc-json.h"
#include "swinewm/input/seat.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/view.h"
#include "swinewm/tree/workspace.h"
#include "test.h"

static struct swine_view *map_floating(const char *title) {
	static int next_handle = 0x900;
	struct swine_view *view = view_create((void *)(size_t)next_handle++);
	view->title = swine_strdup(title);
	view->app_id = swine_strdup("test.exe");
	view->natural_floating = true;
	view->natural = (struct swine_box){100, 100, 300, 200};
	view_map(view);
	transaction_commit_dirty();
	return view;
}

static void test_border_defaults(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	struct swine_view *b = fake_map_view("b");
	// sway's defaults: "normal" (a title bar) and the client.* colours.
	CHECK_INT(b->container->border, B_NORMAL);
	CHECK_INT(b->container->border_thickness, 2);
	CHECK(b->current.deco.titlebar);
	CHECK_INT(b->current.deco.border_color, 0x285577FF);   // focused
	CHECK_INT(b->current.deco.title_color, 0x285577FF);
	CHECK_INT(b->current.deco.title_text_color, 0xFFFFFFFF);
	CHECK_INT(a->current.deco.border_color, 0x222222FF);   // unfocused
	CHECK_INT(a->current.deco.title_text_color, 0x888888FF);
	CHECK(a->current.deco.square_corners);
	CHECK(a->current.deco.alpha == 1.0f);
	// Moving the focus recolours both.
	CHECK(run("focus left"));
	CHECK_INT(a->current.deco.border_color, 0x285577FF);
	CHECK_INT(b->current.deco.border_color, 0x222222FF);
	// Nothing changed: the platform is not asked again.
	int calls = fake.decoration_calls;
	transaction_commit_dirty();
	CHECK_INT(fake.decoration_calls, calls);

	// default_border applies to windows mapped afterwards.
	CHECK(run("default_border pixel 3"));
	struct swine_view *c = fake_map_view("c");
	CHECK_INT(c->container->border, B_PIXEL);
	CHECK_INT(c->container->border_thickness, 3);
	CHECK(!c->current.deco.titlebar);
	CHECK_INT(c->current.deco.title_color, 0);
	CHECK_INT(a->container->border, B_NORMAL);
	// ...and default_floating_border to windows that start floating.
	CHECK(run("default_floating_border none"));
	struct swine_view *d = map_floating("d");
	CHECK_INT(d->container->border, B_NONE);
	CHECK_INT(d->current.deco.border_color, 0);
	CHECK(!d->current.deco.square_corners);
	CHECK(!run("default_border thick"));

	char *tree = ipc_json_get_tree();
	CHECK(strstr(tree, "\"border\":\"pixel\",\"current_border_width\":3") != NULL);
	CHECK(strstr(tree, "\"border\":\"none\",\"current_border_width\":0") != NULL);
	free(tree);
	fake_teardown();
}

static void test_border_command(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	CHECK(run("border pixel 1"));
	CHECK_INT(a->container->border, B_PIXEL);
	CHECK_INT(a->container->border_thickness, 1);
	CHECK(!a->current.deco.titlebar);
	CHECK_INT(a->current.deco.border_color, 0x285577FF);
	CHECK(run("border none"));
	CHECK_INT(a->current.deco.border_color, 0);
	// toggle: none -> pixel -> normal -> none (no CSD on Windows)
	CHECK(run("border toggle"));
	CHECK_INT(a->container->border, B_PIXEL);
	CHECK(run("border toggle"));
	CHECK_INT(a->container->border, B_NORMAL);
	CHECK(a->current.deco.titlebar);
	CHECK(run("border toggle"));
	CHECK_INT(a->container->border, B_NONE);
	CHECK(!run("border csd"));
	CHECK(!run("border wide"));
	// Only views have borders.
	CHECK(run("border normal; focus parent"));
	CHECK(!run("border pixel"));
	// Fullscreen windows have no border.
	CHECK(run("focus child; fullscreen enable"));
	CHECK_INT(a->current.deco.border_color, 0);
	CHECK(a->current.deco.square_corners);
	fake_teardown();
}

static void test_client_colors(void) {
	fake_setup_two_outputs();
	struct swine_view *a = fake_map_view("a");
	CHECK(run("client.focused #ff0000 #00ff00 #0000ff"));
	// child_border defaults to the background, as in sway.
	CHECK_INT(config->border_colors.focused.child_border, 0x00FF00FF);
	CHECK_INT(config->border_colors.focused.indicator, 0x2E9EF4FF);
	CHECK(run("client.focused #ff0000 #00ff00 #0000ff #123456 #abcdef80"));
	CHECK_INT(config->border_colors.focused.child_border, 0xABCDEF80);
	CHECK_INT(config->border_colors.focused.indicator, 0x123456FF);
	transaction_commit_dirty();
	CHECK_INT(a->current.deco.border_color, 0xABCDEF80);
	CHECK_INT(a->current.deco.title_color, 0x00FF00FF);
	CHECK_INT(a->current.deco.title_text_color, 0x0000FFFF);
	CHECK(!run("client.focused #ff0000 #00ff00"));
	CHECK(!run("client.unfocused #ff0000 #00ff00 nocolor"));
	CHECK(run("client.focused_tab_title #111111 #222222 #333333"));
	CHECK(config->has_focused_tab_title);
	CHECK(run("client.placeholder #000000 #0c0c0c #ffffff"));
	CHECK(run("client.background #ffffff"));

	// The window last focused on the other output is focused_inactive.
	CHECK(run("focus output right"));
	transaction_commit_dirty();
	CHECK_INT(a->current.deco.border_color,
		config->border_colors.focused_inactive.child_border);
	CHECK(run("focus output left"));
	// Urgent windows use client.urgent.
	struct swine_view *b = fake_map_view("b");
	(void)b;
	view_set_urgent(a, true);
	transaction_commit_dirty();
	CHECK_INT(a->current.deco.border_color, 0x900000FF);
	fake_teardown();
}

static void test_smart_borders(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	CHECK(run("smart_borders on"));
	transaction_commit_dirty();
	CHECK_INT(a->current.deco.border_color, 0);
	CHECK(a->current.deco.titlebar);   // only the border goes
	struct swine_view *b = fake_map_view("b");
	CHECK_INT(a->current.deco.border_color, 0x222222FF);
	CHECK_INT(b->current.deco.border_color, 0x285577FF);
	// In a tabbed container the hidden tabs do not count.
	CHECK(run("layout tabbed"));
	CHECK_INT(b->current.deco.border_color, 0);
	CHECK(run("layout splith"));
	// no_gaps: only while the workspace has no gaps.
	view_unmap(b);
	view_destroy(b);
	CHECK(run("smart_borders no_gaps"));
	transaction_commit_dirty();
	CHECK_INT(a->current.deco.border_color, 0);
	CHECK(run("gaps inner all set 10"));
	CHECK_INT(a->current.deco.border_color, 0x285577FF);
	CHECK(run("smart_borders off"));
	CHECK_INT(config->hide_edge_borders_smart, ESMART_OFF);
	CHECK(run("hide_edge_borders smart"));
	CHECK_INT(config->hide_edge_borders_smart, ESMART_ON);
	CHECK(run("hide_edge_borders --i3 both"));
	CHECK_INT(config->hide_edge_borders, E_BOTH);
	CHECK(config->hide_lone_tab);
	CHECK(!run("hide_edge_borders --i3"));
	CHECK(!run("hide_edge_borders sideways"));
	fake_teardown();
}

static void test_opacity(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	CHECK(run("opacity 0.5"));
	CHECK(a->current.deco.alpha == 0.5f);
	CHECK(run("opacity plus 0.25"));
	CHECK(a->container->alpha == 0.75f);
	CHECK(run("opacity minus 0.5"));
	CHECK(a->container->alpha == 0.25f);
	CHECK(run("opacity set 1"));
	CHECK(a->current.deco.alpha == 1.0f);
	CHECK(!run("opacity 1.5"));
	CHECK(!run("opacity minus 2"));
	CHECK(!run("opacity half"));
	CHECK(!run("opacity times 0.5"));
	CHECK(run("[title=\"^a$\"] opacity 0.9"));
	CHECK(a->container->alpha == 0.9f);
	fake_teardown();
}

static void test_font_and_titlebar(void) {
	fake_setup();
	CHECK(run("font pango:DejaVu Sans Mono 11"));
	CHECK_STR(config->font, "pango:DejaVu Sans Mono 11");
	CHECK(run("titlebar_padding 6 3"));
	CHECK_INT(config->titlebar_h_padding, 6);
	CHECK_INT(config->titlebar_v_padding, 3);
	CHECK(run("titlebar_padding 8"));
	CHECK_INT(config->titlebar_v_padding, 8);
	CHECK(run("titlebar_border_thickness 2"));
	CHECK_INT(config->titlebar_border_thickness, 2);
	CHECK(!run("titlebar_border_thickness 20"));   // more than the padding
	CHECK(run("title_align center"));
	CHECK_INT(config->title_align, 1);
	CHECK(!run("title_align middle"));
	fake_teardown();
}

void test_decorations_all(void) {
	test_border_defaults();
	test_border_command();
	test_client_colors();
	test_smart_borders();
	test_opacity();
	test_font_and_titlebar();
}
