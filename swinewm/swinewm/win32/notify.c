// Notification popups: one topmost, non-activating layered window per
// visible notification, stacked at the configured anchor of the output and
// drawn as mako's render_notification draws them (rounded border, progress
// bar, icon, text; layout ported from mako's render.c, Copyright (c) 2018
// emersion, MIT).
#define COBJMACROS
#include <stdlib.h>
#include <string.h>
#include "list.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/input/seat.h"
#include "swinewm/notify.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/workspace.h"
#include "win32.h"
#include <shellapi.h>
#include <shlobj.h>

struct popup {
	HWND hwnd;
	uint32_t id;
	struct swine_notification *placeholder;   // owned: "(%h more)"
	double scale;
	RECT rect;        // last position given to the window
	bool shown;
	// the icon, kept while app_icon and the size stay the same
	char *icon_key;
	uint32_t *icon_px;   // premultiplied BGRA
	int icon_w, icon_h;
};

// Where things go inside a popup, in pixels.
struct popup_layout {
	int w, h;
	int border;
	bool has_icon;
	int icon_x, icon_y;
	RECT text;        // the text block
};

static list_t *popups = NULL;
static bool class_registered = false;

static int scaled(const struct popup *p, double v) {
	return (int)(v * p->scale + 0.5);
}

// --- icons ---------------------------------------------------------------

// Colour and alpha of an icon, from drawing it over black and over white.
static uint32_t *hicon_pixels(HICON icon, int size) {
	uint32_t *black_px, *white_px;
	HDC dc = CreateCompatibleDC(NULL);
	HBITMAP black = win32_dib(dc, size, size, &black_px);
	HBITMAP white = win32_dib(dc, size, size, &white_px);
	uint32_t *out = NULL;
	if (black && white) {
		size_t n = (size_t)size * (size_t)size;
		memset(black_px, 0, n * 4);
		memset(white_px, 0xFF, n * 4);
		HGDIOBJ old = SelectObject(dc, black);
		DrawIconEx(dc, 0, 0, icon, size, size, 0, NULL, DI_NORMAL);
		SelectObject(dc, white);
		DrawIconEx(dc, 0, 0, icon, size, size, 0, NULL, DI_NORMAL);
		SelectObject(dc, old);
		GdiFlush();
		out = malloc(n * 4);
		for (size_t i = 0; i < n; ++i) {
			uint32_t k = black_px[i], wh = white_px[i];
			int through = ((int)((wh >> 16) & 0xFF) - (int)((k >> 16) & 0xFF) +
				(int)((wh >> 8) & 0xFF) - (int)((k >> 8) & 0xFF) +
				(int)(wh & 0xFF) - (int)(k & 0xFF)) / 3;
			unsigned a = through < 0 ? 255 : through > 255 ? 0 :
				255 - (unsigned)through;
			unsigned r = (k >> 16) & 0xFF, g = (k >> 8) & 0xFF, b = k & 0xFF;
			out[i] = (unsigned)a << 24 | (r < a ? r : a) << 16 |
				(g < a ? g : a) << 8 | (b < a ? b : a);
		}
	}
	if (black) DeleteObject(black);
	if (white) DeleteObject(white);
	DeleteDC(dc);
	return out;
}

// Icon resource index (or -id) in a file, at a size.
static uint32_t *file_icon_pixels(const wchar_t *path, int index, int size) {
	HICON icon = NULL;
	if (FAILED(SHDefExtractIconW(path, index, 0, &icon, NULL,
			(UINT)MAKELONG(size, 16))) || !icon) {
		return NULL;
	}
	uint32_t *px = hicon_pixels(icon, size);
	DestroyIcon(icon);
	return px;
}

