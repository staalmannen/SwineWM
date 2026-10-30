// Toasts of Windows applications, mirrored as notifications of the
// built-in Mako (windows-notifications= in the mako config).
//
// The WinRT UserNotificationListener reads them. Without package identity
// its NotificationChanged event fails (0x80070490), but reading the list
// works, so a thread reads it when Windows writes its notification store
// (%LOCALAPPDATA%\Microsoft\Windows\Notifications) and every two seconds,
// and posts new and removed toasts to the shell window. The user allows
// it under Settings > Privacy & security > Notifications.
//
// No WinRT headers: the few interfaces are called through their vtables
// (slots from the Windows metadata; 0-2 IUnknown, 3-5 IInspectable), and
// the WinRT runtime functions come from combase.dll at run time.
#include <stdlib.h>
#include <string.h>
#include "list.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/notify.h"
#include "swinewm/platform.h"
#include "win32.h"

typedef void *hstring;

static HRESULT (WINAPI *ro_initialize)(int type);
static void (WINAPI *ro_uninitialize)(void);
static HRESULT (WINAPI *ro_get_activation_factory)(hstring, REFIID, void **);
static HRESULT (WINAPI *create_string)(const wchar_t *, UINT32, hstring *);
static HRESULT (WINAPI *delete_string)(hstring);
static const wchar_t *(WINAPI *string_buffer)(hstring, UINT32 *);

static const IID iid_listener_statics = {0xff6123cf, 0x4386, 0x4aa3,
	{0xb7, 0x3d, 0xb8, 0x04, 0xe5, 0xb6, 0x3b, 0x23}};
