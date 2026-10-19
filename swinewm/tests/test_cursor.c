// Pointer input (input/cursor.c): mouse bindings, focus_follows_mouse,
// floating_modifier moves and resizes, dragging tiled windows and
// mouse_warping. Expected results follow sway's seatop_* code.
#include <stdlib.h>
#include <string.h>
#include "fake_platform.h"
#include "swinewm/config.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/input/cursor.h"
#include "swinewm/input/keyboard.h"
#include "swinewm/input/seat.h"
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

static struct swine_binding *mouse_binding(int i) {
	list_t *bindings = config->current_mode->mouse_bindings;
	return i < bindings->length ? bindings->items[i] : NULL;
}

// Moves the pointer over the middle of a view.
static void hover(struct swine_view *view) {
	struct swine_box *b = &view->container->box;
	cursor_motion(b->x + b->width / 2, b->y + b->height / 2, HIT_CONTENTS,
		view->container);
}

static void test_mouse_bindings_parse(void) {
	fake_setup();
	CHECK(run("bindsym button2 kill"));
	CHECK(run("bindsym --whole-window Mod4+button4 workspace prev"));
	CHECK(run("bindsym --border --exclude-titlebar BTN_SIDE nop back"));
	CHECK(run("bindsym --release button3 nop release"));
	CHECK(!run("bindsym button10 nop"));
	CHECK(!run("bindsym BTN_NOSUCH nop"));
	CHECK(!run("bindsym Mod4+button1+button2 nop"));
	CHECK_INT(config->current_mode->mouse_bindings->length, 4);
	struct swine_binding *b = mouse_binding(0);
	CHECK(b && b->keycode == SWINE_BTN_MIDDLE && b->flags == BINDING_TITLEBAR);
	b = mouse_binding(1);
	CHECK(b && b->keycode == SWINE_SCROLL_UP && b->modifiers == SWINE_MOD_LOGO &&
		b->flags == (BINDING_BORDER | BINDING_CONTENTS | BINDING_TITLEBAR));
	b = mouse_binding(2);
	CHECK(b && b->keycode == SWINE_BTN_SIDE && b->flags == BINDING_BORDER);
	CHECK_STR(get_mouse_button_name(SWINE_BTN_SIDE), "BTN_SIDE");
	// Keyboard bindings are unaffected, and "1" is a key, not a button.
	CHECK(run("bindsym Mod4+1 workspace 1"));
	CHECK(keyboard_find_binding(SWINE_MOD_LOGO, '1', false) != NULL);
	CHECK_INT(config->current_mode->mouse_bindings->length, 4);
	// Rebinding replaces; unbindsym removes.
	CHECK(run("bindsym button2 nop other"));
	CHECK_INT(config->current_mode->mouse_bindings->length, 4);
	CHECK(run("unbindsym button2"));
	CHECK_INT(config->current_mode->mouse_bindings->length, 3);
	fake_teardown();
}

static void test_mouse_bindings_run(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	CHECK(run("bindsym --whole-window Mod4+button4 workspace 2"));
	CHECK(run("bindsym button2 kill"));
	CHECK(run("bindsym --whole-window --release button8 workspace 3"));
	// Whole-window: over the window, with the modifier only.
	struct swine_box *box = &a->container->box;
	CHECK(!cursor_axis(box->x + 10, box->y + 10, HIT_CONTENTS, a->container,
		SWINE_SCROLL_UP, 0));
	CHECK(cursor_axis(box->x + 10, box->y + 10, HIT_CONTENTS, a->container,
		SWINE_SCROLL_UP, SWINE_MOD_LOGO));
	CHECK_STR(seat_get_focused_workspace()->name, "2");
	CHECK(run("workspace 1"));
	// A title bar binding does not fire on the window's contents.
	CHECK(!cursor_button(box->x + 10, box->y + 10, HIT_CONTENTS, a->container,
		SWINE_BTN_MIDDLE, true, 0));
	CHECK(!cursor_button(box->x + 10, box->y + 10, HIT_CONTENTS, a->container,
		SWINE_BTN_MIDDLE, false, 0));
	CHECK_INT(fake.close_calls, 0);
	// ...but on the title bar, and on an empty workspace (sway runs title
	// bar bindings there too, if they cover every region; this one does not).
	CHECK(cursor_button(box->x + 10, box->y + 2, HIT_TITLEBAR, a->container,
		SWINE_BTN_MIDDLE, true, 0));
	CHECK(!cursor_button(box->x + 10, box->y + 2, HIT_TITLEBAR, a->container,
		SWINE_BTN_MIDDLE, false, 0));
	CHECK_INT(fake.close_calls, 1);
	// A release binding swallows the press too, and runs on the release.
	CHECK(cursor_button(box->x + 10, box->y + 10, HIT_CONTENTS, a->container,
		SWINE_BTN_SIDE, true, 0));
	CHECK_STR(seat_get_focused_workspace()->name, "1");
	CHECK(cursor_button(box->x + 10, box->y + 10, HIT_CONTENTS, a->container,
		SWINE_BTN_SIDE, false, 0));
	CHECK_STR(seat_get_focused_workspace()->name, "3");
	// --whole-window covers the desktop (an empty workspace) as well.
	CHECK(cursor_axis(500, 400, HIT_WORKSPACE, NULL, SWINE_SCROLL_UP,
		SWINE_MOD_LOGO));
	CHECK_STR(seat_get_focused_workspace()->name, "2");
	fake_teardown();
}

