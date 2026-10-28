// The notification area ("system tray"), shown by the bar's "tray" module.
//
// Programs add their icons with Shell_NotifyIcon, which sends WM_COPYDATA
// to the first window of class "Shell_TrayWnd" that FindWindow finds.
// swinewm creates such a window, keeps it above Explorer's (FindWindow
// walks the z-order from the top) and broadcasts "TaskbarCreated" so that
// running programs add their icons again, now to swinewm. On exit, after a
// crash and with --recover the broadcast is repeated so they go back to
// Explorer. The approach follows ManagedShell's TrayService and
// NotificationArea (https://github.com/cairoshell/ManagedShell,
// Apache-2.0); see doc/research/windows-shell.md.
//
// The window lives on a thread of its own: Shell_NotifyIcon waits for the
// reply, and messages swinewm does not handle are passed on to Explorer,
// which may hang. The main thread is never blocked by either. The icon
// table is shared under a lock; the main thread is told about changes with
// a posted WM_SWINE_TRAY and copies them into the core (bar/tray.c).
#define COBJMACROS
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <docobj.h>
#include "list.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/bar/bar.h"
#include "swinewm/bar/tray.h"
#include "swinewm/notify.h"
#include "swinewm/platform.h"
#include "win32.h"
#include <shellapi.h>

#define TRAY_MAGIC 0x34753423
#define TIMER_RAISE 1
#define TIMER_PRUNE 2
#define FORWARD_TIMEOUT_MS 1000

// NOTIFYICONDATAW as Shell_NotifyIcon sends it: handles are 32 bit, also
// from 64-bit programs. Older programs send fewer fields (cbSize).
struct tray_nid {
	DWORD cbSize;
	DWORD hWnd;
	UINT uID;
	UINT uFlags;
	UINT uCallbackMessage;
	DWORD hIcon;
	WCHAR szTip[128];
	DWORD dwState;
	DWORD dwStateMask;
	WCHAR szInfo[256];
	UINT uVersion;          // union with uTimeout
	WCHAR szInfoTitle[64];
	DWORD dwInfoFlags;
	GUID guidItem;
	DWORD hBalloonIcon;
};

struct tray_data {          // WM_COPYDATA dwData 1
	DWORD magic;
	DWORD message;          // NIM_*
	struct tray_nid nid;
};

struct tray_icon_query {    // WM_COPYDATA dwData 3: Shell_NotifyIconGetRect
	DWORD magic;
	DWORD message;          // 1: top left, 2: bottom right
	DWORD cbSize;
	DWORD padding;
	DWORD hWnd;
	UINT uID;
	GUID guidItem;
};

struct tray_icon {
	uint64_t id;
	HWND hwnd;
	UINT uid;
	GUID guid;
	bool has_guid;
	UINT callback;
	UINT version;
	HICON icon;
	wchar_t tip[128];
	bool show_tip;          // version 4: NIF_SHOWTIP, Explorer's tooltip
	bool popup_open;        // NIN_POPUPOPEN sent, NIN_POPUPCLOSE owed
	bool hidden;
	DWORD pid;
	char *app_id;
	RECT rect;              // where the bar drew it, screen coordinates
	ULONGLONG last_down[3]; // for double clicks: left, middle, right
};

static CRITICAL_SECTION lock;
static bool lock_ready = false;
static list_t *icons = NULL;      // struct tray_icon *, under lock
static uint64_t next_id = 1;
static volatile LONG sync_posted = 0;

static HANDLE thread = NULL;
static HWND tray_hwnd = NULL;     // owned by the tray thread
static RECT host_rect = {0};
static UINT taskbar_created = 0;
static volatile LONG suspended = 0;

static list_t *icon_files = NULL; // struct icon_file *, main thread
static IOleCommandTarget *systray_object = NULL;

struct icon_file {
	char *path;
	int size;
	HICON icon;
};

