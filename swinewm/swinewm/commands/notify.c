// notify-send and makoctl, the command-line front ends of the integrated
// notification daemon. They are swinewm commands (usable from bindings and
// over IPC), and "exec notify-send ..." / "exec makoctl ..." in a sway
// config are routed here too.
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/bar/bar.h"
#include "swinewm/bar/custom.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/grim.h"
#include "swinewm/notify.h"
#include "swinewm/platform.h"

// Returns the value of "-x value", "--long value" or "--long=value".
static bool take_option(int *i, int argc, char **argv, const char *shortopt,
		const char *longopt, const char **value) {
	const char *arg = argv[*i];
	size_t long_len = strlen(longopt);
	if (strcmp(arg, shortopt) == 0 || strcmp(arg, longopt) == 0) {
		if (*i + 1 >= argc) {
			return false;
		}
		*value = argv[++*i];
		return true;
	}
	if (strncmp(arg, longopt, long_len) == 0 && arg[long_len] == '=') {
		*value = arg + long_len + 1;
		return true;
	}
	return false;
}

static bool parse_urgency(const char *v, enum notify_urgency *out) {
	if (strcmp(v, "low") == 0 || strcmp(v, "0") == 0) {
		*out = NOTIFY_URGENCY_LOW;
	} else if (strcmp(v, "normal") == 0 || strcmp(v, "1") == 0) {
		*out = NOTIFY_URGENCY_NORMAL;
	} else if (strcmp(v, "critical") == 0 || strcmp(v, "2") == 0) {
		*out = NOTIFY_URGENCY_CRITICAL;
	} else {
		return false;
	}
	return true;
}

// What a notify-send command line asks for.
struct notify_send_args {
	struct notify_params p;
	list_t *keys, *titles;   // char *, for p.action_keys/action_titles
	bool wait;
	bool print_id;
};

static void free_send_args(struct notify_send_args *a) {
	list_free_items_and_destroy(a->keys);
	list_free_items_and_destroy(a->titles);
}

// -h TYPE:NAME:VALUE, the hints mako reads (dbus/xdg.c).
static struct cmd_results *parse_hint(const char *hint,
		struct notify_send_args *a) {
	const char *c1 = strchr(hint, ':');
	const char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
	if (!c2) {
		return cmd_results_new(CMD_INVALID,
			"Invalid hint syntax specified. Use TYPE:NAME:VALUE.");
	}
	char *type = swine_strndup(hint, (size_t)(c1 - hint));
	char *name = swine_strndup(c1 + 1, (size_t)(c2 - c1 - 1));
	const char *value = c2 + 1;
	struct cmd_results *error = NULL;
	if (strcmp(type, "int") != 0 && strcmp(type, "double") != 0 &&
			strcmp(type, "string") != 0 && strcmp(type, "byte") != 0 &&
			strcmp(type, "boolean") != 0 && strcmp(type, "variant") != 0) {
		error = cmd_results_new(CMD_INVALID, "Invalid hint type \"%s\". "
			"Valid types are int, double, string, byte, boolean and variant.",
			type);
	} else if (strcmp(name, "value") == 0) {
		a->p.has_progress = true;
		a->p.progress = atoi(value);
	} else if (strcmp(name, "urgency") == 0) {
		if (!parse_urgency(value, &a->p.urgency)) {
			error = cmd_results_new(CMD_INVALID, "Unknown urgency %s", value);
		}
	} else if (strcmp(name, "category") == 0) {
		a->p.category = value;
	} else if (strcmp(name, "desktop-entry") == 0) {
		a->p.desktop_entry = value;
	} else if (strcmp(name, "image-path") == 0 ||
			strcmp(name, "image_path") == 0) {
		// mako shows the image in place of the icon
		a->p.app_icon = value;
	} else if (strcmp(name, "x-canonical-private-synchronous") == 0 ||
			strcmp(name, "x-dunst-stack-tag") == 0) {
		a->p.tag = value;
	}
	free(type);
	free(name);
	return error;
}

