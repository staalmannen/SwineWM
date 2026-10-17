#ifndef _SWINE_IPC_CLIENT_H
#define _SWINE_IPC_CLIENT_H

#include <stdint.h>

// Windows only. Returns the pipe name to use: $SWINESOCK if set, otherwise
// the per-user default. Caller frees.
char *get_ipc_pipe_name(void);

// Windows only. Sends one i3-ipc message to swinewm and returns the reply
// payload (NUL-terminated, caller frees), or NULL if swinewm cannot be
// reached. pipe_name NULL means get_ipc_pipe_name(). *error, if given,
// receives a message on failure (caller frees).
char *ipc_request(const char *pipe_name, uint32_t type, const char *payload,
		char **error);

#endif
