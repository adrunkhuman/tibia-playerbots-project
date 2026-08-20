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

// Playerbot lifecycle, scheduling, navigation execution, and telemetry.
using namespace playerbot;

namespace {
	constexpr uint32_t maximumSpellObservationValue = 10000;
}

const PlayerBotTestPolicy& playerbot::testPolicyFromEnvironment()
{
	static const PlayerBotTestPolicy policy = []() {
		const char* gameplayModeValue = std::getenv("PLAYERBOT_GAMEPLAY_MODE");
		const char* regressionModeValue = std::getenv("PLAYERBOT_REGRESSION_MODE");
		const char* gameplayMode = gameplayModeValue && *gameplayModeValue != '\0' ? gameplayModeValue : nullptr;
		const char* regressionMode = regressionModeValue && *regressionModeValue != '\0' ? regressionModeValue : nullptr;
		const bool progressionMode = gameplayMode &&
			(std::strcmp(gameplayMode, "progression") == 0 ||
			 std::strcmp(gameplayMode, "progression_bundle") == 0 ||
			 std::strcmp(gameplayMode, "progression_nested") == 0 ||
			 std::strcmp(gameplayMode, "progression_resume") == 0 ||
			 std::strcmp(gameplayMode, "progression_nested_resume") == 0 ||
			 std::strcmp(gameplayMode, "progression_space") == 0 ||
			 std::strcmp(gameplayMode, "readiness_no_food") == 0 ||
			 std::strcmp(gameplayMode, "readiness_low_wealth") == 0 ||
			 std::strcmp(gameplayMode, "arbitration") == 0 ||
			 std::strcmp(gameplayMode, "arbitration_interrupt") == 0 ||
			 std::strcmp(gameplayMode, "departure") == 0 ||
			 std::strcmp(gameplayMode, "departure_recovery") == 0 ||
				 std::strcmp(gameplayMode, "spell_training") == 0 || std::strcmp(gameplayMode, "equipment_shadow") == 0 ||
				 std::strcmp(gameplayMode, "equipment_shadow_unaffordable") == 0 ||
				 std::strcmp(gameplayMode, "equipment_shadow_no_upgrade") == 0 ||
				 std::strcmp(gameplayMode, "equipment_buy") == 0 ||
				 std::strcmp(gameplayMode, "equipment_buy_resume") == 0 ||
				 std::strcmp(gameplayMode, "equipment_buy_space") == 0 ||
				 std::strcmp(gameplayMode, "equipment_buy_rejected") == 0 ||
				 std::strcmp(gameplayMode, "slotted_loot_seller") == 0 ||
				 std::strcmp(gameplayMode, "slotted_loot_no_seller") == 0 ||
			 (std::strncmp(gameplayMode, "magic_training", 14) == 0 &&
			  std::strcmp(gameplayMode, "magic_training_hunt") != 0));
		const bool startInHunt = gameplayMode &&
			(std::strcmp(gameplayMode, "navigation") == 0 || std::strcmp(gameplayMode, "navigation_recovery") == 0 ||
			 (std::strcmp(gameplayMode, "corpse") == 0 || std::strcmp(gameplayMode, "corpse_inaccessible") == 0) ||
			 std::strcmp(gameplayMode, "patrol_recovery") == 0 ||
			 (std::strcmp(gameplayMode, "target_pursuit") == 0 || std::strcmp(gameplayMode, "target_pursuit_abandon") == 0) ||
			 std::strcmp(gameplayMode, "healing") == 0 || std::strcmp(gameplayMode, "healing_resupply") == 0 ||
			 std::strcmp(gameplayMode, "value") == 0 || std::strcmp(gameplayMode, "departure_interrupt") == 0 ||
			 std::strcmp(gameplayMode, "stamina_bonus") == 0 || std::strcmp(gameplayMode, "stamina_boundary") == 0 ||
			 std::strcmp(gameplayMode, "stamina_normal") == 0 || std::strcmp(gameplayMode, "hunt_planning") == 0 ||
			 std::strcmp(gameplayMode, "adaptive_challenge") == 0 ||
			 std::strcmp(gameplayMode, "readiness_ready") == 0 || std::strcmp(gameplayMode, "readiness_upgrade") == 0 ||
			std::strcmp(gameplayMode, "readiness_missing_weapon") == 0 || std::strcmp(gameplayMode, "readiness_supplies") == 0 ||
			std::strcmp(gameplayMode, "readiness_food_capacity") == 0 ||
			std::strcmp(gameplayMode, "readiness_retention") == 0 || std::strcmp(gameplayMode, "spell_use") == 0 ||
			std::strcmp(gameplayMode, "magic_training_hunt") == 0 ||
			std::strcmp(gameplayMode, "magic_training_post_hunt") == 0 ||
			std::strcmp(gameplayMode, "magic_training_post_hunt_no_overflow") == 0 ||
			std::strcmp(gameplayMode, "spell_calibration") == 0);
		const bool adaptiveChallengeFixture = gameplayMode && std::strcmp(gameplayMode, "adaptive_challenge") == 0;
		const bool spellCalibrationFixture = gameplayMode && std::strcmp(gameplayMode, "spell_calibration") == 0;
		const bool magicTrainingFixture = gameplayMode && std::strncmp(gameplayMode, "magic_training", 14) == 0;
		const bool fixedFixtureRoute = gameplayMode && std::strcmp(gameplayMode, "stamina_bonus") != 0 &&
		                               std::strcmp(gameplayMode, "stamina_boundary") != 0 &&
			                               std::strcmp(gameplayMode, "stamina_normal") != 0 &&
			                               std::strcmp(gameplayMode, "hunt_planning") != 0 &&
			                               std::strcmp(gameplayMode, "adaptive_challenge") != 0 &&
			                               std::strcmp(gameplayMode, "equipment_shadow") != 0 &&
			                               std::strcmp(gameplayMode, "equipment_shadow_unaffordable") != 0 &&
			                               std::strcmp(gameplayMode, "equipment_shadow_no_upgrade") != 0 &&
			                               std::strcmp(gameplayMode, "mainland") != 0 &&
			                               std::strcmp(gameplayMode, "mainland_reward") != 0 &&
			                               std::strcmp(gameplayMode, "spell_training") != 0 &&
			                               std::strcmp(gameplayMode, "depot") != 0 &&
			                               std::strcmp(gameplayMode, "slotted_loot_seller") != 0 &&
			                               std::strcmp(gameplayMode, "slotted_loot_no_seller") != 0;
		const char* depotRestartPhase = std::getenv("PLAYERBOT_DEPOT_RESTART_PHASE");
		const DepotRestartCheckpoint depotRestartCheckpoint = !depotRestartPhase ? DepotRestartCheckpoint::None :
			std::strcmp(depotRestartPhase, "approach") == 0 ? DepotRestartCheckpoint::Approach :
			std::strcmp(depotRestartPhase, "locker") == 0 ? DepotRestartCheckpoint::Locker :
			std::strcmp(depotRestartPhase, "chest") == 0 ? DepotRestartCheckpoint::Chest :
			std::strcmp(depotRestartPhase, "deposit") == 0 ? DepotRestartCheckpoint::Deposit :
			std::strcmp(depotRestartPhase, "depart") == 0 ? DepotRestartCheckpoint::Depart :
			DepotRestartCheckpoint::None;
		const char* depotMoveCase = std::getenv("PLAYERBOT_DEPOT_MOVE_CASE");
		const DepotMoveFixture depotMoveFixture = depotMoveCase && std::strcmp(depotMoveCase, "partial") == 0 ?
			DepotMoveFixture::Partial : depotMoveCase && std::strcmp(depotMoveCase, "rejected") == 0 ?
			DepotMoveFixture::Rejected : DepotMoveFixture::Normal;
		return PlayerBotTestPolicy{
			!regressionMode && (!gameplayMode || progressionMode),
			startInHunt,
			fixedFixtureRoute,
			gameplayMode && std::strcmp(gameplayMode, "depot") == 0,
			depotRestartCheckpoint,
			depotMoveFixture,
			gameplayMode && std::strcmp(gameplayMode, "hunt_planning") == 0,
			gameplayMode && std::strcmp(gameplayMode, "hunt_planning") == 0,
			gameplayMode && std::strcmp(gameplayMode, "hunt_planning") == 0,
			gameplayMode && std::strcmp(gameplayMode, "navigation_recovery") == 0,
			gameplayMode && std::strcmp(gameplayMode, "corpse_inaccessible") == 0,
			gameplayMode && std::strcmp(gameplayMode, "patrol_recovery") == 0,
			gameplayMode && std::strcmp(gameplayMode, "slotted_loot_no_seller") == 0,
			!gameplayMode || (std::strcmp(gameplayMode, "equipment_shadow") != 0 &&
			                  std::strcmp(gameplayMode, "equipment_shadow_unaffordable") != 0 &&
			                  std::strcmp(gameplayMode, "equipment_shadow_no_upgrade") != 0),
			gameplayMode && std::strcmp(gameplayMode, "equipment_buy_rejected") == 0,
			adaptiveChallengeFixture,
			adaptiveChallengeFixture,
			spellCalibrationFixture,
			magicTrainingFixture,
			gameplayMode && std::strcmp(gameplayMode, "magic_training_failed") == 0,
		};
	}();
	return policy;
}

