#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fileutil.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/bar/bar.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/criteria.h"
#include "swinewm/notify.h"
#include "swinewm/platform.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/workspace.h"

struct swine_config *config = NULL;

static struct swine_mode *mode_create(const char *name) {
	struct swine_mode *mode = calloc(1, sizeof(*mode));
	mode->name = swine_strdup(name);
	mode->keysym_bindings = create_list();
	mode->mouse_bindings = create_list();
	return mode;
}

static void free_mode(struct swine_mode *mode) {
	for (int i = 0; i < mode->keysym_bindings->length; ++i) {
		free_swine_binding(mode->keysym_bindings->items[i]);
	}
	list_free(mode->keysym_bindings);
	for (int i = 0; i < mode->mouse_bindings->length; ++i) {
		free_swine_binding(mode->mouse_bindings->items[i]);
	}
	list_free(mode->mouse_bindings);
	free(mode->name);
	free(mode);
}

void free_swine_binding(struct swine_binding *binding) {
	if (!binding) {
		return;
	}
	free(binding->input);
	free(binding->command);
	free(binding);
}

struct swine_config *config_create(void) {
	struct swine_config *c = calloc(1, sizeof(*c));
	c->symbols = create_list();
	c->modes = create_list();
	c->cmd_queue = create_list();
	c->output_configs = create_list();
	c->criteria = create_list();
	c->workspace_configs = create_list();
	c->config_chain = create_list();
	c->errors = create_list();
	c->current_mode = mode_create("default");
	list_add(c->modes, c->current_mode);
	c->default_orientation = L_NONE;
	c->default_layout = L_NONE;
	c->focus_wrapping = WRAP_YES;
	c->windows_theme = -1;
	c->focus_follows_mouse = FOLLOWS_YES;
	c->mouse_warping = WARP_OUTPUT;
	c->tiling_drag = true;
	c->tiling_drag_threshold = 9;

	// Decorations: sway's defaults (config.c, do_config_defaults)
	c->border = B_NORMAL;
	c->border_thickness = 2;
	c->floating_border = B_NORMAL;
	c->floating_border_thickness = 2;
	c->hide_edge_borders = E_NONE;
	c->show_marks = true;   // sway's default
	c->hide_edge_borders_smart = ESMART_OFF;
	c->font = swine_strdup("monospace 10");
	c->titlebar_border_thickness = 1;
	c->titlebar_h_padding = 5;
	c->titlebar_v_padding = 4;
	c->border_colors.focused = (struct border_colors){
		0x4C7899FF, 0x285577FF, 0xFFFFFFFF, 0x2E9EF4FF, 0x285577FF};
	c->border_colors.focused_inactive = (struct border_colors){
		0x333333FF, 0x5F676AFF, 0xFFFFFFFF, 0x484E50FF, 0x5F676AFF};
	c->border_colors.unfocused = (struct border_colors){
		0x333333FF, 0x222222FF, 0x888888FF, 0x292D2EFF, 0x222222FF};
	c->border_colors.urgent = (struct border_colors){
		0x2F343AFF, 0x900000FF, 0xFFFFFFFF, 0x900000FF, 0x900000FF};
	c->border_colors.placeholder = (struct border_colors){
		0x000000FF, 0x0C0C0CFF, 0xFFFFFFFF, 0x000000FF, 0x0C0C0CFF};
	c->border_colors.background = 0xFFFFFFFF;
	return c;
}

void free_config(struct swine_config *c) {
	if (!c) {
		return;
	}
	for (int i = 0; i < c->symbols->length; ++i) {
		struct swine_variable *var = c->symbols->items[i];
		free(var->name);
		free(var->value);
		free(var);
	}
	list_free(c->symbols);
	for (int i = 0; i < c->modes->length; ++i) {
		free_mode(c->modes->items[i]);
	}
	list_free(c->modes);
	list_free_items_and_destroy(c->cmd_queue);
	for (int i = 0; i < c->output_configs->length; ++i) {
		free_output_config(c->output_configs->items[i]);
	}
	list_free(c->output_configs);
	for (int i = 0; i < c->criteria->length; ++i) {
		criteria_destroy(c->criteria->items[i]);
	}
	list_free(c->criteria);
	for (int i = 0; i < c->workspace_configs->length; ++i) {
		free_workspace_config(c->workspace_configs->items[i]);
	}
	list_free(c->workspace_configs);
	list_free_items_and_destroy(c->config_chain);
	list_free_items_and_destroy(c->errors);
	free(c->font);
	free(c->current_config_path);
	free(c->current_config);
	free(c);
}

