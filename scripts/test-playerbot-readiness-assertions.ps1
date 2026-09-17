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
function New-LowWealthFixture {
    @(
        @{ event = 'action_result'; action = 'bank_withdraw'; result = 'success'; count = 56; bank_before = 56; bank_after = 0 }
        @{ event = 'action_result'; action = 'equip_readiness'; result = 'success'; item_id = 2384 }
        @{ event = 'action_result'; action = 'hunt_cycle'; result = 'started' }
    )
}
function Test-LowWealthFixture([array]$Events, [bool]$Reject, [bool]$VerifiedInventory = $true) {
    $logs = ($Events | ForEach-Object {
        $_.component = 'playerbot'; $_.bot = 'Bot One'
        $_ | ConvertTo-Json -Depth 8 -Compress
    }) -join "`n"
    if ($VerifiedInventory) { $logs += "`nPLAYERBOT_GAMEPLAY_TEST READINESS_LOW_WEALTH_PASS" }
    try { Assert-LowWealthEvents -Logs $logs }
    catch { if ($Reject) { return }; throw }
    if ($Reject) { throw 'Invalid low-wealth fixture unexpectedly passed.' }
}
Test-LowWealthFixture (New-LowWealthFixture) $false
Test-LowWealthFixture (@(@{ event = 'action_result'; action = 'equip_readiness'; result = 'requested'; item_id = 2384 }) + @(New-LowWealthFixture)) $false
foreach ($field in @('count', 'bank_before', 'bank_after', 'event', 'result')) {
    $events = @(New-LowWealthFixture); $events[0][$field] = 50
    Test-LowWealthFixture $events $true
    $events = @(New-LowWealthFixture); $events[0].Remove($field)
    Test-LowWealthFixture $events $true
}
foreach ($index in 0..2) {
    $events = @(New-LowWealthFixture)
    Test-LowWealthFixture @($events | Select-Object -SkipIndex $index) $true
}
$events = @(New-LowWealthFixture); Test-LowWealthFixture ($events + $events[0]) $true
$events = @(New-LowWealthFixture); $events[1].item_id = 2382; Test-LowWealthFixture $events $true
foreach ($item in @(2384, 2699)) {
    Test-LowWealthFixture (@(New-LowWealthFixture) + @{ event = 'action_result'; action = 'sell'; item_id = $item; result = 'success' }) $true
}
Test-LowWealthFixture (@(New-LowWealthFixture) + @{ event = 'terminal' }) $true
Test-LowWealthFixture (New-LowWealthFixture) $true $false
function New-ToolReplenishmentFixture([bool]$Nested = $false) {
    $itemIds = if ($Nested) { @(2554) } else { @(2120, 2554) }
    $events = @()
    foreach ($itemId in $itemIds) {
        $events += @{ event = 'goal_selection'; to_goal = 'buy_equipment'; npc_id = 1; item_id = $itemId; tool_acquisition = $true }
        $events += @{ event = 'action_result'; action = 'buy_equipment'; result = 'success'; npc_id = 1; item_id = $itemId; price = 50; tool_acquisition = $true; carried_before = 100; carried_after = 50; bank_before = 1000; bank_after = 1000 }
        $events += @{ event = 'action_result'; action = 'acquire_tool'; result = 'success'; npc_id = 1; item_id = $itemId; tool_acquisition = $true }
        $events += @{ event = 'goal_result'; goal = 'buy_equipment'; result = 'success'; npc_id = 1; item_id = $itemId; tool_acquisition = $true; reason = 'tool_acquired' }
    }
    return $events
}
function Test-ToolReplenishmentFixture([array]$Events, [bool]$Nested, [bool]$Reject) {
    $logs = ($Events | ForEach-Object {
        $_.component = 'playerbot'; $_.bot = 'Bot One'
        $_ | ConvertTo-Json -Compress
    }) -join "`n"
    try { Assert-EquipmentToolReplenishmentEvents -Logs $logs -Nested:$Nested }
    catch { if ($Reject) { return }; throw }
    if ($Reject) { throw 'Invalid tool-replenishment fixture unexpectedly passed.' }
}
Test-ToolReplenishmentFixture (New-ToolReplenishmentFixture) $false $false
Test-ToolReplenishmentFixture (New-ToolReplenishmentFixture $true) $true $false
$events = @(New-ToolReplenishmentFixture); Test-ToolReplenishmentFixture ($events | Select-Object -Skip 1) $false $true
$events = @(New-ToolReplenishmentFixture $true); $events[0].item_id = 2120
Test-ToolReplenishmentFixture $events $true $true

Write-Host 'Food-capacity, low-wealth, and tool-replenishment assertion regressions passed.'
