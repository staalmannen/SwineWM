// Screenshots for the built-in grim: copy the screen with GDI and encode
// it with the Windows Imaging Component.
#define COBJMACROS
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wincodec.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/platform.h"
#include "win32.h"
#include <shlobj.h>

#define MAX_SCREENSHOT_PIXELS 100000000.0

// FOLDERID_Pictures, spelled out to avoid depending on knownfolders' GUID
// definitions being linked in.
static const GUID folderid_pictures = {0x33E28130, 0x4E1E, 0x4676,
	{0x83, 0x5A, 0x98, 0x39, 0x5C, 0x3B, 0xC3, 0xBB}};

char *platform_pictures_dir(void) {
	PWSTR path = NULL;
	char *result = NULL;
	if (SUCCEEDED(SHGetKnownFolderPath(&folderid_pictures, 0, NULL, &path))) {
		result = utf16_to_utf8(path);
	}
	CoTaskMemFree(path);
	return result;
}

// Encodes bitmap as PNG or JPEG into stream (a file or memory).
static HRESULT encode(HBITMAP bitmap, IStream *stream, bool jpeg,
		int jpeg_quality) {
	IWICImagingFactory *factory = NULL;
	IWICBitmap *source = NULL;
	IWICBitmapEncoder *encoder = NULL;
	IWICBitmapFrameEncode *frame = NULL;
	IPropertyBag2 *options = NULL;
	HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL,
		CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory);
	if (SUCCEEDED(hr)) {
		hr = IWICImagingFactory_CreateBitmapFromHBITMAP(factory, bitmap, NULL,
			WICBitmapIgnoreAlpha, &source);
	}
	if (SUCCEEDED(hr)) {
		hr = IWICImagingFactory_CreateEncoder(factory, jpeg ?
			&GUID_ContainerFormatJpeg : &GUID_ContainerFormatPng, NULL, &encoder);
	}
	if (SUCCEEDED(hr)) {
		hr = IWICBitmapEncoder_Initialize(encoder, stream,
			WICBitmapEncoderNoCache);
	}
	if (SUCCEEDED(hr)) {
		hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &options);
	}
	if (SUCCEEDED(hr)) {
		if (jpeg) {
			if (!options) {
				hr = E_POINTER;
			} else {
				PROPBAG2 property = {0};
				property.pstrName = L"ImageQuality";
				VARIANT value = {0};
				value.vt = VT_R4;
				value.fltVal = jpeg_quality / 100.0f;
				hr = IPropertyBag2_Write(options, 1, &property, &value);
			}
		}
	}
	if (SUCCEEDED(hr)) {
		hr = IWICBitmapFrameEncode_Initialize(frame, options);
	}
	if (SUCCEEDED(hr)) {
		hr = IWICBitmapFrameEncode_WriteSource(frame,
			(IWICBitmapSource *)source, NULL);
	}
	if (SUCCEEDED(hr)) {
		hr = IWICBitmapFrameEncode_Commit(frame);
	}
	if (SUCCEEDED(hr)) {
		hr = IWICBitmapEncoder_Commit(encoder);
	}
	if (frame) IWICBitmapFrameEncode_Release(frame);
	if (options) IPropertyBag2_Release(options);
	if (encoder) IWICBitmapEncoder_Release(encoder);
	if (source) IWICBitmap_Release(source);
	if (factory) IWICImagingFactory_Release(factory);
	return hr;
}

static HRESULT save_file(HBITMAP bitmap, const char *path, bool jpeg,
		int jpeg_quality) {
	IWICImagingFactory *factory = NULL;
	IWICStream *stream = NULL;
	HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL,
		CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory);
	if (SUCCEEDED(hr)) {
		hr = IWICImagingFactory_CreateStream(factory, &stream);
	}
	if (SUCCEEDED(hr)) {
		wchar_t *wpath = utf8_to_utf16(path);
		hr = IWICStream_InitializeFromFilename(stream, wpath, GENERIC_WRITE);
		free(wpath);
	}
	if (SUCCEEDED(hr)) {
		hr = encode(bitmap, (IStream *)stream, jpeg, jpeg_quality);
	}
	if (stream) IWICStream_Release(stream);
	if (factory) IWICImagingFactory_Release(factory);
	return hr;
}

