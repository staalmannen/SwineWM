// i3/sway IPC over a named pipe (\\.\pipe\swinewm-ipc.<user>).
//
// A listener thread accepts connections. Client threads send requests to the
// main thread with SendMessage; per-client writer threads drain event queues.
//
// The pipe is opened for overlapped I/O. On a synchronous handle Windows
// serializes all I/O: while a client thread waits in ReadFile for the next
// request, an event writer's WriteFile would wait too, and a client that
// subscribes and then only listens (i3ipc scripts, Waybar's sway modules)
// would never get its events.
#include <stdlib.h>
#include <string.h>
#include "ipc.h"
#include "ipc-client.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/ipc-server.h"
#include "win32.h"

#define IPC_MAX_PAYLOAD (16 * 1024 * 1024)

static wchar_t *pipe_name = NULL;

struct queued_ipc_event {
	uint32_t type;
	char *payload;
	struct queued_ipc_event *next;
};

struct ipc_client {
	HANDLE pipe;
	HANDLE read_event;    // for the client thread's reads
	HANDLE write_event;   // for writes, which hold write_lock
	HANDLE event_ready;
	HANDLE writer_thread;
	CRITICAL_SECTION write_lock;
	CRITICAL_SECTION queue_lock;
	struct queued_ipc_event *queue_head;
	struct queued_ipc_event *queue_tail;
	unsigned queue_length;
	uint32_t subscriptions;
	bool stopping;
	struct ipc_client *next;
};

static CRITICAL_SECTION clients_lock;
static struct ipc_client *clients;
static bool clients_lock_initialized;

// One overlapped read or write, waited for.
static bool pipe_io(HANDLE pipe, HANDLE event, bool write, void *buf,
		DWORD len, DWORD *done) {
	OVERLAPPED ov = {0};
	ov.hEvent = event;
	ResetEvent(event);
	BOOL ok = write ? WriteFile(pipe, buf, len, NULL, &ov) :
		ReadFile(pipe, buf, len, NULL, &ov);
	if (!ok && GetLastError() != ERROR_IO_PENDING) {
		return false;
	}
	return GetOverlappedResult(pipe, &ov, done, TRUE) != FALSE;
}

static bool read_exact(struct ipc_client *client, void *buf, DWORD len) {
	char *p = buf;
	while (len > 0) {
		DWORD got = 0;
		if (!pipe_io(client->pipe, client->read_event, false, p, len, &got) ||
				got == 0) {
			return false;
		}
		p += got;
		len -= got;
	}
	return true;
}

// With client->write_lock held.
static bool write_exact(struct ipc_client *client, const void *buf, DWORD len) {
	const char *p = buf;
	while (len > 0) {
		DWORD put = 0;
		if (!pipe_io(client->pipe, client->write_event, true, (void *)p, len,
				&put) || put == 0) {
			return false;
		}
		p += put;
		len -= put;
	}
	return true;
}

static bool write_frame_locked(struct ipc_client *client, uint32_t type,
		const char *payload) {
	uint32_t length = (uint32_t)strlen(payload);
	char header[IPC_HEADER_SIZE];
	memcpy(header, IPC_MAGIC, IPC_MAGIC_LEN);
	memcpy(header + IPC_MAGIC_LEN, &length, 4);
	memcpy(header + IPC_MAGIC_LEN + 4, &type, 4);
	EnterCriticalSection(&client->write_lock);
	bool ok = write_exact(client, header, IPC_HEADER_SIZE) &&
		write_exact(client, payload, length);
	LeaveCriticalSection(&client->write_lock);
	return ok;
}

static void wake_event_writer(struct ipc_client *client) {
	if (!SetEvent(client->event_ready)) {
		swine_log(SWINE_ERROR, "IPC: unable to wake an event writer (%lu)",
			GetLastError());
	}
}

