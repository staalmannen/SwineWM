// Pointer input: focus_follows_mouse, mouse bindings, floating_modifier
// moves and resizes, dragging tiled windows, and mouse_warping.
//
// Ported from sway/input/seatop_default.c, seatop_move_floating.c,
// seatop_resize_floating.c, seatop_resize_tiling.c, seatop_move_tiling.c and
// cursor.c (Copyright (c) 2016-2017 Drew DeVault, MIT). sway keeps one
// "seat operation" at a time; so does this file.
//
// Differences that come from Windows: swinewm sees input through a hook and
// may only swallow it, not redirect it. A click on a window still reaches
// it unless swinewm uses the click, and Windows itself activates the
// clicked window. Windows have no border swinewm could hit-test, so there is
// no resizing by dragging a border.
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "ipc.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/input/cursor.h"
#include "swinewm/input/keyboard.h"
#include "swinewm/input/seat.h"
#include "swinewm/ipc-json.h"
#include "swinewm/platform.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/view.h"
#include "swinewm/tree/workspace.h"

// Thickness of the drop zone when dragging to the edge of a layout container.
#define DROP_LAYOUT_BORDER 30

#define PRESSED_BUTTONS_CAP 32

enum seatop {
	OP_DEFAULT,
	OP_MOVE_FLOATING,
	OP_RESIZE_FLOATING,
	OP_RESIZE_TILING,
	OP_MOVE_TILING,
};

static struct {
	int x, y;
	uint32_t pressed_buttons[PRESSED_BUTTONS_CAP];
	size_t pressed_button_count;
	struct swine_node *previous_node;   // under the pointer at the last motion
	enum seatop op;
	struct swine_container *con;        // being moved or resized

	// move floating
	int dx, dy;   // pointer offset in the container
	// resize floating
	unsigned edge;
	bool preserve_ratio;
	int ref_x, ref_y;              // pointer at the start
	int ref_width, ref_height;     // container size at the start
	int ref_con_x, ref_con_y;      // container position at the start
	// resize tiling
	struct swine_container *h_con, *v_con, *h_sib, *v_sib;
	unsigned edge_x, edge_y;
	int h_con_orig_width, v_con_orig_height;
	// move tiling
	struct swine_node *target_node;
	unsigned target_edge;
	bool threshold_reached;

	bool warp_pending;
	bool warp_force;
} cursor;

static const struct {
	const char *name;
	uint32_t code;
} button_names[] = {
	{ "BTN_LEFT", SWINE_BTN_LEFT },
	{ "BTN_RIGHT", SWINE_BTN_RIGHT },
	{ "BTN_MIDDLE", SWINE_BTN_MIDDLE },
	{ "BTN_SIDE", SWINE_BTN_SIDE },
	{ "BTN_EXTRA", SWINE_BTN_EXTRA },
	{ "BTN_FORWARD", SWINE_BTN_FORWARD },
	{ "BTN_BACK", SWINE_BTN_BACK },
	{ "BTN_TASK", SWINE_BTN_TASK },
	{ "SWAY_SCROLL_UP", SWINE_SCROLL_UP },
	{ "SWAY_SCROLL_DOWN", SWINE_SCROLL_DOWN },
	{ "SWAY_SCROLL_LEFT", SWINE_SCROLL_LEFT },
	{ "SWAY_SCROLL_RIGHT", SWINE_SCROLL_RIGHT },
};

uint32_t get_mouse_button(const char *name, char **error) {
	*error = NULL;
	if (swine_strncasecmp(name, "button", strlen("button")) == 0) {
		// Map to X11 mouse buttons
		const char *digits = name + strlen("button");
		int number = digits[0] - '0';
		if (number < 1 || number > 9 || digits[1] != '\0') {
			*error = swine_strdup("Only buttons 1-9 are supported. For other "
				"mouse buttons, use the name of the event code.");
			return 0;
		}
		static const uint32_t buttons[] = {SWINE_BTN_LEFT, SWINE_BTN_MIDDLE,
			SWINE_BTN_RIGHT, SWINE_SCROLL_UP, SWINE_SCROLL_DOWN,
			SWINE_SCROLL_LEFT, SWINE_SCROLL_RIGHT, SWINE_BTN_SIDE,
			SWINE_BTN_EXTRA};
		return buttons[number - 1];
	}
	if (has_prefix(name, "BTN_")) {
		for (size_t i = 0; i < sizeof(button_names) / sizeof(*button_names); ++i) {
			if (strcmp(button_names[i].name, name) == 0) {
				return button_names[i].code;
			}
		}
		*error = format_str("Unknown event %s", name);
		return 0;
	}
	return 0;
}

