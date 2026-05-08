# px-enigma-esp8266 — Functional Specification

**Status:** Draft for review
**Version:** 0.2.0-draft

This document specifies **what the device does**. It is the primary input to
firmware code generation. Update this document before changing behavior.

Hardware references in this spec are summaries; the authoritative electrical
documents are [hardware-spec.md](hardware-spec.md) and
[pin-mapping.md](pin-mapping.md).

---

## 1. Roles and responsibilities

The firmware has six cooperating concerns running on a single non-blocking
loop:

1. **Switch-matrix scanner** — owns the raw 20-bit puzzle state, scanning
   and debouncing in software without blocking the rest of the system.
2. **Code engine** — derives the displayed code, runs the configured
   puzzle mode (live or latching), and emits structured events.
3. **Battery monitor** — samples the supply rail, applies the configured
   discharge curve, and reports voltage + remaining capacity.
4. **Sleep manager** — when on battery, runs an inactivity timer that
   triggers deep sleep and requires a power cycle to wake.
5. **MQTT command/event surface** — receives commands, applies retained
   config overrides at boot, publishes state / events / warnings / announce.
6. **Network/Web UI surface** — WiFi (STA + always-on AP), HTTP settings
   page, JSON API, OTA updates.

The **switch matrix is the source of truth for the displayed code.** The
display renderer never modifies puzzle state. The code engine never reads
from MQTT; commands flow through the command dispatcher into the engine
via explicit setters.

The device is designed to operate **standalone**. WiFi association and
MQTT broker connectivity are best-effort; their absence never blocks
puzzle play. Boot path prioritizes lighting the display and starting the
matrix scanner before any networking work begins.

---

## 2. Power source and operating profiles

The Enigma supports two power-source classes, selected by configuration
(see §14). The profile determines battery monitoring and sleep behavior.

| Profile id      | Description                  | Inactivity sleep | Capacity reporting     |
|-----------------|------------------------------|------------------|------------------------|
| `external`      | Wall / bench supply (default) | Disabled         | Always 100 %           |
| `unknown`       | Any unspecified source       | Disabled         | Always 100 %           |
| `12v-lead-acid` | 12 V SLA / AGM battery       | Enabled          | Built-in curve         |
| `12v-LiFePO4`   | 12 V LiFePO4 (4S) battery    | Enabled          | Built-in curve         |
| `6v-lead-acid`  | 6 V SLA / AGM battery        | Enabled          | Built-in curve         |
| `6v-LiFePO4`    | 6 V LiFePO4 (2S) battery     | Enabled          | Built-in curve         |
| `custom`        | Operator-supplied curve      | Enabled          | Custom curve from config |

Built-in profile names and curve formats mirror the sister
[`px-wifi-v1`](../../esp32/px-wifi-v1) project so a shared library can
later absorb the logic. Other lithium chemistries (Li-ion, LiPo) are
fully supported via the `custom` profile — see §6.

When a battery profile is selected, the firmware enables the inactivity
timer described in §7 and reports `battery.percent` derived from the
curve. When `external` or `unknown` is selected, the timer is disabled
and `battery.percent` is fixed at `100`.

---

## 3. State machine

Display / engine states (network is independent and always active):

| State        | Meaning                                                | Display                              |
|--------------|--------------------------------------------------------|--------------------------------------|
| `OFF`        | Display blank; matrix continues scanning; no MQTT events for code changes | Blank |
| `ACTIVE`     | Normal operation: live code rendering + MQTT events    | `XX-YY-ZZ`                           |
| `LATCHED`    | Latching mode only — code matched target; further changes ignored | `XX-YY-ZZ` blinking 1 Hz (full display 50 % duty) |
| `IDENTIFY`   | Brief identification flash (commanded externally)      | `8888 8888` for ~ 2 s                |
| `LOW_BATT`   | Battery percent below `low_percent`; banner overlay    | `XX-YY-ZZ` with periodic "LOW" banner |
| `CRIT_BATT`  | Battery percent at cutoff; matrix scan halted          | "CRIT" banner; no code updates       |
| `SLEEPING`   | Inactivity timeout reached on battery; deep sleep      | (powered down)                       |

Transitions:

