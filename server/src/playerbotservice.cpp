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
#include "playerbotselllootfilter.h"
#include "playerbotselllootapproach.h"
#include "playerbotselllootbasket.h"
#include "playerbotplanningbudget.h"
#include "playerbothunttravelpolicy.h"
#include "playerbottransportsearch.h"
#include "groups.h"
#include "guild.h"

#include "depotchest.h"
#include "depotlocker.h"

// NPC service discovery, shopping, banking, and depot handling.
using namespace playerbot;

namespace {
	constexpr size_t maximumServiceProviderApproaches = 8;
	constexpr size_t maximumServiceProvidersPerItem = 4;
	constexpr size_t maximumServiceBankers = 2;
	constexpr size_t maximumSellLootRouteValidationsPerDecision = 1;
	// One-second waits; a refused step keeps its tile blocked for ten seconds.
	constexpr uint32_t maximumSellLootBatch = 100;
	constexpr uint32_t sellLootTravelTimeGoldPerMinute = 10;
	constexpr std::chrono::seconds sellLootFailureCooldown(60);
	uint64_t sellLootPlanningPass = 0;

	int32_t approachDirection(const Position& provider, const Position& approach)
	{
		const int32_t x = approach.x < provider.x ? 0 : approach.x > provider.x ? 2 : 1;
		const int32_t y = approach.y < provider.y ? 0 : approach.y > provider.y ? 2 : 1;
		return x * 3 + y;
	}
}

bool PlayerBotController::sellLootManifestFits(Player& player, const Container& chest, const Container& backpack,
                                               const std::vector<SellLootBatch>& batches)
{
	std::vector<SellLootBasketItem> basket;
	basket.reserve(batches.size());
	for (const SellLootBatch& batch : batches) {
		const uint32_t remaining = batch.count - batch.withdrawn;
		uint32_t available = 0, mergeRoom = 0;
		const Item* exemplar = nullptr;
		const bool stackable = Item::items[batch.itemId].stackable;
		for (Item* item : chest.getItemList()) {
			if (item->getID() != batch.itemId || item->getContainer() ||
			    inventoryPolicy.isProtectedDepositItem(player, *item)) continue;
			if (exemplar && stackable && !exemplar->equals(item)) return false;
			exemplar = item;
			available += item->getItemCount();
		}
		if (available < remaining) return false;
		if (stackable && exemplar) for (Item* carried : backpack.getItemList()) {
			if (exemplar->equals(carried)) mergeRoom += 100 - carried->getItemCount();
		}
		basket.push_back({remaining, Item::items[batch.itemId].weight, stackable, mergeRoom});
	}
	const auto counts = playerBotSellLootBasket(basket, player.getFreeCapacity(),
		static_cast<uint32_t>(backpack.capacity() > backpack.size() ? backpack.capacity() - backpack.size() : 0),
		maximumSellLootBatch);
	for (size_t i = 0; i < counts.size(); ++i) {
		if (counts[i] != basket[i].available) return false;
	}
	return true;
}

