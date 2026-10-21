// Tabs of tabbed and stacked containers (tree/titlebar.h), drawn like
// sway's title bars: the client.* border, background and text colours,
// titlebar_border_thickness, titlebar_padding, title_align and font.
//
// One small window of swinewm's own per visible tabbed or stacked
// container. It never takes the focus; clicks and the wheel on it reach the
// core through the mouse hook as title bar input (win32_titlebar_hit), so
// tabs are switched by cursor.c exactly as in sway. The windows sit just
// above the desktop: nothing tiled overlaps them, and floating windows stay
// in front.
#include <stdlib.h>
#include <string.h>
#include "list.h"
#include "stringop.h"
#include "swinewm/config.h"
#include "swinewm/platform.h"
#include "swinewm/tree/titlebar.h"
#include "win32.h"

struct tab_copy {
	struct swine_container *con;
	wchar_t *title;
	wchar_t *marks;   // drawn on the right, as sway does, or NULL
	struct border_colors colors;
};

struct tab_window {
	HWND hwnd;
	struct swine_box box;
	bool stacked;
	int tab_height;
	double scale;
	int count;
	struct tab_copy *tabs;
};

static const wchar_t tab_class[] = L"SwineWMTitlebar";
static list_t *windows = NULL;   // struct tab_window *
static bool class_registered = false;

static COLORREF rgb(uint32_t rgba) {
	return RGB((rgba >> 24) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF);
}

static struct tab_window *find_window(HWND hwnd) {
	for (int i = 0; windows && i < windows->length; ++i) {
		struct tab_window *w = windows->items[i];
		if (w->hwnd == hwnd) {
			return w;
		}
	}
	return NULL;
}

static RECT tab_rect(const struct tab_window *w, int i) {
	RECT r;
	if (w->stacked) {
		r.left = 0;
		r.right = w->box.width;
		r.top = i * w->tab_height;
		r.bottom = r.top + w->tab_height;
	} else {
		r.left = (int)((long long)w->box.width * i / w->count);
		r.right = (int)((long long)w->box.width * (i + 1) / w->count);
		r.top = 0;
		r.bottom = w->box.height;
	}
	return r;
}

static void paint(struct tab_window *w, HDC dc) {
	int border = (int)(config->titlebar_border_thickness * w->scale + 0.5);
	int padding = (int)(config->titlebar_h_padding * w->scale + 0.5);
	HGDIOBJ old_font = SelectObject(dc, win32_sway_font(config->font, w->scale));
	SetBkMode(dc, TRANSPARENT);
	UINT align = config->title_align == 1 ? DT_CENTER :
		config->title_align == 2 ? DT_RIGHT : DT_LEFT;
	for (int i = 0; i < w->count; ++i) {
		const struct tab_copy *tab = &w->tabs[i];
		RECT r = tab_rect(w, i);
		win32_fill(dc, &r, rgb(tab->colors.border));
		RECT inner = {r.left + border, r.top + border, r.right - border,
			r.bottom - border};
		win32_fill(dc, &inner, rgb(tab->colors.background));
		RECT text = {inner.left + padding, inner.top, inner.right - padding,
			inner.bottom};
		SetTextColor(dc, rgb(tab->colors.text));
		if (tab->marks) {
			RECT marks = text;
			DrawTextW(dc, tab->marks, -1, &marks, DT_SINGLELINE | DT_NOPREFIX |
				DT_CALCRECT);
			int width = marks.right - marks.left;
			marks = text;
			marks.left = text.right - width > text.left ? text.right - width :
				text.left;
			DrawTextW(dc, tab->marks, -1, &marks, DT_RIGHT | DT_SINGLELINE |
				DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
			text.right = marks.left - padding > text.left ?
				marks.left - padding : text.left;
		}
		DrawTextW(dc, tab->title, -1, &text, align | DT_SINGLELINE |
			DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
	}
	SelectObject(dc, old_font);
}

static LRESULT CALLBACK tab_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
		LPARAM lparam) {
	switch (msg) {
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		struct tab_window *w = find_window(hwnd);
		if (w && w->count > 0) {
			// Draw off screen, then copy, so tabs never flicker.
			RECT client;
			GetClientRect(hwnd, &client);
			HDC mem = CreateCompatibleDC(dc);
			HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
			HGDIOBJ old = SelectObject(mem, bmp);
			paint(w, mem);
			BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
			SelectObject(mem, old);
			DeleteObject(bmp);
			DeleteDC(mem);
		}
		EndPaint(hwnd, &ps);
		return 0;
	}
	default:
		return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
}

static void free_tabs(struct tab_window *w) {
	for (int i = 0; i < w->count; ++i) {
		free(w->tabs[i].title);
		free(w->tabs[i].marks);
	}
	free(w->tabs);
	w->tabs = NULL;
	w->count = 0;
}

static struct tab_window *create_tab_window(void) {
	HINSTANCE instance = GetModuleHandleW(NULL);
	if (!class_registered) {
		WNDCLASSEXW wc = { .cbSize = sizeof(wc) };
		wc.lpfnWndProc = tab_wndproc;
		wc.hInstance = instance;
		wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
		wc.lpszClassName = tab_class;
		RegisterClassExW(&wc);
		class_registered = true;
	}
	HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, tab_class,
		L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, instance, NULL);
	if (!hwnd) {
		return NULL;
	}
	struct tab_window *w = calloc(1, sizeof(*w));
	w->hwnd = hwnd;
	return w;
}

