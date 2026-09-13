#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
. $PSScriptRoot/playerbot-gameplay/log-parsing.ps1
. $PSScriptRoot/playerbot-gameplay/assertions-progression.ps1
. $PSScriptRoot/playerbot-gameplay/assertions-spells.ps1

function Test-Evidence([array]$Events, [scriptblock]$Assertion, [bool]$Reject = $false, [string]$Marker = '') {
    $logs = ($Events | ForEach-Object {
        $_.component = 'playerbot'; $_.bot = 'Bot One'
        $_ | ConvertTo-Json -Compress
    }) -join "`n"
    try { & $Assertion ($logs + "`n" + $Marker) }
    catch { if ($Reject) { return }; throw }
    if ($Reject) { throw 'Invalid supply evidence unexpectedly passed.' }
}
$supply = @(
    @{ event = 'supply_budget_fixture'; source = 'synthetic_runtime_candidates'; potions = 2; duration_seconds = 900; selected_variant = 1; budget_fits = $true; expected_potions = 1; scored_candidates = 2; selection_rule = 'supply_budget_then_xp' }
    @{ event = 'supply_budget_fixture'; source = 'synthetic_runtime_candidates'; potions = 10; duration_seconds = 900; selected_variant = 2; budget_fits = $true; expected_potions = 8; scored_candidates = 2; selection_rule = 'supply_budget_then_xp' }
    @{ event = 'supply_budget_fixture'; source = 'synthetic_runtime_candidates'; potions = 2; duration_seconds = 1500; selected_variant = 1; budget_fits = $false; expected_potions = 2; scored_candidates = 2; easy_expected_potions = 2; costly_expected_potions = 12; easy_budget_fits = $false; costly_budget_fits = $false; selection_rule = 'lowest_potion_consumption_then_xp' }
)
$checkSupply = { param($logs) Assert-SupplyBudgetEvents -Logs $logs }
Test-Evidence $supply $checkSupply
$supply[0].selected_variant = 2
Test-Evidence $supply $checkSupply $true
$supply[0].selected_variant = 1
$supply[1].budget_fits = $false
Test-Evidence $supply $checkSupply $true
$supply[1].budget_fits = $true
# Neither the old max-XP fallback nor treating this as a fitting hunt may pass.
foreach ($mutation in @(
    @{ selected_variant = 2; expected_potions = 12 }
    @{ selected_variant = 0 }
    @{ selection_rule = 'safe_xp_budget_fallback' }
    @{ easy_budget_fits = $true }
    @{ budget_fits = $true }
    @{ costly_expected_potions = 8 }
    @{ duration_seconds = 900 }
)) {
    $original = $supply[2]
    $supply[2] = $original.Clone()
    foreach ($key in $mutation.Keys) { $supply[2][$key] = $mutation[$key] }
    Test-Evidence $supply $checkSupply $true
    $supply[2] = $original
}
Test-Evidence $supply[0..1] $checkSupply $true
Test-Evidence $supply $checkSupply

$spell = @(
    @{ event = 'spell_candidate'; spell = 'Light Healing'; price = 170; reserve = 100; result = 'feasible' }
    @{ event = 'goal_candidate'; goal = 'learn_spell'; feasible = $true; reason = 'priority_recovery_spell' }
    @{ event = 'goal_selection'; to_goal = 'learn_spell' }
    @{ event = 'action_result'; action = 'learn_spell'; result = 'success'; spell = 'Light Healing'; money_before = 270; money_after = 100 }
)
$checkSpell = { param($logs) Assert-LowSupplySpellTrainingEvents -Logs $logs }
$marker = 'PLAYERBOT_GAMEPLAY_TEST SPELL_TRAINING_LOW_SUPPLIES_PASS'
Test-Evidence $spell $checkSpell $false $marker
Test-Evidence $spell $checkSpell $true
$spell[3].money_after = 99
Test-Evidence $spell $checkSpell $true $marker
$spell[3].money_after = 100
Test-Evidence (@(@{ event = 'goal_selection'; to_goal = 'buy_equipment' }) + $spell) $checkSpell $true $marker
$rejected = @(@{ event = 'spell_candidate'; spell = 'Light Healing'; price = 170; reserve = 100; result = 'rejected'; reason = 'unaffordable_after_reserves' })
$checkRejected = { param($logs) Assert-LowSupplySpellTrainingEvents -Logs $logs -Unaffordable }
Test-Evidence $rejected $checkRejected
Test-Evidence ($rejected + $spell[3]) $checkRejected $true

