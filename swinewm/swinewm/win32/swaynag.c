#include <stdlib.h>
#include "log.h"
#include "win32.h"

#define NAG_RELOAD 1
#define NAG_DISMISS 2
#define NAG_DETAILS 3

static HWND nag_hwnd;
static HWND details_hwnd;
static HBRUSH background_brush;
static bool class_registered;

static void layout_controls(HWND hwnd) {
	RECT client;
	GetClientRect(hwnd, &client);
	int width = client.right;
	int height = client.bottom;
	const int pad = 14;
	const int title_height = 24;
	const int footer_height = 34;
	const int button_width = 124;
	const int button_height = 28;
	const int button_gap = 8;
	int details_top = pad + title_height + 8;
	int details_bottom = height - pad - footer_height - 8;
	if (width <= pad * 2 || details_bottom <= details_top) {
		return;
	}
	SetWindowPos(GetDlgItem(hwnd, NAG_RELOAD), NULL,
		width - pad - button_width * 2 - button_gap,
		height - pad - button_height, button_width, button_height,
		SWP_NOZORDER);
	SetWindowPos(GetDlgItem(hwnd, NAG_DISMISS), NULL,
		width - pad - button_width, height - pad - button_height,
		button_width, button_height, SWP_NOZORDER);
	SetWindowPos(details_hwnd, NULL, pad, details_top,
		width - pad * 2, details_bottom - details_top, SWP_NOZORDER);
}

static void destroy_nag(void) {
	if (nag_hwnd) {
		DestroyWindow(nag_hwnd);
	}
}

static LRESULT CALLBACK nag_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
		LPARAM lparam) {
	switch (msg) {
	case WM_CREATE: {
		HINSTANCE instance = GetModuleHandleW(NULL);
		HWND title_hwnd = CreateWindowExW(0, L"STATIC",
			L"There are errors in your SwineWM configuration:",
			WS_CHILD | WS_VISIBLE | SS_LEFT,
			14, 14, 600, 24, hwnd, NULL, instance, NULL);
		if (!title_hwnd) {
			return -1;
		}
		details_hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY |
			ES_AUTOVSCROLL | ES_NOHIDESEL | WS_VSCROLL,
			14, 46, 600, 180, hwnd, (HMENU)(INT_PTR)NAG_DETAILS,
			instance, NULL);
		HWND reload_hwnd = CreateWindowExW(0, L"BUTTON", L"Reload config",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			0, 0, 124, 28, hwnd, (HMENU)(INT_PTR)NAG_RELOAD, instance, NULL);
		HWND dismiss_hwnd = CreateWindowExW(0, L"BUTTON", L"Dismiss",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			0, 0, 124, 28, hwnd, (HMENU)(INT_PTR)NAG_DISMISS, instance, NULL);
		if (!details_hwnd || !reload_hwnd || !dismiss_hwnd) {
			return -1;
		}
		HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
		SendMessageW(title_hwnd, WM_SETFONT, (WPARAM)font, TRUE);
		SendMessageW(details_hwnd, WM_SETFONT, (WPARAM)font, TRUE);
		SendMessageW(details_hwnd, EM_SETLIMITTEXT, 1024 * 1024, 0);
		SendMessageW(reload_hwnd, WM_SETFONT, (WPARAM)font, TRUE);
		SendMessageW(dismiss_hwnd, WM_SETFONT, (WPARAM)font, TRUE);
		layout_controls(hwnd);
		return 0;
	}
	case WM_SIZE:
		layout_controls(hwnd);
		return 0;
	case WM_COMMAND:
		switch (LOWORD(wparam)) {
		case NAG_RELOAD:
			win32_run_command("reload");
			return 0;
		case NAG_DISMISS:
			destroy_nag();
			return 0;
		}
		break;
	case WM_CTLCOLORSTATIC: {
		HDC dc = (HDC)wparam;
		SetTextColor(dc, RGB(255, 255, 255));
		SetBkMode(dc, TRANSPARENT);
		return (LRESULT)background_brush;
	}
	case WM_CTLCOLOREDIT: {
		HDC dc = (HDC)wparam;
		SetTextColor(dc, RGB(255, 255, 255));
		SetBkColor(dc, RGB(112, 24, 24));
		return (LRESULT)background_brush;
	}
	case WM_ERASEBKGND: {
		RECT client;
		GetClientRect(hwnd, &client);
		FillRect((HDC)wparam, &client, background_brush);
		return 1;
	}
	case WM_CLOSE:
		destroy_nag();
		return 0;
	case WM_NCDESTROY:
		nag_hwnd = NULL;
		details_hwnd = NULL;
		break;
	}
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static bool register_class(void) {
	if (class_registered) {
		return true;
	}
	WNDCLASSEXW wc = { .cbSize = sizeof(wc) };
	wc.lpfnWndProc = nag_wndproc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = NULL;
	wc.lpszClassName = L"swinewm-swaynag";
	if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		swine_log(SWINE_ERROR, "Unable to register nag window class (%lu)",
			GetLastError());
		return false;
	}
	class_registered = true;
	return true;
}

static void position_nag(HWND hwnd) {
	HMONITOR monitor = MonitorFromWindow(win32_shell_hwnd,
		MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO info = { .cbSize = sizeof(info) };
	RECT area = {0, 0, GetSystemMetrics(SM_CXSCREEN),
		GetSystemMetrics(SM_CYSCREEN)};
	if (monitor && GetMonitorInfoW(monitor, &info)) {
		area = info.rcWork;
	}
	int area_width = area.right - area.left;
	int area_height = area.bottom - area.top;
	int width = area_width < 744 ? area_width - 24 : 720;
	int height = area_height < 444 ? area_height - 24 : 420;
	if (width < 300) {
		width = 300;
	}
	if (height < 180) {
		height = 180;
	}
	int x = area.left + (area_width - width) / 2;
	int y = area.top + 16;
	SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height,
		SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void win32_swaynag_show(const char *details) {
	if (!details) {
		destroy_nag();
		return;
	}
	wchar_t *text = utf8_to_utf16(details);
	if (!text) {
		swine_log(SWINE_ERROR, "Unable to convert config errors for nag window");
		return;
	}
	if (!register_class()) {
		free(text);
		return;
	}
	if (!background_brush) {
		background_brush = CreateSolidBrush(RGB(112, 24, 24));
		if (!background_brush) {
			swine_log(SWINE_ERROR, "Unable to create nag window brush");
			free(text);
			return;
		}
	}
	if (!nag_hwnd) {
		nag_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
			L"swinewm-swaynag", L"SwineWM configuration errors",
			WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
			0, 0, 0, 0, win32_shell_hwnd, NULL, GetModuleHandleW(NULL), NULL);
		if (!nag_hwnd) {
			swine_log(SWINE_ERROR, "Unable to create nag window (%lu)",
				GetLastError());
			free(text);
			return;
		}
	}
	if (!SetWindowTextW(details_hwnd, text)) {
		swine_log(SWINE_ERROR, "Unable to update nag window text (%lu)",
			GetLastError());
		free(text);
		return;
	}
	free(text);
	position_nag(nag_hwnd);
	InvalidateRect(nag_hwnd, NULL, TRUE);
}

void win32_swaynag_finish(void) {
	destroy_nag();
	if (background_brush) {
		DeleteObject(background_brush);
		background_brush = NULL;
	}
}
