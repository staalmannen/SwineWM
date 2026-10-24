#ifndef _SWINE_WIN32_H
#define _SWINE_WIN32_H

// Internal interface between the files of the Win32 platform layer. Nothing
// outside swinewm/win32/ includes this or <windows.h>.

#include <windows.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include "swinewm/bar/css.h"

struct swine_view;
struct swine_container;
struct swine_output;

// Messages posted to the shell's own (hidden) window. Everything that
// touches the tree runs on the main thread; hooks and the IPC threads only
// post or send these.
#define WM_SWINE_BINDING (WM_APP + 1)  // lParam: malloc'ed UTF-8 command
#define WM_SWINE_IPC (WM_APP + 2)      // lParam: struct win32_ipc_request *
#define WM_SWINE_EVENTS (WM_APP + 3)   // drain queued WinEvents
#define WM_SWINE_SAVE_HIDDEN (WM_APP + 4)
#define WM_SWINE_TRAY (WM_APP + 5)     // tray.c: icons changed, or a balloon
#define WM_SWINE_DISPLAY_REFRESH (WM_APP + 7) // refresh outputs after a DPI change
#define WM_SWINE_SCRIPT (WM_APP + 8)   // script.c: a line or the end of a bar script

extern HWND win32_shell_hwnd;

// platform.c
// Runs a swinewm command line (bindings, bar clicks) and commits.
void win32_run_command(const char *command);

// layout.c: keyboard layouts. The shell window's WIN32_TIMER_LAYOUT fires
// shortly after a switch to read the new layout.
#define WIN32_TIMER_LAYOUT 4
void win32_layouts_finish(void);

// script.c: bar scripts (platform_script_start).
void win32_script_message(LPARAM lparam);
void win32_scripts_finish(void);

// draw.c: GDI helpers shared by the bar and notification popups
// Picks the first installed family from a CSS/Pango family list.
HFONT win32_font(const char *families, int px, bool bold, bool italic);
void win32_fonts_clear(void);
// A sway "font" description ("pango:DejaVu Sans Mono 10") at a scale.
HFONT win32_sway_font(const char *desc, double scale);
COLORREF win32_blend(struct swine_color color, COLORREF under);
void win32_fill(HDC dc, const RECT *rect, COLORREF color);
double win32_output_scale(struct swine_output *output);

// titlebar.c: tabs of tabbed and stacked containers
// True if hwnd is a tab bar; *con is the child under the point.
bool win32_titlebar_hit(HWND hwnd, POINT pt, struct swine_container **con);
void win32_titlebars_finish(void);

// bar.c
void win32_bar_refresh(void);
void win32_bar_finish(void);

// shell.c: acting as the Windows shell, Explorer's taskbar
extern bool win32_shell_mode;
void win32_shell_detect(void);
void win32_shell_start(void);
void win32_shell_resize_desktop(void);
// swinewm's desktop window as the shell, or NULL.
HWND win32_shell_desktop_window(void);
void win32_shell_update_work_area(bool reset);
void win32_shell_apply_taskbar(void);
void win32_shell_restore_taskbar(void);
bool win32_shell_recover_taskbar(void);
void win32_shell_finish(void);
void win32_shell_exit(void);
void win32_shell_rescue(void);

// watch.c
void win32_watch_start(void);
void win32_watch_signal_exit(void);
void win32_watch_finish(void);
bool win32_watch_responsible(void);

// swaynag.c
void win32_swaynag_show(const char *details);
void win32_swaynag_finish(void);

// Explorer's own taskbar window, skipping swinewm's Shell_TrayWnd.
HWND win32_find_explorer_tray(void);

// tray.c: hosting the notification area for the bar's "tray" module
void win32_tray_update(void);
void win32_tray_finish(void);
void win32_tray_crash(void);
void win32_tray_recover(void);
void win32_tray_handle_message(WPARAM wparam, LPARAM lparam);
void win32_tray_draw(HDC dc, uint64_t id, const char *icon_path,
		const RECT *rect, HWND bar);
