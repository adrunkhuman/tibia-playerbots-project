/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
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
using namespace playerbot;

namespace {
	constexpr size_t maximumEquipmentHuntRegions = 32;
	constexpr size_t maximumEquipmentCatalogProviders = 16;
	constexpr size_t maximumEquipmentProviderRoutes = 4;
	constexpr size_t maximumEquipmentProviderApproaches = 4;
	constexpr uint64_t maximumEquipmentProviderPathNodes = 5000;
	constexpr size_t maximumEquipmentCatalogOffers = 64;

	// Internal playerbot state. Keep these below TFS's reserved 10,000,000 range and outside datapack quest keys.
	constexpr uint32_t backpackUpgradeStatusStorage = 9990000;
	constexpr uint32_t backpackUpgradeDepotStorage = 9990001;
	constexpr uint32_t backpackUpgradeTokenStorage = 9990002;
	constexpr int32_t backpackUpgradeTagged = 1;
	constexpr int32_t backpackUpgradeStaged = 2;
	constexpr int32_t backpackUpgradePurchased = 3;
	const std::string backpackUpgradeMarker = "playerbot_backpack_upgrade";
	const std::string backpackUpgradeReplacementMarker = "playerbot_backpack_upgrade_replacement";

	bool hasBackpackUpgradeToken(Item& item, const std::string& marker, int32_t token)
	{
		const ItemAttributes::CustomAttribute* attribute = item.getCustomAttribute(marker);
		return attribute && attribute->value.type() == typeid(int64_t) &&
			boost::get<int64_t>(attribute->value) == token;
	}

	void collectTaggedBags(Container& container, const std::string& marker, int32_t token, std::vector<Item*>& matches)
	{
		for (ContainerIterator it = container.iterator(); it.hasNext(); it.advance()) {
			Item* item = *it;
			if (item->getID() == ITEM_BAG && hasBackpackUpgradeToken(*item, marker, token)) matches.push_back(item);
		}
	}

	Item* findTaggedBag(Player& player, uint16_t depotId, int32_t token, const std::string& marker, uint32_t* matchCount)
	{
		std::vector<Item*> matches;
		if (token > 0) {
			for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
				Item* root = player.getInventoryItem(static_cast<slots_t>(slot));
				if (!root) continue;
				if (root->getID() == ITEM_BAG && hasBackpackUpgradeToken(*root, marker, token)) matches.push_back(root);
				if (Container* container = root->getContainer()) collectTaggedBags(*container, marker, token, matches);
			}
			if (depotId != 0) {
				if (DepotChest* chest = player.getDepotChest(depotId, false)) {
					collectTaggedBags(*chest, marker, token, matches);
				}
			}
		}
		if (matchCount) *matchCount = static_cast<uint32_t>(matches.size());
		return matches.size() == 1 ? matches.front() : nullptr;
	}
}

Item* PlayerBotController::taggedBackpackUpgradeBag(Player& player, uint32_t* matchCount) const
{
	return findTaggedBag(player, backpackUpgradeDepotId, backpackUpgradeToken, backpackUpgradeMarker, matchCount);
}

Item* PlayerBotController::taggedBackpackUpgradeReplacementBag(Player& player, uint32_t* matchCount) const
{
	return findTaggedBag(player, backpackUpgradeDepotId, backpackUpgradeToken, backpackUpgradeReplacementMarker, matchCount);
}

void PlayerBotController::clearBackpackUpgradePersistence(Player& player, bool clearItemMarker)
{
	if (clearItemMarker) {
		uint32_t matches = 0;
		if (Item* bag = taggedBackpackUpgradeBag(player, &matches); bag && matches == 1) {
			bag->removeCustomAttribute(backpackUpgradeMarker);
		}
		matches = 0;
		if (Item* replacement = taggedBackpackUpgradeReplacementBag(player, &matches); replacement && matches == 1) {
			replacement->removeCustomAttribute(backpackUpgradeReplacementMarker);
		}
	}
	player.addStorageValue(backpackUpgradeStatusStorage, -1);
	player.addStorageValue(backpackUpgradeDepotStorage, -1);
	backpackUpgradeDepot.reset();
	backpackUpgradeDepotId = 0;
	backpackUpgradeToken = 0;
}

bool PlayerBotController::resumeBackpackUpgrade(Player& player, const Position& position)
{
	int32_t status = -1;
	int32_t depotId = -1;
	int32_t token = -1;
	if (!player.getStorageValue(backpackUpgradeStatusStorage, status) || status < backpackUpgradeTagged ||
	    status > backpackUpgradePurchased) return false;
	if (!player.getStorageValue(backpackUpgradeDepotStorage, depotId) || depotId <= 0 || depotId > UINT16_MAX ||
	    !player.getStorageValue(backpackUpgradeTokenStorage, token) || token <= 0) {
		stop("backpack_upgrade_persistence_invalid", position);
		return true;
	}
	backpackUpgradeDepotId = static_cast<uint16_t>(depotId);
	backpackUpgradeToken = token;
	backpackUpgradeDepot.reset();
	depotWorkflow.reset();
	sellLootPlan.reset();
	uint32_t matches = 0;
	Item* bag = taggedBackpackUpgradeBag(player, &matches);
	if (!bag || matches != 1) {
		stop(matches == 0 ? "backpack_upgrade_marker_missing" : "backpack_upgrade_marker_duplicate", position);
		return true;
	}
	uint32_t replacementMatches = 0;
	taggedBackpackUpgradeReplacementBag(player, &replacementMatches);
	if (replacementMatches > 1) {
		stop("backpack_upgrade_replacement_marker_duplicate", position);
		return true;
	}
	Item* backItem = player.getInventoryItem(CONST_SLOT_BACKPACK);
	Container* acquired = backItem && backItem->getID() == ITEM_BACKPACK ? backItem->getContainer() : nullptr;
	for (Cylinder* parent = bag->getParent(); acquired && parent; parent = parent->getParent()) {
		if (parent == acquired) {
			clearBackpackUpgradePersistence(player, true);
			emit("backpack_upgrade_recovery", position, "\"result\":\"already_complete\"");
			return false;
		}
	}
	if (bag == backItem) {
		clearBackpackUpgradePersistence(player, true);
		emit("backpack_upgrade_recovery", position, "\"result\":\"already_restored\"");
		return false;
	}
	if (acquired) {
		status = backpackUpgradePurchased;
		player.addStorageValue(backpackUpgradeStatusStorage, backpackUpgradePurchased);
	}
	const bool blockedReceipt = backItem && !acquired;
	const bool purchasedBackpackMissing = status == backpackUpgradePurchased && !acquired;
	PlayerBotEquipmentOfferEvaluation plan;
	if (!blockedReceipt && status != backpackUpgradePurchased) {
		auto selected = evaluateEquipmentOffers(player, position);
		if (selected && selected->backpackAcquisition) plan = std::move(*selected);
	}
	if (plan.itemId == 0) {
		plan.itemId = ITEM_BACKPACK;
		plan.slot = CONST_SLOT_BACKPACK;
		plan.backpackAcquisition = true;
	}
	plan.bagUpgrade = true;
	progressionRuntime.resumeEquipmentBackpack(std::move(plan), true, acquired != nullptr);
	if (purchasedBackpackMissing || blockedReceipt ||
	    (status != backpackUpgradePurchased && progressionRuntime.equipmentPurchase().plan().npcId == 0)) {
		const char* reason = purchasedBackpackMissing ? "purchased_backpack_missing" :
			blockedReceipt ? "replacement_backpack_present" : "resume_offer_unavailable";
		progressionRuntime.beginEquipmentBackpackRecovery(reason, false);
		emit("backpack_upgrade_recovery", position, "\"result\":\"resumed\",\"status\":\"restore\",\"reason\":" + jsonString(reason));
		return true;
	}
	emit("backpack_upgrade_recovery", position, "\"result\":\"resumed\",\"status\":" +
		jsonString(status == backpackUpgradePurchased ? "retrieve" : "purchase"));
	return true;
}

