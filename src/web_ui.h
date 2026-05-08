// web_ui.h — HTTP server + single-page Web UI for px-enigma-esp8266.
//
// Serves port 80 on both AP and STA interfaces. No authentication in v0.2.x.
// Endpoints match docs/functional-spec.md §13.3.
#pragma once

#include "config.h"
#include <Arduino.h>

namespace web_ui {

// Call once in setup() after cfg::load() and wifi_mgr::begin().
// c must remain valid for the lifetime of the server.
void begin(cfg::Config* c);

// Call every loop iteration (delegates to server.handleClient()).
void loop();

// True once a reboot has been scheduled by /api/config save or /api/restart.
bool     reboot_pending();
uint32_t reboot_at_ms();

} // namespace web_ui
