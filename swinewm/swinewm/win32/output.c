// Monitors become sway outputs. Windows reports a monitor's full rectangle
// and its work area (the part not covered by the taskbar or other app bars);
// tiling uses the work area, fullscreen the full rectangle.
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "list.h"
#include "log.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "win32.h"
#include <shellscalingapi.h>

struct monitor_info {
	HMONITOR handle;
	char *name;
	RECT monitor;
	RECT work;
	bool primary;
};

static BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC hdc, LPRECT rect,
		LPARAM data) {
	(void)hdc;
	(void)rect;
	list_t *list = (list_t *)data;
	MONITORINFOEXW info = { .cbSize = sizeof(info) };
	if (!GetMonitorInfoW(monitor, (MONITORINFO *)&info)) {
		return TRUE;
	}
	struct monitor_info *mi = calloc(1, sizeof(*mi));
	mi->handle = monitor;
	// "\\.\DISPLAY1" -> "DISPLAY1"
	const wchar_t *device = info.szDevice;
	if (wcsncmp(device, L"\\\\.\\", 4) == 0) {
		device += 4;
	}
	mi->name = utf16_to_utf8(device);
	mi->monitor = info.rcMonitor;
	mi->work = info.rcWork;
	mi->primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
	// Primary monitor first so that workspace 1 lands there.
	if (mi->primary) {
		list_insert(list, 0, mi);
	} else {
		list_add(list, mi);
	}
	return TRUE;
}

static struct swine_box box_from_rect(const RECT *r) {
	return (struct swine_box){r->left, r->top, r->right - r->left, r->bottom - r->top};
}

void win32_outputs_update(void) {
	list_t *monitors = create_list();
	EnumDisplayMonitors(NULL, NULL, collect_monitor, (LPARAM)monitors);

	// Remove outputs whose monitor disappeared.
	for (int i = root->outputs->length - 1; i >= 0; --i) {
		struct swine_output *output = root->outputs->items[i];
		bool found = false;
		for (int j = 0; j < monitors->length && !found; ++j) {
			struct monitor_info *mi = monitors->items[j];
			found = strcmp(mi->name, output->name) == 0;
		}
		if (!found) {
			swine_log(SWINE_INFO, "Output %s removed", output->name);
			output_destroy(output);
		}
	}

	// Update existing outputs and add new ones.
	for (int j = 0; j < monitors->length; ++j) {
		struct monitor_info *mi = monitors->items[j];
		struct swine_output *output = output_by_name(mi->name);
		bool is_new = output == NULL;
		if (is_new) {
			output = output_create(mi->name, mi->handle);
		}
		output->handle = mi->handle;
		output->primary = mi->primary;
		output->box = box_from_rect(&mi->monitor);
		output->usable = box_from_rect(&mi->work);
		UINT dpi_x = 96, dpi_y = 96;
		if (SUCCEEDED(GetDpiForMonitor(mi->handle, MDT_EFFECTIVE_DPI, &dpi_x,
				&dpi_y))) {
			output->scale = dpi_x / 96.0;
		}
		if (is_new) {
			output_enable(output);
		}
		free(mi->name);
		free(mi);
	}
	list_free(monitors);
	root_update_box();
}
