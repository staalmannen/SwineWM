// Next to Explorer, each bar is registered as an AppBar (SHAppBarMessage),
// so that Explorer takes its strip out of the monitor's work area: windows
// that swinewm does not manage (maximized ones, dialogs, games) then keep
// clear of the bar, as they keep clear of the taskbar. As the shell,
// swinewm sets the work area itself (shell.c) and registers nothing.
//
// SHAppBarMessage sends to Explorer's taskbar and waits, so the calls are
// made on a thread of their own: the main thread never waits for Explorer.
// The work area Windows reports then lacks the bars' strips; swinewm lays
// out its windows from the work area with them given back
// (win32_appbar_work_area) and reserves the bars itself, as before.
#include <stdlib.h>
#include <string.h>
#include "list.h"
#include "log.h"
#include "swinewm/bar/bar.h"
#include "win32.h"
#include <shellapi.h>

// What the main thread asked for: one entry per bar window.
struct appbar {
	HWND hwnd;
	UINT edge;          // ABE_*
	RECT rect;          // the strip, desktop coordinates
	bool seen;          // still wanted in this refresh
};

enum request_kind {
	REQUEST_SET,
	REQUEST_REMOVE,
	REQUEST_WINDOWPOSCHANGED,
};

struct request {
	enum request_kind kind;
	HWND hwnd;
	UINT edge;
	RECT rect;
	struct request *next;
};

static list_t *appbars = NULL;        // struct appbar *, main thread
static UINT callback_message = 0;

static HANDLE thread = NULL;
static HANDLE wake = NULL;
static CRITICAL_SECTION lock;
static struct request *head = NULL, *tail = NULL;
static bool stopping = false;

// --- the thread that talks to Explorer -----------------------------------

static list_t *registered = NULL;     // HWND, the thread's own

static bool is_registered(HWND hwnd) {
	for (int i = 0; registered && i < registered->length; ++i) {
		if (registered->items[i] == hwnd) {
			return true;
		}
	}
	return false;
}

static void forget(HWND hwnd) {
	for (int i = 0; registered && i < registered->length; ++i) {
		if (registered->items[i] == hwnd) {
			list_del(registered, i);
			return;
		}
	}
}

static void handle(const struct request *r) {
	APPBARDATA abd = { .cbSize = sizeof(abd), .hWnd = r->hwnd };
	switch (r->kind) {
	case REQUEST_SET:
		if (!is_registered(r->hwnd)) {
			abd.uCallbackMessage = callback_message;
			if (!SHAppBarMessage(ABM_NEW, &abd)) {
				// No taskbar to register with (Explorer is not running).
				return;
			}
			list_add(registered, r->hwnd);
		}
		abd.uEdge = r->edge;
		abd.rc = r->rect;
		// Explorer may move it off its taskbar; the bar is already placed
		// next to the taskbar (inside the work area), so it stays.
		SHAppBarMessage(ABM_QUERYPOS, &abd);
		abd.rc = r->rect;
		SHAppBarMessage(ABM_SETPOS, &abd);
		break;
	case REQUEST_REMOVE:
		if (is_registered(r->hwnd)) {
			SHAppBarMessage(ABM_REMOVE, &abd);
			forget(r->hwnd);
		}
		break;
	case REQUEST_WINDOWPOSCHANGED:
		if (is_registered(r->hwnd)) {
			SHAppBarMessage(ABM_WINDOWPOSCHANGED, &abd);
		}
		break;
	}
}

static DWORD WINAPI appbar_thread(LPVOID param) {
	(void)param;
	registered = create_list();
	for (;;) {
		WaitForSingleObject(wake, INFINITE);
		for (;;) {
			EnterCriticalSection(&lock);
			struct request *r = head;
			if (r) {
				head = r->next;
				if (!head) {
					tail = NULL;
				}
			}
			bool stop = stopping && !r;
			LeaveCriticalSection(&lock);
			if (stop) {
				list_free(registered);
				registered = NULL;
				return 0;
			}
			if (!r) {
				break;
			}
			handle(r);
			free(r);
		}
	}
}

static void queue(enum request_kind kind, HWND hwnd, UINT edge,
		const RECT *rect) {
	if (!thread) {
		InitializeCriticalSection(&lock);
		wake = CreateEventW(NULL, FALSE, FALSE, NULL);
		thread = CreateThread(NULL, 0, appbar_thread, NULL, 0, NULL);
		if (!thread) {
			CloseHandle(wake);
			DeleteCriticalSection(&lock);
			return;
		}
	}
	struct request *r = calloc(1, sizeof(*r));
	r->kind = kind;
	r->hwnd = hwnd;
	r->edge = edge;
	if (rect) {
		r->rect = *rect;
	}
	EnterCriticalSection(&lock);
	if (tail) {
		tail->next = r;
	} else {
		head = r;
	}
	tail = r;
	LeaveCriticalSection(&lock);
	SetEvent(wake);
}

