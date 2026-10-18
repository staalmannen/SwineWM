// "swinemenu --open-with <file>": a replacement for Windows' "Open with"
// dialog, which stays empty without Explorer as the shell. The programs come
// from the documented SHAssocEnumHandlers (the ones the dialog would list);
// the chosen one opens the file through IAssocHandler::Invoke.
//
// "--install-open-with" registers swinemenu (per user, HKCU\Software\Classes)
// as an "Open with swinemenu" verb on every file and as the handler Windows
// asks for files it has no program for (the "Unknown" type's "openas" verb,
// which otherwise starts OpenWith.exe).
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "menu.h"
#include "stringop.h"
#include "swinemenu.h"

static IAssocHandler **handlers;
static int handler_count;

static bool has_path(const wchar_t *path) {
	for (int i = 0; i < handler_count; ++i) {
		LPWSTR other = NULL;
		bool same = SUCCEEDED(IAssocHandler_GetName(handlers[i], &other)) &&
			_wcsicmp(other, path) == 0;
		CoTaskMemFree(other);
		if (same) {
			return true;
		}
	}
	return false;
}

static void add_handlers(const wchar_t *ext, ASSOC_FILTER filter,
		list_t *entries, const wchar_t *default_name) {
	IEnumAssocHandlers *list = NULL;
	if (FAILED(SHAssocEnumHandlers(ext, filter, &list))) {
		return;
	}
	IAssocHandler *handler = NULL;
	ULONG fetched = 0;
	while (IEnumAssocHandlers_Next(list, 1, &handler, &fetched) == S_OK &&
			fetched == 1) {
		LPWSTR path = NULL, ui_name = NULL;
		IAssocHandler_GetName(handler, &path);
		IAssocHandler_GetUIName(handler, &ui_name);
		if (!ui_name || (path && has_path(path))) {
			CoTaskMemFree(path);
			CoTaskMemFree(ui_name);
			IAssocHandler_Release(handler);
			continue;
		}
		char *name = swinemenu_utf16_to_utf8(ui_name);
		if (default_name && _wcsicmp(ui_name, default_name) == 0) {
			char *marked = format_str("%s (default)", name);
			free(name);
			name = marked;
		}
		char *command = format_str("openwith:%d", handler_count);
		list_add(entries, menu_entry_create(name, command));
		free(name);
		free(command);
		handlers = realloc(handlers, sizeof(*handlers) * (size_t)(handler_count + 1));
		handlers[handler_count++] = handler;
		CoTaskMemFree(path);
		CoTaskMemFree(ui_name);
	}
	IEnumAssocHandlers_Release(list);
}

// --- registry fallback --------------------------------------------------------
// When SHAssocEnumHandlers lists nothing (Wine, or a type only described in
// the registry), the programs come from the type's ProgIDs and its
// OpenWithProgids and OpenWithList keys, with their "open" command lines.

static bool has_entry(list_t *entries, const char *name) {
	for (int i = 0; i < entries->length; ++i) {
		struct menu_entry *e = entries->items[i];
		if (strcmp(e->name, name) == 0) {
			return true;
		}
	}
	return false;
}

// The command line of an "open" verb with the file filled in for %1/%L.
static char *fill_command(const wchar_t *command, const wchar_t *file) {
	wchar_t expanded[2048];
	if (!ExpandEnvironmentStringsW(command, expanded, 2048)) {
		wcsncpy(expanded, command, 2047);
		expanded[2047] = L'\0';
	}
	char *line = swinemenu_utf16_to_utf8(expanded);
	char *file8 = swinemenu_utf16_to_utf8(file);
	size_t cap = strlen(line) + strlen(file8) * 2 + 8;
	char *out = malloc(cap);
	size_t len = 0;
	bool filled = false;
	for (const char *p = line; *p; ++p) {
		if (p[0] == '%' && (p[1] == '1' || p[1] == 'L' || p[1] == 'l')) {
			size_t n = strlen(file8);
			if (len + n + 1 > cap) {
				cap = (len + n + 1) * 2;
				out = realloc(out, cap);
			}
			memcpy(out + len, file8, n);
			len += n;
			++p;
			filled = true;
		} else if (p[0] == '%' && p[1] == '*') {
			++p;   // "all other arguments": there are none
		} else {
			if (len + 2 > cap) {
				cap *= 2;
				out = realloc(out, cap);
			}
			out[len++] = *p;
		}
	}
	out[len] = '\0';
	if (!filled) {
		char *with_file = format_str("%s \"%s\"", out, file8);
		free(out);
		out = with_file;
	}
	free(line);
	free(file8);
	return out;
}

