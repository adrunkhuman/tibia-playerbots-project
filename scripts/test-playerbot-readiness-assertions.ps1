#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
. $PSScriptRoot/playerbot-gameplay/log-parsing.ps1
. $PSScriptRoot/playerbot-gameplay/assertions-readiness.ps1

function New-FoodFixture {
    @(
        @{ event = 'combat_readiness'; vocation_id = 4; result = 'ready'; requirements = @(
            @{ name = 'health_potions'; item_id = 7618; ready = $true }
            @{ name = 'free_capacity'; ready = $true; current = 0; minimum = 2000; reclaimable_food = 3200; effective = 3200 }
            @{ name = 'food'; required = $false; count = 8; reclaimable_weight = 3200 }
            @{ name = 'weapon'; ready = $true }
            @{ name = 'armor'; ready = $true }
        ) }
        @{ event = 'action_result'; action = 'hunt_cycle'; result = 'started' }
        foreach ($remaining in 7..0) {
            @{ event = 'action_result'; action = 'eat'; result = 'success'; item_id = 2696; count = 1; inventory_count = $remaining; food_ticks = (8 - $remaining) * 108000 }
        }
    )
}
function Test-Fixture([array]$Events, [bool]$Reject) {
    $logs = ($Events | ForEach-Object {
        $_.component = 'playerbot'; $_.bot = 'Bot One'
        $_ | ConvertTo-Json -Depth 8 -Compress
    }) -join "`n"
    try { Assert-CombatReadinessEvents -Logs $logs -Mode food_capacity }
    catch { if ($Reject) { return }; throw }
    if ($Reject) { throw 'Invalid food fixture unexpectedly passed.' }
}
Test-Fixture (New-FoodFixture) $false
# Reject the old six-use/two-retained contract, missing and extra uses, and unverified deltas.
Test-Fixture (New-FoodFixture | Select-Object -First 8) $true
Test-Fixture (New-FoodFixture | Select-Object -First 9) $true
$events = @(New-FoodFixture); Test-Fixture ($events + $events[-1]) $true
foreach ($field in @('inventory_count', 'count', 'food_ticks', 'item_id', 'result')) {
    $events = @(New-FoodFixture); $events[5][$field] = 0
    Test-Fixture $events $true
}
$events = @(New-FoodFixture); $events[0].requirements[1].current = 1; Test-Fixture $events $true
$events = @(New-FoodFixture); $events[0].requirements[1].reclaimable_food = 3199; Test-Fixture $events $true
$events = @(New-FoodFixture); $events[0].requirements[2].count = 2; Test-Fixture $events $true
$events = @(New-FoodFixture); $events[1].result = 'failed'; Test-Fixture $events $true
$events = @(New-FoodFixture); Test-Fixture ($events + @{ action = 'buy_meat'; result = 'success' }) $true
$events = @(New-FoodFixture); Test-Fixture ($events + @{ event = 'combat_readiness'; selected_recovery = 'service' }) $true
Write-Host 'Food-capacity assertion regressions passed.'
