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
	const Item* backpackItem = player.getInventoryItem(CONST_SLOT_BACKPACK);
	const uint16_t potionItemId = recoveryPotionItemId(player.getVocationId());
	const PlayerBotEquipmentReadinessInput readiness{
		backpackItem && backpackItem->getContainer(),
		inventoryPolicy.inventoryItemCount(player, potionItemId) > huntPotionReturnThreshold,
		inventoryPolicy.huntFreeCapacity(player),
		returnCapacityThreshold,
	};
	const bool currentReady = equipmentPolicy.loadoutReady(playerFacts, currentLoadout, readiness);
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
	const bool providersTruncated = shopProviders.size() > maximumEquipmentCatalogProviders;
	if (providersTruncated) {
		constexpr size_t nearbyProviders = maximumEquipmentCatalogProviders / 2;
		const size_t tailSize = shopProviders.size() - nearbyProviders;
		const size_t offset = equipmentProviderScanOffset % tailSize;
		std::vector<Npc*> selected(shopProviders.begin(), shopProviders.begin() + nearbyProviders);
		for (size_t index = 0; index < maximumEquipmentCatalogProviders - nearbyProviders; ++index) {
			selected.push_back(shopProviders[nearbyProviders + (offset + index) % tailSize]);
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
			if (item.head || item.armorSlot || item.legs || item.feet || item.left || item.right) {
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
			const bool carried = carriedCatalogItems.find(offer.itemId) != carriedCatalogItems.end();
			if (auto item = evaluatedItems.find(offer.itemId); item != evaluatedItems.end()) {
				evaluation = item->second;
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
	       << ",\"acquisition\":" << jsonString(purchase.carried ? "carried" : "purchase");
	emit("strategy_selection", position, fields.str());
	say(player, purchase.carried ? "Equipping a carried equipment upgrade." :
	                                      "Going to buy a justified equipment upgrade.");
}

void PlayerBotController::finishEquipmentPurchase(Player* player, const Position& position, const char* result,
	const char* reason)
{
	const auto& purchase = progressionRuntime.equipmentPurchase().plan();
	std::ostringstream fields;
	fields << "\"goal\":\"buy_equipment\",\"npc_id\":" << purchase.npcId
	       << ",\"item_id\":" << purchase.itemId << ",\"price\":" << purchase.price
	       << ",\"rule\":" << jsonString(PlayerBotEquipmentPolicy::decisionRuleName(purchase.rule))
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
	progressionRuntime.completeEquipmentPurchase(succeeded,
	    succeeded ? equipmentPurchaseSuccessCooldown : equipmentPurchaseFailureCooldown);
	progressionRuntime.finish();
	resetNavigation();
	turnRouter.setCyclePhase(CyclePhase::Service);
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
	if (!purchase.carried) {
		observation.providerAvailable = npc && playerBotNpcHasCapability(*npc, PlayerBotNpcCapability::Shop);
		observation.offerAvailable = offer && offer->buyPrice == purchase.price;
		observation.providerInRange = npc && Position::areInRange<3, 3, 0>(position, npc->getPosition());
		observation.shopReady = shopOwner == npc && !player->getShopItemList().empty();
		observation.otherShopOpen = shopOwner && shopOwner != npc;
		const uint64_t reserve = spellTrainingReserve(*player);
		const uint64_t money = player->getMoney() + player->getBankBalance();
		observation.fundingAvailable = reserve != std::numeric_limits<uint64_t>::max() && money >= purchase.price && money - purchase.price >= reserve;
	}
	if (progressionRuntime.equipmentPurchase().stage() == PlayerBotEquipmentPurchaseStage::Travel) {
		bool approachUnavailable = false;
		observation.navigationReached = processNpcApproach(player, position, npc, purchase.approachPosition, approachUnavailable);
		observation.navigationFailed = approachUnavailable ||
		                              navigationRuntime.fixedTargetRouteFailureCount() >= maximumProgressionAttempts ||
		                              navigationRuntime.stepFailureCount() >= maximumRepeatedNavigationStepFailures;
	} else if (progressionRuntime.equipmentPurchase().stage() == PlayerBotEquipmentPurchaseStage::Purchase &&
	           observation.providerAvailable && observation.offerAvailable && !observation.providerInRange) {
		progressionRuntime.restartEquipmentConversation();
		player->closeShopWindow(false);
		bool approachUnavailable = false;
		processNpcApproach(player, position, npc, purchase.approachPosition, approachUnavailable);
		if (approachUnavailable) finishEquipmentPurchase(player, position, "failed", "route_unavailable");
		return;
	} else if (progressionRuntime.equipmentPurchase().stage() == PlayerBotEquipmentPurchaseStage::Equip ||
	           progressionRuntime.equipmentPurchase().stage() == PlayerBotEquipmentPurchaseStage::VerifyEquipment) {
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
	if (result.transaction.amount != 0) {
		emit("action_result", position, "\"action\":\"buy_equipment\",\"result\":\"success\",\"item_id\":" +
			std::to_string(purchase.itemId) + ",\"price\":" + std::to_string(purchase.price) + ",\"carried_before\":" +
			std::to_string(result.transaction.money) + ",\"carried_after\":" + std::to_string(player->getMoney()) + ",\"bank_before\":" +
			std::to_string(result.transaction.balance) + ",\"bank_after\":" + std::to_string(player->getBankBalance()));
	}
	if (result.type == PlayerBotProgressionOutcomeType::Succeeded || result.type == PlayerBotProgressionOutcomeType::Failed) {
		if (result.type == PlayerBotProgressionOutcomeType::Failed && std::strcmp(result.reason, "transaction_delta_mismatch") == 0) {
			logActionFailure("buy_equipment", result.reason, position);
			stop("equipment_purchase_delta_mismatch", position);
			return;
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
			finishEquipmentPurchase(player, position, "failed", "shop_window_unavailable");
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
		Item* purchased = g_game.findItemOfType(player, purchase.itemId, true);
		Container* container = purchased ? dynamic_cast<Container*>(purchased->getParent()) : nullptr;
		while (container && dynamic_cast<Container*>(container->getParent()) && player->getContainerID(dynamic_cast<Container*>(container->getParent())) < 0) container = dynamic_cast<Container*>(container->getParent());
		if (container) {
			uint8_t id = rewardContainerIdBase;
			while (id <= maximumContainerId && player->getContainerByID(id)) ++id;
			Position source; uint8_t index = 0; Item* item = static_cast<Item*>(container);
			g_game.internalGetPosition(item, source, index);
			telemetry.recordActionAttempt();
			g_game.playerUseItem(playerId, source, index, id, item->getClientID());
			emit("action_result", position, "\"action\":\"open_equipment_container\",\"result\":\"requested\",\"item_id\":" + std::to_string(item->getID()) + ",\"container_id\":" + std::to_string(id));
		}
	}
	if (result.command.type == PlayerBotProgressionCommandType::Equip) {
		Item* item = g_game.findItemOfType(player, purchase.itemId, true);
		slots_t slot = purchase.slot;
		if (std::strcmp(result.command.reason, "preserve_displaced_equipment") == 0) {
			for (const auto& [candidateSlot, itemId] : {std::pair<slots_t, uint16_t>{purchase.slot, purchase.replacedItemId}, {CONST_SLOT_LEFT, purchase.displacedLeftItemId}, {CONST_SLOT_RIGHT, purchase.displacedRightItemId}}) {
				Item* candidate = itemId == 0 ? nullptr : player->getInventoryItem(candidateSlot);
				if (candidate && candidate->getID() == itemId && candidate != item) { item = candidate; slot = candidateSlot; break; }
			}
		}
		if (item) {
			Position source; uint8_t index = 0; g_game.internalGetPosition(item, source, index); telemetry.recordActionAttempt();
			emit("action_result", position, "\"action\":" + jsonString(std::strcmp(result.command.reason, "preserve_displaced_equipment") == 0 ? "preserve_displaced_equipment" : "equip_equipment") +
				",\"result\":\"requested\",\"item_id\":" + std::to_string(item->getID()) + ",\"slot\":" + std::to_string(slot));
			g_game.playerMoveItem(player, source, item->getClientID(), index,
				std::strcmp(result.command.reason, "preserve_displaced_equipment") == 0 ? Position(0xFFFF, 0, 0) : Position(0xFFFF, purchase.slot, 0),
				item->getItemCount(), item, nullptr);
		}
	}
	if (result.command.type == PlayerBotProgressionCommandType::Navigate) return;
	const bool actionIssued = result.command.type == PlayerBotProgressionCommandType::Shop ||
	                          result.command.type == PlayerBotProgressionCommandType::Open ||
	                          result.command.type == PlayerBotProgressionCommandType::Equip;
	schedule(actionIssued || (result.command.type == PlayerBotProgressionCommandType::None && result.reason &&
	                          std::strcmp(result.reason, "action_unavailable") == 0) ? navigationDecisionDelay(*player) : SCHEDULER_MINTICKS);
}
