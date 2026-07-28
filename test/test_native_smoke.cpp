// test_native_smoke.cpp — Phase 0 + Phase 1 + Phase 6 + Phase 7 native smoke tests.
//
// Phase 0: verify toolchain, hardware constants.
// Phase 1: verify cfg::Config defaults, JSON round-trip, field validation.
// Phase 6: verify SwitchMatrix debounce, noise rejection, bit assignment.
// Phase 7: code_engine formatter, target parser, live/latching state machines.
// Phase 9b: battery_profiles built-in curves, custom parser, eval + resolve.
// Phase 9c: sleep manager — profile gating, timeout maths, idle_minutes.
//
// config_json.cpp is included directly (test_build_src = false for native);
// it has no LittleFS / WiFi / log dependencies.
#include <unity.h>

// Pull in config.h + config_json.cpp via the -I test/stubs build flag.
#include "../src/config.h"
#include "../src/config_json.cpp"
#include "../src/switch_matrix.h"
#include "../src/switch_matrix.cpp"
#include "../src/code_engine.h"
#include "../src/code_engine.cpp"
#include "../src/boot_time.h"
#include "../src/boot_time.cpp"

// Phase 9b: battery_profiles (needs log.cpp to link pxlog symbols).
#include "../src/log.h"
#include "../src/log.cpp"
#include "../src/battery_profiles.h"
#include "../src/battery_profiles.cpp"

// Provide the EspClass and SerialStub instances that Arduino.h declares extern.
EspClass   ESP;
SerialStub Serial;

static unsigned long g_millis = 0;
unsigned long millis() { return g_millis; }

// ---------------------------------------------------------------------------
void setUp()    {}
void tearDown() {}
// ---------------------------------------------------------------------------

void test_i2c_pins() {
    TEST_ASSERT_EQUAL(0, pins::I2C_SDA);
    TEST_ASSERT_EQUAL(2, pins::I2C_SCL);
}

void test_i2c_addresses() {
    TEST_ASSERT_EQUAL_HEX8(0x70, i2c_addr::DISPLAY_LOW);
    TEST_ASSERT_EQUAL_HEX8(0x71, i2c_addr::DISPLAY_HIGH);
}

void test_matrix_dimensions() {
    TEST_ASSERT_EQUAL(4, pins::NUM_COLS);
    TEST_ASSERT_EQUAL(5, pins::NUM_ROWS);
    TEST_ASSERT_EQUAL(20, MATRIX_NUM_CELLS);
}

void test_matrix_col_pins() {
    TEST_ASSERT_EQUAL(15, pins::COL[0]);  // GPIO15 / D8
    TEST_ASSERT_EQUAL(1,  pins::COL[1]);  // GPIO1  / TX  (accepted HW bug)
    TEST_ASSERT_EQUAL(5,  pins::COL[2]);  // GPIO5  / D1
    TEST_ASSERT_EQUAL(16, pins::COL[3]);  // GPIO16 / D0
}

void test_matrix_row_pins() {
    TEST_ASSERT_EQUAL(12, pins::ROW[0]);  // GPIO12 / D6
    TEST_ASSERT_EQUAL(3,  pins::ROW[1]);  // GPIO3  / RX  (accepted HW bug)
    TEST_ASSERT_EQUAL(14, pins::ROW[2]);  // GPIO14 / D5
    TEST_ASSERT_EQUAL(4,  pins::ROW[3]);  // GPIO4  / D2
    TEST_ASSERT_EQUAL(13, pins::ROW[4]);  // GPIO13 / D7
}

void test_display_defaults() {
    TEST_ASSERT_EQUAL(1,    DISPLAY_BRIGHTNESS_DEFAULT);
    TEST_ASSERT_EQUAL(500,  DISPLAY_BLINK_HALF_PERIOD_MS);
    TEST_ASSERT_EQUAL(2000, IDENTIFY_DURATION_MS);
}

void test_log_constants() {
    TEST_ASSERT_EQUAL(32,  LOG_RING_LINES);
    TEST_ASSERT_EQUAL(160, LOG_LINE_MAX);
}

// ---------------------------------------------------------------------------
// Phase 1 — cfg::Config tests
// ---------------------------------------------------------------------------

void test_config_defaults() {
    cfg::Config c;
    cfg::load_defaults(c);

    TEST_ASSERT_EQUAL_STRING("px-enigma",       c.prop_name.c_str());
    TEST_ASSERT_EQUAL_STRING("paradox/enigma1", c.mqtt_base_topic.c_str());
    TEST_ASSERT_EQUAL(1883, c.mqtt_port);
    TEST_ASSERT_EQUAL_STRING(cfg::PUZZLE_MODE_LIVE,   c.puzzle_mode.c_str());
    TEST_ASSERT_EQUAL_STRING(cfg::START_STATE_ACTIVE, c.puzzle_start_state.c_str());
    TEST_ASSERT_FALSE(c.puzzle_has_target);
    TEST_ASSERT_EQUAL(0u, c.puzzle_target.length());
    TEST_ASSERT_EQUAL(DISPLAY_BRIGHTNESS_DEFAULT, c.display_brightness);
    TEST_ASSERT_TRUE(c.signal_indicator_enabled);
    TEST_ASSERT_EQUAL_STRING(cfg::BATT_PROFILE_EXTERNAL, c.battery_profile.c_str());
    TEST_ASSERT_EQUAL(0u, c.battery_points.length());
    TEST_ASSERT_EQUAL(40, c.battery_low_percent);
    TEST_ASSERT_EQUAL(10, c.battery_cutoff_percent);
    TEST_ASSERT_EQUAL(60, c.battery_inactivity_minutes);
    TEST_ASSERT_EQUAL(10, c.scan_poll_interval_ms);
    TEST_ASSERT_EQUAL(4,  c.scan_debounce_samples);
}

void test_config_rssi_defaults() {
    cfg::Config c;
    cfg::load_defaults(c);

    TEST_ASSERT_EQUAL(-55, c.signal_rssi_dbm[0]);
    TEST_ASSERT_EQUAL(-60, c.signal_rssi_dbm[1]);
    TEST_ASSERT_EQUAL(-65, c.signal_rssi_dbm[2]);
    TEST_ASSERT_EQUAL(-70, c.signal_rssi_dbm[3]);
    TEST_ASSERT_EQUAL(-75, c.signal_rssi_dbm[4]);
    TEST_ASSERT_EQUAL(-80, c.signal_rssi_dbm[5]);
    TEST_ASSERT_EQUAL(-85, c.signal_rssi_dbm[6]);
}

void test_config_roundtrip() {
    cfg::Config c;
    cfg::load_defaults(c);

    c.mqtt_host             = "broker.local";
    c.mqtt_port             = 8883;
    c.puzzle_mode           = cfg::PUZZLE_MODE_LATCHING;
    c.puzzle_target         = "12-34-56";
    c.puzzle_has_target     = true;
    c.display_brightness    = 7;
    c.battery_profile       = cfg::BATT_PROFILE_12V_LEAD;
    c.battery_low_percent   = 30;
    c.battery_points        = "12.85:100,12.00:0";
    c.signal_indicator_enabled = false;

    JsonDocument out;
    cfg::to_json(c, out);

    cfg::Config c2;
    cfg::load_defaults(c2);
    String err;
    bool ok = cfg::from_json(c2, out, &err);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("broker.local",             c2.mqtt_host.c_str());
    TEST_ASSERT_EQUAL(8883,                              c2.mqtt_port);
    TEST_ASSERT_EQUAL_STRING(cfg::PUZZLE_MODE_LATCHING,  c2.puzzle_mode.c_str());
    TEST_ASSERT_EQUAL_STRING("12-34-56",                 c2.puzzle_target.c_str());
    TEST_ASSERT_TRUE(c2.puzzle_has_target);
    TEST_ASSERT_EQUAL(7,                                 c2.display_brightness);
    TEST_ASSERT_EQUAL_STRING(cfg::BATT_PROFILE_12V_LEAD, c2.battery_profile.c_str());
    TEST_ASSERT_EQUAL(30,                                c2.battery_low_percent);
    TEST_ASSERT_EQUAL_STRING("12.85:100,12.00:0",        c2.battery_points.c_str());
    TEST_ASSERT_FALSE(c2.signal_indicator_enabled);
}

