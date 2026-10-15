#ifndef _SWINE_WORKSPACE_H
#define _SWINE_WORKSPACE_H

#include <stdbool.h>
#include "list.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/node.h"

struct swine_output;

struct side_gaps {
	int top, right, bottom, left;
};

struct swine_workspace {
	struct swine_node node;
	char *name;
	struct swine_output *output;
	struct swine_container *fullscreen;
	list_t *tiling;           // struct swine_container *
	list_t *floating;         // struct swine_container *
	enum swine_container_layout layout;
	enum swine_container_layout prev_split_layout;
	int gaps_inner;
	struct side_gaps gaps_outer;
	struct swine_box box;     // tiling area after outer gaps (set by arrange)
	bool urgent;
};

struct swine_workspace *workspace_create(struct swine_output *output,
		const char *name);
void workspace_destroy(struct swine_workspace *ws);
// Destroys ws if it is empty, not visible, and not focused.
void workspace_consider_destroy(struct swine_workspace *ws);

struct swine_workspace *workspace_by_name(const char *name);
struct swine_workspace *workspace_by_number(const char *name);
// Lowest unused workspace number, as a string. Caller frees.
char *workspace_next_name(void);
int workspace_number(const char *name); // -1 if the name is not numbered

struct swine_workspace *workspace_output_next(struct swine_workspace *current);
struct swine_workspace *workspace_output_prev(struct swine_workspace *current);
struct swine_workspace *workspace_next(struct swine_workspace *current);
struct swine_workspace *workspace_prev(struct swine_workspace *current);

// Makes ws the active workspace on its output and focuses it.
bool workspace_switch(struct swine_workspace *ws);
bool workspace_is_visible(struct swine_workspace *ws);
bool workspace_is_empty(struct swine_workspace *ws);

struct swine_container *workspace_add_tiling(struct swine_workspace *ws,
		struct swine_container *con);
struct swine_container *workspace_insert_tiling(struct swine_workspace *ws,
		struct swine_container *con, int index);
void workspace_insert_tiling_direct(struct swine_workspace *ws,
		struct swine_container *con, int index);
void workspace_add_floating(struct swine_workspace *ws,
		struct swine_container *con);
struct swine_container *workspace_wrap_children(struct swine_workspace *ws);
void workspace_unwrap_children(struct swine_workspace *ws,
		struct swine_container *wrap);
struct swine_container *workspace_split(struct swine_workspace *ws,
		enum swine_container_layout layout);
void workspace_squash(struct swine_workspace *ws);
void workspace_detach(struct swine_workspace *ws);
void workspace_for_each_container(struct swine_workspace *ws,
		void (*f)(struct swine_container *con, void *data), void *data);
// Layout for a new workspace or container on this output (config + aspect).
enum swine_container_layout output_get_default_layout(struct swine_output *output);

#endif
