#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stringop.h"
#include "swinewm/bar/format.h"

struct buf {
	char *data;
	size_t len, cap;
};

static void buf_add(struct buf *b, const char *s, size_t n) {
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 64;
		while (b->len + n + 1 > cap) {
			cap *= 2;
		}
		b->data = realloc(b->data, cap);
		b->cap = cap;
	}
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
}

static void buf_str(struct buf *b, const char *s) {
	buf_add(b, s, strlen(s));
}

static const struct fmt_arg *find_arg(const struct fmt_arg *args, int nargs,
		const char *name, size_t len) {
	for (int i = 0; i < nargs; ++i) {
		if (args[i].name && strlen(args[i].name) == len &&
				strncmp(args[i].name, name, len) == 0) {
			return &args[i];
		}
	}
	return NULL;
}

// Waybar's fmt::formatter<pow_format> (include/util/format.hpp).
static void format_pow(struct buf *b, const struct fmt_arg *arg,
		const char *spec, size_t spec_len) {
	static const char *const units[] = {"", "k", "M", "G", "T", "P"};
	const int max_pow = 5;
	char align = 0, scale = 0, unit_pref = 0, base_pref = 0;
	bool force_int = false;
	int width = 0;
	size_t i = 0;
	if (i < spec_len && (spec[i] == '>' || spec[i] == '<' || spec[i] == '=')) {
		align = spec[i++];
	}
	for (; i < spec_len; ++i) {
		char c = spec[i];
		if (c == '#' || c == 'k' || c == 'M' || c == 'G' || c == 'T' || c == 'P') {
			scale = c;
		} else if (c == 'u' || c == 'U') {
			unit_pref = c;
		} else if (c == 'b' || c == 'B') {
			base_pref = c;
		} else if (c == 'i') {
			force_int = true;
		} else if (c >= '0' && c <= '9') {
			width = width * 10 + (c - '0');
		} else {
			break;
		}
	}
	bool binary = base_pref == 'B' ? true : base_pref == 'b' ? false :
		arg->type == FMT_POW_BINARY;
	double base = binary ? 1024.0 : 1000.0;
	double value = arg->num < 0 ? 0 : arg->num;
	double fraction = value, div = 1;
	int pow = 0;
	if (scale) {
		const char *scales = "#kMGTP";
		pow = (int)(strchr(scales, scale) - scales);
		for (int k = 0; k < pow; ++k) {
			div *= base;
		}
		fraction /= div;
	} else {
		while (pow < max_pow && fraction / base >= 1) {
			fraction /= base;
			div *= base;
			pow++;
		}
	}
	int precision = 1;
	if (force_int) {
		precision = 0;
	} else if (arg->type == FMT_POW_SKIP_DECIMAL &&
			fmod(value, div) == 0) {
		precision = 0;
	} else if (arg->type == FMT_POW_COMPACT && !scale && pow < 2) {
		precision = 0;
	}
	bool hide_unit = unit_pref == 'u' || (unit_pref == 0 && scale != 0);
	char prefix[4] = "";
	if (!scale) {
		snprintf(prefix, sizeof(prefix), "%s%s", units[pow],
			binary && pow ? "i" : "");
	}
	const char *unit = hide_unit || !arg->str ? "" : arg->str;
	int number_width = 3 + precision + (precision != 0) + (binary ? 1 : 0);
	int prefix_col = scale ? 0 : 1 + (binary ? 1 : 0);
	int max_width = number_width + prefix_col + (int)strlen(unit);
	bool fixed_num = scale && width > 0;
	char number[64];
	snprintf(number, sizeof(number), "%.*f", precision, fraction);
	if (fixed_num && (int)strlen(number) > width) {
		int w = width < 63 ? width : 63;
		memset(number, '#', (size_t)w);
		number[w] = '\0';
	}
	char text[160];
	if (align == '=') {
		// Column-align: the number left-justified in its column, then the
		// prefix and unit, padded so that columns line up.
		const char *padding = !scale && pow == 0 ? (binary ? "  " : " ") : "";
		snprintf(text, sizeof(text), "%-*s%s%s%s",
			fixed_num ? width : number_width, number, padding, prefix, unit);
		buf_str(b, text);
		return;
	}
	char body[128];
	if (fixed_num) {
		snprintf(body, sizeof(body), "%*s%s%s", width, number, prefix, unit);
	} else {
		snprintf(body, sizeof(body), "%s%s%s", number, prefix, unit);
	}
	if (align == '>') {
		snprintf(text, sizeof(text), "%*s", max_width, body);
	} else if (align == '<') {
		snprintf(text, sizeof(text), "%-*s", max_width, body);
	} else {
		snprintf(text, sizeof(text), "%s", body);
	}
	buf_str(b, text);
}

