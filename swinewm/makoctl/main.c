// makoctl for swinewm's built-in notifications. The commands run inside
// swinewm (over IPC); "menu" runs here, since it starts a dmenu-like
// program and reads the chosen action from it. Usage text and menu logic
// from mako's makoctl.c (Copyright (c) 2018 emersion, MIT).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ipc.h"
#include "ipc-client.h"
#include "json.h"
#include "stringop.h"
#include "swinewm_version.h"

static const char usage[] =
	"Usage: makoctl <command> [options...]\n"
	"\n"
	"Commands:\n"
	"  dismiss [-n id]                Dismiss the notification with the\n"
	"                                 given id, or the last notification\n"
	"                                 if none is given\n"
	"          [-a|--all]             Dismiss all notifications\n"
	"          [-g|--group]           Dismiss all the notifications\n"
	"                                 in the last notification's group\n"
	"          [-h|--no-history]      Dismiss w/o adding to history\n"
	"  restore                        Restore the most recently expired\n"
	"                                 notification from the history buffer\n"
	"  invoke [-n id] [action]        Invoke an action on the notification\n"
	"                                 with the given id, or the last\n"
	"                                 notification if none is given\n"
	"  menu [-n id] <prog> [arg ...]  Use <prog> [args ...] to select one\n"
	"                                 action to be invoked on the notification\n"
	"                                 with the given id, or the last\n"
	"                                 notification if none is given\n"
	"  list [-j]                      List notifications\n"
	"  history [-j]                   List history\n"
	"  reload                         Reload the configuration file\n"
	"  mode                           List modes\n"
	"  mode [-a mode]... [-r mode]... Add/remove modes\n"
	"  mode [-t mode]...              Toggle modes (add if not present, remove if present)\n"
	"  mode -s mode...                Set modes\n"
	"  help                           Show this help\n";

// Runs "makoctl <argv...>" in swinewm. Returns its parsed reply, or NULL
// after printing why there is none.
static struct json_value *run_in_swinewm(int argc, char **argv) {
	struct json_writer w;
	json_init(&w);
	json_begin_array(&w);
	for (int i = 0; i < argc; ++i) {
		json_string(&w, argv[i]);
	}
	json_end_array(&w);
	char *payload = json_finish(&w);
	char *error = NULL;
	char *reply = ipc_request(NULL, SWINE_IPC_MAKOCTL, payload, &error);
	free(payload);
	if (!reply) {
		fprintf(stderr, "%s\n", error ? error : "Cannot reach swinewm");
		free(error);
		return NULL;
	}
	struct json_value *result = json_parse(reply, NULL);
	free(reply);
	if (!result) {
		fprintf(stderr, "Bad reply from swinewm\n");
	}
	return result;
}

// Appends one argument to a CreateProcess command line, quoted as
// CommandLineToArgvW reads it back.
static void append_arg(char **line, const char *arg) {
	size_t len = strlen(*line);
	size_t cap = len + strlen(arg) * 3 + 4;
	*line = realloc(*line, cap);
	char *o = *line + len;
	if (len > 0) {
		*o++ = ' ';
	}
	bool quote = !*arg || strpbrk(arg, " \t\"") != NULL;
	if (quote) {
		*o++ = '"';
	}
	for (const char *a = arg;; ++a) {
		size_t backslashes = strspn(a, "\\");
		a += backslashes;
		if (!*a) {
			// before the closing quote they must be doubled
			backslashes *= quote ? 2 : 1;
		} else if (*a == '"') {
			backslashes = backslashes * 2 + 1;
		}
		for (size_t b = 0; b < backslashes; ++b) {
			*o++ = '\\';
		}
		if (!*a) {
			break;
		}
		*o++ = *a;
	}
	if (quote) {
		*o++ = '"';
	}
	*o = '\0';
}

// Starts the menu with pipes for its standard input and output.
static bool start_menu(const char *cmdline, HANDLE *in, HANDLE *out,
		HANDLE *process) {
	SECURITY_ATTRIBUTES sa = { (DWORD)sizeof(sa), NULL, TRUE };
	HANDLE child_in, child_out;
	if (!CreatePipe(&child_in, in, &sa, 0)) {
		return false;
	}
	if (!CreatePipe(out, &child_out, &sa, 0)) {
		CloseHandle(child_in);
		CloseHandle(*in);
		return false;
	}
	SetHandleInformation(*in, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(*out, HANDLE_FLAG_INHERIT, 0);
	STARTUPINFOW si = { .cb = sizeof(si) };
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = child_in;
	si.hStdOutput = child_out;
	si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	PROCESS_INFORMATION pi = {0};
	int n = MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, NULL, 0);
	wchar_t *wide = calloc((size_t)n + 1, sizeof(wchar_t));
	MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, wide, n);
	BOOL ok = CreateProcessW(NULL, wide, NULL, NULL, TRUE, 0, NULL, NULL,
		&si, &pi);
	free(wide);
	CloseHandle(child_in);
	CloseHandle(child_out);
	if (!ok) {
		CloseHandle(*in);
		CloseHandle(*out);
		return false;
	}
	CloseHandle(pi.hThread);
	*process = pi.hProcess;
	return true;
}

// The Linux menus that mako configs name, which swinemenu stands in for.
static bool is_dmenu_like(const char *program) {
	static const char *names[] = {
		"wofi", "rofi", "dmenu", "fuzzel", "tofi", "bemenu", "wmenu",
		"walker", "anyrun",
	};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
		if (swine_strcasecmp(program, names[i]) == 0) {
			return true;
		}
	}
	return false;
}