bool PlayerBotController::interruptBackpackUpgradeForService(Player& player, const Position& position, const char* reason)
{
	if (!progressionRuntime.active(PlayerBotProgressionProcedure::BuyEquipment) ||
	    !progressionRuntime.equipmentPurchase().plan().bagUpgrade) return false;
	Item* backItem = player.getInventoryItem(CONST_SLOT_BACKPACK);
	Item* taggedBag = taggedBackpackUpgradeBag(player);
	if ((backpackUpgradeToken == 0 && backItem && backItem->getID() == ITEM_BAG) || taggedBag == backItem) {
		clearBackpackUpgradePersistence(player, taggedBag != nullptr);
		progressionRuntime.finish();
		beginService(&player, position, reason);
		schedule(SCHEDULER_MINTICKS);
		return true;
	}
	const bool purchased = player.getInventoryItem(CONST_SLOT_BACKPACK) &&
		player.getInventoryItem(CONST_SLOT_BACKPACK)->getID() == ITEM_BACKPACK;
	progressionRuntime.beginEquipmentBackpackRecovery(reason, purchased);
	resetNavigation();
	emit("backpack_upgrade_recovery", position, "\"result\":\"started\",\"reason\":" + jsonString(reason));
	schedule(SCHEDULER_MINTICKS);
	return true;
}

PlayerBotController::EquipmentHuntSummary PlayerBotController::equipmentHuntSummary(Player& player,
	                                                                                   const PlayerBotCombatProfile& profile) const
{
	PlayerBotHuntRegionPlanner planner;
	const PlayerBotHuntRegionScan scan = planner.beginScan(player);
	const PlayerBotHuntPlanningProfile planning = huntCoordinator.huntPlanningProfile(playerBotHuntPlanningProfile(player, profile, 0));
	const auto performance = huntCoordinator.huntRegionPerformance();
	const uint32_t duration = static_cast<uint32_t>(std::max<int32_t>(1, g_config.getNumber(ConfigManager::PLAYERBOT_HUNT_DURATION_SECONDS)));
	std::vector<PlayerBotHuntRegion> regions;
	regions.reserve(std::min(scan.candidateIndices.size(), maximumEquipmentHuntRegions));
	bool truncated = false;
	for (size_t candidateIndex : scan.candidateIndices) {
		if (regions.size() >= maximumEquipmentHuntRegions) {
			truncated = true;
			break;
		}
		auto score = planner.score(player, planning, scan.revision, candidateIndex, {}, performance, duration);
		if (score.valid) {
			regions.push_back(std::move(score.region));
		}
	}
	return huntCoordinator.summarizeEquipmentHunts(regions, truncated);
}

void PlayerBotController::emitEquipmentOffer(const Player& player, const EquipmentOfferEvaluation& evaluation,
	                                            const PlayerBotCombatProfile& currentProfile,
	                                            const EquipmentHuntSummary& currentHunts, uint64_t reserve,
	                                            const Position& position, const char* result, const char* reason) const
{
	std::ostringstream fields;
	fields << std::fixed << std::setprecision(2)
	       << "\"result\":" << jsonString(result) << ",\"npc_id\":" << evaluation.npcId
	       << ",\"item_id\":" << evaluation.itemId << ",\"price\":" << evaluation.price
	       << ",\"replaced_item_id\":" << (evaluation.replacedItemId == 0 ? "null" : std::to_string(evaluation.replacedItemId))
	       << ",\"carried_money\":" << player.getMoney() << ",\"bank_balance\":" << player.getBankBalance()
	       << ",\"reserve\":" << reserve << ",\"travel_steps\":" << evaluation.travelSteps
	       << ",\"displaced_left_item_id\":" << (evaluation.displacedLeftItemId == 0 ? "null" : std::to_string(evaluation.displacedLeftItemId))
	       << ",\"displaced_right_item_id\":" << (evaluation.displacedRightItemId == 0 ? "null" : std::to_string(evaluation.displacedRightItemId))
	       << ",\"current\":{\"armor\":" << currentProfile.armor << ",\"defense\":" << currentProfile.defense
	       << ",\"attack\":" << currentProfile.attack << ",\"suitable_regions\":" << currentHunts.suitableRegions
	       << ",\"evaluated_regions\":" << currentHunts.evaluatedRegions
	       << ",\"hunt_evaluation_truncated\":" << (currentHunts.truncated ? "true" : "false")
	       << ",\"best_projected_experience\":" << currentHunts.bestProjectedExperience
	       << ",\"lowest_threat_ratio\":" << currentHunts.lowestThreatRatio
	       << ",\"combat_ready\":" << (evaluation.currentReady ? "true" : "false") << '}'
	       << ",\"candidate\":{\"armor\":" << evaluation.profile.armor << ",\"defense\":" << evaluation.profile.defense
	       << ",\"attack\":" << evaluation.profile.attack << ",\"suitable_regions\":" << evaluation.hunts.suitableRegions
	       << ",\"evaluated_regions\":" << evaluation.hunts.evaluatedRegions
	       << ",\"hunt_evaluation_truncated\":" << (evaluation.hunts.truncated ? "true" : "false")
	       << ",\"best_projected_experience\":" << evaluation.hunts.bestProjectedExperience
	       << ",\"lowest_threat_ratio\":" << evaluation.hunts.lowestThreatRatio
	       << ",\"combat_ready\":" << (evaluation.candidateReady ? "true" : "false") << '}'
	       << ",\"rule\":" << jsonString(PlayerBotEquipmentPolicy::decisionRuleName(evaluation.rule))
	       << ",\"carried\":" << (evaluation.carried ? "true" : "false")
	       << ",\"backpack_acquisition\":" << (evaluation.backpackAcquisition ? "true" : "false")
	       << ",\"bag_upgrade\":" << (evaluation.bagUpgrade ? "true" : "false")
	       << ",\"provider_position\":{\"x\":" << evaluation.npcPosition.x << ",\"y\":" << evaluation.npcPosition.y
	       << ",\"z\":" << static_cast<uint16_t>(evaluation.npcPosition.z) << '}';
	if (reason) {
		fields << ",\"reason\":" << jsonString(reason);
	}
	emit("equipment_offer_candidate", position, fields.str());
}

