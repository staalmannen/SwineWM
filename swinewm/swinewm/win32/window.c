// Native windows as sway views.
//
// Discovery: WinEvent hooks (out-of-context, delivered through our message
// loop) report windows being shown, hidden, destroyed, minimized and
// focused. Callbacks only queue events; WM_SWINE_EVENTS drains the queue on
// the main thread so the tree is never changed re-entrantly.
//
// Workspaces are implemented by hiding windows (SW_HIDE). The resulting
// hide/show events are expected and must not be mistaken for the
// application hiding itself, so each view counts the events it is waiting
// for. Hung applications must never block the WM, so only asynchronous
// operations are used on other processes' windows (ShowWindowAsync,
// SWP_ASYNCWINDOWPOS, PostMessage).
#include <dwmapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "list.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/input/seat.h"
#include "swinewm/platform.h"
#include "swinewm/server.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/view.h"
#include "swinewm/tree/workspace.h"
#include "win32.h"

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif
#ifndef EVENT_OBJECT_CLOAKED
#define EVENT_OBJECT_CLOAKED 0x8017
#define EVENT_OBJECT_UNCLOAKED 0x8018
#endif

struct win32_view {
	int expected_hides;
	int expected_shows;
	RECT insets;          // invisible resize borders, from DWM
	bool have_insets;
	bool minimized;       // unmapped because the user minimized it
};

struct queued_event {
	DWORD event;
	HWND hwnd;
};

static HWINEVENTHOOK hooks[8];
static int hook_count = 0;
static struct queued_event *queue = NULL;
static int queue_len = 0, queue_cap = 0;
static bool drain_posted = false;

static struct win32_view *wv(struct swine_view *view) {
	return view->platform;
}

static bool is_cloaked(HWND hwnd) {
	DWORD cloaked = 0;
	return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
		sizeof(cloaked))) && cloaked != 0;
}

static bool class_is_ignored(const wchar_t *cls) {
	static const wchar_t *ignored[] = {
		L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW",
		L"Windows.UI.Core.CoreWindow", L"NotifyIconOverflowWindow",
		L"TopLevelWindowForOverflowXamlIsland", L"XamlExplorerHostIslandWindow",
		L"ForegroundStaging", L"MultitaskingViewFrame",
	};
	for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); ++i) {
		if (wcscmp(cls, ignored[i]) == 0) {
			return true;
		}
	}
	return false;
}

// Windows' own shell UI (Start, search, flyouts) uses these classes without
// WS_EX_WINDOWEDGE; real UWP app frames have it. From ManagedShell's
// ApplicationWindow.IsImmersiveShellWindow (Apache-2.0, Cairo Shell).
static bool is_immersive_shell_window(HWND hwnd, const wchar_t *cls) {
	if (wcscmp(cls, L"ApplicationFrameWindow") != 0 &&
			wcscmp(cls, L"Windows.UI.Core.CoreWindow") != 0 &&
			wcscmp(cls, L"StartMenuSizingFrame") != 0 &&
			wcscmp(cls, L"Shell_LightDismissOverlay") != 0) {
		return false;
	}
	return (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_WINDOWEDGE) == 0;
}

// Heuristic for "an application's top-level window": the rules Explorer's
// taskbar uses (as reimplemented by ManagedShell's CanAddToTaskbar) plus a
// few shell classes to skip.
static bool is_manageable(HWND hwnd) {
	if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || is_cloaked(hwnd)) {
		return false;
	}
	// Minimized windows join the layout when they are restored.
	if (IsIconic(hwnd)) {
		return false;
	}
	if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
		return false;
	}
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == GetCurrentProcessId()) {
		return false;
	}
	LONG style = GetWindowLongW(hwnd, GWL_STYLE);
	LONG exstyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
	if (style & WS_CHILD) {
		return false;
	}
	if ((exstyle & WS_EX_TOOLWINDOW) && !(exstyle & WS_EX_APPWINDOW)) {
		return false;
	}
	if ((exstyle & WS_EX_NOACTIVATE) && !(exstyle & WS_EX_APPWINDOW)) {
		return false;
	}
	// Captionless popups (splash screens, menus, overlays) stay unmanaged
	// unless they explicitly ask for a taskbar button.
	if (!(style & WS_CAPTION) && !(exstyle & WS_EX_APPWINDOW)) {
		return false;
	}
	wchar_t cls[256] = L"";
	GetClassNameW(hwnd, cls, 256);
	if (class_is_ignored(cls) || is_immersive_shell_window(hwnd, cls)) {
		return false;
	}
	// Removed from the taskbar with ITaskbarList::DeleteTab.
	if (GetPropW(hwnd, L"ITaskList_Deleted")) {
		return false;
	}
	RECT r;
	if (!GetWindowRect(hwnd, &r) || r.right - r.left <= 0 ||
			r.bottom - r.top <= 0) {
		return false;
	}
	return true;
}

