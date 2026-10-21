#include <stdlib.h>
#include "ipc.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/criteria.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/input/seat.h"
#include "swinewm/ipc-json.h"
#include "swinewm/platform.h"
#include "swinewm/server.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/view.h"
#include "swinewm/tree/workspace.h"

struct swine_view *view_create(void *handle) {
	struct swine_view *view = calloc(1, sizeof(*view));
	view->handle = handle;
	view->executed_criteria = create_list();
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

bool view_set_title(struct swine_view *view, const char *title) {
	free(view->title);
	view->title = swine_strdup(title);
	if (view->container) {
		char *event = ipc_json_event_window("title", view->container);
		platform_ipc_event((uint32_t)IPC_EVENT_WINDOW, event);
		free(event);
	}
	// As in sway, for_window rules are checked again when the title
	// changes (each rule runs once per view).
	if (!view->container) {
		return false;
	}
	int executed = view->executed_criteria->length;
	view_execute_criteria(view);
	return view->executed_criteria->length != executed;
}

static bool view_has_executed_criteria(struct swine_view *view,
		struct criteria *criteria) {
	return list_find(view->executed_criteria, criteria) != -1;
}

void view_execute_criteria(struct swine_view *view) {
	list_t *criterias = criteria_for_view(view, CT_COMMAND);
	for (int i = 0; i < criterias->length; i++) {
		struct criteria *criteria = criterias->items[i];
		swine_log(SWINE_DEBUG, "Checking criteria %s", criteria->raw);
		if (view_has_executed_criteria(view, criteria)) {
			swine_log(SWINE_DEBUG, "Criteria already executed");
			continue;
		}
		swine_log(SWINE_DEBUG, "for_window '%s' matches view %p, cmd: '%s'",
			criteria->raw, (void *)view, criteria->cmdlist);
		list_add(view->executed_criteria, criteria);
		list_t *res_list = execute_command(criteria->cmdlist, view->container);
		for (int j = 0; j < res_list->length; ++j) {
			struct cmd_results *res = res_list->items[j];
			if (res->status != CMD_SUCCESS) {
				swine_log(SWINE_ERROR, "for_window '%s' failed: %s",
					criteria->raw, res->error ? res->error : "");
			}
		}
		free_cmd_results_list(res_list);
		if (!view->container) {
			break;   // the commands unmapped it
		}
	}
	list_free(criterias);
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
		char *name = workspace_next_name(output);
		ws = workspace_create(output, name);
		free(name);
		output->active_workspace = ws;
	}
	return ws;
}

// Whether the whole box is on some output. Checking only the midpoint lets
// dialogs that are mostly off-screen (but whose center still intersects a
// monitor) keep the bad position they chose.
static bool box_on_an_output(const struct swine_box *box) {
	for (int i = 0; i < root->outputs->length; ++i) {
		struct swine_output *o = root->outputs->items[i];
		if (box->x >= o->box.x && box->y >= o->box.y &&
				box->x + box->width <= o->box.x + o->box.width &&
				box->y + box->height <= o->box.y + o->box.height) {
			return true;
		}
	}
	return false;
}

// sway's select_workspace: an "assign" rule, else the focused workspace.
static struct swine_workspace *select_workspace(struct swine_view *view) {
	list_t *criterias = criteria_for_view(view,
		CT_ASSIGN_WORKSPACE | CT_ASSIGN_WORKSPACE_NUMBER | CT_ASSIGN_OUTPUT);
	struct swine_workspace *ws = NULL;
	for (int i = 0; i < criterias->length; ++i) {
		struct criteria *criteria = criterias->items[i];
		if (criteria->type == CT_ASSIGN_OUTPUT) {
			struct swine_output *output = output_by_name_or_id(criteria->target);
			if (output) {
				ws = output_get_active_workspace(output);
				break;
			}
		} else {
			ws = criteria->type == CT_ASSIGN_WORKSPACE_NUMBER ?
				workspace_by_number(criteria->target) :
				workspace_by_name(criteria->target);
			if (!ws) {
				if (swine_strcasecmp(criteria->target, "back_and_forth") == 0) {
					if (seat->prev_workspace_name) {
						ws = workspace_create(NULL,
							seat->prev_workspace_name);
					}
				} else {
					ws = workspace_create(NULL, criteria->target);
				}
			}
			break;
		}
	}
	list_free(criterias);
	return ws ? ws : view_target_workspace();
}

// sway's should_focus: new views take focus, unless they open elsewhere
// or a no_focus rule matches.
static bool should_focus(struct swine_view *view) {
	struct swine_container *prev_con = seat_get_focused_container();
	struct swine_workspace *prev_ws = seat_get_focused_workspace();
	struct swine_workspace *map_ws = view->container->workspace;

	if (view->container->fullscreen_mode == FULLSCREEN_GLOBAL) {
		return true;
	}
	// View opened "under" a fullscreen view should not be given focus.
	if (!map_ws || map_ws->fullscreen) {
		return false;
	}
	// Views can only take focus if they are mapped into the active workspace
	if (prev_ws != map_ws) {
		return false;
	}
	// If the view is the only one in the focused workspace, it'll get focus
	// regardless of any no_focus criteria.
	if (!view->container->parent && !prev_con) {
		int num_children = map_ws->tiling->length + map_ws->floating->length;
		if (num_children == 1) {
			return true;
		}
	}
	list_t *criterias = criteria_for_view(view, CT_NO_FOCUS);
	int len = criterias->length;
	list_free(criterias);
	return len == 0;
}