# The complete scored-candidate stream can be much larger than the route queue.
# Only candidates marked route_validated consume the selected scan's route budget.
$huntPlanning = @(
    @{ event = 'hunt_region_scan'; phase = 'planning_started'; cache = 'hit'; selection_strategy = 'atlas_topology_selection'
       snapshot_time_us = 0; clustering_time_us = 0; candidate_count = 90; scored_candidate_count = 0
       route_candidate_count = 0; transport_offer_count = 12; transport_arrival_count = 1 }
    @{ event = 'hunt_region_scan'; phase = 'cancelled' }
    @{ event = 'hunt_region_scan'; phase = 'planning_started'; cache = 'hit'; selection_strategy = 'atlas_topology_selection'
       snapshot_time_us = 0; clustering_time_us = 0; candidate_count = 90; scored_candidate_count = 0
       route_candidate_count = 0; transport_offer_count = 12; transport_arrival_count = 1 }
    @{ event = 'hunt_region_scan'; phase = 'stale_revision' }
    @{ event = 'hunt_region_scan'; phase = 'planning_started'; cache = 'build'; selection_strategy = 'atlas_topology_selection'
       snapshot_time_us = 2; clustering_time_us = 3; candidate_count = 90; scored_candidate_count = 0
       route_candidate_count = 0; transport_offer_count = 12; transport_arrival_count = 1 }
    @{ event = 'hunt_region_scan'; phase = 'transport_yield'; cache = 'build'; selection_strategy = 'atlas_topology_selection' }
    @{ event = 'hunt_region_scan'; phase = 'scoring_yield'; cache = 'build'; selection_strategy = 'atlas_topology_selection' }
    @{ event = 'hunt_region_scan'; phase = 'scored'; cache = 'build'; selection_strategy = 'atlas_topology_selection'
       snapshot_time_us = 2; clustering_time_us = 3; candidate_count = 90; scored_candidate_count = 90
       suitable_candidate_count = 10; route_candidate_count = 10 }
)
for ($index = 1; $index -le 80; $index++) {
    $huntPlanning += @{
        event = 'hunt_region_candidate'; region_id = 100 + $index; atlas_variant_id = 100 + $index
        suitable = $false; reachable = $true; route_validated = $false; supply_budget_fits = $true
        supply_expected_potions = 1; score = $index; topology_reachable = $true
        topology_travel_steps = 10; route_danger_cost = 0; center = @{ x = 32500 + $index; y = 31800; z = 7 }
    }
}
for ($index = 1; $index -le 10; $index++) {
    $huntPlanning += @{
        event = 'hunt_region_candidate'; region_id = $index; atlas_variant_id = $index
        suitable = $true; reachable = $true; route_validated = $true; supply_budget_fits = $true
        supply_expected_potions = 1; score = 101 - $index; topology_reachable = $true
        topology_travel_steps = 10; route_danger_cost = 0; center = @{ x = 32400 + $index; y = 31800; z = 7 }
    }
}
$huntPlanning += @(
    @{ event = 'hunt_region_selection'; result = 'selected'; region_id = 1; selection_rule = 'supply_budget_then_xp' }
    @{ event = 'hunt_supply_reserve'; source = 'selected_return_route'; route_danger_cost = 100
       maximum_health = 200; health_loss_cost = 1000; minimum_potion_healing = 125
       return_threshold = 1; restock_target = 10 }
    @{ event = 'hunt_region_scan'; phase = 'selected'; cache = 'build'; selection_strategy = 'atlas_topology_selection'
       topology_time_us = 1; decision_latency_us = 1; candidate_count = 90; route_candidate_count = 10
       route_candidate_policy = 'all_cheap_viable_ranked'; transport_offer_count = 12; transport_arrival_count = 3 }
)
$checkHuntPlanning = { param($logs) Assert-HuntRegionPlanningEvents -Logs $logs }
Test-Evidence $huntPlanning $checkHuntPlanning
$originalSelectedScan = $huntPlanning[$huntPlanning.Count - 1]
$huntPlanning[$huntPlanning.Count - 1] = $originalSelectedScan.Clone()
$huntPlanning[$huntPlanning.Count - 1].route_candidate_count = 8
Test-Evidence $huntPlanning $checkHuntPlanning $true
$huntPlanning[$huntPlanning.Count - 1] = $originalSelectedScan

$unvalidatedCandidate = @($huntPlanning | Where-Object { $_.event -eq 'hunt_region_candidate' -and -not $_.route_validated })[0]
$unvalidatedCandidate.route_validated = $true
Test-Evidence $huntPlanning $checkHuntPlanning $true
$unvalidatedCandidate.route_validated = $false

$completedScore = @($huntPlanning | Where-Object { $_.event -eq 'hunt_region_scan' -and $_.phase -eq 'scored' })[0]
$completedScore.scored_candidate_count = 89
Test-Evidence $huntPlanning $checkHuntPlanning $true
$completedScore.scored_candidate_count = 90
$cacheHitStart = @($huntPlanning | Where-Object {
    $_.event -eq 'hunt_region_scan' -and $_.phase -eq 'planning_started' -and $_.cache -eq 'hit'
})[0]
$cacheHitStart.snapshot_time_us = 1
Test-Evidence $huntPlanning $checkHuntPlanning $true
$cacheHitStart.snapshot_time_us = 0

Write-Host 'Supply, Exura, and hunt-planning telemetry assertion contracts passed.'
