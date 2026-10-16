#include <stdlib.h>
#include "fake_platform.h"
#include "swinewm/config.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/input/seat.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/view.h"
#include "swinewm/tree/workspace.h"
#include "test.h"

static struct swine_workspace *focused_ws(void) {
	return seat_get_focused_workspace();
}

static struct swine_view *focused_view(void) {
	struct swine_container *con = seat_get_focused_container();
	return con ? con->view : NULL;
}

static void test_tiling_geometry(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	CHECK_INT(a->pending.box.width, 1000);
	CHECK_INT(a->pending.box.height, 800);
	CHECK(a->pending.visible);
	CHECK(fake.last_activated == a);

	struct swine_view *b = fake_map_view("b");
	CHECK_INT(a->pending.box.width, 500);
	CHECK_INT(b->pending.box.x, 500);
	CHECK_INT(b->pending.box.width, 500);
	CHECK(focused_view() == b);
	CHECK(fake.last_activated == b);

	// New windows go after the focused one, at the same level.
	CHECK(run("focus left"));
	CHECK(focused_view() == a);
	struct swine_view *c = fake_map_view("c");
	struct swine_workspace *ws = focused_ws();
	CHECK_INT(ws->tiling->length, 3);
	CHECK(ws->tiling->items[1] == c->container);
	CHECK_INT(c->pending.box.x, 333);

	// Gaps: inner between windows plus inner+outer at the edges.
	ws->gaps_inner = 10;
	transaction_commit_dirty();
	CHECK_INT(a->pending.box.x, 10);
	CHECK_INT(a->pending.box.y, 10);
	CHECK_INT(a->pending.box.height, 780);
	CHECK_INT(c->pending.box.x, a->pending.box.x + a->pending.box.width + 10);
	fake_teardown();
}

static void test_split_and_layouts(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	struct swine_view *b = fake_map_view("b");
	CHECK(run("splitv"));
	struct swine_view *c = fake_map_view("c");
	// b and c share a vertical split on the right half.
	CHECK(b->container->parent != NULL);
	CHECK(b->container->parent == c->container->parent);
	CHECK_INT(b->container->parent->layout, L_VERT);
	CHECK_INT(c->pending.box.x, 500);
	CHECK_INT(c->pending.box.y, 400);
	CHECK_INT(b->pending.box.height, 400);

	CHECK(run("focus up"));
	CHECK(focused_view() == b);
	CHECK(run("focus left"));
	CHECK(focused_view() == a);
	CHECK(run("focus right"));
	CHECK(focused_view() == b);  // most recently focused in the split

	// Tabbed: only the active child is visible.
	CHECK(run("layout tabbed"));
	CHECK_INT(b->container->parent->layout, L_TABBED);
	CHECK(b->pending.visible);
	CHECK(!c->pending.visible);
	CHECK_INT(b->pending.box.height, 800);
	CHECK(run("focus right"));  // tabs are horizontal
	CHECK(focused_view() == c);
	CHECK(c->pending.visible);
	CHECK(!b->pending.visible);
	CHECK(run("layout toggle split"));
	CHECK_INT(c->container->parent->layout, L_VERT);
	CHECK(b->pending.visible && c->pending.visible);
	fake_teardown();
}

// $mod+w / $mod+s / $mod+e with two windows directly on the workspace.
static void test_workspace_layouts(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	struct swine_view *b = fake_map_view("b");
	CHECK(a->pending.visible && b->pending.visible);
	CHECK(run("layout tabbed"));
	CHECK(b->pending.visible);
	CHECK(!a->pending.visible);
	CHECK_INT(b->pending.box.width, 1000);
	CHECK(run("layout stacking"));
	CHECK(b->pending.visible && !a->pending.visible);
	CHECK(run("layout toggle split"));
	CHECK(a->pending.visible && b->pending.visible);
	CHECK_INT(b->pending.box.width, 500);
	CHECK(run("layout toggle split"));
	CHECK_INT(b->pending.box.width, 1000);   // now split vertically
	CHECK_INT(b->pending.box.height, 400);
	fake_teardown();
}

static void test_workspaces(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	CHECK_STR(focused_ws()->name, "1");
	CHECK(run("workspace number 2"));
	CHECK_STR(focused_ws()->name, "2");
	CHECK(!a->pending.visible);
	struct swine_view *b = fake_map_view("b");
	CHECK_STR(b->container->workspace->name, "2");
	CHECK_INT(b->pending.box.width, 1000);

	CHECK(run("workspace 1"));
	CHECK(a->pending.visible);
	CHECK(!b->pending.visible);
	CHECK(focused_view() == a);

	CHECK(run("workspace back_and_forth"));
	CHECK_STR(focused_ws()->name, "2");

	// Empty workspaces disappear when left.
	CHECK(run("workspace 7"));
	CHECK(run("workspace 2"));
	CHECK(workspace_by_name("7") == NULL);

	CHECK(run("workspace next"));
	CHECK_STR(focused_ws()->name, "1");
	CHECK(run("workspace prev"));
	CHECK_STR(focused_ws()->name, "2");
	fake_teardown();
}

