<#
.SYNOPSIS
Runs disposable playerbot gameplay scenarios against the local Compose stack.

.DESCRIPTION
Builds or reuses the server image, runs the selected telemetry-backed gameplay
fixtures, and removes the scenario stack unless -KeepStack is set.

.PARAMETER SlottedLoot
Runs seller, no-eligible-seller depot fallback, and interrupted-deposit restart
fixtures for policy-approved loot carried in an invalid equipment slot.
#>
param(
	[ValidateRange(30, 3600)]
	[int]$TimeoutSeconds = 300,
    [switch]$FullNavigation,
	[switch]$TargetPursuit,
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
	[switch]$Focused,
	[switch]$SkipBuild,
	[switch]$KeepStack
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$composeFile = Join-Path $projectRoot "server\compose.yaml"
$gameplayComposeFile = Join-Path $projectRoot "server\compose.playerbot-gameplay.yaml"
$composeArguments = @("compose", "-f", $composeFile, "-f", $gameplayComposeFile)
$previousDuration = $env:PLAYERBOT_HUNT_DURATION_SECONDS
$previousMode = $env:PLAYERBOT_GAMEPLAY_MODE
$previousRelogDelay = $env:PLAYERBOT_RELOG_DELAY_SECONDS
$previousMaximumDeaths = $env:PLAYERBOT_MAX_CONSECUTIVE_DEATHS
$previousDepotRestartPhase = $env:PLAYERBOT_DEPOT_RESTART_PHASE
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

$focusedScenarioRequested = $FullNavigation -or $TargetPursuit -or $CorpseLoot -or $DeathTelemetry -or $Healing -or $ValueLoot -or
	$PickupProgression -or $GoalArbitration -or $OracleDeparture -or $StaminaProjection -or $HuntRegionPlanning -or
	$AdaptiveChallenge -or
	$CombatReadiness -or $EquipmentOffers -or $EquipmentPurchases -or $MainlandRewards -or $Depot -or $SlottedLoot -or $MainlandLoop -or $SpellTraining -or $SpellUse -or $SpellCalibration -or $MagicTraining -or $MagicTrainingCase
if ($Focused -and -not $focusedScenarioRequested) {
	throw "-Focused requires at least one focused scenario switch."
}
if (-not $Focused) {
	$FullNavigation = $TargetPursuit = $CorpseLoot = $DeathTelemetry = $Healing = $ValueLoot = $true
	$PickupProgression = $GoalArbitration = $OracleDeparture = $StaminaProjection = $HuntRegionPlanning = $true
	$AdaptiveChallenge = $CombatReadiness = $EquipmentOffers = $EquipmentPurchases = $MainlandRewards = $true
	$Depot = $SlottedLoot = $MainlandLoop = $SpellTraining = $SpellUse = $SpellCalibration = $MagicTraining = $true
}

try {
	& docker info *> $null
	if ($LASTEXITCODE -ne 0) {
		throw "Docker is not running."
	}

	if ($SkipBuild) {
		& docker image inspect angelion-server:latest *> $null
		if ($LASTEXITCODE -ne 0) {
			throw "-SkipBuild requires an existing angelion-server:latest image."
		}
	} else {
		Invoke-TimedStep -Name "build" -Body { Invoke-Compose build server }
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
	"PLAYERBOT_GAMEPLAY_TEST PASS"
}
finally {
	try {
		Stop-ServerLogFollower
		if (-not $KeepStack -and (Get-Command docker -ErrorAction SilentlyContinue)) {
			& docker @composeArguments down --volumes --remove-orphans
			if ($LASTEXITCODE -ne 0) {
				throw "Could not clean up the playerbot gameplay stack."
			}
		}
	}
	finally {
		$env:PLAYERBOT_HUNT_DURATION_SECONDS = $previousDuration
		$env:PLAYERBOT_GAMEPLAY_MODE = $previousMode
		$env:PLAYERBOT_RELOG_DELAY_SECONDS = $previousRelogDelay
		$env:PLAYERBOT_MAX_CONSECUTIVE_DEATHS = $previousMaximumDeaths
		$env:PLAYERBOT_DEPOT_RESTART_PHASE = $previousDepotRestartPhase
		$env:PLAYERBOT_DEPOT_MOVE_CASE = $previousDepotMoveCase
		foreach ($timing in $timings.GetEnumerator()) {
			"PLAYERBOT_GAMEPLAY_TIMING $($timing.Key)=$([Math]::Round($timing.Value.TotalSeconds, 2))s"
		}
	}
}