static bool should_float(HWND hwnd) {
	LONG style = GetWindowLongW(hwnd, GWL_STYLE);
	wchar_t cls[64] = L"";
	GetClassNameW(hwnd, cls, 64);
	// Owned windows are dialogs/tool palettes; fixed-size windows cannot be
	// tiled; #32770 is the standard dialog class.
	return GetWindow(hwnd, GW_OWNER) != NULL || !(style & WS_THICKFRAME) ||
		wcscmp(cls, L"#32770") == 0;
}

static void update_insets(struct swine_view *view) {
	HWND hwnd = view->handle;
	RECT window, frame;
	if (!IsWindowVisible(hwnd) || IsIconic(hwnd) ||
			!GetWindowRect(hwnd, &window) ||
			FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
				&frame, sizeof(frame)))) {
		return;
	}
	struct win32_view *w = wv(view);
	w->insets.left = frame.left - window.left;
	w->insets.top = frame.top - window.top;
	w->insets.right = window.right - frame.right;
	w->insets.bottom = window.bottom - frame.bottom;
	w->have_insets = true;
}

static struct swine_box visible_rect(HWND hwnd) {
	RECT r;
	if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r,
			sizeof(r)))) {
		GetWindowRect(hwnd, &r);
	}
	return (struct swine_box){r.left, r.top, r.right - r.left, r.bottom - r.top};
}

static char *window_title(HWND hwnd) {
	int len = GetWindowTextLengthW(hwnd);
	wchar_t *buf = calloc((size_t)len + 1, sizeof(wchar_t));
	GetWindowTextW(hwnd, buf, len + 1);
	char *title = utf16_to_utf8(buf);
	free(buf);
	return title;
}

char *win32_process_name(DWORD pid) {
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process) {
		return NULL;
	}
	wchar_t path[MAX_PATH];
	DWORD size = MAX_PATH;
	char *name = NULL;
	if (QueryFullProcessImageNameW(process, 0, path, &size)) {
		wchar_t *base = wcsrchr(path, L'\\');
		name = utf16_to_utf8(base ? base + 1 : path);
	}
	CloseHandle(process);
	return name;
}

static struct swine_view *create_view(HWND hwnd) {
	struct swine_view *view = view_create(hwnd);
	view->platform = calloc(1, sizeof(struct win32_view));
	view->title = window_title(hwnd);
	wchar_t cls[256] = L"";
	GetClassNameW(hwnd, cls, 256);
	view->class_name = utf16_to_utf8(cls);
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	view->pid = pid;
	view->app_id = win32_process_name(pid);
	view->natural_floating = should_float(hwnd);
	view->natural = visible_rect(hwnd);
	update_insets(view);
	return view;
}

static void map_view(struct swine_view *view) {
	view->natural = visible_rect(view->handle);
	view_map(view);
	swine_log(SWINE_INFO, "Managing '%s' (%s, %s)%s", view->title,
		view->app_id ? view->app_id : "?", view->class_name,
		view->natural_floating ? " floating" : "");
}

static void destroy_view(struct swine_view *view) {
	free(view->platform);
	view->platform = NULL;
	view_destroy(view);
}

