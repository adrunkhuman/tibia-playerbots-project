// Standalone pure-contract regression: see playerbot_contracts.sh.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>

#include "playerbotgoalplanner.h"
#include "playerbottransitcombat.h"
#include "playerbothuntregions.h"
#include "playerbotturnrouter.h"

namespace {
void transitCombat()
{
	using Phase = PlayerBotCyclePhase;
	using Stage = PlayerBotScenarioStage;
	assert(PlayerBotTransitCombat::required(Phase::ReturnToDepot, Stage::Traverse, true, false));
	assert(PlayerBotTransitCombat::required(Phase::Hunt, Stage::Traverse, false, false));
	assert(PlayerBotTransitCombat::required(Phase::Hunt, Stage::LootCorpse, true, false));
	assert(PlayerBotTransitCombat::required(Phase::Hunt, Stage::Traverse, true, true));
	assert(!PlayerBotTransitCombat::required(Phase::Hunt, Stage::Traverse, true, false));
	PlayerBotTransitCombat episode;
	const auto start = std::chrono::steady_clock::time_point{};
	assert(episode.observe(true, 1, Phase::ReturnToDepot));
	episode.beginDefense(42, start);
	for (int turn = 0; turn < 100; ++turn) {
		assert(!episode.observe(true, 1, Phase::ReturnToDepot));
		assert(!episode.allowsDefense(42, true));
		for (uint32_t id = 43; id < 48; ++id) assert(!episode.allowsDefense(id, false));
	}
	// Wall-clock expiry is independent of how many turns healing consumes.
	assert(episode.defenseExpired(start + std::chrono::seconds(5)));
	assert(episode.observe(true, 2, Phase::ReturnToDepot));
	assert(episode.allowsDefense(42, true));
	episode.beginDefense(42, start);
	assert(episode.observe(true, 2, Phase::Service));
	assert(episode.allowsDefense(42, true));
	episode.beginDefense(42, start);
	// NPC approach completes a coarse leg and then a local leg under the
	// same goal/phase. Neither arrival completes the transit episode.
	for (int approachLeg = 0; approachLeg < 2; ++approachLeg) {
		assert(!episode.observe(true, 2, Phase::Service));
		assert(!episode.allowsDefense(42, true));
		assert(episode.defenseExpired(start + std::chrono::seconds(5 + approachLeg)));
	}
	// Semantic completion (the next phase), rather than a route-leg arrival,
	// permits a fresh attempt on a subsequent transit episode.
	assert(episode.observe(false, 2, Phase::Hunt));
	assert(episode.allowsDefense(42, false));

	PlayerBotTransitCombat retreat;
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

void crowdDamageInflation()
{
	// One attacker: 10 damage/s for two seconds, with no concurrent exposure.
	assert(playerBotCrowdDamageInflation(10 * 2, 10 * 2) == 1);
	// Two identical attackers: both attack for the first fight, one for the
	// second. Spawn-rate damage is already 2D; 1.5 produces 3D, not 6D.
	const double identicalIsolated = 10 * 2 + 10 * 2;
	const double identicalCrowd = (10 + 10) * 2 + 10 * 2;
	const double identicalInflation = playerBotCrowdDamageInflation(identicalCrowd, identicalIsolated);
	assert(identicalInflation == 1.5);
	assert(identicalIsolated * identicalInflation == identicalCrowd);
	// Unequal damage and durations, in the adapter's descending-DPS order:
	// 20/s for two seconds, then 10/s for five. Normalize against BOTH isolated
	// fights (90), not the strongest solo fight (50), nor another crowd's sum.
	const double unequalIsolated = 20 * 2 + 10 * 5;
	const double unequalCrowd = (20 + 10) * 2 + 10 * 5;
	const double unequalInflation = playerBotCrowdDamageInflation(unequalCrowd, unequalIsolated);
	assert(std::abs(unequalInflation - 11.0 / 9.0) < 1e-12);
	assert(std::abs(unequalIsolated * unequalInflation - unequalCrowd) < 1e-12);
	// The larger absolute-damage crowd need not have the larger ratio.
	assert(unequalCrowd > identicalCrowd);
	assert(std::max(identicalInflation, unequalInflation) == 1.5);
	assert(playerBotCrowdDamageInflation(0, 0) == 1);
	assert(playerBotCrowdDamageInflation(9, 10) == 1);
}

void overBudgetHunts()
{
	PlayerBotHuntRegion easy, costly;
	easy.suitable = costly.suitable = easy.reachable = costly.reachable = true;
	easy.experiencePerMinute = 10;
	costly.experiencePerMinute = 100;
	easy.expectedDamagePerSecond = 0.1;
	costly.expectedDamagePerSecond = 1;
	easy.supplyProfile.potions = costly.supplyProfile.potions = 2;
	easy.supplyProfile.potionHealing = costly.supplyProfile.potionHealing = 125;
	easy.reconcileTravel(1500, 0, 1);
	costly.reconcileTravel(1500, 0, 1);
	assert(easy.supplyBudget.expectedPotions == 2 && costly.supplyBudget.expectedPotions == 12);
	assert(!easy.supplyBudget.fits && !costly.supplyBudget.fits);
	assert(easy.score < costly.score);
	assert(playerBotPreferHuntRegion(easy, costly));
	assert(!playerBotPreferHuntRegion(costly, easy));
	assert(easy.suitable && costly.suitable); // Budget failure is not an eligibility gate.
	assert(std::string(playerBotHuntSelectionRule(easy)) == "lowest_potion_consumption_then_xp");
	easy.suitable = false;
	assert(playerBotPreferHuntRegion(costly, easy));
	easy.suitable = true;
	easy.reachable = false;
	assert(playerBotPreferHuntRegion(costly, easy));
	easy.reachable = true;
	// Equal consumption uses XP; a complete tie does not reorder candidates.
	PlayerBotHuntRegion equalCost = easy;
	equalCost.score = easy.score + 1;
	assert(playerBotPreferHuntRegion(equalCost, easy));
	assert(!playerBotPreferHuntRegion(easy, equalCost));
	equalCost.score = easy.score;
	assert(!playerBotPreferHuntRegion(equalCost, easy) && !playerBotPreferHuntRegion(easy, equalCost));
	// Plentiful supplies preserve XP ordering even with different consumption.
	easy.supplyProfile.potions = costly.supplyProfile.potions = 14;
	easy.reconcileSupplies(1);
	costly.reconcileSupplies(1);
	assert(easy.supplyBudget.fits && costly.supplyBudget.fits);
	assert(playerBotPreferHuntRegion(costly, easy));
	// Actual route reserves can remove one fit, then all fits. Compare again
	// with the same policy used by route-shortlist selection.
	costly.reconcileSupplies(3);
	assert(!costly.supplyBudget.fits && playerBotPreferHuntRegion(easy, costly));
	easy.reconcileSupplies(13);
	costly.reconcileSupplies(13);
	assert(!easy.supplyBudget.fits && !costly.supplyBudget.fits);
	assert(playerBotPreferHuntRegion(easy, costly));
	// Reconciled travel time changes the remaining hunt and expected consumption.
	costly.reconcileTravel(1500, 1400, 1);
	assert(costly.supplyBudget.expectedPotions == 1 && costly.supplyBudget.fits);
	assert(costly.score < easy.score && playerBotPreferHuntRegion(costly, easy));
}

void supplyBudget()
{
	PlayerBotSupplyProfile profile;
	profile.potions = 2;
	profile.potionHealing = 125;
	PlayerBotHuntRegion easy, costly;
	easy.suitable = costly.suitable = easy.reachable = costly.reachable = true;
	easy.experiencePerMinute = 10;
	costly.experiencePerMinute = 100;
	easy.expectedDamagePerSecond = 0.1;
	costly.expectedDamagePerSecond = 1;
	easy.combatFraction = costly.combatFraction = 0.5;
	easy.supplyProfile = costly.supplyProfile = profile;
	easy.reconcileTravel(900, 0, 1);
	costly.reconcileTravel(900, 0, 1);
	assert(easy.supplyBudget.expectedPotions == 1 && easy.supplyBudget.fits);
	assert(costly.supplyBudget.expectedPotions == 8 && !costly.supplyBudget.fits);
	assert(playerBotPreferHuntRegion(easy, costly));
	assert(!playerBotPreferHuntRegion(costly, easy));
	// Safety outranks economy, including an otherwise zero-consumption region.
	easy.suitable = false;
	assert(playerBotPreferHuntRegion(costly, easy));
	easy.suitable = true;
	easy.reachable = false;
	assert(playerBotPreferHuntRegion(costly, easy));
	easy.reachable = true;
	costly.supplyProfile.potions = 10;
	costly.reconcileSupplies(1);
	assert(costly.supplyBudget.fits && playerBotPreferHuntRegion(costly, easy));
	costly.reconcileSupplies(3);
	assert(!costly.supplyBudget.fits && playerBotPreferHuntRegion(easy, costly));
	costly.reconcileTravel(60, 0, 1);
	assert(costly.supplyBudget.expectedPotions == 1 && costly.supplyBudget.fits);
	profile.potions = 1;
	assert(!playerBotSupplyBudget(profile, 0, 0, 900, 0).fits);
	profile.potions = 0;
	assert(!playerBotSupplyBudget(profile, 0, 0, 900, 0).fits);
	profile.potions = 2;
	profile.regenerationSeconds = 900;
	profile.healthGain = 10;
	profile.healthInterval = 10;
	auto budget = playerBotSupplyBudget(profile, 1, 0.5, 900, 0);
	assert(budget.regenerationHealing == 450 && budget.expectedPotions == 4);
	budget = playerBotSupplyBudget(profile, 1, 0.5, 900, 900);
	assert(budget.regenerationHealing == 0 && budget.expectedPotions == 8);
	profile.regenerationSeconds = 100;
	assert(playerBotSupplyBudget(profile, 1, 0.5, 900, 0).regenerationHealing == 50);
	profile.regenerationSeconds = 9;
	assert(playerBotSupplyBudget(profile, 1, 1, 900, 0).regenerationHealing == 0);
	profile.regenerationSeconds = 900;
	profile.mana = profile.maximumMana = 100;
	profile.spellMana = 20;
	profile.spellHealing = 40;
	profile.spellInterval = 1;
	budget = playerBotSupplyBudget(profile, 1, 0.5, 900, 0);
	assert(budget.spellHealing == 0); // unlearned/illegal Exura gets no credit
	profile.spellLegal = true;
	budget = playerBotSupplyBudget(profile, 1, 0.5, 900, 0);
	assert(budget.spellHealing == 120 && budget.expectedPotions == 3);
	profile.mana = 0;
	profile.manaGain = 1;
	profile.manaInterval = 10;
	assert(playerBotSupplyBudget(profile, 1, 0.5, 900, 0).spellHealing == 0);
	profile.manaGain = 10;
	assert(playerBotSupplyBudget(profile, 1, 0.5, 900, 0).spellHealing == 120);
	profile.maximumMana = 59;
	assert(playerBotSupplyBudget(profile, 1, 0.5, 900, 0).spellHealing == 0);
	profile.potionHealing = 0;
	budget = playerBotSupplyBudget(profile, 1, 0, 900, 0);
	assert(std::isfinite(budget.expectedPotions) && !budget.fits);
}

void recoverySpellPriority()
{
	using Goal = PlayerBotGoalArbiter::TopLevelGoal;
	assert(playerBotRecoverySpendingReserve(2, 2, 45, 100) == 100);
	assert(playerBotRecoverySpendingReserve(2, 10, 45, 100) == 460);
	assert(playerBotRecoverySpendingReserve(2, 4, 45, 100) == 190);
	assert(playerBotAffordableAfterReserve(270, 100, 170));
	assert(!playerBotAffordableAfterReserve(269, 100, 170));
	assert(!playerBotAffordableAfterReserve(34, 100, 170));
	assert(!playerBotAffordableAfterReserve(UINT64_MAX, UINT64_MAX, 170));
	PlayerBotGoalPlanner planner;
	PlayerBotGoalPlannerSnapshot snapshot;
	snapshot.spellPlanAvailable = snapshot.recoverySpellPlanAvailable = true;
	snapshot.rewardPlanAvailable = true;
	snapshot.rewardUtility = 2000;
	snapshot.sellLootPlanAvailable = true;
	snapshot.sellLootUtility = 3000;
	snapshot.equipmentEnabled = snapshot.equipmentPlanAvailable = true;
	snapshot.cashAdjustment = true;
	auto candidates = planner.candidates(snapshot);
	auto candidate = [&](Goal goal) -> const PlayerBotGoalArbiter::GoalCandidate& {
		return *std::find_if(candidates.begin(), candidates.end(), [goal](const auto& value) { return value.goal == goal; });
	};
	assert(candidate(Goal::LearnSpell).feasible && candidate(Goal::LearnSpell).reason == "priority_recovery_spell");
	for (Goal goal : {Goal::PickupReward, Goal::BuyEquipment, Goal::SellLoot, Goal::Hunt, Goal::MagicTraining, Goal::Service}) {
		assert(!candidate(goal).feasible && candidate(goal).reason == "deferred_recovery_spell");
	}
	snapshot.lowCapacity = true;
	candidates = planner.candidates(snapshot);
	assert(candidate(Goal::Service).feasible && candidate(Goal::Service).utility > candidate(Goal::LearnSpell).utility);
	snapshot.lowCapacity = false;
	snapshot.spellCoolingDown = true;
	candidates = planner.candidates(snapshot);
	assert(!candidate(Goal::LearnSpell).feasible && candidate(Goal::PickupReward).feasible);
	snapshot.spellCoolingDown = false;
	snapshot.recoverySpellPlanAvailable = false;
	candidates = planner.candidates(snapshot);
	assert(candidate(Goal::SellLoot).feasible && candidate(Goal::SellLoot).utility > candidate(Goal::LearnSpell).utility);
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
	transitCombat();
	crowdDamageInflation();
	overBudgetHunts();
	supplyBudget();
	recoverySpellPriority();
	projection();
	oracleRecovery();
	std::cout << "playerbot contracts passed\n";
}
