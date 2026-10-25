#include <stdlib.h>
#include "swinewm/bar/bar.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/notify.h"
#include "swinewm/input/keyboard.h"
#include "swinewm/input/seat.h"
#include "swinewm/server.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/view.h"

struct swine_server server = {0};

void server_init(void) {
	server.views = create_list();
	seat_create();
	root_create();
	server.running = true;
}

void server_fini(void) {
	while (server.views->length) {
		struct swine_view *view = server.views->items[server.views->length - 1];
		transaction_forget_view(view);
		view_destroy(view);
	}
	root_destroy(root);
	bar_finish();
	notify_finish();
	keyboard_layouts_finish();
	seat_destroy(seat);
	list_free(server.views);
	server.views = NULL;
	server.running = false;
}
