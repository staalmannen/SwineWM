// Media players for Waybar's mpris module: Windows' media sessions, the
// ones the volume flyout shows (Windows.Media.Control's
// GlobalSystemMediaTransportControlsSessionManager).
//
// As in toast.c, no WinRT headers: the interfaces are called through their
// vtables (slots and IIDs from the Windows metadata; 0-2 IUnknown, 3-5
// IInspectable), the runtime functions come from combase.dll. A thread of
// its own reads the sessions every second (getting a track's properties is
// asynchronous) and makes the play/pause/next/previous calls; the bar only
// reads what it last found.
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/sysinfo.h"
#include "win32.h"

typedef void *hstring;

static HRESULT (WINAPI *ro_initialize)(int type);
static void (WINAPI *ro_uninitialize)(void);
static HRESULT (WINAPI *ro_get_activation_factory)(hstring, REFIID, void **);
static HRESULT (WINAPI *create_string)(const wchar_t *, UINT32, hstring *);
static HRESULT (WINAPI *delete_string)(hstring);
static const wchar_t *(WINAPI *string_buffer)(hstring, UINT32 *);

// IGlobalSystemMediaTransportControlsSessionManagerStatics
static const IID iid_manager_statics = {0x2050c4ee, 0x11a0, 0x57de,
	{0xae, 0xd7, 0xc9, 0x7c, 0x70, 0x33, 0x82, 0x45}};
