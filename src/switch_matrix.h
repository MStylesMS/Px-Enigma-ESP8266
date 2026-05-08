// switch_matrix.h — 4×5 switch-matrix scanner (host-testable pure logic).
//
// Design (functional-spec.md §4):
//   - 4 column outputs are driven LOW one at a time during a scan;
//   - while a column is LOW, the 5 row inputs are sampled (active-LOW
//     because rows have pull-ups);
//   - after `debounce_samples` consecutive identical samples, a cell
//     flips its debounced state;
//   - the public state is a single 20-bit integer with bit layout
//       bit = col_index * NUM_ROWS + row_index
//     matching the wiring documented in pin-mapping.md.
//
// The hardware-facing side of the scanner sits behind the `ScanIO`
// interface. The ESP build supplies a GPIO-backed implementation
// (scan_io_esp.cpp); host tests inject a fake to drive deterministic
// scenarios without an Arduino runtime.
//
// This header has no Arduino dependencies and can be built on any host.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace switch_matrix {

// Geometry — must match docs/pin-mapping.md and pins::NUM_COLS / NUM_ROWS.
constexpr uint8_t NUM_COLS  = 4;
constexpr uint8_t NUM_ROWS  = 5;
constexpr uint8_t NUM_CELLS = NUM_COLS * NUM_ROWS;   // 20

// Bit position for (col, row) in the 20-bit packed state.
//   bit = col_index * NUM_ROWS + row_index
// Switch number (1-based) = bit + 1.
constexpr uint8_t bit_index_for(uint8_t col, uint8_t row) {
    return static_cast<uint8_t>(col * NUM_ROWS + row);
}

// ---------------------------------------------------------------------------
// ScanIO — hardware abstraction
// ---------------------------------------------------------------------------
//
// The scanner asks the IO layer to drive one column LOW and read all rows.
// Implementations:
//   - scan_io_esp.cpp     — real ESP8266 GPIO driver (active when col is
//                           driven LOW; rows configured INPUT_PULLUP).
//   - test fakes          — return canned row patterns per call.
//
// `read_rows(col)` returns a 5-bit mask where bit r == 1 means switch
// at (col, r) is currently CLOSED (raw, undebounced). This is the
// natural orientation for callers; the GPIO implementation inverts the
// active-LOW electrical signal internally.
struct ScanIO {
    virtual ~ScanIO() = default;

    // One-time init (configure column outputs HIGH, rows as inputs with
    // pull-ups). Called once from `SwitchMatrix::begin()`.
    virtual void configure() = 0;

    // Drive `col` LOW and every other column HIGH; sample all rows;
    // return rows as bits 0..NUM_ROWS-1, where 1 == switch closed.
    virtual uint8_t drive_and_read(uint8_t col) = 0;

    // Optional idle-state hook called at the end of each full sweep.
    // Default: drive every column HIGH so no row is being pulled.
    // Real ESP impl performs this; the test fake may leave it empty.
    virtual void idle() {}
};

// ---------------------------------------------------------------------------
// SwitchMatrix — pure-logic scanner + debounce state machine
// ---------------------------------------------------------------------------
//
// One scan iteration = one column dwell (poll_interval_ms long, enforced
// by the caller through the cooperative loop). After NUM_COLS iterations
// every cell has been sampled once. A cell flips to its newly-observed
// raw state only after `debounce_samples` consecutive identical samples.
class SwitchMatrix {
public:
    SwitchMatrix();

    // Bind hardware abstraction + debounce parameter. Calls io.configure().
    // `debounce_samples` clamps to [1, 16]; values outside are coerced.
    void begin(ScanIO& io, uint8_t debounce_samples);

    // Advance the scanner by one column dwell. Non-blocking; returns true
    // if any debounced cell flipped during this tick (caller may use it
    // to short-circuit downstream work, but is not required to).
    //
    // The caller is responsible for spacing successive `tick()` calls by
    // `scan.poll_interval_ms`. SwitchMatrix does not call millis() itself
    // so the host test can drive it with deterministic timing.
    bool tick();

    // Current debounced state — low NUM_CELLS bits valid, upper bits 0.
    uint32_t state() const { return s_state_; }

    // Number of confirmed flip transitions observed since `begin()`.
    // Wraps at uint32_t max — used by the inactivity timer in §7.
    uint32_t change_count() const { return s_change_count_; }

    // Force the debounced state (test/inspection only). Does NOT touch the
    // pending-sample counters.
    void force_state(uint32_t s) { s_state_ = s & ((1u << NUM_CELLS) - 1u); }

private:
    ScanIO*  io_;
    uint8_t  debounce_;
    uint8_t  next_col_;             // column scanned on the next tick()

    // For each cell: the candidate raw value (0/1) currently being
    // accumulated, and the count of consecutive identical samples
    // observed for that candidate. When count >= debounce_, the cell
    // commits and (if different from the current debounced bit) flips it.
    uint8_t  cand_value_[NUM_CELLS];
    uint8_t  cand_count_[NUM_CELLS];

    uint32_t s_state_;
    uint32_t s_change_count_;
};

#ifdef ARDUINO
// Returns the singleton GPIO-backed ScanIO (defined in scan_io_esp.cpp).
ScanIO& esp_scan_io();
#endif

} // namespace switch_matrix
