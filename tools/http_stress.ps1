param(
    [string]$BaseUrl = "http://awtrix.local",
    [int]$Iterations = 100,
    [int]$DelayMs = 50
)

$ErrorActionPreference = "Continue"

function Invoke-AwtrixPost {
    param(
        [string]$Path,
        [string]$Body
    )
    try {
        Invoke-RestMethod -Method Post -Uri "$BaseUrl$Path" -ContentType "application/json" -Body $Body | Out-Null
        Write-Host "POST $Path ok"
    } catch {
        Write-Warning "POST $Path failed: $($_.Exception.Message)"
    }
}

function Invoke-AwtrixGet {
    param([string]$Path)
    try {
        Invoke-RestMethod -Method Get -Uri "$BaseUrl$Path" | Out-Null
        Write-Host "GET  $Path ok"
    } catch {
        Write-Warning "GET $Path failed: $($_.Exception.Message)"
    }
}

Write-Host "AWTRIX HTTP stress test"
Write-Host "BaseUrl=$BaseUrl Iterations=$Iterations DelayMs=$DelayMs"

for ($i = 0; $i -lt $Iterations; $i++) {
    $brightness = 10 + ($i % 200)
    $color = if (($i % 2) -eq 0) { "#00ff88" } else { "#ff8800" }

    Invoke-AwtrixPost "/api/notify" "{`"text`":`"stress $i`",`"color`":`"$color`",`"duration`":2}"
    Invoke-AwtrixPost "/api/settings" "{`"BRI`":$brightness}"
    Invoke-AwtrixPost "/api/custom" "{`"name`":`"stress`",`"text`":`"$i`",`"color`":`"$color`",`"duration`":5}"
    Invoke-AwtrixGet "/api/apps"
    Invoke-AwtrixGet "/api/stats"
    Invoke-AwtrixGet "/api/screen"

    Start-Sleep -Milliseconds $DelayMs
}

Write-Host "Done. Watch serial logs for queue overflow, watchdog, heap or reset issues."