PlayerBotController::PlayerBotController(const Player& player,
                            std::map<Position, std::chrono::steady_clock::time_point>& sharedHuntRegionCooldowns,
                            const PlayerBotTestPolicy& testPolicy) :
	playerId(player.getID()), playerGuid(player.getGUID()), playerName(player.getName()), testPolicy(testPolicy),
	huntRegionCooldowns(sharedHuntRegionCooldowns)
{
	if (testPolicy.forceRepeatedNavigationStepFailures) {
		forcedNavigationStepFailuresRemaining = maximumRepeatedNavigationStepFailures;
	}
	if (testPolicy.forceCorpseNavigationFailures) {
		forcedNavigationStepFailuresRemaining = maximumCorpseNavigationFailures;
	}
	if (testPolicy.forcePatrolRouteFailures) {
		forcedNavigationPlanFailuresRemaining = maximumPatrolRouteFailures;
	}
}

void PlayerBotController::start(const Position& position, bool recovered, uint32_t recoveryCount)
{
	lastPosition = position;
	refreshItemValues();
	const bool startInHunt = !recovered && testPolicy.startInHunt;
	Player* controlledPlayer = g_game.getPlayerByID(playerId);
	const bool departureComplete = controlledPlayer && hasCompletedRookgaardDeparture(*controlledPlayer);
	const bool departureRequired = controlledPlayer && requiresRookgaardDeparture(*controlledPlayer);
	const bool useGoalSelector = controlledPlayer && !startInHunt &&
	                             (departureRequired || (!recovered && testPolicy.progressionEnabled));
	if (!testPolicy.magicTrainingFixture && useGoalSelector && !selectTopLevelGoal(*controlledPlayer, position, "startup")) {
		return;
	}
	std::ostringstream lifecycle;
	lifecycle << "\"status\":\"online\",\"message\":\"Playerbot online\""
	          << ",\"recovered\":" << (recovered ? "true" : "false")
	          << ",\"recovery_count\":" << recoveryCount
	          << ",\"objective\":" << jsonString(testPolicy.magicTrainingFixture ? "fixture_pending" : useGoalSelector ? topLevelGoalName(activeGoal) :
	                                                    (startInHunt ? "hunt" : "service"))
	          << ",\"step_speed\":" << (g_game.getPlayerByID(playerId) ? g_game.getPlayerByID(playerId)->getSpeed() : 0)
		          << ",\"spell_calibration_profiles\":" << spellCalibration.size();
	emit("lifecycle", position, lifecycle.str());
	if (testPolicy.spellCalibrationFixture && controlledPlayer) {
		runSpellCalibrationFixture(*controlledPlayer, position);
	}
	if (testPolicy.magicTrainingFixture) {
		magicTrainingFixtureInitializationPending = true;
	} else if (useGoalSelector) {
		// The selected goal initialized its own executor state.
	} else if (startInHunt) {
		activeGoal = TopLevelGoal::Hunt;
		startHunt(g_game.getPlayerByID(playerId), position, "focused_fixture");
	} else if (testPolicy.depotFixture) {
		activeGoal = TopLevelGoal::Service;
		cyclePhase = CyclePhase::ReturnToDepot;
	} else {
		activeGoal = TopLevelGoal::Service;
		cyclePhase = CyclePhase::Service;
	}
	setStage(ScenarioStage::Traverse, position);
	// Login fixtures run through Lua after controller creation. Let the real-map
	// depot fixture establish its Naji position before its first discovery pass.
	schedule(testPolicy.depotFixture ? 2000 : testPolicy.magicTrainingFixture ? 1000 : navigationInterval);
}

