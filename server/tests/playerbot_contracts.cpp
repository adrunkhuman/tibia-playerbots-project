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
#include "playerbotnavigationsession.h"
#include "playerbottransitcombat.h"
#include "playerbottopology.h"
#include "playerbothuntregions.h"
#include "playerbothuntruntime.h"
#include "playerbothunttravelpolicy.h"
#include "playerbothunttravelevidence.h"
#include "playerbothuntrouteretention.h"
#include "playerbotinventorypolicy.h"
#include "playerbotsupplyrecovery.h"
#include "playerbothuntsupplyhandoff.h"
#include "playerbotsupplystock.h"
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

	assert(playerBotHuntCashPressure(0));
	assert(playerBotHuntCashPressure(199));
	assert(!playerBotHuntCashPressure(200));
	assert(!playerBotHuntCashPressure(201));
	assert(!playerBotHuntCashPressure(100 + 100)); // Withdrawing gold does not change total funds.
	PlayerBotHuntRegion income, xp;
	income.suitable = xp.suitable = income.reachable = xp.reachable = true;
	income.supplyProfile.potions = xp.supplyProfile.potions = 20;
	income.availableHuntSeconds = xp.availableHuntSeconds = 60;
	income.reconcileRecovery(1, 199);
	xp.reconcileRecovery(1, 199);
	assert(income.cashPressure && xp.cashPressure);
	income.coinGoldPerMinute = 10;
	income.score = 10;
	xp.score = 100;
	assert(playerBotPreferHuntRegion(income, xp));
	assert(playerBotHuntCandidateCanBeatValidated(income, xp));
	income.coinGoldPerMinute = 0;
	assert(playerBotPreferHuntRegion(xp, income)); // No coin-producing option: retain XP fallback.
	income.coinGoldPerMinute = 10;
	income.reconcileRecovery(1, 200);
	xp.reconcileRecovery(1, 200);
	assert(!income.cashPressure && !xp.cashPressure);
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
	const PlayerBotHuntPlanningSession* session = runtime.planningSession();
	assert(session && session->ready() && session->scoredCandidates() == 300);
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
	assert(cancelledRuntime.planningSession() == nullptr);
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

	// Typed-stock bounds cannot survive either a gain or loss of inventory.
	for (uint32_t changedCount : {1U, 3U}) {
		PlayerBotHuntRuntime stockRuntime({});
		auto stockInput = inputFor(51, 1);
		stockInput.player.supplies = {{playerBotThrowingWeaponRule(2389, 8), 2}};
		const auto stockStart = stockRuntime.advancePlanning(stockInput, now);
		const auto work = stockRuntime.advancePlanning(stockInput, now);
		const auto stockScored = stockRuntime.completeScoreWork(observationsFor(work.scoreWork), 0);
		assert(stockRuntime.beginRouteSelection(stockScored.planningPass, stockScored.scoringRevision,
		    stockScored.routeCandidates));
		const auto staleRequest = stockRuntime.nextRouteRequest();
		assert(staleRequest);
		stockInput.player.supplies[0].count = changedCount;
		const auto stockRestart = stockRuntime.advancePlanning(stockInput, now);
		assert(stockRestart.command == PlayerBotHuntRuntimeCommand::PlanningStarted && stockRestart.planningCancelled &&
		       !stockRestart.staleRevision && std::strcmp(stockRestart.cancellationReason, "supplies_changed") == 0 &&
		       stockRestart.invalidatedPlanningPass == stockStart.planningPass &&
		       stockRestart.planningPass == stockStart.planningPass + 1);
		assert(!stockRuntime.observeRoute(*staleRequest, {}).accepted);
		assert(!stockRuntime.nextRouteRequest()); // Old bound/selector is gone, before rescoring.
	}
}

