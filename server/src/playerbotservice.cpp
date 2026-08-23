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
	return PlayerBotTurnRouter::cyclePhaseName(turnRouter.cyclePhase());
}

void PlayerBotController::setCyclePhase(CyclePhase phase, const Position& position, const char* reason)
{
	if (turnRouter.cyclePhase() == phase) {
		return;
	}
	const char* previous = cyclePhaseName();
	if (turnRouter.cyclePhase() == CyclePhase::Hunt && phase != CyclePhase::Hunt) {
		huntRuntime.cancelPlanning();
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
	const auto traversalTarget = combatRuntime.traversalTarget();
	const uint32_t previousTarget = traversalTarget ? traversalTarget->id : 0;
	g_game.playerCancelAttackAndFollow(playerId);
	clearTraversalTarget(position, reason);
	resetNavigation();
	lootWorkflow.reset();
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
	resetNavigation();
	lootWorkflow.reset();
	player->closeContainer(corpseContainerId);
	setStage(ScenarioStage::Traverse, position);
	setCyclePhase(CyclePhase::Idle, position, reason);
	emit("goal_result", position,
	     "\"decision_id\":" + std::to_string(progressionRuntime.decisionId()) +
	         ",\"goal\":\"hunt\",\"result\":\"success\",\"reason\":" + jsonString(reason));
	selectTopLevelGoal(*player, position, reason);
	schedule(SCHEDULER_MINTICKS);
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

void PlayerBotController::processService(Player* player, const Position& currentPosition)
{
	PlayerBotServiceObservation observation;
	observation.currentPosition = currentPosition;
	observation.freeCapacity = player->getFreeCapacity();
	observation.money = player->getMoney();
	observation.bankBalance = player->getBankBalance();
	observation.goldCoinWeight = Item::items[ITEM_GOLD_COIN].weight;
	observation.smallHealthPotionWeight = Item::items[PlayerBotDispositionPolicy::smallHealthPotionItemId].weight;
	Item* serviceBackpackItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
	Container* serviceBackpack = serviceBackpackItem ? serviceBackpackItem->getContainer() : nullptr;
	observation.actionAvailable = player->canDoAction();
	observation.backpackAvailable = serviceBackpack != nullptr;
	observation.backpackOpen = serviceBackpack && player->getContainerID(serviceBackpack) >= 0;
	observation.maximumAttempts = maximumServiceAttempts;
	observation.slottedSaleCooldownMs = static_cast<uint32_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(unavailableDispositionCooldown).count());
	for (const auto& entry : g_game.getNpcs()) {
		Npc* npc = entry.second;
		const std::string* capability = npc && !npc->isRemoved() ? npc->getParameter("playerbot_service") : nullptr;
		if (!capability || serviceDistance(player->getTemplePosition(), {npc->getID(), npc->getPosition()}) > maximumServiceDistanceFromTemple) continue;
		if (*capability != "shop" && *capability != "banker") continue;
		PlayerBotEconomyProvider provider{npc->getID(), npc->getPosition()};
		for (const ShopInfo& offer : npc->getShopOffers()) {
			const ItemType& type = Item::items[offer.itemId];
			if (type.isFluidContainer() || type.isSplash()) continue;
			const bool buyAvailable = offer.buyPrice != 0 && fixtureDriver.observeProvider(true, offer.itemId, true).available;
			const bool sellAvailable = offer.sellPrice != 0 && fixtureDriver.observeProvider(true, offer.itemId, false).available;
			if (!buyAvailable && !sellAvailable) continue;
			provider.offers.push_back({offer.itemId, buyAvailable ? offer.buyPrice : 0,
			                           sellAvailable ? offer.sellPrice : 0, static_cast<uint8_t>(offer.subType)});
			observation.inventoryCounts.emplace(offer.itemId, inventoryPolicy.inventoryItemCount(*player, offer.itemId));
			observation.backpackSaleCounts.emplace(offer.itemId, inventoryPolicy.backpackSaleItemCount(*player, offer.itemId));
		}
		int32_t onBuy;
		int32_t onSell;
		PlayerBotServiceProviderObservation providerObservation{
			true, Position::areInRange<3, 3, 0>(currentPosition, npc->getPosition()),
			player->getShopOwner(onBuy, onSell) == npc && !player->getShopItemList().empty()};
		if (!providerObservation.inRange) {
			for (int32_t xOffset = -3; xOffset <= 3; ++xOffset) for (int32_t yOffset = -3; yOffset <= 3; ++yOffset) {
				if (xOffset == 0 && yOffset == 0) continue;
				const Position approach(npc->getPosition().x + xOffset, npc->getPosition().y + yOffset, npc->getPosition().z);
				Tile* tile = g_game.map.getTile(approach);
				if (tile && tile->queryAdd(0, *player, 1, 0) == RETURNVALUE_NOERROR) {
					providerObservation.approaches.push_back({approach, static_cast<uint32_t>(
						std::max(Position::getDistanceX(currentPosition, approach), Position::getDistanceY(currentPosition, approach)))});
				}
			}
			std::sort(providerObservation.approaches.begin(), providerObservation.approaches.end(), [](const auto& left,
			                                                                                             const auto& right) {
				return left.distance != right.distance ? left.distance < right.distance : left.position < right.position;
			});
		}
		observation.providers[npc->getID()] = std::move(providerObservation);
		observation.discoveries.push_back({npc->getID(), npc->getName(), *capability,
		                                  static_cast<uint32_t>(npc->getShopOffers().size())});
		if (*capability == "shop" && !provider.offers.empty()) observation.shops.push_back(std::move(provider));
		else if (*capability == "banker") observation.bankers.push_back(std::move(provider));
	}
	observation.now = std::chrono::steady_clock::now();
	for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
		Item* item = player->getInventoryItem(static_cast<slots_t>(slot));
		if (item && inventoryPolicy.isActionableSlottedItem(*player, *item, static_cast<slots_t>(slot), 0)) {
			observation.slottedSaleItems.push_back({item->getID(), static_cast<slots_t>(slot), item->getItemCount()});
		}
	}
	PlayerBotServiceCommand command = serviceWorkflow.advance(observation, economyCatalog, dispositionPolicy);
	const std::vector<PlayerBotServiceDiscovery> discoveries = command.discoveries;
	std::deque<PlayerBotNavigationStep> approachSteps;
	while (command.type == PlayerBotServiceCommandType::ValidateProviderRoute) {
		PlayerBotServiceObservation routeObservation = observation;
		routeObservation.approachRoute.providerId = command.providerId;
		routeObservation.approachRoute.destination = command.destination;
		const auto startedAt = std::chrono::steady_clock::now();
		PlayerBotNavigationRoutePlan routePlan;
		if (command.destination != currentPosition) routePlan = planNavigationRoute(*player, command.destination);
		const bool reached = command.destination == currentPosition ||
		                     (routePlan.metrics.result == PlayerBotNavigationResult::Reached && !routePlan.steps.empty());
		telemetry.recordPathfinding(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - startedAt), reached);
		routeObservation.approachRoute.result = reached ? PlayerBotServiceRouteResult::Reached : PlayerBotServiceRouteResult::Unreachable;
		routeObservation.approachRoute.steps = static_cast<uint32_t>(routePlan.steps.size());
		routeObservation.approachRoute.expandedNodes = routePlan.metrics.expandedNodes;
		if (reached) approachSteps = std::move(routePlan.steps);
		command = serviceWorkflow.advance(routeObservation, economyCatalog, dispositionPolicy);
	}
	for (const PlayerBotServiceDiscovery& discovery : discoveries) {
		emit("service_discovered", currentPosition, "\"capability\":" + jsonString(discovery.capability) +
		     ",\"npc_id\":" + std::to_string(discovery.npcId) + ",\"npc_name\":" + jsonString(discovery.npcName) +
		     ",\"offers\":" + std::to_string(discovery.offers));
	}
	if (command.verification && command.verification->result == PlayerBotServiceVerificationResult::Success && command.transaction) {
		const PlayerBotServiceTransaction& transaction = *command.transaction;
		if (transaction.itemId != 0) {
			const char* action = serviceWorkflow.stage() == PlayerBotServiceStage::SellLoot ? "sell" : "buy_potions";
			emit("action_result", currentPosition, "\"action\":" + jsonString(action) + ",\"result\":\"success\",\"item_id\":" +
			     std::to_string(transaction.itemId) + ",\"count\":" + std::to_string(transaction.amount) +
			     ",\"carried_before\":" + std::to_string(transaction.money) + ",\"carried_after\":" +
			     std::to_string(observation.money) + ",\"bank_before\":" + std::to_string(transaction.balance) +
			     ",\"bank_after\":" + std::to_string(observation.bankBalance));
			const ItemType& itemType = Item::items[transaction.itemId];
			say(*player, std::string(action) == "sell" ? "Sold " + std::to_string(transaction.amount) + " " +
			    (transaction.amount == 1 ? itemType.name : itemType.getPluralName()) + '.' : "Bought " +
			    std::to_string(transaction.amount) + " " + (transaction.amount == 1 ? itemType.name : itemType.getPluralName()) + '.');
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
	Npc* provider = command.providerId == 0 ? nullptr : g_game.getNpcByID(command.providerId);
	if (command.type == PlayerBotServiceCommandType::NavigateProvider && provider) {
		if (!approachSteps.empty()) observeNavigationPlan(command.destination, std::move(approachSteps));
		if (processNavigation(player, currentPosition, command.destination)) schedule(SCHEDULER_MINTICKS);
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
		beginReturn(player, currentPosition, "service_complete");
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
		for (const auto& entry : g_game.map.getDepotLockerPositions()) for (const Position& lockerPosition : entry.second) {
			++result.indexedCandidates;
			if (!playerbot::isInsideLocalPlanningArea(currentPosition, lockerPosition)) continue;
			uint16_t lockerItemId = 0;
			if (!findDepotLocker(lockerPosition, entry.first, lockerItemId)) continue;
			++result.inScopeCandidates;
			for (int32_t xOffset = -1; xOffset <= 1; ++xOffset) for (int32_t yOffset = -1; yOffset <= 1; ++yOffset) {
				if (xOffset == 0 && yOffset == 0) continue;
				const Position approach(lockerPosition.x + xOffset, lockerPosition.y + yOffset, lockerPosition.z);
				Tile* tile = g_game.map.getTile(approach);
				if (!tile || tile->queryAdd(0, player, 1, FLAG_IGNOREBLOCKCREATURE) != RETURNVALUE_NOERROR) continue;
				++result.standableCandidates;
				result.candidates.push_back({entry.first, lockerItemId, lockerPosition, approach,
				                             playerbot::localPlanningDistance(currentPosition, approach)});
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
			(playerbot::isInsideLocalPlanningArea(currentPosition, candidate.lockerPosition) &&
			 findDepotLocker(candidate.lockerPosition, candidate.depotId, lockerItemId) && lockerItemId == candidate.lockerItemId &&
			 tile && tile->queryAdd(0, player, 1, FLAG_IGNOREBLOCKCREATURE) == RETURNVALUE_NOERROR);
		const auto startedAt = std::chrono::steady_clock::now();
		PlayerBotNavigationRoutePlan routePlan;
		if (valid && candidate.approachPosition != currentPosition) routePlan = planNavigationRoute(player, candidate.approachPosition);
		const bool reached = valid && (candidate.approachPosition == currentPosition ||
			(routePlan.metrics.result == PlayerBotNavigationResult::Reached && !routePlan.steps.empty()));
		telemetry.recordPathfinding(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - startedAt), reached);
		observation.routeResult = reached ? PlayerBotDepotRouteResult::Reached : PlayerBotDepotRouteResult::Unreachable;
		observation.routeSteps = static_cast<uint32_t>(routePlan.steps.size());
		observation.expandedNodes = routePlan.metrics.expandedNodes;
		if (reached) steps = std::move(routePlan.steps);
		command = advance(observation);
	}
	if (command.type == PlayerBotDepotCommandType::ValidateRoute) {
		emit("action_result", currentPosition,
		     "\"action\":\"depot_discover\",\"result\":\"continuing\",\"reason\":\"route_validation_budget_exhausted\",\"indexed\":" +
		         std::to_string(command.snapshot.indexedCandidates) + ",\"in_scope\":" +
		         std::to_string(command.snapshot.inScopeCandidates) + ",\"standable\":" +
		         std::to_string(command.snapshot.standableCandidates) + ",\"route_validations\":" +
		         std::to_string(routeValidations));
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
		       << ",\"expanded_nodes\":" << command.telemetry.expandedNodes << ",\"indexed\":" << command.snapshot.indexedCandidates
		       << ",\"in_scope\":" << command.snapshot.inScopeCandidates << ",\"standable\":" << command.snapshot.standableCandidates;
		emit("action_result", currentPosition, fields.str());
		return true;
	}
	if (command.type == PlayerBotDepotCommandType::Fail) {
		const char* reason = command.snapshot.inScopeCandidates == 0 ? "no_local_locker" :
		                     command.snapshot.standableCandidates == 0 ? "no_standable_approach" : "no_reachable_locker";
		logActionFailure("depot_discover", reason, currentPosition);
		stop("depot_unavailable", currentPosition);
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
			emit("action_result", currentPosition, "\"action\":\"deposit\",\"result\":\"failed\",\"reason\":\"no_slot_or_move_rejected\",\"policy\":\"known_loot\",\"depot_id\":" +
			     std::to_string(command.snapshot.selected.depotId) + ",\"container_id\":" + std::to_string(depotChestContainerId) +
			     ",\"item_id\":" + std::to_string(move.itemId) + ",\"requested\":" + std::to_string(move.requestedCount) +
			     ",\"verified\":0,\"inventory_before\":" + std::to_string(move.inventoryCount) + ",\"inventory_after\":" +
			     std::to_string(verification.inventoryCount) + ",\"depot_before\":" + std::to_string(move.destinationCount) +
			     ",\"depot_after\":" + std::to_string(verification.destinationCount) + ",\"retry\":" + std::to_string(verification.attempts));
			stop("depot_no_slot_or_move_rejected", currentPosition);
			return;
		}
		std::ostringstream fields;
		fields << "\"action\":\"deposit\",\"result\":" << jsonString(verification.result == PlayerBotDepotMoveResult::Moved ?
			(verification.movedCount == move.requestedCount ? "success" : "partial") :
			verification.result == PlayerBotDepotMoveResult::Deferred ? "deferred" : "retry") << ",\"policy\":\"known_loot\",\"depot_id\":"
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
		if (verification.result == PlayerBotDepotMoveResult::Deferred) {
			const int64_t delay = command.snapshot.deferredDepositRetryAt ?
			    std::chrono::duration_cast<std::chrono::milliseconds>(
			        *command.snapshot.deferredDepositRetryAt - std::chrono::steady_clock::now()).count() : 1;
			schedule(static_cast<uint32_t>(std::max<int64_t>(1, delay)));
			return;
		}
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
		if (command.outcome == PlayerBotDepotOutcome::Deferred && command.snapshot.deferredDepositRetryAt) {
			const int64_t delay = std::chrono::duration_cast<std::chrono::milliseconds>(
			    *command.snapshot.deferredDepositRetryAt - std::chrono::steady_clock::now()).count();
			schedule(static_cast<uint32_t>(std::max<int64_t>(1, delay)));
		} else {
			schedule(navigationDecisionDelay(*player));
		}
		return;
	}
	if (command.type == PlayerBotDepotCommandType::Fail) {
		stop("depot_action_failed", currentPosition);
		return;
	}
	if (command.type == PlayerBotDepotCommandType::Depart) {
		std::ostringstream fields;
		fields << "\"action\":\"deposit\",\"result\":\"complete\",\"depot_id\":" << command.snapshot.selected.depotId
		       << ",\"container_id\":" << static_cast<uint32_t>(depotChestContainerId) << ",\"cycle\":" << huntRuntime.completedCycles();
		emit("action_result", currentPosition, fields.str());
		player->closeContainer(depotChestContainerId);
		player->closeContainer(depotLockerContainerId);
		if (pauseDepotFixtureForRestart(*player, DepotRestartCheckpoint::Depart, currentPosition)) return;
		if (fixtureDriver.progressionGoalLoop(true).selectGoal) {
			emit("goal_result", currentPosition,
			     "\"decision_id\":" + std::to_string(progressionRuntime.decisionId()) +
			         ",\"goal\":\"service\",\"result\":\"success\",\"reason\":\"service_complete\"");
			selectTopLevelGoal(*player, currentPosition, "service_complete");
		}
		else startHunt(player, currentPosition, "deposit_complete");
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
			const uint32_t reserve = inventoryPolicy.protectedItemReserve(depositItem->getID());
			const uint32_t movable = depositItem->isStackable() && carried > reserve ?
				std::min<uint32_t>(depositItem->getItemCount(), carried - reserve) : (carried > reserve ? 1 : 0);
			count = static_cast<uint8_t>(std::min<uint32_t>(movable, UINT8_MAX));
		}
	}
	if ((!depositItem || count == 0) && inventoryPolicy.effectiveFreeCapacity(*player) < returnCapacityThreshold) {
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
		if (command.outcome == PlayerBotDepotOutcome::Deferred && command.snapshot.deferredDepositRetryAt) {
			const int64_t delay = std::chrono::duration_cast<std::chrono::milliseconds>(
			    *command.snapshot.deferredDepositRetryAt - std::chrono::steady_clock::now()).count();
			schedule(static_cast<uint32_t>(std::max<int64_t>(1, delay)));
		} else {
			schedule(navigationDecisionDelay(*player));
		}
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