void win32_tray_set_host_rect(const RECT *screen);
// button: 0 left, 1 middle, 2 right
void win32_tray_mouse(uint64_t id, int button, bool down, POINT screen);

// window.c
// The executable's file name ("Discord.exe"), or NULL.
char *win32_process_name(DWORD pid);

// wallpaper.c
void win32_wallpaper_refresh(void);
// As the shell: draws the wallpapers into the desktop window, whose client
// area starts at (origin_x, origin_y) on the virtual screen.
void win32_wallpaper_paint(HDC dc, int origin_x, int origin_y);
void win32_wallpaper_finish(void);

// notify.c
void win32_notify_refresh(void);
void win32_notify_finish(void);

// util.c
wchar_t *utf8_to_utf16(const char *str);
char *utf16_to_utf8(const wchar_t *str);
char *win32_error_string(DWORD error);

// output.c
void win32_outputs_update(void);
// Returns every monitor to its saved settings if swinewm changed any
// (output mode/position/transform).
void win32_outputs_restore_settings(void);
void win32_outputs_finish(void);

// window.c
bool win32_windows_init(void);
void win32_windows_finish(void);
void win32_windows_drain_events(void);
void win32_windows_manage_existing(void);
void win32_windows_show_all(void);
// A message from RegisterShellHookWindow (HSHELL_* code, window).
void win32_windows_shell_hook(WPARAM code, LPARAM lparam);

// decor.c: borders, title bars, corners and opacity (worker thread)
#define DECOR_DWM 1u        // DWM attributes were set
#define DECOR_CAPTION 2u    // WS_DLGFRAME was taken away
#define DECOR_LAYERED 4u    // WS_EX_LAYERED was added
// The window is no longer managed: undo its decorations.
void win32_decor_forget(HWND hwnd, DWORD pid);
// Undoes decorations recorded in the recovery file (by a previous run).
void win32_decor_restore(HWND hwnd, DWORD pid, unsigned flags);
// Undoes every decoration and waits for the worker (exit, crash).
void win32_decor_restore_all(DWORD timeout_ms);
void win32_decor_wait(DWORD timeout_ms);
// Writes one recovery line per decorated window.
void win32_decor_save(FILE *f);

// keyboard.c
bool win32_keyboard_init(void);
void win32_keyboard_finish(void);
uint32_t win32_keyboard_modifiers(void);
// A mouse binding or move used these modifiers: releasing Win or Alt must
// not open the Start menu or the window's menu bar.
void win32_keyboard_used_modifiers(uint32_t mods);

// mouse.c: pointer input through a low-level mouse hook
#define WM_SWINE_MOTION (WM_APP + 6)   // the pointer moved
bool win32_mouse_init(void);
void win32_mouse_finish(void);
void win32_mouse_handle_motion(void);
// Nonzero while the main thread runs swinewm code; the mouse hook then
// leaves input alone instead of re-entering the core.
extern int win32_busy;

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
void win32_ipc_publish(uint32_t type, const char *payload);
// Waits (at most timeout_ms) until queued events are written, so that the
// shutdown event reaches subscribers before swinewm exits.
void win32_ipc_flush(DWORD timeout_ms);

// theme.c: windows_theme (Windows' light/dark mode)
void win32_theme_init(void);
void win32_theme_restore(void);
void win32_theme_recover(void);

// recover.c
// %LOCALAPPDATA%\swinewm\<name>. Caller frees.
char *win32_state_path(const char *name, bool create_dir);
void win32_recover_schedule_save(void);
void win32_recover_save(void);
void win32_recover_clear(void);
// Remembers that display settings were changed, so that --recover (or the
// next start) restores them after a crash.
void win32_recover_note_display_changed(void);
void win32_recover_display_done(void);

#endif
