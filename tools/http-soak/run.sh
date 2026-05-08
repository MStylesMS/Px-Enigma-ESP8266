#!/usr/bin/env bash
# tools/http-soak/run.sh — px-enigma HTTP soak test
#
# Polls /api/state on a configurable interval, injects HTTP commands
# periodically, and prints a summary at the end (to stdout and optionally
# to a JSON file). Requires: bash, curl, jq, date, bc.
#
# Usage:
#   ./run.sh [options]
#
# Options:
#   --host <ip|host>         Device address              (default: 10.0.0.100)
#   --port <n>               HTTP port                   (default: 80)
#   --duration <time>        Run time: e.g. 30m, 2h, 90s (default: interactive)
#   --poll-interval <time>   State poll interval          (default: 10s)
#   --command-interval <time> Time between command rounds (default: 30s)
#   --no-commands            Do not inject any commands during the soak
#   --timeout <s>            Per-request curl timeout     (default: 8)
#   --json-out <path>        Write final summary as JSON
#   --help

set -euo pipefail

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
die() { echo "ERROR: $*" >&2; exit 1; }
log() { echo "[$(date '+%Y-%m-%dT%H:%M:%SZ')] $*"; }

parse_duration_s() {
    local v="$1"
    if [[ "$v" =~ ^([0-9]+)(s|m|h|ms)?$ ]]; then
        local n="${BASH_REMATCH[1]}" u="${BASH_REMATCH[2]:-s}"
        case "$u" in
            ms) echo "$(( n / 1000 ))"; return ;;
            s)  echo "$n"; return ;;
            m)  echo "$(( n * 60 ))"; return ;;
            h)  echo "$(( n * 3600 ))"; return ;;
        esac
    fi
    die "Invalid duration: $v (use 30s, 5m, 1h)"
}

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
HOST="10.0.0.100"
PORT="80"
DURATION_S=""       # empty = interactive
POLL_S=10
CMD_INTERVAL_S=30
SEND_COMMANDS=1
CURL_TIMEOUT=8
JSON_OUT=""

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --host)              HOST="$2"; shift 2 ;;
        --port)              PORT="$2"; shift 2 ;;
        --duration)          DURATION_S=$(parse_duration_s "$2"); shift 2 ;;
        --poll-interval)     POLL_S=$(parse_duration_s "$2"); shift 2 ;;
        --command-interval)  CMD_INTERVAL_S=$(parse_duration_s "$2"); shift 2 ;;
        --no-commands)       SEND_COMMANDS=0; shift ;;
        --timeout)           CURL_TIMEOUT="$2"; shift 2 ;;
        --json-out)          JSON_OUT="$2"; shift 2 ;;
        --help)
            sed -n '/^# Usage:/,/^[^#]/{ /^# /p }' "$0" | sed 's/^# //'
            exit 0 ;;
        *) die "Unknown argument: $1" ;;
    esac
done

BASE_URL="http://${HOST}:${PORT}"

# ---------------------------------------------------------------------------
# Verify tools
# ---------------------------------------------------------------------------
for tool in curl jq date; do
    command -v "$tool" >/dev/null 2>&1 || die "$tool is required but not found"
done

# ---------------------------------------------------------------------------
# Counters / state
# ---------------------------------------------------------------------------
POLL_COUNT=0
FETCH_OK=0
FETCH_FAIL=0
REBOOT_COUNT=0
CODE_CHANGES=0
COMMANDS_SENT=0
CMD_IDENTIFY=0
CMD_RESET=0
CMD_BRIGHTNESS=0
BRIGHTNESS_CURRENT=1   # track current brightness for toggle

LAST_UPTIME=-1
LAST_CODE=""
MIN_HEAP=""
MAX_HEAP=""
LATEST_HEAP=""
LATEST_UPTIME=""
LATEST_STATUS=""
LATEST_RSSI=""
MAX_GAP_S=0
LAST_FETCH_TIME=0

STARTED_AT=$(date +%s)
PREV_CMD_TIME=$STARTED_AT

# Command rotation index
CMD_INDEX=0
# Commands to cycle through (rotate on each CMD_INTERVAL tick)
COMMAND_NAMES=("identify" "reset" "identify" "brightness" "identify" "reset")

