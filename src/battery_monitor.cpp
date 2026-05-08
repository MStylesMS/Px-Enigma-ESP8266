// battery_monitor.cpp — A0 voltage sampling + two-point linear calibration.
//
// Phase 9: ADC reading + calibrated voltage only. Percent / curves Phase 9b.
//
// Calibration model:
//   voltage_v = slope * raw_adc + offset
//
//   slope  = battery_adc_full_v / (battery_adc_at_full_v_raw - battery_adc_at_0v_raw)
//   offset = -(slope * battery_adc_at_0v_raw)
//
// This fits the default values documented in hardware-spec §2:
//   V = 0.0531 * raw + 0.1978  (at_0v_raw=0, at_full_v_raw=~226.7, full_v=12.2 V)
#include "battery_monitor.h"
#include "log.h"

#include <Arduino.h>
#include <math.h>

namespace battery_monitor {

static const char* TAG = "batt";

static float    s_slope              = 0.0f;
static float    s_offset             = 0.0f;
static uint32_t s_sample_interval_ms = 5000;

static float    s_voltage_v  = NAN;
static int      s_raw_adc    = -1;
static uint32_t s_last_ms    = 0;
static bool     s_first_tick = true;

void begin(const cfg::Config& c) {
    s_sample_interval_ms = c.battery_sample_interval_ms
                           ? c.battery_sample_interval_ms : 5000;

    uint16_t at_0   = c.battery_adc_at_0v_raw;
    uint16_t at_full = c.battery_adc_at_full_v_raw;
    float    full_v  = c.battery_adc_full_v;

    if (at_full == at_0 || at_full == 0 || full_v <= 0.0f) {
        // Invalid calibration — use hardware-spec §2 legacy defaults.
        s_slope  = 0.0531f;
        s_offset = 0.1978f;
        pxlog::warn(TAG, "calibration invalid, using legacy defaults: V=0.0531*raw+0.1978");
    } else {
        s_slope  = full_v / (float)(at_full - at_0);
        s_offset = -(s_slope * (float)at_0);
    }

    pxlog::info(TAG, "init: slope=%.5f offset=%.4f interval_ms=%u",
                (double)s_slope, (double)s_offset, (unsigned)s_sample_interval_ms);
}

void tick() {
    uint32_t now = millis();
    if (!s_first_tick && (now - s_last_ms) < s_sample_interval_ms) return;

    s_last_ms    = now;
    s_first_tick = false;

    int raw = analogRead(pins::BATTERY_ADC);
    s_raw_adc   = raw;
    s_voltage_v = s_slope * (float)raw + s_offset;

    pxlog::info(TAG, "adc_raw=%d voltage_v=%.3f", raw, (double)s_voltage_v);
}

float voltage_v() { return s_voltage_v; }
int   raw_adc()   { return s_raw_adc; }

} // namespace battery_monitor