static const IID iid_async_info = {0x00000036, 0x0000, 0x0000,
	{0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

#define SLOT(obj, n) ((*(void ***)(obj))[n])
typedef HRESULT (STDMETHODCALLTYPE *get_ptr_fn)(void *, void **);
typedef HRESULT (STDMETHODCALLTYPE *get_u32_fn)(void *, UINT32 *);
typedef HRESULT (STDMETHODCALLTYPE *get_int_fn)(void *, int *);
typedef HRESULT (STDMETHODCALLTYPE *u32_ptr_fn)(void *, UINT32, void **);
typedef HRESULT (STDMETHODCALLTYPE *u32_fn)(void *, UINT32);
typedef HRESULT (STDMETHODCALLTYPE *str_ptr_fn)(void *, hstring, void **);
typedef HRESULT (STDMETHODCALLTYPE *qi_fn)(void *, REFIID, void **);
typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);

// Vtable slots, after the six of IInspectable.
enum {
	STATICS_CURRENT = 6,
	LISTENER_REQUEST_ACCESS = 6,
	LISTENER_ACCESS_STATUS = 7,
	LISTENER_GET_NOTIFICATIONS = 10,
	LISTENER_REMOVE = 13,
	ASYNC_GET_RESULTS = 8,
	ASYNC_INFO_STATUS = 7,
	VECTOR_GET_AT = 6,
	VECTOR_SIZE = 7,
	USER_NOTIFICATION = 6,
	USER_APP_INFO = 7,
	USER_ID = 8,
	NOTIFICATION_VISUAL = 8,
	VISUAL_GET_BINDING = 9,
	BINDING_TEXT_ELEMENTS = 11,
	TEXT_TEXT = 6,
	APP_INFO_AUMID = 7,
	APP_INFO_DISPLAY_INFO = 8,
	DISPLAY_INFO_NAME = 6,
};

static void release(void *obj) {
	if (obj) {
		((release_fn)SLOT(obj, 2))(obj);
	}
}

static void *get_ptr(void *obj, int slot) {
	void *out = NULL;
	if (!obj || FAILED(((get_ptr_fn)SLOT(obj, slot))(obj, &out))) {
		return NULL;
	}
	return out;
}

// A getter returning an HSTRING, as UTF-8 ("" if it fails).
static char *get_string(void *obj, int slot) {
	hstring s = NULL;
	char *out = NULL;
	if (obj && SUCCEEDED(((get_ptr_fn)SLOT(obj, slot))(obj, &s)) && s) {
		out = utf16_to_utf8(string_buffer(s, NULL));
		delete_string(s);
	}
	return out ? out : swine_strdup("");
}

// Waits for an IAsyncOperation and returns its GetResults output.
static bool async_wait(void *op, void *result, HANDLE stop) {
	void *info = NULL;
	if (!op || FAILED(((qi_fn)SLOT(op, 0))(op, &iid_async_info, &info))) {
		return false;
	}
	int status = 0;
	while (SUCCEEDED(((get_int_fn)SLOT(info, ASYNC_INFO_STATUS))(info, &status))
			&& status == 0) {
		if (WaitForSingleObject(stop, 10) == WAIT_OBJECT_0) {
			break;
		}
	}
	release(info);
	return status == 1 &&   // Completed
		SUCCEEDED(((get_ptr_fn)SLOT(op, ASYNC_GET_RESULTS))(op, result));
}

static bool load_runtime(void) {
	HMODULE combase = LoadLibraryW(L"combase.dll");
	if (!combase) {
		return false;
	}
	*(FARPROC *)&ro_initialize = GetProcAddress(combase, "RoInitialize");
	*(FARPROC *)&ro_uninitialize = GetProcAddress(combase, "RoUninitialize");
	*(FARPROC *)&ro_get_activation_factory =
		GetProcAddress(combase, "RoGetActivationFactory");
	*(FARPROC *)&create_string = GetProcAddress(combase, "WindowsCreateString");
	*(FARPROC *)&delete_string = GetProcAddress(combase, "WindowsDeleteString");
	*(FARPROC *)&string_buffer =
		GetProcAddress(combase, "WindowsGetStringRawBuffer");
	return ro_initialize && ro_uninitialize && ro_get_activation_factory &&
		create_string && delete_string && string_buffer;
}

static hstring make_string(const wchar_t *text) {
	hstring s = NULL;
	create_string(text, (UINT32)wcslen(text), &s);
	return s;
}

// --- the thread ----------------------------------------------------------

// Posted to the shell window (WM_SWINE_TOAST).
struct toast_message {
	uint32_t toast_id;
	bool removed;
	char *app_name;
	char *aumid;
	char *summary;
	char *body;
};

static HANDLE thread = NULL;
static bool failed = false;          // until the config is loaded again
static HANDLE stop_event = NULL;
static HANDLE wake_event = NULL;     // removals queued
static CRITICAL_SECTION lock;        // removals
static bool lock_ready = false;
static uint32_t *removals = NULL;
static int removal_count = 0;

static void free_message(struct toast_message *m) {
	free(m->app_name);
	free(m->aumid);
	free(m->summary);
	free(m->body);
	free(m);
}

static void post(struct toast_message *m) {
	if (!win32_shell_hwnd ||
			!PostMessageW(win32_shell_hwnd, WM_SWINE_TOAST, 0, (LPARAM)m)) {
		free_message(m);
	}
}

// The texts of a toast: the first is its title, the rest its body.
static void read_texts(void *user_notification, struct toast_message *m) {
	void *notification = get_ptr(user_notification, USER_NOTIFICATION);
	void *visual = get_ptr(notification, NOTIFICATION_VISUAL);
	void *binding = NULL;
	if (visual) {
		hstring generic = make_string(L"ToastGeneric");
		((str_ptr_fn)SLOT(visual, VISUAL_GET_BINDING))(visual, generic, &binding);
		delete_string(generic);
	}
	void *texts = get_ptr(binding, BINDING_TEXT_ELEMENTS);
	UINT32 count = 0;
	if (texts) {
		((get_u32_fn)SLOT(texts, VECTOR_SIZE))(texts, &count);
	}
	char *body = swine_strdup("");
	for (UINT32 i = 0; i < count; ++i) {
		void *text = NULL;
		if (FAILED(((u32_ptr_fn)SLOT(texts, VECTOR_GET_AT))(texts, i, &text))) {
			continue;
		}
		char *line = get_string(text, TEXT_TEXT);
		release(text);
		if (!m->summary) {
			m->summary = line;
		} else {
			char *next = format_str("%s%s%s", body, *body ? "\n" : "", line);
			free(body);
			free(line);
			body = next;
		}
	}
	if (!m->summary) {
		m->summary = swine_strdup("");
	}
	m->body = body;
	release(texts);
	release(binding);
	release(visual);
	release(notification);
}

static struct toast_message *read_toast(void *user_notification, uint32_t id) {
	struct toast_message *m = calloc(1, sizeof(*m));
	m->toast_id = id;
	void *app = get_ptr(user_notification, USER_APP_INFO);
	void *display = get_ptr(app, APP_INFO_DISPLAY_INFO);
	m->aumid = get_string(app, APP_INFO_AUMID);
	m->app_name = get_string(display, DISPLAY_INFO_NAME);
	release(display);
	release(app);
	read_texts(user_notification, m);
	return m;
}

static bool contains(const uint32_t *ids, int count, uint32_t id) {
	for (int i = 0; i < count; ++i) {
		if (ids[i] == id) {
			return true;
		}
	}
	return false;
}

// Reads the toasts and posts what changed since *seen. Returns false if
// the listener stopped working.
static bool scan(void *listener, uint32_t **seen, int *seen_count,
		bool announce) {
	void *op = NULL;
	if (FAILED(((u32_ptr_fn)SLOT(listener, LISTENER_GET_NOTIFICATIONS))(
			listener, 1 /* NotificationKinds.Toast */, &op))) {
		return false;
	}
	void *list = NULL;
	bool ok = async_wait(op, &list, stop_event);
	release(op);
	if (!ok || !list) {
		return false;
	}
	UINT32 count = 0;
	((get_u32_fn)SLOT(list, VECTOR_SIZE))(list, &count);
	uint32_t *ids = calloc((size_t)count + 1, sizeof(uint32_t));
	int n = 0;
	for (UINT32 i = 0; i < count; ++i) {
		void *un = NULL;
		if (FAILED(((u32_ptr_fn)SLOT(list, VECTOR_GET_AT))(list, i, &un))) {
			continue;
		}
		UINT32 id = 0;
		((get_u32_fn)SLOT(un, USER_ID))(un, &id);
		ids[n++] = id;
		if (announce && !contains(*seen, *seen_count, id)) {
			post(read_toast(un, id));
		}
		release(un);
	}
	release(list);
	for (int i = 0; i < *seen_count; ++i) {
		if (!contains(ids, n, (*seen)[i])) {
			struct toast_message *m = calloc(1, sizeof(*m));
			m->toast_id = (*seen)[i];
			m->removed = true;
			post(m);
		}
	}
	free(*seen);
	*seen = ids;
	*seen_count = n;
	return true;
}

static void remove_queued(void *listener) {
	EnterCriticalSection(&lock);
	uint32_t *ids = removals;
	int count = removal_count;
	removals = NULL;
	removal_count = 0;
	LeaveCriticalSection(&lock);
	for (int i = 0; i < count; ++i) {
		((u32_fn)SLOT(listener, LISTENER_REMOVE))(listener, ids[i]);
	}
	free(ids);
}

static void *open_listener(void) {
	hstring name = make_string(
		L"Windows.UI.Notifications.Management.UserNotificationListener");
	void *statics = NULL;
	HRESULT hr = ro_get_activation_factory(name, &iid_listener_statics,
		&statics);
	delete_string(name);
	if (FAILED(hr)) {
		swine_log(SWINE_ERROR, "Windows notifications: no "
			"UserNotificationListener (0x%08lx)", (unsigned long)hr);
		return NULL;
	}
	void *listener = get_ptr(statics, STATICS_CURRENT);
	release(statics);
	if (!listener) {
		return NULL;
	}
	int status = 0;
	((get_int_fn)SLOT(listener, LISTENER_ACCESS_STATUS))(listener, &status);
	if (status == 0) {   // Unspecified: ask
		void *op = NULL;
		if (SUCCEEDED(((get_ptr_fn)SLOT(listener, LISTENER_REQUEST_ACCESS))(
				listener, &op))) {
			async_wait(op, &status, stop_event);
			release(op);
		}
	}
	if (status != 1) {   // not Allowed
		swine_log(SWINE_ERROR, "Windows notifications: access denied. Allow "
			"it in Settings > Privacy & security > Notifications "
			"(ms-settings:privacy-notifications), then reload");
		release(listener);
		return NULL;
	}
	return listener;
}

static DWORD WINAPI toast_thread(LPVOID param) {
	(void)param;
	if (FAILED(ro_initialize(1 /* RO_INIT_MULTITHREADED */))) {
		return 1;
	}
	void *listener = open_listener();
	if (listener) {
		swine_log(SWINE_INFO, "Showing the notifications of Windows "
			"applications");
		wchar_t dir[MAX_PATH];
		DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
		HANDLE watch = INVALID_HANDLE_VALUE;
		if (len && len < MAX_PATH - 40) {
			wcscat(dir, L"\\Microsoft\\Windows\\Notifications");
			watch = FindFirstChangeNotificationW(dir, TRUE,
				FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
				FILE_NOTIFY_CHANGE_FILE_NAME);
		}
		uint32_t *seen = NULL;
		int seen_count = 0;
		// What is already there was shown before: only mark it seen.
		bool ok = scan(listener, &seen, &seen_count, false);
		while (ok) {
			HANDLE handles[3] = {stop_event, wake_event, watch};
			DWORD n = watch != INVALID_HANDLE_VALUE ? 3 : 2;
			DWORD r = WaitForMultipleObjects(n, handles, FALSE, 2000);
			if (r == WAIT_OBJECT_0) {
				break;
			}
			if (r == WAIT_OBJECT_0 + 2) {
				FindNextChangeNotification(watch);
				// Let Windows finish writing before reading.
				if (WaitForSingleObject(stop_event, 150) == WAIT_OBJECT_0) {
					break;
				}
			}
			remove_queued(listener);
			ok = scan(listener, &seen, &seen_count, true);
		}
		if (!ok) {
			swine_log(SWINE_ERROR, "Windows notifications: reading them "
				"failed; stopped");
		}
		free(seen);
		if (watch != INVALID_HANDLE_VALUE) {
			FindCloseChangeNotification(watch);
		}
		release(listener);
	}
	ro_uninitialize();
	return 0;
}

// --- main thread ---------------------------------------------------------

struct mirrored {
	uint32_t toast_id;
	uint32_t notification_id;
	char *aumid;     // its application, opened by a click
};
static list_t *mirrored = NULL;   // struct mirrored *

static void stop_thread(void) {
	if (!thread) {
		return;
	}
	SetEvent(stop_event);
	WaitForSingleObject(thread, 5000);
	CloseHandle(thread);
	thread = NULL;
}

void win32_toasts_update(void) {
	enum notify_windows mode = notify_windows_mode();
	bool want = mode == NOTIFY_WINDOWS_ON ||
		(mode == NOTIFY_WINDOWS_AUTO && win32_shell_mode);
	if (thread && WaitForSingleObject(thread, 0) == WAIT_OBJECT_0) {
		// It gave up (no access): try again after the next reload.
		CloseHandle(thread);
		thread = NULL;
		failed = true;
	}
	if (want == (thread != NULL) || (want && failed)) {
		return;
	}
	if (!want) {
		stop_thread();
		return;
	}
	if (!ro_initialize && !load_runtime()) {
		swine_log(SWINE_ERROR, "Windows notifications: no WinRT runtime");
		return;
	}
	if (!lock_ready) {
		InitializeCriticalSection(&lock);
		stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
		wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
		lock_ready = true;
	}
	ResetEvent(stop_event);
	thread = CreateThread(NULL, 0, toast_thread, NULL, 0, NULL);
}

void win32_toasts_config_changed(void) {
	failed = false;
}

static struct mirrored *find(uint32_t toast_id, uint32_t notification_id,
		int *index) {
	for (int i = 0; mirrored && i < mirrored->length; ++i) {
		struct mirrored *m = mirrored->items[i];
		if ((toast_id && m->toast_id == toast_id) ||
				(notification_id && m->notification_id == notification_id)) {
			*index = i;
			return m;
		}
	}
	return NULL;
}

static void forget(int index) {
	struct mirrored *m = mirrored->items[index];
	free(m->aumid);
	free(m);
	list_del(mirrored, index);
}

void win32_toast_message(LPARAM lparam) {
	struct toast_message *t = (struct toast_message *)lparam;
	if (!mirrored) {
		mirrored = create_list();
	}
	int index;
	struct mirrored *m = find(t->toast_id, 0, &index);
	if (t->removed) {
		// Gone from Windows (the app took it back, or the user cleared it).
		if (m) {
			uint32_t id = m->notification_id;
			forget(index);
			notify_close(id);
			platform_ui_refresh();
		}
	} else if (!m && thread) {
		char *icon = *t->aumid ? format_str("shell:AppsFolder\\%s", t->aumid) :
			NULL;
		struct notify_params p = {
			.app_name = t->app_name,
			.summary = t->summary,
			.body = t->body,
			.app_icon = icon,
			.desktop_entry = t->aumid,
			.urgency = NOTIFY_URGENCY_NORMAL,
			.expire_timeout = -1,
		};
		// A click on a toast opens its application (with the toast's own
		// arguments, which the listener does not give out); here the
		// default action does the same, without them.
		const char *keys[] = {"default"}, *titles[] = {"Open"};
		if (*t->aumid) {
			p.action_keys = keys;
			p.action_titles = titles;
			p.action_count = 1;
		}
		struct mirrored *entry = calloc(1, sizeof(*entry));
		entry->toast_id = t->toast_id;
		entry->aumid = swine_strdup(t->aumid);
		list_add(mirrored, entry);
		entry->notification_id = notify_add(&p, platform_now_ms());
		free(icon);
		platform_ui_refresh();
	}
	free_message(t);
}

void platform_notification_closed(uint32_t id, int reason) {
	int index;
	struct mirrored *m = find(0, id, &index);
	if (!m) {
		return;
	}
	uint32_t toast_id = m->toast_id;
	forget(index);
	if (reason == NOTIFY_CLOSE_DISMISSED && thread) {
		// Dismissed here: gone from Windows' list too, as clearing it
		// in the notification center would do.
		EnterCriticalSection(&lock);
		removals = realloc(removals, sizeof(uint32_t) * (size_t)(removal_count + 1));
		removals[removal_count++] = toast_id;
		LeaveCriticalSection(&lock);
		SetEvent(wake_event);
	}
}

void platform_notification_action(uint32_t id, const char *action) {
	int index;
	struct mirrored *m = find(0, id, &index);
	if (m && *m->aumid && strcmp(action, "default") == 0) {
		char *command = format_str("shell:AppsFolder\\%s", m->aumid);
		platform_spawn(command);
		free(command);
	}
}

void win32_toasts_finish(void) {
	stop_thread();
	while (mirrored && mirrored->length) {
		forget(mirrored->length - 1);
	}
	if (mirrored) {
		list_free(mirrored);
		mirrored = NULL;
	}
	if (lock_ready) {
		free(removals);
		removals = NULL;
		removal_count = 0;
		CloseHandle(stop_event);
		CloseHandle(wake_event);
		DeleteCriticalSection(&lock);
		lock_ready = false;
	}
}
