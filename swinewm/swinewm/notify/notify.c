// The integrated Mako clone: notification store, history, modes and the
// Mako config format. Defaults follow mako(5). Matching, grouping, sorting
// and bindings are ported from mako's criteria.c, notification.c and
// config.c (Copyright (c) 2018 emersion, MIT).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ipc.h"
#include "json.h"
#include "log.h"
#include "regex.h"
#include "stringop.h"
#include "swinewm/bar/format.h"
#include "swinewm/config.h"
#include "swinewm/input/seat.h"
#include "swinewm/notify.h"
#include "swinewm/platform.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/workspace.h"

// Criteria sections, matched as mako's match_criteria does. NULL and -1
// fields are not part of the criteria.
struct criteria {
	char *app_name;
	char *app_icon;
	char *summary;
	struct swine_regex *summary_re;   // summary~=
	char *body;
	struct swine_regex *body_re;      // body~=
	char *category;
	char *desktop_entry;
	char *mode;
	char *output;
	int urgency;
	int expiring;      // -1, 0, 1
	int actionable;
	int grouped;
	int group_index;
	int hidden;
	int anchor;
	bool none;         // matches nothing (group-by=none)
	bool invalid;      // rejected by validate_criteria: never matches
	char *raw;         // for messages
	list_t *options;   // struct option *
};

struct option {
	char *key;
	char *value;
};

enum sort_criteria {
	SORT_TIME = 1 << 0,
	SORT_URGENCY = 1 << 1,
};

static struct {
	list_t *criteria;      // struct criteria *; [0] is the global section
	list_t *active;        // in display order (mako's sort)
	list_t *history;       // newest first
	list_t *modes;
	uint32_t next_id;
	int max_history;
	uint32_t sort_criteria;
	uint32_t sort_asc;
	enum notify_windows windows;
	bool loaded;
} st = {0};

static void free_actions(list_t *actions) {
	if (!actions) {
		return;
	}
	for (int i = 0; i < actions->length; ++i) {
		struct notify_action_item *a = actions->items[i];
		free(a->key);
		free(a->title);
		free(a);
	}
	list_free(actions);
}

static void free_fields(struct swine_notification *n) {
	free(n->app_name);
	free(n->summary);
	free(n->body);
	free(n->category);
	free(n->app_icon);
	free(n->desktop_entry);
	free(n->tag);
	free_actions(n->actions);
	n->actions = NULL;
}

static void free_notification(struct swine_notification *n) {
	free_fields(n);
	free(n);
}

static void free_criteria(struct criteria *c) {
	free(c->app_name);
	free(c->app_icon);
	free(c->summary);
	free(c->body);
	free(c->category);
	free(c->desktop_entry);
	free(c->mode);
	free(c->output);
	free(c->raw);
	if (c->summary_re) {
		swine_regex_free(c->summary_re);
	}
	if (c->body_re) {
		swine_regex_free(c->body_re);
	}
	for (int i = 0; i < c->options->length; ++i) {
		struct option *o = c->options->items[i];
		free(o->key);
		free(o->value);
		free(o);
	}
	list_free(c->options);
	free(c);
}

static struct criteria *criteria_create(const char *raw) {
	struct criteria *c = calloc(1, sizeof(*c));
	c->urgency = -1;
	c->expiring = -1;
	c->actionable = -1;
	c->grouped = -1;
	c->group_index = -1;
	c->hidden = -1;
	c->anchor = -1;
	c->raw = swine_strdup(raw);
	c->options = create_list();
	return c;
}

static void add_option(struct criteria *c, const char *key, const char *value) {
	struct option *o = calloc(1, sizeof(*o));
	o->key = swine_strdup(key);
	o->value = swine_strdup(value);
	list_add(c->options, o);
}

static void free_config_state(void) {
	if (st.criteria) {
		for (int i = 0; i < st.criteria->length; ++i) {
			free_criteria(st.criteria->items[i]);
		}
		list_free(st.criteria);
		st.criteria = NULL;
	}
}

// mako's init_default_config: the global section, then the sections that
// hide all but the first notification of a group and style the "(%h more)"
// placeholder. User sections follow.
static void default_criteria(void) {
	st.criteria = create_list();
	list_add(st.criteria, criteria_create("(root)"));
	struct criteria *c = criteria_create("(default grouped)");
	c->grouped = 1;
	add_option(c, "invisible", "1");
	add_option(c, "format", "(%g) <b>%s</b>\\n%b");
	list_add(st.criteria, c);
	c = criteria_create("(default group-index=0)");
	c->group_index = 0;
	add_option(c, "invisible", "0");
	list_add(st.criteria, c);
	c = criteria_create("(default hidden)");
	c->hidden = 1;
	add_option(c, "format", "(%h more)");
	list_add(st.criteria, c);
	st.max_history = 5;
	st.sort_criteria = SORT_TIME;
	st.sort_asc = 0;
	st.windows = NOTIFY_WINDOWS_AUTO;
}

static void ensure_init(void) {
	if (!st.active) {
		st.active = create_list();
		st.history = create_list();
		st.modes = create_list();
		list_add(st.modes, swine_strdup("default"));
		st.next_id = 1;
	}
	if (!st.criteria) {
		default_criteria();
	}
}

void notify_finish(void) {
	free_config_state();
	if (st.active) {
		for (int i = 0; i < st.active->length; ++i) {
			free_notification(st.active->items[i]);
		}
		for (int i = 0; i < st.history->length; ++i) {
			free_notification(st.history->items[i]);
		}
		list_free(st.active);
		list_free(st.history);
		list_free_items_and_destroy(st.modes);
	}
	memset(&st, 0, sizeof(st));
}

// --- style ---------------------------------------------------------------

static void default_style(struct notify_style *s) {
	memset(s, 0, sizeof(*s));
	strcpy(s->font_family, "monospace");
	s->font_size = 10;
	s->background = (struct swine_color){0x28, 0x55, 0x77, 0xFF};
	s->text = (struct swine_color){0xFF, 0xFF, 0xFF, 0xFF};
	s->border = (struct swine_color){0x4C, 0x78, 0x99, 0xFF};
	s->width = 300;
	s->height = 100;
	s->margin = (struct css_box){10, 10, 10, 10};
	s->padding = (struct css_box){5, 5, 5, 5};
	s->border_size = 2;
	s->progress = (struct swine_color){0x55, 0x88, 0xAA, 0xFF};
	s->icons = true;
	s->max_icon_size = 64;
	s->icon_location = NOTIFY_ICON_LEFT;
	s->actions = true;
	s->group_by = NOTIFY_FIELD_NONE;
	s->max_visible = 5;
	strcpy(s->format, "<b>%s</b>\\n%b");
	s->markup = true;
	s->history = true;
	s->anchor = NOTIFY_ANCHOR_TOP_RIGHT;
	s->on_button_left.action = NOTIFY_ACTION_INVOKE;
	strcpy(s->on_button_left.command, "default");
	s->on_button_right.action = NOTIFY_ACTION_DISMISS;
	s->on_touch.action = NOTIFY_ACTION_DISMISS;
}

// "5", "5,10", "5,10,5" or "5,10,5,10" (top,right,bottom,left like CSS)
static void parse_directional(const char *v, struct css_box *box) {
	int n[4] = {0};
	int count = sscanf(v, "%d,%d,%d,%d", &n[0], &n[1], &n[2], &n[3]);
	switch (count) {
	case 1: *box = (struct css_box){n[0], n[0], n[0], n[0]}; break;
	case 2: *box = (struct css_box){n[0], n[1], n[0], n[1]}; break;
	case 3: *box = (struct css_box){n[0], n[1], n[2], n[1]}; break;
	case 4: *box = (struct css_box){n[0], n[1], n[2], n[3]}; break;
	default: break;
	}
}