void PlayerBotController::schedule(uint32_t interval)
{
	std::weak_ptr<PlayerBotController> weakController = shared_from_this();
	g_scheduler.addEvent(createSchedulerTask(interval, [weakController]() {
		if (std::shared_ptr<PlayerBotController> controller = weakController.lock()) {
			controller->navigate();
		}
	}));
}

const char* PlayerBotController::stageName(ScenarioStage stage)
{
	switch (stage) {
		case ScenarioStage::LootCorpse: return "loot_corpse";
		case ScenarioStage::Traverse: return "traverse";
		case ScenarioStage::TraversalCombat: return "traversal_combat";
		case ScenarioStage::TargetPursuit: return "target_pursuit";
		case ScenarioStage::Stopped: return "stopped";
	}
	return "unknown";
}

void PlayerBotController::emit(const char* event, const Position& position, const std::string& fields) const
{
	emitPlayerbotEvent(playerName, playerGuid, event, position, fields);
}

void PlayerBotController::say(Player& player, const std::string& text) const
{
	Player* admin = g_game.getPlayerByName("GOD Admin");
	if (!admin || admin->isRemoved()) {
		return;
	}
	admin->sendTextMessage(MESSAGE_STATUS_CONSOLE_ORANGE, "[Bot One] " + text);
	admin->sendPrivateMessage(&player, TALKTYPE_PRIVATE, text);
}

bool PlayerBotController::shouldEmitRepeated(const std::string& key)
{
	const auto now = std::chrono::steady_clock::now();
	auto it = repeatedEventTimes.find(key);
	if (it != repeatedEventTimes.end() && now - it->second < repeatedEventInterval) {
		++counters.suppressedEvents;
		return false;
	}

	repeatedEventTimes[key] = now;
	return true;
}

void PlayerBotController::setStage(ScenarioStage stage, const Position& position)
{
	if (scenarioStage == stage) {
		return;
	}

	const ScenarioStage previousStage = scenarioStage;
	scenarioStage = stage;
	const std::string repeatKey = std::string("state:") + stageName(previousStage) + ':' + stageName(stage);
	if (!shouldEmitRepeated(repeatKey)) {
		return;
	}
	emit("state_transition", position, std::string("\"from\":") + jsonString(stageName(previousStage)) +
	     ",\"to\":" + jsonString(stageName(stage)));
}

void PlayerBotController::clearRatTarget(const Position& position, const char* reason)
{
	if (ratId == 0) {
		return;
	}

	const uint32_t previousTargetId = ratId;
	ratId = 0;
	if (!shouldEmitRepeated(std::string("target:clear:") + reason)) {
		return;
	}
	emit("target_changed", position, "\"previous_target_id\":" + std::to_string(previousTargetId) +
	     ",\"target_id\":null,\"reason\":" + jsonString(reason));
}

void PlayerBotController::logActionFailure(const char* action, const char* reason, const Position& position)
{
	++counters.actionsFailed;
	if (!shouldEmitRepeated(std::string("action:") + action + ':' + reason)) {
		return;
	}
	emit("action_result", position, std::string("\"action\":") + jsonString(action) +
	     ",\"result\":\"failed\",\"reason\":" + jsonString(reason));
}

uint32_t PlayerBotController::getInventoryItemCount(const Player& player, uint16_t itemId) const
{
	return static_cast<const Cylinder&>(player).getItemTypeCount(itemId);
}

uint64_t PlayerBotController::desiredCarriedGold(const Player& player) const
{
	return std::min<uint64_t>(carriedGoldReserve, player.getMoney() + player.getBankBalance());
}

bool PlayerBotController::isFoodItem(uint16_t itemId)
{
	return itemId == 2362 || (itemId >= 2666 && itemId <= 2691) || (itemId >= 2695 && itemId <= 2696) ||
	       (itemId >= 2787 && itemId <= 2796) || itemId == 5097 || itemId == 6125 ||
	       (itemId >= 6278 && itemId <= 6279) || (itemId >= 6393 && itemId <= 6394) || itemId == 6501 ||
	       (itemId >= 6541 && itemId <= 6545) || itemId == 6569 || itemId == 6574 ||
	       (itemId >= 7158 && itemId <= 7159) || (itemId >= 7372 && itemId <= 7377) ||
	       (itemId >= 7909 && itemId <= 7910) || itemId == 7963 || itemId == 8112 ||
	       (itemId >= 8838 && itemId <= 8845) || itemId == 8847 || itemId == 9005 || itemId == 9114 ||
	       itemId == 11246 || itemId == 11370 || itemId == 11429 ||
	       (itemId >= 12415 && itemId <= 12418) || (itemId >= 12637 && itemId <= 12639);
}

