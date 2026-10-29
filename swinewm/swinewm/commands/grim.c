// grim, the screenshot tool sway users bind to Print, with slurp (select
// an area), wl-copy (the clipboard) and sway's grimshot helper. None of
// them exist on Windows, so like notify-send they are swinewm commands, and
// "exec grim ..." in a sway config is routed here, including the shell
// forms sway configs use:
//
//   grim                                      the whole desktop
//   grim -g "$(slurp)" file.png               an area the user drags
//   slurp | grim -g - file.png                the same
//   grim -g "$(slurp)" - | wl-copy            an area to the clipboard
//   grimshot --notify copy|save|savecopy active|screen|output|area|window|anything
//
// Follows grim(1) and slurp(1): PNG or JPEG (-t), saved to the given file
// or to $GRIM_DEFAULT_DIR / the Pictures folder as <date>_grim.png. The
// selection itself is drawn by the platform, which calls grim_slurp_done
// when the user has made it; what was to happen next waits until then.
// grimshot's logic follows sway-contrib's grimshot script (MIT).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/grim.h"
#include "swinewm/input/seat.h"
#include "swinewm/notify.h"
#include "swinewm/platform.h"
#include "swinewm/server.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/view.h"
#include "swinewm/tree/workspace.h"

static bool parse_geometry(const char *text, struct swine_box *box) {
	char extra;
	return sscanf(text, "%d,%d %dx%d%c", &box->x, &box->y, &box->width,
		&box->height, &extra) == 4 && box->width > 0 && box->height > 0;
}

// All outputs together, as grim captures by default.
static struct swine_box desktop_box(void) {
	struct swine_box box = {0};
	for (int i = 0; i < root->outputs->length; ++i) {
		struct swine_output *o = root->outputs->items[i];
		if (i == 0) {
			box = o->box;
			continue;
		}
		int x2 = box.x + box.width, y2 = box.y + box.height;
		int ox2 = o->box.x + o->box.width, oy2 = o->box.y + o->box.height;
		box.x = o->box.x < box.x ? o->box.x : box.x;
		box.y = o->box.y < box.y ? o->box.y : box.y;
		box.width = (ox2 > x2 ? ox2 : x2) - box.x;
		box.height = (oy2 > y2 ? oy2 : y2) - box.y;
	}
	return box;
}

static char *dated_path(const char *dir, const char *format, const char *ext) {
	char name[128];
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	if (!tm || !strftime(name, sizeof(name), format, tm)) {
		snprintf(name, sizeof(name), "screenshot");
	}
	const char *sep = strchr(dir, '\\') ? "\\" : "/";
	return format_str("%s%s%s.%s", dir, sep, name, ext);
}

// $name, else the Pictures folder.
static char *picture_dir(const char *env) {
	const char *dir = env ? getenv(env) : NULL;
	if (dir && *dir) {
		return swine_strdup(dir);
	}
	char *pictures = platform_pictures_dir();
	return pictures ? pictures : swine_strdup(".");
}

static char *default_path(bool jpeg) {
	char *dir = picture_dir("GRIM_DEFAULT_DIR");
	char *path = dated_path(dir, "%Y%m%d_%Hh%Mm%Ss_grim", jpeg ? "jpeg" : "png");
	free(dir);
	return path;
}

static bool is_jpeg_path(const char *path) {
	size_t len = strlen(path);
	return (len > 4 && swine_strcasecmp(path + len - 4, ".jpg") == 0) ||
		(len > 5 && swine_strcasecmp(path + len - 5, ".jpeg") == 0);
}

// --- slurp ---------------------------------------------------------------

static void slurp_defaults(struct slurp_options *o) {
	memset(o, 0, sizeof(*o));
	o->background = (struct swine_color){0xFF, 0xFF, 0xFF, 0x40};
	o->border = (struct swine_color){0x00, 0x00, 0x00, 0xFF};
	o->selection = (struct swine_color){0x00, 0x00, 0x00, 0x00};
	o->choice = (struct swine_color){0xFF, 0xFF, 0xFF, 0x40};
	o->border_weight = 2;
	strcpy(o->font_family, "sans-serif");
	o->choices = create_list();
}

static void slurp_free_choices(list_t *choices) {
	for (int i = 0; choices && i < choices->length; ++i) {
		struct slurp_choice *c = choices->items[i];
		free(c->label);
		free(c);
	}
	list_free(choices);
}

