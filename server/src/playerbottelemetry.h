/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef FS_PLAYERBOTTELEMETRY_H
#define FS_PLAYERBOTTELEMETRY_H

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "position.h"

namespace playerbot {
	inline constexpr std::chrono::seconds summaryInterval(60);
	struct PlayerBotTelemetryTarget {
		uint32_t id;
		Position position;
	};

	struct PlayerBotTelemetrySummary {
		std::string state;
		std::optional<PlayerBotTelemetryTarget> target;
		std::string goal;
		std::string phase;
		std::string activity;
		std::string waitingReason;
		std::string recovery;
		bool playerStateAvailable = false;
		uint32_t health = 0;
		uint32_t maximumHealth = 0;
		uint32_t mana = 0;
		uint32_t maximumMana = 0;
		uint32_t level = 0;
		uint32_t freeCapacity = 0;
		uint64_t carriedGold = 0;
		uint64_t bankBalance = 0;
		uint32_t healthPotions = 0;
	};

	class PlayerBotTelemetry
	{
		public:
			explicit PlayerBotTelemetry(std::string playerName, uint32_t playerGuid, std::string controllerId);

			class DecisionTimer
			{
				public:
					explicit DecisionTimer(PlayerBotTelemetry& telemetry);
					~DecisionTimer();

				private:
					PlayerBotTelemetry& telemetry;
					std::chrono::steady_clock::time_point started;
			};

			DecisionTimer recordDecision();
			void emit(const char* event, const Position& position, const std::string& fields = {}) const;
			void logActionFailure(const char* action, const char* reason, const Position& position);
			void recordActionAttempt();
			void recordActionFailure();
			void recordStuckEvent();
			void recordPathfindingAttempt(std::chrono::microseconds elapsed);
			void recordPathfinding(std::chrono::microseconds elapsed, bool found);
			void maybeEmitSummary(const Position& position, const PlayerBotTelemetrySummary& summary);
			bool terminalLogged() const;
			void emitTerminal(const char* reason, const Position& position, const PlayerBotTelemetrySummary& summary);

		private:
			void emitSummary(const Position& position, bool final, const PlayerBotTelemetrySummary& summary);

			std::string playerName;
			uint32_t playerGuid;
			std::string controllerId;
			uint64_t decisions = 0;
			uint64_t decisionTimeUs = 0;
			uint64_t pathfindingCalls = 0;
			uint64_t pathfindingFailuresCount = 0;
			uint64_t pathfindingTimeUs = 0;
			uint64_t actionsAttemptedCount = 0;
			uint64_t actionsFailed = 0;
			uint64_t stuckEvents = 0;
			const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
			std::chrono::steady_clock::time_point lastSummary = started;
			std::chrono::steady_clock::time_point decisionStarted;
			bool decisionActive = false;
			bool terminal = false;
	};
}

#endif
