#ifndef _SWINE_PLATFORM_H
#define _SWINE_PLATFORM_H

#include <stdbool.h>
#include "list.h"
#include "swinewm/tree/node.h"

struct swine_view;

// Boundary between the portable window-manager core and the OS. The Win32
// implementation lives in swinewm/win32/; tests link a fake implementation.
// The core never includes <windows.h>.

bool platform_init(void);
// Runs the event loop until platform_quit() is called. Returns exit status.
int platform_run(void);
void platform_quit(void);
// Restores every window the WM changed (unhides hidden workspaces) so that
// exiting or crashing never strands applications.
void platform_finish(void);

void platform_view_set_geometry(struct swine_view *view,
		const struct swine_box *box);
void platform_view_set_visible(struct swine_view *view, bool visible);
// Requests foreground activation. Windows may refuse; the result says so.
bool platform_view_activate(struct swine_view *view);
void platform_view_close(struct swine_view *view);

// Starts a process from a command line. Returns false on failure.
bool platform_spawn(const char *cmdline);

// Candidate paths for a config file in priority order (char *, caller
// frees). name is relative, with '/' separators: "config",
// "waybar/config.jsonc", "mako/config", ...
list_t *platform_config_paths(const char *name);
// Expands ~ and %VAR% in a path. Caller frees.
char *platform_expand_path(const char *path);
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
