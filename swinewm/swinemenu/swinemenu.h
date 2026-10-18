#ifndef _SWINEMENU_H
#define _SWINEMENU_H

#include <stdbool.h>
#include <wchar.h>
#include "list.h"

// Entry sources (apps.c). Each returns a list of struct menu_entry *.

// "drun": the Start menu's shortcuts (all users and the current user) and
// Store apps, as "shell:AppsFolder\<AppUserModelID>" commands.
list_t *swinemenu_drun_entries(void);
// "run": programs on PATH (.exe, .bat, .cmd, .com), by name.
list_t *swinemenu_run_entries(void);
// "dmenu": one entry per line of standard input (UTF-8).
list_t *swinemenu_dmenu_entries(void);

// "open with": the programs registered for file's type (openwith.c). An
// entry's command is "openwith:<n>", for swinemenu_openwith_invoke, or
// "line:<command line>" (from the registry), for the fallback.
list_t *swinemenu_openwith_entries(const wchar_t *file);
bool swinemenu_openwith_invoke(const char *command, const wchar_t *file);
// The handler's program and file as a command line, or NULL. Caller frees.
char *swinemenu_openwith_fallback(const char *command, const wchar_t *file);
// --install-open-with / --uninstall-open-with; returns the exit status.
int swinemenu_install_open_with(bool install);

char *swinemenu_utf16_to_utf8(const wchar_t *w);
wchar_t *swinemenu_utf8_to_utf16(const char *s);

#endif
