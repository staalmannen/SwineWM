#ifndef _SWINE_STRINGOP_H
#define _SWINE_STRINGOP_H

#include <stdbool.h>
#include <stddef.h>
#include "list.h"

// Portable replacements for POSIX functions that MSVC lacks or deprecates.
char *swine_strdup(const char *str);
char *swine_strndup(const char *str, size_t n);
int swine_strcasecmp(const char *a, const char *b);
int swine_strncasecmp(const char *a, const char *b, size_t n);

void strip_whitespace(char *str);
void strip_quotes(char *str);
bool has_prefix(const char *str, const char *prefix);

// Splits on any of delims, dropping empty tokens. Free with
// list_free_items_and_destroy.
list_t *split_string(const char *str, const char *delims);

// Splits a command line into arguments. Double or single quotes group words
// and are kept, as in sway; callers strip them where appropriate.
// Free with free_argv.
char **split_args(const char *str, int *argc);
void free_argv(int argc, char **argv);
char *join_args(char **argv, int argc);

// Quote-aware strsep(): splits *stringp at the first unquoted delimiter.
// The delimiter found is written to *matched (or '\0' at end of string).
char *argsep(char **stringp, const char *delim, char *matched);

// printf into a newly allocated string.
char *format_str(const char *fmt, ...);

// Parses yes/no/true/false/enable/disable/on/off/toggle.
bool parse_boolean(const char *boolean, bool current);
// Shell-style wildcard match (*, ?, [abc], [a-z], [!x]), ignoring ASCII
// case as Windows file names do.
bool glob_match(const char *pattern, const char *name);
bool has_glob_chars(const char *str);

#endif
