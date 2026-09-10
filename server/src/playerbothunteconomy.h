/** Static hunt facts, independent of selection preferences. */
#ifndef FS_PLAYERBOTHUNTECONOMY_H
#define FS_PLAYERBOTHUNTECONOMY_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <tuple>
#include <vector>

#include "position.h"

// global.lua and Container.createLootItem share the SAME inclusive integer roll.
// Lua keeps the division/modulo fractional; Game.createItem converts count to an
// integer. Enumerate once per loaded loot definition, never per scoring turn.
inline double playerBotExpectedLootCount(uint32_t chance, uint32_t maximumCount,
    bool stackable, double rate, uint32_t maximumChance = 100000)
{
	if (rate <= 0 || maximumCount == 0) return 0;
	double total = 0;
	for (uint32_t roll = 0; roll <= maximumChance; ++roll) {
		const double value = roll / rate;
		if (value < chance) total += stackable ? std::floor(std::fmod(value, maximumCount) + 1) : 1;
	}
	return total / (static_cast<double>(maximumChance) + 1);
}

// Build-local memo: bounds storage by distinct loaded arithmetic definitions,
// and cannot carry stale estimates across loot-rate or metadata reloads.
class PlayerBotLootCountMemo {
public:
	double expectedCount(uint32_t chance, uint32_t maximumCount, bool stackable, double rate)
	{
		const auto key = std::make_tuple(chance, maximumCount, stackable, rate);
		const auto found = estimates.find(key);
		if (found != estimates.end()) return found->second;
		++evaluations;
		const double value = playerBotExpectedLootCount(chance, maximumCount, stackable, rate);
		estimates.emplace(key, value);
		return value;
	}
	size_t arithmeticEvaluations() const { return evaluations; }

private:
	std::map<std::tuple<uint32_t, uint32_t, bool, double>, double> estimates;
	size_t evaluations = 0;
};

inline bool playerBotHuntCashPressure(uint32_t potions, uint32_t returnReserve,
    uint64_t funds, uint64_t recoverySpendingReserve)
{
	return static_cast<uint64_t>(potions) <= static_cast<uint64_t>(returnReserve) + 1 &&
	    funds < recoverySpendingReserve;
}

struct PlayerBotReplenishmentPoint {
	Position spawn;
	Position approach;
	double fightSeconds = 0;
	double intervalSeconds = 0;
	bool ignoresBlocking = false;
	double coinGold = 0;
	double experience = 0;
};
struct PlayerBotHuntViability {
	uint32_t reachableSpawns = 0;
	uint32_t replenishingSpawns = 0;
	double minimumAwayRatio = 0;
	double minimumUnblockedPatrolRatio = 0;
	double minimumEmptyPatrolRatio = 0;
	bool eligible = false;
	bool modelLimited = false;
	// Same order as the reachable spawn observations, not deduplicated patrol points.
	std::vector<bool> replenishingMembers;
	std::vector<double> unblockedPatrolRatios;
	std::vector<double> emptyPatrolRatios;
};

struct PlayerBotSustainedHuntYield {
	double coinGoldPerMinute = 0;
	double spawnExperiencePerMinute = 0;
	double clearExperiencePerMinute = 0;
};

inline PlayerBotSustainedHuntYield playerBotSustainedHuntYield(
    const std::vector<PlayerBotReplenishmentPoint>& points, const PlayerBotHuntViability& viability,
    double cycleSeconds)
{
	PlayerBotSustainedHuntYield result;
	double cycleGold = 0, spawnGold = 0, cycleExperience = 0;
	const size_t members = std::min({points.size(), viability.replenishingMembers.size(),
	    viability.unblockedPatrolRatios.size(), viability.emptyPatrolRatios.size()});
	for (size_t i = 0; i < members; ++i) {
		if (!viability.replenishingMembers[i] || points[i].intervalSeconds <= 0) continue;
		// Do not bootstrap recurring income from fights that may be absent on
		// later circuits. Both duty-share estimates cap the spawn-rate forecast.
		const double share = std::min(viability.unblockedPatrolRatios[i], viability.emptyPatrolRatios[i]);
		cycleGold += points[i].coinGold;
		spawnGold += points[i].coinGold * share * 60 / points[i].intervalSeconds;
		cycleExperience += points[i].experience;
		result.spawnExperiencePerMinute += points[i].experience * share * 60 / points[i].intervalSeconds;
	}
	if (cycleSeconds > 0) {
		result.coinGoldPerMinute = std::min(spawnGold, cycleGold * 60 / cycleSeconds);
		result.clearExperiencePerMinute = cycleExperience * 60 / cycleSeconds;
	}
	return result;
}

inline const char* playerBotHuntViabilityRejection(const PlayerBotHuntViability& viability)
{
	if (viability.reachableSpawns <= 3) return "tiny_spawn_pocket";
	return viability.modelLimited ? "replenishment_model_limit" : "insufficient_replenishment";
}

