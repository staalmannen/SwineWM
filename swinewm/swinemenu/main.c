// swinemenu: a small application launcher in the spirit of wofi/wmenu, the
// default "$menu" of swinewm's config. It only picks: the chosen entry is
// handed to swinewm as an "exec" command over IPC, so swinewm is the one
// place that knows how to start things on Windows (Store apps included).
//
// It reads wofi's files: wofi\config (the options below, as key=value) and
// wofi\style.css (wofi's widget names: window, #outer-box, #input,
// #inner-box, #entry, #entry:selected, #img, #text), from the swinewm
// config directories. Without a style sheet it takes the bar's colours and
// font (get_bar_config), so the menu matches the Waybar style.
//
//   swinemenu [--show drun|run|dmenu|window] [--dmenu] [--prompt text]
//             [--lines n] [--width px|n%] [--height px|n%] [-I] ...
//   swinemenu --open-with <file>
//   swinemenu --install-open-with | --uninstall-open-with
//
// drun lists the Start menu's apps and Store apps, run lists programs on
// PATH, dmenu reads choices from standard input and prints the chosen one,
// window lists swinewm's windows and focuses the chosen one (rofi's
// "window" mode). --open-with lists the programs registered for a file and
// opens it with the chosen one (Windows' own "Open with" dialog needs
// Explorer).
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "fileutil.h"
#include "ipc.h"
#include "ipc-client.h"
#include "json.h"
#include "menu.h"
#include "stringop.h"
#include "swinemenu.h"
#include "swinewm/bar/css.h"
#include "swinewm_version.h"
#include "../swinewm/win32/win32.h"

// The styles of wofi's widgets.
struct styles {
	struct swine_style window, outer, input, inner, entry, entry_selected,
		img, text, text_selected;
};

// Where things are, in pixels of the window.
struct layout {
	int width, height;
	RECT input;               // empty with hide_search
	RECT list;                // the rows
	int row_pitch;            // a row and its margins
	int row_height;           // the entry box
	int text_height;
	int image_size;           // 0 without images
};

struct icon {
	bool tried;
	uint32_t *px;             // premultiplied BGRA, image_size square
};

static struct {
	struct menu_options opt;
	list_t *entries;          // struct menu_entry *
	list_t *shown;            // indexes into entries (menu_filter)
	struct icon *icons;       // per entry
	wchar_t query[512];
	int selected;             // index into shown
	int top;                  // first visible row
	int lines;
	struct styles st;
	double scale;             // the monitor's DPI / 96
	struct layout layout;
	HFONT input_font, text_font, text_selected_font;
	HWND hwnd;
	int x, y;                 // the window's position
	bool images;
	bool done;
	char *history_path;
} menu;

static const wchar_t window_class[] = L"swinemenu";

static int px(double css) {
	return (int)(css * menu.scale + 0.5);
}

// --- options and files -----------------------------------------------------

static char *env_utf8(const wchar_t *name) {
	wchar_t buf[MAX_PATH];
	DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
	return n > 0 && n < MAX_PATH ? swinemenu_utf16_to_utf8(buf) : NULL;
}

// wofi\<name> in swinewm's config directories (as swinewm looks for its
// own files), then $WOFI_CONFIG_DIR\wofi and $XDG_CONFIG_HOME\wofi.
static char *find_wofi_file(const char *name) {
	char *profile = env_utf8(L"USERPROFILE");
	char *xdg = env_utf8(L"XDG_CONFIG_HOME");
	char *appdata = env_utf8(L"APPDATA");
	char *wofi = env_utf8(L"WOFI_CONFIG_DIR");
	wchar_t exe[MAX_PATH];
	DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
	wchar_t *slash = n ? wcsrchr(exe, L'\\') : NULL;
	if (slash) {
		*slash = L'\0';
	}
	char *exe_dir = slash ? swinemenu_utf16_to_utf8(exe) : NULL;
	char *candidates[6] = {
		profile ? format_str("%s\\.swinewm\\wofi\\%s", profile, name) : NULL,
		xdg ? format_str("%s\\swinewm\\wofi\\%s", xdg, name) : NULL,
		appdata ? format_str("%s\\swinewm\\wofi\\%s", appdata, name) : NULL,
		exe_dir ? format_str("%s\\wofi\\%s", exe_dir, name) : NULL,
		wofi ? format_str("%s\\wofi\\%s", wofi, name) : NULL,
		xdg ? format_str("%s\\wofi\\%s", xdg, name) : NULL,
	};
	char *found = NULL;
	for (int i = 0; i < 6; ++i) {
		if (!found && candidates[i]) {
			FILE *f = swine_fopen(candidates[i], "r");
			if (f) {
				fclose(f);
				found = candidates[i];
				continue;
			}
		}
		free(candidates[i]);
	}
	free(profile);
	free(xdg);
	free(appdata);
	free(wofi);
	free(exe_dir);
	return found;
}

static char *read_text(const char *path) {
	FILE *f = swine_fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	rewind(f);
	char *text = NULL;
	if (size >= 0) {
		text = malloc((size_t)size + 1);
		size_t got = fread(text, 1, (size_t)size, f);
		text[got] = '\0';
	}
	fclose(f);
	return text;
}

static void write_stdout(const char *text) {
	HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	if (out && out != INVALID_HANDLE_VALUE) {
		DWORD n;
		WriteFile(out, text, (DWORD)strlen(text), &n, NULL);
		WriteFile(out, "\n", 1, &n, NULL);
	}
}

static void complain(const char *text) {
	HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
	if (err && err != INVALID_HANDLE_VALUE) {
		DWORD n;
		WriteFile(err, text, (DWORD)strlen(text), &n, NULL);
		WriteFile(err, "\n", 1, &n, NULL);
	}
}

// --- style -----------------------------------------------------------------

