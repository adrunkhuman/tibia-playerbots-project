	if ($CorpseLoot) {
		Invoke-Scenario -Name "corpse" -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "corpse"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			$corpseLogs = Wait-ForLog -Pattern '"reason":"corpse_not_lootable","expected_corpse_item_id":1987'
			Assert-CorpseEvents -Logs $corpseLogs
		}
		Invoke-Scenario -Name "corpse_detour" -DefaultTimeoutSeconds 75 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "corpse_detour"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			$corpseLogs = Wait-ForPlayerbotEvent -Predicate {
				$_.event -eq "action_result" -and $_.action -eq "loot" -and $_.result -eq "success"
			}
			Assert-CorpseDetourEvents -Logs $corpseLogs
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
