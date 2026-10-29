// Screenshots for the built-in grim: copy the screen with GDI and encode
// it with the Windows Imaging Component.
#define COBJMACROS
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wincodec.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/platform.h"
#include "win32.h"
#include <shlobj.h>

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
static HRESULT encode(HBITMAP bitmap, IStream *stream, bool jpeg) {
	IWICImagingFactory *factory = NULL;
	IWICBitmap *source = NULL;
	IWICBitmapEncoder *encoder = NULL;
	IWICBitmapFrameEncode *frame = NULL;
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
		hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, NULL);
	}
	if (SUCCEEDED(hr)) {
		hr = IWICBitmapFrameEncode_Initialize(frame, NULL);
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
	if (encoder) IWICBitmapEncoder_Release(encoder);
	if (source) IWICBitmap_Release(source);
	if (factory) IWICImagingFactory_Release(factory);
	return hr;
}

static HRESULT save_file(HBITMAP bitmap, const char *path, bool jpeg) {
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
		hr = encode(bitmap, (IStream *)stream, jpeg);
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
	if (SUCCEEDED(encode(bitmap, stream, false))) {
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
		bool jpeg, bool clipboard, char **error) {
	HDC screen = GetDC(NULL);
	HDC dc = CreateCompatibleDC(screen);
	HBITMAP bitmap = CreateCompatibleBitmap(screen, box->width, box->height);
	HGDIOBJ old = SelectObject(dc, bitmap);
	// CAPTUREBLT includes layered windows (notification popups).
	bool ok = BitBlt(dc, 0, 0, box->width, box->height, screen, box->x,
		box->y, SRCCOPY | CAPTUREBLT) != FALSE;
	SelectObject(dc, old);
	DeleteDC(dc);
	ReleaseDC(NULL, screen);
	if (!ok) {
		*error = win32_error_string(GetLastError());
		DeleteObject(bitmap);
		return false;
	}
	if (path) {
		HRESULT hr = save_file(bitmap, path, jpeg);
		if (FAILED(hr)) {
			*error = win32_error_string((DWORD)hr);
			ok = false;
		}
	}
	if (ok && clipboard) {
		ok = copy_to_clipboard(bitmap, box->width, box->height, error);
	}
	DeleteObject(bitmap);
	return ok;
}
