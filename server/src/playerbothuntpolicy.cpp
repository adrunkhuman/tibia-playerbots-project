/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 */

#include "playerbothuntpolicy.h"

#include <algorithm>
#include <cmath>
#include <numeric>

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