static DWORD WINAPI event_writer_thread(LPVOID data) {
	struct ipc_client *client = data;
	for (;;) {
		DWORD wait = WaitForSingleObject(client->event_ready, 1000);
		if (wait == WAIT_FAILED) {
			swine_log(SWINE_ERROR, "IPC: event writer wait failed (%lu)",
				GetLastError());
			break;
		}
		EnterCriticalSection(&client->queue_lock);
		if (client->stopping) {
			LeaveCriticalSection(&client->queue_lock);
			break;
		}
		struct queued_ipc_event *event = client->queue_head;
		if (event) {
			client->queue_head = event->next;
			if (!client->queue_head) {
				client->queue_tail = NULL;
			}
			client->queue_length--;
			if (client->queue_head) {
				wake_event_writer(client);
			}
		}
		LeaveCriticalSection(&client->queue_lock);
		if (!event) {
			continue;
		}
		bool ok = write_frame_locked(client, event->type, event->payload);
		free(event->payload);
		free(event);
		if (!ok) {
			break;
		}
	}
	return 0;
}

static uint32_t subscription_for_event(uint32_t type) {
	switch (type) {
	case (uint32_t)IPC_EVENT_WORKSPACE: return IPC_SUB_WORKSPACE;
	case (uint32_t)IPC_EVENT_OUTPUT: return IPC_SUB_OUTPUT;
	case (uint32_t)IPC_EVENT_MODE: return IPC_SUB_MODE;
	case (uint32_t)IPC_EVENT_WINDOW: return IPC_SUB_WINDOW;
	case (uint32_t)IPC_EVENT_BARCONFIG_UPDATE: return IPC_SUB_BARCONFIG_UPDATE;
	case (uint32_t)IPC_EVENT_BINDING: return IPC_SUB_BINDING;
	case (uint32_t)IPC_EVENT_SHUTDOWN: return IPC_SUB_SHUTDOWN;
	case (uint32_t)IPC_EVENT_TICK: return IPC_SUB_TICK;
	case (uint32_t)IPC_EVENT_INPUT: return IPC_SUB_INPUT;
	case (uint32_t)SWINE_IPC_EVENT_NOTIFICATION: return IPC_SUB_NOTIFICATION;
	default: return 0;
	}
}

// With clients_lock held.
static void queue_event(struct ipc_client *client, uint32_t type,
		const char *payload) {
	struct queued_ipc_event *event = calloc(1, sizeof(*event));
	if (!event) {
		swine_log(SWINE_ERROR, "IPC: unable to queue event for a client");
		return;
	}
	event->type = type;
	event->payload = swine_strdup(payload);
	if (!event->payload) {
		free(event);
		swine_log(SWINE_ERROR, "IPC: unable to copy event for a client");
		return;
	}
	EnterCriticalSection(&client->queue_lock);
	bool slow = client->queue_length >= 256;
	if (client->stopping || slow) {
		LeaveCriticalSection(&client->queue_lock);
		free(event->payload);
		free(event);
		if (slow) {
			client->subscriptions = 0;
			swine_log(SWINE_ERROR,
				"IPC: stopped event delivery to a slow client");
		}
		return;
	}
	if (client->queue_tail) {
		client->queue_tail->next = event;
	} else {
		client->queue_head = event;
	}
	client->queue_tail = event;
	client->queue_length++;
	wake_event_writer(client);
	LeaveCriticalSection(&client->queue_lock);
}

void win32_ipc_publish(uint32_t type, const char *payload) {
	uint32_t subscription = subscription_for_event(type);
	if (!subscription || !payload || !clients_lock_initialized) {
		return;
	}
	EnterCriticalSection(&clients_lock);
	for (struct ipc_client *client = clients; client; client = client->next) {
		if (client->subscriptions & subscription) {
			queue_event(client, type, payload);
		}
	}
	LeaveCriticalSection(&clients_lock);
}

static void remove_client(struct ipc_client *client) {
	EnterCriticalSection(&clients_lock);
	struct ipc_client **link = &clients;
	while (*link && *link != client) {
		link = &(*link)->next;
	}
	if (*link) {
		*link = client->next;
	}
	EnterCriticalSection(&client->queue_lock);
	client->stopping = true;
	wake_event_writer(client);
	LeaveCriticalSection(&client->queue_lock);
	LeaveCriticalSection(&clients_lock);
	DisconnectNamedPipe(client->pipe);
	CancelIoEx(client->pipe, NULL);
	WaitForSingleObject(client->writer_thread, INFINITE);
	CloseHandle(client->writer_thread);
	struct queued_ipc_event *event = client->queue_head;
	while (event) {
		struct queued_ipc_event *next = event->next;
		free(event->payload);
		free(event);
		event = next;
	}
	CloseHandle(client->event_ready);
	CloseHandle(client->read_event);
	CloseHandle(client->write_event);
	DeleteCriticalSection(&client->queue_lock);
	DeleteCriticalSection(&client->write_lock);
	free(client);
}

