# px-enigma-esp8266 — User Guide

**Status:** Draft for review
**Version:** 0.2.0-draft

This guide is for operators installing or maintaining the Enigma prop. For
developer-facing documentation, see [functional-spec.md](functional-spec.md)
and [implementation-plan.md](implementation-plan.md).

---

## 1. What you have

A wall- or panel-mounted enclosure containing:

- A 4 × 5 grid of toggle switches.
- Two large 4-digit 7-segment displays showing a six-digit code as
  `XX-YY-ZZ`.
- A 12 V power input (wall supply or battery, see §6).

The prop publishes the displayed code over MQTT and reacts to commands
from your show-control system. It is also fully playable **standalone**
with no WiFi or MQTT — the puzzle works the moment power is applied.

## 2. First-time setup

1. **Power.** Connect a 12 V supply to the power input. The displays
   should illuminate and the device should begin scanning the switches
   within ~ 1.5 s of power-on. The puzzle is fully playable from that
   moment, before any network is up.

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

8. **Instance meaning.** In the Web UI, **Identity → Instance** is the
  prop instance ID (default `enigma1`). It is included in state/announce
  payloads and MQTT metadata so multiple Enigma units can be uniquely
  identified in one deployment.

## 3. Day-to-day operation

### What the displays show

The displays continuously reflect the **current state of the switches**
as a six-digit code. Players manipulate switches and watch the code
change in real time.

The prop runs in one of two **puzzle modes** (configurable in the Web
UI under **Puzzle**):

- **Live (default).** The displayed code always tracks the switches.
  If a target is set, a `code_solved` event fires each time the code
  enters the matching state. If players change switches away from the
  target a `code_unsolved` event fires; solving again fires a fresh
  `code_solved`. There is no single-shot guard — solve and unsolve
  events repeat as often as players cross the boundary.
