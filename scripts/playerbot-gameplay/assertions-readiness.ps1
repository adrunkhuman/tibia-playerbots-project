function Assert-LowWealthEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $withdrawals = @($events | Where-Object { $_.action -eq 'bank_withdraw' })
    $upgrades = @($events | Where-Object { $_.action -eq 'equip_readiness' -and $_.result -eq 'success' })
    $hunts = @($events | Where-Object { $_.event -eq 'action_result' -and $_.action -eq 'hunt_cycle' -and $_.result -eq 'started' })
    $forbidden = @($events | Where-Object { $_.action -eq 'sell' -or $_.event -eq 'terminal' })
    if ($withdrawals.Count -ne 1 -or $withdrawals[0].event -ne 'action_result' -or
        $withdrawals[0].result -ne 'success' -or $withdrawals[0].count -ne 56 -or
        $withdrawals[0].bank_before -ne 56 -or $withdrawals[0].bank_after -ne 0 -or
        $upgrades.Count -ne 1 -or $upgrades[0].event -ne 'action_result' -or
        $upgrades[0].result -ne 'success' -or $upgrades[0].item_id -ne 2384 -or
        $hunts.Count -lt 1 -or $forbidden.Count -ne 0 -or
        $Logs -notmatch '(?m)^PLAYERBOT_GAMEPLAY_TEST READINESS_LOW_WEALTH_PASS\r?$') {
        throw 'Low-wealth banking did not retain the upgrade and carry exactly 56 gp into hunting without sales.'
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
			$_.action -eq "buy_potions" -and $_.result -eq "success" -and $_.item_id -eq 7618 -and $_.count -eq 19
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
		$initial = $readiness[0]
		$capacity = @($initial.requirements | Where-Object {
			$_.name -eq "free_capacity" -and $_.ready -and $_.current -eq 0 -and
			$_.reclaimable_food -eq 3200 -and $_.effective -ge $_.minimum
		})
		$food = @($initial.requirements | Where-Object {
			$_.name -eq "food" -and $_.count -eq 8 -and $_.reclaimable_weight -eq 3200
		})
		$service = @($events | Where-Object { $_.selected_recovery -eq "service" -or $_.action -eq "buy_meat" })
		$hunts = @($events | Where-Object { $_.action -eq "hunt_cycle" -and $_.result -eq "started" })
		$eaten = @($events | Where-Object { $_.action -eq "eat" })
		if ($capacity.Count -ne 1 -or $food.Count -ne 1 -or $service.Count -ne 0 -or $hunts.Count -lt 1 -or $eaten.Count -ne 8) {
			throw "Food weight did not remain reclaimable when physical capacity was exhausted."
		}
		for ($i = 0; $i -lt 8; $i++) {
			$eat = $eaten[$i]
			if ($eat.event -ne "action_result" -or $eat.result -ne "success" -or $eat.item_id -ne 2696 -or
				$eat.count -ne 1 -or $eat.inventory_count -ne (7 - $i) -or $eat.food_ticks -le 0) {
				throw "Food consumption did not verify all eight initial cheeses in order."
			}
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
	param([string]$Logs, [switch]$Rejected, [switch]$Restart, [switch]$Resume, [switch]$ProviderMoved, [switch]$Spear)

	if ($Spear) {
		Assert-ZeroSpearRecoveryEvents -Logs $Logs -Mode "startup"
		return
	}
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
	$staleProviderFailures = @($results | Where-Object { $_.reason -eq "provider_moved" })
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
	if ($ProviderMoved) {
		$selections = @($events | Where-Object {
			$_.event -eq "goal_selection" -and $_.to_goal -eq "buy_equipment" -and $_.item_id -eq 2648 -and $_.price -eq 80
		})
		$focus = @($events | Where-Object { $_.event -eq "npc_reply" -and $_.npc_name -eq "Cornelia" })
		if ($selections.Count -ne 1 -or $purchases.Count -ne 1 -or $purchases[0].item_id -ne 2648 -or
			$equips.Count -ne 1 -or $equips[0].item_id -ne 2648 -or $results.Count -ne 1 -or
			$results[0].result -ne "success" -or $staleProviderFailures.Count -ne 0 -or
			$focus.Count -lt 1 -or $terminal.Count -ne 0) {
			throw "Moving equipment provider was not reacquired through a focused, successful purchase."
		}
		return
	}
	$selections = @($events | Where-Object {
		$_.event -eq "goal_selection" -and $_.to_goal -eq "buy_equipment" -and $_.item_id -eq 2379 -and $_.price -eq 5
	})
	$selectedNpcId = if ($selections.Count -eq 1) { $selections[0].npc_id } else { $null }
	$focus = @($events | Where-Object {
		$_.event -eq "npc_reply" -and $_.npc_id -eq $selectedNpcId
	})
	if ($selections.Count -ne 1 -or $purchases.Count -ne 1 -or $equips.Count -ne 1 -or
		$results.Count -ne 1 -or $results[0].result -ne "success" -or
		$purchases[0].carried_before -ne 5 -or $purchases[0].carried_after -ne 0 -or
		$purchases[0].bank_before -ne 100 -or $purchases[0].bank_after -ne 100 -or
		-not $equips[0].combat_ready -or -not $equips[0].displaced_items_preserved -or $terminal.Count -ne 0 -or
		$focus.Count -lt 1 -or $staleProviderFailures.Count -ne 0) {
		$purchase = $purchases | Select-Object -First 1
		$equip = $equips | Select-Object -First 1
		$terminalReasons = ($terminal | ForEach-Object { $_.reason }) -join ","
		$trace = @($events | Where-Object {
			$_.event -in @("goal_selection", "action_failure", "terminal")
		} | ForEach-Object { $_ | ConvertTo-Json -Compress }) -join "; "
		throw "Justified equipment purchase was incomplete. selections=$($selections.Count), purchases=$($purchases.Count), equips=$($equips.Count), results=$($results.Count), terminal=$($terminal.Count)[$terminalReasons], carried=$($purchase.carried_before)/$($purchase.carried_after), bank=$($purchase.bank_before)/$($purchase.bank_after), ready=$($equip.combat_ready), preserved=$($equip.displaced_items_preserved). trace=[$trace]"
	}
}

function Assert-EquipmentToolReplenishmentEvents {
	param([string]$Logs, [switch]$Nested)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$expectedIds = if ($Nested) { @(2554) } else { @(2120, 2554) }
	$expectedCount = $expectedIds.Count
	$selections = @($events | Where-Object { $_.event -eq "goal_selection" -and $_.to_goal -eq "buy_equipment" })
	$purchases = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "buy_equipment" -and $_.result -eq "success" })
	$receipts = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "acquire_tool" -and $_.result -eq "success" })
	$results = @($events | Where-Object { $_.event -eq "goal_result" -and $_.goal -eq "buy_equipment" -and $_.result -eq "success" })
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	$groups = @($selections, $purchases, $receipts, $results)
	$groupValid = $true
	foreach ($group in $groups) {
		$ids = @($group | ForEach-Object { $_.item_id } | Sort-Object -Unique)
		$groupValid = $groupValid -and $group.Count -eq $expectedCount -and $ids.Count -eq $expectedCount -and
			@($ids | Where-Object { $_ -notin $expectedIds }).Count -eq 0 -and
			@($group | Where-Object { -not $_.tool_acquisition -or $_.npc_id -le 0 }).Count -eq 0
	}
	$selectedProviders = @{}
	foreach ($selection in $selections) { $selectedProviders["$($selection.item_id)"] = $selection.npc_id }
	$providerMatches = @($purchases + $receipts + $results | Where-Object {
		$selectedProviders["$($_.item_id)"] -ne $_.npc_id
	}).Count -eq 0
	$receiptReasonsValid = @($results | Where-Object { $_.reason -ne "tool_acquired" }).Count -eq 0
	$purchaseReceiptsValid = @($purchases | Where-Object {
		$_.price -le 0 -or ($_.carried_before + $_.bank_before - $_.carried_after - $_.bank_after) -ne $_.price
	}).Count -eq 0
	if (-not $groupValid -or -not $providerMatches -or -not $receiptReasonsValid -or -not $purchaseReceiptsValid -or $terminal.Count -ne 0) {
		throw "Tool replenishment did not complete exactly the expected discovered-shop purchases and receipts. selections=$($selections.Count), purchases=$($purchases.Count), receipts=$($receipts.Count), results=$($results.Count), terminal=$($terminal.Count)."
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

# Waits for a hunt that starts after the spear purchase, not the pre-break hunt.
function Wait-ForSpearRestockHunt {
	$script:spearRestockBought = $false
	return Wait-ForPlayerbotEvent {
		if ($_.action -eq "buy_throwing_weapons" -and $_.result -eq "success") { $script:spearRestockBought = $true }
		$script:spearRestockBought -and $_.action -eq "hunt_cycle" -and $_.result -eq "started"
	}
}

function Assert-SpareSpearEvents {
	param([string]$Logs, [ValidateSet("restock", "break")][string]$Mode, [ValidateSet(8, 16)][int]$Level = 8)

	$target = $Level -eq 8 ? 3 : 7
	$scenario = $Mode -eq "break" ? "spear_break" : $Level -eq 16 ? "spear_restock_scaled" : "spear_restock"
	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	for ($i = 0; $i -lt $events.Count; $i++) { $events[$i] | Add-Member -NotePropertyName log_index -NotePropertyValue $i -Force }
	$buys = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "buy_throwing_weapons" -and $_.result -eq "success" -and $_.item_id -eq 2389
	})
	$exhausted = @($events | Where-Object { $_.reason -eq "throwing_weapon_exhausted" })
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	$bought = ($buys | Measure-Object -Property count -Sum).Sum
	$fixture = @($events | Where-Object {
		$_.event -eq "spear_fixture" -and $_.source -eq "lua_setup_verifier" -and $_.scenario -eq $scenario -and $_.level -eq $Level
	})
	$start = @($fixture | Where-Object { $_.phase -eq "start" -and $_.spears -eq ($Mode -eq "break" ? 2 : 1) })
	$pass = @($fixture | Where-Object { $_.phase -eq "pass" -and $_.spears -eq $target })
	$resumed = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "hunt_cycle" -and $_.result -eq "started" -and
		$buys.Count -gt 0 -and $_.log_index -gt $buys[-1].log_index
	})
	$trace = { @($events | Where-Object {
		$_.event -in @("combat_readiness", "hunt_region_outcome", "terminal") -or
		$_.action -in @("equip_readiness", "buy_throwing_weapons", "hunt_cycle")
	} | ForEach-Object { $_ | ConvertTo-Json -Compress -Depth 4 }) -join "; " }
	if ($buys.Count -lt 1 -or $bought -ne ($target - 1) -or $resumed.Count -lt 1 -or
		$terminal.Count -ne 0 -or $start.Count -ne 1 -or $pass.Count -ne 1) {
		throw "Level-$Level Paladin spear $Mode did not restock to $target and start hunting. buys=$($buys.Count), bought=$bought, start=$($start.Count), pass=$($pass.Count), resumed=$($resumed.Count), terminal=$($terminal.Count). trace=[$(& $trace)]"
	}
	if ($Mode -eq "restock") {
		$recovery = @($events | Where-Object {
			$_.event -eq "combat_readiness" -and $_.result -eq "recovery" -and $_.selected_recovery -eq "service" -and
			$_.log_index -lt $buys[0].log_index -and @($_.requirements | Where-Object {
				$_.kind -eq "throwing_weapon" -and $_.ready -eq $false -and $_.count -eq 1 -and $_.restock_target -eq $target
			}).Count -eq 1
		})
		if ($recovery.Count -lt 1) { throw "Spear startup restock did not recover its observed readiness deficit." }
	}
	foreach ($buy in $buys) {
		$paid = $buy.carried_before + $buy.bank_before - $buy.carried_after - $buy.bank_after
		if ($paid -notin @((9 * $buy.count), (10 * $buy.count))) {
			throw "Spear purchase lacks the normal 9-10 gold per spear payment."
		}
	}
	if ($Mode -eq "break") {
		# The hand stack breaks mid-hunt: the backpack spare is equipped before the hunt ends.
		$equips = @($events | Where-Object {
			$_.event -eq "action_result" -and $_.action -eq "equip_readiness" -and $_.result -eq "success" -and $_.item_id -eq 2389
		})
		$outcomes = @($events | Where-Object { $_.event -eq "hunt_region_outcome" -and $_.reason -eq "throwing_weapon_exhausted" })
		if ($equips.Count -lt 1 -or $outcomes.Count -ne 1 -or $exhausted.Count -lt 1 -or
			$outcomes[0].log_index -gt $buys[0].log_index -or $equips[0].log_index -gt $outcomes[0].log_index) {
			throw "Broken hand spear was not replaced by the carried spare before the hunt ended. equips=$($equips.Count), outcomes=$($outcomes.Count). trace=[$(& $trace)]"
		}
	}
}

