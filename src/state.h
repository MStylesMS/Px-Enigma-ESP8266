// state.h — Canonical state/announce snapshot builders for px-enigma-esp8266.
//
// These are shared between the HTTP /api/state endpoint and the MQTT
// state/announce publishers, so the two surfaces never drift.
#pragma once

#include "config.h"
#include <Arduino.h>
#include <ArduinoJson.h>

namespace appstate {

// Populate `out` with the canonical /state snapshot per spec §12.1.
// Modules not yet implemented (code engine, battery, MQTT) emit nulls or
// best-effort placeholders.
void build_state(const cfg::Config& c, JsonDocument& out);

// Populate `out` with the announce envelope per spec §12.4.
void build_announce(const cfg::Config& c, JsonDocument& out);

// Reset the rolling minimum-free-heap watermark.
void reset_heap_watermark();

// MQTT module reports its connection status here so build_state() can
// surface it without creating a hard dependency on mqtt_mgr.
void set_mqtt_connected(bool v);

} // namespace appstate
