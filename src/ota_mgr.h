// ota_mgr.h — OTA firmware update for px-enigma-esp8266.
//
// Two complementary update paths:
//   1. HTTP OTA at /update on the shared web server (POST multipart upload),
//      protected by username "admin" + ap_password.
//   2. ArduinoOTA over UDP+TCP on port 8266, advertised as <hostname>.local
//      via mDNS, password = ap_password (only set if non-empty).
//
// Hostname is derived from device.prop_name via wifi_mgr::network_name_from_prop.
#pragma once

#include "config.h"
#include <ESP8266WebServer.h>

namespace ota_mgr {

// Mount the HTTP update server on the existing web_ui server BEFORE
// server.begin() is called.
void mount_http_update(ESP8266WebServer& server, const cfg::Config& c);

// Start ArduinoOTA + mDNS. Call once in setup() AFTER wifi_mgr::begin().
void begin_arduino_ota(const cfg::Config& c);

// Cooperative tick — call every loop iteration.
void loop();

} // namespace ota_mgr
