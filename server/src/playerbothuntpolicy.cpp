/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 */

#include "playerbothuntpolicy.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <tuple>
#include <utility>

namespace {
	constexpr double minimumChallengeFrontier = 0.10;
	// Keep exploration above the old 0.40 ceiling, but below parity threat.
	// Predicted lethal and route danger remain independent hard gates.
	constexpr double maximumChallengeFrontier = 0.60;
	constexpr double challengeEscalation = 0.05;
	constexpr double strongChallengeEscalation = 0.10;
	constexpr double challengeBackoff = 0.10;
	constexpr double challengeHealthSafetyPercent = 70;
	constexpr double challengeCriticalHealthPercent = 35;
	constexpr double challengeManaSafetyPercent = 35;
	constexpr double challengeCriticalManaPercent = 20;
	constexpr double minimumChallengeActiveSeconds = 60;
	constexpr uint32_t minimumChallengeKills = 3;
	constexpr double minimumPerformanceActiveSeconds = 60;
	constexpr uint32_t minimumPerformanceKills = 3;
	constexpr uint32_t minimumPerformanceOutingSeconds = 120;
	constexpr auto dangerObservationWindow = std::chrono::minutes(2);

	std::pair<uint8_t, double> qualifiedSupplyCrowdCoverage(const PlayerBotHuntCombatSummary& combat)
	{
		uint8_t coverage = 0;
		double coverageSeconds = 0;
		for (std::size_t attackers = 1; attackers < combat.attackerExposureSeconds.size(); ++attackers) {
			double seconds = 0;
			for (std::size_t bin = attackers; bin < combat.attackerExposureSeconds.size(); ++bin) {
				seconds += combat.attackerExposureSeconds[bin];
			}
			if (seconds < playerBotSharedSupplyMinimumCrowdSeconds ||
			    seconds < combat.activeSeconds * playerBotSharedSupplyMinimumCrowdRatio) break;
			coverage = static_cast<uint8_t>(attackers);
			coverageSeconds = seconds;
		}
		return {coverage, coverageSeconds};
	}
}

const char* playerBotHuntChallengeResultName(PlayerBotHuntChallengeResult result)
{
	switch (result) {
		case PlayerBotHuntChallengeResult::Backoff:
			return "backoff";
		case PlayerBotHuntChallengeResult::Hold:
			return "hold";
		case PlayerBotHuntChallengeResult::Escalated:
			return "escalated";
		case PlayerBotHuntChallengeResult::Clamped:
			return "clamped";
		case PlayerBotHuntChallengeResult::InsufficientActiveCombat:
			return "insufficient_active_combat";
	}
	return "insufficient_active_combat";
}

void PlayerBotHuntPolicy::resetCombatEvidence()
{
	evidence = PlayerBotHuntCombatEvidence{};
	lastSample = {};
}

void PlayerBotHuntPolicy::observeCombat(const PlayerBotHuntCombatSample& sample)
{
	if (!sample.active) {
		return;
	}
	evidence.activeSeconds += std::max(0.0, sample.elapsedSeconds);
	evidence.minimumHealth = std::min(evidence.minimumHealth, sample.health);
	evidence.minimumMana = std::min(evidence.minimumMana, sample.mana);
	const uint8_t healthPercent = sample.maximumHealth <= 0 ? 0 : static_cast<uint8_t>(std::clamp(
		sample.health * 100 / sample.maximumHealth, 0, 100));
	const uint8_t manaPercent = sample.maximumMana == 0 ? 100 : static_cast<uint8_t>(std::min<uint64_t>(
		static_cast<uint64_t>(sample.mana) * 100 / sample.maximumMana, 100));
	++evidence.healthPercentSamples[healthPercent];
	++evidence.manaPercentSamples[manaPercent];
	evidence.maximumAttackerOverlap = std::max(evidence.maximumAttackerOverlap, sample.attackers);
	evidence.attackerExposureSeconds[std::min<std::size_t>(sample.attackers, evidence.attackerExposureSeconds.size() - 1)] +=
	    std::max(0.0, sample.elapsedSeconds);
}

void PlayerBotHuntPolicy::sampleCombat(const PlayerBotHuntCombatSnapshot& snapshot)
{
	double elapsedSeconds = 0;
	if (lastSample.time_since_epoch().count() != 0) {
		elapsedSeconds = std::chrono::duration<double>(snapshot.observedAt - lastSample).count();
	}
	lastSample = snapshot.observedAt;
	observeCombat({snapshot.active, elapsedSeconds, snapshot.health, snapshot.maximumHealth,
	               snapshot.mana, snapshot.maximumMana, snapshot.attackers});
}