# ---------------------------------------------------------------------------
# Functions
# ---------------------------------------------------------------------------
fetch_state() {
    curl -sf --max-time "$CURL_TIMEOUT" "${BASE_URL}/api/state" 2>/dev/null
}

post_identify() {
    curl -sf --max-time "$CURL_TIMEOUT" -X POST "${BASE_URL}/api/identify" >/dev/null 2>&1
}

post_reset() {
    curl -sf --max-time "$CURL_TIMEOUT" -X POST "${BASE_URL}/api/reset" >/dev/null 2>&1
}

post_brightness() {
    local b="$1"
    # POST /api/config with partial brightness override
    curl -sf --max-time "$CURL_TIMEOUT" \
        -X POST -H "Content-Type: application/json" \
        -d "{\"display\":{\"brightness\":$b}}" \
        "${BASE_URL}/api/config" >/dev/null 2>&1
}

inject_next_command() {
    local cmd="${COMMAND_NAMES[$((CMD_INDEX % ${#COMMAND_NAMES[@]}))]}"
    CMD_INDEX=$(( CMD_INDEX + 1 ))
    COMMANDS_SENT=$(( COMMANDS_SENT + 1 ))
    case "$cmd" in
        identify)
            if post_identify; then
                CMD_IDENTIFY=$(( CMD_IDENTIFY + 1 ))
                log "cmd -> identify (ok)"
            else
                log "cmd -> identify (FAILED)"
            fi
            ;;
        reset)
            if post_reset; then
                CMD_RESET=$(( CMD_RESET + 1 ))
                log "cmd -> reset (ok)"
            else
                log "cmd -> reset (FAILED)"
            fi
            ;;
        brightness)
            if [[ "$BRIGHTNESS_CURRENT" -lt 8 ]]; then
                BRIGHTNESS_CURRENT=8
            else
                BRIGHTNESS_CURRENT=1
            fi
            if post_brightness "$BRIGHTNESS_CURRENT"; then
                CMD_BRIGHTNESS=$(( CMD_BRIGHTNESS + 1 ))
                log "cmd -> setBrightness ${BRIGHTNESS_CURRENT} (ok)"
            else
                log "cmd -> setBrightness (FAILED)"
            fi
            ;;
    esac
}

process_state() {
    local json="$1"
    local now_s
    now_s=$(date +%s)

    # Gap since last successful fetch
    if [[ "$LAST_FETCH_TIME" -gt 0 ]]; then
        local gap=$(( now_s - LAST_FETCH_TIME ))
        if [[ "$gap" -gt "$MAX_GAP_S" ]]; then
            MAX_GAP_S="$gap"
        fi
        if [[ "$gap" -gt $(( POLL_S * 3 )) ]]; then
            log "WARN: large gap since last fetch: ${gap}s"
        fi
    fi
    LAST_FETCH_TIME="$now_s"

    # Parse fields with jq
    local uptime status heap code rssi
    uptime=$(echo "$json" | jq -r '.uptime_s // empty')
    status=$(echo "$json" | jq -r '.status // "unknown"')
    heap=$(echo  "$json" | jq -r '.health.free_heap_bytes // empty')
    code=$(echo  "$json" | jq -r '.code.code // "unknown"')
    rssi=$(echo  "$json" | jq -r '.wifi.sta.rssi // empty')

    LATEST_STATUS="$status"
    LATEST_UPTIME="${uptime:-?}"
    LATEST_RSSI="${rssi:-?}"

    # Heap tracking
    if [[ -n "$heap" ]]; then
        LATEST_HEAP="$heap"
        if [[ -z "$MIN_HEAP" || "$heap" -lt "$MIN_HEAP" ]]; then MIN_HEAP="$heap"; fi
        if [[ -z "$MAX_HEAP" || "$heap" -gt "$MAX_HEAP" ]]; then MAX_HEAP="$heap"; fi
    fi

    # Reboot detection (uptime dropped)
    if [[ -n "$uptime" && "$LAST_UPTIME" -ge 0 ]]; then
        if [[ "$uptime" -lt "$LAST_UPTIME" ]]; then
            REBOOT_COUNT=$(( REBOOT_COUNT + 1 ))
            log "WARN: probable reboot detected — uptime dropped ${LAST_UPTIME}s -> ${uptime}s"
        fi
    fi
    if [[ -n "$uptime" ]]; then LAST_UPTIME="$uptime"; fi

    # Code change tracking
    if [[ -n "$code" && "$code" != "$LAST_CODE" && -n "$LAST_CODE" ]]; then
        CODE_CHANGES=$(( CODE_CHANGES + 1 ))
        log "code changed: ${LAST_CODE} -> ${code}"
    fi
    LAST_CODE="$code"

    FETCH_OK=$(( FETCH_OK + 1 ))

    # Progress line every 6 polls (~ 1 min at default interval)
    if (( POLL_COUNT % 6 == 0 )); then
        log "poll ${POLL_COUNT}: status=${status} uptime=${uptime:-?}s code=${code} heap=${heap:-?}B rssi=${rssi:-?}dBm"
    fi
}

