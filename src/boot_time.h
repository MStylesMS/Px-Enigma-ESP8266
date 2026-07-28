#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace boot_time {

// Monotonic milliseconds since boot. Extends Arduino's 32-bit millis() across
// its ~49.7-day rollover.
uint64_t milliseconds();

} // namespace boot_time
