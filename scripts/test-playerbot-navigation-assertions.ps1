#Requires -Version 7.0

param([string]$DangerRetreatLogPath, [string]$InaccessibleCorpseLogPath)

$ErrorActionPreference = "Stop"

. $PSScriptRoot/playerbot-gameplay/log-parsing.ps1
. $PSScriptRoot/playerbot-gameplay/assertions-navigation.ps1

function ConvertTo-FixtureLogs {
    param([hashtable[]]$Events)

    return ($Events | ForEach-Object {
        $_.component = "playerbot"
        $_.bot = "Bot One"
        $_ | ConvertTo-Json -Compress -Depth 5
    }) -join "`n"
}

function Assert-Rejected {
    param([string]$Name, [scriptblock]$Body, [string]$ExpectedMessage)

    try {
        & $Body
    }
    catch {
        if ($_.Exception.Message -notlike "$ExpectedMessage*") {
            throw "$Name failed for an unexpected reason: $($_.Exception.Message)"
        }
        return
    }
    throw "$Name unexpectedly passed."
}

$transitMarkers = "PLAYERBOT_GAMEPLAY_TEST TRANSIT_RETURN_START 100 100 7`nPLAYERBOT_GAMEPLAY_TEST TRANSIT_RETURN_STATE_PASS`n"
$transitStart = @{ event = "action_result"; action = "return"; result = "started"; reason = "startup";
    position = @{ x = 100; y = 100; z = 7 } }
$transitEnd = @{ event = "objective_transition"; to = "deposit_loot";
    position = @{ x = 97; y = 100; z = 7 } }
Assert-TransitReturnEvents -Logs ($transitMarkers + (ConvertTo-FixtureLogs @($transitStart, $transitEnd)))
Assert-Rejected "Crowd caused optional combat" {
    Assert-TransitReturnEvents -Logs ($transitMarkers + (ConvertTo-FixtureLogs @($transitStart,
        @{ event = "target_changed"; target_id = 42; reason = "defensive_attacker"; route_critical = $false }, $transitEnd)))
} "Transit return failed: optional combat"
Assert-Rejected "Crowd caused unjustified blocker combat" {
    Assert-TransitReturnEvents -Logs ($transitMarkers + (ConvertTo-FixtureLogs @($transitStart,
        @{ event = "target_changed"; target_id = 42; reason = "defensive_path_blocker"; route_critical = $true }, $transitEnd)))
} "Transit return failed: optional combat"
Assert-Rejected "Crowd prevented movement" {
    Assert-TransitReturnEvents -Logs ($transitMarkers + (ConvertTo-FixtureLogs @($transitStart,
        @{ event = "objective_transition"; to = "deposit_loot"; position = $transitStart.position })))
} "Transit return failed: no ordinary return movement"
Assert-Rejected "No live crowd verification" {
    Assert-TransitReturnEvents -Logs (ConvertTo-FixtureLogs @($transitStart, $transitEnd))
} "Transit return failed: missing crowd fixture"

$dangerStart = @{ event = "objective_transition"; to = "return_to_depot"; reason = "hunt_region_observed_danger";
    position = @{ x = 100; y = 100; z = 7 } }
$dangerEnd = @{ event = "objective_transition"; to = "deposit_loot";
    position = @{ x = 105; y = 100; z = 7 } }
