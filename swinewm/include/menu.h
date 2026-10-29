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
	char *icon;       // a file or shell item whose icon is shown, or NULL
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

// wofi's options, from its config file (key=value, the long option names
// with '_' or '-') and its command line.
enum menu_mode {
	MENU_MODE_DRUN,
	MENU_MODE_RUN,
	MENU_MODE_DMENU,
	MENU_MODE_WINDOW,      // rofi's "window": switch to a window
	MENU_MODE_OPENWITH,    // swinemenu's own
};

struct menu_options {
	enum menu_mode mode;
	char *prompt;          // NULL: the mode's name
	char *width;           // "600" (px) or "50%"; NULL: swinemenu's 40%
	char *height;          // NULL: from lines
	int lines;             // 0: wofi's height, else this many rows
	int location;          // 0 center .. 8 left, as wofi(7)
	int xoffset, yoffset;
	int allow_images;      // -1: the mode's default (drun, window: yes)
	int image_size;        // px, wofi's 32
	bool hide_search;
	bool password;
	char password_char[8];
	bool exec_search;
	char *search;          // typed in at the start (-Q)
	char *conf;            // -c: the config file
	char *style;           // -s, style=: the style sheet
	char *open_with;       // --open-with <file>
};

void menu_options_init(struct menu_options *o);
void menu_options_finish(struct menu_options *o);
// Sets one option by its long name ("allow-images" or "allow_images");
// value is NULL for flags given without one. Returns false and sets
// *error (caller frees) for unknown keys or bad values.
bool menu_option_set(struct menu_options *o, const char *key,
		const char *value, char **error);
// Applies a wofi config file's text.
bool menu_options_parse_config(struct menu_options *o, const char *text,
		char **error);
// The option a short or long command-line flag names ("-I", "--lines"),
// and whether it takes a value. NULL if unknown.
const char *menu_option_name(const char *flag, bool *takes_value);
// Pixels for a wofi size ("600", "50%") of a screen of `screen` pixels.
int menu_size(const char *size, int screen, int fallback);

#endif
