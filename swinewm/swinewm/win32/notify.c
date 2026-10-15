// Notification popups: one topmost, non-activating window per visible
// notification, stacked at the configured anchor of the output.
#include <stdlib.h>
#include <string.h>
#include "list.h"
#include "log.h"
#include "swinewm/input/seat.h"
#include "swinewm/notify.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/workspace.h"
#include "win32.h"

struct popup {
	HWND hwnd;
	uint32_t id;
	double scale;
	RECT rect;        // last position given to the window
	bool shown;
};

static list_t *popups = NULL;
static bool class_registered = false;

static int scaled(const struct popup *p, double v) {
	return (int)(v * p->scale + 0.5);
}

static HFONT line_font(const struct popup *p, const struct notify_style *s,
		const struct notify_line *line) {
	// Pango sizes are points: 1pt = 1/72", 96 dpi at scale 1.
	int px = (int)(s->font_size * 96.0 / 72.0 * p->scale + 0.5);
	return win32_font(s->font_family, px, s->font_bold || line->bold,
		line->italic);
}

static UINT align_flag(const struct notify_style *s) {
	return s->text_alignment == 1 ? DT_CENTER :
		s->text_alignment == 2 ? DT_RIGHT : DT_LEFT;
}

// Lays out (and, with a DC to paint on, draws) the text; returns its height.
static int layout_text(const struct popup *p, HDC dc,
		const struct swine_notification *n, RECT area, bool draw) {
	list_t *lines = notify_format_lines(n);
	int y = area.top;
	for (int i = 0; i < lines->length; ++i) {
		struct notify_line *line = lines->items[i];
		wchar_t *text = utf8_to_utf16(line->text);
		HFONT old = SelectObject(dc, line_font(p, &n->style, line));
		RECT r = {area.left, y, area.right, area.bottom};
		UINT flags = DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS |
			align_flag(&n->style);
		RECT measure = r;
		DrawTextW(dc, text, -1, &measure, flags | DT_CALCRECT);
		if (draw) {
			r.bottom = y + (measure.bottom - measure.top);
			DrawTextW(dc, text, -1, &r, flags);
		}
		y += measure.bottom - measure.top;
		SelectObject(dc, old);
		free(text);
	}
	notify_free_lines(lines);
	return y - area.top;
}

static int popup_height(struct popup *p, const struct swine_notification *n) {
	const struct notify_style *s = &n->style;
	HDC dc = CreateCompatibleDC(NULL);
	int inner_w = scaled(p, s->width - s->padding.left - s->padding.right -
		2 * s->border_size);
	RECT area = {0, 0, inner_w, 100000};
	int text_h = layout_text(p, dc, n, area, false);
	DeleteDC(dc);
	int h = text_h + scaled(p, s->padding.top + s->padding.bottom +
		2 * s->border_size);
	int max_h = scaled(p, s->height);
	return h > max_h ? max_h : h;
}

static void paint(struct popup *p, HDC target) {
	struct swine_notification *n = notify_by_id(p->id);
	if (!n) {
		return;
	}
	const struct notify_style *s = &n->style;
	RECT client;
	GetClientRect(p->hwnd, &client);
	HDC dc = CreateCompatibleDC(target);
	HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
	HBITMAP old = SelectObject(dc, bitmap);
	SetBkMode(dc, TRANSPARENT);

	COLORREF black = RGB(0, 0, 0);
	COLORREF border = win32_blend(s->border, black);
	COLORREF bg = win32_blend(s->background, black);
	win32_fill(dc, &client, border);
	int b = scaled(p, s->border_size);
	RECT inner = {b, b, client.right - b, client.bottom - b};
	win32_fill(dc, &inner, bg);
	SetTextColor(dc, win32_blend(s->text, bg));
	RECT text_area = {
		inner.left + scaled(p, s->padding.left),
		inner.top + scaled(p, s->padding.top),
		inner.right - scaled(p, s->padding.right),
		inner.bottom - scaled(p, s->padding.bottom),
	};
	HRGN clip = CreateRectRgnIndirect(&text_area);
	SelectClipRgn(dc, clip);
	layout_text(p, dc, n, text_area, true);
	SelectClipRgn(dc, NULL);
	DeleteObject(clip);

	BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
	SelectObject(dc, old);
	DeleteObject(bitmap);
	DeleteDC(dc);
}

