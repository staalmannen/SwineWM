// Running as the Windows shell, and living next to Explorer.
//
// Most of what is here follows ManagedShell, the library behind Cairo
// Desktop (https://github.com/cairoshell/ManagedShell, Apache-2.0):
// shell detection (ShellHelper/CairoApplication), the shell-ready event
// (ShellHelper.SetShellReadyEvent), the shell window (ShellWindow), startup
// programs (StartupRunner), the work area (AppBarManager.SetWorkArea) and
// hiding Explorer's taskbar (ExplorerHelper). See doc/research/windows-shell.md.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "fileutil.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/config.h"
#include "swinewm/platform.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "win32.h"
#include <shellapi.h>

bool win32_shell_mode = false;
static int shell_mode_override = -1;
static HWND desktop_hwnd = NULL;

void platform_set_shell_mode(int mode) {
	shell_mode_override = mode;
}

bool platform_is_shell(void) {
	return win32_shell_mode;
}

static char *local_state_path(const char *file) {
	wchar_t buf[MAX_PATH];
	DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) {
		return NULL;
	}
	char *base = utf16_to_utf8(buf);
	char *dir = format_str("%s\\swinewm", base);
	wchar_t *wdir = utf8_to_utf16(dir);
	CreateDirectoryW(wdir, NULL);
	free(wdir);
	char *path = format_str("%s\\%s", dir, file);
	free(dir);
	free(base);
	return path;
}

void win32_shell_detect(void) {
	// Like Cairo: if no shell window exists at start-up, we are the shell.
	bool other_shell = GetShellWindow() != NULL;
	win32_shell_mode = shell_mode_override >= 0 ? shell_mode_override == 1 :
		!other_shell;
	if (win32_shell_mode && other_shell) {
		// Two shells fight over the work area and the desktop window.
		swine_log(SWINE_ERROR, "Another shell (Explorer) is running; not "
			"acting as the shell. Use 'explorer_taskbar autohide' to run "
			"next to Explorer.");
		win32_shell_mode = false;
	}
	if (!win32_shell_mode) {
		return;
	}
	// Started by Winlogon, Windows opens a console just for us that nobody
	// reads: log to a file and close it. Started from a terminal (the
	// console has other processes), keep logging there.
	DWORD pids[2];
	// (0 processes: started without any console, e.g. by "restart".)
	if (GetConsoleProcessList(pids, 2) <= 1) {
		char *path = local_state_path("swinewm.log");
		char *old_path = local_state_path("swinewm.old.log");
		if (path && old_path) {
			wchar_t *wpath = utf8_to_utf16(path);
			wchar_t *wold = utf8_to_utf16(old_path);
			// Keep the previous session's log: after a crash or a
			// "restart" that is the interesting one.
			MoveFileExW(wpath, wold, MOVEFILE_REPLACE_EXISTING);
			if (_wfreopen(wpath, L"w", stderr)) {
				FreeConsole();
				swine_log_set_colors(false);
			}
			free(wold);
			free(wpath);
		}
		free(old_path);
		free(path);
	}
	swine_log(SWINE_INFO, "Running as the Windows shell");
}

// --- the shell window ---------------------------------------------------

