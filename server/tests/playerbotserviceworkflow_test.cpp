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
#include "playerbotsupplyrecovery.h"

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

PlayerBotServiceObservation paladinShop(uint64_t money, uint16_t vocationId = 3, bool split = false)
{
	auto observation = supplyShop({}, money,
	    {{7618, 45, 0, 0}, {7620, 50, 0, 0}, {2389, 10, 0, 0}});
	const auto mana = playerBotSupplyRule(PlayerBotSupplyKind::ManaPotion, vocationId);
	const auto spears = playerBotThrowingWeaponRule(2389, 8);
	observation.supplyCapacityReserve = 3000;
	observation.supplies = {
	    {PlayerBotSupplyKind::HealthPotion, 7618, 270, 1, 20, 2},
	    {mana.kind, mana.itemId, 270, mana.returnThreshold, mana.target, mana.safetyFloor, mana.preferredStock},
	    {spears.kind, spears.itemId, 2000, spears.returnThreshold, spears.target, spears.safetyFloor}};
	if (split) {
		observation.shops = {
		    {1, observation.currentPosition, {{7618, 45, 0, 0}}},
		    {2, observation.currentPosition, {{7620, 50, 0, 0}}},
		    {3, observation.currentPosition, {{2389, 10, 0, 0}}}};
		for (uint32_t id : {2U, 3U}) observation.providers.emplace(id,
		    PlayerBotServiceProviderObservation{true, true, true, true});
	}
	return observation;
}

