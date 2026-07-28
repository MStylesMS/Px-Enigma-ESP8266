// state.h — Canonical state/announce snapshot builders for px-enigma-esp8266.
//
// These are shared between the HTTP /api/state endpoint and the MQTT
// state/announce publishers, so the two surfaces never drift.
#pragma once

#include "config.h"
#include "code_engine.h"
#include <Arduino.h>
#include <ArduinoJson.h>

namespace appstate {

// Maximum UI grid dimensions supported by the grid layout table.
static constexpr uint8_t MAX_GRID_ROWS = 8;
static constexpr uint8_t MAX_GRID_COLS = 8;

// Populate `out` with the canonical /state snapshot per spec §12.1.
void build_state(const cfg::Config& c, JsonDocument& out);

// Populate `out` with the announce envelope per spec §12.4.
void build_announce(const cfg::Config& c, JsonDocument& out);

// Add the physical switch-layout representation for a raw matrix state.
void add_code_grid(JsonObject code_obj, uint32_t code_bits);

// Reset the rolling minimum-free-heap watermark.
void reset_heap_watermark();

// MQTT module reports its connection status here so build_state() can
// surface it without creating a hard dependency on mqtt_mgr.
void set_mqtt_connected(bool v);
bool mqtt_connected();

// Code engine reports its live state here so build_state() stays DRY.
void set_code_state(const code_engine::CodeState* cs);

// Read the latest code snapshot if available.
bool get_code_snapshot(uint32_t* code_bits, const char** code_str);

// Called once at startup (after switch_layout.json is loaded) to register the
// UI-grid-position → code_bits-bit lookup used to generate code.grid in state.
// grid_bit[r][c] is the bit index into code_bits for that cell, or -1 if the
// cell is inactive (swNum > switch_count).
void set_grid_layout(uint8_t ui_rows, uint8_t ui_cols, uint8_t switch_count,
                     const int8_t grid_bit[MAX_GRID_ROWS][MAX_GRID_COLS]);

} // namespace appstate
