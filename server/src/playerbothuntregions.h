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
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <set>
#include <vector>

class Player;
struct PlayerBotTopologyDistances;
class PlayerBotTopologyReachability;

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

struct PlayerBotHuntTransportArrival {
	Position position;
	uint64_t fare = 0;
	uint32_t estimatedSteps = 0;
	std::shared_ptr<const PlayerBotTopologyReachability> topologyReachability;
};

struct PlayerBotHuntTransportOffer {
	Position provider;
	Position destination;
	uint32_t price = 0;
	uint32_t minimumLevel = 0;
	bool premiumRequired = false;
	bool opaqueCondition = false;
	bool opaqueAction = false;
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
	bool supplyRecovery = false;
	std::array<uint16_t, playerBotSupplyEquipmentSlotCount> equipmentItemIds{};
	std::vector<PlayerBotHuntTransportArrival> transportArrivals;
	PlayerBotSupplyProfile supply;
};

inline PlayerBotSupplyCapabilitySnapshot playerBotSupplyCapability(const PlayerBotHuntPlanningProfile& profile)
{
	PlayerBotSupplyCapabilitySnapshot capability;
	capability.level = profile.combat.level;
	capability.maximumHealth = profile.combat.maximumHealth;
	capability.armor = profile.combat.armor;
	capability.defense = profile.combat.defense;
	capability.attack = profile.combat.attack;
	capability.attackSkill = profile.combat.attackSkill;
	capability.attackFactorMilli = static_cast<int32_t>(profile.combat.attackFactor * 1000);
	capability.magicLevel = profile.magicLevel;
	capability.maximumMana = profile.supply.maximumMana;
	capability.spellLegal = profile.supply.spellLegal;
	capability.spellHealing = profile.supply.spellHealing;
	capability.spellMana = profile.supply.spellMana;
	capability.spellIntervalMilliseconds = static_cast<uint32_t>(profile.supply.spellInterval * 1000);
	capability.potionHealing = profile.supply.potionHealing;
	capability.foodActive = profile.supply.regenerationSeconds > 0;
	capability.foodHealthGain = profile.supply.healthGain;
	capability.foodHealthIntervalMilliseconds = static_cast<uint32_t>(profile.supply.healthInterval * 1000);
	capability.foodManaGain = profile.supply.manaGain;
	capability.foodManaIntervalMilliseconds = static_cast<uint32_t>(profile.supply.manaInterval * 1000);
	capability.equipmentItemIds = profile.equipmentItemIds;
	return capability;
}

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

struct PlayerBotSharedSupplyEstimate {
	double observedPotionsPerCombatSecond = 0;
	double upwardPotionsPerCombatSecond = 0;
	double weight = 0;
	uint32_t contributingAreas = 0;
	uint32_t downwardContributingAreas = 0;
	uint8_t targetAttackerOverlap = 0;
	uint8_t minimumObservedAttackerCoverage = 0;
	double minimumObservedAttackerCoverageSeconds = 0;
	const char* reason = "no_shared_evidence";
	bool available = false;
};