std::optional<PlayerBotController::EquipmentOfferEvaluation> PlayerBotController::evaluateEquipmentOffers(
	Player& player, const Position& position)
{
	const uint64_t reserve = spellTrainingReserve(player);
	const uint64_t totalMoney = player.getMoney() + player.getBankBalance();
	const PlayerBotEquipmentPlayerSnapshot playerFacts = PlayerBotEquipmentAdapter::player(player);
	const EquipmentLoadout currentLoadout = PlayerBotEquipmentAdapter::loadout(player);
	const PlayerBotCombatProfile currentProfile = equipmentPolicy.combatProfile(playerFacts, currentLoadout);
	const EquipmentHuntSummary currentHunts = equipmentHuntSummary(player, currentProfile);
	Item* backpackItem = player.getInventoryItem(CONST_SLOT_BACKPACK);
	const uint16_t potionItemId = recoveryPotionItemId(player.getVocationId());
	const PlayerBotEquipmentReadinessInput readiness{
		backpackItem && backpackItem->getContainer(),
		inventoryPolicy.inventoryItemCount(player, potionItemId) > huntPotionReturnThreshold,
		inventoryPolicy.huntFreeCapacity(player),
		returnCapacityThreshold,
	};
	const bool currentReady = equipmentPolicy.loadoutReady(playerFacts, currentLoadout, readiness);
	const bool bagUpgrade = backpackItem && backpackItem->getID() == ITEM_BAG;
	Container* currentBackContainer = backpackItem ? backpackItem->getContainer() : nullptr;
	const PlayerBotBackpackAcquisition backpackPlan = equipmentPolicy.standardBackpackAcquisition(
		playerFacts, backpackItem ? backpackItem->getID() : 0, currentBackContainer,
		currentBackContainer ? static_cast<uint32_t>(currentBackContainer->size()) : 0,
		currentBackContainer ? currentBackContainer->capacity() : 0);
	const bool backpackAcquisition = backpackPlan.eligible;
	std::map<uint16_t, EquipmentOfferEvaluation> evaluatedItems;
	std::map<uint32_t, std::optional<PlayerBotRouteEstimate>> providerRoutes;
	std::set<uint32_t> providerRouteNodeLimits;
	std::vector<PlayerBotEquipmentProviderOfferSnapshot> plannerOffers;
	size_t simulatedItems = 0;
	bool providerRouteBudgetExhausted = false;
	size_t catalogOffers = 0;
	bool catalogTruncated = false;

	auto providerRoute = [&](Npc& npc) -> std::optional<PlayerBotRouteEstimate> {
		if (auto route = providerRoutes.find(npc.getID()); route != providerRoutes.end()) {
			return route->second;
		}
		if (providerRoutes.size() >= maximumEquipmentProviderRoutes) {
			providerRouteBudgetExhausted = true;
			return std::nullopt;
		}
		std::vector<Position> approaches;
		for (int32_t x = -3; x <= 3; ++x) {
			for (int32_t y = -3; y <= 3; ++y) {
				if (x != 0 || y != 0) {
					approaches.emplace_back(npc.getPosition().x + x, npc.getPosition().y + y, npc.getPosition().z);
				}
			}
		}
		std::sort(approaches.begin(), approaches.end(), [&position](const Position& left, const Position& right) {
			const int32_t leftDistance = std::max(Position::getDistanceX(position, left), Position::getDistanceY(position, left));
			const int32_t rightDistance = std::max(Position::getDistanceX(position, right), Position::getDistanceY(position, right));
			return leftDistance == rightDistance ? left < right : leftDistance < rightDistance;
		});
		size_t evaluatedApproaches = 0;
		for (size_t approachIndex = 0; approachIndex < approaches.size(); ++approachIndex) {
			const Position& approach = approaches[approachIndex];
			Tile* tile = g_game.map.getTile(approach);
			if (!tile || tile->queryAdd(0, player, 1, 0) != RETURNVALUE_NOERROR) {
				continue;
			}
			if (evaluatedApproaches >= maximumEquipmentProviderApproaches) {
				break;
			}
			++evaluatedApproaches;
			std::deque<PlayerBotNavigationStep> steps;
			uint64_t expandedNodes = 0;
			const PlayerBotNavigationRoutePlan routePlan = approach == position ? PlayerBotNavigationRoutePlan{} :
				planCompleteNavigationRoute(player, approach, {}, maximumEquipmentProviderPathNodes);
			const PlayerBotNavigationResult result = approach == position ? PlayerBotNavigationResult::Reached : routePlan.metrics.result;
			telemetry.recordPathfinding(std::chrono::microseconds::zero(), result == PlayerBotNavigationResult::Reached);
			if (approach != position) {
				steps = routePlan.steps;
				expandedNodes = routePlan.metrics.expandedNodes;
			}
			if (result == PlayerBotNavigationResult::Reached) {
				PlayerBotRouteEstimate route{true, false, approach,
				    approach == position ? 0 : static_cast<uint32_t>(routePlan.metrics.steps), expandedNodes,
				    approach == position ? 0 : routePlan.metrics.dangerCost,
				    approach == position ? 0 : routePlan.metrics.maximumHealthLossPerSecond};
				return providerRoutes.emplace(npc.getID(), route).first->second;
			}
			if (result == PlayerBotNavigationResult::NodeLimit) {
				providerRouteNodeLimits.insert(npc.getID());
			}
		}
		return providerRoutes.emplace(npc.getID(), std::nullopt).first->second;
	};
	struct CatalogOffer {
		Npc* npc;
		const ShopInfo* offer;
	};
	struct ProviderCatalog {
		Npc* npc;
		std::vector<const ShopInfo*> offers;
	};
	std::vector<Npc*> shopProviders = playerBotNpcProviders(g_game.getNpcs(), PlayerBotNpcCapability::Shop, position);
	const PlayerBotTopology& topology = PlayerBotTopology::instance();
	const PlayerBotTopologyDistances coarseDistances = topology.distancesFrom(
	    position, g_game.findItemOfType(&player, playerbot::ropeItemId, true) != nullptr,
	    g_game.findItemOfType(&player, 2554, true) != nullptr, player.getLevel());
	std::stable_sort(shopProviders.begin(), shopProviders.end(), [&topology, &coarseDistances](const Npc* left, const Npc* right) {
		const std::optional<uint32_t> leftCost = topology.distanceTo(
		    coarseDistances, PlayerBotNavigationGoal::withinRange(left->getPosition(), 3, 3));
		const std::optional<uint32_t> rightCost = topology.distanceTo(
		    coarseDistances, PlayerBotNavigationGoal::withinRange(right->getPosition(), 3, 3));
		if (leftCost.has_value() != rightCost.has_value()) return leftCost.has_value();
		return leftCost && rightCost && *leftCost != *rightCost && *leftCost < *rightCost;
	});
	Npc* backpackProvider = nullptr;
	if (backpackAcquisition) {
		for (Npc* provider : shopProviders) {
			const auto& offers = provider->getShopOffers();
			if (std::any_of(offers.begin(), offers.end(), [](const ShopInfo& offer) {
				return offer.itemId == ITEM_BACKPACK && offer.buyPrice != 0;
			})) {
				backpackProvider = provider;
				break;
			}
		}
	}
	const bool providersTruncated = shopProviders.size() > maximumEquipmentCatalogProviders;
	if (providersTruncated) {
		constexpr size_t nearbyProviders = maximumEquipmentCatalogProviders / 2;
		const size_t tailSize = shopProviders.size() - nearbyProviders;
		const size_t offset = equipmentProviderScanOffset % tailSize;
		std::vector<Npc*> selected(shopProviders.begin(), shopProviders.begin() + nearbyProviders);
		for (size_t index = 0; index < maximumEquipmentCatalogProviders - nearbyProviders; ++index) {
			selected.push_back(shopProviders[nearbyProviders + (offset + index) % tailSize]);
		}
		if (backpackProvider && std::find(selected.begin(), selected.end(), backpackProvider) == selected.end()) {
			selected.back() = backpackProvider;
		}
		equipmentProviderScanOffset = (offset + maximumEquipmentCatalogProviders - nearbyProviders) % tailSize;
		shopProviders = std::move(selected);
	} else {
		equipmentProviderScanOffset = 0;
	}
	std::vector<ProviderCatalog> providerCatalogs;
	std::vector<CatalogOffer> allCatalogOffers;
	std::vector<CatalogOffer> catalog;
	size_t loadedCatalogOffers = 0;
	for (Npc* npc : shopProviders) {
		ProviderCatalog provider{npc, {}};
		for (const ShopInfo& offer : npc->getShopOffers()) {
			const PlayerBotEquipmentItemSnapshot item = PlayerBotEquipmentAdapter::item(offer.itemId);
			if ((backpackAcquisition && offer.itemId == ITEM_BACKPACK) || item.head || item.armorSlot || item.legs || item.feet || item.left || item.right) {
				provider.offers.push_back(&offer);
			}
		}
		loadedCatalogOffers += provider.offers.size();
		if (!provider.offers.empty()) providerCatalogs.push_back(std::move(provider));
	}
	for (size_t offerIndex = 0;; ++offerIndex) {
		bool found = false;
		for (const ProviderCatalog& provider : providerCatalogs) {
			if (offerIndex >= provider.offers.size()) continue;
			found = true;
			allCatalogOffers.push_back({provider.npc, provider.offers[offerIndex]});
		}
		if (!found) break;
	}
	std::set<uint16_t> checkedCarriedItems;
	std::set<uint16_t> carriedCatalogItems;
	for (const CatalogOffer& candidate : allCatalogOffers) {
		if (checkedCarriedItems.insert(candidate.offer->itemId).second &&
		    g_game.findItemOfType(&player, candidate.offer->itemId, true)) {
			carriedCatalogItems.insert(candidate.offer->itemId);
		}
	}
	if (!allCatalogOffers.empty()) {
		const size_t offset = equipmentOfferScanOffset % allCatalogOffers.size();
		std::rotate(allCatalogOffers.begin(), allCatalogOffers.begin() + offset, allCatalogOffers.end());
		if (backpackAcquisition) {
			std::stable_partition(allCatalogOffers.begin(), allCatalogOffers.end(), [](const CatalogOffer& candidate) {
				return candidate.offer->itemId == ITEM_BACKPACK;
			});
		}
		std::stable_partition(allCatalogOffers.begin(), allCatalogOffers.end(), [&carriedCatalogItems, totalMoney, reserve](const CatalogOffer& candidate) {
			const uint64_t price = candidate.offer->buyPrice;
			return carriedCatalogItems.find(candidate.offer->itemId) != carriedCatalogItems.end() ||
			       (price != 0 && reserve != std::numeric_limits<uint64_t>::max() &&
			        totalMoney >= reserve && price <= totalMoney - reserve);
		});
		const size_t count = std::min(maximumEquipmentCatalogOffers, allCatalogOffers.size());
		catalog.assign(allCatalogOffers.begin(), allCatalogOffers.begin() + count);
		equipmentOfferScanOffset = (offset + count) % allCatalogOffers.size();
	}
	catalogTruncated = providersTruncated || loadedCatalogOffers > catalog.size();
	for (const CatalogOffer& catalogOffer : catalog) {
		Npc* npc = catalogOffer.npc;
		const ShopInfo& offer = *catalogOffer.offer;
			++catalogOffers;
			EquipmentOfferEvaluation evaluation;
			const bool backpackOffer = backpackAcquisition && offer.itemId == ITEM_BACKPACK;
			const bool carried = !backpackOffer && carriedCatalogItems.find(offer.itemId) != carriedCatalogItems.end();
			if (auto item = evaluatedItems.find(offer.itemId); item != evaluatedItems.end()) {
				evaluation = item->second;
			} else if (backpackOffer) {
				evaluation.itemId = ITEM_BACKPACK;
				evaluation.slot = CONST_SLOT_BACKPACK;
				evaluation.replacedItemId = bagUpgrade ? ITEM_BAG : 0;
				evaluation.profile = currentProfile;
				evaluation.hunts = currentHunts;
				evaluation.currentReady = currentReady;
				evaluation.candidateReady = equipmentPolicy.loadoutReady(playerFacts, currentLoadout,
					{true, readiness.suppliesReady, readiness.effectiveFreeCapacity, readiness.minimumFreeCapacity},
					Item::items[offer.itemId].weight);
				evaluation.backpackAcquisition = true;
				evaluation.bagUpgrade = bagUpgrade;
				evaluation.rule = PlayerBotEquipmentDecisionRule::ReadinessRepair;
				evaluatedItems.emplace(offer.itemId, evaluation);
			} else {
				evaluation = equipmentPolicy.evaluateCandidate(
					playerFacts, PlayerBotEquipmentAdapter::item(offer.itemId), currentLoadout, currentProfile, currentHunts, currentReady, readiness,
					carried ? 0 : Item::items[offer.itemId].weight,
					simulatedItems < maximumEquipmentCandidateSimulations,
					[this, &player](const PlayerBotCombatProfile& profile) {
						return equipmentHuntSummary(player, profile);
					});
				if (evaluation.simulated) {
					++simulatedItems;
				}
				evaluatedItems.emplace(offer.itemId, evaluation);
			}
			evaluation.npcId = npc->getID();
			evaluation.npcPosition = npc->getPosition();
			evaluation.price = offer.buyPrice;
			evaluation.carried = carried;
			Item* backpackItem = player.getInventoryItem(CONST_SLOT_BACKPACK);
			Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
			const uint32_t freeBackpackSlots = backpack ? backpack->capacity() -
			                                  std::min<uint32_t>(backpack->capacity(), backpack->size()) : 0;
			uint32_t displacedSlots = 0;
			std::set<slots_t> countedSlots;
			for (const auto& displaced : {std::pair<slots_t, uint16_t>{evaluation.slot, evaluation.replacedItemId},
			                              {CONST_SLOT_LEFT, evaluation.displacedLeftItemId},
			                              {CONST_SLOT_RIGHT, evaluation.displacedRightItemId}}) {
				if (displaced.second != 0 && countedSlots.insert(displaced.first).second) {
					++displacedSlots;
				}
			}
			const uint32_t requiredBackpackSlots = displacedSlots + (evaluation.carried ? 0 : 1);
			if (evaluation.carried) {
				evaluation.travelSteps = 0;
				plannerOffers.push_back({evaluation, Item::items[offer.itemId].weight, freeBackpackSlots, backpack != nullptr,
				                         true, false, {true, false, Position(), 0, 0}});
				continue;
			}
			const std::optional<PlayerBotRouteEstimate> route = evaluation.rejection.empty() ?
				providerRoute(*npc) : std::nullopt;
			if (route) {
				evaluation.approachPosition = route->approachPosition;
				evaluation.travelSteps = route->steps;
			}
			plannerOffers.push_back({evaluation, Item::items[offer.itemId].weight, freeBackpackSlots, backpack != nullptr,
			                         offer.buyPrice != 0, providerRouteBudgetExhausted,
			                         route.value_or(PlayerBotRouteEstimate{false,
			                             providerRouteNodeLimits.find(npc->getID()) != providerRouteNodeLimits.end()})});
	}
	const PlayerBotNavigationRiskProfile risk;
	const PlayerBotEquipmentProviderPlannerSnapshot plannerSnapshot{equipmentPolicy.requiresKnightCombatReadiness(playerFacts), reserve,
	    totalMoney, reserve != std::numeric_limits<uint64_t>::max(), player.getFreeCapacity(),
	    static_cast<uint32_t>(risk.maximumRouteHealthLoss * risk.healthLossCost),
	    risk.maximumHealthLossPerSecond, plannerOffers};
	const PlayerBotEquipmentProviderDecision plannerDecision = equipmentProviderPlanner.select(plannerSnapshot);
	if (!plannerDecision.evaluated) return std::nullopt;
	const std::optional<EquipmentOfferEvaluation>& selected = plannerDecision.selected;
	for (size_t offerIndex = 0; offerIndex < plannerOffers.size(); ++offerIndex) {
		const auto& candidate = plannerOffers[offerIndex];
		const auto rejection = std::find_if(plannerDecision.rejections.begin(), plannerDecision.rejections.end(),
		                                    [offerIndex](const PlayerBotPlannerOfferRejection& result) {
			                                    return result.offerIndex == offerIndex;
		                                    });
		emitEquipmentOffer(player, candidate.evaluation, currentProfile, currentHunts, reserve, position,
		                   rejection == plannerDecision.rejections.end() ? "feasible" : "rejected",
		                   rejection == plannerDecision.rejections.end() ? nullptr : rejection->reason.c_str());
	}
	std::ostringstream fields;
	fields << "\"result\":" << jsonString(selected ? (selected->carried ? "would_equip" : "would_buy") : "no_decision")
	       << ",\"feasible_candidates\":" << (selected ? 1 : 0)
	       << ",\"catalog_offers_evaluated\":" << catalogOffers
	       << ",\"catalog_truncated\":" << (catalogTruncated ? "true" : "false")
	       << ",\"reason\":" << jsonString(selected ? PlayerBotEquipmentPolicy::decisionRuleName(selected->rule) : "no_justified_offer");
	if (selected) {
		fields << ",\"npc_id\":" << selected->npcId << ",\"item_id\":" << selected->itemId
		       << ",\"price\":" << selected->price << ",\"travel_steps\":" << selected->travelSteps;
	}
	emit("equipment_offer_shadow", position, fields.str());
	return selected;
}