$blocker = @{ event = "target_changed"; target_id = 42; reason = "defensive_path_blocker"; route_critical = $true }
Assert-DangerRetreatEvents -Logs (ConvertTo-FixtureLogs @($dangerStart, $dangerEnd))
Assert-DangerRetreatEvents -Logs (ConvertTo-FixtureLogs @($dangerStart, $blocker, $dangerEnd))
Assert-Rejected "Adjacent attacker after danger" {
    Assert-DangerRetreatEvents -Logs (ConvertTo-FixtureLogs @($dangerStart,
        @{ event = "target_changed"; target_id = 42; reason = "defensive_attacker"; route_critical = $false }, $dangerEnd))
} "Danger retreat failed: optional combat"
Assert-Rejected "Spent blocker reacquired" {
    Assert-DangerRetreatEvents -Logs (ConvertTo-FixtureLogs @($dangerStart, $blocker, $blocker, $dangerEnd))
} "Danger retreat failed: blocker reacquired"
Assert-Rejected "Incomplete retreat" {
    Assert-DangerRetreatEvents -Logs (ConvertTo-FixtureLogs @($dangerStart))
} "Danger retreat failed: no complete"
$online = @{ event = "lifecycle"; status = "online"; recovered = $true; recovery_count = 1 }
Assert-DangerRetreatEvents -Logs (ConvertTo-FixtureLogs @($dangerStart, $online, $dangerEnd))
foreach ($failure in @(
    @{ event = "lifecycle"; status = "dead"; health = 0; objective = "return_to_depot";
        killer_name = "Valkyrie"; killer_type = "monster"; position = $dangerStart.position }
    @{ event = "lifecycle"; status = "removed"; position = $dangerStart.position }
    @{ event = "lifecycle"; status = "recovery_abandoned"; reason = "death_loop_limit"; death_count = 3 }
    @{ event = "terminal"; reason = "controlled_player_dead"; position = $dangerStart.position }
)) {
    Assert-Rejected "Retreat failure $($failure.event)/$($failure.status) before later arrival" {
        Assert-DangerRetreatEvents -Logs (ConvertTo-FixtureLogs @($dangerStart, $failure, $online, $dangerEnd))
    } "Danger retreat failed: death or terminal"
}
Assert-Rejected "Stationary retreat" {
    Assert-DangerRetreatEvents -Logs (ConvertTo-FixtureLogs @($dangerStart,
        @{ event = "objective_transition"; to = "deposit_loot"; position = $dangerStart.position }))
} "Danger retreat failed: no movement"
if ($DangerRetreatLogPath) {
    Assert-DangerRetreatEvents -Logs (Get-Content -Raw -LiteralPath $DangerRetreatLogPath)
}

function New-InaccessibleCorpseFixture {
    return @(
        @{ event = "action_result"; action = "loot"; result = "failed"; reason = "corpse_inaccessible";
            target_id = 41; navigation_failures = 6; navigation_suspensions = 1; elapsed_ms = 5100 }
    )
}
Assert-InaccessibleCorpseEvents -Logs (ConvertTo-FixtureLogs (New-InaccessibleCorpseFixture))
foreach ($case in @("timeout_deadline", "combat_timeout", "wrong_failure_bound", "wrong_suspension_bound", "duplicate_loot", "terminal")) {
    $events = @(New-InaccessibleCorpseFixture)
    switch ($case) {
        "timeout_deadline" { $events[0].elapsed_ms = 20000 }
        "combat_timeout" {
            $events = @(
                @{ event = "action_result"; action = "defensive_combat"; result = "failed";
                    reason = "combat_timeout"; target_id = 42 }
            ) + $events
        }
        "wrong_failure_bound" { $events[0].navigation_failures = 5 }
        "wrong_suspension_bound" { $events[0].navigation_suspensions = 0 }
        "duplicate_loot" { $events += $events[0] }
        "terminal" { $events += @{ event = "terminal"; reason = "controlled_player_dead" } }
    }
    Assert-Rejected "Inaccessible corpse $case" {
        Assert-InaccessibleCorpseEvents -Logs (ConvertTo-FixtureLogs $events)
    } "Inaccessible corpse work was not bounded"
}

if ($InaccessibleCorpseLogPath) {
    Assert-InaccessibleCorpseEvents -Logs (Get-Content -Raw -LiteralPath $InaccessibleCorpseLogPath)
}

