// Readings for the bar's battery, memory, cpu, disk and temperature
// modules, and the idle inhibitor. Network: network.c; audio: audio.c;
// lock keys: keyboard.c.
#include <stdlib.h>
#include <pdh.h>
#include <pdhmsg.h>
#include "stringop.h"
#include "swinewm/sysinfo.h"
#include "win32.h"

bool sysinfo_battery(struct sysinfo_battery *out) {
	SYSTEM_POWER_STATUS status;
	if (!GetSystemPowerStatus(&status) || (status.BatteryFlag & 128) ||
			status.BatteryFlag == 255 || status.BatteryLifePercent > 100) {
		return false;   // no battery, or unknown
	}
	out->capacity = status.BatteryLifePercent;
	out->charging = (status.BatteryFlag & 8) != 0;
	out->plugged = status.ACLineStatus == 1;
	return true;
}

bool sysinfo_memory(struct sysinfo_memory *out) {
	MEMORYSTATUSEX mem = { .dwLength = sizeof(mem) };
	if (!GlobalMemoryStatusEx(&mem)) {
		return false;
	}
	const double gib = 1024.0 * 1024.0 * 1024.0;
	out->total_gib = (double)mem.ullTotalPhys / gib;
	out->avail_gib = (double)mem.ullAvailPhys / gib;
	out->used_gib = out->total_gib - out->avail_gib;
	out->percentage = (int)mem.dwMemoryLoad;
	return true;
}

static ULONGLONG to_u64(FILETIME ft) {
	return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

bool sysinfo_cpu(int *usage) {
	static ULONGLONG last_idle = 0, last_total = 0, last_tick = 0;
	static int last_usage = 0;
	// The bar is rebuilt on every tree change; only sample once a second so
	// the value is an average rather than noise.
	ULONGLONG tick = GetTickCount64();
	if (last_tick && tick - last_tick < 1000) {
		*usage = last_usage;
		return true;
	}
	last_tick = tick;
	FILETIME idle, kernel, user;
	if (!GetSystemTimes(&idle, &kernel, &user)) {
		return false;
	}
	// Kernel time includes idle time.
	ULONGLONG idle_now = to_u64(idle);
	ULONGLONG total_now = to_u64(kernel) + to_u64(user);
	ULONGLONG idle_delta = idle_now - last_idle;
	ULONGLONG total_delta = total_now - last_total;
	last_idle = idle_now;
	last_total = total_now;
	if (total_delta == 0) {
		*usage = 0;
		return true;
	}
	last_usage = (int)(100 - (idle_delta * 100) / total_delta);
	*usage = last_usage;
	return true;
}

bool sysinfo_disk(const char *path, struct sysinfo_disk *out) {
	wchar_t *wide = utf8_to_utf16(path);
	ULARGE_INTEGER avail = {0}, total = {0}, free_bytes = {0};
	BOOL ok = wide && GetDiskFreeSpaceExW(wide, &avail, &total, &free_bytes);
	free(wide);
	if (!ok) {
		return false;
	}
	out->total = (double)total.QuadPart;
	out->free = (double)free_bytes.QuadPart;
	out->avail = (double)avail.QuadPart;
	return true;
}

// The "Thermal Zone Information" performance counters: the ACPI thermal
// zones in Kelvin, readable without administrator rights (unlike WMI's
// MSAcpi_ThermalZoneTemperature). Many desktops report none.
static PDH_HQUERY temperature_query = NULL;
static PDH_HCOUNTER temperature_counter = NULL;
static bool temperature_failed = false;

bool sysinfo_temperature(int zone, double *celsius) {
	if (temperature_failed) {
		return false;
	}
	if (!temperature_query) {
		if (PdhOpenQueryW(NULL, 0, &temperature_query) != ERROR_SUCCESS ||
				PdhAddEnglishCounterW(temperature_query,
				L"\\Thermal Zone Information(*)\\Temperature", 0,
				&temperature_counter) != ERROR_SUCCESS) {
			if (temperature_query) {
				PdhCloseQuery(temperature_query);
				temperature_query = NULL;
			}
			temperature_failed = true;
			return false;
		}
	}
	if (PdhCollectQueryData(temperature_query) != ERROR_SUCCESS) {
		return false;
	}
	DWORD size = 0, count = 0;
	PdhGetFormattedCounterArrayW(temperature_counter, PDH_FMT_DOUBLE, &size,
		&count, NULL);
	if (size == 0) {
		return false;
	}
	PDH_FMT_COUNTERVALUE_ITEM_W *items = malloc(size);
	bool found = false;
	if (PdhGetFormattedCounterArrayW(temperature_counter, PDH_FMT_DOUBLE, &size,
			&count, items) == ERROR_SUCCESS && zone >= 0 && (DWORD)zone < count &&
			items[zone].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA &&
			items[zone].FmtValue.doubleValue > 0) {
		*celsius = items[zone].FmtValue.doubleValue - 273.15;
		found = true;
	}
	free(items);
	return found;
}

// The execution state belongs to this (the main) thread and ends with the
// process, so nothing is left behind after an exit or a crash.
void sysinfo_inhibit_idle(bool enable) {
	SetThreadExecutionState(enable ?
		ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED :
		ES_CONTINUOUS);
}
