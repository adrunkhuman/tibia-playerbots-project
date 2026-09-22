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

// Only a complete, unexposed same-floor walk can extend known return coverage.
// Added danger needs a new return-risk estimate. Portals, NPC travel, item use,
// doors, and partial approaches may be one-way even within one hunt region.
inline bool playerBotNavigationIsReversibleLocalWalk(
    const Position& source, const Position& destination, const PlayerBotNavigationRoutePlan& plan)
{
	if (plan.metrics.result != PlayerBotNavigationResult::Reached || source.z != destination.z ||
	    plan.metrics.dangerCost != 0 || plan.metrics.maximumHealthLossPerSecond != 0 ||
	    (source != destination && plan.steps.empty()) || plan.metrics.fare != 0 ||
	    plan.metrics.steps != plan.steps.size()) return false;
	Position cursor = source;
	for (const PlayerBotNavigationStep& step : plan.steps) {
		if (step.action != PlayerBotNavigationAction::Move || step.topologyPortal ||
		    step.target != step.expectedPosition || step.target == cursor ||
		    !Position::areInRange<1, 1, 0>(cursor, step.target)) return false;
		cursor = step.expectedPosition;
	}
	return cursor == destination;
}

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

// Fixed-goal route failures are cleared by observed positional progress, not by
// a planner claiming that a route exists. This keeps immediately blocked plans
// in one bounded failure sequence.
class PlayerBotFixedTargetFailureTracker
{
	public:
		bool observePosition(bool sameGoal, uint32_t distance)
		{
			if (!goalSet || !sameGoal) {
				goalSet = true;
				bestDistance = distance;
				failures = 0;
				blockedPlanPendingFailure = false;
				return false;
			}
			if (distance >= bestDistance) return false;
			bestDistance = distance;
			failures = 0;
			blockedPlanPendingFailure = false;
			return true;
		}
		void observePlan(bool routeAvailable)
		{
			if (routeAvailable) {
				blockedPlanPendingFailure = false;
				return;
			}
			if (!blockedPlanPendingFailure) ++failures;
			blockedPlanPendingFailure = false;
		}
		void observeBlockedPlan()
		{
			++failures;
			blockedPlanPendingFailure = true;
		}
		void observeRejectedAcceptedPlan()
		{
			++failures;
			blockedPlanPendingFailure = false;
		}
		void reset()
		{
			goalSet = false;
			failures = 0;
			blockedPlanPendingFailure = false;
		}
		uint32_t count() const { return failures; }
		bool exhausted() const { return failures >= 20; }

	private:
		uint32_t bestDistance = 0;
		uint32_t failures = 0;
		bool goalSet = false;
		bool blockedPlanPendingFailure = false;
};

struct PlayerBotNavigationRuntimeOutcome {
	bool destinationReached = false;
	bool positionalProgress = false;
	bool fixedTargetChanged = false;
	PlayerBotPendingMovementResult movementResult = PlayerBotPendingMovementResult::None;
	std::optional<Position> failedMovementTarget;
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
		PlayerBotNavigationRuntimeOutcome rejectAcceptedPlan();
		PlayerBotNavigationRuntimeOutcome observeStep(const PlayerBotNavigationStepObservation& observation);
		PlayerBotNavigationRuntimeOutcome observeWorldChange(const PlayerBotNavigationWorldChangeObservation& observation);

		void reset()
		{
			session.clear();
			fixedTargetFailures.reset();
			fixedTargetGoal.reset();
		}
		void resetPatrolRecovery() { session.clearBlockedPositions(); session.resetStepFailures(); }
		void clearBlockedPositions() { session.clearBlockedPositions(); }

		size_t routeSize() const { return session.routeSize(); }
		bool hasPendingWork() const { return session.hasPendingWork(); }
		std::set<Position> activeBlockedPositions(std::chrono::steady_clock::time_point now) { return session.activeBlockedPositions(now); }
		bool hasActiveRouteBlock(std::chrono::steady_clock::time_point now) const { return session.hasActiveRouteBlock(now); }
		bool oscillationDetected() const { return session.oscillationDetected(); }
		uint32_t stepFailureCount() const { return session.stepFailureCount(); }
		uint32_t fixedTargetRouteFailureCount() const { return fixedTargetFailures.count(); }
	private:
		void dispatchNextStep(bool canDoAction, PlayerBotNavigationRuntimeOutcome& outcome) const;
		PlayerBotNavigationSession session;
		PlayerBotFixedTargetFailureTracker fixedTargetFailures;
		std::optional<PlayerBotNavigationGoal> fixedTargetGoal;
};

#endif
