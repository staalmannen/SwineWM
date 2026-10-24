#include <stdlib.h>
#include <string.h>
#include "ipc.h"
#include "json.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/ipc-json.h"
#include "swinewm/ipc-server.h"
#include "swinewm/notify.h"
#include "swinewm/platform.h"

static char *simple_reply(bool success) {
	return swine_strdup(success ? "{\"success\":true}" : "{\"success\":false}");
}

bool ipc_parse_subscriptions(const char *payload, uint32_t length,
		uint32_t *subscriptions) {
	char *text = swine_strndup(payload, length);
	char *error = NULL;
	struct json_value *value = json_parse(text, &error);
	free(text);
	free(error);
	if (!value || value->type != JSON_ARRAY) {
		json_value_free(value);
		return false;
	}
	static const struct {
		const char *name;
		uint32_t subscription;
	} event_names[] = {
		{"workspace", IPC_SUB_WORKSPACE},
		{"output", IPC_SUB_OUTPUT},
		{"mode", IPC_SUB_MODE},
		{"window", IPC_SUB_WINDOW},
		{"barconfig_update", IPC_SUB_BARCONFIG_UPDATE},
		{"binding", IPC_SUB_BINDING},
		{"shutdown", IPC_SUB_SHUTDOWN},
		{"tick", IPC_SUB_TICK},
		{"input", IPC_SUB_INPUT},
	};
	uint32_t mask = 0;
	bool valid = true;
	for (int i = 0; i < value->items->length && valid; ++i) {
		struct json_value *item = value->items->items[i];
		if (item->type != JSON_STRING) {
			valid = false;
			break;
		}
		bool found = false;
		for (size_t j = 0; j < sizeof(event_names) / sizeof(event_names[0]); ++j) {
			if (strcmp(item->string, event_names[j].name) == 0) {
				mask |= event_names[j].subscription;
				found = true;
				break;
			}
		}
		if (!found) {
			valid = false;
		}
	}
	json_value_free(value);
	if (valid && subscriptions) {
		*subscriptions = mask;
	}
	return valid;
}

char *ipc_json_tick_event(bool first, const char *payload, uint32_t length) {
	char *text = swine_strndup(payload, length);
	struct json_writer w;
	json_init(&w);
	json_begin_object(&w);
	json_key(&w, "first");
	json_bool(&w, first);
	json_key(&w, "payload");
	json_string(&w, text);
	json_end_object(&w);
	free(text);
	return json_finish(&w);
}

char *ipc_handle_request(uint32_t type, const char *payload, uint32_t length,
		uint32_t *reply_type) {
	*reply_type = type;
	switch (type) {
	case IPC_COMMAND: {
		char *line = swine_strndup(payload, length);
		list_t *res_list = execute_command(line, NULL);
		transaction_commit_dirty();
		char *json = cmd_results_to_json(res_list);
		free_cmd_results_list(res_list);
		free(line);
		return json;
	}
	case IPC_GET_WORKSPACES:
		return ipc_json_get_workspaces();
	case IPC_GET_OUTPUTS:
		return ipc_json_get_outputs();
	case IPC_GET_TREE:
		return ipc_json_get_tree();
	case IPC_GET_VERSION:
		return ipc_json_get_version();
	case IPC_GET_BINDING_MODES:
		return ipc_json_get_binding_modes();
	case IPC_GET_BINDING_STATE:
		return ipc_json_get_binding_state();
	case IPC_GET_CONFIG:
		return ipc_json_get_config();
	case IPC_GET_MARKS:
		return ipc_json_get_marks();
	case IPC_GET_BAR_CONFIG: {
		char *id = swine_strndup(payload, length);
		char *json = ipc_json_get_bar_config(id);
		free(id);
		return json;
	}
	case IPC_GET_INPUTS:
		return ipc_json_get_inputs();
	case IPC_GET_SEATS:
		return ipc_json_get_seats();
	case SWINE_IPC_GET_NOTIFICATIONS:
		return notify_list_json(notify_active());
	case SWINE_IPC_GET_NOTIFICATION_HISTORY:
		return notify_list_json(notify_history());
	case IPC_SEND_TICK: {
		// As in sway: the payload is any text, passed on as it is.
		char *tick = ipc_json_tick_event(false, payload, length);
		platform_ipc_event((uint32_t)IPC_EVENT_TICK, tick);
		free(tick);
		return simple_reply(true);
	}
	case IPC_SUBSCRIBE: {
		uint32_t subscriptions;
		return simple_reply(ipc_parse_subscriptions(payload, length,
			&subscriptions));
	}
	case IPC_SYNC:
		return simple_reply(false);
	default:
		swine_log(SWINE_INFO, "Unknown IPC message type %u", type);
		return NULL;
	}
}