static DWORD WINAPI client_thread(LPVOID data) {
	struct ipc_client *client = data;
	for (;;) {
		char header[IPC_HEADER_SIZE];
		if (!read_exact(client, header, IPC_HEADER_SIZE)) {
			break;
		}
		if (memcmp(header, IPC_MAGIC, IPC_MAGIC_LEN) != 0) {
			swine_log(SWINE_INFO, "IPC client sent a bad header");
			break;
		}
		uint32_t length, type;
		memcpy(&length, header + IPC_MAGIC_LEN, 4);
		memcpy(&type, header + IPC_MAGIC_LEN + 4, 4);
		if (length > IPC_MAX_PAYLOAD) {
			break;
		}
		char *payload = malloc((size_t)length + 1);
		if (!payload || (length && !read_exact(client, payload, length))) {
			free(payload);
			break;
		}
		payload[length] = '\0';
		bool subscribed = type == IPC_SUBSCRIBE &&
			ipc_parse_subscriptions(payload, length, NULL);

		struct win32_ipc_request request = {
			.type = type, .payload = payload, .length = length,
		};
		// Not SendMessageTimeout: after a timeout the message can still be
		// delivered later, and request lives on this thread's stack.
		SendMessageW(win32_shell_hwnd, WM_SWINE_IPC, 0, (LPARAM)&request);
		if (!request.reply) {
			free(payload);
			break;
		}
		uint32_t reply_len = (uint32_t)strlen(request.reply);
		memcpy(header, IPC_MAGIC, IPC_MAGIC_LEN);
		memcpy(header + IPC_MAGIC_LEN, &reply_len, 4);
		memcpy(header + IPC_MAGIC_LEN + 4, &request.reply_type, 4);
		EnterCriticalSection(&client->write_lock);
		bool ok = write_exact(client, header, IPC_HEADER_SIZE) &&
			write_exact(client, request.reply, reply_len);
		LeaveCriticalSection(&client->write_lock);
		bool success = strcmp(request.reply, "{\"success\":true}") == 0;
		uint32_t subscriptions = 0;
		if (subscribed && success) {
			ipc_parse_subscriptions(payload, length, &subscriptions);
		}
		free(request.reply);
		free(payload);
		if (subscribed && success) {
			// As in sway, subscriptions add up, and a new "tick"
			// subscriber gets a first tick right after the reply.
			EnterCriticalSection(&clients_lock);
			client->subscriptions |= subscriptions;
			if (subscriptions & IPC_SUB_TICK) {
				char *first = ipc_json_tick_event(true, "", 0);
				queue_event(client, (uint32_t)IPC_EVENT_TICK, first);
				free(first);
			}
			LeaveCriticalSection(&clients_lock);
		}
		if (!ok) {
			break;
		}
	}
	HANDLE pipe = client->pipe;
	remove_client(client);
	CloseHandle(pipe);
	return 0;
}

static HANDLE create_instance(bool first) {
	DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
		(first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
	return CreateNamedPipeW(pipe_name, open_mode,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
		PIPE_REJECT_REMOTE_CLIENTS,
		PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, NULL);
}

static bool start_client(HANDLE pipe) {
	struct ipc_client *client = calloc(1, sizeof(*client));
	if (!client) {
		return false;
	}
	client->pipe = pipe;
	client->read_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	client->write_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!client->read_event || !client->write_event) {
		if (client->read_event) {
			CloseHandle(client->read_event);
		}
		if (client->write_event) {
			CloseHandle(client->write_event);
		}
		free(client);
		return false;
	}
	InitializeCriticalSection(&client->write_lock);
	InitializeCriticalSection(&client->queue_lock);
	client->event_ready = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!client->event_ready) {
		CloseHandle(client->read_event);
		CloseHandle(client->write_event);
		DeleteCriticalSection(&client->queue_lock);
		DeleteCriticalSection(&client->write_lock);
		free(client);
		return false;
	}
	client->writer_thread = CreateThread(NULL, 0, event_writer_thread,
		client, 0, NULL);
	if (!client->writer_thread) {
		CloseHandle(client->event_ready);
		CloseHandle(client->read_event);
		CloseHandle(client->write_event);
		DeleteCriticalSection(&client->queue_lock);
		DeleteCriticalSection(&client->write_lock);
		free(client);
		return false;
	}
	EnterCriticalSection(&clients_lock);
	client->next = clients;
	clients = client;
	LeaveCriticalSection(&clients_lock);
	HANDLE thread = CreateThread(NULL, 0, client_thread, client, 0, NULL);
	if (!thread) {
		remove_client(client);
		return false;
	}
	CloseHandle(thread);
	return true;
}

