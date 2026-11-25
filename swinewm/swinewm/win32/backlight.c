// The display's brightness for Waybar's backlight module.
//
// A laptop's built-in panel answers through WMI (root\WMI:
// WmiMonitorBrightness to read, WmiMonitorBrightnessMethods.WmiSetBrightness
// to set); external monitors through DDC/CI (dxva2's GetMonitorBrightness /
// SetMonitorBrightness). Both can take a long time (DDC/CI talks to the
// monitor over its cable), so a thread of its own reads every two seconds
// and does the changes; the bar only reads what it last found.
//
// "device" names an output ("DISPLAY2") for that monitor; anything else
// (a Linux name such as "intel_backlight", or none) picks the built-in
// panel, else the primary monitor.
#define COBJMACROS
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/sysinfo.h"
#include "win32.h"
#include <wbemidl.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>

#define POLL_MS 2000
#define MAX_DEVICES 8

struct device {
	char name[64];        // as the config gives it ("" for none)
	bool valid;           // percent was read
	int percent;
	char found[64];       // "panel" or the output's name
	int set_to;           // a change to make, or -1
};

static CRITICAL_SECTION lock;
static bool lock_ready = false;
static struct device devices[MAX_DEVICES];
static int device_count = 0;
static HANDLE thread = NULL;
static HANDLE wake = NULL;
static volatile LONG stopping = 0;

// --- WMI: the built-in panel ----------------------------------------------

