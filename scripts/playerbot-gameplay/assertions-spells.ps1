function Assert-SpellTrainingEvents {
	param([string]$Logs, [switch]$Restart)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$discovery = @($events | Where-Object { $_.event -eq "spell_trainer_discovered" -and $_.offers -gt 0 -and $_.in_scope })
	$selected = @($events | Where-Object {
		$_.event -eq "goal_selection" -and $_.to_goal -eq "learn_spell" -and $_.spell -eq "Light Healing" -and $_.price -eq 170
	})
	$healingAtReserve = @($events | Where-Object {
		$_.event -eq "spell_candidate" -and $_.spell -eq "Light Healing" -and $_.result -eq "feasible" -and
		$_.price -eq 170 -and $_.implemented_use -and $_.learning_priority -eq 0
	})
	$unsupportedUtility = @($events | Where-Object {
		$_.event -eq "spell_candidate" -and $_.spell -eq "Find Person" -and $_.result -eq "rejected" -and
		$_.reason -eq "no_implemented_use" -and -not $_.implemented_use -and $null -eq $_.learning_priority
	})
	$combatPriority = @($events | Where-Object {
		$_.event -eq "spell_candidate" -and $_.spell -eq "Whirlwind Throw" -and $_.implemented_use -and $_.learning_priority -eq 1
	})
	$supportPriority = @($events | Where-Object {
		$_.event -eq "spell_candidate" -and $_.spell -eq "Haste" -and $_.implemented_use -and $_.learning_priority -eq 2
	})
	$purchase = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "learn_spell" -and $_.result -eq "success" -and
		$_.spell -eq "Light Healing" -and $_.price -eq 170 -and $_.money_before -eq 300 -and $_.money_after -eq 130
	})
	$completed = @($events | Where-Object {
		$_.event -eq "goal_result" -and $_.goal -eq "learn_spell" -and $_.result -eq "success"
	})
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	if ($Restart) {
		if ($purchase.Count -ne 1 -or $terminal.Count -ne 0) {
			throw "Spell training restart repeated or failed the completed purchase. purchases=$($purchase.Count), terminal=$($terminal.Count)."
		}
		return
	}
	if ($discovery.Count -lt 1 -or $selected.Count -ne 1 -or $healingAtReserve.Count -lt 1 -or
		$unsupportedUtility.Count -lt 1 -or $combatPriority.Count -lt 1 -or $supportPriority.Count -lt 1 -or $purchase.Count -ne 1 -or
		$completed.Count -ne 1 -or $terminal.Count -ne 0) {
		$lightCandidates = @($events | Where-Object { $_.event -eq "spell_candidate" -and $_.spell -eq "Light Healing" } |
			ForEach-Object { "$($_.result)/$($_.reason)/reserve=$($_.reserve)" }) -join ","
		throw "Spell training priority failed. discovery=$($discovery.Count), selected=$($selected.Count), healing_at_reserve=$($healingAtReserve.Count), unsupported_utility=$($unsupportedUtility.Count), combat_priority=$($combatPriority.Count), support_priority=$($supportPriority.Count), purchases=$($purchase.Count), completed=$($completed.Count), terminal=$($terminal.Count), healing=[$lightCandidates]."
	}
}

