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
	const auto traversalTarget = combatRuntime.traversalTarget();
	const uint32_t previousTarget = traversalTarget ? traversalTarget->id : 0;
	g_game.playerCancelAttackAndFollow(playerId);
	clearTraversalTarget(position, reason);
	clearNavigation();
	lootWorkflow.reset();
	depotWorkflow.reset();
	navigationRuntime.resetFixedTargetRouteFailures();
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
	if (!serviceWorkflow.acceptNpcReply(playerId, replyingPlayerId, npcId, type)) {
		return;
	}
	Npc* npc = g_game.getNpcByID(npcId);
	emit("npc_reply", lastPosition, "\"npc_id\":" + std::to_string(npcId) +
	     ",\"npc_name\":" + jsonString(npc ? npc->getName() : "") + ",\"text\":" + jsonString(text));
}

void PlayerBotController::beginService(Player* player, const Position& position, const char* reason)
{
	const bool interruptedHunt = fixtureRuntime.progressionEnabled() && goalArbiter.activeGoal() == TopLevelGoal::Hunt &&
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
	lootWorkflow.reset();
	player->closeContainer(corpseContainerId);
	setStage(ScenarioStage::Traverse, position);
	serviceWorkflow.reset();
	setCyclePhase(CyclePhase::Service, position, reason);
}

void PlayerBotController::finishHuntAndSelectGoal(Player* player, const Position& position, const char* reason)
{
	finishHuntRegion(*player, position, reason);
	g_game.playerCancelAttackAndFollow(playerId);
	clearTraversalTarget(position, reason);
	clearNavigation();
	lootWorkflow.reset();
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
	std::vector<ServiceNpc> shops;
	std::vector<ServiceNpc> bankers;
	Player* player = g_game.getPlayerByID(playerId);
	for (const auto& entry : g_game.getNpcs()) {
		Npc* npc = entry.second;
		const std::string* capability = npc && !npc->isRemoved() ? npc->getParameter("playerbot_service") : nullptr;
		if (!capability || !player ||
		    serviceDistance(player->getTemplePosition(), {npc->getID(), npc->getPosition()}) > maximumServiceDistanceFromTemple) {
			continue;
		}
		std::vector<ServiceNpc>* services = *capability == "shop" ? &shops :
		                                    (*capability == "banker" ? &bankers : nullptr);
		if (!services) {
			continue;
		}
		ServiceNpc provider{npc->getID(), npc->getPosition()};
		for (const ShopInfo& offer : npc->getShopOffers()) {
			const ItemType& type = Item::items[offer.itemId];
			if (!type.isFluidContainer() && !type.isSplash()) {
				provider.offers.push_back({offer.itemId, offer.buyPrice, offer.sellPrice, static_cast<uint8_t>(offer.subType)});
			}
		}
		services->push_back(std::move(provider));
		emit("service_discovered", position, "\"capability\":" + jsonString(*capability) +
		     ",\"npc_id\":" + std::to_string(npc->getID()) + ",\"npc_name\":" + jsonString(npc->getName()) +
		     ",\"offers\":" + std::to_string(npc->getShopOffers().size()));
	}
	if (shops.empty() || bankers.empty()) {
		stop("service_npc_unavailable", position);
		return;
	}
	serviceWorkflow.setProviders(std::move(shops), std::move(bankers));
	economyCatalog.learn(serviceWorkflow.shops());
	serviceWorkflow.setStage(PlayerBotServiceStage::SellLoot);
}