// assoc: a ProgID ("txtfile") or "Applications\\<program>.exe".
static void add_assoc(const wchar_t *assoc, const wchar_t *file,
		list_t *entries, const wchar_t *default_name) {
	wchar_t name[256], command[2048];
	DWORD n = 256;
	if (FAILED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_FRIENDLYAPPNAME, assoc,
			L"open", name, &n))) {
		return;
	}
	n = 2048;
	if (FAILED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_COMMAND, assoc, L"open",
			command, &n))) {
		return;
	}
	char *name8 = swinemenu_utf16_to_utf8(name);
	if (default_name && _wcsicmp(name, default_name) == 0) {
		char *marked = format_str("%s (default)", name8);
		free(name8);
		name8 = marked;
	}
	if (!has_entry(entries, name8)) {
		char *line = fill_command(command, file);
		char *entry_command = format_str("line:%s", line);
		list_add(entries, menu_entry_create(name8, entry_command));
		free(entry_command);
		free(line);
	}
	free(name8);
}

static void add_registry_handlers(const wchar_t *ext, const wchar_t *file,
		list_t *entries, const wchar_t *default_name) {
	HKEY key;
	if (RegOpenKeyExW(HKEY_CLASSES_ROOT, ext, 0, KEY_READ, &key) != ERROR_SUCCESS) {
		return;
	}
	wchar_t progid[256];
	DWORD size = sizeof(progid);
	if (RegQueryValueExW(key, NULL, NULL, NULL, (BYTE *)progid, &size) ==
			ERROR_SUCCESS && progid[0]) {
		add_assoc(progid, file, entries, default_name);
	}
	HKEY sub;
	if (RegOpenKeyExW(key, L"OpenWithProgids", 0, KEY_READ, &sub) == ERROR_SUCCESS) {
		for (DWORD i = 0;; ++i) {
			wchar_t value[256];
			DWORD len = 256;
			if (RegEnumValueW(sub, i, value, &len, NULL, NULL, NULL, NULL) !=
					ERROR_SUCCESS) {
				break;
			}
			if (value[0]) {
				add_assoc(value, file, entries, default_name);
			}
		}
		RegCloseKey(sub);
	}
	if (RegOpenKeyExW(key, L"OpenWithList", 0, KEY_READ, &sub) == ERROR_SUCCESS) {
		for (DWORD i = 0;; ++i) {
			wchar_t program[256];
			DWORD len = 256;
			if (RegEnumKeyExW(sub, i, program, &len, NULL, NULL, NULL, NULL) !=
					ERROR_SUCCESS) {
				break;
			}
			wchar_t assoc[300];
			_snwprintf(assoc, 300, L"Applications\\%ls", program);
			assoc[299] = L'\0';
			add_assoc(assoc, file, entries, default_name);
		}
		RegCloseKey(sub);
	}
	RegCloseKey(key);
}

list_t *swinemenu_openwith_entries(const wchar_t *file) {
	list_t *entries = create_list();
	const wchar_t *ext = PathFindExtensionW(file);
	if (!ext || !*ext) {
		return entries;   // no type: only a typed command line will do
	}
	wchar_t default_name[256] = L"";
	DWORD size = 256;
	bool have_default = SUCCEEDED(AssocQueryStringW(ASSOCF_NONE,
		ASSOCSTR_FRIENDLYAPPNAME, ext, NULL, default_name, &size));
	// Recommended programs first, then every other registered one.
	add_handlers(ext, ASSOC_FILTER_RECOMMENDED, entries,
		have_default ? default_name : NULL);
	add_handlers(ext, ASSOC_FILTER_NONE, entries,
		have_default ? default_name : NULL);
	if (entries->length == 0) {
		add_registry_handlers(ext, file, entries,
			have_default ? default_name : NULL);
	}
	return entries;
}

