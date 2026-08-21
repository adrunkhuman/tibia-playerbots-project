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

struct PlayerBotCombatProfile {
	uint32_t level = 0;
	int32_t maximumHealth = 0;
	int32_t armor = 0;
	int32_t defense = 0;
	int32_t attack = 0;
	int32_t attackSkill = 0;
	float attackFactor = 1.0f;
};

struct PlayerBotRecoveryPrediction {
	double spellMinimumHealing = 0;
	double potionMinimumHealing = 0;
	double totalMinimumHealing = 0;
	uint32_t spellManaCost = 0;
	uint32_t spellCooldown = 0;
	uint32_t spellCasts = 0;
	uint32_t potionUses = 0;
	uint32_t manaReserve = 0;
	double availableBeforeLethal = 0;
	bool lightHealingLegal = false;
};

struct PlayerBotHuntPlanningProfile {
	PlayerBotCombatProfile combat;
	int32_t currentHealth = 0;
	uint32_t mana = 0;
	uint32_t magicLevel = 0;
	uint32_t potionCount = 0;
	uint32_t lightHealingManaCost = 0;
	uint32_t lightHealingCooldown = 0;
	int32_t lightHealingMinimum = 0;
	double challengeFrontier = 0;
	bool lightHealingLegal = false;
};

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
	double rawThreatRatio = 0;
	int32_t currentHealth = 0;
	double predictedFightSeconds = 0;
	double challengeFrontier = 0;
	double challengeBandMinimum = 0;
	double challengeBandMaximum = 0;
	PlayerBotRecoveryPrediction recovery;
	double score = 0;
	uint32_t travelSteps = 0;
	uint64_t expandedNodes = 0;
	bool suitable = false;
	bool reachable = false;
	bool inChallengeBand = false;
	bool predictedLethal = false;
	std::string rejectionReason;
};

struct PlayerBotHuntRegionPerformance {
	double observedExperiencePerMinute = 0;
	double correction = 1;
	uint32_t samples = 0;
};

struct PlayerBotHuntRegionScan {
	bool cacheHit = false;
	uint64_t revision = 0;
	uint64_t snapshotTimeUs = 0;
	uint64_t clusteringTimeUs = 0;
	size_t candidateCount = 0;
	std::vector<size_t> candidateIndices;
};

PlayerBotHuntPlanningProfile playerBotHuntPlanningProfile(const Player& player, const PlayerBotCombatProfile& combat,
                                                           double challengeFrontier);
PlayerBotRecoveryPrediction playerBotPredictRecovery(const PlayerBotHuntPlanningProfile& profile,
                                                      double predictedFightSeconds);
bool playerBotPredictedLethal(int32_t currentHealth, double predictedDamage);
bool playerBotPreferHuntRegion(const PlayerBotHuntRegion& left, const PlayerBotHuntRegion& right);
bool playerBotHuntScopeExhausted(const std::vector<PlayerBotHuntRegion>& regions);

class PlayerBotHuntRegionPlanner
{
	public:
		static void invalidateCache();
		static uint64_t getCacheRevision();
		PlayerBotHuntRegionScan beginScan(const Player& player) const;
		bool score(Player& player, const PlayerBotHuntPlanningProfile& profile, uint64_t revision, size_t candidateIndex,
		           const std::set<Position>& excludedRegions,
		           const std::map<Position, PlayerBotHuntRegionPerformance>& performance,
		           uint32_t huntDurationSeconds, PlayerBotHuntRegion& region) const;
};

#endif
