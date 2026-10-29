#ifndef _SWINE_IPC_CLIENT_H
#define _SWINE_IPC_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

// The payload of a SUBSCRIBE message from what the user typed: a JSON array
// as it is, otherwise event names separated by spaces or commas, with or
// without brackets and quotes. Windows PowerShell before 7.3 drops the inner
// quotes of '["window","workspace"]', which leaves "[window,workspace]".
// Caller frees.
char *ipc_subscribe_payload(const char *text);

// Windows only. The program's arguments in UTF-8 (main's argv is in the
// ANSI code page, which loses characters outside it).
char **ipc_utf8_argv(int *argc);

// Windows only. Returns the pipe name to use: $SWINESOCK if set, otherwise
// the per-user default. Caller frees.
char *get_ipc_pipe_name(void);

// Windows only. Sends one i3-ipc message to swinewm and returns the reply
// payload (NUL-terminated, caller frees), or NULL if swinewm cannot be
// reached. pipe_name NULL means get_ipc_pipe_name(). *error, if given,
// receives a message on failure (caller frees).
char *ipc_request(const char *pipe_name, uint32_t type, const char *payload,
		char **error);

// Windows only. A connection for more than one message (subscribing):
// ipc_connect returns NULL on failure (and sets *error); ipc_receive returns
// the next message's payload (caller frees) and its type, or NULL when the
// connection is gone.
void *ipc_connect(const char *pipe_name, char **error);
bool ipc_send(void *connection, uint32_t type, const char *payload);
char *ipc_receive(void *connection, uint32_t *type);
void ipc_close(void *connection);

#endif
