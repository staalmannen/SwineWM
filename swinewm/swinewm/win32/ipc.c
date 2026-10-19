// i3/sway IPC over a named pipe (\\.\pipe\swinewm-ipc.<user>).
//
// A listener thread accepts connections. Client threads send requests to the
// main thread with SendMessage; per-client writer threads drain event queues.
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

static bool read_exact(HANDLE pipe, void *buf, DWORD len) {
	char *p = buf;
	while (len > 0) {
		DWORD got = 0;
		if (!ReadFile(pipe, p, len, &got, NULL) || got == 0) {
			return false;
		}
		p += got;
		len -= got;
	}
	return true;
}

static bool write_exact(HANDLE pipe, const void *buf, DWORD len) {
	const char *p = buf;
	while (len > 0) {
		DWORD put = 0;
		if (!WriteFile(pipe, p, len, &put, NULL) || put == 0) {
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
	bool ok = write_exact(client->pipe, header, IPC_HEADER_SIZE) &&
		write_exact(client->pipe, payload, length);
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
	default: return 0;
	}
}

void win32_ipc_publish(uint32_t type, const char *payload) {
	uint32_t subscription = subscription_for_event(type);
	if (!subscription || !payload || !clients_lock_initialized) {
		return;
	}
	EnterCriticalSection(&clients_lock);
	for (struct ipc_client *client = clients; client; client = client->next) {
		if (!(client->subscriptions & subscription)) {
			continue;
		}
		struct queued_ipc_event *event = calloc(1, sizeof(*event));
		if (!event) {
			swine_log(SWINE_ERROR, "IPC: unable to queue event for a client");
			continue;
		}
		event->type = type;
		event->payload = swine_strdup(payload);
		if (!event->payload) {
			free(event);
			swine_log(SWINE_ERROR, "IPC: unable to copy event for a client");
			continue;
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
			continue;
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
	CancelSynchronousIo(client->writer_thread);
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
	DeleteCriticalSection(&client->queue_lock);
	DeleteCriticalSection(&client->write_lock);
	free(client);
}

static DWORD WINAPI client_thread(LPVOID data) {
	struct ipc_client *client = data;
	for (;;) {
		HANDLE pipe = client->pipe;
		char header[IPC_HEADER_SIZE];
		if (!read_exact(pipe, header, IPC_HEADER_SIZE)) {
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
		if (!payload || (length && !read_exact(pipe, payload, length))) {
			free(payload);
			break;
		}
		payload[length] = '\0';
		bool subscribed = type == IPC_SUBSCRIBE &&
			ipc_parse_subscriptions(payload, length, NULL);
		bool send_tick = type == IPC_SEND_TICK;

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
		bool ok = write_exact(pipe, header, IPC_HEADER_SIZE) &&
			write_exact(pipe, request.reply, reply_len);
		LeaveCriticalSection(&client->write_lock);
		bool success = strcmp(request.reply, "{\"success\":true}") == 0;
		uint32_t subscriptions = 0;
		if (subscribed && success) {
			ipc_parse_subscriptions(payload, length, &subscriptions);
		}
		char *tick_event = send_tick && success ?
			ipc_json_tick_event(payload, length) : NULL;
		free(request.reply);
		free(payload);
		if (subscribed && success) {
			EnterCriticalSection(&clients_lock);
			client->subscriptions = subscriptions;
			LeaveCriticalSection(&clients_lock);
		}
		if (tick_event) {
			win32_ipc_publish((uint32_t)IPC_EVENT_TICK, tick_event);
			free(tick_event);
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
	DWORD open_mode = PIPE_ACCESS_DUPLEX |
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
	InitializeCriticalSection(&client->write_lock);
	InitializeCriticalSection(&client->queue_lock);
	client->event_ready = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!client->event_ready) {
		DeleteCriticalSection(&client->queue_lock);
		DeleteCriticalSection(&client->write_lock);
		free(client);
		return false;
	}
	client->writer_thread = CreateThread(NULL, 0, event_writer_thread,
		client, 0, NULL);
	if (!client->writer_thread) {
		CloseHandle(client->event_ready);
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

static DWORD WINAPI listen_thread(LPVOID data) {
	HANDLE pipe = data;
	for (;;) {
		BOOL connected = ConnectNamedPipe(pipe, NULL) ||
			GetLastError() == ERROR_PIPE_CONNECTED;
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
