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
#include "playerbothunteconomy.h"
#include "playerbotsupplypolicy.h"
#include "position.h"

#include <algorithm>
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
	bool cashPressure = false;
	bool diversifyIncomeRoutes = false;
	PlayerBotSupplyProfile supply;
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
	uint64_t atlasSiteId = 0;
	uint64_t atlasVariantId = 0;
	uint32_t atlasPocketCount = 0;
	uint32_t atlasSpawnCount = 0;
	uint32_t atlasFloorCount = 0;
	uint8_t floor = 0;
	Position center;
	Position destination;
	std::vector<Position> patrolPoints;
	std::vector<PlayerBotHuntMonsterProfile> monsters;
	double coinGoldPerMinute = 0;
	bool cashPressure = false;
	// Synthetic policy fixtures have no atlas geometry; engine candidates set this.
	bool sustainedEligible = true;
	PlayerBotHuntViability viability;
	double experiencePerMinute = 0;
	double spawnExperiencePerMinute = 0;
	double clearExperiencePerMinute = 0;
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
	PlayerBotSupplyProfile supplyProfile;
	PlayerBotSupplyBudget supplyBudget;
	double expectedDamagePerSecond = 0;
	double combatFraction = 0;
	uint32_t returnRouteDangerCost = 0;
	bool routeValidated = false;
	double score = 0;
	uint32_t travelSteps = 0;
	uint32_t topologyTravelSteps = 0;
	uint32_t routeDangerCost = 0;
	double maximumRouteDanger = 0;
	bool suitable = false;
	bool reachable = false;
	bool topologyReachable = false;
	bool inChallengeBand = false;
	bool predictedLethal = false;
	std::string rejectionReason;

	void reconcileTravel(double durationSeconds, double travelSeconds, double staminaMultiplier)
	{
		estimatedTravelSeconds = travelSeconds;
		availableHuntSeconds = std::max(0.0, durationSeconds - travelSeconds);
		staminaExperienceMultiplier = staminaMultiplier;
		projectedExperience = experiencePerMinute * observedCorrection *
		                      staminaExperienceMultiplier * availableHuntSeconds / 60.0;
		// XP is the tie-breaker within the duration-budget preference tier.
		score = projectedExperience;
		reconcileSupplies(supplyProfile.reserve);
	}

	void reconcileRecovery(uint32_t reserve, uint64_t funds, uint64_t spendingReserve)
	{
		reconcileSupplies(reserve);
		cashPressure = playerBotHuntCashPressure(supplyProfile.potions, reserve, funds, spendingReserve);
	}

	void reconcileSupplies(uint32_t reserve)
	{
		supplyProfile.reserve = reserve;
		supplyBudget = playerBotSupplyBudget(supplyProfile, expectedDamagePerSecond, combatFraction,
		                                    availableHuntSeconds, estimatedTravelSeconds);
	}
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

