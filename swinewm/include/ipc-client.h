#ifndef _SWINE_IPC_CLIENT_H
#define _SWINE_IPC_CLIENT_H

#include <stdint.h>

// Windows only. Returns the pipe name to use: $SWINESOCK if set, otherwise
// the per-user default. Caller frees.
char *get_ipc_pipe_name(void);

#endif
