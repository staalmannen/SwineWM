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

// Applies a {fmt} spec such as "0.1f", ">3", "3" or "" to an argument.
static void format_arg(struct buf *b, const struct fmt_arg *arg,
		const char *spec, size_t spec_len) {
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
	if (arg->is_number) {
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
		align = arg->is_number ? '>' : '<';
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
		if (*p == '&') {
			static const struct { const char *entity; const char *text; } ents[] = {
				{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
				{"&quot;", "\""}, {"&apos;", "'"},
			};
			bool done = false;
			for (size_t i = 0; i < sizeof(ents) / sizeof(ents[0]); ++i) {
				if (has_prefix(p, ents[i].entity)) {
					buf_str(&b, ents[i].text);
					p += strlen(ents[i].entity);
					done = true;
					break;
				}
			}
			if (done) {
				continue;
			}
		}
		buf_add(&b, p, 1);
		p++;
	}
	return b.data;
}