static LRESULT CALLBACK desktop_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
		LPARAM lparam) {
	switch (msg) {
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		// Black, then the wallpapers ("output * bg"): drawing them here
		// instead of in windows of their own keeps them below everything.
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		RECT r;
		GetClientRect(hwnd, &r);
		win32_fill(dc, &r, RGB(0, 0, 0));
		win32_wallpaper_paint(dc, GetSystemMetrics(SM_XVIRTUALSCREEN),
			GetSystemMetrics(SM_YVIRTUALSCREEN));
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	case WM_WINDOWPOSCHANGING: {
		// Stay at the bottom, below everything else (wallpapers included).
		WINDOWPOS *pos = (WINDOWPOS *)lparam;
		pos->hwndInsertAfter = HWND_BOTTOM;
		return 0;
	}
	default:
		return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
}

// Some programs (and parts of Windows) look for the shell through
// GetShellWindow(). Without Explorer we register a desktop window of our
// own, as ManagedShell's ShellWindow does. SetShellWindow is exported by
// user32 but not declared in the SDK, so it is looked up at run time.
static void create_shell_window(void) {
	HINSTANCE instance = GetModuleHandleW(NULL);
	WNDCLASSEXW wc = { .cbSize = sizeof(wc) };
	wc.lpfnWndProc = desktop_wndproc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.lpszClassName = L"swinewm-desktop";
	RegisterClassExW(&wc);
	int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
	int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
	int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
	// Created hidden and then shown at the bottom: a window created visible
	// starts at the top of the Z order, above the wallpapers.
	desktop_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		L"swinewm-desktop", L"swinewm desktop", WS_POPUP, x, y, w, h, NULL,
		NULL, instance, NULL);
	if (desktop_hwnd) {
		SetWindowPos(desktop_hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE |
			SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
	}
	typedef BOOL (WINAPI *set_shell_window_fn)(HWND);
	set_shell_window_fn set_shell_window = (set_shell_window_fn)(void *)
		GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetShellWindow");
	if (desktop_hwnd && set_shell_window && set_shell_window(desktop_hwnd)) {
		swine_log(SWINE_DEBUG, "Registered the shell window");
	} else {
		swine_log(SWINE_INFO, "Unable to register as the shell window");
	}
}

HWND win32_shell_desktop_window(void) {
	return desktop_hwnd;
}

void win32_shell_resize_desktop(void) {
	if (desktop_hwnd) {
		SetWindowPos(desktop_hwnd, HWND_BOTTOM,
			GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
			GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
			SWP_NOACTIVATE);
	}
}

// --- startup programs ---------------------------------------------------

// HKxx\...\Explorer\StartupApproved\*: the first byte of a value is odd
// when the user disabled that entry in Task Manager.
static bool startup_disabled(HKEY hive, const wchar_t *approved_key,
		const wchar_t *name) {
	if (!approved_key) {
		return false;
	}
	BYTE data[16];
	DWORD size = sizeof(data);
	if (RegGetValueW(hive, approved_key, name, RRF_RT_REG_BINARY, NULL, data,
			&size) != ERROR_SUCCESS || size == 0) {
		return false;
	}
	return (data[0] % 2) != 0;
}

static void run_registry_key(HKEY hive, const wchar_t *path,
		const wchar_t *approved_key, bool run_once) {
	HKEY key;
	REGSAM access = KEY_READ | (run_once ? KEY_SET_VALUE : 0);
	if (RegOpenKeyExW(hive, path, 0, access, &key) != ERROR_SUCCESS) {
		return;
	}
	for (DWORD i = 0;; ++i) {
		wchar_t name[256];
		DWORD name_len = 256;
		wchar_t value[2048];
		DWORD value_size = sizeof(value) - sizeof(wchar_t);
		DWORD type = 0;
		LONG r = RegEnumValueW(key, i, name, &name_len, NULL, &type,
			(BYTE *)value, &value_size);
		if (r == ERROR_NO_MORE_ITEMS) {
			break;
		}
		if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
			continue;
		}
		value[value_size / sizeof(wchar_t)] = L'\0';
		if (startup_disabled(hive, approved_key, name)) {
			continue;
		}
		char *cmd = utf16_to_utf8(value);
		swine_log(SWINE_INFO, "Startup program: %s", cmd);
		platform_spawn(cmd);
		free(cmd);
		if (run_once) {
			// RunOnce entries are deleted once started.
			RegDeleteValueW(key, name);
			--i;
		}
	}
	RegCloseKey(key);
}

static void run_startup_folder(const wchar_t *folder_var, HKEY approved_root) {
	wchar_t folder[MAX_PATH];
	if (!ExpandEnvironmentStringsW(folder_var, folder, MAX_PATH)) {
		return;
	}
	wchar_t pattern[MAX_PATH + 4];
	swprintf(pattern, MAX_PATH + 4, L"%ls\\*", folder);
	WIN32_FIND_DATAW fd;
	HANDLE find = FindFirstFileW(pattern, &fd);
	if (find == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
				FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) {
			continue;   // ".", "..", desktop.ini
		}
		if (startup_disabled(approved_root, L"Software\\Microsoft\\Windows\\"
				L"CurrentVersion\\Explorer\\StartupApproved\\StartupFolder",
				fd.cFileName)) {
			continue;
		}
		wchar_t path[MAX_PATH * 2];
		swprintf(path, MAX_PATH * 2, L"%ls\\%ls", folder, fd.cFileName);
		char *name = utf16_to_utf8(path);
		swine_log(SWINE_INFO, "Startup item: %s", name);
		free(name);
		// .lnk shortcuts and other documents need the shell to open them.
		ShellExecuteW(NULL, L"open", path, NULL, folder, SW_SHOWNORMAL);
	} while (FindNextFileW(find, &fd));
	FindClose(find);
}

