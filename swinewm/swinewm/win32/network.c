// Readings for the bar's network module (sysinfo_network): the interface
// the default route uses (or the configured one) from IP Helper, its byte
// counters, and for Wi-Fi the network, signal and frequency from the WLAN
// API. Since Windows 11 24H2 the WLAN API names the network (SSID, BSSID)
// only when location access is allowed for desktop apps; without it the
// interface is still reported as Wi-Fi.
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <wlanapi.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/sysinfo.h"
#include "win32.h"

static HANDLE wlan = NULL;
static bool wlan_failed = false;

static void ipv4_string(const struct sockaddr_in *sa, char *out, size_t size) {
	const unsigned char *b = (const unsigned char *)&sa->sin_addr;
	snprintf(out, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

// RFC 5952: lower case, the longest run of zero groups as "::".
static void ipv6_bytes_string(const unsigned char *b, char *out, size_t size) {
	unsigned groups[8];
	for (int i = 0; i < 8; ++i) {
		groups[i] = (unsigned)(b[2 * i] << 8) | b[2 * i + 1];
	}
	int best = -1, best_len = 0;
	for (int i = 0; i < 8;) {
		if (groups[i] != 0) {
			i++;
			continue;
		}
		int j = i;
		while (j < 8 && groups[j] == 0) {
			j++;
		}
		if (j - i > best_len && j - i > 1) {
			best = i;
			best_len = j - i;
		}
		i = j;
	}
	char *o = out;
	char *end = out + size;
	*o = '\0';
	for (int i = 0; i < 8 && o < end; ++i) {
		if (i == best) {
			o += snprintf(o, (size_t)(end - o), "::");
			i += best_len - 1;
			continue;
		}
		if (i > 0 && i != best + best_len) {
			o += snprintf(o, (size_t)(end - o), ":");
		}
		o += snprintf(o, (size_t)(end - o), "%x", groups[i]);
	}
}

static void sockaddr_string(const SOCKADDR *sa, char *out, size_t size) {
	if (sa->sa_family == AF_INET) {
		ipv4_string((const struct sockaddr_in *)sa, out, size);
	} else if (sa->sa_family == AF_INET6) {
		const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
		ipv6_bytes_string(in6->sin6_addr.s6_addr, out, size);
	}
}

static void ipv4_mask(int prefix, char *out, size_t size) {
	unsigned long mask = prefix <= 0 ? 0 : 0xFFFFFFFFul << (32 - prefix);
	snprintf(out, size, "%lu.%lu.%lu.%lu", (mask >> 24) & 0xFF,
		(mask >> 16) & 0xFF, (mask >> 8) & 0xFF, mask & 0xFF);
}

static void ipv6_mask(int prefix, char *out, size_t size) {
	unsigned char b[16] = {0};
	for (int i = 0; i < 16; ++i) {
		int bits = prefix - i * 8;
		b[i] = (unsigned char)(bits >= 8 ? 0xFF : bits <= 0 ? 0 :
			(0xFF << (8 - bits)) & 0xFF);
	}
	ipv6_bytes_string(b, out, size);
}

static IP_ADAPTER_ADDRESSES *get_adapters(void) {
	ULONG size = 16384;
	for (int attempt = 0; attempt < 3; ++attempt) {
		IP_ADAPTER_ADDRESSES *list = malloc(size);
		ULONG result = GetAdaptersAddresses(AF_UNSPEC,
			GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
			GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, list, &size);
		if (result == ERROR_SUCCESS) {
			return list;
		}
		free(list);
		if (result != ERROR_BUFFER_OVERFLOW) {
			return NULL;
		}
	}
	return NULL;
}

// The interface Windows would send to the internet through: no packet is
// sent, it only asks the routing table.
static DWORD default_route_interface(void) {
	struct sockaddr_in target = {0};
	target.sin_family = AF_INET;
	target.sin_addr.s_addr = htonl(0x08080808);   // 8.8.8.8
	DWORD index = 0;
	if (GetBestInterfaceEx((struct sockaddr *)&target, &index) == NO_ERROR) {
		return index;
	}
	struct sockaddr_in6 target6 = {0};
	target6.sin6_family = AF_INET6;
	target6.sin6_addr.s6_addr[0] = 0x20;   // 2000::/3, any global address
	target6.sin6_addr.s6_addr[15] = 1;
	if (GetBestInterfaceEx((struct sockaddr *)&target6, &index) == NO_ERROR) {
		return index;
	}
	return 0;
}

static bool wlan_open(void) {
	if (wlan || wlan_failed) {
		return wlan != NULL;
	}
	DWORD version;
	if (WlanOpenHandle(2, NULL, &version, &wlan) != ERROR_SUCCESS) {
		wlan = NULL;
		wlan_failed = true;   // no WLAN service (desktops without Wi-Fi)
	}
	return wlan != NULL;
}

static bool same_guid_string(const GUID *guid, const char *adapter_name) {
	wchar_t text[64];
	if (!StringFromGUID2(guid, text, 64)) {
		return false;
	}
	char *utf8 = utf16_to_utf8(text);
	bool same = utf8 && swine_strcasecmp(utf8, adapter_name) == 0;
	free(utf8);
	return same;
}

static bool radio_off(const GUID *guid) {
	DWORD size = 0;
	WLAN_RADIO_STATE *radio = NULL;
	bool off = false;
	if (WlanQueryInterface(wlan, guid, wlan_intf_opcode_radio_state, NULL, &size,
			(PVOID *)&radio, NULL) == ERROR_SUCCESS && radio) {
		for (DWORD i = 0; i < radio->dwNumberOfPhys; ++i) {
			if (radio->PhyRadioState[i].dot11SoftwareRadioState == dot11_radio_state_off ||
					radio->PhyRadioState[i].dot11HardwareRadioState == dot11_radio_state_off) {
				off = true;
			}
		}
		WlanFreeMemory(radio);
	}
	return off;
}

// The SSID, BSSID, signal and frequency of the network the interface is
// connected to.
static void wlan_details(const GUID *guid, struct sysinfo_network *out) {
	DWORD size = 0;
	WLAN_CONNECTION_ATTRIBUTES *conn = NULL;
	if (WlanQueryInterface(wlan, guid, wlan_intf_opcode_current_connection, NULL,
			&size, (PVOID *)&conn, NULL) != ERROR_SUCCESS || !conn) {
		return;
	}
	if (conn->isState == wlan_interface_state_connected) {
		DOT11_SSID *ssid = &conn->wlanAssociationAttributes.dot11Ssid;
		size_t len = ssid->uSSIDLength < sizeof(out->essid) - 1 ?
			ssid->uSSIDLength : sizeof(out->essid) - 1;
		memcpy(out->essid, ssid->ucSSID, len);
		out->essid[len] = '\0';
		const UCHAR *b = conn->wlanAssociationAttributes.dot11Bssid;
		snprintf(out->bssid, sizeof(out->bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
			b[0], b[1], b[2], b[3], b[4], b[5]);
		ULONG quality = conn->wlanAssociationAttributes.wlanSignalQuality;
		out->signal_strength = (int)quality;
		out->signal_dbm = (int)quality / 2 - 100;   // until the BSS list says
		out->rx_bitrate = conn->wlanAssociationAttributes.ulRxRate * 1000.0;
		out->tx_bitrate = conn->wlanAssociationAttributes.ulTxRate * 1000.0;
		// The access point's entry: its real RSSI and frequency.
		WLAN_BSS_LIST *bss = NULL;
		if (WlanGetNetworkBssList(wlan, guid, ssid,
				conn->wlanAssociationAttributes.dot11BssType, FALSE, NULL,
				&bss) == ERROR_SUCCESS && bss) {
			for (DWORD i = 0; i < bss->dwNumberOfItems; ++i) {
				WLAN_BSS_ENTRY *e = &bss->wlanBssEntries[i];
				if (memcmp(e->dot11Bssid, b, 6) == 0) {
					out->signal_dbm = (int)e->lRssi;
					out->frequency = e->ulChCenterFrequency / 1e6;   // kHz
					break;
				}
			}
			WlanFreeMemory(bss);
		}
	}
	WlanFreeMemory(conn);
}

static void fill_wireless(const IP_ADAPTER_ADDRESSES *a,
		struct sysinfo_network *out) {
	if (!wlan_open()) {
		return;
	}
	WLAN_INTERFACE_INFO_LIST *list = NULL;
	if (WlanEnumInterfaces(wlan, NULL, &list) != ERROR_SUCCESS || !list) {
		return;
	}
	for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
		const GUID *guid = &list->InterfaceInfo[i].InterfaceGuid;
		if (!same_guid_string(guid, a->AdapterName)) {
			continue;
		}
		out->disabled = radio_off(guid);
		if (out->connected) {
			wlan_details(guid, out);
		}
		break;
	}
	WlanFreeMemory(list);
}

static void fill_addresses(const IP_ADAPTER_ADDRESSES *a,
		struct sysinfo_network *out) {
	for (const IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u;
			u = u->Next) {
		const SOCKADDR *sa = u->Address.lpSockaddr;
		if (sa->sa_family == AF_INET && !out->ipaddr[0]) {
			sockaddr_string(sa, out->ipaddr, sizeof(out->ipaddr));
			out->cidr = u->OnLinkPrefixLength;
			ipv4_mask(out->cidr, out->netmask, sizeof(out->netmask));
		} else if (sa->sa_family == AF_INET6 && !out->ipaddr6[0]) {
			const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
			// Skip link-local fe80::/10, as Waybar takes the global one.
			if (in6->sin6_addr.s6_addr[0] == 0xfe &&
					(in6->sin6_addr.s6_addr[1] & 0xc0) == 0x80) {
				continue;
			}
			sockaddr_string(sa, out->ipaddr6, sizeof(out->ipaddr6));
			out->cidr6 = u->OnLinkPrefixLength;
			ipv6_mask(out->cidr6, out->netmask6, sizeof(out->netmask6));
		}
	}
	for (const IP_ADAPTER_GATEWAY_ADDRESS *g = a->FirstGatewayAddress; g;
			g = g->Next) {
		if (g->Address.lpSockaddr->sa_family == AF_INET || !out->gwaddr[0]) {
			sockaddr_string(g->Address.lpSockaddr, out->gwaddr, sizeof(out->gwaddr));
			if (g->Address.lpSockaddr->sa_family == AF_INET) {
				break;
			}
		}
	}
}

bool sysinfo_network(const char *pattern, struct sysinfo_network *out) {
	memset(out, 0, sizeof(*out));
	IP_ADAPTER_ADDRESSES *list = get_adapters();
	if (!list) {
		return false;
	}
	DWORD route = pattern ? 0 : default_route_interface();
	const IP_ADAPTER_ADDRESSES *chosen = NULL, *wireless = NULL;
	for (const IP_ADAPTER_ADDRESSES *a = list; a; a = a->Next) {
		if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
			continue;
		}
		if (a->IfType == IF_TYPE_IEEE80211 && !wireless) {
			wireless = a;
		}
		if (pattern) {
			char *name = utf16_to_utf8(a->FriendlyName);
			bool match = name && (glob_match(pattern, name) ||
				glob_match(pattern, a->AdapterName));
			free(name);
			if (match && !chosen) {
				chosen = a;
			}
		} else if (route && (a->IfIndex == route || a->Ipv6IfIndex == route)) {
			chosen = a;
		}
	}
	if (!chosen) {
		// No route: report a switched-off Wi-Fi radio as "disabled".
		if (wireless && !pattern) {
			char *name = utf16_to_utf8(wireless->FriendlyName);
			snprintf(out->ifname, sizeof(out->ifname), "%s", name ? name : "");
			free(name);
			out->wireless = true;
			fill_wireless(wireless, out);
		}
		free(list);
		return true;
	}
	out->found = true;
	char *name = utf16_to_utf8(chosen->FriendlyName);
	snprintf(out->ifname, sizeof(out->ifname), "%s", name ? name : "");
	free(name);
	out->connected = chosen->OperStatus == IfOperStatusUp;
	out->wireless = chosen->IfType == IF_TYPE_IEEE80211;
	out->link_speed = (double)chosen->TransmitLinkSpeed;
	if (out->link_speed > 1e12) {
		out->link_speed = 0;   // "unknown" is reported as all ones
	}
	fill_addresses(chosen, out);
	MIB_IF_ROW2 row = {0};
	row.InterfaceLuid = chosen->Luid;
	if (GetIfEntry2(&row) == NO_ERROR) {
		out->rx_bytes = (double)row.InOctets;
		out->tx_bytes = (double)row.OutOctets;
	}
	if (out->wireless) {
		fill_wireless(chosen, out);
	}
	free(list);
	return true;
}

void win32_network_finish(void) {
	if (wlan) {
		WlanCloseHandle(wlan, NULL);
		wlan = NULL;
	}
}
