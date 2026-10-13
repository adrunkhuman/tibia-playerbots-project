#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
. $PSScriptRoot/playerbot-gameplay/log-parsing.ps1
. $PSScriptRoot/playerbot-gameplay/assertions-readiness.ps1

function New-RecoveryFixture {
    @(
        @{ event = 'spear_fixture'; scenario = 'spear_unfunded'; phase = 'start'; spears = 0; carried_gold = 0; bank_gold = 0; right_item_id = 2525 }
        @{ event = 'hunt_region_candidate'; candidate_phase = 'route_validation'; region_id = 17; planning_pass = 2; scoring_revision = 3; recovery_loadout_id = 2; route_validated = $true; suitable = $true; supply_recovery = $true; recovery_net_gold_per_minute = 3 }
        @{ event = 'recovery_loadout'; result = 'selected'; region_id = 17; planning_pass = 2; scoring_revision = 3; loadout_id = 2; left_item_id = 0; right_item_id = 2525; expected_net_gold_per_minute = 3 }
        @{ event = 'recovery_loadout'; result = 'equipped'; left_item_id = 0; right_item_id = 2525 }
        @{ event = 'hunt_area_entered'; region_id = 17 }
        @{ event = 'action_result'; action = 'loot'; result = 'success'; coin_gold_acquired = 1 }
    )
}
function Test-RecoveryFixture([array]$Events, [bool]$Reject = $false) {
    $logs = ($Events | ForEach-Object {
        $_.component = 'playerbot'; $_.bot = 'Bot One'
        $_ | ConvertTo-Json -Depth 8 -Compress
    }) -join "`n"
    try { Assert-UnfundedAlternativeRecoveryEvents -Logs $logs }
    catch { if ($Reject) { return }; throw }
    if ($Reject) { throw 'Invalid alternative recovery evidence passed.' }
}
Test-RecoveryFixture (New-RecoveryFixture)
foreach ($field in @('route_validated', 'suitable', 'supply_recovery')) {
    $events = New-RecoveryFixture; $events[1][$field] = $false
    Test-RecoveryFixture $events $true
}
foreach ($field in @('region_id', 'planning_pass', 'scoring_revision', 'recovery_loadout_id')) {
    $events = New-RecoveryFixture; $events[1][$field] = 999
    Test-RecoveryFixture $events $true
}
$events = New-RecoveryFixture; $events[2].expected_net_gold_per_minute = 0
Test-RecoveryFixture $events $true
$events = New-RecoveryFixture; $events[3].left_item_id = 2389
Test-RecoveryFixture $events $true
Test-RecoveryFixture (New-RecoveryFixture | Select-Object -First 5) $true
$events = New-RecoveryFixture
Test-RecoveryFixture @($events[0..3] + @{ action = 'buy_throwing_weapons'; result = 'success' } + $events[4..5]) $true
$failure = @((New-RecoveryFixture)[0], @{ event = 'terminal'; reason = 'recovery_no_safe_income_outing' })
Test-RecoveryFixture $failure
Test-RecoveryFixture @($failure + @{ event = 'hunt_region_selection' }) $true
$failure[1].reason = 'combat_readiness_throwing_weapon_restock_blocked'
Test-RecoveryFixture $failure $true
Write-Host 'playerbot recovery assertion contracts passed'
