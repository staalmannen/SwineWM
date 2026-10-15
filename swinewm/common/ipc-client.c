#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "ipc.h"
#include "ipc-client.h"
#include "stringop.h"

static char *narrow(const wchar_t *w) {
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	char *s = malloc((size_t)(n > 0 ? n : 1));
	if (n > 0) {
		WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
	} else {
		s[0] = '\0';
	}
	return s;
}

char *get_ipc_pipe_name(void) {
	wchar_t buf[512];
	DWORD n = GetEnvironmentVariableW(L"SWINESOCK", buf, 512);
	if (n > 0 && n < 512) {
		return narrow(buf);
	}
	DWORD size = 256;
	wchar_t user[256] = L"user";
	GetUserNameW(user, &size);
	char *user8 = narrow(user);
	char *name = format_str(SWINE_IPC_PIPE_FMT, user8);
	free(user8);
	return name;
}
#endif