static void hex_color(char *out, size_t size, COLORREF fallback,
		const char *text) {
	unsigned r, g, b;
	if (text && text[0] == '#' && sscanf(text + 1, "%2x%2x%2x", &r, &g, &b) == 3) {
		snprintf(out, size, "#%02x%02x%02x", r, g, b);
	} else {
		snprintf(out, size, "#%02x%02x%02x", GetRValue(fallback),
			GetGValue(fallback), GetBValue(fallback));
	}
}

// Without wofi's style.css: the bar's colours and font as a style sheet,
// laid out as swinemenu always looked.
static char *default_css(void) {
	char bg[8], text[8], sel_bg[8], sel_text[8], border[8];
	char family[128] = "sans-serif";
	double pt = 11;
	char *reply = ipc_request(NULL, IPC_GET_BAR_CONFIG, "bar-0", NULL);
	struct json_value *bar = reply ? json_parse(reply, NULL) : NULL;
	struct json_value *colors = json_object_get(bar, "colors");
	hex_color(bg, sizeof(bg), RGB(0x32, 0x32, 0x32),
		json_get_string(colors, "background", NULL));
	hex_color(text, sizeof(text), RGB(0xff, 0xff, 0xff),
		json_get_string(colors, "statusline", NULL));
	hex_color(sel_bg, sizeof(sel_bg), RGB(0x28, 0x55, 0x77),
		json_get_string(colors, "focused_workspace_bg", NULL));
	hex_color(sel_text, sizeof(sel_text), RGB(0xff, 0xff, 0xff),
		json_get_string(colors, "focused_workspace_text", NULL));
	hex_color(border, sizeof(border), RGB(0x4c, 0x78, 0x99),
		json_get_string(colors, "focused_workspace_border", NULL));
	// "pango:Family, Fallback 12": the families and the size in points.
	const char *font = json_get_string(bar, "font", NULL);
	if (font) {
		if (has_prefix(font, "pango:")) {
			font += 6;
		}
		const char *size = strrchr(font, ' ');
		if (size && atof(size + 1) > 0) {
			pt = atof(size + 1);
		}
		size_t len = size ? (size_t)(size - font) : strlen(font);
		if (len > 0 && len < sizeof(family)) {
			memcpy(family, font, len);
			family[len] = '\0';
		}
	}
	json_value_free(bar);
	free(reply);
	double f = pt * 96.0 / 72.0;   // CSS px
	return format_str(
		"window { background-color: %s; color: %s; border: 2px solid %s; "
		"font-family: %s; font-size: %.1fpx; }\n"
		"#outer-box { padding: %dpx; }\n"
		"#input { color: %s; border-bottom: 2px solid %s; "
		"padding: %dpx %dpx; margin-bottom: %dpx; }\n"
		"#entry { padding: %dpx %dpx; }\n"
		"#entry:selected { background-color: %s; }\n"
		"#text:selected { color: %s; }\n"
		"#img { margin-right: %dpx; }\n",
		bg, text, border, family, f, (int)f, text, border, (int)(f / 3),
		(int)(f / 4), (int)(f / 2), (int)(f / 2.5), (int)(f / 2), sel_bg,
		sel_text, (int)(f / 2));
}

static void compute_styles(const struct css_stylesheet *sheet) {
	static const char *focus[] = {CSS_FOCUS, NULL};
	static const char *selected[] = {CSS_SELECTED, NULL};
	struct swine_style root;
	css_default_style(&root);
	struct styles *st = &menu.st;
	struct css_element window = {"window", "window", NULL, NULL};
	struct css_element outer = {"box", "outer-box", NULL, &window};
	struct css_element input = {"entry", "input", focus, &outer};
	struct css_element scroll = {"scrolledwindow", "scroll", NULL, &outer};
	struct css_element inner = {"flowbox", "inner-box", NULL, &scroll};
	struct css_element entry = {"flowboxchild", "entry", NULL, &inner};
	struct css_element entry_sel = {"flowboxchild", "entry", selected, &inner};
	struct css_element img = {"image", "img", NULL, &entry};
	struct css_element text = {"label", "text", NULL, &entry};
	struct css_element text_sel = {"label", "text", selected, &entry_sel};
	struct swine_style scroll_style;
	css_compute(sheet, &window, &root, &st->window);
	css_compute(sheet, &outer, &st->window, &st->outer);
	css_compute(sheet, &input, &st->outer, &st->input);
	css_compute(sheet, &scroll, &st->outer, &scroll_style);
	css_compute(sheet, &inner, &scroll_style, &st->inner);
	css_compute(sheet, &entry, &st->inner, &st->entry);
	css_compute(sheet, &entry_sel, &st->inner, &st->entry_selected);
	css_compute(sheet, &img, &st->entry, &st->img);
	css_compute(sheet, &text, &st->entry, &st->text);
	css_compute(sheet, &text_sel, &st->entry_selected, &st->text_selected);
}

static void load_style(void) {
	char *path = menu.opt.style ? swine_strdup(menu.opt.style) :
		find_wofi_file("style.css");
	char *css = path ? read_text(path) : NULL;
	if (path && !css) {
		char *msg = format_str("swinemenu: unable to read %s", path);
		complain(msg);
		free(msg);
	}
	if (!css) {
		css = default_css();
	}
	struct css_stylesheet *sheet = css_parse(css);
	compute_styles(sheet);
	css_free(sheet);
	free(css);
	free(path);
}

