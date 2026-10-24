#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "json.h"
#include "swinewm/bar/bar.h"
#include "swinewm/config.h"
#include "swinewm/input/keyboard.h"
#include "swinewm/input/seat.h"
#include "swinewm/ipc-json.h"
#include "swinewm/platform.h"
#include "swinewm/server.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/titlebar.h"
#include "swinewm/tree/view.h"
#include "swinewm/tree/workspace.h"

static void write_container(struct json_writer *w, struct swine_container *con);

static void write_rect(struct json_writer *w, const char *key,
		const struct swine_box *box) {
	json_key(w, key);
	json_begin_object(w);
	json_key(w, "x");
	json_int(w, box->x);
	json_key(w, "y");
	json_int(w, box->y);
	json_key(w, "width");
	json_int(w, box->width);
	json_key(w, "height");
	json_int(w, box->height);
	json_end_object(w);
}

// Tree nodes, as sway's ipc_json_describe_node (Copyright (c) 2016-2017
// Drew DeVault, MIT): every node has i3's fields, filled with i3's defaults
// where they mean nothing for its type, then the fields of its type.

static const int i3_output_id = INT32_MAX;
static const int i3_scratch_id = INT32_MAX - 1;

static const char *orientation_to_str(enum swine_container_layout layout) {
	switch (layout) {
	case L_VERT:
	case L_STACKED:
		return "vertical";
	case L_HORIZ:
	case L_TABBED:
		return "horizontal";
	case L_NONE:
		break;
	}
	return "none";
}

static const char *border_to_str(enum swine_container_border border) {
	switch (border) {
	case B_NONE:
		return "none";
	case B_PIXEL:
		return "pixel";
	case B_NORMAL:
		return "normal";
	case B_CSD:
		return "csd";
	}
	return "none";
}

// The fields of sway's ipc_json_create_node. They are written in two parts
// around "nodes", "floating_nodes" and "focus", which keep their place.
struct node_json {
	long long id;
	const char *type;
	const char *orientation;
	bool has_percent;
	double percent;
	bool urgent;
	list_t *marks;               // char *, or NULL for none
	bool skip_focused;           // GET_WORKSPACES puts "focused" last
	bool focused;
	const char *layout;
	const char *border;
	int border_width;
	struct swine_box rect;
	struct swine_box deco_rect;
	struct swine_box window_rect;
	struct swine_box geometry;
	const char *name;
	bool has_window;
	long long window;
	int fullscreen_mode;
	bool sticky;
	const char *floating;        // NULL: null
	const char *scratchpad_state;
};

static void node_json_init(struct node_json *n, long long id, const char *type,
		const char *name, struct swine_node *node) {
	*n = (struct node_json){0};
	n->id = id;
	n->type = type;
	n->name = name;
	n->orientation = orientation_to_str(L_HORIZ);
	n->layout = container_layout_to_str(L_HORIZ);
	n->border = border_to_str(B_NONE);
	n->focused = node && seat_get_focus() == node;
}

static void write_node_head(struct json_writer *w, const struct node_json *n) {
	json_key(w, "id");
	json_int(w, n->id);
	json_key(w, "type");
	json_string(w, n->type);
	json_key(w, "orientation");
	json_string(w, n->orientation);
	json_key(w, "percent");
	if (n->has_percent) {
		json_double(w, n->percent);
	} else {
		json_null(w);
	}
	json_key(w, "urgent");
	json_bool(w, n->urgent);
	json_key(w, "marks");
	json_begin_array(w);
	for (int i = 0; n->marks && i < n->marks->length; ++i) {
		json_string(w, n->marks->items[i]);
	}
	json_end_array(w);
	if (!n->skip_focused) {
		json_key(w, "focused");
		json_bool(w, n->focused);
	}
	json_key(w, "layout");
	json_string(w, n->layout);
	json_key(w, "border");
	json_string(w, n->border);
	json_key(w, "current_border_width");
	json_int(w, n->border_width);
	write_rect(w, "rect", &n->rect);
	write_rect(w, "deco_rect", &n->deco_rect);
	write_rect(w, "window_rect", &n->window_rect);
	write_rect(w, "geometry", &n->geometry);
	json_key(w, "name");
	json_string(w, n->name);
	json_key(w, "window");
	if (n->has_window) {
		json_int(w, n->window);
	} else {
		json_null(w);
	}
}

