// swinemsg: send messages to swinewm over its IPC pipe, like swaymsg.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ipc.h"
#include "ipc-client.h"
#include "stringop.h"
#include "swinewm_version.h"

static const char usage[] =
	"Usage: swinemsg [options] [message]\n"
	"\n"
	"  -h, --help             Show help message and quit.\n"
	"  -q, --quiet            Be quiet.\n"
	"  -r, --raw              Use raw output (the only output format so far).\n"
	"  -s, --socket <pipe>    Use the specified named pipe.\n"
	"  -t, --type <type>      Specify the message type.\n"
	"  -v, --version          Show the version number and quit.\n";

struct type_name {
	const char *name;
	uint32_t type;
};

static const struct type_name types[] = {
	{ "command", IPC_COMMAND },
	{ "get_workspaces", IPC_GET_WORKSPACES },
	{ "get_inputs", IPC_GET_INPUTS },
	{ "get_outputs", IPC_GET_OUTPUTS },
	{ "get_tree", IPC_GET_TREE },
	{ "get_seats", IPC_GET_SEATS },
	{ "get_marks", IPC_GET_MARKS },
	{ "get_bar_config", IPC_GET_BAR_CONFIG },
	{ "get_binding_modes", IPC_GET_BINDING_MODES },
	{ "get_binding_state", IPC_GET_BINDING_STATE },
	{ "get_version", IPC_GET_VERSION },
	{ "get_config", IPC_GET_CONFIG },
	{ "send_tick", IPC_SEND_TICK },
	{ "subscribe", IPC_SUBSCRIBE },
	{ "get_notifications", SWINE_IPC_GET_NOTIFICATIONS },
	{ "get_notification_history", SWINE_IPC_GET_NOTIFICATION_HISTORY },
};

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

static char *quote_args(char **args, int count) {
	size_t len = 1;
	for (int k = 0; k < count; ++k) {
		len += strlen(args[k]) * 2 + 3;
	}
	char *out = malloc(len);
	char *o = out;
	for (int k = 0; k < count; ++k) {
		const char *a = args[k];
		// The first argument is left alone so that a whole command line
		// passed as one argument ("focus left; kill") still works as in
		// swaymsg.
		bool quote = k > 0 && strpbrk(a, " \t;,") != NULL &&
			a[0] != '"' && a[0] != '\'';
		if (k > 0) {
			*o++ = ' ';
		}
		if (quote) {
			*o++ = '"';
		}
		for (; *a; ++a) {
			if (quote && *a == '"') {
				*o++ = '\\';
			}
			*o++ = *a;
		}
		if (quote) {
			*o++ = '"';
		}
	}
	*o = '\0';
	return out;
}

int main(int argc, char **argv) {
	bool quiet = false;
	char *pipe_name = NULL;
	const char *type_name = "command";
	SetConsoleOutputCP(CP_UTF8);   // replies are UTF-8 JSON
	int i = 1;
	for (; i < argc; ++i) {
		const char *arg = argv[i];
		if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
			printf("%s", usage);
			return 0;
		} else if (strcmp(arg, "-q") == 0 || strcmp(arg, "--quiet") == 0) {
			quiet = true;
		} else if (strcmp(arg, "-r") == 0 || strcmp(arg, "--raw") == 0) {
			// Raw JSON is the only format for now.
		} else if ((strcmp(arg, "-s") == 0 || strcmp(arg, "--socket") == 0) &&
				i + 1 < argc) {
			pipe_name = swine_strdup(argv[++i]);
		} else if ((strcmp(arg, "-t") == 0 || strcmp(arg, "--type") == 0) &&
				i + 1 < argc) {
			type_name = argv[++i];
		} else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--version") == 0) {
			printf("swinemsg version " SWINEWM_VERSION "\n");
			return 0;
		} else if (strcmp(arg, "--") == 0) {
			++i;
			break;
		} else {
			break;
		}
	}

	uint32_t type = UINT32_MAX;
	for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); ++t) {
		if (swine_strcasecmp(types[t].name, type_name) == 0) {
			type = types[t].type;
		}
	}
	if (type == UINT32_MAX) {
		fprintf(stderr, "Unknown message type %s\n", type_name);
		return 1;
	}

	// The shell has already removed quotes; put them back around later
	// arguments with spaces so that `swinemsg notify-send "Hello there"`
	// keeps its meaning when swinewm splits the command again.
	char *payload = quote_args(argv + i, argc - i);
	if (!pipe_name) {
		pipe_name = get_ipc_pipe_name();
	}
	HANDLE pipe = connect_pipe(pipe_name);
	if (pipe == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "Unable to connect to %s (is swinewm running?)\n",
			pipe_name);
		return 1;
	}

	char header[IPC_HEADER_SIZE];
	uint32_t len = (uint32_t)strlen(payload);
	memcpy(header, IPC_MAGIC, IPC_MAGIC_LEN);
	memcpy(header + IPC_MAGIC_LEN, &len, 4);
	memcpy(header + IPC_MAGIC_LEN + 4, &type, 4);
	if (!io_exact(pipe, header, IPC_HEADER_SIZE, true) ||
			!io_exact(pipe, payload, len, true)) {
		fprintf(stderr, "Unable to send IPC message\n");
		return 1;
	}
	uint32_t reply_len = 0;
	if (!io_exact(pipe, header, IPC_HEADER_SIZE, false)) {
		fprintf(stderr, "Unable to receive IPC response\n");
		return 1;
	}
	memcpy(&reply_len, header + IPC_MAGIC_LEN, 4);
	char *reply = malloc((size_t)reply_len + 1);
	if (!reply || (reply_len && !io_exact(pipe, reply, reply_len, false))) {
		fprintf(stderr, "Unable to receive IPC response\n");
		return 1;
	}
	reply[reply_len] = '\0';
	CloseHandle(pipe);

	int status = 0;
	if ((type == IPC_COMMAND || type == IPC_SUBSCRIBE) &&
			strstr(reply, "\"success\":false")) {
		status = 2;
	}
	if (!quiet || status) {
		printf("%s\n", reply);
	}
	free(reply);
	free(payload);
	free(pipe_name);
	return status;
}
