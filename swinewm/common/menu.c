#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fileutil.h"
#include "menu.h"
#include "stringop.h"

struct menu_entry *menu_entry_create(const char *name, const char *command) {
	struct menu_entry *entry = calloc(1, sizeof(*entry));
	entry->name = swine_strdup(name);
	entry->command = command ? swine_strdup(command) : NULL;
	return entry;
}

void menu_entry_free(struct menu_entry *entry) {
	if (entry) {
		free(entry->name);
		free(entry->command);
		free(entry->icon);
		free(entry->keywords);
		free(entry);
	}
}

// Position of word in name ignoring ASCII case, or -1.
static int find_word(const char *name, const char *word, size_t len) {
	for (size_t i = 0; name[i]; ++i) {
		size_t k = 0;
		while (k < len && name[i + k] &&
				tolower((unsigned char)name[i + k]) ==
				tolower((unsigned char)word[k])) {
			++k;
		}
		if (k == len) {
			return (int)i;
		}
	}
	return -1;
}

// The query as the initials of the name's words, in order: "vsc" for
// "Visual Studio Code", "vs19" for "Visual Studio 2019" (a number counts
// with all its digits), "ff" for "FireFox".
static bool initials_match(const char *name, const char *query) {
	if (strlen(query) < 2 || strchr(query, ' ')) {
		return false;
	}
	char initials[256];
	size_t n = 0;
	for (size_t i = 0; name[i] && n < sizeof(initials) - 1; ++i) {
		unsigned char c = (unsigned char)name[i];
		unsigned char prev = i > 0 ? (unsigned char)name[i - 1] : ' ';
		if (isdigit(c)) {
			initials[n++] = (char)c;
		} else if (isalpha(c) && (!isalnum(prev) ||
				(isupper(c) && islower(prev)))) {
			initials[n++] = (char)tolower(c);
		}
	}
	initials[n] = '\0';
	const char *at = initials;
	for (const char *q = query; *q; ++q) {
		at = strchr(at, tolower((unsigned char)*q));
		if (!at) {
			return false;
		}
		++at;
	}
	return true;
}

int menu_match_score(const char *name, const char *query) {
	int score = 0;
	const char *p = query;
	while (*p) {
		while (*p == ' ') {
			++p;
		}
		size_t len = strcspn(p, " ");
		if (len == 0) {
			break;
		}
		int at = find_word(name, p, len);
		if (at < 0) {
			return initials_match(name, query) ? 2 : -1;
		}
		if (at == 0) {
			score += 3;   // the name starts with it
		} else if (!isalnum((unsigned char)name[at - 1])) {
			score += 2;   // a word in the name starts with it
		} else {
			score += 1;
		}
		p += len;
	}
	return score;
}

// Where needle is in text (first len bytes of needle), or -1.
static int find(const char *text, const char *needle, size_t len,
		bool insensitive) {
	for (size_t i = 0; text[i]; ++i) {
		size_t k = 0;
		while (k < len && text[i + k] && (insensitive ?
				tolower((unsigned char)text[i + k]) ==
				tolower((unsigned char)needle[k]) :
				text[i + k] == needle[k])) {
			++k;
		}
		if (k == len) {
			return (int)i;
		}
	}
	return len == 0 ? 0 : -1;
}

static bool same_char(char a, char b, bool insensitive) {
	return insensitive ? tolower((unsigned char)a) == tolower((unsigned char)b) :
		a == b;
}

// wofi's fuzzy matching: the query's characters in order. The score
// favours characters at the start, at word starts (after a space or
// punctuation, or a capital after a small letter), and next to each other.
static int fuzzy_score(const char *text, const char *query, bool insensitive) {
	int score = 1000;
	int prev = -2;
	size_t i = 0;
	for (const char *q = query; *q; ++q) {
		while (text[i] && !same_char(text[i], *q, insensitive)) {
			++i;
		}
		if (!text[i]) {
			return -1;
		}
		unsigned char c = (unsigned char)text[i];
		unsigned char before = i > 0 ? (unsigned char)text[i - 1] : 0;
		if (i == 0) {
			score += 8;
		} else if (!isalnum(before) && before < 0x80) {
			score += 6;
		} else if (isupper(c) && islower(before)) {
			score += 5;
		}
		if ((int)i == prev + 1) {
			score += 4;
		} else if (prev >= 0) {
			int gap = (int)i - prev - 1;
			score -= gap < 3 ? gap : 3;
		}
		prev = (int)i;
		++i;
	}
	return score;
}

