# Clawd Tank — Claude Code Hook Event Sender
# Sends activity to the Pi relay (crab.robostadion.com).
# The ESP32 badge polls the relay from anywhere — PC location doesn't matter.
# Usage data comes from the Pi's own usage-sync service, never from here.

param()

$RELAY_URL = "https://crab.robostadion.com/event"

$input_json = $input | Out-String
if (-not $input_json) { exit 0 }

try {
    $data = $input_json | ConvertFrom-Json -ErrorAction Stop
} catch {
    exit 0
}

$payload = @{}
if ($data.tool_name) { $payload["tool"] = $data.tool_name }
if ($data.hook) { $payload["hook"] = $data.hook }
if ($data.session_id) {
    $payload["session"] = $data.session_id.Substring(0, [Math]::Min(16, $data.session_id.Length))
}

if ($payload.Count -eq 0) { exit 0 }

$json_out = $payload | ConvertTo-Json -Compress
try {
    Invoke-RestMethod -Uri $RELAY_URL -Method Post -Body $json_out -ContentType "application/json" -TimeoutSec 3 | Out-Null
} catch {}
