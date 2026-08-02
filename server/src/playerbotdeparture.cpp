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

bool PlayerBotController::hasCompletedRookgaardDeparture(const Player& player) const
{
	return player.getVocation()->getId() != 0 && player.getTown() && player.getTown()->getID() != rookgaardTownId;
}

bool PlayerBotController::requiresRookgaardDeparture(const Player& player) const
{
	return player.getVocation()->getId() == 0 && player.getLevel() >= oracleMinimumLevel;
}

bool PlayerBotController::findOracleDeparture(Player& player, const Position& position, DeparturePlan& plan,
	                                           std::deque<PlayerBotNavigationStep>& departureSteps)
{
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
		++counters.pathfindingCalls;
		const auto startedAt = std::chrono::steady_clock::now();
		const PlayerBotNavigationResult planResult = candidate == position ? PlayerBotNavigationResult::Reached :
			navigator.plan(player, candidate, {}, steps, expandedNodes);
		const bool planned = planResult == PlayerBotNavigationResult::Reached;
		counters.pathfindingTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - startedAt).count();
		if (!planned) {
			++counters.pathfindingFailures;
			continue;
		}
		plan.npcId = oracle->getID();
		plan.npcPosition = oracle->getPosition();
		plan.approachPosition = candidate;
		plan.travelSteps = static_cast<uint32_t>(steps.size());
		plan.expandedNodes = expandedNodes;
		departureSteps = std::move(steps);
		return true;
	}
	return false;
}

bool PlayerBotController::forceOracleDeparture(Player& player, const Position& position, const char* decisionReason)
{
	const TopLevelGoal previousGoal = activeGoal;
	const bool interruptedHunt = previousGoal == TopLevelGoal::Hunt && cyclePhase == CyclePhase::Hunt;
	if (interruptedHunt) {
		finishHuntRegion(player, position, "level_eight_interrupt");
		emit("goal_result", position,
		     "\"decision_id\":" + std::to_string(goalDecisionId - 1) +
		         ",\"goal\":\"hunt\",\"result\":\"interrupted\",\"reason\":\"level_eight_interrupt\"");
	}

	if (defensiveTargetId == 0) {
		g_game.playerCancelAttackAndFollow(playerId);
	}
	clearRatTarget(position, "level_eight_interrupt");
	clearNavigation();
	pendingLootItemId = 0;
	pendingDiscardItemId = 0;
	expectedCorpseItemId = 0;
	expectedCorpseLootable = false;
	player.closeContainer(corpseContainerId);
	setStage(ScenarioStage::Traverse, position);
	progressionObjective = ProgressionObjective::None;
	serviceStage = ServiceStage::Discover;

	DeparturePlan plan;
	std::deque<PlayerBotNavigationStep> route;
	const bool withinOracleLevelRange = player.getLevel() <= oracleMaximumLevel;
	const bool found = withinOracleLevelRange && findOracleDeparture(player, position, plan, route);
	const GoalCandidate candidate{TopLevelGoal::Departure, found, found ? oracleDepartureUtility : 0,
	                              !withinOracleLevelRange ? "above_maximum_level" :
	                              found ? "oracle_reachable" : "oracle_unreachable"};
	emitGoalCandidate(player, candidate, position, decisionReason, nullptr, found ? &plan : nullptr);
	if (!found) {
		emit("goal_selection", position,
		     "\"decision_id\":" + std::to_string(goalDecisionId) + ",\"decision_reason\":" +
		         jsonString(decisionReason) + ",\"from_goal\":" + jsonString(topLevelGoalName(previousGoal)) +
		         ",\"to_goal\":\"oracle_departure\",\"result\":\"failed\",\"reason\":" +
		         jsonString(candidate.reason) + ",\"forced\":true,\"level\":" + std::to_string(player.getLevel()) +
		         ",\"player_vocation_id\":" + std::to_string(player.getVocation()->getId()) +
		         ",\"vocation_id\":" + std::to_string(oracleVocationId));
		stop("oracle_departure_unavailable", position);
		return false;
	}

	activeGoal = TopLevelGoal::Departure;
	emit("goal_selection", position,
	     "\"decision_id\":" + std::to_string(goalDecisionId) + ",\"decision_reason\":" +
	         jsonString(decisionReason) + ",\"from_goal\":" + jsonString(topLevelGoalName(previousGoal)) +
	         ",\"to_goal\":\"oracle_departure\",\"utility\":" + std::to_string(oracleDepartureUtility) +
	         ",\"reason\":\"forced_level_eight_departure\",\"forced\":true,\"level\":" +
	         std::to_string(player.getLevel()) + ",\"player_vocation_id\":" +
	         std::to_string(player.getVocation()->getId()) + ",\"npc_id\":" + std::to_string(plan.npcId) +
	         ",\"town_id\":" + std::to_string(oracleTownId) + ",\"vocation_id\":" +
	         std::to_string(oracleVocationId));
	beginOracleDeparture(player, position, std::move(plan), std::move(route));
	return true;
}