void test_config_null_target_roundtrip() {
    cfg::Config c;
    cfg::load_defaults(c);
    // Deliberately poison to confirm null is written and re-read correctly.
    c.puzzle_has_target = false;
    c.puzzle_target     = "";

    JsonDocument out;
    cfg::to_json(c, out);
    TEST_ASSERT_TRUE(out["puzzle"]["target"].isNull());

    cfg::Config c2;
    cfg::load_defaults(c2);
    c2.puzzle_has_target = true;
    c2.puzzle_target     = "OLD";
    cfg::from_json(c2, out, nullptr);

    TEST_ASSERT_FALSE(c2.puzzle_has_target);
    TEST_ASSERT_EQUAL(0u, c2.puzzle_target.length());
}

void test_config_validation_bad_mode() {
    JsonDocument doc;
    doc["puzzle"]["mode"] = "quantum";

    cfg::Config c;
    cfg::load_defaults(c);
    String err;
    TEST_ASSERT_FALSE(cfg::from_json(c, doc, &err));
    TEST_ASSERT_GREATER_THAN(0u, err.length());
}

void test_config_validation_bad_port() {
    JsonDocument doc;
    doc["mqtt"]["port"] = 99999;

    cfg::Config c;
    cfg::load_defaults(c);
    String err;
    TEST_ASSERT_FALSE(cfg::from_json(c, doc, &err));
}

void test_config_validation_bad_brightness() {
    JsonDocument doc;
    doc["display"]["brightness"] = 16;   // max is 15

    cfg::Config c;
    cfg::load_defaults(c);
    TEST_ASSERT_FALSE(cfg::from_json(c, doc, nullptr));
}

// ---------------------------------------------------------------------------
// Phase 6 — switch matrix
// ---------------------------------------------------------------------------

namespace {

// Test ScanIO: returns whatever the test has poked into row_pattern_[col].
// The test code pokes new values to simulate switch closures + noise.
class FakeScanIO : public switch_matrix::ScanIO {
public:
    FakeScanIO() {
        for (uint8_t c = 0; c < switch_matrix::NUM_COLS; ++c) row_pattern_[c] = 0;
    }
    void configure() override { configure_calls_++; }
    uint8_t drive_and_read(uint8_t col) override {
        if (col >= switch_matrix::NUM_COLS) return 0;
        last_col_read_ = col;
        return row_pattern_[col];
    }
    void idle() override { idle_calls_++; }

    void set_row(uint8_t col, uint8_t row, bool closed) {
        if (col >= switch_matrix::NUM_COLS || row >= switch_matrix::NUM_ROWS) return;
        if (closed) row_pattern_[col] |=  (uint8_t)(1u << row);
        else        row_pattern_[col] &= (uint8_t)~(1u << row);
    }
    void set_col_pattern(uint8_t col, uint8_t pattern) {
        if (col < switch_matrix::NUM_COLS) row_pattern_[col] = pattern;
    }

    uint8_t configure_calls_ = 0;
    uint8_t idle_calls_      = 0;
    uint8_t last_col_read_   = 0xFF;
    uint8_t row_pattern_[switch_matrix::NUM_COLS];
};

// Run a complete scan sweep (NUM_COLS ticks).
void sweep(switch_matrix::SwitchMatrix& sm, uint8_t n = switch_matrix::NUM_COLS) {
    for (uint8_t i = 0; i < n; ++i) sm.tick();
}

} // namespace

void test_matrix_bit_index_for() {
    // Bit layout: bit = col_index * NUM_ROWS + row_index
    using switch_matrix::bit_index_for;
    TEST_ASSERT_EQUAL(0,  bit_index_for(0, 0));   // col0,row0 → switch 1
    TEST_ASSERT_EQUAL(4,  bit_index_for(0, 4));
    TEST_ASSERT_EQUAL(5,  bit_index_for(1, 0));
    TEST_ASSERT_EQUAL(19, bit_index_for(3, 4));   // last cell
}

void test_matrix_initial_state_is_zero() {
    FakeScanIO io;
    switch_matrix::SwitchMatrix sm;
    sm.begin(io, 4);
    TEST_ASSERT_EQUAL_UINT32(0u, sm.state());
    TEST_ASSERT_EQUAL_UINT32(0u, sm.change_count());
    TEST_ASSERT_EQUAL_UINT8(1, io.configure_calls_);
}

void test_matrix_idle_called_at_end_of_sweep() {
    FakeScanIO io;
    switch_matrix::SwitchMatrix sm;
    sm.begin(io, 1);
    TEST_ASSERT_EQUAL_UINT8(0, io.idle_calls_);
    sweep(sm);   // exactly NUM_COLS ticks
    TEST_ASSERT_EQUAL_UINT8(1, io.idle_calls_);
    sweep(sm);
    TEST_ASSERT_EQUAL_UINT8(2, io.idle_calls_);
}

void test_matrix_debounce_on_transition() {
    // With debounce_samples = 4, a closure must persist for 4 sweeps before
    // the bit flips. Sweep #1 starts the candidate (count = 1); flip occurs
    // when count reaches 4.
    FakeScanIO io;
    switch_matrix::SwitchMatrix sm;
    sm.begin(io, 4);

    io.set_row(/*col=*/2, /*row=*/3, true);
    const uint8_t bit = switch_matrix::bit_index_for(2, 3);

    sweep(sm); TEST_ASSERT_EQUAL_UINT32(0u, sm.state());   // sample 1
    sweep(sm); TEST_ASSERT_EQUAL_UINT32(0u, sm.state());   // sample 2
    sweep(sm); TEST_ASSERT_EQUAL_UINT32(0u, sm.state());   // sample 3
    sweep(sm);                                             // sample 4 → flip
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(1u << bit), sm.state());
    TEST_ASSERT_EQUAL_UINT32(1u, sm.change_count());
}

void test_matrix_debounce_off_transition() {
    FakeScanIO io;
    switch_matrix::SwitchMatrix sm;
    sm.begin(io, 4);

    // Pre-arm: close (col=1,row=2) until it commits, then re-open and verify
    // the off-flip is also debounced.
    const uint8_t bit = switch_matrix::bit_index_for(1, 2);
    io.set_row(1, 2, true);
    sweep(sm); sweep(sm); sweep(sm); sweep(sm);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(1u << bit), sm.state());

    io.set_row(1, 2, false);
    sweep(sm); TEST_ASSERT_EQUAL_UINT32((uint32_t)(1u << bit), sm.state());
    sweep(sm); TEST_ASSERT_EQUAL_UINT32((uint32_t)(1u << bit), sm.state());
    sweep(sm); TEST_ASSERT_EQUAL_UINT32((uint32_t)(1u << bit), sm.state());
    sweep(sm); TEST_ASSERT_EQUAL_UINT32(0u, sm.state());
    TEST_ASSERT_EQUAL_UINT32(2u, sm.change_count());
}

