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

// Rookgaard Oracle discovery, dialogue, and departure verification.
using namespace playerbot;

PlayerBotDeparturePlannerSnapshot PlayerBotController::departureSnapshot(const Player& player) const
{
	return {player.getLevel(), player.getVocation()->getId(), player.getTown() ? player.getTown()->getID() : 0,
	        oracleMinimumLevel, oracleMaximumLevel, rookgaardTownId, {}};
}

bool PlayerBotController::findOracleDeparture(Player& player, const Position& position, PlayerBotOracleDeparturePlan& plan,
	                                           std::deque<PlayerBotNavigationStep>& departureSteps)
{
	PlayerBotDeparturePlannerSnapshot snapshot = departureSnapshot(player);
	Npc* oracle = nullptr;
	for (const auto& entry : g_game.getNpcs()) {
		Npc* npc = entry.second;
		const std::string* capability = npc && !npc->isRemoved() ? npc->getParameter("playerbot_service") : nullptr;
		if (capability && *capability == "oracle") {
			oracle = npc;
			break;
		}
	}
	if (!oracle) {
		return false;
	}

	std::vector<Position> candidates;
	for (int32_t xOffset = -3; xOffset <= 3; ++xOffset) {
		for (int32_t yOffset = -3; yOffset <= 3; ++yOffset) {
			if (xOffset != 0 || yOffset != 0) {
				candidates.emplace_back(oracle->getPosition().x + xOffset, oracle->getPosition().y + yOffset,
				                        oracle->getPosition().z);
			}
		}
	}
	std::sort(candidates.begin(), candidates.end(), [&position](const Position& left, const Position& right) {
		const int32_t leftDistance = std::max(Position::getDistanceX(position, left), Position::getDistanceY(position, left));
		const int32_t rightDistance = std::max(Position::getDistanceX(position, right), Position::getDistanceY(position, right));
		return leftDistance == rightDistance ? left < right : leftDistance < rightDistance;
	});

	for (const Position& candidate : candidates) {
		Tile* tile = g_game.map.getTile(candidate);
		if (!tile || tile->queryAdd(0, player, 1, 0) != RETURNVALUE_NOERROR) {
			continue;
		}
		std::deque<PlayerBotNavigationStep> steps;
		uint64_t expandedNodes = 0;
		const auto startedAt = std::chrono::steady_clock::now();
		const PlayerBotNavigationRoutePlan routePlan = candidate == position ? PlayerBotNavigationRoutePlan{} :
			navigationRuntime.plan(player, candidate);
		const PlayerBotNavigationResult planResult = candidate == position ? PlayerBotNavigationResult::Reached : routePlan.metrics.result;
		if (candidate != position) {
			steps = routePlan.steps;
			expandedNodes = routePlan.metrics.expandedNodes;
		}
		const bool planned = planResult == PlayerBotNavigationResult::Reached;
		telemetry.recordPathfinding(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - startedAt), planned);
		if (!planned) {
			continue;
		}
		snapshot.providers.push_back({oracle->getID(), oracle->getPosition(),
		                              {true, false, candidate, static_cast<uint32_t>(steps.size()), expandedNodes}});
		const std::optional<PlayerBotOracleDeparturePlan> selected = departurePlanner.select(snapshot);
		if (!selected) return false;
		plan = *selected;
		departureSteps = std::move(steps);
		return true;
	}
	return false;
}

bool PlayerBotController::forceOracleDeparture(Player& player, const Position& position, const char* decisionReason)
{
	const TopLevelGoal previousGoal = goalArbiter.activeGoal();
	const uint64_t previousDecisionId = goalArbiter.decisionId();
	const bool interruptedHunt = previousGoal == TopLevelGoal::Hunt && cyclePhase == CyclePhase::Hunt;
	if (interruptedHunt) {
		finishHuntRegion(player, position, "level_eight_interrupt");
		emit("goal_result", position,
		     "\"decision_id\":" + std::to_string(previousDecisionId) +
		         ",\"goal\":\"hunt\",\"result\":\"interrupted\",\"reason\":\"level_eight_interrupt\"");
	}

	if (!combatRuntime.hasDefensiveCombat()) {
		g_game.playerCancelAttackAndFollow(playerId);
	}
	clearTraversalTarget(position, "level_eight_interrupt");
	clearNavigation();
	lootWorkflow.reset();
	player.closeContainer(corpseContainerId);
	setStage(ScenarioStage::Traverse, position);
	progressionRuntime.finish();
	serviceWorkflow.setStage(PlayerBotServiceStage::Discover);

	PlayerBotOracleDeparturePlan plan;
	std::deque<PlayerBotNavigationStep> route;
	const bool withinOracleLevelRange = player.getLevel() <= oracleMaximumLevel;
	const bool found = withinOracleLevelRange && findOracleDeparture(player, position, plan, route);
	const GoalCandidate candidate{TopLevelGoal::Departure, found, found ? oracleDepartureUtility : 0,
	                              !withinOracleLevelRange ? "above_maximum_level" :
	                              found ? "oracle_reachable" : "oracle_unreachable"};
	const PlayerBotGoalArbiter::GoalDecision decision = progressionRuntime.force(candidate);
	emitGoalCandidate(player, decision.candidate(TopLevelGoal::Departure), decision.id, position, decisionReason, nullptr,
	                  found ? &plan : nullptr);
	if (!found) {
		emit("goal_selection", position,
		     "\"decision_id\":" + std::to_string(decision.id) + ",\"decision_reason\":" +
		         jsonString(decisionReason) + ",\"from_goal\":" + jsonString(PlayerBotGoalArbiter::goalName(previousGoal)) +
		         ",\"to_goal\":\"oracle_departure\",\"result\":\"failed\",\"reason\":" +
		         jsonString(candidate.reason) + ",\"forced\":true,\"level\":" + std::to_string(player.getLevel()) +
		         ",\"player_vocation_id\":" + std::to_string(player.getVocation()->getId()) +
		         ",\"vocation_id\":" + std::to_string(oracleVocationId));
		stop("oracle_departure_unavailable", position);
		return false;
	}

	progressionRuntime.apply(decision);
	emit("goal_selection", position,
	     "\"decision_id\":" + std::to_string(decision.id) + ",\"decision_reason\":" +
	         jsonString(decisionReason) + ",\"from_goal\":" + jsonString(PlayerBotGoalArbiter::goalName(previousGoal)) +
	         ",\"to_goal\":\"oracle_departure\",\"utility\":" + std::to_string(oracleDepartureUtility) +
	         ",\"reason\":\"forced_level_eight_departure\",\"forced\":true,\"level\":" +
	         std::to_string(player.getLevel()) + ",\"player_vocation_id\":" +
	         std::to_string(player.getVocation()->getId()) + ",\"npc_id\":" + std::to_string(plan.npcId) +
	         ",\"town_id\":" + std::to_string(oracleTownId) + ",\"vocation_id\":" +
	         std::to_string(oracleVocationId));
	beginOracleDeparture(player, position, std::move(plan), std::move(route));
	return true;
}

