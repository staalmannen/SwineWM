#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"

void json_init(struct json_writer *w) {
	memset(w, 0, sizeof(*w));
}

static void append(struct json_writer *w, const char *text, size_t n) {
	if (w->len + n + 1 > w->cap) {
		size_t cap = w->cap ? w->cap : 256;
		while (w->len + n + 1 > cap) {
			cap *= 2;
		}
		char *buf = realloc(w->buf, cap);
		if (!buf) {
			abort();
		}
		w->buf = buf;
		w->cap = cap;
	}
	memcpy(w->buf + w->len, text, n);
	w->len += n;
	w->buf[w->len] = '\0';
}

static void append_str(struct json_writer *w, const char *text) {
	append(w, text, strlen(text));
}

static void before_value(struct json_writer *w) {
	if (w->after_key) {
		w->after_key = false;
		return;
	}
	if (w->need_comma[w->depth]) {
		append(w, ",", 1);
	}
	w->need_comma[w->depth] = true;
}

static void json_open(struct json_writer *w, const char *bracket) {
	before_value(w);
	append_str(w, bracket);
	if (w->depth + 1 < JSON_MAX_DEPTH) {
		w->depth++;
	}
	w->need_comma[w->depth] = false;
}

static void json_close(struct json_writer *w, const char *bracket) {
	append_str(w, bracket);
	if (w->depth > 0) {
		w->depth--;
	}
}

void json_begin_object(struct json_writer *w) { json_open(w, "{"); }
void json_end_object(struct json_writer *w) { json_close(w, "}"); }
void json_begin_array(struct json_writer *w) { json_open(w, "["); }
void json_end_array(struct json_writer *w) { json_close(w, "]"); }

static void write_escaped(struct json_writer *w, const char *value) {
	append(w, "\"", 1);
	for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
		char esc[8];
		switch (*p) {
		case '"': append_str(w, "\\\""); break;
		case '\\': append_str(w, "\\\\"); break;
		case '\n': append_str(w, "\\n"); break;
		case '\r': append_str(w, "\\r"); break;
		case '\t': append_str(w, "\\t"); break;
		default:
			if (*p < 0x20) {
				snprintf(esc, sizeof(esc), "\\u%04x", *p);
				append_str(w, esc);
			} else {
				append(w, (const char *)p, 1);
			}
		}
	}
	append(w, "\"", 1);
}

void json_key(struct json_writer *w, const char *key) {
	before_value(w);
	write_escaped(w, key);
	append(w, ":", 1);
	w->after_key = true;
}

void json_string(struct json_writer *w, const char *value) {
	if (!value) {
		json_null(w);
		return;
	}
	before_value(w);
	write_escaped(w, value);
}

void json_int(struct json_writer *w, long long value) {
	char num[32];
	snprintf(num, sizeof(num), "%lld", value);
	before_value(w);
	append_str(w, num);
}

void json_double(struct json_writer *w, double value) {
	char num[64];
	snprintf(num, sizeof(num), "%g", value);
	before_value(w);
	append_str(w, num);
}

void json_bool(struct json_writer *w, bool value) {
	before_value(w);
	append_str(w, value ? "true" : "false");
}

void json_null(struct json_writer *w) {
	before_value(w);
	append_str(w, "null");
}

char *json_finish(struct json_writer *w) {
	char *buf = w->buf;
	if (!buf) {
		buf = calloc(1, 1);
	}
	json_init(w);
	return buf;
}