static void write_node_tail(struct json_writer *w, const struct node_json *n) {
	json_key(w, "fullscreen_mode");
	json_int(w, n->fullscreen_mode);
	json_key(w, "sticky");
	json_bool(w, n->sticky);
	json_key(w, "floating");
	json_string(w, n->floating);
	json_key(w, "scratchpad_state");
	json_string(w, n->scratchpad_state);
}

static void write_focus_list(struct json_writer *w, struct swine_node *node) {
	// Children ids ordered by focus recency, like sway's "focus" array.
	json_key(w, "focus");
	json_begin_array(w);
	for (int i = 0; i < seat->focus_stack->length; ++i) {
		struct swine_node *cand = seat->focus_stack->items[i];
		if (node_get_parent(cand) == node) {
			json_int(w, (long long)cand->id);
		}
	}
	json_end_array(w);
}

static void write_empty_list(struct json_writer *w, const char *key) {
	json_key(w, key);
	json_begin_array(w);
	json_end_array(w);
}

static void write_container_list(struct json_writer *w, const char *key,
		list_t *list) {
	json_key(w, key);
	json_begin_array(w);
	for (int i = 0; list && i < list->length; ++i) {
		write_container(w, list->items[i]);
	}
	json_end_array(w);
}

static bool container_has_urgent_child(struct swine_container *con) {
	for (int i = 0; con->children && i < con->children->length; ++i) {
		struct swine_container *child = con->children->items[i];
		if ((child->view && child->view->urgent) ||
				container_has_urgent_child(child)) {
			return true;
		}
	}
	return false;
}

// The box of a container's parent, below the tabs if it is tabbed or
// stacked (swinewm's child boxes start below them).
static bool parent_content_box(struct swine_container *con,
		struct swine_box *box) {
	if (con->parent) {
		*box = con->parent->box;
	} else if (con->workspace) {
		*box = con->workspace->box;
	} else {
		return false;
	}
	enum swine_container_layout layout = container_parent_layout(con);
	if (!container_is_floating(con) &&
			(layout == L_TABBED || layout == L_STACKED)) {
		const struct swine_box *tabs = con->parent ? &con->parent->title_box :
			&con->workspace->title_box;
		box->y += tabs->height;
		box->height -= tabs->height;
	}
	return box->width != 0 && box->height != 0;
}

// sway's get_deco_rect: the title bar relative to the parent. Windows keeps
// a window's own title bar inside the window, so only the tabs of tabbed and
// stacked containers count.
static struct swine_box container_deco_rect(struct swine_container *con) {
	struct swine_box deco = {0};
	enum swine_container_layout layout = container_parent_layout(con);
	if (container_is_floating(con) || !con->workspace ||
			con->fullscreen_mode != FULLSCREEN_NONE ||
			(layout != L_TABBED && layout != L_STACKED)) {
		return deco;
	}
	const struct swine_box *tabs = con->parent ? &con->parent->title_box :
		&con->workspace->title_box;
	list_t *siblings = container_get_siblings(con);
	int count = siblings ? siblings->length : 0;
	if (tabs->height <= 0 || count == 0) {
		return deco;
	}
	int index = container_sibling_index(con);
	if (layout == L_TABBED) {
		deco.width = tabs->width / count;
		deco.height = tabs->height;
		deco.x = deco.width * index;
	} else {
		deco.width = tabs->width;
		deco.height = tabs->height / count;
		deco.y = deco.height * index;
	}
	return deco;
}

