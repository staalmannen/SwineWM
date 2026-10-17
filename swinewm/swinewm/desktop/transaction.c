#include "log.h"
#include "swinewm/bar/bar.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/input/cursor.h"
#include "swinewm/input/seat.h"
#include "swinewm/platform.h"
#include "swinewm/server.h"
#include "swinewm/tree/arrange.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/workspace.h"
#include "swinewm/tree/view.h"

static struct swine_view *last_activated = NULL;

static void detect_urgent(struct swine_workspace *ws, void *data) {
	(void)data;
	workspace_detect_urgent(ws);
}

void transaction_commit_dirty(void) {
	// Windows may have moved between workspaces since the last commit.
	root_for_each_workspace(detect_urgent, NULL);
	bar_update_reservations();
	arrange_root();

	// Hide first, then move, then show: the user never sees a window from
	// the old workspace drawn over the new layout.
	for (int i = 0; i < server.views->length; ++i) {
		struct swine_view *view = server.views->items[i];
		if (!view->container) {
			continue;
		}
		if (!view->pending.visible &&
				(!view->current.valid || view->current.visible)) {
			platform_view_set_visible(view, false);
			view->current.visible = false;
		}
	}
	for (int i = 0; i < server.views->length; ++i) {
		struct swine_view *view = server.views->items[i];
		if (!view->container || !view->pending.visible) {
			continue;
		}
		if (!view->current.valid ||
				!box_equal(&view->current.box, &view->pending.box)) {
			platform_view_set_geometry(view, &view->pending.box);
			view->current.box = view->pending.box;
		}
	}
	for (int i = 0; i < server.views->length; ++i) {
		struct swine_view *view = server.views->items[i];
		if (!view->container || !view->pending.visible) {
			continue;
		}
		if (!view->current.valid || !view->current.visible) {
			platform_view_set_visible(view, true);
			view->current.visible = true;
		}
		view->current.valid = true;
	}
	for (int i = 0; i < server.views->length; ++i) {
		struct swine_view *view = server.views->items[i];
		if (view->container && !view->pending.visible) {
			view->current.valid = true;
		}
	}

	// Ask the platform to activate the focused window if focus moved.
	struct swine_container *focus = seat_get_focused_container();
	struct swine_view *view = focus ? focus->view : NULL;
	if (!view && focus) {
		struct swine_container *inactive =
			seat_get_focus_inactive_view(&focus->node);
		view = inactive ? inactive->view : NULL;
	}
	if (view && view != last_activated && view->pending.visible) {
		if (!platform_view_activate(view)) {
			swine_log(SWINE_DEBUG, "Activation of '%s' was refused",
				view->title ? view->title : "");
		}
		last_activated = view;
	} else if (!view) {
		last_activated = NULL;
	}
	cursor_apply_warp();
	platform_ui_refresh();
}

// Called by the platform when the OS reports a focus change, so that the
// next commit does not try to re-activate the same window.
void transaction_note_activated(struct swine_view *view) {
	last_activated = view;
}

void transaction_forget_view(struct swine_view *view) {
	if (last_activated == view) {
		last_activated = NULL;
	}
}