void PlayerBotController::beginEquipmentPurchase(Player& player, const Position& position,
	EquipmentOfferEvaluation evaluation)
{
	backpackUpgradeDepot.reset();
	if (evaluation.bagUpgrade) {
		depotWorkflow.reset();
		sellLootPlan.reset();
	}
	backpackUpgradeToken = 0;
	backpackUpgradeDepotId = 0;
	progressionRuntime.beginEquipmentPurchase(std::move(evaluation));
	const auto& purchase = progressionRuntime.equipmentPurchase().plan();
	if (!purchase.carried) {
		resetNavigation();
	}
	std::ostringstream fields;
	fields << "\"goal\":\"buy_equipment\",\"reason\":"
	       << jsonString(PlayerBotEquipmentPolicy::decisionRuleName(purchase.rule))
	       << ",\"npc_id\":" << purchase.npcId << ",\"item_id\":" << purchase.itemId
	       << ",\"price\":" << purchase.price << ",\"travel_steps\":" << purchase.travelSteps
	       << ",\"acquisition\":" << jsonString(purchase.carried ? "carried" : "purchase")
	       << ",\"backpack_acquisition\":" << (purchase.backpackAcquisition ? "true" : "false")
	       << ",\"bag_upgrade\":" << (purchase.bagUpgrade ? "true" : "false");
	emit("strategy_selection", position, fields.str());
	say(player, purchase.carried ? "Equipping a carried equipment upgrade." :
	                                      "Going to buy a justified equipment upgrade.");
}

