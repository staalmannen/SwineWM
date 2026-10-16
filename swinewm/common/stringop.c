#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stringop.h"

char *swine_strdup(const char *str) {
	if (!str) {
		return NULL;
	}
	size_t len = strlen(str) + 1;
	char *copy = malloc(len);
	if (copy) {
		memcpy(copy, str, len);
	}
	return copy;
}

char *swine_strndup(const char *str, size_t n) {
	size_t len = 0;
	while (len < n && str[len]) {
		++len;
	}
	char *copy = malloc(len + 1);
	if (copy) {
		memcpy(copy, str, len);
		copy[len] = '\0';
	}
	return copy;
}

int swine_strcasecmp(const char *a, const char *b) {
	for (;; ++a, ++b) {
		int ca = tolower((unsigned char)*a);
		int cb = tolower((unsigned char)*b);
		if (ca != cb || ca == '\0') {
			return ca - cb;
		}
	}
}

int swine_strncasecmp(const char *a, const char *b, size_t n) {
	for (; n > 0; --n, ++a, ++b) {
		int ca = tolower((unsigned char)*a);
		int cb = tolower((unsigned char)*b);
		if (ca != cb || ca == '\0') {
			return ca - cb;
		}
	}
	return 0;
}

void strip_whitespace(char *str) {
	size_t len = strlen(str);
	size_t start = strspn(str, " \f\n\r\t\v");
	memmove(str, &str[start], len + 1 - start);
	len -= start;
	while (len > 0 && isspace((unsigned char)str[len - 1])) {
		str[--len] = '\0';
	}
}

void strip_quotes(char *str) {
	bool in_str = false;
	bool in_chr = false;
	bool escaped = false;
	char *end = strchr(str, '\0');
	while (*str) {
		if (*str == '\'' && !in_str && !escaped) {
			in_chr = !in_chr;
			goto shift_over;
		} else if (*str == '\"' && !in_chr && !escaped) {
			in_str = !in_str;
			goto shift_over;
		} else if (*str == '\\') {
			escaped = !escaped;
			++str;
			continue;
		}
		escaped = false;
		++str;
		continue;
shift_over:
		memmove(str, str + 1, end-- - str);
	}
	*end = '\0';
}

bool has_prefix(const char *str, const char *prefix) {
	return strncmp(str, prefix, strlen(prefix)) == 0;
}

list_t *split_string(const char *str, const char *delims) {
	list_t *res = create_list();
	const char *p = str;
	while (*p) {
		size_t skip = strspn(p, delims);
		p += skip;
		if (!*p) {
			break;
		}
		size_t len = strcspn(p, delims);
		list_add(res, swine_strndup(p, len));
		p += len;
	}
	return res;
}

char **split_args(const char *start, int *argc) {
	*argc = 0;
	int alloc = 2;
	char **argv = malloc(sizeof(char *) * alloc);
	bool in_token = false;
	bool in_string = false;
	bool in_char = false;
	bool in_brackets = false;   // criteria: "[app_id=x title=y]" is one token
	bool escaped = false;
	const char *end = start;
	if (start) {
		while (*start) {
			if (!in_token) {
				start = (end += strspn(end, " \t"));
				in_token = true;
			}
			if (*end == '"' && !in_char && !escaped) {
				in_string = !in_string;
			} else if (*end == '\'' && !in_string && !escaped) {
				in_char = !in_char;
			} else if (*end == '[' && !in_string && !in_char && !in_brackets &&
					!escaped) {
				in_brackets = true;
			} else if (*end == ']' && !in_string && !in_char && in_brackets &&
					!escaped) {
				in_brackets = false;
			} else if (*end == '\\') {
				escaped = !escaped;
			} else if (*end == '\0' || (!in_string && !in_char && !in_brackets
					&& !escaped && strchr(" \t", *end))) {
				if (end != start) {
					char *token = swine_strndup(start, (size_t)(end - start));
					argv[(*argc)++] = token;
					if (*argc + 1 >= alloc) {
						alloc *= 2;
						argv = realloc(argv, sizeof(char *) * alloc);
					}
				}
				in_token = false;
				escaped = false;
				if (*end == '\0') {
					break;
				}
			} else {
				escaped = false;
			}
			++end;
		}
	}
	argv[*argc] = NULL;
	return argv;
}

void free_argv(int argc, char **argv) {
	for (int i = 0; i < argc; ++i) {
		free(argv[i]);
	}
	free(argv);
}

char *join_args(char **argv, int argc) {
	size_t len = 0;
	for (int i = 0; i < argc; ++i) {
		len += strlen(argv[i]) + 1;
	}
	char *res = malloc(len + 1);
	if (!res) {
		return NULL;
	}
	res[0] = '\0';
	for (int i = 0; i < argc; ++i) {
		if (i > 0) {
			strcat(res, " ");
		}
		strcat(res, argv[i]);
	}
	return res;
}

char *argsep(char **stringp, const char *delim, char *matched) {
	char *start = *stringp;
	char *end = start;
	bool in_string = false;
	bool in_char = false;
	bool escaped = false;
	char *first = NULL;
	if (matched) {
		*matched = '\0';
	}
	if (!start) {
		return NULL;
	}
	while (*end) {
		if (*end == '"' && !in_char && !escaped) {
			in_string = !in_string;
		} else if (*end == '\'' && !in_string && !escaped) {
			in_char = !in_char;
		} else if (*end == '\\') {
			escaped = !escaped;
		} else if (!in_string && !in_char && !escaped && strchr(delim, *end)) {
			first = end;
			break;
		} else {
			escaped = false;
		}
		++end;
	}
	if (first) {
		if (matched) {
			*matched = *first;
		}
		*first = '\0';
		*stringp = first + 1;
	} else {
		*stringp = NULL;
	}
	return start;
}

char *format_str(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	int length = vsnprintf(NULL, 0, fmt, args);
	va_end(args);
	if (length < 0) {
		return NULL;
	}
	char *buf = malloc((size_t)length + 1);
	if (!buf) {
		return NULL;
	}
	va_start(args, fmt);
	vsnprintf(buf, (size_t)length + 1, fmt, args);
	va_end(args);
	return buf;
}

bool parse_boolean(const char *boolean, bool current) {
	if (swine_strcasecmp(boolean, "1") == 0
			|| swine_strcasecmp(boolean, "yes") == 0
			|| swine_strcasecmp(boolean, "on") == 0
			|| swine_strcasecmp(boolean, "true") == 0
			|| swine_strcasecmp(boolean, "enable") == 0
			|| swine_strcasecmp(boolean, "enabled") == 0
			|| swine_strcasecmp(boolean, "active") == 0) {
		return true;
	} else if (swine_strcasecmp(boolean, "toggle") == 0) {
		return !current;
	}
	return false;
}
