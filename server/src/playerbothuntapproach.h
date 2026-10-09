#ifndef FS_PLAYERBOTHUNTAPPROACH_H
#define FS_PLAYERBOTHUNTAPPROACH_H

#include "playerbotnavigation.h"

// At most eight adjacent approaches plus the spawn fallback. Score must return
// no value for physically/coarsely unusable tiles. Unsafe tiles never compete
// with safe alternatives, even when geometrically closer.
template<class Score, class Sample>
std::optional<Position> playerBotHuntApproach(const Position& spawn, double maximumPeak, Score score, Sample sample)
{
	std::optional<Position> best;
	uint32_t bestScore = UINT32_MAX;
	for (int x = -1; x <= 1; ++x) for (int y = -1; y <= 1; ++y) {
		if ((!x && !y) || int(spawn.x) + x < 0 || int(spawn.x) + x > UINT16_MAX ||
		    int(spawn.y) + y < 0 || int(spawn.y) + y > UINT16_MAX) continue;
		const Position candidate(spawn.x + x, spawn.y + y, spawn.z);
		const auto value = score(candidate);
		if (!value || !playerBotNavigationPeakAccepts(maximumPeak, sample(candidate))) continue;
		if (!best || *value < bestScore) { best = candidate; bestScore = *value; }
	}
	if (best) return best;
	if (score(spawn) && playerBotNavigationPeakAccepts(maximumPeak, sample(spawn))) return spawn;
	return std::nullopt;
}

#endif
