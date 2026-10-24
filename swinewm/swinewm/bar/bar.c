// The integrated Waybar clone: config, modules and styling.
//
// Waybar reads ~/.config/waybar/config.jsonc and style.css; swinewm looks
// for waybar/config.jsonc (or waybar/config) and waybar/style.css in its
// config directories and falls back to built-in copies of
// resources/waybar/, which are adapted from Waybar's defaults with text
// instead of Font Awesome icons, since Windows does not ship that font.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/bar/bar.h"
#include "swinewm/bar/format.h"
#include "swinewm/bar/tray.h"
#include "swinewm/config.h"
#include "swinewm/input/keyboard.h"
#include "swinewm/input/seat.h"
#include "swinewm/platform.h"
#include "swinewm/sysinfo.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/view.h"
#include "swinewm/tree/workspace.h"

// Generated from resources/waybar/config.jsonc and style.css, which are
// also installed next to swinewm.exe.
#include "default_bar_config.h"
#include "default_bar_style.h"

enum module_kind {
	MOD_WORKSPACES,
	MOD_MODE,
	MOD_WINDOW,
	MOD_SCRATCHPAD,
	MOD_CLOCK,
	MOD_CPU,
	MOD_MEMORY,
	MOD_BATTERY,
	MOD_TRAY,
	MOD_LANGUAGE,
	MOD_CUSTOM,
};

struct bar_module {
	enum module_kind kind;
	enum bar_section section;
	char *name;          // as in the config, e.g. "battery#bat2"
	char *css_id;        // "battery", "custom-media"
	char *css_class;     // "bat2", or NULL
	struct json_value *cfg;  // borrowed from the config; may be NULL
	bool alt;            // showing format-alt
};

static struct {
	bool loaded;
	struct json_value *root;
	struct json_value *bar;     // the bar object inside root
	struct css_stylesheet *css;
	list_t *modules;            // struct bar_module *
	int height;
	int spacing;
	int position;               // 0 unset, 1 top, 2 bottom
} bar = {0};

static void free_modules(void) {
	if (!bar.modules) {
		return;
	}
	for (int i = 0; i < bar.modules->length; ++i) {
		struct bar_module *m = bar.modules->items[i];
		free(m->name);
		free(m->css_id);
		free(m->css_class);
		free(m);
	}
	list_free(bar.modules);
	bar.modules = NULL;
}

void bar_finish(void) {
	free_modules();
	json_value_free(bar.root);
	css_free(bar.css);
	memset(&bar, 0, sizeof(bar));
}

static bool module_kind_from_name(const char *name, enum module_kind *kind) {
	static const struct { const char *name; enum module_kind kind; } kinds[] = {
		{"sway/workspaces", MOD_WORKSPACES}, {"sway/mode", MOD_MODE},
		{"sway/window", MOD_WINDOW}, {"sway/scratchpad", MOD_SCRATCHPAD},
		{"clock", MOD_CLOCK}, {"cpu", MOD_CPU}, {"memory", MOD_MEMORY},
		{"battery", MOD_BATTERY}, {"tray", MOD_TRAY},
		{"sway/language", MOD_LANGUAGE},
	};
	if (has_prefix(name, "custom/")) {
		*kind = MOD_CUSTOM;
		return true;
	}
	size_t base_len = strcspn(name, "#");
	for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
		if (strlen(kinds[i].name) == base_len &&
				strncmp(kinds[i].name, name, base_len) == 0) {
			*kind = kinds[i].kind;
			return true;
		}
	}
	return false;
}

