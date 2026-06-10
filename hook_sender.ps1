# Clawd Tank — Claude Code Hook Event Sender (HTTP)
# Tries local network first, falls back to Cloudflare tunnel

param()

$LOCAL_URL = "http://192.168.0.70/event"
$TUNNEL_URL = "https://crab.robostadion.com/event"

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

# Estimate token usage from transcript file size (~4 bytes per token)
if ($data.transcript_path -and (Test-Path $data.transcript_path)) {
    $fileSize = (Get-Item $data.transcript_path).Length
    $payload["tokens"] = [int]($fileSize / 4)
}

if ($payload.Count -eq 0) { exit 0 }

$json_out = $payload | ConvertTo-Json -Compress

try {
    Invoke-RestMethod -Uri $LOCAL_URL -Method Post -Body $json_out -ContentType "application/json" -TimeoutSec 1 | Out-Null
} catch {
    try {
        Invoke-RestMethod -Uri $TUNNEL_URL -Method Post -Body $json_out -ContentType "application/json" -TimeoutSec 3 | Out-Null
    } catch {
        exit 0
    }
}
