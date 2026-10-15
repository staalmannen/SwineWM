#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "list.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/bar/css.h"

struct css_compound {
	char *type;        // NULL or "*" matches any
	char *id;
	list_t *classes;   // char *
};

struct css_selector {
	list_t *compounds;  // struct css_compound *, left to right
	bool never;         // uses something we don't support
	int specificity;
};

struct css_declaration {
	char *property;
	char *value;
};

struct css_rule {
	struct css_selector *selector;
	list_t *declarations;  // shared between selectors of one rule; owned by
	bool owns_declarations; // the first selector's rule
	int order;
};

struct css_color_def {
	char *name;
	char *value;
};

struct css_stylesheet {
	list_t *rules;     // struct css_rule *
	list_t *colors;    // struct css_color_def *
};

static void free_compound(struct css_compound *c) {
	free(c->type);
	free(c->id);
	list_free_items_and_destroy(c->classes);
	free(c);
}

static void free_declarations(list_t *decls) {
	for (int i = 0; i < decls->length; ++i) {
		struct css_declaration *d = decls->items[i];
		free(d->property);
		free(d->value);
		free(d);
	}
	list_free(decls);
}

void css_free(struct css_stylesheet *sheet) {
	if (!sheet) {
		return;
	}
	for (int i = 0; i < sheet->rules->length; ++i) {
		struct css_rule *rule = sheet->rules->items[i];
		for (int j = 0; j < rule->selector->compounds->length; ++j) {
			free_compound(rule->selector->compounds->items[j]);
		}
		list_free(rule->selector->compounds);
		free(rule->selector);
		if (rule->owns_declarations) {
			free_declarations(rule->declarations);
		}
		free(rule);
	}
	list_free(sheet->rules);
	for (int i = 0; i < sheet->colors->length; ++i) {
		struct css_color_def *c = sheet->colors->items[i];
		free(c->name);
		free(c->value);
		free(c);
	}
	list_free(sheet->colors);
	free(sheet);
}

static bool is_ident_char(char c) {
	return isalnum((unsigned char)c) || c == '-' || c == '_' ||
		(unsigned char)c >= 0x80;
}

static char *read_ident(const char **p) {
	const char *start = *p;
	while (is_ident_char(**p)) {
		(*p)++;
	}
	return swine_strndup(start, (size_t)(*p - start));
}

// Parses one selector such as "window#waybar > box #workspaces button.focused".
static struct css_selector *parse_selector(const char *text) {
	struct css_selector *sel = calloc(1, sizeof(*sel));
	sel->compounds = create_list();
	const char *p = text;
	while (*p) {
		while (isspace((unsigned char)*p) || *p == '>') {
			p++;
		}
		if (!*p) {
			break;
		}
		if (*p == '+' || *p == '~') {
			sel->never = true;
			p++;
			continue;
		}
		struct css_compound *c = calloc(1, sizeof(*c));
		c->classes = create_list();
		if (*p == '*') {
			p++;
		} else if (is_ident_char(*p)) {
			c->type = read_ident(&p);
			sel->specificity += 1;
		}
		while (*p == '#' || *p == '.' || *p == ':' || *p == '[') {
			char kind = *p++;
			if (kind == '[') {
				// attribute selectors: unsupported
				while (*p && *p != ']') p++;
				if (*p) p++;
				sel->never = true;
				continue;
			}
			if (kind == ':' && *p == ':') {
				p++;
			}
			char *name = read_ident(&p);
			if (kind == '#') {
				free(c->id);
				c->id = name;
				sel->specificity += 100;
			} else if (kind == '.') {
				list_add(c->classes, name);
				sel->specificity += 10;
			} else {
				// :hover, :first-child, ... never match in a static bar
				free(name);
				if (*p == '(') {
					int depth = 0;
					do {
						if (*p == '(') depth++;
						if (*p == ')') depth--;
						p++;
					} while (*p && depth > 0);
				}
				sel->never = true;
			}
		}
		if (!c->type && !c->id && !c->classes->length && !sel->never &&
				p > text && p[-1] != '*') {
			// Not a valid compound; skip a character to make progress.
			free_compound(c);
			if (*p) p++;
			sel->never = true;
			continue;
		}
		list_add(sel->compounds, c);
	}
	return sel;
}

