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

// ---------------------------------------------------------------------------
// Module tick stubs — replaced with real includes as phases are implemented.
// ---------------------------------------------------------------------------

static inline void sleep_manager_loop() {}   // Phase 9c
// display_mgr   — Phase 9 (live)
// battery_monitor — Phase 9 (live, partial; curves Phase 9b)
// switch_matrix — Phase 6 (live)
// code_engine   — Phase 7 (live)
// mqtt_mgr — Phase 5 (live)
// web_ui — Phase 3 (live)
// ota_mgr — Phase 4 (live)

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
    pxlog::info("main", "phase=9-hw fw=%s", FW_VERSION);

    bool cfg_was_invalid = false;
    cfg::load(g_config, cfg_was_invalid);
    if (cfg_was_invalid) {
        pxlog::warn("main", "config_invalid: using built-in defaults");
        mqtt_mgr::note_config_invalid_pending();
    }

    // ---- Boot order per spec §8.3: display + matrix BEFORE WiFi/MQTT ----
    // This ensures the first lit code appears within 1.5 s of power-on.
    // Sanity-check boot-strap pins BEFORE g_matrix.begin() reconfigures GPIO15
    // (COL0) as an output — after that, its logic level is no longer meaningful
    // as a boot-strap check.
    display_mgr::begin(g_config);
    display_mgr::sanity_check_boot_pins();
    g_matrix.begin(switch_matrix::esp_scan_io(), g_config.scan_debounce_samples);
    battery_monitor::begin(g_config);

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
            d["code_bits"] = code_bits;
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
    sleep_manager_loop();

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
                          g_config.signal_rssi_dbm);
    }
    mqtt_mgr::loop();
    commands::tick();
    web_ui::loop();
    ota_mgr::loop();
    wifi_mgr::loop();
    yield();
}
