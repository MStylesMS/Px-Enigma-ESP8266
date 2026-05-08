// commands.cpp — Phase 5: workspace-required commands only.
#include "commands.h"
#include "log.h"
#include "mqtt_mgr.h"

#include <ArduinoJson.h>
#include <string.h>

namespace commands {

static const char* TAG = "cmd";

static cfg::Config* s_cfg = nullptr;

static bool     s_identify_active = false;
static uint32_t s_identify_until  = 0;
static bool     s_restart_pending = false;
static uint32_t s_restart_at      = 0;

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

void begin(cfg::Config* c) { s_cfg = c; }

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

    // ---- Unknown ----
    pxlog::warn(TAG, "unknown command: %s", cmd);
    JsonDocument data; data["received"] = cmd;
    publish_outcome("command_failed", cmd, req, "unknown_command",
                    data.as<JsonVariantConst>());
}

} // namespace commands
