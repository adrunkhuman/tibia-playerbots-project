/* Run from the repository root:
c++ -std=c++17 -Iserver/src $(pkg-config --cflags luajit) \
  server/tests/playerbotdepotworkflow_test.cpp server/src/playerbotdepotworkflow.cpp \
  server/src/playerbotdepotsession.cpp -o /tmp/playerbotdepotworkflow_test && /tmp/playerbotdepotworkflow_test
*/
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include "playerbotdepotworkflow.h"
#include "playerbotnavigation.h"

#include <cassert>

namespace {
const Position origin(32345, 32225, 7);
const PlayerBotDepotCandidate nearest{2, 2591, {32352, 32225, 7}, {32352, 32226, 7}, 0};
const PlayerBotDepotCandidate upstairs{2, 2591, {32344, 32225, 6}, {32344, 32226, 6}, 0};

PlayerBotDepotCommand scan(PlayerBotDepotWorkflow& workflow, std::vector<PlayerBotDepotCandidate> candidates,
                           const Position& currentPosition = origin)
{
	PlayerBotDepotObservation observation;
	observation.currentPosition = currentPosition;
	observation.scan.observed = true;
	observation.scan.candidates = std::move(candidates);
	return workflow.advance(observation, 2, 4, std::chrono::seconds(2));
}

void expectFirst(std::vector<PlayerBotDepotCandidate> candidates, const PlayerBotDepotCandidate& expected)
{
	for (int order = 0; order < 2; ++order) {
		PlayerBotDepotWorkflow workflow;
		const auto command = scan(workflow, candidates);
		assert(command.type == PlayerBotDepotCommandType::ValidateRoute);
		assert(!command.snapshot.hasSelectedDepot);
		assert(command.snapshot.routeCandidate.depotId == expected.depotId);
		assert(command.snapshot.routeCandidate.lockerPosition == expected.lockerPosition);
		assert(command.snapshot.routeCandidate.approachPosition == expected.approachPosition);
		std::reverse(candidates.begin(), candidates.end());
	}
}
}

int main()
{
	assert(playerBotDepotRouteSafetyAccepted(true, false, false, true));
	assert(playerBotDepotRouteSafetyAccepted(false, true, false, false));
	assert(!playerBotDepotRouteSafetyAccepted(false, true, false, true));
	assert(!playerBotDepotRouteSafetyAccepted(false, true, true, false));

	// Actual real_depot origin and endpoints: the upstairs route took 16 steps,
	// but both topology costs were zero. The old floor-first tie chose upstairs.
	assert(playerBotNavigationDistance(origin, nearest.approachPosition) == 8);
	assert(playerBotNavigationDistance(origin, upstairs.approachPosition) == 22);
	expectFirst({upstairs, nearest}, nearest);

	auto cheaperTopology = upstairs;
	auto closerSpatially = nearest;
	cheaperTopology.distance = 2;
	closerSpatially.distance = 3;
	expectFirst({closerSpatially, cheaperTopology}, cheaperTopology);

	auto lowerId = nearest;
	lowerId.depotId = 1;
	expectFirst({nearest, lowerId}, lowerId);
	auto lowerLocker = nearest;
	lowerLocker.lockerPosition.x = 32351;
	expectFirst({nearest, lowerLocker}, lowerLocker);
	auto lowerApproach = nearest;
	lowerApproach.approachPosition = Position(32353, 32225, 7);
	expectFirst({nearest, lowerApproach}, lowerApproach);

	// Post-hunt discovery ranks from the current hunting area, not the
	// departure city. A Darashia-area bot therefore selects its viable local
	// depot even if the candidate list still contains Carlin.
	const Position darashiaHunt(33200, 32400, 7);
	PlayerBotDepotCandidate carlinDepot{2, 2591, {32352, 32225, 7}, {32352, 32226, 7}, 12};
	PlayerBotDepotCandidate darashiaDepot{7, 2591, {33210, 32400, 7}, {33209, 32400, 7}, 1};
	PlayerBotDepotWorkflow remoteWorkflow;
	auto remoteCommand = scan(remoteWorkflow, {carlinDepot, darashiaDepot}, darashiaHunt);
	assert(remoteCommand.type == PlayerBotDepotCommandType::ValidateRoute);
	assert(remoteCommand.snapshot.routeCandidate.depotId == darashiaDepot.depotId);

	// Fallback is revalidated rather than selected directly when all
	// candidates are unsafe.
	PlayerBotDepotWorkflow workflow;
	scan(workflow, {upstairs, nearest});
	PlayerBotDepotObservation observation;
	observation.currentPosition = origin;
	observation.routeResult = PlayerBotDepotRouteResult::Unsafe;
	observation.dangerCost = 10;
	auto command = workflow.advance(observation, 2, 4, std::chrono::seconds(2));
	assert(command.snapshot.routeCandidate.lockerPosition == upstairs.lockerPosition);
	observation.dangerCost = 20;
	command = workflow.advance(observation, 2, 4, std::chrono::seconds(2));
	assert(command.type == PlayerBotDepotCommandType::ValidateRoute);
	assert(command.snapshot.validatingRiskFallback);
	assert(command.snapshot.routeCandidate.lockerPosition == nearest.lockerPosition);
	observation.routeResult = PlayerBotDepotRouteResult::Reached;
	command = workflow.advance(observation, 2, 4, std::chrono::seconds(2));
	assert(command.type == PlayerBotDepotCommandType::Navigate);
	assert(command.telemetry.riskFallback);
	std::cout << "depot workflow regression tests passed\n";
}
