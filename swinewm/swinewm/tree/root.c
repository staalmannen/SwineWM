#include <limits.h>
#include <stdlib.h>
#include "log.h"
#include "swinewm/input/seat.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/workspace.h"

struct swine_root *root = NULL;

struct swine_root *root_create(void) {
	struct swine_root *r = calloc(1, sizeof(*r));
	node_init(&r->node, N_ROOT, r);
	r->outputs = create_list();
	r->scratchpad = create_list();
	root = r;
	r->fallback_output = output_create("FALLBACK", NULL);
	return r;
}

void root_destroy(struct swine_root *r) {
	while (r->outputs->length) {
		output_destroy(r->outputs->items[r->outputs->length - 1]);
	}
	struct swine_output *fallback = r->fallback_output;
	while (fallback->workspaces->length) {
		struct swine_workspace *ws = fallback->workspaces->items[0];
		workspace_detach(ws);
		workspace_destroy(ws);
	}
	list_free(fallback->workspaces);
	free(fallback->name);
	free(fallback->description);
	free(fallback);
	for (int i = 0; i < r->scratchpad->length; ++i) {
		container_destroy(r->scratchpad->items[i]);
	}
	list_free(r->scratchpad);
	list_free(r->outputs);
	free(r);
	if (root == r) {
		root = NULL;
	}
}

void root_update_box(void) {
	if (!root->outputs->length) {
		root->box = (struct swine_box){0};
		return;
	}
	int x1 = INT_MAX, y1 = INT_MAX, x2 = INT_MIN, y2 = INT_MIN;
	for (int i = 0; i < root->outputs->length; ++i) {
		struct swine_output *o = root->outputs->items[i];
		if (o->box.x < x1) x1 = o->box.x;
		if (o->box.y < y1) y1 = o->box.y;
		if (o->box.x + o->box.width > x2) x2 = o->box.x + o->box.width;
		if (o->box.y + o->box.height > y2) y2 = o->box.y + o->box.height;
	}
	root->box = (struct swine_box){x1, y1, x2 - x1, y2 - y1};
}

void root_scratchpad_add_container(struct swine_container *con) {
	if (con->scratchpad) {
		return;
	}
	struct swine_container *parent = con->parent;
	struct swine_workspace *ws = con->workspace;
	if (con->fullscreen_mode) {
		container_set_fullscreen(con, FULLSCREEN_NONE);
	}
	if (!container_is_floating(con)) {
		container_set_floating(con, true);
	}
	con->scratchpad = true;
	list_add(root->scratchpad, con);
	container_detach(con);
	if (parent) {
		container_reap_empty(parent);
	}
	// Focus whatever was behind the window on its old workspace.
	if (ws) {
		struct swine_node *focus = seat_get_focus_inactive(&ws->node);
		seat_set_focus(focus ? focus : &ws->node);
		workspace_consider_destroy(ws);
	}
}

void root_scratchpad_remove_container(struct swine_container *con) {
	if (!con->scratchpad) {
		return;
	}
	con->scratchpad = false;
	int index = list_find(root->scratchpad, con);
	if (index != -1) {
		list_del(root->scratchpad, index);
	}
}

void root_scratchpad_show(struct swine_container *con) {
	struct swine_workspace *ws = seat_get_focused_workspace();
	if (!ws) {
		return;
	}
	// If the container was shown elsewhere, pull it over.
	if (con->workspace) {
		container_detach(con);
	}
	workspace_add_floating(ws, con);
	container_floating_move_to_center(con);
	seat_set_focus_container(con);
}

void root_scratchpad_hide(struct swine_container *con) {
	struct swine_workspace *ws = con->workspace;
	if (!ws) {
		return;
	}
	container_detach(con);
	struct swine_node *focus = seat_get_focus_inactive(&ws->node);
	seat_set_focus(focus ? focus : &ws->node);
	// Keep it at the end so "scratchpad show" cycles through windows.
	list_move_to_end(root->scratchpad, con);
}

static void for_each_output(void (*f)(struct swine_output *o, void *data),
		void *data) {
	for (int i = 0; i < root->outputs->length; ++i) {
		f(root->outputs->items[i], data);
	}
	f(root->fallback_output, data);
}

struct ws_find {
	bool (*test)(struct swine_workspace *ws, void *data);
	void *data;
	struct swine_workspace *result;
};

static void find_in_output(struct swine_output *o, void *data) {
	struct ws_find *find = data;
	for (int i = 0; !find->result && i < o->workspaces->length; ++i) {
		struct swine_workspace *ws = o->workspaces->items[i];
		if (find->test(ws, find->data)) {
			find->result = ws;
		}
	}
}

struct swine_workspace *root_find_workspace(
		bool (*test)(struct swine_workspace *ws, void *data), void *data) {
	struct ws_find find = {test, data, NULL};
	for_each_output(find_in_output, &find);
	return find.result;
}

struct ws_iter {
	void (*f)(struct swine_workspace *ws, void *data);
	void *data;
};

static void iter_output(struct swine_output *o, void *data) {
	struct ws_iter *it = data;
	for (int i = 0; i < o->workspaces->length; ++i) {
		it->f(o->workspaces->items[i], it->data);
	}
}

void root_for_each_workspace(
		void (*f)(struct swine_workspace *ws, void *data), void *data) {
	struct ws_iter it = {f, data};
	for_each_output(iter_output, &it);
}

struct con_iter {
	void (*f)(struct swine_container *con, void *data);
	void *data;
};

static void iter_ws_containers(struct swine_workspace *ws, void *data) {
	struct con_iter *it = data;
	workspace_for_each_container(ws, it->f, it->data);
}

void root_for_each_container(
		void (*f)(struct swine_container *con, void *data), void *data) {
	struct con_iter it = {f, data};
	root_for_each_workspace(iter_ws_containers, &it);
	// Hidden scratchpad containers are not on any workspace.
	for (int i = 0; i < root->scratchpad->length; ++i) {
		struct swine_container *con = root->scratchpad->items[i];
		if (!con->workspace) {
			f(con, data);
			container_for_each_child(con, f, data);
		}
	}
}