static void add_modules(const char *key, enum bar_section section) {
	struct json_value *list = json_object_get(bar.bar, key);
	if (!list || list->type != JSON_ARRAY) {
		return;
	}
	for (int i = 0; i < list->items->length; ++i) {
		struct json_value *v = list->items->items[i];
		if (v->type != JSON_STRING) {
			continue;
		}
		enum module_kind kind;
		if (!module_kind_from_name(v->string, &kind)) {
			swine_log(SWINE_INFO, "Bar module '%s' is not supported yet; "
				"skipped", v->string);
			continue;
		}
		struct bar_module *m = calloc(1, sizeof(*m));
		m->kind = kind;
		m->section = section;
		m->name = swine_strdup(v->string);
		// Waybar: "battery#bat2" -> id "battery", class "bat2";
		// "custom/media" -> id "custom-media"; "sway/mode" -> id "mode".
		size_t base_len = strcspn(v->string, "#");
		char *base = swine_strndup(v->string, base_len);
		if (v->string[base_len] == '#') {
			m->css_class = swine_strdup(v->string + base_len + 1);
		}
		if (kind == MOD_CUSTOM) {
			for (char *c = base; *c; ++c) {
				if (*c == '/') *c = '-';
			}
			m->css_id = base;
		} else {
			const char *slash = strrchr(base, '/');
			m->css_id = swine_strdup(slash ? slash + 1 : base);
			free(base);
		}
		m->cfg = json_object_get(bar.bar, v->string);
		list_add(bar.modules, m);
	}
}

// Applies config (JSONC) and style (CSS) text; NULL selects the default.
void bar_load_from_strings(const char *config_text, const char *style_text) {
	bar_finish();
	bar.loaded = true;
	bar.modules = create_list();
	if (config_text) {
		char *error = NULL;
		bar.root = json_parse(config_text, &error);
		if (!bar.root) {
			swine_log(SWINE_ERROR, "Bar config: %s; using defaults", error);
			free(error);
		}
	}
	if (!bar.root) {
		bar.root = json_parse(default_bar_config, NULL);
	}
	// The config may hold several bars; the first one is used for now.
	bar.bar = bar.root;
	if (bar.root && bar.root->type == JSON_ARRAY) {
		bar.bar = bar.root->items->length ? bar.root->items->items[0] : NULL;
	}
	bar.css = css_parse(style_text ? style_text : default_bar_style);

	bar.height = (int)json_get_number(bar.bar, "height", 0);
	if (bar.height <= 0) {
		// Auto height: Waybar sizes to the content; estimate from the font.
		struct swine_style window;
		struct css_element el = {"window", "waybar", NULL, NULL};
		css_compute(bar.css, &el, NULL, &window);
		bar.height = (int)ceil(window.font_size * 1.6) + 6 +
			window.border_bottom.width + window.border_top.width;
	}
	bar.spacing = (int)json_get_number(bar.bar, "spacing", 4);
	const char *position = json_get_string(bar.bar, "position", NULL);
	bar.position = !position ? 0 : strcmp(position, "bottom") == 0 ? 2 : 1;

	add_modules("modules-left", BAR_SECTION_LEFT);
	add_modules("modules-center", BAR_SECTION_CENTER);
	add_modules("modules-right", BAR_SECTION_RIGHT);
}

void bar_load(void) {
	char *config_path = find_config_file("waybar/config.jsonc");
	if (!config_path) {
		config_path = find_config_file("waybar/config");
	}
	char *config_text = config_path ? read_file(config_path) : NULL;
	char *style_path = find_config_file("waybar/style.css");
	char *style_text = style_path ? read_file(style_path) : NULL;
	if (config_text) {
		swine_log(SWINE_INFO, "Loading bar config %s", config_path);
	}
	if (style_text) {
		swine_log(SWINE_INFO, "Loading bar style %s", style_path);
	}
	bar_load_from_strings(config_text, style_text);
	free(config_text);
	free(config_path);
	free(style_text);
	free(style_path);
}

static void ensure_loaded(void) {
	if (!bar.loaded) {
		bar_load();
	}
}

bool bar_has_tray(void) {
	if (!bar_enabled()) {
		return false;
	}
	ensure_loaded();
	for (int i = 0; i < bar.modules->length; ++i) {
		struct bar_module *m = bar.modules->items[i];
		if (m->kind == MOD_TRAY) {
			return true;
		}
	}
	return false;
}

