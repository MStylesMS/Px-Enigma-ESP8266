# px-enigma-esp8266

Firmware for the **Enigma** code-entry prop in the Paradox Productions
escape-room ecosystem.

- **MCU:** LoLin NodeMCU ESP-12E/12F (ESP8266MOD)
- **Toolchain:** PlatformIO + Arduino (`espressif8266` core)
- **Inputs:** 4 × 5 toggle-switch matrix (20 bits)
- **Displays:** 2 × HT16K33 4-digit 7-segment displays (I2C)
- **Power:** 12 V wall supply *or* battery (4 built-in profiles +
  custom curve, with inactivity deep-sleep on battery)
- **Comms:** WiFi (dual-SSID failover, AP+STA always-on) + MQTT (Paradox
  protocol), retained `<base>/config` topic for fleet overrides;
  fully playable standalone with no network

Players manipulate the switch matrix to produce a six-digit code displayed
as `XX-YY-ZZ`. The current code is published over MQTT, and an optional
target code drives one of two puzzle modes: **live** (free play, with
`code_solved` / `code_unsolved` boundary events) or **latching** (single
`solve` event then a blinking frozen code until `reset`). The orchestrator
uses these events to advance the game without polling.

See [`docs/`](docs/) for the full specification set, starting with
[`docs/functional-spec.md`](docs/functional-spec.md).

## Architecture

The firmware is composed of small, independently-testable modules running on a
single non-blocking cooperative loop. None of the modules use `delay()` or
busy-wait on hardware.

- `src/switch_matrix.*` — scans the 4 × 5 matrix, debounces, exposes 20-bit state.
- `src/code_engine.*` — owns the displayed code, runs `live` / `latching` puzzle modes, emits events.
- `src/display_renderer.*` — formats `XX-YY-ZZ` across the two HT16K33 displays; drives the optional WiFi/MQTT decimal-point indicator.
- `src/battery_monitor.*` — A0 sampling with profile-based discharge curves (`ok` / `low` / `critical` / `external`).
- `src/sleep_manager.*` — inactivity-driven deep sleep on battery profiles.
- `src/hardware_io.*` — I2C bus and low-level GPIO/display access.
- `src/wifi_mgr.*` — AP+STA setup with non-blocking reconnect.
- `src/mqtt_mgr.*` — MQTT client, command dispatch, state/event publishing.
- `src/web_ui.*` — local configuration page + JSON API.
- `src/ota_mgr.*` — HTTP and ArduinoOTA update flows.
- `src/commands.*` — command handler registry.
- `src/config.*`, `src/state.*`, `src/log.*` — config persistence, shared status, logging.

`src/main.cpp` wires the modules together and advances them cooperatively.

## Repository layout

- `src/` — firmware source
- `lib/` — in-tree libraries (when needed)
- `data/` — LittleFS payload (Web UI assets, runtime config)
- `docs/` — specification and implementation documentation
- `test/` — host-side smoke tests (`pio test -e native`)
- `tools/` — automation helpers

## Quick start

```sh
# 1. Install PlatformIO (one-time): https://platformio.org/install
# 2. Create your local config from the example (this file is gitignored)
cp config.json.example data/config.json
$EDITOR data/config.json   # fill in WiFi SSID/password and MQTT broker

# 3. Build and flash
pio run -t uploadfs        # flash the LittleFS image (config + Web UI assets)
pio run -t upload          # flash the firmware
pio device monitor         # watch the boot log
```

After first boot the device:

- Brings up an always-on AP named after `device.prop_name`
  (default `px-enigma-XXXX` derived from the MAC) at `192.168.4.1`.
  Default AP password: `MCEscher` — change it in the Web UI.
- Joins the configured primary/backup SSIDs.
- Serves the Web UI on `http://192.168.4.1/` and on its LAN IP.
- Connects to MQTT (if `mqtt.host` is set) and announces on `paradox/props`.

## Secrets / credentials

The committed config template is [`config.json.example`](config.json.example).
Real credentials go in `data/config.json`, which is **gitignored**. The
contents of `data/` are flashed verbatim to LittleFS by `pio run -t uploadfs`.

To rotate or update credentials over the network:

```sh
curl http://<device-ip>/api/config -o config.json
$EDITOR config.json
curl -H 'Content-Type: application/json' \
     --data-binary @config.json http://<device-ip>/api/config
```

## Development

```sh
pio run                   # build firmware
pio test -e native        # run host-side smoke tests
pio run -t uploadfs       # reflash LittleFS assets
pio device monitor        # serial console
```

## Documentation

- [Product brief](docs/product-brief.md)
- [Functional spec & MQTT API](docs/functional-spec.md)
- [Hardware spec](docs/hardware-spec.md)
- [Pin mapping](docs/pin-mapping.md)
- [Implementation plan](docs/implementation-plan.md)
- [User guide](docs/user-guide.md)
