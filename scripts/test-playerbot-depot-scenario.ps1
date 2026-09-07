#Requires -Version 7.0
$ErrorActionPreference = "Stop"
$Depot = $true
$MainlandLoop = $SlottedLoot = $SellLoot = $false
$previous = @{}
foreach ($key in @('PLAYERBOT_GAMEPLAY_MODE', 'PLAYERBOT_DEPOT_RESTART_PHASE', 'PLAYERBOT_DEPOT_VERIFIER_PHASE', 'PLAYERBOT_DEPOT_MOVE_CASE')) {
    $previous[$key] = [Environment]::GetEnvironmentVariable($key)
}
function Invoke-Scenario {
    param($Name, $DefaultTimeoutSeconds, [scriptblock]$Body)
    if ($Name -notmatch '^real_depot($|_restart_|_partial_move$)') { return }
    $script:scenario = $Name
    $script:generation = 0
    $script:stopped = $false
    $script:rearmed = $false
    $script:seeded = $false
    $script:checkpoint = $false
    $script:verified = $false
    $env:PLAYERBOT_DEPOT_VERIFIER_PHASE = ''
    & $Body
    $expectedGenerations = if ($Name -eq 'real_depot_partial_move') { 1 } else { 2 }
    if (-not $script:verified -or $script:generation -ne $expectedGenerations) { throw "Missing completed recovery verification: $Name" }
}
function Invoke-Compose {
    $command = $args -join ' '
    if ($command -eq 'stop server') {
        if (-not $script:checkpoint) { throw 'Stopped before checkpoint' }
        if ($script:scenario -eq 'real_depot' -and -not $script:verified) { throw 'Stopped before inventory verification' }
        $script:stopped = $true
    }
    if ($args[0] -eq 'up') {
        if ($script:generation -eq 1) {
            if ($args -notcontains '--no-deps') { throw 'Recovery must not rerun provisioning' }
            if (-not $script:stopped -or -not $script:rearmed) { throw 'Restart before persisted checkpoint rearm' }
            if ($env:PLAYERBOT_DEPOT_RESTART_PHASE -ne 'depart') { throw 'Recovery must pause before selling' }
            if ($script:scenario -eq 'real_depot') {
                if (-not $script:seeded) { throw 'Missing second-cycle cargo rearm' }
            } elseif ($env:PLAYERBOT_DEPOT_VERIFIER_PHASE -ne ($script:scenario -replace '^real_depot_restart_', '')) {
                throw 'Original recovery phase lost'
            }
        }
        $script:generation++
        $script:checkpoint = $script:verified = $false
    }
}
function Invoke-DatabaseCommand {
    param($Query)
    if ($Query -match 'UPDATE player_storage') {
        if (-not $script:stopped) { throw 'Storage changed while server running' }
        if ($Query -match '50096') { $script:rearmed = $true }
        if ($Query -match '50095') { $script:seeded = $true }
    }
}
function Invoke-DatabaseScalar { param($Query) if ($Query -match '50095') { return 0 }; return 7 }
function Wait-ForLog {
    param($Pattern)
    if ($Pattern -match 'depot_restart_checkpoint') {
        if ($Pattern -notmatch ('"phase":"' + $env:PLAYERBOT_DEPOT_RESTART_PHASE + '"')) { throw 'Wrong checkpoint' }
        $script:checkpoint = $true
    } elseif ($Pattern -match 'DEPOT_PASS') {
        if (-not $script:checkpoint) { throw 'Inventory verification before checkpoint wait' }
        $script:verified = $true
    } else { throw "Unexpected wait: $Pattern" }
    return 'stub logs'
}
function Wait-ForLatestServerGenerationLog { param($Pattern) Wait-ForLog -Pattern $Pattern }
function ConvertFrom-PlayerbotLogs {
    param($Logs)
    if ($script:scenario -eq 'real_depot_partial_move') {
        [pscustomobject]@{event='action_result'; action='deposit'; result='partial'; item_id=2684; requested=2; verified=1; inventory_before=4; inventory_after=3; depot_before=0; depot_after=1}
        [pscustomobject]@{event='action_result'; action='deposit'; result='requested'; requested=2; submitted=1}
        return
    }
    [pscustomobject]@{event='action_result'; action='depot_restart_checkpoint'; result='paused'; phase=$env:PLAYERBOT_DEPOT_RESTART_PHASE}
    [pscustomobject]@{event='state_transition'; to='paused'}
}
function Assert-DepotEvents {
    param($Logs, $ExpectedDepositedCount, $ExpectedEquipmentDeposits, $ExpectedToolDeposits)
    if (-not $script:verified) { throw 'Deposit assertions before Lua verification' }
    if ($ExpectedDepositedCount -ne (3 - $script:generation)) { throw 'Changed deposit counts' }
}
function Assert-DepotRecoveryEvents {
    param($Logs, $Phase)
    if (-not $script:verified -or $Phase -ne $env:PLAYERBOT_DEPOT_VERIFIER_PHASE) { throw 'Recovery assertions lost original phase or inventory verification' }
}
try {
    . (Join-Path $PSScriptRoot 'playerbot-gameplay/scenarios-service.ps1')
    Write-Host 'Depot scenario pause/wait/rearm ordering passed for both cycles, all five restarts, and partial moves.'
} finally {
    foreach ($key in $previous.Keys) { [Environment]::SetEnvironmentVariable($key, $previous[$key]) }
}