// Score of text for query (< 0: no match; higher is better) in a mode.
static int match_text(const char *text, const char *query,
		const struct menu_options *o) {
	if (!text) {
		return -1;
	}
	enum menu_matching matching = o ? o->matching : MENU_MATCHING_DEFAULT;
	bool insensitive = o && o->insensitive;
	switch (matching) {
	case MENU_MATCHING_DEFAULT:
		break;
	case MENU_MATCHING_CONTAINS: {
		int at = find(text, query, strlen(query), insensitive);
		return at < 0 ? -1 : at == 0 && *query ? 1 : 0;
	}
	case MENU_MATCHING_MULTI_CONTAINS: {
		long sum = 0;
		for (const char *p = query; *p; ) {
			while (*p == ' ') {
				++p;
			}
			size_t len = strcspn(p, " ");
			if (len == 0) {
				break;
			}
			int at = find(text, p, len, insensitive);
			if (at < 0) {
				return -1;
			}
			sum += at;
			p += len;
		}
		return sum > 1000000 ? 0 : (int)(1000000 - sum);
	}
	case MENU_MATCHING_FUZZY:
		return *query ? fuzzy_score(text, query, insensitive) : 0;
	}
	return menu_match_score(text, query);
}

struct ranked {
	struct menu_entry *entry;
	size_t index;
	int score;
	bool in_name;
	bool shorter_first;   // swinemenu's matching, a typed query
	bool by_index;        // wofi's sort_order=default for dmenu, window
};

static int compare_ranked(const void *a, const void *b) {
	const struct ranked *x = a, *y = b;
	if (x->entry->uses != y->entry->uses) {
		return x->entry->uses > y->entry->uses ? -1 : 1;
	}
	if (x->in_name != y->in_name) {
		return x->in_name ? -1 : 1;
	}
	if (x->score != y->score) {
		return x->score > y->score ? -1 : 1;
	}
	// For a typed query shorter names are closer matches; an empty query
	// lists everything alphabetically.
	size_t lx = strlen(x->entry->name), ly = strlen(y->entry->name);
	if (x->shorter_first && x->score > 0 && lx != ly) {
		return lx < ly ? -1 : 1;
	}
	int c = x->by_index ? 0 : swine_strcasecmp(x->entry->name, y->entry->name);
	if (c != 0) {
		return c;
	}
	return x->index < y->index ? -1 : 1;
}

list_t *menu_filter(list_t *entries, const char *query,
		const struct menu_options *o) {
	struct ranked *ranked = calloc((size_t)entries->length + 1, sizeof(*ranked));
	size_t n = 0;
	const char *q = query ? query : "";
	bool shorter_first = !o || o->matching == MENU_MATCHING_DEFAULT;
	bool by_index = o && !o->sort_alphabetical &&
		(o->mode == MENU_MODE_DMENU || o->mode == MENU_MODE_WINDOW);
	for (int i = 0; i < entries->length; ++i) {
		struct menu_entry *entry = entries->items[i];
		int score = match_text(entry->name, q, o);
		bool in_name = score >= 0;
		if (!in_name && *q) {
			score = match_text(entry->keywords, q, o);
		}
		if (score >= 0) {
			ranked[n++] = (struct ranked){entry, (size_t)i, score, in_name,
				shorter_first, by_index};
		}
	}
	qsort(ranked, n, sizeof(*ranked), compare_ranked);
	list_t *result = create_list();
	for (size_t i = 0; i < n; ++i) {
		list_add(result, (void *)(uintptr_t)ranked[i].index);
	}
	free(ranked);
	return result;
}