struct PlayerBotHuntRegion {
	uint32_t id = 0;
	uint64_t atlasSiteId = 0;
	uint64_t atlasVariantId = 0;
	uint64_t atlasRevision = 0;
	uint32_t atlasPocketCount = 0;
	uint32_t atlasSpawnCount = 0;
	uint32_t atlasFloorCount = 0;
	uint8_t floor = 0;
	Position center;
	Position destination;
	std::vector<Position> patrolPoints;
	std::vector<PlayerBotHuntMonsterProfile> monsters;
	double coinGoldPerMinute = 0;
	double observedCoinGoldPerMinute = 0;
	bool cashPressure = false;
	bool supplyRecovery = false;
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
	uint32_t calibrationSampleCount = 0;
	const char* calibrationSource = "default";
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
	int32_t maximumHealth = 0;
	double predictedFightSeconds = 0;
	double challengeFrontier = 0;
	double challengeBandMinimum = 0;
	double challengeBandMaximum = 0;
	PlayerBotRecoveryPrediction recovery;
	PlayerBotSupplyProfile supplyProfile;
	PlayerBotSupplyBudget supplyBudget;
	PlayerBotSupplyCalibration supplyCalibration;
	PlayerBotSharedSupplyEstimate sharedSupplyEstimate;
	PlayerBotSupplyCapabilitySnapshot supplyCapability;
	const char* supplyEstimateSource = "static";
	const char* supplyEstimateReason = "static_duration_budget";
	double supplyAppliedPotionsPerCombatSecond = 0;
	double expectedDamagePerSecond = 0;
	double combatFraction = 0;
	uint8_t modeledMaximumAttackerOverlap = 0;
	uint32_t returnRouteDangerCost = 0;
	double recoveryRouteHealthLoss = 0;
	uint64_t outboundFare = 0;
	uint64_t exitFare = 0;
	uint64_t supplyFare = 0;
	uint32_t recoveryPotionReserve = 0;
	Position exitDepotDestination;
	Position supplyDestination;
	bool outboundNpcTravel = false;
	bool exitNpcTravel = false;
	bool supplyNpcTravel = false;
	bool routeValidated = false;
	double score = 0;
	uint32_t travelSteps = 0;
	uint32_t topologyTravelSteps = 0;
	uint32_t routeDangerCost = 0;
	double maximumRouteDanger = 0;
	bool suitable = false;
	bool reachable = false;
	bool topologyReachable = false;
	bool transportPlausible = true;
	bool inChallengeBand = false;
	bool predictedLethal = false;
	std::string rejectionReason;

	bool recoverySustainable() const
	{
		return !supplyRecovery || (coinGoldPerMinute > 0 && supplyBudget.fits &&
		    availableHuntSeconds > 0 && currentHealth >= maximumHealth * 0.8);
	}

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
		if (supplyRecovery && supplyProfile.potions <= reserve) {
			// A short recovery outing may spend only health above the 80% floor.
			const double healthBudget = std::max(0.0, currentHealth - maximumHealth * 0.8 - recoveryRouteHealthLoss);
			const double deficit = std::max(0.0, supplyBudget.expectedDamage - supplyBudget.regenerationHealing -
			    supplyBudget.spellHealing - healthBudget);
			supplyBudget.expectedPotions = deficit == 0 ? 0 : supplyProfile.potionHealing > 0 ?
			    std::ceil(deficit / supplyProfile.potionHealing) : std::numeric_limits<double>::max();
			supplyBudget.fits = supplyBudget.expectedPotions == 0 &&
			    recoveryRouteHealthLoss <= currentHealth - maximumHealth * 0.8;
		}
		const double exposure = availableHuntSeconds * std::clamp(combatFraction, 0.0, 1.0);
		supplyEstimateSource = "static";
		supplyEstimateReason = sharedSupplyEstimate.reason;
		supplyAppliedPotionsPerCombatSecond = exposure > 0 ? supplyBudget.expectedPotions / exposure : 0;
		bool observedEstimate = false;
		if (playerBotSupplyCalibrationForCapability(supplyCalibration, supplyCapability)) {
			observedEstimate = true;
			supplyEstimateSource = "local";
			supplyEstimateReason = "local_variant_evidence";
			supplyAppliedPotionsPerCombatSecond = supplyCalibration.potionsPerCombatSecond;
		} else if (sharedSupplyEstimate.available) {
			observedEstimate = true;
			supplyEstimateSource = "shared";
			supplyEstimateReason = sharedSupplyEstimate.reason;
			const double sharedRate = supplyAppliedPotionsPerCombatSecond * (1 - sharedSupplyEstimate.weight) +
			    sharedSupplyEstimate.observedPotionsPerCombatSecond * sharedSupplyEstimate.weight;
			supplyAppliedPotionsPerCombatSecond = std::max(
			    sharedRate, sharedSupplyEstimate.upwardPotionsPerCombatSecond);
		}
		if (observedEstimate) {
			supplyBudget.expectedPotions = std::ceil(supplyAppliedPotionsPerCombatSecond * exposure);
			supplyBudget.fits = (supplyRecovery && supplyBudget.expectedPotions == 0 &&
			    recoveryRouteHealthLoss <= currentHealth - maximumHealth * 0.8) ||
			    ((supplyProfile.potions > reserve || reserve == 0) &&
			     supplyBudget.expectedPotions <= supplyBudget.routinePotions);
		}
	}
};