function New-CorpseDetourFixture {
    return @(
        @{ event = "action_result"; action = "plan"; result = "success"; danger_aware = $true; steps = 3;
            destination = @{ x = 32105; y = 32194; z = 8 } }
        @{ event = "action_result"; action = "loot"; result = "success"; item_id = 2148; count = 1 }
    )
}
$detourLogs = "PLAYERBOT_GAMEPLAY_TEST CORPSE_INACCESSIBLE_DISPLACED`n" +
    (ConvertTo-FixtureLogs (New-CorpseDetourFixture))
# The current controller does not emit the removed hostile_detour telemetry.
Assert-CorpseDetourEvents -Logs $detourLogs
foreach ($case in @("missing_displacement", "no_route", "defensive_combat", "terminal")) {
    $events = @(New-CorpseDetourFixture)
    $logs = $detourLogs
    switch ($case) {
        "missing_displacement" { $logs = ConvertTo-FixtureLogs $events }
        "no_route" { $events = @($events | Where-Object action -ne "plan"); $logs = "PLAYERBOT_GAMEPLAY_TEST CORPSE_INACCESSIBLE_DISPLACED`n" + (ConvertTo-FixtureLogs $events) }
        "defensive_combat" { $events += @{ event = "action_result"; action = "defensive_combat"; result = "started" }; $logs = "PLAYERBOT_GAMEPLAY_TEST CORPSE_INACCESSIBLE_DISPLACED`n" + (ConvertTo-FixtureLogs $events) }
        "terminal" { $events += @{ event = "terminal"; reason = "controlled_player_dead" }; $logs = "PLAYERBOT_GAMEPLAY_TEST CORPSE_INACCESSIBLE_DISPLACED`n" + (ConvertTo-FixtureLogs $events) }
    }
    Assert-Rejected "Corpse detour $case" {
        Assert-CorpseDetourEvents -Logs $logs
    } "The displaced corpse was not reached through a safe route"
}

function New-DepotRiskRouteFixture {
    return @(
        @{ event = "depot_risk_fallback_contract"; safe_precedence = $true; retained_across_turns = $true;
            ranked_fallback = $true; requested_revalidation = $true; failed_revalidation_rejected = $true }
        @{ event = "action_result"; action = "depot_discover"; result = "success"; risk_fallback = $false;
            unsafe_routes = 34; route_steps = 577; danger_cost = 323; maximum_health_loss_per_second = 0.0137222 }
    )
}
$depotRiskLogs = "PLAYERBOT_GAMEPLAY_TEST DEPOT_RISK_FALLBACK_PASS`n" +
    (ConvertTo-FixtureLogs (New-DepotRiskRouteFixture))
Assert-DepotRiskRouteEvents -Logs $depotRiskLogs
foreach ($case in @("fallback_selected", "no_unsafe_rejections", "unsafe_safe_route", "terminal")) {
    $events = @(New-DepotRiskRouteFixture)
    switch ($case) {
        "fallback_selected" { $events[1].risk_fallback = $true }
        "no_unsafe_rejections" { $events[1].unsafe_routes = 0 }
        "unsafe_safe_route" { $events[1].danger_cost = 501 }
        "terminal" { $events += @{ event = "terminal"; reason = "depot_unavailable" } }
    }
    Assert-Rejected "Depot risk route $case" {
        Assert-DepotRiskRouteEvents -Logs ("PLAYERBOT_GAMEPLAY_TEST DEPOT_RISK_FALLBACK_PASS`n" + (ConvertTo-FixtureLogs $events))
    } "The depot risk route did not reject unsafe candidates"
}

function New-MainlandFixture {
    return @(
        @{ event = "action_result"; action = "hunt_cycle"; result = "started"; cycle = 1 }
        @{ event = "action_result"; action = "hunt_cycle"; result = "started"; cycle = 2 }
        @{ event = "action_result"; action = "hunt_cycle"; result = "started"; cycle = 3 }
        @{ event = "action_result"; action = "deposit"; result = "complete"; depot_id = 2 }
        @{ event = "action_result"; action = "deposit"; result = "complete"; depot_id = 2 }
        @{ event = "action_result"; action = "depot_discover"; result = "success"; depot_id = 2
            locker_item_id = 2591; locker = @{ x = 32352; y = 32225; z = 7 }
            approach = @{ x = 32352; y = 32226; z = 7 } }
        @{ event = "hunt_region_selection"; result = "selected"; atlas_site_id = 1; atlas_variant_id = 1; atlas_spawns = 2 }
        @{ event = "action_result"; action = "deposit"; result = "success"; item_id = 2826; verified = 1 }
    )
}

