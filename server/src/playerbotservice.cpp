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

// NPC service discovery, shopping, banking, and depot handling.
using namespace playerbot;

const char* PlayerBotController::cyclePhaseName() const
{
	switch (cyclePhase) {
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
	const uint32_t previousTarget = ratId;
	g_game.playerCancelAttackAndFollow(playerId);
	clearRatTarget(position, reason);
	clearNavigation();
	pendingLootItemId = 0;
	pendingDiscardItemId = 0;
	player->closeContainer(corpseContainerId);
	setStage(ScenarioStage::Traverse, position);
	setCyclePhase(CyclePhase::ReturnToDepot, position, reason);
	std::ostringstream fields;
	fields << "\"action\":\"return\",\"result\":\"started\",\"reason\":" << jsonString(reason)
	       << ",\"previous_target_id\":" << (previousTarget == 0 ? "null" : std::to_string(previousTarget))
	       << ",\"destination\":{\"x\":" << fakeDepotPosition.x << ",\"y\":" << fakeDepotPosition.y
	       << ",\"z\":" << static_cast<uint16_t>(fakeDepotPosition.z) << '}';
	emit("action_result", position, fields.str());
	schedule(navigationInterval);
}

void PlayerBotController::onNpcReply(uint32_t replyingPlayerId, uint32_t npcId, uint8_t type, const std::string& text)
{
	if (replyingPlayerId != playerId || npcId != serviceTargetId || type != TALKTYPE_PRIVATE_NP) {
		return;
	}
	serviceGreetingAcknowledged = true;
	Npc* npc = g_game.getNpcByID(npcId);
	emit("npc_reply", lastPosition, "\"npc_id\":" + std::to_string(npcId) +
	     ",\"npc_name\":" + jsonString(npc ? npc->getName() : "") + ",\"text\":" + jsonString(text));
}

void PlayerBotController::beginService(Player* player, const Position& position, const char* reason)
{
	const bool interruptedHunt = progressionEnabled && activeGoal == TopLevelGoal::Hunt;
	finishHuntRegion(*player, position, reason);
	if (interruptedHunt) {
		emit("goal_result", position,
		     "\"decision_id\":" + std::to_string(goalDecisionId) +
		         ",\"goal\":\"hunt\",\"result\":\"interrupted\",\"reason\":" + jsonString(reason));
		++goalDecisionId;
		emit("goal_selection", position,
		     "\"decision_id\":" + std::to_string(goalDecisionId) + ",\"decision_reason\":" + jsonString(reason) +
		         ",\"from_goal\":\"hunt\",\"to_goal\":\"service\",\"utility\":" +
		         std::to_string(criticalHealingServiceUtility) + ',' +
		         "\"reason\":\"forced_interrupt\",\"forced\":true");
	}
	activeGoal = TopLevelGoal::Service;
	g_game.playerCancelAttackAndFollow(playerId);
	clearRatTarget(position, reason);
	clearNavigation();
	pendingLootItemId = 0;
	pendingDiscardItemId = 0;
	player->closeContainer(corpseContainerId);
	serviceShops.clear();
	serviceBankers.clear();
	serviceTargetId = 0;
	serviceApproachTarget = Position();
	serviceStage = ServiceStage::Discover;
	conversationStep = ConversationStep::Greet;
	serviceAttempts = 0;
	setCyclePhase(CyclePhase::Service, position, reason);
}

void PlayerBotController::finishHuntAndSelectGoal(Player* player, const Position& position, const char* reason)
{
	finishHuntRegion(*player, position, reason);
	g_game.playerCancelAttackAndFollow(playerId);
	clearRatTarget(position, reason);
	clearNavigation();
	pendingLootItemId = 0;
	pendingDiscardItemId = 0;
	player->closeContainer(corpseContainerId);
	emit("goal_result", position,
	     "\"decision_id\":" + std::to_string(goalDecisionId) +
	         ",\"goal\":\"hunt\",\"result\":\"success\",\"reason\":" + jsonString(reason));
	selectTopLevelGoal(*player, position, reason);
	schedule(SCHEDULER_MINTICKS);
}

void PlayerBotController::discoverServices(const Position& position)
{
	refreshItemValues();
	for (const auto& entry : g_game.getNpcs()) {
		Npc* npc = entry.second;
		const std::string* capability = npc && !npc->isRemoved() ? npc->getParameter("playerbot_service") : nullptr;
		if (!capability) {
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
		++counters.pathfindingCalls;
		const auto startedAt = std::chrono::steady_clock::now();
		const bool planned = navigator.plan(*player, candidate, {}, candidateSteps, expandedNodes);
		counters.pathfindingTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - startedAt).count();
		if (!planned || candidateSteps.empty()) {
			++counters.pathfindingFailures;
			serviceRejectedApproaches.insert(candidate);
			schedule(SCHEDULER_MINTICKS);
			return false;
		}
		serviceApproachTarget = candidate;
		navigationTarget = candidate;
		navigationSteps = std::move(candidateSteps);
		std::ostringstream fields;
		fields << "\"action\":\"plan\",\"result\":\"success\",\"steps\":" << navigationSteps.size()
		       << ",\"expanded_nodes\":" << expandedNodes << ",\"destination\":{\"x\":" << candidate.x
		       << ",\"y\":" << candidate.y << ",\"z\":" << static_cast<uint16_t>(candidate.z) << '}';
		emit("action_result", currentPosition, fields.str());
		return processNavigation(player, currentPosition, candidate);
	}
	stop("service_approach_unavailable", currentPosition);
	return false;
}

void PlayerBotController::resetConversation(uint32_t targetId)
{
	serviceTargetId = targetId;
	conversationStep = ConversationStep::Greet;
	serviceAttempts = 0;
	serviceApproachTarget = Position();
	serviceRejectedApproaches.clear();
	clearNavigation();
}

bool PlayerBotController::openServiceShop(Player* player, ServiceNpc& service, const Position& position)
{
	Npc* npc = g_game.getNpcByID(service.id);
	if (!npc || npc->isRemoved()) {
		stop("service_npc_unavailable", position);
		return false;
	}
	if (conversationStep == ConversationStep::Greet) {
		serviceGreetingAcknowledged = false;
		++counters.actionsAttempted;
		npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "hi");
		conversationStep = ConversationStep::Request;
		schedule(1000);
		return false;
	}
	if (conversationStep == ConversationStep::Request) {
		if (!serviceGreetingAcknowledged) {
			if (++serviceAttempts >= maximumServiceAttempts) {
				logActionFailure("shop", "npc_focus_unconfirmed", position);
				return false;
			}
			conversationStep = ConversationStep::Greet;
			schedule(1000);
			return false;
		}
		++counters.actionsAttempted;
		npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "trade");
		conversationStep = ConversationStep::Ready;
		schedule(1000);
		return false;
	}
	int32_t onBuy;
	int32_t onSell;
	if (player->getShopOwner(onBuy, onSell) == npc && !player->getShopItemList().empty()) {
		return true;
	}
	if (++serviceAttempts >= maximumServiceAttempts) {
		logActionFailure("shop", "shop_window_unavailable", position);
		return false;
	}
	conversationStep = ConversationStep::Greet;
	schedule(SCHEDULER_MINTICKS);
	return false;
}