PlayerBotController::FoodInventory PlayerBotController::getFoodInventory(const Player& player) const
{
	uint64_t count = 0;
	uint64_t weight = 0;
	std::function<void(const Item&)> inspect = [&](const Item& item) {
		if (isFoodItem(item.getID())) {
			count += item.getItemCount();
			weight += static_cast<uint64_t>(item.getItemCount()) * item.getBaseWeight();
		}
		if (const Container* container = item.getContainer()) {
			for (const Item* child : container->getItemList()) {
				inspect(*child);
			}
		}
	};
	for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
		if (const Item* item = player.getInventoryItem(static_cast<slots_t>(slot))) {
			inspect(*item);
		}
	}
	FoodInventory food;
	food.count = static_cast<uint32_t>(std::min<uint64_t>(count, std::numeric_limits<uint32_t>::max()));
	food.weight = static_cast<uint32_t>(std::min<uint64_t>(weight, std::numeric_limits<uint32_t>::max()));
	return food;
}

uint32_t PlayerBotController::effectiveFreeCapacity(const Player& player) const
{
	return static_cast<uint32_t>(std::min<uint64_t>(
	    static_cast<uint64_t>(player.getFreeCapacity()) + getFoodInventory(player).weight,
	    std::numeric_limits<uint32_t>::max()));
}

uint32_t PlayerBotController::itemUnitValue(uint16_t itemId) const
{
	const ItemType& type = Item::items[itemId];
	if (type.worth != 0) {
		return type.worth;
	}
	auto it = itemSellValues.find(itemId);
	return it == itemSellValues.end() ? 0 : it->second;
}

uint32_t PlayerBotController::protectedItemReserve(uint16_t itemId) const
{
	if (itemId == ropeItemId || itemId == 2554) {
		return 1;
	}
	if (isFoodItem(itemId)) {
		return preferredFoodCount;
	}
	if (itemId == smallHealthPotionItemId) {
		return smallHealthPotionRestockTarget;
	}
	return 0;
}

uint32_t PlayerBotController::getBackpackSaleItemCount(const Player& player, uint16_t itemId) const
{
	if (isFoodItem(itemId)) {
		return 0;
	}
	const ItemType& type = Item::items[itemId];
	if ((type.isContainer() && type.corpseType == RACE_NONE) || type.isFluidContainer() || type.isSplash()) {
		return 0;
	}
	if ((type.slotPosition & SLOTP_TWO_HAND) != 0 && type.weaponType != WEAPON_NONE) {
		return 0;
	}
	Item* backpackItem = player.getInventoryItem(CONST_SLOT_BACKPACK);
	Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
	if (!backpack) {
		return 0;
	}
	uint32_t count = 0;
	for (Item* item : backpack->getItemList()) {
		if (item->getID() == itemId) {
			if (evaluateEquipmentUpgrade(player, *item)) {
				return 0;
			}
			const Container* container = item->getContainer();
			if (container && !container->empty()) {
				return 0;
			}
			count += item->getItemCount();
		}
	}
	uint32_t removableCount = 0;
	for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
		Item* inventoryItem = player.getInventoryItem(static_cast<slots_t>(slot));
		Container* container = inventoryItem ? inventoryItem->getContainer() : nullptr;
		if (!container) {
			continue;
		}
		for (ContainerIterator it = container->iterator(); it.hasNext(); it.advance()) {
			Item* nestedItem = *it;
			if (nestedItem->getID() == itemId) {
				if (evaluateEquipmentUpgrade(player, *nestedItem)) {
					return 0;
				}
				removableCount += nestedItem->getItemCount();
			}
		}
	}
	if (removableCount != count) {
		return 0;
	}
	const uint32_t reserve = protectedItemReserve(itemId);
	return count > reserve ? count - reserve : 0;
}

bool PlayerBotController::isItemValidForSlot(const Item& item, slots_t slot) const
{
	uint32_t slotPosition = 0;
	switch (slot) {
		case CONST_SLOT_HEAD: slotPosition = SLOTP_HEAD; break;
		case CONST_SLOT_NECKLACE: slotPosition = SLOTP_NECKLACE; break;
		case CONST_SLOT_BACKPACK: slotPosition = SLOTP_BACKPACK; break;
		case CONST_SLOT_ARMOR: slotPosition = SLOTP_ARMOR; break;
		case CONST_SLOT_RIGHT: slotPosition = SLOTP_RIGHT; break;
		case CONST_SLOT_LEFT: slotPosition = SLOTP_LEFT; break;
		case CONST_SLOT_LEGS: slotPosition = SLOTP_LEGS; break;
		case CONST_SLOT_FEET: slotPosition = SLOTP_FEET; break;
		case CONST_SLOT_RING: slotPosition = SLOTP_RING; break;
		case CONST_SLOT_AMMO: slotPosition = SLOTP_AMMO; break;
		default: return false;
	}
	return (item.getSlotPosition() & slotPosition) != 0;
}

Item* PlayerBotController::findActionableSlottedItem(const Player& player, uint16_t itemId, slots_t& slot) const
{
	const auto now = std::chrono::steady_clock::now();
	for (int32_t slotIndex = CONST_SLOT_FIRST; slotIndex <= CONST_SLOT_LAST; ++slotIndex) {
		const slots_t candidateSlot = static_cast<slots_t>(slotIndex);
		Item* item = player.getInventoryItem(candidateSlot);
		if (!item || candidateSlot == CONST_SLOT_BACKPACK || (itemId != 0 && item->getID() != itemId) ||
		    isItemValidForSlot(*item, candidateSlot) || isProtectedInventoryItem(*item) ||
		    isProtectedDepositItem(player, *item)) {
			continue;
		}
		auto suppressed = unavailableSlottedSales.find({item->getID(), candidateSlot});
		if (suppressed != unavailableSlottedSales.end() && suppressed->second > now) {
			continue;
		}
		slot = candidateSlot;
		return item;
	}
	return nullptr;
}