void win32_windows_manage_existing(void) {
	// EnumWindows returns windows in Z order (top first). Map bottom-up so
	// the topmost window ends up focused, as the user last saw it.
	list_t *found = create_list();
	HWND hwnd = GetTopWindow(NULL);
	while (hwnd) {
		if (is_manageable(hwnd) && !view_from_handle(hwnd)) {
			list_insert(found, 0, hwnd);
		}
		hwnd = GetWindow(hwnd, GW_HWNDNEXT);
	}
	for (int i = 0; i < found->length; ++i) {
		map_view(create_view(found->items[i]));
	}
	list_free(found);
}

// The rectangle that counts for fullscreen detection: the client area of
// windows with a frame, else the window (ManagedShell's
// FullScreenHelper.GetEffectiveWindowRect, Apache-2.0).
static RECT effective_rect(HWND hwnd) {
	LONG style = GetWindowLongW(hwnd, GWL_STYLE);
	RECT r = {0};
	if ((style & (WS_CAPTION | WS_THICKFRAME)) == (WS_CAPTION | WS_THICKFRAME) ||
			(style & (WS_POPUP | WS_THICKFRAME)) == (WS_POPUP | WS_THICKFRAME) ||
			(style & (WS_POPUP | WS_BORDER)) == (WS_POPUP | WS_BORDER)) {
		GetClientRect(hwnd, &r);
		MapWindowPoints(hwnd, NULL, (POINT *)&r, 2);
	} else {
		GetWindowRect(hwnd, &r);
	}
	return r;
}

// The output a window covers completely, or NULL if it is not fullscreen.
static struct swine_output *fullscreen_output(HWND hwnd) {
	if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd) ||
			is_cloaked(hwnd) || GetPropW(hwnd, L"NonRudeHWND")) {
		return NULL;
	}
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == GetCurrentProcessId()) {
		return NULL;
	}
	wchar_t cls[256] = L"";
	GetClassNameW(hwnd, cls, 256);
	if (class_is_ignored(cls) || is_immersive_shell_window(hwnd, cls)) {
		return NULL;
	}
	LONG exstyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
	if ((exstyle & WS_EX_LAYERED) &&
			(exstyle & (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE))) {
		return NULL;   // overlays
	}
	RECT r = effective_rect(hwnd);
	for (int i = 0; i < root->outputs->length; ++i) {
		struct swine_output *o = root->outputs->items[i];
		if (r.left == o->box.x && r.top == o->box.y &&
				r.right == o->box.x + o->box.width &&
				r.bottom == o->box.y + o->box.height) {
			return o;
		}
	}
	return NULL;
}

// Keeps fullscreen state in step with what windows do on their own:
// a managed window that makes itself fullscreen (F11 in a browser) becomes
// a fullscreen container, as sway does for an xdg fullscreen request; an
// unmanaged one in front (a game) hides the bar on its output.
// Returns true if anything changed.
static bool check_fullscreen(HWND hwnd) {
	struct swine_view *view = view_from_handle(hwnd);
	struct swine_output *covered = fullscreen_output(hwnd);
	if (view && view->container) {
		if (covered && view->container->fullscreen_mode == FULLSCREEN_NONE) {
			swine_log(SWINE_DEBUG, "'%s' went fullscreen", view->title);
			view_request_fullscreen(view, true);
			return true;
		} else if (!covered && view->fullscreen_requested) {
			swine_log(SWINE_DEBUG, "'%s' left fullscreen", view->title);
			view_request_fullscreen(view, false);
			return true;
		}
		return false;
	}
	if (hwnd != GetForegroundWindow()) {
		return false;
	}
	bool changed = false;
	for (int i = 0; i < root->outputs->length; ++i) {
		struct swine_output *o = root->outputs->items[i];
		bool covers = o == covered;
		changed |= o->external_fullscreen != covers;
		o->external_fullscreen = covers;
	}
	return changed;
}

void win32_windows_shell_hook(WPARAM code, LPARAM lparam) {
	HWND hwnd = (HWND)lparam;
	switch (code) {
	case 0x8006: { // HSHELL_FLASH: FlashWindowEx, i.e. "urgent"
		struct swine_view *view = view_from_handle(hwnd);
		if (view) {
			view_set_urgent(view, true);
			transaction_commit_dirty();
		}
		break;
	}
	case 53: // HSHELL_FULLSCREENENTER
	case 54: // HSHELL_FULLSCREENEXIT
		check_fullscreen(hwnd);
		transaction_commit_dirty();
		break;
	default:
		break;
	}
}

