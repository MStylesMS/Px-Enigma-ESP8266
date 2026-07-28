# px-enigma-esp8266 — API Reference

All interfaces are available once the device is on your network (STA mode) or via its own access point (AP mode). The device exposes three integration surfaces: an **HTTP REST API**, a **Server-Sent Events stream**, and **MQTT topics**.

---

## 1. Addressing

| Mode | Address |
|------|---------|
| STA IP | shown by `GET /api/state` → `wifi.sta.ip` |
| AP IP | `192.168.4.1` (default) |
| mDNS | `<normalized-prop_name>.local` (lowercase; spaces become hyphens; other non-alphanumeric characters are removed) |

Replace `<host>` in every example below with the device IP or mDNS name.

---

## 2. HTTP REST API

All endpoints run on **port 80**. Requests and responses use `application/json` unless otherwise stated. There is no authentication on the REST API (the AP password applies only to OTA uploads and ArduinoOTA).

### 2.1 Static assets

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/` | Web UI (index.html) |
| `GET` | `/style.css` | Stylesheet |
| `GET` | `/app.js` | Frontend script |
| `GET` | `/<path>` | Any file present on LittleFS that has no explicit route (e.g. `/switch_layout.json`, `/logo.png`) |

Warning: the generic static-file route also exposes `/config.json` if it is
present. It contains plaintext credentials; retrieve it only from a trusted
network.

### 2.2 Config

#### `GET /api/config`

Returns the current runtime configuration as JSON. Secrets (Wi-Fi and MQTT passwords) are replaced with dot-masked placeholders by default.

Query parameters:

| Parameter | Value | Effect |
|-----------|-------|--------|
| `reveal` | `1` | Return plaintext passwords (use only on a trusted network) |

```bash
curl http://<host>/api/config
curl http://<host>/api/config?reveal=1
```

Example response (abbreviated):
```json
{
  "device": { "instance": "enigma-01", "prop_name": "Enigma Machine" },
  "wifi": { "primary": { "ssid": "MyNetwork", "password": "........" } },
  "mqtt": { "host": "192.168.1.10", "port": 1883, "base_topic": "paradox/enigma1" },
  "puzzle": { "mode": "live", "has_target": false, "target": "" },
  "display": { "brightness": 1 }
}
```

#### `POST /api/config`

Overlay and persist a partial or full config update. Only the fields present in the request body are changed. Blank/whitespace password values are treated as "leave unchanged".

Returns `{ "ok": true, "reboot_required": <bool> }`. A reboot is scheduled automatically when a Wi-Fi credential, AP password, MQTT host, or MQTT port changes.

```bash
curl -X POST http://<host>/api/config \
  -H 'Content-Type: application/json' \
  -d '{"display": {"brightness": 8}}'
```

```bash
# Change MQTT broker (triggers automatic reboot)
curl -X POST http://<host>/api/config \
  -H 'Content-Type: application/json' \
  -d '{"mqtt": {"host": "10.0.0.5", "port": 1883}}'
