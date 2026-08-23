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
#include "playerbotarea.h"

#include "depotchest.h"
#include "depotlocker.h"

// NPC service discovery, shopping, banking, and depot handling.
using namespace playerbot;

namespace {
	constexpr uint32_t maximumServiceDistanceFromTemple = 200;
}

const char* PlayerBotController::cyclePhaseName() const
{
	switch (cyclePhase) {
		case CyclePhase::Idle: return "idle";
		case CyclePhase::Service: return "service";
		case CyclePhase::ReturnToDepot: return "return_to_depot";
		case CyclePhase::DepositLoot: return "deposit_loot";
		case CyclePhase::Hunt: return "hunt";
	}
	return "unknown";
}

void PlayerBotController::setCyclePhase(CyclePhase phase, const Position& position, const char* reason)
{
	if (cyclePhase == phase) {
		return;
	}
	const char* previous = cyclePhaseName();
	cyclePhase = phase;
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
	const auto traversalTarget = targetingSession.traversalTarget();
	const uint32_t previousTarget = traversalTarget ? traversalTarget->id : 0;
	g_game.playerCancelAttackAndFollow(playerId);
	clearTraversalTarget(position, reason);
	clearNavigation();
	lootSession.reset();
	depotSession.reset();
	fixedTargetRouteFailureCount = 0;
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
	if (!npcSession.acceptReply(playerId, replyingPlayerId, npcId, type)) {
		return;
	}
	Npc* npc = g_game.getNpcByID(npcId);
	emit("npc_reply", lastPosition, "\"npc_id\":" + std::to_string(npcId) +
	     ",\"npc_name\":" + jsonString(npc ? npc->getName() : "") + ",\"text\":" + jsonString(text));
}

void PlayerBotController::beginService(Player* player, const Position& position, const char* reason)
{
	const bool interruptedHunt = testPolicy.progressionEnabled && goalArbiter.activeGoal() == TopLevelGoal::Hunt &&
	                             !hasCompletedRookgaardDeparture(*player);
	finishHuntRegion(*player, position, reason);
	if (interruptedHunt) {
		emit("goal_result", position,
		     "\"decision_id\":" + std::to_string(goalArbiter.decisionId()) +
		         ",\"goal\":\"hunt\",\"result\":\"interrupted\",\"reason\":" + jsonString(reason));
		const PlayerBotGoalArbiter::GoalDecision decision = goalArbiter.force(
		    {TopLevelGoal::Service, true, criticalHealingServiceUtility, "forced_interrupt"});
		goalArbiter.apply(decision);
		emit("goal_selection", position,
		     "\"decision_id\":" + std::to_string(decision.id) + ",\"decision_reason\":" + jsonString(reason) +
		         ",\"from_goal\":\"hunt\",\"to_goal\":\"service\",\"utility\":" +
		         std::to_string(criticalHealingServiceUtility) + ',' +
		         "\"reason\":\"forced_interrupt\",\"forced\":true");
	}
	goalArbiter.setActiveGoal(TopLevelGoal::Service);
	g_game.playerCancelAttackAndFollow(playerId);
	clearTraversalTarget(position, reason);
	clearNavigation();
	lootSession.reset();
	pendingSlottedSaleItemId = 0;
	pendingSlottedSaleSourceSlot = CONST_SLOT_WHEREEVER;
	slottedSaleMoveAttempts = 0;
	player->closeContainer(corpseContainerId);
	setStage(ScenarioStage::Traverse, position);
	serviceShops.clear();
	serviceBankers.clear();
	serviceApproachTarget = Position();
	serviceStage = ServiceStage::Discover;
	npcSession.reset();
	serviceSession.reset();
	setCyclePhase(CyclePhase::Service, position, reason);
}

void PlayerBotController::finishHuntAndSelectGoal(Player* player, const Position& position, const char* reason)
{
	finishHuntRegion(*player, position, reason);
	g_game.playerCancelAttackAndFollow(playerId);
	clearTraversalTarget(position, reason);
	clearNavigation();
	lootSession.reset();
	player->closeContainer(corpseContainerId);
	setStage(ScenarioStage::Traverse, position);
	setCyclePhase(CyclePhase::Idle, position, reason);
	emit("goal_result", position,
	     "\"decision_id\":" + std::to_string(goalArbiter.decisionId()) +
	         ",\"goal\":\"hunt\",\"result\":\"success\",\"reason\":" + jsonString(reason));
	selectTopLevelGoal(*player, position, reason);
	schedule(SCHEDULER_MINTICKS);
}

void PlayerBotController::discoverServices(const Position& position)
{
	refreshItemValues();
	Player* player = g_game.getPlayerByID(playerId);
	for (const auto& entry : g_game.getNpcs()) {
		Npc* npc = entry.second;
		const std::string* capability = npc && !npc->isRemoved() ? npc->getParameter("playerbot_service") : nullptr;
		if (!capability || !player ||
		    serviceDistance(player->getTemplePosition(), {npc->getID(), npc->getPosition()}) > maximumServiceDistanceFromTemple) {
			continue;
		}
		std::vector<ServiceNpc>* services = *capability == "shop" ? &serviceShops :
		                                    (*capability == "banker" ? &serviceBankers : nullptr);
		if (!services) {
			continue;
		}
		services->push_back({npc->getID(), npc->getPosition()});
		emit("service_discovered", position, "\"capability\":" + jsonString(*capability) +
		     ",\"npc_id\":" + std::to_string(npc->getID()) + ",\"npc_name\":" + jsonString(npc->getName()) +
		     ",\"offers\":" + std::to_string(npc->getShopOffers().size()));
	}
	if (serviceShops.empty() || serviceBankers.empty()) {
		stop("service_npc_unavailable", position);
		return;
	}
	std::sort(serviceShops.begin(), serviceShops.end(), [](const ServiceNpc& left, const ServiceNpc& right) {
		return left.id < right.id;
	});
	serviceStage = ServiceStage::SellLoot;
}

bool PlayerBotController::approachServiceNpc(Player* player, ServiceNpc& service, const Position& currentPosition)
{
	Npc* npc = g_game.getNpcByID(service.id);
	if (!npc || npc->isRemoved()) {
		stop("service_npc_unavailable", currentPosition);
		return false;
	}
	service.position = npc->getPosition();
	if (Position::areInRange<3, 3, 0>(currentPosition, service.position)) {
		serviceApproachTarget = Position();
		return true;
	}
	if (serviceApproachTarget != Position()) {
		if (currentPosition == serviceApproachTarget) {
			serviceApproachTarget = Position();
			clearNavigation();
			schedule(SCHEDULER_MINTICKS);
			return false;
		}
		return processNavigation(player, currentPosition, serviceApproachTarget);
	}

	std::vector<Position> candidates;
	candidates.reserve(48);
	for (int32_t xOffset = -3; xOffset <= 3; ++xOffset) {
		for (int32_t yOffset = -3; yOffset <= 3; ++yOffset) {
			if (xOffset != 0 || yOffset != 0) {
				candidates.emplace_back(service.position.x + xOffset, service.position.y + yOffset, service.position.z);
			}
		}
	}
	std::sort(candidates.begin(), candidates.end(), [&currentPosition](const Position& left, const Position& right) {
		const int32_t leftDistance = std::max(Position::getDistanceX(currentPosition, left), Position::getDistanceY(currentPosition, left));
		const int32_t rightDistance = std::max(Position::getDistanceX(currentPosition, right), Position::getDistanceY(currentPosition, right));
		return leftDistance == rightDistance ? left < right : leftDistance < rightDistance;
	});
	for (const Position& candidate : candidates) {
		if (serviceRejectedApproaches.find(candidate) != serviceRejectedApproaches.end()) {
			continue;
		}
		Tile* tile = g_game.map.getTile(candidate);
		if (!tile || tile->queryAdd(0, *player, 1, 0) != RETURNVALUE_NOERROR) {
			continue;
		}
		std::deque<PlayerBotNavigationStep> candidateSteps;
		uint64_t expandedNodes = 0;
		const auto startedAt = std::chrono::steady_clock::now();
		PlayerBotNavigationRoutePlan routePlan = navigationRuntime.plan(*player, candidate);
		const bool planned = routePlan.metrics.result == PlayerBotNavigationResult::Reached;
		candidateSteps = std::move(routePlan.steps);
		expandedNodes = routePlan.metrics.expandedNodes;
		telemetry.recordPathfinding(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - startedAt), planned && !candidateSteps.empty());
		if (!planned || candidateSteps.empty()) {
			serviceRejectedApproaches.insert(candidate);
			schedule(SCHEDULER_MINTICKS);
			return false;
		}
		serviceApproachTarget = candidate;
		navigationRuntime.adopt(candidate, std::move(candidateSteps));
		std::ostringstream fields;
		fields << "\"action\":\"plan\",\"result\":\"success\",\"steps\":" << navigationRuntime.routeSize()
		       << ",\"expanded_nodes\":" << expandedNodes << ",\"destination\":{\"x\":" << candidate.x
		       << ",\"y\":" << candidate.y << ",\"z\":" << static_cast<uint16_t>(candidate.z) << '}';
		emit("action_result", currentPosition, fields.str());
		return processNavigation(player, currentPosition, candidate);
	}
	stop("service_approach_unavailable", currentPosition);
	return false;
}

