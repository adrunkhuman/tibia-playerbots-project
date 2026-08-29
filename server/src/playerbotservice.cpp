/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "otpch.h"

#include "playerbotcontroller.h"
#include "playerbotnpccapabilities.h"
#include "playerbottopology.h"

#include "depotchest.h"
#include "depotlocker.h"

// NPC service discovery, shopping, banking, and depot handling.
using namespace playerbot;

namespace {
	constexpr size_t maximumServiceProviderApproaches = 8;
	constexpr size_t maximumServiceProvidersPerItem = 4;
	constexpr size_t maximumServiceBankers = 2;
	constexpr size_t maximumSellLootItems = 256;
	constexpr size_t maximumSellLootRouteValidationsPerDecision = 1;
	constexpr uint32_t maximumSellLootBatch = 100;
	constexpr uint64_t maximumSellLootRouteNodes = 30000;
	constexpr uint32_t sellLootTravelTimeGoldPerMinute = 10;
	constexpr std::chrono::seconds sellLootFailureCooldown(60);

	int32_t approachDirection(const Position& provider, const Position& approach)
	{
		const int32_t x = approach.x < provider.x ? 0 : approach.x > provider.x ? 2 : 1;
		const int32_t y = approach.y < provider.y ? 0 : approach.y > provider.y ? 2 : 1;
		return x * 3 + y;
	}
}

bool PlayerBotController::planSellLootTrip(Player& player, uint16_t currentDepotId, const Position& position)
{
	sellLootPlan.reset();
	std::vector<Npc*> sellers;
	for (const auto& entry : g_game.getNpcs()) {
		Npc* npc = entry.second;
		if (npc && !npc->isRemoved() && playerBotNpcHasCapability(*npc, PlayerBotNpcCapability::Shop)) sellers.push_back(npc);
	}
	struct Candidate {
		uint16_t sourceDepotId = 0;
		Position sourceApproach;
		Npc* provider = nullptr;
		Position providerApproach;
		std::vector<SellLootBatch> batches;
		uint64_t revenue = 0;
		uint64_t roughCost = 0;
	};
	std::vector<Candidate> candidates;
	uint32_t scannedItems = 0;
	size_t itemWindowCount = 1;
	for (const auto& depotEntry : g_game.map.getDepotLockerPositions()) {
		DepotChest* chest = player.getDepotChest(depotEntry.first, false);
		if (!chest || chest->empty()) continue;
		Position sourceApproach;
		uint32_t sourceDistance = std::numeric_limits<uint32_t>::max();
		for (const Position& locker : depotEntry.second) {
			for (int32_t xOffset = -1; xOffset <= 1; ++xOffset) for (int32_t yOffset = -1; yOffset <= 1; ++yOffset) {
				if (xOffset == 0 && yOffset == 0) continue;
				const Position approach(locker.x + xOffset, locker.y + yOffset, locker.z);
				Tile* tile = g_game.map.getTile(approach);
				if (!tile || tile->queryAdd(0, player, 1, FLAG_IGNOREBLOCKCREATURE) != RETURNVALUE_NOERROR) continue;
				const uint32_t distance = playerBotNavigationDistance(position, approach);
				if (distance < sourceDistance) {
					sourceDistance = distance;
					sourceApproach = approach;
				}
			}
		}
		if (sourceDistance == std::numeric_limits<uint32_t>::max()) continue;
		std::map<uint16_t, uint32_t> depotItems;
		const ItemDeque& items = chest->getItemList();
		itemWindowCount = std::max(itemWindowCount, (items.size() + maximumSellLootItems - 1) / maximumSellLootItems);
		const size_t scanCount = std::min(items.size(), maximumSellLootItems);
		const size_t scanStart = sellLootItemScanOffset % items.size();
		for (size_t index = 0; index < scanCount; ++index) {
			Item* item = items[(scanStart + index) % items.size()];
			++scannedItems;
			if (item->getContainer() || inventoryPolicy.isProtectedDepositItem(player, *item)) continue;
			depotItems[item->getID()] += item->getItemCount();
		}
		for (Npc* seller : sellers) {
			struct OfferedItem { uint16_t itemId; uint32_t count; const ShopInfo* offer; uint32_t weight; };
			std::vector<OfferedItem> offered;
			for (const auto& [itemId, count] : depotItems) {
				auto offer = std::find_if(seller->getShopOffers().begin(), seller->getShopOffers().end(), [itemId](const ShopInfo& value) {
					return value.itemId == itemId && value.sellPrice != 0;
				});
				if (offer != seller->getShopOffers().end()) offered.push_back({itemId, count, &*offer, Item::items[itemId].weight});
			}
			std::sort(offered.begin(), offered.end(), [](const OfferedItem& left, const OfferedItem& right) {
				const uint64_t leftDensity = left.weight == 0 ? std::numeric_limits<uint64_t>::max() :
					static_cast<uint64_t>(left.offer->sellPrice) * 1000 / left.weight;
				const uint64_t rightDensity = right.weight == 0 ? std::numeric_limits<uint64_t>::max() :
					static_cast<uint64_t>(right.offer->sellPrice) * 1000 / right.weight;
				return leftDensity != rightDensity ? leftDensity > rightDensity : left.offer->sellPrice > right.offer->sellPrice;
			});
			Candidate candidate;
			candidate.sourceDepotId = depotEntry.first;
			candidate.sourceApproach = sourceApproach;
			candidate.provider = seller;
			uint64_t capacity = player.getFreeCapacity();
			for (const OfferedItem& item : offered) {
				const uint32_t capacityCount = item.weight == 0 ? maximumSellLootBatch : static_cast<uint32_t>(capacity / item.weight);
				const uint32_t count = std::min<uint32_t>({item.count, maximumSellLootBatch, capacityCount});
				if (count == 0) continue;
				candidate.batches.push_back({item.itemId, count, item.offer->sellPrice,
				                             static_cast<uint8_t>(item.offer->subType), 0});
				candidate.revenue += static_cast<uint64_t>(item.offer->sellPrice) * count;
				if (item.weight != 0) capacity -= static_cast<uint64_t>(item.weight) * count;
			}
			if (candidate.batches.empty()) continue;
			uint32_t providerDistance = std::numeric_limits<uint32_t>::max();
			for (int32_t xOffset = -1; xOffset <= 1; ++xOffset) for (int32_t yOffset = -1; yOffset <= 1; ++yOffset) {
				if (xOffset == 0 && yOffset == 0) continue;
				const Position approach(seller->getPosition().x + xOffset, seller->getPosition().y + yOffset, seller->getPosition().z);
				Tile* tile = g_game.map.getTile(approach);
				if (!tile || tile->queryAdd(0, player, 1, FLAG_IGNOREBLOCKCREATURE) != RETURNVALUE_NOERROR) continue;
				const uint32_t distance = playerBotNavigationDistance(sourceApproach, approach);
				if (distance < providerDistance) {
					providerDistance = distance;
					candidate.providerApproach = approach;
				}
			}
			if (providerDistance == std::numeric_limits<uint32_t>::max()) continue;
			candidate.roughCost = static_cast<uint64_t>(sourceDistance) + providerDistance;
			candidates.push_back(std::move(candidate));
		}
	}
	std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
		const int64_t leftNet = static_cast<int64_t>(left.revenue) - static_cast<int64_t>(left.roughCost);
		const int64_t rightNet = static_cast<int64_t>(right.revenue) - static_cast<int64_t>(right.roughCost);
		return leftNet != rightNet ? leftNet > rightNet : left.revenue > right.revenue;
	});
	if (sellLootItemScanRemaining == 0) sellLootItemScanRemaining = itemWindowCount;
	if (sellLootRouteScanRemaining == 0) sellLootRouteScanRemaining = candidates.size();
	const size_t routeScanCount = std::min({candidates.size(), sellLootRouteScanRemaining,
	                                       maximumSellLootRouteValidationsPerDecision});
	const size_t routeScanStart = candidates.empty() ? 0 : sellLootRouteScanOffset % candidates.size();
	size_t routeValidations = 0;
	bool found = false;
	uint32_t selectedSourceTravelNpcId = 0;
	Position selectedSourceTravelTarget;
	struct RouteChoice {
		std::optional<std::pair<PlayerBotNavigationRoutePlan, bool>> selected;
		PlayerBotNavigationPlanMetrics walking;
		std::optional<PlayerBotNavigationPlanMetrics> paid;
		bool walkingSafe = false;
		bool paidSafe = false;
	};
	auto chooseRoute = [&](const Position& start, const Position& destination) {
		RouteChoice choice;
		if (start == destination) {
			PlayerBotNavigationRoutePlan route;
			route.metrics.result = PlayerBotNavigationResult::Reached;
			choice.walking = route.metrics;
			choice.walkingSafe = true;
			choice.selected = std::pair{std::move(route), false};
			return choice;
		}
		PlayerBotNavigationRoutePlan walking = planCompleteNavigationRoute(player, start, destination, {}, maximumSellLootRouteNodes);
		choice.walking = walking.metrics;
		choice.walkingSafe = walking.metrics.result == PlayerBotNavigationResult::Reached &&
			playerBotNavigationRiskAccepts(PlayerBotNavigationRiskProfile{}, walking.metrics.dangerCost,
			                               walking.metrics.maximumHealthLossPerSecond);
		auto paid = planNpcTravelRoute(player, start, destination, {}, maximumSellLootRouteNodes, true);
		if (paid) choice.paid = paid->metrics;
		choice.paidSafe = paid && paid->metrics.result == PlayerBotNavigationResult::Reached &&
			playerBotNavigationRiskAccepts(PlayerBotNavigationRiskProfile{}, paid->metrics.dangerCost,
			                               paid->metrics.maximumHealthLossPerSecond);
		if (!choice.walkingSafe && !choice.paidSafe) return choice;
		auto cost = [](const PlayerBotNavigationRoutePlan& route) {
			const uint64_t timeCost = static_cast<uint64_t>(std::ceil(
				route.metrics.estimatedTravelSeconds * sellLootTravelTimeGoldPerMinute / 60.0));
			return route.metrics.fare + timeCost + route.metrics.dangerCost / 10;
		};
		const bool paidSafer = choice.paidSafe && choice.walkingSafe &&
			(paid->metrics.maximumHealthLossPerSecond < walking.metrics.maximumHealthLossPerSecond ||
			 paid->metrics.dangerCost < walking.metrics.dangerCost);
		if (choice.paidSafe && (!choice.walkingSafe || paidSafer || cost(*paid) < cost(walking))) {
			choice.selected = std::pair{std::move(*paid), true};
		} else {
			choice.selected = std::pair{std::move(walking), false};
		}
		return choice;
	};
	for (size_t index = 0; index < routeScanCount; ++index) {
		const Candidate& candidate = candidates[(routeScanStart + index) % candidates.size()];
		++routeValidations;
		auto reject = [&](const char* reason, const RouteChoice* routeChoice = nullptr,
		                  uint64_t fare = 0, uint64_t tripCost = 0, uint32_t danger = 0) {
			std::string routeDetails;
			if (routeChoice) {
				routeDetails = ",\"walking_result\":" + std::to_string(static_cast<uint8_t>(routeChoice->walking.result)) +
				               ",\"walking_safe\":" + (routeChoice->walkingSafe ? "true" : "false") +
				               ",\"walking_danger_cost\":" + std::to_string(routeChoice->walking.dangerCost) +
				               ",\"walking_maximum_health_loss_per_second\":" +
				                   std::to_string(routeChoice->walking.maximumHealthLossPerSecond) +
				               ",\"paid_available\":" + (routeChoice->paid ? "true" : "false") +
				               ",\"paid_safe\":" + (routeChoice->paidSafe ? "true" : "false");
			if (routeChoice->paid) {
				routeDetails += ",\"paid_fare\":" + std::to_string(routeChoice->paid->fare) +
				                ",\"paid_danger_cost\":" + std::to_string(routeChoice->paid->dangerCost) +
				                ",\"paid_maximum_health_loss_per_second\":" +
				                    std::to_string(routeChoice->paid->maximumHealthLossPerSecond);
			}
			}
			emit("sell_loot_candidate", position,
			     "\"result\":\"rejected\",\"reason\":" + jsonString(reason) +
			     ",\"source_depot_id\":" + std::to_string(candidate.sourceDepotId) +
			     ",\"npc_id\":" + std::to_string(candidate.provider->getID()) +
			     ",\"expected_revenue\":" + std::to_string(candidate.revenue) +
			     ",\"fare\":" + std::to_string(fare) + ",\"trip_cost\":" + std::to_string(tripCost) +
			     ",\"danger_cost\":" + std::to_string(danger) + routeDetails);
		};
		RouteChoice sourceChoice;
		if (candidate.sourceDepotId == currentDepotId) {
			PlayerBotNavigationRoutePlan route;
			route.metrics.result = PlayerBotNavigationResult::Reached;
			sourceChoice.walking = route.metrics;
			sourceChoice.walkingSafe = true;
			sourceChoice.selected = std::pair{std::move(route), false};
		} else {
			sourceChoice = chooseRoute(position, candidate.sourceApproach);
		}
		if (!sourceChoice.selected) {
			reject("source_route_unavailable", &sourceChoice);
			continue;
		}
		RouteChoice sellerChoice = chooseRoute(candidate.sourceApproach, candidate.providerApproach);
		if (!sellerChoice.selected) {
			reject("seller_route_unavailable", &sellerChoice);
			continue;
		}
		auto& sourceRoute = *sourceChoice.selected;
		auto& sellerRoute = *sellerChoice.selected;
		const uint64_t fare = sourceRoute.first.metrics.fare + sellerRoute.first.metrics.fare;
		if (fare > player.getMoney() + player.getBankBalance()) {
			reject("upfront_fare_unavailable", nullptr, fare);
			continue;
		}
		const double travelSeconds = sourceRoute.first.metrics.estimatedTravelSeconds +
		                             sellerRoute.first.metrics.estimatedTravelSeconds;
		const uint64_t timeCost = static_cast<uint64_t>(std::ceil(
			travelSeconds * sellLootTravelTimeGoldPerMinute / 60.0));
		const uint32_t danger = static_cast<uint32_t>(std::min<uint64_t>(
			static_cast<uint64_t>(sourceRoute.first.metrics.dangerCost) + sellerRoute.first.metrics.dangerCost,
			std::numeric_limits<uint32_t>::max()));
		if (!playerBotNavigationRiskAccepts(PlayerBotNavigationRiskProfile{}, danger,
		    std::max(sourceRoute.first.metrics.maximumHealthLossPerSecond,
		             sellerRoute.first.metrics.maximumHealthLossPerSecond))) {
			reject("combined_route_unsafe", nullptr, fare, 0, danger);
			continue;
		}
		const uint64_t tripCost = fare + timeCost + danger / 10;
		if (candidate.revenue <= tripCost) {
			reject("non_positive_utility", nullptr, fare, tripCost, danger);
			continue;
		}
		SellLootPlan plan;
		plan.sourceDepotId = candidate.sourceDepotId;
		plan.providerId = candidate.provider->getID();
		plan.batches = candidate.batches;
		plan.sourceApproach = candidate.sourceApproach;
		plan.routeSteps = static_cast<uint32_t>(std::min<uint64_t>(
			static_cast<uint64_t>(sourceRoute.first.metrics.steps) + sellerRoute.first.metrics.steps,
			std::numeric_limits<uint32_t>::max()));
		plan.routeDanger = danger;
		plan.expectedRevenue = candidate.revenue;
		plan.fare = fare;
		plan.roundTripRisk = danger;
		plan.roundTripTime = static_cast<uint32_t>(std::min<uint64_t>(timeCost, std::numeric_limits<uint32_t>::max()));
		plan.utility = static_cast<int64_t>(candidate.revenue - tripCost);
		plan.scannedItems = scannedItems;
		plan.routeValidations = static_cast<uint32_t>(routeValidations);
		plan.sourceAllowNpcTravel = sourceRoute.second;
		plan.sourceFare = sourceRoute.first.metrics.fare;
		plan.sellerAllowNpcTravel = sellerRoute.second;
		const auto sourceTravel = std::find_if(sourceRoute.first.steps.begin(), sourceRoute.first.steps.end(),
		    [](const PlayerBotNavigationStep& step) { return step.action == PlayerBotNavigationAction::NpcTravel; });
		if (sourceTravel != sourceRoute.first.steps.end()) {
			selectedSourceTravelNpcId = sourceTravel->npcId;
			selectedSourceTravelTarget = sourceTravel->target;
		}
		sellLootPlan = std::move(plan);
		found = true;
		break;
	}
	auto completeItemWindow = [&]() {
		if (sellLootItemScanRemaining != 0) --sellLootItemScanRemaining;
		if (sellLootItemScanRemaining != 0) {
			sellLootItemScanOffset += maximumSellLootItems;
			sellLootSearchPending = true;
		} else {
			sellLootItemScanOffset = 0;
			sellLootSearchPending = false;
		}
	};
	if (found) {
		sellLootRouteScanOffset = 0;
		sellLootRouteScanRemaining = 0;
		sellLootSearchPending = false;
		sellLootItemScanOffset = 0;
		sellLootItemScanRemaining = 0;
	} else if (candidates.empty()) {
		sellLootRouteScanOffset = 0;
		sellLootRouteScanRemaining = 0;
		completeItemWindow();
	} else {
		sellLootRouteScanOffset = (routeScanStart + routeValidations) % candidates.size();
		sellLootRouteScanRemaining -= std::min(sellLootRouteScanRemaining, routeValidations);
		sellLootSearchPending = sellLootRouteScanRemaining != 0;
		if (!sellLootSearchPending) {
			sellLootRouteScanOffset = 0;
			completeItemWindow();
		}
	}
	std::ostringstream fields;
	fields << "\"action\":\"sell_loot_plan\",\"result\":" << jsonString(found ? "candidate" : "deferred")
	       << ",\"reason\":" << jsonString(found ? "profitable_trip_validated" :
	           sellLootSearchPending ? "candidate_scan_pending" : "no_profitable_trip")
	       << ",\"top_level_items\":" << scannedItems << ",\"top_level_item_budget\":" << maximumSellLootItems
	       << ",\"route_validations\":" << routeValidations
	       << ",\"route_validation_budget\":" << maximumSellLootRouteValidationsPerDecision;
	if (found) {
		const SellLootPlan& plan = *sellLootPlan;
		fields << ",\"source_depot_id\":" << plan.sourceDepotId << ",\"npc_id\":" << plan.providerId
		       << ",\"manifest_batches\":" << plan.batches.size() << ",\"item_batch_budget\":" << maximumSellLootBatch
		       << ",\"route_steps\":" << plan.routeSteps << ",\"expected_revenue\":" << plan.expectedRevenue
		       << ",\"liquidity_urgency\":0,\"fare\":" << plan.fare << ",\"round_trip_risk\":" << plan.roundTripRisk
		       << ",\"round_trip_time_cost\":" << plan.roundTripTime << ",\"foregone_hunt_profit\":0,\"utility\":" << plan.utility
		       << ",\"source_npc_travel\":" << (plan.sourceAllowNpcTravel ? "true" : "false")
		       << ",\"seller_npc_travel\":" << (plan.sellerAllowNpcTravel ? "true" : "false");
		if (selectedSourceTravelNpcId != 0) {
			fields << ",\"source_travel_npc_id\":" << selectedSourceTravelNpcId
			       << ",\"source_travel_npc_position\":{\"x\":" << selectedSourceTravelTarget.x
			       << ",\"y\":" << selectedSourceTravelTarget.y << ",\"z\":"
			       << static_cast<uint32_t>(selectedSourceTravelTarget.z) << '}';
		}
	}
	emit("sell_loot_plan", position, fields.str());
	return found;
}

