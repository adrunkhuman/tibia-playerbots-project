// Standalone pure-contract regression: see playerbot_contracts.sh.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "playerbot.h"
#include "playerbotdepotworkflow.h"
#include "playerboteconomy.h"
#include "playerbotequipmentpolicy.h"
#include "playerbotgoalplanner.h"
#include "playerbotprogressionplanners.h"
#include "playerbotcombatruntime.h"
#include "playerbotcombattarget.h"
#include "playerbotnavigationruntime.h"
#include "playerbottransitcombat.h"
#include "playerbottopology.h"
#include "playerbothuntregions.h"
#include "playerbothuntruntime.h"
#include "playerbotinventorypolicy.h"
#include "playerbotsupplyrecovery.h"
#include "playerbottelemetry.h"
#include "playerbottestpolicy.h"
#include "playerbotturnrouter.h"

namespace {
void loggingContracts()
{
	playerbot::PlayerBotTelemetrySummary unavailableSummary;
	assert(!unavailableSummary.playerStateAvailable);

	using playerbot::PlayerBotLogControl;
	assert(playerbot::parseLogControl("logs on") == PlayerBotLogControl::On);
	assert(playerbot::parseLogControl("logs off") == PlayerBotLogControl::Off);
	assert(playerbot::parseLogControl("Logs on") == PlayerBotLogControl::None);
	assert(playerbot::parseLogControl("logs on ") == PlayerBotLogControl::None);
	assert(playerbot::parseLogControl("status") == PlayerBotLogControl::None);

	playerbot::PlayerBotAnnouncementSettings settings;
	assert(settings.enabled(10));
	assert(settings.enabled(20));
	settings.set(10, false);
	assert(!settings.enabled(10));
	assert(settings.enabled(20));
	settings.set(10, true);
	assert(settings.enabled(10));
}

void huntEconomy()
{
	// Inclusive roll and strict comparison: even chance=100000 misses one roll.
	assert(playerBotExpectedLootCount(0, 10, true, 1) == 0);
	assert(playerBotExpectedLootCount(100000, 10, true, 0) == 0);
	assert(std::abs(playerBotExpectedLootCount(100000, 1, false, 1) - 100000.0 / 100001) < 1e-10);
	assert(std::abs(playerBotExpectedLootCount(3, 10, true, 1) - 6.0 / 100001) < 1e-10);
	assert(std::abs(playerBotExpectedLootCount(3, 10, true, 2) - 12.0 / 100001) < 1e-10);
	assert(std::abs(playerBotExpectedLootCount(3, 10, true, 0.5) - 4.0 / 100001) < 1e-10);
	assert(std::abs(playerBotExpectedLootCount(100000, 10, true, 1) - 550000.0 / 100001) < 1e-10);
	assert(playerBotExpectedLootCount(100000, 1, false, 2) * 10000 == 10000);

	std::vector<PlayerBotReplenishmentPoint> pocket;
	for (int x : {100, 102, 104}) pocket.push_back({Position(x, 100, 7), Position(x, 101, 7), 120, 240, false});
	assert(!playerBotHuntViability(pocket, 0.2).eligible);
	pocket.push_back({Position(103, 100, 7), Position(103, 101, 7), 120, 240, false});
	assert(!playerBotHuntViability(pocket, 0.2).eligible); // count alone is insufficient
	pocket.pop_back();
	// Keep the tiny pocket's members, add a second pocket to the circuit.
	for (int x : {130, 132, 134}) pocket.push_back({Position(x, 100, 7), Position(x, 101, 7), 120, 240, false});
	auto viable = playerBotHuntViability(pocket, 0.2);
	assert(viable.eligible && viable.reachableSpawns == 6 && viable.replenishingSpawns == 6);
	std::rotate(pocket.begin(), pocket.begin() + 1, pocket.end());
	assert(playerBotHuntViability(pocket, 0.2).replenishingSpawns == 6);
	auto narrow = pocket;
	for (auto& point : narrow) {
		const int x = point.spawn.x < 130 ? 100 + (point.spawn.x - 100) / 2 : 130 + (point.spawn.x - 130) / 2;
		point.spawn.x = point.approach.x = x;
	}
	const auto narrowOpportunity = playerBotHuntViability(narrow, 0.2);
	assert(narrowOpportunity.minimumUnblockedPatrolRatio >= 0.25);
	assert(narrowOpportunity.minimumEmptyPatrolRatio < 0.05);
	assert(!narrowOpportunity.eligible); // ample combat cannot hide a token empty-patrol share
	auto sorted = pocket;
	std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
		return left.spawn.x < right.spawn.x;
	});
	std::vector<PlayerBotReplenishmentPoint> alternating;
	for (size_t index : {0, 3, 1, 4, 2, 5}) alternating.push_back(sorted[index]);
	assert(!playerBotHuntViability(alternating, 0.2).eligible); // fights alone cannot support empty patrols
	// One distant block does not make an otherwise blocked pocket sustainable.
	auto token = pocket;
	for (size_t i = 0; i < 5; ++i) {
		token[i].spawn = Position(100, 100, 7);
		token[i].approach = Position(100, 101, 7);
		token[i].fightSeconds = 60;
	}
	token[5].spawn = Position(130, 100, 7);
	token[5].approach = Position(130, 101, 7);
	token[5].fightSeconds = 1;
	assert(playerBotHuntViability(token, 0.2).replenishingSpawns == 0);
	assert(!playerBotHuntViability(token, 0.2).eligible);
	for (auto& point : pocket) point.fightSeconds = 10;
	assert(playerBotHuntViability(pocket, 0.2).eligible); // no full interval of absence required
	assert(playerBotHuntViability(pocket, 0.2).minimumAwayRatio < 1);
	for (auto& point : pocket) point.fightSeconds = 0;
	assert(!playerBotHuntViability(pocket, 0.2).eligible); // brief 6% duty share is insufficient
	for (auto& point : pocket) point.ignoresBlocking = true;
	assert(playerBotHuntViability(pocket, 0.2).eligible);
	pocket.resize(3);
	assert(!playerBotHuntViability(pocket, 0.2).eligible); // exception does not waive size
	pocket.resize(257);
	assert(playerBotHuntViability(pocket, 0.2).modelLimited);

	assert(playerBotHuntCashPressure(2, 1, 132, 500));
	assert(!playerBotHuntCashPressure(2, 1, 500, 500));
	assert(!playerBotHuntCashPressure(10, 1, 132, 500));
	PlayerBotHuntRegion income, xp;
	income.suitable = xp.suitable = income.reachable = xp.reachable = true;
	income.cashPressure = xp.cashPressure = true;
	income.coinGoldPerMinute = 10;
	income.score = 10;
	xp.score = 100;
	assert(playerBotPreferHuntRegion(income, xp));
	income.cashPressure = xp.cashPressure = false;
	assert(playerBotPreferHuntRegion(xp, income));
	income.cashPressure = xp.cashPressure = true;
	income.suitable = false;
	assert(playerBotPreferHuntRegion(xp, income));
	income.suitable = true;
	income.reachable = false;
	assert(playerBotPreferHuntRegion(xp, income));
	income.reachable = true;
	income.supplyBudget.fits = false;
	assert(playerBotPreferHuntRegion(xp, income));
	xp.supplyBudget.fits = false;
	income.supplyBudget.expectedPotions = 2;
	xp.supplyBudget.expectedPotions = 1;
	assert(playerBotPreferHuntRegion(xp, income));
	income.supplyBudget.expectedPotions = 1;
	assert(playerBotPreferHuntRegion(income, xp));
}

void patrolOpportunity()
{
	std::vector<PlayerBotReplenishmentPoint> points;
	for (Position p : {Position(100, 100, 7), Position(140, 100, 7),
	                   Position(140, 140, 7), Position(100, 140, 7)}) {
		points.push_back({p, p, 0, 2400, false, 10, 20});
	}
	const auto empty = playerBotHuntViability(points, 0.2);
	assert(empty.eligible && empty.replenishingSpawns == 4);
	// Two outside edges of a four-edge lap: never count the second diagnostic lap.
	assert(empty.minimumUnblockedPatrolRatio == 0.5);
	assert(empty.minimumEmptyPatrolRatio == 0.5);
	assert(std::abs(empty.minimumAwayRatio - 16.0 / 2400) < 1e-9);
	const auto emptyYield = playerBotSustainedHuntYield(points, empty, 32);
	assert(emptyYield.coinGoldPerMinute == 0.5);
	assert(emptyYield.spawnExperiencePerMinute == 1);
	// Slow fights may increase full-cycle share, but not empty-patrol opportunity
	// or the rate cap. Zero combat already provides a viable circuit here.
	for (auto& point : points) point.fightSeconds = 100;
	const auto slow = playerBotHuntViability(points, 0.2);
	assert(slow.eligible && slow.minimumUnblockedPatrolRatio > 0.5);
	assert(slow.minimumEmptyPatrolRatio == 0.5);
	assert(playerBotSustainedHuntYield(points, slow, 432).coinGoldPerMinute == emptyYield.coinGoldPerMinute);
	std::rotate(points.begin(), points.begin() + 1, points.end());
	const auto rotated = playerBotHuntViability(points, 0.2);
	assert(rotated.minimumUnblockedPatrolRatio == slow.minimumUnblockedPatrolRatio);
	assert(rotated.minimumAwayRatio == slow.minimumAwayRatio);

	// Map-derived six-spawn Snake/Spider site from the failed hunt_planning
	// scan. West/south approaches approximate nearestApproach from Carlin;
	// fight durations mirror the logged static profile, not boosted equipment.
	points.clear();
	for (Position p : {Position(32403, 31725, 9), Position(32402, 31728, 9), Position(32405, 31729, 9),
	                   Position(32413, 31726, 8), Position(32406, 31728, 8), Position(32408, 31730, 8)}) {
		points.push_back({p, Position(p.x - 1, p.y + 1, p.z), p.z == 9 ? 10.8 : 6.6667,
		    240, false, 0, p.z == 9 ? 12.0 : 10.0});
	}
	const auto site = playerBotHuntViability(points, 0.2);
	assert(site.eligible && site.replenishingSpawns == 6);
	assert(site.minimumAwayRatio < 0.1); // full-interval gating rejected this site
	assert(site.minimumUnblockedPatrolRatio >= 0.25);
	assert(site.minimumEmptyPatrolRatio >= 0.05 && site.minimumEmptyPatrolRatio < 0.25);
	assert(playerBotSustainedHuntYield(points, site, 70).spawnExperiencePerMinute > 0);
}

void mixedSustainedYield()
{
	std::vector<PlayerBotReplenishmentPoint> points;
	for (int x : {100, 102, 104, 130, 132, 134}) {
		points.push_back({Position(x, 100, 7), Position(x, 101, 7), x >= 130 ? 1.0 : 100.0, 60, false,
		    x >= 130 ? 10.0 : 10000.0, x >= 130 ? 20.0 : 20000.0});
	}
	const auto viability = playerBotHuntViability(points, 0.2);
	assert(viability.eligible && viability.replenishingSpawns == 3);
	assert(viability.replenishingMembers == std::vector<bool>({false, false, false, true, true, true}));
	const auto spawnLimited = playerBotSustainedHuntYield(points, viability, 30);
	// Four outside travel tiles per 68-tile empty lap cap the spawn rates,
	// despite abundant modeled combat time away from the qualifying group.
	assert(std::abs(spawnLimited.coinGoldPerMinute - 30 * 4.0 / 68) < 1e-9);
	assert(std::abs(spawnLimited.spawnExperiencePerMinute - 60 * 4.0 / 68) < 1e-9);
	assert(spawnLimited.clearExperiencePerMinute == 120);
	const auto clearLimited = playerBotSustainedHuntYield(points, viability, 1200);
	assert(clearLimited.coinGoldPerMinute == 1.5);
	assert(clearLimited.clearExperiencePerMinute == 3);
	// Rich initial-clear monsters must not alter either sustained forecast.
	for (size_t i = 0; i < 3; ++i) points[i].coinGold = points[i].experience = 0;
	assert(playerBotSustainedHuntYield(points, viability, 1200).coinGoldPerMinute == 1.5);
	assert(playerBotSustainedHuntYield(points, viability, 1200).clearExperiencePerMinute == 3);
}

struct RuntimeHuntPlan {
	PlayerBotHuntRuntimeOutcome scored;
	PlayerBotHuntRuntimeOutcome selection;
};

RuntimeHuntPlan planRuntimeHunts(const std::vector<PlayerBotHuntRegion>& candidates)
{
	PlayerBotHuntRuntime runtime({});
	PlayerBotHuntRuntimePlanningInput input;
	input.cacheRevision = 1;
	input.start.emplace();
	input.start->scan.revision = 1;
	input.start->scan.candidateCount = candidates.size();
	for (size_t i = 0; i < candidates.size(); ++i) input.start->scan.candidateIndices.push_back(i);
	const auto now = std::chrono::steady_clock::time_point{};
	assert(runtime.advancePlanning(input, now).command == PlayerBotHuntRuntimeCommand::PlanningStarted);
	assert(runtime.advancePlanning(input, now).scoreWork.size() == candidates.size());
	std::vector<PlayerBotHuntRuntimeScoreObservation> observations;
	for (size_t i = 0; i < candidates.size(); ++i) observations.push_back({i, true, true, true, candidates[i]});
	RuntimeHuntPlan plan;
	plan.scored = runtime.completeScoreWork(observations, 0);
	assert(plan.scored.command == PlayerBotHuntRuntimeCommand::PlanningScored && plan.scored.candidateSnapshot);
	plan.selection = runtime.advancePlanning(input, now);
	assert(plan.selection.command == PlayerBotHuntRuntimeCommand::RegionSelected && plan.selection.selectedRegion);
	return plan;
}

PlayerBotHuntRegion selectRuntimeHunt(const std::vector<PlayerBotHuntRegion>& candidates)
{
	return *planRuntimeHunts(candidates).selection.selectedRegion;
}

void huntCandidateTelemetryCompleteness()
{
	// The completed runtime outcome is the controller's telemetry source. Keep
	// every candidate beyond the former 64-record telemetry-only cap.
	std::vector<PlayerBotHuntRegion> candidates(65);
	for (size_t index = 0; index < candidates.size(); ++index) {
		candidates[index].atlasVariantId = index + 1;
		candidates[index].suitable = true;
		candidates[index].reachable = true;
		candidates[index].supplyBudget.fits = true;
		candidates[index].score = static_cast<double>(candidates.size() - index);
	}
	const RuntimeHuntPlan plan = planRuntimeHunts(candidates);
	assert(plan.scored.candidates.size() == candidates.size());
	assert(plan.scored.candidates.back().atlasVariantId == candidates.size());
	assert(plan.selection.candidates.empty());
}

