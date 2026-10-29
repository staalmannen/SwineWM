// Monitors become sway outputs. Windows reports a monitor's full rectangle
// and its work area (the part not covered by the taskbar or other app bars);
// tiling uses the work area, fullscreen the full rectangle.
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "list.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/platform.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "win32.h"
#include <shellscalingapi.h>

struct monitor_info {
	HMONITOR handle;
	char *name;
	char *friendly;   // monitor model as shown in the display settings
	RECT monitor;
	RECT work;
	bool primary;
};

// The monitor's friendly name ("DELL U2720Q") for a GDI device name
// ("\\.\DISPLAY1"), from the display configuration API. NULL if unknown
// (internal panels often have none).
static char *monitor_friendly_name(const wchar_t *gdi_device) {
	UINT32 path_count = 0, mode_count = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count,
			&mode_count) != ERROR_SUCCESS || path_count == 0) {
		return NULL;
	}
	DISPLAYCONFIG_PATH_INFO *paths = calloc(path_count, sizeof(*paths));
	DISPLAYCONFIG_MODE_INFO *modes = calloc(mode_count ? mode_count : 1,
		sizeof(*modes));
	char *result = NULL;
	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths,
			&mode_count, modes, NULL) != ERROR_SUCCESS) {
		goto out;
	}
	for (UINT32 i = 0; i < path_count && !result; ++i) {
		DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {0};
		source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		source.header.size = sizeof(source);
		source.header.adapterId = paths[i].sourceInfo.adapterId;
		source.header.id = paths[i].sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
				wcscmp(source.viewGdiDeviceName, gdi_device) != 0) {
			continue;
		}
		DISPLAYCONFIG_TARGET_DEVICE_NAME target = {0};
		target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
		target.header.size = sizeof(target);
		target.header.adapterId = paths[i].targetInfo.adapterId;
		target.header.id = paths[i].targetInfo.id;
		if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS &&
				target.monitorFriendlyDeviceName[0]) {
			result = utf16_to_utf8(target.monitorFriendlyDeviceName);
		}
	}
out:
	free(paths);
	free(modes);
	return result;
}

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
	mi->friendly = monitor_friendly_name(info.szDevice);
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

static wchar_t *device_name(struct swine_output *output) {
	char *name = format_str("\\\\.\\%s", output->name);
	wchar_t *wname = utf8_to_utf16(name);
	free(name);
	return wname;
}

// Display settings are changed "dynamically": Windows does not save them,
// and ChangeDisplaySettingsEx(NULL, NULL, ...) returns every monitor to the
// settings in the registry (what the Settings app shows). That happens when
// swinewm exits or crashes, and from --recover, so a mode that leaves a
// monitor black cannot outlive swinewm.
static list_t *changed_outputs;   // char *: names of reconfigured outputs

static bool find_mode(const wchar_t *device, int width, int height,
		float refresh_rate, DWORD bits, DEVMODEW *result) {
	bool found = false;
	DEVMODEW mode = { .dmSize = sizeof(mode) };
	int wanted_hz = (int)(refresh_rate + 0.5f);
	for (DWORD i = 0; EnumDisplaySettingsExW(device, i, &mode, EDS_ROTATEDMODE);
			++i) {
		// Rotated modes are listed with width and height swapped.
		bool same = ((int)mode.dmPelsWidth == width &&
				(int)mode.dmPelsHeight == height) ||
			((int)mode.dmPelsWidth == height && (int)mode.dmPelsHeight == width);
		if (!same || mode.dmBitsPerPel != bits) {
			continue;
		}
		int hz = (int)mode.dmDisplayFrequency;
		int best_hz = found ? (int)result->dmDisplayFrequency : 0;
		bool better = !found ||
			(wanted_hz > 0 ? abs(hz - wanted_hz) < abs(best_hz - wanted_hz) :
				hz > best_hz);
		if (better) {
			*result = mode;
			found = true;
		}
	}
	return found;
}

static void note_changed(struct swine_output *output) {
	if (!changed_outputs) {
		changed_outputs = create_list();
	}
	for (int i = 0; i < changed_outputs->length; ++i) {
		if (strcmp(changed_outputs->items[i], output->name) == 0) {
			return;
		}
	}
	list_add(changed_outputs, swine_strdup(output->name));
	win32_recover_note_display_changed();
}

