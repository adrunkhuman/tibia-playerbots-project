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

struct PlayerBotNavigationPlanMetrics {
	PlayerBotNavigationResult result = PlayerBotNavigationResult::Unreachable;
	uint64_t expandedNodes = 0;
	Position closestPosition;
	Position waypoint;
	size_t steps = 0;
	double estimatedTravelSeconds = 0;
	uint32_t movementCost = 0;
	uint32_t dangerCost = 0;
	uint64_t fare = 0;
	double maximumHealthLossPerSecond = 0;
	bool dangerAware = false;
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

enum class PlayerBotNavigationRuntimeCommand : uint8_t {
	None,
	Plan,
	Move,
	Use,
	Retry,
	Fail,
};

struct PlayerBotNavigationRouteRequest {
	PlayerBotNavigationGoal goal;
	std::set<Position> blockedPositions;
	uint64_t maximumExpandedNodes = playerBotNavigationMaximumExpandedNodes;
};

struct PlayerBotNavigationRuntimeInput {
	Position currentPosition;
	PlayerBotNavigationGoal goal;
	bool actionPending = false;
	bool canDoAction = false;
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
	bool routeUnsafe = false;
	uint32_t fixedTargetRouteFailures = 0;
	bool fixedTargetRouteExhausted = false;
	PlayerBotNavigationRuntimeCommand command = PlayerBotNavigationRuntimeCommand::None;
	std::optional<PlayerBotNavigationRouteRequest> routeRequest;
	std::optional<PlayerBotNavigationStep> nextStep;
};

// Planner and dispatcher results are immutable observations. The runtime owns
// all resulting route and pending-step state transitions.
struct PlayerBotNavigationPlanObservation {
	PlayerBotNavigationGoal goal;
	PlayerBotNavigationRoutePlan plan;
	bool canDoAction = false;
	bool startsNavigation = false;
	std::chrono::steady_clock::time_point now;
};

enum class PlayerBotNavigationStepResult : uint8_t {
	Dispatched,
	Rejected,
};

struct PlayerBotNavigationStepObservation {
	PlayerBotNavigationStep step;
	PlayerBotNavigationStepResult result = PlayerBotNavigationStepResult::Rejected;
	std::chrono::steady_clock::time_point now;
	std::chrono::steady_clock::duration suppression;
};

struct PlayerBotNavigationWorldChangeObservation {
	PlayerBotNavigationStep step;
	bool unresolved = false;
	std::chrono::steady_clock::time_point now;
	std::chrono::steady_clock::duration suppression;
};

// process() owns route and pending-step transitions and returns at most one
// command. Supply a completed Plan through observePlan(), and report the exact
// nextStep from a Move or Use through observeStep(). A dispatched move remains
// pending until a later process() observes its result. A dispatched UseDoor or
// UseShovel becomes pendingWorldChange; inspect that change and pass it to
// observeWorldChange().
// Rejected topology portals and unresolved world changes suppress their target
// before the runtime replans.
class PlayerBotNavigationRuntime
{
	public:
		PlayerBotNavigationRuntimeOutcome process(const PlayerBotNavigationRuntimeInput& input);
		PlayerBotNavigationRuntimeOutcome observePlan(PlayerBotNavigationPlanObservation observation);
		PlayerBotNavigationRuntimeOutcome observeStep(const PlayerBotNavigationStepObservation& observation);
		PlayerBotNavigationRuntimeOutcome observeWorldChange(const PlayerBotNavigationWorldChangeObservation& observation);

		void reset() { session.clear(); fixedTargetRouteFailures = 0; }
		void resetPatrolRecovery() { session.clearBlockedPositions(); session.resetStepFailures(); }
		void clearBlockedPositions() { session.clearBlockedPositions(); }

		size_t routeSize() const { return session.routeSize(); }
		bool hasPendingWork() const { return session.hasPendingWork(); }
		bool avoidPendingRouteBlocker(uint32_t blockerId, const Position& position, std::chrono::steady_clock::time_point now,
		                              std::chrono::steady_clock::duration suppression)
		{
			return session.avoidPendingRouteBlocker(blockerId, position, now, suppression);
		}
		bool isRouteCritical(uint32_t blockerId, const Position& position, std::chrono::steady_clock::time_point now) const { return session.isRouteCritical(blockerId, position, now); }
		std::set<Position> activeBlockedPositions(std::chrono::steady_clock::time_point now) { return session.activeBlockedPositions(now); }
		bool hasActiveRouteBlock(std::chrono::steady_clock::time_point now) const { return session.hasActiveRouteBlock(now); }
		bool oscillationDetected() const { return session.oscillationDetected(); }
		uint32_t stepFailureCount() const { return session.stepFailureCount(); }
		uint32_t fixedTargetRouteFailureCount() const { return fixedTargetRouteFailures; }
	private:
		void dispatchNextStep(bool canDoAction, PlayerBotNavigationRuntimeOutcome& outcome) const;
		PlayerBotNavigationSession session;
		uint32_t fixedTargetRouteFailures = 0;
};

#endif
