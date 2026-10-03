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
#include <map>
#include <cstdint>
#include <optional>
#include <string>

#include "position.h"

namespace playerbot {
	inline constexpr std::chrono::seconds summaryInterval(60);
	// Repetitive per-turn progress (pending searches, budget denials) is merged
	// or throttled to at most one record per interval.
	inline constexpr std::chrono::seconds progressRecordInterval(5);

	class PlayerBotRecordThrottle
	{
		public:
			explicit PlayerBotRecordThrottle(std::chrono::steady_clock::duration interval = progressRecordInterval) :
				interval(interval) {}
			// Returns the number of calls suppressed since the last admitted one,
			// or nullopt when this call is suppressed too.
			std::optional<uint32_t> admit(std::chrono::steady_clock::time_point now)
			{
				if (admitted && now - last < interval) {
					++suppressed;
					return std::nullopt;
				}
				admitted = true;
				last = now;
				const uint32_t count = suppressed;
				suppressed = 0;
				return count;
			}

		private:
			std::chrono::steady_clock::duration interval;
			std::chrono::steady_clock::time_point last;
			uint32_t suppressed = 0;
			bool admitted = false;
	};

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
		std::string planning; // Pending hunt, sell-loot, route, or depot search; empty when idle.
		// Hunt, sell-loot, and route searches end. Depot discovery can cycle
		// (select, fail, rescan), so it is labelled but still counts as idle.
		bool planningResetsIdle = false;
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
			void recordHuntAbort(const std::string& cause);
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
			std::map<std::string, uint64_t> huntAborts; // Patrol failures that ended a hunt, by cause.
			const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
			std::chrono::steady_clock::time_point lastSummary = started;
			// Standing still is expected while planning or fighting; idle time
			// counts only turns with none of movement, planning, or a target.
			std::chrono::steady_clock::time_point lastProgress = started;
			std::optional<Position> lastPosition;
			std::chrono::steady_clock::time_point decisionStarted;
			bool decisionActive = false;
			bool terminal = false;
	};
}

#endif