- **Latching.** When the code first matches the target, the prop
  publishes a single `solve` event, **freezes** the matched code on
  the display (blinking at 1 Hz so it's clearly the answer), and
  ignores further switch motion until you send a `reset` command or
  power-cycle the prop.

When the displayed code matches the configured **target**, the prop
publishes the appropriate event (`code_solved` in live mode, `solve` in
latching mode). Your orchestrator (PxO or equivalent) uses that event
to advance the game.

### Setting the target code

Two options:

- **Web UI:** the **Puzzle** section accepts the target as digits or
  in `XX-YY-ZZ` form.
- **MQTT:** publish to `<base_topic>/commands`:

  ```json
  { "command": "setTarget", "target": "12-34-56" }
  ```

Clear the target with `clearTarget` (or `setTarget` with `target: null`).

### Resetting a latched puzzle

In latching mode, after a `solve` event the display freezes. To re-arm
the puzzle for the next playthrough:

```json
{ "command": "reset" }
```

A power cycle has the same effect. `reset` is a no-op in live mode.

### Switching modes at runtime

```json
{ "command": "setMode", "mode": "latching" }
```

Switching from `latching` to `live` while the prop is currently latched
clears the latch silently (no `code_unsolved` is emitted).

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
| `<base>/config`   | in  | **retained** — fleet-wide config overrides (see below) |
| `paradox/props`   | out | online announce on every MQTT (re)connect |

### Retained config override

`<base>/config` is a **retained** topic. Anything you publish to it
with the retain flag set is delivered to the prop on every reconnect
and merged over its in-memory configuration silently:

```
mosquitto_pub -t paradox/site1/enigma1/config -r -m '{"display":{"brightness":3}}'
```

- Top-level keys present in the override fully replace the matching
  key in the live config; keys not present are left untouched.
- The override is **not** persisted to `data/config.json`; it lives
  only in RAM. Reboot reloads `data/config.json` and re-applies
  whatever override is still retained on the broker.
- Clear the override by publishing an **empty retained payload**:
  `mosquitto_pub -t .../config -r -m ''`.

This is the recommended way to push fleet-wide changes (broker host,
brightness, puzzle mode) without redeploying credentials.

Common subscriptions for game integration:

- Subscribe to `<base>/events` and react to:
  - `code_solved` (live mode) or `solve` (latching mode) → advance the puzzle.
  - `code_changed` → update a hint timer.
- Subscribe to `<base>/warnings` and forward `battery_low` /
  `battery_critical` to your monitoring channel.

See [functional-spec.md §12](functional-spec.md) for the full list of
events, fields, and example payloads.

## 5. Updating firmware

Two over-the-air paths, both reachable on AP or LAN:

- **HTTP upload:** browse to `http://<device-ip>/update`, log in with
  the AP password, and upload a new `.bin` produced by `pio run`.
- **ArduinoOTA:** `pio run -t upload --upload-port <prop-name>.local`.

The device reboots into the new firmware automatically.

## 5.1 Updating switch_layout.json and selected web assets via curl

You can download and upload `switch_layout.json` directly over HTTP.

Download current layout from the prop:

```bash
curl --fail --silent --show-error \
  http://10.0.0.100/switch_layout.json \
  -o ./tmp/switch_layout.json
```

Upload edited layout back to the prop:

```bash
curl --fail --silent --show-error \
  -X POST "http://10.0.0.100/api/files/upload" \
  -F "path=/switch_layout.json" \
  -F "file=@./tmp/switch_layout.json;type=application/json"
```

Notes:

- `switch_layout.json` uploads are JSON-validated.
- `switch_layout.json` changes require a restart before firmware scan
  mapping is updated.
- The same upload endpoint supports an allowlisted set of files such as
  `/config.json` and `/logo.png`.

Example logo upload:

```bash
curl --fail --silent --show-error \
  -X POST "http://10.0.0.100/api/files/upload" \
  -F "path=/logo.png" \
  -F "file=@./data/logo.png;type=image/png"
```

## 6. Battery / power monitoring

The Enigma supports running from a battery as well as a wall supply.
Select the source in the Web UI's **Battery** section by choosing a
**profile**:

| Profile         | Use when… |
|-----------------|----------|
| `external`      | Powered from a wall adapter or bench supply. Battery percent reports `100 %` and inactivity sleep is disabled. |
| `unknown`       | You want monitoring disabled but want to keep the option open. Behaves like `external`. |
| `12v-lead-acid` | 12 V SLA / AGM battery |
| `12v-LiFePO4`   | 12 V LiFePO4 (4S) battery |
| `6v-lead-acid`  | 6 V SLA battery |
| `6v-LiFePO4`    | 6 V LiFePO4 (2S) battery |
| `custom`        | Any other chemistry (Li-ion, LiPo, NiMH stacks, etc.) — supply your own discharge curve. |

Every state snapshot reports:

```json
"battery": {
  "profile": "12v-lead-acid",
  "voltage_v": 12.62,
  "percent": 78,
  "status": "ok"
}
```

Status values are `ok`, `low` (below `low_percent`, default 40 %),
`critical` (below `cutoff_percent`, default 10 %), and `external` (for
the `external` / `unknown` profiles). In `critical` the display blanks
the code and shows `CRIT`; the matrix scanner is suspended to avoid
drawing the battery down further.

Thresholds (`low_percent`, `cutoff_percent`, `hysteresis_pct`) are
adjustable in the Web UI. The percent thresholds work across every
profile; the underlying voltages are looked up from the profile's
discharge curve.

### Custom curves and other chemistries (Li-ion, LiPo, NiMH…)

Li-ion, LiPo, NiMH, or any non-listed chemistry is supported via the
`custom` profile. Curves live in `data/config.json` only — they're
authoring-time content, not Web-UI fields. Either form works:

```json
"battery": {
  "profile": "custom",
  "points": "12.6:100,12.0:80,11.5:50,11.0:20,10.5:0"
}
```

```json
"battery": {
  "profile": "custom",
  "points": [
    { "v": 12.6, "p": 100 },
    { "v": 11.5, "p": 50 },
    { "v": 10.5, "p": 0 }
  ]
}
```

2 to 20 points; voltages and percents both monotonically decreasing.

### ADC calibration

The two-point ADC calibration constants (`adc_at_0v_raw`,
`adc_at_full_v_raw`, `adc_full_v`) live in `data/config.json` and are
shown read-only in the Web UI's diagnostics panel. To recalibrate, edit
`data/config.json` and reflash the FS image (`pio run -t uploadfs`), or
`POST /api/config` over the network.

### Inactivity deep sleep

When any battery profile is selected, the firmware runs an inactivity
timer. If no switch changes are detected for `inactivity_minutes`
(default **60**), the prop publishes a `going_to_sleep` event, shows
the sleep indicator on the display (two middle code dashes only — all other
segments off), and enters deep sleep.

**Wake from deep sleep requires a power cycle.** This matches the
prop's normal lifecycle (powered on for the duration of a game, off
between games) and prevents unattended battery drain.

Set `inactivity_minutes` to `0` in the Web UI to disable auto-sleep on
battery.

The **`sleep` MQTT command** (and the inactivity timer) use the same
display pattern: only the two middle code dashes stay lit so operators
can see that power is applied but the prop is not active.

### Optional WiFi / MQTT signal indicator

Toggle this feature under **Display → Signal indicator** in the Web UI.
The setting is stored in `data/config.json` and persists across reboots.
You can also toggle it at runtime via MQTT:

```json
{ "command": "setSignalIndicator", "enabled": true }
```

When enabled, the seven decimal-point lamps on the displays light to show:

- Dots 0–6 (left to right): STA RSSI as a 0–7 bar (5 dB steps; full
  scale at ≥ −55 dBm, dark below −85 dBm or when STA is disconnected).
- Dot 7 (rightmost): MQTT broker connected.

The `XX-YY-ZZ` digits are unaffected. Dots are suppressed during
`identify`, the `LOW_BATT` banner, and `CRIT_BATT`.

The 7 RSSI threshold values default to 5 dBm steps (−55 to −85) and
can be fine-tuned in `data/config.json` (shown read-only in the Web
UI diagnostics panel).

### Standalone (no-network) operation

The Enigma is fully playable with no WiFi or MQTT broker. Network
association runs in the background and never blocks the puzzle. If the
prop boots without ever seeing WiFi, the displays still light, the
switches still drive the code, and `solve` / `code_solved` events are
still emitted to the in-RAM log (visible in the Web UI when the prop
eventually does come online).

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
- The switch grid is exposed in `/api/state` as `code.grid` — each row
  is a string of `'1'`/`'0'` characters matching the physical layout,
  making it straightforward to identify a misbehaving cell.

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