void test_matrix_rejects_single_sample_noise() {
    FakeScanIO io;
    switch_matrix::SwitchMatrix sm;
    sm.begin(io, 4);

    // Sweep 1: pulse a single closure on (col=0,row=0).
    io.set_row(0, 0, true);
    sweep(sm);                       // sample 1 of "closed"
    io.set_row(0, 0, false);
    sweep(sm);                       // sample 1 of "open" (resets candidate)
    sweep(sm);
    sweep(sm);
    sweep(sm);
    TEST_ASSERT_EQUAL_UINT32(0u, sm.state());
    TEST_ASSERT_EQUAL_UINT32(0u, sm.change_count());
}

void test_matrix_independent_cells() {
    // Multiple cells closing simultaneously should each commit independently
    // and pack into the correct bit positions.
    FakeScanIO io;
    switch_matrix::SwitchMatrix sm;
    sm.begin(io, 2);

    io.set_row(0, 0, true);   // bit 0
    io.set_row(2, 4, true);   // bit 14
    io.set_row(3, 4, true);   // bit 19 (top-most)

    sweep(sm); sweep(sm);     // 2 sweeps == 2 samples per cell → flip

    const uint32_t expected = (1u << 0) | (1u << 14) | (1u << 19);
    TEST_ASSERT_EQUAL_UINT32(expected, sm.state());
    TEST_ASSERT_EQUAL_UINT32(3u, sm.change_count());
}

void test_matrix_force_state_masks_to_20_bits() {
    FakeScanIO io;
    switch_matrix::SwitchMatrix sm;
    sm.begin(io, 4);
    sm.force_state(0xFFFFFFFFu);
    TEST_ASSERT_EQUAL_UINT32((1u << switch_matrix::NUM_CELLS) - 1u, sm.state());
}

void test_matrix_debounce_clamps_to_one() {
    // debounce=0 should clamp to 1 (every sample commits immediately).
    FakeScanIO io;
    switch_matrix::SwitchMatrix sm;
    sm.begin(io, 0);
    io.set_row(0, 0, true);
    sweep(sm, 1);             // single tick on col 0
    TEST_ASSERT_EQUAL_UINT32(1u, sm.state());
}

void test_matrix_custom_bit_map_applies() {
    FakeScanIO io;
    switch_matrix::SwitchMatrix sm;
    sm.begin(io, 1);

    // Build the requested mapping:
    // A(row): [3,2,0,4,1], B(col): [3,2,0,1]
    const uint8_t row_a[switch_matrix::NUM_ROWS] = {3, 2, 0, 4, 1};
    const uint8_t col_b[switch_matrix::NUM_COLS] = {3, 2, 0, 1};
    uint8_t bit_map[switch_matrix::NUM_CELLS] = {0};
    for (uint8_t c = 0; c < switch_matrix::NUM_COLS; ++c) {
        for (uint8_t r = 0; r < switch_matrix::NUM_ROWS; ++r) {
            uint8_t phys = switch_matrix::bit_index_for(c, r);
            bit_map[phys] = (uint8_t)(row_a[r] + switch_matrix::NUM_ROWS * col_b[c]);
        }
    }
    TEST_ASSERT_TRUE(sm.set_bit_map(bit_map));

    // Close physical (col=2,row=2) [1-based (3,3)] => A=0,B=0 => bit 0 => value 1.
    io.set_row(/*col=*/2, /*row=*/2, true);
    sweep(sm);
    TEST_ASSERT_EQUAL_UINT32(1u, sm.state());

    // Also close physical (col=3,row=2) [1-based (4,3)] => A=0,B=1 => bit 5 => +32.
    io.set_row(/*col=*/3, /*row=*/2, true);
    sweep(sm);
    TEST_ASSERT_EQUAL_UINT32(33u, sm.state());
}

// ---------------------------------------------------------------------------

// ============================================================================
// Phase 7 — code engine
// ============================================================================

namespace {

struct EventLog {
    int changed   = 0;
    int solved    = 0;
    int unsolved  = 0;
    int solve     = 0;
    int unlatch   = 0;

    uint32_t last_code_int  = 0;
    uint32_t last_code_bits = 0;
    char     last_code_str[9] = {};

    void reset() { *this = EventLog(); }
};

static EventLog g_evlog;

static code_engine::Callbacks make_callbacks() {
    code_engine::Callbacks cb;
    cb.user = &g_evlog;
    cb.on_code_changed = [](uint32_t ci, uint32_t cb_, const char* cs, void* u) {
        auto* log = static_cast<EventLog*>(u);
        log->changed++;
        log->last_code_int  = ci;
        log->last_code_bits = cb_;
        strncpy(log->last_code_str, cs, 8);
        log->last_code_str[8] = '\0';
    };
    cb.on_code_solved = [](uint32_t, uint32_t, const char*, void* u) {
        static_cast<EventLog*>(u)->solved++;
    };
    cb.on_code_unsolved = [](uint32_t, uint32_t, const char*, void* u) {
        static_cast<EventLog*>(u)->unsolved++;
    };
    cb.on_solve = [](uint32_t, uint32_t, const char*, void* u) {
        static_cast<EventLog*>(u)->solve++;
    };
    cb.on_unlatch = [](void* u) {
        static_cast<EventLog*>(u)->unlatch++;
    };
    return cb;
}

} // namespace

// ---- formatter ----

void test_engine_format_code_basic() {
    char buf[9];
    code_engine::format_code(123456, buf);
    TEST_ASSERT_EQUAL_STRING("12-34-56", buf);
}

void test_engine_format_code_leading_zeros() {
    char buf[9];
    code_engine::format_code(123, buf);
    TEST_ASSERT_EQUAL_STRING("00-01-23", buf);
}

void test_engine_format_code_zero() {
    char buf[9];
    code_engine::format_code(0, buf);
    TEST_ASSERT_EQUAL_STRING("00-00-00", buf);
}

void test_engine_format_code_max() {
    char buf[9];
    code_engine::format_code(999999, buf);
    TEST_ASSERT_EQUAL_STRING("99-99-99", buf);
}

void test_engine_format_code_wraps_at_1M() {
    char buf[9];
    code_engine::format_code(1000001, buf);  // 1,000,001 mod 1M = 1
    TEST_ASSERT_EQUAL_STRING("00-00-01", buf);
}

void test_engine_format_code_custom_digit_order() {
    char buf[9];
    const uint8_t ord[6] = {4, 2, 6, 1, 5, 3};
    TEST_ASSERT_TRUE(code_engine::set_digit_order(ord));

    code_engine::format_code(123456, buf);
    TEST_ASSERT_EQUAL_STRING("42-61-53", buf);
    code_engine::format_code(8192, buf);  // 008192
    TEST_ASSERT_EQUAL_STRING("10-20-98", buf);

    code_engine::reset_digit_order();
}

void test_target_matrix_bits_identity_order() {
    code_engine::reset_digit_order();
    TEST_ASSERT_EQUAL_UINT32(123456u, code_engine::target_matrix_bits(123456u));
}

void test_target_matrix_bits_custom_digit_order() {
    const uint8_t ord[6] = {4, 2, 6, 1, 5, 3};
    TEST_ASSERT_TRUE(code_engine::set_digit_order(ord));
    // Display target 123456 → raw digit rearrangement 426153
    TEST_ASSERT_EQUAL_UINT32(426153u, code_engine::target_matrix_bits(123456u));
    code_engine::reset_digit_order();
}

