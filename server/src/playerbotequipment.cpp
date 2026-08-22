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
using namespace playerbot;

namespace {
	constexpr uint32_t maximumEquipmentProviderDistance = 200;
	constexpr size_t maximumEquipmentHuntRegions = 32;
	constexpr size_t maximumEquipmentProviderRoutes = 4;
	constexpr size_t maximumEquipmentProviderApproaches = 4;
	constexpr uint64_t maximumEquipmentProviderPathNodes = 5000;
	constexpr size_t maximumEquipmentCatalogOffers = 64;
}
PlayerBotController::EquipmentHuntSummary PlayerBotController::equipmentHuntSummary(Player& player,
	                                                                                   const PlayerBotCombatProfile& profile) const
{
	return huntRuntime.scoreEquipmentHunts(player, profile, maximumEquipmentHuntRegions);
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
	if (!equipmentPolicy.requiresKnightCombatReadiness(player)) {
		return std::nullopt;
	}
	const uint64_t reserve = spellTrainingReserve(player);
	const uint64_t totalMoney = player.getMoney() + player.getBankBalance();
	const EquipmentLoadout currentLoadout = equipmentPolicy.loadout(player);
	const PlayerBotCombatProfile currentProfile = equipmentPolicy.combatProfile(player, currentLoadout);
	const EquipmentHuntSummary currentHunts = equipmentHuntSummary(player, currentProfile);
	const Item* backpackItem = player.getInventoryItem(CONST_SLOT_BACKPACK);
	const PlayerBotEquipmentReadinessInput readiness{
		backpackItem && backpackItem->getContainer(),
		inventoryPolicy.inventoryItemCount(player, smallHealthPotionItemId) > smallHealthPotionReturnThreshold,
		inventoryPolicy.effectiveFreeCapacity(player),
		returnCapacityThreshold,
	};
	const bool currentReady = equipmentPolicy.loadoutReady(player, currentLoadout, readiness);
	std::map<uint16_t, EquipmentOfferEvaluation> evaluatedItems;
	std::map<uint32_t, std::optional<std::pair<Position, uint32_t>>> providerRoutes;
	std::set<uint32_t> providerRouteNodeLimits;
	std::optional<EquipmentOfferEvaluation> selected;
	uint32_t feasibleCandidates = 0;
	size_t simulatedItems = 0;
	bool providerRouteBudgetExhausted = false;
	size_t catalogOffers = 0;
	bool catalogTruncated = false;

	auto providerRoute = [&](Npc& npc) -> std::optional<std::pair<Position, uint32_t>> {
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
				navigationRuntime.plan(player, approach, {}, maximumEquipmentProviderPathNodes);
			const PlayerBotNavigationResult result = approach == position ? PlayerBotNavigationResult::Reached : routePlan.metrics.result;
			telemetry.recordPathfinding(std::chrono::microseconds::zero(), result == PlayerBotNavigationResult::Reached);
			if (approach != position) {
				steps = routePlan.steps;
				expandedNodes = routePlan.metrics.expandedNodes;
			}
			if (result == PlayerBotNavigationResult::Reached) {
				return providerRoutes.emplace(npc.getID(), std::make_pair(approach, static_cast<uint32_t>(steps.size()))).first->second;
			}
			if (result == PlayerBotNavigationResult::NodeLimit) {
				providerRouteNodeLimits.insert(npc.getID());
			}
		}
		return providerRoutes.emplace(npc.getID(), std::nullopt).first->second;
	};
	for (const auto& entry : g_game.getNpcs()) {
		if (catalogTruncated) {
			break;
		}
		Npc* npc = entry.second;
		const std::string* capability = npc && !npc->isRemoved() ? npc->getParameter("playerbot_service") : nullptr;
		if (!npc || !capability || *capability != "shop" ||
		    serviceDistance(player.getTemplePosition(), {npc->getID(), npc->getPosition()}) > maximumEquipmentProviderDistance) {
			continue;
		}
		for (const ShopInfo& offer : npc->getShopOffers()) {
			if (offer.buyPrice == 0) {
				continue;
			}
			if (catalogOffers >= maximumEquipmentCatalogOffers) {
				catalogTruncated = true;
				break;
			}
			++catalogOffers;
			EquipmentOfferEvaluation evaluation;
			const bool carried = g_game.findItemOfType(&player, offer.itemId, true) != nullptr;
			if (auto item = evaluatedItems.find(offer.itemId); item != evaluatedItems.end()) {
				evaluation = item->second;
			} else {
				evaluation = equipmentPolicy.evaluateCandidate(
					player, offer.itemId, currentLoadout, currentProfile, currentHunts, currentReady, readiness,
					carried ? 0 : Item::items[offer.itemId].weight,
					simulatedItems < maximumEquipmentCandidateSimulations,
					[this](Player& candidate, const PlayerBotCombatProfile& profile) {
						return equipmentHuntSummary(candidate, profile);
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
			if (!evaluation.rejection.empty()) {
				emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "rejected",
				                   evaluation.rejection.c_str());
				continue;
			}
			if (!evaluation.carried && Item::items[offer.itemId].weight > player.getFreeCapacity()) {
				emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "rejected",
				                   "insufficient_capacity");
				continue;
			}
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
			if (!backpack || freeBackpackSlots < requiredBackpackSlots) {
				emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "rejected",
				                   "insufficient_displaced_item_space");
				continue;
			}
			if (evaluation.carried) {
				evaluation.travelSteps = 0;
				emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "feasible", "carried_upgrade");
				++feasibleCandidates;
				if (!selected || PlayerBotEquipmentPolicy::prefers(evaluation, *selected)) {
					selected = evaluation;
				}
				continue;
			}
			if (reserve == std::numeric_limits<uint64_t>::max()) {
				emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "rejected", "recovery_reserve_unavailable");
				continue;
			}
			if (totalMoney < reserve + evaluation.price) {
				emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "rejected", "unaffordable_after_reserves");
				continue;
			}
			const std::optional<std::pair<Position, uint32_t>> route = providerRoute(*npc);
			if (!route) {
				const char* reason = providerRouteBudgetExhausted ? "provider_evaluation_budget_exhausted" :
				                     providerRouteNodeLimits.find(npc->getID()) != providerRouteNodeLimits.end() ?
				                     "provider_route_node_budget_exhausted" : "provider_unreachable";
				emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "rejected", reason);
				continue;
			}
			evaluation.approachPosition = route->first;
			evaluation.travelSteps = route->second;
			emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "feasible", nullptr);
			++feasibleCandidates;
			if (!selected || PlayerBotEquipmentPolicy::prefers(evaluation, *selected)) {
				selected = evaluation;
			}
		}
	}
	std::ostringstream fields;
	fields << "\"result\":" << jsonString(selected ? (selected->carried ? "would_equip" : "would_buy") : "no_decision")
	       << ",\"feasible_candidates\":" << feasibleCandidates
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
	const auto& purchase = equipmentPurchaseSession.plan();
	serviceWorkflow.resetTransactions();
	if (!purchase.carried) {
		serviceWorkflow.resetNpc(purchase.npcId);
		serviceWorkflow.clearApproach();
		serviceWorkflow.clearRejectedApproaches();
		clearNavigation();
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
	const auto& purchase = equipmentPurchaseSession.plan();
	std::ostringstream fields;
	fields << "\"goal\":\"buy_equipment\",\"npc_id\":" << purchase.npcId
	       << ",\"item_id\":" << purchase.itemId << ",\"price\":" << purchase.price
	       << ",\"rule\":" << jsonString(PlayerBotEquipmentPolicy::decisionRuleName(purchase.rule))
	       << ",\"result\":" << jsonString(result) << ",\"reason\":" << jsonString(reason);
	emit("strategy_objective_result", position, fields.str());
	emit("goal_result", position,
	     "\"decision_id\":" + std::to_string(goalArbiter.decisionId()) +
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
	progressionRuntime.setCooldown(TopLevelGoal::BuyEquipment,
	                       succeeded ? equipmentPurchaseSuccessCooldown : equipmentPurchaseFailureCooldown);
	progressionRuntime.finish();
	serviceWorkflow.resetNpc();
	serviceWorkflow.resetTransactions();
	clearNavigation();
	cyclePhase = CyclePhase::Service;
	if (succeeded && player && fixtureDriver.completeEquipmentPurchase(*player)) {
		return;
	}
	if (fixtureDriver.progressionEnabled() && player) {
		selectTopLevelGoal(*player, position, succeeded ? "equipment_purchase_complete" : "equipment_purchase_failed");
	} else {
		progressionRuntime.setActiveGoal(TopLevelGoal::Service);
	}
	schedule(SCHEDULER_MINTICKS);
}

void PlayerBotController::processEquipmentPurchase(Player* player, const Position& position)
{
	const auto& purchase = equipmentPurchaseSession.plan();
	Npc* npc = purchase.carried ? nullptr : g_game.getNpcByID(purchase.npcId);
	const std::string* capability = npc && !npc->isRemoved() ? npc->getParameter("playerbot_service") : nullptr;
	const ServiceNpc provider{purchase.npcId, npc ? npc->getPosition() : Position()};
	const ShopInfo* offer = purchase.carried || !npc ? nullptr : findOffer(provider, purchase.itemId, true);
	PlayerBotEquipmentPurchaseObservation observation;
	observation.actionAvailable = player->canDoAction();
	if (!purchase.carried) {
		observation.providerAvailable = capability && *capability == "shop";
		observation.offerAvailable = offer && offer->buyPrice == purchase.price;
		observation.providerInRange = npc && Position::areInRange<3, 3, 0>(position, npc->getPosition());
		observation.shopReady = serviceWorkflow.npcStep() == PlayerBotNpcConversationStep::Ready;
		const uint64_t reserve = spellTrainingReserve(*player);
		const uint64_t money = player->getMoney() + player->getBankBalance();
		observation.fundingAvailable = reserve != std::numeric_limits<uint64_t>::max() && money >= purchase.price && money - purchase.price >= reserve;
	}
	if (equipmentPurchaseSession.stage() == PlayerBotEquipmentPurchaseStage::Travel) {
		observation.navigationReached = processNavigation(player, position, purchase.approachPosition);
		observation.navigationFailed = navigationRuntime.fixedTargetRouteFailureCount() >= maximumProgressionAttempts ||
		                              navigationRuntime.stepFailureCount() >= maximumRepeatedNavigationStepFailures;
	} else if (equipmentPurchaseSession.stage() == PlayerBotEquipmentPurchaseStage::VerifyPurchase) {
		const PlayerBotServiceVerification verification = serviceWorkflow.verifyShopTransaction(
			inventoryPolicy.inventoryItemCount(*player, purchase.itemId), player->getMoney(), player->getBankBalance(), true, purchase.price,
			maximumProgressionAttempts);
		observation.transactionSucceeded = verification.result == PlayerBotServiceVerificationResult::Success;
		observation.transactionRejected = verification.result == PlayerBotServiceVerificationResult::Rejected;
		observation.transactionMismatch = verification.result == PlayerBotServiceVerificationResult::Mismatch;
		if (observation.transactionSucceeded) emit("action_result", position, "\"action\":\"buy_equipment\",\"result\":\"success\",\"item_id\":" +
			std::to_string(purchase.itemId) + ",\"price\":" + std::to_string(purchase.price) + ",\"carried_before\":" +
			std::to_string(verification.before.money) + ",\"carried_after\":" + std::to_string(player->getMoney()) + ",\"bank_before\":" +
			std::to_string(verification.before.balance) + ",\"bank_after\":" + std::to_string(player->getBankBalance()));
	} else if (equipmentPurchaseSession.stage() == PlayerBotEquipmentPurchaseStage::Equip ||
	           equipmentPurchaseSession.stage() == PlayerBotEquipmentPurchaseStage::VerifyEquipment) {
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
	const PlayerBotProgressionOutcome result = progressionRuntime.advanceEquipmentPurchase(observation);
	if (result.type == PlayerBotProgressionOutcomeType::Retry && result.reason && std::strcmp(result.reason, "purchase_equipment") == 0) {
		serviceWorkflow.setNpcStep(PlayerBotNpcConversationStep::Ready);
	}
	if (result.type == PlayerBotProgressionOutcomeType::Succeeded || result.type == PlayerBotProgressionOutcomeType::Failed) {
		if (result.type == PlayerBotProgressionOutcomeType::Failed && std::strcmp(result.reason, "transaction_delta_mismatch") == 0) {
			logActionFailure("buy_equipment", result.reason, position);
			stop("equipment_purchase_delta_mismatch", position);
			return;
		}
		if (result.type == PlayerBotProgressionOutcomeType::Failed && std::strcmp(result.reason, "transaction_rejected") == 0) logActionFailure("buy_equipment", result.reason, position);
		if (result.type == PlayerBotProgressionOutcomeType::Succeeded) {
			const EquipmentLoadout loadout = equipmentPolicy.loadout(*player);
			const EquipmentHuntSummary hunts = equipmentHuntSummary(*player, equipmentPolicy.combatProfile(*player, loadout));
			emit("action_result", position, "\"action\":\"equip_equipment\",\"result\":\"success\",\"item_id\":" + std::to_string(purchase.itemId) +
				",\"slot\":" + std::to_string(purchase.slot) + ",\"combat_ready\":" +
				(equipmentPolicy.loadoutReady(*player, loadout,
				    {player->getInventoryItem(CONST_SLOT_BACKPACK) && player->getInventoryItem(CONST_SLOT_BACKPACK)->getContainer(),
				     inventoryPolicy.inventoryItemCount(*player, smallHealthPotionItemId) > smallHealthPotionReturnThreshold,
				     inventoryPolicy.effectiveFreeCapacity(*player), returnCapacityThreshold}) ? "true" : "false") +
				",\"suitable_regions\":" + std::to_string(hunts.suitableRegions) +
				",\"displaced_items_preserved\":true");
		}
		finishEquipmentPurchase(player, position, result.type == PlayerBotProgressionOutcomeType::Succeeded ? "success" : "failed", result.reason);
		return;
	}
	if (result.command.type == PlayerBotProgressionCommandType::Shop && std::strcmp(result.command.reason, "open_shop") == 0) {
		const PlayerBotNpcSessionOutcome opened = serviceWorkflow.openNpcShop(*player, *npc, maximumServiceAttempts);
		for (uint8_t action = 0; action < opened.actionsIssued; ++action) telemetry.recordActionAttempt();
		if (opened.result == PlayerBotNpcSessionResult::Failed) {
			logActionFailure("shop", serviceWorkflow.npcStep() == PlayerBotNpcConversationStep::Request ? "npc_focus_unconfirmed" : "shop_window_unavailable", position);
			finishEquipmentPurchase(player, position, "failed", "shop_window_unavailable");
			return;
		}
		schedule(serviceWorkflow.npcNextDelay() == 0 ? SCHEDULER_MINTICKS : serviceWorkflow.npcNextDelay());
		return;
	}
	if (result.command.type == PlayerBotProgressionCommandType::Shop) {
		if (!serviceWorkflow.hasShopTransaction()) serviceWorkflow.beginShopTransaction({purchase.itemId, 1,
			inventoryPolicy.inventoryItemCount(*player, purchase.itemId), player->getMoney(), player->getBankBalance()});
		telemetry.recordActionAttempt();
		if (!fixtureDriver.forceEquipmentPurchaseRejected()) g_game.playerPurchaseItem(playerId, Item::items[purchase.itemId].clientId,
			static_cast<uint8_t>(offer->subType), 1, false, false);
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
