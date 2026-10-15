// The integrated Mako clone: notification store, history, modes and the
// Mako config format. Defaults follow mako(5).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/bar/format.h"
#include "swinewm/config.h"
#include "swinewm/notify.h"
#include "swinewm/platform.h"

struct criteria {
	// NULL / -1 fields are not part of the criteria
	char *app_name;
	char *summary;
	char *body;
	char *category;
	char *mode;
	int urgency;
	int expiring;      // -1, 0, 1
	bool unsupported;  // uses a field we cannot evaluate yet
	list_t *options;   // struct option *
};

struct option {
	char *key;
	char *value;
};

static struct {
	list_t *criteria;      // struct criteria *; [0] is the global section
	list_t *active;        // newest first
	list_t *history;       // newest first
	list_t *modes;
	uint32_t next_id;
	int max_visible;
	int max_history;
	bool sort_ascending;
	bool loaded;
} st = {0};

static void free_notification(struct swine_notification *n) {
	free(n->app_name);
	free(n->summary);
	free(n->body);
	free(n->category);
	free(n);
}

static void free_criteria(struct criteria *c) {
	free(c->app_name);
	free(c->summary);
	free(c->body);
	free(c->category);
	free(c->mode);
	for (int i = 0; i < c->options->length; ++i) {
		struct option *o = c->options->items[i];
		free(o->key);
		free(o->value);
		free(o);
	}
	list_free(c->options);
	free(c);
}

