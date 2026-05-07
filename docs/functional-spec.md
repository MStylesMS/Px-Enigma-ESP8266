# px-enigma-esp8266 — Functional Specification

**Status:** Draft for review
**Version:** 0.1.0-draft

This document specifies **what the device does**. It is the primary input to
firmware code generation. Update this document before changing behavior.

Hardware references in this spec are summaries; the authoritative electrical
documents are [hardware-spec.md](hardware-spec.md) and
[pin-mapping.md](pin-mapping.md).

---

## 1. Roles and responsibilities

The firmware has four cooperating concerns running on a single non-blocking
loop:

1. **Switch-matrix scanner** — owns the raw 20-bit puzzle state, scanning
   and debouncing without blocking the rest of the system.
2. **Code engine** — derives the displayed code, optionally compares it to
   a configured target, and emits structured events on transitions.
3. **MQTT command/event surface** — receives commands, publishes state /
   events / warnings / announce.
4. **Network/Web UI surface** — WiFi (STA + always-on AP), HTTP settings
   page, JSON API, OTA updates.

The **switch matrix is the source of truth for the displayed code.** The
display renderer never modifies puzzle state; it only reads from the code
engine. The code engine never reads from MQTT; commands flow through the
command dispatcher into the engine via explicit setters.

The device is **permanently powered** from a 12 V supply. There is no sleep
mode, no deep sleep, and no battery management. "Off" means the display is
blanked — the network surface, MQTT client, and Web UI remain fully active.

---

## 2. State machine

Display / engine states (network is independent and always active):

| State      | Meaning                                                | Display behavior                  |
|------------|--------------------------------------------------------|-----------------------------------|
| `OFF`      | Display blank; matrix continues scanning; no MQTT events for code changes | Blank |
| `ACTIVE`   | Normal operation: live code rendering + MQTT events    | `XX-YY-ZZ`                        |
| `IDENTIFY` | Brief identification flash (commanded externally)      | `8888 8888` for ~ 2 s             |
| `LOW_BATT` | Battery rail under `low_v`; banner overlay periodically | `XX-YY-ZZ` with periodic "LOW" banner |
| `CRIT_BATT`| Battery rail under `critical_v`; matrix scan halted     | "CRIT" banner; no code updates     |

Transitions:

```
boot      ────▶  ACTIVE
ACTIVE    ──off──▶  OFF
OFF       ──on ──▶  ACTIVE
*         ──identify──▶  IDENTIFY ──(timeout)──▶  previous state
ACTIVE    ──battery < low_v──▶  LOW_BATT
LOW_BATT  ──battery ≥ low_v + hyst──▶  ACTIVE
*         ──battery < critical_v──▶  CRIT_BATT
CRIT_BATT ──battery ≥ critical_v + hyst──▶  ACTIVE
```

`off` is allowed from any state and never affects WiFi, MQTT, or the Web UI.
`identify` is allowed from any state and resumes the prior state when its
timer expires.

`CRIT_BATT` is the only state in which the matrix scanner is halted; it
prevents drawing the column-output transistors hard during a brownout.

---

## 3. Switch matrix scanner

- **Geometry:** 4 column outputs × 5 row inputs = 20 cells.
- **Scan dwell:** 10 ms per column (configurable; default mirrors the
  legacy unit).
- **Debounce:** N consecutive identical samples per cell (default `N = 4`).
- **Output:** a single `uint32_t` whose low 20 bits represent the matrix
  state. Bit-to-cell mapping is fixed (see [pin-mapping.md](pin-mapping.md)).
- **Idle behavior:** between dwells the column outputs are driven HIGH so
  no row is being actively pulled.
- **Yield discipline:** the scanner returns to the cooperative loop after
  every column dwell — never blocks for a full cycle.

The 20-bit value is exposed as a stable read to consumers. `code_engine`
reads it once per cooperative iteration and decides whether anything has
changed.

---

## 4. Code engine

### 4.1 Displayed code derivation

The displayed integer is `code_state mod 1,000,000`. In the legacy unit
this matched the available six-digit field; the modulo is preserved here
for backwards compatibility with existing target codes.

The displayed *string* is the integer formatted as six decimal digits with
leading zeros, separated into pairs by ASCII hyphens:

```
123456  →  "12-34-56"
   123  →  "00-01-23"
```

### 4.2 Target matching

Optional. When `code.target` is set:

- On every code transition, compare the new displayed integer to the
  target.
