// Where swinemenu's entries come from. Everything here uses documented
// shell APIs that work without Explorer as the shell: the Start menu
// folders are plain directories, and the Applications folder
// (FOLDERID_AppsFolder) lists Store apps with their AppUserModelIDs.
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "ipc.h"
#include "ipc-client.h"
#include "json.h"
#include "menu.h"
#include "stringop.h"
#include "swinemenu.h"

char *swinemenu_utf16_to_utf8(const wchar_t *w) {
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	char *s = malloc((size_t)(n > 0 ? n : 1));
	if (n > 0) {
		WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
	} else {
		s[0] = '\0';
	}
	return s;
}

wchar_t *swinemenu_utf8_to_utf16(const char *s) {
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	wchar_t *w = malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
	if (n > 0) {
		MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
	} else {
		w[0] = L'\0';
	}
	return w;
}

static bool has_entry(list_t *entries, const char *name) {
	for (int i = 0; i < entries->length; ++i) {
		struct menu_entry *e = entries->items[i];
		if (swine_strcasecmp(e->name, name) == 0) {
			return true;
		}
	}
	return false;
}

// What a Start menu shortcut points to (empty if it cannot be read, or for
// installer-advertised shortcuts, which have no plain target).
struct shortcut {
	wchar_t target[MAX_PATH];     // environment variables expanded
	wchar_t args[1024];
	wchar_t description[1024];    // INFOTIPSIZE
};

static void read_shortcut(IShellLinkW *link_obj, const wchar_t *path,
		struct shortcut *out) {
	memset(out, 0, sizeof(*out));
	IPersistFile *file = NULL;
	if (!link_obj || FAILED(IShellLinkW_QueryInterface(link_obj,
			&IID_IPersistFile, (void **)&file))) {
		return;
	}
	if (SUCCEEDED(IPersistFile_Load(file, path, STGM_READ))) {
		wchar_t raw[MAX_PATH] = L"";
		if (SUCCEEDED(IShellLinkW_GetPath(link_obj, raw, MAX_PATH, NULL,
				SLGP_RAWPATH)) && raw[0]) {
			ExpandEnvironmentStringsW(raw, out->target, MAX_PATH);
		}
		if (FAILED(IShellLinkW_GetArguments(link_obj, out->args,
				(int)(sizeof(out->args) / sizeof(out->args[0]))))) {
			out->args[0] = L'\0';
		}
		// Fails (E_FAIL) for shortcuts without a description.
		if (FAILED(IShellLinkW_GetDescription(link_obj, out->description,
				(int)(sizeof(out->description) / sizeof(out->description[0]))))) {
			out->description[0] = L'\0';
		}
	}
	IPersistFile_Release(file);
}

static const wchar_t *file_part(const wchar_t *path) {
	const wchar_t *slash = wcsrchr(path, L'\\');
	return slash ? slash + 1 : path;
}

// Shortcuts to documents (readme files, manuals, web pages) are not apps.
static bool is_document(const wchar_t *target) {
	const wchar_t *dot = wcsrchr(file_part(target), L'.');
	static const wchar_t *documents[] = {
		L".txt", L".htm", L".html", L".pdf", L".chm", L".rtf",
		L".md", L".url", L".hlp", L".doc", L".docx",
	};
	for (size_t i = 0; dot && i < sizeof(documents) / sizeof(documents[0]);
			++i) {
		if (_wcsicmp(dot, documents[i]) == 0) {
			return true;
		}
	}
	return false;
}

// Uninstallers, by the program they start, whatever the shortcut's name
// (unins000.exe, uninst.exe, Uninstall.exe, msiexec /x).
static bool is_uninstaller(const wchar_t *target, const wchar_t *args) {
	wchar_t file[MAX_PATH];
	wcsncpy(file, file_part(target), MAX_PATH - 1);
	file[MAX_PATH - 1] = L'\0';
	_wcslwr(file);
	if (wcsncmp(file, L"unins", 5) == 0 || wcsstr(file, L"uninstall")) {
		return true;
	}
	if (wcscmp(file, L"msiexec.exe") == 0) {
		wchar_t lower[1024];
		wcsncpy(lower, args, 1023);
		lower[1023] = L'\0';
		_wcslwr(lower);
		return wcsstr(lower, L"/x") || wcsstr(lower, L"/uninstall");
	}
	return false;
}

