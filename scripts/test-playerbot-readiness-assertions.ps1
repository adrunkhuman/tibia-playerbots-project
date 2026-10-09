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

function New-SpareSpearFixture([string]$Mode, [int]$Level = 8) {
    $target = $Level -eq 8 ? 3 : 7
    $scenario = $Mode -eq 'break' ? 'spear_break' : $Level -eq 16 ? 'spear_restock_scaled' : 'spear_restock'
    $events = @(
        @{ event = 'spear_fixture'; source = 'lua_setup_verifier'; scenario = $scenario; phase = 'start'; level = $Level; spears = ($Mode -eq 'break' ? 2 : 1) }
    )
    if ($Mode -eq 'break') {
        $events += @{ event = 'action_result'; action = 'hunt_cycle'; result = 'started' }
        $events += @{ event = 'action_result'; action = 'equip_readiness'; result = 'success'; item_id = 2389; slot = 6 }
        $events += @{ event = 'hunt_region_outcome'; region_id = 1; reason = 'throwing_weapon_exhausted' }
    } else {
        $events += @{ event = 'combat_readiness'; result = 'recovery'; selected_recovery = 'service'; requirements = @(
            @{ kind = 'throwing_weapon'; ready = $false; count = 1; restock_target = $target }
        ) }
    }
    $events += @{ event = 'action_result'; action = 'buy_throwing_weapons'; result = 'success'; item_id = 2389; count = ($target - 1); carried_before = 100; carried_after = (100 - 10 * ($target - 1)); bank_before = 100; bank_after = 100 }
    $events += @{ event = 'spear_fixture'; source = 'lua_setup_verifier'; scenario = $scenario; phase = 'pass'; level = $Level; spears = $target }
    $events += @{ event = 'action_result'; action = 'hunt_cycle'; result = 'started' }
    return $events
}
function Test-SpareSpearFixture([array]$Events, [string]$Mode, [bool]$Reject, [int]$Level = 8) {
    $logs = ($Events | ForEach-Object {
        $_.component = 'playerbot'; $_.bot = 'Bot One'
        $_ | ConvertTo-Json -Compress
    }) -join "`n"
    try { Assert-SpareSpearEvents -Logs $logs -Mode $Mode -Level $Level }
    catch { if ($Reject) { return }; throw }
    if ($Reject) { throw "Invalid spare-spear $Mode fixture unexpectedly passed." }
}
foreach ($mode in @('restock', 'break')) {
    Test-SpareSpearFixture (New-SpareSpearFixture $mode) $mode $false
    # Reject missing hunt resumption, a short purchase, or missing recovery evidence.
    Test-SpareSpearFixture (New-SpareSpearFixture $mode | Select-Object -SkipLast 1) $mode $true
    foreach ($count in @(1, 6)) {
        $events = @(New-SpareSpearFixture $mode); $events[-3].count = $count
        Test-SpareSpearFixture $events $mode $true
    }
    $events = @(New-SpareSpearFixture $mode); $events[-3].carried_after = 100
    Test-SpareSpearFixture $events $mode $true
    $events = @(New-SpareSpearFixture $mode); $events[-2].spears = 7
    Test-SpareSpearFixture $events $mode $true
    $events = @(New-SpareSpearFixture $mode | Where-Object {
        $_.reason -ne 'throwing_weapon_exhausted' -and $_.event -ne 'combat_readiness'
    })
    Test-SpareSpearFixture $events $mode $true
}
# The spare must be equipped before the hunt ends.
$events = @(New-SpareSpearFixture 'break'); $events[2], $events[3] = $events[3], $events[2]
Test-SpareSpearFixture $events 'break' $true
Test-SpareSpearFixture (New-SpareSpearFixture 'restock' 16) 'restock' $false 16
foreach ($count in @(2, 5, 7)) {
    $events = @(New-SpareSpearFixture 'restock' 16); $events[-3].count = $count
    Test-SpareSpearFixture $events 'restock' $true 16
}
foreach ($field in @('level', 'spears', 'source')) {
    $events = @(New-SpareSpearFixture 'restock' 16); $events[-2].Remove($field)
    Test-SpareSpearFixture $events 'restock' $true 16
}
$events = @(New-SpareSpearFixture 'restock' 16 | Where-Object { $_.phase -ne 'pass' })
Test-SpareSpearFixture $events 'restock' $true 16