```
boot          ──────────▶  ACTIVE  (or OFF if puzzle.start_state = "off")
ACTIVE        ──off──────▶  OFF
OFF           ──on───────▶  ACTIVE
ACTIVE        ──code matches target & mode = latching──▶  LATCHED
LATCHED       ──reset cmd / power cycle──▶  ACTIVE
*             ──identify──▶  IDENTIFY ──(timeout)──▶  previous state
ACTIVE/LATCHED ──battery < low──▶  LOW_BATT
LOW_BATT      ──battery ≥ low + hyst──▶  previous (ACTIVE or LATCHED)
*             ──battery ≤ cutoff──▶  CRIT_BATT
CRIT_BATT     ──battery ≥ cutoff + hyst──▶  ACTIVE
ACTIVE        ──no switch changes for inactivity_minutes──▶  SLEEPING (deep sleep, power cycle to wake)
```

Notes:

- `off` is allowed from any state and never affects WiFi, MQTT, or the
  Web UI.
- `identify` is allowed from any state and resumes the prior state when
  its timer expires; in `LATCHED` the latch state is preserved across
  the flash.
- `LATCHED` ignores all switch-derived code changes. The display
  continues to render the latched code, blinking. Only `reset` or a
  power cycle exits `LATCHED`.
- `CRIT_BATT` halts the matrix scanner to avoid drawing the supply down
  during a brownout.
- `SLEEPING` is a deep-sleep terminal state on battery profiles. Wake is
  **only** by power cycle; this is intentional and documented for
  operators (see §7).

---

## 4. Switch-matrix scanner

- **Geometry:** 4 column outputs × 5 row inputs = 20 cells.
- **Software debouncing.** No hardware debounce; entirely in firmware.
- **Configuration.** `scan.poll_interval_ms` and `scan.debounce_samples`
  are settable in `data/config.json` only. They are **deliberately not
  exposed in the Web UI** — they are tuning parameters intended to be
  set during commissioning, not by end-users.

| Parameter | Default | Notes |
|---|---|---|
| `scan.poll_interval_ms` | `10` | Per-column dwell |
| `scan.debounce_samples` | `4`  | Consecutive identical samples required to flip a cell (~ 160 ms at defaults) |

- **Output:** a single `uint32_t` whose low 20 bits represent the matrix
  state. Bit-to-cell mapping is fixed (see [pin-mapping.md](pin-mapping.md)).
- **Idle behavior:** between dwells the column outputs are driven HIGH
  so no row is being actively pulled.
- **Yield discipline:** the scanner returns to the cooperative loop
  after every column dwell — never blocks for a full cycle.

The 20-bit value is exposed as a stable read to consumers. `code_engine`
reads it once per cooperative iteration and decides whether anything has
changed.

Every confirmed switch-state change resets the inactivity timer (§7) so
active play prevents deep sleep regardless of how many transitions
occur.

---

## 5. Code engine

### 5.1 Displayed code derivation

The displayed integer is `code_state mod 1,000,000`. The displayed
*string* is the integer formatted as six decimal digits with leading
zeros, separated into pairs by ASCII hyphens:

```
123456  →  "12-34-56"
   123  →  "00-01-23"
```

### 5.2 Puzzle modes

The mode is a configuration value and may be changed at runtime via
`setMode`.

#### `live` mode (default)

- Every confirmed code change publishes a `code_changed` event.
- If `code.target` is set, every transition across the match boundary
  publishes an event:
  - **unmatched → matched:** `code_solved` — emitted every time the
    code enters the matching state, including after it has been solved,
    changed away, and re-solved. There is no single-shot guard;
    `code_solved` fires on each individual solve transition.
  - **matched → unmatched:** `code_unsolved` — emitted every time the
    code leaves the matching state.
- The display always reflects the live matrix state.

#### `latching` mode

- Behaves identically to `live` mode while the displayed code does not
  match `code.target`.
- The first time the code matches the target, the engine:
  1. Snapshots the matched code.
  2. Publishes a `solve` event (single-shot; not republished on
     retries).
  3. Transitions to `LATCHED` (§3).
  4. Drives the display to blink the matched code at 1 Hz.
- In `LATCHED`, switch-state changes are still scanned and reported in
  `state.code.code_bits` (so an operator can confirm the matrix is
  alive) but are **not** rendered on the display and do not trigger
  `code_changed` / `code_solved` / `code_unsolved` events.
- `LATCHED` is left only by:
  - the `reset` MQTT command (returns to `ACTIVE`, clears the latch
    silently — no spurious `code_unsolved`),
  - or a power cycle.

`latching` mode requires `code.target` to be set; if it is null the
engine logs a warning and falls back to `live` until a target is
provided.

### 5.3 Code format on the wire

The code is reported in three interchangeable forms in every event /
state payload:

