/** Hunt-region policy helpers and planner facade. */
#include "otpch.h"

#include "playerbothuntregions.h"

#include "playerbothuntregionadapter.h"

namespace {
	constexpr uint32_t recoveryManaReserve = 20;
}

PlayerBotHuntPlanningProfile playerBotHuntPlanningProfile(const Player& player, const PlayerBotCombatProfile& combat,
	                                                       double challengeFrontier)
{
	return PlayerBotHuntRegionAdapter::planningProfile(player, combat, challengeFrontier);
}

PlayerBotRecoveryPrediction playerBotPredictRecovery(const PlayerBotHuntPlanningProfile& profile,
	                                                   double predictedFightSeconds)
{
	PlayerBotRecoveryPrediction prediction;
	prediction.potionUses = std::min<uint32_t>(1, profile.potionCount);
	prediction.potionMinimumHealing = prediction.potionUses * profile.potionMinimumHealing;
	prediction.lightHealingLegal = profile.lightHealingLegal;
	prediction.spellManaCost = profile.lightHealingManaCost;
	prediction.spellCooldown = profile.lightHealingCooldown;
	prediction.manaReserve = prediction.spellManaCost + recoveryManaReserve;
	if (prediction.lightHealingLegal && prediction.spellManaCost != 0 && profile.mana > prediction.manaReserve) {
		const uint32_t manaCasts = (profile.mana - prediction.manaReserve) / prediction.spellManaCost;
		const uint32_t durationCasts = std::max<uint32_t>(1, static_cast<uint32_t>(std::floor(
			predictedFightSeconds * 1000.0 / std::max<uint32_t>(prediction.spellCooldown, 1))));
		prediction.spellCasts = std::min(manaCasts, durationCasts);
	}
	// Safety remains the callback's audited lower envelope, never observed calibration.
	prediction.spellMinimumHealing = prediction.spellCasts * profile.lightHealingMinimum;
	prediction.totalMinimumHealing = prediction.potionMinimumHealing + prediction.spellMinimumHealing;
	return prediction;
}

bool playerBotPredictedLethal(int32_t currentHealth, double predictedDamage)
{
	return currentHealth <= 0 || predictedDamage >= currentHealth;
}

bool playerBotPreferHuntRegion(const PlayerBotHuntRegion& left, const PlayerBotHuntRegion& right)
{
	const bool leftAvailable = left.suitable && left.reachable;
	const bool rightAvailable = right.suitable && right.reachable;
	if (leftAvailable != rightAvailable) return leftAvailable;
	return left.inChallengeBand != right.inChallengeBand ? left.inChallengeBand : left.score > right.score;
}

bool playerBotHuntScopeExhausted(const std::vector<PlayerBotHuntRegion>& regions)
{
	return std::none_of(regions.begin(), regions.end(), [](const PlayerBotHuntRegion& region) {
		return region.suitable && region.reachable;
	});
}

void PlayerBotHuntRegionPlanner::invalidateCache()
{
	PlayerBotHuntRegionAdapter::invalidateCache();
}

uint64_t PlayerBotHuntRegionPlanner::getCacheRevision()
{
	return PlayerBotHuntRegionAdapter::getCacheRevision();
}

PlayerBotHuntRegionScan PlayerBotHuntRegionPlanner::beginScan(const Player& player) const
{
	return PlayerBotHuntRegionAdapter::beginScan(player);
}

PlayerBotHuntRegionScore PlayerBotHuntRegionPlanner::score(Player& player, const PlayerBotHuntPlanningProfile& profile,
	uint64_t revision, size_t candidateIndex, const std::set<Position>& excludedRegions,
	const std::map<Position, PlayerBotHuntRegionPerformance>& performance, uint32_t huntDurationSeconds) const
{
	return PlayerBotHuntRegionAdapter::score(player, profile, revision, candidateIndex, excludedRegions, performance,
	                                        huntDurationSeconds);
}