void PlayerBotController::deferSellLoot(Player& player, const Position& position, const char* reason)
{
	if (!sellLootPlan) return;
	emit("sell_loot_defer", position, "\"reason\":" + jsonString(reason) + ",\"source_depot_id\":" +
		std::to_string(sellLootPlan->sourceDepotId) + ",\"manifest_batches\":" + std::to_string(sellLootPlan->batches.size()) +
		",\"npc_id\":" + std::to_string(sellLootPlan->providerId) + ",\"cooldown_ms\":" +
		std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(sellLootFailureCooldown).count()));
	progressionRuntime.completeSellLoot(sellLootFailureCooldown);
	serviceWorkflow.reset();
	sellLootPlan.reset();
	beginReturn(&player, position, reason);
}

bool PlayerBotController::processSellLootWithdrawal(Player& player, const Position& position)
{
	if (!sellLootPlan) return false;
	SellLootPlan& plan = *sellLootPlan;
	Container* chest = player.getContainerByID(depotChestContainerId);
	Item* backpackItem = player.getInventoryItem(CONST_SLOT_BACKPACK);
	Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
	if (!chest || !backpack || player.getDepotChest(plan.sourceDepotId, false) != chest) {
		deferSellLoot(player, position, "depot_or_backpack_unavailable");
		return true;
	}
	if (plan.withdrawalPending) {
		if (plan.withdrawalBatch >= plan.batches.size()) {
			deferSellLoot(player, position, "withdraw_manifest_invalid");
			return true;
		}
		SellLootBatch& batch = plan.batches[plan.withdrawalBatch];
		const uint32_t inventory = inventoryPolicy.inventoryItemCount(player, batch.itemId);
		const uint32_t depot = chest->getItemTypeCount(batch.itemId);
		if (inventory != plan.withdrawalInventoryBefore + plan.withdrawalRequested ||
			depot + plan.withdrawalRequested != plan.withdrawalDepotBefore) {
			deferSellLoot(player, position, "withdraw_delta_mismatch");
			return true;
		}
		batch.withdrawn += plan.withdrawalRequested;
		plan.withdrawalPending = false;
		emit("sell_loot_withdraw", position, "\"result\":\"success\",\"item_id\":" + std::to_string(batch.itemId) +
			",\"count\":" + std::to_string(plan.withdrawalRequested) + ",\"withdrawn\":" + std::to_string(batch.withdrawn) +
			",\"inventory_before\":" + std::to_string(plan.withdrawalInventoryBefore) + ",\"inventory_after\":" +
			std::to_string(inventory) + ",\"depot_before\":" + std::to_string(plan.withdrawalDepotBefore) +
			",\"depot_after\":" + std::to_string(depot));
	}
	while (plan.withdrawalBatch < plan.batches.size() &&
	       plan.batches[plan.withdrawalBatch].withdrawn >= plan.batches[plan.withdrawalBatch].count) {
		++plan.withdrawalBatch;
	}
	if (plan.withdrawalBatch >= plan.batches.size()) {
		std::vector<PlayerBotServiceLiquidationBatch> batches;
		for (const SellLootBatch& batch : plan.batches) {
			batches.push_back({batch.itemId, batch.count, batch.price, batch.subType});
		}
		serviceWorkflow.reset(PlayerBotServiceIntent::ResupplyWithLocalSale);
		serviceWorkflow.setLiquidationPlan({plan.providerId, std::move(batches), 0, 0,
		                                    plan.fare - plan.sourceFare, plan.sellerAllowNpcTravel});
		if (progressionRuntime.activeGoal() != TopLevelGoal::Service) {
			const TopLevelGoal previous = progressionRuntime.activeGoal();
			const PlayerBotGoalArbiter::GoalDecision decision = progressionRuntime.interruptHuntForService("sell_trip");
			emit("goal_selection", position,
			     "\"decision_id\":" + std::to_string(decision.id) + ",\"decision_reason\":\"sell_trip\",\"from_goal\":" +
			         jsonString(PlayerBotGoalArbiter::goalName(previous)) + ",\"to_goal\":\"service\",\"utility\":" +
			         std::to_string(decision.candidate(TopLevelGoal::Service).utility) +
			         ",\"reason\":\"sell_trip\",\"forced\":true");
		}
		progressionRuntime.enterService();
		player.closeContainer(depotChestContainerId);
		player.closeContainer(depotLockerContainerId);
		setCyclePhase(CyclePhase::Service, position, "sell_loot_withdrawn");
		schedule(SCHEDULER_MINTICKS);
		return true;
	}
	SellLootBatch& batch = plan.batches[plan.withdrawalBatch];
	if (!player.canDoAction() || !openContainer(player, *backpack, depotSourceContainerId, position)) return true;
	Item* source = nullptr;
	for (Item* item : chest->getItemList()) {
		if (item->getID() == batch.itemId) { source = item; break; }
	}
	if (!source) {
		deferSellLoot(player, position, "planned_depot_item_missing");
		return true;
	}
	const int32_t sourceIndex = chest->getThingIndex(source);
	const int8_t backpackId = player.getContainerID(backpack);
	if (sourceIndex < 0 || sourceIndex > UINT8_MAX || backpackId < 0) {
		deferSellLoot(player, position, "withdraw_source_unavailable");
		return true;
	}
	const uint8_t count = static_cast<uint8_t>(std::min<uint32_t>({batch.count - batch.withdrawn, source->getItemCount(), UINT8_MAX}));
	plan.withdrawalInventoryBefore = inventoryPolicy.inventoryItemCount(player, batch.itemId);
	plan.withdrawalDepotBefore = chest->getItemTypeCount(batch.itemId);
	plan.withdrawalRequested = count;
	plan.withdrawalPending = true;
	telemetry.recordActionAttempt();
	g_game.playerMoveItem(&player, Position(0xFFFF, 0x40 | depotChestContainerId, static_cast<uint8_t>(sourceIndex)),
		source->getClientID(), static_cast<uint8_t>(sourceIndex),
		Position(0xFFFF, 0x40 | static_cast<uint8_t>(backpackId), containerDestinationIndex(*backpack, *source)), count, source, backpack);
	emit("sell_loot_withdraw", position, "\"result\":\"requested\",\"item_id\":" + std::to_string(batch.itemId) +
		",\"count\":" + std::to_string(count) + ",\"inventory_before\":" + std::to_string(plan.withdrawalInventoryBefore) +
		",\"depot_before\":" + std::to_string(plan.withdrawalDepotBefore));
	schedule(navigationDecisionDelay(player));
	return true;
}