void bar_get_theme(struct bar_theme *out) {
	ensure_loaded();
	struct css_element window_el = {"window", "waybar", NULL, NULL};
	css_compute(bar.css, &window_el, NULL, &out->window);
	struct swine_style box;
	struct css_element box_el = {"box", "workspaces", NULL, &window_el};
	css_compute(bar.css, &box_el, &out->window, &box);
	static const char *const normal[] = {"current_output", NULL};
	static const char *const focused[] = {"focused", "visible",
		"current_output", NULL};
	static const char *const urgent[] = {"urgent", "current_output", NULL};
	struct css_element el = {"button", NULL, normal, &box_el};
	css_compute(bar.css, &el, &box, &out->workspace);
	el.classes = focused;
	css_compute(bar.css, &el, &box, &out->focused);
	el.classes = urgent;
	css_compute(bar.css, &el, &box, &out->urgent);
}

bool bar_enabled(void) {
	return config && config->bar_enabled && !config->bar_invisible;
}

int bar_height(void) {
	ensure_loaded();
	return bar.height;
}

bool bar_at_top(void) {
	ensure_loaded();
	int position = bar.position ? bar.position : config->bar_position;
	return position != 2;
}

static bool output_allowed(struct swine_output *output) {
	struct json_value *filter = json_object_get(bar.bar, "output");
	if (!filter) {
		return true;
	}
	if (filter->type == JSON_STRING) {
		return strcmp(filter->string, "*") == 0 ||
			swine_strcasecmp(filter->string, output->name) == 0;
	}
	if (filter->type != JSON_ARRAY) {
		return true;
	}
	bool any_positive = false, matched = false;
	for (int i = 0; i < filter->items->length; ++i) {
		struct json_value *v = filter->items->items[i];
		if (v->type != JSON_STRING) {
			continue;
		}
		if (v->string[0] == '!') {
			if (swine_strcasecmp(v->string + 1, output->name) == 0) {
				return false;
			}
		} else {
			any_positive = true;
			if (strcmp(v->string, "*") == 0 ||
					swine_strcasecmp(v->string, output->name) == 0) {
				matched = true;
			}
		}
	}
	return matched || !any_positive;
}

static int scaled_height(struct swine_output *output) {
	double scale = output->scale > 0 ? output->scale : 1.0;
	return (int)(bar.height * scale + 0.5);
}

void bar_update_reservations(void) {
	bool enabled = bar_enabled();
	if (enabled) {
		ensure_loaded();
	}
	for (int i = 0; i < root->outputs->length; ++i) {
		struct swine_output *output = root->outputs->items[i];
		output->reserved = (struct side_gaps){0};
		if (enabled && output_allowed(output)) {
			if (bar_at_top()) {
				output->reserved.top = scaled_height(output);
			} else {
				output->reserved.bottom = scaled_height(output);
			}
		}
	}
}

bool bar_get_rect(struct swine_output *output, struct swine_box *out) {
	if (!bar_enabled() || !output->enabled || !output_allowed(output)) {
		return false;
	}
	struct swine_workspace *ws = output_get_active_workspace(output);
	if ((ws && ws->fullscreen) || output->external_fullscreen) {
		return false;
	}
	ensure_loaded();
	out->x = output->usable.x;
	out->width = output->usable.width;
	out->height = scaled_height(output);
	out->y = bar_at_top() ? output->usable.y :
		output->usable.y + output->usable.height - out->height;
	return true;
}

// --- model building -------------------------------------------------------

#define MAX_CLASSES 8

struct build_ctx {
	struct bar_model *model;
	struct swine_output *output;
	struct css_element window_el;
	time_t now;
	struct tm tm;
};

static struct bar_module_view *new_view(struct bar_module *m, int index) {
	struct bar_module_view *view = calloc(1, sizeof(*view));
	view->section = m->section;
	view->module_index = index;
	view->items = create_list();
	return view;
}

static char *truncate_utf8(char *text, int max_chars) {
	if (max_chars <= 0) {
		return text;
	}
	int chars = 0;
	for (char *p = text; *p; ++p) {
		if (((unsigned char)*p & 0xC0) != 0x80) {
			if (chars == max_chars) {
				// U+2026 HORIZONTAL ELLIPSIS
				size_t keep = (size_t)(p - text);
				char *out = malloc(keep + 4);
				memcpy(out, text, keep);
				memcpy(out + keep, "\xE2\x80\xA6", 4);
				free(text);
				return out;
			}
			chars++;
		}
	}
	return text;
}

