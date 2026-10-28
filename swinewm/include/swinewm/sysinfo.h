#ifndef _SWINE_SYSINFO_H
#define _SWINE_SYSINFO_H

#include <stdbool.h>

// System readings for bar modules, implemented by the platform layer.

struct sysinfo_battery {
	int capacity;      // percent
	bool charging;
	bool plugged;      // on AC power
	int seconds_left;  // until empty, 0 if Windows does not know
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

// A drive (Waybar's disk module): bytes in all, free, and free for the user.
struct sysinfo_disk {
	double total;
	double free;          // f_bfree
	double avail;         // f_bavail
};
// path: a folder on the drive ("C:\\"); false if it cannot be read.
bool sysinfo_disk(const char *path, struct sysinfo_disk *out);

// Degrees Celsius of an ACPI thermal zone (Waybar's "thermal-zone": its
// index). False if Windows reports none.
bool sysinfo_temperature(int zone, double *celsius);

enum sysinfo_net_family {
	SYSINFO_NET_IPV4,
	SYSINFO_NET_IPV6,
	SYSINFO_NET_IPV4_6,
};

// One network interface (Waybar's network module).
struct sysinfo_network {
	bool found;             // false: no such interface (disconnected)
	bool connected;         // has a carrier / is associated
	bool disabled;          // a wireless radio switched off (rfkill)
	bool wireless;
	char ifname[128];       // "Wi-Fi", "Ethernet"
	char ipaddr[64];
	char ipaddr6[64];
	char gwaddr[64];
	char netmask[64];
	char netmask6[64];
	int cidr;
	int cidr6;
	char essid[64];
	char bssid[32];
	int signal_strength;    // percent
	int signal_dbm;
	double frequency;       // GHz
	double rx_bytes;        // received since the interface came up
	double tx_bytes;
	double link_speed;      // bits per second
	double rx_bitrate;
	double tx_bitrate;
};
// pattern: an interface name ("Wi-Fi"; * and ? match anything), or NULL for the one
// the default route uses.
bool sysinfo_network(const char *pattern, struct sysinfo_network *out);

// The default audio output and input (Waybar's pulseaudio module).
struct sysinfo_audio {
	bool present;
	int volume;             // percent
	bool muted;
	bool bluetooth;
	char desc[128];         // the device's name
	char port[32];          // "speaker", "headphone", "hdmi", "headset", ...
	bool source_present;
	int source_volume;
	bool source_muted;
	char source_desc[128];
};
bool sysinfo_audio(struct sysinfo_audio *out);
// Sets the volume (percent) of the default output, or input (source).
void sysinfo_audio_set_volume(bool source, double percent);
// Mutes (1), unmutes (0) or toggles (-1) the default output or input.
void sysinfo_audio_set_mute(bool source, int mode);

// Keeps the display on and the computer awake (Waybar's idle_inhibitor),
// for as long as swinewm runs or until switched off again.
void sysinfo_inhibit_idle(bool enable);

// Caps Lock, Num Lock and Scroll Lock (Waybar's keyboard-state).
bool sysinfo_keyboard_locks(bool *caps, bool *num, bool *scroll);

#endif
