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
#include <unordered_map>

#include "position.h"

namespace playerbot {
	inline constexpr std::chrono::seconds summaryInterval(60);
	inline constexpr std::chrono::seconds repeatedEventInterval(60);

	struct PlayerBotTelemetryTarget {
		uint32_t id;
		Position position;
	};

	struct PlayerBotTelemetrySummary {
		std::string state;
		std::optional<PlayerBotTelemetryTarget> target;
	};

	class PlayerBotTelemetry
	{
		public:
			explicit PlayerBotTelemetry(std::string playerName, uint32_t playerGuid);

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
			bool shouldEmitRepeated(const std::string& key);
			void logActionFailure(const char* action, const char* reason, const Position& position);
			void recordActionAttempt();
			void recordActionFailure();
			void recordStuckEvent();
			void recordPathfindingAttempt(std::chrono::microseconds elapsed);
			void recordPathfinding(std::chrono::microseconds elapsed, bool found);
			uint64_t actionsAttempted() const;
			uint64_t& actionsAttemptedForSession();
			uint64_t pathfindingFailures() const;
			void maybeEmitSummary(const Position& position, const PlayerBotTelemetrySummary& summary);
			bool terminalLogged() const;
			void emitTerminal(const char* reason, const Position& position, const PlayerBotTelemetrySummary& summary);

		private:
			void emitSummary(const Position& position, bool final, const PlayerBotTelemetrySummary& summary);

			std::string playerName;
			uint32_t playerGuid;
			uint64_t decisions = 0;
			uint64_t decisionTimeUs = 0;
			uint64_t pathfindingCalls = 0;
			uint64_t pathfindingFailuresCount = 0;
			uint64_t pathfindingTimeUs = 0;
			uint64_t actionsAttemptedCount = 0;
			uint64_t actionsFailed = 0;
			uint64_t stuckEvents = 0;
			uint64_t suppressedEvents = 0;
			const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
			std::chrono::steady_clock::time_point lastSummary = started;
			std::chrono::steady_clock::time_point decisionStarted;
			bool decisionActive = false;
			bool terminal = false;
			std::unordered_map<std::string, std::chrono::steady_clock::time_point> repeatedEventTimes;
	};
}

#endif
