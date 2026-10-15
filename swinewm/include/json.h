#ifndef _SWINE_JSON_H
#define _SWINE_JSON_H

#include <stdbool.h>
#include <stddef.h>

// Minimal streaming JSON writer for IPC replies. sway uses json-c; this keeps
// the first draft dependency-free. Swap for a library if parsing is needed.
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

#endif