// ---- target parser ----

void test_engine_parse_target_integer_string() {
    uint32_t v = 0;
    TEST_ASSERT_TRUE(code_engine::parse_target("123456", &v));
    TEST_ASSERT_EQUAL_UINT32(123456u, v);
}

void test_engine_parse_target_hyphenated() {
    uint32_t v = 0;
    TEST_ASSERT_TRUE(code_engine::parse_target("12-34-56", &v));
    TEST_ASSERT_EQUAL_UINT32(123456u, v);
}

void test_engine_parse_target_short_left_padded() {
    uint32_t v = 0;
    TEST_ASSERT_TRUE(code_engine::parse_target("123", &v));
    TEST_ASSERT_EQUAL_UINT32(123u, v);
}

void test_engine_parse_target_hyphenated_single_digits() {
    uint32_t v = 0;
    TEST_ASSERT_TRUE(code_engine::parse_target("1-2-3", &v));
    TEST_ASSERT_EQUAL_UINT32(123u, v);
}

void test_engine_parse_target_zero() {
    uint32_t v = 99;
    TEST_ASSERT_TRUE(code_engine::parse_target("000000", &v));
    TEST_ASSERT_EQUAL_UINT32(0u, v);
}

void test_engine_parse_target_max() {
    uint32_t v = 0;
    TEST_ASSERT_TRUE(code_engine::parse_target("999999", &v));
    TEST_ASSERT_EQUAL_UINT32(999999u, v);
}

void test_engine_parse_target_rejects_letters() {
    uint32_t v = 0;
    TEST_ASSERT_FALSE(code_engine::parse_target("12ab56", &v));
}

void test_engine_parse_target_rejects_overflow() {
    uint32_t v = 0;
    // 7 digits → too long
    TEST_ASSERT_FALSE(code_engine::parse_target("1234567", &v));
}

void test_engine_parse_target_rejects_empty() {
    uint32_t v = 0;
    TEST_ASSERT_FALSE(code_engine::parse_target("", &v));
}

// ---- live mode code_changed ----

void test_engine_live_fires_code_changed() {
    g_evlog.reset();
    code_engine::CodeEngine eng;
    eng.begin("live", false, 0, make_callbacks());

    eng.tick(42u);
    TEST_ASSERT_EQUAL_INT(1, g_evlog.changed);
    TEST_ASSERT_EQUAL_UINT32(42u, g_evlog.last_code_bits);
    TEST_ASSERT_EQUAL_UINT32(42u, g_evlog.last_code_int);
    TEST_ASSERT_EQUAL_STRING("00-00-42", g_evlog.last_code_str);
}

void test_engine_live_no_duplicate_event_for_same_state() {
    g_evlog.reset();
    code_engine::CodeEngine eng;
    eng.begin("live", false, 0, make_callbacks());

    eng.tick(7u);
    eng.tick(7u);
    TEST_ASSERT_EQUAL_INT(1, g_evlog.changed);  // only one
}

// ---- live mode target matching ----

void test_engine_live_code_solved_and_unsolved() {
    g_evlog.reset();
    code_engine::CodeEngine eng;
    eng.begin("live", true, 100u, make_callbacks());  // target = 000100

    eng.tick(100u);   // matches
    TEST_ASSERT_EQUAL_INT(1, g_evlog.solved);
    TEST_ASSERT_EQUAL_INT(0, g_evlog.unsolved);
    TEST_ASSERT_TRUE(eng.state().solved);

    eng.tick(101u);   // unmatches
    TEST_ASSERT_EQUAL_INT(1, g_evlog.unsolved);
    TEST_ASSERT_FALSE(eng.state().solved);
}

void test_engine_live_solved_fires_each_time() {
    // Unlike latching, live mode fires on_code_solved every entry.
    g_evlog.reset();
    code_engine::CodeEngine eng;
    eng.begin("live", true, 50u, make_callbacks());

    eng.tick(50u);   // solve
    eng.tick(51u);   // unsolve
    eng.tick(50u);   // solve again
    TEST_ASSERT_EQUAL_INT(2, g_evlog.solved);
    TEST_ASSERT_EQUAL_INT(1, g_evlog.unsolved);
}

void test_engine_live_no_target_no_solved_event() {
    g_evlog.reset();
    code_engine::CodeEngine eng;
    eng.begin("live", false, 0, make_callbacks());

    eng.tick(123456u);
    TEST_ASSERT_EQUAL_INT(0, g_evlog.solved);
}

// ---- latching mode ----

void test_engine_latching_solve_fires_once() {
    g_evlog.reset();
    code_engine::CodeEngine eng;
    eng.begin("latching", true, 999u, make_callbacks());

    eng.tick(999u);   // first match → solve
    TEST_ASSERT_EQUAL_INT(1, g_evlog.solve);
    TEST_ASSERT_TRUE(eng.state().latched);

    // Subsequent ticks after latching: solve must NOT fire again,
    // and code_changed must NOT fire while latched.
    int cnt_at_latch = g_evlog.changed;
    eng.tick(998u);
    eng.tick(999u);
    TEST_ASSERT_EQUAL_INT(1, g_evlog.solve);           // still 1
    TEST_ASSERT_EQUAL_INT(cnt_at_latch, g_evlog.changed); // no new code_changed while LATCHED
}

void test_engine_latching_reset_clears_latch() {
    g_evlog.reset();
    code_engine::CodeEngine eng;
    eng.begin("latching", true, 5u, make_callbacks());

    eng.tick(5u);     // solve → LATCHED
    TEST_ASSERT_TRUE(eng.state().latched);

    eng.reset();      // → ACTIVE
    TEST_ASSERT_FALSE(eng.state().latched);
    TEST_ASSERT_EQUAL_INT(1, g_evlog.unlatch);

    // After reset, code_changed fires again on next matrix change.
    // changed_count was already 1 from tick(5u) before the solve.
    int cnt_before = g_evlog.changed;
    eng.tick(6u);
    TEST_ASSERT_EQUAL_INT(cnt_before + 1, g_evlog.changed);
}

void test_engine_latching_code_bits_updates_while_latched() {
    // §5.2: "switch-state changes are still tracked in code_bits even
    // while LATCHED"
    g_evlog.reset();
    code_engine::CodeEngine eng;
    eng.begin("latching", true, 3u, make_callbacks());

    eng.tick(3u);     // solve → LATCHED (fires code_changed once)
    int cnt_at_latch = g_evlog.changed;
    eng.tick(4u);     // matrix changes while LATCHED
    TEST_ASSERT_EQUAL_UINT32(4u, eng.state().code_bits);     // updated
    TEST_ASSERT_EQUAL_INT(cnt_at_latch, g_evlog.changed);    // no new code_changed while LATCHED
}

void test_engine_latching_mode_switch_clears_latch_silently() {
    g_evlog.reset();
    code_engine::CodeEngine eng;
    eng.begin("latching", true, 10u, make_callbacks());

    eng.tick(10u);    // → LATCHED
    eng.set_mode(code_engine::Mode::Live);
    TEST_ASSERT_FALSE(eng.state().latched);
    TEST_ASSERT_EQUAL_INT(0, g_evlog.unlatch); // silent
}

