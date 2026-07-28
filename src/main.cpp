// px-enigma-esp8266 — main.cpp
//
// Cooperative-loop firmware skeleton. Modules are wired in here as they are
// implemented across phases. All modules must be non-blocking; no module may
// call delay() or busy-wait on I/O.
//
// Boot order (fast cold start — see docs/functional-spec.md §8.3):
//   1. Serial + logging                (Phase 0  — now)
//   2. Config load from LittleFS       (Phase 1)
//   3. Display init + matrix scanner   (Phase 9  — first lit code ≤ 1.5 s)
//   4. WiFi (AP+STA)                   (Phase 2)
//   5. Web UI + OTA                    (Phase 3 / 4)
//   6. MQTT                            (Phase 5)
//   7. Battery monitor + sleep manager (Phase 9b / 9c)
//
// Cooperative loop order (repeated every iteration):
//   switch_matrix → code_engine → battery_monitor → sleep_manager →
//   mqtt_mgr → web_ui → ota_mgr → wifi_mgr
#include <Arduino.h>
#include <LittleFS.h>

#include "config.h"
#include "log.h"
#include "display_mgr.h"
#include "battery_monitor.h"
#include "wifi_mgr.h"
#include "web_ui.h"
#include "ota_mgr.h"
#include "mqtt_mgr.h"
#include "commands.h"
#include "switch_matrix.h"
#include "code_engine.h"
#include "state.h"
#include "sleep_mgr.h"

// ---------------------------------------------------------------------------
// Module tick stubs — replaced with real includes as phases are implemented.
// ---------------------------------------------------------------------------

// display_mgr   — Phase 9 (live)
// battery_monitor — Phase 9 (live, partial; curves Phase 9b)
// switch_matrix — Phase 6 (live)
// code_engine   — Phase 7 (live)
// mqtt_mgr — Phase 5 (live)
// web_ui — Phase 3 (live)
// ota_mgr — Phase 4 (live)
// sleep_mgr — Phase 9c (live)

// ---------------------------------------------------------------------------
// Device configuration (loaded from LittleFS at boot)
// ---------------------------------------------------------------------------

cfg::Config g_config;

// Switch-matrix scanner (Phase 6). The hardware-facing ScanIO comes from
// scan_io_esp.cpp which is only compiled in the ESP build.
switch_matrix::SwitchMatrix g_matrix;
static uint32_t s_last_matrix_tick_ms = 0;
static uint32_t s_last_matrix_state   = 0;

code_engine::CodeEngine g_engine;
static uint32_t s_last_code_bits = 0;  // for sleep_mgr switch-change detection

