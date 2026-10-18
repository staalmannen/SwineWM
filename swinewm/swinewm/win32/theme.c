// windows_theme: Windows' light/dark mode, which Windows' own dialogs and
// most applications follow, set from the sway config. These are the user's
// "Choose your mode" settings (HKCU ...\Themes\Personalize,
// AppsUseLightTheme and SystemUsesLightTheme). swinewm saves the user's
// values first and puts them back on exit, after a crash and with --recover
// (from the "theme-changed" file next to the recovery file).
#include <stdio.h>
#include <stdlib.h>
#include "fileutil.h"
#include "log.h"
#include "swinewm/platform.h"
#include "win32.h"

static const wchar_t personalize_key[] =
	L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

static bool changed = false;
static DWORD saved_apps = 1, saved_system = 1;
static HANDLE notify_thread = NULL;
// The config is read before the platform starts (and recovers from a
// crash, which would undo the change): apply it in win32_theme_init.
static bool started = false;
static int pending_mode = -1;

static bool read_value(const wchar_t *name, DWORD *value) {
	DWORD size = sizeof(*value);
	return RegGetValueW(HKEY_CURRENT_USER, personalize_key, name,
		RRF_RT_REG_DWORD, NULL, value, &size) == ERROR_SUCCESS;
}

static void write_value(const wchar_t *name, DWORD value) {
	HKEY key;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, personalize_key, 0, NULL, 0,
			KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
		RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&value,
			sizeof(value));
		RegCloseKey(key);
	}
}

// Telling every window about the change sends messages and waits, so it
// happens on a thread of its own; hung windows are skipped.
static DWORD WINAPI notify_proc(LPVOID param) {
	(void)param;
	SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
		(LPARAM)L"ImmersiveColorSet", SMTO_ABORTIFHUNG, 2000, NULL);
	return 0;
}

static void notify_all(void) {
	if (notify_thread) {
		CloseHandle(notify_thread);
	}
	notify_thread = CreateThread(NULL, 0, notify_proc, NULL, 0, NULL);
}

static void set_mode(DWORD apps, DWORD system) {
	write_value(L"AppsUseLightTheme", apps);
	write_value(L"SystemUsesLightTheme", system);
	notify_all();
}

void platform_set_windows_theme(int mode) {
	if (!started) {
		pending_mode = mode;
		return;
	}
	if (mode < 0) {
		win32_theme_restore();
		return;
	}
	if (!changed) {
		read_value(L"AppsUseLightTheme", &saved_apps);
		read_value(L"SystemUsesLightTheme", &saved_system);
		char *path = win32_state_path("theme-changed", true);
		FILE *f = path ? swine_fopen(path, "w") : NULL;
		if (f) {
			fprintf(f, "%lu %lu\n", (unsigned long)saved_apps,
				(unsigned long)saved_system);
			fclose(f);
		}
		free(path);
		changed = true;
	}
	DWORD light = mode == 0 ? 1 : 0;
	set_mode(light, light);
	swine_log(SWINE_INFO, "Windows set to %s mode", light ? "light" : "dark");
}

void win32_theme_init(void) {
	started = true;
	if (pending_mode >= 0) {
		platform_set_windows_theme(pending_mode);
	}
}

static void forget_marker(void) {
	char *path = win32_state_path("theme-changed", false);
	if (path) {
		wchar_t *wpath = utf8_to_utf16(path);
		DeleteFileW(wpath);
		free(wpath);
		free(path);
	}
}

void win32_theme_restore(void) {
	if (!changed) {
		return;
	}
	changed = false;
	set_mode(saved_apps, saved_system);
	forget_marker();
	if (notify_thread) {
		// On exit: let the windows hear about it before the process ends.
		WaitForSingleObject(notify_thread, 2500);
	}
}

void win32_theme_recover(void) {
	char *path = win32_state_path("theme-changed", false);
	FILE *f = path ? swine_fopen(path, "r") : NULL;
	free(path);
	if (!f) {
		return;
	}
	unsigned long apps = 1, system = 1;
	if (fscanf(f, "%lu %lu", &apps, &system) == 2) {
		set_mode((DWORD)apps, (DWORD)system);
		swine_log(SWINE_INFO, "Restored the Windows light/dark mode");
		if (notify_thread) {
			WaitForSingleObject(notify_thread, 2500);
		}
	}
	fclose(f);
	forget_marker();
}