void PlayerBotController::refreshItemValues()
{
	itemSellValues.clear();
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
			if (offer.sellPrice != 0 && getSaleItemCount(*player, offer.itemId) > 0 &&
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

void PlayerBotController::completeServiceAction(Player* player, const char* action, uint16_t itemId, uint32_t amount, const Position& position)
{
	std::ostringstream fields;
	fields << "\"action\":" << jsonString(action) << ",\"result\":\"success\",\"item_id\":" << itemId
	       << ",\"count\":" << amount << ",\"carried_before\":" << serviceBeforeMoney
	       << ",\"carried_after\":" << player->getMoney() << ",\"bank_before\":" << serviceBeforeBalance
	       << ",\"bank_after\":" << player->getBankBalance();
	emit("action_result", position, fields.str());
	const ItemType& itemType = Item::items[itemId];
	const std::string itemName = amount == 1 ? itemType.name : itemType.getPluralName();
	say(*player, std::string(action) == "sell" ?
	     "Sold " + std::to_string(amount) + " " + itemName + '.' :
	     "Bought " + std::to_string(amount) + " " + itemName + '.');
	conversationStep = ConversationStep::Ready;
	serviceAttempts = 0;
	serviceItemId = 0;
	serviceAmount = 0;
	schedule(SCHEDULER_MINTICKS);
}

void PlayerBotController::processServiceShop(Player* player, const Position& currentPosition, ServiceNpc& service, const char* action,
                        uint16_t itemId, uint32_t amount, bool purchase)
{
	if (!approachServiceNpc(player, service, currentPosition)) {
		return;
	}
	if (!openServiceShop(player, service, currentPosition)) {
		if (serviceAttempts >= maximumServiceAttempts) {
			stop("shop_transaction_unavailable", currentPosition);
		}
		return;
	}
	const ShopInfo* offer = findOffer(service, itemId, purchase);
	if (!offer || amount == 0 || amount > 100) {
		stop("shop_offer_unavailable", currentPosition);
		return;
	}
	if (conversationStep == ConversationStep::Ready) {
		serviceBeforeItemCount = getInventoryItemCount(*player, itemId);
		serviceBeforeMoney = player->getMoney();
		serviceBeforeBalance = player->getBankBalance();
		serviceItemId = itemId;
		serviceAmount = amount;
		conversationStep = ConversationStep::Verify;
		++counters.actionsAttempted;
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

	const uint32_t currentCount = getInventoryItemCount(*player, serviceItemId);
	const uint64_t expectedMoneyDelta = static_cast<uint64_t>(serviceAmount) *
	                                    (purchase ? offer->buyPrice : offer->sellPrice);
	const bool itemChanged = purchase ? currentCount == serviceBeforeItemCount + serviceAmount :
	                                  currentCount + serviceAmount == serviceBeforeItemCount;
	const uint64_t expectedMoney = purchase ? (serviceBeforeMoney > expectedMoneyDelta ? serviceBeforeMoney - expectedMoneyDelta : 0) :
	                                           serviceBeforeMoney + expectedMoneyDelta;
	const uint64_t expectedBalance = purchase && expectedMoneyDelta > serviceBeforeMoney ?
	                                     serviceBeforeBalance - (expectedMoneyDelta - serviceBeforeMoney) : serviceBeforeBalance;
	const bool economyChanged = player->getMoney() == expectedMoney && player->getBankBalance() == expectedBalance;
	if (itemChanged && economyChanged) {
		completeServiceAction(player, action, serviceItemId, serviceAmount, currentPosition);
		return;
	}
	if (currentCount != serviceBeforeItemCount || player->getMoney() != serviceBeforeMoney ||
	    player->getBankBalance() != serviceBeforeBalance) {
		logActionFailure(action, "transaction_delta_mismatch", currentPosition);
		stop("shop_transaction_delta_mismatch", currentPosition);
		return;
	}
	if (++serviceAttempts >= maximumServiceAttempts) {
		logActionFailure(action, "transaction_not_verified", currentPosition);
		stop("shop_transaction_not_verified", currentPosition);
		return;
	}
	conversationStep = ConversationStep::Ready;
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
	if (conversationStep == ConversationStep::Greet) {
		serviceGreetingAcknowledged = false;
		++counters.actionsAttempted;
		npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "hi");
		conversationStep = ConversationStep::Request;
		schedule(1000);
		return;
	}
	if (conversationStep == ConversationStep::Request) {
		if (!serviceGreetingAcknowledged) {
			if (++serviceAttempts >= maximumServiceAttempts) {
				logActionFailure("bank", "npc_focus_unconfirmed", currentPosition);
				stop("banker_focus_unconfirmed", currentPosition);
				return;
			}
			conversationStep = ConversationStep::Greet;
			schedule(1000);
			return;
		}
		serviceBeforeMoney = player->getMoney();
		serviceBeforeBalance = player->getBankBalance();
		if (serviceBeforeMoney == 0) {
			bankDepositComplete = true;
			conversationStep = ConversationStep::Ready;
			return;
		}
		++counters.actionsAttempted;
		npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "deposit all");
		conversationStep = ConversationStep::Confirm;
		schedule(SCHEDULER_MINTICKS);
		return;
	}
	if (conversationStep == ConversationStep::Confirm) {
		++counters.actionsAttempted;
		npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "yes");
		conversationStep = ConversationStep::Verify;
		schedule(SCHEDULER_MINTICKS);
		return;
	}
	if (conversationStep == ConversationStep::Verify && !bankDepositComplete) {
		if (player->getMoney() != 0 || player->getBankBalance() < serviceBeforeBalance + serviceBeforeMoney) {
			if (++serviceAttempts >= maximumServiceAttempts) {
				logActionFailure("bank_deposit", "transaction_not_verified", currentPosition);
				stop("bank_deposit_not_verified", currentPosition);
				return;
			}
			conversationStep = ConversationStep::Request;
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		emit("action_result", currentPosition, "\"action\":\"bank_deposit\",\"result\":\"success\",\"count\":" +
		     std::to_string(serviceBeforeMoney) + ",\"bank_before\":" + std::to_string(serviceBeforeBalance) +
		     ",\"bank_after\":" + std::to_string(player->getBankBalance()));
		say(*player, "Deposited " + std::to_string(serviceBeforeMoney) + " gold. Bank: " +
		     std::to_string(player->getBankBalance()) + '.');
		bankDepositComplete = true;
		conversationStep = ConversationStep::Ready;
	}
	if (conversationStep == ConversationStep::Ready) {
		serviceBeforeBalance = player->getBankBalance();
		++counters.actionsAttempted;
		npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "withdraw " + std::to_string(carriedGoldReserve));
		conversationStep = ConversationStep::Confirm;
		schedule(SCHEDULER_MINTICKS);
		return;
	}
	if (conversationStep == ConversationStep::Confirm) {
		++counters.actionsAttempted;
		npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "yes");
		conversationStep = ConversationStep::Verify;
		schedule(SCHEDULER_MINTICKS);
		return;
	}
	if (player->getMoney() == carriedGoldReserve && player->getBankBalance() + carriedGoldReserve == serviceBeforeBalance) {
		emit("action_result", currentPosition, "\"action\":\"bank_withdraw\",\"result\":\"success\",\"count\":100,\"bank_before\":" +
		     std::to_string(serviceBeforeBalance) + ",\"bank_after\":" + std::to_string(player->getBankBalance()));
		serviceStage = ServiceStage::Complete;
		schedule(SCHEDULER_MINTICKS);
		return;
	}
	if (++serviceAttempts >= maximumServiceAttempts) {
		logActionFailure("bank_withdraw", "transaction_not_verified", currentPosition);
		stop("bank_withdraw_not_verified", currentPosition);
		return;
	}
	conversationStep = ConversationStep::Ready;
	schedule(SCHEDULER_MINTICKS);
}

