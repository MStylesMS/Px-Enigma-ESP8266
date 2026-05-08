// test_native_smoke.cpp — Phase 0 + Phase 1 native smoke tests.
//
// Phase 0: verify toolchain, hardware constants.
// Phase 1: verify cfg::Config defaults, JSON round-trip, field validation.
//
// config_json.cpp is included directly (test_build_src = false for native);
// it has no LittleFS / WiFi / log dependencies.
#include <unity.h>

// Pull in config.h + config_json.cpp via the -I test/stubs build flag.
#include "../src/config.h"
#include "../src/config_json.cpp"

// Provide the EspClass and SerialStub instances that Arduino.h declares extern.
EspClass   ESP;
SerialStub Serial;

// millis() stub: return a fixed value sufficient for any timestamp check.
unsigned long millis() { return 1234000UL; }

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
    return UNITY_END();
}
