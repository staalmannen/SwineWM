// Title bars of tabbed and stacked containers (see tree/titlebar.h). The
// representation of split containers is ported from sway's
// container_build_representation (Copyright (c) 2016-2017 Drew DeVault, MIT).
#include <stdlib.h>
#include <string.h>
#include "stringop.h"
#include "swinewm/config.h"
#include "swinewm/platform.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/titlebar.h"
#include "swinewm/tree/view.h"

static list_t *titlebars = NULL;

int container_titlebar_height(void) {
	if (config->font_height <= 0) {
		config->font_height = platform_font_height(config->font);
	}
	return config->font_height + config->titlebar_v_padding * 2;
}

static void append(char **buf, size_t *len, size_t *cap, const char *text) {
	size_t n = strlen(text);
	if (*len + n + 1 > *cap) {
		*cap = (*len + n + 1) * 2;
		*buf = realloc(*buf, *cap);
	}
	memcpy(*buf + *len, text, n + 1);
	*len += n;
}

// sway names a window by its class, else its app_id. On Windows the
// program ("firefox.exe") says more than window classes such as
// "Chrome_WidgetWin_1", so app_id comes first.
static const char *view_identifier(struct swine_view *view) {
	return view->app_id ? view->app_id :
		view->class_name ? view->class_name : NULL;
}

char *container_representation(struct swine_container *con) {
	if (con->view) {
		return swine_strdup(con->view->title ? con->view->title : "");
	}
	size_t len = 0, cap = 64;
	char *buf = malloc(cap);
	buf[0] = '\0';
	switch (con->layout) {
	case L_VERT:
		append(&buf, &len, &cap, "V[");
		break;
	case L_HORIZ:
		append(&buf, &len, &cap, "H[");
		break;
	case L_TABBED:
		append(&buf, &len, &cap, "T[");
		break;
	case L_STACKED:
		append(&buf, &len, &cap, "S[");
		break;
	case L_NONE:
		append(&buf, &len, &cap, "D[");
		break;
	}
	for (int i = 0; i < con->children->length; ++i) {
		if (i != 0) {
			append(&buf, &len, &cap, " ");
		}
		struct swine_container *child = con->children->items[i];
		if (child->view) {
			const char *identifier = view_identifier(child->view);
			append(&buf, &len, &cap, identifier ? identifier : "(null)");
		} else {
			char *nested = container_representation(child);
			append(&buf, &len, &cap, nested);
			free(nested);
		}
	}
	append(&buf, &len, &cap, "]");
	return buf;
}

char *container_formatted_title(struct swine_container *con) {
	const char *format = con->title_format;
	if (!format || strcmp(format, "%title") == 0) {
		return container_representation(con);
	}
	size_t len = 0, cap = 64;
	char *buf = malloc(cap);
	buf[0] = '\0';
	struct swine_view *view = con->view;
	const char *p = format;
	while (*p) {
		const char *next = strchr(p, '%');
		if (!next) {
			append(&buf, &len, &cap, p);
			break;
		}
		char *literal = swine_strndup(p, (size_t)(next - p));
		append(&buf, &len, &cap, literal);
		free(literal);
		p = next;
		const char *value = NULL;
		size_t skip = 0;
		char *owned = NULL;
		if (has_prefix(p, "%title")) {
			owned = container_representation(con);
			value = owned;
			skip = strlen("%title");
		} else if (view && has_prefix(p, "%app_id")) {
			value = view->app_id;
			skip = strlen("%app_id");
		} else if (view && has_prefix(p, "%class")) {
			value = view->class_name;
			skip = strlen("%class");
		} else if (view && has_prefix(p, "%instance")) {
			value = view->class_name;
			skip = strlen("%instance");
		} else if (view && has_prefix(p, "%shell")) {
			value = "win32";
			skip = strlen("%shell");
		}
		if (skip) {
			append(&buf, &len, &cap, value ? value : "");
			p += skip;
		} else {
			append(&buf, &len, &cap, "%");
			++p;
		}
		free(owned);
	}
	return buf;
}

// sway's container_update_marks: "[mark]" for each mark not starting with
// an underscore. NULL if there is nothing to show.
static char *container_marks_text(struct swine_container *con) {
	if (!config->show_marks || !con->marks->length) {
		return NULL;
	}
	size_t len = 0, cap = 32;
	char *buf = malloc(cap);
	buf[0] = '\0';
	for (int i = 0; i < con->marks->length; ++i) {
		const char *mark = con->marks->items[i];
		if (mark[0] == '_') {
			continue;
		}
		append(&buf, &len, &cap, "[");
		append(&buf, &len, &cap, mark);
		append(&buf, &len, &cap, "]");
	}
	if (len == 0) {
		free(buf);
		return NULL;
	}
	return buf;
}

static void free_titlebar(struct swine_titlebar *bar) {
	for (int i = 0; i < bar->tabs->length; ++i) {
		struct swine_tab *tab = bar->tabs->items[i];
		free(tab->title);
		free(tab->marks);
		free(tab);
	}
	list_free(bar->tabs);
	free(bar);
}

list_t *titlebars_get(void) {
	if (!titlebars) {
		titlebars = create_list();
	}
	return titlebars;
}

void titlebars_clear(void) {
	if (!titlebars) {
		return;
	}
	for (int i = 0; i < titlebars->length; ++i) {
		free_titlebar(titlebars->items[i]);
	}
	titlebars->length = 0;
}

void titlebars_add(struct swine_node *parent, list_t *children,
		const struct swine_box *box, bool stacked, int tab_height,
		double scale) {
	(void)parent;
	struct swine_titlebar *bar = calloc(1, sizeof(*bar));
	bar->box = *box;
	bar->stacked = stacked;
	bar->tab_height = tab_height;
	bar->scale = scale;
	bar->tabs = create_list();
	for (int i = 0; i < children->length; ++i) {
		struct swine_container *child = children->items[i];
		struct swine_tab *tab = calloc(1, sizeof(*tab));
		tab->con = child;
		tab->title = container_formatted_title(child);
		tab->marks = container_marks_text(child);
		tab->colors = *container_get_colors(child);
		list_add(bar->tabs, tab);
	}
	list_add(titlebars_get(), bar);
}