function Assert-SpellUseEvents {
	param([string]$Logs)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$casts = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "cast_spell" -and $_.result -eq "success" })
	$healing = @($casts | Where-Object {
		$_.policy_candidate.spell -eq "Light Healing" -and $_.policy_candidate.role -eq "healing" -and $_.need -eq "recovery" -and
		$_.mana_after -eq ($_.mana_before - 20) -and $_.health_after -gt $_.health_before
	})
	$support = @($casts | Where-Object {
		$_.policy_candidate.spell -eq "Haste" -and $_.policy_candidate.role -eq "support" -and $_.need -eq "safe_route" -and
		$_.mana_after -ge $_.mana_reserve -and $_.reserve_survives -and $_.haste_ticks_observed -gt 0
	})
	$offense = @($casts | Where-Object {
		$_.policy_candidate.spell -eq "Whirlwind Throw" -and $_.policy_candidate.role -eq "ranged_offense" -and $_.need -eq "offense" -and
		$_.mana_after -eq ($_.mana_before - 40) -and $_.target_id -gt 0
	})
	$unlearned = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "cast_spell" -and $_.result -eq "skipped" -and
		$_.policy_candidate.spell -eq "Light Healing" -and $_.reason -eq "unlearned" -and $_.engine_result -eq "not_attempted" -and
		$_.fallback -eq "health_potion" -and @($_.legal_candidates).Count -eq 0
	})
	$fallbackPotion = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "heal" -and $_.result -eq "success" -and
		$_.method -eq "health_potion" -and $_.item_id -eq 7618 -and $_.resource_before -eq 6 -and $_.resource_after -eq 5
	})
	$manaFallback = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "cast_spell" -and $_.result -eq "skipped" -and
		$_.policy_candidate.spell -eq "Whirlwind Throw" -and $_.reason -eq "insufficient_mana_reserve" -and
		$_.fallback -eq "normal_melee" -and @($_.legal_candidates).Count -eq 0
	})
	$invalidLegalCandidates = @($events | Where-Object {
		$_.event -eq "action_result" -and $_.action -eq "cast_spell" -and $_.engine_result -ne "accepted" -and
		@($_.legal_candidates).Count -ne 0
	})
	$failed = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "cast_spell" -and $_.result -eq "failed" })
	$terminal = @($events | Where-Object { $_.event -eq "terminal" })
	$targetIndex = -1
	$preemptingHealIndex = -1
	$offenseIndex = -1
	for ($index = 0; $index -lt $events.Count; $index++) {
		$event = $events[$index]
		if ($targetIndex -lt 0 -and $event.event -eq "target_changed" -and $event.target_name -eq "Playerbot Spell Target") {
			$targetIndex = $index
		} elseif ($targetIndex -ge 0 -and $preemptingHealIndex -lt 0 -and $event.event -eq "action_result" -and
			$event.action -eq "cast_spell" -and $event.result -eq "success" -and
			$event.policy_candidate.spell -eq "Light Healing" -and $event.need -eq "recovery") {
			$preemptingHealIndex = $index
		} elseif ($preemptingHealIndex -ge 0 -and $offenseIndex -lt 0 -and $event.event -eq "action_result" -and
			$event.action -eq "cast_spell" -and $event.result -eq "success" -and
			$event.policy_candidate.spell -eq "Whirlwind Throw") {
			$offenseIndex = $index
		}
	}
	$preempted = $targetIndex -ge 0 -and $preemptingHealIndex -gt $targetIndex -and $offenseIndex -gt $preemptingHealIndex
	if ($healing.Count -ge 2 -and $support.Count -eq 1 -and $offense.Count -eq 1 -and $unlearned.Count -eq 1 -and
		$fallbackPotion.Count -eq 1 -and $manaFallback.Count -ge 1 -and $preempted -and $invalidLegalCandidates.Count -eq 0 -and
		$failed.Count -eq 0 -and $terminal.Count -eq 0) {
		return
	}
	throw "Spell use failed. healing=$($healing.Count), support=$($support.Count), offense=$($offense.Count), unlearned=$($unlearned.Count), fallbackPotion=$($fallbackPotion.Count), manaFallback=$($manaFallback.Count), preempted=$preempted, invalidLegalCandidates=$($invalidLegalCandidates.Count), failed=$($failed.Count), terminal=$($terminal.Count)."
}