// Adds a single-label module. Takes ownership of text; NULL or empty text
// hides the module (as an empty format does in Waybar).
static void add_label(struct build_ctx *ctx, struct bar_module *m, int index,
		char *text, const char **classes) {
	if (!text || !*text) {
		free(text);
		return;
	}
	char *plain = markup_strip(text);
	free(text);
	plain = truncate_utf8(plain, (int)json_get_number(m->cfg, "max-length", 0));

	const char *all[MAX_CLASSES + 2];
	int n = 0;
	if (m->css_class) {
		all[n++] = m->css_class;
	}
	for (int i = 0; classes && classes[i] && n < MAX_CLASSES; ++i) {
		all[n++] = classes[i];
	}
	all[n] = NULL;

	struct bar_module_view *view = new_view(m, index);
	struct css_element el = {"label", m->css_id, all, &ctx->window_el};
	struct bar_item *item = calloc(1, sizeof(*item));
	item->text = plain;
	css_compute(bar.css, &el, &ctx->model->window, &item->style);
	view->style = item->style;
	const char *on_click = json_get_string(m->cfg, "on-click", NULL);
	if (on_click) {
		item->on_click = format_str("exec %s", on_click);
	}
	const char *up = json_get_string(m->cfg, "on-scroll-up", NULL);
	const char *down = json_get_string(m->cfg, "on-scroll-down", NULL);
	item->on_scroll_up = up ? format_str("exec %s", up) : NULL;
	item->on_scroll_down = down ? format_str("exec %s", down) : NULL;
	list_add(view->items, item);
	list_add(ctx->model->modules, view);
}

// Waybar "states": {"warning": 30, "critical": 15}. For battery a state
// applies at or below its value, for others at or above. The most severe
// matching state (closest threshold) wins.
static const char *module_state(struct bar_module *m, double value,
		bool lower_is_worse) {
	struct json_value *states = json_object_get(m->cfg, "states");
	if (!states || states->type != JSON_OBJECT) {
		return NULL;
	}
	const char *best = NULL;
	double best_threshold = 0;
	for (int i = 0; i < states->items->length; ++i) {
		struct json_value *v = states->items->items[i];
		if (v->type != JSON_NUMBER) {
			continue;
		}
		bool hit = lower_is_worse ? value <= v->number : value >= v->number;
		bool better = !best || (lower_is_worse ? v->number < best_threshold :
			v->number > best_threshold);
		if (hit && better) {
			best = states->keys->items[i];
			best_threshold = v->number;
		}
	}
	return best;
}

// "format-icons" as an array indexed by a 0..100 value, or an object.
static const char *icon_for_percent(struct bar_module *m, double percent) {
	struct json_value *icons = json_object_get(m->cfg, "format-icons");
	if (!icons) {
		return "";
	}
	if (icons->type == JSON_ARRAY && icons->items->length) {
		int n = icons->items->length;
		int i = (int)(percent * n / 100.0);
		if (i >= n) i = n - 1;
		if (i < 0) i = 0;
		struct json_value *v = icons->items->items[i];
		return v->type == JSON_STRING ? v->string : "";
	}
	if (icons->type == JSON_OBJECT) {
		return json_get_string(icons, "default", "");
	}
	return "";
}

static const char *module_format(struct bar_module *m, const char *state,
		const char *special, const char *fallback) {
	char key[64];
	if (special) {
		snprintf(key, sizeof(key), "format-%s", special);
		struct json_value *v = json_object_get(m->cfg, key);
		if (v && v->type == JSON_STRING) {
			return v->string;
		}
	}
	if (state) {
		snprintf(key, sizeof(key), "format-%s", state);
		struct json_value *v = json_object_get(m->cfg, key);
		if (v && v->type == JSON_STRING) {
			return v->string;
		}
	}
	if (m->alt) {
		const char *alt = json_get_string(m->cfg, "format-alt", NULL);
		if (alt) {
			return alt;
		}
	}
	return json_get_string(m->cfg, "format", fallback);
}

