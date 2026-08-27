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

#include "playerbotcombatprofile.h"
#include "position.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <set>
#include <vector>

class Player;
struct PlayerBotTopologyDistances;

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
	int32_t potionMinimumHealing = 0;
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

struct PlayerBotHuntCorridorDanger {
	bool available = false;
	uint32_t sampledPositions = 0;
	uint32_t nearbySpawnBlocks = 0;
	double dangerRatio = 0;
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
	double optimisticProjectedExperience = 0;
	double threatRatio = 0;
	double rawThreatRatio = 0;
	double corridorDangerRatio = 0;
	uint32_t corridorSampleCount = 0;
	uint32_t corridorSpawnBlocks = 0;
	bool corridorDangerAvailable = false;
	int32_t currentHealth = 0;
	double predictedFightSeconds = 0;
	double challengeFrontier = 0;
	double challengeBandMinimum = 0;
	double challengeBandMaximum = 0;
	PlayerBotRecoveryPrediction recovery;
	double score = 0;
	uint32_t travelSteps = 0;
	uint32_t topologyTravelSteps = 0;
	uint64_t expandedNodes = 0;
	bool suitable = false;
	bool reachable = false;
	bool topologyReachable = false;
	bool routeValidationAttempted = false;
	bool routeValidationDeferredByBound = false;
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

struct PlayerBotHuntRegionScore {
	bool valid = false;
	bool candidateFactsAvailable = false;
	bool withinPlanningScope = false;
	PlayerBotHuntRegion region;
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
		PlayerBotHuntRegionScan beginScan(Player& player,
		                                  const PlayerBotTopologyDistances* topologyDistances = nullptr) const;
		PlayerBotHuntRegionScore score(Player& player, const PlayerBotHuntPlanningProfile& profile, uint64_t revision,
		                               size_t candidateIndex, const std::set<Position>& excludedRegions,
		                               const std::map<Position, PlayerBotHuntRegionPerformance>& performance,
		                               uint32_t huntDurationSeconds,
		                               const PlayerBotTopologyDistances* topologyDistances = nullptr) const;
};

#endif