- On a `not-matching → matching` transition, emit a `code_solved` event
  exactly once.
- On a `matching → not-matching` transition, emit a `code_unsolved` event
  exactly once.
- The target is set via the `setTarget` command or the Web UI; cleared by
  `clearTarget` or by setting it to `null`.

The orchestrator is responsible for any subsequent game logic. The Enigma
does not lock, latch, or otherwise debounce solved-state internally — every
genuine transition produces an event.

### 4.3 Code format on the wire

The code is reported in three interchangeable forms in every event /
state payload:

| Field | Type | Example |
|---|---|---|
| `code` | string, hyphenated | `"12-34-56"` |
| `code_int` | integer | `123456` |
| `code_bits` | integer (20-bit) | `87654` |

`code_bits` is the raw matrix state and is provided primarily for
debugging and authoring custom puzzles.

### 4.4 Inputs accepted on `setTarget`

`target` may be:

1. A JSON integer (`123456`).
2. A JSON string of digits (`"123456"`, `"00123"` — left-padded to 6).
3. A JSON string of hyphenated digits (`"12-34-56"`, `"1-2-3"`).
4. `null` to clear the target.

Anything else → `command_failed` with warning code `invalid_code_format`.

---

## 5. Display behavior

- Format: `XX-YY-ZZ` rendered across the two HT16K33 displays per the
  legacy digit-position layout (see hardware-spec §3).
- Brightness: `0..15` (HT16K33 native).
  - The configured `display.brightness` is applied at boot.
  - `setBrightness` (MQTT or Web UI) updates immediately.
  - Persisting the change is the **default**; an optional `persist: false`
    flag on the command makes it a session-only override.
- `IDENTIFY` shows `8888 8888` (all segments lit) for the configured
  identify duration (default 2 000 ms).
- `OFF` blanks both displays; the matrix continues scanning so a
  subsequent `on` command brings up the correct current code immediately.
- `LOW_BATT` overlays a "LOW" banner for ~ 3 s when first entered, then
  resumes the live code with no further interruptions until the state
  changes.
- `CRIT_BATT` shows "CRIT" continuously until the rail recovers.

Update rate: event-driven. The display is only redrawn when the code
changes, the brightness changes, or a state transition demands a banner.

---

## 6. Battery monitor

- Sample period: 10 s (configurable).
- Sample averaging: trailing 4 samples (rejects single-sample noise).
- Calibration: `V = 0.0531 × analogRead(A0) + 0.1978`.
- Thresholds: `low_v` (default 12.35) and `critical_v` (default 12.10),
  both with `hysteresis_v` (default 0.10) before clearing.
- Reported in `state` as `battery: { voltage_v, status }` where
  `status ∈ { "ok", "low", "critical" }`.
- Transitions emit `battery_low`, `battery_critical`, `battery_normal`
  events and the `battery_*` warning codes (see §10).

---

## 7. WiFi

The ESP8266 supports simultaneous Station + Access Point (`WIFI_AP_STA`)
mode. The Enigma runs in **AP+STA mode at all times**, matching the
sister `px-clock-esp8266` design.

- **STA:** Two configured `(ssid, password)` pairs (primary + backup) via
  `ESP8266WiFiMulti`. Continuous, non-blocking reconnect.
- **AP:** Always on. SSID is derived from `device.prop_name`, normalized
  for hostnames (lowercase, spaces replaced with `-`); same name is used
  for mDNS without the `.local` suffix. Default AP password `MCEscher`.
- **AP IP:** `192.168.4.1` (fixed via `softAPConfig`).
- The Web UI is reachable on **both** the AP and STA interfaces.
- AP follows the STA's channel when STA is associated (ESP8266 hardware
  constraint).

If continuous AP+STA causes thermal or stability issues observed during
soak, the firmware will fall back to "AP only when STA disconnected for
> 30 s" without changing the public Web UI behavior.

---

## 8. MQTT — topic layout

The base topic is configured in the Web UI (default
`paradox/site1/enigma1`). The four core sub-topics are **always**
derived from it:

| Purpose   | Topic                       | Direction | Payload |
|-----------|-----------------------------|-----------|---------|
| Commands  | `<base_topic>/commands`     | in        | JSON    |
| State     | `<base_topic>/state`        | out       | JSON    |
| Events    | `<base_topic>/events`       | out       | JSON    |
| Warnings  | `<base_topic>/warnings`     | out       | JSON    |

