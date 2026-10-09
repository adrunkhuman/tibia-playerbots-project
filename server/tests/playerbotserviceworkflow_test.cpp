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
PlayerBotServiceObservation supplyShop(std::map<uint16_t, uint32_t> counts, uint64_t money,
                                       std::vector<PlayerBotEconomyOffer> offers)
{
	PlayerBotServiceObservation observation;
	observation.currentPosition = Position(1, 1, 7);
	observation.shops.push_back({1, observation.currentPosition, std::move(offers)});
	observation.providers.emplace(1, PlayerBotServiceProviderObservation{true, true, true, true});
	observation.inventoryCounts = std::move(counts);
	observation.freeCapacity = 100000;
	observation.money = money;
	observation.maximumAttempts = 3;
	observation.supplies = {{PlayerBotSupplyKind::HealthPotion, 7618, 0, 1, 10}, {PlayerBotSupplyKind::ManaPotion, 7620, 0, 1, 10}};
	return observation;
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

	// Typed supplies buy in priority order from one budget: health tops up
	// first, then mana, which was below its floor.
	{
		PlayerBotServiceWorkflow supplies;
		PlayerBotServiceObservation observation = supplyShop({{7618, 5}, {7620, 0}}, 1000, {{7618, 50, 0, 0}, {7620, 50, 0, 0}});
		PlayerBotServiceCommand command = supplies.advance(observation, catalog, disposition);
		assert(command.type == PlayerBotServiceCommandType::Buy && command.itemId == 7618 && command.amount == 5);
		observation.inventoryCounts[7618] = 10;
		observation.money = 750;
		for (int step = 0; step < 4 && !(command.type == PlayerBotServiceCommandType::Buy && command.itemId == 7620); ++step) {
			command = supplies.advance(observation, catalog, disposition);
		}
		assert(command.type == PlayerBotServiceCommandType::Buy && command.itemId == 7620 && command.amount == 10);

		// A floor gold cannot cover fails service with InsufficientFunds.
		PlayerBotServiceWorkflow poor;
		const PlayerBotServiceCommand unaffordable = poor.advance(
		    supplyShop({{7618, 10}, {7620, 0}}, 60, {{7618, 50, 0, 0}, {7620, 50, 0, 0}}), catalog, disposition);
		assert(unaffordable.type == PlayerBotServiceCommandType::Fail &&
		       unaffordable.outcome == PlayerBotServiceOutcome::InsufficientFunds);

		// An active kind without a discovered offer cannot complete service.
		PlayerBotServiceWorkflow unstocked;
		const PlayerBotServiceCommand unavailable = unstocked.advance(
		    supplyShop({{7618, 10}, {7620, 0}}, 1000, {{7618, 50, 0, 0}}), catalog, disposition);
		assert(unavailable.type == PlayerBotServiceCommandType::Fail &&
		       unavailable.outcome == PlayerBotServiceOutcome::Unavailable);
	}

	// Missing spears are supplies, not an optional equipment upgrade: 39 gp
	// may buy the whole floor even though it is below the 100-gp cash reserve.
	for (bool survival : {false, true}) {
		PlayerBotServiceWorkflow supplies;
		supplies.setSurvivalRestock(survival);
		auto observation = supplyShop({{7618, 20}, {7620, 20}, {2389, 0}}, 39,
		    {{7618, 45, 0, 0}, {7620, 50, 0, 0}, {2389, 10, 0, 0}});
		observation.supplyCapacityReserve = 3000;
		observation.supplies = {
		    {PlayerBotSupplyKind::HealthPotion, 7618, 270, 1, 20, 2},
		    {PlayerBotSupplyKind::ManaPotion, 7620, 270, 1, 20, 2},
		    {PlayerBotSupplyKind::ThrowingWeapon, 2389, 2000, 1, 3, 3}};
		const auto purchase = supplies.advance(observation, catalog, disposition);
		assert(purchase.type == PlayerBotServiceCommandType::Buy && purchase.itemId == 2389 && purchase.amount == 3);
		assert(purchase.transaction && purchase.transaction->unitPrice == 10);
		observation.money = 9;
		observation.inventoryCounts[2389] = 3;
		bool complete = false;
		for (int step = 0; step < 5; ++step) {
			const auto next = supplies.advance(observation, catalog, disposition);
			assert(next.type != PlayerBotServiceCommandType::Fail && next.type != PlayerBotServiceCommandType::Buy);
			if (next.type == PlayerBotServiceCommandType::Complete) { complete = true; break; }
		}
		assert(complete);
	}

	// A level-8 Paladin's gear and three spears leave 110 oz. Shopping keeps
	// the 30 oz hunting buffer rather than sending protected stock to a depot.
	for (bool survival : {false, true}) {
		for (uint64_t funds : {1000ULL, 10000ULL}) {
			PlayerBotServiceWorkflow supplies;
			supplies.setSurvivalRestock(survival);
			auto observation = supplyShop({{7618, 0}, {7620, 0}, {2389, 3}}, funds,
			    {{7618, 45, 0, 0}, {7620, 50, 0, 0}, {2389, 10, 0, 0}});
			observation.freeCapacity = 11000;
			observation.supplyCapacityReserve = 3000;
			observation.supplies = {
			    {PlayerBotSupplyKind::HealthPotion, 7618, 270, 1, 20, 2},
			    {PlayerBotSupplyKind::ManaPotion, 7620, 270, 1, 20, 2},
			    {PlayerBotSupplyKind::ThrowingWeapon, 2389, 2000, 1, 3, 3}};
			bool complete = false;
			for (int turn = 0; turn < 20; ++turn) {
				const auto command = supplies.advance(observation, catalog, disposition);
				assert(command.type != PlayerBotServiceCommandType::Fail);
				if (command.type == PlayerBotServiceCommandType::Complete) {
					complete = true;
					break;
				}
				if (command.type != PlayerBotServiceCommandType::Buy) continue;
				assert(command.itemId == 7618 || command.itemId == 7620);
				observation.inventoryCounts[command.itemId] += command.amount;
				observation.freeCapacity -= command.amount * 270;
				assert(command.transaction);
				observation.money -= command.amount * command.transaction->unitPrice;
				assert(observation.freeCapacity >= observation.supplyCapacityReserve);
			}
			assert(complete && observation.inventoryCounts.at(7618) == 20);
			assert(observation.inventoryCounts.at(7620) == (funds == 1000 ? 2 : 9));
			assert(observation.inventoryCounts.at(2389) == 3);
			assert(observation.freeCapacity == (funds == 1000 ? 5060 : 3170));
			// Optional targets still below their ceiling do not restart shopping.
			assert(supplies.advance(observation, catalog, disposition).type == PlayerBotServiceCommandType::Complete);
		}
	}
	for (uint32_t capacity : {0U, 2999U, 3000U}) {
		PlayerBotServiceWorkflow supplies;
		auto observation = supplyShop({{7618, 2}, {7620, 2}}, 10000,
		    {{7618, 50, 0, 0}, {7620, 50, 0, 0}});
		observation.freeCapacity = capacity;
		observation.supplyCapacityReserve = 3000;
		for (auto& supply : observation.supplies) supply.weight = 270;
		assert(supplies.advance(observation, catalog, disposition).type == PlayerBotServiceCommandType::Complete);
	}

	std::cout << "service workflow regression tests passed\n";
}