static void strip_comments(char *text) {
	char *r = text, *w = text;
	while (*r) {
		if (r[0] == '/' && r[1] == '*') {
			char *end = strstr(r + 2, "*/");
			r = end ? end + 2 : r + strlen(r);
			continue;
		}
		*w++ = *r++;
	}
	*w = '\0';
}

static list_t *parse_declarations(const char *block) {
	list_t *decls = create_list();
	list_t *parts = split_string(block, ";");
	for (int i = 0; i < parts->length; ++i) {
		char *part = parts->items[i];
		char *colon = strchr(part, ':');
		if (!colon) {
			continue;
		}
		*colon = '\0';
		struct css_declaration *d = calloc(1, sizeof(*d));
		d->property = swine_strdup(part);
		d->value = swine_strdup(colon + 1);
		strip_whitespace(d->property);
		strip_whitespace(d->value);
		char *important = strstr(d->value, "!important");
		if (important) {
			*important = '\0';
			strip_whitespace(d->value);
		}
		for (char *c = d->property; *c; ++c) {
			*c = (char)tolower((unsigned char)*c);
		}
		list_add(decls, d);
	}
	list_free_items_and_destroy(parts);
	return decls;
}

// Skips a { ... } block including nested braces; *p points at '{'.
static const char *skip_block(const char *p) {
	int depth = 0;
	do {
		if (*p == '{') depth++;
		if (*p == '}') depth--;
		p++;
	} while (*p && depth > 0);
	return p;
}

struct css_stylesheet *css_parse(const char *input) {
	struct css_stylesheet *sheet = calloc(1, sizeof(*sheet));
	sheet->rules = create_list();
	sheet->colors = create_list();
	char *text = swine_strdup(input);
	strip_comments(text);
	const char *p = text;
	int order = 0;
	while (*p) {
		while (isspace((unsigned char)*p)) {
			p++;
		}
		if (!*p) {
			break;
		}
		if (*p == '@') {
			if (strncmp(p, "@define-color", 13) == 0) {
				p += 13;
				while (isspace((unsigned char)*p)) p++;
				char *name = read_ident(&p);
				const char *end = strchr(p, ';');
				if (!end) {
					free(name);
					break;
				}
				struct css_color_def *def = calloc(1, sizeof(*def));
				def->name = name;
				def->value = swine_strndup(p, (size_t)(end - p));
				strip_whitespace(def->value);
				list_add(sheet->colors, def);
				p = end + 1;
				continue;
			}
			// @import, @keyframes, @media, ...: skip statement or block
			const char *semi = strchr(p, ';');
			const char *brace = strchr(p, '{');
			if (brace && (!semi || brace < semi)) {
				p = skip_block(brace);
			} else {
				p = semi ? semi + 1 : p + strlen(p);
			}
			continue;
		}
		const char *brace = strchr(p, '{');
		if (!brace) {
			break;
		}
		const char *close = strchr(brace, '}');
		if (!close) {
			break;
		}
		char *selectors = swine_strndup(p, (size_t)(brace - p));
		char *block = swine_strndup(brace + 1, (size_t)(close - brace - 1));
		list_t *decls = parse_declarations(block);
		list_t *sel_texts = split_string(selectors, ",");
		bool owner = true;
		for (int i = 0; i < sel_texts->length; ++i) {
			struct css_rule *rule = calloc(1, sizeof(*rule));
			rule->selector = parse_selector(sel_texts->items[i]);
			rule->declarations = decls;
			rule->owns_declarations = owner;
			rule->order = order++;
			owner = false;
			list_add(sheet->rules, rule);
		}
		if (owner) {
			free_declarations(decls);
		}
		list_free_items_and_destroy(sel_texts);
		free(selectors);
		free(block);
		p = close + 1;
	}
	free(text);
	return sheet;
}