const char* PlayerBotController::cyclePhaseName() const
{
	return PlayerBotTurnRouter::cyclePhaseName(turnRouter.cyclePhase());
}

void PlayerBotController::setCyclePhase(CyclePhase phase, const Position& position, const char* reason)
{
	if (turnRouter.cyclePhase() == phase) {
		return;
	}
	const char* previous = cyclePhaseName();
	if (turnRouter.cyclePhase() == CyclePhase::Hunt && phase != CyclePhase::Hunt) {
		huntCoordinator.cancelPlanning();
	}
	turnRouter.setCyclePhase(phase);
	std::ostringstream fields;
	fields << "\"from\":" << jsonString(previous) << ",\"to\":" << jsonString(cyclePhaseName())
	       << ",\"reason\":" << jsonString(reason);
	emit("objective_transition", position, fields.str());
	if (Player* player = g_game.getPlayerByID(playerId)) {
		say(*player, std::string("Objective: ") + cyclePhaseName() + " (" + reason + ").");
	}
}

void PlayerBotController::beginReturn(Player* player, const Position& position, const char* reason)
{
	const auto traversalTarget = huntCoordinator.traversalTarget();
	const uint32_t previousTarget = traversalTarget ? traversalTarget->id : 0;
	g_game.playerCancelAttackAndFollow(playerId);
	clearTraversalTarget(position, reason);
	resetNavigation();
	huntCoordinator.resetLoot();
	depotWorkflow.reset();
	player->closeContainer(corpseContainerId);
	setStage(ScenarioStage::Traverse, position);
	setCyclePhase(CyclePhase::ReturnToDepot, position, reason);
	std::ostringstream fields;
	fields << "\"action\":\"return\",\"result\":\"started\",\"reason\":" << jsonString(reason)
	       << ",\"previous_target_id\":" << (previousTarget == 0 ? "null" : std::to_string(previousTarget));
	emit("action_result", position, fields.str());
	schedule(navigationInterval);
}

void PlayerBotController::onNpcReply(uint32_t replyingPlayerId, uint32_t npcId, uint8_t type, const std::string& text)
{
	const bool serviceAccepted = serviceWorkflow.reportNpcReply(playerId, replyingPlayerId, npcId, type);
	const bool progressionAccepted = progressionRuntime.reportNpcReply(playerId, replyingPlayerId, npcId, type);
	if (!serviceAccepted && !progressionAccepted) {
		return;
	}
	Npc* npc = g_game.getNpcByID(npcId);
	emit("npc_reply", lastPosition, "\"npc_id\":" + std::to_string(npcId) +
	     ",\"npc_name\":" + jsonString(npc ? npc->getName() : "") + ",\"text\":" + jsonString(text));
}

void PlayerBotController::beginService(Player* player, const Position& position, const char* reason)
{
	const bool interruptedHunt = fixtureDriver.progressionGoalLoop(true).selectGoal && progressionRuntime.activeGoal() == TopLevelGoal::Hunt &&
	                             !departurePlanner.hasCompleted(departureSnapshot(*player));
	finishHuntRegion(*player, position, reason);
	if (interruptedHunt) {
		emit("goal_result", position,
		     "\"decision_id\":" + std::to_string(progressionRuntime.decisionId()) +
		         ",\"goal\":\"hunt\",\"result\":\"interrupted\",\"reason\":" + jsonString(reason));
		const PlayerBotGoalArbiter::GoalDecision decision = progressionRuntime.interruptHuntForService("forced_interrupt");
		emit("goal_selection", position,
		     "\"decision_id\":" + std::to_string(decision.id) + ",\"decision_reason\":" + jsonString(reason) +
		         ",\"from_goal\":\"hunt\",\"to_goal\":\"service\",\"utility\":" +
		         std::to_string(decision.candidate(TopLevelGoal::Service).utility) + ',' +
		         "\"reason\":\"forced_interrupt\",\"forced\":true");
	}
	progressionRuntime.enterService();
	g_game.playerCancelAttackAndFollow(playerId);
	clearTraversalTarget(position, reason);
	resetNavigation();
	huntCoordinator.resetLoot();
	player->closeContainer(corpseContainerId);
	setStage(ScenarioStage::Traverse, position);
	serviceWorkflow.reset();
	beginReturn(player, position, reason);
}

void PlayerBotController::finishHuntAndReturn(Player* player, const Position& position, const char* reason)
{
	finishHuntRegion(*player, position, reason);
	emit("goal_result", position,
	     "\"decision_id\":" + std::to_string(progressionRuntime.decisionId()) +
	         ",\"goal\":\"hunt\",\"result\":\"success\",\"reason\":" + jsonString(reason));
	beginReturn(player, position, reason);
}

void PlayerBotController::refreshItemValues()
{
	std::vector<PlayerBotEconomyProvider> providers;
	for (Npc* npc : playerBotNpcProviders(g_game.getNpcs(), PlayerBotNpcCapability::Shop, Position())) {
		PlayerBotEconomyProvider provider{npc->getID(), npc->getPosition()};
		for (const ShopInfo& offer : npc->getShopOffers()) {
			const ItemType& type = Item::items[offer.itemId];
			if (type.isFluidContainer() || type.isSplash()) continue;
			if (offer.buyPrice != 0 || offer.sellPrice != 0) provider.offers.push_back(
			    {offer.itemId, offer.buyPrice, offer.sellPrice, static_cast<uint8_t>(offer.subType)});
		}
		providers.push_back(std::move(provider));
	}
	economyCatalog.learn(providers);
}

const ShopInfo* PlayerBotController::findOffer(const ServiceNpc& service, uint16_t itemId, bool buying) const
{
	if (!fixtureDriver.observeProvider(true, itemId, buying).available) {
		return nullptr;
	}
	Npc* npc = g_game.getNpcByID(service.id);
	if (!npc || !playerBotNpcHasCapability(*npc, PlayerBotNpcCapability::Shop)) {
		return nullptr;
	}
	const std::vector<ShopInfo>& offers = npc->getShopOffers();
	auto it = std::find_if(offers.begin(), offers.end(), [itemId, buying](const ShopInfo& offer) {
		return offer.itemId == itemId && (buying ? offer.buyPrice != 0 : offer.sellPrice != 0);
	});
	return it == offers.end() ? nullptr : &*it;
}

uint32_t PlayerBotController::serviceDistance(const Position& from, const ServiceNpc& service) const
{
	return std::max(Position::getDistanceX(from, service.position), Position::getDistanceY(from, service.position)) +
	       (from.z == service.position.z ? 0 : 32 * Position::getDistanceZ(from, service.position));
}