struct swine_mode *config_find_mode(const char *name) {
	for (int i = 0; i < config->modes->length; ++i) {
		struct swine_mode *mode = config->modes->items[i];
		if (strcmp(mode->name, name) == 0) {
			return mode;
		}
	}
	return NULL;
}

struct swine_mode *config_add_mode(const char *name) {
	struct swine_mode *mode = config_find_mode(name);
	if (!mode) {
		mode = mode_create(name);
		list_add(config->modes, mode);
	}
	return mode;
}

char *find_config_file(const char *name) {
	list_t *paths = platform_config_paths(name);
	char *found = NULL;
	for (int i = 0; i < paths->length && !found; ++i) {
		FILE *f = swine_fopen(paths->items[i], "r");
		if (f) {
			fclose(f);
			found = swine_strdup(paths->items[i]);
		}
	}
	list_free_items_and_destroy(paths);
	return found;
}

char *read_file(const char *path) {
	FILE *f = swine_fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	long size = ftell(f);
	rewind(f);
	if (size < 0) {
		fclose(f);
		return NULL;
	}
	char *text = malloc((size_t)size + 1);
	size_t n = fread(text, 1, (size_t)size, f);
	text[n] = '\0';
	fclose(f);
	return text;
}

// Splits text into logical lines: joins trailing-backslash continuations and
// attaches a "{" that stands alone on the next line to the previous line.
static list_t *logical_lines(const char *text, list_t *line_numbers) {
	list_t *raw = create_list();
	const char *p = text;
	// Skip a UTF-8 byte order mark, which Notepad likes to write.
	if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
			(unsigned char)p[2] == 0xBF) {
		p += 3;
	}
	while (*p) {
		size_t len = strcspn(p, "\n");
		char *line = swine_strndup(p, len);
		size_t l = strlen(line);
		if (l && line[l - 1] == '\r') {
			line[--l] = '\0';
		}
		list_add(raw, line);
		p += len;
		if (*p == '\n') {
			++p;
		}
	}

	list_t *out = create_list();
	for (int i = 0; i < raw->length; ++i) {
		int start_line = i + 1;
		char *line = swine_strdup(raw->items[i]);
		size_t l = strlen(line);
		while (l && line[l - 1] == '\\' && i + 1 < raw->length) {
			line[l - 1] = '\0';
			char *next = raw->items[++i];
			char *joined = format_str("%s%s", line, next);
			free(line);
			line = joined;
			l = strlen(line);
		}
		strip_whitespace(line);
		if (!*line || line[0] == '#') {
			free(line);
			continue;
		}
		l = strlen(line);
		if (line[l - 1] != '{' && line[l - 1] != '}') {
			// Look ahead for a lone "{" on the next non-empty line.
			for (int j = i + 1; j < raw->length; ++j) {
				char *peek = swine_strdup(raw->items[j]);
				strip_whitespace(peek);
				bool empty = !*peek || peek[0] == '#';
				bool brace = strcmp(peek, "{") == 0;
				free(peek);
				if (empty) {
					continue;
				}
				if (brace) {
					char *joined = format_str("%s {", line);
					free(line);
					line = joined;
					i = j;
				}
				break;
			}
		}
		list_add(out, line);
		list_add(line_numbers, (void *)(size_t)start_line);
	}
	list_free_items_and_destroy(raw);
	return out;
}

static char *expand_line(const char *block, const char *line) {
	if (!block) {
		return swine_strdup(line);
	}
	return format_str("%s %s", block, line);
}

