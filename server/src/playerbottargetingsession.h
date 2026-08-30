/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef FS_PLAYERBOTTARGETINGSESSION_H
#define FS_PLAYERBOTTARGETINGSESSION_H

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "playerbotcombattarget.h"

// The combat runtime supplies only candidates that are valid under controller policy.
class PlayerBotTargetingSession
{
	public:
		std::optional<PlayerBotTarget> selectVisibleTarget(std::vector<PlayerBotTarget> candidates,
		                                                   const Position& currentPosition,
		                                                   std::chrono::steady_clock::time_point now);
		std::optional<PlayerBotDefensiveTarget> selectDefensiveTarget(
			std::vector<PlayerBotDefensiveTarget> candidates, const Position& currentPosition) const;

		void beginTraversalCombat(PlayerBotTarget target, PlayerBotExpectedCorpse expectedCorpse,
		                          std::chrono::steady_clock::time_point now);
		const std::optional<PlayerBotTraversalTarget>& traversalTarget() const { return activeTraversalTarget; }
		void updateTraversalTargetPosition(const Position& position);
		bool traversalCombatTimedOut(std::chrono::steady_clock::time_point now,
		                             std::chrono::steady_clock::duration timeout) const;
		std::optional<PlayerBotTraversalTarget> takeDefeatedTraversalTarget();
		std::optional<PlayerBotTraversalTarget> clearTraversalTarget();
		void suppressTraversalTarget(uint32_t id, std::chrono::steady_clock::time_point expires);

		void beginDefensiveCombat(PlayerBotDefensiveTarget target, std::chrono::steady_clock::time_point now);
		const std::optional<PlayerBotDefensiveTarget>& defensiveTarget() const { return activeDefensiveTarget; }
		void updateDefensiveTargetPosition(const Position& position);
		bool defensiveCombatTimedOut(std::chrono::steady_clock::time_point now,
		                             std::chrono::steady_clock::duration timeout) const;
		std::optional<PlayerBotDefensiveTarget> clearDefensiveTarget();

		std::optional<PlayerBotTarget> activeTarget() const;

	private:
		std::optional<PlayerBotTraversalTarget> activeTraversalTarget;
		std::optional<PlayerBotDefensiveTarget> activeDefensiveTarget;
		std::chrono::steady_clock::time_point traversalCombatStarted;
		std::chrono::steady_clock::time_point defensiveCombatStarted;
		std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> suppressedTraversalTargets;
};

#endif
