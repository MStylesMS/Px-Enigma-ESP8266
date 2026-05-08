# px-enigma-esp8266 — Implementation Plan

**Status:** Draft for review
**Version:** 0.2.0-draft

This plan turns the functional specification into ordered, independently
testable phases. Each phase produces a working, flashable firmware
artifact and a written acceptance check. The ordering writes as much code
as possible **without hardware**, leaving I2C / display / matrix bring-up
to the back of the plan.

Each phase entry includes:

- **Scope:** what is in (and explicitly out of) the phase.
- **Acceptance:** how to know it's done.
- **Suggested model:** which AI assistant to use, with rationale.

The model recommendations assume access to a tier of assistants similar
to today's market: an expensive premium reasoner (e.g., Claude Opus / GPT‑5
"high"), a strong mid-tier coder (e.g., Claude Sonnet, GPT‑4.1), and an
inexpensive fast tier (e.g., Claude Haiku, GPT‑4o-mini). The names below
are illustrative; map them to whichever models are current at the time.

---

## Phase 0 — Project skeleton

**Scope.**

- PlatformIO `nodemcuv2` build compiles with the libraries pinned in
  `platformio.ini`.
- `src/main.cpp` boot banner + cooperative loop scaffold (empty modules
  ok).
- `src/config.h` with the pin / address constants from
  [pin-mapping.md](pin-mapping.md).
- `src/log.h` / `src/log.cpp` with `pxlog::info/warn/err` and a small
  in-RAM ring buffer.
- `src/secrets.h.template` (committed) and `src/secrets.h` (gitignored).
- `test/stubs/Arduino.h` and a stub `test_native_smoke.cpp` so
  `pio test -e native` is wired even without tests.

**Out of scope.** Any hardware access, MQTT, WiFi, or Web UI logic.

**Acceptance.** `pio run` succeeds; `pio test -e native` runs (zero
tests is fine). Boot banner visible on serial when flashed.

**Suggested model.** Mid-tier (Sonnet-class). Mostly mechanical; the
premium tier is wasted here.

---

## Phase 1 — Configuration & persistence

**Scope.**

- LittleFS mount at boot.
- `Config` struct + load / save / validate from `/config.json`.
- Schema matches functional-spec §14.2 verbatim, including
  `puzzle.mode`, `puzzle.start_state`, `signal_indicator`, the new
  `battery` block (profile, points, percent thresholds, calibration,
  `inactivity_minutes`), and the unchanged `scan` block.
- Bake defaults; on missing or corrupt file, fall back to defaults and
  emit `config_invalid` to the log (MQTT not yet present).
- **Ship-nothing-if-missing rule.** `data/config.json` is `.gitignored`.
  When it is absent locally, `pio run -t uploadfs` flashes no config
  and the firmware regenerates defaults at first boot.
- `config.json.example` checked in at repo root.

**Out of scope.** WiFi, MQTT, Web UI.

**Acceptance.** Native test round-trips a representative config through
serialize → parse → serialize, byte-equivalent. On hardware, edits via
direct LittleFS upload survive reboot.

**Suggested model.** Mid-tier. Pure JSON / structs / file I/O.

---

## Phase 2 — WiFi (dual STA + always-on AP)

**Scope.**

- `WIFI_AP_STA` mode at boot.
- `ESP8266WiFiMulti` registers primary + backup credentials.
- AP SSID derived from `device.prop_name` (normalized), default password
  from config.
- Non-blocking reconnect with backoff.
- Connection status reflected in the shared `state` snapshot.

**Out of scope.** Web UI, MQTT.

**Acceptance.** With one valid SSID and one bogus, STA connects to the
valid one within the configured budget. AP is reachable from a phone at
`192.168.4.1` regardless of STA state.

**Suggested model.** Mid-tier. Standard ESP8266 idioms.

---

## Phase 3 — Web UI & HTTP API

**Scope.**

- `ESP8266WebServer` on port 80, served on AP and STA.
- Single-page UI (`data/index.html`, `data/style.css`, `data/app.js`)
  covering every persisted field (functional-spec §11).