| Field | Type | Example |
|---|---|---|
| `code` | string, hyphenated | `"12-34-56"` |
| `code_int` | integer | `123456` |
| `code_bits` | integer (20-bit) | `87654` |

`code_bits` is the raw matrix state, useful for debugging and for the
authoring tool described in §16.

### 5.4 Inputs accepted on `setTarget`

`target` may be:

1. A JSON integer (`123456`).
2. A JSON string of digits (`"123456"`, `"00123"` — left-padded to 6).
3. A JSON string of hyphenated digits (`"12-34-56"`, `"1-2-3"`).
4. `null` to clear the target.

Anything else → `command_failed` with warning code `invalid_code_format`.

---

## 6. Battery monitor

The battery monitor mirrors the px-wifi-v1 design so the two firmwares
share a vocabulary and (eventually) a library.

### 6.1 Sampling

- ADC source: `A0` on the ESP8266 (10-bit raw range 0..1023; the legacy
  carrier scales the supply rail into the ADC's 0..1 V range via an
  on-board resistor divider).
- Sample period: `battery.sample_interval_ms` (default 10 000 ms).
- Smoothing: trailing 4-sample average; rejects single-sample noise.

### 6.2 Calibration

Two-point calibration, configurable but not exposed in the Web UI:

| Field | Meaning |
|---|---|
| `battery.adc_at_0v_raw`     | ADC raw reading when the input rail is 0 V |
| `battery.adc_at_full_v_raw` | ADC raw reading at `battery.adc_full_v` |
| `battery.adc_full_v`        | The reference voltage used for the upper calibration point (typically the nominal full-charge voltage of the battery, or a known bench supply value) |

Voltage from raw:

```
voltage_v = (raw - adc_at_0v_raw) × adc_full_v / (adc_at_full_v_raw - adc_at_0v_raw)
```

Default values reproduce the legacy linear fit
(`V = 0.0531·raw + 0.1978`) and are documented as a starting point for
field calibration.

### 6.3 Built-in discharge curves

Each built-in profile carries an array of `(voltage_v, percent)` points
sorted by descending voltage. Capacity is computed by piecewise-linear
interpolation between the two points bracketing the measured voltage.
Above the highest point → 100 %. Below the lowest → 0 %.

| Profile         | Curve (voltage_v → percent)                                                                                  |
|-----------------|--------------------------------------------------------------------------------------------------------------|
| `12v-lead-acid` | 12.85 → 100, 12.65 → 90, 12.45 → 75, 12.30 → 60, 12.10 → 40, 11.95 → 20, 11.80 → 5, 11.60 → 0                 |
| `12v-LiFePO4`   | 13.60 → 100, 13.30 → 95, 13.20 → 80, 13.10 → 60, 13.00 → 40, 12.90 → 20, 12.50 → 10, 11.20 → 0                |
| `6v-lead-acid`  | 6.60 → 100,  6.45 → 95,  6.35 → 85,  6.25 → 70,  6.15 → 55,  6.05 → 35,  5.95 → 15, 5.85 → 0                  |
| `6v-LiFePO4`    | 6.80 → 100,  6.65 → 95,  6.60 → 80,  6.55 → 60,  6.50 → 40,  6.45 → 20,  6.30 → 10, 5.60 → 0                  |

Numbers are starting values aligned with the px-wifi-v1 reference and
should be revisited per-deployment. The exact arrays live in
`src/battery_profiles.cpp`.

### 6.4 Custom curves

`battery.profile = "custom"` activates a user-supplied curve provided in
`battery.points`. Two equivalent forms are accepted:

CSV string (matches px-wifi-v1):

```json
"battery": {
  "profile": "custom",
  "points": "12.85:100,12.65:90,12.45:75,12.10:40,11.80:0"
}
```

Object array (preferred for readability):

```json
"battery": {
  "profile": "custom",
  "points": [
    { "v": 12.85, "p": 100 },
    { "v": 12.65, "p": 90 },
    { "v": 12.45, "p": 75 },
    { "v": 12.10, "p": 40 },
    { "v": 11.80, "p": 0 }
  ]
}
```

Validation rules:

- Minimum 2 points, maximum 20.
- Voltages monotonically decreasing; percents monotonically decreasing.
- Percents clamped to `0..100`; voltages clamped to `1.0..20.0`.
- Invalid curves → `config_invalid` warning; firmware falls back to
  `unknown` (always 100 %).

### 6.5 Thresholds

