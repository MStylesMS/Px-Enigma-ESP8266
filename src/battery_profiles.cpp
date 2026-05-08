// battery_profiles.cpp — built-in curves + piecewise-linear evaluator.
//
// Phase 9b. Built-in tables match functional-spec §6.3; numbers mirror the
// px-wifi-v1 sister project for later shared-library alignment.
#include "battery_profiles.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ArduinoJson.h>

namespace battery_profiles {

static const char* TAG = "batt";

// ---------------------------------------------------------------------------
// Built-in curve tables (spec §6.3) — voltage descending.
// ---------------------------------------------------------------------------

static const CurvePoint k12vLead[] = {
    {12.85f, 100}, {12.65f, 90}, {12.45f, 75}, {12.30f, 60},
    {12.10f, 40},  {11.95f, 20}, {11.80f,  5}, {11.60f,  0},
};
static const CurvePoint k12vLiFePO4[] = {
    {13.60f, 100}, {13.30f, 95}, {13.20f, 80}, {13.10f, 60},
    {13.00f, 40},  {12.90f, 20}, {12.50f, 10}, {11.20f,  0},
};
static const CurvePoint k6vLead[] = {
    {6.60f, 100}, {6.45f, 95}, {6.35f, 85}, {6.25f, 70},
    {6.15f, 55},  {6.05f, 35}, {5.95f, 15}, {5.85f,  0},
};
static const CurvePoint k6vLiFePO4[] = {
    {6.80f, 100}, {6.65f, 95}, {6.60f, 80}, {6.55f, 60},
    {6.50f, 40},  {6.45f, 20}, {6.30f, 10}, {5.60f,  0},
};

struct BuiltinEntry {
    const char*       name;
    const CurvePoint* pts;
    uint8_t           count;
};

static const BuiltinEntry kBuiltins[] = {
    { cfg::BATT_PROFILE_12V_LEAD,    k12vLead,    8 },
    { cfg::BATT_PROFILE_12V_LIFEPO4, k12vLiFePO4, 8 },
    { cfg::BATT_PROFILE_6V_LEAD,     k6vLead,     8 },
    { cfg::BATT_PROFILE_6V_LIFEPO4,  k6vLiFePO4,  8 },
};

// ---------------------------------------------------------------------------
// Evaluator
// ---------------------------------------------------------------------------

uint8_t eval(const Curve& curve, float voltage_v) {
    if (!curve.valid || curve.count == 0) return 100;
    if (voltage_v >= curve.points[0].v) return 100;
    if (voltage_v <= curve.points[curve.count - 1].v) return 0;
    for (uint8_t i = 0; i + 1 < curve.count; ++i) {
        const CurvePoint& hi = curve.points[i];
        const CurvePoint& lo = curve.points[i + 1];
        if (voltage_v >= lo.v && voltage_v <= hi.v) {
            // Linear interpolation.
            float frac = (voltage_v - lo.v) / (hi.v - lo.v);
            return (uint8_t)(lo.p + frac * (hi.p - lo.p) + 0.5f);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Built-in loader
// ---------------------------------------------------------------------------

bool load_builtin(const char* profile_name, Curve& out) {
    // external / unknown → valid = false (caller shows 100 %).
    if (!strcmp(profile_name, cfg::BATT_PROFILE_EXTERNAL) ||
        !strcmp(profile_name, cfg::BATT_PROFILE_UNKNOWN)) {
        out = Curve{};
        return true;
    }
    for (const auto& e : kBuiltins) {
        if (!strcmp(profile_name, e.name)) {
            out.count = e.count;
            memcpy(out.points, e.pts, e.count * sizeof(CurvePoint));
            out.valid = true;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Custom curve parser
// ---------------------------------------------------------------------------

// Parse a single "v:p" token into a CurvePoint.
static bool parse_vp_token(const char* tok, CurvePoint& pt) {
    char* end;
    float v = strtof(tok, &end);
    if (end == tok || *end != ':') return false;
    const char* pstr = end + 1;
    long p = strtol(pstr, &end, 10);
    if (end == pstr) return false;
    pt.v = v;
    pt.p = (uint8_t)(p < 0 ? 0 : (p > 100 ? 100 : p));
    return true;
}

// Validate a filled Curve (spec §6.4).
static bool validate_curve(const Curve& c, char* err, size_t err_max) {
    if (c.count < 2) {
        snprintf(err, err_max, "at least 2 points required (got %u)", c.count);
        return false;
    }
    for (uint8_t i = 0; i < c.count; ++i) {
        if (c.points[i].v < 1.0f || c.points[i].v > 20.0f) {
            snprintf(err, err_max, "point[%u] voltage %.2f out of range [1.0, 20.0]",
                     i, (double)c.points[i].v);
            return false;
        }
        if (c.points[i].p > 100) {
            snprintf(err, err_max, "point[%u] percent %u out of [0, 100]",
                     i, c.points[i].p);
            return false;
        }
    }
    for (uint8_t i = 1; i < c.count; ++i) {
        if (c.points[i].v >= c.points[i - 1].v) {
            snprintf(err, err_max, "voltages not monotonically decreasing at index %u", i);
            return false;
        }
        if (c.points[i].p >= c.points[i - 1].p) {
            snprintf(err, err_max, "percents not monotonically decreasing at index %u", i);
            return false;
        }
    }
    return true;
}

bool parse_custom(const char* points_str, Curve& out, char* err, size_t err_max) {
    if (!points_str || !*points_str) {
        snprintf(err, err_max, "empty points string");
        return false;
    }

    out = Curve{};

    // Detect JSON array vs CSV by first non-whitespace character.
    const char* p = points_str;
    while (*p == ' ' || *p == '\t') ++p;

    if (*p == '[') {
        // JSON object-array form: [{"v":12.85,"p":100},...]
        JsonDocument doc;
        DeserializationError je = deserializeJson(doc, points_str);
        if (je != DeserializationError::Ok) {
            snprintf(err, err_max, "JSON parse error: %s", je.c_str());
            return false;
        }
        JsonArrayConst arr = doc.as<JsonArrayConst>();
        if (arr.isNull()) {
            snprintf(err, err_max, "expected a JSON array");
            return false;
        }
        for (JsonObjectConst obj : arr) {
            if (out.count >= MAX_POINTS) {
                snprintf(err, err_max, "too many points (max %u)", MAX_POINTS);
                return false;
            }
            if (!obj["v"].is<float>() && !obj["v"].is<int>()) {
                snprintf(err, err_max, "point[%u] missing or non-numeric 'v'", out.count);
                return false;
            }
            if (!obj["p"].is<int>() && !obj["p"].is<float>()) {
                snprintf(err, err_max, "point[%u] missing or non-numeric 'p'", out.count);
                return false;
            }
            out.points[out.count++] = { obj["v"].as<float>(), (uint8_t)obj["p"].as<int>() };
        }
    } else {
        // CSV form: "v:p,v:p,..."
        // Work on a mutable copy.
        char buf[512];
        strncpy(buf, points_str, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char* tok = strtok(buf, ",");
        while (tok) {
            if (out.count >= MAX_POINTS) {
                snprintf(err, err_max, "too many points (max %u)", MAX_POINTS);
                return false;
            }
            CurvePoint pt{};
            if (!parse_vp_token(tok, pt)) {
                snprintf(err, err_max, "invalid token '%s'", tok);
                return false;
            }
            out.points[out.count++] = pt;
            tok = strtok(nullptr, ",");
        }
    }

    if (!validate_curve(out, err, err_max)) return false;

    out.valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// High-level resolver
// ---------------------------------------------------------------------------

void resolve(const cfg::Config& c, Curve& out) {
    const char* profile = c.battery_profile.c_str();

    if (!strcmp(profile, cfg::BATT_PROFILE_CUSTOM)) {
        char err[128];
        if (parse_custom(c.battery_points.c_str(), out, err, sizeof(err))) {
            pxlog::info(TAG, "custom curve loaded: %u points", (unsigned)out.count);
        } else {
            out = Curve{};   // valid = false → caller uses 100 %
            pxlog::warn(TAG, "custom curve invalid (%s) — falling back to unknown", err);
        }
        return;
    }

    if (!load_builtin(profile, out)) {
        out = Curve{};
        pxlog::warn(TAG, "unknown profile '%s' — treating as unknown", profile);
        return;
    }

    if (out.valid) {
        pxlog::info(TAG, "profile '%s': %u points", profile, (unsigned)out.count);
    } else {
        pxlog::info(TAG, "profile '%s': capacity reporting disabled", profile);
    }
}

} // namespace battery_profiles
