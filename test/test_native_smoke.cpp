// test_native_smoke.cpp — Phase 0 native smoke tests.
//
// These tests verify that:
//   - The native build toolchain is correctly wired (PlatformIO + Unity).
//   - src/config.h compiles cleanly on the host.
//   - The hardware constants declared in config.h match docs/pin-mapping.md.
//
// When later phases add testable modules, add corresponding test files in
// test/ (e.g., test_code_engine.cpp, test_switch_matrix.cpp). The smoke
// tests here are intentionally minimal and must always pass.
#include <unity.h>

// Pull in config.h via the -I test/stubs build flag (which provides Arduino.h).
#include "../src/config.h"

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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_i2c_pins);
    RUN_TEST(test_i2c_addresses);
    RUN_TEST(test_matrix_dimensions);
    RUN_TEST(test_matrix_col_pins);
    RUN_TEST(test_matrix_row_pins);
    RUN_TEST(test_display_defaults);
    RUN_TEST(test_log_constants);
    return UNITY_END();
}