// Pango font description: "Family Name [Bold] [Italic] [size]"
static void parse_font(const char *v, struct notify_style *s) {
	list_t *words = split_string(v, " ");
	int end = words->length;
	double size = 0;
	if (end > 0) {
		char *last = words->items[end - 1];
		char *stop;
		double d = strtod(last, &stop);
		if (stop != last && (*stop == '\0' || strcmp(stop, "px") == 0)) {
			size = *stop ? d * 0.75 : d;
			end--;
		}
	}
	s->font_bold = false;
	while (end > 0) {
		char *w = words->items[end - 1];
		if (swine_strcasecmp(w, "bold") == 0) {
			s->font_bold = true;
		} else if (swine_strcasecmp(w, "italic") != 0 &&
				swine_strcasecmp(w, "regular") != 0 &&
				swine_strcasecmp(w, "normal") != 0) {
			break;
		}
		end--;
	}
	char *family = join_args((char **)words->items, end);
	if (family && *family) {
		snprintf(s->font_family, sizeof(s->font_family), "%s", family);
	}
	free(family);
	if (size > 0) {
		s->font_size = size;
	}
	list_free_items_and_destroy(words);
}

static bool parse_button(const char *v, struct notify_button *b) {
	struct notify_button out = {0};
	if (strcmp(v, "none") == 0) {
		out.action = NOTIFY_ACTION_NONE;
	} else if (strcmp(v, "dismiss") == 0) {
		out.action = NOTIFY_ACTION_DISMISS;
	} else if (strcmp(v, "dismiss --no-history") == 0) {
		out.action = NOTIFY_ACTION_DISMISS_NO_HISTORY;
	} else if (strcmp(v, "dismiss-all") == 0) {
		out.action = NOTIFY_ACTION_DISMISS_ALL;
	} else if (strcmp(v, "dismiss-group") == 0) {
		out.action = NOTIFY_ACTION_DISMISS_GROUP;
	} else if (strcmp(v, "invoke-default-action") == 0) {
		out.action = NOTIFY_ACTION_INVOKE;
		strcpy(out.command, "default");
	} else if (has_prefix(v, "invoke-action ")) {
		out.action = NOTIFY_ACTION_INVOKE;
		snprintf(out.command, sizeof(out.command), "%s",
			v + strlen("invoke-action "));
	} else if (has_prefix(v, "exec ")) {
		out.action = NOTIFY_ACTION_EXEC;
		snprintf(out.command, sizeof(out.command), "%s", v + strlen("exec "));
	} else {
		return false;
	}
	*b = out;
	return true;
}

// mako's parse_criteria_spec: "app-name,summary" for group-by.
static bool parse_fields(const char *v, uint32_t *out) {
	static const struct {
		const char *name;
		uint32_t field;
	} names[] = {
		{"app-name", NOTIFY_FIELD_APP_NAME},
		{"app-icon", NOTIFY_FIELD_APP_ICON},
		{"actionable", NOTIFY_FIELD_ACTIONABLE},
		{"expiring", NOTIFY_FIELD_EXPIRING},
		{"urgency", NOTIFY_FIELD_URGENCY},
		{"category", NOTIFY_FIELD_CATEGORY},
		{"desktop-entry", NOTIFY_FIELD_DESKTOP_ENTRY},
		{"summary", NOTIFY_FIELD_SUMMARY},
		{"body", NOTIFY_FIELD_BODY},
		{"none", NOTIFY_FIELD_NONE},
	};
	uint32_t fields = 0;
	bool ok = true;
	list_t *parts = split_string(v, ",");
	for (int i = 0; i < parts->length && ok; ++i) {
		char *part = parts->items[i];
		strip_whitespace(part);
		bool found = false;
		for (size_t j = 0; j < sizeof(names) / sizeof(names[0]); ++j) {
			if (strcmp(part, names[j].name) == 0) {
				fields |= names[j].field;
				found = true;
			}
		}
		if (!found) {
			// grouped, group-index, anchor and output cannot be used
			swine_log(SWINE_ERROR, "group-by: unknown or unusable field '%s'",
				part);
			ok = false;
		}
	}
	list_free_items_and_destroy(parts);
	if (ok) {
		*out = fields;
	}
	return ok;
}

static const char *anchor_names[] = {
	"top-right", "top-center", "top-left", "bottom-right",
	"bottom-center", "bottom-left", "center-right", "center-left",
	"center",
};

static int parse_anchor(const char *v) {
	for (int i = 0; i < 9; ++i) {
		if (strcmp(v, anchor_names[i]) == 0) {
			return i;
		}
	}
	return -1;
}

static bool parse_bool(const char *v) {
	return strcmp(v, "1") == 0 || strcmp(v, "true") == 0;
}

static void apply_option(struct notify_style *s, const char *key,
		const char *v) {
	struct swine_color c;
	if (strcmp(key, "font") == 0) {
		parse_font(v, s);
	} else if (strcmp(key, "background-color") == 0) {
		if (css_parse_color(NULL, v, &c)) s->background = c;
	} else if (strcmp(key, "text-color") == 0) {
		if (css_parse_color(NULL, v, &c)) s->text = c;
	} else if (strcmp(key, "border-color") == 0) {
		if (css_parse_color(NULL, v, &c)) s->border = c;
	} else if (strcmp(key, "progress-color") == 0) {
		// "[over|source] <color>"
		const char *color = v;
		s->progress_source = false;
		if (has_prefix(v, "over ")) {
			color = v + strlen("over ");
		} else if (has_prefix(v, "source ")) {
			color = v + strlen("source ");
			s->progress_source = true;
		}
		if (css_parse_color(NULL, color, &c)) s->progress = c;
	} else if (strcmp(key, "width") == 0) {
		s->width = atoi(v);
	} else if (strcmp(key, "height") == 0) {
		s->height = atoi(v);
	} else if (strcmp(key, "outer-margin") == 0) {
		parse_directional(v, &s->outer_margin);
	} else if (strcmp(key, "margin") == 0) {
		parse_directional(v, &s->margin);
	} else if (strcmp(key, "padding") == 0) {
		parse_directional(v, &s->padding);
		// As in mako: the text keeps clear of rounded corners.
		s->padding.left = s->padding.left > s->border_radius.left ?
			s->padding.left : s->border_radius.left;
		s->padding.right = s->padding.right > s->border_radius.right ?
			s->padding.right : s->border_radius.right;
	} else if (strcmp(key, "border-size") == 0) {
		s->border_size = atoi(v);
	} else if (strcmp(key, "border-radius") == 0) {
		parse_directional(v, &s->border_radius);
		s->padding.left = s->padding.left > s->border_radius.left ?
			s->padding.left : s->border_radius.left;
		s->padding.right = s->padding.right > s->border_radius.right ?
			s->padding.right : s->border_radius.right;
	} else if (strcmp(key, "icons") == 0) {
		s->icons = parse_bool(v);
	} else if (strcmp(key, "max-icon-size") == 0) {
		s->max_icon_size = atoi(v) > 0 ? atoi(v) : s->max_icon_size;
	} else if (strcmp(key, "icon-path") == 0) {
		snprintf(s->icon_path, sizeof(s->icon_path), "%s", v);
	} else if (strcmp(key, "icon-location") == 0) {
		s->icon_location = strcmp(v, "right") == 0 ? NOTIFY_ICON_RIGHT :
			strcmp(v, "top") == 0 ? NOTIFY_ICON_TOP :
			strcmp(v, "bottom") == 0 ? NOTIFY_ICON_BOTTOM : NOTIFY_ICON_LEFT;
	} else if (strcmp(key, "icon-border-radius") == 0) {
		s->icon_border_radius = atoi(v);
	} else if (strcmp(key, "actions") == 0) {
		s->actions = parse_bool(v);
	} else if (strcmp(key, "group-by") == 0) {
		parse_fields(v, &s->group_by);
	} else if (strcmp(key, "max-visible") == 0) {
		s->max_visible = atoi(v);
	} else if (strcmp(key, "format") == 0) {
		snprintf(s->format, sizeof(s->format), "%s", v);
	} else if (strcmp(key, "text-alignment") == 0) {
		s->text_alignment = strcmp(v, "center") == 0 ? 1 :
			strcmp(v, "right") == 0 ? 2 : 0;
	} else if (strcmp(key, "default-timeout") == 0) {
		s->default_timeout = atoi(v);
	} else if (strcmp(key, "ignore-timeout") == 0) {
		s->ignore_timeout = parse_bool(v);
	} else if (strcmp(key, "invisible") == 0) {
		s->invisible = parse_bool(v);
	} else if (strcmp(key, "markup") == 0) {
		s->markup = parse_bool(v);
	} else if (strcmp(key, "history") == 0) {
		s->history = parse_bool(v);
	} else if (strcmp(key, "output") == 0) {
		snprintf(s->output, sizeof(s->output), "%s", v);
	} else if (strcmp(key, "anchor") == 0) {
		int anchor = parse_anchor(v);
		if (anchor >= 0) {
			s->anchor = (enum notify_anchor)anchor;
		}
	} else if (strcmp(key, "on-button-left") == 0) {
		parse_button(v, &s->on_button_left);
	} else if (strcmp(key, "on-button-middle") == 0) {
		parse_button(v, &s->on_button_middle);
	} else if (strcmp(key, "on-button-right") == 0) {
		parse_button(v, &s->on_button_right);
	} else if (strcmp(key, "on-touch") == 0) {
		parse_button(v, &s->on_touch);
	} else if (strcmp(key, "on-notify") == 0) {
		parse_button(v, &s->on_notify);
	}
	// layer: popups are always topmost
}