static void test_focus_follows_mouse(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	struct swine_view *b = fake_map_view("b");
	CHECK(focused_view() == b);
	hover(a);
	CHECK(focused_view() == a);
	// "yes": the pointer has not left a, so a keyboard focus change sticks.
	CHECK(run("focus right"));
	CHECK(focused_view() == b);
	hover(a);
	CHECK(focused_view() == b);
	// "always" takes it back on the next motion.
	CHECK(run("focus_follows_mouse always"));
	hover(a);
	CHECK(focused_view() == a);
	// "no" never moves the focus.
	CHECK(run("focus_follows_mouse no"));
	hover(b);
	CHECK(focused_view() == a);
	// Nor does motion while a button is held (dragging a selection).
	CHECK(run("focus_follows_mouse yes"));
	hover(a);
	CHECK(!cursor_button(100, 100, HIT_CONTENTS, a->container,
		SWINE_BTN_LEFT, true, 0));
	hover(b);
	CHECK(focused_view() == a);
	CHECK(!cursor_button(600, 100, HIT_CONTENTS, b->container,
		SWINE_BTN_LEFT, false, 0));
	hover(a);
	hover(b);
	CHECK(focused_view() == b);
	// Windows swinewm does not manage (menus, launchers) keep the focus.
	cursor_motion(100, 100, HIT_NONE, NULL);
	CHECK(focused_view() == b);
	fake_teardown();
}

static void test_focus_follows_mouse_keeps_owned_dialog_focus(void) {
	fake_setup();
	struct swine_view *owner = fake_map_view("owner");
	struct swine_view *dialog = fake_map_view("dialog");
	CHECK(focused_view() == dialog);
	fake.has_visible_owned_window = true;
	hover(owner);
	CHECK(focused_view() == dialog);
	fake.has_visible_owned_window = false;
	CHECK(run("focus_follows_mouse always"));
	hover(owner);
	CHECK(focused_view() == owner);
	fake_teardown();
}

static void test_focus_follows_mouse_outputs(void) {
	fake_setup_two_outputs();
	struct swine_view *a = fake_map_view("a");
	struct swine_output *right = output_by_name("DISPLAY2");
	// The empty workspace on the other output takes the focus...
	cursor_motion(1500, 400, HIT_WORKSPACE, NULL);
	CHECK(seat_get_focused_workspace() == output_get_active_workspace(right));
	CHECK(focused_view() == NULL);
	// ...but a gap on the focused output does not.
	hover(a);
	CHECK(focused_view() == a);
	cursor_motion(2, 2, HIT_WORKSPACE, NULL);
	CHECK(focused_view() == a);
	// Clicking an empty workspace focuses it.
	CHECK(!cursor_button(1500, 400, HIT_WORKSPACE, NULL, SWINE_BTN_LEFT,
		true, 0));
	CHECK(seat_get_focused_workspace() == output_get_active_workspace(right));
	CHECK(!cursor_button(1500, 400, HIT_WORKSPACE, NULL, SWINE_BTN_LEFT,
		false, 0));
	fake_teardown();
}

