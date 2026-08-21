function Assert-MagicTrainingEvents {
	param([string]$Logs, [string]$Mode, [string]$Spell)
	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$actions = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "magic_training" })
	$requests = @($actions | Where-Object { $_.result -eq "requested" })
	$verified = @($actions | Where-Object { $_.source -eq "engine_verification" })
	$candidates = @($events | Where-Object { $_.event -eq "goal_candidate" -and $_.goal -eq "magic_training" })
	if ($Mode -eq "cast") {
		$success = @($verified | Where-Object { $_.result -eq "success" -and $_.spell -eq $Spell -and $_.mana_delta -eq $_.mana_cost -and $_.mana_spent_after -gt $_.mana_spent_before -and $_.mana_after -ge 20 -and $_.wasted_mana -gt 0 })
		$goal = @($events | Where-Object { $_.event -eq "goal_result" -and $_.goal -eq "magic_training" -and $_.result -eq "success" -and $_.reason -eq "cast_verified" })
		if ($requests.Count -eq 1 -and $success.Count -eq 1 -and $goal.Count -eq 1) { return }
		throw "Magic training $Spell did not make exactly one verified overflow cast. requests=$($requests.Count), success=$($success.Count), goal=$($goal.Count)."
	}
	if ($Mode -eq "failed") {
		$failed = @($verified | Where-Object { $_.result -eq "failed" -and $_.spell -eq "Light" })
		$goal = @($events | Where-Object { $_.event -eq "goal_result" -and $_.goal -eq "magic_training" -and $_.result -eq "failed" -and $_.reason -eq "cast_verification_failed" })
		$continued = @($events | Where-Object { $_.event -eq "goal_selection" -and $_.decision_reason -eq "magic_training_complete" -and $_.from_goal -eq "magic_training" })
		if ($requests.Count -eq 1 -and $failed.Count -eq 1 -and $goal.Count -eq 1 -and $continued.Count -eq 1) { return }
		throw "Failed magic-training verification retried or did not reselect."
	}
	if ($Mode -eq "post_hunt") {
		$idle = @($events | Where-Object { $_.event -eq "objective_transition" -and $_.from -eq "hunt" -and $_.to -eq "idle" })
		$selection = @($events | Where-Object { $_.event -eq "goal_selection" -and $_.decision_reason -eq "hunt_deadline" -and $_.to_goal -eq "magic_training" })
		$continued = @($events | Where-Object { $_.event -eq "goal_selection" -and $_.decision_reason -eq "magic_training_complete" -and $_.from_goal -eq "magic_training" -and $_.to_goal -eq "hunt" })
		$success = @($verified | Where-Object { $_.result -eq "success" -and $_.spell -eq "Haste" })
		if ($idle.Count -ge 1 -and $selection.Count -eq 1 -and $requests.Count -eq 1 -and $success.Count -eq 1 -and $continued.Count -eq 1) { return }
		throw "Post-hunt overflow did not transition Idle to one magic-training cast and back to Hunt."
	}
	if ($Mode -eq "post_hunt_no_overflow") {
		$idle = @($events | Where-Object { $_.event -eq "objective_transition" -and $_.from -eq "hunt" -and $_.to -eq "idle" })
		$selection = @($events | Where-Object { $_.event -eq "goal_selection" -and $_.decision_reason -eq "hunt_deadline" -and $_.to_goal -eq "hunt" })
		$guard = @($candidates | Where-Object { $_.decision_reason -eq "hunt_deadline" -and -not $_.feasible -and $_.reason -eq "next_tick_not_overflow" })
		if ($idle.Count -ge 1 -and $selection.Count -ge 1 -and $guard.Count -ge 1 -and $actions.Count -eq 0) { return }
		throw "Post-hunt non-overflow did not return from Idle directly to Hunt."
	}
	if ($Mode -eq "restart") {
		$online = @($events | Where-Object { $_.event -eq "lifecycle" -and $_.status -eq "online" })
		$forecast = @($events | Where-Object {
			$_.event -eq "magic_training_fixture" -and $_.case -eq "active_default" -and $_.active -and $_.remaining -eq $_.interval
		})
		$guard = @($candidates | Where-Object {
			-not $_.feasible -and $_.reason -eq "next_tick_not_overflow" -and $_.mana_tick_remaining -gt 0 -and
			$_.mana_tick_remaining -le $_.mana_tick_interval -and
			$_.mana + $_.mana_gain -le $_.mana_max
		})
		if ($online.Count -eq 1 -and $forecast.Count -eq 1 -and $guard.Count -ge 1 -and $actions.Count -eq 0) { return }
		throw "Restart did not recompute the fresh regeneration forecast from persisted player state. online=$($online.Count), forecast=$($forecast.Count), guard=$($guard.Count), actions=$($actions.Count)."
	}
	$reason = $Mode -eq "reserve" ? "no_audited_safe_spell" : $Mode -eq "pz" ? "regeneration_paused" :
		$Mode -in @("absent", "expired") ? "no_active_regeneration_forecast" : "next_tick_not_overflow"
	if ($requests.Count -eq 0 -and $verified.Count -eq 0 -and @($candidates | Where-Object { -not $_.feasible -and $_.reason -eq $reason }).Count -ge 1) { return }
	throw "Magic training $Mode guard failed."
}

function Assert-MagicTrainingForecastFixture {
	param([string]$Logs, [string]$Mode)
	$fixture = @(ConvertFrom-PlayerbotLogs -Logs $Logs | Where-Object { $_.event -eq "magic_training_fixture" -and $_.source -eq "authoritative_forecast" })
	if ($Mode -eq "active") {
		$activeDefault = @($fixture | Where-Object { $_.case -eq "active_default" -and $_.active -and $_.gain -eq 10 -and $_.interval -eq 1000 -and $_.remaining -eq 1000 })
		$active = @($fixture | Where-Object { $_.case -eq "active" -and $_.gain -eq 10 -and $_.interval -eq 1000 -and $_.remaining -eq 1000 -and -not $_.exact_full_overflow -and $_.overflow_wasted -eq 5 })
		$finalTick = @($fixture | Where-Object { $_.case -eq "finite_final_tick" -and $_.active })
		$finite = @($fixture | Where-Object { $_.case -eq "finite_expires_before_tick" -and -not $_.active })
		$nonDefault = @($fixture | Where-Object { $_.case -eq "non_default" -and $_.active -and $_.gain -eq 4 -and $_.remaining -eq 1000 })
		$aggregated = @($fixture | Where-Object { $_.case -eq "earliest_same_engine_cycle" -and $_.active -and $_.gain -eq 13 -and $_.remaining -eq 1000 })
		$expired = @($fixture | Where-Object { $_.case -eq "expired" -and -not $_.active })
		if ($activeDefault.Count -eq 1 -and $active.Count -eq 1 -and $finalTick.Count -eq 1 -and $finite.Count -eq 1 -and $nonDefault.Count -eq 1 -and $aggregated.Count -eq 1 -and $expired.Count -eq 1) { return }
		throw "The Creature regeneration forecast fixture did not cover active default, expiration, non-default, earliest, and summed-tick boundaries."
	}
	$expected = $Mode -eq "pz" ? "active_default" : "active_default"
	if (@($fixture | Where-Object { $_.case -eq $expected -and -not $_.active }).Count -eq 1) { return }
	throw "Magic training $Mode did not expose an absent authoritative regeneration forecast."
}