// The name Explorer shows for a file whose folder's desktop.ini gives it
// a translated one (Start menu shortcuts of Windows' own programs are
// named in English: "File Explorer.lnk" is shown as "Utforskaren"), or
// NULL.
static char *localized_name(const wchar_t *path) {
	wchar_t module[MAX_PATH];
	int id = 0;
	if (FAILED(SHGetLocalizedName(path, module, MAX_PATH, &id))) {
		return NULL;
	}
	wchar_t expanded[MAX_PATH];
	if (!ExpandEnvironmentStringsW(module, expanded, MAX_PATH)) {
		return NULL;
	}
	HMODULE lib = LoadLibraryExW(expanded, NULL,
		LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
	if (!lib) {
		return NULL;
	}
	wchar_t text[256];
	int n = LoadStringW(lib, (UINT)(id < 0 ? -id : id), text, 256);
	FreeLibrary(lib);
	return n > 0 ? swinemenu_utf16_to_utf8(text) : NULL;
}

// Programs started from more than one shortcut (or also listed in App
// Paths) are shown once: the first one found.
static bool seen_target(list_t *seen, const wchar_t *target,
		const wchar_t *args) {
	if (!target[0]) {
		return false;
	}
	char *t = swinemenu_utf16_to_utf8(target);
	char *a = swinemenu_utf16_to_utf8(args);
	char *key = format_str("%s|%s", t, a);
	free(t);
	free(a);
	for (char *c = key; *c; ++c) {
		if (*c >= 'A' && *c <= 'Z') {
			*c = (char)(*c - 'A' + 'a');
		}
	}
	for (int i = 0; i < seen->length; ++i) {
		if (strcmp(seen->items[i], key) == 0) {
			free(key);
			return true;
		}
	}
	list_add(seen, key);
	return false;
}

// Words that find an entry without being shown: its English name, the
// program's file name ("code" for Visual Studio Code) and the shortcut's
// description. NULL if there are none.
static char *entry_keywords(const char *name, const char *english,
		const wchar_t *target, const wchar_t *description) {
	char *words = swine_strdup("");
	if (english && strcmp(english, name) != 0) {
		char *next = format_str("%s %s", words, english);
		free(words);
		words = next;
	}
	const wchar_t *file = file_part(target);
	const wchar_t *dot = wcsrchr(file, L'.');
	if (file[0] && dot && _wcsicmp(dot, L".exe") == 0) {
		char *stem = swinemenu_utf16_to_utf8(file);
		stem[strlen(stem) - 4] = '\0';
		char *next = format_str("%s %s", words, stem);
		free(words);
		free(stem);
		words = next;
	}
	if (description && description[0]) {
		char *d = swinemenu_utf16_to_utf8(description);
		if (strcmp(d, name) != 0) {
			char *next = format_str("%s %s", words, d);
			free(words);
			words = next;
		}
		free(d);
	}
	if (!words[0]) {
		free(words);
		return NULL;
	}
	char *trimmed = swine_strdup(words + 1);
	free(words);
	return trimmed;
}

static void add_shortcuts(list_t *entries, list_t *seen, const wchar_t *dir,
		IShellLinkW *link_obj, int depth) {
	if (depth > 8) {
		return;
	}
	wchar_t pattern[MAX_PATH];
	swprintf(pattern, MAX_PATH, L"%ls\\*", dir);
	WIN32_FIND_DATAW data;
	HANDLE find = FindFirstFileExW(pattern, FindExInfoBasic, &data,
		FindExSearchNameMatch, NULL, 0);
	if (find == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		if (wcscmp(data.cFileName, L".") == 0 ||
				wcscmp(data.cFileName, L"..") == 0) {
			continue;
		}
		wchar_t path[MAX_PATH];
		swprintf(path, MAX_PATH, L"%ls\\%ls", dir, data.cFileName);
		if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			add_shortcuts(entries, seen, path, link_obj, depth + 1);
			continue;
		}
		wchar_t *dot = wcsrchr(data.cFileName, L'.');
		bool is_lnk = dot && _wcsicmp(dot, L".lnk") == 0;
		// .url: Steam and other launchers' games (steam://rungameid/...).
		bool is_url = dot && _wcsicmp(dot, L".url") == 0;
		if (!is_lnk && !is_url) {
			continue;
		}
		struct shortcut link;
		if (is_lnk) {
			read_shortcut(link_obj, path, &link);
		} else {
			memset(&link, 0, sizeof(link));
		}
		*dot = L'\0';
		char *file_name = swinemenu_utf16_to_utf8(data.cFileName);
		char *translated = localized_name(path);
		const char *name = translated ? translated : file_name;
		bool skip = has_entry(entries, name) ||
			swine_strncasecmp(file_name, "Uninstall", 9) == 0 ||
			(link.target[0] && (is_document(link.target) ||
				is_uninstaller(link.target, link.args))) ||
			seen_target(seen, link.target, link.args);
		if (!skip) {
			char *path8 = swinemenu_utf16_to_utf8(path);
			char *command = format_str("\"%s\"", path8);
			struct menu_entry *entry = menu_entry_create(name, command);
			entry->icon = swine_strdup(path8);
			entry->keywords = entry_keywords(name, file_name, link.target,
				link.description);
			list_add(entries, entry);
			free(command);
			free(path8);
		}
		free(translated);
		free(file_name);
	} while (FindNextFileW(find, &data));
	FindClose(find);
}

