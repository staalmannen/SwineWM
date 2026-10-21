#ifndef _SWINE_IPC_JSON_H
#define _SWINE_IPC_JSON_H

#include <stdbool.h>

struct swine_container;
struct swine_binding;
struct swine_workspace;

char *ipc_json_get_version(void);
char *ipc_json_get_workspaces(void);
char *ipc_json_get_outputs(void);
char *ipc_json_get_tree(void);
char *ipc_json_get_binding_modes(void);
char *ipc_json_get_binding_state(void);
char *ipc_json_get_inputs(void);
char *ipc_json_get_seats(void);
char *ipc_json_get_config(void);
char *ipc_json_get_marks(void);
char *ipc_json_event_workspace(const char *change,
		struct swine_workspace *current, struct swine_workspace *old);
char *ipc_json_event_window(const char *change, struct swine_container *con);
// sway's mode event: {"change": "<mode name>", "pango_markup": ...}.
char *ipc_json_event_mode(const char *name, bool pango_markup);
char *ipc_json_event_output(const char *change);
char *ipc_json_event_binding(const struct swine_binding *binding,
		const char *event);
// NULL or "" lists the bar ids; otherwise the bar's config (sway format).
char *ipc_json_get_bar_config(const char *id);

#endif