// freedesktop icon names that Windows has stock icons for.
static uint32_t *stock_icon_pixels(const char *name, int size) {
	static const struct {
		const char *name;
		SHSTOCKICONID id;
	} stock[] = {
		{"dialog-information", SIID_INFO},
		{"dialog-warning", SIID_WARNING},
		{"dialog-error", SIID_ERROR},
		{"dialog-question", SIID_HELP},
		{"dialog-password", SIID_SHIELD},
		{"security-high", SIID_SHIELD},
	};
	for (size_t i = 0; i < sizeof(stock) / sizeof(stock[0]); ++i) {
		if (strcmp(name, stock[i].name) != 0) {
			continue;
		}
		SHSTOCKICONINFO sii = { .cbSize = sizeof(sii) };
		if (FAILED(SHGetStockIconInfo(stock[i].id, SHGSI_ICONLOCATION, &sii))) {
			return NULL;
		}
		return file_icon_pixels(sii.szPath, sii.iIcon, size);
	}
	return NULL;
}

static bool has_extension(const char *path, const char *ext) {
	size_t len = strlen(path), elen = strlen(ext);
	return len > elen && swine_strcasecmp(path + len - elen, ext) == 0;
}

// An image file scaled down to fit max x max, as mako's fit_to_square.
static uint32_t *image_pixels(const char *path, int max, int *w, int *h) {
	UINT iw, ih;
	if (!win32_image_size(path, &iw, &ih) || !iw || !ih) {
		return NULL;
	}
	double longest = iw > ih ? iw : ih;
	double scale = longest > max ? max / longest : 1.0;
	HBITMAP bitmap = win32_image_load(path, w, h, scale);
	if (!bitmap) {
		return NULL;
	}
	DIBSECTION ds;
	uint32_t *px = NULL;
	if (GetObjectW(bitmap, sizeof(ds), &ds) == sizeof(ds) && ds.dsBm.bmBits) {
		size_t n = (size_t)*w * (size_t)*h;
		px = malloc(n * 4);
		memcpy(px, ds.dsBm.bmBits, n * 4);
	}
	DeleteObject(bitmap);
	return px;
}

// An item of the shell namespace: "shell:AppsFolder\<AppUserModelID>" is
// how mirrored Windows toasts name their application's icon.
static uint32_t *shell_item_pixels(const char *name, int size) {
	wchar_t *wname = utf8_to_utf16(name);
	IShellItemImageFactory *factory = NULL;
	HRESULT hr = SHCreateItemFromParsingName(wname, NULL,
		&IID_IShellItemImageFactory, (void **)&factory);
	free(wname);
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
	uint32_t *px = NULL;
	if (GetObjectW(bitmap, sizeof(bm), &bm) && bm.bmWidth == size &&
			bm.bmHeight == size) {
		BITMAPINFO bi = {0};
		bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
		bi.bmiHeader.biWidth = size;
		bi.bmiHeader.biHeight = -size;   // top-down
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		px = malloc((size_t)size * (size_t)size * 4);
		HDC dc = GetDC(NULL);
		if (!GetDIBits(dc, bitmap, 0, (UINT)size, px, &bi, DIB_RGB_COLORS)) {
			free(px);
			px = NULL;
		}
		ReleaseDC(NULL, dc);
	}
	DeleteObject(bitmap);
	if (px) {
		// Premultiplied alpha; a bitmap without any is opaque.
		size_t n = (size_t)size * (size_t)size;
		bool has_alpha = false;
		for (size_t i = 0; i < n && !has_alpha; ++i) {
			has_alpha = (px[i] >> 24) != 0;
		}
		for (size_t i = 0; !has_alpha && i < n; ++i) {
			px[i] |= 0xFF000000u;
		}
	}
	return px;
}

static uint32_t *load_icon(const struct swine_notification *n, int max,
		int *w, int *h) {
	*w = *h = max;
	if (has_prefix(n->app_icon, "shell:")) {
		return shell_item_pixels(n->app_icon, max);
	}
	uint32_t *px = stock_icon_pixels(n->app_icon, max);
	if (px) {
		return px;
	}
	list_t *candidates = notify_icon_candidates(n);
	for (int i = 0; i < candidates->length && !px; ++i) {
		const char *path = candidates->items[i];
		wchar_t *wpath = utf8_to_utf16(path);
		if (GetFileAttributesW(wpath) != INVALID_FILE_ATTRIBUTES) {
			if (has_extension(path, ".ico") || has_extension(path, ".exe") ||
					has_extension(path, ".dll")) {
				*w = *h = max;
				px = file_icon_pixels(wpath, 0, max);
			} else {
				px = image_pixels(path, max, w, h);
			}
		}
		free(wpath);
	}
	list_free_items_and_destroy(candidates);
	return px;
}