static int run_menu(int argc, char **argv) {
	const char *id = "0";
	int i = 1;
	if (i + 1 < argc && strcmp(argv[i], "-n") == 0) {
		id = argv[i + 1];
		i += 2;
	}
	if (i >= argc) {
		fprintf(stderr, "Missing menu command\n");
		return 1;
	}
	char *list_args[] = {"list", "-j"};
	struct json_value *result = run_in_swinewm(2, list_args);
	if (!result) {
		return 1;
	}
	struct json_value *list = json_parse(json_get_string(result, "output",
		"[]"), NULL);
	json_value_free(result);
	struct json_value *found = NULL;
	unsigned long want = strtoul(id, NULL, 10);
	for (int k = 0; list && list->type == JSON_ARRAY &&
			k < list->items->length && !found; ++k) {
		struct json_value *n = list->items->items[k];
		if (want == 0 || (unsigned long)json_get_number(n, "id", 0) == want) {
			found = n;
		}
	}
	if (!found) {
		fprintf(stderr, "Notification not found\n");
		json_value_free(list);
		return 1;
	}
	char found_id[16];
	snprintf(found_id, sizeof(found_id), "%lu",
		(unsigned long)json_get_number(found, "id", 0));
	struct json_value *actions = json_object_get(found, "actions");
	if (!actions || actions->type != JSON_OBJECT || !actions->items->length) {
		fprintf(stderr, "Notification has no actions\n");
		json_value_free(list);
		return 1;
	}

	char *cmdline = calloc(1, 1);
	for (int k = i; k < argc; ++k) {
		append_arg(&cmdline, argv[k]);
	}
	HANDLE in, out, process;
	bool started = start_menu(cmdline, &in, &out, &process);
	if (!started && is_dmenu_like(argv[i])) {
		// No such program on Windows: ask swinemenu, keeping the prompt.
		free(cmdline);
		cmdline = calloc(1, 1);
		append_arg(&cmdline, "swinemenu");
		append_arg(&cmdline, "--dmenu");
		for (int k = i + 1; k + 1 < argc; ++k) {
			if (strcmp(argv[k], "-p") == 0 ||
					strcmp(argv[k], "--prompt") == 0) {
				append_arg(&cmdline, "--prompt");
				append_arg(&cmdline, argv[k + 1]);
			}
		}
		started = start_menu(cmdline, &in, &out, &process);
	}
	if (!started) {
		fprintf(stderr, "Unable to start '%s' (error %lu)\n", cmdline,
			GetLastError());
		free(cmdline);
		json_value_free(list);
		return 1;
	}
	free(cmdline);
	for (int k = 0; k < actions->items->length; ++k) {
		struct json_value *title = actions->items->items[k];
		if (title->type == JSON_STRING) {
			DWORD written;
			WriteFile(in, title->string, (DWORD)strlen(title->string),
				&written, NULL);
			WriteFile(in, "\n", 1, &written, NULL);
		}
	}
	CloseHandle(in);
	char *selected = NULL;
	size_t len = 0;
	char chunk[512];
	DWORD got;
	while (ReadFile(out, chunk, sizeof(chunk), &got, NULL) && got > 0) {
		selected = realloc(selected, len + got + 1);
		memcpy(selected + len, chunk, got);
		len += got;
		selected[len] = '\0';
	}
	CloseHandle(out);
	WaitForSingleObject(process, INFINITE);
	DWORD status = 1;
	GetExitCodeProcess(process, &status);
	CloseHandle(process);
	if (!selected) {
		fprintf(stderr, "No action selected\n");
		json_value_free(list);
		return 1;
	}
	if (status != 0) {
		fprintf(stderr, "Menu failed with exit code %lu\n", status);
		free(selected);
		json_value_free(list);
		return 1;
	}
	selected[strcspn(selected, "\r\n")] = '\0';
	const char *key = NULL;
	for (int k = 0; k < actions->items->length && !key; ++k) {
		struct json_value *title = actions->items->items[k];
		if (title->type == JSON_STRING && strcmp(title->string, selected) == 0) {
			key = actions->keys->items[k];
		}
	}
	free(selected);
	if (!key) {
		fprintf(stderr, "Action not found\n");
		json_value_free(list);
		return 1;
	}
	char *invoke_args[] = {"invoke", "-n", found_id, (char *)key};
	result = run_in_swinewm(4, invoke_args);
	json_value_free(list);
	bool ok = result && json_get_bool(result, "success", false);
	json_value_free(result);
	return ok ? 0 : 1;
}

int main(void) {
	SetConsoleOutputCP(CP_UTF8);
	int argc;
	char **argv = ipc_utf8_argv(&argc);
	if (argc <= 1) {
		fprintf(stderr, "%s", usage);
		return 1;
	}
	const char *cmd = argv[1];
	if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
			strcmp(cmd, "-h") == 0) {
		fprintf(stderr, "%s", usage);
		return 0;
	}
	if (strcmp(cmd, "-v") == 0 || strcmp(cmd, "--version") == 0) {
		printf("makoctl " SWINEWM_VERSION " (swinewm)\n");
		return 0;
	}
	if (strcmp(cmd, "menu") == 0) {
		return run_menu(argc - 1, argv + 1);
	}
	struct json_value *result = run_in_swinewm(argc - 1, argv + 1);
	if (!result) {
		return 1;
	}
	const char *output = json_get_string(result, "output", NULL);
	if (output) {
		fputs(output, stdout);
	}
	bool ok = json_get_bool(result, "success", false);
	if (!ok) {
		fprintf(stderr, "%s\n", json_get_string(result, "error", "Failed"));
	}
	json_value_free(result);
	return ok ? 0 : 1;
}
