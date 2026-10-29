// Drawing with per-pixel alpha for layered windows (the bar, notification
// popups). GDI ignores alpha, so shapes are filled here directly into a
// premultiplied 32-bit bitmap; text is drawn white on black into a mask
// and blended by its coverage; icons are drawn over black and over white,
// which gives their colour and alpha.
#include <math.h>
#include <string.h>
#include "win32.h"

HBITMAP win32_dib(HDC ref, int w, int h, uint32_t **bits) {
	BITMAPINFO bi = {0};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;   // top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void *data = NULL;
	HBITMAP bitmap = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &data, NULL, 0);
	*bits = data;
	return bitmap;
}

bool canvas_init(struct canvas *c, HDC ref, int w, int h) {
	memset(c, 0, sizeof(*c));
	c->w = w;
	c->h = h;
	c->dc = CreateCompatibleDC(ref);
	c->mask_dc = CreateCompatibleDC(ref);
	c->white_dc = CreateCompatibleDC(ref);
	c->bitmap = win32_dib(ref, w, h, &c->px);
	c->mask_bitmap = win32_dib(ref, w, h, &c->mask);
	c->white_bitmap = win32_dib(ref, w, h, &c->white);
	if (!c->bitmap || !c->mask_bitmap || !c->white_bitmap) {
		canvas_finish(c);
		return false;
	}
	c->old[0] = SelectObject(c->dc, c->bitmap);
	c->old[1] = SelectObject(c->mask_dc, c->mask_bitmap);
	c->old[2] = SelectObject(c->white_dc, c->white_bitmap);
	SetBkMode(c->mask_dc, TRANSPARENT);
	memset(c->px, 0, sizeof(uint32_t) * (size_t)w * (size_t)h);
	return true;
}

void canvas_finish(struct canvas *c) {
	if (c->old[0]) {
		SelectObject(c->dc, c->old[0]);
		SelectObject(c->mask_dc, c->old[1]);
		SelectObject(c->white_dc, c->old[2]);
	}
	if (c->bitmap) DeleteObject(c->bitmap);
	if (c->mask_bitmap) DeleteObject(c->mask_bitmap);
	if (c->white_bitmap) DeleteObject(c->white_bitmap);
	if (c->dc) DeleteDC(c->dc);
	if (c->mask_dc) DeleteDC(c->mask_dc);
	if (c->white_dc) DeleteDC(c->white_dc);
	memset(c, 0, sizeof(*c));
}

bool canvas_clip(const struct canvas *c, const RECT *r, RECT *out) {
	out->left = r->left < 0 ? 0 : r->left;
	out->top = r->top < 0 ? 0 : r->top;
	out->right = r->right > c->w ? c->w : r->right;
	out->bottom = r->bottom > c->h ? c->h : r->bottom;
	return out->left < out->right && out->top < out->bottom;
}

uint32_t canvas_over(uint32_t dst, unsigned r, unsigned g, unsigned b,
		unsigned a) {
	unsigned keep = 255 - a;
	unsigned db = dst & 0xFF, dg = (dst >> 8) & 0xFF, dr = (dst >> 16) & 0xFF,
		da = dst >> 24;
	return (r + dr * keep / 255) << 16 | (g + dg * keep / 255) << 8 |
		(b + db * keep / 255) | (a + da * keep / 255) << 24;
}

void canvas_fill(struct canvas *c, const RECT *rect, struct swine_color color) {
	RECT r;
	if (!color.a || !canvas_clip(c, rect, &r)) {
		return;
	}
	unsigned a = color.a;
	unsigned pr = color.r * a / 255, pg = color.g * a / 255,
		pb = color.b * a / 255;
	for (int y = r.top; y < r.bottom; ++y) {
		uint32_t *row = c->px + (size_t)y * c->w;
		for (int x = r.left; x < r.right; ++x) {
			row[x] = canvas_over(row[x], pr, pg, pb, a);
		}
	}
}

void canvas_mask_clear(struct canvas *c, const RECT *rect, bool white) {
	RECT r;
	if (!canvas_clip(c, rect, &r)) {
		return;
	}
	for (int y = r.top; y < r.bottom; ++y) {
		memset(c->mask + (size_t)y * c->w + r.left, 0,
			sizeof(uint32_t) * (size_t)(r.right - r.left));
		if (white) {
			memset(c->white + (size_t)y * c->w + r.left, 0xFF,
				sizeof(uint32_t) * (size_t)(r.right - r.left));
		}
	}
}

void canvas_mask_blend(struct canvas *c, const RECT *rect,
		struct swine_color color) {
	RECT r;
	if (!canvas_clip(c, rect, &r)) {
		return;
	}
	GdiFlush();
	for (int y = r.top; y < r.bottom; ++y) {
		uint32_t *row = c->px + (size_t)y * c->w;
		const uint32_t *m = c->mask + (size_t)y * c->w;
		for (int x = r.left; x < r.right; ++x) {
			unsigned v = m[x];
			// ClearType draws per channel; their mean is the coverage.
			unsigned cover = ((v & 0xFF) + ((v >> 8) & 0xFF) +
				((v >> 16) & 0xFF)) / 3;
			if (!cover) {
				continue;
			}
			unsigned a = cover * color.a / 255;
			row[x] = canvas_over(row[x], color.r * a / 255, color.g * a / 255,
				color.b * a / 255, a);
		}
	}
}