static void test_move_floating(void) {
	fake_setup_two_outputs();
	CHECK(run("floating_modifier Mod4"));
	struct swine_view *a = fake_map_view("a");
	CHECK(run("floating enable"));
	struct swine_box start = a->container->floating_box;
	int x = start.x + 20, y = start.y + 20;
	// Without the modifier the click belongs to the window.
	CHECK(!cursor_button(x, y, HIT_CONTENTS, a->container, SWINE_BTN_LEFT,
		true, 0));
	CHECK(!cursor_in_operation());
	CHECK(!cursor_button(x, y, HIT_CONTENTS, a->container, SWINE_BTN_LEFT,
		false, 0));
	CHECK(cursor_button(x, y, HIT_CONTENTS, a->container, SWINE_BTN_LEFT,
		true, SWINE_MOD_LOGO));
	CHECK(cursor_in_operation());
	cursor_motion(x + 50, y + 30, HIT_CONTENTS, a->container);
	CHECK_INT(a->container->floating_box.x, start.x + 50);
	CHECK_INT(a->container->floating_box.y, start.y + 30);
	CHECK_INT(a->container->floating_box.width, start.width);
	CHECK_INT(a->pending.box.x, start.x + 50);
	// Moved onto the other output, it joins the workspace shown there.
	cursor_motion(x + 1000, y, HIT_CONTENTS, a->container);
	CHECK_STR(a->container->workspace->output->name, "DISPLAY2");
	CHECK(cursor_button(x + 1000, y, HIT_CONTENTS, a->container,
		SWINE_BTN_LEFT, false, SWINE_MOD_LOGO));
	CHECK(!cursor_in_operation());
	CHECK(container_is_floating(a->container));
	// "inverse" swaps the buttons.
	CHECK(run("floating_modifier Mod4 inverse"));
	x += 1000;
	CHECK(cursor_button(x, y, HIT_CONTENTS, a->container, SWINE_BTN_RIGHT,
		true, SWINE_MOD_LOGO));
	cursor_motion(x - 10, y, HIT_CONTENTS, a->container);
	CHECK(cursor_button(x - 10, y, HIT_CONTENTS, a->container, SWINE_BTN_RIGHT,
		false, SWINE_MOD_LOGO));
	CHECK_INT(a->container->floating_box.x, start.x + 990);
	CHECK(!run("floating_modifier Mod4 sideways"));
	// The window going away ends the move.
	CHECK(run("floating_modifier Mod4 normal"));
	CHECK(cursor_button(x, y, HIT_CONTENTS, a->container, SWINE_BTN_LEFT,
		true, SWINE_MOD_LOGO));
	view_unmap(a);
	CHECK(!cursor_in_operation());
	view_destroy(a);
	fake_teardown();
}

static void test_resize_floating(void) {
	fake_setup();
	CHECK(run("floating_modifier Mod4"));
	struct swine_view *a = fake_map_view("a");
	CHECK(run("floating enable; resize set 400 300"));
	struct swine_box start = a->container->floating_box;
	CHECK_INT(start.width, 400);
	// Bottom right quarter: that corner follows the pointer.
	int x = start.x + start.width - 10, y = start.y + start.height - 10;
	CHECK(cursor_button(x, y, HIT_CONTENTS, a->container, SWINE_BTN_RIGHT,
		true, SWINE_MOD_LOGO));
	cursor_motion(x + 40, y + 20, HIT_CONTENTS, a->container);
	CHECK_INT(a->container->floating_box.x, start.x);
	CHECK_INT(a->container->floating_box.y, start.y);
	CHECK_INT(a->container->floating_box.width, start.width + 40);
	CHECK_INT(a->container->floating_box.height, start.height + 20);
	CHECK(cursor_button(x + 40, y + 20, HIT_CONTENTS, a->container,
		SWINE_BTN_RIGHT, false, SWINE_MOD_LOGO));
	// Top left quarter: the opposite corner stays put.
	start = a->container->floating_box;
	x = start.x + 10;
	y = start.y + 10;
	CHECK(cursor_button(x, y, HIT_CONTENTS, a->container, SWINE_BTN_RIGHT,
		true, SWINE_MOD_LOGO));
	cursor_motion(x - 30, y - 10, HIT_CONTENTS, a->container);
	CHECK_INT(a->container->floating_box.x, start.x - 30);
	CHECK_INT(a->container->floating_box.y, start.y - 10);
	CHECK_INT(a->container->floating_box.width, start.width + 30);
	CHECK_INT(a->container->floating_box.height, start.height + 10);
	// Never smaller than floating_minimum_size (75x50).
	cursor_motion(x + 2000, y + 2000, HIT_CONTENTS, a->container);
	CHECK_INT(a->container->floating_box.width, 75);
	CHECK_INT(a->container->floating_box.height, 50);
	CHECK(cursor_button(x, y, HIT_CONTENTS, a->container, SWINE_BTN_RIGHT,
		false, SWINE_MOD_LOGO));
	CHECK(!cursor_in_operation());
	fake_teardown();
}