void PlayerBotController::refreshItemValues()
{
	for (const auto& entry : g_game.getNpcs()) {
		Npc* npc = entry.second;
		const std::string* capability = npc && !npc->isRemoved() ? npc->getParameter("playerbot_service") : nullptr;
		if (!capability || *capability != "shop") {
			continue;
		}
		for (const ShopInfo& offer : npc->getShopOffers()) {
			const ItemType& type = Item::items[offer.itemId];
			if (offer.sellPrice != 0 && !type.isFluidContainer() && !type.isSplash()) {
				itemSellValues[offer.itemId] = std::max(itemSellValues[offer.itemId], offer.sellPrice);
			}
		}
	}
}

const ShopInfo* PlayerBotController::findOffer(const ServiceNpc& service, uint16_t itemId, bool buying) const
{
	if (!buying && testPolicy.suppressSlottedLootSeller && itemId == 2398) {
		return nullptr;
	}
	Npc* npc = g_game.getNpcByID(service.id);
	if (!npc || npc->isRemoved()) {
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

PlayerBotController::ServiceNpc* PlayerBotController::findNearestService(std::vector<PlayerBotController::ServiceNpc>& services, const Position& position)
{
	auto it = std::min_element(services.begin(), services.end(), [this, &position](const ServiceNpc& left, const ServiceNpc& right) {
		return serviceDistance(position, left) < serviceDistance(position, right);
	});
	return it == services.end() ? nullptr : &*it;
}

PlayerBotController::ServiceNpc* PlayerBotController::findShopFor(uint16_t itemId, bool buying, const Position& position)
{
	ServiceNpc* nearest = nullptr;
	for (ServiceNpc& service : serviceShops) {
		if (findOffer(service, itemId, buying) && (!nearest || serviceDistance(position, service) < serviceDistance(position, *nearest))) {
			nearest = &service;
		}
	}
	return nearest;
}

PlayerBotController::ServiceNpc* PlayerBotController::findLootSeller(Player* player, const Position& position, uint16_t& itemId)
{
	ServiceNpc* nearest = nullptr;
	uint32_t selectedSellPrice = 0;
	for (ServiceNpc& service : serviceShops) {
		Npc* npc = g_game.getNpcByID(service.id);
		if (!npc || npc->isRemoved()) {
			continue;
		}
		for (const ShopInfo& offer : npc->getShopOffers()) {
			if (offer.sellPrice != 0 &&
			    !(testPolicy.suppressSlottedLootSeller && offer.itemId == 2398) &&
			    getSaleItemCount(*player, offer.itemId) > 0 &&
			    (!nearest || offer.sellPrice > selectedSellPrice ||
			     (offer.sellPrice == selectedSellPrice && serviceDistance(position, service) < serviceDistance(position, *nearest)))) {
				itemId = offer.itemId;
				nearest = &service;
				selectedSellPrice = offer.sellPrice;
			}
		}
	}
	return nearest;
}

bool PlayerBotController::prepareSlottedSaleItem(Player* player, uint16_t itemId, const Position& position)
{
	Item* backpackItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
	Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
	if (!backpack) {
		return false;
	}
	if (pendingSlottedSaleItemId != 0) {
		Item* sourceAfter = player->getInventoryItem(pendingSlottedSaleSourceSlot);
		const uint32_t backpackAfter = backpack->getItemTypeCount(pendingSlottedSaleItemId);
		if ((!sourceAfter || sourceAfter->getID() != pendingSlottedSaleItemId) &&
		    backpackAfter > pendingSlottedSaleBackpackCount) {
			emit("action_result", position,
			     "\"action\":\"item_disposition\",\"result\":\"success\",\"disposition\":\"sell\",\"item_id\":" +
			         std::to_string(pendingSlottedSaleItemId) + ",\"source_slot\":" +
			         std::to_string(pendingSlottedSaleSourceSlot) + ",\"provider_available\":true");
			pendingSlottedSaleItemId = 0;
			pendingSlottedSaleSourceSlot = CONST_SLOT_WHEREEVER;
			slottedSaleMoveAttempts = 0;
			schedule(SCHEDULER_MINTICKS);
			return true;
		}
		const uint16_t failedItemId = pendingSlottedSaleItemId;
		const slots_t failedSlot = pendingSlottedSaleSourceSlot;
		pendingSlottedSaleItemId = 0;
		pendingSlottedSaleSourceSlot = CONST_SLOT_WHEREEVER;
		if (slottedSaleMoveAttempts >= maximumServiceAttempts) {
			unavailableSlottedSales[{failedItemId, failedSlot}] =
				std::chrono::steady_clock::now() + unavailableDispositionCooldown;
			emit("action_result", position,
			     "\"action\":\"item_disposition\",\"result\":\"deferred\",\"reason\":\"move_not_verified\",\"disposition\":\"sell\",\"item_id\":" +
			         std::to_string(failedItemId) + ",\"source_slot\":" + std::to_string(failedSlot) +
			         ",\"provider_available\":true,\"cooldown_ms\":" +
			         std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(unavailableDispositionCooldown).count()));
			serviceStage = ServiceStage::BuyPotions;
			slottedSaleMoveAttempts = 0;
			schedule(SCHEDULER_MINTICKS);
			return true;
		}
		schedule(navigationDecisionDelay(*player));
		return true;
	}

	slots_t sourceSlot = CONST_SLOT_WHEREEVER;
	Item* item = findActionableSlottedItem(*player, itemId, sourceSlot);
	if (!item) {
		return false;
	}
	const int8_t backpackId = player->getContainerID(backpack);
	if (backpackId < 0) {
		if (!player->canDoAction()) {
			schedule(navigationDecisionDelay(*player));
			return true;
		}
		telemetry.recordActionAttempt();
		g_game.playerUseItem(playerId, Position(0xFFFF, CONST_SLOT_BACKPACK, 0), 0,
		                     depotSourceContainerId, backpack->getClientID());
		schedule(navigationDecisionDelay(*player));
		return true;
	}
	if (!player->canDoAction()) {
		schedule(navigationDecisionDelay(*player));
		return true;
	}
	Position sourcePosition;
	uint8_t sourceIndex = 0;
	g_game.internalGetPosition(item, sourcePosition, sourceIndex);
	pendingSlottedSaleItemId = itemId;
	pendingSlottedSaleSourceSlot = sourceSlot;
	pendingSlottedSaleBackpackCount = backpack->getItemTypeCount(itemId);
	++slottedSaleMoveAttempts;
	telemetry.recordActionAttempt();
	g_game.playerMoveItem(player, sourcePosition, item->getClientID(), sourceIndex,
	                      Position(0xFFFF, 0x40 | static_cast<uint8_t>(backpackId),
	                               containerDestinationIndex(*backpack, *item)),
	                      static_cast<uint8_t>(item->getItemCount()), item, backpack);
	emit("action_result", position,
	     "\"action\":\"item_disposition\",\"result\":\"requested\",\"disposition\":\"sell\",\"item_id\":" +
	         std::to_string(itemId) + ",\"source_slot\":" + std::to_string(sourceSlot) +
	         ",\"provider_available\":true,\"attempt\":" + std::to_string(slottedSaleMoveAttempts));
	schedule(navigationDecisionDelay(*player));
	return true;
}

void PlayerBotController::completeServiceAction(Player* player, const char* action,
	const PlayerBotServiceTransaction& transaction, const Position& position)
{
	std::ostringstream fields;
	fields << "\"action\":" << jsonString(action) << ",\"result\":\"success\",\"item_id\":" << transaction.itemId
	       << ",\"count\":" << transaction.amount << ",\"carried_before\":" << transaction.money
	       << ",\"carried_after\":" << player->getMoney() << ",\"bank_before\":" << transaction.balance
	       << ",\"bank_after\":" << player->getBankBalance();
	emit("action_result", position, fields.str());
	const ItemType& itemType = Item::items[transaction.itemId];
	const std::string itemName = transaction.amount == 1 ? itemType.name : itemType.getPluralName();
	say(*player, std::string(action) == "sell" ?
	     "Sold " + std::to_string(transaction.amount) + " " + itemName + '.' :
	     "Bought " + std::to_string(transaction.amount) + " " + itemName + '.');
	npcSession.setStep(PlayerBotNpcConversationStep::Ready);
	npcSession.resetRetries();
	schedule(SCHEDULER_MINTICKS);
}

void PlayerBotController::processServiceShop(Player* player, const Position& currentPosition, ServiceNpc& service, const char* action,
                        uint16_t itemId, uint32_t amount, bool purchase)
{
	if (!approachServiceNpc(player, service, currentPosition)) {
		return;
	}
	Npc* npc = g_game.getNpcByID(service.id);
	if (!npc || npc->isRemoved()) {
		stop("service_npc_unavailable", currentPosition);
		return;
	}
	const PlayerBotNpcSessionResult sessionResult = npcSession.openShop(*player, *npc, telemetry.actionsAttemptedForSession(),
	                                                                    maximumServiceAttempts);
	if (sessionResult != PlayerBotNpcSessionResult::Ready) {
		if (sessionResult == PlayerBotNpcSessionResult::Failed) {
			logActionFailure("shop", npcSession.step() == PlayerBotNpcConversationStep::Request ?
			                 "npc_focus_unconfirmed" : "shop_window_unavailable", currentPosition);
			stop("shop_transaction_unavailable", currentPosition);
		} else {
			schedule(npcSession.nextDelay() == 0 ? SCHEDULER_MINTICKS : npcSession.nextDelay());
		}
		return;
	}
	const ShopInfo* offer = findOffer(service, itemId, purchase);
	if (!offer || amount == 0 || amount > 100) {
		stop("shop_offer_unavailable", currentPosition);
		return;
	}
	if (npcSession.step() == PlayerBotNpcConversationStep::Ready) {
		if (!serviceSession.hasShopTransaction()) {
			serviceSession.beginShopTransaction({itemId, amount, inventoryPolicy.inventoryItemCount(*player, itemId),
			                                     player->getMoney(), player->getBankBalance()});
		}
		npcSession.setStep(PlayerBotNpcConversationStep::Verify);
		telemetry.recordActionAttempt();
		if (purchase) {
			g_game.playerPurchaseItem(playerId, Item::items[itemId].clientId, static_cast<uint8_t>(offer->subType),
			                         static_cast<uint8_t>(amount), false, false);
		} else {
			g_game.playerSellItem(playerId, Item::items[itemId].clientId, static_cast<uint8_t>(offer->subType),
			                     static_cast<uint8_t>(amount), true);
		}
		schedule(navigationDecisionDelay(*player));
		return;
	}

	const PlayerBotServiceTransaction* transaction = serviceSession.shopTransaction();
	if (!transaction) {
		stop("shop_transaction_missing", currentPosition);
		return;
	}
	const PlayerBotServiceVerification verification = serviceSession.verifyShopTransaction(
		inventoryPolicy.inventoryItemCount(*player, transaction->itemId), player->getMoney(), player->getBankBalance(), purchase,
		purchase ? offer->buyPrice : offer->sellPrice, maximumServiceAttempts);
	if (verification.result == PlayerBotServiceVerificationResult::Success) {
		completeServiceAction(player, action, verification.before, currentPosition);
		return;
	}
	if (verification.result == PlayerBotServiceVerificationResult::Mismatch) {
		logActionFailure(action, "transaction_delta_mismatch", currentPosition);
		stop("shop_transaction_delta_mismatch", currentPosition);
		return;
	}
	if (verification.result == PlayerBotServiceVerificationResult::Rejected) {
		logActionFailure(action, "transaction_not_verified", currentPosition);
		stop("shop_transaction_not_verified", currentPosition);
		return;
	}
	npcSession.setStep(PlayerBotNpcConversationStep::Ready);
	schedule(navigationDecisionDelay(*player));
}

void PlayerBotController::processBank(Player* player, const Position& currentPosition, ServiceNpc& banker)
{
	if (!approachServiceNpc(player, banker, currentPosition)) {
		return;
	}
	Npc* npc = g_game.getNpcByID(banker.id);
	if (!npc || npc->isRemoved()) {
		stop("banker_unavailable", currentPosition);
		return;
	}
	if (npcSession.step() == PlayerBotNpcConversationStep::Greet ||
	    npcSession.step() == PlayerBotNpcConversationStep::Request) {
		const PlayerBotNpcSessionResult focus = npcSession.establishFocus(*player, *npc, telemetry.actionsAttemptedForSession(),
		                                                                   maximumServiceAttempts);
		if (focus == PlayerBotNpcSessionResult::Failed) {
			logActionFailure("bank", "npc_focus_unconfirmed", currentPosition);
			stop("banker_focus_unconfirmed", currentPosition);
			return;
		}
		if (focus == PlayerBotNpcSessionResult::Pending) {
			schedule(npcSession.nextDelay());
			return;
		}
	}
	if (npcSession.step() == PlayerBotNpcConversationStep::Request) {
		if (!serviceSession.hasBankDeposit()) {
			serviceSession.beginBankDeposit(player->getMoney(), player->getBankBalance());
		}
		if (serviceSession.bankTransaction().money == 0) {
			serviceSession.setBankDepositComplete(true);
			npcSession.setStep(PlayerBotNpcConversationStep::Ready);
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		telemetry.recordActionAttempt();
		npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "deposit all");
		npcSession.setStep(PlayerBotNpcConversationStep::Confirm);
		schedule(SCHEDULER_MINTICKS);
		return;
	}
	if (npcSession.step() == PlayerBotNpcConversationStep::Confirm) {
		telemetry.recordActionAttempt();
		npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "yes");
		npcSession.setStep(PlayerBotNpcConversationStep::Verify);
		schedule(SCHEDULER_MINTICKS);
		return;
	}
	if (npcSession.step() == PlayerBotNpcConversationStep::Verify && !serviceSession.bankDepositComplete()) {
		const PlayerBotServiceVerification verification = serviceSession.verifyBankDeposit(
			player->getMoney(), player->getBankBalance(), maximumServiceAttempts);
		if (verification.result != PlayerBotServiceVerificationResult::Success) {
			if (verification.result == PlayerBotServiceVerificationResult::Rejected) {
				logActionFailure("bank_deposit", "transaction_not_verified", currentPosition);
				stop("bank_deposit_not_verified", currentPosition);
				return;
			}
			npcSession.setStep(PlayerBotNpcConversationStep::Request);
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		emit("action_result", currentPosition, "\"action\":\"bank_deposit\",\"result\":\"success\",\"count\":" +
		     std::to_string(verification.before.money) + ",\"bank_before\":" + std::to_string(verification.before.balance) +
		     ",\"bank_after\":" + std::to_string(player->getBankBalance()));
		say(*player, "Deposited " + std::to_string(verification.before.money) + " gold. Bank: " +
		     std::to_string(player->getBankBalance()) + '.');
		serviceSession.setBankDepositComplete(true);
		npcSession.setStep(PlayerBotNpcConversationStep::Ready);
	}
	if (npcSession.step() == PlayerBotNpcConversationStep::Ready) {
		if (!serviceSession.hasBankWithdrawal()) {
			uint32_t amount = static_cast<uint32_t>(std::min<uint64_t>(carriedGoldReserve, player->getBankBalance()));
			const uint32_t coinWeight = Item::items[ITEM_GOLD_COIN].weight;
			if (coinWeight != 0) {
				amount = std::min(amount, player->getFreeCapacity() / coinWeight);
			}
			if (amount == 0) {
				serviceStage = ServiceStage::Complete;
				schedule(SCHEDULER_MINTICKS);
				return;
			}
			serviceSession.beginBankWithdrawal(player->getBankBalance(), amount);
		}
		telemetry.recordActionAttempt();
		npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "withdraw " + std::to_string(serviceSession.bankTransaction().amount));
		npcSession.setStep(PlayerBotNpcConversationStep::Confirm);
		schedule(SCHEDULER_MINTICKS);
		return;
	}
	if (npcSession.step() == PlayerBotNpcConversationStep::Confirm) {
		telemetry.recordActionAttempt();
		npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "yes");
		npcSession.setStep(PlayerBotNpcConversationStep::Verify);
		schedule(SCHEDULER_MINTICKS);
		return;
	}
	const PlayerBotServiceVerification verification = serviceSession.verifyBankWithdrawal(
		player->getMoney(), player->getBankBalance(), maximumServiceAttempts);
	if (verification.result == PlayerBotServiceVerificationResult::Success) {
		emit("action_result", currentPosition, "\"action\":\"bank_withdraw\",\"result\":\"success\",\"count\":" +
		     std::to_string(verification.before.amount) + ",\"bank_before\":" +
		     std::to_string(verification.before.balance) + ",\"bank_after\":" + std::to_string(player->getBankBalance()));
		serviceStage = ServiceStage::Complete;
		schedule(SCHEDULER_MINTICKS);
		return;
	}
	if (verification.result == PlayerBotServiceVerificationResult::Rejected) {
		logActionFailure("bank_withdraw", "transaction_not_verified", currentPosition);
		stop("bank_withdraw_not_verified", currentPosition);
		return;
	}
	npcSession.setStep(PlayerBotNpcConversationStep::Ready);
	schedule(SCHEDULER_MINTICKS);
}

