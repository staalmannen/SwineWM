// Win32 platform entry points: the shell's hidden window, the message loop,
// and start-up/shut-down ordering.
#include <stdlib.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/notify.h"
#include "swinewm/platform.h"
#include "swinewm/server.h"
#include "win32.h"
#include <objbase.h>

HWND win32_shell_hwnd = NULL;
static bool keyboard_ok = false;
static UINT shell_hook_message = 0;

// Timers on the shell window: a 1 s tick for the bar (clock, cpu, ...) and
// a one-shot timer for the next notification expiry.
#define TIMER_TICK 1
#define TIMER_NOTIFY 2

void win32_run_command(const char *command) {
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
}

static LRESULT CALLBACK shell_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
		LPARAM lparam) {
	switch (msg) {
	case WM_SWINE_BINDING: {
		char *command = (char *)lparam;
		win32_run_command(command);
		free(command);
		return 0;
	}
	case WM_TIMER:
		notify_tick(platform_now_ms());
		platform_ui_refresh();
		return 0;
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
	case WM_DISPLAYCHANGE:
		win32_shell_resize_desktop();
		win32_outputs_update();
		transaction_commit_dirty();
		return 0;
	case WM_SETTINGCHANGE:
		if (wparam == SPI_SETWORKAREA && !win32_shell_mode) {
			// The taskbar or an app bar changed size. As the shell we set
			// the work area ourselves and do not follow it.
			win32_outputs_update();
			transaction_commit_dirty();
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
	win32_shell_restore_taskbar();
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
	// Shell hook messages: urgency (HSHELL_FLASH) and fullscreen changes.
	shell_hook_message = RegisterWindowMessageW(L"SHELLHOOK");
	if (!RegisterShellHookWindow(win32_shell_hwnd)) {
		shell_hook_message = 0;
	}
	win32_windows_manage_existing();
	SetTimer(win32_shell_hwnd, TIMER_TICK, 1000, NULL);
	win32_shell_start();
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
	win32_shell_exit();
	PostQuitMessage(0);
}

void platform_finish(void) {
	if (keyboard_ok) {
		win32_keyboard_finish();
	}
	if (shell_hook_message) {
		DeregisterShellHookWindow(win32_shell_hwnd);
	}
	win32_shell_finish();
	win32_windows_finish();
	win32_windows_show_all();
	win32_tray_finish();
	win32_bar_finish();
	win32_notify_finish();
	win32_wallpaper_finish();
	CoUninitialize();
	win32_fonts_clear();
	win32_recover_clear();
	if (win32_shell_hwnd) {
		DestroyWindow(win32_shell_hwnd);
		win32_shell_hwnd = NULL;
	}
}

void platform_config_changed(void) {
	// Bindings are looked up live from the config, so nothing to re-register.
}