void test_engine_latching_no_target_does_not_latch() {
    g_evlog.reset();
    code_engine::CodeEngine eng;
    eng.begin("latching", false, 0, make_callbacks());

    eng.tick(0u);
    TEST_ASSERT_FALSE(eng.state().latched);
    TEST_ASSERT_EQUAL_INT(0, g_evlog.solve);
    // code_changed DOES fire (latching falls back to live when no target)
    TEST_ASSERT_EQUAL_INT(1, g_evlog.changed);
}

// ---- set_target at runtime ----

void test_engine_set_target_clears_latch_with_unlatch() {
    g_evlog.reset();
    code_engine::CodeEngine eng;
    eng.begin("latching", true, 7u, make_callbacks());

    eng.tick(7u);     // → LATCHED
    TEST_ASSERT_TRUE(eng.state().latched);

    eng.set_target(true, 8u);    // new target clears latch
    TEST_ASSERT_FALSE(eng.state().latched);
    TEST_ASSERT_EQUAL_INT(1, g_evlog.unlatch);
}

void test_engine_set_target_null_clears_solved() {
    g_evlog.reset();
    code_engine::CodeEngine eng;
    eng.begin("live", true, 55u, make_callbacks());

    eng.tick(55u);   // → solved
    TEST_ASSERT_TRUE(eng.state().solved);

    eng.set_target(false, 0);
    TEST_ASSERT_FALSE(eng.state().has_target);
    TEST_ASSERT_FALSE(eng.state().solved);
}

// ---- derive_code_int ----

void test_engine_derive_code_int_wraps() {
    TEST_ASSERT_EQUAL_UINT32(0u,   code_engine::derive_code_int(0u));
    TEST_ASSERT_EQUAL_UINT32(1u,   code_engine::derive_code_int(1000001u));
    TEST_ASSERT_EQUAL_UINT32(999999u, code_engine::derive_code_int(999999u));
    // Max 20-bit value: 0xFFFFF = 1048575. 1048575 mod 1000000 = 48575.
    TEST_ASSERT_EQUAL_UINT32(48575u, code_engine::derive_code_int(0xFFFFFu));
}

// ---------------------------------------------------------------------------
// Phase 8 — setTarget event semantics (spec §11.2)
// ---------------------------------------------------------------------------

void test_engine_set_target_live_immediate_match_fires_solved() {
    g_evlog.reset();
    code_engine::CodeEngine eng;
    eng.begin("live", false, 0, make_callbacks());
    eng.tick(42u);              // current code = 42, no target
    int before = g_evlog.solved;
    eng.set_target(true, 42u);  // immediate match
    TEST_ASSERT_EQUAL_INT(before + 1, g_evlog.solved);
    TEST_ASSERT_TRUE(eng.state().solved);
}

void test_engine_set_target_live_immediate_no_match_no_event() {
    g_evlog.reset();
    code_engine::CodeEngine eng;
    eng.begin("live", false, 0, make_callbacks());
    eng.tick(42u);
    eng.set_target(true, 99u);
    TEST_ASSERT_EQUAL_INT(0, g_evlog.solved);
    TEST_ASSERT_FALSE(eng.state().solved);
}

void test_engine_set_target_live_already_solved_no_duplicate_event() {
    // If already solved against the old target and the new target also
    // matches, no duplicate code_solved event.
    g_evlog.reset();
    code_engine::CodeEngine eng;
    eng.begin("live", true, 42u, make_callbacks());
    eng.tick(42u);              // fires code_solved (solved=true)
    int before = g_evlog.solved;
    eng.set_target(true, 42u);  // identical target — no new transition
    TEST_ASSERT_EQUAL_INT(before, g_evlog.solved);
}

void test_engine_set_target_latching_immediate_match_fires_solve() {
    g_evlog.reset();
    code_engine::CodeEngine eng;
    eng.begin("latching", false, 0, make_callbacks());
    eng.tick(123u);                 // code=123, no target
    eng.set_target(true, 123u);     // immediate match → latch
    TEST_ASSERT_EQUAL_INT(1, g_evlog.solve);
    TEST_ASSERT_TRUE(eng.state().latched);
}

void test_engine_set_target_latching_no_match_no_solve() {
    g_evlog.reset();
    code_engine::CodeEngine eng;
    eng.begin("latching", false, 0, make_callbacks());
    eng.tick(5u);
    eng.set_target(true, 6u);
    TEST_ASSERT_EQUAL_INT(0, g_evlog.solve);
    TEST_ASSERT_FALSE(eng.state().latched);
}

// ---------------------------------------------------------------------------
// Phase 9 — display digit-position mapping + battery calibration
//
// The hardware driver (display_mgr.cpp) is not compiled into the native build
// (it requires Wire / Adafruit_LEDBackpack).  We pin the pure data transforms
// that sit underneath the driver: the code formatter, the XX/YY/ZZ parser,
// and the battery ADC calibration arithmetic.
//
// The digit-position layout contract being tested:
//   For code "XX-YY-ZZ" (XX = code/10000, YY = (code/100)%100, ZZ = code%100):
//     display_high[0]  = XX / 10   (leftmost)
//     display_high[1]  = XX % 10
//     display_high[3]  = dash
//     display_high[4]  = YY / 10
//     display_low [0]  = YY % 10
//     display_low [1]  = dash
//     display_low [3]  = ZZ / 10
//     display_low [4]  = ZZ % 10   (rightmost)
// ---------------------------------------------------------------------------

// Helper: parse "XX-YY-ZZ" string → xx, yy, zz integers.
static void parse_xxyyzz(const char* s, int* xx, int* yy, int* zz) {
    *xx = (s[0] - '0') * 10 + (s[1] - '0');
    *yy = (s[3] - '0') * 10 + (s[4] - '0');
    *zz = (s[6] - '0') * 10 + (s[7] - '0');
}

void test_display_digit_position_parse() {
    // Verify that parse_xxyyzz correctly reverses format_code.
    char buf[9];
    code_engine::format_code(123456, buf);
    int xx, yy, zz;
    parse_xxyyzz(buf, &xx, &yy, &zz);
    TEST_ASSERT_EQUAL_INT(12, xx);
    TEST_ASSERT_EQUAL_INT(34, yy);
    TEST_ASSERT_EQUAL_INT(56, zz);
}

void test_display_digit_position_xx_yy_zz_parts() {
    // Pin the digit decomposition used by render_code() for "12-34-56":
    //   display_high[0] = xx / 10 = 1
    //   display_high[1] = xx % 10 = 2
    //   display_high[4] = yy / 10 = 3
    //   display_low[0]  = yy % 10 = 4
    //   display_low[3]  = zz / 10 = 5
    //   display_low[4]  = zz % 10 = 6
    char buf[9];
    code_engine::format_code(123456, buf);
    int xx, yy, zz;
    parse_xxyyzz(buf, &xx, &yy, &zz);

    TEST_ASSERT_EQUAL_INT(1, xx / 10);   // display_low[0]
    TEST_ASSERT_EQUAL_INT(2, xx % 10);   // display_high[1]
    TEST_ASSERT_EQUAL_INT(3, yy / 10);   // display_low[4]
    TEST_ASSERT_EQUAL_INT(4, yy % 10);   // display_high[0]
    TEST_ASSERT_EQUAL_INT(5, zz / 10);   // display_low[3]
    TEST_ASSERT_EQUAL_INT(6, zz % 10);   // display_high[4]
}

