# px-enigma-esp8266 — AI Instructions

You are working on **px-enigma-esp8266**, an ESP8266 firmware for an escape-room
"enigma" code-entry prop. Players manipulate a 4 × 5 matrix of toggle switches,
producing a 20-bit value that is rendered as a six-digit code (`XX-XX-XX`) on a
pair of HT16K33-driven 4-digit 7-segment displays. The device participates in
the Paradox prop ecosystem over MQTT.

**Repository:** `git@github.com:MStylesMS/Px-Enigma-ESP8266.git`

## Tech stack

- **MCU:** LoLin NodeMCU ESP-12E/12F (ESP8266MOD)
- **Toolchain:** PlatformIO + Arduino (`espressif8266` / `nodemcuv2` board)
- **Filesystem:** LittleFS (config + Web UI assets in `data/`)
- **Displays:** 2 × Adafruit-style HT16K33 4-digit 7-segment, I2C `0x70` and `0x71`
- **Inputs:** 4 × 5 switch matrix (4 driven outputs, 5 sensed inputs with `INPUT_PULLUP`)
- **Power monitor:** 12 V supply rail sensed on `A0` via on-board divider
- **Comms:** WiFi (dual SSID failover, AP+STA always-on) + MQTT (Paradox protocol)

## Architecture summary

Single non-blocking cooperative loop. Concerns are split into small modules with
explicit ownership; **no module calls `delay()` or busy-waits**.

| Module | Responsibility |
|---|---|
| `clock_engine` *(no — see code_engine)* | n/a |
| `switch_matrix` | Non-blocking matrix scan, debounce, exposes 20-bit state |
| `code_engine` | Owns current code, target matching, emits `code_changed` / `code_solved` |
| `display_renderer` | Formats 6-digit code as `XX-XX-XX` across the two HT16K33 displays |
| `battery_monitor` | A0 sampling, hysteresis, `ok` / `low` / `critical` state |
| `hardware_io` | I2C bus init, low-level display + GPIO drive |
| `wifi_mgr` | Dual-STA + always-on AP, non-blocking reconnect |
| `mqtt_mgr` | `PubSubClient`, topic resolution, command dispatch, publishes |
| `web_ui` | `ESP8266WebServer`, `/api/config`, `/api/state`, settings page |
| `ota_mgr` | `ESP8266HTTPUpdateServer` + ArduinoOTA |
| `commands` | Command handler dispatch (one place to add new commands) |
| `config` | LittleFS JSON config: load / save / validate / defaults |
| `state` | Shared status snapshot consumed by MQTT, Web UI, and logging |
| `log` | `pxlog::info/warn/err` macros — Serial + ring buffer |

The **switch matrix is the source of truth for the displayed code**. The display
renderer reads from `code_engine`; it never owns puzzle state.

## Critical conventions

- **Spec-first.** Read `docs/functional-spec.md` before changing behavior. If a
  change conflicts with the spec, propose a doc update first.
- **Standalone-first.** WiFi and MQTT are best-effort. The puzzle must be
  fully playable from the moment the display lights, regardless of network state.
- **Boot priority.** Display + scanner come up before WiFi / MQTT to keep
  cold boot under ~1.5 s. Players will normally power-cycle the prop at the
  start of each game.
- **MQTT topic structure is fixed:** `<base_topic>/{commands,state,events,warnings,config}`,
  plus a separate `<announce_topic>` (default `paradox/props`) that is **not**
  derived from `base_topic`. `<base_topic>/config` is **retained** and applied
  silently on every (re)connect.
- **Command envelope:** `{ "command": "lowerCamel", "request_id"?: "...", ...params }`.
  `request_id` is echoed verbatim in the corresponding `command_*` outcome event.
- **Outcome events:** every command emits `command_received`, then exactly one of
  `command_success`, `command_failed`, or `command_warning`.
- **JSON field naming:** snake_case for emitted fields (`remaining_s`, `code_bits`),
  lowerCamelCase for command keys (`setBrightness`, `setTarget`).
- **Timestamps:** ISO 8601 with millisecond precision.
- **QoS / retain:** QoS 1 everywhere; retain off everywhere except `<base_topic>/config`.
- **Always-on AP+STA.** `off` affects only the displays — the network
  surface (WiFi, MQTT, Web UI, OTA) stays up.