static void handle_event(DWORD event, HWND hwnd) {
	struct swine_view *view = view_from_handle(hwnd);
	struct win32_view *w = view ? wv(view) : NULL;

	switch (event) {
	case EVENT_OBJECT_DESTROY:
		if (view) {
			swine_log(SWINE_DEBUG, "Window destroyed: '%s'", view->title);
			destroy_view(view);
		}
		break;

	case EVENT_OBJECT_SHOW:
	case EVENT_OBJECT_UNCLOAKED:
		if (view && event == EVENT_OBJECT_SHOW && w->expected_shows > 0) {
			w->expected_shows--;
			break;
		}
		if (!view) {
			if (is_manageable(hwnd)) {
				map_view(create_view(hwnd));
			}
		} else if (!view->container) {
			if (is_manageable(hwnd)) {
				map_view(view);
			}
		} else if (!view->current.visible) {
			// The application showed a window we had hidden (e.g. it was
			// activated from a tray icon): follow it to its workspace.
			swine_log(SWINE_DEBUG, "'%s' showed itself; switching to it",
				view->title);
			view->current.visible = true;
			seat_set_focus_container(view->container);
		}
		break;

	case EVENT_OBJECT_HIDE:
	case EVENT_OBJECT_CLOAKED:
		if (!view) {
			break;
		}
		if (event == EVENT_OBJECT_HIDE && w->expected_hides > 0) {
			w->expected_hides--;
			break;
		}
		if (view->container) {
			// Hidden by the application itself, e.g. minimized to the tray.
			swine_log(SWINE_DEBUG, "'%s' hid itself; unmanaging", view->title);
			view_unmap(view);
		}
		break;

	case EVENT_SYSTEM_MINIMIZESTART:
		if (view && view->container) {
			w->minimized = true;
			view_unmap(view);
		}
		break;

	case EVENT_SYSTEM_MINIMIZEEND:
		if (view && !view->container && w->minimized) {
			w->minimized = false;
			map_view(view);
		} else if (!view && is_manageable(hwnd)) {
			map_view(create_view(hwnd));
		}
		break;

	case EVENT_SYSTEM_FOREGROUND:
		if (!view && is_manageable(hwnd)) {
			view = create_view(hwnd);
			map_view(view);
		}
		// Ignore windows on hidden workspaces: while a workspace is being
		// hidden, Windows briefly activates other windows from it, and
		// following those would switch straight back.
		if (view && view->container && view->pending.visible) {
			transaction_note_activated(view);
			seat_set_focus_container(view->container);
		}
		check_fullscreen(hwnd);
		break;

	case EVENT_OBJECT_LOCATIONCHANGE:
		// Frequent (every frame of a drag): only commit on a change.
		if (!view || !view->container || !check_fullscreen(hwnd)) {
			return;
		}
		break;

	case EVENT_OBJECT_NAMECHANGE:
		if (view) {
			char *title = window_title(hwnd);
			view_set_title(view, title);
			free(title);
			platform_ui_refresh();   // the bar shows the title
		}
		return;

	case EVENT_SYSTEM_MOVESIZEEND:
		if (view && view->container) {
			update_insets(view);
			if (container_is_floating(view->container)) {
				view->container->floating_box = visible_rect(hwnd);
				view->current.box = view->container->floating_box;
			} else {
				// A tiled window was dragged: snap it back into place.
				view->current.valid = false;
			}
		}
		break;

	default:
		return;
	}
	transaction_commit_dirty();
}

void win32_windows_drain_events(void) {
	drain_posted = false;
	// Handlers may queue further events; process until empty.
	for (int i = 0; i < queue_len; ++i) {
		handle_event(queue[i].event, queue[i].hwnd);
	}
	queue_len = 0;
}

