// Screenshots for the built-in grim: copy the screen with GDI and encode
// it with the Windows Imaging Component.
#define COBJMACROS
#include <stdlib.h>
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

static bool encode(HBITMAP bitmap, const wchar_t *path, bool jpeg,
		HRESULT *failure) {
	IWICImagingFactory *factory = NULL;
	IWICBitmap *source = NULL;
	IWICStream *stream = NULL;
	IWICBitmapEncoder *encoder = NULL;
	IWICBitmapFrameEncode *frame = NULL;
	HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL,
		CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory);
	if (SUCCEEDED(hr)) {
		hr = IWICImagingFactory_CreateBitmapFromHBITMAP(factory, bitmap, NULL,
			WICBitmapIgnoreAlpha, &source);
	}
	if (SUCCEEDED(hr)) {
		hr = IWICImagingFactory_CreateStream(factory, &stream);
	}
	if (SUCCEEDED(hr)) {
		hr = IWICStream_InitializeFromFilename(stream, path, GENERIC_WRITE);
	}
	if (SUCCEEDED(hr)) {
		hr = IWICImagingFactory_CreateEncoder(factory, jpeg ?
			&GUID_ContainerFormatJpeg : &GUID_ContainerFormatPng, NULL, &encoder);
	}
	if (SUCCEEDED(hr)) {
		hr = IWICBitmapEncoder_Initialize(encoder, (IStream *)stream,
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
	if (stream) IWICStream_Release(stream);
	if (source) IWICBitmap_Release(source);
	if (factory) IWICImagingFactory_Release(factory);
	*failure = hr;
	return SUCCEEDED(hr);
}

bool platform_screenshot(const struct swine_box *box, const char *path,
		bool jpeg, char **error) {
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
	wchar_t *wpath = utf8_to_utf16(path);
	HRESULT hr = S_OK;
	ok = encode(bitmap, wpath, jpeg, &hr);
	free(wpath);
	DeleteObject(bitmap);
	if (!ok) {
		*error = win32_error_string((DWORD)hr);
	}
	return ok;
}
