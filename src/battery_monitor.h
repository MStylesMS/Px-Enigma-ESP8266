// battery_monitor.h — A0 voltage sampling and two-point calibration.
//
// Phase 9: raw ADC reading + calibrated voltage_v only.
// Percent / discharge curves land in Phase 9b.
#pragma once

#include "config.h"

namespace battery_monitor {

// Initialise with calibration constants from config.
// Must be called in setup() after cfg::load().
void begin(const cfg::Config& c);

// Cooperative tick — sample A0 on battery_sample_interval_ms timer.
void tick();

// Voltage in volts computed from the last ADC reading + calibration.
// Returns NaN (isnan() == true) before the first sample.
float voltage_v();

// Raw 10-bit ADC value from the last sample (0..1023).
// Returns -1 before the first sample.
int   raw_adc();

} // namespace battery_monitor