// The user's startup programs, which Explorer would otherwise start. Only
// once per logon session, so restarting swinewm does not start them again:
// a volatile registry key disappears at log-off.
static void run_startup_programs(void) {
	HKEY key;
	DWORD disposition = 0;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\SwineWM\\Session", 0,
			NULL, REG_OPTION_VOLATILE, KEY_READ, NULL, &key,
			&disposition) != ERROR_SUCCESS) {
		return;
	}
	RegCloseKey(key);
	if (disposition != REG_CREATED_NEW_KEY) {
		swine_log(SWINE_INFO, "Startup programs already ran in this session");
		return;
	}
	const wchar_t *run = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
	const wchar_t *run32 =
		L"Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Run";
	const wchar_t *approved =
		L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
	const wchar_t *approved32 =
		L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run32";
	const wchar_t *policy =
		L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run";
	const wchar_t *run_once =
		L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
	HKEY roots[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
	for (int i = 0; i < 2; ++i) {
		run_registry_key(roots[i], run, approved, false);
		run_registry_key(roots[i], run32, approved32, false);
		run_registry_key(roots[i], policy, NULL, false);
	}
	// HKLM RunOnce entries cannot be deleted without administrator rights,
	// so they would run at every logon; like ManagedShell, skip them.
	run_registry_key(HKEY_CURRENT_USER, run_once, NULL, true);
	run_startup_folder(L"%ProgramData%\\Microsoft\\Windows\\Start Menu\\Programs\\Startup",
		HKEY_LOCAL_MACHINE);
	run_startup_folder(L"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Startup",
		HKEY_CURRENT_USER);
}

void win32_shell_start(void) {
	if (!win32_shell_mode) {
		return;
	}
	create_shell_window();
	// Lets Windows finish the logon (welcome screen, delayed services).
	HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, TRUE, L"ShellDesktopSwitchEvent");
	if (ready) {
		SetEvent(ready);
		CloseHandle(ready);
	}
	run_startup_programs();
}

// --- work area ----------------------------------------------------------

// As the shell, swinewm owns the work area that maximized and unmanaged
// windows respect: the monitor minus the bar. With Explorer running the
// taskbar owns it and swinewm only reserves the bar inside its own layout.
void win32_shell_update_work_area(bool reset) {
	if (!win32_shell_mode) {
		return;
	}
	bool changed = false;
	for (int i = 0; i < root->outputs->length; ++i) {
		struct swine_output *o = root->outputs->items[i];
		struct swine_box area = reset ? o->box : output_get_usable_area(o);
		RECT want = {area.x, area.y, area.x + area.width, area.y + area.height};
		MONITORINFO mi = { .cbSize = sizeof(mi) };
		if (o->handle && GetMonitorInfoW(o->handle, &mi) &&
				EqualRect(&mi.rcWork, &want)) {
			continue;
		}
		// SPIF_SENDCHANGE would broadcast with SendMessage and wait for
		// every window (ours included, re-entering the layout code); the
		// change is announced below without waiting instead.
		SystemParametersInfoW(SPI_SETWORKAREA, 0, &want, 0);
		changed = true;
	}
	if (changed) {
		PostMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, SPI_SETWORKAREA, 0);
	}
}

// --- Explorer's taskbar -------------------------------------------------

// "explorer_taskbar autohide": while swinewm runs next to Explorer, its
// taskbar is switched to auto-hide and kept hidden; the original setting is
// restored on exit, after a crash, or by "swinewm --recover".
static LPARAM saved_taskbar_state = -1;

// With the tray module, FindWindow("Shell_TrayWnd") finds swinewm's own.
HWND win32_find_explorer_tray(void) {
	HWND hwnd = NULL;
	while ((hwnd = FindWindowExW(NULL, hwnd, L"Shell_TrayWnd", NULL))) {
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid != GetCurrentProcessId()) {
			return hwnd;
		}
	}
	return NULL;
}

static void set_taskbar_windows_visible(bool visible) {
	HWND tray = win32_find_explorer_tray();
	if (tray && (IsWindowVisible(tray) != FALSE) != visible) {
		ShowWindowAsync(tray, visible ? SW_SHOWNA : SW_HIDE);
	}
	HWND secondary = NULL;
	while ((secondary = FindWindowExW(NULL, secondary,
			L"Shell_SecondaryTrayWnd", NULL))) {
		if ((IsWindowVisible(secondary) != FALSE) != visible) {
			ShowWindowAsync(secondary, visible ? SW_SHOWNA : SW_HIDE);
		}
	}
}

static void set_taskbar_state(LPARAM state) {
	APPBARDATA abd = { .cbSize = sizeof(abd) };
	abd.hWnd = win32_find_explorer_tray();
	abd.lParam = state;
	if (abd.hWnd) {
		SHAppBarMessage(ABM_SETSTATE, &abd);
	}
}

