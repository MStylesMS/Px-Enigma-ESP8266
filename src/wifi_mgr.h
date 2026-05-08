// wifi_mgr.h — AP+STA always-on WiFi manager for px-enigma-esp8266.
//
// Starts WIFI_AP_STA at boot. STA uses ESP8266WiFiMulti for primary + backup
// credentials. AP SSID is derived from device.prop_name (normalised to
// lowercase-hyphenated). Both interfaces remain up concurrently; STA
// reconnects non-blockingly in loop().
#pragma once

#include "config.h"
#include <Arduino.h>

namespace wifi_mgr {

// Normalise prop_name to a valid AP SSID / mDNS hostname:
//   trim → lowercase → spaces to hyphens → truncate to 31 chars.
// Falls back to "px-enigma" if the result is empty.
String network_name_from_prop(const String& prop_name);

// Call once in setup(), after cfg::load().
void begin(const cfg::Config& c);

// Call every loop iteration (non-blocking).
void loop();

// STA accessors (safe to call before STA is connected — return ""  / 0).
bool   sta_connected();
String sta_ip();
String sta_ssid();
int    sta_rssi();

// AP accessors (valid after begin()).
String ap_ip();
String ap_ssid();
int    ap_clients();

// Device identity helpers.
String mac_address();   // full MAC string  (e.g., "A1:B2:C3:D4:E5:F6")
String mdns_hostname(); // normalised hostname (no ".local")
String mdns_fqdn();     // hostname + ".local"

} // namespace wifi_mgr
