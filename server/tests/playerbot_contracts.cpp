// Standalone pure-contract regression: see playerbot_contracts.sh.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>

#include "playerbotdepotworkflow.h"
#include "playerboteconomy.h"
#include "playerbotgoalplanner.h"
#include "playerbotnavigationruntime.h"
#include "playerbottransitcombat.h"
#include "playerbottopology.h"
#include "playerbothuntregions.h"
#include "playerbothuntruntime.h"
#include "playerbotinventorypolicy.h"
#include "playerbotsupplyrecovery.h"
#include "playerbottestpolicy.h"
#include "playerbotturnrouter.h"

namespace {
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

PlayerBotHuntRuntimeOutcome planRuntimeHunts(const std::vector<PlayerBotHuntRegion>& candidates)
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
	assert(runtime.completeScoreWork(observations, 0).command == PlayerBotHuntRuntimeCommand::PlanningScored);
	const auto selection = runtime.advancePlanning(input, now);
	assert(selection.command == PlayerBotHuntRuntimeCommand::RegionSelected && selection.selectedRegion);
	return selection;
}

PlayerBotHuntRegion selectRuntimeHunt(const std::vector<PlayerBotHuntRegion>& candidates)
{
	return *planRuntimeHunts(candidates).selectedRegion;
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
	// Candidate outbound + return route facts raise the target beyond fixed ten.
	const uint32_t candidateReserve = 2 + 10;
	assert(recoveryPotionRestockTargetForReserve(candidateReserve) == 13);
	assert(recoveryPotionRestockTargetForReserve(UINT32_MAX) == UINT32_MAX);
	reconcile(income, candidateReserve, funds);
	reconcile(xp, candidateReserve, funds);
	assert(income.cashPressure && !income.supplyBudget.fits);
	assert(selectRuntimeHunt({income, xp}).atlasVariantId == 1);
	income.predictedLethal = true;
	assert(selectRuntimeHunt({income, xp}).atlasVariantId == 2);
	income.predictedLethal = false;
	const uint64_t exactFunds = carriedGoldReserve + 3 * price;
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
	const auto planned = planRuntimeHunts(candidates);
	assert(planned.routeCandidates.size() == candidates.size());
	for (size_t index = 0; index < candidates.size(); ++index) {
		assert(planned.routeCandidates[index].atlasVariantId == index + 1);
	}
	// There is no fixed-eight, local/remote, observed/unobserved, or income slot.
	assert(std::count_if(planned.routeCandidates.begin(), planned.routeCandidates.end(),
	    [](const auto& region) { return region.topologyReachable; }) == 5);

	// Nine route failures still leave the tenth candidate available to win.
	size_t validated = 0;
	std::optional<PlayerBotHuntRegion> winner;
	for (const PlayerBotHuntRegion& candidate : planned.routeCandidates) {
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
}

void navigationFailureAccounting()
{
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
	PlayerBotTransitCombat episode;
	const auto start = std::chrono::steady_clock::time_point{};
	assert(episode.observe(true, 1, Phase::ReturnToDepot));
	episode.beginDefense(42, start);
	for (int turn = 0; turn < 100; ++turn) {
		assert(!episode.observe(true, 1, Phase::ReturnToDepot));
		assert(!episode.allowsDefense(42, true));
		assert(!episode.allowsDefense(43, true)); // the budget belongs to the episode, not the monster ID
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
	assert(!retreat.allowsDefense(43, true));

	// Route exhaustion can spend a separate bounded escape window. It permits
	// route-critical targets regardless of the planner's nominal success (a
	// corridor plan can "succeed" straight through a blocked tile), exits on
	// real progress, and never renews on replans.
	assert(!retreat.beginBreakout(false, true, true, now + std::chrono::seconds(75)));
	assert(!retreat.beginBreakout(true, false, true, now + std::chrono::seconds(75)));
	assert(retreat.beginBreakout(true, true, false, now + std::chrono::seconds(75)));
	assert(retreat.breakoutActive() && retreat.allowsDefense(43, true));
	assert(!retreat.beginBreakout(true, true, true, now + std::chrono::seconds(76)));
	assert(!retreat.breakoutExpired(now + std::chrono::seconds(104)));
	assert(retreat.breakoutExpired(now + std::chrono::seconds(105)));
	assert(retreat.observeBreakoutNavigation(true, false));
	assert(!retreat.breakoutActive() && !retreat.allowsDefense(43, true));
	assert(!retreat.beginBreakout(true, true, true, now + std::chrono::seconds(110)));
	assert(retreat.observe(true, 2, Phase::ReturnToDepot));
	assert(retreat.beginBreakout(true, true, true, now + std::chrono::seconds(110)));
	assert(retreat.observeBreakoutNavigation(false, true));
	assert(!retreat.breakoutActive());
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
	performance.emplace(5, PlayerBotHuntRegionPerformance{10, 9, 1, atlasRevision, true});
	performance.emplace(6, PlayerBotHuntRegionPerformance{10, 1.25, 1, atlasRevision, false});
	performance.emplace(8, PlayerBotHuntRegionPerformance{10, 1.75, 1, atlasRevision - 1, true});
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
	assert(!partial.insufficientFunds && partial.amount == 1); // 146 - 100 reserve = 46 gp.

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

	// Free routes need no cash reserve; paid routes still preserve recovery funds.
	assert(playerBotHuntTravelAffordable(0, 100, 0, 0, 0));
	assert(playerBotHuntTravelAffordable(50, 100, 0, 0, 0));
	assert(!playerBotHuntTravelAffordable(50, 100, 1, 0, 0));
	assert(playerBotHuntTravelAffordable(500, 100, 100, 200, 100));
	assert(!playerBotHuntTravelAffordable(499, 100, 100, 200, 100));
	assert(!playerBotHuntTravelAffordable(500, 100, 250, 200));
	assert(playerBotHuntTravelAffordable(550, 100, 250, 200));
	assert(playerBotHuntTravelPaymentAffordable(0, 100, 0, 200));
	assert(!playerBotHuntTravelPaymentAffordable(500, 100, 250, 200));
	assert(playerBotHuntTravelPaymentAffordable(550, 100, 250, 200));
	// Exit validation reserves only the later supplier fare; counting the exit
	// fare again as future money would reject this exactly funded route.
	assert(playerBotHuntTravelPaymentAffordable(400, 100, 200, 100));
	assert(!playerBotHuntTravelPaymentAffordable(400, 100, 200, 300));
	const uint64_t recoveryBeforeRestock = playerBotRecoverySpendingReserve(2, 10, 45, 100);
	const uint64_t recoveryAfterRestock = playerBotRecoverySpendingReserve(10, 10, 45, 100);
	assert(recoveryBeforeRestock == 460 && recoveryAfterRestock == 100);
	assert(!playerBotHuntTravelPaymentAffordable(200, recoveryBeforeRestock, 50, 0));
	assert(playerBotHuntTravelPaymentAffordable(200, recoveryAfterRestock, 50, 0));
	assert(playerBotDepotRouteSafetyAccepted(true, false, false, true));
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
	huntEconomy();
	patrolOpportunity();
	mixedSustainedYield();
	raisedHuntRecoveryReserve();
	incrementalHuntValidationPipeline();
	lootArithmeticMemo();
	modeledPatrolFailure();
	navigationFailureAccounting();
	transitCombat();
	crowdDamageInflation();
	adaptiveChallenge();
	sharedHuntPerformanceCalibration();
	supplyRecoveryMode();
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
