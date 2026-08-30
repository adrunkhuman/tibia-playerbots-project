/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 */

#include "otpch.h"

#include "playerbothuntpolicy.h"

#include <algorithm>
#include <numeric>

namespace {
	constexpr double minimumChallengeFrontier = 0.10;
	constexpr double maximumChallengeFrontier = 0.40;
	constexpr double challengeEscalation = 0.025;
	constexpr double challengeBackoff = 0.05;
	constexpr double challengeHealthSafetyPercent = 85;
	constexpr double minimumChallengeActiveSeconds = 30;
	constexpr uint32_t minimumChallengeKills = 1;
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
	const uint8_t healthPercent = sample.maximumHealth <= 0 ? 0 : static_cast<uint8_t>(std::clamp(
		sample.health * 100 / sample.maximumHealth, 0, 100));
	++evidence.healthPercentSamples[healthPercent];
	evidence.maximumAttackerOverlap = std::max(evidence.maximumAttackerOverlap, sample.attackers);
}

void PlayerBotHuntPolicy::sampleCombat(const PlayerBotHuntCombatSnapshot& snapshot)
{
	double elapsedSeconds = 0;
	if (lastSample.time_since_epoch().count() != 0) {
		elapsedSeconds = std::chrono::duration<double>(snapshot.observedAt - lastSample).count();
	}
	lastSample = snapshot.observedAt;
	observeCombat({snapshot.active, elapsedSeconds, snapshot.health, snapshot.maximumHealth, snapshot.attackers});
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
	summary.dangerObserved = evidence.dangerObserved;
	summary.deathObserved = evidence.deathObserved;
	summary.healthPercentSamples = evidence.healthPercentSamples;
	const uint32_t samples = std::accumulate(evidence.healthPercentSamples.begin(), evidence.healthPercentSamples.end(), 0U);
	if (samples == 0) {
		return summary;
	}
	const uint32_t threshold = (samples + 9) / 10;
	uint32_t cumulative = 0;
	for (uint8_t percent = 0; percent <= 100; ++percent) {
		cumulative += evidence.healthPercentSamples[percent];
		if (cumulative >= threshold) {
			summary.p10HealthPercent = percent;
			return summary;
		}
	}
	summary.p10HealthPercent = 100;
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
	const bool nearFullHealth = update.combat.minimumHealth != std::numeric_limits<int32_t>::max() &&
	                            static_cast<int64_t>(update.combat.minimumHealth) * 100 >=
	                                static_cast<int64_t>(sample.maximumHealth) * challengeHealthSafetyPercent;
	const bool backoff = update.combat.dangerObserved || update.combat.deathObserved || update.verifiedRecoveries != 0;
	const bool qualifyingEasy = enoughActiveCombat && nearFullHealth && !backoff;
	if (backoff && (update.combat.activeSeconds > 0 || update.combat.deathObserved)) {
		frontier = std::max(minimumChallengeFrontier, frontier - challengeBackoff);
		qualifyingHuntsToHold = 2;
		update.result = PlayerBotHuntChallengeResult::Backoff;
	} else if (qualifyingEasy && qualifyingHuntsToHold != 0) {
		--qualifyingHuntsToHold;
		update.result = PlayerBotHuntChallengeResult::Hold;
	} else if (qualifyingEasy) {
		frontier = std::min(maximumChallengeFrontier, frontier + challengeEscalation);
		update.result = frontier == update.frontierBefore ? PlayerBotHuntChallengeResult::Clamped :
		                                                PlayerBotHuntChallengeResult::Escalated;
	}
	update.frontierAfter = frontier;
	update.qualifyingHuntsToHold = qualifyingHuntsToHold;
	return update;
}

PlayerBotHuntPerformanceUpdate PlayerBotHuntPolicy::observePerformance(uint64_t variantId,
	const PlayerBotHuntPerformanceSample& sample)
{
	PlayerBotHuntPerformanceUpdate update;
	update.updatedCorrection = sample.observedCorrection;
	if (sample.durationSeconds < 30 || sample.kills == 0) {
		return update;
	}
	update.actualExperiencePerMinute = sample.experienceGained * 60.0 / sample.durationSeconds;
	const double predictedNetRate = sample.projectedExperience * 60.0 /
	                                std::max<uint32_t>(1, sample.configuredHuntDurationSeconds);
	if (predictedNetRate <= 0) {
		return update;
	}
	PlayerBotHuntRegionPerformance& regionPerformance = performance[variantId];
	const double sampleCorrection = std::clamp(
		sample.observedCorrection * update.actualExperiencePerMinute / predictedNetRate, 0.25, 2.0);
	if (regionPerformance.samples == 0) {
		regionPerformance.observedExperiencePerMinute = update.actualExperiencePerMinute;
		regionPerformance.correction = sampleCorrection;
	} else {
		regionPerformance.observedExperiencePerMinute = regionPerformance.observedExperiencePerMinute * 0.65 +
		                                                 update.actualExperiencePerMinute * 0.35;
		regionPerformance.correction = regionPerformance.correction * 0.65 + sampleCorrection * 0.35;
	}
	++regionPerformance.samples;
	update.updatedCorrection = regionPerformance.correction;
	update.observed = true;
	return update;
}
