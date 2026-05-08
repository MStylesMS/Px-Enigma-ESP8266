// log.cpp — implementation of the pxlog ring-buffer logger.
#include "log.h"
#include "config.h"  // LOG_RING_LINES, LOG_LINE_MAX

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace pxlog {

// ---------------------------------------------------------------------------
// Ring buffer
// ---------------------------------------------------------------------------

struct Line { char buf[LOG_LINE_MAX]; };

static Line   s_ring[LOG_RING_LINES];
static size_t s_head  = 0;   // index of the next write slot
static size_t s_count = 0;   // number of valid entries (0..LOG_RING_LINES)

static void ring_push(const char* level, const char* tag, const char* msg) {
    snprintf(s_ring[s_head].buf, LOG_LINE_MAX,
             "%lu %s [%s] %s",
             (unsigned long)millis(), level, tag, msg);
    s_head = (s_head + 1) % LOG_RING_LINES;
    if (s_count < LOG_RING_LINES) s_count++;
}

// ---------------------------------------------------------------------------
// Internal emit: Serial + ring buffer
// ---------------------------------------------------------------------------

static void emit(const char* level, const char* tag,
                 const char* fmt, va_list ap) {
    char msg[LOG_LINE_MAX];
    vsnprintf(msg, sizeof(msg), fmt, ap);

    // Best-effort serial output. After setup() the matrix scanner claims
    // GPIO1 (TX); these prints may be garbled or absent in normal operation.
    Serial.printf("%lu %s [%s] %s\r\n",
                  (unsigned long)millis(), level, tag, msg);

    ring_push(level, tag, msg);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void begin(uint32_t baud) {
    Serial.begin(baud);
    delay(50);  // let the USB-to-serial bridge enumerate
    Serial.printf("\r\n--- px-enigma-esp8266 boot  fw=%s ---\r\n",
                  FW_VERSION);
}

void info(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit("INFO", tag, fmt, ap);
    va_end(ap);
}

void warn(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit("WARN", tag, fmt, ap);
    va_end(ap);
}

void err(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit("ERR ", tag, fmt, ap);
    va_end(ap);
}

void each_line(LineCb cb, void* user) {
    if (s_count == 0) return;
    const size_t start = (s_head + LOG_RING_LINES - s_count) % LOG_RING_LINES;
    for (size_t i = 0; i < s_count; ++i) {
        const size_t idx = (start + i) % LOG_RING_LINES;
        if (!cb(s_ring[idx].buf, user)) return;
    }
}

void flush() {
    Serial.flush();
}

size_t line_count() {
    return s_count;
}

} // namespace pxlog