void PlayerBotController::processService(Player* player, const Position& currentPosition)
{
	if (serviceStage == ServiceStage::Discover) {
		bankDepositComplete = false;
		discoverServices(currentPosition);
		schedule(SCHEDULER_MINTICKS);
		return;
	}
	if (conversationStep == ConversationStep::Verify && serviceItemId != 0 && serviceAmount != 0 &&
	    (serviceStage == ServiceStage::SellLoot || serviceStage == ServiceStage::BuyPotions ||
	     serviceStage == ServiceStage::BuyMeat)) {
		auto service = std::find_if(serviceShops.begin(), serviceShops.end(), [this](const ServiceNpc& candidate) {
			return candidate.id == serviceTargetId;
		});
		if (service == serviceShops.end()) {
			stop("shop_transaction_service_unavailable", currentPosition);
			return;
		}
		const bool purchase = serviceStage != ServiceStage::SellLoot;
		const char* action = serviceStage == ServiceStage::SellLoot ? "sell" :
		                     (serviceStage == ServiceStage::BuyPotions ? "buy_potions" : "buy_meat");
		processServiceShop(player, currentPosition, *service, action, serviceItemId, serviceAmount, purchase);
		return;
	}
	if (serviceStage == ServiceStage::SellLoot) {
		uint16_t itemId = 0;
		ServiceNpc* seller = findLootSeller(player, currentPosition, itemId);
		if (!seller) {
			serviceStage = ServiceStage::BuyPotions;
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		if (serviceTargetId != seller->id) {
			resetConversation(seller->id);
		}
		processServiceShop(player, currentPosition, *seller, "sell", itemId,
		                   std::min<uint32_t>(100, getSaleItemCount(*player, itemId)), false);
		return;
	}
	if (serviceStage == ServiceStage::BuyPotions || serviceStage == ServiceStage::BuyMeat) {
		const uint16_t itemId = serviceStage == ServiceStage::BuyPotions ? smallHealthPotionItemId : meatItemId;
		const uint32_t minimum = serviceStage == ServiceStage::BuyPotions ? minimumSmallHealthPotions : minimumMeat;
		const uint32_t currentCount = getInventoryItemCount(*player, itemId);
		if (currentCount >= minimum) {
			serviceStage = serviceStage == ServiceStage::BuyPotions ? ServiceStage::BuyMeat : ServiceStage::Bank;
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		ServiceNpc* seller = findShopFor(itemId, true, currentPosition);
		if (!seller) {
			stop("required_shop_offer_unavailable", currentPosition);
			return;
		}
		if (serviceTargetId != seller->id) {
			resetConversation(seller->id);
		}
		processServiceShop(player, currentPosition, *seller, serviceStage == ServiceStage::BuyPotions ? "buy_potions" : "buy_meat",
		                   itemId, minimum - currentCount, true);
		return;
	}
	if (serviceStage == ServiceStage::Bank) {
		ServiceNpc* banker = findNearestService(serviceBankers, currentPosition);
		if (!banker) {
			stop("banker_unavailable", currentPosition);
			return;
		}
		if (serviceTargetId != banker->id) {
			resetConversation(banker->id);
		}
		processBank(player, currentPosition, *banker);
		return;
	}
	beginReturn(player, currentPosition, "service_complete");
}

bool PlayerBotController::isProtectedDepositItem(const Item& item) const
{
	return item.getID() == ropeItemId || item.getID() == 2554 || item.getID() == meatItemId ||
	       item.getID() == smallHealthPotionItemId || item.getWorth() != 0;
}

bool PlayerBotController::findDepositableItem(Container* container, Container*& source, Item*& depositItem) const
{
	for (Item* item : container->getItemList()) {
		if (Container* child = item->getContainer()) {
			(void)child;
			source = container;
			depositItem = item;
			return true;
		}
		if (!isProtectedDepositItem(*item)) {
			source = container;
			depositItem = item;
			return true;
		}
	}
	return false;
}

void PlayerBotController::processDeposit(Player* player, const Position& currentPosition)
{
	Item* backpackItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
	Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
	Tile* destination = g_game.map.getTile(fakeDepotTilePosition);
	if (!backpack || !destination) {
		stop("fake_depot_unavailable", currentPosition);
		return;
	}
	if (pendingDepositItemId != 0) {
		const uint32_t destinationCount = destination->getItemTypeCount(pendingDepositItemId);
		if (destinationCount <= pendingDepositDestinationCount) {
			logActionFailure("deposit", "item_move_failed", currentPosition);
			stop("fake_depot_rejected_loot", currentPosition);
			return;
		}
		std::ostringstream fields;
		fields << "\"action\":\"deposit\",\"result\":\"success\",\"item_id\":" << pendingDepositItemId
		       << ",\"count\":" << (destinationCount - pendingDepositDestinationCount);
		emit("action_result", currentPosition, fields.str());
		pendingDepositItemId = 0;
	}

	if (player->getContainerByID(backpackContainerId) != backpack) {
		if (!player->canDoAction()) {
			schedule(navigationDecisionDelay(*player));
			return;
		}
		const int8_t existingContainerId = player->getContainerID(backpack);
		if (existingContainerId >= 0) {
			player->closeContainer(static_cast<uint8_t>(existingContainerId));
		}
		const Position backpackPosition(0xFFFF, CONST_SLOT_BACKPACK, 0);
		++counters.actionsAttempted;
		g_game.playerUseItem(playerId, backpackPosition, 0, backpackContainerId, backpack->getClientID());
		schedule(navigationDecisionDelay(*player));
		return;
	}

	Container* source = nullptr;
	Item* depositItem = nullptr;
	if (!findDepositableItem(backpack, source, depositItem)) {
		if (player->getFreeCapacity() < returnCapacityThreshold) {
			stop("depot_capacity_not_recovered", currentPosition);
			return;
		}
		std::ostringstream fields;
		fields << "\"action\":\"deposit\",\"result\":\"complete\",\"cycle\":" << completedCycles;
		emit("action_result", currentPosition, fields.str());
		if (progressionEnabled) {
			emit("goal_result", currentPosition,
			     "\"decision_id\":" + std::to_string(goalDecisionId) +
			         ",\"goal\":\"service\",\"result\":\"success\",\"reason\":\"service_complete\"");
			selectTopLevelGoal(*player, currentPosition, "service_complete");
		} else {
			startHunt(player, currentPosition, "deposit_complete");
		}
		schedule(navigationInterval);
		return;
	}

	const int8_t sourceContainerId = player->getContainerID(source);
	const ItemDeque& sourceItems = source->getItemList();
	auto sourceItem = std::find(sourceItems.begin(), sourceItems.end(), depositItem);
	if (sourceContainerId < 0 || sourceItem == sourceItems.end() ||
	    std::distance(sourceItems.begin(), sourceItem) > UINT8_MAX) {
		stop("fake_depot_source_unavailable", currentPosition);
		return;
	}
	if (!player->canDoAction()) {
		schedule(navigationDecisionDelay(*player));
		return;
	}

	const uint8_t sourceIndex = static_cast<uint8_t>(std::distance(sourceItems.begin(), sourceItem));
	const uint8_t count = static_cast<uint8_t>(depositItem->getItemCount());
	const Position sourcePosition(0xFFFF, 0x40 | static_cast<uint8_t>(sourceContainerId), sourceIndex);
	pendingDepositItemId = depositItem->getID();
	pendingDepositDestinationCount = destination->getItemTypeCount(pendingDepositItemId);
	++counters.actionsAttempted;
	g_game.playerMoveItem(player, sourcePosition, depositItem->getClientID(), sourceIndex,
	                      fakeDepotTilePosition, count, depositItem, destination);
	schedule(navigationDecisionDelay(*player));
}
