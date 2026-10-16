#ifndef _SWINE_BAR_TRAY_H
#define _SWINE_BAR_TRAY_H

#include <stdbool.h>
#include <stdint.h>
#include "list.h"

// The items of the notification area ("system tray"), as Waybar's tray
// module shows them. On Linux they are StatusNotifierItems found over
// D-Bus; on Windows the platform hosts the notification area and reports
// its icons here. Main thread only.

struct tray_item {
	uint64_t id;          // chosen by the platform, never 0
	char *app_id;         // the program, like a view's app_id ("Discord.exe")
	char *title;          // tooltip text
	bool passive;         // hidden by its program (Waybar status "Passive")
	bool needs_attention;
};

// Adds the item, or updates it in place if the id is known.
void tray_item_set(uint64_t id, const char *app_id, const char *title,
		bool passive);
bool tray_item_remove(uint64_t id);
void tray_clear(void);
// In the order the items appeared.
list_t *tray_get_items(void);

#endif