Plus one independent topic, configured separately and **not** derived
from the base:

| Purpose  | Topic                                      | Direction | Payload |
|----------|--------------------------------------------|-----------|---------|
| Announce | `<announce_topic>` (default `paradox/props`) | out     | JSON    |

QoS defaults: 1 for every topic. Retain off everywhere by default.

---

## 9. MQTT — commands

All command payloads are JSON. Required envelope:

```json
{
  "command": "lowerCamelCase",
  "request_id": "optional-opaque-string",
  "...": "command-specific fields"
}
```

`request_id` is echoed verbatim into every outcome event.

### 9.1 Workspace-required commands

| Command | Effect |
|---|---|
| `getState` | Publish a fresh `state` snapshot immediately. |
| `restart`  | Reboot the device. Outcome event published before reboot when possible. |
| `identify` | Enter `IDENTIFY` for `identify_duration_ms` (default 2000), then resume prior state. |
| `ping`     | Outcome event only (`pong`). |

### 9.2 Project commands

| Command | Fields | Effect |
|---|---|---|
| `setBrightness` | `brightness` 0–15; optional `persist` bool (default `true`) | Apply immediately; persist unless `persist:false`. |
| `setTarget`     | `target` per §4.4 | Update target; emits `code_solved` immediately if the current code already matches. |
| `clearTarget`   | — | Equivalent to `setTarget` with `target: null`. |
| `getCode`       | — | Publish a `code_changed` event with the current code immediately. |
| `setBatteryThresholds` | `low_v`, `critical_v`, optional `hysteresis_v` | Apply + persist. Validates `critical_v < low_v`. |
| `on`            | — | Leave `OFF`; resume `ACTIVE`. No-op if already active. |
| `off`           | — | Enter `OFF`. No-op if already off. |

### 9.3 Validation

- Unknown commands → `command_failed` with warning code `unknown_command`.
- Out-of-range fields → `command_failed` with warning code
  `invalid_argument` and `data.field` naming the offending field.
- Malformed JSON → silently dropped at the MQTT layer; one
  `invalid_payload` warning per minute is published as a rate limit.

### 9.4 Outcome events

Every command produces, in order:

1. `command_received` — echo of the command name + `request_id`.
2. Exactly one of:
   - `command_success`,
   - `command_warning` (succeeded but with an advisory),
   - `command_failed`.

---

## 10. MQTT — state, events, warnings, announce

### 10.1 State payload

Published on:

- Boot (after MQTT connect).
- Every `heartbeat_interval_ms` (default 10 000 ms).
- On demand via `getState`.
- On any state transition (`OFF`/`ACTIVE`/`LOW_BATT`/etc.), brightness
  change, or target change.

```json
{
  "timestamp": "2026-05-07T16:42:00.123Z",
  "application": "px-enigma-esp8266",
  "instance": "site1-enigma1",
  "version": "0.1.0",
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
  "code": {
    "code": "12-34-56",
    "code_int": 123456,
    "code_bits": 87654,
    "target": "12-34-56",
    "solved": true
  },
  "display": { "brightness": 1, "blanked": false },
  "battery": { "voltage_v": 12.62, "status": "ok" }
}
```

### 10.2 Events

| Event name | When emitted | Notable `data` fields |
|---|---|---|
| `command_received` | every command | `command`, `request_id` |
| `command_success`  | command succeeded | `command`, `request_id` |
| `command_warning`  | command succeeded with advisory | `command`, `request_id`, `warning` code |
| `command_failed`   | command rejected | `command`, `request_id`, `warning` code, `data` |
| `code_changed`     | displayed code transitioned | `code`, `code_int`, `code_bits` |
| `code_solved`      | code matches target (transition only) | `code`, `target` |
| `code_unsolved`    | code no longer matches target | `code`, `target` |
| `battery_low`      | entering `LOW_BATT` | `voltage_v` |
| `battery_critical` | entering `CRIT_BATT` | `voltage_v` |
| `battery_normal`   | recovering to `ok` | `voltage_v` |
| `state_changed`    | display state machine transition | `from`, `to` |
| `pong`             | response to `ping` | `request_id` |

Event envelope:

```json
{
  "timestamp": "2026-05-07T16:42:00.456Z",
  "type": "code",
  "event": "code_changed",
  "message": "Displayed code is now 12-34-56",
  "data": { "code": "12-34-56", "code_int": 123456, "code_bits": 87654 }
}
```

