#include <stdlib.h>
#include <string.h>
#include "ipc.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/ipc-json.h"
#include "swinewm/ipc-server.h"
#include "swinewm/notify.h"

static char *simple_reply(bool success) {
	return swine_strdup(success ? "{\"success\":true}" : "{\"success\":false}");
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
	case IPC_GET_BAR_CONFIG:
	case IPC_GET_INPUTS:
	case IPC_GET_SEATS:
		return swine_strdup("[]");
	case SWINE_IPC_GET_NOTIFICATIONS:
		return notify_list_json(notify_active());
	case SWINE_IPC_GET_NOTIFICATION_HISTORY:
		return notify_list_json(notify_history());
	case IPC_SEND_TICK:
		return simple_reply(true);
	case IPC_SUBSCRIBE:
		// Events are not implemented yet; tell the client so.
		swine_log(SWINE_INFO, "IPC subscribe is not implemented yet");
		return simple_reply(false);
	case IPC_SYNC:
		return simple_reply(false);
	default:
		swine_log(SWINE_INFO, "Unknown IPC message type %u", type);
		return NULL;
	}
}
