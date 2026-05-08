// config.h — compile-time hardware constants and runtime Config for px-enigma-esp8266.
//
// Pin assignments reflect the existing wired units and must not change
// without a hardware revision. See docs/pin-mapping.md for the full rationale
// and the hardware-rework note regarding GPIO1 / GPIO3.
//
// The cfg::Config struct mirrors docs/functional-spec.md §14.2.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// Pin assignments
// ---------------------------------------------------------------------------

namespace pins {

    // I2C bus — GPIO0 (SDA) and GPIO2 (SCL).
    // Both lines require 4.7 kΩ pull-ups and must be HIGH at reset.
    constexpr uint8_t I2C_SDA = 0;   // GPIO0 / D3
    constexpr uint8_t I2C_SCL = 2;   // GPIO2 / D4

    // Switch-matrix outputs (driven LOW one at a time during a scan column).
    // NOTE: COL[1] = GPIO1 (TX). Driving this pin as a column output disables
    //       the UART console at runtime. This is an accepted hardware bug.
    constexpr uint8_t NUM_COLS     = 4;
    constexpr uint8_t COL[NUM_COLS] = {15, 1, 5, 16};
    //                                  col0 col1 col2 col3
    //  GPIO15 (D8) — boot-strap: must be LOW at reset (on-board pull-down).
    //  GPIO1  (TX) — UART TX; serial console disabled once scanning starts.
    //  GPIO5  (D1)
    //  GPIO16 (D0) — no internal pull-up; also the deep-sleep wake pin.

    // Switch-matrix inputs (read while the corresponding column is LOW).
    // NOTE: ROW[1] = GPIO3 (RX). Reused as a row input; no serial receive.
    constexpr uint8_t NUM_ROWS     = 5;
    constexpr uint8_t ROW[NUM_ROWS] = {12, 3, 14, 4, 13};
    //                                  row0 row1 row2 row3 row4
    //  GPIO12 (D6), GPIO3 (RX), GPIO14 (D5), GPIO4 (D2), GPIO13 (D7)

    // Battery voltage sense — 10-bit ADC (0..1023 raw, scaled externally
    // to the 0..1 V input range via on-board resistor divider).
    constexpr uint8_t BATTERY_ADC = A0;

} // namespace pins

// ---------------------------------------------------------------------------
// I2C device addresses
// ---------------------------------------------------------------------------

namespace i2c_addr {
    constexpr uint8_t DISPLAY_LOW  = 0x70;  // HT16K33 #1 — right half (low digits)
    constexpr uint8_t DISPLAY_HIGH = 0x71;  // HT16K33 #2 — left half (high digits)
} // namespace i2c_addr

// ---------------------------------------------------------------------------
// Bit layout: column × row → bit index (row-major, 0-based)
//   bit = col_index * NUM_ROWS + row_index
//   Switch number = bit + 1  (switch 1 = top-left = col0 × row0)
// ---------------------------------------------------------------------------

// Total matrix cells
constexpr uint8_t MATRIX_NUM_CELLS = pins::NUM_COLS * pins::NUM_ROWS;  // 20

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

constexpr uint8_t  DISPLAY_BRIGHTNESS_DEFAULT  = 1;    // 0..15, HT16K33 native
constexpr uint16_t DISPLAY_BLINK_HALF_PERIOD_MS = 500; // 1 Hz = 500 ms on, 500 ms off
constexpr uint32_t IDENTIFY_DURATION_MS         = 2000;

// ---------------------------------------------------------------------------
// Networking / MQTT defaults
// ---------------------------------------------------------------------------

constexpr uint32_t HEARTBEAT_INTERVAL_MS = 10000;

// ---------------------------------------------------------------------------
// Log ring buffer
// ---------------------------------------------------------------------------

constexpr size_t LOG_RING_LINES = 32;
constexpr size_t LOG_LINE_MAX   = 160;

// ---------------------------------------------------------------------------
// Runtime configuration (schema: docs/functional-spec.md §14.2)
// ---------------------------------------------------------------------------