void canvas_icon_blend(struct canvas *c, const RECT *rect) {
	RECT r;
	if (!canvas_clip(c, rect, &r)) {
		return;
	}
	GdiFlush();
	for (int y = r.top; y < r.bottom; ++y) {
		uint32_t *row = c->px + (size_t)y * c->w;
		const uint32_t *k = c->mask + (size_t)y * c->w;
		const uint32_t *wh = c->white + (size_t)y * c->w;
		for (int x = r.left; x < r.right; ++x) {
			// Over black the icon is colour * alpha; white shows the rest.
			unsigned kb = k[x] & 0xFF, kg = (k[x] >> 8) & 0xFF,
				kr = (k[x] >> 16) & 0xFF;
			unsigned wb = wh[x] & 0xFF, wg = (wh[x] >> 8) & 0xFF,
				wr = (wh[x] >> 16) & 0xFF;
			int through = ((int)(wr - kr) + (int)(wg - kg) + (int)(wb - kb)) / 3;
			unsigned a = through < 0 ? 255 : through > 255 ? 0 :
				255 - (unsigned)through;
			if (!a) {
				continue;
			}
			row[x] = canvas_over(row[x], kr < a ? kr : a, kg < a ? kg : a,
				kb < a ? kb : a, a);
		}
	}
}

// How much of pixel (x, y) lies inside the rounded rectangle, 0-255.
// radii: top-left, top-right, bottom-right, bottom-left.
unsigned canvas_rounded_coverage(const RECT *r, const double radii[4],
		int x, int y) {
	double px = x + 0.5, py = y + 0.5;
	if (px < r->left || px > r->right || py < r->top || py > r->bottom) {
		return 0;
	}
	double cx = 0, cy = 0, radius = 0;
	if (px < r->left + radii[0] && py < r->top + radii[0]) {
		radius = radii[0];
		cx = r->left + radius;
		cy = r->top + radius;
	} else if (px > r->right - radii[1] && py < r->top + radii[1]) {
		radius = radii[1];
		cx = r->right - radius;
		cy = r->top + radius;
	} else if (px > r->right - radii[2] && py > r->bottom - radii[2]) {
		radius = radii[2];
		cx = r->right - radius;
		cy = r->bottom - radius;
	} else if (px < r->left + radii[3] && py > r->bottom - radii[3]) {
		radius = radii[3];
		cx = r->left + radius;
		cy = r->bottom - radius;
	} else {
		return 255;
	}
	double d = sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
	double cover = radius - d + 0.5;
	return cover <= 0 ? 0 : cover >= 1 ? 255 : (unsigned)(cover * 255);
}

void canvas_box(struct canvas *c, const RECT *r, double radius,
		struct swine_color background, int border,
		struct swine_color border_color) {
	RECT area;
	if (!canvas_clip(c, r, &area)) {
		return;
	}
	if (radius > (r->right - r->left) / 2.0) {
		radius = (r->right - r->left) / 2.0;
	}
	if (radius > (r->bottom - r->top) / 2.0) {
		radius = (r->bottom - r->top) / 2.0;
	}
	double outer_r[4] = {radius, radius, radius, radius};
	double in = radius - border > 0 ? radius - border : 0;
	double inner_r[4] = {in, in, in, in};
	RECT inner = {r->left + border, r->top + border, r->right - border,
		r->bottom - border};
	unsigned ba = background.a, ea = border_color.a;
	for (int y = area.top; y < area.bottom; ++y) {
		uint32_t *row = c->px + (size_t)y * c->w;
		for (int x = area.left; x < area.right; ++x) {
			unsigned co = radius > 0 ? canvas_rounded_coverage(r, outer_r, x, y) : 255;
			if (!co) {
				continue;
			}
			unsigned ci = border <= 0 ? co :
				canvas_rounded_coverage(&inner, inner_r, x, y);
			if (ci > co) {
				ci = co;
			}
			// the background inside, the border around it
			unsigned a = (ba * ci + ea * (co - ci)) / 255;
			if (!a) {
				continue;
			}
			unsigned pr = (background.r * ba / 255 * ci +
				border_color.r * ea / 255 * (co - ci)) / 255;
			unsigned pg = (background.g * ba / 255 * ci +
				border_color.g * ea / 255 * (co - ci)) / 255;
			unsigned pb = (background.b * ba / 255 * ci +
				border_color.b * ea / 255 * (co - ci)) / 255;
			row[x] = canvas_over(row[x], pr, pg, pb, a);
		}
	}
}

void canvas_image(struct canvas *c, int x, int y, int w, int h,
		const uint32_t *src, double radius) {
	RECT area = {x, y, x + w, y + h};
	RECT r;
	if (!src || !canvas_clip(c, &area, &r)) {
		return;
	}
	double radii[4] = {radius, radius, radius, radius};
	for (int py = r.top; py < r.bottom; ++py) {
		uint32_t *row = c->px + (size_t)py * c->w;
		const uint32_t *s = src + (size_t)(py - y) * w;
		for (int px = r.left; px < r.right; ++px) {
			uint32_t v = s[px - x];
			unsigned cover = radius > 0 ?
				canvas_rounded_coverage(&area, radii, px, py) : 255;
			unsigned a = (v >> 24) * cover / 255;
			if (!a) {
				continue;
			}
			unsigned sr = ((v >> 16) & 0xFF) * cover / 255,
				sg = ((v >> 8) & 0xFF) * cover / 255,
				sb = (v & 0xFF) * cover / 255;
			row[px] = canvas_over(row[px], sr, sg, sb, a);
		}
	}
}

void canvas_present(struct canvas *c, HWND hwnd, int x, int y) {
	HDC screen = GetDC(NULL);
	POINT pos = {x, y};
	SIZE size = {c->w, c->h};
	POINT origin = {0, 0};
	BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
	UpdateLayeredWindow(hwnd, screen, &pos, &size, c->dc, &origin, 0,
		&blend, ULW_ALPHA);
	ReleaseDC(NULL, screen);
}