void PlayerBotController::beginOracleDeparture(Player& player, const Position& position, PlayerBotOracleDeparturePlan plan,
	                                            std::deque<PlayerBotNavigationStep> steps)
{
	progressionRuntime.beginDeparture(std::move(plan));
	const auto& departure = departureSession.plan();
	serviceWorkflow.resetNpc(departure.npcId);
	navigationRuntime.adopt(departure.approachPosition, std::move(steps));
	emit("strategy_selection", position,
	     "\"goal\":\"oracle_departure\",\"npc_id\":" + std::to_string(departure.npcId) +
	         ",\"town\":\"thais\",\"town_id\":" + std::to_string(oracleTownId) +
	         ",\"vocation\":\"knight\",\"vocation_id\":" + std::to_string(oracleVocationId) +
	         ",\"travel_steps\":" + std::to_string(departure.travelSteps));
	say(player, "I have reached level 8. Going to the Oracle to become a knight of Thais.");
}

void PlayerBotController::finishOracleDeparture(Player* player, const Position& position, const char* result, const char* reason)
{
	emit("goal_result", position,
	     "\"decision_id\":" + std::to_string(goalArbiter.decisionId()) +
	         ",\"goal\":\"oracle_departure\",\"result\":" + jsonString(result) +
	         ",\"reason\":" + jsonString(reason));
	progressionRuntime.finish();
	serviceWorkflow.resetNpc();
	clearNavigation();
	if (std::strcmp(result, "success") == 0) {
		if (player) {
			say(*player, "Rookgaard departure complete. Starting mainland service.");
			beginService(player, position, "departure_complete");
			schedule(navigationInterval);
		}
		return;
	}
	if (std::strcmp(result, "interrupted") == 0 && player) {
		beginService(player, position, reason);
		return;
	}
	stop("oracle_departure_failed", position);
}

void PlayerBotController::processOracleDeparture(Player* player, const Position& currentPosition)
{
	const auto& departure = departureSession.plan();
	PlayerBotDepartureObservation observation;
	Npc* oracle = g_game.getNpcByID(departure.npcId);
	if (departureSession.stage() == PlayerBotOracleDepartureStage::Travel) {
		observation.navigationReached = processNavigation(player, currentPosition, departure.approachPosition);
		observation.navigationFailed = navigationRuntime.fixedTargetRouteFailureCount() >= maximumProgressionAttempts;
	} else {
		observation.npcAvailable = oracle && !oracle->isRemoved() && Position::areInRange<3, 3, 0>(currentPosition, oracle->getPosition());
		observation.greetingAcknowledged = serviceWorkflow.isGreetingAcknowledged();
		const bool correctVocation = player->getVocation()->getId() == oracleVocationId;
		const bool correctTown = player->getTown() && player->getTown()->getID() == oracleTownId;
		observation.departureVerified = correctVocation && correctTown && currentPosition == player->getTown()->getTemplePosition();
	}
	const PlayerBotProgressionOutcome result = progressionRuntime.advanceDeparture(observation);
	if (result.type == PlayerBotProgressionOutcomeType::Failed || result.type == PlayerBotProgressionOutcomeType::Succeeded) {
		if (result.type == PlayerBotProgressionOutcomeType::Succeeded) {
			emit("action_result", currentPosition,
			     "\"action\":\"oracle_departure\",\"result\":\"success\",\"town\":\"thais\",\"town_id\":" +
			         std::to_string(oracleTownId) + ",\"vocation\":\"knight\",\"vocation_id\":" +
			         std::to_string(oracleVocationId) + ",\"teleported\":true");
		}
		finishOracleDeparture(player, currentPosition, result.type == PlayerBotProgressionOutcomeType::Succeeded ? "success" : "failed", result.reason);
		return;
	}
	if (result.command.type == PlayerBotProgressionCommandType::Speak) {
		if (!oracle || oracle->isRemoved()) return;
		if (std::strcmp(result.command.reason, "hi") == 0) serviceWorkflow.resetGreetingAcknowledgement();
		telemetry.recordActionAttempt();
		oracle->receiveSpeech(player, TALKTYPE_PRIVATE_PN, result.command.reason);
	}
	if (result.command.type != PlayerBotProgressionCommandType::Navigate) schedule(result.command.type == PlayerBotProgressionCommandType::Speak ? 1000 : SCHEDULER_MINTICKS);
}
