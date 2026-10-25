// Keyboard layouts (platform_keyboard_layouts): the input languages and
// layouts installed in Windows' language settings, described the way sway
// describes the layouts of its xkb keymap.
//
// A layout is an HKL: the low word is the language, the high word the
// layout ("device"). Its names come from the locale (English names, as xkb
// has) and, for a layout that is not the language's default, from the
// "Layout Text" under HKLM\SYSTEM\CurrentControlSet\Control\Keyboard Layouts.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "list.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/input/keyboard.h"
#include "swinewm/platform.h"
#include "win32.h"

#define MAX_LAYOUTS 64

static const wchar_t layouts_key[] =
	L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts";

// Names of each HKL, kept while Windows' list stays the same.
static struct {
	HKL hkls[MAX_LAYOUTS];
	int count;
	struct platform_keyboard_layout names[MAX_LAYOUTS];
} cache;

static char *layout_text(const wchar_t *klid) {
	wchar_t path[128];
	swprintf(path, sizeof(path) / sizeof(path[0]), L"%ls\\%ls", layouts_key, klid);
	wchar_t text[128];
	DWORD size = sizeof(text);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, path, L"Layout Text", RRF_RT_REG_SZ,
			NULL, text, &size) != ERROR_SUCCESS) {
		return NULL;
	}
	return utf16_to_utf8(text);
}

// A layout variant (high word 0xFnnn) names its layout by "Layout Id" nnn:
// find the KLID that has it.
static bool find_layout_id(WORD id, wchar_t klid[KL_NAMELENGTH]) {
	HKEY key;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, layouts_key, 0, KEY_READ, &key) !=
			ERROR_SUCCESS) {
		return false;
	}
	bool found = false;
	wchar_t name[KL_NAMELENGTH + 8];
	for (DWORD i = 0; !found; ++i) {
		DWORD len = sizeof(name) / sizeof(name[0]);
		if (RegEnumKeyExW(key, i, name, &len, NULL, NULL, NULL, NULL) !=
				ERROR_SUCCESS) {
			break;
		}
		wchar_t value[16];
		DWORD size = sizeof(value);
		if (RegGetValueW(key, name, L"Layout Id", RRF_RT_REG_SZ, NULL, value,
				&size) == ERROR_SUCCESS &&
				wcstoul(value, NULL, 16) == id && len < KL_NAMELENGTH) {
			wcscpy(klid, name);
			found = true;
		}
	}
	RegCloseKey(key);
	return found;
}

// The KLID ("00000409", "00020409", "E0010411") of an HKL.
static void hkl_klid(HKL hkl, wchar_t klid[KL_NAMELENGTH]) {
	UINT_PTR value = (UINT_PTR)hkl;
	WORD device = HIWORD(value);
	if ((device & 0xF000) == 0xF000 && find_layout_id(device & 0x0FFF, klid)) {
		return;
	}
	if ((device & 0xF000) == 0xE000) {   // an input method editor
		swprintf(klid, KL_NAMELENGTH, L"%08X", (unsigned)(value & 0xFFFFFFFF));
	} else {
		swprintf(klid, KL_NAMELENGTH, L"%08X", (unsigned)device);
	}
}

static char *locale_info(const wchar_t *locale, LCTYPE type) {
	wchar_t text[128];
	if (!GetLocaleInfoEx(locale, type, text, sizeof(text) / sizeof(text[0]))) {
		return NULL;
	}
	return utf16_to_utf8(text);
}