| Field | Default | Meaning |
|---|---|---|
| `battery.low_percent`    | 40 | Enter `LOW_BATT` below this |
| `battery.cutoff_percent` | 10 | Enter `CRIT_BATT` below this |
| `battery.hysteresis_pct` | 5  | Required to clear back to a healthier state |

Thresholds are **percent-based** (not voltage-based) so the same numbers
work across profiles.

### 6.6 State reporting

Reported in every `state` snapshot:

```json
"battery": {
  "profile": "12v-lead-acid",
  "voltage_v": 12.62,
  "percent": 78,
  "status": "ok"
}
```

`status ∈ { "ok", "low", "critical", "external" }`. `external` is used
when the profile is `external` or `unknown`.

---

## 7. Sleep manager (battery profiles only)

When the configured profile is anything other than `external` or
`unknown`, the firmware runs an inactivity timer:

- Default `battery.inactivity_minutes`: **60** (configurable).
- The timer is reset by **any confirmed switch-state change**.
- The timer is **not** reset by MQTT activity, Web UI activity, or
  display redraws — only by player input.
- When the timer elapses the firmware:
  1. Publishes a `going_to_sleep` event with the elapsed-idle minutes.
  2. Flushes the log ring buffer.
  3. Calls `ESP.deepSleep(0)` so the device sleeps until reset.

A power cycle (or external reset) is required to wake. This is
intentional: the prop's normal lifecycle is "powered on for the
duration of a game, off between games", so requiring a power cycle is a
feature rather than a limitation.

The Web UI exposes the inactivity timeout as a single field. Setting it
to `0` disables the auto-sleep behavior even on battery profiles.

Light sleep is **not** used. The firmware needs the matrix scanner and
display to be live whenever the device is on, and player switch-throws
need to register without a wake-from-sleep latency.

---

## 8. Display behavior

### 8.1 Code rendering

- Format: `XX-YY-ZZ` rendered across the two HT16K33 displays per the
  legacy digit-position layout (see hardware-spec §3). The legacy
  position layout is preserved verbatim — see §15 (resolved open
  questions) for rationale.
- Brightness: `0..15` (HT16K33 native). The configured
  `display.brightness` is applied at boot. `setBrightness` (MQTT or Web
  UI) updates immediately and persists by default; `persist: false`
  makes it a session-only override.

### 8.2 State-specific rendering

- `IDENTIFY` shows `8888 8888` (all segments lit) for the configured
  identify duration (default 2 000 ms).
- `OFF` blanks both displays; the matrix continues scanning so a
  subsequent `on` command brings up the correct current code
  immediately.
- `LATCHED` blinks the entire display at 1 Hz (500 ms on, 500 ms off).
- `LOW_BATT` overlays a "LOW" banner for ~ 3 s when first entered, then
  resumes the live code with no further interruptions until the state
  changes.
- `CRIT_BATT` shows "CRIT" continuously until the rail recovers.

### 8.3 Boot priority

The display renderer initializes **before** WiFi / MQTT to minimize the
time between power-on and first code rendering. Target cold-boot to
first lit code: ≤ 1.5 s on healthy hardware.

### 8.4 Optional WiFi/MQTT signal indicator

`signal_indicator.enabled` controls whether the decimal-point lamps
are used to show network health. The setting is:

- **Stored in** `data/config.json` (persisted across reboots).
- **Editable via** the Web UI under **Display → Signal indicator**.
- Changeable at runtime via the `setSignalIndicator` MQTT command.

When enabled, the firmware lights the HT16K33 decimal-point lamps to
indicate connectivity. The `XX-YY-ZZ` glyph field always uses 8 digit
positions (6 numeric + 2 dashes); only the decimal-point lamps are
repurposed.

- Decimal points 0..6 (left-to-right) indicate STA RSSI as a 0–7 bar.
- Decimal point 7 (rightmost) indicates MQTT broker connectivity.

The 7 RSSI threshold values (`signal_indicator.rssi_dbm`) are stored
in `data/config.json` and are **not** editable in the Web UI (they are
advanced calibration values shown read-only in the diagnostics panel).
The defaults match the table below and are suitable for most
installations.

Default RSSI thresholds:

| Lit dots | RSSI (dBm) |
|---|---|
| 7 | ≥ -55 |
| 6 | ≥ -60 |
| 5 | ≥ -65 |
| 4 | ≥ -70 |
| 3 | ≥ -75 |
| 2 | ≥ -80 |
| 1 | ≥ -85 |
| 0 | < -85 or STA disconnected |

