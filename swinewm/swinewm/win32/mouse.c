// Pointer input through a low-level mouse hook (WH_MOUSE_LL), for
// focus_follows_mouse, mouse bindings, floating_modifier moves and resizes
// and dragging tiled windows (input/cursor.c decides; this file only finds
// what is under the pointer and passes events on).
//
// The hook runs on the main thread, between messages, and must return
// quickly or Windows removes it. Motion is only noted and handled from a
// posted message, a press or release is handed to the core at once because
// the hook has to say whether the application gets it. Commands of mouse
// bindings are posted (platform_queue_command), never run in the hook.
#include <stdlib.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/config.h"
#include "swinewm/input/cursor.h"
#include "swinewm/platform.h"
#include "swinewm/tree/view.h"
#include "win32.h"

int win32_busy = 0;

static HHOOK hook = NULL;
static POINT motion_point;
static bool motion_posted = false;
static bool swallowed[5];   // per button: its press was kept from the app
static HWND indicator = NULL;
static COLORREF indicator_color = RGB(0x2e, 0x9e, 0xf4);

static const wchar_t indicator_class[] = L"SwineWMDropIndicator";

static bool is_desktop_window(HWND top) {
	if (!top) {
		return false;
	}
	if (top == win32_shell_desktop_window() || top == GetShellWindow()) {
		return true;
	}
	// Explorer's desktop when swinewm runs next to it.
	wchar_t cls[32];
	if (GetClassNameW(top, cls, 32)) {
		return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0;
	}
	return false;
}

static enum cursor_hit hit_test(POINT pt, struct swine_container **con) {
	*con = NULL;
	HWND hwnd = WindowFromPoint(pt);
	HWND top = hwnd ? GetAncestor(hwnd, GA_ROOT) : NULL;
	if (!top) {
		return HIT_NONE;
	}
	struct swine_view *view = view_from_handle(top);
	if (view && view->container) {
		*con = view->container;
		return HIT_CONTENTS;
	}
	if (is_desktop_window(top)) {
		return HIT_WORKSPACE;
	}
	return HIT_NONE;
}

// A window swinewm does not manage has the focus: a menu, a launcher, a
// dialog that is left alone. focus_follows_mouse must not take the focus
// away from it.
static bool foreign_foreground(void) {
	HWND fg = GetForegroundWindow();
	if (!fg || !IsWindowVisible(fg) || view_from_handle(fg)) {
		return false;
	}
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	if (pid == GetCurrentProcessId() || is_desktop_window(fg)) {
		return false;
	}
	return true;
}

void win32_mouse_handle_motion(void) {
	motion_posted = false;
	struct swine_container *con = NULL;
	enum cursor_hit hit = hit_test(motion_point, &con);
	if (!cursor_in_operation() && foreign_foreground()) {
		hit = HIT_NONE;
	}
	win32_busy++;
	cursor_motion(motion_point.x, motion_point.y, hit, con);
	win32_busy--;
}

static int button_index(WPARAM msg, DWORD data, uint32_t *button, bool *pressed) {
	switch (msg) {
	case WM_LBUTTONDOWN: case WM_LBUTTONUP:
		*button = SWINE_BTN_LEFT;
		*pressed = msg == WM_LBUTTONDOWN;
		return 0;
	case WM_RBUTTONDOWN: case WM_RBUTTONUP:
		*button = SWINE_BTN_RIGHT;
		*pressed = msg == WM_RBUTTONDOWN;
		return 1;
	case WM_MBUTTONDOWN: case WM_MBUTTONUP:
		*button = SWINE_BTN_MIDDLE;
		*pressed = msg == WM_MBUTTONDOWN;
		return 2;
	case WM_XBUTTONDOWN: case WM_XBUTTONUP: {
		bool first = HIWORD(data) == XBUTTON1;
		*button = first ? SWINE_BTN_SIDE : SWINE_BTN_EXTRA;
		*pressed = msg == WM_XBUTTONDOWN;
		return first ? 3 : 4;
	}
	default:
		return -1;
	}
}