namespace cfg {

// Number of signal-indicator RSSI threshold values.
static constexpr size_t RSSI_THRESHOLDS = 7;

// battery.profile string constants
static constexpr char BATT_PROFILE_EXTERNAL[]    = "external";
static constexpr char BATT_PROFILE_UNKNOWN[]     = "unknown";
static constexpr char BATT_PROFILE_12V_LEAD[]    = "12v-lead-acid";
static constexpr char BATT_PROFILE_12V_LIFEPO4[] = "12v-LiFePO4";
static constexpr char BATT_PROFILE_6V_LEAD[]     = "6v-lead-acid";
static constexpr char BATT_PROFILE_6V_LIFEPO4[]  = "6v-LiFePO4";
static constexpr char BATT_PROFILE_CUSTOM[]      = "custom";

// puzzle.mode string constants
static constexpr char PUZZLE_MODE_LIVE[]     = "live";
static constexpr char PUZZLE_MODE_LATCHING[] = "latching";

// puzzle.start_state string constants
static constexpr char START_STATE_ACTIVE[] = "active";
static constexpr char START_STATE_OFF[]    = "off";

struct WifiCreds {
    String ssid;
    String password;
};

struct Config {
    // device
    String prop_name;
    String instance;

    // wifi
    WifiCreds wifi_primary;
    WifiCreds wifi_backup;
    String    ap_password;

    // mqtt
    String   mqtt_host;
    uint16_t mqtt_port;
    String   mqtt_username;
    String   mqtt_password;
    String   mqtt_base_topic;
    String   mqtt_announce_topic;
    uint32_t mqtt_heartbeat_interval_ms;

    // puzzle
    String   puzzle_mode;                 // PUZZLE_MODE_LIVE | PUZZLE_MODE_LATCHING
    String   puzzle_target;               // "" when puzzle_has_target == false
    bool     puzzle_has_target;           // true ↔ JSON target was non-null
    uint32_t puzzle_identify_duration_ms;
    String   puzzle_start_state;          // START_STATE_ACTIVE | START_STATE_OFF

    // display
    uint8_t display_brightness;           // 0..15 (HT16K33 native)

    // signal_indicator
    bool   signal_indicator_enabled;
    int8_t signal_rssi_dbm[RSSI_THRESHOLDS];  // dBm thresholds (decreasing)

    // battery
    String   battery_profile;             // see BATT_PROFILE_* constants
    String   battery_points;             // "" = null; else CSV string or JSON array as string
    uint8_t  battery_low_percent;
    uint8_t  battery_cutoff_percent;
    uint8_t  battery_hysteresis_pct;
    uint32_t battery_sample_interval_ms;
    uint16_t battery_inactivity_minutes;
    uint16_t battery_adc_at_0v_raw;
    uint16_t battery_adc_at_full_v_raw;
    float    battery_adc_full_v;

    // scan
    uint16_t scan_poll_interval_ms;
    uint8_t  scan_debounce_samples;
};

// ---------------------------------------------------------------------------
// Platform-independent (implemented in config_json.cpp)
// ---------------------------------------------------------------------------

// Fill c with the baked-in spec defaults. Call before from_json() for a clean load.
void load_defaults(Config& c);

// Merge doc into c (overlay semantics — missing keys keep their current value).
// Returns false on any field validation failure; populates *err_out with the reason.
bool from_json(Config& c, const JsonDocument& in, String* err_out = nullptr);

// Serialize c to out (clears out first).
void to_json(const Config& c, JsonDocument& out);

// ---------------------------------------------------------------------------
// Platform-dependent (implemented in config.cpp; use LittleFS + WiFi)
// ---------------------------------------------------------------------------

// Last 4 hex chars of the WiFi MAC address (e.g., "A1B2"). Needs WiFi stack up.
String mac_suffix();

// Returns the factory-default prop_name for this device, derived from the
// MAC suffix so each unit is unique out-of-the-box: e.g. "px-enigma-A1B2".
// The user may override via the Web UI at any time.
String default_prop_name();

// Mount LittleFS + load /config.json into c. On parse/schema failure: renames the
// bad file to /config.bad.json, resets to defaults, sets was_invalid=true.
// Always returns true — firmware is always usable after this call.
bool load(Config& c, bool& was_invalid);

// Persist c to /config.json. Returns true on success.
bool save(const Config& c);

// Remove /config.json and /config.bad.json.
bool wipe();

// Return true if GPIO0 is held LOW for hold_ms at boot (factory-reset trigger).
bool factory_reset_requested(uint32_t hold_ms = 3000);

} // namespace cfg
