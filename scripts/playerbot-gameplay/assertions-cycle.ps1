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
    $capabilityAudit = @($events | Where-Object {
        $_.event -eq "npc_capability_audit" -and $_.result -eq "ok" -and $_.findings -eq 0 -and
        $_.shop_providers -gt 0 -and $_.spell_trainers -gt 0 -and $_.travel_offers -gt 0
    })
	$depositedLoot = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "deposit" -and $_.result -eq "success" -and
		$_.item_id -eq 2992 -and $_.count -eq 1
	})
	$shopTransactions = @($events | Where-Object { $_.action -in @("buy_potions", "sell") })
	$bankDeposits = @($events | Where-Object {
		$_.action -eq "bank_deposit" -and $_.result -eq "success" -and $_.count -eq 100 -and
		$_.bank_before -eq 1000 -and $_.bank_after -eq 1100
	})
	$bankWithdrawals = @($events | Where-Object {
		$_.action -eq "bank_withdraw" -and $_.result -eq "success" -and $_.count -eq 100 -and
		$_.bank_before -eq 1100 -and $_.bank_after -eq 1000
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
        $_.reason -in @("defensive_attacker", "defensive_path_blocker")
    })
    $defensiveBlockerIds = @($defensiveBlockerTargets | ForEach-Object { $_.target_id })
    $defensiveStart = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "defensive_combat" -and
        $_.result -eq "started" -and $_.chase -eq $false -and
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
        throw "The bot did not discover the live shop and banker capabilities."
    }
    if ($capabilityAudit.Count -ne 1) {
        throw "The startup NPC capability audit was missing or reported findings."
    }
	if (@($events | Where-Object { $_.event -eq "service_catalog" }).Count -ne 0) {
		throw "The bot probed a shop window instead of using the live NPC offer catalog."
	}
	if ($depositedLoot.Count -ne 1 -or $shopTransactions.Count -ne 0 -or
		$bankDeposits.Count -ne 1 -or $bankWithdrawals.Count -ne 1) {
		throw "The bot did not deposit loot while preserving completed service reserves."
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

function Assert-CarlinLocalServiceEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $purchases = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "buy_potions" -and $_.result -eq "success" -and
        $_.item_id -eq 7618 -and $_.count -eq 9 -and $_.position.x -ge 32340 -and $_.position.x -le 32346 -and
        $_.position.y -ge 31825 -and $_.position.y -le 31831 -and $_.position.z -eq 7
	})
	$rachelReplies = @($events | Where-Object { $_.event -eq "npc_reply" -and $_.npc_name -eq "Rachel" })
	$evaReplies = @($events | Where-Object { $_.event -eq "npc_reply" -and $_.npc_name -eq "Eva" })
	$withdrawals = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "bank_withdraw" -and $_.result -eq "success" -and
		$_.count -eq 100 -and $_.bank_before -eq 100 -and $_.bank_after -eq 0 -and
		$_.position.x -ge 32323 -and $_.position.x -le 32329 -and
		$_.position.y -ge 31777 -and $_.position.y -le 31783 -and $_.position.z -eq 7
	})
	$remoteAttempts = @($events | Where-Object {
		($_.event -eq "npc_reply" -and $_.npc_name -eq "Xodet") -or
		($_.event -eq "service_provider_rejected" -and $_.npc_name -eq "Xodet") -or
		($_.event -eq "npc_reply" -and $_.npc_name -in @("Lokur", "Suzy")) -or
		($_.event -eq "npc_travel")
	})
	$terminals = @($events | Where-Object { $_.event -eq "terminal" })
	$sales = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "sell" })
	if ($purchases.Count -ne 1 -or $rachelReplies.Count -lt 1 -or $evaReplies.Count -lt 1 -or
		$withdrawals.Count -ne 1 -or $sales.Count -ne 0 -or $remoteAttempts.Count -ne 0 -or $terminals.Count -ne 0) {
		throw "Carlin service was not completed locally without selling at Rachel and Eva. purchases=$($purchases.Count), rachel=$($rachelReplies.Count), eva=$($evaReplies.Count), withdrawals=$($withdrawals.Count), sales=$($sales.Count), remote=$($remoteAttempts.Count), terminal=$($terminals.Count)."
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
	if ($candidate.Count -lt 1 -or $selection.Count -lt 1 -or $result.Count -ne 1 -or
		$goalResult.Count -ne 1) {
		throw "The bot did not complete the selected Oracle departure: candidate=$($candidate.Count), selection=$($selection.Count), result=$($result.Count), goalResult=$($goalResult.Count)."
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
	$flaskDeposits = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "deposit" -and $_.result -eq "success" -and
		$_.item_id -eq 7636 -and $_.count -eq 1
	})
    $heals = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "heal" -and $_.result -eq "success" -and
        $_.objective -eq "service" -and $_.resource_after -eq ($_.resource_before - 1)
    })
	$serviceResumed = @($events | Where-Object {
		$_.event -eq "objective_transition" -and $_.from -eq "service" -and $_.to -eq "hunt"
	})
	$foodPurchases = @($events | Where-Object { $_.action -eq "buy_meat" })
	$lootSales = @($events | Where-Object { $_.action -eq "sell" })
    $transactionFailures = @($events | Where-Object {
        $_.reason -eq "transaction_delta_mismatch" -or $_.reason -eq "shop_transaction_delta_mismatch"
    })
	if ($missingSupply.Count -ne 1 -or $flaskDeposits.Count -ne 1 -or $purchases.Count -lt 1 -or $heals.Count -lt 1) {
		throw "The bot did not deposit loot, refill, and consume potions after the missing-supply healing outcome: missing=$($missingSupply.Count), flaskDeposits=$($flaskDeposits.Count), purchases=$($purchases.Count), heals=$($heals.Count)."
	}
	if ($serviceResumed.Count -lt 1 -or $foodPurchases.Count -ne 0 -or $lootSales.Count -ne 0) {
		throw "The bot did not resume hunting directly after potion-only service."
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
		$_.discarded_item_id -eq 2398 -and $_.discarded_count -eq 1 -and $_.discarded_value -lt 100 -and
		$_.incoming_item_id -eq 2152
    })
    $incomingLoot = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "loot" -and $_.result -eq "success" -and
		$_.item_id -eq 2152 -and $_.count -eq 1 -and $_.unit_value -eq 100 -and $_.total_value -eq 100
    })
    $capacitySkips = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "loot" -and $_.reason -eq "no_capacity"
    })
	$foodPurchases = @($events | Where-Object { $_.action -eq "buy_meat" })
	if ($replacement.Count -ne 1 -or $incomingLoot.Count -ne 1 -or $capacitySkips.Count -ne 0 -or
		$foodPurchases.Count -ne 0) {
		throw "The bot did not replace lower-density cargo with the denser corpse item."
    }
}
