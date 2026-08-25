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
			$_.action -eq "buy_potions" -and $_.result -eq "success" -and $_.item_id -eq 7618 -and $_.count -eq 9
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
	$healthPotions = @($latest.requirements | Where-Object {
		$_.name -eq "health_potions" -and $_.item_id -eq 7618
	})
	if ($healthPotions.Count -ne 1) { throw "Knight readiness did not select regular health potions." }
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
        $affordable = @($candidates | Where-Object { $_.reason -eq "unaffordable_after_reserves" })
		if ($shadow[0].result -ne "no_decision" -or $nonImproving.Count -lt 1 -or $affordable.Count -ne 0) {
            throw "Equipment shadow did not preserve the two-handed loadout against non-improving offers."
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
			$_.event -eq "lifecycle" -and $_.status -eq "online" -and -not $_.recovered -and $_.objective -eq "fixture_pending"
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
		$purchases[0].carried_before -ne 5 -or $purchases[0].carried_after -ne 0 -or
		$purchases[0].bank_before -ne 100 -or $purchases[0].bank_after -ne 100 -or
		-not $equips[0].combat_ready -or -not $equips[0].displaced_items_preserved -or $terminal.Count -ne 0) {
		$purchase = $purchases | Select-Object -First 1
		$equip = $equips | Select-Object -First 1
		$terminalReasons = ($terminal | ForEach-Object { $_.reason }) -join ","
		$trace = @($events | Where-Object {
			$_.event -in @("goal_selection", "action_failure", "terminal")
		} | ForEach-Object { $_ | ConvertTo-Json -Compress }) -join "; "
		throw "Justified equipment purchase was incomplete. selections=$($selections.Count), purchases=$($purchases.Count), equips=$($equips.Count), results=$($results.Count), terminal=$($terminal.Count)[$terminalReasons], carried=$($purchase.carried_before)/$($purchase.carried_after), bank=$($purchase.bank_before)/$($purchase.bank_after), ready=$($equip.combat_ready), preserved=$($equip.displaced_items_preserved). trace=[$trace]"
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
        throw "Critical healing did not interrupt pickup and force service before reward claim. result=$($result.Count), critical_service=$($criticalService.Count), selection=$($selection.Count), claim=$($claim.Count)."
    }
}
