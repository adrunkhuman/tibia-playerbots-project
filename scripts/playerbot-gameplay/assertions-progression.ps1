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

function Assert-UnreachableEquipmentProviderEvents {
	param([string]$Logs)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$routeFailures = @($events | Where-Object {
		$_.event -eq "navigation_progress" -and $_.result -eq "failed" -and
		$_.reason -eq "route_unavailable"
	})
	$goalFailure = @($events | Where-Object {
		$_.event -eq "goal_result" -and $_.goal -eq "buy_equipment" -and
		$_.result -eq "failed" -and $_.reason -eq "route_unavailable"
	})
	$purchaseActions = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -in @("buy_equipment", "equip_equipment")
	})
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	if ($routeFailures.Count -ne 1 -or $goalFailure.Count -ne 1 -or
		$purchaseActions.Count -ne 0 -or $terminal.Count -ne 0) {
		throw "Unreachable NPC approach was not bounded. routeFailures=$($routeFailures.Count), goalFailure=$($goalFailure.Count), purchaseActions=$($purchaseActions.Count), terminal=$($terminal.Count)."
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
        $sequence = ($selections | ForEach-Object {
            "$($_.decision_id):$($_.decision_reason):$($_.from_goal)->$($_.to_goal)"
        }) -join ", "
        throw "Goal arbitration did not select the expected pickup, service, and hunt sequence. observed=[$sequence]"
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
		$corridorPenalty = if ($candidate.corridor_danger_available) {
			[Math]::Min(0.75, $candidate.corridor_danger_ratio * 0.5)
		} else {
			0.0
		}
		$expectedScore = $candidate.projected_experience * (1.0 - $corridorPenalty)
		if ($candidate.stamina_minutes -ne $StaminaMinutes -or
			[Math]::Abs($candidate.stamina_experience_multiplier - $multiplier) -gt 0.01 -or
			[Math]::Abs($candidate.projected_experience - $expected) -gt $tolerance -or
			[Math]::Abs($candidate.score - $expectedScore) -gt $tolerance) {
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
    $topologyScans = @($events | Where-Object {
		$_.event -eq "hunt_region_scan" -and $_.selection_strategy -eq "atlas_topology_selection"
    })
    $selections = @($events | Where-Object { $_.event -eq "hunt_region_selection" -and $_.result -eq "selected" })
    $selection = if ($selections.Count -gt 0) { $selections[$selections.Count - 1] } else { $null }
    $candidates = @($events | Where-Object { $_.event -eq "hunt_region_candidate" })
    $outsideLocalFixture = @($candidates | Where-Object {
		[Math]::Max([Math]::Abs($_.center.x - 32360), [Math]::Abs($_.center.y - 31782)) -gt 32
	})
    $completed = @($events | Where-Object {
		$_.event -eq "hunt_region_scan" -and $_.phase -eq "selected" -and $_.decision_latency_us -gt 0
    })
    $selectedCandidate = if ($selection) { @($candidates | Where-Object { $_.region_id -eq $selection.region_id }) } else { @() }
	$reachableCandidates = @($candidates | Where-Object { $_.suitable -and $_.reachable })
    $bestScore = if ($reachableCandidates.Count -gt 0) { ($reachableCandidates | Measure-Object -Property score -Maximum).Maximum } else { $null }
	$completedTopology = @($completed | Where-Object {
		$_.topology_time_us -ge 0
	})
	$routeValidations = @($candidates | Where-Object { $_.topology_reachable -and $_.topology_travel_steps -gt 0 })
	$supplyReserves = @($events | Where-Object {
		$_.event -eq "hunt_supply_reserve" -and $_.source -eq "selected_route_reverse_estimate" -and
		$_.return_threshold -ge 1 -and $_.restock_target -ge 10 -and $_.restock_target -gt $_.return_threshold
	})
	$reserveFormulaValid = $false
	if ($supplyReserves.Count -eq 1) {
		$reserve = $supplyReserves[0]
		$expectedHealthLoss = [Math]::Ceiling($reserve.maximum_health * $reserve.route_danger_cost / $reserve.health_loss_cost)
		$expectedThreshold = [Math]::Max(1, [Math]::Ceiling($expectedHealthLoss / $reserve.minimum_potion_healing))
		$reserveFormulaValid = $reserve.return_threshold -eq $expectedThreshold
	}
	if ($build.Count -lt 2 -or $hit.Count -lt 1 -or $cancelled.Count -ne 1 -or $staleRevision.Count -ne 1 -or
		$topologyScans.Count -lt 1 -or $routeValidations.Count -lt 1 -or -not $selection -or $selectedCandidate.Count -ne 1 -or
		$bestScore -eq $null -or [Math]::Abs($selectedCandidate[0].score - $bestScore) -gt 0.01 -or
		-not $selectedCandidate[0].topology_reachable -or $selectedCandidate[0].topology_travel_steps -lt 1 -or
		-not $selectedCandidate[0].reachable -or $selectedCandidate[0].route_danger_cost -lt 0 -or
		$outsideLocalFixture.Count -lt 1 -or $completedTopology.Count -lt 1 -or
		-not $reserveFormulaValid) {
		throw "Hunt planning telemetry was incomplete. build=$($build.Count), hit=$($hit.Count), cancelled=$($cancelled.Count), stale=$($staleRevision.Count), topology_scans=$($topologyScans.Count), route_validations=$($routeValidations.Count), selection=$($selection.Count), outside=$($outsideLocalFixture.Count), completed=$($completedTopology.Count)."
	}
}

function Assert-HuntAreaArrivalEvents {
	param([string]$Logs)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$entry = @($events | Where-Object { $_.event -eq "hunt_area_entered" })
	$entryIndex = -1
	$targetIndex = -1
	$targetsBeforeEntry = 0
	$waypointsBeforeCombat = 0
	for ($index = 0; $index -lt $events.Count; $index++) {
		$event = $events[$index]
		if ($entryIndex -lt 0 -and $event.event -eq "hunt_area_entered") { $entryIndex = $index }
		if ($event.event -eq "target_changed" -and $event.reason -eq "visible_monster" -and $event.target_type -eq "monster") {
			if ($entryIndex -lt 0) { $targetsBeforeEntry++ }
			elseif ($targetIndex -lt 0) { $targetIndex = $index }
		}
		if ($targetIndex -lt 0 -and $event.event -eq "action_result" -and $event.action -eq "hunt_waypoint") {
			$waypointsBeforeCombat++
		}
	}
	$target = if ($targetIndex -ge 0) { $events[$targetIndex] } else { $null }
	$candidates = @($events | Where-Object { $_.event -eq "hunt_region_candidate" })
	$selected = if ($entry.Count -gt 0) { @($candidates | Where-Object { $_.region_id -eq $entry[0].region_id }) } else { @() }
	$monsterNames = if ($selected.Count -gt 0) { @($selected[0].monsters | ForEach-Object { $_.name }) } else { @() }
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	if ($entry.Count -ne 1 -or -not $target -or $targetsBeforeEntry -ne 0 -or $waypointsBeforeCombat -ne 0 -or
		$target.target_name -notin $monsterNames -or $terminal.Count -ne 0) {
		throw "Hunt-area arrival did not start matching combat before the first waypoint. entry=$($entry.Count), target=$($targetIndex -ge 0), prior_targets=$targetsBeforeEntry, prior_waypoints=$waypointsBeforeCombat, selected=$($selected.Count), terminal=$($terminal.Count)."
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
	$postTerminalEvents = @()
	if ($terminal.Count -eq 1) {
		$terminalIndex = [Array]::IndexOf($events, $terminal[0])
		if ($terminalIndex -lt $events.Count - 1) {
			$postTerminalEvents = @($events[($terminalIndex + 1)..($events.Count - 1)])
		}
	}
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
		-not $fixture[0].higher_score_preferred -or -not $fixture[0].closest_target_preferred -or -not $fixture[0].wounded_lethal -or
		-not $fixture[0].zero_health_lethal -or -not $fixture[0].helper_scope_exhausted -or
		$fixture[0].capacity_before_grace -or -not $fixture[0].capacity_at_grace -or -not $fixture[0].capacity_cycle_reset -or
		$fixture[0].knight_route_reserve -ne 4 -or $fixture[0].rook_route_reserve -ne 9 -or
		$fixture[0].high_health_route_reserve -ne 12 -or $fixture[0].high_health_restock_target -ne 13 -or
		-not $fixture[0].net_value_loss_rejected -or -not $fixture[0].currency_priority_override -or
		-not $fixture[0].currency_hunt_capacity_excluded -or
		-not $fixture[0].large_restock_batched -or
		-not $fixture[0].preferred_food_consumed -or -not $fixture[0].missing_food_ignored -or
		-not $fixture[0].food_replenished_after_eating -or
		$candidates.Count -lt 1 -or $unsafeLethalRecovery.Count -ne 0 -or $exhausted.Count -ne 3 -or
		@($exhausted | Where-Object { $_.attempt -notin @(1, 2, 3) }).Count -ne 0 -or
		$terminal.Count -ne 1 -or $postTerminalEvents.Count -ne 0) {
		throw "Adaptive challenge evidence was incomplete. idle=$($idle.Count), noKill=$($noKill.Count), invalidEscalation=$($invalidEscalation.Count), escalated=$($escalated.Count), backoff=$($backoff.Count), hold=$($hold.Count), fixture=$($fixture.Count), candidates=$($candidates.Count), exhausted=$($exhausted.Count), terminal=$($terminal.Count), post_terminal=$($postTerminalEvents.Count)."
	}
}
