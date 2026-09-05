function ConvertFrom-PlayerbotLogs {
    param([string]$Logs)

    foreach ($line in $Logs -split "`r?`n") {
        if (-not $line.StartsWith('{')) {
            continue
        }
        try {
            $event = $line | ConvertFrom-Json
            if ($event.component -eq "playerbot") {
                $event
            }
        }
        catch {
            continue
        }
    }
}

function Get-LatestServerGenerationLogs {
    param([string]$Logs)

    $onlineEvents = @($script:serverPlayerbotEvents | Where-Object {
        $_.event -eq "lifecycle" -and $_.status -eq "online"
    }).Count
    if ($onlineEvents -lt $script:minimumServerOnlineEvents) {
        return ""
    }

    $lines = @($Logs -split "`r?`n")
    for ($index = $lines.Count - 1; $index -ge 0; --$index) {
        if ($lines[$index] -match '^The Forgotten Server - Version .+') {
            # Startup decisions can precede online; recovery can emit online again.
            $generationLogs = $lines[$index..($lines.Count - 1)] -join "`n"
            $onlineBot = ConvertFrom-PlayerbotLogs -Logs $generationLogs | Where-Object {
                $_.event -eq "lifecycle" -and $_.status -eq "online"
            } | Select-Object -First 1
            if ($onlineBot) {
                return $generationLogs
            }
            # Never fall back to a previous process while the newest one starts.
            return ""
        }
    }
    return ""
}
