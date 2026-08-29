function Assert-NavigationEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $waypoints = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "hunt_waypoint" -and $_.result -eq "reached"
    })
    $actual = @($waypoints[0..4] | ForEach-Object { "$($_.position.x),$($_.position.y),$($_.position.z)" })
    $forward = @("32084,32144,5", "32103,32124,8", "32117,32090,9", "32103,32124,8", "32084,32144,5")
    $reverse = @("32117,32090,9", "32103,32124,8", "32084,32144,5", "32103,32124,8", "32117,32090,9")
    if (($actual -join '|') -notin @(($forward -join '|'), ($reverse -join '|'))) {
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

function Assert-CarlinServiceRouteEvents {
	param([string]$Logs)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$reached = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "hunt_waypoint" -and $_.result -eq "reached" -and
		$_.position.x -eq 32338 -and $_.position.y -eq 31791 -and $_.position.z -eq 7
	})
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	if ($reached.Count -ne 1 -or $terminal.Count -ne 0) {
		throw "Carlin service route failed. reached=$($reached.Count), terminal=$($terminal.Count)."
	}
}

function Assert-MutablePortalRouteEvents {
	param([string]$Logs)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$reached = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "hunt_waypoint" -and $_.result -eq "reached" -and
		$_.position.x -eq 32181 -and $_.position.y -eq 31794 -and $_.position.z -eq 8
	})
	$transitionFailures = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "navigate" -and
		$_.result -eq "failed" -and $_.reason -eq "transition_unavailable"
	})
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	if ($reached.Count -ne 1 -or $transitionFailures.Count -ne 0 -or $terminal.Count -ne 0) {
		throw "Mutable portal route failed. reached=$($reached.Count), transitionFailures=$($transitionFailures.Count), terminal=$($terminal.Count)."
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

function Assert-TargetApproachEvents {
	param([string]$Logs)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$selected = @($events | Where-Object {
		$_.event -eq "target_changed" -and $_.reason -eq "visible_monster"
	})
	$approachPlan = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "plan" -and $_.result -eq "success" -and $_.same_floor
	})
	$defeated = @($events | Where-Object {
		$_.event -eq "target_changed" -and $_.reason -eq "target_defeated"
	})
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	$fixture = [regex]::Match($Logs, "PLAYERBOT_GAMEPLAY_TEST TARGET_APPROACH_START (\d+) (\d+)")
	$closestTargetId = $fixture.Success ? [uint64]$fixture.Groups[1].Value : 0
	if ($selected.Count -ne 1 -or $approachPlan.Count -lt 1 -or $defeated.Count -lt 1 -or
		$closestTargetId -eq 0 -or $selected[0].target_id -ne $closestTargetId -or
		$selected[0].target_id -ne $defeated[0].previous_target_id -or $terminal.Count -ne 0) {
		throw "Target approach failed. selected=$($selected.Count), closest=$closestTargetId, plans=$($approachPlan.Count), defeated=$($defeated.Count), terminal=$($terminal.Count)."
	}
}

function Assert-UnreachableTargetApproachEvents {
	param([string]$Logs)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$skipped = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "target_approach" -and $_.result -eq "skipped" -and
		$_.reason -eq "route_unavailable" -and $_.same_floor
	})
	$patrolPlan = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "plan" -and $_.result -eq "success" -and -not $_.same_floor
	})
	$selected = @($events | Where-Object { $_.event -eq "target_changed" -and $_.reason -eq "visible_monster" })
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	if ($skipped.Count -ne 1 -or $patrolPlan.Count -lt 1 -or $selected.Count -ne 0 -or $terminal.Count -ne 0) {
		throw "Unreachable target fallback failed. skipped=$($skipped.Count), patrolPlans=$($patrolPlan.Count), selected=$($selected.Count), terminal=$($terminal.Count)."
	}
}

