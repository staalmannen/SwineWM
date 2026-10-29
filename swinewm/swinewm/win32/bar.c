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
	char *on_click_middle;
	char *on_click_right;
	char *on_scroll_up;
	char *on_scroll_down;
	char *tooltip;
	bool tooltip_mono;
	bool has_hover;   // a :hover style that looks different
};

struct bar_window {
	HWND hwnd;
	int bar_index;          // which bar of the config
	char *output_name;
	struct swine_box rect;
	bool shown;
	struct bar_model *model;
	double scale;
	list_t *hits;   // struct hit *, rebuilt on every paint
	// The item under the pointer (by its place: hits are rebuilt).
	bool tracking;
	bool hovering;
	RECT hover_rect;
	uint64_t hover_tray;
};

static HWND tooltip_hwnd = NULL;
static wchar_t *tooltip_text = NULL;
static bool tooltip_mono = false;

static list_t *bars = NULL;
static bool class_registered = false;

static void clear_hits(struct bar_window *bw) {
	for (int i = 0; i < bw->hits->length; ++i) {
		struct hit *h = bw->hits->items[i];
		free(h->on_click);
		free(h->on_click_middle);
		free(h->on_click_right);
		free(h->on_scroll_up);
		free(h->on_scroll_down);
		free(h->tooltip);
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

static HFONT style_font(struct bar_window *bw, const struct swine_style *s,
		int rotate) {
	return win32_font_rotated(s->font_family, px(bw, s->font_size), s->bold,
		s->italic, rotate);
}

static SIZE text_size(HDC dc, HFONT font, const wchar_t *text) {
	SIZE size = {0, 0};
	HFONT old = SelectObject(dc, font);
	GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size);
	SelectObject(dc, old);
	return size;
}

// The content of an item (text or icon) as laid out: rotated text ("rotate"
// 90 or 270) takes its height across and its width along.
static SIZE content_size(struct bar_window *bw, HDC dc,
		const struct bar_item *item) {
	if (item->tray_id) {
		int size = px(bw, item->icon_size);
		return (SIZE){size, size};
	}
	wchar_t *text = utf8_to_utf16(item->text);
	SIZE size = text_size(dc, style_font(bw, &item->style, 0), text);
	free(text);
	if (item->rotate == 90 || item->rotate == 270) {
		return (SIZE){size.cy, size.cx};
	}
	return size;
}

// The length of an item along the bar (its width in a horizontal bar, its
// height in a vertical one) including padding and border; *outer adds the
// margins.
static int item_length(struct bar_window *bw, HDC dc,
		const struct bar_item *item, int *outer) {
	const struct swine_style *s = &item->style;
	SIZE content = content_size(bw, dc, item);
	int length;
	if (bw->model->vertical) {
		length = content.cy + px(bw, s->padding.top + s->padding.bottom +
			s->border_top.width + s->border_bottom.width);
		if (outer) {
			*outer = length + px(bw, s->margin.top + s->margin.bottom);
		}
		return length;
	}
	length = content.cx + px(bw, s->padding.left + s->padding.right +
		s->border_left.width + s->border_right.width);
	if (length < px(bw, s->min_width)) {
		length = px(bw, s->min_width);
	}
	if (outer) {
		*outer = length + px(bw, s->margin.left + s->margin.right);
	}
	return length;
}

static int module_length(struct bar_window *bw, HDC dc,
		const struct bar_module_view *view) {
	int total = 0;
	for (int i = 0; i < view->items->length; ++i) {
		int outer = 0;
		item_length(bw, dc, view->items->items[i], &outer);
		total += outer;
	}
	if (view->items->length > 1) {
		total += px(bw, view->item_spacing) * (view->items->length - 1);
	}
	if (view->is_box) {
		const struct swine_style *s = &view->style;
		total += bw->model->vertical ?
			px(bw, s->padding.top + s->padding.bottom + s->margin.top +
				s->margin.bottom + s->border_top.width + s->border_bottom.width) :
			px(bw, s->padding.left + s->padding.right + s->margin.left +
				s->margin.right + s->border_left.width + s->border_right.width);
	}
	return total;
}

// Text centred in rect, turned counter-clockwise by rotate degrees as GTK
// turns a label: the origin of TextOut is the top-left corner of the text
// cell, which turns with it.
static void draw_text(struct bar_window *bw, HDC dc, const RECT *rect,
		const wchar_t *text, const struct swine_style *s, int rotate) {
	if (!rotate) {
		RECT r = *rect;
		HFONT old = SelectObject(dc, style_font(bw, s, 0));
		DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER |
			DT_NOPREFIX | (bw->model->vertical ? DT_END_ELLIPSIS : 0));
		SelectObject(dc, old);
		return;
	}
	SIZE size = text_size(dc, style_font(bw, s, 0), text);
	int box_w = rect->right - rect->left, box_h = rect->bottom - rect->top;
	int x, y;
	if (rotate == 90) {          // reads bottom to top
		x = rect->left + (box_w - size.cy) / 2;
		y = rect->top + (box_h + size.cx) / 2;
	} else if (rotate == 270) {  // reads top to bottom
		x = rect->left + (box_w + size.cy) / 2;
		y = rect->top + (box_h - size.cx) / 2;
	} else {                     // 180: upside down
		x = rect->left + (box_w + size.cx) / 2;
		y = rect->top + (box_h + size.cy) / 2;
	}
	HFONT old = SelectObject(dc, style_font(bw, s, rotate));
	ExtTextOutW(dc, x, y, ETO_CLIPPED, rect, text, (UINT)wcslen(text), NULL);
	SelectObject(dc, old);
}

