#include <stdlib.h>
#include "fileutil.h"

#ifdef _WIN32
#include <windows.h>

static wchar_t *widen(const char *s) {
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	if (n <= 0) {
		return NULL;
	}
	wchar_t *w = malloc(sizeof(wchar_t) * (size_t)n);
	if (w) {
		MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
	}
	return w;
}

FILE *swine_fopen(const char *path, const char *mode) {
	wchar_t *wpath = widen(path);
	wchar_t *wmode = widen(mode);
	FILE *f = NULL;
	if (wpath && wmode) {
		f = _wfopen(wpath, wmode);
	}
	free(wpath);
	free(wmode);
	return f;
}
#else
FILE *swine_fopen(const char *path, const char *mode) {
	return fopen(path, mode);
}
#endif
