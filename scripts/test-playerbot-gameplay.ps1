param(
	[ValidateRange(30, 3600)]
	[int]$TimeoutSeconds = 300,
    [switch]$FullNavigation,
    [switch]$CorpseLoot,
    [switch]$DeathTelemetry,
	[switch]$Healing,
	[switch]$ValueLoot,
	[switch]$PickupProgression,
	[switch]$GoalArbitration,
	[switch]$OracleDeparture,
	[switch]$StaminaProjection,
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
            $_.event -eq "lifecycle" -and $_.status -eq "online" -and $_.objective -eq "departure_complete"
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
    $stopped = @($events | Where-Object {
        $_.event -eq "state_transition" -and $_.to -eq "stopped"
    })
    $wrongStoppedCount = if ($InterruptedByRestart) { $stopped.Count -lt 1 } else { $stopped.Count -ne 1 }
    if ($candidate.Count -lt 1 -or $selection.Count -lt 1 -or $result.Count -ne 1 -or
        $goalResult.Count -ne 1 -or $wrongStoppedCount) {
        throw "The bot did not complete and verify the selected Oracle departure: candidate=$($candidate.Count), selection=$($selection.Count), result=$($result.Count), goalResult=$($goalResult.Count), stopped=$($stopped.Count)."
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
        $_.event -eq "action_result" -and $_.action -eq "buy_meat" -and $_.result -eq "success"
    })
    $transactionFailures = @($events | Where-Object {
        $_.reason -eq "transaction_delta_mismatch" -or $_.reason -eq "shop_transaction_delta_mismatch"
    })
    if ($missingSupply.Count -ne 1 -or $flaskSales.Count -ne 1 -or $purchases.Count -lt 1 -or $heals.Count -lt 1) {
        throw "The bot did not refill and consume potions after the missing-supply healing outcome."
    }
    if ($serviceResumed.Count -lt 1) {
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
        $_.discarded_item_id -eq 2992 -and $_.discarded_count -eq 1 -and $_.discarded_value -eq 2 -and
        $_.incoming_item_id -eq 2826
    })
    $incomingLoot = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "loot" -and $_.result -eq "success" -and
        $_.item_id -eq 2826 -and $_.count -eq 1 -and $_.unit_value -eq 5 -and $_.total_value -eq 5
    })
    $capacitySkips = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "loot" -and $_.reason -eq "no_capacity"
    })
    $bankFundedPurchase = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "buy_potions" -and $_.result -eq "success" -and
        $_.bank_after -lt $_.bank_before
    })
    if ($replacement.Count -ne 1 -or $incomingLoot.Count -ne 1 -or $capacitySkips.Count -ne 0 -or
        $bankFundedPurchase.Count -ne 1) {
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
		$_.event -eq "strategy_selection" -and $_.candidate_id -ne 64120
    })
    $online = @($events | Where-Object {
        $_.event -eq "lifecycle" -and $_.status -eq "online" -and $_.objective -eq "pickup_reward"
    })
	if ($firstClaims.Count -ne 1 -or $nextSelection.Count -ne 1 -or $online.Count -ne 2) {
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
        $_.item_count -ge 5 -and $_.container_count -eq 1 -and $_.equipment_upgrade_count -eq 3 -and
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

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
	throw "Docker is required to run the playerbot gameplay suite."
}

$focusedScenarioRequested = $FullNavigation -or $CorpseLoot -or $DeathTelemetry -or $Healing -or $ValueLoot -or
    $PickupProgression -or $GoalArbitration -or $OracleDeparture -or $StaminaProjection
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
			$restartLogs = Wait-ForLog -Pattern '"event":"strategy_selection".*"candidate_id":(?!64120)'
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
            $restartLogs = Wait-ForLog -Pattern '"objective":"departure_complete"'
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
			$resupplyLogs = Wait-ForLog -Pattern '"action":"buy_meat".*"result":"success"'
			Assert-HealingResupplyEvents -Logs $resupplyLogs
		}
	}

	if ($ValueLoot) {
		Invoke-Scenario -Name "value" -DefaultTimeoutSeconds 90 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "value"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			$valueLogs = Wait-ForLog -Pattern '"action":"buy_potions".*"result":"success"'
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
		foreach ($timing in $timings.GetEnumerator()) {
			"PLAYERBOT_GAMEPLAY_TIMING $($timing.Key)=$([Math]::Round($timing.Value.TotalSeconds, 2))s"
		}
	}
}
