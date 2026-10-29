// slurp's selection (platform_slurp): a layered, topmost window over each
// output, drawn as slurp's render.c draws its surfaces: the background
// colour everywhere, predefined rectangles, the selection with its border
// and, with -d, its size. Pointer handling follows slurp's main.c (hover a
// predefined rectangle, press, drag, release; Space moves the selection,
// Shift keeps it square, Escape cancels; Copyright (c) 2018 emersion,
// MIT). The right button cancels too.
//
// Keys come from the keyboard hook (win32_slurp_key), so the overlay
// needs no keyboard focus. When the selection ends the windows go and
// DWM is flushed before the core captures the screen.
#include <dwmapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "list.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/grim.h"
#include "swinewm/platform.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "win32.h"

struct overlay {
	HWND hwnd;
	struct swine_box box;      // desktop coordinates
	struct canvas canvas;
	double scale;
	bool ready;
};

static struct {
	struct slurp_options opt;  // its choices are copied below
	struct slurp_choice *choices;
	int choice_count;
	list_t *overlays;          // struct overlay *
	bool pressed;
	bool has_selection;
	struct swine_box selection;
	const char *label;         // of the hovered choice
	int x, y;                  // the pointer
	int anchor_x, anchor_y;
	bool edit_anchor;          // Space held
	double aspect;             // height / width, 0 = free
	bool fixed_aspect;
	struct swine_box drawn;    // the selection last drawn, for redrawing
	bool drawn_valid;
} sl;

volatile bool win32_slurp_running = false;
static bool class_registered = false;

static bool in_box(const struct swine_box *b, int x, int y) {
	return x >= b->x && x < b->x + b->width && y >= b->y && y < b->y + b->height;
}

static long long box_size(const struct swine_box *b) {
	return (long long)b->width * b->height;
}

static uint32_t premultiplied(struct swine_color c) {
	return (uint32_t)c.a << 24 | (uint32_t)(c.r * c.a / 255) << 16 |
		(uint32_t)(c.g * c.a / 255) << 8 | (uint32_t)(c.b * c.a / 255);
}

// --- drawing -------------------------------------------------------------

// Replaces the pixels of r (desktop coordinates) inside clip, as cairo's
// SOURCE operator does in slurp.
static void put_rect(struct overlay *o, struct swine_box r, const RECT *clip,
		uint32_t px) {
	RECT a = {r.x - o->box.x, r.y - o->box.y,
		r.x - o->box.x + r.width, r.y - o->box.y + r.height};
	RECT b;
	if (!IntersectRect(&b, &a, clip)) {
		return;
	}
	// A fully transparent pixel would let clicks through the overlay.
	if (!(px >> 24)) {
		px = 0x01000000;
	}
	for (int y = b.top; y < b.bottom; ++y) {
		uint32_t *row = o->canvas.px + (size_t)y * o->canvas.w;
		for (int x = b.left; x < b.right; ++x) {
			row[x] = px;
		}
	}
}

// slurp's border: a stroke of border_weight centred on the rectangle.
static void put_border(struct overlay *o, struct swine_box r, const RECT *clip,
		uint32_t px) {
	int w = sl.opt.border_weight;
	if (w <= 0) {
		return;
	}
	int in = w / 2, out = w - in;
	struct swine_box top = {r.x - out, r.y - out, r.width + 2 * out, w};
	struct swine_box bottom = {r.x - out, r.y + r.height - in, r.width + 2 * out, w};
	struct swine_box left = {r.x - out, r.y - out, w, r.height + 2 * out};
	struct swine_box right = {r.x + r.width - in, r.y - out, w, r.height + 2 * out};
	put_rect(o, top, clip, px);
	put_rect(o, bottom, clip, px);
	put_rect(o, left, clip, px);
	put_rect(o, right, clip, px);
}

