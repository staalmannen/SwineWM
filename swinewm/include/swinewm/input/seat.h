#ifndef _SWINE_SEAT_H
#define _SWINE_SEAT_H

#include "list.h"
#include "swinewm/tree/node.h"

struct swine_container;
struct swine_workspace;

// Single seat. Like sway, focus history is a stack of nodes (index 0 is the
// most recently focused). "Focus inactive" queries walk this stack to find
// the last focused descendant of a node, which drives directional focus,
// tabbed/stacked visibility and where new windows are inserted.
struct swine_seat {
	list_t *focus_stack;            // struct swine_node *
	char *prev_workspace_name;      // for workspace back_and_forth
};

extern struct swine_seat *seat;

struct swine_seat *seat_create(void);
void seat_destroy(struct swine_seat *seat);

// Adds a freshly created node to the bottom of the focus stack.
void seat_node_created(struct swine_node *node);
// Removes a node that is about to be freed; moves focus if needed.
void seat_node_destroyed(struct swine_node *node);

// Focuses node and raises its ancestors in the focus stack. Switches the
// visible workspace when the node lives on a hidden one.
void seat_set_focus(struct swine_node *node);
void seat_set_focus_container(struct swine_container *con);
void seat_set_focus_workspace(struct swine_workspace *ws);
// Updates the stack without any workspace side effects.
void seat_set_raw_focus(struct swine_node *node);

struct swine_node *seat_get_focus(void);
struct swine_container *seat_get_focused_container(void);
struct swine_workspace *seat_get_focused_workspace(void);
struct swine_node *seat_get_focus_inactive(struct swine_node *ancestor);
struct swine_container *seat_get_focus_inactive_view(struct swine_node *ancestor);
struct swine_container *seat_get_focus_inactive_tiling(struct swine_workspace *ws);
struct swine_container *seat_get_focus_inactive_floating(struct swine_workspace *ws);
// The direct child of parent that was focused most recently.
struct swine_node *seat_get_active_tiling_child(struct swine_node *parent);

#endif