// Apply normal paid receipts and capacity changes, then let the workflow verify
// each purchase. Split shops stay in range to isolate allocation from routing.
void finishShopping(PlayerBotServiceObservation& observation, bool survival = false, bool split = false)
{
	PlayerBotServiceWorkflow workflow;
	workflow.setSurvivalRestock(survival);
	const PlayerBotEconomyCatalog catalog;
	const PlayerBotDispositionPolicy disposition;
	uint32_t purchases = 0;
	uint32_t receipts = 0;
	for (int turn = 0; turn < 40; ++turn) {
		const auto command = workflow.advance(observation, catalog, disposition);
		assert(command.type != PlayerBotServiceCommandType::Fail);
		if (command.verification) {
			assert(command.verification->result == PlayerBotServiceVerificationResult::Success);
			++receipts;
		}
		if (command.type == PlayerBotServiceCommandType::Complete) {
			assert(purchases == receipts);
			assert(workflow.advance(observation, catalog, disposition).type == PlayerBotServiceCommandType::Complete);
			return;
		}
		if (command.type != PlayerBotServiceCommandType::Buy) continue;
		++purchases;
		assert(command.transaction && command.transaction->itemCount == observation.inventoryCounts[command.itemId]);
		if (split) assert(command.providerId == (command.itemId == 7618 ? 1U : command.itemId == 7620 ? 2U : 3U));
		const uint64_t cost = static_cast<uint64_t>(command.amount) * command.transaction->unitPrice;
		assert(cost <= observation.money + observation.bankBalance);
		const uint64_t carriedPayment = std::min(observation.money, cost);
		observation.money -= carriedPayment;
		observation.bankBalance -= cost - carriedPayment;
		const auto supply = std::find_if(observation.supplies.begin(), observation.supplies.end(),
		    [&](const auto& value) { return value.itemId == command.itemId; });
		assert(supply != observation.supplies.end());
		assert(command.amount * supply->weight <= observation.freeCapacity);
		observation.freeCapacity -= command.amount * supply->weight;
		assert(observation.freeCapacity >= observation.supplyCapacityReserve);
		observation.inventoryCounts[command.itemId] += command.amount;
	}
	assert(false && "shopping did not complete");
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

	// Recovery survives the production resupply/liquidation reset handoff.
	// A verified 50-gp sale cannot fund the 90-gp floor, but buys one potion
	// in recovery; the same sale under normal policy must still fail.
	for (bool recovering : {false, true}) {
		PlayerBotSupplyRecoveryState recovery;
		if (recovering) assert(recovery.enter());
		PlayerBotServiceWorkflow supplies;
		supplies.setSurvivalRestock(true); // A reset must also clear stale recovery policy.
		auto observation = supplyShop({{7618, 0}}, 50, {{7618, 45, 0, 0}, {7634, 0, 5, 0}});
		observation.supplies = {{PlayerBotSupplyKind::HealthPotion, 7618, 270, 1, 20, 2}};
		supplies.reset(PlayerBotServiceIntent::Resupply, recovery.active());
		auto command = supplies.advance(observation, catalog, disposition);
		assert(recovering ? command.type == PlayerBotServiceCommandType::Buy && command.amount == 1 :
		       command.type == PlayerBotServiceCommandType::Fail &&
		       command.outcome == PlayerBotServiceOutcome::InsufficientFunds);

		observation.money = 0;
		observation.inventoryCounts[7634] = observation.backpackSaleCounts[7634] = 10;
		supplies.reset(PlayerBotServiceIntent::ResupplyWithLocalSale, recovery.active());
		supplies.setLiquidationPlan({1, {{7634, 10, 5, 0}}});
		command = supplies.advance(observation, catalog, disposition);
		assert(command.type == PlayerBotServiceCommandType::Sell && command.itemId == 7634 && command.amount == 10);
		assert(command.transaction && command.transaction->unitPrice == 5);
		observation.inventoryCounts[7634] = observation.backpackSaleCounts[7634] = 0;
		observation.money = 50;
		command = supplies.advance(observation, catalog, disposition);
		assert(command.verification && command.verification->result == PlayerBotServiceVerificationResult::Success);
		assert(command.transaction && command.transaction->itemId == 7634);
		assert(!supplies.liquidation() && supplies.stage() == PlayerBotServiceStage::BuySupplies);
		if (recovering) {
			// The controller's recovery update sees no mode transition, so it
			// cannot repair a policy lost at reset after observing this receipt.
			assert(!recovery.update(observation.money + observation.bankBalance, 2 * 45));
			assert(recovery.active());
		}
		command = supplies.advance(observation, catalog, disposition);
		assert(command.itemId == 7618);
		if (!recovering) {
			assert(command.type == PlayerBotServiceCommandType::Fail &&
			       command.outcome == PlayerBotServiceOutcome::InsufficientFunds);
			assert(observation.inventoryCounts[7618] == 0 && observation.money == 50);
			continue;
		}
		assert(command.type == PlayerBotServiceCommandType::Buy && command.amount == 1);
		assert(command.transaction && command.transaction->unitPrice == 45);
		observation.inventoryCounts[7618] = 1;
		observation.money = 5;
		observation.freeCapacity -= 270;
		command = supplies.advance(observation, catalog, disposition);
		assert(command.verification && command.verification->result == PlayerBotServiceVerificationResult::Success);
		assert(command.transaction && command.transaction->itemId == 7618);
		assert(!recovery.update(observation.money + observation.bankBalance, 45) && recovery.active());
		assert(supplies.advance(observation, catalog, disposition).type == PlayerBotServiceCommandType::Complete);
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

	// Both Paladin vocations prefer ten mana after ALL active floors, not
	// instead of the spear floor or as a new mandatory departure requirement.
	for (uint16_t vocation : {3, 7}) {
		const auto mana = playerBotSupplyRule(PlayerBotSupplyKind::ManaPotion, vocation);
		assert(mana.safetyFloor == 2 && mana.returnThreshold == 1 && mana.target == 20 && mana.preferredStock == 10);
		const PlayerBotSupplyStocks floorStock{{mana, 2}};
		assert(playerBotMandatorySupplyDeficit(floorStock, false).missing == 0);
		assert(playerBotExhaustedSupply(floorStock) == nullptr);
		for (bool survival : {false, true}) {
			for (bool split : {false, true}) {
				auto poor = paladinShop(620, vocation, split);
				finishShopping(poor, survival, split);
				assert(poor.inventoryCounts[7618] == 2 && poor.inventoryCounts[7620] == 10 && poor.inventoryCounts[2389] == 3);
				assert(poor.money == 0);

				// Less than ten is allowed: keep the floors, buy a partial
				// preference, and finish without failure or another service.
				auto partial = paladinShop(500, vocation, split);
				finishShopping(partial, survival, split);
				assert(partial.inventoryCounts[7618] == 2 && partial.inventoryCounts[7620] == (survival ? 7U : 5U));
				assert(partial.inventoryCounts[2389] == 3);

				auto ample = paladinShop(2000, vocation, split);
				finishShopping(ample, survival, split);
				assert(ample.inventoryCounts[7618] == 20 && ample.inventoryCounts[7620] == 20 && ample.inventoryCounts[2389] == 3);
				assert(ample.money == 70);
			}
		}
	}
	{
		// Extra funds top up health only AFTER reserving the mana preference.
		auto optional = paladinShop(820);
		finishShopping(optional);
		assert(optional.inventoryCounts[7618] == 4 && optional.inventoryCounts[7620] == 10 && optional.inventoryCounts[2389] == 3);

		// Shared capacity follows the same tiers and keeps the hunting buffer.
		for (uint32_t capacity : {11700U, 12780U}) {
			auto limited = paladinShop(10000, 3, true);
			limited.freeCapacity = capacity;
			finishShopping(limited, false, true);
			assert(limited.inventoryCounts[7618] == (capacity == 11700 ? 2U : 4U));
			assert(limited.inventoryCounts[7620] == (capacity == 11700 ? 8U : 10U));
			assert(limited.inventoryCounts[2389] == 3 && limited.freeCapacity == 3000);
		}

		// Keep the first-heal saving target at two; route-required health
		// reserves still take precedence over preferred mana.
		auto saving = paladinShop(590);
		saving.supplies.front().restockTarget = 2;
		saving.inventoryCounts[2389] = 3;
		finishShopping(saving);
		assert(saving.inventoryCounts[7618] == 2 && saving.inventoryCounts[7620] == 10);
		auto route = paladinShop(800, 7, true);
		route.supplies.front().returnThreshold = 5;
		route.supplies.front().restockTarget = 6;
		finishShopping(route, false, true);
		assert(route.inventoryCounts[7618] == 6 && route.inventoryCounts[7620] == 10 && route.inventoryCounts[2389] == 3);

		// Bank funds use the same allocation and exact payment receipts.
		auto bank = paladinShop(90, 3, true);
		bank.bankBalance = 530;
		finishShopping(bank, false, true);
		assert(bank.inventoryCounts[7618] == 2 && bank.inventoryCounts[7620] == 10 && bank.inventoryCounts[2389] == 3);
		assert(bank.money == 0 && bank.bankBalance == 0);

		auto excess = paladinShop(0);
		excess.supplies.front().restockTarget = 2;
		excess.inventoryCounts = {{7618, 3}, {7620, 25}, {2389, 3}};
		finishShopping(excess);
		assert(excess.inventoryCounts[7618] == 3 && excess.inventoryCounts[7620] == 25);
	}
	// A hunt requirement raises the mandatory floor above the first-heal
	// saving ceiling. It is bought before the optional mana preference, even
	// when only the exact required health cost is affordable.
	for (bool survival : {false, true}) {
		auto required = paladinShop(45);
		required.inventoryCounts = {{7618, 2}, {7620, 2}, {2389, 3}};
		PlayerBotSupplyStocks stocks{{{PlayerBotSupplyKind::HealthPotion, 7618, 2, 1, 2}, 2}};
		playerBotRequireSupplies(stocks, {{PlayerBotSupplyKind::HealthPotion, 0, 3}});
		required.supplies.front().restockTarget = stocks.front().rule.target;
		required.supplies.front().safetyFloor = stocks.front().rule.safetyFloor;
		PlayerBotServiceWorkflow mandatory;
		mandatory.setSurvivalRestock(survival);
		const PlayerBotEconomyCatalog catalog;
		const PlayerBotDispositionPolicy disposition;
		auto command = mandatory.advance(required, catalog, disposition);
		for (int i = 0; i < 10 && command.type != PlayerBotServiceCommandType::Buy; ++i)
			command = mandatory.advance(required, catalog, disposition);
		assert(command.type == PlayerBotServiceCommandType::Buy && command.itemId == 7618 && command.amount == 1);
		finishShopping(required, survival);
		assert(required.inventoryCounts[7618] == 3 && required.inventoryCounts[7620] == 2 && required.money == 0);

		auto poor = required;
		poor.inventoryCounts[7618] = 2;
		poor.money = 11;
		if (!survival) {
			PlayerBotServiceWorkflow unfunded;
			auto failure = unfunded.advance(poor, catalog, disposition);
			for (int i = 0; i < 10 && failure.type != PlayerBotServiceCommandType::Fail; ++i)
				failure = unfunded.advance(poor, catalog, disposition);
			assert(failure.type == PlayerBotServiceCommandType::Fail &&
			       failure.outcome == PlayerBotServiceOutcome::InsufficientFunds && failure.itemId == 7618);
		} else {
			finishShopping(poor, true);
			assert(poor.inventoryCounts[7618] == 2); // Controller retains/deferRestock's route shortage.
		}
	}

	// Knight and all other inactive mana rules retain zero preference. A
	// Knight's health-only shopping still reaches its ordinary full target.
	for (uint16_t vocation : {0, 1, 2, 4, 5, 6, 8}) {
		const auto mana = playerBotSupplyRule(PlayerBotSupplyKind::ManaPotion, vocation);
		assert(!mana.active() && mana.preferredStock == 0);
	}
	for (bool survival : {false, true}) {
		auto knight = supplyShop({{7620, 4}}, 1000, {{7618, 45, 0, 0}});
		knight.supplies = {{PlayerBotSupplyKind::HealthPotion, 7618, 270, 1, 20, 2}};
		finishShopping(knight, survival);
		assert(knight.inventoryCounts[7618] == 20 && knight.inventoryCounts[7620] == 4 && knight.money == 100);
	}

	// Food is a required preparation transaction, independent of the normal
	// unaffordable three-health-potion floor, and does not start a bank loop.
	{
		auto food = supplyShop({{7618, 2}, {2666, 0}}, 15, {{2666, 4, 0, 0}});
		food.supplies = {{std::nullopt, 2666, 0, 0, 1, 1}};
		assert(!food.supplies.front().kind); // Food is a neutral transaction, not a potion.
		PlayerBotServiceWorkflow workflow;
		workflow.reset(PlayerBotServiceIntent::PreparationPurchase);
		workflow.setSurvivalRestock(true);
		const PlayerBotEconomyCatalog catalog;
		const PlayerBotDispositionPolicy disposition;
		const Position upstairs(1, 1, 7);
		food.currentPosition = Position(1, 1, 8);
		food.providers[1] = PlayerBotServiceProviderObservation{true, false, false, true};
		food.providers[1].approaches = {{upstairs, 1}};
		auto command = workflow.advance(food, catalog, disposition);
		assert(command.type == PlayerBotServiceCommandType::ValidateProviderRoute && command.destination == upstairs);
		assert(!workflow.buyingPotions()); // Survival stays available during approach, not only after delivery.
		food.approachRoute = {1, upstairs, PlayerBotServiceRouteResult::Reached, 40};
		command = workflow.advance(food, catalog, disposition);
		command = workflow.advance(food, catalog, disposition);
		assert(command.type == PlayerBotServiceCommandType::NavigateProvider && command.destination == upstairs);
		food.currentPosition = upstairs;
		food.providers[1].inRange = food.providers[1].shopOpen = true; // Observed arrival and ordinary trade.
		for (int i = 0; i < 10 && command.type != PlayerBotServiceCommandType::Buy; ++i)
			command = workflow.advance(food, catalog, disposition);
		assert(command.type == PlayerBotServiceCommandType::Buy && command.itemId == 2666 && command.amount == 1);
		assert(workflow.stage() == PlayerBotServiceStage::BuySupplies);
		assert(!workflow.buyingPotions()); // Neutral approach/trade cannot disable survival.
		assert(workflow.pendingPreparationPurchase());
		auto undelivered = workflow;
		command = undelivered.retirePreparationPurchase(food);
		assert(command.verification && command.verification->result == PlayerBotServiceVerificationResult::Rejected);
		assert(undelivered.intent() == PlayerBotServiceIntent::Resupply &&
		       undelivered.stage() == PlayerBotServiceStage::Discover && !undelivered.pendingPreparationPurchase());
		food.inventoryCounts[2666] = 1;
		food.money = 11;
		// Timeout/interruption between synchronous delivery and the next service
		// turn must observe the exact receipt before releasing food consumption.
		auto timedOut = workflow;
		command = timedOut.retirePreparationPurchase(food);
		assert(command.verification && command.verification->result == PlayerBotServiceVerificationResult::Success);
		assert(command.transaction && command.transaction->itemId == 2666);
		assert(timedOut.intent() == PlayerBotServiceIntent::Resupply &&
		       timedOut.stage() == PlayerBotServiceStage::Discover && !timedOut.buyingPotions() &&
		       !timedOut.pendingPreparationPurchase());
		assert(timedOut.retirePreparationPurchase(food).type == PlayerBotServiceCommandType::None);
		auto mismatched = workflow;
		auto badReceipt = food; badReceipt.money = 10;
		command = mismatched.retirePreparationPurchase(badReceipt);
		assert(command.verification && command.verification->result == PlayerBotServiceVerificationResult::Mismatch);
		assert(!mismatched.pendingPreparationPurchase() && !mismatched.buyingPotions());
		command = workflow.advance(food, catalog, disposition);
		assert(command.verification && command.verification->result == PlayerBotServiceVerificationResult::Success);
		command = workflow.advance(food, catalog, disposition);
		assert(command.type == PlayerBotServiceCommandType::Complete);
		assert(food.inventoryCounts[7618] == 2 && food.money == 11);
	}
	std::cout << "service workflow regression tests passed\n";
}