static void update_icon(struct popup *p, const struct swine_notification *n) {
	int max = scaled(p, n->style.max_icon_size);
	char *key = n->style.icons && n->app_icon && *n->app_icon ?
		format_str("%d %s\n%s", max, n->app_icon, n->style.icon_path) : NULL;
	if ((!key && !p->icon_key) ||
			(key && p->icon_key && strcmp(key, p->icon_key) == 0)) {
		free(key);
		return;
	}
	free(p->icon_key);
	free(p->icon_px);
	p->icon_key = key;
	p->icon_px = key ? load_icon(n, max, &p->icon_w, &p->icon_h) : NULL;
	if (key && !p->icon_px) {
		swine_log(SWINE_DEBUG, "No icon found for '%s'", n->app_icon);
	}
}

// --- layout and drawing --------------------------------------------------

static HFONT span_font(const struct popup *p, const struct notify_style *s,
		const struct markup_span *span) {
	// Pango sizes are points: 1pt = 1/72", 96 dpi at scale 1.
	int px = (int)(s->font_size * 96.0 / 72.0 * p->scale + 0.5);
	return win32_font(s->font_family, px, s->font_bold || span->bold,
		span->italic);
}

// A word of a span with the spaces after it: lines break between words.
struct word {
	const wchar_t *text;
	int len;
	HFONT font;
	const struct markup_span *span;
};

// A word, or the part of one, placed on a visual line.
struct placed {
	const struct word *word;
	int start, len;
	int x;
};

struct text_layout {
	HDC dc;
	RECT area;
	UINT align;           // 0 left, 1 center, 2 right
	bool draw;
	int y;
	int widest;
	struct placed *placed;
	int count, cap;
};

static int extent(HDC dc, HFONT font, const wchar_t *text, int len) {
	SIZE size = {0};
	HFONT old = SelectObject(dc, font);
	GetTextExtentPoint32W(dc, text, len, &size);
	SelectObject(dc, old);
	return size.cx;
}

static int trimmed_len(const wchar_t *text, int len) {
	while (len > 0 && text[len - 1] == L' ') {
		len--;
	}
	return len;
}

static void place(struct text_layout *t, const struct word *w, int start,
		int len, int x) {
	if (t->count == t->cap) {
		t->cap = t->cap ? t->cap * 2 : 16;
		t->placed = realloc(t->placed, sizeof(*t->placed) * (size_t)t->cap);
	}
	t->placed[t->count++] = (struct placed){w, start, len, x};
}

