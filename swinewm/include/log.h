#ifndef _SWINE_LOG_H
#define _SWINE_LOG_H

#include <stdarg.h>
#include <stdbool.h>

enum swine_log_importance {
	SWINE_SILENT = 0,
	SWINE_ERROR = 1,
	SWINE_INFO = 2,
	SWINE_DEBUG = 3,
};

void swine_log_init(enum swine_log_importance verbosity);
enum swine_log_importance swine_log_get_verbosity(void);

void _swine_log(enum swine_log_importance verbosity, const char *file, int line,
		const char *format, ...);
bool _swine_assert(bool condition, const char *file, int line,
		const char *format, ...);

// The format string is part of __VA_ARGS__ so that a call without extra
// arguments needs no comma-elision extension (MSVC and C11 compatible).
#define swine_log(verb, ...) _swine_log(verb, __FILE__, __LINE__, __VA_ARGS__)
#define swine_assert(cond, ...) _swine_assert(cond, __FILE__, __LINE__, __VA_ARGS__)

#endif
