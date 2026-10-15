// Readings for the bar's battery, memory and cpu modules.
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
