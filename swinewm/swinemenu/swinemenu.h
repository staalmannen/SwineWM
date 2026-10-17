#ifndef _SWINEMENU_H
#define _SWINEMENU_H

#include "list.h"

// Entry sources (apps.c). Each returns a list of struct menu_entry *.

// "drun": the Start menu's shortcuts (all users and the current user) and
// Store apps, as "shell:AppsFolder\<AppUserModelID>" commands.
list_t *swinemenu_drun_entries(void);
// "run": programs on PATH (.exe, .bat, .cmd, .com), by name.
list_t *swinemenu_run_entries(void);
// "dmenu": one entry per line of standard input (UTF-8).
list_t *swinemenu_dmenu_entries(void);

char *swinemenu_utf16_to_utf8(const wchar_t *w);
wchar_t *swinemenu_utf8_to_utf16(const char *s);

#endif
