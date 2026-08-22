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

PlayerBotController::PlayerBotController(const Player& player,
	                            std::map<Position, std::chrono::steady_clock::time_point>& sharedHuntRegionCooldowns) :
	playerId(player.getID()), playerGuid(player.getGUID()), playerName(player.getName()), fixtureDriver(playerBotTestPolicyFromEnvironment()),
	telemetry(player.getName(), player.getGUID()),
	equipmentPolicy(oracleVocationId),
	inventoryPolicy(economyCatalog.sellValues(), [this](const Player& candidatePlayer, const Item& item) {
		return equipmentPolicy.evaluateUpgrade(candidatePlayer, item).has_value();
	}), huntRuntime(sharedHuntRegionCooldowns, {huntingLoop.begin(), huntingLoop.end()})
{}

void PlayerBotController::start(const Position& position, bool recovered, uint32_t recoveryCount)
{
	lastPosition = position;
	refreshItemValues();
	const bool startInHunt = !recovered && fixtureDriver.startInHunt();
	Player* controlledPlayer = g_game.getPlayerByID(playerId);
	const bool departureComplete = controlledPlayer && departurePlanner.hasCompleted(departureSnapshot(*controlledPlayer));
	const bool departureRequired = controlledPlayer && departurePlanner.required(departureSnapshot(*controlledPlayer));
	const bool useGoalSelector = controlledPlayer && !startInHunt &&
	                             (departureRequired || (!recovered && fixtureDriver.goalLoop(true).selectGoal));
	if (!fixtureDriver.magicTrainingScenario() && !fixtureDriver.deferInitialization() && useGoalSelector &&
	    !selectTopLevelGoal(*controlledPlayer, position, "startup")) {
		return;
	}
	std::ostringstream lifecycle;
	lifecycle << "\"status\":\"online\",\"message\":\"Playerbot online\""
	          << ",\"recovered\":" << (recovered ? "true" : "false")
	          << ",\"recovery_count\":" << recoveryCount
	          << ",\"objective\":" << jsonString((fixtureDriver.magicTrainingScenario() || fixtureDriver.deferInitialization()) ? "fixture_pending" : useGoalSelector ? PlayerBotGoalArbiter::goalName(goalArbiter.activeGoal()) :
	                                                    (startInHunt ? "hunt" : "service"))
	          << ",\"step_speed\":" << (g_game.getPlayerByID(playerId) ? g_game.getPlayerByID(playerId)->getSpeed() : 0)
		          << ",\"spell_calibration_profiles\":" << survivalRuntime.calibrationSize();
	telemetry.emit("lifecycle", position, lifecycle.str());
	if (controlledPlayer) {
		emitFixtureEvents(fixtureDriver.runSpellCalibration(*controlledPlayer, survivalRuntime), position);
		emitFixtureEvents(fixtureDriver.runAdaptiveChallenge(*controlledPlayer, huntRuntime), position);
	}
	if (fixtureDriver.magicTrainingScenario() || fixtureDriver.deferInitialization()) {
		fixtureDriver.beginDelayedInitialization();
	} else if (useGoalSelector) {
		// The selected goal initialized its own executor state.
	} else if (startInHunt) {
		progressionRuntime.setActiveGoal(TopLevelGoal::Hunt);
		startHunt(g_game.getPlayerByID(playerId), position, "focused_fixture");
	} else if (fixtureDriver.depotScenario()) {
		progressionRuntime.setActiveGoal(TopLevelGoal::Service);
		cyclePhase = CyclePhase::ReturnToDepot;
	} else {
		progressionRuntime.setActiveGoal(TopLevelGoal::Service);
		cyclePhase = CyclePhase::Service;
	}
	setStage(ScenarioStage::Traverse, position);
	// Login fixtures run through Lua after controller creation. Let the real-map
	// depot fixture establish its Naji position before its first discovery pass.
	schedule(fixtureDriver.depotScenario() ? 2000 : fixtureDriver.magicTrainingScenario() ? 1000 : navigationInterval);
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

void PlayerBotController::say(Player& player, const std::string& text) const
{
	Player* admin = g_game.getPlayerByName("GOD Admin");
	if (!admin || admin->isRemoved()) {
		return;
	}
	admin->sendTextMessage(MESSAGE_STATUS_CONSOLE_ORANGE, "[Bot One] " + text);
	admin->sendPrivateMessage(&player, TALKTYPE_PRIVATE, text);
}

void PlayerBotController::setStage(ScenarioStage stage, const Position& position)
{
	if (scenarioStage == stage) {
		return;
	}

	const ScenarioStage previousStage = scenarioStage;
	scenarioStage = stage;
	const std::string repeatKey = std::string("state:") + stageName(previousStage) + ':' + stageName(stage);
	if (!telemetry.shouldEmitRepeated(repeatKey)) {
		return;
	}
	telemetry.emit("state_transition", position, std::string("\"from\":") + jsonString(stageName(previousStage)) +
	     ",\"to\":" + jsonString(stageName(stage)));
}

std::optional<PlayerBotTraversalTarget> PlayerBotController::clearTraversalTarget(const Position& position, const char* reason)
{
	const auto target = combatRuntime.clearTraversalTarget();
	if (!target) {
		return std::nullopt;
	}

	if (!telemetry.shouldEmitRepeated(std::string("target:clear:") + reason)) {
		return target;
	}
	telemetry.emit("target_changed", position, "\"previous_target_id\":" + std::to_string(target->id) +
	     ",\"target_id\":null,\"reason\":" + jsonString(reason));
	return target;
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
		if (serviceWorkflow.slottedSaleUnavailable(item->getID(), candidateSlot, now)) {
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

playerbot::PlayerBotTelemetrySummary PlayerBotController::telemetrySummary() const
{
	playerbot::PlayerBotTelemetrySummary summary{stageName(scenarioStage), std::nullopt};
	if (const auto activeTarget = combatRuntime.activeTarget()) {
		summary.target = playerbot::PlayerBotTelemetryTarget{activeTarget->id, activeTarget->position};
	}
	return summary;
}

void PlayerBotController::emitFixtureEvents(const std::vector<playerbot::PlayerBotFixtureEvent>& events, const Position& position) const
{
	for (const auto& event : events) emit(event.name, position, event.fields);
}

void PlayerBotController::stop(const char* reason, const Position& position)
{
	if (telemetry.terminalLogged()) {
		return;
	}

	huntRuntime.cancelPlanning();
	setStage(ScenarioStage::Stopped, position);
	telemetry.emitTerminal(reason, position, telemetrySummary());
}

bool PlayerBotController::findPath(Player* player, const Position& target, std::vector<Direction>& result, const FindPathParams& pathParams)
{
	const auto startedAt = std::chrono::steady_clock::now();
	const bool found = player->getPathTo(target, result, pathParams);
	telemetry.recordPathfinding(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - startedAt), found);
	return found;
}

void PlayerBotController::clearNavigation()
{
	huntRuntime.cancelPlanning();
	navigationRuntime.clear();
}

void PlayerBotController::adoptNavigationPlan(const Position& destination, std::deque<PlayerBotNavigationStep> steps)
{
	clearNavigation();
	navigationRuntime.adopt(destination, std::move(steps));
}

void PlayerBotController::onDeath(const Player& player, const Creature* killer, const Creature* mostDamageKiller)
{
	if (deathObserved || player.getID() != playerId) {
		return;
	}
	deathObserved = true;
	lastPosition = player.getPosition();
	if (huntRuntime.active() && cyclePhase == CyclePhase::Hunt &&
	    (scenarioStage == ScenarioStage::TraversalCombat || scenarioStage == ScenarioStage::TargetPursuit)) {
		huntRuntime.observeDeath(true, std::chrono::steady_clock::now(), huntRegionCooldown);
	} else {
		huntRuntime.observeDeath(false, std::chrono::steady_clock::now(), huntRegionCooldown);
	}
	finishHuntRegion(player, lastPosition, "death");
	std::ostringstream fields;
	fields << "\"status\":\"dead\",\"level\":" << player.getLevel()
	       << ",\"health\":" << player.getHealth() << ",\"objective\":" << jsonString(objectiveName())
	       << ",\"state\":" << jsonString(stageName(scenarioStage))
	       << ",\"target_id\":";
	const auto activeTarget = combatRuntime.activeTarget();
	fields << (activeTarget ? std::to_string(activeTarget->id) : "null");
	fields << ",\"killer_id\":" << (killer ? std::to_string(killer->getID()) : "null")
	       << ",\"killer_name\":" << (killer ? jsonString(killer->getName()) : "null")
	       << ",\"killer_type\":" << (killer ? jsonString(killer->getPlayer() ? "player" : killer->getMonster() ? "monster" : "other") : "null")
	       << ",\"most_damage_id\":" << (mostDamageKiller ? std::to_string(mostDamageKiller->getID()) : "null")
	       << ",\"most_damage_name\":" << (mostDamageKiller ? jsonString(mostDamageKiller->getName()) : "null");
	telemetry.emit("lifecycle", lastPosition, fields.str());
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
	telemetry.recordActionAttempt();
	if (step.action == PlayerBotNavigationAction::Move) {
		if (!fixtureDriver.navigationStepCommand().dispatch) {
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
	survivalRuntime.observeHealthDrain(player.getID() == playerId);
	if (player.getID() == playerId && isActiveHuntCombat(player)) {
		huntRuntime.observeDamage(damage);
	}
}

void PlayerBotController::onCombatDamage(Creature* attacker, const Creature& target, uint32_t damage)
{
	survivalRuntime.observeCombatDamage(attacker ? attacker->getID() : 0, target.getID(), playerId, damage);
}

void PlayerBotController::onHealthGain(Creature* healer, const Creature& target, uint32_t gain)
{
	survivalRuntime.observeHealthGain(healer && healer->getID() == playerId, target.getID() == playerId, gain);
}

bool PlayerBotController::processNavigation(Player* player, const Position& currentPosition, const Position& destination,
                                            PlayerBotNavigationRuntimeOutcome* navigationOutcome)
{
	const auto now = std::chrono::steady_clock::now();
	const PlayerBotFixtureRoutePlan fixturePlan = fixtureDriver.navigationPlan(playerBotNavigationMaximumExpandedNodes);
	const PlayerBotNavigationRuntimeOutcome outcome = navigationRuntime.process({
		*player, currentPosition, destination, player->getWalkDelay() > 0 || !player->canDoAction(), player->canDoAction(),
		fixturePlan.forceFailure,
		{now, navigationStepTimeout, navigationBlockSuppression, navigationOscillationSuppression},
	});
	if (navigationOutcome) *navigationOutcome = outcome;
	fixtureDriver.observeNavigationPlan(outcome.plan.attempted);
	if (outcome.destinationReached) {
		clearNavigation();
		return true;
	}
	if (outcome.oscillation) {
		const PlayerBotNavigationOscillation& oscillation = *outcome.oscillation;
		std::ostringstream fields;
		fields << "\"result\":\"suppressed\",\"reason\":\"position_oscillation\""
		       << ",\"destination\":{\"x\":" << destination.x << ",\"y\":" << destination.y
		       << ",\"z\":" << static_cast<uint16_t>(destination.z) << '}'
		       << ",\"blocked_target\":{\"x\":" << oscillation.blockedTarget.x
		       << ",\"y\":" << oscillation.blockedTarget.y
		       << ",\"z\":" << static_cast<uint16_t>(oscillation.blockedTarget.z) << '}'
		       << ",\"position_a\":{\"x\":" << currentPosition.x << ",\"y\":" << currentPosition.y
		       << ",\"z\":" << static_cast<uint16_t>(currentPosition.z) << '}'
		       << ",\"position_b\":{\"x\":" << oscillation.previousPosition.x
		       << ",\"y\":" << oscillation.previousPosition.y
		       << ",\"z\":" << static_cast<uint16_t>(oscillation.previousPosition.z) << '}';
		telemetry.emit("navigation_progress", currentPosition, fields.str());
		telemetry.recordStuckEvent();
		schedule(blockedRouteRetryInterval);
		return false;
	}

	if (outcome.movementResult == PlayerBotPendingMovementResult::Waiting) {
		schedule(navigationDecisionDelay(*player));
		return false;
	}
	if (outcome.movementResult == PlayerBotPendingMovementResult::Mismatch) {
		telemetry.logActionFailure("navigate", "step_result_mismatch", currentPosition);
		if (outcome.stepFailureCount >= maximumRepeatedNavigationStepFailures) {
			schedule(blockedRouteRetryInterval);
			return false;
		}
	}
	if (outcome.pendingWorldChange) {
		if (Item* unchanged = findNavigationItem(*outcome.pendingWorldChange)) {
			navigationRuntime.suppress(outcome.pendingWorldChange->target, now + navigationBlockSuppression);
			telemetry.logActionFailure("navigate", "transition_state_unchanged", currentPosition);
		}
	}
	if (outcome.plan.attempted) {
		telemetry.recordPathfinding(outcome.plan.elapsed, !outcome.routeUnavailable);
		if (outcome.routeUnavailable) {
			telemetry.emit("navigation_progress", currentPosition,
			     "\"result\":\"failed\",\"reason\":\"route_unavailable\",\"cycle_phase\":" +
			         jsonString(cyclePhaseName()) + ",\"destination\":{\"x\":" + std::to_string(destination.x) +
			         ",\"y\":" + std::to_string(destination.y) + ",\"z\":" +
			         std::to_string(static_cast<uint16_t>(destination.z)) + "},\"plan_result\":" +
			         std::to_string(static_cast<uint16_t>(outcome.plan.result)) + ",\"expanded_nodes\":" +
			         std::to_string(outcome.plan.expandedNodes));
			telemetry.logActionFailure("navigate", "route_unavailable", currentPosition);
			if (outcome.fixedTargetRouteExhausted) {
				stop("navigation_route_unavailable", currentPosition);
			}
			schedule(blockedRouteRetryInterval);
			return false;
		}
		std::ostringstream fields;
		fields << "\"action\":\"plan\",\"result\":\"success\",\"steps\":" << outcome.plan.steps
		       << ",\"expanded_nodes\":" << outcome.plan.expandedNodes << ",\"destination\":{\"x\":" << destination.x
		       << ",\"y\":" << destination.y << ",\"z\":" << static_cast<uint16_t>(destination.z) << '}';
		telemetry.emit("action_result", currentPosition, fields.str());
	}

	if (!outcome.nextStep) {
		schedule(navigationDecisionDelay(*player));
		return false;
	}

	const PlayerBotNavigationStep& step = *outcome.nextStep;
	if (!executeNavigationStep(player, step)) {
		navigationRuntime.rejectNextStep();
		telemetry.logActionFailure("navigate", "transition_unavailable", currentPosition);
		schedule(blockedRouteRetryInterval);
		return false;
	}

	navigationRuntime.completeStep(step, std::chrono::steady_clock::now());
	schedule(navigationDecisionDelay(*player));
	return false;
}

void PlayerBotController::navigate()
{
	auto decisionTimer = telemetry.recordDecision();
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
		telemetry.emit("lifecycle", lastPosition, "\"status\":\"removed\"");
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
	if (const PlayerBotFixtureInitialization initialization = fixtureDriver.delayedInitializationStatus(*player);
	    initialization != PlayerBotFixtureInitialization::NotPending) {
		if (initialization == PlayerBotFixtureInitialization::Cancelled) {
			return;
		}
		if (initialization == PlayerBotFixtureInitialization::Waiting) {
			schedule(navigationInterval);
			return;
		}
		emitFixtureEvents(fixtureDriver.runMagicTraining(*player), currentPosition);
		const bool useGoalSelector = !fixtureDriver.startInHunt() &&
		                             (departurePlanner.required(departureSnapshot(*player)) || fixtureDriver.goalLoop(true).selectGoal);
		if (useGoalSelector) {
			if (!selectTopLevelGoal(*player, currentPosition, "startup")) {
				return;
			}
		} else if (fixtureDriver.startInHunt()) {
			progressionRuntime.setActiveGoal(TopLevelGoal::Hunt);
			startHunt(player, currentPosition, "focused_fixture");
		} else {
			progressionRuntime.setActiveGoal(TopLevelGoal::Service);
			cyclePhase = CyclePhase::Service;
		}
		schedule(navigationInterval);
		return;
	}
	telemetry.maybeEmitSummary(currentPosition, telemetrySummary());
	recordActiveHuntCombat(*player);
	verifySpellCast(*player, currentPosition);
	if (huntRuntime.active() && cyclePhase == CyclePhase::Hunt) {
		if (huntRuntime.dangerObserved(*player, std::chrono::steady_clock::now(), huntRegionCooldown)) {
			beginService(player, currentPosition, "hunt_region_observed_danger");
			schedule(navigationInterval);
			return;
		}
	}
	const bool accessingReward = progressionSession.active(PlayerBotProgressionProcedure::PickupReward) &&
	                             (rewardSession.stage() == PlayerBotRewardStage::VerifyReward ||
	                              rewardSession.stage() == PlayerBotRewardStage::EquipReward ||
	                              rewardSession.stage() == PlayerBotRewardStage::VerifyEquipment);
	const bool verifyingDeparture = progressionSession.active(PlayerBotProgressionProcedure::OracleDeparture) &&
	                                departureSession.stage() == PlayerBotOracleDepartureStage::Verify;
	if (!accessingReward && !verifyingDeparture && handleHealing(player, currentPosition)) {
		schedule(blockedRouteRetryInterval);
		return;
	}
	const bool waitingForRecovery = cyclePhase == CyclePhase::Service && survivalRuntime.needsHealing(survivalSnapshot(*player));
	if (!accessingReward && !progressionSession.active(PlayerBotProgressionProcedure::OracleDeparture) &&
	    departurePlanner.required(departureSnapshot(*player)) && !waitingForRecovery) {
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
