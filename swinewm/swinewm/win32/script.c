// Bar scripts (platform_script_start): Waybar runs a custom module's "exec"
// with sh -c; here it is cmd.exe /c, without a console window, so pipes,
// redirections, %VARIABLES% and .bat files work as in a terminal.
//
// Each script runs in its own job object (kill on close): stopping it ends
// everything it started (cmd.exe, powershell.exe, ...), and if swinewm
// itself dies Windows closes the jobs and nothing keeps running. A thread
// per script reads its output and posts each line to the shell window.
#include <stdlib.h>
#include <string.h>
#include "list.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/bar/custom.h"
#include "swinewm/platform.h"
#include "win32.h"

struct script_job {
	uint64_t id;
	HANDLE job;
};

// What the reader thread owns.
struct script_reader {
	uint64_t id;
	HANDLE process;
	HANDLE pipe;
};

// Posted to the shell window: a line, or the end (line NULL).
struct script_message {
	uint64_t id;
	char *line;
	int status;
};

static list_t *jobs = NULL;   // struct script_job *, main thread only
static uint64_t next_id = 1;

static void post(uint64_t id, char *line, int status) {
	struct script_message *m = malloc(sizeof(*m));
	m->id = id;
	m->line = line;
	m->status = status;
	if (!win32_shell_hwnd ||
			!PostMessageW(win32_shell_hwnd, WM_SWINE_SCRIPT, 0, (LPARAM)m)) {
		free(m->line);
		free(m);
	}
}

// Scripts print in UTF-8 or in the console's code page (cmd's echo,
// Windows PowerShell): take UTF-8 when it is valid.
static char *decode_line(const char *raw, int len) {
	if (len == 0) {
		return swine_strdup("");
	}
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw, len, NULL, 0)) {
		return swine_strndup(raw, (size_t)len);
	}
	UINT cp = GetOEMCP();
	int n = MultiByteToWideChar(cp, 0, raw, len, NULL, 0);
	wchar_t *wide = calloc((size_t)n + 1, sizeof(wchar_t));
	MultiByteToWideChar(cp, 0, raw, len, wide, n);
	char *utf8 = utf16_to_utf8(wide);
	free(wide);
	return utf8 ? utf8 : swine_strdup("");
}

static DWORD WINAPI reader_thread(LPVOID param) {
	struct script_reader *r = param;
	char *buf = NULL;
	size_t len = 0, cap = 0;
	char chunk[4096];
	DWORD got;
	while (ReadFile(r->pipe, chunk, sizeof(chunk), &got, NULL) && got > 0) {
		if (len + got + 1 > cap) {
			cap = (len + got + 1) * 2;
			buf = realloc(buf, cap);
		}
		memcpy(buf + len, chunk, got);
		len += got;
		// Post every complete line, without "\r\n".
		size_t start = 0;
		for (size_t i = 0; i < len; ++i) {
			if (buf[i] != '\n') {
				continue;
			}
			size_t end = i;
			if (end > start && buf[end - 1] == '\r') {
				end--;
			}
			post(r->id, decode_line(buf + start, (int)(end - start)), 0);
			start = i + 1;
		}
		memmove(buf, buf + start, len - start);
		len -= start;
	}
	if (len > 0) {
		if (buf[len - 1] == '\r') {
			len--;
		}
		post(r->id, decode_line(buf, (int)len), 0);
	}
	free(buf);
	WaitForSingleObject(r->process, INFINITE);
	DWORD status = 1;
	GetExitCodeProcess(r->process, &status);
	post(r->id, NULL, (int)status);
	CloseHandle(r->pipe);
	CloseHandle(r->process);
	free(r);
	return 0;
}