static void draw_dimensions(struct overlay *o, const RECT *clip) {
	wchar_t text[32];
	swprintf(text, 32, L"%ix%i", sl.selection.width, sl.selection.height);
	int px = (int)(14 * o->scale + 0.5);
	HFONT font = win32_font(sl.opt.font_family, px, false, false);
	// cairo_move_to sets the baseline, 20 below the bottom right corner
	RECT r = {sl.selection.x + sl.selection.width + 10 - o->box.x,
		sl.selection.y + sl.selection.height + 20 - px - o->box.y, 0, 0};
	r.right = r.left + 20 * px;
	r.bottom = r.top + 2 * px;
	RECT t;
	if (!IntersectRect(&t, &r, clip)) {
		return;
	}
	canvas_mask_clear(&o->canvas, &t, false);
	HGDIOBJ old = SelectObject(o->canvas.mask_dc, font);
	SetTextColor(o->canvas.mask_dc, RGB(255, 255, 255));
	DrawTextW(o->canvas.mask_dc, text, -1, &r, DT_LEFT | DT_TOP |
		DT_NOPREFIX | DT_SINGLELINE);
	SelectObject(o->canvas.mask_dc, old);
	canvas_mask_blend(&o->canvas, &t, sl.opt.border);
}

// slurp's render(), limited to clip (overlay coordinates).
static void render(struct overlay *o, RECT clip) {
	RECT whole = {0, 0, o->box.width, o->box.height};
	if (!IntersectRect(&clip, &clip, &whole)) {
		return;
	}
	struct swine_box all = o->box;
	put_rect(o, all, &clip, premultiplied(sl.opt.background));
	for (int i = 0; i < sl.choice_count; ++i) {
		put_rect(o, sl.choices[i].box, &clip, premultiplied(sl.opt.choice));
	}
	uint32_t border = premultiplied(sl.opt.border);
	if (!sl.has_selection && sl.opt.crosshairs && in_box(&o->box, sl.x, sl.y)) {
		struct swine_box h = {o->box.x, sl.y, o->box.width, 1};
		struct swine_box v = {sl.x, o->box.y, 1, o->box.height};
		put_rect(o, h, &clip, border);
		put_rect(o, v, &clip, border);
	}
	if (sl.has_selection) {
		put_rect(o, sl.selection, &clip, premultiplied(sl.opt.selection));
		put_border(o, sl.selection, &clip, border);
		if (sl.opt.dimensions) {
			draw_dimensions(o, &clip);
		}
	}
	POINT pos = {o->box.x, o->box.y};
	SIZE size = {o->box.width, o->box.height};
	POINT origin = {0, 0};
	BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
	UPDATELAYEREDWINDOWINFO info = {
		.cbSize = sizeof(info), .pptDst = &pos, .psize = &size,
		.hdcSrc = o->canvas.dc, .pptSrc = &origin, .pblend = &blend,
		.dwFlags = ULW_ALPHA, .prcDirty = &clip,
	};
	if (!o->ready) {
		info.prcDirty = NULL;   // the first time, all of it
		o->ready = true;
	}
	UpdateLayeredWindowIndirect(o->hwnd, &info);
}

// The area a selection covers on screen: its border and size text too.
static RECT selection_extent(const struct overlay *o, struct swine_box s) {
	int margin = sl.opt.border_weight + 2;
	RECT r = {s.x - o->box.x - margin, s.y - o->box.y - margin,
		s.x - o->box.x + s.width + margin, s.y - o->box.y + s.height + margin};
	if (sl.opt.dimensions) {
		r.right += 400;
		r.bottom += 80;
	}
	return r;
}

// Redraws what changed since the last frame.
static void refresh(void) {
	bool full = sl.opt.crosshairs || !sl.drawn_valid || !sl.has_selection;
	for (int i = 0; sl.overlays && i < sl.overlays->length; ++i) {
		struct overlay *o = sl.overlays->items[i];
		RECT clip = {0, 0, o->box.width, o->box.height};
		if (!full && o->ready) {
			RECT now = selection_extent(o, sl.selection);
			RECT before = selection_extent(o, sl.drawn);
			UnionRect(&clip, &now, &before);
		}
		render(o, clip);
	}
	sl.drawn = sl.selection;
	sl.drawn_valid = sl.has_selection;
}

// --- selection (slurp's main.c) ------------------------------------------