// Ends a visual line: aligns it, draws it (at a common baseline) and
// moves down by its height.
static void flush_line(struct text_layout *t) {
	if (!t->count) {
		return;
	}
	int ascent = 0, descent = 0, width = 0;
	for (int i = 0; i < t->count; ++i) {
		struct placed *pl = &t->placed[i];
		HFONT old = SelectObject(t->dc, pl->word->font);
		TEXTMETRICW tm;
		GetTextMetricsW(t->dc, &tm);
		SelectObject(t->dc, old);
		if (tm.tmAscent > ascent) ascent = tm.tmAscent;
		if (tm.tmDescent > descent) descent = tm.tmDescent;
		int len = i == t->count - 1 ?
			trimmed_len(pl->word->text + pl->start, pl->len) : pl->len;
		int right = pl->x + extent(t->dc, pl->word->font,
			pl->word->text + pl->start, len);
		if (right > width) width = right;
	}
	int avail = t->area.right - t->area.left;
	int offset = t->align == 1 ? (avail - width) / 2 :
		t->align == 2 ? avail - width : 0;
	if (offset < 0) {
		offset = 0;
	}
	if (width > t->widest) {
		t->widest = width;
	}
	int baseline = t->y + ascent;
	for (int i = 0; t->draw && i < t->count; ++i) {
		struct placed *pl = &t->placed[i];
		const struct markup_span *span = pl->word->span;
		int x = t->area.left + offset + pl->x;
		HFONT old = SelectObject(t->dc, pl->word->font);
		UINT old_align = SetTextAlign(t->dc, TA_BASELINE | TA_LEFT);
		TextOutW(t->dc, x, baseline, pl->word->text + pl->start, pl->len);
		SetTextAlign(t->dc, old_align);
		if (span->underline || span->strikethrough) {
			// The font's own positions; a line under or through the text,
			// not under the spaces that end a line.
			OUTLINETEXTMETRICW otm = {.otmSize = sizeof(otm)};
			bool have = GetOutlineTextMetricsW(t->dc, sizeof(otm), &otm) != 0;
			int len = i == t->count - 1 ?
				trimmed_len(pl->word->text + pl->start, pl->len) : pl->len;
			int w = extent(t->dc, pl->word->font,
				pl->word->text + pl->start, len);
			int thick = have && otm.otmsUnderscoreSize > 0 ?
				(int)otm.otmsUnderscoreSize : 1;
			HBRUSH white = GetStockObject(WHITE_BRUSH);
			if (span->underline) {
				int under = have ? -otm.otmsUnderscorePosition : ascent / 8 + 1;
				RECT r = {x, baseline + under, x + w, baseline + under + thick};
				FillRect(t->dc, &r, white);
			}
			if (span->strikethrough) {
				int through = have ? (int)otm.otmsStrikeoutPosition : ascent / 3;
				int sthick = have && otm.otmsStrikeoutSize > 0 ?
					(int)otm.otmsStrikeoutSize : thick;
				RECT r = {x, baseline - through, x + w,
					baseline - through + sthick};
				FillRect(t->dc, &r, white);
			}
		}
		SelectObject(t->dc, old);
	}
	t->y += ascent + descent;
	t->count = 0;
}

// Lays out one line of the format: its words wrap at the area's width, a
// word wider than the area is broken where it must.
static void layout_line(struct text_layout *t, const struct word *words,
		int count) {
	int avail = t->area.right - t->area.left;
	int x = 0;
	for (int i = 0; i < count && t->y < t->area.bottom; ++i) {
		const struct word *w = &words[i];
		int start = 0;
		while (start < w->len && t->y < t->area.bottom) {
			int len = w->len - start;
			int ink = extent(t->dc, w->font, w->text + start,
				trimmed_len(w->text + start, len));
			if (x > 0 && x + ink > avail) {
				flush_line(t);
				x = 0;
			}
			if (ink > avail - x) {
				// Too wide even alone: as many characters as fit (at
				// least one), the rest on the next line.
				int fit = 0;
				SIZE size;
				HFONT old = SelectObject(t->dc, w->font);
				GetTextExtentExPointW(t->dc, w->text + start, len,
					avail - x, &fit, NULL, &size);
				SelectObject(t->dc, old);
				if (fit < 1) {
					fit = 1;
				}
				if (fit < len) {
					place(t, w, start, fit, x);
					flush_line(t);
					x = 0;
					start += fit;
					continue;
				}
			}
			place(t, w, start, len, x);
			x += extent(t->dc, w->font, w->text + start, len);
			start = w->len;
		}
	}
	flush_line(t);
}