static const char *workspace_icon(struct bar_module *m,
		struct swine_workspace *ws, bool focused, bool visible) {
	struct json_value *icons = json_object_get(m->cfg, "format-icons");
	if (!icons || icons->type != JSON_OBJECT) {
		return ws->name;
	}
	const char *icon;
	if (ws->urgent && (icon = json_get_string(icons, "urgent", NULL))) {
		return icon;
	}
	if (focused && (icon = json_get_string(icons, "focused", NULL))) {
		return icon;
	}
	if ((icon = json_get_string(icons, ws->name, NULL))) {
		return icon;
	}
	if (visible && (icon = json_get_string(icons, "visible", NULL))) {
		return icon;
	}
	return json_get_string(icons, "default", ws->name);
}

static void build_workspaces(struct build_ctx *ctx, struct bar_module *m,
		int index) {
	struct bar_module_view *view = new_view(m, index);
	view->is_box = true;
	const char *box_classes[2] = {m->css_class, NULL};
	struct css_element box_el = {"box", "workspaces",
		m->css_class ? box_classes : NULL, &ctx->window_el};
	css_compute(bar.css, &box_el, &ctx->model->window, &view->style);

	bool all_outputs = json_get_bool(m->cfg, "all-outputs", false);
	bool disable_scroll = json_get_bool(m->cfg, "disable-scroll", false);
	bool disable_click = json_get_bool(m->cfg, "disable-click", false);
	const char *format = json_get_string(m->cfg, "format", "{name}");
	struct swine_workspace *focused_ws = seat_get_focused_workspace();

	list_t *outputs = create_list();
	if (all_outputs) {
		list_cat(outputs, root->outputs);
	} else {
		list_add(outputs, ctx->output);
	}
	for (int o = 0; o < outputs->length; ++o) {
		struct swine_output *output = outputs->items[o];
		for (int i = 0; i < output->workspaces->length; ++i) {
			struct swine_workspace *ws = output->workspaces->items[i];
			bool focused = ws == focused_ws;
			bool visible = workspace_is_visible(ws);
			const char *classes[5];
			int n = 0;
			if (focused) classes[n++] = "focused";
			if (visible) classes[n++] = "visible";
			if (ws->urgent) classes[n++] = "urgent";
			if (output == ctx->output) classes[n++] = "current_output";
			classes[n] = NULL;

			int num = workspace_number(ws->name);
			char num_text[16];
			snprintf(num_text, sizeof(num_text), "%d", num);
			struct fmt_arg args[] = {
				{"name", false, ws->name, 0},
				{"value", false, ws->name, 0},
				{"index", false, num >= 0 ? num_text : "", 0},
				{"icon", false, workspace_icon(m, ws, focused, visible), 0},
				{"output", false, output->name, 0},
			};
			char *text = format_waybar(format, args, 5, &ctx->tm);
			struct bar_item *item = calloc(1, sizeof(*item));
			item->text = markup_strip(text);
			free(text);
			struct css_element el = {"button", NULL, classes, &box_el};
			css_compute(bar.css, &el, &view->style, &item->style);
			if (!disable_click) {
				item->on_click = format_str(
					"workspace --no-auto-back-and-forth \"%s\"", ws->name);
			}
			if (!disable_scroll) {
				item->on_scroll_up = swine_strdup(all_outputs ?
					"workspace prev" : "workspace prev_on_output");
				item->on_scroll_down = swine_strdup(all_outputs ?
					"workspace next" : "workspace next_on_output");
			}
			list_add(view->items, item);
		}
	}
	list_free(outputs);
	list_add(ctx->model->modules, view);
}

// Waybar keys "icons" and "orders" by the item's id ("spotify"); here
// that is the program: "Discord.exe" matches "Discord.exe" and "discord".
static bool tray_key_matches(const char *key, const char *app_id) {
	if (!app_id) {
		return false;
	}
	if (swine_strcasecmp(key, app_id) == 0) {
		return true;
	}
	size_t len = strlen(app_id);
	return len > 4 && swine_strcasecmp(app_id + len - 4, ".exe") == 0 &&
		strlen(key) == len - 4 && swine_strncasecmp(key, app_id, len - 4) == 0;
}

static struct json_value *tray_lookup(struct json_value *map,
		const char *app_id) {
	if (!map || map->type != JSON_OBJECT) {
		return NULL;
	}
	for (int i = 0; i < map->keys->length; ++i) {
		if (tray_key_matches(map->keys->items[i], app_id)) {
			return map->items->items[i];
		}
	}
	return NULL;
}