`type` is one of `command`, `code`, `battery`, `state`, `system`.

### 10.3 Warnings

Published to `<base_topic>/warnings`. Each warning is an event-shaped
payload with `event` set to the warning code:

| Code | Cause |
|---|---|
| `unknown_command` | Command name not recognized |
| `invalid_argument` | Field present but out of range / wrong type |
| `invalid_code_format` | `setTarget` payload could not be parsed |
| `invalid_payload` | Malformed JSON on the commands topic |
| `config_invalid` | Persisted config rejected; defaults applied |
| `battery_low` | Mirror of the `battery_low` event |
| `battery_critical` | Mirror of the `battery_critical` event |
| `mqtt_publish_failed` | One or more publishes failed; rate-limited to 1/min |

Mirroring battery events into the warnings topic lets dashboards that
only subscribe to `+/warnings` show power health without subscribing to
the firehose.

### 10.4 Announce

Published once per MQTT connect to `<announce_topic>` (default
`paradox/props`):

```json
{
  "timestamp": "2026-05-07T16:42:00.000Z",
  "event": "online",
  "application": "px-enigma-esp8266",
  "instance": "site1-enigma1",
  "prop_name": "Front Hall Enigma",
  "version": "0.1.0",
  "ip": "192.168.1.42",
  "ap_ip": "192.168.4.1",
  "mdns": "front-hall-enigma",
  "mdns_fqdn": "front-hall-enigma.local",
  "mac": "AA:BB:CC:DD:EE:FF",
  "base_topic": "paradox/site1/enigma1",
  "commands_topic": "paradox/site1/enigma1/commands"
}
```

---

## 11. Web UI

A single-page UI served from LittleFS on port 80, reachable on both AP
and STA interfaces, no authentication on the LAN-side endpoints.

The form covers every persisted field:

- Identity: `device.prop_name`, `device.instance`.
- WiFi: primary + backup SSID/password, AP password.
- MQTT: host, port, username, password, base topic, announce topic,
  heartbeat interval.
- Puzzle: target code, brightness, identify duration.
- Battery: low / critical / hysteresis thresholds.
- Diagnostics: live state snapshot, last 32 log lines, raw 20-bit matrix.

API endpoints:

| Method | Path | Behavior |
|---|---|---|
| `GET`  | `/`                | Single-page UI |
| `GET`  | `/api/config`      | Returns current `config.json` |
| `POST` | `/api/config`      | Replaces `config.json`; live-applies what it can |
| `GET`  | `/api/state`       | Same payload as MQTT `state` |
| `GET`  | `/api/log`         | Tail of the in-RAM log ring buffer |
| `POST` | `/api/identify`    | Equivalent to MQTT `identify` |
| `POST` | `/api/restart`     | Equivalent to MQTT `restart` |
| `POST` | `/update`          | HTTP OTA upload (password-protected) |

---

## 12. OTA

- HTTP OTA via `ESP8266HTTPUpdateServer` mounted at `/update`. Password
  defaults to the AP password.
- ArduinoOTA always enabled; advertised as `<mdns>.local`.
- Successful upload reboots into the new firmware automatically.

---

## 13. Configuration schema

`data/config.json` (LittleFS). Defaults are baked into firmware; missing
fields fall back to defaults and emit a `config_invalid` warning if the
file fails to parse.

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
    "target": null,
    "identify_duration_ms": 2000
  },
  "display": {
    "brightness": 1
  },
  "battery": {
    "low_v": 12.35,
    "critical_v": 12.10,
    "hysteresis_v": 0.10,
    "sample_interval_ms": 10000
  },
  "scan": {
    "dwell_ms": 10,
    "debounce_samples": 4
  }
}
```

---

## 14. Open questions

Items intentionally left open for the implementation phase to confirm
empirically rather than guess at:

1. **HT16K33 digit-position layout.** The legacy code addresses specific
   digit positions on each display to render `XX-YY-ZZ`. The renderer
   should replicate the legacy mapping verbatim and add a unit test that
   pins it down (Phase 9).
2. **GPIO1/GPIO3 reuse for matrix lines.** Confirm during bring-up that
   the upload flow still works (the boot ROM uses these for `printf` at
   boot before the firmware reconfigures them). Document any required
   recovery steps in the user guide.
3. **Battery calibration drift.** The legacy linear fit may need
   re-calibration on each unit. The Web UI should expose a "raw_a0"
   diagnostic to make field calibration easy.