// The result, handed to the core once the overlay is off the screen.
static struct {
	bool ok;
	struct swine_box box;
	char *label;
	UINT_PTR timer;
} result;

static void deliver(void) {
	if (result.timer) {
		KillTimer(NULL, result.timer);
		result.timer = 0;
	}
	char *label = result.label;
	result.label = NULL;
	grim_slurp_done(result.ok, result.ok ? &result.box : NULL, label);
	free(label);
	platform_ui_refresh();
}

static VOID CALLBACK deliver_timer(HWND hwnd, UINT msg, UINT_PTR id,
		DWORD time) {
	(void)hwnd;
	(void)msg;
	(void)id;
	(void)time;
	deliver();
}

static void finish(bool ok) {
	struct swine_box box = sl.selection;
	const char *label = sl.label;
	char *label_copy = label ? swine_strdup(label) : NULL;
	for (int i = 0; sl.overlays && i < sl.overlays->length; ++i) {
		struct overlay *o = sl.overlays->items[i];
		DestroyWindow(o->hwnd);
		canvas_finish(&o->canvas);
		free(o);
	}
	if (sl.overlays) {
		list_free(sl.overlays);
	}
	for (int i = 0; i < sl.choice_count; ++i) {
		free(sl.choices[i].label);
	}
	free(sl.choices);
	memset(&sl, 0, sizeof(sl));
	win32_slurp_running = false;
	ReleaseCapture();
	// The overlay must be gone from the screen before it is captured: let
	// the windows below repaint and DWM compose a frame first.
	DwmFlush();
	result.ok = ok;
	result.box = box;
	free(result.label);
	result.label = label_copy;
	result.timer = ok ? SetTimer(NULL, 0, 150, deliver_timer) : 0;
	if (!result.timer) {
		deliver();
	}
}

static void update_hover(void) {
	sl.has_selection = false;
	sl.label = NULL;
	// the smallest rectangle under the pointer
	for (int i = 0; i < sl.choice_count; ++i) {
		struct slurp_choice *c = &sl.choices[i];
		if (in_box(&c->box, sl.x, sl.y) && (!sl.has_selection ||
				box_size(&c->box) < box_size(&sl.selection))) {
			sl.selection = c->box;
			sl.label = c->label;
			sl.has_selection = true;
		}
	}
}

static void active_motion(void) {
	if (sl.opt.restrict_choice) {
		return;
	}
	int dist_x = sl.x - sl.anchor_x, dist_y = sl.y - sl.anchor_y;
	int width = abs(dist_x) + 1, height = abs(dist_y) + 1;
	if (sl.aspect > 0) {
		width = width > (int)(height / sl.aspect) ? width : (int)(height / sl.aspect);
		height = height > (int)(width * sl.aspect) ? height : (int)(width * sl.aspect);
	}
	sl.has_selection = true;
	sl.label = NULL;
	sl.selection.x = dist_x > 0 ? sl.anchor_x : sl.anchor_x - (width - 1);
	sl.selection.y = dist_y > 0 ? sl.anchor_y : sl.anchor_y - (height - 1);
	sl.selection.width = width;
	sl.selection.height = height;
}

static void pointer_moved(int x, int y) {
	if (sl.edit_anchor && sl.pressed) {
		sl.anchor_x += x - sl.x;
		sl.anchor_y += y - sl.y;
	}
	sl.x = x;
	sl.y = y;
	if (sl.pressed) {
		active_motion();
	} else {
		update_hover();
	}
	refresh();
}

static void pressed(void) {
	if (sl.opt.single_point) {
		sl.selection = (struct swine_box){sl.x, sl.y, 1, 1};
		finish(true);
	} else if (sl.opt.restrict_choice) {
		if (sl.has_selection) {
			finish(true);
		}
	} else {
		sl.pressed = true;
		sl.anchor_x = sl.x;
		sl.anchor_y = sl.y;
	}
}

static void released(void) {
	if (!sl.pressed) {
		return;
	}
	sl.pressed = false;
	if (!sl.has_selection) {
		sl.selection = (struct swine_box){sl.x, sl.y, 1, 1};
	}
	finish(true);
}