// Whether a style option is one mako knows (to report typos).
static bool known_option(const char *key) {
	static const char *keys[] = {
		"font", "background-color", "text-color", "border-color",
		"progress-color", "width", "height", "outer-margin", "margin",
		"padding", "border-size", "border-radius", "icons", "max-icon-size",
		"icon-path", "icon-location", "icon-border-radius", "actions",
		"group-by", "max-visible", "format", "text-alignment",
		"default-timeout", "ignore-timeout", "invisible", "markup", "history",
		"output", "anchor", "layer", "on-button-left", "on-button-middle",
		"on-button-right", "on-touch", "on-notify",
	};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
		if (strcmp(key, keys[i]) == 0) {
			return true;
		}
	}
	return false;
}

// Checks a value as mako's apply_style_option would.
static bool valid_option(const char *key, const char *v) {
	struct swine_color c;
	struct notify_button b;
	uint32_t fields;
	if (strcmp(key, "background-color") == 0 ||
			strcmp(key, "text-color") == 0 ||
			strcmp(key, "border-color") == 0) {
		return css_parse_color(NULL, v, &c);
	}
	if (has_prefix(key, "on-")) {
		return parse_button(v, &b);
	}
	if (strcmp(key, "group-by") == 0) {
		return parse_fields(v, &fields);
	}
	if (strcmp(key, "anchor") == 0) {
		return parse_anchor(v) >= 0;
	}
	if (strcmp(key, "icon-location") == 0) {
		return strcmp(v, "left") == 0 || strcmp(v, "right") == 0 ||
			strcmp(v, "top") == 0 || strcmp(v, "bottom") == 0;
	}
	if (strcmp(key, "text-alignment") == 0) {
		return strcmp(v, "left") == 0 || strcmp(v, "center") == 0 ||
			strcmp(v, "right") == 0;
	}
	if (strcmp(key, "layer") == 0) {
		return strcmp(v, "background") == 0 || strcmp(v, "bottom") == 0 ||
			strcmp(v, "top") == 0 || strcmp(v, "overlay") == 0;
	}
	return true;
}

static bool mode_active(const char *mode) {
	for (int i = 0; i < st.modes->length; ++i) {
		if (strcmp(st.modes->items[i], mode) == 0) {
			return true;
		}
	}
	return false;
}

static bool str_matches(const char *want, const char *have) {
	return !want || strcmp(want, have ? have : "") == 0;
}

static bool has_actions(const struct swine_notification *n) {
	return n->actions && n->actions->length > 0;
}

// Where a notification is shown, for output= and anchor= criteria (mako's
// mako_surface).
struct surface {
	char output[64];
	int anchor;
};

static void surface_of(const struct notify_style *s, struct surface *out) {
	const char *name = "";
	if (s->output[0] && output_by_name(s->output)) {
		name = s->output;
	} else {
		struct swine_workspace *ws = seat_get_focused_workspace();
		if (ws && ws->output && ws->output->name) {
			name = ws->output->name;
		}
	}
	snprintf(out->output, sizeof(out->output), "%s", name);
	out->anchor = (int)s->anchor;
}

// mako's match_criteria. Without a surface (the first pass), output= and
// anchor= criteria do not match.
static bool criteria_matches(const struct criteria *c,
		const struct swine_notification *n, const struct surface *surface) {
	if (c->none || c->invalid) {
		return false;
	}
	if (c->hidden >= 0 && c->hidden != n->hidden) {
		return false;
	}
	if (!str_matches(c->app_name, n->app_name) ||
			!str_matches(c->app_icon, n->app_icon) ||
			!str_matches(c->category, n->category) ||
			!str_matches(c->desktop_entry, n->desktop_entry) ||
			!str_matches(c->summary, n->summary) ||
			!str_matches(c->body, n->body)) {
		return false;
	}
	if (c->summary_re && !swine_regex_search(c->summary_re,
			n->summary ? n->summary : "")) {
		return false;
	}
	if (c->body_re && !swine_regex_search(c->body_re, n->body ? n->body : "")) {
		return false;
	}
	if (c->actionable >= 0 && c->actionable != has_actions(n)) {
		return false;
	}
	if (c->expiring >= 0 && c->expiring != (n->expire_timeout != 0)) {
		return false;
	}
	if (c->urgency >= 0 && c->urgency != (int)n->urgency) {
		return false;
	}
	if (c->group_index >= 0 && c->group_index != n->group_index) {
		return false;
	}
	if (c->grouped >= 0 && c->grouped != (n->group_index >= 0)) {
		return false;
	}
	if (c->anchor >= 0 && (!surface || c->anchor != surface->anchor)) {
		return false;
	}
	if (c->output && (!surface || strcmp(c->output, surface->output) != 0)) {
		return false;
	}
	if (c->mode && !mode_active(c->mode)) {
		return false;
	}
	return true;
}

static void apply_criteria(struct swine_notification *n,
		const struct surface *surface) {
	default_style(&n->style);
	for (int i = 0; i < st.criteria->length; ++i) {
		struct criteria *c = st.criteria->items[i];
		if (!criteria_matches(c, n, surface)) {
			continue;
		}
		for (int j = 0; j < c->options->length; ++j) {
			struct option *o = c->options->items[j];
			apply_option(&n->style, o->key, o->value);
		}
	}
}

// mako matches once to find the surface, and again before drawing so that
// output= and anchor= criteria apply.
static void resolve_style(struct swine_notification *n) {
	apply_criteria(n, NULL);
	struct surface surface;
	surface_of(&n->style, &surface);
	apply_criteria(n, &surface);
}

static void restyle_all(void) {
	for (int i = 0; i < st.active->length; ++i) {
		resolve_style(st.active->items[i]);
	}
}

// --- config file ---------------------------------------------------------