```

#### `POST /api/config/reset`

Factory-reset: wipes `/config.json` from LittleFS and reboots into first-run defaults.

```bash
curl -X POST http://<host>/api/config/reset
```

Response: `{ "ok": true, "reboot_required": true }`

### 2.3 State

#### `GET /api/state`

Returns a live device snapshot (same schema as the MQTT `state` topic).
`ts` is the device's monotonic milliseconds since boot.

```bash
curl http://<host>/api/state
```

Example response:
```json
{
  "ts": 342000,
  "application": "px-enigma-esp8266",
  "instance": "enigma-01",
  "prop_name": "Enigma Machine",
  "version": "1.0.2",
  "status": "active",
  "uptime_s": 342,
  "health": { "free_heap_bytes": 28400, "min_free_heap_bytes": 27000 },
  "wifi": {
    "sta": { "ssid": "MyNetwork", "rssi": -58, "ip": "192.168.1.42", "connected": true },
    "ap":  { "ssid": "Px-Enigma-AB12", "ip": "192.168.4.1", "clients": 0 }
  },
  "mqtt": { "connected": true, "broker": "192.168.1.10:1883" },
  "puzzle": { "mode": "live", "latched": false },
  "code": {
    "code": "12-34-56",
    "code_int": 123456,
    "grid": ["10101", "01000", "00110", "10010"],
    "target": null,
    "target_grid": null,
    "solved": false
  },
  "display": { "brightness": 1, "blanked": false, "signal_indicator": true },
  "battery": {
    "profile": "external",
    "voltage_v": null,
    "raw_a0": 0,
    "percent": null,
    "status": "unknown"
  },
  "sleep": { "inactivity_minutes": 0, "idle_minutes": 0 }
}
```

### 2.4 Server-Sent Events

#### `GET /api/events`

Opens a persistent SSE stream. The device sends a `state` event immediately on connection, then pushes a `code_changed` event whenever the switch matrix reading changes, and a heartbeat `state` event every 2 seconds.

Only one SSE client is held at a time; a new connection displaces the previous one.

```bash
curl -N http://<host>/api/events
```

Event types:

| Event name | Trigger |
|------------|---------|
| `state` | Connection open; every 2 s heartbeat |
| `code_changed` | Switch matrix reading changed |

Each event payload is the same JSON object as `GET /api/state`.

### 2.5 Device actions

#### `POST /api/identify`

Triggers the identify animation (2 s by default, configurable via `puzzle.identify_duration_ms`).

```bash
curl -X POST http://<host>/api/identify
```

#### `POST /api/reset`

Clears the puzzle latch (Latching mode only). Does not reboot.

```bash
curl -X POST http://<host>/api/reset
```

#### `POST /api/restart`

Schedules a device reboot (~500 ms delay). Returns `{ "ok": true }` before rebooting.

```bash
curl -X POST http://<host>/api/restart
```

#### `POST /api/sleep`

Shows the sleep indicator (two middle code dashes only), then enters deep sleep after a short delay. Returns `{ "ok": true }` before sleeping. Wake requires a power cycle.

```bash
curl -X POST http://<host>/api/sleep
```

### 2.6 Log

#### `GET /api/log`

Returns the in-memory log ring buffer (up to 32 lines, oldest → newest) as a JSON array of strings.

```bash
curl http://<host>/api/log
```

### 2.7 File upload

#### `POST /api/files/upload`

Uploads a file to LittleFS via `multipart/form-data`. The `path` field specifies the destination.

Allowlisted paths:
- `/switch_layout.json`
- `/config.json`
- `/logo.png`
- `/index.html`
- `/app.js`
- `/style.css`

`.json` files are validated before being moved into place. Maximum upload size is 256 KB.

```bash
curl -X POST http://<host>/api/files/upload \
  -F "path=/switch_layout.json" \
  -F "file=@switch_layout.json"
```

Response: `{ "ok": true, "path": "/switch_layout.json", "bytes": 1234, "reboot_required": true }`

`reboot_required` is `true` for `/switch_layout.json` and `/config.json`.

### 2.8 OTA firmware update

#### `POST /update`

HTTP OTA firmware upload (binary). Handled by `ESP8266HTTPUpdateServer`.

- Credentials: username `admin`, password = the AP password configured in `config.json`
- Use PlatformIO (`pio run -t upload --upload-port <host>`) or the web form at `GET /update`

```bash
curl -u admin:<ap_password> \
  -F "image=@.pio/build/nodemcuv2/firmware.bin" \
  http://<host>/update
```

---

## 3. MQTT

All topics are rooted at the configured `mqtt.base_topic` (firmware default
`paradox/enigma1`; configure it for the installed room, for example
`paradox/<room>/enigma1`). The device subscribes and publishes at QoS 1.

### 3.1 Topic map

| Topic | Direction | Retained | Description |
|-------|-----------|----------|-------------|
| `<base>/commands` | → device | No | Send commands to the device |
| `<base>/config` | → device | **Yes** | Retained config override (deep-merged into RAM, not saved to flash) |
| `<base>/state` | ← device | No | Periodic state heartbeat + LWT |
| `<base>/events` | ← device | No | Command outcomes, system events, code changes |
| `<base>/warnings` | ← device | No | Warnings (invalid payloads, config errors, etc.) |
| `<announce_topic>` | ← device | No | Online announcement on connect |

### 3.2 Commands (`<base>/commands`)

All command payloads are JSON objects with a mandatory `command` field and an optional `request_id` string (echoed back in the outcome event).

```json
{ "command": "<name>", "request_id": "optional-correlation-id", ... }
```

For every command the device publishes two events to `<base>/events`:
1. `command_received` — immediately on receipt
2. Exactly one of `command_success`, `command_warning`, or `command_failed` — after execution

#### Workspace standard commands

| Command | Parameters | Description |
|---------|------------|-------------|
| `ping` | — | Health check; device responds with a `pong` event |
| `getState` | — | Forces an immediate state publish |
| `identify` | — | Triggers identify animation (flash/blink display) |
| `restart` | — | Schedules a device reboot |
| `reloadConfig` | — | Reloads `/config.json` from flash, discarding any RAM override |

```bash
mosquitto_pub -h <broker> -t paradox/enigma1/commands \
  -m '{"command":"ping","request_id":"abc123"}'