static void post_sync(void) {
	if (InterlockedExchange(&sync_posted, 1) == 0) {
		PostMessageW(win32_shell_hwnd, WM_SWINE_TRAY, 0, 0);
	}
}

static void free_icon(struct tray_icon *icon) {
	if (icon->icon) {
		DestroyIcon(icon->icon);
	}
	free(icon->app_id);
	free(icon);
}

static bool guid_empty(const GUID *guid) {
	static const GUID zero = {0};
	return memcmp(guid, &zero, sizeof(zero)) == 0;
}

// Under lock.
static int find_icon(HWND hwnd, UINT uid, const GUID *guid) {
	for (int i = 0; i < icons->length; ++i) {
		struct tray_icon *icon = icons->items[i];
		if (guid && !guid_empty(guid)) {
			if (icon->has_guid && IsEqualGUID(&icon->guid, guid)) {
				return i;
			}
		} else if (icon->hwnd == hwnd && icon->uid == uid) {
			return i;
		}
	}
	return -1;
}

static struct tray_icon *find_by_id(uint64_t id) {
	for (int i = 0; icons && i < icons->length; ++i) {
		struct tray_icon *icon = icons->items[i];
		if (icon->id == id) {
			return icon;
		}
	}
	return NULL;
}

// --- balloon tips -> notifications -------------------------------------

struct tray_balloon {
	char *app_name;
	char *summary;
	char *body;
};

static void post_balloon(const struct tray_icon *icon,
		const struct tray_nid *nid) {
	if (!nid->szInfo[0] && !nid->szInfoTitle[0]) {
		return;   // an empty balloon removes the current one
	}
	struct tray_balloon *b = calloc(1, sizeof(*b));
	// The first line of the tooltip names the program best.
	wchar_t name[128];
	wcsncpy(name, icon->tip, 127);
	name[127] = L'\0';
	name[wcscspn(name, L"\r\n")] = L'\0';
	b->app_name = name[0] ? utf16_to_utf8(name) :
		swine_strdup(icon->app_id ? icon->app_id : "");
	b->summary = utf16_to_utf8(nid->szInfoTitle[0] ? nid->szInfoTitle :
		nid->szInfo);
	b->body = utf16_to_utf8(nid->szInfoTitle[0] ? nid->szInfo : L"");
	if (!PostMessageW(win32_shell_hwnd, WM_SWINE_TRAY, 1, (LPARAM)b)) {
		free(b->app_name);
		free(b->summary);
		free(b->body);
		free(b);
	}
}

// --- Shell_NotifyIcon ----------------------------------------------------

