// mqtt_mgr.cpp
#include "mqtt_mgr.h"
#include "log.h"
#include "state.h"
#include "wifi_mgr.h"

#include <ESP8266WiFi.h>
#include <PubSubClient.h>

namespace mqtt_mgr {

static const char* TAG = "mqtt";

static cfg::Config* s_cfg  = nullptr;
static CommandCb    s_cb   = nullptr;
static void*        s_user = nullptr;

static WiFiClient   s_net;
static PubSubClient s_client(s_net);

static String   s_topic_commands;
static String   s_topic_config;
static uint32_t s_last_connect_attempt   = 0;
static uint32_t s_connect_backoff_ms     = 2000;
static uint32_t s_last_state_publish     = 0;
static bool     s_announced_this_connect = false;
static bool     s_config_invalid_pending = false;

// Rate-limited "invalid_payload" warning (spec §11.3).
static uint32_t s_last_invalid_payload_warn = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void write_iso_timestamp(char* out, size_t out_size) {
    uint32_t s = millis() / 1000;
    snprintf(out, out_size, "uptime+%lus", (unsigned long)s);
}

static bool publish_json(const char* topic, const JsonDocument& doc, bool retain = false) {
    if (!s_client.connected()) return false;
    size_t body_len = measureJson(doc);
    if (body_len + 2 >= MQTT_MAX_PACKET_SIZE) {
        pxlog::warn(TAG, "publish skipped: %s payload too large (%u bytes)",
                    topic, (unsigned)body_len);
        return false;
    }
    char body[MQTT_MAX_PACKET_SIZE];
    size_t written = serializeJson(doc, body, sizeof(body));
    bool ok = s_client.publish(topic, (const uint8_t*)body, written, retain);
    if (!ok) pxlog::warn(TAG, "publish failed: %s (%u bytes)", topic, (unsigned)written);
    return ok;
}

bool publish_state() {
    if (!s_cfg || !s_client.connected()) return false;
    JsonDocument doc;
    appstate::build_state(*s_cfg, doc);
    String topic = s_cfg->mqtt_base_topic + "/state";
    bool ok = publish_json(topic.c_str(), doc);
    if (ok) s_last_state_publish = millis();
    return ok;
}

bool publish_announce() {
    if (!s_cfg || !s_client.connected()) return false;
    if (s_cfg->mqtt_announce_topic.length() == 0) return false;
    JsonDocument doc;
    appstate::build_announce(*s_cfg, doc);
    return publish_json(s_cfg->mqtt_announce_topic.c_str(), doc);
}

bool publish_event(const char* type, const char* event, const char* message,
                   JsonVariantConst data) {
    if (!s_cfg || !s_client.connected()) return false;
    JsonDocument doc;
    char timestamp[40];
    write_iso_timestamp(timestamp, sizeof(timestamp));
    doc["timestamp"] = timestamp;
    doc["type"]      = type ? type : "system";
    doc["event"]     = event;
    if (message) doc["message"] = message;
    if (!data.isNull()) doc["data"] = data;
    String topic = s_cfg->mqtt_base_topic + "/events";
    return publish_json(topic.c_str(), doc);
}

bool publish_warning(const char* warning, const char* message,
                     JsonVariantConst data) {
    if (!s_cfg || !s_client.connected()) return false;
    JsonDocument doc;
    char timestamp[40];
    write_iso_timestamp(timestamp, sizeof(timestamp));
    doc["timestamp"] = timestamp;
    doc["warning"]   = warning;
    if (message) doc["message"] = message;
    if (!data.isNull()) doc["data"] = data;
    String topic = s_cfg->mqtt_base_topic + "/warnings";
    return publish_json(topic.c_str(), doc);
}

bool connected() { return s_client.connected(); }

void note_config_invalid_pending() { s_config_invalid_pending = true; }

// ---------------------------------------------------------------------------
// Retained config override (spec §10.1)
// ---------------------------------------------------------------------------

// Recursively deep-merge `src` into `dst`. Per spec §10.1:
//   - For each key in the override, walk into `dst`. If both sides are JSON
//     objects, recurse so unmentioned siblings are preserved (atomic per-leaf
//     semantics: overriding `display.brightness` does NOT wipe sibling
//     `display.signal_indicator.enabled`).
//   - Scalars and arrays in the override REPLACE the corresponding value in
//     `dst` outright (arrays are atomic — there is no element-wise merge).
//   - Keys collected into `merged_keys` track every leaf that was actually
//     written, formatted as dotted paths ("display.brightness", "wifi.primary.ssid").
static void deep_merge_into(JsonVariant dst, JsonVariantConst src,
                            const String& path, JsonArray merged_keys) {
    if (src.is<JsonObjectConst>() && dst.is<JsonObject>()) {
        JsonObject       dst_obj = dst.as<JsonObject>();
        JsonObjectConst  src_obj = src.as<JsonObjectConst>();
        for (JsonPairConst kv : src_obj) {
            const char* k = kv.key().c_str();
            String child_path = path.length() ? (path + "." + k) : String(k);
            if (kv.value().is<JsonObjectConst>() && dst_obj[k].is<JsonObject>()) {
                deep_merge_into(dst_obj[k], kv.value(), child_path, merged_keys);
            } else {
                // Scalar / array / new key / type-change: wholesale replace.
                dst_obj[k] = kv.value();
                merged_keys.add(child_path);
            }
        }
    } else {
        // Mixed-type at top level (shouldn't happen — config root is an object).
        dst.set(src);
        merged_keys.add(path);
    }
}

// Apply a retained override JSON to the running config in-RAM only. Per spec §10.1
// the override is **deep-merged**: only the leaf keys present in the payload are
// touched, every other field is preserved. The override is NOT persisted to
// /config.json — a hard reboot reloads the on-disk config and re-subscribes,
// which causes the broker to redeliver the still-retained payload.
static void apply_retained_override(const uint8_t* payload, size_t len) {
    if (!s_cfg) return;

    // Empty payload -> no-op at runtime. Per spec §10.1 an empty retained
    // payload means "no override is currently published"; it does NOT trigger
    // a reload of the running config. (The on-disk /config.json is what the
    // device will use on next boot anyway, so an empty payload simply means
    // "next boot, just keep what's in the config file".) To explicitly reset
    // the running RAM config back to /config.json without rebooting, use the
    // `reloadConfig` MQTT command.
    if (len == 0) {
        pxlog::info(TAG, "retained config override is empty (no-op)");
        return;
    }

    JsonDocument override_doc;
    DeserializationError de = deserializeJson(override_doc, payload, len);
    if (de) {
        JsonDocument data; data["error"] = de.c_str();
        publish_warning("invalid_payload", "config override JSON parse failed",
                        data.as<JsonVariantConst>());
        return;
    }
    if (!override_doc.is<JsonObject>()) {
        publish_warning("invalid_payload", "config override must be a JSON object",
                        JsonVariantConst());
        return;
    }

    // Build merged doc = current config, deep-merge override into it.
    JsonDocument merged;
    cfg::to_json(*s_cfg, merged);

    JsonDocument keys_doc;
    JsonArray keys_arr = keys_doc.to<JsonArray>();
    deep_merge_into(merged.as<JsonVariant>(),
                    override_doc.as<JsonVariantConst>(),
                    String(), keys_arr);

    // Re-validate the merged config via the normal schema path. Use a
    // defaults-initialised trial struct so any field the override managed to
    // delete (shouldn't be possible with merge, but defensive) reverts to a
    // safe default rather than carrying stale state.
    cfg::Config trial;
    cfg::load_defaults(trial);
    String err;
    if (!cfg::from_json(trial, merged, &err)) {
        JsonDocument data; data["error"] = err;
        publish_warning("config_invalid", "retained override rejected",
                        data.as<JsonVariantConst>());
        return;
    }
    *s_cfg = trial;

    JsonDocument event_data;
    event_data["keys"] = keys_arr;
    publish_event("config", "config_override_applied",
                  "retained override applied (in-RAM, not persisted)",
                  event_data.as<JsonVariantConst>());
}

// ---------------------------------------------------------------------------
// PubSubClient callback
// ---------------------------------------------------------------------------

static void on_msg(char* topic, uint8_t* payload, unsigned int len) {
    if (!topic) return;

    if (s_topic_config.length() && s_topic_config == topic) {
        apply_retained_override(payload, len);
        return;
    }
    if (s_topic_commands.length() && s_topic_commands == topic) {
        if (s_cb) s_cb(payload, len, s_user);
        return;
    }

    // Unknown topic — should not normally happen; rate-limited warning.
    uint32_t now = millis();
    if (now - s_last_invalid_payload_warn > 60000) {
        s_last_invalid_payload_warn = now;
        pxlog::warn(TAG, "msg on unexpected topic: %s", topic);
    }
}

// ---------------------------------------------------------------------------
// Connect
// ---------------------------------------------------------------------------

static bool try_connect() {
    if (!wifi_mgr::sta_connected())     return false;
    if (s_cfg->mqtt_host.length() == 0) return false;
    if (s_client.connected())           return true;

    s_client.setServer(s_cfg->mqtt_host.c_str(), s_cfg->mqtt_port);

    String client_id  = String("px-enigma-") + cfg::mac_suffix() + "-" + String(millis() % 100000);
    String will_topic = s_cfg->mqtt_base_topic + "/state";

    // LWT — simple offline marker.
    JsonDocument will;
    char timestamp[40];
    write_iso_timestamp(timestamp, sizeof(timestamp));
    will["timestamp"]   = timestamp;
    will["application"] = "px-enigma-esp8266";
    will["instance"]    = s_cfg->instance;
    will["status"]      = "offline";
    char will_body[256];
    size_t will_len = serializeJson(will, will_body, sizeof(will_body));

    bool ok;
    if (s_cfg->mqtt_username.length()) {
        ok = s_client.connect(client_id.c_str(),
                              s_cfg->mqtt_username.c_str(),
                              s_cfg->mqtt_password.c_str(),
                              will_topic.c_str(), 1, false,
                              will_body);
    } else {
        ok = s_client.connect(client_id.c_str(),
                              will_topic.c_str(), 1, false,
                              will_body);
    }
    (void)will_len;
    if (!ok) {
        pxlog::warn(TAG, "connect to %s:%u failed rc=%d (backoff=%ums)",
                    s_cfg->mqtt_host.c_str(), (unsigned)s_cfg->mqtt_port,
                    s_client.state(), (unsigned)s_connect_backoff_ms);
        return false;
    }

    pxlog::info(TAG, "connected to %s:%u as %s",
                s_cfg->mqtt_host.c_str(), (unsigned)s_cfg->mqtt_port, client_id.c_str());

    // Subscribe to commands + retained config override topic.
    s_topic_commands = s_cfg->mqtt_base_topic + "/commands";
    s_topic_config   = s_cfg->mqtt_base_topic + "/config";
    s_client.subscribe(s_topic_commands.c_str(), 1);
    s_client.subscribe(s_topic_config.c_str(),   1);
    pxlog::info(TAG, "sub %s", s_topic_commands.c_str());
    pxlog::info(TAG, "sub %s (retained override)", s_topic_config.c_str());

    s_announced_this_connect = false;
    s_connect_backoff_ms     = 2000;  // reset backoff on success
    appstate::set_mqtt_connected(true);
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void begin(cfg::Config* c, CommandCb cb, void* user) {
    s_cfg = c; s_cb = cb; s_user = user;
    s_client.setBufferSize(MQTT_MAX_PACKET_SIZE);
    s_client.setKeepAlive(MQTT_KEEPALIVE);
    s_client.setCallback(on_msg);
    appstate::set_mqtt_connected(false);
    pxlog::info(TAG, "mqtt_mgr ready (host=%s:%u base=%s)",
                c->mqtt_host.length() ? c->mqtt_host.c_str() : "(unset)",
                (unsigned)c->mqtt_port,
                c->mqtt_base_topic.c_str());
}

void loop() {
    if (!s_cfg) return;

    if (s_client.connected()) {
        s_client.loop();
    } else {
        appstate::set_mqtt_connected(false);
        uint32_t now = millis();
        if (now - s_last_connect_attempt >= s_connect_backoff_ms) {
            s_last_connect_attempt = now;
            if (!try_connect()) {
                // Exponential backoff up to 60 s.
                s_connect_backoff_ms = s_connect_backoff_ms * 2;
                if (s_connect_backoff_ms > 60000) s_connect_backoff_ms = 60000;
            }
        }
    }

    // Post-connect side effects: announce + initial state + pending warnings.
    if (s_client.connected() && !s_announced_this_connect) {
        s_announced_this_connect = true;
        publish_announce();
        publish_state();
        if (s_config_invalid_pending) {
            s_config_invalid_pending = false;
            publish_warning("config_invalid",
                            "Saved /config.json was invalid; defaults restored",
                            JsonVariantConst());
        }
    }

    // Periodic state heartbeat.
    if (s_client.connected() && s_cfg->mqtt_heartbeat_interval_ms > 0) {
        uint32_t now = millis();
        if (now - s_last_state_publish >= s_cfg->mqtt_heartbeat_interval_ms) {
            publish_state();
        }
    }
}

} // namespace mqtt_mgr