// --- painting ------------------------------------------------------------
// The bar is a layered window painted with per-pixel alpha (canvas.c), so
// translucent backgrounds (Waybar's default is rgba(43, 48, 59, 0.5)) show
// the desktop behind it.

// Draws white on the black mask inside rect, then blends color through it.
static void canvas_text(struct bar_window *bw, struct canvas *c,
		const RECT *rect, const wchar_t *text, const struct swine_style *s,
		int rotate, struct swine_color color) {
	canvas_mask_clear(c, rect, false);
	SetTextColor(c->mask_dc, RGB(255, 255, 255));
	draw_text(bw, c->mask_dc, rect, text, s, rotate);
	canvas_mask_blend(c, rect, color);
}

static void canvas_tray_icon(struct bar_window *bw, struct canvas *c,
		const RECT *rect, const struct bar_item *item) {
	canvas_mask_clear(c, rect, true);
	win32_tray_draw(c->mask_dc, item->tray_id, item->icon_path, rect, bw->hwnd);
	win32_tray_draw(c->white_dc, item->tray_id, item->icon_path, rect, bw->hwnd);
	canvas_icon_blend(c, rect);
}

static void draw_edges(struct bar_window *bw, struct canvas *c, const RECT *r,
		const struct swine_style *s) {
	RECT e;
	if (s->border_top.width) {
		e = (RECT){r->left, r->top, r->right, r->top + px(bw, s->border_top.width)};
		canvas_fill(c, &e, s->border_top.color);
	}
	if (s->border_bottom.width) {
		e = (RECT){r->left, r->bottom - px(bw, s->border_bottom.width),
			r->right, r->bottom};
		canvas_fill(c, &e, s->border_bottom.color);
	}
	if (s->border_left.width) {
		e = (RECT){r->left, r->top, r->left + px(bw, s->border_left.width),
			r->bottom};
		canvas_fill(c, &e, s->border_left.color);
	}
	if (s->border_right.width) {
		e = (RECT){r->right - px(bw, s->border_right.width), r->top, r->right,
			r->bottom};
		canvas_fill(c, &e, s->border_right.color);
	}
	// "box-shadow: inset 0 -3px color": a line inside the bottom edge
	if (s->inset_bottom) {
		e = (RECT){r->left, r->bottom - px(bw, s->inset_bottom), r->right,
			r->bottom};
		canvas_fill(c, &e, s->inset_color);
	}
	if (s->inset_top) {
		e = (RECT){r->left, r->top, r->right, r->top + px(bw, s->inset_top)};
		canvas_fill(c, &e, s->inset_color);
	}
}