// "ignore-list": hide items whose id or title contains any of the strings.
static bool tray_ignored(struct json_value *list, struct tray_item *item) {
	if (!list || list->type != JSON_ARRAY) {
		return false;
	}
	for (int i = 0; i < list->items->length; ++i) {
		struct json_value *v = list->items->items[i];
		if (v->type == JSON_STRING && *v->string &&
				((item->app_id && strstr(item->app_id, v->string)) ||
				(item->title && strstr(item->title, v->string)))) {
			return true;
		}
	}
	return false;
}

struct tray_entry {
	struct tray_item *item;
	double order;
	int seq;
	const char *icon_path;
};

static int tray_entry_compare(const void *a, const void *b) {
	const struct tray_entry *x = a, *y = b;
	if (x->order != y->order) {
		return x->order < y->order ? -1 : 1;
	}
	return x->seq - y->seq;
}

static void build_tray(struct build_ctx *ctx, struct bar_module *m,
		int index) {
	list_t *items = tray_get_items();
	if (items->length == 0) {
		return;   // Waybar hides an empty tray
	}
	bool show_passive = json_get_bool(m->cfg, "show-passive-items", false);
	bool reverse = json_get_bool(m->cfg, "reverse-direction", false);
	int icon_size = (int)json_get_number(m->cfg, "icon-size", 16);
	if (icon_size <= 0) {
		icon_size = 16;
	}
	struct json_value *icons = json_object_get(m->cfg, "icons");
	struct json_value *orders = json_object_get(m->cfg, "orders");
	struct json_value *ignore = json_object_get(m->cfg, "ignore-list");

	struct tray_entry *entries = calloc((size_t)items->length, sizeof(*entries));
	int count = 0;
	for (int i = 0; i < items->length; ++i) {
		struct tray_item *item = items->items[i];
		if ((item->passive && !show_passive) || tray_ignored(ignore, item)) {
			continue;
		}
		struct json_value *icon = tray_lookup(icons, item->app_id);
		if (icon && icon->type == JSON_BOOL && !icon->boolean) {
			continue;   // "spotify": false
		}
		struct json_value *order = tray_lookup(orders, item->app_id);
		struct tray_entry *e = &entries[count++];
		e->item = item;
		e->order = order && order->type == JSON_NUMBER ? order->number : 0;
		// New items go to the right, or to the left with reverse-direction.
		e->seq = reverse ? -i : i;
		e->icon_path = icon && icon->type == JSON_STRING ? icon->string : NULL;
	}
	if (count == 0) {
		free(entries);
		return;
	}
	qsort(entries, (size_t)count, sizeof(*entries), tray_entry_compare);

	struct bar_module_view *view = new_view(m, index);
	view->is_box = true;
	view->item_spacing = (int)json_get_number(m->cfg, "spacing", 0);
	const char *box_classes[2] = {m->css_class, NULL};
	struct css_element box_el = {"box", "tray",
		m->css_class ? box_classes : NULL, &ctx->window_el};
	css_compute(bar.css, &box_el, &ctx->model->window, &view->style);
	for (int i = 0; i < count; ++i) {
		struct tray_item *t = entries[i].item;
		const char *classes[2] = {
			t->needs_attention ? "needs-attention" :
				t->passive ? "passive" : "active",
			NULL,
		};
		struct bar_item *item = calloc(1, sizeof(*item));
		item->text = swine_strdup("");
		item->tray_id = t->id;
		item->icon_size = icon_size;
		item->icon_path = entries[i].icon_path ?
			platform_expand_path(entries[i].icon_path) : NULL;
		struct css_element el = {"widget", NULL, classes, &box_el};
		css_compute(bar.css, &el, &view->style, &item->style);
		list_add(view->items, item);
	}
	free(entries);
	list_add(ctx->model->modules, view);
}

