#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ipc.h"
#include "ipc-client.h"
#include "json.h"
#include "stringop.h"

char *ipc_subscribe_payload(const char *text) {
	char *error = NULL;
	struct json_value *value = json_parse(text, &error);
	free(error);
	bool is_array = value && value->type == JSON_ARRAY;
	json_value_free(value);
	if (is_array) {
		return swine_strdup(text);
	}
	// Event names, however they arrived: "window workspace",
	// "window,workspace", or "[window,workspace]" (what Windows PowerShell
	// before 7.3 passes for '["window","workspace"]').
	struct json_writer w;
	json_init(&w);
	json_begin_array(&w);
	list_t *names = split_string(text, " ,[]\"'\\\t");
	for (int i = 0; i < names->length; ++i) {
		json_string(&w, names->items[i]);
	}
	list_free_items_and_destroy(names);
	json_end_array(&w);
	return json_finish(&w);
}

#ifdef _WIN32

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

char **ipc_utf8_argv(int *argc) {
	wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), argc);
	if (!wide) {
		*argc = 0;
		return calloc(1, sizeof(char *));
	}
	char **argv = calloc((size_t)*argc + 1, sizeof(char *));
	for (int i = 0; i < *argc; ++i) {
		argv[i] = narrow(wide[i]);
	}
	LocalFree(wide);
	return argv;
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

void *ipc_connect(const char *pipe_name, char **error) {
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
	return pipe;
}

bool ipc_send(void *connection, uint32_t type, const char *payload) {
	char header[IPC_HEADER_SIZE];
	uint32_t len = (uint32_t)strlen(payload);
	memcpy(header, IPC_MAGIC, IPC_MAGIC_LEN);
	memcpy(header + IPC_MAGIC_LEN, &len, 4);
	memcpy(header + IPC_MAGIC_LEN + 4, &type, 4);
	return io_exact(connection, header, IPC_HEADER_SIZE, true) &&
		io_exact(connection, (void *)payload, len, true);
}

char *ipc_receive(void *connection, uint32_t *type) {
	char header[IPC_HEADER_SIZE];
	if (!io_exact(connection, header, IPC_HEADER_SIZE, false) ||
			memcmp(header, IPC_MAGIC, IPC_MAGIC_LEN) != 0) {
		return NULL;
	}
	uint32_t len = 0;
	memcpy(&len, header + IPC_MAGIC_LEN, 4);
	if (type) {
		memcpy(type, header + IPC_MAGIC_LEN + 4, 4);
	}
	char *payload = malloc((size_t)len + 1);
	if (!payload || (len && !io_exact(connection, payload, len, false))) {
		free(payload);
		return NULL;
	}
	payload[len] = '\0';
	return payload;
}

void ipc_close(void *connection) {
	if (connection) {
		CloseHandle(connection);
	}
}

char *ipc_request(const char *pipe_name, uint32_t type, const char *payload,
		char **error) {
	void *connection = ipc_connect(pipe_name, error);
	if (!connection) {
		return NULL;
	}
	char *reply = NULL;
	if (!ipc_send(connection, type, payload)) {
		if (error) {
			*error = swine_strdup("Unable to send IPC message");
		}
	} else if (!(reply = ipc_receive(connection, NULL))) {
		if (error) {
			*error = swine_strdup("Unable to receive IPC response");
		}
	}
	ipc_close(connection);
	return reply;
}
#endif