The MQTT dot is binary: lit if `mqtt.connected == true`, dark
otherwise. Signal indication is suppressed during `IDENTIFY`,
`LOW_BATT` banner, and `CRIT_BATT`. During `LATCHED` blinking the
indicator is drawn during the on-phase only (it inherits the blink).

---

## 9. WiFi

The ESP8266 supports simultaneous Station + Access Point
(`WIFI_AP_STA`) mode. The Enigma runs in **AP+STA mode at all times**,
matching the sister `px-clock-esp8266` design.

- **STA:** Two configured `(ssid, password)` pairs (primary + backup)
  via `ESP8266WiFiMulti`. Continuous, non-blocking reconnect.
- **AP:** Always on. SSID is derived from `device.prop_name`,
  normalized for hostnames (lowercase, spaces replaced with `-`).
  Default AP password `MCEscher`.
- **AP IP:** `192.168.4.1` (fixed via `softAPConfig`).
- **Standalone tolerance.** WiFi association is non-blocking; the
  puzzle is fully playable from the moment the display lights,
  regardless of WiFi state.
- **Fast cold boot.** WiFi initialization yields cooperatively; the
  scanner / display path is never blocked waiting for association.

If continuous AP+STA causes thermal or stability issues observed during
soak, the firmware will fall back to "AP only when STA disconnected for
> 30 s" without changing the public Web UI behavior.

---

## 10. MQTT — topic layout

The base topic is configured in the Web UI (default
`paradox/site1/enigma1`). The four core sub-topics are **always**
derived from it:

| Purpose   | Topic                       | Direction | Retain | Payload                |
|-----------|-----------------------------|-----------|--------|------------------------|
| Commands  | `<base_topic>/commands`     | in        | off    | JSON                   |
| State     | `<base_topic>/state`        | out       | off    | JSON                   |
| Events    | `<base_topic>/events`       | out       | off    | JSON                   |
| Warnings  | `<base_topic>/warnings`     | out       | off    | JSON                   |
| Override  | `<base_topic>/config`       | in        | **on** | JSON (partial config)  |

Plus one independent topic, configured separately and **not** derived
from the base:

| Purpose  | Topic                                        | Direction | Retain | Payload |
|----------|----------------------------------------------|-----------|--------|---------|
| Announce | `<announce_topic>` (default `paradox/props`) | out       | off    | JSON    |

QoS defaults: 1 for every topic.

### 10.1 Retained config override

`<base_topic>/config` is a **retained** topic that lets an operator
push configuration overrides without touching `data/config.json` or the
Web UI. On every MQTT (re)connect:

1. The firmware subscribes to `<base_topic>/config`.
2. If the broker delivers a retained payload, the firmware merges it
   over the in-memory config:
   - Top-level keys not present in the override are left unchanged.
   - Top-level keys present in the override fully replace the
     corresponding key in the live config (no deep merge — predictable
     and easier to reason about).
3. The merged result is applied **silently** — no log noise, no event
   storm, no Web UI prompt.
4. A single `config_override_applied` event is published with a
   redacted summary of which top-level keys were overridden (no
   values, no credentials).
5. Overrides are **not persisted** to `data/config.json`. They live
   only in RAM. A reboot reloads `data/config.json` and re-applies
   whatever override is still retained on the broker.

To clear the override, publish an empty retained payload (`""`) to the
topic.

This mechanism is intended for fleet management: a single retained
publish can switch the entire fleet's MQTT broker host, brightness, or
puzzle mode without redeploying credentials.

---

## 11. MQTT — commands

All command payloads are JSON. Required envelope:

```json
{
  "command": "lowerCamelCase",
  "request_id": "optional-opaque-string",
  "...": "command-specific fields"
}
```

`request_id` is echoed verbatim into every outcome event.

### 11.1 Workspace-required commands

| Command | Effect |
|---|---|
| `getState` | Publish a fresh `state` snapshot immediately. |
| `restart`  | Reboot the device. |
| `identify` | Enter `IDENTIFY` for `identify_duration_ms`, then resume prior state. |
| `ping`     | Outcome event only (`pong`). |

### 11.2 Project commands