static void add_known_folder(list_t *entries, list_t *seen,
		REFKNOWNFOLDERID id, IShellLinkW *link_obj) {
	wchar_t *dir = NULL;
	if (SUCCEEDED(SHGetKnownFolderPath(id, 0, NULL, &dir))) {
		add_shortcuts(entries, seen, dir, link_obj, 0);
	}
	CoTaskMemFree(dir);
}

// Store apps from the Applications folder: their parsing name is the
// AppUserModelID, "<PackageFamilyName>!<AppId>". Desktop programs appear
// there too (with other kinds of IDs); they come from the shortcuts.
static void add_store_apps(list_t *entries) {
	IShellItem *folder = NULL;
	if (FAILED(SHGetKnownFolderItem(&FOLDERID_AppsFolder, KF_FLAG_DEFAULT,
			NULL, &IID_IShellItem, (void **)&folder))) {
		return;
	}
	IEnumShellItems *items = NULL;
	if (SUCCEEDED(IShellItem_BindToHandler(folder, NULL, &BHID_EnumItems,
			&IID_IEnumShellItems, (void **)&items))) {
		IShellItem *item = NULL;
		while (IEnumShellItems_Next(items, 1, &item, NULL) == S_OK) {
			wchar_t *aumid = NULL, *display = NULL;
			if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_PARENTRELATIVEPARSING,
					&aumid)) && wcschr(aumid, L'!') && !wcschr(aumid, L'\\') &&
					SUCCEEDED(IShellItem_GetDisplayName(item,
					SIGDN_NORMALDISPLAY, &display))) {
				char *name = swinemenu_utf16_to_utf8(display);
				if (!has_entry(entries, name)) {
					char *id = swinemenu_utf16_to_utf8(aumid);
					char *command = format_str("shell:AppsFolder\\%s", id);
					struct menu_entry *entry = menu_entry_create(name, command);
					entry->icon = swine_strdup(command);
					list_add(entries, entry);
					free(command);
					free(id);
				}
				free(name);
			}
			CoTaskMemFree(aumid);
			CoTaskMemFree(display);
			IShellItem_Release(item);
		}
		IEnumShellItems_Release(items);
	}
	IShellItem_Release(folder);
}

// A program's FileDescription ("Google Chrome"), or NULL.
static char *file_description(const wchar_t *path) {
	DWORD handle = 0;
	DWORD size = GetFileVersionInfoSizeW(path, &handle);
	if (!size) {
		return NULL;
	}
	void *info = malloc(size);
	char *result = NULL;
	if (GetFileVersionInfoW(path, 0, size, info)) {
		struct { WORD language, codepage; } *tr = NULL;
		UINT len = 0;
		wchar_t query[64];
		if (VerQueryValueW(info, L"\\VarFileInfo\\Translation", (void **)&tr,
				&len) && len >= sizeof(*tr)) {
			swprintf(query, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription",
				tr->language, tr->codepage);
		} else {
			wcscpy(query, L"\\StringFileInfo\\040904b0\\FileDescription");
		}
		wchar_t *text = NULL;
		if (VerQueryValueW(info, query, (void **)&text, &len) && len > 1 &&
				text[0]) {
			result = swinemenu_utf16_to_utf8(text);
			strip_whitespace(result);
			if (!result[0]) {
				free(result);
				result = NULL;
			}
		}
	}
	free(info);
	return result;
}

