#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
. $PSScriptRoot/playerbot-gameplay/log-parsing.ps1

function Assert-Generation {
    param(
        [string]$Name,
        [string]$Logs,
        [string]$Expected,
        [int]$MinimumOnlineEvents = 0
    )

    $script:serverPlayerbotEvents = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $script:minimumServerOnlineEvents = $MinimumOnlineEvents
    $actual = Get-LatestServerGenerationLogs -Logs $Logs
    if ($actual -cne $Expected) {
        throw "${Name}: unexpected generation logs.`nExpected: [$Expected]`nActual: [$actual]"
    }
}

# Matches printServerVersion() in server/src/otserv.cpp, including Git builds.
$boundary = 'The Forgotten Server - Version 1.5'
$newBoundary = 'The Forgotten Server - Version angelion-base-v1.1-42-gabcdef'
$online = '{"component":"playerbot","bot":"Bot One","event":"lifecycle","status":"online"}'
$hunt = '{"component":"playerbot","bot":"Bot One","event":"goal_selection","goal":"hunt","reason":"pickup_progression"}'
$old = "$boundary`nold generation marker`n$online"
$new = "$newBoundary`n$hunt`n$online`nafter online"

Assert-Generation 'startup hunt before online' $new $new 1
Assert-Generation 'latest process only' "$old`n$new" $new 2
Assert-Generation 'CRLF' ("$old`n$new" -replace "`n", "`r`n") $new 2
Assert-Generation 'minimum online count not reached' "$old`n$new" '' 3
Assert-Generation 'recovery online is not a process boundary' "$new`n$online" "$new`n$online" 2
Assert-Generation 'partial newest startup despite earlier online count' "$old`n$online`n$newBoundary`n$hunt" '' 2
Assert-Generation 'newest boundary only' "$old`n$newBoundary" '' 1
Assert-Generation 'no online' "$newBoundary`n$hunt" ''
Assert-Generation 'missing boundary' "$hunt`n$online" '' 1
Assert-Generation 'empty logs' '' ''
Assert-Generation 'non-playerbot online does not qualify' "$old`n$newBoundary`n$($online.Replace('playerbot', 'other'))" '' 1
Assert-Generation 'offline does not qualify' "$old`n$newBoundary`n$($online.Replace('online', 'offline'))" '' 1
Assert-Generation 'malformed online does not qualify' "$old`n$newBoundary`n$online garbage" '' 1
$spacedOnline = '{ "status": "online", "event": "lifecycle", "component": "playerbot", "bot": "Bot One" }'
Assert-Generation 'JSON property order and whitespace' "$newBoundary`n$hunt`n$spacedOnline" "$newBoundary`n$hunt`n$spacedOnline" 1

Write-Host 'Playerbot log-parsing regressions passed.'