// Waybar's {flag}: the regional indicator symbols of a two-letter region.
static void layout_flag(const char *region, char *out, size_t size) {
	out[0] = '\0';
	if (!region || strlen(region) != 2 || size < 9) {
		return;
	}
	char *o = out;
	for (int i = 0; i < 2; ++i) {
		char c = region[i];
		if (c >= 'A' && c <= 'Z') {
			c = (char)(c - 'A' + 'a');
		}
		if (c < 'a' || c > 'z') {
			out[0] = '\0';
			return;
		}
		// U+1F1E6 REGIONAL INDICATOR SYMBOL LETTER A + n, in UTF-8
		unsigned cp = 0x1F1E6u + (unsigned)(c - 'a');
		*o++ = (char)(0xF0 | (cp >> 18));
		*o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
		*o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
		*o++ = (char)(0x80 | (cp & 0x3F));
	}
	*o = '\0';
}

// Waybar's sway/language: the active keyboard layout.
static void build_language(struct build_ctx *ctx, struct bar_module *m,
		int index) {
	list_t *layouts = keyboard_layouts_get();
	int active = keyboard_layouts_active();
	if (!layouts || active < 0 || active >= layouts->length) {
		return;
	}
	if (layouts->length == 1 &&
			json_get_bool(m->cfg, "hide-single-layout", false)) {
		return;
	}
	struct platform_keyboard_layout *l = layouts->items[active];
	char flag[9];
	layout_flag(l->short_name, flag, sizeof(flag));
	struct fmt_arg args[] = {
		{NULL, false, l->short_name, 0},
		{"short", false, l->short_name, 0},
		{"shortDescription", false, l->description, 0},
		{"long", false, l->name, 0},
		{"variant", false, l->variant, 0},
		{"flag", false, flag, 0},
	};
	const char *classes[] = {l->short_name, NULL};
	add_label(ctx, m, index, format_waybar(module_format(m, NULL, NULL, "{}"),
		args, 6, &ctx->tm), classes);
}