void PlayerBotHuntPolicy::observeKill()
{
	++evidence.kills;
}

void PlayerBotHuntPolicy::observeDamage(uint32_t damage)
{
	evidence.damageTaken += damage;
}

void PlayerBotHuntPolicy::observeRecovery(bool potion)
{
	if (potion) {
		++evidence.potionRecoveries;
	} else {
		++evidence.spellRecoveries;
	}
}

void PlayerBotHuntPolicy::observeDeath()
{
	evidence.deathObserved = true;
}

bool PlayerBotHuntPolicy::observeDanger(int32_t maximumHealth, std::chrono::steady_clock::duration huntAge)
{
	if (maximumHealth > 0 && evidence.damageTaken >= static_cast<uint32_t>(maximumHealth) &&
	    huntAge < dangerObservationWindow) {
		evidence.dangerObserved = true;
	}
	return evidence.dangerObserved;
}

PlayerBotHuntCombatSummary PlayerBotHuntPolicy::combatSummary() const
{
	PlayerBotHuntCombatSummary summary;
	summary.activeSeconds = evidence.activeSeconds;
	summary.kills = evidence.kills;
	summary.damageTaken = evidence.damageTaken;
	summary.potionRecoveries = evidence.potionRecoveries;
	summary.spellRecoveries = evidence.spellRecoveries;
	summary.maximumAttackerOverlap = evidence.maximumAttackerOverlap;
	summary.attackerExposureSeconds = evidence.attackerExposureSeconds;
	summary.minimumHealth = evidence.minimumHealth;
	summary.minimumMana = evidence.minimumMana;
	summary.dangerObserved = evidence.dangerObserved;
	summary.deathObserved = evidence.deathObserved;
	summary.healthPercentSamples = evidence.healthPercentSamples;
	summary.manaPercentSamples = evidence.manaPercentSamples;
	auto percentile10 = [](const std::array<uint32_t, 101>& samples, uint8_t fallback) {
		const uint32_t count = std::accumulate(samples.begin(), samples.end(), 0U);
		if (count == 0) return fallback;
		const uint32_t threshold = (count + 9) / 10;
		uint32_t cumulative = 0;
		for (uint16_t percent = 0; percent <= 100; ++percent) {
			cumulative += samples[percent];
			if (cumulative >= threshold) return static_cast<uint8_t>(percent);
		}
		return static_cast<uint8_t>(100);
	};
	summary.p10HealthPercent = percentile10(evidence.healthPercentSamples, 0);
	summary.p10ManaPercent = percentile10(evidence.manaPercentSamples, 100);
	return summary;
}

PlayerBotHuntChallengeUpdate PlayerBotHuntPolicy::updateChallengeFrontier(const PlayerBotHuntChallengeSample& sample)
{
	PlayerBotHuntChallengeUpdate update;
	update.frontierBefore = frontier;
	update.combat = combatSummary();
	update.activeCombatUptime = sample.durationSeconds == 0 ? 0 : update.combat.activeSeconds / sample.durationSeconds;
	update.verifiedRecoveries = update.combat.potionRecoveries + update.combat.spellRecoveries;
	update.minimumActiveCombatSeconds = minimumChallengeActiveSeconds;
	update.minimumKills = minimumChallengeKills;
	const bool enoughActiveCombat = update.combat.activeSeconds >= minimumChallengeActiveSeconds &&
	                                update.combat.kills >= minimumChallengeKills;
	const bool lowHealthPressure = update.combat.p10HealthPercent < challengeHealthSafetyPercent;
	const bool criticalHealth = update.combat.minimumHealth != std::numeric_limits<int32_t>::max() &&
	                            static_cast<int64_t>(update.combat.minimumHealth) * 100 <
	                                static_cast<int64_t>(sample.maximumHealth) * challengeCriticalHealthPercent;
	const bool manaPressure = update.combat.spellRecoveries != 0 &&
	                         update.combat.p10ManaPercent < challengeManaSafetyPercent;
	const bool criticalMana = update.combat.spellRecoveries != 0 &&
	                         update.combat.p10ManaPercent < challengeCriticalManaPercent;
	update.potionRecoveriesPerActiveMinute = update.combat.activeSeconds == 0 ? 0 :
	    update.combat.potionRecoveries * 60.0 / update.combat.activeSeconds;
	const bool heavyRecovery = update.combat.potionRecoveries >= 3 &&
	                           update.potionRecoveriesPerActiveMinute >= 1.0;
	const bool backoff = update.combat.dangerObserved || update.combat.deathObserved || criticalHealth ||
	                     criticalMana || heavyRecovery;
	const bool qualifyingEasy = enoughActiveCombat && !lowHealthPressure && !manaPressure && !backoff &&
	                           update.combat.potionRecoveries <= 2;
	if (backoff && (update.combat.activeSeconds > 0 || update.combat.deathObserved)) {
		frontier = std::max(minimumChallengeFrontier, frontier - challengeBackoff);
		qualifyingHuntsToHold = 2;
		update.result = PlayerBotHuntChallengeResult::Backoff;
	} else if (qualifyingEasy && qualifyingHuntsToHold != 0) {
		--qualifyingHuntsToHold;
		update.result = PlayerBotHuntChallengeResult::Hold;
	} else if (qualifyingEasy) {
		const bool strongEvidence = update.combat.potionRecoveries <= 2 && update.combat.p10ManaPercent >= 50;
		frontier = std::min(maximumChallengeFrontier, frontier +
		                    (strongEvidence ? strongChallengeEscalation : challengeEscalation));
		update.result = frontier == update.frontierBefore ? PlayerBotHuntChallengeResult::Clamped :
		                                                PlayerBotHuntChallengeResult::Escalated;
	} else if (enoughActiveCombat) {
		update.result = PlayerBotHuntChallengeResult::Hold;
	}
	update.frontierAfter = frontier;
	update.qualifyingHuntsToHold = qualifyingHuntsToHold;
	return update;
}