inline constexpr uint32_t playerBotSharedSupplyMinimumSafeSamples = 2;
inline constexpr double playerBotSharedSupplyMinimumCrowdSeconds = 30;
inline constexpr double playerBotSharedSupplyMinimumCrowdRatio = 0.25;

struct PlayerBotHuntRegionPerformance {
	double observedExperiencePerMinute = 0;
	double observedCoinGoldPerMinute = 0;
	double correction = 1;
	uint32_t samples = 0;
	uint64_t atlasRevision = 0;
	bool reliable = false;
	PlayerBotSupplyCalibration supply;
	std::string supplySpecies{};
	uint64_t supplyAtlasSiteId = 0;
	uint32_t supplySafeSamples = 0;
	uint8_t supplyAttackerCoverage = 0;
	double supplyAttackerCoverageSeconds = 0;
	bool supplyUpwardEvidence = false;
};

inline PlayerBotSharedSupplyEstimate playerBotSharedSupplyEstimateForRegion(
    const PlayerBotHuntRegion& target,
    const std::map<uint64_t, PlayerBotHuntRegionPerformance>& performance)
{
	PlayerBotSharedSupplyEstimate result;
	result.targetAttackerOverlap = target.modeledMaximumAttackerOverlap;
	if (target.monsters.size() != 1) {
		result.reason = "composition_not_single_species";
		return result;
	}
	if (target.modeledMaximumAttackerOverlap == 0) {
		result.reason = "target_crowd_unmodeled";
		return result;
	}
	bool matchingSpecies = false;
	bool matchingRevision = false;
	bool compatibleCapability = false;
	bool qualifyingEvidence = false;
	bool downwardCrowdRejected = false;
	struct SiteEvidence {
		double downwardRate = 0;
		double upwardRate = 0;
		uint8_t minimumCoverage = std::numeric_limits<uint8_t>::max();
		double minimumCoverageSeconds = std::numeric_limits<double>::max();
		bool downward = false;
		bool upward = false;
	};
	std::map<uint64_t, SiteEvidence> evidenceBySite;
	for (const auto& [variantId, observed] : performance) {
		if (variantId == target.atlasVariantId || observed.supplySpecies != target.monsters.front().name) continue;
		matchingSpecies = true;
		if (observed.atlasRevision != target.atlasRevision) continue;
		matchingRevision = true;
		if (!playerBotSupplyCalibrationForCapability(observed.supply, target.supplyCapability)) continue;
		compatibleCapability = true;
		const bool safeEvidence =
		    observed.supplySafeSamples >= playerBotSharedSupplyMinimumSafeSamples;
		if (!safeEvidence && !observed.supplyUpwardEvidence) continue;
		qualifyingEvidence = true;
		if (!std::isfinite(observed.supply.potionsPerCombatSecond) ||
		    observed.supply.potionsPerCombatSecond < 0) continue;
		const bool downwardEligible = safeEvidence &&
		    observed.supplyAttackerCoverage >= target.modeledMaximumAttackerOverlap;
		if (safeEvidence && !downwardEligible) downwardCrowdRejected = true;
		if (!downwardEligible && !observed.supplyUpwardEvidence) continue;
		SiteEvidence& site = evidenceBySite[observed.supplyAtlasSiteId];
		if (downwardEligible) {
			// Overlapping pocket/neighborhood variants describe one physical site.
			// Keep one conservative site vote rather than treating those variants
			// as independent confidence.
			site.downward = true;
			site.downwardRate = std::max(site.downwardRate, observed.supply.potionsPerCombatSecond);
			site.minimumCoverage = std::min(site.minimumCoverage, observed.supplyAttackerCoverage);
			site.minimumCoverageSeconds = std::min(
			    site.minimumCoverageSeconds, observed.supplyAttackerCoverageSeconds);
		}
		if (observed.supplyUpwardEvidence) {
			site.upward = true;
			site.upwardRate = std::max(site.upwardRate, observed.supply.potionsPerCombatSecond);
		}
	}
	double totalRate = 0;
	uint8_t minimumCoverage = std::numeric_limits<uint8_t>::max();
	double minimumCoverageSeconds = std::numeric_limits<double>::max();
	for (const auto& [siteId, site] : evidenceBySite) {
		(void)siteId;
		if (!site.downward && !site.upward) continue;
		++result.contributingAreas;
		if (site.downward) {
			totalRate += site.downwardRate;
			minimumCoverage = std::min(minimumCoverage, site.minimumCoverage);
			minimumCoverageSeconds = std::min(minimumCoverageSeconds, site.minimumCoverageSeconds);
			++result.downwardContributingAreas;
		}
		if (site.upward) {
			result.upwardPotionsPerCombatSecond = std::max(
			    result.upwardPotionsPerCombatSecond, site.upwardRate);
		}
	}
	if (result.contributingAreas == 0) {
		result.reason = !matchingSpecies ? "no_matching_species_evidence" :
		                !matchingRevision ? "shared_revision_mismatch" :
		                !compatibleCapability ? "shared_capability_incompatible" :
		                !qualifyingEvidence ? "shared_evidence_insufficient" :
		                downwardCrowdRejected ? "shared_crowd_coverage_insufficient" :
		                                          "shared_evidence_invalid";
		return result;
	}
	result.available = true;
	if (result.downwardContributingAreas != 0) {
		result.observedPotionsPerCombatSecond = totalRate / result.downwardContributingAreas;
		result.weight = std::min(0.5, result.downwardContributingAreas /
		    static_cast<double>(result.downwardContributingAreas + 2));
	}
	if (result.downwardContributingAreas != 0) {
		result.minimumObservedAttackerCoverage = minimumCoverage;
		result.minimumObservedAttackerCoverageSeconds = minimumCoverageSeconds;
	}
	result.reason = result.downwardContributingAreas != 0 ?
	    "shared_single_species_evidence" : "shared_single_species_upward_evidence";
	return result;
}

