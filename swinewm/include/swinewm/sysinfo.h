#ifndef _SWINE_SYSINFO_H
#define _SWINE_SYSINFO_H

#include <stdbool.h>

// System readings for bar modules, implemented by the platform layer.

struct sysinfo_battery {
	int capacity;      // percent
	bool charging;
	bool plugged;      // on AC power
};

struct sysinfo_memory {
	double total_gib;
	double used_gib;
	double avail_gib;
	int percentage;    // used
};

// False if there is no battery.
bool sysinfo_battery(struct sysinfo_battery *out);
bool sysinfo_memory(struct sysinfo_memory *out);
// Average CPU usage in percent since the previous call.
bool sysinfo_cpu(int *usage);

#endif