void PlayerBotController::processService(Player* player, const Position& currentPosition)
{
	const PlayerBotServiceSnapshot service = serviceWorkflow.snapshot();
	const bool localSaleService = serviceWorkflow.intent() == PlayerBotServiceIntent::ResupplyWithLocalSale;
	const bool sellingLocalLoot = localSaleService && serviceWorkflow.liquidation().has_value() &&
	                              (service.stage == PlayerBotServiceStage::Discover ||
	                               service.stage == PlayerBotServiceStage::SellLoot);
	if (service.npcId != 0) {
		Npc* npc = g_game.getNpcByID(service.npcId);
		const PlayerBotNpcCapability capability = service.stage == PlayerBotServiceStage::Bank ?
			PlayerBotNpcCapability::Banker : PlayerBotNpcCapability::Shop;
		if (!npc || npc->isRemoved() || !playerBotNpcHasCapability(*npc, capability)) {
			if (sellingLocalLoot) {
				deferSellLoot(*player, currentPosition, "provider_unavailable");
				return;
			}
			stop("service_provider_unavailable", currentPosition);
			return;
		}
	}

	PlayerBotServiceObservation observation;
	observation.currentPosition = currentPosition;
	observation.freeCapacity = player->getFreeCapacity();
	observation.money = player->getMoney();
	observation.bankBalance = player->getBankBalance();
	observation.goldCoinWeight = Item::items[ITEM_GOLD_COIN].weight;
	observation.healthPotionItemId = recoveryPotionItemId(player->getVocationId());
	observation.healthPotionWeight = Item::items[observation.healthPotionItemId].weight;
	observation.healthPotionReturnThreshold = huntPotionReturnThreshold;
	observation.healthPotionRestockTarget = huntPotionRestockTarget;
	Item* serviceBackpackItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
	Container* serviceBackpack = serviceBackpackItem ? serviceBackpackItem->getContainer() : nullptr;
	observation.actionAvailable = player->canDoAction();
	observation.backpackAvailable = serviceBackpack != nullptr;
	observation.backpackOpen = serviceBackpack && player->getContainerID(serviceBackpack) >= 0;
	observation.maximumAttempts = maximumServiceAttempts;
	observation.slottedSaleCooldownMs = static_cast<uint32_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(unavailableDispositionCooldown).count());
	std::set<uint16_t> relevantItemIds{observation.healthPotionItemId};
	if (sellingLocalLoot) {
		for (const PlayerBotServiceLiquidationBatch& batch : serviceWorkflow.liquidation()->batches) {
			observation.inventoryCounts.emplace(batch.itemId, inventoryPolicy.inventoryItemCount(*player, batch.itemId));
			observation.backpackSaleCounts.emplace(batch.itemId, inventoryPolicy.backpackSaleItemCount(*player, batch.itemId));
			relevantItemIds.insert(batch.itemId);
		}
	}
	observation.inventoryCounts.emplace(observation.healthPotionItemId,
		inventoryPolicy.inventoryItemCount(*player, observation.healthPotionItemId));
	observation.backpackSaleCounts.emplace(observation.healthPotionItemId,
		inventoryPolicy.backpackSaleItemCount(*player, observation.healthPotionItemId));
	std::vector<Npc*> serviceNpcs = playerBotNpcProviders(g_game.getNpcs(), PlayerBotNpcCapability::Shop, currentPosition);
	for (const auto& entry : g_game.getNpcs()) {
		Npc* npc = entry.second;
		if (npc && playerBotNpcHasCapability(*npc, PlayerBotNpcCapability::Banker) &&
		    std::find(serviceNpcs.begin(), serviceNpcs.end(), npc) == serviceNpcs.end()) serviceNpcs.push_back(npc);
	}
	std::sort(serviceNpcs.begin(), serviceNpcs.end(), [&currentPosition](const Npc* left, const Npc* right) {
		const uint32_t leftDistance = playerBotNpcDistance(currentPosition, left->getPosition());
		const uint32_t rightDistance = playerBotNpcDistance(currentPosition, right->getPosition());
		return leftDistance != rightDistance ? leftDistance < rightDistance : left->getID() < right->getID();
	});
	const uint32_t approachProviderId = serviceWorkflow.snapshot().npcId;
	const std::set<uint32_t>& unavailableProviderIds = serviceWorkflow.unavailableProviders();
	const PlayerBotTopology& topology = PlayerBotTopology::instance();
	const bool canUseRope = g_game.findItemOfType(player, playerbot::ropeItemId, true) != nullptr;
	const bool canUseShovel = g_game.findItemOfType(player, 2554, true) != nullptr;
	if (!serviceTopologyDistances || !topology.sameWalkNode(serviceTopologyOrigin, currentPosition) ||
	    serviceTopologyCanUseRope != canUseRope || serviceTopologyCanUseShovel != canUseShovel ||
	    serviceTopologyLevel != player->getLevel() || serviceTopologyGeneration != topology.generation()) {
		serviceTopologyDistances = topology.distancesFrom(currentPosition, canUseRope, canUseShovel, player->getLevel());
		serviceTopologyOrigin = currentPosition;
		serviceTopologyCanUseRope = canUseRope;
		serviceTopologyCanUseShovel = canUseShovel;
		serviceTopologyLevel = player->getLevel();
		serviceTopologyGeneration = topology.generation();
	}
	const PlayerBotTopologyDistances& coarseDistances = *serviceTopologyDistances;
	std::stable_sort(serviceNpcs.begin(), serviceNpcs.end(), [&topology, &coarseDistances](const Npc* left, const Npc* right) {
		const std::optional<uint32_t> leftCost = topology.distanceTo(
		    coarseDistances, PlayerBotNavigationGoal::withinRange(left->getPosition(), 3, 3));
		const std::optional<uint32_t> rightCost = topology.distanceTo(
		    coarseDistances, PlayerBotNavigationGoal::withinRange(right->getPosition(), 3, 3));
		if (leftCost.has_value() != rightCost.has_value()) return leftCost.has_value();
		return leftCost && rightCost && *leftCost != *rightCost && *leftCost < *rightCost;
	});
	std::map<uint16_t, size_t> retainedProviders;
	size_t retainedBankers = 0;
	std::vector<Npc*> matchingNpcs;
	for (Npc* npc : serviceNpcs) {
		if (sellingLocalLoot && npc->getID() != serviceWorkflow.liquidation()->providerId) continue;
		if (unavailableProviderIds.find(npc->getID()) != unavailableProviderIds.end()) continue;
		const bool active = npc->getID() == approachProviderId || Position::areInRange<3, 3, 0>(currentPosition, npc->getPosition());
		const bool banker = playerBotNpcHasCapability(*npc, PlayerBotNpcCapability::Banker);
		bool retain = active || (banker && retainedBankers < maximumServiceBankers);
		std::set<uint16_t> matchedItems;
		for (const ShopInfo& offer : npc->getShopOffers()) {
			const bool neededPurchase = offer.itemId == observation.healthPotionItemId && offer.buyPrice != 0;
			const auto inventoryCount = observation.inventoryCounts.find(offer.itemId);
			const auto backpackSaleCount = observation.backpackSaleCounts.find(offer.itemId);
			const bool neededSale = sellingLocalLoot && offer.sellPrice != 0 &&
			                        ((inventoryCount != observation.inventoryCounts.end() && inventoryCount->second != 0) ||
			                         (backpackSaleCount != observation.backpackSaleCounts.end() && backpackSaleCount->second != 0));
			if ((neededPurchase || neededSale) && relevantItemIds.find(offer.itemId) != relevantItemIds.end() &&
			    retainedProviders[offer.itemId] < maximumServiceProvidersPerItem) {
				retain = true;
				matchedItems.insert(offer.itemId);
			}
		}
		if (!retain) continue;
		matchingNpcs.push_back(npc);
		if (banker && retainedBankers < maximumServiceBankers) ++retainedBankers;
		for (uint16_t itemId : matchedItems) ++retainedProviders[itemId];
	}
	serviceNpcs = std::move(matchingNpcs);
	for (Npc* npc : serviceNpcs) {
		const bool shop = playerBotNpcHasCapability(*npc, PlayerBotNpcCapability::Shop);
		const bool banker = playerBotNpcHasCapability(*npc, PlayerBotNpcCapability::Banker);
		PlayerBotEconomyProvider provider{npc->getID(), npc->getPosition()};
		for (const ShopInfo& offer : npc->getShopOffers()) {
			if (relevantItemIds.find(offer.itemId) == relevantItemIds.end()) continue;
			const ItemType& type = Item::items[offer.itemId];
			if (type.isFluidContainer() || type.isSplash()) continue;
			const bool buyAvailable = offer.buyPrice != 0 && fixtureDriver.observeProvider(true, offer.itemId, true).available;
			const bool sellAvailable = offer.sellPrice != 0 && fixtureDriver.observeProvider(true, offer.itemId, false).available;
			if (!buyAvailable && !sellAvailable) continue;
			provider.offers.push_back({offer.itemId, buyAvailable ? offer.buyPrice : 0,
			                           sellAvailable ? offer.sellPrice : 0, static_cast<uint8_t>(offer.subType)});
		}
		int32_t onBuy;
		int32_t onSell;
		PlayerBotServiceProviderObservation providerObservation{
			true, Position::areInRange<3, 3, 0>(currentPosition, npc->getPosition()),
			player->getShopOwner(onBuy, onSell) == npc && !player->getShopItemList().empty(),
			approachProviderId == npc->getID()};
		if (!providerObservation.inRange && providerObservation.approachesObserved) {
			for (int32_t xOffset = -3; xOffset <= 3; ++xOffset) for (int32_t yOffset = -3; yOffset <= 3; ++yOffset) {
				if (xOffset == 0 && yOffset == 0) continue;
				const Position approach(npc->getPosition().x + xOffset, npc->getPosition().y + yOffset, npc->getPosition().z);
				Tile* tile = g_game.map.getTile(approach);
				if (tile && tile->queryAdd(0, *player, 1, 0) == RETURNVALUE_NOERROR) {
					providerObservation.approaches.push_back({approach, static_cast<uint32_t>(
						std::max(Position::getDistanceX(currentPosition, approach), Position::getDistanceY(currentPosition, approach)))});
				}
			}
			std::sort(providerObservation.approaches.begin(), providerObservation.approaches.end(), [npc](const auto& left,
			                                                                                              const auto& right) {
				const uint32_t leftProviderDistance = std::max(Position::getDistanceX(npc->getPosition(), left.position),
				                                               Position::getDistanceY(npc->getPosition(), left.position));
				const uint32_t rightProviderDistance = std::max(Position::getDistanceX(npc->getPosition(), right.position),
				                                                Position::getDistanceY(npc->getPosition(), right.position));
				if (leftProviderDistance != rightProviderDistance) return leftProviderDistance > rightProviderDistance;
				return left.distance != right.distance ? left.distance < right.distance : left.position < right.position;
			});
			std::array<bool, 9> selectedDirections{};
			std::vector<PlayerBotServiceProviderObservation::Approach> diverseApproaches;
			for (const auto& approach : providerObservation.approaches) {
				const int32_t direction = approachDirection(npc->getPosition(), approach.position);
				if (selectedDirections[direction]) continue;
				selectedDirections[direction] = true;
				diverseApproaches.push_back(approach);
				if (diverseApproaches.size() >= maximumServiceProviderApproaches) break;
			}
			providerObservation.approaches = std::move(diverseApproaches);
		}
		observation.providers[npc->getID()] = std::move(providerObservation);
		if (shop) {
			observation.discoveries.push_back({npc->getID(), npc->getName(), "shop",
			                                  static_cast<uint32_t>(npc->getShopOffers().size())});
			if (!provider.offers.empty()) observation.shops.push_back(provider);
		}
		if (banker) {
			observation.discoveries.push_back({npc->getID(), npc->getName(), "banker", 0});
			observation.bankers.push_back(std::move(provider));
		}
	}
	observation.now = std::chrono::steady_clock::now();
	if (sellingLocalLoot) for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
		Item* item = player->getInventoryItem(static_cast<slots_t>(slot));
		if (item && inventoryPolicy.isActionableSlottedItem(*player, *item, static_cast<slots_t>(slot), 0)) {
			observation.slottedSaleItems.push_back({item->getID(), static_cast<slots_t>(slot), item->getItemCount()});
		}
	}
	PlayerBotServiceCommand command = serviceWorkflow.advance(observation, economyCatalog, dispositionPolicy);
	const std::vector<PlayerBotServiceDiscovery> discoveries = command.discoveries;
	std::deque<PlayerBotNavigationStep> approachSteps;
	uint32_t routeValidations = 0;
	PlayerBotNavigationResult lastRouteResult = PlayerBotNavigationResult::Unreachable;
	uint64_t lastRouteExpandedNodes = 0;
	uint32_t lastRouteSteps = 0;
	bool lastRouteRequiresNpcTravel = false;
	while (command.type == PlayerBotServiceCommandType::ValidateProviderRoute && routeValidations < 1) {
		++routeValidations;
		PlayerBotServiceObservation routeObservation = observation;
		routeObservation.approachRoute.providerId = command.providerId;
		routeObservation.approachRoute.destination = command.destination;
		const auto startedAt = std::chrono::steady_clock::now();
		PlayerBotNavigationRoutePlan routePlan;
		if (command.destination != currentPosition) {
			if (sellingLocalLoot && !serviceWorkflow.liquidation()->allowNpcTravel) {
				routePlan = planCompleteNavigationRoute(*player, command.destination);
			} else {
				routePlan = planNavigationRoute(*player, command.destination);
			}
		}
		const PlayerBotNavigationRiskProfile risk;
		const bool routeSafe = command.destination == currentPosition || playerBotNavigationRiskAccepts(
		    risk, routePlan.metrics.dangerCost, routePlan.metrics.maximumHealthLossPerSecond);
		lastRouteResult = routeSafe ? routePlan.metrics.result : PlayerBotNavigationResult::Unreachable;
		lastRouteExpandedNodes = routePlan.metrics.expandedNodes;
		lastRouteSteps = routePlan.metrics.steps;
		const bool reached = command.destination == currentPosition ||
		                     (routePlan.metrics.result == PlayerBotNavigationResult::Reached && !routePlan.steps.empty() && routeSafe);
		telemetry.recordPathfinding(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - startedAt), reached);
		routeObservation.approachRoute.result = reached ? PlayerBotServiceRouteResult::Reached : PlayerBotServiceRouteResult::Unreachable;
		routeObservation.approachRoute.steps = routePlan.metrics.steps;
		routeObservation.approachRoute.expandedNodes = routePlan.metrics.expandedNodes;
		routeObservation.approachRoute.dangerCost = routePlan.metrics.dangerCost;
		routeObservation.approachRoute.fare = routePlan.metrics.fare;
		routeObservation.approachRoute.maximumDanger = routePlan.metrics.maximumHealthLossPerSecond;
		lastRouteRequiresNpcTravel = std::any_of(routePlan.steps.begin(), routePlan.steps.end(),
			[](const PlayerBotNavigationStep& step) { return step.action == PlayerBotNavigationAction::NpcTravel; });
		routeObservation.approachRoute.requiresNpcTravel = lastRouteRequiresNpcTravel;
		if (reached) approachSteps = std::move(routePlan.steps);
		command = serviceWorkflow.advance(routeObservation, economyCatalog, dispositionPolicy);
		if (reached && command.type == PlayerBotServiceCommandType::Wait &&
		    command.outcome == PlayerBotServiceOutcome::Success) {
			command = serviceWorkflow.advance(routeObservation, economyCatalog, dispositionPolicy);
		}
	}
	if (command.type == PlayerBotServiceCommandType::ValidateProviderRoute) {
		emit("action_result", currentPosition,
		     "\"action\":\"service_discover\",\"result\":\"continuing\",\"reason\":\"route_validation_budget_exhausted\"");
		schedule(blockedRouteRetryInterval);
		return;
	}
	for (const PlayerBotServiceDiscovery& discovery : discoveries) {
		emit("service_discovered", currentPosition, "\"capability\":" + jsonString(discovery.capability) +
		     ",\"npc_id\":" + std::to_string(discovery.npcId) + ",\"npc_name\":" + jsonString(discovery.npcName) +
		     ",\"offers\":" + std::to_string(discovery.offers));
	}
	if (command.verification && command.verification->result == PlayerBotServiceVerificationResult::Success && command.transaction) {
		const PlayerBotServiceTransaction& transaction = *command.transaction;
		if (transaction.itemId != 0) {
			const char* action = sellingLocalLoot ? "sell" : "buy_potions";
			emit("action_result", currentPosition, "\"action\":" + jsonString(action) + ",\"result\":\"success\",\"item_id\":" +
			     std::to_string(transaction.itemId) + ",\"count\":" + std::to_string(transaction.amount) +
			     ",\"carried_before\":" + std::to_string(transaction.money) + ",\"carried_after\":" +
			     std::to_string(observation.money) + ",\"bank_before\":" + std::to_string(transaction.balance) +
			     ",\"bank_after\":" + std::to_string(observation.bankBalance));
			const ItemType& itemType = Item::items[transaction.itemId];
			say(*player, std::string(action) == "sell" ? "Sold " + std::to_string(transaction.amount) + " " +
			    (transaction.amount == 1 ? itemType.name : itemType.getPluralName()) + '.' : "Bought " +
			    std::to_string(transaction.amount) + " " + (transaction.amount == 1 ? itemType.name : itemType.getPluralName()) + '.');
			if (sellingLocalLoot && !serviceWorkflow.liquidation()) sellLootPlan.reset();
		} else if (command.type == PlayerBotServiceCommandType::Complete) {
			emit("action_result", currentPosition, "\"action\":\"bank_withdraw\",\"result\":\"success\",\"count\":" +
			     std::to_string(transaction.amount) + ",\"bank_before\":" + std::to_string(transaction.balance) +
			     ",\"bank_after\":" + std::to_string(observation.bankBalance));
		} else {
			emit("action_result", currentPosition, "\"action\":\"bank_deposit\",\"result\":\"success\",\"count\":" +
			     std::to_string(transaction.money) + ",\"bank_before\":" + std::to_string(transaction.balance) +
			     ",\"bank_after\":" + std::to_string(observation.bankBalance));
			say(*player, "Deposited " + std::to_string(transaction.money) + " gold. Bank: " +
			    std::to_string(observation.bankBalance) + '.');
		}
	}
	if (command.type == PlayerBotServiceCommandType::Fail) {
		if (sellingLocalLoot) {
			deferSellLoot(*player, currentPosition, "provider_or_transaction_invalidated");
			return;
		}
		if (localSaleService && command.outcome == PlayerBotServiceOutcome::InsufficientFunds) {
			serviceWorkflow.reset();
			beginReturn(player, currentPosition, "sell_trip_insufficient_funds");
			return;
		}
		stop(command.outcome == PlayerBotServiceOutcome::InsufficientFunds ? "insufficient_potion_funds" : "required_shop_offer_unavailable", currentPosition);
		return;
	}
	if (command.type == PlayerBotServiceCommandType::Wait && command.itemId != 0 &&
	    command.sourceSlot != CONST_SLOT_WHEREEVER) {
		if (command.outcome == PlayerBotServiceOutcome::Success) {
			emit("action_result", currentPosition,
			     "\"action\":\"item_disposition\",\"result\":\"success\",\"disposition\":\"sell\",\"item_id\":" +
			         std::to_string(command.itemId) + ",\"source_slot\":" + std::to_string(command.sourceSlot) +
			         ",\"provider_available\":" + (command.providerAvailable ? "true" : "false"));
		} else if (command.outcome == PlayerBotServiceOutcome::Unavailable) {
			emit("action_result", currentPosition,
			     "\"action\":\"item_disposition\",\"result\":\"deferred\",\"reason\":\"move_not_verified\",\"disposition\":\"sell\",\"item_id\":" +
			         std::to_string(command.itemId) + ",\"source_slot\":" + std::to_string(command.sourceSlot) +
			         ",\"provider_available\":" + (command.providerAvailable ? "true" : "false") +
			         ",\"cooldown_ms\":" + std::to_string(command.cooldownMs));
		}
	}
	if (command.type == PlayerBotServiceCommandType::Wait && command.outcome == PlayerBotServiceOutcome::Retry &&
	    command.providerId != 0) {
		npcApproach = {};
		Npc* rejected = g_game.getNpcByID(command.providerId);
		emit("service_provider_rejected", currentPosition,
		     "\"reason\":" + jsonString(lastRouteResult == PlayerBotNavigationResult::Reached ?
		         "route_policy_rejected" : "route_unreachable") + ",\"npc_id\":" + std::to_string(command.providerId) +
		         ",\"route_result\":" + jsonString(lastRouteResult == PlayerBotNavigationResult::NodeLimit ? "node_limit" :
		                                                lastRouteResult == PlayerBotNavigationResult::Reached ? "reached" : "unreachable") +
		         ",\"route_steps\":" + std::to_string(lastRouteSteps) +
		         ",\"requires_npc_travel\":" + (lastRouteRequiresNpcTravel ? "true" : "false") +
		         ",\"expanded_nodes\":" + std::to_string(lastRouteExpandedNodes) +
		         ",\"npc_name\":" + jsonString(rejected ? rejected->getName() : "") +
		         ",\"provider_position\":{" +
		         "\"x\":" + std::to_string(rejected ? rejected->getPosition().x : 0) +
		         ",\"y\":" + std::to_string(rejected ? rejected->getPosition().y : 0) +
		         ",\"z\":" + std::to_string(rejected ? static_cast<uint16_t>(rejected->getPosition().z) : 0) + "}");
	}
	Npc* provider = command.providerId == 0 ? nullptr : g_game.getNpcByID(command.providerId);
	if (command.type == PlayerBotServiceCommandType::NavigateProvider && provider) {
		if (!approachSteps.empty()) observeNavigationPlan(command.destination, std::move(approachSteps));
		bool approachUnavailable = false;
		if (processNpcApproach(player, currentPosition, provider, command.destination, approachUnavailable)) {
			schedule(SCHEDULER_MINTICKS);
		} else if (approachUnavailable) {
			if (const std::optional<Position> rejected = serviceWorkflow.rejectSelectedApproach()) {
				emit("service_provider_approach_rejected", currentPosition,
				     "\"reason\":\"route_unavailable\",\"npc_id\":" + std::to_string(command.providerId) +
				         ",\"destination\":{\"x\":" + std::to_string(rejected->x) +
				         ",\"y\":" + std::to_string(rejected->y) + ",\"z\":" +
				         std::to_string(static_cast<uint16_t>(rejected->z)) + "}");
				resetNavigation();
			}
		}
		return;
	}
	if (command.type == PlayerBotServiceCommandType::Speak && provider) {
		telemetry.recordActionAttempt();
		provider->receiveSpeech(player, TALKTYPE_PRIVATE_PN, command.speech);
		schedule(1000);
		return;
	}
	if (command.type == PlayerBotServiceCommandType::OpenBackpack) {
		if (!serviceBackpack) { stop("service_backpack_unavailable", currentPosition); return; }
		if (!player->canDoAction()) { schedule(navigationDecisionDelay(*player)); return; }
		telemetry.recordActionAttempt();
		g_game.playerUseItem(playerId, Position(0xFFFF, CONST_SLOT_BACKPACK, 0), 0, depotSourceContainerId,
		                     serviceBackpack->getClientID());
		schedule(navigationDecisionDelay(*player));
		return;
	}
	if (command.type == PlayerBotServiceCommandType::MoveSlottedSale) {
		Item* item = player->getInventoryItem(command.sourceSlot);
		Item* backpackItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
		Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
		if (!item || !backpack || !player->canDoAction()) { schedule(navigationDecisionDelay(*player)); return; }
		const int8_t backpackId = player->getContainerID(backpack);
		if (backpackId < 0) {
			telemetry.recordActionAttempt();
			g_game.playerUseItem(playerId, Position(0xFFFF, CONST_SLOT_BACKPACK, 0), 0, depotSourceContainerId, backpack->getClientID());
			schedule(navigationDecisionDelay(*player));
			return;
		}
		Position source; uint8_t index = 0;
		g_game.internalGetPosition(item, source, index);
		telemetry.recordActionAttempt();
		g_game.playerMoveItem(player, source, item->getClientID(), index,
		                      Position(0xFFFF, 0x40 | static_cast<uint8_t>(backpackId), containerDestinationIndex(*backpack, *item)),
		                      static_cast<uint8_t>(item->getItemCount()), item, backpack);
		emit("action_result", currentPosition,
		     "\"action\":\"item_disposition\",\"result\":\"requested\",\"disposition\":\"sell\",\"item_id\":" +
		         std::to_string(command.itemId) + ",\"source_slot\":" + std::to_string(command.sourceSlot) +
		         ",\"provider_available\":true,\"attempt\":" + std::to_string(command.attempt));
		schedule(navigationDecisionDelay(*player));
		return;
	}
	if (command.type == PlayerBotServiceCommandType::Sell || command.type == PlayerBotServiceCommandType::Buy) {
		if (!provider || !command.transaction) { stop("shop_offer_unavailable", currentPosition); return; }
		telemetry.recordActionAttempt();
		if (command.type == PlayerBotServiceCommandType::Buy) g_game.playerPurchaseItem(playerId, Item::items[command.itemId].clientId,
		    command.subType, static_cast<uint8_t>(command.amount), false, false);
		else g_game.playerSellItem(playerId, Item::items[command.itemId].clientId, command.subType,
		    static_cast<uint8_t>(command.amount), true);
		schedule(navigationDecisionDelay(*player));
		return;
	}
	if ((command.type == PlayerBotServiceCommandType::DepositAll || command.type == PlayerBotServiceCommandType::Withdraw) && provider) {
		telemetry.recordActionAttempt();
		provider->receiveSpeech(player, TALKTYPE_PRIVATE_PN, command.type == PlayerBotServiceCommandType::DepositAll ? "deposit all" :
		    "withdraw " + std::to_string(command.amount));
		schedule(1000);
		return;
	}
	if (command.type == PlayerBotServiceCommandType::Complete) {
		if (fixtureDriver.progressionGoalLoop(true).selectGoal) {
			emit("goal_result", currentPosition,
			     "\"decision_id\":" + std::to_string(progressionRuntime.decisionId()) +
			         ",\"goal\":\"service\",\"result\":\"success\",\"reason\":\"service_complete\"");
			selectTopLevelGoal(*player, currentPosition, "service_complete");
		} else {
			startHunt(player, currentPosition, "service_complete");
		}
		schedule(SCHEDULER_MINTICKS);
	} else {
		schedule(command.outcome == PlayerBotServiceOutcome::Retry && command.cooldownMs != 0 ?
		         command.cooldownMs : SCHEDULER_MINTICKS);
	}
}