struct PlayerBotHuntSharedCorrection {
	double correction = 1;
	uint32_t testedVariants = 0;
};

inline PlayerBotHuntSharedCorrection playerBotSharedHuntCorrection(
    uint64_t atlasRevision, const std::map<uint64_t, PlayerBotHuntRegionPerformance>& performance)
{
	PlayerBotHuntSharedCorrection result;
	double total = 0;
	for (const auto& entry : performance) {
		const PlayerBotHuntRegionPerformance& observed = entry.second;
		if (!observed.reliable || observed.samples == 0 || observed.atlasRevision != atlasRevision ||
		    !std::isfinite(observed.correction) ||
		    observed.correction < 0.25 || observed.correction > 2.0) continue;
		total += observed.correction;
		++result.testedVariants;
	}
	if (result.testedVariants != 0) {
		result.correction = std::clamp(total / result.testedVariants, 0.25, 2.0);
	}
	return result;
}

struct PlayerBotHuntCorrection {
	double correction = 1;
	double observedExperiencePerMinute = 0;
	double observedCoinGoldPerMinute = 0;
	uint32_t sampleCount = 0;
	const char* source = "default";
};

inline PlayerBotHuntCorrection playerBotHuntCorrectionForVariant(
    uint64_t variantId, uint64_t atlasRevision,
    const std::map<uint64_t, PlayerBotHuntRegionPerformance>& performance)
{
	if (auto found = performance.find(variantId); found != performance.end()) {
		const PlayerBotHuntRegionPerformance& observed = found->second;
		if (observed.reliable && observed.samples != 0 && observed.atlasRevision == atlasRevision &&
		    std::isfinite(observed.correction) &&
		    observed.correction >= 0.25 && observed.correction <= 2.0) {
			return {observed.correction, observed.observedExperiencePerMinute,
			        observed.observedCoinGoldPerMinute, observed.samples, "variant_observed"};
		}
	}
	const PlayerBotHuntSharedCorrection shared = playerBotSharedHuntCorrection(atlasRevision, performance);
	if (shared.testedVariants != 0) {
		return {shared.correction, 0, 0, shared.testedVariants, "shared_observed"};
	}
	return {};
}

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
	const bool recovery = left.supplyRecovery && right.supplyRecovery;
	const double leftIncome = (left.cashPressure || recovery) ?
	    (left.observedCoinGoldPerMinute > 0 ? left.observedCoinGoldPerMinute : left.coinGoldPerMinute) : 0;
	const double rightIncome = (right.cashPressure || recovery) ?
	    (right.observedCoinGoldPerMinute > 0 ? right.observedCoinGoldPerMinute : right.coinGoldPerMinute) : 0;
	if (leftIncome != rightIncome) return leftIncome > rightIncome;
	if (recovery && left.threatRatio != right.threatRatio) return left.threatRatio < right.threatRatio;
	if (left.score != right.score) return left.score > right.score;
	// A zero-duration estimate must not let an unproven disconnected region win
	// a tie merely because it forecasts no combat consumption.
	if (left.topologyReachable != right.topologyReachable) return left.topologyReachable;
	return left.estimatedTravelSeconds < right.estimatedTravelSeconds;
}
inline bool playerBotHuntTravelAffordable(uint64_t funds, uint64_t recoveryReserve,
                                          uint64_t outboundFare, uint64_t exitFare,
                                          uint64_t supplyFare = 0)
{
	uint64_t totalFare = outboundFare > UINT64_MAX - exitFare ? UINT64_MAX : outboundFare + exitFare;
	totalFare = totalFare > UINT64_MAX - supplyFare ? UINT64_MAX : totalFare + supplyFare;
	if (totalFare == 0) return true;
	return totalFare <= funds && recoveryReserve <= funds - totalFare;
}