// Applies a {fmt} spec such as "0.1f", ">3", "3" or "" to an argument.
static void format_arg(struct buf *b, const struct fmt_arg *arg,
		const char *spec, size_t spec_len) {
	if (arg->type >= FMT_POW) {
		format_pow(b, arg, spec, spec_len);
		return;
	}
	bool is_number = arg->type == FMT_NUMBER;
	char align = 0;
	int width = 0, precision = -1;
	char type = 0;
	size_t i = 0;
	if (i < spec_len && (spec[i] == '<' || spec[i] == '>' || spec[i] == '^')) {
		align = spec[i++];
	}
	while (i < spec_len && spec[i] >= '0' && spec[i] <= '9') {
		width = width * 10 + (spec[i++] - '0');
	}
	if (i < spec_len && spec[i] == '.') {
		i++;
		precision = 0;
		while (i < spec_len && spec[i] >= '0' && spec[i] <= '9') {
			precision = precision * 10 + (spec[i++] - '0');
		}
	}
	if (i < spec_len) {
		type = spec[i];
	}
	char text[128];
	if (is_number) {
		if (precision >= 0 || type == 'f') {
			snprintf(text, sizeof(text), "%.*f", precision >= 0 ? precision : 6,
				arg->num);
		} else {
			snprintf(text, sizeof(text), "%.0f", arg->num);
		}
	} else {
		snprintf(text, sizeof(text), "%s", arg->str ? arg->str : "");
	}
	int len = (int)strlen(text);
	int pad = width > len ? width - len : 0;
	// fmt right-aligns numbers and left-aligns strings by default
	if (!align) {
		align = is_number ? '>' : '<';
	}
	int left = align == '>' ? pad : align == '^' ? pad / 2 : 0;
	int right = pad - left;
	for (int k = 0; k < left; ++k) buf_add(b, " ", 1);
	buf_str(b, text);
	for (int k = 0; k < right; ++k) buf_add(b, " ", 1);
}

// Keeps only C99 strftime conversions. GNU extensions common in Waybar
// configs ("%-d", "%P", "%k") make the Windows CRT abort the process, so
// flags are dropped and unknown conversions removed.
static char *sanitize_strftime(const char *pattern) {
	static const char allowed[] = "aAbBcCdDeFgGhHIjmMnprRStTuUVwWxXyYzZ%";
	char *out = malloc(strlen(pattern) + 1);
	char *o = out;
	for (const char *p = pattern; *p; ++p) {
		if (*p != '%') {
			*o++ = *p;
			continue;
		}
		const char *c = p + 1;
		while (*c == '-' || *c == '_' || *c == '0' || *c == '^' || *c == '#' ||
				*c == 'E' || *c == 'O') {
			c++;
		}
		if (*c && strchr(allowed, *c)) {
			*o++ = '%';
			*o++ = *c;
		} else if (*c == 'P') {
			*o++ = '%';   // lower-case am/pm: close enough
			*o++ = 'p';
		} else if (*c == 'k' || *c == 'l') {
			*o++ = '%';
			*o++ = *c == 'k' ? 'H' : 'I';
		}
		p = *c ? c : c - 1;
	}
	*o = '\0';
	return out;
}

char *format_waybar(const char *fmt, const struct fmt_arg *args, int nargs,
		const struct tm *tm) {
	struct buf b = {0};
	buf_add(&b, "", 0);
	const char *p = fmt ? fmt : "";
	while (*p) {
		if (p[0] == '{' && p[1] == '{') {
			buf_add(&b, "{", 1);
			p += 2;
			continue;
		}
		if (p[0] == '}' && p[1] == '}') {
			buf_add(&b, "}", 1);
			p += 2;
			continue;
		}
		if (*p != '{') {
			buf_add(&b, p, 1);
			p++;
			continue;
		}
		const char *close = strchr(p, '}');
		if (!close) {
			buf_str(&b, p);
			break;
		}
		const char *name = p + 1;
		const char *colon = memchr(name, ':', (size_t)(close - name));
		size_t name_len = (size_t)((colon ? colon : close) - name);
		const char *spec = colon ? colon + 1 : close;
		size_t spec_len = (size_t)(close - spec);
		if (spec_len > 0 && (spec[0] == '%' ||
				(spec[0] == 'L' && spec_len > 1 && spec[1] == '%'))) {
			// strftime spec (clock); a leading 'L' asks for the locale
			if (tm) {
				char *raw = swine_strndup(spec[0] == 'L' ? spec + 1 : spec,
					spec[0] == 'L' ? spec_len - 1 : spec_len);
				char *pattern = sanitize_strftime(raw);
				char out[256];
				size_t n = *pattern ? strftime(out, sizeof(out), pattern, tm) : 0;
				buf_add(&b, out, n);
				free(pattern);
				free(raw);
			}
		} else {
			const struct fmt_arg *arg = name_len == 0 ?
				(nargs > 0 ? &args[0] : NULL) :
				find_arg(args, nargs, name, name_len);
			if (arg) {
				format_arg(&b, arg, spec, spec_len);
			}
		}
		p = close + 1;
	}
	return b.data;
}