bool PlayerBotController::approachServiceNpc(Player* player, const ServiceNpc& service, const Position& currentPosition)
{
	Npc* npc = g_game.getNpcByID(service.id);
	if (!npc || npc->isRemoved()) {
		stop("service_npc_unavailable", currentPosition);
		return false;
	}
	const Position servicePosition = npc->getPosition();
	if (Position::areInRange<3, 3, 0>(currentPosition, servicePosition)) {
		serviceWorkflow.clearApproach();
		return true;
	}
	if (serviceWorkflow.approachTarget() != Position()) {
		if (currentPosition == serviceWorkflow.approachTarget()) {
			serviceWorkflow.clearApproach();
			clearNavigation();
			schedule(SCHEDULER_MINTICKS);
			return false;
		}
		return processNavigation(player, currentPosition, serviceWorkflow.approachTarget());
	}

	std::vector<Position> candidates;
	candidates.reserve(48);
	for (int32_t xOffset = -3; xOffset <= 3; ++xOffset) {
		for (int32_t yOffset = -3; yOffset <= 3; ++yOffset) {
			if (xOffset != 0 || yOffset != 0) {
				candidates.emplace_back(servicePosition.x + xOffset, servicePosition.y + yOffset, servicePosition.z);
			}
		}
	}
	std::sort(candidates.begin(), candidates.end(), [&currentPosition](const Position& left, const Position& right) {
		const int32_t leftDistance = std::max(Position::getDistanceX(currentPosition, left), Position::getDistanceY(currentPosition, left));
		const int32_t rightDistance = std::max(Position::getDistanceX(currentPosition, right), Position::getDistanceY(currentPosition, right));
		return leftDistance == rightDistance ? left < right : leftDistance < rightDistance;
	});
	for (const Position& candidate : candidates) {
		if (serviceWorkflow.isApproachRejected(candidate)) {
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
			serviceWorkflow.rejectApproach(candidate);
			schedule(SCHEDULER_MINTICKS);
			return false;
		}
		serviceWorkflow.setApproachTarget(candidate);
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
	std::vector<PlayerBotEconomyProvider> providers;
	for (const auto& entry : g_game.getNpcs()) {
		Npc* npc = entry.second;
		const std::string* capability = npc && !npc->isRemoved() ? npc->getParameter("playerbot_service") : nullptr;
		if (!capability || *capability != "shop") {
			continue;
		}
		PlayerBotEconomyProvider provider{npc->getID(), npc->getPosition()};
		for (const ShopInfo& offer : npc->getShopOffers()) {
			const ItemType& type = Item::items[offer.itemId];
			if (offer.sellPrice != 0 && !type.isFluidContainer() && !type.isSplash()) {
				provider.offers.push_back({offer.itemId, offer.buyPrice, offer.sellPrice, static_cast<uint8_t>(offer.subType)});
			}
		}
		providers.push_back(std::move(provider));
	}
	economyCatalog.learn(providers);
}

const ShopInfo* PlayerBotController::findOffer(const ServiceNpc& service, uint16_t itemId, bool buying) const
{
	if (!buying && fixtureRuntime.suppressSlottedLootSeller() && itemId == 2398) {
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

const PlayerBotController::ServiceNpc* PlayerBotController::findNearestService(const std::vector<PlayerBotController::ServiceNpc>& services, const Position& position) const
{
	auto it = std::min_element(services.begin(), services.end(), [this, &position](const ServiceNpc& left, const ServiceNpc& right) {
		return serviceDistance(position, left) < serviceDistance(position, right);
	});
	return it == services.end() ? nullptr : &*it;
}

const PlayerBotController::ServiceNpc* PlayerBotController::findShopFor(uint16_t itemId, bool buying, const Position& position) const
{
	return serviceWorkflow.rankedProvider(economyCatalog, itemId, buying, position);
}

const PlayerBotController::ServiceNpc* PlayerBotController::findLootSeller(Player* player, const Position& position, uint16_t& itemId) const
{
	const ServiceNpc* nearest = nullptr;
	uint32_t selectedSellPrice = 0;
	for (const ServiceNpc& service : serviceWorkflow.shops()) {
		Npc* npc = g_game.getNpcByID(service.id);
		if (!npc || npc->isRemoved()) {
			continue;
		}
		for (const ShopInfo& offer : npc->getShopOffers()) {
			if (offer.sellPrice != 0 &&
			    !(fixtureRuntime.suppressSlottedLootSeller() && offer.itemId == 2398) &&
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
	if (const auto pending = serviceWorkflow.pendingSlottedSale()) {
		Item* sourceAfter = player->getInventoryItem(pending->sourceSlot);
		const uint32_t backpackAfter = backpack->getItemTypeCount(pending->itemId);
		const bool moved = (!sourceAfter || sourceAfter->getID() != pending->itemId) && backpackAfter > pending->backpackCount;
		const PlayerBotSlottedSaleObservation observation = serviceWorkflow.observeSlottedSale(
		    moved, maximumServiceAttempts, std::chrono::steady_clock::now(), unavailableDispositionCooldown);
		if (observation == PlayerBotSlottedSaleObservation::Moved) {
			emit("action_result", position,
			     "\"action\":\"item_disposition\",\"result\":\"success\",\"disposition\":\"sell\",\"item_id\":" +
			         std::to_string(pending->itemId) + ",\"source_slot\":" +
			         std::to_string(pending->sourceSlot) + ",\"provider_available\":true");
			schedule(SCHEDULER_MINTICKS);
			return true;
		}
		const uint16_t failedItemId = pending->itemId;
		const slots_t failedSlot = pending->sourceSlot;
		if (observation == PlayerBotSlottedSaleObservation::Deferred) {
			emit("action_result", position,
			     "\"action\":\"item_disposition\",\"result\":\"deferred\",\"reason\":\"move_not_verified\",\"disposition\":\"sell\",\"item_id\":" +
			         std::to_string(failedItemId) + ",\"source_slot\":" + std::to_string(failedSlot) +
			         ",\"provider_available\":true,\"cooldown_ms\":" +
			         std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(unavailableDispositionCooldown).count()));
			serviceWorkflow.setStage(PlayerBotServiceStage::BuyPotions);
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
	serviceWorkflow.beginSlottedSale(itemId, sourceSlot, backpack->getItemTypeCount(itemId));
	telemetry.recordActionAttempt();
	g_game.playerMoveItem(player, sourcePosition, item->getClientID(), sourceIndex,
	                      Position(0xFFFF, 0x40 | static_cast<uint8_t>(backpackId),
	                               containerDestinationIndex(*backpack, *item)),
	                      static_cast<uint8_t>(item->getItemCount()), item, backpack);
	emit("action_result", position,
	     "\"action\":\"item_disposition\",\"result\":\"requested\",\"disposition\":\"sell\",\"item_id\":" +
	         std::to_string(itemId) + ",\"source_slot\":" + std::to_string(sourceSlot) +
	         ",\"provider_available\":true,\"attempt\":" + std::to_string(serviceWorkflow.slottedSaleAttempts()));
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
	serviceWorkflow.setNpcStep(PlayerBotNpcConversationStep::Ready);
	serviceWorkflow.resetNpcRetries();
	schedule(SCHEDULER_MINTICKS);
}

void PlayerBotController::processServiceShop(Player* player, const Position& currentPosition, const ServiceNpc& service, const char* action,
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
	const PlayerBotNpcSessionOutcome sessionOutcome = serviceWorkflow.openNpcShop(*player, *npc, maximumServiceAttempts);
	for (uint8_t action = 0; action < sessionOutcome.actionsIssued; ++action) {
		telemetry.recordActionAttempt();
	}
	if (sessionOutcome.result != PlayerBotNpcSessionResult::Ready) {
		if (sessionOutcome.result == PlayerBotNpcSessionResult::Failed) {
			logActionFailure("shop", serviceWorkflow.npcStep() == PlayerBotNpcConversationStep::Request ?
			                 "npc_focus_unconfirmed" : "shop_window_unavailable", currentPosition);
			stop("shop_transaction_unavailable", currentPosition);
		} else {
			schedule(serviceWorkflow.npcNextDelay() == 0 ? SCHEDULER_MINTICKS : serviceWorkflow.npcNextDelay());
		}
		return;
	}
	const ShopInfo* offer = findOffer(service, itemId, purchase);
	if (!offer || amount == 0 || amount > 100) {
		stop("shop_offer_unavailable", currentPosition);
		return;
	}
	if (serviceWorkflow.npcStep() == PlayerBotNpcConversationStep::Ready) {
		if (!serviceWorkflow.hasShopTransaction()) {
			serviceWorkflow.beginShopTransaction({itemId, amount, inventoryPolicy.inventoryItemCount(*player, itemId),
			                                     player->getMoney(), player->getBankBalance()});
		}
		serviceWorkflow.setNpcStep(PlayerBotNpcConversationStep::Verify);
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

	const PlayerBotServiceTransaction* transaction = serviceWorkflow.shopTransaction();
	if (!transaction) {
		stop("shop_transaction_missing", currentPosition);
		return;
	}
	const PlayerBotServiceVerification verification = serviceWorkflow.verifyShopTransaction(
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
	serviceWorkflow.setNpcStep(PlayerBotNpcConversationStep::Ready);
	schedule(navigationDecisionDelay(*player));
}

void PlayerBotController::processBank(Player* player, const Position& currentPosition, const ServiceNpc& banker)
{
	if (!approachServiceNpc(player, banker, currentPosition)) {
		return;
	}
	Npc* npc = g_game.getNpcByID(banker.id);
	if (!npc || npc->isRemoved()) {
		stop("banker_unavailable", currentPosition);
		return;
	}
	if (serviceWorkflow.npcStep() == PlayerBotNpcConversationStep::Greet ||
	    serviceWorkflow.npcStep() == PlayerBotNpcConversationStep::Request) {
		const PlayerBotNpcSessionOutcome focus = serviceWorkflow.establishNpcFocus(*player, *npc, maximumServiceAttempts);
		for (uint8_t action = 0; action < focus.actionsIssued; ++action) {
			telemetry.recordActionAttempt();
		}
		if (focus.result == PlayerBotNpcSessionResult::Failed) {
			logActionFailure("bank", "npc_focus_unconfirmed", currentPosition);
			stop("banker_focus_unconfirmed", currentPosition);
			return;
		}
		if (focus.result == PlayerBotNpcSessionResult::Pending) {
			schedule(serviceWorkflow.npcNextDelay());
			return;
		}
	}
	if (serviceWorkflow.npcStep() == PlayerBotNpcConversationStep::Request) {
		if (!serviceWorkflow.hasBankDeposit()) {
			serviceWorkflow.beginBankDeposit(player->getMoney(), player->getBankBalance());
		}
		if (serviceWorkflow.bankTransaction().money == 0) {
			serviceWorkflow.setBankDepositComplete(true);
			serviceWorkflow.setNpcStep(PlayerBotNpcConversationStep::Ready);
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		telemetry.recordActionAttempt();
		npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "deposit all");
		serviceWorkflow.setNpcStep(PlayerBotNpcConversationStep::Confirm);
		schedule(SCHEDULER_MINTICKS);
		return;
	}
	if (serviceWorkflow.npcStep() == PlayerBotNpcConversationStep::Confirm) {
		telemetry.recordActionAttempt();
		npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "yes");
		serviceWorkflow.setNpcStep(PlayerBotNpcConversationStep::Verify);
		schedule(SCHEDULER_MINTICKS);
		return;
	}
	if (serviceWorkflow.npcStep() == PlayerBotNpcConversationStep::Verify && !serviceWorkflow.bankDepositComplete()) {
		const PlayerBotServiceVerification verification = serviceWorkflow.verifyBankDeposit(
			player->getMoney(), player->getBankBalance(), maximumServiceAttempts);
		if (verification.result != PlayerBotServiceVerificationResult::Success) {
			if (verification.result == PlayerBotServiceVerificationResult::Rejected) {
				logActionFailure("bank_deposit", "transaction_not_verified", currentPosition);
				stop("bank_deposit_not_verified", currentPosition);
				return;
			}
			serviceWorkflow.setNpcStep(PlayerBotNpcConversationStep::Request);
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		emit("action_result", currentPosition, "\"action\":\"bank_deposit\",\"result\":\"success\",\"count\":" +
		     std::to_string(verification.before.money) + ",\"bank_before\":" + std::to_string(verification.before.balance) +
		     ",\"bank_after\":" + std::to_string(player->getBankBalance()));
		say(*player, "Deposited " + std::to_string(verification.before.money) + " gold. Bank: " +
		     std::to_string(player->getBankBalance()) + '.');
		serviceWorkflow.setBankDepositComplete(true);
		serviceWorkflow.setNpcStep(PlayerBotNpcConversationStep::Ready);
	}
	if (serviceWorkflow.npcStep() == PlayerBotNpcConversationStep::Ready) {
		if (!serviceWorkflow.hasBankWithdrawal()) {
			const uint32_t amount = dispositionPolicy.bankWithdrawal(
			    {0, player->getFreeCapacity(), player->getMoney(), player->getBankBalance()}, Item::items[ITEM_GOLD_COIN].weight);
			if (amount == 0) {
				serviceWorkflow.setStage(PlayerBotServiceStage::Complete);
				schedule(SCHEDULER_MINTICKS);
				return;
			}
			serviceWorkflow.beginBankWithdrawal(player->getBankBalance(), amount);
		}
		telemetry.recordActionAttempt();
		npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "withdraw " + std::to_string(serviceWorkflow.bankTransaction().amount));
		serviceWorkflow.setNpcStep(PlayerBotNpcConversationStep::Confirm);
		schedule(SCHEDULER_MINTICKS);
		return;
	}
	if (serviceWorkflow.npcStep() == PlayerBotNpcConversationStep::Confirm) {
		telemetry.recordActionAttempt();
		npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "yes");
		serviceWorkflow.setNpcStep(PlayerBotNpcConversationStep::Verify);
		schedule(SCHEDULER_MINTICKS);
		return;
	}
	const PlayerBotServiceVerification verification = serviceWorkflow.verifyBankWithdrawal(
		player->getMoney(), player->getBankBalance(), maximumServiceAttempts);
	if (verification.result == PlayerBotServiceVerificationResult::Success) {
		emit("action_result", currentPosition, "\"action\":\"bank_withdraw\",\"result\":\"success\",\"count\":" +
		     std::to_string(verification.before.amount) + ",\"bank_before\":" +
		     std::to_string(verification.before.balance) + ",\"bank_after\":" + std::to_string(player->getBankBalance()));
		serviceWorkflow.setStage(PlayerBotServiceStage::Complete);
		schedule(SCHEDULER_MINTICKS);
		return;
	}
	if (verification.result == PlayerBotServiceVerificationResult::Rejected) {
		logActionFailure("bank_withdraw", "transaction_not_verified", currentPosition);
		stop("bank_withdraw_not_verified", currentPosition);
		return;
	}
	serviceWorkflow.setNpcStep(PlayerBotNpcConversationStep::Ready);
	schedule(SCHEDULER_MINTICKS);
}

void PlayerBotController::processService(Player* player, const Position& currentPosition)
{
	if (serviceWorkflow.stage() == PlayerBotServiceStage::Discover) {
		serviceWorkflow.setBankDepositComplete(false);
		discoverServices(currentPosition);
		schedule(SCHEDULER_MINTICKS);
		return;
	}
	if (serviceWorkflow.npcStep() == PlayerBotNpcConversationStep::Verify && serviceWorkflow.hasShopTransaction() &&
	    (serviceWorkflow.stage() == PlayerBotServiceStage::SellLoot || serviceWorkflow.stage() == PlayerBotServiceStage::BuyPotions)) {
		auto service = std::find_if(serviceWorkflow.shops().begin(), serviceWorkflow.shops().end(), [this](const ServiceNpc& candidate) {
			return candidate.id == serviceWorkflow.npcTargetId();
		});
		if (service == serviceWorkflow.shops().end()) {
			stop("shop_transaction_service_unavailable", currentPosition);
			return;
		}
		const bool purchase = serviceWorkflow.stage() != PlayerBotServiceStage::SellLoot;
		const char* action = serviceWorkflow.stage() == PlayerBotServiceStage::SellLoot ? "sell" : "buy_potions";
		const PlayerBotServiceTransaction& transaction = *serviceWorkflow.shopTransaction();
		processServiceShop(player, currentPosition, *service, action, transaction.itemId, transaction.amount, purchase);
		return;
	}
	if (serviceWorkflow.stage() == PlayerBotServiceStage::SellLoot) {
		if (const auto pending = serviceWorkflow.pendingSlottedSale(); pending &&
		    prepareSlottedSaleItem(player, pending->itemId, currentPosition)) {
			return;
		}
		uint16_t itemId = 0;
		const ServiceNpc* seller = findLootSeller(player, currentPosition, itemId);
		if (!seller) {
			serviceWorkflow.setStage(PlayerBotServiceStage::BuyPotions);
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		if (!serviceWorkflow.npcTargets(seller->id)) {
			serviceWorkflow.resetNpc(seller->id);
			serviceWorkflow.clearApproach();
			serviceWorkflow.clearRejectedApproaches();
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
	if (serviceWorkflow.stage() == PlayerBotServiceStage::BuyPotions) {
		const uint16_t itemId = smallHealthPotionItemId;
		const uint32_t currentCount = inventoryPolicy.inventoryItemCount(*player, itemId);
		if (currentCount >= smallHealthPotionRestockTarget) {
			serviceWorkflow.setStage(PlayerBotServiceStage::Bank);
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		const ServiceNpc* seller = findShopFor(itemId, true, currentPosition);
		if (!seller) {
			stop("required_shop_offer_unavailable", currentPosition);
			return;
		}
		const ShopInfo* offer = findOffer(*seller, itemId, true);
		if (!offer || offer->buyPrice == 0) {
			stop("required_shop_offer_unavailable", currentPosition);
			return;
		}
		const PlayerBotEconomyRestockDecision restock = dispositionPolicy.restock(
		    {currentCount, player->getFreeCapacity(), player->getMoney(), player->getBankBalance()}, offer->buyPrice,
		    Item::items[itemId].weight);
		if (restock.insufficientFunds) {
			stop("insufficient_potion_funds", currentPosition);
			return;
		}
		const uint32_t amount = restock.amount;
		if (amount == 0) {
			serviceWorkflow.setStage(PlayerBotServiceStage::Bank);
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		if (!serviceWorkflow.npcTargets(seller->id)) {
			serviceWorkflow.resetNpc(seller->id);
			serviceWorkflow.clearApproach();
			serviceWorkflow.clearRejectedApproaches();
			clearNavigation();
		}
		processServiceShop(player, currentPosition, *seller, "buy_potions", itemId, amount, true);
		return;
	}
	if (serviceWorkflow.stage() == PlayerBotServiceStage::Bank) {
		const ServiceNpc* banker = findNearestService(serviceWorkflow.bankers(), currentPosition);
		if (!banker) {
			stop("banker_unavailable", currentPosition);
			return;
		}
		if (!serviceWorkflow.npcTargets(banker->id)) {
			serviceWorkflow.resetNpc(banker->id);
			serviceWorkflow.clearApproach();
			serviceWorkflow.clearRejectedApproaches();
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
	depotWorkflow.clearDiscovery();
}

bool PlayerBotController::discoverDepot(Player& player, const Position& currentPosition)
{
	const auto now = std::chrono::steady_clock::now();
	depotWorkflow.expireRejectedApproaches(now);
	if (!depotWorkflow.hasSelectedDepot() && depotWorkflow.candidatesPrepared() &&
	    depotWorkflow.discoveryAnchor() != currentPosition) {
		clearDepotDiscovery();
		depotWorkflow.resetAttempts();
	}
	uint16_t lockerItemId = 0;
	if (depotWorkflow.hasSelectedDepot() && depotWorkflow.approachPosition() != Position() &&
	    playerbot::isInsideLocalPlanningArea(currentPosition, depotWorkflow.lockerPosition()) &&
	    findDepotLocker(depotWorkflow.lockerPosition(), depotWorkflow.depotId(), lockerItemId) &&
	    lockerItemId == depotWorkflow.lockerItemId()) {
		return true;
	}
	if (depotWorkflow.hasSelectedDepot()) {
		clearDepotDiscovery();
		depotWorkflow.resetAttempts();
	}
	auto finishUnavailable = [&](const char* reason) {
		const uint32_t attempts = depotWorkflow.incrementAttempts();
		const uint32_t retryDelay = std::min<uint32_t>(depotRetryMaximumInterval,
		                                               depotRetryInitialInterval << std::min<uint32_t>(attempts - 1, 2));
		if (shouldEmitRepeated(std::string("depot_discover:") + reason)) {
			emit("action_result", currentPosition,
			     std::string("\"action\":\"depot_discover\",\"result\":\"unavailable\",\"reason\":") + jsonString(reason) +
			         ",\"indexed\":" + std::to_string(depotWorkflow.indexedCandidateCount()) +
			         ",\"in_scope\":" + std::to_string(depotWorkflow.inScopeCandidateCount()) +
			         ",\"standable\":" + std::to_string(depotWorkflow.standableCandidateCount()) +
			         ",\"suppressed\":" + std::to_string(depotWorkflow.suppressedApproachCount()) +
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

	if (!depotWorkflow.candidatesPrepared()) {
		depotWorkflow.beginDiscovery(currentPosition);
		for (const auto& entry : g_game.map.getDepotLockerPositions()) {
			for (const Position& lockerPosition : entry.second) {
				depotWorkflow.recordIndexedCandidate();
				if (!playerbot::isInsideLocalPlanningArea(currentPosition, lockerPosition)) {
					continue;
				}
				uint16_t indexedLockerItemId = 0;
				if (!findDepotLocker(lockerPosition, entry.first, indexedLockerItemId)) {
					continue;
				}
				depotWorkflow.recordInScopeCandidate();
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
						depotWorkflow.recordStandableCandidate();
						if (depotWorkflow.isApproachRejected(approach)) {
							depotWorkflow.recordSuppressedApproach();
							continue;
						}
						depotWorkflow.recordCandidate({entry.first, indexedLockerItemId, lockerPosition, approach,
						                                     playerbot::localPlanningDistance(currentPosition, approach)});
					}
				}
			}
		}
		depotWorkflow.sortCandidates();
	}

	if (!depotWorkflow.hasCandidates()) {
		if (depotWorkflow.suppressedApproachCount() != 0) {
			const auto earliestExpiry = depotWorkflow.earliestRejectedApproachExpiry();
			if (!earliestExpiry) {
				return false;
			}
			const uint32_t retryDelay = static_cast<uint32_t>(std::max<int64_t>(
			    1, std::chrono::duration_cast<std::chrono::milliseconds>(*earliestExpiry - now).count()));
			clearDepotDiscovery();
			schedule(retryDelay);
			return false;
		}
		finishUnavailable(depotWorkflow.inScopeCandidateCount() == 0 ? "no_local_locker" :
		                  depotWorkflow.standableCandidateCount() == 0 ? "no_standable_approach" : "no_reachable_locker");
		return false;
	}

	uint32_t routeValidations = 0;
	while (depotWorkflow.hasNextCandidate() && routeValidations < depotRouteValidationsPerDecision) {
		const std::optional<PlayerBotDepotCandidate> nextCandidate = depotWorkflow.takeNextCandidate();
		if (!nextCandidate) {
			break;
		}
		const PlayerBotDepotCandidate candidate = *nextCandidate;
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
			depotWorkflow.rejectApproach(candidate.approachPosition, now + depotApproachSuppression);
			continue;
		}
		depotWorkflow.select(candidate);
		navigationRuntime.resetFixedTargetRouteFailures();
		const size_t routeSteps = steps.size();
		adoptNavigationPlan(depotWorkflow.approachPosition(), std::move(steps));
		std::ostringstream fields;
		fields << "\"action\":\"depot_discover\",\"result\":\"success\",\"depot_id\":" << depotWorkflow.depotId()
		       << ",\"locker_item_id\":" << depotWorkflow.lockerItemId() << ",\"locker\":{\"x\":" << depotWorkflow.lockerPosition().x
		       << ",\"y\":" << depotWorkflow.lockerPosition().y << ",\"z\":" << static_cast<uint16_t>(depotWorkflow.lockerPosition().z)
		       << "},\"approach\":{\"x\":" << depotWorkflow.approachPosition().x << ",\"y\":" << depotWorkflow.approachPosition().y
		       << ",\"z\":" << static_cast<uint16_t>(depotWorkflow.approachPosition().z) << "},\"distance\":" << candidate.distance
		       << ",\"route_steps\":" << routeSteps << ",\"expanded_nodes\":" << expandedNodes
		       << ",\"indexed\":" << depotWorkflow.indexedCandidateCount() << ",\"in_scope\":" << depotWorkflow.inScopeCandidateCount()
		       << ",\"standable\":" << depotWorkflow.standableCandidateCount();
		emit("action_result", currentPosition, fields.str());
		return true;
	}

	if (depotWorkflow.hasNextCandidate()) {
		emit("action_result", currentPosition,
		     std::string("\"action\":\"depot_discover\",\"result\":\"continuing\",\"reason\":\"route_validation_budget_exhausted\"") +
		         ",\"indexed\":" + std::to_string(depotWorkflow.indexedCandidateCount()) +
		         ",\"in_scope\":" + std::to_string(depotWorkflow.inScopeCandidateCount()) +
		         ",\"standable\":" + std::to_string(depotWorkflow.standableCandidateCount()) +
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
	if (opened && opened->getDepotLocker() && opened->getDepotLocker()->getDepotId() == depotWorkflow.depotId()) {
		depotWorkflow.resetAttempts();
		depotWorkflow.setStage(PlayerBotDepotStage::OpenChest);
		return true;
	}
	Tile* tile = g_game.map.getTile(depotWorkflow.lockerPosition());
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
		if (!container || !container->getDepotLocker() || container->getDepotLocker()->getDepotId() != depotWorkflow.depotId()) {
			continue;
		}
		const int32_t stackPosition = tile->getThingIndex(item);
		if (stackPosition < 0 || stackPosition > UINT8_MAX) {
			break;
		}
		if (depotWorkflow.attempts() >= maximumDepotAttempts) {
			logActionFailure("depot_open_locker", "open_not_verified", currentPosition);
			stop("depot_locker_open_failed", currentPosition);
			return false;
		}
		const uint32_t attempts = depotWorkflow.incrementAttempts();
		player.closeContainer(depotLockerContainerId);
		telemetry.recordActionAttempt();
		g_game.playerUseItem(playerId, depotWorkflow.lockerPosition(), static_cast<uint8_t>(stackPosition), depotLockerContainerId,
		                     item->getClientID());
		emit("action_result", currentPosition, "\"action\":\"depot_open_locker\",\"result\":\"requested\",\"depot_id\":" +
		     std::to_string(depotWorkflow.depotId()) + ",\"container_id\":" + std::to_string(depotLockerContainerId) +
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
	if (!locker || !locker->getDepotLocker() || locker->getDepotLocker()->getDepotId() != depotWorkflow.depotId()) {
		depotWorkflow.setStage(PlayerBotDepotStage::OpenLocker);
		schedule(blockedRouteRetryInterval);
		return false;
	}
	DepotChest* chest = player.getDepotChest(depotWorkflow.depotId(), false);
	if (!chest) {
		logActionFailure("depot_open_chest", "player_chest_missing", currentPosition);
		stop("depot_chest_missing", currentPosition);
		return false;
	}
	if (fixtureRuntime.depotMoveFixture() == DepotMoveFixture::Rejected) {
		chest->setMaxDepotItems(chest->getItemHoldingCount());
	}
	if (player.getContainerByID(depotChestContainerId) == chest) {
		depotWorkflow.resetAttempts();
		depotWorkflow.setStage(PlayerBotDepotStage::Deposit);
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
	if (depotWorkflow.attempts() >= maximumDepotAttempts) {
		logActionFailure("depot_open_chest", "open_not_verified", currentPosition);
		stop("depot_chest_open_failed", currentPosition);
		return false;
	}
	const uint32_t attempts = depotWorkflow.incrementAttempts();
	player.closeContainer(depotChestContainerId);
	telemetry.recordActionAttempt();
	g_game.playerUseItem(playerId, Position(0xFFFF, 0x40 | depotLockerContainerId, static_cast<uint8_t>(index)),
	                     static_cast<uint8_t>(index), depotChestContainerId, chest->getClientID());
	emit("action_result", currentPosition, "\"action\":\"depot_open_chest\",\"result\":\"requested\",\"depot_id\":" +
	     std::to_string(depotWorkflow.depotId()) + ",\"container_id\":" + std::to_string(depotChestContainerId) +
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
	if (!fixtureRuntime.consumeDepotRestartCheckpoint(player, checkpoint)) {
		return false;
	}
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
	if (depotWorkflow.hasPendingMove()) {
		const PlayerBotDepotMove pending = depotWorkflow.move();
		const uint32_t destinationCount = destination->getItemTypeCount(pending.itemId);
		if (destinationCount <= pending.destinationCount) {
			logActionFailure("deposit", "fixture_item_move_failed", currentPosition);
			stop("fake_depot_rejected_loot", currentPosition);
			return;
		}
		emit("action_result", currentPosition, "\"action\":\"deposit\",\"result\":\"success\",\"fixture\":true,\"item_id\":" +
		     std::to_string(pending.itemId) + ",\"count\":" + std::to_string(destinationCount - pending.destinationCount));
		depotWorkflow.clearMove();
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
	                     std::to_string(huntRuntime.completedCycles()));
		if (fixtureRuntime.progressionEnabled()) {
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
	depotWorkflow.beginMove({depositItem->getID(), destination->getItemTypeCount(depositItem->getID()),
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
	if (depotWorkflow.hasPendingMove()) {
		Container* chest = player->getContainerByID(depotChestContainerId);
		if (!chest) {
			depotWorkflow.setStage(PlayerBotDepotStage::OpenChest);
			schedule(navigationDecisionDelay(*player));
			return;
		}
		const PlayerBotDepotMoveVerification verification = depotWorkflow.verifyMove(
			inventoryPolicy.inventoryItemCount(*player, depotWorkflow.move().itemId),
			chest->getItemTypeCount(depotWorkflow.move().itemId), maximumDepotAttempts);
		const PlayerBotDepotMove& move = verification.before;
		if (verification.result == PlayerBotDepotMoveResult::Moved) {
			std::ostringstream fields;
			fields << "\"action\":\"deposit\",\"result\":" << jsonString(verification.movedCount == move.requestedCount ? "success" : "partial")
			       << ",\"policy\":\"known_loot\",\"depot_id\":" << depotWorkflow.depotId() << ",\"container_id\":"
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
			serviceWorkflow.deferSlottedSale(failedItemId, failedSlot,
			                                 std::chrono::steady_clock::now() + unavailableDispositionCooldown);
			emit("action_result", currentPosition,
			     "\"action\":\"deposit\",\"result\":\"deferred\",\"reason\":\"move_not_verified\",\"policy\":\"known_loot\",\"depot_id\":" +
			         std::to_string(depotWorkflow.depotId()) + ",\"container_id\":" + std::to_string(depotChestContainerId) +
			         ",\"item_id\":" + std::to_string(failedItemId) + ",\"source_slot\":" + std::to_string(failedSlot) +
			         ",\"provider_available\":false,\"disposition\":\"deposit\",\"cooldown_ms\":" +
			         std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(unavailableDispositionCooldown).count()));
			schedule(SCHEDULER_MINTICKS);
			return;
		} else if (verification.result == PlayerBotDepotMoveResult::Rejected) {
			emit("action_result", currentPosition, "\"action\":\"deposit\",\"result\":\"failed\",\"reason\":\"no_slot_or_move_rejected\",\"policy\":\"known_loot\",\"depot_id\":" +
			     std::to_string(depotWorkflow.depotId()) + ",\"container_id\":" + std::to_string(depotChestContainerId) +
			     ",\"item_id\":" + std::to_string(move.itemId) + ",\"requested\":" +
			     std::to_string(move.requestedCount) + ",\"verified\":0,\"inventory_before\":" +
			     std::to_string(move.inventoryCount) + ",\"inventory_after\":" + std::to_string(verification.inventoryCount) +
			     ",\"depot_before\":" + std::to_string(move.destinationCount) + ",\"depot_after\":" +
			     std::to_string(verification.destinationCount) + ",\"retry\":" + std::to_string(verification.attempts));
			stop("depot_no_slot_or_move_rejected", currentPosition);
			return;
		} else {
			emit("action_result", currentPosition, "\"action\":\"deposit\",\"result\":\"retry\",\"reason\":\"not_verified\",\"policy\":\"known_loot\",\"depot_id\":" +
			     std::to_string(depotWorkflow.depotId()) + ",\"container_id\":" + std::to_string(depotChestContainerId) +
			     ",\"item_id\":" + std::to_string(move.itemId) + ",\"requested\":" +
			     std::to_string(move.requestedCount) + ",\"verified\":0,\"inventory_before\":" +
			     std::to_string(move.inventoryCount) + ",\"inventory_after\":" + std::to_string(verification.inventoryCount) +
			     ",\"depot_before\":" + std::to_string(move.destinationCount) + ",\"depot_after\":" +
			     std::to_string(verification.destinationCount) + ",\"retry\":" + std::to_string(verification.attempts));
			schedule(navigationDecisionDelay(*player));
			return;
		}
	}

	if (depotWorkflow.stage() == PlayerBotDepotStage::Approach || depotWorkflow.stage() == PlayerBotDepotStage::Discover) {
		depotWorkflow.setStage(PlayerBotDepotStage::OpenLocker);
	}
	if (depotWorkflow.stage() == PlayerBotDepotStage::OpenLocker && !openDepotLocker(*player, currentPosition)) {
		return;
	}
	if (depotWorkflow.stage() == PlayerBotDepotStage::OpenChest && !openDepotChest(*player, currentPosition)) {
		return;
	}
	Container* chest = player->getContainerByID(depotChestContainerId);
	if (!chest || player->getDepotChest(depotWorkflow.depotId(), false) != chest) {
		depotWorkflow.setStage(PlayerBotDepotStage::OpenChest);
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
			if (const auto deferred = serviceWorkflow.nextDeferredSlottedSale(now)) {
				schedule(static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
				                                   *deferred - now).count()));
				return;
			}
			stop("depot_capacity_not_recovered", currentPosition);
			return;
		}
		std::ostringstream fields;
		fields << "\"action\":\"deposit\",\"result\":\"complete\",\"depot_id\":" << depotWorkflow.depotId()
	                       << ",\"container_id\":" << static_cast<uint32_t>(depotChestContainerId) << ",\"cycle\":" << huntRuntime.completedCycles();
		emit("action_result", currentPosition, fields.str());
		player->closeContainer(depotChestContainerId);
		player->closeContainer(depotLockerContainerId);
		depotWorkflow.setStage(PlayerBotDepotStage::Depart);
		if (pauseDepotFixtureForRestart(*player, DepotRestartCheckpoint::Depart, currentPosition)) {
			return;
		}
		if (fixtureRuntime.progressionEnabled()) {
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

	depotWorkflow.beginMove({depositItem->getID(), chest->getItemTypeCount(depositItem->getID()),
	                        inventoryPolicy.inventoryItemCount(*player, depositItem->getID()), count, sourceSlot});
	const uint8_t submittedCount = fixtureRuntime.depotMoveFixture() == DepotMoveFixture::Partial && count > 1 ? count - 1 : count;
	telemetry.recordActionAttempt();
	g_game.playerMoveItem(player, sourcePosition, depositItem->getClientID(), sourceIndex,
	                      Position(0xFFFF, 0x40 | depotChestContainerId, containerDestinationIndex(*chest, *depositItem)),
	                      submittedCount, depositItem, chest);
	emit("action_result", currentPosition, "\"action\":\"deposit\",\"result\":\"requested\",\"policy\":\"known_loot\",\"depot_id\":" +
	     std::to_string(depotWorkflow.depotId()) + ",\"container_id\":" + std::to_string(depotChestContainerId) + ",\"item_id\":" +
	     std::to_string(depotWorkflow.move().itemId) + ",\"requested\":" + std::to_string(count) + ",\"submitted\":" +
	     std::to_string(submittedCount) + ",\"inventory_before\":" +
	     std::to_string(depotWorkflow.move().inventoryCount) + ",\"depot_before\":" + std::to_string(depotWorkflow.move().destinationCount) +
	     ",\"source_slot\":" + (sourceSlot == CONST_SLOT_WHEREEVER ? "null" : std::to_string(sourceSlot)) +
	     ",\"provider_available\":false,\"disposition\":\"deposit\"");
	if (pauseDepotFixtureForRestart(*player, DepotRestartCheckpoint::Deposit, currentPosition)) {
		return;
	}
	schedule(navigationDecisionDelay(*player));
}