- Endpoints: `GET /`, `GET/POST /api/config`, `GET /api/state`,
  `GET /api/log`, `POST /api/restart`, `POST /api/identify`.
- Save → write `/config.json` → live-apply where possible, otherwise
  prompt reboot.

**Out of scope.** OTA, MQTT.

**Acceptance.** From a browser on AP or STA, every field round-trips
without reboot; restart and identify buttons work.

**Suggested model.** Mid-tier for the C++ side; **fast / cheap tier**
for HTML/CSS/JS authoring. Use the premium tier only if you need a
unified design pass for accessibility / mobile layout.

---

## Phase 4 — OTA updates

**Scope.**

- `ESP8266HTTPUpdateServer` mounted at `/update`, password-protected.
- ArduinoOTA always enabled and advertised as `<mdns>.local`.
- Version string surfaced in `/api/state`.

**Out of scope.** MQTT events on update completion (handled in Phase 5).

**Acceptance.** `pio run -t upload --upload-port <mdns>.local` succeeds;
HTTP `/update` upload of a new firmware image succeeds and reboots into
the new image.

**Suggested model.** Mid-tier or fast tier. Boilerplate.

---

## Phase 5 — MQTT plumbing & standard commands

**Scope.**

- `PubSubClient` with reconnect-with-backoff.
- Topic resolution from configured base + announce topic.
- JSON publish helpers for `state`, `events`, `warnings`, `announce`.
- Workspace commands: `getState`, `restart`, `identify`, `ping`.
- Periodic `state` publish at `heartbeat_interval_ms`.
- `announce` on each (re)connect.
- **Retained config override.** Subscribe to `<base_topic>/config` on
  every (re)connect; merge any retained payload over the in-memory
  config (top-level key replacement, no deep merge), apply silently,
  and emit one `config_override_applied` event listing the merged
  top-level keys. Empty retained payload clears the override.
- Outcome event contract (`command_received`, `command_success`,
  `command_failed`, `command_warning`) + `request_id` echo.

**Out of scope.** Project-specific commands.

**Acceptance.** `mosquitto_pub` / `mosquitto_sub` round-trip for all
four standard commands; outcome events match the contract; `state`
publishes on schedule; `announce` fires once per reconnect.

**Suggested model.** **Premium tier.** Getting the envelope, error
paths, reconnect logic, and `request_id` propagation right pays
dividends across every later phase. This is also where the contract
risk lives — a missed field here ripples everywhere.

---

## Phase 6 — Switch matrix scanner (host-tested)

**Scope.**

- Pure-logic `switch_matrix` module: state machine + debounce.
- Hardware access via a thin "ScanIO" interface so the host build can
  inject canned column / row sequences.
- Native unit tests:
  - Each cell's `on` and `off` transitions debounce correctly.
  - Spurious single-sample noise does not flip a cell.
  - Bit assignments match the pin map.

**Out of scope.** Driving real GPIOs.

**Acceptance.** `pio test -e native` exercises every cell; tests pass.
On hardware (Phase 9) the same module is dropped in unchanged.

**Suggested model.** **Premium tier.** Debounce + scanning state
machine is easy to get subtly wrong. Worth the spend.

---

## Phase 7 — Code engine & target matching (host-tested)

**Scope.**

- `code_engine` module: takes a 20-bit input, emits `code_changed` /
  `code_solved` / `code_unsolved` callbacks via dependency injection.
- **Modes.** Implement both `live` and `latching` modes per
  functional-spec §5.2. Latching engages a single-shot `solve` event,
  freezes display rendering on the matched code, and is cleared by the
  `reset` command (emitting `unlatch`) or a power cycle.
- `setTarget` parser supporting integer / digit-string / hyphenated
  forms (functional-spec §5.4).
- Display string formatter (`123456` → `"12-34-56"`; `123` →
  `"00-01-23"`).
- Native unit tests for every input form + transition rule, including
  latch → reset → active and mode-switch-clears-latch.

**Out of scope.** Driving displays, MQTT publishing.

**Acceptance.** Unit tests cover: format edge cases, target parser
edge cases, single-shot `code_solved` semantics, target clear, target
update mid-match, and the latching state machine (engage / reset /
mode-switch-clear).