build_summary_json() {
    local now_s
    now_s=$(date +%s)
    local elapsed=$(( now_s - STARTED_AT ))

    # Fetch final log tail for the summary
    local log_tail
    log_tail=$(curl -sf --max-time "$CURL_TIMEOUT" "${BASE_URL}/api/log" 2>/dev/null || echo "[]")
    local log_lines
    log_lines=$(echo "$log_tail" | jq -r 'if type == "array" then length else 0 end')

    # Compute issues list
    local issues="[]"
    if [[ "$FETCH_FAIL" -gt 0 ]]; then
        issues=$(echo "$issues" | jq --arg m "Fetch failures occurred (${FETCH_FAIL})" '. + [$m]')
    fi
    if [[ "$REBOOT_COUNT" -gt 0 ]]; then
        issues=$(echo "$issues" | jq --arg m "Probable reboots detected (${REBOOT_COUNT})" '. + [$m]')
    fi
    if [[ "$MAX_GAP_S" -gt $(( POLL_S * 4 )) ]]; then
        issues=$(echo "$issues" | jq --argjson g "$MAX_GAP_S" --argjson t "$(( POLL_S * 4 ))" \
            '. + ["Max fetch gap " + ($g|tostring) + "s exceeded threshold " + ($t|tostring) + "s"]')
    fi
    local assessment
    assessment=$(echo "$issues" | jq -r 'if length == 0 then "stable" else "attention-needed" end')

    jq -n \
        --arg host    "$HOST" \
        --arg base    "$BASE_URL" \
        --argjson elapsed "$elapsed" \
        --arg started_at "$(date -d "@$STARTED_AT" '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || date -r "$STARTED_AT" '+%Y-%m-%dT%H:%M:%SZ')" \
        --arg ended_at   "$(date '+%Y-%m-%dT%H:%M:%SZ')" \
        --argjson polls       "$POLL_COUNT" \
        --argjson fetch_ok    "$FETCH_OK" \
        --argjson fetch_fail  "$FETCH_FAIL" \
        --argjson reboots     "$REBOOT_COUNT" \
        --argjson code_changes "$CODE_CHANGES" \
        --argjson max_gap_s   "$MAX_GAP_S" \
        --argjson min_heap    "${MIN_HEAP:-null}" \
        --argjson max_heap    "${MAX_HEAP:-null}" \
        --argjson latest_heap "${LATEST_HEAP:-null}" \
        --argjson last_uptime "${LAST_UPTIME}" \
        --arg     last_status "${LATEST_STATUS}" \
        --argjson cmds_sent   "$COMMANDS_SENT" \
        --argjson cmd_identify  "$CMD_IDENTIFY" \
        --argjson cmd_reset     "$CMD_RESET" \
        --argjson cmd_brightness "$CMD_BRIGHTNESS" \
        --argjson log_lines   "$log_lines" \
        --argjson issues      "$issues" \
        --arg assessment      "$assessment" \
    '{
      host: $host,
      base_url: $base,
      started_at: $started_at,
      ended_at: $ended_at,
      elapsed_s: $elapsed,
      polls: {
        count: $polls,
        ok: $fetch_ok,
        failed: $fetch_fail,
        max_gap_s: $max_gap_s
      },
      health: {
        min_free_heap_bytes: $min_heap,
        max_free_heap_bytes: $max_heap,
        latest_free_heap_bytes: $latest_heap
      },
      device: {
        last_uptime_s: $last_uptime,
        last_status: $last_status,
        probable_reboots: $reboots,
        code_changes: $code_changes
      },
      commands: {
        sent: $cmds_sent,
        identify: $cmd_identify,
        reset: $cmd_reset,
        brightness: $cmd_brightness
      },
      log_lines_at_end: $log_lines,
      assessment: $assessment,
      issues: $issues
    }'
}

