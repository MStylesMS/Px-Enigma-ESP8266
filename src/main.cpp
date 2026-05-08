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

// ---------------------------------------------------------------------------
// Module tick stubs — replaced with real includes as phases are implemented.
// ---------------------------------------------------------------------------

static inline void switch_matrix_loop() {}   // Phase 6
static inline void code_engine_loop()   {}   // Phase 7
static inline void battery_monitor_loop() {} // Phase 9b
static inline void sleep_manager_loop()  {}  // Phase 9c
static inline void mqtt_mgr_loop()      {}   // Phase 5
// web_ui — Phase 3 (live)
static inline void ota_mgr_loop()       {}   // Phase 4

// ---------------------------------------------------------------------------
// Device configuration (loaded from LittleFS at boot)
// ---------------------------------------------------------------------------

cfg::Config g_config;

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup() {
    pxlog::begin();
    pxlog::info("main", "phase=3-webui fw=%s", FW_VERSION);

    bool cfg_was_invalid = false;
    cfg::load(g_config, cfg_was_invalid);
    if (cfg_was_invalid) {
        pxlog::warn("main", "config_invalid: using built-in defaults");
    }

    wifi_mgr::begin(g_config);
    web_ui::begin(&g_config);

    pxlog::info("main", "matrix_cells=%u cols=%u rows=%u",
                MATRIX_NUM_CELLS, pins::NUM_COLS, pins::NUM_ROWS);
    pxlog::info("main", "i2c sda=%u scl=%u display_low=0x%02x display_high=0x%02x",
                pins::I2C_SDA, pins::I2C_SCL,
                i2c_addr::DISPLAY_LOW, i2c_addr::DISPLAY_HIGH);
}

void loop() {
    switch_matrix_loop();
    code_engine_loop();
    battery_monitor_loop();
    sleep_manager_loop();
    mqtt_mgr_loop();
    web_ui::loop();
    ota_mgr_loop();
    wifi_mgr::loop();
    yield();
}
