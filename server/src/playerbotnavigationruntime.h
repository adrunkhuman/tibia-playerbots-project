/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef FS_PLAYERBOTNAVIGATIONRUNTIME_H
#define FS_PLAYERBOTNAVIGATIONRUNTIME_H

#include "playerbotnavigationsession.h"

#include <chrono>
#include <optional>

class Player;

struct PlayerBotNavigationPlanMetrics {
	PlayerBotNavigationResult result = PlayerBotNavigationResult::Unreachable;
	uint64_t expandedNodes = 0;
	size_t steps = 0;
	std::chrono::microseconds elapsed = std::chrono::microseconds::zero();
	bool attempted = false;
};

struct PlayerBotNavigationRoutePlan {
	PlayerBotNavigationPlanMetrics metrics;
	std::deque<PlayerBotNavigationStep> steps;
};

struct PlayerBotNavigationRuntimeTiming {
	std::chrono::steady_clock::time_point now;
	std::chrono::steady_clock::duration stepTimeout;
	std::chrono::steady_clock::duration blockSuppression;
	std::chrono::steady_clock::duration oscillationSuppression;
};

struct PlayerBotNavigationRuntimeInput {
	Player& player;
	Position currentPosition;
	Position destination;
	bool actionPending = false;
	bool canDoAction = false;
	bool forcePlanFailure = false;
	PlayerBotNavigationRuntimeTiming timing;
};

struct PlayerBotNavigationRuntimeOutcome {
	bool destinationReached = false;
	PlayerBotPendingMovementResult movementResult = PlayerBotPendingMovementResult::None;
	uint32_t stepFailureCount = 0;
	std::optional<PlayerBotNavigationOscillation> oscillation;
	std::optional<PlayerBotNavigationStep> pendingWorldChange;
	std::set<Position> blockedPositions;
	PlayerBotNavigationPlanMetrics plan;
	bool routeUnavailable = false;
	uint32_t fixedTargetRouteFailures = 0;
	bool fixedTargetRouteExhausted = false;
	std::optional<PlayerBotNavigationStep> nextStep;
};

class PlayerBotNavigationRuntime
{
	public:
		PlayerBotNavigationRoutePlan plan(Player& player, const Position& destination,
		                                  const std::set<Position>& blockedPositions = {},
		                                  uint64_t maximumExpandedNodes = playerBotNavigationMaximumExpandedNodes) const;
		PlayerBotNavigationRuntimeOutcome process(const PlayerBotNavigationRuntimeInput& input);

		void clear() { session.clear(); fixedTargetRouteFailures = 0; }
		void adopt(const Position& destination, std::deque<PlayerBotNavigationStep> steps) { session.adopt(destination, std::move(steps)); }
		void rejectNextStep() { session.clearRoute(); }
		void completeStep(const PlayerBotNavigationStep& step, std::chrono::steady_clock::time_point now);
		void suppress(const Position& position, std::chrono::steady_clock::time_point expires) { session.suppress(position, expires); }

		size_t routeSize() const { return session.routeSize(); }
		bool hasPendingWork() const { return session.hasPendingWork(); }
		bool isRouteCritical(const Position& position, std::chrono::steady_clock::time_point now) const { return session.isRouteCritical(position, now); }
		std::set<Position> activeBlockedPositions(std::chrono::steady_clock::time_point now) { return session.activeBlockedPositions(now); }
		bool hasActiveRouteBlock(std::chrono::steady_clock::time_point now) const { return session.hasActiveRouteBlock(now); }
		bool oscillationDetected() const { return session.oscillationDetected(); }
		uint32_t stepFailureCount() const { return session.stepFailureCount(); }
		uint32_t fixedTargetRouteFailureCount() const { return fixedTargetRouteFailures; }
		void resetFixedTargetRouteFailures() { fixedTargetRouteFailures = 0; }
		void clearBlockedPositions() { session.clearBlockedPositions(); }
		void resetStepFailures() { session.resetStepFailures(); }

	private:
		PlayerBotNavigator navigator;
		PlayerBotNavigationSession session;
		uint32_t fixedTargetRouteFailures = 0;
};

#endif
