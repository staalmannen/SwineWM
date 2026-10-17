#ifndef _SWINE_PLATFORM_H
#define _SWINE_PLATFORM_H

#include <stdbool.h>
#include "list.h"
#include "swinewm/tree/node.h"

struct swine_output;
struct swine_view;

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

void platform_view_set_geometry(struct swine_view *view,
		const struct swine_box *box);
void platform_view_set_visible(struct swine_view *view, bool visible);
// Requests foreground activation. Windows may refuse; the result says so.
bool platform_view_activate(struct swine_view *view);
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
// Saves the screen area box (desktop coordinates) as a PNG or JPEG file.
// On failure *error may receive a message (caller frees).
bool platform_screenshot(const struct swine_box *box, const char *path,
		bool jpeg, char **error);
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
// Called after the config changed so the platform can re-register hooks.
void platform_config_changed(void);
// The bar or notifications need repainting (called after every commit).
void platform_ui_refresh(void);
// Milliseconds on a monotonic clock, for notification timeouts.
long long platform_now_ms(void);
// Shows windows left hidden by a previous instance that did not exit
// cleanly (swinewm --recover). Returns the number of windows restored.
int platform_recover_windows(void);

#endif