**Suggested model.** Mid-tier with a brief premium-tier review pass on
the parser to catch input edge cases.

---

## Phase 8 — MQTT project commands & event integration

**Scope.**

- Wire `code_engine` and `battery_monitor` (still hardware-stub) into
  `mqtt_mgr` so events surface on `/events` and `/warnings`.
- Project commands: `setBrightness`, `setTarget`, `clearTarget`,
  `setMode`, `reset`, `getCode`, `setBatteryProfile`,
  `setSignalIndicator`, `on`, `off`.
- State payload reflects everything in functional-spec §12.1
  (including `puzzle.mode`, `puzzle.latched`, `battery.profile/percent`,
  `sleep.idle_minutes`).
- Web UI form save now also publishes a `state` snapshot afterwards.

**Out of scope.** Real hardware in the path.

**Acceptance.** With a stub matrix that lets a host script inject a
20-bit value, every event in functional-spec §12.2 can be triggered
from a script and observed via `mosquitto_sub`.

**Suggested model.** Premium tier. Cross-module integration; this is
the phase where the spec and code converge.

---

## Phase 9 — Hardware bring-up & fast cold boot (first phase requiring hardware)

**Scope.**

- **Boot ordering for fast cold start.** Bring up display + matrix
  scanner before WiFi / MQTT (functional-spec §8.3, target ≤ 1.5 s
  to first lit code). Verify with serial-timestamped log lines.
- I2C bus init on `GPIO0 / GPIO2`.
- HT16K33 driver wiring (`Adafruit_LEDBackpack` or in-tree wrapper).
- Display renderer mapping the legacy `XX-YY-ZZ` digit layout, with a
  small visual test mode that walks through `00-00-00` → `99-99-99`.
- **Optional decimal-point signal indicator** per functional-spec
  §8.4: STA RSSI bar on dots 0–6 (5 dB steps), MQTT-connected on dot 7,
  suppressed during `IDENTIFY` / `LOW_BATT` banner / `CRIT_BATT`.
- Real GPIO drivers behind the `ScanIO` interface from Phase 6.
- A0 sampling + two-point calibration; expose `raw_a0` in `/api/state`
  for field calibration.
- Boot-strap sanity check: log a warning if any of GPIO0/2/15 is in an
  unexpected state at the end of `setup()`.

**Out of scope.** Battery profile curves (Phase 9b), sleep manager
(Phase 9c), soak / OTA validation.

**Acceptance.**

- Cold boot to first lit code under 1.5 s on a representative unit.
- Powering the prop produces the same six-digit display behavior as the
  legacy unit for every switch position checked in a sample of 8 cells.
- Identify pattern shows `8888 8888` then resumes.
- Reading `/api/state` over WiFi yields a sensible `battery.voltage_v`.
- With `signal_indicator.enabled = true`, the decimal points reflect
  STA RSSI and MQTT connectivity changes within one heartbeat.

**Suggested model.** Mid-tier for the wiring; **premium tier for
debugging** if the digit-position mapping or matrix wiring shows
discrepancies vs. the legacy unit.

---

## Phase 9b — Battery profiles & monitoring

**Scope.**

- `battery_monitor` module owning the discharge-curve evaluator.
- Built-in profiles: `external`, `unknown`, `12v-lead-acid`,
  `12v-LiFePO4`, `6v-lead-acid`, `6v-LiFePO4` per functional-spec §6.3.
- `custom` profile parser accepting both CSV (`"v:p,v:p,..."`) and
  object-array forms (`[{v,p}, ...]`), with validation rules from §6.4.
- Percent-based thresholds (`low_percent`, `cutoff_percent`,
  `hysteresis_pct`) and the `LOW_BATT` / `CRIT_BATT` transitions.
- `setBatteryProfile` command with both forms accepted; persisted to
  `data/config.json`.
- Native unit tests covering: each built-in profile at boundary
  voltages, custom-curve CSV + array parsing, invalid curves →
  `config_invalid` and fallback to `unknown`.

