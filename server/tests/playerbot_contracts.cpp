// Standalone pure-contract regression: see playerbot_contracts.sh.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>

#include "playerbotgoalplanner.h"
#include "playerbotdangerretreat.h"
#include "playerbothuntregions.h"
#include "playerbotturnrouter.h"

namespace {
void dangerRetreat()
{
	PlayerBotDangerRetreat retreat;
	const auto now = std::chrono::steady_clock::time_point{};
	assert(!retreat.active());
	assert(retreat.allowsDefense(42, false));
	retreat.begin();
	assert(retreat.active());
	// Adjacent attackers (including a crowd) never override escape without
	// navigation's failed-detour evidence. Repeated turns cannot reacquire them.
	for (int turn = 0; turn < 100; ++turn) {
		for (uint32_t id = 42; id < 46; ++id) assert(!retreat.allowsDefense(id, false));
	}
	assert(retreat.allowsDefense(42, true));
	retreat.beginDefense(42, now);
	assert(!retreat.defenseExpired(now + std::chrono::seconds(4)));
	assert(retreat.defenseExpired(now + std::chrono::seconds(5)));
	assert(!retreat.allowsDefense(42, true));
	// Re-entering service must not renew a spent combat budget.
	retreat.begin();
	assert(!retreat.allowsDefense(42, true));
	assert(retreat.defenseExpired(now + std::chrono::seconds(75)));
	assert(retreat.allowsDefense(43, true));
	retreat.beginDefense(43, now + std::chrono::seconds(75));
	assert(!retreat.defenseExpired(now + std::chrono::seconds(79)));
	assert(retreat.defenseExpired(now + std::chrono::seconds(80)));
	assert(!retreat.allowsDefense(42, true));
	retreat.finish();
	assert(!retreat.active());
	assert(!retreat.defenseExpired(now + std::chrono::hours(1)));
	assert(retreat.allowsDefense(42, false));
	retreat.begin();
	assert(retreat.allowsDefense(42, true));
	assert(!retreat.allowsDefense(42, false));

	PlayerBotTurnRouter router;
	assert(router.route({}) == PlayerBotTurnCommand::ReturnToDepot);
	router.pause();
	assert(router.route({}) == PlayerBotTurnCommand::None);
	router.start();
	router.stop();
	assert(router.route({}) == PlayerBotTurnCommand::None);
}

void projection()
{
	PlayerBotHuntRegion region;
	region.experiencePerMinute = 3;
	region.projectedExperience = region.score = 64.62;
	region.availableHuntSeconds = 861.6;
	region.routeDangerCost = 17;
	region.reconcileTravel(900, 23.6, 1.5);
	assert(std::abs(region.projectedExperience - 65.73) < 1e-9);
	assert(region.score == region.projectedExperience);
	assert(region.routeDangerCost == 17);
	region.reconcileTravel(300, 23.6, 1);
	assert(std::abs(region.score - 13.82) < 1e-9);
	region.reconcileTravel(900, 23.6, 1);
	assert(std::abs(region.score - 43.82) < 1e-9);

	// A changed horizon needs a fresh weighted stamina multiplier, not the old XP/s.
	region.observedCorrection = 0.8;
	region.reconcileTravel(900, 100, 1 + 0.5 * 180 / 800);
	assert(std::abs(region.score - 35.6) < 1e-9);
	assert(region.staminaExperienceMultiplier == 1.1125);
	region.reconcileTravel(900, 1000, 1);
	assert(region.availableHuntSeconds == 0 && region.score == 0);
	region.reconcileTravel(900, 0, 0.5);
	assert(std::abs(region.score - 18) < 1e-9);
	region.reconcileTravel(900, 0, 0);
	assert(region.score == 0);
}

void oracleRecovery()
{
	using Goal = PlayerBotGoalArbiter::TopLevelGoal;
	// Only missing emergency supplies may defer mandatory departure. There is
	// no optional-reward/learning candidate search on either mandatory branch.
	assert(PlayerBotGoalPlanner::requiredGoal(true, true) == Goal::Service);
	assert(PlayerBotGoalPlanner::requiredGoal(true, false) == Goal::Departure);
	assert(!PlayerBotGoalPlanner::requiredGoal(false, true));
	assert(!PlayerBotGoalPlanner::requiredGoal(false, false));

	// Startup selects Service once. Subsequent turns must execute the existing
	// depot/service route rather than re-enter goal selection and reset it.
	PlayerBotTurnRouter router;
	Goal activeGoal = *PlayerBotGoalPlanner::requiredGoal(true, true);
	const PlayerBotCyclePhase phases[] = {PlayerBotCyclePhase::ReturnToDepot,
	                                     PlayerBotCyclePhase::DepositLoot,
	                                     PlayerBotCyclePhase::Service};
	const PlayerBotTurnCommand commands[] = {PlayerBotTurnCommand::ReturnToDepot,
	                                        PlayerBotTurnCommand::DepositLoot,
	                                        PlayerBotTurnCommand::Service};
	for (size_t i = 0; i < 3; ++i) {
		router.setCyclePhase(phases[i]);
		for (int turn = 0; turn < 3; ++turn) {
			assert(PlayerBotGoalPlanner::shouldContinueRecovery(activeGoal, true));
			assert(router.route({}) == commands[i]);
			assert(router.cyclePhase() == phases[i]);
		}
	}
	// Supplied but still hurt: normal healing keeps its existing turn priority.
	assert(PlayerBotGoalPlanner::shouldContinueRecovery(activeGoal, true));
	// Recovered, or selecting a goal at service completion: mandatory departure
	// resumes before any optional candidate search can run.
	assert(!PlayerBotGoalPlanner::shouldContinueRecovery(activeGoal, false));
	activeGoal = *PlayerBotGoalPlanner::requiredGoal(true, false);
	assert(activeGoal == Goal::Departure);
	for (Goal optional : {Goal::PickupReward, Goal::LearnSpell, Goal::BuyEquipment,
	                      Goal::MagicTraining, Goal::SellLoot, Goal::Hunt}) {
		assert(!PlayerBotGoalPlanner::shouldContinueRecovery(optional, true));
		assert(PlayerBotGoalPlanner::requiredGoal(true, false) == Goal::Departure);
	}
}

}

int main()
{
	dangerRetreat();
	projection();
	oracleRecovery();
	std::cout << "playerbot contracts passed\n";
}