PlayerBotSupplyObservation PlayerBotHuntPolicy::observeSupplies(const PlayerBotHuntRegion& region,
    const PlayerBotSupplyCapabilitySnapshot& capabilityAfter, uint64_t durationSeconds,
    int32_t health, int32_t maximumHealth, uint32_t mana, uint32_t potions, bool interrupted)
{
	const auto combat = combatSummary();
	auto& history = performance[region.atlasVariantId];
	if (history.atlasRevision != region.atlasRevision) history = {};
	history.atlasRevision = region.atlasRevision;
	auto& learned = history.supply;
	PlayerBotSupplyObservation update;
	std::tie(update.observedAttackerCoverage, update.observedAttackerCoverageSeconds) =
	    qualifiedSupplyCrowdCoverage(combat);
	if (const auto applicable = playerBotSupplyCalibrationForCapability(learned, capabilityAfter)) {
		update.calibration = *applicable;
	}
	const PlayerBotSupplyCapabilityComparison capabilityChange =
	    playerBotCompareSupplyCapabilities(region.supplyCapability, capabilityAfter);
	update.changedFields = capabilityChange.changedFields;
	update.direction = capabilityChange.direction;

	auto withoutFood = [](PlayerBotSupplyCapabilitySnapshot capability) {
		capability.foodActive = false;
		capability.foodHealthGain = 0;
		capability.foodHealthIntervalMilliseconds = 0;
		capability.foodManaGain = 0;
		capability.foodManaIntervalMilliseconds = 0;
		return capability;
	};
	const PlayerBotSupplyCapabilityComparison nonFoodChange = playerBotCompareSupplyCapabilities(
	    withoutFood(region.supplyCapability), withoutFood(capabilityAfter));
	const bool foodContextBoundary = capabilityChange.recoveryContextChanged &&
	    !capabilityChange.materialChange && nonFoodChange.compatible;
	if (capabilityChange.materialChange) {
		update.reason = "material_capability_change";
		return update;
	}
	if (combat.activeSeconds <= 0) {
		update.reason = "insufficient_active_combat";
		return update;
	}

	const auto compatibleHistory = playerBotSupplyCalibrationForCapability(learned, region.supplyCapability);
	const double exposure = region.availableHuntSeconds * std::clamp(region.combatFraction, 0.0, 1.0);
	const double prior = compatibleHistory ? compatibleHistory->potionsPerCombatSecond :
	    region.sharedSupplyEstimate.available ? region.supplyAppliedPotionsPerCombatSecond :
	    exposure > 0 ? region.supplyBudget.expectedPotions / exposure : 0;
	const double observed = combat.potionRecoveries / combat.activeSeconds;
	const bool depleted = region.supplyProfile.potions > 0 && potions == 0;
	const bool unsafe = combat.dangerObserved || combat.deathObserved || depleted || combat.p10HealthPercent < 70;
	const bool enoughEvidence = !interrupted && durationSeconds >= 120 &&
	    combat.activeSeconds >= 60 && combat.kills >= 3;
	const bool resourcesStable = combat.p10HealthPercent >= 80 && combat.p10ManaPercent >= 50 &&
	    health >= region.currentHealth - maximumHealth / 20 &&
	    mana + region.supplyProfile.maximumMana / 20 >= region.supplyProfile.mana;
	const bool recoveryIndependent = foodContextBoundary && enoughEvidence && resourcesStable &&
	    combat.damageTaken == 0 && combat.spellRecoveries == 0;

	PlayerBotSupplyProfile supplyWithoutFood = region.supplyProfile;
	supplyWithoutFood.regenerationSeconds = 0;
	supplyWithoutFood.healthGain = 0;
	supplyWithoutFood.healthInterval = 0;
	supplyWithoutFood.manaGain = 0;
	supplyWithoutFood.manaInterval = 0;
	const PlayerBotSupplyBudget budgetWithoutFood = playerBotSupplyBudget(supplyWithoutFood,
	    region.expectedDamagePerSecond, region.combatFraction, region.availableHuntSeconds,
	    region.estimatedTravelSeconds);
	const double endStaticRequired = exposure > 0 && !capabilityAfter.foodActive ?
	    budgetWithoutFood.expectedPotions / exposure : prior;
	auto estimateDirection = [](double before, double after) {
		return after > before ? PlayerBotSupplyEstimateDirection::Upward :
		       after < before ? PlayerBotSupplyEstimateDirection::Downward :
		                        PlayerBotSupplyEstimateDirection::Unchanged;
	};
	PlayerBotSupplyCalibration candidate;
	candidate.capability = capabilityAfter;
	candidate.capabilityHash = playerBotSupplyCapabilityHash(capabilityAfter);
	candidate.potionsPerCombatSecond = prior;
	candidate.samples = compatibleHistory ? compatibleHistory->samples : 0;
	auto recordTransferEvidence = [&](bool safeEvidence, bool upwardEvidence) {
		const std::string species = region.monsters.size() == 1 ? region.monsters.front().name : std::string();
		const bool retain = compatibleHistory && !species.empty() && history.supplySpecies == species &&
		    history.supplyAtlasSiteId == region.atlasSiteId;
		if (!retain) {
			history.supplySpecies.clear();
			history.supplyAtlasSiteId = 0;
			history.supplySafeSamples = 0;
			history.supplyAttackerCoverage = 0;
			history.supplyAttackerCoverageSeconds = 0;
			history.supplyUpwardEvidence = false;
		}
		if (species.empty()) return;
		history.supplySpecies = species;
		history.supplyAtlasSiteId = region.atlasSiteId;
		if (upwardEvidence) {
			// Danger or higher demand starts a new downward-evidence window. The
			// raised local rate remains, but pre-danger safe samples cannot make it
			// immediately transferable as a cheaper shared estimate.
			history.supplySafeSamples = 0;
			history.supplyAttackerCoverage = 0;
			history.supplyAttackerCoverageSeconds = 0;
			history.supplyUpwardEvidence = true;
			return;
		}
		if (!safeEvidence) return;
		if (history.supplySafeSamples == 0) {
			history.supplyAttackerCoverage = update.observedAttackerCoverage;
			history.supplyAttackerCoverageSeconds = update.observedAttackerCoverageSeconds;
		} else if (update.observedAttackerCoverage < history.supplyAttackerCoverage) {
			history.supplyAttackerCoverage = update.observedAttackerCoverage;
			history.supplyAttackerCoverageSeconds = update.observedAttackerCoverageSeconds;
		} else {
			// A higher-coverage sample also covers the retained lower bound. Its
			// qualified seconds are a conservative lower bound for that overlap.
			history.supplyAttackerCoverageSeconds = std::min(
			    history.supplyAttackerCoverageSeconds, update.observedAttackerCoverageSeconds);
		}
		++history.supplySafeSamples;
	};

	// Upward evidence may cross a food-context boundary when every non-food
	// capability remains compatible. Preserve both the prior and the static
	// no-food requirement so an unsafe outing can never make demand cheaper.
	if (unsafe || observed > prior) {
		if (!capabilityChange.compatible && !foodContextBoundary) {
			update.reason = "capability_regression";
			return update;
		}
		candidate.potionsPerCombatSecond = std::max({prior, endStaticRequired, observed});
		if (unsafe) candidate.potionsPerCombatSecond = std::max(candidate.potionsPerCombatSecond, 1.0 / 60.0);
		++candidate.samples;
		learned = candidate;
		recordTransferEvidence(false, true);
		update.calibration = learned;
		update.accepted = true;
		update.estimateDirection = estimateDirection(prior, candidate.potionsPerCombatSecond);
		update.reason = unsafe ?
		    (update.estimateDirection == PlayerBotSupplyEstimateDirection::Upward ?
		         "unsafe_upward_correction" : "unsafe_demand_held") :
		    "higher_observed_demand";
		return update;
	}
	if (!capabilityChange.compatible && !recoveryIndependent) {
		update.reason = capabilityChange.recoveryContextChanged ? "recovery_context_changed" :
		                                                          "capability_regression";
		return update;
	}
	if (interrupted) {
		update.reason = "interrupted_outing";
		return update;
	}
	if (!enoughEvidence) {
		update.reason = "insufficient_combat_evidence";
		return update;
	}
	if (!resourcesStable) {
		update.reason = "resource_guard_failed";
		return update;
	}

	candidate.potionsPerCombatSecond = prior * 0.8 + observed * 0.2;
	++candidate.samples;
	// Zero is established only after repeated samples leave less than one
	// projected potion over this outing's combat exposure.
	if (observed == 0 && candidate.samples >= 3 && candidate.potionsPerCombatSecond * exposure < 1)
		candidate.potionsPerCombatSecond = 0;
	learned = candidate;
	recordTransferEvidence(true, false);
	update.calibration = learned;
	update.accepted = true;
	update.estimateDirection = estimateDirection(prior, candidate.potionsPerCombatSecond);
	update.reason = recoveryIndependent ? "recovery_independent_evidence" :
	                capabilityChange.recoveryContextChanged ? "improved_recovery_context" :
	                                                          "safe_combat_evidence";
	return update;
}

