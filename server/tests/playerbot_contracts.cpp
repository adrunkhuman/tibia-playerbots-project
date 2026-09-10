// Standalone pure-contract regression: see playerbot_contracts.sh.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>

#include "playerbotgoalplanner.h"
#include "playerbottransitcombat.h"
#include "playerbothuntregions.h"
#include "playerbothuntruntime.h"
#include "playerbotinventorypolicy.h"
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

PlayerBotHuntRuntimeOutcome planRuntimeHunts(const std::vector<PlayerBotHuntRegion>& candidates, bool diversify = false)
{
	PlayerBotHuntRuntime runtime({});
	PlayerBotHuntRuntimePlanningInput input;
	input.cacheRevision = 1;
	input.start.emplace();
	input.start->scan.revision = 1;
	input.start->profile.diversifyIncomeRoutes = diversify;
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

void diversifiedRouteShortlist()
{
	using namespace playerbot;
	std::vector<PlayerBotHuntRegion> candidates(12);
	for (size_t i = 0; i < candidates.size(); ++i) {
		auto& region = candidates[i];
		region.atlasVariantId = i + 1;
		region.suitable = region.reachable = true;
		region.supplyProfile.potions = 10;
		region.score = 100 - i;
	}
	candidates.back().coinGoldPerMinute = 100;
	const uint32_t maximumLegalReserve = 12;
	auto recoveryCost = [&](uint32_t potions, uint32_t reserve) {
		return playerBotRecoverySpendingReserve(potions, recoveryPotionRestockTargetForReserve(reserve), 45, carriedGoldReserve);
	};
	auto diversify = [&](uint32_t potions, uint64_t funds) {
		return playerBotHuntCashPressure(potions, maximumLegalReserve, funds, recoveryCost(potions, maximumLegalReserve));
	};
	assert(diversify(10, 200));
	assert(!diversify(10, 235)); // can fund even the largest legal route reserve
	assert(!diversify(14, 0)); // plentiful supplies do not need income probes
	const auto ordinary = planRuntimeHunts(candidates);
	assert(ordinary.routeCandidates.size() == 8);
	for (size_t i = 0; i < 8; ++i) assert(ordinary.routeCandidates[i].atlasVariantId == i + 1);
	const auto diversified = planRuntimeHunts(candidates, diversify(10, 200));
	assert(diversified.routeCandidates.size() == 8 && diversified.candidates.size() == 12);
	assert(diversified.selectedRegion->atlasVariantId == ordinary.selectedRegion->atlasVariantId);
	for (size_t i = 0; i < 12; ++i) assert(diversified.candidates[i].atlasVariantId == ordinary.candidates[i].atlasVariantId);
	for (size_t i = 0; i < 4; ++i) assert(diversified.routeCandidates[i].atlasVariantId == i + 1);
	auto containsIncome = [](const auto& regions) {
		return std::any_of(regions.begin(), regions.end(), [](const auto& region) { return region.atlasVariantId == 12; });
	};
	assert(containsIncome(diversified.routeCandidates));
	assert(!containsIncome(ordinary.routeCandidates));
	assert(!containsIncome(planRuntimeHunts(candidates, diversify(10, 235)).routeCandidates));
	// Execute the same reconciliation and final comparison used after routing.
	auto routed = diversified.routeCandidates;
	for (auto& region : routed) {
		region.routeValidated = true;
		region.reconcileRecovery(maximumLegalReserve, 200, recoveryCost(10, maximumLegalReserve));
	}
	auto best = [&]() { return std::min_element(routed.begin(), routed.end(), playerBotPreferHuntRegion)->atlasVariantId; };
	assert(best() == 12); // the initially twelfth XP option actually reaches routing
	for (auto& region : routed) if (region.atlasVariantId == 12) region.suitable = false;
	assert(best() == 1); // rejected outbound/return danger cannot be overridden
	for (auto& region : routed) {
		region.suitable = true;
		region.reconcileRecovery(maximumLegalReserve, 235, recoveryCost(10, maximumLegalReserve));
	}
	assert(best() == 1); // exact affordability retains XP even after diversification
	for (auto& region : routed) region.reconcileRecovery(1, 200, recoveryCost(10, 1));
	assert(best() == 1); // an inexpensive actual route likewise retains XP

	candidates.back().supplyBudget.fits = false;
	assert(!containsIncome(planRuntimeHunts(candidates, true).routeCandidates));
	for (auto& region : candidates) {
		region.supplyBudget.fits = false;
		region.supplyBudget.expectedPotions = 1;
	}
	candidates.back().supplyBudget.expectedPotions = 2;
	assert(!containsIncome(planRuntimeHunts(candidates, true).routeCandidates));
	candidates.back().supplyBudget.expectedPotions = 1;
	candidates[0].supplyBudget.fits = candidates[1].supplyBudget.fits = true;
	const auto mixedTiers = planRuntimeHunts(candidates, true).routeCandidates;
	assert(mixedTiers.size() == 8 && containsIncome(mixedTiers));
	assert(mixedTiers[0].atlasVariantId == 1 && mixedTiers[1].atlasVariantId == 2);
	candidates.back().predictedLethal = true;
	assert(!containsIncome(planRuntimeHunts(candidates, true).routeCandidates));
	candidates.back().predictedLethal = false;
	candidates.back().reachable = false;
	assert(!containsIncome(planRuntimeHunts(candidates, true).routeCandidates));
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
	huntEconomy();
	patrolOpportunity();
	mixedSustainedYield();
	raisedHuntRecoveryReserve();
	diversifiedRouteShortlist();
	lootArithmeticMemo();
	modeledPatrolFailure();
	transitCombat();
	crowdDamageInflation();
	overBudgetHunts();
	supplyBudget();
	recoverySpellPriority();
	projection();
	oracleRecovery();
	std::cout << "playerbot contracts passed\n";
}
