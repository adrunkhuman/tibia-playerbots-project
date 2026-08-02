/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef FS_PLAYERBOTHUNTREGIONS_H
#define FS_PLAYERBOTHUNTREGIONS_H

#include "position.h"

#include <cstdint>
#include <map>
#include <string>
#include <set>
#include <vector>

class Player;
class PlayerBotNavigator;

struct PlayerBotHuntMonsterProfile {
	std::string name;
	double expectedSpawns = 0;
	uint64_t experience = 0;
	int32_t health = 0;
	double expectedDamagePerSecond = 0;
	double predictedFightDamage = 0;
};

struct PlayerBotHuntRegion {
	uint32_t id = 0;
	uint8_t floor = 0;
	Position center;
	Position destination;
	std::vector<Position> patrolPoints;
	std::vector<PlayerBotHuntMonsterProfile> monsters;
	double experiencePerMinute = 0;
	double estimatedTravelSeconds = 0;
	double availableHuntSeconds = 0;
	double observedExperiencePerMinute = 0;
	double observedCorrection = 1;
	uint16_t staminaMinutes = 0;
	double staminaExperienceMultiplier = 1;
	double projectedExperience = 0;
	double threatRatio = 0;
	double score = 0;
	uint32_t travelSteps = 0;
	uint64_t expandedNodes = 0;
	bool suitable = false;
	bool reachable = false;
	std::string rejectionReason;
};

struct PlayerBotHuntRegionPerformance {
	double observedExperiencePerMinute = 0;
	double correction = 1;
	uint32_t samples = 0;
};

class PlayerBotHuntRegionPlanner
{
	public:
		std::vector<PlayerBotHuntRegion> evaluate(Player& player, const PlayerBotNavigator& navigator,
		                                              const std::set<Position>& excludedRegions,
		                                              const std::map<Position, PlayerBotHuntRegionPerformance>& performance,
		                                              uint32_t huntDurationSeconds) const;
};

#endif