static int parse_urgency_name(const char *v) {
	if (strcmp(v, "low") == 0) {
		return NOTIFY_URGENCY_LOW;
	} else if (strcmp(v, "normal") == 0) {
		return NOTIFY_URGENCY_NORMAL;
	} else if (strcmp(v, "critical") == 0 || strcmp(v, "high") == 0) {
		return NOTIFY_URGENCY_CRITICAL;
	}
	return -1;
}

static struct swine_regex *compile_criteria_regex(const char *pattern) {
	char *error = NULL;
	struct swine_regex *re = swine_regex_compile(pattern, false, &error);
	if (!re) {
		swine_log(SWINE_ERROR, "Invalid regex '%s': %s", pattern,
			error ? error : "?");
		free(error);
	}
	return re;
}

// Parses "[app-name="Google Chrome" urgency=critical !actionable]" as
// mako's parse_criteria and apply_criteria_field do. Returns false (and
// marks the section invalid) on an unknown field or a bad value.
static bool parse_criteria(const char *text, struct criteria *c) {
	bool ok = true;
	int argc;
	char **argv = split_args(text, &argc);
	for (int i = 0; i < argc && ok; ++i) {
		char *field = argv[i];
		char *eq = strchr(field, '=');
		char *value = NULL;
		bool pattern = false;
		if (eq) {
			*eq = '\0';
			value = eq + 1;
			strip_quotes(value);
			if (eq > field && eq[-1] == '~') {
				eq[-1] = '\0';
				pattern = true;
			}
		}
		bool negate = field[0] == '!';
		if (negate) {
			field++;
		}
		if (!value) {
			// Bare boolean fields: "actionable", "!expiring", ...
			int b = negate ? 0 : 1;
			if (strcmp(field, "actionable") == 0) {
				c->actionable = b;
			} else if (strcmp(field, "expiring") == 0) {
				c->expiring = b;
			} else if (strcmp(field, "grouped") == 0) {
				c->grouped = b;
			} else if (strcmp(field, "hidden") == 0) {
				c->hidden = b;
			} else {
				swine_log(SWINE_ERROR, "Invalid boolean criteria field '%s'",
					field);
				ok = false;
			}
			continue;
		}
		if (pattern) {
			struct swine_regex **slot = strcmp(field, "summary") == 0 ?
				&c->summary_re : strcmp(field, "body") == 0 ? &c->body_re : NULL;
			if (!slot) {
				swine_log(SWINE_ERROR, "Invalid criteria field '%s~'", field);
				ok = false;
			} else if (!(*slot = compile_criteria_regex(value))) {
				ok = false;
			}
			continue;
		}
		if (strcmp(field, "app-name") == 0) {
			c->app_name = swine_strdup(value);
		} else if (strcmp(field, "app-icon") == 0) {
			c->app_icon = swine_strdup(value);
		} else if (strcmp(field, "summary") == 0) {
			c->summary = swine_strdup(value);
		} else if (strcmp(field, "body") == 0) {
			c->body = swine_strdup(value);
		} else if (strcmp(field, "category") == 0) {
			c->category = swine_strdup(value);
		} else if (strcmp(field, "desktop-entry") == 0) {
			c->desktop_entry = swine_strdup(value);
		} else if (strcmp(field, "mode") == 0) {
			c->mode = swine_strdup(value);
		} else if (strcmp(field, "output") == 0) {
			c->output = swine_strdup(value);
		} else if (strcmp(field, "anchor") == 0) {
			if ((c->anchor = parse_anchor(value)) < 0) {
				swine_log(SWINE_ERROR, "Invalid anchor '%s'", value);
				ok = false;
			}
		} else if (strcmp(field, "urgency") == 0) {
			if ((c->urgency = parse_urgency_name(value)) < 0) {
				swine_log(SWINE_ERROR, "Invalid urgency value '%s'", value);
				ok = false;
			}
		} else if (strcmp(field, "group-index") == 0) {
			char *end;
			long index = strtol(value, &end, 10);
			if (*end || end == value || index < 0) {
				swine_log(SWINE_ERROR, "Invalid group-index value '%s'", value);
				ok = false;
			}
			c->group_index = (int)index;
		} else if (strcmp(field, "actionable") == 0 ||
				strcmp(field, "expiring") == 0 ||
				strcmp(field, "grouped") == 0 ||
				strcmp(field, "hidden") == 0) {
			bool b = parse_bool(value);
			if (!b && strcmp(value, "0") != 0 && strcmp(value, "false") != 0) {
				swine_log(SWINE_ERROR, "Invalid %s value '%s'", field, value);
				ok = false;
			}
			int *slot = field[0] == 'a' ? &c->actionable :
				field[0] == 'e' ? &c->expiring :
				field[0] == 'g' ? &c->grouped : &c->hidden;
			*slot = b;
		} else {
			swine_log(SWINE_ERROR, "Invalid criteria field '%s'", field);
			ok = false;
		}
	}
	free_argv(argc, argv);
	// User sections never style the "(%h more)" placeholder unless they
	// ask for it.
	if (c->hidden < 0) {
		c->hidden = 0;
	}
	return ok;
}

static bool has_option(const struct criteria *c, const char *key) {
	for (int i = 0; i < c->options->length; ++i) {
		struct option *o = c->options->items[i];
		if (strcmp(o->key, key) == 0) {
			return true;
		}
	}
	return false;
}

// mako's validate_criteria: what a section may set depends on what it
// matches.
static bool validate_criteria(const struct criteria *c, const char *name) {
	const char *problem = NULL;
	if (c->grouped >= 0 || c->group_index >= 0 || c->output ||
			c->anchor >= 0) {
		if (has_option(c, "anchor")) {
			problem = "Setting `anchor` is not allowed when matching "
				"`grouped`, `group-index`, `output`, or `anchor`";
		} else if (has_option(c, "output")) {
			problem = "Setting `output` is not allowed when matching "
				"`grouped`, `group-index`, `output`, or `anchor`";
		} else if (has_option(c, "group-by")) {
			problem = "Setting `group-by` is not allowed when matching "
				"`grouped`, `group-index`, `output`, or `anchor`";
		}
	}
	bool any_but_surface = c->app_name || c->app_icon || c->summary ||
		c->summary_re || c->body || c->body_re || c->category ||
		c->desktop_entry || c->mode || c->urgency >= 0 || c->expiring >= 0 ||
		c->actionable >= 0 || c->grouped >= 0 || c->group_index >= 0;
	if (!problem && has_option(c, "max-visible") && any_but_surface) {
		problem = "Setting `max_visible` is allowed only for `output` and/or "
			"`anchor`";
	}
	if (!problem && c->hidden == 1 && any_but_surface) {
		problem = "Can only set `hidden` along with `output` and/or `anchor`";
	}
	if (!problem && c->summary && c->summary_re) {
		problem = "Cannot set both `summary` and `summary~`";
	}
	if (!problem && c->body && c->body_re) {
		problem = "Cannot set both `body` and `body~`";
	}
	if (problem) {
		swine_log(SWINE_ERROR, "%s: [%s]: %s", name, c->raw, problem);
		return false;
	}
	return true;
}

static bool apply_config_option(const char *key, const char *value) {
	if (strcmp(key, "sort") == 0) {
		if (strcmp(value, "+priority") == 0) {
			st.sort_criteria |= SORT_URGENCY;
			st.sort_asc |= SORT_URGENCY;
		} else if (strcmp(value, "-priority") == 0) {
			st.sort_criteria |= SORT_URGENCY;
			st.sort_asc &= ~(uint32_t)SORT_URGENCY;
		} else if (strcmp(value, "+time") == 0) {
			st.sort_criteria |= SORT_TIME;
			st.sort_asc |= SORT_TIME;
		} else if (strcmp(value, "-time") == 0) {
			st.sort_criteria |= SORT_TIME;
			st.sort_asc &= ~(uint32_t)SORT_TIME;
		}
		return true;
	} else if (strcmp(key, "max-history") == 0) {
		st.max_history = atoi(value);
		return true;
	} else if (strcmp(key, "windows-notifications") == 0) {
		// swinewm's own: mirror the toasts of Windows applications
		if (strcmp(value, "auto") == 0) {
			st.windows = NOTIFY_WINDOWS_AUTO;
		} else {
			st.windows = parse_bool(value) || strcmp(value, "yes") == 0 ||
				strcmp(value, "on") == 0 ? NOTIFY_WINDOWS_ON : NOTIFY_WINDOWS_OFF;
		}
		return true;
	}
	return false;
}