```

#### Puzzle commands

**`setBrightness`** — Set display brightness (0–15, HT16K33 native scale).

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `brightness` | integer 0–15 | Yes | Display brightness |
| `persist` | bool | No (default `true`) | Save to `/config.json` |

```bash
mosquitto_pub -h <broker> -t paradox/enigma1/commands \
  -m '{"command":"setBrightness","brightness":8}'
```

---

**`setTarget`** — Set the target code. The puzzle is "solved" when the switch matrix reading matches this value.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `target` | string or `null` | Yes | Decimal code in `XX-YY-ZZ` format (or digits with optional spaces/hyphens), or `null` to clear |

```bash
mosquitto_pub -h <broker> -t paradox/enigma1/commands \
  -m '{"command":"setTarget","target":"12-34-56"}'

# Clear target
mosquitto_pub -h <broker> -t paradox/enigma1/commands \
  -m '{"command":"setTarget","target":null}'
```

---

**`clearTarget`** — Removes the current target (equivalent to `setTarget` with `null`). Persists the change.

```bash
mosquitto_pub -h <broker> -t paradox/enigma1/commands \
  -m '{"command":"clearTarget"}'
```

---

**`setMode`** — Switch between `live` (code always reflects current switches) and `latching` (code locks on first change and requires an explicit `reset` to unlock).

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `mode` | `"live"` \| `"latching"` | Yes | Puzzle mode |

```bash
mosquitto_pub -h <broker> -t paradox/enigma1/commands \
  -m '{"command":"setMode","mode":"latching"}'
```

---

**`reset`** — Clears the latched code state (Latching mode only). Does not reboot.

```bash
mosquitto_pub -h <broker> -t paradox/enigma1/commands \
  -m '{"command":"reset"}'
```

---

**`getCode`** — Forces a `code_changed` event publish with the current code reading.

```bash
mosquitto_pub -h <broker> -t paradox/enigma1/commands \
  -m '{"command":"getCode"}'
```

---

**`setBatteryProfile`** — Configure the battery monitoring profile.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `profile` | string | Yes | One of: `external`, `unknown`, `12v-lead-acid`, `12v-lifepo4`, `6v-lead-acid`, `6v-lifepo4`, `custom` |
| `points` | string or array | No | Custom curve data (for `custom` profile) |

```bash
mosquitto_pub -h <broker> -t paradox/enigma1/commands \
  -m '{"command":"setBatteryProfile","profile":"12v-lead-acid"}'
```

---

**`setSignalIndicator`** — Enable or disable the RSSI signal indicator on the display and optionally configure its thresholds.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `enabled` | bool | Yes | Enable/disable signal indicator |
| `rssi_dbm` | array of 7 integers | No | Strictly decreasing dBm thresholds (−127..0) |

```bash
mosquitto_pub -h <broker> -t paradox/enigma1/commands \
  -m '{"command":"setSignalIndicator","enabled":true}'

mosquitto_pub -h <broker> -t paradox/enigma1/commands \
  -m '{"command":"setSignalIndicator","enabled":true,"rssi_dbm":[-50,-60,-70,-75,-80,-85,-90]}'
```

---

**`on`** — Re-enable display output (reverse of `off`).

```bash
mosquitto_pub -h <broker> -t paradox/enigma1/commands \
  -m '{"command":"on"}'
```

---

**`off`** — Blank the display until `on` is received.

```bash
mosquitto_pub -h <broker> -t paradox/enigma1/commands \
  -m '{"command":"off"}'
```

---

**`sleep`** — Enter deep sleep immediately (operator override). Publishes `command_success` with warning `going_to_sleep`, shows the sleep indicator (two middle code dashes only), then a `going_to_sleep` system event, then calls `ESP.deepSleep(0)`. Wake requires a power cycle.

```bash
mosquitto_pub -h <broker> -t paradox/enigma1/commands \
  -m '{"command":"sleep"}'