struct PlayerBotHuntAtlasSummary {
	uint64_t revision = 0;
	uint64_t topologyGeneration = 0;
	uint64_t spawnGeneration = 0;
	uint64_t buildTimeUs = 0;
	size_t spawnCount = 0;
	size_t pocketCount = 0;
	size_t siteCount = 0;
	size_t variantCount = 0;
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
inline bool playerBotPreferHuntRegion(const PlayerBotHuntRegion& left, const PlayerBotHuntRegion& right)
{
	const bool leftAvailable = left.suitable && left.reachable;
	const bool rightAvailable = right.suitable && right.reachable;
	if (leftAvailable != rightAvailable) return leftAvailable;
	if (left.supplyBudget.fits != right.supplyBudget.fits) return left.supplyBudget.fits;
	if (!left.supplyBudget.fits && left.supplyBudget.expectedPotions != right.supplyBudget.expectedPotions) {
		return left.supplyBudget.expectedPotions < right.supplyBudget.expectedPotions;
	}
	const double leftIncome = left.cashPressure ? left.coinGoldPerMinute : 0;
	const double rightIncome = right.cashPressure ? right.coinGoldPerMinute : 0;
	if (leftIncome != rightIncome) return leftIncome > rightIncome;
	return left.score > right.score;
}
inline constexpr size_t playerBotMaximumHuntRouteCandidates = 8;

// Input retains the planner's normal stable ordering and region IDs. Diversify
// only an oversubscribed supply tier: income must not displace a safer budget.
inline std::vector<PlayerBotHuntRegion> playerBotHuntRouteShortlist(
    const std::vector<PlayerBotHuntRegion>& orderedRegions, bool diversifyIncome)
{
	std::vector<const PlayerBotHuntRegion*> eligible, chosen;
	for (const auto& region : orderedRegions) {
		if (region.suitable && region.reachable) eligible.push_back(&region);
	}
	for (size_t begin = 0; begin < eligible.size() && chosen.size() < playerBotMaximumHuntRouteCandidates;) {
		size_t end = begin + 1;
		const auto& tier = eligible[begin]->supplyBudget;
		while (end < eligible.size() && eligible[end]->supplyBudget.fits == tier.fits &&
		       (tier.fits || eligible[end]->supplyBudget.expectedPotions == tier.expectedPotions)) ++end;
		const size_t slots = playerBotMaximumHuntRouteCandidates - chosen.size();
		const bool diversify = diversifyIncome && end - begin > slots;
		const size_t normalSlots = diversify ? (slots + 1) / 2 : std::min(slots, end - begin);
		chosen.insert(chosen.end(), eligible.begin() + begin, eligible.begin() + begin + normalSlots);
		if (diversify) {
			std::vector<const PlayerBotHuntRegion*> income(eligible.begin() + begin, eligible.begin() + end);
			std::stable_sort(income.begin(), income.end(), [](const auto* left, const auto* right) {
				return left->coinGoldPerMinute > right->coinGoldPerMinute;
			});
			for (const auto* region : income) {
				if (chosen.size() == playerBotMaximumHuntRouteCandidates) break;
				if (std::find(chosen.begin(), chosen.end(), region) == chosen.end()) chosen.push_back(region);
			}
		}
		begin = end;
	}
	std::vector<PlayerBotHuntRegion> result;
	for (const auto* region : chosen) result.push_back(*region);
	return result;
}

inline const char* playerBotHuntSelectionRule(const PlayerBotHuntRegion& region)
{
	if (region.cashPressure) return region.supplyBudget.fits ?
	    "supply_budget_then_coin_income_then_xp" : "lowest_potion_consumption_then_coin_income_then_xp";
	return region.supplyBudget.fits ? "supply_budget_then_xp" : "lowest_potion_consumption_then_xp";
}
inline bool playerBotHuntScopeExhausted(const std::vector<PlayerBotHuntRegion>& regions)
{
	return std::none_of(regions.begin(), regions.end(), [](const PlayerBotHuntRegion& region) {
		return region.suitable && region.reachable;
	});
}

class PlayerBotHuntRegionPlanner
{
	public:
		static void invalidateCache();
		static PlayerBotHuntAtlasSummary rebuildAtlas();
		static PlayerBotHuntAtlasSummary atlasSummary();
		static uint64_t getCacheRevision();
		PlayerBotHuntRegionScan beginScan(Player& player,
		                                  const PlayerBotTopologyDistances* topologyDistances = nullptr) const;
		PlayerBotHuntRegionScore score(Player& player, const PlayerBotHuntPlanningProfile& profile, uint64_t revision,
		                               size_t candidateIndex, const std::set<uint64_t>& excludedVariants,
		                               const std::map<uint64_t, PlayerBotHuntRegionPerformance>& performance,
		                               uint32_t huntDurationSeconds,
		                               const PlayerBotTopologyDistances* topologyDistances = nullptr) const;
};

#endif
