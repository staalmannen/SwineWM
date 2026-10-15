// notify-send and makoctl, the command-line front ends of the integrated
// notification daemon. They are swinewm commands (usable from bindings and
// over IPC), and "exec notify-send ..." / "exec makoctl ..." in a sway
// config are routed here too.
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
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

// notify-send [OPTION...] <SUMMARY> [BODY]
struct cmd_results *cmd_notify_send(int argc, char **argv) {
	struct notify_params p = {
		.urgency = NOTIFY_URGENCY_NORMAL,
		.expire_timeout = -1,
	};
	const char *positional[2] = {NULL, NULL};
	int npos = 0;
	bool options_done = false;
	for (int i = 0; i < argc; ++i) {
		const char *v = NULL;
		if (!options_done && argv[i][0] == '-' && argv[i][1]) {
			if (strcmp(argv[i], "--") == 0) {
				options_done = true;
			} else if (take_option(&i, argc, argv, "-u", "--urgency", &v)) {
				if (!parse_urgency(v, &p.urgency)) {
					return cmd_results_new(CMD_INVALID, "Unknown urgency %s", v);
				}
			} else if (take_option(&i, argc, argv, "-t", "--expire-time", &v)) {
				p.expire_timeout = atoi(v);
			} else if (take_option(&i, argc, argv, "-a", "--app-name", &v)) {
				p.app_name = v;
			} else if (take_option(&i, argc, argv, "-c", "--category", &v)) {
				p.category = v;
			} else if (take_option(&i, argc, argv, "-r", "--replace-id", &v)) {
				p.replaces_id = (uint32_t)strtoul(v, NULL, 10);
			} else if (take_option(&i, argc, argv, "-i", "--icon", &v) ||
					take_option(&i, argc, argv, "-h", "--hint", &v) ||
					take_option(&i, argc, argv, "-A", "--action", &v)) {
				// icons, hints and actions are not supported yet
			} else if (strcmp(argv[i], "-e") == 0 ||
					strcmp(argv[i], "--transient") == 0 ||
					strcmp(argv[i], "-p") == 0 ||
					strcmp(argv[i], "--print-id") == 0 ||
					strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--wait") == 0) {
				// accepted, no effect
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
	p.summary = positional[0];
	p.body = positional[1];
	notify_add(&p, platform_now_ms());
	return cmd_results_new(CMD_SUCCESS, NULL);
}

static struct cmd_results *makoctl_dismiss(int argc, char **argv) {
	bool all = false, history = true;
	uint32_t id = 0;
	for (int i = 0; i < argc; ++i) {
		const char *v = NULL;
		if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0) {
			all = true;
		} else if (strcmp(argv[i], "-g") == 0 ||
				strcmp(argv[i], "--group") == 0) {
			// no grouping yet: a group is a single notification
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
	if (all) {
		notify_dismiss_all(history);
	} else if (!notify_dismiss(id, history)) {
		return cmd_results_new(CMD_FAILURE, "No notification to dismiss");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

static struct cmd_results *makoctl_mode(int argc, char **argv) {
	if (argc == 0) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	if (strcmp(argv[0], "-s") == 0) {
		notify_set_modes(argv + 1, argc - 1);
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	for (int i = 0; i + 1 < argc; i += 2) {
		if (strcmp(argv[i], "-a") == 0) {
			notify_add_mode(argv[i + 1]);
		} else if (strcmp(argv[i], "-r") == 0) {
			notify_remove_mode(argv[i + 1]);
		} else if (strcmp(argv[i], "-t") == 0) {
			notify_toggle_mode(argv[i + 1]);
		} else {
			return cmd_results_new(CMD_INVALID,
				"Expected 'makoctl mode [-a|-r|-t <mode>]... | -s <mode>...'");
		}
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_makoctl(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "makoctl", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	const char *sub = argv[0];
	if (strcmp(sub, "dismiss") == 0) {
		return makoctl_dismiss(argc - 1, argv + 1);
	} else if (strcmp(sub, "restore") == 0) {
		if (!notify_restore(platform_now_ms())) {
			return cmd_results_new(CMD_FAILURE, "History is empty");
		}
	} else if (strcmp(sub, "invoke") == 0) {
		// Without action support, invoking the default action dismisses.
		uint32_t id = 0;
		for (int i = 1; i < argc; ++i) {
			const char *v = NULL;
			if (take_option(&i, argc, argv, "-n", "--id", &v)) {
				id = (uint32_t)strtoul(v, NULL, 10);
			}
		}
		if (!notify_dismiss(id, true)) {
			return cmd_results_new(CMD_FAILURE, "No notification to invoke");
		}
	} else if (strcmp(sub, "mode") == 0) {
		return makoctl_mode(argc - 1, argv + 1);
	} else if (strcmp(sub, "reload") == 0) {
		notify_load_config();
	} else if (strcmp(sub, "list") == 0 || strcmp(sub, "history") == 0) {
		return cmd_results_new(CMD_FAILURE, "Use 'swinemsg -t "
			"get_notifications' or '-t get_notification_history'");
	} else {
		return cmd_results_new(CMD_INVALID,
			"Unknown makoctl command '%s'", sub);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
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

struct cmd_results *exec_builtin(const char *cmdline, bool persistent_only) {
	int argc;
	char **argv = split_args(cmdline, &argc);
	if (argc == 0) {
		free_argv(argc, argv);
		return NULL;
	}
	for (int i = 0; i < argc; ++i) {
		strip_quotes(argv[i]);
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
	} else if (strcmp(name, "makoctl") == 0) {
		res = cmd_makoctl(argc - 1, argv + 1);
	} else if (strcmp(name, "notify-send") == 0) {
		res = cmd_notify_send(argc - 1, argv + 1);
	}
	free(name);
	free_argv(argc, argv);
	return res;
}