uint64_t platform_script_start(const char *cmdline, const char *output_name) {
	wchar_t comspec[MAX_PATH];
	if (!GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH)) {
		wcscpy(comspec, L"cmd.exe");
	}
	char *comspec_utf8 = utf16_to_utf8(comspec);
	// /s: everything after /c is the command line, quotes and all.
	char *line = format_str("\"%s\" /d /s /c \"%s\"", comspec_utf8, cmdline);
	free(comspec_utf8);
	wchar_t *wide = utf8_to_utf16(line);
	free(line);

	SECURITY_ATTRIBUTES sa = { (DWORD)sizeof(sa), NULL, TRUE };
	HANDLE read_end = NULL, write_end = NULL;
	if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
		free(wide);
		return 0;
	}
	SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
	HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);

	STARTUPINFOW si = { .cb = sizeof(si) };
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = null_in;
	si.hStdOutput = write_end;
	si.hStdError = null_in;
	PROCESS_INFORMATION pi = {0};

	// The child inherits the environment: set WAYBAR_OUTPUT_NAME for it.
	wchar_t *output = output_name ? utf8_to_utf16(output_name) : NULL;
	SetEnvironmentVariableW(L"WAYBAR_OUTPUT_NAME", output);
	BOOL ok = CreateProcessW(NULL, wide, NULL, NULL, TRUE,
		CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
		NULL, NULL, &si, &pi);
	SetEnvironmentVariableW(L"WAYBAR_OUTPUT_NAME", NULL);
	free(output);
	free(wide);
	CloseHandle(write_end);
	if (null_in != INVALID_HANDLE_VALUE) {
		CloseHandle(null_in);
	}
	if (!ok) {
		swine_log(SWINE_ERROR, "Unable to start bar script '%s' (error %lu)",
			cmdline, GetLastError());
		CloseHandle(read_end);
		return 0;
	}

	HANDLE job = CreateJobObjectW(NULL, NULL);
	if (job) {
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
		limits.BasicLimitInformation.LimitFlags =
			JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(job, JobObjectExtendedLimitInformation,
			&limits, sizeof(limits));
		if (!AssignProcessToJobObject(job, pi.hProcess)) {
			CloseHandle(job);
			job = NULL;
		}
	}
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);

	struct script_reader *r = malloc(sizeof(*r));
	r->id = next_id++;
	r->process = pi.hProcess;
	r->pipe = read_end;
	uint64_t id = r->id;
	HANDLE thread = CreateThread(NULL, 0, reader_thread, r, 0, NULL);
	if (!thread) {
		if (job) {
			TerminateJobObject(job, 1);
			CloseHandle(job);
		} else {
			TerminateProcess(pi.hProcess, 1);
		}
		CloseHandle(read_end);
		CloseHandle(pi.hProcess);
		free(r);
		return 0;
	}
	CloseHandle(thread);

	struct script_job *j = malloc(sizeof(*j));
	j->id = id;
	j->job = job;
	if (!jobs) {
		jobs = create_list();
	}
	list_add(jobs, j);
	return id;
}

static int find_job(uint64_t id) {
	for (int i = 0; jobs && i < jobs->length; ++i) {
		struct script_job *j = jobs->items[i];
		if (j->id == id) {
			return i;
		}
	}
	return -1;
}

static void remove_job(int index, bool kill) {
	struct script_job *j = jobs->items[index];
	if (j->job) {
		if (kill) {
			TerminateJobObject(j->job, 1);
		}
		CloseHandle(j->job);
	}
	free(j);
	list_del(jobs, index);
}

void platform_script_stop(uint64_t id) {
	int index = find_job(id);
	if (index >= 0) {
		remove_job(index, true);
	}
}

void win32_script_message(LPARAM lparam) {
	struct script_message *m = (struct script_message *)lparam;
	int index = find_job(m->id);
	if (index >= 0) {   // otherwise stopped: it reports nothing more
		if (m->line) {
			bar_script_line(m->id, m->line);
		} else {
			// Closing the job ends what the script left running in the
			// background, as Waybar's process group would.
			remove_job(index, true);
			bar_script_exited(m->id, m->status);
		}
	}
	free(m->line);
	free(m);
}

void win32_scripts_finish(void) {
	while (jobs && jobs->length) {
		remove_job(jobs->length - 1, true);
	}
	if (jobs) {
		list_free(jobs);
		jobs = NULL;
	}
}