uint32_t PlayerBotController::getSaleItemCount(const Player& player, uint16_t itemId) const
{
	uint32_t count = getBackpackSaleItemCount(player, itemId);
	slots_t slot = CONST_SLOT_WHEREEVER;
	if (Item* slotted = findActionableSlottedItem(player, itemId, slot)) {
		count += slotted->getItemCount();
	}
	return count;
}

void PlayerBotController::logSummary(const Position& position, bool final)
{
	const auto uptimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - started).count();
	uint64_t decisionTimeUs = counters.decisionTimeUs;
	if (decisionActive) {
		decisionTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - decisionStarted).count();
	}
	std::ostringstream fields;
	const uint32_t activeTargetId = defensiveTargetId != 0 ? defensiveTargetId : ratId;
	const Position& activeTargetPosition = defensiveTargetId != 0 ? defensiveTargetPosition : ratPosition;
	fields << "\"final\":" << (final ? "true" : "false")
	       << ",\"uptime_ms\":" << uptimeMs
	       << ",\"state\":" << jsonString(stageName(scenarioStage))
	       << ",\"target_id\":";
	if (activeTargetId == 0) {
		fields << "null";
	} else {
		fields << activeTargetId
		       << ",\"target_position\":{\"x\":" << activeTargetPosition.x << ",\"y\":" << activeTargetPosition.y
		       << ",\"z\":" << static_cast<uint16_t>(activeTargetPosition.z) << '}';
	}
	fields << ",\"decisions\":" << counters.decisions
	       << ",\"decision_time_us\":" << decisionTimeUs
	       << ",\"pathfinding_calls\":" << counters.pathfindingCalls
	       << ",\"pathfinding_failures\":" << counters.pathfindingFailures
	       << ",\"pathfinding_time_us\":" << counters.pathfindingTimeUs
	       << ",\"actions_attempted\":" << counters.actionsAttempted
	       << ",\"actions_failed\":" << counters.actionsFailed
	       << ",\"stuck_events\":" << counters.stuckEvents
	       << ",\"suppressed_events\":" << counters.suppressedEvents;
	emit("summary", position, fields.str());
}

void PlayerBotController::maybeLogSummary(const Position& position)
{
	const auto now = std::chrono::steady_clock::now();
	if (now - lastSummary < summaryInterval) {
		return;
	}

	logSummary(position, false);
	lastSummary = now;
}

void PlayerBotController::stop(const char* reason, const Position& position)
{
	if (terminalLogged) {
		return;
	}

	cancelHuntRegionPlanning();
	setStage(ScenarioStage::Stopped, position);
	logSummary(position, true);
	emit("terminal", position, std::string("\"reason\":") + jsonString(reason));
	terminalLogged = true;
}

bool PlayerBotController::findPath(Player* player, const Position& target, std::vector<Direction>& result, const FindPathParams& pathParams)
{
	++counters.pathfindingCalls;
	const auto startedAt = std::chrono::steady_clock::now();
	const bool found = player->getPathTo(target, result, pathParams);
	counters.pathfindingTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - startedAt).count();
	if (!found) {
		++counters.pathfindingFailures;
	}
	return found;
}

void PlayerBotController::clearNavigation()
{
	cancelHuntRegionPlanning();
	navigationSteps.clear();
	navigationPending = false;
	worldChangePending = false;
	navigationTarget = Position();
	blockedStepCount = 0;
	fixedTargetRouteFailureCount = 0;
}

void PlayerBotController::resetPatrolRouteFailures()
{
	patrolRouteFailureCount = 0;
	patrolRouteFailureExpandedNodes = 0;
	patrolRouteFailureTarget = Position();
	patrolRouteFailureStarted = {};
}

void PlayerBotController::adoptNavigationPlan(const Position& destination, std::deque<PlayerBotNavigationStep> steps)
{
	clearNavigation();
	navigationTarget = destination;
	navigationSteps = std::move(steps);
}

void PlayerBotController::onDeath(const Player& player, const Creature* killer, const Creature* mostDamageKiller)
{
	if (deathObserved || player.getID() != playerId) {
		return;
	}
	deathObserved = true;
	lastPosition = player.getPosition();
	if (activeHuntRegion && cyclePhase == CyclePhase::Hunt &&
	    (scenarioStage == ScenarioStage::TraversalCombat || scenarioStage == ScenarioStage::TargetPursuit)) {
		huntCombatEvidence.deathObserved = true;
	}
	if (activeHuntRegion) {
		huntRegionCooldowns[activeHuntRegion->center] = std::chrono::steady_clock::now() + huntRegionCooldown;
	}
	finishHuntRegion(player, lastPosition, "death");
	std::ostringstream fields;
	fields << "\"status\":\"dead\",\"level\":" << player.getLevel()
	       << ",\"health\":" << player.getHealth() << ",\"objective\":" << jsonString(objectiveName())
	       << ",\"state\":" << jsonString(stageName(scenarioStage))
	       << ",\"target_id\":";
	const uint32_t targetId = defensiveTargetId != 0 ? defensiveTargetId : ratId;
	fields << (targetId == 0 ? "null" : std::to_string(targetId));
	fields << ",\"killer_id\":" << (killer ? std::to_string(killer->getID()) : "null")
	       << ",\"killer_name\":" << (killer ? jsonString(killer->getName()) : "null")
	       << ",\"killer_type\":" << (killer ? jsonString(killer->getPlayer() ? "player" : killer->getMonster() ? "monster" : "other") : "null")
	       << ",\"most_damage_id\":" << (mostDamageKiller ? std::to_string(mostDamageKiller->getID()) : "null")
	       << ",\"most_damage_name\":" << (mostDamageKiller ? jsonString(mostDamageKiller->getName()) : "null");
	emit("lifecycle", lastPosition, fields.str());
}