```

### 3.3 Retained config override (`<base>/config`)

Publishing a retained JSON payload to this topic deep-merges it into the running RAM config without rebooting. Only the keys present in the payload are modified; all other config fields are preserved.

The override is **not** written to flash. On reboot the device loads `/config.json` as normal and re-applies whatever retained payload is still on the broker.

To clear an active override without rebooting send the `reloadConfig` command or publish an empty retained payload (clears the broker copy; no change to running state).

```bash
# Override display brightness and battery profile in RAM
mosquitto_pub -h <broker> -r \
  -t paradox/enigma1/config \
  -m '{"display":{"brightness":12},"battery":{"profile":"12v-lead-acid"}}'

# Clear the retained override from the broker
mosquitto_pub -h <broker> -r -t paradox/enigma1/config -m ''
```

### 3.4 State topic (`<base>/state`)

Published on connect, on demand via `getState`, after state changes, and on a configurable heartbeat interval (default 10 s). The LWT (Last Will and Testament) is also delivered to this topic when the device disconnects unexpectedly:

```json
{ "ts": 0, "application": "px-enigma-esp8266", "instance": "enigma-01", "status": "offline" }
```

See section 2.3 for the full live state schema.

### 3.5 Events topic (`<base>/events`)

All event messages share this envelope:

```json
{
  "ts": 342000,
  "type": "<type>",
  "event": "<event>",
  "message": "<optional string>",
  "data": { }
}
```

Common events:

| `type` | `event` | Description |
|--------|---------|-------------|
| `system` | `pong` | Response to `ping` command |
| `command` | `command_received` | Acknowledgement on receipt |
| `command` | `command_success` | Command executed successfully |
| `command` | `command_failed` | Command rejected (see `message` for reason) |
| `code` | `code_changed` | Switch matrix reading changed; `data` has `code`, `code_int`, `grid` |
| `config` | `config_override_applied` | Retained config override merged; `data.keys` lists changed leaf paths |

### 3.6 Warnings topic (`<base>/warnings`)

```json
{
  "ts": 342000,
  "warning": "<warning_code>",
  "message": "<human description>",
  "data": { }
}
```

Common warning codes:

| `warning` | Cause |
|-----------|-------|
| `invalid_payload` | Malformed JSON or missing `command` field (rate-limited to 1/min) |
| `config_invalid` | `/config.json` failed schema validation; defaults were used |
| `config_invalid` | Retained config override was rejected |

### 3.7 Announce topic

Configured via `mqtt.announce_topic` (default `paradox/props`). Published once on each successful MQTT connect:

```json
{
  "ts": 0,
  "event": "online",
  "application": "px-enigma-esp8266",
  "instance": "enigma-01",
  "prop_name": "Enigma Machine",
  "version": "1.0.2",
  "ip": "192.168.1.42",
  "ap_ip": "192.168.4.1",
  "mdns": "enigma-machine",
  "mdns_fqdn": "enigma-machine.local",
  "mac": "AA:BB:CC:DD:EE:FF",
  "base_topic": "paradox/enigma1",
  "commands_topic": "paradox/enigma1/commands",
  "config_topic": "paradox/enigma1/config"
}
```

---

## 4. ArduinoOTA

The device registers itself on mDNS as `<network_name>.local` (derived from `prop_name`) and accepts ArduinoOTA firmware uploads on the standard port 8266.

Password: the AP password configured in `config.json` (blank = unauthenticated).

```bash
# PlatformIO
pio run -e esp8266 -t upload --upload-port <host>

# arduino-cli / espota.py
python espota.py -i <host> -p 8266 -a <ap_password> -f firmware.bin
```

---

## 5. Quick-reference card

```bash
HOST=192.168.1.42
BASE=paradox/enigma1
BROKER=192.168.1.10

# HTTP — get state
curl http://$HOST/api/state

# HTTP — set brightness
curl -X POST http://$HOST/api/config \
  -H 'Content-Type: application/json' \
  -d '{"display":{"brightness":8}}'

# HTTP — reset puzzle latch
curl -X POST http://$HOST/api/reset

# MQTT — ping
mosquitto_pub -h $BROKER -t $BASE/commands -m '{"command":"ping"}'

# MQTT — set target
mosquitto_pub -h $BROKER -t $BASE/commands \
  -m '{"command":"setTarget","target":"1A-2B-3C"}'

# MQTT — watch all device output
mosquitto_sub -h $BROKER -t "$BASE/#" -v

# SSE — live code stream
curl -N http://$HOST/api/events
```