function New-ZeroSpearFixture([string]$Mode = 'startup', [int]$Price = 10) {
    $scenario = switch ($Mode) {
        'startup' { 'equipment_buy_spear' }
        'last_break' { 'spear_last_break' }
        'mirrored' { 'spear_mirrored' }
        'unfunded' { 'spear_unfunded' }
    }
    $mirrored = $Mode -eq 'mirrored'
    function New-Evidence([string]$Phase, [int]$Spears, [int]$Money = 39) {
        @{ event = 'spear_fixture'; source = 'lua_setup_verifier'; scenario = $scenario; phase = $Phase;
            vocation = 3; level = 8; spears = $Spears; equipped_spears = $Spears;
            left_item_id = ($mirrored ? 2525 : ($Spears -gt 0 ? 2389 : 0));
            right_item_id = ($mirrored ? ($Spears -gt 0 ? 2389 : 0) : 2525);
            carried_gold = $Money; bank_gold = 0; health = 185; health_max = 185; mana = 35; mana_max = 35;
            free_capacity = 90000; health_potions = 20; mana_potions = 20 }
    }
    $events = @(New-Evidence 'start' ($Mode -eq 'last_break' ? 3 : 0) ($Mode -eq 'unfunded' ? 0 : 39))
    if ($Mode -eq 'last_break') {
        $events += @{ event = 'action_result'; action = 'hunt_cycle'; result = 'started' }
        $events += New-Evidence 'last_spear' 1
        $events += New-Evidence 'broken' 0
        $events += @{ event = 'hunt_region_outcome'; reason = 'throwing_weapon_exhausted' }
    }
    $events += @{ event = 'combat_readiness'; vocation_id = 3; result = 'recovery'; selected_recovery = 'service'; requirements = @(
        @{ name = 'legal_distance_weapon'; ready = $false; left_item_id = ($mirrored ? 2525 : 0); right_item_id = ($mirrored ? 0 : 2525) }
        @{ kind = 'throwing_weapon'; item_id = 2389; ready = $false; count = 0; restock_target = 3 }
    ) }
    if ($Mode -eq 'unfunded') {
        $events += @{ event = 'action_result'; action = 'restock'; result = 'deferred'; reason = 'safety_stock_unmet' }
        $events += @{ event = 'goal_result'; goal = 'service'; result = 'success'; reason = 'service_complete' }
        $events += @{ event = 'summary'; final = $true; state = 'stopped'; level = 8; carried_gold = 0; bank_balance = 0;
            supplies = @(@{ kind = 'throwing_weapon'; item_id = 2389; count = 0 }) }
        $events += @{ event = 'terminal'; reason = 'combat_readiness_throwing_weapon_restock_blocked' }
        return $events
    }
    $events += @{ event = 'action_result'; action = 'buy_throwing_weapons'; result = 'success'; item_id = 2389; count = 3;
        carried_before = 39; carried_after = (39 - 3 * $Price); bank_before = 0; bank_after = 0 }
    $events += @{ event = 'action_result'; action = 'equip_readiness'; result = 'success'; item_id = 2389; slot = ($mirrored ? 5 : 6) }
    $events += New-Evidence 'pass' 3 (39 - 3 * $Price)
    $events += @{ event = 'combat_readiness'; vocation_id = 3; result = 'ready'; requirements = @(
        @{ name = 'legal_distance_weapon'; ready = $true; left_item_id = ($mirrored ? 2525 : 2389); right_item_id = ($mirrored ? 2389 : 2525) }
        @{ kind = 'throwing_weapon'; item_id = 2389; ready = $true; count = 3; restock_target = 3 }
        @{ name = 'armor_loadout'; ready = $true }
        @{ name = 'health_potions'; ready = $true; item_id = 7618; count = 20 }
        @{ name = 'free_capacity'; ready = $true; effective = 90000; minimum = 3000 }
    ) }
    $events += @{ event = 'action_result'; action = 'hunt_cycle'; result = 'started' }
    return $events
}
function Test-ZeroSpearFixture([array]$Events, [string]$Mode, [bool]$Reject) {
    $logs = ($Events | ForEach-Object {
        $_.component = 'playerbot'; $_.bot = 'Bot One'
        $_ | ConvertTo-Json -Depth 8 -Compress
    }) -join "`n"
    # A pass marker alone must never substitute for paid receipts and inventory evidence.
    $logs += "`nPLAYERBOT_GAMEPLAY_TEST EQUIPMENT_BUY_SPEAR_PASS`nPLAYERBOT_GAMEPLAY_TEST SPEAR_LAST_BREAK_PASS"
    try {
        if ($Mode -eq 'startup') { Assert-EquipmentPurchaseEvents -Logs $logs -Spear }
        else { Assert-ZeroSpearRecoveryEvents -Logs $logs -Mode $Mode }
    }
    catch { if ($Reject) { return }; throw }
    if ($Reject) { throw "Invalid zero-spear $Mode fixture unexpectedly passed." }
}
foreach ($mode in @('startup', 'last_break', 'mirrored')) {
    Test-ZeroSpearFixture (New-ZeroSpearFixture $mode) $mode $false
    Test-ZeroSpearFixture (New-ZeroSpearFixture $mode 9) $mode $false
    # The Lua inventory poll may run after purchase but before receipt verification.
    $events = @(New-ZeroSpearFixture $mode)
    $earlyPass = @($events[0..($events.Count - 6)] + $events[-3] + $events[-5] + $events[-4] + $events[-2] + $events[-1])
    Test-ZeroSpearFixture $earlyPass $mode $false
    $events = @(New-ZeroSpearFixture $mode)
    $request = $events[-4].Clone(); $request.result = 'requested'
    Test-ZeroSpearFixture @($events[0..($events.Count - 5)] + $request + $events[($events.Count - 4)..($events.Count - 1)]) $mode $false
    $events[-4].slot = ($mode -eq 'mirrored' ? 6 : 5)
    Test-ZeroSpearFixture $events $mode $true
    # Auto-equipping on purchase is legal; the verified hand/ready evidence still applies.
    Test-ZeroSpearFixture @(New-ZeroSpearFixture $mode | Where-Object { $_.action -ne 'equip_readiness' }) $mode $false
    # Normal goal selection can equip the purchased stack as a carried objective.
    $events = @(New-ZeroSpearFixture $mode); $events[-4].action = 'equip_equipment'
    $selection = @{ event = 'strategy_selection'; goal = 'buy_equipment'; acquisition = 'carried'; item_id = 2389 }
    $carried = @($events[0..($events.Count - 5)] + $selection + $events[($events.Count - 4)..($events.Count - 1)])
    Test-ZeroSpearFixture $carried $mode $false
    $selection.acquisition = 'purchase'
    Test-ZeroSpearFixture $carried $mode $true
    $baseline = @(New-ZeroSpearFixture $mode)
    foreach ($i in 0..($baseline.Count - 1)) {
        if ($baseline[$i].action -eq 'equip_readiness') { continue }
        Test-ZeroSpearFixture @($baseline | Select-Object -SkipIndex $i) $mode $true
    }
    foreach ($field in @('source', 'vocation', 'level', 'spears', 'equipped_spears', 'carried_gold', 'bank_gold',
        'left_item_id', 'right_item_id', 'health', 'health_max', 'mana', 'mana_max', 'free_capacity', 'health_potions', 'mana_potions')) {
        $events = @(New-ZeroSpearFixture $mode); $events[0].Remove($field)
        Test-ZeroSpearFixture $events $mode $true
    }
    foreach ($field in @('source', 'vocation', 'level', 'spears', 'equipped_spears', 'carried_gold', 'bank_gold', 'left_item_id', 'right_item_id')) {
        $events = @(New-ZeroSpearFixture $mode); $events[-3].Remove($field)
        Test-ZeroSpearFixture $events $mode $true
    }
    foreach ($field in @('item_id', 'count', 'carried_before', 'carried_after', 'bank_before', 'bank_after', 'event', 'result')) {
        $events = @(New-ZeroSpearFixture $mode); $events[-5].Remove($field)
        Test-ZeroSpearFixture $events $mode $true
    }
    foreach ($price in @(0, 1, 8, 11)) { Test-ZeroSpearFixture (New-ZeroSpearFixture $mode $price) $mode $true }
    foreach ($count in @(1, 2, 7)) {
        $events = @(New-ZeroSpearFixture $mode); $events[-5].count = $count
        Test-ZeroSpearFixture $events $mode $true
    }
    foreach ($cash in @(100, 139)) {
        $events = @(New-ZeroSpearFixture $mode); $events[0].carried_gold = $cash; $events[-5].carried_before = $cash
        Test-ZeroSpearFixture $events $mode $true
    }
    foreach ($recovery in @('acquire_weapon', 'equip_carried')) {
        $events = @(New-ZeroSpearFixture $mode); $events[-6].selected_recovery = $recovery
        Test-ZeroSpearFixture $events $mode $true
    }
    foreach ($field in @('kind', 'item_id', 'ready', 'count', 'restock_target')) {
        $events = @(New-ZeroSpearFixture $mode); $events[-6].requirements[1].Remove($field)
        Test-ZeroSpearFixture $events $mode $true
    }
    $events = @(New-ZeroSpearFixture $mode); $events[-2].requirements[2].ready = $false
    Test-ZeroSpearFixture $events $mode $true
    $events = @(New-ZeroSpearFixture $mode); $events[-1], $events[-5] = $events[-5], $events[-1]
    Test-ZeroSpearFixture $events $mode $true
    foreach ($extra in @(@{ event = 'terminal'; reason = 'missing_legal_distance_weapon' },
        @{ event = 'action_result'; action = 'buy_equipment'; result = 'success'; item_id = 2389 },
        @{ event = 'goal_selection'; to_goal = 'buy_equipment' },
        @{ event = 'action_result'; action = 'buy_meat'; result = 'success' },
        @{ event = 'action_result'; action = 'claim_reward'; result = 'success' })) {
        Test-ZeroSpearFixture (@(New-ZeroSpearFixture $mode) + $extra) $mode $true
    }
}
# Distinguish last-spear loss in an active hunt from a pre-hunt loss or a backpack spare.
foreach ($phase in @(1, 2, 3, 4)) {
    $events = @(New-ZeroSpearFixture 'last_break'); $events[0], $events[$phase] = $events[$phase], $events[0]
    Test-ZeroSpearFixture $events 'last_break' $true
}
foreach ($index in @(2, 3)) {
    $events = @(New-ZeroSpearFixture 'last_break'); $events[$index].spears++
    Test-ZeroSpearFixture $events 'last_break' $true
}
$events = @(New-ZeroSpearFixture 'last_break'); $events[2], $events[3] = $events[3], $events[2]
Test-ZeroSpearFixture $events 'last_break' $true
$events = @(New-ZeroSpearFixture 'last_break'); $events[1], $events[2] = $events[2], $events[1]
Test-ZeroSpearFixture $events 'last_break' $true
Test-ZeroSpearFixture @() 'startup' $true
Test-ZeroSpearFixture @() 'last_break' $true