// Handles one NIM_* message on the tray thread; the return value goes back
// to Shell_NotifyIcon.
static BOOL handle_notify(DWORD message, const struct tray_nid *in,
		DWORD in_size) {
	struct tray_nid nid;
	memset(&nid, 0, sizeof(nid));
	DWORD size = in->cbSize < in_size ? in->cbSize : in_size;
	memcpy(&nid, in, size < sizeof(nid) ? size : sizeof(nid));
	if (size <= offsetof(struct tray_nid, dwState)) {
		nid.szTip[63] = L'\0';   // version 1: a 64-character tooltip
	}
	nid.szTip[127] = nid.szInfo[255] = nid.szInfoTitle[63] = L'\0';
	bool has_guid = (nid.uFlags & NIF_GUID) &&
		size >= offsetof(struct tray_nid, hBalloonIcon) &&
		!guid_empty(&nid.guidItem);
	HWND hwnd = (HWND)(UINT_PTR)nid.hWnd;

	EnterCriticalSection(&lock);
	int index = find_icon(hwnd, nid.uID, has_guid ? &nid.guidItem : NULL);
	struct tray_icon *icon = index >= 0 ? icons->items[index] : NULL;
	BOOL result = TRUE;
	bool changed = false, added = false;
	switch (message) {
	case NIM_ADD:
	case NIM_MODIFY:
		if (!icon) {
			if (!hwnd) {
				result = FALSE;   // a new icon needs its window
				break;
			}
			icon = calloc(1, sizeof(*icon));
			icon->id = next_id++;
			icon->hwnd = hwnd;
			icon->uid = nid.uID;
			GetWindowThreadProcessId(hwnd, &icon->pid);
			icon->app_id = win32_process_name(icon->pid);
			list_add(icons, icon);
			added = true;
			// Like Explorer: modifying an unknown icon fails, but keep it
			// (it may have been added before swinewm took over).
			result = message == NIM_ADD;
		}
		if (hwnd) {
			icon->hwnd = hwnd;
			icon->uid = nid.uID;
		}
		if (has_guid) {
			icon->guid = nid.guidItem;
			icon->has_guid = true;
		}
		if (nid.uFlags & NIF_MESSAGE) {
			icon->callback = nid.uCallbackMessage;
		}
		if (nid.uFlags & NIF_ICON) {
			if (icon->icon) {
				DestroyIcon(icon->icon);
			}
			icon->icon = nid.hIcon ? CopyIcon((HICON)(UINT_PTR)nid.hIcon) : NULL;
		}
		if (nid.uFlags & NIF_TIP) {
			wcscpy(icon->tip, nid.szTip);
		}
		// Version 4 icons without NIF_SHOWTIP draw their own popup
		// (NIN_POPUPOPEN); the flag comes with every NIM_MODIFY.
		icon->show_tip = (nid.uFlags & NIF_SHOWTIP) != 0;
		if (nid.uFlags & NIF_STATE) {
			DWORD mask = nid.dwStateMask ? nid.dwStateMask : NIS_HIDDEN;
			if (mask & NIS_HIDDEN) {
				icon->hidden = (nid.dwState & NIS_HIDDEN) != 0;
			}
		}
		if (nid.uVersion > 0 && nid.uVersion <= NOTIFYICON_VERSION_4) {
			icon->version = nid.uVersion;
		}
		if ((nid.uFlags & NIF_INFO) && size > offsetof(struct tray_nid, szInfo)) {
			post_balloon(icon, &nid);
		}
		if (added) {
			char *tip = utf16_to_utf8(icon->tip);
			swine_log(SWINE_DEBUG, "Tray icon %d from %s: '%s'", (int)icon->id,
				icon->app_id ? icon->app_id : "?", tip);
			free(tip);
		}
		changed = true;
		break;
	case NIM_DELETE:
		if (icon) {
			list_del(icons, index);
			free_icon(icon);
			changed = true;
		} else {
			result = FALSE;
		}
		break;
	case NIM_SETVERSION:
		if (icon && nid.uVersion <= NOTIFYICON_VERSION_4) {
			icon->version = nid.uVersion;
		} else {
			result = FALSE;
		}
		break;
	case NIM_SETFOCUS:
		break;
	default:
		result = FALSE;
	}
	LeaveCriticalSection(&lock);
	if (changed) {
		post_sync();
	}
	return result;
}

static LRESULT handle_icon_query(const struct tray_icon_query *q) {
	HWND hwnd = (HWND)(UINT_PTR)q->hWnd;
	LRESULT result = 0;
	EnterCriticalSection(&lock);
	int index = find_icon(hwnd, q->uID, &q->guidItem);
	if (index >= 0) {
		struct tray_icon *icon = icons->items[index];
		if (q->message == 1) {
			result = MAKELPARAM(icon->rect.left, icon->rect.top);
		} else if (q->message == 2) {
			result = MAKELPARAM(icon->rect.right, icon->rect.bottom);
		}
	}
	LeaveCriticalSection(&lock);
	return result;
}

// Passes a message swinewm does not handle (AppBar messages, Explorer's
// private ones) on to Explorer's taskbar, if there is one.
static LRESULT forward(UINT msg, WPARAM wparam, LPARAM lparam) {
	HWND explorer = win32_find_explorer_tray();
	DWORD_PTR result = 0;
	if (explorer && SendMessageTimeoutW(explorer, msg, wparam, lparam,
			SMTO_ABORTIFHUNG, FORWARD_TIMEOUT_MS, &result)) {
		return (LRESULT)result;
	}
	return 0;
}