const char *get_mouse_button_name(uint32_t button) {
	for (size_t i = 0; i < sizeof(button_names) / sizeof(*button_names); ++i) {
		if (button_names[i].code == button) {
			return button_names[i].name;
		}
	}
	return NULL;
}

// --- shared helpers ---------------------------------------------------------

static void state_erase_button(uint32_t button) {
	size_t j = 0;
	for (size_t i = 0; i < cursor.pressed_button_count; ++i) {
		if (i > j) {
			cursor.pressed_buttons[j] = cursor.pressed_buttons[i];
		}
		if (cursor.pressed_buttons[i] != button) {
			++j;
		}
	}
	while (cursor.pressed_button_count > j) {
		--cursor.pressed_button_count;
		cursor.pressed_buttons[cursor.pressed_button_count] = 0;
	}
}

static void state_add_button(uint32_t button) {
	if (cursor.pressed_button_count >= PRESSED_BUTTONS_CAP) {
		return;
	}
	size_t i = 0;
	while (i < cursor.pressed_button_count && cursor.pressed_buttons[i] < button) {
		++i;
	}
	if (i < cursor.pressed_button_count && cursor.pressed_buttons[i] == button) {
		return;   // a press whose release we never saw
	}
	size_t j = cursor.pressed_button_count;
	while (j > i) {
		cursor.pressed_buttons[j] = cursor.pressed_buttons[j - 1];
		--j;
	}
	cursor.pressed_buttons[i] = button;
	cursor.pressed_button_count++;
}

// The mouse binding that matches the modifiers, where the pointer is, and
// the pressed buttons. Bindings have one button each, so the button must be
// the only one pressed.
static struct swine_binding *get_active_mouse_binding(uint32_t modifiers,
		bool release, enum cursor_hit hit) {
	if (!config || !config->current_mode) {
		return NULL;
	}
	bool on_workspace = hit == HIT_WORKSPACE;
	uint32_t click_region =
		((hit == HIT_TITLEBAR || on_workspace) ? BINDING_TITLEBAR : 0) |
		(on_workspace ? BINDING_BORDER : 0) |
		((hit == HIT_CONTENTS || on_workspace) ? BINDING_CONTENTS : 0);
	modifiers &= ~(uint32_t)(SWINE_MOD_CAPS | SWINE_MOD_MOD2);
	list_t *bindings = config->current_mode->mouse_bindings;
	for (int i = 0; i < bindings->length; ++i) {
		struct swine_binding *binding = bindings->items[i];
		if ((modifiers ^ binding->modifiers) ||
				cursor.pressed_button_count != 1 ||
				release != ((binding->flags & BINDING_RELEASE) != 0) ||
				!(click_region & binding->flags) ||
				(on_workspace &&
				 (click_region & binding->flags) != click_region) ||
				binding->keycode != cursor.pressed_buttons[0]) {
			continue;
		}
		return binding;
	}
	return NULL;
}

static void run_binding(struct swine_binding *binding) {
	char *event = ipc_json_event_binding(binding, "mouse");
	platform_ipc_event((uint32_t)IPC_EVENT_BINDING, event);
	free(event);
	platform_queue_command(binding->command);
}

static struct swine_node *hovered_node(int x, int y, enum cursor_hit hit,
		struct swine_container *con) {
	if ((hit == HIT_CONTENTS || hit == HIT_TITLEBAR) && con) {
		return &con->node;
	}
	if (hit == HIT_WORKSPACE) {
		struct swine_output *output = output_at_point(x, y);
		struct swine_workspace *ws = output ?
			output_get_active_workspace(output) : NULL;
		return ws ? &ws->node : NULL;
	}
	return NULL;
}

static struct swine_box node_box(struct swine_node *node) {
	if (node->type == N_WORKSPACE) {
		return node->workspace->box;
	}
	return node->container->box;
}

static void begin_default(void) {
	if (cursor.op == OP_MOVE_TILING) {
		platform_drop_indicator(NULL);
	}
	cursor.op = OP_DEFAULT;
	cursor.con = NULL;
	cursor.h_con = cursor.v_con = cursor.h_sib = cursor.v_sib = NULL;
	cursor.target_node = NULL;
}

// --- move floating ----------------------------------------------------------

static void begin_move_floating(struct swine_container *con) {
	begin_default();
	cursor.op = OP_MOVE_FLOATING;
	cursor.con = con;
	cursor.dx = cursor.x - con->floating_box.x;
	cursor.dy = cursor.y - con->floating_box.y;
	container_raise_floating(con);
	transaction_commit_dirty();
}

static void move_floating_motion(void) {
	container_floating_move_to(cursor.con, cursor.x - cursor.dx,
		cursor.y - cursor.dy);
	transaction_commit_dirty();
}

