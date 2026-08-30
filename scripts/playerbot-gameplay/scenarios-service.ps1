	if ($MainlandLoop) {
		Invoke-Scenario -Name "carlin_local_service" -DefaultTimeoutSeconds 120 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "carlin_local_service"
			Invoke-Compose up --detach
			$logs = Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST CARLIN_LOCAL_SERVICE_PASS'
			Assert-CarlinLocalServiceEvents -Logs $logs
		}

		Invoke-Scenario -Name "mainland_loop" -DefaultTimeoutSeconds 600 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "mainland"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "10"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST MAINLAND_START' | Out-Null
			$loopLogs = Wait-ForLog -Pattern '"action":"hunt_cycle","result":"started","cycle":3'
			Assert-MainlandLoopEvents -Logs $loopLogs

			Invoke-Compose stop server
			Invoke-Compose up --detach server
			Wait-ForLatestServerGenerationLog -Pattern '"action":"deposit","result":"complete","depot_id":2' | Out-Null
			$restartLogs = Wait-ForLatestServerGenerationLog -Pattern '"action":"hunt_cycle","result":"started","cycle":1'
			Assert-MainlandLoopEvents -Logs $restartLogs -MinimumCycles 1 -MinimumDeposits 1
		}
	}

	if ($SlottedLoot) {
		Invoke-Scenario -Name "slotted_loot_seller" -DefaultTimeoutSeconds 240 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "slotted_loot_seller"
			$env:PLAYERBOT_DEPOT_RESTART_PHASE = ""
			Invoke-DatabaseCommand -Query "DELETE FROM player_items WHERE player_id = (SELECT id FROM players WHERE name = 'Bot One') AND (pid = 10 OR itemtype IN (7618, 8704)); INSERT INTO player_items (player_id, sid, pid, itemtype, count, attributes) SELECT id, 9900, 10, 2398, 1, X'' FROM players WHERE name = 'Bot One';"
			Invoke-Compose up --no-deps --detach server
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST SLOTTED_LOOT_SELLER_PASS' | Out-Null
			$sellerLogs = Wait-ForLog -Pattern '"action":"deposit","result":"complete"'
			Assert-SlottedLootEvents -Logs $sellerLogs -SellerAvailable
		}

		Invoke-Scenario -Name "slotted_loot_no_seller" -DefaultTimeoutSeconds 240 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "slotted_loot_no_seller"
			$env:PLAYERBOT_DEPOT_RESTART_PHASE = ""
			Invoke-DatabaseCommand -Query "DELETE FROM player_items WHERE player_id = (SELECT id FROM players WHERE name = 'Bot One') AND (pid = 10 OR itemtype IN (7618, 8704)); INSERT INTO player_items (player_id, sid, pid, itemtype, count, attributes) SELECT id, 9900, 10, 2398, 1, X'' FROM players WHERE name = 'Bot One';"
			Invoke-Compose up --no-deps --detach server
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST SLOTTED_LOOT_NO_SELLER_PASS' | Out-Null
			$noSellerLogs = Wait-ForLog -Pattern '"action":"deposit","result":"complete"'
			Assert-SlottedLootEvents -Logs $noSellerLogs
		}

		Invoke-Scenario -Name "slotted_loot_deposit_restart" -DefaultTimeoutSeconds 300 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "slotted_loot_no_seller"
			$env:PLAYERBOT_DEPOT_RESTART_PHASE = "deposit"
			Invoke-DatabaseCommand -Query "DELETE FROM player_items WHERE player_id = (SELECT id FROM players WHERE name = 'Bot One') AND (pid = 10 OR itemtype IN (7618, 8704)); INSERT INTO player_items (player_id, sid, pid, itemtype, count, attributes) SELECT id, 9900, 10, 2398, 1, X'' FROM players WHERE name = 'Bot One';"
			Invoke-Compose up --no-deps --detach server
			Wait-ForLog -Pattern '"action":"depot_restart_checkpoint","result":"paused","phase":"deposit"' | Out-Null
			Invoke-Compose stop server
			Invoke-Compose up --detach server
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST SLOTTED_LOOT_NO_SELLER_PASS' | Out-Null
			$restartLogs = Wait-ForLog -Pattern '"action":"deposit","result":"complete"'
			Assert-SlottedLootEvents -Logs $restartLogs -Restarted
		}
	}

	if ($SellLoot) {
		Invoke-Scenario -Name "sell_loot" -DefaultTimeoutSeconds 240 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "sell_loot"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "5"
			Invoke-DatabaseCommand -Query "SET @bot = (SELECT id FROM players WHERE name = 'Bot One'); SET @backpack = (SELECT sid FROM player_items WHERE player_id = @bot AND pid = 3); DELETE FROM player_items WHERE player_id = @bot AND itemtype = 7618; INSERT INTO player_items (player_id, sid, pid, itemtype, count, attributes) SELECT @bot, COALESCE(MAX(sid), 100) + 1, @backpack, 7618, 10, X'' FROM player_items WHERE player_id = @bot;"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST SELL_LOOT_PASS' | Out-Null
			$logs = Wait-ForLog -Pattern '"goal":"sell_loot","result":"success"'
			$events = @(ConvertFrom-PlayerbotLogs -Logs $logs)
			$plan = @($events | Where-Object { $_.event -eq "sell_loot_plan" -and $_.result -eq "candidate" -and $_.item_id -eq 7735 -and $_.count -eq 10 -and $_.utility -gt 300 })
			$withdraw = @($events | Where-Object { $_.event -eq "sell_loot_withdraw" -and $_.result -eq "success" -and $_.item_id -eq 7735 })
			$sales = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "sell" -and $_.result -eq "success" -and $_.item_id -eq 7735 -and $_.count -eq 10 })
			if ($plan.Count -ne 1 -or $withdraw.Count -ne 10 -or $sales.Count -ne 1) {
				throw "Local SellLoot did not produce one plan, ten verified withdrawals, and one planned sale. plan=$($plan.Count), withdraw=$($withdraw.Count), sales=$($sales.Count)."
			}
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

			Invoke-Compose stop server
			Invoke-Compose up --detach server
			$secondCycleLogs = Wait-ForLatestServerGenerationLog -Pattern '"action":"deposit","result":"complete"'
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
				$pausedTransitions = @(ConvertFrom-PlayerbotLogs -Logs $checkpointLogs | Where-Object {
					$_.event -eq "state_transition" -and $_.to -eq "paused"
				})
				if ($checkpointEvents.Count -ne 1 -or $pausedTransitions.Count -ne 1) {
					throw "Depot $phase restart checkpoint did not pause exactly once. checkpoint=$($checkpointEvents.Count), transition=$($pausedTransitions.Count)."
				}
				Invoke-Compose stop server
				Invoke-Compose up --detach server
				$recoveryLogs = Wait-ForLatestServerGenerationLog -Pattern '"action":"deposit","result":"complete"'
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
