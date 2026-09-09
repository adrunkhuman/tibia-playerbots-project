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
        @{ event = "navigation_progress"; result = "failed"; reason = "route_unavailable" }
        @{ event = "target_changed"; target_id = 42; target_name = "Playerbot Corpse Blocker";
            reason = "defensive_path_blocker"; route_critical = $true }
        @{ event = "action_result"; action = "defensive_combat"; result = "started"; target_id = 42;
            chase = $false; route_critical = $true; ts = "2026-09-09T16:36:29.485Z" }
        @{ event = "target_changed"; previous_target_id = 42; target_id = $null; reason = "transit_combat_budget" }
        @{ event = "action_result"; action = "defensive_combat"; result = "skipped"; target_id = 42;
            reason = "transit_combat_budget"; ts = "2026-09-09T16:36:34.486Z" }
        @{ event = "navigation_progress"; result = "suspended"; reason = "corpse_route_unchanged"; navigation_failures = 3 }
        @{ event = "navigation_progress"; result = "resumed"; reason = "corpse_retry"; navigation_failures = 3 }
        @{ event = "navigation_progress"; result = "failed"; reason = "route_unavailable" }
        @{ event = "action_result"; action = "loot"; result = "failed"; reason = "corpse_inaccessible";
            target_id = 41; navigation_failures = 6; navigation_suspensions = 1; elapsed_ms = 12150 }
    )
}
Assert-InaccessibleCorpseEvents -Logs (ConvertTo-FixtureLogs (New-InaccessibleCorpseFixture))
foreach ($case in @("old_timeout", "early_release", "late_release", "missing_timestamp", "optional_attacker",
    "unconfirmed_blocker", "chase", "missing_chase", "wrong_release_target", "missing_target_clear",
    "reacquired", "no_suspension", "no_resume", "no_retry", "detour_after_combat", "retry_before_resume",
    "reset_failure_budget", "no_failure_progress", "too_many_failures", "loot_overrun", "duplicate_loot", "terminal")) {
    $events = New-InaccessibleCorpseFixture
    switch ($case) {
        "old_timeout" { $events[4].result = "failed"; $events[4].reason = "combat_timeout" }
        "early_release" { $events[4].ts = "2026-09-09T16:36:34.484Z" }
        "late_release" { $events[4].ts = "2026-09-09T16:36:35.486Z" }
        "missing_timestamp" { $events[4].Remove("ts") }
        "optional_attacker" { $events[1].reason = "defensive_attacker" }
        "unconfirmed_blocker" { $events[1].route_critical = $false }
        "chase" { $events[2].chase = $true }
        "missing_chase" { $events[2].Remove("chase") }
        "wrong_release_target" { $events[4].target_id = 43 }
        "missing_target_clear" { $events = @($events[0..2]) + @($events[4..8]) }
        "reacquired" { $events = @($events[0..6]) + @($events[1], $events[2]) + @($events[7..8]) }
        "no_suspension" { $events = @($events[0..4]) + @($events[6..8]) }
        "no_resume" { $events = @($events[0..5]) + @($events[7..8]) }
        "no_retry" { $events = @($events[0..6]) + @($events[8]) }
        "detour_after_combat" { $events = @($events[1..4]) + @($events[0]) + @($events[5..8]) }
        "retry_before_resume" { $events = @($events[0..5]) + @($events[7], $events[6], $events[8]) }
        "reset_failure_budget" { $events[6].navigation_failures = 0 }
        "no_failure_progress" { $events[8].navigation_failures = 3 }
        "too_many_failures" { $events[8].navigation_failures = 7 }
        "loot_overrun" { $events[8].elapsed_ms = 70001 }
        "duplicate_loot" { $events += $events[8] }
        "terminal" { $events += @{ event = "terminal"; reason = "controlled_player_dead" } }
    }
    Assert-Rejected "Inaccessible corpse $case" {
        Assert-InaccessibleCorpseEvents -Logs (ConvertTo-FixtureLogs $events)
    } "Inaccessible corpse work was not bounded"
}
if ($InaccessibleCorpseLogPath) {
    Assert-InaccessibleCorpseEvents -Logs (Get-Content -Raw -LiteralPath $InaccessibleCorpseLogPath)
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

"Playerbot navigation assertion regression PASS"
