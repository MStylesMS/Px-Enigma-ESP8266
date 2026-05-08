// battery_monitor.cpp — Phase 9b: ADC sampling, smoothing, curve eval, status.
//
// Calibration model (same as Phase 9):
//   voltage_v = slope * raw + offset
//
// Phase 9b additions:
//   - 4-sample trailing average (spec §6.1)
//   - battery_profiles curve evaluation → battery.percent
//   - LOW_BATT / CRIT_BATT status transitions with hysteresis (spec §6.5)
//   - status_str() for JSON reporting
#include "battery_monitor.h"
#include "battery_profiles.h"
#include "log.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

namespace battery_monitor {

static const char* TAG = "batt";

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static float    s_slope              = 0.0f;
static float    s_offset             = 0.0f;
static uint32_t s_sample_interval_ms = 5000;

// 4-sample trailing average ring buffer.
static constexpr uint8_t SMOOTH_N = 4;
static int      s_ring[SMOOTH_N] = {};
static uint8_t  s_ring_pos       = 0;
static uint8_t  s_ring_filled    = 0;    // samples collected so far, capped at SMOOTH_N

static float    s_voltage_v  = NAN;
static int      s_raw_adc    = -1;
static int      s_percent    = -1;       // -1 = not yet sampled
static Status   s_status     = Status::External;
static bool     s_is_external = true;

static uint8_t  s_low_thresh     = 40;
static uint8_t  s_cutoff_thresh  = 10;
static uint8_t  s_hysteresis     = 5;

static uint32_t s_last_ms    = 0;
static bool     s_first_tick = true;

static battery_profiles::Curve s_curve;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const char* status_str(Status s) {
    switch (s) {
        case Status::External:  return "external";
        case Status::Ok:        return "ok";
        case Status::Low:       return "low";
        case Status::Critical:  return "critical";
    }
    return "ok";
}

static float compute_average_voltage() {
    if (s_ring_filled == 0) return NAN;
    int sum = 0;
    for (uint8_t i = 0; i < s_ring_filled; ++i) sum += s_ring[i];
    float avg_raw = (float)sum / (float)s_ring_filled;
    return s_slope * avg_raw + s_offset;
}

static void update_status(int pct) {
    if (s_is_external) {
        s_status = Status::External;
        return;
    }
    // Hysteresis: transition to worse state immediately; require hysteresis to recover.
    switch (s_status) {
        case Status::External:
        case Status::Ok:
            if (pct <= (int)s_cutoff_thresh)
                s_status = Status::Critical;
            else if (pct <= (int)s_low_thresh)
                s_status = Status::Low;
            break;
        case Status::Low:
            if (pct <= (int)s_cutoff_thresh)
                s_status = Status::Critical;
            else if (pct >= (int)(s_low_thresh + s_hysteresis))
                s_status = Status::Ok;
            break;
        case Status::Critical:
            if (pct >= (int)(s_cutoff_thresh + s_hysteresis))
                s_status = (pct <= (int)s_low_thresh) ? Status::Low : Status::Ok;
            break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void begin(const cfg::Config& c) {
    s_sample_interval_ms = c.battery_sample_interval_ms
                           ? c.battery_sample_interval_ms : 5000;
    s_low_thresh     = c.battery_low_percent;
    s_cutoff_thresh  = c.battery_cutoff_percent;
    s_hysteresis     = c.battery_hysteresis_pct;

    // Calibration.
    uint16_t at_0    = c.battery_adc_at_0v_raw;
    uint16_t at_full = c.battery_adc_at_full_v_raw;
    float    full_v  = c.battery_adc_full_v;

    if (at_full == at_0 || at_full == 0 || full_v <= 0.0f) {
        s_slope  = 0.0531f;
        s_offset = 0.1978f;
        pxlog::warn(TAG, "calibration invalid, using legacy defaults: V=0.0531*raw+0.1978");
    } else {
        s_slope  = full_v / (float)(at_full - at_0);
        s_offset = -(s_slope * (float)at_0);
    }

    // Discharge curve.
    battery_profiles::resolve(c, s_curve);
    s_is_external = !s_curve.valid;
    s_status      = s_is_external ? Status::External : Status::Ok;

    // Reset smoothing ring.
    memset(s_ring, 0, sizeof(s_ring));
    s_ring_pos    = 0;
    s_ring_filled = 0;

    pxlog::info(TAG, "init: slope=%.5f offset=%.4f interval_ms=%u low=%u%% cutoff=%u%% hyst=%u%%",
                (double)s_slope, (double)s_offset, (unsigned)s_sample_interval_ms,
                (unsigned)s_low_thresh, (unsigned)s_cutoff_thresh, (unsigned)s_hysteresis);
}

void tick() {
    uint32_t now = millis();
    if (!s_first_tick && (now - s_last_ms) < s_sample_interval_ms) return;
    s_last_ms    = now;
    s_first_tick = false;

    int raw = analogRead(pins::BATTERY_ADC);
    s_raw_adc = raw;

    // Push into ring buffer.
    s_ring[s_ring_pos] = raw;
    s_ring_pos = (uint8_t)((s_ring_pos + 1) % SMOOTH_N);
    if (s_ring_filled < SMOOTH_N) ++s_ring_filled;

    s_voltage_v = compute_average_voltage();

    if (!isnan(s_voltage_v)) {
        int pct = s_is_external ? 100 : (int)battery_profiles::eval(s_curve, s_voltage_v);
        s_percent = pct;
        update_status(pct);
    }

    pxlog::info(TAG, "adc_raw=%d voltage_v=%.3f percent=%d status=%s",
                raw, (double)(isnan(s_voltage_v) ? 0.0f : s_voltage_v),
                s_percent, status_str(s_status));
}

float  voltage_v()  { return s_voltage_v; }
int    raw_adc()    { return s_raw_adc; }
int    percent()    { return s_is_external ? 100 : s_percent; }
Status status()     { return s_status; }
bool   is_external(){ return s_is_external; }

} // namespace battery_monitor
