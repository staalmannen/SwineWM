// Crash safety for hidden workspaces.
//
// Windows on inactive workspaces are hidden with SW_HIDE. If swinewm dies
// without cleaning up they would stay invisible (and absent from the
// taskbar). The handles of currently hidden windows are therefore written to
// %LOCALAPPDATA%\swinewm\hidden-windows.txt (handle and process id);
// "swinewm --recover" and the next
// start of swinewm show them again.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "fileutil.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/platform.h"
#include "swinewm/server.h"
#include "swinewm/tree/view.h"
#include "win32.h"

static bool save_pending = false;

static char *state_dir(void) {
	wchar_t buf[MAX_PATH];
	DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) {
		return NULL;
	}
	char *base = utf16_to_utf8(buf);
	char *dir = format_str("%s\\swinewm", base);
	free(base);
	return dir;
}

static char *state_file(bool create_dir) {
	char *dir = state_dir();
	if (!dir) {
		return NULL;
	}
	if (create_dir) {
		wchar_t *wdir = utf8_to_utf16(dir);
		CreateDirectoryW(wdir, NULL);
		free(wdir);
	}
	char *file = format_str("%s\\hidden-windows.txt", dir);
	free(dir);
	return file;
}

void win32_recover_schedule_save(void) {
	if (!save_pending) {
		save_pending = true;
		PostMessageW(win32_shell_hwnd, WM_SWINE_SAVE_HIDDEN, 0, 0);
	}
}

void win32_recover_save(void) {
	save_pending = false;
	char *path = state_file(true);
	if (!path) {
		return;
	}
	FILE *f = swine_fopen(path, "w");
	free(path);
	if (!f) {
		return;
	}
	for (int i = 0; i < server.views->length; ++i) {
		struct swine_view *view = server.views->items[i];
		if (view->container && !view->current.visible) {
			fprintf(f, "%llx %lu\n", (unsigned long long)(size_t)view->handle,
				view->pid);
		}
	}
	fclose(f);
}

void win32_recover_clear(void) {
	char *path = state_file(false);
	if (path) {
		wchar_t *wpath = utf8_to_utf16(path);
		DeleteFileW(wpath);
		free(wpath);
		free(path);
	}
}

static char *display_marker(bool create_dir) {
	char *file = state_file(create_dir);
	if (!file) {
		return NULL;
	}
	// "...\hidden-windows.txt" -> "...\display-changed"
	char *slash = strrchr(file, '\\');
	char *marker = format_str("%.*s\\display-changed",
		slash ? (int)(slash - file) : 0, file);
	free(file);
	return marker;
}

void win32_recover_note_display_changed(void) {
	char *path = display_marker(true);
	if (!path) {
		return;
	}
	FILE *f = swine_fopen(path, "w");
	if (f) {
		fputs("swinewm changed the display settings\n", f);
		fclose(f);
	}
	free(path);
}

static void recover_display_settings(void) {
	char *path = display_marker(false);
	if (!path) {
		return;
	}
	wchar_t *wpath = utf8_to_utf16(path);
	if (GetFileAttributesW(wpath) != INVALID_FILE_ATTRIBUTES) {
		ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
		swine_log(SWINE_INFO, "Restored the display settings");
		DeleteFileW(wpath);
	}
	free(wpath);
	free(path);
}

void win32_recover_display_done(void) {
	char *path = display_marker(false);
	if (path) {
		wchar_t *wpath = utf8_to_utf16(path);
		DeleteFileW(wpath);
		free(wpath);
		free(path);
	}
}

int platform_recover_windows(void) {
	recover_display_settings();
	if (win32_shell_recover_taskbar()) {
		swine_log(SWINE_INFO, "Restored Explorer's taskbar setting");
	}
	// Tray icons that were shown by a crashed swinewm go back to Explorer.
	win32_tray_recover();
	char *path = state_file(false);
	if (!path) {
		return 0;
	}
	FILE *f = swine_fopen(path, "r");
	free(path);
	if (!f) {
		return 0;
	}
	int restored = 0;
	unsigned long long value;
	unsigned long pid;
	while (fscanf(f, "%llx %lu", &value, &pid) == 2) {
		HWND hwnd = (HWND)(size_t)value;
		// Window handles are recycled: only touch the window if it still
		// belongs to the same process.
		DWORD owner = 0;
		if (IsWindow(hwnd) && GetWindowThreadProcessId(hwnd, &owner) &&
				owner == pid && !IsWindowVisible(hwnd)) {
			ShowWindowAsync(hwnd, SW_SHOWNA);
			restored++;
		}
	}
	fclose(f);
	win32_recover_clear();
	return restored;
}