void raisedHuntRecoveryReserve()
{
	using namespace playerbot;
	PlayerBotHuntRegion income, xp;
	income.atlasVariantId = 1;
	xp.atlasVariantId = 2;
	income.suitable = xp.suitable = income.reachable = xp.reachable = true;
	income.supplyProfile.potions = xp.supplyProfile.potions = 10;
	income.availableHuntSeconds = xp.availableHuntSeconds = 60;
	income.coinGoldPerMinute = 10;
	income.score = 10;
	xp.score = 100;
	const uint64_t funds = 200;
	const uint32_t price = 45;
	auto reconcile = [&](PlayerBotHuntRegion& region, uint32_t reserve, uint64_t money) {
		region.reconcileRecovery(reserve, money);
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
	assert(!income.cashPressure && !income.supplyBudget.fits);
	assert(selectRuntimeHunt({income, xp}).atlasVariantId == 2);
	reconcile(income, candidateReserve, 199);
	reconcile(xp, candidateReserve, 199);
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

PlayerBotHuntRegion routeFixture(uint64_t id, double score = 100)
{
	PlayerBotHuntRegion region;
	region.atlasVariantId = id;
	region.destination = Position(static_cast<uint16_t>(100 + id), 100, 7);
	region.suitable = region.reachable = region.supplyBudget.fits = true;
	region.score = score;
	region.availableHuntSeconds = 60;
	region.optimisticProjectedExperience = score;
	region.experiencePerMinute = score;
	region.observedCorrection = region.staminaExperienceMultiplier = 1;
	region.supplyProfile.potions = 100;
	return region;
}

PlayerBotHuntRouteObservation routeFacts(bool reached = true)
{
	PlayerBotHuntRouteObservation observation;
	observation.reached = reached;
	observation.huntDurationSeconds = 60;
	observation.funds = 100;
	observation.supplyProfile.potions = 100;
	return observation;
}

void preparationContracts()
{
	// A bank basement and a nearby upstairs provider/rest need ordinary floor
	// transitions, not a same-floor whitelist. Coordinates are synthetic.
	const Position basement(100, 100, 8), upstairsShop(86, 76, 7), upstairsRest(100, 90, 7);
	assert(playerBotPreparationProviderInRange(basement, upstairsShop));
	const auto restCandidates = playerBotPreparationRestPositions(basement,
	    [&](const Position& candidate) { return candidate == upstairsRest; });
	assert(restCandidates == std::vector<Position>{upstairsRest});
	assert(!playerBotPreparationProviderInRange(basement, Position(133, 100, 7)));
	assert(!playerBotPreparationProviderInRange(basement, Position(100, 100, 5)));
	const auto boundedRest = playerBotPreparationRestPositions(Position(0, 0, 0), [](const Position&) { return true; });
	assert(boundedRest.size() == 512);
	for (const Position& position : boundedRest) assert(position.x <= 32 && position.y <= 32 && position.z <= 2);
	assert(playerBotPreparationRestPositions(basement, [](const Position&) { return false; }).empty()); // No safe non-PZ endpoints.
	PlayerBotPreparationRouteBudget routes;
	for (int i = 0; i < 4; ++i) { assert(routes.allowance() == 4096); routes.observed(4096); }
	assert(routes.allowance() == 0);
	PlayerBotPreparationRouteBudget cheapRoutes;
	for (int i = 0; i < 8; ++i) { assert(cheapRoutes.allowance() == 4096); cheapRoutes.observed(0); }
	assert(cheapRoutes.allowance() == 0);
	assert(playerBotPreparationRouteAccepted(PlayerBotNavigationResult::Reached, 0, 0, 0, true));
	for (auto result : {PlayerBotNavigationResult::NodeLimit, PlayerBotNavigationResult::Unreachable, PlayerBotNavigationResult::RiskRejected})
		assert(!playerBotPreparationRouteAccepted(result, 0, 0, 0, true));
	assert(!playerBotPreparationRouteAccepted(PlayerBotNavigationResult::Reached, 1, 0, 0, true));
	assert(!playerBotPreparationRouteAccepted(PlayerBotNavigationResult::Reached, 0, 0.01, 0, true));
	assert(!playerBotPreparationRouteAccepted(PlayerBotNavigationResult::Reached, 0, 0, 1, true));
	assert(!playerBotPreparationRouteAccepted(PlayerBotNavigationResult::Reached, 0, 0, 0, false)); // A waypoint is not a complete proof.
	PlayerBotGoalPlanner planner;
	PlayerBotGoalPlannerSnapshot snapshot;
	snapshot.magicTrainingReason = "no_regeneration";
	PlayerBotPreparationRequirement health;
	health.kind = PlayerBotRequirementKind::HealthRegeneration;
	health.health = 186;
	snapshot.huntReadiness = {PlayerBotReadiness::Blocked, health};
	auto candidates = planner.candidates(snapshot);
	auto hunt = [](const auto& candidates) -> const PlayerBotGoalArbiter::GoalCandidate& {
		return *std::find_if(candidates.begin(), candidates.end(), [](const auto& candidate) {
			return candidate.goal == PlayerBotGoalArbiter::TopLevelGoal::Hunt;
		});
	};
	assert(!hunt(candidates).feasible);
	assert(std::none_of(candidates.begin(), candidates.end(), [](const auto& candidate) { return candidate.feasible; }));
	for (auto option : {PlayerBotPreparationOption::RestoreHealth, PlayerBotPreparationOption::Regenerate,
	                    PlayerBotPreparationOption::BuyFood}) {
		snapshot.preparations = {option};
		candidates = planner.candidates(snapshot);
		assert(!hunt(candidates).feasible);
		auto chosen = std::max_element(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
			return (left.feasible ? left.utility : 0) < (right.feasible ? right.utility : 0);
		});
		assert(chosen->goal == PlayerBotGoalArbiter::TopLevelGoal::Prepare && chosen->preparation == option);
	}
	snapshot.preparations = {PlayerBotPreparationOption::RestoreHealth, PlayerBotPreparationOption::Regenerate};
	candidates = planner.candidates(snapshot);
	const auto restore = std::find_if(candidates.begin(), candidates.end(), [](const auto& candidate) {
		return candidate.preparation == PlayerBotPreparationOption::RestoreHealth;
	});
	const auto regenerate = std::find_if(candidates.begin(), candidates.end(), [](const auto& candidate) {
		return candidate.preparation == PlayerBotPreparationOption::Regenerate;
	});
	assert(restore->feasible && regenerate->feasible && restore->utility > regenerate->utility);
	snapshot.huntReadiness.state = PlayerBotReadiness::Pending;
	snapshot.preparations.clear();
	assert(hunt(planner.candidates(snapshot)).feasible); // Request evidence, not executable travel.
	snapshot.huntReadiness.state = PlayerBotReadiness::Blocked;
	snapshot.huntReadiness.requirement->kind = PlayerBotRequirementKind::Stock;
	snapshot.preparations = {PlayerBotPreparationOption::ServiceStock};
	assert(!hunt(planner.candidates(snapshot)).feasible);
	assert(planner.candidates(snapshot)[1].feasible);
	snapshot.preparations = {PlayerBotPreparationOption::RecoveryIncome};
	assert(hunt(planner.candidates(snapshot)).feasible);

	PlayerBotPreparationBudget budget;
	assert(!budget.completed(10));
	assert(!budget.completed(10)); // Currency or unrelated stock identity is not a gap change.
	assert(!budget.completed(9)); // Genuine requirement improvement resets attempts.
	assert(!budget.completed(10)); // A regression does not reset the high-water mark.
	assert(budget.completed(9));
	assert(!budget.completed(8)); // Successive higher health targets remain genuine improvement.
	assert(!budget.completed(8, true)); // First verified regeneration is new evidence.
	assert(!budget.completed(8, false));
	assert(budget.completed(8, true)); // Eating again is not another first observation.
	budget.reset();
	assert(!budget.completed(0, false, PlayerBotRequirementKind::Stock));
	assert(!budget.completed(10)); // Health and stock gaps must not share a numerical scale.
	assert(!budget.completed(9)); // Genuine health improvement still resets after stock work.
	assert(!budget.completed(0, false, PlayerBotRequirementKind::Stock));
	assert(budget.completed(9)); // Switching kinds without improvement cannot loop.
	budget.reset();
	assert(!budget.completed(0)); assert(!budget.completed(0)); assert(budget.completed(0));

	auto region = routeFixture(75);
	region.supplyRecovery = true;
	region.coinGoldPerMinute = 1;
	region.currentHealth = 180;
	region.maximumHealth = 195;
	region.recoveryRouteHealthLoss = 20;
	region.predictedFightSeconds = 10;
	region.combatFraction = 0.2;
	region.expectedDamagePerSecond = 0.2;
	region.supplyProfile.potions = 2;
	region.fitSupplyWindow(2);
	assert(!region.productiveWindow());
	const auto requirement = region.healthPreparation(2);
	assert(requirement && !requirement->observeRegeneration && requirement->health > 180 && requirement->health < 195);
	auto evaluate = [](PlayerBotHuntRegion candidate, bool incompleteAlternative = false) {
		std::vector<PlayerBotHuntRegion> candidates{candidate};
		if (incompleteAlternative) candidates.insert(candidates.begin(), routeFixture(76));
		PlayerBotHuntRouteSelection selection(std::move(candidates));
		if (incompleteAlternative) {
			auto unknown = routeFacts(false);
			unknown.searchIncomplete = true;
			selection.observe(selection.next(), unknown);
		}
		auto facts = routeFacts();
		facts.supplyProfile = candidate.supplyProfile;
		facts.potionReserve = 2;
		facts.recoveryRouteHealthLoss = 20;
		facts.approaches = {Position(200, 100, 7)};
		PlayerBotHuntRouteResult result;
		for (int turn = 0; turn < 8 && !result.terminal; ++turn)
			result = selection.observe(selection.next(), facts);
		assert(result.terminal);
		return result;
	};
	const auto mixed = evaluate(region, true);
	assert(mixed.outcome == PlayerBotHuntRouteOutcome::NeedsPreparation && mixed.readiness.requirement);
	assert(mixed.readiness.state == PlayerBotReadiness::Blocked && mixed.rejectedVariants.empty());
	assert(mixed.failureCounts.at("route_search_incomplete") == 1);
	const auto blocked = evaluate(region);
	assert(blocked.outcome == PlayerBotHuntRouteOutcome::NeedsPreparation && !blocked.selectedRouteRegion);
	assert(blocked.readiness.state == PlayerBotReadiness::Blocked && blocked.readiness.requirement);
	PlayerBotHuntSupplyHandoff handoff;
	handoff.blocked(blocked.readiness, {});
	assert(handoff.readiness().state == PlayerBotReadiness::Blocked);
	handoff.replan();
	assert(handoff.readiness().state == PlayerBotReadiness::Pending && !handoff.readiness().requirement);
	// An actually observed regeneration profile is evaluated anew, not treated
	// as a cached promise from purchasing or carrying food.
	auto fed = region;
	fed.supplyProfile.regenerationSeconds = 180;
	fed.supplyProfile.healthGain = 5;
	fed.supplyProfile.healthInterval = 4;
	assert(evaluate(fed).outcome == PlayerBotHuntRouteOutcome::Selected);
	region.currentHealth = requirement->health;
	region.fitSupplyWindow(2);
	assert(region.productiveWindow() && region.recoverySustainable());
	assert(!region.healthPreparation(2)); // Successful shortened hunts need no preparation.
	region.currentHealth = 180;
	region.expectedDamagePerSecond = 100;
	region.fitSupplyWindow(2);
	const auto unknownFoodEffect = region.healthPreparation(2);
	assert(unknownFoodEffect && unknownFoodEffect->observeRegeneration);
	region.supplyProfile.regenerationSeconds = 180;
	region.fitSupplyWindow(2);
	assert(!region.healthPreparation(2)); // Observing food alone does not promise a fitting hunt.
	region.supplyProfile.regenerationSeconds = 0;
	region.recoveryRouteHealthLoss = 40;
	assert(!region.healthPreparation(2)); // Food cannot overcome a route above the maximum 80%-floor margin.
}

void huntSupplyServiceReplayContracts()
{
	PlayerBotHuntPlanningProfile profile;
	profile.combat.level = 9;
	profile.combat.maximumHealth = 195;
	profile.combat.attack = 25;
	profile.combat.attackSkill = 40;
	profile.supplyCapabilityDefense = 10;
	profile.equipmentItemIds[5] = 2389; // wielded spear; count is stock, not equipment identity
	profile.equipmentItemIds[6] = 2512;
	profile.equipmentItemIds[10] = 2148; // currency in the legacy ammo slot
	profile.supply.potionHealing = 125;
	profile.supply.maximumMana = 50;
	auto stocks = [](uint32_t spears) -> PlayerBotSupplyStocks {
		return {{{PlayerBotSupplyKind::HealthPotion, 7618, 2, 1, 2}, 2},
		        {{PlayerBotSupplyKind::ManaPotion, 7620, 2, 1, 20}, 2},
		        {{PlayerBotSupplyKind::ThrowingWeapon, 2389, 3, 1, 3}, spears}};
	};
	auto facts = [&](uint64_t money, uint64_t bank, uint32_t spears, uint16_t slottedMoney) {
		auto current = profile;
		current.equipmentItemIds[10] = slottedMoney;
		PlayerBotHuntSupplyFacts result;
		result.capability = playerBotSupplyCapability(current);
		result.funds = money + bank;
		result.stockKey = playerBotSupplyStockKey(stocks(spears));
		result.health = 180;
		return result;
	};
	const auto start = facts(11, 0, 3, 2148);
	assert(start.sameDependencies(facts(0, 11, 3, 0)));
	assert(start.sameDependencies(facts(15, 0, 4, 2148)));
	assert(start.sameDependencies(facts(0, 100, 3, 2152)));
	assert(start.sameDependencies(facts(0, 10000, 3, 2160)));
	// Real ammunition and weapon replacements still change loadout identity.
	assert(!start.sameDependencies(facts(15, 0, 3, 2544)));
	auto replacement = start;
	replacement.capability.equipmentItemIds[5] = 2399;
	assert(!start.sameDependencies(replacement));

	PlayerBotHuntSupplyHandoff handoff;
	PlayerBotSupplyRecoveryState recovery;
	handoff.require({{PlayerBotSupplyKind::HealthPotion, 0, 3}}, start);
	assert(!handoff.noProgress(PlayerBotHuntRouteOutcome::NeedsSupplies, start));
	recovery.enter();
	auto service = [&](const PlayerBotHuntSupplyFacts& current, uint32_t spears) {
		assert(!handoff.refresh(current));
		auto carried = stocks(spears);
		playerBotRequireSupplies(carried, handoff.requirements());
		assert(carried.front().rule.safetyFloor == 3 && carried.front().rule.target == 3);
		const auto budget = playerBotSupplyFloorSpendingReserve(0, {{2, carried.front().rule.safetyFloor, 45}});
		assert(budget == 45);
		recovery.restockBlocked(current.funds, current.stockKey);
		recovery.update(current.funds, budget);
		assert(recovery.active()); // Never clear against the generic health floor of two.
		assert(!handoff.noProgress(PlayerBotHuntRouteOutcome::Pending, current));
	};
	service(facts(15, 0, 3, 2148), 3); // hat sale genuinely adds four gold
	service(facts(15, 0, 4, 2148), 4); // depot withdraw merges one spear
	service(facts(15, 0, 3, 2148), 3); // redeposit restores the same stock
	service(facts(0, 15, 3, 0), 3); // deposit all
	service(facts(15, 0, 3, 2148), 3); // withdraw all
	const auto repeated = facts(15, 0, 3, 2148);
	assert(!handoff.noProgress(PlayerBotHuntRouteOutcome::NeedsSupplies, repeated)); // genuinely new funds
	for (int trip = 0; trip < 2; ++trip) {
		service(facts(15, 0, 4, 2148), 4);
		service(facts(15, 0, 3, 2148), 3);
		service(facts(0, 15, 3, 0), 3);
		service(repeated, 3);
		assert(handoff.noProgress(PlayerBotHuntRouteOutcome::NeedsSupplies, repeated) == (trip == 1));
	}
	// Even a completed failure at transient higher stock must not erase the
	// previous completed failure when the next trip restores that old stock.
	PlayerBotHuntSupplyHandoff alternating;
	assert(!alternating.noProgress(PlayerBotHuntRouteOutcome::NeedsSupplies, repeated));
	assert(!alternating.noProgress(PlayerBotHuntRouteOutcome::NeedsSupplies, facts(15, 0, 4, 2148)));
	assert(!alternating.noProgress(PlayerBotHuntRouteOutcome::NeedsSupplies, repeated));
	assert(!alternating.noProgress(PlayerBotHuntRouteOutcome::NeedsSupplies, facts(15, 0, 4, 2148)));
	assert(alternating.noProgress(PlayerBotHuntRouteOutcome::NeedsSupplies, repeated));
	assert(!alternating.noProgress(PlayerBotHuntRouteOutcome::NeedsSupplies, facts(45, 0, 3, 2148)));
}

void huntSupplyHandoffContracts()
{
	auto candidate = routeFixture(1);
	candidate.predictedFightSeconds = 10;
	candidate.combatFraction = 1;
	candidate.expectedDamagePerSecond = 0;
	candidate.supplyProfile.potionHealing = 100;
	candidate.supplyProfile.potions = 2;
	candidate.supplyProfile.reserve = 2;
	candidate.maximumHuntSeconds = 2400;
	candidate.availableHuntSeconds = 2400;
	auto select = [&](uint32_t stock, bool supplierSafe = true, bool recovering = false, bool broke = false,
	                  bool incompleteAlternative = false) {
		auto region = candidate;
		region.supplyRecovery = recovering;
		region.coinGoldPerMinute = 1;
		region.currentHealth = region.maximumHealth = 100;
		std::vector<PlayerBotHuntRegion> candidates{region};
		if (incompleteAlternative) candidates.insert(candidates.begin(), routeFixture(99));
		PlayerBotHuntRouteSelection selector(std::move(candidates));
		if (incompleteAlternative) {
			auto unknown = routeFacts(false);
			unknown.searchIncomplete = true;
			selector.observe(selector.next(), unknown);
		}
		auto facts = routeFacts();
		facts.huntDurationSeconds = 2400;
		facts.supplyProfile = region.supplyProfile;
		facts.supplyProfile.potions = stock;
		facts.potionReserve = 2;
		if (broke) {
			facts.recoverySpendingReserve = facts.funds + 1;
			facts.fare = 1; // Free travel remains eligible below the reserve.
		}
		facts.approaches = {Position(200, 100, 7)};
		PlayerBotHuntRouteResult result;
		for (int turn = 0; turn < 16 && !result.terminal; ++turn) {
			auto request = selector.next();
			auto observation = facts;
			if (request.stage == PlayerBotHuntRouteStage::DiscoverSupply)
				observation.approaches = {Position(300, 100, 7)};
			if (request.stage == PlayerBotHuntRouteStage::Supplier) observation.reached = supplierSafe;
			result = selector.observe(request, observation);
		}
		assert(result.terminal);
		return result;
	};
	const auto mixedShortage = select(2, true, false, false, true);
	assert(mixedShortage.outcome == PlayerBotHuntRouteOutcome::NeedsSupplies && mixedShortage.supplyShortage);
	assert(mixedShortage.readiness.state == PlayerBotReadiness::Blocked && mixedShortage.rejectedVariants.empty());
	const auto shortage = select(2);
	assert(shortage.outcome == PlayerBotHuntRouteOutcome::NeedsSupplies && shortage.supplyShortage);
	assert(!shortage.selectedRouteRegion && shortage.rejectedVariants.empty());
	assert(shortage.failureCounts.at("hunt_supply_window_unavailable") == 1);
	assert(shortage.supplyShortage->requirements.front().count == 3); // reserve + routine, even at zero DPS
	assert(select(2, false).outcome == PlayerBotHuntRouteOutcome::Unavailable); // no unsafe supplier proof
	assert(select(3).outcome == PlayerBotHuntRouteOutcome::Selected);
	const auto economy = select(3, true, false, true);
	assert(economy.outcome == PlayerBotHuntRouteOutcome::Unavailable && economy.rejectedVariants.empty());
	assert(economy.failureCounts.at("travel_fare_breaks_recovery_reserve") == 1);
	const auto unaffordableShortage = select(2, true, false, true);
	assert(unaffordableShortage.outcome == PlayerBotHuntRouteOutcome::Unavailable);
	assert(!unaffordableShortage.supplyShortage && unaffordableShortage.rejectedVariants.empty());
	assert(unaffordableShortage.failureCounts.at("travel_fare_breaks_recovery_reserve") == 1);
	assert(select(2, true, true).outcome == PlayerBotHuntRouteOutcome::Selected); // existing safe recovery

	PlayerBotSupplyStocks stocks{{{PlayerBotSupplyKind::HealthPotion, 7618, 2, 1, 2}, 2}};
	playerBotRequireSupplies(stocks, shortage.supplyShortage->requirements);
	assert(stocks.front().rule.target == 3 && stocks.front().rule.safetyFloor == 3);
	assert(playerBotMandatorySupplyDeficit(stocks, false).missing == 1);
	PlayerBotSupplyRecoveryState recovery;
	recovery.deferRestock(11, playerBotSupplyStockKey(stocks));
	recovery.enter();
	const auto budget = playerBotSupplyFloorSpendingReserve(0, {{2, stocks.front().rule.safetyFloor, 45}});
	recovery.update(11, budget);
	assert(recovery.active() && recovery.restockBlocked(11, playerBotSupplyStockKey(stocks)));
	recovery.restockBlocked(50, playerBotSupplyStockKey(stocks));
	recovery.update(50, budget);
	assert(!recovery.active()); // changed funds allow service, not a generic-floor clear

	PlayerBotHuntSupplyHandoff handoff;
	PlayerBotHuntSupplyFacts facts;
	facts.stockKey = playerBotSupplyStockKey(stocks);
	handoff.require(shortage.supplyShortage->requirements, facts);
	for (int i = 0; i < 100; ++i)
		assert(!handoff.noProgress(PlayerBotHuntRouteOutcome::Pending, facts));
	assert(!handoff.noProgress(PlayerBotHuntRouteOutcome::NeedsSupplies, facts));
	assert(!handoff.noProgress(PlayerBotHuntRouteOutcome::Unavailable, facts));
	assert(handoff.noProgress(PlayerBotHuntRouteOutcome::NeedsSupplies, facts));
	++facts.funds;
	assert(!handoff.noProgress(PlayerBotHuntRouteOutcome::NeedsSupplies, facts));
	++facts.stockKey;
	assert(!handoff.noProgress(PlayerBotHuntRouteOutcome::Unavailable, facts));
	handoff.refresh(facts);
	assert(!handoff.requirements().empty()); // shopping keeps mandatory stock
	++facts.capability.level;
	handoff.refresh(facts);
	assert(handoff.requirements().empty()); // stale combat dependency discarded
	handoff.require(shortage.supplyShortage->requirements, facts);
	handoff.selected(true); // Accepting a recovery hunt does not erase its funding goal.
	assert(!handoff.requirements().empty());
	recovery.deferRestock(11, facts.stockKey);
	recovery.enter();
	recovery.restockBlocked(12, facts.stockKey);
	recovery.update(12, budget);
	assert(recovery.active()); // New income is still below the route requirement.
	assert(!handoff.noProgress(PlayerBotHuntRouteOutcome::Selected, facts));
	assert(handoff.requirements().empty());

	// Demand for one fight, not 2400 seconds. Current low stock still selects
	// the same verified shortened window instead of taking a service trip.
	candidate.expectedDamagePerSecond = 5;
	candidate.supplyProfile.potions = 3;
	candidate.reconcileSupplies(2);
	const auto minimum = candidate.minimumProductiveSupplies(2);
	assert(minimum.front().count == 3);
	candidate.fitSupplyWindow(2);
	assert(candidate.productiveWindow() && candidate.supplyBudget.fits && candidate.availableHuntSeconds < 2400);
	const auto shortened = select(3);
	assert(shortened.outcome == PlayerBotHuntRouteOutcome::Selected && !shortened.supplyShortage);
	assert(shortened.selectedRouteRegion->availableHuntSeconds < 2400);

	// Typed demand includes detailed transit exposure and learned rates. A
	// shortage at a reserve boundary still requires its item-specific supplier.
	candidate.supplyProfile.kinds = {{PlayerBotSupplyKind::ThrowingWeapon, 2389, 1, 1, {0.2, 1}}};
	candidate.routeValidated = true;
	candidate.estimatedTravelSeconds = 20;
	candidate.recoveryRouteHealthLoss = 50;
	const auto typedMinimum = candidate.minimumProductiveSupplies(2);
	assert(typedMinimum.size() == 2 && typedMinimum[1].itemId == 2389);
	assert(typedMinimum[1].count == 5); // one fight plus ten seconds of transit combat
	const auto typedShortage = select(2);
	assert(typedShortage.outcome == PlayerBotHuntRouteOutcome::NeedsSupplies &&
	       typedShortage.supplyShortage->requirements.size() == 2 && typedShortage.rejectedVariants.empty());
}

void routeSelectionContracts()
{
	const Position depot(200, 100, 7), nextDepot(201, 100, 7), supplier(300, 100, 7);
	for (const bool returnLeg : {false, true}) {
		PlayerBotHuntRouteSelection incomplete({routeFixture(99)});
		if (returnLeg) {
			auto outbound = routeFacts(); outbound.approaches = {depot, nextDepot};
			incomplete.observe(incomplete.next(), outbound);
		}
		// Exercise the adapter's aggregation policy, not merely a manually set
		// selector flag: an unsafe reached walk plus exhausted NPC work is unknown.
		const auto aggregate = playerBotHuntAggregateRouteResult(PlayerBotNavigationResult::Reached, 1000, 0, true);
		auto unknown = routeFacts(aggregate == PlayerBotNavigationResult::Reached);
		unknown.dangerCost = 1000;
		unknown.searchIncomplete = aggregate == PlayerBotNavigationResult::NodeLimit;
		auto result = incomplete.observe(incomplete.next(), unknown);
		if (returnLeg) result = incomplete.observe(incomplete.next(), routeFacts(false));
		assert(result.completedCandidate && result.completedCandidate->rejectionReason ==
		    (returnLeg ? "depot_route_search_incomplete" : "route_search_incomplete"));
		result = incomplete.observe(incomplete.next(), {});
		assert(result.terminal && result.rejectedVariants.empty());
		assert(result.outcome == PlayerBotHuntRouteOutcome::Incomplete && result.readiness.state == PlayerBotReadiness::Pending);
	}
	PlayerBotHuntSupplyHandoff exhausted;
	PlayerBotHuntSupplyFacts changing;
	for (int attempt = 1; attempt <= 3; ++attempt) {
		for (int yield = 0; yield < 100; ++yield) assert(!exhausted.noProgress(PlayerBotHuntRouteOutcome::Pending, changing));
		++changing.funds; ++changing.stockKey; ++changing.health;
		exhausted.blocked({PlayerBotReadiness::Pending, std::nullopt}, changing);
		assert(exhausted.noProgress(PlayerBotHuntRouteOutcome::Incomplete, changing) == (attempt == 3));
	}
	exhausted.selected();
	assert(!exhausted.noProgress(PlayerBotHuntRouteOutcome::Incomplete, changing));
	// Outbound failures remain distinct; a later candidate can still win.
	PlayerBotHuntRouteSelection selection({routeFixture(1), routeFixture(2), routeFixture(3), routeFixture(4)});
	auto request = selection.next();
	auto result = selection.observe(request, routeFacts(false));
	assert(result.completedCandidate && result.yield && result.completedCandidate->rejectionReason == "route_unreachable");
	request = selection.next();
	auto unsafe = routeFacts(); unsafe.dangerCost = 501;
	result = selection.observe(request, unsafe);
	assert(result.completedCandidate && result.completedCandidate->rejectionReason == "route_danger_above_tolerance");
	request = selection.next(); unsafe.dangerCost = 0; unsafe.peakDanger = 0.081;
	result = selection.observe(request, unsafe);
	assert(result.completedCandidate && result.completedCandidate->rejectionReason == "route_peak_danger_above_tolerance");
	request = selection.next();
	auto outbound = routeFacts(); outbound.approaches = {depot}; outbound.travelSeconds = 5;
	result = selection.observe(request, outbound);
	assert(result.accepted && result.yield && !result.completedCandidate);
	request = selection.next(); assert(request.stage == PlayerBotHuntRouteStage::Depot && request.to == depot);
	result = selection.observe(request, routeFacts());
	assert(result.accepted && !result.yield && !result.terminal);
	request = selection.next(); assert(request.stage == PlayerBotHuntRouteStage::Final);
	result = selection.observe(request, routeFacts());
	assert(result.terminal && result.selectedRouteRegion && result.selectedRouteRegion->atlasVariantId == 4);
	assert(result.failureCounts.at("route_unreachable") == 1 &&
	       result.failureCounts.at("route_danger_above_tolerance") == 1 &&
	       result.failureCounts.at("route_peak_danger_above_tolerance") == 1 && result.rejectedVariants.size() == 3);

	// Depot search tries another approach; supplier failure never backtracks.
	PlayerBotHuntRouteSelection supply({routeFixture(1), routeFixture(2)});
	request = supply.next(); outbound.approaches = {depot, nextDepot};
	supply.observe(request, outbound);
	request = supply.next(); assert(request.to == depot);
	result = supply.observe(request, routeFacts(false));
	assert(result.yield);
	request = supply.next(); assert(request.to == nextDepot);
	auto recovery = routeFacts(); recovery.potionReserve = 10; recovery.supplyProfile.potions = 0;
	supply.observe(request, recovery);
	request = supply.next(); assert(request.stage == PlayerBotHuntRouteStage::DiscoverSupply);
	auto discovery = routeFacts(); discovery.approaches = {supplier};
	supply.observe(request, discovery);
	request = supply.next(); assert(request.stage == PlayerBotHuntRouteStage::Supplier && request.to == supplier);
	result = supply.observe(request, routeFacts(false));
	assert(result.yield);
	request = supply.next(); assert(request.stage == PlayerBotHuntRouteStage::RejectSupply);
	recovery.supplyProfile.potions = 100; // last supplier failure cannot be undone by a later profile
	result = supply.observe(request, recovery);
	assert(result.completedCandidate && result.completedCandidate->routeValidated &&
	       result.completedCandidate->rejectionReason == "recovery_supply_route_unavailable");
	request = supply.next(); assert(request.stage == PlayerBotHuntRouteStage::Outbound);
	outbound.approaches.clear(); supply.observe(request, outbound);
	request = supply.next(); assert(request.stage == PlayerBotHuntRouteStage::Depot);
	result = supply.observe(request, routeFacts(false));
	assert(result.completedCandidate && result.yield &&
	       result.completedCandidate->rejectionReason == "safe_depot_exit_unavailable");
	request = supply.next(); result = supply.observe(request, routeFacts());
	assert(result.terminal && !result.selectedRouteRegion && result.failureCounts.size() == 2);

	// The depot-time decision is retained: a later supply refresh does not
	// introduce a new supplier requirement after the depot route was accepted.
	PlayerBotHuntRouteSelection refreshed({routeFixture(6)});
	request = refreshed.next(); outbound.approaches = {depot}; refreshed.observe(request, outbound);
	request = refreshed.next(); refreshed.observe(request, routeFacts());
	request = refreshed.next(); assert(request.stage == PlayerBotHuntRouteStage::Final);
	auto depleted = routeFacts(); depleted.supplyProfile.potions = 0;
	result = refreshed.observe(request, depleted);
	assert(result.terminal && result.selectedRouteRegion &&
	       result.selectedRouteRegion->supplyDestination == Position() && result.selectedRouteRegion->routeValidated);

	PlayerBotHuntRouteSelection noSupplier({routeFixture(8)});
	request = noSupplier.next(); noSupplier.observe(request, outbound);
	request = noSupplier.next(); auto shortage = routeFacts(); shortage.supplyProfile.potions = 0;
	noSupplier.observe(request, shortage);
	request = noSupplier.next(); assert(request.stage == PlayerBotHuntRouteStage::DiscoverSupply);
	result = noSupplier.observe(request, routeFacts());
	assert(result.completedCandidate && result.yield && result.completedCandidate->routeValidated &&
	       result.completedCandidate->rejectionReason == "recovery_supply_route_unavailable" &&
	       noSupplier.next().stage == PlayerBotHuntRouteStage::Done);

	// A supplier is only needed for a non-recovery hunt with a deficit. The
	// selected fare must cover all legs, while zero fares bypass unknown prices.
	for (bool recoveryMode : {false, true}) {
		recovery.supplyProfile.potions = 0;
		auto candidate = routeFixture(7); candidate.supplyRecovery = recoveryMode;
		if (recoveryMode) { candidate.coinGoldPerMinute = 1; candidate.currentHealth = candidate.maximumHealth = 100; }
		PlayerBotHuntRouteSelection selector({candidate});
		request = selector.next(); outbound.approaches = {depot};
		outbound.fare = recoveryMode ? 0 : 10;
		selector.observe(request, outbound);
		request = selector.next(); recovery.fare = recoveryMode ? 0 : 10;
		selector.observe(request, recovery);
		request = selector.next();
		assert(request.stage == (recoveryMode ? PlayerBotHuntRouteStage::Final : PlayerBotHuntRouteStage::DiscoverSupply));
		if (!recoveryMode) {
			selector.observe(request, discovery);
			request = selector.next(); auto paid = routeFacts(); paid.fare = 11;
			selector.observe(request, paid);
			request = selector.next();
		}
		auto final = routeFacts(); final.recoverySpendingReserve = recoveryMode ? UINT64_MAX : 69;
		result = selector.observe(request, final);
		assert(result.terminal && result.selectedRouteRegion && result.selectedRouteRegion->routeValidated);
	}
	for (uint64_t reserve : {uint64_t{79}, uint64_t{80}, uint64_t{81}, UINT64_MAX}) {
		PlayerBotHuntRouteSelection selector({routeFixture(9)});
		request = selector.next(); outbound.approaches = {depot}; outbound.fare = 10;
		selector.observe(request, outbound);
		request = selector.next(); auto paid = routeFacts(); paid.fare = 10; selector.observe(request, paid);
		request = selector.next(); auto final = routeFacts(); final.funds = 100;
		final.recoverySpendingReserve = reserve;
		result = selector.observe(request, final);
		if (reserve > 80) {
			assert(result.completedCandidate && result.completedCandidate->rejectionReason ==
			       "travel_fare_breaks_recovery_reserve");
			request = selector.next(); result = selector.observe(request, routeFacts());
		}
		assert(result.terminal && static_cast<bool>(result.selectedRouteRegion) == (reserve <= 80));
	}
	// Travel no longer consumes productive XP time. Honest full-duration bounds
	// can prune weaker XP alternatives when economic progress is tied.
	PlayerBotHuntRouteSelection ranking({routeFixture(1, 100), routeFixture(2, 99),
	                                     routeFixture(3, 10)});
	request = ranking.next(); outbound.fare = 0; outbound.travelSeconds = 40;
	outbound.approaches = {depot}; ranking.observe(request, outbound);
	request = ranking.next(); ranking.observe(request, routeFacts());
	request = ranking.next(); result = ranking.observe(request, routeFacts());
	assert(result.completedCandidate && result.terminal && result.selectedRouteRegion &&
	       result.selectedRouteRegion->atlasVariantId == 1);
	assert(ranking.next().stage == PlayerBotHuntRouteStage::Done); // low optimistic bound is pruned

	// Stable ties preserve the first candidate; a live supply profile can
	// reorder a previously scored tie without changing the initial shortlist.
	PlayerBotHuntRouteSelection ties({routeFixture(4), routeFixture(5)});
	request = ties.next(); ties.observe(request, outbound);
	request = ties.next(); ties.observe(request, routeFacts());
	request = ties.next(); result = ties.observe(request, routeFacts());
	assert(!result.terminal);
	request = ties.next(); ties.observe(request, outbound);
	request = ties.next(); ties.observe(request, routeFacts());
	request = ties.next(); result = ties.observe(request, routeFacts());
	assert(result.terminal && result.selectedRouteRegion->atlasVariantId == 4);

	// A verified supplier does not claim the bot already carries potions:
	// live supply fit still decides the winner after routing.
	auto hungry = routeFixture(11, 100), stocked = routeFixture(12, 90);
	hungry.expectedDamagePerSecond = 1;
	hungry.combatFraction = 1;
	hungry.supplyProfile.potionHealing = 100;
	stocked.optimisticProjectedExperience = 100;
	PlayerBotHuntRouteSelection supplyRank({hungry, stocked});
	request = supplyRank.next(); outbound.travelSeconds = 5; supplyRank.observe(request, outbound);
	request = supplyRank.next(); auto deficit = routeFacts(); deficit.supplyProfile.potions = 0;
	deficit.supplyProfile.potionHealing = 100;
	supplyRank.observe(request, deficit);
	request = supplyRank.next(); supplyRank.observe(request, discovery);
	request = supplyRank.next(); supplyRank.observe(request, routeFacts());
	request = supplyRank.next(); result = supplyRank.observe(request, deficit);
	assert(result.completedCandidate && !result.terminal && !result.completedCandidate->productiveWindow() &&
	       result.completedCandidate->rejectionReason == "hunt_supply_window_unavailable");
	request = supplyRank.next(); assert(request.to == stocked.destination);
	supplyRank.observe(request, outbound);
	request = supplyRank.next(); supplyRank.observe(request, routeFacts());
	request = supplyRank.next(); result = supplyRank.observe(request, routeFacts());
	assert(result.terminal && result.selectedRouteRegion && result.selectedRouteRegion->atlasVariantId == 12);

	// Retry unsafe supplier approaches without abandoning the first safe depot.
	PlayerBotHuntRouteSelection supplierRetry({routeFixture(13)});
	request = supplierRetry.next();
	outbound = routeFacts(); outbound.approaches = {depot, nextDepot};
	supplierRetry.observe(request, outbound);
	request = supplierRetry.next();
	auto lowStock = routeFacts(); lowStock.supplyProfile.potions = 0;
	supplierRetry.observe(request, lowStock);
	request = supplierRetry.next();
	const Position secondSupplier(301, 100, 7);
	discovery.approaches = {supplier, secondSupplier};
	assert(supplierRetry.observe(request, discovery).yield);
	request = supplierRetry.next();
	auto unsafeSupplier = routeFacts(); unsafeSupplier.peakDanger = 0.081;
	result = supplierRetry.observe(request, unsafeSupplier);
	assert(result.yield && !result.completedCandidate);
	request = supplierRetry.next();
	assert(request.stage == PlayerBotHuntRouteStage::Supplier && request.from == depot && request.to == secondSupplier);
	auto safeSupplier = routeFacts(); safeSupplier.fare = 5; safeSupplier.npcTravel = true;
	assert(!supplierRetry.observe(request, safeSupplier).yield);
	request = supplierRetry.next();
	result = supplierRetry.observe(request, routeFacts());
	assert(result.terminal && result.selectedRouteRegion && result.failureCounts.empty());
	assert(result.selectedRouteRegion->exitDepotDestination == depot &&
	       result.selectedRouteRegion->supplyDestination == secondSupplier &&
	       result.selectedRouteRegion->supplyFare == 5 && result.selectedRouteRegion->supplyNpcTravel);

	assert(playerBotHuntTravelAffordable(100, 70, 10, 10, 10));
	assert(!playerBotHuntTravelAffordable(100, 71, 10, 10, 10));
	assert(!playerBotHuntTravelAffordable(UINT64_MAX, UINT64_MAX, UINT64_MAX, 1, 0));
}

void typedSupplyRouteContracts()
{
	const Position depot(200, 100, 7), supplier(300, 100, 7);
	const PlayerBotSupplyRule mana = playerBotSupplyRule(PlayerBotSupplyKind::ManaPotion, 3);
	const PlayerBotSupplyRule spears = playerBotThrowingWeaponRule(2389, 16);
	const PlayerBotSupplyRule ammo{PlayerBotSupplyKind::Ammunition, 2544, 6, 5, 40};
	const uint16_t healthItemId = 7618;
	const PlayerBotSupplyRule health{PlayerBotSupplyKind::HealthPotion, healthItemId, 2, 1, 20};
	// Shop admission is per-kind, not vocation-specific. A potion shop need
	// not sell weapons already at their floors, even below their targets.
	PlayerBotSupplyStocks stocks{{health, 20}, {mana, 0}, {spears, spears.safetyFloor}, {ammo, ammo.safetyFloor}};
	auto items = playerBotSupplyExitShopItems(stocks, healthItemId);
	assert((items == std::vector<uint16_t>{healthItemId, mana.itemId}));
	stocks[1].count = mana.safetyFloor;
	assert((playerBotSupplyExitShopItems(stocks, healthItemId) == std::vector<uint16_t>{healthItemId}));
	stocks[2].count = spears.safetyFloor - 1;
	assert((playerBotSupplyExitShopItems(stocks, healthItemId) == std::vector<uint16_t>{healthItemId, spears.itemId}));
	stocks[2].rule.target = 0; // inactive kinds are not shop requirements
	assert((playerBotSupplyExitShopItems(stocks, healthItemId) == std::vector<uint16_t>{healthItemId}));
	assert((playerBotSupplyExitShopItems({}, healthItemId) == std::vector<uint16_t>{healthItemId}));

	// Three mana potions are projected, while health has no projected use
	// and is fully stocked. At exact routine coverage no supplier is needed.
	for (uint32_t count : {0U, 1U, 2U, 3U, 4U}) {
		auto candidate = routeFixture(41);
		candidate.availableHuntSeconds = 60;
		candidate.combatFraction = 1;
		candidate.supplyProfile.kinds = {{mana.kind, mana.itemId, count, mana.returnThreshold, {0.05, 1}}};
		PlayerBotHuntRouteSelection selector({candidate});
		auto outbound = routeFacts(); outbound.approaches = {depot};
		selector.observe(selector.next(), outbound);
		auto live = routeFacts();
		// The adapter refreshes stock without learned demand: it must not
		// erase the candidate's typed forecast at the depot or final check.
		live.supplyProfile.kinds = {{mana.kind, mana.itemId, count, mana.returnThreshold, {}}};
		selector.observe(selector.next(), live);
		auto request = selector.next();
		const bool needsRoute = count < 4;
		assert(request.stage == (needsRoute ? PlayerBotHuntRouteStage::DiscoverSupply : PlayerBotHuntRouteStage::Final));
		if (needsRoute) {
			assert(request.requiredSupplyItems == std::vector<uint16_t>{mana.itemId});
			auto discovery = routeFacts(); discovery.approaches = {supplier};
			selector.observe(request, discovery);
			request = selector.next();
			assert(request.stage == PlayerBotHuntRouteStage::Supplier && request.from == depot && request.to == supplier);
			selector.observe(request, routeFacts());
		}
		const auto result = selector.observe(selector.next(), live);
		if (count <= mana.returnThreshold) {
			assert(result.completedCandidate && !result.selectedRouteRegion &&
			       result.completedCandidate->rejectionReason == "hunt_supply_window_unavailable");
			continue;
		}
		assert(result.terminal && result.selectedRouteRegion);
		const auto& selected = *result.selectedRouteRegion;
		assert(selected.supplyBudget.expectedPotions == 0 && selected.supplyKindBudgets.size() == 1);
		assert(selected.availableHuntSeconds == (count - 1) * 20 && selected.supplyBudget.fits);
		assert(selected.supplyKindBudgets[0].expected == count - 1);
		assert(selected.supplyDestination == (needsRoute ? supplier : Position()));
	}

	// A mana-only deficit needs actual supplier access. A health-only shop
	// cannot satisfy a missing active mana floor; unreachable access rejects.
	for (bool offersMana : {false, true}) {
		auto candidate = routeFixture(42);
		candidate.availableHuntSeconds = 60;
		candidate.combatFraction = 1;
		candidate.supplyProfile.kinds = {{mana.kind, mana.itemId, 0, mana.returnThreshold, {0.05, 1}}};
		PlayerBotHuntRouteSelection selector({candidate});
		auto outbound = routeFacts(); outbound.approaches = {depot};
		selector.observe(selector.next(), outbound);
		auto live = routeFacts(); live.supplyProfile.kinds = candidate.supplyProfile.kinds;
		selector.observe(selector.next(), live);
		assert(selector.next().stage == PlayerBotHuntRouteStage::DiscoverSupply);
		auto discovery = routeFacts();
		items = playerBotSupplyExitShopItems({{health, 20}, {mana, 0}, {spears, 7}}, healthItemId);
		for (uint16_t itemId : items) {
			const bool sold = itemId == healthItemId || (offersMana && itemId == mana.itemId);
			discovery.supplyApproachGroups.push_back({itemId, sold ? std::vector<Position>{supplier} : std::vector<Position>{}});
		}
		auto result = selector.observe(selector.next(), discovery);
		if (offersMana) {
			assert(selector.next().stage == PlayerBotHuntRouteStage::Supplier);
			selector.observe(selector.next(), routeFacts(false));
			assert(selector.next().stage == PlayerBotHuntRouteStage::RejectSupply);
			result = selector.observe(selector.next(), live);
		}
		assert(result.completedCandidate && result.completedCandidate->rejectionReason == "recovery_supply_route_unavailable");
	}

	// Potions and spears use separate shops. A projected spear deficit must
	// validate both providers, including the path and fare between them.
	const Position weaponSupplier(400, 100, 7);
	for (int failure : {0, 1, 2, 3}) {
		auto candidate = routeFixture(44);
		candidate.availableHuntSeconds = 60;
		candidate.combatFraction = 1;
		candidate.supplyProfile.kinds = {{spears.kind, spears.itemId, 7, spears.returnThreshold, {0.2, 1}}};
		PlayerBotHuntRouteSelection selector({candidate});
		auto outbound = routeFacts(); outbound.approaches = {depot};
		selector.observe(selector.next(), outbound);
		auto live = routeFacts(); live.supplyProfile.kinds = candidate.supplyProfile.kinds;
		selector.observe(selector.next(), live);
		auto request = selector.next();
		assert(request.stage == PlayerBotHuntRouteStage::DiscoverSupply &&
		       request.requiredSupplyItems == std::vector<uint16_t>{spears.itemId});
		auto discovery = routeFacts();
		discovery.supplyApproachGroups = {{healthItemId, {supplier}},
		    {spears.itemId, failure == 1 ? std::vector<Position>{} : std::vector<Position>{weaponSupplier}}};
		auto result = selector.observe(request, discovery);
		if (failure != 1) {
			request = selector.next();
			assert(request.from == depot && request.to == supplier && request.supplyItemId == healthItemId);
			auto first = routeFacts(); first.fare = 5; first.npcTravel = true;
			assert(selector.observe(request, first).yield);
			request = selector.next();
			assert(request.stage == PlayerBotHuntRouteStage::Supplier && request.from == supplier &&
			       request.to == weaponSupplier && request.supplyItemId == spears.itemId);
			auto second = routeFacts(failure != 2); second.fare = 7;
			selector.observe(request, second);
			if (failure == 2) assert(selector.next().stage == PlayerBotHuntRouteStage::RejectSupply);
			else assert(selector.next().stage == PlayerBotHuntRouteStage::Final);
			if (failure == 3) live.recoverySpendingReserve = live.funds - 11;
			result = selector.observe(selector.next(), live);
		}
		if (failure == 0) {
			assert(result.terminal && result.selectedRouteRegion);
			assert(result.selectedRouteRegion->supplyDestination == weaponSupplier &&
			       result.selectedRouteRegion->supplyFare == 12 && result.selectedRouteRegion->supplyNpcTravel);
		} else {
			assert(result.completedCandidate && !result.completedCandidate->suitable);
			assert(result.completedCandidate->rejectionReason == (failure == 3 ?
			    "travel_fare_breaks_recovery_reserve" : "recovery_supply_route_unavailable"));
		}
	}

	// Recovery may spend its remaining mana stock, or use none when empty.
	// Ordinary hunt admission keeps the same stock floor.
	for (uint32_t count : {0U, 1U}) {
		for (double demand : {0.0, 0.01}) {
			auto candidate = routeFixture(43);
			candidate.availableHuntSeconds = 60;
			candidate.combatFraction = 1;
			candidate.coinGoldPerMinute = 1;
			candidate.currentHealth = candidate.maximumHealth = 100;
			candidate.supplyProfile.kinds = {{mana.kind, mana.itemId, count, mana.returnThreshold, {demand, 1}}};
			candidate.reconcileSupplies(1);
			assert(!candidate.supplyBudget.fits); // zero demand alone cannot waive ordinary floors
			candidate.supplyRecovery = true;
			candidate.supplyProfile.potions = 0;
			candidate.reconcileSupplies(1);
			assert(candidate.supplyBudget.expectedPotions == 0);
			const bool fits = demand == 0 || count > 0;
			assert(candidate.supplyBudget.fits == fits);
			assert(candidate.supplyKindBudgets[0].fits == fits);
			PlayerBotHuntRouteSelection selector({candidate});
			auto outbound = routeFacts(); outbound.approaches = {depot};
			selector.observe(selector.next(), outbound);
			auto live = routeFacts(); live.potionReserve = 1; live.supplyProfile = candidate.supplyProfile;
			selector.observe(selector.next(), live);
			assert(selector.next().stage == PlayerBotHuntRouteStage::Final); // recovery does not demand a supplier
			const auto result = selector.observe(selector.next(), live);
			if (fits) assert(result.terminal && result.selectedRouteRegion && result.selectedRouteRegion->recoverySustainable());
			else assert(result.completedCandidate && result.completedCandidate->rejectionReason == "recovery_hunt_not_sustainable");
		}
	}
	// A recorded zero break/use rate does not mean weapons/ammo are optional.
	for (const auto& rule : {spears, ammo}) {
		for (uint32_t count : {0U, rule.returnThreshold}) {
			auto candidate = routeFixture(44);
			candidate.supplyRecovery = true;
			candidate.availableHuntSeconds = 60;
			candidate.combatFraction = 1;
			candidate.supplyProfile.kinds = {{rule.kind, rule.itemId, count, rule.returnThreshold, {0, 3}}};
			candidate.reconcileSupplies(1);
			assert(candidate.supplyKindBudgets[0].fits == (count > 0));
			assert(candidate.supplyBudget.fits == (count > 0));
		}
	}
}

void selectedRouteRetention()
{
	struct Evidence {
		PlayerBotNavigationRoutePlan plan;
		std::shared_ptr<int> lifetime;
	};
	PlayerBotHuntRouteRetention<Evidence> retained;
	retained.begin(10, 1);
	auto distant = routeFixture(1, 100), nearby = routeFixture(2, 99);
	distant.cashPressure = nearby.cashPressure = true;
	distant.coinGoldPerMinute = nearby.coinGoldPerMinute = 10;
	distant.estimatedTravelSeconds = 30;
	nearby.estimatedTravelSeconds = 10;
	PlayerBotHuntRouteSelection selector({distant, nearby});
	std::weak_ptr<int> firstLifetime, secondLifetime;
	PlayerBotHuntRouteResult result;
	for (int turns = 0; !result.terminal && turns < 12; ++turns) {
		const auto request = selector.next();
		auto facts = routeFacts();
		if (request.stage == PlayerBotHuntRouteStage::Outbound) {
			auto& evidence = retained.outbound(request.sequence);
			// A yield keeps the exact current request; it does not refill evidence.
			assert(&evidence == &retained.outbound(request.sequence));
			evidence.lifetime = std::make_shared<int>(request.to.x);
			const bool first = request.to == routeFixture(1).destination;
			(first ? firstLifetime : secondLifetime) = evidence.lifetime;
			PlayerBotNavigationStep boarding;
			boarding.action = PlayerBotNavigationAction::NpcTravel;
			boarding.npcId = request.to.x;
			boarding.expectedPosition = request.to;
			boarding.price = 5;
			boarding.dialogue = {"hi", "passage", "yes"};
			PlayerBotNavigationStep walk;
			walk.action = PlayerBotNavigationAction::Move;
			walk.target = walk.expectedPosition = Position(101, 99, 7);
			evidence.plan.steps = {walk, boarding};
			evidence.plan.metrics.result = PlayerBotNavigationResult::Reached;
			evidence.plan.metrics.steps = 265; // Full itinerary; only the first leg is executable.
			evidence.plan.metrics.fare = 5;
			facts.travelSeconds = first ? 30 : 10; // Preserve the cheap-scored winner's exact route.
			facts.approaches = {Position(200, 100, 7)};
		}
		result = selector.observe(request, facts);
		assert(result.accepted);
		if (result.completedCandidate) retained.complete(*result.completedCandidate);
	}
	assert(result.terminal && result.selectedRouteRegion && result.selectedRouteRegion->atlasVariantId == 2);
	assert(firstLifetime.expired() && !secondLifetime.expired());
	auto winner = retained.take(*result.selectedRouteRegion);
	assert(winner && winner->plan.steps.size() == 2 && winner->plan.metrics.fare == 5 && winner->plan.metrics.steps == 265);
	assert(winner->plan.steps.front().action == PlayerBotNavigationAction::Move &&
	       winner->plan.steps.front().expectedPosition == Position(101, 99, 7));
	const auto& boarding = winner->plan.steps.back();
	assert(boarding.npcId == 102 && boarding.expectedPosition == routeFixture(2).destination &&
	       boarding.price == 5 && boarding.dialogue == std::vector<std::string>({"hi", "passage", "yes"}));
	assert(!retained.take(*result.selectedRouteRegion)); // One-shot handoff.
	winner.reset();
	assert(secondLifetime.expired());

	auto valid = routeFixture(3);
	valid.routeValidated = true;
	retained.begin(20, 1);
	retained.outbound(1).lifetime = std::make_shared<int>(1);
	std::weak_ptr<int> best = retained.pending()->lifetime;
	retained.complete(valid);
	retained.outbound(2).lifetime = std::make_shared<int>(2);
	std::weak_ptr<int> rejected = retained.pending()->lifetime;
	auto invalid = routeFixture(4, 1000);
	invalid.routeValidated = true;
	invalid.rejectionReason = "travel_fare_breaks_recovery_reserve";
	retained.complete(invalid);
	assert(rejected.expired() && !best.expired()); // Outbound success alone never wins.
	retained.outbound(3).lifetime = std::make_shared<int>(3);
	std::weak_ptr<int> tied = retained.pending()->lifetime;
	retained.complete(valid);
	assert(tied.expired() && !best.expired()); // Match the selector's strict preference.
	auto mismatched = valid;
	++mismatched.atlasVariantId;
	assert(!retained.take(mismatched) && best.expired());
	for (int field = 0; field < 3; ++field) {
		retained.begin(21, 1);
		retained.outbound(1);
		retained.complete(valid);
		mismatched = valid;
		if (field == 0) ++mismatched.id;
		if (field == 1) ++mismatched.atlasRevision;
		if (field == 2) ++mismatched.destination.x;
		assert(!retained.take(mismatched));
	}
	for (int change = 0; change < 3; ++change) {
		retained.begin(30, 1);
		retained.outbound(1).lifetime = std::make_shared<int>(1);
		std::weak_ptr<int> pending = retained.pending()->lifetime;
		retained.complete(valid);
		retained.outbound(2).lifetime = std::make_shared<int>(2);
		std::weak_ptr<int> current = retained.pending()->lifetime;
		if (change == 0) retained.clear(); // Cancellation/terminal lifecycle.
		else retained.begin(change == 1 ? 31 : 30, change == 2 ? 2 : 1);
		assert(pending.expired() && current.expired() && !retained.take(valid));
	}
}

void selectedRouteEvidence()
{
	using Evidence = PlayerBotHuntTravelEvidence;
	using Changes = PlayerBotRouteChanges;
	Evidence evidence;
	evidence.source = Position(100, 100, 7);
	evidence.destination = Position(500, 500, 7);
	evidence.topologyGeneration = 7;
	evidence.riskRevision = 8;
	evidence.actor = Evidence::Actor{10, 150, 20, 20, 20, 20, 1.0f, 500, true, false, true, true,
	    100, 150, false, 0, 1, 1, evidence.source, 100, true};
	auto valid = [&](Evidence& snapshot) {
		return snapshot.validContext(evidence.source, evidence.destination, evidence.actor, 7, 8, evidence.risk);
	};
	assert(valid(evidence));
	assert(!evidence.validContext(Position(101, 100, 7), evidence.destination, evidence.actor, 7, 8, evidence.risk));
	assert(!evidence.validContext(evidence.source, Position(501, 500, 7), evidence.actor, 7, 8, evidence.risk));
	assert(!evidence.validContext(evidence.source, evidence.destination, evidence.actor, 9, 8, evidence.risk));
	assert(!evidence.validContext(evidence.source, evidence.destination, evidence.actor, 7, 9, evidence.risk));
	auto checkActor = [&](const Evidence::Actor& actor) {
		assert(!evidence.validContext(evidence.source, evidence.destination, actor, 7, 8, evidence.risk));
	};
	auto actor = evidence.actor;
	std::get<0>(actor)++; checkActor(actor); // Level/combat.
	actor = evidence.actor; std::get<1>(actor)++; checkActor(actor);
	actor = evidence.actor; std::get<7>(actor)--; checkActor(actor); // Funds/reserve eligibility.
	actor = evidence.actor; std::get<8>(actor) = false; checkActor(actor); // Premium.
	actor = evidence.actor; std::get<9>(actor) = true; checkActor(actor); // PZ lock.
	actor = evidence.actor; std::get<10>(actor) = false; checkActor(actor); // Rope.
	actor = evidence.actor; std::get<11>(actor) = false; checkActor(actor); // Shovel.
	actor = evidence.actor; std::get<12>(actor)++; checkActor(actor); // Speed.
	actor = evidence.actor; std::get<15>(actor)++; checkActor(actor); // Permissions.
	actor = evidence.actor; std::get<16>(actor)++; checkActor(actor); // Guild.
	actor = evidence.actor; std::get<18>(actor) = Position(101, 100, 7); checkActor(actor);
	actor = evidence.actor; std::get<19>(actor) = 76; checkActor(actor); // Distance hit chance.
	actor = evidence.actor; std::get<20>(actor) = false; checkActor(actor);
	for (int field = 0; field < 3; ++field) {
		auto risk = evidence.risk;
		if (field == 0) risk.healthLossCost++;
		if (field == 1) risk.maximumRouteHealthLoss += 0.01;
		if (field == 2) risk.maximumHealthLossPerSecond += 0.01;
		assert(!evidence.validContext(evidence.source, evidence.destination, evidence.actor, 7, 8, risk));
	}

	const Position firstBoat(101, 100, 7), laterBoat(400, 400, 7), laterWalk(450, 450, 7), other(99, 99, 7);
	evidence.paid = true;
	evidence.offers = {{1, firstBoat, Position(300, 300, 7), 10, 8, true, {"hi", "yes"}},
	                   {2, laterBoat, evidence.destination, 20, 8, true, {"hi", "yes"}}};
	evidence.providers.reset({{1, firstBoat}, {2, laterBoat}});
	evidence.providers.use(1);
	evidence.providers.use(2); // A later paid leg must retain its provider anchor too.
	assert(evidence.validOffers(evidence.offers));
	auto offers = evidence.offers;
	std::get<1>(offers[1]).x++;
	assert(evidence.validOffers(offers)); // Tolerate wandering without drifting anchors.
	std::get<1>(offers[1]).x += 2;
	assert(!evidence.validOffers(offers));
	offers = evidence.offers; offers.pop_back(); assert(!evidence.validOffers(offers)); // Removed/opaque/ineligible/failed offer.
	offers = evidence.offers; std::get<2>(offers[1]).x++; assert(!evidence.validOffers(offers));
	offers = evidence.offers; std::get<3>(offers[1])++; assert(!evidence.validOffers(offers));
	offers = evidence.offers; std::get<4>(offers[1])++; assert(!evidence.validOffers(offers));
	offers = evidence.offers; std::get<5>(offers[1]) = false; assert(!evidence.validOffers(offers));
	offers = evidence.offers; std::get<6>(offers[1]).push_back("changed"); assert(!evidence.validOffers(offers));
	assert(evidence.validOffers(evidence.offers));
	// The executable route ends at the first boat, but safety proof reads the
	// later walk as well. Dependencies survive slices and completed snapshots.
	{
		Changes::Scope scope(evidence.watch);
		Changes::read(firstBoat);
	}
	{
		Changes::Scope scope(evidence.watch);
		Changes::read(laterWalk);
	}
	Evidence startedBeforeYield;
	{
		Changes::Scope scope(startedBeforeYield.watch);
		Changes::read(firstBoat);
	}
	Changes::changed(firstBoat);
	{
		Changes::Scope scope(startedBeforeYield.watch);
		Changes::read(laterBoat);
	}
	assert(!startedBeforeYield.watch.valid()); // A resumed scope cannot reset original evidence.
	// Start a fresh independent completed proof for the later-leg checks below.
	evidence.watch = Changes::Watch();
	{
		Changes::Scope scope(evidence.watch);
		Changes::read(firstBoat);
		Changes::read(laterWalk);
	}
	Evidence completed = evidence;
	Changes::changed(other);
	assert(valid(completed));
	Changes::changed(laterWalk);
	assert(!valid(completed) && !valid(evidence));
	assert(!valid(completed)); // Checking stale evidence must not reset it.
	Evidence epoch;
	Changes::invalidate();
	assert(!epoch.watch.valid());
	Evidence overflow;
	for (int i = 0; i < 4097; ++i) Changes::changed(other);
	assert(!overflow.watch.valid());
}

void selectedRouteLegProofs()
{
	using Evidence = PlayerBotHuntTravelEvidence;
	using Changes = PlayerBotRouteChanges;
	using Stage = PlayerBotHuntRouteStage;
	const Position origin(100, 100, 7), hunt(133, 100, 7), depot(200, 100, 7), supplier(300, 100, 7);
	const Position rejectedDepot(201, 100, 7), rejectedSupplier(301, 100, 7);
	const Position outboundTile(110, 100, 7), abandonedTile(111, 100, 7), rejectedTile(210, 100, 7);
	const Evidence::Actor actor{10, 150, 20, 20, 20, 20, 1.0f, 500, true, false, true, true,
	    100, 150, false, 0, 1, 1, origin, 100, true};
	const PlayerBotNavigationRiskProfile risk;
	std::map<uint32_t, std::vector<Evidence::Offer>> liveOffers;
	auto proof = [&](Position from, Position to, uint32_t provider, Position tile) {
		auto evidence = std::make_shared<Evidence>();
		evidence->source = from;
		evidence->destination = to;
		evidence->actor = actor; // Actual actor origin, not the synthetic leg's source.
		evidence->topologyGeneration = 7;
		evidence->riskRevision = 8;
		evidence->risk = risk;
		evidence->paid = provider != 0;
		if (provider) {
			const Position anchor(static_cast<uint16_t>(400 + provider), 100, 7);
			evidence->offers = {{provider, anchor, to, 10, 8, true, {"hi", "yes"}}};
			evidence->providers.reset({{provider, anchor}});
			evidence->providers.use(provider);
			liveOffers[provider] = evidence->offers;
		}
		Changes::Scope dependencies(evidence->watch);
		Changes::read(tile);
		return evidence;
	};
	auto validate = [&](Evidence& leg) {
		if (!leg.validContext(leg.source, leg.destination, actor, 7, 8, risk)) return false;
		return !leg.paid || leg.validOffers(liveOffers.at(std::get<0>(leg.offers.front())));
	};
	struct Candidate { PlayerBotHuntRouteEvidence evidence; };
	PlayerBotHuntRouteRetention<Candidate> retained;
	retained.begin(10, 8);
	PlayerBotHuntRouteSelection selector({routeFixture(33)});
	PlayerBotHuntRouteResult result;
	std::weak_ptr<Evidence> rejectedProof;
	for (int turns = 0; !result.terminal && turns < 12; ++turns) {
		const auto request = selector.next();
		auto observation = routeFacts();
		std::shared_ptr<Evidence> completed;
		if (request.stage == Stage::Outbound) {
			auto& current = retained.outbound(request.sequence);
			// The same request invalidates and rebuilds its work. Its abandoned
			// watch must not survive in the eventual completed candidate proof.
			auto abandoned = proof(origin, hunt, 0, abandonedTile);
			Changes::changed(abandonedTile);
			assert(!validate(*abandoned));
			assert(&current == &retained.outbound(selector.next().sequence));
			completed = proof(origin, hunt, 0, outboundTile);
			assert(validate(*completed));
			observation.approaches = {rejectedDepot, depot};
		} else if (request.stage == Stage::Depot) {
			if (request.to == rejectedDepot) {
				auto failed = proof(hunt, rejectedDepot, 3, rejectedTile);
				rejectedProof = failed;
				retained.pending()->evidence.accept(Stage::Depot, false, failed);
				assert(!retained.pending()->evidence.depot);
				observation.reached = false;
			} else {
				completed = proof(hunt, depot, 1, Position(220, 100, 7));
				observation.potionReserve = 10;
				observation.supplyProfile.potions = 0;
			}
		} else if (request.stage == Stage::DiscoverSupply) {
			assert(rejectedProof.expired());
			Changes::changed(rejectedTile); // A rejected alternative is not a prerequisite.
			observation.approaches = {rejectedSupplier, supplier};
		} else if (request.stage == Stage::Supplier) {
			if (request.to == rejectedSupplier) {
				observation.reached = false;
				assert(retained.pending()->evidence.suppliers.empty());
			} else completed = proof(depot, supplier, 2, Position(320, 100, 7));
		}
		result = selector.observe(request, observation);
		assert(result.accepted);
		retained.pending()->evidence.accept(request.stage, result.accepted, completed);
		if (result.completedCandidate) retained.complete(*result.completedCandidate);
	}
	assert(result.terminal && result.selectedRouteRegion);
	auto selected = retained.take(*result.selectedRouteRegion);
	assert(selected && selected->evidence.valid(validate));
	auto& legs = selected->evidence;
	assert(!legs.outbound->paid && legs.depot->paid && legs.suppliers.at(0)->paid);
	assert(legs.depot->source == hunt && legs.suppliers.at(0)->source == depot);
	assert(std::get<18>(legs.depot->actor) == origin && std::get<18>(legs.suppliers.at(0)->actor) == origin);

	// Each required paid leg must check offers/providers even though outbound
	// walking has no provider facts. No tile or NPC-generation change occurs.
	for (uint32_t provider : {1u, 2u}) {
		const auto original = liveOffers.at(provider);
		std::get<1>(liveOffers[provider].front()).x += 3;
		assert(validate(*legs.outbound) && !legs.valid(validate));
		liveOffers[provider] = original;
		assert(legs.valid(validate));
		std::get<3>(liveOffers[provider].front())++;
		assert(!legs.valid(validate)); // Fare/offer semantics, independent of tile changes.
		liveOffers[provider] = original;
		liveOffers[provider].clear();
		assert(!legs.valid(validate)); // Removed, opaque, ineligible or failed offer.
		liveOffers[provider] = original;
		assert(legs.valid(validate));
	}
	// Selected stable-approach/locker dependencies can be attached to the
	// successful endpoint proof without importing the entire discovery scan.
	const Position locker(200, 101, 7);
	{
		Changes::Scope endpointDependencies(legs.depot->watch);
		Changes::read(locker);
	}
	Changes::changed(locker);
	assert(!legs.valid(validate));
	legs.accept(Stage::Depot, true, proof(hunt, depot, 1, Position(220, 100, 7)));
	assert(legs.valid(validate));
	// Replacing one leg never refreshes a stale earlier successful leg.
	Changes::changed(outboundTile);
	legs.accept(Stage::Supplier, true, proof(depot, supplier, 2, Position(321, 100, 7)));
	assert(!legs.valid(validate));
	legs.accept(Stage::Outbound, true, proof(origin, hunt, 0, outboundTile));
	assert(legs.valid(validate));
	legs.suppliers.at(0).reset();
	assert(!legs.valid(validate));
	PlayerBotHuntRouteEvidence missingDepot;
	missingDepot.accept(Stage::Outbound, true, legs.outbound);
	assert(!missingDepot.valid(validate));
	missingDepot.accept(Stage::Depot, true, legs.depot);
	assert(missingDepot.valid(validate)); // A supplier is optional only when never required.
	missingDepot.accept(Stage::Supplier, true, nullptr);
	assert(!missingDepot.valid(validate)); // A missing accepted endpoint proof cannot escape.

	// Retain every selected supplier leg and the actual item it must sell.
	const Position weapons(350, 100, 7), potionTile(330, 100, 7), spearTile(340, 100, 7);
	PlayerBotHuntRouteEvidence grouped;
	grouped.accept(Stage::Outbound, true, proof(origin, hunt, 0, outboundTile));
	grouped.accept(Stage::Depot, true, proof(hunt, depot, 1, Position(220, 100, 7)));
	auto supplierProof = [&](uint16_t itemId) {
		auto leg = itemId == 7618 ? proof(depot, supplier, 2, potionTile) : proof(supplier, weapons, 3, spearTile);
		leg->exit = Evidence::Exit::Supplier;
		leg->supplyItemId = itemId;
		return leg;
	};
	grouped.accept(Stage::Supplier, true, supplierProof(7618), 7618);
	grouped.accept(Stage::Supplier, true, supplierProof(2389), 2389);
	std::set<uint16_t> checkedItems;
	auto validateItems = [&](Evidence& leg) {
		if (leg.exit == Evidence::Exit::Supplier) checkedItems.insert(leg.supplyItemId);
		return validate(leg);
	};
	assert(grouped.valid(validateItems) && (checkedItems == std::set<uint16_t>{2389, 7618}));
	for (uint16_t itemId : {7618, 2389}) {
		Changes::changed(itemId == 7618 ? potionTile : spearTile);
		assert(!grouped.valid(validateItems));
		const uint16_t other = itemId == 7618 ? 2389 : 7618;
		grouped.accept(Stage::Supplier, true, supplierProof(other), other);
		assert(!grouped.valid(validateItems)); // Another item's proof cannot repair the stale leg.
		grouped.accept(Stage::Supplier, true, supplierProof(itemId), itemId);
		assert(grouped.valid(validateItems));
	}
	grouped.accept(Stage::Supplier, true, nullptr, 2389);
	assert(grouped.suppliers.at(7618) && !grouped.valid(validateItems));
}

void routeTurnContracts()
{
	const Position depot(200, 100, 7), supplier(300, 100, 7);
	struct Tick {
		std::vector<PlayerBotHuntRouteStage> requests;
		PlayerBotHuntRouteResult result;
	};
	auto tick = [&](PlayerBotHuntRouteSelection& selection, bool shortSupply, bool failSupplier,
	                bool failDepot = false) {
		Tick turn;
		for (unsigned step = 0; step < 4; ++step) {
			const auto request = selection.next();
			turn.requests.push_back(request.stage);
			auto observation = routeFacts();
			if (request.stage == PlayerBotHuntRouteStage::Outbound) {
				observation.approaches = {depot};
				observation.travelSeconds = 5;
			}
			if (request.stage == PlayerBotHuntRouteStage::Depot && failDepot) observation.reached = false;
			if (request.stage == PlayerBotHuntRouteStage::Depot && shortSupply) {
				observation.potionReserve = 10;
				observation.supplyProfile.potions = 0;
			}
			if (request.stage == PlayerBotHuntRouteStage::DiscoverSupply) observation.approaches = {supplier};
			if (request.stage == PlayerBotHuntRouteStage::Supplier && failSupplier) observation.reached = false;
			turn.result = selection.observe(request, observation);
			assert(turn.result.accepted);
			if (turn.result.yield || turn.result.terminal) break;
		}
		assert(turn.result.yield || turn.result.terminal);
		assert(std::count_if(turn.requests.begin(), turn.requests.end(), [](auto stage) {
			return stage == PlayerBotHuntRouteStage::Outbound || stage == PlayerBotHuntRouteStage::Depot ||
			       stage == PlayerBotHuntRouteStage::Supplier;
		}) <= 1);
		return turn;
	};
	PlayerBotHuntRouteSelection failedSupply({routeFixture(1), routeFixture(2, 99)});
	assert(tick(failedSupply, true, true).requests == std::vector<PlayerBotHuntRouteStage>{PlayerBotHuntRouteStage::Outbound});
	assert((tick(failedSupply, true, true).requests == std::vector<PlayerBotHuntRouteStage>{
	    PlayerBotHuntRouteStage::Depot, PlayerBotHuntRouteStage::DiscoverSupply}));
	assert(tick(failedSupply, true, true).requests == std::vector<PlayerBotHuntRouteStage>{PlayerBotHuntRouteStage::Supplier});
	auto rejected = tick(failedSupply, true, true);
	assert(rejected.requests == std::vector<PlayerBotHuntRouteStage>{PlayerBotHuntRouteStage::RejectSupply} &&
	       rejected.result.completedCandidate && rejected.result.yield);
	assert(tick(failedSupply, false, false).requests == std::vector<PlayerBotHuntRouteStage>{PlayerBotHuntRouteStage::Outbound});
	auto selected = tick(failedSupply, false, false);
	assert((selected.requests == std::vector<PlayerBotHuntRouteStage>{
	    PlayerBotHuntRouteStage::Depot, PlayerBotHuntRouteStage::Final}) &&
	    selected.result.terminal && !selected.result.yield && selected.result.selectedRouteRegion);

	PlayerBotHuntRouteSelection more({routeFixture(1), routeFixture(2)});
	tick(more, false, false);
	auto continueAfterAccept = tick(more, false, false);
	assert((continueAfterAccept.requests == std::vector<PlayerBotHuntRouteStage>{
	    PlayerBotHuntRouteStage::Depot, PlayerBotHuntRouteStage::Final}) &&
	    continueAfterAccept.result.completedCandidate && continueAfterAccept.result.yield &&
	    !continueAfterAccept.result.terminal);
	assert(tick(more, false, false).requests == std::vector<PlayerBotHuntRouteStage>{PlayerBotHuntRouteStage::Outbound});

	PlayerBotHuntRouteSelection failedDepot({routeFixture(3)});
	assert(tick(failedDepot, false, false).requests == std::vector<PlayerBotHuntRouteStage>{PlayerBotHuntRouteStage::Outbound});
	auto depotFailure = tick(failedDepot, false, false, true);
	assert(depotFailure.requests == std::vector<PlayerBotHuntRouteStage>{PlayerBotHuntRouteStage::Depot} &&
	       depotFailure.result.completedCandidate && depotFailure.result.yield);
}

void routeDurationRefreshContracts()
{
	PlayerBotHuntRuntime runtime({});
	PlayerBotHuntRuntimePlanningInput input;
	input.cacheRevision = 1;
	input.huntDurationSeconds = 2400; // Scored session precedes a config/recovery-mode change.
	input.start.emplace();
	input.start->scan.revision = 1;
	input.start->scan.candidateCount = 1;
	input.start->scan.candidateIndices = {0};
	const auto now = std::chrono::steady_clock::time_point{};
	const auto started = runtime.advancePlanning(input, now);
	assert(runtime.advancePlanning(input, now).scoreWork.size() == 1);
	auto region = routeFixture(1, 60);
	region.expectedDamagePerSecond = region.combatFraction = 1;
	region.supplyProfile.potionHealing = 10;
	const auto scored = runtime.completeScoreWork({{0, true, true, true, region}}, 0);
	assert(scored.candidateSnapshot && runtime.beginRouteSelection(
	    started.planningPass, started.scoringRevision, scored.routeCandidates));
	assert(runtime.advancePlanning(input, now).command == PlayerBotHuntRuntimeCommand::RegionSelected);

	const Position depot(200, 100, 7), supplier(300, 100, 7);
	auto request = *runtime.nextRouteRequest();
	auto outbound = routeFacts();
	outbound.approaches = {depot};
	outbound.huntDurationSeconds = 120; // Current select invocation, not scored duration.
	outbound.travelSeconds = 10;
	outbound.staminaMultiplier = 0.5;
	assert(runtime.observeRoute(request, outbound).yield);
	request = *runtime.nextRouteRequest();
	auto live = routeFacts(); live.supplyProfile.potionHealing = 10;
	assert(request.stage == PlayerBotHuntRouteStage::Depot && !runtime.observeRoute(request, live).yield);
	request = *runtime.nextRouteRequest();
	auto discovery = routeFacts(); discovery.approaches = {supplier};
	assert(request.stage == PlayerBotHuntRouteStage::DiscoverSupply && runtime.observeRoute(request, discovery).yield);
	request = *runtime.nextRouteRequest();
	assert(request.stage == PlayerBotHuntRouteStage::Supplier && !runtime.observeRoute(request, routeFacts()).yield);
	request = *runtime.nextRouteRequest();
	assert(request.stage == PlayerBotHuntRouteStage::Final);
	const auto result = runtime.observeRoute(request, live);
	assert(result.terminal && result.selectedRouteRegion);
	const auto& selected = *result.selectedRouteRegion;
	assert(selected.availableHuntSeconds == 120 && selected.estimatedTravelSeconds == 10 &&
	       selected.staminaExperienceMultiplier == 0.5 && selected.projectedExperience == 60 &&
	       selected.score == 60 && selected.supplyBudget.expectedDamage == 120 &&
	       selected.supplyBudget.expectedPotions == 12 && selected.supplyBudget.fits);
}

void routeRuntimeContracts()
{
	PlayerBotHuntRuntime runtime({});
	PlayerBotHuntRuntimePlanningInput input;
	input.cacheRevision = 1;
	input.start.emplace();
	input.start->scan.revision = 1;
	input.start->scan.candidateIndices = {0, 1};
	input.start->scan.candidateCount = 2;
	const auto now = std::chrono::steady_clock::time_point{};
	auto begin = [&]() {
		const auto started = runtime.advancePlanning(input, now);
		assert(started.command == PlayerBotHuntRuntimeCommand::PlanningStarted);
		assert(!runtime.beginRouteSelection(started.planningPass, started.scoringRevision,
		                                    {routeFixture(1)}));
		const auto work = runtime.advancePlanning(input, now);
		assert(work.scoreWork.size() == 2);
		assert(!runtime.beginRouteSelection(started.planningPass, started.scoringRevision,
		                                    {routeFixture(1)}));
		const auto scored = runtime.completeScoreWork({{0, true, true, true, routeFixture(1, 100)},
		                                                  {1, true, true, true, routeFixture(2, 99)}}, 0);
		assert(scored.command == PlayerBotHuntRuntimeCommand::PlanningScored);
		assert(runtime.beginRouteSelection(started.planningPass, started.scoringRevision, scored.routeCandidates));
		assert(!runtime.beginRouteSelection(started.planningPass, started.scoringRevision,
		                                    {routeFixture(99)}));
		assert(runtime.advancePlanning(input, now).command == PlayerBotHuntRuntimeCommand::RegionSelected);
		return started;
	};
	auto started = begin();
	auto request = *runtime.nextRouteRequest();
	assert(request.planningPass == started.planningPass && request.scoringRevision == 1);
	auto unreachable = runtime.observeRoute(request, routeFacts(false));
	assert(unreachable.completedCandidate && unreachable.completedCandidate->atlasVariantId == 1 &&
	       unreachable.planningPass == started.planningPass && unreachable.scoringRevision == 1);
	const Position depot(200, 100, 7), supplier(300, 100, 7);
	for (auto stage : {PlayerBotHuntRouteStage::Outbound, PlayerBotHuntRouteStage::Depot,
	                   PlayerBotHuntRouteStage::DiscoverSupply, PlayerBotHuntRouteStage::Supplier,
	                   PlayerBotHuntRouteStage::Final}) {
		// Drive the real wrapper to each barrier, then cancel or revise the pass.
		for (unsigned advance = 0; advance < 5 && runtime.nextRouteRequest()->stage != stage; ++advance) {
			request = *runtime.nextRouteRequest();
			auto observation = routeFacts();
			if (request.stage == PlayerBotHuntRouteStage::Outbound) observation.approaches = {depot};
			if (request.stage == PlayerBotHuntRouteStage::Depot) {
				observation.potionReserve = 10;
				observation.supplyProfile.potions = 0;
			}
			if (request.stage == PlayerBotHuntRouteStage::DiscoverSupply) observation.approaches = {supplier};
			runtime.observeRoute(request, observation);
		}
		request = *runtime.nextRouteRequest();
		assert(request.stage == stage);
		PlayerBotHuntRuntimeOutcome cancelled;
		if (stage == PlayerBotHuntRouteStage::Depot || stage == PlayerBotHuntRouteStage::Supplier) {
			input.cacheRevision++;
			input.start->scan.revision = input.cacheRevision;
			cancelled = runtime.advancePlanning(input, now);
			assert(cancelled.planningCancelled && cancelled.staleRevision &&
			       cancelled.invalidatedPlanningPass == started.planningPass);
			// Invalidation starts the next pass in the same turn.
			assert(cancelled.command == PlayerBotHuntRuntimeCommand::PlanningStarted);
			assert(!runtime.observeRoute(request, routeFacts()).accepted);
			runtime.cancelPlanning();
		} else {
			cancelled = runtime.cancelPlanning();
			assert(cancelled.planningCancelled && !runtime.nextRouteRequest());
			assert(!runtime.observeRoute(request, routeFacts()).terminal);
		}
		started = begin();
		assert(started.planningPass > cancelled.invalidatedPlanningPass);
		assert(!runtime.observeRoute(request, routeFacts()).accepted);
		assert(runtime.nextRouteRequest()->stage == PlayerBotHuntRouteStage::Outbound);
	}
	// Commit rejections only on the terminal result and never replay it.
	request = *runtime.nextRouteRequest();
	unreachable = runtime.observeRoute(request, routeFacts(false));
	assert(unreachable.completedCandidate && !runtime.observeRoute(request, routeFacts(false)).completedCandidate);
	request = *runtime.nextRouteRequest();
	auto outbound = routeFacts(); outbound.approaches = {depot};
	runtime.observeRoute(request, outbound);
	request = *runtime.nextRouteRequest(); runtime.observeRoute(request, routeFacts());
	request = *runtime.nextRouteRequest();
	auto terminal = runtime.observeRoute(request, routeFacts());
	assert(terminal.terminal && terminal.selectedRouteRegion && terminal.selectedRouteRegion->atlasVariantId == 2 &&
	       terminal.failureCounts.at("route_unreachable") == 1 && terminal.rejectedVariants == std::vector<uint64_t>{1} &&
	       terminal.planningPass == started.planningPass && terminal.scoringRevision == input.cacheRevision);
	assert(runtime.planningActive() && !runtime.observeRoute(request, routeFacts()).terminal &&
	       !runtime.nextRouteRequest());
	assert(!runtime.beginRouteSelection(started.planningPass, started.scoringRevision,
	                                    {routeFixture(99)}) && !runtime.nextRouteRequest());
	assert(!runtime.observeRoute(request, routeFacts()).accepted);
	runtime.completePlanningSelection();
	assert(!runtime.nextRouteRequest() && !runtime.observeRoute(request, routeFacts()).accepted);
	started = begin();
	assert(!runtime.observeRoute(request, routeFacts()).accepted);
	request = *runtime.nextRouteRequest();
	PlayerBotHuntPlanningObservation unavailable;
	unavailable.candidatesAvailable = false;
	const auto exhausted = runtime.advancePlanning(input, now, unavailable);
	assert(exhausted.command == PlayerBotHuntRuntimeCommand::ScopeExhausted && !runtime.nextRouteRequest() &&
	       !runtime.observeRoute(request, routeFacts()).terminal);

	PlayerBotHuntRuntime empty({});
	PlayerBotHuntRuntimePlanningInput noCandidates;
	noCandidates.cacheRevision = 7;
	noCandidates.start.emplace();
	noCandidates.start->scan.revision = 7;
	const auto emptyPass = empty.advancePlanning(noCandidates, now);
	assert(!empty.beginRouteSelection(emptyPass.planningPass, 7, {}));
	const auto scoredEmpty = empty.advancePlanning(noCandidates, now);
	assert(scoredEmpty.command == PlayerBotHuntRuntimeCommand::PlanningScored && scoredEmpty.candidateSnapshot);
	assert(empty.beginRouteSelection(emptyPass.planningPass, 7, scoredEmpty.routeCandidates));
	// The preliminary selection guard still owns scope exhaustion, not the selector.
	assert(empty.advancePlanning(noCandidates, now).command == PlayerBotHuntRuntimeCommand::ScopeExhausted &&
	       !empty.nextRouteRequest());
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
		region.availableHuntSeconds = 60;
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
		sparseCompetition[index].availableHuntSeconds = 60;
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

	// Stepped healing budgets and stock-dependent duration need an honest full
	// productive upper bound, not an assumed monotonic grid fitting bound.
	PlayerBotHuntRegion supplyLimited;
	supplyLimited.suitable = supplyLimited.reachable = true;
	supplyLimited.supplyProfile.potions = 6; // five routine potions above the reserve of one
	supplyLimited.supplyProfile.potionHealing = 100;
	supplyLimited.expectedDamagePerSecond = 1;
	supplyLimited.combatFraction = 1;
	supplyLimited.availableHuntSeconds = 2400;
	supplyLimited.optimisticProjectedExperience = 2400;
	supplyLimited.reconcileSupplies(1);
	assert(!supplyLimited.supplyBudget.fits); // 24 potions needed for the full hunt
	// A shorter window can fit; a sampled fit is not an upper-bound proof.
	supplyLimited.optimisticFittingExperience = playerBotHuntOptimisticFittingExperience(supplyLimited, 2400, 1);
	assert(supplyLimited.optimisticFittingExperience == 2400);
	PlayerBotHuntRegion fittingIncumbent;
	fittingIncumbent.availableHuntSeconds = 60;
	fittingIncumbent.supplyBudget.fits = true;
	fittingIncumbent.score = 2401;
	assert(!playerBotHuntCandidateCanBeatValidated(supplyLimited, fittingIncumbent));
	fittingIncumbent.score = 2400;
	assert(playerBotHuntCandidateCanBeatValidated(supplyLimited, fittingIncumbent));
	PlayerBotHuntRegion fitsFully = supplyLimited;
	fitsFully.supplyProfile.potions = 30;
	assert(playerBotHuntOptimisticFittingExperience(fitsFully, 2400, 1) == fitsFully.optimisticProjectedExperience);
	supplyLimited.supplyProfile.potions = 1; // only the reserve: no hunt length fits
	supplyLimited.optimisticFittingExperience = playerBotHuntOptimisticFittingExperience(supplyLimited, 2400, 1);
	assert(supplyLimited.optimisticFittingExperience == 2400);
	fittingIncumbent.score = 0;
	assert(playerBotHuntCandidateCanBeatValidated(supplyLimited, fittingIncumbent));
	fittingIncumbent.supplyBudget.fits = false; // a non-fitting incumbent can still be displaced
	assert(playerBotHuntCandidateCanBeatValidated(supplyLimited, fittingIncumbent));

	// The full-duration stamina bound dominates every shorter available hunt,
	// so validated travel can never lift a skipped candidate above it.
	for (const uint16_t stamina : {0, 500, 840, 841, 2400, 2401, 2402, 2403, 2410, 2520}) {
		for (const bool system : {false, true}) {
			for (const bool premium : {false, true}) {
				const double duration = 2400;
				const double bound = playerBotHuntStaminaExperienceMultiplier(stamina, system, premium, duration) * duration;
				for (double available = 0; available <= duration; available += 30) {
					assert(playerBotHuntStaminaExperienceMultiplier(stamina, system, premium, available) * available <=
					       bound + 1e-9);
				}
			}
		}
	}
	assert(playerBotHuntStaminaExperienceMultiplier(557, true, true, 2400) == 0.5);
	assert(playerBotHuntStaminaExperienceMultiplier(2520, true, true, 2400) == 1.5);
	assert(playerBotHuntStaminaExperienceMultiplier(2520, true, false, 2400) == 1);

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

	// Inside the area, a quarter of the circuit (here one waypoint) is passed
	// over without removing it; the next failure still ends the region.
	PlayerBotHuntRuntime inside(points);
	modeled.availableHuntSeconds = 60;
	player.health = player.maximumHealth = 100;
	inside.selectPlanningRegion(modeled, player, now);
	PlayerBotSupplyProfile stockedArrival;
	stockedArrival.potions = 20;
	assert(inside.enterHuntArea(player, stockedArrival, now));
	const auto passed = inside.observePatrolNavigation(failure, now, 3, 3);
	assert(passed.command == PlayerBotHuntPatrolCommand::SkipWaypoint && !passed.cooldown);
	assert(inside.region()->patrolPoints == points && inside.patrolTarget().destination == points[1]);
	assert(inside.observePatrolNavigation(failure, now, 3, 3).command == PlayerBotHuntPatrolCommand::RegionExhausted);
}

void incompletePatrolPreflight()
{
	const Position destination(100, 100, 7);
	auto region = routeFixture(42);
	region.destination = destination;
	region.patrolPoints = {destination};
	region.viability.reachableSpawns = 4;
	PlayerBotHuntRuntimePlayerObservation player;
	player.maximumHealth = player.health = 100;
	const auto now = std::chrono::steady_clock::time_point{};
	PlayerBotNavigationPlanMetrics metrics;
	metrics.result = PlayerBotNavigationResult::NodeLimit;
	metrics.expandedNodes = 100000;
	auto incomplete = playerBotHuntRejectedPatrolPreflight(metrics, false);
	assert(incomplete.plan.attempted && !incomplete.routeUnavailable && !incomplete.routeUnsafe);
	PlayerBotHuntRuntime runtime({destination});
	runtime.selectPlanningRegion(region, player, now);
	// Turn, node and request/provider-restart caps all produce the same unknown
	// evidence. Even legacy callers setting routeUnavailable cannot cool a hunt.
	for (int i = 1; i <= 2; ++i) {
		incomplete.routeUnavailable = i == 2;
		const auto outcome = runtime.observePatrolNavigation(incomplete, now, 3, 3);
		assert(outcome.command == PlayerBotHuntPatrolCommand::Continue && !outcome.cooldown);
		assert(outcome.planningIncompleteAttempts == unsigned(i) && outcome.routeFailures == 0);
		assert(outcome.retryAfter == std::chrono::seconds(i));
	}
	PlayerBotNavigationRuntimeOutcome reached;
	reached.destinationReached = true;
	assert(runtime.observePatrolNavigation(reached, now, 3, 3).command == PlayerBotHuntPatrolCommand::WaypointReached);
	assert(runtime.active() && runtime.region()->patrolPoints == region.patrolPoints);
	for (int i = 1; i <= 4; ++i) {
		const auto outcome = runtime.observePatrolNavigation(incomplete, now, 3, 3);
		assert(outcome.planningIncompleteAttempts == unsigned(std::min(i, 3)) && !outcome.cooldown);
		assert(outcome.routeFailures == 0 && outcome.stepFailures == 0);
		assert(outcome.command == (i < 3 ? PlayerBotHuntPatrolCommand::Continue : PlayerBotHuntPatrolCommand::PlanningIncomplete));
		assert(runtime.region()->patrolPoints == region.patrolPoints);
	}
	// Confirmed failures keep the ordinary three-attempt cooldown, separately
	// from incompletes. Successful arrival/activation resets both budgets.
	runtime.selectPlanningRegion(region, player, now);
	metrics.result = PlayerBotNavigationResult::Unreachable;
	const auto failure = playerBotHuntRejectedPatrolPreflight(metrics, false);
	assert(failure.routeUnavailable);
	assert(runtime.observePatrolNavigation(incomplete, now, 3, 3).planningIncompleteAttempts == 1);
	for (int i = 1; i <= 3; ++i) {
		const auto outcome = runtime.observePatrolNavigation(failure, now, 3, 3);
		assert(outcome.command == (i < 3 ? PlayerBotHuntPatrolCommand::Continue : PlayerBotHuntPatrolCommand::RegionExhausted));
		if (i == 3) assert(outcome.routeFailures == 3 && outcome.cooldown && outcome.cooldown->variantId == 42);
	}
	runtime.selectPlanningRegion(region, player, now);
	metrics.result = PlayerBotNavigationResult::Reached;
	const auto unsafe = playerBotHuntRejectedPatrolPreflight(metrics, true);
	const auto outcome = runtime.observePatrolNavigation(unsafe, now, 3, 1);
	assert(outcome.command == PlayerBotHuntPatrolCommand::RegionExhausted && outcome.cooldown);
	runtime.selectPlanningRegion(region, player, now);
	metrics.result = PlayerBotNavigationResult::RiskRejected;
	const auto rejected = playerBotHuntRejectedPatrolPreflight(metrics, false);
	assert(rejected.routeUnsafe && rejected.routeUnavailable);
	const auto rejection = runtime.observePatrolNavigation(rejected, now, 3, 1);
	assert(rejection.command == PlayerBotHuntPatrolCommand::RegionExhausted && rejection.cooldown);
	assert(rejection.planningIncompleteAttempts == 0);
	runtime.selectPlanningRegion(region, player, now);
	incomplete.stepFailureCount = 3;
	assert(runtime.observePatrolNavigation(incomplete, now, 3, 3).command == PlayerBotHuntPatrolCommand::RegionExhausted);
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

void floorChangeLandingOffset()
{
	// A ladder that lands one tile from the predicted tile replans from the
	// actual landing without counting a step failure or blocking the ladder.
	const auto now = std::chrono::steady_clock::now();
	const auto timeout = std::chrono::seconds(2);
	PlayerBotNavigationStep ladder;
	ladder.action = PlayerBotNavigationAction::Use;
	ladder.target = Position(100, 100, 8);
	ladder.expectedPosition = Position(101, 100, 7);
	PlayerBotNavigationStep onward;
	onward.target = onward.expectedPosition = Position(102, 100, 7);
	PlayerBotNavigationGoal goal;
	goal.type = PlayerBotNavigationGoalType::Exact;
	goal.position = onward.target;

	PlayerBotNavigationSession session;
	session.installRoute(goal, {ladder, onward});
	session.beginMovement(ladder, now);
	assert(session.observeMovement(Position(100, 100, 8), true, now, timeout, timeout) ==
	       PlayerBotPendingMovementResult::Waiting);
	assert(session.observeMovement(Position(101, 101, 7), true, now, timeout, timeout) ==
	       PlayerBotPendingMovementResult::LandingOffset);
	assert(session.routeEmpty() && session.stepFailureCount() == 0);
	assert(session.activeBlockedPositions(now).empty());

	// Further away, or on the wrong floor, still fails as before.
	for (const Position& landing : {Position(103, 100, 7), Position(101, 101, 6)}) {
		session.installRoute(goal, {ladder, onward});
		session.beginMovement(ladder, now);
		assert(session.observeMovement(landing, false, now, timeout, timeout) ==
		       PlayerBotPendingMovementResult::Mismatch);
	}
	assert(session.stepFailureCount() == 2);

	// Same-floor moves keep exact verification.
	PlayerBotNavigationStep move;
	move.target = move.expectedPosition = Position(101, 100, 7);
	session.installRoute(goal, {move, onward});
	session.beginMovement(move, now);
	assert(session.observeMovement(Position(101, 101, 7), false, now, timeout, timeout) ==
	       PlayerBotPendingMovementResult::Mismatch);
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

	{
		using namespace std::chrono_literals;
		PlayerBotTransitProgress progress;
		const auto t0 = std::chrono::steady_clock::time_point{} + 1h;
		assert(progress.stalledFor(t0) == 0ms);
		progress.observe(Position(100, 100, 7), t0);
		// Blocked in place: retries accrue stall time.
		for (auto t = t0 + 1s; t <= t0 + 4s; t += 1s) progress.observe(Position(100, 100, 7), t);
		assert(progress.stalledFor(t0 + 4s) == 4000ms);
		// Stepping back and forth between known tiles is not progress.
		progress.observe(Position(101, 100, 7), t0 + 5s);
		assert(progress.stalledFor(t0 + 5s) == 0ms);
		progress.observe(Position(100, 100, 7), t0 + 6s);
		progress.observe(Position(101, 100, 7), t0 + 7s);
		assert(progress.stalledFor(t0 + 7s) == 2000ms);
		// A 30 s fight between navigation turns is not stall time.
		progress.observe(Position(101, 100, 7), t0 + 37s);
		assert(progress.stalledFor(t0 + 37s) == 2000ms);
		progress.observe(Position(102, 100, 7), t0 + 38s);
		assert(progress.stalledFor(t0 + 38s) == 0ms);
		progress.reset();
		assert(progress.stalledFor(t0 + 60s) == 0ms);
	}

	const Position stalled(100, 100, 7);
	const Position intended(101, 100, 7);
	PlayerBotTransitCombat episode;
	assert(episode.observe(true, 1, Phase::Service));
	assert(!episode.movementFallbackRequired()); // planning and legitimate waits do not create evidence
	episode.observePosition(Position(101, 100, 7));
	assert(!episode.movementFallbackRequired()); // ordinary movement does not create evidence either
	assert(!episode.allowsDefense(42, true));
	// Hunt planning has no route yet: an attacker may be defended against during
	// transit, but ordinary transit and route-critical targets remain excluded.
	assert(!episode.allowsDefense(42, false));
	assert(episode.allowsDefense(42, false, true));
	assert(!episode.allowsDefense(42, true, true));
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
		region.coinGoldPerMinute = 1;
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
	assert(manaDebt.manaPotionDebt == 0);

	// A vocation that stocks mana potions repays mana debt with them instead.
	PlayerBotHuntPolicy manaPotionPolicy;
	manaDebtRegion.supplyProfile.kinds.push_back({PlayerBotSupplyKind::ManaPotion, playerBotManaPotionItemId, 5, 1, {}});
	manaPotionPolicy.observeCombat({true, 60, 100, 100, 80, 100, 1});
	manaPotionPolicy.observeRecovery(false);
	for (unsigned i = 0; i < 3; ++i) manaPotionPolicy.observeKill();
	const auto manaPotionDebt = manaPotionPolicy.observeSupplies(
	    manaDebtRegion, manaDebtRegion.supplyCapability, 120, 100, 100, 80, 0, false);
	assert(manaPotionDebt.accepted && manaPotionDebt.potionEquivalentDemand == 0);
	assert(std::abs(manaPotionDebt.manaPotionDebt - 20.0 / playerBotManaPotionMinimumMana) < 1e-12);

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
	region.supplyProfile.potions = player.potions = 20;
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

	// The last unit of a weapon-matched kind leaves the stock list; it still counts as consumed.
	PlayerBotHuntRuntime spearRuntime({});
	PlayerBotHuntRuntimePlayerObservation spearPlayer = player;
	spearPlayer.supplies = {{playerBotThrowingWeaponRule(2389, 8), 3}};
	spearRuntime.selectPlanningRegion(region, spearPlayer, start);
	spearRuntime.enterHuntArea(spearPlayer, region.supplyProfile, start);
	spearRuntime.sampleCombat({true, start + std::chrono::seconds(1), 100, 100, 100, 100, 1});
	spearRuntime.sampleCombat({true, start + std::chrono::seconds(61), 100, 100, 100, 100, 1});
	spearPlayer.supplies.clear();
	const auto spearCompleted = spearRuntime.complete(spearPlayer, start + std::chrono::seconds(120), 2400);
	assert(spearCompleted && spearCompleted->supplyDemand.size() == 1);
	assert(spearCompleted->supplyDemand[0].kind == PlayerBotSupplyKind::ThrowingWeapon);
	assert(spearCompleted->supplyDemand[0].consumed == 3 && spearCompleted->supplyDemand[0].updated);

	// Outbound events are outside the supply frame. Arrival captures the swapped
	// equipment context once; a duplicate waypoint arrival cannot reset evidence.
	PlayerBotHuntRuntime arrivalRuntime({});
	PlayerBotHuntRegion arrivalRegion = regionFor(23, capability(10));
	arrivalRegion.supplyProfile.potions = 20;
	arrivalRegion.expectedDamagePerSecond = 1;
	arrivalRegion.supplyStaticPotionsPerCombatSecond = 10.0 / 60.0;
	arrivalRegion.supplyGlobalLearning = {0.9, 1};
	PlayerBotHuntRuntimePlayerObservation planningPlayer;
	planningPlayer.potions = 20;
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
	assert(arrivalCompleted->durationSeconds == 120);
	assert(arrivalCompleted->supplyObservation.durationSeconds == 120);
	assert(arrivalCompleted->supplyObservation.startingHealth == 150);
	assert(arrivalCompleted->supplyObservation.startingMana == 150);
	assert(arrivalCompleted->supplyObservation.endingHealth == 160);
	assert(arrivalCompleted->supplyObservation.endingMana == 170);
	assert(arrivalCompleted->supplyObservation.levelHealthRestored == 20);
	assert(arrivalCompleted->supplyObservation.levelManaRestored == 30);
	assert(arrivalCompleted->supplyObservation.levelAdjustedHealthDebt == 10);
	assert(arrivalCompleted->supplyObservation.levelAdjustedManaDebt == 10);
	assert(std::abs(arrivalCompleted->supplyObservation.staticPotionsPerCombatSecond -
	                (120.0 / 125.0) / 60.0) < 1e-12);
	assert(std::abs(arrivalCompleted->region.supplyStaticPotionsPerCombatSecond -
	                (120.0 / 125.0) / 60.0) < 1e-12);
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
	regenerationRegion.supplyProfile.potions = 20;
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
	    pretravelBudget.potionEquivalentDemand / 90.0;
	assert(std::abs(regenerationRegion.supplyStaticPotionsPerCombatSecond - (150.0 / 125.0) / 90.0) < 1e-12);
	PlayerBotHuntRuntimePlayerObservation regenerationPlayer;
	regenerationPlayer.potions = 20;
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
	                (90.0 / 125.0) / 90.0) < 1e-12);
	assert(regenerationCompleted->region.availableHuntSeconds == 180);
	assert(std::abs(regenerationCompleted->region.supplyStaticPotionsPerCombatSecond -
	                (90.0 / 125.0) / 90.0) < 1e-12);

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
	assert(std::abs(sourceStatic - (480.0 / 125.0) / 60.0) < 1e-12);
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
	performance.emplace(5, PlayerBotHuntRegionPerformance{10, 0, 11, 1, atlasRevision, true, {}});
	performance.emplace(6, PlayerBotHuntRegionPerformance{10, 0, 1.25, 1, atlasRevision, false, {}});
	performance.emplace(8, PlayerBotHuntRegionPerformance{10, 0, 1.75, 1, atlasRevision - 1, true, {}});
	const auto filtered = playerBotHuntCorrectionForVariant(7, atlasRevision, performance);
	assert(filtered.sampleCount == 2 && std::abs(filtered.correction - 1.5) < 1e-12);
	assert(std::string(playerBotHuntCorrectionForVariant(4, atlasRevision, {}).source) == "default");

	// An already doubled projection must still learn from a much better outing.
	PlayerBotHuntPolicy underestimated;
	PlayerBotHuntPerformanceSample actualHunt{2400, 600, 79, 2096, 876.34, 2, 2400, false, false};
	const double actualCorrection = 2 * 2096 / 876.34;
	const auto corrected = underestimated.observePerformance(1, atlasRevision, actualHunt);
	assert(corrected.observed && std::abs(corrected.updatedCorrection - actualCorrection) < 1e-12);
	for (uint64_t variant : {1, 2}) {
		const auto learned = playerBotHuntCorrectionForVariant(variant, atlasRevision, underestimated.regionPerformance());
		assert(std::abs(learned.correction - actualCorrection) < 1e-12);
		assert(std::abs(876.34 / 2 * learned.correction - 2096) < 1e-9);
	}
	actualHunt.projectedExperience = 2096;
	actualHunt.observedCorrection = actualCorrection;
	assert(std::abs(underestimated.observePerformance(1, atlasRevision, actualHunt).updatedCorrection -
	                actualCorrection) < 1e-12);

	// Both bounds are valid observations and reusable by local and shared lookup.
	for (const auto& [experience, bound] : {std::pair<uint64_t, double>{1, 0.1}, {2000, 10.0}}) {
		PlayerBotHuntPolicy bounded;
		const auto clamped = bounded.observePerformance(1, atlasRevision, reliableSample(experience));
		assert(clamped.observed && clamped.updatedCorrection == bound);
		for (uint64_t variant : {1, 2}) {
			assert(playerBotHuntCorrectionForVariant(variant, atlasRevision,
			       bounded.regionPerformance()).correction == bound);
		}
		auto repeat = reliableSample(experience);
		repeat.observedCorrection = bound;
		repeat.projectedExperience *= bound;
		assert(bounded.observePerformance(1, atlasRevision, repeat).observed);
	}
	for (double invalid : {0.09, 10.01, std::numeric_limits<double>::infinity(),
	                       std::numeric_limits<double>::quiet_NaN()}) {
		PlayerBotHuntPolicy rejected;
		auto invalidSample = reliableSample(200);
		invalidSample.observedCorrection = invalid;
		assert(!rejected.observePerformance(1, atlasRevision, invalidSample).observed);
	}

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
		const PlayerBotSupplyStocks stocks{{{PlayerBotSupplyKind::HealthPotion, 7618, 2, 1, 20}, potions}};
		goal.missingSupplies = playerBotMandatorySupplyDeficit(stocks, false).missing;
		assert(planner.serviceCandidate(goal).feasible == (potions < 2));
		goal.missingSupplies = playerBotMandatorySupplyDeficit(stocks, true).missing;
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
	gold.availableHuntSeconds = xp.availableHuntSeconds = 60;
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

void typedSupplyStock()
{
	PlayerBotDispositionPolicy policy;
	// One request reproduces the single-kind restock exactly, including the
	// partial-reserve guard, the carried-gold reserve, and capacity limits.
	for (uint32_t count : {0U, 1U, 2U, 5U, 20U}) {
		for (uint64_t money : {0ULL, 44ULL, 45ULL, 90ULL, 136ULL, 146ULL, 900ULL, 2000ULL}) {
			for (uint32_t capacity : {0U, 250U, 100000U}) {
				for (uint32_t threshold : {0U, 1U, 3U, UINT32_MAX}) {
					for (bool survival : {false, true}) {
						const uint32_t target = playerbot::recoveryPotionRestockTargetForReserve(threshold);
						const auto single = policy.restock({count, capacity, money, 7}, 45, 120, threshold, target, survival);
						const auto typed = policy.restockSupplies({{count, 45, 120, threshold, target}}, capacity, money, 7, survival);
						assert(typed.size() == 1 && typed[0].amount == single.amount &&
						       typed[0].insufficientFunds == single.insufficientFunds);
						if (threshold == 0) continue;
						// The health floor of two never exceeds threshold plus one, so it changes nothing.
						const auto health = policy.restockSupplies({{count, 45, 120, threshold, target, 2}}, capacity, money, 7, survival);
						assert(health[0].amount == single.amount && health[0].insufficientFunds == single.insufficientFunds);
					}
				}
			}
		}
	}

	// Floors before targets: the second kind's floor is funded before the first tops up.
	const std::vector<PlayerBotEconomySupplyRequest> pair{{0, 50, 0, 1, 20}, {0, 10, 0, 2, 50}};
	auto restock = policy.restockSupplies(pair, 100000, 300, 0);
	assert(restock[0].amount == 3 && restock[1].amount == 5); // 2x50 + 3x10 floors, then (300-130-100)/50 more
	assert(!restock[0].insufficientFunds && !restock[1].insufficientFunds);
	// Higher priority keeps its floor when gold covers only one floor.
	restock = policy.restockSupplies(pair, 100000, 110, 0);
	assert(restock[0].amount == 2 && restock[1].insufficientFunds && restock[1].amount == 0);
	// Survival buys what gold affords, in priority order, without failing.
	restock = policy.restockSupplies(pair, 100000, 110, 0, true);
	assert(restock[0].amount == 2 && restock[1].amount == 1 && !restock[1].insufficientFunds);
	// One capacity budget: heavy first-kind floors limit the second kind.
	restock = policy.restockSupplies({{0, 1, 100, 1, 2}, {0, 1, 100, 1, 2}}, 300, 10000, 0);
	assert(restock[0].amount == 2 && restock[1].amount == 1);
	// A safety floor above threshold plus one is funded in full, or fails when unaffordable.
	restock = policy.restockSupplies({{0, 50, 0, 1, 10, 3}}, 100000, 150, 0);
	assert(restock[0].amount == 3 && !restock[0].insufficientFunds);
	restock = policy.restockSupplies({{0, 50, 0, 1, 10, 3}}, 100000, 100, 0);
	assert(restock[0].amount == 0 && restock[0].insufficientFunds);
	// Survival keeps floors first under cash and under capacity contention.
	const std::vector<PlayerBotEconomySupplyRequest> empty{{0, 50, 0, 1, 10, 2}, {0, 50, 0, 1, 10, 2}};
	restock = policy.restockSupplies(empty, 100000, 200, 0);
	assert(restock[0].amount == 2 && restock[1].amount == 2);
	restock = policy.restockSupplies(empty, 100000, 200, 0, true);
	assert(restock[0].amount == 2 && restock[1].amount == 2);
	restock = policy.restockSupplies(empty, 100000, 150, 0, true);
	assert(restock[0].amount == 2 && restock[1].amount == 1 && !restock[1].insufficientFunds);
	restock = policy.restockSupplies({{0, 1, 100, 1, 10, 2}, {0, 1, 100, 1, 10, 2}}, 300, 10000, 0, true);
	assert(restock[0].amount == 2 && restock[1].amount == 1);
	// A satisfied or unpriced kind buys nothing.
	restock = policy.restockSupplies({{20, 50, 0, 1, 20}, {0, 0, 0, 1, 20}}, 100000, 10000, 0);
	assert(restock[0].amount == 0 && restock[1].amount == 0 && !restock[1].insufficientFunds);

	const PlayerBotSupplyRule health{PlayerBotSupplyKind::HealthPotion, 7618, 2, 1, 20};
	const PlayerBotSupplyRule mana{PlayerBotSupplyKind::ManaPotion, 7620, 3, 1, 10};
	const PlayerBotSupplyRule ammo{PlayerBotSupplyKind::Ammunition, 2389, 2, 1, 6};
	PlayerBotSupplyStocks stocks{{health, 5}, {mana, 1}, {ammo, 0}};
	auto deficit = playerBotMandatorySupplyDeficit(stocks, false);
	assert(deficit.first && deficit.first->rule.kind == PlayerBotSupplyKind::ManaPotion && deficit.missing == 4);
	assert(std::string(playerBotSupplyReserveReason(deficit.first->rule.kind)) == "mana_potion_reserve");
	assert(!playerBotMandatorySupplyDeficit(stocks, true).first);
	stocks[1].rule.target = 0; // inactive kinds never demand service
	deficit = playerBotMandatorySupplyDeficit(stocks, false);
	assert(deficit.first->rule.kind == PlayerBotSupplyKind::Ammunition && deficit.missing == 2);
	assert(playerBotSupplyStock(stocks, PlayerBotSupplyKind::Ammunition)->count == 0);
	const uint64_t key = playerBotSupplyStockKey(stocks);
	stocks[2].count = 1;
	assert(playerBotSupplyStockKey(stocks) != key);
	assert(playerBotSupplyStockKey({{health, 7}}) == 7); // health-only key equals the potion count

	// Only Paladins stock mana; Knights and every other vocation keep no mana target.
	for (uint16_t vocation : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}) {
		const PlayerBotSupplyRule rule = playerBotSupplyRule(PlayerBotSupplyKind::ManaPotion, vocation);
		assert(rule.kind == PlayerBotSupplyKind::ManaPotion && rule.itemId == playerBotManaPotionItemId && rule.itemId == 7620);
		if (vocation == 3 || vocation == 7) {
			assert(rule.active() && rule.target == 20 && rule.safetyFloor == 2 && rule.returnThreshold == 1);
		} else {
			assert(!rule.active() && rule.target == 0 && rule.safetyFloor == 0 && rule.returnThreshold == 0);
			const PlayerBotSupplyStocks inactive{{health, 20}, {rule, 0}};
			assert(!playerBotMandatorySupplyDeficit(inactive, false).first && !playerBotExhaustedSupply(inactive));
		}
		for (const PlayerBotSupplyKind kind : {PlayerBotSupplyKind::Ammunition, PlayerBotSupplyKind::ThrowingWeapon}) {
			assert(!playerBotSupplyRule(kind, vocation).active());
		}
	}

	for (uint16_t vocation : {3, 7}) {
		const PlayerBotSupplyRule rule = playerBotSupplyRule(PlayerBotSupplyKind::ManaPotion, vocation);
		PlayerBotSupplyStocks supplies{{health, 20}, {rule, rule.safetyFloor}};
		assert(!playerBotMandatorySupplyDeficit(supplies, false).first && !playerBotExhaustedSupply(supplies));
		supplies[1].count = rule.returnThreshold;
		deficit = playerBotMandatorySupplyDeficit(supplies, false);
		assert(deficit.first == &supplies[1] && deficit.missing == 1);
		assert(playerBotExhaustedSupply(supplies) == &supplies[1]);
		assert(std::string(playerBotSupplyExhaustedReason(rule.kind)) == "mana_potion_exhausted");
		supplies[1].count = 0;
		assert(playerBotMandatorySupplyDeficit(supplies, false).missing == 2);
		assert(!playerBotMandatorySupplyDeficit(supplies, true).first);

		// Real health/mana rules share cash (including bank gold) and potion capacity.
		const std::vector<PlayerBotEconomySupplyRequest> potions{
		    {0, 45, 270, health.returnThreshold, health.target, health.safetyFloor},
		    {0, 50, 270, rule.returnThreshold, rule.target, rule.safetyFloor}};
		restock = policy.restockSupplies(potions, 40 * 270, 900, 1000);
		assert(restock[0].amount == 20 && restock[1].amount == 20);
		assert(!restock[0].insufficientFunds && !restock[1].insufficientFunds);
		restock = policy.restockSupplies(potions, 40 * 270, 500, 500);
		assert(restock[0].amount == 20 && restock[1].amount == 2); // exactly 1000 gold, not two budgets
		restock = policy.restockSupplies(potions, 4 * 270, 1900, 0);
		assert(restock[0].amount == 2 && restock[1].amount == 2); // both floors before health's target
		for (bool survival : {false, true}) {
			restock = policy.restockSupplies(potions, 40 * 270, 190, 0, survival);
			assert(restock[0].amount == 2 && restock[1].amount == 2);
			assert(!restock[0].insufficientFunds && !restock[1].insufficientFunds);
		}
		restock = policy.restockSupplies(potions, 40 * 270, 300, 0, true);
		assert(restock[0].amount == 4 && restock[1].amount == 2); // mana's floor precedes health top-up
		restock = policy.restockSupplies(potions, 40 * 270, 150, 0, true);
		assert(restock[0].amount == 2 && restock[1].amount == 1 && !restock[1].insufficientFunds);
		restock = policy.restockSupplies(potions, 3 * 270, 1900, 0, true);
		assert(restock[0].amount == 2 && restock[1].amount == 1);

		// Consumed mana potions and unpaid mana debt feed the same learned hunt budget.
		const auto learned = playerBotObserveSupplyDemand({}, 2, 60, true, 1);
		PlayerBotSupplyKindProfile profile{rule.kind, rule.itemId, rule.target, rule.returnThreshold, learned.demand};
		const auto budget = playerBotSupplyKindBudget(profile, 380);
		assert(learned.updated && budget.routine == 19 && budget.expected == 19 && budget.fits);
		assert(!playerBotSupplyKindBudget(profile, 400).fits);
		profile.count = rule.returnThreshold;
		assert(!playerBotSupplyKindBudget(profile, 0).fits);
	}

	// Throwing-weapon counts include the wielded stack, so one carried spear means no spare.
	for (const auto& [level, target] : std::vector<std::pair<uint32_t, uint32_t>>{
	         {0, 3}, {7, 3}, {8, 3}, {9, 3}, {10, 4}, {11, 4}, {12, 5}, {13, 5},
	         {14, 6}, {15, 6}, {16, 7}, {20, 7}, {UINT32_MAX, 7}}) {
		assert(!playerBotThrowingWeaponRule(0, level).active());
		for (uint16_t itemId : {2389, 3965, 7378}) {
			const auto rule = playerBotThrowingWeaponRule(itemId, level);
			assert(rule.active() && rule.itemId == itemId && rule.target == target);
			assert(rule.safetyFloor == 3 && rule.returnThreshold == 1);
		}
	}
	const PlayerBotSupplyRule spears = playerBotThrowingWeaponRule(2389, 16);
	assert(spears.active() && spears.target == 7);
	PlayerBotSupplyStocks paladinStocks{{health, 0}, {spears, 2}};
	assert(!playerBotExhaustedSupply(paladinStocks)); // health returns through the healing interruption
	deficit = playerBotMandatorySupplyDeficit(paladinStocks, false);
	assert(deficit.first->rule.kind == PlayerBotSupplyKind::HealthPotion && deficit.missing == 3);
	paladinStocks[0].count = 20;
	deficit = playerBotMandatorySupplyDeficit(paladinStocks, false);
	assert(deficit.first->rule.kind == PlayerBotSupplyKind::ThrowingWeapon && deficit.missing == 1);
	assert(std::string(playerBotSupplyReserveReason(deficit.first->rule.kind)) == "throwing_weapon_reserve");
	const uint64_t paladinKey = playerBotSupplyStockKey(paladinStocks);
	paladinStocks[1].count = 1;
	assert(playerBotSupplyStockKey(paladinStocks) != paladinKey);
	assert(playerBotExhaustedSupply(paladinStocks) == &paladinStocks[1]);
	assert(std::string(playerBotSupplyExhaustedReason(PlayerBotSupplyKind::ThrowingWeapon)) == "throwing_weapon_exhausted");
	paladinStocks[1].rule.target = 0;
	assert(!playerBotExhaustedSupply(paladinStocks)); // inactive kinds never end a hunt
	// Every kind keeps its own key field.
	assert(playerBotSupplyStockKey({{ammo, 1}}) != playerBotSupplyStockKey({{spears, 1}}));
	assert(playerBotSupplyStockKey({{spears, 70000}}) == playerBotSupplyStockKey({{spears, 65535}}));

	// Demand learning: higher use corrects at once, cheaper evidence needs a full outing.
	PlayerBotSupplyDemand demand;
	auto update = playerBotObserveSupplyDemand(demand, 0, 0, true);
	assert(!update.updated);
	update = playerBotObserveSupplyDemand(demand, 0, 60, false);
	assert(!update.updated && std::string(update.reason) == "partial_outing");
	update = playerBotObserveSupplyDemand(demand, 12, 60, false);
	assert(update.updated && update.demand.unitsPerCombatSecond == 0.2 && update.demand.samples == 1);
	demand = update.demand;
	update = playerBotObserveSupplyDemand(demand, 6, 60, false);
	assert(!update.updated && update.demand.unitsPerCombatSecond == 0.2);
	update = playerBotObserveSupplyDemand(demand, 6, 60, true);
	assert(update.updated && std::abs(update.demand.unitsPerCombatSecond - 0.18) < 1e-12 && update.demand.samples == 2);
	update = playerBotObserveSupplyDemand({}, 0, 120, true);
	assert(update.updated && update.demand.unitsPerCombatSecond == 0 && update.demand.samples == 1);
	// Unpaid debt counts as demand alongside consumed units.
	update = playerBotObserveSupplyDemand({}, 1, 60, true, 2);
	assert(update.updated && update.debt == 2 && std::abs(update.demand.unitsPerCombatSecond - 0.05) < 1e-12);

	// Hunt fit: stock above the return threshold must cover the learned demand.
	PlayerBotSupplyKindProfile arrows{PlayerBotSupplyKind::Ammunition, 2544, 40, 5, {0.1, 3}};
	assert(playerBotSupplyKindBudget(arrows, 300).fits && playerBotSupplyKindBudget(arrows, 300).expected == 30);
	assert(!playerBotSupplyKindBudget(arrows, 400).fits);
	arrows.count = 5;
	assert(!playerBotSupplyKindBudget(arrows, 0).fits);

	PlayerBotHuntRegion region = routeFixture(1);
	region.availableHuntSeconds = 600;
	region.combatFraction = 0.5;
	region.reconcileSupplies(1);
	assert(region.supplyBudget.fits && region.supplyKindBudgets.empty());
	region.supplyProfile.kinds.push_back({PlayerBotSupplyKind::Ammunition, 2544, 40, 5, {0.2, 1}});
	region.reconcileSupplies(1);
	assert(!region.supplyBudget.fits && region.supplyKindBudgets.size() == 1 &&
	       region.supplyKindBudgets[0].expected == 60 && !region.supplyKindBudgets[0].fits);
	region.supplyProfile.kinds[0].count = 70;
	region.reconcileSupplies(1);
	assert(region.supplyBudget.fits && region.supplyKindBudgets[0].fits);

	// Recovery stays active while any floor is unaffordable, not only health's.
	const uint64_t healthStocked = playerBotRecoverySpendingReserve(20, 2, 50, 100);
	assert(playerBotSupplyFloorSpendingReserve(healthStocked, {}) == healthStocked);
	assert(playerBotSupplyFloorSpendingReserve(healthStocked, {{5, 2}}) == healthStocked); // unknown price, no deficit
	const uint64_t withMana = playerBotSupplyFloorSpendingReserve(healthStocked, {{0, 2, 50}});
	assert(withMana == 200);
	PlayerBotSupplyRecoveryState recovering;
	recovering.update(1, withMana - 100);
	assert(recovering.active());
	recovering.update(2, withMana - 100);
	assert(recovering.active());
	assert(playerBotSupplyFloorSpendingReserve(healthStocked, {{0, 2}}) == UINT64_MAX);
	assert(playerBotSupplyFloorSpendingReserve(UINT64_MAX, {{0, 2, 50}}) == UINT64_MAX);

	PlayerBotGoalPlanner planner;
	PlayerBotGoalPlannerSnapshot goal;
	goal.missingSupplies = 2;
	goal.supplyReserveReason = playerBotSupplyReserveReason(PlayerBotSupplyKind::Ammunition);
	assert(planner.serviceCandidate(goal).feasible && planner.serviceCandidate(goal).reason == "ammunition_reserve");
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

	playerbot::PlayerBotRecordThrottle throttle;
	const auto throttleStart = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
	assert(throttle.admit(throttleStart) == 0u);
	assert(!throttle.admit(throttleStart + std::chrono::seconds(1)));
	assert(!throttle.admit(throttleStart + std::chrono::seconds(4)));
	assert(throttle.admit(throttleStart + playerbot::progressRecordInterval) == 2u);
	assert(throttle.admit(throttleStart + 2 * playerbot::progressRecordInterval) == 0u);

	assert(playerBotDepotRouteSafetyAccepted(true, false, PlayerBotRouteIntent::Optional));
	// Only forced exits may exceed the profile; liquidation never can.
	assert(!playerBotDepotRouteSafetyAccepted(false, false, PlayerBotRouteIntent::Optional));
	assert(playerBotDepotRouteSafetyAccepted(false, false, PlayerBotRouteIntent::ForcedReturn));
	assert(!playerBotDepotRouteSafetyAccepted(false, true, PlayerBotRouteIntent::ForcedReturn));
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
	PlayerBotNavigationPlanMetrics metrics;
	metrics.dangerCost = 1001;
	metrics.maximumHealthLossPerSecond = 1;
	metrics.dangerEvidence = PlayerBotNavigationDangerEvidence::Coarse;
	assert(playerBotNavigationRiskVerdict(risk, metrics) == PlayerBotNavigationRiskVerdict::Unknown);
	metrics.dangerCost = 0;
	metrics.maximumHealthLossPerSecond = 0;
	assert(playerBotNavigationRiskVerdict(risk, metrics) == PlayerBotNavigationRiskVerdict::Unknown);
	metrics.localDangerCost = 501;
	assert(playerBotNavigationLocalRiskVerdict(risk, metrics) == PlayerBotNavigationRiskVerdict::Rejected);
	metrics.localDangerCost = 500;
	metrics.localMaximumHealthLossPerSecond = 0.08;
	assert(playerBotNavigationLocalRiskVerdict(risk, metrics) == PlayerBotNavigationRiskVerdict::Accepted);
	metrics.localMaximumHealthLossPerSecond = 0.081;
	assert(playerBotNavigationLocalRiskVerdict(risk, metrics) == PlayerBotNavigationRiskVerdict::Rejected);
	metrics.dangerEvidence = PlayerBotNavigationDangerEvidence::Detailed;
	metrics.dangerCost = 500;
	metrics.maximumHealthLossPerSecond = 0.08;
	assert(playerBotNavigationRiskVerdict(risk, metrics) == PlayerBotNavigationRiskVerdict::Accepted);
	metrics.dangerCost = 501;
	assert(playerBotNavigationRiskVerdict(risk, metrics) == PlayerBotNavigationRiskVerdict::Rejected);
	metrics.dangerCost = 500;
	metrics.maximumHealthLossPerSecond = 0.081;
	assert(playerBotNavigationRiskVerdict(risk, metrics) == PlayerBotNavigationRiskVerdict::Rejected);
	PlayerBotNavigationRiskProfile cautious = risk;
	cautious.maximumRouteHealthLoss = 0.1;
	PlayerBotNavigationPlanMetrics walking, paid;
	walking.result = paid.result = PlayerBotNavigationResult::Reached;
	walking.dangerCost = 550;
	paid.dangerCost = 300;
	assert(playerBotNavigationRiskVerdict(risk, walking) == PlayerBotNavigationRiskVerdict::Rejected);
	assert(playerBotNavigationRiskVerdict(cautious, paid) == PlayerBotNavigationRiskVerdict::Rejected);
	// Forced departure chooses the least-risk detailed route under either profile.
	assert(playerBotPreferForcedPaidRoute(walking, paid));
	paid.dangerEvidence = PlayerBotNavigationDangerEvidence::Coarse;
	assert(!playerBotPreferForcedPaidRoute(walking, paid));
}