// AppBar messages: SHAppBarMessage sends WM_COPYDATA (dwData 0) with this
// header; the APPBARDATA itself is in memory shared with the caller
// (SHAllocShared), where answers such as ABM_GETTASKBARPOS are written.
// Layouts as in ManagedShell's APPBARMSGDATAV3/APPBARDATAV2.
struct appbar_data {
	DWORD cbSize;
	DWORD hWnd;
	UINT uCallbackMessage;
	UINT uEdge;
	RECT rc;
	INT64 lParam;
};

struct appbar_msg {
	struct appbar_data abd;
	DWORD dwMessage;
	DWORD padding1;
	INT64 hSharedMemory;
	DWORD dwSourceProcessId;
	DWORD padding2;
};

_Static_assert(sizeof(struct appbar_data) == 40, "APPBARDATAV2 layout");
_Static_assert(sizeof(struct appbar_msg) == 64, "APPBARMSGDATAV3 layout");

// shlwapi exports these by ordinal only.
typedef void *(WINAPI *lock_shared_fn)(HANDLE, DWORD);
typedef BOOL (WINAPI *unlock_shared_fn)(void *);

// Where the "taskbar" is: swinewm's bar on the primary monitor, or the top
// edge of that monitor when there is no bar.
static UINT taskbar_pos(RECT *rc) {
	POINT origin = {0, 0};
	HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO mi = { .cbSize = sizeof(mi) };
	GetMonitorInfoW(monitor, &mi);
	if (IsRectEmpty(&host_rect)) {
		*rc = mi.rcMonitor;
		rc->bottom = rc->top;
		return ABE_TOP;
	}
	*rc = host_rect;
	int above = host_rect.top - mi.rcMonitor.top;
	int below = mi.rcMonitor.bottom - host_rect.bottom;
	return above <= below ? ABE_TOP : ABE_BOTTOM;
}

// Without Explorer (swinewm is the shell) there is nobody to forward AppBar
// messages to, and programs that place themselves next to the taskbar
// (the "Open with" dialog, flyouts) get no answer and end up off screen.
// Answer the queries like ManagedShell's AppBarManager; registering
// AppBars (ABM_NEW/QUERYPOS/SETPOS) is not supported yet.
static LRESULT handle_appbar(const COPYDATASTRUCT *cds) {
	if (cds->cbData != sizeof(struct appbar_msg)) {
		return 0;
	}
	const struct appbar_msg *amd = cds->lpData;
	switch (amd->dwMessage) {
	case ABM_GETTASKBARPOS: {
		static lock_shared_fn lock_shared;
		static unlock_shared_fn unlock_shared;
		if (!lock_shared) {
			HMODULE shlwapi = LoadLibraryW(L"shlwapi.dll");
			if (shlwapi) {
				lock_shared = (lock_shared_fn)(void (*)(void))GetProcAddress(
					shlwapi, MAKEINTRESOURCEA(8));
				unlock_shared = (unlock_shared_fn)(void (*)(void))GetProcAddress(
					shlwapi, MAKEINTRESOURCEA(9));
			}
		}
		if (!lock_shared || !unlock_shared) {
			return 0;
		}
		struct appbar_data *abd = lock_shared(
			(HANDLE)(INT_PTR)amd->hSharedMemory, amd->dwSourceProcessId);
		if (!abd) {
			return 0;
		}
		abd->uEdge = taskbar_pos(&abd->rc);
		unlock_shared(abd);
		return 1;
	}
	case ABM_GETSTATE:
		return ABS_ALWAYSONTOP;
	case ABM_GETAUTOHIDEBAR:
	case ABM_GETAUTOHIDEBAREX:
		return 0;
	case ABM_ACTIVATE:
	case ABM_WINDOWPOSCHANGED:
		return 1;
	default:
		return 0;
	}
}

