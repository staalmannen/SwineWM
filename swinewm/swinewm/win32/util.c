#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "list.h"
#include "stringop.h"
#include "swinewm/platform.h"
#include "win32.h"

wchar_t *utf8_to_utf16(const char *str) {
	if (!str) {
		return NULL;
	}
	int n = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
	if (n <= 0) {
		return NULL;
	}
	wchar_t *out = malloc(sizeof(wchar_t) * (size_t)n);
	MultiByteToWideChar(CP_UTF8, 0, str, -1, out, n);
	return out;
}

char *utf16_to_utf8(const wchar_t *str) {
	if (!str) {
		return NULL;
	}
	int n = WideCharToMultiByte(CP_UTF8, 0, str, -1, NULL, 0, NULL, NULL);
	if (n <= 0) {
		return NULL;
	}
	char *out = malloc((size_t)n);
	WideCharToMultiByte(CP_UTF8, 0, str, -1, out, n, NULL, NULL);
	return out;
}

char *win32_error_string(DWORD error) {
	wchar_t *buf = NULL;
	FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error, 0, (LPWSTR)&buf, 0, NULL);
	char *msg = buf ? utf16_to_utf8(buf) : format_str("error %lu", error);
	if (buf) {
		LocalFree(buf);
	}
	if (msg) {
		strip_whitespace(msg);
	}
	return msg;
}

static char *get_env(const wchar_t *name) {
	DWORD n = GetEnvironmentVariableW(name, NULL, 0);
	if (n == 0) {
		return NULL;
	}
	wchar_t *buf = malloc(sizeof(wchar_t) * n);
	GetEnvironmentVariableW(name, buf, n);
	char *out = utf16_to_utf8(buf);
	free(buf);
	return out;
}

static char *exe_dir(void) {
	wchar_t path[MAX_PATH];
	DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
	if (n == 0 || n == MAX_PATH) {
		return NULL;
	}
	wchar_t *slash = wcsrchr(path, L'\\');
	if (slash) {
		*slash = L'\0';
	}
	return utf16_to_utf8(path);
}

list_t *platform_config_paths(void) {
	list_t *paths = create_list();
	char *profile = get_env(L"USERPROFILE");
	char *xdg = get_env(L"XDG_CONFIG_HOME");
	char *appdata = get_env(L"APPDATA");
	char *dir = exe_dir();
	if (profile) {
		list_add(paths, format_str("%s\\.swinewm\\config", profile));
	}
	if (xdg) {
		list_add(paths, format_str("%s\\swinewm\\config", xdg));
	}
	if (appdata) {
		list_add(paths, format_str("%s\\swinewm\\config", appdata));
	}
	if (dir) {
		list_add(paths, format_str("%s\\config", dir));
	}
	free(profile);
	free(xdg);
	free(appdata);
	free(dir);
	return paths;
}

char *platform_expand_path(const char *path) {
	char *tilde = NULL;
	if (path[0] == '~' && (path[1] == '\0' || path[1] == '/' || path[1] == '\\')) {
		char *profile = get_env(L"USERPROFILE");
		tilde = format_str("%s%s", profile ? profile : "", path + 1);
		free(profile);
		path = tilde;
	}
	wchar_t *wide = utf8_to_utf16(path);
	DWORD n = ExpandEnvironmentStringsW(wide, NULL, 0);
	char *result = NULL;
	if (n > 0) {
		wchar_t *buf = malloc(sizeof(wchar_t) * n);
		ExpandEnvironmentStringsW(wide, buf, n);
		result = utf16_to_utf8(buf);
		free(buf);
	}
	free(wide);
	if (!result) {
		result = swine_strdup(path);
	}
	free(tilde);
	return result;
}