void throwingWeaponDemand()
{
	PlayerBotHuntPolicy policy;
	auto outing = [&](uint32_t consumed, double seconds, bool interrupted) {
		policy.resetCombatEvidence();
		policy.observeCombat({true, seconds, 100, 100, 100, 100, 1});
		return policy.observeSupplyDemand(PlayerBotSupplyKind::ThrowingWeapon, consumed,
		    static_cast<uint64_t>(seconds), interrupted);
	};
	// One early break is not a 3.65/min planning rate.
	auto update = outing(1, 16.42, true);
	assert(!update.updated && update.demand.unitsPerCombatSecond == 0 && update.demand.samples == 0);
	assert(update.demand.accumulatedConsumed == 1 && update.demand.accumulatedCombatSeconds == 16.42);
	assert(update.observedUnitsPerCombatSecond == 1.0 / 16.42);
	update = outing(0, 20, true);
	assert(!update.updated && update.demand.accumulatedConsumed == 1);
	update = outing(0, 23.58, true);
	assert(update.updated && std::abs(update.demand.unitsPerCombatSecond - 1.0 / 60) < 1e-12);
	assert(update.demand.samples == 1 && std::string(update.reason) == "pooled_combat_evidence");
	update = outing(0, 60, true);
	assert(update.updated && std::abs(update.demand.unitsPerCombatSecond - 1.0 / 120) < 1e-12);
	update = outing(1, 60, true);
	assert(update.updated && std::abs(update.demand.unitsPerCombatSecond - 2.0 / 180) < 1e-12);
	// Sustained higher use raises the pooled estimate, not a latest-sample maximum.
	update = outing(6, 60, false);
	assert(update.updated && std::abs(update.demand.unitsPerCombatSecond - 8.0 / 240) < 1e-12);
	update = outing(4, 120, false);
	assert(std::abs(update.demand.unitsPerCombatSecond - 12.0 / 360) < 1e-12);
	const auto prior = update.demand;
	update = outing(1, 0, true);
	assert(!update.updated && update.demand.accumulatedConsumed == prior.accumulatedConsumed);
	policy.resetCombatEvidence();
	policy.observeCombat({true, 60, 100, 100, 100, 100, 1});
	policy.observeDeath();
	update = policy.observeSupplyDemand(PlayerBotSupplyKind::ThrowingWeapon, 3, 60, true);
	assert(!update.updated && std::string(update.reason) == "death_observed");
	assert(update.demand.accumulatedCombatSeconds == prior.accumulatedCombatSeconds);
	assert(update.demand.unitsPerCombatSecond == prior.unitsPerCombatSecond);

	// Other kinds retain their immediate-upward/guarded-downward semantics.
	for (const auto kind : {PlayerBotSupplyKind::HealthPotion, PlayerBotSupplyKind::ManaPotion,
	                       PlayerBotSupplyKind::Ammunition}) {
		PlayerBotHuntPolicy unchanged;
		unchanged.observeCombat({true, 16.42, 100, 100, 100, 100, 1});
		const auto spike = unchanged.observeSupplyDemand(kind, 1, 20, true);
		assert(spike.updated && spike.demand.unitsPerCombatSecond == 1.0 / 16.42);
		unchanged.resetCombatEvidence();
		unchanged.observeCombat({true, 60, 100, 100, 100, 100, 1});
		const auto partial = unchanged.observeSupplyDemand(kind, 0, 60, true);
		assert(!partial.updated && partial.demand.unitsPerCombatSecond == spike.demand.unitsPerCombatSecond);
		assert(partial.demand.accumulatedCombatSeconds == 0);
	}

	// Runtime completion stores sub-guard evidence across interrupted outings.
	PlayerBotHuntRuntime runtime({});
	PlayerBotHuntRegion region = routeFixture(91);
	region.supplyProfile.kinds = {{PlayerBotSupplyKind::ThrowingWeapon, 2389, 3, 1, {}}};
	auto runtimeOuting = [&](uint32_t remaining, uint16_t endingItem, int64_t milliseconds) {
		PlayerBotHuntRuntimePlayerObservation player;
		player.health = player.maximumHealth = 100;
		player.supplyInterrupted = true;
		player.supplies = {{playerBotThrowingWeaponRule(2389, 8), 3}};
		const auto start = std::chrono::steady_clock::time_point{} + std::chrono::seconds(1);
		runtime.selectPlanningRegion(region, player, start);
		runtime.enterHuntArea(player, region.supplyProfile, start);
		runtime.sampleCombat({true, start, 100, 100, 100, 100, 1});
		runtime.sampleCombat({true, start + std::chrono::milliseconds(milliseconds), 100, 100, 100, 100, 1});
		player.supplies = {{playerBotThrowingWeaponRule(endingItem, 8), remaining}};
		return runtime.complete(player, start + std::chrono::milliseconds(milliseconds), 2400);
	};
	const auto first = runtimeOuting(2, 2389, 16420);
	assert(first && first->supplyDemand.size() == 1 && !first->supplyDemand[0].updated);
	const auto zero = runtimeOuting(3, 2389, 43580);
	assert(zero && zero->supplyDemand.size() == 1 && zero->supplyDemand[0].updated);
	assert(std::abs(zero->supplyDemand[0].demand.unitsPerCombatSecond - 1.0 / 60) < 1e-12);
	const auto gained = runtimeOuting(4, 2389, 60000);
	assert(gained && gained->supplyDemand.empty());
	const auto changed = runtimeOuting(2, 7378, 60000);
	assert(changed && changed->supplyDemand.empty());
	PlayerBotHuntPlanningProfile profile;
	profile.supply = region.supplyProfile;
	const auto learned = runtime.planningProfile(profile).supply.kinds[0].demand;
	assert(learned.accumulatedCombatSeconds == 60 && learned.accumulatedConsumed == 1);
	assert(playerBotSupplyKindBudget({PlayerBotSupplyKind::ThrowingWeapon, 2389, 3, 1, learned}, 120).fits);
}

