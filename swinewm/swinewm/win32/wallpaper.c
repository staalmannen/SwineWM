// Wallpapers (swaybg): "output <name> bg <file> <mode> [<fallback>]".
//
// Each output gets a window at the bottom of the stacking order showing the
// image, decoded and scaled with the Windows Imaging Component. When
// Explorer runs, its desktop windows (Progman/WorkerW) are at the very
// bottom; the wallpaper window is kept directly above them and Explorer's
// own windows are never moved, so quitting swinewm leaves the normal
// desktop untouched.
#define COBJMACROS
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wincodec.h>
#include "list.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/bar/css.h"
#include "swinewm/config.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "win32.h"

struct wallpaper_window {
	HWND hwnd;
	char *output_name;
	// What the current bitmap was made from
	char *path;
	char *mode;
	char *fallback;
	int width, height;
	HBITMAP bitmap;
};

static list_t *windows = NULL;
static bool class_registered = false;
static IWICImagingFactory *factory = NULL;

static bool is_desktop_window(HWND hwnd) {
	wchar_t cls[32];
	if (!GetClassNameW(hwnd, cls, 32)) {
		return false;
	}
	return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0;
}

static bool is_wallpaper_window(HWND hwnd) {
	wchar_t cls[32];
	return GetClassNameW(hwnd, cls, 32) &&
		wcscmp(cls, L"swinewm-wallpaper") == 0;
}

// The window our wallpapers must sit directly above: the highest of the
// desktop windows at the bottom of the Z order, or NULL without Explorer.
static HWND desktop_top(void) {
	HWND top_window = GetTopWindow(NULL);
	if (!top_window) {
		return NULL;
	}
	HWND found = NULL;
	for (HWND w = GetWindow(top_window, GW_HWNDLAST); w;
			w = GetWindow(w, GW_HWNDPREV)) {
		if (!IsWindowVisible(w) || is_wallpaper_window(w)) {
			continue;
		}
		if (!is_desktop_window(w)) {
			break;
		}
		found = w;
	}
	return found;
}

// True if only invisible windows, other wallpapers and the desktop are below.
static bool placed_correctly(HWND hwnd) {
	for (HWND w = GetWindow(hwnd, GW_HWNDNEXT); w; w = GetWindow(w, GW_HWNDNEXT)) {
		if (!IsWindowVisible(w) || is_wallpaper_window(w)) {
			continue;
		}
		return is_desktop_window(w);
	}
	return true;
}

static void place_at_bottom(struct wallpaper_window *ww,
		const struct swine_box *box) {
	HWND after = HWND_BOTTOM;
	HWND desktop = desktop_top();
	if (desktop) {
		HWND above = GetWindow(desktop, GW_HWNDPREV);
		after = above && above != ww->hwnd ? above : HWND_BOTTOM;
		if (above == ww->hwnd) {
			after = NULL;   // already directly above the desktop
		}
	}
	UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW | (after ? 0 : SWP_NOZORDER);
	SetWindowPos(ww->hwnd, after, box->x, box->y, box->width, box->height,
		flags);
}

static bool ensure_factory(void) {
	if (factory) {
		return true;
	}
	HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL,
		CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory);
	if (FAILED(hr)) {
		swine_log(SWINE_ERROR, "Unable to create the WIC imaging factory "
			"(0x%08lx)", (unsigned long)hr);
		factory = NULL;
		return false;
	}
	return true;
}

static HBITMAP create_dib(int width, int height, void **bits) {
	BITMAPINFO bmi = {0};
	bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
	bmi.bmiHeader.biWidth = width;
	bmi.bmiHeader.biHeight = -height;   // top-down
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;
	return CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, bits, NULL, 0);
}