// --- resize floating --------------------------------------------------------

static void begin_resize_floating(struct swine_container *con, unsigned edge,
		uint32_t modifiers) {
	begin_default();
	cursor.op = OP_RESIZE_FLOATING;
	cursor.con = con;
	cursor.preserve_ratio = (modifiers & SWINE_MOD_SHIFT) != 0;
	cursor.edge = edge == SWINE_EDGE_NONE ?
		SWINE_EDGE_BOTTOM | SWINE_EDGE_RIGHT : edge;
	cursor.ref_x = cursor.x;
	cursor.ref_y = cursor.y;
	cursor.ref_con_x = con->floating_box.x;
	cursor.ref_con_y = con->floating_box.y;
	cursor.ref_width = con->floating_box.width;
	cursor.ref_height = con->floating_box.height;
	container_raise_floating(con);
	transaction_commit_dirty();
}

static double clamp(double value, double min, double max) {
	return value < min ? min : value > max ? max : value;
}

static void resize_floating_motion(void) {
	struct swine_container *con = cursor.con;
	unsigned edge = cursor.edge;

	// How far the pointer moved since the start; positive is down/right.
	double mouse_move_x = cursor.x - cursor.ref_x;
	double mouse_move_y = cursor.y - cursor.ref_y;
	if (edge == SWINE_EDGE_TOP || edge == SWINE_EDGE_BOTTOM) {
		mouse_move_x = 0;
	}
	if (edge == SWINE_EDGE_LEFT || edge == SWINE_EDGE_RIGHT) {
		mouse_move_y = 0;
	}
	double grow_width = edge & SWINE_EDGE_LEFT ? -mouse_move_x : mouse_move_x;
	double grow_height = edge & SWINE_EDGE_TOP ? -mouse_move_y : mouse_move_y;
	if (cursor.preserve_ratio) {
		double x_multiplier = grow_width / cursor.ref_width;
		double y_multiplier = grow_height / cursor.ref_height;
		double max_multiplier = x_multiplier > y_multiplier ?
			x_multiplier : y_multiplier;
		grow_width = cursor.ref_width * max_multiplier;
		grow_height = cursor.ref_height * max_multiplier;
	}

	int min_width, max_width, min_height, max_height;
	floating_calculate_constraints(con, &min_width, &max_width,
		&min_height, &max_height);
	double width = clamp(cursor.ref_width + grow_width, min_width, max_width);
	double height = clamp(cursor.ref_height + grow_height, min_height,
		max_height);
	grow_width = width - cursor.ref_width;
	grow_height = height - cursor.ref_height;

	// Position changes relative to the container at the start.
	double grow_x, grow_y;
	if (edge & SWINE_EDGE_LEFT) {
		grow_x = -grow_width;
	} else if (edge & SWINE_EDGE_RIGHT) {
		grow_x = 0;
	} else {
		grow_x = -grow_width / 2;
	}
	if (edge & SWINE_EDGE_TOP) {
		grow_y = -grow_height;
	} else if (edge & SWINE_EDGE_BOTTOM) {
		grow_y = 0;
	} else {
		grow_y = -grow_height / 2;
	}
	con->floating_box.x = cursor.ref_con_x + (int)grow_x;
	con->floating_box.y = cursor.ref_con_y + (int)grow_y;
	con->floating_box.width = (int)width;
	con->floating_box.height = (int)height;
	transaction_commit_dirty();
}

static void resize_floating_end(void) {
	struct swine_container *con = cursor.con;
	begin_default();
	// It may now be mostly on another output.
	container_floating_move_to(con, con->floating_box.x, con->floating_box.y);
	transaction_commit_dirty();
}

// --- resize tiling ----------------------------------------------------------

static struct swine_container *container_get_resize_sibling(
		struct swine_container *con, unsigned edge) {
	if (!con) {
		return NULL;
	}
	list_t *siblings = container_get_siblings(con);
	int index = container_sibling_index(con);
	int offset = edge & (SWINE_EDGE_TOP | SWINE_EDGE_LEFT) ? -1 : 1;
	if (siblings->length == 1) {
		return NULL;
	}
	return siblings->items[index + offset];
}

