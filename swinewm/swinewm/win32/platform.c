// Win32 platform entry points: the shell's hidden window, the message loop,
// and start-up/shut-down ordering.
#include <stdlib.h>
#include <wchar.h>
#include "ipc.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/ipc-json.h"
#include "swinewm/notify.h"
#include "swinewm/platform.h"
#include "swinewm/server.h"
#include "win32.h"
#include <objbase.h>

HWND win32_shell_hwnd = NULL;
static bool keyboard_ok = false;
static bool mouse_ok = false;
static UINT shell_hook_message = 0;

// Timers on the shell window: a 1 s tick for the bar (clock, cpu, ...) and
// a one-shot timer for the next notification expiry.
#define TIMER_TICK 1
#define TIMER_NOTIFY 2
#define TIMER_DISPLAY 3

// Display settings can produce a burst of notifications while Windows and
// the display driver settle on the final monitor geometry and DPI.
#define DISPLAY_REFRESH_DELAY 150

static void refresh_display_state(void) {
	win32_shell_resize_desktop();
	win32_outputs_update();
	transaction_commit_dirty();
	char *event = ipc_json_event_output("unspecified");
	platform_ipc_event((uint32_t)IPC_EVENT_OUTPUT, event);
	free(event);
}

static void schedule_display_refresh(void) {
	if (!SetTimer(win32_shell_hwnd, TIMER_DISPLAY, DISPLAY_REFRESH_DELAY, NULL)) {
		swine_log(SWINE_ERROR, "Unable to schedule display refresh (error %lu)",
			GetLastError());
		refresh_display_state();
	}
}

void win32_run_command(const char *command) {
	win32_busy++;
	char *copy = swine_strdup(command);
	list_t *res_list = execute_command(copy, NULL);
	free(copy);
	for (int i = 0; i < res_list->length; ++i) {
		struct cmd_results *res = res_list->items[i];
		if (res->status != CMD_SUCCESS) {
			swine_log(SWINE_ERROR, "Command '%s' failed: %s", command,
				res->error ? res->error : "");
		}
	}
	free_cmd_results_list(res_list);
	transaction_commit_dirty();
	win32_busy--;
}

static LRESULT handle_shell_message(HWND hwnd, UINT msg, WPARAM wparam,
		LPARAM lparam) {
	switch (msg) {
	case WM_SWINE_BINDING: {
		char *command = (char *)lparam;
		win32_run_command(command);
		free(command);
		return 0;
	}
	case WM_SWINE_IPC:
		win32_ipc_handle((struct win32_ipc_request *)lparam);
		return 0;
	case WM_SWINE_EVENTS:
		win32_windows_drain_events();
		return 0;
	case WM_SWINE_SAVE_HIDDEN:
		win32_recover_save();
		return 0;
	case WM_SWINE_TRAY:
		win32_tray_handle_message(wparam, lparam);
		return 0;
	case WM_SWINE_DISPLAY_REFRESH:
		schedule_display_refresh();
		return 0;
	case WM_SWINE_MOTION:
		win32_mouse_handle_motion();
		return 0;
	case WM_DISPLAYCHANGE:
	case WM_DPICHANGED:
		schedule_display_refresh();
		return 0;
	case WM_SETTINGCHANGE:
		if (wparam == SPI_SETWORKAREA && !win32_shell_mode) {
			// The taskbar or an app bar changed size. As the shell we set
			// the work area ourselves and do not follow it.
			schedule_display_refresh();
		}
		return 0;
	case WM_TIMER:
		if (wparam == TIMER_DISPLAY) {
			KillTimer(hwnd, TIMER_DISPLAY);
			refresh_display_state();
		} else if (wparam == TIMER_TICK || wparam == TIMER_NOTIFY) {
			notify_tick(platform_now_ms());
			platform_ui_refresh();
		}
		return 0;
	case WM_CLOSE:
		PostQuitMessage(0);
		return 0;
	default:
		if (shell_hook_message && msg == shell_hook_message) {
			win32_windows_shell_hook(wparam, lparam);
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
}

static LRESULT CALLBACK shell_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
		LPARAM lparam) {
	win32_busy++;
	LRESULT result = handle_shell_message(hwnd, msg, wparam, lparam);
	win32_busy--;
	return result;
}

static BOOL WINAPI console_handler(DWORD type) {
	(void)type;
	// Runs on a separate thread: only post, and let the main loop clean up.
	if (win32_shell_hwnd) {
		PostMessageW(win32_shell_hwnd, WM_CLOSE, 0, 0);
		return TRUE;
	}
	return FALSE;
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS *info) {
	(void)info;
	// Best effort: never leave the user with invisible applications, a
	// hidden taskbar, or (as the shell) no shell at all.
	win32_windows_show_all();
	win32_decor_restore_all(1000);
	win32_theme_restore();
	win32_shell_restore_taskbar();
	win32_outputs_restore_settings();
	win32_tray_crash();
	win32_shell_rescue();
	return EXCEPTION_CONTINUE_SEARCH;
}

bool platform_init(void) {
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	// COM is needed by the Windows Imaging Component (wallpapers).
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

	int restored = platform_recover_windows();
	if (restored) {
		swine_log(SWINE_INFO, "Restored %d window(s) hidden by a previous run",
			restored);
	}

	HINSTANCE instance = GetModuleHandleW(NULL);
	WNDCLASSEXW wc = { .cbSize = sizeof(wc) };
	wc.lpfnWndProc = shell_wndproc;
	wc.hInstance = instance;
	wc.lpszClassName = L"swinewm";
	if (!RegisterClassExW(&wc)) {
		swine_log(SWINE_ERROR, "RegisterClassEx failed");
		return false;
	}
	// A hidden top-level window rather than a message-only window, because
	// only top-level windows receive WM_DISPLAYCHANGE / WM_SETTINGCHANGE.
	win32_shell_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"swinewm", L"swinewm",
		WS_POPUP, 0, 0, 0, 0, NULL, NULL, instance, NULL);
	if (!win32_shell_hwnd) {
		swine_log(SWINE_ERROR, "CreateWindowEx failed");
		return false;
	}

	SetConsoleCtrlHandler(console_handler, TRUE);
	SetUnhandledExceptionFilter(crash_handler);

	if (!win32_ipc_init()) {
		return false;
	}
	win32_outputs_update();
	if (!win32_windows_init()) {
		swine_log(SWINE_ERROR, "Unable to install window event hooks");
		return false;
	}
	keyboard_ok = win32_keyboard_init();
	mouse_ok = win32_mouse_init();
	// Shell hook messages: urgency (HSHELL_FLASH) and fullscreen changes.
	shell_hook_message = RegisterWindowMessageW(L"SHELLHOOK");
	if (!RegisterShellHookWindow(win32_shell_hwnd)) {
		shell_hook_message = 0;
	}
	win32_windows_manage_existing();
	SetTimer(win32_shell_hwnd, TIMER_TICK, 1000, NULL);
	win32_shell_start();
	win32_theme_init();   // after the crash recovery above
	return true;
}

