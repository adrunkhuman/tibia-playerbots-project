/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef FS_PLAYERBOTNAVIGATIONSESSION_H
#define FS_PLAYERBOTNAVIGATIONSESSION_H

#include "playerbotnavigation.h"

#include <chrono>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>

enum class PlayerBotPendingMovementResult : uint8_t {
	None,
	Completed,
	Waiting,
	Mismatch,
};

struct PlayerBotNavigationOscillation {
	Position blockedTarget;
	Position blockedExpected;
	Position previousPosition;
};

class PlayerBotNavigationSession
{
	public:
		void clear();
		void adopt(const PlayerBotNavigationGoal& goal, std::deque<PlayerBotNavigationStep> steps);
		void prepareGoal(const PlayerBotNavigationGoal& goal);
		void installRoute(const PlayerBotNavigationGoal& goal, std::deque<PlayerBotNavigationStep> steps);

		const PlayerBotNavigationGoal& goal() const { return target; }
		bool routeEmpty() const { return steps.empty(); }
		const PlayerBotNavigationStep& nextStep() const { return steps.front(); }
		void clearRoute() { steps.clear(); }
		size_t routeSize() const { return steps.size(); }
		bool hasPendingWork() const { return movementPending || worldChangePending || !steps.empty(); }

		PlayerBotPendingMovementResult observeMovement(const Position& currentPosition, bool actionPending,
		                                                   std::chrono::steady_clock::time_point now,
		                                                   std::chrono::steady_clock::duration timeout,
		                                                   std::chrono::steady_clock::duration suppression);
		void beginMovement(const PlayerBotNavigationStep& step, std::chrono::steady_clock::time_point now);
		void beginWorldChange(const PlayerBotNavigationStep& step);
		std::optional<PlayerBotNavigationStep> takeWorldChange();

		std::set<Position> activeBlockedPositions(std::chrono::steady_clock::time_point now);
		void suppress(const Position& position, std::chrono::steady_clock::time_point expires);
		void clearBlockedPositions() { temporarilyBlockedPositions.clear(); }

		std::optional<PlayerBotNavigationOscillation> observeProgress(
			const Position& currentPosition, const PlayerBotNavigationGoal& goal,
			std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration suppression);

		bool isRouteCritical(const Position& position, std::chrono::steady_clock::time_point now) const;
		bool hasActiveRouteBlock(std::chrono::steady_clock::time_point now) const { return now < blockedTargetExpires; }
		bool oscillationDetected() const { return detectedOscillation; }
		uint32_t stepFailureCount() const { return blockedStepCount; }
		void resetStepFailures() { blockedStepCount = 0; }

	private:
		std::deque<PlayerBotNavigationStep> steps;
		PlayerBotNavigationGoal target;
		Position expectedPosition;
		Position stepTarget;
		PlayerBotNavigationGoal progressTarget;
		bool progressTargetSet = false;
		Position progressPrevious;
		Position progressTwoAgo;
		uint32_t bestDistance = std::numeric_limits<uint32_t>::max();
		uint32_t oscillationCount = 0;
		Position blockedTarget;
		std::chrono::steady_clock::time_point stepStarted;
		std::chrono::steady_clock::time_point blockedTargetExpires;
		PlayerBotNavigationStep worldChangeStep;
		std::map<Position, std::chrono::steady_clock::time_point> temporarilyBlockedPositions;
		uint32_t blockedStepCount = 0;
		bool movementPending = false;
		bool worldChangePending = false;
		bool detectedOscillation = false;
};

#endif