enum notify_windows notify_windows_mode(void) {
	ensure_init();
	return st.windows;
}

static bool parse_config_text(const char *text, const char *name, int depth);

// mako's include: another config file read in place, starting in the global
// section. mako wants an absolute path or ~/; here a relative one is also
// taken relative to the including file.
static bool include_config(const char *value, const char *name, int depth) {
	if (depth >= 16) {
		swine_log(SWINE_ERROR, "%s: too many nested includes", name);
		return false;
	}
	char *path = resolve_path(value, name);
	char *text = read_file(path);
	bool ok = text != NULL;
	if (!text) {
		swine_log(SWINE_ERROR, "Unable to open %s for reading", path);
	} else {
		ok = parse_config_text(text, path, depth + 1);
	}
	free(text);
	free(path);
	return ok;
}

bool notify_parse_config(const char *text, const char *name) {
	free_config_state();
	ensure_init();
	bool ok = parse_config_text(text, name, 0);
	for (int i = 1; i < st.criteria->length; ++i) {
		struct criteria *c = st.criteria->items[i];
		if (!c->invalid && !validate_criteria(c, name)) {
			c->invalid = true;
			ok = false;
		}
	}
	restyle_all();
	return ok;
}

static bool parse_config_text(const char *text, const char *name, int depth) {
	struct criteria *global = st.criteria->items[0];
	struct criteria *current = global;
	bool ok = true;
	int line_number = 0;
	list_t *lines = split_string(text, "\n");
	for (int i = 0; i < lines->length; ++i) {
		char *line = lines->items[i];
		line_number++;
		strip_whitespace(line);
		if (!*line || line[0] == '#') {
			continue;
		}
		if (line[0] == '[') {
			char *end = strrchr(line, ']');
			if (!end) {
				swine_log(SWINE_ERROR, "%s:%d: missing ']'", name, line_number);
				ok = false;
				continue;
			}
			*end = '\0';
			current = criteria_create(line + 1);
			if (!parse_criteria(line + 1, current)) {
				swine_log(SWINE_ERROR, "%s:%d: invalid criteria [%s]", name,
					line_number, line + 1);
				current->invalid = true;
				ok = false;
			}
			list_add(st.criteria, current);
			continue;
		}
		char *eq = strchr(line, '=');
		if (!eq) {
			swine_log(SWINE_ERROR, "%s:%d: expected key=value", name, line_number);
			ok = false;
			continue;
		}
		*eq = '\0';
		char *key = line;
		char *value = eq + 1;
		strip_whitespace(key);
		strip_whitespace(value);
		if (current == global && strcmp(key, "include") == 0) {
			if (!include_config(value, name, depth)) {
				ok = false;
			}
			continue;
		}
		if (current == global && apply_config_option(key, value)) {
			continue;
		}
		if (!known_option(key) || !valid_option(key, value)) {
			swine_log(SWINE_ERROR, "%s:%d: failed to parse option '%s=%s'",
				name, line_number, key, value);
			ok = false;
			continue;
		}
		add_option(current, key, value);
	}
	list_free_items_and_destroy(lines);
	return ok;
}

void notify_load_config(void) {
	char *path = find_config_file("mako/config");
	char *text = path ? read_file(path) : NULL;
	if (text) {
		notify_parse_config(text, path);
		swine_log(SWINE_INFO, "Loaded notification config %s", path);
	} else {
		free_config_state();
		ensure_init();
	}
	st.loaded = true;
	restyle_all();
	free(text);
	free(path);
}

// --- notifications -------------------------------------------------------

static void ensure_loaded(void) {
	ensure_init();
}

struct swine_notification *notify_by_id(uint32_t id) {
	ensure_loaded();
	for (int i = 0; i < st.active->length; ++i) {
		struct swine_notification *n = st.active->items[i];
		if (n->id == id) {
			return n;
		}
	}
	return NULL;
}

// The given notification, or with id 0 the first one shown.
static struct swine_notification *notification_or_first(uint32_t id) {
	ensure_loaded();
	if (id) {
		return notify_by_id(id);
	}
	return st.active->length ? st.active->items[0] : NULL;
}

static void compute_expiry(struct swine_notification *n, long long now_ms) {
	int timeout = n->expire_timeout;
	if (timeout < 0 || n->style.ignore_timeout) {
		timeout = n->style.default_timeout;
	}
	n->expire_ms = timeout > 0 ? now_ms + timeout : 0;
}

// mako's get_last_notif_by_urgency: the position after the last
// notification of the given urgency, trying the next urgencies in the
// given direction; the start if there is none.
static int position_by_urgency(int urgency, int direction) {
	for (int current = urgency; current >= -1 &&
			current <= NOTIFY_URGENCY_CRITICAL; current += direction) {
		for (int i = st.active->length - 1; i >= 0; --i) {
			struct swine_notification *n = st.active->items[i];
			if ((int)n->urgency == current) {
				return i + 1;
			}
		}
	}
	return 0;
}

// mako's insert_notification.
static void insert_notification(struct swine_notification *n) {
	int position = 0;
	if (st.sort_criteria == SORT_TIME && !(st.sort_asc & SORT_TIME)) {
		position = 0;
	} else if (st.sort_criteria == SORT_TIME) {
		position = st.active->length;
	} else if (st.sort_criteria & SORT_URGENCY) {
		int direction = (st.sort_asc & SORT_URGENCY) ? -1 : 1;
		int offset = (st.sort_asc & SORT_TIME) ? 0 : direction;
		position = position_by_urgency((int)n->urgency + offset, direction);
	}
	list_insert(st.active, position, n);
}

// Whether b belongs to a's group: the same values in all the group-by
// fields (mako builds a criteria from a for this).
static bool same_group(const struct swine_notification *a,
		const struct swine_notification *b, uint32_t fields) {
	if (fields & NOTIFY_FIELD_NONE) {
		return false;
	}
	return (!(fields & NOTIFY_FIELD_APP_NAME) ||
			strcmp(a->app_name, b->app_name) == 0) &&
		(!(fields & NOTIFY_FIELD_APP_ICON) ||
			strcmp(a->app_icon, b->app_icon) == 0) &&
		(!(fields & NOTIFY_FIELD_ACTIONABLE) ||
			has_actions(a) == has_actions(b)) &&
		(!(fields & NOTIFY_FIELD_EXPIRING) ||
			(a->expire_timeout != 0) == (b->expire_timeout != 0)) &&
		(!(fields & NOTIFY_FIELD_URGENCY) || a->urgency == b->urgency) &&
		(!(fields & NOTIFY_FIELD_CATEGORY) ||
			strcmp(a->category, b->category) == 0) &&
		(!(fields & NOTIFY_FIELD_DESKTOP_ENTRY) ||
			strcmp(a->desktop_entry, b->desktop_entry) == 0) &&
		(!(fields & NOTIFY_FIELD_SUMMARY) ||
			strcmp(a->summary, b->summary) == 0) &&
		(!(fields & NOTIFY_FIELD_BODY) || strcmp(a->body, b->body) == 0);
}

