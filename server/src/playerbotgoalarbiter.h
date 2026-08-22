/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef FS_PLAYERBOTGOALARBITER_H
#define FS_PLAYERBOTGOALARBITER_H

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class PlayerBotGoalArbiter {
	public:
		enum class TopLevelGoal : uint8_t {
			Departure,
			Service,
			PickupReward,
			LearnSpell,
			BuyEquipment,
			MagicTraining,
			Hunt,
		};

		struct GoalCandidate {
			TopLevelGoal goal;
			bool feasible;
			int32_t utility;
			std::string reason;
		};

		struct GoalDecision {
			uint64_t id;
			TopLevelGoal previousGoal;
			std::vector<GoalCandidate> candidates;
			std::optional<GoalCandidate> selected;
			bool forced = false;

			const GoalCandidate& candidate(TopLevelGoal goal) const;
		};

		TopLevelGoal activeGoal() const;
		uint64_t decisionId() const;
		bool isCoolingDown(TopLevelGoal goal, std::chrono::steady_clock::time_point now) const;

		static const char* goalName(TopLevelGoal goal);

	private:
		friend class PlayerBotProgressionRuntime;

		GoalDecision decide(std::vector<GoalCandidate> candidates);
		GoalDecision force(GoalCandidate candidate);
		void apply(const GoalDecision& decision);
		void setActiveGoal(TopLevelGoal goal);
		void setCooldown(TopLevelGoal goal, std::chrono::steady_clock::duration duration);
		std::chrono::steady_clock::time_point& cooldownUntil(TopLevelGoal goal);
		const std::chrono::steady_clock::time_point& cooldownUntil(TopLevelGoal goal) const;

		TopLevelGoal currentGoal = TopLevelGoal::Service;
		uint64_t currentDecisionId = 0;
		std::chrono::steady_clock::time_point pickupRewardCooldownUntil;
		std::chrono::steady_clock::time_point spellTrainingCooldownUntil;
		std::chrono::steady_clock::time_point equipmentPurchaseCooldownUntil;
		std::chrono::steady_clock::time_point magicTrainingCooldownUntil;
};

#endif
