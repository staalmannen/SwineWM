#ifndef _SWINE_CRITERIA_H
#define _SWINE_CRITERIA_H

#include <stdbool.h>
#include <stddef.h>
#include "list.h"
#include "regex.h"
#include "swinewm/tree/view.h"

// Criteria ("[app_id=... title=...]") for for_window, assign, no_focus and
// commands, ported from sway/criteria.c. Windows has no Wayland app_id or
// X11 class; see the token table in doc/compatibility.md for what each
// token matches here.

enum criteria_type {
	CT_COMMAND                 = 1 << 0,
	CT_ASSIGN_OUTPUT           = 1 << 1,
	CT_ASSIGN_WORKSPACE        = 1 << 2,
	CT_ASSIGN_WORKSPACE_NUMBER = 1 << 3,
	CT_NO_FOCUS                = 1 << 4,
};

enum pattern_type {
	PATTERN_REGEX,
	PATTERN_FOCUSED,   // "__focused__"
};

struct pattern {
	enum pattern_type match_type;
	struct swine_regex *regex;
};

enum criteria_window_type {
	WINDOW_TYPE_ANY,
	WINDOW_TYPE_NORMAL,
	WINDOW_TYPE_DIALOG,
	WINDOW_TYPE_OTHER,   // utility, toolbar, ...: never matches here
};

struct criteria {
	enum criteria_type type;
	char *raw;      // entire criteria string (for logging)
	char *cmdlist;
	char *target;   // workspace or output name for `assign` criteria

	struct pattern *title;
	struct pattern *shell;
	struct pattern *app_id;
	struct pattern *con_mark;
	size_t con_id;
	struct pattern *class;
	unsigned long id;   // the window handle
	struct pattern *instance;
	struct pattern *window_role;
	enum criteria_window_type window_type;
	bool all;
	bool floating;
	bool tiling;
	char urgent;    // 'l' for latest or 'o' for oldest
	struct pattern *workspace;
	unsigned long pid;
	struct pattern *sandbox_engine;
	struct pattern *sandbox_app_id;
	struct pattern *sandbox_instance_id;
	struct pattern *tag;
};

bool criteria_is_empty(struct criteria *criteria);
bool criteria_is_equal(struct criteria *left, struct criteria *right);
bool criteria_already_exists(struct criteria *criteria);
void criteria_destroy(struct criteria *criteria);

// Parses "[class="foo" instance="bar"]" (brackets included). On error,
// returns NULL and sets *error (caller frees).
struct criteria *criteria_parse(const char *raw, char **error);

// Criteria from the config of the given types that match the view.
list_t *criteria_for_view(struct swine_view *view, enum criteria_type types);
// Containers matching the criteria.
list_t *criteria_get_containers(struct criteria *criteria);

#endif
