#ifndef _SWINE_NOTIFY_H
#define _SWINE_NOTIFY_H

#include <stdbool.h>
#include <stdint.h>
#include "list.h"
#include "swinewm/bar/css.h"

// The integrated Mako clone. Notifications arrive through the notify-send
// command (also reachable as "exec notify-send ..." and over IPC), are
// styled from a Mako config (mako/config) and shown by the platform layer.

enum notify_urgency {
	NOTIFY_URGENCY_LOW,
	NOTIFY_URGENCY_NORMAL,
	NOTIFY_URGENCY_CRITICAL,
};

enum notify_anchor {
	NOTIFY_ANCHOR_TOP_RIGHT,
	NOTIFY_ANCHOR_TOP_CENTER,
	NOTIFY_ANCHOR_TOP_LEFT,
	NOTIFY_ANCHOR_BOTTOM_RIGHT,
	NOTIFY_ANCHOR_BOTTOM_CENTER,
	NOTIFY_ANCHOR_BOTTOM_LEFT,
	NOTIFY_ANCHOR_CENTER_RIGHT,
	NOTIFY_ANCHOR_CENTER_LEFT,
	NOTIFY_ANCHOR_CENTER,
};

enum notify_action {
	NOTIFY_ACTION_NONE,
	NOTIFY_ACTION_DISMISS,
	NOTIFY_ACTION_DISMISS_ALL,
	NOTIFY_ACTION_INVOKE_DEFAULT,
	NOTIFY_ACTION_EXEC,
};

struct notify_button {
	enum notify_action action;
	char command[256];        // for NOTIFY_ACTION_EXEC
};

struct notify_style {
	char font_family[128];
	double font_size;         // points, as in Pango
	bool font_bold;
	struct swine_color background;
	struct swine_color text;
	struct swine_color border;
	int width, height;
	struct css_box outer_margin, margin, padding;
	int border_size;
	int border_radius;
	char format[256];
	int text_alignment;       // 0 left, 1 center, 2 right
	int default_timeout;      // ms, 0 = never
	bool ignore_timeout;
	bool invisible;
	bool markup;
	bool history;
	enum notify_anchor anchor;
	char output[64];          // "" = focused output
	struct notify_button on_button_left, on_button_middle, on_button_right;
};

struct swine_notification {
	uint32_t id;
	char *app_name;
	char *summary;
	char *body;
	char *category;
	enum notify_urgency urgency;
	int expire_timeout;       // as requested: -1 = default, 0 = never
	long long created_ms;
	long long expire_ms;      // 0 = does not expire
	struct notify_style style;
};

struct notify_params {
	const char *app_name;
	const char *summary;
	const char *body;
	const char *category;
	enum notify_urgency urgency;
	int expire_timeout;
	uint32_t replaces_id;
};

struct notify_line {
	char *text;
	bool bold;
	bool italic;
};

// Reads mako/config (or uses defaults). Restyles current notifications.
void notify_load_config(void);
void notify_finish(void);
bool notify_parse_config(const char *text, const char *name);

uint32_t notify_add(const struct notify_params *params, long long now_ms);
bool notify_dismiss(uint32_t id, bool to_history);   // id 0 = newest
void notify_dismiss_all(bool to_history);
bool notify_restore(long long now_ms);
// Expires notifications. Returns true if anything changed.
bool notify_tick(long long now_ms);
// Earliest pending expiry, or 0.
long long notify_next_expiry(void);

// Notifications to display, in display order (struct swine_notification *).
// The list is owned by the caller; the items are not.
list_t *notify_visible(void);
int notify_hidden_count(void);
list_t *notify_active(void);    // owned by the module, newest first
list_t *notify_history(void);   // owned by the module, newest first
struct swine_notification *notify_by_id(uint32_t id);
// Runs a mouse button action on a notification (1 left, 2 middle, 3 right).
void notify_button(uint32_t id, int button);

// Applies the format option; returns struct notify_line * items.
list_t *notify_format_lines(const struct swine_notification *n);
void notify_free_lines(list_t *lines);

// Mako modes, used by [mode=...] criteria (do-not-disturb, ...).
list_t *notify_modes(void);    // char *
void notify_set_modes(char **modes, int count);
void notify_add_mode(const char *mode);
void notify_remove_mode(const char *mode);
void notify_toggle_mode(const char *mode);

char *notify_list_json(list_t *notifications);

#endif
