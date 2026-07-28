// state.cpp
#include "state.h"
#include "battery_monitor.h"
#include "boot_time.h"
#include "code_engine.h"
#include "sleep_mgr.h"
#include "log.h"
#include "wifi_mgr.h"

#include <ESP8266WiFi.h>

namespace appstate {

static uint32_t s_min_free_heap_bytes = UINT32_MAX;
static bool     s_mqtt_connected      = false;
static const code_engine::CodeState* s_code_state = nullptr;

// Grid layout (loaded from switch_layout.json at startup via set_grid_layout()).
static uint8_t s_ui_rows      = 4;
static uint8_t s_ui_cols      = 5;
static uint8_t s_switch_count = 20;
static int8_t  s_grid_bit[MAX_GRID_ROWS][MAX_GRID_COLS];

void set_grid_layout(uint8_t ui_rows, uint8_t ui_cols, uint8_t switch_count,
                     const int8_t grid_bit[MAX_GRID_ROWS][MAX_GRID_COLS]) {
    s_ui_rows      = (ui_rows <= MAX_GRID_ROWS) ? ui_rows : MAX_GRID_ROWS;
    s_ui_cols      = (ui_cols <= MAX_GRID_COLS) ? ui_cols : MAX_GRID_COLS;
    s_switch_count = switch_count;
    for (uint8_t r = 0; r < MAX_GRID_ROWS; ++r)
        for (uint8_t c = 0; c < MAX_GRID_COLS; ++c)
            s_grid_bit[r][c] = grid_bit[r][c];
}

void reset_heap_watermark() { s_min_free_heap_bytes = UINT32_MAX; }
void set_mqtt_connected(bool v) { s_mqtt_connected = v; }
bool mqtt_connected() { return s_mqtt_connected; }
void set_code_state(const code_engine::CodeState* cs) { s_code_state = cs; }
bool get_code_snapshot(uint32_t* code_bits, const char** code_str) {
    if (!s_code_state) return false;
    if (code_bits) *code_bits = s_code_state->code_bits;
    if (code_str) *code_str = s_code_state->code_str;
    return true;
}

void add_code_grid(JsonObject code_obj, uint32_t code_bits,
                   const char* array_key) {
    if (!array_key) array_key = "grid";
    JsonArray arr = code_obj[array_key].to<JsonArray>();
    char row_str[MAX_GRID_COLS + 1];
    for (uint8_t r = 0; r < s_ui_rows; ++r) {
        for (uint8_t col = 0; col < s_ui_cols; ++col) {
            uint8_t sw_num = (uint8_t)(r * s_ui_cols + col + 1);
            int8_t bit = s_grid_bit[r][col];
            if (sw_num > s_switch_count || bit < 0)
                row_str[col] = '-';
            else
                row_str[col] = ((code_bits >> (uint8_t)bit) & 1u) ? '1' : '0';
        }
        row_str[s_ui_cols] = '\0';
        arr.add(row_str);
    }
}

void build_state(const cfg::Config& c, JsonDocument& out) {
    out.clear();

    out["ts"]          = boot_time::milliseconds();
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
        code_obj["code"]     = s_code_state->code_str;
        code_obj["code_int"] = s_code_state->code_int;
        add_code_grid(code_obj, s_code_state->code_bits);
        if (s_code_state->has_target) {
            code_obj["target"] = s_code_state->target_str;
            add_code_grid(code_obj,
                          code_engine::target_matrix_bits(s_code_state->target_int),
                          "target_grid");
        } else {
            code_obj["target"]      = nullptr;
            code_obj["target_grid"] = nullptr;
        }
        code_obj["solved"]   = s_code_state->solved;
    } else {
        code_obj["code"]     = nullptr;
        code_obj["code_int"] = nullptr;
        code_obj["grid"]         = nullptr;
        code_obj["target_grid"]  = nullptr;
        if (c.puzzle_has_target) code_obj["target"] = c.puzzle_target.c_str();
        else                     code_obj["target"] = nullptr;
        code_obj["solved"]   = false;
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
    out["ts"]          = boot_time::milliseconds();
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