bool PlayerBotController::findDepositableItem(const Player& player, Container* container, Container*& source,
                                              Item*& depositItem, uint8_t& count) const
{
	for (Item* item : container->getItemList()) {
		if (Container* nested = item->getContainer()) {
			if (findDepositableItem(player, nested, source, depositItem, count)) {
				return true;
			}
			if (nested->empty() && item->getID() == ITEM_BAG) {
				source = container;
				depositItem = item;
				count = 1;
				return true;
			}
		}
		if (inventoryPolicy.isProtectedDepositItem(player, *item)) {
			continue;
		}
		const uint32_t carried = inventoryPolicy.inventoryItemCount(player, item->getID());
		const uint32_t reserve = item->getID() == recoveryPotionItemId(player.getVocationId()) ?
			std::max(inventoryPolicy.protectedItemReserve(player, item->getID()), huntPotionRestockTarget) :
			inventoryPolicy.protectedItemReserve(player, item->getID());
		const uint32_t movable = item->isStackable() && carried > reserve ?
			std::min<uint32_t>(item->getItemCount(), carried - reserve) : (carried > reserve ? 1 : 0);
		if (movable != 0 && movable <= UINT8_MAX) {
			source = container;
			depositItem = item;
			count = static_cast<uint8_t>(movable);
			return true;
		}
	}
	return false;
}

