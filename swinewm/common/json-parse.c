#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "stringop.h"

struct parser {
	const char *p;
	const char *start;
	char *error;
	int depth;
};

static void fail(struct parser *ps, const char *what) {
	if (ps->error) {
		return;
	}
	int line = 1, col = 1;
	for (const char *c = ps->start; c < ps->p && *c; ++c) {
		if (*c == '\n') {
			line++;
			col = 1;
		} else {
			col++;
		}
	}
	ps->error = format_str("%s at line %d, column %d", what, line, col);
}

static void skip_ws(struct parser *ps) {
	for (;;) {
		while (*ps->p && isspace((unsigned char)*ps->p)) {
			ps->p++;
		}
		if (ps->p[0] == '/' && ps->p[1] == '/') {
			while (*ps->p && *ps->p != '\n') {
				ps->p++;
			}
		} else if (ps->p[0] == '/' && ps->p[1] == '*') {
			const char *end = strstr(ps->p + 2, "*/");
			if (!end) {
				fail(ps, "Unterminated comment");
				ps->p += strlen(ps->p);
				return;
			}
			ps->p = end + 2;
		} else {
			return;
		}
	}
}

static struct json_value *new_value(enum json_type type) {
	struct json_value *v = calloc(1, sizeof(*v));
	v->type = type;
	if (type == JSON_ARRAY || type == JSON_OBJECT) {
		v->items = create_list();
	}
	if (type == JSON_OBJECT) {
		v->keys = create_list();
	}
	return v;
}

