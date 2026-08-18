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
	const uint32_t previousTarget = ratId;
	g_game.playerCancelAttackAndFollow(playerId);
	clearRatTarget(position, reason);
	clearNavigation();
	pendingLootItemId = 0;
	pendingDiscardItemId = 0;
	pendingDepositItemId = 0;
	depotAttempts = 0;
	fixedTargetRouteFailureCount = 0;
	clearDepotDiscovery();
	rejectedDepotApproaches.clear();
	depotStage = DepotStage::Discover;
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
	const bool interruptedHunt = testPolicy.progressionEnabled && activeGoal == TopLevelGoal::Hunt &&
	                             !hasCompletedRookgaardDeparture(*player);
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
	setCyclePhase(CyclePhase::Idle, position, reason);
	emit("goal_result", position,
	     "\"decision_id\":" + std::to_string(goalDecisionId) +
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
		++counters.pathfindingCalls;
		const auto startedAt = std::chrono::steady_clock::now();
		const bool planned = navigator.plan(*player, candidate, {}, candidateSteps, expandedNodes) ==
		                     PlayerBotNavigationResult::Reached;
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
	serviceGreetingAcknowledged = false;
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
	const ItemType& type = Item::items[item.getID()];
	return type.isContainer() || isCombatEquipment(item) || item.getID() == ropeItemId || item.getID() == 2554 ||
	       item.getWorth() != 0 || itemSellValues.find(item.getID()) == itemSellValues.end();
}

