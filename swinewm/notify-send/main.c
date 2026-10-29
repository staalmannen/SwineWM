// notify-send for swinewm's built-in notifications, with libnotify's
// options. It hands its arguments to swinewm over IPC; with --wait or
// actions it stays until the notification closes and prints the key of the
// action the user picked, as libnotify's notify-send does.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ipc.h"
#include "ipc-client.h"
#include "json.h"
#include "list.h"
#include "swinewm_version.h"

static const char usage[] =
	"Usage:\n"
	"  notify-send [OPTION...] <SUMMARY> [BODY] - create a notification\n"
	"\n"
	"Help Options:\n"
	"  -?, --help                        Show help options\n"
	"\n"
	"Application Options:\n"
	"  -u, --urgency=LEVEL               Specifies the urgency level (low, normal, critical).\n"
	"  -t, --expire-time=TIME            Specifies the timeout in milliseconds at which to expire the notification.\n"
	"  -a, --app-name=APP_NAME           Specifies the app name for the icon\n"
	"  -i, --icon=ICON                   Specifies an icon filename or stock icon to display.\n"
	"  -c, --category=TYPE[,TYPE...]     Specifies the notification category.\n"
	"  -e, --transient                   Create a transient notification\n"
	"  -h, --hint=TYPE:NAME:VALUE        Specifies basic extra data to pass. Valid types are boolean, int, double, string, byte and variant.\n"
	"  -p, --print-id                    Print the notification ID.\n"
	"  -r, --replace-id=REPLACE_ID       The ID of the notification to replace.\n"
	"  -w, --wait                        Wait for the notification to be closed before exiting.\n"
	"  -A, --action=[NAME=]Text...       Specifies the actions to display to the user. Implies --wait to wait for user input. May be set multiple times. The name of the action is output to stdout. If NAME is not specified, the numerical index of the option is used (starting with 0).\n"
	"  -v, --version                     Version of the package.\n";

// Handles a "notification" event; returns true when the wait is over.
static bool handle_event(void *connection, const char *payload, uint32_t id) {
	struct json_value *event = json_parse(payload, NULL);
	if (!event) {
		return false;
	}
	bool done = false;
	if ((uint32_t)json_get_number(event, "id", 0) == id) {
		const char *change = json_get_string(event, "change", "");
		if (strcmp(change, "action") == 0) {
			printf("%s\n", json_get_string(event, "action", ""));
			fflush(stdout);
			// libnotify's notify-send closes the notification then
			char text[16];
			snprintf(text, sizeof(text), "%u", id);
			ipc_send(connection, SWINE_IPC_CLOSE_NOTIFICATION, text);
		} else if (strcmp(change, "close") == 0) {
			done = true;
		}
	}
	json_value_free(event);
	return done;
}

int main(void) {
	SetConsoleOutputCP(CP_UTF8);
	int argc;
	char **argv = ipc_utf8_argv(&argc);
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--") == 0) {
			break;
		} else if (strcmp(argv[i], "-?") == 0 ||
				strcmp(argv[i], "--help") == 0) {
			printf("%s", usage);
			return 0;
		} else if (strcmp(argv[i], "-v") == 0 ||
				strcmp(argv[i], "--version") == 0) {
			printf("notify-send " SWINEWM_VERSION " (swinewm)\n");
			return 0;
		}
	}

	struct json_writer w;
	json_init(&w);
	json_begin_array(&w);
	for (int i = 1; i < argc; ++i) {
		json_string(&w, argv[i]);
	}
	json_end_array(&w);
	char *payload = json_finish(&w);

	// Subscribe first so that no action or close is missed.
	char *error = NULL;
	void *connection = ipc_connect(NULL, &error);
	char *reply = NULL;
	if (!connection || !ipc_send(connection, IPC_SUBSCRIBE,
			"[\"notification\"]") || !(reply = ipc_receive(connection, NULL)) ||
			!ipc_send(connection, SWINE_IPC_NOTIFY_SEND, payload)) {
		fprintf(stderr, "%s\n", error ? error : "Cannot reach swinewm");
		return 1;
	}
	free(reply);
	free(payload);

	// Events can come before the reply: keep them until the id is known.
	list_t *early = create_list();
	uint32_t type = 0;
	while ((reply = ipc_receive(connection, &type)) != NULL &&
			(type & 0x80000000u)) {
		list_add(early, reply);
	}
	if (!reply) {
		fprintf(stderr, "Lost the connection to swinewm\n");
		return 1;
	}
	struct json_value *result = json_parse(reply, NULL);
	free(reply);
	if (!result || !json_get_bool(result, "success", false)) {
		fprintf(stderr, "%s\n", result ? json_get_string(result, "error",
			"Failed to send the notification") : "Bad reply from swinewm");
		return 1;
	}
	uint32_t id = (uint32_t)json_get_number(result, "id", 0);
	const char *output = json_get_string(result, "output", NULL);
	if (output) {
		fputs(output, stdout);
		fflush(stdout);
	}
	bool wait = json_get_bool(result, "wait", false);
	json_value_free(result);

	bool done = !wait;
	for (int i = 0; i < early->length; ++i) {
		done = done || handle_event(connection, early->items[i], id);
	}
	list_free_items_and_destroy(early);
	while (!done) {
		char *event = ipc_receive(connection, &type);
		if (!event) {
			fprintf(stderr, "Lost the connection to swinewm\n");
			return 1;
		}
		if (type == (uint32_t)SWINE_IPC_EVENT_NOTIFICATION) {
			done = handle_event(connection, event, id);
		}
		free(event);
	}
	ipc_close(connection);
	return 0;
}
