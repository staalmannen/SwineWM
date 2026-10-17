#define COBJMACROS
#include <stdlib.h>
#include <wchar.h>
#include "log.h"
#include "swinewm/platform.h"
#include "win32.h"
#include <shellapi.h>
#include <shobjidl.h>

// Store (packaged) apps: "shell:AppsFolder\<AppUserModelID> [arguments]".
// ShellExecute fails for these with "Class not registered" when swinewm is
// the shell (the shell namespace needs Explorer as the shell), but the
// documented IApplicationActivationManager starts them without Explorer.
// The IDs are listed by PowerShell's Get-StartApps.
static const wchar_t apps_folder[] = L"shell:AppsFolder\\";

// Defined here rather than taken from uuid.lib, which may lack them.
static const CLSID clsid_activation_manager = {0x45ba127d, 0x10a8, 0x46ea,
	{0x8a, 0xb7, 0x56, 0xea, 0x90, 0x78, 0x94, 0x3c}};
static const IID iid_activation_manager = {0x2e941141, 0x7f97, 0x4756,
	{0xba, 0x1d, 0x9d, 0xec, 0xde, 0x89, 0x4a, 0x3d}};

// Returns true if cmdline named a Store app; *started says whether it started.
static bool activate_store_app(const wchar_t *cmdline, const char *utf8,
		bool *started) {
	const wchar_t *p = cmdline;
	bool quoted = *p == L'"';
	if (quoted) {
		++p;
	}
	size_t prefix = wcslen(apps_folder);
	if (_wcsnicmp(p, apps_folder, prefix) != 0) {
		return false;
	}
	p += prefix;
	const wchar_t *end = p + wcscspn(p, quoted ? L"\"" : L" ");
	wchar_t *aumid = calloc((size_t)(end - p) + 1, sizeof(wchar_t));
	wcsncpy(aumid, p, (size_t)(end - p));
	const wchar_t *args = *end ? end + 1 : end;
	while (*args == L' ') {
		++args;
	}

	// Startup programs are spawned from another thread, so make sure COM is
	// initialized here (S_FALSE or RPC_E_CHANGED_MODE if it already is).
	HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	IApplicationActivationManager *manager = NULL;
	DWORD pid = 0;
	// CLSCTX_ALL, as PowerShell uses (which started Store apps in testing
	// when CLSCTX_LOCAL_SERVER alone did not).
	HRESULT hr = CoCreateInstance(&clsid_activation_manager, NULL,
		CLSCTX_ALL, &iid_activation_manager, (void **)&manager);
	if (SUCCEEDED(hr)) {
		hr = IApplicationActivationManager_ActivateApplication(manager, aumid,
			*args ? args : NULL, AO_NONE, &pid);
		IApplicationActivationManager_Release(manager);
	}
	if (SUCCEEDED(init)) {
		CoUninitialize();
	}
	if (SUCCEEDED(hr)) {
		swine_log(SWINE_DEBUG, "Started Store app '%s' (pid %lu)", utf8, pid);
	} else {
		char *msg = win32_error_string((DWORD)hr);
		char *id = utf16_to_utf8(aumid);
		swine_log(SWINE_ERROR, "Unable to start Store app '%s' (AUMID '%s'): "
			"%s (0x%08lx)", utf8, id ? id : "", msg, (unsigned long)hr);
		free(id);
		free(msg);
	}
	free(aumid);
	*started = SUCCEEDED(hr);
	return true;
}

// sway runs exec commands through "sh -c". Here the command line goes to
// CreateProcess, which searches PATH and App Paths aliases (wt.exe, ...).
// Things CreateProcess cannot start (documents, URLs, shell: folders) are
// handed to ShellExecute. %VAR% references are expanded first. Use
// "cmd /c ..." for other shell syntax.
bool platform_spawn(const char *cmdline) {
	wchar_t *raw = utf8_to_utf16(cmdline);
	if (!raw) {
		return false;
	}
	// Expand %VAR% like shortcuts and the Run dialog do, so that commands
	// such as %LOCALAPPDATA%\FlowLauncher\Flow.Launcher.exe work.
	DWORD n = ExpandEnvironmentStringsW(raw, NULL, 0);
	wchar_t *wide = raw;
	if (n > 0) {
		wide = malloc(sizeof(wchar_t) * n);
		ExpandEnvironmentStringsW(raw, wide, n);
		free(raw);
	}
	bool started = false;
	if (activate_store_app(wide, cmdline, &started)) {
		free(wide);
		return started;
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

	// The whole command line may be one path with spaces: sway removes the
	// quotes of `exec "C:\Start Menu\App.lnk"` (one quoted argument), and
	// swinemenu's Start menu entries are such paths. Shortcuts and documents
	// are opened by ShellExecute.
	DWORD attributes = GetFileAttributesW(wide);
	if (attributes != INVALID_FILE_ATTRIBUTES) {
		SHELLEXECUTEINFOW sei = { .cbSize = sizeof(sei) };
		sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
		sei.lpVerb = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? NULL : L"open";
		sei.lpFile = wide;
		sei.nShow = SW_SHOWNORMAL;
		ok = ShellExecuteExW(&sei);
		if (!ok) {
			char *msg = win32_error_string(GetLastError());
			swine_log(SWINE_ERROR, "Unable to open '%s': %s", cmdline, msg);
			free(msg);
		}
		free(wide);
		return ok != FALSE;
	}

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