static void write_container(struct json_writer *w, struct swine_container *con) {
	struct swine_view *view = con->view;
	bool floating = container_is_floating(con);
	struct node_json n;
	node_json_init(&n, (long long)con->node.id, floating ? "floating_con" : "con",
		view ? view->title : NULL, &con->node);
	enum swine_container_layout layout = view ? L_NONE : con->layout;
	n.layout = container_layout_to_str(layout);
	n.orientation = orientation_to_str(layout);
	n.urgent = view ? view->urgent : container_has_urgent_child(con);
	n.sticky = con->is_sticky;
	// Like sway, the reason for floating is not tracked.
	n.floating = floating ? "user_on" : "auto_off";
	n.fullscreen_mode = con->fullscreen_mode;
	n.scratchpad_state = con->scratchpad ? "fresh" : "none";
	struct swine_box parent;
	if (parent_content_box(con, &parent)) {
		n.has_percent = true;
		n.percent = ((double)con->box.width / parent.width) *
			((double)con->box.height / parent.height);
	}
	n.border = border_to_str(con->border);
	n.border_width = con->border_thickness;
	n.rect = con->box;
	n.deco_rect = container_deco_rect(con);
	n.marks = con->marks;
	if (view) {
		n.window_rect = (struct swine_box){
			view->pending.box.x - con->box.x, view->pending.box.y - con->box.y,
			view->pending.box.width, view->pending.box.height};
		n.geometry = (struct swine_box){0, 0, view->natural.width,
			view->natural.height};
		// The HWND stands in for the X11 window id.
		n.has_window = true;
		n.window = (long long)(size_t)view->handle;
	}
	json_begin_object(w);
	write_node_head(w, &n);
	write_container_list(w, "nodes", con->children);
	write_empty_list(w, "floating_nodes");
	write_focus_list(w, &con->node);
	write_node_tail(w, &n);
	if (view) {
		// sway's ipc_json_describe_view
		json_key(w, "pid");
		json_int(w, (long long)view->pid);
		json_key(w, "app_id");
		json_string(w, view->app_id);
		json_key(w, "foreign_toplevel_identifier");
		json_null(w);
		json_key(w, "visible");
		json_bool(w, view_is_visible(view));
		json_key(w, "max_render_time");
		json_int(w, 0);
		json_key(w, "allow_tearing");
		json_bool(w, false);
		json_key(w, "shell");
		json_string(w, "win32");
		json_key(w, "inhibit_idle");
		json_bool(w, false);
		json_key(w, "sandbox_engine");
		json_null(w);
		json_key(w, "sandbox_app_id");
		json_null(w);
		json_key(w, "sandbox_instance_id");
		json_null(w);
		json_key(w, "tag");
		json_null(w);
		json_key(w, "idle_inhibitors");
		json_begin_object(w);
		json_key(w, "user");
		json_string(w, "none");
		json_key(w, "application");
		json_string(w, "none");
		json_end_object(w);
		// As sway does for Xwayland windows; Windows has no instance, so it
		// is the class, as for criteria.
		json_key(w, "window_properties");
		json_begin_object(w);
		if (view->class_name) {
			json_key(w, "class");
			json_string(w, view->class_name);
			json_key(w, "instance");
			json_string(w, view->class_name);
		}
		if (view->title) {
			json_key(w, "title");
			json_string(w, view->title);
		}
		json_key(w, "transient_for");
		json_null(w);
		json_end_object(w);
	}
	json_end_object(w);
}

// sway's ipc_json_describe_workspace. GET_WORKSPACES leaves out the
// children and adds "focused" (the focused workspace) and "visible" last.
static void write_workspace(struct json_writer *w, struct swine_workspace *ws,
		bool recursive) {
	struct node_json n;
	node_json_init(&n, (long long)ws->node.id, "workspace", ws->name, &ws->node);
	n.skip_focused = !recursive;
	n.urgent = ws->urgent;
	n.layout = container_layout_to_str(ws->layout);
	n.orientation = orientation_to_str(ws->layout);
	n.rect = ws->box;
	n.fullscreen_mode = 1;
	json_begin_object(w);
	write_node_head(w, &n);
	if (recursive) {
		write_container_list(w, "nodes", ws->tiling);
	} else {
		write_empty_list(w, "nodes");
	}
	write_container_list(w, "floating_nodes", ws->floating);
	write_focus_list(w, &ws->node);
	write_node_tail(w, &n);
	json_key(w, "num");
	json_int(w, workspace_number(ws->name));
	json_key(w, "output");
	json_string(w, ws->output && ws->output->enabled ? ws->output->name : NULL);
	json_key(w, "representation");
	char *representation = container_build_representation(ws->layout,
		ws->tiling);
	json_string(w, representation);
	free(representation);
	if (!recursive) {
		json_key(w, "focused");
		json_bool(w, seat_get_focused_workspace() == ws);
		json_key(w, "visible");
		json_bool(w, workspace_is_visible(ws));
	}
	json_end_object(w);
}