void view_map(struct swine_view *view) {
	if (view->container) {
		return;
	}
	struct swine_container *con = container_create(view);
	struct swine_workspace *ws = select_workspace(view);

	if (view->natural_floating) {
		con->border = config->floating_border;
		con->border_thickness = config->floating_border_thickness;
		workspace_add_floating(ws, con);
		con->floating_box = view->natural;
		if (con->floating_box.width <= 0 || con->floating_box.height <= 0) {
			con->floating_box.width = 640;
			con->floating_box.height = 480;
			container_floating_move_to_center(con);
		} else if (!box_on_an_output(&con->floating_box)) {
			// Dialogs keep the position they chose, unless it is off every
			// screen (placed relative to a taskbar that is not there, for
			// example); sway centers new floating windows.
			container_floating_move_to_center(con);
		}
	} else {
		con->border = config->border;
		con->border_thickness = config->border_thickness;
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
	// As in sway: "new" first, then what the rules and the focus do.
	char *event = ipc_json_event_window("new", con);
	platform_ipc_event((uint32_t)IPC_EVENT_WINDOW, event);
	free(event);
	view_execute_criteria(view);
	if (view->container && should_focus(view)) {
		seat_set_focus_container(view->container);
	}
}

void view_unmap(struct swine_view *view) {
	struct swine_container *con = view->container;
	if (!con) {
		return;
	}
	char *event = ipc_json_event_window("close", con);
	platform_ipc_event((uint32_t)IPC_EVENT_WINDOW, event);
	free(event);
	view->executed_criteria->length = 0;
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
	list_free(view->executed_criteria);
	free(view);
}

void view_set_urgent(struct swine_view *view, bool enable) {
	if (view->urgent == enable) {
		return;
	}
	if (enable) {
		if (view->urgency_denied || !view->container ||
				seat_get_focused_container() == view->container) {
			return;
		}
	}
	view->urgent = enable;
	if (enable) {
		view->urgent_since = platform_now_ms();
	}
	if (view->container && view->container->workspace) {
		workspace_detect_urgent(view->container->workspace);
	}
}

void view_request_fullscreen(struct swine_view *view, bool enable) {
	view->fullscreen_requested = enable;
	struct swine_container *con = view->container;
	if (!con) {
		return;
	}
	bool is_fullscreen = con->fullscreen_mode != FULLSCREEN_NONE;
	if (enable != is_fullscreen) {
		container_set_fullscreen(con, enable ?
			FULLSCREEN_WORKSPACE : FULLSCREEN_NONE);
	}
}

bool view_is_visible(struct swine_view *view) {
	return view->pending.visible;
}

// sway's view_is_only_visible: no sibling on the way up, except in tabbed
// and stacked containers (whose other children are hidden).
static bool view_is_only_visible(struct swine_view *view) {
	struct swine_container *con = view->container;
	while (con) {
		enum swine_container_layout layout = container_parent_layout(con);
		if (layout != L_TABBED && layout != L_STACKED) {
			list_t *siblings = container_get_siblings(con);
			if (siblings && siblings->length > 1) {
				return false;
			}
		}
		con = con->parent;
	}
	return true;
}

static bool gaps_to_edge(struct swine_workspace *ws) {
	struct side_gaps g = ws->gaps_outer;
	int inner = ws->gaps_inner;
	return g.top + inner > 0 || g.right + inner > 0 || g.bottom + inner > 0 ||
		g.left + inner > 0;
}

void view_update_decoration(struct swine_view *view) {
	struct swine_container *con = view->container;
	struct view_decoration d = {0};
	d.alpha = con->alpha;
	struct swine_container *toplevel = container_toplevel_ancestor(con);
	bool fullscreen = con->fullscreen_mode != FULLSCREEN_NONE ||
		toplevel->fullscreen_mode != FULLSCREEN_NONE;
	bool floating = container_is_floating_or_child(con);
	if (fullscreen) {
		d.square_corners = true;
		d.titlebar = false;
		view->pending.deco = d;
		return;
	}
	struct border_colors *colors = container_get_colors(con);
	bool show_border = con->border != B_NONE;
	if (!floating && con->workspace) {
		bool smart = config->hide_edge_borders_smart == ESMART_ON ||
			(config->hide_edge_borders_smart == ESMART_NO_GAPS &&
			 !gaps_to_edge(con->workspace));
		if (smart && view_is_only_visible(view)) {
			show_border = false;
		}
	}
	d.border_color = show_border ? colors->child_border : 0;
	// In a tabbed or stacked container the tab is the title bar.
	struct swine_box *tabs = con->parent ? &con->parent->title_box :
		con->workspace && !floating ? &con->workspace->title_box : NULL;
	d.titlebar = con->border == B_NORMAL && !(tabs && tabs->height > 0);
	if (d.titlebar) {
		d.title_color = colors->background;
		d.title_text_color = colors->text;
	}
	d.square_corners = !floating;
	view->pending.deco = d;
}

bool view_decoration_equal(const struct view_decoration *a,
		const struct view_decoration *b) {
	return a->border_color == b->border_color && a->titlebar == b->titlebar &&
		a->title_color == b->title_color &&
		a->title_text_color == b->title_text_color &&
		a->square_corners == b->square_corners && a->alpha == b->alpha;
}