// The first family of a CSS list, with CSS's generic names mapped.
static HFONT style_font(const struct swine_style *s) {
	char family[128];
	const char *list = s->font_family;
	size_t len = strcspn(list, ",");
	while (len > 0 && list[len - 1] == ' ') {
		len--;
	}
	while (*list == ' ' && len > 0) {
		list++;
		len--;
	}
	snprintf(family, sizeof(family), "%.*s", (int)len, list);
	const char *mapped = family;
	if (!*family || swine_strcasecmp(family, "sans-serif") == 0 ||
			swine_strcasecmp(family, "sans") == 0 ||
			swine_strcasecmp(family, "system-ui") == 0) {
		mapped = "Segoe UI";
	} else if (swine_strcasecmp(family, "monospace") == 0) {
		mapped = "Consolas";
	} else if (swine_strcasecmp(family, "serif") == 0) {
		mapped = "Times New Roman";
	}
	wchar_t *face = swinemenu_utf8_to_utf16(mapped);
	int size = px(s->font_size > 0 ? s->font_size : 13);
	HFONT font = CreateFontW(-size, 0, 0, 0, s->bold ? FW_BOLD : FW_NORMAL,
		s->italic, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
		CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
	free(face);
	return font;
}

static int font_height(HFONT font) {
	HDC dc = CreateCompatibleDC(NULL);
	HGDIOBJ old = SelectObject(dc, font);
	TEXTMETRICW tm;
	GetTextMetricsW(dc, &tm);
	SelectObject(dc, old);
	DeleteDC(dc);
	return tm.tmHeight;
}

// --- entries ---------------------------------------------------------------

static void refilter(void) {
	list_free(menu.shown);
	char *query = swinemenu_utf16_to_utf8(menu.query);
	menu.shown = menu_filter(menu.entries, query);
	free(query);
	menu.selected = 0;
	menu.top = 0;
}

static struct menu_entry *selected_entry(void) {
	if (menu.selected < 0 || menu.selected >= menu.shown->length) {
		return NULL;
	}
	size_t index = (size_t)menu.shown->items[menu.selected];
	return menu.entries->items[index];
}

static char *history_path(void) {
	wchar_t *base = NULL;
	if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &base))) {
		return NULL;
	}
	char *base8 = swinemenu_utf16_to_utf8(base);
	CoTaskMemFree(base);
	char *dir = format_str("%s\\swinewm", base8);
	wchar_t *wdir = swinemenu_utf8_to_utf16(dir);
	CreateDirectoryW(wdir, NULL);
	free(wdir);
	enum menu_mode mode = menu.opt.mode;
	char *path = format_str("%s\\swinemenu-%s.txt", dir,
		mode == MENU_MODE_RUN ? "run" :
		mode == MENU_MODE_OPENWITH ? "openwith" :
		mode == MENU_MODE_WINDOW ? "window" : "drun");
	free(dir);
	free(base8);
	return path;
}

// An entry's icon (a file, a shortcut, or "shell:AppsFolder\<AUMID>"), as
// premultiplied pixels of size x size.
static uint32_t *load_icon(const char *source, int size) {
	wchar_t *name = swinemenu_utf8_to_utf16(source);
	IShellItemImageFactory *factory = NULL;
	HRESULT hr = SHCreateItemFromParsingName(name, NULL,
		&IID_IShellItemImageFactory, (void **)&factory);
	free(name);
	if (FAILED(hr)) {
		return NULL;
	}
	SIZE want = {size, size};
	HBITMAP bitmap = NULL;
	hr = IShellItemImageFactory_GetImage(factory, want, SIIGBF_ICONONLY,
		&bitmap);
	IShellItemImageFactory_Release(factory);
	if (FAILED(hr) || !bitmap) {
		return NULL;
	}
	BITMAP bm;
	uint32_t *pixels = NULL;
	if (GetObjectW(bitmap, sizeof(bm), &bm) && bm.bmWidth == size &&
			bm.bmHeight == size) {
		BITMAPINFO bi = {0};
		bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
		bi.bmiHeader.biWidth = size;
		bi.bmiHeader.biHeight = -size;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		pixels = malloc((size_t)size * (size_t)size * 4);
		HDC dc = GetDC(NULL);
		if (!GetDIBits(dc, bitmap, 0, (UINT)size, pixels, &bi, DIB_RGB_COLORS)) {
			free(pixels);
			pixels = NULL;
		}
		ReleaseDC(NULL, dc);
	}
	DeleteObject(bitmap);
	if (pixels) {
		size_t n = (size_t)size * (size_t)size;
		bool alpha = false;
		for (size_t i = 0; i < n && !alpha; ++i) {
			alpha = (pixels[i] >> 24) != 0;
		}
		for (size_t i = 0; !alpha && i < n; ++i) {
			pixels[i] |= 0xFF000000u;
		}
	}
	return pixels;
}

static const uint32_t *entry_icon(size_t index) {
	struct icon *icon = &menu.icons[index];
	struct menu_entry *entry = menu.entries->items[index];
	if (!icon->tried) {
		icon->tried = true;
		icon->px = entry->icon ? load_icon(entry->icon,
			menu.layout.image_size) : NULL;
	}
	return icon->px;
}

// --- launching -------------------------------------------------------------

// Starts a Store app entry ("shell:AppsFolder\<AppUserModelID>") with the
// documented IApplicationActivationManager, as swinewm's exec does. Used when
// swinewm could not start it, so the menu does not fail silently.
static const CLSID clsid_activation_manager = {0x45ba127d, 0x10a8, 0x46ea,
	{0x8a, 0xb7, 0x56, 0xea, 0x90, 0x78, 0x94, 0x3c}};
static const IID iid_activation_manager = {0x2e941141, 0x7f97, 0x4756,
	{0xba, 0x1d, 0x9d, 0xec, 0xde, 0x89, 0x4a, 0x3d}};