// Draws one item at pos along the bar, between cross_start and cross_end
// across it; returns the position after it.
static int draw_item(struct bar_window *bw, struct canvas *c, int pos,
		int cross_start, int cross_end, const struct bar_item *item,
		int module_index) {
	const struct swine_style *s = &item->style;
	int outer = 0;
	int length = item_length(bw, c->dc, item, &outer);
	RECT box;
	if (bw->model->vertical) {
		box = (RECT){cross_start + px(bw, s->margin.left),
			pos + px(bw, s->margin.top),
			cross_end - px(bw, s->margin.right),
			pos + px(bw, s->margin.top) + length};
	} else {
		box = (RECT){pos + px(bw, s->margin.left),
			cross_start + px(bw, s->margin.top),
			pos + px(bw, s->margin.left) + length,
			cross_end - px(bw, s->margin.bottom)};
	}
	// :hover changes the look, never the size.
	if (item->has_hover && bw->hovering && EqualRect(&box, &bw->hover_rect)) {
		s = &item->hover_style;
	}
	canvas_fill(c, &box, s->background);
	draw_edges(bw, c, &box, s);
	RECT content = {
		box.left + px(bw, s->padding.left + s->border_left.width),
		box.top + px(bw, s->padding.top + s->border_top.width),
		box.right - px(bw, s->padding.right + s->border_right.width),
		box.bottom - px(bw, s->padding.bottom + s->border_bottom.width),
	};
	if (item->tray_id) {
		int size = px(bw, item->icon_size);
		int ix = content.left + (content.right - content.left - size) / 2;
		int iy = content.top + (content.bottom - content.top - size) / 2;
		RECT icon = {ix, iy, ix + size, iy + size};
		canvas_tray_icon(bw, c, &icon, item);
	} else {
		wchar_t *text = utf8_to_utf16(item->text);
		canvas_text(bw, c, &content, text, s, item->rotate, s->color);
		free(text);
	}

	struct hit *h = calloc(1, sizeof(*h));
	h->rect = box;
	h->module_index = module_index;
	h->tray_id = item->tray_id;
	h->on_click = item->on_click ? swine_strdup(item->on_click) : NULL;
	h->on_click_middle = item->on_click_middle ?
		swine_strdup(item->on_click_middle) : NULL;
	h->on_click_right = item->on_click_right ?
		swine_strdup(item->on_click_right) : NULL;
	h->on_scroll_up = item->on_scroll_up ? swine_strdup(item->on_scroll_up) : NULL;
	h->on_scroll_down = item->on_scroll_down ?
		swine_strdup(item->on_scroll_down) : NULL;
	h->tooltip = item->tooltip ? swine_strdup(item->tooltip) : NULL;
	h->tooltip_mono = item->tooltip_mono;
	h->has_hover = item->has_hover;
	list_add(bw->hits, h);
	return pos + outer;
}

