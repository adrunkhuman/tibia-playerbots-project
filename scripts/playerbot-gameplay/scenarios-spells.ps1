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
			Invoke-Compose stop server
			Invoke-Compose up --detach server
			$restartLogs = ""
			while ([DateTime]::UtcNow -lt $currentScenarioDeadline) {
				$restartLogs = Get-LatestServerGenerationLogs -Logs (Get-ServerLogs)
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

	if ($MagicTraining -or $MagicTrainingCase) {
		$magicTrainingCastCases = @(
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
		) | Where-Object { -not $MagicTrainingCase -or $_.Name -eq $MagicTrainingCase }
		foreach ($case in $magicTrainingCastCases) {
			Invoke-Scenario -Name $case.Name -DefaultTimeoutSeconds 120 -Body {
				Invoke-Compose down --volumes --remove-orphans
				$env:PLAYERBOT_GAMEPLAY_MODE = $case.Name
				Invoke-Compose up --detach
				$pattern = if ($case.Mode -eq "cast") {
					'"event":"goal_result".*"goal":"magic_training".*"result":"success".*"reason":"cast_verified"'
				} elseif ($case.Mode -eq "failed") {
					'"event":"goal_selection".*"decision_reason":"magic_training_complete".*"from_goal":"magic_training"'
				} else {
					'"event":"goal_candidate".*"goal":"magic_training"'
				}
				$logs = Wait-ForLog -Pattern $pattern
				Assert-MagicTrainingEvents -Logs $logs -Mode $case.Mode -Spell $case.Spell
				if ($case.Name -eq "magic_training_haste") {
					Assert-MagicTrainingForecastFixture -Logs $logs -Mode "active"
				} elseif ($case.Mode -in @("pz", "absent", "expired")) {
					Assert-MagicTrainingForecastFixture -Logs $logs -Mode $case.Mode
				}
			}
		}
		$magicTrainingPriorityCases = @(
			@{ Name = "magic_training_service"; Winner = "service" },
			@{ Name = "magic_training_progression"; Winner = "learn_spell" }
		) | Where-Object { -not $MagicTrainingCase -or $_.Name -eq $MagicTrainingCase }
		foreach ($case in $magicTrainingPriorityCases) {
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
		$magicTrainingPostHuntCases = @(
			@{ Name = "magic_training_post_hunt"; Mode = "post_hunt" },
			@{ Name = "magic_training_post_hunt_no_overflow"; Mode = "post_hunt_no_overflow" }
		) | Where-Object { -not $MagicTrainingCase -or $_.Name -eq $MagicTrainingCase }
		foreach ($case in $magicTrainingPostHuntCases) {
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
		if (-not $MagicTrainingCase -or $MagicTrainingCase -eq "magic_training_restart") {
			Invoke-Scenario -Name "magic_training_restart" -DefaultTimeoutSeconds 120 -Body {
				Invoke-Compose down --volumes --remove-orphans
				$env:PLAYERBOT_GAMEPLAY_MODE = "magic_training_restart"
				Invoke-Compose up --detach
				$initialLogs = Wait-ForLog -Pattern '"action":"magic_training","result":"success"'
				Assert-MagicTrainingEvents -Logs $initialLogs -Mode "cast" -Spell "Light"
				$cast = @(ConvertFrom-PlayerbotLogs -Logs $initialLogs | Where-Object {
					$_.event -eq "action_result" -and $_.action -eq "magic_training" -and $_.source -eq "engine_verification" -and $_.result -eq "success"
				})[0]
				Invoke-Compose stop server
				$persistedMagicLevel = Invoke-DatabaseScalar -Query "SELECT maglevel FROM players WHERE name = 'Bot One'"
				$persistedManaSpent = Invoke-DatabaseScalar -Query "SELECT manaspent FROM players WHERE name = 'Bot One'"
				if ($persistedMagicLevel -lt $cast.magic_level_after -or $persistedManaSpent -lt $cast.mana_spent_after) {
					throw "Magic training cast progress was lost during clean shutdown. magicLevel=$persistedMagicLevel/$($cast.magic_level_after), manaSpent=$persistedManaSpent/$($cast.mana_spent_after)."
				}
				Invoke-DatabaseCommand -Query "UPDATE players SET mana = $($cast.mana_after) WHERE name = 'Bot One'"
				Invoke-Compose up --detach server
				Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST MAGIC_TRAINING_RESTART_START' | Out-Null
				$restartLogs = Wait-ForLatestServerGenerationLog -Pattern '"event":"goal_candidate".*"goal":"magic_training".*"reason":"next_tick_not_overflow"'
				Assert-MagicTrainingEvents -Logs $restartLogs -Mode "restart"
			}
		}
		if (-not $MagicTrainingCase -or $MagicTrainingCase -eq "magic_training_hunt") {
			Invoke-Scenario -Name "magic_training_hunt" -DefaultTimeoutSeconds 90 -Body {
				Invoke-Compose down --volumes --remove-orphans
				$env:PLAYERBOT_GAMEPLAY_MODE = "magic_training_hunt"
				Invoke-Compose up --detach
				$logs = Wait-ForLog -Pattern '"action":"hunt_cycle","result":"started"'
				if (@(ConvertFrom-PlayerbotLogs -Logs $logs | Where-Object { $_.event -eq "action_result" -and $_.action -eq "magic_training" }).Count -ne 0) { throw "Magic training ran while hunting." }
			}
		}
	}
