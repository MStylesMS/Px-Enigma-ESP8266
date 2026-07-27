# Tools Overview

This directory contains helper scripts for operating and validating px-enigma.

## 1) HTTP soak test

Script: `tools/http-soak/run.sh`

Purpose:
- Poll the prop for a long period and optionally send periodic commands.
- Produce a JSON summary with pass/fail assessment.

Example:

```bash
cd /opt/repos/esp8266/px-enigma-esp8266
./tools/http-soak/run.sh \
  --host 10.0.0.100 \
  --duration 30m \
  --poll-interval 10s \
  --command-interval 30s \
  --json-out logs/enigma-soak-30m.json
```

### 12 V-only power test (USB unplugged)

Serial monitoring is unavailable when USB is unplugged, so use Wi-Fi instead.

**Windows:** `tools/power-soak/Run-PowerSoak.ps1`

1. Prop powered from 12 V, USB still connected.
2. Confirm reachability: `curl http://192.168.1.100/api/state` (STA) or
   join the device AP and use `http://192.168.4.1/api/state`.
3. Start the monitor, then unplug USB:

```powershell
cd props\esp8266\Px-Enigma-ESP8266
.\tools\power-soak\Run-PowerSoak.ps1 -DeviceHost 192.168.1.100 -PollSeconds 5
# Unplug USB now — prop must run from 12 V only
```

4. Watch for `REBOOT` lines (uptime dropped) or repeated `FETCH FAIL`.
   Either indicates brownout/reset looping on the 12 V rail.

Prefer monitoring via the device AP (`192.168.4.1`) if STA reconnect is
also failing during resets.

## 2) Code to matrix

Script: `tools/code-to-matrix.sh`

Purpose:
- Convert a code like `12-34-56` into required switch states.
- Uses the exact mapping in `switch_layout.json`, so the result matches the prop.

Usage:

```bash
./tools/code-to-matrix.sh <path-to-switch_layout.json> [CODE]
```

Examples:

```bash
./tools/code-to-matrix.sh ./tmp/switch_layout.json
./tools/code-to-matrix.sh ./tmp/switch_layout.json 12-34-56
```

Behavior:
- If `CODE` is omitted, the script prompts interactively.
- If the switch layout file is missing, unreadable, malformed, or invalid,
  the script exits with an error and tells you to read this README.

## 3) Download switch layout from the prop

The active layout file can be downloaded directly:

```bash
curl --fail --silent --show-error \
  http://10.0.0.100/switch_layout.json \
  -o ./tmp/switch_layout.json
```

## 4) Upload switch layout (or other allowlisted files) via curl

The firmware supports multipart upload to an allowlisted set of files.

Endpoint:
- `POST /api/files/upload`

Required form fields:
- `path` (target path on LittleFS)
- `file` (file content)

Examples:

Upload switch layout:

```bash
curl --fail --silent --show-error \
  -X POST "http://10.0.0.100/api/files/upload" \
  -F "path=/switch_layout.json" \
  -F "file=@./tmp/switch_layout.json;type=application/json"
```

Upload logo:

```bash
curl --fail --silent --show-error \
  -X POST "http://10.0.0.100/api/files/upload" \
  -F "path=/logo.png" \
  -F "file=@./data/logo.png;type=image/png"
```

Notes:
- `switch_layout.json` updates require a restart to affect firmware mapping.
- JSON uploads are validated; malformed JSON is rejected.
- If upload fails, read the returned error and verify path is allowlisted.