// sway's ipc_json_describe_enabled_output; GET_OUTPUTS leaves out the
// workspaces and adds swinewm's "description" and "usable_rect".
static void write_output(struct json_writer *w, struct swine_output *output,
		bool recursive) {
	struct swine_workspace *ws = output_get_active_workspace(output);
	struct node_json n;
	node_json_init(&n, (long long)output->node.id, "output", output->name,
		&output->node);
	n.orientation = orientation_to_str(L_NONE);
	n.layout = "output";
	n.rect = output->box;
	if (root->box.width != 0 && root->box.height != 0) {
		n.has_percent = true;
		n.percent = ((double)output->box.width / root->box.width) *
			((double)output->box.height / root->box.height);
	}
	json_begin_object(w);
	write_node_head(w, &n);
	json_key(w, "nodes");
	json_begin_array(w);
	for (int i = 0; recursive && i < output->workspaces->length; ++i) {
		write_workspace(w, output->workspaces->items[i], true);
	}
	json_end_array(w);
	write_empty_list(w, "floating_nodes");
	write_focus_list(w, &output->node);
	write_node_tail(w, &n);
	json_key(w, "primary");
	json_bool(w, output->primary);
	json_key(w, "make");
	json_string(w, "Unknown");
	json_key(w, "model");
	json_string(w, output->identifier ? output->identifier : "Unknown");
	json_key(w, "serial");
	json_string(w, "Unknown");
	json_key(w, "features");
	json_begin_object(w);
	json_key(w, "adaptive_sync");
	json_bool(w, false);
	json_key(w, "hdr");
	json_bool(w, false);
	json_end_object(w);
	json_key(w, "non_desktop");
	json_bool(w, false);
	json_key(w, "active");
	json_bool(w, true);
	json_key(w, "dpms");
	json_bool(w, true);
	json_key(w, "power");
	json_bool(w, true);
	json_key(w, "scale");
	json_double(w, output->scale);
	json_key(w, "scale_filter");
	json_string(w, "linear");
	json_key(w, "transform");
	static const char *const transforms[] = {"normal", "90", "180", "270"};
	json_string(w, transforms[output->transform & 3]);
	json_key(w, "adaptive_sync_status");
	json_string(w, "disabled");
	json_key(w, "current_workspace");
	json_string(w, ws ? ws->name : NULL);
	struct swine_box mode = {0, 0,
		output->transform & 1 ? output->box.height : output->box.width,
		output->transform & 1 ? output->box.width : output->box.height};
	json_key(w, "modes");
	json_begin_array(w);
	json_begin_object(w);
	json_key(w, "width");
	json_int(w, mode.width);
	json_key(w, "height");
	json_int(w, mode.height);
	json_key(w, "refresh");
	json_int(w, output->refresh_mhz);
	json_end_object(w);
	json_end_array(w);
	json_key(w, "current_mode");
	json_begin_object(w);
	json_key(w, "width");
	json_int(w, mode.width);
	json_key(w, "height");
	json_int(w, mode.height);
	json_key(w, "refresh");
	json_int(w, output->refresh_mhz);
	json_end_object(w);
	json_key(w, "max_render_time");
	json_int(w, 0);
	json_key(w, "allow_tearing");
	json_bool(w, false);
	json_key(w, "hdr");
	json_bool(w, false);
	if (!recursive) {
		json_key(w, "description");
		json_string(w, output->description ? output->description : "Unknown");
		struct swine_box usable = output_get_usable_area(output);
		write_rect(w, "usable_rect", &usable);
	}
	json_end_object(w);
}

