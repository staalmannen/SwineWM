#include <stdlib.h>
#include <string.h>
#include "fake_platform.h"
#include "swinewm/config.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/input/seat.h"
#include "swinewm/ipc-json.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/view.h"
#include "swinewm/tree/workspace.h"
#include "test.h"

static struct swine_view *focused_view(void) {
	struct swine_container *con = seat_get_focused_container();
	return con ? con->view : NULL;
}

static void test_mark_unmark(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	struct swine_view *b = fake_map_view("b");
	CHECK(run("mark one"));
	CHECK(container_has_mark(b->container, "one"));
	// A mark is unique: setting it elsewhere moves it.
	CHECK(run("focus left; mark one"));
	CHECK(container_has_mark(a->container, "one"));
	CHECK(!container_has_mark(b->container, "one"));
	// --replace (the default) drops the other marks, --add keeps them
	CHECK(run("mark --add two"));
	CHECK_INT(a->container->marks->length, 2);
	CHECK(run("mark three"));
	CHECK_INT(a->container->marks->length, 1);
	CHECK(run("mark --add --toggle three"));
	CHECK_INT(a->container->marks->length, 0);
	CHECK(run("mark --add --toggle three"));
	CHECK(container_has_mark(a->container, "three"));
	CHECK(!run("mark --frobnicate x"));
	CHECK(!run("mark --add"));

	// Criteria and commands find containers by mark.
	CHECK(run("[con_mark=\"^thr\"] focus"));
	CHECK(focused_view() == a);
	CHECK(run("focus right; [con_mark=three] floating enable"));
	CHECK(container_is_floating(a->container));
	CHECK(focused_view() == a);   // as in sway, floating focuses
	CHECK(run("[con_mark=three] floating disable; [title=\"^b$\"] focus"));

	char *marks = ipc_json_get_marks();
	CHECK_STR(marks, "[\"three\"]");
	free(marks);

	// unmark NAME, [criteria] unmark, unmark
	CHECK(run("mark --add four; mark --add five"));
	CHECK(run("unmark four"));
	CHECK(!container_has_mark(b->container, "four"));
	CHECK(container_has_mark(b->container, "five"));
	CHECK(run("[con_mark=three] unmark"));
	CHECK_INT(a->container->marks->length, 0);
	CHECK(container_has_mark(b->container, "five"));
	CHECK(run("unmark"));
	CHECK_INT(b->container->marks->length, 0);
	fake_teardown();
}

static void test_mark_for_window(void) {
	fake_setup();
	// "mark" re-runs for_window rules, so con_mark rules can apply.
	CHECK(run("for_window [con_mark=\"^float$\"] floating enable"));
	struct swine_view *a = fake_map_view("a");
	CHECK(!container_is_floating(a->container));
	CHECK(run("mark float"));
	CHECK(container_is_floating(a->container));
	fake_teardown();
}

static void test_move_to_mark(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	CHECK(run("mark target"));
	CHECK(run("workspace 2"));
	struct swine_view *b = fake_map_view("b");
	CHECK(run("move container to mark target"));
	CHECK_STR(b->container->workspace->name, "1");
	CHECK(b->container->parent == a->container->parent);
	CHECK(!run("move to mark nowhere"));
	fake_teardown();
}

