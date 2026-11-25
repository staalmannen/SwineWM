#ifndef _SWINE_SYSINFO_H
#define _SWINE_SYSINFO_H

#include <stdbool.h>
#include <stddef.h>

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

// The display's brightness (Waybar's backlight module): the built-in
// panel's, or with device an output's ("DISPLAY2", through DDC/CI). The
// value is read in the background; false until there is one (or if the
// display has no brightness Windows can read).
struct sysinfo_backlight {
	int percent;
	char device[64];        // "panel", or the output's name
};
bool sysinfo_backlight(const char *device, struct sysinfo_backlight *out);
// Sets the brightness (percent), in the background.
void sysinfo_backlight_set(const char *device, int percent);

// Media players (Waybar's mpris module): Windows' media sessions.
enum sysinfo_media_status {
	MEDIA_STOPPED,
	MEDIA_PLAYING,
	MEDIA_PAUSED,
};
struct sysinfo_media {
	char player[64];        // "spotify", "firefox", ...: from the app's ID
	enum sysinfo_media_status status;
	char title[256];
	char artist[256];
	char album_artist[256];
	char album[256];
	long long length_ms;    // -1 if unknown
	long long position_ms;  // -1 if unknown
};
// The players, the one Windows thinks current first; returns how many
// (at most max). Read in the background.
int sysinfo_media_players(struct sysinfo_media *out, int max);
enum sysinfo_media_action {
	MEDIA_PLAY_PAUSE,
	MEDIA_NEXT,
	MEDIA_PREVIOUS,
};
void sysinfo_media_control(const char *player, enum sysinfo_media_action action);

// Keeps the display on and the computer awake (Waybar's idle_inhibitor),
// for as long as swinewm runs or until switched off again.
void sysinfo_inhibit_idle(bool enable);

// Waybar's power-profiles-daemon: Windows' power mode, as the profile
// names of power-profiles-daemon ("power-saver", "balanced",
// "performance"), and what sets it ({driver}). False if unknown.
bool sysinfo_power_profile(char *profile, size_t size, char *driver,
		size_t driver_size);
void sysinfo_set_power_profile(const char *profile);

// Caps Lock, Num Lock and Scroll Lock (Waybar's keyboard-state).
bool sysinfo_keyboard_locks(bool *caps, bool *num, bool *scroll);

#endif