char *markup_escape(const char *text) {
	struct buf b = {0};
	buf_add(&b, "", 0);
	for (const char *p = text ? text : ""; *p; ++p) {
		switch (*p) {
		case '&': buf_str(&b, "&amp;"); break;
		case '<': buf_str(&b, "&lt;"); break;
		case '>': buf_str(&b, "&gt;"); break;
		case '"': buf_str(&b, "&quot;"); break;
		case '\'': buf_str(&b, "&apos;"); break;
		default: buf_add(&b, p, 1); break;
		}
	}
	return b.data;
}

static void buf_utf8(struct buf *b, unsigned long c) {
	char out[4];
	size_t n;
	if (c < 0x80) {
		out[0] = (char)c;
		n = 1;
	} else if (c < 0x800) {
		out[0] = (char)(0xC0 | (c >> 6));
		out[1] = (char)(0x80 | (c & 0x3F));
		n = 2;
	} else if (c < 0x10000) {
		out[0] = (char)(0xE0 | (c >> 12));
		out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
		out[2] = (char)(0x80 | (c & 0x3F));
		n = 3;
	} else {
		out[0] = (char)(0xF0 | (c >> 18));
		out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
		out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
		out[3] = (char)(0x80 | (c & 0x3F));
		n = 4;
	}
	buf_add(b, out, n);
}

