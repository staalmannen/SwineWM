#ifndef _SWINE_IPC_H
#define _SWINE_IPC_H

#include <stdint.h>

// Wire format is the i3/sway IPC protocol: the 6-byte magic "i3-ipc", a
// 32-bit payload length and a 32-bit message type (native byte order),
// followed by the payload. Message numbers match sway's include/ipc.h so that
// i3/sway IPC libraries can be pointed at the swinewm named pipe.
#define IPC_MAGIC "i3-ipc"
#define IPC_MAGIC_LEN 6
#define IPC_HEADER_SIZE (IPC_MAGIC_LEN + 8)

enum ipc_command_type {
	IPC_COMMAND = 0,
	IPC_GET_WORKSPACES = 1,
	IPC_SUBSCRIBE = 2,
	IPC_GET_OUTPUTS = 3,
	IPC_GET_TREE = 4,
	IPC_GET_MARKS = 5,
	IPC_GET_BAR_CONFIG = 6,
	IPC_GET_VERSION = 7,
	IPC_GET_BINDING_MODES = 8,
	IPC_GET_CONFIG = 9,
	IPC_SEND_TICK = 10,
	IPC_SYNC = 11,
	IPC_GET_BINDING_STATE = 12,

	IPC_GET_INPUTS = 100,
	IPC_GET_SEATS = 101,

	// swinewm extensions (the integrated Mako's "makoctl list/history")
	SWINE_IPC_GET_NOTIFICATIONS = 1000,
	SWINE_IPC_GET_NOTIFICATION_HISTORY = 1001,

	IPC_EVENT_WORKSPACE = (int)((1u << 31) | 0),
	IPC_EVENT_OUTPUT = (int)((1u << 31) | 1),
	IPC_EVENT_MODE = (int)((1u << 31) | 2),
	IPC_EVENT_WINDOW = (int)((1u << 31) | 3),
	IPC_EVENT_BARCONFIG_UPDATE = (int)((1u << 31) | 4),
	IPC_EVENT_BINDING = (int)((1u << 31) | 5),
	IPC_EVENT_SHUTDOWN = (int)((1u << 31) | 6),
	IPC_EVENT_TICK = (int)((1u << 31) | 7),
};

// Default pipe name pattern; %s is the user name. Overridden by SWINESOCK.
#define SWINE_IPC_PIPE_FMT "\\\\.\\pipe\\swinewm-ipc.%s"

#endif