// Measures (and with draw, draws white into dc) the lines inside area.
// Returns the height; *width gets the widest line.
static int layout_text(const struct popup *p, HDC dc,
		const struct swine_notification *n, RECT area, bool draw, int *width) {
	list_t *lines = notify_format_lines(n);
	struct text_layout t = {
		.dc = dc,
		.area = area,
		.align = (UINT)n->style.text_alignment,
		.draw = draw,
		.y = area.top,
	};
	for (int i = 0; i < lines->length && t.y < area.bottom; ++i) {
		struct notify_line *line = lines->items[i];
		list_t *texts = create_list();   // wchar_t *, one per span
		struct word *words = NULL;
		int count = 0, cap = 0;
		for (int s = 0; s < line->spans->length; ++s) {
			const struct markup_span *span = line->spans->items[s];
			wchar_t *text = utf8_to_utf16(span->text);
			if (!text) {
				continue;
			}
			list_add(texts, text);
			HFONT font = span_font(p, &n->style, span);
			const wchar_t *c = text;
			while (*c) {
				const wchar_t *e = c;
				while (*e && *e != L' ') {
					e++;
				}
				while (*e == L' ') {
					e++;
				}
				if (count == cap) {
					cap = cap ? cap * 2 : 16;
					words = realloc(words, sizeof(*words) * (size_t)cap);
				}
				words[count++] = (struct word){c, (int)(e - c), font, span};
				c = e;
			}
		}
		layout_line(&t, words, count);
		free(words);
		list_free_items_and_destroy(texts);
	}
	free(t.placed);
	notify_free_lines(lines);
	int avail = area.right - area.left;
	if (width) {
		*width = t.widest > avail ? avail : t.widest;
	}
	int h = t.y - area.top;
	return h < area.bottom - area.top ? h : area.bottom - area.top;
}

// mako's render_notification, sizes only.
static void compute_layout(struct popup *p, const struct swine_notification *n,
		struct popup_layout *l) {
	const struct notify_style *s = &n->style;
	int b = scaled(p, s->border_size);
	int border2 = 2 * b;
	int pt = scaled(p, s->padding.top), pb = scaled(p, s->padding.bottom),
		pl = scaled(p, s->padding.left), pr = scaled(p, s->padding.right);
	bool icon = p->icon_px != NULL;
	bool vertical = s->icon_location == NOTIFY_ICON_TOP ||
		s->icon_location == NOTIFY_ICON_BOTTOM;
	int iw = icon ? p->icon_w : 0, ih = icon ? p->icon_h : 0;
	int w = scaled(p, s->width);
	int text_x = pl, text_y = pt;
	if (icon && s->icon_location == NOTIFY_ICON_LEFT) {
		text_x = iw + 2 * pl;
	}
	if (icon && s->icon_location == NOTIFY_ICON_TOP) {
		text_y = ih + 2 * pt;
	}
	int layout_w = w - border2 - pl - pr;
	if (icon && !vertical) {
		layout_w -= iw + (s->icon_location == NOTIFY_ICON_LEFT ? 2 * pl : 2 * pr);
	}
	int layout_h = scaled(p, s->height) - border2 - pt - pb;
	if (icon && vertical) {
		layout_h -= ih + (s->icon_location == NOTIFY_ICON_TOP ? 2 * pt : 2 * pb);
	}
	if (layout_w < 1) layout_w = 1;
	if (layout_h < 1) layout_h = 1;

	HDC dc = CreateCompatibleDC(NULL);
	RECT area = {0, 0, layout_w, layout_h};
	int text_w = 0;
	int text_h = layout_text(p, dc, n, area, false, &text_w);
	DeleteDC(dc);

	int h = text_h + border2 + pt + pb;
	if (icon && vertical) {
		h += ih + (s->icon_location == NOTIFY_ICON_TOP ? pt : pb);
	}
	if (icon && !vertical && ih > text_h) {
		h = ih + border2 + pt + pb;
	}
	int rtl = scaled(p, s->border_radius.top), rtr = scaled(p, s->border_radius.right),
		rbr = scaled(p, s->border_radius.bottom), rbl = scaled(p, s->border_radius.left);
	if (h < rtl + rbl) {
		h = rtl + rbl + border2;
	}
	if (h < rtr + rbr) {
		h = rtr + rbr + border2;
	}
	if (vertical) {
		text_x = (w - text_w - border2) / 2;
	} else {
		text_y = (h - text_h - border2) / 2;
	}
	l->w = w;
	l->h = h;
	l->border = b;
	l->has_icon = icon;
	if (icon) {
		int cy = b + (h - ih - border2) / 2, cx = b + (w - iw - border2) / 2;
		switch (s->icon_location) {
		case NOTIFY_ICON_LEFT: l->icon_x = b + pl; l->icon_y = cy; break;
		case NOTIFY_ICON_RIGHT: l->icon_x = w - b - pr - iw; l->icon_y = cy; break;
		case NOTIFY_ICON_TOP: l->icon_x = cx; l->icon_y = b + pt; break;
		case NOTIFY_ICON_BOTTOM: l->icon_x = cx; l->icon_y = h - b - pb - ih; break;
		}
	}
	l->text = (RECT){b + text_x, b + text_y, b + text_x + layout_w,
		b + text_y + text_h};
	if (vertical) {
		// centred: the layout keeps its full width for wrapping
		l->text.left = b + (w - layout_w - border2) / 2;
		l->text.right = l->text.left + layout_w;
	}
}