static void add_choice(struct slurp_options *o, struct swine_box box,
		const char *label) {
	struct slurp_choice *c = calloc(1, sizeof(*c));
	c->box = box;
	c->label = label ? swine_strdup(label) : NULL;
	list_add(o->choices, c);
}

// "#RRGGBB[AA]", the # optional.
static bool parse_slurp_color(const char *text, struct swine_color *out) {
	if (*text == '#') {
		text++;
	}
	size_t len = strlen(text);
	if ((len != 6 && len != 8) || strspn(text, "0123456789abcdefABCDEF") != len) {
		return false;
	}
	unsigned long v = strtoul(text, NULL, 16);
	if (len == 6) {
		v = (v << 8) | 0xFF;
	}
	*out = (struct swine_color){(uint8_t)(v >> 24), (uint8_t)(v >> 16),
		(uint8_t)(v >> 8), (uint8_t)v};
	return true;
}

static void add_output_choices(struct slurp_options *o) {
	for (int i = 0; i < root->outputs->length; ++i) {
		struct swine_output *output = root->outputs->items[i];
		if (output->enabled) {
			add_choice(o, output->box, output->name);
		}
	}
}

// slurp's options (argv without "slurp") over o's defaults. *format gets
// -f (caller frees).
static struct cmd_results *parse_slurp(int argc, char **argv,
		struct slurp_options *o, char **format) {
	for (int i = 0; i < argc; ++i) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;
		struct swine_color *color = strcmp(a, "-b") == 0 ? &o->background :
			strcmp(a, "-c") == 0 ? &o->border :
			strcmp(a, "-s") == 0 ? &o->selection :
			strcmp(a, "-B") == 0 ? &o->choice : NULL;
		if (color && v) {
			if (!parse_slurp_color(v, color)) {
				return cmd_results_new(CMD_INVALID, "slurp: invalid color '%s'", v);
			}
			i++;
		} else if (strcmp(a, "-w") == 0 && v) {
			o->border_weight = atoi(v);
			i++;
		} else if (strcmp(a, "-F") == 0 && v) {
			snprintf(o->font_family, sizeof(o->font_family), "%s", v);
			i++;
		} else if (strcmp(a, "-f") == 0 && v) {
			free(*format);
			*format = swine_strdup(v);
			i++;
		} else if (strcmp(a, "-a") == 0 && v) {
			if (sscanf(v, "%d:%d", &o->aspect_w, &o->aspect_h) != 2 ||
					o->aspect_w <= 0 || o->aspect_h <= 0) {
				return cmd_results_new(CMD_INVALID,
					"slurp: invalid aspect ratio '%s'", v);
			}
			i++;
		} else if (strcmp(a, "-d") == 0) {
			o->dimensions = true;
		} else if (strcmp(a, "-p") == 0) {
			o->single_point = true;
		} else if (strcmp(a, "-o") == 0) {
			add_output_choices(o);
		} else if (strcmp(a, "-r") == 0) {
			o->restrict_choice = true;
		} else if (strcmp(a, "-x") == 0) {
			o->crosshairs = true;
		} else {
			return cmd_results_new(CMD_INVALID, "slurp: unknown option '%s'", a);
		}
	}
	if (o->single_point && o->restrict_choice) {
		return cmd_results_new(CMD_INVALID, "-p and -r cannot be used together");
	}
	return NULL;
}

