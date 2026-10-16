// Criteria, ported from sway/criteria.c (Copyright (c) 2016-2017 Drew
// DeVault, MIT). Patterns use swinewm's regex engine instead of PCRE2.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/config.h"
#include "swinewm/criteria.h"
#include "swinewm/input/seat.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/view.h"
#include "swinewm/tree/workspace.h"

// What a view reports for the "shell" token (sway: xdg_shell, xwayland).
#define VIEW_SHELL "native"

bool criteria_is_empty(struct criteria *criteria) {
	return !criteria->title
		&& !criteria->shell
		&& !criteria->all
		&& !criteria->app_id
		&& !criteria->con_mark
		&& !criteria->con_id
		&& !criteria->class
		&& !criteria->id
		&& !criteria->instance
		&& !criteria->window_role
		&& criteria->window_type == WINDOW_TYPE_ANY
		&& !criteria->floating
		&& !criteria->tiling
		&& !criteria->urgent
		&& !criteria->workspace
		&& !criteria->pid
		&& !criteria->sandbox_engine
		&& !criteria->sandbox_app_id
		&& !criteria->sandbox_instance_id
		&& !criteria->tag;
}

// Set by the parsing functions, as in sway.
static char *error = NULL;

static bool pattern_create(struct pattern **pattern, const char *value,
		bool icase) {
	*pattern = calloc(1, sizeof(struct pattern));
	if (strcmp(value, "__focused__") == 0) {
		(*pattern)->match_type = PATTERN_FOCUSED;
		return true;
	}
	(*pattern)->match_type = PATTERN_REGEX;
	char *regex_error = NULL;
	(*pattern)->regex = swine_regex_compile(value, icase, &regex_error);
	if (!(*pattern)->regex) {
		error = format_str("Regex compilation for '%s' failed: %s", value,
			regex_error);
		free(regex_error);
		return false;
	}
	return true;
}

static void pattern_destroy(struct pattern *pattern) {
	if (pattern) {
		swine_regex_free(pattern->regex);
		free(pattern);
	}
}

void criteria_destroy(struct criteria *criteria) {
	pattern_destroy(criteria->title);
	pattern_destroy(criteria->shell);
	pattern_destroy(criteria->app_id);
	pattern_destroy(criteria->class);
	pattern_destroy(criteria->instance);
	pattern_destroy(criteria->window_role);
	pattern_destroy(criteria->con_mark);
	pattern_destroy(criteria->workspace);
	pattern_destroy(criteria->sandbox_engine);
	pattern_destroy(criteria->sandbox_app_id);
	pattern_destroy(criteria->sandbox_instance_id);
	pattern_destroy(criteria->tag);
	free(criteria->target);
	free(criteria->cmdlist);
	free(criteria->raw);
	free(criteria);
}

static bool regex_matches(const char *item, const struct swine_regex *regex) {
	return swine_regex_search(regex, item ? item : "");
}

static int lenient_strcmp(const char *a, const char *b) {
	if (a == b) {
		return 0;
	} else if (!a) {
		return -1;
	} else if (!b) {
		return 1;
	}
	return strcmp(a, b);
}

// One string token: a regex, or "__focused__" (same value as the focused
// view). value_of(NULL) for a view without that property.
static bool string_token_matches(const struct pattern *pattern,
		const char *value, const char *focused_value, bool have_focused) {
	switch (pattern->match_type) {
	case PATTERN_FOCUSED:
		return have_focused && lenient_strcmp(value ? value : "",
			focused_value ? focused_value : "") == 0;
	case PATTERN_REGEX:
		return regex_matches(value, pattern->regex);
	}
	return false;
}

static int cmp_urgent(const void *_a, const void *_b) {
	const struct swine_view *a = *(void *const *)_a;
	const struct swine_view *b = *(void *const *)_b;
	return a->urgent_since < b->urgent_since ? -1 :
		a->urgent_since > b->urgent_since ? 1 : 0;
}

static void find_urgent_iterator(struct swine_container *con, void *data) {
	if (!con->view || !con->view->urgent) {
		return;
	}
	list_add(data, con->view);
}

static bool has_container_criteria(struct criteria *criteria) {
	return criteria->con_mark || criteria->con_id;
}

static bool criteria_matches_container(struct criteria *criteria,
		struct swine_container *container) {
	if (criteria->con_mark) {
		bool exists = false;
		for (int i = 0; container->marks && i < container->marks->length; ++i) {
			if (regex_matches(container->marks->items[i],
					criteria->con_mark->regex)) {
				exists = true;
				break;
			}
		}
		if (!exists) {
			return false;
		}
	}
	if (criteria->con_id && container->node.id != criteria->con_id) {
		return false;
	}
	return true;
}