// Decodes the entity at p (the XML ones, &#N; and &#xN;) into b; returns
// its length, or 0 if p is no entity.
static size_t decode_entity(const char *p, struct buf *b) {
	static const struct { const char *entity; const char *text; } ents[] = {
		{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
		{"&quot;", "\""}, {"&apos;", "'"},
	};
	for (size_t i = 0; i < sizeof(ents) / sizeof(ents[0]); ++i) {
		if (has_prefix(p, ents[i].entity)) {
			buf_str(b, ents[i].text);
			return strlen(ents[i].entity);
		}
	}
	if (p[1] == '#') {
		bool hex = p[2] == 'x' || p[2] == 'X';
		char *end;
		unsigned long c = strtoul(p + (hex ? 3 : 2), &end, hex ? 16 : 10);
		if (end > p + (hex ? 3 : 2) && *end == ';' && c > 0 && c <= 0x10FFFF) {
			buf_utf8(b, c);
			return (size_t)(end + 1 - p);
		}
	}
	return 0;
}

char *markup_strip(const char *text) {
	struct buf b = {0};
	buf_add(&b, "", 0);
	const char *p = text ? text : "";
	while (*p) {
		if (*p == '<') {
			const char *end = strchr(p, '>');
			if (end) {
				p = end + 1;
				continue;
			}
		}
		size_t n = *p == '&' ? decode_entity(p, &b) : 0;
		if (n) {
			p += n;
			continue;
		}
		buf_add(&b, p, 1);
		p++;
	}
	return b.data;
}

struct span_state {
	bool bold, italic, underline, strikethrough;
};

// The value of attribute name in a tag's text (between '<' and '>'), or
// NULL. Caller frees.
static char *tag_attribute(const char *tag, size_t len, const char *name) {
	size_t nlen = strlen(name);
	for (size_t i = 0; i + nlen < len; ++i) {
		if ((i == 0 || tag[i - 1] == ' ' || tag[i - 1] == '\t') &&
				strncmp(tag + i, name, nlen) == 0) {
			size_t j = i + nlen;
			while (j < len && tag[j] == ' ') {
				j++;
			}
			if (j >= len || tag[j] != '=') {
				continue;
			}
			j++;
			while (j < len && tag[j] == ' ') {
				j++;
			}
			if (j >= len || (tag[j] != '"' && tag[j] != '\'')) {
				continue;
			}
			char quote = tag[j++];
			size_t start = j;
			while (j < len && tag[j] != quote) {
				j++;
			}
			char *value = malloc(j - start + 1);
			memcpy(value, tag + start, j - start);
			value[j - start] = '\0';
			return value;
		}
	}
	return NULL;
}

// Applies an opening tag (its text between '<' and '>') to state.
static void apply_tag(struct span_state *state, const char *tag, size_t len) {
	size_t name_len = 0;
	while (name_len < len && tag[name_len] != ' ' && tag[name_len] != '\t') {
		name_len++;
	}
	if (name_len == 1) {
		switch (tag[0]) {
		case 'b': state->bold = true; break;
		case 'i': state->italic = true; break;
		case 'u': state->underline = true; break;
		case 's': state->strikethrough = true; break;
		default: break;
		}
		return;
	}
	if (name_len != 4 || strncmp(tag, "span", 4) != 0) {
		return;
	}
	char *v = tag_attribute(tag, len, "weight");
	if (!v) {
		v = tag_attribute(tag, len, "font_weight");
	}
	if (v) {
		int number = atoi(v);
		state->bold = number > 0 ? number >= 600 :
			strcmp(v, "bold") == 0 || strcmp(v, "ultrabold") == 0 ||
			strcmp(v, "heavy") == 0 || strcmp(v, "ultraheavy") == 0 ||
			strcmp(v, "semibold") == 0;
		free(v);
	}
	v = tag_attribute(tag, len, "style");
	if (!v) {
		v = tag_attribute(tag, len, "font_style");
	}
	if (v) {
		state->italic = strcmp(v, "italic") == 0 || strcmp(v, "oblique") == 0;
		free(v);
	}
	v = tag_attribute(tag, len, "underline");
	if (v) {
		state->underline = strcmp(v, "none") != 0 && strcmp(v, "false") != 0;
		free(v);
	}
	v = tag_attribute(tag, len, "strikethrough");
	if (v) {
		state->strikethrough = strcmp(v, "true") == 0;
		free(v);
	}
}

static void add_span(list_t *spans, struct buf *text,
		const struct span_state *state) {
	if (!text->len) {
		return;
	}
	struct markup_span *last = spans->length ?
		spans->items[spans->length - 1] : NULL;
	if (last && last->bold == state->bold && last->italic == state->italic &&
			last->underline == state->underline &&
			last->strikethrough == state->strikethrough) {
		size_t old = strlen(last->text);
		last->text = realloc(last->text, old + text->len + 1);
		memcpy(last->text + old, text->data, text->len + 1);
	} else {
		struct markup_span *span = calloc(1, sizeof(*span));
		span->text = text->data;
		text->data = NULL;
		text->cap = 0;
		span->bold = state->bold;
		span->italic = state->italic;
		span->underline = state->underline;
		span->strikethrough = state->strikethrough;
		list_add(spans, span);
	}
	text->len = 0;
	if (text->data) {
		text->data[0] = '\0';
	}
}

list_t *markup_spans(const char *text) {
	enum { MAX_DEPTH = 32 };
	struct span_state stack[MAX_DEPTH] = {{0}};
	int depth = 0, overflow = 0;
	list_t *spans = create_list();
	struct buf b = {0};
	const char *p = text ? text : "";
	while (*p) {
		const char *end = *p == '<' ? strchr(p, '>') : NULL;
		if (end) {
			add_span(spans, &b, &stack[depth]);
			const char *tag = p + 1;
			size_t len = (size_t)(end - tag);
			if (len && tag[0] == '/') {
				if (overflow) {
					overflow--;
				} else if (depth > 0) {
					depth--;
				}
			} else if (len && tag[len - 1] != '/') {
				if (depth + 1 < MAX_DEPTH) {
					stack[depth + 1] = stack[depth];
					depth++;
					apply_tag(&stack[depth], tag, len);
				} else {
					overflow++;
				}
			}
			p = end + 1;
			continue;
		}
		size_t n = *p == '&' ? decode_entity(p, &b) : 0;
		if (n) {
			p += n;
			continue;
		}
		buf_add(&b, p, 1);
		p++;
	}
	add_span(spans, &b, &stack[depth]);
	free(b.data);
	return spans;
}

void markup_spans_free(list_t *spans) {
	for (int i = 0; spans && i < spans->length; ++i) {
		struct markup_span *span = spans->items[i];
		free(span->text);
		free(span);
	}
	list_free(spans);
}