bool read_config_text(const char *text, const char *name) {
	list_t *line_numbers = create_list();
	list_t *lines = logical_lines(text, line_numbers);
	list_t *stack = create_list();
	bool success = true;
	const char *old_file = config->current_file;
	int old_line = config->current_line;
	config->current_file = name;

	for (int i = 0; i < lines->length; ++i) {
		char *line = lines->items[i];
		int line_number = (int)(size_t)line_numbers->items[i];
		config->current_line = line_number;
		char *block = stack->length ? stack->items[0] : NULL;
		char *expanded = expand_line(block, line);
		char *new_block = NULL;
		struct cmd_results *res = config_command(expanded, &new_block);
		switch (res->status) {
		case CMD_FAILURE:
		case CMD_INVALID:
			swine_log(SWINE_ERROR, "Error on line %d '%s': %s (%s)",
				line_number, line, res->error, name);
			list_add(config->errors, format_str("Error on line %d (%s) '%s': %s",
				line_number, name, line, res->error ? res->error : ""));
			success = false;
			break;
		case CMD_DEFER:
			swine_log(SWINE_DEBUG, "Deferring command '%s'", line);
			list_add(config->cmd_queue, swine_strdup(expanded));
			break;
		case CMD_BLOCK:
			swine_log(SWINE_DEBUG, "Entering block '%s'", new_block);
			list_insert(stack, 0, swine_strdup(new_block));
			// An empty "bar { }" still asks for a bar.
			if (strcmp(new_block, "bar") == 0 || has_prefix(new_block, "bar ")) {
				char *bar_line = swine_strdup(new_block);
				char *ignored = NULL;
				free_cmd_results(config_command(bar_line, &ignored));
				free(ignored);
				free(bar_line);
			}
			break;
		case CMD_BLOCK_END:
			if (!block) {
				swine_log(SWINE_ERROR, "Unmatched '}' on line %d (%s)",
					line_number, name);
				list_add(config->errors, format_str(
					"Error on line %d (%s): unmatched '}'", line_number, name));
				success = false;
				break;
			}
			list_del(stack, 0);
			free(block);
			break;
		case CMD_SUCCESS:
			break;
		}
		free(new_block);
		free(expanded);
		free_cmd_results(res);
	}
	if (stack->length) {
		swine_log(SWINE_ERROR, "Unclosed block '%s' in %s",
			(char *)stack->items[0], name);
		list_add(config->errors, format_str("Unclosed block '%s' in %s",
			(char *)stack->items[0], name));
		success = false;
	}
	list_free_items_and_destroy(stack);
	list_free_items_and_destroy(lines);
	list_free(line_numbers);
	config->current_file = old_file;
	config->current_line = old_line;
	return success;
}

// Ported from sway/config.c (load_include_config), minus realpath: paths
// are compared ignoring case and the kind of slash.
static bool same_path(const char *a, const char *b) {
	for (;; ++a, ++b) {
		char ca = *a == '\\' ? '/' : (char)tolower((unsigned char)*a);
		char cb = *b == '\\' ? '/' : (char)tolower((unsigned char)*b);
		if (ca != cb) {
			return false;
		}
		if (!ca) {
			return true;
		}
	}
}

// Removes "." and ".." segments (sway uses realpath for this), keeping the
// separators as they are.
static char *normalize_path(const char *path) {
	size_t len = strlen(path);
	char *out = malloc(len + 1);
	size_t n = 0;
	size_t keep = 0;   // never remove "C:" or a leading separator
	if (len >= 2 && path[1] == ':') {
		keep = 2;
	}
	while (keep < len && (path[keep] == '/' || path[keep] == '\\')) {
		++keep;
	}
	memcpy(out, path, keep);
	n = keep;
	const char *p = path + keep;
	while (*p) {
		const char *end = p + strcspn(p, "/\\");
		size_t seg = (size_t)(end - p);
		if (seg == 1 && p[0] == '.') {
			// skip
		} else if (seg == 2 && p[0] == '.' && p[1] == '.' && n > keep) {
			// drop the previous segment (and its separator)
			if (out[n - 1] == '/' || out[n - 1] == '\\') {
				--n;
			}
			while (n > keep && out[n - 1] != '/' && out[n - 1] != '\\') {
				--n;
			}
		} else if (seg > 0) {
			memcpy(out + n, p, seg);
			n += seg;
			if (*end) {
				out[n++] = *end;
			}
		}
		p = *end ? end + 1 : end;
	}
	// "a/b/" after removing a trailing "." or ".."
	if (n > keep && (out[n - 1] == '/' || out[n - 1] == '\\')) {
		--n;
	}
	out[n] = '\0';
	return out;
}