// The executable name without ".exe": Windows' closest thing to an X11
// instance name.
static char *view_instance(struct swine_view *view) {
	if (!view->app_id) {
		return NULL;
	}
	char *instance = swine_strdup(view->app_id);
	size_t len = strlen(instance);
	if (len > 4 && swine_strcasecmp(instance + len - 4, ".exe") == 0) {
		instance[len - 4] = '\0';
	}
	return instance;
}

static bool criteria_matches_view(struct criteria *criteria,
		struct swine_view *view) {
	struct swine_container *focus = seat_get_focused_container();
	struct swine_view *focused = focus ? focus->view : NULL;

	if (!view->container) {
		return false;
	}
	if (criteria->title && !string_token_matches(criteria->title,
			view->title, focused ? focused->title : NULL, focused)) {
		return false;
	}
	if (criteria->shell && !string_token_matches(criteria->shell,
			VIEW_SHELL, VIEW_SHELL, focused)) {
		return false;
	}
	if (criteria->app_id && !string_token_matches(criteria->app_id,
			view->app_id, focused ? focused->app_id : NULL, focused)) {
		return false;
	}
	// Windows has no sandboxes or tags: these never match, as in sway for
	// a view without them.
	if (criteria->sandbox_engine || criteria->sandbox_app_id ||
			criteria->sandbox_instance_id || criteria->tag) {
		return false;
	}
	if (!criteria_matches_container(criteria, view->container)) {
		return false;
	}
	if (criteria->id && criteria->id != (unsigned long)(uintptr_t)view->handle) {
		return false;
	}
	if (criteria->class && !string_token_matches(criteria->class,
			view->class_name, focused ? focused->class_name : NULL, focused)) {
		return false;
	}
	if (criteria->instance) {
		char *instance = view_instance(view);
		char *focused_instance = focused ? view_instance(focused) : NULL;
		bool ok = string_token_matches(criteria->instance, instance,
			focused_instance, focused);
		free(instance);
		free(focused_instance);
		if (!ok) {
			return false;
		}
	}
	if (criteria->window_role && !string_token_matches(criteria->window_role,
			"", "", focused)) {
		return false;
	}
	switch (criteria->window_type) {
	case WINDOW_TYPE_ANY:
		break;
	case WINDOW_TYPE_NORMAL:
		if (view->natural_floating) {
			return false;
		}
		break;
	case WINDOW_TYPE_DIALOG:
		if (!view->natural_floating) {
			return false;
		}
		break;
	case WINDOW_TYPE_OTHER:
		return false;
	}
	if (criteria->floating && !container_is_floating(view->container)) {
		return false;
	}
	if (criteria->tiling && container_is_floating(view->container)) {
		return false;
	}
	if (criteria->urgent) {
		if (!view->urgent) {
			return false;
		}
		list_t *urgent_views = create_list();
		root_for_each_container(find_urgent_iterator, urgent_views);
		list_stable_sort(urgent_views, cmp_urgent);
		struct swine_view *target = criteria->urgent == 'o' ?
			urgent_views->items[0] :
			urgent_views->items[urgent_views->length - 1];
		list_free(urgent_views);
		if (view != target) {
			return false;
		}
	}
	if (criteria->workspace) {
		struct swine_workspace *ws = view->container->workspace;
		if (!ws) {
			return false;
		}
		const char *focused_ws = focused && focused->container->workspace ?
			focused->container->workspace->name : NULL;
		if (!string_token_matches(criteria->workspace, ws->name, focused_ws,
				focused_ws != NULL)) {
			return false;
		}
	}
	if (criteria->pid && criteria->pid != view->pid) {
		return false;
	}
	return true;
}

list_t *criteria_for_view(struct swine_view *view, enum criteria_type types) {
	list_t *matches = create_list();
	for (int i = 0; i < config->criteria->length; ++i) {
		struct criteria *criteria = config->criteria->items[i];
		if ((criteria->type & types) && criteria_matches_view(criteria, view)) {
			list_add(matches, criteria);
		}
	}
	return matches;
}

struct match_data {
	struct criteria *criteria;
	list_t *matches;
};