bool platform_output_configure(struct swine_output *output,
		const struct output_display_config *cfg) {
	wchar_t *device = device_name(output);
	DEVMODEW current = { .dmSize = sizeof(current) };
	if (!EnumDisplaySettingsExW(device, ENUM_CURRENT_SETTINGS, &current, 0)) {
		free(device);
		return false;
	}
	if (!cfg->any) {
		// No longer configured (e.g. after a reload): back to the default.
		for (int i = 0; changed_outputs && i < changed_outputs->length; ++i) {
			if (strcmp(changed_outputs->items[i], output->name) == 0) {
				ChangeDisplaySettingsExW(device, NULL, NULL, 0, NULL);
				free(changed_outputs->items[i]);
				list_del(changed_outputs, i);
				break;
			}
		}
		free(device);
		return true;
	}

	bool ok = true;
	int current_transform = (int)(current.dmDisplayOrientation & 3);
	int transform = cfg->transform >= 0 ? cfg->transform : current_transform;
	// Mode in the unrotated frame, as sway's "mode" is.
	int width = (int)current.dmPelsWidth, height = (int)current.dmPelsHeight;
	if (current_transform & 1) {
		width = (int)current.dmPelsHeight;
		height = (int)current.dmPelsWidth;
	}
	DWORD frequency = current.dmDisplayFrequency;
	if (cfg->width > 0 && cfg->height > 0) {
		DEVMODEW mode;
		if (find_mode(device, cfg->width, cfg->height, cfg->refresh_rate,
				current.dmBitsPerPel, &mode)) {
			width = cfg->width;
			height = cfg->height;
			frequency = mode.dmDisplayFrequency;
		} else {
			swine_log(SWINE_ERROR, "Output %s does not support the mode "
				"%dx%d", output->name, cfg->width, cfg->height);
			ok = false;
		}
	}

	DEVMODEW dm = current;
	dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY |
		DM_DISPLAYORIENTATION;
	dm.dmPelsWidth = (DWORD)((transform & 1) ? height : width);
	dm.dmPelsHeight = (DWORD)((transform & 1) ? width : height);
	dm.dmDisplayFrequency = frequency;
	dm.dmDisplayOrientation = (DWORD)transform;
	bool same_position = true;
	if (cfg->x != INT_MIN && cfg->y != INT_MIN) {
		dm.dmFields |= DM_POSITION;
		dm.dmPosition.x = cfg->x;
		dm.dmPosition.y = cfg->y;
		same_position = current.dmPosition.x == cfg->x &&
			current.dmPosition.y == cfg->y;
	}
	if (dm.dmPelsWidth == current.dmPelsWidth &&
			dm.dmPelsHeight == current.dmPelsHeight &&
			dm.dmDisplayFrequency == current.dmDisplayFrequency &&
			dm.dmDisplayOrientation == current.dmDisplayOrientation &&
			same_position) {
		free(device);
		return ok;
	}

	swine_log(SWINE_INFO, "Output %s: %lux%lu@%luHz, rotation %d, at %ld,%ld",
		output->name, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency,
		transform * 90, dm.dmPosition.x, dm.dmPosition.y);
	LONG result = ChangeDisplaySettingsExW(device, &dm, NULL, CDS_TEST, NULL);
	if (result == DISP_CHANGE_SUCCESSFUL) {
		note_changed(output);
		result = ChangeDisplaySettingsExW(device, &dm, NULL, 0, NULL);
	}
	if (result != DISP_CHANGE_SUCCESSFUL) {
		swine_log(SWINE_ERROR, "Windows refused the settings for output %s "
			"(error %ld)", output->name, result);
		ok = false;
	}
	free(device);
	return ok;
}

void win32_outputs_restore_settings(void) {
	if (changed_outputs && changed_outputs->length) {
		ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
		swine_log(SWINE_INFO, "Restored the display settings");
	}
}

void win32_outputs_finish(void) {
	win32_outputs_restore_settings();
	list_free_items_and_destroy(changed_outputs);
	changed_outputs = NULL;
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
		free(output->identifier);
		output->identifier = mi->friendly;
		mi->friendly = NULL;
		free(output->description);
		output->description = output->identifier ?
			format_str("%s (%s)", output->identifier, output->name) : NULL;
		output->primary = mi->primary;
		output->box = box_from_rect(&mi->monitor);
		// As the shell, swinewm sets the work area itself (monitor minus
		// the bar); its own reservation is applied on top of the monitor.
		// Next to Explorer, the work area without the bars' AppBar strips
		// (appbar.c): they are reserved inside swinewm's layout instead.
		RECT work = mi->work;
		if (!win32_shell_mode) {
			win32_appbar_work_area(&work);
		}
		output->usable = win32_shell_mode ? output->box : box_from_rect(&work);
		UINT dpi_x = 96, dpi_y = 96;
		if (SUCCEEDED(GetDpiForMonitor(mi->handle, MDT_EFFECTIVE_DPI, &dpi_x,
				&dpi_y))) {
			output->scale = dpi_x / 96.0;
		}
		DEVMODEW current = { .dmSize = sizeof(current) };
		wchar_t *device = device_name(output);
		if (EnumDisplaySettingsExW(device, ENUM_CURRENT_SETTINGS, &current, 0)) {
			output->transform = (int)(current.dmDisplayOrientation & 3);
			output->refresh_mhz = current.dmDisplayFrequency > 1 ?
				(int)current.dmDisplayFrequency * 1000 : 0;
		}
		free(device);
		if (is_new) {
			output_enable(output);
		}
		free(mi->name);
		free(mi->friendly);
		free(mi);
	}
	list_free(monitors);
	root_update_box();
}
