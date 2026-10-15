#ifndef _SWINE_WIN32_H
#define _SWINE_WIN32_H

// Internal interface between the files of the Win32 platform layer. Nothing
// outside swinewm/win32/ includes this or <windows.h>.

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

struct swine_view;

// Messages posted to the shell's own (hidden) window. Everything that
// touches the tree runs on the main thread; hooks and the IPC threads only
// post or send these.
#define WM_SWINE_BINDING (WM_APP + 1)  // lParam: malloc'ed UTF-8 command
#define WM_SWINE_IPC (WM_APP + 2)      // lParam: struct win32_ipc_request *
#define WM_SWINE_EVENTS (WM_APP + 3)   // drain queued WinEvents
#define WM_SWINE_SAVE_HIDDEN (WM_APP + 4)

extern HWND win32_shell_hwnd;

// util.c
wchar_t *utf8_to_utf16(const char *str);
char *utf16_to_utf8(const wchar_t *str);
char *win32_error_string(DWORD error);

// output.c
void win32_outputs_update(void);

// window.c
bool win32_windows_init(void);
void win32_windows_finish(void);
void win32_windows_drain_events(void);
void win32_windows_manage_existing(void);
void win32_windows_show_all(void);

// keyboard.c
bool win32_keyboard_init(void);
void win32_keyboard_finish(void);

// ipc.c
struct win32_ipc_request {
	uint32_t type;
	const char *payload;
	uint32_t length;
	char *reply;
	uint32_t reply_type;
};
bool win32_ipc_init(void);
void win32_ipc_handle(struct win32_ipc_request *request);

// recover.c
void win32_recover_schedule_save(void);
void win32_recover_save(void);
void win32_recover_clear(void);

#endif
