/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "playerbotgoalarbiter.h"

#include <algorithm>
#include <stdexcept>

namespace {
	uint8_t goalOrder(PlayerBotGoalArbiter::TopLevelGoal goal)
	{
		switch (goal) {
			case PlayerBotGoalArbiter::TopLevelGoal::Departure: return 0;
			case PlayerBotGoalArbiter::TopLevelGoal::Service: return 1;
			case PlayerBotGoalArbiter::TopLevelGoal::PickupReward: return 2;
			case PlayerBotGoalArbiter::TopLevelGoal::LearnSpell: return 3;
			case PlayerBotGoalArbiter::TopLevelGoal::BuyEquipment: return 4;
			case PlayerBotGoalArbiter::TopLevelGoal::MagicTraining: return 5;
			case PlayerBotGoalArbiter::TopLevelGoal::Hunt: return 6;
		}
		return 7;
	}
}

const PlayerBotGoalArbiter::GoalCandidate& PlayerBotGoalArbiter::GoalDecision::candidate(TopLevelGoal goal) const
{
	for (const GoalCandidate& candidate : candidates) {
		if (candidate.goal == goal) {
			return candidate;
		}
	}
	throw std::logic_error("goal candidate missing from decision");
}

PlayerBotGoalArbiter::GoalDecision PlayerBotGoalArbiter::decide(std::vector<GoalCandidate> candidates)
{
	std::stable_sort(candidates.begin(), candidates.end(), [](const GoalCandidate& left, const GoalCandidate& right) {
		return goalOrder(left.goal) < goalOrder(right.goal);
	});

	for (GoalCandidate& candidate : candidates) {
		if (candidate.goal != TopLevelGoal::Hunt) {
			continue;
		}

		for (const GoalCandidate& higherPriority : candidates) {
			if (&higherPriority == &candidate) {
				break;
			}
			if (higherPriority.feasible && higherPriority.utility > candidate.utility) {
				candidate.feasible = false;
				candidate.reason = "deferred_lower_utility";
				break;
			}
		}
	}

	GoalDecision decision{++currentDecisionId, currentGoal, std::move(candidates), std::nullopt};
	for (const GoalCandidate& candidate : decision.candidates) {
		if (candidate.feasible && (!decision.selected || candidate.utility > decision.selected->utility)) {
			decision.selected = candidate;
		}
	}
	return decision;
}

PlayerBotGoalArbiter::GoalDecision PlayerBotGoalArbiter::force(GoalCandidate candidate)
{
	GoalDecision decision{++currentDecisionId, currentGoal, {std::move(candidate)}, std::nullopt, true};
	if (decision.candidates.front().feasible) {
		decision.selected = decision.candidates.front();
	}
	return decision;
}

void PlayerBotGoalArbiter::apply(const GoalDecision& decision)
{
	if (decision.id == currentDecisionId && decision.selected) {
		currentGoal = decision.selected->goal;
	}
}

PlayerBotGoalArbiter::TopLevelGoal PlayerBotGoalArbiter::activeGoal() const
{
	return currentGoal;
}

void PlayerBotGoalArbiter::setActiveGoal(TopLevelGoal goal)
{
	currentGoal = goal;
}

uint64_t PlayerBotGoalArbiter::decisionId() const
{
	return currentDecisionId;
}

bool PlayerBotGoalArbiter::isCoolingDown(TopLevelGoal goal, std::chrono::steady_clock::time_point now) const
{
	return cooldownUntil(goal) > now;
}

void PlayerBotGoalArbiter::setCooldown(TopLevelGoal goal, std::chrono::steady_clock::duration duration)
{
	cooldownUntil(goal) = std::chrono::steady_clock::now() + duration;
}

const char* PlayerBotGoalArbiter::goalName(TopLevelGoal goal)
{
	switch (goal) {
		case TopLevelGoal::Departure: return "oracle_departure";
		case TopLevelGoal::Service: return "service";
		case TopLevelGoal::PickupReward: return "pickup_reward";
		case TopLevelGoal::LearnSpell: return "learn_spell";
		case TopLevelGoal::BuyEquipment: return "buy_equipment";
		case TopLevelGoal::MagicTraining: return "magic_training";
		case TopLevelGoal::Hunt: return "hunt";
	}
	return "unknown";
}

std::chrono::steady_clock::time_point& PlayerBotGoalArbiter::cooldownUntil(TopLevelGoal goal)
{
	switch (goal) {
		case TopLevelGoal::PickupReward: return pickupRewardCooldownUntil;
		case TopLevelGoal::LearnSpell: return spellTrainingCooldownUntil;
		case TopLevelGoal::BuyEquipment: return equipmentPurchaseCooldownUntil;
		case TopLevelGoal::MagicTraining: return magicTrainingCooldownUntil;
		default: throw std::logic_error("goal has no cooldown");
	}
}

const std::chrono::steady_clock::time_point& PlayerBotGoalArbiter::cooldownUntil(TopLevelGoal goal) const
{
	switch (goal) {
		case TopLevelGoal::PickupReward: return pickupRewardCooldownUntil;
		case TopLevelGoal::LearnSpell: return spellTrainingCooldownUntil;
		case TopLevelGoal::BuyEquipment: return equipmentPurchaseCooldownUntil;
		case TopLevelGoal::MagicTraining: return magicTrainingCooldownUntil;
		default: throw std::logic_error("goal has no cooldown");
	}
}
