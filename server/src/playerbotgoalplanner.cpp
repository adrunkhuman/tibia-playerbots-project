/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 */

#include "playerbotgoalplanner.h"

#include <algorithm>

namespace {
	constexpr int32_t serviceGoalBaseUtility = 400;
	constexpr int32_t spellTrainingGoalUtility = 550;
	constexpr int32_t equipmentPurchaseGoalUtility = 500;
	constexpr int32_t magicTrainingGoalUtility = 350;
	constexpr int32_t huntGoalUtility = 300;
	constexpr int32_t oracleDepartureUtility = 950;
	constexpr int32_t capacityServiceUtility = 900;
	constexpr int32_t criticalHealingServiceUtility = 1000;
	constexpr int32_t missingPotionUtility = 15;
	constexpr int32_t sellableItemUtility = 10;
}

PlayerBotGoalArbiter::GoalCandidate PlayerBotGoalPlanner::departureCandidate(const PlayerBotGoalPlannerSnapshot& snapshot) const
{
	using Goal = PlayerBotGoalArbiter::TopLevelGoal;
	const bool feasible = snapshot.departureEligible && snapshot.departurePlanAvailable;
	return {Goal::Departure, feasible, feasible ? oracleDepartureUtility : 0,
	        snapshot.alreadyDeparted ? "already_departed" :
	        snapshot.belowDepartureLevel ? "below_minimum_level" :
	        snapshot.aboveDepartureLevel ? "above_maximum_level" :
	        feasible ? "oracle_reachable" : "oracle_unreachable"};
}

PlayerBotGoalArbiter::GoalCandidate PlayerBotGoalPlanner::serviceCandidate(const PlayerBotGoalPlannerSnapshot& snapshot) const
{
	using Goal = PlayerBotGoalArbiter::TopLevelGoal;
	const bool feasible = snapshot.lowCapacity || snapshot.missingPotions != 0 || snapshot.sellableItems != 0 || snapshot.cashAdjustment;
	int32_t utility = feasible ? serviceGoalBaseUtility : 0;
	utility += static_cast<int32_t>(snapshot.missingPotions) * missingPotionUtility +
	           static_cast<int32_t>(std::min<uint32_t>(snapshot.sellableItems, 20)) * sellableItemUtility;
	utility += snapshot.cashAdjustment ? 10 : 0;
	if (snapshot.lowCapacity) utility = std::max(utility, capacityServiceUtility);
	if (snapshot.criticalHealing) utility = std::max(utility, criticalHealingServiceUtility);
	return {Goal::Service, feasible, utility,
	        snapshot.criticalHealing ? "critical_healing" : snapshot.lowCapacity ? "capacity" :
	        snapshot.missingPotions != 0 ? "healing_reserve" : snapshot.sellableItems != 0 ? "sellable_inventory" :
	        snapshot.cashAdjustment ? "cash_reserve" : "no_service_need"};
}

PlayerBotGoalArbiter::GoalDecision PlayerBotGoalPlanner::decide(const PlayerBotGoalPlannerSnapshot& snapshot,
	                                                               PlayerBotGoalArbiter& arbiter) const
{
	using Goal = PlayerBotGoalArbiter::TopLevelGoal;
	const auto departure = departureCandidate(snapshot);
	const auto service = serviceCandidate(snapshot);
	const auto pickup = PlayerBotGoalArbiter::GoalCandidate{Goal::PickupReward, !snapshot.pickupCoolingDown && snapshot.rewardPlanAvailable,
		snapshot.pickupCoolingDown || !snapshot.rewardPlanAvailable ? 0 : snapshot.rewardUtility,
		snapshot.pickupCoolingDown ? "cooldown" : snapshot.rewardPlanAvailable ? "useful_reachable_reward" : "no_useful_reward"};
	const auto spell = PlayerBotGoalArbiter::GoalCandidate{Goal::LearnSpell, !snapshot.spellCoolingDown && snapshot.spellPlanAvailable,
		snapshot.spellCoolingDown || !snapshot.spellPlanAvailable ? 0 : spellTrainingGoalUtility,
		snapshot.spellCoolingDown ? "cooldown" : snapshot.spellPlanAvailable ? "eligible_reachable_spell" : "no_eligible_spell"};
	const auto equipment = PlayerBotGoalArbiter::GoalCandidate{Goal::BuyEquipment,
		!snapshot.equipmentCoolingDown && snapshot.equipmentEnabled && snapshot.equipmentPlanAvailable,
		snapshot.equipmentCoolingDown || !snapshot.equipmentEnabled || !snapshot.equipmentPlanAvailable ? 0 : equipmentPurchaseGoalUtility,
		snapshot.equipmentCoolingDown ? "cooldown" : !snapshot.equipmentEnabled ? "shadow_only" :
		snapshot.equipmentPlanAvailable ? snapshot.equipmentReason : "no_justified_offer"};
	const auto magic = PlayerBotGoalArbiter::GoalCandidate{Goal::MagicTraining,
		!snapshot.magicCoolingDown && snapshot.magicTrainingReason.empty(),
		!snapshot.magicCoolingDown && snapshot.magicTrainingReason.empty() ? magicTrainingGoalUtility : 0,
		snapshot.magicCoolingDown ? "cooldown" : snapshot.magicTrainingReason.empty() ? "next_tick_overflow" : snapshot.magicTrainingReason};
	return arbiter.decide({departure, service, pickup, spell, equipment, magic,
	                       {Goal::Hunt, true, huntGoalUtility, "autonomous_hunting_available"}});
}