void recoverySupplyReserves()
{
	// A restock preference must not make every cash-recovery outing impossible.
	for (const auto kind : {PlayerBotSupplyKind::ManaPotion, PlayerBotSupplyKind::Ammunition,
	                        PlayerBotSupplyKind::ThrowingWeapon}) {
		auto recovery = routeFixture(94);
		recovery.supplyRecovery = true;
		recovery.currentHealth = recovery.maximumHealth = 100;
		recovery.coinGoldPerMinute = 1;
		recovery.availableHuntSeconds = 60;
		recovery.combatFraction = 1;
		recovery.expectedDamagePerSecond = 0;
		recovery.supplyProfile.potions = 20;
		recovery.supplyProfile.potionHealing = 125;
		recovery.supplyProfile.kinds = {{kind, 2389, 1, 1, {1.0 / 60, 1}}};
		recovery.reconcileSupplies(1);
		assert(recovery.supplyKindBudgets[0].routine == 1);
		assert(recovery.supplyKindBudgets[0].expected == 1 && recovery.recoverySustainable());
		auto normal = recovery;
		normal.supplyRecovery = false;
		normal.reconcileSupplies(1);
		assert(normal.supplyKindBudgets[0].routine == 0 && !normal.supplyBudget.fits);
		auto tooLong = recovery;
		tooLong.availableHuntSeconds = 61;
		tooLong.reconcileSupplies(1);
		assert(tooLong.supplyKindBudgets[0].expected == 2 && !tooLong.recoverySustainable());
		recovery.coinGoldPerMinute = 0;
		assert(!recovery.recoverySustainable());
		recovery.coinGoldPerMinute = 1;
		recovery.currentHealth = 79;
		assert(!recovery.recoverySustainable());
		recovery.currentHealth = 100;
		recovery.supplyProfile.kinds[0].count = 0;
		recovery.supplyProfile.kinds[0].demand = {};
		recovery.reconcileSupplies(1);
		assert(recovery.recoverySustainable() == (kind == PlayerBotSupplyKind::ManaPotion));
	}
	assert(playerBotSupplyHuntReserve(PlayerBotSupplyKind::HealthPotion, 3, true) == 3);
	const auto rule = playerBotThrowingWeaponRule(2389, 8);
	PlayerBotSupplyStocks stocks{{rule, 1}};
	assert(playerBotExhaustedSupply(stocks));
	assert(!playerBotExhaustedSupply(stocks, true)); // Accepted stock must survive continuous readiness checks.
	stocks[0].count = 0;
	assert(playerBotExhaustedSupply(stocks, true)); // Last-unit loss remains an interruption.
	stocks[0].count = 3;
	assert(!playerBotExhaustedSupply(stocks)); // Normal reserves resume after restock.
	for (bool recovery : {false, true}) {
		PlayerBotHuntRuntime runtime({});
		runtime.setSupplyRecovery(recovery);
		PlayerBotHuntRuntimePlanningInput input;
		input.start.emplace();
		input.start->scan.revision = input.cacheRevision = 1;
		auto now = std::chrono::steady_clock::time_point{};
		for (unsigned attempt = 1; attempt <= 3; ++attempt) {
			PlayerBotHuntRuntimeOutcome outcome;
			for (unsigned turn = 0; turn < 8; ++turn) {
				outcome = runtime.advancePlanning(input, now);
				if (outcome.command == PlayerBotHuntRuntimeCommand::ScopeExhausted) break;
			}
			assert(outcome.command == PlayerBotHuntRuntimeCommand::ScopeExhausted);
			assert(outcome.scopeExhaustionAttempt == attempt && outcome.stopForScopeExhaustion == (attempt == 3));
			assert(runtime.advancePlanning(input, now).command == PlayerBotHuntRuntimeCommand::ScopeReevaluationPending);
			now += outcome.retryAfter;
		}
	}
}

