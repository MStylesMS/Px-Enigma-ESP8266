// commands.cpp — Phase 5 + Phase 8 command dispatcher.
// Phase 5: workspace-required commands (ping, getState, identify, restart,
//          reloadConfig).
// Phase 8: project commands per spec §11.2 (setBrightness, setTarget,
//          clearTarget, setMode, reset, getCode, setBatteryProfile,
//          setSignalIndicator, on, off).
#include "commands.h"
#include "display_mgr.h"
#include "log.h"
#include "mqtt_mgr.h"

#include <ArduinoJson.h>
#include <string.h>

namespace commands {

static const char* TAG = "cmd";

static cfg::Config* s_cfg = nullptr;
static code_engine::CodeEngine* s_engine = nullptr;

static bool     s_identify_active = false;
static uint32_t s_identify_until  = 0;
static bool     s_restart_pending = false;
static uint32_t s_restart_at      = 0;
static bool     s_off             = false;

// ---------------------------------------------------------------------------
// Outcome event helpers (spec §11.4)
// ---------------------------------------------------------------------------

static void publish_received(const char* command, const char* request_id) {
    JsonDocument d;
    d["command"] = command;
    if (request_id) d["request_id"] = request_id;
    mqtt_mgr::publish_event("command", "command_received",
                            nullptr, d.as<JsonVariantConst>());
}

static void publish_outcome(const char* outcome, const char* command,
                            const char* request_id, const char* warning_or_msg,
                            JsonVariantConst extra_data) {
    JsonDocument d;
    d["command"] = command;
    if (request_id) d["request_id"] = request_id;
    if (warning_or_msg) d["warning"] = warning_or_msg;
    if (!extra_data.isNull()) d["data"] = extra_data;
    mqtt_mgr::publish_event("command", outcome,
                            warning_or_msg, d.as<JsonVariantConst>());
}

// ---------------------------------------------------------------------------
// Local actions
// ---------------------------------------------------------------------------

void begin(cfg::Config* c, code_engine::CodeEngine* engine) {
    s_cfg = c;
    s_engine = engine;
}

void sync_engine_from_config() {
    if (!s_cfg || !s_engine) return;

    if (s_cfg->puzzle_mode == cfg::PUZZLE_MODE_LATCHING) {
        s_engine->set_mode(code_engine::Mode::Latching);
    } else {
        s_engine->set_mode(code_engine::Mode::Live);
    }

    if (s_cfg->puzzle_has_target) {
        uint32_t tgt = 0;
        if (code_engine::parse_target(s_cfg->puzzle_target.c_str(), &tgt)) {
            s_engine->set_target(true, tgt);
        } else {
            s_engine->set_target(false, 0);
            s_cfg->puzzle_has_target = false;
            s_cfg->puzzle_target = "";
            cfg::save(*s_cfg);
        }
    } else {
        s_engine->set_target(false, 0);
    }

    mqtt_mgr::publish_state();
}

void reset_puzzle() {
    if (!s_engine) return;
    s_engine->reset();
    mqtt_mgr::publish_state();
    pxlog::info(TAG, "puzzle reset (unlatch)");
}

void identify() {
    s_identify_active = true;
    uint32_t dur = s_cfg ? s_cfg->puzzle_identify_duration_ms : 2000;
    s_identify_until = millis() + dur;
    pxlog::info(TAG, "identify (duration_ms=%u)", (unsigned)dur);
}

bool identify_active() { return s_identify_active; }

void schedule_restart(uint32_t delay_ms) {
    s_restart_pending = true;
    s_restart_at = millis() + delay_ms;
    pxlog::warn(TAG, "restart scheduled in %u ms", (unsigned)delay_ms);
}

bool is_off() { return s_off; }

void tick() {
    uint32_t now = millis();
    if (s_identify_active && (int32_t)(now - s_identify_until) >= 0) {
        s_identify_active = false;
    }
    if (s_restart_pending && (int32_t)(now - s_restart_at) >= 0) {
        pxlog::warn(TAG, "restarting now");
        ESP.restart();
    }
}

// ---------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------

void handle_command_payload(const uint8_t* payload, size_t len) {
    JsonDocument doc;
    DeserializationError de = deserializeJson(doc, payload, len);
    if (de) {
        // Spec §11.3: malformed JSON -> silently dropped at MQTT layer; one
        // invalid_payload warning per minute is published as a rate limit.
        // Rate limiting is enforced inside mqtt_mgr; here we just emit once.
        JsonDocument data; data["error"] = de.c_str();
        mqtt_mgr::publish_warning("invalid_payload",
                                  "command JSON parse failed",
                                  data.as<JsonVariantConst>());
        return;
    }
    if (!doc.is<JsonObject>()) {
        mqtt_mgr::publish_warning("invalid_payload",
                                  "command payload is not a JSON object",
                                  JsonVariantConst());
        return;
    }

    const char* cmd = doc["command"] | (const char*)nullptr;
    const char* req = doc["request_id"] | (const char*)nullptr;
    if (!cmd) {
        mqtt_mgr::publish_warning("invalid_payload",
                                  "missing 'command' field",
                                  JsonVariantConst());
        return;
    }

    pxlog::info(TAG, "received command=%s request_id=%s",
                cmd, req ? req : "(none)");
    publish_received(cmd, req);

    // ---- Workspace-required commands ----
    if (!strcmp(cmd, "ping")) {
        publish_outcome("command_success", cmd, req, nullptr, JsonVariantConst());
        // Spec §12.2 also lists a "pong" event response.
        JsonDocument d;
        if (req) d["request_id"] = req;
        mqtt_mgr::publish_event("system", "pong", nullptr, d.as<JsonVariantConst>());
        return;
    }
    if (!strcmp(cmd, "getState")) {
        mqtt_mgr::publish_state();
        publish_outcome("command_success", cmd, req, nullptr, JsonVariantConst());
        return;
    }
    if (!strcmp(cmd, "identify")) {
        identify();
        publish_outcome("command_success", cmd, req, nullptr, JsonVariantConst());
        return;
    }
    if (!strcmp(cmd, "restart")) {
        publish_outcome("command_success", cmd, req, "restarting", JsonVariantConst());
        schedule_restart(500);
        return;
    }
    if (!strcmp(cmd, "reloadConfig")) {
        // Reset the running RAM config back to whatever is in /config.json,
        // without rebooting. Any retained MQTT override that was previously
        // deep-merged is dropped from RAM (the broker still has it; re-applying
        // it would require a reconnect or a fresh retained publish).
        if (!s_cfg) {
            publish_outcome("command_failed", cmd, req, "not_initialised",
                            JsonVariantConst());
            return;
        }
        bool was_invalid = false;
        cfg::load(*s_cfg, was_invalid);
        JsonDocument data;
        data["was_invalid"] = was_invalid;
        if (was_invalid) {
            mqtt_mgr::note_config_invalid_pending();
        }
        publish_outcome("command_success", cmd, req,
                        was_invalid ? "config_invalid" : nullptr,
                        data.as<JsonVariantConst>());
        // Echo a fresh state so subscribers see the reverted values.
        mqtt_mgr::publish_state();
        return;
    }

    // -----------------------------------------------------------------------
    // Phase 8 — project commands
    // -----------------------------------------------------------------------

    auto fail_no_init = [&](){
        publish_outcome("command_failed", cmd, req, "not_initialised",
                        JsonVariantConst());
    };
    auto fail_invalid_arg = [&](const char* field){
        JsonDocument data; data["field"] = field;
        publish_outcome("command_failed", cmd, req, "invalid_argument",
                        data.as<JsonVariantConst>());
    };

    if (!strcmp(cmd, "setBrightness")) {
        if (!s_cfg) { fail_no_init(); return; }
        if (!doc["brightness"].is<int>()) { fail_invalid_arg("brightness"); return; }
        int b = doc["brightness"].as<int>();
        if (b < 0 || b > 15) { fail_invalid_arg("brightness"); return; }
        bool persist = doc["persist"] | true;
        s_cfg->display_brightness = (uint8_t)b;
        display_mgr::set_brightness((uint8_t)b);
        if (persist) cfg::save(*s_cfg);
        publish_outcome("command_success", cmd, req, nullptr, JsonVariantConst());
        mqtt_mgr::publish_state();
        return;
    }

    if (!strcmp(cmd, "setTarget")) {
        if (!s_cfg || !s_engine) { fail_no_init(); return; }
        // Accept null target (== clearTarget) per spec §11.2.
        if (doc["target"].isNull()) {
            s_engine->set_target(false, 0);
            s_cfg->puzzle_has_target = false;
            s_cfg->puzzle_target = "";
            cfg::save(*s_cfg);
            publish_outcome("command_success", cmd, req, nullptr, JsonVariantConst());
            mqtt_mgr::publish_state();
            return;
        }
        const char* tgt_s = doc["target"] | (const char*)nullptr;
        if (!tgt_s) { fail_invalid_arg("target"); return; }
        uint32_t tgt = 0;
        if (!code_engine::parse_target(tgt_s, &tgt)) {
            JsonDocument data; data["field"] = "target"; data["value"] = tgt_s;
            publish_outcome("command_failed", cmd, req, "invalid_code_format",
                            data.as<JsonVariantConst>());
            return;
        }
        s_engine->set_target(true, tgt);
        // Persist canonical "XX-YY-ZZ" form (matches spec §5.4).
        char buf[9];
        code_engine::format_code(tgt, buf);
        s_cfg->puzzle_has_target = true;
        s_cfg->puzzle_target = buf;
        cfg::save(*s_cfg);
        publish_outcome("command_success", cmd, req, nullptr, JsonVariantConst());
        mqtt_mgr::publish_state();
        return;
    }

    if (!strcmp(cmd, "clearTarget")) {
        if (!s_cfg || !s_engine) { fail_no_init(); return; }
        s_engine->set_target(false, 0);
        s_cfg->puzzle_has_target = false;
        s_cfg->puzzle_target = "";
        cfg::save(*s_cfg);
        publish_outcome("command_success", cmd, req, nullptr, JsonVariantConst());
        mqtt_mgr::publish_state();
        return;
    }

    if (!strcmp(cmd, "setMode")) {
        if (!s_cfg || !s_engine) { fail_no_init(); return; }
        const char* m = doc["mode"] | (const char*)nullptr;
        if (!m) { fail_invalid_arg("mode"); return; }
        code_engine::Mode new_mode;
        if      (!strcmp(m, cfg::PUZZLE_MODE_LIVE))     new_mode = code_engine::Mode::Live;
        else if (!strcmp(m, cfg::PUZZLE_MODE_LATCHING)) new_mode = code_engine::Mode::Latching;
        else { fail_invalid_arg("mode"); return; }
        s_engine->set_mode(new_mode);
        s_cfg->puzzle_mode = m;
        cfg::save(*s_cfg);
        publish_outcome("command_success", cmd, req, nullptr, JsonVariantConst());
        mqtt_mgr::publish_state();
        return;
    }

    if (!strcmp(cmd, "reset")) {
        if (!s_engine) { fail_no_init(); return; }
        s_engine->reset();
        publish_outcome("command_success", cmd, req, nullptr, JsonVariantConst());
        mqtt_mgr::publish_state();
        return;
    }

    if (!strcmp(cmd, "getCode")) {
        if (!s_engine) { fail_no_init(); return; }
        const auto& s = s_engine->state();
        JsonDocument data;
        data["code"]      = s.code_str;
        data["code_int"]  = s.code_int;
        data["code_bits"] = s.code_bits;
        mqtt_mgr::publish_event("code", "code_changed", nullptr,
                                data.as<JsonVariantConst>());
        publish_outcome("command_success", cmd, req, nullptr, JsonVariantConst());
        return;
    }

    if (!strcmp(cmd, "setBatteryProfile")) {
        if (!s_cfg) { fail_no_init(); return; }
        const char* p = doc["profile"] | (const char*)nullptr;
        if (!p) { fail_invalid_arg("profile"); return; }
        // Accept the seven spec'd profile names.
        bool known =
            !strcmp(p, cfg::BATT_PROFILE_EXTERNAL)    ||
            !strcmp(p, cfg::BATT_PROFILE_UNKNOWN)     ||
            !strcmp(p, cfg::BATT_PROFILE_12V_LEAD)    ||
            !strcmp(p, cfg::BATT_PROFILE_12V_LIFEPO4) ||
            !strcmp(p, cfg::BATT_PROFILE_6V_LEAD)     ||
            !strcmp(p, cfg::BATT_PROFILE_6V_LIFEPO4)  ||
            !strcmp(p, cfg::BATT_PROFILE_CUSTOM);
        if (!known) { fail_invalid_arg("profile"); return; }
        s_cfg->battery_profile = p;
        // `points` field accepted as opaque string for now (full curve
        // validation lands in Phase 9b). Accept either string or array form.
        if (!doc["points"].isNull()) {
            if (doc["points"].is<const char*>()) {
                s_cfg->battery_points = doc["points"].as<const char*>();
            } else if (doc["points"].is<JsonArrayConst>()) {
                String s; serializeJson(doc["points"], s);
                s_cfg->battery_points = s;
            }
        }
        cfg::save(*s_cfg);
        publish_outcome("command_success", cmd, req, nullptr, JsonVariantConst());
        mqtt_mgr::publish_state();
        return;
    }

    if (!strcmp(cmd, "setSignalIndicator")) {
        if (!s_cfg) { fail_no_init(); return; }
        if (!doc["enabled"].is<bool>()) { fail_invalid_arg("enabled"); return; }
        s_cfg->signal_indicator_enabled = doc["enabled"].as<bool>();
        if (doc["rssi_dbm"].is<JsonArrayConst>()) {
            JsonArrayConst arr = doc["rssi_dbm"].as<JsonArrayConst>();
            if (arr.size() != cfg::RSSI_THRESHOLDS) {
                fail_invalid_arg("rssi_dbm"); return;
            }
            // Must be strictly decreasing and within int8 range.
            int8_t tmp[cfg::RSSI_THRESHOLDS];
            int prev = 0;
            size_t i = 0;
            for (JsonVariantConst v : arr) {
                if (!v.is<int>()) { fail_invalid_arg("rssi_dbm"); return; }
                int n = v.as<int>();
                if (n < -127 || n > 0) { fail_invalid_arg("rssi_dbm"); return; }
                if (i > 0 && n >= prev) { fail_invalid_arg("rssi_dbm"); return; }
                tmp[i++] = (int8_t)n;
                prev = n;
            }
            for (size_t j = 0; j < cfg::RSSI_THRESHOLDS; ++j)
                s_cfg->signal_rssi_dbm[j] = tmp[j];
        }
        cfg::save(*s_cfg);
        publish_outcome("command_success", cmd, req, nullptr, JsonVariantConst());
        mqtt_mgr::publish_state();
        return;
    }

    if (!strcmp(cmd, "on")) {
        s_off = false;
        publish_outcome("command_success", cmd, req, nullptr, JsonVariantConst());
        mqtt_mgr::publish_state();
        return;
    }
    if (!strcmp(cmd, "off")) {
        s_off = true;
        publish_outcome("command_success", cmd, req, nullptr, JsonVariantConst());
        mqtt_mgr::publish_state();
        return;
    }

    // ---- Unknown ----
    pxlog::warn(TAG, "unknown command: %s", cmd);
    JsonDocument data; data["received"] = cmd;
    publish_outcome("command_failed", cmd, req, "unknown_command",
                    data.as<JsonVariantConst>());
}

} // namespace commands
