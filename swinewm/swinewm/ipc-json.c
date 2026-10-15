#include <stdlib.h>
#include "json.h"
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
	json_int(&w, 0);
	json_key(&w, "minor");
	json_int(&w, 0);
	json_key(&w, "patch");
	json_int(&w, 1);
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
		json_key(&w, "current_workspace");
		json_string(&w, ws ? ws->name : NULL);
		write_rect(&w, "rect", &output->box);
		write_rect(&w, "usable_rect", &output->usable);
		json_end_object(&w);
	}
	json_end_array(&w);
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

static void write_container(struct json_writer *w, struct swine_container *con);

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
