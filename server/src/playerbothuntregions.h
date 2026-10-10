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
#include "const.h"
#include "playerbothunteconomy.h"
#include "playerbotsupplypolicy.h"
#include "playerbotsupplyrecovery.h"
#include "playerbotpreparation.h"
#include "position.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
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
	bool foodAvailable = false;
	// Supply compatibility uses a stable equipment/skill defense value. The
	// combat profile keeps the engine's live defense for actual hunt scoring.
	int32_t supplyCapabilityDefense = -1;
	std::array<uint16_t, playerBotSupplyEquipmentSlotCount> equipmentItemIds{};
	std::vector<PlayerBotHuntTransportArrival> transportArrivals;
	PlayerBotSupplyProfile supply;
	uint32_t healthPotionUnitPrice = 0; // Zero means no known purchase quote.
	PlayerBotSupplyGlobalLearning supplyGlobalLearning;
};

inline PlayerBotSupplyCapabilitySnapshot playerBotSupplyCapability(const PlayerBotHuntPlanningProfile& profile)
{
	PlayerBotSupplyCapabilitySnapshot capability;
	capability.level = profile.combat.level;
	capability.maximumHealth = profile.combat.maximumHealth;
	capability.armor = profile.combat.armor;
	capability.defense = profile.supplyCapabilityDefense >= 0 ?
	    profile.supplyCapabilityDefense : profile.combat.defense;
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
	capability.foodAvailable = profile.foodAvailable;
	capability.foodHealthGain = profile.supply.healthGain;
	capability.foodHealthIntervalMilliseconds = static_cast<uint32_t>(profile.supply.healthInterval * 1000);
	capability.foodManaGain = profile.supply.manaGain;
	capability.foodManaIntervalMilliseconds = static_cast<uint32_t>(profile.supply.manaInterval * 1000);
	capability.equipmentItemIds = profile.equipmentItemIds;
	// Legacy equipment slots can hold coins. Banking or denomination changes
	// alter liquidity, not combat capability; real ammunition still matters.
	for (auto& itemId : capability.equipmentItemIds) {
		if (itemId == ITEM_GOLD_COIN || itemId == ITEM_PLATINUM_COIN || itemId == ITEM_CRYSTAL_COIN) itemId = 0;
	}
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

enum class PlayerBotHuntProfitabilityHint : uint8_t { NotApplicable, Uncertain, LikelySurplus };

inline const char* playerBotHuntProfitabilityHintName(PlayerBotHuntProfitabilityHint hint)
{
	switch (hint) {
		case PlayerBotHuntProfitabilityHint::LikelySurplus: return "likely_surplus";
		case PlayerBotHuntProfitabilityHint::Uncertain: return "uncertain";
		default: return "not_applicable";
	}
}

struct PlayerBotHuntEconomicKey {
	bool cashPressure = false;
	bool supplyRecovery = false;
	PlayerBotHuntProfitabilityHint profitabilityHint = PlayerBotHuntProfitabilityHint::NotApplicable;
	double coinGoldPerMinute = 0;
};

inline int playerBotCompareHuntEconomicKeys(const PlayerBotHuntEconomicKey& left,
                                          const PlayerBotHuntEconomicKey& right)
{
	const bool leftSurplus = left.profitabilityHint == PlayerBotHuntProfitabilityHint::LikelySurplus;
	const bool rightSurplus = right.profitabilityHint == PlayerBotHuntProfitabilityHint::LikelySurplus;
	if (leftSurplus != rightSurplus) return leftSurplus ? 1 : -1;
	if (left.coinGoldPerMinute != right.coinGoldPerMinute) return left.coinGoldPerMinute > right.coinGoldPerMinute ? 1 : -1;
	return 0;
}

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
	std::optional<PlayerBotHuntEconomicKey> frozenEconomicKey;
	// Synthetic policy fixtures have no atlas geometry; engine candidates set this.
	bool sustainedEligible = true;
	PlayerBotHuntViability viability;
	double experiencePerMinute = 0;
	double spawnExperiencePerMinute = 0;
	double clearExperiencePerMinute = 0;
	double estimatedTravelSeconds = 0;
	double estimatedReturnSeconds = 0;
	double estimatedSupplySeconds = 0;
	double availableHuntSeconds = 0;
	double maximumHuntSeconds = 0;
	uint32_t healthPotionUnitPrice = 0;
	double observedExperiencePerMinute = 0;
	double observedCorrection = 1;
	uint32_t calibrationSampleCount = 0;
	const char* calibrationSource = "default";
	uint16_t staminaMinutes = 0;
	double staminaExperienceMultiplier = 1;
	double projectedExperience = 0;
	double optimisticProjectedExperience = 0;
	// Best score this candidate could reach while fitting its supply budget;
	// negative when no hunt length fits. Infinity means not computed.
	double optimisticFittingExperience = std::numeric_limits<double>::infinity();
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
	std::vector<PlayerBotSupplyKindBudget> supplyKindBudgets;
	PlayerBotSupplyCalibration supplyCalibration;
	PlayerBotSupplyGlobalLearning supplyGlobalLearning;
	PlayerBotSupplyCapabilitySnapshot supplyCapability;
	const char* supplyEstimateSource = "static";
	const char* supplyEstimateReason = "static_duration_budget";
	const char* supplyLocalRejectionReason = "no_local_evidence";
	double supplyStaticPotionsPerCombatSecond = 0;
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

	bool productiveWindow() const
	{
		return availableHuntSeconds > 0 && (predictedFightSeconds <= 0 ||
		    availableHuntSeconds * std::clamp(combatFraction, 0.0, 1.0) >= predictedFightSeconds);
	}

	bool recoverySustainable() const
	{
		return !supplyRecovery || (coinGoldPerMinute > 0 && supplyBudget.fits && productiveWindow() &&
		    currentHealth >= static_cast<int32_t>(playerBotSupplyRecoveryRequiredHealth(std::max(0, maximumHealth))));
	}

	double transitCombatSeconds() const
	{
		const double travel = std::max(0.0, estimatedTravelSeconds + estimatedReturnSeconds);
		const double fraction = std::clamp(combatFraction, 0.0, 1.0);
		// Before detailed validation use the hunt's duty share. Afterwards route
		// damage translates to active encounter exposure at this hunt's DPS.
		return routeValidated && expectedDamagePerSecond > 0 ?
		    std::min(travel, std::max(0.0, recoveryRouteHealthLoss) * fraction / expectedDamagePerSecond) :
		    travel * fraction;
	}

	PlayerBotHuntEconomicKey cheapEconomicKey() const
	{
		PlayerBotHuntEconomicKey key;
		key.cashPressure = cashPressure;
		key.supplyRecovery = supplyRecovery;
		if (!cashPressure && !supplyRecovery) return key;
		key.profitabilityHint = PlayerBotHuntProfitabilityHint::Uncertain;
		const double income = observedCoinGoldPerMinute > 0 ? observedCoinGoldPerMinute : coinGoldPerMinute;
		const double productive = availableHuntSeconds;
		const double travel = std::max(0.0, estimatedTravelSeconds);
		const double seconds = productive + 2 * travel;
		if (!std::isfinite(income) || income <= 0 || !std::isfinite(productive) || productive <= 0 ||
		    !std::isfinite(estimatedTravelSeconds) || !std::isfinite(seconds)) return key;
		key.coinGoldPerMinute = income * (productive / seconds);
		if (!std::isfinite(key.coinGoldPerMinute)) key.coinGoldPerMinute = 0;

		// Hunt-only demand and known prices give a sign hint, not a net-margin
		// objective. No fare, supplier itinerary, or route-danger credit/cost.
		bool known = std::isfinite(supplyBudget.expectedPotions) && supplyBudget.expectedPotions >= 0 &&
		    supplyBudget.expectedPotions != std::numeric_limits<double>::max();
		double cost = supplyBudget.expectedPotions * healthPotionUnitPrice;
		if (supplyBudget.expectedPotions > 0 && healthPotionUnitPrice == 0) known = false;
		const double exposure = productive * std::clamp(combatFraction, 0.0, 1.0);
		if (!std::isfinite(combatFraction)) known = false;
		for (const auto& kind : supplyProfile.kinds) {
			const bool demandKnown = kind.demand.samples != 0 || kind.demand.unitsPerCombatSecond > 0 ||
			    kind.staticUnitsPerCombatSecond > 0;
			const double demand = kind.demand.samples != 0 ? kind.demand.unitsPerCombatSecond :
			    std::max(kind.demand.unitsPerCombatSecond, kind.staticUnitsPerCombatSecond);
			if (!demandKnown || !std::isfinite(demand) || demand < 0) {
				known = false;
				continue;
			}
			const double expected = std::ceil(demand * exposure);
			if (expected > 0 && kind.unitPrice == 0) known = false;
			cost += expected * kind.unitPrice;
		}
		const double gross = income * productive / 60.0;
		if (known && std::isfinite(cost) && std::isfinite(gross) && gross > cost)
			key.profitabilityHint = PlayerBotHuntProfitabilityHint::LikelySurplus;
		return key;
	}

	void freezeEconomicKey()
	{
		if (!frozenEconomicKey) frozenEconomicKey = cheapEconomicKey();
	}

	PlayerBotHuntEconomicKey economicKey() const
	{
		// Standalone synthetic policy fixtures default to the same cheap model,
		// not zero income. Engine scoring and the selector freeze before routing.
		return frozenEconomicKey ? *frozenEconomicKey : cheapEconomicKey();
	}

	double nonproductiveTripSeconds() const
	{
		return estimatedTravelSeconds + estimatedReturnSeconds + estimatedSupplySeconds;
	}

	// Find a verified fitting whole-second window, not a fixed short recovery
	// timer. Bounded probes tolerate stepped regeneration/spell budgets; they
	// may conservatively miss a narrow fit, but never authorize an untested one.
	// Coarse admission may omit unproven transit combat; final validation and
	// arrival always use the default modeled exposure and verify the real fit.
	void fitSupplyWindow(uint32_t reserve, bool optimisticTransitCombat = false)
	{
		if (maximumHuntSeconds == 0) maximumHuntSeconds = availableHuntSeconds;
		const double maximum = std::floor(std::max(0.0, maximumHuntSeconds));
		double upper = maximum;
		for (int probe = 64; probe >= 0; --probe) {
			availableHuntSeconds = std::floor(maximum * probe / 64);
			reconcileSupplies(reserve, optimisticTransitCombat);
			if (supplyBudget.fits) {
				double lower = availableHuntSeconds;
				for (int refinement = 0; refinement < 32 && upper - lower > 1; ++refinement) {
					availableHuntSeconds = std::floor((lower + upper) / 2);
					reconcileSupplies(reserve, optimisticTransitCombat);
					if (supplyBudget.fits) lower = availableHuntSeconds;
					else upper = availableHuntSeconds;
				}
				availableHuntSeconds = lower;
				break;
			}
			upper = availableHuntSeconds;
		}
		reconcileSupplies(reserve, optimisticTransitCombat);
		projectedExperience = experiencePerMinute * observedCorrection * staminaExperienceMultiplier * availableHuntSeconds / 60.0;
		score = projectedExperience;
	}

	// Probe the same fitting window at higher health after route proof. Never
	// lower the recovery floor or invent regeneration from a prospective meal.
	std::optional<PlayerBotPreparationRequirement> healthPreparation(uint32_t reserve) const
	{
		if (!supplyRecovery || maximumHealth <= 0 || (productiveWindow() && supplyBudget.fits)) return std::nullopt;
		const bool typedFit = std::all_of(supplyKindBudgets.begin(), supplyKindBudgets.end(),
		    [](const auto& budget) { return budget.fits; });
		const double fraction = std::clamp(combatFraction, 0.0, 1.0);
		if (!typedFit || coinGoldPerMinute <= 0 ||
		    recoveryRouteHealthLoss > maximumHealth - static_cast<double>(playerBotSupplyRecoveryRequiredHealth(maximumHealth)) ||
		    (predictedFightSeconds > 0 && (fraction <= 0 || predictedFightSeconds > maximumHuntSeconds * fraction)))
			return std::nullopt;
		PlayerBotHuntRegion probe = *this;
		probe.currentHealth = maximumHealth;
		probe.fitSupplyWindow(reserve);
		PlayerBotPreparationRequirement requirement;
		requirement.kind = PlayerBotRequirementKind::HealthRegeneration;
		if (!probe.productiveWindow() || !probe.supplyBudget.fits || !probe.recoverySustainable()) {
			// Only the next live evaluator can establish a food effect.
			if (supplyProfile.regenerationSeconds > 0) return std::nullopt;
			requirement.health = std::max(0, currentHealth);
			requirement.observeRegeneration = true;
			return requirement;
		}
		int32_t lower = currentHealth, upper = maximumHealth;
		for (int i = 0; i < 32 && upper - lower > 1; ++i) {
			probe.currentHealth = lower + (upper - lower) / 2;
			probe.fitSupplyWindow(reserve);
			if (probe.productiveWindow() && probe.supplyBudget.fits && probe.recoverySustainable()) upper = probe.currentHealth;
			else lower = probe.currentHealth;
		}
		requirement.health = upper;
		return requirement;
	}

	// Smallest productive whole-second outing (one modeled fight), not the
	// configured full hunt. Reuse the same learned/static demand and transit
	// exposure as final admission, and verify the resulting stock with its fit check.
	PlayerBotSupplyRequirements minimumProductiveSupplies(uint32_t reserve) const
	{
		if (supplyRecovery) return {}; // Recovery has a different health contract.
		PlayerBotHuntRegion minimum = *this;
		const double fraction = std::clamp(combatFraction, 0.0, 1.0);
		if (predictedFightSeconds > 0 && fraction <= 0) return {};
		minimum.availableHuntSeconds = std::max(1.0, std::ceil(
		    predictedFightSeconds > 0 ? predictedFightSeconds / fraction : 1.0));
		if (minimum.availableHuntSeconds > maximumHuntSeconds) return {};
		minimum.reconcileSupplies(reserve);
		auto required = [](uint32_t reserveStock, double expected, uint32_t minimumRoutine = 1) -> uint32_t {
			if (!std::isfinite(expected) || expected < 0 || expected >= UINT32_MAX - reserveStock)
				return UINT32_MAX;
			return reserveStock + std::max(minimumRoutine, static_cast<uint32_t>(std::ceil(expected)));
		};
		PlayerBotSupplyRequirements requirements{{PlayerBotSupplyKind::HealthPotion, 0,
		    required(reserve, minimum.supplyBudget.expectedPotions)}};
		minimum.supplyProfile.potions = requirements.front().count;
		for (size_t i = 0; i < minimum.supplyProfile.kinds.size(); ++i) {
			auto& kind = minimum.supplyProfile.kinds[i];
			const bool needsStock = kind.returnThreshold != 0 || kind.kind == PlayerBotSupplyKind::Ammunition ||
			    kind.kind == PlayerBotSupplyKind::ThrowingWeapon;
			const uint32_t count = required(kind.returnThreshold, minimum.supplyKindBudgets[i].expected, needsStock ? 1 : 0);
			requirements.push_back({kind.kind, kind.itemId, count});
			kind.count = count;
		}
		minimum.reconcileSupplies(reserve);
		if (!minimum.productiveWindow() || !minimum.supplyBudget.fits ||
		    std::any_of(requirements.begin(), requirements.end(),
		        [](const auto& requirement) { return requirement.count == UINT32_MAX; })) return {};
		return requirements;
	}

	void reconcileTravel(double durationSeconds, double travelSeconds, double staminaMultiplier)
	{
		estimatedTravelSeconds = std::max(0.0, travelSeconds);
		maximumHuntSeconds = availableHuntSeconds = std::max(0.0, durationSeconds);
		staminaExperienceMultiplier = staminaMultiplier;
		projectedExperience = experiencePerMinute * observedCorrection *
		                      staminaExperienceMultiplier * availableHuntSeconds / 60.0;
		// XP is the tie-breaker within the duration-budget preference tier.
		score = projectedExperience;
		reconcileSupplies(supplyProfile.reserve);
	}

	void reconcileRecovery(uint32_t reserve, uint64_t funds)
	{
		reconcileSupplies(reserve);
		if (!frozenEconomicKey) cashPressure = playerBotHuntCashPressure(funds);
	}

	void reconcileSupplies(uint32_t reserve, bool optimisticTransitCombat = false)
	{
		supplyProfile.reserve = reserve;
		supplyBudget = playerBotSupplyBudget(supplyProfile, expectedDamagePerSecond, combatFraction,
		                                    availableHuntSeconds, estimatedTravelSeconds);
		if (supplyRecovery && supplyProfile.potions <= reserve) {
			// A short recovery outing may spend only health above the 80% floor.
			const double healthBudget = std::max(0.0, currentHealth - static_cast<double>(playerBotSupplyRecoveryRequiredHealth(std::max(0, maximumHealth))) - recoveryRouteHealthLoss);
			const double deficit = std::max(0.0, supplyBudget.expectedDamage - supplyBudget.regenerationHealing -
			    supplyBudget.spellHealing - healthBudget);
			supplyBudget.potionEquivalentDemand = deficit == 0 ? 0 : supplyProfile.potionHealing > 0 ?
			    deficit / supplyProfile.potionHealing : std::numeric_limits<double>::max();
			supplyBudget.expectedPotions = std::ceil(supplyBudget.potionEquivalentDemand);
			supplyBudget.fits = supplyBudget.expectedPotions == 0 &&
			    recoveryRouteHealthLoss <= currentHealth - static_cast<double>(playerBotSupplyRecoveryRequiredHealth(std::max(0, maximumHealth)));
		}
		const double exposure = availableHuntSeconds * std::clamp(combatFraction, 0.0, 1.0);
		supplyEstimateSource = "static";
		supplyEstimateReason = "static_duration_budget";
		supplyLocalRejectionReason = "no_local_evidence";
		supplyStaticPotionsPerCombatSecond = exposure > 0 ? supplyBudget.potionEquivalentDemand / exposure : 0;
		supplyAppliedPotionsPerCombatSecond = supplyStaticPotionsPerCombatSecond;
		bool learnedEstimate = false;
		if (playerBotSupplyCalibrationForCapability(supplyCalibration, supplyCapability)) {
			learnedEstimate = true;
			supplyEstimateSource = "local";
			supplyEstimateReason = "local_variant_learning";
			supplyLocalRejectionReason = nullptr;
			supplyAppliedPotionsPerCombatSecond = supplyCalibration.potionsPerCombatSecond;
			supplyBudget.potionEquivalentDemand = supplyAppliedPotionsPerCombatSecond * exposure;
		} else {
			if (supplyCalibration.samples != 0) {
				const auto change = playerBotCompareSupplyCapabilities(
				    supplyCalibration.capability, supplyCapability);
				supplyLocalRejectionReason = change.materialChange ? "material_capability_change" :
				    change.recoveryContextChanged ? "recovery_contract_changed" : "capability_regression";
			}
			if (supplyGlobalLearning.samples != 0) {
				learnedEstimate = true;
				supplyEstimateSource = "global";
				supplyEstimateReason = "global_policy_multiplier";
				const double multiplier = std::max(playerBotSupplyGlobalMinimumMultiplier, supplyGlobalLearning.multiplier);
				supplyAppliedPotionsPerCombatSecond = supplyStaticPotionsPerCombatSecond * multiplier;
				// Scale raw demand directly: converting to a rate and back can add
				// floating-point error at an exact whole-potion boundary.
				supplyBudget.potionEquivalentDemand = exposure > 0 ?
				    supplyBudget.potionEquivalentDemand * multiplier : 0;
			}
		}
		if (learnedEstimate) {
			supplyBudget.expectedPotions = std::ceil(supplyBudget.potionEquivalentDemand);
			supplyBudget.fits = (supplyRecovery && supplyBudget.expectedPotions == 0 &&
			    recoveryRouteHealthLoss <= currentHealth - static_cast<double>(playerBotSupplyRecoveryRequiredHealth(std::max(0, maximumHealth)))) ||
			    ((supplyProfile.potions > reserve || reserve == 0) &&
			     supplyBudget.expectedPotions <= supplyBudget.routinePotions);
		}
		supplyKindBudgets.clear();
		for (const PlayerBotSupplyKindProfile& kind : supplyProfile.kinds) {
			// Coarse distance cannot prove encounters: admit optimistically until
			// detailed routes supply health-loss exposure. Keep travel time for
			// economic scoring and food lifetime; never bypass final stock checks.
			supplyKindBudgets.push_back(playerBotSupplyKindBudget(kind,
			    exposure + (optimisticTransitCombat ? 0 : transitCombatSeconds()), supplyRecovery));
			supplyBudget.fits = supplyBudget.fits && supplyKindBudgets.back().fits;
		}
	}
};

