#include <stdio.h>
#include <string.h>
#include <time.h>
#include "log.h"

#ifdef _WIN32
#include <windows.h>
#endif

static enum swine_log_importance log_importance = SWINE_ERROR;
static bool use_colors = true;

static const char *verbosity_colors[] = {
	[SWINE_SILENT] = "",
	[SWINE_ERROR] = "\x1B[1;31m",
	[SWINE_INFO] = "\x1B[1;34m",
	[SWINE_DEBUG] = "\x1B[1;90m",
};

static const char *verbosity_headers[] = {
	[SWINE_SILENT] = "",
	[SWINE_ERROR] = "[ERROR]",
	[SWINE_INFO] = "[INFO]",
	[SWINE_DEBUG] = "[DEBUG]",
};

void swine_log_init(enum swine_log_importance verbosity) {
	if (verbosity <= SWINE_DEBUG) {
		log_importance = verbosity;
	}
#ifdef _WIN32
	// The color codes below need VT processing in the Windows console.
	HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
	DWORD mode = 0;
	if (err != INVALID_HANDLE_VALUE && GetConsoleMode(err, &mode)) {
		SetConsoleMode(err, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
	}
#endif
}

void swine_log_set_colors(bool enable) {
	use_colors = enable;
}

bool swine_log_parse_importance(const char *name,
		enum swine_log_importance *out) {
	static const char *names[] = {"silent", "error", "info", "debug"};
	for (int i = 0; name && i <= SWINE_DEBUG; ++i) {
		if (strcmp(name, names[i]) == 0) {
			*out = (enum swine_log_importance)i;
			return true;
		}
	}
	return false;
}

enum swine_log_importance swine_log_get_verbosity(void) {
	return log_importance;
}

static const char *strip_path(const char *file) {
	const char *slash = strrchr(file, '/');
	const char *backslash = strrchr(file, '\\');
	if (backslash > slash) {
		slash = backslash;
	}
	return slash ? slash + 1 : file;
}

static void log_va(enum swine_log_importance verbosity, const char *file,
		int line, const char *format, va_list args) {
	if (verbosity > log_importance) {
		return;
	}
	time_t now = time(NULL);
	struct tm *tm_info = localtime(&now);
	char stamp[32] = "";
	if (tm_info) {
		strftime(stamp, sizeof(stamp), "%H:%M:%S", tm_info);
	}
	unsigned c = verbosity <= SWINE_DEBUG ? (unsigned)verbosity : SWINE_DEBUG;
	fprintf(stderr, "%s %s%s [%s:%d] ", stamp,
		use_colors ? verbosity_colors[c] : "", verbosity_headers[c],
		strip_path(file), line);
	vfprintf(stderr, format, args);
	fprintf(stderr, "%s\n", use_colors ? "\x1B[0m" : "");
	fflush(stderr);
}

void _swine_log(enum swine_log_importance verbosity, const char *file, int line,
		const char *format, ...) {
	va_list args;
	va_start(args, format);
	log_va(verbosity, file, line, format, args);
	va_end(args);
}

bool _swine_assert(bool condition, const char *file, int line,
		const char *format, ...) {
	if (condition) {
		return true;
	}
	va_list args;
	va_start(args, format);
	log_va(SWINE_ERROR, file, line, format, args);
	va_end(args);
	return false;
}
