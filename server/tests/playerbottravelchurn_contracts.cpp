#include "playerbottravelsafety.h"
#include "playerbotpathsearch.h"
#include "playerbothuntregions.h"

#include <cassert>
#include <iostream>

namespace {
using Field = PlayerBotTravelDangerField;
using Memory = PlayerBotTravelSafetyMemory;
const Position source(100, 100, 7), destination(140, 100, 7);

PlayerBotNavigationStep move(Position position) {
	PlayerBotNavigationStep step;
	step.action = PlayerBotNavigationAction::Move;
	step.direction = DIRECTION_EAST;
	step.target = step.expectedPosition = position;
	return step;
}

PlayerBotNavigationCostPolicy policy(std::shared_ptr<const Field> field) {
	PlayerBotNavigationCostPolicy result;
	result.expectedHealthLossPerSecond = [field](Position p) { return field->dangerAt(p); };
	result.transientHealthLossPerSecond = result.expectedHealthLossPerSecond;
	return result;
}

std::shared_ptr<const Field> field(Position position, double danger) {
	return std::make_shared<const Field>(std::vector<Field::Rectangle>{},
	    std::vector<Field::LocalThreat>{{position, danger}}, std::vector<Position>{});
}

void slicedRequestAndReturnReuse() {
	Memory memory;
	const auto start = Memory::Clock::time_point{} + std::chrono::seconds(100);
	const std::string planning = playerBotTravelPlanningIdentity(17, 9);
	assert(planning != playerBotTravelPlanningIdentity(18, 9)); // Private cache partition.
	const auto frozen = policy(field({100, 200, 7}, 0.4));
	PlayerBotNavigationGoal goal;
	goal.type = PlayerBotNavigationGoalType::Exact;
	goal.position = destination;
	PlayerBotPathSearch search(source, goal, 100);
	search.constrain(frozen.risk);
	auto expand = [&](Position from) {
		const Position next(from.x + 1, from.y, from.z);
		return std::vector<PlayerBotPathSearch::Arc>{{move(next), 10,
		    frozen.dangerCost(next, 1000), frozen.dangerAt(next)}};
	};
	uint64_t revision = 0;
	int slices = 0;
	while (!search.result) {
		// Hostile sightings move on every slice, including harmless movement
		// directly on the corridor. None changes the request's immutable field.
		memory.observeThreat(1, {uint16_t(100 + slices), 200, 7}, "dragon", start + std::chrono::seconds(slices));
		memory.observeThreat(2, {uint16_t(100 + slices), 100, 7}, "rat", start + std::chrono::seconds(slices));
		assert(memory.revision() > revision);
		revision = memory.revision();
		assert(playerBotTravelPlanningIdentity(17, 9) == planning);
		search.advance(1, expand, [](Position p) { return p == destination; },
		    [](Position p) { return playerBotNavigationDistance(p, destination); });
		++slices;
	}
	assert(slices == 41 && search.result == PlayerBotNavigationResult::Reached);
	assert(search.steps.size() == 40 && search.summary.dangerCost == 0);

	auto exposure = std::make_shared<PlayerBotTravelRouteExposure>();
	exposure->retain(search.steps, frozen, [](const auto&) { return 1000u; }, 0, 0);
	PlayerBotHuntReturnCoverageContext original;
	original.coveredPosition = source;
	original.depotDestination = destination;
	original.travelSafetyIdentity = planning;
	original.travelExposure = exposure;
	original.travelField = field({100, 200, 7}, 0.4);
	original.observationIdentity = "17:9:0";
	PlayerBotHuntReturnCoverage coverage;
	coverage.validate(2, 3, original);
	for (int observation = 0; observation < 60; ++observation) {
		auto live = original;
		live.observationIdentity = "17:9:" + std::to_string(observation + 1);
		live.travelField = field({uint16_t(100 + observation % 40), 100, 7}, 0.001);
		assert(!exposure->refreshed(*live.travelField).transientRiskRejected);
		assert(coverage.covers(2, 3, live));
		live.travelField = field({uint16_t(100 + observation), 200, 7}, 4.0);
		assert(coverage.covers(2, 3, live));
	}
	// Fresh on-route evidence prevents both admission and retained-proof reuse,
	// even though the completed search itself used a safe snapshot.
	auto live = original;
	live.observationIdentity = "17:9:999";
	live.travelField = field({120, 100, 7}, 0.4);
	assert(exposure->refreshed(*live.travelField).transientRiskRejected);
	assert(!coverage.covers(2, 3, live));
	const auto step = std::array{move({120, 100, 7})};
	assert(playerBotNavigationTransientRouteRisk(policy(live.travelField), step,
	    [](const auto&) { return 1000u; }).transientRiskRejected);
	live.travelField = original.travelField;
	live.travelSafetyIdentity = playerBotTravelPlanningIdentity(17, 10);
	assert(!coverage.covers(2, 3, live)); // A new raid wave invalidates coarse proofs.
}

void extendedReturnConnector() {
	const auto clear = policy(field({100, 200, 7}, 0));
	PlayerBotTravelRouteExposure original;
	original.retain(std::array{move({101, 100, 7})}, clear, [](const auto&) { return 1000u; }, 400, 0.04);
	std::deque<PlayerBotNavigationStep> outgoing;
	for (int x = 99; x >= 90; --x) {
		auto step = move({uint16_t(x), 100, 7});
		step.direction = DIRECTION_WEST;
		outgoing.push_back(step);
	}
	const auto reverse = playerBotTravelReverseWalk(source, outgoing);
	assert(reverse.size() == 10 && reverse.front().expectedPosition == Position(91, 100, 7));
	assert(reverse.back().expectedPosition == source);
	assert(reverse.front().direction == DIRECTION_EAST);
	PlayerBotTravelRouteExposure connector;
	connector.retain(reverse, clear, [](const auto&) { return 1000u; }, 0, 0);
	auto combined = std::make_shared<PlayerBotTravelRouteExposure>(original);
	combined->prepend(connector);
	assert(combined->dangerCost == 400 && combined->samples.size() == 11);
	PlayerBotHuntReturnCoverageContext context;
	context.coveredPosition = {90, 100, 7};
	context.travelExposure = combined;
	context.travelSafetyIdentity = playerBotTravelPlanningIdentity(17, 9);
	context.observationIdentity = "old";
	context.travelField = field({100, 200, 7}, 0);
	PlayerBotHuntReturnCoverage coverage;
	coverage.validate(2, 3, context);
	assert(!coverage.travelExposureAt(source)); // No implicit extension to another source.
	context.observationIdentity = "new";
	context.travelField = std::make_shared<const Field>(std::vector<Field::Rectangle>{{{95, 100, 7}, {95, 100, 7}, 0.4}},
	    std::vector<Field::LocalThreat>{}, std::vector<Position>{});
	assert(!original.refreshed(*context.travelField).transientRiskRejected);
	assert(!coverage.covers(2, 3, context)); // Danger occurs ONLY on the added connector.
	context.travelField = std::make_shared<const Field>(std::vector<Field::Rectangle>{{{91, 100, 7}, {100, 100, 7}, 0.02}},
	    std::vector<Field::LocalThreat>{}, std::vector<Position>{});
	assert(!coverage.covers(2, 3, context)); // 400 inherited + 200 connector exceeds the total budget.
	context.travelField = field({95, 100, 7}, 0.001);
	assert(coverage.covers(2, 3, context));
}

void cumulativeAndConsumedExposure() {
	PlayerBotNavigationCostPolicy baseline;
	baseline.expectedHealthLossPerSecond = [](Position) { return 0.04; };
	std::vector<PlayerBotNavigationStep> steps;
	for (int i = 0; i < 10; ++i) steps.push_back(move({uint16_t(101 + i), 100, 7}));
	PlayerBotTravelRouteExposure exposure;
	exposure.retain(steps, baseline, [](const auto&) { return 1000u; }, 400, 0.04);
	Field newExposure({{{101, 100, 7}, {110, 100, 7}, 0.06}}, {}, {});
	const auto rejected = exposure.refreshed(newExposure);
	assert(rejected.maximumHealthLossPerSecond == 0.06);
	assert(rejected.dangerCost == 600 && rejected.transientRiskRejected);
	Field empty({}, {}, {});
	assert(exposure.refreshed(empty).dangerCost == 400); // No manufactured budget.
	exposure.passed({110, 100, 7});
	assert(exposure.samples.empty());
	assert(!exposure.refreshed(newExposure).transientRiskRejected); // Behind us.

	// Below-peak transient exposure can combine with disjoint static exposure
	// to exceed the total budget, even when each component alone would fit.
	baseline.expectedHealthLossPerSecond = [](Position p) { return p.x <= 105 ? 0.08 : 0.; };
	exposure.retain(steps, baseline, [](const auto&) { return 1000u; }, 400, 0.08);
	Field mixed({{{106, 100, 7}, {110, 100, 7}, 0.04}}, {}, {});
	assert(exposure.refreshed(mixed).transientRiskRejected);
	assert(exposure.refreshed(mixed).dangerCost == 600);
}
}

int main() {
	slicedRequestAndReturnReuse();
	extendedReturnConnector();
	cumulativeAndConsumedExposure();
	std::cout << "playerbot travel churn contracts passed\n";
}
