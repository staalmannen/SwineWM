// A companion process restores the desktop if the window manager exits
// unexpectedly or stops servicing its message loop.
#include <stdlib.h>
#include <wchar.h>
#include "log.h"
#include "win32.h"

static HANDLE exit_event;
static HANDLE watcher_process;

static bool event_name(DWORD pid, const wchar_t *suffix, wchar_t *name,
		size_t capacity) {
	return swprintf(name, capacity, L"Local\\SwineWM-watch-%ls-%lu", suffix,
		(unsigned long)pid) >= 0;
}

void win32_watch_signal_exit(void) {
	if (exit_event && !SetEvent(exit_event)) {
		swine_log(SWINE_ERROR, "Unable to signal watchdog shutdown (%lu)",
			GetLastError());
	}
}

void win32_watch_start(void) {
	DWORD pid = GetCurrentProcessId();
	wchar_t clean_name[64];
	wchar_t ready_name[64];
	if (!event_name(pid, L"exit", clean_name,
			sizeof(clean_name) / sizeof(clean_name[0])) ||
			!event_name(pid, L"ready", ready_name,
			sizeof(ready_name) / sizeof(ready_name[0]))) {
		swine_log(SWINE_ERROR, "Unable to create watchdog event names");
		return;
	}
	SetLastError(ERROR_SUCCESS);
	exit_event = CreateEventW(NULL, TRUE, FALSE, clean_name);
	if (!exit_event) {
		swine_log(SWINE_ERROR, "Unable to create watchdog exit event (%lu)",
			GetLastError());
		return;
	}
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		swine_log(SWINE_ERROR, "Watchdog exit event already exists for %lu", pid);
		CloseHandle(exit_event);
		exit_event = NULL;
		return;
	}
	SetLastError(ERROR_SUCCESS);
	HANDLE ready_event = CreateEventW(NULL, TRUE, FALSE, ready_name);
	if (!ready_event || GetLastError() == ERROR_ALREADY_EXISTS) {
		swine_log(SWINE_ERROR, "Unable to create watchdog ready event (%lu)",
			GetLastError());
		if (ready_event) {
			CloseHandle(ready_event);
		}
		CloseHandle(exit_event);
		exit_event = NULL;
		return;
	}

	wchar_t executable[MAX_PATH];
	DWORD length = GetModuleFileNameW(NULL, executable, MAX_PATH);
	if (!length || length >= MAX_PATH) {
		swine_log(SWINE_ERROR, "Unable to find executable for watchdog");
		CloseHandle(ready_event);
		CloseHandle(exit_event);
		exit_event = NULL;
		return;
	}
	wchar_t command[MAX_PATH + 64];
	if (swprintf(command, sizeof(command) / sizeof(command[0]),
			L"\"%ls\" --watch %lu %d", executable, (unsigned long)pid,
			win32_shell_mode ? 1 : 0) < 0) {
		swine_log(SWINE_ERROR, "Unable to build watchdog command line");
		CloseHandle(ready_event);
		CloseHandle(exit_event);
		exit_event = NULL;
		return;
	}
	STARTUPINFOW startup = { .cb = sizeof(startup) };
	PROCESS_INFORMATION process = {0};
	if (!CreateProcessW(executable, command, NULL, NULL, FALSE,
			CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
		swine_log(SWINE_ERROR, "Unable to start watchdog (%lu)", GetLastError());
		CloseHandle(ready_event);
		CloseHandle(exit_event);
		exit_event = NULL;
		return;
	}
	CloseHandle(process.hThread);
	DWORD wait = WaitForSingleObject(ready_event, 5000);
	CloseHandle(ready_event);
	if (wait != WAIT_OBJECT_0) {
		swine_log(SWINE_ERROR, "Crash watchdog did not become ready for %lu", pid);
		if (TerminateProcess(process.hProcess, ERROR_PROCESS_ABORTED)) {
			WaitForSingleObject(process.hProcess, 1000);
			CloseHandle(process.hProcess);
			CloseHandle(exit_event);
			exit_event = NULL;
		} else {
			swine_log(SWINE_ERROR, "Unable to stop failed watchdog (%lu)",
				GetLastError());
			watcher_process = process.hProcess;
		}
		return;
	}
	watcher_process = process.hProcess;
	swine_log(SWINE_INFO, "Started crash watchdog for process %lu", pid);
}

void win32_watch_finish(void) {
	win32_watch_signal_exit();
	if (exit_event) {
		CloseHandle(exit_event);
		exit_event = NULL;
	}
	if (watcher_process) {
		CloseHandle(watcher_process);
		watcher_process = NULL;
	}
}

bool win32_watch_responsible(void) {
	return watcher_process &&
		WaitForSingleObject(watcher_process, 0) == WAIT_TIMEOUT;
}