void test_display_format_code_roundtrip_all_parts() {
    // Walk the range boundary values and verify that the decomposition
    // xx = code / 10000, yy = (code / 100) % 100, zz = code % 100
    // is consistent with the string produced by format_code().
    const uint32_t cases[] = { 0u, 1u, 9u, 99u, 100u, 9999u, 10000u,
                                99999u, 100000u, 999999u };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint32_t code = cases[i];
        int exp_xx = (int)(code / 10000u);
        int exp_yy = (int)((code / 100u) % 100u);
        int exp_zz = (int)(code % 100u);

        char buf[9];
        code_engine::format_code(code, buf);
        int xx, yy, zz;
        parse_xxyyzz(buf, &xx, &yy, &zz);

        TEST_ASSERT_EQUAL_INT(exp_xx, xx);
        TEST_ASSERT_EQUAL_INT(exp_yy, yy);
        TEST_ASSERT_EQUAL_INT(exp_zz, zz);
    }
}

// ---------------------------------------------------------------------------
// Battery calibration arithmetic (mirrors battery_monitor.cpp maths).
// ---------------------------------------------------------------------------

static float batt_calc_voltage(uint16_t raw, uint16_t at_0v, uint16_t at_full, float full_v) {
    if (at_full == at_0v || at_full == 0 || full_v <= 0.0f) {
        // Default legacy: V = 0.0531 * raw + 0.1978
        return 0.0531f * (float)raw + 0.1978f;
    }
    float slope  = full_v / (float)(at_full - at_0v);
    float offset = -(slope * (float)at_0v);
    return slope * (float)raw + offset;
}

void test_battery_calibration_slope_offset() {
    // at_0v_raw=0, at_full_raw=230, full_v=12.0 → slope=12.0/230 ≈ 0.0522
    // V(0)   = 0.0
    // V(230) = 12.0
    float v0   = batt_calc_voltage(0,   0, 230, 12.0f);
    float v230 = batt_calc_voltage(230, 0, 230, 12.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f,  v0);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 12.0f, v230);
}

void test_battery_calibration_defaults_on_zero_range() {
    // Invalid calibration (at_full == at_0) → falls back to legacy defaults.
    // Legacy: V = 0.0531 * raw + 0.1978
    // At raw=100 → 0.0531*100 + 0.1978 = 5.508
    float v = batt_calc_voltage(100, 0, 0, 12.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0531f * 100.0f + 0.1978f, v);
}

// ---------------------------------------------------------------------------
// RSSI bars logic (mirrors display_mgr.cpp rssi_bars()).
// ---------------------------------------------------------------------------

static int rssi_bars_local(int rssi_dbm, const int8_t thresholds[cfg::RSSI_THRESHOLDS]) {
    if (rssi_dbm > 0) return 0;
    for (int i = 0; i < (int)cfg::RSSI_THRESHOLDS; ++i) {
        if (rssi_dbm >= thresholds[i]) return (int)cfg::RSSI_THRESHOLDS - i;
    }
    return 0;
}

void test_battery_rssi_bars_full_signal() {
    cfg::Config c;
    cfg::load_defaults(c);
    // Default thresholds: -55,-60,-65,-70,-75,-80,-85
    // rssi = -55 → >= thresholds[0] → bars = 7 - 0 = 7
    int bars = rssi_bars_local(-55, c.signal_rssi_dbm);
    TEST_ASSERT_EQUAL_INT(7, bars);
}

void test_battery_rssi_bars_no_signal() {
    cfg::Config c;
    cfg::load_defaults(c);
    // rssi = -95 → below all thresholds → 0
    int bars = rssi_bars_local(-95, c.signal_rssi_dbm);
    TEST_ASSERT_EQUAL_INT(0, bars);
}

void test_battery_rssi_bars_partial() {
    cfg::Config c;
    cfg::load_defaults(c);
    // rssi = -75 → >= thresholds[4] (-75) → bars = 7 - 4 = 3
    int bars = rssi_bars_local(-75, c.signal_rssi_dbm);
    TEST_ASSERT_EQUAL_INT(3, bars);
}

void test_battery_rssi_bars_disconnected() {
    cfg::Config c;
    cfg::load_defaults(c);
    // rssi > 0 means disconnected
    int bars = rssi_bars_local(1, c.signal_rssi_dbm);
    TEST_ASSERT_EQUAL_INT(0, bars);
}

// ---------------------------------------------------------------------------
// Phase 9b — battery_profiles: eval, built-in curves, custom parser, resolve
// ---------------------------------------------------------------------------

// ---- eval on 12v-lead-acid ----

void test_profile_eval_12v_lead_acid_above_full() {
    battery_profiles::Curve c;
    bool ok = battery_profiles::load_builtin(cfg::BATT_PROFILE_12V_LEAD, c);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(c.valid);
    // Voltage above highest point → 100 %
    TEST_ASSERT_EQUAL_INT(100, battery_profiles::eval(c, 13.5f));
}

void test_profile_eval_12v_lead_acid_below_empty() {
    battery_profiles::Curve c;
    battery_profiles::load_builtin(cfg::BATT_PROFILE_12V_LEAD, c);
    // Voltage below lowest point → 0 %
    TEST_ASSERT_EQUAL_INT(0, battery_profiles::eval(c, 10.0f));
}

void test_profile_eval_12v_lead_acid_midpoint_interpolation() {
    battery_profiles::Curve c;
    battery_profiles::load_builtin(cfg::BATT_PROFILE_12V_LEAD, c);
    // Table: {12.85,100}, {12.65,90} — midpoint ~12.75 V → ~95 %
    int pct = battery_profiles::eval(c, 12.75f);
    TEST_ASSERT_INT_WITHIN(3, 95, pct);
}

void test_profile_eval_12v_lifepo4_full() {
    battery_profiles::Curve c;
    bool ok = battery_profiles::load_builtin(cfg::BATT_PROFILE_12V_LIFEPO4, c);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(100, battery_profiles::eval(c, 14.5f));
}

void test_profile_eval_12v_lifepo4_empty() {
    battery_profiles::Curve c;
    battery_profiles::load_builtin(cfg::BATT_PROFILE_12V_LIFEPO4, c);
    TEST_ASSERT_EQUAL_INT(0, battery_profiles::eval(c, 10.0f));
}

void test_profile_eval_external_always_100() {
    // External profile → load_builtin returns true but Curve.valid = false.
    // eval() on an invalid curve returns 100 (treat as always-powered).
    battery_profiles::Curve c;
    bool ok = battery_profiles::load_builtin(cfg::BATT_PROFILE_EXTERNAL, c);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FALSE(c.valid);
    TEST_ASSERT_EQUAL_INT(100, battery_profiles::eval(c, 0.0f));
}

// ---- custom curve parser ----

void test_profile_parse_custom_csv_valid() {
    // "v:p,v:p,..." form with 3 descending points.
    battery_profiles::Curve c;
    char err[64] = "";
    bool ok = battery_profiles::parse_custom("12.5:100,12.0:50,11.5:0", c, err, sizeof(err));
    TEST_ASSERT_TRUE_MESSAGE(ok, err);
    TEST_ASSERT_TRUE(c.valid);
    TEST_ASSERT_EQUAL_UINT8(3, c.count);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 12.5f, c.points[0].v);
    TEST_ASSERT_EQUAL_UINT8(100, c.points[0].p);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 11.5f, c.points[2].v);
    TEST_ASSERT_EQUAL_UINT8(0,   c.points[2].p);
}