void PlayerBotController::processService(Player* player, const Position& currentPosition)
{
	if (serviceStage == ServiceStage::Discover) {
		serviceSession.setBankDepositComplete(false);
		discoverServices(currentPosition);
		schedule(SCHEDULER_MINTICKS);
		return;
	}
	if (npcSession.step() == PlayerBotNpcConversationStep::Verify && serviceSession.hasShopTransaction() &&
	    (serviceStage == ServiceStage::SellLoot || serviceStage == ServiceStage::BuyPotions)) {
		auto service = std::find_if(serviceShops.begin(), serviceShops.end(), [this](const ServiceNpc& candidate) {
			return candidate.id == npcSession.targetId();
		});
		if (service == serviceShops.end()) {
			stop("shop_transaction_service_unavailable", currentPosition);
			return;
		}
		const bool purchase = serviceStage != ServiceStage::SellLoot;
		const char* action = serviceStage == ServiceStage::SellLoot ? "sell" : "buy_potions";
		const PlayerBotServiceTransaction& transaction = *serviceSession.shopTransaction();
		processServiceShop(player, currentPosition, *service, action, transaction.itemId, transaction.amount, purchase);
		return;
	}
	if (serviceStage == ServiceStage::SellLoot) {
		if (pendingSlottedSaleItemId != 0 &&
		    prepareSlottedSaleItem(player, pendingSlottedSaleItemId, currentPosition)) {
			return;
		}
		uint16_t itemId = 0;
		ServiceNpc* seller = findLootSeller(player, currentPosition, itemId);
		if (!seller) {
			serviceStage = ServiceStage::BuyPotions;
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		if (!npcSession.targets(seller->id)) {
			npcSession.reset(seller->id);
			serviceApproachTarget = Position();
			serviceRejectedApproaches.clear();
			clearNavigation();
		}
		const uint32_t backpackSaleCount = inventoryPolicy.backpackSaleItemCount(*player, itemId);
		if (backpackSaleCount == 0 && prepareSlottedSaleItem(player, itemId, currentPosition)) {
			return;
		}
		processServiceShop(player, currentPosition, *seller, "sell", itemId,
		                   std::min<uint32_t>(100, backpackSaleCount), false);
		return;
	}
	if (serviceStage == ServiceStage::BuyPotions) {
		const uint16_t itemId = smallHealthPotionItemId;
		const uint32_t currentCount = inventoryPolicy.inventoryItemCount(*player, itemId);
		if (currentCount >= smallHealthPotionRestockTarget) {
			serviceStage = ServiceStage::Bank;
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		ServiceNpc* seller = findShopFor(itemId, true, currentPosition);
		if (!seller) {
			stop("required_shop_offer_unavailable", currentPosition);
			return;
		}
		const ShopInfo* offer = findOffer(*seller, itemId, true);
		if (!offer || offer->buyPrice == 0) {
			stop("required_shop_offer_unavailable", currentPosition);
			return;
		}
		const uint32_t targetGap = smallHealthPotionRestockTarget - currentCount;
		const uint64_t totalMoney = player->getMoney() + player->getBankBalance();
		const uint64_t reserve = inventoryPolicy.desiredCarriedGold(*player);
		const uint32_t requiredGap = currentCount <= smallHealthPotionReturnThreshold ?
		                                 smallHealthPotionReturnThreshold + 1 - currentCount : 0;
		if (totalMoney / offer->buyPrice < requiredGap) {
			stop("insufficient_potion_funds", currentPosition);
			return;
		}
		uint32_t amount = totalMoney / offer->buyPrice >= targetGap ? targetGap :
			static_cast<uint32_t>(std::min<uint64_t>(targetGap,
				totalMoney > reserve ? (totalMoney - reserve) / offer->buyPrice : 0));
		if (currentCount <= smallHealthPotionReturnThreshold) {
			amount = std::max(amount, static_cast<uint32_t>(std::min<uint64_t>(requiredGap, totalMoney / offer->buyPrice)));
		}
		const uint32_t itemWeight = Item::items[itemId].weight;
		if (itemWeight != 0) {
			amount = std::min<uint32_t>(amount, player->getFreeCapacity() / itemWeight);
		}
		if (amount == 0) {
			serviceStage = ServiceStage::Bank;
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		if (!npcSession.targets(seller->id)) {
			npcSession.reset(seller->id);
			serviceApproachTarget = Position();
			serviceRejectedApproaches.clear();
			clearNavigation();
		}
		processServiceShop(player, currentPosition, *seller, "buy_potions", itemId, amount, true);
		return;
	}
	if (serviceStage == ServiceStage::Bank) {
		ServiceNpc* banker = findNearestService(serviceBankers, currentPosition);
		if (!banker) {
			stop("banker_unavailable", currentPosition);
			return;
		}
		if (!npcSession.targets(banker->id)) {
			npcSession.reset(banker->id);
			serviceApproachTarget = Position();
			serviceRejectedApproaches.clear();
			clearNavigation();
		}
		processBank(player, currentPosition, *banker);
		return;
	}
	beginReturn(player, currentPosition, "service_complete");
}

bool PlayerBotController::findDepositableItem(const Player& player, Container* container, Container*& source,
                                              Item*& depositItem, uint8_t& count) const
{
	for (Item* item : container->getItemList()) {
		if (Container* nested = item->getContainer(); nested &&
		    findDepositableItem(player, nested, source, depositItem, count)) {
			return true;
		}
		if (inventoryPolicy.isProtectedDepositItem(player, *item)) {
			continue;
		}
		const uint32_t carried = inventoryPolicy.inventoryItemCount(player, item->getID());
		const uint32_t reserve = inventoryPolicy.protectedItemReserve(item->getID());
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

void PlayerBotController::clearDepotDiscovery()
{
	depotSession.clearDiscovery();
}

bool PlayerBotController::discoverDepot(Player& player, const Position& currentPosition)
{
	const auto now = std::chrono::steady_clock::now();
	depotSession.expireRejectedApproaches(now);
	if (!depotSession.hasSelectedDepot() && depotSession.candidatesPrepared() &&
	    depotSession.discoveryAnchor() != currentPosition) {
		clearDepotDiscovery();
		depotSession.resetAttempts();
	}
	uint16_t lockerItemId = 0;
	if (depotSession.hasSelectedDepot() && depotSession.approachPosition() != Position() &&
	    playerbot::isInsideLocalPlanningArea(currentPosition, depotSession.lockerPosition()) &&
	    findDepotLocker(depotSession.lockerPosition(), depotSession.depotId(), lockerItemId) &&
	    lockerItemId == depotSession.lockerItemId()) {
		return true;
	}
	if (depotSession.hasSelectedDepot()) {
		clearDepotDiscovery();
		depotSession.resetAttempts();
	}
	auto finishUnavailable = [&](const char* reason) {
		const uint32_t attempts = depotSession.incrementAttempts();
		const uint32_t retryDelay = std::min<uint32_t>(depotRetryMaximumInterval,
		                                               depotRetryInitialInterval << std::min<uint32_t>(attempts - 1, 2));
		if (shouldEmitRepeated(std::string("depot_discover:") + reason)) {
			emit("action_result", currentPosition,
			     std::string("\"action\":\"depot_discover\",\"result\":\"unavailable\",\"reason\":") + jsonString(reason) +
			         ",\"indexed\":" + std::to_string(depotSession.indexedCandidateCount()) +
			         ",\"in_scope\":" + std::to_string(depotSession.inScopeCandidateCount()) +
			         ",\"standable\":" + std::to_string(depotSession.standableCandidateCount()) +
			         ",\"suppressed\":" + std::to_string(depotSession.suppressedApproachCount()) +
			         ",\"attempt\":" + std::to_string(attempts));
		}
		clearDepotDiscovery();
		if (attempts >= maximumDepotDiscoveryAttempts) {
			logActionFailure("depot_discover", reason, currentPosition);
			stop("depot_unavailable", currentPosition);
			return;
		}
		schedule(retryDelay);
	};

	if (!depotSession.candidatesPrepared()) {
		depotSession.prepareCandidates(currentPosition);
		auto& depotCandidates = depotSession.candidates();
		for (const auto& entry : g_game.map.getDepotLockerPositions()) {
			for (const Position& lockerPosition : entry.second) {
				++depotSession.indexedCandidateCount();
				if (!playerbot::isInsideLocalPlanningArea(currentPosition, lockerPosition)) {
					continue;
				}
				uint16_t indexedLockerItemId = 0;
				if (!findDepotLocker(lockerPosition, entry.first, indexedLockerItemId)) {
					continue;
				}
				++depotSession.inScopeCandidateCount();
				for (int32_t xOffset = -1; xOffset <= 1; ++xOffset) {
					for (int32_t yOffset = -1; yOffset <= 1; ++yOffset) {
						if (xOffset == 0 && yOffset == 0) {
							continue;
						}
						const Position approach(lockerPosition.x + xOffset, lockerPosition.y + yOffset, lockerPosition.z);
						Tile* approachTile = g_game.map.getTile(approach);
						if (!approachTile || approachTile->queryAdd(0, player, 1, FLAG_IGNOREBLOCKCREATURE) != RETURNVALUE_NOERROR) {
							continue;
						}
						++depotSession.standableCandidateCount();
						if (depotSession.isApproachRejected(approach)) {
							++depotSession.suppressedApproachCount();
							continue;
						}
						depotCandidates.push_back({entry.first, indexedLockerItemId, lockerPosition, approach,
						                           playerbot::localPlanningDistance(currentPosition, approach)});
					}
				}
			}
		}
		std::sort(depotCandidates.begin(), depotCandidates.end(), [](const PlayerBotDepotCandidate& left, const PlayerBotDepotCandidate& right) {
			return left.distance != right.distance ? left.distance < right.distance :
			       left.depotId != right.depotId ? left.depotId < right.depotId :
			       left.lockerPosition != right.lockerPosition ? left.lockerPosition < right.lockerPosition :
			       left.approachPosition < right.approachPosition;
		});
	}

	auto& depotCandidates = depotSession.candidates();
	if (depotCandidates.empty()) {
		if (depotSession.suppressedApproachCount() != 0) {
			auto earliestExpiry = depotSession.rejectedApproaches().begin()->second;
			for (const auto& rejected : depotSession.rejectedApproaches()) {
				earliestExpiry = std::min(earliestExpiry, rejected.second);
			}
			const uint32_t retryDelay = static_cast<uint32_t>(std::max<int64_t>(
			    1, std::chrono::duration_cast<std::chrono::milliseconds>(earliestExpiry - now).count()));
			clearDepotDiscovery();
			schedule(retryDelay);
			return false;
		}
		finishUnavailable(depotSession.inScopeCandidateCount() == 0 ? "no_local_locker" :
		                  depotSession.standableCandidateCount() == 0 ? "no_standable_approach" : "no_reachable_locker");
		return false;
	}

	uint32_t routeValidations = 0;
	while (depotSession.nextCandidate() < depotCandidates.size() && routeValidations < depotRouteValidationsPerDecision) {
		const PlayerBotDepotCandidate candidate = depotCandidates[depotSession.nextCandidate()];
		depotSession.advanceCandidate();
		uint16_t candidateLockerItemId = 0;
		Tile* approachTile = g_game.map.getTile(candidate.approachPosition);
		if (!playerbot::isInsideLocalPlanningArea(currentPosition, candidate.lockerPosition) ||
		    !findDepotLocker(candidate.lockerPosition, candidate.depotId, candidateLockerItemId) ||
		    candidateLockerItemId != candidate.lockerItemId || !approachTile ||
		    approachTile->queryAdd(0, player, 1, FLAG_IGNOREBLOCKCREATURE) != RETURNVALUE_NOERROR) {
			continue;
		}
		std::deque<PlayerBotNavigationStep> steps;
		uint64_t expandedNodes = 0;
		++routeValidations;
		const auto startedAt = std::chrono::steady_clock::now();
		const PlayerBotNavigationRoutePlan routePlan = candidate.approachPosition == currentPosition ? PlayerBotNavigationRoutePlan{} :
			navigationRuntime.plan(player, candidate.approachPosition);
		const PlayerBotNavigationResult result = candidate.approachPosition == currentPosition ? PlayerBotNavigationResult::Reached : routePlan.metrics.result;
		if (candidate.approachPosition != currentPosition) {
			steps = routePlan.steps;
			expandedNodes = routePlan.metrics.expandedNodes;
		}
		telemetry.recordPathfinding(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - startedAt), result == PlayerBotNavigationResult::Reached &&
				(candidate.approachPosition == currentPosition || !steps.empty()));
		if (result != PlayerBotNavigationResult::Reached ||
		    (candidate.approachPosition != currentPosition && steps.empty())) {
			depotSession.rejectApproach(candidate.approachPosition, now + depotApproachSuppression);
			continue;
		}
		depotSession.select(candidate);
		fixedTargetRouteFailureCount = 0;
		const size_t routeSteps = steps.size();
		adoptNavigationPlan(depotSession.approachPosition(), std::move(steps));
		std::ostringstream fields;
		fields << "\"action\":\"depot_discover\",\"result\":\"success\",\"depot_id\":" << depotSession.depotId()
		       << ",\"locker_item_id\":" << depotSession.lockerItemId() << ",\"locker\":{\"x\":" << depotSession.lockerPosition().x
		       << ",\"y\":" << depotSession.lockerPosition().y << ",\"z\":" << static_cast<uint16_t>(depotSession.lockerPosition().z)
		       << "},\"approach\":{\"x\":" << depotSession.approachPosition().x << ",\"y\":" << depotSession.approachPosition().y
		       << ",\"z\":" << static_cast<uint16_t>(depotSession.approachPosition().z) << "},\"distance\":" << candidate.distance
		       << ",\"route_steps\":" << routeSteps << ",\"expanded_nodes\":" << expandedNodes
		       << ",\"indexed\":" << depotSession.indexedCandidateCount() << ",\"in_scope\":" << depotSession.inScopeCandidateCount()
		       << ",\"standable\":" << depotSession.standableCandidateCount();
		emit("action_result", currentPosition, fields.str());
		return true;
	}

	if (depotSession.nextCandidate() < depotCandidates.size()) {
		emit("action_result", currentPosition,
		     std::string("\"action\":\"depot_discover\",\"result\":\"continuing\",\"reason\":\"route_validation_budget_exhausted\"") +
		         ",\"indexed\":" + std::to_string(depotSession.indexedCandidateCount()) +
		         ",\"in_scope\":" + std::to_string(depotSession.inScopeCandidateCount()) +
		         ",\"standable\":" + std::to_string(depotSession.standableCandidateCount()) +
		         ",\"route_validations\":" + std::to_string(routeValidations));
		schedule(blockedRouteRetryInterval);
	return false;
	}

	finishUnavailable("no_reachable_locker");
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

bool PlayerBotController::openDepotLocker(Player& player, const Position& currentPosition)
{
	Container* opened = player.getContainerByID(depotLockerContainerId);
	if (opened && opened->getDepotLocker() && opened->getDepotLocker()->getDepotId() == depotSession.depotId()) {
		depotSession.resetAttempts();
		depotSession.setStage(PlayerBotDepotStage::OpenChest);
		return true;
	}
	Tile* tile = g_game.map.getTile(depotSession.lockerPosition());
	TileItemVector* items = tile ? tile->getItemList() : nullptr;
	if (!items) {
		clearDepotDiscovery();
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
		if (!container || !container->getDepotLocker() || container->getDepotLocker()->getDepotId() != depotSession.depotId()) {
			continue;
		}
		const int32_t stackPosition = tile->getThingIndex(item);
		if (stackPosition < 0 || stackPosition > UINT8_MAX) {
			break;
		}
		if (depotSession.attempts() >= maximumDepotAttempts) {
			logActionFailure("depot_open_locker", "open_not_verified", currentPosition);
			stop("depot_locker_open_failed", currentPosition);
			return false;
		}
		const uint32_t attempts = depotSession.incrementAttempts();
		player.closeContainer(depotLockerContainerId);
		telemetry.recordActionAttempt();
		g_game.playerUseItem(playerId, depotSession.lockerPosition(), static_cast<uint8_t>(stackPosition), depotLockerContainerId,
		                     item->getClientID());
		emit("action_result", currentPosition, "\"action\":\"depot_open_locker\",\"result\":\"requested\",\"depot_id\":" +
		     std::to_string(depotSession.depotId()) + ",\"container_id\":" + std::to_string(depotLockerContainerId) +
		     ",\"attempt\":" + std::to_string(attempts));
		if (pauseDepotFixtureForRestart(player, DepotRestartCheckpoint::Locker, currentPosition)) {
			return false;
		}
		schedule(navigationDecisionDelay(player));
		return false;
	}
	logActionFailure("depot_open_locker", "locker_identity_changed", currentPosition);
	clearDepotDiscovery();
	clearNavigation();
	setCyclePhase(CyclePhase::ReturnToDepot, currentPosition, "depot_locker_identity_changed");
	schedule(blockedRouteRetryInterval);
	return false;
}

bool PlayerBotController::openDepotChest(Player& player, const Position& currentPosition)
{
	Container* locker = player.getContainerByID(depotLockerContainerId);
	if (!locker || !locker->getDepotLocker() || locker->getDepotLocker()->getDepotId() != depotSession.depotId()) {
		depotSession.setStage(PlayerBotDepotStage::OpenLocker);
		schedule(blockedRouteRetryInterval);
		return false;
	}
	DepotChest* chest = player.getDepotChest(depotSession.depotId(), false);
	if (!chest) {
		logActionFailure("depot_open_chest", "player_chest_missing", currentPosition);
		stop("depot_chest_missing", currentPosition);
		return false;
	}
	if (testPolicy.depotMoveFixture == DepotMoveFixture::Rejected) {
		chest->setMaxDepotItems(chest->getItemHoldingCount());
	}
	if (player.getContainerByID(depotChestContainerId) == chest) {
		depotSession.resetAttempts();
		depotSession.setStage(PlayerBotDepotStage::Deposit);
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
	if (depotSession.attempts() >= maximumDepotAttempts) {
		logActionFailure("depot_open_chest", "open_not_verified", currentPosition);
		stop("depot_chest_open_failed", currentPosition);
		return false;
	}
	const uint32_t attempts = depotSession.incrementAttempts();
	player.closeContainer(depotChestContainerId);
	telemetry.recordActionAttempt();
	g_game.playerUseItem(playerId, Position(0xFFFF, 0x40 | depotLockerContainerId, static_cast<uint8_t>(index)),
	                     static_cast<uint8_t>(index), depotChestContainerId, chest->getClientID());
	emit("action_result", currentPosition, "\"action\":\"depot_open_chest\",\"result\":\"requested\",\"depot_id\":" +
	     std::to_string(depotSession.depotId()) + ",\"container_id\":" + std::to_string(depotChestContainerId) +
	     ",\"attempt\":" + std::to_string(attempts));
	if (pauseDepotFixtureForRestart(player, DepotRestartCheckpoint::Chest, currentPosition)) {
		return false;
	}
	schedule(navigationDecisionDelay(player));
	return false;
}

bool PlayerBotController::pauseDepotFixtureForRestart(Player& player, DepotRestartCheckpoint checkpoint,
                                                       const Position& currentPosition)
{
	if (testPolicy.depotRestartCheckpoint != checkpoint) {
		return false;
	}
	int32_t consumed = -1;
	if (player.getStorageValue(depotRestartCheckpointStorage, consumed) && consumed == 1) {
		return false;
	}
	player.addStorageValue(depotRestartCheckpointStorage, 1);
	const char* phase = checkpoint == DepotRestartCheckpoint::Approach ? "approach" :
	                    checkpoint == DepotRestartCheckpoint::Locker ? "locker" :
	                    checkpoint == DepotRestartCheckpoint::Chest ? "chest" :
	                    checkpoint == DepotRestartCheckpoint::Deposit ? "deposit" : "depart";
	emit("action_result", currentPosition,
	     "\"action\":\"depot_restart_checkpoint\",\"result\":\"paused\",\"phase\":" + jsonString(phase));
	setStage(ScenarioStage::Stopped, currentPosition);
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

void PlayerBotController::processFixtureDeposit(Player* player, const Position& currentPosition)
{
	Item* backpackItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
	Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
	Tile* destination = g_game.map.getTile(fakeDepotTilePosition);
	if (!backpack || !destination) {
		stop("fake_depot_unavailable", currentPosition);
		return;
	}
	if (depotSession.hasPendingMove()) {
		const PlayerBotDepotMove pending = depotSession.move();
		const uint32_t destinationCount = destination->getItemTypeCount(pending.itemId);
		if (destinationCount <= pending.destinationCount) {
			logActionFailure("deposit", "fixture_item_move_failed", currentPosition);
			stop("fake_depot_rejected_loot", currentPosition);
			return;
		}
		emit("action_result", currentPosition, "\"action\":\"deposit\",\"result\":\"success\",\"fixture\":true,\"item_id\":" +
		     std::to_string(pending.itemId) + ",\"count\":" + std::to_string(destinationCount - pending.destinationCount));
		depotSession.clearMove();
	}
	if (player->getContainerByID(backpackContainerId) != backpack) {
		if (!player->canDoAction()) {
			schedule(navigationDecisionDelay(*player));
			return;
		}
		if (const int8_t existingContainerId = player->getContainerID(backpack); existingContainerId >= 0) {
			player->closeContainer(static_cast<uint8_t>(existingContainerId));
		}
		telemetry.recordActionAttempt();
		g_game.playerUseItem(playerId, Position(0xFFFF, CONST_SLOT_BACKPACK, 0), 0, backpackContainerId, backpack->getClientID());
		schedule(navigationDecisionDelay(*player));
		return;
	}
	Item* upgrade = nullptr;
	EquipmentUpgrade upgradeInfo{};
	if (equipmentPolicy.findCarriedUpgrade(*player, upgrade, upgradeInfo)) {
		readinessResumeService = true;
		if (!beginReadinessEquipment(player, currentPosition, "depot_carried_upgrade")) {
			schedule(navigationDecisionDelay(*player));
		}
		return;
	}
	Container* source = nullptr;
	Item* depositItem = nullptr;
	for (Item* item : backpack->getItemList()) {
		if (item->getContainer() || !inventoryPolicy.isProtectedInventoryItem(*item)) {
			source = backpack;
			depositItem = item;
			break;
		}
	}
	if (!depositItem) {
		if (inventoryPolicy.effectiveFreeCapacity(*player) < returnCapacityThreshold) {
			stop("depot_capacity_not_recovered", currentPosition);
			return;
		}
		emit("action_result", currentPosition, "\"action\":\"deposit\",\"result\":\"complete\",\"fixture\":true,\"cycle\":" +
		     std::to_string(completedCycles));
		if (testPolicy.progressionEnabled) {
			emit("goal_result", currentPosition,
			     "\"decision_id\":" + std::to_string(goalArbiter.decisionId()) +
			         ",\"goal\":\"service\",\"result\":\"success\",\"reason\":\"service_complete\"");
			selectTopLevelGoal(*player, currentPosition, "service_complete");
		} else {
			startHunt(player, currentPosition, "fixture_deposit_complete");
		}
		schedule(navigationInterval);
		return;
	}
	const ItemDeque& sourceItems = source->getItemList();
	auto sourceItem = std::find(sourceItems.begin(), sourceItems.end(), depositItem);
	if (sourceItem == sourceItems.end() || !player->canDoAction()) {
		schedule(navigationDecisionDelay(*player));
		return;
	}
	const uint8_t sourceIndex = static_cast<uint8_t>(std::distance(sourceItems.begin(), sourceItem));
	depotSession.beginMove({depositItem->getID(), destination->getItemTypeCount(depositItem->getID()),
	                        inventoryPolicy.inventoryItemCount(*player, depositItem->getID()),
	                        static_cast<uint8_t>(depositItem->getItemCount()), CONST_SLOT_WHEREEVER});
	telemetry.recordActionAttempt();
	g_game.playerMoveItem(player, Position(0xFFFF, 0x40 | backpackContainerId, sourceIndex), depositItem->getClientID(), sourceIndex,
	                      fakeDepotTilePosition, static_cast<uint8_t>(depositItem->getItemCount()), depositItem, destination);
	schedule(navigationDecisionDelay(*player));
}

void PlayerBotController::processDeposit(Player* player, const Position& currentPosition)
{
	Item* backpackItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
	Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
	if (!backpack) {
		stop("depot_backpack_unavailable", currentPosition);
		return;
	}
	if (depotSession.hasPendingMove()) {
		Container* chest = player->getContainerByID(depotChestContainerId);
		if (!chest) {
			depotSession.setStage(PlayerBotDepotStage::OpenChest);
			schedule(navigationDecisionDelay(*player));
			return;
		}
		const PlayerBotDepotMoveVerification verification = depotSession.verifyMove(
			inventoryPolicy.inventoryItemCount(*player, depotSession.move().itemId),
			chest->getItemTypeCount(depotSession.move().itemId), maximumDepotAttempts);
		const PlayerBotDepotMove& move = verification.before;
		if (verification.result == PlayerBotDepotMoveResult::Moved) {
			std::ostringstream fields;
			fields << "\"action\":\"deposit\",\"result\":" << jsonString(verification.movedCount == move.requestedCount ? "success" : "partial")
			       << ",\"policy\":\"known_loot\",\"depot_id\":" << depotSession.depotId() << ",\"container_id\":"
			       << static_cast<uint32_t>(depotChestContainerId) << ",\"item_id\":" << move.itemId
			       << ",\"requested\":" << static_cast<uint32_t>(move.requestedCount)
			       << ",\"verified\":" << verification.movedCount << ",\"inventory_before\":" << move.inventoryCount
			       << ",\"inventory_after\":" << verification.inventoryCount << ",\"depot_before\":" << move.destinationCount
			       << ",\"depot_after\":" << verification.destinationCount
			       << ",\"source_slot\":" << (move.sourceSlot == CONST_SLOT_WHEREEVER ? "null" : std::to_string(move.sourceSlot))
			       << ",\"provider_available\":false,\"disposition\":\"deposit\"";
			emit("action_result", currentPosition, fields.str());
		} else if (verification.result == PlayerBotDepotMoveResult::Mismatch) {
			logActionFailure("deposit", "move_delta_mismatch", currentPosition);
			stop("depot_move_delta_mismatch", currentPosition);
			return;
		} else if (verification.result == PlayerBotDepotMoveResult::Deferred) {
			const uint16_t failedItemId = move.itemId;
			const slots_t failedSlot = move.sourceSlot;
			unavailableSlottedSales[{failedItemId, failedSlot}] =
				std::chrono::steady_clock::now() + unavailableDispositionCooldown;
			emit("action_result", currentPosition,
			     "\"action\":\"deposit\",\"result\":\"deferred\",\"reason\":\"move_not_verified\",\"policy\":\"known_loot\",\"depot_id\":" +
			         std::to_string(depotSession.depotId()) + ",\"container_id\":" + std::to_string(depotChestContainerId) +
			         ",\"item_id\":" + std::to_string(failedItemId) + ",\"source_slot\":" + std::to_string(failedSlot) +
			         ",\"provider_available\":false,\"disposition\":\"deposit\",\"cooldown_ms\":" +
			         std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(unavailableDispositionCooldown).count()));
			schedule(SCHEDULER_MINTICKS);
			return;
		} else if (verification.result == PlayerBotDepotMoveResult::Rejected) {
			emit("action_result", currentPosition, "\"action\":\"deposit\",\"result\":\"failed\",\"reason\":\"no_slot_or_move_rejected\",\"policy\":\"known_loot\",\"depot_id\":" +
			     std::to_string(depotSession.depotId()) + ",\"container_id\":" + std::to_string(depotChestContainerId) +
			     ",\"item_id\":" + std::to_string(move.itemId) + ",\"requested\":" +
			     std::to_string(move.requestedCount) + ",\"verified\":0,\"inventory_before\":" +
			     std::to_string(move.inventoryCount) + ",\"inventory_after\":" + std::to_string(verification.inventoryCount) +
			     ",\"depot_before\":" + std::to_string(move.destinationCount) + ",\"depot_after\":" +
			     std::to_string(verification.destinationCount) + ",\"retry\":" + std::to_string(verification.attempts));
			stop("depot_no_slot_or_move_rejected", currentPosition);
			return;
		} else {
			emit("action_result", currentPosition, "\"action\":\"deposit\",\"result\":\"retry\",\"reason\":\"not_verified\",\"policy\":\"known_loot\",\"depot_id\":" +
			     std::to_string(depotSession.depotId()) + ",\"container_id\":" + std::to_string(depotChestContainerId) +
			     ",\"item_id\":" + std::to_string(move.itemId) + ",\"requested\":" +
			     std::to_string(move.requestedCount) + ",\"verified\":0,\"inventory_before\":" +
			     std::to_string(move.inventoryCount) + ",\"inventory_after\":" + std::to_string(verification.inventoryCount) +
			     ",\"depot_before\":" + std::to_string(move.destinationCount) + ",\"depot_after\":" +
			     std::to_string(verification.destinationCount) + ",\"retry\":" + std::to_string(verification.attempts));
			schedule(navigationDecisionDelay(*player));
			return;
		}
	}

	if (depotSession.stage() == PlayerBotDepotStage::Approach || depotSession.stage() == PlayerBotDepotStage::Discover) {
		depotSession.setStage(PlayerBotDepotStage::OpenLocker);
	}
	if (depotSession.stage() == PlayerBotDepotStage::OpenLocker && !openDepotLocker(*player, currentPosition)) {
		return;
	}
	if (depotSession.stage() == PlayerBotDepotStage::OpenChest && !openDepotChest(*player, currentPosition)) {
		return;
	}
	Container* chest = player->getContainerByID(depotChestContainerId);
	if (!chest || player->getDepotChest(depotSession.depotId(), false) != chest) {
		depotSession.setStage(PlayerBotDepotStage::OpenChest);
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
	if (equipmentPolicy.findCarriedUpgrade(*player, upgrade, upgradeInfo)) {
		readinessResumeService = true;
		if (!beginReadinessEquipment(player, currentPosition, "depot_carried_upgrade")) {
			schedule(navigationDecisionDelay(*player));
		}
		return;
	}
	if (!findDepositableItem(*player, backpack, source, depositItem, count)) {
		depositItem = findActionableSlottedItem(*player, 0, sourceSlot);
		if (depositItem) {
			const uint32_t carried = inventoryPolicy.inventoryItemCount(*player, depositItem->getID());
			const uint32_t reserve = inventoryPolicy.protectedItemReserve(depositItem->getID());
			const uint32_t movable = depositItem->isStackable() && carried > reserve ?
				std::min<uint32_t>(depositItem->getItemCount(), carried - reserve) : (carried > reserve ? 1 : 0);
			count = static_cast<uint8_t>(std::min<uint32_t>(movable, UINT8_MAX));
		}
	}
	if (!depositItem || count == 0) {
		if (inventoryPolicy.effectiveFreeCapacity(*player) < returnCapacityThreshold) {
			const auto now = std::chrono::steady_clock::now();
			const auto deferred = std::find_if(unavailableSlottedSales.begin(), unavailableSlottedSales.end(),
			                                   [now](const auto& entry) { return entry.second > now; });
			if (deferred != unavailableSlottedSales.end()) {
				schedule(static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
				                                   deferred->second - now).count()));
				return;
			}
			stop("depot_capacity_not_recovered", currentPosition);
			return;
		}
		std::ostringstream fields;
		fields << "\"action\":\"deposit\",\"result\":\"complete\",\"depot_id\":" << depotSession.depotId()
		       << ",\"container_id\":" << static_cast<uint32_t>(depotChestContainerId) << ",\"cycle\":" << completedCycles;
		emit("action_result", currentPosition, fields.str());
		player->closeContainer(depotChestContainerId);
		player->closeContainer(depotLockerContainerId);
		depotSession.setStage(PlayerBotDepotStage::Depart);
		if (pauseDepotFixtureForRestart(*player, DepotRestartCheckpoint::Depart, currentPosition)) {
			return;
		}
		if (testPolicy.progressionEnabled) {
			emit("goal_result", currentPosition,
			     "\"decision_id\":" + std::to_string(goalArbiter.decisionId()) +
			         ",\"goal\":\"service\",\"result\":\"success\",\"reason\":\"service_complete\"");
			selectTopLevelGoal(*player, currentPosition, "service_complete");
		} else {
			startHunt(player, currentPosition, "deposit_complete");
		}
		schedule(navigationInterval);
		return;
	}

	if (source && !openContainer(*player, *source, depotSourceContainerId, currentPosition)) {
		return;
	}
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
	if (!player->canDoAction()) {
		schedule(navigationDecisionDelay(*player));
		return;
	}

	depotSession.beginMove({depositItem->getID(), chest->getItemTypeCount(depositItem->getID()),
	                        inventoryPolicy.inventoryItemCount(*player, depositItem->getID()), count, sourceSlot});
	const uint8_t submittedCount = testPolicy.depotMoveFixture == DepotMoveFixture::Partial && count > 1 ? count - 1 : count;
	telemetry.recordActionAttempt();
	g_game.playerMoveItem(player, sourcePosition, depositItem->getClientID(), sourceIndex,
	                      Position(0xFFFF, 0x40 | depotChestContainerId, containerDestinationIndex(*chest, *depositItem)),
	                      submittedCount, depositItem, chest);
	emit("action_result", currentPosition, "\"action\":\"deposit\",\"result\":\"requested\",\"policy\":\"known_loot\",\"depot_id\":" +
	     std::to_string(depotSession.depotId()) + ",\"container_id\":" + std::to_string(depotChestContainerId) + ",\"item_id\":" +
	     std::to_string(depotSession.move().itemId) + ",\"requested\":" + std::to_string(count) + ",\"submitted\":" +
	     std::to_string(submittedCount) + ",\"inventory_before\":" +
	     std::to_string(depotSession.move().inventoryCount) + ",\"depot_before\":" + std::to_string(depotSession.move().destinationCount) +
	     ",\"source_slot\":" + (sourceSlot == CONST_SLOT_WHEREEVER ? "null" : std::to_string(sourceSlot)) +
	     ",\"provider_available\":false,\"disposition\":\"deposit\"");
	if (pauseDepotFixtureForRestart(*player, DepotRestartCheckpoint::Deposit, currentPosition)) {
		return;
	}
	schedule(navigationDecisionDelay(*player));
}
