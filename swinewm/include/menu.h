#ifndef _SWINE_MENU_H
#define _SWINE_MENU_H

#include <stdbool.h>
#include "list.h"

// Matching and ranking for swinemenu, like rofi/wofi's default mode: the
// query is split at spaces and every word must appear in the entry's name,
// ignoring (ASCII) case.

struct menu_entry {
	char *name;       // what is shown and matched
	char *command;    // what is run (swinewm "exec" argument), or NULL
	int uses;         // how often it was chosen (history)
};

struct menu_entry *menu_entry_create(const char *name, const char *command);
void menu_entry_free(struct menu_entry *entry);

// Score of name for query: < 0 means no match; higher ranks first. An
// empty query matches everything with score 0.
int menu_match_score(const char *name, const char *query);

// Indexes (into entries, as (size_t) pointers cast to void *) of the entries
// that match query, best first: more uses, then higher score, then (for a
// non-empty query) shorter names, then alphabetical. Caller frees the list.
list_t *menu_filter(list_t *entries, const char *query);

// History file: lines "<uses>\t<name>". Missing files are no error.
void menu_history_load(list_t *entries, const char *path);
void menu_history_record(const char *path, const char *name);

#endif