static bool load_include_config(const char *raw_path) {
	char *path = normalize_path(raw_path);
	bool ok = false;
	for (int j = 0; j < config->config_chain->length; ++j) {
		if (same_path(path, config->config_chain->items[j])) {
			swine_log(SWINE_DEBUG,
				"%s already included once, won't be included again.", path);
			goto out;
		}
	}
	char *text = read_file(path);
	if (!text) {
		swine_log(SWINE_DEBUG, "%s not found.", path);
		goto out;
	}
	swine_log(SWINE_INFO, "Including %s", path);
	list_add(config->config_chain, swine_strdup(path));
	ok = read_config_text(text, config->config_chain->items[
		config->config_chain->length - 1]);
	free(text);
out:
	free(path);
	return ok;
}

static int cmp_strings(const void *a, const void *b) {
	return strcmp(*(char *const *)a, *(char *const *)b);
}

void load_include_configs(const char *path) {
	char *resolved = config_resolve_path(path);
	char *slash = strrchr(resolved, '/');
	char *backslash = strrchr(resolved, '\\');
	if (backslash > slash) {
		slash = backslash;
	}
	const char *file = slash ? slash + 1 : resolved;
	if (!has_glob_chars(file)) {
		load_include_config(resolved);
		free(resolved);
		return;
	}
	// Like sway's wordexp(): the matches in sorted order. Wildcards are
	// supported in the file name, not in directory names.
	char *dir = slash ? swine_strndup(resolved, (size_t)(slash - resolved)) :
		swine_strdup(".");
	char sep = slash ? *slash : '/';
	list_t *names = platform_list_dir(dir);
	if (names) {
		qsort(names->items, (size_t)names->length, sizeof(void *),
			cmp_strings);
		for (int i = 0; i < names->length; ++i) {
			const char *name = names->items[i];
			// As in a shell, "*" does not match hidden files.
			if (glob_match(file, name) && (name[0] != '.' || file[0] == '.')) {
				char *full = format_str("%s%c%s", dir, sep, name);
				load_include_config(full);
				free(full);
			}
		}
		list_free_items_and_destroy(names);
	}
	free(dir);
	free(resolved);
}

static void apply_config_to_workspace(struct swine_workspace *ws, void *data) {
	(void)data;
	ws->gaps_inner = config->gaps_inner;
	ws->gaps_outer = config->gaps_outer;
}

// windows_theme auto: dark if the bar's background is dark.
static void apply_windows_theme(void) {
	int mode = config->windows_theme;
	if (mode == 2) {
		struct bar_theme theme;
		bar_get_theme(&theme);
		struct swine_color c = theme.window.background;
		double luminance = (0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b) / 255.0;
		mode = luminance < 0.5 ? 1 : 0;
	}
	platform_set_windows_theme(mode);
}