static LRESULT CALLBACK mouse_proc(int code, WPARAM wparam, LPARAM lparam) {
	if (code != HC_ACTION) {
		return CallNextHookEx(hook, code, wparam, lparam);
	}
	const MSLLHOOKSTRUCT *m = (const MSLLHOOKSTRUCT *)lparam;
	// Synthetic input (ours, or from other programs) is left alone.
	if (m->flags & LLMHF_INJECTED) {
		return CallNextHookEx(hook, code, wparam, lparam);
	}

	if (wparam == WM_MOUSEMOVE) {
		motion_point = m->pt;
		if (!motion_posted) {
			motion_posted = true;
			PostMessageW(win32_shell_hwnd, WM_SWINE_MOTION, 0, 0);
		}
		return CallNextHookEx(hook, code, wparam, lparam);
	}

	uint32_t mods = win32_keyboard_modifiers();
	if (wparam == WM_MOUSEWHEEL || wparam == WM_MOUSEHWHEEL) {
		if (win32_busy) {
			return CallNextHookEx(hook, code, wparam, lparam);
		}
		short delta = (short)HIWORD(m->mouseData);
		uint32_t button = wparam == WM_MOUSEWHEEL ?
			(delta > 0 ? SWINE_SCROLL_UP : SWINE_SCROLL_DOWN) :
			(delta > 0 ? SWINE_SCROLL_RIGHT : SWINE_SCROLL_LEFT);
		struct swine_container *con = NULL;
		enum cursor_hit hit = hit_test(m->pt, &con);
		win32_busy++;
		bool used = cursor_axis(m->pt.x, m->pt.y, hit, con, button, mods);
		win32_busy--;
		if (used) {
			win32_keyboard_used_modifiers(mods);
			return 1;
		}
		return CallNextHookEx(hook, code, wparam, lparam);
	}

	uint32_t button = 0;
	bool pressed = false;
	int index = button_index(wparam, m->mouseData, &button, &pressed);
	if (index < 0) {
		return CallNextHookEx(hook, code, wparam, lparam);
	}
	if (win32_busy) {
		// Re-entered while swinewm code runs (a COM call pumping
		// messages): never swallow half a click.
		if (!pressed && swallowed[index]) {
			swallowed[index] = false;
			return 1;
		}
		return CallNextHookEx(hook, code, wparam, lparam);
	}
	struct swine_container *con = NULL;
	enum cursor_hit hit = hit_test(m->pt, &con);
	win32_busy++;
	bool used = cursor_button(m->pt.x, m->pt.y, hit, con, button, pressed, mods);
	win32_busy--;
	if (pressed) {
		swallowed[index] = used;
		if (used) {
			win32_keyboard_used_modifiers(mods);
			return 1;
		}
	} else if (swallowed[index]) {
		// The application never saw the press: keep the release too.
		swallowed[index] = false;
		return 1;
	}
	return CallNextHookEx(hook, code, wparam, lparam);
}

static LRESULT CALLBACK indicator_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
		LPARAM lparam) {
	if (msg == WM_NCHITTEST) {
		return HTTRANSPARENT;
	}
	if (msg == WM_ERASEBKGND) {
		RECT rect;
		GetClientRect(hwnd, &rect);
		win32_fill((HDC)wparam, &rect, indicator_color);
		return 1;
	}
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool win32_mouse_init(void) {
	HINSTANCE instance = GetModuleHandleW(NULL);
	WNDCLASSEXW wc = { .cbSize = sizeof(wc) };
	wc.lpfnWndProc = indicator_wndproc;
	wc.hInstance = instance;
	wc.lpszClassName = indicator_class;
	RegisterClassExW(&wc);

	hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, instance, 0);
	if (!hook) {
		char *msg = win32_error_string(GetLastError());
		swine_log(SWINE_ERROR, "Unable to install mouse hook: %s", msg);
		free(msg);
		return false;
	}
	return true;
}

void win32_mouse_finish(void) {
	if (hook) {
		UnhookWindowsHookEx(hook);
		hook = NULL;
	}
	if (indicator) {
		DestroyWindow(indicator);
		indicator = NULL;
	}
}

void platform_queue_command(const char *command) {
	char *copy = swine_strdup(command);
	if (!PostMessageW(win32_shell_hwnd, WM_SWINE_BINDING, 0, (LPARAM)copy)) {
		free(copy);
	}
}

bool platform_cursor_position(int *x, int *y) {
	POINT pt;
	if (!GetCursorPos(&pt)) {
		return false;
	}
	*x = pt.x;
	*y = pt.y;
	return true;
}

void platform_cursor_warp(int x, int y) {
	SetCursorPos(x, y);
}

void platform_drop_indicator(const struct swine_box *box) {
	if (!box) {
		if (indicator) {
			ShowWindow(indicator, SW_HIDE);
		}
		return;
	}
	if (!indicator) {
		// Layered and transparent: drawn over everything, never hit by
		// WindowFromPoint or clicks.
		indicator = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT |
			WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
			indicator_class, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL,
			GetModuleHandleW(NULL), NULL);
		if (!indicator) {
			return;
		}
	}
	// sway draws the drop zone in client.focused's indicator colour at half
	// its opacity.
	uint32_t rgba = config ? config->border_colors.focused.indicator : 0x2E9EF4FF;
	indicator_color = RGB((rgba >> 24) & 0xFF, (rgba >> 16) & 0xFF,
		(rgba >> 8) & 0xFF);
	SetLayeredWindowAttributes(indicator, 0, (BYTE)((rgba & 0xFF) / 2), LWA_ALPHA);
	InvalidateRect(indicator, NULL, TRUE);
	SetWindowPos(indicator, HWND_TOPMOST, box->x, box->y, box->width,
		box->height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
