#include "boot_time.h"

namespace boot_time {

uint64_t milliseconds() {
    static uint32_t s_previous = 0;
    static uint64_t s_epoch    = 0;

    uint32_t now = millis();
    if (now < s_previous) s_epoch += (UINT64_C(1) << 32);
    s_previous = now;
    return s_epoch + now;
}

} // namespace boot_time