void platform_ui_refresh(void) {
	if (!win32_shell_hwnd) {
		return;
	}
	win32_shell_update_work_area(false);
	win32_shell_apply_taskbar();
	win32_tray_update();
	win32_wallpaper_refresh();
	win32_bar_refresh();
	win32_notify_refresh();
	long long next = notify_next_expiry();
	if (next) {
		long long delay = next - platform_now_ms();
		SetTimer(win32_shell_hwnd, TIMER_NOTIFY,
			(UINT)(delay < 10 ? 10 : delay), NULL);
	} else {
		KillTimer(win32_shell_hwnd, TIMER_NOTIFY);
	}
}

long long platform_now_ms(void) {
	return (long long)GetTickCount64();
}

int platform_run(void) {
	MSG msg;
	BOOL result;
	while ((result = GetMessageW(&msg, NULL, 0, 0)) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	return result == -1 ? EXIT_FAILURE : (int)msg.wParam;
}

void platform_quit(void) {
	platform_ipc_event((uint32_t)IPC_EVENT_SHUTDOWN, "{\"change\":\"exit\"}");
	win32_shell_exit();
	PostQuitMessage(0);
}

static bool restart_requested = false;

void platform_restart(void) {
	swine_log(SWINE_INFO, "Restarting");
	platform_ipc_event((uint32_t)IPC_EVENT_SHUTDOWN,
		"{\"change\":\"restart\"}");
	restart_requested = true;
	PostQuitMessage(0);
}

// Starts this executable again, by its original path: after the running
// swinewm.exe was renamed and a new one copied in, that is the new one.
// Runs last in platform_finish, when our shell window, desktop and tray are
// gone, so the new instance finds things as at logon.
static void respawn(void) {
	wchar_t *cmdline = _wcsdup(GetCommandLineW());
	// The new instance waits for this one to be gone (IPC pipe, hooks).
	wchar_t pid[16];
	swprintf(pid, 16, L"%lu", GetCurrentProcessId());
	SetEnvironmentVariableW(L"SWINEWM_RESTART_PID", pid);
	STARTUPINFOW si = { .cb = sizeof(si) };
	PROCESS_INFORMATION pi = {0};
	// As the shell there is no terminal to share: no console window.
	DWORD flags = win32_shell_mode ? DETACHED_PROCESS : 0;
	if (cmdline && CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, flags,
			NULL, NULL, &si, &pi)) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	} else {
		char *msg = win32_error_string(GetLastError());
		swine_log(SWINE_ERROR, "Unable to restart: %s", msg);
		free(msg);
		if (win32_shell_mode) {
			win32_shell_rescue();   // never leave the user without a shell
		}
	}
	free(cmdline);
}

void platform_finish(void) {
	if (keyboard_ok) {
		win32_keyboard_finish();
	}
	if (mouse_ok) {
		win32_mouse_finish();
	}
	if (shell_hook_message) {
		DeregisterShellHookWindow(win32_shell_hwnd);
	}
	win32_shell_finish();
	win32_windows_finish();
	win32_windows_show_all();
	win32_decor_restore_all(2000);
	win32_theme_restore();
	win32_tray_finish();
	win32_titlebars_finish();
	win32_bar_finish();
	win32_notify_finish();
	win32_wallpaper_finish();
	win32_outputs_finish();
	win32_recover_display_done();
	CoUninitialize();
	win32_fonts_clear();
	win32_recover_clear();
	if (win32_shell_hwnd) {
		DestroyWindow(win32_shell_hwnd);
		win32_shell_hwnd = NULL;
	}
	if (restart_requested) {
		respawn();
	}
}

void platform_ipc_event(uint32_t type, const char *payload) {
	win32_ipc_publish(type, payload);
}

void platform_config_changed(void) {
	// Bindings are looked up live from the config, so nothing to re-register.
	platform_ipc_event((uint32_t)IPC_EVENT_BARCONFIG_UPDATE,
		"{\"id\":\"bar-0\"}");
}