static void test_swap(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	struct swine_view *b = fake_map_view("b");
	struct swine_view *c = fake_map_view("c");
	struct swine_workspace *ws = a->container->workspace;
	CHECK(run("[title=\"^a$\"] mark left"));
	CHECK(run("swap container with mark left"));
	CHECK(ws->tiling->items[0] == c->container);
	CHECK(ws->tiling->items[2] == a->container);
	CHECK_INT(c->pending.box.x, 0);
	CHECK(focused_view() == c);   // focus stays with the container
	char line[64];
	snprintf(line, sizeof(line), "swap container with con_id %u",
		(unsigned)b->container->node.id);
	CHECK(run(line));
	CHECK(ws->tiling->items[1] == c->container);
	CHECK(ws->tiling->items[0] == b->container);

	// Across workspaces, focus follows the container that was focused.
	CHECK(run("workspace 2"));
	struct swine_view *d = fake_map_view("d");
	snprintf(line, sizeof(line), "swap container with id %llu",
		(unsigned long long)(size_t)a->handle);
	CHECK(run(line));
	CHECK_STR(d->container->workspace->name, "1");
	CHECK_STR(a->container->workspace->name, "2");
	// As in sway, focus stays where it was: on the container now there.
	CHECK(focused_view() == a);
	CHECK_STR(seat_get_focused_workspace()->name, "2");

	CHECK(!run("swap container with mark nowhere"));
	CHECK(!run("swap container with con_id 999999"));
	snprintf(line, sizeof(line), "swap container with con_id %u",
		(unsigned)a->container->node.id);
	CHECK(!run(line));   // itself
	CHECK(!run("swap window with mark left"));
	fake_teardown();
}

static void test_sticky(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	struct swine_view *b = fake_map_view("b");
	(void)a;
	CHECK(run("floating enable, sticky enable"));
	CHECK(container_is_sticky(b->container));
	CHECK(run("workspace 2"));
	CHECK_STR(b->container->workspace->name, "2");
	CHECK(b->pending.visible);
	// A workspace holding only sticky windows counts as empty.
	CHECK(run("workspace 3"));
	CHECK(workspace_by_name("2") == NULL);
	CHECK(!run("move container to workspace 4"));
	CHECK(!run("sticky toggle"));   // the empty workspace has the focus
	CHECK(run("[title=\"^b$\"] sticky toggle"));
	CHECK(!container_is_sticky(b->container));
	CHECK(run("workspace 1"));
	CHECK_STR(b->container->workspace->name, "3");
	CHECK(!b->pending.visible);
	// Tiled windows can be marked sticky but only follow while floating.
	CHECK(run("sticky enable"));
	CHECK(!container_is_sticky(a->container));
	fake_teardown();
}

static void test_rename(void) {
	fake_setup();
	fake_map_view("a");
	CHECK(run("rename workspace to 5: mail"));
	CHECK_STR(seat_get_focused_workspace()->name, "5: mail");
	CHECK(run("workspace 2"));
	fake_map_view("b");
	CHECK(run("rename workspace number 5 to 1: web"));
	CHECK(workspace_by_name("1: web") != NULL);
	struct swine_output *output = root->outputs->items[0];
	CHECK_STR(((struct swine_workspace *)output->workspaces->items[0])->name,
		"1: web");
	CHECK(run("rename workspace 2 to code"));
	CHECK_STR(seat_get_focused_workspace()->name, "code");
	CHECK(!run("rename workspace code to 1: web"));   // exists
	CHECK(run("rename workspace code to code"));
	CHECK(!run("rename workspace code to next"));
	CHECK(!run("rename workspace nothing to x"));
	CHECK(!run("rename workspace code"));
	CHECK(!run("rename output x to y"));
	fake_teardown();
}

static void test_focus_next_prev(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	struct swine_view *b = fake_map_view("b");
	CHECK(run("splitv"));
	struct swine_view *c = fake_map_view("c");
	CHECK(run("focus prev"));
	CHECK(focused_view() == b);
	CHECK(run("focus prev"));   // wraps within the column
	CHECK(focused_view() == c);
	CHECK(run("focus parent; focus prev"));
	CHECK(focused_view() == a);
	CHECK(run("focus next"));
	CHECK(focused_view() == c);   // the column's last focus
	// sibling: the container itself, without descending
	CHECK(run("focus left; focus next sibling"));
	struct swine_container *focused = seat_get_focused_container();
	CHECK(focused && focused->view == NULL);
	CHECK(!run("focus sideways"));
	fake_teardown();
}

void test_marks_all(void) {
	test_mark_unmark();
	test_mark_for_window();
	test_move_to_mark();
	test_swap();
	test_sticky();
	test_rename();
	test_focus_next_prev();
}