static void test_move_to_workspace(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	struct swine_view *b = fake_map_view("b");
	CHECK(run("move container to workspace number 3"));
	// Like sway, focus stays on the current workspace.
	CHECK_STR(focused_ws()->name, "1");
	CHECK(focused_view() == a);
	CHECK(!b->pending.visible);
	CHECK_STR(b->container->workspace->name, "3");
	CHECK_INT(a->pending.box.width, 1000);
	CHECK(run("workspace 3"));
	CHECK(focused_view() == b);
	CHECK(b->pending.visible);
	fake_teardown();
}

static void test_move_direction(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	struct swine_view *b = fake_map_view("b");
	struct swine_view *c = fake_map_view("c");
	struct swine_workspace *ws = focused_ws();
	CHECK(run("move left"));
	CHECK(ws->tiling->items[1] == c->container);
	CHECK(ws->tiling->items[2] == b->container);
	CHECK(focused_view() == c);

	// Moving up out of a horizontal workspace re-orients it (sway behaviour).
	CHECK(run("move up"));
	CHECK_INT(ws->layout, L_VERT);
	CHECK_INT(ws->tiling->length, 2);
	CHECK(ws->tiling->items[0] == c->container);
	CHECK_INT(c->pending.box.width, 1000);
	CHECK_INT(c->pending.box.height, 400);
	(void)a;
	fake_teardown();
}

static void test_close_focus(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	struct swine_view *b = fake_map_view("b");
	CHECK(run("splitv"));
	struct swine_view *c = fake_map_view("c");
	struct swine_workspace *ws = focused_ws();
	// Closing c focuses its sibling b, and the split disappears with it gone.
	view_destroy(c);
	transaction_commit_dirty();
	CHECK(focused_view() == b);
	view_destroy(b);
	transaction_commit_dirty();
	CHECK(focused_view() == a);
	CHECK_INT(ws->tiling->length, 1);
	CHECK_INT(a->pending.box.width, 1000);
	view_destroy(a);
	transaction_commit_dirty();
	CHECK(seat_get_focus() == &ws->node);
	CHECK(run("kill"));
	CHECK_INT(fake.close_calls, 0);
	fake_teardown();
}

static void test_floating_fullscreen(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	a->natural = (struct swine_box){0, 0, 400, 300};
	struct swine_view *b = fake_map_view("b");
	CHECK(run("focus left"));
	CHECK(run("floating toggle"));
	CHECK(container_is_floating(a->container));
	CHECK_INT(a->pending.box.width, 400);
	CHECK_INT(a->pending.box.x, 300);
	CHECK_INT(b->pending.box.width, 1000);
	CHECK(run("move right 20"));
	CHECK_INT(a->pending.box.x, 320);
	CHECK(run("focus mode_toggle"));
	CHECK(focused_view() == b);
	CHECK(run("fullscreen"));
	CHECK(!a->pending.visible);
	CHECK_INT(b->pending.box.width, 1000);
	CHECK(run("fullscreen toggle"));
	CHECK(a->pending.visible);
	CHECK(run("focus mode_toggle"));
	CHECK(run("floating disable"));
	CHECK(!container_is_floating(a->container));
	CHECK_INT(a->pending.box.width, 500);
	fake_teardown();
}

static void test_natural_floating(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	struct swine_view *dialog = view_create((void *)0x9999);
	dialog->natural_floating = true;
	dialog->natural = (struct swine_box){100, 100, 300, 200};
	view_map(dialog);
	transaction_commit_dirty();
	CHECK(container_is_floating(dialog->container));
	CHECK_INT(dialog->pending.box.x, 100);
	CHECK_INT(a->pending.box.width, 1000);

	// A dialog that places itself off the screen is centered.
	struct swine_view *lost = view_create((void *)0x9998);
	lost->natural_floating = true;
	lost->natural = (struct swine_box){-400, -300, 300, 200};
	view_map(lost);
	transaction_commit_dirty();
	CHECK_INT(lost->pending.box.x, 350);
	CHECK_INT(lost->pending.box.y, 300);
	CHECK_INT(lost->pending.box.width, 300);
	fake_teardown();
}

static void test_scratchpad(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	struct swine_view *b = fake_map_view("b");
	CHECK(run("move scratchpad"));
	CHECK(!b->pending.visible);
	CHECK(focused_view() == a);
	CHECK_INT(a->pending.box.width, 1000);
	CHECK(run("scratchpad show"));
	CHECK(b->pending.visible);
	CHECK(container_is_floating(b->container));
	CHECK(focused_view() == b);
	CHECK(run("scratchpad show"));
	CHECK(!b->pending.visible);
	CHECK(focused_view() == a);
	// Unmapping a hidden scratchpad window must not crash.
	view_destroy(b);
	transaction_commit_dirty();
	CHECK(focused_view() == a);
	fake_teardown();
}

