// The volume tools of Linux Waybar and sway configs, run inside swinewm when
// they appear in exec (an on-click, a binding), for the default Windows
// output and input:
//
//   pactl set-sink-volume @DEFAULT_SINK@ +5%      (also -5%, 50%)
//   pactl set-sink-mute @DEFAULT_SINK@ toggle     (also 1, 0, yes, no)
//   pactl set-source-volume|set-source-mute @DEFAULT_SOURCE@ ...
//   wpctl set-volume [-l 1.0] @DEFAULT_AUDIO_SINK@ 5%+   (also 5%-, 0.5, 50%)
//   wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle
//   pamixer -i 5 | -d 5 | --set-volume 50 | -t | -m | -u [--default-source]
//
// Anything else is left to run as a program.
#include <stdlib.h>
#include <string.h>
#include "stringop.h"
#include "swinewm/bar/bar.h"
#include "swinewm/commands.h"
#include "swinewm/platform.h"
#include "swinewm/sysinfo.h"

static bool is_sink(const char *target) {
	return strcmp(target, "@DEFAULT_SINK@") == 0 ||
		strcmp(target, "@DEFAULT_AUDIO_SINK@") == 0;
}

static bool is_source(const char *target) {
	return strcmp(target, "@DEFAULT_SOURCE@") == 0 ||
		strcmp(target, "@DEFAULT_AUDIO_SOURCE@") == 0;
}

// "toggle", "1", "0", "yes", "no", "true", "false": -1, 1, 0; -2 if not.
static int mute_mode(const char *arg) {
	if (strcmp(arg, "toggle") == 0) {
		return -1;
	}
	if (strcmp(arg, "1") == 0 || strcmp(arg, "yes") == 0 ||
			strcmp(arg, "true") == 0 || strcmp(arg, "on") == 0) {
		return 1;
	}
	if (strcmp(arg, "0") == 0 || strcmp(arg, "no") == 0 ||
			strcmp(arg, "false") == 0 || strcmp(arg, "off") == 0) {
		return 0;
	}
	return -2;
}

static int current_volume(bool source) {
	struct sysinfo_audio a = {0};
	if (!sysinfo_audio(&a)) {
		return 0;
	}
	return source ? a.source_volume : a.volume;
}

static void set_volume(bool source, double percent, double max) {
	if (percent > max) {
		percent = max;
	}
	if (percent < 0) {
		percent = 0;
	}
	sysinfo_audio_set_volume(source, percent);
}

// A volume as pactl or wpctl writes it: "50%", "0.5" (a factor), and
// relative "+5%"/"-5%" (pactl) or "5%+"/"0.05-" (wpctl). Returns false if
// it is none of these.
static bool parse_volume(const char *arg, bool *relative, double *percent) {
	size_t len = strlen(arg);
	if (len == 0) {
		return false;
	}
	double sign = 0;
	char *copy = swine_strdup(arg);
	char *p = copy;
	if (*p == '+' || *p == '-') {
		sign = *p == '+' ? 1 : -1;
		p++;
	} else if (copy[len - 1] == '+' || copy[len - 1] == '-') {
		sign = copy[len - 1] == '+' ? 1 : -1;
		copy[len - 1] = '\0';
	}
	char *end;
	double value = strtod(p, &end);
	bool ok = end != p;
	if (ok && *end == '%') {
		end++;
	} else if (ok) {
		value *= 100;   // a factor: 0.5 is 50%
	}
	ok = ok && *end == '\0' && value >= 0;
	free(copy);
	*relative = sign != 0;
	*percent = sign < 0 ? -value : value;
	return ok;
}

static struct cmd_results *done(void) {
	bar_refresh_readings();
	platform_ui_refresh();
	return cmd_results_new(CMD_SUCCESS, NULL);
}

