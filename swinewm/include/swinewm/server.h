#ifndef _SWINE_SERVER_H
#define _SWINE_SERVER_H

#include <stdbool.h>
#include "list.h"

#include "swinewm_version.h"   // SWINEWM_VERSION, generated from git

struct swine_server {
	list_t *views;     // struct swine_view *, mapped and unmapped
	bool running;
};

extern struct swine_server server;

void server_init(void);
void server_fini(void);

#endif