function Assert-ZeroSpearRecoveryEvents {
	param([string]$Logs, [ValidateSet("startup", "last_break", "mirrored", "unfunded")][string]$Mode)

	$scenario = switch ($Mode) {
		"startup" { "equipment_buy_spear" }
		"last_break" { "spear_last_break" }
		"mirrored" { "spear_mirrored" }
		"unfunded" { "spear_unfunded" }
	}
	$mirrored = $Mode -eq "mirrored"
	$initialSpears = $Mode -eq "last_break" ? 3 : 0
	$initialGold = $Mode -eq "unfunded" ? 0 : 39
	$weaponSlot = $mirrored ? 5 : 6
	$leftWeapon = $mirrored ? 2525 : 2389
	$rightWeapon = $mirrored ? 2389 : 2525
	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	for ($i = 0; $i -lt $events.Count; $i++) { $events[$i] | Add-Member -NotePropertyName log_index -NotePropertyValue $i -Force }
	$fixture = @($events | Where-Object {
		$_.event -eq "spear_fixture" -and $_.source -eq "lua_setup_verifier" -and $_.scenario -eq $scenario -and
		$_.vocation -eq 3 -and $_.level -eq 8
	})
	$start = @($fixture | Where-Object {
		$_.phase -eq "start" -and $_.spears -eq $initialSpears -and
		$_.equipped_spears -eq $_.spears -and $_.left_item_id -eq ($mirrored ? 2525 : ($initialSpears -gt 0 ? 2389 : 0)) -and
		$_.right_item_id -eq ($mirrored ? 0 : 2525) -and
		$_.carried_gold -eq $initialGold -and $_.bank_gold -eq 0 -and $_.health -gt 0 -and $_.health -eq $_.health_max -and
		$_.mana -gt 0 -and $_.mana -eq $_.mana_max -and $_.free_capacity -ge 3000 -and
		$_.health_potions -eq 20 -and $_.mana_potions -eq 20
	})
	$buys = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "buy_throwing_weapons" -and $_.result -eq "success"
	})
	$carriedEquipment = @($events | Where-Object {
		$_.event -eq "strategy_selection" -and $_.goal -eq "buy_equipment" -and $_.acquisition -eq "carried" -and $_.item_id -eq 2389
	})
	$forbidden = @($events | Where-Object {
		($_.event -eq "terminal" -and $Mode -ne "unfunded") -or
		$_.action -in @("buy_equipment", "buy_potions", "buy_meat", "buy_ammunition", "sell", "claim_reward") -or
		(($_.action -eq "equip_equipment" -or $_.to_goal -eq "buy_equipment") -and
		 ($_.item_id -ne 2389 -or $carriedEquipment.Count -ne 1))
	})
	if ($Mode -eq "unfunded") {
		$terminal = @($events | Where-Object { $_.event -eq "terminal" })
		$service = @($events | Where-Object { $_.event -eq "goal_result" -and $_.goal -eq "service" })
		$deferred = @($events | Where-Object {
			$_.event -eq "action_result" -and $_.action -eq "restock" -and $_.result -eq "deferred" -and
			$_.reason -eq "safety_stock_unmet"
		})
		$pass = @($events | Where-Object {
			$_.event -eq "summary" -and $_.final -and $_.state -eq "stopped" -and $_.level -eq 8 -and
			$_.carried_gold -eq 0 -and $_.bank_balance -eq 0 -and
			@($_.supplies | Where-Object { $_.kind -eq "throwing_weapon" -and $_.item_id -eq 2389 -and $_.count -eq 0 }).Count -eq 1
		})
		$unsafe = @($events | Where-Object {
			$_.action -in @("hunt_cycle", "buy_throwing_weapons", "equip_readiness", "equip_equipment", "attack") -or
			$_.reason -eq "visible_monster"
		})
		if ($start.Count -ne 1 -or $pass.Count -ne 1 -or $terminal.Count -ne 1 -or
			$terminal[0].reason -ne "combat_readiness_throwing_weapon_restock_blocked" -or
			$service.Count -ne 1 -or $service[0].result -ne "success" -or $service[0].reason -ne "service_complete" -or
			$deferred.Count -ne 1 -or $forbidden.Count -ne 0 -or $unsafe.Count -ne 0) {
			throw "Unfunded zero-spear recovery did not terminate after one unsuccessful restock without hunting or purchasing. start=$($start.Count), pass=$($pass.Count), terminal=$($terminal.Count), service=$($service.Count), deferred=$($deferred.Count), forbidden=$($forbidden.Count), unsafe=$($unsafe.Count)."
		}
		$recovery = @($events | Where-Object {
			$_.event -eq "combat_readiness" -and $_.vocation_id -eq 3 -and $_.result -eq "recovery" -and
			$_.selected_recovery -eq "service" -and -not $_.terminal_reason -and $_.log_index -gt $start[0].log_index -and
			$_.log_index -lt $service[0].log_index -and
			@($_.requirements | Where-Object {
				$_.kind -eq "throwing_weapon" -and $_.item_id -eq 2389 -and $_.ready -eq $false -and
				$_.count -eq 0 -and $_.restock_target -eq 3
			}).Count -eq 1
		})
		# Lua's independent inventory sample is not a controller event. Every
		# controller event, including attempts and scheduling, must precede terminal.
		$afterTerminal = @($events | Where-Object {
			$_.log_index -gt $terminal[0].log_index -and
			-not ($_.event -eq "spear_fixture" -and $_.source -eq "lua_setup_verifier")
		})
		if ($recovery.Count -lt 1 -or $afterTerminal.Count -ne 0 -or
			$pass[0].log_index -le $service[0].log_index -or $pass[0].log_index -ge $terminal[0].log_index -or
			$deferred[0].log_index -le $start[0].log_index -or
			$deferred[0].log_index -ge $service[0].log_index -or $service[0].log_index -ge $terminal[0].log_index) {
			throw "Unfunded spear recovery lacks ordered service/terminal evidence or emitted events after terminal."
		}
		return
	}
	if ($start.Count -ne 1 -or $buys.Count -ne 1 -or $forbidden.Count -ne 0) {
		throw "Zero-spear $Mode recovery lacks isolated startup/service evidence. start=$($start.Count), buys=$($buys.Count), forbidden=$($forbidden.Count)."
	}
	$buy = $buys[0]
	$paid = $buy.carried_before + $buy.bank_before - $buy.carried_after - $buy.bank_after
	if ($buy.item_id -ne 2389 -or $buy.count -ne 3 -or $buy.carried_before -ne 39 -or
		$buy.bank_before -ne 0 -or $buy.bank_after -ne 0 -or $buy.carried_after -notin @(9, 12) -or $paid -notin @(27, 30)) {
		throw "Zero-spear $Mode recovery did not spend its sub-100-gold reserve on three ordinary 9-10-gold spears."
	}
	$recovery = @($events | Where-Object {
		$_.event -eq "combat_readiness" -and $_.vocation_id -eq 3 -and $_.result -eq "recovery" -and
		$_.selected_recovery -eq "service" -and -not $_.terminal_reason -and
		$_.log_index -gt $start[0].log_index -and $_.log_index -lt $buy.log_index -and
		@($_.requirements | Where-Object {
			$_.kind -eq "throwing_weapon" -and $_.item_id -eq 2389 -and $_.ready -eq $false -and
			$_.count -eq 0 -and $_.restock_target -eq 3
		}).Count -eq 1 -and
		@($_.requirements | Where-Object { $_.name -eq "legal_distance_weapon" -and $_.ready -eq $false }).Count -eq 1
	})
	# Lua can observe the completed purchase before the controller verifies
	# its receipt on the next turn. Readiness and hunt must still follow it.
	$pass = @($fixture | Where-Object {
		$_.phase -eq "pass" -and $_.spears -eq 3 -and $_.equipped_spears -eq 3 -and
		$_.left_item_id -eq $leftWeapon -and $_.right_item_id -eq $rightWeapon -and
		$_.carried_gold -eq $buy.carried_after -and $_.bank_gold -eq 0 -and $_.log_index -gt $start[0].log_index
	})
	$ready = @($events | Where-Object {
		$_.event -eq "combat_readiness" -and $_.vocation_id -eq 3 -and $_.result -eq "ready" -and
		$_.log_index -gt $buy.log_index -and -not $_.terminal_reason -and
		@($_.requirements | Where-Object { $_.required -ne $false -and $_.ready -ne $true }).Count -eq 0 -and
		@($_.requirements | Where-Object {
			$_.kind -eq "throwing_weapon" -and $_.item_id -eq 2389 -and $_.count -eq 3 -and $_.restock_target -eq 3 -and $_.ready
		}).Count -eq 1 -and
		@($_.requirements | Where-Object {
			$_.name -eq "legal_distance_weapon" -and $_.ready -and
			$_.left_item_id -eq $leftWeapon -and $_.right_item_id -eq $rightWeapon
		}).Count -eq 1
	})
	# Normal goal selection may equip the already bought stock through its
	# carried-equipment objective; it must not buy another weapon.
	$equipmentGoals = @($events | Where-Object { $_.event -eq "strategy_selection" -and $_.goal -eq "buy_equipment" })
	if (@($equipmentGoals | Where-Object { $_.acquisition -ne "carried" -or $_.item_id -ne 2389 -or $_.log_index -le $buy.log_index }).Count -gt 0) {
		throw "Spear recovery selected a paid or unrelated equipment acquisition."
	}
	$equips = @($events | Where-Object { $_.action -in @("equip_readiness", "equip_equipment") -and $_.item_id -eq 2389 })
	$illegalEquips = @($equips | Where-Object {
		$_.event -ne "action_result" -or $_.result -notin @("requested", "success") -or
		($_.action -eq "equip_readiness" -and $_.slot -ne $weaponSlot) -or $_.log_index -lt $buy.log_index
	})
	if ($illegalEquips.Count -gt 0) { throw "Zero-spear $Mode recovery did not equip into the free weapon hand after restocking." }
	$hunts = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "hunt_cycle" -and $_.result -eq "started" })
	$resumed = @($hunts | Where-Object { $ready.Count -gt 0 -and $_.log_index -gt $ready[0].log_index })
	if ($recovery.Count -lt 1 -or $pass.Count -ne 1 -or $ready.Count -lt 1 -or $resumed.Count -lt 1) {
		throw "Zero-spear $Mode recovery did not show service readiness, three equipped spears, paid inventory, and a later hunt. recovery=$($recovery.Count), pass=$($pass.Count), ready=$($ready.Count), resumed=$($resumed.Count)."
	}
	$beforePurchase = @($hunts | Where-Object { $_.log_index -lt $buy.log_index })
	if ($Mode -ne "last_break") {
		if ($beforePurchase.Count -ne 0) { throw "Zero-spear $Mode startup hunted before restoring its weapon." }
		return
	}
	$last = @($fixture | Where-Object {
		$_.phase -eq "last_spear" -and $_.spears -eq 1 -and $_.equipped_spears -eq 1 -and $_.left_item_id -eq 2389 -and
		$_.carried_gold -eq 39 -and $_.bank_gold -eq 0
	})
	$broken = @($fixture | Where-Object {
		$_.phase -eq "broken" -and $_.spears -eq 0 -and $_.equipped_spears -eq 0 -and $_.left_item_id -eq 0 -and
		$_.carried_gold -eq 39 -and $_.bank_gold -eq 0
	})
	$outcomes = @($events | Where-Object {
		$_.event -eq "hunt_region_outcome" -and $_.reason -eq "throwing_weapon_exhausted" -and
		$broken.Count -eq 1 -and $_.log_index -gt $broken[0].log_index -and $_.log_index -lt $buy.log_index
	})
	$prematureEquip = @($events | Where-Object {
		$_.action -eq "equip_readiness" -and $_.item_id -eq 2389 -and $_.log_index -lt $buy.log_index
	})
	if ($beforePurchase.Count -ne 1 -or $last.Count -ne 1 -or $broken.Count -ne 1 -or $outcomes.Count -ne 1 -or
		$prematureEquip.Count -ne 0 -or $beforePurchase[0].log_index -le $start[0].log_index -or
		$beforePurchase[0].log_index -ge $last[0].log_index -or $last[0].log_index -ge $broken[0].log_index -or
		$recovery[0].log_index -le $broken[0].log_index -or $pass[0].log_index -le $broken[0].log_index) {
		throw "Last-spear loss was not an active-hunt zero-stock recovery without a carried spare."
	}
}