bool PlayerBotController::planSellLootTrip(Player& player, uint16_t currentDepotId, const Position& position,
                                           std::chrono::steady_clock::duration* retryAfter)
{
	const auto began = std::chrono::steady_clock::now();
	auto& budget = playerBotHuntPlanningBudget();
	const auto admission = budget.request(playerId, began);
	if (!admission.admitted) {
		sellLootSearchPending = true;
		// Round up so the head does not retry before it has earned credit.
		if (retryAfter) *retryAfter = std::chrono::duration_cast<std::chrono::milliseconds>(
		    admission.wait + std::chrono::microseconds(999));
		if (const auto suppressed = sellLootBudgetRecords.admit(began)) {
			emit("sell_loot_plan", position, "\"action\":\"sell_loot_plan\",\"result\":\"deferred\",\"reason\":\"planning_budget\",\"suppressed_denials\":" +
			     std::to_string(*suppressed) + ",\"budget_wait_us\":" + std::to_string(admission.wait.count()) +
			     ",\"budget_debt_us\":" + std::to_string(admission.debtUs));
		}
		return false;
	}
	PlayerBotPlanningBudget::Charge charge(budget, playerId);
	const bool survivalSell = supplyRecovery.active();
	// Use the same fare reserve that service travel checks after withdrawal.
	// Free routes remain eligible when no paid travel is affordable.
	const uint64_t fareReserve = std::max(huntTravelRecoveryFundsReserve(player, huntTravelBudgetPhase),
	                                      huntTravelReturnFareReserve(huntTravelBudgetPhase));
	const uint64_t spendableFare = playerBotHuntTransportFunds(player.getMoney() + player.getBankBalance(), fareReserve);
	const bool canUseRope = g_game.findItemOfType(&player, ropeItemId, true) != nullptr;
	const bool canUseShovel = g_game.findItemOfType(&player, 2554, true) != nullptr;
	const auto& topology = PlayerBotTopology::instance();
	// A standable tile behind a shop counter is not necessarily reachable.
	// Rank all conversation tiles by coarse connectivity, then validate them
	// with the normal sliced live-route search before choosing a sale.
	auto conversationApproaches = [&](const Position& start, const Position& provider,
	                                 const PlayerBotTopologyDistances& distances) {
		std::vector<SellLootApproach> approaches;
		for (int32_t x = -3; x <= 3; ++x) for (int32_t y = -3; y <= 3; ++y) {
			if (!x && !y) continue;
			const Position target(provider.x + x, provider.y + y, provider.z);
			Tile* tile = g_game.map.getTile(target);
			if (!playerBotStableApproachTile(tile, player)) continue;
			const auto coarseDistance = topology.distanceTo(distances, target);
			approaches.push_back({target, coarseDistance.has_value(), coarseDistance.value_or(UINT32_MAX),
				playerBotNavigationDistance(start, target), static_cast<uint8_t>(approachDirection(provider, target))});
		}
		std::vector<PlayerBotApproachTile> tiles;
		uint32_t rank = 0;
		for (const Position& target : playerBotSellLootApproaches(std::move(approaches), maximumServiceProviderApproaches)) {
			const Tile* tile = g_game.map.getTile(target);
			const Creature* occupant = tile ? tile->getTopCreature() : nullptr;
			tiles.push_back({target, rank++, occupant && occupant != &player});
		}
		return tiles;
	};
	auto approachPositions = [](const PlayerBotApproach& approach) {
		std::vector<Position> positions;
		for (const PlayerBotApproachTile& tile : approach.offered()) positions.push_back(tile.position);
		return positions;
	};
	auto approachIndex = [](const SellLootCandidate& candidate) {
		const auto& tiles = candidate.approach.offered();
		return static_cast<size_t>(std::find_if(tiles.begin(), tiles.end(), [&candidate](const PlayerBotApproachTile& tile) {
			return tile.position == candidate.providerApproach;
		}) - tiles.begin());
	};
	PlayerBotHuntRouteTiming timing;
	int64_t snapshotUs = 0;
	bool rebuilt = false;
	sellLootPlan.reset();
	if (sellLootSearch && (sellLootSearch->origin != position || sellLootSearch->depotId != currentDepotId)) {
		sellLootSearch.reset();
	}
	if (!sellLootSearch) {
		rebuilt = true;
		sellLootSearch.emplace();
		sellLootSearch->origin = position;
		sellLootSearch->depotId = currentDepotId;
		sellLootSearch->pass = (uint64_t(1) << 63) | ++sellLootPlanningPass;
		std::vector<Npc*> sellers;
		for (const auto& entry : g_game.getNpcs()) {
			Npc* npc = entry.second;
			if (npc && !npc->isRemoved() && playerBotNpcHasCapability(*npc, PlayerBotNpcCapability::Shop)) sellers.push_back(npc);
		}
		auto& candidates = sellLootSearch->candidates;
		auto& scannedItems = sellLootSearch->scannedItems;
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
					if (!playerBotStableApproachTile(tile, player)) continue;
					const uint32_t distance = playerBotNavigationDistance(position, approach);
					if (distance < sourceDistance) {
						sourceDistance = distance;
						sourceApproach = approach;
					}
				}
			}
			if (sourceDistance == std::numeric_limits<uint32_t>::max()) continue;
			const Position sellerStart = depotEntry.first == currentDepotId ? position : sourceApproach;
			const auto sellerDistances = topology.distancesFrom(sellerStart, canUseRope, canUseShovel, player.getLevel());
			struct DepotItem { uint32_t count = 0; const Item* exemplar = nullptr; bool compatible = true; };
			std::map<uint16_t, DepotItem> depotItems;
			const ItemDeque& items = chest->getItemList();
			// Snapshot every eligible top-level item once. A scan window must not
			// split a profitable manifest or silently lose the tail of a depot.
			for (Item* item : items) {
				++scannedItems;
				if (item->getContainer() || inventoryPolicy.isProtectedDepositItem(player, *item)) continue;
				auto& entry = depotItems[item->getID()];
				if (entry.exemplar && item->isStackable() && !entry.exemplar->equals(item)) entry.compatible = false;
				entry.count += item->getItemCount();
				entry.exemplar = item;
			}
			Item* backpackItem = player.getInventoryItem(CONST_SLOT_BACKPACK);
			Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
			if (!backpack) continue;
			for (Npc* seller : sellers) {
				struct OfferedItem { uint16_t itemId; uint32_t count; const ShopInfo* offer; uint32_t weight; bool stackable; uint32_t mergeRoom; };
				std::vector<OfferedItem> offered;
				for (const auto& [itemId, depotItem] : depotItems) {
					const ItemType& type = Item::items[itemId];
					// Mixed item attributes cannot safely share a single stack slot.
					if (type.isFluidContainer() || type.isSplash() || !depotItem.compatible) continue;
					auto offer = std::find_if(seller->getShopOffers().begin(), seller->getShopOffers().end(), [itemId](const ShopInfo& value) {
						return value.itemId == itemId && value.sellPrice != 0;
					});
					if (offer == seller->getShopOffers().end()) continue;
					uint32_t mergeRoom = 0;
					if (type.stackable) for (Item* carried : backpack->getItemList()) {
						if (depotItem.exemplar->equals(carried)) mergeRoom += 100 - carried->getItemCount();
					}
					offered.push_back({itemId, depotItem.count, &*offer, type.weight, type.stackable, mergeRoom});
				}
				std::sort(offered.begin(), offered.end(), [](const OfferedItem& left, const OfferedItem& right) {
					const uint64_t leftDensity = left.weight == 0 ? std::numeric_limits<uint64_t>::max() :
						static_cast<uint64_t>(left.offer->sellPrice) * 1000 / left.weight;
					const uint64_t rightDensity = right.weight == 0 ? std::numeric_limits<uint64_t>::max() :
						static_cast<uint64_t>(right.offer->sellPrice) * 1000 / right.weight;
					return leftDensity != rightDensity ? leftDensity > rightDensity : left.offer->sellPrice > right.offer->sellPrice;
				});
				SellLootCandidate candidate;
				candidate.sourceDepotId = depotEntry.first;
				candidate.sourceApproach = sourceApproach;
				candidate.providerId = seller->getID();
				candidate.providerPosition = seller->getPosition();
				std::vector<SellLootBasketItem> basket;
				basket.reserve(offered.size());
				for (const OfferedItem& item : offered) {
					basket.push_back({item.count, item.weight, item.stackable, item.mergeRoom});
				}
				const auto counts = playerBotSellLootBasket(basket, player.getFreeCapacity(),
					static_cast<uint32_t>(backpack->capacity() > backpack->size() ? backpack->capacity() - backpack->size() : 0),
					maximumSellLootBatch);
				for (size_t i = 0; i < offered.size(); ++i) {
					const OfferedItem& item = offered[i];
					const uint32_t count = counts[i];
					if (count == 0) continue;
					candidate.batches.push_back({item.itemId, count, item.offer->sellPrice,
					                             static_cast<uint8_t>(item.offer->subType), 0});
					candidate.revenue += static_cast<uint64_t>(item.offer->sellPrice) * count;
				}
				if (candidate.batches.empty()) continue;
				candidate.approach.offer(conversationApproaches(sellerStart, candidate.providerPosition, sellerDistances));
				const std::optional<Position> firstApproach = candidate.approach.next();
				if (!firstApproach) continue;
				candidate.providerApproach = *firstApproach;
				candidate.roughCost = static_cast<uint64_t>(sourceDistance) +
					playerBotNavigationDistance(sourceApproach, candidate.providerApproach);
				candidates.push_back(std::move(candidate));
			}
		}
		std::sort(candidates.begin(), candidates.end(), [](const SellLootCandidate& left, const SellLootCandidate& right) {
			const int64_t leftNet = static_cast<int64_t>(left.revenue) - static_cast<int64_t>(left.roughCost);
			const int64_t rightNet = static_cast<int64_t>(right.revenue) - static_cast<int64_t>(right.roughCost);
			return leftNet != rightNet ? leftNet > rightNet : left.revenue > right.revenue;
		});
		snapshotUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count();
	}
	auto& search = *sellLootSearch;
	const auto scannedItems = search.scannedItems;
	size_t routeValidations = 0;
	bool found = false;
	uint32_t selectedSourceTravelNpcId = 0;
	Position selectedSourceTravelTarget;
	Position selectedProviderApproach;
	size_t selectedApproachIndex = 0;
	// The existing route engine slices both walking and transport connections.
	// Borrow its entry point, not the hunt's pending request. The dispatcher is
	// single-threaded; restore the hunt state even if route construction throws.
	auto chooseRoute = [&](const Position& start, const Position& destination,
	                       PlayerBotNavigationRoutePlan* walkingAlternative) {
		struct WorkSlot {
			std::shared_ptr<PlayerBotHuntTravelWork>& hunt;
			std::shared_ptr<PlayerBotHuntTravelWork>& sale;
			WorkSlot(decltype(hunt) hunt, decltype(sale) sale) : hunt(hunt), sale(sale) { hunt.swap(sale); }
			~WorkSlot() { hunt.swap(sale); }
		} slot(huntTravelWork, search.routeWork);
		PlayerBotHuntRouteRequest request;
		request.planningPass = search.pass;
		request.sequence = search.sequence;
		request.from = start;
		request.to = destination;
		return advanceHuntTravelRoute(player, request, start, timing, fareReserve, true, walkingAlternative);
	};
	auto safe = [this](const PlayerBotNavigationRoutePlan& route) {
		return route.metrics.result == PlayerBotNavigationResult::Reached &&
		       playerBotNavigationRiskVerdict(riskProfile, route.metrics) == PlayerBotNavigationRiskVerdict::Accepted;
	};
	auto usesTravel = [](const PlayerBotNavigationRoutePlan& route) {
		return std::any_of(route.steps.begin(), route.steps.end(), [](const auto& step) {
			return step.action == PlayerBotNavigationAction::NpcTravel;
		});
	};
	// Coarse topology decides only what walking and map portals can connect.
	// A null result is unknown and must never prune a candidate.
	auto reachabilityFrom = [&](const Position& start) -> const PlayerBotTopologyReachability* {
		auto& entry = search.reachability[start];
		if (!entry.second || entry.first != topology.generation()) {
			entry = {topology.generation(), topology.walkNode(start) ?
			    topology.reachabilityFrom(start, canUseRope, canUseShovel, player.getLevel()) : nullptr};
		}
		return entry.second.get();
	};
	auto emitRejected = [&](const SellLootCandidate& candidate, const char* reason, uint64_t fare, uint64_t tripCost,
	                        uint32_t danger, const std::string& extra) {
		emit("sell_loot_candidate", position,
		     "\"result\":\"rejected\",\"reason\":" + jsonString(reason) +
		     ",\"source_depot_id\":" + std::to_string(candidate.sourceDepotId) +
		     ",\"npc_id\":" + std::to_string(candidate.providerId) +
		     ",\"expected_revenue\":" + std::to_string(candidate.revenue) +
		     ",\"fare\":" + std::to_string(fare) + ",\"trip_cost\":" + std::to_string(tripCost) +
		     ",\"danger_cost\":" + std::to_string(danger) +
		     ",\"provider_approach_index\":" + std::to_string(approachIndex(candidate)) + extra);
	};
	// A provider counts as boardable when any walkable tile beside it is
	// reachable. Tiles outside the walk graph are unknown, so never prune on them.
	auto boardable = [&](const PlayerBotTopologyReachability& from, const Position& provider) {
		bool unknown = false;
		for (int32_t x = -1; x <= 1; ++x) for (int32_t y = -1; y <= 1; ++y) {
			const Position tile(provider.x + x, provider.y + y, provider.z);
			if (!topology.walkNode(tile)) {
				unknown = true;
				continue;
			}
			if (topology.reachable(from, tile)) return true;
		}
		return unknown;
	};
	// Offers use the route engine's own eligibility, so no bound below can
	// reject a route that the engine could return.
	if (!search.travelOffers) {
		search.travelOffers.emplace();
		const auto now = std::chrono::steady_clock::now();
		for (const auto& entry : g_game.getNpcs()) {
			Npc* npc = entry.second;
			if (player.isPzLocked() || !npc || npc->isRemoved() ||
			    !playerBotNpcHasCapability(*npc, PlayerBotNpcCapability::Travel)) continue;
			for (const auto& offer : npc->getTravelOffers()) {
				const auto unavailable = unavailableTravelOffers.find({npc->getID(), offer.destination});
				if (!playerBotNpcTravelOfferEligible(player.getLevel(), player.isPremium(), spendableFare, offer.level,
				        offer.premium, offer.price, offer.hasOpaqueCondition, offer.hasOpaqueAction) ||
				    (unavailable != unavailableTravelOffers.end() && unavailable->second > now)) continue;
				search.travelOffers->push_back({npc->getPosition(), offer.destination, offer.price});
			}
		}
		const auto& offers = *search.travelOffers;
		auto& graph = search.travelGraph;
		graph.fares.clear();
		graph.boardableAfter.assign(offers.size(), std::vector<bool>(offers.size()));
		for (size_t i = 0; i < offers.size(); ++i) {
			graph.fares.push_back(offers[i].fare);
			const auto* landing = reachabilityFrom(offers[i].destination);
			for (size_t j = 0; j < offers.size(); ++j) {
				graph.boardableAfter[i][j] = !landing || boardable(*landing, offers[j].provider);
			}
		}
	}
	struct LegBound { uint64_t fare = 0; double seconds = 0; };
	// Fare and time bounds are separate minima over the same routes, so their
	// sum is still a lower bound. Time uses the engine's own step model: one
	// relaxed hop covers at most one move or half a one-second tool use.
	const double secondsPerHop = std::min<uint32_t>(player.getStepDuration(DIRECTION_NORTH), 500) / 1000.0;
	auto legBound = [&](const Position& start, const std::vector<Position>& targets) -> std::optional<LegBound> {
		LegBound bound;
		const auto potential = search.potentials.find(start);
		if (potential != search.potentials.end() && !targets.empty()) {
			uint32_t cost = std::numeric_limits<uint32_t>::max();
			for (const Position& target : targets) cost = std::min(cost, potential->second.estimate(target));
			bound.seconds = cost / 10 * secondsPerHop;
		}
		const auto* from = reachabilityFrom(start);
		if (!from || targets.empty() || std::any_of(targets.begin(), targets.end(), [&](const Position& target) {
			    return !topology.walkNode(target) || topology.reachable(*from, target);
		    })) return bound;
		const auto& offers = *search.travelOffers;
		std::vector<bool> fromStart(offers.size()), nearTarget(offers.size());
		for (size_t i = 0; i < offers.size(); ++i) {
			fromStart[i] = boardable(*from, offers[i].provider);
			const auto* landing = reachabilityFrom(offers[i].destination);
			nearTarget[i] = !landing || std::any_of(targets.begin(), targets.end(), [&](const Position& target) {
				return !topology.walkNode(target) || topology.reachable(*landing, target);
			});
		}
		const auto fare = playerbot::playerBotSellLootMinimumFare(search.travelGraph, false, fromStart, nearTarget);
		if (!fare) return std::nullopt;
		bound.fare = *fare;
		return bound;
	};
	// Bound every candidate on the graph before any sliced route search, then
	// validate only optimistic profits, best first. Each potential is one
	// map-sized relaxed search, so build at most one per decision.
	bool boundingPending = false;
	if (!search.bounded) {
		std::vector<Position> starts{position};
		for (const auto& candidate : search.candidates) {
			if (candidate.sourceDepotId != currentDepotId) starts.push_back(candidate.sourceApproach);
		}
		const auto missing = std::find_if(starts.begin(), starts.end(), [&](const Position& start) {
			return !search.potentials.count(start);
		});
		if (missing != starts.end()) {
			std::vector<PlayerBotShortcut> travel;
			for (const auto& offer : *search.travelOffers) {
				for (int32_t x = -1; x <= 1; ++x) for (int32_t y = -1; y <= 1; ++y) {
					travel.push_back({Position(offer.provider.x + x, offer.provider.y + y, offer.provider.z), offer.destination});
				}
			}
			search.potentials.emplace(*missing, topology.heuristicFrom(*missing, travel));
			boundingPending = true;
		} else {
			std::vector<SellLootCandidate> kept;
			for (auto& candidate : search.candidates) {
				const Position sellerStart = candidate.sourceDepotId == currentDepotId ? position : candidate.sourceApproach;
				std::optional<LegBound> source = LegBound{};
				if (candidate.sourceDepotId != currentDepotId) source = legBound(position, {candidate.sourceApproach});
				const auto seller = source ? legBound(sellerStart, approachPositions(candidate.approach)) : std::nullopt;
				if (!source || !seller) {
					emitRejected(candidate, !source ? "source_route_unavailable" : "seller_route_unavailable", 0, 0, 0,
					             std::string(!source ? ",\"source_result\"" : ",\"seller_result\"") +
					                 ":\"no_affordable_travel\",\"pruned\":true");
					++search.pruned;
					continue;
				}
				const uint64_t fare = source->fare + seller->fare;
				candidate.tripCostBound = fare + static_cast<uint64_t>(std::ceil(
				    (source->seconds + seller->seconds) * sellLootTravelTimeGoldPerMinute / 60.0));
				const auto bound = playerBotSellLootPrefilter({candidate.revenue, candidate.tripCostBound, fare,
				                                               spendableFare, 0, survivalSell});
				if (bound != SellLootPrefilterResult::NeedsRouteValidation) {
					emitRejected(candidate, bound == SellLootPrefilterResult::UnaffordableFare ?
					                 "upfront_fare_unavailable" : "optimistic_trip_unprofitable",
					             fare, candidate.tripCostBound, 0, ",\"pruned\":true");
					++search.pruned;
					continue;
				}
				kept.push_back(std::move(candidate));
			}
			std::stable_sort(kept.begin(), kept.end(), [](const SellLootCandidate& left, const SellLootCandidate& right) {
				const int64_t leftNet = static_cast<int64_t>(left.revenue) - static_cast<int64_t>(left.tripCostBound);
				const int64_t rightNet = static_cast<int64_t>(right.revenue) - static_cast<int64_t>(right.tripCostBound);
				return leftNet != rightNet ? leftNet > rightNet : left.revenue > right.revenue;
			});
			search.candidates = std::move(kept);
			search.bounded = true;
		}
	}
	const auto candidateCount = search.candidates.size();
	for (size_t index = 0; !boundingPending && index < maximumSellLootRouteValidationsPerDecision &&
	                       search.next < candidateCount;) {
		auto& candidate = search.candidates[search.next];
		auto resetRoutes = [&]() {
			search.routeWork.reset();
			search.sourceRoute.reset();
			search.sourceWalkingRoute.reset();
			search.routeWatch = {};
			search.routeFacts.clear();
			search.sourceTravelPositions.clear();
			++search.sequence;
		};
		auto reject = [&](const char* reason, uint64_t fare = 0, uint64_t tripCost = 0, uint32_t danger = 0,
		                  const std::string& extra = {}) {
			emitRejected(candidate, reason, fare, tripCost, danger, extra);
			++search.next;
			search.invalidationRetries = 0;
			resetRoutes();
		};
		const Position sellerStart = candidate.sourceDepotId == currentDepotId ? position : candidate.sourceApproach;
		if (const auto cached = search.unavailableSources.find(candidate.sourceDepotId);
		    cached != search.unavailableSources.end()) {
			reject("source_route_unavailable", 0, 0, 0, cached->second + ",\"cached\":true");
			continue;
		}
		if (const auto known = search.sourceCosts.find(candidate.sourceDepotId); known != search.sourceCosts.end()) {
			const auto [cost, fare] = known->second;
			const auto result = playerBotSellLootPrefilter({candidate.revenue, cost, fare, spendableFare, 0, survivalSell});
			if (result != SellLootPrefilterResult::NeedsRouteValidation) {
				reject(result == SellLootPrefilterResult::UnaffordableFare ? "upfront_fare_unavailable" :
				       "source_cost_exceeds_revenue", fare, cost, 0, ",\"cached\":true");
				continue;
			}
		}
		++index;
		++routeValidations;
		// No NPC, Item, or ShopInfo pointer is retained across dispatcher turns.
		Npc* seller = g_game.getNpcByID(candidate.providerId);
		DepotChest* chest = player.getDepotChest(candidate.sourceDepotId, false);
		if (!seller || seller->isRemoved() || !chest ||
		    !playerBotNpcHasCapability(*seller, PlayerBotNpcCapability::Shop)) {
			reject("provider_or_depot_changed");
			continue;
		}
		bool sourcePresent = false;
		for (int x = -1; x <= 1; ++x) for (int y = -1; y <= 1; ++y) {
			uint16_t lockerItemId = 0;
			sourcePresent |= findDepotLocker(Position(candidate.sourceApproach.x + x, candidate.sourceApproach.y + y,
			                                          candidate.sourceApproach.z), candidate.sourceDepotId, lockerItemId);
		}
		if (!sourcePresent) { reject("depot_locker_changed"); continue; }
		Item* backpackItem = player.getInventoryItem(CONST_SLOT_BACKPACK);
		Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
		bool manifestValid = backpack && sellLootManifestFits(player, *chest, *backpack, candidate.batches);
		for (const auto& batch : candidate.batches) {
			const auto& offers = seller->getShopOffers();
			manifestValid &= std::any_of(offers.begin(), offers.end(), [&](const ShopInfo& offer) {
				return offer.itemId == batch.itemId && offer.sellPrice == batch.price &&
				       static_cast<uint8_t>(offer.subType) == batch.subType;
			});
		}
		if (!manifestValid) {
			reject("manifest_changed");
			continue;
		}
		auto refreshApproaches = [&]() {
			candidate.providerPosition = seller->getPosition();
			const auto distances = topology.distancesFrom(sellerStart, canUseRope, canUseShovel, player.getLevel());
			candidate.approach.offer(conversationApproaches(sellerStart, candidate.providerPosition, distances));
		};
		if (!Position::areInRange<3, 3, 0>(candidate.providerApproach, seller->getPosition())) {
			refreshApproaches();
			resetRoutes();
			++search.invalidationRetries;
			const std::optional<Position> liveApproach = candidate.approach.next();
			if (!liveApproach) { reject("provider_approach_unavailable"); continue; }
			candidate.providerApproach = *liveApproach;
		}
		// The engine revalidates its pending leg. This enclosing watch and actor/
		// offer fingerprint also protect a COMPLETED source leg while the seller
		// leg yields. Do not confuse topology generation with live tile validity.
		const auto combat = equipmentPolicy.combatProfile(PlayerBotEquipmentAdapter::player(player),
		                                                 PlayerBotEquipmentAdapter::loadout(player));
		std::ostringstream facts;
		facts.precision(17);
		facts << combat.level << ':' << combat.maximumHealth << ':' << combat.armor << ':' << combat.defense
		      << ':' << combat.attack << ':' << combat.attackSkill << ':' << combat.attackFactor
		      << ':' << combat.hitChance << ':' << combat.blockedByShield
		      << ':' << player.getMoney() << ':' << player.getBankBalance() << ':' << player.isPremium()
		      << ':' << player.isPzLocked() << ':' << player.getStepDuration() << ':' << player.getStepDuration(DIRECTION_NORTHEAST)
		      << ':' << (g_game.findItemOfType(&player, ropeItemId, true) != nullptr)
		      << ':' << (g_game.findItemOfType(&player, 2554, true) != nullptr)
		      << ':' << (player.getGroup() ? player.getGroup()->flags : 0)
		      << ':' << (player.getGroup() && player.getGroup()->access)
		      << ':' << (player.getGuild() ? player.getGuild()->getId() : 0)
		      << ':' << (player.getGuildRank() ? player.getGuildRank()->id : 0)
		      << ':' << fareReserve << ':' << PlayerBotTopology::instance().generation()
		      << ':' << PlayerBotHuntRegionPlanner::getCacheRevision();
		std::map<uint32_t, std::string> travelFacts;
		std::map<uint32_t, Position> travelPositions;
		for (const auto& entry : g_game.getNpcs()) {
			Npc* npc = entry.second;
			if (!npc || npc->isRemoved() || npc->getTravelOffers().empty()) continue;
			std::ostringstream offers;
			// Movement invalidates the pending connection inside the route engine,
			// not unrelated walking work. Completed paid evidence is checked below.
			for (const auto& offer : npc->getTravelOffers()) {
				offers << ':' << offer.destination << ':' << offer.price << ':' << offer.level << ':' << offer.premium
				       << ':' << offer.hasOpaqueCondition << ':' << offer.hasOpaqueAction;
				for (const auto& word : offer.dialogue) offers << ':' << word.size() << ':' << word;
				const auto unavailable = unavailableTravelOffers.find({npc->getID(), offer.destination});
				offers << ':' << (unavailable != unavailableTravelOffers.end() && unavailable->second > began);
			}
			travelFacts.emplace(npc->getID(), offers.str());
			travelPositions.emplace(npc->getID(), npc->getPosition());
		}
		for (const auto& entry : travelFacts) facts << ':' << entry.first << ':' << entry.second;
		std::ostringstream anchors;
		for (const auto& entry : travelPositions) anchors << entry.first << ':' << entry.second << ';';
		const auto liveTravelPositions = anchors.str();
		const auto liveFacts = facts.str();
		if (!search.routeWatch.valid() || (!search.routeFacts.empty() && search.routeFacts != liveFacts)) {
			resetRoutes();
			++search.invalidationRetries;
			++timing.invalidations;
		}
		// Paid estimates expose only their first boarding marker, not every
		// connection. Conservatively invalidate a completed paid source leg on
		// any transport anchor change; never mistake that marker for a full path.
		if (search.sourceRoute && usesTravel(*search.sourceRoute) && search.sourceTravelPositions != liveTravelPositions) {
			resetRoutes();
			++search.invalidationRetries;
			++timing.invalidations;
		}
		if (search.invalidationRetries > 3) {
			reject("route_changed_repeatedly");
			continue;
		}
		search.routeFacts = liveFacts;
		PlayerBotRouteChanges::Scope watch(search.routeWatch);
		if (!search.sourceRoute) {
			if (candidate.sourceDepotId == currentDepotId) {
				search.sourceRoute.emplace();
				search.sourceRoute->metrics.result = PlayerBotNavigationResult::Reached;
			} else {
				search.sourceWalkingRoute.emplace();
				search.sourceRoute = chooseRoute(position, candidate.sourceApproach, &*search.sourceWalkingRoute);
				if (!search.sourceRoute) break;
				// Never run two route slices in one dispatcher turn.
				if (safe(*search.sourceRoute)) {
					search.sourceTravelPositions = liveTravelPositions;
					++search.sequence;
					break;
				}
			}
		}
		if (!safe(*search.sourceRoute)) {
			const auto& metrics = search.sourceRoute->metrics;
			const auto result = metrics.result;
			const std::string cause = ",\"source_result\":" + jsonString(
			    result == PlayerBotNavigationResult::Reached ? "unsafe" :
			    result == PlayerBotNavigationResult::NodeLimit ? "incomplete" : "unreachable") +
			    ",\"source_maximum_health_loss_per_second\":" + std::to_string(metrics.maximumHealthLossPerSecond);
			const uint64_t fare = metrics.fare;
			const uint32_t danger = metrics.dangerCost;
			search.unavailableSources.emplace(candidate.sourceDepotId, cause);
			reject("source_route_unavailable", fare, 0, danger, cause);
			continue;
		}
		const auto& sourceMetrics = search.sourceRoute->metrics;
		const uint64_t sourceCost = sourceMetrics.fare + sourceMetrics.dangerCost / 10 +
		    static_cast<uint64_t>(std::ceil(sourceMetrics.estimatedTravelSeconds * sellLootTravelTimeGoldPerMinute / 60.0));
		// Once the source leg is fixed, its cost is unavoidable for this plan.
		// Geometry alone is NOT a bound: free NPC travel/portals can defeat it.
		if (candidate.sourceDepotId != currentDepotId) {
			search.sourceCosts.emplace(candidate.sourceDepotId, std::make_pair(sourceCost, sourceMetrics.fare));
		}
		const auto prefilter = playerBotSellLootPrefilter({candidate.revenue, sourceCost, sourceMetrics.fare,
		    spendableFare, 0, survivalSell});
		if (prefilter != SellLootPrefilterResult::NeedsRouteValidation) {
			reject(prefilter == SellLootPrefilterResult::UnaffordableFare ? "upfront_fare_unavailable" : "source_cost_exceeds_revenue",
			       sourceMetrics.fare, sourceCost);
			continue;
		}
		auto sellerPlan = chooseRoute(candidate.sourceDepotId == currentDepotId ? position : candidate.sourceApproach,
		                              candidate.providerApproach, nullptr);
		if (!sellerPlan) break;
		if (!safe(*sellerPlan)) {
			const Position failedApproach = candidate.providerApproach;
			const auto result = sellerPlan->metrics.result;
			const bool providerMoved = candidate.providerPosition != seller->getPosition();
			// A failed transport search may instead have failed on its last walking
			// leg. Try the other speech tiles before rejecting the seller. An
			// exhausted or unsafe search fails the same way for every tile, and a
			// wandering seller is retargeted only within the shared bound.
			PlayerBotApproachVerdict verdict = PlayerBotApproachVerdict::Exhausted;
			if (providerMoved) {
				verdict = candidate.approach.providerMoved();
				if (verdict == PlayerBotApproachVerdict::Continue) refreshApproaches();
			}
			// After a move the failed tile may still be in range; never retry it.
			if (providerMoved ? verdict == PlayerBotApproachVerdict::Continue : result == PlayerBotNavigationResult::Unreachable) {
				verdict = candidate.approach.reject(failedApproach, providerMoved ? "provider_moved" : "route_unavailable");
			}
			const std::optional<Position> nextApproach = verdict == PlayerBotApproachVerdict::Continue ?
			    candidate.approach.next() : std::nullopt;
			if (nextApproach) {
				candidate.providerApproach = *nextApproach;
				search.routeWork.reset();
				++search.sequence;
				emitApproachResult("sell_loot", "retry", candidate.approach.reason(), position, candidate.providerId,
				                   *nextApproach, ",\"approach_index\":" + std::to_string(approachIndex(candidate)) +
				                       ",\"approach_count\":" + std::to_string(candidate.approach.offered().size()));
				break;
			}
			emitApproachResult("sell_loot", "failed", result == PlayerBotNavigationResult::Unreachable || providerMoved ?
			                   candidate.approach.reason() : result == PlayerBotNavigationResult::NodeLimit ?
			                   "route_incomplete" : "route_unsafe", position, candidate.providerId, failedApproach);
			const std::string cause = ",\"seller_result\":" + jsonString(
			    result == PlayerBotNavigationResult::Reached ? "unsafe" :
			    result == PlayerBotNavigationResult::NodeLimit ? "incomplete" : "unreachable");
			reject("seller_route_unavailable", sellerPlan->metrics.fare, 0, sellerPlan->metrics.dangerCost, cause);
			continue;
		}
		bool sourceTravelUsed = usesTravel(*search.sourceRoute);
		const bool sellerTravelUsed = usesTravel(*sellerPlan);
		// Both legs execute through this same sliced engine, so its quotes are the
		// executable routes. Each leg checked fares alone; when two paid legs do not
		// fit together, fall back to the validated walking source.
		if (sourceTravelUsed && sellerTravelUsed && search.sourceWalkingRoute && safe(*search.sourceWalkingRoute) &&
		    !huntTravelFareAffordable(player, search.sourceRoute->metrics.fare + sellerPlan->metrics.fare, huntTravelBudgetPhase)) {
			*search.sourceRoute = *search.sourceWalkingRoute;
			sourceTravelUsed = false;
		}
		auto sourceRoute = std::pair{std::move(*search.sourceRoute), sourceTravelUsed};
		auto sellerRoute = std::pair{std::move(*sellerPlan), sellerTravelUsed};
		const uint64_t fare = sourceRoute.first.metrics.fare + sellerRoute.first.metrics.fare;
		if (!huntTravelFareAffordable(player, fare, huntTravelBudgetPhase)) {
			reject("upfront_fare_unavailable", fare);
			continue;
		}
		const double travelSeconds = sourceRoute.first.metrics.estimatedTravelSeconds +
		                             sellerRoute.first.metrics.estimatedTravelSeconds;
		const uint64_t timeCost = static_cast<uint64_t>(std::ceil(
			travelSeconds * sellLootTravelTimeGoldPerMinute / 60.0));
		const uint32_t danger = static_cast<uint32_t>(std::min<uint64_t>(
			static_cast<uint64_t>(sourceRoute.first.metrics.dangerCost) + sellerRoute.first.metrics.dangerCost,
			std::numeric_limits<uint32_t>::max()));
		if (!playerBotNavigationRiskAccepts(riskProfile, danger,
		    std::max(sourceRoute.first.metrics.maximumHealthLossPerSecond,
		             sellerRoute.first.metrics.maximumHealthLossPerSecond))) {
			reject("combined_route_unsafe", fare, 0, danger);
			continue;
		}
		const uint64_t tripCost = fare + timeCost + danger / 10;
		if (!survivalSell && candidate.revenue <= tripCost) {
			reject("non_positive_utility", fare, tripCost, danger);
			continue;
		}
		SellLootPlan plan;
		plan.sourceDepotId = candidate.sourceDepotId;
		plan.providerId = candidate.providerId;
		plan.survivalSell = survivalSell;
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
		plan.utility = static_cast<int64_t>(candidate.revenue) - static_cast<int64_t>(tripCost);
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
		selectedProviderApproach = candidate.providerApproach;
		selectedApproachIndex = approachIndex(candidate);
		found = true;
		break;
	}
	sellLootSearchPending = !found && (boundingPending || search.next < candidateCount);
	// Each slice is already bounded and charged to the shared budget. Waiting
	// a full movement tick between slices made scans take minutes of wall time.
	if (sellLootSearchPending && retryAfter) *retryAfter = std::chrono::milliseconds(1);
	const auto candidateIndex = search.next;
	const bool bounded = search.bounded;
	const size_t pruned = search.pruned;
	if (!sellLootSearchPending) sellLootSearch.reset();
	// A candidate's route search runs one slice per turn. Merge those pending
	// calls until the scan reaches another candidate, finishes, or ages out.
	if (rebuilt) sellLootPlanReport.reset();
	const auto now = std::chrono::steady_clock::now();
	const int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(now - began).count();
	SellLootPlanReport& report = sellLootPlanReport ? *sellLootPlanReport :
	    sellLootPlanReport.emplace(SellLootPlanReport{candidateIndex, began});
	++report.slices;
	report.elapsedUs += elapsedUs;
	report.maxElapsedUs = std::max(report.maxElapsedUs, elapsedUs);
	report.snapshotUs += snapshotUs;
	report.rebuilt = report.rebuilt || rebuilt;
	report.routes.add(timing);
	if (sellLootSearchPending && candidateIndex == report.candidateIndex &&
	    now - report.started < progressRecordInterval) return found;
	const SellLootPlanReport merged = std::move(report);
	sellLootPlanReport.reset();
	std::ostringstream fields;
	fields << "\"action\":\"sell_loot_plan\",\"result\":" << jsonString(found ? "candidate" : "deferred")
	       << ",\"reason\":" << jsonString(found ? (survivalSell ? "survival_trip_validated" : "profitable_trip_validated") :
	           sellLootSearchPending ? "candidate_scan_pending" : "no_profitable_trip")
	       << ",\"top_level_items\":" << scannedItems
	       << ",\"snapshot_rebuilt\":" << (merged.rebuilt ? "true" : "false")
	       << ",\"candidate_count\":" << candidateCount << ",\"candidate_index\":" << candidateIndex
	       << ",\"bounded\":" << (bounded ? "true" : "false") << ",\"pruned_candidates\":" << pruned
	       << ",\"slices\":" << merged.slices << ",\"snapshot_us\":" << merged.snapshotUs
	       << ",\"walking_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(merged.routes.walkingTime).count()
	       << ",\"npc_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(merged.routes.npcTime).count()
	       << ",\"route_expanded_nodes\":" << merged.routes.localExpandedNodes << ",\"route_yields\":" << merged.routes.yields
	       << ",\"route_invalidations\":" << merged.routes.invalidations
	       << ",\"route_unknown_connections\":" << merged.routes.unknownConnections
	       << ",\"elapsed_us\":" << merged.elapsedUs << ",\"max_elapsed_us\":" << merged.maxElapsedUs
	       << ",\"route_validations\":" << routeValidations
	       << ",\"route_validation_budget\":" << maximumSellLootRouteValidationsPerDecision
	       << (survivalSell ? ",\"survival\":true" : "");
	if (found) {
		const SellLootPlan& plan = *sellLootPlan;
		fields << ",\"source_depot_id\":" << plan.sourceDepotId << ",\"npc_id\":" << plan.providerId
		       << ",\"manifest_batches\":" << plan.batches.size() << ",\"item_batch_budget\":" << maximumSellLootBatch
		       << ",\"route_steps\":" << plan.routeSteps << ",\"expected_revenue\":" << plan.expectedRevenue
		       << ",\"liquidity_urgency\":0,\"fare\":" << plan.fare << ",\"round_trip_risk\":" << plan.roundTripRisk
		       << ",\"round_trip_time_cost\":" << plan.roundTripTime << ",\"foregone_hunt_profit\":0,\"utility\":" << plan.utility
		       << ",\"provider_approach_index\":" << selectedApproachIndex
		       << ",\"provider_approach\":{\"x\":" << selectedProviderApproach.x
		       << ",\"y\":" << selectedProviderApproach.y << ",\"z\":" << uint32_t(selectedProviderApproach.z) << '}'
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
	if (!sellLootManifestFits(player, *chest, *backpack, plan.batches)) {
		deferSellLoot(player, position, "withdraw_capacity_changed");
		return true;
	}
	SellLootBatch& batch = plan.batches[plan.withdrawalBatch];
	if (!player.canDoAction() || !openContainer(player, *backpack, depotSourceContainerId, position)) return true;
	Item* source = nullptr;
	for (Item* item : chest->getItemList()) {
		if (item->getID() == batch.itemId && !item->getContainer() &&
		    !inventoryPolicy.isProtectedDepositItem(player, *item)) { source = item; break; }
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
	const uint8_t destinationIndex = containerDestinationIndex(*backpack, *source);
	uint32_t count = std::min<uint32_t>({batch.count - batch.withdrawn, source->getItemCount(), UINT8_MAX});
	if (source->isStackable()) {
		Item* destination = backpack->getItemByIndex(destinationIndex);
		if (destination && destination->equals(source)) count = std::min<uint32_t>(count, 100 - destination->getItemCount());
	}
	plan.withdrawalInventoryBefore = inventoryPolicy.inventoryItemCount(player, batch.itemId);
	plan.withdrawalDepotBefore = chest->getItemTypeCount(batch.itemId);
	plan.withdrawalRequested = count;
	plan.withdrawalPending = true;
	telemetry.recordActionAttempt();
	g_game.playerMoveItem(&player, Position(0xFFFF, 0x40 | depotChestContainerId, static_cast<uint8_t>(sourceIndex)),
		source->getClientID(), static_cast<uint8_t>(sourceIndex),
		Position(0xFFFF, 0x40 | static_cast<uint8_t>(backpackId), destinationIndex), static_cast<uint8_t>(count), source, backpack);
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
	if (phase == CyclePhase::ReturnToDepot) {
		huntTravelBudgetPhase = HuntTravelBudgetPhase::ReturnToDepot;
		lastDepotDiscoveryHealth = 0;
		depotDiscoveryUnderAttack = false;
	}
	if (phase == CyclePhase::Hunt) {
		huntTravelBudgetPhase = HuntTravelBudgetPhase::None;
		huntExitFareReserve = 0;
		huntRecoveryPotionReserve = 0;
		huntReturnCoverageVariantId = 0;
		huntReturnDestination = Position();
		huntExitValidationAttempted = false;
		huntReturnCoverage.invalidate();
		huntPatrolTrip.reset();
		huntTransitProgress.reset();
	}
	const char* previous = cyclePhaseName();
	if (turnRouter.cyclePhase() == CyclePhase::Hunt && phase != CyclePhase::Hunt) {
		cancelHuntPlanning(reason, position);
	}
	if (turnRouter.cyclePhase() == CyclePhase::Service && phase != CyclePhase::Service) {
		serviceRouteSearch.reset();
		playerBotHuntPlanningBudget().cancel(playerId, std::chrono::steady_clock::now());
	}
	if (turnRouter.cyclePhase() == CyclePhase::ReturnToDepot && phase != CyclePhase::ReturnToDepot) {
		if (phase != CyclePhase::DepositLoot) {
			emitDepotDiscovery(playerbot::PlayerBotDepotTelemetry::Result::Cancelled, position, reason);
		}
		depotSourceRouteSearch.reset();
	}
	if (phase == CyclePhase::Hunt || phase == CyclePhase::DepositLoot) {
		huntCoordinator.finishDangerRetreat();
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
	emitDepotDiscovery(playerbot::PlayerBotDepotTelemetry::Result::Cancelled, position, reason);
	huntTravelBudgetPhase = HuntTravelBudgetPhase::ReturnToDepot;
	huntExitValidationAttempted = false;
	// The selected exit depot belongs to the hunt area. A hunt that ends before
	// reaching it returns to the nearest depot through the normal scan.
	if (!huntRegionReached) huntReturnDestination = Position();
	lastDepotDiscoveryHealth = 0;
	depotDiscoveryUnderAttack = false;
	pendingHuntCompletionReason.clear();
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
	updateSupplyRecovery(*player, position);
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
	serviceWorkflow.setSurvivalRestock(supplyRecovery.active());
	beginReturn(player, position, reason);
}

void PlayerBotController::updateSupplyRecovery(const Player& player, const Position& position)
{
	const uint64_t funds = player.getMoney() + player.getBankBalance();
	const bool wasActive = supplyRecovery.active();
	supplyRecovery.restockBlocked(funds, playerBotSupplyStockKey(supplyStocks(player)));
	const uint64_t spending = supplyFloorSpendingReserve(player);
	const uint64_t budget = spending == UINT64_MAX ? spending : spending - carriedGoldReserve;
	supplyRecovery.update(funds, budget);
	if (wasActive == supplyRecovery.active()) return;
	huntCoordinator.setSupplyRecovery(supplyRecovery.active());
	serviceWorkflow.setSurvivalRestock(supplyRecovery.active());
	emit("action_result", position,
	     std::string("\"action\":\"supply_recovery\",\"result\":\"") + (supplyRecovery.active() ? "started" : "ended") +
	         "\",\"funds\":" + std::to_string(funds) + ",\"potion_budget\":" +
	         (budget == std::numeric_limits<uint64_t>::max() ? std::string("\"unknown\"") : std::to_string(budget)));
	if (Player* speakingPlayer = g_game.getPlayerByID(playerId)) {
		say(*speakingPlayer, supplyRecovery.active() ?
		    "Supply recovery started; prioritizing funds and safe restock." :
		    "Supply recovery complete; normal goals resumed.");
	}
}

void PlayerBotController::enterSupplyRecovery(const Position& position, uint64_t funds, uint64_t potionBudget,
                                              const char* reason)
{
	if (supplyRecovery.enter()) {
		huntCoordinator.setSupplyRecovery(true);
		serviceWorkflow.setSurvivalRestock(true);
	}
	emit("action_result", position,
	     std::string("\"action\":\"supply_recovery\",\"result\":\"started\",\"reason\":") + jsonString(reason) +
	         ",\"funds\":" + std::to_string(funds) + ",\"potion_budget\":" +
	         (potionBudget == std::numeric_limits<uint64_t>::max() ? std::string("\"unknown\"") : std::to_string(potionBudget)));
	if (Player* speakingPlayer = g_game.getPlayerByID(playerId)) {
		say(*speakingPlayer, "Supply recovery started: " + std::string(reason) + '.');
	}
}

void PlayerBotController::finishHuntAndReturn(Player* player, const Position& position, const char* reason)
{
	const bool failedArrival = huntCoordinator.huntActive() && !huntCoordinator.huntArrived();
	if (failedArrival && std::strcmp(reason, "hunt_deadline") == 0) reason = "hunt_arrival_timeout";
	if (std::strcmp(reason, "hunt_arrival_timeout") == 0)
		huntCoordinator.observeHuntArrivalTimeout(std::chrono::steady_clock::now());
	finishHuntRegion(*player, position, reason);
	emit("goal_result", position,
	     "\"decision_id\":" + std::to_string(progressionRuntime.decisionId()) +
	         ",\"goal\":\"hunt\",\"result\":" + jsonString(failedArrival ? "failed" : "success") +
	         ",\"reason\":" + jsonString(reason));
	beginReturn(player, position, reason);
	pendingHuntCompletionReason = reason;
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
	observation.supplyCapacityReserve = returnCapacityThreshold;
	observation.money = player->getMoney();
	observation.bankBalance = player->getBankBalance();
	observation.goldCoinWeight = Item::items[ITEM_GOLD_COIN].weight;
	const PlayerBotSupplyStocks stocks = supplyStocks(*player);
	for (const PlayerBotSupplyStock& stock : stocks) {
		observation.supplies.push_back({stock.rule.kind, stock.rule.itemId, Item::items[stock.rule.itemId].weight,
		                                stock.rule.returnThreshold, stock.rule.target, stock.rule.safetyFloor});
	}
	Item* serviceBackpackItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
	Container* serviceBackpack = serviceBackpackItem ? serviceBackpackItem->getContainer() : nullptr;
	observation.actionAvailable = player->canDoAction();
	observation.backpackAvailable = serviceBackpack != nullptr;
	observation.backpackOpen = serviceBackpack && player->getContainerID(serviceBackpack) >= 0;
	observation.maximumAttempts = maximumServiceAttempts;
	observation.slottedSaleCooldownMs = static_cast<uint32_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(unavailableDispositionCooldown).count());
	std::set<uint16_t> relevantItemIds;
	for (const PlayerBotServiceSupply& supply : observation.supplies) relevantItemIds.insert(supply.itemId);
	if (sellingLocalLoot) {
		for (const PlayerBotServiceLiquidationBatch& batch : serviceWorkflow.liquidation()->batches) {
			observation.inventoryCounts.emplace(batch.itemId, inventoryPolicy.inventoryItemCount(*player, batch.itemId));
			observation.backpackSaleCounts.emplace(batch.itemId, inventoryPolicy.backpackSaleItemCount(*player, batch.itemId));
			relevantItemIds.insert(batch.itemId);
		}
	}
	for (const PlayerBotServiceSupply& supply : observation.supplies) {
		observation.inventoryCounts.emplace(supply.itemId, inventoryPolicy.inventoryItemCount(*player, supply.itemId));
		observation.backpackSaleCounts.emplace(supply.itemId, inventoryPolicy.backpackSaleItemCount(*player, supply.itemId));
	}
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
			const bool neededPurchase = offer.buyPrice != 0 && std::any_of(observation.supplies.begin(), observation.supplies.end(),
			    [&offer](const PlayerBotServiceSupply& supply) { return supply.itemId == offer.itemId; });
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
	std::shared_ptr<const PlayerBotTopologyReachability> walkReachability;
	bool walkReachabilityObserved = false;
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
				// Occupied tiles stay eligible as a last resort (see playerbotapproach.h).
				if (playerBotStableApproachTile(tile, *player)) {
					const Creature* occupant = tile->getTopCreature();
					providerObservation.approaches.push_back({approach, static_cast<uint32_t>(
						std::max(Position::getDistanceX(currentPosition, approach), Position::getDistanceY(currentPosition, approach))),
						occupant && occupant != player});
				}
			}
			// Tiles behind a shop counter are never walkable. When any tile is, skip
			// the rest instead of letting each one run a map-wide travel search.
			const auto& topology = PlayerBotTopology::instance();
			if (!walkReachabilityObserved) {
				walkReachabilityObserved = true;
				if (topology.walkNode(currentPosition)) {
					walkReachability = topology.reachabilityFrom(currentPosition,
					    g_game.findItemOfType(player, playerbot::ropeItemId, true) != nullptr,
					    g_game.findItemOfType(player, 2554, true) != nullptr, player->getLevel());
				}
			}
			if (walkReachability) {
				auto& approaches = providerObservation.approaches;
				auto walkable = [&](const PlayerBotServiceProviderObservation::Approach& approach) {
					return !topology.walkNode(approach.position) || topology.reachable(*walkReachability, approach.position);
				};
				if (std::any_of(approaches.begin(), approaches.end(), walkable)) {
					approaches.erase(std::remove_if(approaches.begin(), approaches.end(), [&](const auto& approach) {
						return !walkable(approach);
					}), approaches.end());
				}
			}
			std::sort(providerObservation.approaches.begin(), providerObservation.approaches.end(), [npc](const auto& left,
			                                                                                              const auto& right) {
				const uint32_t leftProviderDistance = std::max(Position::getDistanceX(npc->getPosition(), left.position),
				                                               Position::getDistanceY(npc->getPosition(), left.position));
				const uint32_t rightProviderDistance = std::max(Position::getDistanceX(npc->getPosition(), right.position),
				                                                Position::getDistanceY(npc->getPosition(), right.position));
				if (left.occupied != right.occupied) return !left.occupied;
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
	// A partial paid approach has no boarding step. Once its steps are consumed,
	// validate another full itinerary before taking the next leg.
	bool revalidatingSelectedApproach = false;
	if (command.type == PlayerBotServiceCommandType::NavigateProvider &&
	    !navigationRuntime.hasPendingWork() && command.destination != currentPosition) {
		Npc* provider = g_game.getNpcByID(command.providerId);
		if (provider && !Position::areInRange<3, 3, 0>(currentPosition, provider->getPosition())) {
			command.type = PlayerBotServiceCommandType::ValidateProviderRoute;
			revalidatingSelectedApproach = true;
		}
	}
	// Keep a sliced search when an exhausted approach needs validation again.
	if (command.type != PlayerBotServiceCommandType::ValidateProviderRoute && serviceRouteSearch) {
		serviceRouteSearch.reset();
		playerBotHuntPlanningBudget().cancel(playerId, std::chrono::steady_clock::now());
	}
	while (command.type == PlayerBotServiceCommandType::ValidateProviderRoute && routeValidations < 1) {
		++routeValidations;
		const uint32_t routeProviderId = command.providerId;
		const Position routeDestination = command.destination;
		PlayerBotServiceObservation routeObservation = observation;
		routeObservation.approachRoute.providerId = routeProviderId;
		routeObservation.approachRoute.destination = command.destination;
		const auto startedAt = std::chrono::steady_clock::now();
		PlayerBotNavigationRoutePlan routePlan;
		bool validatedNpcTravel = false;
		// Failed steps mark their tile blocked for a while, usually a wandering
		// NPC or animal. The planner ignores creatures, so route around them here.
		const std::set<Position> blockedPositions = navigationRuntime.activeBlockedPositions(startedAt);
		if (command.destination == currentPosition && serviceRouteSearch) {
			serviceRouteSearch.reset();
			playerBotHuntPlanningBudget().cancel(playerId, startedAt);
		}
		if (command.destination != currentPosition) {
			const bool travelAllowed = !sellingLocalLoot || serviceWorkflow.liquidation()->allowNpcTravel;
			if (!travelAllowed) {
				if (serviceRouteSearch) {
					serviceRouteSearch.reset();
					playerBotHuntPlanningBudget().cancel(playerId, startedAt);
				}
				routePlan = planCompleteNavigationRoute(*player, command.destination);
			} else {
				const uint64_t reserve = std::max(huntTravelRecoveryFundsReserve(*player, huntTravelBudgetPhase),
				                                  huntTravelReturnFareReserve(huntTravelBudgetPhase));
				const bool sellEconomy = sellingLocalLoot;
				const char* restartReason = !serviceRouteSearch ? nullptr :
				    serviceRouteSearch->origin != currentPosition ? "origin" :
				    serviceRouteSearch->destination != command.destination ? "destination" :
				    serviceRouteSearch->providerId != command.providerId ? "provider" :
				    serviceRouteSearch->reserve != reserve ? "reserve" :
				    serviceRouteSearch->sellEconomy != sellEconomy ? "economy" : nullptr;
				if (!serviceRouteSearch || restartReason) {
					if (restartReason) {
						emit("service_route_restarted", currentPosition,
						     "\"source\":\"service\",\"reason\":" + jsonString(restartReason) +
						         ",\"npc_id\":" + std::to_string(command.providerId) +
						         ",\"turns\":" + std::to_string(serviceRouteSearch->turns));
					}
					serviceRouteSearch.emplace();
					auto& search = *serviceRouteSearch;
					search.origin = currentPosition;
					search.destination = command.destination;
					search.providerId = command.providerId;
					search.reserve = reserve;
					search.sellEconomy = sellEconomy;
					search.pass = (uint64_t(1) << 62) | (uint64_t(playerId) << 24) | ++serviceRouteSerial;
				}
				auto& search = *serviceRouteSearch;
				// Share admission and the sliced route engine, but not the hunt's pending work.
				auto& budget = playerBotHuntPlanningBudget();
				const auto admission = budget.request(playerId, startedAt);
				if (!admission.admitted) {
					schedule(static_cast<uint32_t>(std::clamp<int64_t>(
					    std::chrono::duration_cast<std::chrono::milliseconds>(admission.wait).count(),
					    blockedRouteRetryInterval, 5000)));
					return;
				}
				PlayerBotPlanningBudget::Charge charge(budget, playerId);
				{
					struct WorkSlot {
						std::shared_ptr<PlayerBotHuntTravelWork>& hunt;
						std::shared_ptr<PlayerBotHuntTravelWork>& service;
						WorkSlot(decltype(hunt) hunt, decltype(service) service) : hunt(hunt), service(service) { hunt.swap(service); }
						~WorkSlot() { hunt.swap(service); }
					} slot(huntTravelWork, search.work);
					PlayerBotHuntRouteRequest request;
					request.planningPass = search.pass;
					// A nearby safe walk needs no search through every paid connection,
					// including on a sale: a fare-free walk cannot exceed a quoted fare.
					request.preferSafeWalking = playerBotNavigationDistance(currentPosition, command.destination) <= 128;
					request.from = currentPosition;
					request.to = command.destination;
					PlayerBotHuntRouteTiming timing;
					PlayerBotNavigationRoutePlan walking;
					auto planned = advanceHuntTravelRoute(*player, request, currentPosition, timing, reserve,
					                                      sellEconomy, &walking);
					if (timing.invalidations != 0 || timing.transportRestarts != 0 || timing.requestRestartLimits != 0 ||
					    timing.transportRestartLimits != 0) {
						emit("service_route_restarted", currentPosition,
						     "\"source\":\"route_engine\",\"reason\":" + jsonString(timing.invalidationReason) +
						         ",\"cause\":" + jsonString(timing.invalidationCause) +
						         ",\"tile_key\":" + std::to_string(timing.invalidationChangedTile) +
						         ",\"journal_delta\":" + std::to_string(timing.invalidationJournalDelta) +
						         ",\"watched_tiles\":" + std::to_string(timing.invalidationWatchedTiles) +
						         ",\"transport_restarts\":" + std::to_string(timing.transportRestarts) +
						         ",\"request_restart_limits\":" + std::to_string(timing.requestRestartLimits) +
						         ",\"transport_restart_limits\":" + std::to_string(timing.transportRestartLimits) +
						         ",\"npc_id\":" + std::to_string(command.providerId) +
						         ",\"turns\":" + std::to_string(search.turns));
					}
					if (!planned && ++search.turns < maximumRouteSearchTurns) {
						schedule(routeSearchContinuationInterval);
						return;
					}
					if (!planned) {
						routePlan.metrics.result = PlayerBotNavigationResult::NodeLimit;
					} else {
						routePlan = std::move(*planned);
						const auto paid = std::find_if(routePlan.steps.begin(), routePlan.steps.end(), [](const auto& step) {
							return step.action == PlayerBotNavigationAction::NpcTravel;
						});
						if (paid != routePlan.steps.end() && routePlan.metrics.result == PlayerBotNavigationResult::Reached &&
						    playerBotNavigationRiskVerdict(riskProfile, routePlan.metrics) == PlayerBotNavigationRiskVerdict::Accepted) {
							const auto* quote = sellingLocalLoot ? &*serviceWorkflow.liquidation() : nullptr;
							if (quote && (routePlan.metrics.fare > quote->maximumFare ||
							    (quote->maximumRouteSteps != 0 && routePlan.metrics.steps > quote->maximumRouteSteps))) {
								routePlan = std::move(walking);
							} else {
								// The sliced search returns its validated walk to the boarding NPC.
								validatedNpcTravel = true;
							}
						}
					}
				}
			}
			serviceRouteSearch.reset();
		}
		// A creature blocking a validated step (usually a wandering NPC or pet)
		// is not a route failure. Walk around it, or wait for it to move.
		bool routeBlocked = false;
		const auto firstTravel = std::find_if(routePlan.steps.begin(), routePlan.steps.end(), [](const auto& step) {
			return step.action == PlayerBotNavigationAction::NpcTravel;
		});
		if (command.destination != currentPosition && routePlan.metrics.result == PlayerBotNavigationResult::Reached &&
		    std::any_of(routePlan.steps.begin(), firstTravel, [&blockedPositions](const auto& step) {
			    return blockedPositions.count(step.target) || blockedPositions.count(step.expectedPosition);
		    })) {
			auto detour = planCompleteNavigationRoute(*player, command.destination, blockedPositions);
			if (detour.metrics.result == PlayerBotNavigationResult::Reached && !detour.steps.empty() &&
			    playerBotNavigationRiskVerdict(riskProfile, detour.metrics) == PlayerBotNavigationRiskVerdict::Accepted) {
				routePlan = std::move(detour);
				validatedNpcTravel = false;
			} else if (serviceWorkflow.approach().blocked() == PlayerBotApproachVerdict::Wait) {
				emitApproachResult("service", "waiting", "route_blocked", currentPosition, command.providerId,
				                   command.destination, ",\"waits\":" + std::to_string(serviceWorkflow.approach().blockedWaits()));
				schedule(1000);
				return;
			} else {
				routeBlocked = true;
			}
		}
		if (!routeBlocked) serviceWorkflow.approach().unblocked();
		const auto* quote = sellingLocalLoot ? &*serviceWorkflow.liquidation() : nullptr;
		const bool quoteAllowed = !quote || (routePlan.metrics.fare <= quote->maximumFare &&
		    (quote->maximumRouteSteps == 0 || routePlan.metrics.steps <= quote->maximumRouteSteps));
		const bool routeSafe = command.destination == currentPosition ||
		    playerBotNavigationRiskVerdict(riskProfile, routePlan.metrics) == PlayerBotNavigationRiskVerdict::Accepted;
		const bool routeAffordable = command.destination == currentPosition ||
		                             huntTravelFareAffordable(*player, routePlan.metrics.fare,
		                                                      huntTravelBudgetPhase);
		lastRouteResult = routeSafe && routeAffordable && quoteAllowed && !routeBlocked ?
		    routePlan.metrics.result : PlayerBotNavigationResult::Unreachable;
		lastRouteExpandedNodes = routePlan.metrics.expandedNodes;
		lastRouteSteps = routePlan.metrics.steps;
		const bool reached = command.destination == currentPosition ||
		                     (routePlan.metrics.result == PlayerBotNavigationResult::Reached && !routePlan.steps.empty() &&
		                      routeSafe && routeAffordable && quoteAllowed && !routeBlocked);
		const auto routeElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
		    std::chrono::steady_clock::now() - startedAt);
		telemetry.recordPathfinding(routeElapsed, reached);
		routeObservation.approachRoute.result = reached ? PlayerBotServiceRouteResult::Reached : PlayerBotServiceRouteResult::Unreachable;
		routeObservation.approachRoute.steps = routePlan.metrics.steps;
		routeObservation.approachRoute.expandedNodes = routePlan.metrics.expandedNodes;
		routeObservation.approachRoute.dangerCost = routePlan.metrics.dangerCost;
		routeObservation.approachRoute.fare = routePlan.metrics.fare;
		routeObservation.approachRoute.maximumDanger = routePlan.metrics.maximumHealthLossPerSecond;
		lastRouteRequiresNpcTravel = validatedNpcTravel || std::any_of(routePlan.steps.begin(), routePlan.steps.end(),
			[](const PlayerBotNavigationStep& step) { return step.action == PlayerBotNavigationAction::NpcTravel; });
		routeObservation.approachRoute.requiresNpcTravel = lastRouteRequiresNpcTravel;
		if (reached) approachSteps = std::move(routePlan.steps);
		else if (routePlan.metrics.result != PlayerBotNavigationResult::Reached) {
			huntCoordinator.observeTransitMovementFailure(currentPosition);
		}
		const auto* liquidation = sellingLocalLoot ? &*serviceWorkflow.liquidation() : nullptr;
		const char* routeReason = !reached ?
		    (routePlan.metrics.result == PlayerBotNavigationResult::NodeLimit ? "node_limit" :
		     routePlan.metrics.result != PlayerBotNavigationResult::Reached ? "route_unreachable" :
		     routeBlocked ? "route_blocked" : !routeSafe ? "route_unsafe" : !routeAffordable ? "fare_unaffordable" :
		     !quoteAllowed && liquidation && routePlan.metrics.fare > liquidation->maximumFare ? "fare_exceeds_quote" :
		     !quoteAllowed ? "route_steps_exceed_quote" : "no_executable_steps") :
		    liquidation && lastRouteRequiresNpcTravel && !liquidation->allowNpcTravel ? "npc_travel_not_quoted" :
		    liquidation && liquidation->maximumRouteSteps != 0 && routePlan.metrics.steps > liquidation->maximumRouteSteps ?
		        "route_steps_exceed_quote" :
		    liquidation && routePlan.metrics.fare > liquidation->maximumFare ? "fare_exceeds_quote" : "route_accepted";
		if (revalidatingSelectedApproach && !reached) {
			serviceWorkflow.rejectSelectedApproach();
			npcApproach = {};
			resetNavigation();
		}
		const uint64_t quotedFare = liquidation ? liquidation->maximumFare : 0;
		const uint32_t quotedSteps = liquidation ? liquidation->maximumRouteSteps : 0;
		const bool quotedNpcTravel = liquidation && liquidation->allowNpcTravel;
		command = serviceWorkflow.advance(routeObservation, economyCatalog, dispositionPolicy);
		if (sellingLocalLoot) {
			const char* nextCommand = command.type == PlayerBotServiceCommandType::Fail ? "fail" :
			    command.type == PlayerBotServiceCommandType::ValidateProviderRoute ? "validate_route" :
			    command.type == PlayerBotServiceCommandType::NavigateProvider ? "navigate" :
			    command.type == PlayerBotServiceCommandType::Wait ? "wait" : "other";
			std::ostringstream fields;
			fields << "\"action\":\"sell_loot_provider_route\",\"result\":"
			       << jsonString(std::strcmp(routeReason, "route_accepted") == 0 ? "accepted" : "rejected")
			       << ",\"reason\":" << jsonString(routeReason) << ",\"npc_id\":" << routeProviderId
			       << ",\"destination\":{\"x\":" << routeDestination.x << ",\"y\":" << routeDestination.y
			       << ",\"z\":" << static_cast<uint32_t>(routeDestination.z) << "}"
			       << ",\"waypoint\":{\"x\":" << (routeDestination == currentPosition ? currentPosition.x : routePlan.metrics.waypoint.x)
			       << ",\"y\":" << (routeDestination == currentPosition ? currentPosition.y : routePlan.metrics.waypoint.y)
			       << ",\"z\":" << static_cast<uint32_t>(routeDestination == currentPosition ? currentPosition.z : routePlan.metrics.waypoint.z) << "}"
			       << ",\"planner_result\":" << jsonString(routeDestination == currentPosition || routePlan.metrics.result == PlayerBotNavigationResult::Reached ? "reached" :
			           routePlan.metrics.result == PlayerBotNavigationResult::NodeLimit ? "node_limit" : "unreachable")
			       << ",\"route_steps\":" << routePlan.metrics.steps << ",\"expanded_nodes\":" << routePlan.metrics.expandedNodes
			       << ",\"fare\":" << routePlan.metrics.fare << ",\"quoted_fare\":" << quotedFare
			       << ",\"quoted_steps\":" << quotedSteps << ",\"requires_npc_travel\":"
			       << (lastRouteRequiresNpcTravel ? "true" : "false") << ",\"quoted_npc_travel\":"
			       << (quotedNpcTravel ? "true" : "false") << ",\"route_safe\":" << (routeSafe ? "true" : "false")
			       << ",\"fare_affordable\":" << (routeAffordable ? "true" : "false")
			       << ",\"danger_cost\":" << routePlan.metrics.dangerCost
			       << ",\"maximum_health_loss_per_second\":" << routePlan.metrics.maximumHealthLossPerSecond
			       << ",\"route_elapsed_us\":" << routeElapsed.count()
			       << ",\"workflow_next\":" << jsonString(nextCommand);
			emit("action_result", currentPosition, fields.str());
		}
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
			const auto supply = std::find_if(observation.supplies.begin(), observation.supplies.end(),
			    [&transaction](const PlayerBotServiceSupply& value) { return value.itemId == transaction.itemId; });
			const PlayerBotSupplyKind kind = supply == observation.supplies.end() ? PlayerBotSupplyKind::HealthPotion : supply->kind;
			const char* action = sellingLocalLoot ? "sell" : kind == PlayerBotSupplyKind::Ammunition ? "buy_ammunition" :
			    kind == PlayerBotSupplyKind::ThrowingWeapon ? "buy_throwing_weapons" : "buy_potions";
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
			say(*player, "Withdrew " + std::to_string(transaction.amount) + " gold for supplies.");
		} else {
			emit("action_result", currentPosition, "\"action\":\"bank_deposit\",\"result\":\"success\",\"count\":" +
			     std::to_string(transaction.money) + ",\"bank_before\":" + std::to_string(transaction.balance) +
			     ",\"bank_after\":" + std::to_string(observation.bankBalance));
			say(*player, "Deposited " + std::to_string(transaction.money) + " gold. Bank: " +
			    std::to_string(observation.bankBalance) + '.');
		}
		updateSupplyRecovery(*player, currentPosition);
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
		if (command.outcome == PlayerBotServiceOutcome::InsufficientFunds) {
			supplyRecovery.deferRestock(observation.money + observation.bankBalance,
			    playerBotSupplyStockKey(supplyStocks(*player)));
			const auto supply = std::find_if(observation.supplies.begin(), observation.supplies.end(),
			    [&command](const PlayerBotServiceSupply& value) { return value.itemId == command.itemId; });
			const PlayerBotSupplyKind kind = supply == observation.supplies.end() ? PlayerBotSupplyKind::HealthPotion : supply->kind;
			const std::string reason = kind == PlayerBotSupplyKind::HealthPotion ? "insufficient_potion_funds" :
			    "insufficient_" + std::string(playerBotSupplyKindName(kind)) + "_funds";
			// Unaffordable supplies degrade operation instead of stopping: hunt
			// and sell under supply recovery until loot funds a restock.
			enterSupplyRecovery(currentPosition, observation.money + observation.bankBalance,
			                    recoverySpendingReserve(*player, potionStockTarget(*player)), reason.c_str());
			serviceWorkflow.reset();
			if (fixtureDriver.progressionGoalLoop(true).selectGoal) {
				selectTopLevelGoal(*player, currentPosition, "supply_recovery_potions_unaffordable");
			} else {
				startHunt(player, currentPosition, "supply_recovery_potions_unaffordable");
			}
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		stop("required_shop_offer_unavailable", currentPosition);
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
		if (Position::areInRange<3, 3, 0>(currentPosition, provider->getPosition())) {
			resetNavigation();
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		if (navigationRuntime.hasPendingWork()) {
			// Execute only validated steps. An exhausted leg returns to detailed
			// service validation, never ordinary navigation planning.
			PlayerBotNavigationRuntimeOutcome navigation;
			const bool reached = processNavigation(player, currentPosition,
			    PlayerBotNavigationGoal::exact(command.destination), &navigation,
			    playerBotNavigationMaximumExpandedNodes, false, false, nullptr, false);
			if (reached || navigation.routeRequest) schedule(SCHEDULER_MINTICKS);
			// A failed step clears the route. The next turn revalidates around the
			// blocked tile and rejects this approach only if no route avoids it.
			return;
		}
		if (currentPosition == command.destination) {
			// This turn's live tiles are all in range of the provider; ours is not.
			const auto live = observation.providers.find(command.providerId);
			const bool alternativeInRange = live != observation.providers.end() && std::any_of(
			    live->second.approaches.begin(), live->second.approaches.end(), [&currentPosition](const auto& approach) {
				    return approach.position != currentPosition;
			    });
			const PlayerBotServiceProviderWait wait = serviceWorkflow.awaitProviderAtApproach(alternativeInRange);
			if (wait == PlayerBotServiceProviderWait::Wait) {
				schedule(1000);
				return;
			}
			emitApproachResult("service", wait == PlayerBotServiceProviderWait::Released ? "released" : "rejected",
			                   "provider_out_of_range", currentPosition, command.providerId, currentPosition,
			                   ",\"provider_position\":{\"x\":" + std::to_string(provider->getPosition().x) +
			                       ",\"y\":" + std::to_string(provider->getPosition().y) + ",\"z\":" +
			                       std::to_string(static_cast<uint16_t>(provider->getPosition().z)) + "}");
			resetNavigation();
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		if (const std::optional<Position> rejected = serviceWorkflow.rejectSelectedApproach()) {
			emitApproachResult("service", "rejected", "route_unavailable", currentPosition, command.providerId, rejected);
			resetNavigation();
		}
		schedule(SCHEDULER_MINTICKS);
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
		const PlayerBotSupplyStocks completedStocks = supplyStocks(*player);
		const uint64_t stockKey = playerBotSupplyStockKey(completedStocks);
		if (playerBotMandatorySupplyDeficit(completedStocks, false).missing != 0 &&
		    !supplyRecovery.restockBlocked(player->getMoney() + player->getBankBalance(), stockKey)) {
			supplyRecovery.deferRestock(player->getMoney() + player->getBankBalance(), stockKey);
			huntCoordinator.setSupplyRecovery(true);
			emit("action_result", currentPosition,
			     "\"action\":\"restock\",\"result\":\"deferred\",\"reason\":\"safety_stock_unmet\"");
		}
		if (supplyRecovery.active() && survivalRuntime.needsHealing(survivalSnapshot(*player))) {
			// Stay at the completed service site while normal food/spell recovery runs.
			// Re-entering goal selection here would immediately interrupt another hunt.
			schedule(1000);
			return;
		}
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
		const uint32_t reserve = carriedSupplyReserve(player, item->getID());
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

bool PlayerBotController::depotApproachOccupied(const Player& player, const Position& approach) const
{
	const Tile* tile = g_game.map.getTile(approach);
	const CreatureVector* creatures = tile ? tile->getCreatures() : nullptr;
	return creatures && std::any_of(creatures->begin(), creatures->end(), [&player](const Creature* creature) {
		return creature != &player && !creature->isRemoved();
	});
}

void PlayerBotController::emitDepotDiscovery(playerbot::PlayerBotDepotTelemetry::Result result,
                                            const Position& position, const char* reason)
{
	if (telemetry.terminalLogged()) return;
	depotDiscoveryTelemetry.observe(depotWorkflow.snapshot());
	const auto record = depotDiscoveryTelemetry.report(std::chrono::steady_clock::now(), result);
	if (!record) return;
	using Result = playerbot::PlayerBotDepotTelemetry::Result;
	using Intent = playerbot::PlayerBotDepotTelemetry::Intent;
	const char* resultName = record->result == Result::Started ? "started" :
	    record->result == Result::Fallback ? "fallback" : record->result == Result::Selected ? "selected" :
	    record->result == Result::Arrived ? "arrived" : record->result == Result::Failed ? "failed" :
	    record->result == Result::Cancelled ? "cancelled" : "progress";
	const char* intentName = record->intent == Intent::Liquidation ? "liquidation" :
	    record->intent == Intent::ForcedReturn ? "forced_return" : "optional";
	const auto& counts = record->counters;
	const auto& snapshot = record->snapshot;
	std::ostringstream fields;
	auto positionField = [&](const char* key, const Position& value) {
		fields << ",\"" << key << "\":{\"x\":" << value.x << ",\"y\":" << value.y
		       << ",\"z\":" << static_cast<uint32_t>(value.z) << '}';
	};
	fields << "\"episode\":" << record->episode << ",\"result\":" << jsonString(resultName)
	       << ",\"reason\":" << jsonString(reason) << ",\"intent\":" << jsonString(intentName)
	       << ",\"elapsed_us\":" << record->elapsedUs << ",\"scans\":" << counts.scans
	       << ",\"scan_us\":" << counts.scanUs << ",\"route_slices\":" << counts.routeSlices
	       << ",\"route_active_us\":" << counts.routeUs << ",\"route_validations\":" << counts.validations
	       << ",\"accepted_routes\":" << counts.accepted << ",\"unknown_routes\":" << counts.unknown
	       << ",\"unsafe_routes\":" << counts.unsafe << ",\"unexecutable_routes\":" << counts.unexecutable
	       << ",\"reported_expanded_nodes\":" << counts.expandedNodes
	       << ",\"budget_denials\":" << counts.budgetDenials << ",\"budget_retry_us\":" << counts.budgetRetryUs
	       << ",\"budget_wait_us\":" << counts.budgetWaitUs << ",\"approach_failures\":" << counts.approachFailures
	       << ",\"selections\":" << counts.selections << ",\"attempt\":" << snapshot.attempts
	       << ",\"candidate_offset\":" << snapshot.candidateOffset << ",\"candidate_count\":" << snapshot.candidateCount
	       << ",\"indexed\":" << snapshot.indexedCandidates << ",\"in_scope\":" << snapshot.inScopeCandidates
	       << ",\"standable\":" << snapshot.standableCandidates << ",\"suppressed_approaches\":" << snapshot.suppressedApproaches
	       << ",\"has_risk_fallback\":" << (snapshot.hasRiskFallback ? "true" : "false")
	       << ",\"validating_risk_fallback\":" << (snapshot.validatingRiskFallback ? "true" : "false");
	positionField("origin", record->origin);
	positionField("hunt_exit", huntReturnDestination);
	if (snapshot.hasRouteCandidate || snapshot.hasSelectedDepot) {
		const auto& candidate = snapshot.hasRouteCandidate ? snapshot.routeCandidate : snapshot.selected;
		fields << ",\"depot_id\":" << candidate.depotId << ",\"locker_item_id\":" << candidate.lockerItemId;
		positionField("locker", candidate.lockerPosition);
		positionField("approach", candidate.approachPosition);
	}
	if (record->route) {
		const auto& route = *record->route;
		fields << ",\"last_route\":{\"depot_id\":" << route.candidate.depotId
		       << ",\"locker_item_id\":" << route.candidate.lockerItemId << ",\"candidate_offset\":" << route.candidateOffset
		       << ",\"result\":" << jsonString(route.result == PlayerBotNavigationResult::Reached ? "reached" :
		           route.result == PlayerBotNavigationResult::NodeLimit ? "node_limit" : "unreachable")
		       << ",\"safety_verdict\":" << jsonString(route.verdict == PlayerBotNavigationRiskVerdict::Accepted ? "accepted" :
		           route.verdict == PlayerBotNavigationRiskVerdict::Rejected ? "rejected" : "unknown")
		       << ",\"danger_evidence\":" << jsonString(route.evidence == PlayerBotNavigationDangerEvidence::Detailed ? "detailed" : "coarse")
		       << ",\"danger_cost\":" << route.dangerCost
		       << ",\"maximum_health_loss_per_second\":" << route.maximumHealthLossPerSecond
		       << ",\"fare\":" << route.fare << ",\"fare_accepted\":" << (route.fareAccepted ? "true" : "false")
		       << ",\"executable\":" << (route.executable ? "true" : "false")
		       << ",\"preferred_exit\":" << (route.preferredExit ? "true" : "false")
		       << ",\"risk_fallback\":" << (route.riskFallback ? "true" : "false")
		       << ",\"planner_kind\":" << jsonString(route.planner);
		positionField("origin", route.origin);
		positionField("locker", route.candidate.lockerPosition);
		positionField("approach", route.candidate.approachPosition);
		fields << '}';
	}
	emit("depot_discovery", position, fields.str());
}

bool PlayerBotController::discoverDepot(Player& player, const Position& currentPosition)
{
	const auto now = std::chrono::steady_clock::now();
	using DepotTelemetry = playerbot::PlayerBotDepotTelemetry;
	if (!depotDiscoveryTelemetry.active() && depotWorkflow.snapshot().stage == PlayerBotDepotStage::Discover) {
		depotDiscoveryTelemetry.begin(now, currentPosition, sellLootPlan ? DepotTelemetry::Intent::Liquidation :
		    huntTravelBudgetPhase == HuntTravelBudgetPhase::ReturnToDepot ? DepotTelemetry::Intent::ForcedReturn :
		    DepotTelemetry::Intent::Optional);
		emitDepotDiscovery(DepotTelemetry::Result::Started, currentPosition);
	}
	emitDepotDiscovery(DepotTelemetry::Result::Progress, currentPosition);
	const PlayerBotFixtureDepotEndpoint fixtureDepot = fixtureDriver.depotEndpoint();
	const int32_t health = player.getHealth();
	if (lastDepotDiscoveryHealth > health) depotDiscoveryUnderAttack = true;
	lastDepotDiscoveryHealth = health;
	auto advance = [&](PlayerBotDepotObservation observation) {
		observation.currentPosition = currentPosition;
		observation.now = now;
		observation.fixtureSynthetic = fixtureDepot.synthetic;
		// A forced return must not keep evaluating lockers while the bot is taking damage.
		observation.emergencyReturn = !sellLootPlan && huntTravelBudgetPhase == HuntTravelBudgetPhase::ReturnToDepot &&
		    depotDiscoveryUnderAttack;
		const auto scanStarted = observation.scan.observed ? std::chrono::steady_clock::now() :
		    std::chrono::steady_clock::time_point{};
		auto command = depotWorkflow.advance(observation, depotRouteValidationsPerDecision, maximumDepotDiscoveryAttempts,
		                                     depotApproachSuppression);
		if (observation.scan.observed) {
			depotDiscoveryTelemetry.scanBookkeeping(std::chrono::duration_cast<std::chrono::microseconds>(
			    std::chrono::steady_clock::now() - scanStarted));
		}
		if (command.snapshot.validatingRiskFallback) emitDepotDiscovery(DepotTelemetry::Result::Fallback, currentPosition);
		return command;
	};
	auto scan = [&](bool preferredExit = false) {
		PlayerBotDepotScan result;
		result.observed = true;
		if (fixtureDepot.synthetic) {
			result.indexedCandidates = result.inScopeCandidates = result.standableCandidates = 1;
			result.candidates.push_back({fixtureDepot.depotId, fixtureDepot.lockerItemId, fixtureDepot.lockerPosition,
			                             fixtureDepot.approachPosition, 0});
			return result;
		}
		if (preferredExit) {
			// The selected hunt already validated this exact exit approach.
			for (int32_t x = -1; x <= 1; ++x) for (int32_t y = -1; y <= 1; ++y) {
				if (!x && !y) continue;
				const Position locker(huntReturnDestination.x + x, huntReturnDestination.y + y, huntReturnDestination.z);
				Tile* tile = g_game.map.getTile(locker);
				TileItemVector* items = tile ? tile->getItemList() : nullptr;
				if (!items) continue;
				for (Item* item : *items) {
					Container* container = item->getContainer();
					DepotLocker* depot = container ? container->getDepotLocker() : nullptr;
					if (!depot) continue;
					++result.indexedCandidates;
					uint16_t lockerItemId = 0;
					if (!findDepotLocker(locker, depot->getDepotId(), lockerItemId)) continue;
					++result.inScopeCandidates;
					Tile* approach = g_game.map.getTile(huntReturnDestination);
					if (!playerBotStableApproachTile(approach, player)) continue;
					++result.standableCandidates;
					result.candidates.push_back({depot->getDepotId(), lockerItemId, locker, huntReturnDestination, 0});
				}
			}
			return result;
		}
		const PlayerBotTopology& topology = PlayerBotTopology::instance();
		const bool canUseRope = g_game.findItemOfType(&player, playerbot::ropeItemId, true) != nullptr;
		const bool canUseShovel = g_game.findItemOfType(&player, 2554, true) != nullptr;
		const PlayerBotTopologyDistances distances = topology.distancesFrom(
			currentPosition, canUseRope, canUseShovel, player.getLevel());
		for (const auto& entry : g_game.map.getDepotLockerPositions()) {
			if (backpackUpgradeDepotId != 0 && entry.first != backpackUpgradeDepotId) continue;
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
				if (!playerBotStableApproachTile(tile, player)) continue;
				++result.standableCandidates;
				const std::optional<uint32_t> routeCost = topology.distanceTo(
					distances, PlayerBotNavigationGoal::exact(approach));
				// Keep disconnected lockers in the bounded route workflow. Normal
				// navigation may reach them through a verified NPC travel offer.
				result.candidates.push_back({entry.first, lockerItemId, lockerPosition, approach,
				                             routeCost.value_or(playerBotNavigationDistance(currentPosition, approach)),
				                             depotApproachOccupied(player, approach)});
			}
			}
		}
		return result;
	};

	auto measuredScan = [&](bool preferredExit = false) {
		const auto started = std::chrono::steady_clock::now();
		auto result = scan(preferredExit);
		depotDiscoveryTelemetry.scan(std::chrono::duration_cast<std::chrono::microseconds>(
		    std::chrono::steady_clock::now() - started));
		return result;
	};
	PlayerBotDepotCommand command = advance({});
	bool preferredExit = command.type == PlayerBotDepotCommandType::Scan && !fixtureDepot.synthetic &&
	    !sellLootPlan && huntTravelBudgetPhase == HuntTravelBudgetPhase::ReturnToDepot &&
	    !huntExitValidationAttempted && huntReturnDestination != Position();
	if (preferredExit) huntExitValidationAttempted = true;
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
		observation.scan = measuredScan(preferredExit);
		command = advance(observation);
		if (preferredExit && command.type != PlayerBotDepotCommandType::ValidateRoute) {
			depotWorkflow.reset();
			command = advance({});
			observation.scan = measuredScan();
			command = advance(observation);
			preferredExit = false;
		}
	}

	if (command.type != PlayerBotDepotCommandType::ValidateRoute) depotSourceRouteSearch.reset();
	std::deque<PlayerBotNavigationStep> steps;
	uint32_t routeValidations = 0;
	uint32_t lastDangerCost = 0;
	double lastMaximumHealthLossPerSecond = 0;
	const char* lastPlannerKind = "none";
	const char* lastDangerEvidence = "unknown";
	const char* lastSafetyVerdict = "unknown";
	while (command.type == PlayerBotDepotCommandType::ValidateRoute && command.snapshot.hasRouteCandidate &&
	       routeValidations < depotRouteValidationsPerDecision) {
		auto& budget = playerBotHuntPlanningBudget();
		const auto requestedAt = std::chrono::steady_clock::now();
		const auto admission = budget.request(playerId, requestedAt);
		if (!admission.admitted) {
			const auto retry = std::chrono::milliseconds(std::clamp<int64_t>(
			    std::chrono::duration_cast<std::chrono::milliseconds>(admission.wait).count(),
			    blockedRouteRetryInterval, 5000));
			depotDiscoveryTelemetry.denied(requestedAt, std::chrono::duration_cast<std::chrono::microseconds>(retry));
			emitDepotDiscovery(DepotTelemetry::Result::Progress, currentPosition);
			schedule(static_cast<uint32_t>(retry.count()));
			return false;
		}
		depotDiscoveryTelemetry.admitted(requestedAt);
		PlayerBotPlanningBudget::Charge charge(budget, playerId);
		++routeValidations;
		const PlayerBotDepotCandidate& candidate = command.snapshot.routeCandidate;
		PlayerBotDepotObservation observation;
		uint16_t lockerItemId = 0;
		Tile* tile = g_game.map.getTile(candidate.approachPosition);
		const bool valid = fixtureDepot.synthetic ||
			(findDepotLocker(candidate.lockerPosition, candidate.depotId, lockerItemId) && lockerItemId == candidate.lockerItemId &&
			 playerBotStableApproachTile(tile, player));
		const auto startedAt = std::chrono::steady_clock::now();
		PlayerBotNavigationRoutePlan routePlan;
		const bool remoteLiquidation = valid && candidate.approachPosition != currentPosition &&
		    sellLootPlan && candidate.depotId == sellLootPlan->sourceDepotId;
		if (!remoteLiquidation) depotSourceRouteSearch.reset();
		if (remoteLiquidation) {
			const uint64_t reserve = std::max(huntTravelRecoveryFundsReserve(player, huntTravelBudgetPhase),
			                                  huntTravelReturnFareReserve(huntTravelBudgetPhase));
			if (!depotSourceRouteSearch || depotSourceRouteSearch->origin != currentPosition ||
			    depotSourceRouteSearch->destination != candidate.approachPosition ||
			    depotSourceRouteSearch->locker != candidate.lockerPosition ||
			    depotSourceRouteSearch->depotId != candidate.depotId ||
			    depotSourceRouteSearch->lockerItemId != candidate.lockerItemId ||
			    depotSourceRouteSearch->reserve != reserve ||
			    depotSourceRouteSearch->sourceFare != sellLootPlan->sourceFare) {
				depotSourceRouteSearch.emplace();
				auto& search = *depotSourceRouteSearch;
				search.origin = currentPosition;
				search.destination = candidate.approachPosition;
				search.locker = candidate.lockerPosition;
				search.depotId = candidate.depotId;
				search.lockerItemId = candidate.lockerItemId;
				search.reserve = reserve;
				search.sourceFare = sellLootPlan->sourceFare;
				search.pass = (uint64_t(1) << 61) | (uint64_t(playerId) << 24) | ++depotSourceRouteSerial;
			}
			auto& search = *depotSourceRouteSearch;
			PlayerBotNavigationRoutePlan walking;
			std::optional<PlayerBotNavigationRoutePlan> planned;
			{
				// The selector owns its own resumable work; never replace the hunt's frontier.
				struct WorkSlot {
					std::shared_ptr<PlayerBotHuntTravelWork>& hunt;
					std::shared_ptr<PlayerBotHuntTravelWork>& depot;
					WorkSlot(decltype(hunt) hunt, decltype(depot) depot) : hunt(hunt), depot(depot) { hunt.swap(depot); }
					~WorkSlot() { hunt.swap(depot); }
				} slot(huntTravelWork, search.work);
				PlayerBotHuntRouteRequest request;
				request.planningPass = search.pass;
				// Revalidate the selected walking source without searching unrelated travel offers.
				request.walkingOnly = !sellLootPlan->sourceAllowNpcTravel;
				request.from = currentPosition;
				request.to = candidate.approachPosition;
				PlayerBotHuntRouteTiming timing;
				planned = advanceHuntTravelRoute(player, request, currentPosition, timing, reserve, true, &walking);
			}
			if (!planned && ++search.turns < maximumRouteSearchTurns) {
				depotDiscoveryTelemetry.pending(std::chrono::duration_cast<std::chrono::microseconds>(
				    std::chrono::steady_clock::now() - startedAt));
				emitDepotDiscovery(DepotTelemetry::Result::Progress, currentPosition);
				schedule(routeSearchContinuationInterval);
				return false;
			}
			if (!planned) {
				routePlan.metrics.attempted = true;
				routePlan.metrics.result = PlayerBotNavigationResult::NodeLimit;
			} else {
				routePlan = std::move(*planned);
				const auto paid = std::find_if(routePlan.steps.begin(), routePlan.steps.end(), [](const auto& step) {
					return step.action == PlayerBotNavigationAction::NpcTravel;
				});
				// The detailed selector proves the entire trip and returns its validated
				// walk to the boarding NPC. Otherwise walk.
				if (paid != routePlan.steps.end() && !(sellLootPlan->sourceAllowNpcTravel &&
				    routePlan.metrics.result == PlayerBotNavigationResult::Reached &&
				    playerBotNavigationRiskVerdict(riskProfile, routePlan.metrics) == PlayerBotNavigationRiskVerdict::Accepted &&
				    routePlan.metrics.fare <= sellLootPlan->sourceFare)) {
					routePlan = std::move(walking);
				}
			}
			depotSourceRouteSearch.reset();
		} else if (valid && candidate.approachPosition != currentPosition) {
			// Coarse topology orders candidates; detailed planning judges danger.
			routePlan = planHuntTravelRoute(player, currentPosition, candidate.approachPosition,
			    navigationRuntime.activeBlockedPositions(startedAt), false, nullptr,
			    huntTravelBudgetPhase == HuntTravelBudgetPhase::ReturnToDepot ?
			        PlayerBotRouteIntent::ForcedReturn : PlayerBotRouteIntent::Optional);
		}
		const auto verdict = playerBotNavigationRiskVerdict(riskProfile, routePlan.metrics);
		const bool routeSafe = candidate.approachPosition == currentPosition ||
		    verdict == PlayerBotNavigationRiskVerdict::Accepted;
		lastDangerCost = routePlan.metrics.dangerCost;
		lastMaximumHealthLossPerSecond = routePlan.metrics.maximumHealthLossPerSecond;
		lastPlannerKind = remoteLiquidation && sellLootPlan->sourceAllowNpcTravel ? "npc_travel" : "hunt_travel";
		lastDangerEvidence = routePlan.metrics.dangerEvidence == PlayerBotNavigationDangerEvidence::Detailed ? "detailed" : "coarse";
		lastSafetyVerdict = verdict == PlayerBotNavigationRiskVerdict::Rejected ?
		    (routePlan.metrics.dangerCost > static_cast<uint32_t>(riskProfile.maximumRouteHealthLoss * riskProfile.healthLossCost) ?
		        "danger_cost" : "peak_health_loss") : verdict == PlayerBotNavigationRiskVerdict::Unknown ? "unknown" : "accepted";
		const bool fareAccepted = (!sellLootPlan || candidate.depotId != sellLootPlan->sourceDepotId ||
		                           routePlan.metrics.fare <= sellLootPlan->sourceFare) &&
		                          huntTravelFareAffordable(player, routePlan.metrics.fare,
		                                                   huntTravelBudgetPhase);
		const bool executable = valid && fareAccepted && (candidate.approachPosition == currentPosition ||
			(routePlan.metrics.result == PlayerBotNavigationResult::Reached && !routePlan.steps.empty()));
		const bool liquidationSource = sellLootPlan && candidate.depotId == sellLootPlan->sourceDepotId;
		const bool forcedReturn = huntTravelBudgetPhase == HuntTravelBudgetPhase::ReturnToDepot && !liquidationSource;
		observation.allowUnknownFallback = forcedReturn;
		// An NPC itinerary cannot certify its future legs at this point. A
		// forced return may execute the verified first leg and replan afterward;
		// optional trips and liquidation still require detailed acceptance.
		const bool acceptingForcedExit = forcedReturn && (preferredExit || command.snapshot.validatingRiskFallback);
		const bool reached = executable && playerBotDepotRouteSafetyAccepted(
		    routeSafe, liquidationSource,
		    acceptingForcedExit ? PlayerBotRouteIntent::ForcedReturn : PlayerBotRouteIntent::Optional) &&
		    (candidate.approachPosition == currentPosition ||
		     routePlan.metrics.dangerEvidence == PlayerBotNavigationDangerEvidence::Detailed || acceptingForcedExit);
		telemetry.recordPathfinding(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - startedAt), executable);
		observation.routeResult = reached ? PlayerBotDepotRouteResult::Reached :
		    !executable ? PlayerBotDepotRouteResult::Unreachable :
		    verdict == PlayerBotNavigationRiskVerdict::Unknown ? PlayerBotDepotRouteResult::Unknown :
		    PlayerBotDepotRouteResult::Unsafe;
		observation.routeSteps = static_cast<uint32_t>(routePlan.metrics.steps);
		observation.expandedNodes = routePlan.metrics.expandedNodes;
		observation.dangerCost = routePlan.metrics.dangerCost;
		observation.maximumHealthLossPerSecond = routePlan.metrics.maximumHealthLossPerSecond;
		DepotTelemetry::Route routeEvidence;
		routeEvidence.candidate = candidate;
		routeEvidence.origin = currentPosition;
		routeEvidence.candidateOffset = command.snapshot.candidateOffset;
		routeEvidence.result = routePlan.metrics.result;
		routeEvidence.evidence = routePlan.metrics.dangerEvidence;
		routeEvidence.verdict = verdict;
		routeEvidence.fare = routePlan.metrics.fare;
		routeEvidence.dangerCost = lastDangerCost;
		routeEvidence.maximumHealthLossPerSecond = lastMaximumHealthLossPerSecond;
		routeEvidence.executable = executable;
		routeEvidence.fareAccepted = fareAccepted;
		routeEvidence.preferredExit = preferredExit;
		routeEvidence.riskFallback = command.snapshot.validatingRiskFallback;
		routeEvidence.planner = lastPlannerKind;
		depotDiscoveryTelemetry.validated(std::move(routeEvidence), observation.routeResult, routePlan.metrics.expandedNodes,
		    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - startedAt));
		if (reached) {
			steps = std::move(routePlan.steps);
		} else if (!preferredExit && routePlan.metrics.attempted &&
		           routePlan.metrics.result != PlayerBotNavigationResult::Reached) {
			huntCoordinator.observeTransitMovementFailure(currentPosition);
			// Feed failed fixed-goal validation into the shared navigation bound.
			PlayerBotNavigationRoutePlan failure;
			failure.metrics = routePlan.metrics;
			const PlayerBotNavigationRuntimeOutcome fed = navigationRuntime.observePlan(
			    {PlayerBotNavigationGoal::exact(candidate.approachPosition), std::move(failure),
			     false, false, startedAt});
			if (handleFixedTargetRouteExhausted(&player, currentPosition, fed, startedAt, false)) {
				schedule(blockedRouteRetryInterval);
				return false;
			}
		}
		if (preferredExit && !reached) {
			depotWorkflow.reset();
			command = advance({});
			PlayerBotDepotObservation fallback;
			fallback.scan = measuredScan();
			command = advance(fallback);
			preferredExit = false;
		} else {
			command = advance(observation);
		}
	}
	if (command.type == PlayerBotDepotCommandType::ValidateRoute) {
		emitDepotDiscovery(DepotTelemetry::Result::Progress, currentPosition, "route_validation_budget_exhausted");
		schedule(blockedRouteRetryInterval);
		return false;
	}
	if (command.type == PlayerBotDepotCommandType::Navigate && command.snapshot.hasSelectedDepot) {
		if (!steps.empty()) {
			observeNavigationPlan(command.snapshot.selected.approachPosition, std::move(steps));
		}
		if (routeValidations == 0) return true; // Already selected; do not report another discovery every movement turn.
		emitDepotDiscovery(DepotTelemetry::Result::Selected, currentPosition);
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
		       << ",\"planner_kind\":" << jsonString(lastPlannerKind)
		       << ",\"danger_evidence\":" << jsonString(lastDangerEvidence)
		       << ",\"safety_verdict\":" << jsonString(lastSafetyVerdict)
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
		// Temporary blocker recovery remains in this episode, rather than
		// emitting a new failure/start pair on every retry.
		const bool finalFailure = sellLootPlan || navigationRuntime.activeBlockedPositions(now).empty();
		emitDepotDiscovery(finalFailure ? DepotTelemetry::Result::Failed : DepotTelemetry::Result::Progress,
		                   currentPosition, reason);
		if (finalFailure) logActionFailure("depot_discover", reason, currentPosition);
		else telemetry.recordActionFailure();
		if (sellLootPlan) {
			deferSellLoot(player, currentPosition, reason);
		} else if (!navigationRuntime.activeBlockedPositions(now).empty()) {
			// A temporarily suppressed blocker can make every approach unreachable.
			// Retry discovery while the shared suppression remains active.
			schedule(blockedRouteRetryInterval);
		} else {
			stop("depot_unavailable", currentPosition);
		}
		return false;
	}
	const uint32_t retryDelay = command.snapshot.retryAt ? static_cast<uint32_t>(std::max<int64_t>(1,
		std::chrono::duration_cast<std::chrono::milliseconds>(*command.snapshot.retryAt - now).count())) :
		std::min<uint32_t>(depotRetryMaximumInterval, depotRetryInitialInterval << std::min<uint32_t>(command.snapshot.attempts, 2));
	schedule(command.type == PlayerBotDepotCommandType::Wait ? retryDelay : blockedRouteRetryInterval);
	return false;
}