static void begin_resize_tiling(struct swine_container *con, unsigned edge) {
	begin_default();
	cursor.op = OP_RESIZE_TILING;
	cursor.con = con;
	cursor.edge = edge;
	cursor.ref_x = cursor.x;
	cursor.ref_y = cursor.y;
	if (edge & (SWINE_EDGE_LEFT | SWINE_EDGE_RIGHT)) {
		cursor.edge_x = edge & (SWINE_EDGE_LEFT | SWINE_EDGE_RIGHT);
		cursor.h_con = container_find_resize_parent(con, cursor.edge_x);
		cursor.h_sib = container_get_resize_sibling(cursor.h_con,
			cursor.edge_x);
		if (cursor.h_con) {
			cursor.h_con_orig_width = cursor.h_con->box.width;
		}
	}
	if (edge & (SWINE_EDGE_TOP | SWINE_EDGE_BOTTOM)) {
		cursor.edge_y = edge & (SWINE_EDGE_TOP | SWINE_EDGE_BOTTOM);
		cursor.v_con = container_find_resize_parent(con, cursor.edge_y);
		cursor.v_sib = container_get_resize_sibling(cursor.v_con,
			cursor.edge_y);
		if (cursor.v_con) {
			cursor.v_con_orig_height = cursor.v_con->box.height;
		}
	}
	transaction_commit_dirty();
}

static void resize_tiling_motion(void) {
	int amount_x = 0;
	int amount_y = 0;
	int moved_x = cursor.x - cursor.ref_x;
	int moved_y = cursor.y - cursor.ref_y;
	if (cursor.h_con) {
		if (cursor.edge & SWINE_EDGE_LEFT) {
			amount_x = (cursor.h_con_orig_width - moved_x) -
				cursor.h_con->box.width;
		} else if (cursor.edge & SWINE_EDGE_RIGHT) {
			amount_x = (cursor.h_con_orig_width + moved_x) -
				cursor.h_con->box.width;
		}
	}
	if (cursor.v_con) {
		if (cursor.edge & SWINE_EDGE_TOP) {
			amount_y = (cursor.v_con_orig_height - moved_y) -
				cursor.v_con->box.height;
		} else if (cursor.edge & SWINE_EDGE_BOTTOM) {
			amount_y = (cursor.v_con_orig_height + moved_y) -
				cursor.v_con->box.height;
		}
	}
	if (amount_x != 0) {
		container_resize_tiled(cursor.h_con, cursor.edge_x, amount_x);
	}
	if (amount_y != 0) {
		container_resize_tiled(cursor.v_con, cursor.edge_y, amount_y);
	}
	transaction_commit_dirty();
}

// --- move tiling ------------------------------------------------------------

static void begin_move_tiling(struct swine_container *con, bool threshold) {
	begin_default();
	cursor.op = OP_MOVE_TILING;
	cursor.con = con;
	cursor.ref_x = cursor.x;
	cursor.ref_y = cursor.y;
	cursor.threshold_reached = !threshold;
	cursor.target_node = NULL;
	cursor.target_edge = SWINE_EDGE_NONE;
}

static void resize_box(struct swine_box *box, unsigned edge, int thickness) {
	switch (edge) {
	case SWINE_EDGE_TOP:
		box->height = thickness;
		break;
	case SWINE_EDGE_LEFT:
		box->width = thickness;
		break;
	case SWINE_EDGE_RIGHT:
		box->x = box->x + box->width - thickness;
		box->width = thickness;
		break;
	case SWINE_EDGE_BOTTOM:
		box->y = box->y + box->height - thickness;
		box->height = thickness;
		break;
	default:
		box->x += thickness;
		box->y += thickness;
		box->width -= thickness * 2;
		box->height -= thickness * 2;
		break;
	}
}

static void no_target(void) {
	cursor.target_node = NULL;
	cursor.target_edge = SWINE_EDGE_NONE;
	platform_drop_indicator(NULL);
}

