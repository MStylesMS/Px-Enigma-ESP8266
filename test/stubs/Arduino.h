#pragma once
// Minimal Arduino API stub for native (host) unit-test builds.
// Provides just enough of the Arduino type system and runtime functions for
// code under test to compile and link without the ESP8266 toolchain.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// ---- fundamental types ---------------------------------------------------

using String = std::string;
using byte   = unsigned char;

// A0 is defined by the Arduino core as the analogue pin number.
// For host builds, we use a sentinel that is out of the digital GPIO range.
#ifndef A0
#define A0 17
#endif

// F() — on Arduino this moves string literals to flash. For the host build it
// is a no-op passthrough so the same source compiles in both environments.
#define F(x) (x)

// ---- ESP class stub ------------------------------------------------------

struct EspClass {
    uint32_t freeHeap    = 38000;
    uint32_t minFreeHeap = 37500;

    uint32_t getFreeHeap()    const { return freeHeap; }
    uint32_t getMinFreeHeap() const { return minFreeHeap; }
    void     deepSleep(uint64_t) {}
    void     restart() {}
};

extern EspClass ESP;

// ---- Arduino runtime stubs -----------------------------------------------

unsigned long millis();

inline void delay(unsigned long) {}
inline void yield() {}

// Serial stub — silently discards output so log.cpp compiles on the host.
// Real Serial output is not tested in unit tests.
struct SerialStub {
    template<typename T> void print(T)   {}
    template<typename T> void println(T) {}
    void println() {}
    void printf(const char*, ...) {}
    void begin(unsigned long) {}
    void flush() {}
};

extern SerialStub Serial;

// Analogue input stub.
inline int analogRead(uint8_t) { return 0; }

// Digital I/O stubs (used by battery_monitor and display_mgr; no-ops on host).
inline void    pinMode(uint8_t, uint8_t) {}
inline int     digitalRead(uint8_t)      { return 0; }