- **Deep sleep policy.** Light sleep is *not* used. Deep sleep is only
  triggered by the inactivity timer on battery profiles, and waking
  requires a power cycle by design.
- **Persisted config** lives in `data/config.json` (LittleFS). It is
  `.gitignored`. The committed `config.json.example` at the repo root is
  the schema reference. If `data/config.json` is missing the firmware
  generates defaults at boot — **do not flash placeholder credentials**.
- **Config visibility.** Calibration constants, custom-curve points,
  scan timing (`scan.poll_interval_ms`, `scan.debounce_samples`), and
  RSSI thresholds live in `data/config.json` only and are **not** editable
  in the Web UI. The UI shows their current values read-only.
- **Logging:** use `pxlog::info(tag, fmt, ...)` etc. — never `Serial.print*`
  directly outside `log.cpp`. Serial output is best-effort; GPIO1/3 are reused
  by the matrix scanner (see hardware spec).
- **MQTT publishing:** always go through `mqtt_mgr` helpers; never call the raw
  `PubSubClient` from other modules.

## Required commands

Workspace-standard (must always be implemented and behave identically across
Paradox props):

- `getState` — publish a fresh `state` snapshot on demand.
- `restart` — reboot the device.
- `identify` — flash all `8`s on the displays for ~2 s, then resume.
- `ping` — outcome event only (`pong`).

Project-specific (see `docs/functional-spec.md` for full schemas):

- `setBrightness` — display brightness 0–15 (HT16K33 native range).
- `setTarget` / `clearTarget` — set or clear the target code.
- `setMode` — switch between `live` and `latching` puzzle modes.
- `reset` — clear `LATCHED` state and return to `ACTIVE`.
- `getCode` — publish the current code immediately.
- `setBatteryProfile` — select a built-in profile or supply a custom curve.
- `setSignalIndicator` — toggle / retune the decimal-point WiFi+MQTT indicator.
- `on` / `off` — enable / blank the display (network surface stays up).

## Hardware constraints (do not change without explicit approval)

The chip and wiring match the legacy reference implementation. **Pin assignments
are load-bearing** — the hardware exists.

- I2C: `SDA = GPIO0` (D3), `SCL = GPIO2` (D4), 4.7 kΩ pull-ups.
- Matrix outputs (driven LOW one at a time): `GPIO15, GPIO1, GPIO5, GPIO16`.
- Matrix inputs (`INPUT_PULLUP`): `GPIO12, GPIO3, GPIO14, GPIO4, GPIO13`.
- Battery sense: `A0` with calibration `V = 0.0531 × raw + 0.1978`.
- Display I2C addresses: `0x70` (low pair) and `0x71` (high pair).

GPIO0, GPIO2, GPIO15 carry boot-strap responsibilities — the schematic accounts
for this; firmware must not reconfigure them in conflicting ways during boot.

## Testing

- `pio test -e native` — host-side Unity smoke tests for pure logic
  (debouncer, code aggregation, target matcher, time/format parsers, config
  round-trip). Designed to run without hardware.
- Integration / soak testing is performed on a real device, scripted from the
  `tools/` directory where applicable.

## Repository conventions

- **Documentation-first.** Spec or design changes precede code changes.
- **Commit message prefixes:** `Docs:`, `Implement:`, `Fix:`, `Test:`, `Refactor:`, `Chore:`.
- **Branching:** `main` is the integration branch. Topic branches are short-lived.
- **Edits are minimal.** Don't reformat unrelated code, don't add commentary
  unrelated to the change, don't widen scope without asking.

## Key references

| Document | Purpose |
|---|---|
| [docs/product-brief.md](docs/product-brief.md) | What the device is and who it serves |
| [docs/functional-spec.md](docs/functional-spec.md) | Definitive behavior + MQTT API contract |
| [docs/hardware-spec.md](docs/hardware-spec.md) | Electrical and component reference |
| [docs/pin-mapping.md](docs/pin-mapping.md) | GPIO assignments and I2C addresses |
| [docs/implementation-plan.md](docs/implementation-plan.md) | Ordered, testable phases |
| [docs/user-guide.md](docs/user-guide.md) | Operator-facing setup and usage |
| [README.md](README.md) | Project overview and quick start |
