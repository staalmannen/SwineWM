#ifndef _SWINE_IPC_SERVER_H
#define _SWINE_IPC_SERVER_H

#include <stdint.h>

// Portable request handler. The transport (named pipe on Windows) reads one
// i3-ipc message and calls this on the main thread. Returns a malloc'ed JSON
// reply; *reply_type receives the message type to send back.
char *ipc_handle_request(uint32_t type, const char *payload, uint32_t length,
		uint32_t *reply_type);

#endif
