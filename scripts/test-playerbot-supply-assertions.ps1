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
Write-Host 'Supply and Exura telemetry assertion contracts passed.'
