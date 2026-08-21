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
        if ($lines[$index] -match '"event":"lifecycle"' -and $lines[$index] -match '"status":"online"') {
            return ($lines[$index..($lines.Count - 1)] -join "`n")
        }
    }
    return ""
}
