// Readings for the bar's battery, memory, cpu, disk and temperature
// modules, and the idle inhibitor. Network: network.c; audio: audio.c;
// lock keys: keyboard.c.
#include <stdlib.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <powrprof.h>
#include <stdio.h>
#include <string.h>
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
	out->seconds_left = status.BatteryLifeTime == (DWORD)-1 ? 0 :
		(int)status.BatteryLifeTime;
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

// Waybar's power-profiles-daemon: Windows' power mode ("Best power
// efficiency", "Balanced", "Best performance"), an overlay on the Balanced
// power plan set through powrprof's (undocumented, so looked up at run
// time) overlay functions. With another plan active, or without them, the
// classic plans stand in: Power saver, Balanced, High performance.
static const GUID plan_balanced = {0x381b4222, 0xf694, 0x41f0,
	{0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e}};
static const GUID plan_saver = {0xa1841308, 0x3541, 0x4fab,
	{0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a}};
static const GUID plan_high = {0x8c5e7fda, 0xe8bf, 0x4a96,
	{0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
static const GUID plan_ultimate = {0xe9a42b02, 0xd5df, 0x448d,
	{0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};
static const GUID overlay_efficiency = {0x961cc777, 0x2547, 0x4f9d,
	{0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a}};
static const GUID overlay_performance = {0xded574b5, 0x45a0, 0x4f42,
	{0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}};
static const GUID overlay_balanced = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};

typedef DWORD (WINAPI *get_overlay_fn)(GUID *);
typedef DWORD (WINAPI *set_overlay_fn)(GUID);
static get_overlay_fn get_overlay = NULL;
static set_overlay_fn set_overlay = NULL;

static void load_overlay_functions(void) {
	static bool loaded = false;
	if (loaded) {
		return;
	}
	loaded = true;
	HMODULE powrprof = GetModuleHandleW(L"powrprof.dll");
	if (!powrprof) {
		powrprof = LoadLibraryW(L"powrprof.dll");
	}
	if (powrprof) {
		get_overlay = (get_overlay_fn)(void (*)(void))GetProcAddress(powrprof,
			"PowerGetEffectiveOverlayScheme");
		set_overlay = (set_overlay_fn)(void (*)(void))GetProcAddress(powrprof,
			"PowerSetActiveOverlayScheme");
	}
}

static bool active_plan(GUID *out) {
	GUID *active = NULL;
	if (PowerGetActiveScheme(NULL, &active) != ERROR_SUCCESS || !active) {
		return false;
	}
	*out = *active;
	LocalFree(active);
	return true;
}

bool sysinfo_power_profile(char *profile, size_t size, char *driver,
		size_t driver_size) {
	load_overlay_functions();
	GUID plan;
	if (!active_plan(&plan)) {
		return false;
	}
	GUID overlay;
	if (IsEqualGUID(&plan, &plan_balanced) && get_overlay &&
			get_overlay(&overlay) == ERROR_SUCCESS) {
		snprintf(profile, size, "%s",
			IsEqualGUID(&overlay, &overlay_efficiency) ? "power-saver" :
			IsEqualGUID(&overlay, &overlay_performance) ? "performance" :
			"balanced");
		snprintf(driver, driver_size, "Windows power mode");
		return true;
	}
	snprintf(profile, size, "%s",
		IsEqualGUID(&plan, &plan_saver) ? "power-saver" :
		IsEqualGUID(&plan, &plan_high) || IsEqualGUID(&plan, &plan_ultimate) ?
		"performance" : "balanced");
	snprintf(driver, driver_size, "Windows power plan");
	return true;
}

void sysinfo_set_power_profile(const char *profile) {
	load_overlay_functions();
	bool saver = strcmp(profile, "power-saver") == 0;
	bool performance = strcmp(profile, "performance") == 0;
	GUID plan;
	bool balanced_plan = active_plan(&plan) &&
		IsEqualGUID(&plan, &plan_balanced);
	if (!balanced_plan || !set_overlay) {
		// The classic plans; if this one is missing (Modern Standby PCs
		// have only Balanced), Balanced with the power mode below.
		const GUID *want = saver ? &plan_saver : performance ? &plan_high :
			&plan_balanced;
		if (PowerSetActiveScheme(NULL, want) == ERROR_SUCCESS || !set_overlay) {
			return;
		}
		PowerSetActiveScheme(NULL, &plan_balanced);
	}
	set_overlay(saver ? overlay_efficiency : performance ? overlay_performance :
		overlay_balanced);
}