// OpenClipboard fails while another program has it open: try a little.
static bool open_clipboard(void) {
	for (int i = 0; i < 20; ++i) {
		if (OpenClipboard(win32_shell_hwnd)) {
			return true;
		}
		Sleep(10);
	}
	return false;
}

// The bitmap as a 24-bit CF_DIB (Windows makes the other bitmap formats
// from it).
static HGLOBAL dib_global(HBITMAP bitmap, int w, int h) {
	int stride = ((w * 3 + 3) / 4) * 4;
	size_t size = sizeof(BITMAPINFOHEADER) + (size_t)stride * (size_t)h;
	HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
	BITMAPINFO *bi = mem ? GlobalLock(mem) : NULL;
	if (!bi) {
		if (mem) GlobalFree(mem);
		return NULL;
	}
	memset(bi, 0, sizeof(BITMAPINFOHEADER));
	bi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi->bmiHeader.biWidth = w;
	bi->bmiHeader.biHeight = h;   // bottom-up, as CF_DIB readers expect
	bi->bmiHeader.biPlanes = 1;
	bi->bmiHeader.biBitCount = 24;
	bi->bmiHeader.biCompression = BI_RGB;
	bi->bmiHeader.biSizeImage = (DWORD)((size_t)stride * (size_t)h);
	HDC dc = GetDC(NULL);
	int lines = GetDIBits(dc, bitmap, 0, (UINT)h,
		(BYTE *)bi + sizeof(BITMAPINFOHEADER), bi, DIB_RGB_COLORS);
	ReleaseDC(NULL, dc);
	GlobalUnlock(mem);
	if (lines != h) {
		GlobalFree(mem);
		return NULL;
	}
	return mem;
}

// The bitmap as PNG bytes, for programs that take "PNG" from the clipboard
// (browsers, chat programs: it keeps the image exact).
static HGLOBAL png_global(HBITMAP bitmap) {
	IStream *stream = NULL;
	if (FAILED(CreateStreamOnHGlobal(NULL, FALSE, &stream))) {
		return NULL;
	}
	HGLOBAL mem = NULL;
	if (SUCCEEDED(encode(bitmap, stream, false, 80))) {
		GetHGlobalFromStream(stream, &mem);
	} else if (SUCCEEDED(GetHGlobalFromStream(stream, &mem))) {
		GlobalFree(mem);
		mem = NULL;
	}
	IStream_Release(stream);
	return mem;
}

static bool copy_to_clipboard(HBITMAP bitmap, int w, int h, char **error) {
	HGLOBAL dib = dib_global(bitmap, w, h);
	HGLOBAL png = png_global(bitmap);
	if (!dib || !open_clipboard()) {
		*error = swine_strdup(dib ? "the clipboard is in use" :
			"out of memory");
		if (dib) GlobalFree(dib);
		if (png) GlobalFree(png);
		return false;
	}
	EmptyClipboard();
	bool ok = SetClipboardData(CF_DIB, dib) != NULL;
	if (!ok) {
		GlobalFree(dib);
	}
	if (png && !SetClipboardData(RegisterClipboardFormatW(L"PNG"), png)) {
		GlobalFree(png);
	}
	CloseClipboard();
	if (!ok) {
		*error = win32_error_string(GetLastError());
	}
	return ok;
}

bool platform_clipboard_set_text(const char *text) {
	wchar_t *wide = utf8_to_utf16(text);
	size_t bytes = (wcslen(wide) + 1) * sizeof(wchar_t);
	HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
	void *data = mem ? GlobalLock(mem) : NULL;
	if (data) {
		memcpy(data, wide, bytes);
		GlobalUnlock(mem);
	}
	free(wide);
	if (!data || !open_clipboard()) {
		if (mem) GlobalFree(mem);
		return false;
	}
	EmptyClipboard();
	bool ok = !*text || SetClipboardData(CF_UNICODETEXT, mem) != NULL;
	if (!ok || !*text) {
		GlobalFree(mem);
	}
	CloseClipboard();
	return ok;
}

