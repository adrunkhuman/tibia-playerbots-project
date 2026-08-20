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

function Invoke-Compose {
    param([Parameter(ValueFromRemainingArguments)][string[]]$Arguments)

    & docker @composeArguments @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "docker compose failed: $($Arguments -join ' ')"
    }
}

function Get-ServerLogs {
    $output = & docker @composeArguments logs --no-log-prefix server 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Could not read server logs."
    }
    return $output -join "`n"
}

function Get-OnlineBotCount {
    $query = "SELECT COUNT(*) FROM players_online JOIN players ON players.id = players_online.player_id WHERE players.name = 'Bot One';"
    $output = & docker @composeArguments exec -T database mariadb --host=database --user=angelion --password=angelion --skip-column-names angelion -e $query
    if ($LASTEXITCODE -ne 0) {
        throw "Could not query Bot One's online state."
    }
    return [int]($output | Select-Object -Last 1)
}

function Invoke-DatabaseScalar {
	param([string]$Query)

	$output = & docker @composeArguments exec -T database mariadb --host=database --user=angelion --password=angelion --skip-column-names angelion -e $Query
	if ($LASTEXITCODE -ne 0) {
		throw "Database query failed."
	}
	return [int]($output | Select-Object -Last 1)
}

function Invoke-DatabaseCommand {
	param([string]$Query)

	& docker @composeArguments exec -T database mariadb --host=database --user=angelion --password=angelion angelion -e $Query
	if ($LASTEXITCODE -ne 0) {
		throw "Database command failed."
	}
}