bool swinemenu_openwith_invoke(const char *command, const wchar_t *file) {
	if (strncmp(command, "openwith:", 9) != 0) {
		return false;
	}
	int index = atoi(command + 9);
	if (index < 0 || index >= handler_count) {
		return false;
	}
	IShellItem *item = NULL;
	IDataObject *data = NULL;
	HRESULT hr = SHCreateItemFromParsingName(file, NULL, &IID_IShellItem,
		(void **)&item);
	if (SUCCEEDED(hr)) {
		hr = IShellItem_BindToHandler(item, NULL, &BHID_DataObject,
			&IID_IDataObject, (void **)&data);
		IShellItem_Release(item);
	}
	if (SUCCEEDED(hr)) {
		hr = IAssocHandler_Invoke(handlers[index], data);
		IDataObject_Release(data);
	}
	return SUCCEEDED(hr);
}

// A command line that opens file with the handler's program, for when
// Invoke fails: "<program>" "<file>". NULL if the handler has no program
// file. Caller frees.
char *swinemenu_openwith_fallback(const char *command, const wchar_t *file) {
	if (strncmp(command, "line:", 5) == 0) {
		return swine_strdup(command + 5);   // from the registry fallback
	}
	if (strncmp(command, "openwith:", 9) != 0) {
		return NULL;
	}
	int index = atoi(command + 9);
	if (index < 0 || index >= handler_count) {
		return NULL;
	}
	LPWSTR path = NULL;
	if (FAILED(IAssocHandler_GetName(handlers[index], &path)) ||
			GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
		CoTaskMemFree(path);
		return NULL;
	}
	char *path8 = swinemenu_utf16_to_utf8(path);
	char *file8 = swinemenu_utf16_to_utf8(file);
	char *line = format_str("\"%s\" \"%s\"", path8, file8);
	free(path8);
	free(file8);
	CoTaskMemFree(path);
	return line;
}

// --- registration ------------------------------------------------------------

static const wchar_t verb_key[] =
	L"Software\\Classes\\*\\shell\\SwineWMOpenWith";
static const wchar_t unknown_key[] =
	L"Software\\Classes\\Unknown\\shell\\openas";

static bool set_string(HKEY root, const wchar_t *key, const wchar_t *name,
		const wchar_t *value) {
	HKEY k;
	if (RegCreateKeyExW(root, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k,
			NULL) != ERROR_SUCCESS) {
		return false;
	}
	LONG rc = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)value,
		(DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
	RegCloseKey(k);
	return rc == ERROR_SUCCESS;
}

int swinemenu_install_open_with(bool install) {
	if (!install) {
		RegDeleteTreeW(HKEY_CURRENT_USER, verb_key);
		RegDeleteTreeW(HKEY_CURRENT_USER, unknown_key);
		SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
		return 0;
	}
	wchar_t exe[MAX_PATH];
	if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) {
		return 1;
	}
	wchar_t command[MAX_PATH + 32];
	_snwprintf(command, MAX_PATH + 32, L"\"%ls\" --open-with \"%%1\"", exe);
	command[MAX_PATH + 31] = L'\0';
	wchar_t verb_command[sizeof(verb_key) / sizeof(wchar_t) + 16];
	_snwprintf(verb_command, sizeof(verb_command) / sizeof(wchar_t),
		L"%ls\\command", verb_key);
	wchar_t unknown_command[sizeof(unknown_key) / sizeof(wchar_t) + 16];
	_snwprintf(unknown_command, sizeof(unknown_command) / sizeof(wchar_t),
		L"%ls\\command", unknown_key);
	bool ok = set_string(HKEY_CURRENT_USER, verb_key, L"MUIVerb",
			L"Open with swinemenu") &&
		set_string(HKEY_CURRENT_USER, verb_key, L"Icon", exe) &&
		set_string(HKEY_CURRENT_USER, verb_command, NULL, command) &&
		set_string(HKEY_CURRENT_USER, unknown_command, NULL, command);
	SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
	return ok ? 0 : 1;
}