static void put_utf8(char **out, unsigned cp) {
	char *o = *out;
	if (cp < 0x80) {
		*o++ = (char)cp;
	} else if (cp < 0x800) {
		*o++ = (char)(0xC0 | (cp >> 6));
		*o++ = (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		*o++ = (char)(0xE0 | (cp >> 12));
		*o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
		*o++ = (char)(0x80 | (cp & 0x3F));
	} else {
		*o++ = (char)(0xF0 | (cp >> 18));
		*o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
		*o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
		*o++ = (char)(0x80 | (cp & 0x3F));
	}
	*out = o;
}

static bool read_hex4(const char *p, unsigned *out) {
	unsigned v = 0;
	for (int i = 0; i < 4; ++i) {
		char c = p[i];
		v <<= 4;
		if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
		else return false;
	}
	*out = v;
	return true;
}

static char *parse_string(struct parser *ps) {
	ps->p++; // opening quote
	// The decoded string is never longer than the source text.
	const char *end = ps->p;
	while (*end && *end != '"') {
		end += (*end == '\\' && end[1]) ? 2 : 1;
	}
	char *buf = malloc((size_t)(end - ps->p) + 1);
	char *o = buf;
	while (*ps->p && *ps->p != '"') {
		char c = *ps->p++;
		if (c != '\\') {
			*o++ = c;
			continue;
		}
		char e = *ps->p++;
		switch (e) {
		case '"': *o++ = '"'; break;
		case '\\': *o++ = '\\'; break;
		case '/': *o++ = '/'; break;
		case 'b': *o++ = '\b'; break;
		case 'f': *o++ = '\f'; break;
		case 'n': *o++ = '\n'; break;
		case 'r': *o++ = '\r'; break;
		case 't': *o++ = '\t'; break;
		case 'u': {
			unsigned cp;
			if (!read_hex4(ps->p, &cp)) {
				fail(ps, "Invalid \\u escape");
				free(buf);
				return NULL;
			}
			ps->p += 4;
			if (cp >= 0xD800 && cp < 0xDC00 && ps->p[0] == '\\' &&
					ps->p[1] == 'u') {
				unsigned lo;
				if (read_hex4(ps->p + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
					ps->p += 6;
				}
			}
			put_utf8(&o, cp);
			break;
		}
		default:
			fail(ps, "Invalid escape sequence");
			free(buf);
			return NULL;
		}
	}
	if (*ps->p != '"') {
		fail(ps, "Unterminated string");
		free(buf);
		return NULL;
	}
	ps->p++;
	*o = '\0';
	return buf;
}

static struct json_value *parse_value(struct parser *ps);

static struct json_value *parse_container(struct parser *ps, bool object) {
	struct json_value *v = new_value(object ? JSON_OBJECT : JSON_ARRAY);
	char close = object ? '}' : ']';
	ps->p++;
	if (++ps->depth > 128) {
		fail(ps, "Nesting too deep");
		json_value_free(v);
		return NULL;
	}
	for (;;) {
		skip_ws(ps);
		if (*ps->p == close) {
			ps->p++;
			break;
		}
		char *key = NULL;
		if (object) {
			if (*ps->p != '"') {
				fail(ps, "Expected a string key");
				break;
			}
			key = parse_string(ps);
			if (!key) {
				break;
			}
			skip_ws(ps);
			if (*ps->p != ':') {
				free(key);
				fail(ps, "Expected ':'");
				break;
			}
			ps->p++;
		}
		struct json_value *item = parse_value(ps);
		if (!item) {
			free(key);
			break;
		}
		if (object) {
			list_add(v->keys, key);
		}
		list_add(v->items, item);
		skip_ws(ps);
		if (*ps->p == ',') {
			ps->p++;  // a trailing comma before the close is fine (JSONC)
		} else if (*ps->p != close) {
			fail(ps, object ? "Expected ',' or '}'" : "Expected ',' or ']'");
			break;
		}
	}
	ps->depth--;
	if (ps->error) {
		json_value_free(v);
		return NULL;
	}
	return v;
}

static struct json_value *parse_value(struct parser *ps) {
	skip_ws(ps);
	if (ps->error) {
		return NULL;
	}
	char c = *ps->p;
	if (c == '{' || c == '[') {
		return parse_container(ps, c == '{');
	}
	if (c == '"') {
		char *s = parse_string(ps);
		if (!s) {
			return NULL;
		}
		struct json_value *v = new_value(JSON_STRING);
		v->string = s;
		return v;
	}
	if (strncmp(ps->p, "true", 4) == 0 || strncmp(ps->p, "false", 5) == 0) {
		struct json_value *v = new_value(JSON_BOOL);
		v->boolean = c == 't';
		ps->p += v->boolean ? 4 : 5;
		return v;
	}
	if (strncmp(ps->p, "null", 4) == 0) {
		ps->p += 4;
		return new_value(JSON_NULL);
	}
	if (c == '-' || isdigit((unsigned char)c)) {
		char *end = NULL;
		double n = strtod(ps->p, &end);
		if (end == ps->p) {
			fail(ps, "Invalid number");
			return NULL;
		}
		ps->p = end;
		struct json_value *v = new_value(JSON_NUMBER);
		v->number = n;
		return v;
	}
	fail(ps, c ? "Unexpected character" : "Unexpected end of input");
	return NULL;
}

struct json_value *json_parse(const char *text, char **error) {
	struct parser ps = { .p = text, .start = text };
	// Skip a UTF-8 byte order mark.
	if ((unsigned char)ps.p[0] == 0xEF && (unsigned char)ps.p[1] == 0xBB &&
			(unsigned char)ps.p[2] == 0xBF) {
		ps.p += 3;
	}
	struct json_value *v = parse_value(&ps);
	if (v) {
		skip_ws(&ps);
		if (*ps.p) {
			fail(&ps, "Trailing characters");
			json_value_free(v);
			v = NULL;
		}
	}
	if (error) {
		*error = ps.error;
	} else {
		free(ps.error);
	}
	return v;
}

void json_value_free(struct json_value *v) {
	if (!v) {
		return;
	}
	if (v->items) {
		for (int i = 0; i < v->items->length; ++i) {
			json_value_free(v->items->items[i]);
		}
		list_free(v->items);
	}
	list_free_items_and_destroy(v->keys);
	free(v->string);
	free(v);
}

struct json_value *json_object_get(const struct json_value *obj,
		const char *key) {
	if (!obj || obj->type != JSON_OBJECT) {
		return NULL;
	}
	// Last one wins for duplicate keys, as in most JSON libraries.
	for (int i = obj->keys->length - 1; i >= 0; --i) {
		if (strcmp(obj->keys->items[i], key) == 0) {
			return obj->items->items[i];
		}
	}
	return NULL;
}

const char *json_get_string(const struct json_value *obj, const char *key,
		const char *fallback) {
	struct json_value *v = json_object_get(obj, key);
	return v && v->type == JSON_STRING ? v->string : fallback;
}

double json_get_number(const struct json_value *obj, const char *key,
		double fallback) {
	struct json_value *v = json_object_get(obj, key);
	return v && v->type == JSON_NUMBER ? v->number : fallback;
}

bool json_get_bool(const struct json_value *obj, const char *key,
		bool fallback) {
	struct json_value *v = json_object_get(obj, key);
	return v && v->type == JSON_BOOL ? v->boolean : fallback;
}
