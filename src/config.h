// config.h — compile-time hardware constants for px-enigma-esp8266.
//
// Pin assignments reflect the existing wired units and must not change
// without a hardware revision. See docs/pin-mapping.md for the full rationale
// and the hardware-rework note regarding GPIO1 / GPIO3.
//
// The runtime configuration struct (cfg::Config) is added in Phase 1.
#pragma once

#include <Arduino.h>

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