static void criteria_get_containers_iterator(struct swine_container *container,
		void *data) {
	struct match_data *match_data = data;
	if (container->view) {
		if (criteria_matches_view(match_data->criteria, container->view)) {
			list_add(match_data->matches, container);
		}
	} else if (has_container_criteria(match_data->criteria)) {
		if (criteria_matches_container(match_data->criteria, container)) {
			list_add(match_data->matches, container);
		}
	}
}

list_t *criteria_get_containers(struct criteria *criteria) {
	list_t *matches = create_list();
	struct match_data data = {criteria, matches};
	root_for_each_container(criteria_get_containers_iterator, &data);
	return matches;
}

static enum criteria_window_type parse_window_type(const char *type) {
	if (swine_strcasecmp(type, "normal") == 0) {
		return WINDOW_TYPE_NORMAL;
	} else if (swine_strcasecmp(type, "dialog") == 0) {
		return WINDOW_TYPE_DIALOG;
	}
	static const char *others[] = {"utility", "toolbar", "splash", "menu",
		"dropdown_menu", "popup_menu", "tooltip", "notification"};
	for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); ++i) {
		if (swine_strcasecmp(type, others[i]) == 0) {
			return WINDOW_TYPE_OTHER;
		}
	}
	return WINDOW_TYPE_ANY;   // as in sway: an unknown type does not filter
}

static bool parse_number(const char *value, unsigned long *out,
		const char *message) {
	char *endptr = NULL;
	*out = strtoul(value, &endptr, 10);
	if (*endptr != '\0') {
		error = swine_strdup(message);
		return false;
	}
	return true;
}

static bool parse_token(struct criteria *criteria, const char *name,
		const char *value) {
	static const char *const no_value[] = {"all", "floating", "tiling"};
	bool needs_value = true;
	for (size_t i = 0; i < 3; ++i) {
		if (strcmp(name, no_value[i]) == 0) {
			needs_value = false;
		}
	}
	static const char *const tokens[] = {"all", "app_id", "con_id",
		"con_mark", "class", "id", "instance", "window_role", "window_type",
		"shell", "title", "urgent", "workspace", "tiling", "floating", "pid",
		"sandbox_engine", "sandbox_app_id", "sandbox_instance_id", "tag"};
	bool known = false;
	for (size_t i = 0; i < sizeof(tokens) / sizeof(tokens[0]); ++i) {
		if (strcmp(name, tokens[i]) == 0) {
			known = true;
		}
	}
	if (!known) {
		error = format_str("Token '%s' is not recognized", name);
		return false;
	}
	if (!value && needs_value) {
		error = format_str("Token '%s' requires a value", name);
		return false;
	}

	if (strcmp(name, "all") == 0) {
		criteria->all = true;
	} else if (strcmp(name, "title") == 0) {
		return pattern_create(&criteria->title, value, false);
	} else if (strcmp(name, "shell") == 0) {
		return pattern_create(&criteria->shell, value, false);
	} else if (strcmp(name, "app_id") == 0) {
		// Executable names: Windows file names ignore case.
		return pattern_create(&criteria->app_id, value, true);
	} else if (strcmp(name, "con_id") == 0) {
		if (strcmp(value, "__focused__") == 0) {
			struct swine_container *focus = seat_get_focused_container();
			criteria->con_id = focus ? focus->node.id : 0;
		} else {
			unsigned long id;
			if (!parse_number(value, &id,
					"The value for 'con_id' should be '__focused__' or numeric")) {
				return false;
			}
			criteria->con_id = id;
		}
	} else if (strcmp(name, "con_mark") == 0) {
		return pattern_create(&criteria->con_mark, value, false);
	} else if (strcmp(name, "class") == 0) {
		return pattern_create(&criteria->class, value, false);
	} else if (strcmp(name, "id") == 0) {
		return parse_number(value, &criteria->id,
			"The value for 'id' should be numeric");
	} else if (strcmp(name, "instance") == 0) {
		return pattern_create(&criteria->instance, value, true);
	} else if (strcmp(name, "window_role") == 0) {
		return pattern_create(&criteria->window_role, value, false);
	} else if (strcmp(name, "window_type") == 0) {
		criteria->window_type = parse_window_type(value);
	} else if (strcmp(name, "floating") == 0) {
		criteria->floating = true;
	} else if (strcmp(name, "tiling") == 0) {
		criteria->tiling = true;
	} else if (strcmp(name, "urgent") == 0) {
		if (strcmp(value, "latest") == 0 || strcmp(value, "newest") == 0 ||
				strcmp(value, "last") == 0 || strcmp(value, "recent") == 0) {
			criteria->urgent = 'l';
		} else if (strcmp(value, "oldest") == 0 || strcmp(value, "first") == 0) {
			criteria->urgent = 'o';
		} else {
			error = swine_strdup("The value for 'urgent' must be 'first', "
				"'last', 'latest', 'newest', 'oldest' or 'recent'");
			return false;
		}
	} else if (strcmp(name, "workspace") == 0) {
		return pattern_create(&criteria->workspace, value, false);
	} else if (strcmp(name, "pid") == 0) {
		return parse_number(value, &criteria->pid,
			"The value for 'pid' should be numeric");
	} else if (strcmp(name, "sandbox_engine") == 0) {
		return pattern_create(&criteria->sandbox_engine, value, false);
	} else if (strcmp(name, "sandbox_app_id") == 0) {
		return pattern_create(&criteria->sandbox_app_id, value, false);
	} else if (strcmp(name, "sandbox_instance_id") == 0) {
		return pattern_create(&criteria->sandbox_instance_id, value, false);
	} else if (strcmp(name, "tag") == 0) {
		return pattern_create(&criteria->tag, value, false);
	}
	return true;
}

