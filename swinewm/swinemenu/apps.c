// Where swinemenu's entries come from. Everything here uses documented
// shell APIs that work without Explorer as the shell: the Start menu
// folders are plain directories, and the Applications folder
// (FOLDERID_AppsFolder) lists Store apps with their AppUserModelIDs.
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
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

// Shortcuts to documents (readme files, manuals, web pages) are not apps.
static bool shortcut_is_app(IShellLinkW *link_obj, const wchar_t *path) {
	IPersistFile *file = NULL;
	if (FAILED(IShellLinkW_QueryInterface(link_obj, &IID_IPersistFile,
			(void **)&file))) {
		return true;
	}
	bool app = true;
	if (SUCCEEDED(IPersistFile_Load(file, path, STGM_READ))) {
		wchar_t target[MAX_PATH] = L"";
		if (SUCCEEDED(IShellLinkW_GetPath(link_obj, target, MAX_PATH, NULL,
				SLGP_RAWPATH)) && target[0]) {
			const wchar_t *dot = wcsrchr(target, L'.');
			static const wchar_t *documents[] = {
				L".txt", L".htm", L".html", L".pdf", L".chm", L".rtf",
				L".md", L".url", L".hlp", L".doc", L".docx",
			};
			for (size_t i = 0; dot && i < sizeof(documents) /
					sizeof(documents[0]); ++i) {
				if (_wcsicmp(dot, documents[i]) == 0) {
					app = false;
				}
			}
		}
	}
	IPersistFile_Release(file);
	return app;
}

static void add_shortcuts(list_t *entries, const wchar_t *dir,
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
			add_shortcuts(entries, path, link_obj, depth + 1);
			continue;
		}
		wchar_t *dot = wcsrchr(data.cFileName, L'.');
		bool is_lnk = dot && _wcsicmp(dot, L".lnk") == 0;
		// .url: Steam and other launchers' games (steam://rungameid/...).
		bool is_url = dot && _wcsicmp(dot, L".url") == 0;
		if (!is_lnk && !is_url) {
			continue;
		}
		*dot = L'\0';
		char *name = swinemenu_utf16_to_utf8(data.cFileName);
		bool skip = has_entry(entries, name) ||
			swine_strncasecmp(name, "Uninstall", 9) == 0 ||
			(is_lnk && link_obj && !shortcut_is_app(link_obj, path));
		if (!skip) {
			char *path8 = swinemenu_utf16_to_utf8(path);
			char *command = format_str("\"%s\"", path8);
			list_add(entries, menu_entry_create(name, command));
			free(command);
			free(path8);
		}
		free(name);
	} while (FindNextFileW(find, &data));
	FindClose(find);
}

static void add_known_folder(list_t *entries, REFKNOWNFOLDERID id,
		IShellLinkW *link_obj) {
	wchar_t *dir = NULL;
	if (SUCCEEDED(SHGetKnownFolderPath(id, 0, NULL, &dir))) {
		add_shortcuts(entries, dir, link_obj, 0);
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
					list_add(entries, menu_entry_create(name, command));
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

list_t *swinemenu_drun_entries(void) {
	list_t *entries = create_list();
	IShellLinkW *link_obj = NULL;
	CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
		&IID_IShellLinkW, (void **)&link_obj);
	add_known_folder(entries, &FOLDERID_Programs, link_obj);
	add_known_folder(entries, &FOLDERID_CommonPrograms, link_obj);
	if (link_obj) {
		IShellLinkW_Release(link_obj);
	}
	add_store_apps(entries);
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