static IWbemServices *wmi_connect(void) {
	IWbemLocator *locator = NULL;
	IWbemServices *services = NULL;
	if (FAILED(CoCreateInstance(&CLSID_WbemLocator, NULL,
			CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (void **)&locator))) {
		return NULL;
	}
	BSTR ns = SysAllocString(L"ROOT\\WMI");
	HRESULT hr = IWbemLocator_ConnectServer(locator, ns, NULL, NULL, NULL, 0,
		NULL, NULL, &services);
	SysFreeString(ns);
	IWbemLocator_Release(locator);
	if (FAILED(hr)) {
		return NULL;
	}
	CoSetProxyBlanket((IUnknown *)services, RPC_C_AUTHN_WINNT,
		RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL,
		RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
	return services;
}

// The first object a WQL query returns, or NULL.
static IWbemClassObject *wmi_first(IWbemServices *services,
		const wchar_t *query) {
	BSTR language = SysAllocString(L"WQL");
	BSTR text = SysAllocString(query);
	IEnumWbemClassObject *e = NULL;
	IWbemClassObject *object = NULL;
	if (SUCCEEDED(IWbemServices_ExecQuery(services, language, text,
			WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &e))) {
		ULONG n = 0;
		if (FAILED(IEnumWbemClassObject_Next(e, 2000, 1, &object, &n)) || !n) {
			object = NULL;
		}
		IEnumWbemClassObject_Release(e);
	}
	SysFreeString(language);
	SysFreeString(text);
	return object;
}

static bool panel_read(IWbemServices *services, int *percent) {
	IWbemClassObject *o = wmi_first(services,
		L"SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active=TRUE");
	if (!o) {
		return false;
	}
	VARIANT v;
	VariantInit(&v);
	bool ok = SUCCEEDED(IWbemClassObject_Get(o, L"CurrentBrightness", 0, &v,
		NULL, NULL));
	if (ok) {
		*percent = v.vt == VT_UI1 ? v.bVal : v.vt == VT_I4 ? (int)v.lVal : 0;
	}
	VariantClear(&v);
	IWbemClassObject_Release(o);
	return ok;
}

static void panel_set(IWbemServices *services, int percent) {
	IWbemClassObject *instance = wmi_first(services,
		L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active=TRUE");
	if (!instance) {
		return;
	}
	VARIANT path;
	VariantInit(&path);
	IWbemClassObject *cls = NULL, *in_def = NULL, *in = NULL;
	BSTR class_name = SysAllocString(L"WmiMonitorBrightnessMethods");
	BSTR method = SysAllocString(L"WmiSetBrightness");
	if (SUCCEEDED(IWbemClassObject_Get(instance, L"__PATH", 0, &path, NULL,
			NULL)) && path.vt == VT_BSTR &&
			SUCCEEDED(IWbemServices_GetObject(services, class_name, 0, NULL,
				&cls, NULL)) &&
			SUCCEEDED(IWbemClassObject_GetMethod(cls, method, 0, &in_def, NULL)) &&
			SUCCEEDED(IWbemClassObject_SpawnInstance(in_def, 0, &in))) {
		VARIANT timeout, brightness;
		VariantInit(&timeout);
		timeout.vt = VT_I4;
		timeout.lVal = 0;
		VariantInit(&brightness);
		brightness.vt = VT_UI1;
		brightness.bVal = (BYTE)percent;
		IWbemClassObject_Put(in, L"Timeout", 0, &timeout, 0);
		IWbemClassObject_Put(in, L"Brightness", 0, &brightness, 0);
		HRESULT hr = IWbemServices_ExecMethod(services, path.bstrVal, method, 0,
			NULL, in, NULL, NULL);
		if (FAILED(hr)) {
			swine_log(SWINE_DEBUG, "WmiSetBrightness failed: 0x%08lx",
				(unsigned long)hr);
		}
	}
	if (in) IWbemClassObject_Release(in);
	if (in_def) IWbemClassObject_Release(in_def);
	if (cls) IWbemClassObject_Release(cls);
	VariantClear(&path);
	SysFreeString(class_name);
	SysFreeString(method);
	IWbemClassObject_Release(instance);
}

// --- DDC/CI: external monitors --------------------------------------------

struct find_monitor {
	const char *output;   // NULL: the primary monitor
	HMONITOR found;
	char name[64];
};

static BOOL CALLBACK find_monitor_cb(HMONITOR monitor, HDC dc, LPRECT r,
		LPARAM data) {
	(void)dc;
	(void)r;
	struct find_monitor *f = (struct find_monitor *)data;
	MONITORINFOEXW mi = { .cbSize = sizeof(mi) };
	if (!GetMonitorInfoW(monitor, (MONITORINFO *)&mi)) {
		return TRUE;
	}
	char *device = utf16_to_utf8(mi.szDevice);   // "\\.\DISPLAY2"
	const char *name = device ? device : "";
	if (has_prefix(name, "\\\\.\\")) {
		name += 4;
	}
	bool match = f->output ? swine_strcasecmp(name, f->output) == 0 :
		(mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
	if (match) {
		f->found = monitor;
		snprintf(f->name, sizeof(f->name), "%s", name);
	}
	free(device);
	return !match;
}

// Reads (set < 0) or sets the brightness of a monitor through DDC/CI.
static bool ddc_brightness(HMONITOR monitor, int set, int *percent) {
	DWORD count = 0;
	if (!GetNumberOfPhysicalMonitorsFromHMONITOR(monitor, &count) || !count) {
		return false;
	}
	PHYSICAL_MONITOR *list = calloc(count, sizeof(*list));
	bool ok = false;
	if (GetPhysicalMonitorsFromHMONITOR(monitor, count, list)) {
		DWORD min = 0, cur = 0, max = 0;
		if (GetMonitorBrightness(list[0].hPhysicalMonitor, &min, &cur, &max) &&
				max > min) {
			if (set >= 0) {
				DWORD value = min + (DWORD)((max - min) * set / 100.0 + 0.5);
				ok = SetMonitorBrightness(list[0].hPhysicalMonitor, value) != 0;
				cur = value;
			} else {
				ok = true;
			}
			*percent = (int)((cur - min) * 100.0 / (max - min) + 0.5);
		}
		DestroyPhysicalMonitors(count, list);
	}
	free(list);
	return ok;
}

// --- the thread -----------------------------------------------------------

// Reads, or with set >= 0 changes, one device; true if it has a value.
static bool update_device(IWbemServices *services, const char *name, int set,
		int *percent, char *found, size_t found_size) {
	struct find_monitor f = {NULL, NULL, ""};
	if (name && *name) {
		f.output = name;
		EnumDisplayMonitors(NULL, NULL, find_monitor_cb, (LPARAM)&f);
	}
	if (!f.found && services) {
		// No such output: the built-in panel, if there is one.
		if (set >= 0) {
			panel_set(services, set);
		}
		if (panel_read(services, percent)) {
			snprintf(found, found_size, "panel");
			return true;
		}
	}
	if (!f.found) {
		f.output = NULL;
		EnumDisplayMonitors(NULL, NULL, find_monitor_cb, (LPARAM)&f);
	}
	if (f.found && ddc_brightness(f.found, set, percent)) {
		snprintf(found, found_size, "%s", f.name);
		return true;
	}
	return false;
}

static DWORD WINAPI backlight_thread(LPVOID param) {
	(void)param;
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	IWbemServices *services = wmi_connect();
	while (!stopping) {
		struct device snapshot[MAX_DEVICES];
		int n;
		EnterCriticalSection(&lock);
		n = device_count;
		memcpy(snapshot, devices, sizeof(snapshot));
		for (int i = 0; i < n; ++i) {
			devices[i].set_to = -1;   // taken
		}
		LeaveCriticalSection(&lock);
		for (int i = 0; i < n && !stopping; ++i) {
			int percent = 0;
			char found[64] = "";
			bool ok = update_device(services, snapshot[i].name,
				snapshot[i].set_to, &percent, found, sizeof(found));
			EnterCriticalSection(&lock);
			devices[i].valid = ok;
			if (ok && devices[i].set_to < 0) {
				devices[i].percent = percent;
				memcpy(devices[i].found, found, sizeof(found));
			}
			LeaveCriticalSection(&lock);
		}
		WaitForSingleObject(wake, POLL_MS);
	}
	if (services) {
		IWbemServices_Release(services);
	}
	CoUninitialize();
	return 0;
}

// The entry for a device name, added (and the thread started) if new.
// With the lock held.
static struct device *device_entry(const char *name) {
	const char *key = name ? name : "";
	for (int i = 0; i < device_count; ++i) {
		if (strcmp(devices[i].name, key) == 0) {
			return &devices[i];
		}
	}
	if (device_count == MAX_DEVICES) {
		return NULL;
	}
	struct device *d = &devices[device_count++];
	memset(d, 0, sizeof(*d));
	snprintf(d->name, sizeof(d->name), "%s", key);
	d->set_to = -1;
	if (!thread) {
		wake = CreateEventW(NULL, FALSE, FALSE, NULL);
		thread = CreateThread(NULL, 0, backlight_thread, NULL, 0, NULL);
	}
	SetEvent(wake);
	return d;
}

static void ensure_lock(void) {
	if (!lock_ready) {
		InitializeCriticalSection(&lock);
		lock_ready = true;
	}
}

bool sysinfo_backlight(const char *device, struct sysinfo_backlight *out) {
	ensure_lock();
	EnterCriticalSection(&lock);
	struct device *d = device_entry(device);
	bool ok = d && d->valid;
	if (ok) {
		out->percent = d->percent;
		memcpy(out->device, d->found, sizeof(out->device));
	}
	LeaveCriticalSection(&lock);
	return ok;
}

void sysinfo_backlight_set(const char *device, int percent) {
	ensure_lock();
	EnterCriticalSection(&lock);
	struct device *d = device_entry(device);
	if (d) {
		d->set_to = percent < 0 ? 0 : percent > 100 ? 100 : percent;
		d->percent = d->set_to;   // shown at once; read back afterwards
	}
	LeaveCriticalSection(&lock);
	if (wake) {
		SetEvent(wake);
	}
}

void win32_backlight_finish(void) {
	if (thread) {
		InterlockedExchange(&stopping, 1);
		SetEvent(wake);
		// DDC/CI can be slow; do not wait long for it.
		if (WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0) {
			CloseHandle(wake);
		}
		CloseHandle(thread);
		thread = NULL;
	}
}
