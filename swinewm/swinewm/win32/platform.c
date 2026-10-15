// Win32 platform entry points: the shell's hidden window, the message loop,
// and start-up/shut-down ordering.
#include <stdlib.h>
#include "log.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/platform.h"
#include "swinewm/server.h"
#include "win32.h"

HWND win32_shell_hwnd = NULL;
static bool keyboard_ok = false;

static void run_binding(char *command) {
	list_t *res_list = execute_command(command, NULL);
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
		run_binding(command);
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
	case WM_DISPLAYCHANGE:
		win32_outputs_update();
		transaction_commit_dirty();
		return 0;
	case WM_SETTINGCHANGE:
		if (wparam == SPI_SETWORKAREA) {
			// The taskbar or an app bar changed size.
			win32_outputs_update();
			transaction_commit_dirty();
		}
		return 0;
	case WM_CLOSE:
		PostQuitMessage(0);
		return 0;
	default:
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
	// Best effort: never leave the user with invisible applications.
	win32_windows_show_all();
	return EXCEPTION_CONTINUE_SEARCH;
}

bool platform_init(void) {
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

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
	win32_windows_manage_existing();
	return true;
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
	PostQuitMessage(0);
}

void platform_finish(void) {
	if (keyboard_ok) {
		win32_keyboard_finish();
	}
	win32_windows_finish();
	win32_windows_show_all();
	win32_recover_clear();
	if (win32_shell_hwnd) {
		DestroyWindow(win32_shell_hwnd);
		win32_shell_hwnd = NULL;
	}
}

void platform_config_changed(void) {
	// Bindings are looked up live from the config, so nothing to re-register.
}