static void restore_taskbar(LPARAM state) {
	set_taskbar_state(state);
	set_taskbar_windows_visible(true);
	char *path = local_state_path("taskbar-state.txt");
	if (path) {
		wchar_t *wpath = utf8_to_utf16(path);
		DeleteFileW(wpath);
		free(wpath);
		free(path);
	}
}

void win32_shell_apply_taskbar(void) {
	bool hide = !win32_shell_mode && config && config->explorer_taskbar_autohide;
	if (!hide) {
		if (saved_taskbar_state >= 0) {
			restore_taskbar(saved_taskbar_state);
			saved_taskbar_state = -1;
		}
		return;
	}
	HWND tray = win32_find_explorer_tray();
	if (!tray) {
		return;
	}
	if (saved_taskbar_state < 0) {
		APPBARDATA abd = { .cbSize = sizeof(abd), .hWnd = tray };
		saved_taskbar_state = (LPARAM)SHAppBarMessage(ABM_GETSTATE, &abd);
		char *path = local_state_path("taskbar-state.txt");
		FILE *f = path ? swine_fopen(path, "w") : NULL;
		if (f) {
			fprintf(f, "%ld\n", (long)saved_taskbar_state);
			fclose(f);
		}
		free(path);
		set_taskbar_state(ABS_AUTOHIDE);
	}
	// Explorer shows it again on some occasions (Start menu): re-hide on
	// every refresh, as ManagedShell does with a timer.
	set_taskbar_windows_visible(false);
}

void win32_shell_restore_taskbar(void) {
	if (saved_taskbar_state >= 0) {
		restore_taskbar(saved_taskbar_state);
		saved_taskbar_state = -1;
	}
}

bool win32_shell_recover_taskbar(void) {
	char *path = local_state_path("taskbar-state.txt");
	FILE *f = path ? swine_fopen(path, "r") : NULL;
	free(path);
	if (!f) {
		return false;
	}
	long state = 0;
	bool ok = fscanf(f, "%ld", &state) == 1;
	fclose(f);
	if (ok) {
		restore_taskbar((LPARAM)state);
	}
	return ok;
}

// --- leaving ------------------------------------------------------------

void win32_shell_finish(void) {
	win32_shell_restore_taskbar();
	if (!win32_shell_mode) {
		return;
	}
	win32_shell_update_work_area(true);
	if (desktop_hwnd) {
		DestroyWindow(desktop_hwnd);
		desktop_hwnd = NULL;
	}
}

// sway's "exit" ends the session; as the Windows shell, so does ours.
void win32_shell_exit(void) {
	if (win32_shell_mode) {
		swine_log(SWINE_INFO, "Logging off");
		ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_MAJOR_OTHER |
			SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
	}
}

// After a crash as the shell, start Explorer so the user is not left on a
// black screen. It is started with a delay so that it finds no shell
// window (ours is gone by then) and becomes the shell itself.
void win32_shell_rescue(void) {
	if (!win32_shell_mode) {
		return;
	}
	wchar_t cmd[] = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & start explorer.exe";
	STARTUPINFOW si = { .cb = sizeof(si) };
	PROCESS_INFORMATION pi = {0};
	if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
			NULL, &si, &pi)) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}
}

// --- configuring Windows to start swinewm at logon ----------------------

#define WINLOGON_KEY L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon"

int platform_set_login_shell(bool enable) {
	HKEY key;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, WINLOGON_KEY, 0, NULL, 0,
			KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS) {
		fprintf(stderr, "Unable to open HKCU\\%ls\n", WINLOGON_KEY);
		return 1;
	}
	LONG r;
	if (enable) {
		wchar_t exe[MAX_PATH];
		DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
		wchar_t value[MAX_PATH + 2];
		swprintf(value, MAX_PATH + 2, L"\"%.*ls\"", (int)n, exe);
		r = RegSetValueExW(key, L"Shell", 0, REG_SZ, (const BYTE *)value,
			(DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
	} else {
		r = RegDeleteValueW(key, L"Shell");
		if (r == ERROR_FILE_NOT_FOUND) {
			r = ERROR_SUCCESS;
		}
	}
	RegCloseKey(key);
	if (r != ERROR_SUCCESS) {
		char *msg = win32_error_string((DWORD)r);
		fprintf(stderr, "Unable to change the login shell: %s\n", msg);
		free(msg);
		return 1;
	}
	if (enable) {
		printf("swinewm will replace Explorer as your shell from your next "
			"logon.\nTo undo: swinewm --unset-shell (or press Ctrl+Alt+Del, "
			"open Task Manager, run explorer.exe, then run that command).\n");
	} else {
		printf("Explorer will be your shell again from your next logon.\n");
	}
	return 0;
}