// The programs registered under App Paths (what Win+R starts by name),
// for each the key's name ("chrome.exe") and its path. Caller frees both
// lists.
static void read_app_paths(list_t *names, list_t *paths) {
	static const HKEY roots[] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
	for (size_t r = 0; r < sizeof(roots) / sizeof(roots[0]); ++r) {
		HKEY key;
		if (RegOpenKeyExW(roots[r],
				L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths", 0,
				KEY_READ, &key) != ERROR_SUCCESS) {
			continue;
		}
		wchar_t name[256];
		for (DWORD i = 0; ; ++i) {
			DWORD len = 256;
			if (RegEnumKeyExW(key, i, name, &len, NULL, NULL, NULL, NULL) !=
					ERROR_SUCCESS) {
				break;
			}
			wchar_t value[MAX_PATH];
			DWORD size = sizeof(value);
			// The default value; RRF_RT_REG_SZ takes REG_EXPAND_SZ too,
			// expanded (asking for that type itself needs RRF_NOEXPAND).
			if (RegGetValueW(key, name, NULL, RRF_RT_REG_SZ, NULL, value,
					&size) != ERROR_SUCCESS) {
				continue;
			}
			wchar_t *p = value;
			if (*p == L'"') {
				++p;
				wchar_t *quote = wcschr(p, L'"');
				if (quote) {
					*quote = L'\0';
				}
			}
			const wchar_t *dot = wcsrchr(file_part(p), L'.');
			if (!dot || _wcsicmp(dot, L".exe") != 0 ||
					GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) {
				continue;
			}
			list_add(names, swinemenu_utf16_to_utf8(name));
			list_add(paths, swinemenu_utf16_to_utf8(p));
		}
		RegCloseKey(key);
	}
}

// Programs from App Paths that no shortcut starts, named by their
// FileDescription.
static void add_app_paths(list_t *entries, list_t *seen) {
	list_t *names = create_list(), *paths = create_list();
	read_app_paths(names, paths);
	for (int i = 0; i < paths->length; ++i) {
		wchar_t *path = swinemenu_utf8_to_utf16(paths->items[i]);
		if (is_uninstaller(path, L"") || seen_target(seen, path, L"")) {
			free(path);
			continue;
		}
		char *stem = swine_strdup(names->items[i]);
		char *dot = strrchr(stem, '.');
		if (dot) {
			*dot = '\0';
		}
		char *description = file_description(path);
		const char *name = description ? description : stem;
		if (!has_entry(entries, name)) {
			char *command = format_str("\"%s\"", (char *)paths->items[i]);
			struct menu_entry *entry = menu_entry_create(name, command);
			entry->icon = swine_strdup(paths->items[i]);
			if (description) {
				entry->keywords = swine_strdup(stem);
			}
			list_add(entries, entry);
			free(command);
		}
		free(description);
		free(stem);
		free(path);
	}
	list_free_items_and_destroy(names);
	list_free_items_and_destroy(paths);
}

list_t *swinemenu_drun_entries(void) {
	list_t *entries = create_list();
	list_t *seen = create_list();   // char *: target|arguments, lower case
	IShellLinkW *link_obj = NULL;
	CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
		&IID_IShellLinkW, (void **)&link_obj);
	add_known_folder(entries, seen, &FOLDERID_Programs, link_obj);
	add_known_folder(entries, seen, &FOLDERID_CommonPrograms, link_obj);
	if (link_obj) {
		IShellLinkW_Release(link_obj);
	}
	add_store_apps(entries);
	add_app_paths(entries, seen);
	list_free_items_and_destroy(seen);
	return entries;
}

list_t *swinemenu_run_entries(void) {
	list_t *entries = create_list();
	DWORD n = GetEnvironmentVariableW(L"PATH", NULL, 0);
	if (n == 0) {
		return entries;
	}
	wchar_t *path = malloc(sizeof(wchar_t) * n);
	GetEnvironmentVariableW(L"PATH", path, n);
	for (wchar_t *dir = path, *next; dir; dir = next) {
		next = wcschr(dir, L';');
		if (next) {
			*next++ = L'\0';
		}
		if (!*dir) {
			continue;
		}
		wchar_t pattern[MAX_PATH];
		swprintf(pattern, MAX_PATH, L"%ls\\*", dir);
		WIN32_FIND_DATAW data;
		HANDLE find = FindFirstFileExW(pattern, FindExInfoBasic, &data,
			FindExSearchNameMatch, NULL, 0);
		if (find == INVALID_HANDLE_VALUE) {
			continue;
		}
		do {
			if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				continue;
			}
			wchar_t *dot = wcsrchr(data.cFileName, L'.');
			if (!dot || (_wcsicmp(dot, L".exe") != 0 &&
					_wcsicmp(dot, L".bat") != 0 && _wcsicmp(dot, L".cmd") != 0 &&
					_wcsicmp(dot, L".com") != 0)) {
				continue;
			}
			char *file = swinemenu_utf16_to_utf8(data.cFileName);
			if (!has_entry(entries, file)) {
				list_add(entries, menu_entry_create(file, file));
			}
			free(file);
		} while (FindNextFileW(find, &data));
		FindClose(find);
	}
	free(path);
	// What Win+R also starts by name; ShellExecute finds them.
	list_t *names = create_list(), *paths = create_list();
	read_app_paths(names, paths);
	for (int i = 0; i < names->length; ++i) {
		if (!has_entry(entries, names->items[i])) {
			list_add(entries, menu_entry_create(names->items[i],
				names->items[i]));
		}
	}
	list_free_items_and_destroy(names);
	list_free_items_and_destroy(paths);
	return entries;
}

