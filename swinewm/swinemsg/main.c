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
	"  -m, --monitor          Monitor until killed (-t SUBSCRIBE only)\n"
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

int main(void) {
	// main's argv is in the ANSI code page: take the UTF-8 one.
	int argc;
	char **argv = ipc_utf8_argv(&argc);
	bool quiet = false;
	bool monitor = false;
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
		} else if (strcmp(arg, "-m") == 0 || strcmp(arg, "--monitor") == 0) {
			monitor = true;
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
	if (type == IPC_SUBSCRIBE) {
		char *events = ipc_subscribe_payload(payload);
		free(payload);
		payload = events;
	}
	if (monitor && type != IPC_SUBSCRIBE) {
		fprintf(stderr, "Monitor can only be used with -t SUBSCRIBE\n");
		free(payload);
		return 1;
	}
	char *error = NULL;
	if (type == IPC_SUBSCRIBE) {
		// As swaymsg: print the first event, or every event with -m.
		void *connection = ipc_connect(pipe_name, &error);
		char *reply = NULL;
		if (!connection || !ipc_send(connection, type, payload) ||
				!(reply = ipc_receive(connection, NULL))) {
			fprintf(stderr, "%s\n", error ? error : "IPC request failed");
			free(error);
			ipc_close(connection);
			free(payload);
			return 1;
		}
		free(payload);
		if (strstr(reply, "\"success\":false")) {
			if (!quiet) {
				printf("%s\n", reply);
			}
			free(reply);
			ipc_close(connection);
			return 2;
		}
		free(reply);
		int status = 0;
		do {
			char *event = ipc_receive(connection, NULL);
			if (!event) {
				status = 1;
				break;
			}
			if (!quiet) {
				printf("%s\n", event);
				fflush(stdout);
			}
			free(event);
		} while (monitor);
		ipc_close(connection);
		free(pipe_name);
		return status;
	}
	char *reply = ipc_request(pipe_name, type, payload, &error);
	if (!reply) {
		fprintf(stderr, "%s\n", error);
		free(error);
		return 1;
	}

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
