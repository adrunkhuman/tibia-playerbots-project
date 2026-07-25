param(
    [ValidateRange(60, 3600)]
    [int]$TimeoutSeconds = 900,
    [switch]$KeepStack
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$composeFile = Join-Path $projectRoot "server\compose.yaml"
$gameplayComposeFile = Join-Path $projectRoot "server\compose.playerbot-gameplay.yaml"
$composeArguments = @("compose", "-f", $composeFile, "-f", $gameplayComposeFile)
$previousMode = $env:PLAYERBOT_GAMEPLAY_MODE
$previousTrips = $env:PLAYERBOT_TRAVERSAL_TRIPS

function Invoke-Compose {
    param([Parameter(ValueFromRemainingArguments)][string[]]$Arguments)

    & docker @composeArguments @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "docker compose failed: $($Arguments -join ' ')"
    }
}

function Get-ServerLogs {
    $output = & docker @composeArguments logs --no-log-prefix server 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Could not read server logs."
    }
    return $output -join "`n"
}

function Wait-ForLog {
    param([string]$Pattern)

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $logs = Get-ServerLogs
        if ($logs -match $Pattern) {
            return $logs
        }
        Start-Sleep -Seconds 2
    }
    throw "Timed out after $TimeoutSeconds seconds waiting for server log pattern: $Pattern"
}

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

function Assert-TripEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $actualCheckpoints = @($events |
        Where-Object { $_.event -eq "action_result" -and $_.action -eq "transition" -and $_.result -eq "success" } |
        ForEach-Object checkpoint)
    $expectedCheckpoints = @(
        "temple_stairs_up",
        "north_stairs_down",
        "cave_stairs_down",
        "rope_up",
        "first_ladder_up",
        "second_ladder_up",
        "third_ladder_up"
    )
    if (($actualCheckpoints -join ',') -ne ($expectedCheckpoints -join ',')) {
        throw "Unexpected traversal checkpoint sequence: $($actualCheckpoints -join ', ')"
    }

    $terminal = @($events | Where-Object { $_.event -eq "terminal" -and $_.reason -eq "trips_completed" })
    if ($terminal.Count -ne 1 -or $terminal[0].trips -ne 1) {
        throw "The one-trip traversal did not emit exactly one successful terminal event."
    }
}

function Assert-RecoveryEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $transition = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "transition" -and
        $_.result -eq "success" -and $_.checkpoint -eq "third_ladder_up" -and $_.trip -eq 1
    })
    $terminal = @($events | Where-Object {
        $_.event -eq "terminal" -and $_.reason -eq "trips_completed" -and $_.trips -eq 1
    })
    if ($transition.Count -ne 1 -or $terminal.Count -ne 1) {
        throw "The interrupted transition was not reconciled exactly once."
    }
}

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    throw "Docker is required to run the playerbot gameplay suite."
}

try {
    & docker info *> $null
    if ($LASTEXITCODE -ne 0) {
        throw "Docker is not running."
    }

    Invoke-Compose down --volumes --remove-orphans
    $env:PLAYERBOT_TRAVERSAL_TRIPS = "1"
    $env:PLAYERBOT_GAMEPLAY_MODE = "traversal"
    Invoke-Compose up --build --detach

    $traversalLogs = Wait-ForLog -Pattern '"reason":"trips_completed","trips":1'
    Assert-TripEvents -Logs $traversalLogs

    Invoke-Compose stop server
    $env:PLAYERBOT_GAMEPLAY_MODE = "transition_recovery"
    Invoke-Compose up --detach --force-recreate server

    $recoveryLogs = Wait-ForLog -Pattern '"reason":"trips_completed","trips":1'
    Assert-RecoveryEvents -Logs $recoveryLogs

    "PLAYERBOT_GAMEPLAY_TEST PASS"
}
finally {
    try {
        if (-not $KeepStack -and (Get-Command docker -ErrorAction SilentlyContinue)) {
            & docker @composeArguments down --volumes --remove-orphans
            if ($LASTEXITCODE -ne 0) {
                throw "Could not clean up the playerbot gameplay stack."
            }
        }
    }
    finally {
        $env:PLAYERBOT_GAMEPLAY_MODE = $previousMode
        $env:PLAYERBOT_TRAVERSAL_TRIPS = $previousTrips
    }
}
