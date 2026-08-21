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

#include "playerbottargetingsession.h"

namespace {
	uint32_t targetDistance(const Position& from, const Position& target)
	{
		return std::max(Position::getDistanceX(from, target), Position::getDistanceY(from, target));
	}
}

std::optional<PlayerBotTarget> PlayerBotTargetingSession::selectVisibleTarget(
	std::vector<PlayerBotTarget> candidates, const Position& currentPosition, std::chrono::steady_clock::time_point now)
{
	for (auto suppressed = suppressedTraversalTargets.begin(); suppressed != suppressedTraversalTargets.end();) {
		if (suppressed->second <= now) {
			suppressed = suppressedTraversalTargets.erase(suppressed);
		} else {
			++suppressed;
		}
	}
	std::sort(candidates.begin(), candidates.end(), [&currentPosition](const PlayerBotTarget& left, const PlayerBotTarget& right) {
		const uint32_t leftDistance = targetDistance(currentPosition, left.position);
		const uint32_t rightDistance = targetDistance(currentPosition, right.position);
		return leftDistance == rightDistance ? left.id < right.id : leftDistance < rightDistance;
	});
	for (const PlayerBotTarget& candidate : candidates) {
		if (suppressedTraversalTargets.find(candidate.id) == suppressedTraversalTargets.end()) {
			return candidate;
		}
	}
	return std::nullopt;
}

std::optional<PlayerBotDefensiveTarget> PlayerBotTargetingSession::selectDefensiveTarget(
	std::vector<PlayerBotDefensiveTarget> candidates, const Position& currentPosition) const
{
	std::sort(candidates.begin(), candidates.end(), [&currentPosition](const PlayerBotDefensiveTarget& left,
	                                                                  const PlayerBotDefensiveTarget& right) {
		if (left.routeCritical != right.routeCritical) {
			return left.routeCritical;
		}
		const uint32_t leftDistance = targetDistance(currentPosition, left.position);
		const uint32_t rightDistance = targetDistance(currentPosition, right.position);
		return leftDistance == rightDistance ? left.id < right.id : leftDistance < rightDistance;
	});
	return candidates.empty() ? std::nullopt : std::optional<PlayerBotDefensiveTarget>(candidates.front());
}

void PlayerBotTargetingSession::beginTraversalCombat(PlayerBotTarget target, PlayerBotExpectedCorpse expectedCorpse,
	                                                  std::chrono::steady_clock::time_point now)
{
	PlayerBotTraversalTarget traversalTarget;
	traversalTarget.id = target.id;
	traversalTarget.position = target.position;
	traversalTarget.name = std::move(target.name);
	traversalTarget.expectedCorpse = expectedCorpse;
	activeTraversalTarget = std::move(traversalTarget);
	traversalCombatStarted = now;
	state = TraversalState::Combat;
}

void PlayerBotTargetingSession::updateTraversalTargetPosition(const Position& position)
{
	if (activeTraversalTarget) {
		activeTraversalTarget->position = position;
	}
}

bool PlayerBotTargetingSession::traversalCombatTimedOut(std::chrono::steady_clock::time_point now,
	                                                       std::chrono::steady_clock::duration timeout) const
{
	return activeTraversalTarget && now - traversalCombatStarted >= timeout;
}

std::optional<PlayerBotTraversalTarget> PlayerBotTargetingSession::takeDefeatedTraversalTarget()
{
	std::optional<PlayerBotTraversalTarget> target = std::move(activeTraversalTarget);
	activeTraversalTarget.reset();
	state = TraversalState::None;
	return target;
}

std::optional<PlayerBotTraversalTarget> PlayerBotTargetingSession::clearTraversalTarget()
{
	return takeDefeatedTraversalTarget();
}

void PlayerBotTargetingSession::suppressTraversalTarget(uint32_t id, std::chrono::steady_clock::time_point expires)
{
	if (id != 0) {
		suppressedTraversalTargets[id] = expires;
	}
}

void PlayerBotTargetingSession::beginDefensiveCombat(PlayerBotDefensiveTarget target, std::chrono::steady_clock::time_point now)
{
	activeDefensiveTarget = std::move(target);
	defensiveCombatStarted = now;
}

void PlayerBotTargetingSession::updateDefensiveTargetPosition(const Position& position)
{
	if (activeDefensiveTarget) {
		activeDefensiveTarget->position = position;
	}
}

bool PlayerBotTargetingSession::defensiveCombatTimedOut(std::chrono::steady_clock::time_point now,
	                                                       std::chrono::steady_clock::duration timeout) const
{
	return activeDefensiveTarget && now - defensiveCombatStarted >= timeout;
}

std::optional<PlayerBotDefensiveTarget> PlayerBotTargetingSession::clearDefensiveTarget()
{
	std::optional<PlayerBotDefensiveTarget> target = std::move(activeDefensiveTarget);
	activeDefensiveTarget.reset();
	return target;
}

void PlayerBotTargetingSession::beginPursuit(const Position& start, const Position& destination,
	                                          std::chrono::steady_clock::time_point now)
{
	pursuitStarted = now;
	pursuitStartPosition = start;
	lastKnownPursuitDestination = destination;
	state = TraversalState::Pursuit;
}

bool PlayerBotTargetingSession::pursuitBudgetExhausted(const Position& currentPosition,
	                                                     std::chrono::steady_clock::time_point now,
	                                                     std::chrono::steady_clock::duration timeout,
	                                                     uint32_t maximumDistance) const
{
	return !activeTraversalTarget || now - pursuitStarted >= timeout ||
	       targetDistance(pursuitStartPosition, currentPosition) > maximumDistance;
}

std::optional<PlayerBotTraversalTarget> PlayerBotTargetingSession::abandonPursuit(
	std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration suppression)
{
	if (activeTraversalTarget) {
		suppressTraversalTarget(activeTraversalTarget->id, now + suppression);
	}
	return clearTraversalTarget();
}

std::optional<PlayerBotTarget> PlayerBotTargetingSession::activeTarget() const
{
	if (activeDefensiveTarget) {
		PlayerBotTarget target;
		target.id = activeDefensiveTarget->id;
		target.position = activeDefensiveTarget->position;
		target.name = activeDefensiveTarget->name;
		return target;
	}
	if (activeTraversalTarget) {
		PlayerBotTarget target;
		target.id = activeTraversalTarget->id;
		target.position = activeTraversalTarget->position;
		target.name = activeTraversalTarget->name;
		return target;
	}
	return std::nullopt;
}