bool PlayerBotController::findDepotLocker(const Position& position, uint16_t expectedDepotId, uint16_t& lockerItemId) const
{
	Tile* tile = g_game.map.getTile(position);
	TileItemVector* items = tile ? tile->getItemList() : nullptr;
	if (!items) {
		return false;
	}
	for (Item* item : *items) {
		Container* container = item->getContainer();
		if (container && container->getDepotLocker() && container->getDepotLocker()->getDepotId() == expectedDepotId) {
			lockerItemId = item->getID();
			return true;
		}
	}
	return false;
}

bool PlayerBotController::discoverDepot(Player& player, const Position& currentPosition)
{
	const auto now = std::chrono::steady_clock::now();
	const PlayerBotFixtureDepotEndpoint fixtureDepot = fixtureDriver.depotEndpoint();
	auto advance = [&](PlayerBotDepotObservation observation) {
		observation.currentPosition = currentPosition;
		observation.now = now;
		observation.fixtureSynthetic = fixtureDepot.synthetic;
		return depotWorkflow.advance(observation, depotRouteValidationsPerDecision, maximumDepotDiscoveryAttempts,
		                             depotApproachSuppression);
	};
	auto scan = [&] {
		PlayerBotDepotScan result;
		result.observed = true;
		if (fixtureDepot.synthetic) {
			result.indexedCandidates = result.inScopeCandidates = result.standableCandidates = 1;
			result.candidates.push_back({fixtureDepot.depotId, fixtureDepot.lockerItemId, fixtureDepot.lockerPosition,
			                             fixtureDepot.approachPosition, 0});
			return result;
		}
		const PlayerBotTopology& topology = PlayerBotTopology::instance();
		const bool canUseRope = g_game.findItemOfType(&player, playerbot::ropeItemId, true) != nullptr;
		const bool canUseShovel = g_game.findItemOfType(&player, 2554, true) != nullptr;
		const PlayerBotTopologyDistances distances = topology.distancesFrom(
			currentPosition, canUseRope, canUseShovel, player.getLevel());
		for (const auto& entry : g_game.map.getDepotLockerPositions()) {
			if (sellLootPlan && entry.first != sellLootPlan->sourceDepotId) continue;
			for (const Position& lockerPosition : entry.second) {
			++result.indexedCandidates;
			uint16_t lockerItemId = 0;
			if (!findDepotLocker(lockerPosition, entry.first, lockerItemId)) continue;
			++result.inScopeCandidates;
			for (int32_t xOffset = -1; xOffset <= 1; ++xOffset) for (int32_t yOffset = -1; yOffset <= 1; ++yOffset) {
				if (xOffset == 0 && yOffset == 0) continue;
				const Position approach(lockerPosition.x + xOffset, lockerPosition.y + yOffset, lockerPosition.z);
				if (sellLootPlan && approach != sellLootPlan->sourceApproach) continue;
				Tile* tile = g_game.map.getTile(approach);
				if (!tile || tile->queryAdd(0, player, 1, FLAG_IGNOREBLOCKCREATURE) != RETURNVALUE_NOERROR) continue;
				++result.standableCandidates;
				const std::optional<uint32_t> routeCost = topology.distanceTo(
					distances, PlayerBotNavigationGoal::exact(approach));
				if (!routeCost && (!sellLootPlan || entry.first != sellLootPlan->sourceDepotId)) continue;
				result.candidates.push_back({entry.first, lockerItemId, lockerPosition, approach,
				                             routeCost.value_or(playerBotNavigationDistance(currentPosition, approach))});
			}
			}
		}
		return result;
	};

	PlayerBotDepotCommand command = advance({});
	if (command.snapshot.hasSelectedDepot && !fixtureDepot.synthetic) {
		uint16_t lockerItemId = 0;
		if (!findDepotLocker(command.snapshot.selected.lockerPosition, command.snapshot.selected.depotId, lockerItemId) ||
		    lockerItemId != command.snapshot.selected.lockerItemId) {
			PlayerBotDepotObservation observation;
			observation.actionResult = PlayerBotDepotActionResult::SelectedLockerUnavailable;
			command = advance(observation);
		}
	}
	if (command.type == PlayerBotDepotCommandType::Scan) {
		PlayerBotDepotObservation observation;
		observation.scan = scan();
		command = advance(observation);
	}

	std::deque<PlayerBotNavigationStep> steps;
	uint32_t routeValidations = 0;
	while (command.type == PlayerBotDepotCommandType::ValidateRoute && command.snapshot.hasRouteCandidate &&
	       routeValidations < depotRouteValidationsPerDecision) {
		++routeValidations;
		const PlayerBotDepotCandidate& candidate = command.snapshot.routeCandidate;
		PlayerBotDepotObservation observation;
		uint16_t lockerItemId = 0;
		Tile* tile = g_game.map.getTile(candidate.approachPosition);
		const bool valid = fixtureDepot.synthetic ||
			(findDepotLocker(candidate.lockerPosition, candidate.depotId, lockerItemId) && lockerItemId == candidate.lockerItemId &&
			 tile && tile->queryAdd(0, player, 1, FLAG_IGNOREBLOCKCREATURE) == RETURNVALUE_NOERROR);
		const auto startedAt = std::chrono::steady_clock::now();
		PlayerBotNavigationRoutePlan routePlan;
		if (valid && candidate.approachPosition != currentPosition) {
			if (sellLootPlan && candidate.depotId == sellLootPlan->sourceDepotId && sellLootPlan->sourceAllowNpcTravel) {
				if (auto paidRoute = planNpcTravelRoute(player, candidate.approachPosition, {},
				                                        playerBotNavigationMaximumExpandedNodes)) {
					routePlan = std::move(*paidRoute);
				}
			} else {
				routePlan = planCompleteNavigationRoute(player, candidate.approachPosition);
			}
		}
		const PlayerBotNavigationRiskProfile risk;
		const bool routeSafe = candidate.approachPosition == currentPosition || playerBotNavigationRiskAccepts(
		    risk, routePlan.metrics.dangerCost, routePlan.metrics.maximumHealthLossPerSecond);
		const bool fareAccepted = !sellLootPlan || candidate.depotId != sellLootPlan->sourceDepotId ||
		                          routePlan.metrics.fare <= sellLootPlan->sourceFare;
		const bool executable = valid && fareAccepted && (candidate.approachPosition == currentPosition ||
			(routePlan.metrics.result == PlayerBotNavigationResult::Reached && !routePlan.steps.empty()));
		const bool liquidationSource = sellLootPlan && candidate.depotId == sellLootPlan->sourceDepotId;
		const bool reached = executable && (routeSafe || (!liquidationSource && command.snapshot.validatingRiskFallback));
		telemetry.recordPathfinding(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - startedAt), executable);
		observation.routeResult = reached ? PlayerBotDepotRouteResult::Reached :
		                          executable ? PlayerBotDepotRouteResult::Unsafe : PlayerBotDepotRouteResult::Unreachable;
		observation.routeSteps = static_cast<uint32_t>(routePlan.metrics.steps);
		observation.expandedNodes = routePlan.metrics.expandedNodes;
		observation.dangerCost = routePlan.metrics.dangerCost;
		observation.maximumHealthLossPerSecond = routePlan.metrics.maximumHealthLossPerSecond;
		if (reached) steps = std::move(routePlan.steps);
		command = advance(observation);
	}
	if (command.type == PlayerBotDepotCommandType::ValidateRoute) {
		emit("action_result", currentPosition,
		     "\"action\":\"depot_discover\",\"result\":\"continuing\",\"reason\":\"route_validation_budget_exhausted\",\"indexed\":" +
		         std::to_string(command.snapshot.indexedCandidates) + ",\"in_scope\":" +
		         std::to_string(command.snapshot.inScopeCandidates) + ",\"standable\":" +
		         std::to_string(command.snapshot.standableCandidates) + ",\"route_validations\":" +
		         std::to_string(routeValidations) + ",\"unsafe_routes\":" +
		         std::to_string(command.snapshot.unsafeRouteCandidates));
		schedule(blockedRouteRetryInterval);
		return false;
	}
	if (command.type == PlayerBotDepotCommandType::Navigate && command.snapshot.hasSelectedDepot) {
		if (!steps.empty()) {
			observeNavigationPlan(command.snapshot.selected.approachPosition, std::move(steps));
		}
		const PlayerBotDepotCandidate& depot = command.snapshot.selected;
		std::ostringstream fields;
		fields << "\"action\":\"depot_discover\",\"result\":\"success\",\"depot_id\":" << depot.depotId
		       << ",\"locker_item_id\":" << depot.lockerItemId << ",\"locker\":{\"x\":" << depot.lockerPosition.x
		       << ",\"y\":" << depot.lockerPosition.y << ",\"z\":" << static_cast<uint16_t>(depot.lockerPosition.z)
		       << "},\"approach\":{\"x\":" << depot.approachPosition.x << ",\"y\":" << depot.approachPosition.y
		       << ",\"z\":" << static_cast<uint16_t>(depot.approachPosition.z) << "},\"distance\":" << depot.distance
		       << ",\"route_steps\":" << command.telemetry.routeSteps
		       << ",\"expanded_nodes\":" << command.telemetry.expandedNodes
		       << ",\"danger_cost\":" << command.telemetry.dangerCost
		       << ",\"maximum_health_loss_per_second\":" << command.telemetry.maximumHealthLossPerSecond
		       << ",\"risk_fallback\":" << (command.telemetry.riskFallback ? "true" : "false")
		       << ",\"unsafe_routes\":" << command.snapshot.unsafeRouteCandidates
		       << ",\"indexed\":" << command.snapshot.indexedCandidates
		       << ",\"in_scope\":" << command.snapshot.inScopeCandidates << ",\"standable\":" << command.snapshot.standableCandidates;
		emit("action_result", currentPosition, fields.str());
		return true;
	}
	if (command.type == PlayerBotDepotCommandType::Fail) {
		const char* reason = command.snapshot.inScopeCandidates == 0 ? "no_local_locker" :
		                     command.snapshot.standableCandidates == 0 ? "no_standable_approach" : "no_reachable_locker";
		logActionFailure("depot_discover", reason, currentPosition);
		if (sellLootPlan) deferSellLoot(player, currentPosition, reason);
		else stop("depot_unavailable", currentPosition);
		return false;
	}
	const uint32_t retryDelay = command.snapshot.retryAt ? static_cast<uint32_t>(std::max<int64_t>(1,
		std::chrono::duration_cast<std::chrono::milliseconds>(*command.snapshot.retryAt - now).count())) :
		std::min<uint32_t>(depotRetryMaximumInterval, depotRetryInitialInterval << std::min<uint32_t>(command.snapshot.attempts, 2));
	schedule(command.type == PlayerBotDepotCommandType::Wait ? retryDelay : blockedRouteRetryInterval);
	return false;
}

bool PlayerBotController::openContainer(Player& player, Container& container, uint8_t containerId, const Position& currentPosition)
{
	if (player.getContainerID(&container) >= 0) {
		return true;
	}
	Container* parent = dynamic_cast<Container*>(container.getParent());
	if (parent && player.getContainerID(parent) < 0) {
		return openContainer(player, *parent, containerId, currentPosition);
	}
	if (!player.canDoAction()) {
		schedule(navigationDecisionDelay(player));
		return false;
	}
	Position fromPosition(0xFFFF, CONST_SLOT_BACKPACK, 0);
	uint8_t fromIndex = 0;
	if (parent) {
		const int8_t parentId = player.getContainerID(parent);
		const int32_t index = parentId < 0 ? -1 : parent->getThingIndex(&container);
		if (index < 0 || index > UINT8_MAX) {
			logActionFailure("depot_open_source", "container_parent_unavailable", currentPosition);
			schedule(blockedRouteRetryInterval);
			return false;
		}
		fromPosition = Position(0xFFFF, 0x40 | static_cast<uint8_t>(parentId), static_cast<uint8_t>(index));
		fromIndex = static_cast<uint8_t>(index);
		if (parentId == containerId) {
			--containerId;
		}
	} else if (player.getInventoryItem(CONST_SLOT_BACKPACK) != &container) {
		logActionFailure("depot_open_source", "container_not_carried", currentPosition);
		schedule(blockedRouteRetryInterval);
		return false;
	}
	player.closeContainer(containerId);
	telemetry.recordActionAttempt();
	g_game.playerUseItem(playerId, fromPosition, fromIndex, containerId, container.getClientID());
	schedule(navigationDecisionDelay(player));
	return false;
}