void menu_history_load(list_t *entries, const char *path) {
	FILE *f = path ? swine_fopen(path, "r") : NULL;
	if (!f) {
		return;
	}
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		char *tab = strchr(line, '\t');
		if (!tab) {
			continue;
		}
		*tab = '\0';
		int uses = atoi(line);
		for (int i = 0; i < entries->length; ++i) {
			struct menu_entry *entry = entries->items[i];
			if (strcmp(entry->name, tab + 1) == 0) {
				entry->uses = uses;
			}
		}
	}
	fclose(f);
}

void menu_history_record(const char *path, const char *name) {
	if (!path) {
		return;
	}
	list_t *names = create_list();
	list_t *counts = create_list();
	bool found = false;
	FILE *f = swine_fopen(path, "r");
	if (f) {
		char line[1024];
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = '\0';
			char *tab = strchr(line, '\t');
			if (!tab) {
				continue;
			}
			*tab = '\0';
			int uses = atoi(line);
			if (strcmp(tab + 1, name) == 0) {
				++uses;
				found = true;
			}
			list_add(names, swine_strdup(tab + 1));
			list_add(counts, (void *)(intptr_t)uses);
		}
		fclose(f);
	}
	if (!found) {
		list_add(names, swine_strdup(name));
		list_add(counts, (void *)(intptr_t)1);
	}
	f = swine_fopen(path, "w");
	if (f) {
		for (int i = 0; i < names->length; ++i) {
			fprintf(f, "%d\t%s\n", (int)(intptr_t)counts->items[i],
				(char *)names->items[i]);
		}
		fclose(f);
	}
	list_free_items_and_destroy(names);
	list_free(counts);
}

// --- wofi's options --------------------------------------------------------

void menu_options_init(struct menu_options *o) {
	memset(o, 0, sizeof(*o));
	o->mode = MENU_MODE_DRUN;
	o->allow_images = -1;
	o->image_size = 32;
	strcpy(o->password_char, "*");
}

void menu_options_finish(struct menu_options *o) {
	free(o->prompt);
	free(o->width);
	free(o->height);
	free(o->search);
	free(o->conf);
	free(o->style);
	free(o->open_with);
	memset(o, 0, sizeof(*o));
}

static bool parse_flag(const char *value, bool *out) {
	if (!value || strcmp(value, "true") == 0 || strcmp(value, "1") == 0 ||
			strcmp(value, "yes") == 0) {
		*out = true;
	} else if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0 ||
			strcmp(value, "no") == 0) {
		*out = false;
	} else {
		return false;
	}
	return true;
}

static bool parse_int(const char *value, int *out) {
	if (!value) {
		return false;
	}
	char *end;
	long v = strtol(value, &end, 10);
	if (end == value || *end) {
		return false;
	}
	*out = (int)v;
	return true;
}

static void set_string(char **slot, const char *value) {
	free(*slot);
	*slot = value ? swine_strdup(value) : NULL;
}

// wofi(7)'s locations, by name or number.
static int parse_location(const char *value) {
	static const char *names[] = {
		"center", "top_left", "top", "top_right", "right",
		"bottom_right", "bottom", "bottom_left", "left",
	};
	for (int i = 0; i < 9; ++i) {
		if (strcmp(value, names[i]) == 0) {
			return i;
		}
	}
	// "top-left" and the like, and the numbers 0-8
	if (strchr(value, '-')) {
		char *copy = swine_strdup(value);
		for (char *c = copy; *c; ++c) {
			if (*c == '-') {
				*c = '_';
			}
		}
		int l = parse_location(copy);
		free(copy);
		return l;
	}
	int n;
	return parse_int(value, &n) && n >= 0 && n <= 8 ? n : -1;
}