void PlayerBotController::finishEquipmentPurchase(Player* player, const Position& position, const char* result,
	const char* reason)
{
	// Recovery reasons can belong to the session that finish() clears below.
	const std::string completionReason = reason;
	reason = completionReason.c_str();
	const auto& purchase = progressionRuntime.equipmentPurchase().plan();
	std::ostringstream fields;
	fields << "\"goal\":\"buy_equipment\",\"npc_id\":" << purchase.npcId
	       << ",\"item_id\":" << purchase.itemId << ",\"price\":" << purchase.price
	       << ",\"rule\":" << jsonString(PlayerBotEquipmentPolicy::decisionRuleName(purchase.rule))
	       << ",\"backpack_acquisition\":" << (purchase.backpackAcquisition ? "true" : "false")
	       << ",\"bag_upgrade\":" << (purchase.bagUpgrade ? "true" : "false")
	       << ",\"result\":" << jsonString(result) << ",\"reason\":" << jsonString(reason);
	emit("strategy_objective_result", position, fields.str());
	emit("goal_result", position,
	     "\"decision_id\":" + std::to_string(progressionRuntime.decisionId()) +
	         ",\"goal\":\"buy_equipment\",\"result\":" + jsonString(result) +
	         ",\"reason\":" + jsonString(reason));
	const bool succeeded = std::strcmp(result, "success") == 0;
	if (player) {
		if (succeeded) {
			Npc* npc = g_game.getNpcByID(purchase.npcId);
			if (npc && !npc->isRemoved()) {
				npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "bye");
			}
		}
		player->closeShopWindow();
		say(*player, std::string("Equipment purchase ") + result + ": " + reason + '.');
	}
	const bool unresolvedPreservation = purchase.bagUpgrade && !succeeded && std::strncmp(reason, "old_bag_", 8) == 0;
	const bool terminalFailure = unresolvedPreservation || std::strcmp(reason, "transaction_delta_mismatch") == 0;
	progressionRuntime.completeEquipmentPurchase(succeeded,
	    succeeded ? equipmentPurchaseSuccessCooldown : equipmentPurchaseFailureCooldown);
	if (player && purchase.bagUpgrade && !unresolvedPreservation) {
		clearBackpackUpgradePersistence(*player, true);
	} else if (!purchase.bagUpgrade) {
		backpackUpgradeDepot.reset();
		backpackUpgradeDepotId = 0;
		backpackUpgradeToken = 0;
	}
	progressionRuntime.finish();
	resetNavigation();
	if (terminalFailure) {
		stop((std::string("equipment_purchase_") + reason).c_str(), position);
		return;
	}
	turnRouter.setCyclePhase(CyclePhase::Service);
	if (!succeeded && player && std::strcmp(reason, "healing_supply_missing") == 0) {
		beginService(player, position, reason);
		schedule(SCHEDULER_MINTICKS);
		return;
	}
	if (succeeded && player && fixtureDriver.equipmentPurchaseCompletion(*player).pause) {
		return;
	}
	if (player && fixtureDriver.progressionGoalLoop(true).selectGoal) {
		selectTopLevelGoal(*player, position, succeeded ? "equipment_purchase_complete" : "equipment_purchase_failed");
	} else {
		progressionRuntime.enterService();
	}
	schedule(SCHEDULER_MINTICKS);
}