static void CALLBACK win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
		LONG object_id, LONG child_id, DWORD thread, DWORD time) {
	(void)hook;
	(void)thread;
	(void)time;
	if (!hwnd || object_id != OBJID_WINDOW || child_id != CHILDID_SELF) {
		return;
	}
	if (event == EVENT_OBJECT_LOCATIONCHANGE && !view_from_handle(hwnd)) {
		return;   // very frequent; only managed windows matter
	}
	if (queue_len == queue_cap) {
		queue_cap = queue_cap ? queue_cap * 2 : 64;
		queue = realloc(queue, sizeof(*queue) * (size_t)queue_cap);
	}
	queue[queue_len++] = (struct queued_event){event, hwnd};
	if (!drain_posted) {
		drain_posted = true;
		PostMessageW(win32_shell_hwnd, WM_SWINE_EVENTS, 0, 0);
	}
}

static void add_hook(DWORD min, DWORD max) {
	HWINEVENTHOOK hook = SetWinEventHook(min, max, NULL, win_event_proc, 0, 0,
		WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
	if (hook) {
		hooks[hook_count++] = hook;
	} else {
		swine_log(SWINE_ERROR, "SetWinEventHook(0x%lx) failed", min);
	}
}

bool win32_windows_init(void) {
	add_hook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND);
	add_hook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND);
	add_hook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND);
	add_hook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE);   // destroy, show, hide
	add_hook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE);
	add_hook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED);
	return hook_count > 0;
}

void win32_windows_finish(void) {
	for (int i = 0; i < hook_count; ++i) {
		UnhookWinEvent(hooks[i]);
	}
	hook_count = 0;
	free(queue);
	queue = NULL;
	queue_len = queue_cap = 0;
}

void win32_windows_show_all(void) {
	if (!server.views) {
		return;
	}
	for (int i = 0; i < server.views->length; ++i) {
		struct swine_view *view = server.views->items[i];
		if (view->container && IsWindow(view->handle) &&
				!IsWindowVisible(view->handle)) {
			ShowWindowAsync(view->handle, SW_SHOWNA);
		}
	}
}

void platform_view_set_geometry(struct swine_view *view,
		const struct swine_box *box) {
	HWND hwnd = view->handle;
	if (!IsWindow(hwnd)) {
		return;
	}
	struct win32_view *w = wv(view);
	if (IsZoomed(hwnd) || IsIconic(hwnd)) {
		// Leave the maximized/minimized state first, or the window keeps
		// treating our size as its "restored" size.
		ShowWindowAsync(hwnd, SW_SHOWNOACTIVATE);
	}
	update_insets(view);
	RECT in = w->have_insets ? w->insets : (RECT){0, 0, 0, 0};
	// Windows 10+ windows have invisible resize borders outside the visible
	// frame; grow the rectangle so the visible frame matches the layout.
	SetWindowPos(hwnd, NULL, box->x - in.left, box->y - in.top,
		box->width + in.left + in.right, box->height + in.top + in.bottom,
		SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

void platform_view_set_visible(struct swine_view *view, bool visible) {
	HWND hwnd = view->handle;
	if (!IsWindow(hwnd)) {
		return;
	}
	struct win32_view *w = wv(view);
	bool is_visible = IsWindowVisible(hwnd) != FALSE;
	if (visible == is_visible) {
		return;
	}
	if (visible) {
		w->expected_shows++;
		ShowWindowAsync(hwnd, SW_SHOWNOACTIVATE);
	} else {
		update_insets(view);
		w->expected_hides++;
		ShowWindowAsync(hwnd, SW_HIDE);
	}
	win32_recover_schedule_save();
}

bool platform_view_activate(struct swine_view *view) {
	HWND hwnd = view->handle;
	if (!IsWindow(hwnd)) {
		return false;
	}
	if (GetForegroundWindow() == hwnd) {
		return true;
	}
	if (SetForegroundWindow(hwnd)) {
		return true;
	}
	// Windows only lets the process that received the last input event take
	// the foreground. A zero-length synthetic mouse move makes us that
	// process without side effects in the target application.
	INPUT input = { .type = INPUT_MOUSE };
	input.mi.dwFlags = MOUSEEVENTF_MOVE;
	SendInput(1, &input, sizeof(input));
	return SetForegroundWindow(hwnd) != FALSE;
}

void platform_view_close(struct swine_view *view) {
	if (IsWindow(view->handle)) {
		PostMessageW(view->handle, WM_CLOSE, 0, 0);
	}
}
