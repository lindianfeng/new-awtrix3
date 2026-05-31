param(
    [string]$HostName = "192.168.1.10",
    [string]$Prefix = "awtrix",
    [int]$Iterations = 100,
    [int]$DelayMs = 50,
    [string]$MosquittoPub = "mosquitto_pub"
)

$ErrorActionPreference = "Continue"

function Publish-Awtrix {
    param(
        [string]$Topic,
        [string]$Payload
    )
    $fullTopic = "$Prefix/$Topic"
    try {
        & $MosquittoPub -h $HostName -t $fullTopic -m $Payload | Out-Null
        Write-Host "PUB $fullTopic ok"
    } catch {
        Write-Warning "PUB $fullTopic failed: $($_.Exception.Message)"
    }
}

Write-Host "AWTRIX MQTT stress test"
Write-Host "Host=$HostName Prefix=$Prefix Iterations=$Iterations DelayMs=$DelayMs"

for ($i = 0; $i -lt $Iterations; $i++) {
    $brightness = 10 + ($i % 200)
    $color = if (($i % 2) -eq 0) { "#00ff88" } else { "#ff8800" }

    Publish-Awtrix "notify" "{`"text`":`"mqtt stress $i`",`"color`":`"$color`",`"duration`":2}"
    Publish-Awtrix "settings" "{`"BRI`":$brightness}"
    Publish-Awtrix "custom/stress" "{`"text`":`"$i`",`"color`":`"$color`",`"duration`":5}"
    Publish-Awtrix "nextapp" "{}"
    Publish-Awtrix "sendscreen" "{}"

    Start-Sleep -Milliseconds $DelayMs
}

Write-Host "Done. Watch serial logs for dropped event queue entries, watchdog, heap or MQTT reconnect issues."
