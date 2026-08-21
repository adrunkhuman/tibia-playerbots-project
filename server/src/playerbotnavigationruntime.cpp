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

#include "playerbotnavigationruntime.h"

#include "player.h"

PlayerBotNavigationRoutePlan PlayerBotNavigationRuntime::plan(Player& player, const Position& destination,
	                                                            const std::set<Position>& blockedPositions,
	                                                            uint64_t maximumExpandedNodes) const
{
	PlayerBotNavigationRoutePlan routePlan;
	routePlan.metrics.attempted = true;
	const auto startedAt = std::chrono::steady_clock::now();
	routePlan.metrics.result = navigator.plan(player, destination, blockedPositions, routePlan.steps,
	                                          routePlan.metrics.expandedNodes, maximumExpandedNodes);
	routePlan.metrics.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - startedAt);
	routePlan.metrics.steps = routePlan.steps.size();
	return routePlan;
}

PlayerBotNavigationRuntimeOutcome PlayerBotNavigationRuntime::process(const PlayerBotNavigationRuntimeInput& input)
{
	PlayerBotNavigationRuntimeOutcome outcome;
	if (input.currentPosition == input.destination) {
		session.clear();
		outcome.destinationReached = true;
		outcome.stepFailureCount = session.stepFailureCount();
		return outcome;
	}

	if (const auto oscillation = session.observeProgress(input.currentPosition, input.destination, input.timing.now,
	                                                     input.timing.oscillationSuppression)) {
		outcome.oscillation = oscillation;
		outcome.stepFailureCount = session.stepFailureCount();
		return outcome;
	}

	outcome.movementResult = session.observeMovement(input.currentPosition, input.actionPending, input.timing.now,
	                                                input.timing.stepTimeout, input.timing.blockSuppression);
	if (outcome.movementResult == PlayerBotPendingMovementResult::Waiting) {
		outcome.stepFailureCount = session.stepFailureCount();
		return outcome;
	}

	outcome.pendingWorldChange = session.takeWorldChange();
	session.prepareDestination(input.destination);
	if (session.routeEmpty()) {
		outcome.blockedPositions = session.activeBlockedPositions(input.timing.now);
		PlayerBotNavigationRoutePlan routePlan;
		if (input.forcePlanFailure) {
			routePlan.metrics.attempted = true;
			routePlan.metrics.result = PlayerBotNavigationResult::Unreachable;
			routePlan.metrics.expandedNodes = playerBotNavigationMaximumExpandedNodes;
		} else {
			routePlan = plan(input.player, input.destination, outcome.blockedPositions);
		}
		outcome.plan = routePlan.metrics;
		if (routePlan.metrics.result != PlayerBotNavigationResult::Reached || routePlan.steps.empty()) {
			outcome.routeUnavailable = true;
			outcome.stepFailureCount = session.stepFailureCount();
			return outcome;
		}
		// Keep the old route active until the complete replacement route has been planned.
		session.installRoute(input.destination, std::move(routePlan.steps));
	}

	if (input.canDoAction && !session.routeEmpty()) {
		outcome.nextStep = session.nextStep();
	}
	outcome.stepFailureCount = session.stepFailureCount();
	return outcome;
}

void PlayerBotNavigationRuntime::completeStep(const PlayerBotNavigationStep& step,
	                                           std::chrono::steady_clock::time_point now)
{
	if (step.action == PlayerBotNavigationAction::UseDoor || step.action == PlayerBotNavigationAction::UseShovel) {
		session.beginWorldChange(step);
		return;
	}
	session.beginMovement(step, now);
}