// Options that are accepted and change nothing (GTK, Wayland, keys, ...).
static const char *ignored_options[] = {
	"normal_window", "allow_markup", "cache_file", "term", "hide_scroll",
	"parse_search", "no_actions", "columns", "gtk_dark", "monitor", "orientation", "halign", "valign",
	"content_halign", "filter_rate", "line_wrap", "global_coords",
	"dynamic_lines", "layer", "copy_exec", "single_click", "pre_display_cmd",
	"pre_display_exec", "use_search_box", "color", "colors", "fork",
	"define", "key_up", "key_down", "key_left", "key_right", "key_forward",
	"key_backward", "key_submit", "key_exit", "key_pgup", "key_pgdn",
	"key_expand", "key_hide_search", "key_copy", "key_custom_0",
	"close_on_focus_loss",
};

bool menu_option_set(struct menu_options *o, const char *raw_key,
		const char *value, char **error) {
	char key[64];
	snprintf(key, sizeof(key), "%s", raw_key);
	for (char *c = key; *c; ++c) {
		if (*c == '-') {
			*c = '_';
		}
	}
	bool ok = true;
	if (strcmp(key, "show") == 0 || strcmp(key, "mode") == 0) {
		if (!value) {
			ok = false;
		} else if (strcmp(value, "drun") == 0) {
			o->mode = MENU_MODE_DRUN;
		} else if (strcmp(value, "run") == 0) {
			o->mode = MENU_MODE_RUN;
		} else if (strcmp(value, "dmenu") == 0) {
			o->mode = MENU_MODE_DMENU;
		} else if (strcmp(value, "window") == 0) {
			o->mode = MENU_MODE_WINDOW;
		} else {
			ok = false;
		}
	} else if (strcmp(key, "dmenu") == 0) {
		bool on = true;
		ok = parse_flag(value, &on);
		if (ok && on) {
			o->mode = MENU_MODE_DMENU;
		}
	} else if (strcmp(key, "prompt") == 0) {
		set_string(&o->prompt, value ? value : "");
	} else if (strcmp(key, "width") == 0) {
		set_string(&o->width, value);
		ok = value && menu_size(value, 1000, -1) >= 0;
	} else if (strcmp(key, "height") == 0) {
		set_string(&o->height, value);
		ok = value && menu_size(value, 1000, -1) >= 0;
	} else if (strcmp(key, "lines") == 0) {
		ok = parse_int(value, &o->lines) && o->lines > 0;
	} else if (strcmp(key, "location") == 0) {
		int l = value ? parse_location(value) : -1;
		ok = l >= 0;
		if (ok) {
			o->location = l;
		}
	} else if (strcmp(key, "xoffset") == 0) {
		ok = parse_int(value, &o->xoffset);
	} else if (strcmp(key, "yoffset") == 0) {
		ok = parse_int(value, &o->yoffset);
	} else if (strcmp(key, "allow_images") == 0) {
		bool on = true;
		ok = parse_flag(value, &on);
		o->allow_images = on;
	} else if (strcmp(key, "image_size") == 0) {
		ok = parse_int(value, &o->image_size) && o->image_size > 0;
	} else if (strcmp(key, "hide_search") == 0) {
		ok = parse_flag(value, &o->hide_search);
	} else if (strcmp(key, "password") == 0) {
		// -P [character]; in the config password=<character>
		o->password = true;
		if (value && *value && strcmp(value, "true") != 0) {
			snprintf(o->password_char, sizeof(o->password_char), "%s", value);
		}
	} else if (strcmp(key, "matching") == 0) {
		if (!value) {
			ok = false;
		} else if (strcmp(value, "contains") == 0) {
			o->matching = MENU_MATCHING_CONTAINS;
		} else if (strcmp(value, "multi-contains") == 0) {
			o->matching = MENU_MATCHING_MULTI_CONTAINS;
		} else if (strcmp(value, "fuzzy") == 0) {
			o->matching = MENU_MATCHING_FUZZY;
		} else {
			ok = false;
		}
	} else if (strcmp(key, "insensitive") == 0) {
		ok = parse_flag(value, &o->insensitive);
	} else if (strcmp(key, "sort_order") == 0) {
		if (value && strcmp(value, "alphabetical") == 0) {
			o->sort_alphabetical = true;
		} else if (value && strcmp(value, "default") == 0) {
			o->sort_alphabetical = false;
		} else {
			ok = false;
		}
	} else if (strcmp(key, "exec_search") == 0) {
		ok = parse_flag(value, &o->exec_search);
	} else if (strcmp(key, "search") == 0) {
		set_string(&o->search, value);
		ok = value != NULL;
	} else if (strcmp(key, "conf") == 0) {
		set_string(&o->conf, value);
		ok = value != NULL;
	} else if (strcmp(key, "style") == 0 || strcmp(key, "stylesheet") == 0) {
		set_string(&o->style, value);
		ok = value != NULL;
	} else if (strcmp(key, "open_with") == 0) {
		set_string(&o->open_with, value);
		o->mode = MENU_MODE_OPENWITH;
		ok = value != NULL;
	} else {
		bool known = false;
		for (size_t i = 0; i < sizeof(ignored_options) / sizeof(ignored_options[0]);
				++i) {
			known = known || strcmp(key, ignored_options[i]) == 0;
		}
		if (!known) {
			*error = format_str("Unknown option '%s'", raw_key);
			return false;
		}
	}
	if (!ok) {
		*error = format_str("Invalid value '%s' for %s", value ? value : "",
			raw_key);
	}
	return ok;
}