static void build_module(struct build_ctx *ctx, struct bar_module *m,
		int index) {
	switch (m->kind) {
	case MOD_WORKSPACES:
		build_workspaces(ctx, m, index);
		return;
	case MOD_MODE: {
		const char *mode = config->current_mode->name;
		if (strcmp(mode, "default") == 0) {
			return;
		}
		struct fmt_arg args[] = {{NULL, false, mode, 0}};
		add_label(ctx, m, index, format_waybar(module_format(m, NULL, NULL, "{}"),
			args, 1, &ctx->tm), NULL);
		return;
	}
	case MOD_WINDOW: {
		struct swine_workspace *ws = output_get_active_workspace(ctx->output);
		struct swine_container *con = ws ?
			seat_get_focus_inactive_view(&ws->node) : NULL;
		struct swine_view *view = con ? con->view : NULL;
		if (!view) {
			return;
		}
		struct fmt_arg args[] = {
			{"title", false, view->title, 0},
			{"app_id", false, view->app_id, 0},
			{"shell", false, "native", 0},
		};
		add_label(ctx, m, index, format_waybar(module_format(m, NULL, NULL, "{}"),
			args, 3, &ctx->tm), NULL);
		return;
	}
	case MOD_SCRATCHPAD: {
		int count = root->scratchpad->length;
		if (count == 0 && !json_get_bool(m->cfg, "show-empty", false)) {
			return;
		}
		struct json_value *icons = json_object_get(m->cfg, "format-icons");
		const char *icon = "";
		if (icons && icons->type == JSON_ARRAY && icons->items->length) {
			int i = count > 0 && icons->items->length > 1 ? 1 : 0;
			struct json_value *v = icons->items->items[i];
			icon = v->type == JSON_STRING ? v->string : "";
		}
		struct fmt_arg args[] = {
			{"count", true, NULL, count},
			{"icon", false, icon, 0},
		};
		const char *classes[] = {count == 0 ? "empty" : NULL, NULL};
		add_label(ctx, m, index, format_waybar(module_format(m, NULL, NULL,
			"{icon} {count}"), args, 2, &ctx->tm), classes);
		return;
	}
	case MOD_CLOCK:
		add_label(ctx, m, index, format_waybar(module_format(m, NULL, NULL,
			"{:%H:%M}"), NULL, 0, &ctx->tm), NULL);
		return;
	case MOD_CPU: {
		int usage = 0;
		if (!sysinfo_cpu(&usage)) {
			return;
		}
		struct fmt_arg args[] = {
			{"usage", true, NULL, usage},
			{"icon", false, icon_for_percent(m, usage), 0},
		};
		const char *state = module_state(m, usage, false);
		const char *classes[] = {state, NULL};
		add_label(ctx, m, index, format_waybar(module_format(m, state, NULL,
			"{usage}%"), args, 2, &ctx->tm), classes);
		return;
	}
	case MOD_MEMORY: {
		struct sysinfo_memory mem;
		if (!sysinfo_memory(&mem)) {
			return;
		}
		struct fmt_arg args[] = {
			{"percentage", true, NULL, mem.percentage},
			{"used", true, NULL, mem.used_gib},
			{"total", true, NULL, mem.total_gib},
			{"avail", true, NULL, mem.avail_gib},
			{"icon", false, icon_for_percent(m, mem.percentage), 0},
		};
		const char *state = module_state(m, mem.percentage, false);
		const char *classes[] = {state, NULL};
		add_label(ctx, m, index, format_waybar(module_format(m, state, NULL,
			"{percentage}%"), args, 5, &ctx->tm), classes);
		return;
	}
	case MOD_BATTERY: {
		struct sysinfo_battery bat;
		if (!sysinfo_battery(&bat)) {
			return;
		}
		const char *status = bat.charging ? "charging" :
			bat.plugged ? (bat.capacity >= 100 ? "full" : "plugged") :
			"discharging";
		const char *state = module_state(m, bat.capacity, true);
		struct fmt_arg args[] = {
			{"capacity", true, NULL, bat.capacity},
			{"icon", false, icon_for_percent(m, bat.capacity), 0},
			{"time", false, "", 0},
		};
		const char *classes[] = {status, state, NULL};
		add_label(ctx, m, index, format_waybar(module_format(m, state, status,
			"{capacity}%"), args, 3, &ctx->tm), classes);
		return;
	}
	case MOD_TRAY:
		build_tray(ctx, m, index);
		return;
	case MOD_LANGUAGE:
		build_language(ctx, m, index);
		return;
	case MOD_CUSTOM: {
		if (json_object_get(m->cfg, "exec")) {
			// Running scripts and reading their output is not implemented.
			return;
		}
		struct fmt_arg args[] = {{NULL, false, "", 0}};
		add_label(ctx, m, index, format_waybar(module_format(m, NULL, NULL,
			"{}"), args, 1, &ctx->tm), NULL);
		return;
	}
	}
}

struct bar_model *bar_build(struct swine_output *output, time_t now) {
	ensure_loaded();
	struct build_ctx ctx = {0};
	ctx.model = calloc(1, sizeof(*ctx.model));
	ctx.model->modules = create_list();
	ctx.model->spacing = bar.spacing;
	ctx.output = output;
	ctx.now = now;
	struct tm *tm = localtime(&now);
	if (tm) {
		ctx.tm = *tm;
	}
	ctx.window_el = (struct css_element){"window", "waybar", NULL, NULL};
	css_compute(bar.css, &ctx.window_el, NULL, &ctx.model->window);
	for (int i = 0; i < bar.modules->length; ++i) {
		build_module(&ctx, bar.modules->items[i], i);
	}
	return ctx.model;
}

void bar_model_free(struct bar_model *model) {
	if (!model) {
		return;
	}
	for (int i = 0; i < model->modules->length; ++i) {
		struct bar_module_view *view = model->modules->items[i];
		for (int j = 0; j < view->items->length; ++j) {
			struct bar_item *item = view->items->items[j];
			free(item->text);
			free(item->on_click);
			free(item->on_scroll_up);
			free(item->on_scroll_down);
			free(item->icon_path);
			free(item);
		}
		list_free(view->items);
		free(view);
	}
	list_free(model->modules);
	free(model);
}

void bar_module_clicked(int module_index) {
	if (!bar.modules || module_index < 0 || module_index >= bar.modules->length) {
		return;
	}
	struct bar_module *m = bar.modules->items[module_index];
	if (json_object_get(m->cfg, "format-alt")) {
		m->alt = !m->alt;
	}
}
