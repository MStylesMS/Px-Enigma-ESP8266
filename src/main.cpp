// px-enigma-esp8266 — boot skeleton (Phase 0)
//
// The full module layout described in docs/implementation-plan.md is built
// out incrementally. This file currently does just enough to exercise the
// PlatformIO build and produce a boot banner on the serial console.

#include <Arduino.h>

#ifndef FW_VERSION
#define FW_VERSION "0.0.0-skeleton"
#endif

static unsigned long g_last_heartbeat_ms = 0;

void setup() {
    Serial.begin(115200);
    delay(50);  // brief settle so the first banner is not garbled by the bootrom
    Serial.println();
    Serial.println(F("px-enigma-esp8266 boot"));
    Serial.print(F("  fw_version="));
    Serial.println(F(FW_VERSION));
    Serial.println(F("  status=phase-0-skeleton"));
}

void loop() {
    const unsigned long now = millis();
    if (now - g_last_heartbeat_ms >= 5000UL) {
        g_last_heartbeat_ms = now;
        Serial.print(F("alive uptime_ms="));
        Serial.println(now);
    }
    yield();
}