static void test_resize_tiling(void) {
	fake_setup();
	CHECK(run("floating_modifier Mod4"));
	struct swine_view *a = fake_map_view("a");
	struct swine_view *b = fake_map_view("b");
	int width = a->container->box.width;
	// Right half of a: its right edge follows the pointer.
	int x = a->container->box.x + width - 20, y = 100;
	CHECK(cursor_button(x, y, HIT_CONTENTS, a->container, SWINE_BTN_RIGHT,
		true, SWINE_MOD_LOGO));
	CHECK(focused_view() == a);
	cursor_motion(x + 100, y, HIT_CONTENTS, a->container);
	CHECK_INT(a->container->box.width, width + 100);
	CHECK_INT(b->container->box.width, width - 100);
	cursor_motion(x + 50, y, HIT_CONTENTS, a->container);
	CHECK_INT(a->container->box.width, width + 50);
	CHECK(cursor_button(x + 50, y, HIT_CONTENTS, a->container, SWINE_BTN_RIGHT,
		false, SWINE_MOD_LOGO));
	// The left edge of a is the workspace's: nothing to resize.
	x = a->container->box.x + 10;
	CHECK(cursor_button(x, y, HIT_CONTENTS, a->container, SWINE_BTN_RIGHT,
		true, SWINE_MOD_LOGO));
	cursor_motion(x - 50, y, HIT_CONTENTS, a->container);
	CHECK_INT(a->container->box.width, width + 50);
	CHECK(cursor_button(x - 50, y, HIT_CONTENTS, a->container, SWINE_BTN_RIGHT,
		false, SWINE_MOD_LOGO));
	fake_teardown();
}

static const char *ws_order(struct swine_workspace *ws, char *buf, size_t size) {
	buf[0] = '\0';
	for (int i = 0; i < ws->tiling->length; ++i) {
		struct swine_container *con = ws->tiling->items[i];
		if (con->view) {
			strncat(buf, con->view->title, size - strlen(buf) - 1);
		} else {
			strncat(buf, "[", size - strlen(buf) - 1);
			for (int j = 0; j < con->children->length; ++j) {
				struct swine_container *child = con->children->items[j];
				strncat(buf, child->view ? child->view->title : "?",
					size - strlen(buf) - 1);
			}
			strncat(buf, "]", size - strlen(buf) - 1);
		}
	}
	return buf;
}

static void drag(struct swine_view *view, int to_x, int to_y,
		enum cursor_hit hit, struct swine_container *over) {
	struct swine_box *b = &view->container->box;
	int x = b->x + b->width / 2, y = b->y + b->height / 2;
	cursor_button(x, y, HIT_CONTENTS, view->container, SWINE_BTN_LEFT, true,
		SWINE_MOD_LOGO);
	cursor_motion(to_x, to_y, hit, over);
	cursor_button(to_x, to_y, hit, over, SWINE_BTN_LEFT, false, SWINE_MOD_LOGO);
}

static void test_move_tiling(void) {
	fake_setup_two_outputs();
	CHECK(run("floating_modifier Mod4"));
	struct swine_view *a = fake_map_view("a");
	struct swine_view *b = fake_map_view("b");
	struct swine_view *c = fake_map_view("c");
	struct swine_workspace *ws = a->container->workspace;
	char buf[64];
	CHECK_STR(ws_order(ws, buf, sizeof(buf)), "abc");
	// Dropped in the middle of another window: the two swap.
	struct swine_box cb = c->container->box;
	drag(a, cb.x + cb.width / 2, cb.y + cb.height / 2, HIT_CONTENTS,
		c->container);
	CHECK_STR(ws_order(ws, buf, sizeof(buf)), "cba");
	CHECK(!fake.drop_indicator_shown);
	// Near the bottom of b: b is split and a goes below it.
	struct swine_box bb = b->container->box;
	drag(a, bb.x + bb.width / 2, bb.y + bb.height - 60, HIT_CONTENTS,
		b->container);
	CHECK_STR(ws_order(ws, buf, sizeof(buf)), "c[ba]");
	// Close to the top edge of the workspace: a new top row.
	drag(c, 750, 10, HIT_CONTENTS, b->container);
	// (as in sway, the old row is wrapped in a container of its own)
	CHECK_STR(ws_order(ws, buf, sizeof(buf)), "c[?]");
	CHECK_INT(ws->layout, L_VERT);
	// Onto the empty workspace of the other output.
	drag(c, 1500, 400, HIT_WORKSPACE, NULL);
	CHECK_STR(c->container->workspace->output->name, "DISPLAY2");
	CHECK(focused_view() == c);
	// Released over nothing swinewm manages: nothing happens.
	drag(a, 300, 300, HIT_NONE, NULL);
	CHECK(a->container->workspace == ws);
	// tiling_drag disable: the modifier and left button do nothing.
	CHECK(run("tiling_drag disable"));
	struct swine_box ab = a->container->box;
	CHECK(!cursor_button(ab.x + 5, ab.y + 5, HIT_CONTENTS, a->container,
		SWINE_BTN_LEFT, true, SWINE_MOD_LOGO));
	CHECK(!cursor_button(ab.x + 5, ab.y + 5, HIT_CONTENTS, a->container,
		SWINE_BTN_LEFT, false, SWINE_MOD_LOGO));
	CHECK(run("tiling_drag toggle"));
	CHECK(config->tiling_drag);
	CHECK(run("tiling_drag_threshold 20"));
	CHECK_INT(config->tiling_drag_threshold, 20);
	CHECK(!run("tiling_drag_threshold -1"));
	fake_teardown();
}