namespace {

struct SwitchLayoutConfig {
    uint8_t row_a[switch_matrix::NUM_ROWS];
    uint8_t col_b[switch_matrix::NUM_COLS];
    uint8_t digit_order[6];
    // UI grid geometry (from switch_layout.json; used to build code.grid in state).
    uint8_t ui_rows;
    uint8_t ui_cols;
    uint8_t switch_count;
    uint8_t prop_row_to_scan_col[appstate::MAX_GRID_ROWS]; // ui_rows entries
    uint8_t prop_col_to_scan_row[appstate::MAX_GRID_COLS]; // ui_cols entries
};

static void load_switch_layout_defaults(SwitchLayoutConfig& m) {
    for (uint8_t i = 0; i < switch_matrix::NUM_ROWS; ++i) m.row_a[i] = i;
    for (uint8_t i = 0; i < switch_matrix::NUM_COLS; ++i) m.col_b[i] = i;
    for (uint8_t i = 0; i < 6; ++i) m.digit_order[i] = (uint8_t)(i + 1);
    // Default UI geometry: physical rows = scan cols, physical cols = scan rows.
    m.ui_rows = switch_matrix::NUM_COLS;          // 4
    m.ui_cols = switch_matrix::NUM_ROWS;          // 5
    m.switch_count = switch_matrix::NUM_CELLS;    // 20
    for (uint8_t i = 0; i < m.ui_rows; ++i) m.prop_row_to_scan_col[i] = i;
    for (uint8_t i = 0; i < m.ui_cols; ++i) m.prop_col_to_scan_row[i] = i;
}

static bool is_perm_0_n(const uint8_t* vals, uint8_t n) {
    bool seen[20] = {false};
    for (uint8_t i = 0; i < n; ++i) {
        if (vals[i] >= n || seen[vals[i]]) return false;
        seen[vals[i]] = true;
    }
    return true;
}

static bool load_switch_layout_file(SwitchLayoutConfig& out) {
    File f = LittleFS.open("/switch_layout.json", "r");
    if (!f) {
        pxlog::warn("main", "switch_layout.json missing; using identity maps");
        return false;
    }

    JsonDocument doc;
    DeserializationError de = deserializeJson(doc, f);
    f.close();
    if (de) {
        pxlog::warn("main", "switch_layout.json parse failed: %s", de.c_str());
        return false;
    }

    JsonObject row_obj = doc["row_gpio_to_a"].as<JsonObject>();
    JsonObject col_obj = doc["col_gpio_to_b"].as<JsonObject>();
    JsonArray digit_order = doc["digit_order"].as<JsonArray>();
    if (row_obj.isNull() || col_obj.isNull() || digit_order.isNull() || digit_order.size() != 6) {
        pxlog::warn("main", "switch_layout.json missing required mapping keys");
        return false;
    }

    for (uint8_t r = 0; r < switch_matrix::NUM_ROWS; ++r) {
        String k = String(pins::ROW[r]);
        JsonVariant v = row_obj[k];
        if (v.isNull()) return false;
        int a = v.as<int>();
        if (a < 0 || a >= switch_matrix::NUM_ROWS) return false;
        out.row_a[r] = (uint8_t)a;
    }
    for (uint8_t c = 0; c < switch_matrix::NUM_COLS; ++c) {
        String k = String(pins::COL[c]);
        JsonVariant v = col_obj[k];
        if (v.isNull()) return false;
        int b = v.as<int>();
        if (b < 0 || b >= switch_matrix::NUM_COLS) return false;
        out.col_b[c] = (uint8_t)b;
    }
    if (!is_perm_0_n(out.row_a, switch_matrix::NUM_ROWS) ||
        !is_perm_0_n(out.col_b, switch_matrix::NUM_COLS)) {
        pxlog::warn("main", "switch_layout.json has non-permutation mapping");
        return false;
    }

    bool seen_digit[6] = {false};
    for (uint8_t i = 0; i < 6; ++i) {
        int d = digit_order[i].as<int>();
        if (d < 1 || d > 6 || seen_digit[d - 1]) {
            pxlog::warn("main", "switch_layout.json has invalid digit_order");
            return false;
        }
        out.digit_order[i] = (uint8_t)d;
        seen_digit[d - 1] = true;
    }

    // Optional UI grid fields — fall back to defaults already set if absent/invalid.
    int ui_rows_v    = doc["ui_rows"]      | (int)out.ui_rows;
    int ui_cols_v    = doc["ui_cols"]      | (int)out.ui_cols;
    int sw_count_v   = doc["switch_count"] | (int)out.switch_count;
    if (ui_rows_v >= 1 && ui_rows_v <= (int)appstate::MAX_GRID_ROWS &&
        ui_cols_v >= 1 && ui_cols_v <= (int)appstate::MAX_GRID_COLS &&
        sw_count_v >= 1 && sw_count_v <= (int)switch_matrix::NUM_CELLS) {
        out.ui_rows      = (uint8_t)ui_rows_v;
        out.ui_cols      = (uint8_t)ui_cols_v;
        out.switch_count = (uint8_t)sw_count_v;
    }
    JsonArray prow = doc["prop_row_to_scan_col"].as<JsonArray>();
    if (!prow.isNull() && (int)prow.size() == (int)out.ui_rows) {
        bool ok = true;
        for (uint8_t i = 0; i < out.ui_rows && ok; ++i) {
            int v = prow[i].as<int>();
            if (v < 0 || v >= switch_matrix::NUM_COLS) { ok = false; break; }
            out.prop_row_to_scan_col[i] = (uint8_t)v;
        }
        if (!ok) pxlog::warn("main", "switch_layout.json: prop_row_to_scan_col invalid; using default");
    }
    JsonArray pcol = doc["prop_col_to_scan_row"].as<JsonArray>();
    if (!pcol.isNull() && (int)pcol.size() == (int)out.ui_cols) {
        bool ok = true;
        for (uint8_t i = 0; i < out.ui_cols && ok; ++i) {
            int v = pcol[i].as<int>();
            if (v < 0 || v >= switch_matrix::NUM_ROWS) { ok = false; break; }
            out.prop_col_to_scan_row[i] = (uint8_t)v;
        }
        if (!ok) pxlog::warn("main", "switch_layout.json: prop_col_to_scan_row invalid; using default");
    }
    return true;
}

static void apply_switch_layout(const SwitchLayoutConfig& m) {
    uint8_t bit_map[switch_matrix::NUM_CELLS] = {0};
    for (uint8_t c = 0; c < switch_matrix::NUM_COLS; ++c) {
        for (uint8_t r = 0; r < switch_matrix::NUM_ROWS; ++r) {
            uint8_t phys_i = switch_matrix::bit_index_for(c, r);
            uint8_t bit_i = (uint8_t)(m.row_a[r] + switch_matrix::NUM_ROWS * m.col_b[c]);
            bit_map[phys_i] = bit_i;
        }
    }
    if (!g_matrix.set_bit_map(bit_map)) {
        g_matrix.reset_bit_map_identity();
        pxlog::warn("main", "switch layout bit map invalid; identity fallback");
    }

    if (!code_engine::set_digit_order(m.digit_order)) {
        code_engine::reset_digit_order();
        pxlog::warn("main", "switch layout digit_order invalid; identity fallback");
    }

    // Compute UI-grid-position → code_bits-bit lookup and register with state module.
    {
        int8_t grid_bit[appstate::MAX_GRID_ROWS][appstate::MAX_GRID_COLS];
        for (uint8_t r = 0; r < appstate::MAX_GRID_ROWS; ++r)
            for (uint8_t c = 0; c < appstate::MAX_GRID_COLS; ++c)
                grid_bit[r][c] = -1;
        for (uint8_t r = 0; r < m.ui_rows && r < appstate::MAX_GRID_ROWS; ++r) {
            uint8_t scan_col = m.prop_row_to_scan_col[r];
            for (uint8_t c = 0; c < m.ui_cols && c < appstate::MAX_GRID_COLS; ++c) {
                uint8_t scan_row = m.prop_col_to_scan_row[c];
                grid_bit[r][c] = (int8_t)(m.row_a[scan_row] +
                                          switch_matrix::NUM_ROWS * m.col_b[scan_col]);
            }
        }
        appstate::set_grid_layout(m.ui_rows, m.ui_cols, m.switch_count, grid_bit);
    }
}

} // namespace

