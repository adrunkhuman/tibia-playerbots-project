/* Run from the repository root with the server build dependencies installed:
c++ -std=c++17 -Iserver/src -I/usr/include/luajit-2.1 \
  server/tests/playerbotserviceworkflow_test.cpp server/src/playerbotserviceworkflow.cpp \
  server/src/playerbotnpcsession.cpp server/src/playerbotservicesession.cpp server/src/playerboteconomy.cpp \
  -o /tmp/playerbotserviceworkflow_test && /tmp/playerbotserviceworkflow_test
*/
#include "otpch.h"

#include <cassert>
#include <iostream>

#include "playerbotserviceworkflow.h"

namespace {
constexpr uint32_t sellerId = 7;
const Position northTile(32399, 32219, 7);
const Position secondNorthTile(32400, 32219, 7);
const Position liveTile(32400, 32220, 7);

// Xodet's shape: customers stand north of a counter while he wanders behind it.
PlayerBotServiceObservation observe(std::vector<Position> approaches, const Position& current = Position(32352, 32226, 7))
{
	PlayerBotServiceObservation observation;
	observation.currentPosition = current;
	observation.shops.push_back({sellerId, Position(32399, 32222, 7), {{7634, 0, 5, 0}}});
	PlayerBotServiceProviderObservation provider{true, false, false, true};
	for (const Position& position : approaches) provider.approaches.push_back({position, 1});
	observation.providers[sellerId] = provider;
	observation.maximumAttempts = 3;
	return observation;
}

PlayerBotServiceWorkflow selling()
{
	PlayerBotServiceWorkflow workflow;
	workflow.reset(PlayerBotServiceIntent::ResupplyWithLocalSale);
	workflow.setLiquidationPlan({sellerId, {{7634, 10, 5, 0}}});
	return workflow;
}

PlayerBotServiceCommand select(PlayerBotServiceWorkflow& workflow, PlayerBotServiceObservation observation,
                               const Position& expected)
{
	const PlayerBotEconomyCatalog catalog;
	const PlayerBotDispositionPolicy disposition;
	PlayerBotServiceCommand command = workflow.advance(observation, catalog, disposition);
	assert(command.type == PlayerBotServiceCommandType::ValidateProviderRoute);
	assert(command.destination == expected);
	observation.approachRoute = {sellerId, expected, PlayerBotServiceRouteResult::Reached, 10};
	command = workflow.advance(observation, catalog, disposition);
	assert(command.type == PlayerBotServiceCommandType::Wait && command.outcome == PlayerBotServiceOutcome::Success);
	command = workflow.advance(observation, catalog, disposition);
	assert(command.type == PlayerBotServiceCommandType::NavigateProvider && command.destination == expected);
	return command;
}
}

int main()
{
	// The provider wandered out of range after the bot arrived, and no other
	// reachable tile is in range of it: wait for it to come back.
	PlayerBotServiceWorkflow workflow = selling();
	select(workflow, observe({northTile, secondNorthTile}), northTile);
	for (int wait = 0; wait < 15; ++wait) {
		assert(workflow.awaitProviderAtApproach(false) == PlayerBotServiceProviderWait::Wait);
	}
	// A release keeps the tile eligible and chooses from the live tiles, not
	// from the list ranked around the provider's earlier position.
	assert(workflow.awaitProviderAtApproach(false) == PlayerBotServiceProviderWait::Released);
	select(workflow, observe({liveTile, northTile}), liveTile);

	// Another reachable tile is in range now: move there without waiting.
	PlayerBotServiceWorkflow eager = selling();
	select(eager, observe({northTile}), northTile);
	assert(eager.awaitProviderAtApproach(true) == PlayerBotServiceProviderWait::Released);
	select(eager, observe({liveTile}), liveTile);

	// Repeated releases eventually reject the tile instead of chasing forever.
	PlayerBotServiceWorkflow restless = selling();
	for (int release = 0; release < 5; ++release) {
		select(restless, observe({northTile}), northTile);
		assert(restless.awaitProviderAtApproach(true) == PlayerBotServiceProviderWait::Released);
	}
	select(restless, observe({northTile, secondNorthTile}), northTile);
	assert(restless.awaitProviderAtApproach(true) == PlayerBotServiceProviderWait::Rejected);
	select(restless, observe({northTile, secondNorthTile}), secondNorthTile);

	// An occupied tile is used only after free tiles (see playerbotapproach.h).
	PlayerBotServiceWorkflow crowded = selling();
	PlayerBotServiceObservation occupied = observe({northTile, secondNorthTile});
	occupied.providers[sellerId].approaches.front().occupied = true;
	select(crowded, occupied, secondNorthTile);

	// Rejected routes move to the next tile; the shared bound then rejects the
	// provider even while tiles remain.
	PlayerBotServiceWorkflow unreachable = selling();
	const PlayerBotEconomyCatalog catalog;
	const PlayerBotDispositionPolicy disposition;
	std::vector<Position> tiles;
	for (int x = 0; x < 6; ++x) tiles.emplace_back(32396 + x, 32219, 7);
	PlayerBotServiceObservation blocked = observe(tiles);
	for (size_t tile = 0; tile < PlayerBotApproachLimits::rejectedTiles; ++tile) {
		PlayerBotServiceCommand command = unreachable.advance(blocked, catalog, disposition);
		assert(command.type == PlayerBotServiceCommandType::ValidateProviderRoute && command.destination == tiles[tile]);
		blocked.approachRoute = {sellerId, tiles[tile], PlayerBotServiceRouteResult::Unreachable, 0};
	}
	const PlayerBotServiceCommand rejected = unreachable.advance(blocked, catalog, disposition);
	assert(rejected.type == PlayerBotServiceCommandType::Wait && rejected.outcome == PlayerBotServiceOutcome::Retry &&
	       rejected.providerId == sellerId);

	std::cout << "service workflow regression tests passed\n";
}
