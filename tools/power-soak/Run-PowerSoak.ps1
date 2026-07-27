# Monitor px-enigma uptime over Wi-Fi while USB is unplugged (12V-only test).
#
# Usage (from repo root or this folder):
#   .\tools\power-soak\Run-PowerSoak.ps1 -DeviceHost 192.168.1.100
#   .\tools\power-soak\Run-PowerSoak.ps1 -DeviceHost 192.168.4.1 -PollSeconds 3
#   .\tools\power-soak\Run-PowerSoak.ps1 -DeviceHost 192.168.1.100 -DurationMinutes 10
#
# Recommended workflow:
#   1. Prop on 12V, USB still connected — confirm /api/state responds.
#   2. Start this script.
#   3. Unplug USB (prop must run from 12V only).
#   4. Watch for "REBOOT" lines (uptime_s dropped) or repeated fetch failures.

param(
    [Parameter(Mandatory = $true)]
    [string]$DeviceHost,

    [int]$PollSeconds = 5,
    [int]$DurationMinutes = 0,
    [int]$TimeoutSeconds = 8
)

$BaseUrl = "http://$DeviceHost/api/state"
$PollCount = 0
$FetchOk = 0
$FetchFail = 0
$RebootCount = 0
$LastUptime = -1
$Started = Get-Date
$Deadline = if ($DurationMinutes -gt 0) { $Started.AddMinutes($DurationMinutes) } else { $null }

function Write-Stamp([string]$Message) {
    $ts = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    Write-Host "[$ts] $Message"
}

Write-Stamp "Power soak starting - $BaseUrl every ${PollSeconds}s"
Write-Stamp 'Unplug USB now if you have not already (12V-only test).'
Write-Stamp "Press Ctrl+C to stop."
Write-Host ""

try {
    $probe = Invoke-RestMethod -Uri $BaseUrl -TimeoutSec $TimeoutSeconds
    Write-Stamp ("Device OK: {0} v{1} uptime={2}s heap={3}B sta={4} mqtt={5}" -f `
        $probe.instance, $probe.version, $probe.uptime_s, `
        $probe.health.free_heap_bytes, $probe.wifi.sta.connected, $probe.mqtt.connected)
    $LastUptime = [int]$probe.uptime_s
}
catch {
    Write-Stamp "ERROR: Cannot reach $BaseUrl - fix Wi-Fi/host first, then retry."
    Write-Stamp $_.Exception.Message
    exit 1
}

while ($true) {
    if ($Deadline -and (Get-Date) -ge $Deadline) {
        Write-Stamp "Duration elapsed."
        break
    }

    Start-Sleep -Seconds $PollSeconds
    $PollCount++

    try {
        $state = Invoke-RestMethod -Uri $BaseUrl -TimeoutSec $TimeoutSeconds
        $FetchOk++
        $uptime = [int]$state.uptime_s
        $heap = $state.health.free_heap_bytes
        $code = $state.code.code
        $sta = $state.wifi.sta.connected
        $rssi = $state.wifi.sta.rssi
        $mqtt = $state.mqtt.connected

        if ($LastUptime -ge 0 -and $uptime -lt $LastUptime) {
            $RebootCount++
            Write-Stamp "REBOOT #$RebootCount - uptime dropped ${LastUptime}s -> ${uptime}s (likely brownout/reset)"
        }
        $LastUptime = $uptime

        if ($PollCount % 6 -eq 0) {
            Write-Stamp ("poll {0}: uptime={1}s code={2} heap={3}B sta={4} rssi={5} mqtt={6}" -f `
                $PollCount, $uptime, $code, $heap, $sta, $rssi, $mqtt)
        }
    }
    catch {
        $FetchFail++
        Write-Stamp "FETCH FAIL #$FetchFail - device unreachable (reset loop or Wi-Fi down?)"
    }
}

Write-Host ""
Write-Stamp "=== Power soak summary ==="
Write-Stamp "Elapsed:        $([int]((Get-Date) - $Started).TotalSeconds)s"
Write-Stamp "Polls OK/FAIL:  $FetchOk / $FetchFail"
Write-Stamp "Reboots seen:   $RebootCount"
Write-Stamp "Last uptime:    ${LastUptime}s"
if ($RebootCount -gt 0 -or $FetchFail -gt 3) {
    Write-Stamp "Assessment:     UNSTABLE - investigate 12V supply/regulation/wiring"
}
else {
    Write-Stamp "Assessment:     STABLE over test window"
}