static bool element_has_class(const struct css_element *el, const char *cls) {
	if (!el->classes) {
		return false;
	}
	for (const char *const *c = el->classes; *c; ++c) {
		if (strcmp(*c, cls) == 0) {
			return true;
		}
	}
	return false;
}

static bool compound_matches(const struct css_compound *c,
		const struct css_element *el) {
	if (c->type && strcmp(c->type, el->type ? el->type : "") != 0) {
		return false;
	}
	if (c->id && (!el->id || strcmp(c->id, el->id) != 0)) {
		return false;
	}
	for (int i = 0; i < c->classes->length; ++i) {
		if (!element_has_class(el, c->classes->items[i])) {
			return false;
		}
	}
	return true;
}

static bool selector_matches(const struct css_selector *sel,
		const struct css_element *el) {
	if (sel->never || !sel->compounds->length) {
		return false;
	}
	int i = sel->compounds->length - 1;
	if (!compound_matches(sel->compounds->items[i], el)) {
		return false;
	}
	const struct css_element *anc = el->parent;
	for (--i; i >= 0; --i) {
		while (anc && !compound_matches(sel->compounds->items[i], anc)) {
			anc = anc->parent;
		}
		if (!anc) {
			return false;
		}
		anc = anc->parent;
	}
	return true;
}

static const char *lookup_color(const struct css_stylesheet *sheet,
		const char *name) {
	if (!sheet) {
		return NULL;
	}
	for (int i = sheet->colors->length - 1; i >= 0; --i) {
		struct css_color_def *def = sheet->colors->items[i];
		if (strcmp(def->name, name) == 0) {
			return def->value;
		}
	}
	return NULL;
}

static int hexval(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	c = (char)tolower((unsigned char)c);
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

static bool parse_color_depth(const struct css_stylesheet *sheet,
		const char *text, struct swine_color *out, int depth) {
	if (depth > 8) {
		return false;
	}
	while (isspace((unsigned char)*text)) text++;
	if (*text == '@') {
		char name[64];
		size_t n = 0;
		text++;
		while (is_ident_char(*text) && n < sizeof(name) - 1) {
			name[n++] = *text++;
		}
		name[n] = '\0';
		const char *value = lookup_color(sheet, name);
		return value && parse_color_depth(sheet, value, out, depth + 1);
	}
	if (*text == '#') {
		const char *h = text + 1;
		size_t len = 0;
		while (hexval(h[len]) >= 0) len++;
		int v[8];
		for (size_t i = 0; i < len && i < 8; ++i) v[i] = hexval(h[i]);
		if (len == 3 || len == 4) {
			out->r = (uint8_t)(v[0] * 17);
			out->g = (uint8_t)(v[1] * 17);
			out->b = (uint8_t)(v[2] * 17);
			out->a = len == 4 ? (uint8_t)(v[3] * 17) : 255;
			return true;
		}
		if (len == 6 || len == 8) {
			out->r = (uint8_t)(v[0] * 16 + v[1]);
			out->g = (uint8_t)(v[2] * 16 + v[3]);
			out->b = (uint8_t)(v[4] * 16 + v[5]);
			out->a = len == 8 ? (uint8_t)(v[6] * 16 + v[7]) : 255;
			return true;
		}
		return false;
	}
	if (has_prefix(text, "rgb(") || has_prefix(text, "rgba(")) {
		const char *args = strchr(text, '(') + 1;
		double c[4] = {0, 0, 0, 1};
		int n = 0;
		char *end;
		while (n < 4) {
			c[n] = strtod(args, &end);
			if (end == args) break;
			n++;
			args = end;
			while (*args == ',' || isspace((unsigned char)*args)) args++;
		}
		if (n < 3) {
			return false;
		}
		out->r = (uint8_t)(c[0] < 0 ? 0 : c[0] > 255 ? 255 : c[0]);
		out->g = (uint8_t)(c[1] < 0 ? 0 : c[1] > 255 ? 255 : c[1]);
		out->b = (uint8_t)(c[2] < 0 ? 0 : c[2] > 255 ? 255 : c[2]);
		double a = c[3] < 0 ? 0 : c[3] > 1 ? 1 : c[3];
		out->a = (uint8_t)(a * 255 + 0.5);
		return true;
	}
	if (has_prefix(text, "alpha(")) {
		// GTK's alpha(color, factor)
		const char *inner = text + 6;
		const char *comma = strrchr(inner, ',');
		if (!comma) {
			return false;
		}
		char *color_text = swine_strndup(inner, (size_t)(comma - inner));
		bool ok = parse_color_depth(sheet, color_text, out, depth + 1);
		free(color_text);
		if (ok) {
			double f = strtod(comma + 1, NULL);
			out->a = (uint8_t)(out->a * (f < 0 ? 0 : f > 1 ? 1 : f));
		}
		return ok;
	}
	static const struct { const char *name; uint32_t rgb; } named[] = {
		{"black", 0x000000}, {"white", 0xffffff}, {"red", 0xff0000},
		{"green", 0x008000}, {"blue", 0x0000ff}, {"yellow", 0xffff00},
		{"orange", 0xffa500}, {"gray", 0x808080}, {"grey", 0x808080},
		{"cyan", 0x00ffff}, {"magenta", 0xff00ff}, {"purple", 0x800080},
		{"silver", 0xc0c0c0}, {"lime", 0x00ff00}, {"navy", 0x000080},
	};
	if (has_prefix(text, "transparent")) {
		*out = (struct swine_color){0, 0, 0, 0};
		return true;
	}
	for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); ++i) {
		size_t len = strlen(named[i].name);
		if (swine_strncasecmp(text, named[i].name, len) == 0 &&
				!is_ident_char(text[len])) {
			out->r = (uint8_t)(named[i].rgb >> 16);
			out->g = (uint8_t)(named[i].rgb >> 8);
			out->b = (uint8_t)named[i].rgb;
			out->a = 255;
			return true;
		}
	}
	return false;
}

