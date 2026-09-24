#Requires -Version 7.0

<#
.SYNOPSIS
Runs disposable playerbot gameplay scenarios against the local Compose stack.

.DESCRIPTION
Builds or reuses the server image, runs the selected telemetry-backed gameplay
fixtures, and removes the scenario stack unless -KeepStack is set.

.PARAMETER SlottedLoot
Runs seller, no-eligible-seller depot fallback, and interrupted-deposit restart
fixtures for policy-approved loot carried in an invalid equipment slot.

.PARAMETER ValueLoot
Runs value replacement, currency-versus-spear, partial-capacity, nested-destination,
and protected full-slot cargo fixtures.

.PARAMETER Scenario
Runs only the named scenarios. Names must match the gameplay scenario catalog.

.PARAMETER ContinueOnFailure
Captures diagnostics and continues with the next selected scenario after a failure.

.PARAMETER Verbose
Shows suppressed Docker Compose command output while the suite runs.
#>
[CmdletBinding()]
param(
	[ValidateRange(30, 3600)]
	[int]$TimeoutSeconds = 300,
    [switch]$FullNavigation,
	[switch]$TargetApproach,
    [switch]$CorpseLoot,
    [switch]$DeathTelemetry,
	[switch]$Healing,
	[switch]$ValueLoot,
	[switch]$PickupProgression,
	[switch]$GoalArbitration,
	[switch]$OracleDeparture,
	[switch]$StaminaProjection,
	[switch]$HuntRegionPlanning,
	[switch]$AdaptiveChallenge,
	[switch]$CombatReadiness,
	[switch]$EquipmentOffers,
	[switch]$EquipmentPurchases,
	[switch]$MainlandRewards,
	[switch]$Depot,
	[switch]$SlottedLoot,
	[switch]$SellLoot,
	[switch]$MainlandLoop,
	[switch]$SpellTraining,
	[switch]$SpellUse,
	[switch]$SpellCalibration,
	[switch]$MagicTraining,
	[ValidateSet(
		"magic_training_haste", "magic_training_great_light", "magic_training_light", "magic_training_refresh",
		"magic_training_reserve", "magic_training_exact_full", "magic_training_pz", "magic_training_absent",
		"magic_training_expired", "magic_training_failed", "magic_training_service", "magic_training_progression",
		"magic_training_post_hunt", "magic_training_post_hunt_no_overflow", "magic_training_restart", "magic_training_hunt"
	)]
	[string]$MagicTrainingCase,
	[string[]]$Scenario,
	[switch]$ContinueOnFailure,
	[string]$FailureArtifactsPath,
	[switch]$Focused,
	[switch]$SkipBuild,
	[switch]$KeepStack
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$composeFile = Join-Path $projectRoot "server\compose.yaml"
$gameplayComposeFile = Join-Path $projectRoot "server\compose.playerbot-gameplay.yaml"
$composeArguments = @("compose", "-f", $composeFile, "-f", $gameplayComposeFile)
$scenarioCatalog = @(
	"cycle", "transit_return",
	"carlin_local_service", "mainland_loop", "slotted_loot_seller", "slotted_loot_no_seller", "slotted_loot_deposit_restart", "sell_loot", "sell_loot_remote_depot",
	"real_depot", "depot_risk_fallback", "real_depot_restart_approach", "real_depot_restart_locker", "real_depot_restart_chest",
	"real_depot_restart_deposit", "real_depot_restart_depart", "real_depot_partial_move", "real_depot_rejected_move",
	"pickup_progression", "pickup_progression_bundle", "pickup_progression_nested", "pickup_progression_resume",
	"pickup_progression_nested_resume", "pickup_progression_space", "goal_arbitration", "goal_arbitration_interrupt",
	"stamina_bonus_projection", "stamina_boundary_projection", "stamina_normal_projection", "hunt_region_planning", "supply_recovery",
	"combat_readiness_ready", "combat_readiness_upgrade", "combat_readiness_missing_weapon", "combat_readiness_supplies",
	"combat_readiness_no_food", "combat_readiness_low_wealth", "combat_readiness_food_capacity",
	"combat_readiness_retention", "hunt_area_arrival", "remote_hunt", "equipment_offer_shadow_upgrade", "equipment_offer_shadow_unaffordable",
	"equipment_offer_shadow_no_upgrade", "equipment_purchase", "equipment_purchase_resume", "equipment_purchase_provider_moved", "equipment_purchase_provider_unreachable", "equipment_purchase_space",
	"equipment_purchase_rejected", "equipment_tool_replenishment", "equipment_tool_nested_inventory", "adaptive_challenge", "mainland_equipment_reward", "oracle_departure",
		"oracle_level_eight_interrupt", "oracle_level_eight_recovery",
		"navigation", "navigation_recovery", "svargrond_local_route_recovery", "navigation_fare_rejection", "navigation_risk_rejection", "door_passages", "carlin_service_route", "mutable_portal_route", "mutable_portal_open_no_shovel", "patrol_recovery", "target_approach", "target_approach_unreachable", "target_attacker_priority",
	"spell_training", "spell_training_shortlist", "spell_training_low_supplies", "spell_training_low_supplies_unaffordable", "spell_use", "spell_calibration", "magic_training_haste", "magic_training_great_light",
	"magic_training_light", "magic_training_refresh", "magic_training_reserve", "magic_training_exact_full",
	"magic_training_pz", "magic_training_absent", "magic_training_expired", "magic_training_failed",
	"magic_training_service", "magic_training_progression", "magic_training_post_hunt",
	"magic_training_post_hunt_no_overflow", "magic_training_restart", "magic_training_hunt",
	"corpse", "corpse_detour", "corpse_inaccessible", "death", "healing", "healing_resupply", "value",
	"cargo_currency", "cargo_partial", "cargo_nested", "cargo_protected"
)
$scenarioCatalogSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($scenarioName in $scenarioCatalog) {
	if (-not $scenarioCatalogSet.Add($scenarioName)) {
		throw "Duplicate gameplay scenario name: $scenarioName"
	}
}
if ($scenarioCatalog.Count -ne 103) {
	throw "The gameplay scenario catalog must contain 103 scenarios; found $($scenarioCatalog.Count)."
}
$requestedScenarioNames = @($Scenario | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$exactScenarioSelection = $requestedScenarioNames.Count -gt 0
$selectedScenarios = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($scenarioName in $requestedScenarioNames) {
	if (-not $scenarioCatalogSet.Contains($scenarioName)) {
		throw "Unknown gameplay scenario '$scenarioName'."
	}
	[void]$selectedScenarios.Add($scenarioName)
}
if ($exactScenarioSelection -and ($Focused -or $MagicTrainingCase)) {
	throw "-Scenario cannot be combined with -Focused or -MagicTrainingCase."
}
if (-not $FailureArtifactsPath) {
	$FailureArtifactsPath = Join-Path $projectRoot "artifacts\playerbot-gameplay"
}
$scenarioResults = [System.Collections.Generic.List[object]]::new()
$scenarioRunId = [DateTime]::UtcNow.ToString("yyyyMMddTHHmmssZ")
$previousDuration = $env:PLAYERBOT_HUNT_DURATION_SECONDS
$previousMode = $env:PLAYERBOT_GAMEPLAY_MODE
$previousMutablePortalVariant = $env:PLAYERBOT_MUTABLE_PORTAL_VARIANT
$previousRelogDelay = $env:PLAYERBOT_RELOG_DELAY_SECONDS
$previousMaximumDeaths = $env:PLAYERBOT_MAX_CONSECUTIVE_DEATHS
$previousDepotRestartPhase = $env:PLAYERBOT_DEPOT_RESTART_PHASE
$previousDepotVerifierPhase = $env:PLAYERBOT_DEPOT_VERIFIER_PHASE
$previousDepotMoveCase = $env:PLAYERBOT_DEPOT_MOVE_CASE
$timeoutOverridden = $PSBoundParameters.ContainsKey("TimeoutSeconds")
$timings = [ordered]@{}
$currentWaitTimeoutSeconds = $TimeoutSeconds
$currentScenario = "startup"
$currentScenarioDeadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
$testStackInitialized = $false
$serverLogProcess = $null
$serverLogOutputTask = $null
$serverLogErrorTask = $null
$serverLogBuffer = [System.Text.StringBuilder]::new()
$serverLogLines = [System.Collections.Generic.List[string]]::new()
$serverPlayerbotEvents = [System.Collections.Generic.List[object]]::new()
$serverLogSinceUtc = $null
$composeCommandLogBuffer = [System.Text.StringBuilder]::new()
$minimumServerOnlineEvents = 0

. $PSScriptRoot/playerbot-gameplay/log-parsing.ps1
. $PSScriptRoot/playerbot-gameplay/runtime.ps1
. $PSScriptRoot/playerbot-gameplay/assertions-cycle.ps1
. $PSScriptRoot/playerbot-gameplay/assertions-progression.ps1
. $PSScriptRoot/playerbot-gameplay/assertions-readiness.ps1
. $PSScriptRoot/playerbot-gameplay/assertions-navigation.ps1
. $PSScriptRoot/playerbot-gameplay/assertions-spells.ps1
. $PSScriptRoot/playerbot-gameplay/assertions-magic-training.ps1

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
	throw "Docker is required to run the playerbot gameplay suite."
}

$focusedScenarioRequested = $FullNavigation -or $TargetApproach -or $CorpseLoot -or $DeathTelemetry -or $Healing -or $ValueLoot -or
	$PickupProgression -or $GoalArbitration -or $OracleDeparture -or $StaminaProjection -or $HuntRegionPlanning -or
	$AdaptiveChallenge -or
	$CombatReadiness -or $EquipmentOffers -or $EquipmentPurchases -or $MainlandRewards -or $Depot -or $SlottedLoot -or $SellLoot -or $MainlandLoop -or $SpellTraining -or $SpellUse -or $SpellCalibration -or $MagicTraining -or $MagicTrainingCase
if ($Focused -and -not $focusedScenarioRequested) {
	throw "-Focused requires at least one focused scenario switch."
}
if (-not $Focused) {
	$FullNavigation = $TargetApproach = $CorpseLoot = $DeathTelemetry = $Healing = $ValueLoot = $true
	$PickupProgression = $GoalArbitration = $OracleDeparture = $StaminaProjection = $HuntRegionPlanning = $true
	$AdaptiveChallenge = $CombatReadiness = $EquipmentOffers = $EquipmentPurchases = $MainlandRewards = $true
	$Depot = $SlottedLoot = $SellLoot = $MainlandLoop = $SpellTraining = $SpellUse = $SpellCalibration = $MagicTraining = $true
}

# Fail before entering stack cleanup when the daemon is unavailable or access is denied.
& docker info *> $null
if ($LASTEXITCODE -ne 0) {
	throw "Docker is unavailable or access is denied. Start Docker and use an explicitly authorized shell with daemon access; this script does not elevate privileges."
}

try {
	if ($SkipBuild) {
		& docker image inspect angelion-server:latest *> $null
		if ($LASTEXITCODE -ne 0) {
			throw "-SkipBuild requires an existing angelion-server:latest image."
		}
	} else {
		"PLAYERBOT_GAMEPLAY_TEST PHASE name=build status=start"
		Invoke-TimedStep -Name "build" -Body { Invoke-Compose build server }
		"PLAYERBOT_GAMEPLAY_TEST PHASE name=build status=pass duration_seconds=$([Math]::Round($timings.build.TotalSeconds, 2))"
	}

	if (-not $Focused) {
		Invoke-Scenario -Name "cycle" -DefaultTimeoutSeconds 180 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "cycle"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "10"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST SERVICE_PASS' | Out-Null
			$cycleLogs = Wait-ForLog -Pattern '"action":"hunt_cycle","result":"started","cycle":2'
			Assert-CycleEvents -Logs $cycleLogs
		}
	}

	. $PSScriptRoot/playerbot-gameplay/scenarios-service.ps1

	. $PSScriptRoot/playerbot-gameplay/scenarios-progression.ps1

	. $PSScriptRoot/playerbot-gameplay/scenarios-navigation.ps1

	. $PSScriptRoot/playerbot-gameplay/scenarios-spells.ps1

	. $PSScriptRoot/playerbot-gameplay/scenarios-combat-loot.ps1
	if (-not $Focused) {
		$resultNames = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
		foreach ($result in $scenarioResults) {
			[void]$resultNames.Add($result.Name)
		}
		if ($scenarioResults.Count -ne $scenarioCatalog.Count -or -not $resultNames.SetEquals($scenarioCatalogSet)) {
			throw "The gameplay scenario catalog does not match the scenarios enumerated by the suite."
		}
	}
	$failedScenarios = @($scenarioResults | Where-Object { $_.Status -in @("fail", "timeout") })
	if ($failedScenarios.Count -gt 0) {
		throw "$($failedScenarios.Count) gameplay scenario(s) failed."
	}
}
finally {
	try {
		Stop-ServerLogFollower
		if (-not $KeepStack -and (Get-Command docker -ErrorAction SilentlyContinue)) {
			Invoke-RawCompose down --volumes --remove-orphans
		}
	}
	finally {
		$env:PLAYERBOT_HUNT_DURATION_SECONDS = $previousDuration
		$env:PLAYERBOT_GAMEPLAY_MODE = $previousMode
		$env:PLAYERBOT_MUTABLE_PORTAL_VARIANT = $previousMutablePortalVariant
		$env:PLAYERBOT_RELOG_DELAY_SECONDS = $previousRelogDelay
		$env:PLAYERBOT_MAX_CONSECUTIVE_DEATHS = $previousMaximumDeaths
		$env:PLAYERBOT_DEPOT_RESTART_PHASE = $previousDepotRestartPhase
		$env:PLAYERBOT_DEPOT_VERIFIER_PHASE = $previousDepotVerifierPhase
		$env:PLAYERBOT_DEPOT_MOVE_CASE = $previousDepotMoveCase
		foreach ($timing in $timings.GetEnumerator()) {
			"PLAYERBOT_GAMEPLAY_TIMING $($timing.Key)=$([Math]::Round($timing.Value.TotalSeconds, 2))s"
		}
		Write-ScenarioSummary
	}
}
