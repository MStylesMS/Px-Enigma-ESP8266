// scan_io_esp.cpp — ESP8266 GPIO driver for switch_matrix::ScanIO.
//
// Active-LOW columns + INPUT_PULLUP rows. The matrix is wired so that
// closing a switch shorts a row to its column; while the column is
// driven LOW the corresponding row reads LOW (== closed). This file
// inverts that signal so callers see "1 == closed" — matching the
// natural bit semantics of `SwitchMatrix::state()`.
//
// Excluded from the native (host) build via the ARDUINO guard so
// `pio test -e native` does not need an Arduino runtime.
#ifdef ARDUINO

#include "switch_matrix.h"
#include "config.h"   // pins::COL[], pins::ROW[]
#include <Arduino.h>

namespace switch_matrix {

class Esp8266ScanIO : public ScanIO {
public:
    void configure() override {
        // Drive every column HIGH; the active column is pulled LOW
        // immediately before the row read in drive_and_read().
        for (uint8_t c = 0; c < NUM_COLS; ++c) {
            pinMode(pins::COL[c], OUTPUT);
            digitalWrite(pins::COL[c], HIGH);
        }
        for (uint8_t r = 0; r < NUM_ROWS; ++r) {
            // GPIO16 has no internal pull-up — the wired matrix relies on
            // the on-board pull-up resistor for that line. INPUT_PULLUP is
            // a no-op there but harmless.
            pinMode(pins::ROW[r], INPUT_PULLUP);
        }
    }

    uint8_t drive_and_read(uint8_t col) override {
        if (col >= NUM_COLS) return 0;

        // Drive the requested column LOW, every other column HIGH.
        for (uint8_t c = 0; c < NUM_COLS; ++c) {
            digitalWrite(pins::COL[c], (c == col) ? LOW : HIGH);
        }
        // Tiny settle — pull-up + parasitic capacitance recovers in ~µs.
        // A handful of NOPs is enough; we deliberately avoid delay()/yield()
        // because the cooperative loop already paces successive ticks.
        for (volatile uint8_t i = 0; i < 8; ++i) { /* settle */ }

        uint8_t mask = 0;
        for (uint8_t r = 0; r < NUM_ROWS; ++r) {
            // Active-LOW: pin reads LOW when switch is closed. Invert.
            if (digitalRead(pins::ROW[r]) == LOW) mask |= (uint8_t)(1u << r);
        }
        return mask;
    }

    void idle() override {
        for (uint8_t c = 0; c < NUM_COLS; ++c) {
            digitalWrite(pins::COL[c], HIGH);
        }
    }
};

// Singleton — all callers share one driver instance.
static Esp8266ScanIO s_io;

ScanIO& esp_scan_io() { return s_io; }

} // namespace switch_matrix

#endif // ARDUINO