// mako's group_notifications: gathers the notifications of ref's group
// where the first of them is, and numbers them. A group of one is no group.
static void group_notifications(const struct swine_notification *ref,
		uint32_t fields) {
	list_t *matches = create_list();
	int location = -1;
	for (int i = 0; i < st.active->length;) {
		struct swine_notification *n = st.active->items[i];
		if (!same_group(ref, n, fields)) {
			i++;
			continue;
		}
		if (location < 0) {
			location = i;
		}
		list_del(st.active, i);
		n->group_index = matches->length;
		list_add(matches, n);
	}
	if (matches->length == 1) {
		((struct swine_notification *)matches->items[0])->group_index = -1;
	}
	for (int i = 0; i < matches->length; ++i) {
		struct swine_notification *n = matches->items[i];
		n->group_count = matches->length;
		list_insert(st.active, location + i, n);
	}
	list_free(matches);
}

// The "notification" IPC event (swinewm's stand-in for the D-Bus signals
// ActionInvoked and NotificationClosed).
static void notification_event(const char *change, uint32_t id,
		const char *key, const char *value) {
	struct json_writer w;
	json_init(&w);
	json_begin_object(&w);
	json_key(&w, "change");
	json_string(&w, change);
	json_key(&w, "id");
	json_int(&w, id);
	json_key(&w, key);
	json_string(&w, value);
	json_end_object(&w);
	char *json = json_finish(&w);
	platform_ipc_event((uint32_t)SWINE_IPC_EVENT_NOTIFICATION, json);
	free(json);
}

static void execute_binding(struct swine_notification *n,
		const struct notify_button *b);

uint32_t notify_add(const struct notify_params *p, long long now_ms) {
	ensure_loaded();
	const char *app_name = p->app_name ? p->app_name : "";
	struct swine_notification *n = p->replaces_id ?
		notify_by_id(p->replaces_id) : NULL;
	if (!n && p->tag && *p->tag) {
		// Replace the notification with the same tag from the same app.
		for (int i = 0; i < st.active->length && !n; ++i) {
			struct swine_notification *t = st.active->items[i];
			if (t->tag && strcmp(t->tag, p->tag) == 0 &&
					strcmp(t->app_name, app_name) == 0) {
				n = t;
			}
		}
	}
	bool replacing = n != NULL;
	if (n) {
		// Replaced notifications keep their place.
		free_fields(n);
	} else {
		n = calloc(1, sizeof(*n));
		n->id = st.next_id++;
		n->group_index = -1;
	}
	n->app_name = swine_strdup(app_name);
	n->summary = swine_strdup(p->summary ? p->summary : "");
	n->body = swine_strdup(p->body ? p->body : "");
	n->category = swine_strdup(p->category ? p->category : "");
	n->app_icon = swine_strdup(p->app_icon ? p->app_icon : "");
	n->desktop_entry = swine_strdup(p->desktop_entry ? p->desktop_entry : "");
	n->tag = swine_strdup(p->tag ? p->tag : "");
	n->actions = create_list();
	for (int i = 0; i < p->action_count; ++i) {
		struct notify_action_item *a = calloc(1, sizeof(*a));
		a->key = swine_strdup(p->action_keys[i]);
		a->title = swine_strdup(p->action_titles[i]);
		list_add(n->actions, a);
	}
	n->progress = !p->has_progress ? -1 : p->progress < 0 ? 0 :
		p->progress > 100 ? 100 : p->progress;
	n->urgency = p->urgency;
	n->expire_timeout = p->expire_timeout;
	n->waited_on = p->waited_on;
	n->created_ms = now_ms;
	if (!replacing) {
		insert_notification(n);
	}
	resolve_style(n);
	compute_expiry(n, now_ms);
	group_notifications(n, n->style.group_by);
	restyle_all();
	uint32_t id = n->id;
	swine_log(SWINE_DEBUG, "Notification %u: %s", n->id, n->summary);
	struct notify_button on_notify = n->style.on_notify;
	execute_binding(n, &on_notify);
	return id;
}

// mako's close_notification.
static void close_notification(struct swine_notification *n,
		enum notify_close_reason reason, bool to_history) {
	static const char *reasons[] = {"", "expired", "dismissed", "request"};
	notification_event("close", n->id, "reason", reasons[reason]);
	platform_notification_closed(n->id, (int)reason);
	int index = list_find(st.active, n);
	if (index != -1) {
		list_del(st.active, index);
	}
	group_notifications(n, n->style.group_by);   // what remains of its group
	if (to_history && n->style.history && st.max_history > 0) {
		list_insert(st.history, 0, n);
		while (st.history->length > st.max_history) {
			free_notification(st.history->items[st.history->length - 1]);
			list_del(st.history, st.history->length - 1);
		}
	} else {
		free_notification(n);
	}
	restyle_all();
}

bool notify_dismiss(uint32_t id, bool to_history) {
	struct swine_notification *n = notification_or_first(id);
	if (!n) {
		return false;
	}
	close_notification(n, NOTIFY_CLOSE_DISMISSED, to_history);
	return true;
}

// mako's close_group_notifications.
bool notify_dismiss_group(uint32_t id, bool to_history) {
	struct swine_notification *top = notification_or_first(id);
	if (!top) {
		return false;
	}
	if (top->style.group_by & NOTIFY_FIELD_NONE) {
		close_notification(top, NOTIFY_CLOSE_DISMISSED, to_history);
		return true;
	}
	uint32_t fields = top->style.group_by;
	list_t *group = create_list();
	for (int i = 0; i < st.active->length; ++i) {
		struct swine_notification *n = st.active->items[i];
		if (same_group(top, n, fields)) {
			list_add(group, n);
		}
	}
	for (int i = 0; i < group->length; ++i) {
		close_notification(group->items[i], NOTIFY_CLOSE_DISMISSED, to_history);
	}
	list_free(group);
	return true;
}

void notify_dismiss_all(bool to_history) {
	ensure_loaded();
	while (st.active->length) {
		close_notification(st.active->items[0], NOTIFY_CLOSE_DISMISSED,
			to_history);
	}
}

bool notify_close(uint32_t id) {
	struct swine_notification *n = id ? notify_by_id(id) : NULL;
	if (!n) {
		return false;
	}
	close_notification(n, NOTIFY_CLOSE_REQUEST, true);
	return true;
}

// Sends the action to whoever waits for it; false if there is none such.
static bool invoke_action(struct swine_notification *n, const char *key) {
	if (!n->style.actions || !n->actions) {
		return false;
	}
	for (int i = 0; i < n->actions->length; ++i) {
		struct notify_action_item *a = n->actions->items[i];
		if (strcmp(a->key, key) == 0) {
			notification_event("action", n->id, "action", key);
			platform_notification_action(n->id, key);
			return true;
		}
	}
	return false;
}

bool notify_invoke(uint32_t id, const char *action) {
	struct swine_notification *n = notification_or_first(id);
	if (!n || !invoke_action(n, action ? action : "default")) {
		return false;
	}
	if (!n->waited_on) {
		// A waiting notify-send closes it after the action, as
		// applications do; otherwise nobody would.
		close_notification(n, NOTIFY_CLOSE_DISMISSED, true);
	}
	return true;
}

bool notify_restore(long long now_ms) {
	ensure_loaded();
	if (!st.history->length) {
		return false;
	}
	struct swine_notification *n = st.history->items[0];
	list_del(st.history, 0);
	n->created_ms = now_ms;
	n->group_index = -1;
	n->waited_on = false;
	insert_notification(n);
	resolve_style(n);
	compute_expiry(n, now_ms);
	group_notifications(n, n->style.group_by);
	restyle_all();
	return true;
}

bool notify_tick(long long now_ms) {
	ensure_loaded();
	bool changed = false;
	bool again = true;
	while (again) {
		again = false;
		for (int i = 0; i < st.active->length; ++i) {
			struct swine_notification *n = st.active->items[i];
			if (n->expire_ms && n->expire_ms <= now_ms) {
				close_notification(n, NOTIFY_CLOSE_EXPIRED, true);
				changed = again = true;
				break;
			}
		}
	}
	return changed;
}