void huntCandidateTelemetryDeltas()
{
	auto inputFor = [](uint64_t revision, size_t count) {
		PlayerBotHuntRuntimePlanningInput input;
		input.cacheRevision = revision;
		input.start.emplace();
		input.start->scan.revision = revision;
		input.start->scan.candidateCount = count;
		for (size_t index = 0; index < count; ++index) input.start->scan.candidateIndices.push_back(index);
		return input;
	};
	auto observationsFor = [](const std::vector<PlayerBotHuntRuntimeScoreWork>& work) {
		std::vector<PlayerBotHuntRuntimeScoreObservation> observations;
		observations.reserve(work.size());
		for (const PlayerBotHuntRuntimeScoreWork& candidate : work) {
			PlayerBotHuntRegion region;
			region.atlasVariantId = candidate.candidateIndex + 1;
			region.suitable = region.reachable = true;
			region.score = static_cast<double>(300 - candidate.candidateIndex);
			observations.push_back({candidate.candidateIndex, true, true, true, region});
		}
		return observations;
	};

	const auto now = std::chrono::steady_clock::time_point{};
	PlayerBotHuntRuntime runtime({});
	auto input = inputFor(11, 300);
	const auto started = runtime.advancePlanning(input, now);
	assert(started.command == PlayerBotHuntRuntimeCommand::PlanningStarted && started.planningPass == 1 &&
	       started.scoringRevision == 11);
	const auto firstWork = runtime.advancePlanning(input, now);
	assert(firstWork.scoreWork.size() == 256);
	const auto firstScored = runtime.completeScoreWork(observationsFor(firstWork.scoreWork), 0);
	assert(firstScored.command == PlayerBotHuntRuntimeCommand::PlanningYield && !firstScored.candidateSnapshot &&
	       firstScored.candidates.empty() && firstScored.planningPass == started.planningPass);
	const auto secondWork = runtime.advancePlanning(input, now);
	assert(secondWork.scoreWork.size() == 44);
	const auto scored = runtime.completeScoreWork(observationsFor(secondWork.scoreWork), 0);
	assert(scored.command == PlayerBotHuntRuntimeCommand::PlanningScored && scored.candidateSnapshot &&
	       scored.candidates.size() == 300 && scored.routeCandidates.size() == 300 &&
	       scored.planningPass == started.planningPass && scored.scoringRevision == 11);
	const auto validationTurn = runtime.advancePlanning(input, now);
	assert(validationTurn.command == PlayerBotHuntRuntimeCommand::RegionSelected && validationTurn.selectedRegion &&
	       !validationTurn.candidateSnapshot && validationTurn.candidates.empty() && validationTurn.routeCandidates.empty() &&
	       validationTurn.planningPass == started.planningPass);
	PlayerBotHuntPlanningObservation unavailable;
	unavailable.candidatesAvailable = false;
	const auto exhausted = runtime.advancePlanning(input, now, unavailable);
	assert(exhausted.command == PlayerBotHuntRuntimeCommand::ScopeExhausted && exhausted.candidates.empty() &&
	       exhausted.planningPass == started.planningPass && exhausted.scoringRevision == 11);

	PlayerBotHuntRuntime cancelledRuntime({});
	auto cancelledInput = inputFor(21, 1);
	const auto cancelledStart = cancelledRuntime.advancePlanning(cancelledInput, now);
	PlayerBotHuntPlanningObservation cancel;
	cancel.cancelAtScoreBarrier = true;
	const auto cancelled = cancelledRuntime.advancePlanning(cancelledInput, now, cancel);
	assert(cancelled.command == PlayerBotHuntRuntimeCommand::PlanningCancelled &&
	       cancelled.planningPass == cancelledStart.planningPass && cancelled.scoringRevision == 21);
	const auto restarted = cancelledRuntime.advancePlanning(cancelledInput, now);
	assert(restarted.command == PlayerBotHuntRuntimeCommand::PlanningStarted &&
	       restarted.planningPass == cancelledStart.planningPass + 1);
	const auto explicitlyCancelled = cancelledRuntime.cancelPlanning();
	assert(explicitlyCancelled.command == PlayerBotHuntRuntimeCommand::PlanningCancelled &&
	       explicitlyCancelled.planningCancelled && explicitlyCancelled.planningPass == restarted.planningPass &&
	       explicitlyCancelled.scoringRevision == restarted.scoringRevision &&
	       std::strcmp(explicitlyCancelled.cancellationReason, "planning_cancelled") == 0);

	PlayerBotHuntRuntime invalidatedRuntime({});
	auto oldInput = inputFor(31, 1);
	const auto oldStart = invalidatedRuntime.advancePlanning(oldInput, now);
	auto refreshedInput = inputFor(32, 1);
	const auto refreshedStart = invalidatedRuntime.advancePlanning(refreshedInput, now);
	assert(refreshedStart.command == PlayerBotHuntRuntimeCommand::PlanningStarted && refreshedStart.planningCancelled &&
	       refreshedStart.staleRevision && std::strcmp(refreshedStart.cancellationReason, "cache_revision_changed") == 0 &&
	       refreshedStart.invalidatedPlanningPass == oldStart.planningPass &&
	       refreshedStart.invalidatedScoringRevision == oldStart.scoringRevision &&
	       refreshedStart.planningPass == oldStart.planningPass + 1 && refreshedStart.scoringRevision == 32);

	PlayerBotHuntRuntime healthInvalidatedRuntime({});
	auto healthyInput = inputFor(41, 1);
	healthyInput.player.health = 100;
	const auto healthyStart = healthInvalidatedRuntime.advancePlanning(healthyInput, now);
	auto injuredInput = healthyInput;
	injuredInput.player.health = 99;
	const auto injuredRestart = healthInvalidatedRuntime.advancePlanning(injuredInput, now);
	assert(injuredRestart.command == PlayerBotHuntRuntimeCommand::PlanningStarted &&
	       injuredRestart.planningCancelled && !injuredRestart.staleRevision &&
	       std::strcmp(injuredRestart.cancellationReason, "health_decreased") == 0 &&
	       injuredRestart.invalidatedPlanningPass == healthyStart.planningPass &&
	       injuredRestart.invalidatedScoringRevision == healthyStart.scoringRevision &&
	       injuredRestart.planningPass == healthyStart.planningPass + 1);
}

void raisedHuntRecoveryReserve()
{
	using namespace playerbot;
	PlayerBotHuntRegion income, xp;
	income.atlasVariantId = 1;
	xp.atlasVariantId = 2;
	income.suitable = xp.suitable = income.reachable = xp.reachable = true;
	income.supplyProfile.potions = xp.supplyProfile.potions = 10;
	income.coinGoldPerMinute = 10;
	income.score = 10;
	xp.score = 100;
	const uint64_t funds = 200;
	const uint32_t price = 45;
	auto reconcile = [&](PlayerBotHuntRegion& region, uint32_t reserve, uint64_t money) {
		const uint32_t target = recoveryPotionRestockTargetForReserve(reserve);
		region.reconcileRecovery(reserve, money, playerBotRecoverySpendingReserve(
		    region.supplyProfile.potions, target, price, carriedGoldReserve));
	};
	reconcile(income, 1, funds);
	reconcile(xp, 1, funds);
	assert(!income.cashPressure && income.supplyBudget.fits);
	assert(selectRuntimeHunt({income, xp}).atlasVariantId == 2);
	assert(recoveryPotionRestockTargetForReserve(1, healthPotionSafetyTarget) == 2);
	assert(recoveryPotionRestockTargetForReserve(1) == healthPotionAmmoTarget);
	// Candidate outbound + return route facts raise the target beyond the ammo stack.
	const uint32_t candidateReserve = 2 + 10;
	assert(recoveryPotionRestockTargetForReserve(candidateReserve) == healthPotionAmmoTarget);
	assert(recoveryPotionRestockTargetForReserve(healthPotionAmmoTarget + 5) == healthPotionAmmoTarget + 6);
	assert(recoveryPotionRestockTargetForReserve(UINT32_MAX) == UINT32_MAX);
	reconcile(income, candidateReserve, funds);
	reconcile(xp, candidateReserve, funds);
	assert(income.cashPressure && !income.supplyBudget.fits);
	assert(selectRuntimeHunt({income, xp}).atlasVariantId == 1);
	income.predictedLethal = true;
	assert(selectRuntimeHunt({income, xp}).atlasVariantId == 2);
	income.predictedLethal = false;
	const uint64_t exactFunds = carriedGoldReserve +
	    static_cast<uint64_t>(healthPotionAmmoTarget - income.supplyProfile.potions) * price;
	reconcile(income, candidateReserve, exactFunds);
	reconcile(xp, candidateReserve, exactFunds);
	assert(!income.cashPressure && !income.supplyBudget.fits);
	assert(selectRuntimeHunt({income, xp}).atlasVariantId == 2);
}

void incrementalHuntValidationPipeline()
{
	std::vector<PlayerBotHuntRegion> candidates(10);
	for (size_t index = 0; index < candidates.size(); ++index) {
		PlayerBotHuntRegion& region = candidates[index];
		region.atlasVariantId = index + 1;
		region.suitable = region.reachable = true;
		region.supplyBudget.fits = true;
		region.score = 100 - index;
		region.optimisticProjectedExperience = 100 - index;
		region.topologyReachable = index % 2 == 0;
	}
	const RuntimeHuntPlan planned = planRuntimeHunts(candidates);
	assert(planned.scored.routeCandidates.size() == candidates.size());
	for (size_t index = 0; index < candidates.size(); ++index) {
		assert(planned.scored.routeCandidates[index].atlasVariantId == index + 1);
	}
	// There is no fixed-eight, local/remote, observed/unobserved, or income slot.
	assert(std::count_if(planned.scored.routeCandidates.begin(), planned.scored.routeCandidates.end(),
	    [](const auto& region) { return region.topologyReachable; }) == 5);

	// Nine route failures still leave the tenth candidate available to win.
	size_t validated = 0;
	std::optional<PlayerBotHuntRegion> winner;
	for (const PlayerBotHuntRegion& candidate : planned.scored.routeCandidates) {
		++validated;
		if (candidate.atlasVariantId == 10) winner = candidate;
	}
	assert(validated == 10 && winner && winner->atlasVariantId == 10);

	PlayerBotHuntRegion incumbent = candidates.front();
	incumbent.score = 95;
	incumbent.optimisticProjectedExperience = 95;
	assert(!playerBotHuntRemainingCanBeatValidated(candidates, 6, incumbent));
	candidates[6].optimisticProjectedExperience = incumbent.score;
	assert(playerBotHuntRemainingCanBeatValidated(candidates, 6, incumbent));
	candidates[6].optimisticProjectedExperience = 94;
	incumbent.supplyBudget.fits = false;
	assert(playerBotHuntRemainingCanBeatValidated(candidates, 6, incumbent));

	// A validated fitting winner skips each dominated candidate without hiding a
	// later candidate whose optimistic bound can still win in another supply tier.
	std::vector<PlayerBotHuntRegion> sparseCompetition(52);
	for (size_t index = 0; index < sparseCompetition.size(); ++index) {
		sparseCompetition[index].atlasVariantId = index + 1;
		sparseCompetition[index].suitable = sparseCompetition[index].reachable = true;
		sparseCompetition[index].supplyBudget.fits = true;
		sparseCompetition[index].optimisticProjectedExperience = 499;
	}
	PlayerBotHuntRegion earlyWinner = sparseCompetition.front();
	earlyWinner.score = 548;
	earlyWinner.supplyBudget.fits = true;
	sparseCompetition.back().supplyBudget.fits = false;
	sparseCompetition.back().optimisticProjectedExperience = 600;
	assert(playerBotNextHuntCandidateToValidate(sparseCompetition, 1, &earlyWinner) == 51);
	assert(playerBotHuntRemainingCanBeatValidated(sparseCompetition, 1, earlyWinner));
	sparseCompetition.back().optimisticProjectedExperience = 547;
	assert(playerBotNextHuntCandidateToValidate(sparseCompetition, 1, &earlyWinner) ==
	       sparseCompetition.size());

	// Scoring remains bounded per turn and cancellation is honored before a
	// second 256-candidate batch starts.
	PlayerBotHuntRuntime runtime({});
	PlayerBotHuntRuntimePlanningInput input;
	input.cacheRevision = 1;
	input.start.emplace();
	input.start->scan.revision = 1;
	input.start->scan.candidateCount = 300;
	for (size_t index = 0; index < 300; ++index) input.start->scan.candidateIndices.push_back(index);
	const auto now = std::chrono::steady_clock::time_point{};
	assert(runtime.advancePlanning(input, now).command == PlayerBotHuntRuntimeCommand::PlanningStarted);
	const auto firstTurn = runtime.advancePlanning(input, now);
	assert(firstTurn.scoreWork.size() == 256);
	std::vector<PlayerBotHuntRuntimeScoreObservation> observations;
	for (const auto& work : firstTurn.scoreWork) {
		PlayerBotHuntRegion region;
		region.suitable = region.reachable = true;
		observations.push_back({work.candidateIndex, true, true, true, region});
	}
	assert(runtime.completeScoreWork(observations, 0).command == PlayerBotHuntRuntimeCommand::PlanningYield);
	PlayerBotHuntPlanningObservation cancel;
	cancel.cancelAtScoreBarrier = true;
	assert(runtime.advancePlanning(input, now, cancel).command == PlayerBotHuntRuntimeCommand::PlanningCancelled);
	assert(!runtime.planningActive());

	// Transport closure is a separate bounded phase and cancellation is honored
	// between its eight-offer batches.
	PlayerBotHuntRuntime boundedTransport({});
	PlayerBotHuntRuntimePlanningInput boundedTransportInput;
	boundedTransportInput.cacheRevision = 1;
	boundedTransportInput.start.emplace();
	boundedTransportInput.start->scan.revision = 1;
	boundedTransportInput.start->originReachability = std::make_shared<PlayerBotTopologyReachability>();
	boundedTransportInput.start->transportOffers = std::make_shared<std::vector<PlayerBotHuntTransportOffer>>(10);
	assert(boundedTransport.advancePlanning(boundedTransportInput, now).command ==
	       PlayerBotHuntRuntimeCommand::PlanningStarted);
	const auto boundedTransportTurn = boundedTransport.advancePlanning(boundedTransportInput, now);
	assert(boundedTransportTurn.transportWork.size() == 8);
	std::vector<PlayerBotHuntRuntimeTransportObservation> emptyTransportObservations;
	for (const auto& work : boundedTransportTurn.transportWork) {
		emptyTransportObservations.push_back({work.offerIndex, std::nullopt});
	}
	assert(boundedTransport.completeTransportWork(emptyTransportObservations).command ==
	       PlayerBotHuntRuntimeCommand::PlanningYield);
	PlayerBotHuntPlanningObservation cancelBoundedTransport;
	cancelBoundedTransport.cancelAtScoreBarrier = true;
	assert(boundedTransport.advancePlanning(boundedTransportInput, now, cancelBoundedTransport).command ==
	       PlayerBotHuntRuntimeCommand::PlanningCancelled);

	// A newly reached transport destination becomes available to later offers on
	// the next pass, preserving affordable multihop labels without one synchronous
	// all-NPC closure.
	PlayerBotHuntRuntime transportRuntime({});
	PlayerBotHuntRuntimePlanningInput transportInput;
	transportInput.cacheRevision = 1;
	transportInput.player.level = 20;
	transportInput.player.funds = 100;
	transportInput.start.emplace();
	transportInput.start->scan.revision = 1;
	transportInput.start->scan.candidateCount = 1;
	transportInput.start->scan.candidateIndices = {0};
	transportInput.start->originReachability = std::make_shared<PlayerBotTopologyReachability>();
	transportInput.start->transportOffers = std::make_shared<std::vector<PlayerBotHuntTransportOffer>>(
	    std::initializer_list<PlayerBotHuntTransportOffer>{{Position(1, 1, 7), Position(2, 2, 7), 10},
	                                                      {Position(2, 2, 7), Position(3, 3, 7), 20}});
	assert(transportRuntime.advancePlanning(transportInput, now).command == PlayerBotHuntRuntimeCommand::PlanningStarted);
	auto transportTurn = transportRuntime.advancePlanning(transportInput, now);
	assert(transportTurn.transportWork.size() == 2 && transportTurn.transportWork[0].arrivals &&
	       transportTurn.transportWork[0].arrivals->size() == 1);
	auto reachability = std::make_shared<PlayerBotTopologyReachability>();
	std::vector<PlayerBotHuntRuntimeTransportObservation> transportObservations = {
	    {0, PlayerBotHuntTransportArrival{Position(2, 2, 7), 10, 1, reachability}}, {1, std::nullopt}};
	assert(transportRuntime.completeTransportWork(transportObservations).command == PlayerBotHuntRuntimeCommand::PlanningYield);
	transportTurn = transportRuntime.advancePlanning(transportInput, now);
	assert(transportTurn.transportWork.size() == 2 && transportTurn.transportWork[1].arrivals &&
	       transportTurn.transportWork[1].arrivals->size() == 2);
	PlayerBotHuntPlanningObservation cancelTransport;
	cancelTransport.cancelAtScoreBarrier = true;
	transportRuntime.completeTransportWork({{0, std::nullopt}, {1, std::nullopt}});
	assert(transportRuntime.advancePlanning(transportInput, now, cancelTransport).command ==
	       PlayerBotHuntRuntimeCommand::PlanningCancelled);
}