bool css_parse_color(const struct css_stylesheet *sheet, const char *text,
		struct swine_color *out) {
	return parse_color_depth(sheet, text, out, 0);
}

struct swine_color color_blend(struct swine_color over,
		struct swine_color under) {
	unsigned a = over.a;
	struct swine_color out = {
		(uint8_t)((over.r * a + under.r * (255 - a)) / 255),
		(uint8_t)((over.g * a + under.g * (255 - a)) / 255),
		(uint8_t)((over.b * a + under.b * (255 - a)) / 255),
		255,
	};
	return out;
}

static double parse_length(const char *text, double em) {
	char *end;
	double v = strtod(text, &end);
	if (end == text) {
		return 0;
	}
	if (has_prefix(end, "pt")) {
		v = v * 4.0 / 3.0;
	} else if (has_prefix(end, "em") || has_prefix(end, "rem")) {
		v = v * em;
	}
	return v;
}

static void parse_box(const char *value, struct css_box *box, double em) {
	list_t *parts = split_string(value, " \t");
	int v[4] = {0};
	int n = parts->length > 4 ? 4 : parts->length;
	for (int i = 0; i < n; ++i) {
		v[i] = (int)(parse_length(parts->items[i], em) + 0.5);
	}
	list_free_items_and_destroy(parts);
	switch (n) {
	case 1: *box = (struct css_box){v[0], v[0], v[0], v[0]}; break;
	case 2: *box = (struct css_box){v[0], v[1], v[0], v[1]}; break;
	case 3: *box = (struct css_box){v[0], v[1], v[2], v[1]}; break;
	case 4: *box = (struct css_box){v[0], v[1], v[2], v[3]}; break;
	default: break;
	}
}

