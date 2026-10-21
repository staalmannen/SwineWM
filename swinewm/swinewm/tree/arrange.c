#include <math.h>
#include <limits.h>
#include "swinewm/config.h"
#include "swinewm/input/seat.h"
#include "swinewm/server.h"
#include "swinewm/tree/arrange.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/titlebar.h"
#include "swinewm/tree/view.h"
#include "swinewm/tree/workspace.h"

#define MIN_SANE_W 100
#define MIN_SANE_H 60

static void arrange_container(struct swine_container *con, bool visible);

static int inner_gap_for(struct swine_container *child) {
	struct swine_workspace *ws = child->workspace;
	int gap = ws ? ws->gaps_inner : 0;
	// Descendants of tabbed/stacked containers don't have gaps
	for (struct swine_container *temp = child; temp; temp = temp->parent) {
		enum swine_container_layout layout = container_parent_layout(temp);
		if (layout == L_TABBED || layout == L_STACKED) {
			return 0;
		}
	}
	return gap;
}

// Shared by the horizontal and vertical cases; "fraction" selects
// width_fraction or height_fraction. Follows sway's apply_horiz_layout.
static void apply_linear_layout(list_t *children, const struct swine_box *parent,
		bool horizontal) {
	if (!children->length) {
		return;
	}
	int new_children = 0;
	double current_fraction = 0;
	for (int i = 0; i < children->length; ++i) {
		struct swine_container *child = children->items[i];
		double f = horizontal ? child->width_fraction : child->height_fraction;
		current_fraction += f;
		if (f <= 0) {
			new_children += 1;
		}
	}
	double total_fraction = 0;
	for (int i = 0; i < children->length; ++i) {
		struct swine_container *child = children->items[i];
		double *f = horizontal ? &child->width_fraction : &child->height_fraction;
		if (*f <= 0) {
			if (current_fraction <= 0) {
				*f = 1.0;
			} else if (children->length > new_children) {
				*f = current_fraction / (children->length - new_children);
			} else {
				*f = current_fraction;
			}
		}
		total_fraction += *f;
	}
	for (int i = 0; i < children->length; ++i) {
		struct swine_container *child = children->items[i];
		double *f = horizontal ? &child->width_fraction : &child->height_fraction;
		*f /= total_fraction;
	}

	int parent_len = horizontal ? parent->width : parent->height;
	int min_len = horizontal ? MIN_SANE_W : MIN_SANE_H;
	double inner_gap = inner_gap_for(children->items[0]);
	double total_gap = 0;
	if (children->length > 1) {
		total_gap = fmin(inner_gap * (children->length - 1),
			fmax(0, parent_len - min_len * children->length));
		inner_gap = floor(total_gap / (children->length - 1));
	}
	double child_total = parent_len - total_gap;

	double pos = horizontal ? parent->x : parent->y;
	for (int i = 0; i < children->length; ++i) {
		struct swine_container *child = children->items[i];
		double f = horizontal ? child->width_fraction : child->height_fraction;
		int len = (int)round(f * child_total);
		if (i == children->length - 1) {
			len = (horizontal ? parent->x + parent->width :
				parent->y + parent->height) - (int)pos;
		}
		if (horizontal) {
			child->box = (struct swine_box){(int)pos, parent->y, len, parent->height};
		} else {
			child->box = (struct swine_box){parent->x, (int)pos, parent->width, len};
		}
		pos += len + inner_gap;
	}
}

static struct swine_box *title_box_of(struct swine_node *node) {
	return node->type == N_WORKSPACE ? &node->workspace->title_box :
		&node->container->title_box;
}

// Tabbed and stacked: the title bars on top (one row of tabs, or one row
// per child), the active child below them; the others are hidden.
static struct swine_node *apply_tabbed_layout(enum swine_container_layout layout,
		list_t *children, const struct swine_box *box,
		struct swine_node *parent, bool visible) {
	struct swine_node *active = seat_get_active_tiling_child(parent);
	if (!active && children->length) {
		struct swine_container *first = children->items[0];
		active = &first->node;
	}
	struct swine_output *output = node_get_output(parent);
	double scale = output && output->scale > 0 ? output->scale : 1;
	int tab_height = (int)(container_titlebar_height() * scale + 0.5);
	int rows = layout == L_STACKED ? children->length : 1;
	if (config->hide_lone_tab && children->length == 1) {
		rows = 0;
	}
	int title = tab_height * rows;
	if (title > box->height - MIN_SANE_H) {
		title = box->height > MIN_SANE_H ? box->height - MIN_SANE_H : 0;
	}
	struct swine_box *title_box = title_box_of(parent);
	*title_box = (struct swine_box){box->x, box->y, box->width, title};
	for (int i = 0; i < children->length; ++i) {
		struct swine_container *child = children->items[i];
		child->box = (struct swine_box){box->x, box->y + title, box->width,
			box->height - title};
	}
	if (visible && title > 0 && children->length) {
		titlebars_add(parent, children, title_box, layout == L_STACKED,
			rows ? title / rows : 0, scale);
	}
	return active;
}