| Command              | Fields                                                | Effect |
|----------------------|-------------------------------------------------------|--------|
| `setBrightness`      | `brightness` 0–15; optional `persist` bool (default `true`) | Apply immediately; persist unless `persist:false`. |
| `setTarget`          | `target` per §5.4                                     | Update target; emits `code_solved` (live) or `solve` (latching) immediately if the current code already matches. |
| `clearTarget`        | —                                                     | Equivalent to `setTarget` with `target: null`. |
| `setMode`            | `mode` ∈ `"live"` / `"latching"`                      | Switch puzzle mode. If currently `LATCHED` and mode goes to `live`, the latch is cleared (equivalent to `reset`). |
| `reset`              | —                                                     | Clear `LATCHED` and return to `ACTIVE`. No-op if not latched. |
| `getCode`            | —                                                     | Publish a `code_changed` event with the current code immediately. |
| `setBatteryProfile`  | `profile`, optional `points` for `custom`             | Apply + persist. Validates curve. |
| `on`                 | —                                                     | Leave `OFF`; resume `ACTIVE`. No-op if already on. |
| `off`                | —                                                     | Enter `OFF`. No-op if already off. |
| `setSignalIndicator` | `enabled` bool, optional `rssi_dbm` array of 7 thresholds | Toggle / retune the decimal-point indicator. |

### 11.3 Validation

- Unknown commands → `command_failed` with warning code
  `unknown_command`.
- Out-of-range fields → `command_failed` with warning code
  `invalid_argument` and `data.field` naming the offending field.
- Malformed JSON → silently dropped at the MQTT layer; one
  `invalid_payload` warning per minute is published as a rate limit.

### 11.4 Outcome events

Every command produces, in order:

1. `command_received` — echo of the command name + `request_id`.
2. Exactly one of:
   - `command_success`,
   - `command_warning` (succeeded but with an advisory),
   - `command_failed`.

---

## 12. MQTT — state, events, warnings, announce

### 12.1 State payload

Published on:

- Boot (after MQTT connect).
- Every `heartbeat_interval_ms` (default 10 000 ms).
- On demand via `getState`.
- On any state-machine transition, brightness change, mode change, or
  target change.

```json
{
  "timestamp": "2026-05-07T16:42:00.123Z",
  "application": "px-enigma-esp8266",
  "instance": "site1-enigma1",
  "version": "0.2.0",
  "status": "active",
  "uptime_s": 1234,
  "health": {
    "free_heap_bytes": 38192,
    "min_free_heap_bytes": 37744
  },
  "wifi": {
    "sta": { "ssid": "guest", "rssi": -62, "ip": "192.168.1.42", "connected": true },
    "ap":  { "ssid": "px-enigma-a1b2", "ip": "192.168.4.1", "clients": 0 }
  },
  "mqtt": { "connected": true, "broker": "mqtt.local:1883" },
  "puzzle": { "mode": "latching", "latched": false },
  "code": {
    "code": "12-34-56",
    "code_int": 123456,
    "code_bits": 87654,
    "target": "12-34-56",
    "solved": true
  },
  "display": { "brightness": 1, "blanked": false, "signal_indicator": true },
  "battery": {
    "profile": "12v-lead-acid",
    "voltage_v": 12.62,
    "percent": 78,
    "status": "ok"
  },
  "sleep": { "inactivity_minutes": 60, "idle_minutes": 3 }
}
```

### 12.2 Events

| Event name                | When emitted                                  | Notable `data` fields                  |
|---------------------------|-----------------------------------------------|----------------------------------------|
| `command_received`        | every command                                 | `command`, `request_id`                |
| `command_success`         | command succeeded                             | `command`, `request_id`                |
| `command_warning`         | command succeeded with advisory               | `command`, `request_id`, `warning`     |
| `command_failed`          | command rejected                              | `command`, `request_id`, `warning`, `data` |
| `code_changed`            | displayed code transitioned                   | `code`, `code_int`, `code_bits`        |
| `code_solved`             | live mode: code matches target (transition)   | `code`, `target`                       |
| `code_unsolved`           | live mode: code no longer matches target      | `code`, `target`                       |
| `solve`                   | **latching mode**: target reached, latching engaged | `code`, `target`                |
| `unlatch`                 | latching cleared by `reset`                   | `code`                                 |
| `state_changed`           | display state machine transition              | `from`, `to`                           |
| `battery_low`             | entering `LOW_BATT`                           | `voltage_v`, `percent`                 |
| `battery_critical`        | entering `CRIT_BATT`                          | `voltage_v`, `percent`                 |
| `battery_normal`          | recovering to `ok`                            | `voltage_v`, `percent`                 |
| `going_to_sleep`          | inactivity timer elapsed                      | `idle_minutes`                         |
| `config_override_applied` | retained config consumed at (re)connect       | `keys` (top-level keys merged)         |
| `pong`                    | response to `ping`                            | `request_id`                           |