static struct cmd_results *pactl(int argc, char **argv) {
	if (argc != 4) {
		return NULL;
	}
	const char *sub = argv[1], *target = argv[2], *value = argv[3];
	bool sink_cmd = strcmp(sub, "set-sink-volume") == 0 ||
		strcmp(sub, "set-sink-mute") == 0;
	bool source_cmd = strcmp(sub, "set-source-volume") == 0 ||
		strcmp(sub, "set-source-mute") == 0;
	if ((sink_cmd && !is_sink(target)) || (source_cmd && !is_source(target)) ||
			(!sink_cmd && !source_cmd)) {
		return NULL;
	}
	bool source = source_cmd;
	if (strstr(sub, "-mute") != NULL) {
		int mode = mute_mode(value);
		if (mode == -2) {
			return NULL;
		}
		sysinfo_audio_set_mute(source, mode);
		return done();
	}
	bool relative;
	double percent;
	if (!parse_volume(value, &relative, &percent)) {
		return NULL;
	}
	set_volume(source, relative ? current_volume(source) + percent : percent,
		100);
	return done();
}

static struct cmd_results *wpctl(int argc, char **argv) {
	if (argc < 4) {
		return NULL;
	}
	double max = 100;
	int i = 2;
	if (strcmp(argv[1], "set-volume") == 0 && i + 1 < argc &&
			(strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--limit") == 0)) {
		max = strtod(argv[i + 1], NULL) * 100;
		i += 2;
	}
	if (i + 2 != argc) {
		return NULL;
	}
	const char *target = argv[i], *value = argv[i + 1];
	if (!is_sink(target) && !is_source(target)) {
		return NULL;
	}
	bool source = is_source(target);
	if (strcmp(argv[1], "set-mute") == 0) {
		int mode = mute_mode(value);
		if (mode == -2) {
			return NULL;
		}
		sysinfo_audio_set_mute(source, mode);
		return done();
	}
	if (strcmp(argv[1], "set-volume") != 0) {
		return NULL;
	}
	bool relative;
	double percent;
	if (!parse_volume(value, &relative, &percent)) {
		return NULL;
	}
	set_volume(source, relative ? current_volume(source) + percent : percent,
		max);
	return done();
}

static struct cmd_results *pamixer(int argc, char **argv) {
	bool source = false;
	int action = 0;   // 'i', 'd', 's', 't', 'm', 'u'
	double amount = 0;
	for (int i = 1; i < argc; ++i) {
		const char *a = argv[i];
		if (strcmp(a, "--default-source") == 0) {
			source = true;
		} else if (strcmp(a, "--allow-boost") == 0) {
			// Windows has no volume above 100%.
		} else if ((strcmp(a, "-i") == 0 || strcmp(a, "--increase") == 0 ||
				strcmp(a, "-d") == 0 || strcmp(a, "--decrease") == 0 ||
				strcmp(a, "--set-volume") == 0) && i + 1 < argc) {
			action = a[1] == 'i' || strcmp(a, "--increase") == 0 ? 'i' :
				a[1] == 'd' || strcmp(a, "--decrease") == 0 ? 'd' : 's';
			amount = strtod(argv[++i], NULL);
		} else if (strcmp(a, "-t") == 0 || strcmp(a, "--toggle-mute") == 0) {
			action = 't';
		} else if (strcmp(a, "-m") == 0 || strcmp(a, "--mute") == 0) {
			action = 'm';
		} else if (strcmp(a, "-u") == 0 || strcmp(a, "--unmute") == 0) {
			action = 'u';
		} else {
			return NULL;   // a query (--get-volume) or unknown: the program
		}
	}
	switch (action) {
	case 'i':
	case 'd':
		set_volume(source, current_volume(source) +
			(action == 'i' ? amount : -amount), 100);
		return done();
	case 's':
		set_volume(source, amount, 100);
		return done();
	case 't':
	case 'm':
	case 'u':
		sysinfo_audio_set_mute(source, action == 't' ? -1 : action == 'm');
		return done();
	default:
		return NULL;
	}
}

struct cmd_results *audio_builtin(const char *name, int argc, char **argv) {
	if (strcmp(name, "pactl") == 0) {
		return pactl(argc, argv);
	}
	if (strcmp(name, "wpctl") == 0) {
		return wpctl(argc, argv);
	}
	if (strcmp(name, "pamixer") == 0) {
		return pamixer(argc, argv);
	}
	return NULL;
}