Item* PlayerBotController::findNavigationItem(const PlayerBotNavigationStep& step) const
{
	Tile* tile = g_game.map.getTile(step.target);
	if (!tile) {
		return nullptr;
	}
	if (Item* ground = tile->getGround(); ground && ground->getID() == step.itemId) {
		return ground;
	}
	if (TileItemVector* items = tile->getItemList()) {
		for (Item* item : *items) {
			if (item->getID() == step.itemId) {
				return item;
			}
		}
	}
	return nullptr;
}

bool PlayerBotController::executeNavigationStep(Player* player, const PlayerBotNavigationStep& step)
{
	++counters.actionsAttempted;
	if (step.action == PlayerBotNavigationAction::Move) {
		if (forcedNavigationStepFailuresRemaining > 0) {
			--forcedNavigationStepFailuresRemaining;
			return true;
		}
		g_game.playerMove(playerId, step.direction);
		return true;
	}

	Item* target = findNavigationItem(step);
	Tile* tile = g_game.map.getTile(step.target);
	const int32_t stackPosition = target && tile ? tile->getThingIndex(target) : -1;
	if (!target || stackPosition < 0 || stackPosition > UINT8_MAX) {
		return false;
	}

	if (step.action == PlayerBotNavigationAction::UseRope ||
	    step.action == PlayerBotNavigationAction::UseShovel) {
		const uint16_t toolId = step.action == PlayerBotNavigationAction::UseRope ? ropeItemId : 2554;
		Item* tool = g_game.findItemOfType(player, toolId, true);
		if (!tool) {
			return false;
		}
		g_game.playerUseItemEx(playerId, Position(0xFFFF, 0, 0), 0, tool->getClientID(), step.target,
		                         static_cast<uint8_t>(stackPosition), target->getClientID());
		return true;
	}

	g_game.playerUseItem(playerId, step.target, static_cast<uint8_t>(stackPosition), 0, target->getClientID());
	return true;
}

uint32_t PlayerBotController::navigationDecisionDelay(const Player& player) const
{
	const uint32_t walkDelay = static_cast<uint32_t>(std::max<int32_t>(0, player.getWalkDelay()));
	return std::max<uint32_t>(SCHEDULER_MINTICKS, std::max(walkDelay, player.getNextActionTime()));
}

void PlayerBotController::onHealthDrain(const Player& player, uint32_t damage)
{
	if (player.getID() == playerId && !pendingSpellCast.name.empty()) {
		pendingSpellCast.concurrentDamage = true;
	}
	if (player.getID() == playerId && isActiveHuntCombat(player)) {
		huntRegionDamageTaken += damage;
		huntCombatEvidence.damageTaken += damage;
	}
}

void PlayerBotController::onCombatDamage(Creature* attacker, const Creature& target, uint32_t damage)
{
	if (pendingSpellCast.name.empty() || !spellCastExecuting) {
		return;
	}
	if (!attacker || attacker->getID() != playerId) {
		if (target.getID() == pendingSpellCast.targetId) {
			pendingSpellCast.otherAttacker = true;
		}
		return;
	}
	for (uint8_t index = 0; index < pendingSpellCast.spellVictimCount; ++index) {
		if (pendingSpellCast.spellVictimIds[index] == target.getID()) {
			if (target.getID() == pendingSpellCast.targetId) {
				pendingSpellCast.observedSpellDamage = std::min<uint32_t>(maximumSpellObservationValue,
					pendingSpellCast.observedSpellDamage + damage);
			}
			return;
		}
	}
	if (pendingSpellCast.spellVictimCount < pendingSpellCast.spellVictimIds.size()) {
		pendingSpellCast.spellVictimIds[pendingSpellCast.spellVictimCount++] = target.getID();
	} else {
		pendingSpellCast.spellVictimOverflow = true;
	}
	if (target.getID() == pendingSpellCast.targetId) {
		pendingSpellCast.observedSpellDamage = std::min<uint32_t>(maximumSpellObservationValue,
			pendingSpellCast.observedSpellDamage + damage);
	}
}

void PlayerBotController::onHealthGain(Creature* healer, const Creature& target, uint32_t gain)
{
	if (pendingSpellCast.name.empty() || target.getID() != playerId || pendingSpellCast.role != "healing") {
		return;
	}
	if (healer && healer->getID() == playerId && spellCastExecuting) {
		pendingSpellCast.observedSpellHealing = std::min<uint32_t>(maximumSpellObservationValue, pendingSpellCast.observedSpellHealing + gain);
	} else {
		pendingSpellCast.otherRecovery = true;
	}
}

uint32_t PlayerBotController::navigationDistance(const Position& from, const Position& destination) const
{
	return Position::getDistanceX(from, destination) + Position::getDistanceY(from, destination) +
	       Position::getDistanceZ(from, destination) * 20;
}