static void move_tiling_motion(enum cursor_hit hit,
		struct swine_container *hovered) {
	if (!cursor.threshold_reached) {
		struct swine_output *output = output_at_point(cursor.x, cursor.y);
		double scale = output && output->scale > 0 ? output->scale : 1;
		double threshold = config->tiling_drag_threshold * scale;
		double dx = cursor.x - cursor.ref_x, dy = cursor.y - cursor.ref_y;
		if (dx * dx + dy * dy > threshold * threshold) {
			cursor.threshold_reached = true;
		} else {
			return;
		}
	}

	struct swine_node *node = hovered_node(cursor.x, cursor.y, hit, hovered);
	if (!node) {
		// For example over the bar or a window swinewm does not manage.
		no_target();
		return;
	}
	if (node->type == N_WORKSPACE) {
		// An empty workspace (or the gaps)
		cursor.target_node = node;
		cursor.target_edge = SWINE_EDGE_NONE;
		struct swine_box drop_box = node->workspace->box;
		platform_drop_indicator(&drop_box);
		return;
	}

	// Deny moving within its own workspace if it is the only window there.
	struct swine_container *con = node->container;
	if (workspace_num_tiling_views(cursor.con->workspace) == 1 &&
			con->workspace == cursor.con->workspace) {
		no_target();
		return;
	}
	if (container_is_floating_or_child(con)) {
		no_target();
		return;
	}

	struct swine_output *output = output_at_point(cursor.x, cursor.y);
	double scale = output && output->scale > 0 ? output->scale : 1;
	int border = (int)(DROP_LAYOUT_BORDER * scale);
	int thresh_top = con->box.y + border;
	int thresh_bottom = con->box.y + con->box.height - border;
	int thresh_left = con->box.x + border;
	int thresh_right = con->box.x + con->box.width - border;

	// Traverse the ancestors, looking for a layout container perpendicular
	// to the edge, e.g. close to the top or bottom of a horizontal layout.
	while (con) {
		unsigned edge = SWINE_EDGE_NONE;
		enum swine_container_layout layout = container_parent_layout(con);
		struct swine_box box = node_box(node_get_parent(&con->node));
		if (layout == L_HORIZ || layout == L_TABBED) {
			if (cursor.y < thresh_top) {
				edge = SWINE_EDGE_TOP;
				if (thresh_top < box.y) {
					thresh_top = box.y;
				}
				box.height = thresh_top - box.y;
			} else if (cursor.y > thresh_bottom) {
				edge = SWINE_EDGE_BOTTOM;
				if (thresh_bottom > box.y + box.height) {
					thresh_bottom = box.y + box.height;
				}
				box.height = box.y + box.height - thresh_bottom;
				box.y = thresh_bottom;
			}
		} else if (layout == L_VERT || layout == L_STACKED) {
			if (cursor.x < thresh_left) {
				edge = SWINE_EDGE_LEFT;
				if (thresh_left < box.x) {
					thresh_left = box.x;
				}
				box.width = thresh_left - box.x;
			} else if (cursor.x > thresh_right) {
				edge = SWINE_EDGE_RIGHT;
				if (thresh_right > box.x + box.width) {
					thresh_right = box.x + box.width;
				}
				box.width = box.x + box.width - thresh_right;
				box.x = thresh_right;
			}
		}
		if (edge) {
			cursor.target_node = node_get_parent(&con->node);
			if (cursor.target_node &&
					(cursor.target_node == &cursor.con->node ||
					 node_has_ancestor(cursor.target_node, &cursor.con->node))) {
				cursor.target_node = node_get_parent(&cursor.con->node);
			}
			cursor.target_edge = edge;
			platform_drop_indicator(&box);
			return;
		}
		con = con->parent;
	}

	// Use the hovered view, but only over the window itself.
	con = node->container;
	if (!con->view || hit != HIT_CONTENTS || node == &cursor.con->node ||
			node_has_ancestor(node, &cursor.con->node)) {
		no_target();
		return;
	}

	// Find the closest edge.
	int smaller = con->box.width < con->box.height ?
		con->box.width : con->box.height;
	int thickness = (int)(smaller * 0.3);
	int closest_dist = INT_MAX;
	int dist;
	cursor.target_edge = SWINE_EDGE_NONE;
	if ((dist = cursor.y - con->box.y) < closest_dist) {
		closest_dist = dist;
		cursor.target_edge = SWINE_EDGE_TOP;
	}
	if ((dist = cursor.x - con->box.x) < closest_dist) {
		closest_dist = dist;
		cursor.target_edge = SWINE_EDGE_LEFT;
	}
	if ((dist = con->box.x + con->box.width - cursor.x) < closest_dist) {
		closest_dist = dist;
		cursor.target_edge = SWINE_EDGE_RIGHT;
	}
	if ((dist = con->box.y + con->box.height - cursor.y) < closest_dist) {
		closest_dist = dist;
		cursor.target_edge = SWINE_EDGE_BOTTOM;
	}
	if (closest_dist > thickness) {
		cursor.target_edge = SWINE_EDGE_NONE;
	}
	cursor.target_node = node;
	struct swine_box drop_box = con->box;
	resize_box(&drop_box, cursor.target_edge, thickness);
	platform_drop_indicator(&drop_box);
}

static bool is_parallel(enum swine_container_layout layout, unsigned edge) {
	bool layout_is_horiz = layout == L_HORIZ || layout == L_TABBED;
	bool edge_is_horiz = edge == SWINE_EDGE_LEFT || edge == SWINE_EDGE_RIGHT;
	return layout_is_horiz == edge_is_horiz;
}