static void arrange_children(enum swine_container_layout layout,
		list_t *children, const struct swine_box *box,
		struct swine_node *parent, bool visible) {
	struct swine_node *active = NULL;
	*title_box_of(parent) = (struct swine_box){0};
	switch (layout) {
	case L_VERT:
		apply_linear_layout(children, box, false);
		break;
	case L_TABBED:
	case L_STACKED:
		active = apply_tabbed_layout(layout, children, box, parent, visible);
		break;
	case L_HORIZ:
	case L_NONE:
	default:
		apply_linear_layout(children, box, true);
		break;
	}
	for (int i = 0; i < children->length; ++i) {
		struct swine_container *child = children->items[i];
		bool child_visible = visible && (!active || active == &child->node);
		arrange_container(child, child_visible);
	}
}

static void arrange_container(struct swine_container *con, bool visible) {
	if (con->view) {
		con->view->pending.box = con->box;
		con->view->pending.visible = visible;
		return;
	}
	arrange_children(con->layout, con->children, &con->box, &con->node, visible);
}

static void hide_container(struct swine_container *con, void *data) {
	(void)data;
	if (con->view) {
		con->view->pending.visible = false;
	}
}

static void arrange_workspace(struct swine_workspace *ws) {
	bool visible = workspace_is_visible(ws);
	struct swine_output *output = ws->output;
	if (!visible || !output) {
		workspace_for_each_container(ws, hide_container, NULL);
		return;
	}
	struct swine_box area = output_get_usable_area(output);
	// sway's workspace_add_gaps: smart_gaps drops the gaps around a lone
	// window, inverse_outer the outer gaps when there is more than one.
	struct side_gaps outer = ws->gaps_outer;
	int inner = ws->gaps_inner;
	bool single = config->smart_gaps != SMART_GAPS_OFF &&
		workspace_has_single_visible_container(ws);
	if (config->smart_gaps == SMART_GAPS_ON && single) {
		outer = (struct side_gaps){0, 0, 0, 0};
		inner = 0;
	} else if (config->smart_gaps == SMART_GAPS_INVERSE_OUTER && !single) {
		outer = (struct side_gaps){0, 0, 0, 0};
	}
	long long x = (long long)area.x + outer.left + inner;
	long long y = (long long)area.y + outer.top + inner;
	long long width = (long long)area.width - outer.left -
		outer.right - 2LL * inner;
	long long height = (long long)area.height - outer.top -
		outer.bottom - 2LL * inner;
	if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX ||
			width < MIN_SANE_W || width > INT_MAX ||
			height < MIN_SANE_H || height > INT_MAX) {
		area = output_get_usable_area(output);
	} else {
		area.x = (int)x;
		area.y = (int)y;
		area.width = (int)width;
		area.height = (int)height;
	}
	ws->box = area;

	if (ws->fullscreen) {
		workspace_for_each_container(ws, hide_container, NULL);
		ws->fullscreen->box = output->box;
		arrange_container(ws->fullscreen, true);
		return;
	}

	arrange_children(ws->layout, ws->tiling, &area, &ws->node, true);
	for (int i = 0; i < ws->floating->length; ++i) {
		struct swine_container *con = ws->floating->items[i];
		con->box = con->floating_box;
		arrange_container(con, true);
	}
}

void arrange_root(void) {
	titlebars_clear();
	// Anything not reached below (hidden scratchpad, unmapped) is hidden.
	for (int i = 0; i < server.views->length; ++i) {
		struct swine_view *view = server.views->items[i];
		if (!view->container) {
			continue;
		}
		view->pending.visible = false;
	}
	for (int i = 0; i < root->outputs->length; ++i) {
		struct swine_output *output = root->outputs->items[i];
		for (int j = 0; j < output->workspaces->length; ++j) {
			arrange_workspace(output->workspaces->items[j]);
		}
	}
	struct swine_output *fallback = root->fallback_output;
	for (int j = 0; j < fallback->workspaces->length; ++j) {
		arrange_workspace(fallback->workspaces->items[j]);
	}
}