bool PlayerBotController::openDepotLocker(Player& player, const PlayerBotDepotSnapshot& depot, const Position& currentPosition)
{
	if (fixtureDriver.depotEndpoint().synthetic) {
		return true;
	}
	Container* opened = player.getContainerByID(depotLockerContainerId);
	if (opened && opened->getDepotLocker() && opened->getDepotLocker()->getDepotId() == depot.selected.depotId) {
		return true;
	}
	Tile* tile = g_game.map.getTile(depot.selected.lockerPosition);
	TileItemVector* items = tile ? tile->getItemList() : nullptr;
	if (!items) {
		setCyclePhase(CyclePhase::ReturnToDepot, currentPosition, "depot_locker_tile_unavailable");
		schedule(blockedRouteRetryInterval);
		return false;
	}
	if (!player.canDoAction()) {
		schedule(navigationDecisionDelay(player));
		return false;
	}
	for (Item* item : *items) {
		Container* container = item->getContainer();
		if (!container || !container->getDepotLocker() || container->getDepotLocker()->getDepotId() != depot.selected.depotId) {
			continue;
		}
		const int32_t stackPosition = tile->getThingIndex(item);
		if (stackPosition < 0 || stackPosition > UINT8_MAX) {
			break;
		}
		player.closeContainer(depotLockerContainerId);
		telemetry.recordActionAttempt();
		g_game.playerUseItem(playerId, depot.selected.lockerPosition, static_cast<uint8_t>(stackPosition), depotLockerContainerId,
		                     item->getClientID());
		emit("action_result", currentPosition, "\"action\":\"depot_open_locker\",\"result\":\"requested\",\"depot_id\":" +
		     std::to_string(depot.selected.depotId) + ",\"container_id\":" + std::to_string(depotLockerContainerId) +
		     ",\"attempt\":" + std::to_string(depot.attempts));
		if (pauseDepotFixtureForRestart(player, DepotRestartCheckpoint::Locker, currentPosition)) {
			return false;
		}
		schedule(navigationDecisionDelay(player));
		return false;
	}
	logActionFailure("depot_open_locker", "locker_identity_changed", currentPosition);
	resetNavigation();
	setCyclePhase(CyclePhase::ReturnToDepot, currentPosition, "depot_locker_identity_changed");
	schedule(blockedRouteRetryInterval);
	return false;
}

bool PlayerBotController::openDepotChest(Player& player, const PlayerBotDepotSnapshot& depot, const Position& currentPosition)
{
	if (fixtureDriver.depotEndpoint().synthetic) {
		return true;
	}
	Container* locker = player.getContainerByID(depotLockerContainerId);
	if (!locker || !locker->getDepotLocker() || locker->getDepotLocker()->getDepotId() != depot.selected.depotId) {
		schedule(blockedRouteRetryInterval);
		return false;
	}
	DepotChest* chest = player.getDepotChest(depot.selected.depotId, false);
	if (!chest) {
		logActionFailure("depot_open_chest", "player_chest_missing", currentPosition);
		stop("depot_chest_missing", currentPosition);
		return false;
	}
	fixtureDriver.prepareDepotMoveDestination(*chest);
	if (player.getContainerByID(depotChestContainerId) == chest) {
		return true;
	}
	const int32_t index = locker->getThingIndex(chest);
	if (index < 0 || index > UINT8_MAX) {
		logActionFailure("depot_open_chest", "chest_not_in_locker", currentPosition);
		stop("depot_chest_not_in_locker", currentPosition);
		return false;
	}
	if (!player.canDoAction()) {
		schedule(navigationDecisionDelay(player));
		return false;
	}
	player.closeContainer(depotChestContainerId);
	telemetry.recordActionAttempt();
	g_game.playerUseItem(playerId, Position(0xFFFF, 0x40 | depotLockerContainerId, static_cast<uint8_t>(index)),
	                     static_cast<uint8_t>(index), depotChestContainerId, chest->getClientID());
	emit("action_result", currentPosition, "\"action\":\"depot_open_chest\",\"result\":\"requested\",\"depot_id\":" +
	     std::to_string(depot.selected.depotId) + ",\"container_id\":" + std::to_string(depotChestContainerId) +
	     ",\"attempt\":" + std::to_string(depot.attempts));
	if (pauseDepotFixtureForRestart(player, DepotRestartCheckpoint::Chest, currentPosition)) {
		return false;
	}
	schedule(navigationDecisionDelay(player));
	return false;
}

bool PlayerBotController::pauseDepotFixtureForRestart(Player& player, DepotRestartCheckpoint checkpoint,
                                                       const Position& currentPosition)
{
	if (!fixtureDriver.depotRestartObservation(player, checkpoint).pause) {
		return false;
	}
	const char* phase = checkpoint == DepotRestartCheckpoint::Approach ? "approach" :
	                    checkpoint == DepotRestartCheckpoint::Locker ? "locker" :
	                    checkpoint == DepotRestartCheckpoint::Chest ? "chest" :
	                    checkpoint == DepotRestartCheckpoint::Deposit ? "deposit" : "depart";
	emit("action_result", currentPosition,
	     "\"action\":\"depot_restart_checkpoint\",\"result\":\"paused\",\"phase\":" + jsonString(phase));
	pause(currentPosition);
	return true;
}

uint8_t PlayerBotController::containerDestinationIndex(const Container& container, const Item& item) const
{
	if (item.isStackable()) {
		const ItemDeque& items = container.getItemList();
		for (size_t index = 0; index < items.size(); ++index) {
			if (items[index]->getID() == item.getID() && items[index]->getItemCount() < 100) {
				return static_cast<uint8_t>(index);
			}
		}
	}
	return static_cast<uint8_t>(std::min<size_t>(container.size(), UINT8_MAX));
}