function Assert-SpellCalibrationEvents {
	param([string]$Logs)

	$events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
	$fixture = @($events | Where-Object { $_.event -eq "spell_calibration" })
	$phases = @($fixture | ForEach-Object { $_.phase })
	$required = @("isolated_healing", "healing_equality_exact", "overheal_censored", "concurrent_damage", "single_target_damage", "melee_ambiguous",
		"rejected_cast", "other_attacker_ambiguous", "target_loss_ambiguous", "multi_target_ambiguous", "support_duration", "support_preexisting_or_replaced", "low_confidence", "gradual_ranking", "bounded_range", "fixture_profile_clear")
	$missing = @($required | Where-Object { $_ -notin $phases })
	$acceptedHealing = @($fixture | Where-Object { $_.source -eq "classifier_helper" -and $_.phase -eq "isolated_healing" -and $_.evidence -eq "accepted" -and
		$_.engine_bounds.maximum -lt 10000 -and $_.calibration.accepted -eq 1 })
	$equalityHealing = @($fixture | Where-Object { $_.source -eq "classifier_helper" -and $_.phase -eq "healing_equality_exact" -and $_.evidence -eq "accepted" })
	$censored = @($fixture | Where-Object { $_.source -eq "classifier_helper" -and $_.phase -eq "overheal_censored" -and $_.evidence -eq "censored_overheal" })
	$concurrent = @($fixture | Where-Object { $_.source -eq "classifier_helper" -and $_.phase -eq "concurrent_damage" -and $_.evidence -eq "concurrent_damage" })
	$damage = @($fixture | Where-Object { $_.source -eq "classifier_helper" -and $_.phase -eq "single_target_damage" -and $_.evidence -eq "accepted" -and $_.target_class -eq "monster:fixture" })
	$ambiguous = @($fixture | Where-Object { $_.source -eq "classifier_helper" -and $_.phase -in @("melee_ambiguous", "other_attacker_ambiguous", "target_loss_ambiguous") -and $_.calibration.ambiguous -gt 0 })
	$multiTarget = @($fixture | Where-Object { $_.source -eq "classifier_helper" -and $_.phase -eq "multi_target_ambiguous" -and $_.evidence -eq "multi_target" -and $_.calibration.accepted -eq 0 })
	$support = @($fixture | Where-Object { $_.phase -eq "support_duration" -and $_.calibration.conservative -eq 33000 -and $_.engine_bounds.duration_ms -eq 33000 })
	$supportRejected = @($fixture | Where-Object { $_.phase -eq "support_preexisting_or_replaced" -and $_.evidence -eq "preexisting_or_replaced_condition" -and $_.calibration.ambiguous -eq 1 })
	$lowConfidence = @($fixture | Where-Object { $_.source -eq "profile_math" -and $_.phase -eq "low_confidence" -and $_.calibration.confidence -lt 1 -and $_.policy_unchanged })
	$adjusted = @($fixture | Where-Object { $_.source -eq "profile_math" -and $_.phase -eq "gradual_ranking" -and $_.calibration.confidence -eq 1 -and
		$_.calibration.ranking -gt $_.engine_bounds.minimum -and $_.calibration.maximum -le 60000 })
	$bounded = @($fixture | Where-Object { $_.source -eq "profile_math" -and $_.phase -eq "bounded_range" -and $_.calibration.maximum -eq 60000 })
	$fixtureClear = @($fixture | Where-Object { $_.source -eq "profile_math" -and $_.phase -eq "fixture_profile_clear" -and $_.profiles_before -eq 12 -and $_.profiles_after -eq 0 })
	$evicted = @($events | Where-Object { $_.event -eq "spell_calibration_eviction" -and $_.source -eq "profile_math" -and $_.profile_count -eq 12 })
	$engineHealing = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "cast_spell" -and $_.result -eq "success" -and
		$_.policy_candidate.spell -eq "Light Healing" -and $_.reason -eq "accepted" -and $_.calibration.accepted -eq 1 })
	$engineHaste = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "cast_spell" -and $_.result -eq "success" -and
		$_.policy_candidate.spell -eq "Haste" -and $_.reason -eq "accepted" -and $_.haste_ticks_after_cast -gt 0 -and
		$_.haste_ticks_observed -gt 0 -and $_.haste_duration_measured -ge 32000 -and $_.haste_duration_measured -le 34000 })
	$engineBerserkSingle = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "cast_spell" -and $_.result -eq "success" -and
		$_.policy_candidate.spell -eq "Berserk" -and $_.reason -eq "accepted" -and $_.spell_victim_count -eq 1 -and
		-not $_.spell_victim_overflow -and $_.target_class -eq "monster:Playerbot Spell Target" -and $_.calibration.accepted -eq 1 -and $_.calibration.confidence -lt 1 })
	$engineBerserkMulti = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "cast_spell" -and $_.result -eq "success" -and
		$_.policy_candidate.spell -eq "Berserk" -and $_.reason -eq "multi_target" -and $_.spell_victim_count -gt 1 -and
		-not $_.spell_victim_overflow -and $_.target_class -eq "monster:Playerbot Spell Target" -and $_.calibration.ambiguous -ge 1 })
	$firstOffensiveRequest = $null
	foreach ($event in $events) {
		if ($event.event -eq "action_result" -and $event.action -eq "cast_spell" -and $event.result -eq "requested" -and $event.need -eq "offense") {
			$firstOffensiveRequest = $event
			break
		}
	}
	$defaultBerserk = $null -ne $firstOffensiveRequest -and $firstOffensiveRequest.policy_candidate.spell -eq "Berserk" -and
		$firstOffensiveRequest.calibration.confidence -eq 0
	$actualLowConfidence = @($events | Where-Object { $_.event -eq "action_result" -and $_.action -eq "cast_spell" -and $_.result -eq "requested" -and
		$_.policy_candidate.spell -eq "Light Healing" -and $_.calibration.confidence -eq 0 })
	if ($missing.Count -eq 0 -and $acceptedHealing.Count -eq 1 -and $equalityHealing.Count -eq 1 -and $censored.Count -eq 1 -and $concurrent.Count -eq 1 -and
		$damage.Count -eq 1 -and $ambiguous.Count -eq 3 -and $multiTarget.Count -eq 1 -and $support.Count -eq 1 -and $supportRejected.Count -eq 1 -and $lowConfidence.Count -eq 1 -and
		$adjusted.Count -ge 1 -and $bounded.Count -eq 1 -and $fixtureClear.Count -eq 1 -and $evicted.Count -ge 1 -and
		$engineHealing.Count -ge 1 -and $engineHaste.Count -ge 1 -and $engineBerserkSingle.Count -eq 1 -and $engineBerserkMulti.Count -eq 1 -and
		$defaultBerserk -and $actualLowConfidence.Count -ge 1) { return }
	throw "Spell calibration telemetry was incomplete. missing=$($missing -join ','), acceptedHealing=$($acceptedHealing.Count), equalityHealing=$($equalityHealing.Count), censored=$($censored.Count), concurrent=$($concurrent.Count), damage=$($damage.Count), ambiguous=$($ambiguous.Count), multiTarget=$($multiTarget.Count), support=$($support.Count), supportRejected=$($supportRejected.Count), lowConfidence=$($lowConfidence.Count), adjusted=$($adjusted.Count), bounded=$($bounded.Count), fixtureClear=$($fixtureClear.Count), evicted=$($evicted.Count), engineHealing=$($engineHealing.Count), engineHaste=$($engineHaste.Count), engineBerserkSingle=$($engineBerserkSingle.Count), engineBerserkMulti=$($engineBerserkMulti.Count), defaultBerserk=$defaultBerserk, actualLowConfidence=$($actualLowConfidence.Count)."
}
