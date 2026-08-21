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
			 std::strcmp(gameplayMode, "mainland_reward") == 0 ||
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
			gameplayMode && std::strncmp(gameplayMode, "equipment_buy", 13) == 0,
			gameplayMode && std::strcmp(gameplayMode, "equipment_buy_rejected") == 0,
			gameplayMode && std::strcmp(gameplayMode, "equipment_buy_space") == 0,
			adaptiveChallengeFixture,
			adaptiveChallengeFixture,
			gameplayMode && (std::strcmp(gameplayMode, "mainland_reward") == 0 ||
			                 std::strcmp(gameplayMode, "spell_training") == 0 ||
			                 std::strncmp(gameplayMode, "equipment_buy", 13) == 0),
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
	inventoryPolicy(itemSellValues, [this](const Player& candidatePlayer, const Item& item) {
		return evaluateEquipmentUpgrade(candidatePlayer, item).has_value();
	}), huntRegionCooldowns(sharedHuntRegionCooldowns)
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
	if (!testPolicy.magicTrainingFixture && !testPolicy.deferProgressionFixtureInitialization && useGoalSelector &&
	    !selectTopLevelGoal(*controlledPlayer, position, "startup")) {
		return;
	}
	std::ostringstream lifecycle;
	lifecycle << "\"status\":\"online\",\"message\":\"Playerbot online\""
	          << ",\"recovered\":" << (recovered ? "true" : "false")
	          << ",\"recovery_count\":" << recoveryCount
	          << ",\"objective\":" << jsonString((testPolicy.magicTrainingFixture || testPolicy.deferProgressionFixtureInitialization) ? "fixture_pending" : useGoalSelector ? topLevelGoalName(activeGoal) :
	                                                    (startInHunt ? "hunt" : "service"))
	          << ",\"step_speed\":" << (g_game.getPlayerByID(playerId) ? g_game.getPlayerByID(playerId)->getSpeed() : 0)
		          << ",\"spell_calibration_profiles\":" << spellCalibration.size();
	emit("lifecycle", position, lifecycle.str());
	if (testPolicy.spellCalibrationFixture && controlledPlayer) {
		runSpellCalibrationFixture(*controlledPlayer, position);
	}
	if (testPolicy.magicTrainingFixture || testPolicy.deferProgressionFixtureInitialization) {
		fixtureInitializationPending = true;
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

std::optional<PlayerBotTraversalTarget> PlayerBotController::clearTraversalTarget(const Position& position, const char* reason)
{
	const auto target = targetingSession.clearTraversalTarget();
	if (!target) {
		return std::nullopt;
	}

	if (!shouldEmitRepeated(std::string("target:clear:") + reason)) {
		return target;
	}
	emit("target_changed", position, "\"previous_target_id\":" + std::to_string(target->id) +
	     ",\"target_id\":null,\"reason\":" + jsonString(reason));
	return target;
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

Item* PlayerBotController::findActionableSlottedItem(const Player& player, uint16_t itemId, slots_t& slot) const
{
	const auto now = std::chrono::steady_clock::now();
	for (int32_t slotIndex = CONST_SLOT_FIRST; slotIndex <= CONST_SLOT_LAST; ++slotIndex) {
		const slots_t candidateSlot = static_cast<slots_t>(slotIndex);
		Item* item = player.getInventoryItem(candidateSlot);
		if (!item || !inventoryPolicy.isActionableSlottedItem(player, *item, candidateSlot, itemId)) {
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
	uint32_t count = inventoryPolicy.backpackSaleItemCount(player, itemId);
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
	const auto activeTarget = targetingSession.activeTarget();
	fields << "\"final\":" << (final ? "true" : "false")
	       << ",\"uptime_ms\":" << uptimeMs
	       << ",\"state\":" << jsonString(stageName(scenarioStage))
	       << ",\"target_id\":";
	if (!activeTarget) {
		fields << "null";
	} else {
		fields << activeTarget->id
		       << ",\"target_position\":{\"x\":" << activeTarget->position.x << ",\"y\":" << activeTarget->position.y
		       << ",\"z\":" << static_cast<uint16_t>(activeTarget->position.z) << '}';
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
	navigationSession.clear();
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
	navigationSession.adopt(destination, std::move(steps));
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
	const auto activeTarget = targetingSession.activeTarget();
	fields << (activeTarget ? std::to_string(activeTarget->id) : "null");
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
	spellRuntime.observeHealthDrain(player.getID() == playerId);
	if (player.getID() == playerId && isActiveHuntCombat(player)) {
		huntRegionDamageTaken += damage;
		huntCombatEvidence.damageTaken += damage;
	}
}

void PlayerBotController::onCombatDamage(Creature* attacker, const Creature& target, uint32_t damage)
{
	spellRuntime.observeCombatDamage(attacker ? attacker->getID() : 0, target.getID(), playerId, damage);
}

void PlayerBotController::onHealthGain(Creature* healer, const Creature& target, uint32_t gain)
{
	spellRuntime.observeHealthGain(healer && healer->getID() == playerId, target.getID() == playerId, gain);
}

bool PlayerBotController::processNavigation(Player* player, const Position& currentPosition, const Position& destination)
{
	lastNavigationRouteUnavailable = false;
	lastNavigationExpandedNodes = 0;
	if (currentPosition == destination) {
		clearNavigation();
		return true;
	}
	const auto now = std::chrono::steady_clock::now();
	if (const auto oscillation = navigationSession.observeProgress(
			currentPosition, destination, now, navigationOscillationSuppression)) {
		std::ostringstream fields;
		fields << "\"result\":\"suppressed\",\"reason\":\"position_oscillation\""
		       << ",\"destination\":{\"x\":" << destination.x << ",\"y\":" << destination.y
		       << ",\"z\":" << static_cast<uint16_t>(destination.z) << '}'
		       << ",\"blocked_target\":{\"x\":" << oscillation->blockedTarget.x
		       << ",\"y\":" << oscillation->blockedTarget.y
		       << ",\"z\":" << static_cast<uint16_t>(oscillation->blockedTarget.z) << '}'
		       << ",\"position_a\":{\"x\":" << currentPosition.x << ",\"y\":" << currentPosition.y
		       << ",\"z\":" << static_cast<uint16_t>(currentPosition.z) << '}'
		       << ",\"position_b\":{\"x\":" << oscillation->previousPosition.x
		       << ",\"y\":" << oscillation->previousPosition.y
		       << ",\"z\":" << static_cast<uint16_t>(oscillation->previousPosition.z) << '}';
		emit("navigation_progress", currentPosition, fields.str());
		++counters.stuckEvents;
		schedule(blockedRouteRetryInterval);
		return false;
	}

	const PlayerBotPendingMovementResult movementResult = navigationSession.observeMovement(
		currentPosition, player->getWalkDelay() > 0 || !player->canDoAction(), now,
		navigationStepTimeout, navigationBlockSuppression);
	if (movementResult == PlayerBotPendingMovementResult::Waiting) {
		schedule(navigationDecisionDelay(*player));
		return false;
	}
	if (movementResult == PlayerBotPendingMovementResult::Mismatch) {
		logActionFailure("navigate", "step_result_mismatch", currentPosition);
		if (navigationSession.stepFailureCount() >= maximumRepeatedNavigationStepFailures) {
			schedule(blockedRouteRetryInterval);
			return false;
		}
	}
	if (const auto pendingStep = navigationSession.takeWorldChange()) {
		if (Item* unchanged = findNavigationItem(*pendingStep)) {
			navigationSession.suppress(pendingStep->target, now + navigationBlockSuppression);
			logActionFailure("navigate", "transition_state_unchanged", currentPosition);
		}
	}

	navigationSession.prepareDestination(destination);
	if (navigationSession.routeEmpty()) {
		const std::set<Position> blockedPositions = navigationSession.activeBlockedPositions(now);
		std::deque<PlayerBotNavigationStep> plannedSteps;
		uint64_t expandedNodes = 0;
		++counters.pathfindingCalls;
		const auto startedAt = std::chrono::steady_clock::now();
		PlayerBotNavigationResult planResult;
		if (forcedNavigationPlanFailuresRemaining != 0) {
			--forcedNavigationPlanFailuresRemaining;
			expandedNodes = playerBotNavigationMaximumExpandedNodes;
			planResult = PlayerBotNavigationResult::Unreachable;
		} else {
			planResult = navigator.plan(*player, destination, blockedPositions, plannedSteps, expandedNodes);
		}
		const bool planned = planResult == PlayerBotNavigationResult::Reached;
		counters.pathfindingTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - startedAt).count();
		if (!planned || plannedSteps.empty()) {
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
		fields << "\"action\":\"plan\",\"result\":\"success\",\"steps\":" << plannedSteps.size()
		       << ",\"expanded_nodes\":" << expandedNodes << ",\"destination\":{\"x\":" << destination.x
		       << ",\"y\":" << destination.y << ",\"z\":" << static_cast<uint16_t>(destination.z) << '}';
		emit("action_result", currentPosition, fields.str());
		navigationSession.installRoute(destination, std::move(plannedSteps));
	}

	if (!player->canDoAction() || navigationSession.routeEmpty()) {
		schedule(navigationDecisionDelay(*player));
		return false;
	}

	const PlayerBotNavigationStep& step = navigationSession.nextStep();
	if (!executeNavigationStep(player, step)) {
		navigationSession.clearRoute();
		logActionFailure("navigate", "transition_unavailable", currentPosition);
		schedule(blockedRouteRetryInterval);
		return false;
	}

	if (step.action == PlayerBotNavigationAction::UseDoor ||
	    step.action == PlayerBotNavigationAction::UseShovel) {
		navigationSession.beginWorldChange(step);
		schedule(navigationDecisionDelay(*player));
		return false;
	}
	navigationSession.beginMovement(step, std::chrono::steady_clock::now());
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
	if (fixtureInitializationPending) {
		int32_t fixtureReady = -1;
		if (testPolicy.deferProgressionFixtureInitialization) {
			player->getStorageValue(gameplayFixtureReadyStorage, fixtureReady);
			if (fixtureReady == 2) {
				fixtureInitializationPending = false;
				return;
			}
			if (fixtureReady != 1) {
				schedule(navigationInterval);
				return;
			}
		}
		fixtureInitializationPending = false;
		if (testPolicy.magicTrainingFixture) {
			runMagicTrainingFixture(*player, currentPosition);
		}
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