static int recover_owner(HANDLE owner, DWORD pid, bool terminate_owner) {
	bool owner_stopped = WaitForSingleObject(owner, 0) == WAIT_OBJECT_0;
	if (terminate_owner && !owner_stopped) {
		if (!TerminateProcess(owner, ERROR_PROCESS_ABORTED) &&
				WaitForSingleObject(owner, 0) != WAIT_OBJECT_0) {
			swine_log(SWINE_ERROR, "Unable to stop hung swinewm process %lu (%lu)",
				pid, GetLastError());
			return EXIT_FAILURE;
		}
		if (WaitForSingleObject(owner, 10000) != WAIT_OBJECT_0) {
			swine_log(SWINE_ERROR,
				"Hung swinewm process %lu did not stop after termination", pid);
			return EXIT_FAILURE;
		}
		owner_stopped = true;
	}
	if (!owner_stopped) {
		swine_log(SWINE_ERROR, "Refusing recovery while process %lu is active",
			pid);
		return EXIT_FAILURE;
	}

	wchar_t executable[MAX_PATH];
	DWORD length = GetModuleFileNameW(NULL, executable, MAX_PATH);
	if (!length || length >= MAX_PATH) {
		swine_log(SWINE_ERROR, "Unable to find executable for recovery");
		if (win32_shell_mode) {
			win32_shell_rescue();
		}
		return EXIT_FAILURE;
	}
	wchar_t command[MAX_PATH + 32];
	if (swprintf(command, sizeof(command) / sizeof(command[0]),
			L"\"%ls\" --recover", executable) < 0) {
		swine_log(SWINE_ERROR, "Unable to build recovery command line");
		if (win32_shell_mode) {
			win32_shell_rescue();
		}
		return EXIT_FAILURE;
	}
	STARTUPINFOW startup = { .cb = sizeof(startup) };
	PROCESS_INFORMATION process = {0};
	if (!CreateProcessW(executable, command, NULL, NULL, FALSE,
			CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
		swine_log(SWINE_ERROR, "Unable to start recovery (%lu)", GetLastError());
		if (win32_shell_mode) {
			win32_shell_rescue();
		}
		return EXIT_FAILURE;
	}
	CloseHandle(process.hThread);
	swine_log(SWINE_INFO, "Recovering windows after swinewm process %lu failed",
		pid);
	DWORD wait = WaitForSingleObject(process.hProcess, 30000);
	DWORD status = EXIT_FAILURE;
	if (wait == WAIT_OBJECT_0) {
		if (!GetExitCodeProcess(process.hProcess, &status)) {
			swine_log(SWINE_ERROR, "Unable to read recovery exit status (%lu)",
				GetLastError());
			status = EXIT_FAILURE;
		}
	} else {
		swine_log(SWINE_ERROR, "Recovery process did not finish in time");
	}
	CloseHandle(process.hProcess);
	if (win32_shell_mode) {
		win32_shell_rescue();
	}
	return status == EXIT_SUCCESS ? EXIT_SUCCESS : EXIT_FAILURE;
}

int platform_watch(unsigned long process_id, bool shell_mode) {
	DWORD pid = (DWORD)process_id;
	win32_shell_mode = shell_mode;
	HANDLE owner = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
	if (!owner) {
		swine_log(SWINE_ERROR, "Unable to open swinewm process %lu (%lu)",
			pid, GetLastError());
		return EXIT_FAILURE;
	}
	wchar_t clean_name[64];
	wchar_t ready_name[64];
	if (!event_name(pid, L"exit", clean_name,
			sizeof(clean_name) / sizeof(clean_name[0])) ||
			!event_name(pid, L"ready", ready_name,
			sizeof(ready_name) / sizeof(ready_name[0]))) {
		CloseHandle(owner);
		swine_log(SWINE_ERROR, "Unable to create watchdog event names");
		return EXIT_FAILURE;
	}
	HANDLE clean_exit = OpenEventW(SYNCHRONIZE, FALSE, clean_name);
	if (!clean_exit) {
		CloseHandle(owner);
		swine_log(SWINE_ERROR, "Unable to open watchdog exit event (%lu)",
			GetLastError());
		return EXIT_FAILURE;
	}
	HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name);
	if (!ready) {
		CloseHandle(clean_exit);
		CloseHandle(owner);
		swine_log(SWINE_ERROR, "Unable to open watchdog ready event (%lu)",
			GetLastError());
		return EXIT_FAILURE;
	}
	if (!SetEvent(ready)) {
		CloseHandle(ready);
		CloseHandle(clean_exit);
		CloseHandle(owner);
		swine_log(SWINE_ERROR, "Unable to signal watchdog readiness (%lu)",
			GetLastError());
		return EXIT_FAILURE;
	}
	CloseHandle(ready);

	HANDLE handles[] = { clean_exit, owner };
	unsigned missed_heartbeats = 0;
	int status = EXIT_SUCCESS;
	swine_log(SWINE_INFO, "Watching swinewm process %lu", pid);
	for (;;) {
		DWORD wait = WaitForMultipleObjects(2, handles, FALSE, 2000);
		if (wait == WAIT_OBJECT_0) {
			swine_log(SWINE_INFO, "swinewm process %lu is shutting down", pid);
			break;
		}
		if (wait == WAIT_OBJECT_0 + 1) {
			if (WaitForSingleObject(clean_exit, 0) == WAIT_OBJECT_0) {
				break;
			}
			status = recover_owner(owner, pid, false);
			break;
		}
		if (wait == WAIT_FAILED) {
			swine_log(SWINE_ERROR, "Watchdog wait failed (%lu)", GetLastError());
			status = EXIT_FAILURE;
			break;
		}
		if (wait != WAIT_TIMEOUT) {
			swine_log(SWINE_ERROR, "Watchdog received unexpected wait result %lu",
				wait);
			status = EXIT_FAILURE;
			break;
		}

		HWND hwnd = FindWindowW(L"swinewm", NULL);
		DWORD window_pid = 0;
		bool responsive = hwnd &&
			GetWindowThreadProcessId(hwnd, &window_pid) && window_pid == pid;
		DWORD_PTR result = 0;
		if (responsive) {
			responsive = SendMessageTimeoutW(hwnd, WM_NULL, 0, 0,
				SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &result) != 0;
		}
		if (responsive) {
			missed_heartbeats = 0;
		} else if (++missed_heartbeats >= 3) {
			if (WaitForSingleObject(clean_exit, 0) == WAIT_OBJECT_0) {
				break;
			}
			swine_log(SWINE_ERROR,
				"swinewm process %lu stopped responding; terminating it", pid);
			status = recover_owner(owner, pid, true);
			break;
		}
	}
	CloseHandle(clean_exit);
	CloseHandle(owner);
	return status;
}