static bool activate_store_app(const char *command) {
	static const char prefix[] = "shell:AppsFolder\\";
	if (_strnicmp(command, prefix, sizeof(prefix) - 1) != 0) {
		return false;
	}
	wchar_t *aumid = swinemenu_utf8_to_utf16(command + sizeof(prefix) - 1);
	if (!aumid) {
		return false;
	}
	IApplicationActivationManager *manager = NULL;
	DWORD pid = 0;
	HRESULT hr = CoCreateInstance(&clsid_activation_manager, NULL, CLSCTX_ALL,
		&iid_activation_manager, (void **)&manager);
	if (SUCCEEDED(hr)) {
		hr = IApplicationActivationManager_ActivateApplication(manager, aumid,
			NULL, AO_NONE, &pid);
		IApplicationActivationManager_Release(manager);
	}
	free(aumid);
	return SUCCEEDED(hr);
}

// Asks swinewm to run command; starts it directly if swinewm is not there
// or could not start it.
static void launch(const char *command) {
	char *payload = format_str("exec %s", command);
	char *reply = ipc_request(NULL, IPC_COMMAND, payload, NULL);
	free(payload);
	bool done = reply && !strstr(reply, "\"success\":false");
	free(reply);
	if (done) {
		return;
	}
	if (activate_store_app(command)) {
		return;
	}
	wchar_t *w = swinemenu_utf8_to_utf16(command);
	wchar_t *file = w, *params = NULL;
	if (*file == L'"') {
		++file;
		wchar_t *end = wcschr(file, L'"');
		if (end) {
			*end = L'\0';
			params = end + 1;
		}
	}
	ShellExecuteW(NULL, L"open", file, params && *params ? params : NULL,
		NULL, SW_SHOWNORMAL);
	free(w);
}

static int exit_status = 1;   // 0 once something was chosen

static void close_menu(void) {
	menu.done = true;
	DestroyWindow(menu.hwnd);
}

// The query, or for a password the characters that stand for it.
static wchar_t *display_query(void) {
	size_t len = wcslen(menu.query);
	if (!menu.opt.password) {
		return _wcsdup(menu.query);
	}
	wchar_t *mask = swinemenu_utf8_to_utf16(menu.opt.password_char);
	wchar_t c = mask && *mask ? mask[0] : L'*';
	free(mask);
	wchar_t *out = calloc(len + 1, sizeof(wchar_t));
	for (size_t i = 0; i < len; ++i) {
		out[i] = c;
	}
	return out;
}

static void accept(void) {
	struct menu_entry *entry = selected_entry();
	char *typed = swinemenu_utf16_to_utf8(menu.query);
	if (menu.opt.exec_search && *typed) {
		entry = NULL;   // -e: Enter runs what was typed
	}
	switch (menu.opt.mode) {
	case MENU_MODE_DMENU:
		// dmenu prints the typed text when nothing matches.
		write_stdout(entry ? entry->name : typed);
		exit_status = 0;
		break;
	case MENU_MODE_WINDOW:
		if (entry) {
			char *reply = ipc_request(NULL, IPC_COMMAND, entry->command, NULL);
			free(reply);
			exit_status = 0;
		}
		break;
	case MENU_MODE_OPENWITH: {
		wchar_t *file = swinemenu_utf8_to_utf16(menu.opt.open_with);
		if (entry) {
			menu_history_record(menu.history_path, entry->name);
			if (!swinemenu_openwith_invoke(entry->command, file)) {
				char *line = swinemenu_openwith_fallback(entry->command, file);
				if (line) {
					launch(line);
					free(line);
				}
			}
			exit_status = 0;
		} else if (*typed) {
			// A program typed by hand: "<program> "<file>"
			char *line = format_str("%s \"%s\"", typed, menu.opt.open_with);
			launch(line);
			free(line);
			exit_status = 0;
		}
		free(file);
		break;
	}
	default:
		if (entry) {
			menu_history_record(menu.history_path, entry->name);
			launch(entry->command ? entry->command : entry->name);
			exit_status = 0;
		} else if (*typed) {
			launch(typed);   // a command line typed by hand
			exit_status = 0;
		}
		break;
	}
	free(typed);
	close_menu();
}

// --- layout ----------------------------------------------------------------

static int box_v(const struct swine_style *s) {   // padding and border
	return px(s->padding.top + s->padding.bottom) +
		px(s->border_top.width + s->border_bottom.width);
}

static int margin_v(const struct swine_style *s) {
	return px(s->margin.top + s->margin.bottom);
}

// Inside a box: its border and padding.
static RECT inside(RECT r, const struct swine_style *s) {
	r.left += px(s->border_left.width + s->padding.left);
	r.top += px(s->border_top.width + s->padding.top);
	r.right -= px(s->border_right.width + s->padding.right);
	r.bottom -= px(s->border_bottom.width + s->padding.bottom);
	return r;
}

// Within a parent: its margins.
static RECT margins(RECT r, const struct swine_style *s) {
	r.left += px(s->margin.left);
	r.top += px(s->margin.top);
	r.right -= px(s->margin.right);
	r.bottom -= px(s->margin.bottom);
	return r;
}

// The list's height for n rows, and the window's for a list of that height.
static int chrome_height(int input_h) {
	const struct styles *st = &menu.st;
	int h = box_v(&st->window) + margin_v(&st->outer) + box_v(&st->outer) +
		margin_v(&st->inner) + box_v(&st->inner);
	if (!menu.opt.hide_search) {
		h += margin_v(&st->input) + input_h;
	}
	return h;
}

