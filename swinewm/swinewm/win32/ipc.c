// i3/sway IPC over a named pipe (\\.\pipe\swinewm-ipc.<user>).
//
// A listener thread accepts connections; each client gets a thread that
// reads framed requests and hands them to the main thread with SendMessage,
// so all tree access stays single-threaded. Event subscriptions are not
// implemented yet.
#include <stdlib.h>
#include <string.h>
#include "ipc.h"
#include "ipc-client.h"
#include "log.h"
#include "swinewm/ipc-server.h"
#include "win32.h"

#define IPC_MAX_PAYLOAD (16 * 1024 * 1024)

static wchar_t *pipe_name = NULL;

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

static DWORD WINAPI client_thread(LPVOID data) {
	HANDLE pipe = data;
	for (;;) {
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

		struct win32_ipc_request request = {
			.type = type, .payload = payload, .length = length,
		};
		// Not SendMessageTimeout: after a timeout the message can still be
		// delivered later, and request lives on this thread's stack.
		SendMessageW(win32_shell_hwnd, WM_SWINE_IPC, 0, (LPARAM)&request);
		free(payload);
		if (!request.reply) {
			break;
		}
		uint32_t reply_len = (uint32_t)strlen(request.reply);
		memcpy(header, IPC_MAGIC, IPC_MAGIC_LEN);
		memcpy(header + IPC_MAGIC_LEN, &reply_len, 4);
		memcpy(header + IPC_MAGIC_LEN + 4, &request.reply_type, 4);
		bool ok = write_exact(pipe, header, IPC_HEADER_SIZE) &&
			write_exact(pipe, request.reply, reply_len);
		free(request.reply);
		if (!ok) {
			break;
		}
	}
	FlushFileBuffers(pipe);
	DisconnectNamedPipe(pipe);
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

static DWORD WINAPI listen_thread(LPVOID data) {
	HANDLE pipe = data;
	for (;;) {
		BOOL connected = ConnectNamedPipe(pipe, NULL) ||
			GetLastError() == ERROR_PIPE_CONNECTED;
		if (connected) {
			HANDLE thread = CreateThread(NULL, 0, client_thread, pipe, 0, NULL);
			if (thread) {
				CloseHandle(thread);
			} else {
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
