// commands.h — Dispatcher for MQTT commands and locally-invoked actions.
//
// Phase 5 implements only the four workspace-required commands per
// implementation-plan.md §Phase 5:
//     getState | restart | identify | ping
// Project-specific commands (setBrightness, setTarget, …) arrive in later
// phases when the puzzle engine, battery monitor, etc. exist.
#pragma once

#include "config.h"
#include <Arduino.h>

namespace commands {

void begin(cfg::Config* c);

// Handle a payload that arrived on the commands topic (called from mqtt_mgr).
void handle_command_payload(const uint8_t* payload, size_t len);

// Locally trigger identify (also called from /api/identify). Phase 5 stub:
// just sets a timer + flag. The display layer (Phase 9) will read the flag.
void identify();
bool identify_active();

// Schedule a deferred restart so the calling handler can return / publish
// outcome events before the device reboots.
void schedule_restart(uint32_t delay_ms = 500);

// Tick — runs short-running async tasks (identify timeout, restart pending).
// Call every loop iteration.
void tick();

} // namespace commands