// sway's ipc_json_describe_scratchpad_output: the __i3 output with the
// __i3_scratch workspace, whose floating nodes are the hidden scratchpad.
static void write_scratchpad_output(struct json_writer *w) {
	struct node_json n;
	node_json_init(&n, i3_scratch_id, "workspace", "__i3_scratch", NULL);
	n.rect = root->box;
	n.fullscreen_mode = 1;
	struct node_json out;
	node_json_init(&out, i3_output_id, "output", "__i3", NULL);
	out.layout = "output";
	out.rect = root->box;

	json_begin_object(w);
	write_node_head(w, &out);
	json_key(w, "nodes");
	json_begin_array(w);
	json_begin_object(w);
	write_node_head(w, &n);
	write_empty_list(w, "nodes");
	json_key(w, "floating_nodes");
	json_begin_array(w);
	for (int i = 0; i < root->scratchpad->length; ++i) {
		struct swine_container *con = root->scratchpad->items[i];
		if (container_is_scratchpad_hidden(con)) {
			write_container(w, con);
		}
	}
	json_end_array(w);
	json_key(w, "focus");
	json_begin_array(w);
	for (int i = root->scratchpad->length - 1; i >= 0; --i) {
		struct swine_container *con = root->scratchpad->items[i];
		json_int(w, (long long)con->node.id);
	}
	json_end_array(w);
	write_node_tail(w, &n);
	json_end_object(w);
	json_end_array(w);
	write_empty_list(w, "floating_nodes");
	json_key(w, "focus");
	json_begin_array(w);
	json_int(w, i3_scratch_id);
	json_end_array(w);
	write_node_tail(w, &out);
	json_end_object(w);
}

char *ipc_json_get_version(void) {
	struct json_writer w;
	json_init(&w);
	json_begin_object(&w);
	json_key(&w, "human_readable");
	json_string(&w, SWINEWM_VERSION);
	json_key(&w, "variant");
	json_string(&w, "swinewm");
	json_key(&w, "major");
	json_int(&w, SWINEWM_VERSION_MAJOR);
	json_key(&w, "minor");
	json_int(&w, SWINEWM_VERSION_MINOR);
	json_key(&w, "patch");
	json_int(&w, SWINEWM_VERSION_PATCH);
	json_key(&w, "loaded_config_file_name");
	json_string(&w, config->current_config_path);
	json_end_object(&w);
	return json_finish(&w);
}

static void write_workspace_summary(struct swine_workspace *ws, void *data) {
	if (ws->output && ws->output->enabled) {
		write_workspace(data, ws, false);
	}
}

char *ipc_json_get_workspaces(void) {
	struct json_writer w;
	json_init(&w);
	json_begin_array(&w);
	root_for_each_workspace(write_workspace_summary, &w);
	json_end_array(&w);
	return json_finish(&w);
}

char *ipc_json_event_workspace(const char *change,
		struct swine_workspace *current, struct swine_workspace *old) {
	struct json_writer w;
	json_init(&w);
	json_begin_object(&w);
	json_key(&w, "change");
	json_string(&w, change);
	json_key(&w, "current");
	if (current && current->output && current->output->enabled) {
		write_workspace(&w, current, true);
	} else {
		json_null(&w);
	}
	json_key(&w, "old");
	if (old && old->output && old->output->enabled) {
		write_workspace(&w, old, true);
	} else {
		json_null(&w);
	}
	json_end_object(&w);
	return json_finish(&w);
}

char *ipc_json_get_outputs(void) {
	struct json_writer w;
	json_init(&w);
	json_begin_array(&w);
	for (int i = 0; i < root->outputs->length; ++i) {
		write_output(&w, root->outputs->items[i], false);
	}
	json_end_array(&w);
	return json_finish(&w);
}

