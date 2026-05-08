// display_mgr.h — HT16K33 display driver and state-machine renderer.
//
// Phase 9: I2C init, legacy digit-position layout, signal indicator,
// identify flash, OFF blanking, LATCHED blink.
//
// The "display state" is derived on every tick() from the rest of the
// system rather than being held as a separate FSM copy.
//
// Call order in setup():
//   1. display_mgr::begin(&g_config)   ← BEFORE wifi/MQTT
//   2. display_mgr::show_boot_code(code_str)  ← first lit code
//
// Call every loop():
//   display_mgr::tick(engine_state, identify_active, is_off,
//                     mqtt_connected, sta_rssi_dbm)
#pragma once

#include "config.h"

namespace display_mgr {

// Initialise the I2C bus (Wire.begin), probe both HT16K33 displays,
// apply configured brightness. Must be called before any show_* or tick().
// Returns true if both displays ACKed on I2C; false if either is missing
// (logged as a warning; rendering continues on whichever display is present).
bool begin(const cfg::Config& c);

// Show an initial code immediately (used just after begin() in setup()).
// code_str: 9-byte "XX-YY-ZZ\0" string from code_engine.
void show_boot_code(const char* code_str);

// tick() with signal indicator enabled state and RSSI thresholds.
// Call this form when you have the config available in the loop.
//   si_enabled      : from c.signal_indicator_enabled
//   rssi_thresholds : from c.signal_rssi_dbm
//   low_batt        : battery_monitor::status() == Status::Low
//   crit_batt       : battery_monitor::status() == Status::Critical
void tick(const char* code_str, bool latched, bool identify,
          bool is_off, bool mqtt_connected, int rssi_dbm,
          bool si_enabled,
          const int8_t rssi_thresholds[cfg::RSSI_THRESHOLDS],
          bool low_batt  = false,
          bool crit_batt = false);

// Update brightness immediately (called from setBrightness command).
void set_brightness(uint8_t b);

// Boot-strap GPIO sanity check. Logs a warning if GPIO0, GPIO2, or GPIO15
// is in an unexpected logic state at end of setup(). Returns true = all OK.
bool sanity_check_boot_pins();

} // namespace display_mgr
