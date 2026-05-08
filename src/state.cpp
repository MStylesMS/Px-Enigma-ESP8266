// state.cpp
#include "state.h"
#include "battery_monitor.h"
#include "sleep_mgr.h"
#include "log.h"
#include "wifi_mgr.h"

#include <ESP8266WiFi.h>

namespace appstate {

static uint32_t s_min_free_heap_bytes = UINT32_MAX;
static bool     s_mqtt_connected      = false;
static const code_engine::CodeState* s_code_state = nullptr;

void reset_heap_watermark() { s_min_free_heap_bytes = UINT32_MAX; }
void set_mqtt_connected(bool v) { s_mqtt_connected = v; }
bool mqtt_connected() { return s_mqtt_connected; }
void set_code_state(const code_engine::CodeState* cs) { s_code_state = cs; }

static void write_iso_timestamp(char* out, size_t out_size) {
    // No NTP yet; emit uptime-based marker (matches clock project convention).
    uint32_t s = millis() / 1000;
    snprintf(out, out_size, "uptime+%lus", (unsigned long)s);
}

void build_state(const cfg::Config& c, JsonDocument& out) {
    out.clear();

    char timestamp[40];
    write_iso_timestamp(timestamp, sizeof(timestamp));
    out["timestamp"]   = timestamp;
    out["application"] = "px-enigma-esp8266";
    out["instance"]    = c.instance;
    out["prop_name"]   = c.prop_name;
    out["version"]     = FW_VERSION;
    out["status"]      = "active";  // Phase 5: code-engine FSM not yet wired
    out["uptime_s"]    = millis() / 1000;

    uint32_t free_heap_bytes = ESP.getFreeHeap();
    if (s_min_free_heap_bytes == UINT32_MAX || free_heap_bytes < s_min_free_heap_bytes) {
        s_min_free_heap_bytes = free_heap_bytes;
    }

    JsonObject health = out["health"].to<JsonObject>();
    health["free_heap_bytes"]     = free_heap_bytes;
    health["min_free_heap_bytes"] = s_min_free_heap_bytes;

    JsonObject wifi = out["wifi"].to<JsonObject>();
    JsonObject sta = wifi["sta"].to<JsonObject>();
    sta["ssid"]      = wifi_mgr::sta_ssid();
    sta["rssi"]      = wifi_mgr::sta_rssi();
    sta["ip"]        = wifi_mgr::sta_ip();
    sta["connected"] = wifi_mgr::sta_connected();
    JsonObject ap = wifi["ap"].to<JsonObject>();
    ap["ssid"]    = wifi_mgr::ap_ssid();
    ap["ip"]      = wifi_mgr::ap_ip();
    ap["clients"] = wifi_mgr::ap_clients();

    JsonObject mqtt = out["mqtt"].to<JsonObject>();
    mqtt["connected"] = s_mqtt_connected;
    mqtt["broker"]    = c.mqtt_host + ":" + String(c.mqtt_port);

    JsonObject puzzle = out["puzzle"].to<JsonObject>();
    puzzle["mode"]    = c.puzzle_mode;
    puzzle["latched"] = s_code_state ? s_code_state->latched : false;

    JsonObject code_obj = out["code"].to<JsonObject>();
    if (s_code_state) {
        code_obj["code"]      = s_code_state->code_str;
        code_obj["code_int"]  = s_code_state->code_int;
        code_obj["code_bits"] = s_code_state->code_bits;
        if (s_code_state->has_target) code_obj["target"] = s_code_state->target_str;
        else                          code_obj["target"] = nullptr;
        code_obj["solved"]    = s_code_state->solved;
    } else {
        code_obj["code"]      = nullptr;
        code_obj["code_int"]  = nullptr;
        code_obj["code_bits"] = nullptr;
        if (c.puzzle_has_target) code_obj["target"] = c.puzzle_target.c_str();
        else                     code_obj["target"] = nullptr;
        code_obj["solved"]    = false;
    }

    JsonObject disp = out["display"].to<JsonObject>();
    disp["brightness"]       = c.display_brightness;
    disp["blanked"]          = false;
    disp["signal_indicator"] = c.signal_indicator_enabled;

    // Battery (Phase 9b: percent + status wired from battery_monitor).
    JsonObject batt = out["battery"].to<JsonObject>();
    batt["profile"]  = c.battery_profile;
    float v = battery_monitor::voltage_v();
    if (!isnan(v)) batt["voltage_v"] = v;
    else           batt["voltage_v"] = nullptr;
    batt["raw_a0"]  = (battery_monitor::raw_adc() >= 0)
                      ? (int)battery_monitor::raw_adc() : (int)0;
    int pct = battery_monitor::percent();
    if (pct >= 0 && !battery_monitor::is_external()) batt["percent"] = pct;
    else                                              batt["percent"] = nullptr;
    batt["status"]  = battery_monitor::status_str(battery_monitor::status());

    JsonObject slp = out["sleep"].to<JsonObject>();
    slp["inactivity_minutes"] = c.battery_inactivity_minutes;
    slp["idle_minutes"]       = sleep_mgr::enabled()
                                  ? sleep_mgr::idle_minutes()
                                  : (uint32_t)0;
}

void build_announce(const cfg::Config& c, JsonDocument& out) {
    out.clear();
    char timestamp[40];
    write_iso_timestamp(timestamp, sizeof(timestamp));
    out["timestamp"]   = timestamp;
    out["event"]       = "online";
    out["application"] = "px-enigma-esp8266";
    out["instance"]    = c.instance;
    out["prop_name"]   = c.prop_name;
    out["version"]     = FW_VERSION;
    out["ip"]          = wifi_mgr::sta_ip();
    out["ap_ip"]       = wifi_mgr::ap_ip();
    out["mdns"]        = wifi_mgr::mdns_hostname();
    out["mdns_fqdn"]   = wifi_mgr::mdns_fqdn();
    out["mac"]         = wifi_mgr::mac_address();
    out["base_topic"]  = c.mqtt_base_topic;
    out["commands_topic"] = c.mqtt_base_topic + "/commands";
    out["config_topic"]   = c.mqtt_base_topic + "/config";
}

} // namespace appstate
