/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef FS_PLAYERBOTCOMBATTARGET_H
#define FS_PLAYERBOTCOMBATTARGET_H

#include <algorithm>
#include <cstdint>
#include <string>

#include "position.h"

struct PlayerBotExpectedCorpse {
	uint16_t itemId = 0;
	bool lootable = false;
};

struct PlayerBotTarget {
	uint32_t id = 0;
	Position position;
	std::string name;
};

struct PlayerBotTraversalTarget : PlayerBotTarget {
	PlayerBotExpectedCorpse expectedCorpse;
};

struct PlayerBotDefensiveTarget : PlayerBotTarget {
	bool routeCritical = false;
	bool intendedStep = false;
	double predictedFightDamage = 0;
	double predictedFightSeconds = 0;
};

struct PlayerBotTraversalCandidate : PlayerBotTarget {
	PlayerBotExpectedCorpse expectedCorpse;
	bool attacksPlayer = false;
};

inline bool playerBotPassageFightManageable(int32_t currentHealth, uint32_t minimumRecovery,
                                             double incomingDamagePerSecond, double fightSeconds)
{
	return currentHealth > 0 && incomingDamagePerSecond >= 0 && fightSeconds > 0 &&
	       incomingDamagePerSecond * fightSeconds < currentHealth + minimumRecovery;
}

inline bool playerBotPreferDefensiveTarget(const PlayerBotDefensiveTarget& left,
                                           const PlayerBotDefensiveTarget& right,
                                           const Position& currentPosition)
{
	if (left.routeCritical != right.routeCritical) return left.routeCritical;
	if (left.intendedStep != right.intendedStep) return left.intendedStep;
	if (left.predictedFightDamage != right.predictedFightDamage) {
		return left.predictedFightDamage < right.predictedFightDamage;
	}
	if (left.predictedFightSeconds != right.predictedFightSeconds) {
		return left.predictedFightSeconds < right.predictedFightSeconds;
	}
	const uint32_t leftDistance = std::max(Position::getDistanceX(currentPosition, left.position),
	                                       Position::getDistanceY(currentPosition, left.position));
	const uint32_t rightDistance = std::max(Position::getDistanceX(currentPosition, right.position),
	                                        Position::getDistanceY(currentPosition, right.position));
	return leftDistance == rightDistance ? left.id < right.id : leftDistance < rightDistance;
}

#endif