static void test_move_tiling_threshold(void) {
	fake_setup();
	struct swine_view *a = fake_map_view("a");
	struct swine_view *b = fake_map_view("b");
	struct swine_workspace *ws = a->container->workspace;
	char buf[64];
	// Dragged by its title bar, a window only moves after the pointer
	// went past tiling_drag_threshold (9 px).
	struct swine_box ab = a->container->box;
	struct swine_box bb = b->container->box;
	CHECK(cursor_button(ab.x + 50, ab.y + 2, HIT_TITLEBAR, a->container,
		SWINE_BTN_LEFT, true, 0));
	cursor_motion(ab.x + 55, ab.y + 2, HIT_TITLEBAR, a->container);
	CHECK(!fake.drop_indicator_shown);
	CHECK(cursor_button(ab.x + 55, ab.y + 2, HIT_TITLEBAR, a->container,
		SWINE_BTN_LEFT, false, 0));
	CHECK_STR(ws_order(ws, buf, sizeof(buf)), "ab");
	CHECK(cursor_button(ab.x + 50, ab.y + 2, HIT_TITLEBAR, a->container,
		SWINE_BTN_LEFT, true, 0));
	cursor_motion(bb.x + bb.width / 2, bb.y + bb.height / 2, HIT_CONTENTS,
		b->container);
	CHECK(fake.drop_indicator_shown);
	CHECK(cursor_button(bb.x + bb.width / 2, bb.y + bb.height / 2,
		HIT_CONTENTS, b->container, SWINE_BTN_LEFT, false, 0));
	CHECK(!fake.drop_indicator_shown);
	CHECK_STR(ws_order(ws, buf, sizeof(buf)), "ba");
	fake_teardown();
}

static void test_mouse_warping(void) {
	fake_setup_two_outputs();
	struct swine_view *a = fake_map_view("a");
	struct swine_view *b = fake_map_view("b");
	(void)a;
	// "output" (the default): only when the focus leaves the output under
	// the pointer.
	fake.cursor_x = 100;
	fake.cursor_y = 100;
	CHECK(run("focus left"));
	CHECK_INT(fake.warp_calls, 0);
	CHECK(run("focus output right"));
	CHECK_INT(fake.warp_calls, 1);
	CHECK_INT(fake.cursor_x, 1500);
	CHECK(run("focus output left"));
	CHECK_INT(fake.warp_calls, 2);
	// "container": to the middle of the focused window.
	CHECK(run("mouse_warping container"));
	CHECK(run("focus right"));
	CHECK_INT(fake.warp_calls, 3);
	CHECK_INT(fake.cursor_x, b->container->box.x + b->container->box.width / 2);
	// A workspace switch does not warp, as in sway.
	CHECK(run("workspace 5"));
	CHECK_INT(fake.warp_calls, 3);
	CHECK(run("workspace 1; mouse_warping none; focus left"));
	CHECK_INT(fake.warp_calls, 3);
	CHECK(!run("mouse_warping sometimes"));
	fake_teardown();
}

void test_cursor_all(void) {
	test_mouse_bindings_parse();
	test_mouse_bindings_run();
	test_focus_follows_mouse();
	test_focus_follows_mouse_keeps_owned_dialog_focus();
	test_focus_follows_mouse_outputs();
	test_move_floating();
	test_resize_floating();
	test_resize_tiling();
	test_move_tiling();
	test_move_tiling_threshold();
	test_mouse_warping();
}
