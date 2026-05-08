# px-enigma-esp8266 — Pin Mapping

**Status:** Draft for review
**Version:** 0.1.0-draft

Authoritative GPIO table. These assignments match the existing wired units
and **must not change** without a hardware revision. They are reflected as
compile-time constants in `src/config.h`.

> **Hardware-rework note.** GPIO1 (TX) and GPIO3 (RX) are reused as
> matrix lines in the current revision, which disables the serial
> console at runtime. This is an accepted bug in this hardware
> revision. **Any future hardware spin must move the matrix lines off
> GPIO1 / GPIO3 — and ideally off GPIO0 / GPIO2 — to restore a
> first-class serial console and reserve dedicated I2C pins.**
> Migrating to an MCU with more usable GPIOs (ESP32-C3 / ESP32-S3) is
> the cleanest path.

## ESP8266 GPIO assignments

| GPIO  | NodeMCU label | Direction | Role | Notes |
|------|---------------|-----------|------|------|
| GPIO0  | D3 | I/O — open-drain | **I2C SDA** | Boot-strap: must be HIGH at reset; 4.7 kΩ pull-up |
| GPIO1  | TX | OUT | Matrix column output #2 | Shared with UART TX; serial console disabled at runtime |
| GPIO2  | D4 | I/O — open-drain | **I2C SCL** | Boot-strap: must be HIGH at reset; on-board LED also tied here on some carriers |
| GPIO3  | RX | IN  pullup | Matrix row input #2 | Shared with UART RX |
| GPIO4  | D2 | IN  pullup | Matrix row input #4 | |
| GPIO5  | D1 | OUT | Matrix column output #3 | |
| GPIO12 | D6 | IN  pullup | Matrix row input #1 | |
| GPIO13 | D7 | IN  pullup | Matrix row input #5 | |
| GPIO14 | D5 | IN  pullup | Matrix row input #3 | |
| GPIO15 | D8 | OUT | Matrix column output #1 | Boot-strap: must be LOW at reset; external pull-down on board |
| GPIO16 | D0 | OUT | Matrix column output #4 | No internal pull-up; deep-sleep wake pin (unused) |
| A0     | A0 | ADC | 12 V rail voltage sense | Calibration: `V = 0.0531·raw + 0.1978` |

### Matrix wiring summary

```
          rows  →   GPIO12  GPIO3   GPIO14  GPIO4   GPIO13
columns ↓        (row0)  (row1)  (row2)  (row3)  (row4)
GPIO15 (col0)     bit0    bit1    bit2    bit3    bit4
GPIO1  (col1)     bit5    bit6    bit7    bit8    bit9
GPIO5  (col2)     bit10   bit11   bit12   bit13   bit14
GPIO16 (col3)     bit15   bit16   bit17   bit18   bit19
```

Bit numbering matches the legacy implementation so existing target codes
remain valid across the rewrite.

## I2C device addresses

| Address | Device | Role |
|---------|--------|------|
| `0x70` | HT16K33 #1 (`display_low`)  | Right half of `XX-YY-ZZ` field |
| `0x71` | HT16K33 #2 (`display_high`) | Left half of `XX-YY-ZZ` field |

## Boot-strap reminder

At reset:

- GPIO0 must be **HIGH** (held by I2C pull-up).
- GPIO2 must be **HIGH** (held by I2C pull-up).
- GPIO15 must be **LOW** (held by on-board pull-down on the column-output line).

Firmware must not drive any of these pins to a conflicting state before
the boot ROM has latched the boot mode.

## Suggested compile-time constants

```cpp
// src/config.h — pin layout (do not change without a hardware revision)
namespace pins {
    inline constexpr uint8_t I2C_SDA          = 0;   // GPIO0  / D3
    inline constexpr uint8_t I2C_SCL          = 2;   // GPIO2  / D4
    inline constexpr uint8_t MATRIX_COLS[4]   = {15, 1, 5, 16};
    inline constexpr uint8_t MATRIX_ROWS[5]   = {12, 3, 14, 4, 13};
    inline constexpr uint8_t BATTERY_SENSE_A0 = A0;
}

namespace i2c_addr {
    inline constexpr uint8_t DISPLAY_LOW  = 0x70;
    inline constexpr uint8_t DISPLAY_HIGH = 0x71;
}
```