enum class PlayerBotHuntTravelBudgetPhase : uint8_t {
	None,
	Outbound,
	ReturnToDepot,
	Supply,
};

inline uint64_t playerBotHuntTravelRecoveryReserve(PlayerBotHuntTravelBudgetPhase phase,
                                                   uint64_t recoveryReserve)
{
	return phase == PlayerBotHuntTravelBudgetPhase::Supply ? recoveryReserve : 0;
}

inline uint64_t playerBotHuntTravelReturnFareReserve(PlayerBotHuntTravelBudgetPhase phase,
                                                     uint64_t returnFare)
{
	return phase == PlayerBotHuntTravelBudgetPhase::Outbound ? returnFare : 0;
}

inline bool playerBotHuntTravelPaymentAffordable(uint64_t funds, uint64_t recoveryReserve,
                                                 uint64_t fare, uint64_t returnFare,
                                                 PlayerBotHuntTravelBudgetPhase phase)
{
	if (phase == PlayerBotHuntTravelBudgetPhase::None ||
	    phase == PlayerBotHuntTravelBudgetPhase::ReturnToDepot) {
		return fare <= funds;
	}
	return playerBotHuntTravelAffordable(funds,
	    playerBotHuntTravelRecoveryReserve(phase, recoveryReserve), fare,
	    playerBotHuntTravelReturnFareReserve(phase, returnFare));
}

inline const char* playerBotHuntTravelBudgetPhaseName(PlayerBotHuntTravelBudgetPhase phase)
{
	switch (phase) {
		case PlayerBotHuntTravelBudgetPhase::Outbound: return "outbound";
		case PlayerBotHuntTravelBudgetPhase::ReturnToDepot: return "return_to_depot";
		case PlayerBotHuntTravelBudgetPhase::Supply: return "supply";
		default: return "none";
	}
}

inline const char* playerBotHuntTravelFareRejectionReason(PlayerBotHuntTravelBudgetPhase phase)
{
	switch (phase) {
		case PlayerBotHuntTravelBudgetPhase::Outbound: return "fare_breaks_return_reserve";
		case PlayerBotHuntTravelBudgetPhase::Supply: return "fare_breaks_restock_reserve";
		default: return "fare_unaffordable";
	}
}

struct PlayerBotHuntReturnCoverageContext {
	uint64_t topologyGeneration = 0;
	uint64_t npcGeneration = 0;
	uint32_t level = 0;
	Position coveredPosition;
	Position depotDestination;
	uint64_t fare = 0;
	bool canUseRope = false;
	bool canUseShovel = false;
	bool premium = false;

