#include <stdlib.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/input/seat.h"
#include "swinewm/server.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/view.h"
#include "swinewm/tree/workspace.h"

struct swine_view *view_create(void *handle) {
	struct swine_view *view = calloc(1, sizeof(*view));
	view->handle = handle;
	list_add(server.views, view);
	return view;
}

struct swine_view *view_from_handle(void *handle) {
	for (int i = 0; i < server.views->length; ++i) {
		struct swine_view *view = server.views->items[i];
		if (view->handle == handle) {
			return view;
		}
	}
	return NULL;
}

void view_set_title(struct swine_view *view, const char *title) {
	free(view->title);
	view->title = swine_strdup(title);
}

static struct swine_workspace *view_target_workspace(void) {
	struct swine_workspace *ws = seat_get_focused_workspace();
	if (ws) {
		return ws;
	}
	struct swine_output *output = root->outputs->length ?
		root->outputs->items[0] : root->fallback_output;
	ws = output_get_active_workspace(output);
	if (!ws) {
		char *name = workspace_next_name();
		ws = workspace_create(output, name);
		free(name);
		output->active_workspace = ws;
	}
	return ws;
}

void view_map(struct swine_view *view) {
	if (view->container) {
		return;
	}
	struct swine_workspace *ws = view_target_workspace();
	struct swine_container *con = container_create(view);

	if (view->natural_floating) {
		workspace_add_floating(ws, con);
		con->floating_box = view->natural;
		if (con->floating_box.width <= 0 || con->floating_box.height <= 0) {
			con->floating_box.width = 640;
			con->floating_box.height = 480;
			container_floating_move_to_center(con);
		}
	} else {
		if (ws->fullscreen) {
			container_set_fullscreen(ws->fullscreen, FULLSCREEN_NONE);
		}
		struct swine_container *target = seat_get_focus_inactive_tiling(ws);
		if (target && target->view) {
			container_add_sibling(target, con, true);
		} else if (target) {
			container_add_child(target, con);
		} else {
			workspace_add_tiling(ws, con);
		}
	}
	swine_log(SWINE_DEBUG, "Mapped view '%s' (%s) on workspace %s",
		view->title ? view->title : "", view->app_id ? view->app_id : "?",
		ws->name);
	seat_set_focus_container(con);
}

void view_unmap(struct swine_view *view) {
	struct swine_container *con = view->container;
	if (!con) {
		return;
	}
	struct swine_container *parent = con->parent;
	struct swine_workspace *ws = con->workspace;
	container_destroy(con);
	view->container = NULL;
	if (parent) {
		container_reap_empty(parent);
	} else if (ws) {
		workspace_consider_destroy(ws);
	}
	view->current.valid = false;
}

void view_destroy(struct swine_view *view) {
	transaction_forget_view(view);
	view_unmap(view);
	int index = list_find(server.views, view);
	if (index != -1) {
		list_del(server.views, index);
	}
	free(view->title);
	free(view->app_id);
	free(view->class_name);
	free(view);
}

bool view_is_visible(struct swine_view *view) {
	return view->pending.visible;
}
