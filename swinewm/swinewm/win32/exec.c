#include <stdlib.h>
#include <wchar.h>
#include "log.h"
#include "swinewm/platform.h"
#include "win32.h"
#include <shellapi.h>

// sway runs exec commands through "sh -c". Here the command line goes to
// CreateProcess, which searches PATH and App Paths aliases (wt.exe, ...).
// Things CreateProcess cannot start (documents, URLs, shell: folders) are
// handed to ShellExecute. Use "cmd /c ..." for shell syntax.
bool platform_spawn(const char *cmdline) {
	wchar_t *wide = utf8_to_utf16(cmdline);
	if (!wide) {
		return false;
	}
	STARTUPINFOW si = { .cb = sizeof(si) };
	PROCESS_INFORMATION pi = {0};
	// CreateProcessW may modify the command line buffer, so pass our copy.
	BOOL ok = CreateProcessW(NULL, wide, NULL, NULL, FALSE,
		CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT,
		NULL, NULL, &si, &pi);
	if (ok) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		free(wide);
		return true;
	}
	DWORD error = GetLastError();

	// Split off the first (possibly quoted) token for ShellExecute.
	wchar_t *file = wide;
	wchar_t *params = NULL;
	if (*file == L'"') {
		++file;
		wchar_t *end = wcschr(file, L'"');
		if (end) {
			*end = L'\0';
			params = end + 1;
		}
	} else {
		wchar_t *space = wcschr(file, L' ');
		if (space) {
			*space = L'\0';
			params = space + 1;
		}
	}
	while (params && *params == L' ') {
		++params;
	}
	SHELLEXECUTEINFOW sei = { .cbSize = sizeof(sei) };
	sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
	sei.lpVerb = L"open";
	sei.lpFile = file;
	sei.lpParameters = params && *params ? params : NULL;
	sei.nShow = SW_SHOWNORMAL;
	ok = ShellExecuteExW(&sei);
	free(wide);
	if (!ok) {
		char *msg = win32_error_string(error);
		swine_log(SWINE_ERROR, "Unable to start '%s': %s", cmdline, msg);
		free(msg);
	}
	return ok != FALSE;
}
