// wifi_mgr.cpp — AP+STA always-on WiFi manager for px-enigma-esp8266.
//
// Cooperative loop: loop() is called every iteration but internally
// rate-limits work to once per second. No delay() calls.
#include "wifi_mgr.h"
#include "log.h"

#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>

namespace wifi_mgr {

static const char* TAG = "wifi";

static ESP8266WiFiMulti s_multi;
static String           s_ap_ssid;
static String           s_mdns_host;
static uint32_t         s_last_check    = 0;
static bool             s_was_connected = false;
static int              s_last_ap_clients = -1;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

String network_name_from_prop(const String& prop_name) {
    String name = prop_name;
    name.trim();
    name.toLowerCase();
    name.replace(" ", "-");
    // Remove any character that is not alphanumeric or hyphen.
    String clean;
    clean.reserve(name.length());
    for (size_t i = 0; i < name.length(); ++i) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
            clean += c;
        }
    }
    if (clean.length() == 0) clean = "px-enigma";
    if (clean.length() > 31) clean = clean.substring(0, 31);
    return clean;
}

// ---------------------------------------------------------------------------
// begin
// ---------------------------------------------------------------------------

void begin(const cfg::Config& c) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(true);

    // --- STA ---
    if (c.wifi_primary.ssid.length() > 0) {
        s_multi.addAP(c.wifi_primary.ssid.c_str(),
                      c.wifi_primary.password.length()
                          ? c.wifi_primary.password.c_str()
                          : nullptr);
        pxlog::info(TAG, "STA primary: %s", c.wifi_primary.ssid.c_str());
    }
    if (c.wifi_backup.ssid.length() > 0) {
        s_multi.addAP(c.wifi_backup.ssid.c_str(),
                      c.wifi_backup.password.length()
                          ? c.wifi_backup.password.c_str()
                          : nullptr);
        pxlog::info(TAG, "STA backup:  %s", c.wifi_backup.ssid.c_str());
    }

    // --- AP ---
    s_ap_ssid = String("Px-Enigma-") + cfg::mac_suffix();
    s_mdns_host = network_name_from_prop(c.prop_name);
    IPAddress ip(192, 168, 4, 1), gw(192, 168, 4, 1), nm(255, 255, 255, 0);
    WiFi.softAPConfig(ip, gw, nm);
    bool ok = WiFi.softAP(s_ap_ssid.c_str(),
                          c.ap_password.length()
                              ? c.ap_password.c_str()
                              : nullptr);
    pxlog::info(TAG, "AP %s ssid=%s pwd=%s",
                ok ? "up" : "FAILED",
                s_ap_ssid.c_str(),
                c.ap_password.length() ? "***" : "(open)");
    pxlog::info(TAG, "AP IP: %s", WiFi.softAPIP().toString().c_str());
}

// ---------------------------------------------------------------------------
// loop — non-blocking; runs checks at most once per second
// ---------------------------------------------------------------------------

void loop() {
    uint32_t now = millis();
    if (now - s_last_check < 1000) return;
    s_last_check = now;

    s_multi.run();

    bool conn = (WiFi.status() == WL_CONNECTED);
    if (conn != s_was_connected) {
        s_was_connected = conn;
        if (conn) {
            pxlog::info(TAG, "STA connected: ssid=%s ip=%s rssi=%d",
                        WiFi.SSID().c_str(),
                        WiFi.localIP().toString().c_str(),
                        WiFi.RSSI());
        } else {
            pxlog::warn(TAG, "STA disconnected");
        }
    }

    int clients = WiFi.softAPgetStationNum();
    if (s_last_ap_clients < 0) {
        // First tick: log initial count without treating it as a change.
        s_last_ap_clients = clients;
        pxlog::info(TAG, "AP ready; clients=%d", clients);
    } else if (clients != s_last_ap_clients) {
        pxlog::info(TAG, "AP client %s: now %d client(s)",
                    clients > s_last_ap_clients ? "connected" : "disconnected",
                    clients);
        s_last_ap_clients = clients;
    }
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

bool   sta_connected() { return WiFi.status() == WL_CONNECTED; }
String sta_ip()        { return WiFi.localIP().toString(); }
String sta_ssid()      { return WiFi.SSID(); }
int    sta_rssi()      { return WiFi.RSSI(); }

String ap_ip()         { return WiFi.softAPIP().toString(); }
String ap_ssid()       { return s_ap_ssid; }
int    ap_clients()    { return WiFi.softAPgetStationNum(); }

String mac_address()   { return WiFi.macAddress(); }
String mdns_hostname() { return s_mdns_host.length() ? s_mdns_host : String("px-enigma"); }
String mdns_fqdn()     { return mdns_hostname() + ".local"; }

} // namespace wifi_mgr
