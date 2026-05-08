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
#include "wifi_mgr.h"
#include "web_ui.h"
#include "ota_mgr.h"
#include "mqtt_mgr.h"
#include "commands.h"
#include "switch_matrix.h"

// ---------------------------------------------------------------------------
// Module tick stubs — replaced with real includes as phases are implemented.
// ---------------------------------------------------------------------------

static inline void code_engine_loop()   {}   // Phase 7
static inline void battery_monitor_loop() {} // Phase 9b
static inline void sleep_manager_loop()  {}  // Phase 9c
// switch_matrix — Phase 6 (live)
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
    pxlog::info("main", "phase=6-matrix fw=%s", FW_VERSION);

    bool cfg_was_invalid = false;
    cfg::load(g_config, cfg_was_invalid);
    if (cfg_was_invalid) {
        pxlog::warn("main", "config_invalid: using built-in defaults");
        mqtt_mgr::note_config_invalid_pending();
    }

    commands::begin(&g_config);
    g_matrix.begin(switch_matrix::esp_scan_io(), g_config.scan_debounce_samples);
    wifi_mgr::begin(g_config);
    web_ui::begin(&g_config);
    ota_mgr::begin_arduino_ota(g_config);
    mqtt_mgr::begin(&g_config, [](const uint8_t* p, size_t n, void*) {
        commands::handle_command_payload(p, n);
    }, nullptr);

    pxlog::info("main", "matrix_cells=%u cols=%u rows=%u",
                MATRIX_NUM_CELLS, pins::NUM_COLS, pins::NUM_ROWS);
    pxlog::info("main", "i2c sda=%u scl=%u display_low=0x%02x display_high=0x%02x",
                pins::I2C_SDA, pins::I2C_SCL,
                i2c_addr::DISPLAY_LOW, i2c_addr::DISPLAY_HIGH);
}

void loop() {
    matrix_tick();
    code_engine_loop();
    battery_monitor_loop();
    sleep_manager_loop();
    mqtt_mgr::loop();
    commands::tick();
    web_ui::loop();
    ota_mgr::loop();
    wifi_mgr::loop();
    yield();
}