static void compute_layout(int monitor_w, int monitor_h) {
	struct layout *l = &menu.layout;
	const struct styles *st = &menu.st;
	l->text_height = font_height(menu.text_font);
	l->image_size = menu.images ? px(menu.opt.image_size) : 0;
	int content = l->text_height;
	if (l->image_size) {
		int img = l->image_size + margin_v(&st->img);
		content = content > img ? content : img;
	}
	l->row_height = content + box_v(&st->entry);
	l->row_pitch = l->row_height + margin_v(&st->entry);
	int input_h = font_height(menu.input_font) + box_v(&st->input);

	// swinemenu's own defaults: 40% wide, 10 rows; wofi's options win.
	l->width = menu_size(menu.opt.width, monitor_w, monitor_w * 2 / 5);
	if (menu.opt.width && !strchr(menu.opt.width, '%')) {
		l->width = px(l->width);
	}
	int chrome = chrome_height(input_h);
	menu.lines = menu.opt.lines > 0 ? menu.opt.lines : 10;
	l->height = chrome + menu.lines * l->row_pitch;
	if (menu.opt.height && menu.opt.lines <= 0) {
		l->height = menu_size(menu.opt.height, monitor_h, l->height);
		if (!strchr(menu.opt.height, '%')) {
			l->height = px(l->height);
		}
		menu.lines = (l->height - chrome) / l->row_pitch;
		menu.lines = menu.lines > 1 ? menu.lines : 1;
	}
	if (l->width > monitor_w) l->width = monitor_w;
	if (l->height > monitor_h) l->height = monitor_h;

	RECT win = {0, 0, l->width, l->height};
	RECT outer = inside(margins(inside(win, &st->window), &st->outer),
		&st->outer);
	int y = outer.top;
	if (menu.opt.hide_search) {
		SetRectEmpty(&l->input);
	} else {
		RECT in = {outer.left, y, outer.right, y + input_h +
			margin_v(&st->input)};
		l->input = margins(in, &st->input);
		y = in.bottom;
	}
	RECT list = {outer.left, y, outer.right, outer.bottom};
	l->list = inside(margins(list, &st->inner), &st->inner);
}

// The window's place on the monitor: wofi's location and offsets.
static void place(const RECT *work, int *x, int *y) {
	int w = menu.layout.width, h = menu.layout.height;
	int left = work->left, right = work->right - w;
	int top = work->top, bottom = work->bottom - h;
	int cx = work->left + (work->right - work->left - w) / 2;
	int cy = work->top + (work->bottom - work->top - h) / 2;
	static const int columns[9] = {1, 0, 1, 2, 2, 2, 1, 0, 0};
	static const int rows[9] = {1, 0, 0, 0, 1, 2, 2, 2, 1};
	int loc = menu.opt.location;
	*x = columns[loc] == 0 ? left : columns[loc] == 1 ? cx : right;
	*y = rows[loc] == 0 ? top : rows[loc] == 1 ? cy : bottom;
	*x += px(menu.opt.xoffset);
	*y += px(menu.opt.yoffset);
}

// --- drawing ---------------------------------------------------------------

static void draw_box(struct canvas *c, RECT r, const struct swine_style *s) {
	int border = px(s->border_top.width);
	const struct css_edge *all[4] = {&s->border_top, &s->border_right,
		&s->border_bottom, &s->border_left};
	bool any_border = false;
	for (int i = 0; i < 4; ++i) {
		any_border = any_border || (all[i]->width > 0 && all[i]->color.a);
	}
	if (!s->background.a && !any_border) {
		return;
	}
	if (s->border_radius > 0 || (s->border_top.width == s->border_bottom.width &&
			s->border_top.width == s->border_left.width &&
			s->border_top.width == s->border_right.width)) {
		canvas_box(c, &r, px(s->border_radius), s->background, border,
			s->border_top.color);
		return;
	}
	// Different sides (an underline): square, each side its own.
	canvas_box(c, &r, 0, s->background, 0, s->background);
	const struct css_edge *edges[4] = {&s->border_top, &s->border_right,
		&s->border_bottom, &s->border_left};
	for (int i = 0; i < 4; ++i) {
		int w = px(edges[i]->width);
		if (!w) {
			continue;
		}
		RECT e = r;
		switch (i) {
		case 0: e.bottom = e.top + w; break;
		case 1: e.left = e.right - w; break;
		case 2: e.top = e.bottom - w; break;
		case 3: e.right = e.left + w; break;
		}
		canvas_fill(c, &e, edges[i]->color);
	}
}

static void draw_text(struct canvas *c, RECT r, const wchar_t *text,
		HFONT font, struct swine_color color, UINT align) {
	RECT clip;
	if (!IntersectRect(&clip, &r, &(RECT){0, 0, c->w, c->h})) {
		return;
	}
	canvas_mask_clear(c, &clip, false);
	HGDIOBJ old = SelectObject(c->mask_dc, font);
	SetTextColor(c->mask_dc, RGB(255, 255, 255));
	DrawTextW(c->mask_dc, text, -1, &r, DT_SINGLELINE | DT_VCENTER |
		DT_NOPREFIX | DT_END_ELLIPSIS | align);
	SelectObject(c->mask_dc, old);
	canvas_mask_blend(c, &clip, color);
}

static int text_width(HFONT font, const wchar_t *text) {
	HDC dc = CreateCompatibleDC(NULL);
	HGDIOBJ old = SelectObject(dc, font);
	SIZE size = {0, 0};
	GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size);
	SelectObject(dc, old);
	DeleteDC(dc);
	return size.cx;
}

static void draw_input(struct canvas *c) {
	const struct styles *st = &menu.st;
	const struct layout *l = &menu.layout;
	draw_box(c, l->input, &st->input);
	RECT r = inside(l->input, &st->input);
	struct swine_color color = st->input.color;
	if (!menu.query[0]) {
		// wofi's prompt is the input's placeholder
		wchar_t *prompt = swinemenu_utf8_to_utf16(menu.opt.prompt);
		struct swine_color dim = color;
		dim.a = (uint8_t)(color.a / 2);
		draw_text(c, r, prompt, menu.input_font, dim, DT_LEFT);
		free(prompt);
	}
	wchar_t *shown = display_query();
	draw_text(c, r, shown, menu.input_font, color, DT_LEFT);
	// the caret
	int x = r.left + text_width(menu.input_font, shown) + px(1);
	RECT caret = {x, r.top + px(2), x + (px(1) > 1 ? px(1) : 1), r.bottom - px(2)};
	if (caret.right <= r.right) {
		canvas_fill(c, &caret, color);
	}
	free(shown);
}

