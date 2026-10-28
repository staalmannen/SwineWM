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
#include "swinewm/bar/custom.h"
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
	MOD_NETWORK,
	MOD_PULSEAUDIO,
	MOD_DISK,
	MOD_TEMPERATURE,
	MOD_IDLE_INHIBITOR,
	MOD_KEYBOARD_STATE,
	MOD_POWER_PROFILES,
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
	struct bar_script *script;  // custom/* with "exec" or "exec-if"
	// The last system reading (struct sysinfo_*), read again when due.
	void *reading;
	long long next_read;
	// network: bandwidth over the last "interval"
	long long sample_ms;
	double rx_total, tx_total, rx_rate, tx_rate;
};

// idle_inhibitor: one state for every module, as in Waybar.
static struct {
	bool active;
	long long deadline;   // platform_now_ms when "timeout" ends it, or 0
} idle_inhibit;

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
		bar_script_destroy(m->script);
		free(m->reading);
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
		{"sway/language", MOD_LANGUAGE}, {"network", MOD_NETWORK},
		{"pulseaudio", MOD_PULSEAUDIO}, {"wireplumber", MOD_PULSEAUDIO},
		{"disk", MOD_DISK}, {"temperature", MOD_TEMPERATURE},
		{"idle_inhibitor", MOD_IDLE_INHIBITOR},
		{"keyboard-state", MOD_KEYBOARD_STATE},
		{"power-profiles-daemon", MOD_POWER_PROFILES},
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
		if (kind == MOD_IDLE_INHIBITOR && config->bar_enabled &&
				json_get_bool(m->cfg, "start-activated", false) &&
				!idle_inhibit.active) {
			idle_inhibit.active = true;
			sysinfo_inhibit_idle(true);
		}
		if (kind == MOD_CUSTOM && config->bar_enabled) {
			// Waybar runs a module per bar; swinewm one per module, with
			// the first monitor's name.
			struct swine_output *first = root && root->outputs->length ?
				root->outputs->items[0] : NULL;
			m->script = bar_script_create(m->cfg, first ? first->name : NULL);
		}
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