bool PlayerBotController::detectNavigationOscillation(const Position& currentPosition, const Position& destination)
{
	navigationOscillationDetected = false;
	if (navigationProgressTarget != destination) {
		navigationProgressTarget = destination;
		navigationProgressPrevious = currentPosition;
		navigationProgressTwoAgo = Position();
		navigationBestDistance = navigationDistance(currentPosition, destination);
		navigationOscillationCount = 0;
		return false;
	}

	const uint32_t distance = navigationDistance(currentPosition, destination);
	if (currentPosition == navigationProgressPrevious) {
		return false;
	}
	const Position previousPosition = navigationProgressPrevious;
	const bool oscillating = navigationProgressTwoAgo != Position() && currentPosition == navigationProgressTwoAgo &&
	                         distance >= navigationBestDistance;
	if (distance < navigationBestDistance) {
		navigationBestDistance = distance;
		if (!oscillating) {
			navigationOscillationCount = 0;
		}
	}
	if (oscillating) {
		++navigationOscillationCount;
	}
	navigationProgressTwoAgo = previousPosition;
	navigationProgressPrevious = currentPosition;
	if (navigationOscillationCount < 3) {
		return false;
	}

	Position blockedTarget = currentPosition;
	Position blockedExpected = currentPosition;
	for (const PlayerBotNavigationStep& step : navigationSteps) {
		if (step.action != PlayerBotNavigationAction::Move || step.expectedPosition.z != currentPosition.z) {
			blockedTarget = step.target;
			blockedExpected = step.expectedPosition;
			break;
		}
	}
	const auto expires = std::chrono::steady_clock::now() + navigationOscillationSuppression;
	temporarilyBlockedPositions[blockedTarget] = expires;
	temporarilyBlockedPositions[blockedExpected] = expires;
	std::ostringstream fields;
	fields << "\"result\":\"suppressed\",\"reason\":\"position_oscillation\""
	       << ",\"destination\":{\"x\":" << destination.x << ",\"y\":" << destination.y
	       << ",\"z\":" << static_cast<uint16_t>(destination.z) << '}'
	       << ",\"blocked_target\":{\"x\":" << blockedTarget.x << ",\"y\":" << blockedTarget.y
	       << ",\"z\":" << static_cast<uint16_t>(blockedTarget.z) << '}'
	       << ",\"position_a\":{\"x\":" << currentPosition.x << ",\"y\":" << currentPosition.y
	       << ",\"z\":" << static_cast<uint16_t>(currentPosition.z) << '}'
	       << ",\"position_b\":{\"x\":" << previousPosition.x
	       << ",\"y\":" << previousPosition.y
	       << ",\"z\":" << static_cast<uint16_t>(previousPosition.z) << '}';
	emit("navigation_progress", currentPosition, fields.str());
	++counters.stuckEvents;
	navigationSteps.clear();
	navigationPending = false;
	worldChangePending = false;
	navigationOscillationCount = 0;
	navigationOscillationDetected = true;
	schedule(blockedRouteRetryInterval);
	return true;
}

bool PlayerBotController::processNavigation(Player* player, const Position& currentPosition, const Position& destination)
{
	lastNavigationRouteUnavailable = false;
	lastNavigationExpandedNodes = 0;
	if (currentPosition == destination) {
		clearNavigation();
		return true;
	}
	if (detectNavigationOscillation(currentPosition, destination)) {
		return false;
	}

	if (navigationPending) {
		if (currentPosition == navigationExpectedPosition) {
			navigationPending = false;
			blockedStepCount = 0;
			if (!navigationSteps.empty()) {
				navigationSteps.pop_front();
			}
		} else if ((player->getWalkDelay() > 0 || !player->canDoAction()) &&
		           std::chrono::steady_clock::now() - navigationStepStarted < navigationStepTimeout) {
			schedule(navigationDecisionDelay(*player));
			return false;
		} else {
			navigationPending = false;
			navigationSteps.clear();
			blockedNavigationTarget = navigationStepTarget;
			blockedNavigationTargetExpires = std::chrono::steady_clock::now() + navigationBlockSuppression;
			temporarilyBlockedPositions[navigationStepTarget] =
				std::chrono::steady_clock::now() + navigationBlockSuppression;
			logActionFailure("navigate", "step_result_mismatch", currentPosition);
			++blockedStepCount;
			if (blockedStepCount >= maximumRepeatedNavigationStepFailures) {
				schedule(blockedRouteRetryInterval);
				return false;
			}
		}
	}
	if (worldChangePending) {
		const PlayerBotNavigationStep pendingStep = worldChangeStep;
		worldChangePending = false;
		if (Item* unchanged = findNavigationItem(pendingStep)) {
			temporarilyBlockedPositions[pendingStep.target] =
				std::chrono::steady_clock::now() + navigationBlockSuppression;
			logActionFailure("navigate", "transition_state_unchanged", currentPosition);
		}
	}

	if (navigationTarget != destination) {
		navigationSteps.clear();
		navigationTarget = destination;
		blockedStepCount = 0;
	}
	if (navigationSteps.empty()) {
		const auto now = std::chrono::steady_clock::now();
		for (auto it = temporarilyBlockedPositions.begin(); it != temporarilyBlockedPositions.end();) {
			if (it->second <= now) {
				it = temporarilyBlockedPositions.erase(it);
			} else {
				++it;
			}
		}
		std::set<Position> blockedPositions;
		for (const auto& blocked : temporarilyBlockedPositions) {
			blockedPositions.insert(blocked.first);
		}
		uint64_t expandedNodes = 0;
		++counters.pathfindingCalls;
		const auto startedAt = std::chrono::steady_clock::now();
		PlayerBotNavigationResult planResult;
		if (forcedNavigationPlanFailuresRemaining != 0) {
			--forcedNavigationPlanFailuresRemaining;
			expandedNodes = playerBotNavigationMaximumExpandedNodes;
			planResult = PlayerBotNavigationResult::Unreachable;
		} else {
			planResult = navigator.plan(*player, destination, blockedPositions, navigationSteps, expandedNodes);
		}
		const bool planned = planResult == PlayerBotNavigationResult::Reached;
		counters.pathfindingTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - startedAt).count();
		if (!planned || navigationSteps.empty()) {
			lastNavigationRouteUnavailable = true;
			lastNavigationExpandedNodes = expandedNodes;
			++counters.pathfindingFailures;
			emit("navigation_progress", currentPosition,
			     "\"result\":\"failed\",\"reason\":\"route_unavailable\",\"cycle_phase\":" +
			         jsonString(cyclePhaseName()) + ",\"destination\":{\"x\":" + std::to_string(destination.x) +
			         ",\"y\":" + std::to_string(destination.y) + ",\"z\":" +
			         std::to_string(static_cast<uint16_t>(destination.z)) + "},\"plan_result\":" +
			         std::to_string(static_cast<uint16_t>(planResult)) + ",\"expanded_nodes\":" +
			         std::to_string(expandedNodes));
			logActionFailure("navigate", "route_unavailable", currentPosition);
			if (blockedPositions.empty() && ++fixedTargetRouteFailureCount >= 20) {
				stop("navigation_route_unavailable", currentPosition);
			}
			schedule(blockedRouteRetryInterval);
			return false;
		}
		fixedTargetRouteFailureCount = 0;
		std::ostringstream fields;
		fields << "\"action\":\"plan\",\"result\":\"success\",\"steps\":" << navigationSteps.size()
		       << ",\"expanded_nodes\":" << expandedNodes << ",\"destination\":{\"x\":" << destination.x
		       << ",\"y\":" << destination.y << ",\"z\":" << static_cast<uint16_t>(destination.z) << '}';
		emit("action_result", currentPosition, fields.str());
	}

	if (!player->canDoAction() || navigationSteps.empty()) {
		schedule(navigationDecisionDelay(*player));
		return false;
	}

	const PlayerBotNavigationStep& step = navigationSteps.front();
	if (!executeNavigationStep(player, step)) {
		navigationSteps.clear();
		logActionFailure("navigate", "transition_unavailable", currentPosition);
		schedule(blockedRouteRetryInterval);
		return false;
	}

	if (step.action == PlayerBotNavigationAction::UseDoor ||
	    step.action == PlayerBotNavigationAction::UseShovel) {
		worldChangeStep = step;
		worldChangePending = true;
		navigationSteps.clear();
		schedule(navigationDecisionDelay(*player));
		return false;
	}
	navigationExpectedPosition = step.expectedPosition;
	navigationStepTarget = step.target;
	navigationStepStarted = std::chrono::steady_clock::now();
	navigationPending = true;
	schedule(navigationDecisionDelay(*player));
	return false;
}