void PlayerBotController::processEquipmentPurchase(Player* player, const Position& position)
{
	const auto& purchase = progressionRuntime.equipmentPurchase().plan();
	Npc* npc = purchase.carried ? nullptr : g_game.getNpcByID(purchase.npcId);
	const ServiceNpc provider{purchase.npcId, npc ? npc->getPosition() : Position()};
	const ShopInfo* offer = purchase.carried || !npc ? nullptr : findOffer(provider, purchase.itemId, true);
	int32_t onBuy = 0;
	int32_t onSell = 0;
	Npc* shopOwner = player->getShopOwner(onBuy, onSell);
	PlayerBotEquipmentPurchaseObservation observation;
	observation.actionAvailable = player->canDoAction();
	if (purchase.backpackAcquisition) {
		Item* backItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
		observation.backpackReceiptSafe = backItem == nullptr;
		uint32_t markerMatches = 0;
		Item* taggedBag = taggedBackpackUpgradeBag(*player, &markerMatches);
		if (backpackUpgradeToken > 0 && markerMatches != 1) {
			finishEquipmentPurchase(player, position, "failed", markerMatches == 0 ?
				"old_bag_marker_missing" : "old_bag_marker_duplicate");
			return;
		}
		uint32_t replacementMatches = 0;
		Item* replacementBag = taggedBackpackUpgradeReplacementBag(*player, &replacementMatches);
		if (replacementMatches > 1) {
			finishEquipmentPurchase(player, position, "failed", "old_bag_replacement_marker_duplicate");
			return;
		}
		observation.depotAvailable = markerMatches <= 1;
		observation.oldBagEquipped = purchase.bagUpgrade && taggedBag && taggedBag == backItem;
		observation.replacementBagEquipped = purchase.bagUpgrade && backItem && backItem->getID() == ITEM_BAG && backItem != taggedBag;
		Container* acquired = backItem && backItem->getID() == ITEM_BACKPACK ? backItem->getContainer() : nullptr;
		observation.equipmentVerified = acquired != nullptr;
		observation.equipmentAvailable = acquired != nullptr;
		observation.backpackDestinationOpen = acquired && player->getContainerID(acquired) >= 0;
		for (Cylinder* parent = taggedBag ? taggedBag->getParent() : nullptr; parent; parent = parent->getParent()) {
			if (parent == acquired) { observation.oldBagPreserved = true; break; }
		}
		if (backpackUpgradeDepotId != 0) {
			DepotChest* chest = player->getDepotChest(backpackUpgradeDepotId, false);
			observation.depotAvailable = observation.depotAvailable && chest != nullptr;
			observation.depotOpen = chest && player->getContainerByID(depotChestContainerId) == chest;
			observation.oldBagAtDepot = chest && taggedBag && taggedBag->getParent() == chest;
			observation.replacementBagAtDepot = chest && replacementBag && replacementBag->getParent() == chest;
		}
	}
	if (!purchase.carried) {
		observation.providerAvailable = npc && playerBotNpcHasCapability(*npc, PlayerBotNpcCapability::Shop);
		observation.offerAvailable = offer && offer->buyPrice == purchase.price;
		observation.providerInRange = npc && Position::areInRange<3, 3, 0>(position, npc->getPosition());
		observation.shopReady = shopOwner == npc && !player->getShopItemList().empty();
		observation.otherShopOpen = shopOwner && shopOwner != npc;
		const uint64_t reserve = spellTrainingReserve(*player);
		const uint64_t money = player->getMoney() + player->getBankBalance();
		observation.fundingAvailable = reserve != std::numeric_limits<uint64_t>::max() && money >= purchase.price && money - purchase.price >= reserve;
		observation.bankFundingAvailable = reserve != std::numeric_limits<uint64_t>::max() &&
			player->getBankBalance() >= purchase.price && player->getBankBalance() - purchase.price >= reserve;
	}
	const PlayerBotEquipmentPurchaseStage equipmentStage = progressionRuntime.equipmentPurchase().stage();
	if (purchase.bagUpgrade && (equipmentStage == PlayerBotEquipmentPurchaseStage::TravelDepot ||
	    equipmentStage == PlayerBotEquipmentPurchaseStage::ReturnDepot)) {
		if (!backpackUpgradeDepot) {
			if (!discoverDepot(*player, position)) return;
			const PlayerBotDepotSnapshot selected = depotWorkflow.snapshot();
			if (!selected.hasSelectedDepot) { schedule(blockedRouteRetryInterval); return; }
			backpackUpgradeDepot = selected.selected;
			backpackUpgradeDepotId = selected.selected.depotId;
			if (backpackUpgradeToken == 0) {
				Item* bag = player->getInventoryItem(CONST_SLOT_BACKPACK);
				int32_t previousToken = 0;
				player->getStorageValue(backpackUpgradeTokenStorage, previousToken);
				backpackUpgradeToken = previousToken >= INT32_MAX ? 1 : std::max<int32_t>(1, previousToken + 1);
				if (!bag || bag->getID() != ITEM_BAG) {
					finishEquipmentPurchase(player, position, "failed", "old_bag_tag_source_unavailable");
					return;
				}
				std::string marker = backpackUpgradeMarker;
				bag->setCustomAttribute(marker, static_cast<int64_t>(backpackUpgradeToken));
				player->addStorageValue(backpackUpgradeTokenStorage, backpackUpgradeToken);
				player->addStorageValue(backpackUpgradeDepotStorage, backpackUpgradeDepotId);
				player->addStorageValue(backpackUpgradeStatusStorage, backpackUpgradeTagged);
				emit("backpack_upgrade_persistence", position, "\"result\":\"tagged\",\"depot_id\":" +
					std::to_string(backpackUpgradeDepotId) + ",\"token\":" + std::to_string(backpackUpgradeToken));
			}
		}
		observation.depotReached = Position::areInRange<1, 1, 0>(position, backpackUpgradeDepot->approachPosition);
		if (!observation.depotReached) {
			if (!processNavigation(player, position, backpackUpgradeDepot->approachPosition)) {
				observation.depotNavigationFailed = navigationRuntime.fixedTargetRouteFailureCount() >= maximumProgressionAttempts ||
					navigationRuntime.stepFailureCount() >= maximumRepeatedNavigationStepFailures;
			}
		}
	}
	if (equipmentStage == PlayerBotEquipmentPurchaseStage::Travel) {
		bool approachUnavailable = false;
		observation.navigationReached = processNpcApproach(player, position, npc, purchase.approachPosition, approachUnavailable);
		observation.navigationFailed = approachUnavailable ||
		                              navigationRuntime.fixedTargetRouteFailureCount() >= maximumProgressionAttempts ||
		                              navigationRuntime.stepFailureCount() >= maximumRepeatedNavigationStepFailures;
	} else if (!purchase.bagUpgrade && progressionRuntime.equipmentPurchase().stage() == PlayerBotEquipmentPurchaseStage::Purchase &&
	           observation.providerAvailable && observation.offerAvailable && !observation.providerInRange) {
		progressionRuntime.restartEquipmentConversation();
		player->closeShopWindow(false);
		bool approachUnavailable = false;
		processNpcApproach(player, position, npc, purchase.approachPosition, approachUnavailable);
		if (approachUnavailable) finishEquipmentPurchase(player, position, "failed", "route_unavailable");
		return;
	} else if (equipmentStage == PlayerBotEquipmentPurchaseStage::Equip ||
	           equipmentStage == PlayerBotEquipmentPurchaseStage::VerifyEquipment ||
	           equipmentStage == PlayerBotEquipmentPurchaseStage::RetrieveBackpack ||
	           equipmentStage == PlayerBotEquipmentPurchaseStage::VerifyBackpackRetrieved) {
		Item* purchased = g_game.findItemOfType(player, purchase.itemId, true);
		Item* equipped = player->getInventoryItem(purchase.slot);
		observation.equipmentVerified = equipped && equipped->getID() == purchase.itemId;
		observation.equipmentAvailable = purchased;
		Container* sourceContainer = purchased ? dynamic_cast<Container*>(purchased->getParent()) : nullptr;
		if (sourceContainer && player->getContainerID(sourceContainer) < 0) {
			observation.openContainerRequired = true;
			Container* container = sourceContainer;
			while (Container* parent = dynamic_cast<Container*>(container->getParent())) {
				if (player->getContainerID(parent) >= 0) break;
				container = parent;
			}
			uint8_t id = rewardContainerIdBase;
			while (id <= maximumContainerId && player->getContainerByID(id)) ++id;
			Position source;
			uint8_t index = 0;
			g_game.internalGetPosition(static_cast<Item*>(container), source, index);
			observation.containerAccessAvailable = id <= maximumContainerId && source.x == 0xFFFF;
		}
		if (purchased) {
			Position source;
			uint8_t index = 0;
			g_game.internalGetPosition(purchased, source, index);
			observation.equipmentPositionAvailable = source.x == 0xFFFF;
		}
		for (const auto& [slot, itemId] : {std::pair<slots_t, uint16_t>{purchase.slot, purchase.replacedItemId},
		                                  {CONST_SLOT_LEFT, purchase.displacedLeftItemId}, {CONST_SLOT_RIGHT, purchase.displacedRightItemId}}) {
			Item* item = itemId == 0 ? nullptr : player->getInventoryItem(slot);
			if (item && item->getID() == itemId && item != purchased) { observation.displacedMoveRequired = true; break; }
		}
		for (uint16_t itemId : {purchase.replacedItemId, purchase.displacedLeftItemId, purchase.displacedRightItemId}) {
			if (itemId != 0) observation.displacedCounts[itemId] = inventoryPolicy.inventoryItemCount(*player, itemId);
		}
	}
	observation.itemCount = inventoryPolicy.inventoryItemCount(*player, purchase.itemId);
	observation.money = player->getMoney();
	observation.bankBalance = player->getBankBalance();
	const PlayerBotProgressionOutcome result = progressionRuntime.advanceEquipmentPurchase(observation, maximumProgressionAttempts);
	if (purchase.bagUpgrade && observation.oldBagAtDepot) {
		int32_t persistedStatus = -1;
		player->getStorageValue(backpackUpgradeStatusStorage, persistedStatus);
		if (persistedStatus == backpackUpgradeTagged) {
			player->addStorageValue(backpackUpgradeStatusStorage, backpackUpgradeStaged);
			emit("backpack_upgrade_persistence", position, "\"result\":\"staged\",\"depot_id\":" +
				std::to_string(backpackUpgradeDepotId) + ",\"token\":" + std::to_string(backpackUpgradeToken));
		}
	}
	if (result.transaction.amount != 0) {
		if (purchase.bagUpgrade) {
			player->addStorageValue(backpackUpgradeStatusStorage, backpackUpgradePurchased);
			emit("backpack_upgrade_persistence", position, "\"result\":\"purchased\",\"depot_id\":" +
				std::to_string(backpackUpgradeDepotId) + ",\"token\":" + std::to_string(backpackUpgradeToken));
		}

		if (purchase.bagUpgrade) resetNavigation();
		emit("action_result", position, "\"action\":\"buy_equipment\",\"result\":\"success\",\"item_id\":" +
			std::to_string(purchase.itemId) + ",\"price\":" + std::to_string(purchase.price) + ",\"carried_before\":" +
			std::to_string(result.transaction.money) + ",\"carried_after\":" + std::to_string(player->getMoney()) + ",\"bank_before\":" +
			std::to_string(result.transaction.balance) + ",\"bank_after\":" + std::to_string(player->getBankBalance()));
	}
	if (result.type == PlayerBotProgressionOutcomeType::Succeeded || result.type == PlayerBotProgressionOutcomeType::Failed) {
		if (result.type == PlayerBotProgressionOutcomeType::Failed && std::strcmp(result.reason, "transaction_delta_mismatch") == 0) {
			logActionFailure("buy_equipment", result.reason, position);
		}
		if (result.type == PlayerBotProgressionOutcomeType::Failed && std::strcmp(result.reason, "transaction_rejected") == 0) logActionFailure("buy_equipment", result.reason, position);
		if (result.type == PlayerBotProgressionOutcomeType::Failed && std::strcmp(result.reason, "shop_window_unavailable") == 0) {
			logActionFailure("shop", result.command.reason, position);
		}
		if (result.type == PlayerBotProgressionOutcomeType::Succeeded) {
			const PlayerBotEquipmentPlayerSnapshot playerFacts = PlayerBotEquipmentAdapter::player(*player);
			const EquipmentLoadout loadout = PlayerBotEquipmentAdapter::loadout(*player);
			const EquipmentHuntSummary hunts = equipmentHuntSummary(*player, equipmentPolicy.combatProfile(playerFacts, loadout));
			const uint16_t potionItemId = recoveryPotionItemId(player->getVocationId());
			emit("action_result", position, "\"action\":\"equip_equipment\",\"result\":\"success\",\"item_id\":" + std::to_string(purchase.itemId) +
				",\"slot\":" + std::to_string(purchase.slot) + ",\"combat_ready\":" +
				(equipmentPolicy.loadoutReady(playerFacts, loadout,
				    {player->getInventoryItem(CONST_SLOT_BACKPACK) && player->getInventoryItem(CONST_SLOT_BACKPACK)->getContainer(),
				     inventoryPolicy.inventoryItemCount(*player, potionItemId) > huntPotionReturnThreshold,
				     inventoryPolicy.huntFreeCapacity(*player), returnCapacityThreshold}) ? "true" : "false") +
				",\"suitable_regions\":" + std::to_string(hunts.suitableRegions) +
				",\"displaced_items_preserved\":true");
		}
		finishEquipmentPurchase(player, position, result.type == PlayerBotProgressionOutcomeType::Succeeded ? "success" : "failed", result.reason);
		return;
	}
	if (result.command.type == PlayerBotProgressionCommandType::Shop && std::strcmp(result.command.reason, "open_shop") == 0) {
		const PlayerBotEquipmentShopCommand shop = progressionRuntime.advanceEquipmentShop(
			{observation.shopReady, observation.otherShopOpen}, maximumServiceAttempts);
		if (shop.result == PlayerBotNpcSessionResult::Failed) {
			logActionFailure("shop", shop.failureReason, position);
			if (purchase.bagUpgrade && observation.oldBagAtDepot) {
				progressionRuntime.beginEquipmentBackpackRecovery("shop_window_unavailable", observation.equipmentVerified);
				player->closeShopWindow(false);
				resetNavigation();
				schedule(SCHEDULER_MINTICKS);
			} else finishEquipmentPurchase(player, position, "failed", "shop_window_unavailable");
			return;
		}
		if (shop.closeOtherShop) player->closeShopWindow(false);
		if (shop.speech && npc && !npc->isRemoved()) {
			telemetry.recordActionAttempt();
			npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, shop.speech);
		}
		schedule(shop.delay == 0 ? SCHEDULER_MINTICKS : shop.delay);
		return;
	}
	if (result.command.type == PlayerBotProgressionCommandType::Shop) {
		telemetry.recordActionAttempt();
		const PlayerBotFixtureEngineCommand command = fixtureDriver.equipmentPurchaseCommand();
		if (command.dispatch) g_game.playerPurchaseItem(playerId, Item::items[purchase.itemId].clientId,
			static_cast<uint8_t>(offer->subType), command.count, false, false);
	}
	if (result.command.type == PlayerBotProgressionCommandType::Open) {
		Container* container = nullptr;
		const char* action = "open_equipment_container";
		if (result.command.reason && std::strcmp(result.command.reason, "open_backpack_depot") == 0) {
			if (!backpackUpgradeDepot || !Position::areInRange<1, 1, 0>(position, backpackUpgradeDepot->approachPosition)) {
				schedule(blockedRouteRetryInterval);
				return;
			}
			PlayerBotDepotSnapshot depot;
			depot.hasSelectedDepot = true;
			depot.selected = *backpackUpgradeDepot;
			if (!openDepotLocker(*player, depot, position)) return;
			Container* locker = player->getContainerByID(depotLockerContainerId);
			DepotChest* chest = player->getDepotChest(backpackUpgradeDepot->depotId, false);
			if (!locker || !chest || locker->getThingIndex(chest) < 0) {
				schedule(navigationDecisionDelay(*player));
				return;
			}
			if (!openDepotChest(*player, depot, position)) return;
			schedule(SCHEDULER_MINTICKS);
			return;
		} else if (result.command.reason && std::strcmp(result.command.reason, "open_backpack_upgrade_destination") == 0) {
			Item* backpack = player->getInventoryItem(CONST_SLOT_BACKPACK);
			container = backpack ? backpack->getContainer() : nullptr;
			action = "open_backpack_upgrade_destination";
		} else {
			Item* purchased = g_game.findItemOfType(player, purchase.itemId, true);
			container = purchased ? dynamic_cast<Container*>(purchased->getParent()) : nullptr;
			while (container && dynamic_cast<Container*>(container->getParent()) && player->getContainerID(dynamic_cast<Container*>(container->getParent())) < 0) container = dynamic_cast<Container*>(container->getParent());
		}
		if (container) {
			uint8_t id = rewardContainerIdBase;
			while (id <= maximumContainerId && player->getContainerByID(id)) ++id;
			Position source; uint8_t index = 0; Item* item = static_cast<Item*>(container);
			g_game.internalGetPosition(item, source, index);
			telemetry.recordActionAttempt();
			g_game.playerUseItem(playerId, source, index, id, item->getClientID());
			emit("action_result", position, "\"action\":" + jsonString(action) + ",\"result\":\"requested\",\"item_id\":" + std::to_string(item->getID()) + ",\"container_id\":" + std::to_string(id));
		}
	}
	if (result.command.type == PlayerBotProgressionCommandType::Equip) {
		Item* item = g_game.findItemOfType(player, purchase.itemId, true);
		slots_t slot = purchase.slot;
		Container* destination = nullptr;
		const bool stageOldBag = result.command.reason && std::strcmp(result.command.reason, "depot_stage_old_bag") == 0;
		const bool stageReplacementBag = result.command.reason && std::strcmp(result.command.reason, "depot_stage_replacement_bag") == 0;
		const bool retrieveOldBag = result.command.reason && std::strcmp(result.command.reason, "depot_retrieve_old_bag") == 0;
		const bool restoreOldBag = result.command.reason && std::strcmp(result.command.reason, "depot_restore_old_bag") == 0;
		DepotChest* depotChest = backpackUpgradeDepot ? player->getDepotChest(backpackUpgradeDepot->depotId, false) : nullptr;
		if (stageOldBag) {
			Item* equippedBag = player->getInventoryItem(CONST_SLOT_BACKPACK);
			item = equippedBag && hasBackpackUpgradeToken(*equippedBag, backpackUpgradeMarker, backpackUpgradeToken) ? equippedBag : nullptr;
			destination = depotChest;
		} else if (stageReplacementBag) {
			Item* equippedBag = player->getInventoryItem(CONST_SLOT_BACKPACK);
			item = equippedBag && equippedBag->getID() == ITEM_BAG ? equippedBag : nullptr;
			if (item) {
				if (Item* previous = taggedBackpackUpgradeReplacementBag(*player); previous && previous != item) {
					previous->removeCustomAttribute(backpackUpgradeReplacementMarker);
				}
				std::string marker = backpackUpgradeReplacementMarker;
				item->setCustomAttribute(marker, static_cast<int64_t>(backpackUpgradeToken));
			}
			destination = depotChest;
		} else if (retrieveOldBag || restoreOldBag) {
			Item* taggedBag = taggedBackpackUpgradeBag(*player);
			item = taggedBag && taggedBag->getParent() == depotChest ? taggedBag : nullptr;
			if (retrieveOldBag) {
				Item* backpack = player->getInventoryItem(CONST_SLOT_BACKPACK);
				destination = backpack ? backpack->getContainer() : nullptr;
			}
		}
		if (result.command.reason && std::strcmp(result.command.reason, "preserve_displaced_equipment") == 0) {
			for (const auto& [candidateSlot, itemId] : {std::pair<slots_t, uint16_t>{purchase.slot, purchase.replacedItemId}, {CONST_SLOT_LEFT, purchase.displacedLeftItemId}, {CONST_SLOT_RIGHT, purchase.displacedRightItemId}}) {
				Item* candidate = itemId == 0 ? nullptr : player->getInventoryItem(candidateSlot);
				if (candidate && candidate->getID() == itemId && candidate != item) { item = candidate; slot = candidateSlot; break; }
			}
		}
		if (item && (!stageOldBag && !stageReplacementBag && !retrieveOldBag || destination)) {
			Position source; uint8_t index = 0; g_game.internalGetPosition(item, source, index); telemetry.recordActionAttempt();
			const char* action = stageOldBag ? "depot_stage_old_bag" : stageReplacementBag ? "depot_stage_replacement_bag" : retrieveOldBag ? "depot_retrieve_old_bag" :
				restoreOldBag ? "depot_restore_old_bag" :
				(result.command.reason && std::strcmp(result.command.reason, "preserve_displaced_equipment") == 0 ? "preserve_displaced_equipment" : "equip_equipment");
			Position target(0xFFFF, purchase.slot, 0);
			if (stageOldBag || stageReplacementBag || retrieveOldBag) {
				const int8_t containerId = player->getContainerID(destination);
				if (containerId < 0) { schedule(navigationDecisionDelay(*player)); return; }
				target = Position(0xFFFF, 0x40 | static_cast<uint8_t>(containerId), containerDestinationIndex(*destination, *item));
			} else if (restoreOldBag) {
				target = Position(0xFFFF, CONST_SLOT_BACKPACK, 0);
			} else if (result.command.reason && std::strcmp(result.command.reason, "preserve_displaced_equipment") == 0) {
				target = Position(0xFFFF, 0, 0);
			}
			emit("action_result", position, "\"action\":" + jsonString(action) +
				",\"result\":\"requested\",\"item_id\":" + std::to_string(item->getID()) + ",\"slot\":" + std::to_string(slot));
			g_game.playerMoveItem(player, source, item->getClientID(), index, target,
				item->getItemCount(), item, destination);
		}
	}
	if (result.command.type == PlayerBotProgressionCommandType::Navigate) return;
	if (result.reason && std::strcmp(result.reason, "restore_old_bag") == 0) resetNavigation();
	const bool actionIssued = result.command.type == PlayerBotProgressionCommandType::Shop ||
	                          result.command.type == PlayerBotProgressionCommandType::Open ||
	                          result.command.type == PlayerBotProgressionCommandType::Equip;
	schedule(actionIssued || (result.command.type == PlayerBotProgressionCommandType::None && result.reason &&
	                          std::strcmp(result.reason, "action_unavailable") == 0) ? navigationDecisionDelay(*player) : SCHEDULER_MINTICKS);
}