static unsigned premul(unsigned c, unsigned a) {
	return c * a / 255;
}

// Background, progress and border, as mako: the border is a band of
// border-size along the rounded edge that replaces what is beneath it.
static void draw_frame(struct popup *p, struct canvas *c,
		const struct swine_notification *n, const struct popup_layout *l) {
	const struct notify_style *s = &n->style;
	double b = l->border;
	double r[4] = {
		s->border_radius.top * p->scale, s->border_radius.right * p->scale,
		s->border_radius.bottom * p->scale, s->border_radius.left * p->scale,
	};
	double outer_r[4], inner_r[4];
	for (int i = 0; i < 4; ++i) {
		outer_r[i] = r[i] > 0 ? r[i] + b / 2 : 0;
		inner_r[i] = r[i] - b / 2 > 0 ? r[i] - b / 2 : 0;
	}
	RECT outer = {0, 0, l->w, l->h};
	RECT inner = {l->border, l->border, l->w - l->border, l->h - l->border};
	int progress_end = l->border;
	if (n->progress > 0) {
		progress_end += (l->w - 2 * l->border) * n->progress / 100;
	}
	struct swine_color bg = s->background, fg = s->progress, bd = s->border;
	// premultiplied background, and background with progress
	unsigned bg_r = premul(bg.r, bg.a), bg_g = premul(bg.g, bg.a),
		bg_b = premul(bg.b, bg.a);
	uint32_t with_bg = (uint32_t)bg.a << 24 | bg_r << 16 | bg_g << 8 | bg_b;
	uint32_t with_progress = s->progress_source ?
		((uint32_t)fg.a << 24 | premul(fg.r, fg.a) << 16 |
			premul(fg.g, fg.a) << 8 | premul(fg.b, fg.a)) :
		canvas_over(with_bg, premul(fg.r, fg.a), premul(fg.g, fg.a),
			premul(fg.b, fg.a), fg.a);
	for (int y = 0; y < l->h; ++y) {
		uint32_t *row = c->px + (size_t)y * c->w;
		for (int x = 0; x < l->w; ++x) {
			unsigned co = canvas_rounded_coverage(&outer, outer_r, x, y);
			if (!co) {
				continue;
			}
			unsigned ci = canvas_rounded_coverage(&inner, inner_r, x, y);
			if (ci > co) {
				ci = co;
			}
			uint32_t fill = x < progress_end ? with_progress : with_bg;
			unsigned pa = (fill >> 24) * ci / 255 + premul(bd.a, co - ci);
			unsigned pr = ((fill >> 16) & 0xFF) * ci / 255 +
				premul(premul(bd.r, bd.a), co - ci);
			unsigned pg = ((fill >> 8) & 0xFF) * ci / 255 +
				premul(premul(bd.g, bd.a), co - ci);
			unsigned pb = (fill & 0xFF) * ci / 255 +
				premul(premul(bd.b, bd.a), co - ci);
			row[x] = (pa > 255 ? 255 : pa) << 24 | (pr > 255 ? 255 : pr) << 16 |
				(pg > 255 ? 255 : pg) << 8 | (pb > 255 ? 255 : pb);
		}
	}
}