static void draw_module(struct bar_window *bw, struct canvas *c, int pos,
		int cross_start, int cross_end, const struct bar_module_view *view) {
	if (!view->is_box) {
		draw_item(bw, c, pos, cross_start, cross_end, view->items->items[0],
			view->module_index);
		return;
	}
	const struct swine_style *s = &view->style;
	int length = module_length(bw, c->dc, view);
	bool vertical = bw->model->vertical;
	RECT box;
	if (vertical) {
		box = (RECT){cross_start + px(bw, s->margin.left),
			pos + px(bw, s->margin.top), cross_end - px(bw, s->margin.right),
			pos + length - px(bw, s->margin.bottom)};
	} else {
		box = (RECT){pos + px(bw, s->margin.left),
			cross_start + px(bw, s->margin.top),
			pos + length - px(bw, s->margin.right),
			cross_end - px(bw, s->margin.bottom)};
	}
	canvas_fill(c, &box, s->background);
	draw_edges(bw, c, &box, s);
	RECT inner = {
		box.left + px(bw, s->padding.left + s->border_left.width),
		box.top + px(bw, s->padding.top + s->border_top.width),
		box.right - px(bw, s->padding.right + s->border_right.width),
		box.bottom - px(bw, s->padding.bottom + s->border_bottom.width),
	};
	int item_pos = vertical ? inner.top : inner.left;
	for (int i = 0; i < view->items->length; ++i) {
		item_pos = draw_item(bw, c, item_pos,
			vertical ? inner.left : inner.top,
			vertical ? inner.right : inner.bottom,
			view->items->items[i], view->module_index) +
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

// Modules-left from the start of the bar (left, or top of a vertical bar),
// modules-center in the middle, modules-right at the end. Paints the whole
// window and hands it to Windows with its alpha (UpdateLayeredWindow).
static void render(struct bar_window *bw) {
	if (!bw->model || !bw->shown) {
		return;
	}
	int w = bw->rect.width, h = bw->rect.height;
	if (w <= 0 || h <= 0) {
		return;
	}
	HDC screen = GetDC(NULL);
	struct canvas c;
	bool ok = canvas_init(&c, screen, w, h);
	ReleaseDC(NULL, screen);
	if (!ok) {
		return;
	}
	clear_hits(bw);

	struct bar_model *model = bw->model;
	bool vertical = model->vertical;
	RECT client = {0, 0, w, h};
	canvas_fill(&c, &client, model->window.background);
	draw_edges(bw, &c, &client, &model->window);
	int cross_start = vertical ? px(bw, model->window.border_left.width) :
		px(bw, model->window.border_top.width);
	int cross_end = vertical ? w - px(bw, model->window.border_right.width) :
		h - px(bw, model->window.border_bottom.width);
	int total = vertical ? h : w;
	int spacing = px(bw, model->spacing);

	int start = 0;
	int end_len = 0, center_len = 0;
	for (int i = 0; i < model->modules->length; ++i) {
		struct bar_module_view *v = model->modules->items[i];
		if (v->section == BAR_SECTION_RIGHT) {
			end_len += module_length(bw, c.dc, v) + spacing;
		} else if (v->section == BAR_SECTION_CENTER) {
			center_len += module_length(bw, c.dc, v) + spacing;
		}
	}
	int end = total - end_len + (end_len ? spacing : 0);
	int center = (total - center_len + (center_len ? spacing : 0)) / 2;
	for (int i = 0; i < model->modules->length; ++i) {
		struct bar_module_view *v = model->modules->items[i];
		int length = module_length(bw, c.dc, v);
		int *pos = v->section == BAR_SECTION_LEFT ? &start :
			v->section == BAR_SECTION_CENTER ? &center : &end;
		draw_module(bw, &c, *pos, cross_start, cross_end, v);
		*pos += length + spacing;
	}
	// Fully transparent pixels would let clicks through to the desktop;
	// Waybar's surface takes them everywhere.
	for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) {
		if (!(c.px[i] >> 24)) {
			c.px[i] = 0x01000000;
		}
	}

	canvas_present(&c, bw->hwnd, bw->rect.x, bw->rect.y);
	canvas_finish(&c);
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

// --- tooltips ------------------------------------------------------------
// One small window of our own, styled by "tooltip" and "tooltip label" in
// style.css, shown after the hover delay as GTK shows Waybar's tooltips.

static HFONT tooltip_font(const struct swine_style *label, double scale) {
	int size = (int)(label->font_size * scale + 0.5);
	return tooltip_mono ? win32_font("Consolas, Courier New", size, false, false) :
		win32_font(label->font_family, size, label->bold, label->italic);
}

static LRESULT CALLBACK tooltip_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
		LPARAM lparam) {
	if (msg == WM_PAINT) {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		struct swine_style box, label;
		bar_tooltip_style(&box, &label);
		double scale = (double)GetDpiForWindow(hwnd) / 96.0;
		RECT client;
		GetClientRect(hwnd, &client);
		COLORREF under = RGB(0, 0, 0);
		COLORREF bg = win32_blend(box.background, under);
		win32_fill(dc, &client, bg);
		int border = (int)(box.border_top.width * scale + 0.5);
		if (border > 0) {
			COLORREF edge = win32_blend(box.border_top.color, bg);
			RECT e = {0, 0, client.right, border};
			win32_fill(dc, &e, edge);
			e = (RECT){0, client.bottom - border, client.right, client.bottom};
			win32_fill(dc, &e, edge);
			e = (RECT){0, 0, border, client.bottom};
			win32_fill(dc, &e, edge);
			e = (RECT){client.right - border, 0, client.right, client.bottom};
			win32_fill(dc, &e, edge);
		}
		RECT text = {
			border + (int)(box.padding.left * scale + 0.5),
			border + (int)(box.padding.top * scale + 0.5),
			client.right - border - (int)(box.padding.right * scale + 0.5),
			client.bottom - border - (int)(box.padding.bottom * scale + 0.5),
		};
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, win32_blend(label.color, bg));
		HGDIOBJ old = SelectObject(dc, tooltip_font(&label, scale));
		if (tooltip_text) {
			DrawTextW(dc, tooltip_text, -1, &text, DT_NOPREFIX | DT_EXPANDTABS);
		}
		SelectObject(dc, old);
		EndPaint(hwnd, &ps);
		return 0;
	}
	if (msg == WM_MOUSEACTIVATE) {
		return MA_NOACTIVATE;
	}
	if (msg == WM_NCHITTEST) {
		return HTTRANSPARENT;
	}
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static void tooltip_hide(void) {
	if (tooltip_hwnd) {
		ShowWindow(tooltip_hwnd, SW_HIDE);
	}
}

// Shows text near the pointer, beside the bar (below a top bar).
static void tooltip_show(struct bar_window *bw, const char *text, bool mono,
		POINT pointer) {
	HINSTANCE instance = GetModuleHandleW(NULL);
	if (!tooltip_hwnd) {
		WNDCLASSEXW wc = { .cbSize = sizeof(wc) };
		wc.lpfnWndProc = tooltip_wndproc;
		wc.hInstance = instance;
		wc.lpszClassName = L"swinewm-tooltip";
		RegisterClassExW(&wc);
		tooltip_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
			WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, L"swinewm-tooltip", L"",
			WS_POPUP, 0, 0, 0, 0, NULL, NULL, instance, NULL);
		if (!tooltip_hwnd) {
			return;
		}
	}
	free(tooltip_text);
	tooltip_text = utf8_to_utf16(text);
	tooltip_mono = mono;
	struct swine_style box, label;
	bar_tooltip_style(&box, &label);
	double scale = bw->scale > 0 ? bw->scale : 1.0;
	HDC dc = GetDC(tooltip_hwnd);
	HGDIOBJ old = SelectObject(dc, tooltip_font(&label, scale));
	RECT measure = {0, 0, 0, 0};
	DrawTextW(dc, tooltip_text, -1, &measure,
		DT_CALCRECT | DT_NOPREFIX | DT_EXPANDTABS);
	SelectObject(dc, old);
	ReleaseDC(tooltip_hwnd, dc);
	int border = (int)(box.border_top.width * scale + 0.5);
	int w = measure.right + 2 * border +
		(int)((box.padding.left + box.padding.right) * scale + 0.5);
	int h = measure.bottom + 2 * border +
		(int)((box.padding.top + box.padding.bottom) * scale + 0.5);
	// Beside the bar, at the pointer, kept on the monitor.
	HMONITOR monitor = MonitorFromPoint(pointer, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi = { .cbSize = sizeof(mi) };
	GetMonitorInfoW(monitor, &mi);
	int gap = (int)(4 * scale + 0.5);
	int x = pointer.x - w / 2;
	int y = bw->rect.y + bw->rect.height + gap;
	if (y + h > mi.rcMonitor.bottom) {
		y = bw->rect.y - h - gap;   // a bottom bar
	}
	if (x + w > mi.rcMonitor.right) {
		x = mi.rcMonitor.right - w;
	}
	if (x < mi.rcMonitor.left) {
		x = mi.rcMonitor.left;
	}
	SetWindowPos(tooltip_hwnd, HWND_TOPMOST, x, y, w, h,
		SWP_NOACTIVATE | SWP_SHOWWINDOW);
	InvalidateRect(tooltip_hwnd, NULL, TRUE);
}

// The pointer moved over the bar: follow which item it is on, tell tray
// icons (Explorer's WM_MOUSEMOVE), and wait for the hover delay again.
static void bar_mouse_move(struct bar_window *bw, int x, int y) {
	if (!bw->tracking) {
		TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE | TME_HOVER, bw->hwnd,
			HOVER_DEFAULT };
		TrackMouseEvent(&t);
		bw->tracking = true;
	}
	struct hit *h = hit_test(bw, x, y);
	POINT screen = {x, y};
	ClientToScreen(bw->hwnd, &screen);
	bool same = h && bw->hovering && EqualRect(&h->rect, &bw->hover_rect);
	if (!same) {
		tooltip_hide();
		struct hit *old = bw->hovering ? hit_test(bw,
			bw->hover_rect.left, bw->hover_rect.top) : NULL;
		if ((old && old->has_hover) || (h && h->has_hover)) {
			render(bw);   // :hover styles
		}
		if (bw->hovering && bw->hover_tray) {
			win32_tray_hover(bw->hover_tray, TRAY_HOVER_LEAVE, screen);
		}
		bw->hovering = h != NULL;
		bw->hover_rect = h ? h->rect : (RECT){0};
		bw->hover_tray = h ? h->tray_id : 0;
		// Restart the hover delay for the new item.
		TRACKMOUSEEVENT t = { sizeof(t), TME_HOVER, bw->hwnd, HOVER_DEFAULT };
		TrackMouseEvent(&t);
	}
	if (h && h->tray_id) {
		win32_tray_hover(h->tray_id, TRAY_HOVER_MOVE, screen);
	}
}