Test-ZeroSpearFixture (New-ZeroSpearFixture 'unfunded') 'unfunded' $false
$baseline = @(New-ZeroSpearFixture 'unfunded')
foreach ($i in 0..($baseline.Count - 1)) {
    Test-ZeroSpearFixture @($baseline | Select-Object -SkipIndex $i) 'unfunded' $true
}
foreach ($phase in @('start')) {
    foreach ($field in @('source', 'scenario', 'vocation', 'level', 'spears', 'equipped_spears', 'carried_gold', 'bank_gold', 'left_item_id', 'right_item_id')) {
        $events = @(New-ZeroSpearFixture 'unfunded')
        ($events | Where-Object { $_.phase -eq $phase }).Remove($field)
        Test-ZeroSpearFixture $events 'unfunded' $true
    }
    foreach ($field in @('spears', 'equipped_spears', 'carried_gold', 'bank_gold')) {
        $events = @(New-ZeroSpearFixture 'unfunded')
        ($events | Where-Object { $_.phase -eq $phase })[$field] = 1
        Test-ZeroSpearFixture $events 'unfunded' $true
    }
}
foreach ($field in @('health', 'health_max', 'mana', 'mana_max', 'free_capacity', 'health_potions', 'mana_potions')) {
    $events = @(New-ZeroSpearFixture 'unfunded'); $events[0].Remove($field)
    Test-ZeroSpearFixture $events 'unfunded' $true
}
foreach ($reason in @('combat_readiness_missing_legal_distance_weapon', 'hunt_scope_exhausted', '')) {
    $events = @(New-ZeroSpearFixture 'unfunded'); $events[-1].reason = $reason
    Test-ZeroSpearFixture $events 'unfunded' $true
}
foreach ($index in @(2, 3, 4)) {
    $events = @(New-ZeroSpearFixture 'unfunded'); $events[$index].Remove('event')
    Test-ZeroSpearFixture $events 'unfunded' $true
}
foreach ($field in @('kind', 'item_id', 'ready', 'count', 'restock_target')) {
    $events = @(New-ZeroSpearFixture 'unfunded'); $events[1].requirements[1].Remove($field)
    Test-ZeroSpearFixture $events 'unfunded' $true
}
$events = @(New-ZeroSpearFixture 'unfunded'); $events[1].terminal_reason = 'missing_legal_distance_weapon'
Test-ZeroSpearFixture $events 'unfunded' $true
foreach ($extra in @(
    @{ event = 'terminal'; reason = 'combat_readiness_throwing_weapon_restock_blocked' },
    @{ event = 'goal_result'; goal = 'service'; result = 'success'; reason = 'service_complete' },
    @{ event = 'action_result'; action = 'restock'; result = 'deferred'; reason = 'safety_stock_unmet' },
    @{ event = 'action_result'; action = 'hunt_cycle'; result = 'started' },
    @{ event = 'goal_selection'; to_goal = 'buy_equipment' },
    @{ event = 'action_result'; action = 'buy_equipment'; result = 'success'; item_id = 2389 },
    @{ event = 'action_result'; action = 'buy_throwing_weapons'; result = 'success'; item_id = 2389; count = 3 },
    @{ event = 'action_result'; action = 'claim_reward'; result = 'success' },
    @{ event = 'action_result'; action = 'sell'; result = 'success' },
    @{ event = 'action_attempt'; action = 'walk' },
    @{ event = 'scheduler'; status = 'scheduled' }
)) {
    Test-ZeroSpearFixture (@(New-ZeroSpearFixture 'unfunded') + $extra) 'unfunded' $true
}
# Pre-terminal hunting is forbidden too, and service must complete before terminal.
$events = @(New-ZeroSpearFixture 'unfunded')
Test-ZeroSpearFixture @($events[0..3] + @{ event = 'action_result'; action = 'hunt_cycle'; result = 'started' } + $events[4..5]) 'unfunded' $true
$events = @(New-ZeroSpearFixture 'unfunded'); $events[3], $events[5] = $events[5], $events[3]
Test-ZeroSpearFixture $events 'unfunded' $true
$events = @(New-ZeroSpearFixture 'mirrored'); $events[-2].requirements[0].left_item_id = 2389; $events[-2].requirements[0].right_item_id = 2525
Test-ZeroSpearFixture $events 'mirrored' $true
Test-ZeroSpearFixture @() 'mirrored' $true
Test-ZeroSpearFixture @() 'unfunded' $true

Write-Host 'Food-capacity, low-wealth, tool-replenishment, spare-spear, mirrored and unfunded zero-spear assertion regressions passed.'