static void render_popup(struct popup *p, const struct swine_notification *n,
		const struct popup_layout *l, int x, int y) {
	HDC screen = GetDC(NULL);
	struct canvas c;
	bool ok = canvas_init(&c, screen, l->w, l->h);
	ReleaseDC(NULL, screen);
	if (!ok) {
		return;
	}
	draw_frame(p, &c, n, l);
	if (l->has_icon) {
		canvas_image(&c, l->icon_x, l->icon_y, p->icon_w, p->icon_h,
			p->icon_px, n->style.icon_border_radius * p->scale);
	}
	RECT inside = {l->border, l->border, l->w - l->border, l->h - l->border};
	RECT text;
	if (IntersectRect(&text, &l->text, &inside)) {
		canvas_mask_clear(&c, &text, false);
		SetTextColor(c.mask_dc, RGB(255, 255, 255));
		HRGN clip = CreateRectRgnIndirect(&text);
		SelectClipRgn(c.mask_dc, clip);
		layout_text(p, c.mask_dc, n, l->text, true, NULL);
		SelectClipRgn(c.mask_dc, NULL);
		DeleteObject(clip);
		canvas_mask_blend(&c, &text, n->style.text);
	}
	canvas_present(&c, p->hwnd, x, y);
	canvas_finish(&c);
}

// --- windows -------------------------------------------------------------

static struct popup *popup_from_hwnd(HWND hwnd) {
	return (struct popup *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static void click(HWND hwnd, int button) {
	struct popup *p = popup_from_hwnd(hwnd);
	if (p && p->id) {
		notify_button(p->id, button);
		win32_notify_refresh();
	}
}

static LRESULT CALLBACK popup_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
		LPARAM lparam) {
	switch (msg) {
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
		WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
		L"swinewm-notification", L"swinewm notification", WS_POPUP,
		0, 0, 0, 0, NULL, NULL, instance, NULL);
	SetWindowLongPtrW(p->hwnd, GWLP_USERDATA, (LONG_PTR)p);
	return p;
}

static void destroy_popup(struct popup *p) {
	DestroyWindow(p->hwnd);
	if (p->placeholder) {
		list_t *one = create_list();
		list_add(one, p->placeholder);
		notify_free_visible(one);
	}
	free(p->icon_key);
	free(p->icon_px);
	free(p);
}

static void set_placeholder(struct popup *p, struct swine_notification *n) {
	if (p->placeholder && p->placeholder != n) {
		list_t *one = create_list();
		list_add(one, p->placeholder);
		notify_free_visible(one);
	}
	p->placeholder = n;
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
		destroy_popup(popups->items[popups->length - 1]);
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
		// The popup keeps the placeholder until the next refresh.
		set_placeholder(p, n->hidden ? n : NULL);
		p->id = n->id;
		const struct notify_style *s = &n->style;
		struct swine_output *output = target_output(s);
		if (!output) {
			if (p->shown) {
				ShowWindow(p->hwnd, SW_HIDE);
				p->shown = false;
			}
			continue;
		}
		p->scale = win32_output_scale(output);
		update_icon(p, n);
		struct popup_layout l;
		compute_layout(p, n, &l);
		struct swine_box area = output_get_usable_area(output);
		int w = l.w, h = l.h;
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
		// Redrawn every time: the text, group count or progress may have
		// changed under the same id. UpdateLayeredWindow also moves it.
		render_popup(p, n, &l, x, y);
		RECT rect = {x, y, x + w, y + h};
		if (!p->shown || !EqualRect(&rect, &p->rect)) {
			SetWindowPos(p->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE |
				SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
			p->rect = rect;
			p->shown = true;
		}
	}
	// The placeholders now belong to the popups.
	list_free(visible);
}

void win32_notify_finish(void) {
	if (!popups) {
		return;
	}
	for (int i = 0; i < popups->length; ++i) {
		destroy_popup(popups->items[i]);
	}
	list_free(popups);
	popups = NULL;
}
