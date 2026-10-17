// swinemenu: a small application launcher in the spirit of wofi/wmenu, the
// default "$menu" of swinewm's config. It only picks: the chosen entry is
// handed to swinewm as an "exec" command over IPC, so swinewm is the one
// place that knows how to start things on Windows (Store apps included).
// Colours and font come from swinewm's bar (get_bar_config), so the menu
// matches the Waybar style. Other launchers can replace it in the config.
//
//   swinemenu [--show drun|run|dmenu] [--dmenu] [--prompt text]
//             [--lines n] [--width px]
//
// drun lists the Start menu's apps and Store apps, run lists programs on
// PATH, dmenu reads choices from standard input and prints the chosen one.
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
#include "ipc.h"
#include "ipc-client.h"
#include "json.h"
#include "menu.h"
#include "stringop.h"
#include "swinemenu.h"
#include "swinewm_version.h"

enum mode { MODE_DRUN, MODE_RUN, MODE_DMENU };

struct theme {
	COLORREF background, text, selected_bg, selected_text, border;
	wchar_t font[64];
	double font_pt;
};

static struct {
	enum mode mode;
	wchar_t prompt[64];
	int lines;
	int width;               // logical px; 0 = 40% of the monitor
	list_t *entries;         // struct menu_entry *
	list_t *shown;           // indexes into entries (menu_filter)
	wchar_t query[512];
	int selected;            // index into shown
	int top;                 // first visible row
	struct theme theme;
	HFONT font;
	int row_height;
	int padding;
	HWND hwnd;
	bool done;
	char *history_path;
} menu;

static const wchar_t window_class[] = L"swinemenu";

// --- theme ---------------------------------------------------------------

static COLORREF parse_hex(const char *text, COLORREF fallback) {
	unsigned r, g, b;
	if (text && text[0] == '#' && sscanf(text + 1, "%2x%2x%2x", &r, &g, &b) == 3) {
		return RGB(r, g, b);
	}
	return fallback;
}