// Decodes path and scales it to width x height (premultiplied BGRA).
static HBITMAP load_scaled(const char *path, int *width, int *height,
		double scale) {
	if (!ensure_factory()) {
		return NULL;
	}
	wchar_t *wpath = utf8_to_utf16(path);
	IWICBitmapDecoder *decoder = NULL;
	IWICBitmapFrameDecode *frame = NULL;
	IWICFormatConverter *converter = NULL;
	IWICBitmapScaler *scaler = NULL;
	HBITMAP result = NULL;
	HRESULT hr = IWICImagingFactory_CreateDecoderFromFilename(factory, wpath,
		NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
	free(wpath);
	if (FAILED(hr)) {
		swine_log(SWINE_ERROR, "Unable to load wallpaper '%s' (0x%08lx)", path,
			(unsigned long)hr);
		return NULL;
	}
	UINT src_w = 0, src_h = 0;
	if (FAILED(IWICBitmapDecoder_GetFrame(decoder, 0, &frame)) ||
			FAILED(IWICImagingFactory_CreateFormatConverter(factory, &converter)) ||
			FAILED(IWICFormatConverter_Initialize(converter,
				(IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppPBGRA,
				WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)) ||
			FAILED(IWICBitmapSource_GetSize((IWICBitmapSource *)converter,
				&src_w, &src_h))) {
		goto out;
	}
	int w = (int)(src_w * scale + 0.5), h = (int)(src_h * scale + 0.5);
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	IWICBitmapSource *source = (IWICBitmapSource *)converter;
	if ((UINT)w != src_w || (UINT)h != src_h) {
		if (FAILED(IWICImagingFactory_CreateBitmapScaler(factory, &scaler)) ||
				FAILED(IWICBitmapScaler_Initialize(scaler, source, (UINT)w,
					(UINT)h, WICBitmapInterpolationModeFant))) {
			goto out;
		}
		source = (IWICBitmapSource *)scaler;
	}
	void *bits = NULL;
	result = create_dib(w, h, &bits);
	if (result && FAILED(IWICBitmapSource_CopyPixels(source, NULL, (UINT)w * 4,
			(UINT)w * 4 * (UINT)h, bits))) {
		DeleteObject(result);
		result = NULL;
	}
	*width = w;
	*height = h;
out:
	if (scaler) IWICBitmapScaler_Release(scaler);
	if (converter) IWICFormatConverter_Release(converter);
	if (frame) IWICBitmapFrameDecode_Release(frame);
	if (decoder) IWICBitmapDecoder_Release(decoder);
	return result;
}

static bool image_size(const char *path, UINT *w, UINT *h) {
	if (!ensure_factory()) {
		return false;
	}
	wchar_t *wpath = utf8_to_utf16(path);
	IWICBitmapDecoder *decoder = NULL;
	IWICBitmapFrameDecode *frame = NULL;
	bool ok = SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(factory,
			wpath, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
		SUCCEEDED(IWICBitmapDecoder_GetFrame(decoder, 0, &frame)) &&
		SUCCEEDED(IWICBitmapFrameDecode_GetSize(frame, w, h));
	if (frame) IWICBitmapFrameDecode_Release(frame);
	if (decoder) IWICBitmapDecoder_Release(decoder);
	free(wpath);
	return ok;
}

static void blend(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy) {
	BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
	AlphaBlend(dst, x, y, w, h, src, sx, sy, w, h, bf);
}

// Renders the wallpaper for a window of width x height.
static HBITMAP compose(const struct output_config *oc, int width, int height) {
	void *bits = NULL;
	HBITMAP target = create_dib(width, height, &bits);
	if (!target) {
		return NULL;
	}
	HDC dc = CreateCompatibleDC(NULL);
	HBITMAP old = SelectObject(dc, target);
	struct swine_color color = {0, 0, 0, 255};
	bool solid = strcmp(oc->background_mode, "solid_color") == 0;
	const char *color_text = solid ? oc->background : oc->background_fallback;
	if (color_text) {
		css_parse_color(NULL, color_text, &color);
	}
	RECT all = {0, 0, width, height};
	win32_fill(dc, &all, RGB(color.r, color.g, color.b));

	UINT src_w = 0, src_h = 0;
	if (!solid && image_size(oc->background, &src_w, &src_h) && src_w && src_h) {
		const char *mode = oc->background_mode;
		double sx = (double)width / src_w, sy = (double)height / src_h;
		double scale = strcmp(mode, "fill") == 0 ? (sx > sy ? sx : sy) :
			strcmp(mode, "fit") == 0 ? (sx < sy ? sx : sy) : 1.0;
		int w = 0, h = 0;
		HBITMAP image;
		if (strcmp(mode, "stretch") == 0) {
			// Scale each axis independently: load at 1:1, stretch with GDI.
			image = load_scaled(oc->background, &w, &h, 1.0);
			if (image) {
				HDC idc = CreateCompatibleDC(NULL);
				HBITMAP iold = SelectObject(idc, image);
				SetStretchBltMode(dc, HALFTONE);
				BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
				AlphaBlend(dc, 0, 0, width, height, idc, 0, 0, w, h, bf);
				SelectObject(idc, iold);
				DeleteDC(idc);
				DeleteObject(image);
			}
		} else if ((image = load_scaled(oc->background, &w, &h, scale))) {
			HDC idc = CreateCompatibleDC(NULL);
			HBITMAP iold = SelectObject(idc, image);
			if (strcmp(mode, "tile") == 0) {
				for (int y = 0; y < height; y += h) {
					for (int x = 0; x < width; x += w) {
						int bw = x + w > width ? width - x : w;
						int bh = y + h > height ? height - y : h;
						blend(dc, x, y, bw, bh, idc, 0, 0);
					}
				}
			} else {
				// fill, fit, center: centre the image, cropping if larger
				int dx = (width - w) / 2, dy = (height - h) / 2;
				int src_x = dx < 0 ? -dx : 0, src_y = dy < 0 ? -dy : 0;
				int dst_x = dx < 0 ? 0 : dx, dst_y = dy < 0 ? 0 : dy;
				int bw = w - src_x, bh = h - src_y;
				if (bw > width) bw = width;
				if (bh > height) bh = height;
				blend(dc, dst_x, dst_y, bw, bh, idc, src_x, src_y);
			}
			SelectObject(idc, iold);
			DeleteDC(idc);
			DeleteObject(image);
		}
	}
	SelectObject(dc, old);
	DeleteDC(dc);
	return target;
}

static LRESULT CALLBACK wallpaper_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
		LPARAM lparam) {
	struct wallpaper_window *ww =
		(struct wallpaper_window *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	switch (msg) {
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		if (ww && ww->bitmap) {
			HDC mem = CreateCompatibleDC(dc);
			HBITMAP old = SelectObject(mem, ww->bitmap);
			BitBlt(dc, 0, 0, ww->width, ww->height, mem, 0, 0, SRCCOPY);
			SelectObject(mem, old);
			DeleteDC(mem);
		}
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_ERASEBKGND:
		return 1;
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	default:
		return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
}

static void destroy_window(struct wallpaper_window *ww) {
	DestroyWindow(ww->hwnd);
	if (ww->bitmap) {
		DeleteObject(ww->bitmap);
	}
	free(ww->output_name);
	free(ww->path);
	free(ww->mode);
	free(ww->fallback);
	free(ww);
}

static struct wallpaper_window *find_window(const char *output_name) {
	for (int i = 0; i < windows->length; ++i) {
		struct wallpaper_window *ww = windows->items[i];
		if (strcmp(ww->output_name, output_name) == 0) {
			return ww;
		}
	}
	return NULL;
}

static bool same(const char *a, const char *b) {
	return (!a && !b) || (a && b && strcmp(a, b) == 0);
}

static struct wallpaper_window *create_window(const char *output_name) {
	HINSTANCE instance = GetModuleHandleW(NULL);
	if (!class_registered) {
		WNDCLASSEXW wc = { .cbSize = sizeof(wc) };
		wc.lpfnWndProc = wallpaper_wndproc;
		wc.hInstance = instance;
		wc.hCursor = LoadCursor(NULL, IDC_ARROW);
		wc.lpszClassName = L"swinewm-wallpaper";
		RegisterClassExW(&wc);
		class_registered = true;
	}
	struct wallpaper_window *ww = calloc(1, sizeof(*ww));
	ww->output_name = swine_strdup(output_name);
	ww->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		L"swinewm-wallpaper", L"swinewm wallpaper", WS_POPUP, 0, 0, 0, 0,
		NULL, NULL, instance, NULL);
	SetWindowLongPtrW(ww->hwnd, GWLP_USERDATA, (LONG_PTR)ww);
	return ww;
}

void win32_wallpaper_refresh(void) {
	if (!windows) {
		windows = create_list();
	}
	// Drop windows for outputs that are gone or have no background.
	for (int i = windows->length - 1; i >= 0; --i) {
		struct wallpaper_window *ww = windows->items[i];
		struct output_config *oc = output_config_for_name(ww->output_name);
		if (!output_by_name(ww->output_name) || !oc || !oc->background) {
			destroy_window(ww);
			list_del(windows, i);
		}
	}
	for (int i = 0; i < root->outputs->length; ++i) {
		struct swine_output *output = root->outputs->items[i];
		struct output_config *oc = output_config_for_name(output->name);
		if (!oc || !oc->background) {
			continue;
		}
		struct wallpaper_window *ww = find_window(output->name);
		bool fresh = false;
		if (!ww) {
			ww = create_window(output->name);
			list_add(windows, ww);
			fresh = true;
		}
		const struct swine_box *box = &output->box;
		if (fresh || !same(ww->path, oc->background) ||
				!same(ww->mode, oc->background_mode) ||
				!same(ww->fallback, oc->background_fallback) ||
				ww->width != box->width || ww->height != box->height) {
			if (ww->bitmap) {
				DeleteObject(ww->bitmap);
			}
			ww->bitmap = compose(oc, box->width, box->height);
			free(ww->path);
			free(ww->mode);
			free(ww->fallback);
			ww->path = swine_strdup(oc->background);
			ww->mode = swine_strdup(oc->background_mode);
			ww->fallback = swine_strdup(oc->background_fallback);
			ww->width = box->width;
			ww->height = box->height;
			place_at_bottom(ww, box);
			InvalidateRect(ww->hwnd, NULL, FALSE);
		} else if (!placed_correctly(ww->hwnd)) {
			place_at_bottom(ww, box);
		}
	}
}

void win32_wallpaper_finish(void) {
	if (windows) {
		for (int i = 0; i < windows->length; ++i) {
			destroy_window(windows->items[i]);
		}
		list_free(windows);
		windows = NULL;
	}
	if (factory) {
		IWICImagingFactory_Release(factory);
		factory = NULL;
	}
}
