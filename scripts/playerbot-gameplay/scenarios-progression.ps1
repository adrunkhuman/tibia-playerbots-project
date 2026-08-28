    if ($PickupProgression) {
        Invoke-Scenario -Name "pickup_progression" -DefaultTimeoutSeconds 300 -Body {
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
			Wait-ForLatestServerGenerationLog -Pattern '"event":"goal_selection".*"to_goal":"hunt"' | Out-Null
			$restartLogs = Get-ServerLogs
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
            Wait-ForLog -Pattern '"candidate_id":64120.*"reason":"insufficient_inventory_space"' | Out-Null
            $spaceLogs = Wait-ForPlayerbotEvent {
                $_.event -eq "lifecycle" -and $_.status -eq "online" -and $_.objective -eq "service"
            }
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
		Invoke-Scenario -Name "hunt_area_arrival" -DefaultTimeoutSeconds 300 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "hunt_area_arrival"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST HUNT_AREA_ARRIVAL_START' | Out-Null
			Wait-ForLog -Pattern '"event":"hunt_area_entered"' | Out-Null
			$arrivalLogs = Wait-ForLog -Pattern '"event":"target_changed".*"reason":"visible_monster"'
			Assert-HuntAreaArrivalEvents -Logs $arrivalLogs
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
            Invoke-Compose stop server
            Invoke-Compose up --detach server
			$restartLogs = ""
			for ($attempt = 0; $attempt -lt 30; $attempt++) {
				Start-Sleep -Seconds 1
				$restartLogs = Get-LatestServerGenerationLogs -Logs (Get-ServerLogs)
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
			$supplyLogs = Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST READINESS_SUPPLIES_PASS'
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
			$purchaseLogs = Wait-ForPlayerbotEvent {
				$_.event -eq "goal_result" -and $_.goal -eq "buy_equipment" -and $_.result -eq "success"
			}
			Assert-EquipmentPurchaseEvents -Logs $purchaseLogs

			Invoke-Compose stop server
			Invoke-Compose up --detach server
			$restartLogs = ""
			while ([DateTime]::UtcNow -lt $currentScenarioDeadline) {
				$allLogs = Get-ServerLogs
				$restartLogs = Get-LatestServerGenerationLogs -Logs $allLogs
				$restartOnline = @(ConvertFrom-PlayerbotLogs -Logs $restartLogs | Where-Object {
					$_.event -eq "lifecycle" -and $_.status -eq "online" -and -not $_.recovered -and $_.objective -eq "fixture_pending"
				}).Count -gt 0
				if ($allLogs -match 'PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_BUY_RESTART_PASS' -and $restartOnline) { break }
				Start-Sleep -Seconds 1
			}
			if ($allLogs -notmatch 'PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_BUY_RESTART_PASS' -or -not $restartOnline) {
				Throw-WaitTimeout "Timed out waiting for equipment purchase restart reconstruction."
			}
			Assert-EquipmentPurchaseEvents -Logs $restartLogs -Restart
		}
		Invoke-Scenario -Name "equipment_purchase_resume" -DefaultTimeoutSeconds 240 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "equipment_buy_resume"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			$resumeLogs = Wait-ForPlayerbotEvent {
				$_.event -eq "goal_result" -and $_.goal -eq "buy_equipment" -and $_.result -eq "success"
			}
			Assert-EquipmentPurchaseEvents -Logs $resumeLogs -Resume
		}
		Invoke-Scenario -Name "equipment_purchase_provider_moved" -DefaultTimeoutSeconds 240 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "equipment_buy_provider_moved"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_PROVIDER_MOVED' | Out-Null
			$movedLogs = Wait-ForPlayerbotEvent {
				$_.event -eq "goal_result" -and $_.goal -eq "buy_equipment" -and $_.result -eq "success"
			}
			Assert-EquipmentPurchaseEvents -Logs $movedLogs -ProviderMoved
		}
		Invoke-Scenario -Name "equipment_purchase_provider_unreachable" -DefaultTimeoutSeconds 120 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "equipment_buy_provider_unreachable"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			$unreachableLogs = Wait-ForPlayerbotEvent {
				$_.event -eq "goal_result" -and $_.goal -eq "buy_equipment" -and
				$_.result -eq "failed" -and $_.reason -eq "route_unavailable"
			}
			Assert-UnreachableEquipmentProviderEvents -Logs $unreachableLogs
		}
		Invoke-Scenario -Name "equipment_purchase_space" -DefaultTimeoutSeconds 240 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "equipment_buy_space"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_BUY_SPACE_PASS' | Out-Null
			$spaceLogs = Wait-ForPlayerbotEvent {
				$_.event -eq "equipment_offer_candidate" -and $_.item_id -eq 2379 -and
				$_.reason -eq "insufficient_displaced_item_space"
			}
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
		Invoke-Scenario -Name "mainland_equipment_reward" -DefaultTimeoutSeconds 300 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "mainland_reward"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			$initialLogs = Wait-ForPlayerbotEvent {
				$_.event -eq "goal_selection" -and $_.decision_id -eq 1
			}
			$initialEvents = @(ConvertFrom-PlayerbotLogs -Logs $initialLogs)
			$initialSelection = @($initialEvents | Where-Object { $_.event -eq "goal_selection" -and $_.decision_id -eq 1 })
			if ($initialSelection.Count -ne 1 -or $initialSelection[0].to_goal -ne "pickup_reward") {
				$rewardCandidate = $initialEvents | Where-Object {
					$_.event -eq "goal_candidate" -and $_.decision_id -eq 1 -and $_.goal -eq "pickup_reward"
				} | Select-Object -First 1
				throw "Mainland reward startup selected $($initialSelection[0].to_goal); reward candidate reason=$($rewardCandidate.reason)."
			}
			$logs = Wait-ForPlayerbotEvent {
				$_.event -eq "strategy_objective_result" -and $_.goal -eq "pickup_reward" -and
				$_.candidate_id -eq 50076 -and $_.result -eq "success"
			}
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

			Invoke-Compose stop server
			Invoke-Compose up --detach server
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST MAINLAND_REWARD_RESTART_PASS' | Out-Null
			$restartLogs = Wait-ForLatestServerGenerationLog -Pattern '"event":"goal_selection"'
			$restartEvents = @(ConvertFrom-PlayerbotLogs -Logs $restartLogs)
			$restartSelection = @($restartEvents | Where-Object { $_.event -eq "goal_selection" } | Select-Object -First 1)
			$rewardCandidate = @($restartEvents | Where-Object {
				$_.event -eq "goal_candidate" -and $_.goal -eq "pickup_reward" -and
				-not $_.feasible -and $_.reason -eq "no_useful_reward"
			})
			if ($restartSelection.Count -ne 1 -or $restartSelection[0].to_goal -eq "pickup_reward" -or
				$rewardCandidate.Count -ne 1) {
				throw "Mainland reward restart did not advance beyond the persisted reward objective."
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