static void bar_mouse_hover(struct bar_window *bw, int x, int y) {
	struct hit *h = hit_test(bw, x, y);
	if (!h) {
		return;
	}
	POINT screen = {x, y};
	ClientToScreen(bw->hwnd, &screen);
	bool show = h->tooltip != NULL;
	if (h->tray_id) {
		show = win32_tray_hover(h->tray_id, TRAY_HOVER_POPUP, screen) && show;
	}
	if (show) {
		tooltip_show(bw, h->tooltip, h->tooltip_mono, screen);
	}
}

static void bar_mouse_leave(struct bar_window *bw) {
	tooltip_hide();
	struct hit *old = bw->hovering ? hit_test(bw, bw->hover_rect.left,
		bw->hover_rect.top) : NULL;
	if (old && old->has_hover) {
		render(bw);
	}
	if (bw->hovering && bw->hover_tray) {
		POINT screen;
		GetCursorPos(&screen);
		win32_tray_hover(bw->hover_tray, TRAY_HOVER_LEAVE, screen);
	}
	bw->tracking = false;
	bw->hovering = false;
	bw->hover_tray = 0;
}

static LRESULT CALLBACK bar_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
		LPARAM lparam) {
	struct bar_window *bw = bar_from_hwnd(hwnd);
	if (msg == WM_WINDOWPOSCHANGED || (msg == win32_appbar_message() &&
			msg != 0)) {
		// AppBar bookkeeping (Explorer asks to be told of moves)
		win32_appbar_window_message(hwnd, msg, wparam);
		if (msg != WM_WINDOWPOSCHANGED) {
			return 0;
		}
	}
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
		// Layered: Windows keeps what render() gave it.
		PAINTSTRUCT ps;
		BeginPaint(hwnd, &ps);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_ERASEBKGND:
		return 1;
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;   // clicking the bar never takes focus
	case WM_MOUSEMOVE:
		if (bw) {
			bar_mouse_move(bw, (short)LOWORD(lparam), (short)HIWORD(lparam));
		}
		return 0;
	case WM_MOUSEHOVER:
		if (bw) {
			bar_mouse_hover(bw, (short)LOWORD(lparam), (short)HIWORD(lparam));
		}
		return 0;
	case WM_MOUSELEAVE:
		if (bw) {
			bar_mouse_leave(bw);
		}
		return 0;
	case WM_LBUTTONDOWN:
		tooltip_hide();
		tray_mouse(bw, 0, true, lparam);
		return 0;
	case WM_MBUTTONDOWN:
	case WM_RBUTTONDOWN:
		tray_mouse(bw, msg == WM_MBUTTONDOWN ? 1 : 2, true, lparam);
		return 0;
	case WM_MBUTTONUP:
	case WM_RBUTTONUP: {
		bool middle = msg == WM_MBUTTONUP;
		if (tray_mouse(bw, middle ? 1 : 2, false, lparam)) {
			return 0;
		}
		// on-click-middle / on-click-right
		struct hit *h = bw ? hit_test(bw, (short)LOWORD(lparam),
			(short)HIWORD(lparam)) : NULL;
		if (h) {
			const char *command = middle ? h->on_click_middle : h->on_click_right;
			char *copy = command ? swine_strdup(command) : NULL;
			int index = h->module_index;
			if (copy) {
				win32_run_command(copy);
				free(copy);
			}
			if (middle) {
				bar_module_event(index);
			} else {
				bar_module_right_clicked(index);
			}
			win32_bar_refresh();
		}
		return 0;
	}
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
		int index = h ? h->module_index : -1;
		if (command) {
			char *copy = swine_strdup(command);
			win32_run_command(copy);
			free(copy);
			bar_module_event(index);
		} else if (index >= 0 && bar_module_scroll(index, delta > 0)) {
			win32_bar_refresh();
		}
		return 0;
	}
	default:
		return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
}