// ConnectNamedPipe on an overlapped handle needs an OVERLAPPED.
static bool wait_for_client(HANDLE pipe, HANDLE event) {
	OVERLAPPED ov = {0};
	ov.hEvent = event;
	ResetEvent(event);
	if (ConnectNamedPipe(pipe, &ov)) {
		return true;
	}
	DWORD error = GetLastError();
	if (error == ERROR_PIPE_CONNECTED) {
		return true;
	}
	DWORD done = 0;
	return error == ERROR_IO_PENDING &&
		GetOverlappedResult(pipe, &ov, &done, TRUE);
}

static DWORD WINAPI listen_thread(LPVOID data) {
	HANDLE pipe = data;
	HANDLE connect_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!connect_event) {
		swine_log(SWINE_ERROR, "IPC: unable to create an event");
		CloseHandle(pipe);
		return 1;
	}
	for (;;) {
		bool connected = wait_for_client(pipe, connect_event);
		if (connected) {
			if (!start_client(pipe)) {
				CloseHandle(pipe);
			}
		} else {
			CloseHandle(pipe);
		}
		pipe = create_instance(false);
		if (pipe == INVALID_HANDLE_VALUE) {
			swine_log(SWINE_ERROR, "IPC: unable to create pipe instance");
			CloseHandle(connect_event);
			return 1;
		}
	}
}

bool win32_ipc_init(void) {
	InitializeCriticalSection(&clients_lock);
	clients_lock_initialized = true;
	char *name = get_ipc_pipe_name();
	pipe_name = utf8_to_utf16(name);
	HANDLE first = create_instance(true);
	if (first == INVALID_HANDLE_VALUE) {
		DWORD error = GetLastError();
		if (error == ERROR_ACCESS_DENIED) {
			swine_log(SWINE_ERROR, "IPC pipe %s already exists; is another "
				"swinewm running?", name);
		} else {
			char *msg = win32_error_string(error);
			swine_log(SWINE_ERROR, "Unable to create IPC pipe %s: %s", name, msg);
			free(msg);
		}
		free(name);
		return false;
	}
	// Programs started with exec find us the way sway clients use SWAYSOCK.
	SetEnvironmentVariableW(L"SWINESOCK", pipe_name);
	HANDLE thread = CreateThread(NULL, 0, listen_thread, first, 0, NULL);
	if (!thread) {
		CloseHandle(first);
		free(name);
		return false;
	}
	CloseHandle(thread);
	swine_log(SWINE_INFO, "IPC listening on %s", name);
	free(name);
	return true;
}

void win32_ipc_handle(struct win32_ipc_request *request) {
	request->reply = ipc_handle_request(request->type, request->payload,
		request->length, &request->reply_type);
}

void win32_ipc_flush(DWORD timeout_ms) {
	if (!clients_lock_initialized) {
		return;
	}
	ULONGLONG deadline = GetTickCount64() + timeout_ms;
	for (;;) {
		bool pending = false;
		EnterCriticalSection(&clients_lock);
		for (struct ipc_client *client = clients; client; client = client->next) {
			EnterCriticalSection(&client->queue_lock);
			pending = pending || (client->queue_length > 0 && !client->stopping);
			LeaveCriticalSection(&client->queue_lock);
		}
		LeaveCriticalSection(&clients_lock);
		if (!pending || GetTickCount64() >= deadline) {
			return;
		}
		Sleep(10);
	}
}