// sway's colours for its default bar, used when swinewm cannot be asked.
static void load_theme(void) {
	struct theme *t = &menu.theme;
	t->background = RGB(0x32, 0x32, 0x32);
	t->text = RGB(0xff, 0xff, 0xff);
	t->selected_bg = RGB(0x28, 0x55, 0x77);
	t->selected_text = RGB(0xff, 0xff, 0xff);
	t->border = RGB(0x4c, 0x78, 0x99);
	wcscpy(t->font, L"Segoe UI");
	t->font_pt = 11;

	char *reply = ipc_request(NULL, IPC_GET_BAR_CONFIG, "bar-0", NULL);
	struct json_value *bar = reply ? json_parse(reply, NULL) : NULL;
	struct json_value *colors = json_object_get(bar, "colors");
	if (colors) {
		t->background = parse_hex(json_get_string(colors, "background", NULL),
			t->background);
		t->text = parse_hex(json_get_string(colors, "statusline", NULL), t->text);
		t->selected_bg = parse_hex(json_get_string(colors,
			"focused_workspace_bg", NULL), t->selected_bg);
		t->selected_text = parse_hex(json_get_string(colors,
			"focused_workspace_text", NULL), t->selected_text);
		t->border = parse_hex(json_get_string(colors,
			"focused_workspace_border", NULL), t->border);
	}
	// "pango:Family, Fallback 12": the first family and the size in points.
	const char *font = json_get_string(bar, "font", NULL);
	if (font) {
		if (has_prefix(font, "pango:")) {
			font += 6;
		}
		const char *size = strrchr(font, ' ');
		if (size && atof(size + 1) > 0) {
			t->font_pt = atof(size + 1);
		}
		size_t family_len = strcspn(font, ",");
		if (size && (size_t)(size - font) < family_len) {
			family_len = (size_t)(size - font);
		}
		char *family = swine_strndup(font, family_len);
		const char *mapped = family;
		if (swine_strcasecmp(family, "sans-serif") == 0 ||
				swine_strcasecmp(family, "sans") == 0) {
			mapped = "Segoe UI";
		} else if (swine_strcasecmp(family, "monospace") == 0) {
			mapped = "Consolas";
		} else if (swine_strcasecmp(family, "serif") == 0) {
			mapped = "Times New Roman";
		}
		if (*mapped) {
			wchar_t *w = swinemenu_utf8_to_utf16(mapped);
			wcsncpy(t->font, w, 63);
			t->font[63] = L'\0';
			free(w);
		}
		free(family);
	}
	json_value_free(bar);
	free(reply);
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
	char *path = format_str("%s\\swinemenu-%s.txt", dir,
		menu.mode == MODE_RUN ? "run" : "drun");
	free(dir);
	free(base8);
	return path;
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

static void write_stdout(const char *text) {
	HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	if (out && out != INVALID_HANDLE_VALUE) {
		DWORD n;
		WriteFile(out, text, (DWORD)strlen(text), &n, NULL);
		WriteFile(out, "\n", 1, &n, NULL);
	}
}

static int exit_status = 1;   // 0 once something was chosen

static void accept(void) {
	struct menu_entry *entry = selected_entry();
	char *typed = swinemenu_utf16_to_utf8(menu.query);
	if (menu.mode == MODE_DMENU) {
		// dmenu prints the typed text when nothing matches.
		write_stdout(entry ? entry->name : typed);
		exit_status = 0;
	} else if (entry) {
		menu_history_record(menu.history_path, entry->name);
		launch(entry->command ? entry->command : entry->name);
		exit_status = 0;
	} else if (*typed) {
		launch(typed);   // a command line typed by hand
		exit_status = 0;
	}
	free(typed);
	menu.done = true;
	DestroyWindow(menu.hwnd);
}

// --- drawing ---------------------------------------------------------------

static int visible_rows(void) {
	return menu.lines;
}

static void paint(HWND hwnd, HDC target) {
	RECT client;
	GetClientRect(hwnd, &client);
	int w = client.right, h = client.bottom;
	HDC dc = CreateCompatibleDC(target);
	HBITMAP bitmap = CreateCompatibleBitmap(target, w, h);
	HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
	HGDIOBJ old_font = SelectObject(dc, menu.font);
	SetBkMode(dc, TRANSPARENT);

	HBRUSH bg = CreateSolidBrush(menu.theme.background);
	FillRect(dc, &client, bg);
	DeleteObject(bg);
	// Border in the focused workspace's accent colour.
	int border = menu.padding / 4 > 1 ? menu.padding / 4 : 2;
	HBRUSH accent = CreateSolidBrush(menu.theme.border);
	RECT edge = client;
	for (int i = 0; i < border; ++i) {
		FrameRect(dc, &edge, accent);
		InflateRect(&edge, -1, -1);
	}

	// Input line: prompt, query, caret; underlined.
	int x = menu.padding, y = menu.padding;
	RECT line = {x, y, w - menu.padding, y + menu.row_height};
	SetTextColor(dc, menu.theme.border);
	RECT measure = line;
	DrawTextW(dc, menu.prompt, -1, &measure, DT_SINGLELINE | DT_VCENTER |
		DT_CALCRECT | DT_NOPREFIX);
	DrawTextW(dc, menu.prompt, -1, &line, DT_SINGLELINE | DT_VCENTER |
		DT_NOPREFIX);
	line.left = measure.right + menu.padding / 2;
	SetTextColor(dc, menu.theme.text);
	wchar_t shown[520];
	swprintf(shown, 520, L"%ls│", menu.query);
	DrawTextW(dc, shown, -1, &line, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX |
		DT_END_ELLIPSIS);
	RECT underline = {menu.padding, y + menu.row_height,
		w - menu.padding, y + menu.row_height + border};
	FillRect(dc, &underline, accent);
	DeleteObject(accent);

	// Rows.
	y += menu.row_height + border + menu.padding / 2;
	for (int i = 0; i < visible_rows() && menu.top + i < menu.shown->length;
			++i) {
		int index = menu.top + i;
		size_t entry_index = (size_t)menu.shown->items[index];
		struct menu_entry *entry = menu.entries->items[entry_index];
		RECT row = {menu.padding, y, w - menu.padding, y + menu.row_height};
		if (index == menu.selected) {
			HBRUSH sel = CreateSolidBrush(menu.theme.selected_bg);
			FillRect(dc, &row, sel);
			DeleteObject(sel);
			SetTextColor(dc, menu.theme.selected_text);
		} else {
			SetTextColor(dc, menu.theme.text);
		}
		row.left += menu.padding / 2;
		wchar_t *name = swinemenu_utf8_to_utf16(entry->name);
		DrawTextW(dc, name, -1, &row, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX |
			DT_END_ELLIPSIS);
		free(name);
		y += menu.row_height;
	}

	BitBlt(target, 0, 0, w, h, dc, 0, 0, SRCCOPY);
	SelectObject(dc, old_font);
	SelectObject(dc, old_bitmap);
	DeleteObject(bitmap);
	DeleteDC(dc);
}

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
	if (menu.selected >= menu.top + visible_rows()) {
		menu.top = menu.selected - visible_rows() + 1;
	}
	InvalidateRect(menu.hwnd, NULL, FALSE);
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
	int border = menu.padding / 4 > 1 ? menu.padding / 4 : 2;
	int first = menu.padding + menu.row_height + border + menu.padding / 2;
	if (y < first) {
		return -1;
	}
	int row = menu.top + (y - first) / menu.row_height;
	return row < menu.shown->length ? row : -1;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wparam,
		LPARAM lparam) {
	switch (msg) {
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		paint(hwnd, dc);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_ERASEBKGND:
		return 1;
	case WM_ACTIVATE:
		// Like rofi: clicking elsewhere closes the menu.
		if (LOWORD(wparam) == WA_INACTIVE && !menu.done) {
			menu.done = true;
			DestroyWindow(hwnd);
		}
		return 0;
	case WM_KEYDOWN: {
		bool ctrl = GetKeyState(VK_CONTROL) < 0;
		switch (wparam) {
		case VK_ESCAPE:
			menu.done = true;
			DestroyWindow(hwnd);
			return 0;
		case VK_RETURN:
			accept();
			return 0;
		case VK_UP: move_selection(-1); return 0;
		case VK_DOWN: move_selection(1); return 0;
		case VK_TAB: move_selection(GetKeyState(VK_SHIFT) < 0 ? -1 : 1); return 0;
		case VK_PRIOR: move_selection(-visible_rows()); return 0;
		case VK_NEXT: move_selection(visible_rows()); return 0;
		case VK_HOME: if (ctrl) { move_selection(-menu.selected); } return 0;
		case VK_END: if (ctrl) { move_selection(menu.shown->length); } return 0;
		case 'N': if (ctrl) { move_selection(1); } return 0;
		case 'P': if (ctrl) { move_selection(-1); } return 0;
		case 'J': if (ctrl) { move_selection(1); } return 0;
		case 'K': if (ctrl) { move_selection(-1); } return 0;
		case 'U':
			if (ctrl) {
				menu.query[0] = L'\0';
				refilter();
				InvalidateRect(hwnd, NULL, FALSE);
			}
			return 0;
		case 'W':
			if (ctrl) {
				delete_word();
				refilter();
				InvalidateRect(hwnd, NULL, FALSE);
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
		refilter();
		InvalidateRect(hwnd, NULL, FALSE);
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
	"  -S, --show <mode>   drun (apps, the default), run (programs on PATH)\n"
	"                      or dmenu (choices from standard input)\n"
	"  -d, --dmenu         Same as --show dmenu: print the chosen line\n"
	"  -p, --prompt <text> Text in front of the input\n"
	"  -L, --lines <n>     Number of rows (default 10)\n"
	"  -W, --width <px>    Width (default 40% of the screen)\n"
	"  -v, --version       Show the version number and quit\n";

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

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR cmd, int show) {
	(void)prev;
	(void)cmd;
	(void)show;
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

	menu.mode = MODE_DRUN;
	menu.lines = 10;
	wcscpy(menu.prompt, L"drun");
	bool prompt_given = false;
	int argc = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	for (int i = 1; i < argc; ++i) {
		const wchar_t *a = argv[i];
		const wchar_t *next = i + 1 < argc ? argv[i + 1] : NULL;
		if ((!wcscmp(a, L"-S") || !wcscmp(a, L"--show")) && next) {
			++i;
			menu.mode = !wcscmp(next, L"run") ? MODE_RUN :
				!wcscmp(next, L"dmenu") ? MODE_DMENU : MODE_DRUN;
		} else if (!wcscmp(a, L"-d") || !wcscmp(a, L"--dmenu")) {
			menu.mode = MODE_DMENU;
		} else if ((!wcscmp(a, L"-p") || !wcscmp(a, L"--prompt")) && next) {
			++i;
			wcsncpy(menu.prompt, next, 63);
			menu.prompt[63] = L'\0';
			prompt_given = true;
		} else if ((!wcscmp(a, L"-L") || !wcscmp(a, L"--lines")) && next) {
			++i;
			menu.lines = _wtoi(next) > 0 ? _wtoi(next) : 10;
		} else if ((!wcscmp(a, L"-W") || !wcscmp(a, L"--width")) && next) {
			++i;
			menu.width = _wtoi(next);
		} else if (!wcscmp(a, L"-v") || !wcscmp(a, L"--version")) {
			write_stdout("swinemenu version " SWINEWM_VERSION);
			return 0;
		} else if (!wcscmp(a, L"-I") || !wcscmp(a, L"-i") ||
				!wcscmp(a, L"--insensitive") || !wcscmp(a, L"--allow-images") ||
				!wcscmp(a, L"--no-actions")) {
			// wofi options that are the default here (or not supported).
		} else {
			write_stdout(usage);
			return 2;
		}
	}
	LocalFree(argv);
	if (!prompt_given) {
		wcscpy(menu.prompt, menu.mode == MODE_RUN ? L"run" :
			menu.mode == MODE_DMENU ? L">" : L"drun");
	}

	// A second "swinemenu" closes the open one, so the key toggles it.
	HWND open = FindWindowW(window_class, NULL);
	if (open) {
		PostMessageW(open, WM_CLOSE, 0, 0);
		return 1;
	}

	menu.entries = menu.mode == MODE_RUN ? swinemenu_run_entries() :
		menu.mode == MODE_DMENU ? swinemenu_dmenu_entries() :
		swinemenu_drun_entries();
	if (menu.mode != MODE_DMENU) {
		menu.history_path = history_path();
		menu_history_load(menu.entries, menu.history_path);
	}
	refilter();
	load_theme();

	// On the monitor of the window that had the focus (swinewm's focused
	// output).
	HWND foreground = GetForegroundWindow();
	POINT origin = {0, 0};
	HMONITOR monitor = foreground ?
		MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY) :
		MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO mi = { .cbSize = sizeof(mi) };
	GetMonitorInfoW(monitor, &mi);
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
	int font_px = (int)(menu.theme.font_pt * dpi / 72.0 + 0.5);
	menu.font = CreateFontW(-font_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH, menu.theme.font);
	menu.row_height = font_px * 2;
	menu.padding = font_px;

	int monitor_w = mi.rcWork.right - mi.rcWork.left;
	int monitor_h = mi.rcWork.bottom - mi.rcWork.top;
	int width = menu.width > 0 ? (int)(menu.width * dpi / 96) : monitor_w * 2 / 5;
	if (width > monitor_w) {
		width = monitor_w;
	}
	int border = menu.padding / 4 > 1 ? menu.padding / 4 : 2;
	int height = menu.padding * 2 + menu.row_height + border +
		menu.padding / 2 + menu.row_height * menu.lines;
	if (height > monitor_h) {
		height = monitor_h;
	}
	int x = mi.rcWork.left + (monitor_w - width) / 2;
	int y = mi.rcWork.top + (monitor_h - height) / 3;

	WNDCLASSEXW wc = { .cbSize = sizeof(wc) };
	wc.lpfnWndProc = wndproc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	wc.lpszClassName = window_class;
	RegisterClassExW(&wc);
	// A tool window, so that swinewm does not tile it.
	menu.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, window_class,
		L"swinemenu", WS_POPUP, x, y, width, height, NULL, NULL, instance, NULL);
	ShowWindow(menu.hwnd, SW_SHOW);
	take_foreground(menu.hwnd);

	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	DeleteObject(menu.font);
	for (int i = 0; i < menu.entries->length; ++i) {
		menu_entry_free(menu.entries->items[i]);
	}
	list_free(menu.entries);
	list_free(menu.shown);
	free(menu.history_path);
	CoUninitialize();
	return exit_status;
}
