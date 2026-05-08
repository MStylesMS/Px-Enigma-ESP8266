// log.h — printf-style logging with an in-RAM ring buffer.
//
// All log output goes to two places simultaneously:
//   1. Serial (best-effort). GPIO1 (TX) is reused by the matrix scanner after
//      setup(), so serial output is only reliable during early boot. The ring
//      buffer is the production diagnostic path.
//   2. A 32-line ring buffer surfaced by the Web UI (/api/log endpoint).
//
// Usage:
//   pxlog::begin();                          // once, in setup()
//   pxlog::info("wifi", "STA connected");
//   pxlog::warn("cfg",  "key missing: %s", key);
//   pxlog::err ("mqtt", "connect failed rc=%d", rc);
//
//   // Iterate the buffer (e.g. for /api/log):
//   pxlog::each_line([](const char* line, void*) { ... return true; }, nullptr);
//
//   // Before ESP.deepSleep():
//   pxlog::flush();
#pragma once

#include <Arduino.h>

namespace pxlog {

// Call once in setup() before the first log call.
void begin(uint32_t baud = 115200);

// printf-style emit at INFO / WARN / ERR level.
// Format string must be a string literal so the compiler can check args.
void info(const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));
void warn(const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));
void err (const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));

// Iterate the ring buffer from oldest to newest.
// cb returns false to stop early. user is passed through to cb.
typedef bool (*LineCb)(const char* line, void* user);
void each_line(LineCb cb, void* user);

// Drain any bytes still in the hardware serial transmit FIFO.
// Call immediately before ESP.deepSleep() so the last log line is not lost.
void flush();

// Number of lines currently held in the buffer (0..LOG_RING_LINES).
size_t line_count();

} // namespace pxlog