char *slurp_format(const char *format, const struct swine_box *box,
		const char *label) {
	// The output holding the top left corner, for %X %Y %W %H %o.
	struct swine_output *output = NULL;
	for (int i = 0; i < root->outputs->length && !output; ++i) {
		struct swine_output *o = root->outputs->items[i];
		if (box->x >= o->box.x && box->x < o->box.x + o->box.width &&
				box->y >= o->box.y && box->y < o->box.y + o->box.height) {
			output = o;
		}
	}
	char *out = swine_strdup("");
	for (const char *p = format; *p; ++p) {
		char piece[64] = "";
		const char *text = piece;
		if (*p == '%' && p[1]) {
			++p;
			int ox = output ? output->box.x : 0, oy = output ? output->box.y : 0;
			int right = output ? output->box.x + output->box.width : 0;
			int bottom = output ? output->box.y + output->box.height : 0;
			switch (*p) {
			case 'x': snprintf(piece, sizeof(piece), "%d", box->x); break;
			case 'y': snprintf(piece, sizeof(piece), "%d", box->y); break;
			case 'w': snprintf(piece, sizeof(piece), "%d", box->width); break;
			case 'h': snprintf(piece, sizeof(piece), "%d", box->height); break;
			case 'X': snprintf(piece, sizeof(piece), "%d", box->x - ox); break;
			case 'Y': snprintf(piece, sizeof(piece), "%d", box->y - oy); break;
			case 'W':
				snprintf(piece, sizeof(piece), "%d", output &&
					box->x + box->width > right ? right - box->x : box->width);
				break;
			case 'H':
				snprintf(piece, sizeof(piece), "%d", output &&
					box->y + box->height > bottom ? bottom - box->y : box->height);
				break;
			case 'l': text = label ? label : ""; break;
			case 'o': text = output ? output->name : "<unknown>"; break;
			case '%': text = "%"; break;
			default: snprintf(piece, sizeof(piece), "%%%c", *p); break;
			}
		} else if (*p == '\\' && p[1] == 'n') {
			++p;
			text = "\n";
		} else {
			piece[0] = *p;
			piece[1] = '\0';
		}
		char *next = format_str("%s%s", out, text);
		free(out);
		out = next;
	}
	return out;
}

// --- capturing -----------------------------------------------------------

// What a grim (or grimshot) command line asks for.
struct capture {
	struct swine_box box;
	bool box_from_slurp;      // -g "$(slurp ...)" or -g - after slurp
	struct slurp_options slurp;
	char *slurp_format;       // -f, for geometry text
	char *path;               // NULL: none (clipboard only)
	bool jpeg;
	bool clipboard;           // | wl-copy
	bool copy_geometry;       // slurp | wl-copy: the text, not an image
	// grimshot --notify: what to say afterwards
	bool notify;
	char *notify_title;
	char *notify_body;
	char *notify_icon;
	char *subject;            // grimshot's WHAT
	bool grimshot_save;
	bool grimshot_savecopy;
};

static void capture_free(struct capture *c) {
	if (!c) {
		return;
	}
	slurp_free_choices(c->slurp.choices);
	free(c->slurp_format);
	free(c->path);
	free(c->notify_title);
	free(c->notify_body);
	free(c->notify_icon);
	free(c->subject);
	free(c);
}

static struct capture *capture_create(void) {
	struct capture *c = calloc(1, sizeof(*c));
	c->box = desktop_box();
	slurp_defaults(&c->slurp);
	return c;
}

static void notify_grimshot(const char *title, const char *body,
		const char *icon, bool critical) {
	struct notify_params p = {
		.app_name = "grimshot",
		.summary = title,
		.body = body,
		.app_icon = icon,
		.urgency = critical ? NOTIFY_URGENCY_CRITICAL : NOTIFY_URGENCY_NORMAL,
		.expire_timeout = 3000,
	};
	notify_add(&p, platform_now_ms());
	platform_ui_refresh();
}

static const char *base_name(const char *path) {
	const char *base = path;
	for (const char *p = path; *p; ++p) {
		if (*p == '/' || *p == '\\') {
			base = p + 1;
		}
	}
	return base;
}

