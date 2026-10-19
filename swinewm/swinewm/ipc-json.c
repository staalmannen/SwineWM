#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "json.h"
#include "swinewm/bar/bar.h"
#include "swinewm/config.h"
#include "swinewm/input/keyboard.h"
#include "swinewm/input/seat.h"
#include "swinewm/ipc-json.h"
#include "swinewm/server.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
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
	struct json_writer *w = data;
	if (!ws->output || !ws->output->enabled) {
		return;
	}
	json_begin_object(w);
	json_key(w, "id");
	json_int(w, (long long)ws->node.id);
	json_key(w, "type");
	json_string(w, "workspace");
	json_key(w, "num");
	json_int(w, workspace_number(ws->name));
	json_key(w, "name");
	json_string(w, ws->name);
	json_key(w, "visible");
	json_bool(w, workspace_is_visible(ws));
	json_key(w, "focused");
	json_bool(w, seat_get_focused_workspace() == ws);
	json_key(w, "urgent");
	json_bool(w, ws->urgent);
	json_key(w, "layout");
	json_string(w, container_layout_to_str(ws->layout));
	json_key(w, "output");
	json_string(w, ws->output->name);
	write_rect(w, "rect", &ws->box);
	json_end_object(w);
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
		write_workspace_summary(current, &w);
	} else {
		json_null(&w);
	}
	json_key(&w, "old");
	if (old && old->output && old->output->enabled) {
		write_workspace_summary(old, &w);
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
		struct swine_output *output = root->outputs->items[i];
		struct swine_workspace *ws = output_get_active_workspace(output);
		json_begin_object(&w);
		json_key(&w, "id");
		json_int(&w, (long long)output->node.id);
		json_key(&w, "type");
		json_string(&w, "output");
		json_key(&w, "name");
		json_string(&w, output->name);
		json_key(&w, "make");
		json_string(&w, "Unknown");
		json_key(&w, "model");
		json_string(&w, output->identifier ? output->identifier : "Unknown");
		json_key(&w, "serial");
		json_string(&w, "Unknown");
		json_key(&w, "description");
		json_string(&w, output->description ? output->description : "Unknown");
		json_key(&w, "active");
		json_bool(&w, true);
		json_key(&w, "primary");
		json_bool(&w, output->primary);
		json_key(&w, "focused");
		json_bool(&w, seat_get_focused_workspace() &&
			seat_get_focused_workspace()->output == output);
		json_key(&w, "scale");
		json_double(&w, output->scale);
		json_key(&w, "transform");
		static const char *transforms[] = {"normal", "90", "180", "270"};
		json_string(&w, transforms[output->transform & 3]);
		json_key(&w, "current_mode");
		json_begin_object(&w);
		json_key(&w, "width");
		json_int(&w, output->transform & 1 ?
			output->box.height : output->box.width);
		json_key(&w, "height");
		json_int(&w, output->transform & 1 ?
			output->box.width : output->box.height);
		json_key(&w, "refresh");
		json_int(&w, output->refresh_mhz);
		json_end_object(&w);
		json_key(&w, "current_workspace");
		json_string(&w, ws ? ws->name : NULL);
		write_rect(&w, "rect", &output->box);
		struct swine_box usable = output_get_usable_area(output);
		write_rect(&w, "usable_rect", &usable);
		json_end_object(&w);
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

char *ipc_json_event_mode(const char *change, const char *name) {
	struct json_writer w;
	json_init(&w);
	json_begin_object(&w);
	json_key(&w, "change");
	json_string(&w, change);
	json_key(&w, "pango_markup");
	json_bool(&w, false);
	json_key(&w, "name");
	json_string(&w, name);
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

static void write_container_list(struct json_writer *w, const char *key,
		list_t *list) {
	json_key(w, key);
	json_begin_array(w);
	for (int i = 0; list && i < list->length; ++i) {
		write_container(w, list->items[i]);
	}
	json_end_array(w);
}

static void write_container(struct json_writer *w, struct swine_container *con) {
	json_begin_object(w);
	json_key(w, "id");
	json_int(w, (long long)con->node.id);
	json_key(w, "type");
	json_string(w, container_is_floating(con) ? "floating_con" : "con");
	json_key(w, "name");
	json_string(w, con->view ? con->view->title : NULL);
	json_key(w, "layout");
	json_string(w, con->view ? "none" : container_layout_to_str(con->layout));
	json_key(w, "focused");
	json_bool(w, seat_get_focus() == &con->node);
	json_key(w, "fullscreen_mode");
	json_int(w, con->fullscreen_mode);
	json_key(w, "percent");
	json_double(w, container_parent_layout(con) == L_VERT ?
		con->height_fraction : con->width_fraction);
	write_rect(w, "rect", &con->box);
	json_key(w, "marks");
	json_begin_array(w);
	for (int i = 0; i < con->marks->length; ++i) {
		json_string(w, con->marks->items[i]);
	}
	json_end_array(w);
	json_key(w, "sticky");
	json_bool(w, con->is_sticky);
	if (con->view) {
		static const char *const borders[] = {"none", "pixel", "normal", "csd"};
		json_key(w, "border");
		json_string(w, borders[con->border]);
		json_key(w, "current_border_width");
		json_int(w, con->border == B_NONE ? 0 : con->border_thickness);
	}
	if (con->view) {
		json_key(w, "visible");
		json_bool(w, con->view->pending.visible);
		json_key(w, "app_id");
		json_string(w, con->view->app_id);
		json_key(w, "pid");
		json_int(w, (long long)con->view->pid);
		json_key(w, "window");
		json_int(w, (long long)(size_t)con->view->handle);
		json_key(w, "window_properties");
		json_begin_object(w);
		json_key(w, "class");
		json_string(w, con->view->class_name);
		json_key(w, "title");
		json_string(w, con->view->title);
		json_end_object(w);
	}
	write_focus_list(w, &con->node);
	write_container_list(w, "nodes", con->children);
	json_key(w, "floating_nodes");
	json_begin_array(w);
	json_end_array(w);
	json_end_object(w);
}

static void write_workspace(struct json_writer *w, struct swine_workspace *ws) {
	json_begin_object(w);
	json_key(w, "id");
	json_int(w, (long long)ws->node.id);
	json_key(w, "type");
	json_string(w, "workspace");
	json_key(w, "name");
	json_string(w, ws->name);
	json_key(w, "num");
	json_int(w, workspace_number(ws->name));
	json_key(w, "layout");
	json_string(w, container_layout_to_str(ws->layout));
	json_key(w, "focused");
	json_bool(w, seat_get_focus() == &ws->node);
	write_rect(w, "rect", &ws->box);
	write_focus_list(w, &ws->node);
	write_container_list(w, "nodes", ws->tiling);
	write_container_list(w, "floating_nodes", ws->floating);
	json_end_object(w);
}

static void write_output(struct json_writer *w, struct swine_output *output) {
	json_begin_object(w);
	json_key(w, "id");
	json_int(w, (long long)output->node.id);
	json_key(w, "type");
	json_string(w, "output");
	json_key(w, "name");
	json_string(w, output->name);
	write_rect(w, "rect", &output->box);
	json_key(w, "nodes");
	json_begin_array(w);
	for (int i = 0; i < output->workspaces->length; ++i) {
		write_workspace(w, output->workspaces->items[i]);
	}
	json_end_array(w);
	json_end_object(w);
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
	struct json_writer w;
	json_init(&w);
	json_begin_object(&w);
	json_key(&w, "id");
	json_int(&w, (long long)root->node.id);
	json_key(&w, "type");
	json_string(&w, "root");
	json_key(&w, "name");
	json_string(&w, "root");
	write_rect(&w, "rect", &root->box);
	json_key(&w, "nodes");
	json_begin_array(&w);
	// sway exposes the scratchpad as the __i3 output; mirror that.
	json_begin_object(&w);
	json_key(&w, "type");
	json_string(&w, "output");
	json_key(&w, "name");
	json_string(&w, "__i3");
	json_key(&w, "nodes");
	json_begin_array(&w);
	json_begin_object(&w);
	json_key(&w, "type");
	json_string(&w, "workspace");
	json_key(&w, "name");
	json_string(&w, "__i3_scratch");
	write_container_list(&w, "floating_nodes", root->scratchpad);
	json_end_object(&w);
	json_end_array(&w);
	json_end_object(&w);
	for (int i = 0; i < root->outputs->length; ++i) {
		write_output(&w, root->outputs->items[i]);
	}
	json_end_array(&w);
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