void PlayerBotController::processDeposit(Player* player, const Position& currentPosition)
{
	if (sellLootPlan && player->getContainerByID(depotChestContainerId) &&
		processSellLootWithdrawal(*player, currentPosition)) {
		return;
	}
	const PlayerBotFixtureDepotEndpoint fixtureDepot = fixtureDriver.depotEndpoint();
	Tile* fixtureDestination = fixtureDepot.synthetic ? g_game.map.getTile(fixtureDepot.destinationPosition) : nullptr;
	if (fixtureDepot.synthetic && !fixtureDestination) {
		stop("depot_destination_unavailable", currentPosition);
		return;
	}
	auto advance = [&](PlayerBotDepotObservation observation) {
		observation.currentPosition = currentPosition;
		observation.now = std::chrono::steady_clock::now();
		observation.fixtureSynthetic = fixtureDepot.synthetic;
		return depotWorkflow.advance(observation, depotRouteValidationsPerDecision, maximumDepotAttempts, depotApproachSuppression);
	};
	PlayerBotDepotCommand command = advance({});
	PlayerBotDepotObservation observation;
	if (command.snapshot.hasSelectedDepot) {
		observation.atApproach = fixtureDepot.synthetic || Position::areInRange<1, 1, 0>(currentPosition, command.snapshot.selected.lockerPosition);
		observation.lockerOpen = fixtureDepot.synthetic || player->getContainerByID(depotLockerContainerId) != nullptr;
		observation.chestOpen = fixtureDepot.synthetic || player->getContainerByID(depotChestContainerId) != nullptr;
		observation.canDoAction = player->canDoAction();
		if (command.snapshot.hasPendingMove) {
			Container* chest = player->getContainerByID(depotChestContainerId);
			if (!chest && !fixtureDepot.synthetic) {
				observation.atApproach = true;
				observation.lockerOpen = player->getContainerByID(depotLockerContainerId) != nullptr;
				observation.chestOpen = false;
				observation.canDoAction = player->canDoAction();
				observation.actionResult = PlayerBotDepotActionResult::MoveDestinationUnavailable;
				command = advance(observation);
				if (command.type == PlayerBotDepotCommandType::OpenLocker &&
				    !openDepotLocker(*player, command.snapshot, currentPosition)) return;
				if (command.type == PlayerBotDepotCommandType::OpenChest &&
				    !openDepotChest(*player, command.snapshot, currentPosition)) return;
				schedule(navigationDecisionDelay(*player));
				return;
			}
			observation.move = {true, inventoryPolicy.inventoryItemCount(*player, command.snapshot.pendingMove.itemId),
			                    fixtureDepot.synthetic ? fixtureDestination->getItemTypeCount(command.snapshot.pendingMove.itemId) :
			                                             chest->getItemTypeCount(command.snapshot.pendingMove.itemId)};
		}
		command = advance(observation);
	}
	if (command.telemetry.moveVerification) {
		const PlayerBotDepotMoveVerification& verification = *command.telemetry.moveVerification;
		const PlayerBotDepotMove& move = verification.before;
		if (verification.result == PlayerBotDepotMoveResult::Mismatch) {
			logActionFailure("deposit", "move_delta_mismatch", currentPosition);
			stop("depot_move_delta_mismatch", currentPosition);
			return;
		}
		if (verification.result == PlayerBotDepotMoveResult::Rejected) {
			if (!player->canDoAction()) {
				schedule(navigationDecisionDelay(*player));
				return;
			}
			Item* discardItem = nullptr;
			uint8_t discardableCount = 0;
			Position sourcePosition;
			uint8_t sourceIndex = 0;
			if (move.sourceSlot == CONST_SLOT_WHEREEVER) {
				Item* backpackItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
				Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
				Container* source = nullptr;
				if (!backpack || !findDepositableItem(*player, backpack, source, discardItem, discardableCount) ||
				    !source || !discardItem || discardItem->getID() != move.itemId) {
					stop("depot_discard_source_unavailable", currentPosition);
					return;
				}
				const int8_t sourceContainerId = player->getContainerID(source);
				const ItemDeque& sourceItems = source->getItemList();
				auto sourceItem = std::find(sourceItems.begin(), sourceItems.end(), discardItem);
				if (sourceContainerId < 0 || sourceItem == sourceItems.end() ||
				    std::distance(sourceItems.begin(), sourceItem) > UINT8_MAX) {
					stop("depot_discard_source_unavailable", currentPosition);
					return;
				}
				sourceIndex = static_cast<uint8_t>(std::distance(sourceItems.begin(), sourceItem));
				sourcePosition = Position(0xFFFF, 0x40 | static_cast<uint8_t>(sourceContainerId), sourceIndex);
			} else {
				slots_t discardSlot = CONST_SLOT_WHEREEVER;
				discardItem = findActionableSlottedItem(*player, move.itemId, discardSlot);
				if (!discardItem || discardSlot != move.sourceSlot) {
					stop("depot_discard_source_unavailable", currentPosition);
					return;
				}
				discardableCount = discardItem->getItemCount();
				g_game.internalGetPosition(discardItem, sourcePosition, sourceIndex);
				if (sourcePosition.x != 0xFFFF || sourcePosition.y != discardSlot) {
					stop("depot_discard_source_unavailable", currentPosition);
					return;
				}
			}
			Tile* destination = g_game.map.getTile(currentPosition);
			if (!destination) {
				stop("depot_discard_source_unavailable", currentPosition);
				return;
			}
			const uint8_t discardCount = std::min(move.requestedCount, discardableCount);
			const uint32_t inventoryBefore = inventoryPolicy.inventoryItemCount(*player, move.itemId);
			const uint32_t groundBefore = destination->getItemTypeCount(move.itemId);
			telemetry.recordActionAttempt();
			g_game.playerMoveItem(player,
			    sourcePosition,
			    discardItem->getClientID(), sourceIndex, currentPosition, discardCount, discardItem, destination);
			const uint32_t inventoryAfter = inventoryPolicy.inventoryItemCount(*player, move.itemId);
			const uint32_t groundAfter = destination->getItemTypeCount(move.itemId);
			if (discardCount == 0 || inventoryBefore - std::min(inventoryBefore, inventoryAfter) != discardCount ||
			    groundAfter - std::min(groundBefore, groundAfter) != discardCount) {
				stop("depot_discard_move_rejected", currentPosition);
				return;
			}
			emit("action_result", currentPosition,
			     "\"action\":\"deposit\",\"result\":\"discarded\",\"reason\":\"depot_rejected\",\"policy\":\"known_loot\",\"depot_id\":" +
			     std::to_string(command.snapshot.selected.depotId) + ",\"container_id\":" + std::to_string(depotChestContainerId) +
			     ",\"item_id\":" + std::to_string(move.itemId) + ",\"count\":" + std::to_string(discardCount) +
			     ",\"inventory_before\":" + std::to_string(inventoryBefore) + ",\"inventory_after\":" + std::to_string(inventoryAfter) +
			     ",\"ground_before\":" + std::to_string(groundBefore) + ",\"ground_after\":" + std::to_string(groundAfter) +
			     ",\"depot_before\":" + std::to_string(move.destinationCount) + ",\"depot_after\":" +
			     std::to_string(verification.destinationCount) + ",\"retry\":" + std::to_string(verification.attempts));
			observation.actionResult = PlayerBotDepotActionResult::RejectedMoveDiscarded;
			advance(observation);
			schedule(navigationDecisionDelay(*player));
			return;
		}
		std::ostringstream fields;
		fields << "\"action\":\"deposit\",\"result\":" << jsonString(verification.result == PlayerBotDepotMoveResult::Moved ?
			(verification.movedCount == move.requestedCount ? "success" : "partial") : "retry") << ",\"policy\":\"known_loot\",\"depot_id\":"
		       << command.snapshot.selected.depotId << ",\"container_id\":" << static_cast<uint32_t>(depotChestContainerId)
		       << ",\"item_id\":" << move.itemId << ",\"requested\":" << static_cast<uint32_t>(move.requestedCount)
		       << ",\"verified\":" << verification.movedCount;
		if (verification.result == PlayerBotDepotMoveResult::Moved && verification.movedCount == move.requestedCount) {
			fields << ",\"count\":" << verification.movedCount;
		}
		fields << ",\"inventory_before\":" << move.inventoryCount
		       << ",\"inventory_after\":" << verification.inventoryCount << ",\"depot_before\":" << move.destinationCount
		       << ",\"depot_after\":" << verification.destinationCount << ",\"source_slot\":"
		       << (move.sourceSlot == CONST_SLOT_WHEREEVER ? "null" : std::to_string(move.sourceSlot))
		       << ",\"provider_available\":false,\"disposition\":\"deposit\",\"retry\":" << verification.attempts;
		emit("action_result", currentPosition, fields.str());
		if (verification.result == PlayerBotDepotMoveResult::Retry) {
			schedule(navigationDecisionDelay(*player));
			return;
		}
	}
	if (command.type == PlayerBotDepotCommandType::OpenLocker) {
		if (!openDepotLocker(*player, command.snapshot, currentPosition)) return;
		schedule(navigationDecisionDelay(*player));
		return;
	}
	if (command.type == PlayerBotDepotCommandType::OpenChest) {
		if (!openDepotChest(*player, command.snapshot, currentPosition)) return;
		schedule(navigationDecisionDelay(*player));
		return;
	}
	if (command.type == PlayerBotDepotCommandType::Wait) {
		schedule(navigationDecisionDelay(*player));
		return;
	}
	if (command.type == PlayerBotDepotCommandType::Fail) {
		stop("depot_action_failed", currentPosition);
		return;
	}
	if (command.type == PlayerBotDepotCommandType::Depart) {
		std::ostringstream fields;
		fields << "\"action\":\"deposit\",\"result\":\"complete\",\"depot_id\":" << command.snapshot.selected.depotId
		       << ",\"container_id\":" << static_cast<uint32_t>(depotChestContainerId) << ",\"cycle\":" << huntCoordinator.completedHuntCycles();
		emit("action_result", currentPosition, fields.str());
		Container* chest = player->getContainerByID(depotChestContainerId);
		bool selectedAfterDeposit = false;
		if (!fixtureDepot.synthetic && chest) {
			const bool saleCoolingDown = progressionRuntime.isCoolingDown(TopLevelGoal::SellLoot, std::chrono::steady_clock::now());
			if (sellLootPlan && sellLootPlan->sourceDepotId == command.snapshot.selected.depotId) {
				processSellLootWithdrawal(*player, currentPosition);
				return;
			}
			if (!saleCoolingDown && !sellLootPlan &&
			    planSellLootTrip(*player, command.snapshot.selected.depotId, currentPosition)) {
				if (sellLootPlan->sourceDepotId == command.snapshot.selected.depotId) {
					processSellLootWithdrawal(*player, currentPosition);
					return;
				}
				player->closeContainer(depotChestContainerId);
				player->closeContainer(depotLockerContainerId);
				depotWorkflow.reset();
				resetNavigation();
				setCyclePhase(CyclePhase::ReturnToDepot, currentPosition, "sell_loot_source_selected");
				schedule(SCHEDULER_MINTICKS);
				return;
			}
			if (sellLootSearchPending) {
				schedule(blockedRouteRetryInterval);
				return;
			}
			if (progressionRuntime.activeGoal() != TopLevelGoal::Service) {
				selectTopLevelGoal(*player, currentPosition, "depot_deposit_complete");
				selectedAfterDeposit = true;
				if (progressionRuntime.activeGoal() == TopLevelGoal::SellLoot) {
					processDeposit(player, currentPosition);
					return;
				}
			}
		}
		player->closeContainer(depotChestContainerId);
		player->closeContainer(depotLockerContainerId);
		if (pauseDepotFixtureForRestart(*player, DepotRestartCheckpoint::Depart, currentPosition)) return;
		if (selectedAfterDeposit) {
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		if (progressionRuntime.activeGoal() == TopLevelGoal::Service) {
			setCyclePhase(CyclePhase::Service, currentPosition, "depot_complete");
		} else if (fixtureDriver.progressionGoalLoop(true).selectGoal) {
			selectTopLevelGoal(*player, currentPosition, "service_complete");
		} else {
			startHunt(player, currentPosition, "deposit_complete");
		}
		schedule(navigationInterval);
		return;
	}
	if (command.type != PlayerBotDepotCommandType::SelectDeposit) return;

	Item* backpackItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
	Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
	if (!backpack) {
		stop("depot_backpack_unavailable", currentPosition);
		return;
	}
	Container* chest = player->getContainerByID(depotChestContainerId);
	if (!fixtureDepot.synthetic && (!chest || player->getDepotChest(command.snapshot.selected.depotId, false) != chest)) {
		schedule(blockedRouteRetryInterval);
		return;
	}

	Container* source = nullptr;
	Item* depositItem = nullptr;
	uint8_t count = 0;
	slots_t sourceSlot = CONST_SLOT_WHEREEVER;
	refreshItemValues();
	Item* upgrade = nullptr;
	EquipmentUpgrade upgradeInfo{};
	if (PlayerBotEquipmentAdapter::findCarriedUpgrade(equipmentPolicy, *player, upgrade, upgradeInfo)) {
		if (!beginReadinessEquipment(player, currentPosition, "depot_carried_upgrade", true)) {
			schedule(navigationDecisionDelay(*player));
		}
		return;
	}
	if (!findDepositableItem(*player, backpack, source, depositItem, count)) {
		depositItem = findActionableSlottedItem(*player, 0, sourceSlot);
		if (depositItem) {
			const uint32_t carried = inventoryPolicy.inventoryItemCount(*player, depositItem->getID());
			const uint32_t reserve = depositItem->getID() == recoveryPotionItemId(player->getVocationId()) ?
				std::max(inventoryPolicy.protectedItemReserve(*player, depositItem->getID()), huntPotionRestockTarget) :
				inventoryPolicy.protectedItemReserve(*player, depositItem->getID());
			const uint32_t movable = depositItem->isStackable() && carried > reserve ?
				std::min<uint32_t>(depositItem->getItemCount(), carried - reserve) : (carried > reserve ? 1 : 0);
			count = static_cast<uint8_t>(std::min<uint32_t>(movable, UINT8_MAX));
		}
	}
	if ((!depositItem || count == 0) && inventoryPolicy.huntFreeCapacity(*player) < returnCapacityThreshold) {
		stop("depot_capacity_not_recovered", currentPosition);
		return;
	}
	if (depositItem && count != 0 && source &&
	    !openContainer(*player, *source, depotSourceContainerId, currentPosition)) {
		return;
	}
	PlayerBotDepotObservation depositObservation;
	depositObservation.atApproach = true;
	depositObservation.lockerOpen = true;
	depositObservation.chestOpen = true;
	depositObservation.canDoAction = player->canDoAction();
	depositObservation.deposit.observed = true;
	depositObservation.deposit.hasDepositableItem = depositItem && count != 0;
	if (depositObservation.deposit.hasDepositableItem) {
		depositObservation.deposit.move = {depositItem->getID(), fixtureDepot.synthetic ? fixtureDestination->getItemTypeCount(depositItem->getID()) : chest->getItemTypeCount(depositItem->getID()),
		                                   inventoryPolicy.inventoryItemCount(*player, depositItem->getID()), count, sourceSlot};
	}
	command = advance(depositObservation);
	if (command.type == PlayerBotDepotCommandType::Depart) {
		processDeposit(player, currentPosition);
		return;
	}
	if (command.type == PlayerBotDepotCommandType::Wait) {
		schedule(navigationDecisionDelay(*player));
		return;
	}
	if (command.type != PlayerBotDepotCommandType::MoveDeposit || !depositItem) return;

	Position sourcePosition;
	uint8_t sourceIndex = 0;
	if (source) {
		const int8_t sourceContainerId = player->getContainerID(source);
		const ItemDeque& sourceItems = source->getItemList();
		auto sourceItem = std::find(sourceItems.begin(), sourceItems.end(), depositItem);
		if (sourceContainerId < 0 || sourceItem == sourceItems.end() ||
		    std::distance(sourceItems.begin(), sourceItem) > UINT8_MAX) {
			logActionFailure("deposit", "source_unavailable", currentPosition);
			schedule(blockedRouteRetryInterval);
			return;
		}
		sourceIndex = static_cast<uint8_t>(std::distance(sourceItems.begin(), sourceItem));
		sourcePosition = Position(0xFFFF, 0x40 | static_cast<uint8_t>(sourceContainerId), sourceIndex);
	} else {
		g_game.internalGetPosition(depositItem, sourcePosition, sourceIndex);
		if (sourcePosition.x != 0xFFFF || sourcePosition.y != sourceSlot) {
			logActionFailure("deposit", "slotted_source_unavailable", currentPosition);
			schedule(blockedRouteRetryInterval);
			return;
		}
	}
	const PlayerBotFixtureEngineCommand moveCommand = fixtureDriver.depotMoveCommand(count);
	const uint8_t submittedCount = moveCommand.count;
	telemetry.recordActionAttempt();
	if (moveCommand.dispatch) g_game.playerMoveItem(player, sourcePosition, depositItem->getClientID(), sourceIndex,
		fixtureDepot.synthetic ? fixtureDepot.destinationPosition :
		                          Position(0xFFFF, 0x40 | depotChestContainerId, containerDestinationIndex(*chest, *depositItem)),
		submittedCount, depositItem, fixtureDepot.synthetic ? static_cast<Cylinder*>(fixtureDestination) : static_cast<Cylinder*>(chest));
	emit("action_result", currentPosition, "\"action\":\"deposit\",\"result\":\"requested\",\"policy\":\"known_loot\",\"depot_id\":" +
	     std::to_string(command.snapshot.selected.depotId) + ",\"container_id\":" + std::to_string(depotChestContainerId) + ",\"item_id\":" +
	     std::to_string(command.snapshot.pendingMove.itemId) + ",\"requested\":" + std::to_string(count) + ",\"submitted\":" +
	     std::to_string(submittedCount) + ",\"inventory_before\":" +
	     std::to_string(command.snapshot.pendingMove.inventoryCount) + ",\"depot_before\":" + std::to_string(command.snapshot.pendingMove.destinationCount) +
	     ",\"source_slot\":" + (sourceSlot == CONST_SLOT_WHEREEVER ? "null" : std::to_string(sourceSlot)) +
	     ",\"provider_available\":false,\"disposition\":\"deposit\"");
	if (pauseDepotFixtureForRestart(*player, DepotRestartCheckpoint::Deposit, currentPosition)) {
		return;
	}
	schedule(navigationDecisionDelay(*player));
}