static struct bar_window *create_bar(int index, struct swine_output *output) {
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
	bw->bar_index = index;
	bw->output_name = swine_strdup(output->name);
	bw->hits = create_list();
	bw->hwnd = CreateWindowExW(
		WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
		L"swinewm-bar", L"swinewm bar", WS_POPUP, 0, 0, 0, 0,
		NULL, NULL, instance, NULL);
	SetWindowLongPtrW(bw->hwnd, GWLP_USERDATA, (LONG_PTR)bw);
	return bw;
}

static struct bar_window *find_bar(int index, const char *output_name) {
	for (int i = 0; i < bars->length; ++i) {
		struct bar_window *bw = bars->items[i];
		if (bw->bar_index == index && strcmp(bw->output_name, output_name) == 0) {
			return bw;
		}
	}
	return NULL;
}

void win32_bar_refresh(void) {
	if (!bars) {
		bars = create_list();
	}
	// Drop windows whose output or bar is gone.
	int count = bar_enabled() ? bar_count() : 0;
	for (int i = bars->length - 1; i >= 0; --i) {
		struct bar_window *bw = bars->items[i];
		if (!output_by_name(bw->output_name) || bw->bar_index >= count) {
			destroy_bar(bw);
			list_del(bars, i);
		}
	}
	time_t now = time(NULL);
	bool tray_host_set = false;
	win32_appbar_begin();
	for (int n = 0; n < count * root->outputs->length; ++n) {
		int index = n / root->outputs->length;
		struct swine_output *output = root->outputs->items[n % root->outputs->length];
		struct swine_box rect;
		bool wanted = bar_get_rect_n(index, output, &rect);
		struct bar_window *bw = find_bar(index, output->name);
		if (!wanted) {
			if (bw && bw->shown) {
				ShowWindow(bw->hwnd, SW_HIDE);
				bw->shown = false;
			}
			continue;
		}
		if (!bw) {
			bw = create_bar(index, output);
			list_add(bars, bw);
		}
		bw->scale = win32_output_scale(output);
		clear_hits(bw);
		bar_model_free(bw->model);
		bw->model = bar_build_n(index, output, now);
		if (!bw->shown || !box_equal(&bw->rect, &rect)) {
			SetWindowPos(bw->hwnd, HWND_TOPMOST, rect.x, rect.y, rect.width,
				rect.height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
			bw->rect = rect;
			bw->shown = true;
		}
		struct swine_box zone;
		if (bar_get_zone_n(index, output, &zone)) {
			win32_appbar_set(bw->hwnd, (int)bar_position_n(index), &zone);
		}
		// Programs place tray menus and some dialogs next to the
		// "taskbar" (our Shell_TrayWnd): make that the primary bar, as
		// ManagedShell does with Cairo's menu bar.
		if (output->box.x == 0 && output->box.y == 0 && !tray_host_set) {
			RECT r = {rect.x, rect.y, rect.x + rect.width, rect.y + rect.height};
			win32_tray_set_host_rect(&r);
			tray_host_set = true;
		}
		render(bw);
	}
	win32_appbar_end();
}

void win32_bar_finish(void) {
	if (tooltip_hwnd) {
		DestroyWindow(tooltip_hwnd);
		tooltip_hwnd = NULL;
	}
	free(tooltip_text);
	tooltip_text = NULL;
	if (!bars) {
		return;
	}
	for (int i = 0; i < bars->length; ++i) {
		destroy_bar(bars->items[i]);
	}
	list_free(bars);
	bars = NULL;
}
