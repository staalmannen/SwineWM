#ifndef _SWINE_IPC_SERVER_H
#define _SWINE_IPC_SERVER_H

#include <stdbool.h>
#include <stdint.h>

enum ipc_subscription {
	IPC_SUB_WORKSPACE = 1u << 0,
	IPC_SUB_OUTPUT = 1u << 1,
	IPC_SUB_MODE = 1u << 2,
	IPC_SUB_WINDOW = 1u << 3,
	IPC_SUB_BARCONFIG_UPDATE = 1u << 4,
	IPC_SUB_BINDING = 1u << 5,
	IPC_SUB_SHUTDOWN = 1u << 6,
	IPC_SUB_TICK = 1u << 7,
	IPC_SUB_INPUT = 1u << 8,
};

// Parses the sway SUBSCRIBE payload into a bit mask. Unknown event names and
// malformed payloads are rejected.
bool ipc_parse_subscriptions(const char *payload, uint32_t length,
		uint32_t *subscriptions);
// A tick event: {"first": first, "payload": "<payload>"}. SEND_TICK sends
// first false to every tick subscriber; a new subscriber gets first true.
char *ipc_json_tick_event(bool first, const char *payload, uint32_t length);

// Portable request handler. The transport (named pipe on Windows) reads one
// i3-ipc message and calls this on the main thread. Returns a malloc'ed JSON
// reply; *reply_type receives the message type to send back.
char *ipc_handle_request(uint32_t type, const char *payload, uint32_t length,
		uint32_t *reply_type);

#endif
