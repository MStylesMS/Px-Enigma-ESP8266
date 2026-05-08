// battery_profiles.h — Built-in discharge curves + curve evaluator.
//
// Phase 9b: piecewise-linear interpolation over (voltage_v, percent) point
// arrays. Built-in profiles per functional-spec §6.3. Custom curve parser
// per spec §6.4 (CSV and object-array forms).
#pragma once

#include "config.h"
#include <stdint.h>

namespace battery_profiles {

// Maximum number of points in any curve (spec §6.4: max 20).
static constexpr uint8_t MAX_POINTS = 20;

struct CurvePoint {
    float   v;    // voltage (descending)
    uint8_t p;    // percent 0..100
};

// A resolved curve ready for evaluation.
struct Curve {
    CurvePoint points[MAX_POINTS];
    uint8_t    count = 0;
    bool       valid = false;   // false → treat as "external" (always 100 %)
};

// Evaluate the curve at a given voltage — piecewise-linear interpolation.
// Above highest point → 100.  Below lowest point → 0.
// Returns 100 if curve.valid == false (external / unknown).
uint8_t eval(const Curve& curve, float voltage_v);

// Load the named built-in profile into out_curve.
// Returns true if the profile name is recognised.
// external / unknown → out_curve.valid = false (caller returns 100 %).
bool load_builtin(const char* profile_name, Curve& out_curve);

// Parse battery.points (CSV or JSON object array) into out_curve.
// On parse / validation error writes a description to err_out (truncated to
// err_max-1 chars) and returns false.
// spec §6.4 validation: 2..20 points, voltages monotonically decreasing,
// percents monotonically decreasing, v in [1.0, 20.0], p in [0, 100].
bool parse_custom(const char* points_str, Curve& out_curve,
                  char* err_out, size_t err_max);

// High-level helper: given a full cfg::Config, populate out_curve for use
// across the firmware lifetime.  Falls back to unknown (valid=false) on any
// error and logs a warning.
void resolve(const cfg::Config& c, Curve& out_curve);

} // namespace battery_profiles
