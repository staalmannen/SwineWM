#ifndef _SWINE_PLATFORM_H
#define _SWINE_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>
#include "list.h"
#include "swinewm/tree/node.h"

struct swine_output;
struct swine_view;
struct view_decoration;

enum platform_ipc_input_capability {
	PLATFORM_IPC_INPUT_KEYBOARD = 1u << 0,
	PLATFORM_IPC_INPUT_POINTER = 1u << 1,
};

// Boundary between the portable window-manager core and the OS. The Win32
// implementation lives in swinewm/win32/; tests link a fake implementation.
// The core never includes <windows.h>.

// Called before anything else is set up. On Windows it sets %SWINEWM_DATA%
// to the directory of swinewm.exe and decides whether swinewm is the shell.
void platform_setup_environment(void);
// -1: detect (no other shell running), 0: never act as the shell, 1: always.
void platform_set_shell_mode(int mode);
// True when swinewm replaces Explorer as the Windows shell.
bool platform_is_shell(void);
// Makes swinewm (or Explorer again) the login shell of the current user.
// Returns a process exit status.
int platform_set_login_shell(bool enable);
// Starts swinewm at logon as a startup program, with Explorer as the shell
// (or stops doing so). Returns the process exit status.
int platform_set_autostart(bool enable);
bool platform_init(void);
// Runs the event loop until platform_quit() is called. Returns exit status.
int platform_run(void);
void platform_quit(void);
// Like platform_quit, but once everything is cleaned up the same
// executable is started again with the same arguments (i3's "restart").
// As the shell, this does not log off.
void platform_restart(void);
// Restores every window the WM changed (unhides hidden workspaces) so that
// exiting or crashing never strands applications.
void platform_finish(void);
// Sends a JSON event body to subscribed IPC clients. The transport copies
// the payload before returning.
void platform_ipc_event(uint32_t type, const char *payload);
uint32_t platform_ipc_input_capabilities(void);

// A keyboard layout installed in Windows' language settings, described as
// sway describes the layouts of its xkb keymap.
struct platform_keyboard_layout {
	char *name;          // xkb's long name: "English (United States)"
	char *short_name;    // xkb's layout: "us", the region in lower case
	char *description;   // Waybar's {shortDescription}: "en", the language
	char *variant;       // "Dvorak" for a layout other than the language's
	                     // default, else ""
};
// Fills layouts (struct platform_keyboard_layout *, freed with
// keyboard_layouts_free) in Windows' order. Returns the index of the active
// layout, or -1 if it is not known.
int platform_keyboard_layouts(list_t *layouts);
// Switches the active layout (an index into platform_keyboard_layouts).
void platform_keyboard_set_layout(int index);

void platform_view_set_geometry(struct swine_view *view,
		const struct swine_box *box);
void platform_view_set_visible(struct swine_view *view, bool visible);
// Draws the tabs of tabbed and stacked containers (struct swine_titlebar *,
// see tree/titlebar.h); the list is only valid during the call.
void platform_titlebars_update(list_t *titlebars);
// Height of a line of text in a sway "font" description, at scale 1.
int platform_font_height(const char *font);
// Border colour, title bar, corners and opacity (struct view_decoration).
// Changing a window's styles may block on its application, so this may
// happen later; it is undone on exit, after a crash and by --recover.
void platform_view_set_decoration(struct swine_view *view,
		const struct view_decoration *deco);
// Requests foreground activation. Windows may refuse; the result says so.
bool platform_view_activate(struct swine_view *view);
// True when the view owns a visible, managed window that should stay above it.
bool platform_view_has_visible_owned_window(struct swine_view *view);
void platform_view_close(struct swine_view *view);

// Runs a command line on the main loop soon (a mouse binding: the input
// hook that found it must return quickly).
void platform_queue_command(const char *command);
// The mouse pointer (virtual-desktop pixels).
bool platform_cursor_position(int *x, int *y);
void platform_cursor_warp(int x, int y);
// Shows where a window dragged with the mouse will land (box), or hides
// the indicator (NULL).
void platform_drop_indicator(const struct swine_box *box);

// Starts a process from a command line. Returns false on failure.
bool platform_spawn(const char *cmdline);
// Starts a command line in the background for a bar script (on Windows
// through cmd.exe, without a window), with %WAYBAR_OUTPUT_NAME% set to
// output_name. Each line it prints reaches bar_script_line and its end
// bar_script_exited (bar/custom.h), on the main thread. Returns an id, or 0
// if it could not be started.
uint64_t platform_script_start(const char *cmdline, const char *output_name);
// Ends the script and every process it started; it reports nothing more.
void platform_script_stop(uint64_t id);

// Candidate paths for a config file in priority order (char *, caller
// frees). name is relative, with '/' separators: "config",
// "waybar/config.jsonc", "mako/config", ...
list_t *platform_config_paths(const char *name);
// Names of the entries of a directory (char *, without "." and ".."), or
// NULL if it cannot be read. Caller frees.
list_t *platform_list_dir(const char *dir);
// Expands ~ and %VAR% in a path. Caller frees.
char *platform_expand_path(const char *path);
// The user's Pictures folder, or NULL. Caller frees.
char *platform_pictures_dir(void);
// Saves the screen area box (desktop coordinates) as a PNG or JPEG file
// (path NULL: none), and with clipboard also puts it on the clipboard.
// On failure *error may receive a message (caller frees).
bool platform_screenshot(const struct swine_box *box, const char *path,
		bool jpeg, bool clipboard, char **error);
// Puts text on the clipboard ("" empties it).
bool platform_clipboard_set_text(const char *text);
// Shows slurp's selection over every output; grim_slurp_done (grim.h)
// is called when the user made it or cancelled.
struct slurp_options;
bool platform_slurp(const struct slurp_options *options);
// Display settings for one output, merged from its "output" configs.
// Unset fields keep the current setting.
struct output_display_config {
	int width, height;     // unrotated mode; 0 = unset
	float refresh_rate;    // Hz; 0 = the highest for the mode
	int x, y;              // INT_MIN = unset
	int transform;         // quarter turns clockwise; -1 = unset
	bool any;              // false: nothing set, restore the defaults
};
// Applies display settings (on Windows temporarily, undone on exit and by
// --recover). Returns false if they could not be applied.
bool platform_output_configure(struct swine_output *output,
		const struct output_display_config *cfg);
// windows_theme: 0 light, 1 dark for Windows' app and system mode, -1 the
// user's own setting again. Undone on exit, after a crash and by --recover.
void platform_set_windows_theme(int mode);
// Called after the config changed so the platform can re-register hooks.
void platform_config_changed(void);
// The bar or notifications need repainting (called after every commit).
void platform_ui_refresh(void);
// Shows persistent config errors in the native nag UI; NULL hides it.
void platform_swaynag_show(const char *details);
// A notification closed (reason as enum notify_close_reason): one mirrored
// from a Windows toast and dismissed by the user goes from Windows' list too.
void platform_notification_closed(uint32_t id, int reason);
// Milliseconds on a monotonic clock, for notification timeouts.
long long platform_now_ms(void);
// Shows windows left hidden by a previous instance that did not exit
// cleanly (swinewm --recover). Returns the number of windows restored.
int platform_recover_windows(void);
// Runs the standalone crash watchdog for the manager process.
int platform_watch(unsigned long process_id, bool shell_mode);

#endif
