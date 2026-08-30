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

#include "playerbotnavigationsession.h"

void PlayerBotNavigationSession::clear()
{
	steps.clear();
	movementPending = false;
	worldChangePending = false;
	target = PlayerBotNavigationGoal();
	blockedStepCount = 0;
	pendingRouteBlocker.reset();
	pendingRouteBlockerId.reset();
	requiredRouteBlockerIds.clear();
}

void PlayerBotNavigationSession::adopt(const PlayerBotNavigationGoal& goal, std::deque<PlayerBotNavigationStep> newSteps)
{
	target = goal;
	steps = std::move(newSteps);
	movementPending = false;
	worldChangePending = false;
}

void PlayerBotNavigationSession::prepareGoal(const PlayerBotNavigationGoal& goal)
{
	if (target == goal) {
		return;
	}
	steps.clear();
	target = goal;
	blockedStepCount = 0;
	clearRequiredRouteBlockers();
	pendingRouteBlocker.reset();
	pendingRouteBlockerId.reset();
}

void PlayerBotNavigationSession::installRoute(const PlayerBotNavigationGoal& goal,
	                                           std::deque<PlayerBotNavigationStep> newSteps)
{
	target = goal;
	steps = std::move(newSteps);
}

PlayerBotPendingMovementResult PlayerBotNavigationSession::observeMovement(
	const Position& currentPosition, bool actionPending, std::chrono::steady_clock::time_point now,
	std::chrono::steady_clock::duration timeout, std::chrono::steady_clock::duration suppression)
{
	if (!movementPending) {
		return PlayerBotPendingMovementResult::None;
	}
	if (currentPosition == expectedPosition) {
		movementPending = false;
		blockedStepCount = 0;
		if (!steps.empty()) {
			steps.pop_front();
		}
		clearRequiredRouteBlockers();
		pendingRouteBlocker.reset();
		pendingRouteBlockerId.reset();
		return PlayerBotPendingMovementResult::Completed;
	}
	if (actionPending && now - stepStarted < timeout) {
		return PlayerBotPendingMovementResult::Waiting;
	}

	movementPending = false;
	steps.clear();
	blockedTarget = stepTarget;
	blockedTargetExpires = now + suppression;
	temporarilyBlockedPositions[stepTarget] = blockedTargetExpires;
	++blockedStepCount;
	return PlayerBotPendingMovementResult::Mismatch;
}

void PlayerBotNavigationSession::beginMovement(const PlayerBotNavigationStep& step,
	                                             std::chrono::steady_clock::time_point now)
{
	expectedPosition = step.expectedPosition;
	stepTarget = step.target;
	stepStarted = now;
	movementPending = true;
}

void PlayerBotNavigationSession::beginWorldChange(const PlayerBotNavigationStep& step)
{
	worldChangeStep = step;
	worldChangePending = true;
	steps.clear();
}

std::optional<PlayerBotNavigationStep> PlayerBotNavigationSession::takeWorldChange()
{
	if (!worldChangePending) {
		return std::nullopt;
	}
	worldChangePending = false;
	return worldChangeStep;
}

std::set<Position> PlayerBotNavigationSession::activeBlockedPositions(std::chrono::steady_clock::time_point now)
{
	std::set<Position> active;
	for (auto it = temporarilyBlockedPositions.begin(); it != temporarilyBlockedPositions.end();) {
		if (it->second <= now) {
			if (pendingRouteBlocker && *pendingRouteBlocker == it->first) {
				if (pendingRouteBlockerId) requiredRouteBlockerIds.erase(*pendingRouteBlockerId);
				pendingRouteBlocker.reset();
				pendingRouteBlockerId.reset();
			}
			it = temporarilyBlockedPositions.erase(it);
		} else {
			active.insert(it->first);
			++it;
		}
	}
	return active;
}

void PlayerBotNavigationSession::suppress(const Position& position, std::chrono::steady_clock::time_point expires)
{
	temporarilyBlockedPositions[position] = expires;
}

bool PlayerBotNavigationSession::avoidPendingRouteBlocker(
	uint32_t blockerId, const Position& position, std::chrono::steady_clock::time_point now,
	std::chrono::steady_clock::duration suppression)
{
	if (!movementPending || position != stepTarget) return false;
	movementPending = false;
	steps.clear();
	temporarilyBlockedPositions[position] = now + suppression;
	requiredRouteBlockerIds.erase(blockerId);
	pendingRouteBlocker = position;
	pendingRouteBlockerId = blockerId;
	return true;
}

void PlayerBotNavigationSession::confirmRequiredRouteBlocker()
{
	if (pendingRouteBlocker && pendingRouteBlockerId &&
	    temporarilyBlockedPositions.find(*pendingRouteBlocker) != temporarilyBlockedPositions.end()) {
		requiredRouteBlockerIds.insert(*pendingRouteBlockerId);
	}
}

std::optional<PlayerBotNavigationOscillation> PlayerBotNavigationSession::observeProgress(
	const Position& currentPosition, const PlayerBotNavigationGoal& goal, std::chrono::steady_clock::time_point now,
	std::chrono::steady_clock::duration suppression)
{
	detectedOscillation = false;
	if (!progressTargetSet || progressTarget != goal) {
		progressTarget = goal;
		progressTargetSet = true;
		progressPrevious = currentPosition;
		progressTwoAgo = Position();
		bestDistance = goal.distance(currentPosition);
		oscillationCount = 0;
		return std::nullopt;
	}

	const uint32_t currentDistance = goal.distance(currentPosition);
	if (currentPosition == progressPrevious) {
		return std::nullopt;
	}
	const Position previousPosition = progressPrevious;
	const bool oscillating = progressTwoAgo != Position() && currentPosition == progressTwoAgo &&
	                         currentDistance >= bestDistance;
	if (currentDistance < bestDistance) {
		bestDistance = currentDistance;
		if (!oscillating) {
			oscillationCount = 0;
		}
	}
	if (oscillating) {
		++oscillationCount;
	}
	progressTwoAgo = previousPosition;
	progressPrevious = currentPosition;
	if (oscillationCount < 3) {
		return std::nullopt;
	}

	Position suppressedTarget = currentPosition;
	Position suppressedExpected = currentPosition;
	for (const PlayerBotNavigationStep& step : steps) {
		if (step.action != PlayerBotNavigationAction::Move || step.expectedPosition.z != currentPosition.z) {
			suppressedTarget = step.target;
			suppressedExpected = step.expectedPosition;
			break;
		}
	}
	const auto expires = now + suppression;
	temporarilyBlockedPositions[suppressedTarget] = expires;
	temporarilyBlockedPositions[suppressedExpected] = expires;
	steps.clear();
	movementPending = false;
	worldChangePending = false;
	oscillationCount = 0;
	detectedOscillation = true;
	return PlayerBotNavigationOscillation{suppressedTarget, suppressedExpected, previousPosition};
}

bool PlayerBotNavigationSession::isRouteCritical(uint32_t blockerId, const Position& position,
	                                               std::chrono::steady_clock::time_point now) const
{
	const auto blocked = temporarilyBlockedPositions.find(position);
	return requiredRouteBlockerIds.find(blockerId) != requiredRouteBlockerIds.end() &&
	       blocked != temporarilyBlockedPositions.end() && now < blocked->second;
}