void PlayerBotController::navigate()
{
	DecisionTimer decisionTimer(*this);
	if (g_game.getGameState() == GAME_STATE_SHUTDOWN) {
		return;
	}
	if (deathObserved) {
		stop("controlled_player_dead", lastPosition);
		return;
	}
	Player* player = g_game.getPlayerByID(playerId);
	if (!player) {
		stop("controlled_player_not_found", lastPosition);
		return;
	}
	if (!player->isPlayerBot()) {
		stop("controlled_player_ownership_lost", lastPosition);
		return;
	}
	if (player->isRemoved()) {
		emit("lifecycle", lastPosition, "\"status\":\"removed\"");
		stop("controlled_player_removed", lastPosition);
		return;
	}
	if (player->isDead()) {
		onDeath(*player, nullptr, nullptr);
		stop("controlled_player_dead", player->getPosition());
		return;
	}

	const Position currentPosition = player->getPosition();
	lastPosition = currentPosition;
	if (magicTrainingFixtureInitializationPending) {
		magicTrainingFixtureInitializationPending = false;
		runMagicTrainingFixture(*player, currentPosition);
		const bool useGoalSelector = !testPolicy.startInHunt &&
		                             (requiresRookgaardDeparture(*player) || testPolicy.progressionEnabled);
		if (useGoalSelector) {
			if (!selectTopLevelGoal(*player, currentPosition, "startup")) {
				return;
			}
		} else if (testPolicy.startInHunt) {
			activeGoal = TopLevelGoal::Hunt;
			startHunt(player, currentPosition, "focused_fixture");
		} else {
			activeGoal = TopLevelGoal::Service;
			cyclePhase = CyclePhase::Service;
		}
		schedule(navigationInterval);
		return;
	}
	maybeLogSummary(currentPosition);
	recordActiveHuntCombat(*player);
	verifySpellCast(*player, currentPosition);
	if (activeHuntRegion && cyclePhase == CyclePhase::Hunt) {
		if (huntRegionDamageTaken >= static_cast<uint32_t>(player->getMaxHealth()) &&
		    std::chrono::steady_clock::now() - huntRegionStarted < std::chrono::minutes(2)) {
			huntCombatEvidence.dangerObserved = true;
			huntRegionCooldowns[activeHuntRegion->center] = std::chrono::steady_clock::now() + huntRegionCooldown;
			beginService(player, currentPosition, "hunt_region_observed_danger");
			schedule(navigationInterval);
			return;
		}
	}
	const bool accessingReward = progressionObjective == ProgressionObjective::PickupReward &&
	                             (progressionStage == ProgressionStage::VerifyReward ||
	                              progressionStage == ProgressionStage::EquipReward ||
	                              progressionStage == ProgressionStage::VerifyEquipment);
	const bool verifyingDeparture = progressionObjective == ProgressionObjective::OracleDeparture &&
	                                departureStage == DepartureStage::Verify;
	if (!accessingReward && !verifyingDeparture && handleHealing(player, currentPosition)) {
		schedule(blockedRouteRetryInterval);
		return;
	}
	const bool waitingForRecovery = cyclePhase == CyclePhase::Service && needsHealing(*player);
	if (!accessingReward && progressionObjective != ProgressionObjective::OracleDeparture &&
	    requiresRookgaardDeparture(*player) && !waitingForRecovery) {
		if (selectTopLevelGoal(*player, currentPosition, "level_eight_interrupt")) {
			schedule(SCHEDULER_MINTICKS);
		}
		return;
	}
	if (!accessingReward && !verifyingDeparture && handleFood(player, currentPosition)) {
		schedule(blockedRouteRetryInterval);
		return;
	}
	if (scenarioStage == ScenarioStage::Stopped) {
		return;
	}
	processTraversal(player, currentPosition);
}