long long notify_next_expiry(void) {
	ensure_loaded();
	long long next = 0;
	for (int i = 0; i < st.active->length; ++i) {
		struct swine_notification *n = st.active->items[i];
		if (n->expire_ms && (!next || n->expire_ms < next)) {
			next = n->expire_ms;
		}
	}
	return next;
}

list_t *notify_active(void) {
	ensure_loaded();
	return st.active;
}

list_t *notify_history(void) {
	ensure_loaded();
	return st.history;
}

// The "(%h more)" pseudo-notification of a surface.
static struct swine_notification *hidden_placeholder(
		const struct surface *surface, int hidden, int total) {
	struct swine_notification *n = calloc(1, sizeof(*n));
	n->app_name = swine_strdup("");
	n->summary = swine_strdup("");
	n->body = swine_strdup("");
	n->category = swine_strdup("");
	n->app_icon = swine_strdup("");
	n->desktop_entry = swine_strdup("");
	n->tag = swine_strdup("");
	n->progress = -1;
	n->group_index = -1;
	n->hidden = true;
	n->hidden_count = hidden;
	n->total_count = total;
	apply_criteria(n, surface);
	// It is drawn below the others on their surface.
	snprintf(n->style.output, sizeof(n->style.output), "%s", surface->output);
	n->style.anchor = (enum notify_anchor)surface->anchor;
	return n;
}

// mako's render loop, per surface (output and anchor): max-visible counts
// groups, the rest goes into the "(%h more)" placeholder.
list_t *notify_visible(void) {
	ensure_loaded();
	restyle_all();   // the focused output may have changed
	list_t *visible = create_list();
	int count = st.active->length;
	struct surface *surfaces = calloc((size_t)count + 1, sizeof(*surfaces));
	int nsurfaces = 0;
	for (int i = 0; i < count; ++i) {
		struct surface s;
		surface_of(&((struct swine_notification *)st.active->items[i])->style,
			&s);
		bool known = false;
		for (int j = 0; j < nsurfaces && !known; ++j) {
			known = surfaces[j].anchor == s.anchor &&
				strcmp(surfaces[j].output, s.output) == 0;
		}
		if (!known) {
			surfaces[nsurfaces++] = s;
		}
	}
	for (int j = 0; j < nsurfaces; ++j) {
		int visible_count = 0, hidden_count = 0, total = 0;
		for (int i = 0; i < count; ++i) {
			struct swine_notification *n = st.active->items[i];
			struct surface s;
			surface_of(&n->style, &s);
			if (s.anchor != surfaces[j].anchor ||
					strcmp(s.output, surfaces[j].output) != 0) {
				continue;
			}
			total++;
			if (n->style.max_visible >= 0 &&
					visible_count >= n->style.max_visible) {
				hidden_count++;
				continue;
			}
			if (n->style.invisible) {
				continue;
			}
			list_add(visible, n);
			if (n->group_index < 1) {
				// A group counts once against max-visible.
				visible_count++;
			}
		}
		if (hidden_count > 0) {
			struct swine_notification *placeholder =
				hidden_placeholder(&surfaces[j], hidden_count, total);
			if (placeholder->style.invisible) {
				free_notification(placeholder);
			} else {
				list_add(visible, placeholder);
			}
		}
	}
	free(surfaces);
	return visible;
}

void notify_free_visible(list_t *visible) {
	for (int i = 0; i < visible->length; ++i) {
		struct swine_notification *n = visible->items[i];
		if (n->hidden) {
			free_notification(n);
		}
	}
	list_free(visible);
}

// Runs "exec" bindings as sway's exec does, with $id set to the
// notification (mako passes it to sh as $id).
static void exec_binding(const struct swine_notification *n,
		const char *command) {
	char id[16];
	snprintf(id, sizeof(id), "%u", n->id);
	char *cmd = swine_strdup(command);
	const char *vars[] = {"${id}", "$id"};
	for (size_t v = 0; v < sizeof(vars) / sizeof(vars[0]); ++v) {
		char *at;
		while ((at = strstr(cmd, vars[v])) != NULL) {
			char *next = format_str("%.*s%s%s", (int)(at - cmd), cmd, id,
				at + strlen(vars[v]));
			free(cmd);
			cmd = next;
		}
	}
	char *line = format_str("exec %s", cmd);
	platform_queue_command(line);
	free(line);
	free(cmd);
}

// mako's notification_execute_binding.
static void execute_binding(struct swine_notification *n,
		const struct notify_button *b) {
	switch (b->action) {
	case NOTIFY_ACTION_NONE:
		break;
	case NOTIFY_ACTION_DISMISS:
		close_notification(n, NOTIFY_CLOSE_DISMISSED, true);
		break;
	case NOTIFY_ACTION_DISMISS_NO_HISTORY:
		close_notification(n, NOTIFY_CLOSE_DISMISSED, false);
		break;
	case NOTIFY_ACTION_DISMISS_GROUP:
		notify_dismiss_group(n->id, true);
		break;
	case NOTIFY_ACTION_DISMISS_ALL:
		notify_dismiss_all(true);
		break;
	case NOTIFY_ACTION_INVOKE:
		invoke_action(n, b->command);
		close_notification(n, NOTIFY_CLOSE_DISMISSED, true);
		break;
	case NOTIFY_ACTION_EXEC:
		exec_binding(n, b->command);
		break;
	}
}

void notify_button(uint32_t id, int button) {
	struct swine_notification *n = id ? notify_by_id(id) : NULL;
	if (!n) {
		return;
	}
	struct notify_button b = button == 1 ? n->style.on_button_left :
		button == 2 ? n->style.on_button_middle : n->style.on_button_right;
	execute_binding(n, &b);
}

// --- icons ---------------------------------------------------------------

static int hex_value(char c) {
	return c >= '0' && c <= '9' ? c - '0' :
		c >= 'a' && c <= 'f' ? c - 'a' + 10 :
		c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// file:///C:/Users/x%20y/a.png -> C:/Users/x y/a.png
static char *path_from_uri(const char *uri) {
	const char *p = uri + strlen("file://");
	if (p[0] == '/' && isalpha((unsigned char)p[1]) && p[2] == ':') {
		p++;
	}
	char *out = malloc(strlen(p) + 1);
	size_t len = 0;
	for (; *p; ++p) {
		if (*p == '%' && hex_value(p[1]) >= 0 && hex_value(p[2]) >= 0) {
			out[len++] = (char)(hex_value(p[1]) * 16 + hex_value(p[2]));
			p += 2;
		} else {
			out[len++] = *p;
		}
	}
	out[len] = '\0';
	return out;
}

static bool is_path(const char *s) {
	return strchr(s, '/') || strchr(s, '\\') ||
		(isalpha((unsigned char)s[0]) && s[1] == ':');
}

list_t *notify_icon_candidates(const struct swine_notification *n) {
	list_t *out = create_list();
	const char *icon = n->app_icon;
	if (!icon || !*icon) {
		return out;
	}
	if (has_prefix(icon, "file://")) {
		list_add(out, path_from_uri(icon));
		return out;
	}
	if (is_path(icon)) {
		list_add(out, swine_strdup(icon));
		return out;
	}
	// An icon name: look in the icon-path directories, separated by ';'
	// or by ':' where that is not a drive letter's.
	static const char *exts[] = {"png", "ico", "jpg", "jpeg", "bmp", "gif"};
	const char *dirs = n->style.icon_path;
	while (*dirs) {
		size_t len = 0;
		while (dirs[len] && dirs[len] != ';' && !(dirs[len] == ':' &&
				!(len == 1 && isalpha((unsigned char)dirs[0])))) {
			len++;
		}
		if (len > 0) {
			char *dir = swine_strndup(dirs, len);
			size_t dlen = strlen(dir);
			const char *sep = dir[dlen - 1] == '\\' || dir[dlen - 1] == '/' ?
				"" : "\\";
			if (strchr(icon, '.')) {
				list_add(out, format_str("%s%s%s", dir, sep, icon));
			}
			for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); ++e) {
				list_add(out, format_str("%s%s%s.%s", dir, sep, icon, exts[e]));
			}
			free(dir);
		}
		dirs += len;
		if (*dirs) {
			dirs++;
		}
	}
	return out;
}