bool PlayerBotController::findDepositableItem(const Player& player, Container* container, Container*& source,
                                              Item*& depositItem, uint8_t& count) const
{
	for (Item* item : container->getItemList()) {
		if (Container* nested = item->getContainer(); nested &&
		    findDepositableItem(player, nested, source, depositItem, count)) {
			return true;
		}
		if (isProtectedDepositItem(*item)) {
			continue;
		}
		const uint32_t carried = getInventoryItemCount(player, item->getID());
		const uint32_t reserve = protectedItemReserve(item->getID());
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
	depotId = 0;
	depotLockerItemId = 0;
	depotLockerPosition = Position();
	depotApproachPosition = Position();
	depotCandidates.clear();
	nextDepotCandidate = 0;
	depotIndexedCandidateCount = 0;
	depotInScopeCandidateCount = 0;
	depotStandableCandidateCount = 0;
	depotSuppressedApproachCount = 0;
	depotDiscoveryAnchor = Position();
	depotCandidatesPrepared = false;
}

bool PlayerBotController::discoverDepot(Player& player, const Position& currentPosition)
{
	const auto now = std::chrono::steady_clock::now();
	for (auto it = rejectedDepotApproaches.begin(); it != rejectedDepotApproaches.end();) {
		if (it->second <= now) {
			it = rejectedDepotApproaches.erase(it);
		} else {
			++it;
		}
	}
	if (depotId == 0 && depotCandidatesPrepared && depotDiscoveryAnchor != currentPosition) {
		clearDepotDiscovery();
		depotAttempts = 0;
	}
	uint16_t lockerItemId = 0;
	if (depotId != 0 && depotApproachPosition != Position() &&
	    playerbot::isInsideLocalPlanningArea(currentPosition, depotLockerPosition) &&
	    findDepotLocker(depotLockerPosition, depotId, lockerItemId) && lockerItemId == depotLockerItemId) {
		return true;
	}
	if (depotId != 0) {
		clearDepotDiscovery();
		depotAttempts = 0;
	}
	auto finishUnavailable = [&](const char* reason) {
		++depotAttempts;
		const uint32_t retryDelay = std::min<uint32_t>(depotRetryMaximumInterval,
		                                               depotRetryInitialInterval << std::min<uint32_t>(depotAttempts - 1, 2));
		if (shouldEmitRepeated(std::string("depot_discover:") + reason)) {
			emit("action_result", currentPosition,
			     std::string("\"action\":\"depot_discover\",\"result\":\"unavailable\",\"reason\":") + jsonString(reason) +
			         ",\"indexed\":" + std::to_string(depotIndexedCandidateCount) +
			         ",\"in_scope\":" + std::to_string(depotInScopeCandidateCount) +
			         ",\"standable\":" + std::to_string(depotStandableCandidateCount) +
			         ",\"suppressed\":" + std::to_string(depotSuppressedApproachCount) +
			         ",\"attempt\":" + std::to_string(depotAttempts));
		}
		clearDepotDiscovery();
		if (depotAttempts >= maximumDepotDiscoveryAttempts) {
			logActionFailure("depot_discover", reason, currentPosition);
			stop("depot_unavailable", currentPosition);
			return;
		}
		schedule(retryDelay);
	};

	if (!depotCandidatesPrepared) {
		depotCandidatesPrepared = true;
		depotDiscoveryAnchor = currentPosition;
		for (const auto& entry : g_game.map.getDepotLockerPositions()) {
			for (const Position& lockerPosition : entry.second) {
				++depotIndexedCandidateCount;
				if (!playerbot::isInsideLocalPlanningArea(currentPosition, lockerPosition)) {
					continue;
				}
				uint16_t indexedLockerItemId = 0;
				if (!findDepotLocker(lockerPosition, entry.first, indexedLockerItemId)) {
					continue;
				}
				++depotInScopeCandidateCount;
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
						++depotStandableCandidateCount;
						if (rejectedDepotApproaches.find(approach) != rejectedDepotApproaches.end()) {
							++depotSuppressedApproachCount;
							continue;
						}
						depotCandidates.push_back({entry.first, indexedLockerItemId, lockerPosition, approach,
						                           playerbot::localPlanningDistance(currentPosition, approach)});
					}
				}
			}
		}
		std::sort(depotCandidates.begin(), depotCandidates.end(), [](const DepotCandidate& left, const DepotCandidate& right) {
			return left.distance != right.distance ? left.distance < right.distance :
			       left.depotId != right.depotId ? left.depotId < right.depotId :
			       left.lockerPosition != right.lockerPosition ? left.lockerPosition < right.lockerPosition :
			       left.approachPosition < right.approachPosition;
		});
	}

	if (depotCandidates.empty()) {
		if (depotSuppressedApproachCount != 0) {
			auto earliestExpiry = rejectedDepotApproaches.begin()->second;
			for (const auto& rejected : rejectedDepotApproaches) {
				earliestExpiry = std::min(earliestExpiry, rejected.second);
			}
			const uint32_t retryDelay = static_cast<uint32_t>(std::max<int64_t>(
			    1, std::chrono::duration_cast<std::chrono::milliseconds>(earliestExpiry - now).count()));
			clearDepotDiscovery();
			schedule(retryDelay);
			return false;
		}
		finishUnavailable(depotInScopeCandidateCount == 0 ? "no_local_locker" :
		                  depotStandableCandidateCount == 0 ? "no_standable_approach" : "no_reachable_locker");
		return false;
	}

	uint32_t routeValidations = 0;
	while (nextDepotCandidate < depotCandidates.size() && routeValidations < depotRouteValidationsPerDecision) {
		const DepotCandidate candidate = depotCandidates[nextDepotCandidate++];
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
		++counters.pathfindingCalls;
		const auto startedAt = std::chrono::steady_clock::now();
		const PlayerBotNavigationResult result = candidate.approachPosition == currentPosition ? PlayerBotNavigationResult::Reached :
		                                         navigator.plan(player, candidate.approachPosition, {}, steps, expandedNodes);
		counters.pathfindingTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - startedAt).count();
		if (result != PlayerBotNavigationResult::Reached ||
		    (candidate.approachPosition != currentPosition && steps.empty())) {
			++counters.pathfindingFailures;
			rejectedDepotApproaches[candidate.approachPosition] = now + depotApproachSuppression;
			continue;
		}
		depotId = candidate.depotId;
		depotLockerItemId = candidate.lockerItemId;
		depotLockerPosition = candidate.lockerPosition;
		depotApproachPosition = candidate.approachPosition;
		depotStage = DepotStage::Approach;
		depotAttempts = 0;
		fixedTargetRouteFailureCount = 0;
		const size_t routeSteps = steps.size();
		adoptNavigationPlan(depotApproachPosition, std::move(steps));
		depotCandidates.clear();
		nextDepotCandidate = 0;
		depotCandidatesPrepared = false;
		depotDiscoveryAnchor = Position();
		std::ostringstream fields;
		fields << "\"action\":\"depot_discover\",\"result\":\"success\",\"depot_id\":" << depotId
		       << ",\"locker_item_id\":" << depotLockerItemId << ",\"locker\":{\"x\":" << depotLockerPosition.x
		       << ",\"y\":" << depotLockerPosition.y << ",\"z\":" << static_cast<uint16_t>(depotLockerPosition.z)
		       << "},\"approach\":{\"x\":" << depotApproachPosition.x << ",\"y\":" << depotApproachPosition.y
		       << ",\"z\":" << static_cast<uint16_t>(depotApproachPosition.z) << "},\"distance\":" << candidate.distance
		       << ",\"route_steps\":" << routeSteps << ",\"expanded_nodes\":" << expandedNodes
		       << ",\"indexed\":" << depotIndexedCandidateCount << ",\"in_scope\":" << depotInScopeCandidateCount
		       << ",\"standable\":" << depotStandableCandidateCount;
		emit("action_result", currentPosition, fields.str());
		return true;
	}

	if (nextDepotCandidate < depotCandidates.size()) {
		emit("action_result", currentPosition,
		     std::string("\"action\":\"depot_discover\",\"result\":\"continuing\",\"reason\":\"route_validation_budget_exhausted\"") +
		         ",\"indexed\":" + std::to_string(depotIndexedCandidateCount) +
		         ",\"in_scope\":" + std::to_string(depotInScopeCandidateCount) +
		         ",\"standable\":" + std::to_string(depotStandableCandidateCount) +
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
			return false;
		}
		fromPosition = Position(0xFFFF, 0x40 | static_cast<uint8_t>(parentId), static_cast<uint8_t>(index));
		fromIndex = static_cast<uint8_t>(index);
		if (parentId == containerId) {
			--containerId;
		}
	} else if (player.getInventoryItem(CONST_SLOT_BACKPACK) != &container) {
		logActionFailure("depot_open_source", "container_not_carried", currentPosition);
		return false;
	}
	player.closeContainer(containerId);
	++counters.actionsAttempted;
	g_game.playerUseItem(playerId, fromPosition, fromIndex, containerId, container.getClientID());
	schedule(navigationDecisionDelay(player));
	return false;
}