static void move_tiling_finalize(void) {
	struct swine_node *target_node = cursor.target_node;
	struct swine_container *con = cursor.con;
	unsigned edge = cursor.target_edge;
	begin_default();
	if (!target_node) {
		transaction_commit_dirty();
		return;
	}

	struct swine_container *old_parent = con->parent;
	struct swine_workspace *new_ws = target_node->type == N_WORKSPACE ?
		target_node->workspace : target_node->container->workspace;
	bool after = edge != SWINE_EDGE_TOP && edge != SWINE_EDGE_LEFT;
	bool swap = edge == SWINE_EDGE_NONE && target_node->type == N_CONTAINER;
	if (!swap) {
		container_detach(con);
	}

	if (target_node->type == N_WORKSPACE && edge == SWINE_EDGE_NONE) {
		// Moving the container into an empty workspace
		con = workspace_add_tiling(new_ws, con);
	} else if (target_node->type == N_CONTAINER) {
		// Moving the container before or after another one
		struct swine_container *target = target_node->container;
		if (swap) {
			container_swap(target, con);
		} else {
			enum swine_container_layout layout = container_parent_layout(target);
			if (edge && !is_parallel(layout, edge)) {
				enum swine_container_layout new_layout =
					edge == SWINE_EDGE_TOP || edge == SWINE_EDGE_BOTTOM ?
					L_VERT : L_HORIZ;
				container_split(target, new_layout);
			}
			container_add_sibling(target, con, after);
		}
	} else {
		// The target is a workspace, which needs splitting.
		enum swine_container_layout new_layout =
			edge == SWINE_EDGE_TOP || edge == SWINE_EDGE_BOTTOM ?
			L_VERT : L_HORIZ;
		workspace_split(new_ws, new_layout);
		workspace_insert_tiling(new_ws, con, after ? new_ws->tiling->length : 0);
	}
	if (old_parent) {
		container_reap_empty(old_parent);
	}

	// As in sway: take the size of a sibling.
	list_t *siblings = container_get_siblings(con);
	if (siblings && siblings->length > 1) {
		int index = list_find(siblings, con);
		struct swine_container *sibling = index == 0 ?
			siblings->items[1] : siblings->items[index - 1];
		con->width_fraction = sibling->width_fraction;
		con->height_fraction = sibling->height_fraction;
	}
	seat_set_focus_container(con->view ? con :
		seat_get_focus_inactive_view(&con->node));
	char *event = ipc_json_event_window("move", con);
	platform_ipc_event((uint32_t)IPC_EVENT_WINDOW, event);
	free(event);
	transaction_commit_dirty();
}

// --- default: focus_follows_mouse and buttons -------------------------------

static void check_focus_follows_mouse(struct swine_node *hovered) {
	struct swine_node *focus = seat_get_focus();
	if (!hovered) {
		// A window swinewm does not manage, or the bar: on Windows these
		// include menus and launchers, which must keep the focus.
		return;
	}
	// A workspace (the desktop or a gap) only takes the focus when it is on
	// another output than the focus.
	if (hovered->type == N_WORKSPACE) {
		struct swine_output *focused_output = focus ? node_get_output(focus) : NULL;
		struct swine_output *hovered_output = node_get_output(hovered);
		if (focus && hovered_output != focused_output) {
			struct swine_node *next = seat_get_focus_inactive(hovered);
			seat_set_focus(next ? next : hovered);
			transaction_commit_dirty();
		}
		return;
	}
	// The common case. Hidden tabs are never focused.
	struct swine_container *con = hovered->container;
	if (con->view && view_is_visible(con->view) && focus != hovered) {
		if (platform_view_has_visible_owned_window(con->view)) {
			return;
		}
		// If the pointer got over the window because of, say, a workspace
		// switch, "yes" leaves the focus alone; "always" takes it.
		if (hovered != cursor.previous_node ||
				config->focus_follows_mouse == FOLLOWS_ALWAYS) {
			seat_set_focus(hovered);
			transaction_commit_dirty();
		}
	}
}

void cursor_motion(int x, int y, enum cursor_hit hit,
		struct swine_container *con) {
	cursor.x = x;
	cursor.y = y;
	switch (cursor.op) {
	case OP_MOVE_FLOATING:
		move_floating_motion();
		return;
	case OP_RESIZE_FLOATING:
		resize_floating_motion();
		return;
	case OP_RESIZE_TILING:
		resize_tiling_motion();
		return;
	case OP_MOVE_TILING:
		move_tiling_motion(hit, con);
		return;
	case OP_DEFAULT:
		break;
	}
	struct swine_node *node = hovered_node(x, y, hit, con);
	// While a button is held (selecting text, dragging a file) the focus
	// stays where the press was, as in sway's seatop_down.
	if (config && config->focus_follows_mouse != FOLLOWS_NO &&
			cursor.pressed_button_count == 0) {
		check_focus_follows_mouse(node);
	}
	cursor.previous_node = node;
}

