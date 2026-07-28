// sleep_mgr.cpp — Inactivity sleep manager (spec §7).
//
// Phase 9c.
#include "sleep_mgr.h"
#include "battery_monitor.h"
#include "mqtt_mgr.h"
#include "log.h"

#include <Arduino.h>
#include <ArduinoJson.h>

namespace sleep_mgr {

static const char* TAG = "sleep";

static bool     s_enabled           = false;
static uint32_t s_timeout_ms        = 0;   // 0 means disabled
static uint32_t s_last_activity_ms  = 0;

// ---------------------------------------------------------------------------

void begin(const cfg::Config& c) {
    // Sleep is enabled for battery profiles only (not external / unknown).
    s_enabled = !battery_monitor::is_external()
                && (c.battery_inactivity_minutes > 0);

    if (s_enabled) {
        s_timeout_ms       = (uint32_t)c.battery_inactivity_minutes * 60UL * 1000UL;
        s_last_activity_ms = millis();
        pxlog::info(TAG, "sleep enabled: timeout=%u min", c.battery_inactivity_minutes);
    } else {
        s_timeout_ms = 0;
        pxlog::info(TAG, "sleep disabled (profile=%s inactivity=%u)",
                    c.battery_profile.c_str(), c.battery_inactivity_minutes);
    }
}

void loop(bool switch_changed) {
    if (!s_enabled) return;

    if (switch_changed) {
        s_last_activity_ms = millis();
        return;
    }

    uint32_t elapsed = millis() - s_last_activity_ms;
    if (elapsed >= s_timeout_ms) {
        uint32_t idle_min = elapsed / 60000UL;
        pxlog::info(TAG, "inactivity timeout — idle %u min, going to sleep", idle_min);

        // Publish going_to_sleep event (spec §12.2).
        StaticJsonDocument<96> doc;
        doc["idle_minutes"] = idle_min;
        mqtt_mgr::publish_event("system", "going_to_sleep",
                                "Inactivity timeout elapsed; entering deep sleep",
                                doc.as<JsonVariantConst>());

        pxlog::flush();

        ESP.deepSleep(0);
        // deepSleep does not return; hardware resets after wake.
    }
}

uint32_t idle_minutes() {
    if (!s_enabled) return 0;
    uint32_t elapsed = millis() - s_last_activity_ms;
    return elapsed / 60000UL;
}

bool enabled() { return s_enabled; }

void enter_sleep_now() {
    uint32_t idle_min = s_enabled ? idle_minutes() : 0;
    pxlog::info(TAG, "sleep command — entering deep sleep (idle %u min)", idle_min);

    StaticJsonDocument<96> doc;
    doc["idle_minutes"] = idle_min;
    doc["manual"]       = true;
    mqtt_mgr::publish_event("system", "going_to_sleep",
                            "Sleep command received; entering deep sleep",
                            doc.as<JsonVariantConst>());

    pxlog::flush();
    ESP.deepSleep(0);
}

} // namespace sleep_mgr
