# px-enigma-esp8266 — Hardware Specification

**Status:** Draft for review
**Version:** 0.1.0-draft

This document is the electrical reference for firmware authors. It captures
the chip, the peripherals, the I2C addresses, and the GPIO assignments. The
firmware **must not change** any of these without a corresponding hardware
revision; the existing units are already wired to this layout.

For the full GPIO table in isolation, see [pin-mapping.md](pin-mapping.md).

---

## 1. MCU

| Item | Value |
|---|---|
| Module | Espressif ESP8266MOD (ESP-12E or ESP-12F) |
| Carrier | LoLin NodeMCU v3 (CH340G USB-UART) |
| Clock | 80 MHz default; 160 MHz optional |
| Flash | 4 MB (`board_build.filesystem = littlefs`) |
| User heap | ~ 80 KB available at runtime after framework + WiFi + MQTT |
| USB role | Programming + serial diagnostics only; not used in production wiring |

Boot-strap pins of note: **GPIO0** (must be HIGH at reset), **GPIO2** (must
be HIGH at reset), **GPIO15** (must be LOW at reset). The schematic enforces
these states; firmware must not violate them during boot.

---

## 2. Power

The Enigma may be powered from one of two source classes, selected by
the `battery.profile` configuration field (see
[functional-spec §2](functional-spec.md)):

- **External / wall supply** — typically 12 V from a wall adapter. The
  battery monitor still reads `A0` for diagnostics but reports a
  fixed 100 % capacity and the inactivity-deep-sleep timer is
  disabled.
- **Battery** — 12 V SLA / LiFePO4 or 6 V SLA / LiFePO4. The monitor
  applies the appropriate built-in discharge curve; the inactivity
  timer triggers `ESP.deepSleep(0)` after the configured idle window.
  Other lithium chemistries (Li-ion, LiPo) are supported via the
  `custom` profile.

| Rail | Source | Notes |
|---|---|---|
| VIN | External 12 V or battery | Sensed on `A0` via on-board divider |
| 3V3 | NodeMCU on-board regulator | Powers the ESP8266 |
| 5 V (USB) | USB during programming | Not used in production |

The supply also powers the HT16K33 displays through their own onboard
5 V regulator (the displays accept 5 V VCC; the carrier boards on the
existing units handle the step-down).

**ADC voltage-sense calibration.** Two-point calibration in
`data/config.json` (`battery.adc_at_0v_raw`, `battery.adc_at_full_v_raw`,
`battery.adc_full_v`). Default values reproduce the legacy linear fit
`V = 0.0531·analogRead(A0) + 0.1978` and are documented as a starting
point for field calibration. The Web UI shows the current calibration
but does not edit it (see functional-spec §13.2).

**Battery thresholds and curves** are percent-based, not voltage-based,
and live in functional-spec §6. This document no longer carries
voltage-level thresholds because they vary per profile.

---

## 3. Displays

Two HT16K33-based 4-digit 7-segment displays (Adafruit-compatible).

| Display | I2C address | Role |
|---|---|---|
| `display_low`  | `0x70` | Right half of the six-digit field (digits `Y`, `Z`) |
| `display_high` | `0x71` | Left half + colon-segment for first dash (digits `X`, `Y`) |

The legacy implementation uses both displays cooperatively to render the
`XX-YY-ZZ` layout: each display contributes specific digit positions, with
the dash glyph (`0x40`) drawn into otherwise-unused digit positions to form
the separators. The renderer in this firmware preserves that layout.

| Property | Value |
|---|---|
| Brightness range | 0–15 (HT16K33 native) |
| Default brightness | `1` (matches legacy behavior; bright but not retina-searing) |
| Identify pattern | All digits show `8` for ~ 2 s, then resume normal output |

---

## 4. Switch matrix

A 4 × 5 matrix of SPST toggle switches.

- **4 column outputs**, driven LOW one at a time during the active scan
  slot, HIGH otherwise (open-drain emulated by alternating output state).
- **5 row inputs**, configured `INPUT_PULLUP`. A switch closure pulls the
  row LOW while its column is the active (LOW) output.

Scan timing:

| Parameter | Default |
|---|---|
| Per-column dwell | 10 ms |
| Full-cycle period | 40 ms (4 columns) |
| Debounce window | 4 consecutive identical samples per cell (~160 ms) |

The 20 cells are mapped to bits 0..19 of a single `uint32_t` "code state".
The displayed integer is `code_state & 0xFFFFF` rendered as decimal mod
1,000,000 (i.e. always six decimal digits or fewer; values above 999,999
are clamped to fit the six-digit field — see Functional Spec §6).

---

## 5. I2C bus

| Property | Value |
|---|---|
| SDA | GPIO0 (D3) |
| SCL | GPIO2 (D4) |
| Pull-ups | 4.7 kΩ to 3V3 (also serve the boot-strap requirements) |
| Bus speed | 100 kHz (default `Wire` clock) |
| Devices | `0x70` display, `0x71` display |

GPIO0 and GPIO2 are boot-strap pins. The displays must not pull either
line LOW during reset. The 4.7 kΩ pull-ups are sufficient against the
HT16K33's idle state.

---

## 6. Reserved / unused

| GPIO | Status |
|---|---|
| GPIO9, GPIO10 | Reserved (used for SPI flash on -12E/F modules) |
| GPIO6–GPIO11 | Not exposed; tied to flash |
| TXD0 / RXD0 | Used as GPIO1 / GPIO3 in the matrix — **serial console output is therefore limited** during normal operation. See Implementation Plan §0 for the diagnostic strategy. |

The reuse of `GPIO1 (TX)` and `GPIO3 (RX)` as matrix lines is a legacy
hardware decision. It means:

- Serial diagnostics are best-effort during normal operation. The
  framework still attempts `Serial.begin(115200)` for boot-time logs
  and for upload mode, but the matrix scanner toggles those pins as
  soon as it starts.
- Production logging relies on the in-RAM ring buffer surfaced by the
  Web UI / `/api/log` endpoint, not on serial.

> **Hardware-rework note.** This is an accepted bug in the existing
> revision and the firmware lives with it. **Any future hardware
> revision must move the matrix lines off GPIO1 / GPIO3** (and ideally
> off GPIO0 / GPIO2 as well, freeing dedicated I2C pins) to restore a
> first-class serial console. Migrating to an MCU with more usable
> GPIOs — for example ESP32-C3 or ESP32-S3 — is the cleanest path.

---

## 7. Mechanical / environmental

- Mounting: panel-mount switches and displays, no specific firmware
  implications.
- Operating temperature: indoor, 0–40 °C; the firmware does not
  thermally throttle.
- Ingress: not specified; assume dry indoor use.