void win32_slurp_key(WPARAM vk, LPARAM down) {
	if (!win32_slurp_running) {
		return;
	}
	if (vk == VK_ESCAPE && down) {
		finish(false);
	} else if (vk == VK_SPACE) {
		sl.edit_anchor = down != 0;
	} else if ((vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) &&
			!sl.fixed_aspect) {
		sl.aspect = down ? 1 : 0;
		if (sl.pressed) {
			active_motion();
			refresh();
		}
	}
}

static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wparam,
		LPARAM lparam) {
	if (!win32_slurp_running) {
		return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
	switch (msg) {
	case WM_SETCURSOR:
		SetCursor(LoadCursor(NULL, IDC_CROSS));
		return TRUE;
	case WM_MOUSEMOVE: {
		POINT pt;
		GetCursorPos(&pt);   // desktop coordinates, whichever window
		pointer_moved(pt.x, pt.y);
		return 0;
	}
	case WM_LBUTTONDOWN:
		SetCapture(hwnd);
		pressed();
		return 0;
	case WM_LBUTTONUP:
		released();
		return 0;
	case WM_RBUTTONDOWN:
		finish(false);
		return 0;
	case WM_KEYDOWN:
	case WM_KEYUP:
		win32_slurp_key(wparam, msg == WM_KEYDOWN);
		return 0;
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	default:
		return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
}

bool platform_slurp(const struct slurp_options *options) {
	if (win32_slurp_running || result.timer) {
		return false;
	}
	HINSTANCE instance = GetModuleHandleW(NULL);
	if (!class_registered) {
		WNDCLASSEXW wc = { .cbSize = sizeof(wc) };
		wc.lpfnWndProc = overlay_proc;
		wc.hInstance = instance;
		wc.hCursor = LoadCursor(NULL, IDC_CROSS);
		wc.lpszClassName = L"swinewm-slurp";
		RegisterClassExW(&wc);
		class_registered = true;
	}
	memset(&sl, 0, sizeof(sl));
	sl.opt = *options;
	sl.opt.choices = NULL;
	if (options->choices && options->choices->length) {
		sl.choice_count = options->choices->length;
		sl.choices = calloc((size_t)sl.choice_count, sizeof(*sl.choices));
		for (int i = 0; i < sl.choice_count; ++i) {
			struct slurp_choice *c = options->choices->items[i];
			sl.choices[i].box = c->box;
			sl.choices[i].label = c->label ? swine_strdup(c->label) : NULL;
		}
	}
	if (options->aspect_w > 0 && options->aspect_h > 0) {
		sl.aspect = (double)options->aspect_h / options->aspect_w;
		sl.fixed_aspect = true;
	}
	POINT pt;
	GetCursorPos(&pt);
	sl.x = pt.x;
	sl.y = pt.y;
	win32_slurp_running = true;
	sl.overlays = create_list();
	HDC screen = GetDC(NULL);
	for (int i = 0; i < root->outputs->length; ++i) {
		struct swine_output *output = root->outputs->items[i];
		if (!output->enabled || output->box.width <= 0) {
			continue;
		}
		struct overlay *o = calloc(1, sizeof(*o));
		o->box = output->box;
		o->scale = win32_output_scale(output);
		if (!canvas_init(&o->canvas, screen, o->box.width, o->box.height)) {
			free(o);
			continue;
		}
		o->hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
			WS_EX_LAYERED | WS_EX_NOACTIVATE, L"swinewm-slurp", L"slurp",
			WS_POPUP, o->box.x, o->box.y, o->box.width, o->box.height,
			NULL, NULL, instance, NULL);
		list_add(sl.overlays, o);
	}
	ReleaseDC(NULL, screen);
	if (!sl.overlays->length) {
		finish(false);
		return false;
	}
	update_hover();
	refresh();
	for (int i = 0; i < sl.overlays->length; ++i) {
		struct overlay *o = sl.overlays->items[i];
		SetWindowPos(o->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE |
			SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
	}
	return true;
}

void win32_slurp_finish(void) {
	if (win32_slurp_running) {
		finish(false);
	}
	if (result.timer) {
		result.ok = false;
		deliver();
	}
}