list_t *swinemenu_dmenu_entries(void) {
	list_t *entries = create_list();
	HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	if (!in || in == INVALID_HANDLE_VALUE) {
		return entries;
	}
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	DWORD n = 0;
	for (;;) {
		if (cap - len < 2048) {
			cap *= 2;
			buf = realloc(buf, cap);
		}
		if (!ReadFile(in, buf + len, (DWORD)(cap - len - 1), &n, NULL) || n == 0) {
			break;
		}
		len += n;
	}
	buf[len] = '\0';
	char *p = buf;
	// A UTF-8 byte order mark (PowerShell may send one).
	if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
			(unsigned char)p[2] == 0xBF) {
		p += 3;
	}
	while (*p) {
		size_t line = strcspn(p, "\r\n");
		if (line > 0) {
			char *text = swine_strndup(p, line);
			list_add(entries, menu_entry_create(text, NULL));
			free(text);
		}
		p += line;
		while (*p == '\r' || *p == '\n') {
			++p;
		}
	}
	free(buf);
	return entries;
}

// The program file of a process, for its icon.
static char *process_path(unsigned long pid) {
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
		(DWORD)pid);
	if (!process) {
		return NULL;
	}
	wchar_t buf[MAX_PATH];
	DWORD len = MAX_PATH;
	char *path = QueryFullProcessImageNameW(process, 0, buf, &len) ?
		swinemenu_utf16_to_utf8(buf) : NULL;
	CloseHandle(process);
	return path;
}

static void add_windows(list_t *entries, const struct json_value *node,
		const char *workspace) {
	const char *type = json_get_string(node, "type", "");
	if (strcmp(type, "workspace") == 0) {
		workspace = json_get_string(node, "name", workspace);
		if (strcmp(workspace, "__i3_scratch") == 0) {
			workspace = "scratchpad";
		}
	}
	struct json_value *pid = json_object_get(node, "pid");
	if (pid && pid->type == JSON_NUMBER &&
			(strcmp(type, "con") == 0 || strcmp(type, "floating_con") == 0)) {
		const char *title = json_get_string(node, "name", "");
		const char *app = json_get_string(node, "app_id", "");
		char *app_name = swine_strdup(app ? app : "");
		size_t len = strlen(app_name);
		if (len > 4 && swine_strcasecmp(app_name + len - 4, ".exe") == 0) {
			app_name[len - 4] = '\0';
		}
		// rofi's window mode: workspace, program, title
		char *name = format_str("%s  %s  %s", workspace ? workspace : "",
			app_name, title ? title : "");
		char *command = format_str("[con_id=%lld] focus",
			(long long)json_get_number(node, "id", 0));
		struct menu_entry *entry = menu_entry_create(name, command);
		entry->icon = process_path((unsigned long)pid->number);
		list_add(entries, entry);
		free(name);
		free(command);
		free(app_name);
	}
	const char *children[] = {"nodes", "floating_nodes"};
	for (size_t k = 0; k < 2; ++k) {
		struct json_value *list = json_object_get(node, children[k]);
		for (int i = 0; list && list->type == JSON_ARRAY &&
				i < list->items->length; ++i) {
			add_windows(entries, list->items->items[i], workspace);
		}
	}
}

list_t *swinemenu_window_entries(void) {
	list_t *entries = create_list();
	char *reply = ipc_request(NULL, IPC_GET_TREE, "", NULL);
	struct json_value *tree = reply ? json_parse(reply, NULL) : NULL;
	if (tree) {
		add_windows(entries, tree, NULL);
	}
	json_value_free(tree);
	free(reply);
	return entries;
}