**Out of scope.** Sleep manager (Phase 9c).

**Acceptance.** Native suite passes. On hardware, swapping profiles
through the Web UI updates `battery.percent` in `/api/state` without a
reboot. A deliberately-bad custom curve falls back to `unknown` and
emits `config_invalid`.

**Suggested model.** Mid-tier with brief premium review of the
curve-evaluator boundary cases.

---

## Phase 9c — Sleep manager

**Scope.**

- `sleep_manager` runs only when the configured profile enables it
  (anything other than `external` / `unknown`).
- Inactivity timer: any confirmed switch-state change resets it; MQTT
  / Web UI activity does **not**. Default `inactivity_minutes = 60`,
  configurable in `data/config.json` and via the Web UI; `0` disables.
- On expiry: publish `going_to_sleep`, flush the log buffer, call
  `ESP.deepSleep(0)`. Wake requires a power cycle by design.
- Shared `state` snapshot exposes `sleep.inactivity_minutes` and
  `sleep.idle_minutes`.

**Acceptance.** With `inactivity_minutes = 1` for testing, a unit on a
battery profile sleeps after one minute of no switch activity, emits
the `going_to_sleep` event before sleeping, and stays asleep until
power-cycled. With profile = `external`, no sleep occurs.

**Suggested model.** Mid-tier; this is a single small state machine.

---

## Phase 10 — Soak, polish, and field readiness

**Scope.**

- Continuous-run soak test with a scripted MQTT load (`tools/`).
  Configurable duration (in minutes); **default 30 minutes** for the
  routine pre-release run, with the option to extend to multi-hour or
  multi-day windows for release candidates.
- Watchdog hygiene: `yield()` in any non-trivial loop; verify no
  silent reboots.
- OTA validation end-to-end (HTTP and ArduinoOTA), including a
  "rollback by reflash" rehearsal.
- Standalone-mode validation: pull the WiFi credentials and confirm
  the puzzle still plays normally; reconnect and confirm event order
  is sane.
- Documentation pass: README, user guide, and any changes accumulated
  during implementation merged back into the spec.

**Acceptance.** Default 30-minute soak completes with no resets and
a stable free-heap floor; an extended run (≥ 8 h, configurable) is
performed before any tagged release. MQTT reconnect count consistent
with broker / WiFi incident count, and a known-good firmware image
tagged in git.

**Suggested model.** **Premium tier** for soak-data analysis and any
diagnosed regressions; mid-tier for the docs sweep.

---

## Cross-cutting practices

- **Documentation-first.** Spec changes precede code changes. If a
  phase reveals a spec gap, update the spec in the same PR.
- **Native tests first.** Anything that can be host-tested should be.
  Hardware time is precious; bugs caught on the host save hours.
- **No `delay()`** outside of the explicit "I just rebooted, give the
  serial a moment" startup gate. The cooperative loop is sacred.
- **One commit per acceptance criterion** when practical, with the
  `Implement:` / `Test:` / `Docs:` prefix appropriate to the change.
- **Commit message hygiene.** Reference the phase by number where it
  helps reviewers (`Implement: Phase 5 — MQTT plumbing`).

---

## Suggested model usage at a glance

| Activity | Recommended tier | Why |
|---|---|---|
| Spec / architecture revisions | Premium | High leverage; a wrong choice rebounds for weeks. |
| Cross-module integration (Phases 5, 8) | Premium | Contract work; small omissions cost a lot downstream. |
| Pure-logic modules + unit tests (Phases 6, 7) | Mid-tier with brief premium review | Mid-tier is fluent; premium catches edge cases on parsers and state machines. |
| Hardware bring-up & debugging (Phase 9) | Mid-tier; escalate to Premium on stuck issues | Most bring-up is mechanical; reasoning helps when symptoms are weird. |
| HTML / CSS / JS for the Web UI | Fast / cheap tier | High volume, low risk. |
| Boilerplate (config, logging, OTA wiring) | Fast / cheap tier | Cheap is plenty. |
| Documentation passes | Mid-tier | Quality matters; cost doesn't. |
| Soak-log triage | Premium | Reasoning over noisy data. |
