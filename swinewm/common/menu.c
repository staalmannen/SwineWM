#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fileutil.h"
#include "menu.h"
#include "stringop.h"

struct menu_entry *menu_entry_create(const char *name, const char *command) {
	struct menu_entry *entry = calloc(1, sizeof(*entry));
	entry->name = swine_strdup(name);
	entry->command = command ? swine_strdup(command) : NULL;
	return entry;
}

void menu_entry_free(struct menu_entry *entry) {
	if (entry) {
		free(entry->name);
		free(entry->command);
		free(entry);
	}
}

// Position of word in name ignoring ASCII case, or -1.
static int find_word(const char *name, const char *word, size_t len) {
	for (size_t i = 0; name[i]; ++i) {
		size_t k = 0;
		while (k < len && name[i + k] &&
				tolower((unsigned char)name[i + k]) ==
				tolower((unsigned char)word[k])) {
			++k;
		}
		if (k == len) {
			return (int)i;
		}
	}
	return -1;
}

int menu_match_score(const char *name, const char *query) {
	int score = 0;
	const char *p = query;
	while (*p) {
		while (*p == ' ') {
			++p;
		}
		size_t len = strcspn(p, " ");
		if (len == 0) {
			break;
		}
		int at = find_word(name, p, len);
		if (at < 0) {
			return -1;
		}
		if (at == 0) {
			score += 3;   // the name starts with it
		} else if (!isalnum((unsigned char)name[at - 1])) {
			score += 2;   // a word in the name starts with it
		} else {
			score += 1;
		}
		p += len;
	}
	return score;
}

struct ranked {
	struct menu_entry *entry;
	size_t index;
	int score;
};

static int compare_ranked(const void *a, const void *b) {
	const struct ranked *x = a, *y = b;
	if (x->entry->uses != y->entry->uses) {
		return x->entry->uses > y->entry->uses ? -1 : 1;
	}
	if (x->score != y->score) {
		return x->score > y->score ? -1 : 1;
	}
	// For a typed query shorter names are closer matches; an empty query
	// lists everything alphabetically.
	size_t lx = strlen(x->entry->name), ly = strlen(y->entry->name);
	if (x->score > 0 && lx != ly) {
		return lx < ly ? -1 : 1;
	}
	int c = swine_strcasecmp(x->entry->name, y->entry->name);
	if (c != 0) {
		return c;
	}
	return x->index < y->index ? -1 : 1;
}

list_t *menu_filter(list_t *entries, const char *query) {
	struct ranked *ranked = calloc((size_t)entries->length + 1, sizeof(*ranked));
	size_t n = 0;
	for (int i = 0; i < entries->length; ++i) {
		struct menu_entry *entry = entries->items[i];
		int score = menu_match_score(entry->name, query ? query : "");
		if (score >= 0) {
			ranked[n++] = (struct ranked){entry, (size_t)i, score};
		}
	}
	qsort(ranked, n, sizeof(*ranked), compare_ranked);
	list_t *result = create_list();
	for (size_t i = 0; i < n; ++i) {
		list_add(result, (void *)(uintptr_t)ranked[i].index);
	}
	free(ranked);
	return result;
}

void menu_history_load(list_t *entries, const char *path) {
	FILE *f = path ? swine_fopen(path, "r") : NULL;
	if (!f) {
		return;
	}
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		char *tab = strchr(line, '\t');
		if (!tab) {
			continue;
		}
		*tab = '\0';
		int uses = atoi(line);
		for (int i = 0; i < entries->length; ++i) {
			struct menu_entry *entry = entries->items[i];
			if (strcmp(entry->name, tab + 1) == 0) {
				entry->uses = uses;
			}
		}
	}
	fclose(f);
}

void menu_history_record(const char *path, const char *name) {
	if (!path) {
		return;
	}
	list_t *names = create_list();
	list_t *counts = create_list();
	bool found = false;
	FILE *f = swine_fopen(path, "r");
	if (f) {
		char line[1024];
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = '\0';
			char *tab = strchr(line, '\t');
			if (!tab) {
				continue;
			}
			*tab = '\0';
			int uses = atoi(line);
			if (strcmp(tab + 1, name) == 0) {
				++uses;
				found = true;
			}
			list_add(names, swine_strdup(tab + 1));
			list_add(counts, (void *)(intptr_t)uses);
		}
		fclose(f);
	}
	if (!found) {
		list_add(names, swine_strdup(name));
		list_add(counts, (void *)(intptr_t)1);
	}
	f = swine_fopen(path, "w");
	if (f) {
		for (int i = 0; i < names->length; ++i) {
			fprintf(f, "%d\t%s\n", (int)(intptr_t)counts->items[i],
				(char *)names->items[i]);
		}
		fclose(f);
	}
	list_free_items_and_destroy(names);
	list_free(counts);
}
