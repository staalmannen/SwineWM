#ifndef _SWINE_JSON_H
#define _SWINE_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include "list.h"

// Minimal JSON writer (IPC replies) and JSONC reader (Waybar configs).
// sway and Waybar use json-c / jsoncpp; this keeps swinewm dependency-free.
#define JSON_MAX_DEPTH 64

struct json_writer {
	char *buf;
	size_t len, cap;
	int depth;
	bool need_comma[JSON_MAX_DEPTH];
	bool after_key;
};

void json_init(struct json_writer *w);
void json_begin_object(struct json_writer *w);
void json_end_object(struct json_writer *w);
void json_begin_array(struct json_writer *w);
void json_end_array(struct json_writer *w);
void json_key(struct json_writer *w, const char *key);
void json_string(struct json_writer *w, const char *value); // NULL -> null
void json_int(struct json_writer *w, long long value);
void json_double(struct json_writer *w, double value);
void json_bool(struct json_writer *w, bool value);
void json_null(struct json_writer *w);
// Returns the accumulated text; the caller owns it. The writer is reset.
char *json_finish(struct json_writer *w);

enum json_type {
	JSON_NULL,
	JSON_BOOL,
	JSON_NUMBER,
	JSON_STRING,
	JSON_ARRAY,
	JSON_OBJECT,
};

struct json_value {
	enum json_type type;
	bool boolean;
	double number;
	char *string;
	list_t *items;  // array elements, or object values (struct json_value *)
	list_t *keys;   // object keys (char *), parallel to items
};

// Parses JSON with the JSONC extensions Waybar accepts: // and /* */
// comments and trailing commas. Returns NULL and sets *error (caller frees)
// on failure.
struct json_value *json_parse(const char *text, char **error);
void json_value_free(struct json_value *value);
// Object member lookup; NULL if obj is not an object or has no such key.
struct json_value *json_object_get(const struct json_value *obj,
		const char *key);
const char *json_get_string(const struct json_value *obj, const char *key,
		const char *fallback);
double json_get_number(const struct json_value *obj, const char *key,
		double fallback);
bool json_get_bool(const struct json_value *obj, const char *key,
		bool fallback);

#endif