// --- formatting ----------------------------------------------------------

static void append(char **buf, size_t *len, const char *s, size_t n) {
	*buf = realloc(*buf, *len + n + 1);
	memcpy(*buf + *len, s, n);
	*len += n;
	(*buf)[*len] = '\0';
}

// Text values go into the markup escaped, as mako's format_text does;
// only the body may carry markup.
static void append_value(char **buf, size_t *len, const char *value,
		bool escape) {
	for (const char *v = value; *v; ++v) {
		const char *entity = !escape ? NULL : *v == '&' ? "&amp;" :
			*v == '<' ? "&lt;" : *v == '>' ? "&gt;" : NULL;
		if (entity) {
			append(buf, len, entity, strlen(entity));
		} else {
			append(buf, len, v, 1);
		}
	}
}

list_t *notify_format_lines(const struct swine_notification *n) {
	char *text = NULL;
	size_t len = 0;
	append(&text, &len, "", 0);
	char num[32];
	bool markup = n->style.markup;
	for (const char *p = n->style.format; *p; ++p) {
		if (*p == '\\' && p[1] == 'n') {
			append(&text, &len, "\n", 1);
			p++;
			continue;
		}
		if (*p != '%' || !p[1]) {
			append(&text, &len, p, 1);
			continue;
		}
		const char *value = "";
		bool escape = markup;
		char c = *++p;
		if (c == '%') {
			value = "%";
		} else if (n->hidden) {
			// format_hidden_text: only %h and %t
			if (c == 'h' || c == 't') {
				snprintf(num, sizeof(num), "%d",
					c == 'h' ? n->hidden_count : n->total_count);
				value = num;
			}
		} else {
			switch (c) {
			case 'a': value = n->app_name; break;
			case 's': value = n->summary; break;
			case 'b': value = n->body; escape = false; break;
			case 'i': snprintf(num, sizeof(num), "%u", n->id); value = num; break;
			case 'g':
				snprintf(num, sizeof(num), "%d", n->group_count);
				value = num;
				break;
			default: break;
			}
		}
		append_value(&text, &len, value, escape);
	}

	list_t *lines = create_list();
	list_t *raw = split_string(text, "\n");
	for (int i = 0; i < raw->length; ++i) {
		char *line = raw->items[i];
		struct notify_line *out = calloc(1, sizeof(*out));
		if (n->style.markup) {
			size_t l = strlen(line);
			out->bold = has_prefix(line, "<b>") && l >= 7 &&
				strcmp(line + l - 4, "</b>") == 0;
			out->italic = has_prefix(line, "<i>") && l >= 7 &&
				strcmp(line + l - 4, "</i>") == 0;
			out->text = markup_strip(line);
		} else {
			out->text = swine_strdup(line);
		}
		if (!*out->text) {
			free(out->text);
			free(out);
			continue;
		}
		list_add(lines, out);
	}
	list_free_items_and_destroy(raw);
	free(text);
	return lines;
}

void notify_free_lines(list_t *lines) {
	for (int i = 0; i < lines->length; ++i) {
		struct notify_line *l = lines->items[i];
		free(l->text);
		free(l);
	}
	list_free(lines);
}

// --- modes ---------------------------------------------------------------

list_t *notify_modes(void) {
	ensure_loaded();
	return st.modes;
}

void notify_set_modes(char **modes, int count) {
	ensure_loaded();
	list_free_items_and_destroy(st.modes);
	st.modes = create_list();
	for (int i = 0; i < count; ++i) {
		list_add(st.modes, swine_strdup(modes[i]));
	}
	restyle_all();
}

void notify_add_mode(const char *mode) {
	ensure_loaded();
	if (!mode_active(mode)) {
		list_add(st.modes, swine_strdup(mode));
	}
	restyle_all();
}

void notify_remove_mode(const char *mode) {
	ensure_loaded();
	for (int i = 0; i < st.modes->length; ++i) {
		if (strcmp(st.modes->items[i], mode) == 0) {
			free(st.modes->items[i]);
			list_del(st.modes, i);
			break;
		}
	}
	restyle_all();
}

void notify_toggle_mode(const char *mode) {
	ensure_loaded();
	if (mode_active(mode)) {
		notify_remove_mode(mode);
	} else {
		notify_add_mode(mode);
	}
}

static const char *urgency_name(enum notify_urgency urgency) {
	switch (urgency) {
	case NOTIFY_URGENCY_LOW: return "low";
	case NOTIFY_URGENCY_NORMAL: return "normal";
	case NOTIFY_URGENCY_CRITICAL: return "critical";
	}
	return NULL;
}

// makoctl list -j: empty strings are null.
static void json_text(struct json_writer *w, const char *key, const char *v) {
	json_key(w, key);
	json_string(w, v && *v ? v : NULL);
}

char *notify_list_json(list_t *notifications) {
	struct json_writer w;
	json_init(&w);
	json_begin_array(&w);
	for (int i = 0; i < notifications->length; ++i) {
		struct swine_notification *n = notifications->items[i];
		json_begin_object(&w);
		json_key(&w, "id");
		json_int(&w, n->id);
		json_text(&w, "app_name", n->app_name);
		json_text(&w, "app_icon", n->app_icon);
		json_text(&w, "category", n->category);
		json_text(&w, "desktop_entry", n->desktop_entry);
		json_text(&w, "summary", n->summary);
		json_text(&w, "body", n->body);
		json_text(&w, "urgency", urgency_name(n->urgency));
		json_key(&w, "actions");
		json_begin_object(&w);
		for (int j = 0; n->actions && j < n->actions->length; ++j) {
			struct notify_action_item *a = n->actions->items[j];
			json_key(&w, a->key);
			json_string(&w, a->title);
		}
		json_end_object(&w);
		json_end_object(&w);
	}
	json_end_array(&w);
	return json_finish(&w);
}

// makoctl's print_notification_as_text.
char *notify_list_text(list_t *notifications) {
	char *out = NULL;
	size_t len = 0;
	append(&out, &len, "", 0);
	for (int i = 0; i < notifications->length; ++i) {
		struct swine_notification *n = notifications->items[i];
		char *line = format_str("Notification %u:%s%s\n", n->id,
			*n->summary ? " " : "", n->summary);
		append(&out, &len, line, strlen(line));
		free(line);
		const struct {
			const char *label;
			const char *value;
		} fields[] = {
			{"App name", n->app_name},
			{"Category", n->category},
			{"Desktop entry", n->desktop_entry},
			{"Urgency", urgency_name(n->urgency)},
		};
		for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); ++f) {
			if (fields[f].value && *fields[f].value) {
				line = format_str("  %s: %s\n", fields[f].label, fields[f].value);
				append(&out, &len, line, strlen(line));
				free(line);
			}
		}
		if (has_actions(n)) {
			append(&out, &len, "  Actions:\n", strlen("  Actions:\n"));
			for (int j = 0; j < n->actions->length; ++j) {
				struct notify_action_item *a = n->actions->items[j];
				line = format_str("    %s: %s\n", a->key, a->title);
				append(&out, &len, line, strlen(line));
				free(line);
			}
		}
	}
	return out;
}
