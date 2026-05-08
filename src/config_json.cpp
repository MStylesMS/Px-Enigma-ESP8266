// config_json.cpp — Platform-independent JSON serialization for cfg::Config.
//
// Implements load_defaults(), from_json(), to_json().
// No LittleFS, WiFi, or logging dependencies — safe to include directly in
// native unit tests.
#include "config.h"

#include <string.h>  // strcmp, memcpy
#include <stdio.h>   // snprintf

namespace cfg {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Assign JSON string to dst only when the variant is a non-null string.
static void opt_str(JsonVariantConst v, String& dst) {
    if (!v.isNull()) {
        const char* s = v.as<const char*>();
        dst = s ? s : "";
    }
}

// ---------------------------------------------------------------------------
// load_defaults
// ---------------------------------------------------------------------------

void load_defaults(Config& c) {
    c.prop_name = "px-enigma";
    c.instance  = "enigma1";

    c.wifi_primary = {"", ""};
    c.wifi_backup  = {"", ""};
    c.ap_password  = "MCEscher";

    c.mqtt_host                  = "";
    c.mqtt_port                  = 1883;
    c.mqtt_username              = "";
    c.mqtt_password              = "";
    c.mqtt_base_topic            = "paradox/enigma1";
    c.mqtt_announce_topic        = "paradox/props";
    c.mqtt_heartbeat_interval_ms = HEARTBEAT_INTERVAL_MS;

    c.puzzle_mode                 = PUZZLE_MODE_LIVE;
    c.puzzle_target               = "";
    c.puzzle_has_target           = false;
    c.puzzle_identify_duration_ms = IDENTIFY_DURATION_MS;
    c.puzzle_start_state          = START_STATE_ACTIVE;

    c.display_brightness = DISPLAY_BRIGHTNESS_DEFAULT;

    c.signal_indicator_enabled = true;
    const int8_t default_rssi[RSSI_THRESHOLDS] = {-55, -60, -65, -70, -75, -80, -85};
    memcpy(c.signal_rssi_dbm, default_rssi, sizeof(default_rssi));

    c.battery_profile               = BATT_PROFILE_EXTERNAL;
    c.battery_points                = "";
    c.battery_low_percent           = 40;
    c.battery_cutoff_percent        = 10;
    c.battery_hysteresis_pct        = 5;
    c.battery_sample_interval_ms    = 10000;
    c.battery_inactivity_minutes    = 60;
    c.battery_adc_at_0v_raw         = 0;
    c.battery_adc_at_full_v_raw     = 1023;
    c.battery_adc_full_v            = 15.0f;

    c.scan_poll_interval_ms  = 10;
    c.scan_debounce_samples  = 4;
}

// ---------------------------------------------------------------------------
// from_json — merge doc into c (overlay semantics)
// ---------------------------------------------------------------------------

bool from_json(Config& c, const JsonDocument& in, String* err_out) {

    // device
    JsonVariantConst dev = in["device"];
    if (!dev.isNull()) {
        opt_str(dev["prop_name"], c.prop_name);
        opt_str(dev["instance"],  c.instance);
    }

    // wifi
    JsonVariantConst wifi = in["wifi"];
    if (!wifi.isNull()) {
        JsonVariantConst pri = wifi["primary"];
        if (!pri.isNull()) {
            opt_str(pri["ssid"],     c.wifi_primary.ssid);
            opt_str(pri["password"], c.wifi_primary.password);
        }
        JsonVariantConst bak = wifi["backup"];
        if (!bak.isNull()) {
            opt_str(bak["ssid"],     c.wifi_backup.ssid);
            opt_str(bak["password"], c.wifi_backup.password);
        }
        JsonVariantConst ap = wifi["ap"];
        if (!ap.isNull()) {
            opt_str(ap["password"], c.ap_password);
        }
    }

    // mqtt
    JsonVariantConst mqtt = in["mqtt"];
    if (!mqtt.isNull()) {
        opt_str(mqtt["host"],           c.mqtt_host);
        opt_str(mqtt["username"],       c.mqtt_username);
        opt_str(mqtt["password"],       c.mqtt_password);
        opt_str(mqtt["base_topic"],     c.mqtt_base_topic);
        opt_str(mqtt["announce_topic"], c.mqtt_announce_topic);
        if (!mqtt["port"].isNull()) {
            int p = mqtt["port"].as<int>();
            if (p < 1 || p > 65535) {
                if (err_out) *err_out = "mqtt.port out of range (1..65535)";
                return false;
            }
            c.mqtt_port = static_cast<uint16_t>(p);
        }
        if (!mqtt["heartbeat_interval_ms"].isNull()) {
            long v = mqtt["heartbeat_interval_ms"].as<long>();
            if (v < 1000 || v > 3600000) {
                if (err_out) *err_out = "mqtt.heartbeat_interval_ms out of range (1000..3600000)";
                return false;
            }
            c.mqtt_heartbeat_interval_ms = static_cast<uint32_t>(v);
        }
    }

    // puzzle
    JsonVariantConst puz = in["puzzle"];
    if (!puz.isNull()) {
        if (!puz["mode"].isNull()) {
            const char* m = puz["mode"].as<const char*>();
            if (!m || (strcmp(m, PUZZLE_MODE_LIVE) != 0 && strcmp(m, PUZZLE_MODE_LATCHING) != 0)) {
                if (err_out) *err_out = "puzzle.mode must be 'live' or 'latching'";
                return false;
            }
            c.puzzle_mode = m;
        }
        // target: null → no target; integer or string → target string
        JsonVariantConst tgt = puz["target"];
        if (tgt.isNull()) {
            c.puzzle_target     = "";
            c.puzzle_has_target = false;
        } else if (tgt.is<int>()) {
            char buf[9];
            snprintf(buf, sizeof(buf), "%d", tgt.as<int>());
            c.puzzle_target     = buf;
            c.puzzle_has_target = true;
        } else {
            const char* t = tgt.as<const char*>();
            c.puzzle_target     = t ? t : "";
            c.puzzle_has_target = (c.puzzle_target.length() > 0);
        }
        if (!puz["identify_duration_ms"].isNull()) {
            long v = puz["identify_duration_ms"].as<long>();
            if (v < 100 || v > 30000) {
                if (err_out) *err_out = "puzzle.identify_duration_ms out of range (100..30000)";
                return false;
            }
            c.puzzle_identify_duration_ms = static_cast<uint32_t>(v);
        }
        if (!puz["start_state"].isNull()) {
            const char* s = puz["start_state"].as<const char*>();
            if (!s || (strcmp(s, START_STATE_ACTIVE) != 0 && strcmp(s, START_STATE_OFF) != 0)) {
                if (err_out) *err_out = "puzzle.start_state must be 'active' or 'off'";
                return false;
            }
            c.puzzle_start_state = s;
        }
    }

    // display
    JsonVariantConst disp = in["display"];
    if (!disp.isNull() && !disp["brightness"].isNull()) {
        int v = disp["brightness"].as<int>();
        if (v < 0 || v > 15) {
            if (err_out) *err_out = "display.brightness out of range (0..15)";
            return false;
        }
        c.display_brightness = static_cast<uint8_t>(v);
    }

    // signal_indicator
    JsonVariantConst si = in["signal_indicator"];
    if (!si.isNull()) {
        if (!si["enabled"].isNull()) {
            c.signal_indicator_enabled = si["enabled"].as<bool>();
        }
        JsonVariantConst rssi_v = si["rssi_dbm"];
        if (!rssi_v.isNull() && rssi_v.is<JsonArray>()) {
            JsonArrayConst arr = rssi_v.as<JsonArrayConst>();
            if (arr.size() == RSSI_THRESHOLDS) {
                for (size_t i = 0; i < RSSI_THRESHOLDS; ++i) {
                    c.signal_rssi_dbm[i] = arr[i].as<int8_t>();
                }
            }
        }
    }

    // battery
    static const char* const VALID_PROFILES[] = {
        BATT_PROFILE_EXTERNAL, BATT_PROFILE_UNKNOWN,
        BATT_PROFILE_12V_LEAD, BATT_PROFILE_12V_LIFEPO4,
        BATT_PROFILE_6V_LEAD,  BATT_PROFILE_6V_LIFEPO4,
        BATT_PROFILE_CUSTOM
    };
    static const size_t NUM_PROFILES = sizeof(VALID_PROFILES) / sizeof(VALID_PROFILES[0]);

    JsonVariantConst batt = in["battery"];
    if (!batt.isNull()) {
        if (!batt["profile"].isNull()) {
            const char* p = batt["profile"].as<const char*>();
            bool valid = false;
            if (p) {
                for (size_t i = 0; i < NUM_PROFILES; ++i) {
                    if (strcmp(p, VALID_PROFILES[i]) == 0) { valid = true; break; }
                }
            }
            if (!valid) {
                if (err_out) *err_out = "battery.profile unrecognized";
                return false;
            }
            c.battery_profile = p;
        }
        // points: null → ""; string → as-is; array → serialized JSON string
        JsonVariantConst pts = batt["points"];
        if (pts.isNull()) {
            c.battery_points = "";
        } else if (pts.is<JsonArray>()) {
            String serialized;
            serializeJson(pts, serialized);
            c.battery_points = serialized;
        } else {
            const char* s = pts.as<const char*>();
            c.battery_points = s ? s : "";
        }
        if (!batt["low_percent"].isNull()) {
            int v = batt["low_percent"].as<int>();
            if (v < 0 || v > 100) { if (err_out) *err_out = "battery.low_percent out of range (0..100)"; return false; }
            c.battery_low_percent = static_cast<uint8_t>(v);
        }
        if (!batt["cutoff_percent"].isNull()) {
            int v = batt["cutoff_percent"].as<int>();
            if (v < 0 || v > 100) { if (err_out) *err_out = "battery.cutoff_percent out of range (0..100)"; return false; }
            c.battery_cutoff_percent = static_cast<uint8_t>(v);
        }
        if (!batt["hysteresis_pct"].isNull()) {
            int v = batt["hysteresis_pct"].as<int>();
            if (v < 0 || v > 50) { if (err_out) *err_out = "battery.hysteresis_pct out of range (0..50)"; return false; }
            c.battery_hysteresis_pct = static_cast<uint8_t>(v);
        }
        if (!batt["sample_interval_ms"].isNull()) {
            long v = batt["sample_interval_ms"].as<long>();
            if (v < 1000 || v > 3600000) { if (err_out) *err_out = "battery.sample_interval_ms out of range (1000..3600000)"; return false; }
            c.battery_sample_interval_ms = static_cast<uint32_t>(v);
        }
        if (!batt["inactivity_minutes"].isNull()) {
            long v = batt["inactivity_minutes"].as<long>();
            if (v < 0 || v > 1440) { if (err_out) *err_out = "battery.inactivity_minutes out of range (0..1440)"; return false; }
            c.battery_inactivity_minutes = static_cast<uint16_t>(v);
        }
        if (!batt["adc_at_0v_raw"].isNull()) {
            int v = batt["adc_at_0v_raw"].as<int>();
            if (v < 0 || v > 1023) { if (err_out) *err_out = "battery.adc_at_0v_raw out of range (0..1023)"; return false; }
            c.battery_adc_at_0v_raw = static_cast<uint16_t>(v);
        }
        if (!batt["adc_at_full_v_raw"].isNull()) {
            int v = batt["adc_at_full_v_raw"].as<int>();
            if (v < 0 || v > 1023) { if (err_out) *err_out = "battery.adc_at_full_v_raw out of range (0..1023)"; return false; }
            c.battery_adc_at_full_v_raw = static_cast<uint16_t>(v);
        }
        if (!batt["adc_full_v"].isNull()) {
            float v = batt["adc_full_v"].as<float>();
            if (v < 1.0f || v > 25.0f) { if (err_out) *err_out = "battery.adc_full_v out of range (1.0..25.0)"; return false; }
            c.battery_adc_full_v = v;
        }
    }

    // scan
    JsonVariantConst scan = in["scan"];
    if (!scan.isNull()) {
        if (!scan["poll_interval_ms"].isNull()) {
            long v = scan["poll_interval_ms"].as<long>();
            if (v < 1 || v > 1000) { if (err_out) *err_out = "scan.poll_interval_ms out of range (1..1000)"; return false; }
            c.scan_poll_interval_ms = static_cast<uint16_t>(v);
        }
        if (!scan["debounce_samples"].isNull()) {
            int v = scan["debounce_samples"].as<int>();
            if (v < 1 || v > 32) { if (err_out) *err_out = "scan.debounce_samples out of range (1..32)"; return false; }
            c.scan_debounce_samples = static_cast<uint8_t>(v);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// to_json — serialize c to out (clears out first)
// ---------------------------------------------------------------------------

void to_json(const Config& c, JsonDocument& out) {
    out.clear();

    JsonObject device = out["device"].to<JsonObject>();
    device["prop_name"] = c.prop_name;
    device["instance"]  = c.instance;

    JsonObject wifi = out["wifi"].to<JsonObject>();
    JsonObject pri = wifi["primary"].to<JsonObject>();
    pri["ssid"]     = c.wifi_primary.ssid;
    pri["password"] = c.wifi_primary.password;
    JsonObject bak = wifi["backup"].to<JsonObject>();
    bak["ssid"]     = c.wifi_backup.ssid;
    bak["password"] = c.wifi_backup.password;
    JsonObject ap_obj = wifi["ap"].to<JsonObject>();
    ap_obj["password"] = c.ap_password;

    JsonObject mqtt = out["mqtt"].to<JsonObject>();
    mqtt["host"]                  = c.mqtt_host;
    mqtt["port"]                  = c.mqtt_port;
    mqtt["username"]              = c.mqtt_username;
    mqtt["password"]              = c.mqtt_password;
    mqtt["base_topic"]            = c.mqtt_base_topic;
    mqtt["announce_topic"]        = c.mqtt_announce_topic;
    mqtt["heartbeat_interval_ms"] = c.mqtt_heartbeat_interval_ms;

    JsonObject puz = out["puzzle"].to<JsonObject>();
    puz["mode"] = c.puzzle_mode;
    if (c.puzzle_has_target) {
        puz["target"] = c.puzzle_target;
    } else {
        puz["target"] = nullptr;
    }
    puz["identify_duration_ms"] = c.puzzle_identify_duration_ms;
    puz["start_state"]          = c.puzzle_start_state;

    JsonObject disp = out["display"].to<JsonObject>();
    disp["brightness"] = c.display_brightness;

    JsonObject si = out["signal_indicator"].to<JsonObject>();
    si["enabled"] = c.signal_indicator_enabled;
    JsonArray rssi_arr = si["rssi_dbm"].to<JsonArray>();
    for (size_t i = 0; i < RSSI_THRESHOLDS; ++i) {
        rssi_arr.add(c.signal_rssi_dbm[i]);
    }

    JsonObject batt = out["battery"].to<JsonObject>();
    batt["profile"] = c.battery_profile;
    if (c.battery_points.length() == 0) {
        batt["points"] = nullptr;
    } else {
        // Stored as either a CSV string or a serialized JSON array — always
        // write back as a JSON string; the battery monitor will parse the form.
        batt["points"] = c.battery_points;
    }
    batt["low_percent"]           = c.battery_low_percent;
    batt["cutoff_percent"]        = c.battery_cutoff_percent;
    batt["hysteresis_pct"]        = c.battery_hysteresis_pct;
    batt["sample_interval_ms"]    = c.battery_sample_interval_ms;
    batt["inactivity_minutes"]    = c.battery_inactivity_minutes;
    batt["adc_at_0v_raw"]         = c.battery_adc_at_0v_raw;
    batt["adc_at_full_v_raw"]     = c.battery_adc_at_full_v_raw;
    batt["adc_full_v"]            = c.battery_adc_full_v;

    JsonObject scan = out["scan"].to<JsonObject>();
    scan["poll_interval_ms"]  = c.scan_poll_interval_ms;
    scan["debounce_samples"]  = c.scan_debounce_samples;
}

} // namespace cfg