static void describe(HKL hkl, struct platform_keyboard_layout *out) {
	WORD lang = LOWORD((UINT_PTR)hkl);
	wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"";
	LCIDToLocaleName(MAKELCID(lang, SORT_DEFAULT), locale,
		LOCALE_NAME_MAX_LENGTH, 0);
	// "sv-SE": the language and the region; "sr-Latn-RS" has a script too.
	char *tag = utf16_to_utf8(locale);
	char *dash = strchr(tag, '-');
	char *region = strrchr(tag, '-');
	out->description = swine_strndup(tag, dash ? (size_t)(dash - tag) :
		strlen(tag));
	out->short_name = swine_strdup(region ? region + 1 : tag);
	for (char *c = out->short_name; *c; ++c) {
		if (*c >= 'A' && *c <= 'Z') {
			*c = (char)(*c - 'A' + 'a');
		}
	}
	free(tag);

	wchar_t klid[KL_NAMELENGTH];
	hkl_klid(hkl, klid);
	wchar_t default_klid[KL_NAMELENGTH];
	swprintf(default_klid, KL_NAMELENGTH, L"%08X", (unsigned)lang);
	char *display = locale_info(locale, LOCALE_SENGLISHDISPLAYNAME);
	if (wcscmp(klid, default_klid) == 0) {
		out->name = display ? display : swine_strdup(out->short_name);
		out->variant = swine_strdup("");
		return;
	}
	// Another layout for the language: "English (United States-Dvorak)".
	char *text = layout_text(klid);
	char *language = locale_info(locale, LOCALE_SENGLISHLANGUAGENAME);
	out->variant = text ? text : swine_strdup("");
	if (language && text) {
		out->name = format_str("%s (%s)", language, text);
	} else {
		out->name = display ? swine_strdup(display) :
			swine_strdup(out->short_name);
	}
	free(language);
	free(display);
}

static void clear_cache(void) {
	for (int i = 0; i < cache.count; ++i) {
		free(cache.names[i].name);
		free(cache.names[i].short_name);
		free(cache.names[i].description);
		free(cache.names[i].variant);
	}
	memset(&cache, 0, sizeof(cache));
}

static void update_cache(void) {
	HKL hkls[MAX_LAYOUTS];
	int count = GetKeyboardLayoutList(MAX_LAYOUTS, hkls);
	if (count < 0) {
		count = 0;
	}
	if (count == cache.count &&
			memcmp(hkls, cache.hkls, (size_t)count * sizeof(HKL)) == 0) {
		return;
	}
	clear_cache();
	cache.count = count;
	for (int i = 0; i < count; ++i) {
		cache.hkls[i] = hkls[i];
		describe(hkls[i], &cache.names[i]);
	}
}

// The layout of the window the user types into. Windows keeps one per
// thread (by default switching it switches every window).
static HKL active_hkl(void) {
	HWND fg = GetForegroundWindow();
	DWORD thread = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
	return GetKeyboardLayout(thread);
}

int platform_keyboard_layouts(list_t *layouts) {
	update_cache();
	HKL active = active_hkl();
	int index = -1;
	for (int i = 0; i < cache.count; ++i) {
		struct platform_keyboard_layout *l = calloc(1, sizeof(*l));
		l->name = swine_strdup(cache.names[i].name);
		l->short_name = swine_strdup(cache.names[i].short_name);
		l->description = swine_strdup(cache.names[i].description);
		l->variant = swine_strdup(cache.names[i].variant);
		list_add(layouts, l);
		if (cache.hkls[i] == active) {
			index = i;
		}
	}
	return index;
}

void platform_keyboard_set_layout(int index) {
	update_cache();
	if (index < 0 || index >= cache.count) {
		return;
	}
	HKL hkl = cache.hkls[index];
	HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	if (fg) {
		GetWindowThreadProcessId(fg, &pid);
	}
	if (fg && pid != GetCurrentProcessId()) {
		// What the language bar does: ask the window to switch. Posted,
		// never sent, so a hung application cannot block swinewm.
		PostMessageW(fg, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)hkl);
	}
	ActivateKeyboardLayout(hkl, KLF_SETFORPROCESS);
	// The window switches when it reads the message: look again shortly.
	if (win32_shell_hwnd) {
		SetTimer(win32_shell_hwnd, WIN32_TIMER_LAYOUT, 150, NULL);
	}
}

void win32_layouts_finish(void) {
	clear_cache();
}
