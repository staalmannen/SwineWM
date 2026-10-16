#ifndef _SWINE_REGEX_H
#define _SWINE_REGEX_H

#include <stdbool.h>

// A small regular-expression engine for sway criteria ([title="..."]),
// which sway matches with PCRE2. Supported: literals, ".", classes
// ("[a-z]", "[^...]", "\d \w \s" and their negations), anchors "^ $ \b \B",
// quantifiers "* + ? {n} {n,} {n,m}" and their lazy forms, groups "(...)"
// and "(?:...)", alternation "|", and the flag "(?i)" (case-insensitive
// from there on). Text and patterns are UTF-8; "." and classes match whole
// code points. Like pcre2_match, a match may start anywhere in the text.

struct swine_regex;

// Returns NULL and sets *error (caller frees) if the pattern is invalid.
struct swine_regex *swine_regex_compile(const char *pattern, bool icase,
		char **error);
bool swine_regex_search(const struct swine_regex *re, const char *text);
void swine_regex_free(struct swine_regex *re);

#endif