void PlayerBotController::beginOracleDeparture(Player& player, const Position& position, DeparturePlan plan,
	                                            std::deque<PlayerBotNavigationStep> steps)
{
	departurePlan = std::move(plan);
	progressionObjective = ProgressionObjective::OracleDeparture;
	departureStage = DepartureStage::Travel;
	progressionAttempts = 0;
	serviceTargetId = departurePlan.npcId;
	serviceGreetingAcknowledged = false;
	navigationTarget = departurePlan.approachPosition;
	navigationSteps = std::move(steps);
	emit("strategy_selection", position,
	     "\"goal\":\"oracle_departure\",\"npc_id\":" + std::to_string(departurePlan.npcId) +
	         ",\"town\":\"thais\",\"town_id\":" + std::to_string(oracleTownId) +
	         ",\"vocation\":\"knight\",\"vocation_id\":" + std::to_string(oracleVocationId) +
	         ",\"travel_steps\":" + std::to_string(departurePlan.travelSteps));
	say(player, "I have reached level 8. Going to the Oracle to become a knight of Thais.");
}

void PlayerBotController::finishOracleDeparture(Player* player, const Position& position, const char* result, const char* reason)
{
	emit("goal_result", position,
	     "\"decision_id\":" + std::to_string(goalDecisionId) +
	         ",\"goal\":\"oracle_departure\",\"result\":" + jsonString(result) +
	         ",\"reason\":" + jsonString(reason));
	progressionObjective = ProgressionObjective::None;
	serviceTargetId = 0;
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
	if (departureStage == DepartureStage::Travel) {
		if (!processNavigation(player, currentPosition, departurePlan.approachPosition)) {
			if (fixedTargetRouteFailureCount >= maximumProgressionAttempts) {
				finishOracleDeparture(player, currentPosition, "failed", "route_unavailable");
			}
			return;
		}
		departureStage = DepartureStage::Greet;
		schedule(SCHEDULER_MINTICKS);
		return;
	}

	if (departureStage == DepartureStage::Verify) {
		const bool correctVocation = player->getVocation()->getId() == oracleVocationId;
		const bool correctTown = player->getTown() && player->getTown()->getID() == oracleTownId;
		const bool teleported = correctTown && currentPosition == player->getTown()->getTemplePosition();
		if (!correctVocation || !correctTown || !teleported) {
			finishOracleDeparture(player, currentPosition, "failed", "departure_not_verified");
			return;
		}
		emit("action_result", currentPosition,
		     "\"action\":\"oracle_departure\",\"result\":\"success\",\"town\":\"thais\",\"town_id\":" +
		         std::to_string(oracleTownId) + ",\"vocation\":\"knight\",\"vocation_id\":" +
		         std::to_string(oracleVocationId) + ",\"teleported\":true");
		finishOracleDeparture(player, currentPosition, "success", "vocation_town_and_teleport_verified");
		return;
	}

	Npc* oracle = g_game.getNpcByID(departurePlan.npcId);
	if (!oracle || oracle->isRemoved() || !Position::areInRange<3, 3, 0>(currentPosition, oracle->getPosition())) {
		finishOracleDeparture(player, currentPosition, "failed", "oracle_unavailable");
		return;
	}

	if (departureStage == DepartureStage::Greet) {
		serviceGreetingAcknowledged = false;
		++counters.actionsAttempted;
		oracle->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "hi");
		departureStage = DepartureStage::ConfirmReady;
		schedule(1000);
		return;
	}
	if (departureStage == DepartureStage::ConfirmReady) {
		if (!serviceGreetingAcknowledged) {
			if (++progressionAttempts >= maximumProgressionAttempts) {
				finishOracleDeparture(player, currentPosition, "failed", "oracle_focus_unconfirmed");
				return;
			}
			departureStage = DepartureStage::Greet;
			schedule(1000);
			return;
		}
		++counters.actionsAttempted;
		oracle->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "yes");
		departureStage = DepartureStage::ChooseTown;
		schedule(1000);
		return;
	}
	if (departureStage == DepartureStage::ChooseTown) {
		++counters.actionsAttempted;
		oracle->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "thais");
		departureStage = DepartureStage::ChooseVocation;
		schedule(1000);
		return;
	}
	if (departureStage == DepartureStage::ChooseVocation) {
		++counters.actionsAttempted;
		oracle->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "knight");
		departureStage = DepartureStage::ConfirmVocation;
		schedule(1000);
		return;
	}
	if (departureStage == DepartureStage::ConfirmVocation) {
		++counters.actionsAttempted;
		departureStage = DepartureStage::Verify;
		oracle->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "yes");
		schedule(1000);
		return;
	}
}
