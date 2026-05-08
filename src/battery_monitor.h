// battery_monitor.h — A0 voltage sampling, calibration, and discharge curves.
//
// Phase 9b: adds 4-sample trailing average, battery_profiles curve evaluator,
// percent reporting, and LOW_BATT / CRIT_BATT status transitions.
#pragma once

#include "config.h"
#include "battery_profiles.h"

namespace battery_monitor {

// Battery health state (spec §3 state machine, §6.5 thresholds).
enum class Status : uint8_t {
    External,   // external / unknown profile — capacity reporting disabled
    Ok,
    Low,        // below battery_low_percent
    Critical,   // below battery_cutoff_percent
};

// String form for JSON / MQTT (spec §6.6).
const char* status_str(Status s);

// Initialise: calibration, curve resolve, threshold setup.
// Must be called in setup() after cfg::load().
void begin(const cfg::Config& c);

// Cooperative tick — sample A0 on battery_sample_interval_ms timer,
// update 4-sample average, re-evaluate curve percent + status transitions.
void tick();

// Voltage in volts (4-sample trailing average, calibrated).
// Returns NaN before the first sample.
float voltage_v();

// Raw 10-bit ADC value most recently read from A0 (0..1023).
// Returns -1 before the first sample.
int   raw_adc();

// Battery capacity 0..100, or 100 when profile = external/unknown.
// Returns -1 before the first sample.
int   percent();

// Current health status.
Status status();

// True if the profile is external or unknown (sleep + percent disabled).
bool is_external();

} // namespace battery_monitor