static bool operation_button(uint32_t button, bool pressed) {
	if (pressed) {
		state_add_button(button);
		return true;
	}
	state_erase_button(button);
	if (cursor.pressed_button_count > 0) {
		return true;
	}
	switch (cursor.op) {
	case OP_RESIZE_FLOATING:
		resize_floating_end();
		break;
	case OP_MOVE_TILING:
		move_tiling_finalize();
		break;
	default:
		begin_default();
		transaction_commit_dirty();
		break;
	}
	return true;
}

bool cursor_button(int x, int y, enum cursor_hit hit,
		struct swine_container *cont, uint32_t button, bool pressed,
		uint32_t modifiers) {
	cursor.x = x;
	cursor.y = y;
	if (cursor.op != OP_DEFAULT) {
		return operation_button(button, pressed);
	}
	if (!config) {
		return false;
	}

	// Mouse bindings. A press is also kept from the window when a binding
	// waits for its release, so that the window never sees half a click.
	if (pressed) {
		state_add_button(button);
		struct swine_binding *binding =
			get_active_mouse_binding(modifiers, false, hit);
		if (binding) {
			run_binding(binding);
			return true;
		}
		if (get_active_mouse_binding(modifiers, true, hit)) {
			return true;
		}
	} else {
		struct swine_binding *binding =
			get_active_mouse_binding(modifiers, true, hit);
		state_erase_button(button);
		if (binding) {
			run_binding(binding);
			return true;
		}
		return false;
	}

	if (hit == HIT_NONE) {
		return false;
	}
	struct swine_node *node = hovered_node(x, y, hit, cont);
	if (!node) {
		return false;
	}

	// Clicking an empty workspace focuses it.
	if (node->type == N_WORKSPACE) {
		seat_set_focus(node);
		transaction_commit_dirty();
		return false;
	}
	if (!cont) {
		return false;
	}

	bool is_floating_or_child = container_is_floating_or_child(cont);
	bool is_fullscreen = container_toplevel_ancestor(cont)->fullscreen_mode !=
		FULLSCREEN_NONE || cont->fullscreen_mode != FULLSCREEN_NONE;
	bool on_titlebar = hit == HIT_TITLEBAR;
	bool mod_pressed = (modifiers & config->floating_mod) != 0;
	uint32_t mod_move_btn = config->floating_mod_inverse ?
		SWINE_BTN_RIGHT : SWINE_BTN_LEFT;
	uint32_t mod_resize_btn = config->floating_mod_inverse ?
		SWINE_BTN_LEFT : SWINE_BTN_RIGHT;
	bool mod_move_btn_pressed = mod_pressed && button == mod_move_btn;
	bool mod_resize_btn_pressed = mod_pressed && button == mod_resize_btn;
	bool titlebar_left_btn_pressed = on_titlebar && button == SWINE_BTN_LEFT;

	// Resizing a tiled window with the modifier
	if (!is_floating_or_child && mod_resize_btn_pressed) {
		unsigned edge = 0;
		edge |= x > cont->box.x + cont->box.width / 2 ?
			SWINE_EDGE_RIGHT : SWINE_EDGE_LEFT;
		edge |= y > cont->box.y + cont->box.height / 2 ?
			SWINE_EDGE_BOTTOM : SWINE_EDGE_TOP;
		seat_set_focus_container(cont);
		begin_resize_tiling(cont, edge);
		return true;
	}

	// Focus the container that was clicked. On its own window Windows
	// does that; on a title bar swinewm does, as sway: clicking a tab whose
	// contents have the focus focuses the tab container itself.
	if (on_titlebar || mod_move_btn_pressed || mod_resize_btn_pressed) {
		struct swine_node *focus_node = &cont->node;
		if (on_titlebar) {
			struct swine_container *focus = seat_get_focused_container();
			if (focus == cont || !focus || !container_has_ancestor(focus, cont)) {
				focus_node = seat_get_focus_inactive(&cont->node);
			}
		}
		seat_set_focus(focus_node);
		transaction_commit_dirty();
	}

	// Moving a floating window
	if (is_floating_or_child && !is_fullscreen &&
			(mod_move_btn_pressed || titlebar_left_btn_pressed)) {
		begin_move_floating(container_toplevel_ancestor(cont));
		return true;
	}

	// Resizing a floating window with the modifier
	if (is_floating_or_child && !is_fullscreen && mod_resize_btn_pressed) {
		struct swine_container *floater = container_toplevel_ancestor(cont);
		unsigned edge = 0;
		edge |= x > floater->floating_box.x + floater->floating_box.width / 2 ?
			SWINE_EDGE_RIGHT : SWINE_EDGE_LEFT;
		edge |= y > floater->floating_box.y + floater->floating_box.height / 2 ?
			SWINE_EDGE_BOTTOM : SWINE_EDGE_TOP;
		seat_set_focus_container(floater);
		begin_resize_floating(floater, edge, modifiers);
		return true;
	}

	// Dragging a tiled window
	if (config->tiling_drag && !is_floating_or_child && !is_fullscreen &&
			(mod_move_btn_pressed || titlebar_left_btn_pressed)) {
		// By its title bar, only after the pointer moved a little.
		begin_move_tiling(cont, !mod_pressed && config->tiling_drag_threshold > 0);
		return true;
	}
	return on_titlebar;
}

