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
	int degrees;
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
	return win32_font_rotated(families, px, bold, italic, 0);
}

HFONT win32_font_rotated(const char *families, int px, bool bold, bool italic,
		int degrees) {
	if (!fonts) {
		fonts = create_list();
	}
	for (int i = 0; i < fonts->length; ++i) {
		struct cached_font *f = fonts->items[i];
		if (f->px == px && f->bold == bold && f->italic == italic &&
				f->degrees == degrees && strcmp(f->families, families) == 0) {
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
	// Counter-clockwise, as GTK's label angle.
	lf.lfEscapement = lf.lfOrientation = degrees * 10;
	wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
	free(face);
	struct cached_font *f = calloc(1, sizeof(*f));
	f->families = swine_strdup(families);
	f->px = px;
	f->bold = bold;
	f->italic = italic;
	f->degrees = degrees;
	f->font = CreateFontIndirectW(&lf);
	list_add(fonts, f);
	return f->font;
}

// A sway/Pango font description: "pango:DejaVu Sans Mono Bold 10" (size in
// points, style words before it, families separated by commas).
HFONT win32_sway_font(const char *desc, double scale) {
	if (!desc) {
		desc = "monospace 10";
	}
	if (strncmp(desc, "pango:", 6) == 0) {
		desc += 6;
	}
	list_t *words = split_string(desc, " ");
	double pt = 10;
	bool bold = false, italic = false;
	int end = words->length;
	if (end > 0) {
		char *last = words->items[end - 1];
		char *rest = NULL;
		double size = strtod(last, &rest);
		if (rest != last && (*rest == '\0' || strcmp(rest, "px") == 0)) {
			pt = strcmp(rest, "px") == 0 ? size * 72.0 / 96.0 : size;
			end--;
		}
	}
	while (end > 0) {
		const char *word = words->items[end - 1];
		if (swine_strcasecmp(word, "bold") == 0) {
			bold = true;
		} else if (swine_strcasecmp(word, "italic") == 0 ||
				swine_strcasecmp(word, "oblique") == 0) {
			italic = true;
		} else if (swine_strcasecmp(word, "regular") != 0 &&
				swine_strcasecmp(word, "normal") != 0 &&
				swine_strcasecmp(word, "book") != 0) {
			break;
		}
		end--;
	}
	char families[256] = "";
	for (int i = 0; i < end; ++i) {
		if (i > 0) {
			strncat(families, " ", sizeof(families) - strlen(families) - 1);
		}
		strncat(families, words->items[i], sizeof(families) - strlen(families) - 1);
	}
	list_free_items_and_destroy(words);
	if (!families[0]) {
		strcpy(families, "monospace");
	}
	int px = (int)(pt * 96.0 / 72.0 * scale + 0.5);
	return win32_font(families, px > 0 ? px : 1, bold, italic);
}

int platform_font_height(const char *desc) {
	HDC dc = GetDC(NULL);
	HGDIOBJ old = SelectObject(dc, win32_sway_font(desc, 1.0));
	TEXTMETRICW tm = {0};
	GetTextMetricsW(dc, &tm);
	SelectObject(dc, old);
	ReleaseDC(NULL, dc);
	return tm.tmHeight > 0 ? tm.tmHeight : 16;
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
