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

// After "restart", wait until the previous instance has exited: it still
// owns the IPC pipe and its hooks until then.
static void wait_for_previous_instance(void) {
	wchar_t buf[16];
	DWORD n = GetEnvironmentVariableW(L"SWINEWM_RESTART_PID", buf, 16);
	if (n == 0 || n >= 16) {
		return;
	}
	SetEnvironmentVariableW(L"SWINEWM_RESTART_PID", NULL);
	HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)wcstoul(buf, NULL, 10));
	if (old) {
		WaitForSingleObject(old, 10000);
		CloseHandle(old);
	}
}

void platform_setup_environment(void) {
	wait_for_previous_instance();
	// The log is UTF-8 (window titles); the console defaults to the OEM
	// code page, which shows "Vänner" as "V├ñnner".
	SetConsoleOutputCP(CP_UTF8);
	wchar_t path[MAX_PATH];
	DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
	if (n == 0 || n == MAX_PATH) {
		return;
	}
	wchar_t *slash = wcsrchr(path, L'\\');
	if (slash) {
		*slash = L'\0';
	}
	// Inherited by exec'd programs too, like SWAYSOCK.
	SetEnvironmentVariableW(L"SWINEWM_DATA", path);
	win32_shell_detect();
}

list_t *platform_config_paths(const char *name) {
	char *native = swine_strdup(name);
	for (char *c = native; *c; ++c) {
		if (*c == '/') {
			*c = '\\';
		}
	}
	list_t *paths = create_list();
	char *profile = get_env(L"USERPROFILE");
	char *xdg = get_env(L"XDG_CONFIG_HOME");
	char *appdata = get_env(L"APPDATA");
	char *dir = exe_dir();
	if (profile) {
		list_add(paths, format_str("%s\\.swinewm\\%s", profile, native));
	}
	if (xdg) {
		list_add(paths, format_str("%s\\swinewm\\%s", xdg, native));
	}
	if (appdata) {
		list_add(paths, format_str("%s\\swinewm\\%s", appdata, native));
	}
	if (dir) {
		list_add(paths, format_str("%s\\%s", dir, native));
	}
	free(profile);
	free(xdg);
	free(appdata);
	free(dir);
	free(native);
	return paths;
}

list_t *platform_list_dir(const char *dir) {
	char *pattern = format_str("%s\\*", dir);
	wchar_t *wpattern = utf8_to_utf16(pattern);
	free(pattern);
	WIN32_FIND_DATAW data;
	HANDLE find = FindFirstFileExW(wpattern, FindExInfoBasic, &data,
		FindExSearchNameMatch, NULL, 0);
	free(wpattern);
	if (find == INVALID_HANDLE_VALUE) {
		return NULL;
	}
	list_t *names = create_list();
	do {
		if (wcscmp(data.cFileName, L".") != 0 &&
				wcscmp(data.cFileName, L"..") != 0) {
			list_add(names, utf16_to_utf8(data.cFileName));
		}
	} while (FindNextFileW(find, &data));
	FindClose(find);
	return names;
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