Event envelope:

```json
{
  "timestamp": "2026-05-07T16:42:00.456Z",
  "type": "code",
  "event": "solve",
  "message": "Puzzle solved (latched at 12-34-56)",
  "data": { "code": "12-34-56", "target": "12-34-56" }
}
```

`type` is one of `command`, `code`, `battery`, `state`, `system`,
`config`.

### 12.3 Warnings

Published to `<base_topic>/warnings`:

| Code                    | Cause                                              |
|-------------------------|----------------------------------------------------|
| `unknown_command`       | Command name not recognized                        |
| `invalid_argument`      | Field present but out of range / wrong type        |
| `invalid_code_format`   | `setTarget` payload could not be parsed            |
| `invalid_payload`       | Malformed JSON on `commands` or `config` topics    |
| `config_invalid`        | Persisted config rejected; defaults applied        |
| `battery_low`           | Mirror of the `battery_low` event                  |
| `battery_critical`      | Mirror of the `battery_critical` event             |
| `mqtt_publish_failed`   | One or more publishes failed; rate-limited to 1/min |

### 12.4 Announce

Published once per MQTT connect to `<announce_topic>` (default
`paradox/props`):

```json
{
  "timestamp": "2026-05-07T16:42:00.000Z",
  "event": "online",
  "application": "px-enigma-esp8266",
  "instance": "site1-enigma1",
  "prop_name": "Front Hall Enigma",
  "version": "0.2.0",
  "ip": "192.168.1.42",
  "ap_ip": "192.168.4.1",
  "mdns": "front-hall-enigma",
  "mdns_fqdn": "front-hall-enigma.local",
  "mac": "AA:BB:CC:DD:EE:FF",
  "base_topic": "paradox/site1/enigma1",
  "commands_topic": "paradox/site1/enigma1/commands",
  "config_topic": "paradox/site1/enigma1/config"
}
```

---

## 13. Web UI

A single-page UI served from LittleFS on port 80, reachable on both AP
and STA interfaces, no authentication on the LAN-side endpoints.

### 13.1 Fields exposed in the UI

- Identity: `device.prop_name`, `device.instance`.
- WiFi: primary + backup SSID/password, AP password.
- MQTT: host, port, username, password, base topic, announce topic,
  heartbeat interval.
- Puzzle: mode (`live` / `latching`), target code, brightness, identify
  duration.
- Battery: profile selector (drop-down), inactivity timeout (minutes),
  low / cutoff / hysteresis percent.
- Display: signal-indicator on/off.
- Diagnostics (read-only): live state snapshot, last 32 log lines, raw
  20-bit matrix, raw ADC reading.

### 13.2 Fields **not** exposed in the UI

These live in `data/config.json` only:

- `scan.poll_interval_ms`, `scan.debounce_samples` (commissioning
  parameters).
- `battery.adc_at_0v_raw`, `battery.adc_at_full_v_raw`,
  `battery.adc_full_v` (calibration constants).
- `battery.points` for a `custom` profile (curve authoring).
- `signal_indicator.rssi_dbm` (threshold tuning).

The UI shows the *names* and *current values* of these fields in the
read-only diagnostics panel so the operator can confirm what is in
effect, but provides no editor.

### 13.3 API endpoints

| Method | Path             | Behavior                                                               |
|--------|------------------|------------------------------------------------------------------------|
| `GET`  | `/`              | Single-page UI                                                         |
| `GET`  | `/api/config`    | Returns current `config.json` (with secrets redacted unless `?reveal=1`) |
| `POST` | `/api/config`    | Replaces `config.json`; live-applies what it can                       |
| `GET`  | `/api/state`     | Same payload as MQTT `state`                                           |
| `GET`  | `/api/log`       | Tail of the in-RAM log ring buffer                                     |
| `POST` | `/api/identify`  | Equivalent to MQTT `identify`                                          |
| `POST` | `/api/reset`     | Equivalent to MQTT `reset`                                             |
| `POST` | `/api/restart`   | Equivalent to MQTT `restart`                                           |
| `POST` | `/update`        | HTTP OTA upload (password-protected)                                   |

---

## 14. Configuration schema and persistence

### 14.1 File location and policy

- `data/config.json` is **not committed**. It is `.gitignored`.
- `config.json.example` (at repo root) is committed and serves as the
  schema reference.
