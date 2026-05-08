// ota_mgr.cpp — OTA firmware update for px-enigma-esp8266.
#include "ota_mgr.h"
#include "log.h"
#include "wifi_mgr.h"

#include <ArduinoOTA.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266mDNS.h>

namespace ota_mgr {

static const char* TAG = "ota";

static ESP8266HTTPUpdateServer s_http_updater(true /* serial debug */);
static bool                    s_arduino_ota_started = false;

void mount_http_update(ESP8266WebServer& server, const cfg::Config& c) {
    // Auth: username "admin", password = ap_password. If ap_password is empty
    // the updater still works but is effectively unauthenticated; the spec
    // requires AP password to be configured, so this should be a non-issue.
    s_http_updater.setup(&server, "/update", "admin", c.ap_password.c_str());
    pxlog::info(TAG, "HTTP OTA mounted at /update (user=admin)");
}

void begin_arduino_ota(const cfg::Config& c) {
    String host = wifi_mgr::network_name_from_prop(c.prop_name);
    ArduinoOTA.setHostname(host.c_str());
    if (c.ap_password.length()) {
        ArduinoOTA.setPassword(c.ap_password.c_str());
    }
    ArduinoOTA.onStart([]() { pxlog::warn(TAG, "ArduinoOTA start"); });
    ArduinoOTA.onEnd  ([]() { pxlog::warn(TAG, "ArduinoOTA end");   });
    ArduinoOTA.onError([](ota_error_t e) {
        pxlog::err(TAG, "ArduinoOTA error %u", (unsigned)e);
    });
    ArduinoOTA.begin();
    s_arduino_ota_started = true;
    pxlog::info(TAG, "ArduinoOTA up: %s.local", host.c_str());

    if (MDNS.begin(host.c_str())) {
        MDNS.addService("http", "tcp", 80);
        pxlog::info(TAG, "mDNS started: %s.local", host.c_str());
    } else {
        pxlog::warn(TAG, "mDNS failed to start");
    }
}

void loop() {
    if (s_arduino_ota_started) ArduinoOTA.handle();
    MDNS.update();
}

} // namespace ota_mgr