void lootArithmeticMemo()
{
	PlayerBotLootCountMemo memo;
	for (int repeat = 0; repeat < 1000; ++repeat) {
		assert(memo.expectedCount(3, 10, true, 1) == 6.0 / 100001);
		assert(memo.expectedCount(4, 10, true, 1) == 10.0 / 100001);
		assert(memo.expectedCount(3, 2, true, 1) == 4.0 / 100001);
		assert(memo.expectedCount(3, 10, false, 1) == 3.0 / 100001);
		assert(memo.expectedCount(3, 10, true, 2) == 12.0 / 100001);
	}
	assert(memo.arithmeticEvaluations() == 5); // each exact tuple is enumerated once, not once per monster
	PlayerBotLootCountMemo nextBuild;
	assert(nextBuild.expectedCount(3, 10, true, 2) == 12.0 / 100001);
	assert(nextBuild.arithmeticEvaluations() == 1); // a new build owns a fresh memo
}

void modeledPatrolFailure()
{
	const std::vector<Position> points = {
	    Position(100, 100, 7), Position(130, 100, 7), Position(130, 130, 7), Position(100, 130, 7)};
	const auto now = std::chrono::steady_clock::time_point{};
	PlayerBotHuntRuntimePlayerObservation player;
	player.maximumHealth = player.health = 100;
	PlayerBotNavigationRuntimeOutcome failure;
	failure.stepFailureCount = 3;
	PlayerBotHuntRegion modeled;
	modeled.atlasVariantId = 42;
	modeled.patrolPoints = points;
	modeled.destination = points.front();
	modeled.viability.reachableSpawns = 4;
	PlayerBotHuntRuntime runtime(points);
	runtime.selectPlanningRegion(modeled, player, now);
	const auto result = runtime.observePatrolNavigation(failure, now, 3, 3);
	assert(result.command == PlayerBotHuntPatrolCommand::RegionExhausted);
	assert(result.cooldown && result.cooldown->variantId == 42 && result.cooldown->duration == std::chrono::minutes(10));
	assert(runtime.region()->patrolPoints == points); // never silently reuse altered geometry
	assert(runtime.complete(player, now + std::chrono::seconds(1), 1500));
	assert(!runtime.active() && runtime.planningStartRequired(now));
	// No modeled geometry: retain synthetic waypoint skipping and empty-patrol exhaustion.
	modeled.viability = {};
	runtime.selectPlanningRegion(modeled, player, now);
	assert(runtime.observePatrolNavigation(failure, now, 3, 3).command == PlayerBotHuntPatrolCommand::SkipWaypoint);
	assert(runtime.region()->patrolPoints.size() == 3);
	for (int i = 0; i < 2; ++i) {
		assert(runtime.observePatrolNavigation(failure, now, 3, 3).command == PlayerBotHuntPatrolCommand::SkipWaypoint);
	}
	assert(runtime.observePatrolNavigation(failure, now, 3, 3).command == PlayerBotHuntPatrolCommand::RegionExhausted);
	PlayerBotHuntRuntime fallback(points);
	const auto skipped = fallback.observePatrolNavigation(failure, now, 3, 3);
	assert(skipped.command == PlayerBotHuntPatrolCommand::SkipWaypoint && !skipped.cooldown);
	assert(fallback.patrolTarget().destination == points[1]);

	// A rejected NPC-fare preflight is a route attempt. Three deterministic
	// failures exhaust a modeled patrol instead of retrying until the deadline.
	modeled.viability.reachableSpawns = 4;
	PlayerBotHuntRuntime fareFailures(points);
	fareFailures.selectPlanningRegion(modeled, player, now);
	PlayerBotNavigationPlanMetrics rejectedFarePlan;
	rejectedFarePlan.result = PlayerBotNavigationResult::Reached;
	rejectedFarePlan.fare = 50;
	const PlayerBotNavigationRuntimeOutcome rejectedFare =
	    playerBotHuntRejectedPatrolPreflight(rejectedFarePlan, false);
	assert(rejectedFare.plan.attempted && rejectedFare.routeUnavailable && !rejectedFare.routeUnsafe);
	assert(fareFailures.observePatrolNavigation(rejectedFare, now, 3, 3).command ==
	       PlayerBotHuntPatrolCommand::Continue);
	assert(fareFailures.observePatrolNavigation(rejectedFare, now, 3, 3).command ==
	       PlayerBotHuntPatrolCommand::Continue);
	const auto exhaustedFare = fareFailures.observePatrolNavigation(rejectedFare, now, 3, 3);
	assert(exhaustedFare.command == PlayerBotHuntPatrolCommand::RegionExhausted);
	assert(exhaustedFare.routeFailures == 3);
}

void mutableShovelPassages()
{
	for (const auto& [closedItemId, openItemId] : std::array<std::pair<uint16_t, uint16_t>, 4>{
	         std::pair<uint16_t, uint16_t>{468, 469}, {481, 482}, {483, 484}, {7932, 7933}}) {
		const auto closed = playerBotShovelPassage(closedItemId);
		const auto open = playerBotShovelPassage(openItemId);
		assert(closed && open && closed->closedItemId == closedItemId && closed->openItemId == openItemId);
		assert(open->closedItemId == closed->closedItemId && open->openItemId == closed->openItemId);
		assert(playerBotResolveShovelPassageAction(closedItemId, openItemId, false) ==
		       PlayerBotNavigationAction::Move);
		assert(playerBotResolveShovelPassageAction(openItemId, closedItemId, true) ==
		       PlayerBotNavigationAction::UseShovel);
		assert(!playerBotResolveShovelPassageAction(openItemId, closedItemId, false));
	}
	// A planned passage follows its own identity, not an arbitrary hole-like tile.
	assert(!playerBotShovelPassage(470));
	assert(!playerBotResolveShovelPassageAction(468, 482, true));
	assert(!playerBotResolveShovelPassageAction(468, 470, true));
}

void navigationFailureAccounting()
{
	// Only a completed local proof of unreachability can offer paid travel.
	// An exhausted search is incomplete, even where a topology route exists.
	assert(playerBotNavigationMayFallbackToNpcTravel(PlayerBotNavigationResult::Unreachable));
	assert(!playerBotNavigationMayFallbackToNpcTravel(PlayerBotNavigationResult::NodeLimit));
	PlayerBotNavigationGoal blockedGoal;
	blockedGoal.type = PlayerBotNavigationGoalType::Exact;
	blockedGoal.position = Position(100, 100, 7);
	assert(playerBotNavigationExactGoalBlocked(blockedGoal, {Position(100, 100, 7)}));
	assert(!playerBotNavigationExactGoalBlocked(blockedGoal, {Position(101, 100, 7)}));

	PlayerBotFixedTargetFailureTracker failures;
	assert(!failures.observePosition(false, 20));
	for (int attempt = 0; attempt < 20; ++attempt) {
		failures.observePlan(true);
		failures.observeBlockedPlan(); // the dispatched route is immediately blocked
		failures.observePlan(false); // the same cycle's unavailable replan is not double-counted
		assert(failures.count() == static_cast<uint32_t>(attempt + 1));
	}
	assert(failures.exhausted());
	assert(failures.observePosition(true, 19));
	assert(failures.count() == 0 && !failures.exhausted());
	failures.observePlan(false);
	failures.observePosition(false, 200);
	assert(failures.count() == 0); // a different fixed goal starts a new sequence

	PlayerBotFixedTargetFailureTracker rejectedAcceptedPlans;
	assert(!rejectedAcceptedPlans.observePosition(false, 20));
	for (int attempt = 0; attempt < 20; ++attempt) {
		rejectedAcceptedPlans.observePlan(true); // a route was initially accepted
		rejectedAcceptedPlans.observeRejectedAcceptedPlan(); // fare/risk rejects it before execution
		assert(rejectedAcceptedPlans.count() == static_cast<uint32_t>(attempt + 1));
	}
	assert(rejectedAcceptedPlans.exhausted());
}