void typedSupplyFallback()
{
	PlayerBotHuntRegion lowHealth = routeFixture(92), lowSpears = routeFixture(93);
	lowHealth.supplyBudget = {}; lowSpears.supplyBudget = {};
	lowHealth.supplyBudget.fits = lowSpears.supplyBudget.fits = false;
	lowHealth.supplyBudget.routinePotions = lowSpears.supplyBudget.routinePotions = 19;
	lowHealth.supplyBudget.expectedPotions = 0;
	lowSpears.supplyBudget.expectedPotions = 3; // Both health budgets fit.
	lowHealth.supplyKindBudgets = {{PlayerBotSupplyKind::ThrowingWeapon, 3, 2, 8, false}};
	lowSpears.supplyKindBudgets = {{PlayerBotSupplyKind::ThrowingWeapon, 3, 2, 3, false}};
	assert(playerBotHuntSupplyPressure(lowHealth) == 4);
	assert(playerBotHuntSupplyPressure(lowSpears) == 1.5);
	assert(playerBotPreferHuntRegion(lowSpears, lowHealth));
	assert(!playerBotPreferHuntRegion(lowHealth, lowSpears));
	// Units are not interchangeable: the worst normalized kind governs.
	auto manaBound = lowSpears;
	manaBound.supplyKindBudgets.push_back({PlayerBotSupplyKind::ManaPotion, 3, 2, 6, false});
	assert(playerBotHuntSupplyPressure(manaBound) == 3 && playerBotPreferHuntRegion(lowSpears, manaBound));
	auto betterCoverage = lowHealth;
	betterCoverage.supplyKindBudgets[0].routine = 6;
	assert(playerBotPreferHuntRegion(betterCoverage, lowSpears)); // More units, smaller relative shortage.
	auto empty = lowSpears;
	empty.supplyKindBudgets[0] = {PlayerBotSupplyKind::ThrowingWeapon, 1, 0, 0, false};
	assert(playerBotHuntSupplyPressure(empty) == 1); // No zero-cost empty weapon budget.
	empty.supplyKindBudgets[0].expected = 3;
	assert(playerBotHuntSupplyPressure(empty) == 3 && std::isfinite(playerBotHuntSupplyPressure(empty)));
	// Eligibility, productive time, and fit precede pressure, income, and XP.
	lowSpears.suitable = false;
	assert(playerBotPreferHuntRegion(lowHealth, lowSpears));
	lowSpears.suitable = true; lowSpears.reachable = false;
	assert(playerBotPreferHuntRegion(lowHealth, lowSpears));
	lowSpears.reachable = true;
	auto fitting = lowHealth;
	fitting.supplyBudget.fits = true;
	assert(playerBotPreferHuntRegion(fitting, lowSpears));
	auto zeroDuration = fitting;
	zeroDuration.availableHuntSeconds = 0;
	assert(playerBotPreferHuntRegion(lowSpears, zeroDuration));
	assert(playerBotHuntCandidateCanBeatValidated(lowSpears, zeroDuration));
	assert(playerBotHuntCandidateCanBeatValidated(lowHealth, lowSpears)); // No XP-only pruning of fallback.
	// At equal pressure, preserve cash-pressure and recovery-safety ordering.
	auto income = lowSpears;
	income.cashPressure = lowSpears.cashPressure = true;
	income.coinGoldPerMinute = 10;
	income.score = 1; lowSpears.score = 1000;
	assert(playerBotPreferHuntRegion(income, lowSpears));
	assert(std::string(playerBotHuntSelectionRule(income)) == "lowest_supply_pressure_then_coin_income_then_xp");
	income.supplyRecovery = lowSpears.supplyRecovery = true;
	income.coinGoldPerMinute = lowSpears.coinGoldPerMinute = 10;
	income.threatRatio = 0.1; lowSpears.threatRatio = 0.4;
	assert(playerBotPreferHuntRegion(income, lowSpears));
	assert(std::string(playerBotHuntSelectionRule(income)) == "supply_recovery_pressure_then_coin_safety_then_xp");
	// The comparator remains a strict ordering for sorting mixed tiers.
	std::vector<PlayerBotHuntRegion> candidates{lowHealth, lowSpears, manaBound, fitting, zeroDuration, empty};
	for (const auto& left : candidates) for (const auto& right : candidates) {
		assert(!(playerBotPreferHuntRegion(left, right) && playerBotPreferHuntRegion(right, left)));
		for (const auto& last : candidates) if (playerBotPreferHuntRegion(left, right) && playerBotPreferHuntRegion(right, last))
			assert(playerBotPreferHuntRegion(left, last));
	}
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
	assert(std::string(playerBotHuntSelectionRule(easy)) == "lowest_supply_pressure_then_xp");
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
	// Productive duration is stock-limited, not the difference between a total
	// trip timer and travel. Route validation explicitly fits the stock window.
	costly.reconcileTravel(1500, 1400, 1);
	costly.fitSupplyWindow(13);
	assert(costly.availableHuntSeconds == 125 && costly.supplyBudget.expectedPotions == 1 && costly.supplyBudget.fits);
	assert(costly.score < easy.score && playerBotPreferHuntRegion(costly, easy));
}

