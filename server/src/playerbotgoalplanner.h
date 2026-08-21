/**
 * Builds top-level playerbot goal candidates from one immutable observation.
 */
#ifndef FS_PLAYERBOTGOALPLANNER_H
#define FS_PLAYERBOTGOALPLANNER_H

#include "playerbotgoalarbiter.h"

#include <string>

struct PlayerBotGoalPlannerSnapshot {
	bool departureRequired = false;
	bool departureEligible = false;
	bool departurePlanAvailable = false;
	bool alreadyDeparted = false;
	bool belowDepartureLevel = false;
	bool aboveDepartureLevel = false;

	bool lowCapacity = false;
	bool criticalHealing = false;
	uint32_t missingPotions = 0;
	uint32_t sellableItems = 0;
	bool cashAdjustment = false;

	bool pickupCoolingDown = false;
	bool rewardPlanAvailable = false;
	int32_t rewardUtility = 0;

	bool spellCoolingDown = false;
	bool spellPlanAvailable = false;

	bool equipmentCoolingDown = false;
	bool equipmentEnabled = false;
	bool equipmentPlanAvailable = false;
	std::string equipmentReason;

	bool magicCoolingDown = false;
	std::string magicTrainingReason;
};

class PlayerBotGoalPlanner {
	public:
		PlayerBotGoalArbiter::GoalDecision decide(const PlayerBotGoalPlannerSnapshot& snapshot,
		                                          PlayerBotGoalArbiter& arbiter) const;
		PlayerBotGoalArbiter::GoalCandidate departureCandidate(const PlayerBotGoalPlannerSnapshot& snapshot) const;
		PlayerBotGoalArbiter::GoalCandidate serviceCandidate(const PlayerBotGoalPlannerSnapshot& snapshot) const;
};

#endif