struct PlayerBotHuntRegionPerformance {
	double observedExperiencePerMinute = 0;
	double observedCoinGoldPerMinute = 0;
	double correction = 1;
	uint32_t samples = 0;
	uint64_t atlasRevision = 0;
	bool reliable = false;
	PlayerBotSupplyCalibration supply;
};

inline constexpr double playerBotHuntMinimumXpCorrection = 0.1;
inline constexpr double playerBotHuntMaximumXpCorrection = 10.0;

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
		    observed.correction < playerBotHuntMinimumXpCorrection ||
		    observed.correction > playerBotHuntMaximumXpCorrection) continue;
		total += observed.correction;
		++result.testedVariants;
	}
	if (result.testedVariants != 0) {
		result.correction = std::clamp(total / result.testedVariants,
		    playerBotHuntMinimumXpCorrection, playerBotHuntMaximumXpCorrection);
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
		    observed.correction >= playerBotHuntMinimumXpCorrection &&
		    observed.correction <= playerBotHuntMaximumXpCorrection) {
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
// Dimensionless use of the scarcest routine supply. A one-unit denominator
// also orders shortages when no routine stock remains, without division by zero.
// All fitting budgets rank first; among failures this compares the actual
// bottleneck rather than unrelated health-potion consumption.
inline double playerBotHuntSupplyPressure(const PlayerBotHuntRegion& region)
{
	double pressure = region.supplyBudget.expectedPotions / std::max<uint32_t>(1, region.supplyBudget.routinePotions);
	for (const PlayerBotSupplyKindBudget& budget : region.supplyKindBudgets) {
		double typedPressure = budget.expected / std::max<uint32_t>(1, budget.routine);
		// Essential stock at its return threshold cannot masquerade as free
		// merely because there is no learned consumption yet.
		if (!budget.fits && budget.routine == 0) typedPressure = std::max(1.0, typedPressure);
		pressure = std::max(pressure, typedPressure);
	}
	return pressure;
}

inline bool playerBotPreferHuntRegion(const PlayerBotHuntRegion& left, const PlayerBotHuntRegion& right)
{
	const bool leftAvailable = left.suitable && left.reachable;
	const bool rightAvailable = right.suitable && right.reachable;
	if (leftAvailable != rightAvailable) return leftAvailable;
	// Spending the whole outing on travel is not a free, supply-fitting hunt.
	const bool leftProductive = left.productiveWindow();
	const bool rightProductive = right.productiveWindow();
	if (leftProductive != rightProductive) return leftProductive;
	if (left.supplyBudget.fits != right.supplyBudget.fits) return left.supplyBudget.fits;
	if (!left.supplyBudget.fits) {
		const double leftPressure = playerBotHuntSupplyPressure(left);
		const double rightPressure = playerBotHuntSupplyPressure(right);
		if (leftPressure != rightPressure) return leftPressure < rightPressure;
	}
	const auto leftKey = left.economicKey(), rightKey = right.economicKey();
	const bool recovery = leftKey.supplyRecovery && rightKey.supplyRecovery;
	const int economicOrder = playerBotCompareHuntEconomicKeys(leftKey, rightKey);
	if (economicOrder != 0) return economicOrder > 0;
	if (recovery && left.threatRatio != right.threatRatio) return left.threatRatio < right.threatRatio;
	if (left.score != right.score) return left.score > right.score;
	// A zero-duration estimate must not let an unproven disconnected region win
	// a tie merely because it forecasts no combat consumption.
	if (left.topologyReachable != right.topologyReachable) return left.topologyReachable;
	return left.nonproductiveTripSeconds() < right.nonproductiveTripSeconds();
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

// For one stamina snapshot, multiplier * available seconds never decreases as
// available time grows, so the full hunt duration gives a zero-travel XP upper
// bound. A stamina or premium change restarts hunt planning.
inline double playerBotHuntStaminaExperienceMultiplier(uint16_t staminaMinutes, bool staminaSystem, bool premium,
                                                       double availableHuntSeconds)
{
	if (staminaMinutes == 0) return 0;
	if (!staminaSystem) return 1;
	if (staminaMinutes > 2400 && premium && availableHuntSeconds > 0) {
		// The award callback can consume two minutes before it checks the premium threshold.
		const double bonusSeconds = std::min(availableHuntSeconds,
		                                     std::max<int32_t>(0, staminaMinutes - 2402) * 60.0);
		return 1 + 0.5 * bonusSeconds / availableHuntSeconds;
	}
	return staminaMinutes <= 840 ? 0.5 : 1;
}

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

// Health's stepped healing budget keeps the full XP bound. Typed demand is
// linear: a fitting hunt cannot spend more than its snapshotted routine stock.
// Ignore transit demand optimistically. Stock changes invalidate the pass; live
// route refreshes retain the same item's learned/static demand, without buying.
inline double playerBotHuntOptimisticFittingExperience(const PlayerBotHuntRegion& region, double durationSeconds,
                                                       double peakExperiencePerSecond)
{
	const double conservative = std::max(region.optimisticProjectedExperience, peakExperiencePerSecond * durationSeconds);
	if (!std::isfinite(durationSeconds) || durationSeconds < 0 ||
	    !std::isfinite(peakExperiencePerSecond) || peakExperiencePerSecond < 0 ||
	    !std::isfinite(region.combatFraction) || region.combatFraction <= 0) return conservative;
	const double fraction = std::min(1.0, region.combatFraction);
	double typedSeconds = durationSeconds;
	bool knownDemand = false;
	for (const auto& kind : region.supplyProfile.kinds) {
		const double rate = kind.demand.samples != 0 ? kind.demand.unitsPerCombatSecond :
		    std::max(kind.demand.unitsPerCombatSecond, kind.staticUnitsPerCombatSecond);
		const double perSecond = rate * fraction;
		if (!std::isfinite(rate) || rate <= 0 || !std::isfinite(perSecond) || perSecond <= 0) continue;
		const uint32_t reserve = playerBotSupplyHuntReserve(kind.kind, kind.returnThreshold, region.supplyRecovery);
		const uint32_t routine = kind.count > reserve ? kind.count - reserve : 0;
		typedSeconds = std::min(typedSeconds, routine / perSecond);
		knownDemand = true;
	}
	if (!knownDemand) return conservative;
	// Keep a whole second beyond the rounded bound: final XP uses a different
	// arithmetic order, so one nextafter alone cannot protect tied scores.
	typedSeconds = std::min(durationSeconds, std::ceil(typedSeconds) + 1);
	const double typedExperience = std::nextafter(peakExperiencePerSecond * typedSeconds,
	    std::numeric_limits<double>::infinity());
	const double fullExperience = std::isfinite(region.optimisticProjectedExperience) &&
	    region.optimisticProjectedExperience >= 0 ? region.optimisticProjectedExperience : conservative;
	return std::min(fullExperience, typedExperience);
}

inline bool playerBotHuntCandidateCanBeatValidated(
    const PlayerBotHuntRegion& candidate, const PlayerBotHuntRegion& validated)
{
	if (!candidate.suitable || !candidate.reachable) return false;
	// An unproductive incumbent must not prune a useful fallback. Only compare
	// upper bounds after both productivity and supply preference tiers are met.
	if (!validated.productiveWindow() || !validated.supplyBudget.fits) return true;
	// A fitting incumbent outranks every non-fitting result, so the candidate
	// must fit after validation to win.
	if (candidate.optimisticFittingExperience < 0) return false;
	const auto candidateKey = candidate.economicKey(), validatedKey = validated.economicKey();
	const int economicOrder = playerBotCompareHuntEconomicKeys(candidateKey, validatedKey);
	if (economicOrder != 0) return economicOrder > 0;
	// Equal recovery keys may still win on safety before XP. Otherwise keep
	// the existing conservative XP bound, including ties and unknown bounds.
	if (candidateKey.supplyRecovery && validatedKey.supplyRecovery) return true;
	return std::min(candidate.optimisticProjectedExperience, candidate.optimisticFittingExperience) >= validated.score;
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
	const auto key = region.economicKey();
	if (key.supplyRecovery) return region.supplyBudget.fits ?
	    "supply_recovery_coin_safety_then_xp" : "supply_recovery_pressure_then_coin_safety_then_xp";
	if (key.cashPressure) return region.supplyBudget.fits ?
	    "supply_budget_then_coin_income_then_xp" : "lowest_supply_pressure_then_coin_income_then_xp";
	return region.supplyBudget.fits ? "supply_budget_then_xp" : "lowest_supply_pressure_then_xp";
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