// "3px solid #ffffff", "none", "1px rgba(...)": width and color in any order.
static void parse_edge(const struct css_stylesheet *sheet, const char *value,
		struct css_edge *edge, double em) {
	if (strncmp(value, "none", 4) == 0 || strcmp(value, "0") == 0) {
		edge->width = 0;
		return;
	}
	const char *p = value;
	while (*p) {
		while (isspace((unsigned char)*p)) p++;
		if (!*p) break;
		struct swine_color c;
		if (isdigit((unsigned char)*p) || *p == '.') {
			edge->width = (int)(parse_length(p, em) + 0.5);
		} else if (css_parse_color(sheet, p, &c)) {
			edge->color = c;
		}
		// advance past this token, keeping rgb(...) together
		int depth = 0;
		while (*p && (depth > 0 || !isspace((unsigned char)*p))) {
			if (*p == '(') depth++;
			if (*p == ')') depth--;
			p++;
		}
	}
}

static void parse_box_shadow(const struct css_stylesheet *sheet,
		const char *value, struct swine_style *s, double em) {
	s->inset_bottom = s->inset_top = 0;
	if (strncmp(value, "none", 4) == 0 || !strstr(value, "inset")) {
		return;
	}
	const char *p = strstr(value, "inset") + 5;
	double x = 0, y = 0;
	char *end;
	x = strtod(p, &end);
	if (end == p) return;
	p = end;
	while (*p && !isspace((unsigned char)*p)) p++;   // unit
	y = parse_length(p, em);
	strtod(p, &end);
	p = end;
	while (*p && !isspace((unsigned char)*p)) p++;
	(void)x;
	struct swine_color c = s->color;
	// optional blur/spread lengths before the color
	while (*p) {
		while (isspace((unsigned char)*p)) p++;
		if (isdigit((unsigned char)*p) || *p == '-') {
			while (*p && !isspace((unsigned char)*p)) p++;
			continue;
		}
		if (*p) css_parse_color(sheet, p, &c);
		break;
	}
	if (c.a == 0) {
		return;
	}
	s->inset_color = c;
	if (y < 0) {
		s->inset_bottom = (int)(-y + 0.5);
	} else if (y > 0) {
		s->inset_top = (int)(y + 0.5);
	}
}

static void apply_declaration(const struct css_stylesheet *sheet,
		const struct css_declaration *d, struct swine_style *s,
		const struct swine_style *parent) {
	const char *v = d->value;
	const char *p = d->property;
	struct swine_color c;
	double em = s->font_size;
	if (strcmp(v, "inherit") == 0) {
		if (strcmp(p, "background-color") == 0 || strcmp(p, "background") == 0) {
			s->background = parent->background;
		}
		return;
	}
	if (strcmp(p, "background-color") == 0 || strcmp(p, "background") == 0) {
		if (strncmp(v, "none", 4) == 0) {
			s->background = (struct swine_color){0, 0, 0, 0};
		} else if (css_parse_color(sheet, v, &c)) {
			s->background = c;
		}
	} else if (strcmp(p, "color") == 0) {
		if (css_parse_color(sheet, v, &c)) {
			s->color = c;
		}
	} else if (strcmp(p, "font-family") == 0) {
		char family[128];
		size_t n = 0;
		for (const char *f = v; *f && n < sizeof(family) - 1; ++f) {
			if (*f != '"' && *f != '\'') {
				family[n++] = *f;
			}
		}
		family[n] = '\0';
		memcpy(s->font_family, family, n + 1);
	} else if (strcmp(p, "font-size") == 0) {
		double size = parse_length(v, parent->font_size);
		if (size > 0) {
			s->font_size = size;
		}
	} else if (strcmp(p, "font-weight") == 0) {
		s->bold = strcmp(v, "bold") == 0 || strcmp(v, "bolder") == 0 ||
			atoi(v) >= 600;
	} else if (strcmp(p, "font-style") == 0) {
		s->italic = strcmp(v, "italic") == 0 || strcmp(v, "oblique") == 0;
	} else if (strcmp(p, "font") == 0) {
		// "bold 13px Font Name" (simplified)
		if (strstr(v, "bold")) s->bold = true;
	} else if (strcmp(p, "padding") == 0) {
		parse_box(v, &s->padding, em);
	} else if (strcmp(p, "margin") == 0) {
		parse_box(v, &s->margin, em);
	} else if (has_prefix(p, "padding-") || has_prefix(p, "margin-")) {
		struct css_box *box = p[0] == 'p' ? &s->padding : &s->margin;
		const char *side = strchr(p, '-') + 1;
		int len = (int)(parse_length(v, em) + 0.5);
		if (strcmp(side, "top") == 0) box->top = len;
		else if (strcmp(side, "right") == 0) box->right = len;
		else if (strcmp(side, "bottom") == 0) box->bottom = len;
		else if (strcmp(side, "left") == 0) box->left = len;
	} else if (strcmp(p, "border") == 0) {
		struct css_edge e = {0, s->color};
		parse_edge(sheet, v, &e, em);
		s->border_top = s->border_right = s->border_bottom = s->border_left = e;
	} else if (strcmp(p, "border-top") == 0) {
		parse_edge(sheet, v, &s->border_top, em);
	} else if (strcmp(p, "border-right") == 0) {
		parse_edge(sheet, v, &s->border_right, em);
	} else if (strcmp(p, "border-bottom") == 0) {
		parse_edge(sheet, v, &s->border_bottom, em);
	} else if (strcmp(p, "border-left") == 0) {
		parse_edge(sheet, v, &s->border_left, em);
	} else if (strcmp(p, "border-color") == 0) {
		if (css_parse_color(sheet, v, &c)) {
			s->border_top.color = s->border_right.color =
				s->border_bottom.color = s->border_left.color = c;
		}
	} else if (strcmp(p, "border-width") == 0) {
		struct css_box b = {0};
		parse_box(v, &b, em);
		s->border_top.width = b.top;
		s->border_right.width = b.right;
		s->border_bottom.width = b.bottom;
		s->border_left.width = b.left;
	} else if (strcmp(p, "box-shadow") == 0) {
		parse_box_shadow(sheet, v, s, em);
	} else if (strcmp(p, "min-width") == 0) {
		s->min_width = (int)(parse_length(v, em) + 0.5);
	} else if (strcmp(p, "opacity") == 0) {
		s->opacity = strtod(v, NULL);
	}
}