static void draw_rows(struct canvas *c) {
	const struct styles *st = &menu.st;
	const struct layout *l = &menu.layout;
	int y = l->list.top;
	for (int i = 0; i < menu.lines && menu.top + i < menu.shown->length; ++i) {
		int index = menu.top + i;
		size_t entry_index = (size_t)menu.shown->items[index];
		struct menu_entry *entry = menu.entries->items[entry_index];
		bool selected = index == menu.selected;
		const struct swine_style *es = selected ? &st->entry_selected : &st->entry;
		const struct swine_style *ts = selected ? &st->text_selected : &st->text;
		RECT row = {l->list.left, y, l->list.right, y + l->row_pitch};
		row = margins(row, es);
		if (row.bottom > l->list.bottom) {
			break;
		}
		draw_box(c, row, es);
		RECT content = inside(row, es);
		if (l->image_size) {
			RECT img = margins(content, &st->img);
			int iy = img.top + (img.bottom - img.top - l->image_size) / 2;
			const uint32_t *pixels = entry_icon(entry_index);
			if (pixels) {
				canvas_image(c, img.left, iy, l->image_size, l->image_size,
					pixels, px(st->img.border_radius));
			}
			content.left = img.left + l->image_size + px(st->img.margin.right);
		}
		wchar_t *name = swinemenu_utf8_to_utf16(entry->name);
		draw_text(c, content, name, selected ? menu.text_selected_font :
			menu.text_font, ts->color, DT_LEFT);
		free(name);
		y += l->row_pitch;
	}
}

static void render(void) {
	if (!menu.hwnd || menu.done) {
		return;
	}
	const struct layout *l = &menu.layout;
	HDC screen = GetDC(NULL);
	struct canvas c;
	bool ok = canvas_init(&c, screen, l->width, l->height);
	ReleaseDC(NULL, screen);
	if (!ok) {
		return;
	}
	RECT win = {0, 0, l->width, l->height};
	draw_box(&c, win, &menu.st.window);
	RECT outer = margins(inside(win, &menu.st.window), &menu.st.outer);
	draw_box(&c, outer, &menu.st.outer);
	if (!menu.opt.hide_search) {
		draw_input(&c);
	}
	draw_rows(&c);
	// Fully transparent pixels would let clicks through.
	for (size_t i = 0; i < (size_t)c.w * (size_t)c.h; ++i) {
		if (!(c.px[i] >> 24)) {
			c.px[i] = 0x01000000;
		}
	}
	canvas_present(&c, menu.hwnd, menu.x, menu.y);
	canvas_finish(&c);
}

// --- input -----------------------------------------------------------------

static void move_selection(int delta) {
	if (!menu.shown->length) {
		return;
	}
	menu.selected += delta;
	if (menu.selected < 0) {
		menu.selected = 0;
	}
	if (menu.selected >= menu.shown->length) {
		menu.selected = menu.shown->length - 1;
	}
	if (menu.selected < menu.top) {
		menu.top = menu.selected;
	}
	if (menu.selected >= menu.top + menu.lines) {
		menu.top = menu.selected - menu.lines + 1;
	}
	render();
}

static void delete_word(void) {
	size_t len = wcslen(menu.query);
	while (len > 0 && menu.query[len - 1] == L' ') {
		--len;
	}
	while (len > 0 && menu.query[len - 1] != L' ') {
		--len;
	}
	menu.query[len] = L'\0';
}

static int row_at(int y) {
	const struct layout *l = &menu.layout;
	if (y < l->list.top || y >= l->list.bottom) {
		return -1;
	}
	int row = menu.top + (y - l->list.top) / l->row_pitch;
	return row < menu.shown->length ? row : -1;
}

