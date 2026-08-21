/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef FS_PLAYERBOTCOMBATRUNTIME_H
#define FS_PLAYERBOTCOMBATRUNTIME_H

#include <chrono>
#include <memory>
#include <optional>
#include <vector>

#include "playerbotcombattarget.h"

enum class PlayerBotCombatCommand : uint8_t {
	None,
	AttackTraversal,
	AttackDefensive,
	BeginPursuit,
	PursueDestination,
	Abandon,
	BeginLoot,
	CompleteDefensiveCombat,
};

struct PlayerBotCombatDecision {
	PlayerBotCombatCommand command = PlayerBotCombatCommand::None;
	PlayerBotTarget target;
	PlayerBotExpectedCorpse expectedCorpse;
	Position destination;
	bool routeCritical = false;
	const char* result = nullptr;
	const char* reason = nullptr;
};

struct PlayerBotCombatTargetSnapshot {
	bool present = false;
	bool removed = false;
	bool dead = false;
	bool visible = false;
	bool visibleCreature = false;
	bool adjacent = false;
	bool attacksPlayer = false;
	bool attackedByPlayer = false;
	PlayerBotTarget target;
};

struct PlayerBotCombatSnapshot {
	Position currentPosition;
	std::chrono::steady_clock::time_point now;
	PlayerBotCombatTargetSnapshot traversal;
	PlayerBotCombatTargetSnapshot defensive;
	std::optional<Position> pursuitDestination;
};

struct PlayerBotCombatRuntimeConfig {
	std::chrono::steady_clock::duration combatTimeout;
	std::chrono::steady_clock::duration traversalSuppression;
	std::chrono::steady_clock::duration pursuitTimeout;
	std::chrono::steady_clock::duration pursuitSuppression;
	uint32_t maximumPursuitDistance = 0;
	uint32_t maximumReacquisitionDistance = 0;
};

// Owns combat transitions. The controller supplies already eligible world candidates and executes commands.
class PlayerBotCombatRuntime
{
	public:
		explicit PlayerBotCombatRuntime(PlayerBotCombatRuntimeConfig config);
		~PlayerBotCombatRuntime();

		std::optional<PlayerBotCombatDecision> selectTraversalAttack(std::vector<PlayerBotTraversalCandidate> candidates,
		                                                              const Position& currentPosition,
		                                                              std::chrono::steady_clock::time_point now);
		std::optional<PlayerBotCombatDecision> selectDefensiveAttack(std::vector<PlayerBotDefensiveTarget> candidates,
		                                                              const Position& currentPosition) const;
		PlayerBotCombatDecision confirmAttack(const PlayerBotCombatDecision& command, bool accepted,
		                                     std::chrono::steady_clock::time_point now);
		PlayerBotCombatDecision advance(const PlayerBotCombatSnapshot& snapshot);
		PlayerBotCombatDecision beginPursuit(const Position& currentPosition, const Position& destination,
		                                    std::chrono::steady_clock::time_point now);
		PlayerBotCombatDecision abandonPursuit(std::chrono::steady_clock::time_point now);
		std::optional<PlayerBotTraversalTarget> clearTraversalTarget();
		std::optional<PlayerBotDefensiveTarget> clearDefensiveTarget();

		bool hasDefensiveCombat() const;
		bool hasActiveCombat() const;
		std::optional<PlayerBotTarget> activeTarget() const;
		std::optional<PlayerBotTraversalTarget> traversalTarget() const;
		std::optional<PlayerBotDefensiveTarget> defensiveTarget() const;

	private:
		PlayerBotCombatRuntimeConfig config;
		class PlayerBotTargetingSessionImpl;
		std::unique_ptr<PlayerBotTargetingSessionImpl> session;
};

#endif