void css_default_style(struct swine_style *style) {
	memset(style, 0, sizeof(*style));
	style->color = (struct swine_color){255, 255, 255, 255};
	strcpy(style->font_family, "sans-serif");
	style->font_size = 13;
	style->opacity = 1;
}

struct match {
	const struct css_rule *rule;
};

static int match_cmp(const void *a, const void *b) {
	const struct css_rule *ra = ((const struct match *)a)->rule;
	const struct css_rule *rb = ((const struct match *)b)->rule;
	if (ra->selector->specificity != rb->selector->specificity) {
		return ra->selector->specificity - rb->selector->specificity;
	}
	return ra->order - rb->order;
}

void css_compute(const struct css_stylesheet *sheet,
		const struct css_element *element,
		const struct swine_style *parent_style, struct swine_style *out) {
	struct swine_style base;
	css_default_style(&base);
	if (!parent_style) {
		parent_style = &base;
	}
	*out = base;
	// Inherited properties
	out->color = parent_style->color;
	memcpy(out->font_family, parent_style->font_family,
		sizeof(out->font_family));
	out->font_size = parent_style->font_size;
	out->bold = parent_style->bold;
	out->italic = parent_style->italic;
	if (!sheet) {
		return;
	}
	struct match *matches = malloc(sizeof(*matches) *
		(size_t)(sheet->rules->length + 1));
	int n = 0;
	for (int i = 0; i < sheet->rules->length; ++i) {
		struct css_rule *rule = sheet->rules->items[i];
		if (selector_matches(rule->selector, element)) {
			matches[n++].rule = rule;
		}
	}
	qsort(matches, (size_t)n, sizeof(*matches), match_cmp);
	for (int i = 0; i < n; ++i) {
		list_t *decls = matches[i].rule->declarations;
		for (int j = 0; j < decls->length; ++j) {
			apply_declaration(sheet, decls->items[j], out, parent_style);
		}
	}
	free(matches);
}
