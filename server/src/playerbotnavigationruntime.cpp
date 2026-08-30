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

PlayerBotNavigationRuntimeOutcome PlayerBotNavigationRuntime::process(const PlayerBotNavigationRuntimeInput& input)
{
	PlayerBotNavigationRuntimeOutcome outcome;
	if (input.goal.reached(input.currentPosition)) {
		session.clear();
		outcome.destinationReached = true;
		outcome.command = PlayerBotNavigationRuntimeCommand::None;
		outcome.stepFailureCount = session.stepFailureCount();
		return outcome;
	}

	if (const auto oscillation = session.observeProgress(input.currentPosition, input.goal, input.timing.now,
	                                                     input.timing.oscillationSuppression)) {
		outcome.oscillation = oscillation;
		outcome.command = PlayerBotNavigationRuntimeCommand::Retry;
		outcome.stepFailureCount = session.stepFailureCount();
		return outcome;
	}

	outcome.movementResult = session.observeMovement(input.currentPosition, input.actionPending, input.timing.now,
	                                                input.timing.stepTimeout, input.timing.blockSuppression);
	if (outcome.movementResult == PlayerBotPendingMovementResult::Mismatch && session.stepFailureCount() >= 3) {
		session.confirmRequiredRouteBlocker();
	}
	if (outcome.movementResult == PlayerBotPendingMovementResult::Waiting) {
		outcome.command = PlayerBotNavigationRuntimeCommand::Retry;
		outcome.stepFailureCount = session.stepFailureCount();
		return outcome;
	}

	outcome.pendingWorldChange = session.takeWorldChange();
	if (outcome.pendingWorldChange) {
		outcome.command = PlayerBotNavigationRuntimeCommand::Retry;
		outcome.stepFailureCount = session.stepFailureCount();
		return outcome;
	}
	session.prepareGoal(input.goal);
	if (session.routeEmpty()) {
		outcome.blockedPositions = session.activeBlockedPositions(input.timing.now);
		outcome.routeRequest = {input.goal, outcome.blockedPositions, playerBotNavigationMaximumExpandedNodes};
		outcome.command = PlayerBotNavigationRuntimeCommand::Plan;
		outcome.stepFailureCount = session.stepFailureCount();
		return outcome;
	}

	dispatchNextStep(input.canDoAction, outcome);
	outcome.stepFailureCount = session.stepFailureCount();
	return outcome;
}

PlayerBotNavigationRuntimeOutcome PlayerBotNavigationRuntime::observePlan(PlayerBotNavigationPlanObservation observation)
{
	PlayerBotNavigationRuntimeOutcome outcome;
	outcome.plan = observation.plan.metrics;
	if (observation.plan.metrics.result != PlayerBotNavigationResult::Reached ||
	    (!observation.startsNavigation && observation.plan.steps.empty())) {
		outcome.routeUnavailable = true;
		const std::set<Position> activeBlockers = session.activeBlockedPositions(observation.now);
		if (activeBlockers.empty()) ++fixedTargetRouteFailures;
		else session.confirmRequiredRouteBlocker();
		outcome.fixedTargetRouteFailures = fixedTargetRouteFailures;
		outcome.fixedTargetRouteExhausted = fixedTargetRouteFailures >= 20;
		outcome.command = outcome.fixedTargetRouteExhausted ? PlayerBotNavigationRuntimeCommand::Fail :
			PlayerBotNavigationRuntimeCommand::Retry;
		outcome.stepFailureCount = session.stepFailureCount();
		return outcome;
	}
	fixedTargetRouteFailures = 0;
	// A planned route replaces existing work only after planning completed.
	if (observation.startsNavigation) session.adopt(observation.goal, std::move(observation.plan.steps));
	else session.installRoute(observation.goal, std::move(observation.plan.steps));
	dispatchNextStep(observation.canDoAction, outcome);
	outcome.stepFailureCount = session.stepFailureCount();
	return outcome;
}

PlayerBotNavigationRuntimeOutcome PlayerBotNavigationRuntime::observeStep(const PlayerBotNavigationStepObservation& observation)
{
	PlayerBotNavigationRuntimeOutcome outcome;
	if (observation.result == PlayerBotNavigationStepResult::Rejected) {
		if (observation.step.topologyPortal) {
			session.suppress(observation.step.target, observation.now + observation.suppression);
		}
		session.clearRoute();
	} else if (observation.step.action == PlayerBotNavigationAction::UseDoor || observation.step.action == PlayerBotNavigationAction::UseShovel) {
		session.beginWorldChange(observation.step);
	} else {
		session.beginMovement(observation.step, observation.now);
	}
	outcome.stepFailureCount = session.stepFailureCount();
	return outcome;
}

PlayerBotNavigationRuntimeOutcome PlayerBotNavigationRuntime::observeWorldChange(const PlayerBotNavigationWorldChangeObservation& observation)
{
	if (observation.unresolved) session.suppress(observation.step.target, observation.now + observation.suppression);
	PlayerBotNavigationRuntimeOutcome outcome;
	outcome.stepFailureCount = session.stepFailureCount();
	return outcome;
}

void PlayerBotNavigationRuntime::dispatchNextStep(bool canDoAction, PlayerBotNavigationRuntimeOutcome& outcome) const
{
	if (!canDoAction || session.routeEmpty()) {
		outcome.command = PlayerBotNavigationRuntimeCommand::Retry;
		return;
	}
	outcome.nextStep = session.nextStep();
	outcome.command = outcome.nextStep->action == PlayerBotNavigationAction::Move ?
		PlayerBotNavigationRuntimeCommand::Move : PlayerBotNavigationRuntimeCommand::Use;
}