// notify-send [OPTION...] <SUMMARY> [BODY], as libnotify's notify-send.
// The strings in a->p point into argv.
static struct cmd_results *parse_notify_send(int argc, char **argv,
		struct notify_send_args *a) {
	memset(a, 0, sizeof(*a));
	a->p.urgency = NOTIFY_URGENCY_NORMAL;
	a->p.expire_timeout = -1;
	a->keys = create_list();
	a->titles = create_list();
	const char *positional[2] = {NULL, NULL};
	int npos = 0;
	bool options_done = false;
	for (int i = 0; i < argc; ++i) {
		const char *v = NULL;
		if (!options_done && argv[i][0] == '-' && argv[i][1]) {
			struct cmd_results *error = NULL;
			if (strcmp(argv[i], "--") == 0) {
				options_done = true;
			} else if (take_option(&i, argc, argv, "-u", "--urgency", &v)) {
				if (!parse_urgency(v, &a->p.urgency)) {
					return cmd_results_new(CMD_INVALID, "Unknown urgency %s", v);
				}
			} else if (take_option(&i, argc, argv, "-t", "--expire-time", &v)) {
				a->p.expire_timeout = atoi(v);
			} else if (take_option(&i, argc, argv, "-a", "--app-name", &v)) {
				a->p.app_name = v;
			} else if (take_option(&i, argc, argv, "-c", "--category", &v)) {
				a->p.category = v;
			} else if (take_option(&i, argc, argv, "-r", "--replace-id", &v)) {
				a->p.replaces_id = (uint32_t)strtoul(v, NULL, 10);
			} else if (take_option(&i, argc, argv, "-i", "--icon", &v)) {
				a->p.app_icon = v;
			} else if (take_option(&i, argc, argv, "-h", "--hint", &v)) {
				if ((error = parse_hint(v, a))) {
					return error;
				}
			} else if (take_option(&i, argc, argv, "-A", "--action", &v)) {
				// [NAME=]Text; unnamed actions are numbered from 0
				const char *eq = strchr(v, '=');
				list_add(a->keys, eq ? swine_strndup(v, (size_t)(eq - v)) :
					format_str("%d", a->keys->length));
				list_add(a->titles, swine_strdup(eq ? eq + 1 : v));
			} else if (strcmp(argv[i], "-w") == 0 ||
					strcmp(argv[i], "--wait") == 0) {
				a->wait = true;
			} else if (strcmp(argv[i], "-p") == 0 ||
					strcmp(argv[i], "--print-id") == 0) {
				a->print_id = true;
			} else if (strcmp(argv[i], "-e") == 0 ||
					strcmp(argv[i], "--transient") == 0) {
				// mako has no transient notifications either
			} else {
				return cmd_results_new(CMD_INVALID,
					"Unknown notify-send option '%s'", argv[i]);
			}
			continue;
		}
		if (npos < 2) {
			positional[npos++] = argv[i];
		} else {
			return cmd_results_new(CMD_INVALID,
				"notify-send takes a summary and an optional body; quote them");
		}
	}
	if (!positional[0]) {
		return cmd_results_new(CMD_INVALID, "No summary specified");
	}
	a->p.summary = positional[0];
	a->p.body = positional[1];
	a->p.action_keys = (const char **)a->keys->items;
	a->p.action_titles = (const char **)a->titles->items;
	a->p.action_count = a->keys->length;
	// libnotify's notify-send waits when there are actions
	if (a->p.action_count > 0) {
		a->wait = true;
	}
	return NULL;
}

struct cmd_results *cmd_notify_send(int argc, char **argv) {
	struct notify_send_args a;
	struct cmd_results *error = parse_notify_send(argc, argv, &a);
	if (!error) {
		// Nobody waits here: invoking an action closes the notification.
		notify_add(&a.p, platform_now_ms());
	}
	free_send_args(&a);
	return error ? error : cmd_results_new(CMD_SUCCESS, NULL);
}