bool load_main_config(const char *path, bool is_active, bool validating) {
	char *text = path ? read_file(path) : NULL;
	if (path && !text) {
		swine_log(SWINE_ERROR, "Unable to read config file %s", path);
		if (config) {
			list_add(config->errors, format_str(
				"Unable to read config file %s", path));
		}
		return false;
	}
	if (!path) {
		swine_log(SWINE_INFO, "No config file found; using built-in defaults");
	} else {
		swine_log(SWINE_INFO, "Loading config from %s", path);
	}

	struct swine_config *old_config = config;
	config = config_create();
	config->active = is_active;
	config->reloading = old_config != NULL;
	config->validating = validating;
	config->current_config_path = swine_strdup(path);
	config->current_config = text;
	config->reading = true;
	if (path) {
		list_add(config->config_chain, normalize_path(path));
	}

	bool success = text ? read_config_text(text, path) : true;

	config->reading = false;
	config->current_mode = config_find_mode("default");
	free_config(old_config);

	if (!validating && root) {
		root_for_each_workspace(apply_config_to_workspace, NULL);
	}
	if (!validating) {
		// The integrated Waybar and Mako read their own config files; a
		// sway "reload" reloads them too.
		bar_load();
		notify_load_config();
		apply_windows_theme();
	}
	return success;
}

void config_run_deferred(void) {
	while (config->cmd_queue->length) {
		char *line = config->cmd_queue->items[0];
		list_del(config->cmd_queue, 0);
		list_t *res_list = execute_command(line, NULL);
		for (int i = 0; i < res_list->length; ++i) {
			struct cmd_results *res = res_list->items[i];
			if (res->status != CMD_SUCCESS) {
				swine_log(SWINE_ERROR, "Error on line '%s': %s", line,
					res->error ? res->error : "");
				list_add(config->errors, format_str("Error on line '%s': %s",
					line, res->error ? res->error : ""));
			}
		}
		free_cmd_results_list(res_list);
		free(line);
	}
}

static bool path_is_absolute(const char *p) {
	return p[0] == '/' || p[0] == '\\' ||
		(((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z')) &&
		p[1] == ':');
}

char *config_resolve_path(const char *path) {
	char *expanded = platform_expand_path(path);
	const char *base = config->current_file ? config->current_file :
		config->current_config_path;
	if (path_is_absolute(expanded) || !base) {
		return expanded;
	}
	const char *slash = strrchr(base, '/');
	const char *backslash = strrchr(base, '\\');
	if (backslash > slash) {
		slash = backslash;
	}
	if (!slash) {
		return expanded;
	}
	char *resolved = format_str("%.*s%c%s", (int)(slash - base), base, *slash,
		expanded);
	free(expanded);
	return resolved;
}

struct output_config *config_find_output_config(const char *name) {
	for (int i = 0; i < config->output_configs->length; ++i) {
		struct output_config *oc = config->output_configs->items[i];
		if (swine_strcasecmp(oc->name, name) == 0) {
			return oc;
		}
	}
	return NULL;
}

struct output_config *output_config_for_name(const char *output_name) {
	struct output_config *oc = config_find_output_config(output_name);
	return oc && oc->background ? oc : config_find_output_config("*");
}

void free_output_config(struct output_config *oc) {
	free(oc->name);
	free(oc->background);
	free(oc->background_mode);
	free(oc->background_fallback);
	free(oc);
}

char *do_var_replacement(char *str) {
	int i;
	char *find = str;
	while ((find = strchr(find, '$'))) {
		// Skip if escaped.
		if (find > str && find[-1] == '\\') {
			if (find == str + 1 || !(find > str + 1 && find[-2] == '\\')) {
				++find;
				continue;
			}
		}
		// Unescape double $ and move on
		if (find[1] == '$') {
			size_t length = strlen(find + 1);
			memmove(find, find + 1, length);
			find[length] = '\0';
			++find;
			continue;
		}
		// Find matching variable (symbols are sorted longest first)
		for (i = 0; i < config->symbols->length; ++i) {
			struct swine_variable *var = config->symbols->items[i];
			if (has_prefix(find, var->name)) {
				size_t vnlen = strlen(var->name);
				size_t vvlen = strlen(var->value);
				char *newstr = malloc(strlen(str) - vnlen + vvlen + 1);
				size_t offset = (size_t)(find - str);
				memcpy(newstr, str, offset);
				memcpy(newstr + offset, var->value, vvlen);
				strcpy(newstr + offset + vvlen, find + vnlen);
				free(str);
				str = newstr;
				find = str + offset + vvlen;
				break;
			}
		}
		if (i == config->symbols->length) {
			++find;
		}
	}
	return str;
}
