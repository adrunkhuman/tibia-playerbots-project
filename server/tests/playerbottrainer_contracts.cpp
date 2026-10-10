#include <iostream>

#include "playerbottrainerroute.h"
#include "playerbotplanningbudget.h"
#include "playerbotpathsearch.h"
#include "playerbotnavigationruntime.h"
#include "playerbotprogressionplanners.h"
#include "playerbotprogressionruntime.h"
#include "playerbottransportsearch.h"
#include "playerbothunttravelevidence.h"
#include "playerbotturnrouter.h"
#include "const.h"

#include <cassert>
#include <memory>

namespace {
void paidQuoteBoundaries()
{
	PlayerBotSpellTrainingPlanner planner;
	PlayerBotSpellTrainingPlannerSnapshot snapshot;
	snapshot.reserveAvailable = true;
	snapshot.maximumRouteDangerCost = 100;
	snapshot.maximumRouteDanger = 10;
	PlayerBotSpellOfferSnapshot offer;
	offer.npcId = 12;
	offer.spellName = "Light Healing";
	offer.price = 170;
	offer.fare = 40;
	offer.reserve = 45;
	offer.inScope = offer.registryMatches = offer.implementedUse = offer.vocationEligible = true;
	offer.levelEligible = offer.premiumEligible = offer.suppliesReady = offer.route.reachable = true;
	snapshot.offers = {offer};
	snapshot.totalMoney = 254;
	assert(!planner.select(snapshot).selected);
	assert(planner.select(snapshot).rejections.front().reason == "trip_fare_unaffordable");
	snapshot.totalMoney = 255;
	const auto selected = planner.select(snapshot).selected;
	assert(selected && selected->fare == 40 && selected->reserve == 45);
	// Releasing the first-heal cash buffer does not release missing supply money.
	snapshot.offers.front().reserve = 0;
	snapshot.totalMoney = 210;
	assert(planner.select(snapshot).selected);
	snapshot.totalMoney = 209;
	assert(!planner.select(snapshot).selected);
	snapshot.totalMoney = 177; // Spell-only capital is not a boat ticket.
	assert(!planner.select(snapshot).selected);
	snapshot.totalMoney = 113;
	assert(planner.select(snapshot).rejections.front().reason == "unaffordable_after_reserves");
	snapshot.totalMoney = 1000;
	snapshot.offers.front().routeRejection = "route_proof_exhausted";
	assert(planner.select(snapshot).rejections.front().reason == "route_proof_exhausted");
	snapshot.offers.front().routeRejection = "route_fare_limited";
	assert(planner.select(snapshot).rejections.front().reason == "route_fare_limited");
	snapshot.offers.front().routeRejection.clear();
	snapshot.offers.front().route.dangerCost = 101;
	assert(planner.select(snapshot).rejections.front().reason == "route_danger_above_tolerance");
	snapshot.offers.front().route.dangerCost = 0;
	snapshot.offers.front().suppliesReady = false;
	assert(planner.select(snapshot).rejections.front().reason == "supply_reserve_unmet");
	assert(!playerBotTrainerTripAffordable(UINT64_MAX, UINT64_MAX, 0, 0));
	assert(!playerBotTrainerTripAffordable(UINT64_MAX, UINT64_MAX - 1, 2, 0));
	assert(!playerBotTrainerTripAffordable(UINT64_MAX, 0, UINT64_MAX, 1));
	assert(playerBotTrainerTripAffordable(UINT64_MAX, 0, UINT64_MAX - 40, 40));
	assert(playerBotTrainerFareAffordable(285, 45, 170, 70, 40));
	assert(!playerBotTrainerFareAffordable(284, 45, 170, 70, 40));
	assert(!playerBotTrainerFareAffordable(1000, 45, 170, 40, 41)); // Extra income cannot extend the quote.
	assert(playerBotTrainerFareAffordable(245, 45, 170, 30, 30)); // Remaining itinerary after paying 40.
	assert(!playerBotTrainerFareAffordable(244, 45, 170, 30, 30));
}

void scanAccountingAndRanking()
{
	using namespace std::chrono_literals;
	using Progress = PlayerBotTrainerScanProgress;
	const auto start = Progress::Clock::time_point{};
	Progress progress;
	progress.deadline = start + 10min;
	// Replay the observed first proof and a second request across the old
	// whole-scan 4096 boundary. Queued admission waits are not failed proofs.
	progress.continuations = 3442;
	uint32_t secondRequestSlices = 0;
	for (unsigned continuation = 0; continuation < 4096; ++continuation) {
		++progress.continuations;
		if (continuation % 2 == 0) ++secondRequestSlices;
		assert(!progress.exhaustion(start + 33s, 2));
	}
	assert(progress.continuations > 4096 && secondRequestSlices < 4096);
	assert(std::string(progress.exhaustion(start + 10min, 2)) == "scan_elapsed_limit");
	progress.restarts = 2;
	assert(!progress.exhaustion(start + 1min, 2));
	progress.restarts = 3;
	assert(std::string(progress.exhaustion(start + 1min, 2)) == "scan_restart_limit");

	PlayerBotSpellTrainingPlanner planner;
	PlayerBotSpellTrainingPlannerSnapshot snapshot;
	snapshot.reserveAvailable = true;
	snapshot.totalMoney = 1000;
	snapshot.maximumRouteDangerCost = 100;
	snapshot.maximumRouteDanger = 10;
	PlayerBotSpellOfferSnapshot support;
	support.npcId = 1; support.spellName = "support";
	support.learningPriority = 2; support.level = 14; support.price = 600; support.reserve = 100;
	support.inScope = support.registryMatches = support.implementedUse = support.vocationEligible = true;
	support.levelEligible = support.premiumEligible = support.suppliesReady = true;
	auto heal = support;
	heal.npcId = 2; heal.spellName = "heal";
	heal.learningPriority = 0; heal.level = 9; heal.price = 170; heal.reserve = 0;
	auto alternate = heal;
	alternate.npcId = 3;
	snapshot.offers = {support, heal, alternate}; // Loaded provider/script order is not ranking.
	std::stable_sort(snapshot.offers.begin(), snapshot.offers.end(), [](const auto& a, const auto& b) {
		return playerBotTrainerPriority(a) < playerBotTrainerPriority(b);
	});
	assert(snapshot.offers[0].npcId == 2 && snapshot.offers[1].npcId == 3);
	assert(playerBotTrainerPriority(snapshot.offers[0]) == playerBotTrainerPriority(snapshot.offers[1]));
	assert(playerBotTrainerPriority(snapshot.offers[0]) < playerBotTrainerPriority(snapshot.offers[2]));
	// Equal static rank still compares travel steps, including paid routes.
	snapshot.offers[0].route.reachable = snapshot.offers[1].route.reachable = true;
	snapshot.offers[0].route.steps = 200;
	snapshot.offers[0].fare = 40;
	snapshot.offers[1].route.steps = 100;
	snapshot.offers[1].fare = 70;
	assert(planner.select(snapshot).selected->npcId == 3);
	snapshot.offers[2].routeRejection = "lower_learning_priority";
	assert(!playerBotTrainerDeferredNeeded(snapshot.offers, planner.select(snapshot).selectedOfferIndex));
	assert(playerBotTrainerDeferredNeeded(snapshot.offers, std::nullopt) == 2);
	snapshot.offers[2].routeRejection.clear();
	// Deadline ends only pending proofs. Even a retained route beyond a
	// reopened stale cursor must remain selectable, not silently discarded.
	playerBotTrainerEndPendingProofs(snapshot.offers, 0);
	assert(snapshot.offers[0].routeRejection.empty() && snapshot.offers[1].routeRejection.empty());
	assert(snapshot.offers[2].routeRejection == "route_proof_exhausted");
	assert(planner.select(snapshot).selected->npcId == 3);
	snapshot.offers[1].registryMatches = false;
	assert(planner.select(snapshot).selected->npcId == 2); // Loaded-state validation still authoritative.
	snapshot.offers[0].suppliesReady = false;
	assert(!planner.select(snapshot).selected); // A retained route cannot erase its route potion reserve.
	snapshot.offers[2].routeRejection = "lower_learning_priority";
	assert(playerBotTrainerDeferredNeeded(snapshot.offers, planner.select(snapshot).selectedOfferIndex) == 2);
	snapshot.offers[0].suppliesReady = true;
	snapshot.offers[2].routeRejection = "route_proof_exhausted";
	snapshot.totalMoney = 209;
	assert(!planner.select(snapshot).selected); // Deadline cannot release spell/fare protection.
}

void localTrainerProofAndRankBound()
{
	const Position source(100, 100, 7), representative(99, 99, 7), remote(200, 200, 7);
	const std::vector<Position> original{representative, source};
	const std::vector<Position> wandered{source, Position(103, 101, 7)};
	assert(original != wandered);
	PlayerBotNavigationRoutePlan local;
	local.metrics.result = PlayerBotNavigationResult::Reached;
	local.metrics.waypoint = source; // Zero-step proof endpoint is not the any-of representative.
	assert(playerBotTrainerDiscoveryDestinationRetained(local, original, wandered));
	assert(!playerBotTrainerDiscoveryDestinationRetained(local, original, {remote}));
	PlayerBotHuntTravelEvidence evidence;
	evidence.source = source; evidence.destination = representative;
	evidence.topologyGeneration = 1; evidence.riskRevision = 2;
	std::get<18>(evidence.actor) = source;
	assert(evidence.validContext(source, evidence.destination, evidence.actor, 1, 2, evidence.risk));
	assert(!evidence.validContext(source, wandered.front(), evidence.actor, 1, 2, evidence.risk));
	assert(playerBotTrainerMinimumSteps(source, wandered) == 0);
	assert(playerBotTrainerMinimumSteps(source, {remote}) == 1);
	assert(playerBotTrainerMinimumSteps(source, {}) == 1);

	PlayerBotSpellTrainingPlanner planner;
	PlayerBotSpellTrainingPlannerSnapshot snapshot;
	snapshot.reserveAvailable = true; snapshot.totalMoney = 1000;
	snapshot.maximumRouteDangerCost = 100; snapshot.maximumRouteDanger = 10;
	PlayerBotSpellOfferSnapshot incumbent;
	incumbent.npcId = 20; incumbent.spellName = "M";
	incumbent.learningPriority = 0; incumbent.level = 9; incumbent.price = 170; incumbent.reserve = 0;
	incumbent.inScope = incumbent.registryMatches = incumbent.implementedUse = incumbent.vocationEligible = true;
	incumbent.levelEligible = incumbent.premiumEligible = incumbent.suppliesReady = incumbent.route.reachable = true;
	incumbent.route.approachPosition = source; incumbent.route.steps = 0;
	auto candidate = incumbent;
	candidate.npcId = 19; // Better suffix cannot beat zero steps from a remote source.
	candidate.route.reachable = false;
	assert(!playerBotTrainerOptimisticCanBeat(candidate, 1, incumbent));
	assert(playerBotTrainerOptimisticCanBeat(candidate, 0, incumbent)); // Co-located equal-rank lower ID can win.
	candidate.routeRejection = "route_rank_dominated";
	snapshot.offers = {incumbent, candidate};
	const auto selected = planner.select(snapshot).selectedOfferIndex;
	assert(selected == 0);
	assert(!playerBotTrainerDeferredNeeded(snapshot.offers, selected, [](size_t) { return uint32_t(1); }));
	assert(playerBotTrainerDeferredNeeded(snapshot.offers, selected, [](size_t) { return uint32_t(0); }) == 1);
	assert(playerBotTrainerDeferredNeeded(snapshot.offers, std::nullopt, [](size_t) { return uint32_t(1); }) == 1);

	// Compare every pruning verdict to the actual original six-field planner
	// rank, including spell/provider suffixes, zero-step ties and boat steps.
	// If the optimistic route cannot win, no longer completed route may win.
	for (uint8_t priority : {0, 1}) for (uint32_t level : {9, 10}) for (uint32_t price : {170, 171})
	for (const char* name : {"A", "M", "Z"}) for (uint32_t id : {19, 20, 21}) for (uint32_t minimum : {0, 1}) {
		candidate = incumbent;
		candidate.learningPriority = priority; candidate.level = level; candidate.price = price;
		candidate.spellName = name; candidate.npcId = id;
		if (playerBotTrainerOptimisticCanBeat(candidate, minimum, incumbent)) continue;
		for (uint32_t actual = minimum; actual <= 3; ++actual) {
			candidate.route.steps = actual;
			candidate.fare = actual == 0 ? 0 : 40;
			snapshot.offers = {incumbent, candidate};
			assert(planner.select(snapshot).selectedOfferIndex == 0);
		}
	}
	// A better static rank still gets proof, regardless of distance. Equal
	// positive-step ranks still get proof when their suffix could win.
	candidate = incumbent; candidate.level = 8;
	assert(playerBotTrainerOptimisticCanBeat(candidate, 1, incumbent));
	candidate = incumbent; candidate.npcId = 19;
	incumbent.route.steps = 1;
	assert(playerBotTrainerOptimisticCanBeat(candidate, 1, incumbent));
}

void discardedRoutePotionReserve()
{
	PlayerBotSpellTrainingPlanner planner;
	PlayerBotSpellTrainingPlannerSnapshot snapshot;
	snapshot.reserveAvailable = true;
	snapshot.totalMoney = 250;
	snapshot.maximumRouteDangerCost = 1000;
	snapshot.maximumRouteDanger = 10;
	PlayerBotSpellOfferSnapshot offer;
	offer.npcId = 12; offer.spellName = "heal"; offer.price = 170; offer.fare = 40; offer.reserve = 0;
	offer.inScope = offer.registryMatches = offer.implementedUse = offer.vocationEligible = true;
	offer.levelEligible = offer.premiumEligible = true;
	const uint32_t potions = 3;
	auto refreshStock = [&](auto& candidate) { candidate.suppliesReady = potions > candidate.potionReserve; };

	// The completed route is legal but needs more stock than is carried.
	// While retained it must continue to block selection; a refresh cannot
	// silently replace its four-potion requirement with the ordinary floor.
	offer.route.reachable = true;
	offer.route.dangerCost = 500;
	offer.potionReserve = 4;
	refreshStock(offer);
	snapshot.offers = {offer};
	assert(planner.select(snapshot).rejections.front().reason == "supply_reserve_unmet");
	refreshStock(snapshot.offers.front());
	assert(!planner.select(snapshot).selected);

	// Both stale-world/provider invalidation and interruption use this exact
	// production reset. The old requirement must not fail the cheap admission
	// check before a safer replacement can be proved by the sliced engine.
	auto& candidate = snapshot.offers.front();
	playerBotTrainerDiscardRouteProof(candidate);
	assert(!candidate.route.reachable && candidate.potionReserve == 0);
	refreshStock(candidate);
	assert(candidate.suppliesReady); // Admits reproof, not selection.
	assert(!planner.select(snapshot).selected); // No proof is still no route.
	candidate.route.reachable = true;
	candidate.route.dangerCost = 0;
	candidate.potionReserve = 1;
	refreshStock(candidate);
	const auto selected = planner.select(snapshot).selected;
	assert(selected && selected->potionReserve == 1 && selected->fare == 40);
	assert(selected->price == 170 && selected->reserve == 0);
}

void pendingOwnership()
{
	using Search = PlayerBotTransportSearch;
	using Segment = PlayerBotTransportSegment;
	using Result = PlayerBotNavigationResult;
	auto trainer = std::make_shared<Search>(2, std::vector<Search::Offer>{{0, 1, 40}}, 40);
	auto hunt = std::make_shared<Search>(1, std::vector<Search::Offer>{}, 0);
	auto service = std::make_shared<Search>(1, std::vector<Search::Offer>{}, 0);
	const auto huntIdentity = hunt.get(), serviceIdentity = service.get(), trainerIdentity = trainer.get();
	bool allowConnection = false;
	auto query = [&](size_t state, size_t connection, bool) -> std::optional<Segment> {
		if (!allowConnection) return std::nullopt;
		if (connection == SIZE_MAX) return Segment{state == 1 ? Result::Reached : Result::Unreachable, 2, 2};
		return Segment{state == 0 ? Result::Reached : Result::Unreachable, 1, 1};
	};
	{
		PlayerBotRouteWorkSlot slot(hunt, trainer);
		assert(hunt.get() == trainerIdentity);
		for (unsigned turn = 0; turn < 4; ++turn) assert(!hunt->advance(query));
	}
	assert(hunt.get() == huntIdentity && trainer.get() == trainerIdentity && service.get() == serviceIdentity);
	assert(!trainer->bestPaid); // In-flight work is not unreachable evidence.
	allowConnection = true;
	unsigned turns = 0;
	{
		PlayerBotRouteWorkSlot slot(hunt, trainer);
		while (!hunt->advance(query)) assert(++turns < 32);
		assert(hunt->bestPaid && hunt->bestPaid->fare == 40);
	}
	assert(hunt.get() == huntIdentity && service.get() == serviceIdentity);
	trainer.reset(); // Cancellation owns no callback and cannot reset another owner's frontier.
	assert(hunt.get() == huntIdentity && service.get() == serviceIdentity);

	PlayerBotTurnRouter router;
	PlayerBotTurnObservation observation;
	observation.trainerDiscoveryActive = observation.huntPlanningActive = observation.preparationActive = true;
	for (auto phase : {PlayerBotCyclePhase::Service, PlayerBotCyclePhase::Hunt, PlayerBotCyclePhase::ReturnToDepot}) {
		router.setCyclePhase(phase);
		assert(router.route(observation) == PlayerBotTurnCommand::TrainerDiscovery);
	}
	router.pause();
	assert(router.route(observation) == PlayerBotTurnCommand::None);
	router.start();
	router.stop();
	assert(router.route(observation) == PlayerBotTurnCommand::None);
}

void invalidation()
{
	PlayerBotHuntTravelEvidence evidence;
	evidence.paid = true;
	evidence.source = Position(100, 100, 7);
	evidence.destination = Position(300, 300, 7);
	evidence.topologyGeneration = 1;
	evidence.riskRevision = 2;
	evidence.actor = {};
	evidence.offers = {{9, Position(101, 100, 7), Position(200, 200, 7), 40, 0, false, {"mainland", "yes"}}};
	evidence.providers.reset({{9, Position(101, 100, 7)}});
	evidence.providers.use(9);
	assert(evidence.validContext(evidence.source, evidence.destination, evidence.actor, 1, 2, evidence.risk));
	auto actor = evidence.actor;
	std::get<7>(actor) = 211;
	assert(!evidence.validContext(evidence.source, evidence.destination, actor, 1, 2, evidence.risk));
	assert(!evidence.validContext(evidence.source, evidence.destination, evidence.actor, 3, 2, evidence.risk));
	assert(evidence.validOffers(evidence.offers));
	auto offers = evidence.offers;
	std::get<3>(offers.front()) = 41;
	assert(!evidence.validOffers(offers));
	offers = evidence.offers;
	std::get<1>(offers.front()).x += 3;
	assert(!evidence.validOffers(offers));
	{ PlayerBotRouteChanges::Scope watch(evidence.watch); PlayerBotRouteChanges::read(Position(102, 100, 7)); }
	PlayerBotRouteChanges::changed(Position(102, 100, 7));
	assert(!evidence.validContext(evidence.source, evidence.destination, evidence.actor, 1, 2, evidence.risk));
}

template<size_t Field = 0> void executionMaterialActorFacts(PlayerBotHuntTravelEvidence& evidence)
{
	if constexpr (Field < std::tuple_size<PlayerBotHuntTravelEvidence::Actor>::value) {
		if constexpr (Field != 12 && Field != 13 && Field != 18) {
			auto actor = evidence.actor;
			auto& value = std::get<Field>(actor);
			if constexpr (std::is_same_v<std::decay_t<decltype(value)>, bool>) value = !value;
			else ++value;
			assert(std::string(evidence.executionContextChange(actor, 546, 1, 2, evidence.risk)) == "actor_facts_changed");
		}
		executionMaterialActorFacts<Field + 1>(evidence);
	}
}

void executionLegValidity()
{
	using Evidence = PlayerBotHuntTravelEvidence;
	const Position source(100, 100, 7), approach(102, 100, 7), landing(200, 200, 7);
	const Position trainerTile(202, 200, 7), alternativeTile(500, 500, 7);
	Evidence evidence;
	evidence.source = source; evidence.destination = trainerTile;
	evidence.topologyGeneration = 1; evidence.riskRevision = 2;
	evidence.actor = {14, 245, 13, 20, 25, 40, 1.0f, 250, true, false, true, true,
	                  200, 600, false, 0, 0, 0, source, 76, true};
	evidence.paid = true;
	evidence.offers = {{9, approach, landing, 70, 0, false, {"destination", "yes"}},
	                   {10, alternativeTile, Position(600, 600, 7), 40, 0, false, {"elsewhere", "yes"}}};
	evidence.providers.reset({{9, approach}, {10, alternativeTile}});
	evidence.providers.use(9);
	evidence.providers.use(10); // Visited alternative, not chosen itinerary.
	{
		PlayerBotRouteChanges::Scope reads(evidence.watch);
		PlayerBotRouteChanges::read(approach);
		PlayerBotRouteChanges::read(alternativeTile);
	}
	PlayerBotNavigationRoutePlan route;
	PlayerBotNavigationStep walk;
	walk.action = PlayerBotNavigationAction::Move;
	walk.target = walk.expectedPosition = approach;
	PlayerBotNavigationStep boat;
	boat.action = PlayerBotNavigationAction::NpcTravel;
	boat.npcId = 9; boat.target = approach; boat.expectedPosition = landing; boat.price = 70;
	boat.dialogue = {"destination", "yes"};
	route.steps = {walk, boat};
	route.metrics.firstNpcTravelOffer = PlayerBotNpcTravelOfferIdentity{9, landing, 70, 0, false, boat.dialogue};
	evidence.retainExecutionLeg(route, 546); // Exact production first-leg dependency capture.
	assert(!evidence.executionContextChange(evidence.actor, 546, 1, 2, evidence.risk));

	// Replay the loaded-map durations at the five observed invalidations.
	// The old planning context fails even after its position-only patch;
	// execution accepts ordinary walking while preserving actual actor speed.
	for (uint32_t duration : {300, 200, 250, 200, 250}) {
		auto actor = evidence.actor;
		std::get<12>(actor) = duration;
		std::get<13>(actor) = duration * 3;
		std::get<18>(actor) = approach;
		assert(!evidence.executionContextChange(actor, 546, 1, 2, evidence.risk));
		if (duration != 200) {
			std::get<18>(actor) = source;
			assert(!evidence.validContext(source, trainerTile, actor, 1, 2, evidence.risk));
		}
	}
	assert(std::string(evidence.executionContextChange(evidence.actor, 545, 1, 2, evidence.risk)) == "actor_speed_changed");
	executionMaterialActorFacts(evidence); // Gear/combat, funds, eligibility, tools, permissions remain material.
	assert(std::string(evidence.executionContextChange(evidence.actor, 546, 3, 2, evidence.risk)) == "topology_changed");
	assert(std::string(evidence.executionContextChange(evidence.actor, 546, 1, 3, evidence.risk)) == "risk_changed");
	auto risk = evidence.risk;
	risk.maximumHealthLossPerSecond += 0.01;
	assert(std::string(evidence.executionContextChange(evidence.actor, 546, 1, 2, risk)) == "risk_changed");

	// Broad planning proof is correctly stale when an explored captain moves.
	// This is not a change to the accepted first leg or its exact fare quote.
	auto movedOffers = evidence.offers;
	std::get<1>(movedOffers[1]).x += 3;
	assert(!evidence.validOffers(movedOffers));
	PlayerBotRouteChanges::changed(alternativeTile);
	assert(!evidence.watch.valid());
	assert(!evidence.executionContextChange(evidence.actor, 546, 1, 2, evidence.risk));
	const auto boarding = playerBotTrainerBoardingApproach(route, source);
	assert(boarding && *boarding == approach);
	assert(Position::areInRange<3, 3, 0>(*boarding, Position(103, 100, 7)));
	assert(!Position::areInRange<3, 3, 0>(*boarding, Position(106, 100, 7)));
	assert(!Position::areInRange<3, 3, 0>(*boarding, Position(102, 100, 6)));
	assert(playerBotTrainerDestinationRetained(route, {trainerTile, Position(203, 200, 7)},
	    {trainerTile, Position(201, 200, 7)}));
	assert(!playerBotTrainerDestinationRetained(route, {trainerTile}, {alternativeTile}));
	// A walking-only trainer leg must retain its actual conversation endpoint.
	auto finalWalk = route;
	finalWalk.metrics.firstNpcTravelOffer.reset();
	finalWalk.steps = {walk};
	assert(playerBotTrainerDestinationRetained(finalWalk, {approach}, {approach}));
	assert(!playerBotTrainerDestinationRetained(finalWalk, {approach, trainerTile}, {trainerTile}));

	PlayerBotRouteChanges::changed(approach, PlayerBotRouteChanges::Cause::TileFlag);
	assert(std::string(evidence.executionContextChange(evidence.actor, 546, 1, 2, evidence.risk)) == "execution_world_changed");
	// Landing consumes exactly one fare, then the remaining trip needs fresh
	// shared proof. The old actor context cannot survive the payment change.
	assert(playerBotTrainerFareAffordable(250, 0, 170, 70, 70));
	assert(!playerBotTrainerFareAffordable(250, 0, 170, 70, 71));
	assert(playerBotTrainerTravelReceipt(true, 250, 180, 70));
	assert(playerBotTrainerTripAffordable(180, 0, 170, 0));
	auto landedActor = evidence.actor;
	std::get<7>(landedActor) = 180; std::get<18>(landedActor) = landing;
	assert(std::string(evidence.executionContextChange(landedActor, 546, 1, 2, evidence.risk)) == "actor_facts_changed");
}

void arbitrationContention()
{
	using Arbitration = PlayerBotTrainerArbitration<PlayerBotSpellTrainingPlan>;
	using Clock = PlayerBotPlanningBudget::Clock;
	using namespace std::chrono_literals;
	for (const bool found : {false, true}) {
		const auto start = Clock::time_point{};
		PlayerBotPlanningBudget budget(start);
		Arbitration arbitration;
		unsigned trainerScans = 0, preparationEvaluations = 0;
		auto discover = [&] {
			++trainerScans;
			assert(budget.request(1, start).admitted); // Real shared admission for completed trainer proof.
			assert(!budget.request(2, start).admitted); // Another controller queues while it is owned.
			assert(budget.finish(1, start));
			return Arbitration::Result{false,
			    found ? std::optional<PlayerBotSpellTrainingPlan>(PlayerBotSpellTrainingPlan{}) : std::nullopt};
		};
		auto result = arbitration.advanceTrainer("unchanged semantic facts", true, discover);
		assert(!result.pending && result.selected.has_value() == found);
		assert(arbitration.phase() == Arbitration::Phase::Preparation);
		assert(!budget.request(1, start).admitted); // Preparation loses to controller 2, not infeasibility.
		assert(budget.request(2, start + 1ms).admitted);
		assert(budget.finish(2, start + 1ms));
		result = arbitration.advanceTrainer("unchanged semantic facts", true, [&]() -> Arbitration::Result {
			assert(false && "preparation retry must not rescan trainers or consume its admission");
			return {};
		});
		assert(result.selected.has_value() == found);
		assert(budget.request(1, start + 2ms).admitted);
		++preparationEvaluations;
		assert(budget.finish(1, start + 2ms));
		assert(trainerScans == 1 && preparationEvaluations == 1);

		// Changed facts, changed proof, and explicit interruption each invalidate,
		// but elapsed waiting or an unchanged negative result never does.
		for (const auto& facts : {"changed funds", "changed learned state"}) {
			arbitration.advanceTrainer(facts, true, [&] { ++trainerScans; return Arbitration::Result{}; });
		}
		arbitration.advanceTrainer("changed learned state", false, [&] { ++trainerScans; return Arbitration::Result{}; });
		arbitration.reset();
		arbitration.advanceTrainer("changed learned state", true, [&] { ++trainerScans; return Arbitration::Result{}; });
		assert(trainerScans == 5);
	}
}

void blockedLandingDetour()
{
	using namespace std::chrono_literals;
	const Position landing(100, 100, 7), direct(101, 100, 7), trainer(102, 100, 7);
	const auto goal = PlayerBotNavigationGoal::exact(trainer);
	PlayerBotNavigationRuntime navigation;
	PlayerBotNavigationRoutePlan original;
	original.metrics.result = PlayerBotNavigationResult::Reached;
	auto move = [](Position target, Direction direction) {
		PlayerBotNavigationStep step;
		step.action = PlayerBotNavigationAction::Move;
		step.direction = direction;
		step.target = step.expectedPosition = target;
		return step;
	};
	original.steps = {move(direct, DIRECTION_EAST), move(trainer, DIRECTION_EAST)};
	const auto now = std::chrono::steady_clock::time_point{};
	navigation.observePlan({goal, original, true, true, now});

	// One paid landing has happened. A failed *walking* step after it must
	// propagate its exclusion into replanning, not re-board/pay the captain.
	uint64_t funds = 210, remainingFare = 40;
	unsigned paidTrips = 0;
	auto dispatch = [&](const PlayerBotNavigationStep& step) {
		if (step.action != PlayerBotNavigationAction::NpcTravel) return true;
		if (!playerBotTrainerFareAffordable(funds, 0, 170, remainingFare, step.price)) return false;
		const auto before = funds;
		funds -= step.price;
		assert(playerBotTrainerTravelReceipt(true, before, funds, step.price));
		remainingFare -= step.price;
		++paidTrips;
		return true;
	};
	PlayerBotNavigationStep boat;
	boat.action = PlayerBotNavigationAction::NpcTravel;
	boat.price = 40;
	assert(dispatch(boat));
	navigation.observeStep({original.steps.front(), PlayerBotNavigationStepResult::Dispatched, now, 30s});
	auto outcome = navigation.process({landing, goal, false, true, {now + 1s, 1s, 30s, 30s}});
	assert(outcome.routeRequest && outcome.routeRequest->blockedPositions.count(direct));
	navigation.reset(); // Trainer invalidation resets steps, not short-lived block evidence.
	assert(navigation.activeBlockedPositions(now + 1s) == outcome.routeRequest->blockedPositions);

	PlayerBotHuntRouteRequest request;
	request.from = landing; request.to = trainer;
	request.blockedPositions = navigation.activeBlockedPositions(now + 1s);
	const auto identity = playerBotNavigationBlockedIdentity("actor/risk", request.blockedPositions);
	assert(identity != playerBotNavigationBlockedIdentity("actor/risk", {}));
	assert(identity == playerBotNavigationBlockedIdentity("actor/risk", {direct}));
	assert(identity != playerBotNavigationBlockedIdentity("actor/risk", {trainer}));
	assert(PlayerBotHuntRouteRequest{}.blockedPositions.empty()); // Existing exact-route callers unchanged.

	// Use the real sliced path search with this request's graph exclusions.
	// The direct path is shorter, but its first step is occupied. A four-step
	// cardinal detour exists; the immediate two-step sidestep cannot take it.
	PlayerBotPathSearch search(landing, goal, 32, true);
	unsigned slices = 0;
	auto expand = [&](Position from) {
		std::vector<PlayerBotPathSearch::Arc> arcs;
		for (auto direction : {DIRECTION_NORTH, DIRECTION_EAST, DIRECTION_SOUTH, DIRECTION_WEST}) {
			Position to = from;
			if (direction == DIRECTION_NORTH) --to.y;
			else if (direction == DIRECTION_SOUTH) ++to.y;
			else if (direction == DIRECTION_EAST) ++to.x;
			else --to.x;
			if (to.x < landing.x || to.x > trainer.x || to.y < landing.y || to.y > landing.y + 1 ||
			    request.blockedPositions.count(to)) continue;
			arcs.push_back({move(to, direction), 10, 0, 0});
		}
		return arcs;
	};
	while (!search.advance(1, expand, [&](Position p) { return p == trainer; }, [](Position) { return 0u; }))
		assert(++slices < 32);
	assert(search.result == PlayerBotNavigationResult::Reached && slices > 1 && search.steps.size() == 4);
	PlayerBotNavigationRoutePlan detour;
	detour.metrics.result = PlayerBotNavigationResult::Reached;
	detour.steps = search.steps;
	assert(playerBotNavigationRiskVerdict(PlayerBotNavigationRiskProfile{}, detour.metrics) == PlayerBotNavigationRiskVerdict::Accepted);
	assert(playerBotTrainerFareAffordable(funds, 0, 170, remainingFare, detour.metrics.fare));
	assert(!playerBotTrainerFareAffordable(funds, 0, 170, remainingFare, 40));
	navigation.observePlan({goal, std::move(detour), true, true, now + 1s});
	Position cursor = landing;
	for (const auto& step : search.steps) {
		auto next = navigation.process({cursor, goal, false, true, {now + 2s, 1s, 30s, 30s}});
		assert(next.nextStep && next.nextStep->target == step.target && step.target != direct);
		assert(next.nextStep->action == PlayerBotNavigationAction::Move); // No second NPC trip.
		assert(dispatch(*next.nextStep));
		navigation.observeStep({step, PlayerBotNavigationStepResult::Dispatched, now + 2s, 30s});
		cursor = step.expectedPosition;
	}
	assert(navigation.process({cursor, goal, false, true, {now + 3s, 1s, 30s, 30s}}).destinationReached);
	assert(paidTrips == 1 && funds == 170 && remainingFare == 0);
	assert(!dispatch(boat)); // A repeated paid leg cannot be authorized after the detour.
	assert(paidTrips == 1 && funds == 170);
	assert(navigation.activeBlockedPositions(now + 31s).empty());
}

void paidTravelThenLearning()
{
	PlayerBotProgressionRuntime runtime;
	PlayerBotSpellTrainingPlan plan;
	plan.npcId = 12;
	plan.spellName = "Light Healing";
	plan.keyword = "light healing";
	plan.price = 170;
	plan.fare = 40;
	runtime.beginSpellTraining(plan);
	PlayerBotSpellTrainingObservation observation;
	observation.totalMoney = 210;
	// Pending travel leaves the normal learning session in Travel without spending retries.
	for (unsigned turn = 0; turn < 10; ++turn) {
		assert(runtime.advanceSpellTraining(observation).type == PlayerBotProgressionOutcomeType::Pending);
		assert(runtime.spellTraining().retries() == 0);
	}
	assert(playerBotTrainerTravelReceipt(true, 210, 170, 40));
	assert(!playerBotTrainerTravelReceipt(false, 210, 170, 40));
	assert(!playerBotTrainerTravelReceipt(true, 210, 169, 40));
	assert(!playerBotTrainerTravelReceipt(true, 39, 0, 40));
	observation.totalMoney = 170;
	observation.navigationReached = true;
	assert(runtime.advanceSpellTraining(observation).command.reason == std::string("hi"));
	assert(runtime.reportNpcReply(1, 1, plan.npcId, TALKTYPE_PRIVATE_NP));
	observation.npcAvailable = observation.greetingAcknowledged = true;
	assert(runtime.advanceSpellTraining(observation).command.reason == std::string("request"));
	assert(runtime.advanceSpellTraining(observation).command.reason == std::string("yes"));
	observation.totalMoney = 0;
	observation.learned = true;
	assert(runtime.advanceSpellTraining(observation).type == PlayerBotProgressionOutcomeType::Succeeded);
	runtime.finish();
	assert(runtime.session().active() == PlayerBotProgressionProcedure::None);

	// A learned spell without the exact quoted payment remains a failed receipt.
	runtime.beginSpellTraining(plan);
	observation = {};
	observation.navigationReached = true;
	observation.npcAvailable = observation.greetingAcknowledged = true;
	observation.totalMoney = 170;
	runtime.advanceSpellTraining(observation);
	runtime.advanceSpellTraining(observation);
	runtime.advanceSpellTraining(observation);
	observation.learned = true;
	observation.totalMoney = 1;
	assert(runtime.advanceSpellTraining(observation).type == PlayerBotProgressionOutcomeType::Failed);

	// Lost focus and rejected learning are bounded, not blocking loops.
	runtime.beginSpellTraining(plan);
	observation = {};
	observation.navigationReached = observation.npcAvailable = true;
	runtime.advanceSpellTraining(observation);
	for (unsigned retry = 0; retry < 2; ++retry)
		assert(runtime.advanceSpellTraining(observation).type == PlayerBotProgressionOutcomeType::Retry);
	assert(runtime.advanceSpellTraining(observation).type == PlayerBotProgressionOutcomeType::Failed);
	runtime.restartSpellTrainingTravel();
	assert(runtime.spellTraining().stage() == PlayerBotSpellTrainingStage::Travel);
}
}

int main()
{
	paidQuoteBoundaries();
	scanAccountingAndRanking();
	localTrainerProofAndRankBound();
	discardedRoutePotionReserve();
	pendingOwnership();
	invalidation();
	executionLegValidity();
	paidTravelThenLearning();
	arbitrationContention();
	blockedLandingDetour();
	std::cout << "playerbot trainer contracts passed\n";
}