static void skip_spaces(const char **head) {
	while (**head == ' ') {
		++*head;
	}
}

// Remove escaping backslashes before quotes from value.
static void unescape(char *value) {
	char *write = value;
	for (const char *read = value; *read; ++read) {
		if (read[0] == '\\' && read[1] == '"') {
			continue;
		}
		*write++ = *read;
	}
	*write = '\0';
}

struct criteria *criteria_parse(const char *raw, char **error_arg) {
	*error_arg = NULL;
	error = NULL;
	const char *head = raw;
	skip_spaces(&head);
	if (*head != '[') {
		*error_arg = swine_strdup("No criteria");
		return NULL;
	}
	++head;

	struct criteria *criteria = calloc(1, sizeof(struct criteria));
	char *name = NULL, *value = NULL;
	while (*head && *head != ']') {
		skip_spaces(&head);
		const char *namestart = head;
		while ((*head >= 'a' && *head <= 'z') || *head == '_') {
			++head;
		}
		name = swine_strndup(namestart, (size_t)(head - namestart));
		skip_spaces(&head);
		value = NULL;
		if (*head == '=') {
			++head;
			skip_spaces(&head);
			bool in_quotes = *head == '"';
			if (in_quotes) {
				++head;
			}
			const char *valuestart = head;
			if (in_quotes) {
				while (*head && (*head != '"' || *(head - 1) == '\\')) {
					++head;
				}
				if (!*head) {
					*error_arg = swine_strdup("Quote mismatch in criteria");
					goto cleanup;
				}
			} else {
				while (*head && *head != ' ' && *head != ']') {
					++head;
				}
			}
			value = swine_strndup(valuestart, (size_t)(head - valuestart));
			if (in_quotes) {
				++head;
			}
			unescape(value);
			swine_log(SWINE_DEBUG, "Found pair: %s=%s", name, value);
		}
		if (!parse_token(criteria, name, value)) {
			*error_arg = error;
			error = NULL;
			goto cleanup;
		}
		skip_spaces(&head);
		free(name);
		free(value);
		name = NULL;
		value = NULL;
	}
	if (*head != ']') {
		*error_arg = swine_strdup("No closing brace found in criteria");
		goto cleanup;
	}
	if (criteria_is_empty(criteria)) {
		*error_arg = swine_strdup("Criteria is empty");
		goto cleanup;
	}
	++head;
	criteria->raw = swine_strndup(raw, (size_t)(head - raw));
	return criteria;

cleanup:
	free(name);
	free(value);
	criteria_destroy(criteria);
	return NULL;
}

bool criteria_is_equal(struct criteria *left, struct criteria *right) {
	if (left->type != right->type) {
		return false;
	}
	// Only implemented for CT_NO_FOCUS and CT_COMMAND, as in sway.
	if (left->type == CT_NO_FOCUS) {
		return strcmp(left->raw, right->raw) == 0;
	}
	if (left->type == CT_COMMAND) {
		return strcmp(left->raw, right->raw) == 0 &&
			strcmp(left->cmdlist, right->cmdlist) == 0;
	}
	return false;
}

bool criteria_already_exists(struct criteria *criteria) {
	if (criteria->type != CT_NO_FOCUS && criteria->type != CT_COMMAND) {
		return false;
	}
	for (int i = 0; i < config->criteria->length; ++i) {
		if (criteria_is_equal(criteria, config->criteria->items[i])) {
			return true;
		}
	}
	return false;
}