void bar_tooltip_style(struct swine_style *box, struct swine_style *label) {
	ensure_loaded();
	// GTK draws tooltips in their own window: "tooltip" and "tooltip label"
	struct css_element tooltip_el = {"tooltip", NULL, NULL, NULL};
	css_compute(bar.css, &tooltip_el, NULL, box);
	struct css_element label_el = {"label", NULL, NULL, &tooltip_el};
	css_compute(bar.css, &label_el, box, label);
	if (box->background.a == 0) {
		// A theme without tooltip rules: GTK's dark default.
		box->background = (struct swine_color){43, 48, 59, 242};
	}
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

static bool color_equal(struct swine_color a, struct swine_color b) {
	return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static bool edge_equal(struct css_edge a, struct css_edge b) {
	return a.width == b.width && color_equal(a.color, b.color);
}

// What a :hover rule may change that the platform draws differently.
static bool look_equal(const struct swine_style *a, const struct swine_style *b) {
	return color_equal(a->background, b->background) &&
		color_equal(a->color, b->color) && a->bold == b->bold &&
		a->italic == b->italic && edge_equal(a->border_top, b->border_top) &&
		edge_equal(a->border_right, b->border_right) &&
		edge_equal(a->border_bottom, b->border_bottom) &&
		edge_equal(a->border_left, b->border_left) &&
		a->inset_bottom == b->inset_bottom && a->inset_top == b->inset_top &&
		color_equal(a->inset_color, b->inset_color);
}

// An item's style, and its style while the pointer is over it (:hover).
static void compute_item_style(const struct css_element *el,
		const struct swine_style *parent, struct bar_item *item) {
	css_compute(bar.css, el, parent, &item->style);
	const char *classes[MAX_CLASSES + 4];
	int n = 0;
	for (int i = 0; el->classes && el->classes[i] && n < MAX_CLASSES + 2; ++i) {
		classes[n++] = el->classes[i];
	}
	classes[n++] = CSS_HOVER;
	classes[n] = NULL;
	struct css_element hovered = *el;
	hovered.classes = classes;
	css_compute(bar.css, &hovered, parent, &item->hover_style);
	item->has_hover = !look_equal(&item->style, &item->hover_style);
}

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
static struct bar_item *add_label(struct build_ctx *ctx, struct bar_module *m,
		int index, char *text, const char **classes) {
	if (!text || !*text) {
		free(text);
		return NULL;
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
	compute_item_style(&el, &ctx->model->window, item);
	view->style = item->style;
	const char *on_click = json_get_string(m->cfg, "on-click", NULL);
	if (on_click) {
		item->on_click = format_str("exec %s", on_click);
	}
	const char *middle = json_get_string(m->cfg, "on-click-middle", NULL);
	const char *right = json_get_string(m->cfg, "on-click-right", NULL);
	item->on_click_middle = middle ? format_str("exec %s", middle) : NULL;
	item->on_click_right = right ? format_str("exec %s", right) : NULL;
	const char *up = json_get_string(m->cfg, "on-scroll-up", NULL);
	const char *down = json_get_string(m->cfg, "on-scroll-down", NULL);
	item->on_scroll_up = up ? format_str("exec %s", up) : NULL;
	item->on_scroll_down = down ? format_str("exec %s", down) : NULL;
	list_add(view->items, item);
	list_add(ctx->model->modules, view);
	return item;
}

// Tooltip text from Pango markup: tags dropped, "\r" (which Waybar's
// scripts use for line breaks) made a line break. NULL if empty.
static char *tooltip_text(const char *markup) {
	char *text = markup_strip(markup);
	char *o = text;
	for (const char *p = text; *p; ++p) {
		if (*p == '\r') {
			if (p[1] != '\n') {
				*o++ = '\n';
			}
			continue;
		}
		*o++ = *p;
	}
	*o = '\0';
	while (o > text && (o[-1] == '\n' || o[-1] == ' ')) {
		*--o = '\0';
	}
	if (!*text) {
		free(text);
		return NULL;
	}
	return text;
}

// Waybar's tooltip: none with "tooltip": false, else tooltip-format-<state>,
// tooltip-format, the module's default format (with the label's arguments),
// or default_text.
static void set_tooltip(struct build_ctx *ctx, struct bar_module *m,
		struct bar_item *item, const char *state, const char *default_format,
		const struct fmt_arg *args, int nargs, const char *default_text) {
	if (!item || !json_get_bool(m->cfg, "tooltip", true)) {
		return;
	}
	const char *format = NULL;
	if (state) {
		char key[96];
		snprintf(key, sizeof(key), "tooltip-format-%s", state);
		format = json_get_string(m->cfg, key, NULL);
	}
	if (!format) {
		format = json_get_string(m->cfg, "tooltip-format", default_format);
	}
	char *raw = format ? format_waybar(format, args, nargs, &ctx->tm) :
		swine_strdup(default_text ? default_text : "");
	// <tt> asks for a fixed-width font (the clock's calendar).
	item->tooltip_mono = strstr(format ? format : raw, "<tt>") != NULL;
	free(item->tooltip);
	item->tooltip = tooltip_text(raw);
	free(raw);
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

// Waybar's ALabel::getIcon: "format-icons" as a string, an array (by
// percentage of max, or of {"icon": ..., "max": n} thresholds), or an object
// keyed by the first of alts it has (else "default").
static const char *module_icon(struct bar_module *m, double percentage,
		const char *const *alts, int max) {
	struct json_value *icons = json_object_get(m->cfg, "format-icons");
	if (icons && icons->type == JSON_OBJECT) {
		struct json_value *chosen = NULL;
		for (int i = 0; alts && alts[i] && !chosen; ++i) {
			struct json_value *v = alts[i][0] ? json_object_get(icons, alts[i]) : NULL;
			if (v && (v->type == JSON_STRING || v->type == JSON_ARRAY)) {
				chosen = v;
			}
		}
		icons = chosen ? chosen : json_object_get(icons, "default");
	}
	if (icons && icons->type == JSON_ARRAY && icons->items->length) {
		int size = icons->items->length;
		struct json_value *first = icons->items->items[0];
		if (first->type == JSON_OBJECT) {
			const char *last = NULL;
			for (int i = 0; i < size; ++i) {
				struct json_value *t = icons->items->items[i];
				const char *icon = json_get_string(t, "icon", NULL);
				struct json_value *limit = json_object_get(t, "max");
				if (!icon || !limit || limit->type != JSON_NUMBER) {
					continue;
				}
				last = icon;
				if (percentage <= limit->number) {
					return icon;
				}
			}
			return last ? last : "";
		}
		int divisor = (max == 0 ? 100 : max) / size;
		if (divisor < 1) {
			divisor = 1;
		}
		int idx = (percentage < 0 ? 0 : (int)percentage) / divisor;
		if (idx > size - 1) {
			idx = size - 1;
		}
		icons = icons->items->items[idx];
	}
	return icons && icons->type == JSON_STRING ? icons->string : "";
}

static const char *icon_for_percent(struct bar_module *m, double percent) {
	return module_icon(m, percent, NULL, 0);
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
			compute_item_style(&el, &view->style, item);
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
		item->tooltip = t->title && t->title[0] ? swine_strdup(t->title) : NULL;
		item->icon_size = icon_size;
		item->icon_path = entries[i].icon_path ?
			platform_expand_path(entries[i].icon_path) : NULL;
		struct css_element el = {"widget", NULL, classes, &box_el};
		compute_item_style(&el, &view->style, item);
		list_add(view->items, item);
	}
	free(entries);
	list_add(ctx->model->modules, view);
}

// Waybar's Custom::update.
static void build_custom(struct build_ctx *ctx, struct bar_module *m,
		int index) {
	if (!m->script) {
		if (!json_object_get(m->cfg, "exec") &&
				!json_object_get(m->cfg, "exec-if")) {
			struct fmt_arg args[] = {{NULL, false, "", 0}};
			add_label(ctx, m, index, format_waybar(module_format(m, NULL, NULL,
				"{}"), args, 1, &ctx->tm), NULL);
		}
		return;
	}
	struct bar_script_result r;
	bar_script_result(m->script, &r);
	if (r.hidden || (json_get_bool(m->cfg, "hide-empty-text", false) &&
			!r.text[0])) {
		bar_script_result_finish(&r);
		return;
	}
	struct fmt_arg args[] = {
		{NULL, false, r.text, 0},
		{"text", false, r.text, 0},
		{"alt", false, r.alt, 0},
		{"icon", false, module_icon(m, r.percentage,
			(const char *const[]){r.alt, NULL}, 0), 0},
		{"percentage", true, NULL, r.percentage},
	};
	const char *classes[MAX_CLASSES + 1];
	int n = 0;
	for (int i = 0; i < r.classes->length && n < MAX_CLASSES; ++i) {
		classes[n++] = r.classes->items[i];
	}
	classes[n] = NULL;
	char *label = format_waybar(module_format(m, NULL, NULL, "{}"), args, 5,
		&ctx->tm);
	char *label_copy = swine_strdup(label);
	struct bar_item *item = add_label(ctx, m, index, label, classes);
	// Waybar: tooltip-format (with {tooltip}), else the script's tooltip,
	// or the label when the script gave none.
	struct fmt_arg tip_args[] = {
		{NULL, false, r.text, 0},
		{"text", false, r.text, 0},
		{"tooltip", false, r.tooltip, 0},
		{"alt", false, r.alt, 0},
		{"icon", false, args[3].str, 0},
		{"percentage", true, NULL, r.percentage},
	};
	set_tooltip(ctx, m, item, NULL, NULL, tip_args, 6,
		strcmp(r.text, r.tooltip) == 0 ? label_copy : r.tooltip);
	free(label_copy);
	bar_script_result_finish(&r);
}

// The module's last reading, or NULL when it has to be read again: after
// "interval" seconds (cap_s: at most that long, for state that has no
// change notification here).
static void *cached_reading(struct bar_module *m, size_t size,
		double default_interval, double cap_s) {
	long long now = platform_now_ms();
	if (m->reading && now < m->next_read) {
		return m->reading;
	}
	double interval = json_get_number(m->cfg, "interval", default_interval);
	if (cap_s > 0 && interval > cap_s) {
		interval = cap_s;
	}
	if (interval < 0.001) {
		interval = 0.001;
	}
	m->next_read = now + (long long)(interval * 1000);
	if (!m->reading) {
		m->reading = calloc(1, size);
	}
	return NULL;
}

// Waybar's ALabel::getState applies "states" thresholds; the network module
// puts the state first: format-<state>-<threshold>, format-<state>, format.
static const char *state_format(struct bar_module *m, const char *state,
		const char *threshold, const char *fallback) {
	char key[96];
	if (m->alt) {
		const char *alt = json_get_string(m->cfg, "format-alt", NULL);
		if (alt) {
			return alt;
		}
	}
	if (state && threshold) {
		snprintf(key, sizeof(key), "format-%s-%s", state, threshold);
		const char *f = json_get_string(m->cfg, key, NULL);
		if (f) {
			return f;
		}
	}
	if (state) {
		snprintf(key, sizeof(key), "format-%s", state);
		const char *f = json_get_string(m->cfg, key, NULL);
		if (f) {
			return f;
		}
	}
	return json_get_string(m->cfg, "format", fallback);
}

// Waybar's network module.
static void build_network(struct build_ctx *ctx, struct bar_module *m,
		int index) {
	long long now = platform_now_ms();
	struct sysinfo_network *n = cached_reading(m, sizeof(*n), 60, 5);
	if (!n) {
		n = m->reading;
		const char *iface = json_get_string(m->cfg, "interface", NULL);
		struct sysinfo_network fresh = {0};
		if (!sysinfo_network(iface, &fresh)) {
			memset(&fresh, 0, sizeof(fresh));
		}
		*n = fresh;
	}
	// Bandwidth over each "interval" (Waybar samples on its timer).
	double interval = json_get_number(m->cfg, "interval", 60);
	if (interval < 0.001) {
		interval = 0.001;
	}
	double elapsed = (double)(now - m->sample_ms) / 1000.0;
	if (!m->sample_ms || elapsed >= interval * 0.5) {
		if (m->sample_ms && elapsed > 0 && n->rx_bytes >= m->rx_total &&
				n->tx_bytes >= m->tx_total) {
			m->rx_rate = (n->rx_bytes - m->rx_total) / elapsed;
			m->tx_rate = (n->tx_bytes - m->tx_total) / elapsed;
		} else {
			m->rx_rate = m->tx_rate = 0;
		}
		m->rx_total = n->rx_bytes;
		m->tx_total = n->tx_bytes;
		m->sample_ms = now ? now : 1;
	}

	const char *family = json_get_string(m->cfg, "family", "ipv4");
	const char *state;
	if (!n->found || !n->connected) {
		state = n->disabled && json_get_bool(m->cfg, "rfkill", true) &&
			(!n->ifname[0] || n->wireless) ? "disabled" : "disconnected";
	} else if (!n->ipaddr[0] && !n->ipaddr6[0]) {
		state = "linked";
	} else if (!n->essid[0] && !n->wireless) {
		// Waybar tells Wi-Fi by its network name; Windows may hide the name
		// (location access), so a wireless interface is Wi-Fi regardless.
		state = "ethernet";
	} else {
		state = "wifi";
	}
	const char *threshold = module_state(m, n->signal_strength, false);
	char ipaddr[160];
	if (strcmp(family, "ipv6") == 0) {
		snprintf(ipaddr, sizeof(ipaddr), "%s", n->ipaddr6);
	} else if (strcmp(family, "ipv4_6") == 0) {
		snprintf(ipaddr, sizeof(ipaddr), "%s\n%s", n->ipaddr, n->ipaddr6);
	} else {
		snprintf(ipaddr, sizeof(ipaddr), "%s", n->ipaddr);
	}
	const char *app = n->signal_dbm >= -50 ? "Great Connectivity" :
		n->signal_dbm >= -60 ? "Good Connectivity" :
		n->signal_dbm >= -67 ? "Streaming" :
		n->signal_dbm >= -70 ? "Web Surfing" :
		n->signal_dbm >= -80 ? "Basic Connectivity" : "Poor Connectivity";
	char frequency[32];
	snprintf(frequency, sizeof(frequency), "%.1f", n->frequency);
	double down = m->rx_rate, up = m->tx_rate;
	struct fmt_arg args[] = {
		{"essid", FMT_STRING, n->essid, 0},
		{"bssid", FMT_STRING, n->bssid, 0},
		{"signaldBm", FMT_NUMBER, NULL, n->signal_dbm},
		{"signalStrength", FMT_NUMBER, NULL, n->signal_strength},
		{"signalStrengthApp", FMT_STRING, n->essid[0] ? app : "", 0},
		{"ifname", FMT_STRING, n->ifname, 0},
		{"netmask", FMT_STRING, n->netmask, 0},
		{"netmask6", FMT_STRING, n->netmask6, 0},
		{"ipaddr", FMT_STRING, ipaddr, 0},
		{"gwaddr", FMT_STRING, n->gwaddr, 0},
		{"cidr", FMT_NUMBER, NULL, n->cidr},
		{"cidr6", FMT_NUMBER, NULL, n->cidr6},
		{"frequency", FMT_STRING, frequency, 0},
		{"icon", FMT_STRING, module_icon(m, n->signal_strength,
			(const char *const[]){state, NULL}, 0), 0},
		{"bandwidthDownBits", FMT_POW, "b/s", down * 8},
		{"bandwidthUpBits", FMT_POW, "b/s", up * 8},
		{"bandwidthTotalBits", FMT_POW, "b/s", (up + down) * 8},
		{"bandwidthDownOctets", FMT_POW, "o/s", down},
		{"bandwidthUpOctets", FMT_POW, "o/s", up},
		{"bandwidthTotalOctets", FMT_POW, "o/s", up + down},
		{"bandwidthDownBytes", FMT_POW, "B/s", down},
		{"bandwidthUpBytes", FMT_POW, "B/s", up},
		{"bandwidthTotalBytes", FMT_POW, "B/s", up + down},
		{"bandwidthDownBytesCompact", FMT_POW_COMPACT, "B", down},
		{"bandwidthUpBytesCompact", FMT_POW_COMPACT, "B", up},
		{"rxBitrate", FMT_POW, "b/s", n->rx_bitrate},
		{"txBitrate", FMT_POW, "b/s", n->tx_bitrate},
		{"linkSpeed", FMT_POW_SKIP_DECIMAL, "b/s", n->link_speed},
	};
	const char *classes[] = {state, threshold, NULL};
	int nargs = (int)(sizeof(args) / sizeof(args[0]));
	char *label = format_waybar(state_format(m, state, threshold, "{ifname}"),
		args, nargs, &ctx->tm);
	char *label_copy = swine_strdup(label);
	struct bar_item *item = add_label(ctx, m, index, label, classes);
	// tooltip-format-<state>-<threshold> first, as for the label
	const char *tip_state = state;
	char combined[96];
	if (threshold) {
		snprintf(combined, sizeof(combined), "tooltip-format-%s-%s", state,
			threshold);
		if (json_object_get(m->cfg, combined)) {
			snprintf(combined, sizeof(combined), "%s-%s", state, threshold);
			tip_state = combined;
		}
	}
	set_tooltip(ctx, m, item, tip_state, NULL, args, nargs, label_copy);
	free(label_copy);
}

// Waybar's pulseaudio module (and wireplumber, which reads the same).
static void build_pulseaudio(struct build_ctx *ctx, struct bar_module *m,
		int index) {
	struct sysinfo_audio *a = cached_reading(m, sizeof(*a), 1, 1);
	if (!a) {
		a = m->reading;
		struct sysinfo_audio fresh = {0};
		if (!sysinfo_audio(&fresh)) {
			memset(&fresh, 0, sizeof(fresh));
		}
		*a = fresh;
	}
	if (!a->present) {
		return;
	}
	char format_name[48] = "format";
	if (a->bluetooth) {
		strcat(format_name, "-bluetooth");
	}
	if (a->muted) {
		char key[64];
		snprintf(key, sizeof(key), "%s-muted", format_name);
		if (strcmp(format_name, "format") != 0 &&
				!json_get_string(m->cfg, key, NULL)) {
			strcpy(format_name, "format");
		}
		strcat(format_name, "-muted");
	}
	const char *state = module_state(m, a->volume, false);
	const char *format = NULL;
	if (m->alt) {
		format = json_get_string(m->cfg, "format-alt", NULL);
	}
	if (!format && state) {
		char key[96];
		snprintf(key, sizeof(key), "%s-%s", format_name, state);
		format = json_get_string(m->cfg, key, NULL);
	}
	if (!format) {
		format = json_get_string(m->cfg, format_name, NULL);
	}
	bool wireplumber = has_prefix(m->name, "wireplumber");
	if (!format) {
		format = json_get_string(m->cfg, "format",
			wireplumber ? "{volume}%" : "{volume}%");
	}
	const char *source_format = a->source_muted ?
		json_get_string(m->cfg, "format-source-muted", "{volume}%") :
		json_get_string(m->cfg, "format-source", "{volume}%");
	struct fmt_arg source_args[] = {
		{"volume", FMT_NUMBER, NULL, a->source_volume},
		{"source_desc", FMT_STRING, a->source_desc, 0},
	};
	char *format_source = format_waybar(source_format, source_args, 2, NULL);
	char port_muted[48];
	snprintf(port_muted, sizeof(port_muted), "%s-muted", a->port);
	const char *alts_muted[] = {port_muted, a->port, "default-muted", NULL};
	const char *alts[] = {a->port, NULL};
	struct fmt_arg args[] = {
		{"desc", FMT_STRING, a->desc, 0},
		{"volume", FMT_NUMBER, NULL, a->volume},
		{"format_source", FMT_STRING, format_source, 0},
		{"source_volume", FMT_NUMBER, NULL, a->source_volume},
		{"source_desc", FMT_STRING, a->source_desc, 0},
		{"icon", FMT_STRING, module_icon(m, a->volume,
			a->muted ? alts_muted : alts, 0), 0},
		// wireplumber's names
		{"node_name", FMT_STRING, a->desc, 0},
		{"source_node_name", FMT_STRING, a->source_desc, 0},
	};
	const char *classes[6];
	int c = 0;
	if (a->bluetooth) classes[c++] = "bluetooth";
	if (a->muted) {
		classes[c++] = "muted";
		classes[c++] = "sink-muted";
	}
	if (a->source_muted) classes[c++] = "source-muted";
	if (state) classes[c++] = state;
	classes[c] = NULL;
	int nargs = (int)(sizeof(args) / sizeof(args[0]));
	struct bar_item *item = add_label(ctx, m, index,
		format_waybar(format, args, nargs, &ctx->tm), classes);
	set_tooltip(ctx, m, item, NULL, NULL, args, nargs, a->desc);
	free(format_source);
}

// Changes the volume as Waybar's pulseaudio module does on a scroll.
static void pulseaudio_scroll(struct bar_module *m, bool up) {
	struct sysinfo_audio a = {0};
	if (!sysinfo_audio(&a)) {
		return;
	}
	bool source = strcmp(json_get_string(m->cfg, "target", "sink"),
		"source") == 0;
	if (json_get_bool(m->cfg, "reverse-scrolling", false)) {
		up = !up;
	}
	double step = json_get_number(m->cfg, "scroll-step", 1);
	double max = json_get_number(m->cfg, "max-volume", 100);
	double volume = (source ? a.source_volume : a.volume) + (up ? step : -step);
	if (volume > max) volume = max;
	if (volume < 0) volume = 0;
	sysinfo_audio_set_volume(source, volume);
	free(m->reading);
	m->reading = NULL;   // read it again now
}

// "path": "/" means the Windows drive, as / is the root on Linux.
static char *disk_path(const char *path) {
	if (!path || strcmp(path, "/") == 0) {
		return platform_expand_path("%SystemDrive%\\");
	}
	return platform_expand_path(path);
}

static double disk_divisor(const char *unit) {
	static const struct { const char *unit; double divisor; } units[] = {
		{"kB", 1e3}, {"kiB", 1024.0}, {"MB", 1e6}, {"MiB", 1048576.0},
		{"GB", 1e9}, {"GiB", 1073741824.0}, {"TB", 1e12},
		{"TiB", 1099511627776.0},
	};
	for (size_t i = 0; unit && i < sizeof(units) / sizeof(units[0]); ++i) {
		if (strcmp(unit, units[i].unit) == 0) {
			return units[i].divisor;
		}
	}
	return 1.0;
}

// One path of Waybar's disk module. Returns NULL if it cannot be read.
static char *disk_text(struct bar_module *m, const char *path,
		const char *format, const char **state_out, struct tm *tm,
		char **tooltip) {
	char *real = disk_path(path);
	struct sysinfo_disk d = {0};
	bool ok = real && sysinfo_disk(real, &d) && d.total > 0;
	free(real);
	if (!ok) {
		return NULL;
	}
	double used = d.total - d.free;
	int percentage_used = (int)(used * 100 / d.total);
	int percentage_free = (int)(d.avail * 100 / d.total);
	const char *state = module_state(m, percentage_used, false);
	if (state_out) {
		*state_out = state;
	}
	double divisor = disk_divisor(json_get_string(m->cfg, "unit", NULL));
	struct fmt_arg args[] = {
		{NULL, FMT_NUMBER, NULL, percentage_used},
		{"percentage_used", FMT_NUMBER, NULL, percentage_used},
		{"percentage_free", FMT_NUMBER, NULL, percentage_free},
		{"free", FMT_POW_BINARY, "B", d.avail},
		{"used", FMT_POW_BINARY, "B", used},
		{"total", FMT_POW_BINARY, "B", d.total},
		{"path", FMT_STRING, path, 0},
		{"specific_free", FMT_NUMBER, NULL, d.avail / divisor},
		{"specific_used", FMT_NUMBER, NULL, used / divisor},
		{"specific_total", FMT_NUMBER, NULL, d.total / divisor},
	};
	if (state) {
		char key[64];
		snprintf(key, sizeof(key), "format-%s", state);
		format = json_get_string(m->cfg, key, format);
	}
	int nargs = (int)(sizeof(args) / sizeof(args[0]));
	if (tooltip) {
		*tooltip = format_waybar(json_get_string(m->cfg, "tooltip-format",
			"{used} used out of {total} on {path} ({percentage_used}%)"),
			args, nargs, tm);
	}
	return format_waybar(format, args, nargs, tm);
}

// Waybar's disk module: "path", or "paths" joined by "separator" after
// "header". Drives are read on every build; Windows caches them.
static void build_disk(struct build_ctx *ctx, struct bar_module *m,
		int index) {
	const char *format = module_format(m, NULL, NULL, "{}%");
	struct json_value *paths = json_object_get(m->cfg, "paths");
	const char *state = NULL;
	if (!paths || paths->type != JSON_ARRAY || !paths->items->length) {
		char *tooltip = NULL;
		char *text = disk_text(m, json_get_string(m->cfg, "path", "/"), format,
			&state, &ctx->tm, &tooltip);
		const char *classes[] = {state, NULL};
		struct bar_item *item = add_label(ctx, m, index, text, classes);
		if (item && tooltip && json_get_bool(m->cfg, "tooltip", true)) {
			item->tooltip = tooltip_text(tooltip);
		}
		free(tooltip);
		return;
	}
	char *tooltips = swine_strdup("");
	const char *separator = json_get_string(m->cfg, "separator", " ");
	const char *header = json_get_string(m->cfg, "header", "");
	size_t len = strlen(header), cap = len + 64;
	char *all = malloc(cap);
	memcpy(all, header, len + 1);
	bool first = true;
	for (int i = 0; i < paths->items->length; ++i) {
		struct json_value *p = paths->items->items[i];
		if (p->type != JSON_STRING) {
			continue;
		}
		char *tooltip = NULL;
		char *text = disk_text(m, p->string, format, NULL, &ctx->tm, &tooltip);
		if (!text) {
			continue;
		}
		size_t tlen = strlen(tooltips);
		tooltips = realloc(tooltips, tlen + strlen(tooltip) + 2);
		if (tlen) {
			strcat(tooltips, "\n");
		}
		strcat(tooltips, tooltip);
		free(tooltip);
		size_t need = len + strlen(separator) + strlen(text) + 1;
		if (need > cap) {
			cap = need * 2;
			all = realloc(all, cap);
		}
		if (!first) {
			strcat(all, separator);
		}
		strcat(all, text);
		len = strlen(all);
		first = false;
		free(text);
	}
	struct bar_item *item = add_label(ctx, m, index, all, NULL);
	if (item && json_get_bool(m->cfg, "tooltip", true)) {
		item->tooltip = tooltip_text(tooltips);
	}
	free(tooltips);
}

// Waybar's temperature module.
static void build_temperature(struct build_ctx *ctx, struct bar_module *m,
		int index) {
	double *celsius = cached_reading(m, sizeof(double) * 2, 10, 0);
	if (!celsius) {
		celsius = m->reading;
		// [0]: the reading, [1]: 1 if there is one
		int zone = (int)json_get_number(m->cfg, "thermal-zone", 0);
		celsius[1] = sysinfo_temperature(zone, &celsius[0]) ? 1 : 0;
	}
	if (!celsius[1]) {
		return;
	}
	int c = (int)lround(celsius[0]);
	int f = (int)lround(celsius[0] * 1.8 + 32);
	int k = (int)lround(celsius[0] + 273.15);
	struct json_value *crit = json_object_get(m->cfg, "critical-threshold");
	struct json_value *warn = json_object_get(m->cfg, "warning-threshold");
	bool critical = crit && crit->type == JSON_NUMBER && c >= crit->number;
	bool warning = warn && warn->type == JSON_NUMBER && c >= warn->number;
	const char *format = json_get_string(m->cfg, "format", "{temperatureC}°C");
	const char *cls = NULL;
	if (critical) {
		format = json_get_string(m->cfg, "format-critical", format);
		cls = "critical";
	} else if (warning) {
		format = json_get_string(m->cfg, "format-warning", format);
		cls = "warning";
	}
	if (m->alt) {
		format = json_get_string(m->cfg, "format-alt", format);
	}
	int max_temp = crit && crit->type == JSON_NUMBER ? (int)crit->number : 0;
	struct fmt_arg args[] = {
		{"temperatureC", FMT_NUMBER, NULL, c},
		{"temperatureF", FMT_NUMBER, NULL, f},
		{"temperatureK", FMT_NUMBER, NULL, k},
		{"icon", FMT_STRING, module_icon(m, c, NULL, max_temp), 0},
	};
	const char *classes[] = {cls, NULL};
	struct bar_item *item = add_label(ctx, m, index,
		format_waybar(format, args, 4, &ctx->tm), classes);
	set_tooltip(ctx, m, item, NULL, "{temperatureC}°C", args, 4, NULL);
}

static void idle_inhibit_set(bool active, struct bar_module *m) {
	idle_inhibit.active = active;
	idle_inhibit.deadline = 0;
	double timeout = json_get_number(m->cfg, "timeout", 0);
	if (active && timeout > 0) {
		idle_inhibit.deadline = platform_now_ms() + (long long)(timeout * 60000);
	}
	sysinfo_inhibit_idle(active);
}

// Waybar's idle_inhibitor: a click switches it.
static void build_idle_inhibitor(struct build_ctx *ctx, struct bar_module *m,
		int index) {
	long long now = platform_now_ms();
	if (idle_inhibit.active && idle_inhibit.deadline &&
			now >= idle_inhibit.deadline) {
		idle_inhibit_set(false, m);
	}
	const char *status = idle_inhibit.active ? "activated" : "deactivated";
	double timeout = json_get_number(m->cfg, "timeout", 0);
	double timeleft = idle_inhibit.deadline ?
		(double)(idle_inhibit.deadline - now) / 60000.0 : timeout;
	struct fmt_arg args[] = {
		{"status", FMT_STRING, status, 0},
		{"timeout", FMT_NUMBER, NULL, timeout},
		{"timeleft", FMT_NUMBER, NULL, timeleft < 0 ? 0 : timeleft},
		{"icon", FMT_STRING, module_icon(m, 0,
			(const char *const[]){status, NULL}, 0), 0},
	};
	const char *classes[] = {status, NULL};
	struct bar_item *item = add_label(ctx, m, index, format_waybar(
		module_format(m, NULL, status, "{status}"), args, 4, &ctx->tm), classes);
	set_tooltip(ctx, m, item, status, "{status}", args, 4, NULL);
}

// power-profiles-daemon's profiles, in its order (a click cycles them).
static const char *const power_profiles[] = {"power-saver", "balanced",
	"performance"};

struct power_reading {
	bool known;
	char profile[32];
	char driver[64];
};

// Waybar's power-profiles-daemon module: Windows' power mode.
static void build_power_profiles(struct build_ctx *ctx, struct bar_module *m,
		int index) {
	struct power_reading *r = cached_reading(m, sizeof(*r), 2, 2);
	if (!r) {
		r = m->reading;
		r->known = sysinfo_power_profile(r->profile, sizeof(r->profile),
			r->driver, sizeof(r->driver));
	}
	if (!r->known) {
		return;
	}
	const char *icon = module_icon(m, 0,
		(const char *const[]){r->profile, NULL}, 0);
	if (!json_object_get(m->cfg, "format-icons")) {
		// Waybar's default icons need Font Awesome, which Windows lacks.
		icon = strcmp(r->profile, "performance") == 0 ? "PERF" :
			strcmp(r->profile, "power-saver") == 0 ? "SAVE" : "BAL";
	}
	struct fmt_arg args[] = {
		{"profile", FMT_STRING, r->profile, 0},
		{"icon", FMT_STRING, icon, 0},
		{"driver", FMT_STRING, r->driver, 0},
		{"cpu_driver", FMT_STRING, r->driver, 0},
		{"platform_driver", FMT_STRING, r->driver, 0},
	};
	const char *classes[] = {r->profile, NULL};
	struct bar_item *item = add_label(ctx, m, index, format_waybar(
		module_format(m, NULL, NULL, "{icon}"), args, 5, &ctx->tm), classes);
	set_tooltip(ctx, m, item, NULL,
		"Power profile: {profile}\nDriver: {driver}", args, 5, NULL);
}

// The next (or previous) power profile, as Waybar's left (right) click.
static void power_profiles_cycle(struct bar_module *m, int step) {
	char profile[32], driver[64];
	if (!sysinfo_power_profile(profile, sizeof(profile), driver,
			sizeof(driver))) {
		return;
	}
	int n = (int)(sizeof(power_profiles) / sizeof(power_profiles[0]));
	int current = 1;
	for (int i = 0; i < n; ++i) {
		if (strcmp(profile, power_profiles[i]) == 0) {
			current = i;
		}
	}
	sysinfo_set_power_profile(power_profiles[(current + step + n) % n]);
	free(m->reading);
	m->reading = NULL;
}

// Waybar's keyboard-state: a label for each lock that is enabled.
static void build_keyboard_state(struct build_ctx *ctx, struct bar_module *m,
		int index) {
	bool locks[3];
	if (!sysinfo_keyboard_locks(&locks[1], &locks[0], &locks[2])) {
		return;
	}
	static const char *const keys[] = {"numlock", "capslock", "scrolllock"};
	static const char *const names[] = {"Num", "Caps", "Scroll"};
	struct bar_module_view *view = new_view(m, index);
	view->is_box = true;
	const char *box_classes[2] = {m->css_class, NULL};
	struct css_element box_el = {"box", m->css_id,
		m->css_class ? box_classes : NULL, &ctx->window_el};
	css_compute(bar.css, &box_el, &ctx->model->window, &view->style);
	struct json_value *format_cfg = json_object_get(m->cfg, "format");
	struct json_value *icons = json_object_get(m->cfg, "format-icons");
	bool common = icons && icons->type == JSON_OBJECT &&
		(json_object_get(icons, "locked") || json_object_get(icons, "unlocked"));
	for (int i = 0; i < 3; ++i) {
		if (!json_get_bool(m->cfg, keys[i], false)) {
			continue;
		}
		const char *format = format_cfg && format_cfg->type == JSON_STRING ?
			format_cfg->string : json_get_string(format_cfg, keys[i],
			"{name} {icon}");
		struct json_value *set = common ? icons : json_object_get(icons, keys[i]);
		const char *icon = locks[i] ? json_get_string(set, "locked", "locked") :
			json_get_string(set, "unlocked", "unlocked");
		struct fmt_arg args[] = {
			{"name", FMT_STRING, names[i], 0},
			{"icon", FMT_STRING, icon, 0},
		};
		char *text = format_waybar(format, args, 2, &ctx->tm);
		struct bar_item *item = calloc(1, sizeof(*item));
		item->text = markup_strip(text);
		free(text);
		const char *classes[] = {keys[i], locks[i] ? "locked" : NULL, NULL};
		struct css_element el = {"label", NULL, classes, &box_el};
		compute_item_style(&el, &view->style, item);
		list_add(view->items, item);
	}
	if (!view->items->length) {
		list_free(view->items);
		free(view);
		return;
	}
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
	char *label = format_waybar(module_format(m, NULL, NULL, "{}"), args, 6,
		&ctx->tm);
	char *label_copy = swine_strdup(label);
	struct bar_item *item = add_label(ctx, m, index, label, classes);
	set_tooltip(ctx, m, item, NULL, NULL, args, 6, label_copy);
	free(label_copy);
}

// Waybar's {calendar}: the month of tm, weeks from Monday.
static char *month_calendar(const struct tm *tm) {
	static const int days_in[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	int year = tm->tm_year + 1900;
	int days = days_in[tm->tm_mon];
	if (tm->tm_mon == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
		days = 29;
	}
	// Weekday of the 1st (Monday 0), from today's.
	int first = ((tm->tm_wday + 6) % 7 - (tm->tm_mday - 1) % 7 + 7) % 7;
	char *out = malloc(8 * 21 + 1);
	char *o = out;
	o += sprintf(o, "Mo Tu We Th Fr Sa Su");
	int column = 0;
	for (int i = 0; i < first; ++i, ++column) {
		o += sprintf(o, "%s  ", column == 0 ? "\n" : " ");
	}
	for (int day = 1; day <= days; ++day, ++column) {
		if (column == 7) {
			column = 0;
		}
		o += sprintf(o, "%s%2d", column == 0 ? "\n" : " ", day);
	}
	return out;
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
		struct bar_item *item = add_label(ctx, m, index, format_waybar(
			module_format(m, NULL, NULL, "{}"), args, 1, &ctx->tm), NULL);
		set_tooltip(ctx, m, item, NULL, NULL, args, 1, mode);
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
		struct bar_item *item = add_label(ctx, m, index, format_waybar(
			module_format(m, NULL, NULL, "{}"), args, 3, &ctx->tm), NULL);
		// The whole title, which max-length may have cut.
		set_tooltip(ctx, m, item, NULL, NULL, args, 3, view->title);
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
		struct bar_item *item = add_label(ctx, m, index, format_waybar(
			module_format(m, NULL, NULL, "{icon} {count}"), args, 2, &ctx->tm),
			classes);
		if (item && json_get_bool(m->cfg, "tooltip", true)) {
			// Waybar: one line per window, "{app}: {title}"
			const char *line = json_get_string(m->cfg, "tooltip-format",
				"{app}: {title}");
			size_t len = 0;
			char *all = swine_strdup("");
			for (int i = 0; i < count; ++i) {
				struct swine_container *con = root->scratchpad->items[i];
				if (!con->view) {
					continue;
				}
				struct fmt_arg wargs[] = {
					{"app", false, con->view->app_id, 0},
					{"title", false, con->view->title, 0},
				};
				char *text = format_waybar(line, wargs, 2, &ctx->tm);
				all = realloc(all, len + strlen(text) + 2);
				if (len) {
					all[len++] = '\n';
				}
				strcpy(all + len, text);
				len += strlen(text);
				free(text);
			}
			item->tooltip = tooltip_text(all);
			free(all);
		}
		return;
	}
	case MOD_CLOCK: {
		struct bar_item *item = add_label(ctx, m, index, format_waybar(
			module_format(m, NULL, NULL, "{:%H:%M}"), NULL, 0, &ctx->tm), NULL);
		// Waybar's clock has a tooltip only with tooltip-format.
		if (item && json_object_get(m->cfg, "tooltip-format")) {
			char *calendar = month_calendar(&ctx->tm);
			struct fmt_arg args[] = {{"calendar", false, calendar, 0}};
			set_tooltip(ctx, m, item, NULL, NULL, args, 1, NULL);
			free(calendar);
		}
		return;
	}
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
		struct bar_item *item = add_label(ctx, m, index, format_waybar(
			module_format(m, state, NULL, "{usage}%"), args, 2, &ctx->tm), classes);
		set_tooltip(ctx, m, item, state, "{usage}%", args, 2, NULL);
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
		struct bar_item *item = add_label(ctx, m, index, format_waybar(
			module_format(m, state, NULL, "{percentage}%"), args, 5, &ctx->tm),
			classes);
		set_tooltip(ctx, m, item, state, "{used:0.1f}GiB used", args, 5, NULL);
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
		// Waybar's format-time, "{H} h {M} min"
		char time_text[64] = "";
		if (bat.seconds_left > 0) {
			struct fmt_arg targs[] = {
				{"H", true, NULL, bat.seconds_left / 3600},
				{"M", true, NULL, (bat.seconds_left / 60) % 60},
				{"m", true, NULL, (bat.seconds_left / 60) % 60},
			};
			char *t = format_waybar(json_get_string(m->cfg, "format-time",
				"{H} h {M} min"), targs, 3, NULL);
			snprintf(time_text, sizeof(time_text), "%s", t);
			free(t);
		}
		static const char *const pretty[][2] = {
			{"charging", "Charging"}, {"discharging", "Discharging"},
			{"full", "Full"}, {"plugged", "Not charging"},
		};
		const char *time_to = status;
		for (size_t i = 0; i < sizeof(pretty) / sizeof(pretty[0]); ++i) {
			if (strcmp(status, pretty[i][0]) == 0) {
				time_to = pretty[i][1];
			}
		}
		char empty_in[96];
		if (time_text[0] && strcmp(status, "discharging") == 0) {
			snprintf(empty_in, sizeof(empty_in), "Empty in %s", time_text);
			time_to = empty_in;
		}
		struct fmt_arg args[] = {
			{"capacity", true, NULL, bat.capacity},
			{"icon", false, icon_for_percent(m, bat.capacity), 0},
			{"time", false, time_text, 0},
			{"timeTo", false, time_to, 0},
		};
		const char *classes[] = {status, state, NULL};
		struct bar_item *item = add_label(ctx, m, index, format_waybar(
			module_format(m, state, status, "{capacity}%"), args, 4, &ctx->tm),
			classes);
		set_tooltip(ctx, m, item, status, "{timeTo}", args, 4, NULL);
		return;
	}
	case MOD_TRAY:
		build_tray(ctx, m, index);
		return;
	case MOD_LANGUAGE:
		build_language(ctx, m, index);
		return;
	case MOD_NETWORK:
		build_network(ctx, m, index);
		return;
	case MOD_PULSEAUDIO:
		build_pulseaudio(ctx, m, index);
		return;
	case MOD_DISK:
		build_disk(ctx, m, index);
		return;
	case MOD_TEMPERATURE:
		build_temperature(ctx, m, index);
		return;
	case MOD_IDLE_INHIBITOR:
		build_idle_inhibitor(ctx, m, index);
		return;
	case MOD_KEYBOARD_STATE:
		build_keyboard_state(ctx, m, index);
		return;
	case MOD_POWER_PROFILES:
		build_power_profiles(ctx, m, index);
		return;
	case MOD_CUSTOM:
		build_custom(ctx, m, index);
		return;
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
			free(item->tooltip);
			free(item->on_click);
			free(item->on_click_middle);
			free(item->on_click_right);
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
	if (m->kind == MOD_IDLE_INHIBITOR) {
		idle_inhibit_set(!idle_inhibit.active, m);
	}
	if (m->kind == MOD_POWER_PROFILES) {
		power_profiles_cycle(m, 1);
	}
	bar_script_event(m->script);
}

void bar_module_right_clicked(int module_index) {
	if (!bar.modules || module_index < 0 || module_index >= bar.modules->length) {
		return;
	}
	struct bar_module *m = bar.modules->items[module_index];
	if (m->kind == MOD_POWER_PROFILES) {
		power_profiles_cycle(m, -1);
	}
	bar_script_event(m->script);
}

void bar_refresh_readings(void) {
	for (int i = 0; bar.modules && i < bar.modules->length; ++i) {
		struct bar_module *m = bar.modules->items[i];
		if (m->kind != MOD_NETWORK) {   // keeps its bandwidth sample
			free(m->reading);
			m->reading = NULL;
		}
	}
}

bool bar_module_scroll(int module_index, bool up) {
	if (!bar.modules || module_index < 0 || module_index >= bar.modules->length) {
		return false;
	}
	struct bar_module *m = bar.modules->items[module_index];
	bar_script_event(m->script);
	if (m->kind == MOD_PULSEAUDIO &&
			!json_object_get(m->cfg, "on-scroll-up") &&
			!json_object_get(m->cfg, "on-scroll-down")) {
		pulseaudio_scroll(m, up);
		return true;
	}
	return false;
}

void bar_module_event(int module_index) {
	if (!bar.modules || module_index < 0 || module_index >= bar.modules->length) {
		return;
	}
	struct bar_module *m = bar.modules->items[module_index];
	bar_script_event(m->script);
}