// --- main thread ---------------------------------------------------------

UINT win32_appbar_message(void) {
	if (!callback_message) {
		callback_message = RegisterWindowMessageW(L"swinewm-appbar");
	}
	return callback_message;
}

static struct appbar *find(HWND hwnd) {
	for (int i = 0; appbars && i < appbars->length; ++i) {
		struct appbar *a = appbars->items[i];
		if (a->hwnd == hwnd) {
			return a;
		}
	}
	return NULL;
}

void win32_appbar_begin(void) {
	for (int i = 0; appbars && i < appbars->length; ++i) {
		((struct appbar *)appbars->items[i])->seen = false;
	}
}

void win32_appbar_set(HWND hwnd, int position, const struct swine_box *zone) {
	if (win32_shell_mode) {
		return;
	}
	win32_appbar_message();
	if (!appbars) {
		appbars = create_list();
	}
	UINT edge = position == BAR_BOTTOM ? ABE_BOTTOM :
		position == BAR_LEFT ? ABE_LEFT : position == BAR_RIGHT ? ABE_RIGHT :
		ABE_TOP;
	RECT rect = {zone->x, zone->y, zone->x + zone->width,
		zone->y + zone->height};
	struct appbar *a = find(hwnd);
	if (!a) {
		a = calloc(1, sizeof(*a));
		a->hwnd = hwnd;
		list_add(appbars, a);
	} else if (a->edge == edge && EqualRect(&a->rect, &rect)) {
		a->seen = true;
		return;   // unchanged: no work area change, no loop
	}
	a->edge = edge;
	a->rect = rect;
	a->seen = true;
	queue(REQUEST_SET, hwnd, edge, &rect);
}

void win32_appbar_end(void) {
	for (int i = appbars ? appbars->length - 1 : -1; i >= 0; --i) {
		struct appbar *a = appbars->items[i];
		if (!a->seen) {
			queue(REQUEST_REMOVE, a->hwnd, 0, NULL);
			free(a);
			list_del(appbars, i);
		}
	}
}

void win32_appbar_window_message(HWND hwnd, UINT msg, WPARAM wparam) {
	struct appbar *a = find(hwnd);
	if (!a) {
		return;
	}
	if (msg == WM_WINDOWPOSCHANGED) {
		queue(REQUEST_WINDOWPOSCHANGED, hwnd, 0, NULL);
	} else if (msg == callback_message && wparam == ABN_POSCHANGED) {
		// The taskbar moved or changed: claim the strip again.
		queue(REQUEST_SET, hwnd, a->edge, &a->rect);
	}
}

// The monitor's work area with our own bars' strips given back: the bars
// are reserved inside swinewm's layout, so taking them out twice would
// shrink it, move the bars and register them again.
void win32_appbar_work_area(RECT *work) {
	bool changed = true;
	while (changed && appbars) {
		changed = false;
		for (int i = 0; i < appbars->length; ++i) {
			const RECT *r = &((struct appbar *)appbars->items[i])->rect;
			int w = r->right - r->left, h = r->bottom - r->top;
			bool spans_x = r->left <= work->left && r->right >= work->right;
			bool spans_y = r->top <= work->top && r->bottom >= work->bottom;
			if (spans_x && r->bottom == work->top && h > 0) {
				work->top -= h;
				changed = true;
			} else if (spans_x && r->top == work->bottom && h > 0) {
				work->bottom += h;
				changed = true;
			} else if (spans_y && r->right == work->left && w > 0) {
				work->left -= w;
				changed = true;
			} else if (spans_y && r->left == work->right && w > 0) {
				work->right += w;
				changed = true;
			}
		}
	}
}

void win32_appbar_finish(void) {
	while (appbars && appbars->length) {
		struct appbar *a = appbars->items[appbars->length - 1];
		queue(REQUEST_REMOVE, a->hwnd, 0, NULL);
		free(a);
		list_del(appbars, appbars->length - 1);
	}
	if (appbars) {
		list_free(appbars);
		appbars = NULL;
	}
	if (thread) {
		EnterCriticalSection(&lock);
		stopping = true;
		LeaveCriticalSection(&lock);
		SetEvent(wake);
		// The removals are quick unless Explorer hangs; do not wait forever.
		if (WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0) {
			CloseHandle(thread);
			CloseHandle(wake);
			DeleteCriticalSection(&lock);
		}
		thread = NULL;
	}
}