static struct cmd_results *makoctl_dismiss(int argc, char **argv) {
	bool all = false, group = false, history = true;
	uint32_t id = 0;
	for (int i = 0; i < argc; ++i) {
		const char *v = NULL;
		if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0) {
			all = true;
		} else if (strcmp(argv[i], "-g") == 0 ||
				strcmp(argv[i], "--group") == 0) {
			group = true;
		} else if (strcmp(argv[i], "-h") == 0 ||
				strcmp(argv[i], "--no-history") == 0) {
			history = false;
		} else if (take_option(&i, argc, argv, "-n", "--id", &v)) {
			id = (uint32_t)strtoul(v, NULL, 10);
		} else {
			return cmd_results_new(CMD_INVALID,
				"Unknown makoctl dismiss option '%s'", argv[i]);
		}
	}
	if (all && group) {
		return cmd_results_new(CMD_INVALID, "-a and -g cannot be used together");
	} else if ((all || group) && id != 0) {
		return cmd_results_new(CMD_INVALID, "-n cannot be used with -a or -g");
	}
	if (all) {
		notify_dismiss_all(history);
	} else if (group) {
		notify_dismiss_group(0, history);
	} else {
		notify_dismiss(id, history);   // mako: nothing to dismiss is fine
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

static struct cmd_results *makoctl_mode(int argc, char **argv, char **output) {
	if (argc > 0 && strcmp(argv[0], "-s") == 0) {
		notify_set_modes(argv + 1, argc - 1);
	} else {
		for (int i = 0; i < argc; ++i) {
			if (i + 1 >= argc || argv[i][0] != '-') {
				return cmd_results_new(CMD_INVALID, "Expected 'makoctl mode "
					"[-a|-r|-t <mode>]... | -s <mode>...'");
			}
			const char *mode = argv[i + 1];
			if (strcmp(argv[i], "-a") == 0) {
				notify_add_mode(mode);
			} else if (strcmp(argv[i], "-r") == 0) {
				notify_remove_mode(mode);
			} else if (strcmp(argv[i], "-t") == 0) {
				notify_toggle_mode(mode);
			} else {
				return cmd_results_new(CMD_INVALID, "Expected 'makoctl mode "
					"[-a|-r|-t <mode>]... | -s <mode>...'");
			}
			i++;
		}
	}
	// makoctl prints the modes afterwards
	list_t *modes = notify_modes();
	char *text = swine_strdup("");
	for (int i = 0; i < modes->length; ++i) {
		char *next = format_str("%s%s\n", text, (char *)modes->items[i]);
		free(text);
		text = next;
	}
	*output = text;
	return cmd_results_new(CMD_SUCCESS, NULL);
}

static struct cmd_results *makoctl_list(list_t *notifications, int argc,
		char **argv, char **output) {
	bool json = false;
	for (int i = 0; i < argc; ++i) {
		if (strcmp(argv[i], "-j") == 0) {
			json = true;
		} else {
			return cmd_results_new(CMD_INVALID,
				"Unknown makoctl option '%s'", argv[i]);
		}
	}
	if (json) {
		char *list = notify_list_json(notifications);
		*output = format_str("%s\n", list);
		free(list);
	} else {
		*output = notify_list_text(notifications);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

// makoctl <command> [options...]. Text for standard output goes to *output.
static struct cmd_results *makoctl(int argc, char **argv, char **output) {
	*output = NULL;
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "makoctl", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	const char *sub = argv[0];
	if (strcmp(sub, "dismiss") == 0) {
		return makoctl_dismiss(argc - 1, argv + 1);
	} else if (strcmp(sub, "restore") == 0) {
		notify_restore(platform_now_ms());
	} else if (strcmp(sub, "invoke") == 0) {
		uint32_t id = 0;
		const char *action = "default";
		for (int i = 1; i < argc; ++i) {
			const char *v = NULL;
			if (take_option(&i, argc, argv, "-n", "--id", &v)) {
				id = (uint32_t)strtoul(v, NULL, 10);
			} else {
				action = argv[i];
			}
		}
		if (!notify_invoke(id, action)) {
			return cmd_results_new(CMD_FAILURE, "No notification with action "
				"'%s'", action);
		}
	} else if (strcmp(sub, "mode") == 0) {
		return makoctl_mode(argc - 1, argv + 1, output);
	} else if (strcmp(sub, "reload") == 0) {
		notify_load_config();
	} else if (strcmp(sub, "list") == 0) {
		return makoctl_list(notify_active(), argc - 1, argv + 1, output);
	} else if (strcmp(sub, "history") == 0) {
		return makoctl_list(notify_history(), argc - 1, argv + 1, output);
	} else if (strcmp(sub, "menu") == 0) {
		// It runs a program and reads its answer: makoctl.exe does that.
		return cmd_results_new(CMD_FAILURE,
			"makoctl menu runs only as the makoctl program");
	} else {
		return cmd_results_new(CMD_INVALID,
			"Unknown makoctl command '%s'", sub);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_makoctl(int argc, char **argv) {
	char *output = NULL;
	struct cmd_results *res = makoctl(argc, argv, &output);
	free(output);   // nobody to print it to
	return res;
}

static char *tool_reply(struct cmd_results *res, long long id, bool wait,
		const char *output) {
	struct json_writer w;
	json_init(&w);
	json_begin_object(&w);
	json_key(&w, "success");
	json_bool(&w, res->status == CMD_SUCCESS);
	if (res->error) {
		json_key(&w, "error");
		json_string(&w, res->error);
	}
	if (id) {
		json_key(&w, "id");
		json_int(&w, id);
	}
	if (wait) {
		json_key(&w, "wait");
		json_bool(&w, true);
	}
	if (output) {
		json_key(&w, "output");
		json_string(&w, output);
	}
	json_end_object(&w);
	free_cmd_results(res);
	return json_finish(&w);
}

char *notify_send_tool(int argc, char **argv) {
	struct notify_send_args a;
	struct cmd_results *res = parse_notify_send(argc, argv, &a);
	uint32_t id = 0;
	char *output = NULL;
	if (!res) {
		// notify-send.exe stays to print the action and closes the
		// notification, as libnotify's does.
		a.p.waited_on = a.wait;
		id = notify_add(&a.p, platform_now_ms());
		platform_ui_refresh();
		if (a.print_id) {
			output = format_str("%u\n", id);
		}
		res = cmd_results_new(CMD_SUCCESS, NULL);
	}
	char *reply = tool_reply(res, id, id && a.wait, output);
	free(output);
	free_send_args(&a);
	return reply;
}

char *makoctl_tool(int argc, char **argv) {
	char *output = NULL;
	struct cmd_results *res = makoctl(argc, argv, &output);
	platform_ui_refresh();
	char *reply = tool_reply(res, 0, false, output);
	free(output);
	return reply;
}

// Lower-case base name of the program, without directory or ".exe".
static char *program_name(const char *arg) {
	const char *base = arg;
	for (const char *p = arg; *p; ++p) {
		if (*p == '/' || *p == '\\') {
			base = p + 1;
		}
	}
	char *name = swine_strdup(base);
	size_t len = strlen(name);
	if (len > 4 && swine_strcasecmp(name + len - 4, ".exe") == 0) {
		name[len - 4] = '\0';
	}
	for (char *c = name; *c; ++c) {
		if (*c >= 'A' && *c <= 'Z') {
			*c = (char)(*c - 'A' + 'a');
		}
	}
	return name;
}

// Waybar's signals, sent with "pkill -RTMIN+8 waybar" (also -SIGRTMIN+8,
// --signal RTMIN+8, -s RTMIN+8, killall): RTMIN+n runs the custom modules
// with "signal": n, USR1 shows or hides the bar, USR2 reloads it. Returns
// false if the line is not that (it is then run as a program).
static bool waybar_signal(int argc, char **argv, bool apply) {
	const char *sig = NULL;
	bool waybar = false;
	for (int i = 1; i < argc; ++i) {
		const char *a = argv[i];
		if ((strcmp(a, "-s") == 0 || strcmp(a, "--signal") == 0) && i + 1 < argc) {
			sig = argv[++i];
		} else if (has_prefix(a, "--signal=")) {
			sig = a + strlen("--signal=");
		} else if (a[0] == '-' && strcmp(a, "-x") != 0 && strcmp(a, "-e") != 0) {
			sig = a + 1;
		} else if (strcmp(a, "waybar") == 0) {
			waybar = true;
		}
	}
	if (!sig || !waybar) {
		return false;
	}
	if (has_prefix(sig, "SIG")) {
		sig += 3;
	}
	if (has_prefix(sig, "RTMIN+")) {
		char *end;
		long n = strtol(sig + strlen("RTMIN+"), &end, 10);
		if (*end || n <= 0 || n > 64) {
			return false;
		}
		if (apply) {
			bar_scripts_signal((int)n);
		}
		return true;
	}
	if (strcmp(sig, "USR1") == 0 || strcmp(sig, "USR2") == 0) {
		if (apply && strcmp(sig, "USR1") == 0) {
			platform_queue_command("bar mode toggle");
		} else if (apply) {
			bar_load();
			platform_ui_refresh();
		}
		return true;
	}
	return false;
}

// swaymsg or swinemsg with a command and no options (those print replies
// or query state, so they still run as a program).
static bool is_msg_command(const char *name, int argc, char **argv) {
	return (strcmp(name, "swaymsg") == 0 || strcmp(name, "swinemsg") == 0) &&
		argc > 1 && argv[1][0] != '-';
}

static struct cmd_results *run_builtin(int argc, char **argv,
		bool persistent_only) {
	if (argc == 0) {
		return NULL;
	}
	char *name = program_name(argv[0]);
	struct cmd_results *res = NULL;
	if (strcmp(name, "waybar") == 0 || strcmp(name, "swaybar") == 0) {
		swine_log(SWINE_INFO, "Using the integrated bar instead of %s", name);
		config->bar_enabled = true;
		res = cmd_results_new(CMD_SUCCESS, NULL);
	} else if (strcmp(name, "mako") == 0) {
		swine_log(SWINE_DEBUG, "Notifications are built in; not starting mako");
		res = cmd_results_new(CMD_SUCCESS, NULL);
	} else if (persistent_only) {
		res = NULL;
	} else if (strcmp(name, "makoctl") == 0 &&
			!(argc > 1 && strcmp(argv[1], "menu") == 0)) {
		// (makoctl menu runs as the makoctl program: it starts the menu)
		res = cmd_makoctl(argc - 1, argv + 1);
	} else if (strcmp(name, "notify-send") == 0) {
		res = cmd_notify_send(argc - 1, argv + 1);
	} else if ((strcmp(name, "pkill") == 0 || strcmp(name, "killall") == 0) &&
			waybar_signal(argc, argv, false)) {
		waybar_signal(argc, argv, true);
		res = cmd_results_new(CMD_SUCCESS, NULL);
	} else if ((res = audio_builtin(name, argc, argv)) != NULL) {
		// pactl, wpctl or pamixer changed the Windows volume
	} else if (is_msg_command(name, argc, argv)) {
		// "swaymsg <command>" (a Waybar on-click, a binding): run the
		// command here instead of starting a process to send it back.
		char *command = join_args(argv + 1, argc - 1);
		platform_queue_command(command);
		free(command);
		res = cmd_results_new(CMD_SUCCESS, NULL);
	}
	free(name);
	return res;
}

// sh's "$(slurp -d)" unquoted is split at its spaces: join it again.
static void join_substitutions(char **argv, int *argc) {
	for (int i = 0; i < *argc; ++i) {
		if (!has_prefix(argv[i], "$(")) {
			continue;
		}
		while (argv[i][strlen(argv[i]) - 1] != ')' && i + 1 < *argc) {
			char *joined = format_str("%s %s", argv[i], argv[i + 1]);
			free(argv[i]);
			free(argv[i + 1]);
			argv[i] = joined;
			memmove(argv + i + 1, argv + i + 2,
				sizeof(char *) * (size_t)(*argc - i - 2));
			(*argc)--;
		}
	}
}

// The arguments as one line again, quoted where they have spaces.
static char *quote_join(char **argv, int argc) {
	char *line = swine_strdup("");
	for (int i = 0; i < argc; ++i) {
		const char *a = argv[i];
		bool quote = !*a || strpbrk(a, " \t") != NULL;
		char *next = format_str("%s%s%s%s%s", line, i ? " " : "",
			quote ? "\"" : "", a, quote ? "\"" : "");
		free(line);
		line = next;
	}
	return line;
}

struct cmd_results *exec_builtin(const char *cmdline, bool persistent_only) {
	int argc;
	char **argv = split_args(cmdline, &argc);
	join_substitutions(argv, &argc);
	if (argc == 0) {
		free_argv(argc, argv);
		return NULL;
	}
	// sway runs exec through sh, so "grim && notify-send Saved" is common:
	// run such chains here when every part is built in.
	int segments = 1;
	for (int i = 0; i < argc; ++i) {
		if (strcmp(argv[i], "&&") == 0) {
			segments++;
		}
	}
	if (segments > 1 && persistent_only) {
		free_argv(argc, argv);
		return NULL;
	}
	if (segments > 1) {
		int start = 0;
		for (int i = 0; i <= argc && start < argc; ++i) {
			if (i < argc && strcmp(argv[i], "&&") != 0) {
				continue;
			}
			char *name = program_name(argv[start]);
			bool builtin = (strcmp(name, "makoctl") == 0 &&
					!(i - start > 1 && strcmp(argv[start + 1], "menu") == 0)) ||
				strcmp(name, "notify-send") == 0 ||
				grim_exec_handles(i - start, argv + start) ||
				is_msg_command(name, i - start, argv + start) ||
				((strcmp(name, "pkill") == 0 || strcmp(name, "killall") == 0) &&
				waybar_signal(i - start, argv + start, false));
			free(name);
			if (!builtin) {
				free_argv(argc, argv);
				return NULL;   // not ours: run the whole line as a program
			}
			start = i + 1;
		}
	}
	for (int i = 0; i < argc; ++i) {
		strip_quotes(argv[i]);
	}
	struct cmd_results *res = NULL;
	int start = 0;
	for (int i = 0; i <= argc; ++i) {
		if (i < argc && (segments == 1 || strcmp(argv[i], "&&") != 0)) {
			continue;
		}
		if (res) {
			free_cmd_results(res);
		}
		bool deferred = false;
		res = NULL;
		if (!persistent_only) {
			// grim, slurp, wl-copy, grimshot: a selection may come first,
			// and the rest of the chain runs after it.
			char *rest = i < argc ? quote_join(argv + i + 1, argc - i - 1) : NULL;
			res = grim_exec(i - start, argv + start, rest, &deferred);
			free(rest);
		}
		if (!res) {
			res = run_builtin(i - start, argv + start, persistent_only);
		}
		if (!res || res->status != CMD_SUCCESS || deferred) {
			break;
		}
		start = i + 1;
	}
	free_argv(argc, argv);
	return res;
}
