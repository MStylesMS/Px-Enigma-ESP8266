// state.cpp
#include "state.h"
#include "log.h"
#include "wifi_mgr.h"

#include <ESP8266WiFi.h>

namespace appstate {

static uint32_t s_min_free_heap_bytes = UINT32_MAX;
static bool     s_mqtt_connected      = false;

void reset_heap_watermark() { s_min_free_heap_bytes = UINT32_MAX; }
void set_mqtt_connected(bool v) { s_mqtt_connected = v; }

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
    puzzle["latched"] = false;  // Phase 7

    // Code engine (Phase 7) not implemented yet — emit nulls but include the
    // configured target so operators can see what the device will be matching.
    JsonObject code_obj = out["code"].to<JsonObject>();
    code_obj["code"]      = nullptr;
    code_obj["code_int"]  = nullptr;
    code_obj["code_bits"] = nullptr;
    if (c.puzzle_has_target) code_obj["target"] = c.puzzle_target.c_str();
    else                     code_obj["target"] = nullptr;
    code_obj["solved"]    = false;

    JsonObject disp = out["display"].to<JsonObject>();
    disp["brightness"]       = c.display_brightness;
    disp["blanked"]          = false;
    disp["signal_indicator"] = c.signal_indicator_enabled;

    // Battery (Phase 9b) not implemented yet.
    JsonObject batt = out["battery"].to<JsonObject>();
    batt["profile"]   = c.battery_profile;
    batt["voltage_v"] = nullptr;
    batt["percent"]   = nullptr;
    batt["status"]    = "external";

    JsonObject sleep = out["sleep"].to<JsonObject>();
    sleep["inactivity_minutes"] = c.battery_inactivity_minutes;
    sleep["idle_minutes"]       = nullptr;
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
