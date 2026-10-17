#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ipc.h"
#include "ipc-client.h"
#include "stringop.h"

static char *narrow(const wchar_t *w) {
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	char *s = malloc((size_t)(n > 0 ? n : 1));
	if (n > 0) {
		WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
	} else {
		s[0] = '\0';
	}
	return s;
}

char *get_ipc_pipe_name(void) {
	wchar_t buf[512];
	DWORD n = GetEnvironmentVariableW(L"SWINESOCK", buf, 512);
	if (n > 0 && n < 512) {
		return narrow(buf);
	}
	DWORD size = 256;
	wchar_t user[256] = L"user";
	GetUserNameW(user, &size);
	char *user8 = narrow(user);
	char *name = format_str(SWINE_IPC_PIPE_FMT, user8);
	free(user8);
	return name;
}

static bool io_exact(HANDLE pipe, void *buf, DWORD len, bool write) {
	char *p = buf;
	while (len > 0) {
		DWORD n = 0;
		BOOL ok = write ? WriteFile(pipe, p, len, &n, NULL) :
			ReadFile(pipe, p, len, &n, NULL);
		if (!ok || n == 0) {
			return false;
		}
		p += n;
		len -= n;
	}
	return true;
}

static HANDLE connect_pipe(const char *name) {
	int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, NULL, 0);
	wchar_t *wide = malloc(sizeof(wchar_t) * (size_t)n);
	MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, n);
	HANDLE pipe = INVALID_HANDLE_VALUE;
	for (int attempt = 0; attempt < 5; ++attempt) {
		pipe = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, 0, NULL,
			OPEN_EXISTING, 0, NULL);
		if (pipe != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) {
			break;
		}
		WaitNamedPipeW(wide, 2000);
	}
	free(wide);
	return pipe;
}

char *ipc_request(const char *pipe_name, uint32_t type, const char *payload,
		char **error) {
	char *own_name = pipe_name ? NULL : get_ipc_pipe_name();
	const char *name = pipe_name ? pipe_name : own_name;
	HANDLE pipe = connect_pipe(name);
	if (pipe == INVALID_HANDLE_VALUE) {
		if (error) {
			*error = format_str("Unable to connect to %s (is swinewm running?)",
				name);
		}
		free(own_name);
		return NULL;
	}
	free(own_name);
	char header[IPC_HEADER_SIZE];
	uint32_t len = (uint32_t)strlen(payload);
	memcpy(header, IPC_MAGIC, IPC_MAGIC_LEN);
	memcpy(header + IPC_MAGIC_LEN, &len, 4);
	memcpy(header + IPC_MAGIC_LEN + 4, &type, 4);
	char *reply = NULL;
	uint32_t reply_len = 0;
	if (!io_exact(pipe, header, IPC_HEADER_SIZE, true) ||
			!io_exact(pipe, (void *)payload, len, true)) {
		if (error) {
			*error = swine_strdup("Unable to send IPC message");
		}
	} else if (!io_exact(pipe, header, IPC_HEADER_SIZE, false)) {
		if (error) {
			*error = swine_strdup("Unable to receive IPC response");
		}
	} else {
		memcpy(&reply_len, header + IPC_MAGIC_LEN, 4);
		reply = malloc((size_t)reply_len + 1);
		if (!reply || (reply_len && !io_exact(pipe, reply, reply_len, false))) {
			free(reply);
			reply = NULL;
			if (error) {
				*error = swine_strdup("Unable to receive IPC response");
			}
		} else {
			reply[reply_len] = '\0';
		}
	}
	CloseHandle(pipe);
	return reply;
}
#endif
