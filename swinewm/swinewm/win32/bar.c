// Draws the integrated bar: one topmost tool window per output, painted
// with GDI from the model built by swinewm/bar/bar.c.
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "list.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/bar/bar.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "win32.h"

struct hit {
	RECT rect;
	int module_index;
	uint64_t tray_id;
	char *on_click;
	char *on_scroll_up;
	char *on_scroll_down;
};

struct bar_window {
	HWND hwnd;
	char *output_name;
	struct swine_box rect;
	bool shown;
	struct bar_model *model;
	double scale;
	list_t *hits;   // struct hit *, rebuilt on every paint
};

static list_t *bars = NULL;
static bool class_registered = false;

static void clear_hits(struct bar_window *bw) {
	for (int i = 0; i < bw->hits->length; ++i) {
		struct hit *h = bw->hits->items[i];
		free(h->on_click);
		free(h->on_scroll_up);
		free(h->on_scroll_down);
		free(h);
	}
	bw->hits->length = 0;
}

static void destroy_bar(struct bar_window *bw) {
	DestroyWindow(bw->hwnd);
	bar_model_free(bw->model);
	clear_hits(bw);
	list_free(bw->hits);
	free(bw->output_name);
	free(bw);
}

static struct bar_window *bar_from_hwnd(HWND hwnd) {
	return (struct bar_window *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static int px(struct bar_window *bw, double v) {
	return (int)(v * bw->scale + 0.5);
}

static HFONT style_font(struct bar_window *bw, const struct swine_style *s) {
	return win32_font(s->font_family, px(bw, s->font_size), s->bold, s->italic);
}

static int text_width(HDC dc, HFONT font, const wchar_t *text) {
	SIZE size = {0};
	HFONT old = SelectObject(dc, font);
	GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size);
	SelectObject(dc, old);
	return size.cx;
}

// Width of an item including its padding, border and margin.
static int item_outer_width(struct bar_window *bw, HDC dc,
		const struct bar_item *item, int *inner) {
	const struct swine_style *s = &item->style;
	int content;
	if (item->tray_id) {
		content = px(bw, item->icon_size);
	} else {
		wchar_t *text = utf8_to_utf16(item->text);
		content = text_width(dc, style_font(bw, s), text);
		free(text);
	}
	int w = content + px(bw, s->padding.left + s->padding.right +
		s->border_left.width + s->border_right.width);
	if (w < px(bw, s->min_width)) {
		w = px(bw, s->min_width);
	}
	if (inner) {
		*inner = w;
	}
	return w + px(bw, s->margin.left + s->margin.right);
}

static int module_width(struct bar_window *bw, HDC dc,
		const struct bar_module_view *view) {
	int w = 0;
	for (int i = 0; i < view->items->length; ++i) {
		w += item_outer_width(bw, dc, view->items->items[i], NULL);
	}
	if (view->items->length > 1) {
		w += px(bw, view->item_spacing) * (view->items->length - 1);
	}
	if (view->is_box) {
		const struct swine_style *s = &view->style;
		w += px(bw, s->padding.left + s->padding.right + s->margin.left +
			s->margin.right + s->border_left.width + s->border_right.width);
	}
	return w;
}

static void draw_edges(struct bar_window *bw, HDC dc, const RECT *r,
		const struct swine_style *s, COLORREF under) {
	RECT e;
	if (s->border_top.width) {
		e = (RECT){r->left, r->top, r->right, r->top + px(bw, s->border_top.width)};
		win32_fill(dc, &e, win32_blend(s->border_top.color, under));
	}
	if (s->border_bottom.width) {
		e = (RECT){r->left, r->bottom - px(bw, s->border_bottom.width),
			r->right, r->bottom};
		win32_fill(dc, &e, win32_blend(s->border_bottom.color, under));
	}
	if (s->border_left.width) {
		e = (RECT){r->left, r->top, r->left + px(bw, s->border_left.width),
			r->bottom};
		win32_fill(dc, &e, win32_blend(s->border_left.color, under));
	}
	if (s->border_right.width) {
		e = (RECT){r->right - px(bw, s->border_right.width), r->top, r->right,
			r->bottom};
		win32_fill(dc, &e, win32_blend(s->border_right.color, under));
	}
	// "box-shadow: inset 0 -3px color": a line inside the bottom edge
	if (s->inset_bottom) {
		e = (RECT){r->left, r->bottom - px(bw, s->inset_bottom), r->right,
			r->bottom};
		win32_fill(dc, &e, win32_blend(s->inset_color, under));
	}
	if (s->inset_top) {
		e = (RECT){r->left, r->top, r->right, r->top + px(bw, s->inset_top)};
		win32_fill(dc, &e, win32_blend(s->inset_color, under));
	}
}

// Draws one item with its left edge at x; returns the x after it.
static int draw_item(struct bar_window *bw, HDC dc, int x, int top, int bottom,
		const struct bar_item *item, COLORREF under, int module_index) {
	const struct swine_style *s = &item->style;
	int inner = 0;
	int outer = item_outer_width(bw, dc, item, &inner);
	RECT box = {x + px(bw, s->margin.left), top + px(bw, s->margin.top),
		x + px(bw, s->margin.left) + inner, bottom - px(bw, s->margin.bottom)};
	COLORREF bg = win32_blend(s->background, under);
	if (s->background.a) {
		win32_fill(dc, &box, bg);
	}
	draw_edges(bw, dc, &box, s, bg);
	RECT text_rect = {
		box.left + px(bw, s->padding.left + s->border_left.width),
		box.top + px(bw, s->padding.top + s->border_top.width),
		box.right - px(bw, s->padding.right + s->border_right.width),
		box.bottom - px(bw, s->padding.bottom + s->border_bottom.width),
	};
	if (item->tray_id) {
		int size = px(bw, item->icon_size);
		int ix = text_rect.left + (text_rect.right - text_rect.left - size) / 2;
		int iy = text_rect.top + (text_rect.bottom - text_rect.top - size) / 2;
		RECT icon = {ix, iy, ix + size, iy + size};
		win32_tray_draw(dc, item->tray_id, item->icon_path, &icon, bw->hwnd);
	} else {
		wchar_t *text = utf8_to_utf16(item->text);
		HFONT old = SelectObject(dc, style_font(bw, s));
		SetTextColor(dc, win32_blend(s->color, bg));
		DrawTextW(dc, text, -1, &text_rect,
			DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
		SelectObject(dc, old);
		free(text);
	}

	struct hit *h = calloc(1, sizeof(*h));
	h->rect = box;
	h->module_index = module_index;
	h->tray_id = item->tray_id;
	h->on_click = item->on_click ? swine_strdup(item->on_click) : NULL;
	h->on_scroll_up = item->on_scroll_up ? swine_strdup(item->on_scroll_up) : NULL;
	h->on_scroll_down = item->on_scroll_down ?
		swine_strdup(item->on_scroll_down) : NULL;
	list_add(bw->hits, h);
	return x + outer;
}

static void draw_module(struct bar_window *bw, HDC dc, int x, int top,
		int bottom, const struct bar_module_view *view, COLORREF under) {
	if (!view->is_box) {
		draw_item(bw, dc, x, top, bottom, view->items->items[0], under,
			view->module_index);
		return;
	}
	const struct swine_style *s = &view->style;
	int width = module_width(bw, dc, view);
	RECT box = {x + px(bw, s->margin.left), top + px(bw, s->margin.top),
		x + width - px(bw, s->margin.right), bottom - px(bw, s->margin.bottom)};
	COLORREF bg = win32_blend(s->background, under);
	if (s->background.a) {
		win32_fill(dc, &box, bg);
	}
	draw_edges(bw, dc, &box, s, bg);
	int item_x = box.left + px(bw, s->padding.left + s->border_left.width);
	int item_top = box.top + px(bw, s->padding.top + s->border_top.width);
	int item_bottom = box.bottom - px(bw, s->padding.bottom +
		s->border_bottom.width);
	for (int i = 0; i < view->items->length; ++i) {
		item_x = draw_item(bw, dc, item_x, item_top, item_bottom,
			view->items->items[i], bg, view->module_index) +
			px(bw, view->item_spacing);
	}
}

static struct hit *hit_test(struct bar_window *bw, int x, int y);

// Mouse buttons on tray icons go to the icon's program.
static bool tray_mouse(struct bar_window *bw, int button, bool down,
		LPARAM lparam) {
	struct hit *h = bw ? hit_test(bw, (short)LOWORD(lparam),
		(short)HIWORD(lparam)) : NULL;
	if (!h || !h->tray_id) {
		return false;
	}
	POINT p = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
	ClientToScreen(bw->hwnd, &p);
	win32_tray_mouse(h->tray_id, button, down, p);
	return true;
}

static void paint(struct bar_window *bw, HDC target) {
	RECT client;
	GetClientRect(bw->hwnd, &client);
	int w = client.right, h = client.bottom;
	HDC dc = CreateCompatibleDC(target);
	HBITMAP bitmap = CreateCompatibleBitmap(target, w, h);
	HBITMAP old_bitmap = SelectObject(dc, bitmap);
	SetBkMode(dc, TRANSPARENT);
	clear_hits(bw);

	struct bar_model *model = bw->model;
	// Translucent backgrounds are blended over black for now; true
	// per-pixel transparency needs a layered window.
	COLORREF bg = win32_blend(model->window.background, RGB(0, 0, 0));
	win32_fill(dc, &client, bg);
	draw_edges(bw, dc, &client, &model->window, bg);
	int top = px(bw, model->window.border_top.width);
	int bottom = h - px(bw, model->window.border_bottom.width);
	int spacing = px(bw, model->spacing);

	int left_x = 0;
	int right_w = 0, center_w = 0;
	for (int i = 0; i < model->modules->length; ++i) {
		struct bar_module_view *v = model->modules->items[i];
		if (v->section == BAR_SECTION_RIGHT) {
			right_w += module_width(bw, dc, v) + spacing;
		} else if (v->section == BAR_SECTION_CENTER) {
			center_w += module_width(bw, dc, v) + spacing;
		}
	}
	int right_x = w - right_w + (right_w ? spacing : 0);
	int center_x = (w - center_w + (center_w ? spacing : 0)) / 2;
	for (int i = 0; i < model->modules->length; ++i) {
		struct bar_module_view *v = model->modules->items[i];
		int mw = module_width(bw, dc, v);
		int *x = v->section == BAR_SECTION_LEFT ? &left_x :
			v->section == BAR_SECTION_CENTER ? &center_x : &right_x;
		draw_module(bw, dc, *x, top, bottom, v, bg);
		*x += mw + spacing;
	}

	BitBlt(target, 0, 0, w, h, dc, 0, 0, SRCCOPY);
	SelectObject(dc, old_bitmap);
	DeleteObject(bitmap);
	DeleteDC(dc);
}

static struct hit *hit_test(struct bar_window *bw, int x, int y) {
	POINT p = {x, y};
	for (int i = 0; i < bw->hits->length; ++i) {
		struct hit *h = bw->hits->items[i];
		if (PtInRect(&h->rect, p)) {
			return h;
		}
	}
	return NULL;
}

static LRESULT CALLBACK bar_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
		LPARAM lparam) {
	struct bar_window *bw = bar_from_hwnd(hwnd);
	switch (msg) {
	case WM_DPICHANGED:
		// Bar windows are per-monitor-aware top-level windows. Defer the
		// output refresh to the shell thread so every bar is repositioned
		// with the final monitor scale and geometry.
		if (win32_shell_hwnd) {
			if (!PostMessageW(win32_shell_hwnd, WM_SWINE_DISPLAY_REFRESH, 0, 0)) {
				swine_log(SWINE_ERROR, "Unable to request a display refresh "
					"from the shell (error %lu)", GetLastError());
			}
		}
		return 0;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		if (bw && bw->model) {
			paint(bw, dc);
		}
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_ERASEBKGND:
		return 1;
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;   // clicking the bar never takes focus
	case WM_LBUTTONDOWN:
		tray_mouse(bw, 0, true, lparam);
		return 0;
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
		tray_mouse(bw, 1, msg == WM_MBUTTONDOWN, lparam);
		return 0;
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
		tray_mouse(bw, 2, msg == WM_RBUTTONDOWN, lparam);
		return 0;
	case WM_LBUTTONUP: {
		if (tray_mouse(bw, 0, false, lparam)) {
			return 0;
		}
		struct hit *h = bw ? hit_test(bw, (short)LOWORD(lparam),
			(short)HIWORD(lparam)) : NULL;
		if (h) {
			char *command = h->on_click ? swine_strdup(h->on_click) : NULL;
			bar_module_clicked(h->module_index);
			if (command) {
				win32_run_command(command);
				free(command);
			} else {
				win32_bar_refresh();
			}
		}
		return 0;
	}
	case WM_MOUSEWHEEL: {
		POINT p = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		ScreenToClient(hwnd, &p);
		struct hit *h = bw ? hit_test(bw, p.x, p.y) : NULL;
		short delta = (short)HIWORD(wparam);
		const char *command = !h ? NULL :
			delta > 0 ? h->on_scroll_up : h->on_scroll_down;
		if (command) {
			char *copy = swine_strdup(command);
			win32_run_command(copy);
			free(copy);
		}
		return 0;
	}
	default:
		return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
}

static struct bar_window *create_bar(struct swine_output *output) {
	HINSTANCE instance = GetModuleHandleW(NULL);
	if (!class_registered) {
		WNDCLASSEXW wc = { .cbSize = sizeof(wc) };
		wc.lpfnWndProc = bar_wndproc;
		wc.hInstance = instance;
		wc.hCursor = LoadCursor(NULL, IDC_ARROW);
		wc.lpszClassName = L"swinewm-bar";
		RegisterClassExW(&wc);
		class_registered = true;
	}
	struct bar_window *bw = calloc(1, sizeof(*bw));
	bw->output_name = swine_strdup(output->name);
	bw->hits = create_list();
	bw->hwnd = CreateWindowExW(
		WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
		L"swinewm-bar", L"swinewm bar", WS_POPUP, 0, 0, 0, 0,
		NULL, NULL, instance, NULL);
	SetWindowLongPtrW(bw->hwnd, GWLP_USERDATA, (LONG_PTR)bw);
	return bw;
}

static struct bar_window *find_bar(const char *output_name) {
	for (int i = 0; i < bars->length; ++i) {
		struct bar_window *bw = bars->items[i];
		if (strcmp(bw->output_name, output_name) == 0) {
			return bw;
		}
	}
	return NULL;
}

void win32_bar_refresh(void) {
	if (!bars) {
		bars = create_list();
	}
	// Drop windows whose output is gone.
	for (int i = bars->length - 1; i >= 0; --i) {
		struct bar_window *bw = bars->items[i];
		if (!output_by_name(bw->output_name)) {
			destroy_bar(bw);
			list_del(bars, i);
		}
	}
	time_t now = time(NULL);
	for (int i = 0; i < root->outputs->length; ++i) {
		struct swine_output *output = root->outputs->items[i];
		struct swine_box rect;
		bool wanted = bar_get_rect(output, &rect);
		struct bar_window *bw = find_bar(output->name);
		if (!wanted) {
			if (bw && bw->shown) {
				ShowWindow(bw->hwnd, SW_HIDE);
				bw->shown = false;
			}
			continue;
		}
		if (!bw) {
			bw = create_bar(output);
			list_add(bars, bw);
		}
		bw->scale = win32_output_scale(output);
		clear_hits(bw);
		bar_model_free(bw->model);
		bw->model = bar_build(output, now);
		if (!bw->shown || !box_equal(&bw->rect, &rect)) {
			SetWindowPos(bw->hwnd, HWND_TOPMOST, rect.x, rect.y, rect.width,
				rect.height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
			bw->rect = rect;
			bw->shown = true;
		}
		// Programs place tray menus and some dialogs next to the
		// "taskbar" (our Shell_TrayWnd): make that the primary bar, as
		// ManagedShell does with Cairo's menu bar.
		if (output->box.x == 0 && output->box.y == 0) {
			RECT r = {rect.x, rect.y, rect.x + rect.width, rect.y + rect.height};
			win32_tray_set_host_rect(&r);
		}
		InvalidateRect(bw->hwnd, NULL, FALSE);
	}
}

void win32_bar_finish(void) {
	if (!bars) {
		return;
	}
	for (int i = 0; i < bars->length; ++i) {
		destroy_bar(bars->items[i]);
	}
	list_free(bars);
	bars = NULL;
}