foreach ($lockerId in @(2589, 2590, 2591, 2592)) {
    $events = New-MainlandFixture
    $discovery = $events | Where-Object action -eq "depot_discover"
    $discovery.locker_item_id = $lockerId
    Assert-MainlandLoopEvents -Logs (ConvertTo-FixtureLogs $events)
}
foreach ($lockerId in @(2588, 2593)) {
    $events = New-MainlandFixture
    $discovery = $events | Where-Object action -eq "depot_discover"
    $discovery.locker_item_id = $lockerId
    Assert-Rejected "Invalid locker $lockerId" {
        Assert-MainlandLoopEvents -Logs (ConvertTo-FixtureLogs $events)
    } "Mainland loop failed."
}
foreach ($axis in @("x", "y", "z")) {
    $events = New-MainlandFixture
    $discovery = $events | Where-Object action -eq "depot_discover"
    $discovery.approach[$axis] = $discovery.locker[$axis] + 2
    Assert-Rejected "Invalid approach $axis" {
        Assert-MainlandLoopEvents -Logs (ConvertTo-FixtureLogs $events)
    } "Mainland loop failed."
}

function New-SlottedLootFixture {
    return @(
        @{ event = "action_result"; action = "deposit"; result = "success"; item_id = 2398
            verified = 1; source_slot = 10; disposition = "deposit"; provider_available = $false }
        @{ event = "sell_loot_plan"; action = "sell_loot_plan"; result = "candidate"
            reason = "profitable_trip_validated"; source_depot_id = 2; manifest_batches = 1
            expected_revenue = 30; round_trip_time_cost = 5; utility = 25 }
        @{ event = "action_result"; action = "sell"; result = "success"; item_id = 2398
            count = 1; carried_before = 0; bank_before = 100; carried_after = 30; bank_after = 100 }
    )
}

Assert-SlottedLootEvents -Logs (ConvertTo-FixtureLogs (New-SlottedLootFixture)) -SellerAvailable
foreach ($change in @(
    @{ Field = "source_depot_id"; Value = 1 }
    @{ Field = "manifest_batches"; Value = 0 }
    @{ Field = "utility"; Value = 0 }
    @{ Field = "utility"; Value = -1 }
    @{ Field = "result"; Value = "deferred" }
)) {
    $events = New-SlottedLootFixture
    $plan = $events | Where-Object event -eq "sell_loot_plan"
    $plan[$change.Field] = $change.Value
    Assert-Rejected "Invalid plan $($change.Field)=$($change.Value)" {
        Assert-SlottedLootEvents -Logs (ConvertTo-FixtureLogs $events) -SellerAvailable
    } "The slotted loot sale lacked a candidate plan"
}
foreach ($field in @("source_depot_id", "manifest_batches", "utility", "plan")) {
    $events = New-SlottedLootFixture
    if ($field -eq "plan") {
        $events = @($events | Where-Object event -ne "sell_loot_plan")
    } else {
        $plan = $events | Where-Object event -eq "sell_loot_plan"
        $plan.Remove($field)
    }
    Assert-Rejected "Missing $field" {
        Assert-SlottedLootEvents -Logs (ConvertTo-FixtureLogs $events) -SellerAvailable
    } "The slotted loot sale lacked a candidate plan"
}
foreach ($case in @("missing_sale", "wrong_item", "no_money_gain", "unverified_deposit", "protected_item")) {
    $events = New-SlottedLootFixture
    $sale = $events | Where-Object action -eq "sell"
    $expectedMessage = "Slotted loot did not use the expected local disposition."
    switch ($case) {
        "missing_sale" { $events = @($events | Where-Object action -ne "sell") }
        "wrong_item" { $sale.item_id = 2399 }
        "no_money_gain" { $sale.carried_after = 0 }
        "unverified_deposit" { ($events | Where-Object action -eq "deposit").verified = 0 }
        "protected_item" {
            $events += @{ event = "action_result"; action = "item_disposition"; item_id = 2463 }
            $expectedMessage = "Slotted disposition did not preserve protected state or bounded service."
        }
    }
    Assert-Rejected $case {
        Assert-SlottedLootEvents -Logs (ConvertTo-FixtureLogs $events) -SellerAvailable
    } $expectedMessage
}

