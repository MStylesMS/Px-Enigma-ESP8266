// sleep_mgr.h — Inactivity sleep manager (spec §7).
//
// Phase 9c. Runs only when the battery profile is anything other than
// `external` / `unknown`. Tracks the time since the last confirmed
// switch-state change; when the inactivity timer elapses it:
//   1. Publishes `going_to_sleep` to MQTT.
//   2. Flushes the log ring buffer.
//   3. Blanks the display except for the two code-separator dashes (sleep
//      indicator).
//   4. Calls ESP.deepSleep(0) — wake requires a power cycle.
//
// `inactivity_minutes == 0` disables the timer entirely, even on battery
// profiles.
#pragma once

#include "config.h"
#include <stdint.h>

namespace sleep_mgr {

// Call once in setup(), after battery_monitor::begin().
// Reads inactivity_minutes and is_external() from the config / monitor.
void begin(const cfg::Config& c);

// Call every cooperative loop iteration — always before display_mgr::tick().
// Pass `switch_changed = true` on the iteration where the code_engine
// detects a new confirmed switch state (i.e. code_bits changed).
void loop(bool switch_changed);

// Idle time in whole minutes since the last confirmed switch-state change.
// Returns 0 if the sleep manager is disabled or not yet started.
uint32_t idle_minutes();

// True if the sleep manager is enabled for the current profile.
bool enabled();

// Operator-triggered or deferred deep sleep. Display sleep indicator must
// already be shown (via display_mgr::show_sleep_indicator()).
void enter_deep_sleep();

} // namespace sleep_mgr