static struct criteria *criteria_create(void) {
	struct criteria *c = calloc(1, sizeof(*c));
	c->urgency = -1;
	c->expiring = -1;
	c->options = create_list();
	return c;
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

static void ensure_init(void) {
	if (!st.active) {
		st.active = create_list();
		st.history = create_list();
		st.modes = create_list();
		list_add(st.modes, swine_strdup("default"));
		st.next_id = 1;
	}
	if (!st.criteria) {
		st.criteria = create_list();
		list_add(st.criteria, criteria_create());
		st.max_visible = 5;
		st.max_history = 5;
		st.sort_ascending = false;
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
	strcpy(s->format, "<b>%s</b>\\n%b");
	s->markup = true;
	s->history = true;
	s->anchor = NOTIFY_ANCHOR_TOP_RIGHT;
	s->on_button_left.action = NOTIFY_ACTION_INVOKE_DEFAULT;
	s->on_button_right.action = NOTIFY_ACTION_DISMISS;
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

static void parse_button(const char *v, struct notify_button *b) {
	if (strcmp(v, "none") == 0) {
		b->action = NOTIFY_ACTION_NONE;
	} else if (strcmp(v, "dismiss") == 0 || strcmp(v, "dismiss-group") == 0) {
		b->action = NOTIFY_ACTION_DISMISS;
	} else if (strcmp(v, "dismiss-all") == 0) {
		b->action = NOTIFY_ACTION_DISMISS_ALL;
	} else if (has_prefix(v, "invoke-")) {
		b->action = NOTIFY_ACTION_INVOKE_DEFAULT;
	} else if (has_prefix(v, "exec ")) {
		b->action = NOTIFY_ACTION_EXEC;
		snprintf(b->command, sizeof(b->command), "%s", v + 5);
	}
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
	} else if (strcmp(key, "border-size") == 0) {
		s->border_size = atoi(v);
	} else if (strcmp(key, "border-radius") == 0) {
		s->border_radius = atoi(v);
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
		static const char *names[] = {
			"top-right", "top-center", "top-left", "bottom-right",
			"bottom-center", "bottom-left", "center-right", "center-left",
			"center",
		};
		for (int i = 0; i < 9; ++i) {
			if (strcmp(v, names[i]) == 0) {
				s->anchor = (enum notify_anchor)i;
			}
		}
	} else if (strcmp(key, "on-button-left") == 0) {
		parse_button(v, &s->on_button_left);
	} else if (strcmp(key, "on-button-middle") == 0) {
		parse_button(v, &s->on_button_middle);
	} else if (strcmp(key, "on-button-right") == 0) {
		parse_button(v, &s->on_button_right);
	}
	// layer, icons, max-icon-size, icon-path, progress-color, actions,
	// group-by, on-touch, on-notify: not implemented yet
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

static bool criteria_matches(const struct criteria *c,
		const struct swine_notification *n) {
	if (c->unsupported) {
		return false;
	}
	if (!str_matches(c->app_name, n->app_name) ||
			!str_matches(c->summary, n->summary) ||
			!str_matches(c->body, n->body) ||
			!str_matches(c->category, n->category)) {
		return false;
	}
	if (c->urgency >= 0 && c->urgency != (int)n->urgency) {
		return false;
	}
	if (c->expiring >= 0 && c->expiring != (n->expire_timeout != 0)) {
		return false;
	}
	if (c->mode && !mode_active(c->mode)) {
		return false;
	}
	return true;
}

static void resolve_style(struct swine_notification *n) {
	default_style(&n->style);
	for (int i = 0; i < st.criteria->length; ++i) {
		struct criteria *c = st.criteria->items[i];
		if (!criteria_matches(c, n)) {
			continue;
		}
		for (int j = 0; j < c->options->length; ++j) {
			struct option *o = c->options->items[j];
			apply_option(&n->style, o->key, o->value);
		}
	}
}

// --- config file ---------------------------------------------------------

// Parses "[app-name="Google Chrome" urgency=critical !actionable]".
static struct criteria *parse_criteria(const char *text) {
	struct criteria *c = criteria_create();
	int argc;
	char **argv = split_args(text, &argc);
	for (int i = 0; i < argc; ++i) {
		char *field = argv[i];
		char *eq = strchr(field, '=');
		char *value = NULL;
		if (eq) {
			*eq = '\0';
			value = eq + 1;
			strip_quotes(value);
		}
		bool negate = field[0] == '!';
		if (negate) {
			field++;
		}
		if (strcmp(field, "app-name") == 0 && value) {
			c->app_name = swine_strdup(value);
		} else if (strcmp(field, "summary") == 0 && value) {
			c->summary = swine_strdup(value);
		} else if (strcmp(field, "body") == 0 && value) {
			c->body = swine_strdup(value);
		} else if (strcmp(field, "category") == 0 && value) {
			c->category = swine_strdup(value);
		} else if (strcmp(field, "mode") == 0 && value) {
			c->mode = swine_strdup(value);
		} else if (strcmp(field, "urgency") == 0 && value) {
			c->urgency = strcmp(value, "low") == 0 ? NOTIFY_URGENCY_LOW :
				strcmp(value, "critical") == 0 || strcmp(value, "high") == 0 ?
				NOTIFY_URGENCY_CRITICAL : NOTIFY_URGENCY_NORMAL;
		} else if (strcmp(field, "expiring") == 0) {
			c->expiring = negate ? 0 : value ? parse_bool(value) : 1;
		} else if (strcmp(field, "actionable") == 0) {
			// Actions are not supported, so nothing is actionable.
			bool want = negate ? false : value ? parse_bool(value) : true;
			if (want) {
				c->unsupported = true;
			}
		} else {
			// summary~/body~ (regex), grouped, group-index, hidden, output,
			// anchor, app-icon, desktop-entry: not supported yet
			swine_log(SWINE_INFO, "mako criteria field '%s' is not supported "
				"yet; section ignored", field);
			c->unsupported = true;
		}
	}
	free_argv(argc, argv);
	return c;
}

bool notify_parse_config(const char *text, const char *name) {
	free_config_state();
	ensure_init();
	struct criteria *current = st.criteria->items[0];
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
			current = parse_criteria(line + 1);
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
		if (current == st.criteria->items[0]) {
			// Options that only make sense globally
			if (strcmp(key, "max-visible") == 0) {
				st.max_visible = atoi(value);
				continue;
			} else if (strcmp(key, "max-history") == 0) {
				st.max_history = atoi(value);
				continue;
			} else if (strcmp(key, "sort") == 0) {
				st.sort_ascending = value[0] == '+';
				continue;
			} else if (strcmp(key, "include") == 0) {
				swine_log(SWINE_INFO, "mako include is not supported yet");
				continue;
			}
		}
		struct option *o = calloc(1, sizeof(*o));
		o->key = swine_strdup(key);
		o->value = swine_strdup(value);
		list_add(current->options, o);
	}
	list_free_items_and_destroy(lines);
	return ok;
}

static void restyle_all(void) {
	for (int i = 0; i < st.active->length; ++i) {
		resolve_style(st.active->items[i]);
	}
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

static void compute_expiry(struct swine_notification *n, long long now_ms) {
	int timeout = n->expire_timeout;
	if (timeout < 0 || n->style.ignore_timeout) {
		timeout = n->style.default_timeout;
	}
	n->expire_ms = timeout > 0 ? now_ms + timeout : 0;
}

uint32_t notify_add(const struct notify_params *p, long long now_ms) {
	ensure_loaded();
	struct swine_notification *n = p->replaces_id ?
		notify_by_id(p->replaces_id) : NULL;
	if (n) {
		free(n->app_name);
		free(n->summary);
		free(n->body);
		free(n->category);
		list_del(st.active, list_find(st.active, n));
	} else {
		n = calloc(1, sizeof(*n));
		n->id = st.next_id++;
	}
	n->app_name = swine_strdup(p->app_name ? p->app_name : "");
	n->summary = swine_strdup(p->summary ? p->summary : "");
	n->body = swine_strdup(p->body ? p->body : "");
	n->category = swine_strdup(p->category ? p->category : "");
	n->urgency = p->urgency;
	n->expire_timeout = p->expire_timeout;
	n->created_ms = now_ms;
	resolve_style(n);
	compute_expiry(n, now_ms);
	list_insert(st.active, 0, n);
	swine_log(SWINE_DEBUG, "Notification %u: %s", n->id, n->summary);
	return n->id;
}

static void retire(struct swine_notification *n, bool to_history) {
	int index = list_find(st.active, n);
	if (index != -1) {
		list_del(st.active, index);
	}
	if (to_history && n->style.history && st.max_history > 0) {
		list_insert(st.history, 0, n);
		while (st.history->length > st.max_history) {
			free_notification(st.history->items[st.history->length - 1]);
			list_del(st.history, st.history->length - 1);
		}
	} else {
		free_notification(n);
	}
}

bool notify_dismiss(uint32_t id, bool to_history) {
	ensure_loaded();
	struct swine_notification *n = id ? notify_by_id(id) :
		(st.active->length ? st.active->items[0] : NULL);
	if (!n) {
		return false;
	}
	retire(n, to_history);
	return true;
}

void notify_dismiss_all(bool to_history) {
	ensure_loaded();
	while (st.active->length) {
		retire(st.active->items[st.active->length - 1], to_history);
	}
}

bool notify_restore(long long now_ms) {
	ensure_loaded();
	if (!st.history->length) {
		return false;
	}
	struct swine_notification *n = st.history->items[0];
	list_del(st.history, 0);
	n->created_ms = now_ms;
	resolve_style(n);
	compute_expiry(n, now_ms);
	list_insert(st.active, 0, n);
	return true;
}

bool notify_tick(long long now_ms) {
	ensure_loaded();
	bool changed = false;
	for (int i = st.active->length - 1; i >= 0; --i) {
		struct swine_notification *n = st.active->items[i];
		if (n->expire_ms && n->expire_ms <= now_ms) {
			retire(n, true);
			changed = true;
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

list_t *notify_visible(void) {
	ensure_loaded();
	list_t *visible = create_list();
	for (int k = 0; k < st.active->length; ++k) {
		// active is newest first; "+time" shows the oldest first
		int i = st.sort_ascending ? st.active->length - 1 - k : k;
		struct swine_notification *n = st.active->items[i];
		if (n->style.invisible) {
			continue;
		}
		if (st.max_visible >= 0 && visible->length >= st.max_visible) {
			break;
		}
		list_add(visible, n);
	}
	return visible;
}

int notify_hidden_count(void) {
	ensure_loaded();
	int shown, total = 0;
	for (int i = 0; i < st.active->length; ++i) {
		struct swine_notification *n = st.active->items[i];
		if (!n->style.invisible) {
			total++;
		}
	}
	shown = st.max_visible >= 0 && total > st.max_visible ?
		st.max_visible : total;
	return total - shown;
}

void notify_button(uint32_t id, int button) {
	struct swine_notification *n = notify_by_id(id);
	if (!n) {
		return;
	}
	const struct notify_button *b = button == 1 ? &n->style.on_button_left :
		button == 2 ? &n->style.on_button_middle : &n->style.on_button_right;
	switch (b->action) {
	case NOTIFY_ACTION_NONE:
		break;
	case NOTIFY_ACTION_INVOKE_DEFAULT:
		// Actions are not supported yet; invoking closes the notification
		// as it does once mako has sent the action.
	case NOTIFY_ACTION_DISMISS:
		notify_dismiss(id, true);
		break;
	case NOTIFY_ACTION_DISMISS_ALL:
		notify_dismiss_all(true);
		break;
	case NOTIFY_ACTION_EXEC:
		platform_spawn(b->command);
		break;
	}
}

// --- formatting ----------------------------------------------------------

static void append(char **buf, size_t *len, const char *s, size_t n) {
	*buf = realloc(*buf, *len + n + 1);
	memcpy(*buf + *len, s, n);
	*len += n;
	(*buf)[*len] = '\0';
}

list_t *notify_format_lines(const struct swine_notification *n) {
	char *text = NULL;
	size_t len = 0;
	append(&text, &len, "", 0);
	char num[32];
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
		const char *value = NULL;
		switch (*++p) {
		case 'a': value = n->app_name; break;
		case 's': value = n->summary; break;
		case 'b': value = n->body; break;
		case 'i': snprintf(num, sizeof(num), "%u", n->id); value = num; break;
		case 'g': value = "1"; break;
		case 'h':
			snprintf(num, sizeof(num), "%d", notify_hidden_count());
			value = num;
			break;
		case 't':
			snprintf(num, sizeof(num), "%d", st.active ? st.active->length : 0);
			value = num;
			break;
		case '%': value = "%"; break;
		default: value = ""; break;
		}
		append(&text, &len, value, strlen(value));
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

char *notify_list_json(list_t *notifications) {
	static const char *urgency[] = {"low", "normal", "critical"};
	struct json_writer w;
	json_init(&w);
	json_begin_array(&w);
	for (int i = 0; i < notifications->length; ++i) {
		struct swine_notification *n = notifications->items[i];
		json_begin_object(&w);
		json_key(&w, "id");
		json_int(&w, n->id);
		json_key(&w, "app_name");
		json_string(&w, n->app_name);
		json_key(&w, "summary");
		json_string(&w, n->summary);
		json_key(&w, "body");
		json_string(&w, n->body);
		json_key(&w, "category");
		json_string(&w, n->category);
		json_key(&w, "urgency");
		json_string(&w, urgency[n->urgency]);
		json_end_object(&w);
	}
	json_end_array(&w);
	return json_finish(&w);
}