static void matrix_tick() {
    // Pace ticks by the configured poll interval. SwitchMatrix itself does
    // not call millis() so it stays host-testable; the spacing is enforced
    // here in the cooperative loop.
    uint32_t now = millis();
    uint32_t interval = g_config.scan_poll_interval_ms;
    if (interval == 0) interval = 1;
    if (now - s_last_matrix_tick_ms < interval) return;
    s_last_matrix_tick_ms = now;
    if (g_matrix.tick()) {
        uint32_t st = g_matrix.state();
        pxlog::info("matrix", "state=0x%05lx changes=%lu",
                    (unsigned long)st,
                    (unsigned long)g_matrix.change_count());
        s_last_matrix_state = st;
    }
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup() {
    pxlog::begin();
    pxlog::info("main", "phase=9c fw=%s", FW_VERSION);

    bool cfg_was_invalid = false;
    cfg::load(g_config, cfg_was_invalid);
    if (cfg_was_invalid) {
        pxlog::warn("main", "config_invalid: using built-in defaults");
        mqtt_mgr::note_config_invalid_pending();
    }

    SwitchLayoutConfig sw_layout;
    load_switch_layout_defaults(sw_layout);
    if (!load_switch_layout_file(sw_layout)) {
        pxlog::warn("main", "switch layout using defaults");
    }
    apply_switch_layout(sw_layout);

    // ---- Boot order per spec §8.3: display + matrix BEFORE WiFi/MQTT ----
    // This ensures the first lit code appears within 1.5 s of power-on.
    // Sanity-check boot-strap pins BEFORE g_matrix.begin() reconfigures GPIO15
    // (COL0) as an output — after that, its logic level is no longer meaningful
    // as a boot-strap check.
    display_mgr::begin(g_config);
    display_mgr::sanity_check_boot_pins();
    g_matrix.begin(switch_matrix::esp_scan_io(), g_config.scan_debounce_samples);
    battery_monitor::begin(g_config);
    sleep_mgr::begin(g_config);

    commands::begin(&g_config, &g_engine);

    // Code engine: wire callbacks to MQTT event publishers.
    {
        code_engine::Callbacks cb;
        cb.user = nullptr;
        cb.on_code_changed = [](uint32_t code_int, uint32_t code_bits,
                                const char* code_str, void*) {
            JsonDocument d;
            d["code"]      = code_str;
            d["code_int"]  = code_int;
            appstate::add_code_grid(d.as<JsonObject>(), code_bits);
            mqtt_mgr::publish_event("code", "code_changed", nullptr,
                                    d.as<JsonVariantConst>());
        };
        cb.on_code_solved = [](uint32_t /*code_int*/, uint32_t /*code_bits*/,
                               const char* code_str, void*) {
            JsonDocument d;
            d["code"]   = code_str;
            d["target"] = g_engine.state().target_str;
            mqtt_mgr::publish_event("code", "code_solved",
                                    "code matches target",
                                    d.as<JsonVariantConst>());
            mqtt_mgr::publish_state();
        };
        cb.on_code_unsolved = [](uint32_t /*code_int*/, uint32_t /*code_bits*/,
                                 const char* code_str, void*) {
            JsonDocument d;
            d["code"]   = code_str;
            d["target"] = g_engine.state().target_str;
            mqtt_mgr::publish_event("code", "code_unsolved",
                                    "code no longer matches target",
                                    d.as<JsonVariantConst>());
            mqtt_mgr::publish_state();
        };
        cb.on_solve = [](uint32_t /*code_int*/, uint32_t /*code_bits*/,
                         const char* code_str, void*) {
            JsonDocument d;
            d["code"]   = code_str;
            d["target"] = g_engine.state().target_str;
            mqtt_mgr::publish_event("code", "solve",
                                    "puzzle solved (latched)",
                                    d.as<JsonVariantConst>());
            mqtt_mgr::publish_state();
        };
        cb.on_unlatch = [](void*) {
            JsonDocument d;
            d["code"] = g_engine.state().code_str;
            mqtt_mgr::publish_event("code", "unlatch",
                                    "latch cleared",
                                    d.as<JsonVariantConst>());
            mqtt_mgr::publish_state();
        };
        uint32_t target_int = 0;
        bool has_target = g_config.puzzle_has_target;
        if (has_target) {
            code_engine::parse_target(g_config.puzzle_target.c_str(), &target_int);
        }
        g_engine.begin(g_config.puzzle_mode.c_str(), has_target, target_int, cb);
        appstate::set_code_state(&g_engine.state());
    }

    // Capture initial switch state before showing the boot code.
    // The matrix needs (scan_debounce_samples + 1) full sweeps to commit a
    // stable reading. We do this synchronously here — it costs at most
    // (samples+1) * scan_poll_interval_ms ≈ 50 ms with defaults, well within
    // the ≤ 1.5 s cold-boot budget and far faster than WiFi association.
    {
        uint8_t  sweeps   = (uint8_t)(g_config.scan_debounce_samples + 1);
        uint32_t interval = g_config.scan_poll_interval_ms
                            ? g_config.scan_poll_interval_ms : 10u;
        for (uint8_t i = 0; i < sweeps; ++i) {
            for (uint8_t c = 0; c < switch_matrix::NUM_COLS; ++c) g_matrix.tick();
            delay(interval);
        }
        g_engine.tick(g_matrix.state());
    }

    // Show first lit code immediately (spec §8.3: ≤ 1.5 s cold-boot target).
    display_mgr::show_boot_code(g_engine.state().code_str);

    wifi_mgr::begin(g_config);
    web_ui::begin(&g_config);
    ota_mgr::begin_arduino_ota(g_config);
    mqtt_mgr::begin(&g_config, [](const uint8_t* p, size_t n, void*) {
        commands::handle_command_payload(p, n);
    }, nullptr);

    pxlog::info("main", "matrix_cells=%u cols=%u rows=%u",
                MATRIX_NUM_CELLS, pins::NUM_COLS, pins::NUM_ROWS);
}

void loop() {
    matrix_tick();
    g_engine.tick(g_matrix.state());
    battery_monitor::tick();

    // Notify sleep manager of any confirmed switch-state change.
    {
        uint32_t cb = g_engine.state().code_bits;
        bool changed = (cb != s_last_code_bits);
        s_last_code_bits = cb;
        sleep_mgr::loop(changed);
    }

    // Drive display from current engine + command state.
    {
        const auto& es = g_engine.state();
        display_mgr::tick(es.code_str,
                          es.latched,
                          commands::identify_active(),
                          commands::is_off(),
                          appstate::mqtt_connected(),
                          wifi_mgr::sta_rssi(),
                          g_config.signal_indicator_enabled,
                          g_config.signal_rssi_dbm,
                          battery_monitor::status() == battery_monitor::Status::Low,
                          battery_monitor::status() == battery_monitor::Status::Critical);
    }
    mqtt_mgr::loop();
    commands::tick();
    web_ui::loop();
    ota_mgr::loop();
    wifi_mgr::loop();
    yield();
}