void supplyCalibrationRounding()
{
	// Bot Four: no learned heal/food, three potions, and two reserved by the
	// detailed route. A global correction must not turn every positive window
	// into a two-potion budget just because the raw stock count rounded to one.
	PlayerBotHuntRegion region;
	region.supplyProfile.potions = 3;
	region.supplyProfile.potionHealing = 125;
	region.expectedDamagePerSecond = 1;
	region.combatFraction = 0.5;
	region.supplyGlobalLearning = {1.32, 1};
	region.reconcileTravel(120, 0, 1);
	region.reconcileSupplies(2);
	assert(region.supplyBudget.expectedPotions == 2 && !region.supplyBudget.fits);
	region.fitSupplyWindow(2);
	assert(region.availableHuntSeconds == 94 && region.supplyBudget.fits);
	assert(region.supplyBudget.expectedPotions == 1);
	assert(region.supplyBudget.reservedPotions == 2 && region.supplyBudget.routinePotions == 1);
	assert(region.supplyProfile.potions == 3); // Planning does not spend the reserve.
	assert(std::abs(region.supplyStaticPotionsPerCombatSecond - 1.0 / 62.5) < 1e-12);
	assert(std::abs(region.supplyAppliedPotionsPerCombatSecond - 1.32 / 62.5) < 1e-12);
	region.availableHuntSeconds = 95;
	region.reconcileSupplies(2);
	assert(region.supplyBudget.expectedPotions == 2 && !region.supplyBudget.fits);
	region.availableHuntSeconds = 1;
	region.reconcileSupplies(2);
	assert(region.supplyBudget.expectedPotions == 1 && region.supplyBudget.fits);
	region.supplyProfile.potions = 2;
	region.reconcileSupplies(2);
	assert(!region.supplyBudget.fits && region.supplyBudget.routinePotions == 0);
	region.supplyProfile.potions = 4;
	region.availableHuntSeconds = 120;
	region.reconcileSupplies(2);
	assert(region.supplyBudget.expectedPotions == 2 && region.supplyBudget.fits);
	assert(region.supplyBudget.reservedPotions == 2 && region.supplyBudget.routinePotions == 2);

	// Local rates are final absolute estimates, not global-multiplier inputs.
	region.supplyProfile.potions = 3;
	region.supplyCalibration = {region.supplyCapability, 1.0 / 120.0, 1};
	region.reconcileSupplies(2);
	assert(std::string(region.supplyEstimateSource) == "local");
	assert(region.supplyBudget.expectedPotions == 1 && region.supplyBudget.fits);
	region.supplyCalibration.potionsPerCombatSecond = 1.0 / 60.0;
	region.reconcileSupplies(2);
	assert(region.supplyBudget.expectedPotions == 1 && region.supplyBudget.fits);
	region.availableHuntSeconds = 121;
	region.reconcileSupplies(2);
	assert(region.supplyBudget.expectedPotions == 2 && !region.supplyBudget.fits);

	// Calibration cannot bypass typed stock or the recovery health/route floor.
	region.availableHuntSeconds = 120;
	region.supplyProfile.kinds = {{PlayerBotSupplyKind::ManaPotion, playerBotManaPotionItemId, 1, 1, {0.01, 1}}};
	region.reconcileSupplies(2);
	assert(region.supplyBudget.expectedPotions == 1 && !region.supplyBudget.fits);
	assert(region.supplyKindBudgets.size() == 1 && !region.supplyKindBudgets.front().fits);
	region.supplyProfile.kinds.clear();
	region.supplyCalibration = {};
	region.supplyRecovery = true;
	region.currentHealth = region.maximumHealth = 100;
	region.supplyProfile.potions = 2;
	region.availableHuntSeconds = 20;
	region.reconcileSupplies(2);
	assert(region.supplyBudget.expectedPotions == 0 && region.supplyBudget.fits);
	region.availableHuntSeconds = 21;
	region.reconcileSupplies(2);
	assert(region.supplyBudget.expectedPotions == 1 && !region.supplyBudget.fits);
	region.availableHuntSeconds = 0;
	region.recoveryRouteHealthLoss = 21;
	region.reconcileSupplies(2);
	assert(!region.supplyBudget.fits);

	// Learning uses the fractional static denominator, while keeping immediate
	// upward corrections and slower, guarded global/local downward blends.
	region.supplyRecovery = false;
	region.recoveryRouteHealthLoss = 0;
	region.supplyGlobalLearning = {};
	region.supplyProfile.potions = 20;
	region.availableHuntSeconds = 120;
	region.reconcileSupplies(2);
	assert(region.supplyBudget.expectedPotions == 1);
	PlayerBotHuntPolicy upwardPolicy;
	upwardPolicy.observeCombat({true, 60, 100, 100, 0, 0, 1});
	upwardPolicy.observeRecovery(true);
	const auto upward = upwardPolicy.observeSupplies(
	    region, region.supplyCapability, 60, 100, 100, 0, 19, true);
	assert(upward.accepted && upward.globalUpdated && upward.localUpdated);
	assert(upward.globalEstimateDirection == PlayerBotSupplyEstimateDirection::Upward);
	assert(upward.localEstimateDirection == PlayerBotSupplyEstimateDirection::Upward);
	assert(std::abs(upward.globalMultiplierAfter - 1.0 / 0.96) < 1e-12);
	assert(std::abs(upward.calibration.potionsPerCombatSecond - 1.0 / 60.0) < 1e-12);
	PlayerBotHuntPolicy downwardPolicy;
	downwardPolicy.observeCombat({true, 60, 100, 100, 0, 0, 1});
	for (unsigned kill = 0; kill < 3; ++kill) downwardPolicy.observeKill();
	const auto downward = downwardPolicy.observeSupplies(
	    region, region.supplyCapability, 120, 100, 100, 0, 20, false);
	assert(downward.accepted && downward.globalUpdated && downward.localUpdated);
	assert(downward.globalEstimateDirection == PlayerBotSupplyEstimateDirection::Downward);
	assert(downward.localEstimateDirection == PlayerBotSupplyEstimateDirection::Downward);
	assert(std::abs(downward.globalMultiplierAfter - 0.9) < 1e-12);
	assert(std::abs(downward.calibration.potionsPerCombatSecond - 0.8 / 62.5) < 1e-12);
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

void firstHealingBootstrap()
{
	constexpr uint32_t cashBuffer = 100, spellPrice = 170;
	const auto stocked = playerBotRecoverySpendingReserve(3, 3, 45, cashBuffer);
	const auto first = playerBotSpellTrainingSpendingReserve(stocked, cashBuffer, true);
	const auto normal = playerBotSpellTrainingSpendingReserve(stocked, cashBuffer, false);
	assert(first == 0 && normal == cashBuffer);
	assert(playerBotAffordableAfterReserve(170, first, spellPrice));
	assert(!playerBotAffordableAfterReserve(169, first, spellPrice));
	assert(playerBotAffordableAfterReserve(270, normal, spellPrice));
	assert(!playerBotAffordableAfterReserve(269, normal, spellPrice));

	// Releasing cash does not release the missing health potion's 45 gold.
	const auto missingHealth = playerBotRecoverySpendingReserve(2, 3, 45, cashBuffer);
	const auto healthFloor = playerBotSpellTrainingSpendingReserve(missingHealth, cashBuffer, true);
	assert(healthFloor == 45);
	assert(!playerBotAffordableAfterReserve(170, healthFloor, spellPrice));
	assert(!playerBotAffordableAfterReserve(214, healthFloor, spellPrice));
	assert(playerBotAffordableAfterReserve(215, healthFloor, spellPrice));
	assert(playerBotAffordableAfterReserve(217, healthFloor, spellPrice));

	// One spear and two mana potions must also remain funded.
	const auto allFloors = playerBotSupplyFloorSpendingReserve(missingHealth, {{2, 3, 10}, {0, 2, 50}});
	const auto firstFloors = playerBotSpellTrainingSpendingReserve(allFloors, cashBuffer, true);
	assert(firstFloors == 155);
	assert(!playerBotAffordableAfterReserve(324, firstFloors, spellPrice));
	assert(playerBotAffordableAfterReserve(325, firstFloors, spellPrice));
	assert(playerBotSpellTrainingSpendingReserve(allFloors, cashBuffer, false) == 255);

	// Unknown health reserve or a missing typed-supply quote must stay unknown.
	for (const auto reserve : {UINT64_MAX,
	     playerBotSupplyFloorSpendingReserve(missingHealth, {{2, 3}}),
	     playerBotSupplyFloorSpendingReserve(missingHealth, {{0, 2}}), uint64_t{99}}) {
		for (bool firstHeal : {false, true}) {
			const auto blocked = playerBotSpellTrainingSpendingReserve(reserve, cashBuffer, firstHeal);
			assert(blocked == UINT64_MAX);
			assert(!playerBotAffordableAfterReserve(UINT64_MAX, blocked, spellPrice));
		}
	}

	PlayerBotSpellTrainingPlanner planner;
	PlayerBotSpellTrainingPlannerSnapshot state;
	state.totalMoney = spellPrice;
	state.maximumRouteDangerCost = 100;
	state.maximumRouteDanger = 10;
	PlayerBotSpellOfferSnapshot offer;
	offer.spellName = "Light Healing";
	offer.price = spellPrice;
	offer.reserve = first;
	offer.inScope = offer.registryMatches = offer.implementedUse = offer.vocationEligible = true;
	offer.levelEligible = offer.premiumEligible = offer.suppliesReady = offer.route.reachable = true;
	offer.potionReserve = 2;
	state.offers = {offer};
	const auto accepted = planner.select(state);
	assert(accepted.selected && accepted.selected->reserve == 0 && accepted.selected->potionReserve == 2);
	auto rejected = [&](const char* reason) {
		const auto decision = planner.select(state);
		assert(!decision.selected && decision.rejections.size() == 1);
		assert(decision.rejections.front().reason == reason);
		state.offers = {offer};
	};
	state.offers.front().reserve = normal;
	rejected("unaffordable_after_reserves"); // Later heals and nonhealing spells.
	state.offers.front().reserve = UINT64_MAX;
	rejected("recovery_reserve_unavailable");
	state.offers.front().registryMatches = false; rejected("spell_registry_mismatch");
	state.offers.front().implementedUse = false; rejected("no_implemented_use");
	state.offers.front().vocationEligible = false; rejected("vocation_ineligible");
	state.offers.front().levelEligible = false; rejected("level_ineligible");
	state.offers.front().premiumEligible = false; rejected("premium_ineligible");
	state.offers.front().known = true; rejected("already_learned");
	state.offers.front().suppliesReady = false; rejected("supply_reserve_unmet");
	state.offers.front().route.reachable = false; rejected("trainer_unreachable");
	state.offers.front().route.dangerCost = 101; rejected("route_danger_above_tolerance");
	state.offers.front().route.maximumDanger = 11; rejected("route_peak_danger_above_tolerance");
}

void recoverySpellPriority()
{
	PlayerBotSpellTrainingPlanner training;
	PlayerBotSpellTrainingPlannerSnapshot trainingSnapshot;
	trainingSnapshot.reserveAvailable = true;
	trainingSnapshot.totalMoney = 1000;
	PlayerBotSpellOfferSnapshot offer;
	offer.spellName = "A Healing Spell";
	offer.inScope = offer.registryMatches = offer.implementedUse = offer.vocationEligible = true;
	offer.levelEligible = offer.premiumEligible = offer.suppliesReady = offer.route.reachable = true;
	offer.worthLearning = false;
	trainingSnapshot.offers = {offer};
	auto rejected = training.select(trainingSnapshot);
	assert(!rejected.selected && rejected.rejections.size() == 1);
	assert(rejected.rejections.front().reason == "uneconomical_healing_spell");
	trainingSnapshot.offers.front().worthLearning = true;
	assert(training.select(trainingSnapshot).selected);
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
	snapshot.missingSupplies = 2;
	candidates = planner.candidates(snapshot);
	assert(candidate(Goal::LearnSpell).feasible && candidate(Goal::LearnSpell).reason == "priority_recovery_spell");
	assert(candidate(Goal::Service).feasible && candidate(Goal::Service).reason == "healing_reserve");
	assert(!candidate(Goal::Hunt).feasible && candidate(Goal::Hunt).reason == "deferred_recovery_spell");
	snapshot.missingSupplies = 0;
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
	assert(std::abs(region.projectedExperience - 67.5) < 1e-9);
	assert(region.score == region.projectedExperience);
	assert(region.routeDangerCost == 17);
	region.reconcileTravel(300, 23.6, 1);
	assert(std::abs(region.score - 15) < 1e-9);
	region.reconcileTravel(900, 23.6, 1);
	assert(std::abs(region.score - 45) < 1e-9);

	// A changed horizon needs a fresh weighted stamina multiplier, not the old XP/s.
	region.observedCorrection = 0.8;
	region.reconcileTravel(900, 100, 1 + 0.5 * 180 / 900);
	assert(std::abs(region.score - 39.6) < 1e-9);
	assert(region.staminaExperienceMultiplier == 1.1);
	region.reconcileTravel(900, 1000, 1);
	assert(region.availableHuntSeconds == 900 && std::abs(region.score - 36) < 1e-9);
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
	PlayerBotEquipmentPolicy policy;
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
	knight.vocationId = 2;
	acquisition = policy.standardBackpackAcquisition(knight, 0, false, 0, 0);
	assert(!acquisition.eligible && std::string(acquisition.rejection) == "unsupported_vocation");

	PlayerBotEquipmentLoadout loadout;
	PlayerBotEquipmentReadinessInput readiness{false, true, 10000, 100};
	auto combat = policy.combatReadiness(PlayerBotEquipmentPlayerSnapshot{0, 0, 4}, loadout, false, readiness);
	// A missing weapon is bought before the backpack; the controller stops only without an offer.
	assert(combat.recovery == "acquire_weapon" && combat.terminalReason.empty());
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
	loadout.items[4] = {};
	combat = policy.combatReadiness(PlayerBotEquipmentPlayerSnapshot{0, 0, 4}, loadout, false, readiness);
	assert(combat.recovery == "acquire_armor" && combat.terminalReason.empty());
}

static void carriedShieldUpgrade()
{
	PlayerBotEquipmentPolicy policy;
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

static void combatStyles()
{
	static_assert(playerBotCombatStyle(4).managed && playerBotCombatStyle(4).weapons == PlayerBotWeaponFamily::Melee);
	static_assert(!playerBotCombatStyle(0).managed && playerBotCombatStyle(0).weapons == PlayerBotWeaponFamily::Melee);
	static_assert(playerBotCombatStyle(3).managed && playerBotCombatStyle(3).weapons == PlayerBotWeaponFamily::Distance);
	for (uint16_t unsupported : {uint16_t{1}, uint16_t{2}, uint16_t{8}}) {
		assert(!playerBotCombatStyle(unsupported).managed);
		assert(playerBotCombatStyle(unsupported).weapons == PlayerBotWeaponFamily::None);
	}

	PlayerBotEquipmentPolicy policy;
	PlayerBotEquipmentLoadout loadout;
	PlayerBotEquipmentItemSnapshot weapon;
	weapon.itemId = 2376;
	weapon.weaponType = PlayerBotEquipmentWeaponType::Sword;
	weapon.attack = 14;
	weapon.left = weapon.pickupable = true;
	loadout.items[6] = weapon;
	PlayerBotEquipmentItemSnapshot armor;
	armor.itemId = 2463;
	armor.armorSlot = armor.pickupable = true;
	armor.armor = 10;
	loadout.items[4] = armor;
	const PlayerBotEquipmentReadinessInput readiness{true, true, 10000, 100};

	// Rookgaard rewards rely on melee readiness without the managed Knight loop.
	PlayerBotEquipmentPlayerSnapshot player{0, 0, 0};
	assert(!policy.managesEquipment(player) && policy.weaponReady(player, loadout) && policy.loadoutReady(player, loadout, readiness));
	player.vocationId = 4;
	assert(policy.managesEquipment(player) && policy.loadoutReady(player, loadout, readiness));
	assert(std::string(PlayerBotEquipmentPolicy::weaponRequirement(player)) == "legal_melee_weapon");
	player.vocationId = 2;
	assert(!policy.managesEquipment(player) && !policy.weaponReady(player, loadout));
	assert(policy.combatReadiness(player, {}, false, readiness).ready);
	assert(std::string(PlayerBotEquipmentPolicy::weaponRequirement(player)) == "legal_weapon");
	player.vocationId = 3;
	assert(policy.managesEquipment(player) && !policy.weaponReady(player, loadout));
	assert(std::string(PlayerBotEquipmentPolicy::weaponRequirement(player)) == "legal_distance_weapon");

	player.vocationId = 4;
	loadout.items[6].weaponType = PlayerBotEquipmentWeaponType::Distance;
	assert(!policy.weaponReady(player, loadout));
	assert(policy.combatReadiness(player, loadout, false, readiness).recovery == "acquire_weapon");
	PlayerBotEquipmentLoadout empty;
	auto evaluation = policy.evaluateCandidate(player, loadout.items[6], empty, {}, {}, false, readiness, 0, true,
	    [](const PlayerBotCombatProfile&) { return PlayerBotEquipmentHuntSummary{}; });
	assert(evaluation.rejection == "unsupported_weapon_type");
}

static PlayerBotEquipmentItemSnapshot distanceItem(uint16_t itemId, int32_t attack, int32_t maxHitChance, uint8_t breakChance,
                                                 uint16_t minimumLevel = 0)
{
	PlayerBotEquipmentItemSnapshot item;
	item.itemId = itemId;
	item.weaponType = PlayerBotEquipmentWeaponType::Distance;
	item.attack = attack;
	item.maxHitChance = maxHitChance;
	item.breakChance = breakChance;
	item.minimumLevel = minimumLevel;
	item.shootRange = 3;
	item.left = item.right = item.pickupable = true;
	return item;
}

static void paladinLoadouts()
{
	PlayerBotEquipmentPolicy policy;
	PlayerBotEquipmentPlayerSnapshot paladin;
	paladin.vocationId = 3;
	paladin.level = 8;
	paladin.distanceSkill = 20;
	paladin.shieldSkill = 15;
	const PlayerBotEquipmentReadinessInput readiness{true, true, 10000, 100};
	const auto noHunts = [](const PlayerBotCombatProfile&) { return PlayerBotEquipmentHuntSummary{}; };
	// Loaded 8.60 values: spear 25/76%/3%, hunting spear 32/80%/6% at 20, royal spear 35/80%/3% at 25.
	const auto spear = distanceItem(2389, 25, 76, 3);
	const auto huntingSpear = distanceItem(3965, 32, 80, 6, 20);
	const auto royalSpear = distanceItem(7378, 35, 80, 3, 25);
	PlayerBotEquipmentItemSnapshot shield;
	shield.itemId = 2526;
	shield.weaponType = PlayerBotEquipmentWeaponType::Shield;
	shield.defense = 16;
	shield.left = shield.right = shield.pickupable = true;
	PlayerBotEquipmentItemSnapshot armor;
	armor.itemId = 2467;
	armor.armorSlot = armor.pickupable = true;
	armor.armor = 6;

	PlayerBotEquipmentLoadout loadout;
	loadout.items[4] = armor;
	loadout.items[5] = shield;
	loadout.itemIds[5] = shield.itemId;
	auto readinessResult = policy.combatReadiness(paladin, loadout, false, readiness);
	assert(readinessResult.recovery == "acquire_weapon" && readinessResult.terminalReason.empty());
	assert(policy.fillsReadinessGap(paladin, loadout, spear) && !policy.fillsReadinessGap(paladin, loadout, shield));
	PlayerBotEquipmentItemSnapshot sword;
	sword.itemId = 2376;
	sword.weaponType = PlayerBotEquipmentWeaponType::Sword;
	sword.attack = 14;
	sword.left = sword.pickupable = true;
	assert(!policy.fillsReadinessGap(paladin, loadout, sword));

	// A spear has no defense, so it trades away fist defense; filling the weapon gap still wins.
	const auto unarmed = policy.combatProfile(paladin, loadout);
	auto evaluation = policy.evaluateCandidate(paladin, spear, loadout, unarmed, {}, false, readiness, 0, true, noHunts);
	assert(evaluation.rejection.empty() && evaluation.rule == PlayerBotEquipmentDecisionRule::ReadinessRepair);
	assert(static_cast<uint8_t>(evaluation.slot) == 6 && evaluation.candidateReady);
	assert(evaluation.profile.hitChance == 76 && !evaluation.profile.blockedByShield && evaluation.profile.attackRange == 3);
	assert(evaluation.profile.attack == 25 && evaluation.profile.attackSkill == 20);
	assert(policy.evaluateCandidate(paladin, sword, loadout, unarmed, {}, false, readiness, 0, true, noHunts).rejection ==
	       "unsupported_weapon_type");

	loadout.items[6] = spear;
	loadout.itemIds[6] = spear.itemId;
	assert(policy.weaponReady(paladin, loadout) && policy.combatReadiness(paladin, loadout, false, readiness).ready);
	const auto armed = policy.combatProfile(paladin, loadout);
	assert(armed.defense == policy.combatProfile(paladin, [&] { auto copy = loadout; copy.items[6] = {}; return copy; }()).defense);

	// Breakage is a cost trade-off: Hunting Spear hits harder but breaks twice as often.
	paladin.level = 25;
	const auto leveled = policy.combatProfile(paladin, loadout);
	evaluation = policy.evaluateCandidate(paladin, huntingSpear, loadout, leveled, {}, true, readiness, 0, true, noHunts);
	assert(evaluation.rejection == "ambiguous_tradeoff");
	evaluation = policy.evaluateCandidate(paladin, royalSpear, loadout, leveled, {}, true, readiness, 0, true, noHunts);
	assert(evaluation.rejection.empty() && evaluation.rule == PlayerBotEquipmentDecisionRule::ParetoImprovement);
	assert(evaluation.profile.hitChance == 80);
	PlayerBotEquipmentCarriedCandidate carriedHuntingSpear{huntingSpear, true};
	carriedHuntingSpear.item.inContainer = true;
	assert(!policy.findCarriedUpgrade(paladin, loadout, {carriedHuntingSpear}));
	PlayerBotEquipmentCarriedCandidate carriedRoyalSpear{royalSpear, true};
	carriedRoyalSpear.item.inContainer = true;
	assert(policy.findCarriedUpgrade(paladin, loadout, {carriedRoyalSpear}));
	paladin.level = 24;
	assert(policy.evaluateCandidate(paladin, royalSpear, loadout, leveled, {}, true, readiness, 0, true, noHunts).rejection ==
	       "level_ineligible");

	// A broken hand spear leaves the slot empty; a carried spare refills it.
	auto broken = loadout;
	broken.items[6] = {};
	broken.itemIds[6] = 0;
	PlayerBotEquipmentCarriedCandidate spare{spear, true};
	spare.item.inContainer = true;
	const auto refill = policy.findCarriedUpgrade(paladin, broken, {spare});
	assert(refill && static_cast<uint8_t>(refill->upgrade.slot) == 6);
	// Spare stock follows the wielded throwing weapon, or the carried one readiness would equip.
	assert(policy.isThrowingWeapon(paladin, spear) && !policy.isThrowingWeapon(paladin, shield));
	assert(policy.throwingWeaponSupplyItem(paladin, loadout, {}) == spear.itemId);
	assert(policy.throwingWeaponSupplyItem(paladin, broken, {shield, spear}) == spear.itemId);
	assert(policy.throwingWeaponSupplyItem(paladin, broken, {royalSpear}) == 0); // level 24 cannot wield it
	assert(policy.throwingWeaponSupplyItem(paladin, broken, {}) == 0);
	assert(policy.throwingWeaponSupplyItem(paladin, broken, {}, spear) == spear.itemId);
	assert(policy.throwingWeaponSupplyItem(paladin, broken, {royalSpear}, spear) == spear.itemId);
	assert(policy.throwingWeaponSupplyItem(paladin, broken, {}, royalSpear) == 0);
	auto exhaustedReadiness = readiness;
	exhaustedReadiness.suppliesReady = false;
	exhaustedReadiness.restockableWeaponMissing = true;
	assert(policy.combatReadiness(paladin, broken, false, exhaustedReadiness).recovery == "service");
	assert(policy.combatReadiness(paladin, broken, true, exhaustedReadiness).recovery == "equip_carried");
	exhaustedReadiness.restockableWeaponMissing = false;
	assert(policy.combatReadiness(paladin, broken, false, exhaustedReadiness).recovery == "acquire_weapon");
	auto mirrored = broken;
	mirrored.items[6] = shield;
	mirrored.itemIds[6] = shield.itemId;
	mirrored.items[5] = {};
	mirrored.itemIds[5] = 0;
	assert(policy.throwingWeaponSupplyItem(paladin, mirrored, {}, spear) == spear.itemId);
	assert(policy.throwingWeaponSupplyItem(paladin, mirrored, {spear}) == spear.itemId);
	const auto mirroredRefill = policy.findCarriedUpgrade(paladin, mirrored, {spare});
	assert(mirroredRefill && static_cast<uint8_t>(mirroredRefill->upgrade.slot) == 5);
	// Banking can put coins in the newly empty hand before restocking.
	auto occupied = broken;
	occupied.items[6].itemId = occupied.itemIds[6] = 2148;
	occupied.items[6].pickupable = true;
	assert(policy.throwingWeaponSupplyItem(paladin, occupied, {}, spear) == spear.itemId);
	const auto occupiedRefill = policy.findCarriedUpgrade(paladin, occupied, {spare});
	assert(occupiedRefill && static_cast<uint8_t>(occupiedRefill->upgrade.slot) == 6);
	paladin.level = 25;
	assert(policy.throwingWeaponSupplyItem(paladin, broken, {spear, royalSpear}) == royalSpear.itemId);
	paladin.level = 24;
	auto unbreakable = spear;
	unbreakable.breakChance = 0;
	assert(!policy.isThrowingWeapon(paladin, unbreakable));
	PlayerBotEquipmentPlayerSnapshot spearKnight = paladin;
	spearKnight.vocationId = 4;
	assert(policy.throwingWeaponSupplyItem(spearKnight, loadout, {}) == 0);
	assert(policy.throwingWeaponSupplyItem(spearKnight, broken, {}, spear) == 0);

	// Launchers are modeled but not acquired until #255.
	auto bow = distanceItem(2456, 0, -1, 0);
	bow.ammoType = 2;
	bow.shootRange = 6;
	bow.twoHanded = true;
	bow.right = false;
	PlayerBotEquipmentItemSnapshot arrow;
	arrow.itemId = 2544;
	arrow.weaponType = PlayerBotEquipmentWeaponType::Ammo;
	arrow.ammoType = 2;
	arrow.attack = 25;
	arrow.maxHitChance = 91;
	arrow.ammoSlot = arrow.pickupable = true;
	auto bolt = arrow;
	bolt.itemId = 2543;
	bolt.ammoType = 1;
	bolt.attack = 30;
	assert(policy.evaluateCandidate(paladin, bow, loadout, armed, {}, true, readiness, 0, true, noHunts).rejection ==
	       "launcher_loadout_deferred");
	PlayerBotEquipmentLoadout launcherLoadout;
	launcherLoadout.items[4] = armor;
	launcherLoadout.items[6] = bow;
	// An unfed launcher is no weapon; a spear replaces it.
	assert(!policy.weaponReady(paladin, launcherLoadout) && policy.fillsReadinessGap(paladin, launcherLoadout, spear));
	assert(policy.combatProfile(paladin, launcherLoadout).hitChance == 0);
	launcherLoadout.items[10] = bolt;
	assert(!policy.weaponReady(paladin, launcherLoadout));
	launcherLoadout.items[10] = arrow;
	assert(policy.weaponReady(paladin, launcherLoadout));
	// A fed launcher stocks ammunition, not spare spears.
	assert(!policy.isThrowingWeapon(paladin, bow) && policy.throwingWeaponSupplyItem(paladin, launcherLoadout, {spear}) == 0);
	assert(policy.throwingWeaponSupplyItem(paladin, launcherLoadout, {}, spear) == 0);
	const auto bowProfile = policy.combatProfile(paladin, launcherLoadout);
	assert(bowProfile.attack == 25 && bowProfile.hitChance == 91 && bowProfile.attackRange == 6 && !bowProfile.blockedByShield);
	// A launcher's own hit bonus adds to its ammunition's chance.
	launcherLoadout.items[6].hitChance = 3;
	assert(policy.combatProfile(paladin, launcherLoadout).hitChance == 94);

	// Formula tiers apply when loaded data has no flat chance (WeaponDistance::useWeapon).
	auto formula = loadout;
	formula.items[6].maxHitChance = -1;
	assert(policy.combatProfile(paladin, formula).hitChance == 21);
	launcherLoadout.items[6].hitChance = 0;
	launcherLoadout.items[10].maxHitChance = -1;
	assert(policy.combatProfile(paladin, launcherLoadout).hitChance == 25);

	// Melee keeps certain hits, so Knight comparisons are unchanged.
	PlayerBotEquipmentPlayerSnapshot knight;
	knight.vocationId = 4;
	PlayerBotEquipmentLoadout melee;
	melee.items[6] = sword;
	const auto meleeProfile = policy.combatProfile(knight, melee);
	assert(meleeProfile.hitChance == 100 && meleeProfile.blockedByShield && meleeProfile.attackRange == 1);
}

void productiveHuntTripContracts()
{
	using namespace std::chrono;
	const auto start = steady_clock::time_point{} + seconds(1);
	PlayerBotHuntRuntimePlayerObservation player;
	player.health = player.maximumHealth = 100;
	player.potions = 100;
	for (bool recovery : {false, true}) {
		auto region = routeFixture(700);
		region.supplyRecovery = recovery;
		region.coinGoldPerMinute = 6;
		region.currentHealth = region.maximumHealth = 100;
		region.combatFraction = 1;
		region.predictedFightSeconds = 10;
		region.reconcileTravel(600, 300, 1);
		assert(region.availableHuntSeconds == 600); // transit does not consume productive time
		PlayerBotHuntRuntime runtime({});
		runtime.selectPlanningRegion(region, player, start);
		runtime.beginCycle(start, 600);
		assert(!runtime.deadlineReached(start + seconds(659)));
		assert(runtime.enterHuntArea(player, region.supplyProfile, start + seconds(300)));
		assert(runtime.effectiveDurationSeconds() == 600);
		assert(!runtime.deadlineReached(start + seconds(899)));
		assert(runtime.deadlineReached(start + seconds(900)));
		assert(runtime.enterHuntArea(player, region.supplyProfile, start + seconds(400)));
		assert(runtime.deadlineReached(start + seconds(900))); // duplicate arrival cannot extend

		PlayerBotHuntRuntime timedOut({});
		timedOut.selectPlanningRegion(region, player, start);
		timedOut.beginCycle(start, 600);
		assert(timedOut.deadlineReached(start + seconds(660)));
		assert(!timedOut.arrivalTimeout(start + seconds(659)));
		const auto timeout = timedOut.arrivalTimeout(start + seconds(660));
		assert(timeout && timeout->variantId == 700 && timeout->duration == minutes(10));
		assert(!runtime.arrivalTimeout(start + seconds(1000)));
		assert(!timedOut.enterHuntArea(player, region.supplyProfile, start + seconds(660)));
		timedOut.sampleCombat({true, start + seconds(1), 100, 100, 100, 100, 1});
		timedOut.sampleCombat({true, start + seconds(300), 100, 100, 100, 100, 1});
		for (int kill = 0; kill < 5; ++kill) timedOut.observeKill();
		const auto failed = timedOut.complete(player, start + seconds(660), 600);
		assert(failed && failed->durationSeconds == 0 && timedOut.regionPerformance().empty());
		assert(std::string(failed->performance.evidenceReason) == "hunt_arrival_not_observed");
		assert(!failed->supplyObservation.arrivalBaselineObserved);
	}

	auto limited = routeFixture(701);
	limited.currentHealth = limited.maximumHealth = 100;
	limited.coinGoldPerMinute = 6;
	limited.combatFraction = 1;
	limited.expectedDamagePerSecond = 1;
	limited.supplyProfile.potionHealing = 125;
	limited.predictedFightSeconds = 20;
	limited.supplyProfile.kinds = {{PlayerBotSupplyKind::ThrowingWeapon, 2389, 3, 1, {}, 0.01, 9}};
	limited.routeValidated = true;
	limited.recoveryRouteHealthLoss = 20;
	limited.estimatedReturnSeconds = 50;
	limited.reconcileTravel(600, 100, 1);
	limited.fitSupplyWindow(1);
	assert(limited.availableHuntSeconds == 180); // 200 combat seconds minus transit combat
	assert(limited.supplyBudget.fits && limited.productiveWindow());
	limited.supplyRecovery = true;
	limited.fitSupplyWindow(1);
	assert(limited.availableHuntSeconds == 280); // recovery's reserve remains zero, not a 120 s cap
	limited.predictedFightSeconds = 281;
	assert(!limited.recoverySustainable()); // no arbitrary 30-second minimum
	limited.predictedFightSeconds = 20;
	PlayerBotHuntRuntime depleted({});
	depleted.selectPlanningRegion(limited, player, start);
	depleted.beginCycle(start, 600);
	auto live = limited.supplyProfile;
	live.kinds.clear(); // broken last weapon is not an omitted requirement
	assert(!depleted.enterHuntArea(player, live, start + seconds(100)));
	assert(!depleted.arrived());
	live = limited.supplyProfile;
	player.health = 79;
	assert(!depleted.enterHuntArea(player, live, start + seconds(100)));
	player.health = 100;
	live.kinds[0].count = 1;
	assert(depleted.enterHuntArea(player, live, start + seconds(100)));
	assert(depleted.effectiveDurationSeconds() < 280); // actual arrival stock wins

	assert(playerBotThrowingBreakDemand(1, 2000) == 0.005);
	PlayerBotSupplyKindProfile spear{PlayerBotSupplyKind::ThrowingWeapon, 2389, 3, 1, {}, 0.005, 9};
	assert(playerBotSupplyKindBudget(spear, 600).expected == 3);
	spear.demand = {0, 1}; // observed zero, unlike unknown zero, is valid evidence
	assert(playerBotSupplyKindBudget(spear, 600).expected == 0);
	spear.count = 0;
	assert(!playerBotSupplyKindBudget(spear, 600, true).fits);

	auto far = routeFixture(702), near = routeFixture(703);
	far.cashPressure = near.cashPressure = true;
	far.coinGoldPerMinute = 10;
	near.coinGoldPerMinute = 5;
	far.availableHuntSeconds = near.availableHuntSeconds = 300;
	far.estimatedTravelSeconds = far.estimatedReturnSeconds = 600;
	assert(playerBotPreferHuntRegion(near, far)); // lower gross rate, more useful trip
	assert(playerBotHuntCandidateCanBeatValidated(near, far));
	near.freezeEconomicKey();
	far.freezeEconomicKey();
	near.healthPotionUnitPrice = 45;
	near.supplyBudget.expectedPotions = 1;
	assert(playerBotPreferHuntRegion(near, far)); // Live costs cannot reverse the frozen choice
	near.supplyBudget.expectedPotions = 0;
	near.exitFare = 30;
	assert(playerBotPreferHuntRegion(near, far)); // Actual fare affordability remains a separate gate
}

void frozenHuntEconomicRankingContracts()
{
	using Hint = PlayerBotHuntProfitabilityHint;
	auto score = [](double income, double travel = 0, double productive = 60, uint32_t price = 45) {
		auto region = routeFixture(750);
		region.cashPressure = true;
		region.coinGoldPerMinute = income;
		region.estimatedTravelSeconds = travel;
		region.availableHuntSeconds = productive;
		region.supplyBudget.expectedPotions = 1;
		region.healthPotionUnitPrice = price;
		region.freezeEconomicKey();
		return region;
	};
	auto hint = [](const PlayerBotHuntRegion& region) { return region.economicKey().profitabilityHint; };
	auto rate = [](const PlayerBotHuntRegion& region) { return region.economicKey().coinGoldPerMinute; };
	auto sameKey = [](const PlayerBotHuntRegion& left, const PlayerBotHuntRegion& right) {
		const auto a = left.economicKey(), b = right.economicKey();
		assert(a.cashPressure == b.cashPressure && a.supplyRecovery == b.supplyRecovery);
		assert(a.profitabilityHint == b.profitabilityHint && a.coinGoldPerMinute == b.coinGoldPerMinute);
	};
	auto loss = score(2, 20); // Old signed net was -43: no least-loss exception.
	auto fasterLoss = score(20, 20);
	auto zero = score(45, 20); // Exact cost coverage remains uncertain.
	auto positive = score(47, 20);
	assert(hint(loss) == Hint::Uncertain && hint(zero) == Hint::Uncertain);
	assert(hint(positive) == Hint::LikelySurplus);
	assert(playerBotPreferHuntRegion(fasterLoss, loss));
	assert(!playerBotHuntCandidateCanBeatValidated(loss, fasterLoss));
	assert(playerBotPreferHuntRegion(positive, zero));
	// Surplus hint precedes gross rate; costs never become a signed objective.
	auto highUncertain = score(1000, 0, 60, 0);
	assert(hint(highUncertain) == Hint::Uncertain && rate(highUncertain) > rate(positive));
	assert(playerBotPreferHuntRegion(positive, highUncertain));
	assert(!playerBotHuntCandidateCanBeatValidated(highUncertain, positive));

	for (const auto& trip : {loss, zero, positive, highUncertain}) {
		auto farther = trip;
		farther.frozenEconomicKey.reset(); // A different cheap-scoring snapshot.
		farther.estimatedTravelSeconds += 600;
		farther.freezeEconomicKey();
		assert(hint(farther) == hint(trip) && rate(farther) < rate(trip));
		assert(!playerBotPreferHuntRegion(farther, trip));
		assert(!playerBotHuntCandidateCanBeatValidated(farther, trip));
	}
	assert(std::abs(rate(score(10, 60, 120, 0)) - 5) < 1e-12);
	for (double productive : {0.0, -1.0, std::numeric_limits<double>::infinity()}) {
		const auto empty = score(10, 20, productive);
		assert(rate(empty) == 0 && hint(empty) == Hint::Uncertain);
	}
	for (double invalid : {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
		assert(rate(score(invalid)) == 0);
		assert(rate(score(10, invalid)) == 0);
	}
	auto observed = score(1);
	observed.frozenEconomicKey.reset();
	observed.observedCoinGoldPerMinute = 60;
	observed.freezeEconomicKey();
	assert(rate(observed) == 60 && hint(observed) == Hint::LikelySurplus);

	// Unknown required demand or quotes cannot masquerade as free supplies.
	auto unknown = routeFixture(751);
	unknown.cashPressure = true;
	unknown.coinGoldPerMinute = 100;
	unknown.combatFraction = 1;
	unknown.supplyProfile.kinds = {{PlayerBotSupplyKind::ManaPotion, 7620, 20, 1, {}, 0, 50}};
	assert(hint(unknown) == Hint::Uncertain && rate(unknown) == 100);
	unknown.supplyProfile.kinds[0].demand = {0, 1}; // Learned zero is known.
	assert(hint(unknown) == Hint::LikelySurplus);
	unknown.supplyProfile.kinds[0].demand = {0.01, 1};
	unknown.supplyProfile.kinds[0].unitPrice = 0;
	assert(hint(unknown) == Hint::Uncertain);
	unknown.supplyProfile.kinds = {{PlayerBotSupplyKind::ThrowingWeapon, 2389, 3, 1, {}, 0.01, 9}};
	assert(hint(unknown) == Hint::LikelySurplus); // Existing static break demand.
	unknown.supplyProfile.kinds[0].unitPrice = 0;
	assert(hint(unknown) == Hint::Uncertain);
	unknown.supplyProfile.kinds.clear();
	unknown.supplyBudget.expectedPotions = 1;
	assert(hint(unknown) == Hint::Uncertain); // Health quote is also required.
	unknown.healthPotionUnitPrice = 45;
	assert(hint(unknown) == Hint::LikelySurplus);
	unknown.supplyBudget.expectedPotions = std::numeric_limits<double>::max();
	assert(hint(unknown) == Hint::Uncertain); // No health-demand estimate.
	unknown.supplyBudget.expectedPotions = 0;
	unknown.cashPressure = false;
	assert(hint(unknown) == Hint::NotApplicable && rate(unknown) == 0);
	auto ordinary = routeFixture(753, 100), lowerXp = routeFixture(754, 10);
	ordinary.coinGoldPerMinute = 1;
	lowerXp.coinGoldPerMinute = 1000;
	ordinary.freezeEconomicKey();
	lowerXp.freezeEconomicKey();
	ordinary.reconcileRecovery(1, 0); // A funds drop cannot switch XP to income midpass.
	lowerXp.reconcileRecovery(1, 0);
	assert(!ordinary.cashPressure && hint(ordinary) == Hint::NotApplicable);
	assert(playerBotPreferHuntRegion(ordinary, lowerXp));
	assert(!playerBotHuntCandidateCanBeatValidated(lowerXp, ordinary));

	// Live prices, route facts, funds and shortened windows can reject a hunt,
	// but never switch its objective or recompute its economic preference.
	for (bool recovery : {false, true}) {
		auto frozen = routeFixture(752);
		frozen.cashPressure = true;
		frozen.supplyRecovery = recovery;
		frozen.coinGoldPerMinute = 100;
		frozen.currentHealth = frozen.maximumHealth = 100;
		frozen.combatFraction = 1;
		frozen.expectedDamagePerSecond = 0.01;
		frozen.supplyProfile.potionHealing = 125;
		frozen.supplyProfile.kinds = {{PlayerBotSupplyKind::ThrowingWeapon, 2389, 3, 1, {}, 0.01, 9}};
		frozen.freezeEconomicKey();
		auto changed = frozen;
		changed.estimatedReturnSeconds = changed.estimatedSupplySeconds = 6000;
		changed.outboundFare = changed.exitFare = changed.supplyFare = 20;
		changed.healthPotionUnitPrice = 1000;
		changed.supplyProfile.kinds[0].unitPrice = 1000;
		changed.recoveryRouteHealthLoss = 10;
		changed.reconcileTravel(30, 6000, 1);
		changed.reconcileRecovery(1, 10000);
		changed.fitSupplyWindow(1);
		changed.freezeEconomicKey(); // Idempotent even with different live facts.
		sameKey(changed, frozen);
		assert(changed.cashPressure);
		assert(!playerBotHuntTravelAffordable(59, 0, changed.outboundFare, changed.exitFare, changed.supplyFare));

		PlayerBotHuntRouteSelection selector({frozen});
		auto live = routeFacts();
		live.huntDurationSeconds = 30;
		live.travelSeconds = 600;
		live.fare = 1;
		live.approaches = {Position(200, 100, 7)};
		selector.observe(selector.next(), live);
		live.supplyProfile = changed.supplyProfile;
		live.healthPotionUnitPrice = changed.healthPotionUnitPrice;
		live.funds = 10000;
		selector.observe(selector.next(), live);
		if (!recovery) {
			assert(selector.next().stage == PlayerBotHuntRouteStage::DiscoverSupply);
			live.approaches = {Position(300, 100, 7)};
			selector.observe(selector.next(), live);
			assert(selector.next().stage == PlayerBotHuntRouteStage::Supplier);
			selector.observe(selector.next(), live);
		}
		assert(selector.next().stage == PlayerBotHuntRouteStage::Final);
		const auto selected = selector.observe(selector.next(), live);
		assert(selected.terminal && selected.selectedRouteRegion);
		sameKey(*selected.selectedRouteRegion, frozen);
		assert(selected.selectedRouteRegion->availableHuntSeconds == 30);

		using namespace std::chrono;
		const auto start = steady_clock::time_point{} + seconds(1);
		PlayerBotHuntRuntimePlayerObservation player;
		player.health = player.maximumHealth = 100;
		player.funds = 10000;
		PlayerBotHuntRuntime runtime({});
		runtime.selectPlanningRegion(*selected.selectedRouteRegion, player, start);
		runtime.beginCycle(start, 20);
		live.supplyProfile.kinds[0].count = 1;
		assert(runtime.enterHuntArea(player, live.supplyProfile, start + seconds(5)) == recovery);
		if (recovery) {
			assert(runtime.region()->availableHuntSeconds == 20);
			sameKey(*runtime.region(), frozen);
		}
	}

	// All-uncertain queues stop at a fitting productive incumbent. There is no
	// exhaustive least-loss escape hatch, and no detached raw-income cutoff.
	auto incumbent = score(20, 0, 60, 0);
	auto lower = score(10, 0, 60, 0);
	assert(hint(incumbent) == Hint::Uncertain && hint(lower) == Hint::Uncertain);
	lower.coinGoldPerMinute = 10000; // A later diagnostic/live change is not its key.
	lower.optimisticProjectedExperience = 10000;
	assert(!playerBotHuntCandidateCanBeatValidated(lower, incumbent));
	assert(playerBotNextHuntCandidateToValidate({lower}, 0, &incumbent) == 1);
	PlayerBotHuntRouteSelection pruned({incumbent, lower});
	auto live = routeFacts(); live.approaches = {Position(200, 100, 7)};
	pruned.observe(pruned.next(), live);
	pruned.observe(pruned.next(), routeFacts());
	const auto done = pruned.observe(pruned.next(), routeFacts());
	assert(done.terminal && done.selectedRouteRegion);
	sameKey(*done.selectedRouteRegion, incumbent);
	auto unproductive = incumbent;
	unproductive.availableHuntSeconds = 0;
	assert(playerBotHuntCandidateCanBeatValidated(lower, unproductive));
	auto nonfitting = incumbent;
	nonfitting.supplyBudget.fits = false;
	assert(playerBotHuntCandidateCanBeatValidated(lower, nonfitting));

	for (bool recovery : {false, true}) {
		auto tied = incumbent;
		tied.frozenEconomicKey.reset();
		tied.supplyRecovery = recovery;
		tied.freezeEconomicKey();
		auto challenger = tied;
		challenger.score = challenger.optimisticProjectedExperience = 1;
		challenger.threatRatio = 0.1;
		tied.threatRatio = 0.4;
		assert(playerBotPreferHuntRegion(challenger, tied) == recovery);
		assert(playerBotHuntCandidateCanBeatValidated(challenger, tied) == recovery);
		challenger.optimisticProjectedExperience = tied.score;
		assert(playerBotHuntCandidateCanBeatValidated(challenger, tied)); // XP ties stay possible.
	}
	// Across old loss/zero/positive cases and recovery ties, every preferred
	// final result survives pruning. Detailed mutations preserve coarse keys.
	for (bool recovery : {false, true}) {
		std::vector<PlayerBotHuntRegion> outcomes;
		for (double income : {0.0, 2.0, 45.0, 100.0}) for (uint32_t price : {0U, 2U, 45U})
		for (double travel : {0.0, 30.0, 300.0}) for (double productive : {30.0, 60.0, 120.0}) {
			auto result = score(income, travel, productive, price);
			result.frozenEconomicKey.reset();
			result.supplyRecovery = recovery;
			result.score = productive;
			result.optimisticProjectedExperience = 120;
			result.threatRatio = travel / 1000;
			result.freezeEconomicKey();
			outcomes.push_back(result);
		}
		for (const auto& validated : outcomes) for (const auto& result : outcomes) {
			const int order = playerBotCompareHuntEconomicKeys(result.economicKey(), validated.economicKey());
			if (order < 0) assert(!playerBotHuntCandidateCanBeatValidated(result, validated));
			if (!playerBotPreferHuntRegion(result, validated)) continue;
			auto coarse = result;
			coarse.estimatedTravelSeconds = 1000;
			coarse.supplyBudget.fits = false; // Final shortening/route exposure may fit.
			assert(playerBotHuntCandidateCanBeatValidated(coarse, validated));
		}
	}
}

void optimisticCoarseSupplyAdmissionContracts()
{
	auto candidate = routeFixture(760);
	candidate.supplyRecovery = true;
	candidate.currentHealth = candidate.maximumHealth = 100;
	candidate.coinGoldPerMinute = 6;
	candidate.combatFraction = 0.5;
	candidate.expectedDamagePerSecond = 0.01;
	candidate.predictedFightSeconds = 10;
	candidate.supplyProfile.potions = 0;
	candidate.supplyProfile.potionHealing = 125;
	candidate.supplyProfile.kinds = {{PlayerBotSupplyKind::ThrowingWeapon, 2389, 1, 1, {}, 0.01, 9}};
	candidate.reconcileTravel(100, 500, 1);
	auto pessimistic = candidate;
	pessimistic.fitSupplyWindow(1);
	assert(!pessimistic.supplyBudget.fits && pessimistic.availableHuntSeconds == 0);
	assert(!pessimistic.recoverySustainable()); // old coarse duty-share exposure falsely excludes it
	candidate.fitSupplyWindow(1, true);
	assert(candidate.availableHuntSeconds == 100 && candidate.recoverySustainable());
	assert(candidate.supplyKindBudgets[0].expected == 1 && candidate.estimatedTravelSeconds == 500);
	candidate.freezeEconomicKey();
	assert(std::abs(candidate.economicKey().coinGoldPerMinute - 6.0 / 11) < 1e-12); // coarse roundtrip time counts
	auto missing = candidate;
	missing.supplyProfile.kinds[0].count = 0;
	missing.fitSupplyWindow(1, true);
	assert(!missing.recoverySustainable()); // optimism cannot invent essential stock

	const Position depot(200, 100, 7);
	for (double loss : {0.0, 4.0, 25.0}) {
		PlayerBotHuntRouteSelection selector({candidate});
		auto outbound = routeFacts();
		outbound.approaches = {depot};
		outbound.travelSeconds = 500;
		outbound.huntDurationSeconds = 100;
		outbound.dangerCost = static_cast<uint32_t>(loss * 5); // half the total normalized health loss
		assert(selector.observe(selector.next(), outbound).accepted);
		auto live = routeFacts();
		live.travelSeconds = 500;
		live.dangerCost = outbound.dangerCost;
		live.supplyProfile = candidate.supplyProfile;
		live.potionReserve = 1;
		live.funds = 6;
		assert(selector.next().stage == PlayerBotHuntRouteStage::Depot);
		assert(selector.observe(selector.next(), live).accepted);
		assert(selector.next().stage == PlayerBotHuntRouteStage::Final);
		live.recoveryRouteHealthLoss = loss;
		const auto result = selector.observe(selector.next(), live);
		if (loss == 0) {
			assert(result.terminal && result.selectedRouteRegion);
			assert(result.selectedRouteRegion->routeValidated && result.selectedRouteRegion->supplyBudget.fits);
			assert(result.selectedRouteRegion->availableHuntSeconds == 100);
		} else {
			assert(!result.selectedRouteRegion && result.completedCandidate);
			assert(result.completedCandidate->rejectionReason == "recovery_hunt_not_sustainable");
			assert(!result.completedCandidate->supplyBudget.fits);
			// Four health loss exhausts typed stock through route combat demand;
			// 25 also violates the recovery health floor even with zero hunt time.
			if (loss == 4) assert(result.completedCandidate->supplyKindBudgets[0].expected > 1);
		}
	}
	PlayerBotHuntRouteSelection unsafe({candidate});
	auto outbound = routeFacts();
	outbound.riskProfile.healthLossCost = 1000;
	outbound.riskProfile.maximumRouteHealthLoss = 0.05;
	outbound.dangerCost = 51;
	const auto rejected = unsafe.observe(unsafe.next(), outbound);
	assert(rejected.completedCandidate && !rejected.selectedRouteRegion);
	assert(rejected.completedCandidate->rejectionReason == "route_danger_above_tolerance");
}

void typedStockOptimisticXpContracts()
{
	constexpr double duration = 1500;
	auto make = [](double rate = 0.01, double fraction = 1) {
		auto region = routeFixture(780, duration);
		region.experiencePerMinute = 60;
		region.combatFraction = fraction;
		region.supplyProfile.kinds = {{PlayerBotSupplyKind::ThrowingWeapon, 2389, 2, 1, {}, rate, 9}};
		region.reconcileTravel(duration, 0, 1);
		region.fitSupplyWindow(1, true);
		region.freezeEconomicKey();
		return region;
	};
	auto bound = [](const PlayerBotHuntRegion& region) {
		return playerBotHuntOptimisticFittingExperience(region, duration, 1);
	};
	auto one = make();
	assert(one.availableHuntSeconds == 100 && one.score == 100);
	const double oneBound = bound(one);
	assert(oneBound >= 100 && oneBound <= std::nextafter(101.0, std::numeric_limits<double>::infinity()));
	auto more = one;
	more.supplyProfile.kinds[0].count = 3;
	assert(bound(more) > oneBound && bound(more) < 202);
	auto recovery = one;
	recovery.supplyRecovery = true;
	assert(bound(recovery) == bound(more)); // Same two routine units after removing the recovery reserve.
	auto fractional = make(0.01, 0.5);
	assert(bound(fractional) == bound(more));
	assert(bound(make(0.01, 2)) == oneBound); // Same clamped fraction as stock budgeting.
	auto multi = one;
	multi.supplyProfile.kinds.push_back({PlayerBotSupplyKind::ManaPotion, 7620, 2, 1, {0.05, 1}});
	assert(bound(multi) >= 20 && bound(multi) < 22); // Every known kind constrains duration.

	for (double rate : {0.0, -1.0, std::numeric_limits<double>::infinity(),
	    std::numeric_limits<double>::quiet_NaN()}) {
		auto unknown = one;
		unknown.supplyProfile.kinds[0].staticUnitsPerCombatSecond = rate;
		assert(bound(unknown) == duration);
	}
	for (double fraction : {0.0, -1.0, std::numeric_limits<double>::infinity(),
	    std::numeric_limits<double>::quiet_NaN()}) {
		auto unknown = one;
		unknown.combatFraction = fraction;
		assert(bound(unknown) == duration);
	}
	auto learnedZero = one;
	learnedZero.supplyProfile.kinds[0].demand = {0, 1};
	assert(bound(learnedZero) == duration); // Learned zero overrides positive static break demand.
	auto noKinds = one;
	noKinds.supplyProfile.kinds.clear();
	noKinds.supplyProfile.potions = 1;
	noKinds.expectedDamagePerSecond = 100;
	assert(bound(noKinds) == duration); // Health alone cannot justify a tighter bound.
	auto empty = learnedZero;
	empty.supplyProfile.kinds[0].count = 0;
	empty.fitSupplyWindow(1, true);
	assert(!empty.supplyBudget.fits && bound(empty) == duration); // Existing essential-stock gate, not a new XP gate.

	// Configured duration and a valid full-duration XP bound remain ceilings.
	auto shorter = one;
	shorter.optimisticProjectedExperience = 50; // Full XP bound matches this configured duration.
	assert(playerBotHuntOptimisticFittingExperience(shorter, 50, 1) == 50);
	auto stocked = one;
	stocked.supplyProfile.kinds[0].count = 100;
	stocked.optimisticProjectedExperience = 1200;
	assert(bound(stocked) == 1200);
	// The rounded exact boundary stays eligible, even when an incumbent's XP
	// equals the outward-rounded bound. Actual fitting boundary winners survive too.
	one.optimisticFittingExperience = oneBound;
	auto incumbent = routeFixture(781, oneBound);
	assert(playerBotHuntCandidateCanBeatValidated(one, incumbent));
	incumbent.score = std::nextafter(one.score, 0.0);
	assert(playerBotPreferHuntRegion(one, incumbent));
	assert(playerBotHuntCandidateCanBeatValidated(one, incumbent));
	auto noninteger = make(1.0 / 100.25);
	assert(bound(noninteger) >= 102 && bound(noninteger) < 103);
	// Non-unit XP and correction expose different rounding in peak-rate and
	// final-score arithmetic. An XP tie can still win on shorter travel.
	auto rounding = make(1.0 / (123 * 0.1), 0.1);
	rounding.experiencePerMinute = 271.0 / 7;
	rounding.observedCorrection = 0.1;
	rounding.reconcileTravel(duration, 0, 1);
	rounding.availableHuntSeconds = 123;
	rounding.reconcileSupplies(1);
	assert(rounding.supplyBudget.fits);
	rounding.score = rounding.experiencePerMinute * rounding.observedCorrection * 123 / 60;
	rounding.optimisticProjectedExperience = rounding.experiencePerMinute * rounding.observedCorrection * duration / 60;
	rounding.optimisticFittingExperience = playerBotHuntOptimisticFittingExperience(rounding, duration,
	    rounding.experiencePerMinute * rounding.observedCorrection / 60);
	auto tied = rounding;
	tied.estimatedReturnSeconds = 10;
	assert(playerBotPreferHuntRegion(rounding, tied));
	assert(rounding.optimisticFittingExperience >= rounding.score);
	assert(playerBotHuntCandidateCanBeatValidated(rounding, tied));

	// Cheap queue ordering is by fitted score, not by the full optimistic XP
	// bound. A productive incumbent now skips a lower-stock candidate safely.
	incumbent = routeFixture(781, 194);
	assert(playerBotPreferHuntRegion(incumbent, one));
	assert(!playerBotHuntCandidateCanBeatValidated(one, incumbent));
	PlayerBotHuntRouteSelection selector({incumbent, one});
	auto live = routeFacts(); live.approaches = {Position(200, 100, 7)};
	selector.observe(selector.next(), live);
	selector.observe(selector.next(), routeFacts());
	const auto selected = selector.observe(selector.next(), routeFacts());
	assert(selected.terminal && selected.selectedRouteRegion->atlasVariantId == incumbent.atlasVariantId);

	// Detailed routes only spend more stock. Healthy fitting winners remain
	// below the cheap typed bound across learned/static demand and both reserves.
	for (bool recoveryMode : {false, true}) for (bool learned : {false, true})
	for (double rate : {0.005, 0.01, 1.0 / 7}) for (double fraction : {0.2, 0.5, 1.0})
	for (uint32_t count : {2U, 3U, 7U}) for (double loss : {0.0, 1.0, 5.0}) {
		auto coarse = make(rate, fraction);
		coarse.supplyRecovery = recoveryMode;
		coarse.currentHealth = coarse.maximumHealth = 10000;
		coarse.coinGoldPerMinute = 1;
		coarse.expectedDamagePerSecond = 0.25;
		coarse.supplyProfile.potionHealing = 125;
		coarse.supplyProfile.kinds[0].count = count;
		if (learned) {
			coarse.supplyProfile.kinds[0].demand = {rate, 1};
			coarse.supplyProfile.kinds[0].staticUnitsPerCombatSecond = 1; // Learned demand wins.
		}
		coarse.frozenEconomicKey.reset();
		coarse.freezeEconomicKey();
		coarse.optimisticFittingExperience = bound(coarse);
		auto detailed = coarse;
		detailed.routeValidated = true;
		detailed.recoveryRouteHealthLoss = loss;
		detailed.estimatedReturnSeconds = 120;
		detailed.reconcileTravel(duration, 60, 1);
		detailed.fitSupplyWindow(1);
		if (!detailed.productiveWindow() || !detailed.supplyBudget.fits || !detailed.recoverySustainable()) continue;
		assert(detailed.score <= coarse.optimisticFittingExperience);
		auto rival = detailed;
		rival.score = std::nextafter(detailed.score, 0.0);
		assert(playerBotPreferHuntRegion(detailed, rival));
		assert(playerBotHuntCandidateCanBeatValidated(coarse, rival));
	}
}

int main()
{
	typedStockOptimisticXpContracts();
	frozenHuntEconomicRankingContracts();
	optimisticCoarseSupplyAdmissionContracts();
	productiveHuntTripContracts();
	backpackAcquisition();
	paladinLoadouts();
	carriedShieldUpgrade();
	combatStyles();
	toolReplenishmentPlannerGuards();
	loggingContracts();
	huntEconomy();
	patrolOpportunity();
	mixedSustainedYield();
	raisedHuntRecoveryReserve();
	huntCandidateTelemetryCompleteness();
	huntCandidateTelemetryDeltas();
	incrementalHuntValidationPipeline();
	preparationContracts();
	huntSupplyServiceReplayContracts();
	huntSupplyHandoffContracts();
	routeSelectionContracts();
	typedSupplyRouteContracts();
	selectedRouteRetention();
	selectedRouteEvidence();
	selectedRouteLegProofs();
	routeTurnContracts();
	routeDurationRefreshContracts();
	routeRuntimeContracts();
	lootArithmeticMemo();
	modeledPatrolFailure();
	incompletePatrolPreflight();
	mutableShovelPassages();
	navigationFailureAccounting();
	floorChangeLandingOffset();
	transitCombat();
	crowdDamageInflation();
	adaptiveChallenge();
	sharedHuntPerformanceCalibration();
	supplyRecoveryMode();
	typedSupplyStock();
	supplyCalibration();
	supplyCalibrationRounding();
	globalAndLocalSupplyLearning();
	navigationFixedObjective();
	topologyComponentCompression();
	fixtureHuntHorizons();
	remoteHuntTravelGuards();
	throwingWeaponDemand();
	recoverySupplyReserves();
	typedSupplyFallback();
	overBudgetHunts();
	supplyBudget();
	firstHealingBootstrap();
	recoverySpellPriority();
	projection();
	oracleRecovery();
	std::cout << "playerbot contracts passed\n";
}