inline PlayerBotHuntViability playerBotHuntViability(
    const std::vector<PlayerBotReplenishmentPoint>& points, double stepSeconds,
    int32_t blockerX = 11, int32_t blockerY = 11)
{
	PlayerBotHuntViability result;
	result.reachableSpawns = static_cast<uint32_t>(points.size());
	if (points.size() <= 3) return result;
	// Bound the quadratic geometry screen per candidate. Smaller pocket and
	// neighborhood variants remain available when a whole site exceeds this.
	if (points.size() > 256) { result.modelLimited = true; return result; }
	result.minimumAwayRatio = 1;
	result.minimumUnblockedPatrolRatio = 1;
	result.minimumEmptyPatrolRatio = 1;
	stepSeconds = std::max(0.0, stepSeconds);
	std::vector<PlayerBotReplenishmentPoint> patrol;
	for (const auto& point : points) {
		auto existing = std::find_if(patrol.begin(), patrol.end(), [&](const auto& entry) {
			return entry.approach == point.approach;
		});
		if (existing == patrol.end()) patrol.push_back(point);
		else existing->fightSeconds += point.fightSeconds;
	}
	double emptyCycleSeconds = 0, cycleSeconds = 0;
	for (size_t i = 0; i < patrol.size(); ++i) {
		const auto& from = patrol[i].approach;
		const auto& to = patrol[(i + 1) % patrol.size()].approach;
		// Match the atlas clear-cycle denominator, including its portal proxy.
		emptyCycleSeconds += (Position::getDistanceX(from, to) + Position::getDistanceY(from, to) +
		    Position::getDistanceZ(from, to) * 20) * stepSeconds;
		cycleSeconds += std::max(0.0, patrol[i].fightSeconds);
	}
	cycleSeconds += emptyCycleSeconds;
	for (const auto& spawn : points) {
		// One tile beyond the engine rectangle tolerates approach/combat movement.
		auto outside = [&](const Position& p) {
			return p.z != spawn.spawn.z ||
			    Position::getDistanceX(p, spawn.spawn) > blockerX + 1 ||
			    Position::getDistanceY(p, spawn.spawn) > blockerY + 1;
		};
		double run = 0, longest = 0, outsideSeconds = 0, emptyOutsideSeconds = 0;
		// Two laps capture the continuous maximum across the patrol origin;
		// totals count ONLY the first lap. Absence at an attempt is sufficient,
		// so this maximum is diagnostic, not a full-interval eligibility gate.
		for (size_t i = 0; i < patrol.size() * 2; ++i) {
			const auto& p = patrol[i % patrol.size()];
			const auto& next = patrol[(i + 1) % patrol.size()];
			if (!outside(p.approach)) { run = 0; continue; }
			run += std::max(0.0, p.fightSeconds);
			if (i < patrol.size()) outsideSeconds += std::max(0.0, p.fightSeconds);
			longest = std::max(longest, run);
			// Credit only same-floor segments whose entire bounding rectangle is
			// outside the blocker. No speculative portal travel-time credit.
			const bool sameSide = p.approach.z == next.approach.z && (
			    p.approach.z != spawn.spawn.z ||
			    std::min(p.approach.x, next.approach.x) > spawn.spawn.x + blockerX + 1 ||
			    std::max(p.approach.x, next.approach.x) < spawn.spawn.x - blockerX - 1 ||
			    std::min(p.approach.y, next.approach.y) > spawn.spawn.y + blockerY + 1 ||
			    std::max(p.approach.y, next.approach.y) < spawn.spawn.y - blockerY - 1);
			if (sameSide) {
				const double travelSeconds = std::max(Position::getDistanceX(p.approach, next.approach),
				    Position::getDistanceY(p.approach, next.approach)) * stepSeconds;
				run += travelSeconds;
				if (i < patrol.size()) {
					outsideSeconds += travelSeconds;
					emptyOutsideSeconds += travelSeconds;
				}
				longest = std::max(longest, run);
			} else run = 0;
		}
		const double ratio = spawn.ignoresBlocking ? 1 :
		    (spawn.intervalSeconds > 0 ? std::min(1.0, std::min(longest, cycleSeconds) / spawn.intervalSeconds) : 0);
		const double share = spawn.ignoresBlocking ? 1 :
		    (cycleSeconds > 0 ? std::clamp(outsideSeconds / cycleSeconds, 0.0, 1.0) : 0);
		const double emptyShare = spawn.ignoresBlocking ? 1 :
		    (emptyCycleSeconds > 0 ? std::clamp(emptyOutsideSeconds / emptyCycleSeconds, 0.0, 1.0) : 0);
		// Bounded duty-share heuristic, not a guarantee of scheduler alignment.
		// Require meaningful modeled exposure and non-token opportunity even
		// when every fight disappears. Safety still models every member.
		const bool qualifies = share >= 0.25 && emptyShare >= 0.05;
		result.minimumAwayRatio = std::min(result.minimumAwayRatio, ratio);
		result.minimumUnblockedPatrolRatio = std::min(result.minimumUnblockedPatrolRatio, share);
		result.minimumEmptyPatrolRatio = std::min(result.minimumEmptyPatrolRatio, emptyShare);
		result.unblockedPatrolRatios.push_back(share);
		result.emptyPatrolRatios.push_back(emptyShare);
		result.replenishingMembers.push_back(qualifies);
		if (qualifies) ++result.replenishingSpawns;
	}
	// A majority-sized productive core, never just one replenishing monster.
	result.eligible = result.replenishingSpawns >= 3 && result.replenishingSpawns * 2 >= points.size();
	return result;
}
#endif