// Just above the desktop: below every window, but visible.
static HWND insert_after(void) {
	HWND desktop = win32_shell_desktop_window();
	if (desktop) {
		HWND above = GetWindow(desktop, GW_HWNDPREV);
		return above ? above : HWND_TOP;
	}
	return HWND_BOTTOM;
}

void platform_titlebars_update(list_t *titlebars) {
	if (!windows) {
		windows = create_list();
	}
	for (int i = 0; i < titlebars->length; ++i) {
		const struct swine_titlebar *bar = titlebars->items[i];
		struct tab_window *w = NULL;
		if (i < windows->length) {
			w = windows->items[i];
		} else {
			w = create_tab_window();
			if (!w) {
				return;
			}
			list_add(windows, w);
		}
		free_tabs(w);
		w->box = bar->box;
		w->stacked = bar->stacked;
		w->tab_height = bar->tab_height;
		w->scale = bar->scale;
		w->count = bar->tabs->length;
		w->tabs = calloc((size_t)(w->count ? w->count : 1), sizeof(*w->tabs));
		for (int j = 0; j < w->count; ++j) {
			const struct swine_tab *tab = bar->tabs->items[j];
			w->tabs[j].con = tab->con;
			w->tabs[j].title = utf8_to_utf16(tab->title);
			w->tabs[j].marks = tab->marks ? utf8_to_utf16(tab->marks) : NULL;
			w->tabs[j].colors = tab->colors;
		}
		SetWindowPos(w->hwnd, insert_after(), w->box.x, w->box.y, w->box.width,
			w->box.height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
		InvalidateRect(w->hwnd, NULL, FALSE);
	}
	for (int i = titlebars->length; i < windows->length; ++i) {
		struct tab_window *w = windows->items[i];
		if (w->count) {
			free_tabs(w);
			ShowWindow(w->hwnd, SW_HIDE);
		}
	}
}

bool win32_titlebar_hit(HWND hwnd, POINT pt, struct swine_container **con) {
	struct tab_window *w = find_window(hwnd);
	if (!w || w->count == 0) {
		return false;
	}
	int x = pt.x - w->box.x, y = pt.y - w->box.y;
	*con = NULL;
	for (int i = 0; i < w->count; ++i) {
		RECT r = tab_rect(w, i);
		if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
			*con = w->tabs[i].con;
			break;
		}
	}
	return true;
}

void win32_titlebars_finish(void) {
	for (int i = 0; windows && i < windows->length; ++i) {
		struct tab_window *w = windows->items[i];
		free_tabs(w);
		DestroyWindow(w->hwnd);
		free(w);
	}
	if (windows) {
		list_free(windows);
		windows = NULL;
	}
}