bool menu_options_parse_config(struct menu_options *o, const char *text,
		char **error) {
	bool ok = true;
	list_t *lines = split_string(text, "\n");
	for (int i = 0; i < lines->length; ++i) {
		char *line = lines->items[i];
		strip_whitespace(line);
		if (!*line || line[0] == '#') {
			continue;
		}
		char *eq = strchr(line, '=');
		char *value = NULL;
		if (eq) {
			*eq = '\0';
			value = eq + 1;
			strip_whitespace(value);
		}
		strip_whitespace(line);
		char *problem = NULL;
		if (!menu_option_set(o, line, value, &problem)) {
			if (ok) {
				*error = problem;
				problem = NULL;
			}
			free(problem);
			ok = false;
		}
	}
	list_free_items_and_destroy(lines);
	return ok;
}

const char *menu_option_name(const char *flag, bool *takes_value) {
	static const struct {
		char short_flag;
		const char *name;
		bool value;
	} flags[] = {
		{'c', "conf", true}, {'s', "style", true}, {'C', "color", true},
		{'d', "dmenu", false}, {'S', "show", true}, {'W', "width", true},
		{'H', "height", true}, {'p', "prompt", true}, {'x', "xoffset", true},
		{'y', "yoffset", true}, {'n', "normal-window", false},
		{'I', "allow-images", false}, {'m', "allow-markup", false},
		{'k', "cache-file", true}, {'t', "term", true},
		{'P', "password", false}, {'e', "exec-search", false},
		{'b', "hide-scroll", false}, {'M', "matching", true},
		{'i', "insensitive", false}, {'q', "parse-search", false},
		{'l', "location", true}, {'a', "no-actions", false},
		{'D', "define", true}, {'L', "lines", true}, {'w', "columns", true},
		{'O', "sort-order", true}, {'G', "gtk-dark", false},
		{'Q', "search", true}, {'o', "monitor", true},
		{'r', "pre-display-cmd", true}, {'f', "fork", false},
		{0, "open-with", true}, {0, "image-size", true},
		{0, "hide-search", false},
	};
	for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
		bool match = (flag[0] == '-' && flag[1] == flags[i].short_flag &&
				flags[i].short_flag && !flag[2]) ||
			(flag[0] == '-' && flag[1] == '-' &&
				strcmp(flag + 2, flags[i].name) == 0);
		if (match) {
			*takes_value = flags[i].value;
			return flags[i].name;
		}
	}
	return NULL;
}

int menu_size(const char *size, int screen, int fallback) {
	if (!size) {
		return fallback;
	}
	char *end;
	double v = strtod(size, &end);
	if (end == size || v < 0) {
		return fallback;
	}
	if (*end == '%' && !end[1]) {
		return (int)(screen * v / 100.0 + 0.5);
	}
	return *end ? fallback : (int)v;
}