static void query_changed(void) {
	refilter();
	render();
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wparam,
		LPARAM lparam) {
	switch (msg) {
	case WM_ACTIVATE:
		// Like rofi: clicking elsewhere closes the menu.
		if (LOWORD(wparam) == WA_INACTIVE && !menu.done) {
			close_menu();
		}
		return 0;
	case WM_KEYDOWN: {
		bool ctrl = GetKeyState(VK_CONTROL) < 0;
		switch (wparam) {
		case VK_ESCAPE:
			close_menu();
			return 0;
		case VK_RETURN:
			accept();
			return 0;
		case VK_UP: move_selection(-1); return 0;
		case VK_DOWN: move_selection(1); return 0;
		case VK_TAB: move_selection(GetKeyState(VK_SHIFT) < 0 ? -1 : 1); return 0;
		case VK_PRIOR: move_selection(-menu.lines); return 0;
		case VK_NEXT: move_selection(menu.lines); return 0;
		case VK_HOME: if (ctrl) { move_selection(-menu.selected); } return 0;
		case VK_END: if (ctrl) { move_selection(menu.shown->length); } return 0;
		case 'N': if (ctrl) { move_selection(1); } return 0;
		case 'P': if (ctrl) { move_selection(-1); } return 0;
		case 'J': if (ctrl) { move_selection(1); } return 0;
		case 'K': if (ctrl) { move_selection(-1); } return 0;
		case 'U':
			if (ctrl) {
				menu.query[0] = L'\0';
				query_changed();
			}
			return 0;
		case 'W':
			if (ctrl) {
				delete_word();
				query_changed();
			}
			return 0;
		}
		break;
	}
	case WM_CHAR: {
		wchar_t c = (wchar_t)wparam;
		size_t len = wcslen(menu.query);
		if (c == L'\b') {
			if (GetKeyState(VK_CONTROL) < 0) {
				delete_word();
			} else if (len > 0) {
				menu.query[len - 1] = L'\0';
			}
		} else if (c >= L' ' && c != 0x7f && len + 1 < 512) {
			menu.query[len] = c;
			menu.query[len + 1] = L'\0';
		} else {
			return 0;
		}
		query_changed();
		return 0;
	}
	case WM_MOUSEWHEEL:
		move_selection(GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -3 : 3);
		return 0;
	case WM_LBUTTONDOWN: {
		int row = row_at(GET_Y_LPARAM(lparam));
		if (row >= 0) {
			menu.selected = row;
			accept();
		}
		return 0;
	}
	case WM_DESTROY:
		menu.done = true;
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// --- start-up --------------------------------------------------------------

static const char usage[] =
	"Usage: swinemenu [options...]\n"
	"\n"
	"  -S, --show <mode>     drun (apps, the default), run (programs on\n"
	"                        PATH), dmenu (choices from standard input) or\n"
	"                        window (swinewm's windows)\n"
	"  -d, --dmenu           Same as --show dmenu: print the chosen line\n"
	"  -p, --prompt <text>   The input's placeholder\n"
	"  -L, --lines <n>       Number of rows (default 10)\n"
	"  -W, --width <w>       Width in pixels, or n% of the screen (40%)\n"
	"  -H, --height <h>      Height in pixels or n%, instead of --lines\n"
	"  -l, --location <loc>  center, top_left, top, top_right, right,\n"
	"                        bottom_right, bottom, bottom_left, left (0-8)\n"
	"  -x, -y <px>           Offset from the location\n"
	"  -I, --allow-images    Icons (the default in drun and window)\n"
	"  -P, --password [c]    Hide the input behind c (default *)\n"
	"  -e, --exec-search     Enter runs what was typed\n"
	"  -Q, --search <text>   Start with this typed in\n"
	"  -c, --conf <file>     wofi config file (default wofi\\config)\n"
	"  -s, --style <file>    wofi style sheet (default wofi\\style.css)\n"
	"  -D, --define key=val  Set a config option\n"
	"  --open-with <file>    Pick a program to open file with\n"
	"  --install-open-with   Add \"Open with swinemenu\" to every file's menu\n"
	"                        and use it for files no program is set for\n"
	"  --uninstall-open-with Undo --install-open-with\n"
	"  -v, --version         Show the version number and quit\n"
	"\n"
	"Other wofi options are accepted and change nothing.\n";

// Brings our window to the front. Windows only lets the process that
// received the last input take the foreground; a zero-length synthetic
// mouse move makes us that process (as swinewm itself does).
static void take_foreground(HWND hwnd) {
	if (!SetForegroundWindow(hwnd)) {
		INPUT input = { .type = INPUT_MOUSE };
		input.mi.dwFlags = MOUSEEVENTF_MOVE;
		SendInput(1, &input, sizeof(input));
		SetForegroundWindow(hwnd);
	}
	SetFocus(hwnd);
}

// One "key=value" from -D, or an option with its value.
static bool set_option(const char *key, const char *value) {
	char *error = NULL;
	if (strcmp(key, "define") == 0 && value) {
		char *copy = swine_strdup(value);
		char *eq = strchr(copy, '=');
		bool ok = eq && (*eq = '\0', menu_option_set(&menu.opt, copy, eq + 1,
			&error));
		free(copy);
		if (!ok) {
			char *msg = format_str("swinemenu: %s", error ? error :
				"-D expects key=value");
			complain(msg);
			free(msg);
		}
		free(error);
		return ok;
	}
	if (!menu_option_set(&menu.opt, key, value, &error)) {
		char *msg = format_str("swinemenu: %s", error);
		complain(msg);
		free(msg);
		free(error);
		return false;
	}
	return true;
}

// The command line as (option, value) pairs: they are applied after the
// config file, which -c may name.
struct cli_option {
	const char *name;
	char *value;
};

static int parse_command_line(int argc, char **argv, struct cli_option *out,
		int max, bool *version, bool *help) {
	int n = 0;
	for (int i = 1; i < argc && n < max; ++i) {
		const char *a = argv[i];
		bool takes_value = false;
		const char *eq = has_prefix(a, "--") ? strchr(a, '=') : NULL;
		char *flag = eq ? swine_strndup(a, (size_t)(eq - a)) : swine_strdup(a);
		const char *name = menu_option_name(flag, &takes_value);
		free(flag);
		if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
			*version = true;
		} else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			*help = true;
		} else if (strcmp(a, "--install-open-with") == 0 ||
				strcmp(a, "--uninstall-open-with") == 0) {
			out[n].name = a + 2;
			out[n++].value = NULL;
		} else if (!name) {
			*help = true;
		} else if (eq) {
			out[n].name = name;
			out[n++].value = swine_strdup(eq + 1);
		} else if (takes_value && i + 1 < argc) {
			out[n].name = name;
			out[n++].value = swine_strdup(argv[++i]);
		} else if (takes_value) {
			*help = true;
		} else if (strcmp(name, "password") == 0 && i + 1 < argc &&
				argv[i + 1][0] != '-' && strlen(argv[i + 1]) <= 4) {
			out[n].name = name;   // -P [character]
			out[n++].value = swine_strdup(argv[++i]);
		} else {
			out[n].name = name;
			out[n++].value = NULL;
		}
	}
	return n;
}