char *ipc_json_event_window(const char *change, struct swine_container *con) {
	struct json_writer w;
	json_init(&w);
	json_begin_object(&w);
	json_key(&w, "change");
	json_string(&w, change);
	json_key(&w, "container");
	if (con) {
		write_container(&w, con);
	} else {
		json_null(&w);
	}
	json_end_object(&w);
	return json_finish(&w);
}

// sway's mode event: "change" is the name of the new mode.
char *ipc_json_event_mode(const char *name, bool pango_markup) {
	struct json_writer w;
	json_init(&w);
	json_begin_object(&w);
	json_key(&w, "change");
	json_string(&w, name);
	json_key(&w, "pango_markup");
	json_bool(&w, pango_markup);
	json_end_object(&w);
	return json_finish(&w);
}

char *ipc_json_event_output(const char *change) {
	struct json_writer w;
	json_init(&w);
	json_begin_object(&w);
	json_key(&w, "change");
	json_string(&w, change);
	json_end_object(&w);
	return json_finish(&w);
}

char *ipc_json_event_binding(const struct swine_binding *binding,
		const char *event) {
	static const uint32_t modifier_bits[] = {
		SWINE_MOD_SHIFT, SWINE_MOD_CAPS, SWINE_MOD_CTRL, SWINE_MOD_ALT,
		SWINE_MOD_MOD2, SWINE_MOD_MOD3, SWINE_MOD_LOGO, SWINE_MOD_MOD5,
	};
	struct json_writer w;
	json_init(&w);
	json_begin_object(&w);
	json_key(&w, "change");
	json_string(&w, "run");
	json_key(&w, "binding");
	json_begin_object(&w);
	json_key(&w, "command");
	json_string(&w, binding->command);
	json_key(&w, "event_state_mask");
	json_int(&w, 0);
	json_key(&w, "input_code");
	json_int(&w, binding->keycode);
	json_key(&w, "symbol");
	json_string(&w, strcmp(event, "keyboard") == 0 ?
		keycode_to_name(binding->keycode) : NULL);
	json_key(&w, "input_type");
	json_string(&w, event);
	json_key(&w, "input");
	json_string(&w, binding->input);
	json_key(&w, "mods");
	json_begin_array(&w);
	for (size_t i = 0; i < sizeof(modifier_bits) / sizeof(modifier_bits[0]); ++i) {
		if (binding->modifiers & modifier_bits[i]) {
			json_string(&w, get_modifier_name_by_mask(modifier_bits[i]));
		}
	}
	json_end_array(&w);
	json_end_object(&w);
	json_end_object(&w);
	return json_finish(&w);
}

char *ipc_json_get_tree(void) {
	struct node_json n;
	node_json_init(&n, (long long)root->node.id, "root", "root", &root->node);
	n.rect = root->box;
	struct json_writer w;
	json_init(&w);
	json_begin_object(&w);
	write_node_head(&w, &n);
	json_key(&w, "nodes");
	json_begin_array(&w);
	write_scratchpad_output(&w);
	for (int i = 0; i < root->outputs->length; ++i) {
		write_output(&w, root->outputs->items[i], true);
	}
	json_end_array(&w);
	write_empty_list(&w, "floating_nodes");
	write_focus_list(&w, &root->node);
	write_node_tail(&w, &n);
	json_end_object(&w);
	return json_finish(&w);
}

char *ipc_json_get_binding_modes(void) {
	struct json_writer w;
	json_init(&w);
	json_begin_array(&w);
	for (int i = 0; i < config->modes->length; ++i) {
		struct swine_mode *mode = config->modes->items[i];
		json_string(&w, mode->name);
	}
	json_end_array(&w);
	return json_finish(&w);
}

char *ipc_json_get_binding_state(void) {
	struct json_writer w;
	json_init(&w);
	json_begin_object(&w);
	json_key(&w, "name");
	json_string(&w, config->current_mode->name);
	json_end_object(&w);
	return json_finish(&w);
}