$svargrondReached = @{ event = "local_route_recovery"; result = "reached";
    position = @{ x = 32232; y = 31076; z = 6 } }
Assert-SvargrondLocalRouteRecoveryEvents -Logs (ConvertTo-FixtureLogs @($svargrondReached))
foreach ($paidEvent in @(
    @{ event = "npc_travel"; result = "success" },
    @{ event = "action_result"; action = "plan"; result = "success"; fare = 290;
       destination = @{ x = 32232; y = 31076; z = 6 } }
)) {
    Assert-Rejected "Paid route cannot prove local recovery" {
        Assert-SvargrondLocalRouteRecoveryEvents -Logs (ConvertTo-FixtureLogs @($paidEvent, $svargrondReached))
    } "Svargrond local route recovery failed."
}
foreach ($reason in @("fare_breaks_restock_reserve", "route_danger_above_tolerance")) {
    $rejections = @(1..20 | ForEach-Object {
        @{ event = "navigation_progress"; result = "skipped"; reason = $reason;
           fixed_target_route_failures = $_; sequence = $_ }
    })
    $terminal = @{ event = "terminal"; reason = "navigation_route_unavailable"; sequence = 21 }
    Assert-NavigationPreflightRejectionEvents -Logs (ConvertTo-FixtureLogs ($rejections + @($terminal))) -Reason $reason
    Assert-Rejected "Post-terminal navigation event" {
        Assert-NavigationPreflightRejectionEvents -Reason $reason -Logs (ConvertTo-FixtureLogs (
            $rejections + @($terminal, @{ event = "summary"; sequence = 22 })))
    } "Navigation $reason preflight rejection was not bounded."
    $rejections[-1].fixed_target_route_failures = 1
    Assert-Rejected "Rejection cleared failure memory" {
        Assert-NavigationPreflightRejectionEvents -Reason $reason -Logs (ConvertTo-FixtureLogs ($rejections + @($terminal)))
    } "Navigation $reason preflight rejection was not bounded."
}

function New-MutablePortalFixture {
    param([bool]$ShovelAvailable)

    return @(
        @{ event = "shovel_passages_contract"; item_closed_lookup = $true; closed_requires_shovel = $true;
           shovel_available = $ShovelAvailable; closed_resolves_use = $ShovelAvailable;
           item_open_lookup = $true; open_without_shovel = $true; normal_open_semantic = $true;
           open_resolves_move = $true; blocked_rejected = $true; invalid_rejected = $true }
        @{ event = "action_result"; action = "hunt_waypoint"; result = "reached";
           position = @{ x = 32181; y = 31794; z = 8 } }
    )
}
Assert-MutablePortalRouteEvents -Logs (ConvertTo-FixtureLogs (New-MutablePortalFixture $true))
Assert-MutablePortalRouteEvents -Logs (ConvertTo-FixtureLogs (New-MutablePortalFixture $false))
$invalidMutablePortal = New-MutablePortalFixture $false
$invalidMutablePortal[0].closed_resolves_use = $true
Assert-Rejected "Closed shovel passage ignored missing tool" {
    Assert-MutablePortalRouteEvents -Logs (ConvertTo-FixtureLogs $invalidMutablePortal)
} "Mutable portal route failed."

"Playerbot navigation assertion regression PASS"