static void prune_icons(void) {
	bool changed = false;
	EnterCriticalSection(&lock);
	for (int i = icons->length - 1; i >= 0; --i) {
		struct tray_icon *icon = icons->items[i];
		if (!IsWindow(icon->hwnd)) {
			list_del(icons, i);
			free_icon(icon);
			changed = true;
		}
	}
	LeaveCriticalSection(&lock);
	if (changed) {
		post_sync();
	}
}

static LRESULT CALLBACK tray_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
		LPARAM lparam) {
	switch (msg) {
	case WM_COPYDATA: {
		const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lparam;
		if (!cds || !cds->lpData) {
			return 0;
		}
		if (cds->dwData == 1 && cds->cbData >= offsetof(struct tray_data, nid) +
				offsetof(struct tray_nid, szTip)) {
			const struct tray_data *data = cds->lpData;
			if (data->magic != TRAY_MAGIC) {
				return FALSE;
			}
			return handle_notify(data->message, &data->nid,
				cds->cbData - (DWORD)offsetof(struct tray_data, nid));
		}
		if (cds->dwData == 3 && cds->cbData >= sizeof(struct tray_icon_query)) {
			return handle_icon_query(cds->lpData);
		}
		if (cds->dwData == 0 &&
				(win32_shell_mode || !win32_find_explorer_tray())) {
			return handle_appbar(cds);
		}
		return forward(msg, wparam, lparam);   // AppBar messages (0)
	}
	case WM_TIMER:
		if (wparam == TIMER_RAISE && !suspended &&
				FindWindowW(L"Shell_TrayWnd", NULL) != hwnd) {
			SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		} else if (wparam == TIMER_PRUNE) {
			prune_icons();
		}
		return 0;
	case WM_WINDOWPOSCHANGING: {
		WINDOWPOS *pos = (WINDOWPOS *)lparam;
		pos->flags &= ~(UINT)SWP_SHOWWINDOW;   // never shown
		return 0;
	}
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	default:
		if (msg >= WM_USER) {
			return forward(msg, wparam, lparam);
		}
		return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
}

static DWORD WINAPI tray_thread(LPVOID ready) {
	HINSTANCE instance = GetModuleHandleW(NULL);
	WNDCLASSEXW wc = { .cbSize = sizeof(wc) };
	wc.lpfnWndProc = tray_wndproc;
	wc.hInstance = instance;
	wc.lpszClassName = L"Shell_TrayWnd";
	RegisterClassExW(&wc);
	WNDCLASSEXW notify_wc = { .cbSize = sizeof(notify_wc) };
	notify_wc.lpfnWndProc = DefWindowProcW;
	notify_wc.hInstance = instance;
	notify_wc.lpszClassName = L"TrayNotifyWnd";
	RegisterClassExW(&notify_wc);

	// Some programs look for these windows and their size.
	tray_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
		L"Shell_TrayWnd", L"", WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
		host_rect.left, host_rect.top, host_rect.right - host_rect.left,
		host_rect.bottom - host_rect.top, NULL, NULL, instance, NULL);
	if (tray_hwnd) {
		CreateWindowExW(0, L"TrayNotifyWnd", NULL, WS_CHILD, 0, 0,
			host_rect.right - host_rect.left, host_rect.bottom - host_rect.top,
			tray_hwnd, NULL, instance, NULL);
		SetTimer(tray_hwnd, TIMER_RAISE, 100, NULL);
		SetTimer(tray_hwnd, TIMER_PRUNE, 2000, NULL);
	}
	SetEvent((HANDLE)ready);
	if (!tray_hwnd) {
		return 1;
	}
	// Running programs add their icons again, to us.
	SendNotifyMessageW(HWND_BROADCAST, taskbar_created, 0, 0);
	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	tray_hwnd = NULL;
	return 0;
}