	bool operator==(const PlayerBotHuntReturnCoverageContext& other) const
	{
		return topologyGeneration == other.topologyGeneration && npcGeneration == other.npcGeneration &&
		       level == other.level && coveredPosition == other.coveredPosition &&
		       depotDestination == other.depotDestination && fare == other.fare &&
		       canUseRope == other.canUseRope && canUseShovel == other.canUseShovel && premium == other.premium;
	}
};

// Region selection validates a safe route from one patrol position to a depot.
// Coverage can move to another position only across a complete reversible local
// walking plan; geometric region membership alone is not connectivity evidence.
class PlayerBotHuntReturnCoverage
{
	public:
		void validate(uint64_t variantId, uint64_t atlasRevision,
		              const PlayerBotHuntReturnCoverageContext& context)
		{
			evidence = Evidence{variantId, atlasRevision, context, true};
		}

		bool covers(uint64_t variantId, uint64_t atlasRevision,
		            const PlayerBotHuntReturnCoverageContext& context) const
		{
			return evidence && evidence->routeValid && evidence->variantId == variantId &&
			       evidence->atlasRevision == atlasRevision && evidence->context == context;
		}

		void invalidate()
		{
			if (evidence) evidence->routeValid = false;
		}
		bool valid() const { return evidence && evidence->routeValid; }

	private:
		struct Evidence {
			uint64_t variantId = 0;
			uint64_t atlasRevision = 0;
			PlayerBotHuntReturnCoverageContext context;
			bool routeValid = false;
		};
		std::optional<Evidence> evidence;
};

inline bool playerBotHuntNeedsSupplyRoute(double expectedPotions, uint32_t availablePotions,
                                          uint32_t routeReserve)
{
	return expectedPotions > 0 || availablePotions <= routeReserve;
}

// Preserve the normal policy order across every cheap viable candidate. Route
// validation consumes this finite queue incrementally; it does not reserve
// slots for local, remote, observed, unobserved, or income candidates.
inline std::vector<PlayerBotHuntRegion> playerBotHuntRouteCandidates(
    const std::vector<PlayerBotHuntRegion>& orderedRegions)
{
	std::vector<PlayerBotHuntRegion> result;
	for (const PlayerBotHuntRegion& region : orderedRegions) {
		if (region.suitable && region.reachable) result.push_back(region);
	}
	return result;
}

inline bool playerBotHuntCandidateCanBeatValidated(
    const PlayerBotHuntRegion& candidate, const PlayerBotHuntRegion& validated)
{
	if (!candidate.suitable || !candidate.reachable) return false;
	// Travel can improve the candidate's supply tier, but it cannot improve past
	// an incumbent that already fits. Compare that best case with the candidate's
	// no-travel XP bound before spending another route budget.
	if (!validated.supplyBudget.fits) return true;
	if (validated.cashPressure && candidate.coinGoldPerMinute > validated.coinGoldPerMinute) return true;
	if (validated.cashPressure && candidate.coinGoldPerMinute < validated.coinGoldPerMinute) return false;
	return candidate.optimisticProjectedExperience >= validated.score;
}

inline size_t playerBotNextHuntCandidateToValidate(
    const std::vector<PlayerBotHuntRegion>& candidates, size_t begin,
    const PlayerBotHuntRegion* validated = nullptr)
{
	for (size_t index = std::min(begin, candidates.size()); index < candidates.size(); ++index) {
		const PlayerBotHuntRegion& candidate = candidates[index];
		if (!candidate.suitable || !candidate.reachable) continue;
		if (!validated || playerBotHuntCandidateCanBeatValidated(candidate, *validated)) return index;
	}
	return candidates.size();
}

inline bool playerBotHuntRemainingCanBeatValidated(
    const std::vector<PlayerBotHuntRegion>& candidates, size_t begin,
    const PlayerBotHuntRegion& validated)
{
	return playerBotNextHuntCandidateToValidate(candidates, begin, &validated) < candidates.size();
}

inline const char* playerBotHuntSelectionRule(const PlayerBotHuntRegion& region)
{
	if (region.supplyRecovery) return region.supplyBudget.fits ?
	    "supply_recovery_coin_safety_then_xp" : "supply_recovery_potions_then_coin_safety_then_xp";
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