static UINT monitor_dpi(HMONITOR monitor) {
	UINT dpi = 96;
	HMODULE shcore = LoadLibraryW(L"shcore.dll");
	if (shcore) {
		typedef HRESULT (WINAPI *get_dpi_fn)(HMONITOR, int, UINT *, UINT *);
		get_dpi_fn get_dpi = (get_dpi_fn)(void (*)(void))GetProcAddress(shcore,
			"GetDpiForMonitor");
		UINT dpi_y;
		if (get_dpi) {
			get_dpi(monitor, 0, &dpi, &dpi_y);
		}
	}
	return dpi;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR cmd, int show) {
	(void)prev;
	(void)cmd;
	(void)show;
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

	menu_options_init(&menu.opt);
	int argc = 0;
	char **argv = ipc_utf8_argv(&argc);
	struct cli_option cli[64];
	bool version = false, help = false;
	int ncli = parse_command_line(argc, argv, cli, 64, &version, &help);
	if (version) {
		write_stdout("swinemenu version " SWINEWM_VERSION);
		return 0;
	}
	if (help) {
		write_stdout(usage);
		return 2;
	}
	for (int i = 0; i < ncli; ++i) {
		if (strcmp(cli[i].name, "install-open-with") == 0 ||
				strcmp(cli[i].name, "uninstall-open-with") == 0) {
			return swinemenu_install_open_with(cli[i].name[0] == 'i');
		}
		if (strcmp(cli[i].name, "conf") == 0) {
			set_option("conf", cli[i].value);
		}
	}
	// wofi's config first, then the command line over it.
	char *conf = menu.opt.conf ? swine_strdup(menu.opt.conf) :
		find_wofi_file("config");
	char *conf_text = conf ? read_text(conf) : NULL;
	if (conf_text) {
		char *error = NULL;
		if (!menu_options_parse_config(&menu.opt, conf_text, &error)) {
			char *msg = format_str("swinemenu: %s: %s", conf, error);
			complain(msg);
			free(msg);
			free(error);
		}
	} else if (menu.opt.conf) {
		char *msg = format_str("swinemenu: unable to read %s", conf);
		complain(msg);
		free(msg);
	}
	free(conf_text);
	free(conf);
	for (int i = 0; i < ncli; ++i) {
		set_option(cli[i].name, cli[i].value);
		free(cli[i].value);
	}
	if (!menu.opt.prompt) {
		enum menu_mode m = menu.opt.mode;
		menu.opt.prompt = swine_strdup(m == MENU_MODE_RUN ? "run" :
			m == MENU_MODE_DMENU ? "dmenu" : m == MENU_MODE_WINDOW ? "window" :
			m == MENU_MODE_OPENWITH ? "open with" : "drun");
	}
	if (menu.opt.search) {
		wchar_t *search = swinemenu_utf8_to_utf16(menu.opt.search);
		wcsncpy(menu.query, search, 511);
		menu.query[511] = L'\0';
		free(search);
	}
	menu.images = menu.opt.allow_images >= 0 ? menu.opt.allow_images != 0 :
		(menu.opt.mode == MENU_MODE_DRUN || menu.opt.mode == MENU_MODE_WINDOW);

	// A second "swinemenu" closes the open one, so the key toggles it.
	HWND open = FindWindowW(window_class, NULL);
	if (open) {
		PostMessageW(open, WM_CLOSE, 0, 0);
		return 1;
	}

	switch (menu.opt.mode) {
	case MENU_MODE_RUN: menu.entries = swinemenu_run_entries(); break;
	case MENU_MODE_DMENU: menu.entries = swinemenu_dmenu_entries(); break;
	case MENU_MODE_WINDOW: menu.entries = swinemenu_window_entries(); break;
	case MENU_MODE_OPENWITH: {
		wchar_t *file = swinemenu_utf8_to_utf16(menu.opt.open_with);
		menu.entries = swinemenu_openwith_entries(file);
		free(file);
		break;
	}
	default: menu.entries = swinemenu_drun_entries(); break;
	}
	menu.icons = calloc((size_t)menu.entries->length + 1, sizeof(*menu.icons));
	if (menu.opt.mode != MENU_MODE_DMENU && menu.opt.mode != MENU_MODE_WINDOW) {
		menu.history_path = history_path();
		menu_history_load(menu.entries, menu.history_path);
	}
	refilter();
	load_style();

	// On the monitor of the window that had the focus (swinewm's focused
	// output).
	HWND foreground = GetForegroundWindow();
	POINT origin = {0, 0};
	HMONITOR monitor = foreground ?
		MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY) :
		MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO mi = { .cbSize = sizeof(mi) };
	GetMonitorInfoW(monitor, &mi);
	menu.scale = monitor_dpi(monitor) / 96.0;
	menu.input_font = style_font(&menu.st.input);
	menu.text_font = style_font(&menu.st.text);
	menu.text_selected_font = style_font(&menu.st.text_selected);
	compute_layout(mi.rcWork.right - mi.rcWork.left,
		mi.rcWork.bottom - mi.rcWork.top);
	place(&mi.rcWork, &menu.x, &menu.y);

	WNDCLASSEXW wc = { .cbSize = sizeof(wc) };
	wc.lpfnWndProc = wndproc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	wc.lpszClassName = window_class;
	RegisterClassExW(&wc);
	// A tool window, so that swinewm does not tile it; layered, so that
	// rounded and translucent wofi themes show what is behind.
	menu.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
		window_class, L"swinemenu", WS_POPUP, menu.x, menu.y,
		menu.layout.width, menu.layout.height, NULL, NULL, instance, NULL);
	render();
	ShowWindow(menu.hwnd, SW_SHOW);
	take_foreground(menu.hwnd);

	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	DeleteObject(menu.input_font);
	DeleteObject(menu.text_font);
	DeleteObject(menu.text_selected_font);
	for (int i = 0; i < menu.entries->length; ++i) {
		free(menu.icons[i].px);
		menu_entry_free(menu.entries->items[i]);
	}
	free(menu.icons);
	list_free(menu.entries);
	list_free(menu.shown);
	free(menu.history_path);
	menu_options_finish(&menu.opt);
	CoUninitialize();
	return exit_status;
}