void transitCombat()
{
	using Phase = PlayerBotCyclePhase;
	using Stage = PlayerBotScenarioStage;
	assert(PlayerBotTransitCombat::required(Phase::ReturnToDepot, Stage::Traverse, true, false));
	assert(PlayerBotTransitCombat::required(Phase::Hunt, Stage::Traverse, false, false));
	assert(PlayerBotTransitCombat::required(Phase::Hunt, Stage::LootCorpse, true, false));
	assert(PlayerBotTransitCombat::required(Phase::Hunt, Stage::Traverse, true, true));
	assert(!PlayerBotTransitCombat::required(Phase::Hunt, Stage::Traverse, true, false));
	assert(PlayerBotTransitCombat::lootDeadlineRequiresRelease(true, true));
	assert(!PlayerBotTransitCombat::lootDeadlineRequiresRelease(true, false));
	assert(!PlayerBotTransitCombat::lootDeadlineRequiresRelease(false, true));

	const Position stalled(100, 100, 7);
	const Position intended(101, 100, 7);
	PlayerBotTransitCombat episode;
	assert(episode.observe(true, 1, Phase::Service));
	assert(!episode.movementFallbackRequired()); // planning and legitimate waits do not create evidence
	episode.observePosition(Position(101, 100, 7));
	assert(!episode.movementFallbackRequired()); // ordinary movement does not create evidence either
	assert(!episode.allowsDefense(42, true));
	episode.observeMovementFailure(stalled); // failed NPC/depot planning has no adjacent waypoint
	assert(episode.movementFallbackRequired() && !episode.intendedStep());
	assert(episode.allowsDefense(42, true));
	episode.beginDefense(42, false);
	// No five- or sixty-second timer can cycle a live passage target.
	assert(episode.retainsDefense(42, stalled, intended, true));
	assert(episode.retainsDefense(42, stalled, intended, true));
	assert(!playerBotDefensiveCombatTimeoutApplies(true, true));
	assert(playerBotDefensiveCombatTimeoutApplies(false, true));
	PlayerBotDefensiveTarget runtimeTarget;
	runtimeTarget.id = 42;
	runtimeTarget.position = intended;
	runtimeTarget.name = "Frost Troll";
	runtimeTarget.routeCritical = true;
	PlayerBotCombatTargetSnapshot liveTarget;
	liveTarget.present = true;
	assert(!playerBotDefensiveLifetimeCompletion(runtimeTarget, liveTarget));
	liveTarget.dead = true;
	const auto deadCompletion = playerBotDefensiveLifetimeCompletion(runtimeTarget, liveTarget);
	assert(deadCompletion && deadCompletion->command == PlayerBotCombatCommand::CompleteDefensiveCombat);
	assert(std::string(deadCompletion->result) == "success" && std::string(deadCompletion->reason) == "target_defeated");
	liveTarget.dead = false;
	liveTarget.removed = true;
	assert(std::string(playerBotDefensiveLifetimeCompletion(runtimeTarget, liveTarget)->reason) == "target_defeated");
	assert(!episode.allowsDefense(43, true));
	assert(!episode.retainsDefense(42, stalled, intended, false)); // live safety release
	episode.clearFallback();
	assert(!episode.movementFallbackRequired() && !episode.allowsDefense(42, true));

	episode.observeMovementFailure(stalled, intended);
	episode.beginDefense(42, true);
	assert(episode.retainsDefense(42, stalled, intended, true));
	assert(!episode.retainsDefense(42, stalled, Position(100, 101, 7), true)); // intended blocker moved
	episode.clearFallback();
	episode.observeMovementFailure(stalled, intended);
	episode.beginDefense(43, false);
	assert(episode.retainsDefense(43, stalled, Position(100, 101, 7), true)); // alternate target is retained
	assert(!episode.allowsDefense(42, true));
	episode.clearFallback();
	episode.observeMovementFailure(stalled);
	episode.observePosition(Position(100, 101, 7));
	assert(!episode.movementFallbackRequired()); // actual progress clears the stall
	episode.observeMovementFailure(stalled, intended);
	episode.observeViableMovement();
	assert(!episode.movementFallbackRequired()); // a usable first step resumes navigation
	episode.observeMovementFailure(stalled);
	episode.beginDefense(42, false);
	episode.clearFallback();
	assert(!episode.movementFallbackRequired()); // target death completes the fallback

	// Goal abandonment is an explicit interruption; replans within a goal are not.
	episode.observeMovementFailure(stalled);
	assert(!episode.observe(true, 1, Phase::Service));
	assert(episode.movementFallbackRequired());
	assert(episode.observe(true, 2, Phase::ReturnToDepot));
	assert(!episode.movementFallbackRequired());

	// The same fallback contract also applies to active hunt patrol movement.
	assert(episode.observe(false, 3, Phase::Hunt));
	episode.observeMovementFailure(stalled, intended);
	assert(episode.movementFallbackRequired() && episode.allowsDefense(42, true));
	assert(episode.allowsDefense(42, false)); // ordinary hunt defense remains available

	// Four attackers are accepted when their real aggregate estimate is survivable.
	assert(playerBotPassageFightManageable(500, 0, 20.0, 20.0));
	assert(!playerBotPassageFightManageable(400, 0, 20.0, 20.0));
	assert(playerBotPassageFightManageable(350, 100, 20.0, 20.0));

	PlayerBotDefensiveTarget easy;
	easy.id = 10;
	easy.position = Position(99, 100, 7);
	easy.routeCritical = true;
	easy.predictedFightDamage = 100;
	easy.predictedFightSeconds = 5;
	PlayerBotDefensiveTarget intendedAttacker = easy;
	intendedAttacker.id = 20;
	intendedAttacker.position = intended;
	intendedAttacker.intendedStep = true;
	intendedAttacker.predictedFightDamage = 200;
	assert(playerBotPreferDefensiveTarget(intendedAttacker, easy, stalled));
	intendedAttacker.intendedStep = false;
	assert(playerBotPreferDefensiveTarget(easy, intendedAttacker, stalled));

	episode.finish();
	assert(!episode.active() && !episode.movementFallbackRequired());

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

void adaptiveChallenge()
{
	auto observe = [](PlayerBotHuntPolicy& policy, double seconds, uint32_t kills,
	                  int32_t health, uint32_t mana, uint32_t potions, uint32_t spells) {
		policy.resetCombatEvidence();
		policy.observeCombat({true, seconds, health, 100, mana, 100, 1});
		for (uint32_t index = 0; index < kills; ++index) policy.observeKill();
		for (uint32_t index = 0; index < potions; ++index) policy.observeRecovery(true);
		for (uint32_t index = 0; index < spells; ++index) policy.observeRecovery(false);
		return policy.updateChallengeFrontier({300, 100});
	};

	PlayerBotHuntPolicy exura;
	auto update = observe(exura, 120, 4, 90, 80, 0, 1);
	assert(update.result == PlayerBotHuntChallengeResult::Escalated);
	assert(std::abs(update.frontierAfter - 0.30) < 1e-12);
	assert(update.combat.p10ManaPercent == 80);

	PlayerBotHuntPolicy routinePotion;
	update = observe(routinePotion, 120, 4, 90, 80, 1, 0);
	assert(update.result == PlayerBotHuntChallengeResult::Escalated);
	assert(std::abs(update.frontierAfter - 0.30) < 1e-12);
	PlayerBotHuntPolicy twoPotions;
	assert(observe(twoPotions, 180, 5, 80, 80, 2, 0).result == PlayerBotHuntChallengeResult::Escalated);

	PlayerBotHuntPolicy insufficient;
	update = observe(insufficient, 30, 1, 100, 100, 0, 0);
	assert(update.result == PlayerBotHuntChallengeResult::InsufficientActiveCombat);
	assert(update.frontierAfter == update.frontierBefore);

	PlayerBotHuntPolicy routineLongHunt;
	update = observe(routineLongHunt, 300, 8, 80, 80, 3, 0);
	assert(update.result == PlayerBotHuntChallengeResult::Hold);
	assert(update.frontierAfter == update.frontierBefore);

	PlayerBotHuntPolicy heavyPotions;
	update = observe(heavyPotions, 60, 3, 75, 80, 3, 0);
	assert(update.result == PlayerBotHuntChallengeResult::Backoff);
	assert(std::abs(update.frontierAfter - 0.10) < 1e-12);
	PlayerBotHuntPolicy manaPressure;
	assert(observe(manaPressure, 90, 3, 80, 10, 0, 1).result == PlayerBotHuntChallengeResult::Backoff);
	PlayerBotHuntPolicy criticalHealth;
	assert(observe(criticalHealth, 90, 3, 30, 80, 0, 0).result == PlayerBotHuntChallengeResult::Backoff);
	PlayerBotHuntPolicy danger;
	danger.observeCombat({true, 90, 80, 100, 80, 100, 2});
	for (uint32_t index = 0; index < 3; ++index) danger.observeKill();
	danger.observeDamage(100);
	assert(danger.observeDanger(100, std::chrono::seconds(30)));
	assert(danger.updateChallengeFrontier({300, 100}).result == PlayerBotHuntChallengeResult::Backoff);
	PlayerBotHuntPolicy death;
	death.observeDeath();
	assert(death.updateChallengeFrontier({300, 100}).result == PlayerBotHuntChallengeResult::Backoff);

	PlayerBotHuntPolicy bounded;
	for (uint32_t index = 0; index < 10; ++index) observe(bounded, 120, 4, 90, 90, 0, 0);
	assert(std::abs(bounded.challengeFrontier() - 0.60) < 1e-12);
}

void supplyCalibration()
{
	auto capability = [](uint32_t level = 10) {
		PlayerBotHuntPlanningProfile profile;
		profile.combat.level = level;
		profile.combat.maximumHealth = 100 + static_cast<int32_t>(level - 10) * 5;
		profile.combat.armor = 10;
		profile.combat.defense = 20 + static_cast<int32_t>(level - 10);
		profile.combat.attack = 15;
		profile.combat.attackSkill = 30 + static_cast<int32_t>(level - 10);
		profile.magicLevel = level - 10;
		profile.supply.maximumMana = 100 + (level - 10) * 5;
		profile.supply.potionHealing = 125;
		profile.equipmentItemIds[5] = 2383;
		return playerBotSupplyCapability(profile);
	};
	auto regionFor = [&](uint64_t variant, PlayerBotSupplyCapabilitySnapshot snapshot) {
		PlayerBotHuntRegion region;
		region.atlasVariantId = variant;
		region.atlasRevision = 2;
		region.supplyCapability = snapshot;
		region.supplyRecovery = true;
		region.currentHealth = region.maximumHealth = 100;
		region.supplyProfile.potionHealing = 125;
		region.supplyProfile.maximumMana = region.supplyProfile.mana = 100;
		region.combatFraction = 0.5;
		region.availableHuntSeconds = 120;
		region.supplyBudget.expectedPotions = 1;
		region.supplyStaticPotionsPerCombatSecond = 1.0 / 60.0;
		return region;
	};
	PlayerBotHuntPolicy policy;
	PlayerBotHuntRegion region = regionFor(1, capability());
	auto observe = [&](uint32_t potionUses, int32_t health, uint32_t mana, bool interrupted = false) {
		policy.resetCombatEvidence();
		policy.observeCombat({true, 60, health, 100, mana, 100, 1});
		for (unsigned i = 0; i < 3; ++i) policy.observeKill();
		for (unsigned i = 0; i < potionUses; ++i) policy.observeRecovery(true);
		return policy.observeSupplies(region, region.supplyCapability, 120,
		                              health, 100, mana, 0, interrupted);
	};
	const auto healthDebt = observe(0, 90, 100);
	assert(healthDebt.accepted && healthDebt.levelAdjustedHealthDebt == 10);
	assert(healthDebt.potionEquivalentDemand > 0);
	assert(!observe(0, 100, 100, true).accepted);
	assert(std::string(observe(0, 100, 100, true).reason) == "interrupted_outing");
	const auto firstSafe = observe(0, 100, 100);
	assert(firstSafe.calibration.potionsPerCombatSecond > 0);
	observe(0, 100, 100);
	region.supplyCalibration = observe(0, 100, 100).calibration;
	// Zero-demand evidence decays at the local 0.20 blend; it never snaps to zero.
	assert(region.supplyCalibration.potionsPerCombatSecond > 0);
	assert(region.supplyCalibration.samples == 4);

	// Guarded zero evidence does not cross an incompatible stored capability.
	PlayerBotHuntPolicy contextPolicy;
	PlayerBotHuntRegion contextA = regionFor(20, capability());
	auto safeContextOuting = [&](const PlayerBotHuntRegion& context) {
		contextPolicy.resetCombatEvidence();
		contextPolicy.observeCombat({true, 60, 100, 100, 100, 100, 1});
		for (unsigned i = 0; i < 3; ++i) contextPolicy.observeKill();
		return contextPolicy.observeSupplies(
		    context, context.supplyCapability, 120, 100, 100, 100, 0, false);
	};
	assert(safeContextOuting(contextA).accepted);
	assert(safeContextOuting(contextA).accepted);
	PlayerBotHuntRegion contextB = contextA;
	contextB.supplyCapability.equipmentItemIds[5] = 2395;
	const auto firstNewContext = safeContextOuting(contextB);
	assert(firstNewContext.accepted && firstNewContext.calibration.potionsPerCombatSecond > 0);
	assert(contextPolicy.regionPerformance().at(contextB.atlasVariantId).supply.samples == 1);

	// A final spell and its mana debt are ordinary accounted demand, not an
	// endpoint veto.
	PlayerBotHuntPolicy manaDebtPolicy;
	PlayerBotHuntRegion manaDebtRegion = regionFor(2, capability());
	manaDebtRegion.supplyProfile.spellLegal = true;
	manaDebtRegion.supplyProfile.spellMana = 10;
	manaDebtRegion.supplyProfile.spellHealing = 20;
	manaDebtRegion.supplyProfile.spellInterval = 1;
	manaDebtPolicy.observeCombat({true, 60, 100, 100, 80, 100, 1});
	manaDebtPolicy.observeRecovery(false);
	for (unsigned i = 0; i < 3; ++i) manaDebtPolicy.observeKill();
	const auto manaDebt = manaDebtPolicy.observeSupplies(
	    manaDebtRegion, manaDebtRegion.supplyCapability, 120, 100, 100, 80, 0, false);
	assert(manaDebt.accepted && manaDebt.levelAdjustedManaDebt == 20);
	assert(manaDebt.potionEquivalentDemand > 0);

	// Level restoration is netted before clipping, so only resources spent
	// during the outing become debt.
	auto levelDebt = [&](int32_t startHealth, uint32_t startMana, uint32_t restoredHealth,
	                     uint32_t restoredMana, int32_t endHealth, uint32_t endMana) {
		PlayerBotHuntPolicy debtPolicy;
		PlayerBotHuntRegion debtRegion = regionFor(21, capability());
		debtRegion.currentHealth = startHealth;
		debtRegion.supplyProfile.mana = startMana;
		debtRegion.supplyProfile.maximumMana = 200;
		debtRegion.supplyProfile.spellLegal = true;
		debtRegion.supplyProfile.spellMana = 10;
		debtRegion.supplyProfile.spellHealing = 20;
		debtPolicy.observeCombat({true, 60, endHealth, 200, endMana, 200, 1});
		debtPolicy.observeRecovery(false);
		debtPolicy.observeLevelRestoration(restoredHealth, restoredMana);
		for (unsigned i = 0; i < 3; ++i) debtPolicy.observeKill();
		return debtPolicy.observeSupplies(
		    debtRegion, debtRegion.supplyCapability, 120, endHealth, 200, endMana, 0, false);
	};
	const auto fullAdvance = levelDebt(185, 90, 15, 30, 200, 120);
	assert(fullAdvance.levelAdjustedHealthDebt == 0 && fullAdvance.levelAdjustedManaDebt == 0);
	const auto depletedAdvance = levelDebt(100, 50, 100, 150, 200, 200);
	assert(depletedAdvance.levelAdjustedHealthDebt == 0 && depletedAdvance.levelAdjustedManaDebt == 0);
	const auto restoredDamage = levelDebt(185, 100, 55, 40, 200, 120);
	assert(restoredDamage.levelAdjustedHealthDebt == 40 && restoredDamage.levelAdjustedManaDebt == 20);
	const auto postAdvanceDamage = levelDebt(185, 100, 55, 40, 180, 100);
	assert(postAdvanceDamage.levelAdjustedHealthDebt == 60 && postAdvanceDamage.levelAdjustedManaDebt == 40);
	PlayerBotHuntPolicy multipleAdvancePolicy;
	PlayerBotHuntRegion multipleAdvance = regionFor(22, capability());
	multipleAdvance.currentHealth = 185;
	multipleAdvancePolicy.observeCombat({true, 60, 200, 210, 100, 100, 1});
	multipleAdvancePolicy.observeLevelRestoration(10, 0);
	multipleAdvancePolicy.observeLevelRestoration(15, 0);
	for (unsigned i = 0; i < 3; ++i) multipleAdvancePolicy.observeKill();
	const auto multipleAdvances = multipleAdvancePolicy.observeSupplies(
	    multipleAdvance, multipleAdvance.supplyCapability, 120, 200, 210, 100, 0, false);
	assert(multipleAdvances.levelHealthRestored == 25);
	assert(multipleAdvances.levelAdjustedHealthDebt == 10);

	// A partially fed hunt may contain damage, a healing spell, and one level-up.
	// Food exposure remains telemetry but does not partition supply learning.
	PlayerBotHuntPolicy progression;
	PlayerBotHuntRegion progressing = regionFor(3, capability(10));
	progressing.supplyCapability.foodAvailable = true;
	progressing.supplyCapability.foodHealthGain = 1;
	progressing.supplyCapability.foodHealthIntervalMilliseconds = 6000;
	progressing.supplyCapability.foodManaGain = 2;
	progressing.supplyCapability.foodManaIntervalMilliseconds = 6000;
	progressing.supplyProfile.healthGain = 1;
	progressing.supplyProfile.healthInterval = 6;
	progressing.supplyProfile.manaGain = 2;
	progressing.supplyProfile.manaInterval = 6;
	progressing.supplyProfile.spellLegal = true;
	progressing.supplyProfile.spellMana = 10;
	progressing.supplyProfile.spellHealing = 20;
	progression.observeCombat({true, 30, 90, 105, 85, 105, 1, true, true});
	progression.observeCombat({true, 30, 90, 105, 85, 105, 1, false, false});
	progression.observeDamage(30);
	progression.observeRecovery(false);
	progression.observeLevelRestoration(30, 20);
	for (unsigned i = 0; i < 3; ++i) progression.observeKill();
	auto leveledWithoutFood = capability(11);
	leveledWithoutFood.foodHealthGain = 1;
	leveledWithoutFood.foodHealthIntervalMilliseconds = 6000;
	leveledWithoutFood.foodManaGain = 2;
	leveledWithoutFood.foodManaIntervalMilliseconds = 6000;
	const auto leveled = progression.observeSupplies(
	    progressing, leveledWithoutFood, 120, 105, 105, 105, 0, false);
	assert(leveled.accepted && std::string(leveled.reason) == "level_restoration_accounted");
	assert(leveled.levelHealthRestored == 30 && leveled.levelManaRestored == 20);
	assert(leveled.foodActiveSeconds == 30 && leveled.foodAvailableSeconds == 30);
	assert(leveled.potionEquivalentDemand > 0);
	auto leveledWithFood = leveledWithoutFood;
	leveledWithFood.foodAvailable = true;
	assert(leveled.calibration.capability == leveledWithFood);
	assert(playerBotSupplyCalibrationForCapability(leveled.calibration, leveledWithFood));
	assert(playerBotSupplyCalibrationForCapability(leveled.calibration, leveledWithoutFood));
	progressing.supplyCalibration = leveled.calibration;
	progressing.supplyCapability = leveledWithoutFood;
	progressing.reconcileSupplies(1);
	assert(std::string(progressing.supplyEstimateSource) == "local");
	assert(progressing.supplyLocalRejectionReason == nullptr);

	// Equipment remains a hard boundary; food-only differences do not.
	PlayerBotHuntPolicy boundaryPolicy;
	PlayerBotHuntRegion boundary = regionFor(4, capability());
	boundaryPolicy.observeCombat({true, 60, 100, 100, 100, 100, 1, true, true});
	for (unsigned i = 0; i < 3; ++i) boundaryPolicy.observeKill();
	auto changedEquipment = boundary.supplyCapability;
	changedEquipment.equipmentItemIds[5] = 2395;
	const auto material = boundaryPolicy.observeSupplies(
	    boundary, changedEquipment, 120, 100, 100, 100, 0, false);
	assert(!material.accepted && std::string(material.reason) == "material_capability_change");
	auto changedFood = boundary.supplyCapability;
	changedFood.foodHealthGain = 2;
	changedFood.foodManaGain = 3;
	assert(changedFood == boundary.supplyCapability);
	const auto foodChange = boundaryPolicy.observeSupplies(
	    boundary, changedFood, 120, 100, 100, 100, 0, false);
	assert(foodChange.accepted && std::string(foodChange.reason) == "safe_combat_evidence");
	assert(foodChange.foodActiveSeconds == 60 && foodChange.foodAvailableSeconds == 60);
	assert(foodChange.changedFields == 0);
	assert(foodChange.direction == PlayerBotSupplyCapabilityDirection::Unchanged);

	// Transient live defense never changes a supply capability when the stable
	// equipment-derived defense and fight mode are unchanged.
	PlayerBotHuntPlanningProfile transientA;
	transientA.combat.defense = 10;
	transientA.combat.attackFactor = 1.2f;
	transientA.supplyCapabilityDefense = 24;
	PlayerBotHuntPlanningProfile transientB = transientA;
	transientB.combat.defense = 20;
	assert(playerBotSupplyCapability(transientA) == playerBotSupplyCapability(transientB));
	transientB.combat.attackFactor = 1.0f;
	assert(playerBotSupplyCapability(transientA) != playerBotSupplyCapability(transientB));

	// Failed exposure guards identify both the measured and required values.
	PlayerBotHuntPolicy guards;
	PlayerBotHuntRegion guarded = regionFor(5, capability());
	guards.observeCombat({true, 60, 100, 100, 100, 100, 1});
	for (unsigned i = 0; i < 3; ++i) guards.observeKill();
	const auto shortDuration = guards.observeSupplies(
	    guarded, guarded.supplyCapability, 119, 100, 100, 100, 0, false);
	assert(!shortDuration.accepted && std::string(shortDuration.reason) == "insufficient_duration");
	assert(shortDuration.durationSeconds == 119 && shortDuration.minimumDurationSeconds == 120);
	guards.resetCombatEvidence();
	guards.observeCombat({true, 59, 100, 100, 100, 100, 1});
	for (unsigned i = 0; i < 3; ++i) guards.observeKill();
	const auto shortCombat = guards.observeSupplies(
	    guarded, guarded.supplyCapability, 120, 100, 100, 100, 0, false);
	assert(!shortCombat.accepted && std::string(shortCombat.reason) == "insufficient_active_combat");
	assert(shortCombat.activeCombatSeconds == 59 && shortCombat.minimumActiveCombatSeconds == 60);
	guards.resetCombatEvidence();
	guards.observeCombat({true, 60, 100, 100, 100, 100, 1});
	for (unsigned i = 0; i < 2; ++i) guards.observeKill();
	const auto fewKills = guards.observeSupplies(
	    guarded, guarded.supplyCapability, 120, 100, 100, 100, 0, false);
	assert(!fewKills.accepted && std::string(fewKills.reason) == "insufficient_kills");
	assert(fewKills.kills == 2 && fewKills.minimumKills == 3);
	guards.resetCombatEvidence();
	guards.observeCombat({true, 60, 30, 100, 100, 100, 1});
	for (unsigned i = 0; i < 3; ++i) guards.observeKill();
	const auto thresholdHealth = guards.observeSupplies(
	    guarded, guarded.supplyCapability, 120, 100, 100, 100, 0, false);
	assert(thresholdHealth.accepted && thresholdHealth.p10HealthPercent == 30);
	assert(thresholdHealth.minimumHealthPercent == 30);
	PlayerBotHuntPolicy belowHealth;
	belowHealth.observeCombat({true, 60, 29, 100, 100, 100, 1});
	for (unsigned i = 0; i < 3; ++i) belowHealth.observeKill();
	const auto healthPressure = belowHealth.observeSupplies(
	    guarded, guarded.supplyCapability, 120, 100, 100, 100, 0, false);
	assert(!healthPressure.accepted && std::string(healthPressure.reason) == "health_pressure");
	assert(healthPressure.p10HealthPercent == 29 && healthPressure.minimumHealthPercent == 30);

	// Exhausted mana is valid evidence. Its unrecovered debt remains part of demand.
	guards.resetCombatEvidence();
	guarded.supplyProfile.spellLegal = true;
	guarded.supplyProfile.spellMana = 10;
	guarded.supplyProfile.spellHealing = 20;
	guards.observeCombat({true, 60, 100, 100, 0, 100, 1});
	guards.observeRecovery(false);
	for (unsigned i = 0; i < 3; ++i) guards.observeKill();
	const auto manaDebtAccepted = guards.observeSupplies(
	    guarded, guarded.supplyCapability, 120, 100, 100, 0, 0, false);
	assert(manaDebtAccepted.accepted && manaDebtAccepted.p10ManaPercent == 0);
	assert(manaDebtAccepted.levelAdjustedManaDebt == 100);
	assert(manaDebtAccepted.potionEquivalentDemand > 0);

	// Unsafe short evidence cannot lower a high prior, but can raise a low prior
	// immediately. Neither path contributes a guarded cheap sample.
	PlayerBotHuntPolicy unsafePolicy;
	PlayerBotHuntRegion unsafeRegion = regionFor(6, capability());
	unsafeRegion.supplyBudget.expectedPotions = 10;
	unsafeRegion.supplyStaticPotionsPerCombatSecond = 10.0 / 60.0;
	unsafePolicy.observeCombat({true, 10, 20, 100, 100, 100, 1});
	const auto unsafeRejected = unsafePolicy.observeSupplies(
	    unsafeRegion, unsafeRegion.supplyCapability, 20, 20, 100, 100, 0, false);
	assert(!unsafeRejected.accepted && std::string(unsafeRejected.reason) == "health_pressure");
	assert(unsafeRejected.p10HealthPercent == 20 && unsafeRejected.minimumHealthPercent == 30);
	unsafePolicy.resetCombatEvidence();
	unsafeRegion.supplyBudget.expectedPotions = 0;
	unsafeRegion.supplyStaticPotionsPerCombatSecond = 0;
	unsafePolicy.observeCombat({true, 10, 40, 100, 100, 100, 1});
	unsafePolicy.observeDeath();
	const auto unsafe = unsafePolicy.observeSupplies(
	    unsafeRegion, unsafeRegion.supplyCapability, 20, 40, 100, 100, 0, false);
	assert(unsafe.accepted && std::string(unsafe.reason) == "unsafe_upward_correction");
	assert(unsafe.localEstimateDirection == PlayerBotSupplyEstimateDirection::Upward);

	PlayerBotHuntRuntime runtime({});
	PlayerBotHuntRuntimePlayerObservation player;
	region = regionFor(8, capability());
	region.expectedDamagePerSecond = 1;
	player.health = player.maximumHealth = player.mana = 100;
	player.supplyCapability = capability(11);
	const auto start = std::chrono::steady_clock::time_point{};
	runtime.selectPlanningRegion(region, player, start);
	runtime.enterHuntArea(player, region.supplyProfile, start);
	runtime.sampleCombat({true, start + std::chrono::seconds(1), 100, 100, 100, 100, 1});
	runtime.sampleCombat({true, start + std::chrono::seconds(61), 100, 100, 100, 100, 1});
	for (unsigned i = 0; i < 3; ++i) runtime.observeKill();
	const auto completed = runtime.complete(player, start + std::chrono::seconds(120), 2400);
	assert(completed && completed->supplyObservation.accepted);
	assert(completed->supplyObservation.arrivalBaselineObserved);
	assert(completed->region.supplyCalibration.samples == 1);
	assert(runtime.regionPerformance().at(region.atlasVariantId).supply.samples == 1);
	assert(runtime.supplyGlobalLearning().samples == 1);
	assert(runtime.planningProfile({}).supplyGlobalLearning.samples == 1);
	assert(!runtime.complete(player, start + std::chrono::seconds(121), 2400));

	// Outbound events are outside the supply frame. Arrival captures the swapped
	// equipment context once; a duplicate waypoint arrival cannot reset evidence.
	PlayerBotHuntRuntime arrivalRuntime({});
	PlayerBotHuntRegion arrivalRegion = regionFor(23, capability(10));
	arrivalRegion.expectedDamagePerSecond = 1;
	arrivalRegion.supplyStaticPotionsPerCombatSecond = 10.0 / 60.0;
	arrivalRegion.supplyGlobalLearning = {0.9, 1};
	PlayerBotHuntRuntimePlayerObservation planningPlayer;
	planningPlayer.health = planningPlayer.maximumHealth = planningPlayer.mana = 100;
	planningPlayer.supplyCapability = arrivalRegion.supplyCapability;
	arrivalRuntime.selectPlanningRegion(arrivalRegion, planningPlayer, start);
	arrivalRuntime.observeDamage(80);
	arrivalRuntime.observeLevelRestoration(50, 50);
	arrivalRuntime.observeKill();

	PlayerBotHuntRuntimePlayerObservation arrivalPlayer = planningPlayer;
	arrivalPlayer.health = 150;
	arrivalPlayer.maximumHealth = 170;
	arrivalPlayer.mana = 150;
	arrivalPlayer.supplyCapability = capability(11);
	arrivalPlayer.supplyCapability.equipmentItemIds[5] = 2395;
	PlayerBotSupplyProfile arrivalSupply = arrivalRegion.supplyProfile;
	arrivalSupply.mana = 150;
	arrivalSupply.maximumMana = 170;
	arrivalRuntime.enterHuntArea(arrivalPlayer, arrivalSupply, start + std::chrono::seconds(60));
	arrivalRuntime.observeLevelRestoration(20, 30);
	arrivalRuntime.observeDamage(10);
	arrivalRuntime.sampleCombat({true, start + std::chrono::seconds(60), 160, 170, 170, 180, 1});
	arrivalRuntime.sampleCombat({true, start + std::chrono::seconds(90), 160, 170, 170, 180, 1});
	arrivalRuntime.observeKill();
	PlayerBotHuntRuntimePlayerObservation duplicateArrival = arrivalPlayer;
	duplicateArrival.health = 1;
	duplicateArrival.mana = 1;
	arrivalRuntime.enterHuntArea(duplicateArrival, arrivalSupply, start + std::chrono::seconds(90));
	arrivalRuntime.sampleCombat({true, start + std::chrono::seconds(120), 160, 170, 170, 180, 1});
	arrivalRuntime.observeKill();
	arrivalRuntime.observeKill();
	PlayerBotHuntRuntimePlayerObservation arrivalEnd = arrivalPlayer;
	arrivalEnd.health = 160;
	arrivalEnd.maximumHealth = 170;
	arrivalEnd.mana = 170;
	arrivalEnd.supplyCapability = capability(12);
	arrivalEnd.supplyCapability.equipmentItemIds[5] = 2395;
	const auto arrivalCompleted = arrivalRuntime.complete(
	    arrivalEnd, start + std::chrono::seconds(180), 2400);
	assert(arrivalCompleted && arrivalCompleted->supplyObservation.accepted);
	assert(arrivalCompleted->durationSeconds == 180);
	assert(arrivalCompleted->supplyObservation.durationSeconds == 120);
	assert(arrivalCompleted->supplyObservation.startingHealth == 150);
	assert(arrivalCompleted->supplyObservation.startingMana == 150);
	assert(arrivalCompleted->supplyObservation.endingHealth == 160);
	assert(arrivalCompleted->supplyObservation.endingMana == 170);
	assert(arrivalCompleted->supplyObservation.levelHealthRestored == 20);
	assert(arrivalCompleted->supplyObservation.levelManaRestored == 30);
	assert(arrivalCompleted->supplyObservation.levelAdjustedHealthDebt == 10);
	assert(arrivalCompleted->supplyObservation.levelAdjustedManaDebt == 10);
	assert(std::abs(arrivalCompleted->supplyObservation.staticPotionsPerCombatSecond - 1.0 / 60.0) < 1e-12);
	assert(arrivalCompleted->region.supplyStaticPotionsPerCombatSecond == 10.0 / 60.0);
	assert(arrivalCompleted->combat.damageTaken == 10 && arrivalCompleted->combat.kills == 3);
	assert((arrivalCompleted->supplyObservation.changedFields & PlayerBotSupplyCapabilityLevel) != 0);
	assert((arrivalCompleted->supplyObservation.changedFields & PlayerBotSupplyCapabilityEquipment) == 0);
	assert(arrivalCompleted->supplyObservation.direction == PlayerBotSupplyCapabilityDirection::Improved);
	assert(arrivalCompleted->supplyObservation.localEstimateDirection == PlayerBotSupplyEstimateDirection::Downward);
	assert(arrivalCompleted->supplyObservation.globalUpdated);
	assert(arrivalCompleted->supplyObservation.calibration.capability == arrivalEnd.supplyCapability);

	// Arrival recomputes the raw static denominator from the arrival profile,
	// zero outbound travel, and the hunt time still planned at arrival. It does
	// not retain the pretravel rate or substitute the later observed duration.
	PlayerBotHuntRuntime regenerationArrivalRuntime({});
	PlayerBotHuntRegion regenerationRegion = regionFor(25, capability());
	regenerationRegion.expectedDamagePerSecond = 1.5;
	regenerationRegion.availableHuntSeconds = 180;
	regenerationRegion.estimatedTravelSeconds = 60;
	PlayerBotSupplyProfile regenerationSupply = regenerationRegion.supplyProfile;
	regenerationSupply.regenerationSeconds = 180;
	regenerationSupply.healthGain = 20;
	regenerationSupply.healthInterval = 10;
	const PlayerBotSupplyBudget pretravelBudget = playerBotSupplyBudget(
	    regenerationSupply, regenerationRegion.expectedDamagePerSecond,
	    regenerationRegion.combatFraction, 180, 60);
	regenerationRegion.supplyStaticPotionsPerCombatSecond =
	    pretravelBudget.expectedPotions / 90.0;
	assert(std::abs(regenerationRegion.supplyStaticPotionsPerCombatSecond - 2.0 / 90.0) < 1e-12);
	PlayerBotHuntRuntimePlayerObservation regenerationPlayer;
	regenerationPlayer.health = regenerationPlayer.maximumHealth = 100;
	regenerationPlayer.mana = 100;
	regenerationPlayer.supplyCapability = regenerationRegion.supplyCapability;
	regenerationArrivalRuntime.selectPlanningRegion(regenerationRegion, regenerationPlayer, start);
	regenerationArrivalRuntime.beginCycle(start, 240);
	regenerationArrivalRuntime.enterHuntArea(
	    regenerationPlayer, regenerationSupply, start + std::chrono::seconds(60));
	regenerationArrivalRuntime.sampleCombat(
	    {true, start + std::chrono::seconds(60), 100, 100, 100, 100, 1});
	regenerationArrivalRuntime.sampleCombat(
	    {true, start + std::chrono::seconds(120), 100, 100, 100, 100, 1});
	for (unsigned i = 0; i < 3; ++i) regenerationArrivalRuntime.observeKill();
	const auto regenerationCompleted = regenerationArrivalRuntime.complete(
	    regenerationPlayer, start + std::chrono::seconds(180), 240);
	assert(regenerationCompleted && regenerationCompleted->supplyObservation.accepted);
	assert(regenerationCompleted->supplyObservation.durationSeconds == 120);
	assert(std::abs(regenerationCompleted->supplyObservation.staticPotionsPerCombatSecond -
	                1.0 / 90.0) < 1e-12);
	assert(regenerationCompleted->region.availableHuntSeconds == 180);
	assert(regenerationCompleted->region.supplyStaticPotionsPerCombatSecond == 2.0 / 90.0);

	PlayerBotHuntRuntime noArrivalRuntime({});
	PlayerBotHuntRegion noArrivalRegion = regionFor(24, capability());
	noArrivalRuntime.selectPlanningRegion(noArrivalRegion, planningPlayer, start);
	noArrivalRuntime.sampleCombat({true, start + std::chrono::seconds(1), 100, 100, 100, 100, 1});
	noArrivalRuntime.sampleCombat({true, start + std::chrono::seconds(61), 100, 100, 100, 100, 1});
	for (unsigned i = 0; i < 3; ++i) noArrivalRuntime.observeKill();
	const auto noArrival = noArrivalRuntime.complete(
	    planningPlayer, start + std::chrono::seconds(120), 2400);
	assert(noArrival && !noArrival->supplyObservation.accepted);
	assert(!noArrival->supplyObservation.arrivalBaselineObserved);
	assert(std::string(noArrival->supplyObservation.reason) == "hunt_arrival_not_observed");
	assert(noArrival->supplyObservation.endingHealth == planningPlayer.health);
	const auto noArrivalPerformance = noArrivalRuntime.regionPerformance();
	assert(noArrivalPerformance.find(noArrivalRegion.atlasVariantId) == noArrivalPerformance.end());

	PlayerBotHuntRuntime fallbackArrival({});
	fallbackArrival.enterHuntArea(planningPlayer, noArrivalRegion.supplyProfile, start);
	assert(!fallbackArrival.active());

	PlayerBotHuntPlanningProfile capabilities;
	const auto original = playerBotSupplyCapability(capabilities);
	capabilities.supply.spellLegal = true;
	assert(playerBotSupplyCapability(capabilities) != original);
	capabilities.supply.spellLegal = false;
	capabilities.combat.armor = 5;
	assert(playerBotSupplyCapability(capabilities) != original);

	PlayerBotHuntRegion recovery;
	recovery.supplyRecovery = true;
	recovery.currentHealth = recovery.maximumHealth = 100;
	recovery.supplyProfile.potionHealing = 125;
	recovery.expectedDamagePerSecond = 0.1;
	recovery.reconcileTravel(120, 0, 1);
	assert(recovery.supplyBudget.expectedPotions == 0 && recovery.supplyBudget.fits);
	recovery.supplyProfile.potions = 1;
	recovery.reconcileSupplies(1);
	assert(recovery.supplyBudget.expectedPotions == 0 && recovery.supplyBudget.fits);
	recovery.supplyProfile.potions = 0;
	recovery.coinGoldPerMinute = 1;
	recovery.atlasVariantId = 1;
	recovery.reachable = true;
	assert(recovery.recoverySustainable());
	recovery.recoveryRouteHealthLoss = 10;
	recovery.reconcileSupplies(1);
	assert(!recovery.supplyBudget.fits); // 12 hunt + 10 travel damage exceeds 20 HP.
	auto affordable = recovery;
	affordable.atlasVariantId = 2;
	affordable.recoveryRouteHealthLoss = 5;
	affordable.reconcileSupplies(1);
	assert(affordable.recoverySustainable());
	assert(selectRuntimeHunt({recovery, affordable}).atlasVariantId == 2);
	recovery.currentHealth = 80;
	recovery.reconcileSupplies(1);
	assert(!recovery.supplyBudget.fits);
}

void globalAndLocalSupplyLearning()
{
	auto capability = [] {
		PlayerBotHuntPlanningProfile profile;
		profile.combat.level = 20;
		profile.combat.maximumHealth = 200;
		profile.combat.armor = 20;
		profile.combat.defense = 30;
		profile.combat.attack = 20;
		profile.combat.attackSkill = 50;
		profile.supply.maximumMana = 100;
		profile.supply.potionHealing = 125;
		profile.equipmentItemIds[5] = 2383;
		return playerBotSupplyCapability(profile);
	}();
	auto region = [&](uint64_t variant, const char* firstSpecies,
	                  const char* secondSpecies = nullptr, double damagePerSecond = 4) {
		PlayerBotHuntRegion value;
		value.atlasVariantId = variant;
		value.atlasSiteId = 1000 + variant;
		value.atlasRevision = 7;
		value.supplyCapability = capability;
		value.currentHealth = value.maximumHealth = 200;
		value.supplyProfile.maximumMana = value.supplyProfile.mana = 100;
		value.supplyProfile.potionHealing = 125;
		value.supplyProfile.potions = 20;
		value.availableHuntSeconds = 120;
		value.combatFraction = 0.5;
		value.expectedDamagePerSecond = damagePerSecond;
		value.monsters.push_back({firstSpecies});
		if (secondSpecies) value.monsters.push_back({secondSpecies});
		value.reconcileSupplies(1);
		return value;
	};
	auto easyOuting = [&](PlayerBotHuntPolicy& policy, const PlayerBotHuntRegion& value,
	                     bool interrupted = false, uint32_t endingPotions = 20) {
		policy.resetCombatEvidence();
		policy.observeCombat({true, 60, 200, 200, 100, 100, 1});
		for (unsigned kill = 0; kill < 3; ++kill) policy.observeKill();
		return policy.observeSupplies(
		    value, value.supplyCapability, 120, 200, 200, 100, endingPotions, interrupted);
	};

	// One ordinary mixed-troll observation updates one policy-global multiplier.
	// The global 0.10 blend is modest; the final local 0.20 blend is stronger.
	PlayerBotHuntPolicy policy;
	PlayerBotHuntRegion source = region(1, "Troll", "Swamp Troll");
	const double sourceStatic = source.supplyStaticPotionsPerCombatSecond;
	const auto easy = easyOuting(policy, source);
	assert(easy.accepted && easy.localUpdated && easy.globalUpdated);
	assert(easy.globalSamplesBefore == 0 && easy.globalSamplesAfter == 1);
	assert(std::abs(easy.globalMultiplierAfter - 0.9) < 1e-12);
	assert(std::abs(easy.calibration.potionsPerCombatSecond - sourceStatic * 0.8) < 1e-12);
	assert(policy.regionPerformance().size() == 1);

	// A legitimate zero raw estimate stays zero even when a learned/local budget
	// is positive. Local evidence still updates, but no global ratio is invented.
	PlayerBotHuntPolicy zeroStaticPolicy;
	PlayerBotHuntRegion zeroStatic = region(10, "Rat", nullptr, 0);
	zeroStaticPolicy.observeCombat({true, 60, 200, 200, 100, 100, 1});
	zeroStaticPolicy.observeRecovery(true);
	const auto firstZeroStatic = zeroStaticPolicy.observeSupplies(
	    zeroStatic, capability, 120, 200, 200, 100, 20, false);
	assert(firstZeroStatic.accepted && firstZeroStatic.localUpdated && !firstZeroStatic.globalUpdated);
	assert(firstZeroStatic.staticPotionsPerCombatSecond == 0);
	assert(zeroStaticPolicy.supplyGlobalLearning().samples == 0);
	zeroStatic.supplyCalibration = firstZeroStatic.calibration;
	zeroStatic.reconcileSupplies(1);
	assert(std::string(zeroStatic.supplyEstimateSource) == "local");
	assert(zeroStatic.supplyStaticPotionsPerCombatSecond == 0);
	assert(zeroStatic.supplyBudget.expectedPotions > 0);
	zeroStaticPolicy.resetCombatEvidence();
	zeroStaticPolicy.observeCombat({true, 60, 200, 200, 100, 100, 1});
	for (unsigned kill = 0; kill < 3; ++kill) zeroStaticPolicy.observeKill();
	const auto learnedBudgetAtZeroStatic = zeroStaticPolicy.observeSupplies(
	    zeroStatic, capability, 120, 200, 200, 100, 20, false);
	assert(learnedBudgetAtZeroStatic.accepted && learnedBudgetAtZeroStatic.localUpdated);
	assert(!learnedBudgetAtZeroStatic.globalUpdated);
	assert(learnedBudgetAtZeroStatic.staticPotionsPerCombatSecond == 0);
	assert(std::string(learnedBudgetAtZeroStatic.globalReason) == "static_rate_unavailable");
	assert(zeroStaticPolicy.supplyGlobalLearning().samples == 0);
	assert(learnedBudgetAtZeroStatic.calibration.potionsPerCombatSecond > 0);

	// The global multiplier applies to every unvisited area, including a mixed
	// different-species area, and preserves the static model's relative costs.
	PlayerBotHuntRegion different = region(2, "Rotworm", "Carrion Worm");
	different.supplyGlobalLearning = policy.supplyGlobalLearning();
	different.supplyCapability.equipmentItemIds[5] = 2395;
	different.reconcileSupplies(1);
	assert(std::string(different.supplyEstimateSource) == "global");
	assert(std::string(different.supplyEstimateReason) == "global_policy_multiplier");
	assert(std::abs(different.supplyAppliedPotionsPerCombatSecond -
	                different.supplyStaticPotionsPerCombatSecond * 0.9) < 1e-12);
	PlayerBotHuntRegion changedLocal = region(1, "Troll", "Swamp Troll");
	changedLocal.supplyCalibration = easy.calibration;
	changedLocal.supplyGlobalLearning = policy.supplyGlobalLearning();
	changedLocal.supplyCapability.equipmentItemIds[5] = 2395;
	changedLocal.reconcileSupplies(1);
	assert(std::string(changedLocal.supplyEstimateSource) == "global");
	assert(std::string(changedLocal.supplyLocalRejectionReason) == "material_capability_change");
	PlayerBotHuntRegion harder = region(3, "Dragon", nullptr, 8);
	harder.supplyGlobalLearning = policy.supplyGlobalLearning();
	harder.reconcileSupplies(1);
	assert(harder.supplyStaticPotionsPerCombatSecond > different.supplyStaticPotionsPerCombatSecond);
	assert(std::abs(harder.supplyAppliedPotionsPerCombatSecond /
	                    harder.supplyStaticPotionsPerCombatSecond - 0.9) < 1e-12);

	// Local is a final absolute estimate. The same observation's global factor is
	// not multiplied into it a second time.
	source.supplyGlobalLearning = policy.supplyGlobalLearning();
	source.supplyCalibration = easy.calibration;
	source.reconcileSupplies(1);
	assert(std::string(source.supplyEstimateSource) == "local");
	assert(source.supplyLocalRejectionReason == nullptr);
	assert(std::abs(source.supplyAppliedPotionsPerCombatSecond - sourceStatic * 0.8) < 1e-12);
	assert(source.supplyAppliedPotionsPerCombatSecond < different.supplyAppliedPotionsPerCombatSecond);

	// Scoring any number of atlas variants only reads policy state. It cannot
	// duplicate the single actual outing's global update.
	PlayerBotHuntRegion duplicateA = region(4, "Troll");
	PlayerBotHuntRegion duplicateB = region(5, "Troll");
	duplicateA.supplyGlobalLearning = duplicateB.supplyGlobalLearning = policy.supplyGlobalLearning();
	duplicateA.reconcileSupplies(1);
	duplicateB.reconcileSupplies(1);
	assert(policy.supplyGlobalLearning().samples == 1);
	assert(duplicateA.supplyGlobalLearning.samples == 1 && duplicateB.supplyGlobalLearning.samples == 1);

	// Interrupted, dangerous, and potion-exhausted evidence cannot reduce global
	// or local estimates.
	const PlayerBotSupplyGlobalLearning guardedBefore = policy.supplyGlobalLearning();
	PlayerBotHuntRegion interruptedRegion = region(6, "Troll");
	interruptedRegion.supplyGlobalLearning = guardedBefore;
	const auto interrupted = easyOuting(policy, interruptedRegion, true);
	assert(!interrupted.accepted && std::string(interrupted.reason) == "interrupted_outing");
	assert(policy.supplyGlobalLearning().samples == guardedBefore.samples);
	assert(policy.supplyGlobalLearning().multiplier == guardedBefore.multiplier);

	PlayerBotHuntRegion dangerRegion = region(7, "Troll");
	dangerRegion.supplyGlobalLearning = policy.supplyGlobalLearning();
	policy.resetCombatEvidence();
	policy.observeCombat({true, 60, 200, 200, 100, 100, 1});
	for (unsigned kill = 0; kill < 3; ++kill) policy.observeKill();
	policy.observeDamage(200);
	assert(policy.observeDanger(200, std::chrono::seconds(30)));
	const auto danger = policy.observeSupplies(
	    dangerRegion, capability, 120, 200, 200, 100, 20, false);
	assert(!danger.accepted && std::string(danger.reason) == "danger_observed");
	assert(policy.supplyGlobalLearning().samples == guardedBefore.samples);

	PlayerBotHuntRegion deathRegion = region(70, "Troll");
	deathRegion.supplyGlobalLearning = policy.supplyGlobalLearning();
	policy.resetCombatEvidence();
	policy.observeCombat({true, 60, 200, 200, 100, 100, 1});
	for (unsigned kill = 0; kill < 3; ++kill) policy.observeKill();
	policy.observeDeath();
	const auto death = policy.observeSupplies(
	    deathRegion, capability, 120, 200, 200, 100, 20, false);
	assert(!death.accepted && std::string(death.reason) == "death_observed");
	assert(policy.supplyGlobalLearning().samples == guardedBefore.samples);

	PlayerBotHuntRegion depletedRegion = region(8, "Troll");
	depletedRegion.supplyGlobalLearning = policy.supplyGlobalLearning();
	const auto depleted = easyOuting(policy, depletedRegion, false, 0);
	assert(!depleted.accepted && std::string(depleted.reason) == "potions_depleted");
	assert(policy.supplyGlobalLearning().samples == guardedBefore.samples);

	// Underestimation transfers in one observation rather than using the slow
	// downward blend. This still changes supply cost only, never viability.
	PlayerBotHuntPolicy upwardPolicy;
	PlayerBotHuntRegion costly = region(9, "Troll");
	upwardPolicy.observeCombat({true, 60, 200, 200, 100, 100, 1});
	for (unsigned potion = 0; potion < 8; ++potion) upwardPolicy.observeRecovery(true);
	upwardPolicy.observeDeath();
	const auto upward = upwardPolicy.observeSupplies(
	    costly, capability, 120, 200, 200, 100, 20, false);
	assert(upward.accepted && upward.globalUpdated);
	assert(upward.globalEstimateDirection == PlayerBotSupplyEstimateDirection::Upward);
	assert(std::abs(upward.globalMultiplierAfter -
	                (8.0 / 60.0) / costly.supplyStaticPotionsPerCombatSecond) < 1e-12);

	PlayerBotHuntRegion safe = different;
	safe.suitable = safe.reachable = true;
	safe.score = 10;
	PlayerBotHuntRegion lethal = harder;
	lethal.suitable = false;
	lethal.reachable = true;
	lethal.predictedLethal = true;
	lethal.score = 1000;
	assert(selectRuntimeHunt({lethal, safe}).atlasVariantId == safe.atlasVariantId);

	// Repeated easy evidence approaches but never crosses the positive floor.
	PlayerBotHuntPolicy floorPolicy;
	for (unsigned outing = 0; outing < 40; ++outing) {
		PlayerBotHuntRegion floorRegion = region(100 + outing, "Troll");
		floorRegion.supplyGlobalLearning = floorPolicy.supplyGlobalLearning();
		assert(easyOuting(floorPolicy, floorRegion).accepted);
	}
	assert(floorPolicy.supplyGlobalLearning().multiplier == playerBotSupplyGlobalMinimumMultiplier);
	assert(floorPolicy.supplyGlobalLearning().samples == 40);

	// Learning is controller-session state only; a new policy starts clean.
	PlayerBotHuntPolicy restartedPolicy;
	assert(restartedPolicy.supplyGlobalLearning().multiplier == 1);
	assert(restartedPolicy.supplyGlobalLearning().samples == 0);
	PlayerBotHuntRuntime restartedRuntime({});
	assert(restartedRuntime.supplyGlobalLearning().multiplier == 1);
	assert(restartedRuntime.supplyGlobalLearning().samples == 0);
}

void sharedHuntPerformanceCalibration()
{
	auto reliableSample = [](uint64_t experience) {
		return PlayerBotHuntPerformanceSample{120, 90, 4, experience, 100, 1, 120, false, false};
	};
	PlayerBotHuntPolicy policy;
	constexpr uint64_t atlasRevision = 7;
	auto update = policy.observePerformance(1, atlasRevision, reliableSample(200));
	assert(update.observed && std::abs(update.updatedCorrection - 2.0) < 1e-12);
	update = policy.observePerformance(2, atlasRevision, reliableSample(100));
	assert(update.observed && std::abs(update.updatedCorrection - 1.0) < 1e-12);

	auto performance = policy.regionPerformance();
	const auto tested = playerBotHuntCorrectionForVariant(1, atlasRevision, performance);
	assert(std::string(tested.source) == "variant_observed");
	assert(tested.sampleCount == 1 && std::abs(tested.correction - 2.0) < 1e-12);
	const auto untested = playerBotHuntCorrectionForVariant(3, atlasRevision, performance);
	assert(std::string(untested.source) == "shared_observed");
	assert(untested.sampleCount == 2 && std::abs(untested.correction - 1.5) < 1e-12);

	// Repeated outings refine their own variant but do not multiply its weight in
	// the shared prior: there are still two tested variants.
	assert(policy.observePerformance(1, atlasRevision, reliableSample(200)).observed);
	performance = policy.regionPerformance();
	const auto repeated = playerBotHuntCorrectionForVariant(3, atlasRevision, performance);
	assert(repeated.sampleCount == 2 && std::abs(repeated.correction - 1.5) < 1e-12);

	// Default, malformed, stale, and explicitly unreliable entries are not observations.
	performance.emplace(4, PlayerBotHuntRegionPerformance{});
	performance.emplace(5, PlayerBotHuntRegionPerformance{10, 0, 9, 1, atlasRevision, true, {}});
	performance.emplace(6, PlayerBotHuntRegionPerformance{10, 0, 1.25, 1, atlasRevision, false, {}});
	performance.emplace(8, PlayerBotHuntRegionPerformance{10, 0, 1.75, 1, atlasRevision - 1, true, {}});
	const auto filtered = playerBotHuntCorrectionForVariant(7, atlasRevision, performance);
	assert(filtered.sampleCount == 2 && std::abs(filtered.correction - 1.5) < 1e-12);
	assert(std::string(playerBotHuntCorrectionForVariant(4, atlasRevision, {}).source) == "default");

	PlayerBotHuntPolicy insufficient;
	PlayerBotHuntPerformanceSample sample = reliableSample(200);
	sample.activeCombatSeconds = 59;
	assert(!insufficient.observePerformance(1, atlasRevision, sample).observed);
	sample = reliableSample(200);
	sample.kills = 2;
	assert(!insufficient.observePerformance(1, atlasRevision, sample).observed);
	sample = reliableSample(200);
	sample.durationSeconds = 119;
	assert(!insufficient.observePerformance(1, atlasRevision, sample).observed);
	sample = reliableSample(200);
	sample.dangerObserved = true;
	const auto unsafe = insufficient.observePerformance(1, atlasRevision, sample);
	assert(!unsafe.observed && unsafe.actualExperiencePerMinute == 100);
	sample.durationSeconds = 0;
	assert(insufficient.observePerformance(1, atlasRevision, sample).actualExperiencePerMinute == 0);
	assert(insufficient.regionPerformance().empty());
}

void supplyRecoveryMode()
{
	PlayerBotSupplyRecoveryState recovery;
	assert(!recovery.active());
	assert(recovery.enter());
	assert(recovery.active() && !recovery.enter());
	// An unknown budget must not clear the degraded state.
	assert(!recovery.update(std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max()));
	assert(recovery.update(500, 400)); // Funds above budget end recovery.
	assert(!recovery.active());
	assert(recovery.update(100, 400)); // A shortfall enters recovery.
	assert(recovery.active());

	assert(playerBotSupplyRecoveryChallengeFrontier(0.6, true) == 0.20);
	assert(playerBotSupplyRecoveryChallengeFrontier(0.1, true) == 0.1);
	assert(playerBotSupplyRecoveryChallengeFrontier(0.6, false) == 0.6);

	PlayerBotEconomyInventorySnapshot inventory{1, 100, 36, 0};
	PlayerBotDispositionPolicy policy;
	// Normal restock refuses gold that cannot lift the stock past the return
	// threshold; survival restock buys what is affordable instead.
	const auto normal = policy.restock(inventory, 45, 0, 1, 10);
	assert(normal.insufficientFunds && normal.amount == 0);
	const auto survival = policy.restock(inventory, 45, 0, 1, 10, true);
	assert(!survival.insufficientFunds && survival.amount == 0);
	PlayerBotEconomyInventorySnapshot richer{1, 100, 146, 0};
	const auto partial = policy.restock(richer, 45, 0, 1, 10, true);
	assert(!partial.insufficientFunds && partial.amount == 3); // Survival may spend the cash reserve.
	assert(policy.restock({0, 100, 45, 0}, 45, 0, 1, 20, true).amount == 1);
	assert(policy.restock({0, 0, 45, 0}, 45, 10, 1, 20, true).amount == 0);

	PlayerBotSupplyRecoveryState deferred;
	deferred.deferRestock(126, 0);
	assert(deferred.active() && deferred.restockBlocked(126, 0));
	assert(!deferred.restockBlocked(171, 0) && !deferred.active());
	deferred.deferRestock(126, 0);
	assert(!deferred.restockBlocked(126, 1));
	PlayerBotGoalPlanner planner;
	PlayerBotGoalPlannerSnapshot goal;
	for (uint32_t potions : {0U, 1U, 2U, 19U, 20U}) {
		goal.missingPotions = playerBotMandatoryPotionDeficit(potions, 2, false);
		assert(planner.serviceCandidate(goal).feasible == (potions < 2));
		goal.missingPotions = playerBotMandatoryPotionDeficit(potions, 2, true);
		assert(!planner.serviceCandidate(goal).feasible);
	}

	goal.criticalHealing = true;
	assert(planner.serviceCandidate(goal).feasible);

	const PlayerBotSurvivalSellCandidate rejected{
	    true, true, true, true, 10, 500};
	assert(!playerBotSurvivalSellAccepted(true, rejected));
	const PlayerBotSurvivalSellCandidate unreachable{false, false, false, true, 0, 500};
	assert(!playerBotSurvivalSellAccepted(true, unreachable));
	const PlayerBotSurvivalSellCandidate unsafe{false, true, true, false, 0, 500};
	assert(!playerBotSurvivalSellAccepted(true, unsafe));
	const PlayerBotSurvivalSellCandidate unaffordable{false, true, true, true, 600, 500};
	assert(!playerBotSurvivalSellAccepted(true, unaffordable));
	const PlayerBotSurvivalSellCandidate viable{false, true, true, true, 10, 500};
	assert(playerBotSurvivalSellAccepted(true, viable));
	assert(!playerBotSurvivalSellAccepted(false, viable));

	// Recovery ordering puts coin income ahead of experience even for regions
	// that are not cash-pressed, and breaks income ties toward lower threat.
	PlayerBotHuntRegion gold, xp;
	gold.suitable = xp.suitable = gold.reachable = xp.reachable = true;
	gold.supplyRecovery = xp.supplyRecovery = true;
	gold.coinGoldPerMinute = 5;
	gold.score = 10;
	xp.score = 100;
	assert(playerBotPreferHuntRegion(gold, xp));
	PlayerBotHuntRegion safer = xp;
	safer.coinGoldPerMinute = 5;
	safer.threatRatio = 0.1;
	xp.threatRatio = 0.4;
	assert(playerBotPreferHuntRegion(safer, xp));
}

void navigationFixedObjective()
{
	PlayerBotNavigationGoal depot;
	depot.position = Position(32341, 32229, 8);
	PlayerBotNavigationGoal flap;
	flap.position = Position(32343, 32229, 8);
	PlayerBotNavigationGoal distant;
	distant.position = Position(32105, 32191, 8);
	assert(playerBotNavigationSameFixedObjective(depot, depot));
	// Adjacent approach tiles of the same depot are one objective.
	assert(playerBotNavigationSameFixedObjective(depot, flap));
	assert(!playerBotNavigationSameFixedObjective(depot, distant));
}

void topologyComponentCompression()
{
	const auto bidirectional = playerBotTopologyBidirectionalComponents(
	    3, {{0, 1, true}, {1, 0, true}, {1, 2, true}});
	assert(bidirectional[0] == bidirectional[1]);
	assert(bidirectional[1] != bidirectional[2]); // A one-way edge stays directed.

	const auto gated = playerBotTopologyBidirectionalComponents(
	    2, {{0, 1, true}, {1, 0, false}});
	assert(gated[0] != gated[1]); // A level-gated reverse edge cannot merge components.

	const auto gatedBothWays = playerBotTopologyBidirectionalComponents(
	    2, {{0, 1, false}, {1, 0, false}});
	assert(gatedBothWays[0] != gatedBothWays[1]);
}

void fixtureHuntHorizons()
{
	assert(playerbot::playerBotFixtureHuntPlanningDuration(false, 10) == 10);
	assert(playerbot::playerBotFixtureHuntPlanningDuration(true, 10) == 900);
	assert(playerbot::playerBotFixtureHuntPlanningDuration(true, 1500) == 1500);
}

void remoteHuntTravelGuards()
{
	// Once a coarse route reaches the provider's local area, finish against the
	// live interaction range instead of an arbitrary exact topology approach tile.
	const Position provider(32386, 31820, 6);
	assert(playerBotNpcTravelUsesLocalApproach(Position(32382, 31816, 6), provider, 11));
	assert(playerBotNpcTravelUsesLocalApproach(Position(32375, 31820, 6), provider, 11));
	assert(!playerBotNpcTravelUsesLocalApproach(Position(32374, 31820, 6), provider, 11));
	assert(!playerBotNpcTravelUsesLocalApproach(Position(32382, 31816, 7), provider, 11));
	assert(playerBotNpcTravelApproachComplete(true, false, false));
	assert(playerBotNpcTravelApproachComplete(false, true, true));
	assert(!playerBotNpcTravelApproachComplete(false, false, false));
	assert(!playerBotNpcTravelApproachComplete(false, true, false));

	// Free routes need no cash reserve; planned paid routes account for all fares.
	assert(playerBotHuntTravelAffordable(0, 100, 0, 0, 0));
	assert(playerBotHuntTravelAffordable(50, 100, 0, 0, 0));
	assert(!playerBotHuntTravelAffordable(50, 100, 1, 0, 0));
	assert(playerBotHuntTravelAffordable(500, 100, 100, 200, 100));
	assert(!playerBotHuntTravelAffordable(499, 100, 100, 200, 100));
	assert(!playerBotHuntTravelAffordable(500, 100, 250, 200));
	assert(playerBotHuntTravelAffordable(550, 100, 250, 200));

	using BudgetPhase = PlayerBotHuntTravelBudgetPhase;
	// Returning to a depot spends only the actual fare. A future restock budget
	// must not strand 198 gold behind a 50-gold Svargrond fare.
	assert(playerBotHuntTravelPaymentAffordable(198, 820, 50, 500, BudgetPhase::ReturnToDepot));
	assert(!playerBotHuntTravelPaymentAffordable(49, 820, 50, 0, BudgetPhase::ReturnToDepot));
	// Outbound travel preserves the selected return fare, including when the
	// current outbound leg itself is free. Restock affordability is separate.
	assert(playerBotHuntTravelPaymentAffordable(100, 820, 50, 50, BudgetPhase::Outbound));
	assert(!playerBotHuntTravelPaymentAffordable(99, 820, 50, 50, BudgetPhase::Outbound));
	assert(playerBotHuntTravelPaymentAffordable(50, 820, 0, 50, BudgetPhase::Outbound));
	assert(!playerBotHuntTravelPaymentAffordable(49, 820, 0, 50, BudgetPhase::Outbound));
	// A return revalidation can raise the reserve after the outbound plan was saved.
	assert(playerBotHuntTravelPaymentAffordable(150, 820, 50, 50, BudgetPhase::Outbound));
	assert(!playerBotHuntTravelPaymentAffordable(150, 820, 50, 110, BudgetPhase::Outbound));
	assert(playerBotHuntTravelPaymentAffordable(50, 820, 50, 0, BudgetPhase::None));
	assert(std::string(playerBotHuntTravelBudgetPhaseName(BudgetPhase::ReturnToDepot)) == "return_to_depot");
	assert(std::string(playerBotHuntTravelFareRejectionReason(BudgetPhase::Outbound)) ==
	       "fare_breaks_return_reserve");
	assert(std::string(playerBotHuntTravelFareRejectionReason(BudgetPhase::ReturnToDepot)) ==
	       "fare_unaffordable");
	const uint64_t recoveryBeforeRestock = playerBotRecoverySpendingReserve(2, 10, 45, 100);
	const uint64_t recoveryAfterRestock = playerBotRecoverySpendingReserve(10, 10, 45, 100);
	assert(recoveryBeforeRestock == 460 && recoveryAfterRestock == 100);
	assert(!playerBotHuntTravelPaymentAffordable(
	    200, recoveryBeforeRestock, 50, 0, BudgetPhase::Supply));
	assert(playerBotHuntTravelPaymentAffordable(
	    200, recoveryAfterRestock, 50, 0, BudgetPhase::Supply));

	const Position coveredPosition(32090, 31263, 7);
	const Position nextPosition(32091, 31263, 7);
	const Position depotPosition(32080, 31250, 7);
	PlayerBotHuntReturnCoverage coverage;
	PlayerBotHuntReturnCoverageContext coverageContext{
	    11, 22, 17, coveredPosition, depotPosition, 50, true, true, false};
	coverage.validate(42, 7, coverageContext);
	assert(coverage.covers(42, 7, coverageContext));
	assert(!coverage.covers(43, 7, coverageContext));
	assert(!coverage.covers(42, 8, coverageContext));
	auto uncoveredPosition = coverageContext;
	uncoveredPosition.coveredPosition = nextPosition;
	assert(!coverage.covers(42, 7, uncoveredPosition));
	auto changedCoverage = coverageContext;
	++changedCoverage.topologyGeneration;
	assert(!coverage.covers(42, 7, changedCoverage));
	coverage.validate(42, 7, changedCoverage);
	assert(coverage.covers(42, 7, changedCoverage));
	++changedCoverage.npcGeneration;
	assert(!coverage.covers(42, 7, changedCoverage));
	coverage.invalidate(); // A route failure always forces one fresh return validation.
	assert(!coverage.valid());

	PlayerBotNavigationRoutePlan localWalk;
	localWalk.metrics.result = PlayerBotNavigationResult::Reached;
	localWalk.metrics.steps = 1;
	PlayerBotNavigationStep localStep;
	localStep.action = PlayerBotNavigationAction::Move;
	localStep.direction = DIRECTION_EAST;
	localStep.target = localStep.expectedPosition = nextPosition;
	localWalk.steps.push_back(localStep);
	assert(playerBotNavigationIsReversibleLocalWalk(coveredPosition, nextPosition, localWalk));
	coverage.validate(42, 7, coverageContext);
	if (playerBotNavigationIsReversibleLocalWalk(coveredPosition, nextPosition, localWalk)) {
		coverage.validate(42, 7, uncoveredPosition);
	}
	assert(coverage.covers(42, 7, uncoveredPosition));

	PlayerBotNavigationRoutePlan exposedWalk = localWalk;
	exposedWalk.metrics.dangerCost = 1;
	assert(!playerBotNavigationIsReversibleLocalWalk(coveredPosition, nextPosition, exposedWalk));
	exposedWalk.metrics.dangerCost = 0;
	exposedWalk.metrics.maximumHealthLossPerSecond = 0.001;
	assert(!playerBotNavigationIsReversibleLocalWalk(coveredPosition, nextPosition, exposedWalk));
	PlayerBotNavigationRoutePlan portalWalk = localWalk;
	portalWalk.steps.front().topologyPortal = true;
	coverage.validate(42, 7, coverageContext);
	assert(!playerBotNavigationIsReversibleLocalWalk(coveredPosition, nextPosition, portalWalk));
	assert(!coverage.covers(42, 7, uncoveredPosition));
	PlayerBotNavigationRoutePlan npcTravel = localWalk;
	npcTravel.steps.front().action = PlayerBotNavigationAction::NpcTravel;
	assert(!playerBotNavigationIsReversibleLocalWalk(coveredPosition, nextPosition, npcTravel));
	PlayerBotNavigationRoutePlan floorChange = localWalk;
	floorChange.steps.front().expectedPosition.z = 8;
	assert(!playerBotNavigationIsReversibleLocalWalk(coveredPosition, Position(32091, 31263, 8), floorChange));
	PlayerBotNavigationRoutePlan partialExactMetrics = localWalk;
	assert(!playerBotNavigationIsReversibleLocalWalk(
	    coveredPosition, Position(32092, 31263, 7), partialExactMetrics));
	PlayerBotNavigationRoutePlan discontinuousWalk = localWalk;
	discontinuousWalk.metrics.steps = 2;
	PlayerBotNavigationStep jumpedStep = localStep;
	jumpedStep.target = jumpedStep.expectedPosition = Position(32093, 31263, 7);
	discontinuousWalk.steps.push_back(jumpedStep);
	assert(!playerBotNavigationIsReversibleLocalWalk(
	    coveredPosition, Position(32093, 31263, 7), discontinuousWalk));
	PlayerBotNavigationRoutePlan incrementalNpcApproach = localWalk;
	incrementalNpcApproach.metrics.steps = 2; // The uninstalled next step is a free NPC transition.
	assert(!playerBotNavigationIsReversibleLocalWalk(
	    coveredPosition, nextPosition, incrementalNpcApproach));
	assert(playerBotDepotRouteSafetyAccepted(true, false, false, true));
	// Return-payment policy does not imply selected-hunt route protection:
	// startup/service returns retain the established risky-depot fallback.
	assert(playerBotDepotRouteSafetyAccepted(false, true, false, false));
	assert(!playerBotDepotRouteSafetyAccepted(false, true, false, true));
	assert(!playerBotDepotRouteSafetyAccepted(false, true, true, false));
	assert(!playerBotHuntTravelAffordable(500, 200, UINT64_MAX, 1));
	assert(!playerBotHuntNeedsSupplyRoute(0, 3, 2));
	assert(playerBotHuntNeedsSupplyRoute(1, 3, 2));
	assert(playerBotHuntNeedsSupplyRoute(0, 2, 2));
	assert(playerBotHuntNeedsSupplyRoute(0, 1, 2));
	assert(playerBotNpcTravelOfferEligible(15, false, 100, 10, false, 100, false, false));
	assert(!playerBotNpcTravelOfferEligible(9, true, 100, 10, false, 100, false, false));
	assert(!playerBotNpcTravelOfferEligible(15, false, 100, 10, true, 100, false, false));
	assert(!playerBotNpcTravelOfferEligible(15, true, 99, 10, true, 100, false, false));
	assert(!playerBotNpcTravelOfferEligible(15, true, 100, 10, true, 100, true, false));
	assert(!playerBotNpcTravelOfferEligible(15, true, 100, 10, true, 100, false, true));
	const PlayerBotNavigationRiskProfile risk;
	assert(playerBotNavigationRiskAccepts(risk, 500, 0.08));
	assert(!playerBotNavigationRiskAccepts(risk, 501, 0.08));
	assert(!playerBotNavigationRiskAccepts(risk, 500, 0.081));
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
	profile.reserve = 0;
	assert(playerBotSupplyBudget(profile, 0, 0, 900, 0).fits);
	profile.reserve = 1;
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
	assert(playerBotRecoverySpendingReserve(2, 20, 45, 100) == 910);
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
	snapshot.missingPotions = 2;
	candidates = planner.candidates(snapshot);
	assert(candidate(Goal::LearnSpell).feasible && candidate(Goal::LearnSpell).reason == "priority_recovery_spell");
	assert(candidate(Goal::Service).feasible && candidate(Goal::Service).reason == "healing_reserve");
	assert(!candidate(Goal::Hunt).feasible && candidate(Goal::Hunt).reason == "deferred_recovery_spell");
	snapshot.missingPotions = 0;
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

void toolReplenishmentPlannerGuards()
{
	PlayerBotEquipmentProviderPlanner planner;
	PlayerBotEquipmentProviderPlannerSnapshot snapshot;
	snapshot.enabled = true;
	snapshot.reserve = 100;
	snapshot.totalMoney = 150;
	snapshot.reserveAvailable = true;
	snapshot.freeCapacity = 100;
	snapshot.maximumRouteDangerCost = 100;
	snapshot.maximumRouteDanger = 100;

	PlayerBotEquipmentProviderOfferSnapshot carried;
	carried.evaluation.itemId = 2376;
	carried.evaluation.carried = true;
	carried.evaluation.rule = PlayerBotEquipmentDecisionRule::ReadinessRepair;
	carried.backpackAvailable = true;

	PlayerBotEquipmentProviderOfferSnapshot tool;
	tool.evaluation.itemId = 2120;
	tool.evaluation.price = 50;
	tool.evaluation.toolAcquisition = true;
	tool.evaluation.rule = PlayerBotEquipmentDecisionRule::ReadinessRepair;
	tool.itemWeight = 100;
	tool.freeBackpackSlots = 1;
	tool.backpackAvailable = true;
	tool.purchaseAvailable = true;
	tool.route.reachable = true;

	auto select = [&planner, &snapshot, &carried, &tool]() {
		snapshot.offers = {carried, tool};
		return planner.select(snapshot);
	};
	auto rejection = [](const PlayerBotEquipmentProviderDecision& decision) {
		auto result = std::find_if(decision.rejections.begin(), decision.rejections.end(), [](const auto& value) {
			return value.offerIndex == 1;
		});
		assert(result != decision.rejections.end());
		return result->reason;
	};

	// A tool wins only after it passes all ordinary purchase guards.
	auto decision = select();
	assert(decision.selected && decision.selected->itemId == tool.evaluation.itemId);
	snapshot.totalMoney = 149;
	decision = select();
	assert(decision.selected && decision.selected->itemId == carried.evaluation.itemId);
	assert(rejection(decision) == "unaffordable_after_reserves");

	snapshot.totalMoney = 150;
	tool.freeBackpackSlots = 0;
	decision = select();
	assert(decision.selected && decision.selected->itemId == carried.evaluation.itemId);
	assert(rejection(decision) == "insufficient_displaced_item_space");

	tool.freeBackpackSlots = 1;
	snapshot.freeCapacity = 99;
	decision = select();
	assert(decision.selected && decision.selected->itemId == carried.evaluation.itemId);
	assert(rejection(decision) == "insufficient_capacity");

	snapshot.freeCapacity = 100;
	tool.backpackAvailable = false;
	decision = select();
	assert(decision.selected && decision.selected->itemId == carried.evaluation.itemId);
	assert(rejection(decision) == "insufficient_displaced_item_space");

	PlayerBotGoalPlanner goalPlanner;
	PlayerBotGoalPlannerSnapshot goal;
	goal.spellPlanAvailable = true;
	goal.equipmentEnabled = goal.equipmentPlanAvailable = true;
	goal.equipmentToolAcquisition = true;
	auto goals = goalPlanner.candidates(goal);
	auto goalCandidate = [&goals](PlayerBotGoalArbiter::TopLevelGoal type) -> const PlayerBotGoalArbiter::GoalCandidate& {
		return *std::find_if(goals.begin(), goals.end(), [type](const auto& candidate) { return candidate.goal == type; });
	};
	assert(goalCandidate(PlayerBotGoalArbiter::TopLevelGoal::BuyEquipment).utility >
	       goalCandidate(PlayerBotGoalArbiter::TopLevelGoal::LearnSpell).utility);
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

static void backpackAcquisition()
{
	PlayerBotEquipmentPolicy policy(4);
	PlayerBotEquipmentPlayerSnapshot knight;
	knight.vocationId = 4;
	auto acquisition = policy.standardBackpackAcquisition(knight, 0, false, 0, 0);
	assert(acquisition.eligible && !acquisition.requiresStaging && acquisition.rejection == nullptr);

	// A full bag is safe because the complete container is staged, not its contents.
	acquisition = policy.standardBackpackAcquisition(knight, 1987, true, 8, 8);
	assert(acquisition.eligible && acquisition.requiresStaging && acquisition.rejection == nullptr);
	acquisition = policy.standardBackpackAcquisition(knight, 1987, false, 0, 0);
	assert(!acquisition.eligible && std::string(acquisition.rejection) == "back_slot_not_upgradeable");
	for (uint16_t unchanged : {uint16_t{1988}, uint16_t{2000}}) {
		acquisition = policy.standardBackpackAcquisition(knight, unchanged, true, 20, 20);
		assert(!acquisition.eligible && std::string(acquisition.rejection) == "back_slot_not_upgradeable");
	}
	knight.vocationId = 3;
	acquisition = policy.standardBackpackAcquisition(knight, 0, false, 0, 0);
	assert(!acquisition.eligible && std::string(acquisition.rejection) == "unsupported_vocation");

	PlayerBotEquipmentLoadout loadout;
	PlayerBotEquipmentReadinessInput readiness{false, true, 10000, 100};
	auto combat = policy.combatReadiness(PlayerBotEquipmentPlayerSnapshot{0, 0, 4}, loadout, false, readiness);
	assert(combat.recovery.empty()); // Missing weapon remains the earlier, terminal readiness gap.
	assert(combat.terminalReason == "missing_legal_melee_weapon");
	PlayerBotEquipmentItemSnapshot sword;
	sword.itemId = 2376;
	sword.weaponType = PlayerBotEquipmentWeaponType::Sword;
	sword.attack = 14;
	sword.left = sword.pickupable = true;
	loadout.items[6] = sword;
	PlayerBotEquipmentItemSnapshot armor;
	armor.itemId = 2463;
	armor.armorSlot = armor.pickupable = true;
	armor.armor = 10;
	loadout.items[4] = armor;
	combat = policy.combatReadiness(PlayerBotEquipmentPlayerSnapshot{0, 0, 4}, loadout, false, readiness);
	assert(combat.recovery == "acquire_backpack" && combat.terminalReason.empty());
}

static void carriedShieldUpgrade()
{
	PlayerBotEquipmentPolicy policy(4);
	PlayerBotEquipmentPlayerSnapshot knight;
	knight.vocationId = 4;
	knight.level = 20;
	PlayerBotEquipmentLoadout loadout;
	auto& sword = loadout.items[6];
	sword.itemId = 2376;
	sword.weaponType = PlayerBotEquipmentWeaponType::Sword;
	sword.attack = 14;
	sword.left = sword.pickupable = true;
	loadout.itemIds[6] = sword.itemId;
	PlayerBotEquipmentItemSnapshot shield;
	shield.itemId = 2512;
	shield.weaponType = PlayerBotEquipmentWeaponType::Shield;
	shield.defense = 14;
	shield.right = shield.pickupable = shield.inContainer = true;
	auto selected = policy.findCarriedUpgrade(knight, loadout, {{shield, true}, {shield, true}});
	assert(selected && selected->index == 0);
	assert(static_cast<uint8_t>(selected->upgrade.slot) == 5);
	assert(std::string(selected->upgrade.metric) == "defense");
	assert(selected->upgrade.benefit == 14);
	loadout.items[5] = shield;
	loadout.itemIds[5] = shield.itemId;
	assert(!policy.findCarriedUpgrade(knight, loadout, {{shield, true}}));
	shield.defense = 13;
	assert(!policy.findCarriedUpgrade(knight, loadout, {{shield, true}}));
	shield.defense = 16;
	selected = policy.findCarriedUpgrade(knight, loadout, {{shield, true}});
	assert(selected && selected->upgrade.benefit == 2);
	assert(!policy.findCarriedUpgrade(knight, loadout, {{shield, false}}));
	shield.inContainer = false;
	assert(!policy.findCarriedUpgrade(knight, loadout, {{shield, true}}));
	shield.inContainer = true;
	shield.minimumLevel = 21;
	assert(!policy.findCarriedUpgrade(knight, loadout, {{shield, true}}));
	shield.minimumLevel = 0;
	shield.pickupable = false;
	assert(!policy.findCarriedUpgrade(knight, loadout, {{shield, true}}));
	shield.pickupable = true;
	shield.attack = 100;
	for (auto type : {PlayerBotEquipmentWeaponType::Distance, PlayerBotEquipmentWeaponType::Ammo,
	                 PlayerBotEquipmentWeaponType::Other}) {
		shield.weaponType = type;
		assert(!policy.findCarriedUpgrade(knight, loadout, {{shield, true}}));
	}
	for (auto type : {PlayerBotEquipmentWeaponType::Sword, PlayerBotEquipmentWeaponType::Axe,
	                 PlayerBotEquipmentWeaponType::Club}) {
		shield.weaponType = type;
		assert(policy.findCarriedUpgrade(knight, loadout, {{shield, true}}));
	}
}

int main()
{
	backpackAcquisition();
	carriedShieldUpgrade();
	toolReplenishmentPlannerGuards();
	loggingContracts();
	huntEconomy();
	patrolOpportunity();
	mixedSustainedYield();
	raisedHuntRecoveryReserve();
	huntCandidateTelemetryCompleteness();
	huntCandidateTelemetryDeltas();
	incrementalHuntValidationPipeline();
	lootArithmeticMemo();
	modeledPatrolFailure();
	mutableShovelPassages();
	navigationFailureAccounting();
	transitCombat();
	crowdDamageInflation();
	adaptiveChallenge();
	sharedHuntPerformanceCalibration();
	supplyRecoveryMode();
	supplyCalibration();
	globalAndLocalSupplyLearning();
	navigationFixedObjective();
	topologyComponentCompression();
	fixtureHuntHorizons();
	remoteHuntTravelGuards();
	overBudgetHunts();
	supplyBudget();
	recoverySpellPriority();
	projection();
	oracleRecovery();
	std::cout << "playerbot contracts passed\n";
}
