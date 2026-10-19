// Decorations: border, default_border, client.* colours, smart_borders and
// opacity, as worked out by view_update_decoration (compare with sway's
// container_get_current_colors and view_autoconfigure).
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fake_platform.h"
#include "stringop.h"
#include "swinewm/config.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/input/cursor.h"
#include "swinewm/ipc-json.h"
#include "swinewm/input/seat.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/titlebar.h"
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

static void test_gap_bounds(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	int max_gap = INT_MAX / 4;
	char command[64];
	CHECK(!run("gaps inner 2147483647"));
	CHECK(!run("gaps inner 999999999999999999999999"));
	snprintf(command, sizeof(command), "gaps inner current set %d", max_gap);
	CHECK(run(command));
	CHECK_INT(a->container->workspace->gaps_inner, max_gap);
	CHECK(!run("gaps inner current plus 1"));
	CHECK_INT(a->container->workspace->gaps_inner, max_gap);
	snprintf(command, sizeof(command), "gaps left current set %d", max_gap);
	CHECK(run(command));
	CHECK(!run("gaps left current plus 1"));
	CHECK_INT(a->container->workspace->gaps_outer.left, max_gap);

	struct swine_workspace *first = a->container->workspace;
	CHECK(run("workspace 2"));
	struct swine_view *b = fake_map_view("b");
	struct swine_workspace *second = b->container->workspace;
	first->gaps_inner = max_gap - 1;
	second->gaps_inner = max_gap;
	CHECK(!run("gaps inner all plus 1"));
	CHECK_INT(first->gaps_inner, max_gap - 1);
	CHECK_INT(second->gaps_inner, max_gap);
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

static void test_titlebars(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	struct swine_view *b = fake_map_view("b");
	CHECK_INT(fake.titlebar_count, 0);
	CHECK(b->current.deco.titlebar);
	CHECK(run("layout tabbed"));
	// One row of tabs on top (font height 15 + 2 * titlebar_v_padding 4).
	CHECK_INT(container_titlebar_height(), 23);
	CHECK_INT(b->pending.box.y, 23);
	CHECK_INT(b->pending.box.height, 800 - 23);
	CHECK_INT(fake.titlebar_count, 1);
	list_t *bars = titlebars_get();
	struct swine_titlebar *bar = bars->items[0];
	CHECK(!bar->stacked);
	CHECK_INT(bar->box.height, 23);
	CHECK_INT(bar->box.width, 1000);
	CHECK_INT(bar->tabs->length, 2);
	struct swine_tab *ta = bar->tabs->items[0], *tb = bar->tabs->items[1];
	CHECK_STR(ta->title, "a");
	CHECK(tb->con == b->container);
	CHECK_INT(tb->colors.background, 0x285577FF);   // focused
	CHECK_INT(ta->colors.background, 0x222222FF);   // unfocused
	// The tab is the title bar: the windows lose their own.
	CHECK(!b->current.deco.titlebar);
	// Stacked: one row per child.
	CHECK(run("layout stacking"));
	bar = titlebars_get()->items[0];
	CHECK(bar->stacked);
	CHECK_INT(bar->box.height, 46);
	CHECK_INT(bar->tab_height, 23);
	CHECK_INT(b->pending.box.y, 46);
	// A split container's tab shows its representation, as in sway.
	CHECK(run("layout tabbed; split v"));
	struct swine_view *c = fake_map_view("c");
	(void)c;
	bar = titlebars_get()->items[0];
	CHECK_STR(((struct swine_tab *)bar->tabs->items[1])->title,
		"V[test.exe test.exe]");
	// title_format, as in sway (without Pango markup).
	CHECK(run("[title=\"^a$\"] title_format \"%app_id: %title %shell 100%\""));
	bar = titlebars_get()->items[0];
	CHECK_STR(((struct swine_tab *)bar->tabs->items[0])->title,
		"test.exe: a win32 100%");
	CHECK(!run("title_format"));
	// Its children have title bars of their own (vertical split).
	CHECK(b->current.deco.titlebar);
	// Clicking a tab focuses its window, scrolling switches tabs.
	bar = titlebars_get()->items[0];
	struct swine_box ab = bar->box;
	CHECK(cursor_button(ab.x + 10, ab.y + 5, HIT_TITLEBAR, a->container,
		SWINE_BTN_LEFT, true, 0));
	CHECK(cursor_button(ab.x + 10, ab.y + 5, HIT_TITLEBAR, a->container,
		SWINE_BTN_LEFT, false, 0));
	CHECK(seat_get_focused_container() == a->container);
	CHECK(cursor_axis(ab.x + 10, ab.y + 5, HIT_TITLEBAR, a->container,
		SWINE_SCROLL_DOWN, 0));
	CHECK(seat_get_focused_container() != a->container);
	CHECK(!a->pending.visible);
	// Other workspaces' tabs are not drawn.
	CHECK(run("workspace 2"));
	CHECK_INT(fake.titlebar_count, 0);
	CHECK(run("workspace 1"));
	CHECK_INT(fake.titlebar_count, 1);
	// hide_edge_borders --i3: no tab for a lone child.
	CHECK(run("hide_edge_borders --i3 none"));
	fake_teardown();

	fake_setup();
	CHECK(run("hide_edge_borders --i3 none"));
	struct swine_view *d = fake_map_view("d");
	CHECK(run("layout tabbed"));
	CHECK_INT(fake.titlebar_count, 0);
	CHECK_INT(d->pending.box.y, 0);
	CHECK(d->current.deco.titlebar);
	// font resets the cached height (measured again at the next arrange).
	config->font_height = 99;
	CHECK(run("font pango:monospace 12"));
	CHECK_INT(config->font_height, 15);
	fake_teardown();
}

void test_decorations_all(void) {
	test_border_defaults();
	test_border_command();
	test_client_colors();
	test_smart_borders();
	test_gap_bounds();
	test_opacity();
	test_font_and_titlebar();
	test_titlebars();
}