bool PlayerBotController::depotApproachStalled(Player& player, const Position& currentPosition, const Position& approach,
	                                          const PlayerBotNavigationRuntimeOutcome* navigation)
{
	if (currentPosition == approach) return false;
	PlayerBotApproach& shared = depotWorkflow.approach();
	if (navigation && navigation->positionalProgress) shared.unblocked();
	// Without a navigation outcome, runtime counters may still describe an
	// earlier goal. Depot suppression is short; occupancy ordering, not
	// suppression, keeps a rescan from selecting the same approach again.
	const bool stalled = navigation && (navigation->oscillation ||
	                                    navigation->stepFailureCount >= maximumRepeatedNavigationStepFailures);
	const char* reason = navigation && navigationRuntime.fixedTargetRouteFailureCount() != 0 ? "route_unavailable" :
	    stalled ? "navigation_stalled" :
	    Position::areInRange<7, 5, 0>(currentPosition, approach) && depotApproachOccupied(player, approach) ?
	        "approach_occupied" : nullptr;
	if (!reason) return false;
	// Walking already sidestepped and replanning detours around the blocked
	// tile. Wait out a creature on the path within the shared bound.
	// Oscillation may follow an older step failure; only repeated failed steps
	// name their current blocker.
	const char* cause = lastStepFailure.cause;
	if (std::strcmp(reason, "navigation_stalled") == 0 &&
	    navigation->stepFailureCount >= maximumRepeatedNavigationStepFailures &&
	    (!std::strcmp(cause, "monster") || !std::strcmp(cause, "player") || !std::strcmp(cause, "npc") ||
	     !std::strcmp(cause, "creature"))) {
		if (shared.blocked() == PlayerBotApproachVerdict::Wait) {
			emitApproachResult("depot", "waiting", "route_blocked", currentPosition, 0, approach,
			                   ",\"waits\":" + std::to_string(shared.blockedWaits()));
			return true;
		}
		reason = "route_blocked";
	}
	const PlayerBotApproachVerdict verdict = shared.reject(approach, reason);
	emitApproachResult("depot", verdict == PlayerBotApproachVerdict::Exhausted ? "failed" : "rejected", reason,
	                   currentPosition, 0, approach);
	// Depot exhaustion only defers: a depot failure stops the controller.
	if (verdict == PlayerBotApproachVerdict::Exhausted) shared.reset();
	depotDiscoveryTelemetry.approachFailed();
	PlayerBotDepotObservation observation;
	observation.currentPosition = currentPosition;
	observation.now = std::chrono::steady_clock::now();
	observation.routeResult = PlayerBotDepotRouteResult::Unreachable;
	depotWorkflow.advance(observation, depotRouteValidationsPerDecision, maximumDepotDiscoveryAttempts,
	                      depotApproachSuppression);
	resetNavigation();
	return true;
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
	if (checkpoint == DepotRestartCheckpoint::Depart) {
		player.closeContainer(depotChestContainerId);
		player.closeContainer(depotLockerContainerId);
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
			if (items[index]->equals(&item) && items[index]->getItemCount() < 100) {
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
	if (command.snapshot.stage != PlayerBotDepotStage::Depart) {
		depotCompletionAnnounced = false;
		sellLootSearch.reset();
		sellLootSearchPending = false;
	}
	PlayerBotDepotObservation observation;
	if (command.snapshot.hasSelectedDepot) {
		observation.atApproach = fixtureDepot.synthetic || Position::areInRange<1, 1, 0>(currentPosition, command.snapshot.selected.lockerPosition);
		if (observation.atApproach) emitDepotDiscovery(playerbot::PlayerBotDepotTelemetry::Result::Arrived, currentPosition);
		if (observation.atApproach && huntTravelBudgetPhase == HuntTravelBudgetPhase::ReturnToDepot) {
			huntTravelBudgetPhase = HuntTravelBudgetPhase::Supply;
		}
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
		if (!depotCompletionAnnounced) {
			emit("action_result", currentPosition, fields.str());
			say(*player, "Depot work complete.");
			depotCompletionAnnounced = true;
		}
		if (pauseDepotFixtureForRestart(*player, DepotRestartCheckpoint::Depart, currentPosition)) return;
		Container* chest = player->getContainerByID(depotChestContainerId);
		bool selectedAfterDeposit = false;
		if (!fixtureDepot.synthetic && chest) {
			const bool saleCoolingDown = progressionRuntime.isCoolingDown(TopLevelGoal::SellLoot, std::chrono::steady_clock::now());
			if (sellLootPlan && sellLootPlan->sourceDepotId == command.snapshot.selected.depotId) {
				processSellLootWithdrawal(*player, currentPosition);
				return;
			}
			std::chrono::steady_clock::duration saleRetryAfter{};
			if (!saleCoolingDown && !sellLootPlan &&
			    planSellLootTrip(*player, command.snapshot.selected.depotId, currentPosition, &saleRetryAfter)) {
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
			if (sellLootSearchPending && !saleCoolingDown) {
				const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(saleRetryAfter).count();
				schedule(static_cast<uint32_t>(delay > 0 ? delay : SCHEDULER_MINTICKS));
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
			const uint32_t reserve = carriedSupplyReserve(*player, depositItem->getID());
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