bool platform_screenshot(const struct swine_box *box, const char *path,
		bool jpeg, int jpeg_quality, double scale, bool clipboard, bool cursor,
		char **error) {
	double scaled_width = box->width * scale;
	double scaled_height = box->height * scale;
	if (!isfinite(scale) || scale <= 0.0 || scaled_width < 1.0 ||
			scaled_height < 1.0 || scaled_width > INT_MAX ||
			scaled_height > INT_MAX ||
			scaled_width * scaled_height > MAX_SCREENSHOT_PIXELS) {
		*error = swine_strdup("invalid screenshot scale or dimensions");
		return false;
	}
	int width = (int)scaled_width;
	int height = (int)scaled_height;
	HDC screen = GetDC(NULL);
	if (!screen) {
		*error = win32_error_string(GetLastError());
		return false;
	}
	HDC dc = CreateCompatibleDC(screen);
	HBITMAP bitmap = CreateCompatibleBitmap(screen, box->width, box->height);
	if (!dc || !bitmap) {
		*error = win32_error_string(GetLastError());
		if (bitmap) DeleteObject(bitmap);
		if (dc) DeleteDC(dc);
		ReleaseDC(NULL, screen);
		return false;
	}
	HGDIOBJ old = SelectObject(dc, bitmap);
	if (!old || old == HGDI_ERROR) {
		*error = win32_error_string(GetLastError());
		DeleteObject(bitmap);
		DeleteDC(dc);
		ReleaseDC(NULL, screen);
		return false;
	}
	// CAPTUREBLT includes layered windows (notification popups).
	bool ok = BitBlt(dc, 0, 0, box->width, box->height, screen, box->x,
		box->y, SRCCOPY | CAPTUREBLT) != FALSE;
	if (ok && cursor) {
		CURSORINFO ci = {.cbSize = sizeof(ci)};
		if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor) {
			ICONINFO ii;
			if (GetIconInfo(ci.hCursor, &ii)) {
				int x = ci.ptScreenPos.x - box->x - (int)ii.xHotspot;
				int y = ci.ptScreenPos.y - box->y - (int)ii.yHotspot;
				if (!DrawIconEx(dc, x, y, ci.hCursor, 0, 0, 0, NULL,
						DI_NORMAL)) {
					ok = false;
				}
				if (ii.hbmColor) DeleteObject(ii.hbmColor);
				if (ii.hbmMask) DeleteObject(ii.hbmMask);
			}
		}
	}
	SelectObject(dc, old);
	if (!ok) {
		*error = win32_error_string(GetLastError());
		DeleteDC(dc);
		ReleaseDC(NULL, screen);
		DeleteObject(bitmap);
		return false;
	}
	if (scale != 1.0) {
		HDC scaled_dc = CreateCompatibleDC(screen);
		HBITMAP scaled_bitmap = CreateCompatibleBitmap(screen, width, height);
		if (!scaled_dc || !scaled_bitmap) {
			*error = win32_error_string(GetLastError());
			if (scaled_bitmap) DeleteObject(scaled_bitmap);
			if (scaled_dc) DeleteDC(scaled_dc);
			DeleteDC(dc);
			ReleaseDC(NULL, screen);
			DeleteObject(bitmap);
			return false;
		}
		HGDIOBJ scaled_old = SelectObject(scaled_dc, scaled_bitmap);
		HGDIOBJ source_old = SelectObject(dc, bitmap);
		SetStretchBltMode(scaled_dc, HALFTONE);
		SetBrushOrgEx(scaled_dc, 0, 0, NULL);
		ok = scaled_old && scaled_old != HGDI_ERROR && source_old &&
			source_old != HGDI_ERROR && StretchBlt(scaled_dc, 0, 0, width,
				height, dc, 0, 0, box->width, box->height, SRCCOPY) != FALSE;
		if (source_old && source_old != HGDI_ERROR) SelectObject(dc, source_old);
		if (scaled_old && scaled_old != HGDI_ERROR) {
			SelectObject(scaled_dc, scaled_old);
		}
		DeleteDC(scaled_dc);
		DeleteDC(dc);
		ReleaseDC(NULL, screen);
		DeleteObject(bitmap);
		if (!ok) {
			*error = win32_error_string(GetLastError());
			DeleteObject(scaled_bitmap);
			return false;
		}
		bitmap = scaled_bitmap;
	} else {
		DeleteDC(dc);
		ReleaseDC(NULL, screen);
	}
	if (path) {
		HRESULT hr = save_file(bitmap, path, jpeg, jpeg_quality);
		if (FAILED(hr)) {
			*error = win32_error_string((DWORD)hr);
			ok = false;
		}
	}
	if (ok && clipboard) {
		ok = copy_to_clipboard(bitmap, width, height, error);
	}
	DeleteObject(bitmap);
	return ok;
}
