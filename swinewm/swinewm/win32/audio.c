// Readings for the bar's pulseaudio module (sysinfo_audio): the default
// output and input endpoints of Core Audio, their volume, mute, name, form
// factor ("speaker", "headphone", ...) and whether they are Bluetooth.
// Runs on the main thread, which platform_init initialised for COM.
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stringop.h"
#include "swinewm/sysinfo.h"
#include "win32.h"

// Spelled out here so that MSVC and MinGW link without extra libraries.
static const CLSID clsid_enumerator = {0xBCDE0395, 0xE52F, 0x467C,
	{0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID iid_enumerator = {0xA95664D2, 0x9614, 0x4F35,
	{0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID iid_endpoint_volume = {0x5CDF2C82, 0x841E, 0x4546,
	{0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};
// PKEY_AudioEndpoint_FormFactor and PKEY_Device_EnumeratorName
static const PROPERTYKEY key_form_factor = {{0x1da5d803, 0xd492, 0x4edd,
	{0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 0};
static const PROPERTYKEY key_enumerator = {{0xa45c254e, 0xdf1c, 0x4efd,
	{0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 24};
static const PROPERTYKEY key_friendly_name = {{0xa45c254e, 0xdf1c, 0x4efd,
	{0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

static IMMDeviceEnumerator *enumerator = NULL;

static IMMDevice *default_device(bool source) {
	if (!enumerator && FAILED(CoCreateInstance(&clsid_enumerator, NULL,
			CLSCTX_ALL, &iid_enumerator, (void **)&enumerator))) {
		enumerator = NULL;
		return NULL;
	}
	IMMDevice *device = NULL;
	if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator,
			source ? eCapture : eRender, eMultimedia, &device))) {
		return NULL;
	}
	return device;
}

static IAudioEndpointVolume *endpoint_volume(IMMDevice *device) {
	IAudioEndpointVolume *volume = NULL;
	if (FAILED(IMMDevice_Activate(device, &iid_endpoint_volume, CLSCTX_ALL, NULL,
			(void **)&volume))) {
		return NULL;
	}
	return volume;
}

// Waybar's port names for Windows' endpoint form factors.
static const char *form_factor_port(UINT form) {
	switch (form) {
	case 1: return "speaker";      // Speakers
	case 3: return "headphone";    // Headphones
	case 5: return "headset";      // Headset
	case 6: return "phone";        // Handset
	case 8: return "hifi";         // SPDIF
	case 9: return "hdmi";         // DigitalAudioDisplayDevice
	default: return "";
	}
}

// Volume, mute, name and kind of one endpoint.
static bool read_device(bool source, int *volume_out, bool *muted_out,
		char *desc, size_t desc_size, char *port, size_t port_size,
		bool *bluetooth) {
	IMMDevice *device = default_device(source);
	if (!device) {
		return false;
	}
	bool ok = false;
	IAudioEndpointVolume *volume = endpoint_volume(device);
	if (volume) {
		float level = 0;
		BOOL muted = FALSE;
		if (SUCCEEDED(IAudioEndpointVolume_GetMasterVolumeLevelScalar(volume,
				&level)) &&
				SUCCEEDED(IAudioEndpointVolume_GetMute(volume, &muted))) {
			*volume_out = (int)(level * 100.0f + 0.5f);
			*muted_out = muted != FALSE;
			ok = true;
		}
		IAudioEndpointVolume_Release(volume);
	}
	IPropertyStore *props = NULL;
	if (ok && SUCCEEDED(IMMDevice_OpenPropertyStore(device, STGM_READ, &props))) {
		PROPVARIANT v;
		PropVariantInit(&v);
		if (SUCCEEDED(IPropertyStore_GetValue(props, &key_friendly_name, &v)) &&
				v.vt == VT_LPWSTR && v.pwszVal) {
			char *name = utf16_to_utf8(v.pwszVal);
			snprintf(desc, desc_size, "%s", name ? name : "");
			free(name);
		}
		PropVariantClear(&v);
		if (port && SUCCEEDED(IPropertyStore_GetValue(props, &key_form_factor, &v)) &&
				v.vt == VT_UI4) {
			snprintf(port, port_size, "%s", form_factor_port(v.ulVal));
		}
		PropVariantClear(&v);
		if (bluetooth && SUCCEEDED(IPropertyStore_GetValue(props, &key_enumerator,
				&v)) && v.vt == VT_LPWSTR && v.pwszVal) {
			// BTHENUM (A2DP) or BTHHFENUM (hands-free)
			*bluetooth = wcsncmp(v.pwszVal, L"BTH", 3) == 0;
			if (port && wcscmp(v.pwszVal, L"BTHHFENUM") == 0) {
				snprintf(port, port_size, "hands-free");
			}
		}
		PropVariantClear(&v);
		IPropertyStore_Release(props);
	}
	IMMDevice_Release(device);
	return ok;
}

bool sysinfo_audio(struct sysinfo_audio *out) {
	memset(out, 0, sizeof(*out));
	out->present = read_device(false, &out->volume, &out->muted, out->desc,
		sizeof(out->desc), out->port, sizeof(out->port), &out->bluetooth);
	out->source_present = read_device(true, &out->source_volume,
		&out->source_muted, out->source_desc, sizeof(out->source_desc), NULL, 0,
		NULL);
	return out->present;
}

void sysinfo_audio_set_volume(bool source, double percent) {
	IMMDevice *device = default_device(source);
	if (!device) {
		return;
	}
	IAudioEndpointVolume *volume = endpoint_volume(device);
	if (volume) {
		float level = (float)(percent < 0 ? 0 : percent > 100 ? 100 : percent) / 100.0f;
		IAudioEndpointVolume_SetMasterVolumeLevelScalar(volume, level, NULL);
		IAudioEndpointVolume_Release(volume);
	}
	IMMDevice_Release(device);
}

void sysinfo_audio_set_mute(bool source, int mode) {
	IMMDevice *device = default_device(source);
	if (!device) {
		return;
	}
	IAudioEndpointVolume *volume = endpoint_volume(device);
	if (volume) {
		BOOL muted = FALSE;
		IAudioEndpointVolume_GetMute(volume, &muted);
		BOOL want = mode < 0 ? !muted : mode == 1;
		IAudioEndpointVolume_SetMute(volume, want, NULL);
		IAudioEndpointVolume_Release(volume);
	}
	IMMDevice_Release(device);
}

void win32_audio_finish(void) {
	if (enumerator) {
		IMMDeviceEnumerator_Release(enumerator);
		enumerator = NULL;
	}
}