void test_profile_parse_custom_json_array_valid() {
    const char* json = "[{\"v\":12.5,\"p\":100},{\"v\":12.0,\"p\":50},{\"v\":11.5,\"p\":0}]";
    battery_profiles::Curve c;
    char err[64] = "";
    bool ok = battery_profiles::parse_custom(json, c, err, sizeof(err));
    TEST_ASSERT_TRUE_MESSAGE(ok, err);
    TEST_ASSERT_TRUE(c.valid);
    TEST_ASSERT_EQUAL_UINT8(3, c.count);
}

void test_profile_parse_custom_too_few_points() {
    battery_profiles::Curve c;
    char err[64] = "";
    // Only 1 point — needs ≥ 2.
    bool ok = battery_profiles::parse_custom("12.0:100", c, err, sizeof(err));
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_FALSE(c.valid);
}

void test_profile_parse_custom_voltages_not_decreasing() {
    battery_profiles::Curve c;
    char err[64] = "";
    // Voltages ascending → invalid.
    bool ok = battery_profiles::parse_custom("11.0:100,12.0:50,13.0:0", c, err, sizeof(err));
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_FALSE(c.valid);
}

void test_profile_parse_custom_percents_not_decreasing() {
    battery_profiles::Curve c;
    char err[64] = "";
    // Percents ascending → invalid.
    bool ok = battery_profiles::parse_custom("13.0:0,12.0:50,11.0:100", c, err, sizeof(err));
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_FALSE(c.valid);
}

void test_profile_parse_custom_voltage_out_of_range() {
    battery_profiles::Curve c;
    char err[64] = "";
    // Voltage 0.5 < 1.0 minimum.
    bool ok = battery_profiles::parse_custom("0.5:100,0.2:0", c, err, sizeof(err));
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_FALSE(c.valid);
}

void test_profile_resolve_builtin() {
    cfg::Config cfg_val;
    cfg::load_defaults(cfg_val);
    cfg_val.battery_profile = cfg::BATT_PROFILE_12V_LEAD;
    battery_profiles::Curve c;
    battery_profiles::resolve(cfg_val, c);
    TEST_ASSERT_TRUE(c.valid);
}

void test_profile_resolve_external_not_valid() {
    cfg::Config cfg_val;
    cfg::load_defaults(cfg_val);
    cfg_val.battery_profile = cfg::BATT_PROFILE_EXTERNAL;
    battery_profiles::Curve c;
    battery_profiles::resolve(cfg_val, c);
    TEST_ASSERT_FALSE(c.valid);  // external → caller uses 100 %
}

void test_profile_resolve_custom_valid_points() {
    cfg::Config cfg_val;
    cfg::load_defaults(cfg_val);
    cfg_val.battery_profile = cfg::BATT_PROFILE_CUSTOM;
    cfg_val.battery_points  = "12.5:100,12.0:50,11.5:0";
    battery_profiles::Curve c;
    battery_profiles::resolve(cfg_val, c);
    TEST_ASSERT_TRUE(c.valid);
    TEST_ASSERT_EQUAL_UINT8(3, c.count);
}

void test_profile_resolve_custom_invalid_fallback() {
    cfg::Config cfg_val;
    cfg::load_defaults(cfg_val);
    cfg_val.battery_profile = cfg::BATT_PROFILE_CUSTOM;
    cfg_val.battery_points  = "bad_data";  // unparseable
    battery_profiles::Curve c;
    battery_profiles::resolve(cfg_val, c);
    TEST_ASSERT_FALSE(c.valid);  // bad parse → falls back to unknown
}

// ---------------------------------------------------------------------------
// Phase 9c — sleep manager: profile gating, timeout maths, idle_minutes
//
// sleep_mgr.cpp pulls in mqtt_mgr and calls ESP.deepSleep(), so we test the
// core arithmetic and config-gating logic inline here rather than linking the
// full implementation.
// ---------------------------------------------------------------------------

// Mirror of sleep_mgr::begin() enable logic.
static bool sleep_enabled_for(const cfg::Config& c, bool is_external) {
    return !is_external && (c.battery_inactivity_minutes > 0);
}

// Mirror of sleep_mgr::idle_minutes() maths.
static uint32_t sleep_idle_minutes(uint32_t now_ms, uint32_t last_activity_ms) {
    return (now_ms - last_activity_ms) / 60000UL;
}

// Mirror of sleep_mgr timeout check.
static bool sleep_should_sleep(uint32_t now_ms, uint32_t last_activity_ms,
                                uint32_t timeout_ms) {
    return (now_ms - last_activity_ms) >= timeout_ms;
}

void test_sleep_disabled_for_external_profile() {
    cfg::Config c;
    cfg::load_defaults(c);
    // Default profile is "external" → is_external = true → disabled.
    c.battery_inactivity_minutes = 60;
    TEST_ASSERT_FALSE(sleep_enabled_for(c, /*is_external=*/true));
}

void test_sleep_disabled_for_zero_inactivity() {
    cfg::Config c;
    cfg::load_defaults(c);
    c.battery_inactivity_minutes = 0;
    // Battery profile, but minutes = 0 → disabled.
    TEST_ASSERT_FALSE(sleep_enabled_for(c, /*is_external=*/false));
}

void test_sleep_enabled_for_battery_profile() {
    cfg::Config c;
    cfg::load_defaults(c);
    c.battery_inactivity_minutes = 60;
    TEST_ASSERT_TRUE(sleep_enabled_for(c, /*is_external=*/false));
}

void test_sleep_idle_minutes_zero_immediately() {
    // At t=0, no time has elapsed → 0 idle minutes.
    TEST_ASSERT_EQUAL_UINT32(0, sleep_idle_minutes(1000UL, 1000UL));
}

void test_sleep_idle_minutes_rounds_down() {
    // 90 s elapsed → 1 minute (floors, not rounds).
    uint32_t elapsed_ms = 90UL * 1000UL;
    TEST_ASSERT_EQUAL_UINT32(1, sleep_idle_minutes(elapsed_ms, 0UL));
}

void test_sleep_idle_minutes_full_minute() {
    // Exactly 60 s → 1 minute.
    TEST_ASSERT_EQUAL_UINT32(1, sleep_idle_minutes(60000UL, 0UL));
}

void test_sleep_no_timeout_before_deadline() {
    // 59 s elapsed, timeout 60 s → should NOT sleep.
    uint32_t timeout_ms = 60UL * 1000UL;
    TEST_ASSERT_FALSE(sleep_should_sleep(59000UL, 0UL, timeout_ms));
}

void test_sleep_timeout_at_deadline() {
    // Exactly 60 s elapsed, timeout 60 s → SHOULD sleep.
    uint32_t timeout_ms = 60UL * 1000UL;
    TEST_ASSERT_TRUE(sleep_should_sleep(60000UL, 0UL, timeout_ms));
}

void test_sleep_timeout_past_deadline() {
    // Well past deadline → should sleep.
    uint32_t timeout_ms = 60UL * 1000UL;
    TEST_ASSERT_TRUE(sleep_should_sleep(120000UL, 0UL, timeout_ms));
}

void test_sleep_reset_by_switch_change() {
    // After a switch change the activity timestamp is reset.
    // Simulate: 90 s elapsed, switch fires at 60 s → only 30 s idle.
    uint32_t now_ms           = 90000UL;
    uint32_t last_activity_ms = 60000UL;   // reset at 60 s
    uint32_t timeout_ms       = 60UL * 1000UL;
    TEST_ASSERT_FALSE(sleep_should_sleep(now_ms, last_activity_ms, timeout_ms));
    TEST_ASSERT_EQUAL_UINT32(0, sleep_idle_minutes(now_ms, last_activity_ms));
}

