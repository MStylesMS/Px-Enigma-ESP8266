# px-enigma-esp8266 — User Guide

**Status:** Draft for review
**Version:** 0.1.0-draft

This guide is for operators installing or maintaining the Enigma prop. For
developer-facing documentation, see [functional-spec.md](functional-spec.md)
and [implementation-plan.md](implementation-plan.md).

---

## 1. What you have

A wall- or panel-mounted enclosure containing:

- A 4 × 5 grid of toggle switches.
- Two large 4-digit 7-segment displays showing a six-digit code as
  `XX-YY-ZZ`.
- A 12 V power input.

The prop publishes the displayed code over MQTT and reacts to commands
from your show-control system.

## 2. First-time setup

1. **Power.** Connect a 12 V supply to the power input. The displays
   should illuminate and the device should begin scanning the switches
   within ~ 2 s of power-on.

2. **Find the device.** The Enigma brings up its own WiFi access point
   immediately, even before joining your venue's network.
   - SSID: `px-enigma-XXXX` (the four hex digits are the last two bytes
     of the device's MAC address; printed on the back label).
   - Password: `MCEscher` (default — change it during setup).
   - Connect a phone or laptop to that SSID.

3. **Open the Web UI.** Navigate to `http://192.168.4.1/`. There is no
   login.

4. **Configure WiFi.** Enter your venue's WiFi as **Primary**. Optionally
   add a **Backup** network (the device will fall back to it
   automatically if the primary is unreachable).

5. **Configure MQTT.** Enter your broker host, port, optional
   username/password, and the **base topic** for this prop (default
   `paradox/site1/enigma1`). The device's command, state, event, and
   warning topics are derived from this base.

6. **Set identity.** Give the prop a human-readable name in `prop_name`.
   This becomes the AP SSID, the mDNS hostname, and the value reported
   in announce messages.

7. **Save.** The device applies what it can live and prompts for a
   reboot only if necessary (changing WiFi credentials usually requires
   a reboot).

## 3. Day-to-day operation

### What the displays show

The displays continuously reflect the **current state of the switches**
as a six-digit code. Players manipulate switches and watch the code
change in real time.

When the displayed code matches the configured **target**, the prop
publishes a `code_solved` event. Your orchestrator (PxO or equivalent)
uses that event to advance the game.

### Setting the target code

Two options:

- **Web UI:** the **Puzzle** section accepts the target as digits or
  in `XX-YY-ZZ` form.
- **MQTT:** publish to `<base_topic>/commands`:

  ```json
  { "command": "setTarget", "target": "12-34-56" }
  ```

Clear the target with `clearTarget` (or `setTarget` with `target: null`).

### Brightness

Brightness is `0`–`15`. The default is `1` (legible without being
overpowering). Adjust via the Web UI or:

```json
{ "command": "setBrightness", "brightness": 5 }
```

### Identifying a unit

To make a specific Enigma flash so you can identify it on a wall of
props:

```json
{ "command": "identify" }
```

The display shows `8888 8888` for ~ 2 s, then resumes.

### Restart

```json
{ "command": "restart" }
```

…or use the **Restart** button in the Web UI.

## 4. MQTT integration cheat sheet

Topics (replace `<base>` with your configured base topic):

| Topic | Direction | Payload |
|---|---|---|
| `<base>/commands` | in  | `{ "command": "...", ... }` |
| `<base>/state`    | out | full state snapshot every 10 s + on demand |
| `<base>/events`   | out | code changes, solves, command outcomes |
| `<base>/warnings` | out | low battery, malformed commands |
| `paradox/props`   | out | online announce on every MQTT (re)connect |

Common subscriptions for game integration:

- Subscribe to `<base>/events` and react to:
  - `code_solved` → advance the puzzle.
  - `code_changed` → update a hint timer.
- Subscribe to `<base>/warnings` and forward `battery_low` /
  `battery_critical` to your monitoring channel.

See [functional-spec.md §10](functional-spec.md) for the full list of
events, fields, and example payloads.

## 5. Updating firmware

Two over-the-air paths, both reachable on AP or LAN:

- **HTTP upload:** browse to `http://<device-ip>/update`, log in with
  the AP password, and upload a new `.bin` produced by `pio run`.
- **ArduinoOTA:** `pio run -t upload --upload-port <prop-name>.local`.

The device reboots into the new firmware automatically.

## 6. Battery / power monitoring

Even though the Enigma is designed for a permanent 12 V supply, it
monitors that supply on `A0` and reports it in every state snapshot:

```json
"battery": { "voltage_v": 12.62, "status": "ok" }
```

Status values are `ok`, `low` (≤ `low_v`, default 12.35 V), and
`critical` (≤ `critical_v`, default 12.10 V). In `critical` the
display blanks the code and shows `CRIT`; the matrix scanner is
suspended to avoid drawing the supply down further.

Adjust thresholds via the Web UI's **Battery** section or:

```json
{ "command": "setBatteryThresholds", "low_v": 12.20, "critical_v": 11.95 }
```

## 7. Troubleshooting

### The prop joined neither network

- Make sure at least the **Primary** SSID/password are correct in the
  Web UI.
- Check `<base>/state` (or the Web UI's diagnostics panel) for the
  `wifi.sta.ssid` and `wifi.sta.connected` fields.
- The AP stays up regardless; you can always reach `192.168.4.1` over
  the prop's own WiFi.

### MQTT shows the prop offline

- Confirm `mqtt.host` is reachable from the venue WiFi.
- The Web UI's diagnostics panel shows recent log lines including
  reconnect attempts and broker errors.

### A switch position seems "stuck"

- The matrix uses software debouncing; sustained dirty contacts can
  cause flicker. Power-cycle to clear any latched state.
- The raw 20-bit value is exposed in `/api/state` as `code.code_bits`,
  which makes it easy to identify a single misbehaving cell.

### The display is dark but the network surface is up

This is the `OFF` state, intentional. Send:

```json
{ "command": "on" }
```

…or use the Web UI's **On** button. The device may also be in
`CRIT_BATT` (see §6) — check `battery.status` in `/api/state`.

### Recovering from a bad firmware image

Plug a USB cable into the carrier and use `pio run -t upload`. Serial
upload uses the boot-ROM bootloader and works even when the running
firmware is broken.