static struct cmd_results *run_capture(struct capture *c) {
	if (c->copy_geometry) {
		char *text = slurp_format(c->slurp_format ? c->slurp_format :
			"%x,%y %wx%h\\n", &c->box, NULL);
		bool ok = platform_clipboard_set_text(text);
		free(text);
		return ok ? cmd_results_new(CMD_SUCCESS, NULL) :
			cmd_results_new(CMD_FAILURE, "Clipboard error");
	}
	if (c->box.width <= 0 || c->box.height <= 0) {
		return cmd_results_new(CMD_FAILURE, "Nothing to capture");
	}
	char *error = NULL;
	if (!platform_screenshot(&c->box, c->path, c->jpeg, c->clipboard, &error)) {
		struct cmd_results *res = cmd_results_new(CMD_FAILURE,
			"Unable to %s screenshot%s%s: %s", c->path ? "save" : "copy",
			c->path ? " " : "", c->path ? c->path : "", error ? error : "");
		if (c->notify) {
			notify_grimshot("Screenshot",
				"Error: Error taking screenshot with grim", NULL, true);
		}
		free(error);
		return res;
	}
	if (c->path) {
		swine_log(SWINE_INFO, "Saved screenshot %s", c->path);
	}
	if (c->notify) {
		// grimshot's notifyOk
		if (c->grimshot_save || c->grimshot_savecopy) {
			char *title = format_str("Screenshot of %s", c->subject);
			char *body = format_str("%s%s", base_name(c->path),
				c->grimshot_savecopy ? " and clipboard" : "");
			notify_grimshot(title, body, c->path, false);
			free(title);
			free(body);
		} else {
			char *body = format_str("%s copied to clipboard", c->subject);
			notify_grimshot("Screenshot", body, NULL, false);
			free(body);
		}
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

// --- waiting for a selection ---------------------------------------------

static struct {
	struct capture *capture;
	char *rest;
} pending = {0};

bool grim_slurp_active(void) {
	return pending.capture != NULL;
}

static struct cmd_results *start_slurp(struct capture *c, const char *rest,
		bool *deferred) {
	if (pending.capture) {
		capture_free(c);
		return cmd_results_new(CMD_FAILURE, "slurp: a selection is already "
			"being made");
	}
	if (c->slurp.restrict_choice && !c->slurp.choices->length) {
		capture_free(c);
		return cmd_results_new(CMD_FAILURE, "slurp: nothing to choose from");
	}
	if (!platform_slurp(&c->slurp)) {
		capture_free(c);
		return cmd_results_new(CMD_FAILURE, "slurp: unable to show the "
			"selection");
	}
	pending.capture = c;
	pending.rest = rest ? swine_strdup(rest) : NULL;
	*deferred = true;
	return cmd_results_new(CMD_SUCCESS, NULL);
}

void grim_slurp_done(bool ok, const struct swine_box *box, const char *label) {
	struct capture *c = pending.capture;
	char *rest = pending.rest;
	pending.capture = NULL;
	pending.rest = NULL;
	if (!c) {
		return;
	}
	(void)label;
	if (ok && box && box->width > 0 && box->height > 0) {
		c->box = *box;
		struct cmd_results *res = run_capture(c);
		if (res->status != CMD_SUCCESS) {
			swine_log(SWINE_ERROR, "%s", res->error ? res->error : "grim failed");
		} else if (rest) {
			// The rest of the "&&" chain, as sh would run it now.
			char *line = format_str("exec %s", rest);
			platform_queue_command(line);
			free(line);
		}
		free_cmd_results(res);
	} else {
		// slurp exits with an error: the chain stops.
		swine_log(SWINE_DEBUG, "slurp: selection cancelled");
	}
	capture_free(c);
	free(rest);
}

void grim_finish(void) {
	capture_free(pending.capture);
	free(pending.rest);
	pending.capture = NULL;
	pending.rest = NULL;
}

// --- grim ----------------------------------------------------------------

// "$(slurp -d)" (the quotes already removed): slurp's options into c.
static struct cmd_results *slurp_substitution(const char *text,
		struct capture *c) {
	size_t len = strlen(text);
	if (!has_prefix(text, "$(") || len < 3 || text[len - 1] != ')') {
		return cmd_results_new(CMD_INVALID, "Invalid geometry '%s' (expected "
			"\"x,y wxh\")", text);
	}
	char *inner = swine_strndup(text + 2, len - 3);
	int argc;
	char **argv = split_args(inner, &argc);
	for (int i = 0; i < argc; ++i) {
		strip_quotes(argv[i]);
	}
	struct cmd_results *res = NULL;
	if (argc == 0 || strcmp(argv[0], "slurp") != 0) {
		res = cmd_results_new(CMD_INVALID, "grim: only $(slurp ...) can give "
			"the geometry, not '%s'", text);
	} else {
		res = parse_slurp(argc - 1, argv + 1, &c->slurp, &c->slurp_format);
		c->box_from_slurp = true;
	}
	free_argv(argc, argv);
	free(inner);
	return res;
}

// grim's options (argv without "grim"). slurp_before: "slurp | grim", so
// "-g -" reads its geometry; piped: "grim ... | wl-copy".
static struct cmd_results *parse_grim(int argc, char **argv, struct capture *c,
		bool slurp_before, bool piped) {
	const char *file = NULL;
	for (int i = 0; i < argc; ++i) {
		const char *arg = argv[i];
		bool has_value = i + 1 < argc;
		if (strcmp(arg, "-g") == 0 && has_value) {
			const char *g = argv[++i];
			struct cmd_results *res = NULL;
			if (strcmp(g, "-") == 0) {
				if (!slurp_before) {
					return cmd_results_new(CMD_INVALID, "grim: -g - reads the "
						"geometry from slurp: slurp | grim -g - ...");
				}
				c->box_from_slurp = true;
			} else if (has_prefix(g, "$(")) {
				if ((res = slurp_substitution(g, c))) {
					return res;
				}
			} else if (!parse_geometry(g, &c->box)) {
				return cmd_results_new(CMD_INVALID,
					"Invalid geometry '%s' (expected \"x,y wxh\")", g);
			}
		} else if (strcmp(arg, "-o") == 0 && has_value) {
			struct swine_output *output = output_by_name_or_id(argv[++i]);
			if (!output) {
				return cmd_results_new(CMD_INVALID, "Unknown output '%s'",
					argv[i]);
			}
			c->box = output->box;
		} else if (strcmp(arg, "-t") == 0 && has_value) {
			++i;
			if (swine_strcasecmp(argv[i], "jpeg") == 0 ||
					swine_strcasecmp(argv[i], "jpg") == 0) {
				c->jpeg = true;
			} else if (swine_strcasecmp(argv[i], "png") != 0) {
				return cmd_results_new(CMD_INVALID,
					"Unsupported image type '%s' (png or jpeg)", argv[i]);
			}
		} else if ((strcmp(arg, "-q") == 0 || strcmp(arg, "-l") == 0 ||
				strcmp(arg, "-s") == 0) && has_value) {
			++i;   // quality, compression level, scale: defaults are used
		} else if (strcmp(arg, "-c") == 0) {
			// The cursor is never part of the capture.
		} else if (strcmp(arg, "-h") == 0) {
			return cmd_results_new(CMD_SUCCESS,
				"Usage: grim [-t png|jpeg] [-g \"x,y wxh\"] [-o output] "
				"[output-file]");
		} else if (arg[0] == '-' && arg[1] != '\0') {
			return cmd_results_new(CMD_INVALID, "Unsupported grim option '%s'",
				arg);
		} else if (!file) {
			file = arg;
		} else {
			return cmd_results_new(CMD_INVALID, "Too many arguments to grim");
		}
	}
	if (file && strcmp(file, "-") == 0) {
		if (!piped) {
			return cmd_results_new(CMD_INVALID, "grim: standard output only "
				"works piped to wl-copy");
		}
		c->clipboard = true;
	} else if (piped) {
		return cmd_results_new(CMD_INVALID, "grim: pipe its output with "
			"grim ... - | wl-copy");
	} else {
		c->path = file ? platform_expand_path(file) : default_path(c->jpeg);
		c->jpeg = c->jpeg || (file && is_jpeg_path(c->path));
	}
	return NULL;
}

struct cmd_results *cmd_grim(int argc, char **argv) {
	struct capture *c = capture_create();
	struct cmd_results *res = parse_grim(argc, argv, c, false, false);
	if (!res && c->box_from_slurp) {
		res = cmd_results_new(CMD_INVALID, "grim: $(slurp) works in exec");
	}
	if (!res) {
		res = run_capture(c);
	}
	capture_free(c);
	return res;
}

// --- grimshot ------------------------------------------------------------

static const char grimshot_usage[] =
	"Usage:\n"
	"  grimshot [--notify] [--cursor] [--wait N] (copy|save|savecopy) "
	"[active|screen|output|area|window|anything] [FILE|-]\n"
	"  grimshot check\n"
	"  grimshot usage";

// The windows on screen, as grimshot gets them from get_tree for slurp.
static void add_window_choices(struct slurp_options *o) {
	for (int i = 0; i < server.views->length; ++i) {
		struct swine_view *view = server.views->items[i];
		if (view->container && view_is_visible(view)) {
			add_choice(o, view->container->box, NULL);
		}
	}
}

static struct cmd_results *grimshot(int argc, char **argv, const char *rest,
		bool *deferred) {
	struct capture *c = capture_create();
	const char *positional[3] = {NULL, NULL, NULL};
	int npos = 0;
	for (int i = 0; i < argc; ++i) {
		const char *a = argv[i];
		if (strcmp(a, "-n") == 0 || strcmp(a, "--notify") == 0) {
			c->notify = true;
		} else if (strcmp(a, "-c") == 0 || strcmp(a, "--cursor") == 0 ||
				strcmp(a, "-f") == 0 || strcmp(a, "--freeze") == 0) {
			// The cursor is never captured; nothing to freeze.
		} else if (strcmp(a, "-w") == 0 || strcmp(a, "--wait") == 0) {
			if (i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) {
				i++;
				swine_log(SWINE_INFO, "grimshot: --wait is not supported; "
					"taking the screenshot now");
			}
		} else if (npos < 3) {
			positional[npos++] = a;
		}
	}
	const char *action = positional[0] ? positional[0] : "usage";
	const char *subject = positional[1] ? positional[1] : "screen";
	const char *file = positional[2];
	if (strcmp(action, "usage") == 0 || strcmp(action, "check") == 0) {
		capture_free(c);
		return cmd_results_new(CMD_SUCCESS, strcmp(action, "check") == 0 ?
			"grimshot, grim, slurp, wl-copy and notify-send are built in" :
			grimshot_usage);
	}
	bool copy = strcmp(action, "copy") == 0;
	c->grimshot_save = strcmp(action, "save") == 0;
	c->grimshot_savecopy = strcmp(action, "savecopy") == 0;
	if (!copy && !c->grimshot_save && !c->grimshot_savecopy) {
		capture_free(c);
		return cmd_results_new(CMD_INVALID, "%s", grimshot_usage);
	}
	if (copy || c->grimshot_savecopy) {
		c->clipboard = true;
	}
	if (!copy) {
		if (file && strcmp(file, "-") == 0) {
			capture_free(c);
			return cmd_results_new(CMD_INVALID,
				"grimshot: saving to standard output is not supported");
		}
		if (file) {
			c->path = platform_expand_path(file);
		} else {
			char *dir = picture_dir("XDG_SCREENSHOTS_DIR");
			const char *format = getenv("GRIMSHOT_FILENAME_FORMAT");
			// date -Ins has colons, which Windows file names cannot.
			c->path = dated_path(dir, format && *format ? format :
				"%Y-%m-%dT%H-%M-%S", "png");
			free(dir);
		}
		c->jpeg = is_jpeg_path(c->path);
	}
	c->subject = swine_strdup(subject);
	if (strcmp(subject, "active") == 0) {
		struct swine_container *con = seat_get_focused_container();
		if (!con || !con->view) {
			capture_free(c);
			return cmd_results_new(CMD_FAILURE, "grimshot: no active window");
		}
		c->box = con->box;
		free(c->subject);
		c->subject = format_str("%s window",
			con->view->app_id ? con->view->app_id : "");
	} else if (strcmp(subject, "screen") == 0) {
		free(c->subject);
		c->subject = swine_strdup("Screen");
	} else if (strcmp(subject, "output") == 0) {
		struct swine_workspace *ws = seat_get_focused_workspace();
		if (!ws || !ws->output) {
			capture_free(c);
			return cmd_results_new(CMD_FAILURE, "grimshot: no active output");
		}
		c->box = ws->output->box;
		free(c->subject);
		c->subject = swine_strdup(ws->output->name);
	} else if (strcmp(subject, "area") == 0) {
		c->slurp.dimensions = true;
		c->box_from_slurp = true;
		free(c->subject);
		c->subject = swine_strdup("Area");
	} else if (strcmp(subject, "window") == 0) {
		add_window_choices(&c->slurp);
		c->slurp.restrict_choice = true;
		c->box_from_slurp = true;
		free(c->subject);
		c->subject = swine_strdup("Window");
	} else if (strcmp(subject, "anything") == 0) {
		add_window_choices(&c->slurp);
		add_output_choices(&c->slurp);
		c->box_from_slurp = true;
		free(c->subject);
		c->subject = swine_strdup("Selection");
	} else {
		capture_free(c);
		return cmd_results_new(CMD_INVALID, "Error: Unknown subject to take a "
			"screenshot from '%s'", subject);
	}
	if (c->box_from_slurp) {
		return start_slurp(c, rest, deferred);
	}
	struct cmd_results *res = run_capture(c);
	capture_free(c);
	return res;
}

// --- exec lines ----------------------------------------------------------

static bool named(const char *arg, const char *name) {
	return arg && strcmp(arg, name) == 0;
}

// The stages of argv split at "|": starts[k], counts[k]. Returns the count.
static int split_pipe(int argc, char **argv, int *starts, int *counts,
		int max) {
	int n = 0;
	int start = 0;
	for (int i = 0; i <= argc; ++i) {
		if (i < argc && strcmp(argv[i], "|") != 0) {
			continue;
		}
		if (n == max) {
			return max + 1;
		}
		starts[n] = start;
		counts[n] = i - start;
		n++;
		start = i + 1;
	}
	return n;
}

static bool wl_copy_options_only(int argc, char **argv) {
	for (int i = 1; i < argc; ++i) {
		const char *a = argv[i];
		if ((strcmp(a, "-t") == 0 || strcmp(a, "--type") == 0) && i + 1 < argc) {
			i++;
		} else if (a[0] != '-' || strcmp(a, "-") == 0) {
			return false;
		}
	}
	return true;
}

bool grim_exec_handles(int argc, char **argv) {
	int starts[3], counts[3];
	int n = split_pipe(argc, argv, starts, counts, 2);
	if (n < 1 || n > 2 || counts[0] == 0 || (n == 2 && counts[1] == 0)) {
		return false;
	}
	const char *first = argv[starts[0]];
	if (n == 1) {
		return named(first, "grim") || named(first, "grimshot") ||
			named(first, "wl-copy");
	}
	const char *second = argv[starts[1]];
	return ((named(first, "grim") || named(first, "slurp")) &&
		named(second, "wl-copy") &&
		wl_copy_options_only(counts[1], argv + starts[1])) ||
		(named(first, "slurp") && named(second, "grim"));
}

// wl-copy [options] <text...>, or --clear.
static struct cmd_results *wl_copy(int argc, char **argv) {
	char *text = NULL;
	bool clear = false;
	for (int i = 1; i < argc; ++i) {
		const char *a = argv[i];
		if ((strcmp(a, "-t") == 0 || strcmp(a, "--type") == 0) && i + 1 < argc) {
			i++;
		} else if (strcmp(a, "-c") == 0 || strcmp(a, "--clear") == 0) {
			clear = true;
		} else if (a[0] == '-' && a[1] && !text) {
			// -n, -o, -f, -p: they change nothing here
		} else {
			char *next = text ? format_str("%s %s", text, a) : swine_strdup(a);
			free(text);
			text = next;
		}
	}
	if (!text && !clear) {
		return cmd_results_new(CMD_INVALID,
			"wl-copy: give the text, or pipe grim or slurp into it");
	}
	bool ok = platform_clipboard_set_text(clear ? "" : text);
	free(text);
	return ok ? cmd_results_new(CMD_SUCCESS, NULL) :
		cmd_results_new(CMD_FAILURE, "Clipboard error");
}

struct cmd_results *grim_exec(int argc, char **argv, const char *rest,
		bool *deferred) {
	*deferred = false;
	if (!grim_exec_handles(argc, argv)) {
		return NULL;
	}
	int starts[3], counts[3];
	int n = split_pipe(argc, argv, starts, counts, 2);
	char **first = argv + starts[0];
	if (n == 1 && named(first[0], "grimshot")) {
		return grimshot(counts[0] - 1, first + 1, rest, deferred);
	}
	if (n == 1 && named(first[0], "wl-copy")) {
		return wl_copy(counts[0], first);
	}
	struct capture *c = capture_create();
	struct cmd_results *res = NULL;
	if (named(first[0], "slurp")) {
		res = parse_slurp(counts[0] - 1, first + 1, &c->slurp, &c->slurp_format);
		c->box_from_slurp = true;
		if (!res && named(argv[starts[1]], "wl-copy")) {
			c->copy_geometry = true;
		} else if (!res) {
			res = parse_grim(counts[1] - 1, argv + starts[1] + 1, c, true, false);
		}
	} else {
		res = parse_grim(counts[0] - 1, first + 1, c, false, n == 2);
	}
	if (res) {
		capture_free(c);
		return res;
	}
	if (c->box_from_slurp) {
		return start_slurp(c, rest, deferred);
	}
	res = run_capture(c);
	capture_free(c);
	return res;
}