- `pio run -t uploadfs` flashes the contents of `data/` verbatim. If
  no `data/config.json` exists locally, **nothing is flashed for it** —
  the firmware generates a defaults-only config on first boot, the
  same one it would generate if the file were corrupted.
- The boot sequence is:
  1. Mount LittleFS.
  2. Load `/config.json`; on failure, log a `config_invalid` warning
     and synthesize defaults.
  3. Bring up display + scanner immediately.
  4. Bring up WiFi.
  5. Connect MQTT and apply any retained `<base_topic>/config`
     override (§10.1).

### 14.2 Schema

```json
{
  "device": {
    "prop_name": "Front Hall Enigma",
    "instance": "site1-enigma1"
  },
  "wifi": {
    "primary":  { "ssid": "", "password": "" },
    "backup":   { "ssid": "", "password": "" },
    "ap":       { "password": "MCEscher" }
  },
  "mqtt": {
    "host": "",
    "port": 1883,
    "username": "",
    "password": "",
    "base_topic": "paradox/site1/enigma1",
    "announce_topic": "paradox/props",
    "heartbeat_interval_ms": 10000
  },
  "puzzle": {
    "mode": "live",
    "target": null,
    "identify_duration_ms": 2000,
    "start_state": "active"
  },
  "display": {
    "brightness": 1
  },
  "signal_indicator": {
    "enabled": true,
    "rssi_dbm": [-55, -60, -65, -70, -75, -80, -85]
  },
  "battery": {
    "profile": "external",
    "points": null,
    "low_percent": 40,
    "cutoff_percent": 10,
    "hysteresis_pct": 5,
    "sample_interval_ms": 10000,
    "inactivity_minutes": 60,
    "adc_at_0v_raw": 0,
    "adc_at_full_v_raw": 1023,
    "adc_full_v": 15.0
  },
  "scan": {
    "poll_interval_ms": 10,
    "debounce_samples": 4
  }
}
```

`battery.points` is ignored unless `battery.profile == "custom"`.
`puzzle.start_state` is `"active"` (default) or `"off"`.

---

## 15. Resolved open questions

These are concrete decisions from prior reviews; recorded here so future
maintainers don't re-litigate them.

1. **HT16K33 digit-position layout — replicate legacy verbatim.** The
   legacy `enigma.ino` addresses specific digit positions on each
   display to render `XX-YY-ZZ`. The renderer in this firmware
   reproduces that mapping bit-for-bit. A unit test (Phase 9) pins the
   mapping down.

2. **GPIO1 / GPIO3 reuse for matrix lines — accepted hardware bug.**
   The legacy unit reuses the UART TX/RX pins as matrix lines, which
   disables the serial console at runtime. This is a known hardware
   issue and the firmware accepts it: production logging relies on the
   in-RAM ring buffer surfaced by the Web UI / `/api/log` endpoint.
   **If the hardware is ever revised, this should be addressed by
   moving the matrix lines to other GPIOs (or by switching to an MCU
   with more usable pins, e.g., ESP32-C3 / ESP32-S3) to restore a
   first-class serial console.** The pin-mapping document carries the
   same warning for hardware authors.

3. **Battery calibration in config file only.** The voltage-divider
   calibration constants and custom-curve points live in
   `data/config.json`. The Web UI exposes only the *profile selector*
   and the high-level thresholds (low / cutoff / hysteresis percent).
   Field calibration is performed by editing `data/config.json` and
   reflashing the FS image, or by `POST /api/config` over the network.

---

## 16. Authoring tool — code-to-matrix script

The repository ships an interactive POSIX shell script,
`tools/code-to-matrix.sh`, that converts a desired six-digit code (in
`XX-YY-ZZ` form) to a switch-state pattern. It exists so puzzle
designers can author target codes without running the firmware.

- **No Node.js / Python dependency.** Pure POSIX `sh`.
- **Configurable matrix shape.** Any rectangular layout from `1×20` to
  `20×1`. Default is the `5×4` (5 columns × 4 rows) layout from
  [pin-mapping.md](pin-mapping.md).
- **Numbering.** Switch index 1 is the top-left cell. Numbering
  proceeds left-to-right, then top-to-bottom (so the top row is
  switches `1..cols`, the second row is `cols+1..2·cols`, etc.).
- **Output.** A list of `0` (open) and `1` (closed) values, one per
  switch, plus the equivalent decimal `code_int` and the 20-bit
  `code_bits` value.

Implementation details (bit layout, default prompt values) are
documented in the script's header comment.