void test_sleep_config_default_inactivity() {
    // Spec §7: default inactivity_minutes = 60.
    cfg::Config c;
    cfg::load_defaults(c);
    TEST_ASSERT_EQUAL_UINT32(60, c.battery_inactivity_minutes);
}

// ---------------------------------------------------------------------------

void test_boot_time_extends_millis_rollover() {
    g_millis = 0xFFFFFFF0UL;
    TEST_ASSERT_EQUAL_UINT64(0xFFFFFFF0ULL, boot_time::milliseconds());

    g_millis = 20UL;
    TEST_ASSERT_EQUAL_UINT64(0x100000014ULL, boot_time::milliseconds());
}

// ---------------------------------------------------------------------------

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_i2c_pins);
    RUN_TEST(test_i2c_addresses);
    RUN_TEST(test_matrix_dimensions);
    RUN_TEST(test_matrix_col_pins);
    RUN_TEST(test_matrix_row_pins);
    RUN_TEST(test_display_defaults);
    RUN_TEST(test_log_constants);
    // Phase 1 — config
    RUN_TEST(test_config_defaults);
    RUN_TEST(test_config_rssi_defaults);
    RUN_TEST(test_config_roundtrip);
    RUN_TEST(test_config_null_target_roundtrip);
    RUN_TEST(test_config_validation_bad_mode);
    RUN_TEST(test_config_validation_bad_port);
    RUN_TEST(test_config_validation_bad_brightness);
    // Phase 6 — switch matrix
    RUN_TEST(test_matrix_bit_index_for);
    RUN_TEST(test_matrix_initial_state_is_zero);
    RUN_TEST(test_matrix_idle_called_at_end_of_sweep);
    RUN_TEST(test_matrix_debounce_on_transition);
    RUN_TEST(test_matrix_debounce_off_transition);
    RUN_TEST(test_matrix_rejects_single_sample_noise);
    RUN_TEST(test_matrix_independent_cells);
    RUN_TEST(test_matrix_force_state_masks_to_20_bits);
    RUN_TEST(test_matrix_debounce_clamps_to_one);
    RUN_TEST(test_matrix_custom_bit_map_applies);
    // Phase 7 — code engine
    RUN_TEST(test_engine_format_code_basic);
    RUN_TEST(test_engine_format_code_leading_zeros);
    RUN_TEST(test_engine_format_code_zero);
    RUN_TEST(test_engine_format_code_max);
    RUN_TEST(test_engine_format_code_wraps_at_1M);
    RUN_TEST(test_engine_format_code_custom_digit_order);
    RUN_TEST(test_target_matrix_bits_identity_order);
    RUN_TEST(test_target_matrix_bits_custom_digit_order);
    RUN_TEST(test_engine_parse_target_integer_string);
    RUN_TEST(test_engine_parse_target_hyphenated);
    RUN_TEST(test_engine_parse_target_short_left_padded);
    RUN_TEST(test_engine_parse_target_hyphenated_single_digits);
    RUN_TEST(test_engine_parse_target_zero);
    RUN_TEST(test_engine_parse_target_max);
    RUN_TEST(test_engine_parse_target_rejects_letters);
    RUN_TEST(test_engine_parse_target_rejects_overflow);
    RUN_TEST(test_engine_parse_target_rejects_empty);
    RUN_TEST(test_engine_live_fires_code_changed);
    RUN_TEST(test_engine_live_no_duplicate_event_for_same_state);
    RUN_TEST(test_engine_live_code_solved_and_unsolved);
    RUN_TEST(test_engine_live_solved_fires_each_time);
    RUN_TEST(test_engine_live_no_target_no_solved_event);
    RUN_TEST(test_engine_latching_solve_fires_once);
    RUN_TEST(test_engine_latching_reset_clears_latch);
    RUN_TEST(test_engine_latching_code_bits_updates_while_latched);
    RUN_TEST(test_engine_latching_mode_switch_clears_latch_silently);
    RUN_TEST(test_engine_latching_no_target_does_not_latch);
    RUN_TEST(test_engine_set_target_clears_latch_with_unlatch);
    RUN_TEST(test_engine_set_target_null_clears_solved);
    RUN_TEST(test_engine_derive_code_int_wraps);
    // Phase 8 — setTarget immediate-match event semantics
    RUN_TEST(test_engine_set_target_live_immediate_match_fires_solved);
    RUN_TEST(test_engine_set_target_live_immediate_no_match_no_event);
    RUN_TEST(test_engine_set_target_live_already_solved_no_duplicate_event);
    RUN_TEST(test_engine_set_target_latching_immediate_match_fires_solve);
    RUN_TEST(test_engine_set_target_latching_no_match_no_solve);
    // Phase 9 — display digit-position mapping + battery calibration
    RUN_TEST(test_display_digit_position_parse);
    RUN_TEST(test_display_digit_position_xx_yy_zz_parts);
    RUN_TEST(test_display_format_code_roundtrip_all_parts);
    RUN_TEST(test_battery_calibration_slope_offset);
    RUN_TEST(test_battery_calibration_defaults_on_zero_range);
    RUN_TEST(test_battery_rssi_bars_full_signal);
    RUN_TEST(test_battery_rssi_bars_no_signal);
    RUN_TEST(test_battery_rssi_bars_partial);
    RUN_TEST(test_battery_rssi_bars_disconnected);
    // Phase 9b — battery_profiles
    RUN_TEST(test_profile_eval_12v_lead_acid_above_full);
    RUN_TEST(test_profile_eval_12v_lead_acid_below_empty);
    RUN_TEST(test_profile_eval_12v_lead_acid_midpoint_interpolation);
    RUN_TEST(test_profile_eval_12v_lifepo4_full);
    RUN_TEST(test_profile_eval_12v_lifepo4_empty);
    RUN_TEST(test_profile_eval_external_always_100);
    RUN_TEST(test_profile_parse_custom_csv_valid);
    RUN_TEST(test_profile_parse_custom_json_array_valid);
    RUN_TEST(test_profile_parse_custom_too_few_points);
    RUN_TEST(test_profile_parse_custom_voltages_not_decreasing);
    RUN_TEST(test_profile_parse_custom_percents_not_decreasing);
    RUN_TEST(test_profile_parse_custom_voltage_out_of_range);
    RUN_TEST(test_profile_resolve_builtin);
    RUN_TEST(test_profile_resolve_external_not_valid);
    RUN_TEST(test_profile_resolve_custom_valid_points);
    RUN_TEST(test_profile_resolve_custom_invalid_fallback);
    // Phase 9c — sleep manager
    RUN_TEST(test_sleep_disabled_for_external_profile);
    RUN_TEST(test_sleep_disabled_for_zero_inactivity);
    RUN_TEST(test_sleep_enabled_for_battery_profile);
    RUN_TEST(test_sleep_idle_minutes_zero_immediately);
    RUN_TEST(test_sleep_idle_minutes_rounds_down);
    RUN_TEST(test_sleep_idle_minutes_full_minute);
    RUN_TEST(test_sleep_no_timeout_before_deadline);
    RUN_TEST(test_sleep_timeout_at_deadline);
    RUN_TEST(test_sleep_timeout_past_deadline);
    RUN_TEST(test_sleep_reset_by_switch_change);
    RUN_TEST(test_sleep_config_default_inactivity);
    RUN_TEST(test_boot_time_extends_millis_rollover);
    return UNITY_END();
}