bool cursor_axis(int x, int y, enum cursor_hit hit,
		struct swine_container *cont, uint32_t button, uint32_t modifiers) {
	cursor.x = x;
	cursor.y = y;
	if (!config || cursor.op != OP_DEFAULT) {
		return false;
	}
	bool handled = false;

	// Mouse bindings for buttons 4-7: press...
	state_add_button(button);
	struct swine_binding *binding =
		get_active_mouse_binding(modifiers, false, hit);
	if (binding) {
		run_binding(binding);
		handled = true;
	}

	// Scrolling on a tabbed or stacked title bar switches tabs.
	if (!handled && hit == HIT_TITLEBAR && cont) {
		struct swine_node *new_focus;
		enum swine_container_layout layout = container_parent_layout(cont);
		if (layout == L_TABBED || layout == L_STACKED) {
			struct swine_node *tabcontainer = node_get_parent(&cont->node);
			struct swine_node *active = seat_get_active_tiling_child(tabcontainer);
			list_t *siblings = container_get_siblings(cont);
			int desired = list_find(siblings,
				active ? active->container : cont);
			desired += button == SWINE_SCROLL_UP || button == SWINE_SCROLL_LEFT ?
				-1 : 1;
			if (desired < 0) {
				desired = 0;
			} else if (desired >= siblings->length) {
				desired = siblings->length - 1;
			}
			struct swine_container *new_sibling = siblings->items[desired];
			new_focus = seat_get_focus_inactive(&new_sibling->node);
		} else {
			new_focus = seat_get_focus_inactive(&cont->node);
		}
		seat_set_focus(new_focus);
		transaction_commit_dirty();
		handled = true;
	}

	// ...and release.
	binding = get_active_mouse_binding(modifiers, true, hit);
	state_erase_button(button);
	if (binding) {
		run_binding(binding);
		handled = true;
	}
	return handled;
}

bool cursor_in_operation(void) {
	return cursor.op != OP_DEFAULT;
}

void cursor_container_destroyed(struct swine_container *con) {
	if (cursor.previous_node == &con->node) {
		cursor.previous_node = NULL;
	}
	if (cursor.op == OP_DEFAULT) {
		return;
	}
	if (cursor.target_node == &con->node) {
		cursor.target_node = NULL;
	}
	if (cursor.con == con || cursor.h_sib == con || cursor.v_sib == con ||
			cursor.h_con == con || cursor.v_con == con) {
		begin_default();
	}
}

void cursor_reset(void) {
	if (cursor.op == OP_MOVE_TILING) {
		platform_drop_indicator(NULL);
	}
	memset(&cursor, 0, sizeof(cursor));
}

// --- mouse_warping ----------------------------------------------------------

void cursor_request_warp(bool force_container) {
	if (!config || config->mouse_warping == WARP_NO) {
		return;
	}
	cursor.warp_pending = true;
	cursor.warp_force = cursor.warp_force || force_container;
}

void cursor_apply_warp(void) {
	if (!cursor.warp_pending) {
		return;
	}
	bool force = cursor.warp_force;
	cursor.warp_pending = false;
	cursor.warp_force = false;
	struct swine_node *focus = seat_get_focus();
	int x, y;
	if (!config || config->mouse_warping == WARP_NO || !focus ||
			!platform_cursor_position(&x, &y)) {
		return;
	}
	if (config->mouse_warping == WARP_OUTPUT) {
		struct swine_output *output = node_get_output(focus);
		if (output && box_contains_point(&output->box, x, y)) {
			return;
		}
		force = false;
	}
	struct swine_box box;
	if (focus->type == N_CONTAINER) {
		box = focus->container->box;
		if (!force && box_contains_point(&box, x, y)) {
			return;
		}
	} else if (focus->type == N_WORKSPACE) {
		box = focus->workspace->box;
	} else {
		return;
	}
	cursor.x = box.x + box.width / 2;
	cursor.y = box.y + box.height / 2;
	cursor.previous_node = focus;
	platform_cursor_warp(cursor.x, cursor.y);
}