bool PlayerBotController::openDepotLocker(Player& player, const Position& currentPosition)
{
	Container* opened = player.getContainerByID(depotLockerContainerId);
	if (opened && opened->getDepotLocker() && opened->getDepotLocker()->getDepotId() == depotId) {
		depotAttempts = 0;
		depotStage = DepotStage::OpenChest;
		return true;
	}
	Tile* tile = g_game.map.getTile(depotLockerPosition);
	TileItemVector* items = tile ? tile->getItemList() : nullptr;
	if (!items) {
		return false;
	}
	if (!player.canDoAction()) {
		schedule(navigationDecisionDelay(player));
		return false;
	}
	for (Item* item : *items) {
		Container* container = item->getContainer();
		if (!container || !container->getDepotLocker() || container->getDepotLocker()->getDepotId() != depotId) {
			continue;
		}
		const int32_t stackPosition = tile->getThingIndex(item);
		if (stackPosition < 0 || stackPosition > UINT8_MAX) {
			break;
		}
		if (depotAttempts >= maximumDepotAttempts) {
			logActionFailure("depot_open_locker", "open_not_verified", currentPosition);
			stop("depot_locker_open_failed", currentPosition);
			return false;
		}
		++depotAttempts;
		player.closeContainer(depotLockerContainerId);
		++counters.actionsAttempted;
		g_game.playerUseItem(playerId, depotLockerPosition, static_cast<uint8_t>(stackPosition), depotLockerContainerId,
		                     item->getClientID());
		emit("action_result", currentPosition, "\"action\":\"depot_open_locker\",\"result\":\"requested\",\"depot_id\":" +
		     std::to_string(depotId) + ",\"container_id\":" + std::to_string(depotLockerContainerId) +
		     ",\"attempt\":" + std::to_string(depotAttempts));
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
	if (!locker || !locker->getDepotLocker() || locker->getDepotLocker()->getDepotId() != depotId) {
		depotStage = DepotStage::OpenLocker;
		return false;
	}
	DepotChest* chest = player.getDepotChest(depotId, false);
	if (!chest) {
		logActionFailure("depot_open_chest", "player_chest_missing", currentPosition);
		stop("depot_chest_missing", currentPosition);
		return false;
	}
	if (testPolicy.depotMoveFixture == DepotMoveFixture::Rejected) {
		chest->setMaxDepotItems(chest->getItemHoldingCount());
	}
	if (player.getContainerByID(depotChestContainerId) == chest) {
		depotAttempts = 0;
		depotStage = DepotStage::Deposit;
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
	if (depotAttempts >= maximumDepotAttempts) {
		logActionFailure("depot_open_chest", "open_not_verified", currentPosition);
		stop("depot_chest_open_failed", currentPosition);
		return false;
	}
	++depotAttempts;
	player.closeContainer(depotChestContainerId);
	++counters.actionsAttempted;
	g_game.playerUseItem(playerId, Position(0xFFFF, 0x40 | depotLockerContainerId, static_cast<uint8_t>(index)),
	                     static_cast<uint8_t>(index), depotChestContainerId, chest->getClientID());
	emit("action_result", currentPosition, "\"action\":\"depot_open_chest\",\"result\":\"requested\",\"depot_id\":" +
	     std::to_string(depotId) + ",\"container_id\":" + std::to_string(depotChestContainerId) +
	     ",\"attempt\":" + std::to_string(depotAttempts));
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
	if (pendingDepositItemId != 0) {
		const uint32_t destinationCount = destination->getItemTypeCount(pendingDepositItemId);
		if (destinationCount <= pendingDepositDestinationCount) {
			logActionFailure("deposit", "fixture_item_move_failed", currentPosition);
			stop("fake_depot_rejected_loot", currentPosition);
			return;
		}
		emit("action_result", currentPosition, "\"action\":\"deposit\",\"result\":\"success\",\"fixture\":true,\"item_id\":" +
		     std::to_string(pendingDepositItemId) + ",\"count\":" + std::to_string(destinationCount - pendingDepositDestinationCount));
		pendingDepositItemId = 0;
	}
	if (player->getContainerByID(backpackContainerId) != backpack) {
		if (!player->canDoAction()) {
			schedule(navigationDecisionDelay(*player));
			return;
		}
		if (const int8_t existingContainerId = player->getContainerID(backpack); existingContainerId >= 0) {
			player->closeContainer(static_cast<uint8_t>(existingContainerId));
		}
		++counters.actionsAttempted;
		g_game.playerUseItem(playerId, Position(0xFFFF, CONST_SLOT_BACKPACK, 0), 0, backpackContainerId, backpack->getClientID());
		schedule(navigationDecisionDelay(*player));
		return;
	}
	Container* source = nullptr;
	Item* depositItem = nullptr;
	for (Item* item : backpack->getItemList()) {
		if (item->getContainer() || !isProtectedInventoryItem(*item)) {
			source = backpack;
			depositItem = item;
			break;
		}
	}
	if (!depositItem) {
		if (player->getFreeCapacity() < returnCapacityThreshold) {
			stop("depot_capacity_not_recovered", currentPosition);
			return;
		}
		emit("action_result", currentPosition, "\"action\":\"deposit\",\"result\":\"complete\",\"fixture\":true,\"cycle\":" +
		     std::to_string(completedCycles));
		if (testPolicy.progressionEnabled) {
			selectTopLevelGoal(*player, currentPosition, "fixture_deposit_complete");
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
	pendingDepositItemId = depositItem->getID();
	pendingDepositDestinationCount = destination->getItemTypeCount(pendingDepositItemId);
	++counters.actionsAttempted;
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
	if (pendingDepositItemId != 0) {
		Container* chest = player->getContainerByID(depotChestContainerId);
		if (!chest) {
			depotStage = DepotStage::OpenChest;
			schedule(navigationDecisionDelay(*player));
			return;
		}
		const uint32_t inventoryAfter = getInventoryItemCount(*player, pendingDepositItemId);
		const uint32_t depotAfter = chest->getItemTypeCount(pendingDepositItemId);
		const uint32_t movedFromInventory = pendingDepositInventoryCount - std::min(pendingDepositInventoryCount, inventoryAfter);
		const uint32_t movedToDepot = depotAfter - std::min(pendingDepositDestinationCount, depotAfter);
		if (movedFromInventory != 0 && movedFromInventory == movedToDepot) {
			std::ostringstream fields;
			fields << "\"action\":\"deposit\",\"result\":" << jsonString(movedFromInventory == pendingDepositRequestedCount ? "success" : "partial")
			       << ",\"policy\":\"known_loot\",\"depot_id\":" << depotId << ",\"container_id\":"
			       << static_cast<uint32_t>(depotChestContainerId) << ",\"item_id\":" << pendingDepositItemId
			       << ",\"requested\":" << static_cast<uint32_t>(pendingDepositRequestedCount)
			       << ",\"verified\":" << movedFromInventory << ",\"inventory_before\":" << pendingDepositInventoryCount
			       << ",\"inventory_after\":" << inventoryAfter << ",\"depot_before\":" << pendingDepositDestinationCount
			       << ",\"depot_after\":" << depotAfter;
			emit("action_result", currentPosition, fields.str());
			pendingDepositItemId = 0;
			depotAttempts = 0;
			depotStage = DepotStage::Deposit;
		} else if (movedFromInventory != 0 || movedToDepot != 0) {
			logActionFailure("deposit", "move_delta_mismatch", currentPosition);
			stop("depot_move_delta_mismatch", currentPosition);
			return;
		} else if (++depotAttempts >= maximumDepotAttempts) {
			emit("action_result", currentPosition, "\"action\":\"deposit\",\"result\":\"failed\",\"reason\":\"no_slot_or_move_rejected\",\"policy\":\"known_loot\",\"depot_id\":" +
			     std::to_string(depotId) + ",\"container_id\":" + std::to_string(depotChestContainerId) +
			     ",\"item_id\":" + std::to_string(pendingDepositItemId) + ",\"requested\":" +
			     std::to_string(pendingDepositRequestedCount) + ",\"verified\":0,\"inventory_before\":" +
			     std::to_string(pendingDepositInventoryCount) + ",\"inventory_after\":" + std::to_string(inventoryAfter) +
			     ",\"depot_before\":" + std::to_string(pendingDepositDestinationCount) + ",\"depot_after\":" +
			     std::to_string(depotAfter) + ",\"retry\":" + std::to_string(depotAttempts));
			stop("depot_no_slot_or_move_rejected", currentPosition);
			return;
		} else {
			emit("action_result", currentPosition, "\"action\":\"deposit\",\"result\":\"retry\",\"reason\":\"not_verified\",\"policy\":\"known_loot\",\"depot_id\":" +
			     std::to_string(depotId) + ",\"container_id\":" + std::to_string(depotChestContainerId) +
			     ",\"item_id\":" + std::to_string(pendingDepositItemId) + ",\"requested\":" +
			     std::to_string(pendingDepositRequestedCount) + ",\"verified\":0,\"inventory_before\":" +
			     std::to_string(pendingDepositInventoryCount) + ",\"inventory_after\":" + std::to_string(inventoryAfter) +
			     ",\"depot_before\":" + std::to_string(pendingDepositDestinationCount) + ",\"depot_after\":" +
			     std::to_string(depotAfter) + ",\"retry\":" + std::to_string(depotAttempts));
			pendingDepositItemId = 0;
			schedule(navigationDecisionDelay(*player));
			return;
		}
	}

	if (depotStage == DepotStage::Approach || depotStage == DepotStage::Discover) {
		depotStage = DepotStage::OpenLocker;
	}
	if (depotStage == DepotStage::OpenLocker && !openDepotLocker(*player, currentPosition)) {
		return;
	}
	if (depotStage == DepotStage::OpenChest && !openDepotChest(*player, currentPosition)) {
		return;
	}
	Container* chest = player->getContainerByID(depotChestContainerId);
	if (!chest || player->getDepotChest(depotId, false) != chest) {
		depotStage = DepotStage::OpenChest;
		return;
	}

	Container* source = nullptr;
	Item* depositItem = nullptr;
	uint8_t count = 0;
	refreshItemValues();
	if (!findDepositableItem(*player, backpack, source, depositItem, count)) {
		if (player->getFreeCapacity() < returnCapacityThreshold) {
			stop("depot_capacity_not_recovered", currentPosition);
			return;
		}
		std::ostringstream fields;
		fields << "\"action\":\"deposit\",\"result\":\"complete\",\"depot_id\":" << depotId
		       << ",\"container_id\":" << static_cast<uint32_t>(depotChestContainerId) << ",\"cycle\":" << completedCycles;
		emit("action_result", currentPosition, fields.str());
		player->closeContainer(depotChestContainerId);
		player->closeContainer(depotLockerContainerId);
		depotStage = DepotStage::Depart;
		if (pauseDepotFixtureForRestart(*player, DepotRestartCheckpoint::Depart, currentPosition)) {
			return;
		}
		if (testPolicy.progressionEnabled) {
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

	if (!openContainer(*player, *source, depotSourceContainerId, currentPosition)) {
		return;
	}
	const int8_t sourceContainerId = player->getContainerID(source);
	const ItemDeque& sourceItems = source->getItemList();
	auto sourceItem = std::find(sourceItems.begin(), sourceItems.end(), depositItem);
	if (sourceContainerId < 0 || sourceItem == sourceItems.end() ||
	    std::distance(sourceItems.begin(), sourceItem) > UINT8_MAX) {
		stop("depot_source_unavailable", currentPosition);
		return;
	}
	if (!player->canDoAction()) {
		schedule(navigationDecisionDelay(*player));
		return;
	}

	const uint8_t sourceIndex = static_cast<uint8_t>(std::distance(sourceItems.begin(), sourceItem));
	const Position sourcePosition(0xFFFF, 0x40 | static_cast<uint8_t>(sourceContainerId), sourceIndex);
	pendingDepositItemId = depositItem->getID();
	pendingDepositRequestedCount = count;
	pendingDepositInventoryCount = getInventoryItemCount(*player, pendingDepositItemId);
	pendingDepositDestinationCount = chest->getItemTypeCount(pendingDepositItemId);
	depotStage = DepotStage::VerifyMove;
	const uint8_t submittedCount = testPolicy.depotMoveFixture == DepotMoveFixture::Partial && count > 1 ? count - 1 : count;
	++counters.actionsAttempted;
	g_game.playerMoveItem(player, sourcePosition, depositItem->getClientID(), sourceIndex,
	                      Position(0xFFFF, 0x40 | depotChestContainerId, containerDestinationIndex(*chest, *depositItem)),
	                      submittedCount, depositItem, chest);
	emit("action_result", currentPosition, "\"action\":\"deposit\",\"result\":\"requested\",\"policy\":\"known_loot\",\"depot_id\":" +
	     std::to_string(depotId) + ",\"container_id\":" + std::to_string(depotChestContainerId) + ",\"item_id\":" +
	     std::to_string(pendingDepositItemId) + ",\"requested\":" + std::to_string(count) + ",\"submitted\":" +
	     std::to_string(submittedCount) + ",\"inventory_before\":" +
	     std::to_string(pendingDepositInventoryCount) + ",\"depot_before\":" + std::to_string(pendingDepositDestinationCount));
	if (pauseDepotFixtureForRestart(*player, DepotRestartCheckpoint::Deposit, currentPosition)) {
		return;
	}
	schedule(navigationDecisionDelay(*player));
}
