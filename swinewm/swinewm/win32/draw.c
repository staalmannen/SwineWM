#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "list.h"
#include "stringop.h"
#include "swinewm/tree/output.h"
#include "win32.h"

struct cached_font {
	char *families;
	int px;
	bool bold, italic;
	HFONT font;
};

static list_t *fonts = NULL;

static int CALLBACK font_exists_proc(const LOGFONTW *lf,
		const TEXTMETRICW *tm, DWORD type, LPARAM data) {
	(void)lf;
	(void)tm;
	(void)type;
	*(bool *)data = true;
	return 0;
}

static bool font_installed(const wchar_t *face) {
	HDC dc = GetDC(NULL);
	LOGFONTW lf = { .lfCharSet = DEFAULT_CHARSET };
	wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
	bool found = false;
	EnumFontFamiliesExW(dc, &lf, font_exists_proc, (LPARAM)&found, 0);
	ReleaseDC(NULL, dc);
	return found;
}

// Generic CSS/Pango families mapped to fonts every Windows install has.
static const char *generic_family(const char *name) {
	if (swine_strcasecmp(name, "sans-serif") == 0 ||
			swine_strcasecmp(name, "sans") == 0 ||
			swine_strcasecmp(name, "system-ui") == 0) {
		return "Segoe UI";
	}
	if (swine_strcasecmp(name, "serif") == 0) {
		return "Times New Roman";
	}
	if (swine_strcasecmp(name, "monospace") == 0) {
		return "Consolas";
	}
	return NULL;
}

static wchar_t *pick_family(const char *families) {
	list_t *names = split_string(families, ",");
	wchar_t *chosen = NULL;
	for (int i = 0; i < names->length && !chosen; ++i) {
		char *name = names->items[i];
		strip_whitespace(name);
		const char *generic = generic_family(name);
		wchar_t *wide = utf8_to_utf16(generic ? generic : name);
		if (wide && (generic || font_installed(wide))) {
			chosen = wide;
		} else {
			free(wide);
		}
	}
	list_free_items_and_destroy(names);
	return chosen ? chosen : utf8_to_utf16("Segoe UI");
}

HFONT win32_font(const char *families, int px, bool bold, bool italic) {
	if (!fonts) {
		fonts = create_list();
	}
	for (int i = 0; i < fonts->length; ++i) {
		struct cached_font *f = fonts->items[i];
		if (f->px == px && f->bold == bold && f->italic == italic &&
				strcmp(f->families, families) == 0) {
			return f->font;
		}
	}
	wchar_t *face = pick_family(families);
	LOGFONTW lf = {0};
	lf.lfHeight = -px;   // negative: character height, like CSS font-size
	lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
	lf.lfItalic = italic ? TRUE : FALSE;
	lf.lfCharSet = DEFAULT_CHARSET;
	lf.lfQuality = CLEARTYPE_QUALITY;
	wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
	free(face);
	struct cached_font *f = calloc(1, sizeof(*f));
	f->families = swine_strdup(families);
	f->px = px;
	f->bold = bold;
	f->italic = italic;
	f->font = CreateFontIndirectW(&lf);
	list_add(fonts, f);
	return f->font;
}

void win32_fonts_clear(void) {
	if (!fonts) {
		return;
	}
	for (int i = 0; i < fonts->length; ++i) {
		struct cached_font *f = fonts->items[i];
		DeleteObject(f->font);
		free(f->families);
		free(f);
	}
	list_free(fonts);
	fonts = NULL;
}

COLORREF win32_blend(struct swine_color c, COLORREF under) {
	unsigned a = c.a;
	unsigned r = (c.r * a + GetRValue(under) * (255 - a)) / 255;
	unsigned g = (c.g * a + GetGValue(under) * (255 - a)) / 255;
	unsigned b = (c.b * a + GetBValue(under) * (255 - a)) / 255;
	return RGB(r, g, b);
}

void win32_fill(HDC dc, const RECT *rect, COLORREF color) {
	SetDCBrushColor(dc, color);
	FillRect(dc, rect, (HBRUSH)GetStockObject(DC_BRUSH));
}

double win32_output_scale(struct swine_output *output) {
	return output && output->scale > 0 ? output->scale : 1.0;
}