print_summary() {
    local json="$1"
    echo ""
    echo "=== px-enigma HTTP Soak Summary ==="
    echo "$json" | jq .
}

# ---------------------------------------------------------------------------
# Startup probe
# ---------------------------------------------------------------------------
log "Soak starting — probing ${BASE_URL}/api/state ..."
PROBE=$(fetch_state) || die "Device not reachable at ${BASE_URL}"
FIRST_UPTIME=$(echo "$PROBE" | jq -r '.uptime_s // 0')
FIRST_CODE=$(echo   "$PROBE" | jq -r '.code.code // "unknown"')
FIRST_HEAP=$(echo   "$PROBE" | jq -r '.health.free_heap_bytes // 0')
FIRST_VER=$(echo    "$PROBE" | jq -r '.version // "unknown"')
FIRST_INSTANCE=$(echo "$PROBE" | jq -r '.instance // "unknown"')
LAST_CODE="$FIRST_CODE"
LAST_UPTIME="$FIRST_UPTIME"
MIN_HEAP="$FIRST_HEAP"
MAX_HEAP="$FIRST_HEAP"
LATEST_HEAP="$FIRST_HEAP"
LAST_FETCH_TIME=$(date +%s)

log "Device: ${FIRST_INSTANCE} v${FIRST_VER} uptime=${FIRST_UPTIME}s code=${FIRST_CODE} heap=${FIRST_HEAP}B"
if [[ -n "$DURATION_S" ]]; then
    log "Run duration: ${DURATION_S}s, polling every ${POLL_S}s, commands every ${CMD_INTERVAL_S}s"
else
    log "Running interactively. Press Ctrl-C to stop."
fi
[[ "$SEND_COMMANDS" -eq 0 ]] && log "Command injection disabled."
echo ""

# ---------------------------------------------------------------------------
# Trap for clean exit
# ---------------------------------------------------------------------------
STOPPING=0
finish() {
    if [[ "$STOPPING" -eq 1 ]]; then return; fi
    STOPPING=1
    log "Stopping soak..."
    local summary
    summary=$(build_summary_json)

    if [[ -n "$JSON_OUT" ]]; then
        mkdir -p "$(dirname "$JSON_OUT")"
        echo "$summary" > "$JSON_OUT"
        log "Wrote summary to $JSON_OUT"
    fi

    print_summary "$summary"
    exit 0
}
trap finish SIGINT SIGTERM

# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------
END_TIME=""
if [[ -n "$DURATION_S" ]]; then
    END_TIME=$(( $(date +%s) + DURATION_S ))
fi

while true; do
    # Duration check
    if [[ -n "$END_TIME" && "$(date +%s)" -ge "$END_TIME" ]]; then
        log "Duration elapsed."
        finish
    fi

    # Poll state
    POLL_COUNT=$(( POLL_COUNT + 1 ))
    STATE=$(fetch_state) || {
        FETCH_FAIL=$(( FETCH_FAIL + 1 ))
        log "WARN: fetch failed (total fails: ${FETCH_FAIL})"
        sleep "$POLL_S"
        continue
    }
    process_state "$STATE"

    # Command injection
    if [[ "$SEND_COMMANDS" -eq 1 ]]; then
        NOW_S=$(date +%s)
        if [[ $(( NOW_S - PREV_CMD_TIME )) -ge "$CMD_INTERVAL_S" ]]; then
            PREV_CMD_TIME="$NOW_S"
            inject_next_command
        fi
    fi

    sleep "$POLL_S"
done
