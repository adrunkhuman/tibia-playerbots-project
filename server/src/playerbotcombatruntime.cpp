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

#include "playerbotcombatruntime.h"
#include "playerbottargetingsession.h"

class PlayerBotCombatRuntime::PlayerBotTargetingSessionImpl {
	public:
		PlayerBotTargetingSession value;
};

PlayerBotCombatRuntime::PlayerBotCombatRuntime(PlayerBotCombatRuntimeConfig config) :
	config(config), session(std::make_unique<PlayerBotTargetingSessionImpl>())
{}

PlayerBotCombatRuntime::~PlayerBotCombatRuntime() = default;

namespace {
	uint32_t targetDistance(const Position& from, const Position& target)
	{
		return std::max(Position::getDistanceX(from, target), Position::getDistanceY(from, target));
	}
}

std::optional<PlayerBotCombatDecision> PlayerBotCombatRuntime::selectTraversalAttack(
	std::vector<PlayerBotTraversalCandidate> candidates, const Position& currentPosition, std::chrono::steady_clock::time_point now)
{
	auto select = [&candidates, &currentPosition, now, this](bool attacksPlayer) {
		std::vector<PlayerBotTarget> targets;
		for (const PlayerBotTraversalCandidate& candidate : candidates) {
			if (candidate.attacksPlayer == attacksPlayer) {
				targets.push_back({candidate.id, candidate.position, candidate.name});
			}
		}
		return session->value.selectVisibleTarget(std::move(targets), currentPosition, now);
	};
	auto selected = select(true);
	if (!selected) selected = select(false);
	if (!selected) {
		return std::nullopt;
	}
	auto candidate = std::find_if(candidates.begin(), candidates.end(), [&selected](const PlayerBotTraversalCandidate& value) {
		return value.id == selected->id;
	});
	return PlayerBotCombatDecision{PlayerBotCombatCommand::AttackTraversal, *selected,
	                               candidate == candidates.end() ? PlayerBotExpectedCorpse{} : candidate->expectedCorpse};
}

std::optional<PlayerBotCombatDecision> PlayerBotCombatRuntime::selectDefensiveAttack(
	std::vector<PlayerBotDefensiveTarget> candidates, const Position& currentPosition) const
{
	const auto selected = session->value.selectDefensiveTarget(std::move(candidates), currentPosition);
	if (!selected) {
		return std::nullopt;
	}
	return PlayerBotCombatDecision{PlayerBotCombatCommand::AttackDefensive, {selected->id, selected->position, selected->name}, {}, {},
	                               selected->routeCritical};
}

PlayerBotCombatDecision PlayerBotCombatRuntime::confirmAttack(const PlayerBotCombatDecision& command, bool accepted,
	std::chrono::steady_clock::time_point now)
{
	PlayerBotCombatDecision result = command;
	if (!accepted) {
		result.result = "failed";
		result.reason = "target_rejected";
		result.command = PlayerBotCombatCommand::None;
		return result;
	}
	if (command.command == PlayerBotCombatCommand::AttackTraversal) {
		session->value.beginTraversalCombat(command.target, command.expectedCorpse, now);
		result.result = "started";
		return result;
	}
	if (command.command == PlayerBotCombatCommand::AttackDefensive) {
		session->value.beginDefensiveCombat({command.target.id, command.target.position, command.target.name, command.routeCritical}, now);
		result.result = "started";
		return result;
	}
	result.command = PlayerBotCombatCommand::None;
	return result;
}

PlayerBotCombatDecision PlayerBotCombatRuntime::advance(const PlayerBotCombatSnapshot& snapshot)
{
	if (const auto defensive = session->value.defensiveTarget()) {
		const PlayerBotCombatTargetSnapshot& target = snapshot.defensive;
		if (!target.present || target.removed || target.dead) {
			return {PlayerBotCombatCommand::CompleteDefensiveCombat, {defensive->id, defensive->position, defensive->name}, {}, {},
			        defensive->routeCritical, "success", "target_defeated"};
		}
		if ((!defensive->routeCritical && !target.attacksPlayer) || !target.visible || !target.adjacent) {
			return {PlayerBotCombatCommand::CompleteDefensiveCombat, {defensive->id, defensive->position, defensive->name}, {}, {},
			        defensive->routeCritical, "skipped", "threat_disengaged"};
		}
		if (!target.attackedByPlayer) {
			return {PlayerBotCombatCommand::CompleteDefensiveCombat, {defensive->id, defensive->position, defensive->name}, {}, {},
			        defensive->routeCritical, "failed", "target_lost"};
		}
		if (session->value.defensiveCombatTimedOut(snapshot.now, config.combatTimeout)) {
			return {PlayerBotCombatCommand::CompleteDefensiveCombat, {defensive->id, defensive->position, defensive->name}, {}, {},
			        defensive->routeCritical, "failed", "combat_timeout"};
		}
		session->value.updateDefensiveTargetPosition(target.target.position);
		return {};
	}

	const auto traversal = session->value.traversalTarget();
	if (!traversal) {
		return {};
	}
	const PlayerBotCombatTargetSnapshot& target = snapshot.traversal;
	if (!target.present || target.removed || target.dead) {
		const auto defeated = session->value.takeDefeatedTraversalTarget();
		return {PlayerBotCombatCommand::BeginLoot, defeated ? PlayerBotTarget{defeated->id, defeated->position, defeated->name} : PlayerBotTarget{},
		        defeated ? defeated->expectedCorpse : PlayerBotExpectedCorpse{}};
	}
	if (!target.visible || !target.visibleCreature || !target.attackedByPlayer) {
		session->value.suppressTraversalTarget(target.target.id, snapshot.now + config.traversalSuppression);
		return {PlayerBotCombatCommand::Abandon, {traversal->id, traversal->position, traversal->name}, traversal->expectedCorpse,
		        {}, false, "abandoned", "target_lost"};
	}
	if (session->value.traversalCombatTimedOut(snapshot.now, config.combatTimeout)) {
		session->value.suppressTraversalTarget(target.target.id, snapshot.now + config.traversalSuppression);
		return {PlayerBotCombatCommand::Abandon, {traversal->id, traversal->position, traversal->name}, traversal->expectedCorpse, {}, false,
		        "failed", "combat_timeout"};
	}
	session->value.updateTraversalTargetPosition(target.target.position);
	return {};
}

void PlayerBotCombatRuntime::suppressTraversalTarget(uint32_t id, std::chrono::steady_clock::time_point now,
	                                                  std::chrono::steady_clock::duration suppression)
{
	session->value.suppressTraversalTarget(id, now + suppression);
}

std::optional<PlayerBotTraversalTarget> PlayerBotCombatRuntime::clearTraversalTarget() { return session->value.clearTraversalTarget(); }
std::optional<PlayerBotDefensiveTarget> PlayerBotCombatRuntime::clearDefensiveTarget() { return session->value.clearDefensiveTarget(); }
bool PlayerBotCombatRuntime::hasDefensiveCombat() const { return session->value.defensiveTarget().has_value(); }
bool PlayerBotCombatRuntime::hasActiveCombat() const { return session->value.activeTarget().has_value(); }
std::optional<PlayerBotTarget> PlayerBotCombatRuntime::activeTarget() const { return session->value.activeTarget(); }
std::optional<PlayerBotTraversalTarget> PlayerBotCombatRuntime::traversalTarget() const { return session->value.traversalTarget(); }
std::optional<PlayerBotDefensiveTarget> PlayerBotCombatRuntime::defensiveTarget() const { return session->value.defensiveTarget(); }