function Assert-TargetAttackerPriorityEvents {
	param([string]$Logs)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$initial = @($events | Where-Object {
		$_.event -eq "target_changed" -and $_.target_name -eq "Playerbot Empty Corpse"
	})
	$preempted = @($events | Where-Object {
		$_.event -eq "target_changed" -and $_.reason -eq "active_attacker_preempted"
	})
	$attacker = @($events | Where-Object {
		$_.event -eq "target_changed" -and $_.target_name -eq "Playerbot Defensive Threat"
	})
	$defeated = @($events | Where-Object {
		$_.event -eq "target_changed" -and $_.reason -eq "target_defeated"
	})
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	if ($initial.Count -ne 1 -or $preempted.Count -ne 1 -or $attacker.Count -ne 1 -or
		$preempted[0].previous_target_id -ne $initial[0].target_id -or
		$attacker[0].target_id -eq $initial[0].target_id -or $defeated.Count -lt 1 -or
		$defeated[0].previous_target_id -ne $attacker[0].target_id -or $terminal.Count -ne 0) {
		throw "Active attacker priority failed. initial=$($initial.Count), preempted=$($preempted.Count), attacker=$($attacker.Count), defeated=$($defeated.Count), terminal=$($terminal.Count)."
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
	$failedDetour = @($events | Where-Object {
		$_.event -eq "navigation_progress" -and $_.result -eq "failed" -and
		$_.reason -eq "route_unavailable"
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
	$blockerTimedOut = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "defensive_combat" -and
		$_.result -eq "failed" -and $_.reason -eq "combat_timeout"
	})
	$controllerTerminal = @($events | Where-Object { $_.event -eq "terminal" })
	if ($failedDetour.Count -lt 1 -or $combatPreemption.Count -ne 1 -or $blockerEngaged.Count -ne 1 -or
		$blockerTimedOut.Count -ne 1 -or
		$terminalResult.Count -ne 1 -or
		$terminalResult[0].target_id -le 0 -or
		$terminalResult[0].navigation_failures -gt 6 -or $terminalResult[0].elapsed_ms -gt 70000 -or
		$controllerTerminal.Count -ne 0) {
		throw "Inaccessible corpse work was not bounded. failed_detour=$($failedDetour.Count), preemption=$($combatPreemption.Count)/$($blockerEngaged.Count)/$($blockerTimedOut.Count), results=$($terminalResult.Count), terminal=$($controllerTerminal.Count)."
	}
}

function Assert-CorpseDetourEvents {
	param([string]$Logs)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$detours = @($events | Where-Object {
		$_.event -eq "navigation_progress" -and $_.reason -eq "hostile_detour" -and
		$_.blocker_id -gt 0
	})
	$loot = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "loot" -and $_.result -eq "success"
	})
	$defensiveCombat = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "defensive_combat" -and $_.result -eq "started"
	})
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	if ($detours.Count -lt 1 -or $loot.Count -lt 1 -or $defensiveCombat.Count -ne 0 -or $terminal.Count -ne 0) {
		throw "The corpse blocker was not bypassed. detours=$($detours.Count), loot=$($loot.Count), defensive=$($defensiveCombat.Count), terminal=$($terminal.Count)."
	}
}

function Assert-DepotEvents {
	param([string]$Logs, [int]$ExpectedDepositedCount, [int]$ExpectedEquipmentDeposits)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$discovery = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "depot_discover" -and
		$_.result -eq "success" -and $_.locker.x -eq 32352 -and $_.locker.y -eq 32225 -and
		$_.locker.z -eq 7 -and $_.approach.x -eq 32352 -and $_.approach.y -eq 32226 -and $_.approach.z -eq 7 -and
		$_.distance -ge 0 -and $_.route_steps -ge 0 -and $_.expanded_nodes -ge 0 -and $_.indexed -ge $_.in_scope -and
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
		$_.item_id -in @(2395, 2461, 2643)
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
		throw "Real Thais depot evidence was incomplete. discovery=$($discovery.Count), depotId=$depotId, locker=$($locker.Count), chest=$($chest.Count), deposited=$deposited/$ExpectedDepositedCount, equipment_deposits=$($equipmentDeposits.Count)/$ExpectedEquipmentDeposits, equipment_upgrades=$($equipmentUpgrades.Count)/$(3 * [Math]::Min($ExpectedEquipmentDeposits, 1)), complete=$($complete.Count), unsafe=$($unsafeMoves.Count), terminal=$($terminal.Count)."
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
	$localPlans = @($events | Where-Object {
		$_.event -eq "sell_loot_plan" -and $_.result -eq "candidate" -and $_.item_id -eq 2398 -and $_.utility -lt 0
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
	$expectedSales = if ($SellerAvailable) { 1 } else { 0 }
	if (($Restarted -and ($depositRequests.Count -ne 1 -or $deposits.Count -ne 0)) -or
		(-not $Restarted -and $deposits.Count -ne 1) -or $sales.Count -ne $expectedSales -or $sellMoves.Count -ne 0) {
		throw "Slotted loot did not use the expected local disposition. requests=$($depositRequests.Count), deposits=$($deposits.Count), sales=$($sales.Count)/$expectedSales, sell_moves=$($sellMoves.Count), seller=$SellerAvailable, restarted=$Restarted."
	}
	if ($SellerAvailable -and $localPlans.Count -lt 1) {
		throw "The local sale was still gated by global utility."
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
		$_.depot_id -eq 2 -and $_.locker_item_id -eq 2589 -and
		[Math]::Abs($_.locker.x - $_.approach.x) -le 1 -and
		[Math]::Abs($_.locker.y - $_.approach.y) -le 1 -and $_.locker.z -eq $_.approach.z
	})
	$trainingRoomDepot = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "depot_discover" -and $_.result -eq "success" -and
		$_.locker.x -eq 32276 -and $_.locker.y -eq 32218 -and $_.locker.z -eq 11
	})
	$selection = @($events | Where-Object {
		$_.event -eq "hunt_region_selection" -and $_.result -eq "selected" -and
		$_.atlas_site_id -gt 0 -and $_.atlas_variant_id -gt 0 -and $_.atlas_spawns -gt 0
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
		$selection.Count -lt 1 -or $remoteBuyerDeposit.Count -lt 1 -or
		$remoteBuyerSale.Count -ne 0 -or $rookService.Count -ne 0 -or $terminal.Count -ne 0) {
		throw "Mainland loop failed. hunts=$($hunts.Count), deposits=$($deposits.Count), realDepot=$($realDepot.Count), trainingRoomDepot=$($trainingRoomDepot.Count), localSelections=$($selection.Count), remoteBuyerDeposits=$($remoteBuyerDeposit.Count), rookService=$($rookService.Count), terminal=$($terminal.Count)."
	}
}