static void write_input(struct json_writer *w, const char *identifier,
		const char *name, const char *type) {
	json_begin_object(w);
	json_key(w, "identifier");
	json_string(w, identifier);
	json_key(w, "name");
	json_string(w, name);
	json_key(w, "vendor");
	json_int(w, 0);
	json_key(w, "product");
	json_int(w, 0);
	json_key(w, "type");
	json_string(w, type);
	if (strcmp(type, "keyboard") == 0) {
		// sway's xkb layouts: the layouts installed in Windows
		list_t *layouts = keyboard_layouts_get();
		int active = keyboard_layouts_active();
		json_key(w, "xkb_layout_names");
		json_begin_array(w);
		for (int i = 0; layouts && i < layouts->length; ++i) {
			struct platform_keyboard_layout *l = layouts->items[i];
			json_string(w, l->name);
		}
		json_end_array(w);
		if (active >= 0) {
			struct platform_keyboard_layout *l = layouts->items[active];
			json_key(w, "xkb_active_layout_index");
			json_int(w, active);
			json_key(w, "xkb_active_layout_name");
			json_string(w, l->name);
		}
	}
	json_end_object(w);
}

// sway's input event: the keyboard after its layout changed.
char *ipc_json_event_input(const char *change) {
	struct json_writer w;
	json_init(&w);
	json_begin_object(&w);
	json_key(&w, "change");
	json_string(&w, change);
	json_key(&w, "input");
	write_input(&w, "swinewm:keyboard", "Windows keyboard hook", "keyboard");
	json_end_object(&w);
	return json_finish(&w);
}

char *ipc_json_get_inputs(void) {
	uint32_t capabilities = platform_ipc_input_capabilities();
	struct json_writer w;
	json_init(&w);
	json_begin_array(&w);
	if (capabilities & PLATFORM_IPC_INPUT_KEYBOARD) {
		write_input(&w, "swinewm:keyboard",
			"Windows keyboard hook", "keyboard");
	}
	if (capabilities & PLATFORM_IPC_INPUT_POINTER) {
		write_input(&w, "swinewm:mouse", "Windows mouse hook", "pointer");
	}
	json_end_array(&w);
	return json_finish(&w);
}

char *ipc_json_get_seats(void) {
	struct swine_node *focus = seat_get_focus();
	uint32_t capabilities = platform_ipc_input_capabilities();
	int seat_capabilities = 0;
	if (capabilities & PLATFORM_IPC_INPUT_POINTER) {
		seat_capabilities |= 1;
	}
	if (capabilities & PLATFORM_IPC_INPUT_KEYBOARD) {
		seat_capabilities |= 2;
	}
	struct json_writer w;
	json_init(&w);
	json_begin_array(&w);
	json_begin_object(&w);
	json_key(&w, "name");
	json_string(&w, "seat0");
	json_key(&w, "capabilities");
	json_int(&w, seat_capabilities);
	json_key(&w, "focus");
	json_int(&w, focus ? (long long)focus->id : 0);
	json_key(&w, "devices");
	json_begin_array(&w);
	if (capabilities & PLATFORM_IPC_INPUT_KEYBOARD) {
		write_input(&w, "swinewm:keyboard",
			"Windows keyboard hook", "keyboard");
	}
	if (capabilities & PLATFORM_IPC_INPUT_POINTER) {
		write_input(&w, "swinewm:mouse", "Windows mouse hook", "pointer");
	}
	json_end_array(&w);
	json_end_object(&w);
	json_end_array(&w);
	return json_finish(&w);
}

char *ipc_json_get_config(void) {
	struct json_writer w;
	json_init(&w);
	json_begin_object(&w);
	json_key(&w, "config");
	json_string(&w, config->current_config ? config->current_config : "");
	json_end_object(&w);
	return json_finish(&w);
}

static void collect_marks(struct swine_container *con, void *data) {
	struct json_writer *w = data;
	for (int i = 0; i < con->marks->length; ++i) {
		json_string(w, con->marks->items[i]);
	}
}