PlayerBotHuntPerformanceUpdate PlayerBotHuntPolicy::observePerformance(uint64_t variantId, uint64_t atlasRevision,
	const PlayerBotHuntPerformanceSample& sample)
{
	PlayerBotHuntPerformanceUpdate update;
	update.updatedCorrection = sample.observedCorrection;
	update.actualExperiencePerMinute = sample.durationSeconds == 0 ? 0 :
	    sample.experienceGained * 60.0 / sample.durationSeconds;
	const uint64_t requiredOutingSeconds = std::min<uint32_t>(
	    std::max<uint32_t>(sample.configuredHuntDurationSeconds, 1), minimumPerformanceOutingSeconds);
	if (sample.dangerObserved || sample.deathObserved) {
		update.evidenceReason = "unsafe_outing";
		return update;
	}
	if (sample.durationSeconds < requiredOutingSeconds ||
	    sample.activeCombatSeconds < minimumPerformanceActiveSeconds ||
	    sample.kills < minimumPerformanceKills) {
		update.evidenceReason = "insufficient_combat_evidence";
		return update;
	}
	if (sample.experienceGained == 0 || !std::isfinite(sample.projectedExperience) ||
	    !std::isfinite(sample.observedCorrection) || sample.observedCorrection < 0.25 ||
	    sample.observedCorrection > 2.0) {
		update.evidenceReason = "invalid_performance_sample";
		return update;
	}
	const double predictedNetRate = sample.projectedExperience * 60.0 /
	                                std::max<uint32_t>(1, sample.configuredHuntDurationSeconds);
	if (!std::isfinite(update.actualExperiencePerMinute) || !std::isfinite(predictedNetRate) ||
	    predictedNetRate <= 0) {
		update.evidenceReason = "invalid_performance_sample";
		return update;
	}
	PlayerBotHuntRegionPerformance& regionPerformance = performance[variantId];
	if (regionPerformance.atlasRevision != atlasRevision) regionPerformance = {};
	const double sampleCorrection = std::clamp(
		sample.observedCorrection * update.actualExperiencePerMinute / predictedNetRate, 0.25, 2.0);
	if (!regionPerformance.reliable || regionPerformance.samples == 0 ||
	    regionPerformance.atlasRevision != atlasRevision || !std::isfinite(regionPerformance.correction)) {
		regionPerformance.observedExperiencePerMinute = update.actualExperiencePerMinute;
		regionPerformance.correction = sampleCorrection;
		regionPerformance.samples = 0;
	} else {
		regionPerformance.observedExperiencePerMinute = regionPerformance.observedExperiencePerMinute * 0.65 +
		                                                 update.actualExperiencePerMinute * 0.35;
		regionPerformance.correction = regionPerformance.correction * 0.65 + sampleCorrection * 0.35;
	}
	++regionPerformance.samples;
	regionPerformance.atlasRevision = atlasRevision;
	regionPerformance.reliable = true;
	update.updatedCorrection = regionPerformance.correction;
	update.evidenceReason = "reliable_combat_outing";
	update.observed = true;
	return update;
}
