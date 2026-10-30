#ifndef _SWINE_NOTIFY_H
#define _SWINE_NOTIFY_H

#include <stdbool.h>
#include <stdint.h>
#include "list.h"
#include "swinewm/bar/css.h"
#include "swinewm/bar/format.h"

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

enum notify_icon_location {
	NOTIFY_ICON_LEFT,
	NOTIFY_ICON_RIGHT,
	NOTIFY_ICON_TOP,
	NOTIFY_ICON_BOTTOM,
};

// mako's bindings (on-button-left=..., on-notify=...)
enum notify_action {
	NOTIFY_ACTION_NONE,
	NOTIFY_ACTION_DISMISS,
	NOTIFY_ACTION_DISMISS_NO_HISTORY,
	NOTIFY_ACTION_DISMISS_GROUP,
	NOTIFY_ACTION_DISMISS_ALL,
	NOTIFY_ACTION_INVOKE,     // invoke-default-action, invoke-action <name>
	NOTIFY_ACTION_EXEC,
};

struct notify_button {
	enum notify_action action;
	char command[256];        // the command for EXEC, the action for INVOKE
};

// Fields of mako criteria, as used by group-by.
enum notify_field {
	NOTIFY_FIELD_APP_NAME = 1 << 0,
	NOTIFY_FIELD_APP_ICON = 1 << 1,
	NOTIFY_FIELD_ACTIONABLE = 1 << 2,
	NOTIFY_FIELD_EXPIRING = 1 << 3,
	NOTIFY_FIELD_URGENCY = 1 << 4,
	NOTIFY_FIELD_CATEGORY = 1 << 5,
	NOTIFY_FIELD_DESKTOP_ENTRY = 1 << 6,
	NOTIFY_FIELD_SUMMARY = 1 << 7,
	NOTIFY_FIELD_BODY = 1 << 8,
	NOTIFY_FIELD_NONE = 1 << 9,
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
	struct css_box border_radius;   // corners: top-left, top-right,
	                                // bottom-right, bottom-left
	struct swine_color progress;
	bool progress_source;     // progress-color=source ...: replaces the
	                          // background instead of blending over it
	bool icons;
	int max_icon_size;
	char icon_path[512];      // directories, separated by ';' (or ':')
	enum notify_icon_location icon_location;
	int icon_border_radius;
	bool actions;
	uint32_t group_by;        // enum notify_field bits
	int max_visible;
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
	struct notify_button on_touch, on_notify;
};

struct notify_action_item {
	char *key;
	char *title;
};

struct swine_notification {
	uint32_t id;
	char *app_name;
	char *summary;
	char *body;
	char *category;
	char *app_icon;           // a file, file:// URI or icon name ("" = none)
	char *desktop_entry;
	char *tag;                // x-canonical-private-synchronous / x-dunst-stack-tag
	list_t *actions;          // struct notify_action_item *
	int progress;             // 0-100, -1 = none (the "value" hint)
	enum notify_urgency urgency;
	int expire_timeout;       // as requested: -1 = default, 0 = never
	long long created_ms;
	long long expire_ms;      // 0 = does not expire
	int group_index;          // -1 = not grouped
	int group_count;
	bool hidden;              // the "(%h more)" placeholder
	int hidden_count;         // for the placeholder: %h and %t
	int total_count;
	bool waited_on;           // a notify-send --wait/-A client handles actions
	struct notify_style style;
};

struct notify_params {
	const char *app_name;
	const char *summary;
	const char *body;
	const char *category;
	const char *app_icon;
	const char *desktop_entry;
	const char *tag;
	const char **action_keys;   // action_count of each
	const char **action_titles;
	int action_count;
	bool has_progress;
	int progress;
	enum notify_urgency urgency;
	int expire_timeout;
	uint32_t replaces_id;
	bool waited_on;
};

// Why a notification closed, as in the freedesktop NotificationClosed signal.
enum notify_close_reason {
	NOTIFY_CLOSE_EXPIRED = 1,
	NOTIFY_CLOSE_DISMISSED = 2,
	NOTIFY_CLOSE_REQUEST = 3,
};

struct notify_line {
	char *text;      // without markup
	list_t *spans;   // struct markup_span *: the text with its attributes
};

// windows-notifications= (swinewm): mirror Windows toasts as notifications.
enum notify_windows {
	NOTIFY_WINDOWS_AUTO,      // when swinewm is the shell
	NOTIFY_WINDOWS_ON,
	NOTIFY_WINDOWS_OFF,
};
enum notify_windows notify_windows_mode(void);

// Reads mako/config (or uses defaults). Restyles current notifications.
void notify_load_config(void);
void notify_finish(void);
bool notify_parse_config(const char *text, const char *name);

uint32_t notify_add(const struct notify_params *params, long long now_ms);
bool notify_dismiss(uint32_t id, bool to_history);   // id 0 = newest
// Dismisses the notification and the rest of its group.
bool notify_dismiss_group(uint32_t id, bool to_history);
void notify_dismiss_all(bool to_history);
// Closes a notification on its sender's request (CloseNotification).
bool notify_close(uint32_t id);
// Invokes an action (id 0 = newest). Sends the "notification" IPC event
// with change "action"; without a waiting client the notification is
// dismissed, as the sending application would do. Returns false if there
// is no such notification or action.
bool notify_invoke(uint32_t id, const char *action);
bool notify_restore(long long now_ms);
// Expires notifications. Returns true if anything changed.
bool notify_tick(long long now_ms);
// Earliest pending expiry, or 0.
long long notify_next_expiry(void);

// Notifications to display, in display order (struct swine_notification *).
// The list is owned by the caller; the items are not, except that the
// "(%h more)" placeholders (hidden set) are freed with notify_free_visible.
list_t *notify_visible(void);
void notify_free_visible(list_t *visible);
list_t *notify_active(void);    // owned by the module, in display order
list_t *notify_history(void);   // owned by the module, newest first
struct swine_notification *notify_by_id(uint32_t id);
// Runs a mouse button action on a notification (1 left, 2 middle, 3 right).
void notify_button(uint32_t id, int button);

// Files that may hold the notification's icon, best first: the file named
// by app_icon (a path or file:// URI), or <dir>\<name>.png/.ico/... for each
// icon-path directory. Caller frees with list_free_items_and_destroy.
list_t *notify_icon_candidates(const struct swine_notification *n);

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
// makoctl list/history text output.
char *notify_list_text(list_t *notifications);

#endif