char *ipc_json_get_marks(void) {
	struct json_writer w;
	json_init(&w);
	json_begin_array(&w);
	root_for_each_container(collect_marks, &w);
	json_end_array(&w);
	return json_finish(&w);
}

// get_bar_config, as in sway-ipc(7). swinewm has one bar, "bar-0", drawn
// from the Waybar config; its colours and font come from style.css so that
// tools that theme themselves after the bar (swinemenu) match it.
static void write_color(struct json_writer *w, const char *key,
		struct swine_color c) {
	char text[16];
	snprintf(text, sizeof(text), "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
	json_key(w, key);
	json_string(w, text);
}

// What a button looks like: transparent backgrounds show the bar's.
static void write_button_colors(struct json_writer *w, const char *prefix,
		const struct swine_style *button, const struct swine_style *window) {
	struct swine_color bg = button->background.a ?
		color_blend(button->background, window->background) :
		window->background;
	struct swine_color border = button->inset_bottom || button->inset_top ?
		button->inset_color : button->border_bottom.width ?
		button->border_bottom.color : bg;
	char key[64];
	snprintf(key, sizeof(key), "%s_border", prefix);
	write_color(w, key, border);
	snprintf(key, sizeof(key), "%s_bg", prefix);
	write_color(w, key, bg);
	snprintf(key, sizeof(key), "%s_text", prefix);
	write_color(w, key, button->color);
}

char *ipc_json_get_bar_config(const char *id) {
	struct json_writer w;
	json_init(&w);
	if (!id || !*id) {
		json_begin_array(&w);
		json_string(&w, "bar-0");
		json_end_array(&w);
		return json_finish(&w);
	}
	if (strcmp(id, "bar-0") != 0) {
		json_begin_object(&w);
		json_key(&w, "success");
		json_bool(&w, false);
		json_key(&w, "error");
		json_string(&w, "No bar with that ID");
		json_end_object(&w);
		return json_finish(&w);
	}
	struct bar_theme theme;
	bar_get_theme(&theme);
	struct swine_color window_bg = theme.window.background;
	window_bg.a = window_bg.a ? window_bg.a : 255;
	theme.window.background = window_bg;

	json_begin_object(&w);
	json_key(&w, "id");
	json_string(&w, "bar-0");
	json_key(&w, "mode");
	json_string(&w, !bar_enabled() ? "invisible" : "dock");
	json_key(&w, "hidden_state");
	json_string(&w, "hide");
	json_key(&w, "position");
	json_string(&w, bar_at_top() ? "top" : "bottom");
	json_key(&w, "status_command");
	json_null(&w);
	// sway gives Pango font descriptions: family and size in points.
	char font[200];
	snprintf(font, sizeof(font), "pango:%s %g", theme.window.font_family,
		theme.window.font_size * 0.75);
	json_key(&w, "font");
	json_string(&w, font);
	json_key(&w, "bar_height");
	json_int(&w, bar_height());
	json_key(&w, "workspace_buttons");
	json_bool(&w, true);
	json_key(&w, "binding_mode_indicator");
	json_bool(&w, true);
	json_key(&w, "pango_markup");
	json_bool(&w, false);
	json_key(&w, "colors");
	json_begin_object(&w);
	write_color(&w, "background", window_bg);
	write_color(&w, "statusline", theme.window.color);
	write_color(&w, "separator", theme.window.color);
	write_color(&w, "focused_background", window_bg);
	write_color(&w, "focused_statusline", theme.window.color);
	write_color(&w, "focused_separator", theme.window.color);
	write_button_colors(&w, "focused_workspace", &theme.focused, &theme.window);
	write_button_colors(&w, "active_workspace", &theme.workspace, &theme.window);
	write_button_colors(&w, "inactive_workspace", &theme.workspace,
		&theme.window);
	write_button_colors(&w, "urgent_workspace", &theme.urgent, &theme.window);
	write_button_colors(&w, "binding_mode", &theme.urgent, &theme.window);
	json_end_object(&w);
	json_end_object(&w);
	return json_finish(&w);
}
