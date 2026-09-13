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
#include "playerbottelemetry.h"

using namespace playerbot;

PlayerBotTelemetry::PlayerBotTelemetry(std::string playerName, uint32_t playerGuid, std::string controllerId) :
	playerName(std::move(playerName)), playerGuid(playerGuid), controllerId(std::move(controllerId))
{
}

PlayerBotTelemetry::DecisionTimer::DecisionTimer(PlayerBotTelemetry& telemetry) :
	telemetry(telemetry), started(std::chrono::steady_clock::now())
{
	telemetry.decisionStarted = started;
	telemetry.decisionActive = true;
	++telemetry.decisions;
}

PlayerBotTelemetry::DecisionTimer::~DecisionTimer()
{
	telemetry.decisionTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - started).count();
	telemetry.decisionActive = false;
}

PlayerBotTelemetry::DecisionTimer PlayerBotTelemetry::recordDecision()
{
	return DecisionTimer(*this);
}

void PlayerBotTelemetry::emit(const char* event, const Position& position, const std::string& fields) const
{
	emitPlayerbotEvent(playerName, playerGuid, controllerId, event, position, fields);
}

void PlayerBotTelemetry::logActionFailure(const char* action, const char* reason, const Position& position)
{
	++actionsFailed;
	emit("action_result", position, std::string("\"action\":") + jsonString(action) +
	     ",\"result\":\"failed\",\"reason\":" + jsonString(reason));
}

void PlayerBotTelemetry::recordActionAttempt()
{
	++actionsAttemptedCount;
}

void PlayerBotTelemetry::recordActionFailure()
{
	++actionsFailed;
}

void PlayerBotTelemetry::recordStuckEvent()
{
	++stuckEvents;
}

void PlayerBotTelemetry::recordPathfindingAttempt(std::chrono::microseconds elapsed)
{
	++pathfindingCalls;
	pathfindingTimeUs += elapsed.count();
}

void PlayerBotTelemetry::recordPathfinding(std::chrono::microseconds elapsed, bool found)
{
	recordPathfindingAttempt(elapsed);
	if (!found) {
		++pathfindingFailuresCount;
	}
}

void PlayerBotTelemetry::emitSummary(const Position& position, bool final, const PlayerBotTelemetrySummary& summary)
{
	const auto uptimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - started).count();
	uint64_t activeDecisionTimeUs = decisionTimeUs;
	if (decisionActive) {
		activeDecisionTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - decisionStarted).count();
	}

	std::ostringstream fields;
	fields << "\"final\":" << (final ? "true" : "false")
	       << ",\"uptime_ms\":" << uptimeMs
	       << ",\"state\":" << jsonString(summary.state)
	       << ",\"goal\":" << jsonString(summary.goal)
	       << ",\"phase\":" << jsonString(summary.phase)
	       << ",\"activity\":" << jsonString(summary.activity)
	       << ",\"waiting_reason\":" << (summary.waitingReason.empty() ? "null" : jsonString(summary.waitingReason))
	       << ",\"recovery\":" << (summary.recovery.empty() ? "null" : jsonString(summary.recovery))
	       << ",\"player_state_available\":" << (summary.playerStateAvailable ? "true" : "false")
	       << ",\"health\":";
	if (summary.playerStateAvailable) {
		fields << summary.health << ",\"maximum_health\":" << summary.maximumHealth
		       << ",\"mana\":" << summary.mana << ",\"maximum_mana\":" << summary.maximumMana
		       << ",\"level\":" << summary.level << ",\"free_capacity\":" << summary.freeCapacity
		       << ",\"carried_gold\":" << summary.carriedGold << ",\"bank_balance\":" << summary.bankBalance
		       << ",\"health_potions\":" << summary.healthPotions;
	} else {
		fields << "null,\"maximum_health\":null,\"mana\":null,\"maximum_mana\":null"
		       << ",\"level\":null,\"free_capacity\":null,\"carried_gold\":null"
		       << ",\"bank_balance\":null,\"health_potions\":null";
	}
	fields << ",\"target_id\":";
	if (!summary.target) {
		fields << "null";
	} else {
		fields << summary.target->id
		       << ",\"target_position\":{\"x\":" << summary.target->position.x
		       << ",\"y\":" << summary.target->position.y
		       << ",\"z\":" << static_cast<uint16_t>(summary.target->position.z) << '}';
	}
	fields << ",\"decisions\":" << decisions
	       << ",\"decision_time_us\":" << activeDecisionTimeUs
	       << ",\"pathfinding_calls\":" << pathfindingCalls
	       << ",\"pathfinding_failures\":" << pathfindingFailuresCount
	       << ",\"pathfinding_time_us\":" << pathfindingTimeUs
	       << ",\"actions_attempted\":" << actionsAttemptedCount
	       << ",\"actions_failed\":" << actionsFailed
	       << ",\"stuck_events\":" << stuckEvents
	       << ",\"suppressed_events\":0";
	emit("summary", position, fields.str());
}

void PlayerBotTelemetry::maybeEmitSummary(const Position& position, const PlayerBotTelemetrySummary& summary)
{
	const auto now = std::chrono::steady_clock::now();
	if (now - lastSummary < summaryInterval) {
		return;
	}
	emitSummary(position, false, summary);
	lastSummary = now;
}

bool PlayerBotTelemetry::terminalLogged() const
{
	return terminal;
}

void PlayerBotTelemetry::emitTerminal(const char* reason, const Position& position, const PlayerBotTelemetrySummary& summary)
{
	if (terminal) {
		return;
	}
	emitSummary(position, true, summary);
	emit("terminal", position, std::string("\"reason\":") + jsonString(reason));
	terminal = true;
}
