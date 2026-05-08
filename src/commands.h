// commands.h — Dispatcher for MQTT commands and locally-invoked actions.
//
// Phase 5 added the workspace-required commands (getState | restart |
// identify | ping | reloadConfig).
// Phase 8 adds the project commands per spec §11.2:
//     setBrightness | setTarget | clearTarget | setMode | reset |
//     getCode | setBatteryProfile | setSignalIndicator | on | off
#pragma once

#include "config.h"
#include "code_engine.h"
#include <Arduino.h>

namespace commands {

// `engine` may be nullptr in early boot phases / unit tests; project
// commands that need it will return command_failed("not_initialised").
void begin(cfg::Config* c, code_engine::CodeEngine* engine = nullptr);

// Handle a payload that arrived on the commands topic (called from mqtt_mgr).
void handle_command_payload(const uint8_t* payload, size_t len);

// Locally trigger identify (also called from /api/identify). Phase 5 stub:
// just sets a timer + flag. The display layer (Phase 9) will read the flag.
void identify();
bool identify_active();

// Schedule a deferred restart so the calling handler can return / publish
// outcome events before the device reboots.
void schedule_restart(uint32_t delay_ms = 500);

// True iff the device is in the OFF state (set by the `off` command;
// cleared by `on`). Phase 8: just a flag — the display FSM (Phase 9)
// will honour it.
bool is_off();

// Tick — runs short-running async tasks (identify timeout, restart pending).
// Call every loop iteration.
void tick();

} // namespace commands