function Throw-WaitTimeout {
	param([string]$Message)

	$status = & docker @composeArguments ps --all 2>&1
	$logs = try { Get-ServerLogs } catch { "Server logs unavailable: $($_.Exception.Message)" }
	$tail = (($logs -split "`r?`n") | Select-Object -Last 80) -join "`n"
	throw "$Message`nScenario: $currentScenario`n--- compose status ---`n$($status -join "`n")`n--- server log tail ---`n$tail"
}

function Wait-ForLog {
    param([string]$Pattern)

	$deadline = $currentScenarioDeadline
    while ([DateTime]::UtcNow -lt $deadline) {
        $logs = Get-ServerLogs
        if ($logs -match $Pattern) {
            return $logs
        }
        Start-Sleep -Seconds 2
    }
	Throw-WaitTimeout "Timed out after $currentWaitTimeoutSeconds seconds waiting for server log pattern: $Pattern"
}

function Wait-ForPlayerbotEvent {
    param([scriptblock]$Predicate)

	$deadline = $currentScenarioDeadline
    while ([DateTime]::UtcNow -lt $deadline) {
        $logs = Get-ServerLogs
        if (@(ConvertFrom-PlayerbotLogs -Logs $logs | Where-Object $Predicate).Count -gt 0) {
            return $logs
        }
        Start-Sleep -Seconds 1
    }
	Throw-WaitTimeout "Timed out after $currentWaitTimeoutSeconds seconds waiting for a playerbot event."
}

function Wait-ForPlayerbotEventCount {
    param(
        [string]$Action,
        [int]$Count
    )

	$deadline = $currentScenarioDeadline
    while ([DateTime]::UtcNow -lt $deadline) {
        $logs = Get-ServerLogs
        $events = @(ConvertFrom-PlayerbotLogs -Logs $logs | Where-Object {
            $_.event -eq "action_result" -and $_.action -eq $Action -and $_.result -eq "reached"
        })
        if ($events.Count -ge $Count) {
            return $logs
        }
        Start-Sleep -Seconds 2
    }
	Throw-WaitTimeout "Timed out after $currentWaitTimeoutSeconds seconds waiting for $Count '$Action' events."
}

function Invoke-TimedStep {
	param(
		[string]$Name,
		[scriptblock]$Body
	)

	$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
	try {
		& $Body
	}
	finally {
		$stopwatch.Stop()
		$timings[$Name] = $stopwatch.Elapsed
	}
}

function Invoke-Scenario {
	param(
		[string]$Name,
		[int]$DefaultTimeoutSeconds,
		[scriptblock]$Body
	)

	$script:currentScenario = $Name
	$script:currentWaitTimeoutSeconds = if ($timeoutOverridden) { $TimeoutSeconds } else { $DefaultTimeoutSeconds }
	$script:currentScenarioDeadline = [DateTime]::UtcNow.AddSeconds($currentWaitTimeoutSeconds)
	Invoke-TimedStep -Name $Name -Body $Body
}

function ConvertFrom-PlayerbotLogs {
    param([string]$Logs)

    foreach ($line in $Logs -split "`r?`n") {
        if (-not $line.StartsWith('{')) {
            continue
        }
        try {
            $event = $line | ConvertFrom-Json
            if ($event.component -eq "playerbot") {
                $event
            }
        }
        catch {
            continue
        }
    }
}

function Assert-CycleEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $deposit = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "deposit" -and
        $_.result -eq "success" -and $_.item_id -eq 1987 -and $_.count -eq 1
    })
    $serviceDiscovery = @($events | Where-Object {
        $_.event -eq "service_discovered" -and $_.capability -in @("shop", "banker")
    })
    $shopCatalog = @($serviceDiscovery | Where-Object { $_.capability -eq "shop" -and $_.offers -gt 0 })
    $bankerDiscovery = @($serviceDiscovery | Where-Object { $_.capability -eq "banker" })
    $npcReplies = @($events | Where-Object { $_.event -eq "npc_reply" -and $_.npc_name })
    $bankDeposit = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "bank_deposit" -and $_.result -eq "success"
    })
    $bankWithdraw = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "bank_withdraw" -and $_.result -eq "success"
    })
    $dynamicSale = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "sell" -and $_.result -eq "success" -and
        $_.item_id -eq 2992 -and $_.count -eq 1
    })
    $cycles = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "hunt_cycle" -and $_.result -eq "started"
    })
    $huntPlan = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "plan" -and $_.result -eq "success" -and
        $_.destination.x -eq 32084 -and $_.destination.y -eq 32144 -and $_.destination.z -eq 5
    })
    $terminal = @($events | Where-Object { $_.event -eq "terminal" })
    $defensiveBlockerTargets = @($events | Where-Object {
        $_.event -eq "target_changed" -and $_.target_name -eq "Playerbot Defensive Threat" -and
        $_.reason -eq "defensive_path_blocker" -and $_.route_critical -eq $true
    })
    $defensiveBlockerIds = @($defensiveBlockerTargets | ForEach-Object { $_.target_id })
    $defensiveStart = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "defensive_combat" -and
        $_.result -eq "started" -and $_.chase -eq $false -and $_.route_critical -eq $true -and
        $defensiveBlockerIds -contains $_.target_id
    })
    $defensiveComplete = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "defensive_combat" -and
        $_.result -eq "success" -and $_.reason -eq "target_defeated" -and
        $defensiveBlockerIds -contains $_.target_id
    })

    if ($deposit.Count -ne 1) {
        throw "Expected exactly one injected-loot deposit event, found $($deposit.Count)."
    }
    if ($shopCatalog.Count -lt 1 -or $bankerDiscovery.Count -lt 1) {
        throw "The bot did not discover the tagged live service NPC catalogues."
    }
    if ($npcReplies.Count -lt 3) {
        throw "The bot did not acknowledge the selected NPCs before requesting services."
    }
    if (@($events | Where-Object { $_.event -eq "service_catalog" }).Count -ne 0) {
        throw "The bot probed a shop window instead of using the live NPC offer catalog."
    }
    if ($bankDeposit.Count -lt 1 -or $bankWithdraw.Count -lt 1) {
        throw "The bot did not produce the expected purchase, sale, and bank balance result."
    }
    if ($dynamicSale.Count -ne 1) {
        throw "The bot did not sell the dead rabbit discovered from the live NPC offer catalog."
    }
    if ($cycles.Count -lt 2) {
        throw "The bot did not begin a second hunt cycle."
    }
    if ($huntPlan.Count -lt 1) {
        throw "The bot did not plan a map-derived route to hunting point A."
    }
    if ($defensiveBlockerTargets.Count -lt 1 -or $defensiveStart.Count -lt 1 -or $defensiveComplete.Count -lt 1) {
        throw "The bot did not prioritize and defeat the defensive attacker blocking its navigation step."
    }
    if ($terminal.Count -ne 0) {
        throw "The playerbot emitted a terminal event during the gameplay cycle."
    }
}

function Assert-OracleDepartureEvents {
    param([string]$Logs, [switch]$Restart, [switch]$InterruptedByRestart)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    if ($Restart) {
        $restored = @($events | Where-Object {
            $_.event -eq "lifecycle" -and $_.status -eq "online" -and $_.objective -eq "service"
        })
        if ($restored.Count -lt 1) {
            throw "The persisted Oracle departure state was not restored."
        }
        return
    }

    $candidate = @($events | Where-Object {
        $_.event -eq "goal_candidate" -and $_.goal -eq "oracle_departure" -and $_.feasible -eq $true -and
        $_.town_id -eq 2 -and $_.vocation_id -eq 4
    })
    $selection = @($events | Where-Object {
        $_.event -eq "goal_selection" -and $_.to_goal -eq "oracle_departure" -and
        $_.town_id -eq 2 -and $_.vocation_id -eq 4
    })
    $result = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "oracle_departure" -and $_.result -eq "success" -and
        $_.town_id -eq 2 -and $_.vocation_id -eq 4 -and $_.teleported -eq $true
    })
    $goalResult = @($events | Where-Object {
        $_.event -eq "goal_result" -and $_.goal -eq "oracle_departure" -and $_.result -eq "success"
    })
    $continued = @($events | Where-Object {
        $_.event -eq "objective_transition" -and $_.to -eq "service" -and $_.reason -eq "departure_complete"
    })
    if ($candidate.Count -lt 1 -or $selection.Count -lt 1 -or $result.Count -ne 1 -or
        $goalResult.Count -ne 1 -or $continued.Count -ne 1) {
        throw "The bot did not complete and continue after the selected Oracle departure: candidate=$($candidate.Count), selection=$($selection.Count), result=$($result.Count), goalResult=$($goalResult.Count), continued=$($continued.Count)."
    }
}

function Assert-OracleLevelEightInterruptEvents {
    param([string]$Logs, [switch]$Recovery)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $selection = @($events | Where-Object {
        $_.event -eq "goal_selection" -and $_.to_goal -eq "oracle_departure" -and $_.forced -eq $true -and
        $_.level -eq 8 -and $_.player_vocation_id -eq 0
    })
    $selectionIndex = -1
    for ($index = 0; $index -lt $events.Count; $index++) {
        if ($events[$index].event -eq "goal_selection" -and $events[$index].to_goal -eq "oracle_departure" -and
            $events[$index].forced -eq $true) {
            $selectionIndex = $index
            break
        }
    }
    $postInterrupt = if ($selectionIndex -ge 0 -and $selectionIndex + 1 -lt $events.Count) {
        @($events[($selectionIndex + 1)..($events.Count - 1)])
    } else {
        @()
    }
    $huntAfterInterrupt = @($postInterrupt | Where-Object {
        ($_.event -eq "goal_selection" -and $_.to_goal -eq "hunt") -or
        ($_.event -eq "action_result" -and $_.action -in @("hunt_cycle", "hunt_waypoint", "loot")) -or
        ($_.event -eq "target_changed" -and $_.reason -eq "visible_monster")
    })
    if ($Recovery) {
        $restored = @($events | Where-Object {
            $_.event -eq "lifecycle" -and $_.status -eq "online" -and $_.objective -eq "oracle_departure"
        })
        $healed = @($events | Where-Object {
            $_.event -eq "action_result" -and $_.action -eq "heal" -and $_.result -eq "success"
        })
        if ($restored.Count -lt 1 -or $selection.Count -lt 1 -or $healed.Count -lt 1 -or
            $huntAfterInterrupt.Count -ne 0) {
            throw "The restored level-8 player did not remain committed to Oracle departure."
        }
        return
    }

    $combat = @($events | Where-Object {
        $_.event -eq "target_changed" -and $_.target_name -eq "Playerbot Level Eight Target"
    })
    $interruptedHunt = @($events | Where-Object {
        $_.event -eq "goal_result" -and $_.goal -eq "hunt" -and $_.result -eq "interrupted" -and
        $_.reason -eq "level_eight_interrupt"
    })
    $loot = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "loot" })
    $defensiveStart = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "defensive_combat" -and $_.result -eq "started"
    })
    $defensiveComplete = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "defensive_combat" -and $_.result -eq "success"
    })
    if ($combat.Count -lt 1 -or $interruptedHunt.Count -ne 1 -or $selection.Count -ne 1 -or
        $loot.Count -ne 0 -or $defensiveStart.Count -lt 1 -or $defensiveComplete.Count -lt 1 -or
        $huntAfterInterrupt.Count -ne 0) {
        throw "Level 8 did not interrupt combat, looting, and hunting for Oracle departure."
    }
}

function Assert-DeathEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $deaths = @($events | Where-Object {
        $_.event -eq "lifecycle" -and $_.status -eq "dead" -and
        $_.level -gt 0 -and $_.objective -and $_.state -and $_.health -eq 0 -and
        $_.killer_id -gt 0 -and $_.killer_name -eq "Playerbot Death Threat" -and
        $_.killer_type -eq "monster" -and $_.most_damage_id -eq $_.killer_id -and
        $_.most_damage_name -eq $_.killer_name
    })
    $terminals = @($events | Where-Object {
        $_.event -eq "terminal" -and $_.reason -eq "controlled_player_dead"
    })
    $scheduled = @($events | Where-Object {
        $_.event -eq "lifecycle" -and $_.status -eq "recovery_scheduled" -and
        $_.reason -eq "death" -and $_.relog_attempt -eq 1
    })
    $recovered = @($events | Where-Object {
        $_.event -eq "lifecycle" -and $_.status -eq "online" -and $_.recovered -eq $true -and
        $_.objective -eq "service"
    })
    $abandoned = @($events | Where-Object {
        $_.event -eq "lifecycle" -and $_.status -eq "recovery_abandoned" -and
        $_.reason -eq "death_loop_limit"
    })
    if ($deaths.Count -ne 3) {
        throw "Expected exactly three contextual playerbot death events, found $($deaths.Count)."
    }
    if ($terminals.Count -ne 3) {
        throw "Expected exactly three controlled_player_dead terminal events, found $($terminals.Count)."
    }
    if ($scheduled.Count -ne 2 -or $scheduled[0].death_count -ne 1 -or $scheduled[0].delay_ms -ne 1000 -or
        $scheduled[1].death_count -ne 2 -or $scheduled[1].delay_ms -ne 2000) {
        throw "Death recovery did not schedule the expected bounded exponential backoff."
    }
    if ($recovered.Count -ne 2 -or $recovered[0].recovery_count -ne 1 -or $recovered[1].recovery_count -ne 2) {
        throw "Expected two fresh service-first recovered controllers."
    }
    if ($abandoned.Count -ne 1 -or $abandoned[0].death_count -ne 3 -or $abandoned[0].maximum_deaths -ne 2) {
        throw "Death recovery did not stop at the configured consecutive-death limit."
    }
    if (@($events | Where-Object { $_.event -eq "lifecycle" -and $_.status -eq "recovery_failed" }).Count -ne 0) {
        throw "The focused death recovery scenario produced a failed relog."
    }

    for ($deathIndex = 0; $deathIndex -lt $recovered.Count; $deathIndex++) {
        $start = [Array]::IndexOf($events, $deaths[$deathIndex])
        $scheduledIndex = [Array]::IndexOf($events, $scheduled[$deathIndex])
        $terminalIndex = [Array]::IndexOf($events, $terminals[$deathIndex])
        $end = [Array]::IndexOf($events, $recovered[$deathIndex])
        if ($start -lt 0 -or $scheduledIndex -le $start -or $terminalIndex -le $scheduledIndex -or $end -le $terminalIndex) {
            throw "Death, scheduling, terminal, and recovered events were not causally ordered."
        }
        $actionsDuringRelog = @($events[($start + 1)..($end - 1)] | Where-Object { $_.event -eq "action_result" })
        if ($actionsDuringRelog.Count -ne 0) {
            throw "The dead controller continued acting during its relog delay."
        }
        $scheduledAt = [DateTimeOffset]::Parse($scheduled[$deathIndex].ts).ToUnixTimeMilliseconds()
        $recoveredAt = [DateTimeOffset]::Parse($recovered[$deathIndex].ts).ToUnixTimeMilliseconds()
        if ($recoveredAt - $scheduledAt -lt $scheduled[$deathIndex].delay_ms - 100) {
            throw "The bot recovered before its requested relog delay elapsed."
        }
    }

    $secondRecoveryIndex = [Array]::IndexOf($events, $recovered[1])
    $thirdDeathIndex = [Array]::IndexOf($events, $deaths[2])
    $serviceAfterRecovery = @($events[($secondRecoveryIndex + 1)..($thirdDeathIndex - 1)] | Where-Object {
        $_.event -eq "service_discovered"
    })
    $huntAfterRecovery = @($events[($secondRecoveryIndex + 1)..($thirdDeathIndex - 1)] | Where-Object {
        $_.event -eq "action_result" -and $_.action -in @("attack", "hunt_cycle", "hunt_waypoint")
    })
    if ($serviceAfterRecovery.Count -lt 1 -or $huntAfterRecovery.Count -ne 0) {
        throw "The final recovered controller did not resume service before hunting."
    }
    if (@($events[($thirdDeathIndex + 1)..($events.Count - 1)] | Where-Object {
        $_.event -eq "lifecycle" -and $_.status -eq "online"
    }).Count -ne 0) {
        throw "The bot relogged after reaching the configured death-loop limit."
    }
}

function Assert-HealingEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $heals = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "heal" -and $_.result -eq "success" -and
        $_.method -eq "small_health_potion" -and $_.item_id -eq 8704 -and
        $_.trigger -eq "health_threshold" -and $_.objective -eq "hunt" -and
        $_.health_after -gt $_.health_before -and $_.resource_after -eq ($_.resource_before - 1)
    })
    $failures = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "heal" -and $_.result -ne "success"
    })
    $lastHealIndex = -1
    $planAfterHealing = $false
    for ($index = 0; $index -lt $events.Count; $index++) {
        $event = $events[$index]
        if ($event.event -eq "action_result" -and $event.action -eq "heal" -and $event.result -eq "success") {
            $lastHealIndex = $index
        }
        elseif ($lastHealIndex -ge 0 -and $event.event -eq "action_result" -and $event.action -eq "plan" -and
            $event.result -eq "success" -and $event.destination.x -eq 32084 -and
            $event.destination.y -eq 32144 -and $event.destination.z -eq 5) {
            $planAfterHealing = $true
        }
    }
    if ($heals.Count -lt 1 -or $heals.Count -gt 3) {
        throw "Expected one to three verified small-health-potion actions, found $($heals.Count)."
    }
    if ($heals[-1].health_after * 100 -le $heals[-1].health_max * 60) {
        throw "The bot did not heal above its configured health threshold."
    }
    if ($failures.Count -ne 0) {
        throw "The focused healing scenario produced a failed or skipped healing outcome."
    }
    if (-not $planAfterHealing) {
        throw "The bot did not resume its interrupted hunt objective after healing."
    }
    if (@($events | Where-Object { $_.event -eq "terminal" }).Count -ne 0) {
        throw "The playerbot emitted a terminal event during healing."
    }
}

function Assert-HealingResupplyEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $missingSupply = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "heal" -and
        $_.result -eq "skipped" -and $_.reason -eq "missing_supply" -and $_.objective -eq "hunt"
    })
    $purchases = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "buy_potions" -and $_.result -eq "success"
    })
    $flaskSales = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "sell" -and $_.result -eq "success" -and
        $_.item_id -eq 7636 -and $_.count -eq 1
    })
    $heals = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "heal" -and $_.result -eq "success" -and
        $_.objective -eq "service" -and $_.resource_after -eq ($_.resource_before - 1)
    })
	$serviceResumed = @($events | Where-Object {
		$_.event -eq "objective_transition" -and $_.from -eq "service" -and $_.to -eq "return_to_depot"
	})
	$foodPurchases = @($events | Where-Object { $_.action -eq "buy_meat" })
    $transactionFailures = @($events | Where-Object {
        $_.reason -eq "transaction_delta_mismatch" -or $_.reason -eq "shop_transaction_delta_mismatch"
    })
    if ($missingSupply.Count -ne 1 -or $flaskSales.Count -ne 1 -or $purchases.Count -lt 1 -or $heals.Count -lt 1) {
        throw "The bot did not refill and consume potions after the missing-supply healing outcome: missing=$($missingSupply.Count), flaskSales=$($flaskSales.Count), purchases=$($purchases.Count), heals=$($heals.Count)."
    }
	if ($serviceResumed.Count -lt 1 -or $foodPurchases.Count -ne 0) {
		throw "The bot did not resume service after healing with newly purchased potions."
    }
    if ($transactionFailures.Count -ne 0 -or @($events | Where-Object { $_.event -eq "terminal" }).Count -ne 0) {
        throw "Healing interfered with service transaction verification."
    }
}

function Assert-ValueLootEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$replacement = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "loot_replace" -and $_.result -eq "success" -and
		$_.discarded_item_id -eq 2671 -and $_.discarded_count -eq 1 -and $_.discarded_value -gt 0 -and
		$_.incoming_item_id -eq 2826
    })
    $incomingLoot = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "loot" -and $_.result -eq "success" -and
        $_.item_id -eq 2826 -and $_.count -eq 1 -and $_.unit_value -eq 5 -and $_.total_value -eq 5
    })
    $capacitySkips = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "loot" -and $_.reason -eq "no_capacity"
    })
	$foodPurchases = @($events | Where-Object { $_.action -eq "buy_meat" })
	$foodPreference = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "loot" -and $_.result -eq "skipped" -and
		$_.reason -eq "food_preference_satisfied" -and $_.item_id -eq 2666 -and
		$_.carried -ge $_.preferred -and $_.preferred -eq 2
	})
	if ($replacement.Count -ne 1 -or $incomingLoot.Count -ne 1 -or $capacitySkips.Count -ne 0 -or
		$foodPurchases.Count -ne 0 -or $foodPreference.Count -ne 1) {
        throw "The bot did not replace lower-value cargo with the more profitable corpse item."
    }
}

function Assert-PickupProgressionEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $candidate = @($events | Where-Object {
        $_.event -eq "strategy_candidate" -and $_.candidate_id -eq 64120 -and $_.result -eq "feasible" -and
        $_.item_id -eq 2384 -and $_.benefit -eq 3 -and $_.travel_steps -gt 0
    })
    $selection = @($events | Where-Object {
        $_.event -eq "strategy_selection" -and $_.candidate_id -eq 64120 -and
		$_.reason -eq "highest_known_utility_reachable_reward"
    })
    $claim = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "claim_reward" -and $_.result -eq "success" -and
        $_.candidate_id -eq 64120 -and $_.item_id -eq 2384 -and $_.inventory_after -eq ($_.inventory_before + 1)
    })
    $equip = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "equip" -and $_.result -eq "success" -and
        $_.item_id -eq 2384 -and $_.displaced_item_id -eq 2382 -and $_.metric -eq "attack" -and
		$_.value_before -eq 7 -and $_.value_after -eq 10
    })
    $complete = @($events | Where-Object {
        $_.event -eq "strategy_objective_result" -and $_.candidate_id -eq 64120 -and
        $_.result -eq "success" -and $_.reason -eq "reward_equipped"
    })
    $online = @($events | Where-Object {
        $_.event -eq "lifecycle" -and $_.status -eq "online" -and $_.objective -eq "pickup_reward" -and
        $_.step_speed -gt 220
    })
    if ($candidate.Count -ne 1 -or $selection.Count -ne 1 -or $claim.Count -ne 1 -or
        $equip.Count -ne 1 -or $complete.Count -ne 1 -or $online.Count -ne 1) {
        throw "The pickup progression objective did not complete the expected candidate, claim, equip, and lifecycle sequence."
    }
    if (@($events | Where-Object { $_.event -eq "terminal" }).Count -ne 0) {
        throw "The playerbot emitted a terminal event during pickup progression."
    }
}

function Assert-PickupProgressionRestartEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $firstClaims = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "claim_reward" -and
		$_.result -eq "success" -and $_.candidate_id -eq 64120
    })
    $nextSelection = @($events | Where-Object {
		$_.event -eq "goal_selection" -and $_.to_goal -eq "hunt"
    })
    $online = @($events | Where-Object {
		$_.event -eq "lifecycle" -and $_.status -eq "online"
    })
	if ($firstClaims.Count -ne 1 -or $nextSelection.Count -lt 1 -or $online.Count -lt 2) {
        throw "Pickup progression did not reconstruct the next objective from persisted claim and equipment state."
    }
}

function Assert-NestedPickupProgressionEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $selection = @($events | Where-Object {
        $_.event -eq "strategy_selection" -and $_.candidate_id -eq 50083 -and $_.item_id -eq 2512 -and
        $_.root_item_id -eq 1994 -and $_.known_utility -gt 0
    })
    $inspection = @($events | Where-Object {
        $_.event -eq "reward_inspection" -and $_.candidate_id -eq 50083 -and $_.recursive -and
		$_.item_count -ge 5 -and $_.container_count -eq 1 -and $_.equipment_upgrade_count -eq 1 -and
		$_.equipment_rule -in @("pareto_improvement", "unlocks_suitable_hunt", "fills_readiness_gap") -and
        @($_.items | Where-Object { $_.item_id -eq 2512 -and $_.classes -contains "equipment_upgrade" }).Count -eq 1 -and
        @($_.items | Where-Object { $_.item_id -eq 2380 }).Count -eq 1 -and
        @($_.items | Where-Object { $_.item_id -eq 2175 }).Count -eq 1 -and
        (@($_.items | Where-Object { $_.item_id -eq 2666 -and $_.classes -contains "food" }) |
            Measure-Object -Property count -Sum).Sum -eq 2
    })
    $claim = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "claim_reward" -and $_.candidate_id -eq 50083 -and
        $_.root_item_id -eq 1994 -and $_.root_count_after -eq ($_.root_count_before + 1)
    })
    $opened = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "open_reward_container" -and
        $_.result -eq "requested" -and $_.item_id -eq 1994 -and $_.depth -eq 0
    })
    $equip = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "equip" -and $_.result -eq "success" -and
        $_.item_id -eq 2512 -and $_.slot -eq 5
    })
    $equipIndex = -1
    $claimSeen = $false
    $eatDuringAccess = $false
    for ($index = 0; $index -lt $events.Count; $index++) {
        if ($events[$index].event -eq "action_result" -and $events[$index].action -eq "claim_reward" -and
            $events[$index].result -eq "success" -and $events[$index].candidate_id -eq 50083) {
            $claimSeen = $true
        }
        if ($events[$index].event -eq "action_result" -and $events[$index].action -eq "equip" -and
            $events[$index].result -eq "success" -and $events[$index].item_id -eq 2512) {
            $equipIndex = $index
            break
        }
        if ($claimSeen -and $events[$index].event -eq "action_result" -and $events[$index].action -eq "eat") {
            $eatDuringAccess = $true
        }
    }
    if ($selection.Count -ne 1 -or $inspection.Count -lt 1 -or $claim.Count -ne 1 -or
        $opened.Count -ne 1 -or $equip.Count -ne 1 -or $equipIndex -lt 0 -or $eatDuringAccess) {
        throw "Nested pickup progression failed: selection=$($selection.Count), inspection=$($inspection.Count), claim=$($claim.Count), opened=$($opened.Count), equip=$($equip.Count), equipIndex=$equipIndex, eatDuringAccess=$eatDuringAccess."
    }
}

function Assert-EconomicPickupProgressionEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $selection = @($events | Where-Object {
        $_.event -eq "strategy_selection" -and $_.candidate_id -eq 50082 -and $_.item_id -eq 2152 -and
        $_.root_item_id -eq 2152 -and $_.known_utility -eq 1000
    })
    $inspection = @($events | Where-Object {
        $_.event -eq "reward_inspection" -and $_.candidate_id -eq 50082 -and $_.item_count -eq 2 -and
        $_.currency_value -eq 1000 -and $_.equipment_upgrade_count -eq 0 -and
        @($_.items | Where-Object { $_.item_id -eq 2152 -and $_.classes -contains "currency" }).Count -eq 1 -and
        @($_.items | Where-Object { $_.item_id -eq 2050 -and $_.classes -contains "unknown_keep" }).Count -eq 1
    })
    $claim = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "claim_reward" -and $_.candidate_id -eq 50082 -and
        $_.inventory_before -eq 5 -and $_.inventory_after -eq 15 -and
        $_.top_level_root_count -eq 2 -and $_.all_roots_verified
    })
    $complete = @($events | Where-Object {
        $_.event -eq "strategy_objective_result" -and $_.candidate_id -eq 50082 -and
        $_.result -eq "success" -and $_.reason -eq "reward_bundle_claimed"
    })
    $equipmentActions = @($events | Where-Object { $_.action -eq "equip" -or $_.action -eq "open_reward_container" })
    if ($selection.Count -ne 1 -or $inspection.Count -lt 1 -or $claim.Count -ne 1 -or
        $complete.Count -ne 1 -or $equipmentActions.Count -ne 0) {
        throw "Economic pickup progression did not preserve and verify the complete non-equipment reward bundle."
    }
}

function Assert-PickupProgressionResumeEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $selection = @($events | Where-Object {
        $_.event -eq "strategy_selection" -and $_.candidate_id -eq 64120 -and
        $_.reason -eq "resume_claimed_upgrade" -and $_.travel_steps -eq 0
    })
    $equip = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "equip" -and $_.result -eq "success" -and
        $_.item_id -eq 2384 -and $_.displaced_item_id -eq 2382
    })
    if ($selection.Count -ne 1 -or $equip.Count -ne 1) {
        throw "Pickup progression did not resume a persisted claimed-but-unequipped reward."
    }
    if (@($events | Where-Object { $_.action -eq "claim_reward" }).Count -ne 0) {
        throw "Claimed-reward reconstruction attempted to claim the reward again."
    }
}

function Assert-NestedPickupProgressionResumeEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $selection = @($events | Where-Object {
        $_.event -eq "strategy_selection" -and $_.candidate_id -eq 50083 -and
        $_.item_id -eq 2512 -and $_.root_item_id -eq 1994 -and
        $_.reason -eq "resume_claimed_upgrade" -and $_.travel_steps -eq 0
    })
    $opened = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "open_reward_container" -and $_.item_id -eq 1994
    })
    $equip = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "equip" -and $_.result -eq "success" -and
        $_.item_id -eq 2512
    })
    if ($selection.Count -ne 1 -or $opened.Count -ne 1 -or $equip.Count -ne 1) {
        throw "Pickup progression did not resume a persisted nested claimed-but-unequipped reward."
    }
    if (@($events | Where-Object { $_.action -eq "claim_reward" }).Count -ne 0) {
        throw "Nested claimed-reward reconstruction attempted to claim the reward again."
    }
}

function Assert-PickupProgressionSpaceEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $rejected = @($events | Where-Object {
        $_.event -eq "strategy_candidate" -and $_.candidate_id -eq 64120 -and
        $_.result -eq "rejected" -and $_.reason -eq "insufficient_inventory_space"
    })
    $selected = @($events | Where-Object { $_.event -eq "strategy_selection" })
    $online = @($events | Where-Object {
        $_.event -eq "lifecycle" -and $_.status -eq "online" -and $_.objective -eq "service"
    })
    if ($rejected.Count -ne 1 -or $selected.Count -ne 0 -or $online.Count -ne 1) {
        throw "Pickup progression did not reject a claim that lacked reward-and-exchange space."
    }
}

function Assert-GoalArbitrationEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $selections = @($events | Where-Object { $_.event -eq "goal_selection" } | Sort-Object decision_id)
    if ($selections.Count -ne 4 -or
        $selections[0].decision_id -ne 1 -or $selections[0].decision_reason -ne "startup" -or
        $selections[0].to_goal -ne "pickup_reward" -or $selections[0].candidate_id -ne 64120 -or
        $selections[1].decision_id -ne 2 -or $selections[1].decision_reason -ne "pickup_complete" -or
        $selections[1].from_goal -ne "pickup_reward" -or $selections[1].to_goal -ne "service" -or
        $selections[2].decision_id -ne 3 -or $selections[2].decision_reason -ne "service_complete" -or
        $selections[2].from_goal -ne "service" -or $selections[2].to_goal -ne "hunt" -or
        $selections[3].decision_id -ne 4 -or $selections[3].decision_reason -ne "hunt_deadline" -or
        $selections[3].from_goal -ne "hunt" -or $selections[3].to_goal -ne "hunt") {
        throw "Goal arbitration did not select the expected pickup, service, and hunt sequence."
    }

    $initialCandidates = @($events | Where-Object { $_.event -eq "goal_candidate" -and $_.decision_id -eq 1 })
    $initialService = @($initialCandidates | Where-Object { $_.goal -eq "service" -and $_.feasible -and $_.utility -gt 300 })
    $initialPickup = @($initialCandidates | Where-Object {
        $_.goal -eq "pickup_reward" -and $_.feasible -and $_.candidate_id -eq 64120 -and $_.utility -gt 500
    })
    $initialHunt = @($initialCandidates | Where-Object {
        $_.goal -eq "hunt" -and -not $_.evaluated -and -not $_.feasible -and $_.utility -eq 300 -and
        $_.reason -eq "deferred_lower_utility"
    })
    $cooldown = @($events | Where-Object {
        $_.event -eq "goal_candidate" -and $_.decision_id -eq 2 -and $_.goal -eq "pickup_reward" -and
        -not $_.feasible -and $_.reason -eq "cooldown"
    })
    $settledService = @($events | Where-Object {
        $_.event -eq "goal_candidate" -and $_.decision_id -eq 3 -and $_.goal -eq "service" -and
        -not $_.feasible -and $_.reason -eq "no_service_need"
    })
    $results = @($events | Where-Object { $_.event -eq "goal_result" })
    $pickupResult = @($results | Where-Object {
        $_.decision_id -eq 1 -and $_.goal -eq "pickup_reward" -and $_.result -eq "success"
    })
    $serviceResult = @($results | Where-Object {
        $_.decision_id -eq 2 -and $_.goal -eq "service" -and $_.result -eq "success"
    })
    $huntResult = @($results | Where-Object {
        $_.decision_id -eq 3 -and $_.goal -eq "hunt" -and $_.result -eq "success" -and $_.reason -eq "hunt_deadline"
    })
    $huntStarted = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "hunt_cycle" -and $_.result -eq "started"
    })
    if ($initialService.Count -ne 1 -or $initialPickup.Count -ne 1 -or $initialHunt.Count -ne 1 -or
        $cooldown.Count -ne 1 -or $settledService.Count -ne 1 -or $pickupResult.Count -ne 1 -or
        $serviceResult.Count -ne 1 -or $huntResult.Count -ne 1 -or $huntStarted.Count -ne 2) {
        throw "Goal arbitration candidate, cooldown, result, or hunt evidence was incomplete."
    }
    if (@($events | Where-Object { $_.event -eq "terminal" }).Count -ne 0) {
        throw "The playerbot emitted a terminal event during goal arbitration."
    }
}

function Assert-StaminaProjectionEvents {
    param([string]$Logs, [int]$StaminaMinutes)

    $candidates = @(ConvertFrom-PlayerbotLogs -Logs $Logs | Where-Object {
        $_.event -eq "hunt_region_candidate" -and $_.reachable -and $_.suitable
    })
    if ($candidates.Count -lt 1) {
        throw "Stamina projection emitted no suitable reachable hunt candidate."
    }
    foreach ($candidate in $candidates) {
        $bonusSeconds = [Math]::Min($candidate.available_hunt_seconds,
            [Math]::Max(0, $StaminaMinutes - 2402) * 60.0)
        $multiplier = if ($candidate.available_hunt_seconds -gt 0) {
            1.0 + 0.5 * $bonusSeconds / $candidate.available_hunt_seconds
        } else {
            1.0
        }
        $expected = $candidate.experience_per_minute * $candidate.observed_correction * $multiplier *
            $candidate.available_hunt_seconds / 60.0
        $tolerance = [Math]::Max(0.1, [Math]::Abs($expected) * 0.01)
        if ($candidate.stamina_minutes -ne $StaminaMinutes -or
            [Math]::Abs($candidate.stamina_experience_multiplier - $multiplier) -gt 0.01 -or
            [Math]::Abs($candidate.projected_experience - $expected) -gt $tolerance -or
            [Math]::Abs($candidate.score - $candidate.projected_experience) -gt 0.01) {
            throw "Hunt projection did not apply the weighted stamina multiplier at $StaminaMinutes minutes."
        }
    }
}

function Assert-HuntRegionPlanningEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $scored = @($events | Where-Object {
        $_.event -eq "hunt_region_scan" -and $_.phase -in @("scoring_started", "scored")
    })
    $build = @($scored | Where-Object { $_.cache -eq "build" })
    $hit = @($scored | Where-Object { $_.cache -eq "hit" })
	$cancelled = @($events | Where-Object { $_.event -eq "hunt_region_scan" -and $_.phase -eq "cancelled" })
	$staleRevision = @($events | Where-Object { $_.event -eq "hunt_region_scan" -and $_.phase -eq "stale_revision" })
	$scoringYields = @($events | Where-Object { $_.event -eq "hunt_region_scan" -and $_.phase -eq "scoring_yield" })
    $yields = @($events | Where-Object {
        $_.event -eq "hunt_region_scan" -and $_.phase -eq "reachability_yield" -and
        $_.pathfinding_calls -ge 1 -and $_.batch_pathfinding_calls -le 1 -and $_.yields -ge 1
    })
    $selections = @($events | Where-Object { $_.event -eq "hunt_region_selection" -and $_.result -eq "selected" })
    $selection = if ($selections.Count -gt 0) { $selections[$selections.Count - 1] } else { $null }
    $candidates = @($events | Where-Object { $_.event -eq "hunt_region_candidate" })
    $unreachableBeforeSelection = @($candidates | Where-Object {
        $_.rejection_reason -eq "unreachable" -and $selection -and $_.region_id -lt $selection.region_id
    })
    $outsideLocalFixture = @($candidates | Where-Object {
        [Math]::Max([Math]::Abs($_.center.x - 32105), [Math]::Abs($_.center.y - 32195)) -gt 32
    })
    $completed = @($events | Where-Object {
        $_.event -eq "hunt_region_scan" -and $_.phase -eq "selected" -and $_.decision_latency_us -gt 0 -and
        $_.expanded_nodes -ge 0
    })
    $selectedCandidate = if ($selection) { @($candidates | Where-Object { $_.region_id -eq $selection.region_id }) } else { @() }
    $reachableCandidates = @($candidates | Where-Object { $_.suitable -and $_.reachable })
    $bestScore = if ($reachableCandidates.Count -gt 0) { ($reachableCandidates | Measure-Object -Property score -Maximum).Maximum } else { $null }
    $nodeBudget = @($candidates | Where-Object { $_.rejection_reason -eq "navigation_node_budget" })
    $nodeBudgetMisclassified = @($nodeBudget | Where-Object {
        $regionId = $_.region_id
        @($candidates | Where-Object { $_.region_id -eq $regionId -and $_.rejection_reason -eq "unreachable" }).Count -gt 0
    })
    $tooManyPathCalls = @($events | Where-Object {
        $_.event -eq "hunt_region_scan" -and $_.phase -eq "reachability_yield" -and $_.batch_pathfinding_calls -gt 1
    })
	if ($build.Count -lt 2 -or $hit.Count -lt 1 -or $cancelled.Count -ne 1 -or $staleRevision.Count -ne 1 -or $scoringYields.Count -lt 1 -or
        $yields.Count -lt 1 -or $tooManyPathCalls.Count -ne 0 -or -not $selection -or $selectedCandidate.Count -ne 1 -or
        $bestScore -eq $null -or [Math]::Abs($selectedCandidate[0].score - $bestScore) -gt 0.01 -or
        $nodeBudget.Count -lt 1 -or $nodeBudgetMisclassified.Count -ne 0 -or
        $unreachableBeforeSelection.Count -lt 1 -or $outsideLocalFixture.Count -lt 1 -or $completed.Count -lt 1) {
		throw "Hunt planning telemetry was incomplete. build=$($build.Count), hit=$($hit.Count), cancelled=$($cancelled.Count), stale=$($staleRevision.Count), scoring_yields=$($scoringYields.Count), yields=$($yields.Count), over_budget=$($tooManyPathCalls.Count), selection=$($selection.Count), fallback=$($unreachableBeforeSelection.Count), outside=$($outsideLocalFixture.Count), completed=$($completed.Count)."
    }
}

function Assert-AdaptiveChallengeEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $frontier = @($events | Where-Object { $_.event -eq "hunt_challenge_frontier" })
	$idle = @($frontier | Where-Object { $_.result -eq "insufficient_active_combat" -and $_.active_combat_seconds -eq 0 -and $_.kills -eq 0 })
	$noKill = @($frontier | Where-Object { $_.result -eq "insufficient_active_combat" -and $_.active_combat_seconds -ge 30 -and $_.kills -eq 0 })
	$escalated = @($frontier | Where-Object { $_.result -eq "escalated" })
	$invalidEscalation = @($escalated | Where-Object {
		$_.active_combat_seconds -lt $_.minimum_active_combat_seconds -or $_.kills -lt $_.minimum_kills
	})
    $backoff = @($frontier | Where-Object { $_.result -eq "backoff" })
	$deathBackoff = @($backoff | Where-Object { $_.death })
    $hold = @($frontier | Where-Object { $_.result -eq "hold" })
    $fixture = @($events | Where-Object { $_.event -eq "adaptive_challenge_fixture" })
    $candidates = @($events | Where-Object {
        $_.event -eq "hunt_region_candidate" -and $_.recovery -and $_.challenge_frontier -ge 0.1 -and
        $_.challenge_band_minimum -lt $_.challenge_band_maximum -and $_.predicted_lethal -ne $null
    })
	$unsafeLethalRecovery = @($candidates | Where-Object { $_.recovery.available_before_lethal -ne 0 })
	$exhausted = @($events | Where-Object {
		$_.event -eq "hunt_scope_exhausted" -and $_.reason -eq "local_scope_exhausted" -and
		$_.retry_delay_ms -eq 1000 -and $_.maximum_attempts -eq 3
	})
	$terminal = @($events | Where-Object { $_.event -eq "terminal" -and $_.reason -eq "hunt_scope_exhausted" })
	if ($idle.Count -ne 1 -or $noKill.Count -ne 1 -or $invalidEscalation.Count -ne 0 -or
		$escalated.Count -ne 3 -or $backoff.Count -ne 2 -or $deathBackoff.Count -ne 1 -or $hold.Count -ne 2 -or
        [Math]::Abs($escalated[0].frontier_before - 0.20) -gt 0.001 -or
        [Math]::Abs($escalated[1].frontier_after - 0.25) -gt 0.001 -or
		[Math]::Abs($escalated[2].frontier_after - 0.225) -gt 0.001 -or
        [Math]::Abs($backoff[0].frontier_after - 0.20) -gt 0.001 -or
		[Math]::Abs($deathBackoff[0].frontier_after - 0.175) -gt 0.001 -or
        $hold[0].hold_qualifying_hunts -ne 1 -or $hold[1].hold_qualifying_hunts -ne 0 -or
        $fixture.Count -ne 1 -or -not $fixture[0].recovery_spell_legal -or $fixture[0].recovery_spell_casts -lt 1 -or
        $fixture[0].equipment_pressure_after -gt $fixture[0].equipment_pressure_before -or
		$fixture[0].idle_observed_seconds -ne 0 -or $fixture[0].active_observed_seconds -ne 30 -or
		-not $fixture[0].in_band_outranks_easier -or -not $fixture[0].wounded_lethal -or
		-not $fixture[0].zero_health_lethal -or -not $fixture[0].helper_scope_exhausted -or
		$candidates.Count -lt 1 -or $unsafeLethalRecovery.Count -ne 0 -or $exhausted.Count -ne 3 -or
		@($exhausted | Where-Object { $_.attempt -notin @(1, 2, 3) }).Count -ne 0 -or $terminal.Count -ne 1) {
		throw "Adaptive challenge evidence was incomplete. idle=$($idle.Count), noKill=$($noKill.Count), invalidEscalation=$($invalidEscalation.Count), escalated=$($escalated.Count), backoff=$($backoff.Count), hold=$($hold.Count), fixture=$($fixture.Count), candidates=$($candidates.Count), exhausted=$($exhausted.Count), terminal=$($terminal.Count)."
    }
}

function Assert-CombatReadinessEvents {
    param([string]$Logs, [string]$Mode)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	if ($Mode -eq "no_food") {
		$service = @($events | Where-Object { $_.selected_recovery -eq "service" -or $_.action -eq "buy_meat" })
		$serviceCandidates = @($events | Where-Object {
			$_.event -eq "goal_candidate" -and $_.goal -eq "service" -and -not $_.feasible -and
			$_.reason -eq "no_service_need" -and $_.food_count -eq 0 -and $_.food_gap -eq 2 -and $_.food_utility -gt 0
		})
		if ($service.Count -ne 0 -or $serviceCandidates.Count -lt 1) { throw "Missing optional food made service feasible." }
		return
	}
	if ($Mode -eq "supplies") {
		$service = @($events | Where-Object { $_.event -eq "combat_readiness" -and $_.selected_recovery -eq "service" })
		$potions = @($events | Where-Object {
			$_.action -eq "buy_potions" -and $_.result -eq "success" -and $_.count -eq 9
		})
		$food = @($events | Where-Object { $_.action -eq "buy_meat" -and $_.result -eq "success" })
		if ($service.Count -lt 1 -or $potions.Count -lt 1 -or $food.Count -ne 0) { throw "Missing healing supplies did not select potion-only service recovery." }
		return
	}
	$readiness = @($events | Where-Object {
        $_.event -eq "combat_readiness" -and $_.vocation_id -eq 4 -and $_.requirements.Count -eq 5
    })
    if ($readiness.Count -lt 1) {
        throw "Combat readiness emitted no complete Knight requirement evidence for $Mode."
    }
    $latest = $readiness[-1]
    if ($Mode -eq "missing_weapon") {
        $terminal = @($events | Where-Object {
            $_.event -eq "terminal" -and $_.reason -eq "combat_readiness_missing_legal_melee_weapon"
        })
        $hunts = @($events | Where-Object { $_.action -eq "hunt_cycle" -or $_.reason -eq "visible_monster" })
        if ($terminal.Count -ne 1 -or $hunts.Count -ne 0 -or $latest.terminal_reason -ne "missing_legal_melee_weapon") {
            throw "Missing weapon did not produce the terminal no-naked-hunt state."
        }
        return
    }
	$ready = @($readiness | Where-Object {
		$_.result -eq "ready" -and @($_.requirements | Where-Object { $_.required -ne $false -and -not $_.ready }).Count -eq 0
	})
    if ($ready.Count -lt 1) {
        throw "Combat readiness did not reach a fully evidenced ready state for $Mode."
    }
    if ($Mode -eq "upgrade") {
        $equip = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "equip_readiness" -and $_.result -eq "success" -and $_.item_id -eq 2384 })
        if ($equip.Count -ne 1) { throw "Carried legal weapon was not equipped and verified." }
    }
	if ($Mode -eq "food_capacity") {
		$capacity = @($latest.requirements | Where-Object {
			$_.name -eq "free_capacity" -and $_.ready -and $_.current -lt $_.minimum -and
			$_.reclaimable_food -ge 3200 -and $_.effective -ge $_.minimum
		})
		$food = @($latest.requirements | Where-Object {
			$_.name -eq "food" -and $_.count -eq 8 -and $_.reclaimable_weight -ge 3200
		})
		$service = @($events | Where-Object { $_.selected_recovery -eq "service" })
		$hunts = @($events | Where-Object { $_.action -eq "hunt_cycle" -and $_.result -eq "started" })
		$eaten = @($events | Where-Object { $_.action -eq "eat" -and $_.result -eq "success" -and $_.item_id -eq 2696 })
		if ($capacity.Count -ne 1 -or $food.Count -ne 1 -or $service.Count -ne 0 -or $hunts.Count -lt 1 -or $eaten.Count -ne 6) {
			throw "Food weight did not remain reclaimable when physical capacity was exhausted."
		}
	}
    if ($Mode -eq "retention") {
        $depositedUnknown = @($events | Where-Object { $_.action -eq "deposit" -and $_.item_id -eq 2050 })
        if ($depositedUnknown.Count -ne 0) { throw "Depot policy deposited an unknown retained item." }
    }
}

function Assert-EquipmentOfferEvents {
    param([string]$Logs, [string]$Mode)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $shadow = @($events | Where-Object { $_.event -eq "equipment_offer_shadow" })
    $candidates = @($events | Where-Object { $_.event -eq "equipment_offer_candidate" })
    $purchases = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "buy_equipment" })
    $equipmentMoves = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "equip_equipment" })
    if ($shadow.Count -ne 1 -or $candidates.Count -lt 1 -or $purchases.Count -ne 0 -or $equipmentMoves.Count -ne 0) {
        throw "Equipment shadow telemetry was incomplete or mutated player state. shadow=$($shadow.Count), candidates=$($candidates.Count), purchases=$($purchases.Count), equipmentMoves=$($equipmentMoves.Count)."
    }
    if ($Mode -eq "upgrade") {
        $selected = @($candidates | Where-Object {
            $_.result -eq "feasible" -and $_.npc_id -eq $shadow[0].npc_id -and $_.item_id -eq $shadow[0].item_id
        })
        if ($shadow[0].result -ne "would_buy" -or $selected.Count -ne 1 -or $selected[0].replaced_item_id -ne 2382 -or
            -not $selected[0].current -or -not $selected[0].candidate -or $selected[0].rule -notin @("pareto_improvement", "unlocks_suitable_hunt")) {
            throw "Equipment shadow did not select a loaded strict weapon improvement."
        }
    } elseif ($Mode -eq "unaffordable") {
        $unaffordable = @($candidates | Where-Object { $_.result -eq "rejected" -and $_.reason -eq "unaffordable_after_reserves" })
        if ($shadow[0].result -ne "no_decision" -or $unaffordable.Count -lt 1) {
            throw "Equipment shadow did not preserve the supply reserve before evaluating a purchase."
        }
    } else {
        $nonImproving = @($candidates | Where-Object { $_.result -eq "rejected" -and $_.reason -eq "non_improving" })
        $illegal = @($candidates | Where-Object { $_.result -eq "rejected" -and $_.reason -eq "unsupported_weapon_type" })
        $affordable = @($candidates | Where-Object { $_.reason -eq "unaffordable_after_reserves" })
        if ($shadow[0].result -ne "no_decision" -or $nonImproving.Count -lt 1 -or $illegal.Count -lt 1 -or $affordable.Count -ne 0) {
            throw "Equipment shadow did not abstain from non-improving or two-handed tradeoff offers."
        }
    }
}

function Assert-EquipmentPurchaseEvents {
	param([string]$Logs, [switch]$Rejected, [switch]$Restart, [switch]$Resume)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$purchases = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "buy_equipment" -and $_.result -eq "success"
	})
	$equips = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "equip_equipment" -and $_.result -eq "success"
	})
	$results = @($events | Where-Object {
		$_.event -eq "goal_result" -and $_.goal -eq "buy_equipment"
	})
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	if ($Restart) {
		$online = @($events | Where-Object {
			$_.event -eq "lifecycle" -and $_.status -eq "online" -and -not $_.recovered -and $_.objective -eq "service"
		})
		if ($online.Count -ne 1 -or $purchases.Count -ne 0 -or $equips.Count -ne 0 -or $terminal.Count -ne 0) {
			throw "Equipment purchase restart reconstruction failed. online=$($online.Count), purchases=$($purchases.Count), equips=$($equips.Count), terminal=$($terminal.Count)."
		}
		return
	}
	if ($Rejected) {
		$fallback = @($events | Where-Object {
			$_.event -eq "goal_selection" -and $_.decision_reason -eq "equipment_purchase_failed" -and
			$_.from_goal -eq "buy_equipment" -and $_.to_goal -ne "buy_equipment"
		})
		if ($results.Count -ne 1 -or $results[0].result -ne "failed" -or
			$results[0].reason -ne "transaction_rejected" -or $fallback.Count -ne 1 -or
			$purchases.Count -ne 0 -or $equips.Count -ne 0 -or $terminal.Count -ne 0) {
			throw "Rejected equipment transaction did not preserve state and return to a valid goal."
		}
		return
	}
	if ($Resume) {
		$selections = @($events | Where-Object {
			$_.event -eq "strategy_selection" -and $_.goal -eq "buy_equipment" -and
			$_.item_id -eq 2379 -and $_.acquisition -eq "carried"
		})
		if ($selections.Count -ne 1 -or $purchases.Count -ne 0 -or $equips.Count -ne 1 -or
			$results.Count -ne 1 -or $results[0].result -ne "success" -or $terminal.Count -ne 0) {
			throw "Persisted equipment purchase state was not reconstructed as an equip-only goal."
		}
		return
	}
	$selections = @($events | Where-Object {
		$_.event -eq "goal_selection" -and $_.to_goal -eq "buy_equipment" -and $_.item_id -eq 2379 -and $_.price -eq 5
	})
	if ($selections.Count -ne 1 -or $purchases.Count -ne 1 -or $equips.Count -ne 1 -or
		$results.Count -ne 1 -or $results[0].result -ne "success" -or
		$purchases[0].carried_before -ne 110 -or $purchases[0].carried_after -ne 105 -or
		$purchases[0].bank_before -ne 100 -or $purchases[0].bank_after -ne 100 -or
		-not $equips[0].combat_ready -or -not $equips[0].displaced_items_preserved -or $terminal.Count -ne 0) {
		throw "Justified equipment purchase was not selected, paid, equipped, and verified exactly once."
	}
}

function Assert-GoalArbitrationInterruptEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $result = @($events | Where-Object {
        $_.event -eq "goal_result" -and $_.decision_id -eq 1 -and $_.goal -eq "pickup_reward" -and
        $_.result -eq "interrupted" -and $_.reason -eq "healing_supply_missing"
    })
    $criticalService = @($events | Where-Object {
        $_.event -eq "goal_candidate" -and $_.decision_id -eq 2 -and $_.goal -eq "service" -and
        $_.feasible -and $_.utility -eq 1000 -and $_.reason -eq "critical_healing"
    })
    $selection = @($events | Where-Object {
        $_.event -eq "goal_selection" -and $_.decision_id -eq 2 -and $_.decision_reason -eq "pickup_interrupted" -and
        $_.from_goal -eq "pickup_reward" -and $_.to_goal -eq "service"
    })
    $claim = @($events | Where-Object { $_.action -eq "claim_reward" })
    if ($result.Count -ne 1 -or $criticalService.Count -ne 1 -or $selection.Count -ne 1 -or $claim.Count -ne 0) {
        throw "Critical healing did not interrupt pickup and force service before reward claim."
    }
}

function Assert-NavigationEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $waypoints = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "hunt_waypoint" -and $_.result -eq "reached"
    })
    $actual = @($waypoints[0..4] | ForEach-Object { "$($_.position.x),$($_.position.y),$($_.position.z)" })
    $expected = @("32084,32144,5", "32103,32124,8", "32117,32090,9", "32103,32124,8", "32084,32144,5")
    if (($actual -join '|') -ne ($expected -join '|')) {
        throw "Unexpected hunting waypoint sequence: $($actual -join ' -> ')"
    }
    $terminal = @($events | Where-Object { $_.event -eq "terminal" })
    $blockedRecovery = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "navigate" -and
        $_.result -eq "failed" -and $_.reason -eq "step_result_mismatch"
    })
    if ($blockedRecovery.Count -lt 1) {
        throw "The full navigation test did not exercise temporary blockage recovery."
    }
    if ($terminal.Count -ne 0) {
        throw "The playerbot emitted a terminal event during full navigation."
    }
}

function Assert-NavigationRecoveryEvents {
	param([string]$Logs)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$mismatches = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "navigate" -and
		$_.result -eq "failed" -and $_.reason -eq "step_result_mismatch"
	})
	$recovery = @($events | Where-Object {
		$_.event -eq "hunt_region_patrol" -and $_.result -eq "skipped" -and
		$_.reason -eq "repeated_step_failure" -and $_.step_failures -eq 3 -and $_.region_id -eq $null
	})
	$waypoints = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "hunt_waypoint" -and $_.result -eq "reached"
	})
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	$firstWaypoint = if ($waypoints.Count -gt 0) {
		"$($waypoints[0].position.x),$($waypoints[0].position.y),$($waypoints[0].position.z)"
	} else { "" }
	if ($mismatches.Count -lt 1 -or $recovery.Count -ne 1 -or $firstWaypoint -ne "32103,32124,8" -or
		$terminal.Count -ne 0) {
		throw "Repeated route execution recovery failed. mismatches=$($mismatches.Count), recovery=$($recovery.Count), firstWaypoint=$firstWaypoint, terminal=$($terminal.Count)."
	}
}

function Assert-PatrolRecoveryEvents {
	param([string]$Logs)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$routeFailures = @($events | Where-Object {
		$_.event -eq "navigation_progress" -and $_.result -eq "failed" -and
		$_.reason -eq "route_unavailable"
	})
	$skipped = @($events | Where-Object {
		$_.event -eq "hunt_region_patrol" -and $_.result -eq "skipped" -and
		$_.reason -eq "route_unavailable" -and $_.route_failures -eq 3 -and
		$_.expanded_nodes -le 300000 -and $_.elapsed_ms -le 3000
	})
	$continued = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "plan" -and $_.result -eq "success"
	})
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	$failureDestinations = @($routeFailures | ForEach-Object {
		"$($_.destination.x),$($_.destination.y),$($_.destination.z)"
	} | Sort-Object -Unique)
	$skippedDestination = $skipped.Count -eq 1 ?
		"$($skipped[0].destination.x),$($skipped[0].destination.y),$($skipped[0].destination.z)" : ""
	$continuedDestination = $continued.Count -ge 1 ?
		"$($continued[0].destination.x),$($continued[0].destination.y),$($continued[0].destination.z)" : ""
	if ($routeFailures.Count -ne 3 -or $failureDestinations.Count -ne 1 -or
		$skipped.Count -ne 1 -or $skipped[0].expanded_nodes -ne 300000 -or
		$failureDestinations[0] -ne $skippedDestination -or $continuedDestination -eq $skippedDestination -or
		$continued.Count -lt 1 -or $terminal.Count -ne 0) {
		throw "Patrol route recovery was not bounded. failures=$($routeFailures.Count), skipped=$($skipped.Count), continued=$($continued.Count), terminal=$($terminal.Count)."
	}
}

function Assert-TargetPursuitEvents {
	param([string]$Logs)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$started = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "target_pursuit" -and $_.result -eq "started"
	})
	$reacquired = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "target_pursuit" -and $_.result -eq "reacquired"
	})
	$defeated = @($events | Where-Object {
		$_.event -eq "target_changed" -and $_.reason -eq "target_defeated"
	})
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	$firstPlan = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "plan" -and $_.result -eq "success"
	}) | Select-Object -First 1
	$lastSeenPlanDistance = $started.Count -eq 1 -and $firstPlan.Count -eq 1 ?
		[Math]::Max([Math]::Abs($started[0].last_seen_position.x - $firstPlan[0].destination.x),
			[Math]::Abs($started[0].last_seen_position.y - $firstPlan[0].destination.y)) : 99
	$distance = $started.Count -eq 1 -and $reacquired.Count -eq 1 ?
		[Math]::Max([Math]::Abs($started[0].position.x - $reacquired[0].position.x),
			[Math]::Abs($started[0].position.y - $reacquired[0].position.y)) : 0
	if ($started.Count -ne 1 -or $reacquired.Count -ne 1 -or $defeated.Count -lt 1 -or
		$started[0].target_id -ne $reacquired[0].target_id -or
		$reacquired[0].target_id -ne $defeated[0].previous_target_id -or
		$distance -lt 1 -or $distance -gt 6 -or $lastSeenPlanDistance -gt 1 -or $terminal.Count -ne 0) {
		throw "Target pursuit failed. started=$($started.Count), reacquired=$($reacquired.Count), defeated=$($defeated.Count), distance=$distance, lastSeenPlanDistance=$lastSeenPlanDistance, terminal=$($terminal.Count)."
	}
}

function Assert-TargetPursuitAbandonEvents {
	param([string]$Logs)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$started = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "target_pursuit" -and $_.result -eq "started"
	})
	$abandoned = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "target_pursuit" -and $_.result -eq "abandoned" -and
		$_.reason -in @("last_seen_position_reached", "pursuit_budget_exhausted")
	})
	$reacquired = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "target_pursuit" -and $_.result -eq "reacquired"
	})
	$routeUnavailable = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "navigate" -and $_.result -eq "failed" -and
		$_.reason -eq "route_unavailable"
	})
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	$firstPlan = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "plan" -and $_.result -eq "success"
	}) | Select-Object -First 1
	$lastSeenPlanDistance = $started.Count -eq 1 -and $firstPlan.Count -eq 1 ?
		[Math]::Max([Math]::Abs($started[0].last_seen_position.x - $firstPlan[0].destination.x),
			[Math]::Abs($started[0].last_seen_position.y - $firstPlan[0].destination.y)) : 99
	$distance = $started.Count -eq 1 -and $abandoned.Count -eq 1 ?
		[Math]::Max([Math]::Abs($started[0].position.x - $abandoned[0].position.x),
			[Math]::Abs($started[0].position.y - $abandoned[0].position.y)) : 0
	if ($started.Count -ne 1 -or $abandoned.Count -ne 1 -or
		$started[0].target_id -ne $abandoned[0].target_id -or $distance -gt 6 -or
		($distance -eq 0 -and $routeUnavailable.Count -lt 1) -or $lastSeenPlanDistance -gt 1 -or
		$reacquired.Count -ne 0 -or $terminal.Count -ne 0) {
		throw "Target pursuit fallback failed. started=$($started.Count), abandoned=$($abandoned.Count), distance=$distance, routeUnavailable=$($routeUnavailable.Count), lastSeenPlanDistance=$lastSeenPlanDistance, reacquired=$($reacquired.Count), terminal=$($terminal.Count)."
	}
}

function Assert-CorpseEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $nonlootableTarget = @($events | Where-Object {
        $_.event -eq "target_changed" -and $_.target_name -eq "Playerbot Nonlootable Corpse"
    })
    $nonlootableResult = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "loot" -and
        $_.result -eq "skipped" -and $_.reason -eq "corpse_not_lootable" -and
        $_.expected_corpse_item_id -eq 2148
    })
    $containerDeathItemTarget = @($events | Where-Object {
        $_.event -eq "target_changed" -and $_.target_name -eq "Playerbot Container Death Item"
    })
    $containerDeathItemResult = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "loot" -and
        $_.result -eq "skipped" -and $_.reason -eq "corpse_not_lootable" -and
        $_.expected_corpse_item_id -eq 1987
    })
    $emptyTarget = @($events | Where-Object {
        $_.event -eq "target_changed" -and $_.target_name -eq "Playerbot Empty Corpse"
    })
    $emptyResult = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "loot" -and
        $_.result -eq "skipped" -and $_.reason -eq "corpse_empty"
    })
    $lootTarget = @($events | Where-Object {
        $_.event -eq "target_changed" -and $_.target_name -eq "Playerbot Loot Corpse"
    })
    $lootResult = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "loot" -and
        $_.result -eq "success" -and $_.item_id -eq 2148
    })
    $falseUnavailable = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "loot" -and
        $_.result -eq "failed" -and $_.reason -eq "owned_corpse_unavailable"
    })
    $terminal = @($events | Where-Object { $_.event -eq "terminal" })

    if ($nonlootableTarget.Count -lt 1 -or $nonlootableResult.Count -ne 1) {
        throw "The non-lootable corpse was not skipped exactly once."
    }
    if ($containerDeathItemTarget.Count -lt 1 -or $containerDeathItemResult.Count -ne 1) {
        throw "The container-only death item was not skipped exactly once."
    }
    if ($emptyTarget.Count -lt 1 -or $emptyResult.Count -ne 1) {
        throw "The empty corpse was not opened and classified exactly once."
    }
    if ($lootTarget.Count -lt 1 -or $lootResult.Count -lt 1) {
        throw "The guaranteed-loot corpse did not preserve normal looting."
    }
    if ($falseUnavailable.Count -ne 0) {
        throw "An available test corpse was reported unavailable."
    }
    if ($terminal.Count -ne 0) {
        throw "The playerbot emitted a terminal event during corpse classification."
    }
}

function Assert-InaccessibleCorpseEvents {
	param([string]$Logs)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$suspended = @($events | Where-Object {
		$_.event -eq "navigation_progress" -and $_.result -eq "suspended" -and
		$_.reason -eq "corpse_route_unchanged"
	})
	$terminalResult = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "loot" -and $_.result -eq "failed" -and
		$_.reason -eq "corpse_inaccessible"
	})
	$combatPreemption = @($events | Where-Object {
		$_.event -eq "target_changed" -and $_.target_name -eq "Playerbot Corpse Blocker" -and
		$_.reason -in @("defensive_path_blocker", "defensive_attacker")
	})
	$blockerEngaged = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "defensive_combat" -and
		$_.result -eq "started" -and
		$combatPreemption.Count -ge 1 -and $_.target_id -eq $combatPreemption[0].target_id
	})
	$controllerTerminal = @($events | Where-Object { $_.event -eq "terminal" })
	if ($suspended.Count -lt 1 -or $combatPreemption.Count -ne 1 -or $blockerEngaged.Count -ne 1 -or
		$terminalResult.Count -ne 1 -or
		$terminalResult[0].target_id -le 0 -or
		$terminalResult[0].navigation_failures -gt 6 -or $terminalResult[0].navigation_suspensions -lt 1 -or
		$terminalResult[0].elapsed_ms -gt 22000 -or $controllerTerminal.Count -ne 0) {
		throw "Inaccessible corpse work was not bounded. suspended=$($suspended.Count), preemption=$($combatPreemption.Count)/$($blockerEngaged.Count), results=$($terminalResult.Count), terminal=$($controllerTerminal.Count)."
	}
}

function Assert-DepotEvents {
	param([string]$Logs, [int]$ExpectedDepositedCount, [int]$ExpectedEquipmentDeposits)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$discovery = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "depot_discover" -and
		$_.result -eq "success" -and $_.locker.x -eq 32352 -and $_.locker.y -eq 32225 -and
		$_.locker.z -eq 7 -and $_.approach.x -eq 32352 -and $_.approach.y -eq 32226 -and $_.approach.z -eq 7 -and
		$_.distance -ge 0 -and $_.route_steps -ge 0 -and $_.expanded_nodes -ge 0 -and $_.indexed -gt $_.in_scope -and
		$_.in_scope -ge 1 -and $_.standable -gt 1
	})
	$depotId = if ($discovery.Count -gt 0) { $discovery[0].depot_id } else { -1 }
	$locker = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "depot_open_locker" -and
		$_.result -eq "requested" -and $_.depot_id -eq $depotId -and $_.container_id -eq 14
	})
	$chest = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "depot_open_chest" -and
		$_.result -eq "requested" -and $_.depot_id -eq $depotId -and $_.container_id -eq 13
	})
	$verified = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "deposit" -and
		$_.result -eq "success" -and $_.policy -eq "known_loot" -and $_.depot_id -eq $depotId -and
		$_.container_id -eq 13 -and $_.item_id -eq 2684 -and $_.verified -gt 0 -and
		$_.inventory_after -eq ($_.inventory_before - $_.verified) -and
		$_.depot_after -eq ($_.depot_before + $_.verified)
	})
	$complete = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "deposit" -and
		$_.result -eq "complete" -and $_.depot_id -eq $depotId -and $_.container_id -eq 13
	})
	$equipmentDeposits = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "deposit" -and $_.result -in @("success", "partial") -and
		$_.item_id -in @(2380, 2382) -and $_.verified -eq 1
	})
	$equipmentUpgrades = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "equip_readiness" -and $_.result -eq "success" -and
		$_.item_id -in @(2389, 2461, 2643)
	})
	$deposited = ($verified | Measure-Object -Property verified -Sum).Sum
	$unsafeMoves = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "deposit" -and
		$_.item_id -in @(2050, 2120, 2554, 2467, 2666, 7618)
	})
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	if ($discovery.Count -lt 1 -or $locker.Count -lt 1 -or $chest.Count -lt 1 -or
		$deposited -ne $ExpectedDepositedCount -or $complete.Count -lt 1 -or
		$equipmentDeposits.Count -ne $ExpectedEquipmentDeposits -or $equipmentUpgrades.Count -ne (3 * [Math]::Min($ExpectedEquipmentDeposits, 1)) -or
		$unsafeMoves.Count -ne 0 -or $terminal.Count -ne 0) {
		throw "Real Thais depot evidence was incomplete. discovery=$($discovery.Count), depotId=$depotId, locker=$($locker.Count), chest=$($chest.Count), deposited=$deposited, equipment_deposits=$($equipmentDeposits.Count), equipment_upgrades=$($equipmentUpgrades.Count), complete=$($complete.Count), unsafe=$($unsafeMoves.Count), terminal=$($terminal.Count)."
	}
}

function Assert-DepotRecoveryEvents {
	param([string]$Logs, [string]$Phase)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$online = @($events | Where-Object {
		$_.event -eq "lifecycle" -and $_.status -eq "online" -and $_.objective -eq "service"
	})
	$complete = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "deposit" -and
		$_.result -eq "complete" -and $_.depot_id -eq 2
	})
	$verified = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "deposit" -and
		$_.result -in @("success", "partial") -and $_.item_id -eq 2684
	})
	$expectedVerified = if ($Phase -eq "depart") { 0 } else { 2 }
	$verifiedCount = ($verified | Measure-Object -Property verified -Sum).Sum
	if ($null -eq $verifiedCount) { $verifiedCount = 0 }
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	if ($online.Count -ne 1 -or $complete.Count -lt 1 -or $verifiedCount -ne $expectedVerified -or
		$terminal.Count -ne 0) {
		throw "Depot $Phase restart recovery failed. online=$($online.Count), complete=$($complete.Count), verified=$verifiedCount, terminal=$($terminal.Count)."
	}
}

function Assert-SlottedLootEvents {
	param([string]$Logs, [switch]$SellerAvailable, [switch]$Restarted)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$sellMoves = @($events | Where-Object {
		$_.action -eq "item_disposition" -and $_.item_id -eq 2398 -and $_.source_slot -eq 10 -and
		$_.disposition -eq "sell" -and $_.provider_available
	})
	$sales = @($events | Where-Object {
		$_.action -eq "sell" -and $_.result -eq "success" -and $_.item_id -eq 2398 -and
		$_.count -eq 1 -and ($_.carried_after + $_.bank_after) -gt ($_.carried_before + $_.bank_before)
	})
	$deposits = @($events | Where-Object {
		$_.action -eq "deposit" -and $_.result -in @("success", "partial") -and $_.item_id -eq 2398 -and
		$_.verified -eq 1 -and $_.source_slot -eq 10 -and $_.disposition -eq "deposit" -and
		$_.provider_available -eq $false
	})
	$depositRequests = @($events | Where-Object {
		$_.action -eq "deposit" -and $_.result -eq "requested" -and $_.item_id -eq 2398 -and
		$_.source_slot -eq 10 -and $_.disposition -eq "deposit" -and $_.provider_available -eq $false
	})
	$protectedMoves = @($events | Where-Object {
		$_.action -in @("sell", "deposit", "item_disposition") -and $_.item_id -eq 2463
	})
	$reselectedService = @($events | Where-Object {
		$_.event -eq "goal_selection" -and $_.decision_reason -eq "service_complete" -and
		$_.to_goal -eq "service" -and $_.reason -eq "sellable_inventory"
	})
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	if ($SellerAvailable) {
		if ($sellMoves.Count -lt 2 -or $sales.Count -ne 1 -or $deposits.Count -ne 0) {
			throw "Slotted seller disposition failed. moves=$($sellMoves.Count), sales=$($sales.Count), deposits=$($deposits.Count)."
		}
	} elseif (($Restarted -and ($depositRequests.Count -ne 1 -or $deposits.Count -ne 0)) -or
		(-not $Restarted -and $deposits.Count -ne 1) -or $sales.Count -ne 0) {
		throw "Slotted no-seller disposition failed. requests=$($depositRequests.Count), deposits=$($deposits.Count), sales=$($sales.Count), restarted=$Restarted."
	}
	if ($protectedMoves.Count -ne 0 -or $reselectedService.Count -ne 0 -or $terminal.Count -ne 0) {
		throw "Slotted disposition did not preserve protected state or bounded service. protected=$($protectedMoves.Count), repeated=$($reselectedService.Count), terminal=$($terminal.Count), restarted=$Restarted."
	}
}

function Assert-MainlandLoopEvents {
	param([string]$Logs, [int]$MinimumCycles = 3, [int]$MinimumDeposits = 2)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$hunts = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "hunt_cycle" -and $_.result -eq "started"
	})
	$deposits = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "deposit" -and $_.result -eq "complete"
	})
	$realDepot = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "depot_discover" -and $_.result -eq "success" -and
		$_.locker.x -eq 32352 -and $_.locker.y -eq 32225 -and $_.locker.z -eq 7 -and
		$_.approach.x -eq 32352 -and $_.approach.y -eq 32226 -and $_.approach.z -eq 7
	})
	$trainingRoomDepot = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "depot_discover" -and $_.result -eq "success" -and
		$_.locker.x -eq 32276 -and $_.locker.y -eq 32218 -and $_.locker.z -eq 11
	})
	$selection = @($events | Where-Object {
		$_.event -eq "hunt_region_selection" -and $_.result -eq "selected" -and
		[Math]::Abs($_.center.x - 32369) + [Math]::Abs($_.center.y - 32241) +
		20 * [Math]::Abs($_.center.z - 7) -le 200
	})
	$cheeseDeposit = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "deposit" -and $_.result -in @("success", "partial") -and
		$_.item_id -eq 2696
	})
	$remoteBuyerDeposit = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "deposit" -and $_.result -in @("success", "partial") -and
		$_.item_id -eq 2826
	})
	$remoteBuyerSale = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "sell" -and $_.result -eq "success" -and $_.item_id -eq 2826
	})
	$rookService = @($events | Where-Object {
		$_.event -eq "npc_reply" -and $_.npc_name -in @("Billy", "Willie", "Lily", "Paulie")
	})
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	if ($hunts.Count -lt $MinimumCycles -or $deposits.Count -lt $MinimumDeposits -or $realDepot.Count -lt 1 -or
		$trainingRoomDepot.Count -ne 0 -or
		$selection.Count -lt 1 -or $cheeseDeposit.Count -lt 1 -or $remoteBuyerDeposit.Count -lt 1 -or
		$remoteBuyerSale.Count -ne 0 -or $rookService.Count -ne 0 -or $terminal.Count -ne 0) {
		throw "Mainland loop failed. hunts=$($hunts.Count), deposits=$($deposits.Count), realDepot=$($realDepot.Count), trainingRoomDepot=$($trainingRoomDepot.Count), localSelections=$($selection.Count), cheeseDeposits=$($cheeseDeposit.Count), rookService=$($rookService.Count), terminal=$($terminal.Count)."
	}
}

function Assert-SpellTrainingEvents {
	param([string]$Logs, [switch]$Restart)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$discovery = @($events | Where-Object { $_.event -eq "spell_trainer_discovered" -and $_.offers -gt 0 -and $_.in_scope })
	$selected = @($events | Where-Object {
		$_.event -eq "goal_selection" -and $_.to_goal -eq "learn_spell" -and $_.spell -eq "Find Person" -and $_.price -eq 80
	})
	$rejected = @($events | Where-Object {
		$_.event -eq "spell_candidate" -and $_.spell -eq "Light" -and $_.result -eq "rejected" -and
		$_.reason -eq "unaffordable_after_reserves"
	})
	$purchase = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "learn_spell" -and $_.result -eq "success" -and
		$_.spell -eq "Find Person" -and $_.price -eq 80 -and $_.money_before -eq 300 -and $_.money_after -eq 220
	})
	$completed = @($events | Where-Object {
		$_.event -eq "goal_result" -and $_.goal -eq "learn_spell" -and $_.result -eq "success"
	})
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	if ($Restart) {
		if ($purchase.Count -ne 1 -or $terminal.Count -ne 0) {
			throw "Spell training restart repeated or failed the completed purchase. purchases=$($purchase.Count), terminal=$($terminal.Count)."
		}
		return
	}
	if ($discovery.Count -lt 1 -or $selected.Count -ne 1 -or $rejected.Count -lt 1 -or $purchase.Count -ne 1 -or
		$completed.Count -ne 1 -or $terminal.Count -ne 0) {
		throw "Spell training failed. discovery=$($discovery.Count), selected=$($selected.Count), rejected=$($rejected.Count), purchases=$($purchase.Count), completed=$($completed.Count), terminal=$($terminal.Count)."
	}
}

function Assert-SpellUseEvents {
	param([string]$Logs)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$casts = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "cast_spell" -and $_.result -eq "success" })
	$healing = @($casts | Where-Object {
		$_.policy_candidate.spell -eq "Light Healing" -and $_.policy_candidate.role -eq "healing" -and $_.need -eq "recovery" -and
		$_.mana_after -eq ($_.mana_before - 20) -and $_.health_after -gt $_.health_before
	})
	$support = @($casts | Where-Object {
		$_.policy_candidate.spell -eq "Haste" -and $_.policy_candidate.role -eq "support" -and $_.need -eq "safe_route" -and
		$_.mana_after -eq ($_.mana_before - 60) -and $_.mana_after -ge $_.mana_reserve -and $_.reserve_survives
	})
	$offense = @($casts | Where-Object {
		$_.policy_candidate.spell -eq "Whirlwind Throw" -and $_.policy_candidate.role -eq "ranged_offense" -and $_.need -eq "offense" -and
		$_.mana_after -eq ($_.mana_before - 40) -and $_.target_id -gt 0
	})
	$unlearned = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "cast_spell" -and $_.result -eq "skipped" -and
		$_.policy_candidate.spell -eq "Light Healing" -and $_.reason -eq "unlearned" -and $_.engine_result -eq "not_attempted" -and
		$_.fallback -eq "small_health_potion" -and @($_.legal_candidates).Count -eq 0
	})
	$fallbackPotion = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "heal" -and $_.result -eq "success" -and
		$_.method -eq "small_health_potion" -and $_.resource_before -eq 6 -and $_.resource_after -eq 5
	})
	$manaFallback = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "cast_spell" -and $_.result -eq "skipped" -and
		$_.policy_candidate.spell -eq "Whirlwind Throw" -and $_.reason -eq "insufficient_mana_reserve" -and
		$_.fallback -eq "normal_melee" -and @($_.legal_candidates).Count -eq 0
	})
	$invalidLegalCandidates = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "cast_spell" -and $_.engine_result -ne "accepted" -and
		@($_.legal_candidates).Count -ne 0
	})
	$failed = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "cast_spell" -and $_.result -eq "failed" })
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	$targetIndex = -1
	$preemptingHealIndex = -1
	$offenseIndex = -1
	for ($index = 0; $index -lt $events.Count; $index++) {
		$event = $events[$index]
		if ($targetIndex -lt 0 -and $event.event -eq "target_changed" -and $event.target_name -eq "Playerbot Spell Target") {
			$targetIndex = $index
		} elseif ($targetIndex -ge 0 -and $preemptingHealIndex -lt 0 -and $event.event -eq "action_result" -and
			$event.action -eq "cast_spell" -and $event.result -eq "success" -and
			$event.policy_candidate.spell -eq "Light Healing" -and $event.need -eq "recovery") {
			$preemptingHealIndex = $index
		} elseif ($preemptingHealIndex -ge 0 -and $offenseIndex -lt 0 -and $event.event -eq "action_result" -and
			$event.action -eq "cast_spell" -and $event.result -eq "success" -and
			$event.policy_candidate.spell -eq "Whirlwind Throw") {
			$offenseIndex = $index
		}
	}
	$preempted = $targetIndex -ge 0 -and $preemptingHealIndex -gt $targetIndex -and $offenseIndex -gt $preemptingHealIndex
	if ($healing.Count -ge 2 -and $support.Count -eq 1 -and $offense.Count -eq 1 -and $unlearned.Count -eq 1 -and
		$fallbackPotion.Count -eq 1 -and $manaFallback.Count -ge 1 -and $preempted -and $invalidLegalCandidates.Count -eq 0 -and
		$failed.Count -eq 0 -and $terminal.Count -eq 0) {
		return
	}
	throw "Spell use failed. healing=$($healing.Count), support=$($support.Count), offense=$($offense.Count), unlearned=$($unlearned.Count), fallbackPotion=$($fallbackPotion.Count), manaFallback=$($manaFallback.Count), preempted=$preempted, invalidLegalCandidates=$($invalidLegalCandidates.Count), failed=$($failed.Count), terminal=$($terminal.Count)."
}

function Assert-SpellCalibrationEvents {
	param([string]$Logs)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$fixture = @($events | Where-Object { $_.event -eq "spell_calibration" })
	$phases = @($fixture | ForEach-Object { $_.phase })
	$required = @("isolated_healing", "healing_equality_exact", "overheal_censored", "concurrent_damage", "single_target_damage", "melee_ambiguous",
		"rejected_cast", "other_attacker_ambiguous", "target_loss_ambiguous", "multi_target_ambiguous", "support_duration", "support_preexisting_or_replaced", "low_confidence", "gradual_ranking", "bounded_range", "fixture_profile_clear")
	$missing = @($required | Where-Object { $_ -notin $phases })
	$acceptedHealing = @($fixture | Where-Object { $_.source -eq "classifier_helper" -and $_.phase -eq "isolated_healing" -and $_.evidence -eq "accepted" -and
		$_.engine_bounds.maximum -lt 10000 -and $_.calibration.accepted -eq 1 })
	$equalityHealing = @($fixture | Where-Object { $_.source -eq "classifier_helper" -and $_.phase -eq "healing_equality_exact" -and $_.evidence -eq "accepted" })
	$censored = @($fixture | Where-Object { $_.source -eq "classifier_helper" -and $_.phase -eq "overheal_censored" -and $_.evidence -eq "censored_overheal" })
	$concurrent = @($fixture | Where-Object { $_.source -eq "classifier_helper" -and $_.phase -eq "concurrent_damage" -and $_.evidence -eq "concurrent_damage" })
	$damage = @($fixture | Where-Object { $_.source -eq "classifier_helper" -and $_.phase -eq "single_target_damage" -and $_.evidence -eq "accepted" -and $_.target_class -eq "monster:fixture" })
	$ambiguous = @($fixture | Where-Object { $_.source -eq "classifier_helper" -and $_.phase -in @("melee_ambiguous", "other_attacker_ambiguous", "target_loss_ambiguous") -and $_.calibration.ambiguous -gt 0 })
	$multiTarget = @($fixture | Where-Object { $_.source -eq "classifier_helper" -and $_.phase -eq "multi_target_ambiguous" -and $_.evidence -eq "multi_target" -and $_.calibration.accepted -eq 0 })
	$support = @($fixture | Where-Object { $_.phase -eq "support_duration" -and $_.calibration.conservative -eq 33000 -and $_.engine_bounds.duration_ms -eq 33000 })
	$supportRejected = @($fixture | Where-Object { $_.phase -eq "support_preexisting_or_replaced" -and $_.evidence -eq "preexisting_or_replaced_condition" -and $_.calibration.ambiguous -eq 1 })
	$lowConfidence = @($fixture | Where-Object { $_.source -eq "profile_math" -and $_.phase -eq "low_confidence" -and $_.calibration.confidence -lt 1 -and $_.policy_unchanged })
	$adjusted = @($fixture | Where-Object { $_.source -eq "profile_math" -and $_.phase -eq "gradual_ranking" -and $_.calibration.confidence -eq 1 -and
		$_.calibration.ranking -gt $_.engine_bounds.minimum -and $_.calibration.maximum -le 60000 })
	$bounded = @($fixture | Where-Object { $_.source -eq "profile_math" -and $_.phase -eq "bounded_range" -and $_.calibration.maximum -eq 60000 })
	$fixtureClear = @($fixture | Where-Object { $_.source -eq "profile_math" -and $_.phase -eq "fixture_profile_clear" -and $_.profiles_before -eq 12 -and $_.profiles_after -eq 0 })
	$evicted = @($events | Where-Object { $_.event -eq "spell_calibration_eviction" -and $_.source -eq "profile_math" -and $_.profile_count -eq 12 })
	$engineHealing = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "cast_spell" -and $_.result -eq "success" -and
		$_.policy_candidate.spell -eq "Light Healing" -and $_.reason -eq "accepted" -and $_.calibration.accepted -eq 1 })
	$engineHaste = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "cast_spell" -and $_.result -eq "success" -and
		$_.policy_candidate.spell -eq "Haste" -and $_.reason -eq "accepted" -and $_.haste_ticks_after_cast -gt 0 -and
		$_.haste_ticks_observed -gt 0 -and $_.haste_duration_measured -ge 32000 -and $_.haste_duration_measured -le 34000 })
	$engineBerserkSingle = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "cast_spell" -and $_.result -eq "success" -and
		$_.policy_candidate.spell -eq "Berserk" -and $_.reason -eq "accepted" -and $_.spell_victim_count -eq 1 -and
		-not $_.spell_victim_overflow -and $_.target_class -eq "monster:Playerbot Spell Target" -and $_.calibration.accepted -eq 1 -and $_.calibration.confidence -lt 1 })
	$engineBerserkMulti = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "cast_spell" -and $_.result -eq "success" -and
		$_.policy_candidate.spell -eq "Berserk" -and $_.reason -eq "multi_target" -and $_.spell_victim_count -gt 1 -and
		-not $_.spell_victim_overflow -and $_.target_class -eq "monster:Playerbot Spell Target" -and $_.calibration.ambiguous -ge 1 })
	$firstOffensiveRequest = $null
	foreach ($event in $events) {
		if ($event.event -eq "action_result" -and $event.action -eq "cast_spell" -and $event.result -eq "requested" -and $event.need -eq "offense") {
			$firstOffensiveRequest = $event
			break
		}
	}
	$defaultBerserk = $null -ne $firstOffensiveRequest -and $firstOffensiveRequest.policy_candidate.spell -eq "Berserk" -and
		$firstOffensiveRequest.calibration.confidence -eq 0
	$actualLowConfidence = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "cast_spell" -and $_.result -eq "requested" -and
		$_.policy_candidate.spell -eq "Light Healing" -and $_.calibration.confidence -eq 0 })
	if ($missing.Count -eq 0 -and $acceptedHealing.Count -eq 1 -and $equalityHealing.Count -eq 1 -and $censored.Count -eq 1 -and $concurrent.Count -eq 1 -and
		$damage.Count -eq 1 -and $ambiguous.Count -eq 3 -and $multiTarget.Count -eq 1 -and $support.Count -eq 1 -and $supportRejected.Count -eq 1 -and $lowConfidence.Count -eq 1 -and
		$adjusted.Count -ge 1 -and $bounded.Count -eq 1 -and $fixtureClear.Count -eq 1 -and $evicted.Count -ge 1 -and
		$engineHealing.Count -ge 1 -and $engineHaste.Count -ge 1 -and $engineBerserkSingle.Count -eq 1 -and $engineBerserkMulti.Count -eq 1 -and
		$defaultBerserk -and $actualLowConfidence.Count -ge 1) { return }
	throw "Spell calibration telemetry was incomplete. missing=$($missing -join ','), acceptedHealing=$($acceptedHealing.Count), equalityHealing=$($equalityHealing.Count), censored=$($censored.Count), concurrent=$($concurrent.Count), damage=$($damage.Count), ambiguous=$($ambiguous.Count), multiTarget=$($multiTarget.Count), support=$($support.Count), supportRejected=$($supportRejected.Count), lowConfidence=$($lowConfidence.Count), adjusted=$($adjusted.Count), bounded=$($bounded.Count), fixtureClear=$($fixtureClear.Count), evicted=$($evicted.Count), engineHealing=$($engineHealing.Count), engineHaste=$($engineHaste.Count), engineBerserkSingle=$($engineBerserkSingle.Count), engineBerserkMulti=$($engineBerserkMulti.Count), defaultBerserk=$defaultBerserk, actualLowConfidence=$($actualLowConfidence.Count)."
}

function Assert-MagicTrainingEvents {
	param([string]$Logs, [string]$Mode, [string]$Spell)
	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$actions = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "magic_training" })
	$requests = @($actions | Where-Object { $_.result -eq "requested" })
	$verified = @($actions | Where-Object { $_.source -eq "engine_verification" })
	$candidates = @($events | Where-Object { $_.event -eq "goal_candidate" -and $_.goal -eq "magic_training" })
	if ($Mode -eq "cast") {
		$success = @($verified | Where-Object { $_.result -eq "success" -and $_.spell -eq $Spell -and $_.mana_delta -eq $_.mana_cost -and $_.mana_spent_after -gt $_.mana_spent_before -and $_.mana_after -ge 20 -and $_.wasted_mana -gt 0 })
		$goal = @($events | Where-Object { $_.event -eq "goal_result" -and $_.goal -eq "magic_training" -and $_.result -eq "success" -and $_.reason -eq "cast_verified" })
		if ($requests.Count -eq 1 -and $success.Count -eq 1 -and $goal.Count -eq 1) { return }
		throw "Magic training $Spell did not make exactly one verified overflow cast. requests=$($requests.Count), success=$($success.Count), goal=$($goal.Count)."
	}
	if ($Mode -eq "failed") {
		$failed = @($verified | Where-Object { $_.result -eq "failed" -and $_.spell -eq "Light" })
		$goal = @($events | Where-Object { $_.event -eq "goal_result" -and $_.goal -eq "magic_training" -and $_.result -eq "failed" -and $_.reason -eq "cast_verification_failed" })
		$continued = @($events | Where-Object { $_.event -eq "goal_selection" -and $_.decision_reason -eq "magic_training_complete" -and $_.from_goal -eq "magic_training" })
		if ($requests.Count -eq 1 -and $failed.Count -eq 1 -and $goal.Count -eq 1 -and $continued.Count -eq 1) { return }
		throw "Failed magic-training verification retried or did not reselect."
	}
	if ($Mode -eq "post_hunt") {
		$idle = @($events | Where-Object { $_.event -eq "objective_transition" -and $_.from -eq "hunt" -and $_.to -eq "idle" })
		$selection = @($events | Where-Object { $_.event -eq "goal_selection" -and $_.decision_reason -eq "hunt_deadline" -and $_.to_goal -eq "magic_training" })
		$continued = @($events | Where-Object { $_.event -eq "goal_selection" -and $_.decision_reason -eq "magic_training_complete" -and $_.from_goal -eq "magic_training" -and $_.to_goal -eq "hunt" })
		$success = @($verified | Where-Object { $_.result -eq "success" -and $_.spell -eq "Haste" })
		if ($idle.Count -ge 1 -and $selection.Count -eq 1 -and $requests.Count -eq 1 -and $success.Count -eq 1 -and $continued.Count -eq 1) { return }
		throw "Post-hunt overflow did not transition Idle to one magic-training cast and back to Hunt."
	}
	if ($Mode -eq "post_hunt_no_overflow") {
		$idle = @($events | Where-Object { $_.event -eq "objective_transition" -and $_.from -eq "hunt" -and $_.to -eq "idle" })
		$selection = @($events | Where-Object { $_.event -eq "goal_selection" -and $_.decision_reason -eq "hunt_deadline" -and $_.to_goal -eq "hunt" })
		$guard = @($candidates | Where-Object { $_.decision_reason -eq "hunt_deadline" -and -not $_.feasible -and $_.reason -eq "next_tick_not_overflow" })
		if ($idle.Count -ge 1 -and $selection.Count -ge 1 -and $guard.Count -ge 1 -and $actions.Count -eq 0) { return }
		throw "Post-hunt non-overflow did not return from Idle directly to Hunt."
	}
	if ($Mode -eq "restart") {
		$online = @($events | Where-Object { $_.event -eq "lifecycle" -and $_.status -eq "online" })
		$forecast = @($events | Where-Object {
			$_.event -eq "magic_training_fixture" -and $_.case -eq "active_default" -and $_.active -and $_.remaining -eq $_.interval
		})
		$guard = @($candidates | Where-Object {
			-not $_.feasible -and $_.reason -eq "next_tick_not_overflow" -and $_.mana_tick_remaining -eq $_.mana_tick_interval -and
			$_.mana + $_.mana_gain -le $_.mana_max
		})
		if ($online.Count -eq 1 -and $forecast.Count -eq 1 -and $guard.Count -ge 1 -and $actions.Count -eq 0) { return }
		throw "Restart did not recompute the fresh regeneration forecast from persisted player state."
	}
	$reason = $Mode -eq "reserve" ? "no_audited_safe_spell" : $Mode -eq "pz" ? "regeneration_paused" :
		$Mode -in @("absent", "expired") ? "no_active_regeneration_forecast" : "next_tick_not_overflow"
	if ($requests.Count -eq 0 -and $verified.Count -eq 0 -and @($candidates | Where-Object { -not $_.feasible -and $_.reason -eq $reason }).Count -ge 1) { return }
	throw "Magic training $Mode guard failed."
}

function Assert-MagicTrainingForecastFixture {
	param([string]$Logs, [string]$Mode)
	$fixture = @(ConvertFrom-PlayerbotLogs -Logs $Logs | Where-Object { $_.event -eq "magic_training_fixture" -and $_.source -eq "authoritative_forecast" })
	if ($Mode -eq "active") {
		$activeDefault = @($fixture | Where-Object { $_.case -eq "active_default" -and $_.active -and $_.gain -eq 10 -and $_.interval -eq 1000 -and $_.remaining -eq 1000 })
		$active = @($fixture | Where-Object { $_.case -eq "active" -and $_.gain -eq 10 -and $_.interval -eq 1000 -and $_.remaining -eq 1000 -and -not $_.exact_full_overflow -and $_.overflow_wasted -eq 5 })
		$finalTick = @($fixture | Where-Object { $_.case -eq "finite_final_tick" -and $_.active })
		$finite = @($fixture | Where-Object { $_.case -eq "finite_expires_before_tick" -and -not $_.active })
		$nonDefault = @($fixture | Where-Object { $_.case -eq "non_default" -and $_.active -and $_.gain -eq 4 -and $_.remaining -eq 1000 })
		$aggregated = @($fixture | Where-Object { $_.case -eq "earliest_same_engine_cycle" -and $_.active -and $_.gain -eq 13 -and $_.remaining -eq 1000 })
		$expired = @($fixture | Where-Object { $_.case -eq "expired" -and -not $_.active })
		if ($activeDefault.Count -eq 1 -and $active.Count -eq 1 -and $finalTick.Count -eq 1 -and $finite.Count -eq 1 -and $nonDefault.Count -eq 1 -and $aggregated.Count -eq 1 -and $expired.Count -eq 1) { return }
		throw "The Creature regeneration forecast fixture did not cover active default, expiration, non-default, earliest, and summed-tick boundaries."
	}
	$expected = $Mode -eq "pz" ? "active_default" : "active_default"
	if (@($fixture | Where-Object { $_.case -eq $expected -and -not $_.active }).Count -eq 1) { return }
	throw "Magic training $Mode did not expose an absent authoritative regeneration forecast."
}

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
	throw "Docker is required to run the playerbot gameplay suite."
}

$focusedScenarioRequested = $FullNavigation -or $TargetPursuit -or $CorpseLoot -or $DeathTelemetry -or $Healing -or $ValueLoot -or
	$PickupProgression -or $GoalArbitration -or $OracleDeparture -or $StaminaProjection -or $HuntRegionPlanning -or
	$AdaptiveChallenge -or
	$CombatReadiness -or $EquipmentOffers -or $EquipmentPurchases -or $MainlandRewards -or $Depot -or $SlottedLoot -or $MainlandLoop -or $SpellTraining -or $SpellUse -or $SpellCalibration -or $MagicTraining
if ($Focused -and -not $focusedScenarioRequested) {
	throw "-Focused requires at least one focused scenario switch."
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

	if ($MainlandLoop) {
		Invoke-Scenario -Name "mainland_loop" -DefaultTimeoutSeconds 600 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "mainland"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "10"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST MAINLAND_START' | Out-Null
			$loopLogs = Wait-ForLog -Pattern '"action":"hunt_cycle","result":"started","cycle":3'
			Assert-MainlandLoopEvents -Logs $loopLogs

			$restartLineCount = @((Get-ServerLogs) -split "`r?`n").Count
			Invoke-Compose stop server
			Invoke-Compose up --detach server
			$restartLogs = ""
			for ($attempt = 0; $attempt -lt 300; $attempt++) {
				Start-Sleep -Seconds 1
				$restartLogs = ((Get-ServerLogs) -split "`r?`n" | Select-Object -Skip $restartLineCount) -join "`n"
				if ($restartLogs -match '"action":"deposit","result":"complete","depot_id":2' -and
					$restartLogs -match '"action":"hunt_cycle","result":"started","cycle":1') { break }
			}
			Assert-MainlandLoopEvents -Logs $restartLogs -MinimumCycles 1 -MinimumDeposits 1
		}
	}

	if ($SlottedLoot) {
		Invoke-Scenario -Name "slotted_loot_seller" -DefaultTimeoutSeconds 240 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "slotted_loot_seller"
			$env:PLAYERBOT_DEPOT_RESTART_PHASE = ""
			Invoke-Compose up --detach playerbot-setup
			Start-Sleep -Seconds 3
			Invoke-DatabaseCommand -Query "DELETE FROM player_items WHERE player_id = (SELECT id FROM players WHERE name = 'Bot One') AND (pid = 10 OR itemtype = 8704); INSERT INTO player_items (player_id, sid, pid, itemtype, count, attributes) SELECT id, 9900, 10, 2398, 1, X'' FROM players WHERE name = 'Bot One';"
			Invoke-Compose up --no-deps --detach server
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST SLOTTED_LOOT_SELLER_PASS' | Out-Null
			$sellerLogs = Wait-ForLog -Pattern '"decision_reason":"service_complete"'
			Assert-SlottedLootEvents -Logs $sellerLogs -SellerAvailable
		}

		Invoke-Scenario -Name "slotted_loot_no_seller" -DefaultTimeoutSeconds 240 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "slotted_loot_no_seller"
			$env:PLAYERBOT_DEPOT_RESTART_PHASE = ""
			Invoke-Compose up --detach playerbot-setup
			Start-Sleep -Seconds 3
			Invoke-DatabaseCommand -Query "DELETE FROM player_items WHERE player_id = (SELECT id FROM players WHERE name = 'Bot One') AND (pid = 10 OR itemtype = 8704); INSERT INTO player_items (player_id, sid, pid, itemtype, count, attributes) SELECT id, 9900, 10, 2398, 1, X'' FROM players WHERE name = 'Bot One';"
			Invoke-Compose up --no-deps --detach server
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST SLOTTED_LOOT_NO_SELLER_PASS' | Out-Null
			$noSellerLogs = Wait-ForLog -Pattern '"decision_reason":"service_complete"'
			Assert-SlottedLootEvents -Logs $noSellerLogs
		}

		Invoke-Scenario -Name "slotted_loot_deposit_restart" -DefaultTimeoutSeconds 300 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "slotted_loot_no_seller"
			$env:PLAYERBOT_DEPOT_RESTART_PHASE = "deposit"
			Invoke-Compose up --detach playerbot-setup
			Start-Sleep -Seconds 3
			Invoke-DatabaseCommand -Query "DELETE FROM player_items WHERE player_id = (SELECT id FROM players WHERE name = 'Bot One') AND (pid = 10 OR itemtype = 8704); INSERT INTO player_items (player_id, sid, pid, itemtype, count, attributes) SELECT id, 9900, 10, 2398, 1, X'' FROM players WHERE name = 'Bot One';"
			Invoke-Compose up --no-deps --detach server
			Wait-ForLog -Pattern '"action":"depot_restart_checkpoint","result":"paused","phase":"deposit"' | Out-Null
			Invoke-Compose stop server
			Invoke-Compose up --detach server
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST SLOTTED_LOOT_NO_SELLER_PASS' | Out-Null
			$restartLogs = Wait-ForLog -Pattern '"decision_reason":"service_complete"'
			Assert-SlottedLootEvents -Logs $restartLogs -Restarted
		}
	}

	if ($Depot) {
		Invoke-Scenario -Name "real_depot" -DefaultTimeoutSeconds 240 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "depot"
			$env:PLAYERBOT_DEPOT_RESTART_PHASE = ""
			$env:PLAYERBOT_DEPOT_MOVE_CASE = "normal"
			Invoke-Compose up --detach
			Invoke-DatabaseCommand -Query "INSERT INTO player_depotitems (player_id, sid, pid, itemtype, count, attributes) SELECT id, 9001, 2, 2684, 7, X'' FROM players WHERE name = 'Rook Tester'"
			$firstCycleLogs = Wait-ForLog -Pattern '"action":"deposit","result":"complete"'
			Assert-DepotEvents -Logs $firstCycleLogs -ExpectedDepositedCount 2 -ExpectedEquipmentDeposits 2

			$restartLineCount = @((Get-ServerLogs) -split "`r?`n").Count
			Invoke-Compose stop server
			Invoke-Compose up --detach server
			$secondCycleLogs = ""
			for ($attempt = 0; $attempt -lt 180; $attempt++) {
				Start-Sleep -Seconds 1
				$secondCycleLogs = ((Get-ServerLogs) -split "`r?`n" | Select-Object -Skip $restartLineCount) -join "`n"
				if ($secondCycleLogs -match '"action":"deposit","result":"complete"') { break }
			}
			Assert-DepotEvents -Logs $secondCycleLogs -ExpectedDepositedCount 1 -ExpectedEquipmentDeposits 0
			$sentinelCount = Invoke-DatabaseScalar -Query "SELECT COALESCE(SUM(count), 0) FROM player_depotitems JOIN players ON players.id = player_depotitems.player_id WHERE players.name = 'Rook Tester' AND pid = 2 AND itemtype = 2684"
			if ($sentinelCount -ne 7) {
				throw "Bot One's depot cycle changed Rook Tester's persisted depot sentinel."
			}
		}

		foreach ($phase in @("approach", "locker", "chest", "deposit", "depart")) {
			Invoke-Scenario -Name "real_depot_restart_$phase" -DefaultTimeoutSeconds 180 -Body {
				Invoke-Compose down --volumes --remove-orphans
				$env:PLAYERBOT_GAMEPLAY_MODE = "depot"
				$env:PLAYERBOT_DEPOT_RESTART_PHASE = $phase
				$env:PLAYERBOT_DEPOT_MOVE_CASE = "normal"
				Invoke-Compose up --detach
				$checkpointPattern = '"action":"depot_restart_checkpoint","result":"paused","phase":"' + [regex]::Escape($phase) + '"'
				$checkpointLogs = Wait-ForLog -Pattern $checkpointPattern
				$checkpointEvents = @(ConvertFrom-PlayerbotLogs -Logs $checkpointLogs | Where-Object {
					$_.event -eq "action_result" -and $_.action -eq "depot_restart_checkpoint" -and
					$_.result -eq "paused" -and $_.phase -eq $phase
				})
				if ($checkpointEvents.Count -ne 1) {
					throw "Depot $phase restart checkpoint was not reached exactly once."
				}
				$restartLineCount = @((Get-ServerLogs) -split "`r?`n").Count
				Invoke-Compose stop server
				Invoke-Compose up --detach server
				$recoveryLogs = ""
				for ($attempt = 0; $attempt -lt 150; $attempt++) {
					Start-Sleep -Seconds 1
					$recoveryLogs = ((Get-ServerLogs) -split "`r?`n" | Select-Object -Skip $restartLineCount) -join "`n"
					if ($recoveryLogs -match '"action":"deposit","result":"complete"') { break }
				}
				Assert-DepotRecoveryEvents -Logs $recoveryLogs -Phase $phase
			}
		}

		Invoke-Scenario -Name "real_depot_partial_move" -DefaultTimeoutSeconds 120 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "depot"
			$env:PLAYERBOT_DEPOT_RESTART_PHASE = ""
			$env:PLAYERBOT_DEPOT_MOVE_CASE = "partial"
			Invoke-Compose up --detach
			$partialLogs = Wait-ForLog -Pattern '"action":"deposit","result":"complete","depot_id":2'
			$events = @(ConvertFrom-PlayerbotLogs -Logs $partialLogs)
			$partial = @($events | Where-Object {
				$_.action -eq "deposit" -and $_.result -eq "partial" -and $_.item_id -eq 2684 -and
				$_.requested -eq 2 -and $_.verified -eq 1 -and $_.inventory_before -eq 4 -and
				$_.inventory_after -eq 3 -and $_.depot_before -eq 0 -and $_.depot_after -eq 1
			})
			$submitted = @($events | Where-Object {
				$_.action -eq "deposit" -and $_.result -eq "requested" -and $_.requested -eq 2 -and $_.submitted -eq 1
			})
			if ($partial.Count -ne 1 -or $submitted.Count -ne 1 -or
				@($events | Where-Object { $_.event -eq "terminal" }).Count -ne 0) {
				throw "The partial depot move did not emit exact verified deltas and finish the remainder."
			}
		}

		Invoke-Scenario -Name "real_depot_rejected_move" -DefaultTimeoutSeconds 90 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "depot"
			$env:PLAYERBOT_DEPOT_RESTART_PHASE = ""
			$env:PLAYERBOT_DEPOT_MOVE_CASE = "rejected"
			Invoke-Compose up --detach
			$rejectedLogs = Wait-ForLog -Pattern '"event":"terminal".*"reason":"depot_no_slot_or_move_rejected"'
			$events = @(ConvertFrom-PlayerbotLogs -Logs $rejectedLogs)
			$requests = @($events | Where-Object { $_.action -eq "deposit" -and $_.result -eq "requested" -and $_.item_id -eq 2382 })
			$retries = @($events | Where-Object {
				$_.action -eq "deposit" -and $_.result -eq "retry" -and $_.item_id -eq 2382 -and $_.verified -eq 0 -and
				$_.inventory_before -eq 1 -and $_.inventory_after -eq 1 -and $_.depot_before -eq 0 -and $_.depot_after -eq 0
			})
			$failed = @($events | Where-Object {
				$_.action -eq "deposit" -and $_.result -eq "failed" -and $_.reason -eq "no_slot_or_move_rejected" -and $_.item_id -eq 2382 -and
				$_.retry -eq 3 -and $_.verified -eq 0 -and $_.inventory_before -eq 1 -and $_.inventory_after -eq 1 -and
				$_.depot_before -eq 0 -and $_.depot_after -eq 0
			})
			$terminals = @($events | Where-Object { $_.event -eq "terminal" -and $_.reason -eq "depot_no_slot_or_move_rejected" })
			if ($requests.Count -ne 3 -or $retries.Count -ne 2 -or $failed.Count -ne 1 -or $terminals.Count -ne 1) {
				throw "Rejected depot moves were not bounded to three attempts with unchanged exact deltas. requests=$($requests.Count), retries=$($retries.Count), failed=$($failed.Count), terminals=$($terminals.Count)."
			}
		}
	}

    if ($PickupProgression) {
        Invoke-Scenario -Name "pickup_progression" -DefaultTimeoutSeconds 180 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "progression"
            $env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
            Invoke-Compose up --detach
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_PASS' | Out-Null
            $progressionLogs = Wait-ForLog -Pattern '"event":"strategy_objective_result".*"candidate_id":64120.*"result":"success"'
            Assert-PickupProgressionEvents -Logs $progressionLogs
            Invoke-Compose stop server
            Invoke-Compose up --detach server
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_RESTART_START' | Out-Null
			$restartLogs = Wait-ForLog -Pattern '"event":"goal_selection".*"to_goal":"hunt"'
            Assert-PickupProgressionRestartEvents -Logs $restartLogs
        }

		Invoke-Scenario -Name "pickup_progression_bundle" -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "progression_bundle"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_BUNDLE_PASS' | Out-Null
			$bundleLogs = Wait-ForLog -Pattern '"event":"strategy_objective_result".*"candidate_id":50082.*"result":"success"'
			Assert-EconomicPickupProgressionEvents -Logs $bundleLogs
		}

		Invoke-Scenario -Name "pickup_progression_nested" -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "progression_nested"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_NESTED_PASS' | Out-Null
			$nestedLogs = Wait-ForLog -Pattern '"event":"strategy_objective_result".*"candidate_id":50083.*"result":"success"'
			Assert-NestedPickupProgressionEvents -Logs $nestedLogs
		}

        Invoke-Scenario -Name "pickup_progression_resume" -DefaultTimeoutSeconds 60 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "progression_resume"
            Invoke-Compose up --detach
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_PASS' | Out-Null
            $resumeLogs = Wait-ForLog -Pattern '"event":"strategy_selection".*"reason":"resume_claimed_upgrade"'
            Assert-PickupProgressionResumeEvents -Logs $resumeLogs
        }

        Invoke-Scenario -Name "pickup_progression_nested_resume" -DefaultTimeoutSeconds 60 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "progression_nested_resume"
            Invoke-Compose up --detach
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_NESTED_PASS' | Out-Null
            $nestedResumeLogs = Wait-ForLog -Pattern '"event":"strategy_selection".*"reason":"resume_claimed_upgrade".*"root_item_id":1994'
            Assert-NestedPickupProgressionResumeEvents -Logs $nestedResumeLogs
        }

        Invoke-Scenario -Name "pickup_progression_space" -DefaultTimeoutSeconds 60 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "progression_space"
            Invoke-Compose up --detach
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_SPACE_START' | Out-Null
            $spaceLogs = Wait-ForLog -Pattern '"candidate_id":64120.*"reason":"insufficient_inventory_space"'
            Assert-PickupProgressionSpaceEvents -Logs $spaceLogs
        }
    }

    if ($GoalArbitration) {
        Invoke-Scenario -Name "goal_arbitration" -DefaultTimeoutSeconds 240 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "arbitration"
            $env:PLAYERBOT_HUNT_DURATION_SECONDS = "10"
            Invoke-Compose up --detach
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST GOAL_ARBITRATION_START' | Out-Null
            $arbitrationLogs = Wait-ForLog -Pattern '"event":"goal_selection".*"decision_id":4.*"to_goal":"hunt"'
            Assert-GoalArbitrationEvents -Logs $arbitrationLogs
        }

        Invoke-Scenario -Name "goal_arbitration_interrupt" -DefaultTimeoutSeconds 60 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "arbitration_interrupt"
            $env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
            Invoke-Compose up --detach
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST GOAL_ARBITRATION_INTERRUPT_TRIGGERED' | Out-Null
            $interruptLogs = Wait-ForLog -Pattern '"event":"goal_selection".*"decision_id":2.*"to_goal":"service"'
            Assert-GoalArbitrationInterruptEvents -Logs $interruptLogs
        }
    }

    if ($StaminaProjection) {
        Invoke-Scenario -Name "stamina_bonus_projection" -DefaultTimeoutSeconds 90 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "stamina_bonus"
            $env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
            Invoke-Compose up --detach
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST STAMINA_PROJECTION_START 2520' | Out-Null
            $bonusLogs = Wait-ForLog -Pattern '"event":"hunt_region_selection".*"result":"selected"'
            Assert-StaminaProjectionEvents -Logs $bonusLogs -StaminaMinutes 2520
        }

        Invoke-Scenario -Name "stamina_boundary_projection" -DefaultTimeoutSeconds 90 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "stamina_boundary"
            $env:PLAYERBOT_HUNT_DURATION_SECONDS = "300"
            Invoke-Compose up --detach
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST STAMINA_PROJECTION_START 2401' | Out-Null
            $boundaryLogs = Wait-ForLog -Pattern '"event":"hunt_region_selection".*"result":"selected"'
            Assert-StaminaProjectionEvents -Logs $boundaryLogs -StaminaMinutes 2401
        }

        Invoke-Scenario -Name "stamina_normal_projection" -DefaultTimeoutSeconds 90 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "stamina_normal"
            $env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
            Invoke-Compose up --detach
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST STAMINA_PROJECTION_START 2400' | Out-Null
            $normalLogs = Wait-ForLog -Pattern '"event":"hunt_region_selection".*"result":"selected"'
            Assert-StaminaProjectionEvents -Logs $normalLogs -StaminaMinutes 2400
        }
    }

	if ($HuntRegionPlanning) {
        Invoke-Scenario -Name "hunt_region_planning" -DefaultTimeoutSeconds 180 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "hunt_planning"
            $env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
            Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST HUNT_PLANNING_START' | Out-Null
            $planningLogs = Wait-ForLog -Pattern '"event":"hunt_region_scan".*"phase":"selected"'
            Assert-HuntRegionPlanningEvents -Logs $planningLogs
        }
    }

    if ($CombatReadiness) {
        Invoke-Scenario -Name "combat_readiness_ready" -DefaultTimeoutSeconds 60 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "readiness_ready"
            Invoke-Compose up --detach
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST READINESS_READY_PASS' | Out-Null
            $readyLogs = Wait-ForLog -Pattern '"event":"combat_readiness".*"result":"ready"'
            Assert-CombatReadinessEvents -Logs $readyLogs -Mode "ready"
			$restartLineCount = @((Get-ServerLogs) -split "`r?`n").Count
            Invoke-Compose stop server
            Invoke-Compose up --detach server
			$restartLogs = ""
			for ($attempt = 0; $attempt -lt 30; $attempt++) {
				Start-Sleep -Seconds 1
				$restartLogs = ((Get-ServerLogs) -split "`r?`n" | Select-Object -Skip $restartLineCount) -join "`n"
				if ($restartLogs -match '"event":"combat_readiness".*"vocation_id":4') { break }
			}
            Assert-CombatReadinessEvents -Logs $restartLogs -Mode "ready"
        }
        Invoke-Scenario -Name "combat_readiness_upgrade" -DefaultTimeoutSeconds 60 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "readiness_upgrade"
            Invoke-Compose up --detach
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST READINESS_UPGRADE_PASS' | Out-Null
            Assert-CombatReadinessEvents -Logs (Get-ServerLogs) -Mode "upgrade"
        }
        Invoke-Scenario -Name "combat_readiness_missing_weapon" -DefaultTimeoutSeconds 45 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "readiness_missing_weapon"
            Invoke-Compose up --detach
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST READINESS_MISSING_WEAPON_START' | Out-Null
            $missingWeaponLogs = Wait-ForLog -Pattern '"reason":"combat_readiness_missing_legal_melee_weapon"'
            Assert-CombatReadinessEvents -Logs $missingWeaponLogs -Mode "missing_weapon"
        }
		Invoke-Scenario -Name "combat_readiness_supplies" -DefaultTimeoutSeconds 120 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "readiness_supplies"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST READINESS_SUPPLIES_PASS' | Out-Null
			$supplyLogs = Wait-ForLog -Pattern '"action":"buy_potions".*"result":"success"'
			Assert-CombatReadinessEvents -Logs $supplyLogs -Mode "supplies"
		}
		Invoke-Scenario -Name "combat_readiness_no_food" -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "readiness_no_food"
			Invoke-Compose up --detach
			$noFoodLogs = Wait-ForLog -Pattern '"event":"goal_candidate".*"goal":"service".*"feasible":false.*"reason":"no_service_need"'
			Assert-CombatReadinessEvents -Logs $noFoodLogs -Mode "no_food"
		}
		Invoke-Scenario -Name "combat_readiness_low_wealth" -DefaultTimeoutSeconds 120 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "readiness_low_wealth"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST READINESS_LOW_WEALTH_PASS' | Out-Null
			$lowWealthLogs = Wait-ForLog -Pattern '"action":"hunt_cycle".*"result":"started"'
			$events = @(ConvertFrom-PlayerbotLogs -Logs $lowWealthLogs)
			$withdrawal = @($events | Where-Object {
				$_.action -eq "bank_withdraw" -and $_.result -eq "success" -and $_.count -eq 56 -and
				$_.bank_before -eq 56 -and $_.bank_after -eq 0
			})
			$upgrade = @($events | Where-Object {
				$_.action -eq "equip_readiness" -and $_.result -eq "success" -and $_.item_id -eq 2384
			})
			$soldUpgrade = @($events | Where-Object { $_.action -eq "sell" -and $_.item_id -eq 2384 })
			if ($withdrawal.Count -lt 1 -or $upgrade.Count -ne 1 -or $soldUpgrade.Count -ne 0 -or
				@($events | Where-Object { $_.event -eq "terminal" }).Count -ne 0) {
				throw "Low-wealth service did not withdraw the available balance and resume hunting."
			}
		}
		Invoke-Scenario -Name "combat_readiness_food_capacity" -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "readiness_food_capacity"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST READINESS_FOOD_CAPACITY_PASS' | Out-Null
			$foodCapacityLogs = Wait-ForLog -Pattern '"action":"eat".*"item_id":2696.*"inventory_count":2'
			Assert-CombatReadinessEvents -Logs $foodCapacityLogs -Mode "food_capacity"
		}
        Invoke-Scenario -Name "combat_readiness_retention" -DefaultTimeoutSeconds 120 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "readiness_retention"
            Invoke-Compose up --detach
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST READINESS_RETENTION_PASS' | Out-Null
            $retentionLogs = Wait-ForLog -Pattern '"event":"combat_readiness".*"result":"ready"'
            Assert-CombatReadinessEvents -Logs $retentionLogs -Mode "retention"
        }
    }

    if ($EquipmentOffers) {
        Invoke-Scenario -Name "equipment_offer_shadow_upgrade" -DefaultTimeoutSeconds 90 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "equipment_shadow"
            Invoke-Compose up --detach
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_SHADOW_START' | Out-Null
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_SHADOW_PASS' | Out-Null
            $upgradeLogs = Wait-ForPlayerbotEvent { $_.event -eq "equipment_offer_shadow" }
            Assert-EquipmentOfferEvents -Logs $upgradeLogs -Mode "upgrade"
        }
        Invoke-Scenario -Name "equipment_offer_shadow_unaffordable" -DefaultTimeoutSeconds 90 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "equipment_shadow_unaffordable"
            Invoke-Compose up --detach
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_SHADOW_UNAFFORDABLE_START' | Out-Null
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_SHADOW_UNAFFORDABLE_PASS' | Out-Null
            $unaffordableLogs = Wait-ForPlayerbotEvent { $_.event -eq "equipment_offer_shadow" }
            Assert-EquipmentOfferEvents -Logs $unaffordableLogs -Mode "unaffordable"
        }
        Invoke-Scenario -Name "equipment_offer_shadow_no_upgrade" -DefaultTimeoutSeconds 90 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "equipment_shadow_no_upgrade"
            Invoke-Compose up --detach
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_SHADOW_NO_UPGRADE_START' | Out-Null
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_SHADOW_NO_UPGRADE_PASS' | Out-Null
            $noUpgradeLogs = Wait-ForPlayerbotEvent { $_.event -eq "equipment_offer_shadow" }
            Assert-EquipmentOfferEvents -Logs $noUpgradeLogs -Mode "no_upgrade"
        }
    }

	if ($EquipmentPurchases) {
		Invoke-Scenario -Name "equipment_purchase" -DefaultTimeoutSeconds 240 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "equipment_buy"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_BUY_PASS' | Out-Null
			$purchaseLogs = Wait-ForPlayerbotEvent {
				$_.event -eq "goal_result" -and $_.goal -eq "buy_equipment" -and $_.result -eq "success"
			}
			Assert-EquipmentPurchaseEvents -Logs $purchaseLogs

			$restartLineCount = @((Get-ServerLogs) -split "`r?`n").Count
			Invoke-Compose stop server
			Invoke-Compose up --detach server
			$restartLogs = ""
			while ([DateTime]::UtcNow -lt $currentScenarioDeadline) {
				$restartLogs = ((Get-ServerLogs) -split "`r?`n" | Select-Object -Skip $restartLineCount) -join "`n"
				$restartOnline = @(ConvertFrom-PlayerbotLogs -Logs $restartLogs | Where-Object {
					$_.event -eq "lifecycle" -and $_.status -eq "online" -and -not $_.recovered -and $_.objective -eq "service"
				}).Count -gt 0
				if ($restartLogs -match 'PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_BUY_RESTART_PASS' -and $restartOnline) { break }
				Start-Sleep -Seconds 1
			}
			if ($restartLogs -notmatch 'PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_BUY_RESTART_PASS' -or -not $restartOnline) {
				Throw-WaitTimeout "Timed out waiting for equipment purchase restart reconstruction."
			}
			Assert-EquipmentPurchaseEvents -Logs $restartLogs -Restart
		}
		Invoke-Scenario -Name "equipment_purchase_resume" -DefaultTimeoutSeconds 240 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "equipment_buy_resume"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_BUY_RESUME_PASS' | Out-Null
			$resumeLogs = Wait-ForPlayerbotEvent {
				$_.event -eq "goal_result" -and $_.goal -eq "buy_equipment" -and $_.result -eq "success"
			}
			Assert-EquipmentPurchaseEvents -Logs $resumeLogs -Resume
		}
		Invoke-Scenario -Name "equipment_purchase_space" -DefaultTimeoutSeconds 240 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "equipment_buy_space"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			$spaceLogs = Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_BUY_SPACE_PASS'
			$spaceEvents = @(ConvertFrom-PlayerbotLogs -Logs $spaceLogs)
			$spaceRejections = @($spaceEvents | Where-Object {
				$_.event -eq "equipment_offer_candidate" -and $_.item_id -eq 2379 -and
				$_.reason -eq "insufficient_displaced_item_space"
			})
			$spaceActions = @($spaceEvents | Where-Object {
				$_.event -eq "action_result" -and $_.action -in @("buy_equipment", "equip_equipment")
			})
			if ($spaceRejections.Count -lt 1 -or $spaceActions.Count -ne 0) {
				throw "Equipment purchase did not reject insufficient displaced-item storage before payment."
			}
		}
		Invoke-Scenario -Name "equipment_purchase_rejected" -DefaultTimeoutSeconds 240 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "equipment_buy_rejected"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_BUY_REJECTED_PASS' | Out-Null
			$rejectedLogs = Wait-ForPlayerbotEvent {
				$_.event -eq "goal_selection" -and $_.decision_reason -eq "equipment_purchase_failed" -and
				$_.from_goal -eq "buy_equipment" -and $_.to_goal -ne "buy_equipment"
			}
			Assert-EquipmentPurchaseEvents -Logs $rejectedLogs -Rejected
		}
	}

	if ($AdaptiveChallenge) {
		Invoke-Scenario -Name "adaptive_challenge" -DefaultTimeoutSeconds 120 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "adaptive_challenge"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "60"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST ADAPTIVE_CHALLENGE_START' | Out-Null
			$challengeLogs = Wait-ForLog -Pattern '"event":"terminal".*"reason":"hunt_scope_exhausted"'
			Assert-AdaptiveChallengeEvents -Logs $challengeLogs
		}
	}

	if ($MainlandRewards) {
		Invoke-Scenario -Name "mainland_equipment_reward" -DefaultTimeoutSeconds 240 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "mainland_reward"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST MAINLAND_REWARD_PASS' | Out-Null
			$logs = Get-ServerLogs
			$events = @(ConvertFrom-PlayerbotLogs -Logs $logs)
			$candidate = @($events | Where-Object {
				$_.event -eq "strategy_candidate" -and $_.candidate_id -eq 50076 -and
				$_.acquisition_source -eq "map_reward" -and $_.result -eq "feasible" -and $_.item_id -eq 2483
			})
			$selection = @($events | Where-Object {
				$_.event -eq "strategy_selection" -and $_.candidate_id -eq 50076 -and $_.item_id -eq 2483
			})
			$claim = @($events | Where-Object {
				$_.event -eq "action_result" -and $_.action -eq "claim_reward" -and
				$_.result -eq "success" -and $_.candidate_id -eq 50076
			})
			$equip = @($events | Where-Object {
				$_.event -eq "action_result" -and $_.action -eq "equip" -and
				$_.result -eq "success" -and $_.item_id -eq 2483 -and $_.displaced_item_id -eq 2650
			})
			$result = @($events | Where-Object {
				$_.event -eq "strategy_objective_result" -and $_.goal -eq "pickup_reward" -and
				$_.candidate_id -eq 50076 -and $_.acquisition_source -eq "map_reward" -and $_.result -eq "success"
			})
			$battleAxeRejection = @($events | Where-Object {
				$_.event -eq "reward_inspection" -and $_.candidate_id -eq 9217 -and
				$_.equipment_upgrade_count -eq 0 -and $null -ne $_.equipment_rejection
			})
			if ($candidate.Count -ne 1 -or $selection.Count -ne 1 -or $claim.Count -ne 1 -or
				$equip.Count -ne 1 -or $result.Count -ne 1 -or $battleAxeRejection.Count -lt 1) {
				throw "Mainland reward discovery did not produce the expected selection, rejection, claim, and equip evidence."
			}

			$restartLineCount = @(($logs -split "`r?`n")).Count
			Invoke-Compose stop server
			Invoke-Compose up --detach server
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST MAINLAND_REWARD_RESTART_PASS' | Out-Null
			Wait-ForLog -Pattern '"event":"goal_selection".*"to_goal":"hunt"' | Out-Null
			$restartLogs = ((Get-ServerLogs) -split "`r?`n" | Select-Object -Skip $restartLineCount) -join "`n"
			$restartEvents = @(ConvertFrom-PlayerbotLogs -Logs $restartLogs)
			$duplicateActions = @($restartEvents | Where-Object {
				$_.event -eq "action_result" -and $_.action -in @("claim_reward", "equip") -and $_.item_id -eq 2483
			})
			if ($duplicateActions.Count -ne 0) {
				throw "Mainland reward restart repeated a completed claim or equip action."
			}
		}
	}

    if ($OracleDeparture) {
        Invoke-Scenario -Name "oracle_departure" -DefaultTimeoutSeconds 180 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "departure"
            $env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
            Invoke-Compose up --detach
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST ORACLE_DEPARTURE_PASS' | Out-Null
            $departureLogs = Wait-ForLog -Pattern '"action":"oracle_departure","result":"success"'
            Assert-OracleDepartureEvents -Logs $departureLogs
            Invoke-Compose stop server
            Invoke-Compose up --detach server
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST ORACLE_DEPARTURE_RESTART_PASS' | Out-Null
            $restartLogs = Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST ORACLE_DEPARTURE_RESTART_PASS'
            Assert-OracleDepartureEvents -Logs $restartLogs -Restart
        }

        Invoke-Scenario -Name "oracle_level_eight_interrupt" -DefaultTimeoutSeconds 180 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "departure_interrupt"
            $env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
            Invoke-Compose up --detach
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST ORACLE_LEVEL_EIGHT_INTERRUPT_START' | Out-Null
            $interruptLogs = Wait-ForLog -Pattern '"action":"oracle_departure","result":"success"'
            Assert-OracleDepartureEvents -Logs $interruptLogs
            Assert-OracleLevelEightInterruptEvents -Logs $interruptLogs
        }

        Invoke-Scenario -Name "oracle_level_eight_recovery" -DefaultTimeoutSeconds 180 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $env:PLAYERBOT_GAMEPLAY_MODE = "departure_recovery"
            $env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
            Invoke-Compose up --detach
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST ORACLE_LEVEL_EIGHT_RECOVERY_PREPARED' | Out-Null
            Invoke-Compose stop server
            Invoke-Compose up --detach server
            Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST ORACLE_LEVEL_EIGHT_RECOVERY_START' | Out-Null
            $recoveryLogs = Wait-ForLog -Pattern '"action":"oracle_departure","result":"success"'
            Assert-OracleDepartureEvents -Logs $recoveryLogs -InterruptedByRestart
            Assert-OracleLevelEightInterruptEvents -Logs $recoveryLogs -Recovery
        }
    }

	if ($FullNavigation) {
		Invoke-Scenario -Name "navigation" -DefaultTimeoutSeconds 180 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "navigation"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			$navigationLogs = Wait-ForPlayerbotEventCount -Action "hunt_waypoint" -Count 5
			Assert-NavigationEvents -Logs $navigationLogs
		}
		Invoke-Scenario -Name "navigation_recovery" -DefaultTimeoutSeconds 150 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "navigation_recovery"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST NAVIGATION_RECOVERY_START' | Out-Null
			$recoveryLogs = Wait-ForPlayerbotEventCount -Action "hunt_waypoint" -Count 1
			Assert-NavigationRecoveryEvents -Logs $recoveryLogs
		}
		Invoke-Scenario -Name "patrol_recovery" -DefaultTimeoutSeconds 120 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "patrol_recovery"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST PATROL_RECOVERY_START' | Out-Null
			$recoveryLogs = Wait-ForLog -Pattern '"action":"plan","result":"success"'
			Assert-PatrolRecoveryEvents -Logs $recoveryLogs
		}
	}

	if ($TargetPursuit) {
		Invoke-Scenario -Name "target_pursuit" -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "target_pursuit"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST TARGET_PURSUIT_HIDDEN' | Out-Null
			Wait-ForLog -Pattern '"action":"target_pursuit","result":"reacquired"' | Out-Null
			$pursuitLogs = Wait-ForLog -Pattern '"reason":"target_defeated"'
			Assert-TargetPursuitEvents -Logs $pursuitLogs
		}
		Invoke-Scenario -Name "target_pursuit_abandon" -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "target_pursuit_abandon"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST TARGET_PURSUIT_HIDDEN' | Out-Null
			$pursuitLogs = Wait-ForLog -Pattern '"action":"target_pursuit","result":"abandoned"'
			Assert-TargetPursuitAbandonEvents -Logs $pursuitLogs
		}
	}

	if ($SpellTraining) {
		Invoke-Scenario -Name "spell_training" -DefaultTimeoutSeconds 180 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "spell_training"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST SPELL_TRAINING_PASS' | Out-Null
			$trainingLogs = Wait-ForLog -Pattern '"action":"learn_spell","result":"success"'
			Assert-SpellTrainingEvents -Logs $trainingLogs
			Invoke-Compose stop server
			Invoke-Compose up --detach server
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST SPELL_TRAINING_RESTART_PASS' | Out-Null
			$restartLogs = Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST SPELL_TRAINING_RESTART_PASS'
			Assert-SpellTrainingEvents -Logs $restartLogs -Restart
		}
	}

	if ($SpellUse) {
		Invoke-Scenario -Name "spell_use" -DefaultTimeoutSeconds 90 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "spell_use"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			$spellLogs = Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST SPELL_USE_PASS'
			Assert-SpellUseEvents -Logs $spellLogs
		}
	}

	if ($SpellCalibration) {
		Invoke-Scenario -Name "spell_calibration" -DefaultTimeoutSeconds 90 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "spell_calibration"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST SPELL_CALIBRATION_START' | Out-Null
			$calibrationLogs = Wait-ForLog -Pattern '"event":"spell_calibration".*"phase":"fixture_profile_clear"'
			$engineLogs = Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST SPELL_USE_PASS'
			Assert-SpellCalibrationEvents -Logs $engineLogs
			$restartLineCount = @((Get-ServerLogs) -split "`r?`n").Count
			Invoke-Compose stop server
			Invoke-Compose up --detach server
			$restartLogs = ""
			while ([DateTime]::UtcNow -lt $currentScenarioDeadline) {
				$restartLogs = ((Get-ServerLogs) -split "`r?`n" | Select-Object -Skip $restartLineCount) -join "`n"
				$fresh = @(ConvertFrom-PlayerbotLogs -Logs $restartLogs | Where-Object {
					$_.event -eq "lifecycle" -and $_.status -eq "online" -and $_.spell_calibration_profiles -eq 0
				})
				if ($fresh.Count -eq 1) { break }
				Start-Sleep -Seconds 1
			}
			if (@(ConvertFrom-PlayerbotLogs -Logs $restartLogs | Where-Object {
				$_.event -eq "lifecycle" -and $_.status -eq "online" -and $_.spell_calibration_profiles -eq 0
			}).Count -ne 1) {
				Throw-WaitTimeout "Controller recreation did not start with an empty spell calibration profile store."
			}
		}
	}

	if ($MagicTraining) {
		foreach ($case in @(
			@{ Name = "magic_training_haste"; Mode = "cast"; Spell = "Haste" },
			@{ Name = "magic_training_great_light"; Mode = "cast"; Spell = "Great Light" },
			@{ Name = "magic_training_light"; Mode = "cast"; Spell = "Light" },
			@{ Name = "magic_training_refresh"; Mode = "cast"; Spell = "Light" },
			@{ Name = "magic_training_reserve"; Mode = "reserve"; Spell = "" },
			@{ Name = "magic_training_exact_full"; Mode = "exact_full"; Spell = "" },
			@{ Name = "magic_training_pz"; Mode = "pz"; Spell = "" },
			@{ Name = "magic_training_absent"; Mode = "absent"; Spell = "" },
			@{ Name = "magic_training_expired"; Mode = "expired"; Spell = "" },
			@{ Name = "magic_training_failed"; Mode = "failed"; Spell = "" }
		)) {
			Invoke-Scenario -Name $case.Name -DefaultTimeoutSeconds 120 -Body {
				Invoke-Compose down --volumes --remove-orphans
				$env:PLAYERBOT_GAMEPLAY_MODE = $case.Name
				Invoke-Compose up --detach
				$pattern = if ($case.Mode -eq "cast") { '"action":"magic_training","result":"success"' } elseif ($case.Mode -eq "failed") { '"action":"magic_training","result":"failed"' } else { '"event":"goal_candidate".*"goal":"magic_training"' }
				$logs = Wait-ForLog -Pattern $pattern
				Assert-MagicTrainingEvents -Logs $logs -Mode $case.Mode -Spell $case.Spell
				if ($case.Name -eq "magic_training_haste") {
					Assert-MagicTrainingForecastFixture -Logs $logs -Mode "active"
				} elseif ($case.Mode -in @("pz", "absent", "expired")) {
					Assert-MagicTrainingForecastFixture -Logs $logs -Mode $case.Mode
				}
			}
		}
		foreach ($case in @(
			@{ Name = "magic_training_service"; Winner = "service" },
			@{ Name = "magic_training_progression"; Winner = "learn_spell" }
		)) {
			Invoke-Scenario -Name $case.Name -DefaultTimeoutSeconds 120 -Body {
				Invoke-Compose down --volumes --remove-orphans
				$env:PLAYERBOT_GAMEPLAY_MODE = $case.Name
				Invoke-Compose up --detach
				$logs = Wait-ForLog -Pattern ('"event":"goal_selection".*"to_goal":"' + $case.Winner + '"')
				$events = @(ConvertFrom-PlayerbotLogs -Logs $logs)
				$magic = @($events | Where-Object { $_.event -eq "goal_candidate" -and $_.goal -eq "magic_training" -and $_.feasible -and $_.utility -eq 350 })
				$actions = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "magic_training" })
				if ($magic.Count -lt 1 -or $actions.Count -ne 0) { throw "Magic training did not yield to $($case.Winner)." }
			}
		}
		foreach ($case in @(
			@{ Name = "magic_training_post_hunt"; Mode = "post_hunt" },
			@{ Name = "magic_training_post_hunt_no_overflow"; Mode = "post_hunt_no_overflow" }
		)) {
			Invoke-Scenario -Name $case.Name -DefaultTimeoutSeconds 120 -Body {
				Invoke-Compose down --volumes --remove-orphans
				$env:PLAYERBOT_GAMEPLAY_MODE = $case.Name
				$env:PLAYERBOT_HUNT_DURATION_SECONDS = "1"
				Invoke-Compose up --detach
				$pattern = $case.Mode -eq "post_hunt" ? '"event":"goal_selection".*"decision_reason":"magic_training_complete".*"to_goal":"hunt"' :
					'"event":"goal_selection".*"decision_reason":"hunt_deadline".*"to_goal":"hunt"'
				$logs = Wait-ForLog -Pattern $pattern
				Assert-MagicTrainingEvents -Logs $logs -Mode $case.Mode
			}
		}
		Invoke-Scenario -Name "magic_training_restart" -DefaultTimeoutSeconds 120 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "magic_training_restart"
			Invoke-Compose up --detach
			$initialLogs = Wait-ForLog -Pattern '"action":"magic_training","result":"success"'
			Assert-MagicTrainingEvents -Logs $initialLogs -Mode "cast" -Spell "Light"
			$cast = @(ConvertFrom-PlayerbotLogs -Logs $initialLogs | Where-Object {
				$_.event -eq "action_result" -and $_.action -eq "magic_training" -and $_.source -eq "engine_verification" -and $_.result -eq "success"
			})[0]
			$restartLineCount = @((Get-ServerLogs) -split "`r?`n").Count
			Invoke-Compose stop server
			$persistedMagicLevel = Invoke-DatabaseScalar -Query "SELECT maglevel FROM players WHERE name = 'Bot One'"
			$persistedManaSpent = Invoke-DatabaseScalar -Query "SELECT manaspent FROM players WHERE name = 'Bot One'"
			if ($persistedMagicLevel -ne $cast.magic_level_after -or $persistedManaSpent -ne $cast.mana_spent_after) {
				throw "Magic training cast progress was not persisted by clean shutdown. magicLevel=$persistedMagicLevel/$($cast.magic_level_after), manaSpent=$persistedManaSpent/$($cast.mana_spent_after)."
			}
			Invoke-Compose up --detach server
			Wait-ForLog -Pattern '"event":"goal_candidate".*"goal":"magic_training".*"reason":"next_tick_not_overflow"' | Out-Null
			$restartLogs = ((Get-ServerLogs) -split "`r?`n" | Select-Object -Skip $restartLineCount) -join "`n"
			Assert-MagicTrainingEvents -Logs $restartLogs -Mode "restart"
		}
		Invoke-Scenario -Name "magic_training_hunt" -DefaultTimeoutSeconds 90 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "magic_training_hunt"
			Invoke-Compose up --detach
			$logs = Wait-ForLog -Pattern '"action":"hunt_cycle","result":"started"'
			if (@(ConvertFrom-PlayerbotLogs -Logs $logs | Where-Object { $_.event -eq "action_result" -and $_.action -eq "magic_training" }).Count -ne 0) { throw "Magic training ran while hunting." }
		}
	}

	if ($CorpseLoot) {
		Invoke-Scenario -Name "corpse" -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "corpse"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			$corpseLogs = Wait-ForLog -Pattern '"reason":"corpse_not_lootable","expected_corpse_item_id":1987'
			Assert-CorpseEvents -Logs $corpseLogs
		}
		Invoke-Scenario -Name "corpse_inaccessible" -DefaultTimeoutSeconds 75 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "corpse_inaccessible"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			$corpseLogs = Wait-ForLog -Pattern '"action":"loot","result":"failed","reason":"corpse_inaccessible"'
			Assert-InaccessibleCorpseEvents -Logs $corpseLogs
		}
	}

	if ($DeathTelemetry) {
		Invoke-Scenario -Name "death" -DefaultTimeoutSeconds 45 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "death"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			$env:PLAYERBOT_RELOG_DELAY_SECONDS = "1"
			$env:PLAYERBOT_MAX_CONSECUTIVE_DEATHS = "2"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST DEATH_RECOVERY_STATE_PASS' | Out-Null
			$deathLogs = Wait-ForPlayerbotEvent -Predicate {
				$_.event -eq "lifecycle" -and $_.status -eq "recovery_abandoned" -and $_.reason -eq "death_loop_limit"
			}
			Start-Sleep -Seconds 1
			if ((Get-OnlineBotCount) -ne 0) {
				throw "Bot One remained online after death recovery was abandoned."
			}
			Assert-DeathEvents -Logs (Get-ServerLogs)
		}
	}

	if ($Healing) {
		Invoke-Scenario -Name "healing" -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "healing"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST HEALING_STATE_PASS' | Out-Null
			$healingLogs = Wait-ForLog -Pattern '"action":"plan","result":"success".*"destination":\{"x":32084,"y":32144,"z":5\}'
			Assert-HealingEvents -Logs $healingLogs
		}

		Invoke-Scenario -Name "healing_resupply" -DefaultTimeoutSeconds 90 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "healing_resupply"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST HEALING_RESUPPLY_STATE_PASS' | Out-Null
			$resupplyLogs = Wait-ForLog -Pattern '"event":"objective_transition".*"from":"service".*"to":"return_to_depot"'
			Assert-HealingResupplyEvents -Logs $resupplyLogs
		}
	}

	if ($ValueLoot) {
		Invoke-Scenario -Name "value" -DefaultTimeoutSeconds 90 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "value"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			$valueLogs = Wait-ForLog -Pattern '"action":"loot".*"result":"success".*"item_id":2826'
			Assert-ValueLootEvents -Logs $valueLogs
		}
	}
	"PLAYERBOT_GAMEPLAY_TEST PASS"
}
finally {
	try {
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