static void test_two_outputs(void) {
	fake_setup_two_outputs();
	CHECK_INT(root->outputs->length, 2);
	struct swine_view *a = fake_map_view("a");
	CHECK_STR(a->container->workspace->name, "1");
	CHECK(run("focus right"));
	CHECK_STR(focused_ws()->name, "2");
	struct swine_view *b = fake_map_view("b");
	CHECK_INT(b->pending.box.x, 1000);
	CHECK(a->pending.visible && b->pending.visible);
	CHECK(run("focus left"));
	CHECK(focused_view() == a);
	CHECK(run("move right"));
	CHECK_STR(a->container->workspace->name, "2");
	CHECK_INT(a->pending.box.x, 1000);
	CHECK_INT(a->pending.box.width, 500);
	CHECK(run("focus output left"));
	CHECK_STR(focused_ws()->name, "1");
	fake_teardown();
}

static void test_resize(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	struct swine_view *b = fake_map_view("b");
	CHECK(run("resize grow width 100px"));
	CHECK_INT(b->pending.box.width, 600);
	CHECK_INT(a->pending.box.width, 400);
	CHECK(run("resize shrink width 10 ppt"));
	CHECK_INT(b->pending.box.width, 500);
	CHECK(!run("resize grow height 10px"));
	fake_teardown();

	// An edge resizes only the neighbour on that side.
	fake_setup();
	a = fake_map_view("a");
	b = fake_map_view("b");
	struct swine_view *c = fake_map_view("c");
	CHECK(run("focus left"));
	CHECK(run("resize grow right 100px"));
	CHECK_INT(b->pending.box.width, 433);
	CHECK_INT(a->pending.box.width, 333);
	CHECK_INT(c->pending.box.width, 234);
	CHECK(run("resize shrink left 33px"));
	CHECK_INT(a->pending.box.width, 366);
	CHECK_INT(b->pending.box.width, 400);
	// "or": the second amount is used where the first does not apply
	CHECK(run("resize set width 50 ppt"));
	CHECK_INT(b->pending.box.width, 500);
	CHECK_INT(a->pending.box.width + c->pending.box.width, 500);
	CHECK(!run("resize grow sideways 10px"));
	CHECK(!run("resize set width 10 parsecs"));

	// Floating windows: px, or ppt of the output; clamped to the minimum.
	c->natural = (struct swine_box){0, 0, 400, 300};
	CHECK(run("focus right"));
	CHECK(run("floating enable"));
	CHECK(run("resize grow width 10 px or 10 ppt"));
	CHECK_INT(c->pending.box.width, 410);
	CHECK(run("resize set 50 ppt 200"));
	CHECK_INT(c->pending.box.width, 500);
	CHECK_INT(c->pending.box.height, 200);
	CHECK(run("resize shrink height 1000px"));
	CHECK_INT(c->pending.box.height, 50);
	CHECK(run("resize set width 600 height 400"));
	CHECK_INT(c->pending.box.width, 600);
	CHECK_INT(c->pending.box.height, 400);
	fake_teardown();
}

static void test_urgency(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	CHECK(run("workspace 2"));
	struct swine_view *b = fake_map_view("b");
	struct swine_workspace *ws1 = a->container->workspace;
	struct swine_workspace *ws2 = b->container->workspace;

	// The focused view cannot become urgent.
	view_set_urgent(b, true);
	CHECK(!b->urgent);
	view_set_urgent(a, true);
	CHECK(a->urgent);
	CHECK(ws1->urgent);
	CHECK(!ws2->urgent);

	// Moving the urgent window carries the urgency along.
	CHECK(run("workspace 1"));   // focusing it clears urgency
	CHECK(!a->urgent);
	CHECK(!ws1->urgent);
	CHECK(run("workspace 2"));
	view_set_urgent(a, true);
	CHECK(!run("[app_id=nothing_matches] nop"));   // no matching node
	CHECK(run("urgent disable") && !b->urgent);
	CHECK(run("urgent deny"));
	view_set_urgent(a, false);
	CHECK(!ws1->urgent);
	CHECK(run("workspace 1"));
	CHECK(run("urgent deny"));
	CHECK(run("workspace 2"));
	view_set_urgent(a, true);
	CHECK(!a->urgent);           // denied
	fake_teardown();
}

static void test_requested_fullscreen(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("browser");
	struct swine_view *b = fake_map_view("other");
	view_request_fullscreen(a, true);
	transaction_commit_dirty();
	CHECK_INT(a->container->fullscreen_mode, FULLSCREEN_WORKSPACE);
	CHECK(a->pending.visible);
	CHECK(!b->pending.visible);
	CHECK_INT(a->pending.box.width, 1000);
	view_request_fullscreen(a, false);
	transaction_commit_dirty();
	CHECK_INT(a->container->fullscreen_mode, FULLSCREEN_NONE);
	CHECK(b->pending.visible);
	CHECK_INT(a->pending.box.width, 500);
	fake_teardown();
}

void test_tree_all(void) {
	test_urgency();
	test_requested_fullscreen();
	test_tiling_geometry();
	test_split_and_layouts();
	test_workspace_layouts();
	test_workspaces();
	test_move_to_workspace();
	test_move_direction();
	test_close_focus();
	test_floating_fullscreen();
	test_natural_floating();
	test_scratchpad();
	test_two_outputs();
	test_resize();
}