static const IID iid_async_info = {0x00000036, 0x0000, 0x0000,
	{0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

#define SLOT(obj, n) ((*(void ***)(obj))[n])
typedef HRESULT (STDMETHODCALLTYPE *get_ptr_fn)(void *, void **);
typedef HRESULT (STDMETHODCALLTYPE *get_u32_fn)(void *, UINT32 *);
typedef HRESULT (STDMETHODCALLTYPE *get_int_fn)(void *, int *);
typedef HRESULT (STDMETHODCALLTYPE *get_i64_fn)(void *, INT64 *);
typedef HRESULT (STDMETHODCALLTYPE *u32_ptr_fn)(void *, UINT32, void **);
typedef HRESULT (STDMETHODCALLTYPE *qi_fn)(void *, REFIID, void **);
typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);

// Vtable slots, after the six of IInspectable.
enum {
	STATICS_REQUEST = 6,              // RequestAsync
	MANAGER_CURRENT = 6,              // GetCurrentSession
	MANAGER_SESSIONS = 7,             // GetSessions
	SESSION_AUMID = 6,                // get_SourceAppUserModelId
	SESSION_PROPERTIES = 7,           // TryGetMediaPropertiesAsync
	SESSION_TIMELINE = 8,             // GetTimelineProperties
	SESSION_PLAYBACK = 9,             // GetPlaybackInfo
	SESSION_NEXT = 16,                // TrySkipNextAsync
	SESSION_PREVIOUS = 17,            // TrySkipPreviousAsync
	SESSION_TOGGLE = 20,              // TryTogglePlayPauseAsync
	PROPERTIES_TITLE = 6,
	PROPERTIES_ALBUM_ARTIST = 8,
	PROPERTIES_ARTIST = 9,
	PROPERTIES_ALBUM = 10,
	TIMELINE_START = 6,
	TIMELINE_END = 7,
	TIMELINE_POSITION = 10,
	TIMELINE_UPDATED = 11,            // a DateTime
	PLAYBACK_STATUS = 7,
	ASYNC_INFO_STATUS = 7,
	ASYNC_GET_RESULTS = 8,
	VECTOR_GET_AT = 6,
	VECTOR_SIZE = 7,
};

// GlobalSystemMediaTransportControlsSessionPlaybackStatus
enum {
	PLAYBACK_CLOSED = 0,
	PLAYBACK_STOPPED = 3,
	PLAYBACK_PLAYING = 4,
	PLAYBACK_PAUSED = 5,
};

#define POLL_MS 1000
#define MAX_PLAYERS 8

static CRITICAL_SECTION lock;
static bool lock_ready = false;
static struct sysinfo_media players[MAX_PLAYERS];
static int player_count = 0;
static HANDLE thread = NULL;
static HANDLE wake = NULL;
static volatile LONG stopping = 0;
static bool failed = false;
// Requests for the thread: one pending action (the latest wins).
static char request_player[64];
static int request_action = -1;

static void release(void *obj) {
	if (obj) {
		((release_fn)SLOT(obj, 2))(obj);
	}
}

static void *get_ptr(void *obj, int slot) {
	void *out = NULL;
	if (obj && FAILED(((get_ptr_fn)SLOT(obj, slot))(obj, &out))) {
		out = NULL;
	}
	return out;
}

static void get_string(void *obj, int slot, char *out, size_t size) {
	hstring s = NULL;
	out[0] = '\0';
	if (obj && SUCCEEDED(((get_ptr_fn)SLOT(obj, slot))(obj, (void **)&s)) && s) {
		char *utf8 = utf16_to_utf8(string_buffer(s, NULL));
		if (utf8) {
			snprintf(out, size, "%s", utf8);
			// mpris's sanitize: no line breaks in one-line fields
			for (char *c = out; *c; ++c) {
				if (*c == '\n' || *c == '\r') {
					*c = ' ';
				}
			}
			free(utf8);
		}
		delete_string(s);
	}
}

// Waits (at most timeout_ms) for an IAsyncOperation; its result, or NULL.
static void *async_result(void *op, DWORD timeout_ms) {
	void *info = NULL, *result = NULL;
	if (!op || FAILED(((qi_fn)SLOT(op, 0))(op, &iid_async_info, &info))) {
		return NULL;
	}
	int status = 0;
	DWORD waited = 0;
	while (SUCCEEDED(((get_int_fn)SLOT(info, ASYNC_INFO_STATUS))(info, &status))
			&& status == 0 && waited < timeout_ms && !stopping) {
		Sleep(10);
		waited += 10;
	}
	release(info);
	if (status != 1 ||   // Completed
			FAILED(((get_ptr_fn)SLOT(op, ASYNC_GET_RESULTS))(op, &result))) {
		result = NULL;
	}
	return result;
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

// A player's name from its app's AppUserModelID, the way MPRIS names
// players: "Spotify.exe" and "SpotifyAB.SpotifyMusic_...!Spotify" are
// "spotify", "Chrome" is "chrome", "MSEdge" "msedge".
static void player_name(const char *aumid, char *out, size_t size) {
	static const struct { const char *aumid; const char *name; } known[] = {
		{"308046B0AF4A39CB", "firefox"},    // Firefox's hashed ID
		{"6F193CCC56814779", "firefox"},    // Firefox Nightly
		{"E7CF176E110C211B", "firefox"},    // Firefox Developer Edition
	};
	for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
		if (swine_strcasecmp(aumid, known[i].aumid) == 0) {
			snprintf(out, size, "%s", known[i].name);
			return;
		}
	}
	const char *bang = strrchr(aumid, '!');
	const char *start = bang ? bang + 1 : aumid;
	char buf[256];
	snprintf(buf, sizeof(buf), "%s", start);
	size_t len = strlen(buf);
	if (len > 4 && swine_strcasecmp(buf + len - 4, ".exe") == 0) {
		buf[len - 4] = '\0';
	}
	// "Microsoft.ZuneMusic" -> "zunemusic"; a path -> its file name
	const char *base = buf;
	for (const char *c = buf; *c; ++c) {
		if (*c == '.' || *c == '\\' || *c == '/') {
			base = c + 1;
		}
	}
	if (!*base) {
		base = buf;
	}
	snprintf(out, size, "%s", base);
	for (char *c = out; *c; ++c) {
		if (*c >= 'A' && *c <= 'Z') {
			*c = (char)(*c - 'A' + 'a');
		}
	}
}

// Reads one session into out; false if it has no player to show.
static bool read_session(void *session, struct sysinfo_media *out) {
	memset(out, 0, sizeof(*out));
	out->length_ms = out->position_ms = -1;
	char aumid[256];
	get_string(session, SESSION_AUMID, aumid, sizeof(aumid));
	player_name(aumid, out->player, sizeof(out->player));
	void *playback = get_ptr(session, SESSION_PLAYBACK);
	int status = PLAYBACK_CLOSED;
	if (!playback || FAILED(((get_int_fn)SLOT(playback, PLAYBACK_STATUS))(
			playback, &status))) {
		status = PLAYBACK_CLOSED;
	}
	release(playback);
	if (status == PLAYBACK_CLOSED) {
		return false;
	}
	out->status = status == PLAYBACK_PLAYING ? MEDIA_PLAYING :
		status == PLAYBACK_PAUSED ? MEDIA_PAUSED : MEDIA_STOPPED;

	void *op = get_ptr(session, SESSION_PROPERTIES);
	void *properties = async_result(op, 500);
	release(op);
	if (properties) {
		get_string(properties, PROPERTIES_TITLE, out->title, sizeof(out->title));
		get_string(properties, PROPERTIES_ARTIST, out->artist,
			sizeof(out->artist));
		get_string(properties, PROPERTIES_ALBUM_ARTIST, out->album_artist,
			sizeof(out->album_artist));
		get_string(properties, PROPERTIES_ALBUM, out->album, sizeof(out->album));
		release(properties);
	}

	// TimeSpan and DateTime are 100 ns ticks; the position is as of
	// LastUpdatedTime, so a playing track has moved on since.
	void *timeline = get_ptr(session, SESSION_TIMELINE);
	INT64 start = 0, end = 0, position = 0, updated = 0;
	if (timeline &&
			SUCCEEDED(((get_i64_fn)SLOT(timeline, TIMELINE_START))(timeline, &start)) &&
			SUCCEEDED(((get_i64_fn)SLOT(timeline, TIMELINE_END))(timeline, &end)) &&
			SUCCEEDED(((get_i64_fn)SLOT(timeline, TIMELINE_POSITION))(timeline,
				&position)) &&
			SUCCEEDED(((get_i64_fn)SLOT(timeline, TIMELINE_UPDATED))(timeline,
				&updated)) && end > start) {
		out->length_ms = (end - start) / 10000;
		if (out->status == MEDIA_PLAYING && updated > 0) {
			FILETIME now;
			GetSystemTimeAsFileTime(&now);
			INT64 ticks = ((INT64)now.dwHighDateTime << 32) | now.dwLowDateTime;
			if (ticks > updated) {
				position += ticks - updated;
			}
		}
		if (position > end - start) {
			position = end - start;
		}
		out->position_ms = position > 0 ? position / 10000 : 0;
	}
	release(timeline);
	return true;
}

// The sessions, Windows' current one first.
static int read_sessions(void *manager, struct sysinfo_media *out, int max) {
	int n = 0;
	void *current = get_ptr(manager, MANAGER_CURRENT);
	char current_aumid[256] = "";
	if (current) {
		get_string(current, SESSION_AUMID, current_aumid, sizeof(current_aumid));
		if (read_session(current, &out[n])) {
			n++;
		}
	}
	void *list = get_ptr(manager, MANAGER_SESSIONS);
	UINT32 size = 0;
	if (list && SUCCEEDED(((get_u32_fn)SLOT(list, VECTOR_SIZE))(list, &size))) {
		for (UINT32 i = 0; i < size && n < max; ++i) {
			void *session = NULL;
			if (FAILED(((u32_ptr_fn)SLOT(list, VECTOR_GET_AT))(list, i, &session))) {
				continue;
			}
			char aumid[256];
			get_string(session, SESSION_AUMID, aumid, sizeof(aumid));
			if (!current || strcmp(aumid, current_aumid) != 0) {
				if (read_session(session, &out[n])) {
					n++;
				}
			}
			release(session);
		}
	}
	release(list);
	release(current);
	return n;
}

// Sends an action to the session of the named player.
static void control(void *manager, const char *player, int action) {
	void *list = get_ptr(manager, MANAGER_SESSIONS);
	UINT32 size = 0;
	if (!list || FAILED(((get_u32_fn)SLOT(list, VECTOR_SIZE))(list, &size))) {
		release(list);
		return;
	}
	for (UINT32 i = 0; i < size; ++i) {
		void *session = NULL;
		if (FAILED(((u32_ptr_fn)SLOT(list, VECTOR_GET_AT))(list, i, &session))) {
			continue;
		}
		char aumid[256], name[64];
		get_string(session, SESSION_AUMID, aumid, sizeof(aumid));
		player_name(aumid, name, sizeof(name));
		if (strcmp(name, player) == 0) {
			int slot = action == MEDIA_NEXT ? SESSION_NEXT :
				action == MEDIA_PREVIOUS ? SESSION_PREVIOUS : SESSION_TOGGLE;
			void *op = get_ptr(session, slot);
			release(async_result(op, 1000));   // a bool; only waited for
			release(op);
			release(session);
			break;
		}
		release(session);
	}
	release(list);
}

static DWORD WINAPI media_thread(LPVOID param) {
	(void)param;
	if (FAILED(ro_initialize(1))) {   // RO_INIT_MULTITHREADED
		failed = true;
		return 1;
	}
	static const wchar_t class_name[] =
		L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
	hstring name = NULL;
	void *statics = NULL, *manager = NULL;
	if (SUCCEEDED(create_string(class_name, (UINT32)wcslen(class_name), &name)) &&
			SUCCEEDED(ro_get_activation_factory(name, &iid_manager_statics,
				&statics))) {
		void *op = get_ptr(statics, STATICS_REQUEST);
		manager = async_result(op, 5000);
		release(op);
	}
	if (name) {
		delete_string(name);
	}
	release(statics);
	if (!manager) {
		swine_log(SWINE_INFO, "Media sessions are not available; the mpris "
			"module stays hidden");
		failed = true;
		ro_uninitialize();
		return 1;
	}
	while (!stopping) {
		char player[64];
		int action;
		EnterCriticalSection(&lock);
		action = request_action;
		memcpy(player, request_player, sizeof(player));
		request_action = -1;
		LeaveCriticalSection(&lock);
		if (action >= 0) {
			control(manager, player, action);
		}
		struct sysinfo_media fresh[MAX_PLAYERS];
		int n = read_sessions(manager, fresh, MAX_PLAYERS);
		EnterCriticalSection(&lock);
		memcpy(players, fresh, sizeof(fresh[0]) * (size_t)n);
		player_count = n;
		LeaveCriticalSection(&lock);
		WaitForSingleObject(wake, POLL_MS);
	}
	release(manager);
	ro_uninitialize();
	return 0;
}

static void start(void) {
	if (!lock_ready) {
		InitializeCriticalSection(&lock);
		lock_ready = true;
	}
	if (!thread && !failed) {
		if (!load_runtime()) {
			failed = true;
			return;
		}
		wake = CreateEventW(NULL, FALSE, FALSE, NULL);
		thread = CreateThread(NULL, 0, media_thread, NULL, 0, NULL);
	}
}

int sysinfo_media_players(struct sysinfo_media *out, int max) {
	start();
	EnterCriticalSection(&lock);
	int n = player_count < max ? player_count : max;
	memcpy(out, players, sizeof(players[0]) * (size_t)(n > 0 ? n : 0));
	LeaveCriticalSection(&lock);
	return n;
}

void sysinfo_media_control(const char *player,
		enum sysinfo_media_action action) {
	start();
	EnterCriticalSection(&lock);
	snprintf(request_player, sizeof(request_player), "%s", player);
	request_action = (int)action;
	LeaveCriticalSection(&lock);
	if (wake) {
		SetEvent(wake);
	}
}

void win32_media_finish(void) {
	if (thread) {
		InterlockedExchange(&stopping, 1);
		SetEvent(wake);
		if (WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0) {
			CloseHandle(wake);
		}
		CloseHandle(thread);
		thread = NULL;
	}
}