static struct popup *popup_from_hwnd(HWND hwnd) {
	return (struct popup *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static void click(HWND hwnd, int button) {
	struct popup *p = popup_from_hwnd(hwnd);
	if (p) {
		notify_button(p->id, button);
		win32_notify_refresh();
	}
}

static LRESULT CALLBACK popup_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
		LPARAM lparam) {
	switch (msg) {
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		struct popup *p = popup_from_hwnd(hwnd);
		if (p) {
			paint(p, dc);
		}
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_ERASEBKGND:
		return 1;
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	case WM_LBUTTONUP:
		click(hwnd, 1);
		return 0;
	case WM_MBUTTONUP:
		click(hwnd, 2);
		return 0;
	case WM_RBUTTONUP:
		click(hwnd, 3);
		return 0;
	default:
		return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
}

static struct popup *create_popup(void) {
	HINSTANCE instance = GetModuleHandleW(NULL);
	if (!class_registered) {
		WNDCLASSEXW wc = { .cbSize = sizeof(wc) };
		wc.lpfnWndProc = popup_wndproc;
		wc.hInstance = instance;
		wc.hCursor = LoadCursor(NULL, IDC_HAND);
		wc.lpszClassName = L"swinewm-notification";
		RegisterClassExW(&wc);
		class_registered = true;
	}
	struct popup *p = calloc(1, sizeof(*p));
	p->hwnd = CreateWindowExW(
		WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
		L"swinewm-notification", L"swinewm notification", WS_POPUP,
		0, 0, 0, 0, NULL, NULL, instance, NULL);
	SetWindowLongPtrW(p->hwnd, GWLP_USERDATA, (LONG_PTR)p);
	return p;
}

static struct swine_output *target_output(const struct notify_style *s) {
	if (s->output[0]) {
		struct swine_output *named = output_by_name(s->output);
		if (named) {
			return named;
		}
	}
	struct swine_workspace *ws = seat_get_focused_workspace();
	if (ws && ws->output && ws->output->enabled) {
		return ws->output;
	}
	return root->outputs->length ? root->outputs->items[0] : NULL;
}

void win32_notify_refresh(void) {
	if (!popups) {
		popups = create_list();
	}
	list_t *visible = notify_visible();
	while (popups->length > visible->length) {
		struct popup *p = popups->items[popups->length - 1];
		DestroyWindow(p->hwnd);
		free(p);
		list_del(popups, popups->length - 1);
	}
	while (popups->length < visible->length) {
		list_add(popups, create_popup());
	}

	// Stack per anchor: the offset grows away from the anchor edge.
	int offset[9] = {0};
	for (int i = 0; i < visible->length; ++i) {
		struct swine_notification *n = visible->items[i];
		struct popup *p = popups->items[i];
		const struct notify_style *s = &n->style;
		struct swine_output *output = target_output(s);
		if (!output) {
			if (p->shown) {
				ShowWindow(p->hwnd, SW_HIDE);
				p->shown = false;
			}
			continue;
		}
		bool same_notification = p->id == n->id;
		p->id = n->id;
		p->scale = win32_output_scale(output);
		struct swine_box area = output_get_usable_area(output);
		int w = scaled(p, s->width);
		int h = popup_height(p, n);
		int mt = scaled(p, s->outer_margin.top + s->margin.top);
		int mb = scaled(p, s->outer_margin.bottom + s->margin.bottom);
		int ml = scaled(p, s->outer_margin.left + s->margin.left);
		int mr = scaled(p, s->outer_margin.right + s->margin.right);
		int x, y;
		switch (s->anchor) {
		case NOTIFY_ANCHOR_TOP_LEFT:
		case NOTIFY_ANCHOR_BOTTOM_LEFT:
		case NOTIFY_ANCHOR_CENTER_LEFT:
			x = area.x + ml;
			break;
		case NOTIFY_ANCHOR_TOP_CENTER:
		case NOTIFY_ANCHOR_BOTTOM_CENTER:
		case NOTIFY_ANCHOR_CENTER:
			x = area.x + (area.width - w) / 2;
			break;
		default:
			x = area.x + area.width - w - mr;
			break;
		}
		int *off = &offset[s->anchor];
		switch (s->anchor) {
		case NOTIFY_ANCHOR_BOTTOM_RIGHT:
		case NOTIFY_ANCHOR_BOTTOM_CENTER:
		case NOTIFY_ANCHOR_BOTTOM_LEFT:
			y = area.y + area.height - mb - h - *off;
			break;
		case NOTIFY_ANCHOR_CENTER_RIGHT:
		case NOTIFY_ANCHOR_CENTER_LEFT:
		case NOTIFY_ANCHOR_CENTER:
			y = area.y + area.height / 3 + *off;
			break;
		default:
			y = area.y + mt + *off;
			break;
		}
		*off += h + scaled(p, s->margin.top);
		// Only touch the window when something changed: this runs every
		// second and after every command.
		RECT rect = {x, y, x + w, y + h};
		if (!p->shown || !EqualRect(&rect, &p->rect)) {
			SetWindowPos(p->hwnd, HWND_TOPMOST, x, y, w, h,
				SWP_NOACTIVATE | SWP_SHOWWINDOW);
			p->rect = rect;
			p->shown = true;
		}
		if (!same_notification) {
			InvalidateRect(p->hwnd, NULL, FALSE);
		}
	}
	list_free(visible);
}

void win32_notify_finish(void) {
	if (!popups) {
		return;
	}
	for (int i = 0; i < popups->length; ++i) {
		struct popup *p = popups->items[i];
		DestroyWindow(p->hwnd);
		free(p);
	}
	list_free(popups);
	popups = NULL;
}