// As the shell, the volume, network and power icons are missing: Explorer
// gets them from the SysTray shell service object (stobject.dll), which
// adds them with Shell_NotifyIcon. ManagedShell's ShellServiceObject.
static const CLSID clsid_systray = {0x35CEC8A3, 0x2BE6, 0x11D2,
	{0x87, 0x73, 0x92, 0xE2, 0x20, 0x52, 0x41, 0x53}};
static const GUID cgid_shell_service_object = {0x000214D2, 0x0000, 0x0000,
	{0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

static void systray_object_start(void) {
	if (!win32_shell_mode || systray_object) {
		return;
	}
	if (FAILED(CoCreateInstance(&clsid_systray, NULL, CLSCTX_INPROC_SERVER,
			&IID_IOleCommandTarget, (void **)&systray_object))) {
		swine_log(SWINE_INFO, "Unable to load the system icons (SysTray)");
		systray_object = NULL;
		return;
	}
	IOleCommandTarget_Exec(systray_object, &cgid_shell_service_object,
		OLECMDID_NEW, OLECMDEXECOPT_DODEFAULT, NULL, NULL);
}

static void systray_object_stop(void) {
	if (!systray_object) {
		return;
	}
	IOleCommandTarget_Exec(systray_object, &cgid_shell_service_object,
		OLECMDID_SAVE, OLECMDEXECOPT_DODEFAULT, NULL, NULL);
	IOleCommandTarget_Release(systray_object);
	systray_object = NULL;
}

static void tray_start(void) {
	if (!lock_ready) {
		InitializeCriticalSection(&lock);
		lock_ready = true;
	}
	taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
	icons = create_list();
	suspended = 0;
	HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
	thread = CreateThread(NULL, 0, tray_thread, ready, 0, NULL);
	if (!thread) {
		CloseHandle(ready);
		return;
	}
	WaitForSingleObject(ready, 5000);
	CloseHandle(ready);
	swine_log(SWINE_DEBUG, "Hosting the notification area");
	systray_object_start();
}

// Programs add their icons to whichever Shell_TrayWnd is found first.
static void hand_back_icons(void) {
	SendNotifyMessageW(HWND_BROADCAST,
		RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);
}

static void tray_stop(void) {
	if (!thread) {
		return;
	}
	systray_object_stop();
	if (tray_hwnd) {
		PostMessageW(tray_hwnd, WM_CLOSE, 0, 0);
	}
	if (WaitForSingleObject(thread, 3000) != WAIT_OBJECT_0) {
		swine_log(SWINE_ERROR, "The tray thread did not stop");
	}
	CloseHandle(thread);
	thread = NULL;
	EnterCriticalSection(&lock);
	for (int i = 0; i < icons->length; ++i) {
		free_icon(icons->items[i]);
	}
	list_free(icons);
	icons = NULL;
	LeaveCriticalSection(&lock);
	tray_clear();
	hand_back_icons();
	swine_log(SWINE_DEBUG, "Stopped hosting the notification area");
}

void win32_tray_update(void) {
	bool wanted = bar_has_tray();
	if (wanted && !thread) {
		tray_start();
	} else if (!wanted && thread) {
		tray_stop();
	}
}

void win32_tray_finish(void) {
	tray_stop();
	for (int i = 0; icon_files && i < icon_files->length; ++i) {
		struct icon_file *f = icon_files->items[i];
		if (f->icon) {
			DestroyIcon(f->icon);
		}
		free(f->path);
		free(f);
	}
	list_free(icon_files);
	icon_files = NULL;
}

// After a crash: the tray thread may be stuck, so instead of destroying
// its window, drop it to the bottom (FindWindow then finds Explorer's
// first) and have programs add their icons again.
void win32_tray_crash(void) {
	if (!thread) {
		return;
	}
	suspended = 1;
	if (tray_hwnd) {
		SetWindowPos(tray_hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE |
			SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
	}
	hand_back_icons();
}

void win32_tray_recover(void) {
	hand_back_icons();
}

// --- main thread: the core's copy, drawing and clicks -------------------

void win32_tray_handle_message(WPARAM wparam, LPARAM lparam) {
	if (wparam == 1) {
		struct tray_balloon *b = (struct tray_balloon *)lparam;
		struct notify_params p = {0};
		p.app_name = b->app_name;
		p.summary = b->summary;
		p.body = b->body;
		// Balloons are transient, even "error" ones; critical
		// notifications would stay until dismissed.
		p.urgency = NOTIFY_URGENCY_NORMAL;
		p.expire_timeout = -1;
		notify_add(&p, platform_now_ms());
		free(b->app_name);
		free(b->summary);
		free(b->body);
		free(b);
		platform_ui_refresh();
		return;
	}
	InterlockedExchange(&sync_posted, 0);
	if (!thread) {
		return;
	}
	EnterCriticalSection(&lock);
	list_t *core = tray_get_items();
	for (int i = core->length - 1; i >= 0; --i) {
		struct tray_item *item = core->items[i];
		if (!find_by_id(item->id)) {
			tray_item_remove(item->id);
		}
	}
	for (int i = 0; i < icons->length; ++i) {
		struct tray_icon *icon = icons->items[i];
		char *tip = utf16_to_utf8(icon->tip);
		tray_item_set(icon->id, icon->app_id, tip, icon->hidden);
		free(tip);
	}
	LeaveCriticalSection(&lock);
	win32_bar_refresh();
}

static HICON icon_from_file(const char *path, int size) {
	if (!icon_files) {
		icon_files = create_list();
	}
	for (int i = 0; i < icon_files->length; ++i) {
		struct icon_file *f = icon_files->items[i];
		if (f->size == size && strcmp(f->path, path) == 0) {
			return f->icon;
		}
	}
	struct icon_file *f = calloc(1, sizeof(*f));
	f->path = swine_strdup(path);
	f->size = size;
	wchar_t *wpath = utf8_to_utf16(path);
	f->icon = LoadImageW(NULL, wpath, IMAGE_ICON, size, size, LR_LOADFROMFILE);
	free(wpath);
	if (!f->icon) {
		swine_log(SWINE_ERROR, "Unable to load tray icon %s", path);
	}
	list_add(icon_files, f);
	return f->icon;
}

void win32_tray_draw(HDC dc, uint64_t id, const char *icon_path,
		const RECT *rect, HWND bar) {
	int size = rect->right - rect->left;
	HICON file_icon = icon_path ? icon_from_file(icon_path, size) : NULL;
	if (!thread) {
		return;
	}
	EnterCriticalSection(&lock);
	struct tray_icon *icon = find_by_id(id);
	if (icon) {
		HICON h = file_icon ? file_icon : icon->icon;
		if (!h) {
			// No icon, or one that could not be copied: like ManagedShell,
			// show a generic one so the item can still be clicked.
			h = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
		}
		if (h) {
			DrawIconEx(dc, rect->left, rect->top, h, size, size, 0, NULL,
				DI_NORMAL);
		}
		RECT screen = *rect;
		MapWindowPoints(bar, NULL, (POINT *)&screen, 2);
		icon->rect = screen;
	}
	LeaveCriticalSection(&lock);
}

// Tells the tray window where the tray is, for programs that position
// their menus next to Shell_TrayWnd.
void win32_tray_set_host_rect(const RECT *screen) {
	if (EqualRect(&host_rect, screen)) {
		return;
	}
	host_rect = *screen;
	if (tray_hwnd) {
		SetWindowPos(tray_hwnd, NULL, screen->left, screen->top,
			screen->right - screen->left, screen->bottom - screen->top,
			SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
	}
}

// Mouse buttons on an icon, encoded the way Explorer does (ManagedShell's
// NotifyIcon.IconMouseDown/Up): version 4 icons get the position in wParam
// and the icon id in the high word of lParam, older ones the id in wParam.
// The pointer over an icon, as Explorer reports it: WM_MOUSEMOVE as it
// moves (TRAY_HOVER_MOVE); after the hover delay (TRAY_HOVER_POPUP)
// NIN_POPUPOPEN for version 4 icons that show their own popup, and true if
// swinewm should show the tooltip text instead; NIN_POPUPCLOSE when it
// leaves (TRAY_HOVER_LEAVE).
bool win32_tray_hover(uint64_t id, enum tray_hover what, POINT screen) {
	if (!thread) {
		return false;
	}
	EnterCriticalSection(&lock);
	struct tray_icon *icon = find_by_id(id);
	if (!icon) {
		LeaveCriticalSection(&lock);
		return false;
	}
	HWND hwnd = icon->hwnd;
	UINT callback = icon->callback, uid = icon->uid, version = icon->version;
	bool own_popup = version > 3 && !icon->show_tip;
	bool show_text = !own_popup && icon->tip[0];
	UINT msg = 0;
	if (what == TRAY_HOVER_MOVE) {
		msg = WM_MOUSEMOVE;
	} else if (what == TRAY_HOVER_POPUP && own_popup && !icon->popup_open) {
		msg = NIN_POPUPOPEN;
		icon->popup_open = true;
	} else if (what == TRAY_HOVER_LEAVE && icon->popup_open) {
		msg = NIN_POPUPCLOSE;
		icon->popup_open = false;
	}
	LeaveCriticalSection(&lock);
	if (msg && IsWindow(hwnd) && callback) {
		WPARAM wparam = version > 3 ?
			MAKEWPARAM((WORD)screen.x, (WORD)screen.y) : (WPARAM)uid;
		LPARAM high = version > 3 ? (LPARAM)uid << 16 : 0;
		SendNotifyMessageW(hwnd, callback, wparam, (LPARAM)msg | high);
	}
	return what == TRAY_HOVER_POPUP && show_text;
}

void win32_tray_mouse(uint64_t id, int button, bool down, POINT screen) {
	static const UINT down_msg[3] = {WM_LBUTTONDOWN, WM_MBUTTONDOWN,
		WM_RBUTTONDOWN};
	static const UINT dblclk_msg[3] = {WM_LBUTTONDBLCLK, WM_MBUTTONDBLCLK,
		WM_RBUTTONDBLCLK};
	static const UINT up_msg[3] = {WM_LBUTTONUP, WM_MBUTTONUP, WM_RBUTTONUP};
	if (!thread || button < 0 || button > 2) {
		return;
	}
	EnterCriticalSection(&lock);
	struct tray_icon *icon = find_by_id(id);
	if (!icon) {
		LeaveCriticalSection(&lock);
		return;
	}
	HWND hwnd = icon->hwnd;
	UINT callback = icon->callback, uid = icon->uid, version = icon->version;
	DWORD pid = icon->pid;
	bool dblclk = false;
	if (down) {
		ULONGLONG now = GetTickCount64();
		dblclk = now - icon->last_down[button] <= GetDoubleClickTime();
		icon->last_down[button] = dblclk ? 0 : now;
	}
	LeaveCriticalSection(&lock);
	if (!IsWindow(hwnd) || !callback) {
		return;
	}
	// The program may take the focus to show a menu or its window.
	AllowSetForegroundWindow(pid);
	WPARAM wparam = version > 3 ?
		MAKEWPARAM((WORD)screen.x, (WORD)screen.y) : (WPARAM)uid;
	LPARAM high = version > 3 ? (LPARAM)uid << 16 : 0;
	UINT msgs[2] = {0, 0};
	if (down) {
		msgs[0] = dblclk ? dblclk_msg[button] : down_msg[button];
	} else {
		msgs[0] = up_msg[button];
		// Documented for version 4; Explorer sends them for 3 as well.
		if (version >= 3 && button == 0) {
			msgs[1] = NIN_SELECT;
		} else if (version >= 3 && button == 2) {
			msgs[1] = WM_CONTEXTMENU;
		}
	}
	for (int i = 0; i < 2 && msgs[i]; ++i) {
		SendNotifyMessageW(hwnd, callback, wparam, (LPARAM)msgs[i] | high);
	}
}
