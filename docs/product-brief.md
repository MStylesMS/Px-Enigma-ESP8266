# px-enigma-esp8266 — Product Brief

**Status:** Draft for review
**Version:** 0.1.0-draft

## What it is

The Enigma is a tactile **code-entry puzzle** prop for escape rooms and
themed installations. A 4 × 5 grid of toggle switches feeds a six-digit
code onto two large 7-segment displays, formatted as `XX-XX-XX`. Players
discover, by inference and experimentation, which combination of switch
positions produces the code the game is looking for.

When the displayed code matches a target value the prop signals success
over MQTT, allowing a game-orchestration system to react without polling
the device.

## Who it's for

- **Escape-room and haunted-house operators** who need a reliable,
  repeatable, mechanically interesting puzzle that integrates with their
  existing MQTT-based show control.
- **Paradox Productions** rooms that already deploy other props
  speaking the same MQTT contract (clocks, lighting controllers,
  player terminals).

## Where it fits in the Paradox family

Inputs flow from the Enigma to the orchestration engine the same way
they do from any other Paradox sensor / input prop:

```
px-enigma  ──MQTT──▶  PxO (orchestrator)  ──▶  PFx / PxB / other props
```

The Enigma never originates show-control commands. It only reports
**what code is currently displayed** and, optionally, **whether that
code matches the configured target**. All higher-level game logic
(hints, fail states, reset choreography) lives in the orchestrator.

## What it isn't

- It is **not** an HMI for arbitrary numeric entry — the input is
  intentionally indirect and tactile.
- It does **not** run game logic, manage hints, or coordinate other props.
- It does **not** sleep, suspend, or manage power saving. The network
  surface is always available even when the display is blanked.
- It does **not** ship a battery (despite supporting a battery monitor
  on the analog rail) — it is designed for permanent installation on
  a 12 V supply.

## Headline capabilities

- **Always-on network surface.** WiFi (dual-SSID failover) and MQTT stay
  up regardless of display state. The local Web UI is reachable on the
  device's own SoftAP at `192.168.4.1` even with no infrastructure WiFi.
- **Self-contained configuration.** All settings (WiFi, MQTT, display
  brightness, target code, battery thresholds, identity) live in a single
  JSON file in LittleFS, editable through a single-page Web UI or the
  HTTP API.
- **Standards-compliant MQTT.** Implements the Paradox MQTT contract:
  derived `<base>/{commands,state,events,warnings}` topics plus a separate
  announce topic, JSON envelopes with `request_id` echo, and standard
  `command_received` / `command_success` outcome events.
- **OTA updates.** HTTP and ArduinoOTA both supported.
- **Diagnostic surfaces.** State snapshot includes WiFi RSSI, free heap,
  uptime, battery voltage, raw 20-bit matrix state, and the formatted
  six-digit code.

## Non-goals (for v1)

- No on-device hint logic.
- No multi-target / sequenced puzzles. A single target code is
  sufficient; sequenced puzzles are composed at the orchestrator layer.
- No accessibility audio or haptic feedback. The puzzle is purely
  visual and tactile by design.
- No support for switch matrices other than 4 × 5.
